#include "video/renderers/software/software_renderer.h"

#include "utils/allocator.h"
#include "utils/log.h"
#include "utils/platform_time.h"
#include "video/enums.h"
#include "video/vga_state.h"

#include <SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GAME_W 320
#define GAME_H 200
#define MAGIC_REMAP_ROUNDS 12

// Like the OpenGL renderer's RGBA16 intermediate buffer: index, remap,
// dark-tint index and additive index must survive separate drawing passes.
typedef struct {
    uint16_t index;
    uint16_t dark;
    uint16_t add;
    uint8_t remap;
} indexed_pixel;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *sdl;
    SDL_Texture *texture;
    indexed_pixel framebuffer[GAME_W * GAME_H];
    uint32_t pixels[GAME_W * GAME_H];
    vga_palette palette;
    vga_remap_tables remaps;
    int screen_w;
    int screen_h;
    int aspect;
    int fb_scale;
    int target_x;
    int target_y;
    int fps_limit;
    uint32_t last_frame;
    unsigned framebuffer_options;
    SDL_Rect offscreen_area;
    video_screenshot_signal screenshot;
    bool fullscreen;
    bool vsync;
} software_context;

static bool available(void) {
    return true;
}

static const char *description(void) {
    return "Portable 320x200 paletted SDL2 software renderer";
}

static const char *name(void) {
    return "Software";
}

static void create(renderer *r) {
    r->ctx = omf_calloc(1, sizeof(software_context));
}

static void destroy(renderer *r) {
    omf_free(r->ctx);
    r->ctx = NULL;
}

