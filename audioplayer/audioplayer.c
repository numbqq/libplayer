/*
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */


/**************************************************
* example based on amcodec
**************************************************/
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <codec.h>
#include <stdbool.h>
#include <ctype.h>
#include <execinfo.h>
#include <unistd.h>

#define READ_SIZE (64 * 1024)

int main(int argc, char *argv[])
{
    int ret = CODEC_ERROR_NONE;
    char buffer[READ_SIZE];
    struct buf_status buf_stat;
    codec_para_t a_codec_para;
    FILE* fp = NULL;
    codec_para_t *pcodec;
    int type = 0;

    if (argc < 3) {
        printf("Usage: mp3player <filename> <type 0:mpeg 1:aac 2:AC3 3:EAC3 4:dts>\n");
        return -1;
    }

    type = atoi(argv[2]);

    codec_audio_basic_init();

    pcodec = &a_codec_para;
    memset(pcodec, 0, sizeof(codec_para_t));
    if (type == 0) {
       pcodec->audio_type = AFORMAT_MPEG;
    } else if(type == 1){
       pcodec->audio_type = AFORMAT_AAC;
    } else if (type == 2){
       pcodec->audio_type = AFORMAT_AC3;
    } else if (type == 3) {
      pcodec->audio_type = AFORMAT_EAC3;
    }else if (type == 4) {
      pcodec->audio_type = AFORMAT_DTS;
    }
    //pcodec->audio_type = (type == 0) ? AFORMAT_MPEG : AFORMAT_AAC;
    pcodec->stream_type = STREAM_TYPE_ES_AUDIO;
    pcodec->has_audio = 1;
    pcodec->audio_channels = 2;
    pcodec->audio_samplerate = 44100;
    pcodec->noblock = 1;
    pcodec->audio_info.channels = 2;
    pcodec->audio_info.sample_rate = 48000;

    printf("file %s to be played\n", argv[1]);

    if ((fp = fopen(argv[1], "rb")) == NULL) {
        printf("open file error!\n");
        return -1;
    }

    ret = codec_init(pcodec);
    if (ret != CODEC_ERROR_NONE) {
        printf("codec init failed, ret=-0x%x", -ret);
        return -1;
    }

    codec_checkin_pts(pcodec, 0);

    while (!feof(fp)) {
        int isize = 0;
        int Readlen;

        Readlen = fread(buffer, 1, READ_SIZE, fp);
        if (Readlen <= 0) {
            printf("read file error!\n");
            break;
        }

        do {
            ret = codec_write(pcodec, buffer + isize, Readlen - isize);
            if (ret < 0) {
                if (errno != EAGAIN) {
                    printf("write data failed, errno %d\n", errno);
                    goto error;
                } else {
                    continue;
                }
            } else {
                printf("Send %d bytes\n", ret);
                isize += ret;
            }
        } while (isize < Readlen);
    }

    do {
        ret = codec_get_abuf_state(pcodec, &buf_stat);
        if (ret != 0) {
            printf("codec_get_abuf_state error: %x\n", -ret);
            goto error;
        }
    } while (buf_stat.data_len > 0x100);

error:
    codec_close(pcodec);
    fclose(fp);

    return 0;
}

