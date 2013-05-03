#include "game/har.h"
#include "utils/log.h"
#include "video/texture.h"
#include "video/video.h"
#include "audio/sound.h"
#include <stdlib.h>

int har_load(har *h, sd_palette *pal, char *soundtable, const char *file) {
    h->x = 60;
    h->y = 180;
    h->tick = 0; // TEMPORARY
    h->af = sd_af_create();
    
    // Load AF
    if(sd_af_load(h->af, file)) {
        return 1;
    }
    
    // Handle animations
    animation *ani;
    sd_move *move;
    array_create(&h->animations);
    for(unsigned int i = 0; i < 70; i++) {
        move = h->af->moves[i];
        if(move != NULL) {
            // Create animation + textures, etc.
            ani = malloc(sizeof(animation));
            animation_create(ani, move->animation, pal, -1, soundtable);
            array_set(&h->animations, i, ani);
            DEBUG("Loading animation %d", i);
        }
    }
    
    // Start player with animation 11
    h->player.x = h->x;
    h->player.y = h->y;
    animationplayer_create(&h->player, 11, array_get(&h->animations, 11));
    animationplayer_set_repeat(&h->player, 1);
    DEBUG("Har %s loaded!", file);
    return 0;
}

void har_free(har *h) {
    // Free AF
    sd_af_delete(h->af);
    
    // Free animations
    animation *ani = 0;
    iterator it;
    array_iter_begin(&h->animations, &it);
    while((ani = iter_next(&it)) != 0) {
        animation_free(ani);
        free(ani);
    }
    array_free(&h->animations);
    
    // Unload player
    animationplayer_free(&h->player);
}

void har_tick(har *har) {
    har->tick++;
    if(har->tick > 8) {
        animationplayer_run(&har->player);
        har->tick = 0;
    }
}

void har_render(har *har) {
    animationplayer_render(&har->player);
}

void har_act(har *har, int act_type) {

}
