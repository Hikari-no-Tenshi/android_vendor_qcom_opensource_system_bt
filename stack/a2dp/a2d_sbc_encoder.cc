/******************************************************************************
 *
 *  Copyright (C) 2016 The Android Open Source Project
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "a2d_sbc_encoder"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "bt_target.h"
#include "bt_common.h"
#include "a2d_api.h"
#include "a2d_sbc.h"
#include "a2d_sbc_encoder.h"
#include "a2d_sbc_up_sample.h"
#include "avdt_api.h"
#include "embdrv/sbc/encoder/include/sbc_encoder.h"
#include "osi/include/log.h"
#include "osi/include/time.h"

#define STATS_UPDATE_MAX(current_value_storage, new_value) \
do { \
    if ((new_value) > (current_value_storage)) \
        (current_value_storage) = (new_value); \
} while (0)

/* Buffer pool */
#define A2D_SBC_BUFFER_SIZE  BT_DEFAULT_BUFFER_SIZE

// A2DP SBC encoder interval in milliseconds.
#define A2D_SBC_ENCODER_INTERVAL_MS     20

/* High quality quality setting @ 44.1 khz */
#define A2D_SBC_DEFAULT_BITRATE         328

#define A2D_SBC_NON_EDR_MAX_RATE        229

/*
 * 2DH5 payload size of:
 * 679 bytes - (4 bytes L2CAP Header + 12 bytes AVDTP Header)
 */
#define MAX_2MBPS_AVDTP_MTU             663
#define A2D_SBC_MAX_PCM_ITER_NUM_PER_TICK 3

#define A2D_SBC_MAX_HQ_FRAME_SIZE_44_1  119
#define A2D_SBC_MAX_HQ_FRAME_SIZE_48    115

/* Define the bitrate step when trying to match bitpool value */
#define A2D_SBC_BITRATE_STEP 5

/* Readability constants */
#define A2D_SBC_FRAME_HEADER_SIZE_BYTES 4 // A2DP Spec v1.3, 12.4, Table 12.12
#define A2D_SBC_SCALE_FACTOR_BITS       4 // A2DP Spec v1.3, 12.4, Table 12.13

/* offset */
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
/* A2DP header will contain a CP header of size 1 */
#define A2DP_HDR_SIZE               2
#define A2D_SBC_OFFSET (AVDT_MEDIA_OFFSET + A2D_SBC_MPL_HDR_LEN + 1)
#else
#define A2DP_HDR_SIZE               1
#define A2D_SBC_OFFSET (AVDT_MEDIA_OFFSET + A2D_SBC_MPL_HDR_LEN)
#endif

typedef struct {
    uint32_t aa_frame_counter;
    int32_t  aa_feed_counter;
    int32_t  aa_feed_residue;
    uint32_t counter;
    uint32_t bytes_per_tick;  /* pcm bytes read each media task tick */
    uint64_t last_frame_us;
} tA2D_FEEDING_STATE;

typedef struct {
    uint64_t session_start_us;

    size_t media_read_total_expected_frames;
    size_t media_read_max_expected_frames;
    size_t media_read_expected_count;

    size_t media_read_total_limited_frames;
    size_t media_read_max_limited_frames;
    size_t media_read_limited_count;
} a2d_sbc_encoder_stats_t;

typedef struct {
    a2d_source_read_callback_t read_callback;
    a2d_source_enqueue_callback_t enqueue_callback;
    uint16_t TxAaMtuSize;
    uint8_t tx_sbc_frames;
    bool is_peer_edr;           /* True if the peer device supports EDR */
    bool peer_supports_3mbps;   /* True if the peer device supports 3Mbps EDR */
    uint32_t timestamp;         /* Timestamp for the A2DP frames */
    SBC_ENC_PARAMS sbc_encoder_params;
    tA2D_FEEDING_PARAMS feeding_params;
    tA2D_FEEDING_STATE feeding_state;

    a2d_sbc_encoder_stats_t stats;
} tA2D_SBC_ENCODER_CB;

static tA2D_SBC_ENCODER_CB a2d_sbc_encoder_cb;

static bool a2d_sbc_read_feeding(void);
static void a2d_sbc_encode_frames(uint8_t nb_frame);
static void a2d_sbc_get_num_frame_iteration(uint8_t *num_of_iterations,
                                            uint8_t *num_of_frames,
                                            uint64_t timestamp_us);
static uint8_t calculate_max_frames_per_packet(void);
static uint16_t a2d_sbc_source_rate(void);
static uint32_t a2d_sbc_frame_length(void);

