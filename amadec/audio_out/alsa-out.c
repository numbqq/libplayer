/**
 * \file alsa-out.c
 * \brief  Functions of Auduo output control for Linux Platform
 * \version 1.0.0
 * \date 2011-03-08
 */
/* Copyright (C) 2007-2011, Amlogic Inc.
 * All right reserved
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/soundcard.h>
//#include <config.h>
#include <alsa/asoundlib.h>

#include <audio-dec.h>
#include <adec-pts-mgt.h>
#include <log-print.h>
#include <alsa-out.h>
#include <amthreadpool.h>
#include "alsa-out-raw.h"
#ifdef ANDROID
#include <cutils/properties.h>
#endif

#define USE_INTERPOLATION
#define   RESAMPLE_ENALBE  1
#ifdef    RESAMPLE_ENALBE
#define   MAXRATE 2
#define   MINRATE 0.25/*up-resample need more buffer for output, make sure resample-process without overflow*/
static int rate_check=0;
static double arate=1.0;
static int resample_enable=0;
static unsigned char pcm_buffer[64 * 1024];
#endif
static snd_pcm_sframes_t (*readi_func)(snd_pcm_t *handle, void *buffer, snd_pcm_uframes_t size);
static snd_pcm_sframes_t (*writei_func)(snd_pcm_t *handle, const void *buffer, snd_pcm_uframes_t size);
static snd_pcm_sframes_t (*readn_func)(snd_pcm_t *handle, void **bufs, snd_pcm_uframes_t size);
static snd_pcm_sframes_t (*writen_func)(snd_pcm_t *handle, void **bufs, snd_pcm_uframes_t size);

static float  alsa_default_vol = 1.0;
static int hdmi_out = 0;
static int fragcount = 16;
static snd_pcm_uframes_t chunk_size = 1024;

#define   PERIOD_SIZE  1024
#define   PERIOD_NUM    4

#ifdef USE_INTERPOLATION
static int pass1_history[8][8];
#pragma align_to(64,pass1_history)
static int pass2_history[8][8];
#pragma align_to(64,pass2_history)
static short pass1_interpolation_output[0x4000];
#pragma align_to(64,pass1_interpolation_output)
static short interpolation_output[0x8000];
#pragma align_to(64,interpolation_output)
static int tv_mode = 0;
unsigned long alsa_latency(struct aml_audio_dec* audec);

static int getprop_bool(const char * path)
{
    char buf[PROPERTY_VALUE_MAX];
    int ret = -1;
    ret = property_get(path, buf, NULL);
    if (ret > 0) {
        if (strcasecmp(buf,"true") == 0 || strcmp(buf,"1") == 0)
            return 1;
    }
    return 0;
}
static inline short CLIPTOSHORT(int x)
{
    short res;
#if 0
    __asm__ __volatile__(
        "min  r0, %1, 0x7fff\r\n"
        "max r0, r0, -0x8000\r\n"
        "mov %0, r0\r\n"
        :"=r"(res)
        :"r"(x)
        :"r0"
    );
#else
    if (x > 0x7fff) {
        res = 0x7fff;
    } else if (x < -0x8000) {
        res = -0x8000;
    } else {
        res = x;
    }
#endif
    return res;
}

static int set_sysfs_type(const char *path, const char *type)
{
    int ret = -1;
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        char set_type[10] = {0};
        int length = snprintf(set_type, sizeof(set_type), "%s", type);
        if (length > 0)
            ret = write(fd, set_type, length);
        adec_print("%s , %s\n", path, set_type);
        close(fd);
    }else{
        adec_print("open hdmi-tx node:%s failed!\n", path);
    }
    return ret;
}
#ifdef RESAMPLE_ENALBE
static int
gst_aoption_rate (double * rate)
{
    char *srate = NULL;
    int ret = 0 ;
    srate=getenv("media_gst_rate");
    if (!srate)
    {
        return ret;
    }
    *rate = (double)(atof(srate));
    if ( *rate != 1 && *rate <= MAXRATE && *rate >= MINRATE)
    {
        ret =(int) (*rate);
        resample_enable = 1;
        printf("playback audio resample -> *rate %f\n", *rate);
    }
    return ret;
}
#endif
static void pcm_interpolation(int interpolation, unsigned num_channel, unsigned num_sample, short *samples)
{
    int i, k, l, ch;
    int *s;
    short *d;
    for (ch = 0; ch < num_channel; ch++) {
        s = pass1_history[ch];
        if (interpolation < 2) {
            d = interpolation_output;
        } else {
            d = pass1_interpolation_output;
        }
        for (i = 0, k = l = ch; i < num_sample; i++, k += num_channel) {
            s[0] = s[1];
            s[1] = s[2];
            s[2] = s[3];
            s[3] = s[4];
            s[4] = s[5];
            s[5] = samples[k];
            d[l] = s[2];
            l += num_channel;
            d[l] = CLIPTOSHORT((150 * (s[2] + s[3]) - 25 * (s[1] + s[4]) + 3 * (s[0] + s[5]) + 128) >> 8);
            l += num_channel;
        }
        if (interpolation >= 2) {
            s = pass2_history[ch];
            d = interpolation_output;
            for (i = 0, k = l = ch; i < num_sample * 2; i++, k += num_channel) {
                s[0] = s[1];
                s[1] = s[2];
                s[2] = s[3];
                s[3] = s[4];
                s[4] = s[5];
                s[5] = pass1_interpolation_output[k];
                d[l] = s[2];
                l += num_channel;
                d[l] = CLIPTOSHORT((150 * (s[2] + s[3]) - 25 * (s[1] + s[4]) + 3 * (s[0] + s[5]) + 128) >> 8);
                l += num_channel;
            }
        }
    }
}
#endif


