#ifndef OMF_PLATFORM_TIME_H
#define OMF_PLATFORM_TIME_H

#include <stdint.h>

/* Game time is expressed in milliseconds. Keep the desktop SDL clock and
 * use the PSP kernel's monotonic microsecond clock on native PSP builds. */
#if defined(__PSP__) && defined(PSP_NATIVE_TIMING)
#include <pspthreadman.h>

static inline uint64_t omf_time_ms(void) {
    return (uint64_t)sceKernelGetSystemTimeWide() / 1000u;
}

static inline void omf_sleep_ms(uint32_t milliseconds) {
    /* sceKernelDelayThread accepts microseconds (32 bit). Chunk long waits
     * to avoid overflow; normal game sleeps are just 1-4 milliseconds. */
    while(milliseconds > 0) {
        const uint32_t chunk = milliseconds > 1000u ? 1000u : milliseconds;
        sceKernelDelayThread(chunk * 1000u);
        milliseconds -= chunk;
    }
}

#else
#include <SDL.h>

static inline uint64_t omf_time_ms(void) {
    return SDL_GetTicks64();
}

static inline void omf_sleep_ms(uint32_t milliseconds) {
    SDL_Delay(milliseconds);
}
#endif

static inline uint32_t omf_time_ms32(void) {
    return (uint32_t)omf_time_ms();
}

#endif // OMF_PLATFORM_TIME_H