void a2d_sbc_encoder_init(bool is_peer_edr, bool peer_supports_3mbps,
                          const tA2D_ENCODER_INIT_PARAMS *p_init_params,
                          a2d_source_read_callback_t read_callback,
                          a2d_source_enqueue_callback_t enqueue_callback)
{
    SBC_ENC_PARAMS *p_encoder_params = &a2d_sbc_encoder_cb.sbc_encoder_params;

    memset(&a2d_sbc_encoder_cb, 0, sizeof(a2d_sbc_encoder_cb));

    a2d_sbc_encoder_cb.stats.session_start_us = time_get_os_boottime_us();

    a2d_sbc_encoder_cb.read_callback = read_callback;
    a2d_sbc_encoder_cb.enqueue_callback = enqueue_callback;
    a2d_sbc_encoder_cb.is_peer_edr = is_peer_edr;
    a2d_sbc_encoder_cb.peer_supports_3mbps = peer_supports_3mbps;
    a2d_sbc_encoder_cb.timestamp = 0;

    /* SBC encoder config (enforced even if not used) */
    p_encoder_params->s16ChannelMode = p_init_params->ChannelMode;
    p_encoder_params->s16NumOfSubBands = p_init_params->NumOfSubBands;
    p_encoder_params->s16NumOfBlocks = p_init_params->NumOfBlocks;
    p_encoder_params->s16AllocationMethod = p_init_params->AllocationMethod;
    p_encoder_params->s16SamplingFreq = p_init_params->SamplingFreq;

    p_encoder_params->u16BitRate = a2d_sbc_source_rate();

    uint16_t mtu_size = A2D_SBC_BUFFER_SIZE - A2D_SBC_OFFSET - sizeof(BT_HDR);
    if (mtu_size < p_init_params->MtuSize) {
        a2d_sbc_encoder_cb.TxAaMtuSize = mtu_size;
    } else {
        a2d_sbc_encoder_cb.TxAaMtuSize = p_init_params->MtuSize;
    }

    LOG_DEBUG(LOG_TAG, "%s: mtu %d, peer mtu %d", __func__,
                a2d_sbc_encoder_cb.TxAaMtuSize, p_init_params->MtuSize);
    LOG_DEBUG(LOG_TAG, "%s: ch mode %d, subnd %d, nb blk %d, alloc %d, rate %d, freq %d",
              __func__,
              p_encoder_params->s16ChannelMode,
              p_encoder_params->s16NumOfSubBands,
              p_encoder_params->s16NumOfBlocks,
              p_encoder_params->s16AllocationMethod,
              p_encoder_params->u16BitRate,
              p_encoder_params->s16SamplingFreq);

    /* Reset entirely the SBC encoder */
    SBC_Encoder_Init(&a2d_sbc_encoder_cb.sbc_encoder_params);
    a2d_sbc_encoder_cb.tx_sbc_frames = calculate_max_frames_per_packet();

    LOG_DEBUG(LOG_TAG, "%s: bit pool %d", __func__,
              p_encoder_params->s16BitPool);
}

