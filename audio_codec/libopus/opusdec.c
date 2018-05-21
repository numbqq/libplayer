/*
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 * Description: adpcm decoder
 */
/*
** @file
 * Opus decoder
 *
 * This file is part of audio_codec.
*/

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <adec-armdec-mgt.h>
#include <audio-dec.h>

#define audio_codec_print printf
#define MULTI_DECODER
#ifdef MULTI_DECODER
#include "opus_multistream.h"
#else
#include "opus.h"
#endif
// Maximum packet size used in Xiph's opusdec.
#define MAX_OPUS_OUTPUT_PACKET_SIZE_SAMPLES (960*6)

// Default audio output channel layout. Used to initialize |stream_map| in
// OpusHeader, and passed to opus_multistream_decoder_create() when the header
// does not contain mapping information. The values are valid only for mono and
// stereo output: Opus streams with more than 2 channels require a stream map.
#define MAX_CHANNELS_DEFAULT_LAYOUT 2

// Opus uses Vorbis channel mapping, and Vorbis channel mapping specifies
// mappings for up to 8 channels. This information is part of the Vorbis I
// Specification:
// http://www.xiph.org/vorbis/doc/Vorbis_I_spec.html
#define MAX_CHANNELS 8

#define OPUS_RATE 48000

typedef struct {
    int rate;
    int channels;
    int skip_samples;
    int channel_mapping;
    int num_streams;
    int num_coupled;
    int16_t gain_db;
    uint8_t stream_map[8];
} opus_header_t;

typedef struct {
    int64_t mCodecDelay;
    int64_t mSeekPreRoll;
    int64_t mSamplesToDiscard;
    opus_header_t* header;
    void* adec_ops;
} opus_priv_data_t;

static uint64_t ns_to_samples(uint64_t ns, int kRate) {
    return ns * kRate / 1000000000;
}

static uint16_t ReadLE16(const uint8_t *data, size_t data_size,
                         uint32_t read_offset) {
    if (read_offset + 1 > data_size)
        return 0;
    uint16_t val;
    val = data[read_offset];
    val |= data[read_offset + 1] << 8;
    return val;
}

// Parses Opus Header. Header spec: http://wiki.xiph.org/OggOpus#ID_Header
static int ParseOpusHeader(const char *data, size_t data_size,
                            opus_header_t* header) {
    // Size of the Opus header excluding optional mapping information.
    const size_t kOpusHeaderSize = 19;

    // Offset to the channel count byte in the Opus header.
    const size_t kOpusHeaderChannelsOffset = 9;

    // Offset to the pre-skip value in the Opus header.
    const size_t kOpusHeaderSkipSamplesOffset = 10;

    // Offset to the gain value in the Opus header.
    const size_t kOpusHeaderGainOffset = 16;

    // Offset to the channel mapping byte in the Opus header.
    const size_t kOpusHeaderChannelMappingOffset = 18;

    // Opus Header contains a stream map. The mapping values are in the header
    // beyond the always present |kOpusHeaderSize| bytes of data. The mapping
    // data contains stream count, coupling information, and per channel mapping
    // values:
    //   - Byte 0: Number of streams.
    //   - Byte 1: Number coupled.
    //   - Byte 2: Starting at byte 2 are |header->channels| uint8 mapping
    //             values.
    const size_t kOpusHeaderNumStreamsOffset = kOpusHeaderSize;
    const size_t kOpusHeaderNumCoupledOffset = kOpusHeaderNumStreamsOffset + 1;
    const size_t kOpusHeaderStreamMapOffset = kOpusHeaderNumStreamsOffset + 2;
    int i;
    if (data_size < kOpusHeaderSize) {
        audio_codec_print("Header size is too small.\n");
        return -1;
    }
    header->channels = *(data + kOpusHeaderChannelsOffset);

    if (header->channels <= 0 || header->channels > MAX_CHANNELS) {
        audio_codec_print("Invalid Header, wrong channel count: %x.\n", header->channels);
        return -2;
    }
    header->skip_samples = ReadLE16(data, kOpusHeaderSize,
                                        kOpusHeaderSkipSamplesOffset);
    header->gain_db = ReadLE16(data, kOpusHeaderSize,
                                       kOpusHeaderGainOffset);
    header->channel_mapping = *(data + kOpusHeaderChannelMappingOffset);
    if (!header->channel_mapping) {
        if (header->channels > MAX_CHANNELS_DEFAULT_LAYOUT) {
            audio_codec_print("Invalid Header, missing stream map.\n");
            return -3;
        }
        header->num_streams = 1;
        header->num_coupled = header->channels > 1;
        header->stream_map[0] = 0;
        header->stream_map[1] = 1;
        return 0;
    }
    if (data_size < kOpusHeaderStreamMapOffset + header->channels) {
        audio_codec_print("Invalid stream map; insufficient data for current channel "
              "count: %d.\n", header->channels);
        return -4;
    }
    header->num_streams = *(data + kOpusHeaderNumStreamsOffset);
    header->num_coupled = *(data + kOpusHeaderNumCoupledOffset);
    if (header->num_streams + header->num_coupled != header->channels) {
        audio_codec_print("Inconsistent channel mapping.\n");
        return -5;
    }
    for (i = 0; i < header->channels; ++i)
        header->stream_map[i] = *(data + kOpusHeaderStreamMapOffset + i);
    return 0;
}


