#ifndef VIDEO_COMMON_H
#define VIDEO_COMMON_H

#include "utils/platform_types.h"

void find_resolution_for_aspect_ratio(omf_rect *view, const omf_rect *window, unsigned ratio_w, unsigned ratio_h);

#endif // VIDEO_COMMON_H
