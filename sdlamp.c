#include "SDL.h"

static SDL_AudioDeviceID audio_device = 0;
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;

#if defined(__GNUC__) || defined(__clang__)
static void panic_and_abort(const char *title, const char *text) __attribute__((noreturn));
#endif

static void panic_and_abort(const char *title, const char *text)
{
    fprintf(stderr, "PANIC: %s ... %s\n", title, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, window);
    SDL_Quit();
    exit(1);
}

static Uint8 *wavbuf = NULL;
static Uint32 wavlen = 0;
static SDL_AudioSpec wavspec;
static SDL_AudioStream *stream = NULL;

static SDL_bool open_new_audio_file(const char *fname)
{
    SDL_FreeAudioStream(stream);
    stream = NULL;
    SDL_FreeWAV(wavbuf);
    wavbuf = NULL;
    wavlen = 0;

    if (SDL_LoadWAV(fname, &wavspec, &wavbuf, &wavlen) == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load wav file!", SDL_GetError(), window);
        return SDL_FALSE;
    }

    stream = SDL_NewAudioStream(wavspec.format, wavspec.channels, wavspec.freq, AUDIO_F32, 2, 48000);
    if (!stream) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't create audio stream!", SDL_GetError(), window);
        SDL_FreeWAV(wavbuf);
        wavbuf = NULL;
        wavlen = 0;
        return SDL_FALSE;
    }

    if (SDL_AudioStreamPut(stream, wavbuf, wavlen) == -1) {  // !!! FIXME: graceful handling.
        panic_and_abort("Audio stream put failed", SDL_GetError());
    }

    SDL_AudioStreamFlush(stream);  // !!! FIXME: error handling

    return SDL_TRUE;
}

int main(int argc, char **argv)
{
    SDL_AudioSpec desired;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1) {
        panic_and_abort("SDL_Init failed", SDL_GetError());
    }

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, 0);
    if (!window) {
        panic_and_abort("SDL_CreateWindow failed", SDL_GetError());
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        panic_and_abort("SDL_CreateRenderer failed", SDL_GetError());
    }

    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_F32;
    desired.channels = 2;
    desired.samples = 4096;
    desired.callback = NULL;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (audio_device == 0) {
        panic_and_abort("Couldn't audio device!", SDL_GetError());
    }

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);  // tell SDL we want this event that is disabled by default.

    open_new_audio_file("music.wav");

    SDL_bool paused = SDL_TRUE;
    const SDL_Rect rewind_rect = { 100, 100, 100, 100 };
    const SDL_Rect pause_rect = { 400, 100, 100, 100 };

    int green = 0;
    SDL_bool keep_going = SDL_TRUE;
    while (keep_going) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    keep_going = SDL_FALSE;
                    break;

                case SDL_MOUSEBUTTONDOWN: {
                    const SDL_Point pt = { e.button.x, e.button.y };
                    if (SDL_PointInRect(&pt, &rewind_rect)) {  // pressed the "rewind" button
                        SDL_ClearQueuedAudio(audio_device);
                        SDL_AudioStreamClear(stream);
                        if (SDL_AudioStreamPut(stream, wavbuf, wavlen) == -1) {  // !!! FIXME: graceful handling.
                            panic_and_abort("Audio stream put failed", SDL_GetError());
                        }
                        SDL_AudioStreamFlush(stream);  // !!! FIXME: error handling
                    } else if (SDL_PointInRect(&pt, &pause_rect)) {  // pressed the "pause" button
                        paused = paused ? SDL_FALSE : SDL_TRUE;
                        SDL_PauseAudioDevice(audio_device, paused);
                    }
                    break;
                }

                case SDL_DROPFILE: {
                    open_new_audio_file(e.drop.file);
                    SDL_free(e.drop.file);
                    break;
                }
            }
        }

        if (SDL_GetQueuedAudioSize(audio_device) < 8192) {
            const int bytes_remaining = SDL_AudioStreamAvailable(stream);
            if (bytes_remaining > 0) {
                const int new_bytes = SDL_min(bytes_remaining, 32 * 1024);
                static Uint8 converted_buffer[32 * 1024];
                SDL_AudioStreamGet(stream, converted_buffer, new_bytes);  // !!! FIXME: error checking
                SDL_QueueAudio(audio_device, converted_buffer, new_bytes);
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, green, 0, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

        SDL_RenderFillRect(renderer, &rewind_rect);
        SDL_RenderFillRect(renderer, &pause_rect);
        SDL_RenderPresent(renderer);

        green = (green + 1) % 256;
    }

    SDL_FreeWAV(wavbuf);
    SDL_CloseAudioDevice(audio_device);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}