static int set_params(alsa_param_t *alsa_params)
{
    snd_pcm_hw_params_t *hwparams;
    snd_pcm_sw_params_t *swparams;
    //  snd_pcm_uframes_t buffer_size;
    //  snd_pcm_uframes_t boundary;
    //  unsigned int period_time = 0;
    //  unsigned int buffer_time = 0;
    snd_pcm_uframes_t bufsize;
    int err;
    unsigned int rate;
    snd_pcm_uframes_t start_threshold, stop_threshold;
    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);

    err = snd_pcm_hw_params_any(alsa_params->handle, hwparams);
    if (err < 0) {
        adec_print("Broken configuration for this PCM: no configurations available");
        return err;
    }
    err = snd_pcm_hw_params_set_access(alsa_params->handle, hwparams,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        adec_print("Access type not available");
        return err;
    }
    if (tv_mode) {
        alsa_params->format = SND_PCM_FORMAT_S32_LE;
    }
    err = snd_pcm_hw_params_set_format(alsa_params->handle,hwparams,alsa_params->format);
    if (err < 0) {
        adec_print("Sample format non available");
        return err;
    }
    alsa_params->format = SND_PCM_FORMAT_S16_LE;
    if (tv_mode) {
        alsa_params->channelcount = 8;
    }
    err = snd_pcm_hw_params_set_channels(alsa_params->handle, hwparams, alsa_params->channelcount);
    alsa_params->channelcount = 2;
    if (err < 0) {
        adec_print("Channels count non available");
        return err;
    }

    rate = alsa_params->rate;
    err = snd_pcm_hw_params_set_rate_near(alsa_params->handle, hwparams, &alsa_params->rate, 0);
    assert(err >= 0);
#if 0
    err = snd_pcm_hw_params_get_buffer_time_max(hwparams,  &buffer_time, 0);
    assert(err >= 0);
    if (buffer_time > 500000) {
        buffer_time = 500000;
    }

    period_time = buffer_time / 4;

    err = snd_pcm_hw_params_set_period_time_near(handle, hwparams,
            &period_time, 0);
    assert(err >= 0);

    err = snd_pcm_hw_params_set_buffer_time_near(handle, hwparams,
            &buffer_time, 0);
    assert(err >= 0);

#endif
    alsa_params->bits_per_sample = snd_pcm_format_physical_width(alsa_params->format);
    //bits_per_frame = bits_per_sample * hwparams.realchanl;
    alsa_params->bits_per_frame = alsa_params->bits_per_sample * alsa_params->channelcount;
    adec_print("bits_per_sample %d,bits_per_frame %d\n",alsa_params->bits_per_sample,alsa_params->bits_per_frame);
    bufsize = PERIOD_NUM * PERIOD_SIZE;

    if (tv_mode) {
        set_sysfs_type("/sys/class/amhdmitx/amhdmitx0/aud_output_chs", "2:1");
    }

    err = snd_pcm_hw_params_set_buffer_size_near(alsa_params->handle, hwparams, &bufsize);
    if (err < 0) {
        adec_print("Unable to set	buffer	size \n");
        return err;
    }
    err = snd_pcm_hw_params_set_period_size_near(alsa_params->handle, hwparams, &chunk_size, NULL);
    if (err < 0) {
        adec_print("Unable to set period size \n");
        return err;
    }

    //err = snd_pcm_hw_params_set_periods_near(handle, hwparams, &fragcount, NULL);
    //if (err < 0) {
    //  adec_print("Unable to set periods \n");
    //  return err;
    //}

    err = snd_pcm_hw_params(alsa_params->handle, hwparams);
    if (err < 0) {
        adec_print("Unable to install hw params:");
        return err;
    }

    err = snd_pcm_hw_params_get_buffer_size(hwparams, &bufsize);
    if (err < 0) {
        adec_print("Unable to get buffersize \n");
        return err;
    }
    alsa_params->buffer_size = bufsize * alsa_params->bits_per_frame / 8;

#if 1
    err = snd_pcm_sw_params_current(alsa_params->handle, swparams);
    if (err < 0) {
        adec_print("??Unable to get sw-parameters\n");
        return err;
    }

    //err = snd_pcm_sw_params_get_boundary(swparams, &boundary);
    //if (err < 0){
    //  adec_print("Unable to get boundary\n");
    //  return err;
    //}

    //err = snd_pcm_sw_params_set_start_threshold(handle, swparams, bufsize);
    //if (err < 0) {
    //  adec_print("Unable to set start threshold \n");
    //  return err;
    //}

    //err = snd_pcm_sw_params_set_stop_threshold(handle, swparams, buffer_size);
    //if (err < 0) {
    //  adec_print("Unable to set stop threshold \n");
    //  return err;
    //}

    //  err = snd_pcm_sw_params_set_silence_size(handle, swparams, buffer_size);
    //  if (err < 0) {
    //      adec_print("Unable to set silence size \n");
    //      return err;
    //  }

    err = snd_pcm_sw_params(alsa_params->handle, swparams);
    if (err < 0) {
        adec_print("Unable to get sw-parameters\n");
        return err;
    }

    //snd_pcm_sw_params_free(swparams);
#endif


    //chunk_bytes = chunk_size * bits_per_frame / 8;

    return 0;
}

