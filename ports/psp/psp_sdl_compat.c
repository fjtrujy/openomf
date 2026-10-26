/*
 * PSP transitional compatibility for shared UI and trace-file code.
 * The native PSP build does not link SDL2. Keyboard and clipboard functions
 * are inert because PSP input comes solely from sceCtrl; net traces and
 * logging use normal files and PSP kernel synchronization.
 *
 * Shared interfaces now use OpenOMF-owned types. Only these transitional
 * SDL-named utility implementations remain for PSP-only legacy call sites.
 */
#include "utils/platform_types.h"
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct SDL_mutex {
    SceUID semaphore;
};

SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *mutex = malloc(sizeof(*mutex));
    if(!mutex) return NULL;
    mutex->semaphore = sceKernelCreateSema("OMFLog", 0, 1, 1, NULL);
    if(mutex->semaphore < 0) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

int SDL_LockMutex(SDL_mutex *mutex) {
    return mutex && sceKernelWaitSema(mutex->semaphore, 1, NULL) >= 0 ? 0 : -1;
}

int SDL_UnlockMutex(SDL_mutex *mutex) {
    return mutex && sceKernelSignalSema(mutex->semaphore, 1) >= 0 ? 0 : -1;
}

void SDL_DestroyMutex(SDL_mutex *mutex) {
    if(mutex) {
        sceKernelDeleteSema(mutex->semaphore);
        free(mutex);
    }
}

char *SDL_GetBasePath(void) {
    char cwd[1024];
    if(!getcwd(cwd, sizeof(cwd))) return NULL;
    size_t n = strlen(cwd);
    char *path = malloc(n + 2);
    if(path) {
        memcpy(path, cwd, n);
        if(n == 0 || cwd[n - 1] != '/') path[n++] = '/';
        path[n] = 0;
    }
    return path;
}

char *SDL_GetPrefPath(const char *org, const char *app) {
    (void)org;
    (void)app;
    return SDL_GetBasePath();
}

void SDL_free(void *p) { free(p); }
const char *SDL_GetError(void) { return "Native PSP operation failed"; }

const char *SDL_GetScancodeName(SDL_Scancode code) {
    (void)code;
    return "";
}

SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    (void)name;
    return SDL_SCANCODE_UNKNOWN;
}

const char *SDL_GameControllerNameForIndex(int joystick_index) {
    (void)joystick_index;
    return NULL;
}

const Uint8 *SDL_GetKeyboardState(int *count) {
    static const Uint8 keys[SDL_NUM_SCANCODES] = {0};
    if(count) *count = SDL_NUM_SCANCODES;
    return keys;
}

SDL_bool SDL_IsTextInputActive(void) { return SDL_FALSE; }
void SDL_StartTextInput(void) {}
void SDL_StopTextInput(void) {}
SDL_bool SDL_HasClipboardText(void) { return SDL_FALSE; }
char *SDL_GetClipboardText(void) {
    char *empty = malloc(1);
    if(empty) empty[0] = 0;
    return empty;
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window) {
    (void)flags;
    (void)window;
    fprintf(stderr, "%s: %s\n", title ? title : "OpenOMF", message ? message : "");
    return 0;
}

/* Shared netplay trace code uses SDL_RWops only as an opaque FILE handle. */
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode) {
    return (SDL_RWops *)fopen(file, mode);
}

size_t SDL_RWwrite(SDL_RWops *context, const void *ptr, size_t size, size_t num) {
    return context ? fwrite(ptr, size, num, (FILE *)context) : 0;
}

int SDL_RWclose(SDL_RWops *context) {
    return context ? fclose((FILE *)context) : -1;
}
