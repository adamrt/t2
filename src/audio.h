#pragma once

#include <stdbool.h>
#include <stdint.h>

// The sound device: a queue of signed 16-bit mono samples at AUDIO_RATE.

#define AUDIO_RATE 22050

typedef struct Audio Audio;

// Opens the host's sound output. A silent device plays nothing, but empties
// its queue at the same pace, so programs behave the same.
Audio *audio_open(bool silent);
void audio_close(Audio *a);
// Queues samples, unless that would put more than two seconds in the queue.
bool audio_queue(Audio *a, const uint8_t *samples, uint32_t bytes);
// Bytes queued and not played yet.
uint32_t audio_queued(Audio *a);
