/*
 * MoQ Support
 * Copyright (c) 2023 The FFmpeg Project
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavcodec/avcodec.h"
#include "libavcodec/codec_desc.h"
#include "libavcodec/h264.h"
#include "libavcodec/startcode.h"
#include "libavutil/base64.h"
#include "libavutil/bprint.h"
#include "libavutil/crc.h"
#include "libavutil/hmac.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/lfg.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/random_seed.h"
#include "libavutil/time.h"
#include "avc.h"
#include "nal.h"
#include "avio_internal.h"
#include "http.h"
#include "hang.h"
#include "internal.h"
#include "mux.h"
#include "network.h"
#include "srtp.h"
#include "tls.h"

/* Calculate the elapsed time from starttime to endtime in milliseconds. */
#define ELAPSED(starttime, endtime) ((int)(endtime - starttime) / 1000)

enum MOQState {
    MOQ_STATE_NONE,

    /* The initial state. */
    MOQ_STATE_INIT,
    /* The muxer is ready to send/receive media frames. */
    MOQ_STATE_STARTED,
    /* The muxer is failed. */
    MOQ_STATE_STOPPED,
    /* The muxer is failed. */
    MOQ_STATE_FAILED,
};

typedef struct MOQContext {
    AVClass *av_class;

    /* The state of the RTC connection. */
    enum MOQState state;

    /* Parameters for the input audio and video codecs. */
    AVCodecParameters *audio_par;
    AVCodecParameters *video_par;

    /* These variables represent timestamps used for calculating and tracking the cost. */
    int64_t moq_starttime;
    int64_t moq_init_time;

    int h264_annexb_insert_sps_pps;

    /**
     * The MoQ Path (maybe namespace)
     */
    char* path;
} MOQContext;

/**
 * Initialize and check the options for the WebRTC muxer.
 */
static av_cold int initialize(AVFormatContext *s)
{
    MOQContext *moq = s->priv_data;

    moq->moq_starttime = av_gettime();

    if (moq->state < MOQ_STATE_INIT)
        moq->state = MOQ_STATE_INIT;
    moq->moq_init_time = av_gettime();
    av_log(moq, AV_LOG_VERBOSE, "Init state=%d, elapsed=%dms\n",
        moq->state, ELAPSED(moq->moq_starttime, av_gettime()));

    return 0;
}

static int parse_codec(AVFormatContext *s)
{
    int i, ret = 0;
    MOQContext *moq = s->priv_data;

    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
        switch (par->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (moq->video_par) {
                av_log(moq, AV_LOG_ERROR, "Only one video stream is supported by MoQ\n");
                return AVERROR(EINVAL);
            }
            moq->video_par = par;

            if (par->codec_id != AV_CODEC_ID_H264) {
                av_log(moq, AV_LOG_ERROR, "Unsupported video codec %s by MoQ, choose h264\n",
                       desc ? desc->name : "unknown");
                return AVERROR_PATCHWELCOME;
            }

            if (par->video_delay > 0) {
                av_log(moq, AV_LOG_ERROR, "Unsupported B frames by MoQ\n");
                return AVERROR_PATCHWELCOME;
            }

            break;
        case AVMEDIA_TYPE_AUDIO:
            if (moq->audio_par) {
                av_log(moq, AV_LOG_ERROR, "Only one audio stream is supported by MoQ\n");
                return AVERROR(EINVAL);
            }
            moq->audio_par = par;

            if (par->codec_id != AV_CODEC_ID_AAC) {
                av_log(moq, AV_LOG_ERROR, "Unsupported audio codec %s by MoQ, choose aac\n",
                    desc ? desc->name : "unknown");
                return AVERROR_PATCHWELCOME;
            }

            if (par->ch_layout.nb_channels != 2) {
                av_log(moq, AV_LOG_ERROR, "Unsupported audio channels %d by MoQ, choose stereo\n",
                    par->ch_layout.nb_channels);
                return AVERROR_PATCHWELCOME;
            }

            if (par->sample_rate != 48000) {
                av_log(moq, AV_LOG_ERROR, "Unsupported audio sample rate %d by MoQ, choose 48000\n", par->sample_rate);
                return AVERROR_PATCHWELCOME;
            }
            break;
        default:
            av_log(moq, AV_LOG_ERROR, "Codec type '%s' for stream %d is not supported by MoQ\n",
                   av_get_media_type_string(par->codec_type), i);
            return AVERROR_PATCHWELCOME;
        }
    }

    return ret;
}

