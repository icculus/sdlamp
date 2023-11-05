#include <stdio.h>
#include "SDL.h"
#include "SDL_sound.h"
#include "physfs.h"
#include "physfs/extras/physfsrwops.h"
#include "physfs/extras/ignorecase.h"

typedef void (*ClickFn)(void);

typedef struct
{
    SDL_Texture *texture;  // YOU DO NOT OWN THIS POINTER, DON'T FREE IT.
    SDL_Rect srcrect_unpressed;
    SDL_Rect srcrect_pressed;
    SDL_Rect dstrect;
    ClickFn clickfn;
} WinAmpSkinButton;

typedef enum
{
    WASBTN_SYSTEM=0,
    WASBTN_MINIMIZE,
    WASBTN_CLOSE,
    WASBTN_PREV,
    WASBTN_PLAY,
    WASBTN_PAUSE,
    WASBTN_STOP,
    WASBTN_NEXT,
    WASBTN_EJECT,
    WASBTN_TOTAL
} WinAmpSkinButtonId;

typedef struct
{
    SDL_Texture *texture;  // YOU DO NOT OWN THIS POINTER, DON'T FREE IT.
    WinAmpSkinButton knob;
    int num_frames;
    int frame_x_offset;
    int frame_y_offset;
    int frame_width;
    int frame_height;
    SDL_Rect dstrect;
    float value;
} WinAmpSkinSlider;

typedef enum
{
    WASSLD_VOLUME=0,
    WASSLD_BALANCE,
    WASSLD_TOTAL
} WinAmpSkinSliderId;

typedef struct
{
    SDL_Texture *tex_main;
    SDL_Texture *tex_cbuttons;
    SDL_Texture *tex_volume;
    SDL_Texture *tex_balance;
    SDL_Texture *tex_titlebar;
    WinAmpSkinButton buttons[WASBTN_TOTAL];
    WinAmpSkinSlider sliders[WASSLD_TOTAL];
    WinAmpSkinButton *pressed;
} WinAmpSkin;

static WinAmpSkin skin;
static SDL_AudioDeviceID audio_device = 0;
static Sound_AudioInfo audio_device_spec;
static Sound_Sample *current_sample = NULL;
static Uint32 sample_available = 0;
static Uint32 sample_position = 0;
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

static const char *physfs_errstr(void)
{
    return PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
}

static SDL_RWops *openrw(const char *_fname)
{
    char *fname = SDL_strdup(_fname);
    if (!fname) {
        SDL_OutOfMemory();
        return NULL;
    }

    PHYSFSEXT_locateCorrectCase(fname);
    SDL_RWops *retval = PHYSFSRWOPS_openRead(fname);
    SDL_free(fname);
    return retval;
}

static void SDLCALL feed_audio_device_callback(void *userdata, Uint8 *output_stream, int len)
{
    Sound_Sample *sample = (Sound_Sample *) SDL_AtomicGetPtr((void **) &current_sample);

    if (sample == NULL) {  // nothing playing, just write silence and bail.
        SDL_memset(output_stream, '\0', len);
        return;
    }

    const float volume = skin.sliders[WASSLD_VOLUME].value;
    const float balance = skin.sliders[WASSLD_BALANCE].value;

    while (len > 0) {
        if (sample_available == 0) {
            const Uint32 br = Sound_Decode(sample);
            if (br == 0) {
                // !!! FIXME: rework stop_audio and use it here.
                Sound_FreeSample(current_sample);
                SDL_AtomicSetPtr((void **) &current_sample, NULL);
                sample_available = 0;
                sample_position = 0;
                SDL_memset(output_stream, '\0', len);  // write silence and bail.
                return;
            }

            sample_available = br;
            sample_position = 0;
        }

        float *samples = (float *) output_stream;
        const Uint32 cpy = SDL_min(sample_available, (Uint32) len);
        SDL_assert(cpy > 0);
        SDL_memcpy(samples, ((const Uint8 *) sample->buffer) + sample_position, (size_t) cpy);

        const int num_samples = (int) (cpy / sizeof (float));
        SDL_assert((num_samples % 2) == 0);  // this should always be stereo data (at least for now).

        // change the volume of the audio we're playing.
        if (volume != 1.0f) {
            for (int i = 0; i < num_samples; i++) {
                samples[i] *= volume;
            }
        }

        // first sample is left, second is right.
        // change the balance of the audio we're playing.
        if (balance > 0.5f) {
            for (int i = 0; i < num_samples; i += 2) {
                samples[i] *= 1.0f - balance;
            }
        } else if (balance < 0.5f) {
            for (int i = 0; i < num_samples; i += 2) {
                samples[i+1] *= balance;
            }
        }

        len -= cpy;
        output_stream += cpy;
        sample_available -= cpy;
        sample_position += cpy;
    }
}

