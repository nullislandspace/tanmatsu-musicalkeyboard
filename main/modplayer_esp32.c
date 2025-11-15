// MOD Player for ESP32
// Adapted from modplayer.c for I2S audio output via ring buffer
// Plays 4-channel Amiga MOD files
//
// HEAVILY BASED ON CODE BY Tony Tascioglu
// https://wiki.tonytascioglu.com/articles/playing_mod_tracker_music


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "modplayer_esp32.h"
#include "popcorn_remix_mod.h"

static const char *TAG = "MOD";

// MOD file constants
#define MAX_SAMPLES        31
#define NUM_CHANNELS       4
#define ROWS_PER_PATTERN   64
#define BYTES_PER_NOTE     4
#define SAMPLE_RATE        22050  // MOD playback rate (half of I2S output rate)
#define PROCESS_BUFFER_SIZE 1024
#define BASE_TEMPO         125  // Default MOD tempo in BPM
#define MOD_VOLUME_SCALE   0.7f // Background music at 70% volume

// Ring buffer globals (defined in main.c)
int16_t mod_ring_buffer[MOD_BUFFER_SIZE];
volatile uint32_t mod_write_pos = 0;
volatile uint32_t mod_read_pos = 0;
SemaphoreHandle_t mod_buffer_mutex = NULL;
volatile bool mod_player_running = false;

// Note period table for Amiga frequency conversion
const uint16_t period_table[] = {
    // C    C#   D    D#   E    F    F#   G    G#   A    A#   B
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, // Octave 1
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226, // Octave 2
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113  // Octave 3
};

// Pattern data structure
typedef struct {
    uint8_t sample;    // Sample number (0-31)
    uint16_t period;   // Note period
    uint8_t effect;    // Effect number
    uint8_t param;     // Effect parameter
} Note;

// Sample data structure
typedef struct {
    char name[22];
    uint16_t length;           // Length in bytes
    uint8_t finetune;          // Finetune value (0-15)
    uint8_t volume;            // Default volume (0-64)
    uint16_t repeat_point;     // Repeat point in bytes
    uint16_t repeat_length;    // Repeat length in bytes
    const int8_t *data;        // Sample data (8-bit signed, in flash)
} Sample;

// MOD file structure
typedef struct {
    char title[21];
    Sample samples[MAX_SAMPLES];
    uint8_t song_length;       // Number of positions
    uint8_t positions[128];    // Pattern sequence
    uint8_t num_patterns;      // Number of patterns (calculated)
    Note *patterns;            // Pattern data (dynamically allocated)
} MODFile;

// Channel state
typedef struct {
    uint16_t period;           // Current note period
    uint8_t sample_num;        // Current sample number
    uint8_t volume;            // Current volume (0-64)
    uint32_t sample_pos;       // Position in sample (fixed point: 16.16)
    uint32_t sample_increment; // Sample position increment per tick
    uint8_t effect;            // Current effect
    uint8_t param;             // Effect parameter
} ChannelState;

// Read a 2-byte big-endian value
static uint16_t read_big_endian_16(const uint8_t *data) {
    return (data[0] << 8) | data[1];
}

// Convert Amiga period to frequency
static float period_to_freq(uint16_t period) {
    if (period == 0) return 0;
    return 7159090.5f / (period * 2);
}

