// MOD Player for ESP32
// Adapted from modplayer.c for I2S audio output
// Plays 4-channel Amiga MOD files

#ifndef MODPLAYER_ESP32_H
#define MODPLAYER_ESP32_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Ring buffer for MOD output
#define MOD_BUFFER_SIZE 2048

// Global ring buffer (written by MOD task, read by audio task)
extern int16_t mod_ring_buffer[MOD_BUFFER_SIZE];
extern volatile uint32_t mod_write_pos;
extern volatile uint32_t mod_read_pos;
extern SemaphoreHandle_t mod_buffer_mutex;

// MOD player control
extern volatile bool mod_player_running;

// Initialize MOD player (call once at startup)
void modplayer_init(void);

// MOD player task (runs continuously)
void modplayer_task(void* arg);

// Stop MOD playback
void modplayer_stop(void);

#endif // MODPLAYER_ESP32_H