static int moq_start(AVFormatContext *s)
{
    int ret = 0;
    MOQContext *moq = s->priv_data;

    // TODO: dynamic profile
	hang_start_from_c(s->url, moq->path, "main");

    if (moq->state < MOQ_STATE_STARTED)
        moq->state = MOQ_STATE_STARTED;
    moq->moq_starttime = av_gettime();
    av_log(moq, AV_LOG_VERBOSE, "MoQ state=%d, elapsed=%dms\n",
        moq->state, ELAPSED(moq->moq_starttime, av_gettime()));

    return ret;
}

static av_cold int moq_init(AVFormatContext *s)
{
    int ret;
    MOQContext *moq = s->priv_data;

    if ((ret = initialize(s)) < 0)
        goto end;

    if ((ret = parse_codec(s)) < 0)
        goto end;

    if ((ret = moq_start(s)) < 0)
        goto end;

end:
    if (ret < 0 && moq->state < MOQ_STATE_FAILED)
        moq->state = MOQ_STATE_FAILED;
    return ret;
}

/**
 * Since the h264_mp4toannexb filter only processes the MP4 ISOM format and bypasses
 * the annexb format, it is necessary to manually insert encoder metadata before each
 * IDR when dealing with annexb format packets. For instance, in the case of H.264,
 * we must insert SPS and PPS before the IDR frame.
 */
static int h264_annexb_insert_sps_pps(AVFormatContext *s, AVPacket *pkt)
{
    int ret = 0;
    AVPacket *in = NULL;
    AVCodecParameters *par = s->streams[pkt->stream_index]->codecpar;
    uint32_t nal_size = 0, out_size = par ? par->extradata_size : 0;
    uint8_t unit_type, sps_seen = 0, pps_seen = 0, idr_seen = 0, *out;
    const uint8_t *buf, *buf_end, *r1;

    if (!par || !par->extradata || par->extradata_size <= 0)
        return ret;

    /* Discover NALU type from packet. */
    buf_end  = pkt->data + pkt->size;
    for (buf = ff_nal_find_startcode(pkt->data, buf_end); buf < buf_end; buf += nal_size) {
        while (!*(buf++));
        r1 = ff_nal_find_startcode(buf, buf_end);
        if ((nal_size = r1 - buf) > 0) {
            unit_type = *buf & 0x1f;
            if (unit_type == H264_NAL_SPS) {
                sps_seen = 1;
            } else if (unit_type == H264_NAL_PPS) {
                pps_seen = 1;
            } else if (unit_type == H264_NAL_IDR_SLICE) {
                idr_seen = 1;
            }

            out_size += 3 + nal_size;
        }
    }

    if (!idr_seen || (sps_seen && pps_seen))
        return ret;

    /* See av_bsf_send_packet */
    in = av_packet_alloc();
    if (!in)
        return AVERROR(ENOMEM);

    ret = av_packet_make_refcounted(pkt);
    if (ret < 0)
        goto fail;

    av_packet_move_ref(in, pkt);

    /* Create a new packet with sps/pps inserted. */
    ret = av_new_packet(pkt, out_size);
    if (ret < 0)
        goto fail;

    ret = av_packet_copy_props(pkt, in);
    if (ret < 0)
        goto fail;

    memcpy(pkt->data, par->extradata, par->extradata_size);
    out = pkt->data + par->extradata_size;
    buf_end  = in->data + in->size;
    for (buf = ff_nal_find_startcode(in->data, buf_end); buf < buf_end; buf += nal_size) {
        while (!*(buf++));
        r1 = ff_nal_find_startcode(buf, buf_end);
        if ((nal_size = r1 - buf) > 0) {
            AV_WB24(out, 0x00001);
            memcpy(out + 3, buf, nal_size);
            out += 3 + nal_size;
        }
    }

fail:
    if (ret < 0)
        av_packet_unref(pkt);
    av_packet_free(&in);

    return ret;
}

