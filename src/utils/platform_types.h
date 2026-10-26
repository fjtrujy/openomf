#ifndef OMF_PLATFORM_TYPES_H
#define OMF_PLATFORM_TYPES_H

/*
 * Common game interfaces use OpenOMF geometry and event types. Desktop
 * aliases SDL for ABI compatibility; the standalone PSP has no SDL runtime.
 */
#if defined(__PSP__) && defined(PSP_STANDALONE)
#include <stdint.h>
#include <stddef.h>

typedef struct omf_rect {
    int x, y, w, h;
} omf_rect;

typedef int omf_scancode;

/* Keyboard events cannot originate from the PSP's native sceCtrl input.
 * Keep the legacy menu callbacks linkable without pulling in SDL headers. */
enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_C = 6, SDL_SCANCODE_D = 7,
    SDL_SCANCODE_E = 8, SDL_SCANCODE_Q = 20, SDL_SCANCODE_V = 25,
    SDL_SCANCODE_W = 26, SDL_SCANCODE_X = 27, SDL_SCANCODE_Z = 29,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_BACKSPACE = 42, SDL_SCANCODE_GRAVE = 53,
    SDL_SCANCODE_HOME = 74, SDL_SCANCODE_PAGEUP = 75,
    SDL_SCANCODE_DELETE = 76, SDL_SCANCODE_END = 77,
    SDL_SCANCODE_PAGEDOWN = 78, SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82, SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_KP_2 = 90, SDL_SCANCODE_KP_4 = 92,
    SDL_SCANCODE_KP_6 = 94, SDL_SCANCODE_KP_8 = 96,
    SDL_SCANCODE_KP_0 = 98, SDL_SCANCODE_EXSEL = 164,
    SDL_SCANCODE_KP_00 = 176, SDL_SCANCODE_KP_HEXADECIMAL = 221,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_RSHIFT = 229, SDL_SCANCODE_RGUI = 231,
    SDL_NUM_SCANCODES = 512,
    SDL_KEYDOWN = 0x300, SDL_TEXTINPUT = 0x303,
    SDL_MESSAGEBOX_ERROR = 0x10,
    SDLK_ESCAPE = 27, SDLK_RETURN = 13, SDLK_e = 'e',
    SDLK_1 = '1', SDLK_6 = '6', SDLK_9 = '9',
    KMOD_CTRL = 0xc0
};

typedef uint8_t Uint8;
typedef uint32_t Uint32;
typedef int SDL_bool;
#define SDL_FALSE 0
#define SDL_TRUE 1
typedef omf_scancode SDL_Scancode;
typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_RWops SDL_RWops;
typedef struct SDL_Window SDL_Window;

SDL_mutex *SDL_CreateMutex(void);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);
void SDL_DestroyMutex(SDL_mutex *mutex);
char *SDL_GetBasePath(void);
char *SDL_GetPrefPath(const char *org, const char *app);
void SDL_free(void *ptr);
const char *SDL_GetError(void);
const char *SDL_GetScancodeName(omf_scancode code);
omf_scancode SDL_GetScancodeFromName(const char *name);
const char *SDL_GameControllerNameForIndex(int index);
const Uint8 *SDL_GetKeyboardState(int *count);
SDL_bool SDL_IsTextInputActive(void);
void SDL_StartTextInput(void);
void SDL_StopTextInput(void);
SDL_bool SDL_HasClipboardText(void);
char *SDL_GetClipboardText(void);
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode);
size_t SDL_RWwrite(SDL_RWops *context, const void *ptr, size_t size, size_t num);
int SDL_RWclose(SDL_RWops *context);

typedef struct omf_event {
    uint32_t type;
    struct {
        struct {
            int scancode;
            int sym;
            uint16_t mod;
        } keysym;
        uint8_t repeat;
    } key;
    struct {
        char text[32];
    } text;
    struct {
        uint8_t event;
    } window;
    struct {
        int x, y;
    } motion;
} omf_event;

#else
#include <SDL.h>
typedef SDL_Rect omf_rect;
typedef SDL_Event omf_event;
typedef SDL_Scancode omf_scancode;
#endif

#endif