// Load MOD from embedded data
static int load_mod_embedded(const uint8_t *data, size_t data_len, MODFile *mod) {
    const uint8_t *ptr = data;

    // Read title
    memcpy(mod->title, ptr, 20);
    mod->title[20] = '\0';
    ptr += 20;

    // Read sample information
    for (int i = 0; i < MAX_SAMPLES; i++) {
        memcpy(mod->samples[i].name, ptr, 22);
        mod->samples[i].name[21] = '\0';
        ptr += 22;

        mod->samples[i].length = read_big_endian_16(ptr) * 2; // Convert to bytes
        ptr += 2;

        mod->samples[i].finetune = *ptr++;
        mod->samples[i].volume = *ptr++;

        mod->samples[i].repeat_point = read_big_endian_16(ptr) * 2; // Convert to bytes
        ptr += 2;

        mod->samples[i].repeat_length = read_big_endian_16(ptr) * 2; // Convert to bytes
        ptr += 2;
    }

    // Read song information
    mod->song_length = *ptr++;
    ptr++; // Skip unused byte

    memcpy(mod->positions, ptr, 128);
    ptr += 128;

    // Skip 4 bytes (format identifier M.K. or similar)
    ptr += 4;

    // Determine number of patterns
    mod->num_patterns = 0;
    for (int i = 0; i < mod->song_length; i++) {
        if (mod->positions[i] > mod->num_patterns) {
            mod->num_patterns = mod->positions[i];
        }
    }
    mod->num_patterns++;

    // Allocate pattern data
    size_t pattern_data_size = mod->num_patterns * ROWS_PER_PATTERN * NUM_CHANNELS * sizeof(Note);
    mod->patterns = (Note*)malloc(pattern_data_size);
    if (!mod->patterns) {
        ESP_LOGE(TAG, "Failed to allocate pattern memory (%d bytes)", pattern_data_size);
        return 0;
    }

    // Read pattern data
    Note *note_ptr = mod->patterns;
    for (int p = 0; p < mod->num_patterns; p++) {
        for (int r = 0; r < ROWS_PER_PATTERN; r++) {
            for (int c = 0; c < NUM_CHANNELS; c++) {
                uint8_t b0 = *ptr++;
                uint8_t b1 = *ptr++;
                uint8_t b2 = *ptr++;
                uint8_t b3 = *ptr++;

                // Decode note data
                uint16_t period = ((b0 & 0x0F) << 8) | b1;
                uint8_t sample = (b0 & 0xF0) | (b2 >> 4);

                note_ptr->period = period;
                note_ptr->sample = sample;
                note_ptr->effect = b2 & 0x0F;
                note_ptr->param = b3;
                note_ptr++;
            }
        }
    }

    // Set sample data pointers (pointing into flash)
    for (int i = 0; i < MAX_SAMPLES; i++) {
        if (mod->samples[i].length > 0) {
            mod->samples[i].data = (const int8_t*)ptr;
            ptr += mod->samples[i].length;
        } else {
            mod->samples[i].data = NULL;
        }
    }

    ESP_LOGI(TAG, "Loaded MOD: %s", mod->title);
    ESP_LOGI(TAG, "Song length: %d positions, %d patterns", mod->song_length, mod->num_patterns);

    return 1;
}

// Release MOD file resources
static void free_mod_file(MODFile *mod) {
    if (mod->patterns) {
        free(mod->patterns);
        mod->patterns = NULL;
    }
}

// Write to ring buffer (thread-safe)
// Upsamples from 22050 Hz to 44100 Hz by duplicating each sample
static void write_to_ring_buffer(int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        int16_t scaled_sample = (int16_t)(samples[i] * MOD_VOLUME_SCALE);

        // Write sample twice to upsample from 22050 to 44100 Hz
        for (int j = 0; j < 2; j++) {
            // Calculate next write position
            uint32_t next_pos = (mod_write_pos + 1) % MOD_BUFFER_SIZE;

            // Check for buffer overflow (don't overwrite unread data)
            if (next_pos == mod_read_pos) {
                // Buffer full, skip this sample
                continue;
            }

            // Write upsampled data
            mod_ring_buffer[mod_write_pos] = scaled_sample;
            mod_write_pos = next_pos;
        }
    }
}

// Process one tick of audio
static void process_tick(MODFile *mod, ChannelState *channels, int16_t *buffer, int buffer_size) {
    // Clear buffer
    memset(buffer, 0, buffer_size * sizeof(int16_t));

    // Mix channels
    for (int i = 0; i < buffer_size; i++) {
        int32_t mixed = 0;

        for (int c = 0; c < NUM_CHANNELS; c++) {
            if (channels[c].period > 0 && channels[c].sample_num > 0) {
                int sample_idx = channels[c].sample_num - 1;

                if (sample_idx >= 0 && sample_idx < MAX_SAMPLES &&
                    mod->samples[sample_idx].data &&
                    mod->samples[sample_idx].length > 0) {

                    // Get current sample position
                    uint32_t pos = channels[c].sample_pos >> 16;

                    if (pos < mod->samples[sample_idx].length) {
                        // Apply volume and mix
                        int sample_val = mod->samples[sample_idx].data[pos];
                        mixed += ((sample_val * channels[c].volume) / 64) * 32; // 32x gain for louder output

                        // Update position
                        channels[c].sample_pos += channels[c].sample_increment;

                        // Handle looping
                        if (mod->samples[sample_idx].repeat_length > 2) {
                            uint32_t loop_end = mod->samples[sample_idx].repeat_point +
                                                mod->samples[sample_idx].repeat_length;

                            if ((channels[c].sample_pos >> 16) >= loop_end) {
                                channels[c].sample_pos = mod->samples[sample_idx].repeat_point << 16;
                            }
                        } else if ((channels[c].sample_pos >> 16) >= mod->samples[sample_idx].length) {
                            // Stop playback if no loop
                            channels[c].sample_pos = 0;
                            channels[c].period = 0;
                        }
                    }
                }
            }
        }

        // Clamp and store in buffer
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;

        buffer[i] = (int16_t)mixed;
    }
}