static void stop_audio(void)
{
    Sound_Sample *sample = current_sample;
    // make sure the audio callback can't touch `sample` while we're freeing it.
    if (sample) {
        SDL_LockAudioDevice(audio_device);
        SDL_AtomicSetPtr((void **) &current_sample, NULL);
        SDL_UnlockAudioDevice(audio_device);
        Sound_FreeSample(sample);
    }

    sample_available = 0;
    sample_position = 0;
}

static SDL_bool open_new_audio_file(const char *fname)
{
    stop_audio();

    SDL_assert(current_sample == NULL);  // stop_audio should have set this to NULL.

    Sound_Sample *sample = Sound_NewSampleFromFile(fname, &audio_device_spec, 64 * 1024);
    if (!sample) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load audio file!", Sound_GetError(), window);
        return SDL_FALSE;
    }

    // make new `sample` available to the audio callback thread.
    SDL_LockAudioDevice(audio_device);
    SDL_AtomicSetPtr((void **) &current_sample, sample);
    SDL_UnlockAudioDevice(audio_device);

    return SDL_TRUE;
}

static SDL_HitTestResult SDLCALL hit_test_callback(SDL_Window *win, const SDL_Point *area, void *data)
{
    WinAmpSkinButtonId i;

    if (area->y >= 14) {
        return SDL_HITTEST_NORMAL;  /* definitely not in the titlebar. */
    }

    /* we are in the titlebar, make sure we're not in a button. */
    for (i = WASBTN_SYSTEM; i <= WASBTN_CLOSE; i++) {
        if (SDL_PointInRect(area, &skin.buttons[i].dstrect)) {
            return SDL_HITTEST_NORMAL;  /* we're in a button, don't drag from the buttons! */
        }
    }

    return SDL_HITTEST_DRAGGABLE;
}



// !!! FIXME: maybe a better name.

static void minimize_clicked(void)
{
    SDL_MinimizeWindow(window);
}

static void close_clicked(void)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

static void previous_clicked(void)
{
    int rc = 1;
    SDL_LockAudioDevice(audio_device);
    if (current_sample) {
        rc = Sound_Rewind(current_sample);
    }
    sample_available = 0;
    sample_position = 0;
    SDL_UnlockAudioDevice(audio_device);
    if (!rc) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't rewind audio file!", Sound_GetError(), window);
    }
}

static SDL_bool paused = SDL_TRUE;  // !!! FIXME: move this later.

static void pause_clicked(void)
{
    paused = paused ? SDL_FALSE : SDL_TRUE;
    SDL_PauseAudioDevice(audio_device, paused);
}

static void stop_clicked(void)
{
    stop_audio();
}

