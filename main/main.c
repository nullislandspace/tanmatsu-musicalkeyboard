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
#include "keyboard_waveform.h"
#include "keyboard_notes.h"
#include "logo_image.h"

//#define CAVAC_DEBUG

// Constants
//static char const TAG[] = "main";

// External BSP audio function (not in public header)
extern void bsp_audio_initialize(uint32_t rate);

// Audio constants
#define MAX_ACTIVE_NOTES  13    // 8 white keys + 5 black keys
#define FRAMES_PER_WRITE  64
#define SAMPLE_RATE       44100

// ADSR envelope states
typedef enum {
    ADSR_IDLE = 0,      // Note not playing
    ADSR_ATTACK,        // Ramping up from 0% to 100%
    ADSR_DECAY,         // Ramping down from 100% to sustain level
    ADSR_SUSTAIN,       // Holding at sustain level while key pressed
    ADSR_RELEASE        // Ramping down to 0% after key release
} adsr_state_t;

// Audio data structure for active notes
typedef struct {
    int note_index;             // Which note (0-12), or -1 if inactive
    float playback_position;    // Fractional sample position in waveform
    float playback_speed;       // Speed multiplier (frequency / base_freq)
    adsr_state_t adsr_state;    // Current ADSR envelope state
    uint32_t adsr_timer;        // Sample counter for ADSR timing
    float adsr_level;           // Current envelope level (0.0 to 1.0)
    bool key_held;              // Is the key currently pressed?
} active_note_t;

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};
static pax_buf_t                    logo_buf             = {0};  // Logo image buffer
static QueueHandle_t                input_event_queue    = NULL;

// Audio global variables
static i2s_chan_handle_t i2s_handle = NULL;
static active_note_t active_notes[MAX_ACTIVE_NOTES];
static bool note_keys_pressed[NUM_NOTES] = {false};  // Track which keys are currently pressed
static float current_normalization = 1.0f;  // Smoothed normalization factor
static uint8_t audio_volume = 100;  // Current volume (0-100%)

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

// Helper function: Get interpolated sample from waveform
static inline float get_waveform_sample(float position) {
    // Get integer and fractional parts
    int pos_int = (int)position;
    float pos_frac = position - pos_int;

    // Wrap around the waveform cycle
    pos_int = pos_int % WAVEFORM_CYCLE_LENGTH;
    int next_pos = (pos_int + 1) % WAVEFORM_CYCLE_LENGTH;

    // Linear interpolation
    float sample1 = waveform_data[pos_int] / 32768.0f;
    float sample2 = waveform_data[next_pos] / 32768.0f;
    return sample1 + (sample2 - sample1) * pos_frac;
}

// Helper function: Update ADSR envelope for a note
static inline void update_adsr(active_note_t* note) {
    switch (note->adsr_state) {
        case ADSR_IDLE:
            note->adsr_level = 0.0f;
            break;

        case ADSR_ATTACK:
            note->adsr_timer++;
            note->adsr_level = (float)note->adsr_timer / ADSR_ATTACK_SAMPLES;
            if (note->adsr_timer >= ADSR_ATTACK_SAMPLES) {
                note->adsr_state = ADSR_DECAY;
                note->adsr_timer = 0;
                note->adsr_level = 1.0f;
            }
            break;

        case ADSR_DECAY:
            note->adsr_timer++;
            float decay_progress = (float)note->adsr_timer / ADSR_DECAY_SAMPLES;
            note->adsr_level = 1.0f - (1.0f - ADSR_SUSTAIN_LEVEL) * decay_progress;
            if (note->adsr_timer >= ADSR_DECAY_SAMPLES) {
                note->adsr_state = ADSR_SUSTAIN;
                note->adsr_level = ADSR_SUSTAIN_LEVEL;
            }
            break;

        case ADSR_SUSTAIN:
            note->adsr_level = ADSR_SUSTAIN_LEVEL;
            // Check if key was released
            if (!note->key_held) {
                note->adsr_state = ADSR_RELEASE;
                note->adsr_timer = 0;
            }
            break;

        case ADSR_RELEASE:
            note->adsr_timer++;
            float release_progress = (float)note->adsr_timer / ADSR_RELEASE_SAMPLES;
            note->adsr_level = ADSR_SUSTAIN_LEVEL * (1.0f - release_progress);
            if (note->adsr_timer >= ADSR_RELEASE_SAMPLES) {
                note->adsr_state = ADSR_IDLE;
                note->adsr_level = 0.0f;
                note->note_index = -1;  // Mark slot as free
            }
            break;
    }
}

