#include <stdio.h>
#include "SDL.h"

typedef struct
{
    SDL_Texture *texture;  // YOU DO NOT OWN THIS POINTER, DON'T FREE IT.
    SDL_Rect srcrect_unpressed;
    SDL_Rect srcrect_pressed;
    SDL_Rect dstrect;
    SDL_bool pressed;
} WinAmpSkinButton;

typedef enum
{
    WASBTN_PREV=0,
    WASBTN_PLAY,
    WASBTN_PAUSE,
    WASBTN_STOP,
    WASBTN_NEXT,
    WASBTN_EJECT,
    WASBTN_TOTAL
} WinAmpSkinButtonId;

typedef struct
{
    SDL_Texture *tex_main;
    SDL_Texture *tex_cbuttons;
    WinAmpSkinButton buttons[WASBTN_TOTAL];
} WinAmpSkin;

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

static WinAmpSkin skin;
static float volume_slider_value = 1.0f;
static float balance_slider_value = 0.5f;

static Uint8 *wavbuf = NULL;
static Uint32 wavlen = 0;
static SDL_AudioSpec wavspec;
static SDL_AudioStream *stream = NULL;

static void stop_audio(void)
{
    if (stream) {
        SDL_FreeAudioStream(stream);
    }

    if (wavbuf) {
        SDL_FreeWAV(wavbuf);
    }

    stream = NULL;
    wavbuf = NULL;
    wavlen = 0;
}


static SDL_bool open_new_audio_file(const char *fname)
{
    SDL_FreeAudioStream(stream);
    stream = NULL;
    SDL_FreeWAV(wavbuf);
    wavbuf = NULL;
    wavlen = 0;

    if (SDL_LoadWAV(fname, &wavspec, &wavbuf, &wavlen) == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load wav file!", SDL_GetError(), window);
        goto failed;
    }

    stream = SDL_NewAudioStream(wavspec.format, wavspec.channels, wavspec.freq, AUDIO_F32, 2, 48000);
    if (!stream) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't create audio stream!", SDL_GetError(), window);
        goto failed;
    }

    if (SDL_AudioStreamPut(stream, wavbuf, wavlen) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Audio stream put failed", SDL_GetError(), window);
        goto failed;
    }

    if (SDL_AudioStreamFlush(stream) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Audio stream flush failed", SDL_GetError(), window);
        goto failed;
    }

    return SDL_TRUE;

failed:
    stop_audio();
    return SDL_FALSE;
}

static SDL_Texture *load_texture(const char *fname)
{
    SDL_Surface *surface = SDL_LoadBMP(fname);
    if (!surface) {
        return NULL;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;  // MAY BE NULL.
}


static SDL_INLINE void init_skin_button(WinAmpSkinButton *btn, SDL_Texture *tex,
                                        const int w, const int h,
                                        const int dx, const int dy,
                                        const int sxu, const int syu,
                                        const int sxp, const int syp)
{
    btn->texture = tex;
    btn->srcrect_unpressed.x = sxu;
    btn->srcrect_unpressed.y = syu;
    btn->srcrect_unpressed.w = w;
    btn->srcrect_unpressed.h = h;
    btn->srcrect_pressed.x = sxp;
    btn->srcrect_pressed.y = syp;
    btn->srcrect_pressed.w = w;
    btn->srcrect_pressed.h = h;
    btn->dstrect.x = dx;
    btn->dstrect.y = dy;
    btn->dstrect.w = w;
    btn->dstrect.h = h;
    btn->pressed = SDL_FALSE;
}

static SDL_bool load_skin(WinAmpSkin *skin, const char *fname)  // !!! FIXME: use this variable
{
    SDL_zerop(skin);

    skin->tex_main = load_texture("hifi/Main.bmp");  // !!! FIXME: hardcoded
    skin->tex_cbuttons = load_texture("hifi/Cbuttons.bmp"); // !!! FIXME: hardcoded

    init_skin_button(&skin->buttons[WASBTN_PREV], skin->tex_cbuttons, 23, 18, 16, 88, 0, 0, 0, 18);
    init_skin_button(&skin->buttons[WASBTN_PLAY], skin->tex_cbuttons, 23, 18, 39, 88, 23, 0, 23, 18);
    init_skin_button(&skin->buttons[WASBTN_PAUSE], skin->tex_cbuttons, 23, 18, 62, 88, 46, 0, 46, 18);
    init_skin_button(&skin->buttons[WASBTN_STOP], skin->tex_cbuttons, 23, 18, 85, 88, 69, 0, 69, 18);
    init_skin_button(&skin->buttons[WASBTN_NEXT], skin->tex_cbuttons, 22, 18, 108, 88, 92, 0, 92, 18);
    init_skin_button(&skin->buttons[WASBTN_EJECT], skin->tex_cbuttons, 22, 16, 136, 89, 114, 0, 114, 16);

    return SDL_TRUE;
}

static void init_everything(int argc, char **argv)
{
    SDL_AudioSpec desired;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1) {
        panic_and_abort("SDL_Init failed", SDL_GetError());
    }

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 275, 116, 0);
    if (!window) {
        panic_and_abort("SDL_CreateWindow failed", SDL_GetError());
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        panic_and_abort("SDL_CreateRenderer failed", SDL_GetError());
    }

    if (!load_skin(&skin, "")) {  // !!! FIXME: load a real thing, not an empty string
        panic_and_abort("Failed to load initial skin", SDL_GetError());
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
}

