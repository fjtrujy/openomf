#include "audio/backends/sdl/sdl_backend.h"
#include "audio/backends/audio_backend.h"
#include "audio/music_sources/music_source.h"
#include "utils/allocator.h"
#include "utils/c_array_util.h"
#include "utils/log.h"
#include "utils/miscmath.h"

#include <assert.h>
#include <stdlib.h>

#include <SDL.h>
#include <SDL_mixer.h>

#ifdef __PSP__
// PSP SDL_ConvertAudio is expensive, especially when several effects start
// within a single 16ms frame. Sound DAT samples are stable borrowed buffers,
// so keep the 48kHz stereo conversion for reuse instead of recomputing it.
#define PSP_PCM_CACHE_MAX_BYTES (4u * 1024u * 1024u)
#define PSP_PCM_CACHE_MAX_ENTRIES 256
typedef struct {
    const char *source;
    size_t source_len;
    int source_freq;
    Uint8 *pcm;
    Uint32 pcm_len;
} psp_pcm_cache_entry;
#endif

static_assert(SDL_MIXER_VERSION_ATLEAST(2, 0, 4), "SDL_mixer version should be 2.0.4 or later");

static const audio_sample_rate supported_sample_rates[] = {
    {11025, 0, "11025Hz"},
    {22050, 0, "22050Hz"},
    {44100, 0, "44100Hz"},
    {48000, 1, "48000Hz"},
};
static const int supported_sample_rate_count = N_ELEMENTS(supported_sample_rates);

typedef struct sdl_audio_context {
    int sample_rate;
    Uint16 format;
    int channels;
    int resampler;
    float volume;
    music_source music;
    Mix_Chunk chunks[SOUND_CHANNEL_COUNT]; // Per-channel converted-sample buffers.
#ifdef __PSP__
    psp_pcm_cache_entry pcm_cache[PSP_PCM_CACHE_MAX_ENTRIES];
    unsigned cached_count;
    size_t cached_bytes;
    unsigned cache_hits;
    unsigned cache_misses;
#endif
} sdl_audio_context;

static bool is_available(void) {
    return true; // This is always available if compiled in.
}

static const char *get_description(void) {
    return "Audio output using SDL_mixer";
}

static const char *get_name(void) {
    return "SDL_mixer";
}

static unsigned int get_sample_rates(const audio_sample_rate **sample_rates) {
    *sample_rates = supported_sample_rates;
    return supported_sample_rate_count;
}

static void create_backend(audio_backend *player) {
    player->ctx = omf_calloc(1, sizeof(sdl_audio_context));
}

static void destroy_backend(audio_backend *player) {
    omf_free(player->ctx);
}

static const char *get_sdl_audio_format_string(SDL_AudioFormat format) {
    switch(format) {
        case AUDIO_U8:
            return "AUDIO_U8";
        case AUDIO_S8:
            return "AUDIO_S8";
        case AUDIO_U16LSB:
            return "AUDIO_U16LSB";
        case AUDIO_S16LSB:
            return "AUDIO_S16LSB";
        case AUDIO_U16MSB:
            return "AUDIO_U16MSB";
        case AUDIO_S16MSB:
            return "AUDIO_S16MSB";
        case AUDIO_S32LSB:
            return "AUDIO_S32LSB";
        case AUDIO_S32MSB:
            return "AUDIO_S32MSB";
        case AUDIO_F32LSB:
            return "AUDIO_F32LSB";
        case AUDIO_F32MSB:
            return "AUDIO_F32MSB";
    }
    return "UNKNOWN";
}

static inline void free_chunk(sdl_audio_context *ctx, const int i) {
    Mix_Chunk *chunk = &ctx->chunks[i];
    if(chunk->allocated) {
        SDL_free(chunk->abuf);
    }
    chunk->abuf = NULL;
    chunk->alen = 0;
    chunk->allocated = 0;
}