// Process a row of the pattern
static void process_row(MODFile *mod, ChannelState *channels, int position, int row) {
    int pattern = mod->positions[position];
    Note *pattern_data = &mod->patterns[pattern * ROWS_PER_PATTERN * NUM_CHANNELS];

    // Process each channel
    for (int c = 0; c < NUM_CHANNELS; c++) {
        Note *note = &pattern_data[row * NUM_CHANNELS + c];

        // Process sample change
        if (note->sample > 0) {
            channels[c].sample_num = note->sample;
            channels[c].volume = mod->samples[note->sample-1].volume;
        }

        // Process note
        if (note->period != 0) {
            if (note->effect != 0x3) { // Skip if portamento effect
                channels[c].period = note->period;
                channels[c].sample_pos = 0;

                // Calculate sample increment based on period
                float freq = period_to_freq(note->period);
                channels[c].sample_increment = (uint32_t)((freq * 65536.0f) / SAMPLE_RATE);
            }
        }

        // Save effect and param
        channels[c].effect = note->effect;
        channels[c].param = note->param;

        // Process effects
        switch (note->effect) {
            case 0xC: // Set volume
                if (note->param <= 64) {
                    channels[c].volume = note->param;
                }
                break;
        }
    }
}

// Calculate samples per tick based on tempo
static int calculate_tick_samples(int tempo) {
    // 2500 / tempo = milliseconds per tick
    // (ms * sample_rate) / 1000 = samples per tick
    return (2500 * SAMPLE_RATE) / (tempo * 1000);
}

// MOD player task
void modplayer_task(void* arg) {
    MODFile mod = {0};
    ChannelState channels[NUM_CHANNELS] = {0};
    int16_t buffer[PROCESS_BUFFER_SIZE];

    // Load embedded MOD data
    if (!load_mod_embedded(mod_data, mod_data_len, &mod)) {
        ESP_LOGE(TAG, "Failed to load embedded MOD file");
        vTaskDelete(NULL);
        return;
    }

    mod_player_running = true;

    // Player state
    int position = 0;
    int row = 0;
    int ticks_per_row = 5; // Default speed
    int current_tick = 0;
    int tempo = 125; // Default tempo (BPM)
    int samples_per_tick = calculate_tick_samples(tempo);

    ESP_LOGI(TAG, "Starting MOD playback");

    // Main playback loop (loops continuously)
    while (mod_player_running) {
        if (current_tick == 0) {
            // Process new row
            process_row(&mod, channels, position, row);

            // Check for effects
            for (int c = 0; c < NUM_CHANNELS; c++) {
                if (channels[c].effect == 0xD) {
                    row = ROWS_PER_PATTERN - 1; // Pattern break
                    break;
                } else if (channels[c].effect == 0xF && channels[c].param <= 0x1F) {
                    // Set speed (ticks per row)
                    if (channels[c].param > 0) {
                        ticks_per_row = channels[c].param;
                    }
                } else if (channels[c].effect == 0xF && channels[c].param >= 0x20) {
                    // Set tempo (BPM)
                    tempo = channels[c].param;
                    samples_per_tick = calculate_tick_samples(tempo);
                }
            }
        }

        // Process and output audio for this tick
        // Generate the correct number of samples based on tempo
        int samples_to_generate = (samples_per_tick < PROCESS_BUFFER_SIZE) ? samples_per_tick : PROCESS_BUFFER_SIZE;
        process_tick(&mod, channels, buffer, samples_to_generate);
        write_to_ring_buffer(buffer, samples_to_generate);

        // Update tick counter
        current_tick++;
        if (current_tick >= ticks_per_row) {
            current_tick = 0;
            row++;

            if (row >= ROWS_PER_PATTERN) {
                row = 0;
                position++;

                // Loop back to start when song ends
                if (position >= mod.song_length) {
                    position = 0;
                    ESP_LOGI(TAG, "Looping MOD playback");
                }
            }
        }

        // Delay based on actual samples generated
        // Calculate milliseconds for the samples we just generated
        int delay_ms = (samples_to_generate * 1000) / SAMPLE_RATE;
        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    // Cleanup
    free_mod_file(&mod);
    ESP_LOGI(TAG, "MOD playback stopped");
    mod_player_running = false;
    vTaskDelete(NULL);
}

// Initialize MOD player
void modplayer_init(void) {
    // Initialize ring buffer
    memset(mod_ring_buffer, 0, sizeof(mod_ring_buffer));
    mod_write_pos = 0;
    mod_read_pos = 0;

    // Create mutex
    mod_buffer_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "MOD player initialized");
}

// Stop MOD playback
void modplayer_stop(void) {
    mod_player_running = false;
}