static int alsa_get_hdmi_state()
{
    return 0;
#if 0
    int fd = -1, err = 0, state = 0;
    unsigned fileSize = 32;
    char *read_buf = NULL;
    static const char *const HDMI_STATE_PATH = "/sys/class/switch/hdmi/state";

    fd = open(HDMI_STATE_PATH, O_RDONLY);
    if (fd < 0) {
        adec_print("ERROR: failed to open config file %s error: %d\n", HDMI_STATE_PATH, errno);
        close(fd);
        return -EINVAL;
    }

    read_buf = (char *)malloc(fileSize);
    if (!read_buf) {
        adec_print("Failed to malloc read_buf");
        goto OUT;
    }
    memset(read_buf, 0x0, fileSize);
    err = read(fd, read_buf, fileSize);
    if (fd < 0) {
        adec_print("ERROR: failed to read config file %s error: %d\n", HDMI_STATE_PATH, errno);
        goto OUT;
    }

    if (*read_buf == '1') {
        state = 1;
    }

OUT:
    free(read_buf);
    close(fd);
    return state;
#endif
}
/*
static int alsa_get_aml_card()
{
    int card = -1, err = 0;
    int fd = -1;
    unsigned fileSize = 512;
    char *read_buf = NULL, *pd = NULL;
    static const char *const SOUND_CARDS_PATH = "/proc/asound/cards";
    fd = open(SOUND_CARDS_PATH, O_RDONLY);
    if (fd < 0) {
        adec_print("ERROR: failed to open config file %s error: %d\n", SOUND_CARDS_PATH, errno);
        close(fd);
        return -EINVAL;
    }

    read_buf = (char *)malloc(fileSize);
    if (!read_buf) {
        adec_print("Failed to malloc read_buf");
        close(fd);
        return -ENOMEM;
    }
    memset(read_buf, 0x0, fileSize);
    err = read(fd, read_buf, fileSize);
    if (fd < 0) {
        adec_print("ERROR: failed to read config file %s error: %d\n", SOUND_CARDS_PATH, errno);
        free(read_buf);
        close(fd);
        return -EINVAL;
    }
    pd = strstr(read_buf, "AML");
    card = *(pd - 3) - '0';

OUT:
    free(read_buf);
    close(fd);
    return card;
}

static int alsa_get_spdif_port()
{
    // for pcm output,i2s/958 share the same data from i2s,so always use device 0 as i2s output
    return 0;
    int port = -1, err = 0;
    int fd = -1;
    unsigned fileSize = 512;
    char *read_buf = NULL, *pd = NULL;
    static const char *const SOUND_PCM_PATH = "/proc/asound/pcm";
    fd = open(SOUND_PCM_PATH, O_RDONLY);
    if (fd < 0) {
        adec_print("ERROR: failed to open config file %s error: %d\n", SOUND_PCM_PATH, errno);
        close(fd);
        return -EINVAL;
    }

    read_buf = (char *)malloc(fileSize);
    if (!read_buf) {
        adec_print("Failed to malloc read_buf");
        close(fd);
        return -ENOMEM;
    }
    memset(read_buf, 0x0, fileSize);
    err = read(fd, read_buf, fileSize);
    if (fd < 0) {
        adec_print("ERROR: failed to read config file %s error: %d\n", SOUND_PCM_PATH, errno);
        free(read_buf);
        close(fd);
        return -EINVAL;
    }
    pd = strstr(read_buf, "SPDIF");
    if (!pd) {
        goto OUT;
    }
    adec_print("%s  \n", pd);

    port = *(pd - 3) - '0';
    adec_print("%s  \n", (pd - 3));

OUT:
    free(read_buf);
    close(fd);
    return port;
}
*/

static void alsa_volume_ramping(alsa_param_t * alsa_param, u_char * data,size_t count){
    size_t i;
    float target_vol;
    int fade_sample = alsa_param->rate;
    adec_bool_t fade_in,fade_out;
    short *sample = (short*)data;

    /* recalculate ramping step when target volume changes */
    if (alsa_param->target_vol != alsa_param->staging_vol) {
        alsa_param->target_vol = alsa_param->staging_vol;
        alsa_param->fade = alsa_param->target_vol - alsa_param->last_vol;
    }

    target_vol = alsa_param->target_vol;

    if (alsa_param->fade != 0) {
        /* volume ramping in 1 second */
        for (i = 0; i < count * 2; i = i + 2) {
            if (target_vol > alsa_param->last_vol) {
                alsa_param->last_vol += alsa_param->fade / fade_sample;
                if (alsa_param->last_vol >= target_vol) {
                    alsa_param->last_vol = target_vol;
                    alsa_param->fade = 0;
                }
            } else if (target_vol < alsa_param->last_vol) {
                alsa_param->last_vol += alsa_param->fade / fade_sample;
                if (alsa_param->last_vol <= target_vol) {
                    alsa_param->last_vol = target_vol;
                    alsa_param->fade = 0;
                }
            }

            sample[i] = (short)(alsa_param->last_vol * (float)sample[i]);
            sample[i + 1] = (short)(alsa_param->last_vol * (float)sample[i + 1]);
        }
    } else {
        for (i = 0; i < count * 2; i = i + 2) {
            sample[i] = (short)(target_vol * (float)sample[i]);
            sample[i + 1] = (short)(target_vol * (float)sample[i + 1]);
        }
    }
}

static size_t pcm_write(alsa_param_t * alsa_param, u_char * data, size_t count)
{
    snd_pcm_sframes_t r;
    size_t result = 0;
    int i;
    short *sample = (short*)data;
    /*
        if (count < chunk_size) {
            snd_pcm_format_set_silence(hwparams.format, data + count * bits_per_frame / 8, (chunk_size - count) * hwparams.channels);
            count = chunk_size;
        }
    */
    /* volume control for bootplayer  */
    if (alsa_default_vol  != 1.0) {
        for (i  = 0;i < count*2;i++) {
            sample[i] = (short)(alsa_default_vol*(float)sample[i]);
        }
    } else {
        alsa_volume_ramping(alsa_param,data,count);
    }
    // dump  pcm data here
#if 0
        FILE *fp1=fopen("/data/audio_out.pcm","a+");
        if (fp1) {
            int flen=fwrite((char *)data,1,count*2*2,fp1);
            adec_print("flen = %d---outlen=%d ", flen, count*2*2);
            fclose(fp1);
        }else{
            adec_print("could not open file:audio_out.pcm");
        }
#endif
    int *tmp_buffer = NULL;
    int bits_per_frame = alsa_param->bits_per_frame;
    if (tv_mode) {
        tmp_buffer = (int*)malloc(count*8*4);
        if (tmp_buffer == NULL) {
            adec_print("malloc tmp_buffer failed\n");
            return -1;
        }
        int i;
        int out_frames = count;
        short  *in_buffer = (short*)data;
        for (i = 0; i < out_frames; i ++) {
            tmp_buffer[8*i] = ((int)(in_buffer[2*i])) << 16;
            tmp_buffer[8*i + 1] = ((int)(in_buffer[2*i + 1])) << 16;
            tmp_buffer[8*i + 2] = ((int)(in_buffer[2*i])) << 16;
            tmp_buffer[8*i + 3] = ((int)(in_buffer[2*i + 1])) << 16;
            tmp_buffer[8*i + 4] = 0;
            tmp_buffer[8*i + 5] = 0;
            tmp_buffer[8*i + 6] = 0;
            tmp_buffer[8*i + 7] = 0;
        }
        data = (char*)tmp_buffer;
        bits_per_frame = bits_per_frame*8;//8ch,32bit
    }
    pthread_mutex_lock(&alsa_param->playback_mutex);
    if (alsa_param->pause_flag) {
        result = 0;
        goto  done;
    }
    while (count > 0) {
        r = writei_func(alsa_param->handle, data, count);

        if (r == -EINTR) {
            r = 0;
        }
        if (r == -ESTRPIPE) {
            while ((r = snd_pcm_resume(alsa_param->handle)) == -EAGAIN) {
                amthreadpool_thread_usleep(1000);
            }
        }

        if (r < 0) {
            printf("xun in\n");
            if ((r = snd_pcm_prepare(alsa_param->handle)) < 0) {
                result = 0;
                goto  done;
            }
        }

        if (r > 0) {
            result += r;
            count -= r;
            data += r * bits_per_frame / 8;
        }
    }
done:
    pthread_mutex_unlock(&alsa_param->playback_mutex);
    if (tmp_buffer) {
        free(tmp_buffer);
    }
    return result;
}