static SDL_Texture *load_texture(SDL_RWops *rw)
{
    if (rw == NULL) {
        return NULL;
    }

    SDL_Surface *surface = SDL_LoadBMP_RW(rw, 1);
    if (!surface) {
        return NULL;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;  // MAY BE NULL.
}


static SDL_INLINE void init_skin_button(WinAmpSkinButton *btn, SDL_Texture *tex, ClickFn clickfn,
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
    btn->clickfn = clickfn;
}

static SDL_INLINE void init_skin_slider(WinAmpSkinSlider *slider, SDL_Texture *tex,
                                        const int w, const int h,
                                        const int dx, const int dy,
                                        const int knobw, const int knobh,
                                        const int sxu, const int syu,
                                        const int sxp, const int syp,
                                        const int num_frames,
                                        const int frame_x_offset,
                                        const int frame_y_offset,
                                        const int frame_width, const int frame_height,
                                        const float initial_value)
{
    init_skin_button(&slider->knob, tex, NULL, knobw, knobh, dx, dy, sxu, syu, sxp, syp);
    slider->texture = tex;
    slider->num_frames = num_frames;
    slider->frame_x_offset = frame_x_offset;
    slider->frame_y_offset = frame_y_offset;
    slider->frame_width = frame_width;
    slider->frame_height = frame_height;
    slider->dstrect.x = dx;
    slider->dstrect.y = dy;
    slider->dstrect.w = w;
    slider->dstrect.h = h;
    slider->value = initial_value;

    SDL_assert(initial_value >= 0.0f);
    SDL_assert(initial_value <= 1.0f);
    const int knobx = dx + (int) ( ( ((((float) w) * initial_value) - (((float) knobw) / 2.0f)) + 0.5f ) );
    slider->knob.dstrect.x = SDL_clamp(knobx, dx, ((dx + w) - knobw));
}

static void free_skin(WinAmpSkin *skin)
{
    if (skin->tex_main) { SDL_DestroyTexture(skin->tex_main); }
    if (skin->tex_cbuttons) { SDL_DestroyTexture(skin->tex_cbuttons); };
    if (skin->tex_volume) { SDL_DestroyTexture(skin->tex_volume); };
    if (skin->tex_balance) { SDL_DestroyTexture(skin->tex_balance); };
    if (skin->tex_titlebar) { SDL_DestroyTexture(skin->tex_titlebar); };

    SDL_zerop(skin);
}

static void load_skin(WinAmpSkin *skin, const char *fname)
{
    free_skin(skin);

    if (!PHYSFS_mount(fname, NULL, 1)) {
        return;
    }

    skin->tex_main = load_texture(openrw("main.bmp"));
    skin->tex_cbuttons = load_texture(openrw("cbuttons.bmp"));
    skin->tex_volume = load_texture(openrw("volume.bmp"));
    skin->tex_balance = load_texture(openrw("balance.bmp"));
    skin->tex_titlebar = load_texture(openrw("titlebar.bmp"));

    PHYSFS_unmount(fname);

    init_skin_button(&skin->buttons[WASBTN_SYSTEM], skin->tex_titlebar, NULL, 9, 9, 6, 3, 0, 0, 0, 9);
    init_skin_button(&skin->buttons[WASBTN_MINIMIZE], skin->tex_titlebar, minimize_clicked, 9, 9, 244, 3, 9, 0, 9, 9);
    init_skin_button(&skin->buttons[WASBTN_CLOSE], skin->tex_titlebar, close_clicked, 9, 9, 264, 3, 18, 0, 18, 9);
    init_skin_button(&skin->buttons[WASBTN_PREV], skin->tex_cbuttons, previous_clicked, 23, 18, 16, 88, 0, 0, 0, 18);
    init_skin_button(&skin->buttons[WASBTN_PLAY], skin->tex_cbuttons, NULL, 23, 18, 39, 88, 23, 0, 23, 18);
    init_skin_button(&skin->buttons[WASBTN_PAUSE], skin->tex_cbuttons, pause_clicked, 23, 18, 62, 88, 46, 0, 46, 18);
    init_skin_button(&skin->buttons[WASBTN_STOP], skin->tex_cbuttons, stop_clicked, 23, 18, 85, 88, 69, 0, 69, 18);
    init_skin_button(&skin->buttons[WASBTN_NEXT], skin->tex_cbuttons, NULL, 22, 18, 108, 88, 92, 0, 92, 18);
    init_skin_button(&skin->buttons[WASBTN_EJECT], skin->tex_cbuttons, NULL, 22, 16, 136, 89, 114, 0, 114, 16);

    init_skin_slider(&skin->sliders[WASSLD_VOLUME], skin->tex_volume, 68, 13, 107, 57, 14, 11, 15, 422, 0, 422, 28, 0, 0, 68, 15, 1.0f);
    init_skin_slider(&skin->sliders[WASSLD_BALANCE], skin->tex_balance, 38, 13, 177, 57, 14, 11, 15, 422, 0, 422, 28, 9, 0, 47, 15, 0.5f);
}

static void init_everything(int argc, char **argv)
{
    SDL_AudioSpec desired;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1) {
        panic_and_abort("SDL_Init failed", SDL_GetError());
    }

    if (!PHYSFS_init(argv[0])) {
        panic_and_abort("PHYSFS_init failed", physfs_errstr());
    }

    if (!Sound_Init()) {
        panic_and_abort("Sound_Init failed", Sound_GetError());
    }

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 275, 116, SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS);
    if (!window) {
        panic_and_abort("SDL_CreateWindow failed", SDL_GetError());
    }

    SDL_SetWindowHitTest(window, hit_test_callback, NULL);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        panic_and_abort("SDL_CreateRenderer failed", SDL_GetError());
    }

    SDL_ShowWindow(window);

    load_skin(&skin, "classic.wsz");

    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_F32;
    desired.channels = 2;
    desired.samples = 4096;
    desired.callback = feed_audio_device_callback;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (audio_device == 0) {
        panic_and_abort("Couldn't audio device!", SDL_GetError());
    }

    SDL_zero(audio_device_spec);
    audio_device_spec.format = desired.format;
    audio_device_spec.channels = desired.channels;
    audio_device_spec.rate = desired.freq;

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);  // tell SDL we want this event that is disabled by default.

    open_new_audio_file("music.wav");
}

