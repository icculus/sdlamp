#include <stdio.h>
#include "SDL.h"

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
    WinAmpSkinButton buttons[WASBTN_TOTAL];
    WinAmpSkinSlider sliders[WASSLD_TOTAL];
    WinAmpSkinButton *pressed;
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


typedef struct ZipEntry
{
    char *fname;
    Uint32 compression_type;
    Uint32 compressed_size;
    Uint32 uncompressed_size;
    Uint32 filepos;
} ZipEntry;

typedef struct ZipArchive
{
    SDL_RWops *rw;
    Uint32 num_entries;
    ZipEntry *entries;
} ZipArchive;

static void unload_zip_archive(ZipArchive *zip)
{
    if (zip) {
        if (zip->rw) { SDL_RWclose(zip->rw); }
        SDL_free(zip->entries);
        SDL_free(zip);
    }
}

static ZipArchive *load_zip_archive(const char *fname)
{
    SDL_RWops *rw = SDL_RWFromFile(fname, "rb");
    if (!rw) {
        return NULL;
    }

    ZipArchive *retval = (ZipArchive *) SDL_calloc(1, sizeof (ZipArchive));
    if (retval == NULL) {
        SDL_RWclose(rw);
        return NULL;
    }

    retval->rw = rw;

    Uint32 ui32;
    Uint16 ui16;

    while ( SDL_RWread(rw, &ui32, sizeof (ui32), 1) == 1 ) {
        ZipEntry entry;
        Uint16 fnamelen;
        Uint16 extralen;
        SDL_zero(entry);

        ui32 = SDL_SwapLE32(ui32);
        if (ui32 != 0x04034b50) {   // magic number for local file header.
            break;
        }

        SDL_RWread(rw, &ui16, sizeof (ui16), 1);  // version needed to extract
        SDL_RWread(rw, &ui16, sizeof (ui16), 1);  // general purpose bit flag
        SDL_RWread(rw, &ui16, sizeof (ui16), 1);  // compression method
        entry.compression_type = SDL_SwapLE16(ui16);
        SDL_RWread(rw, &ui16, sizeof (ui16), 1);  // last mod file time
        SDL_RWread(rw, &ui16, sizeof (ui16), 1);  // last mod file date
        SDL_RWread(rw, &ui32, sizeof (ui32), 1);  // crc-32
        SDL_RWread(rw, &ui32, sizeof (ui32), 1);  // compressed size
        entry.compressed_size = SDL_SwapLE32(ui32);
        SDL_RWread(rw, &ui32, sizeof (ui32), 1);  // uncompressed size
        entry.uncompressed_size = SDL_SwapLE32(ui32);
        SDL_RWread(rw, &ui16, sizeof (ui16), 1);  // file name length
        fnamelen = SDL_SwapLE16(ui16);
        SDL_RWread(rw, &ui16, sizeof (ui16), 1);  // extra field length
        extralen = SDL_SwapLE16(ui16);

        entry.fname = (char *) SDL_malloc(fnamelen + 1);
        SDL_RWread(rw, entry.fname, fnamelen, 1);
        entry.fname[fnamelen] = '\0';
        SDL_RWseek(rw, extralen, RW_SEEK_CUR);

        entry.filepos = SDL_RWtell(rw);

        SDL_RWseek(rw, entry.compressed_size, RW_SEEK_CUR);  /* ready for next local file header */

        void *ptr = SDL_realloc(retval->entries, sizeof (ZipEntry) * (retval->num_entries + 1));
        retval->entries = ptr;
        SDL_memcpy(&retval->entries[retval->num_entries], &entry, sizeof (ZipEntry));
        retval->num_entries++;
    }

    return retval;
}


static WinAmpSkin skin;

static Uint8 *wavbuf = NULL;
static Uint32 wavlen = 0;
static SDL_AudioSpec wavspec;
static SDL_AudioStream *stream = NULL;

static void SDLCALL feed_audio_device_callback(void *userdata, Uint8 *output_stream, int len)
{
    SDL_AudioStream *input_stream = (SDL_AudioStream *) SDL_AtomicGetPtr((void **) &stream);

    if (input_stream == NULL) {  // nothing playing, just write silence and bail.
        SDL_memset(output_stream, '\0', len);
        return;
    }

    const int num_converted_bytes = SDL_AudioStreamGet(input_stream, output_stream, len);
    if (num_converted_bytes > 0) {
        const float volume = skin.sliders[WASSLD_VOLUME].value;
        const float balance = skin.sliders[WASSLD_BALANCE].value;
        const int num_samples = (num_converted_bytes / sizeof (float));
        float *samples = (float *) output_stream;

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
    }

    len -= num_converted_bytes;  // now has number of bytes left after feeding the device.
    output_stream += num_converted_bytes;
    if (len > 0) {
        SDL_memset(output_stream, '\0', len);
    }
}

