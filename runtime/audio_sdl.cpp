// SDL device output for the portable mix in audio.cpp. Desktop only; Android uses
// audio_aaudio.cpp, which is the same handful of lines against a different API.
#include "runtime.h"
#include <SDL3/SDL.h>
#include <vector>

void audio_render(int16_t* out, int frames);
void audio_open_wav();

static constexpr int OUT_RATE = 48000;
static SDL_AudioStream* g_stream;

// SDL asks for `additional` more bytes; they are mixed and handed over.
static void audio_callback(void*, SDL_AudioStream* stream, int additional, int) {
    static std::vector<int16_t> buf;
    const int frames = additional / 4;
    if (frames <= 0) return;
    buf.resize((size_t)frames * 2);
    audio_render(buf.data(), frames);
    SDL_PutAudioStreamData(stream, buf.data(), frames * 4);
}

bool audio_open() {
    audio_open_wav();
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG(LOG_AI, "audio: %s", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");   // ~10 ms, as before
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, OUT_RATE};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, nullptr);
    if (!g_stream) {
        LOG(LOG_AI, "audio: cannot open device: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

// While the game is paused: what is buffered waits for it, and the streamed music (DTK),
// which the device pulls from the disc on its own, stops with it.
void audio_set_paused(bool paused) {
    if (!g_stream) return;
    if (paused) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}
