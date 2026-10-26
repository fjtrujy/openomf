/*
 * PSP GPU renderer for OpenOMF.
 *
 * Ordinary sprites remain VGA indexed throughout their lifetime: the PSP GU
 * samples 8-bit textures using its hardware CLUT. This avoids CPU rasterizing
 * the 320x200 framebuffer and avoids RGBA texture conversions on palette fades.
 *
 * Fixed-function GU cannot remap the DESTINATION palette index (GL3's
 * multi-channel intermediate). For those unusual overlays we replay recorded
 * sprite commands through the existing indexed compositing rules and submit
 * one RGBA image to GU. Offscreen capture also uses indexed compositing.
 *
 * The PSP GU owns the framebuffer and display lifecycle. omf_rect remains a
 * shared renderer interface type; a native PSP build needs no SDL window.
 */
#include "video/renderers/psp/gu_renderer.h"

#include "utils/allocator.h"
#include "utils/log.h"
#include "video/enums.h"
#include "video/vga_state.h"

#ifndef PSP_NATIVE_DISPLAY
#include "utils/platform_types.h"
#endif
#include <malloc.h>
#include <pspdisplay.h>
#include "utils/platform_time.h"
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 480
#define SCREEN_H 272
#define BUFFER_PITCH 512
#define GAME_W 320
#define GAME_H 200
#define FRAME_BYTES (BUFFER_PITCH * SCREEN_H * 4)
#define GU_LIST_WORDS 262144
#define MAX_TEXTURES 256
#define MAX_TEXTURE_BYTES (4u * 1024u * 1024u)
#define MAX_COMMANDS 4096
#define MAX_CLUTS 64
#define MAGIC_REMAP_ROUNDS 12

#if VGA_PALETTE_SIZE != 256
#error "PSP GU T8 textures require OpenOMF's 256-color palette build"
#endif

typedef struct {
    unsigned guid;
    int w, h, pitch, tex_h;
    size_t bytes;
    unsigned long recent;
    unsigned long used_frame;
    uint8_t *data; /* immutable snapshot of one source surface GUID */
    uint8_t *shadow_mask; /* cached four-tap coverage values */
} gu_texture;

typedef struct {
    uint16_t texture;
    int transparent;
    omf_rect dst;
    int remap_offset, remap_rounds;
    int palette_offset, palette_limit, opacity;
    unsigned flip, options;
} draw_command;

typedef struct {
    int transparent, remap_offset, remap_rounds, palette_offset, palette_limit;
    unsigned options;
    uint32_t *colors;
} clut_variant;

typedef struct {
    uint16_t index;
    uint16_t dark;
    uint16_t add;
    uint8_t remap;
} indexed_pixel;

typedef struct {
#ifndef PSP_NATIVE_DISPLAY
    SDL_Window *window;
#endif
    bool force_software;
    int screen_w, screen_h, aspect, fb_scale, fps;
    int target_x, target_y;
    bool fullscreen, vsync, offscreen, needs_cpu;
    unsigned framebuffer_options;
    uint32_t last_frame;
    unsigned long frame_no, use_clock;
    size_t texture_bytes;
    gu_texture textures[MAX_TEXTURES];
    draw_command commands[MAX_COMMANDS];
    unsigned command_count;
    unsigned long gpu_frames, cpu_frames, shadow_frames, remap_frames, tint_frames, additive_frames;
    unsigned frame_effects;
    vga_palette palette;
    vga_remap_tables remaps;
    _Alignas(16) uint32_t display_list[GU_LIST_WORDS];
    _Alignas(16) uint32_t clut_storage[MAX_CLUTS][VGA_PALETTE_SIZE];
    clut_variant cluts[MAX_CLUTS];
    unsigned clut_count;
    indexed_pixel *cpu_frame;
    uint32_t *rgba_frame;
    omf_rect offscreen_area;
    video_screenshot_signal screenshot;
} gu_context;

typedef struct {
    float u, v, x, y, z;
} gu_sprite_vertex;

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int value) { return imin(imax(value, 0), VGA_PALETTE_SIZE - 1); }

static bool is_available(void) { return true; }
static const char *get_name(void) { return "PSP GU"; }
static const char *get_description(void) { return "Native PSP GU 8-bit indexed sprite renderer"; }