#ifdef __PSP__
// SDL_BuildAudioCVT+SDL_ConvertAudio was taking up to ~140 ms for cold sound
// effects in PPSSPP, causing entire gameplay frames to stall. The only input
// format provided by SOUNDS.DAT is unsigned 8-bit mono. Resample directly to
// the S16 output format with fixed-point linear interpolation. This also
// handles runtime pitch changes (src_freq differs for pitched playback).
static bool psp_convert_pcm(const sdl_audio_context *ctx, Mix_Chunk *chunk,
                            const char *src_buf, const size_t src_len,
                            int src_freq, int volume) {
    if(ctx->format != AUDIO_S16SYS || (ctx->channels != 1 && ctx->channels != 2) ||
       ctx->sample_rate <= 0 || src_freq <= 0 || src_len == 0) {
        return false;
    }
    const uint64_t samples64 = ((uint64_t)src_len * (unsigned)ctx->sample_rate) / (unsigned)src_freq;
    if(samples64 == 0 || samples64 > (UINT32_MAX / ((unsigned)ctx->channels * sizeof(int16_t)))) {
        return false;
    }
    const size_t samples = (size_t)samples64;
    const size_t size = samples * (size_t)ctx->channels * sizeof(int16_t);
    int16_t *buffer = SDL_malloc(size);
    if(!buffer) return false;

    const uint8_t *input = (const uint8_t *)src_buf;
    const uint64_t increment = ((uint64_t)(unsigned)src_freq << 32) / (unsigned)ctx->sample_rate;
    uint64_t phase = 0;
    for(size_t i = 0; i < samples; i++, phase += increment) {
        size_t idx = (size_t)(phase >> 32);
        if(idx >= src_len) idx = src_len - 1;
        const size_t next = idx + 1 < src_len ? idx + 1 : idx;
        const int a = (int)input[idx] - 128;
        const int b = (int)input[next] - 128;
        const unsigned fraction = (unsigned)(phase >> 16) & 0xffffu;
        const int16_t sample = (int16_t)(a * 256 + ((b - a) * (int)fraction) / 256);
        for(int channel = 0; channel < ctx->channels; channel++) {
            buffer[i * (size_t)ctx->channels + (size_t)channel] = sample;
        }
    }
    chunk->allocated = 1;
    chunk->abuf = (Uint8 *)buffer;
    chunk->alen = (Uint32)size;
    chunk->volume = volume;
    return true;
}
#endif

#ifdef __PSP__
static bool psp_pcm_cache_get(sdl_audio_context *ctx, Mix_Chunk *chunk, const char *source,
                              size_t source_len, int source_freq, int volume) {
    for(unsigned i = 0; i < ctx->cached_count; i++) {
        const psp_pcm_cache_entry *item = &ctx->pcm_cache[i];
        if(item->source == source && item->source_len == source_len &&
           item->source_freq == source_freq) {
            *chunk = (Mix_Chunk){.allocated = 0, .abuf = item->pcm,
                                  .alen = item->pcm_len, .volume = volume};
            ctx->cache_hits++;
            return true;
        }
    }
    ctx->cache_misses++;
    return false;
}

static void psp_pcm_cache_store(sdl_audio_context *ctx, Mix_Chunk *chunk, const char *source,
                                size_t source_len, int source_freq) {
    if(ctx->cached_count >= PSP_PCM_CACHE_MAX_ENTRIES ||
       chunk->alen > PSP_PCM_CACHE_MAX_BYTES - ctx->cached_bytes) {
        return; // Allow non-cached playback; never sacrifice audio or allocate unboundedly.
    }
    psp_pcm_cache_entry *item = &ctx->pcm_cache[ctx->cached_count++];
    *item = (psp_pcm_cache_entry){.source = source, .source_len = source_len,
                                  .source_freq = source_freq,
                                  .pcm = chunk->abuf, .pcm_len = chunk->alen};
    ctx->cached_bytes += chunk->alen;
    chunk->allocated = 0; // The cache now owns the PCM; the channel only borrows it.
}
#endif

static bool audio_get_chunk(const sdl_audio_context *ctx, Mix_Chunk *chunk, const char *src_buf, const size_t src_len,
                            const int src_freq, const int volume) {
#ifdef __PSP__
    if(ctx->format == AUDIO_S16SYS && (ctx->channels == 1 || ctx->channels == 2)) {
        return psp_convert_pcm(ctx, chunk, src_buf, src_len, src_freq, volume);
    }
#endif
    SDL_AudioCVT cvt;

    if(SDL_BuildAudioCVT(&cvt, AUDIO_U8, 1, src_freq, ctx->format, ctx->channels, ctx->sample_rate) < 0) {
        log_error("Unable to build audio converter: %s", SDL_GetError());
        goto exit_0;
    }

    // Buffer must hold both the source bytes (copied below) and the converted output.
    Uint8 *dst_buf = SDL_malloc(src_len * cvt.len_mult + 1);
    if(dst_buf == NULL) {
        log_error("Unable to allocate memory for sound buffer");
        goto exit_0;
    }
    SDL_memcpy((void *)dst_buf, (void *)src_buf, src_len);

    cvt.buf = dst_buf;
    cvt.len = src_len;
    if(SDL_ConvertAudio(&cvt) != 0) {
        log_error("Unable to convert audio sample: %s", SDL_GetError());
        goto exit_1;
    }

    chunk->volume = volume;
    chunk->abuf = dst_buf;
    chunk->alen = cvt.len_cvt;
    chunk->allocated = 1;
    return true;

exit_1:
    SDL_free(dst_buf);
exit_0:
    return false;
}

