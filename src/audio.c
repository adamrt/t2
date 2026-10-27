#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "host.h"

#include <stdlib.h>

#ifndef HEADLESS
#include <SDL3/SDL.h>
#endif

#define AUDIO_MAX_QUEUED_BYTES (AUDIO_RATE * 2 * 2)

// Built without SDL, every device is silent.
struct Audio {
#ifndef HEADLESS
    SDL_AudioStream *stream;
#endif
    // For the silent device: bytes queued, and when that was last worked out.
    uint32_t queued_bytes;
    uint64_t updated_at_us;
};

Audio *
audio_open(bool silent)
{
    Audio *audio = calloc(1, sizeof(Audio));
    if (audio == NULL)
        return NULL;
    audio->updated_at_us = host_monotonic_us();
#ifndef HEADLESS
    if (silent || !SDL_InitSubSystem(SDL_INIT_AUDIO))
        return audio;
    SDL_AudioSpec spec = { .format = SDL_AUDIO_S16LE, .channels = 1, .freq = AUDIO_RATE };
    audio->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio->stream != NULL)
        SDL_ResumeAudioStreamDevice(audio->stream);
#else
    (void)silent;
#endif
    return audio;
}

void
audio_close(Audio *audio)
{
    if (audio == NULL)
        return;
#ifndef HEADLESS
    if (audio->stream != NULL)
        SDL_DestroyAudioStream(audio->stream);
#endif
    free(audio);
}

uint32_t
audio_queued(Audio *audio)
{
#ifndef HEADLESS
    if (audio->stream != NULL) {
        int n = SDL_GetAudioStreamQueued(audio->stream);
        return n > 0 ? (uint32_t)n : 0;
    }
#endif
    uint64_t now_us = host_monotonic_us();
    uint64_t played_bytes = (now_us - audio->updated_at_us) * AUDIO_RATE * 2 / 1000000;
    if (played_bytes >= audio->queued_bytes) {
        audio->queued_bytes = 0;
        audio->updated_at_us = now_us;
    } else if (played_bytes > 0) {
        // Only the time those whole bytes took passes, so none is lost.
        audio->queued_bytes -= (uint32_t)played_bytes;
        audio->updated_at_us += played_bytes * 1000000 / (AUDIO_RATE * 2);
    }
    return audio->queued_bytes;
}

bool
audio_queue(Audio *audio, const uint8_t *samples, uint32_t bytes)
{
    if (audio_queued(audio) + bytes > AUDIO_MAX_QUEUED_BYTES)
        return false;
#ifndef HEADLESS
    if (audio->stream != NULL)
        return SDL_PutAudioStreamData(audio->stream, samples, (int)bytes);
#else
    (void)samples;
#endif
    audio->queued_bytes += bytes;
    return true;
}