static void create(renderer *r) {
    // Large command and palette storage belongs on the heap rather than the PSP stack.
    // GU command lists and CLUT arrays require 16-byte alignment on hardware.
    gu_context *ctx = memalign(16, sizeof(gu_context));
    if(ctx) {
        memset(ctx, 0, sizeof(*ctx));
    } else {
        log_error("PSP GU: out of memory creating renderer context");
    }
    r->ctx = ctx;
}

#ifdef PSP_STANDALONE
static void create_software(renderer *r) {
    create(r);
    if(r->ctx) ((gu_context *)r->ctx)->force_software = true;
}
static const char *get_software_name(void) { return "Software"; }
static const char *get_software_description(void) {
    return "PSP indexed CPU software compositor presented through GU";
}
#endif

static void clear_textures(gu_context *ctx) {
    for(int i = 0; i < MAX_TEXTURES; i++) {
        free(ctx->textures[i].data);
        free(ctx->textures[i].shadow_mask);
        memset(&ctx->textures[i], 0, sizeof(ctx->textures[i]));
    }
    ctx->texture_bytes = 0;
}

static void destroy(renderer *r) {
    gu_context *ctx = r->ctx;
    if(!ctx) return;
    clear_textures(ctx);
    free(ctx->cpu_frame);
    free(ctx->rgba_frame);
    free(ctx);
    r->ctx = NULL;
}

