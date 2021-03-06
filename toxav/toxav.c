/*
 * Copyright © 2016-2017 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "toxav.h"

#include "msi.h"
#include "rtp.h"

#include "../toxcore/Messenger.h"
#include "../toxcore/logger.h"
#include "../toxcore/mono_time.h"
#include "../toxcore/util.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// TODO(zoff99): don't hardcode this, let the application choose it
// VPX Info: Time to spend encoding, in microseconds (it's a *soft* deadline)
#define WANTED_MAX_ENCODER_FPS (40)
#define MAX_ENCODE_TIME_US (1000000 / WANTED_MAX_ENCODER_FPS) // to allow x fps

#define VIDEO_SEND_X_KEYFRAMES_FIRST 7 // force the first n frames to be keyframes!

/*
VPX_DL_REALTIME       (1)       deadline parameter analogous to VPx REALTIME mode.
VPX_DL_GOOD_QUALITY   (1000000) deadline parameter analogous to VPx GOOD QUALITY mode.
VPX_DL_BEST_QUALITY   (0)       deadline parameter analogous to VPx BEST QUALITY mode.
*/

typedef struct ToxAVCall_s {
    ToxAV *av;

    pthread_mutex_t mutex_audio[1];
    PAIR(RTPSession *, ACSession *) audio;

    pthread_mutex_t mutex_video[1];
    PAIR(RTPSession *, VCSession *) video;

    BWController *bwc;

    bool active;
    MSICall *msi_call;
    uint32_t friend_number;

    uint32_t audio_bit_rate; /* Sending audio bit rate */
    uint32_t video_bit_rate; /* Sending video bit rate */

    /** Required for monitoring changes in states */
    uint8_t previous_self_capabilities;

    pthread_mutex_t mutex[1];

    struct ToxAVCall_s *prev;
    struct ToxAVCall_s *next;
} ToxAVCall;

struct ToxAV {
    Messenger *m;
    MSISession *msi;

    /* Two-way storage: first is array of calls and second is list of calls with head and tail */
    ToxAVCall **calls;
    uint32_t calls_tail;
    uint32_t calls_head;
    pthread_mutex_t mutex[1];

    PAIR(toxav_call_cb *, void *) ccb; /* Call callback */
    PAIR(toxav_call_state_cb *, void *) scb; /* Call state callback */
    PAIR(toxav_audio_receive_frame_cb *, void *) acb; /* Audio frame receive callback */
    PAIR(toxav_video_receive_frame_cb *, void *) vcb; /* Video frame receive callback */
    PAIR(toxav_audio_bit_rate_cb *, void *) abcb; /* Bit rate control callback */
    PAIR(toxav_video_bit_rate_cb *, void *) vbcb; /* Bit rate control callback */

    /** Decode time measures */
    int32_t dmssc; /** Measure count */
    int32_t dmsst; /** Last cycle total */
    int32_t dmssa; /** Average decoding time in ms */

    uint32_t interval; /** Calculated interval */
};

void callback_bwc(BWController *bwc, uint32_t friend_number, float loss, void *user_data);

int callback_invite(void *toxav_inst, MSICall *call);
int callback_start(void *toxav_inst, MSICall *call);
int callback_end(void *toxav_inst, MSICall *call);
int callback_error(void *toxav_inst, MSICall *call);
int callback_capabilites(void *toxav_inst, MSICall *call);

bool audio_bit_rate_invalid(uint32_t bit_rate);
bool video_bit_rate_invalid(uint32_t bit_rate);
bool invoke_call_state_callback(ToxAV *av, uint32_t friend_number, uint32_t state);
ToxAVCall *call_new(ToxAV *av, uint32_t friend_number, TOXAV_ERR_CALL *error);
ToxAVCall *call_get(ToxAV *av, uint32_t friend_number);
ToxAVCall *call_remove(ToxAVCall *call);
bool call_prepare_transmission(ToxAVCall *call);
void call_kill_transmission(ToxAVCall *call);