static unsigned oversample_play(alsa_param_t * alsa_param, char * src, unsigned count)
{
    int frames = 0;
    int ret, i;
    char output_buffer[64 * 1024];
    unsigned short * to, *from;
    to = (unsigned short *)output_buffer;
    from = (unsigned short *)src;

    if (alsa_param->realchanl == 2) {
        if (alsa_param->oversample == -1) {
            frames = count * 8 / alsa_param->bits_per_frame;
            frames = frames & (~(32 - 1));
            for (i = 0; i < (frames * 2); i += 4) { // i for sample
                *to++ = *from++;
                *to ++ = *from++;
                from += 2;
            }
            ret = pcm_write(alsa_param, output_buffer, frames / 2);
            ret = ret * alsa_param->bits_per_frame / 8;
            ret = ret * 2;
        } else if (alsa_param->oversample == 1) {
            frames = count * 8 / alsa_param->bits_per_frame;
            frames = frames & (~(16 - 1));
#ifdef USE_INTERPOLATION
            pcm_interpolation(1, alsa_param->realchanl, frames, (short*)src);
            memcpy(output_buffer, interpolation_output, (frames * alsa_param->bits_per_frame / 4));
#else
            short l, r;
            for (i = 0; i < (frames * 2); i += 2) {
                l = *from++;
                r = *from++;
                *to++ = l;
                *to++ = r;
                *to++ = l;
                *to++ = r;
            }
#endif
            ret = pcm_write(alsa_param, output_buffer, frames * 2);
            ret = ret * alsa_param->bits_per_frame / 8;
            ret = ret / 2;
        } else if (alsa_param->oversample == 2) {
            frames = count * 8 / alsa_param->bits_per_frame;
            frames = frames & (~(8 - 1));
#ifdef USE_INTERPOLATION
            pcm_interpolation(2, alsa_param->realchanl, frames, (short*)src);
            memcpy(output_buffer, interpolation_output, (frames * alsa_param->bits_per_frame / 2));
#else
            short l, r;
            for (i = 0; i < (frames * 2); i += 2) {
                l = *from++;
                r = *from++;
                *to++ = l;
                *to++ = r;
                *to++ = l;
                *to++ = r;
                *to++ = l;
                *to++ = r;
                *to++ = l;
                *to++ = r;
            }
#endif
            ret = pcm_write(alsa_param, output_buffer, frames * 4);
            ret = ret * alsa_param->bits_per_frame / 8;
            ret = ret / 4;
        }
    } else if (alsa_param->realchanl == 1) {
        if (alsa_param->oversample == -1) {
            frames = count * 8 / alsa_param->bits_per_frame;
            frames = frames & (~(32 - 1));
            for (i = 0; i < (frames * 2); i += 2) {
                *to++ = *from;
                *to++ = *from++;
                from++;
            }
            ret = pcm_write(alsa_param, output_buffer, frames);
            ret = ret * alsa_param->bits_per_frame / 8;
        } else if (alsa_param->oversample == 0) {
            frames = count * 8 / (alsa_param->bits_per_frame >> 1);
            frames = frames & (~(16 - 1));
            for (i = 0; i < (frames); i++) {
                *to++ = *from;
                *to++ = *from++;
            }
            ret = pcm_write(alsa_param, output_buffer, frames);
            ret = ret * (alsa_param->bits_per_frame) / 8;
            ret = ret / 2;
        } else if (alsa_param->oversample == 1) {
            frames = count * 8 / (alsa_param->bits_per_frame >> 1);
            frames = frames & (~(8 - 1));
#ifdef USE_INTERPOLATION
            pcm_interpolation(1, alsa_param->realchanl, frames, (short*)src);
            from = (unsigned short*)interpolation_output;
            for (i = 0; i < (frames * 2); i++) {
                *to++ = *from;
                *to++ = *from++;
            }
#else
            for (i = 0; i < (frames); i++) {
                *to++ = *from;
                *to++ = *from;
                *to++ = *from;
                *to++ = *from++;
            }
#endif
            ret = pcm_write(alsa_param, output_buffer, frames * 2);
            ret = ret * (alsa_param->bits_per_frame) / 8;
            ret = ret / 4;
        } else if (alsa_param->oversample == 2) {
            frames = count * 8 / (alsa_param->bits_per_frame >> 1);
            frames = frames & (~(8 - 1));
#ifdef USE_INTERPOLATION
            pcm_interpolation(2, alsa_param->realchanl, frames, (short*)src);
            from = (unsigned short*)interpolation_output;
            for (i = 0; i < (frames * 4); i++) {
                *to++ = *from;
                *to++ = *from++;
            }
#else
            for (i = 0; i < (frames); i++) {
                *to++ = *from;
                *to++ = *from;
                *to++ = *from;
                *to++ = *from;
                *to++ = *from;
                *to++ = *from;
                *to++ = *from;
                *to++ = *from++;
            }
#endif
            ret = pcm_write(alsa_param, output_buffer, frames * 4);
            ret = ret * (alsa_param->bits_per_frame) / 8;
            ret = ret / 8;
        }
    }

    return ret;
}