static bool setup(void *data, int width, int height, bool fullscreen, bool vsync, int aspect, int fps,
                  int fb_scale, int scaling_mode) {
    software_context *ctx = data;
#ifdef __PSP__
    width = 480;
    height = 272;
    fullscreen = true;
#endif
    ctx->screen_w = width;
    ctx->screen_h = height;
    ctx->aspect = aspect;
    ctx->fb_scale = fb_scale;
    ctx->fullscreen = fullscreen;
    ctx->vsync = vsync;
    ctx->fps_limit = fps;
    ctx->last_frame = omf_time_ms32();

    ctx->window = SDL_CreateWindow("OpenOMF", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    width, height, SDL_WINDOW_SHOWN);
    if(!ctx->window) {
        log_error("SDL software window: %s", SDL_GetError());
        return false;
    }
    ctx->sdl = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if(!ctx->sdl) {
        ctx->sdl = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if(!ctx->sdl) {
        log_error("SDL software renderer: %s", SDL_GetError());
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
        return false;
    }
    ctx->texture = SDL_CreateTexture(ctx->sdl, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, GAME_W, GAME_H);
    if(!ctx->texture) {
        log_error("SDL software framebuffer: %s", SDL_GetError());
        SDL_DestroyRenderer(ctx->sdl);
        SDL_DestroyWindow(ctx->window);
        ctx->sdl = NULL;
        ctx->window = NULL;
        return false;
    }
    vga_state_mark_dirty();
    log_info("SDL2 software renderer initialized at %dx%d", width, height);
    return true;
}

static void get_state(void *data, int *width, int *height, bool *fullscreen, bool *vsync, int *aspect, int *fb_scale) {
    software_context *ctx = data;
    if(width) *width = ctx->screen_w;
    if(height) *height = ctx->screen_h;
    if(fullscreen) *fullscreen = ctx->fullscreen;
    if(vsync) *vsync = ctx->vsync;
    if(aspect) *aspect = ctx->aspect;
    if(fb_scale) *fb_scale = ctx->fb_scale;
}

static bool reset_with(void *data, int width, int height, bool fullscreen, bool vsync, int aspect, int fps,
                       int fb_scale, int scaling_mode) {
    software_context *ctx = data;
#ifndef __PSP__
    if(SDL_SetWindowSize(ctx->window, width, height) < 0) {
        return false;
    }
    ctx->screen_w = width;
    ctx->screen_h = height;
    SDL_SetWindowFullscreen(ctx->window, fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    ctx->fullscreen = fullscreen;
#endif
    ctx->aspect = aspect;
    ctx->vsync = vsync;
    ctx->fb_scale = fb_scale;
    ctx->fps_limit = fps;
    return true;
}

static void reset(void *data) {
    (void)data;
}

static void close_context(void *data) {
    software_context *ctx = data;
    if(ctx->texture) SDL_DestroyTexture(ctx->texture);
    if(ctx->sdl) SDL_DestroyRenderer(ctx->sdl);
    if(ctx->window) SDL_DestroyWindow(ctx->window);
    ctx->texture = NULL;
    ctx->sdl = NULL;
    ctx->window = NULL;
}

static int clamp_index(int index) {
    if(index < 0) return 0;
    if(index >= VGA_PALETTE_SIZE) return VGA_PALETTE_SIZE - 1;
    return index;
}

static int remap_index(const software_context *ctx, int index, int row) {
    if(row < 0 || row >= VGA_REMAP_COUNT) return index;
    return ctx->remaps.tables[row].data[clamp_index(index)];
}

static int resolve_index(const software_context *ctx, const indexed_pixel *p) {
    int idx = p->index + p->add;
    if(ctx->framebuffer_options & FBUFOPT_CREDITS) {
        return clamp_index(idx);
    }

    int rounds = p->remap / VGA_REMAP_COUNT;
    int row = p->remap % VGA_REMAP_COUNT;
    if(p->dark > 0 && rounds != MAGIC_REMAP_ROUNDS) {
        idx = p->dark;
    } else if(p->dark >= 0x60) {
        idx = p->dark;
        rounds = 0;
    } else if(p->dark > 0) {
        int behind = 1 + (remap_index(ctx, idx, 4) - 0xA8 < 0 ? 0 :
                          remap_index(ctx, idx, 4) - 0xA8 > 7 ? 7 : remap_index(ctx, idx, 4) - 0xA8) * 2;
        idx = (p->dark & 0xF0) + ((p->dark & 0x0F) * 3 + behind * 2) / 5;
    }
    for(int i = 0; i < rounds; i++) {
        idx = remap_index(ctx, idx, row);
    }
    return clamp_index(idx);
}

static void flush_state(software_context *ctx) {
    vga_palette *palette;
    vga_remap_tables *remaps;
    if(vga_state_is_palette_dirty(&palette, NULL, NULL)) {
        memcpy(&ctx->palette, palette, sizeof(ctx->palette));
        vga_state_mark_palette_flushed();
    }
    if(vga_state_is_remap_dirty(&remaps)) {
        memcpy(&ctx->remaps, remaps, sizeof(ctx->remaps));
        vga_state_mark_remaps_flushed();
    }
}

static void draw(void *data, const surface *src, SDL_Rect *rect, int remap_offset, int remap_rounds,
                 int palette_offset, int palette_limit, int opacity, unsigned int flip, unsigned int options) {
    software_context *ctx = data;
    if(!src || !src->data || rect->w <= 0 || rect->h <= 0 || src->w <= 0 || src->h <= 0) return;
    // SPRITE_REMAP samples the current table during the paletted pass.
    if(options & SPRITE_REMAP) {
        vga_remap_tables *remaps;
        if(vga_state_is_remap_dirty(&remaps)) {
            memcpy(&ctx->remaps, remaps, sizeof(ctx->remaps));
            vga_state_mark_remaps_flushed();
        }
    }
    int x0 = rect->x < 0 ? 0 : rect->x;
    int y0 = rect->y < 0 ? 0 : rect->y;
    int x1 = rect->x + rect->w > GAME_W ? GAME_W : rect->x + rect->w;
    int y1 = rect->y + rect->h > GAME_H ? GAME_H : rect->y + rect->h;
    for(int y = y0; y < y1; y++) {
        int src_y = (y - rect->y) * src->h / rect->h;
        if(flip & FLIP_VERTICAL) src_y = src->h - 1 - src_y;
        for(int x = x0; x < x1; x++) {
            int src_x = (x - rect->x) * src->w / rect->w;
            if(flip & FLIP_HORIZONTAL) src_x = src->w - 1 - src_x;
            int idx = src->data[src_y * src->w + src_x];
            if(options & SPRITE_SHADOW) {
                int coverage = 0;
                for(int i = -1; i <= 2; i++) {
                    int sy = src_y + i;
                    if(sy >= 0 && sy < src->h && src->data[sy * src->w + src_x] != src->transparent) coverage++;
                }
                if(!coverage) continue;
                idx = coverage;
            } else if(idx == src->transparent) {
                continue;
            }
            // Four-by-four coverage dithering, cheaper on PSP than the GLSL noise function.
            static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
            if(opacity < 255 && bayer[(x & 3) | ((y & 3) << 2)] * 16 > opacity) continue;
            if(idx <= palette_limit) {
                idx = clamp_index(idx + palette_offset);
                if(idx > palette_limit) idx = palette_limit;
            }
            if((options & SPRITE_REMAP) && !((options & SPRITE_HAR_QUIRKS) && idx > 0x30)) {
                idx = remap_index(ctx, idx, remap_offset);
            }
            indexed_pixel *p = &ctx->framebuffer[y * GAME_W + x];
            if(options & SPRITE_DARK_TINT) {
                p->remap = remap_offset + MAGIC_REMAP_ROUNDS * VGA_REMAP_COUNT;
                p->dark = idx;
                p->add = 0;
            } else if(options & SPRITE_SHADOW) {
                int encoded = remap_offset + remap_rounds * VGA_REMAP_COUNT + idx;
                if(encoded > p->remap) p->remap = encoded > 255 ? 255 : encoded;
            } else if(remap_rounds > 0) {
                int encoded = remap_offset + remap_rounds * VGA_REMAP_COUNT + idx;
                p->remap = encoded > 255 ? 255 : encoded;
            } else if(options & SPRITE_INDEX_ADD) {
                p->add = idx * 60;
            } else {
                p->index = idx;
                p->remap = 0;
                p->dark = 0;
                p->add = 0;
            }
        }
    }
}

static void move_target(void *data, int x, int y) {
    software_context *ctx = data;
    ctx->target_x = x;
    ctx->target_y = y;
}

static void prepare(void *data, unsigned opts) {
    software_context *ctx = data;
    ctx->framebuffer_options = opts;
    memset(ctx->framebuffer, 0, sizeof(ctx->framebuffer));
}

static void finish(void *data) {
    software_context *ctx = data;
    flush_state(ctx);
    for(int i = 0; i < GAME_W * GAME_H; i++) {
        int idx = resolve_index(ctx, &ctx->framebuffer[i]);
        vga_color c = ctx->palette.colors[idx];
        ctx->pixels[i] = 0xff000000u | ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
    }

    if(ctx->screenshot) {
        uint8_t *rgb = omf_malloc(GAME_W * GAME_H * 3);
        for(int i = 0; i < GAME_W * GAME_H; i++) {
            rgb[i * 3] = ctx->pixels[i] & 255;
            rgb[i * 3 + 1] = (ctx->pixels[i] >> 8) & 255;
            rgb[i * 3 + 2] = (ctx->pixels[i] >> 16) & 255;
        }
        SDL_Rect rect = {0, 0, GAME_W, GAME_H};
        ctx->screenshot(&rect, rgb, false);
        ctx->screenshot = NULL;
        omf_free(rgb);
    }

    SDL_UpdateTexture(ctx->texture, NULL, ctx->pixels, GAME_W * sizeof(uint32_t));
    SDL_SetRenderDrawColor(ctx->sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx->sdl);
    SDL_Rect viewport = {0, 0, ctx->screen_w, ctx->screen_h};
    if(ctx->aspect == 0) {
        int fit_w = ctx->screen_h * 4 / 3;
        if(fit_w <= ctx->screen_w) {
            viewport.w = fit_w;
            viewport.x = (ctx->screen_w - fit_w) / 2;
        } else {
            viewport.h = ctx->screen_w * 3 / 4;
            viewport.y = (ctx->screen_h - viewport.h) / 2;
        }
    }
    viewport.x += ctx->target_x * viewport.w / GAME_W;
    viewport.y += ctx->target_y * viewport.w / GAME_W;
    SDL_RenderCopy(ctx->sdl, ctx->texture, NULL, &viewport);
    SDL_RenderPresent(ctx->sdl);
    if(ctx->fps_limit > 0) {
        const uint32_t min_frame = 1000 / (unsigned)ctx->fps_limit;
        const uint32_t now = omf_time_ms32();
        if(now - ctx->last_frame < min_frame) omf_sleep_ms(min_frame - (now - ctx->last_frame));
    }
    ctx->last_frame = omf_time_ms32();
}

static void area_prepare(void *data, const SDL_Rect *area) {
    software_context *ctx = data;
    ctx->offscreen_area = *area;
    memset(ctx->framebuffer, 0, sizeof(ctx->framebuffer));
}

static void area_finish(void *data, surface *dst) {
    software_context *ctx = data;
    const SDL_Rect *area = &ctx->offscreen_area;
    surface_create(dst, area->w, area->h);
    dst->transparent = -1;
    for(int y = 0; y < area->h; y++) {
        int src_y = area->y + y;
        for(int x = 0; x < area->w; x++) {
            int src_x = area->x + x;
            if(src_x >= 0 && src_x < GAME_W && src_y >= 0 && src_y < GAME_H) {
                dst->data[y * dst->w + x] = clamp_index(ctx->framebuffer[src_y * GAME_W + src_x].index);
            }
        }
    }
}

static void capture(void *data, video_screenshot_signal cb) {
    ((software_context *)data)->screenshot = cb;
}

static void no_op(void *data) {
    (void)data;
}

static void atlas(void *data, bool enabled) {
    (void)data;
    (void)enabled;
}

void software_renderer_set_callbacks(renderer *r) {
    r->is_available = available;
    r->get_description = description;
    r->get_name = name;
    r->create = create;
    r->destroy = destroy;
    r->setup_context = setup;
    r->get_context_state = get_state;
    r->reset_context_with = reset_with;
    r->reset_context = reset;
    r->close_context = close_context;
    r->draw_surface = draw;
    r->move_target = move_target;
    r->render_prepare = prepare;
    r->render_finish = finish;
    r->render_area_prepare = area_prepare;
    r->render_area_finish = area_finish;
    r->capture_screen = capture;
    r->signal_scene_change = no_op;
    r->signal_draw_atlas = atlas;
}
