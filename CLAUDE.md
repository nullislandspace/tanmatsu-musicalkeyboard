# Claude Implementation Log - Tanmatsu Ballz I2S Audio

## Project Overview
Adding I2S sound support to the bouncing balls demo with proper audio mixing.

## Requirements
- Play a short click sound when balls bounce off screen edges
- 5 balls with different frequencies (one per ball)
- Support overlapping/simultaneous sounds
- Non-blocking audio playback (animation must not pause)
- Master volume at 100%, individual sounds scaled to prevent clipping
- Use background audio mixing task with I2S writes independent of main loop

## Architecture Decisions

### Audio System Design
**Chosen Approach:** FreeRTOS Task-based Audio Mixer

**Rationale:**
- Cannot use `esp_timer` callbacks for I2S writes (I2S driver uses mutexes, not ISR-safe)
- Task-based approach allows blocking I2S writes that yield to other tasks
- Clean separation: Core 0 runs main loop, Core 1 runs audio task
- Lock-free communication via simple boolean trigger array

**Key Components:**
1. **Pre-generated sound samples** - 5 sine waves stored in flash
2. **Active sound tracker** - Tracks up to 5 concurrent sounds with playback positions
3. **Audio mixing task** - High-priority task on Core 1
4. **Lock-free triggers** - `volatile bool sound_trigger[5]` for communication
5. **Float-based mixing** - Accumulates samples, soft-clips output

### Technical Specifications
- **Sample Rate:** 44,100 Hz
- **Format:** 16-bit stereo PCM
- **DMA Configuration:** 2 descriptors × 64 frames = 512 bytes
- **Buffer Size:** 64 frames (256 bytes per write)
- **Latency:** ~1.45ms per buffer
- **CPU Budget:** <2% for audio mixing
- **Memory:** ~90KB flash for samples, ~1KB RAM for buffers

### Sound Design
- **Frequencies:** 440, 554, 659, 784, 880 Hz (pentatonic scale)
- **Duration:** 0.15 seconds (~6,615 samples)
- **Amplitude:** 60-70% of maximum to prevent clipping when mixed

## Implementation Progress

### Phase 1: Documentation
- [x] Create CLAUDE.md (this file)
- [x] Create I2S_HOWTO.md

### Phase 2: Audio Sample Generation
- [x] Generate 5 sine wave arrays using Perl
- [x] Created `main/bounce_sounds.h` with pre-generated samples

### Phase 3: Code Implementation
- [x] Add audio data structures
- [x] Implement audio mixing task
- [x] Add initialization code
- [x] Integrate with bounce detection
- [x] Add cleanup code (handled by task termination)

### Phase 4: Testing
- [x] Build and test - Build successful!

## Implementation Details

### Files Created/Modified
1. **`main/bounce_sounds.h`** (241KB) - Auto-generated sound samples
   - 5 sine waves: 440, 554, 659, 784, 880 Hz
   - 6,615 samples each (0.15 seconds @ 44.1kHz)
   - Includes fade in/out to prevent clicks
   - Lookup arrays for easy access

2. **`main/main.c`** - Modified to add audio system
   - Added includes for I2S, FreeRTOS, math, audio BSP
   - Declared external `bsp_audio_initialize()` function
   - Added audio data structures and globals
   - Implemented `audio_task()` for mixing and I2S output
   - Initialized audio system in `app_main()`
   - Integrated sound triggers in bounce detection loop

3. **`CLAUDE.md`** (this file) - Implementation tracking

4. **`I2S_HOWTO.md`** - Comprehensive I2S guide for future projects

### Code Architecture

**Main Loop (Core 0):**
- Runs physics simulation and rendering
- Sets `sound_trigger[i] = true` when ball bounces
- Non-blocking, runs at full speed

**Audio Task (Core 1):**
- High priority FreeRTOS task
- Checks sound triggers each cycle
- Mixes up to 5 active sounds using float accumulation
- Soft-clips output to prevent distortion
- Writes 64 stereo frames to I2S (~1.45ms blocking)
- Runs independently of main loop timing

**Communication:**
- Lock-free via `volatile bool sound_trigger[5]`
- Main loop writes (sets to true)
- Audio task reads and clears
- No mutex needed (simple producer-consumer)

## Research References
- ESP-IDF I2S Standard Mode: `/examples/peripherals/i2s/i2s_basic/i2s_std/main/i2s_std_example_main.c`
- Tanmatsu Nofrendo I2S: `/home/cavac/src/tanmatsu/tanmatsu-nofrendo/main/main.c`
- Tanmatsu BSP Audio: `managed_components/badgeteam__badge-bsp/targets/tanmatsu/badge_bsp_audio.c`
- ESP Timer API: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html
- Real-world example: Infrasonic Audio ESP32 synthesizer (blog.infrasonicaudio.com)