static void draw_button(SDL_Renderer *renderer, WinAmpSkinButton *btn)
{
    const SDL_bool pressed = btn->pressed;
    if (btn->texture == NULL) {
        if (pressed) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        }
        SDL_RenderFillRect(renderer, &btn->dstrect);
    } else {
        SDL_RenderCopy(renderer, btn->texture, pressed ? &btn->srcrect_pressed : &btn->srcrect_unpressed, &btn->dstrect);
    }
}

static void draw_frame(SDL_Renderer *renderer, WinAmpSkin *skin)
{
    int i;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_RenderCopy(renderer, skin->tex_main, NULL, NULL);

    for (i = 0; i < SDL_arraysize(skin->buttons); i++) {
        draw_button(renderer, &skin->buttons[i]);
    }

    SDL_RenderPresent(renderer);
}

static void feed_more_audio(void)
{
    if (SDL_GetQueuedAudioSize(audio_device) < 8192) {
        const int bytes_remaining = SDL_AudioStreamAvailable(stream);
        if (bytes_remaining > 0) {
            const int new_bytes = SDL_min(bytes_remaining, 32 * 1024);
            static Uint8 converted_buffer[32 * 1024];
            const int num_converted_bytes = SDL_AudioStreamGet(stream, converted_buffer, new_bytes);
            if (num_converted_bytes > 0) {
                const int num_samples = (num_converted_bytes / sizeof (float));
                float *samples = (float *) converted_buffer;

                SDL_assert((num_samples % 2) == 0);  // this should always be stereo data (at least for now).

                // change the volume of the audio we're playing.
                if (volume_slider_value != 1.0f) {
                    for (int i = 0; i < num_samples; i++) {
                        samples[i] *= volume_slider_value;
                    }
                }

                // first sample is left, second is right.
                // change the balance of the audio we're playing.
                if (balance_slider_value > 0.5f) {
                    for (int i = 0; i < num_samples; i += 2) {
                        samples[i] *= 1.0f - balance_slider_value;
                    }
                } else if (balance_slider_value < 0.5f) {
                    for (int i = 0; i < num_samples; i += 2) {
                        samples[i+1] *= balance_slider_value;
                    }
                }

                SDL_QueueAudio(audio_device, converted_buffer, num_converted_bytes);
            }
        }
    }
}

