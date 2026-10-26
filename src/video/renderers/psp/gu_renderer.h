#ifndef PSP_GU_RENDERER_H
#define PSP_GU_RENDERER_H

#include "video/renderers/renderer.h"

void psp_gu_renderer_set_callbacks(renderer *renderer);
#ifdef PSP_STANDALONE
void psp_gu_software_renderer_set_callbacks(renderer *renderer);
#endif

#endif
