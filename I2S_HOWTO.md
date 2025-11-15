# I2S Audio Implementation Guide for Tanmatsu Device

## Hardware Overview

### Audio Codec
- **Chip:** ES8156 (I2C-controlled DAC)
- **Interface:** I2S standard mode
- **Amplifier:** Controlled via coprocessor (not directly accessible)

### I2S Pin Configuration
The Tanmatsu BSP handles pin configuration automatically. Pins used:
- **MCLK:** Master Clock - `BSP_I2S_MCLK`
- **BCLK:** Bit Clock - `BSP_I2S_BCLK`
- **WS:** Word Select (LRCLK) - `BSP_I2S_WS`
- **DOUT:** Data Out - `BSP_I2S_DOUT`
- **DIN:** Data In (unused) - `I2S_GPIO_UNUSED`

## BSP Audio API

### Initialization

The BSP provides audio initialization, but the function is **not in the public header**. You need to declare it:

```c
// Declare external BSP function
extern void bsp_audio_initialize(uint32_t rate);

// Initialize audio subsystem
bsp_audio_initialize(44100);  // 44.1 kHz sample rate
```

**What this does:**
1. Initializes I2C bus for codec control
2. Configures ES8156 codec registers
3. Creates and configures I2S channel
4. Starts I2S channel (auto-enabled)

### Getting I2S Handle

```c
#include "badge_bsp_audio.h"

i2s_chan_handle_t i2s_handle;
bsp_audio_get_i2s_handle(&i2s_handle);
```

### Amplifier Control

```c
// Enable amplifier (required for sound output)
bsp_audio_set_amplifier(true);

// Disable amplifier
bsp_audio_set_amplifier(false);
```

### Volume Control

```c
// Set master volume (0-100%)
bsp_audio_set_volume(100);  // Maximum volume
```

**Note:** Volume control affects the codec's DAC output. For mixing multiple sounds, control individual sound amplitudes in software, not master volume.

## I2S Configuration Details

### Standard Configuration (from BSP)

```c
// Channel configuration
i2s_chan_config_t chan_cfg = {
    .id = I2S_NUM_AUTO,
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = 2,        // Double buffering
    .dma_frame_num = 64,      // Frames per DMA buffer
    .auto_clear = true
};

// Standard mode configuration
i2s_std_config_t std_cfg = {
    .clk_cfg = {
        .sample_rate_hz = 44100,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256
    },
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO
    ),
    .gpio_cfg = { /* Pins configured by BSP */ }
};
```

### Audio Format

- **Sample Rate:** 44,100 Hz (standard CD quality)
- **Bit Depth:** 16-bit signed PCM
- **Channels:** Stereo (left + right)
- **Byte Order:** Little-endian
- **Sample Format:** Interleaved (L, R, L, R, ...)

**Frame structure:**
```c
typedef struct {
    int16_t left;   // Left channel sample
    int16_t right;  // Right channel sample
} stereo_frame_t;  // 4 bytes per frame
```

## Writing Audio Data

### Basic Write

```c
int16_t audio_buffer[128];  // 64 stereo frames
size_t bytes_written;

esp_err_t ret = i2s_channel_write(
    i2s_handle,
    audio_buffer,
    sizeof(audio_buffer),
    &bytes_written,
    portMAX_DELAY  // Wait indefinitely
);
```

### Non-blocking Write

```c
// Timeout in ticks (or 0 for immediate return)
i2s_channel_write(i2s_handle, audio_buffer, size, &bytes_written, 0);
```

### DMA Behavior

- I2S uses double-buffering internally
- `i2s_channel_write()` blocks until DMA buffer space is available
- While blocked, FreeRTOS yields to other tasks (efficient)
- With 64-frame buffers @ 44.1kHz: ~1.45ms between writes

## Audio Mixing Patterns

### Pattern 1: Direct Write (Simple, No Mixing)

```c
// Generate or load audio sample
const int16_t beep_sound[1000];  // Mono samples

// Write to I2S (duplicate to stereo)
int16_t stereo_buffer[2000];
for (int i = 0; i < 1000; i++) {
    stereo_buffer[i * 2] = beep_sound[i];      // Left
    stereo_buffer[i * 2 + 1] = beep_sound[i];  // Right
}

size_t bytes_written;
i2s_channel_write(i2s_handle, stereo_buffer,
                  sizeof(stereo_buffer), &bytes_written, portMAX_DELAY);
```

**Limitations:** Cannot overlap sounds, main loop blocks during playback.