static void draw_button(SDL_Renderer *renderer, WinAmpSkinButton *btn)
{
    const SDL_bool pressed = (skin.pressed == btn);
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

static void draw_slider(SDL_Renderer *renderer, WinAmpSkinSlider *slider)
{
    SDL_assert(slider->value >= 0.0f);
    SDL_assert(slider->value <= 1.0f);

    if (slider->texture == NULL) {
        const int color = (int) (255.0f * slider->value);
        SDL_SetRenderDrawColor(renderer, color, color, color, 255);
        SDL_RenderFillRect(renderer, &slider->dstrect);
    } else {
        int frameidx = (int) (((float) slider->num_frames) * slider->value);
        frameidx = SDL_clamp(frameidx, 0, slider->num_frames - 1);
        const int srcy = slider->frame_y_offset + (slider->frame_height * frameidx);
        const SDL_Rect srcrect = { slider->frame_x_offset, srcy, slider->dstrect.w, slider->dstrect.h };
        SDL_RenderCopy(renderer, slider->texture, &srcrect, &slider->dstrect);
    }
    draw_button(renderer, &slider->knob);
}

static void draw_frame(SDL_Renderer *renderer, WinAmpSkin *skin)
{
    SDL_Rect src_rect, dst_rect;
    int i;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_RenderCopy(renderer, skin->tex_main, NULL, NULL);

    src_rect.x = 27;
    src_rect.y = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) ? 0 : 15;
    src_rect.w = 275;
    src_rect.h = 14;
    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.w = 275;
    dst_rect.h = 14;
    SDL_RenderCopy(renderer, skin->tex_titlebar, &src_rect, &dst_rect);

    for (i = 0; i < SDL_arraysize(skin->buttons); i++) {
        draw_button(renderer, &skin->buttons[i]);
    }

    for (i = 0; i < SDL_arraysize(skin->sliders); i++) {
        draw_slider(renderer, &skin->sliders[i]);
    }

    SDL_RenderPresent(renderer);
}