## Issues and Solutions

### Issue 1: Sample Generation Tool
**Problem:** User requested Perl instead of Python for sample generation.
**Solution:** Rewrote sample generation script in Perl, successfully generated all 5 sound samples.

### Issue 2: Build System
**Problem:** Need to ensure audio BSP headers are accessible.
**Solution:** Added `#include "bsp/audio.h"` and declared external `bsp_audio_initialize()` function.

## Build Results

**Build Status:** ✓ Successful

```
Project build complete.
Binary size: 0x7edc0 bytes (519 KB)
Partition size: 0x100000 bytes (1 MB)
Free space: 0x81240 bytes (520 KB, 50% free)
```

All components compiled successfully with no errors.

## Testing Notes

**Hardware Testing: ✅ COMPLETED AND VERIFIED**

The implementation has been tested on actual Tanmatsu hardware and confirmed working:
1. ✅ Each ball plays a different tone when bouncing (pentatonic scale)
2. ✅ Multiple balls can bounce simultaneously with sounds mixing cleanly
3. ✅ Animation remains smooth (no pausing or stuttering)
4. ✅ LEDs and keyboard backlight continue working as expected

**Status: FULLY FUNCTIONAL**

---

## MOD Player Integration (Background Music)

### Phase 1: Research and Planning
- [x] Analyze original modplayer.c from /home/cavac/src/modtracker/
- [x] Understand MOD file format and playback engine
- [x] Design integration strategy with existing I2S audio system
- [x] Choose ring buffer architecture for audio mixing

### Phase 2: File Conversion and Headers
- [x] Convert popcorn_remix.mod (60KB) to C header using xxd
- [x] Create modplayer_esp32.h interface
- [x] Create modplayer_esp32.c adapted from modplayer.c

### Phase 3: Integration
- [x] Add ring buffer (2048 samples) for MOD output
- [x] Modify audio_task to mix MOD background with bounce sounds
- [x] Initialize MOD player and create playback task
- [x] Update CMakeLists.txt to include modplayer in build

### Phase 4: Build and Test
- [x] Build successful! Binary size: 586 KB (44% partition free)
- [x] Hardware testing - initial issues found and fixed

### Implementation Details

**MOD Player Architecture:**
- **Separate FreeRTOS task** on Core 1 (lower priority than audio task)
- **Ring buffer** (2048 int16_t samples) for MOD→audio communication
- **4-channel Amiga MOD** playback with full effect support
- **30% volume scaling** for background music
- **Continuous looping** playback

**File Details:**
- **popcorn_remix.mod** - 60KB, 4-channel MOD by xain l.k (Oct 1990)
- Embedded directly in flash (no file I/O needed)
- Parsed once at startup, patterns allocated dynamically

**Audio Mixing Flow:**
```
1. MOD task → generates samples → writes to ring buffer
2. Audio task → reads MOD samples (30%) + bounce sounds (70%)
3. Soft clipping and stereo conversion
4. I2S output
```

**Memory Usage:**
- Flash: +60KB for MOD data, +~7KB for code
- RAM: +8KB ring buffer, +~15KB for MOD structures
- Total increase: ~67KB (acceptable for ESP32)

**Technical Specs:**
- Sample rate: 44,100 Hz (matches I2S)
- Format: Mono MOD output duplicated to stereo
- Ring buffer: 2048 samples (~46ms latency)
- MOD task priority: configMAX_PRIORITIES - 3
- Audio task priority: configMAX_PRIORITIES - 2

### Files Created/Modified for MOD Player

1. **main/popcorn_remix_mod.h** (241KB) - Embedded MOD file data
2. **main/modplayer_esp32.h** - MOD player interface
3. **main/modplayer_esp32.c** - Adapted MOD playback engine
4. **main/main.c** - Integrated MOD mixing into audio_task
5. **main/CMakeLists.txt** - Added modplayer_esp32.c to build

### Key Adaptations from Original modplayer.c

**Removed:**
- All ALSA audio output code
- File I/O (fopen, fread, etc.)
- Terminal visualization and user interaction
- malloc for sample data (now points to flash)

**Added:**
- FreeRTOS task-based playback
- Ring buffer output for integration
- ESP32 heap allocation for patterns
- Continuous looping functionality
- Volume scaling (30%) for background music
- ESP-IDF logging

**Preserved:**
- Complete MOD format parsing
- 4-channel mixing engine
- Sample playback with looping
- Effect processing (volume, speed, tempo, pattern break)
- Amiga period-to-frequency conversion

### Build Results

**Before MOD Player:**
- Binary size: 0x7edc0 (519 KB)
- Free space: 0x81240 (520 KB, 50%)

