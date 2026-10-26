#define _POSIX_C_SOURCE 200809L

#include "audio.h"

#include <SDL3/SDL.h>
#include <time.h>

#define MAX_QUEUED (AUDIO_RATE * 2 * 2)

struct Audio {
    SDL_AudioStream *stream;
    // For the silent device: bytes queued, and when that was last worked out.
    uint32_t queued;
    uint64_t since;
};

static uint64_t
micros(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

Audio *
audio_open(bool silent)
{
    Audio *a = SDL_calloc(1, sizeof(Audio));
    a->since = micros();
    if (silent || !SDL_InitSubSystem(SDL_INIT_AUDIO))
        return a;
    SDL_AudioSpec spec = { .format = SDL_AUDIO_S16LE, .channels = 1, .freq = AUDIO_RATE };
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (a->stream != NULL)
        SDL_ResumeAudioStreamDevice(a->stream);
    return a;
}

void
audio_close(Audio *a)
{
    if (a == NULL)
        return;
    if (a->stream != NULL)
        SDL_DestroyAudioStream(a->stream);
    SDL_free(a);
}

uint32_t
audio_queued(Audio *a)
{
    if (a->stream != NULL) {
        int n = SDL_GetAudioStreamQueued(a->stream);
        return n > 0 ? (uint32_t)n : 0;
    }
    uint64_t now = micros();
    uint64_t played = (now - a->since) * AUDIO_RATE * 2 / 1000000;
    if (played > 0) {
        a->queued = played >= a->queued ? 0 : a->queued - (uint32_t)played;
        a->since = now;
    }
    return a->queued;
}

bool
audio_queue(Audio *a, const uint8_t *samples, uint32_t bytes)
{
    if (audio_queued(a) + bytes > MAX_QUEUED)
        return false;
    if (a->stream != NULL)
        return SDL_PutAudioStreamData(a->stream, samples, (int)bytes);
    a->queued += bytes;
    return true;
}