void a2d_sbc_encoder_update(const tA2D_ENCODER_UPDATE_PARAMS *p_update_params)
{
    SBC_ENC_PARAMS *p_encoder_params = &a2d_sbc_encoder_cb.sbc_encoder_params;
    uint16_t s16SamplingFreq;
    int16_t s16BitPool = 0;
    int16_t s16BitRate;
    int16_t s16FrameLen;
    uint8_t protect = 0;

    LOG_DEBUG(LOG_TAG, "%s: minmtu %d, maxbp %d minbp %d", __func__,
              p_update_params->MinMtuSize, p_update_params->MaxBitPool,
              p_update_params->MinBitPool);

    if (!p_encoder_params->s16NumOfSubBands) {
        LOG_WARN(LOG_TAG, "%s: SubBands are set to 0, resetting to max (%d)",
                 __func__, SBC_MAX_NUM_OF_SUBBANDS);
        p_encoder_params->s16NumOfSubBands = SBC_MAX_NUM_OF_SUBBANDS;
    }

    if (!p_encoder_params->s16NumOfBlocks) {
        LOG_WARN(LOG_TAG, "%s: Blocks are set to 0, resetting to max (%d)",
                 __func__, SBC_MAX_NUM_OF_BLOCKS);
        p_encoder_params->s16NumOfBlocks = SBC_MAX_NUM_OF_BLOCKS;
    }

    if (!p_encoder_params->s16NumOfChannels) {
        LOG_WARN(LOG_TAG, "%s: Channels are set to 0, resetting to max (%d)",
                 __func__, SBC_MAX_NUM_OF_CHANNELS);
        p_encoder_params->s16NumOfChannels = SBC_MAX_NUM_OF_CHANNELS;
    }

    uint16_t mtu_size = A2D_SBC_BUFFER_SIZE - A2D_SBC_OFFSET - sizeof(BT_HDR);
    if (mtu_size < p_update_params->MinMtuSize) {
        a2d_sbc_encoder_cb.TxAaMtuSize = mtu_size;
    } else {
        a2d_sbc_encoder_cb.TxAaMtuSize = p_update_params->MinMtuSize;
    }

    /* Set the initial target bit rate */
    p_encoder_params->u16BitRate = a2d_sbc_source_rate();

    if (p_encoder_params->s16SamplingFreq == SBC_sf16000)
        s16SamplingFreq = 16000;
    else if (p_encoder_params->s16SamplingFreq == SBC_sf32000)
        s16SamplingFreq = 32000;
    else if (p_encoder_params->s16SamplingFreq == SBC_sf44100)
        s16SamplingFreq = 44100;
    else
        s16SamplingFreq = 48000;

    do {
        if (p_encoder_params->s16NumOfBlocks == 0 ||
            p_encoder_params->s16NumOfSubBands == 0 ||
            p_encoder_params->s16NumOfChannels == 0) {
            LOG_ERROR(LOG_TAG, "%s: avoiding division by zero...", __func__);
            LOG_ERROR(LOG_TAG, "%s: block=%d, subBands=%d, channels=%d",
                      __func__,
                      p_encoder_params->s16NumOfBlocks,
                      p_encoder_params->s16NumOfSubBands,
                      p_encoder_params->s16NumOfChannels);
            break;
        }

        if ((p_encoder_params->s16ChannelMode == SBC_JOINT_STEREO) ||
            (p_encoder_params->s16ChannelMode == SBC_STEREO)) {
            s16BitPool = (int16_t)((p_encoder_params->u16BitRate *
                    p_encoder_params->s16NumOfSubBands * 1000 / s16SamplingFreq)
                    - ((32 + (4 * p_encoder_params->s16NumOfSubBands *
                    p_encoder_params->s16NumOfChannels)
                    + ((p_encoder_params->s16ChannelMode - 2) *
                    p_encoder_params->s16NumOfSubBands))
                    / p_encoder_params->s16NumOfBlocks));

            s16FrameLen = 4 + (4*p_encoder_params->s16NumOfSubBands *
                    p_encoder_params->s16NumOfChannels) / 8
                    + (((p_encoder_params->s16ChannelMode - 2) *
                    p_encoder_params->s16NumOfSubBands)
                    + (p_encoder_params->s16NumOfBlocks * s16BitPool)) / 8;

            s16BitRate = (8 * s16FrameLen * s16SamplingFreq)
                    / (p_encoder_params->s16NumOfSubBands *
                    p_encoder_params->s16NumOfBlocks * 1000);

            if (s16BitRate > p_encoder_params->u16BitRate)
                s16BitPool--;

            if (p_encoder_params->s16NumOfSubBands == 8)
                s16BitPool = (s16BitPool > 255) ? 255 : s16BitPool;
            else
                s16BitPool = (s16BitPool > 128) ? 128 : s16BitPool;
        } else {
            s16BitPool = (int16_t)(((p_encoder_params->s16NumOfSubBands *
                    p_encoder_params->u16BitRate * 1000)
                    / (s16SamplingFreq * p_encoder_params->s16NumOfChannels))
                    - (((32 / p_encoder_params->s16NumOfChannels) +
                    (4 * p_encoder_params->s16NumOfSubBands))
                    / p_encoder_params->s16NumOfBlocks));

            p_encoder_params->s16BitPool =
                (s16BitPool > (16 * p_encoder_params->s16NumOfSubBands)) ?
                        (16 * p_encoder_params->s16NumOfSubBands) : s16BitPool;
        }

        if (s16BitPool < 0)
            s16BitPool = 0;

        LOG_DEBUG(LOG_TAG, "%s: bitpool candidate: %d (%d kbps)", __func__,
                  s16BitPool, p_encoder_params->u16BitRate);

        if (s16BitPool > p_update_params->MaxBitPool) {
            LOG_DEBUG(LOG_TAG, "%s: computed bitpool too large (%d)",
                      __func__, s16BitPool);
            /* Decrease bitrate */
            p_encoder_params->u16BitRate -= A2D_SBC_BITRATE_STEP;
            /* Record that we have decreased the bitrate */
            protect |= 1;
        } else if (s16BitPool < p_update_params->MinBitPool) {
            LOG_WARN(LOG_TAG, "%s: computed bitpool too small (%d)", __func__,
                     s16BitPool);

            /* Increase bitrate */
            uint16_t previous_u16BitRate = p_encoder_params->u16BitRate;
            p_encoder_params->u16BitRate += A2D_SBC_BITRATE_STEP;
            /* Record that we have increased the bitrate */
            protect |= 2;
            /* Check over-flow */
            if (p_encoder_params->u16BitRate < previous_u16BitRate)
                protect |= 3;
        } else {
            break;
        }
        /* In case we have already increased and decreased the bitrate, just stop */
        if (protect == 3) {
            LOG_ERROR(LOG_TAG, "%s: could not find bitpool in range",
                      __func__);
            break;
        }
    } while (true);

    /* Finally update the bitpool in the encoder structure */
    p_encoder_params->s16BitPool = s16BitPool;

    LOG_DEBUG(LOG_TAG, "%s: final bit rate %d, final bit pool %d", __func__,
              p_encoder_params->u16BitRate, p_encoder_params->s16BitPool);

    /* make sure we reinitialize encoder with new settings */
    SBC_Encoder_Init(&a2d_sbc_encoder_cb.sbc_encoder_params);

    a2d_sbc_encoder_cb.tx_sbc_frames = calculate_max_frames_per_packet();
}

void a2d_sbc_encoder_cleanup(void)
{
    memset(&a2d_sbc_encoder_cb, 0, sizeof(a2d_sbc_encoder_cb));
}