**After MOD Player:**
- Binary size: 0x8e750 (586 KB)
- Free space: 0x718b0 (465 KB, 44%)
- Increase: +67 KB

### Issues and Fixes

#### Issue 1: Audio Clicking/Buzzing Instead of Music
**Problem:** MOD playback sounded like low clicking/buzzing noise instead of music.
**Root Cause:** MOD task delay was 1ms but generated 1024 samples (~23ms at 44.1kHz), causing ring buffer overflow and sample dropping.
**Solution:** Changed vTaskDelay from 1ms to 20ms in modplayer_esp32.c:383.
**Result:** ✅ Music played correctly.

#### Issue 2: Keyboard Input Not Working
**Problem:** Cannot exit program using keyboard after MOD integration.
**Root Cause:** Input queue timeout was set to 0 (non-blocking), main loop spinning too fast.
**Solution:** Changed delay from 0 to pdMS_TO_TICKS(1) in main.c:292 (1ms timeout for responsive input).
**Result:** ✅ Keyboard handling restored.

#### Issue 3: Background Music Too Quiet
**Problem:** MOD music volume very low compared to bounce sounds.
**Root Cause:** 8-bit MOD samples (-128 to 127) inherently quieter than 16-bit, only had 4x gain.
**Solution:** Increased internal gain from 4x to 32x in modplayer_esp32.c:244.
**Result:** ✅ Music audible at appropriate volume.

#### Issue 4: Music Playing 2x Too Fast
**Problem:** Music tempo approximately twice as fast as it should be.
**Root Cause:** Generated fixed 1024 samples per tick instead of calculating correct samples based on tempo (should be 882 at 125 BPM).
**Solution:**
- Added `calculate_tick_samples()` function using formula: `(2500 × sample_rate) / (tempo × 1000)`
- Modified tick processing to generate correct number of samples based on current tempo
- Fixed delay calculation to match actual samples generated
**Result:** ✅ Tempo corrected to proper speed.

#### Issue 5: Speed and Pitch Both Too High
**Problem:** Music still too fast and pitch too high after tempo fix.
**Root Cause:** MOD playback rate at 44100 Hz was too high. Original Amiga MODs typically play at lower sample rates (8363-22050 Hz).
**Solution:**
- Changed SAMPLE_RATE from 44100 to 22050 in modplayer_esp32.c:24
- Implemented 2x upsampling in write_to_ring_buffer() - duplicate each sample
- This maintains correct pitch and tempo while outputting at 44100 Hz for I2S
**Changes:**
```c
#define SAMPLE_RATE        22050  // MOD playback rate (half of I2S output rate)

static void write_to_ring_buffer(int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        int16_t scaled_sample = (int16_t)(samples[i] * MOD_VOLUME_SCALE);
        // Write sample twice to upsample from 22050 to 44100 Hz
        for (int j = 0; j < 2; j++) {
            uint32_t next_pos = (mod_write_pos + 1) % MOD_BUFFER_SIZE;
            if (next_pos == mod_read_pos) continue;
            mod_ring_buffer[mod_write_pos] = scaled_sample;
            mod_write_pos = next_pos;
        }
    }
}
```
**Result:** ✅ Correct tempo and pitch (pending user verification).

### Volume Balance Adjustments
**User Request:** 70% background music, 10% ball sounds.
**Implementation:**
- MOD_VOLUME_SCALE = 0.7f in modplayer_esp32.c:27
- Ball sound volume = 0.1f in main.c:84
**Result:** ✅ Proper volume balance.

### Final Technical Specifications

**MOD Playback:**
- Internal sample rate: 22,050 Hz (matches Amiga MOD standards)
- Output sample rate: 44,100 Hz (2x upsampling)
- Volume: 70% of maximum
- Internal gain: 32x (compensates for 8-bit samples)
- Format: Mono upsampled to stereo
- Tempo calculation: `(2500 × 22050) / (tempo × 1000)` samples per tick
  - At 125 BPM: 441 samples per tick at 22.05kHz (882 after upsampling)

**Ball Sounds:**
- Volume: 10% of maximum
- Sample rate: 44,100 Hz native
- Format: Mono duplicated to stereo

**Audio Mixing:**
- Float accumulation prevents overflow
- Soft clipping prevents distortion
- MOD + ball sounds mixed in audio_task before I2S output

## Future Enhancements

Possible improvements for future iterations:
1. Add pitch variation based on ball velocity
2. Implement simple reverb/echo for spatial effects
3. Add volume fade-out for smoother sound endings
4. Support for additional sound effects (whoosh, collision, etc.)
5. Dynamic frequency adjustment based on collision energy
6. Support for multiple MOD files or playlist functionality
7. Add MOD file selection via user input
