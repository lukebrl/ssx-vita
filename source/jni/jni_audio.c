/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_audio.c
 * @brief android.media.AudioTrack, as used by EAAudioCore.
 *
 * The game mixes everything itself and pushes 16-bit stereo PCM to an
 * AudioTrack from its own audio thread with write(short[], int, int).
 *
 * The PCM goes into a queue of AUDIO_SLOTS grains that our own output thread
 * plays on a Vita audio port. write() blocks while the queue is full, which
 * paces the game's mixer like a real AudioTrack in blocking mode does. The
 * queue lets the mixer run late by up to AUDIO_SLOTS - 1 grains (64 ms)
 * before the sound drops out: with only the double buffer sceAudioOutOutput
 * gives, any 21 ms the mixer thread spent waiting for a core (the job
 * threads keep three cores busy in a race) was an audible gap.
 */

#include "jni/jni_ssx.h"

#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

char jobj_audio_track[16];

#define AUDIO_SLOTS 4
// Above every game thread (0x9f, the audio ones 0x80, see prctl_soloader);
// it only copies a grain to the port every 21 ms.
#define AUDIO_OUTPUT_PRIORITY 0x50

static int audio_port = -1;
static int16_t audio_buf[AUDIO_SLOTS][AUDIO_GRAIN * 2];
static int16_t audio_silence[AUDIO_GRAIN * 2];
static SceUID audio_free_sema = -1; // slots the writer may fill
static SceUID audio_used_sema = -1; // slots ready to play
static int audio_write_slot = 0;
static int audio_fill = 0;          // in samples (frames * 2), -1 = no slot held
static volatile uint32_t audio_underruns;
static volatile uint32_t audio_grains;

static int audio_output_thread(SceSize args, void *argp) {
    int read_slot = 0;
    int playing = -1; // slot the port is still reading, freed after the next output
    int started = 0;
    while (1) {
        const int16_t *buf = audio_silence;
        int slot = -1;
        if (sceKernelPollSema(audio_used_sema, 1) >= 0) {
            slot = read_slot;
            read_slot = (read_slot + 1) % AUDIO_SLOTS;
            buf = audio_buf[slot];
            started = 1;
            audio_grains++;
        } else if (started) {
            audio_underruns++;
        }
        sceAudioOutOutput(audio_port, buf);
        if (playing >= 0)
            sceKernelSignalSema(audio_free_sema, 1);
        playing = slot;
    }
    return 0;
}

void audio_start(void) {
    if (audio_port >= 0)
        return;

    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN,
                                     AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) {
        l_error("[AudioTrack] sceAudioOutOpenPort failed: 0x%x", audio_port);
        return;
    }

    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    // One slot stays with the port while it plays, so the writer gets the rest
    audio_free_sema = sceKernelCreateSema("ssx_audio_free", 0, AUDIO_SLOTS - 1, AUDIO_SLOTS, NULL);
    audio_used_sema = sceKernelCreateSema("ssx_audio_used", 0, 0, AUDIO_SLOTS, NULL);
    audio_fill = -1;
    SceUID thid = sceKernelCreateThread("ssx_audio_out", audio_output_thread, AUDIO_OUTPUT_PRIORITY,
                                        0x2000, 0, 0, NULL);
    if (audio_free_sema < 0 || audio_used_sema < 0 || thid < 0) {
        l_error("[AudioTrack] output thread setup failed: 0x%x 0x%x 0x%x", audio_free_sema,
                audio_used_sema, thid);
        sceAudioOutReleasePort(audio_port);
        audio_port = -1;
        return;
    }
    sceKernelStartThread(thid, 0, NULL);
    l_info("[AudioTrack] port %d open, %d Hz, grain %d, %d grains queued at most", audio_port,
           AUDIO_RATE, AUDIO_GRAIN, AUDIO_SLOTS - 1);
}

void audio_report(void) {
    static uint32_t seen;
    uint32_t n = audio_underruns;
    if (n != seen) {
        l_warn("[AUDIO] mixer late %u times since the last report (%u ms of silence), %u grains played",
               n - seen, (n - seen) * AUDIO_GRAIN * 1000 / AUDIO_RATE, audio_grains);
        seen = n;
    }
}

// void play()
void AudioTrack_play(jmethodID id, va_list args) {
    audio_start();
}

// void stop()
void AudioTrack_stop(jmethodID id, va_list args) {
    // Whatever was mixed into the held slot is dropped; the slot is kept
    if (audio_fill > 0)
        audio_fill = 0;
}

// int write(short[] audioData, int offsetInShorts, int sizeInShorts)
jint AudioTrack_write(jmethodID id, va_list args) {
    JavaDynArray *jda = va_arg(args, JavaDynArray *);
    jint offset = va_arg(args, jint);
    jint size = va_arg(args, jint);

    if (!jda || offset < 0 || size < 0 || offset + size > jda->len)
        return -3; // AudioTrack.ERROR_BAD_VALUE

    if (audio_port < 0)
        audio_start();
    if (audio_port < 0)
        return size;

    const int16_t *src = (const int16_t *)jda->array + offset;
    int left = size;
    while (left > 0) {
        if (audio_fill < 0) {
            sceKernelWaitSema(audio_free_sema, 1, NULL);
            audio_fill = 0;
        }
        int n = AUDIO_GRAIN * 2 - audio_fill;
        if (n > left)
            n = left;
        memcpy(audio_buf[audio_write_slot] + audio_fill, src, n * sizeof(int16_t));
        audio_fill += n;
        src += n;
        left -= n;

        if (audio_fill == AUDIO_GRAIN * 2) {
            audio_write_slot = (audio_write_slot + 1) % AUDIO_SLOTS;
            audio_fill = -1;
            sceKernelSignalSema(audio_used_sema, 1);
        }
    }
    return size;
}