static int alsa_play(alsa_param_t * alsa_param, char * data, unsigned len)
{
    size_t l = 0, r;

    if (!alsa_param->flag) {
        l = len * 8 / alsa_param->bits_per_frame;
        l = l & (~(32 - 1)); /*driver only support  32 frames each time */
        r = pcm_write(alsa_param, data, l);
        r = r * alsa_param->bits_per_frame / 8;
    } else {
        r = oversample_play(alsa_param, data, len);
    }

    return r ;
}
/*
static int alsa_swtich_port(alsa_param_t *alsa_params, int card, int port)
{
    char dev[10] = {0};
    adec_print("card = %d, port = %d\n", card, port);
    sprintf(dev, "hw:%d,%d", (card >= 0) ? card : 0, (port >= 0) ? port : 0);
    pthread_mutex_lock(&alsa_params->playback_mutex);
    snd_pcm_drop(alsa_params->handle);
    snd_pcm_close(alsa_params->handle);
    alsa_params->handle = NULL;
    int err = snd_pcm_open(&alsa_params->handle, dev, SND_PCM_STREAM_PLAYBACK, 0);

    if (err < 0) {
        adec_print("audio open error: %s", snd_strerror(err));
        pthread_mutex_unlock(&alsa_params->playback_mutex);
        return -1;
    }

    set_params(alsa_params);
    pthread_mutex_unlock(&alsa_params->playback_mutex);

    return 0;
}
*/
static void *alsa_playback_loop(void *args)
{
    int len = 0;
    int offset = 0;
    aml_audio_dec_t *audec;
    alsa_param_t *alsa_params;
    char value[PROPERTY_VALUE_MAX]={0};
    audec = (aml_audio_dec_t *)args;
    alsa_params = (alsa_param_t *)audec->aout_ops.private_data;
    unsigned char *decode_buffer = (unsigned char *)(((unsigned long)alsa_params->decode_buffer + 32) & (~0x1f));
    unsigned char *buffer = decode_buffer;
    audec->render_position = 0;
#ifdef RESAMPLE_ENALBE
    unsigned char *feed_buffer = pcm_buffer;
    int feed_remain = 0;
#endif

    // bootplayer default volume configuration
    if (property_get("media.amplayer.boot_vol",value,NULL) > 0) {
        alsa_default_vol = atof(value);
        if (alsa_default_vol < 0.0 || alsa_default_vol > 1.0 ) {
            adec_print("wrong alsa default volume %f, set to 1.0 \n",alsa_default_vol);
            alsa_default_vol  = 1.0;
        }
    }
    adec_print("alsa default volume  %f  \n",alsa_default_vol);
    /*   pthread_mutex_init(&alsa_params->playback_mutex, NULL);
       pthread_cond_init(&alsa_params->playback_cond, NULL);*/

    while (!alsa_params->wait_flag && alsa_params->stop_flag == 0) {
        amthreadpool_thread_usleep(10000);
    }

    adec_print("alsa playback loop start to run !\n");

    while (!alsa_params->stop_flag) {
#if 0
    if (hdmi_out == 0) {
            adec_print("===dynmiac get hdmi plugin state===\n");
            if (alsa_get_hdmi_state() == 1) {
                if (alsa_swtich_port(alsa_params, alsa_get_aml_card(), alsa_get_spdif_port()) == -1) {
                    adec_print("switch to hdmi port failed.\n");
                    goto exit;
                }

                hdmi_out = 1;
                adec_print("[%s,%d]get hdmi device, use hdmi device \n", __FUNCTION__, __LINE__);
            }
        } else if (alsa_get_hdmi_state() == 0) {
            if (alsa_swtich_port(alsa_params, alsa_get_aml_card(), 0) == -1) {
                adec_print("switch to default port failed.\n");
                goto exit;
            }

            hdmi_out = 0;
            adec_print("[%s,%d]get default device, use default device \n", __FUNCTION__, __LINE__);
        }
#endif
      if (alsa_params->pause_flag ||
          (audec->use_render_add && !audec->apts_start_flag)) {
        amthreadpool_thread_usleep(10000);
        continue;
      }
      if (len < 128*2) {
        if (offset > 0) {
          if (len > 0) {
            memmove(buffer, buffer + offset, len);
          }
          offset = 0;
        }
#ifdef RESAMPLE_ENALBE
        if (buffer == feed_buffer) {
          feed_remain = len;
          len = 0;
          buffer = decode_buffer;
        }
#endif
        int readlen = audec->adsp_ops.dsp_read(&audec->adsp_ops, (buffer + len),
                                        (OUTPUT_BUFFER_SIZE - len));
        if (readlen > 0) {
          len += readlen;
          audec->pcm_bytes_readed += readlen;
        }
        continue;
      }
#ifdef RESAMPLE_ENALBE
      if ((buffer != feed_buffer) && resample_enable && ((int)(arate * 1000) != 1000)) {
        int resample_len = af_resample_in_buffer( buffer, len, feed_buffer+feed_remain, audec->channels, audec->samplerate, arate);
        //printf("resample %d bytes, at rate %f, return %d bytes, offset %d\n", len, arate, resample_len, feed_remain);
        buffer = feed_buffer;
        len = resample_len + feed_remain;
      }
#endif
      int feedlen = alsa_play(alsa_params, (buffer + offset), len);
      if (feedlen >= 0) {
        offset += feedlen;
        len -= feedlen;
        //audec->alsa_cache_size = len / arate;
        audec->render_position += feedlen *arate / (alsa_params->bits_per_frame /4);
        if (audec->render_position / alsa_params->rate - alsa_latency(audec) > 0) {
            audec->apts64 = (audec->render_position / alsa_params->rate - alsa_latency(audec)) * 90;
        } else {
            audec->apts64 = 0;
        }
      } else {
        offset = 0;
        len = 0;
      }
    }
exit:

    adec_print("Exit alsa playback loop !\n");
    pthread_exit(NULL);
    return NULL;
}

