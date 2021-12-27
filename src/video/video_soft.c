#include <stdlib.h>
#include "video/video_soft.h"
#include "utils/allocator.h"
#include "utils/log.h"

/*
* This is a software renderer for the special cases where the hardware renderer
* can't do something. The idea is that scene can switch renderer on-the-fly as
* required. In the future, we might want to expand this to work along, so that
* playing the game fully software rendered would be possible.
*
* Note that currently Z-index handling is not anywhere near perfect. RGBA surfaces are
* handled as one layer while sprite surfaces are blitted on another layer. These are then
* blitted together and dumped on the screen. This creates all kinds of interesting trouble ...
*
* TODO: Rewrite and rethink
*/

typedef struct soft_renderer_t {
    char *tmp_normal;
    char *tmp_scaling;
    surface lower;
    SDL_Surface *higher;
} soft_renderer;

SDL_Surface* surface_from_pixels(char *pixels, int w, int h) {
    return SDL_CreateRGBSurfaceFrom(pixels,
                                    w, h,
                                    32,
                                    w*4,
                                    0x000000FF,
                                    0x0000FF00,
                                    0x00FF0000,
                                    0xFF000000);
}


void soft_render_close(video_state *state) {
    soft_renderer *sr = state->userdata;
    SDL_FreeSurface(sr->higher);
    surface_free(&sr->lower);
    omf_free(sr->tmp_normal);
    omf_free(sr->tmp_scaling);
    omf_free(sr);
}

void soft_render_reinit(video_state *state) {

}

void soft_render_prepare(video_state *state) {
    soft_renderer *sr = state->userdata;
    SDL_FillRect(sr->higher, NULL, SDL_MapRGBA(sr->higher->format, 0, 0, 0, 0));
}

void soft_render_finish(video_state *state) {
    soft_renderer *sr = state->userdata;
    SDL_Texture *tex;
    SDL_Surface *low_s;

    // Blit lower
    surface_to_rgba(&sr->lower, sr->tmp_normal, state->cur_palette, NULL, 0, 0);

    // Scale if necessary
    if(state->scale_factor > 1) {
        int nw = 320 * state->scale_factor;
        int nh = 200 * state->scale_factor;
        scaler_scale(&state->scaler, sr->tmp_normal, sr->tmp_scaling, 320, 200, state->scale_factor);
        low_s = surface_from_pixels(sr->tmp_scaling, nw, nh);
    } else {
        low_s = surface_from_pixels(sr->tmp_normal, 320, 200);
    }

    // Make a texture
    tex = SDL_CreateTextureFromSurface(state->renderer, low_s);
    SDL_RenderCopy(state->renderer, tex, NULL, NULL);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(low_s);

    // Blit upper
    tex = SDL_CreateTextureFromSurface(state->renderer, sr->higher);
    SDL_RenderCopy(state->renderer, tex, NULL, NULL);
    SDL_DestroyTexture(tex);
}

void soft_render_background(
                    video_state *state,
                    surface *sur) {

    soft_renderer *sr = state->userdata;
    surface_alpha_blit(&sr->lower, sur, 0, 0, 0);
}

void soft_render_sprite_fsot(
                    video_state *state,
                    surface *sur,
                    SDL_Rect *dst,
                    SDL_BlendMode blend_mode,
                    int pal_offset,
                    SDL_RendererFlip flip_mode,
                    uint8_t opacity,
                    color color_mod) {

    // No scaling,
    // No opacity for paletted surfaces
    // No color modulation for paletted surfaces

    soft_renderer *sr = state->userdata;
    if(sur->type == SURFACE_TYPE_PALETTE) {
        if(blend_mode == SDL_BLENDMODE_ADD) {
            surface_additive_blit(&sr->lower, sur, dst->x, dst->y, state->base_palette, flip_mode);
        } else {
            surface_alpha_blit(&sr->lower, sur, dst->x, dst->y, flip_mode);
        }
    } else {
        surface_to_rgba(sur, sr->tmp_normal, state->cur_palette, NULL, 0, 0);
        SDL_Surface *s = surface_from_pixels(sr->tmp_normal, sur->w, sur->h);
        SDL_SetSurfaceAlphaMod(s, opacity);
        SDL_SetSurfaceColorMod(s, color_mod.r, color_mod.g, color_mod.b);
        SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(s, NULL, sr->higher, dst);
    }
}

void video_soft_init(video_state *state) {
    soft_renderer *sr = omf_calloc(1, sizeof(soft_renderer));
    sr->higher = SDL_CreateRGBSurface(0,
                                    320,
                                    200,
                                    32,
                                    0x000000FF,
                                    0x0000FF00,
                                    0x00FF0000,
                                    0xFF000000);
    SDL_SetSurfaceRLE(sr->higher, 1);
    surface_create(&sr->lower, SURFACE_TYPE_PALETTE, 320, 200);

    // Preallocate memory for more efficient drawing
    sr->tmp_normal = omf_calloc(1, 320 * 200 * 4);
    sr->tmp_scaling = NULL;
    if(state->scale_factor > 1) {
        sr->tmp_scaling = omf_calloc(1, 320 * 200 * 4 * state->scale_factor * state->scale_factor);
    }

    // Set as userdata
    state->userdata = sr;

    // Bind functions
    state->cb.render_close = soft_render_close;
    state->cb.render_reinit = soft_render_reinit;
    state->cb.render_prepare = soft_render_prepare;
    state->cb.render_finish = soft_render_finish;
    state->cb.render_fsot = soft_render_sprite_fsot;
    state->cb.render_background = soft_render_background;
    DEBUG("Switched to software renderer.");
}