ToxAV *toxav_new(Tox *tox, TOXAV_ERR_NEW *error)
{
    TOXAV_ERR_NEW rc = TOXAV_ERR_NEW_OK;
    ToxAV *av = nullptr;

    if (tox == nullptr) {
        rc = TOXAV_ERR_NEW_NULL;
        goto END;
    }

    // TODO(iphydf): Don't rely on toxcore internals.
    Messenger *m;
    m = *(Messenger **)tox;

    if (m->msi_packet) {
        rc = TOXAV_ERR_NEW_MULTIPLE;
        goto END;
    }

    av = (ToxAV *)calloc(sizeof(ToxAV), 1);

    if (av == nullptr) {
        LOGGER_WARNING(m->log, "Allocation failed!");
        rc = TOXAV_ERR_NEW_MALLOC;
        goto END;
    }

    if (create_recursive_mutex(av->mutex) != 0) {
        LOGGER_WARNING(m->log, "Mutex creation failed!");
        rc = TOXAV_ERR_NEW_MALLOC;
        goto END;
    }

    av->m = m;
    av->msi = msi_new(av->m);

    if (av->msi == nullptr) {
        pthread_mutex_destroy(av->mutex);
        rc = TOXAV_ERR_NEW_MALLOC;
        goto END;
    }

    av->interval = 200;
    av->msi->av = av;

    msi_register_callback(av->msi, callback_invite, msi_OnInvite);
    msi_register_callback(av->msi, callback_start, msi_OnStart);
    msi_register_callback(av->msi, callback_end, msi_OnEnd);
    msi_register_callback(av->msi, callback_error, msi_OnError);
    msi_register_callback(av->msi, callback_error, msi_OnPeerTimeout);
    msi_register_callback(av->msi, callback_capabilites, msi_OnCapabilities);

END:

    if (error) {
        *error = rc;
    }

    if (rc != TOXAV_ERR_NEW_OK) {
        free(av);
        av = nullptr;
    }

    return av;
}
void toxav_kill(ToxAV *av)
{
    if (av == nullptr) {
        return;
    }

    pthread_mutex_lock(av->mutex);

    /* To avoid possible deadlocks */
    while (av->msi && msi_kill(av->msi, av->m->log) != 0) {
        pthread_mutex_unlock(av->mutex);
        pthread_mutex_lock(av->mutex);
    }

    /* Msi kill will hang up all calls so just clean these calls */
    if (av->calls) {
        ToxAVCall *it = call_get(av, av->calls_head);

        while (it) {
            call_kill_transmission(it);
            it->msi_call = nullptr; /* msi_kill() frees the call's msi_call handle; which causes #278 */
            it = call_remove(it); /* This will eventually free av->calls */
        }
    }

    pthread_mutex_unlock(av->mutex);
    pthread_mutex_destroy(av->mutex);

    free(av);
}
Tox *toxav_get_tox(const ToxAV *av)
{
    return (Tox *) av->m;
}
uint32_t toxav_iteration_interval(const ToxAV *av)
{
    /* If no call is active interval is 200 */
    return av->calls ? av->interval : 200;
}
void toxav_iterate(ToxAV *av)
{
    pthread_mutex_lock(av->mutex);

    if (av->calls == nullptr) {
        pthread_mutex_unlock(av->mutex);
        return;
    }

    uint64_t start = current_time_monotonic();
    int32_t rc = 500;

    ToxAVCall *i = av->calls[av->calls_head];

    for (; i; i = i->next) {
        if (i->active) {
            pthread_mutex_lock(i->mutex);
            pthread_mutex_unlock(av->mutex);

            ac_iterate(i->audio.second);
            vc_iterate(i->video.second);

            // TODO(iphydf): Find out what MIN-semantics are desired here and
            // use a min_* function from util.h.
#define MIN(a,b) (((a)<(b))?(a):(b))

            if (i->msi_call->self_capabilities & msi_CapRAudio &&
                    i->msi_call->peer_capabilities & msi_CapSAudio) {
                rc = MIN(i->audio.second->lp_frame_duration, rc);
            }

            if (i->msi_call->self_capabilities & msi_CapRVideo &&
                    i->msi_call->peer_capabilities & msi_CapSVideo) {
                rc = MIN(i->video.second->lcfd, (uint32_t) rc);
            }

            uint32_t fid = i->friend_number;

            pthread_mutex_unlock(i->mutex);
            pthread_mutex_lock(av->mutex);

            /* In case this call is popped from container stop iteration */
            if (call_get(av, fid) != i) {
                break;
            }
        }
    }

    pthread_mutex_unlock(av->mutex);

    av->interval = rc < av->dmssa ? 0 : (rc - av->dmssa);
    av->dmsst += current_time_monotonic() - start;

    if (++av->dmssc == 3) {
        av->dmssa = av->dmsst / 3 + 5 /* NOTE Magic Offset for precission */;
        av->dmssc = 0;
        av->dmsst = 0;
    }
}
bool toxav_call(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate, uint32_t video_bit_rate,
                TOXAV_ERR_CALL *error)
{
    TOXAV_ERR_CALL rc = TOXAV_ERR_CALL_OK;
    ToxAVCall *call;

    pthread_mutex_lock(av->mutex);

    if ((audio_bit_rate && audio_bit_rate_invalid(audio_bit_rate))
            || (video_bit_rate && video_bit_rate_invalid(video_bit_rate))) {
        rc = TOXAV_ERR_CALL_INVALID_BIT_RATE;
        goto END;
    }

    call = call_new(av, friend_number, &rc);

    if (call == nullptr) {
        goto END;
    }

    call->audio_bit_rate = audio_bit_rate;
    call->video_bit_rate = video_bit_rate;

    call->previous_self_capabilities = msi_CapRAudio | msi_CapRVideo;

    call->previous_self_capabilities |= audio_bit_rate > 0 ? msi_CapSAudio : 0;
    call->previous_self_capabilities |= video_bit_rate > 0 ? msi_CapSVideo : 0;

    if (msi_invite(av->msi, &call->msi_call, friend_number, call->previous_self_capabilities) != 0) {
        call_remove(call);
        rc = TOXAV_ERR_CALL_SYNC;
        goto END;
    }

    call->msi_call->av_call = call;

END:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_CALL_OK;
}
void toxav_callback_call(ToxAV *av, toxav_call_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->ccb.first = callback;
    av->ccb.second = user_data;
    pthread_mutex_unlock(av->mutex);
}
bool toxav_answer(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate, uint32_t video_bit_rate,
                  TOXAV_ERR_ANSWER *error)
{
    pthread_mutex_lock(av->mutex);

    TOXAV_ERR_ANSWER rc = TOXAV_ERR_ANSWER_OK;
    ToxAVCall *call;

    if (m_friend_exists(av->m, friend_number) == 0) {
        rc = TOXAV_ERR_ANSWER_FRIEND_NOT_FOUND;
        goto END;
    }

    if ((audio_bit_rate && audio_bit_rate_invalid(audio_bit_rate))
            || (video_bit_rate && video_bit_rate_invalid(video_bit_rate))
       ) {
        rc = TOXAV_ERR_ANSWER_INVALID_BIT_RATE;
        goto END;
    }

    call = call_get(av, friend_number);

    if (call == nullptr) {
        rc = TOXAV_ERR_ANSWER_FRIEND_NOT_CALLING;
        goto END;
    }

    if (!call_prepare_transmission(call)) {
        rc = TOXAV_ERR_ANSWER_CODEC_INITIALIZATION;
        goto END;
    }

    call->audio_bit_rate = audio_bit_rate;
    call->video_bit_rate = video_bit_rate;

    call->previous_self_capabilities = msi_CapRAudio | msi_CapRVideo;

    call->previous_self_capabilities |= audio_bit_rate > 0 ? msi_CapSAudio : 0;
    call->previous_self_capabilities |= video_bit_rate > 0 ? msi_CapSVideo : 0;

    if (msi_answer(call->msi_call, call->previous_self_capabilities) != 0) {
        rc = TOXAV_ERR_ANSWER_SYNC;
    }

END:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_ANSWER_OK;
}
void toxav_callback_call_state(ToxAV *av, toxav_call_state_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->scb.first = callback;
    av->scb.second = user_data;
    pthread_mutex_unlock(av->mutex);
}
bool toxav_call_control(ToxAV *av, uint32_t friend_number, TOXAV_CALL_CONTROL control, TOXAV_ERR_CALL_CONTROL *error)
{
    pthread_mutex_lock(av->mutex);
    TOXAV_ERR_CALL_CONTROL rc = TOXAV_ERR_CALL_CONTROL_OK;
    ToxAVCall *call;

    if (m_friend_exists(av->m, friend_number) == 0) {
        rc = TOXAV_ERR_CALL_CONTROL_FRIEND_NOT_FOUND;
        goto END;
    }

    call = call_get(av, friend_number);

    if (call == nullptr || (!call->active && control != TOXAV_CALL_CONTROL_CANCEL)) {
        rc = TOXAV_ERR_CALL_CONTROL_FRIEND_NOT_IN_CALL;
        goto END;
    }

    switch (control) {
        case TOXAV_CALL_CONTROL_RESUME: {
            /* Only act if paused and had media transfer active before */
            if (call->msi_call->self_capabilities == 0 &&
                    call->previous_self_capabilities) {

                if (msi_change_capabilities(call->msi_call,
                                            call->previous_self_capabilities) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto END;
                }

                rtp_allow_receiving(call->audio.first);
                rtp_allow_receiving(call->video.first);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto END;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_PAUSE: {
            /* Only act if not already paused */
            if (call->msi_call->self_capabilities) {
                call->previous_self_capabilities = call->msi_call->self_capabilities;

                if (msi_change_capabilities(call->msi_call, 0) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto END;
                }

                rtp_stop_receiving(call->audio.first);
                rtp_stop_receiving(call->video.first);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto END;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_CANCEL: {
            /* Hang up */
            pthread_mutex_lock(call->mutex);

            if (msi_hangup(call->msi_call) != 0) {
                rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                pthread_mutex_unlock(call->mutex);
                goto END;
            }

            call->msi_call = nullptr;
            pthread_mutex_unlock(call->mutex);

            /* No mather the case, terminate the call */
            call_kill_transmission(call);
            call_remove(call);
        }
        break;

        case TOXAV_CALL_CONTROL_MUTE_AUDIO: {
            if (call->msi_call->self_capabilities & msi_CapRAudio) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities ^ msi_CapRAudio) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto END;
                }

                rtp_stop_receiving(call->audio.first);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto END;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_UNMUTE_AUDIO: {
            if (call->msi_call->self_capabilities ^ msi_CapRAudio) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities | msi_CapRAudio) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto END;
                }

                rtp_allow_receiving(call->audio.first);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto END;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_HIDE_VIDEO: {
            if (call->msi_call->self_capabilities & msi_CapRVideo) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities ^ msi_CapRVideo) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto END;
                }

                rtp_stop_receiving(call->video.first);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto END;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_SHOW_VIDEO: {
            if (call->msi_call->self_capabilities ^ msi_CapRVideo) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities | msi_CapRVideo) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto END;
                }

                rtp_allow_receiving(call->video.first);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto END;
            }
        }
        break;
    }

END:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_CALL_CONTROL_OK;
}
bool toxav_audio_set_bit_rate(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate,
                              TOXAV_ERR_BIT_RATE_SET *error)
{
    TOXAV_ERR_BIT_RATE_SET rc = TOXAV_ERR_BIT_RATE_SET_OK;
    ToxAVCall *call;

    if (m_friend_exists(av->m, friend_number) == 0) {
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_FOUND;
        goto END;
    }

    if (audio_bit_rate > 0 && audio_bit_rate_invalid(audio_bit_rate)) {
        rc = TOXAV_ERR_BIT_RATE_SET_INVALID_BIT_RATE;
        goto END;
    }

    pthread_mutex_lock(av->mutex);
    call = call_get(av, friend_number);

    if (call == nullptr || !call->active || call->msi_call->state != msi_CallActive) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_IN_CALL;
        goto END;
    }

    LOGGER_DEBUG(av->m->log, "Setting new audio bitrate to: %d", audio_bit_rate);

    if (call->audio_bit_rate == audio_bit_rate) {
        LOGGER_DEBUG(av->m->log, "Audio bitrate already set to: %d", audio_bit_rate);
    } else if (audio_bit_rate == 0) {
        LOGGER_DEBUG(av->m->log, "Turned off audio sending");

        if (msi_change_capabilities(call->msi_call, call->msi_call->
                                    self_capabilities ^ msi_CapSAudio) != 0) {
            pthread_mutex_unlock(av->mutex);
            rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
            goto END;
        }

        /* Audio sending is turned off; notify peer */
        call->audio_bit_rate = 0;
    } else {
        pthread_mutex_lock(call->mutex);

        if (call->audio_bit_rate == 0) {
            LOGGER_DEBUG(av->m->log, "Turned on audio sending");

            /* The audio has been turned off before this */
            if (msi_change_capabilities(call->msi_call, call->
                                        msi_call->self_capabilities | msi_CapSAudio) != 0) {
                pthread_mutex_unlock(call->mutex);
                pthread_mutex_unlock(av->mutex);
                rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                goto END;
            }
        } else {
            LOGGER_DEBUG(av->m->log, "Set new audio bit rate %d", audio_bit_rate);
        }

        call->audio_bit_rate = audio_bit_rate;
        pthread_mutex_unlock(call->mutex);
    }

    pthread_mutex_unlock(av->mutex);
END:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_BIT_RATE_SET_OK;
}
bool toxav_video_set_bit_rate(ToxAV *av, uint32_t friend_number, uint32_t video_bit_rate,
                              TOXAV_ERR_BIT_RATE_SET *error)
{
    TOXAV_ERR_BIT_RATE_SET rc = TOXAV_ERR_BIT_RATE_SET_OK;
    ToxAVCall *call;

    if (m_friend_exists(av->m, friend_number) == 0) {
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_FOUND;
        goto END;
    }

    if (video_bit_rate > 0 && video_bit_rate_invalid(video_bit_rate)) {
        rc = TOXAV_ERR_BIT_RATE_SET_INVALID_BIT_RATE;
        goto END;
    }

    pthread_mutex_lock(av->mutex);
    call = call_get(av, friend_number);

    if (call == nullptr || !call->active || call->msi_call->state != msi_CallActive) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_IN_CALL;
        goto END;
    }

    LOGGER_DEBUG(av->m->log, "Setting new video bitrate to: %d", video_bit_rate);

    if (call->video_bit_rate == video_bit_rate) {
        LOGGER_DEBUG(av->m->log, "Video bitrate already set to: %d", video_bit_rate);
    } else if (video_bit_rate == 0) {
        LOGGER_DEBUG(av->m->log, "Turned off video sending");

        /* Video sending is turned off; notify peer */
        if (msi_change_capabilities(call->msi_call, call->msi_call->
                                    self_capabilities ^ msi_CapSVideo) != 0) {
            pthread_mutex_unlock(av->mutex);
            rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
            goto END;
        }

        call->video_bit_rate = 0;
    } else {
        pthread_mutex_lock(call->mutex);

        if (call->video_bit_rate == 0) {
            LOGGER_DEBUG(av->m->log, "Turned on video sending");

            /* The video has been turned off before this */
            if (msi_change_capabilities(call->msi_call, call->
                                        msi_call->self_capabilities | msi_CapSVideo) != 0) {
                pthread_mutex_unlock(call->mutex);
                pthread_mutex_unlock(av->mutex);
                rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                goto END;
            }
        } else {
            LOGGER_DEBUG(av->m->log, "Set new video bit rate %d", video_bit_rate);
        }

        call->video_bit_rate = video_bit_rate;
        pthread_mutex_unlock(call->mutex);
    }

    pthread_mutex_unlock(av->mutex);
END:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_BIT_RATE_SET_OK;
}
void toxav_callback_audio_bit_rate(ToxAV *av, toxav_audio_bit_rate_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->abcb.first = callback;
    av->abcb.second = user_data;
    pthread_mutex_unlock(av->mutex);
}
void toxav_callback_video_bit_rate(ToxAV *av, toxav_video_bit_rate_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->vbcb.first = callback;
    av->vbcb.second = user_data;
    pthread_mutex_unlock(av->mutex);
}
bool toxav_audio_send_frame(ToxAV *av, uint32_t friend_number, const int16_t *pcm, size_t sample_count,
                            uint8_t channels, uint32_t sampling_rate, TOXAV_ERR_SEND_FRAME *error)
{
    TOXAV_ERR_SEND_FRAME rc = TOXAV_ERR_SEND_FRAME_OK;
    ToxAVCall *call;

    if (m_friend_exists(av->m, friend_number) == 0) {
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_FOUND;
        goto END;
    }

    if (pthread_mutex_trylock(av->mutex) != 0) {
        rc = TOXAV_ERR_SEND_FRAME_SYNC;
        goto END;
    }

    call = call_get(av, friend_number);

    if (call == nullptr || !call->active || call->msi_call->state != msi_CallActive) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_IN_CALL;
        goto END;
    }

    if (call->audio_bit_rate == 0 ||
            !(call->msi_call->self_capabilities & msi_CapSAudio) ||
            !(call->msi_call->peer_capabilities & msi_CapRAudio)) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED;
        goto END;
    }

    pthread_mutex_lock(call->mutex_audio);
    pthread_mutex_unlock(av->mutex);

    if (pcm == nullptr) {
        pthread_mutex_unlock(call->mutex_audio);
        rc = TOXAV_ERR_SEND_FRAME_NULL;
        goto END;
    }

    if (channels > 2) {
        pthread_mutex_unlock(call->mutex_audio);
        rc = TOXAV_ERR_SEND_FRAME_INVALID;
        goto END;
    }

    { /* Encode and send */
        if (ac_reconfigure_encoder(call->audio.second, call->audio_bit_rate * 1000, sampling_rate, channels) != 0) {
            pthread_mutex_unlock(call->mutex_audio);
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto END;
        }

        VLA(uint8_t, dest, sample_count + sizeof(sampling_rate)); /* This is more than enough always */

        sampling_rate = net_htonl(sampling_rate);
        memcpy(dest, &sampling_rate, sizeof(sampling_rate));
        int vrc = opus_encode(call->audio.second->encoder, pcm, sample_count,
                              dest + sizeof(sampling_rate), SIZEOF_VLA(dest) - sizeof(sampling_rate));

        if (vrc < 0) {
            LOGGER_WARNING(av->m->log, "Failed to encode frame %s", opus_strerror(vrc));
            pthread_mutex_unlock(call->mutex_audio);
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto END;
        }

        if (rtp_send_data(call->audio.first, dest, vrc + sizeof(sampling_rate), false, av->m->log) != 0) {
            LOGGER_WARNING(av->m->log, "Failed to send audio packet");
            rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
        }
    }

    pthread_mutex_unlock(call->mutex_audio);

END:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_SEND_FRAME_OK;
}

