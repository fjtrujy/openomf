#ifndef _MUSIC_H
#define _MUSIC_H

#include "audio/stream.h"

int music_play(const char *filename);
void music_stop();
int music_playing();

#endif // _MUSIC_H