/**
 * \brief output initialization
 * \param audec pointer to audec
 * \return 0 on success otherwise negative error code
 */
int alsa_init(struct aml_audio_dec* audec)
{
    adec_print("alsa out init");
    char sound_card_dev[10] = {0};
    int sound_card_id = 0;
    int sound_dev_id = 0;
    int err;
    pthread_t tid;
    alsa_param_t *alsa_param;
    audio_out_operations_t *out_ops = &audec->aout_ops;
    int dgraw = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");
    tv_mode = getprop_bool("ro.platform.has.tvuimode");
#ifdef RESAMPLE_ENALBE
    int ratei = out_ops->track_rate * 1000000;
    if (ratei == 8800000) {
      arate = 1.0;
      resample_enable = 0;
    } else {
      out_ops->set_track_rate(audec, (void*)ratei);
    }
#endif
    alsa_param = (alsa_param_t *)malloc(sizeof(alsa_param_t));
    if (!alsa_param) {
        adec_print("alloc alsa_param failed, not enough memory!");
        return -1;
    }
    memset(alsa_param, 0, sizeof(alsa_param_t));

    if (audec->samplerate >= (176400 + 192000) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = -1;
        alsa_param->rate = 96000;
    } else if (audec->samplerate >= (96000 + 176400) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = -1;
        alsa_param->rate = 88200;
    } else if (audec->samplerate >= (88200 + 96000) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = -1;
        alsa_param->rate = 48000;
    } else if (audec->samplerate >= (64000 + 88200) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = -1;
        alsa_param->rate = 44100;
    } else if (audec->samplerate >= (48000 + 64000) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = -1;
        alsa_param->rate = 32000;
    } else if (audec->samplerate >= (44100 + 48000) / 2) {
        alsa_param->oversample = 0;
        alsa_param->rate = 48000;
        if (audec->channels == 1) {
            alsa_param->flag = 1;
        } else if (audec->channels == 2) {
            alsa_param->flag = 0;
        }
    } else if (audec->samplerate >= (32000 + 44100) / 2) {
        alsa_param->oversample = 0;
        alsa_param->rate = 44100;
        if (audec->channels == 1) {
            alsa_param->flag = 1;
        } else if (audec->channels == 2) {
            alsa_param->flag = 0;
        }
    } else if (audec->samplerate >= (24000 + 32000) / 2) {
        alsa_param->oversample = 0;
        alsa_param->rate = 32000;
        if (audec->channels == 1) {
            alsa_param->flag = 1;
        } else if (audec->channels == 2) {
            alsa_param->flag = 0;
        }
    } else if (audec->samplerate >= (22050 + 24000) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = 1;
        alsa_param->rate = 48000;
    } else if (audec->samplerate >= (16000 + 22050) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = 1;
        alsa_param->rate = 44100;
    } else if (audec->samplerate >= (12000 + 16000) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = 1;
        alsa_param->rate = 32000;
    } else if (audec->samplerate >= (11025 + 12000) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = 2;
        alsa_param->rate = 48000;
    } else if (audec->samplerate >= (8000 + 11025) / 2) {
        alsa_param->flag = 1;
        alsa_param->oversample = 2;
        alsa_param->rate = 44100;
    } else {
        alsa_param->flag = 1;
        alsa_param->oversample = 2;
        alsa_param->rate = 32000;
    }

    alsa_param->channelcount = 2;
    alsa_param->realchanl = audec->channels;
    //alsa_param->rate = audec->samplerate;
    alsa_param->format = SND_PCM_FORMAT_S16_LE;
    alsa_param->wait_flag = 0;
    alsa_param->staging_vol = 1.0f;
    alsa_param->target_vol = 1.0f;
    alsa_param->last_vol = 1.0f;
    alsa_param->fade = 0;

#ifdef USE_INTERPOLATION
    memset(pass1_history, 0, 64 * sizeof(int));
    memset(pass2_history, 0, 64 * sizeof(int));
#endif

    if (((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw))&& \
        ((ACODEC_FMT_AC3 == audec->format) || (ACODEC_FMT_EAC3 == audec->format)|| \
        (ACODEC_FMT_DTS == audec->format))) {

        sound_card_id = alsa_get_aml_card();
        if (sound_card_id  < 0) {
            sound_card_id = 0;
            adec_print("[%s::%d]--[get aml card fail, use default]\n",__FUNCTION__, __LINE__);
        }
        adec_print("[%s::%d]--[alsa_get_aml_card return:%d]\n",__FUNCTION__, __LINE__,sound_card_id);

        sound_dev_id = alsa_get_spdif_port();
        if (sound_dev_id < 0) {
            sound_dev_id = 0;
            adec_print("[%s::%d]--[get aml card device fail, use default]\n",__FUNCTION__, __LINE__);
        }

        adec_print("[%s::%d]--[sound_dev_id(spdif) return :%d]\n",__FUNCTION__, __LINE__,sound_dev_id);

        if (0 == sound_dev_id) {
            sound_dev_id = 1;
        } else if(1 == sound_dev_id){
            sound_dev_id = 0;
        }
        adec_print("[%s::%d]--[sound_dev_id convert to(pcm):%d]\n",__FUNCTION__, __LINE__,sound_dev_id);

        sprintf(sound_card_dev, "hw:%d,%d", sound_card_id, sound_dev_id);
    }else {
        memcpy(sound_card_dev, PCM_DEVICE_DEFAULT, sizeof(PCM_DEVICE_DEFAULT));
        amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",0);//set output codec type as pcm
    }
    adec_print("[%s::%d]--[sound_card_dev:%s]\n",__FUNCTION__, __LINE__,sound_card_dev);
    err = snd_pcm_open(&alsa_param->handle, sound_card_dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        adec_print("[%s::%d]--[audio open error: %s]\n", __FUNCTION__, __LINE__,snd_strerror(err));
        return -1;
    } else {
        adec_print("[%s::%d]--[audio open(snd_pcm_open) successfully]\n", __FUNCTION__, __LINE__);
    }
    readi_func = snd_pcm_readi;
    writei_func = snd_pcm_writei;
    readn_func = snd_pcm_readn;
    writen_func = snd_pcm_writen;

    set_params(alsa_param);

    out_ops->private_data = (void *)alsa_param;

    /*TODO:  create play thread */
    pthread_mutex_init(&alsa_param->playback_mutex, NULL);
    pthread_cond_init(&alsa_param->playback_cond, NULL);
    err = amthreadpool_pthread_create(&tid, NULL, (void *)alsa_playback_loop, (void *)audec);
    if (err != 0) {
        adec_print("alsa_playback_loop thread create failed!");
        snd_pcm_close(alsa_param->handle);
        return -1;
    }
    pthread_setname_np(tid, "ALSAOUTLOOP");

    adec_print("Create alsa playback loop thread success ! tid = %d\n", tid);

    alsa_param->playback_tid = tid;
    //ac3 and eac3 passthrough
    if (((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw)) && \
        ((ACODEC_FMT_AC3 == audec->format) || (ACODEC_FMT_EAC3 == audec->format) || \
        (ACODEC_FMT_DTS == audec->format))) {

        int err;
        err = alsa_init_raw(audec);
        if (err != 0) {
            adec_print("alsa_init_raw return error:%d\n", err);
            snd_pcm_close(alsa_param->handle);
            return -1;
        }

    }

    return 0;
}