void a2d_sbc_feeding_init(const tA2D_FEEDING_PARAMS *p_feeding_params)
{
    SBC_ENC_PARAMS *p_encoder_params = &a2d_sbc_encoder_cb.sbc_encoder_params;
    bool reconfig_needed = false;

    LOG_DEBUG(LOG_TAG, "%s: PCM feeding: sampling_freq:%d num_channel:%d bit_per_sample:%d",
              __func__,
              p_feeding_params->sampling_freq,
              p_feeding_params->num_channel,
              p_feeding_params->bit_per_sample);

    /* Save the feeding information */
    memcpy(&a2d_sbc_encoder_cb.feeding_params, p_feeding_params,
           sizeof(tA2D_FEEDING_PARAMS));

    /* Check the PCM feeding sampling_freq */
    switch (p_feeding_params->sampling_freq) {
        case  8000:
        case 12000:
        case 16000:
        case 24000:
        case 32000:
        case 48000:
            /* For these sampling_freq the AV connection must be 48000 */
            if (p_encoder_params->s16SamplingFreq != SBC_sf48000) {
                /* Reconfiguration needed at 48000 */
                LOG_DEBUG(LOG_TAG, "%s: SBC Reconfiguration needed at 48000",
                          __func__);
                p_encoder_params->s16SamplingFreq = SBC_sf48000;
                reconfig_needed = true;
            }
            break;

        case 11025:
        case 22050:
        case 44100:
            /* For these sampling_freq the AV connection must be 44100 */
            if (p_encoder_params->s16SamplingFreq != SBC_sf44100) {
                /* Reconfiguration needed at 44100 */
                LOG_DEBUG(LOG_TAG, "%s: SBC Reconfiguration needed at 44100",
                          __func__);
                p_encoder_params->s16SamplingFreq = SBC_sf44100;
                reconfig_needed = true;
            }
            break;
        default:
            LOG_DEBUG(LOG_TAG, "%s: Feeding PCM sampling_freq unsupported",
                      __func__);
            break;
    }

    /* Some AV Headsets do not support Mono => always ask for Stereo */
    if (p_encoder_params->s16ChannelMode == SBC_MONO) {
        LOG_DEBUG(LOG_TAG, "%s: SBC Reconfiguration needed in Stereo",
                  __func__);
        p_encoder_params->s16ChannelMode = SBC_JOINT_STEREO;
        reconfig_needed = true;
    }

    if (reconfig_needed) {
        LOG_DEBUG(LOG_TAG, "%s: mtu %d ch mode %d, nbsubd %d, nb %d, alloc %d, rate %d, freq %d",
                  __func__,
                  a2d_sbc_encoder_cb.TxAaMtuSize,
                  p_encoder_params->s16ChannelMode,
                  p_encoder_params->s16NumOfSubBands,
                  p_encoder_params->s16NumOfBlocks,
                  p_encoder_params->s16AllocationMethod,
                  p_encoder_params->u16BitRate,
                  p_encoder_params->s16SamplingFreq);
        SBC_Encoder_Init(p_encoder_params);
    } else {
        LOG_DEBUG(LOG_TAG, "%s: no SBC reconfig needed", __func__);
    }
}

void a2d_sbc_feeding_reset(void)
{
    /* By default, just clear the entire state */
    memset(&a2d_sbc_encoder_cb.feeding_state, 0,
           sizeof(a2d_sbc_encoder_cb.feeding_state));

    a2d_sbc_encoder_cb.feeding_state.bytes_per_tick =
        (a2d_sbc_encoder_cb.feeding_params.sampling_freq *
         a2d_sbc_encoder_cb.feeding_params.bit_per_sample / 8 *
         a2d_sbc_encoder_cb.feeding_params.num_channel *
         A2D_SBC_ENCODER_INTERVAL_MS) / 1000;

    LOG_DEBUG(LOG_TAG, "%s: PCM bytes per tick %u",
              __func__, a2d_sbc_encoder_cb.feeding_state.bytes_per_tick);
}

void a2d_sbc_feeding_flush(void)
{
    a2d_sbc_encoder_cb.feeding_state.counter = 0;
    a2d_sbc_encoder_cb.feeding_state.aa_feed_residue = 0;
}

period_ms_t a2d_sbc_get_encoder_interval_ms(void)
{
    return A2D_SBC_ENCODER_INTERVAL_MS;
}

void a2d_sbc_send_frames(uint64_t timestamp_us)
{
    uint8_t nb_frame = 0;
    uint8_t nb_iterations = 0;

    a2d_sbc_get_num_frame_iteration(&nb_iterations, &nb_frame, timestamp_us);
    LOG_VERBOSE(LOG_TAG, "%s: Sending %d frames per iteration, %d iterations",
                __func__, nb_frame, nb_iterations);
    if (nb_frame == 0)
        return;

    for (uint8_t counter = 0; counter < nb_iterations; counter++) {
        // Transcode frame and enqueue
        a2d_sbc_encode_frames(nb_frame);
    }
}

