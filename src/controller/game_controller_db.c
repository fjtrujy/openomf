#include <SDL_gamecontroller.h>

#include "resources/resource_files.h"
#include "utils/log.h"
#include "utils/path.h"
#include "utils/str.h"

void joystick_load_external_mappings(void) {
#ifdef __PSP__
    // SDL's PSP joystick backend exposes physical buttons but has no default
    // SDL_GameController mapping. Register one before scanning controllers.
    // SDL_sysjoystick.c's order: triangle, circle, cross, square, L, R,
    // down, left, up, right, select, start, home, hold.
    if(SDL_NumJoysticks() > 0) {
        char guid[33];
        char mapping[256];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(0), guid, sizeof(guid));
        snprintf(mapping, sizeof(mapping),
                 "%s,PSP builtin joypad,a:b2,b:b1,x:b3,y:b0,"
                 "leftshoulder:b4,rightshoulder:b5,dpdown:b6,dpleft:b7,"
                 "dpup:b8,dpright:b9,back:b10,start:b11,leftx:a0,lefty:a1,",
                 guid);
        if(SDL_GameControllerAddMapping(mapping) < 0) {
            log_error("Unable to register PSP controller mapping: %s", SDL_GetError());
        } else {
            log_info("Registered PSP built-in controller mapping");
        }
    }
#else
    int loaded;
    path db_filename = get_game_controller_db_filename();
    if((loaded = SDL_GameControllerAddMappingsFromFile(path_c(&db_filename))) > 0) {
        log_info("Loaded %d external game controller mappings from %s", loaded, path_c(&db_filename));
        log_info("We have %d known game controller mappings", SDL_GameControllerNumMappings());
    } else {
        log_info("No external game controller mappings file found from '%s' (%s); skipping ...", path_c(&db_filename),
                 SDL_GetError());
    }
#endif
}
