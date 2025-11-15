#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "bsp/audio.h"
#include "custom_certificates.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "wifi_connection.h"
#include "wifi_remote.h"
#include "bounce_sounds.h"
#include "modplayer_esp32.h"
#include "logo_image.h"

// Constants
//static char const TAG[] = "main";

// External BSP audio function (not in public header)
extern void bsp_audio_initialize(uint32_t rate);

// Audio constants
#define MAX_ACTIVE_SOUNDS 5
#define FRAMES_PER_WRITE  64
#define SAMPLE_RATE       44100

// Audio data structures
typedef struct {
    const int16_t* sample_data;    // Pointer to sound sample in flash
    uint32_t sample_length;        // Total samples in sound
    uint32_t playback_position;    // Current playback position
    bool active;                   // Is this sound currently playing?
    float volume;                  // Volume 0.0 to 1.0
} active_sound_t;

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};
static pax_buf_t                    logo_buf             = {0};  // Logo image buffer
static QueueHandle_t                input_event_queue    = NULL;

// Audio global variables
static i2s_chan_handle_t i2s_handle = NULL;
static active_sound_t active_sounds[MAX_ACTIVE_SOUNDS];
static volatile bool sound_trigger[5] = {false, false, false, false, false};

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

// Audio mixing task
void audio_task(void* arg) {
    int16_t output_buffer[FRAMES_PER_WRITE * 2];  // Stereo: 2 channels per frame
    size_t bytes_written;

    while (1) {
        // 1. Check for new sound triggers from main loop
        for (int i = 0; i < 5; i++) {
            if (sound_trigger[i]) {
                // Find a free slot or reuse the slot for this sound
                for (int slot = 0; slot < MAX_ACTIVE_SOUNDS; slot++) {
                    if (!active_sounds[slot].active ||
                        active_sounds[slot].sample_data == bounce_samples[i]) {
                        active_sounds[slot].sample_data = bounce_samples[i];
                        active_sounds[slot].sample_length = bounce_lengths[i];
                        active_sounds[slot].playback_position = 0;
                        active_sounds[slot].volume = 0.1f;  // 10% volume for ball sounds
                        active_sounds[slot].active = true;
                        break;
                    }
                }
                sound_trigger[i] = false;  // Clear the trigger
            }
        }

        // 2. Mix all active sounds into output buffer
        for (int frame = 0; frame < FRAMES_PER_WRITE; frame++) {
            float mix_left = 0.0f;
            float mix_right = 0.0f;

            // Add MOD background music (if available)
            if (mod_read_pos != mod_write_pos) {
                int16_t mod_sample = mod_ring_buffer[mod_read_pos];
                float mod_sample_f = mod_sample / 32768.0f;
                mix_left += mod_sample_f;
                mix_right += mod_sample_f;
                mod_read_pos = (mod_read_pos + 1) % MOD_BUFFER_SIZE;
            }

            // Accumulate all active sounds
            for (int i = 0; i < MAX_ACTIVE_SOUNDS; i++) {
                if (active_sounds[i].active) {
                    // Get sample value
                    int16_t sample = active_sounds[i].sample_data[
                        active_sounds[i].playback_position
                    ];

                    // Convert to float (-1.0 to 1.0) and apply volume
                    float sample_f = (sample / 32768.0f) * active_sounds[i].volume;

                    // Accumulate (mono to stereo)
                    mix_left += sample_f;
                    mix_right += sample_f;

                    // Advance playback position
                    active_sounds[i].playback_position++;
                    if (active_sounds[i].playback_position >=
                        active_sounds[i].sample_length) {
                        active_sounds[i].active = false;  // Sound finished
                    }
                }
            }

            // Soft clip to prevent distortion
            mix_left = fminf(1.0f, fmaxf(-1.0f, mix_left));
            mix_right = fminf(1.0f, fmaxf(-1.0f, mix_right));

            // Convert back to 16-bit stereo
            output_buffer[frame * 2] = (int16_t)(mix_left * 32767.0f);
            output_buffer[frame * 2 + 1] = (int16_t)(mix_right * 32767.0f);
        }

        // 3. Write to I2S (blocks until DMA buffer is ready, ~1.45ms)
        if (i2s_handle != NULL) {
            i2s_channel_write(i2s_handle, output_buffer,
                            sizeof(output_buffer), &bytes_written, portMAX_DELAY);
        }
    }
}

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage service
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    // Initialize the Board Support Package
    ESP_ERROR_CHECK(bsp_device_initialize());

    // Initialize audio subsystem
    bsp_audio_initialize(SAMPLE_RATE);
    bsp_audio_get_i2s_handle(&i2s_handle);
    bsp_audio_set_amplifier(true);   // Enable amplifier
    bsp_audio_set_volume(100);       // Set master volume to maximum

    // Initialize active sounds array
    memset(active_sounds, 0, sizeof(active_sounds));

    // Initialize MOD player
    modplayer_init();

    // Create audio mixing task on Core 1 with high priority
    xTaskCreatePinnedToCore(
        audio_task,
        "audio",
        4096,                           // Stack size
        NULL,                           // Parameters
        configMAX_PRIORITIES - 2,       // High priority
        NULL,                           // Task handle
        1                               // Pin to Core 1
    );

    // Create MOD player task on Core 1 with lower priority
    xTaskCreatePinnedToCore(
        modplayer_task,
        "modplayer",
        8192,                           // Stack size (larger for MOD processing)
        NULL,                           // Parameters
        configMAX_PRIORITIES - 3,       // Lower priority than audio task
        NULL,                           // Task handle
        1                               // Pin to Core 1
    );

    uint8_t led_data[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    bsp_led_write(led_data, sizeof(led_data));

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    ESP_ERROR_CHECK(res);  // Check that the display parameters have been initialized
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();

    // Convert ESP-IDF color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
            format = PAX_BUF_16_565RGB;
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
            format = PAX_BUF_24_888RGB;
            break;
        default:
            break;
    }

    // Convert BSP display rotation format into PAX orientation type
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            break;
        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            break;
        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            break;
    }

        // Initialize graphics stack