static int opus_dec_init(audio_decoder_operations_t *adec_ops)
{
    int status = OPUS_INVALID_STATE;
    int ret = -1;
#ifdef MULTI_DECODER
    OpusMSDecoder* decoder;
#else
    OpusDecoder* decoder;
#endif
    opus_priv_data_t *opus = (opus_priv_data_t*)adec_ops->priv_dec_data;
    aml_audio_dec_t * audec = (aml_audio_dec_t*)adec_ops->priv_data;

    //opus->mCodecDelay = ns_to_samples(0x632ea0, OPUS_RATE);
    //opus->mSeekPreRoll = ns_to_samples(0x04c4b400, OPUS_RATE);
    opus->mSamplesToDiscard = opus->mCodecDelay;
    ret = ParseOpusHeader(audec->extradata, audec->extradata_size, opus->header);
    if (ret < 0) {
        audio_codec_print("opus ParseOpusHeader error.\n");
        return -1;
    }
    opus->header->rate = OPUS_RATE;
#ifdef MULTI_DECODER
    audio_codec_print("opus_multistream_decoder_create.\n");
    decoder = opus_multistream_decoder_create(opus->header->rate,
                opus->header->channels, opus->header->num_streams,
                opus->header->num_coupled, opus->header->stream_map, &status);
#else
    audio_codec_print("opus_decoder_create.\n");
    decoder = opus_decoder_create(opus->header->rate, opus->header->channels, &status);
#endif
    if (!decoder || status != OPUS_OK) {
        audio_codec_print("opus_multistream_decoder_create failed status=%s",
                opus_strerror(status));
        return -1;
    }

#ifdef MULTI_DECODER
    status = opus_multistream_decoder_ctl(decoder, OPUS_SET_GAIN(opus->header->gain_db));
#else
    status = opus_decoder_ctl(decoder, OPUS_SET_GAIN(opus->header->gain_db));
#endif
    if (status != OPUS_OK) {
        audio_codec_print("Failed to set OPUS header gain; status=%s", opus_strerror(status));
        return -1;
    }
    adec_ops->pdecoder = decoder;
    return 0;
}

static int opus_init(audio_decoder_operations_t *adec_ops)
{
    opus_priv_data_t *pOpus = NULL;
    pOpus = malloc(sizeof(opus_priv_data_t));
    if (pOpus == NULL) {
        audio_codec_print("[%s %d]malloc opus_priv_data_t failed!\n", __FUNCTION__, __LINE__);
        return -1;
    }
    memset(pOpus, 0, sizeof(opus_priv_data_t));
    pOpus->header = malloc(sizeof(opus_header_t));
    if (!pOpus->header) {
        free(pOpus);
        audio_codec_print("[%s %d]malloc OpusHeader failed!\n", __FUNCTION__, __LINE__);
        return -1;
    }

    memset(pOpus->header, 0, sizeof(opus_header_t));
    adec_ops->priv_dec_data = pOpus;
    pOpus->adec_ops = adec_ops;

    return opus_dec_init(adec_ops);
}