// Obtains the number of frames to send and number of iterations
// to be used. |num_of_iterations| and |num_of_frames| parameters
// are used as output param for returning the respective values.
static void a2d_sbc_get_num_frame_iteration(uint8_t *num_of_iterations,
                                            uint8_t *num_of_frames,
                                            uint64_t timestamp_us)
{
    uint8_t nof = 0;
    uint8_t noi = 1;

    uint32_t projected_nof = 0;
    uint32_t pcm_bytes_per_frame =
        a2d_sbc_encoder_cb.sbc_encoder_params.s16NumOfSubBands *
        a2d_sbc_encoder_cb.sbc_encoder_params.s16NumOfBlocks *
        a2d_sbc_encoder_cb.feeding_params.num_channel *
        a2d_sbc_encoder_cb.feeding_params.bit_per_sample / 8;
    LOG_VERBOSE(LOG_TAG, "%s: pcm_bytes_per_frame %u", __func__,
                pcm_bytes_per_frame);

    uint32_t us_this_tick = A2D_SBC_ENCODER_INTERVAL_MS * 1000;
    uint64_t now_us = timestamp_us;
    if (a2d_sbc_encoder_cb.feeding_state.last_frame_us != 0)
        us_this_tick = (now_us - a2d_sbc_encoder_cb.feeding_state.last_frame_us);
    a2d_sbc_encoder_cb.feeding_state.last_frame_us = now_us;

    a2d_sbc_encoder_cb.feeding_state.counter +=
        a2d_sbc_encoder_cb.feeding_state.bytes_per_tick *
        us_this_tick / (A2D_SBC_ENCODER_INTERVAL_MS * 1000);

    /* Calculate the number of frames pending for this media tick */
    projected_nof =
        a2d_sbc_encoder_cb.feeding_state.counter / pcm_bytes_per_frame;
    // Update the stats
    STATS_UPDATE_MAX(a2d_sbc_encoder_cb.stats.media_read_max_expected_frames,
                     projected_nof);
    a2d_sbc_encoder_cb.stats.media_read_total_expected_frames += projected_nof;
    a2d_sbc_encoder_cb.stats.media_read_expected_count++;

    if (projected_nof > MAX_PCM_FRAME_NUM_PER_TICK) {
        LOG_WARN(LOG_TAG, "%s: limiting frames to be sent from %d to %d",
                 __func__, projected_nof, MAX_PCM_FRAME_NUM_PER_TICK);

        // Update the stats
        size_t delta = projected_nof - MAX_PCM_FRAME_NUM_PER_TICK;
        a2d_sbc_encoder_cb.stats.media_read_limited_count++;
        a2d_sbc_encoder_cb.stats.media_read_total_limited_frames += delta;
        STATS_UPDATE_MAX(a2d_sbc_encoder_cb.stats.media_read_max_limited_frames,
                         delta);

        projected_nof = MAX_PCM_FRAME_NUM_PER_TICK;
    }

    LOG_VERBOSE(LOG_TAG, "%s: frames for available PCM data %u",
                __func__, projected_nof);

    if (a2d_sbc_encoder_cb.is_peer_edr) {
        if (!a2d_sbc_encoder_cb.tx_sbc_frames) {
            LOG_ERROR(LOG_TAG, "%s: tx_sbc_frames not updated, update from here",
                      __func__);
            a2d_sbc_encoder_cb.tx_sbc_frames = calculate_max_frames_per_packet();
        }

        nof = a2d_sbc_encoder_cb.tx_sbc_frames;
        if (!nof) {
            LOG_ERROR(LOG_TAG, "%s: number of frames not updated, set calculated values",
                      __func__);
            nof = projected_nof;
            noi = 1;
        } else {
            if (nof < projected_nof) {
                noi = projected_nof / nof; // number of iterations would vary
                if (noi > A2D_SBC_MAX_PCM_ITER_NUM_PER_TICK) {
                    LOG_ERROR(LOG_TAG, "%s: Audio Congestion (iterations:%d > max (%d))",
                              __func__, noi, A2D_SBC_MAX_PCM_ITER_NUM_PER_TICK);
                    noi = A2D_SBC_MAX_PCM_ITER_NUM_PER_TICK;
                    a2d_sbc_encoder_cb.feeding_state.counter
                        = noi * nof * pcm_bytes_per_frame;
                }
                projected_nof = nof;
            } else {
                noi = 1;        // number of iterations is 1
                LOG_VERBOSE(LOG_TAG, "%s: reducing frames for available PCM data",
                            __func__);
                nof = projected_nof;
            }
        }
    } else {
        // For BR cases nof will be same as the value retrieved at projected_nof
        LOG_VERBOSE(LOG_TAG, "%s: headset BR, number of frames %u",
                    __func__, nof);
        if (projected_nof > MAX_PCM_FRAME_NUM_PER_TICK) {
            LOG_ERROR(LOG_TAG, "%s: Audio Congestion (frames: %d > max (%d))",
                      __func__, projected_nof, MAX_PCM_FRAME_NUM_PER_TICK);
            projected_nof = MAX_PCM_FRAME_NUM_PER_TICK;
            a2d_sbc_encoder_cb.feeding_state.counter =
                noi * projected_nof * pcm_bytes_per_frame;
        }
        nof = projected_nof;
    }
    a2d_sbc_encoder_cb.feeding_state.counter -= noi * nof * pcm_bytes_per_frame;
    LOG_VERBOSE(LOG_TAG, "%s: effective num of frames %u, iterations %u",
                __func__, nof, noi);

    *num_of_frames = nof;
    *num_of_iterations = noi;
}

