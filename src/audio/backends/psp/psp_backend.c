/*
 * Native PSP PCM backend. libxmp renders music, SOUNDS.DAT provides borrowed
 * unsigned 8-bit mono samples, and a single PSP audio thread mixes both.
 * SDL_mixer remains a separately selectable backend for A/B regression.
 */
#include "audio/backends/psp/psp_backend.h"
#include "utils/allocator.h"
#include "utils/log.h"

#include <malloc.h>
#include <pspaudio.h>
#include <pspthreadman.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PSP_MIX_RATE 44100
#define PSP_MIX_FRAMES 512
#define PSP_PCM_CACHE_BYTES (4u * 1024u * 1024u)
#define PSP_PCM_CACHE_ENTRIES 256

typedef struct {
    const char *source;
    size_t length;
    int frequency;
    int16_t *data;
    size_t frames;
} pcm_entry;

typedef struct {
    const int16_t *pcm;
    int16_t *owned_pcm;
    size_t frames;
    size_t cursor;
    int volume;
    int pan;
    int fade_in_frames;
    int fade_out_frames;
    int fade_out_left;
} voice;

typedef struct {
    int output_channel;
    SceUID lock;
    SceUID thread;
    volatile int running;
    float music_volume;
    int sound_volume;
    int resampler;
    music_source music;
    voice voices[SOUND_CHANNEL_COUNT];
    pcm_entry cache[PSP_PCM_CACHE_ENTRIES];
    unsigned cache_count, cache_hits, cache_misses;
    size_t cache_bytes;
    unsigned long mixed_blocks;
    unsigned long missed_blocks;
    unsigned long music_blocks;
    unsigned long effect_blocks;
    unsigned long sound_starts;
    int16_t *output;
} psp_audio_context;

static psp_audio_context *active_context;