// Audio mixing task
void audio_task(void* arg) {
    int16_t output_buffer[FRAMES_PER_WRITE * 2];  // Stereo: 2 channels per frame
    size_t bytes_written;

    while (1) {
        // Mix all active notes into output buffer
        for (int frame = 0; frame < FRAMES_PER_WRITE; frame++) {
            float mix_left = 0.0f;
            float mix_right = 0.0f;
            int active_count = 0;

            // Process each active note
            for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
                if (active_notes[i].adsr_state != ADSR_IDLE) {
                    active_count++;

                    // Get interpolated sample from waveform
                    float sample = get_waveform_sample(active_notes[i].playback_position);

                    // Update ADSR envelope
                    update_adsr(&active_notes[i]);

                    // Apply envelope
                    sample *= active_notes[i].adsr_level;

                    // Accumulate (mono to stereo)
                    mix_left += sample;
                    mix_right += sample;

                    // Advance playback position at the correct speed
                    // Speed is independent per note - this ensures correct pitch
                    active_notes[i].playback_position += active_notes[i].playback_speed;

                    // Keep position within reasonable bounds to prevent overflow
                    if (active_notes[i].playback_position >= WAVEFORM_CYCLE_LENGTH * 1000) {
                        active_notes[i].playback_position -= WAVEFORM_CYCLE_LENGTH * 1000;
                    }
                }
            }

            // Normalize by number of active notes to prevent clipping
            // This ensures total output stays within -1.0 to 1.0 range
            if (active_count > 0) {
                // Calculate target normalization (sqrt for better perceived loudness)
                float target_normalization = 1.0f / sqrtf((float)active_count);

                // Smooth the normalization change to prevent clicks when notes start/stop
                // Use exponential smoothing: smaller alpha = smoother but slower response
                // Alpha of 0.01 means normalization reaches 99% of target in ~460 samples (~10ms)
                float alpha = 0.01f;
                current_normalization += alpha * (target_normalization - current_normalization);

                mix_left *= current_normalization;
                mix_right *= current_normalization;
            } else {
                // No active notes, reset normalization to 1.0
                current_normalization = 1.0f;
            }

            // Soft clip to prevent distortion (should rarely trigger now)
            mix_left = fminf(1.0f, fmaxf(-1.0f, mix_left));
            mix_right = fminf(1.0f, fmaxf(-1.0f, mix_right));

            // Convert back to 16-bit stereo
            output_buffer[frame * 2] = (int16_t)(mix_left * 32767.0f);
            output_buffer[frame * 2 + 1] = (int16_t)(mix_right * 32767.0f);
        }

        // Write to I2S (blocks until DMA buffer is ready, ~1.45ms)
        // I2S output rate is constant at SAMPLE_RATE (44100 Hz)
        // regardless of how many notes are playing
        if (i2s_handle != NULL) {
            i2s_channel_write(i2s_handle, output_buffer,
                            sizeof(output_buffer), &bytes_written, portMAX_DELAY);
        }
    }
}

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