static int moq_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret = 0;
    MOQContext *moq = s->priv_data;
    AVStream *st = s->streams[pkt->stream_index];

    int64_t dts_microseconds = av_rescale_q(pkt->dts, st->time_base, (AVRational){1, 1000000});

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (pkt->flags & AV_PKT_FLAG_KEY) {
            if ((ret = h264_annexb_insert_sps_pps(s, pkt)) < 0) {
                av_log(moq, AV_LOG_ERROR, "Failed to insert SPS/PPS before IDR\n");
                goto end;
            }
        }

        // av_log(moq, AV_LOG_ERROR, "joy %d %lld %d\n", pkt->flags & AV_PKT_FLAG_KEY, dts_microseconds, pkt->size);
        
		hang_write_video_packet_from_c(pkt->data, pkt->size, pkt->flags & AV_PKT_FLAG_KEY, dts_microseconds);
	} else {
		hang_write_audio_packet_from_c(pkt->data, pkt->size, dts_microseconds);
	}

end:
    if (ret < 0 && moq->state < MOQ_STATE_FAILED)
        moq->state = MOQ_STATE_FAILED;
    return ret;
}

static av_cold void moq_deinit(AVFormatContext *s)
{
    int i;

    for (i = 0; i < s->nb_streams; i++) {
        AVFormatContext* moq_ctx = s->streams[i]->priv_data;
        if (!moq_ctx)
            continue;

        av_write_trailer(moq_ctx);
        avformat_free_context(moq_ctx);
        s->streams[i]->priv_data = NULL;
    }

    hang_stop_from_c();
}

static int moq_check_bitstream(AVFormatContext *s, AVStream *st, const AVPacket *pkt)
{
    int ret = 1, extradata_isom = 0;
    uint8_t *b = pkt->data;
    MOQContext *moq = s->priv_data;

    if (st->codecpar->codec_id == AV_CODEC_ID_H264) {
        extradata_isom = st->codecpar->extradata_size > 0 && st->codecpar->extradata[0] == 1;
        if (pkt->size >= 5 && AV_RB32(b) != 0x0000001 && (AV_RB24(b) != 0x000001 || extradata_isom)) {
            ret = ff_stream_add_bitstream_filter(st, "h264_mp4toannexb", NULL);
            av_log(moq, AV_LOG_VERBOSE, "Enable BSF h264_mp4toannexb, packet=[%x %x %x %x %x ...], extradata_isom=%d\n",
                b[0], b[1], b[2], b[3], b[4], extradata_isom);
        } else
            moq->h264_annexb_insert_sps_pps = 1;
    }

    return ret;
}

#define OFFSET(x) offsetof(MOQContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "path", "The MoQ path (maybe namespace)", OFFSET(path), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { NULL },
};

static const AVClass moq_muxer_class = {
    .class_name = "MoQ muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_moq_muxer = {
    .p.name             = "moq_hang",
    .p.long_name        = NULL_IF_CONFIG_SMALL("MoQ ingestion protocol muxer"),
    .p.audio_codec      = AV_CODEC_ID_AAC,
    .p.video_codec      = AV_CODEC_ID_H264,
    .p.flags            = AVFMT_GLOBALHEADER | AVFMT_NOFILE | AVFMT_EXPERIMENTAL,
    .p.priv_class       = &moq_muxer_class,
    .priv_data_size     = sizeof(MOQContext),
    .check_bitstream    = moq_check_bitstream,
    .init               = moq_init,
    .write_packet       = moq_write_packet,
    .deinit             = moq_deinit,
};
