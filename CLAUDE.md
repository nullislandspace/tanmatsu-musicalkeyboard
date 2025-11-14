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

The implementation is ready for hardware testing. Expected behavior:
1. Each ball should play a different tone when bouncing (pentatonic scale)
2. Multiple balls can bounce simultaneously with sounds mixing
3. Animation should remain smooth (no pausing)
4. LEDs and keyboard backlight should still work as before

## Future Enhancements

Possible improvements for future iterations:
1. Add pitch variation based on ball velocity
2. Implement simple reverb/echo for spatial effects
3. Add volume fade-out for smoother sound endings
4. Support for additional sound effects (whoosh, collision, etc.)
5. Dynamic frequency adjustment based on collision energy