### Pattern 2: Task-Based Mixing (Recommended)

**Architecture:**
1. Dedicated audio task runs on Core 1
2. Main loop triggers sounds via lock-free communication
3. Audio task mixes multiple sounds and writes to I2S

**Data structures:**

```c
// Sound playback state
typedef struct {
    const int16_t* sample_data;   // Pointer to sample array
    uint32_t sample_length;        // Total samples in sound
    uint32_t playback_position;    // Current read position
    bool active;                   // Is this sound playing?
    float volume;                  // 0.0 to 1.0
} active_sound_t;

#define MAX_ACTIVE_SOUNDS 8
active_sound_t active_sounds[MAX_ACTIVE_SOUNDS];

// Lock-free triggers (main loop -> audio task)
volatile bool sound_trigger[5];
```

**Audio task:**

```c
void audio_task(void* arg) {
    int16_t output_buffer[128];  // 64 stereo frames
    size_t bytes_written;

    while (1) {
        // 1. Check for new sound triggers
        for (int i = 0; i < 5; i++) {
            if (sound_trigger[i]) {
                // Find free slot and activate sound
                for (int slot = 0; slot < MAX_ACTIVE_SOUNDS; slot++) {
                    if (!active_sounds[slot].active) {
                        active_sounds[slot].sample_data = sound_samples[i];
                        active_sounds[slot].sample_length = sound_lengths[i];
                        active_sounds[slot].playback_position = 0;
                        active_sounds[slot].volume = 0.7f;
                        active_sounds[slot].active = true;
                        break;
                    }
                }
                sound_trigger[i] = false;
            }
        }

        // 2. Mix all active sounds
        for (int frame = 0; frame < 64; frame++) {
            float mix_left = 0.0f;
            float mix_right = 0.0f;

            for (int i = 0; i < MAX_ACTIVE_SOUNDS; i++) {
                if (active_sounds[i].active) {
                    // Get sample and convert to float
                    int16_t sample = active_sounds[i].sample_data[
                        active_sounds[i].playback_position
                    ];
                    float sample_f = (sample / 32768.0f) * active_sounds[i].volume;

                    // Accumulate
                    mix_left += sample_f;
                    mix_right += sample_f;

                    // Advance position
                    active_sounds[i].playback_position++;
                    if (active_sounds[i].playback_position >=
                        active_sounds[i].sample_length) {
                        active_sounds[i].active = false;
                    }
                }
            }

            // Soft clip to prevent distortion
            mix_left = fminf(1.0f, fmaxf(-1.0f, mix_left));
            mix_right = fminf(1.0f, fmaxf(-1.0f, mix_right));

            // Convert to 16-bit
            output_buffer[frame * 2] = (int16_t)(mix_left * 32767.0f);
            output_buffer[frame * 2 + 1] = (int16_t)(mix_right * 32767.0f);
        }

        // 3. Write to I2S (blocks until DMA ready)
        i2s_channel_write(i2s_handle, output_buffer,
                         sizeof(output_buffer), &bytes_written, portMAX_DELAY);
    }
}
```

**Task creation:**

```c
xTaskCreatePinnedToCore(
    audio_task,
    "audio",
    4096,                           // Stack size
    NULL,                           // Parameters
    configMAX_PRIORITIES - 2,       // High priority
    NULL,                           // Task handle
    1                               // Pin to Core 1
);
```

**Triggering sounds from main loop:**

```c
// When event occurs (e.g., ball bounce)
sound_trigger[ball_id] = true;  // Atomic write
```

## Important Gotchas

### 1. I2S Functions Are NOT ISR-Safe

**Problem:** `i2s_channel_write()` and related functions use mutex locks.

**Impact:** Cannot be called from:
- `esp_timer` callbacks (run in interrupt context)
- Hardware timer ISRs
- Any interrupt handler

**Solution:** Use FreeRTOS tasks for I2S operations.

### 2. Audio Initialization Order

**Correct order:**
```c
bsp_device_initialize();        // Initialize device first
bsp_audio_initialize(44100);    // Then initialize audio
bsp_audio_set_amplifier(true);  // Enable amplifier
bsp_audio_set_volume(100);      // Set volume
```

**Note:** `bsp_audio_initialize()` is separate from `bsp_device_initialize()`.

### 3. Master Volume vs. Mix Volume

- **Master volume** (0-100%): Controls ES8156 codec DAC output
- **Mix volume**: Control individual sound amplitudes in software