static void flush_palette(gu_context *ctx) {
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

static bool setup(void *userdata, int w, int h, bool fullscreen, bool vsync, int aspect, int fps,
                  int fb_scale, int scaling_mode) {
    (void)w; (void)h; (void)fullscreen; (void)scaling_mode;
    gu_context *ctx = userdata;
    if(!ctx) return false;
    ctx->screen_w = SCREEN_W;
    ctx->screen_h = SCREEN_H;
    ctx->fullscreen = true;
    ctx->vsync = vsync;
    ctx->aspect = aspect;
    ctx->fps = fps;
    ctx->fb_scale = fb_scale;
    ctx->last_frame = omf_time_ms32();
#ifdef PSP_NATIVE_DISPLAY
    // Initialize the physical PSP display before configuring GU buffers.
    // No SDL video driver or SDL window is needed on the native path.
    if(sceDisplaySetMode(0, SCREEN_W, SCREEN_H) < 0) {
        log_error("PSP GU: sceDisplaySetMode failed");
        return false;
    }
#else
    ctx->window = SDL_CreateWindow("OpenOMF PSP GU", SDL_WINDOWPOS_UNDEFINED,
                                    SDL_WINDOWPOS_UNDEFINED, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if(!ctx->window) {
        log_error("PSP GU window: %s", SDL_GetError());
        return false;
    }
#endif

    sceGuInit();
    sceGuStart(GU_DIRECT, ctx->display_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUFFER_PITCH);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, (void *)FRAME_BYTES, BUFFER_PITCH);
    sceGuOffset(2048 - SCREEN_W / 2, 2048 - SCREEN_H / 2);
    sceGuViewport(2048, 2048, SCREEN_W, SCREEN_H);
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuClearColor(0xff000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceGuDisplay(GU_TRUE);
    vga_state_mark_dirty();
#ifdef PSP_NATIVE_DISPLAY
    log_info("PSP GU hardware renderer initialized (%dx%d, native sceDisplay, no SDL window, indexed texture CLUT)",
             SCREEN_W, SCREEN_H);
#else
    log_info("PSP GU hardware renderer initialized (%dx%d, SDL window, indexed texture CLUT)", SCREEN_W, SCREEN_H);
#endif
    return true;
}

static void state(void *userdata, int *w, int *h, bool *fs, bool *vsync, int *aspect, int *fb_scale) {
    const gu_context *ctx = userdata;
    if(w) *w = ctx->screen_w;
    if(h) *h = ctx->screen_h;
    if(fs) *fs = ctx->fullscreen;
    if(vsync) *vsync = ctx->vsync;
    if(aspect) *aspect = ctx->aspect;
    if(fb_scale) *fb_scale = ctx->fb_scale;
}

static bool reset_with(void *userdata, int w, int h, bool fs, bool vsync, int aspect, int fps,
                       int fb_scale, int scaling_mode) {
    (void)w; (void)h; (void)fs; (void)scaling_mode;
    gu_context *ctx = userdata;
    ctx->vsync = vsync;
    ctx->aspect = aspect;
    ctx->fps = fps;
    ctx->fb_scale = fb_scale;
    return true;
}

static void reset(void *userdata) { (void)userdata; }

static void close_context(void *userdata) {
    gu_context *ctx = userdata;
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
#ifndef PSP_NATIVE_DISPLAY
    if(ctx->window) SDL_DestroyWindow(ctx->window);
    ctx->window = NULL;
#endif
}

static int next_pow2(int n) {
    int r = 16;
    while(r < n && r < 512) r <<= 1;
    return r;
}

static gu_texture *get_texture(gu_context *ctx, const surface *src) {
    if(src->w <= 0 || src->h <= 0 || !src->data) {
        return NULL;
    }
    ctx->use_clock++;
    for(int i = 0; i < MAX_TEXTURES; i++) {
        gu_texture *tex = &ctx->textures[i];
        if(tex->data && tex->guid == src->guid && tex->w == src->w && tex->h == src->h) {
            tex->recent = ctx->use_clock;
            tex->used_frame = ctx->frame_no;
            return tex;
        }
    }

    // GU indexed textures max out at 512x512. Keep larger surfaces in the
    // same immutable cache and replay them with the CPU compositor instead of
    // silently omitting part of a scene.
    bool gpu_compatible = src->w <= 512 && src->h <= 512;
    int width = gpu_compatible ? next_pow2(src->w) : src->w;
    int height = gpu_compatible ? next_pow2(src->h) : src->h;
    size_t bytes = (size_t)width * height;
    if(bytes > MAX_TEXTURE_BYTES) return NULL;
    int slot = -1;
    while(ctx->texture_bytes + bytes > MAX_TEXTURE_BYTES || slot < 0) {
        slot = -1;
        for(int i = 0; i < MAX_TEXTURES; i++) {
            if(!ctx->textures[i].data) { slot = i; break; }
        }
        if(slot >= 0 && ctx->texture_bytes + bytes <= MAX_TEXTURE_BYTES) break;

        unsigned long least_recent = ~0ul;
        int victim = -1;
        for(int i = 0; i < MAX_TEXTURES; i++) {
            gu_texture *tex = &ctx->textures[i];
            if(tex->data && tex->used_frame != ctx->frame_no && tex->recent < least_recent) {
                least_recent = tex->recent;
                victim = i;
            }
        }
        if(victim < 0) {
            return NULL;
        }
        ctx->texture_bytes -= ctx->textures[victim].bytes;
        free(ctx->textures[victim].data);
        free(ctx->textures[victim].shadow_mask);
        memset(&ctx->textures[victim], 0, sizeof(ctx->textures[victim]));
        slot = victim;
    }

    gu_texture *tex = &ctx->textures[slot];
    uint8_t *pixels = memalign(16, bytes);
    if(!pixels) return NULL;
    memset(pixels, 0, bytes);
    for(int y = 0; y < src->h; y++) {
        for(int x = 0; x < src->w; x++) {
            pixels[y * width + x] = (uint8_t)src->data[y * src->w + x];
        }
    }
    if(gpu_compatible) sceKernelDcacheWritebackRange(pixels, bytes);
    tex->data = pixels;
    tex->guid = src->guid;
    tex->w = src->w;
    tex->h = src->h;
    tex->pitch = width;
    tex->tex_h = height;
    tex->bytes = bytes;
    tex->recent = ctx->use_clock;
    tex->used_frame = ctx->frame_no;
    ctx->texture_bytes += bytes;
    return tex;
}

static void prepare(void *userdata, unsigned framebuffer_options) {
    gu_context *ctx = userdata;
    ctx->command_count = 0;
    ctx->clut_count = 0;
    ctx->frame_no++;
    ctx->frame_effects = 0;
    ctx->offscreen = false;
    ctx->needs_cpu = ctx->force_software || ctx->screenshot != NULL;
    ctx->framebuffer_options = framebuffer_options;
}

static void draw(void *userdata, const surface *src, omf_rect *rect, int remap_offset, int remap_rounds,
                 int palette_offset, int palette_limit, int opacity, unsigned int flip, unsigned int options) {
    gu_context *ctx = userdata;
    if(!src || !src->data || rect->w <= 0 || rect->h <= 0) return;
    if(ctx->command_count >= MAX_COMMANDS) {
        // Hard safety limit: the command buffer must never overrun memory.
        log_error("PSP GU draw command capacity exceeded");
        return;
    }
    gu_texture *tex = get_texture(ctx, src);
    if(!tex) {
        log_error("PSP GU texture cache exhausted (source=%dx%d)", src->w, src->h);
        return;
    }
    int index = (int)(tex - ctx->textures);
    draw_command *cmd = &ctx->commands[ctx->command_count++];
    *cmd = (draw_command) {
        .texture = index, .transparent = src->transparent, .dst = *rect,
        .remap_offset = remap_offset, .remap_rounds = remap_rounds,
        .palette_offset = palette_offset, .palette_limit = palette_limit,
        .opacity = opacity, .flip = flip, .options = options
    };
    if(remap_rounds != 0 || (options & (SPRITE_SHADOW | SPRITE_DARK_TINT | SPRITE_INDEX_ADD))) {
#ifdef PSP_GU_APPROX_EFFECTS
        if(options & SPRITE_INDEX_ADD) ctx->needs_cpu = true;
#else
        ctx->needs_cpu = true;
#endif
        if(remap_rounds) ctx->frame_effects |= 1;
        if(options & SPRITE_SHADOW) ctx->frame_effects |= 2;
        if(options & SPRITE_DARK_TINT) ctx->frame_effects |= 4;
        if(options & SPRITE_INDEX_ADD) ctx->frame_effects |= 8;
    }
    if(tex->w > 512 || tex->h > 512) {
        ctx->needs_cpu = true;
    }
}

#ifdef PSP_GU_APPROX_EFFECTS
static bool ensure_shadow_mask(gu_context *ctx, gu_texture *tex, int transparent) {
    if(tex->shadow_mask) return true;
    // Precompute the coverage used by OpenOMF's four-tap shadow shader ONCE
    // rather than evaluating its mask at every destination pixel every frame.
    const size_t bytes = (size_t)tex->pitch * tex->tex_h;
    if(ctx->texture_bytes + bytes > MAX_TEXTURE_BYTES) return false;
    uint8_t *mask = memalign(16, bytes);
    if(!mask) return false;
    memset(mask, 0, bytes);
    for(int y = 0; y < tex->h; y++) {
        for(int x = 0; x < tex->w; x++) {
            int coverage = 0;
            for(int k = -1; k <= 2; k++) {
                int sy = y + k;
                if(sy >= 0 && sy < tex->h &&
                   tex->data[sy * tex->pitch + x] != transparent) coverage++;
            }
            mask[y * tex->pitch + x] = (uint8_t)coverage;
        }
    }
    sceKernelDcacheWritebackRange(mask, bytes);
    tex->shadow_mask = mask;
    tex->bytes += bytes;
    ctx->texture_bytes += bytes;
    return true;
}
#endif

static int remap_index(const gu_context *ctx, int index, int row) {
    if(row < 0 || row >= VGA_REMAP_COUNT) return index;
    return ctx->remaps.tables[row].data[iclamp(index)];
}

static int source_index(const gu_context *ctx, const draw_command *cmd, int idx) {
    if(idx <= cmd->palette_limit) {
        idx = imin(iclamp(idx + cmd->palette_offset), cmd->palette_limit);
    }
    if((cmd->options & SPRITE_REMAP) &&
       !((cmd->options & SPRITE_HAR_QUIRKS) && idx > 0x30)) {
        idx = remap_index(ctx, idx, cmd->remap_offset);
    }
    return iclamp(idx);
}

static int get_clut(gu_context *ctx, const draw_command *cmd) {
    for(unsigned i = 0; i < ctx->clut_count; i++) {
        clut_variant *entry = &ctx->cluts[i];
        if(entry->transparent == cmd->transparent && entry->remap_offset == cmd->remap_offset &&
           entry->remap_rounds == cmd->remap_rounds &&
           entry->palette_offset == cmd->palette_offset && entry->palette_limit == cmd->palette_limit &&
           entry->options == cmd->options) return (int)i;
    }
    if(ctx->clut_count >= MAX_CLUTS) return -1;
    unsigned slot = ctx->clut_count++;
    clut_variant *v = &ctx->cluts[slot];
    v->transparent = cmd->transparent;
    v->remap_offset = cmd->remap_offset;
    v->remap_rounds = cmd->remap_rounds;
    v->palette_offset = cmd->palette_offset;
    v->palette_limit = cmd->palette_limit;
    v->options = cmd->options;
    v->colors = ctx->clut_storage[slot];
    for(int i = 0; i < VGA_PALETTE_SIZE; i++) {
#ifdef PSP_GU_APPROX_EFFECTS
        if(cmd->options & SPRITE_SHADOW) {
            // Covers 0..4 vertically adjacent source pixels. The PSP GU has
            // no destination-index lookup, so darken with coverage-based alpha.
            const unsigned a = i == 0 ? 0 : (unsigned)imin(220, 12 + i * 38);
            v->colors[i] = a << 24;
            continue;
        }
        if(cmd->remap_rounds > 0) {
            // The intro's electrical flash lightens destination indices.
            // A black-alpha approximation incorrectly turns it into a dark
            // silhouette. Use a bright blend without forcing CPU replay.
            const bool bright = !!(ctx->framebuffer_options & FBUFOPT_BRIGHT_DESTINATION_REMAP);
            const unsigned a = i == cmd->transparent ? 0 : (unsigned)(bright ?
                    imin(255, 120 + cmd->remap_rounds * 60) :
                    imin(210, 60 + cmd->remap_rounds * 25));
            v->colors[i] = (a << 24) | (bright ? 0x00ffffffu : 0u);
            continue;
        }
        if(cmd->options & SPRITE_DARK_TINT) {
            int idx = source_index(ctx, cmd, i);
            vga_color c = ctx->palette.colors[idx];
            v->colors[i] = (i == cmd->transparent ? 0u : 0xb0000000u) |
                           ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
            continue;
        }
#endif
        int idx = source_index(ctx, cmd, i);
        vga_color c = ctx->palette.colors[idx];
        v->colors[i] = (i == cmd->transparent ? 0u : 0xff000000u) |
                       ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
    }
    sceKernelDcacheWritebackRange(v->colors, sizeof(ctx->clut_storage[slot]));
    return slot;
}

static omf_rect viewport(const gu_context *ctx) {
    omf_rect r = {0, 0, SCREEN_W, SCREEN_H};
    if(ctx->aspect == 0) {
        r.w = SCREEN_H * 4 / 3;
        r.x = (SCREEN_W - r.w) / 2;
    }
    r.x += ctx->target_x * r.w / GAME_W;
    r.y += ctx->target_y * r.w / GAME_W;
    return r;
}

static void sprite_vertices(gu_sprite_vertex *v, const omf_rect *screen, int tw, int th, unsigned flip) {
    v[0] = (gu_sprite_vertex) {
        .u = flip & FLIP_HORIZONTAL ? tw : 0,
        .v = flip & FLIP_VERTICAL ? th : 0,
        .x = screen->x, .y = screen->y, .z = 0
    };
    v[1] = (gu_sprite_vertex) {
        .u = flip & FLIP_HORIZONTAL ? 0 : tw,
        .v = flip & FLIP_VERTICAL ? 0 : th,
        .x = screen->x + screen->w, .y = screen->y + screen->h, .z = 0
    };
}

static void gpu_draw(gu_context *ctx, const draw_command *cmd, const omf_rect *vp, int clut,
                     bool load_clut) {
    gu_texture *tex = &ctx->textures[cmd->texture];
    if(load_clut) {
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, ctx->clut_storage[clut]);
    }
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
#ifdef PSP_GU_APPROX_EFFECTS
    if(cmd->options & SPRITE_SHADOW) {
        // The mask was prepared before sceGuStart. Do not allocate or drop
        // a draw command while a GPU display list is being constructed.
        sceGuTexImage(0, tex->pitch, tex->tex_h, tex->pitch, tex->shadow_mask);
    } else {
        sceGuTexImage(0, tex->pitch, tex->tex_h, tex->pitch, tex->data);
    }
#else
    sceGuTexImage(0, tex->pitch, tex->tex_h, tex->pitch, tex->data);
#endif
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuColor(((unsigned)(uint8_t)cmd->opacity << 24) | 0x00ffffffu);
    // Preserve the GL3 logical coordinate system; the PSP scales at the end.
    omf_rect dst = {
        vp->x + cmd->dst.x * vp->w / GAME_W,
        vp->y + cmd->dst.y * vp->h / GAME_H,
        (cmd->dst.w * vp->w + GAME_W / 2) / GAME_W,
        (cmd->dst.h * vp->h + GAME_H / 2) / GAME_H
    };
    gu_sprite_vertex *v = sceGuGetMemory(sizeof(gu_sprite_vertex) * 2);
    sprite_vertices(v, &dst, tex->w, tex->h, cmd->flip);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

static indexed_pixel *cpu_buffer(gu_context *ctx) {
    if(!ctx->cpu_frame) {
        ctx->cpu_frame = calloc(GAME_W * GAME_H, sizeof(indexed_pixel));
    }
    if(ctx->cpu_frame) memset(ctx->cpu_frame, 0, GAME_W * GAME_H * sizeof(indexed_pixel));
    return ctx->cpu_frame;
}

static void cpu_draw(gu_context *ctx, const draw_command *cmd) {
    const gu_texture *tex = &ctx->textures[cmd->texture];
    const omf_rect *r = &cmd->dst;
    const int x0 = imax(0, r->x), y0 = imax(0, r->y);
    const int x1 = imin(GAME_W, r->x + r->w), y1 = imin(GAME_H, r->y + r->h);
    if(r->w <= 0 || r->h <= 0) return;
    for(int y = y0; y < y1; y++) {
        int sy = (y - r->y) * tex->h / r->h;
        if(cmd->flip & FLIP_VERTICAL) sy = tex->h - 1 - sy;
        for(int x = x0; x < x1; x++) {
            int sx = (x - r->x) * tex->w / r->w;
            if(cmd->flip & FLIP_HORIZONTAL) sx = tex->w - 1 - sx;
            int index = tex->data[sy * tex->pitch + sx];
            if(cmd->options & SPRITE_SHADOW) {
                int coverage = 0;
                for(int k = -1; k <= 2; k++) {
                    int ty = sy + k;
                    if(ty >= 0 && ty < tex->h && tex->data[ty * tex->pitch + sx] != cmd->transparent)
                        coverage++;
                }
                if(!coverage) continue;
                index = coverage;
            } else if(index == cmd->transparent) {
                continue;
            }
            static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
            if(cmd->opacity < 255 && bayer[(x & 3) | ((y & 3) << 2)] * 16 > cmd->opacity) continue;
            index = source_index(ctx, cmd, index);
            indexed_pixel *p = &ctx->cpu_frame[y * GAME_W + x];
            if(cmd->options & SPRITE_DARK_TINT) {
                p->remap = cmd->remap_offset + MAGIC_REMAP_ROUNDS * VGA_REMAP_COUNT;
                p->dark = index;
                p->add = 0;
            } else if(cmd->options & SPRITE_SHADOW) {
                int encoded = cmd->remap_offset + cmd->remap_rounds * VGA_REMAP_COUNT + index;
                if(encoded > p->remap) p->remap = imin(encoded, 255);
            } else if(cmd->remap_rounds > 0) {
                p->remap = imin(cmd->remap_offset + cmd->remap_rounds * VGA_REMAP_COUNT + index, 255);
            } else if(cmd->options & SPRITE_INDEX_ADD) {
                p->add = index * 60;
            } else {
                p->index = index;
                p->remap = 0;
                p->dark = 0;
                p->add = 0;
            }
        }
    }
}

static int cpu_resolve(const gu_context *ctx, const indexed_pixel *p) {
    int index = p->index + p->add;
    if(ctx->framebuffer_options & FBUFOPT_CREDITS) return iclamp(index);
    int rounds = p->remap / VGA_REMAP_COUNT;
    int row = p->remap % VGA_REMAP_COUNT;
    if(p->dark > 0 && rounds != MAGIC_REMAP_ROUNDS) {
        index = p->dark;
    } else if(p->dark >= 0x60) {
        index = p->dark;
        rounds = 0;
    } else if(p->dark > 0) {
        const int val = imin(imax(remap_index(ctx, index, 4) - 0xA8, 0), 7);
        const int behind = 1 + val * 2;
        index = (p->dark & 0xf0) + ((p->dark & 0x0f) * 3 + behind * 2) / 5;
    }
    for(int i = 0; i < rounds; i++) index = remap_index(ctx, index, row);
    return iclamp(index);
}

static bool cpu_replay(gu_context *ctx) {
    if(!cpu_buffer(ctx)) return false;
    for(unsigned i = 0; i < ctx->command_count; i++) cpu_draw(ctx, &ctx->commands[i]);
    return true;
}

static void capture_cpu(gu_context *ctx) {
    if(!ctx->screenshot) return;
    uint8_t *rgb = malloc(GAME_W * GAME_H * 3);
    if(!rgb) return;
    for(int i = 0; i < GAME_W * GAME_H; i++) {
        int index = cpu_resolve(ctx, &ctx->cpu_frame[i]);
        vga_color c = ctx->palette.colors[index];
        rgb[i * 3] = c.r;
        rgb[i * 3 + 1] = c.g;
        rgb[i * 3 + 2] = c.b;
    }
    omf_rect r = {0, 0, GAME_W, GAME_H};
    ctx->screenshot(&r, rgb, false);
    ctx->screenshot = NULL;
    free(rgb);
}

static void gpu_draw_cpu_frame(gu_context *ctx, const omf_rect *vp) {
    if(!ctx->rgba_frame) {
        ctx->rgba_frame = memalign(16, 512 * 256 * sizeof(uint32_t));
    }
    if(!ctx->rgba_frame) return;
    for(int y = 0; y < GAME_H; y++) {
        for(int x = 0; x < GAME_W; x++) {
            int i = cpu_resolve(ctx, &ctx->cpu_frame[y * GAME_W + x]);
            vga_color c = ctx->palette.colors[i];
            ctx->rgba_frame[y * 512 + x] = 0xff000000u |
                ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
        }
    }
    sceKernelDcacheWritebackRange(ctx->rgba_frame, 512 * 256 * sizeof(uint32_t));
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 512, 256, 512, ctx->rgba_frame);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuColor(0xffffffff);
    gu_sprite_vertex *v = sceGuGetMemory(sizeof(gu_sprite_vertex) * 2);
    sprite_vertices(v, vp, GAME_W, GAME_H, 0);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

static void finish(void *userdata) {
    gu_context *ctx = userdata;
#ifdef PSP_GU_PROFILE
    const uint64_t gu_start_ms = omf_time_ms();
#endif
    flush_palette(ctx);
    // CLUT variants cannot represent destination-index remap. Replay precisely
    // through the indexed CPU buffer for such frames, not a lossy RGB overlay.
    if(!ctx->needs_cpu) {
        for(unsigned i = 0; i < ctx->command_count; i++) {
            if(get_clut(ctx, &ctx->commands[i]) < 0) {
                ctx->needs_cpu = true;
                break;
            }
#ifdef PSP_GU_APPROX_EFFECTS
            const draw_command *cmd = &ctx->commands[i];
            if(cmd->options & SPRITE_SHADOW) {
                gu_texture *tex = &ctx->textures[cmd->texture];
                if(!ensure_shadow_mask(ctx, tex, cmd->transparent)) {
                    // Preserve the entire frame rather than silently losing
                    // one shadow if the indexed-texture budget is exhausted.
                    ctx->needs_cpu = true;
                    break;
                }
            }
#endif
        }
    }
    if(ctx->needs_cpu && !cpu_replay(ctx)) {
        log_error("PSP GU CPU effect buffer unavailable");
        return;
    }
    if(ctx->screenshot && ctx->needs_cpu) capture_cpu(ctx);
    omf_rect vp = viewport(ctx);
    sceGuStart(GU_DIRECT, ctx->display_list);
    sceGuClearColor(0xff000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    if(ctx->needs_cpu) {
        ctx->cpu_frames++;
        if(ctx->frame_effects & 1) ctx->remap_frames++;
        if(ctx->frame_effects & 2) ctx->shadow_frames++;
        if(ctx->frame_effects & 4) ctx->tint_frames++;
        if(ctx->frame_effects & 8) ctx->additive_frames++;
        gpu_draw_cpu_frame(ctx, &vp);
    } else {
        ctx->gpu_frames++;
        int last_clut = -1;
        for(unsigned i = 0; i < ctx->command_count; i++) {
            const draw_command *cmd = &ctx->commands[i];
            // Palette changes are handled with GU CLUT loads, not framebuffer
            // conversions or texture uploads.
            int clut = get_clut(ctx, cmd);
            bool change = clut != last_clut;
            if(change) last_clut = clut;
            gpu_draw(ctx, cmd, &vp, clut, change);
        }
    }
    sceGuFinish();
#ifdef PSP_GU_PROFILE
    const uint64_t gu_commands_ms = omf_time_ms();
#endif
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
#ifdef PSP_GU_PROFILE
    const uint64_t gu_sync_ms = omf_time_ms();
#endif
    // The PSP display refreshes at 60 Hz. Even if desktop settings request
    // VSync OFF, cap native presentation to VBlank: otherwise the engine can
    // run thousands of frames per second in PPSSPP and animate too quickly.
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
#ifdef PSP_GU_PROFILE
    const uint64_t gu_swap_ms = omf_time_ms();
    if(gu_swap_ms - gu_start_ms > 120) {
        log_warn("PSP GU slow frame: commands=%llums sync=%llums vblank/swap=%llums draws=%u cache=%u KiB CPU=%d",
                 (unsigned long long)(gu_commands_ms - gu_start_ms),
                 (unsigned long long)(gu_sync_ms - gu_commands_ms),
                 (unsigned long long)(gu_swap_ms - gu_sync_ms),
                 ctx->command_count, (unsigned)(ctx->texture_bytes / 1024), (int)ctx->needs_cpu);
    }
#endif
    if(ctx->fps > 0) {
        const uint32_t delay = 1000 / (uint32_t)ctx->fps;
        const uint32_t now = omf_time_ms32();
        if(now - ctx->last_frame < delay) omf_sleep_ms(delay - (now - ctx->last_frame));
    }
    ctx->last_frame = omf_time_ms32();
    // Keep frequent I/O off flash storage; opt in explicitly when profiling.
#ifdef PSP_GU_PROFILE
    if(ctx->frame_no % 600 == 0) {
        log_info("PSP GU statistics: GPU=%lu CPU-effects=%lu remap=%lu shadow=%lu tint=%lu add=%lu, last draw=%u, texture-cache=%u KiB",
                 ctx->gpu_frames, ctx->cpu_frames, ctx->remap_frames, ctx->shadow_frames,
                 ctx->tint_frames, ctx->additive_frames, ctx->command_count,
                 (unsigned)(ctx->texture_bytes / 1024));
    }
#endif
}

static void area_prepare(void *userdata, const omf_rect *area) {
    gu_context *ctx = userdata;
    ctx->command_count = 0;
    ctx->frame_no++;
    ctx->offscreen = true;
    ctx->offscreen_area = *area;
}

static void area_finish(void *userdata, surface *dst) {
    gu_context *ctx = userdata;
    const omf_rect *r = &ctx->offscreen_area;
    flush_palette(ctx);
    if(!cpu_replay(ctx)) {
        surface_create(dst, 1, 1);
        return;
    }
    surface_create(dst, r->w, r->h);
    dst->transparent = -1;
    for(int y = 0; y < r->h; y++) {
        for(int x = 0; x < r->w; x++) {
            int sx = r->x + x;
            int sy = r->y + y;
            if(sx >= 0 && sx < GAME_W && sy >= 0 && sy < GAME_H) {
                dst->data[y * r->w + x] = iclamp(ctx->cpu_frame[sy * GAME_W + sx].index);
            }
        }
    }
}

static void capture(void *userdata, video_screenshot_signal cb) {
    ((gu_context *)userdata)->screenshot = cb;
}
static void scene_change(void *userdata) {
    // Only called between frames, after the GPU has completed its previous list.
    clear_textures(userdata);
}
static void draw_atlas(void *userdata, bool enabled) {
    (void)userdata; (void)enabled;
}
static void move_target(void *userdata, int x, int y) {
    gu_context *ctx = userdata;
    ctx->target_x = x;
    ctx->target_y = y;
}

void psp_gu_renderer_set_callbacks(renderer *r) {
    r->is_available = is_available;
    r->get_description = get_description;
    r->get_name = get_name;
    r->create = create;
    r->destroy = destroy;
    r->setup_context = setup;
    r->get_context_state = state;
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
    r->signal_scene_change = scene_change;
    r->signal_draw_atlas = draw_atlas;
}

#ifdef PSP_STANDALONE
void psp_gu_software_renderer_set_callbacks(renderer *r) {
    psp_gu_renderer_set_callbacks(r);
    r->get_name = get_software_name;
    r->get_description = get_software_description;
    r->create = create_software;
}
#endif
