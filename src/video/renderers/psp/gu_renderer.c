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
#define TEXTURE_HINTS 512
#ifndef MAX_TEXTURE_BYTES
#define MAX_TEXTURE_BYTES (4u * 1024u * 1024u)
#endif
#define MAX_COMMANDS 4096
#define MAX_CLUTS 64
#define CLUT_HINTS 128
#define MAGIC_REMAP_ROUNDS 12

// PSP SDK's advancedSpriteBlit technique: split large textured quads into
// narrow strips so the GE texture cache can reuse adjacent texels efficiently.
// Only large textures benefit: small sprites keep the single-quad path.
#define PSP_GU_BLIT_SLICE 32
#define PSP_GU_BLIT_MIN_WIDTH 128
#define PSP_GU_BLIT_MIN_HEIGHT 64

// The GE can only blend RGBA, not remap the palette index already behind a
// sprite. Reconstruct small effect regions in indexed colors, then replace
// only their final pixels. Larger regions keep the existing fast GU path.
#define PSP_EXACT_EFFECT_MAX_PIXELS (GAME_W * GAME_H / 4)
#define PSP_EXACT_TILE_SIZE 16
#define PSP_EXACT_TILE_COLS ((GAME_W + PSP_EXACT_TILE_SIZE - 1) / PSP_EXACT_TILE_SIZE)
#define PSP_EXACT_TILE_ROWS ((GAME_H + PSP_EXACT_TILE_SIZE - 1) / PSP_EXACT_TILE_SIZE)
#define PSP_EXACT_TILE_COUNT (PSP_EXACT_TILE_COLS * PSP_EXACT_TILE_ROWS)
#define PSP_EXACT_TILE_LIMIT (PSP_EXACT_EFFECT_MAX_PIXELS / (PSP_EXACT_TILE_SIZE * PSP_EXACT_TILE_SIZE))
#define PSP_EXACT_PREFIX_MAX_COMMANDS 16

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
    uint16_t clut;
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
    unsigned long last_used_frame;
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
    // Direct-mapped accelerator; entries are always verified against the
    // immutable texture GUID and dimensions, so evicted slots stay safe.
    uint16_t texture_hints[TEXTURE_HINTS];
    draw_command commands[MAX_COMMANDS];
    unsigned command_count;
    unsigned long gpu_frames, cpu_frames, shadow_frames, remap_frames, tint_frames, additive_frames;
    unsigned frame_effects;
    vga_palette palette;
    vga_remap_tables remaps;
    _Alignas(16) uint32_t display_list[GU_LIST_WORDS];
    _Alignas(16) uint32_t clut_storage[MAX_CLUTS][VGA_PALETTE_SIZE];
    clut_variant cluts[MAX_CLUTS];
    uint8_t clut_hints[CLUT_HINTS]; /* Slot + 1; zero denotes no hint. */
    unsigned clut_count;
    unsigned cached_clut_options;
#ifdef PSP_GU_PROFILE
    unsigned long clut_reused, clut_built, clut_invalidated;
    unsigned long shadow_masks_evicted, shadow_textures_evicted;
    uint64_t profile_prepare_us, profile_commands_us, profile_sync_us, profile_vblank_us;
    unsigned long profile_sprite_draws, profile_large_draws, profile_background_draws;
    unsigned long profile_texture_binds, profile_texel_references, profile_texture_misses;
#endif
    indexed_pixel *cpu_frame;
    indexed_pixel *cpu_prefix_frame;
    draw_command cpu_prefix_commands[PSP_EXACT_PREFIX_MAX_COMMANDS];
    unsigned cpu_prefix_guids[PSP_EXACT_PREFIX_MAX_COMMANDS];
    unsigned cpu_prefix_count;
    bool cpu_prefix_valid;
#ifdef PSP_GU_PROFILE
    unsigned long cpu_prefix_hits, cpu_prefix_builds;
    uint64_t exact_tile_scan_us, exact_prefix_us, exact_replay_us, exact_convert_us;
#endif
    uint32_t *rgba_frame;
#ifdef PSP_GU_PROFILE
    unsigned long exact_effect_frames, exact_effect_pixels, skipped_effect_frames;
    uint64_t exact_effect_cpu_us;
#endif
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
    free(ctx->cpu_prefix_frame);
    free(ctx->rgba_frame);
    free(ctx);
    r->ctx = NULL;
}

