/*
 * Native PSP controls. Both the menu and gameplay use the existing ACT_*
 * controller interface; only the device polling is platform-specific.
 */
#include "controller/joystick.h"
#include "utils/allocator.h"
#include "utils/log.h"

#include <pspctrl.h>
#include <string.h>

#define PSP_JOYSTICK_NAME "PSP builtin joypad"
#define PSP_ANALOG_LOW 64
#define PSP_ANALOG_HIGH 192

static int initialized;

static void ensure_initialized(void) {
    if(initialized) return;
    const int mode = sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    const int cycle = sceCtrlSetSamplingCycle(0);
    initialized = mode >= 0 && cycle >= 0;
    if(!initialized) log_error("PSP native controls: sceCtrl initialization failed (%d, %d)", mode, cycle);
    else log_info("PSP native controls: sceCtrl digital + analog sampling initialized");
}

static bool read_pad(SceCtrlData *pad) {
    memset(pad, 0, sizeof(*pad));
    pad->Lx = 128;
    pad->Ly = 128;
    return sceCtrlPeekBufferPositive(pad, 1) > 0;
}

static int pad_action(const SceCtrlData *pad) {
    int action = ACT_NONE;
    /* Match existing SDL semantics: analog takes priority over D-pad. */
    if(pad->Lx < PSP_ANALOG_LOW) action |= ACT_LEFT;
    else if(pad->Lx > PSP_ANALOG_HIGH) action |= ACT_RIGHT;
    if(pad->Ly < PSP_ANALOG_LOW) action |= ACT_UP;
    else if(pad->Ly > PSP_ANALOG_HIGH) action |= ACT_DOWN;
    if(!(action & ACT_Mask_Dirs)) {
        if(pad->Buttons & PSP_CTRL_LEFT) action |= ACT_LEFT;
        else if(pad->Buttons & PSP_CTRL_RIGHT) action |= ACT_RIGHT;
        if(pad->Buttons & PSP_CTRL_UP) action |= ACT_UP;
        else if(pad->Buttons & PSP_CTRL_DOWN) action |= ACT_DOWN;
    }

    /* SDL PSP mapping: Cross=A (punch), Circle=B (kick), Start=escape. */
    if(pad->Buttons & PSP_CTRL_CROSS) action |= ACT_PUNCH;
    else if(pad->Buttons & PSP_CTRL_CIRCLE) action |= ACT_KICK;
    return action;
}

static int poll_gamepad(controller *ctrl, ctrl_event **events, bool menu) {
    SceCtrlData pad;
    if(!read_pad(&pad)) return 0;
    const int action = pad_action(&pad);
#ifdef PSP_NATIVE_INPUT_TRACE
    static int last_action = -1;
    static int last_escape = -1;
    const int escape = !!(pad.Buttons & PSP_CTRL_START);
    if(last_action != action || last_escape != escape) {
        log_info("PSP INPUT: %s act=0x%x start=%d buttons=0x%lx analog=(%u,%u)",
                 menu ? "menu" : "game", action, escape,
                 (unsigned long)pad.Buttons, pad.Lx, pad.Ly);
        last_action = action;
        last_escape = escape;
    }
#endif
    if(action) controller_cmd(ctrl, action, events);
    if(menu && (pad.Buttons & PSP_CTRL_START)) controller_cmd(ctrl, ACT_ESC, events);
    if(ctrl->current == 0) controller_cmd(ctrl, ACT_STOP, events);
    return 0;
}

static int native_poll(controller *ctrl, ctrl_event **events) {
    ctrl->last = ctrl->current;
    ctrl->current = ACT_NONE;
    return poll_gamepad(ctrl, events, false);
}

static void native_free(controller *ctrl) {
    omf_free(ctrl->data);
}

int joystick_create(controller *ctrl, int joystick_id) {
    ensure_initialized();
    if(!initialized || joystick_id != 0) return 0;
    ctrl->data = omf_calloc(1, sizeof(int));
    ctrl->type = CTRL_TYPE_GAMEPAD;
    ctrl->poll_fun = native_poll;
    ctrl->free_fun = native_free;
    ctrl->supports_delay = true;
    return 1;
}

int joystick_count(void) { ensure_initialized(); return initialized ? 1 : 0; }
int joystick_nth_id(int n) { ensure_initialized(); return initialized && n == 1 ? 0 : -1; }
int joystick_name_to_id(const char *name, int offset) {
    (void)name;
    ensure_initialized();
    return initialized && offset == 0 ? 0 : -1;
}
int joystick_offset(int id, const char *name) {
    (void)name;
    return id == 0 ? 0 : -1;
}

const char *joystick_device_name(int id) {
    return initialized && id == 0 ? PSP_JOYSTICK_NAME : NULL;
}

void joystick_init(void) {
    ensure_initialized();
}

void joystick_close(void) { initialized = 0; }

void joystick_menu_poll_all(controller *menu_ctrl, ctrl_event **events) {
    if(initialized) poll_gamepad(menu_ctrl, events, true);
}

/* PSP built-in controls are not hot-pluggable like desktop joysticks. */
void joystick_deviceadded(int index) { (void)index; }
void joystick_deviceremoved(int index) { (void)index; }
