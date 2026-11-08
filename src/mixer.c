#include "mixer.h"
#include "emulator.h"
#include "utils.h"

#include <math.h>
#include <string.h>

#ifdef DISABLE_AUDIO

void init_mixer(Emulator* emulator){}
void sample(Mixer* mixer){}
void begin_audio_batch(Mixer* mixer){}
void end_audio_batch(Mixer* mixer){}
size_t drain_audio(Mixer* mixer, int16_t* out, size_t max){return 0;}
void adjust_audio_rate(Mixer* mixer, double ratio){}
void queue_audio(Mixer* mixer){}
void pause_audio(Mixer* mixer, int flag){}
void free_mixer(const Mixer* mixer){}

#else

static void init_sampler(Mixer* mixer, int frequency, TVSystem sys);
static void flush_run(Mixer* mixer);
#ifndef EMU_HEADLESS
static void init_audio_device(Mixer* mixer);
#endif

void init_mixer(Emulator* emulator) {
    Mixer* mixer = &emulator->mixer;
    mixer->apu = &emulator->apu;
    // For keeping track of queue_size statistics for use by the adaptive sampler
    memset(mixer->stat_window, 0, sizeof(mixer->stat_window));
    mixer->stat = 0;
    mixer->stat_index = 0;
    mixer->audio_start = 0;
    mixer->volume = 1;
    mixer->run_level = 0;
    mixer->run_length = 0;
    mixer->batch = 0;

    init_sampler(mixer, SAMPLING_FREQUENCY, emulator->type);
#ifndef EMU_HEADLESS
    init_audio_device(mixer);
    pause_audio(mixer, 1);
#endif
}

#ifndef EMU_HEADLESS
void init_audio_device(Mixer* mixer) {

    const SDL_AudioSpec spec = {
        .format = SDL_AUDIO_S16,
        .channels = 1,
        .freq = SAMPLING_FREQUENCY
    };

    mixer->audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (mixer->audio_stream == NULL) {
        LOG(ERROR , SDL_GetError());
        quit(EXIT_FAILURE);
    }
}
#endif

void init_sampler(Mixer* mixer, int frequency, TVSystem sys) {
    float cycles_per_frame = sys == PAL? PAL_CYCLES_PER_FRAME: NTSC_CYCLES_PER_FRAME;
    float rate = sys == PAL? 50.0f : 60.0f;
    Sampler* sampler = &mixer->sampler;
    // Q = 0.707 => BW = 1.414 (1 octave)
    biquad_init(&mixer->filter, HPF, 0, 20, frequency, 1);
    // anti-aliasing filter.
    biquad_init(&mixer->aa_filter, LPF, 0, 20000, cycles_per_frame * rate, 1);

    sampler->nominal_period = (sys == PAL? PAL_CPU_CLOCK: NTSC_CPU_CLOCK) / frequency;
    sampler->period = sampler->nominal_period;
    sampler->index = 0;
    sampler->max_index = AUDIO_BUFF_SIZE;
    sampler->samples = 0;
    sampler->counter = 0;
}

void adjust_audio_rate(Mixer* mixer, double ratio) {
    flush_run(mixer);
    mixer->sampler.period = mixer->sampler.nominal_period / ratio;
}

static int same_double(double a, double b) {
    return a == b && signbit(a) == signbit(b);
}

static void emit_sample(Mixer* mixer, float sample) {
    Sampler* sampler = &mixer->sampler;
    mixer->buff[sampler->index++] = 32000 * biquad(sample, &mixer->filter) * mixer->volume;
    if(sampler->index >= sampler->max_index) {
        sampler->index = 0;
    }
    sampler->samples++;
}

static void flush_run(Mixer* mixer) {
    size_t n = mixer->run_length;
    if(!n)
        return;
    mixer->run_length = 0;

    const double level = mixer->run_level;
    Biquad* b = &mixer->aa_filter;
    double x1 = b->x1, x2 = b->x2, y1 = b->y1, y2 = b->y2;
    double counter = mixer->sampler.counter;
    const double period = mixer->sampler.period;

    while(n && !(same_double(x1, level) && same_double(x2, level))) {
        double result = b->a0 * level + b->a1 * x1 + b->a2 * x2 - b->a3 * y1 - b->a4 * y2;
        x2 = x1;
        x1 = level;
        y2 = y1;
        y1 = result;
        counter++;
        if(counter >= period) {
            emit_sample(mixer, (float)result);
            counter -= period;
        }
        n--;
    }

    if(n) {
        const double c = b->a0 * level + b->a1 * x1 + b->a2 * x2;
        while(n) {
            double result = c - b->a3 * y1 - b->a4 * y2;
            if(same_double(result, y1) && same_double(y1, y2))
                break;
            y2 = y1;
            y1 = result;
            counter++;
            if(counter >= period) {
                emit_sample(mixer, (float)result);
                counter -= period;
            }
            n--;
        }
        const float steady = (float)y1;
        while(n) {
            counter++;
            if(counter >= period) {
                emit_sample(mixer, steady);
                counter -= period;
            }
            n--;
        }
    }

    b->x1 = x1;
    b->x2 = x2;
    b->y1 = y1;
    b->y2 = y2;
    mixer->sampler.counter = counter;
}