bool toxav_video_send_frame(ToxAV *av, uint32_t friend_number, uint16_t width, uint16_t height, const uint8_t *y,
                            const uint8_t *u, const uint8_t *v, TOXAV_ERR_SEND_FRAME *error)
{
    TOXAV_ERR_SEND_FRAME rc = TOXAV_ERR_SEND_FRAME_OK;
    ToxAVCall *call;

    int vpx_encode_flags = 0;

    if (m_friend_exists(av->m, friend_number) == 0) {
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_FOUND;
        goto END;
    }

    if (pthread_mutex_trylock(av->mutex) != 0) {
        rc = TOXAV_ERR_SEND_FRAME_SYNC;
        goto END;
    }

    call = call_get(av, friend_number);

    if (call == nullptr || !call->active || call->msi_call->state != msi_CallActive) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_IN_CALL;
        goto END;
    }

    if (call->video_bit_rate == 0 ||
            !(call->msi_call->self_capabilities & msi_CapSVideo) ||
            !(call->msi_call->peer_capabilities & msi_CapRVideo)) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED;
        goto END;
    }

    pthread_mutex_lock(call->mutex_video);
    pthread_mutex_unlock(av->mutex);

    if (y == nullptr || u == nullptr || v == nullptr) {
        pthread_mutex_unlock(call->mutex_video);
        rc = TOXAV_ERR_SEND_FRAME_NULL;
        goto END;
    }

    if (vc_reconfigure_encoder(call->video.second, call->video_bit_rate * 1000, width, height, -1) != 0) {
        pthread_mutex_unlock(call->mutex_video);
        rc = TOXAV_ERR_SEND_FRAME_INVALID;
        goto END;
    }

    if (call->video.first->ssrc < VIDEO_SEND_X_KEYFRAMES_FIRST) {
        // Key frame flag for first frames
        vpx_encode_flags = VPX_EFLAG_FORCE_KF;
        LOGGER_INFO(av->m->log, "I_FRAME_FLAG:%d only-i-frame mode", call->video.first->ssrc);

        call->video.first->ssrc++;
    } else if (call->video.first->ssrc == VIDEO_SEND_X_KEYFRAMES_FIRST) {
        // normal keyframe placement
        vpx_encode_flags = 0;
        LOGGER_INFO(av->m->log, "I_FRAME_FLAG:%d normal mode", call->video.first->ssrc);

        call->video.first->ssrc++;
    }

    // we start with I-frames (full frames) and then switch to normal mode later

    { /* Encode */
        vpx_image_t img;
        img.w = img.h = img.d_w = img.d_h = 0;
        vpx_img_alloc(&img, VPX_IMG_FMT_I420, width, height, 0);

        /* I420 "It comprises an NxM Y plane followed by (N/2)x(M/2) V and U planes."
         * http://fourcc.org/yuv.php#IYUV
         */
        memcpy(img.planes[VPX_PLANE_Y], y, width * height);
        memcpy(img.planes[VPX_PLANE_U], u, (width / 2) * (height / 2));
        memcpy(img.planes[VPX_PLANE_V], v, (width / 2) * (height / 2));

        vpx_codec_err_t vrc = vpx_codec_encode(call->video.second->encoder, &img,
                                               call->video.second->frame_counter, 1, vpx_encode_flags, MAX_ENCODE_TIME_US);

        vpx_img_free(&img);

        if (vrc != VPX_CODEC_OK) {
            pthread_mutex_unlock(call->mutex_video);
            LOGGER_ERROR(av->m->log, "Could not encode video frame: %s\n", vpx_codec_err_to_string(vrc));
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto END;
        }
    }

    ++call->video.second->frame_counter;

    { /* Send frames */
        vpx_codec_iter_t iter = nullptr;
        const vpx_codec_cx_pkt_t *pkt;

        while ((pkt = vpx_codec_get_cx_data(call->video.second->encoder, &iter)) != nullptr) {
            if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
                const bool is_keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;

                // https://www.webmproject.org/docs/webm-sdk/structvpx__codec__cx__pkt.html
                // pkt->data.frame.sz -> size_t
                const uint32_t frame_length_in_bytes = pkt->data.frame.sz;

                const int res = rtp_send_data(
                                    call->video.first,
                                    (const uint8_t *)pkt->data.frame.buf,
                                    frame_length_in_bytes,
                                    is_keyframe,
                                    av->m->log);

                LOGGER_DEBUG(av->m->log, "+ _sending_FRAME_TYPE_==%s bytes=%d frame_len=%d", is_keyframe ? "K" : ".",
                             (int)pkt->data.frame.sz, (int)frame_length_in_bytes);
                LOGGER_DEBUG(av->m->log, "+ _sending_FRAME_ b0=%d b1=%d", ((const uint8_t *)pkt->data.frame.buf)[0],
                             ((const uint8_t *)pkt->data.frame.buf)[1]);

                if (res < 0) {
                    pthread_mutex_unlock(call->mutex_video);
                    LOGGER_WARNING(av->m->log, "Could not send video frame: %s", strerror(errno));
                    rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
                    goto END;
                }
            }
        }
    }

    pthread_mutex_unlock(call->mutex_video);

END:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_SEND_FRAME_OK;
}

void toxav_callback_audio_receive_frame(ToxAV *av, toxav_audio_receive_frame_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->acb.first = callback;
    av->acb.second = user_data;
    pthread_mutex_unlock(av->mutex);
}

void toxav_callback_video_receive_frame(ToxAV *av, toxav_video_receive_frame_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->vcb.first = callback;
    av->vcb.second = user_data;
    pthread_mutex_unlock(av->mutex);
}

/*******************************************************************************
 *
 * :: Internal
 *
 ******************************************************************************/