static int clamp_int(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

static float clamp_volume(float value) {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static int lock_audio(psp_audio_context *ctx) {
    return sceKernelWaitSema(ctx->lock, 1, NULL) >= 0;
}

static void unlock_audio(psp_audio_context *ctx) {
    sceKernelSignalSema(ctx->lock, 1);
}

static void release_voice(voice *v) {
    free(v->owned_pcm);
    memset(v, 0, sizeof(*v));
}

/* Data is unsigned 8-bit mono. Resample to 44.1 kHz once per distinct pitch. */
static int16_t *resample_mono(const sound_source *src, size_t *frames) {
    if(!src->buf || src->len == 0 || src->freq <= 0) return NULL;
    const uint64_t count = ((uint64_t)src->len * PSP_MIX_RATE) / (unsigned)src->freq;
    if(count == 0 || count > SIZE_MAX / sizeof(int16_t) || count > 8u * 1024u * 1024u) return NULL;
    int16_t *pcm = malloc((size_t)count * sizeof(int16_t));
    if(!pcm) return NULL;
    const uint8_t *input = (const uint8_t *)src->buf;
    const uint64_t step = ((uint64_t)(unsigned)src->freq << 32) / PSP_MIX_RATE;
    uint64_t phase = 0;
    for(size_t i = 0; i < (size_t)count; ++i, phase += step) {
        size_t pos = (size_t)(phase >> 32);
        if(pos >= src->len) pos = src->len - 1;
        size_t next = pos + 1 < src->len ? pos + 1 : pos;
        const int a = (int)input[pos] - 128;
        const int b = (int)input[next] - 128;
        const int fraction = (int)((phase >> 16) & 0xffff);
        pcm[i] = (int16_t)(a * 256 + ((b - a) * fraction) / 256);
    }
    *frames = (size_t)count;
    return pcm;
}

static int16_t clip_sample(int value) {
    return (int16_t)clamp_int(value, -32768, 32767);
}

/* The lock protects both the music callback lifetime and voice PCM pointers. */
static void mix_block(psp_audio_context *ctx) {
    if(!lock_audio(ctx)) {
        memset(ctx->output, 0, PSP_MIX_FRAMES * 2 * sizeof(int16_t));
        ++ctx->missed_blocks;
        return;
    }

    memset(ctx->output, 0, PSP_MIX_FRAMES * 2 * sizeof(int16_t));
    if(ctx->music.context) ++ctx->music_blocks;
    music_source_render(&ctx->music, (char *)ctx->output,
                        PSP_MIX_FRAMES * 2 * sizeof(int16_t));

    bool has_effects = false;
    int left_gain[SOUND_CHANNEL_COUNT] = {0};
    int right_gain[SOUND_CHANNEL_COUNT] = {0};
    for(int ch = 0; ch < SOUND_CHANNEL_COUNT; ++ch) {
        voice *v = &ctx->voices[ch];
        if(!v->pcm || v->cursor >= v->frames) continue;
        has_effects = true;
        const int left_pan = v->pan > 0 ? 100 - v->pan : 100;
        const int right_pan = v->pan < 0 ? 100 + v->pan : 100;
        // Q15 gains: calculate the divisions once per block, not per PCM frame.
        const int64_t gain = (int64_t)v->volume * ctx->sound_volume * 32768;
        left_gain[ch] = (int)(gain * left_pan / (127 * 127 * 100));
        right_gain[ch] = (int)(gain * right_pan / (127 * 127 * 100));
    }
    if(has_effects) ++ctx->effect_blocks;
    for(unsigned frame = 0; frame < PSP_MIX_FRAMES; ++frame) {
        int left = ctx->output[frame * 2];
        int right = ctx->output[frame * 2 + 1];
        for(int ch = 0; ch < SOUND_CHANNEL_COUNT; ++ch) {
            voice *v = &ctx->voices[ch];
            if(!v->pcm || v->cursor >= v->frames) continue;
            int lg = left_gain[ch];
            int rg = right_gain[ch];
            if(v->fade_in_frames > 0 && v->cursor < (size_t)v->fade_in_frames) {
                const int fade = (int)((int64_t)v->cursor * 32768 / v->fade_in_frames);
                lg = lg * fade >> 15;
                rg = rg * fade >> 15;
            }
            if(v->fade_out_frames > 0) {
                const int fade = (int)((int64_t)v->fade_out_left * 32768 / v->fade_out_frames);
                lg = lg * fade >> 15;
                rg = rg * fade >> 15;
                if(--v->fade_out_left <= 0) {
                    v->cursor = v->frames;
                    continue;
                }
            }
            const int sample = v->pcm[v->cursor++];
            left += (sample * lg) >> 15;
            right += (sample * rg) >> 15;
        }
        ctx->output[frame * 2] = clip_sample(left);
        ctx->output[frame * 2 + 1] = clip_sample(right);
    }
    // Expired uncached effects must not retain their PCM until replacement.
    for(int ch = 0; ch < SOUND_CHANNEL_COUNT; ++ch) {
        voice *v = &ctx->voices[ch];
        if(v->pcm && v->cursor >= v->frames) release_voice(v);
    }
    ++ctx->mixed_blocks;
    unlock_audio(ctx);
}

static int audio_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    psp_audio_context *ctx = active_context;
    while(ctx && ctx->running) {
        mix_block(ctx);
        const int result = sceAudioOutputBlocking(ctx->output_channel, PSP_AUDIO_VOLUME_MAX, ctx->output);
        if(result < 0) {
            ++ctx->missed_blocks;
            sceKernelDelayThread(1000);
        }
    }
    return 0;
}

static bool available(void) { return true; }
static const char *name(void) { return "PSP native"; }
static const char *description(void) { return "Native PSP sceAudio PCM mixer"; }
static const audio_sample_rate rates[] = {{PSP_MIX_RATE, true, "44100Hz (PSP hardware)"}};
static unsigned rate_list(const audio_sample_rate **out) {
    *out = rates;
    return 1;
}
static void get_info(void *ptr, unsigned *rate, unsigned *channels, unsigned *resampler) {
    psp_audio_context *ctx = ptr;
    if(rate) *rate = PSP_MIX_RATE;
    if(channels) *channels = 2;
    if(resampler) *resampler = (unsigned)ctx->resampler;
}
static void create(audio_backend *backend) {
    backend->ctx = omf_calloc(1, sizeof(psp_audio_context));
}
static void destroy(audio_backend *backend) {
    omf_free(backend->ctx);
}

static bool setup(void *ptr, unsigned rate, bool mono, int resampler,
                  float music_volume, float sound_volume) {
    psp_audio_context *ctx = ptr;
    (void)rate; (void)mono;
    ctx->output_channel = -1;
    ctx->lock = -1;
    ctx->thread = -1;
    ctx->resampler = resampler;
    ctx->music_volume = clamp_volume(music_volume);
    ctx->sound_volume = (int)(clamp_volume(sound_volume) * 127.0f);
    ctx->output = memalign(64, PSP_MIX_FRAMES * 2 * sizeof(int16_t));
    if(!ctx->output) goto failed;
    ctx->lock = sceKernelCreateSema("OpenOMFAudioLock", 0, 1, 1, NULL);
    if(ctx->lock < 0) goto failed;
    ctx->output_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, PSP_MIX_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if(ctx->output_channel < 0) goto failed;
    ctx->thread = sceKernelCreateThread("OpenOMFAudio", audio_thread, 18, 32 * 1024, 0, NULL);
    if(ctx->thread < 0) goto failed;
    active_context = ctx;
    ctx->running = 1;
    if(sceKernelStartThread(ctx->thread, 0, NULL) < 0) {
        ctx->running = 0;
        active_context = NULL;
        goto failed;
    }
    log_info("PSP native audio: 44100 Hz stereo, %d-frame buffers, 3 sound channels", PSP_MIX_FRAMES);
    return true;
failed:
    if(ctx->thread >= 0) sceKernelDeleteThread(ctx->thread);
    if(ctx->output_channel >= 0) sceAudioChRelease(ctx->output_channel);
    if(ctx->lock >= 0) sceKernelDeleteSema(ctx->lock);
    free(ctx->output);
    ctx->output = NULL;
    ctx->thread = ctx->lock = ctx->output_channel = -1;
    log_error("PSP native audio: could not initialize audio thread or hardware channel");
    return false;
}

static void close_audio(void *ptr) {
    psp_audio_context *ctx = ptr;
    ctx->running = 0;
    if(ctx->thread >= 0) {
        sceKernelWaitThreadEnd(ctx->thread, NULL);
        sceKernelDeleteThread(ctx->thread);
        ctx->thread = -1;
    }
    if(ctx->lock >= 0 && lock_audio(ctx)) {
        music_source_close(&ctx->music);
        for(int ch = 0; ch < SOUND_CHANNEL_COUNT; ++ch) release_voice(&ctx->voices[ch]);
        unlock_audio(ctx);
    }
    if(ctx->output_channel >= 0) sceAudioChRelease(ctx->output_channel);
    if(ctx->lock >= 0) sceKernelDeleteSema(ctx->lock);
    ctx->output_channel = ctx->lock = -1;
    log_info("PSP native audio: blocks=%lu music=%lu effects=%lu starts=%lu output-errors=%lu cache hits=%u misses=%u entries=%u size=%u KiB",
             ctx->mixed_blocks, ctx->music_blocks, ctx->effect_blocks, ctx->sound_starts,
             ctx->missed_blocks, ctx->cache_hits, ctx->cache_misses,
             ctx->cache_count, (unsigned)(ctx->cache_bytes / 1024));
    for(unsigned i = 0; i < ctx->cache_count; ++i) free(ctx->cache[i].data);
    ctx->cache_count = 0;
    ctx->cache_bytes = 0;
    free(ctx->output);
    ctx->output = NULL;
    active_context = NULL;
}

static void set_sound_volume(void *ptr, float volume) {
    psp_audio_context *ctx = ptr;
    if(lock_audio(ctx)) {
        ctx->sound_volume = (int)(clamp_volume(volume) * 127.0f);
        unlock_audio(ctx);
    }
}
static void set_music_volume(void *ptr, float volume) {
    psp_audio_context *ctx = ptr;
    if(lock_audio(ctx)) {
        ctx->music_volume = clamp_volume(volume);
        music_source_set_volume(&ctx->music, ctx->music_volume);
        unlock_audio(ctx);
    }
}

static bool play_sound(void *ptr, int channel, const sound_source *src,
                       int volume, int panning, int fade_in_ms) {
    psp_audio_context *ctx = ptr;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT || !src) return false;
    const int16_t *pcm = NULL;
    int16_t *owned = NULL;
    size_t frames = 0;
    for(unsigned i = 0; i < ctx->cache_count; ++i) {
        const pcm_entry *e = &ctx->cache[i];
        if(e->source == src->buf && e->length == src->len && e->frequency == src->freq) {
            pcm = e->data;
            frames = e->frames;
            ++ctx->cache_hits;
            break;
        }
    }
    if(!pcm) {
        ++ctx->cache_misses;
        owned = resample_mono(src, &frames);
        if(!owned) return false;
        pcm = owned;
        const size_t bytes = frames * sizeof(int16_t);
        if(ctx->cache_count < PSP_PCM_CACHE_ENTRIES && bytes <= PSP_PCM_CACHE_BYTES - ctx->cache_bytes) {
            pcm_entry *e = &ctx->cache[ctx->cache_count++];
            *e = (pcm_entry){.source = src->buf, .length = src->len, .frequency = src->freq,
                             .data = owned, .frames = frames};
            ctx->cache_bytes += bytes;
            owned = NULL;
        }
    }
    if(!lock_audio(ctx)) {
        free(owned);
        return false;
    }
    voice *v = &ctx->voices[channel];
    release_voice(v);
    *v = (voice){.pcm = pcm, .owned_pcm = owned, .frames = frames,
                 .volume = clamp_int(volume, 0, 127), .pan = clamp_int(panning, -100, 100),
                 .fade_in_frames = fade_in_ms > 0 ? (int)((int64_t)fade_in_ms * PSP_MIX_RATE / 1000) : 0};
    ++ctx->sound_starts;
    unlock_audio(ctx);
    return true;
}