static void a2d_sbc_encode_frames(uint8_t nb_frame)
{
    SBC_ENC_PARAMS *p_encoder_params = &a2d_sbc_encoder_cb.sbc_encoder_params;
    uint8_t remain_nb_frame = nb_frame;
    uint16_t blocm_x_subband = p_encoder_params->s16NumOfSubBands *
        p_encoder_params->s16NumOfBlocks;

    while (nb_frame) {
        BT_HDR *p_buf = (BT_HDR *)osi_malloc(A2D_SBC_BUFFER_SIZE);

        /* Init buffer */
        p_buf->offset = A2D_SBC_OFFSET;
        p_buf->len = 0;
        p_buf->layer_specific = 0;

        do {
            /* Write @ of allocated buffer in sbc_encoder_params.pu8Packet */
            p_encoder_params->pu8Packet =
                (uint8_t *) (p_buf + 1) + p_buf->offset + p_buf->len;
            /* Fill allocated buffer with 0 */
            memset(p_encoder_params->as16PcmBuffer, 0,
                   blocm_x_subband * p_encoder_params->s16NumOfChannels);

            /* Read PCM data and upsample them if needed */
            if (a2d_sbc_read_feeding()) {
                SBC_Encoder(p_encoder_params);

                /* Update SBC frame length */
                p_buf->len += p_encoder_params->u16PacketLength;
                nb_frame--;
                p_buf->layer_specific++;
            } else {
                LOG_WARN(LOG_TAG, "%s: underflow %d, %d",
                         __func__, nb_frame,
                         a2d_sbc_encoder_cb.feeding_state.aa_feed_residue);
                a2d_sbc_encoder_cb.feeding_state.counter +=
                    nb_frame *
                    p_encoder_params->s16NumOfSubBands *
                    p_encoder_params->s16NumOfBlocks *
                    a2d_sbc_encoder_cb.feeding_params.num_channel *
                    a2d_sbc_encoder_cb.feeding_params.bit_per_sample / 8;
                /* no more pcm to read */
                nb_frame = 0;
            }
        } while (((p_buf->len + p_encoder_params->u16PacketLength) <
                  a2d_sbc_encoder_cb.TxAaMtuSize)
                 && (p_buf->layer_specific < 0x0F) && nb_frame);

        if (p_buf->len) {
            /*
             * Timestamp of the media packet header represent the TS of the
             * first SBC frame, i.e the timestamp before including this frame.
             */
            *((uint32_t *) (p_buf + 1)) = a2d_sbc_encoder_cb.timestamp;

            a2d_sbc_encoder_cb.timestamp +=
                p_buf->layer_specific * blocm_x_subband;

            uint8_t done_nb_frame = remain_nb_frame - nb_frame;
            remain_nb_frame = nb_frame;
            if (!a2d_sbc_encoder_cb.enqueue_callback(p_buf, done_nb_frame))
                return;
        } else {
            osi_free(p_buf);
        }
    }
}

static bool a2d_sbc_read_feeding(void)
{
    SBC_ENC_PARAMS *p_encoder_params = &a2d_sbc_encoder_cb.sbc_encoder_params;
    uint16_t blocm_x_subband = p_encoder_params->s16NumOfSubBands *
        p_encoder_params->s16NumOfBlocks;
    uint32_t read_size;
    uint32_t sbc_sampling = 48000;
    uint32_t src_samples;
    uint16_t bytes_needed = blocm_x_subband *
        p_encoder_params->s16NumOfChannels *
        a2d_sbc_encoder_cb.feeding_params.bit_per_sample / 8;
    static uint16_t up_sampled_buffer[SBC_MAX_NUM_FRAME * SBC_MAX_NUM_OF_BLOCKS
            * SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS * 2];
    static uint16_t read_buffer[SBC_MAX_NUM_FRAME * SBC_MAX_NUM_OF_BLOCKS
            * SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS];
    uint32_t src_size_used;
    uint32_t dst_size_used;
    bool fract_needed;
    int32_t fract_max;
    int32_t fract_threshold;
    uint32_t nb_byte_read;

    /* Get the SBC sampling rate */
    switch (p_encoder_params->s16SamplingFreq) {
    case SBC_sf48000:
        sbc_sampling = 48000;
        break;
    case SBC_sf44100:
        sbc_sampling = 44100;
        break;
    case SBC_sf32000:
        sbc_sampling = 32000;
        break;
    case SBC_sf16000:
        sbc_sampling = 16000;
        break;
    }

    if (sbc_sampling == a2d_sbc_encoder_cb.feeding_params.sampling_freq) {
        read_size =
            bytes_needed - a2d_sbc_encoder_cb.feeding_state.aa_feed_residue;
        nb_byte_read = a2d_sbc_encoder_cb.read_callback(
            ((uint8_t *)p_encoder_params->as16PcmBuffer) +
            a2d_sbc_encoder_cb.feeding_state.aa_feed_residue,
            read_size);
        if (nb_byte_read != read_size) {
            a2d_sbc_encoder_cb.feeding_state.aa_feed_residue += nb_byte_read;
            return false;
        }
        a2d_sbc_encoder_cb.feeding_state.aa_feed_residue = 0;
        return true;
    }

    /*
     * Some Feeding PCM frequencies require to split the number of sample
     * to read.
     * E.g 128 / 6 = 21.3333 => read 22 and 21 and 21 => max = 2; threshold = 0
     */
    fract_needed = false;   /* Default */
    switch (a2d_sbc_encoder_cb.feeding_params.sampling_freq) {
    case 32000:
    case 8000:
        fract_needed = true;
        fract_max = 2;          /* 0, 1 and 2 */
        fract_threshold = 0;    /* Add one for the first */
        break;
    case 16000:
        fract_needed = true;
        fract_max = 2;          /* 0, 1 and 2 */
        fract_threshold = 1;    /* Add one for the first two frames*/
        break;
    }

    /* Compute number of sample to read from source */
    src_samples = blocm_x_subband;
    src_samples *= a2d_sbc_encoder_cb.feeding_params.sampling_freq;
    src_samples /= sbc_sampling;

    /* The previous division may have a remainder not null */
    if (fract_needed) {
        if (a2d_sbc_encoder_cb.feeding_state.aa_feed_counter <= fract_threshold) {
            src_samples++; /* for every read before threshold add one sample */
        }

        /* do nothing if counter >= threshold */
        a2d_sbc_encoder_cb.feeding_state.aa_feed_counter++; /* one more read */
        if (a2d_sbc_encoder_cb.feeding_state.aa_feed_counter > fract_max) {
            a2d_sbc_encoder_cb.feeding_state.aa_feed_counter = 0;
        }
    }

    /* Compute number of bytes to read from source */
    read_size = src_samples;
    read_size *= a2d_sbc_encoder_cb.feeding_params.num_channel;
    read_size *= (a2d_sbc_encoder_cb.feeding_params.bit_per_sample / 8);

    /* Read Data from UIPC channel */
    nb_byte_read = a2d_sbc_encoder_cb.read_callback((uint8_t *)read_buffer,
                                                    read_size);

    if (nb_byte_read < read_size) {
        if (nb_byte_read == 0)
            return false;

        /* Fill the unfilled part of the read buffer with silence (0) */
        memset(((uint8_t *)read_buffer) + nb_byte_read, 0,
               read_size - nb_byte_read);
        nb_byte_read = read_size;
    }

    /* Initialize PCM up-sampling engine */
    a2d_sbc_init_up_sample(a2d_sbc_encoder_cb.feeding_params.sampling_freq,
                           sbc_sampling,
                           a2d_sbc_encoder_cb.feeding_params.bit_per_sample,
                           a2d_sbc_encoder_cb.feeding_params.num_channel);

    /*
     * Re-sample the read buffer.
     * The output PCM buffer will be stereo, 16 bit per sample.
     */
    dst_size_used = a2d_sbc_up_sample((uint8_t *)read_buffer,
            (uint8_t *)up_sampled_buffer + a2d_sbc_encoder_cb.feeding_state.aa_feed_residue,
            nb_byte_read,
            sizeof(up_sampled_buffer) - a2d_sbc_encoder_cb.feeding_state.aa_feed_residue,
            &src_size_used);

    /* update the residue */
    a2d_sbc_encoder_cb.feeding_state.aa_feed_residue += dst_size_used;

    /* only copy the pcm sample when we have up-sampled enough PCM */
    if (a2d_sbc_encoder_cb.feeding_state.aa_feed_residue < bytes_needed)
        return false;

    /* Copy the output pcm samples in SBC encoding buffer */
    memcpy((uint8_t *)p_encoder_params->as16PcmBuffer,
           (uint8_t *)up_sampled_buffer, bytes_needed);
    /* update the residue */
    a2d_sbc_encoder_cb.feeding_state.aa_feed_residue -= bytes_needed;

    if (a2d_sbc_encoder_cb.feeding_state.aa_feed_residue != 0) {
        memcpy((uint8_t *)up_sampled_buffer,
               (uint8_t *)up_sampled_buffer + bytes_needed,
               a2d_sbc_encoder_cb.feeding_state.aa_feed_residue);
    }
    return true;
}