void callback_bwc(BWController *bwc, uint32_t friend_number, float loss, void *user_data)
{
    /* Callback which is called when the internal measure mechanism reported packet loss.
     * We report suggested lowered bitrate to an app. If app is sending both audio and video,
     * we will report lowered bitrate for video only because in that case video probably
     * takes more than 90% bandwidth. Otherwise, we report lowered bitrate on audio.
     * The application may choose to disable video totally if the stream is too bad.
     */

    ToxAVCall *call = (ToxAVCall *)user_data;
    assert(call);

    LOGGER_DEBUG(call->av->m->log, "Reported loss of %f%%", (double)loss * 100);

    /* if less than 10% data loss we do nothing! */
    if (loss < 0.1f) {
        return;
    }

    pthread_mutex_lock(call->av->mutex);

    if (call->video_bit_rate) {
        if (!call->av->vbcb.first) {
            pthread_mutex_unlock(call->av->mutex);
            LOGGER_WARNING(call->av->m->log, "No callback to report loss on");
            return;
        }

        (*call->av->vbcb.first)(call->av, friend_number,
                                call->video_bit_rate - (call->video_bit_rate * loss),
                                call->av->vbcb.second);
    } else if (call->audio_bit_rate) {
        if (!call->av->abcb.first) {
            pthread_mutex_unlock(call->av->mutex);
            LOGGER_WARNING(call->av->m->log, "No callback to report loss on");
            return;
        }

        (*call->av->abcb.first)(call->av, friend_number,
                                call->audio_bit_rate - (call->audio_bit_rate * loss),
                                call->av->abcb.second);
    }

    pthread_mutex_unlock(call->av->mutex);
}
int callback_invite(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    ToxAVCall *av_call = call_new(toxav, call->friend_number, nullptr);

    if (av_call == nullptr) {
        LOGGER_WARNING(toxav->m->log, "Failed to initialize call...");
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    call->av_call = av_call;
    av_call->msi_call = call;

    if (toxav->ccb.first) {
        toxav->ccb.first(toxav, call->friend_number, call->peer_capabilities & msi_CapSAudio,
                         call->peer_capabilities & msi_CapSVideo, toxav->ccb.second);
    } else {
        /* No handler to capture the call request, send failure */
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}
int callback_start(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    ToxAVCall *av_call = call_get(toxav, call->friend_number);

    if (av_call == nullptr) {
        /* Should this ever happen? */
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    if (!call_prepare_transmission(av_call)) {
        callback_error(toxav_inst, call);
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    if (!invoke_call_state_callback(toxav, call->friend_number, call->peer_capabilities)) {
        callback_error(toxav_inst, call);
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}
int callback_end(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    invoke_call_state_callback(toxav, call->friend_number, TOXAV_FRIEND_CALL_STATE_FINISHED);

    if (call->av_call) {
        call_kill_transmission((ToxAVCall *)call->av_call);
        call_remove((ToxAVCall *)call->av_call);
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}
int callback_error(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    invoke_call_state_callback(toxav, call->friend_number, TOXAV_FRIEND_CALL_STATE_ERROR);

    if (call->av_call) {
        call_kill_transmission((ToxAVCall *)call->av_call);
        call_remove((ToxAVCall *)call->av_call);
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}
int callback_capabilites(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    if (call->peer_capabilities & msi_CapSAudio) {
        rtp_allow_receiving(((ToxAVCall *)call->av_call)->audio.first);
    } else {
        rtp_stop_receiving(((ToxAVCall *)call->av_call)->audio.first);
    }

    if (call->peer_capabilities & msi_CapSVideo) {
        rtp_allow_receiving(((ToxAVCall *)call->av_call)->video.first);
    } else {
        rtp_stop_receiving(((ToxAVCall *)call->av_call)->video.first);
    }

    invoke_call_state_callback(toxav, call->friend_number, call->peer_capabilities);

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}
bool audio_bit_rate_invalid(uint32_t bit_rate)
{
    /* Opus RFC 6716 section-2.1.1 dictates the following:
     * Opus supports all bit rates from 6 kbit/s to 510 kbit/s.
     */
    return bit_rate < 6 || bit_rate > 510;
}
bool video_bit_rate_invalid(uint32_t bit_rate)
{
    /* https://www.webmproject.org/docs/webm-sdk/structvpx__codec__enc__cfg.html shows the following:
     * unsigned int rc_target_bitrate
     * the range of uint varies from platform to platform
     * though, uint32_t should be large enough to store bitrates,
     * we may want to prevent from passing overflowed bitrates to libvpx
     * more in detail, it's the case where bit_rate is larger than uint, but smaller than uint32_t
     */
    return bit_rate > UINT_MAX;
}
bool invoke_call_state_callback(ToxAV *av, uint32_t friend_number, uint32_t state)
{
    if (av->scb.first) {
        av->scb.first(av, friend_number, state, av->scb.second);
    } else {
        return false;
    }

    return true;
}

ToxAVCall *call_new(ToxAV *av, uint32_t friend_number, TOXAV_ERR_CALL *error)
{
    /* Assumes mutex locked */
    TOXAV_ERR_CALL rc = TOXAV_ERR_CALL_OK;
    ToxAVCall *call = nullptr;

    if (m_friend_exists(av->m, friend_number) == 0) {
        rc = TOXAV_ERR_CALL_FRIEND_NOT_FOUND;
        goto END;
    }

    if (m_get_friend_connectionstatus(av->m, friend_number) < 1) {
        rc = TOXAV_ERR_CALL_FRIEND_NOT_CONNECTED;
        goto END;
    }

    if (call_get(av, friend_number) != nullptr) {
        rc = TOXAV_ERR_CALL_FRIEND_ALREADY_IN_CALL;
        goto END;
    }

    call = (ToxAVCall *)calloc(sizeof(ToxAVCall), 1);

    if (call == nullptr) {
        rc = TOXAV_ERR_CALL_MALLOC;
        goto END;
    }

    call->av = av;
    call->friend_number = friend_number;

    if (av->calls == nullptr) { /* Creating */
        av->calls = (ToxAVCall **)calloc(sizeof(ToxAVCall *), friend_number + 1);

        if (av->calls == nullptr) {
            free(call);
            call = nullptr;
            rc = TOXAV_ERR_CALL_MALLOC;
            goto END;
        }

        av->calls_tail = av->calls_head = friend_number;
    } else if (av->calls_tail < friend_number) { /* Appending */
        ToxAVCall **tmp = (ToxAVCall **)realloc(av->calls, sizeof(ToxAVCall *) * (friend_number + 1));

        if (tmp == nullptr) {
            free(call);
            call = nullptr;
            rc = TOXAV_ERR_CALL_MALLOC;
            goto END;
        }

        av->calls = tmp;

        /* Set fields in between to null */
        uint32_t i = av->calls_tail + 1;

        for (; i < friend_number; i ++) {
            av->calls[i] = nullptr;
        }

        call->prev = av->calls[av->calls_tail];
        av->calls[av->calls_tail]->next = call;

        av->calls_tail = friend_number;
    } else if (av->calls_head > friend_number) { /* Inserting at front */
        call->next = av->calls[av->calls_head];
        av->calls[av->calls_head]->prev = call;
        av->calls_head = friend_number;
    }

    av->calls[friend_number] = call;

END:

    if (error) {
        *error = rc;
    }

    return call;
}

ToxAVCall *call_get(ToxAV *av, uint32_t friend_number)
{
    /* Assumes mutex locked */
    if (av->calls == nullptr || av->calls_tail < friend_number) {
        return nullptr;
    }

    return av->calls[friend_number];
}

ToxAVCall *call_remove(ToxAVCall *call)
{
    if (call == nullptr) {
        return nullptr;
    }

    uint32_t friend_number = call->friend_number;
    ToxAV *av = call->av;

    ToxAVCall *prev = call->prev;
    ToxAVCall *next = call->next;

    /* Set av call in msi to NULL in order to know if call if ToxAVCall is
     * removed from the msi call.
     */
    if (call->msi_call) {
        call->msi_call->av_call = nullptr;
    }

    free(call);

    if (prev) {
        prev->next = next;
    } else if (next) {
        av->calls_head = next->friend_number;
    } else {
        goto CLEAR;
    }

    if (next) {
        next->prev = prev;
    } else if (prev) {
        av->calls_tail = prev->friend_number;
    } else {
        goto CLEAR;
    }

    av->calls[friend_number] = nullptr;
    return next;

CLEAR:
    av->calls_head = av->calls_tail = 0;
    free(av->calls);
    av->calls = nullptr;

    return nullptr;
}

bool call_prepare_transmission(ToxAVCall *call)
{
    /* Assumes mutex locked */

    if (call == nullptr) {
        return false;
    }

    ToxAV *av = call->av;

    if (!av->acb.first && !av->vcb.first) {
        /* It makes no sense to have CSession without callbacks */
        return false;
    }

    if (call->active) {
        LOGGER_WARNING(av->m->log, "Call already active!\n");
        return true;
    }

    if (create_recursive_mutex(call->mutex_audio) != 0) {
        return false;
    }

    if (create_recursive_mutex(call->mutex_video) != 0) {
        goto FAILURE_3;
    }

    if (create_recursive_mutex(call->mutex) != 0) {
        goto FAILURE_2;
    }

    /* Prepare bwc */
    call->bwc = bwc_new(av->m, call->friend_number, callback_bwc, call);

    { /* Prepare audio */
        call->audio.second = ac_new(av->m->log, av, call->friend_number, av->acb.first, av->acb.second);

        if (!call->audio.second) {
            LOGGER_ERROR(av->m->log, "Failed to create audio codec session");
            goto FAILURE;
        }

        call->audio.first = rtp_new(rtp_TypeAudio, av->m, call->friend_number, call->bwc,
                                    call->audio.second, ac_queue_message);

        if (!call->audio.first) {
            LOGGER_ERROR(av->m->log, "Failed to create audio rtp session");;
            goto FAILURE;
        }
    }
    { /* Prepare video */
        call->video.second = vc_new(av->m->log, av, call->friend_number, av->vcb.first, av->vcb.second);

        if (!call->video.second) {
            LOGGER_ERROR(av->m->log, "Failed to create video codec session");
            goto FAILURE;
        }

        call->video.first = rtp_new(rtp_TypeVideo, av->m, call->friend_number, call->bwc,
                                    call->video.second, vc_queue_message);

        if (!call->video.first) {
            LOGGER_ERROR(av->m->log, "Failed to create video rtp session");
            goto FAILURE;
        }
    }

    call->active = 1;
    return true;

FAILURE:
    bwc_kill(call->bwc);
    rtp_kill(call->audio.first);
    ac_kill(call->audio.second);
    call->audio.first = nullptr;
    call->audio.second = nullptr;
    rtp_kill(call->video.first);
    vc_kill(call->video.second);
    call->video.first = nullptr;
    call->video.second = nullptr;
    pthread_mutex_destroy(call->mutex);
FAILURE_2:
    pthread_mutex_destroy(call->mutex_video);
FAILURE_3:
    pthread_mutex_destroy(call->mutex_audio);
    return false;
}

void call_kill_transmission(ToxAVCall *call)
{
    if (call == nullptr || call->active == 0) {
        return;
    }

    call->active = 0;

    pthread_mutex_lock(call->mutex_audio);
    pthread_mutex_unlock(call->mutex_audio);
    pthread_mutex_lock(call->mutex_video);
    pthread_mutex_unlock(call->mutex_video);
    pthread_mutex_lock(call->mutex);
    pthread_mutex_unlock(call->mutex);

    bwc_kill(call->bwc);

    rtp_kill(call->audio.first);
    ac_kill(call->audio.second);
    call->audio.first = nullptr;
    call->audio.second = nullptr;

    rtp_kill(call->video.first);
    vc_kill(call->video.second);
    call->video.first = nullptr;
    call->video.second = nullptr;

    pthread_mutex_destroy(call->mutex_audio);
    pthread_mutex_destroy(call->mutex_video);
    pthread_mutex_destroy(call->mutex);
}