static bool is_playing(void *ptr, int channel) {
    psp_audio_context *ctx = ptr;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) return false;
    if(!lock_audio(ctx)) return false;
    const voice *v = &ctx->voices[channel];
    bool playing = v->pcm != NULL && v->cursor < v->frames;
    unlock_audio(ctx);
    return playing;
}
static void stop_sound(void *ptr, int channel) {
    psp_audio_context *ctx = ptr;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) return;
    if(lock_audio(ctx)) {
        release_voice(&ctx->voices[channel]);
        unlock_audio(ctx);
    }
}
static void fade_out(void *ptr, int channel, int ms) {
    psp_audio_context *ctx = ptr;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) return;
    if(!lock_audio(ctx)) return;
    voice *v = &ctx->voices[channel];
    if(ms <= 0) release_voice(v);
    else {
        const int frames = (int)((int64_t)ms * PSP_MIX_RATE / 1000);
        v->fade_out_frames = frames > 0 ? frames : 1;
        v->fade_out_left = v->fade_out_frames;
    }
    unlock_audio(ctx);
}
static void set_pan(void *ptr, int channel, int pan) {
    psp_audio_context *ctx = ptr;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) return;
    if(lock_audio(ctx)) {
        ctx->voices[channel].pan = clamp_int(pan, -100, 100);
        unlock_audio(ctx);
    }
}
static void stop_music(void *ptr) {
    psp_audio_context *ctx = ptr;
    if(lock_audio(ctx)) {
        music_source_close(&ctx->music);
        unlock_audio(ctx);
    }
}
static void play_music(void *ptr, const music_source *src) {
    psp_audio_context *ctx = ptr;
    if(!lock_audio(ctx)) return;
    music_source_close(&ctx->music);
    if(src) {
        ctx->music = *src;
        music_source_set_volume(&ctx->music, ctx->music_volume);
    }
    unlock_audio(ctx);
}

void psp_audio_backend_set_callbacks(audio_backend *backend) {
    *backend = (audio_backend){
        .is_available = available, .get_description = description, .get_name = name,
        .get_sample_rates = rate_list, .get_info = get_info,
        .create = create, .destroy = destroy, .setup_context = setup, .close_context = close_audio,
        .set_sound_volume = set_sound_volume, .set_music_volume = set_music_volume,
        .play_pcm_sound = play_sound, .is_channel_playing = is_playing,
        .stop_channel = stop_sound, .fade_out_channel = fade_out,
        .set_channel_panning = set_pan, .play_music = play_music, .stop_music = stop_music,
    };
}