// Helper function: Start playing a note
void start_note(int note_index) {
    if (note_index < 0 || note_index >= NUM_NOTES) return;

    // Find a free slot or reuse a slot with the same note
    int slot = -1;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (active_notes[i].note_index == note_index) {
            slot = i;  // Reuse existing slot for this note
            break;
        }
        if (slot == -1 && active_notes[i].adsr_state == ADSR_IDLE) {
            slot = i;  // Found a free slot
        }
    }

    if (slot >= 0) {
        // Start the note
        active_notes[slot].note_index = note_index;
        active_notes[slot].playback_position = 0.0f;
        active_notes[slot].playback_speed = note_defs[note_index].frequency / WAVEFORM_BASE_FREQ;
        active_notes[slot].adsr_state = ADSR_ATTACK;
        active_notes[slot].adsr_timer = 0;
        active_notes[slot].adsr_level = 0.0f;
        active_notes[slot].key_held = true;
    }
}

// Helper function: Stop playing a note
void stop_note(int note_index) {
    if (note_index < 0 || note_index >= NUM_NOTES) return;

    // Find the active note and trigger release
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (active_notes[i].note_index == note_index) {
            active_notes[i].key_held = false;  // Trigger release phase
        }
    }
}

// Render on-screen keyboard
void render_keyboard(pax_buf_t* fb, int width, int height) {
    // Color constants (defined inline since they're used before main)
    const pax_col_t COLOR_BLACK = 0xFF000000;
    const pax_col_t COLOR_WHITE = 0xFFFFFFFF;
    const pax_col_t COLOR_BLUE = 0xFF4444FF;
    const pax_col_t COLOR_RED = 0xFFFF0000;
    const pax_col_t COLOR_DARK_GREEN = 0xFF006400;  // Dark green for white keys
    const pax_col_t COLOR_BRIGHT_GREEN = 0xFF00FF00;  // Bright green for black keys

    // Keyboard dimensions (optimized for 480x800 display in landscape/portrait)
    const int white_key_width = 60;
    const int white_key_height = 200;
    const int black_key_width = 40;
    const int black_key_height = 130;
    const int keyboard_start_y = height - white_key_height - 20;

    // Keyboard key names for white keys (A, S, D, F, G, H, J, K)
    const char* white_key_names[8] = {"A", "S", "D", "F", "G", "H", "J", "K"};

    // Draw white keys (C, D, E, F, G, A, B, C)
    for (int i = 0; i < 8; i++) {
        int x = 10 + i * white_key_width;
        int y = keyboard_start_y;

        // Check if key is pressed
        bool is_pressed = note_keys_pressed[i];
        pax_col_t key_color = is_pressed ? COLOR_BLUE : COLOR_WHITE;  // Blue when pressed

        // Draw key
        pax_draw_rect(fb, key_color, x, y, white_key_width - 2, white_key_height);
        pax_outline_rect(fb, COLOR_BLACK, x, y, white_key_width - 2, white_key_height);

        // Draw keyboard key name in dark green (above note name)
        pax_draw_text(fb, COLOR_DARK_GREEN, pax_font_sky_mono, 14, x + 21, y + white_key_height - 50, white_key_names[i]);

        // Draw note name
        pax_draw_text(fb, COLOR_BLACK, pax_font_sky_mono, 12, x + 18, y + white_key_height - 30, note_defs[i].name);
    }

    // Keyboard key names for black keys (Q, W, R, T, Y)
    const char* black_key_names[5] = {"Q", "W", "R", "T", "Y"};

    // Draw black keys (C#, D#, F#, G#, A#)
    int black_key_map[5] = {8, 9, 10, 11, 12};  // Note indices for black keys
    int black_key_x_offsets[5] = {
        40,   // C# (between C and D)
        100,  // D# (between D and E)
        220,  // F# (between F and G)
        280,  // G# (between G and A)
        340   // A# (between A and B)
    };

    for (int i = 0; i < 5; i++) {
        int x = 10 + black_key_x_offsets[i];
        int y = keyboard_start_y;
        int note_idx = black_key_map[i];

        // Check if key is pressed
        bool is_pressed = note_keys_pressed[note_idx];
        pax_col_t key_color = is_pressed ? COLOR_RED : COLOR_BLACK;  // Red when pressed

        // Draw key
        pax_draw_rect(fb, key_color, x, y, black_key_width, black_key_height);
        pax_outline_rect(fb, COLOR_WHITE, x, y, black_key_width, black_key_height);

        // Draw keyboard key name in bright green (above note name)
        pax_draw_text(fb, COLOR_BRIGHT_GREEN, pax_font_sky_mono, 12, x + 12, y + black_key_height - 40, black_key_names[i]);

        // Draw note name
        pax_draw_text(fb, COLOR_WHITE, pax_font_sky_mono, 10, x + 5, y + black_key_height - 20, note_defs[note_idx].name);
    }
}