static void stop_audio(void)
{
    SDL_LockAudioDevice(audio_device);
    if (stream) {
        SDL_FreeAudioStream(stream);
        SDL_AtomicSetPtr((void **) &stream, NULL);
    }
    SDL_UnlockAudioDevice(audio_device);

    if (wavbuf) {
        SDL_FreeWAV(wavbuf);
    }

    wavbuf = NULL;
    wavlen = 0;
}

static SDL_bool open_new_audio_file(const char *fname)
{
    SDL_AudioStream *tmpstream = stream;

    // make sure the audio callback can't touch `stream` while we're freeing it.
    SDL_LockAudioDevice(audio_device);
    SDL_AtomicSetPtr((void **) &stream, NULL);
    SDL_UnlockAudioDevice(audio_device);

    SDL_FreeAudioStream(tmpstream);
    SDL_FreeWAV(wavbuf);
    wavbuf = NULL;
    wavlen = 0;

    if (SDL_LoadWAV(fname, &wavspec, &wavbuf, &wavlen) == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load wav file!", SDL_GetError(), window);
        goto failed;
    }

    tmpstream = SDL_NewAudioStream(wavspec.format, wavspec.channels, wavspec.freq, AUDIO_F32, 2, 48000);
    if (!tmpstream) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't create audio stream!", SDL_GetError(), window);
        goto failed;
    }

    if (SDL_AudioStreamPut(tmpstream, wavbuf, wavlen) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Audio stream put failed", SDL_GetError(), window);
        goto failed;
    }

    if (SDL_AudioStreamFlush(tmpstream) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Audio stream flush failed", SDL_GetError(), window);
        goto failed;
    }

    // make new `stream` available to the audio callback thread.
    SDL_LockAudioDevice(audio_device);
    SDL_AtomicSetPtr((void **) &stream, tmpstream);
    SDL_UnlockAudioDevice(audio_device);

    return SDL_TRUE;

failed:
    stop_audio();
    return SDL_FALSE;
}

// !!! FIXME: maybe a better name.

static void previous_clicked(void)
{
    SDL_AudioStreamClear(stream);
    if (SDL_AudioStreamPut(stream, wavbuf, wavlen) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Audio stream put failed", SDL_GetError(), window);
        stop_audio();
    } else if (SDL_AudioStreamFlush(stream) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Audio stream flush failed", SDL_GetError(), window);
        stop_audio();
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

static SDL_RWops *openrw(ZipArchive *zip, const char *dirname, const char *fname)
{
    if (zip) {
        for (Uint32 i = 0; i < zip->num_entries; i++) {
            const ZipEntry *entry = &zip->entries[i];
            if (SDL_strcasecmp(entry->fname, fname) == 0) {
                SDL_RWseek(zip->rw, entry->filepos, RW_SEEK_SET);
                void *data = SDL_malloc(entry->compressed_size);
                SDL_RWread(zip->rw, data, entry->compressed_size, 1);
                SDL_assert(entry->compression_type == 0);  // if (entry->compression_type != 0) { push data through zlib };
                return SDL_RWFromConstMem(data, entry->uncompressed_size);
            }
        }
        return NULL;
    }

    // we don't have a zip file, read from disk
    const size_t fullpathlen = SDL_strlen(dirname) + SDL_strlen(fname) + 2;
    char *fullpath = (char *) SDL_malloc(fullpathlen);
    SDL_snprintf(fullpath, fullpathlen, "%s/%s", dirname, fname);  // !!! FIXME: filename case is a problem on Unix.
    SDL_RWops *retval = SDL_RWFromFile(fullpath, "rb");
    SDL_free(fullpath);
    return retval;
}

static void load_skin(WinAmpSkin *skin, const char *fname)
{
    if (skin->tex_main) { SDL_DestroyTexture(skin->tex_main); }
    if (skin->tex_cbuttons) { SDL_DestroyTexture(skin->tex_cbuttons); };
    if (skin->tex_volume) { SDL_DestroyTexture(skin->tex_volume); };
    if (skin->tex_balance) { SDL_DestroyTexture(skin->tex_balance); };

    SDL_zerop(skin);

    ZipArchive *zip = load_zip_archive(fname);

    skin->tex_main = load_texture(openrw(zip, fname, "main.bmp"));
    skin->tex_cbuttons = load_texture(openrw(zip, fname, "cbuttons.bmp"));
    skin->tex_volume = load_texture(openrw(zip, fname, "volume.bmp"));
    skin->tex_balance = load_texture(openrw(zip, fname, "balance.bmp"));

    unload_zip_archive(zip);

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

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 275, 116, 0);
    if (!window) {
        panic_and_abort("SDL_CreateWindow failed", SDL_GetError());
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        panic_and_abort("SDL_CreateRenderer failed", SDL_GetError());
    }

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
    int i;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_RenderCopy(renderer, skin->tex_main, NULL, NULL);

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
    // !!! FIXME: free_skin()
    SDL_FreeWAV(wavbuf);
    SDL_CloseAudioDevice(audio_device);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
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