static void flush_palette(gu_context *ctx) {
    vga_palette *palette;
    vga_remap_tables *remaps;
    bool changed = false;
    if(vga_state_is_palette_dirty(&palette, NULL, NULL)) {
        memcpy(&ctx->palette, palette, sizeof(ctx->palette));
        vga_state_mark_palette_flushed();
        changed = true;
    }
    if(vga_state_is_remap_dirty(&remaps)) {
        memcpy(&ctx->remaps, remaps, sizeof(ctx->remaps));
        vga_state_mark_remaps_flushed();
        ctx->cpu_prefix_valid = false;
        changed = true;
    }
    // A CLUT contains final RGB values, not indices. A palette/remap change
    // invalidates all previously derived colors, even for identical commands.
    if(changed || ctx->cached_clut_options != ctx->framebuffer_options) {
        ctx->clut_count = 0;
        memset(ctx->clut_hints, 0, sizeof(ctx->clut_hints));
        ctx->cached_clut_options = ctx->framebuffer_options;
#ifdef PSP_GU_PROFILE
        ++ctx->clut_invalidated;
#endif
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

static unsigned texture_hint_key(const surface *src) {
    uint32_t h = (uint32_t)src->guid * 2654435761u;
    h ^= (uint32_t)src->w * 2246822519u;
    h ^= (uint32_t)src->h * 3266489917u;
    return (h ^ (h >> 16)) & (TEXTURE_HINTS - 1);
}

static gu_texture *get_texture(gu_context *ctx, const surface *src) {
    if(src->w <= 0 || src->h <= 0 || !src->data) {
        return NULL;
    }
    ctx->use_clock++;
    const unsigned hint_key = texture_hint_key(src);
    const unsigned hint_slot = ctx->texture_hints[hint_key];
    if(hint_slot) {
        gu_texture *tex = &ctx->textures[hint_slot - 1];
        if(tex->data && tex->guid == src->guid && tex->w == src->w && tex->h == src->h) {
            tex->recent = ctx->use_clock;
            tex->used_frame = ctx->frame_no;
            return tex;
        }
    }
    for(int i = 0; i < MAX_TEXTURES; i++) {
        gu_texture *tex = &ctx->textures[i];
        if(tex->data && tex->guid == src->guid && tex->w == src->w && tex->h == src->h) {
            ctx->texture_hints[hint_key] = (uint16_t)(i + 1);
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
#ifdef PSP_GU_PROFILE
    ++ctx->profile_texture_misses;
#endif
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
    ctx->texture_hints[hint_key] = (uint16_t)(slot + 1);
    return tex;
}

static void prepare(void *userdata, unsigned framebuffer_options) {
    gu_context *ctx = userdata;
    ctx->command_count = 0;
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
    // Shadow masks are disposable derived data. Prefer evicting an unused
    // cached mask rather than forcing a full CPU-rendered frame when the
    // texture budget fills during a long fight. Never evict data referenced
    // by a draw command from the current frame.
    while(ctx->texture_bytes + bytes > MAX_TEXTURE_BYTES) {
        int victim = -1;
        unsigned long oldest = ~0ul;
        for(int i = 0; i < MAX_TEXTURES; i++) {
            const gu_texture *candidate = &ctx->textures[i];
            if(candidate->shadow_mask && candidate->used_frame != ctx->frame_no &&
               candidate->recent < oldest) {
                victim = i;
                oldest = candidate->recent;
            }
        }
        if(victim < 0) {
            // A scene can fill the budget with sprites that have no shadow
            // masks. Discard an entirely unused texture before giving up.
            for(int i = 0; i < MAX_TEXTURES; i++) {
                const gu_texture *candidate = &ctx->textures[i];
                if(candidate->data && candidate->used_frame != ctx->frame_no &&
                   candidate->recent < oldest) {
                    victim = i;
                    oldest = candidate->recent;
                }
            }
            if(victim < 0) return false;
            gu_texture *discard = &ctx->textures[victim];
            ctx->texture_bytes -= discard->bytes;
            free(discard->data);
            free(discard->shadow_mask);
            memset(discard, 0, sizeof(*discard));
#ifdef PSP_GU_PROFILE
            ++ctx->shadow_textures_evicted;
#endif
            continue;
        }
        gu_texture *discard = &ctx->textures[victim];
        const size_t freed = (size_t)discard->pitch * discard->tex_h;
        free(discard->shadow_mask);
        discard->shadow_mask = NULL;
        discard->bytes -= freed;
        ctx->texture_bytes -= freed;
#ifdef PSP_GU_PROFILE
        ++ctx->shadow_masks_evicted;
#endif
    }
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

static bool clut_matches(const clut_variant *entry, const draw_command *cmd) {
    return entry->transparent == cmd->transparent &&
           entry->remap_offset == cmd->remap_offset &&
           entry->remap_rounds == cmd->remap_rounds &&
           entry->palette_offset == cmd->palette_offset &&
           entry->palette_limit == cmd->palette_limit &&
           entry->options == cmd->options;
}

static unsigned clut_hint_key(const draw_command *cmd) {
    uint32_t h = (uint32_t)cmd->transparent * 2654435761u;
    h ^= (uint32_t)cmd->remap_offset * 2246822519u;
    h ^= (uint32_t)cmd->remap_rounds * 3266489917u;
    h ^= (uint32_t)cmd->palette_offset * 668265263u;
    h ^= (uint32_t)cmd->palette_limit * 374761393u;
    h ^= cmd->options * 1274126177u;
    return (h ^ (h >> 16)) & (CLUT_HINTS - 1);
}

static int get_clut(gu_context *ctx, const draw_command *cmd) {
    const unsigned hint_key = clut_hint_key(cmd);
    const unsigned hint_slot = ctx->clut_hints[hint_key];
    if(hint_slot && hint_slot <= ctx->clut_count) {
        clut_variant *entry = &ctx->cluts[hint_slot - 1];
        if(clut_matches(entry, cmd)) {
            entry->last_used_frame = ctx->frame_no;
#ifdef PSP_GU_PROFILE
            ++ctx->clut_reused;
#endif
            return (int)(hint_slot - 1);
        }
    }
    for(unsigned i = 0; i < ctx->clut_count; i++) {
        clut_variant *entry = &ctx->cluts[i];
        if(clut_matches(entry, cmd)) {
            entry->last_used_frame = ctx->frame_no;
            ctx->clut_hints[hint_key] = (uint8_t)(i + 1);
#ifdef PSP_GU_PROFILE
            ++ctx->clut_reused;
#endif
            return (int)i;
        }
    }
    unsigned slot;
    if(ctx->clut_count < MAX_CLUTS) {
        slot = ctx->clut_count++;
    } else {
        // Palette variants can accumulate across frames. Replace one that is
        // not referenced by any draw command from THIS frame, since those
        // draw commands hold direct indices into clut_storage[].
        unsigned long oldest_frame = ~0ul;
        int victim = -1;
        for(unsigned i = 0; i < MAX_CLUTS; i++) {
            if(ctx->cluts[i].last_used_frame != ctx->frame_no &&
               ctx->cluts[i].last_used_frame < oldest_frame) {
                oldest_frame = ctx->cluts[i].last_used_frame;
                victim = (int)i;
            }
        }
        if(victim < 0) {
#ifdef PSP_GU_PROFILE
            log_warn("PSP GU CLUT saturated: frame=%lu draws=%u entries=%u",
                     ctx->frame_no, ctx->command_count, ctx->clut_count);
#endif
            return -1; // Preserve exact CPU fallback on saturation.
        }
        slot = (unsigned)victim;
    }
    clut_variant *v = &ctx->cluts[slot];
    v->last_used_frame = ctx->frame_no;
    ctx->clut_hints[hint_key] = (uint8_t)(slot + 1);
#ifdef PSP_GU_PROFILE
    ++ctx->clut_built;
#endif
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

static omf_rect viewport_bounds(const gu_context *ctx) {
    omf_rect r = {0, 0, SCREEN_W, SCREEN_H};
    if(ctx->aspect == 0) {
        r.w = SCREEN_H * 4 / 3;
        r.x = (SCREEN_W - r.w) / 2;
    }
    return r;
}

static omf_rect viewport(const gu_context *ctx) {
    omf_rect r = viewport_bounds(ctx);
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

static void gpu_draw(gu_context *ctx, const draw_command *cmd, const omf_rect *vp,
                     bool load_clut, bool bind_texture) {
    gu_texture *tex = &ctx->textures[cmd->texture];
#ifdef PSP_GU_PROFILE
    ++ctx->profile_sprite_draws;
    if(tex->w >= 128 && tex->h >= 64) ++ctx->profile_large_draws;
    if(cmd->dst.w >= GAME_W / 2 && cmd->dst.h >= GAME_H / 2)
        ++ctx->profile_background_draws;
    ctx->profile_texel_references += (unsigned long)tex->w * (unsigned long)tex->h;
    if(bind_texture) ++ctx->profile_texture_binds;
#endif
    if(load_clut) {
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, ctx->clut_storage[cmd->clut]);
    }
    if(bind_texture) {
#ifdef PSP_GU_APPROX_EFFECTS
        if(cmd->options & SPRITE_SHADOW) {
            // Shadow masks are separate T8 textures even if the GUID matches.
            sceGuTexImage(0, tex->pitch, tex->tex_h, tex->pitch, tex->shadow_mask);
        } else {
            sceGuTexImage(0, tex->pitch, tex->tex_h, tex->pitch, tex->data);
        }
#else
        sceGuTexImage(0, tex->pitch, tex->tex_h, tex->pitch, tex->data);
#endif
    }
    sceGuColor(((unsigned)(uint8_t)cmd->opacity << 24) | 0x00ffffffu);
    // Preserve the GL3 logical coordinate system; the PSP scales at the end.
    omf_rect dst = {
        vp->x + cmd->dst.x * vp->w / GAME_W,
        vp->y + cmd->dst.y * vp->h / GAME_H,
        (cmd->dst.w * vp->w + GAME_W / 2) / GAME_W,
        (cmd->dst.h * vp->h + GAME_H / 2) / GAME_H
    };
    if(tex->w >= PSP_GU_BLIT_MIN_WIDTH && tex->h >= PSP_GU_BLIT_MIN_HEIGHT && dst.w > 0) {
        const unsigned strips = (unsigned)(tex->w + PSP_GU_BLIT_SLICE - 1) / PSP_GU_BLIT_SLICE;
        gu_sprite_vertex *v = sceGuGetMemory(sizeof(gu_sprite_vertex) * 2 * strips);
        for(unsigned i = 0; i < strips; ++i) {
            const int left = (int)i * PSP_GU_BLIT_SLICE;
            const int right = imin(left + PSP_GU_BLIT_SLICE, tex->w);
            const float x0 = dst.x + (float)left * dst.w / tex->w;
            const float x1 = dst.x + (float)right * dst.w / tex->w;
            const float u0 = (cmd->flip & FLIP_HORIZONTAL) ? tex->w - left : left;
            const float u1 = (cmd->flip & FLIP_HORIZONTAL) ? tex->w - right : right;
            const float v0 = (cmd->flip & FLIP_VERTICAL) ? tex->h : 0;
            const float v1 = (cmd->flip & FLIP_VERTICAL) ? 0 : tex->h;
            v[i * 2] = (gu_sprite_vertex){.u=u0, .v=v0, .x=x0, .y=dst.y, .z=0};
            v[i * 2 + 1] = (gu_sprite_vertex){.u=u1, .v=v1,
                                             .x=x1, .y=dst.y + dst.h, .z=0};
        }
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       strips * 2, 0, v);
        return;
    }
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

static void cpu_draw_clipped(gu_context *ctx, const draw_command *cmd, const omf_rect *clip) {
    const gu_texture *tex = &ctx->textures[cmd->texture];
    const omf_rect *r = &cmd->dst;
    const int x0 = imax(clip->x, imax(0, r->x));
    const int y0 = imax(clip->y, imax(0, r->y));
    const int x1 = imin(clip->x + clip->w, imin(GAME_W, r->x + r->w));
    const int y1 = imin(clip->y + clip->h, imin(GAME_H, r->y + r->h));
    if(r->w <= 0 || r->h <= 0) return;
    // The localized exact path evaluates the same x coordinate on every
    // row of a 16x16 tile. Computing source coordinates once avoids hundreds
    // of expensive R4000 integer divisions for each intersecting sprite.
    int cached_x[PSP_EXACT_TILE_SIZE];
    const bool use_cached_x = x1 > x0 && x1 - x0 <= PSP_EXACT_TILE_SIZE;
    if(use_cached_x) {
        for(int x = x0; x < x1; ++x) {
            int sx = (x - r->x) * tex->w / r->w;
            if(cmd->flip & FLIP_HORIZONTAL) sx = tex->w - 1 - sx;
            cached_x[x - x0] = sx;
        }
    }
    for(int y = y0; y < y1; y++) {
        int sy = (y - r->y) * tex->h / r->h;
        if(cmd->flip & FLIP_VERTICAL) sy = tex->h - 1 - sy;
        for(int x = x0; x < x1; x++) {
            int sx = use_cached_x ? cached_x[x - x0] : (x - r->x) * tex->w / r->w;
            if(!use_cached_x && (cmd->flip & FLIP_HORIZONTAL)) sx = tex->w - 1 - sx;
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

static void cpu_draw(gu_context *ctx, const draw_command *cmd) {
    const omf_rect full = {0, 0, GAME_W, GAME_H};
    cpu_draw_clipped(ctx, cmd, &full);
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

#ifdef PSP_GU_APPROX_EFFECTS
// The logo underneath the intro lightning is mostly unchanged. Cache all
// draws preceding its first destination-index effect, and only replay the
// changing suffix in the affected tiles. Match every field, not a hash: an
// accidental collision would incorrectly reuse indexed destination pixels.
static bool same_prefix_command(const draw_command *a, const draw_command *b) {
    return a->texture == b->texture && a->transparent == b->transparent &&
           a->dst.x == b->dst.x && a->dst.y == b->dst.y &&
           a->dst.w == b->dst.w && a->dst.h == b->dst.h &&
           a->remap_offset == b->remap_offset && a->remap_rounds == b->remap_rounds &&
           a->palette_offset == b->palette_offset && a->palette_limit == b->palette_limit &&
           a->opacity == b->opacity && a->flip == b->flip && a->options == b->options;
}

static bool cpu_prepare_exact_prefix(gu_context *ctx) {
    unsigned prefix = 0;
    while(prefix < ctx->command_count &&
          !ctx->commands[prefix].remap_rounds &&
          !(ctx->commands[prefix].options & (SPRITE_SHADOW | SPRITE_DARK_TINT | SPRITE_INDEX_ADD))) {
        ++prefix;
    }
    if(prefix == 0 || prefix > PSP_EXACT_PREFIX_MAX_COMMANDS) {
        ctx->cpu_prefix_valid = false;
        return false;
    }
    bool hit = ctx->cpu_prefix_valid && ctx->cpu_prefix_count == prefix;
    for(unsigned i = 0; hit && i < prefix; ++i) {
        hit = same_prefix_command(&ctx->cpu_prefix_commands[i], &ctx->commands[i]) &&
              ctx->cpu_prefix_guids[i] == ctx->textures[ctx->commands[i].texture].guid;
    }
    if(hit) {
#ifdef PSP_GU_PROFILE
        ++ctx->cpu_prefix_hits;
#endif
        return true;
    }
    if(!ctx->cpu_prefix_frame)
        ctx->cpu_prefix_frame = calloc(GAME_W * GAME_H, sizeof(indexed_pixel));
    if(!ctx->cpu_prefix_frame) return false;

    indexed_pixel *saved_frame = ctx->cpu_frame;
    ctx->cpu_frame = ctx->cpu_prefix_frame;
    memset(ctx->cpu_prefix_frame, 0, GAME_W * GAME_H * sizeof(indexed_pixel));
    const omf_rect full = {0, 0, GAME_W, GAME_H};
    for(unsigned i = 0; i < prefix; ++i)
        cpu_draw_clipped(ctx, &ctx->commands[i], &full);
    ctx->cpu_frame = saved_frame;
    for(unsigned i = 0; i < prefix; ++i) {
        ctx->cpu_prefix_commands[i] = ctx->commands[i];
        ctx->cpu_prefix_guids[i] = ctx->textures[ctx->commands[i].texture].guid;
    }
    ctx->cpu_prefix_count = prefix;
    ctx->cpu_prefix_valid = true;
#ifdef PSP_GU_PROFILE
    ++ctx->cpu_prefix_builds;
#endif
    return true;
}

// Only reconstruct the damaged rectangle. A full indexed replay remains the
// fallback for large effects, screenshots, and genuinely unsupported draws.
// Replay *all* commands in submission order: a foreground sprite can cover
// an earlier destination-index effect, and its final pixels must win.
static bool cpu_replay_region(gu_context *ctx, const omf_rect *region) {
    if(!ctx->cpu_frame) ctx->cpu_frame = calloc(GAME_W * GAME_H, sizeof(indexed_pixel));
    if(!ctx->cpu_frame) return false;
    for(int y = region->y; y < region->y + region->h; ++y) {
        indexed_pixel *dst = ctx->cpu_frame + y * GAME_W + region->x;
        if(ctx->cpu_prefix_valid)
            memcpy(dst, ctx->cpu_prefix_frame + y * GAME_W + region->x,
                   (size_t)region->w * sizeof(indexed_pixel));
        else
            memset(dst, 0, (size_t)region->w * sizeof(indexed_pixel));
    }
    for(unsigned i = ctx->cpu_prefix_valid ? ctx->cpu_prefix_count : 0;
        i < ctx->command_count; ++i)
        cpu_draw_clipped(ctx, &ctx->commands[i], region);
    return true;
}

static omf_rect exact_tile_bounds(int col, int row) {
    const int x = col * PSP_EXACT_TILE_SIZE;
    const int y = row * PSP_EXACT_TILE_SIZE;
    return (omf_rect){x, y, imin(PSP_EXACT_TILE_SIZE, GAME_W - x),
                              imin(PSP_EXACT_TILE_SIZE, GAME_H - y)};
}

// A lightning or shadow image can be mostly transparent. Correct only the
// 16x16 tiles with an actual effect sample rather than its entire bounding
// box. Tiles are disjoint, so their final indexed results can be overlaid in
// one GU draw without altering draw order elsewhere on the screen.
static unsigned exact_effect_tiles(const gu_context *ctx, uint8_t marked[PSP_EXACT_TILE_COUNT]) {
    memset(marked, 0, PSP_EXACT_TILE_COUNT);
    // Only the electrical reveal currently has a validated exact path.
    // Ordinary combat remaps are frequent and too expensive to replay on
    // the EE-like PSP CPU; retain their fast GU approximation for now.
    if(!(ctx->framebuffer_options & FBUFOPT_BRIGHT_DESTINATION_REMAP)) return 0;
    unsigned count = 0;
    for(unsigned i = 0; i < ctx->command_count; ++i) {
        const draw_command *cmd = &ctx->commands[i];
        // Shadows and dark tint appear on most combat frames: reconstructing
        // them tile by tile is substantially slower than the fast GU mask.
        // Target destination-index remaps first (notably intro lightning).
        if(cmd->remap_rounds <= 0) continue;
        const gu_texture *tex = &ctx->textures[cmd->texture];
        const omf_rect *r = &cmd->dst;
        if(r->w <= 0 || r->h <= 0) continue;
        const int x0 = imax(0, cmd->dst.x), y0 = imax(0, cmd->dst.y);
        const int x1 = imin(GAME_W, cmd->dst.x + cmd->dst.w);
        const int y1 = imin(GAME_H, cmd->dst.y + cmd->dst.h);
        if(x0 >= x1 || y0 >= y1) continue;
        for(int ty = y0 / PSP_EXACT_TILE_SIZE; ty <= (y1 - 1) / PSP_EXACT_TILE_SIZE; ++ty) {
            for(int tx = x0 / PSP_EXACT_TILE_SIZE; tx <= (x1 - 1) / PSP_EXACT_TILE_SIZE; ++tx) {
                const int tile = ty * PSP_EXACT_TILE_COLS + tx;
                if(marked[tile]) continue;
                const omf_rect bounds = exact_tile_bounds(tx, ty);
                const int sx0 = imax(x0, bounds.x), sx1 = imin(x1, bounds.x + bounds.w);
                const int sy0 = imax(y0, bounds.y), sy1 = imin(y1, bounds.y + bounds.h);
                bool covered = false;
                for(int y = sy0; y < sy1 && !covered; ++y) {
                    int sy = (y - r->y) * tex->h / r->h;
                    if(cmd->flip & FLIP_VERTICAL) sy = tex->h - 1 - sy;
                    for(int x = sx0; x < sx1; ++x) {
                        int sx = (x - r->x) * tex->w / r->w;
                        if(cmd->flip & FLIP_HORIZONTAL) sx = tex->w - 1 - sx;
                        if(cmd->options & SPRITE_SHADOW) {
                            for(int k = -1; k <= 2; ++k) {
                                const int yy = sy + k;
                                if(yy >= 0 && yy < tex->h &&
                                   tex->data[yy * tex->pitch + sx] != cmd->transparent) {
                                    covered = true;
                                    break;
                                }
                            }
                        } else {
                            covered = tex->data[sy * tex->pitch + sx] != cmd->transparent;
                        }
                        if(covered) break;
                    }
                }
                if(covered) {
                    marked[tile] = 1;
                    if(++count > PSP_EXACT_TILE_LIMIT) return count;
                }
            }
        }
    }
    return count;
}
#endif

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

#ifdef PSP_GU_APPROX_EFFECTS
// Convert disjoint affected tiles before GU starts consuming the texture.
// A single 512x256 scratch texture holds their absolute logical coordinates.
static bool cpu_prepare_exact_tiles(gu_context *ctx, const uint8_t marked[PSP_EXACT_TILE_COUNT]) {
    if(!ctx->rgba_frame) ctx->rgba_frame = memalign(16, 512 * 256 * sizeof(uint32_t));
    if(!ctx->rgba_frame) return false;
#ifdef PSP_GU_PROFILE
    uint64_t stage_start = sceKernelGetSystemTimeWide();
#endif
    cpu_prepare_exact_prefix(ctx);
#ifdef PSP_GU_PROFILE
    ctx->exact_prefix_us += sceKernelGetSystemTimeWide() - stage_start;
#endif
    for(int tile = 0; tile < PSP_EXACT_TILE_COUNT; ++tile) {
        if(!marked[tile]) continue;
        const omf_rect r = exact_tile_bounds(tile % PSP_EXACT_TILE_COLS, tile / PSP_EXACT_TILE_COLS);
#ifdef PSP_GU_PROFILE
        stage_start = sceKernelGetSystemTimeWide();
#endif
        if(!cpu_replay_region(ctx, &r)) return false;
#ifdef PSP_GU_PROFILE
        ctx->exact_replay_us += sceKernelGetSystemTimeWide() - stage_start;
        stage_start = sceKernelGetSystemTimeWide();
#endif
        for(int y = r.y; y < r.y + r.h; ++y) {
            for(int x = r.x; x < r.x + r.w; ++x) {
                const int index = cpu_resolve(ctx, &ctx->cpu_frame[y * GAME_W + x]);
                const vga_color c = ctx->palette.colors[index];
                ctx->rgba_frame[y * 512 + x] = 0xff000000u |
                    ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
            }
        }
#ifdef PSP_GU_PROFILE
        ctx->exact_convert_us += sceKernelGetSystemTimeWide() - stage_start;
#endif
    }
    // One coherent writeback is cheaper than hundreds of 16-pixel row flushes
    // on physical PSP. Only corrected tiles are sampled by the following GU
    // draw, so untouched scratch-buffer pixels are irrelevant.
    sceKernelDcacheWritebackRange(ctx->rgba_frame, 512 * 256 * sizeof(uint32_t));
    return true;
}

// Overlay final RGBA colors after the indexed GU sprite stream. Non-effect
// tiles are untouched, and all corrected tiles share a single GE draw call.
static void gpu_draw_exact_tiles(gu_context *ctx, const omf_rect *vp,
                                 const uint8_t marked[PSP_EXACT_TILE_COUNT], unsigned count) {
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, 512, 256, 512, ctx->rgba_frame);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuColor(0xffffffff);
    gu_sprite_vertex *v = sceGuGetMemory(sizeof(*v) * 2 * count);
    unsigned i = 0;
    for(int tile = 0; tile < PSP_EXACT_TILE_COUNT; ++tile) {
        if(!marked[tile]) continue;
        const omf_rect r = exact_tile_bounds(tile % PSP_EXACT_TILE_COLS, tile / PSP_EXACT_TILE_COLS);
        const float x0 = vp->x + (float)r.x * vp->w / GAME_W;
        const float y0 = vp->y + (float)r.y * vp->h / GAME_H;
        const float x1 = vp->x + (float)(r.x + r.w) * vp->w / GAME_W;
        const float y1 = vp->y + (float)(r.y + r.h) * vp->h / GAME_H;
        v[i++] = (gu_sprite_vertex){.u=r.x, .v=r.y, .x=x0, .y=y0, .z=0};
        v[i++] = (gu_sprite_vertex){.u=r.x + r.w, .v=r.y + r.h,
                                    .x=x1, .y=y1, .z=0};
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   count * 2, 0, v);
}
#endif

static void finish(void *userdata) {
    gu_context *ctx = userdata;
#ifdef PSP_GU_PROFILE
    const uint64_t gu_start_ms = omf_time_ms();
    const uint64_t gu_start_us = sceKernelGetSystemTimeWide();
#endif
    flush_palette(ctx);
#ifdef PSP_GU_APPROX_EFFECTS
    uint8_t exact_tiles[PSP_EXACT_TILE_COUNT];
    bool exact_tiles_ready = false;
#ifdef PSP_GU_PROFILE
    const uint64_t scan_start = sceKernelGetSystemTimeWide();
#endif
    const unsigned exact_tile_count = ctx->needs_cpu ? 0 : exact_effect_tiles(ctx, exact_tiles);
#ifdef PSP_GU_PROFILE
    ctx->exact_tile_scan_us += sceKernelGetSystemTimeWide() - scan_start;
#endif
    if(exact_tile_count) {
        if(exact_tile_count <= PSP_EXACT_TILE_LIMIT) {
#ifdef PSP_GU_PROFILE
            const uint64_t exact_start = sceKernelGetSystemTimeWide();
#endif
            exact_tiles_ready = cpu_prepare_exact_tiles(ctx, exact_tiles);
#ifdef PSP_GU_PROFILE
            if(exact_tiles_ready) {
                ++ctx->exact_effect_frames;
                ctx->exact_effect_pixels += exact_tile_count * PSP_EXACT_TILE_SIZE * PSP_EXACT_TILE_SIZE;
                ctx->exact_effect_cpu_us += sceKernelGetSystemTimeWide() - exact_start;
            }
#endif
        } else {
#ifdef PSP_GU_PROFILE
            ++ctx->skipped_effect_frames;
#endif
        }
    }
#endif
    // CLUT variants cannot represent destination-index remap. Replay precisely
    // through the indexed CPU buffer for such frames, not a lossy RGB overlay.
    if(!ctx->needs_cpu) {
        for(unsigned i = 0; i < ctx->command_count; i++) {
            int clut = get_clut(ctx, &ctx->commands[i]);
            if(clut < 0) {
#ifdef PSP_GU_PROFILE
                log_warn("PSP GU CPU fallback: CLUT lookup failed frame=%lu draw=%u",
                         ctx->frame_no, i);
#endif
                ctx->needs_cpu = true;
                break;
            }
            ctx->commands[i].clut = (uint16_t)clut;
#ifdef PSP_GU_APPROX_EFFECTS
            const draw_command *cmd = &ctx->commands[i];
            if(cmd->options & SPRITE_SHADOW) {
                gu_texture *tex = &ctx->textures[cmd->texture];
                if(!ensure_shadow_mask(ctx, tex, cmd->transparent)) {
#ifdef PSP_GU_PROFILE
                    log_warn("PSP GU CPU fallback: shadow mask budget frame=%lu draw=%u",
                             ctx->frame_no, i);
#endif
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
#ifdef PSP_GU_PROFILE
    const uint64_t gu_prepared_us = sceKernelGetSystemTimeWide();
#endif
    sceGuStart(GU_DIRECT, ctx->display_list);
    // A prior frame may have clipped to 4:3. Clear the entire display first
    // so the side borders stay black even during camera shake or mode changes.
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
    sceGuClearColor(0xff000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    // GU sprite coordinates can extend beyond the 320x200 logical game area.
    // Clip to the unshifted 4:3 viewport instead of leaking into pillarboxes.
    // The NATIVE aspect option intentionally uses the whole PSP screen.
    const omf_rect clip = viewport_bounds(ctx);
    sceGuScissor(clip.x, clip.y, clip.w, clip.h);
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
        int last_texture = -1;
        bool last_shadow = false;
        sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        for(unsigned i = 0; i < ctx->command_count; i++) {
            const draw_command *cmd = &ctx->commands[i];
            // Palette changes are handled with GU CLUT loads, not framebuffer
            // conversions or texture uploads.
            int clut = cmd->clut;
            bool change = clut != last_clut;
            if(change) last_clut = clut;
            const bool shadow = !!(cmd->options & SPRITE_SHADOW);
            const bool bind_texture = cmd->texture != last_texture || shadow != last_shadow;
            if(bind_texture) {
                last_texture = cmd->texture;
                last_shadow = shadow;
            }
            gpu_draw(ctx, cmd, &vp, change, bind_texture);
        }
#ifdef PSP_GU_APPROX_EFFECTS
        if(exact_tiles_ready) gpu_draw_exact_tiles(ctx, &vp, exact_tiles, exact_tile_count);
#endif
    }
    sceGuFinish();
#ifdef PSP_GU_PROFILE
    const uint64_t gu_commands_ms = omf_time_ms();
    const uint64_t gu_commands_us = sceKernelGetSystemTimeWide();
#endif
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
#ifdef PSP_GU_PROFILE
    const uint64_t gu_sync_ms = omf_time_ms();
    const uint64_t gu_sync_us = sceKernelGetSystemTimeWide();
#endif
    // The PSP display refreshes at 60 Hz. Even if desktop settings request
    // VSync OFF, cap native presentation to VBlank: otherwise the engine can
    // run thousands of frames per second in PPSSPP and animate too quickly.
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
#ifdef PSP_GU_PROFILE
    const uint64_t gu_swap_ms = omf_time_ms();
    const uint64_t gu_swap_us = sceKernelGetSystemTimeWide();
    ctx->profile_prepare_us += gu_prepared_us - gu_start_us;
    ctx->profile_commands_us += gu_commands_us - gu_prepared_us;
    ctx->profile_sync_us += gu_sync_us - gu_commands_us;
    ctx->profile_vblank_us += gu_swap_us - gu_sync_us;
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
        log_info("PSP GU CLUT cache (600 frames): reused=%lu built=%lu invalidated=%lu",
                 ctx->clut_reused, ctx->clut_built, ctx->clut_invalidated);
        log_info("PSP GU shadow budget (600 frames): masks-evicted=%lu textures-evicted=%lu",
                 ctx->shadow_masks_evicted, ctx->shadow_textures_evicted);
        log_info("PSP GU timings (600 frames): prep=%lluus commands=%lluus sync=%lluus vblank=%lluus",
                 (unsigned long long)(ctx->profile_prepare_us / 600),
                 (unsigned long long)(ctx->profile_commands_us / 600),
                 (unsigned long long)(ctx->profile_sync_us / 600),
                 (unsigned long long)(ctx->profile_vblank_us / 600));
        log_info("PSP GU textures (600 frames): draws=%lu large=%lu background=%lu binds=%lu texel-refs=%lu new-textures=%lu",
                 ctx->profile_sprite_draws, ctx->profile_large_draws,
                 ctx->profile_background_draws, ctx->profile_texture_binds,
                 ctx->profile_texel_references, ctx->profile_texture_misses);
#ifdef PSP_GU_APPROX_EFFECTS
        log_info("PSP GU exact effects (600 frames): exact=%lu pixels=%lu skipped=%lu replay-us=%llu",
                 ctx->exact_effect_frames, ctx->exact_effect_pixels, ctx->skipped_effect_frames,
                 (unsigned long long)ctx->exact_effect_cpu_us);
        ctx->exact_effect_frames = ctx->exact_effect_pixels = ctx->skipped_effect_frames = 0;
        ctx->exact_effect_cpu_us = 0;
        log_info("PSP GU indexed prefix (600 frames): hits=%lu builds=%lu",
                 ctx->cpu_prefix_hits, ctx->cpu_prefix_builds);
        log_info("PSP GU exact stage (600 frames): scan=%lluus prefix=%lluus replay=%lluus convert=%lluus",
                 (unsigned long long)ctx->exact_tile_scan_us,
                 (unsigned long long)ctx->exact_prefix_us,
                 (unsigned long long)ctx->exact_replay_us,
                 (unsigned long long)ctx->exact_convert_us);
        ctx->cpu_prefix_hits = ctx->cpu_prefix_builds = 0;
        ctx->exact_tile_scan_us = ctx->exact_prefix_us = ctx->exact_replay_us = ctx->exact_convert_us = 0;
#endif
        ctx->clut_reused = ctx->clut_built = ctx->clut_invalidated = 0;
        ctx->shadow_masks_evicted = ctx->shadow_textures_evicted = 0;
        ctx->profile_prepare_us = ctx->profile_commands_us = 0;
        ctx->profile_sync_us = ctx->profile_vblank_us = 0;
        ctx->profile_sprite_draws = ctx->profile_large_draws = ctx->profile_background_draws = 0;
        ctx->profile_texture_binds = ctx->profile_texel_references = ctx->profile_texture_misses = 0;
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
    gu_context *ctx = userdata;
    clear_textures(ctx);
    ctx->clut_count = 0;
    memset(ctx->clut_hints, 0, sizeof(ctx->clut_hints));
    ctx->cpu_prefix_valid = false;
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
