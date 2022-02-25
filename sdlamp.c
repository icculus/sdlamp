#include "SDL.h"

static SDL_AudioDeviceID audio_device = 0;

int main(int argc, char **argv)
{
    // !!! FIXME: error checking!
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    SDL_AudioSpec wavspec;
    Uint8 *wavbuf = NULL;
    Uint32 wavlen = 0;
    if (SDL_LoadWAV("music.wav", &wavspec, &wavbuf, &wavlen) == NULL) {
        fprintf(stderr, "Uhoh, couldn't load wav file! %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
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
    // !!! FIXME: ERROR CHECKING DAMMIT

    SDL_QueueAudio(audio_device, wavbuf, wavlen);
    SDL_FreeWAV(wavbuf);

    SDL_PauseAudioDevice(audio_device, 0);

    SDL_Delay(5000);

    SDL_CloseAudioDevice(audio_device);
    SDL_Quit();

    return 0;
}