void begin_audio_batch(Mixer* mixer) {
    mixer->apu->level_dirty = 1;
    mixer->run_length = 0;
    mixer->batch = 1;
}

void end_audio_batch(Mixer* mixer) {
    flush_run(mixer);
    mixer->batch = 0;
}

void sample(Mixer* mixer) {
    if(mixer->batch) {
        APU* apu = mixer->apu;
        if(!apu->level_dirty) {
            mixer->run_length++;
            return;
        }
        apu->level_dirty = 0;
        const float level = get_sample(apu);
        if(mixer->run_length && level == mixer->run_level) {
            mixer->run_length++;
            return;
        }
        flush_run(mixer);
        mixer->run_level = level;
        mixer->run_length = 1;
        return;
    }
    float sample = biquad(get_sample(mixer->apu), &mixer->aa_filter);
#if AVERAGE_DOWNSAMPLING
    static float avg = -1;
    // average samples in a bin
    if(avg < 0)
        avg = sample;
    else
        avg = (avg + sample)/2;
#endif

    Sampler* sampler = &mixer->sampler;
    sampler->counter++;
    if(sampler->counter >= sampler->period) {
#if AVERAGE_DOWNSAMPLING
        apu->buff[sampler->index++] = 32767 * biquad(avg, &apu->filter);
        // begin fresh average for the next bin
        avg = -1;
#else

        mixer->buff[sampler->index++] = 32000 * biquad(sample, &mixer->filter) * mixer->volume;
#endif
        if(sampler->index >= sampler->max_index) {
            sampler->index = 0;
        }
        sampler->samples++;
        sampler->counter -= sampler->period;
    }
}

size_t drain_audio(Mixer* mixer, int16_t* out, size_t max) {
    flush_run(mixer);
    size_t count = mixer->sampler.index < max ? mixer->sampler.index : max;
    memcpy(out, mixer->buff, count * sizeof(int16_t));
    mixer->sampler.index = 0;
    return count;
}

#ifndef EMU_HEADLESS
void queue_audio(Mixer* mixer) {
    uint32_t queue_size = SDL_GetAudioStreamQueued(mixer->audio_stream);
    mixer->stat = mixer->stat - mixer->stat_window[mixer->stat_index] + queue_size;
    mixer->stat_window[mixer->stat_index++] = queue_size;
    if(mixer->stat_index >= STATS_WIN_SIZE)
        mixer->stat_index = 0;

    size_t avg = mixer->stat / STATS_WIN_SIZE;
    // printf("queue size %d, avg: %llu \n", queue_size, avg);

    // From here we tweak the sampling rate ever so slightly to prevent underruns and runaway latency
    // by minimising deviation from the nominal queue size with a bit of control engineering
    float error = ((float)avg - NOMINAL_QUEUE_SIZE) / NOMINAL_QUEUE_SIZE;
    Sampler* s = &mixer->sampler;
    adjust_audio_rate(mixer, 1 - MAX_RATE_DEVIATION * (error > 1 ? 1 : error));

    SDL_PutAudioStreamData(mixer->audio_stream, mixer->buff, s->index * 2);
    // wait till queue is filled to prevent early onset underruns
    if(!mixer->audio_start && queue_size >= NOMINAL_QUEUE_SIZE) {
        pause_audio(mixer, 0);
        mixer->audio_start = 1;
    }
#if AUDIO_TO_FILE
    if(out_wav)
        fwrite(apu->buff, 2, s->index, out_wav);
#endif
    memset(mixer->buff, 0, AUDIO_BUFF_SIZE * 2);
    // reset sampler
    s->index = 0;
}

void pause_audio(Mixer* mixer, const int flag) {
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(mixer->audio_stream);
    int paused = SDL_AudioDevicePaused(dev);
    if(paused == flag)
        return;
    if(flag)
        SDL_PauseAudioDevice(dev);
    else
        SDL_ResumeAudioDevice(dev);
}

void free_mixer(const Mixer* mixer) {
    SDL_DestroyAudioStream(mixer->audio_stream);
}
#endif
#endif