static void set_backend_sound_volume(void *userdata, float volume) {
    assert(userdata);
    volume = clampf(volume, 0.0f, 1.0f);
    Mix_Volume(-1, volume * MIX_MAX_VOLUME);
}

static void set_backend_music_volume(void *userdata, const float volume) {
    assert(userdata);
    sdl_audio_context *const ctx = userdata;
    ctx->volume = clampf(volume, 0.0f, 1.0f);
    music_source_set_volume(&ctx->music, ctx->volume);
}

static void get_info(void *userdata, unsigned *sample_rate, unsigned *channels, unsigned *resampler) {
    assert(userdata);
    const sdl_audio_context *const ctx = userdata;
    if(sample_rate != NULL) {
        *sample_rate = ctx->sample_rate;
    }
    if(channels != NULL) {
        *channels = ctx->channels;
    }
    if(resampler != NULL) {
        *resampler = ctx->resampler;
    }
}

static bool play_pcm_sound(void *userdata, const int channel, const sound_source *src, const int volume,
                           const int panning, const int fade_in_ms) {
    assert(userdata);
    assert(src);
    sdl_audio_context *const ctx = userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return false;
    }

    // map -100 ... 100 to SDL_mixers range
    const int pan_left = (panning > 0) ? (100 - panning) * 255 / 100 : 255;
    const int pan_right = (panning < 0) ? (100 + panning) * 255 / 100 : 255;

    // The mixer may still be reading the previous chunk on its audio thread.
    // Never release or replace the PCM before SDL_mixer has stopped using it.
    // Mix_FadeInChannelTimed also replaces the channel, but doing so *after*
    // free_chunk would leave a short use-after-free window.
    Mix_HaltChannel(channel);
    free_chunk(ctx, channel);
#ifdef __PSP__
    if(!psp_pcm_cache_get(ctx, &ctx->chunks[channel], src->buf, src->len, src->freq, volume)) {
        if(!audio_get_chunk(ctx, &ctx->chunks[channel], src->buf, src->len, src->freq, volume)) {
            log_error("Unable to play sound: Failed to convert chunk");
            return false;
        }
        psp_pcm_cache_store(ctx, &ctx->chunks[channel], src->buf, src->len, src->freq);
    }
#else
    if(!audio_get_chunk(ctx, &ctx->chunks[channel], src->buf, src->len, src->freq, volume)) {
        log_error("Unable to play sound: Failed to load chunk");
        return false;
    }
#endif
    Mix_SetPanning(channel, pan_left, pan_right);
    if(Mix_FadeInChannelTimed(channel, &ctx->chunks[channel], 0, fade_in_ms, -1) == -1) {
        log_error("Unable to play sound: %s", Mix_GetError());
        return false;
    }
    return true;
}

static bool is_channel_playing(void *userdata, const int channel) {
    assert(userdata);
    (void)userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return false;
    }
    return Mix_Playing(channel) != 0;
}

static void stop_channel(void *userdata, const int channel) {
    assert(userdata);
    sdl_audio_context *const ctx = userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return;
    }
    Mix_HaltChannel(channel);
    free_chunk(ctx, channel);
}

static void fade_out_channel(void *userdata, const int channel, const int ms) {
    assert(userdata);
    (void)userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return;
    }
    Mix_FadeOutChannel(channel, ms);
}

static void set_channel_panning(void *userdata, const int channel, const int panning) {
    assert(userdata);
    (void)userdata;
    if(channel < 0 || channel >= SOUND_CHANNEL_COUNT) {
        return;
    }
    const int pan_left = (panning > 0) ? (100 - panning) * 255 / 100 : 255;
    const int pan_right = (panning < 0) ? (100 + panning) * 255 / 100 : 255;
    Mix_SetPanning(channel, pan_left, pan_right);
}

static void stop_music(void *userdata) {
    assert(userdata);
    sdl_audio_context *const ctx = userdata;
    // The audio callback may still read ctx->music. Removing the hook first
    // synchronizes with the mixer before the underlying music source is freed.
    Mix_HookMusic(NULL, NULL);
    Mix_HaltMusic();
    music_source_close(&ctx->music);
}