// Render volume indicator
void render_volume_indicator(pax_buf_t* fb, int width, int height) {
    const pax_col_t COLOR_WHITE = 0xFFFFFFFF;
    const pax_col_t COLOR_DARK_GREEN = 0xFF006400;

    // Volume bar dimensions
    const int bar_width = 20;
    const int bar_height = 100;
    const int margin = 10;

    // Position: bottom right corner, just a few pixels from bottom
    int x = width - bar_width - margin;
    int y = height - bar_height - margin;

    // Draw hollow white rectangle (full volume range)
    pax_outline_rect(fb, COLOR_WHITE, x, y, bar_width, bar_height);

    // Calculate filled height based on volume (0-100%)
    // Account for borders (2 pixels: top and bottom)
    int max_fill_height = bar_height - 2;
    int filled_height = (max_fill_height * audio_volume) / 100;

    // Draw filled dark green bar from bottom up, staying inside the white border
    if (filled_height > 0) {
        int filled_y = y + bar_height - 1 - filled_height;  // -1 for bottom border
        pax_draw_rect(fb, COLOR_DARK_GREEN, x + 1, filled_y, bar_width - 2, filled_height);
    }
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
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = display_color_format,
                .num_fbs                = 1,
            },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_configuration));

    // Initialize audio subsystem
    bsp_audio_initialize(SAMPLE_RATE);
    bsp_audio_get_i2s_handle(&i2s_handle);
    bsp_audio_set_amplifier(true);   // Enable amplifier
    bsp_audio_set_volume(audio_volume);  // Set initial volume (100%)

    // Initialize active notes array
    memset(active_notes, 0, sizeof(active_notes));
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        active_notes[i].note_index = -1;  // Mark all slots as free
        active_notes[i].adsr_state = ADSR_IDLE;
    }

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

    uint8_t led_data[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    bsp_led_write(led_data, sizeof(led_data));

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    ESP_ERROR_CHECK(res);  // Check that the display parameters have been initialized
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();

    char debugrotation[20] = "";
    char debugcolor[20] = "";
    char debugwidth[20] = "";
    char debugheight[20] = "";

    sprintf(debugwidth, "WIDTH: %d", display_h_res);
    sprintf(debugheight, "HEIGHT: %d", display_v_res);

    // Convert ESP-IDF color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    sprintf(debugcolor, "Mode RGB888");
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
            format = PAX_BUF_16_565RGB;
            sprintf(debugcolor, "Mode RGB565");
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
            format = PAX_BUF_24_888RGB;
            sprintf(debugcolor, "Mode RGB888");
            break;
        default:
            break;
    }

    // Convert BSP display rotation format into PAX orientation type
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            sprintf(debugrotation, "Rot: 90");
            break;
        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            sprintf(debugrotation, "Rot: 180");
            break;
        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            sprintf(debugrotation, "Rot: 270");
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            sprintf(debugrotation, "Rot: 0");
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

    // Initialize logo buffer (already pre-rotated in the image data)
    pax_buf_init(&logo_buf, (void*)logo_image_data, LOGO_WIDTH, LOGO_HEIGHT, PAX_BUF_24_888RGB);

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

    // Main section of the app - Musical Keyboard

    // Get framebuffer dimensions
    int fb_w = pax_buf_get_width(&fb);
    int fb_h = pax_buf_get_height(&fb);

    // Track whether screen needs updating
    bool screen_needs_update = true;
    uint32_t last_update_time = 0;
    const uint32_t min_update_interval_ms = 33;  // Max 30 FPS to reduce DMA contention

    uint32_t delay = pdMS_TO_TICKS(10);  // 10ms timeout for input
    while(1) {
        bsp_input_event_t event;
        bool input_received = false;

        // Process input events
        while (xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
            input_received = true;
            if (event.type == INPUT_EVENT_TYPE_SCANCODE) {
                uint32_t scancode = event.args_scancode.scancode;

                // Remove release modifier to get actual key
                uint32_t key = scancode & ~BSP_INPUT_SCANCODE_RELEASE_MODIFIER;

                // Check if ESC key (0x01) is pressed
                if (key == 0x01 && is_key_press(scancode)) {
                    bsp_device_restart_to_launcher();
                }

                // Check for volume keys (only on key press)
                if (is_key_press(scancode)) {
                    bool volume_changed = false;

                    // Volume down (dedicated volume down key)
                    if (scancode == 0xE02E) {
                        if (audio_volume >= 10) {
                            audio_volume -= 10;
                            volume_changed = true;
                        }
                    }
                    // Volume up (dedicated volume up key)
                    else if (scancode == 0xE030) {
                        if (audio_volume <= 90) {
                            audio_volume += 10;
                            volume_changed = true;
                        }
                    }

                    if (volume_changed) {
                        bsp_audio_set_volume(audio_volume);
                        screen_needs_update = true;  // Update screen to show new volume
                    }
                }

                int note_idx = find_note_by_scancode(scancode);

                if (note_idx >= 0) {
                    if (is_key_press(scancode)) {
                        // Key pressed - start note
                        note_keys_pressed[note_idx] = true;
                        start_note(note_idx);
                        screen_needs_update = true;  // Update screen to show pressed key
                    } else if (is_key_release(scancode)) {
                        // Key released - stop note
                        note_keys_pressed[note_idx] = false;
                        stop_note(note_idx);
                        screen_needs_update = true;  // Update screen to show released key
                    }
                }
                // Ignore all other unmapped keys
            }
        }

        // Only update screen if needed and enough time has passed
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (screen_needs_update && (current_time - last_update_time >= min_update_interval_ms)) {
            // Render display
            pax_background(&fb, BLACK);

            // Draw centered logo
            pax_draw_image_op(&fb, &logo_buf, (fb_w - LOGO_WIDTH) / 2, 20);

            // Draw title
            //pax_draw_text(&fb, WHITE, pax_font_sky_mono, 18, 120, 160, "Musical Keyboard");

            // Instructions
            pax_draw_text(&fb, WHITE, pax_font_sky_mono, 12, 60, 190, "Play notes using your keyboard");
            pax_draw_text(&fb, WHITE, pax_font_sky_mono, 12, 80, 210, "Press ESC to exit");

#ifdef CAVAC_DEBUG
            pax_draw_text(&fb, WHITE, pax_font_sky_mono, 12, 20, 240, debugrotation);
            pax_draw_text(&fb, WHITE, pax_font_sky_mono, 12, 20, 260, debugcolor);
            pax_draw_text(&fb, WHITE, pax_font_sky_mono, 12, 20, 280, debugwidth);
            pax_draw_text(&fb, WHITE, pax_font_sky_mono, 12, 20, 300, debugheight);
#endif // CAVAC_DEBUG

            // Render keyboard
            render_keyboard(&fb, fb_w, fb_h);

            // Render volume indicator
            render_volume_indicator(&fb, fb_w, fb_h);

            // Update display (DMA transfer to screen)
            blit();

            // Reset update flag and record time
            screen_needs_update = false;
            last_update_time = current_time;
        }

        // Small delay to prevent busy-waiting and allow audio task to run
        vTaskDelay(delay);
    }
}