**Best practice:** Set master volume to 100%, control individual sound levels during mixing to prevent clipping.

### 4. Buffer Size Calculations

```c
// For 44,100 Hz stereo 16-bit:
samples_per_second = 44100;
bytes_per_sample = 2;  // 16-bit
channels = 2;          // Stereo

// Buffer for 10ms of audio:
duration_ms = 10;
frames = (44100 * 10) / 1000;  // 441 frames
buffer_size = frames * 2 * 2;   // 1764 bytes
```

### 5. Clipping Prevention

When mixing multiple sounds:

```c
// BAD: Can exceed ±32767
int16_t mix = sample1 + sample2 + sample3;  // May overflow!

// GOOD: Use float, then clip
float mix_f = (sample1 + sample2 + sample3) / 32768.0f;
mix_f = fminf(1.0f, fmaxf(-1.0f, mix_f));  // Clip to ±1.0
int16_t mix = (int16_t)(mix_f * 32767.0f);
```

### 6. Memory Placement

**For large sample arrays:**

```c
// Store in flash (read-only)
const int16_t sound_data[] = { /* ... */ };

// Or use PROGMEM attribute
const int16_t sound_data[] PROGMEM = { /* ... */ };
```

**For DMA buffers:**

```c
// Keep in SRAM (not PSRAM) for DMA access
int16_t output_buffer[256];  // Stack or .bss section
```

## Generating Audio Samples

### Sine Wave Generation (Python)

```python
import numpy as np
import struct

def generate_sine_wave(frequency, duration, sample_rate=44100, amplitude=0.7):
    """Generate sine wave as 16-bit PCM samples"""
    num_samples = int(duration * sample_rate)
    t = np.linspace(0, duration, num_samples, False)
    wave = np.sin(2 * np.pi * frequency * t) * amplitude
    samples = (wave * 32767).astype(np.int16)
    return samples

# Generate 440 Hz tone, 0.15 seconds
samples = generate_sine_wave(440, 0.15)

# Output as C array
print(f"const int16_t bounce_sound_440hz[{len(samples)}] = {{")
for i in range(0, len(samples), 10):
    values = ", ".join(str(s) for s in samples[i:i+10])
    print(f"    {values},")
print("};")
```

### Fade In/Out (Avoid Clicks)

```python
def apply_fade(samples, fade_ms=5, sample_rate=44100):
    """Apply fade in/out to prevent clicks"""
    fade_samples = int(fade_ms * sample_rate / 1000)

    # Fade in
    for i in range(fade_samples):
        samples[i] = int(samples[i] * (i / fade_samples))

    # Fade out
    for i in range(fade_samples):
        idx = len(samples) - fade_samples + i
        samples[idx] = int(samples[idx] * (1 - i / fade_samples))

    return samples
```

## Performance Considerations

### CPU Usage Estimates

**For 64-frame buffer @ 44.1kHz:**
- Time per buffer: 1.45 ms
- CPU @ 240 MHz: ~348,000 cycles available

**Mixing 5 sounds:**
- Operations per frame: ~50 (fetch, multiply, accumulate, convert)
- Total: 64 frames × 50 ops × 5 sounds = 16,000 operations
- Estimated cycles: ~80,000 (23% of budget)
- Actual usage: ~2-5% (compiler optimizations, FPU)

### Optimization Tips

1. **Pre-generate samples** - Store in flash, don't compute in real-time
2. **Use FPU** - ESP32 has hardware FPU, float math is fast
3. **Minimize active sounds** - Limit to 5-8 simultaneous sounds
4. **Pin task to Core 1** - Leave Core 0 for main application
5. **Use const arrays** - Compiler can optimize access

## Testing and Debugging

### Verify I2S Output

```c
// Generate simple test tone
int16_t test_buffer[128];
for (int i = 0; i < 64; i++) {
    int16_t sample = (i % 20 < 10) ? 10000 : -10000;  // Square wave
    test_buffer[i * 2] = sample;
    test_buffer[i * 2 + 1] = sample;
}

size_t written;
i2s_channel_write(i2s_handle, test_buffer, sizeof(test_buffer),
                  &written, portMAX_DELAY);
```

### Monitor Task Performance

```c
// In audio task
uint64_t start = esp_timer_get_time();
// ... mixing code ...
uint64_t elapsed = esp_timer_get_time() - start;

if (elapsed > 1450) {  // Buffer duration in microseconds
    ESP_LOGW("AUDIO", "Mixing took too long: %llu us", elapsed);
}
```