/**
 * \brief start output
 * \param audec pointer to audec
 * \return 0 on success otherwise negative error code
 *
 * Call alsa_start(), then the callback will start being called.
 */
int alsa_start(struct aml_audio_dec* audec)
{
    adec_print("alsa out start!\n");

    audio_out_operations_t *out_ops = &audec->aout_ops;
    alsa_param_t *alsa_param = (alsa_param_t *)out_ops->private_data;
    int dgraw = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");

    pthread_mutex_lock(&alsa_param->playback_mutex);
    adec_print("yvonne pthread_cond_signalalsa_param->wait_flag=1\n");
    alsa_param->wait_flag = 1; //yvonneadded
    pthread_cond_signal(&alsa_param->playback_cond);
    pthread_mutex_unlock(&alsa_param->playback_mutex);
    //ac3 and eac3 passthrough
    if (((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw)) && \
        ((ACODEC_FMT_AC3 == audec->format) || (ACODEC_FMT_EAC3 == audec->format) || \
        (ACODEC_FMT_DTS == audec->format) )) {
        int err = alsa_start_raw(audec);
        if (err) {
            printf("alsa_start_raw return  error: %d\n", err);
        }
    }
    adec_print("exit alsa out start!\n");

    return 0;
}

/**
 * \brief pause output
 * \param audec pointer to audec
 * \return 0 on success otherwise negative error code
 */
int alsa_pause(struct aml_audio_dec* audec)
{
    adec_print("alsa out pause\n");

    int res;
    alsa_param_t *alsa_params;
    int dgraw = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");
    if (((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw)) && \
        ((ACODEC_FMT_AC3 == audec->format) || (ACODEC_FMT_EAC3 == audec->format) || \
        (ACODEC_FMT_DTS == audec->format)) ) {
        int err = alsa_pause_raw(audec);
        if (err) {
              printf("alsa_pause_raw return  error: %d\n", err);
        }
        return err;
    }
    alsa_params = (alsa_param_t *)audec->aout_ops.private_data;
    // pcm_write will lock and unlock the mutex in a busy loop
    // in that case this thread do not have much chance to lock the mutex
    // sometime it will take 2~30 seconds to acquire the lock
    // move pause_flag outside of the lock can notify pcm_write immediately
    alsa_params->pause_flag = 1;
    pthread_mutex_lock(&alsa_params->playback_mutex);

    while ((res = snd_pcm_pause(alsa_params->handle, 1)) == -EAGAIN) {
        sleep(1);
    }
    pthread_mutex_unlock(&alsa_params->playback_mutex);
    adec_print("exit alsa out pause\n");

    return res;
}

/**
 * \brief resume output
 * \param audec pointer to audec
 * \return 0 on success otherwise negative error code
 */
int alsa_resume(struct aml_audio_dec* audec)
{
    adec_print("alsa out rsume\n");

    int res;
    alsa_param_t *alsa_params;

    //ac3 and eac3 passthrough
    int dgraw = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");
    if (((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw)) && \
        ((ACODEC_FMT_AC3 == audec->format) || (ACODEC_FMT_EAC3 == audec->format) || \
        (ACODEC_FMT_DTS == audec->format)) ) {
        int err = alsa_resume_raw(audec);
        if (err) {
              printf("alsa_resume_raw return  error: %d\n", err);
        }
        return err;
    }
    alsa_params = (alsa_param_t *)audec->aout_ops.private_data;
    pthread_mutex_lock(&alsa_params->playback_mutex);

    alsa_params->pause_flag = 0;
    while ((res = snd_pcm_resume(alsa_params->handle)) == -EAGAIN) {
        sleep(1);
    }
    snd_pcm_prepare(alsa_params->handle);
    pthread_mutex_unlock(&alsa_params->playback_mutex);

    return res;
}

/**
 * \brief stop output
 * \param audec pointer to audec
 * \return 0 on success otherwise negative error code
 */