static void deinit_everything(void)
{
    SDL_CloseAudioDevice(audio_device);
    if (current_sample) {
        Sound_FreeSample(current_sample);
        current_sample = NULL;
    }

    free_skin(&skin);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    PHYSFS_deinit();
    Sound_Quit();
    SDL_Quit();
}

static void handle_slider_motion(WinAmpSkinSlider *slider, const SDL_Point *pt)
{
    if (skin.pressed == &slider->knob) {
        const float new_value = ((float) (pt->x - slider->dstrect.x)) / ((float) slider->dstrect.w);  // between 0.0f and 1.0f
        const int new_knob_x = pt->x - (slider->knob.dstrect.w / 2);
        const int xnear = slider->dstrect.x;
        const int xfar = (slider->dstrect.x + slider->dstrect.w) - slider->knob.dstrect.w;
        slider->knob.dstrect.x = SDL_clamp(new_knob_x, xnear, xfar);

        // make sure the mixer thread isn't running when this value changes.
        SDL_LockAudioDevice(audio_device);
        slider->value = SDL_clamp(new_value, 0.0f, 1.0f);
        SDL_UnlockAudioDevice(audio_device);
    }
}

static SDL_bool handle_events(WinAmpSkin *skin)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                return SDL_FALSE;  // don't keep going.

            case SDL_MOUSEBUTTONDOWN: {
                const SDL_Point pt = { e.button.x, e.button.y };

                if (e.button.button != SDL_BUTTON_LEFT) {
                    break;
                }

                if (!skin->pressed) {
                    for (int i = 0; i < SDL_arraysize(skin->buttons); i++) {
                        WinAmpSkinButton *btn = &skin->buttons[i];
                        if (SDL_PointInRect(&pt, &btn->dstrect)) {
                            skin->pressed = btn;
                            break;
                        }
                    }
                }

                if (!skin->pressed) {
                    for (int i = 0; i < SDL_arraysize(skin->sliders); i++) {
                        WinAmpSkinSlider *slider = &skin->sliders[i];
                        if (SDL_PointInRect(&pt, &slider->dstrect)) {
                            skin->pressed = &slider->knob;
                            break;
                        }
                    }
                }

                if (skin->pressed) {
                    SDL_CaptureMouse(SDL_TRUE);
                }

                break;
            }

            case SDL_MOUSEBUTTONUP: {
                if (e.button.button != SDL_BUTTON_LEFT) {
                    break;
                }

                if (skin->pressed) {
                    SDL_CaptureMouse(SDL_FALSE);
                    if (skin->pressed->clickfn) {
                        const SDL_Point pt = { e.button.x, e.button.y };
                        if (SDL_PointInRect(&pt, &skin->pressed->dstrect)) {
                            skin->pressed->clickfn();
                        }
                    }
                    skin->pressed = NULL;
                }
                break;
            }

            case SDL_MOUSEMOTION: {
                const SDL_Point pt = { e.motion.x, e.motion.y };
                for (int i = 0; i < SDL_arraysize(skin->sliders); i++) {
                    handle_slider_motion(&skin->sliders[i], &pt);
                }
                break;
            }

            case SDL_DROPFILE: {
                const char *ptr = SDL_strrchr(e.drop.file, '.');
                if (ptr && ((SDL_strcasecmp(ptr, ".wsz") == 0) || (SDL_strcasecmp(ptr, ".zip") == 0))) {
                    load_skin(skin, e.drop.file);
                } else {
                    open_new_audio_file(e.drop.file);
                }
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

    while (handle_events(&skin)) {
        draw_frame(renderer, &skin);
    }

    deinit_everything();

    return 0;
}

// end of sdlamp.c ...