### Check for Audio Glitches

- Listen for clicks/pops (buffer underrun)
- Listen for distortion (clipping)
- Monitor CPU usage with `vTaskGetRunTimeStats()`

## References

- **ESP-IDF I2S Driver:** https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html
- **ES8156 Datasheet:** (Codec chip documentation)
- **Tanmatsu BSP:** `managed_components/badgeteam__badge-bsp/`
- **Example Implementation:** tanmatsu-ballz project (this project)

## MOD Music Playback Integration

### Overview

Adding background music via MOD (Module) files requires a separate playback engine that outputs to a ring buffer, which the audio task then mixes with sound effects.

### Ring Buffer Architecture

**Why a ring buffer?**
- Decouples MOD playback rate from I2S output rate
- Allows independent task priorities (MOD lower, audio higher)
- Provides buffering for smooth playback during CPU spikes

**Implementation:**

```c
#define MOD_BUFFER_SIZE 2048

// Global ring buffer
int16_t mod_ring_buffer[MOD_BUFFER_SIZE];
volatile uint32_t mod_write_pos = 0;
volatile uint32_t mod_read_pos = 0;

// MOD task writes samples
void write_to_ring_buffer(int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t next_pos = (mod_write_pos + 1) % MOD_BUFFER_SIZE;
        if (next_pos == mod_read_pos) {
            continue;  // Buffer full, skip sample
        }
        mod_ring_buffer[mod_write_pos] = samples[i];
        mod_write_pos = next_pos;
    }
}

// Audio task reads samples in mixing loop
if (mod_read_pos != mod_write_pos) {
    int16_t mod_sample = mod_ring_buffer[mod_read_pos];
    float mod_sample_f = mod_sample / 32768.0f;
    mix_left += mod_sample_f * 0.7f;   // 70% volume
    mix_right += mod_sample_f * 0.7f;
    mod_read_pos = (mod_read_pos + 1) % MOD_BUFFER_SIZE;
}
```

### MOD Player Task

**Key Requirements:**
1. Separate FreeRTOS task with lower priority than audio task
2. Generates mono samples at MOD native sample rate (22050 Hz)
3. Writes to ring buffer (upsampled if needed)
4. Handles tempo and pitch correctly

**Sample Rate Considerations:**

Amiga MOD files were designed for lower sample rates:
- **Typical Amiga rate:** 8363-22050 Hz
- **Modern I2S rate:** 44100 Hz

**Critical lesson:** If MOD plays at wrong sample rate, both tempo AND pitch will be wrong!

**Solution:** Play MOD at native 22050 Hz, then upsample to 44100 Hz:

```c
#define SAMPLE_RATE        22050  // MOD playback rate
#define OUTPUT_RATE        44100  // I2S output rate

static void write_to_ring_buffer(int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        int16_t scaled_sample = (int16_t)(samples[i] * 0.7f);  // Volume

        // Write sample twice to upsample 22050 -> 44100 Hz
        for (int j = 0; j < 2; j++) {
            uint32_t next_pos = (mod_write_pos + 1) % MOD_BUFFER_SIZE;
            if (next_pos == mod_read_pos) continue;
            mod_ring_buffer[mod_write_pos] = scaled_sample;
            mod_write_pos = next_pos;
        }
    }
}
```

### MOD Tempo Calculation

MOD files specify tempo in BPM and speed (ticks per row):
- **Tempo:** BPM (default 125)
- **Speed:** Ticks per row (default 6)

**Critical formula for samples per tick:**

```c
// From original Amiga/PC MOD players
samples_per_tick = (2500 * sample_rate) / (tempo * 1000);

// Example at 125 BPM, 22050 Hz:
samples_per_tick = (2500 * 22050) / (125 * 1000) = 441
```

**Implementation:**

```c
static int calculate_tick_samples(int tempo) {
    return (2500 * SAMPLE_RATE) / (tempo * 1000);
}

// In playback loop
int tempo = 125;  // Default or from MOD effect
int samples_per_tick = calculate_tick_samples(tempo);

// Generate correct number of samples
process_tick(&mod, channels, buffer, samples_per_tick);
write_to_ring_buffer(buffer, samples_per_tick);

// Delay for the time it took to generate those samples
int delay_ms = (samples_per_tick * 1000) / SAMPLE_RATE;
if (delay_ms > 0) {
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}
```

### MOD Volume Issues

8-bit MOD samples (-128 to 127) are inherently quieter than 16-bit:
- **8-bit range:** ±128 = ±0.39% of 16-bit range
- **16-bit range:** ±32768