#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    format = PAX_BUF_2_PAL;
#endif
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    fb.palette      = palette;
    fb.palette_size = sizeof(palette) / sizeof(pax_col_t);
#endif
    pax_buf_set_orientation(&fb, orientation);

    // Initialize logo buffer with same orientation as framebuffer
    pax_buf_init(&logo_buf, (void*)logo_image_data, LOGO_WIDTH, LOGO_HEIGHT, PAX_BUF_16_565RGB);
    pax_buf_set_orientation(&logo_buf, orientation);

#if defined(CONFIG_BSP_TARGET_KAMI)
#define BLACK 0
#define WHITE 1
#define RED   2
#else
#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED   0xFFFF0000
#endif

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Main section of the app

    // Bounce some balls around the screen
    //
    // On every bounce, light up one LED with the specific ball color, also blink the keyboard


    // Setup data for the balls, "physics" and the corresponding LEDs
    int32_t xoffs[5] = {100, 130, 170, 230, 335}; // Starting X position of balls
    int32_t yoffs[5] = {100, 107, 209, 305, 227}; // Starting Y position of balls
    int32_t xspeed[5] = {2, 1, -1, 3, -2}; // X speed (delta) of balls
    int32_t yspeed[5] = {1, -1, 2, -2, 3}; // Y speed (delta) of balls
    uint32_t color[5] = {0xFFFF0000, 0xFF00FF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFF00}; // Ball colors
    bool bounce[5] = {false, false, false, false, false}; // Has bounced this render cycle

    uint8_t led_offs[5] = {0 * 3, 1 * 3, 2 * 3, 4 * 3, 5 * 3};  // Starting offset in the led_data for the corresponding balls
    uint8_t led_colormap[15] = {0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00}; // Color of the balls, split into bytes

    // Bounce sound frequencies: 440, 554, 659, 784, 880 Hz (pentatonic scale)
    // Sound samples are pre-generated and loaded from bounce_sounds.h

    uint8_t i;
    uint8_t ledoffs;
    uint32_t delay = pdMS_TO_TICKS(1);  // 1ms timeout for responsive input
    uint8_t bright = 100;
    while(1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, delay) == pdTRUE) {
            bsp_device_restart_to_launcher();
        }
        // Draw black background
        pax_background(&fb, BLACK);
        // Draw centered logo using actual hardware dimensions
        // (logo is unrotated, fb coordinates account for rotation via pax_buf_get_*)
        int fb_w = pax_buf_get_width(&fb);
        int fb_h = pax_buf_get_height(&fb);
        //pax_draw_image_op(&fb, &logo_buf, (fb_w - LOGO_WIDTH) / 2, (fb_h - LOGO_HEIGHT) / 2);
        pax_draw_image_op(&fb, &logo_buf, 200, -100);
        //pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 0, "Press any key to exit the demo.");

        memset(led_data, 0, 18); // LEDS OFF
        led_data[(3 * 3) + 0] = 0xFF; // Power LED on
        led_data[(3 * 3) + 1] = 0xFF;

        for(i = 0; i < 5; i++) {
            bounce[i] = false;
            xoffs[i] += xspeed[i];
            if(xoffs[i] < 1 || xoffs[i] > (display_h_res - 50)) {
                xspeed[i] *= -1;
                xoffs[i] += xspeed[i];
                bright = 100;
                bounce[i] = true;

            }
            yoffs[i] += yspeed[i];
            if(yoffs[i] < 1 || yoffs[i] > (display_v_res - 50)) {
                yspeed[i] *= -1;
                yoffs[i] += yspeed[i];
                bright = 100;
                bounce[i] = true;
            }
            pax_draw_circle(&fb, color[i], yoffs[i] + 25, xoffs[i] + 25, 25);
            if(bounce[i]) {
                pax_draw_circle(&fb, BLACK, yoffs[i] + 25, xoffs[i] + 25, 15);
                pax_draw_circle(&fb, WHITE, yoffs[i] + 25, xoffs[i] + 25, 10);

                ledoffs = led_offs[i];

                // Trigger bounce sound for this ball
                sound_trigger[i] = true;

                // For some strange reason, the LED array seems to expect G R B (instead of R G B), so we swap the bytes accordingly
                led_data[ledoffs + 0] = led_colormap[(i * 3) + 1];
                led_data[ledoffs + 1] = led_colormap[(i * 3) + 0];
                led_data[ledoffs + 2] = led_colormap[(i * 3) + 2];
            }
        }
        bsp_input_set_backlight_brightness(bright);
        if(bright > 0) {
            bright -= 25;
        }
        blit();
        bsp_led_write(led_data, sizeof(led_data));
    }
}