static uint8_t calculate_max_frames_per_packet(void)
{
    uint16_t effective_mtu_size = a2d_sbc_encoder_cb.TxAaMtuSize;
    SBC_ENC_PARAMS *p_encoder_params = &a2d_sbc_encoder_cb.sbc_encoder_params;
    uint16_t result = 0;
    uint32_t frame_len;

    LOG_VERBOSE(LOG_TAG, "%s: original AVDTP MTU size: %d",
                __func__, a2d_sbc_encoder_cb.TxAaMtuSize);
    if (a2d_sbc_encoder_cb.is_peer_edr &&
        !a2d_sbc_encoder_cb.peer_supports_3mbps) {
        // This condition would be satisfied only if the remote device is
        // EDR and supports only 2 Mbps, but the effective AVDTP MTU size
        // exceeds the 2DH5 packet size.
        LOG_VERBOSE(LOG_TAG, "%s: The remote devce is EDR but does not support 3 Mbps",
                    __func__);

        if (effective_mtu_size > MAX_2MBPS_AVDTP_MTU) {
            LOG_WARN(LOG_TAG, "%s: Restricting AVDTP MTU size to %d",
                     __func__, MAX_2MBPS_AVDTP_MTU);
            effective_mtu_size = MAX_2MBPS_AVDTP_MTU;
            a2d_sbc_encoder_cb.TxAaMtuSize = effective_mtu_size;
        }
    }

    if (!p_encoder_params->s16NumOfSubBands) {
        LOG_ERROR(LOG_TAG, "%s: SubBands are set to 0, resetting to %d",
                  __func__, SBC_MAX_NUM_OF_SUBBANDS);
        p_encoder_params->s16NumOfSubBands = SBC_MAX_NUM_OF_SUBBANDS;
    }
    if (!p_encoder_params->s16NumOfBlocks) {
        LOG_ERROR(LOG_TAG, "%s: Blocks are set to 0, resetting to %d",
                  __func__, SBC_MAX_NUM_OF_BLOCKS);
        p_encoder_params->s16NumOfBlocks = SBC_MAX_NUM_OF_BLOCKS;
    }
    if (!p_encoder_params->s16NumOfChannels) {
        LOG_ERROR(LOG_TAG, "%s: Channels are set to 0, resetting to %d",
                  __func__, SBC_MAX_NUM_OF_CHANNELS);
        p_encoder_params->s16NumOfChannels = SBC_MAX_NUM_OF_CHANNELS;
    }

    frame_len = a2d_sbc_frame_length();

    LOG_VERBOSE(LOG_TAG, "%s: Effective Tx MTU to be considered: %d",
                __func__, effective_mtu_size);

    switch (p_encoder_params->s16SamplingFreq) {
        case SBC_sf44100:
            if (frame_len == 0) {
                LOG_ERROR(LOG_TAG, "%s: Calculating frame length, resetting it to default %d",
                          __func__, A2D_SBC_MAX_HQ_FRAME_SIZE_44_1);
                frame_len = A2D_SBC_MAX_HQ_FRAME_SIZE_44_1;
            }
            result = (effective_mtu_size - A2DP_HDR_SIZE) / frame_len;
            LOG_VERBOSE(LOG_TAG, "%s: Max number of SBC frames: %d",
                        __func__, result);
            break;

        case SBC_sf48000:
            if (frame_len == 0) {
              LOG_ERROR(LOG_TAG, "%s: Calculating frame length, resetting it to default %d",
                        __func__, A2D_SBC_MAX_HQ_FRAME_SIZE_48);
                frame_len = A2D_SBC_MAX_HQ_FRAME_SIZE_48;
            }
            result = (effective_mtu_size - A2DP_HDR_SIZE) / frame_len;
            LOG_VERBOSE(LOG_TAG, "%s: Max number of SBC frames: %d",
                        __func__, result);
            break;

        default:
            LOG_ERROR(LOG_TAG, "%s: Max number of SBC frames: %d",
                      __func__, result);
            break;
    }
    return result;
}