int audio_dec_init(audio_decoder_operations_t *adec_ops)
{
    audio_codec_print("\n\n[%s]opus BuildDate--%s  BuildTime--%s", __FUNCTION__, __DATE__, __TIME__);
    adec_ops->nInBufSize = MAX_OPUS_OUTPUT_PACKET_SIZE_SAMPLES;
    adec_ops->nOutBufSize = MAX_OPUS_OUTPUT_PACKET_SIZE_SAMPLES*MAX_CHANNELS*sizeof(int16_t);
    return opus_init(adec_ops);
}

int audio_dec_decode(audio_decoder_operations_t *adec_ops, char *outbuf, int *outlen, char *inbuf, int inlen)
{
    int offset = 0;
    opus_priv_data_t *opus = (opus_priv_data_t*)adec_ops->priv_dec_data;
    //audio_codec_print("\n\nopus audio_dec_decode inlen=%d.\n", inlen);

#ifdef MULTI_DECODER
    OpusMSDecoder* decoder = adec_ops->pdecoder;
#else
    OpusDecoder* decoder = adec_ops->pdecoder;
#endif
    int framesize = 0;
    int nbytes = 0;
    do
    {
        if (inlen <= 2)
            break;
        nbytes = (inbuf[1]<<8)|inbuf[0];
        inbuf+=2;
        inlen-=2;
        if (inlen < nbytes) {
            break;
        }
        offset+=2;
#ifdef MULTI_DECODER
        framesize = opus_multistream_decode(decoder, inbuf, nbytes, outbuf, MAX_OPUS_OUTPUT_PACKET_SIZE_SAMPLES, 0);
#else
        framesize = opus_decode(decoder, inbuf, nbytes, outbuf, MAX_OPUS_OUTPUT_PACKET_SIZE_SAMPLES, 0);
#endif
        if (framesize < 0) {
            audio_codec_print("opus_decode error: %d. nbytes=%d, inlen=%d\n", framesize, nbytes, inlen);
        } else
            offset += nbytes;
    } while(framesize < 0);
    if ((opus->mSamplesToDiscard > 0) && (framesize > 0)) {
        if (opus->mSamplesToDiscard > framesize) {
            opus->mSamplesToDiscard -= framesize;
            framesize = 0;
        } else {
            opus->mSamplesToDiscard = 0;
            framesize -= opus->mSamplesToDiscard;
        }
    }
    *outlen  = framesize * sizeof(int16_t) * opus->header->channels;
    return offset;
}

int audio_dec_release(audio_decoder_operations_t *adec_ops)
{
    audio_codec_print("opus audio_dec_release\n");
    opus_priv_data_t *opus = (opus_priv_data_t*)adec_ops->priv_dec_data;
    if (opus) {
        if (opus->header) {
            free(opus->header);
        }
        free(opus);
    }
    if (adec_ops->pdecoder) {
#ifdef MULTI_DECODER
        opus_multistream_decoder_destroy((OpusMSDecoder*)adec_ops->pdecoder);
#else
        opus_decoder_destroy((OpusDecoder*)adec_ops->pdecoder);
#endif
        adec_ops->pdecoder = NULL;
    }
    return 0;
}

int audio_dec_getinfo(audio_decoder_operations_t *adec_ops, void *pAudioInfo)
{
    //audio_codec_print("audio_dec_getinfo\n");
    ((AudioInfo *)pAudioInfo)->channels = adec_ops->channels;;
    ((AudioInfo *)pAudioInfo)->samplerate = adec_ops->samplerate;
    return 0;
}