int alsa_stop(struct aml_audio_dec* audec)
{
    adec_print("enter alsa out stop\n");

    alsa_param_t *alsa_params;

    alsa_params = (alsa_param_t *)audec->aout_ops.private_data;
    int dgraw = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");
    int res;
    if (alsa_params) {
        pthread_mutex_lock(&alsa_params->playback_mutex);
        //resume the card, otherwise the output will block
        if (alsa_params->pause_flag == 1) {
            while ((res = snd_pcm_pause(alsa_params->handle, 0)) == -EAGAIN) {
                 sleep(1);
            }
        }
        alsa_params->pause_flag = 0;
        alsa_params->stop_flag = 1;
        //alsa_params->wait_flag = 0;
    //ac3 and eac3 passthrough
    if (((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw)) && \
        ((ACODEC_FMT_AC3 == audec->format) || (ACODEC_FMT_EAC3 == audec->format) || \
        (ACODEC_FMT_DTS == audec->format)) ) {

        adec_print("enter alsa_stop_raw step\n");
        int err = alsa_stop_raw(audec);
        if (err) {
            printf("alsa_stop_raw return  error: %d\n", err);
        }
    }
        pthread_mutex_unlock(&alsa_params->playback_mutex);
        //playback loop acquire playback_mutex lock, move signal out of lock
        pthread_cond_signal(&alsa_params->playback_cond);
        amthreadpool_pthread_join(alsa_params->playback_tid, NULL);
        pthread_cond_destroy(&alsa_params->playback_cond);


        snd_pcm_drop(alsa_params->handle);
        snd_pcm_close(alsa_params->handle);
        pthread_mutex_destroy(&alsa_params->playback_mutex);

        free(alsa_params);
        audec->aout_ops.private_data = NULL;
    }

    adec_print("exit alsa out stop\n");

    return 0;
}

static int alsa_get_space(alsa_param_t * alsa_param)
{
    snd_pcm_status_t *status;
    int ret;
    int bits_per_sample = alsa_param->bits_per_sample;
    snd_pcm_status_alloca(&status);
    if ((ret = snd_pcm_status(alsa_param->handle, status)) < 0) {
        adec_print("Cannot get pcm status \n");
        return 0;
    }
    ret = snd_pcm_status_get_avail(status) * alsa_param->bits_per_sample / 8;
    if (ret > alsa_param->buffer_size) {
        ret = alsa_param->buffer_size;
    }
    return ret;
}

/**
 * \brief get output latency in ms
 * \param audec pointer to audec
 * \return output latency
 */
unsigned long alsa_latency(struct aml_audio_dec* audec)
{
    int buffered_data;
    snd_pcm_sframes_t sample_num;
    alsa_param_t *alsa_param = (alsa_param_t *)audec->aout_ops.private_data;
    int bits_per_sample = alsa_param->bits_per_sample;
    if (tv_mode) {
        bits_per_sample = bits_per_sample*4;
    }
#if 0
    //buffered_data = alsa_param->buffer_size - alsa_get_space(alsa_param);
    //sample_num = buffered_data / (alsa_param->channelcount * (bits_per_sample / 8)); /*16/2*/
#else
    int ret = snd_pcm_delay(alsa_param->handle,&sample_num);
    if (ret != 0) {
       adec_print("Cannot get pcm delay \n");
       return 0;
    }
    buffered_data = sample_num * alsa_param->bits_per_sample / 8;
    //adec_print("alsa_param->buffer_size :%d buffered_data:%d \n",alsa_param->buffer_size,buffered_data);
#endif
    if (buffered_data > alsa_param->buffer_size) {
        sample_num = alsa_param->buffer_size/( alsa_param->bits_per_sample / 8);
    }
    return ((sample_num * 1000) / alsa_param->rate);
}

static int alsa_set_volume(struct aml_audio_dec* audec, float vol){
    alsa_param_t *alsa_param = (alsa_param_t *)audec->aout_ops.private_data;
    alsa_param->staging_vol = vol;
    return 0;
}
static int alsa_mute(struct aml_audio_dec* audec, adec_bool_t en){

    int dgraw = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");
    //ac3 and eac3 passthrough
    if (((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw)) && \
    ((ACODEC_FMT_AC3 == audec->format) || (ACODEC_FMT_EAC3 == audec->format) || \
    (ACODEC_FMT_DTS == audec->format)) ) {
        int err = alsa_mute_raw(audec,en);
        if (err) {
            adec_print("alsa_mute_raw return  error: %d\n", err);
        }
    }
    return 0;
}

#ifdef RESAMPLE_ENALBE
static int alsa_set_track_rate(struct aml_audio_dec *audec, void *rate) {
  int ratei = (int)rate;
  resample_enable = 1;
  if (ratei >= 2000000) {
    ratei = 2000000;
    arate = 2.0;
  } else if (ratei >= 1500000) {
    ratei = 1500000;
    arate = 1.5;
  } else if (ratei >= 1250000) {
    ratei = 1250000;
    arate = 1.25;
  } else if (ratei >= 1000000) {
    ratei = 1000000;
    arate = 1.0;
  } else if (ratei >= 500000) {
    ratei = 500000;
    arate = 0.5;
  } else if (ratei >= 250000) {
    ratei = 250000;
    arate = 0.25;
  } else {
    ratei = 1;
    arate = 1.0;
    resample_enable = 0;
  }
  audec->aout_ops.track_rate = arate;
  amsysfs_set_sysfs_int("/sys/class/video/vsync_slow_factor", ratei);
}
#endif

/**
 * \brief get output handle
 * \param audec pointer to audec
 */
void get_output_func(struct aml_audio_dec* audec)
{
    audio_out_operations_t *out_ops = &audec->aout_ops;

    out_ops->init = alsa_init;
    out_ops->start = alsa_start;
    out_ops->pause = alsa_pause;
    out_ops->resume = alsa_resume;
    out_ops->mute = alsa_mute;
    out_ops->stop = alsa_stop;
    out_ops->set_volume = alsa_set_volume;
    out_ops->latency = alsa_latency;
    out_ops->track_rate = 8.8f;
#ifdef RESAMPLE_ENALBE
    out_ops->set_track_rate = alsa_set_track_rate;
    amsysfs_set_sysfs_int("/sys/class/video/vsync_slow_factor", 1);
#endif
}