**Solution:** Apply gain during mixing:

```c
// Convert 8-bit MOD sample to 16-bit with gain
int8_t sample_val = mod->samples[idx].data[pos];
int16_t mixed = ((sample_val * volume) / 64) * 32;  // 32x gain

// Then apply master volume when writing to ring buffer
int16_t output = (int16_t)(mixed * MOD_VOLUME_SCALE);  // 0.7 for 70%
```

### Task Priority and Timing

**Task setup:**

```c
// MOD player task (Core 1, lower priority)
xTaskCreatePinnedToCore(
    modplayer_task,
    "modplayer",
    8192,                           // Larger stack for MOD processing
    NULL,
    configMAX_PRIORITIES - 3,       // Lower than audio task
    NULL,
    1                               // Core 1
);

// Audio task (Core 1, higher priority)
xTaskCreatePinnedToCore(
    audio_task,
    "audio",
    4096,
    NULL,
    configMAX_PRIORITIES - 2,       // Higher priority
    NULL,
    1                               // Core 1
);
```

**Why this matters:**
- Audio task has higher priority → never starved
- MOD task runs when audio task is blocked on I2S write
- Both on Core 1 → leaves Core 0 for main application

### Common MOD Integration Issues

#### Issue 1: Clicking/Buzzing Instead of Music
**Symptom:** Audio sounds like rapid clicking or buzzing.
**Cause:** Task delay doesn't match actual sample generation time.
**Example:** Generating 1024 samples (~23ms) but delay is only 1ms → ring buffer overflow.
**Fix:** Match delay to actual samples generated:

```c
int samples_generated = 441;  // At 22050 Hz, 125 BPM
int delay_ms = (samples_generated * 1000) / SAMPLE_RATE;  // ~20ms
vTaskDelay(pdMS_TO_TICKS(delay_ms));
```

#### Issue 2: Music 2x Too Fast
**Symptom:** Music plays at double speed, correct pitch.
**Cause:** Generating wrong number of samples per tick.
**Example:** Fixed 1024 samples instead of calculated 441 samples.
**Fix:** Use tempo calculation formula (see above).

#### Issue 3: Music Fast AND High Pitch
**Symptom:** Both tempo and pitch are too high.
**Cause:** Playing MOD at wrong sample rate (44100 Hz instead of 22050 Hz).
**Fix:** Play MOD at native 22050 Hz, upsample to 44100 Hz (see above).

#### Issue 4: Music Too Quiet
**Symptom:** Background music barely audible compared to sound effects.
**Cause:** Insufficient gain for 8-bit samples.
**Fix:** Apply 32x gain during mixing, then volume scale at output.

### Embedding MOD Files

Convert MOD file to C header:

```bash
xxd -i popcorn_remix.mod > popcorn_remix_mod.h
```

**Result:**
```c
const unsigned char mod_data[] = {
    0x70, 0x6f, 0x70, 0x63, 0x6f, 0x72, 0x6e, ...
};
const unsigned int mod_data_len = 61436;
```

**Load from embedded data:**

```c
MODFile mod;
if (!load_mod_embedded(mod_data, mod_data_len, &mod)) {
    ESP_LOGE(TAG, "Failed to load MOD");
    return;
}
```

### Memory Considerations

**MOD file in flash:** ~60KB (read-only, no RAM cost)
**Ring buffer:** 2048 samples × 2 bytes = 4KB RAM
**MOD structures:** ~15KB RAM (patterns, channel state)
**Total RAM:** ~19KB

**Optimization:** Sample data points to flash, only patterns allocated in RAM.

### Volume Balance Best Practices

**Recommended mix:**
- Background music: 70% (0.7f)
- Sound effects: 10-30% (0.1f - 0.3f)
- Master volume: 100%

**Implementation:**

```c
// MOD player
#define MOD_VOLUME_SCALE   0.7f
output = (int16_t)(sample * MOD_VOLUME_SCALE);

// Sound effects
active_sounds[i].volume = 0.1f;

// Audio mixing
mix += mod_sample_f;              // 70%
mix += sound_sample_f * volume;   // 10%

// Soft clip
mix = fminf(1.0f, fmaxf(-1.0f, mix));
```

## Changelog

- **2025-11-14:** Initial version based on tanmatsu-ballz implementation
- **2025-11-15:** Added MOD music playback integration, ring buffer architecture, sample rate handling, tempo calculation, and common issues