static uint16_t a2d_sbc_source_rate(void)
{
    uint16_t rate = A2D_SBC_DEFAULT_BITRATE;

    /* restrict bitrate if a2dp link is non-edr */
    if (!a2d_sbc_encoder_cb.is_peer_edr) {
        rate = A2D_SBC_NON_EDR_MAX_RATE;
        LOG_VERBOSE(LOG_TAG, "%s: non-edr a2dp sink detected, restrict rate to %d",
                    __func__, rate);
    }

    return rate;
}

static uint32_t a2d_sbc_frame_length(void)
{
    SBC_ENC_PARAMS *p_encoder_params = &a2d_sbc_encoder_cb.sbc_encoder_params;
    uint32_t frame_len = 0;

    LOG_VERBOSE(LOG_TAG, "%s: channel mode: %d, sub-band: %d, number of block: %d, bitpool: %d, sampling frequency: %d, num channels: %d",
                __func__,
                p_encoder_params->s16ChannelMode,
                p_encoder_params->s16NumOfSubBands,
                p_encoder_params->s16NumOfBlocks,
                p_encoder_params->s16BitPool,
                p_encoder_params->s16SamplingFreq,
                p_encoder_params->s16NumOfChannels);

    switch (p_encoder_params->s16ChannelMode) {
        case SBC_MONO:
            /* FALLTHROUGH */
        case SBC_DUAL:
            frame_len = A2D_SBC_FRAME_HEADER_SIZE_BYTES +
                ((uint32_t)(A2D_SBC_SCALE_FACTOR_BITS *
                            p_encoder_params->s16NumOfSubBands *
                            p_encoder_params->s16NumOfChannels) / CHAR_BIT) +
              ((uint32_t)(p_encoder_params->s16NumOfBlocks *
                          p_encoder_params->s16NumOfChannels *
                          p_encoder_params->s16BitPool) / CHAR_BIT);
            break;
        case SBC_STEREO:
            frame_len = A2D_SBC_FRAME_HEADER_SIZE_BYTES +
                ((uint32_t)(A2D_SBC_SCALE_FACTOR_BITS *
                            p_encoder_params->s16NumOfSubBands *
                            p_encoder_params->s16NumOfChannels) / CHAR_BIT) +
                ((uint32_t)(p_encoder_params->s16NumOfBlocks *
                            p_encoder_params->s16BitPool) / CHAR_BIT);
            break;
        case SBC_JOINT_STEREO:
            frame_len = A2D_SBC_FRAME_HEADER_SIZE_BYTES +
                ((uint32_t)(A2D_SBC_SCALE_FACTOR_BITS *
                            p_encoder_params->s16NumOfSubBands *
                            p_encoder_params->s16NumOfChannels) / CHAR_BIT) +
                ((uint32_t)(p_encoder_params->s16NumOfSubBands +
                            (p_encoder_params->s16NumOfBlocks *
                             p_encoder_params->s16BitPool)) / CHAR_BIT);
            break;
        default:
            LOG_VERBOSE(LOG_TAG, "%s: Invalid channel number: %d",
                        __func__, p_encoder_params->s16ChannelMode);
            break;
    }
    LOG_VERBOSE(LOG_TAG, "%s: calculated frame length: %d",
                __func__, frame_len);
    return frame_len;
}

void a2d_sbc_debug_codec_dump(int fd)
{
    a2d_sbc_encoder_stats_t *stats = &a2d_sbc_encoder_cb.stats;
    size_t ave_size;

    dprintf(fd, "\nA2DP SBC State:\n");

    ave_size = 0;
    if (stats->media_read_expected_count != 0) {
        ave_size = stats->media_read_total_expected_frames /
            stats->media_read_expected_count;
    }
    dprintf(fd, "  Frames expected (total/max/ave)                         : %zu / %zu / %zu\n",
            stats->media_read_total_expected_frames,
            stats->media_read_max_expected_frames,
            ave_size);

    ave_size = 0;
    if (stats->media_read_limited_count != 0) {
        ave_size = stats->media_read_total_limited_frames /
            stats->media_read_limited_count;
    }
    dprintf(fd, "  Frames limited (total/max/ave)                          : %zu / %zu / %zu\n",
            stats->media_read_total_limited_frames,
            stats->media_read_max_limited_frames,
            ave_size);

    dprintf(fd, "  Counts (expected/limited)                               : %zu / %zu\n",
            stats->media_read_expected_count,
            stats->media_read_limited_count);
}