static void sdl_hook(void *userdata, Uint8 *stream, const int len) {
    sdl_audio_context *const ctx = userdata;
    music_source_render(&ctx->music, (char *)stream, len);
}

static void play_music(void *userdata, const music_source *src) {
    assert(userdata);
    sdl_audio_context *const ctx = userdata;
    stop_music(ctx);
    memcpy(&ctx->music, src, sizeof(music_source));
    music_source_set_volume(&ctx->music, ctx->volume);
    Mix_HookMusic(sdl_hook, ctx);
}

static bool setup_backend_context(void *userdata, const unsigned sample_rate, const bool mono, const int resampler,
                                  const float music_volume, const float sound_volume) {
    assert(userdata);
    sdl_audio_context *const ctx = userdata;
    memset(ctx, 0, sizeof(sdl_audio_context));

    if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        log_error("Unable to initialize audio subsystem: %s", SDL_GetError());
        goto error_0;
    }
    if(Mix_Init(0) != 0) {
        log_error("Unable to initialize mixer subsystem: %s", Mix_GetError());
        goto error_1;
    }

    log_info("Requested audio device with options:");
    log_info(" * Sample rate: %uHz", sample_rate);
    log_info(" * Channels: %d", mono ? 1 : 2);
    log_info(" * Format: %s", get_sdl_audio_format_string(AUDIO_S16SYS));

    // Request a device -- the actual configuration we get back may differ.
    if(Mix_OpenAudioDevice(sample_rate, AUDIO_S16SYS, mono ? 1 : 2, 2048, NULL, 0) != 0) {
        log_error("Unable to initialize audio device: %s", SDL_GetError());
        goto error_2;
    }

    Mix_AllocateChannels(SOUND_CHANNEL_COUNT);

    set_backend_sound_volume(ctx, sound_volume);
    set_backend_music_volume(ctx, music_volume);
    ctx->resampler = resampler;

    // Query back what the device actually gave us, vs what we asked for above.
    Mix_QuerySpec(&ctx->sample_rate, &ctx->format, &ctx->channels);
    log_info("Opened audio device:");
    log_info(" * Sample rate: %dHz", ctx->sample_rate);
    log_info(" * Channels: %d", ctx->channels);
    log_info(" * Format: %s", get_sdl_audio_format_string(ctx->format));
    return true;

error_2:
    Mix_Quit();
error_1:
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
error_0:
    return false;
}

static void close_backend_context(void *userdata) {
    assert(userdata);
    sdl_audio_context *const ctx = userdata;
    log_debug("closing audio");
    stop_music(ctx);
    Mix_ChannelFinished(NULL);
    Mix_CloseAudio();
    for(int i = 0; i < SOUND_CHANNEL_COUNT; i++) {
        free_chunk(ctx, i);
    }
#ifdef __PSP__
    log_info("PSP PCM cache: hits=%u misses=%u entries=%u size=%u KiB",
             ctx->cache_hits, ctx->cache_misses, ctx->cached_count,
             (unsigned)(ctx->cached_bytes / 1024));
    for(unsigned i = 0; i < ctx->cached_count; i++) {
        SDL_free(ctx->pcm_cache[i].pcm);
        ctx->pcm_cache[i].pcm = NULL;
    }
    ctx->cached_count = 0;
    ctx->cached_bytes = 0;
#endif
    Mix_Quit();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void sdl_audio_backend_set_callbacks(audio_backend *sdl_backend) {
    sdl_backend->is_available = is_available;
    sdl_backend->get_description = get_description;
    sdl_backend->get_name = get_name;
    sdl_backend->get_sample_rates = get_sample_rates;
    sdl_backend->get_info = get_info;
    sdl_backend->create = create_backend;
    sdl_backend->destroy = destroy_backend;
    sdl_backend->set_music_volume = set_backend_music_volume;
    sdl_backend->set_sound_volume = set_backend_sound_volume;
    sdl_backend->setup_context = setup_backend_context;
    sdl_backend->close_context = close_backend_context;
    sdl_backend->play_pcm_sound = play_pcm_sound;
    sdl_backend->is_channel_playing = is_channel_playing;
    sdl_backend->stop_channel = stop_channel;
    sdl_backend->fade_out_channel = fade_out_channel;
    sdl_backend->set_channel_panning = set_channel_panning;
    sdl_backend->play_music = play_music;
    sdl_backend->stop_music = stop_music;
}
