#include <pspkernel.h>
#include <pspthreadman.h>

/*
 * SDL2main used to own the PSP process callbacks. Register the OS exit
 * callback directly so HOME -> Quit still exits to the PSP shell.
 */
static int on_exit(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize argc, void *argv) {
    (void)argc;
    (void)argv;
    const SceUID callback = sceKernelCreateCallback("OpenOMF Exit", on_exit, NULL);
    if(callback < 0) return (int)callback;
    if(sceKernelRegisterExitCallback(callback) < 0) {
        sceKernelDeleteCallback(callback);
        return -1;
    }
    for(;;) {
        sceKernelSleepThreadCB();
    }
    return 0;
}

int psp_install_exit_callback(void) {
    const SceUID thread = sceKernelCreateThread(
        "OpenOMF System", callback_thread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if(thread < 0) return (int)thread;
    const int result = sceKernelStartThread(thread, 0, NULL);
    if(result < 0) {
        sceKernelDeleteThread(thread);
        return result;
    }
    return 0;
}
