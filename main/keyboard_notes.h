// Musical keyboard note definitions and scancode mappings
// Generated for Tanmatsu Musical Keyboard project
//
// White keys: ASDFGHJK (C natural scale)
// Black keys: QWRTY (sharps/flats)

#ifndef KEYBOARD_NOTES_H
#define KEYBOARD_NOTES_H

#include <stdint.h>
#include "bsp/input.h"

// Total number of notes (8 white + 5 black)
#define NUM_NOTES 13

// ADSR envelope parameters (piano-like sound)
#define ADSR_ATTACK_MS   5      // Attack time in milliseconds
#define ADSR_DECAY_MS    100    // Decay time in milliseconds
#define ADSR_SUSTAIN_LEVEL 0.7f // Sustain level (0.0 to 1.0)
#define ADSR_RELEASE_MS  50     // Release time in milliseconds

// Convert milliseconds to samples at 44.1kHz
#define ADSR_ATTACK_SAMPLES  (44100 * ADSR_ATTACK_MS / 1000)   // 220 samples
#define ADSR_DECAY_SAMPLES   (44100 * ADSR_DECAY_MS / 1000)    // 4410 samples
#define ADSR_RELEASE_SAMPLES (44100 * ADSR_RELEASE_MS / 1000)  // 2205 samples

// Note structure
typedef struct {
    const char* name;          // Note name (e.g., "C1", "C#1")
    float frequency;           // Frequency in Hz
    uint32_t scancode_press;   // Scancode when key is pressed
    uint32_t scancode_release; // Scancode when key is released
    bool is_black_key;         // true for sharps/flats, false for natural notes
} note_def_t;

// Note definitions (13 notes: C1 to C2 chromatic scale)
const note_def_t note_defs[NUM_NOTES] = {
    // White keys (ASDFGHJK row)
    {"C1",  261.63, 0x1E, 0x9E, false},  // A key
    {"D1",  293.66, 0x1F, 0x9F, false},  // S key
    {"E1",  329.63, 0x20, 0xA0, false},  // D key
    {"F1",  349.23, 0x21, 0xA1, false},  // F key
    {"G1",  392.00, 0x22, 0xA2, false},  // G key
    {"A1",  440.00, 0x23, 0xA3, false},  // H key
    {"B1",  493.88, 0x24, 0xA4, false},  // J key
    {"C2",  523.25, 0x25, 0xA5, false},  // K key

    // Black keys (QWRTY row)
    {"C#1", 277.18, 0x10, 0x90, true},   // Q key (between C and D)
    {"D#1", 311.13, 0x11, 0x91, true},   // W key (between D and E)
    {"F#1", 369.99, 0x13, 0x93, true},   // R key (between F and G)
    {"G#1", 415.30, 0x14, 0x94, true},   // T key (between G and A)
    {"A#1", 466.16, 0x15, 0x95, true},   // Y key (between A and B)
};

// Helper function: Find note index by scancode
// Returns: note index (0-12) or -1 if not found
static inline int find_note_by_scancode(uint32_t scancode) {
    // Remove release modifier if present
    scancode &= ~BSP_INPUT_SCANCODE_RELEASE_MODIFIER;

    for (int i = 0; i < NUM_NOTES; i++) {
        if (note_defs[i].scancode_press == scancode) {
            return i;
        }
    }
    return -1;  // Not found
}

// Helper function: Check if scancode is a key press
static inline bool is_key_press(uint32_t scancode) {
    return !(scancode & BSP_INPUT_SCANCODE_RELEASE_MODIFIER);
}

// Helper function: Check if scancode is a key release
static inline bool is_key_release(uint32_t scancode) {
    return (scancode & BSP_INPUT_SCANCODE_RELEASE_MODIFIER);
}

#endif // KEYBOARD_NOTES_H
