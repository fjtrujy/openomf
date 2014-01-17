#ifndef _SCORE_H
#define _SCORE_H

#include <string.h>
#include <stdlib.h>
#include "utils/list.h"
#include "video/surface.h"
#include "game/protos/object.h"
#include "game/text/text.h"

enum {
    SCORE_EV_PUNCH = 0,
    SCORE_EV_KICK,
    SCORE_EV_ROUNDHOUSE,
    SCORE_EV_PERFECTROUND,
};

typedef struct chr_score_t {
    int score;
    int x,y;
    int direction;
    float multiplier;
    list texts;

    int consecutive_hits;
    int consecutive_hit_score;
    int combo_hits;
    int combo_hit_score;
} chr_score;

void chr_score_create(chr_score *score, float multiplier);
void chr_score_set_pos(chr_score *score, int x, int y, int direction);
void chr_score_free(chr_score *score);
void chr_score_tick(chr_score *score);
void chr_score_render(chr_score *score);

void chr_score_hit(chr_score *score, int points);
void chr_score_victory(chr_score *score, int health);
int chr_score_end_combo(chr_score *score, vec2i pos);
int chr_score_interrupt(chr_score *score, vec2i pos);

void chr_score_serialize(chr_score *score, serial *ser);
void chr_score_unserialize(chr_score *score, serial *ser);
#endif // _SCORE_H
