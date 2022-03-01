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

int main(int argc, char **argv)
{
    Uint8 *wavbuf = NULL;
    Uint32 wavlen = 0;
    SDL_AudioSpec wavspec;

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

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);  // tell SDL we want this event that is disabled by default.

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
                    if (SDL_PointInRect(&pt, &rewind_rect)) {
                        if (audio_device) {
                            SDL_ClearQueuedAudio(audio_device);
                            SDL_QueueAudio(audio_device, wavbuf, wavlen);
                        }
                    } else if (SDL_PointInRect(&pt, &pause_rect)) {
                        paused = paused ? SDL_FALSE : SDL_TRUE;
                        if (audio_device) {
                            SDL_PauseAudioDevice(audio_device, paused);
                        }
                    }
                    break;
                }

                case SDL_DROPFILE: {
                    if (audio_device) {
                        SDL_CloseAudioDevice(audio_device);
                        audio_device = 0;
                    }

                    SDL_FreeWAV(wavbuf);
                    wavbuf = NULL;
                    wavlen = 0;

                    if (SDL_LoadWAV(e.drop.file, &wavspec, &wavbuf, &wavlen) == NULL) {
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load wav file!", SDL_GetError(), window);
                    }

                    #if 0  // !!! FIXME: come back to this.
                    SDL_AudioSpec desired;
                    SDL_zero(desired);
                    desired.freq = 48000;
                    desired.format = AUDIO_F32;
                    desired.channels = 2;
                    desired.samples = 4096;
                    desired.callback = NULL;
                    #endif

                    audio_device = SDL_OpenAudioDevice(NULL, 0, &wavspec, NULL, 0);
                    if (audio_device == 0) {
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't audio device!", SDL_GetError(), window);
                        SDL_FreeWAV(wavbuf);
                        wavbuf = NULL;
                        wavlen = 0;
                    } else {
                        SDL_QueueAudio(audio_device, wavbuf, wavlen);
                        SDL_PauseAudioDevice(audio_device, paused);
                    }

                    SDL_free(e.drop.file);
                    break;
                }

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