static void deinit_everything(void)
{
    // !!! FIXME: free_skin()
    SDL_FreeWAV(wavbuf);
    SDL_CloseAudioDevice(audio_device);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

static SDL_bool paused = SDL_TRUE;  // !!! FIXME: move this later.

static SDL_bool handle_events(WinAmpSkin *skin)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                return SDL_FALSE;  // don't keep going.

            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEBUTTONDOWN: {
                const SDL_bool pressed = (e.button.state == SDL_PRESSED) ? SDL_TRUE : SDL_FALSE;
                const SDL_Point pt = { e.button.x, e.button.y };
                int i;

                for (i = 0; i < SDL_arraysize(skin->buttons); i++) {
                    WinAmpSkinButton *btn = &skin->buttons[i];
                    btn->pressed = (pressed && SDL_PointInRect(&pt, &btn->dstrect)) ? SDL_TRUE : SDL_FALSE;
                    if (btn->pressed) {
                        switch ((WinAmpSkinButtonId) i) {
                            case WASBTN_PREV:
                                SDL_ClearQueuedAudio(audio_device);
                                SDL_AudioStreamClear(stream);
                                if (SDL_AudioStreamPut(stream, wavbuf, wavlen) == -1) {
                                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Audio stream put failed", SDL_GetError(), window);
                                    stop_audio();
                                } else if (SDL_AudioStreamFlush(stream) == -1) {
                                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Audio stream flush failed", SDL_GetError(), window);
                                    stop_audio();
                                }
                                break;

                            case WASBTN_PAUSE:
                                paused = paused ? SDL_FALSE : SDL_TRUE;
                                SDL_PauseAudioDevice(audio_device, paused);
                                break;

                            case WASBTN_STOP:
                                stop_audio();
                                break;
                        }
                    }
                }
                break;
            }

            #if 0
            case SDL_MOUSEMOTION: {
                const SDL_Point pt = { e.motion.x, e.motion.y };
                if (SDL_PointInRect(&pt, &volume_rect) && (e.motion.state & SDL_BUTTON_LMASK)) {  // mouse is pressed inside the "volume" "slider"?
                    const float fx = (float) (pt.x - volume_rect.x);
                    volume_slider_value = (fx / ((float) volume_rect.w));  // a value between 0.0f and 1.0f
                    //printf("SLIDING! At %dx%d (%d percent)\n", pt.x, pt.y, (int) SDL_round(volume_slider_value * 100.0f));
                    volume_knob.x = pt.x - (volume_knob.w / 2);
                    volume_knob.x = SDL_max(volume_knob.x, volume_rect.x);
                    volume_knob.x = SDL_min(volume_knob.x, (volume_rect.x + volume_rect.w) - volume_knob.w);
                } else if (SDL_PointInRect(&pt, &balance_rect) && (e.motion.state & SDL_BUTTON_LMASK)) {  // mouse is pressed inside the "balance" "slider"?
                    const float fx = (float) (pt.x - balance_rect.x);
                    balance_slider_value = (fx / ((float) balance_rect.w));  // a value between 0.0f and 1.0f
                    //printf("SLIDING! At %dx%d (%d percent)\n", pt.x, pt.y, (int) SDL_round(balance_slider_value * 100.0f));
                    balance_knob.x = pt.x - (balance_knob.w / 2);
                    balance_knob.x = SDL_max(balance_knob.x, balance_rect.x);
                    balance_knob.x = SDL_min(balance_knob.x, (balance_rect.x + balance_rect.w) - balance_knob.w);
                }
                break;
            }
            #endif

            case SDL_DROPFILE: {
                open_new_audio_file(e.drop.file);
                SDL_free(e.drop.file);
                break;
            }
        }
    }

    return SDL_TRUE;  // keep going.
}

int main(int argc, char **argv)
{
    init_everything(argc, argv);  // will panic_and_abort on issues.

    #if 0   // !!! FIXME: do something with this.
    SDL_Rect volume_knob;
    volume_knob.y = volume_rect.y;
    volume_knob.h = volume_rect.h;
    volume_knob.w = 20;
    volume_knob.x = (volume_rect.x + volume_rect.w) - volume_knob.w;

    SDL_Rect balance_knob;
    balance_knob.y = balance_rect.y;
    balance_knob.h = balance_rect.h;
    balance_knob.w = 20;
    balance_knob.x = (balance_rect.x + (balance_rect.w / 2)) - balance_knob.w;
    #endif

    while (handle_events(&skin)) {
        feed_more_audio();
        draw_frame(renderer, &skin);
    }

    deinit_everything();

    return 0;
}

// end of sdlamp.c ...

