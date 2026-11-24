/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_recv_ex.c - Receiver PTZ, recording, and web control extensions
 */

#include "avx_internal.h"
#include <stdio.h>
#include <string.h>

/* Formats and transmits upstream XML metadata control frames over the receiver connection. */
static bool send_recv_metadata_xml(avx_rx_instance_t p_instance, const char* xml) {
    if (!p_instance || !xml) return false;
    oavx_metadata_frame_t meta = {
        .length = strlen(xml),
        .pts_ns = 0,
        .data = (char*)xml
    };
    return avx_rx_send_metadata(p_instance, &meta);
}

static inline const char* get_ptz_tag(avx_rx_instance_t p_instance) {
    if (!p_instance) return "oavx_ptz";
    avx_recv_ctx_t* slot = (avx_recv_ctx_t*)p_instance;
    return slot->wire_profile.ptz_tag ? slot->wire_profile.ptz_tag : "oavx_ptz";
}

static inline const char* get_record_tag(avx_rx_instance_t p_instance) {
    if (!p_instance) return "oavx_record";
    avx_recv_ctx_t* slot = (avx_recv_ctx_t*)p_instance;
    return slot->wire_profile.record_tag ? slot->wire_profile.record_tag : "oavx_record";
}

/* ------------------------------------------------------------------------- */
/* 1. PTZ Controls                                                           */
/* ------------------------------------------------------------------------- */

/* Dispatches PTZ focal zoom commands to remote sender via XML metadata frames. */
bool avx_rx_ptz_zoom(avx_rx_instance_t p_instance, float zoom_val) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s zoom=\"%.4f\"/>", get_ptz_tag(p_instance), (double)zoom_val);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_zoom_speed(avx_rx_instance_t p_instance, float zoom_speed) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s zoom_speed=\"%.4f\"/>", get_ptz_tag(p_instance), (double)zoom_speed);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_pan_tilt(avx_rx_instance_t p_instance, float pan_val, float tilt_val) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s pan=\"%.4f\" tilt=\"%.4f\"/>", get_ptz_tag(p_instance), (double)pan_val, (double)tilt_val);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_pan_tilt_speed(avx_rx_instance_t p_instance, float pan_speed, float tilt_speed) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s pan_speed=\"%.4f\" tilt_speed=\"%.4f\"/>", get_ptz_tag(p_instance), (double)pan_speed, (double)tilt_speed);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_store_preset(avx_rx_instance_t p_instance, int preset_no) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s store_preset=\"%d\"/>", get_ptz_tag(p_instance), preset_no);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_recall_preset(avx_rx_instance_t p_instance, int preset_no, float speed) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s recall_preset=\"%d\" speed=\"%.4f\"/>", get_ptz_tag(p_instance), preset_no, (double)speed);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_auto_focus(avx_rx_instance_t p_instance) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s auto_focus=\"true\"/>", get_ptz_tag(p_instance));
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_focus(avx_rx_instance_t p_instance, float focus_val) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s focus=\"%.4f\"/>", get_ptz_tag(p_instance), (double)focus_val);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_focus_speed(avx_rx_instance_t p_instance, float focus_speed) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s focus_speed=\"%.4f\"/>", get_ptz_tag(p_instance), (double)focus_speed);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_white_balance_auto(avx_rx_instance_t p_instance) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s white_balance=\"auto\"/>", get_ptz_tag(p_instance));
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_white_balance_indoor(avx_rx_instance_t p_instance) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s white_balance=\"indoor\"/>", get_ptz_tag(p_instance));
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_white_balance_outdoor(avx_rx_instance_t p_instance) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s white_balance=\"outdoor\"/>", get_ptz_tag(p_instance));
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_white_balance_oneshot(avx_rx_instance_t p_instance) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s white_balance=\"oneshot\"/>", get_ptz_tag(p_instance));
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_white_balance_manual(avx_rx_instance_t p_instance, float red, float blue) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s white_balance=\"manual\" red=\"%.4f\" blue=\"%.4f\"/>", get_ptz_tag(p_instance), (double)red, (double)blue);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_exposure_auto(avx_rx_instance_t p_instance) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s exposure=\"auto\"/>", get_ptz_tag(p_instance));
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_ptz_exposure_manual(avx_rx_instance_t p_instance, float iris, float gain, float shutter) {
    char xml[128];
    snprintf(xml, sizeof(xml), "<%s exposure=\"manual\" iris=\"%.4f\" gain=\"%.4f\" shutter=\"%.4f\"/>",
             get_ptz_tag(p_instance), (double)iris, (double)gain, (double)shutter);
    return send_recv_metadata_xml(p_instance, xml);
}

/* ------------------------------------------------------------------------- */
/* 2. Remote Recording Controls                                              */
/* ------------------------------------------------------------------------- */

/* Sends upstream remote recording trigger metadata with optional filename hint. */
bool avx_rx_recording_start(avx_rx_instance_t p_instance, const char* p_filename_hint) {
    if (!p_instance) return false;
    avx_recv_ctx_t* slot = (avx_recv_ctx_t*)p_instance;
    const char* tag = get_record_tag(p_instance);

    char xml[512];
    if (p_filename_hint && p_filename_hint[0] != '\0') {
        snprintf(xml, sizeof(xml), "<%s action=\"start\" filename=\"%s\"/>", tag, p_filename_hint);
        strncpy(slot->current_recording_filename, p_filename_hint, sizeof(slot->current_recording_filename) - 1);
        slot->current_recording_filename[sizeof(slot->current_recording_filename) - 1] = '\0';
    } else {
        snprintf(xml, sizeof(xml), "<%s action=\"start\"/>", tag);
    }
    slot->is_recording = true;
    slot->current_recording_error[0] = '\0';

    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_recording_stop(avx_rx_instance_t p_instance) {
    if (!p_instance) return false;
    avx_recv_ctx_t* slot = (avx_recv_ctx_t*)p_instance;
    const char* tag = get_record_tag(p_instance);

    char xml[128];
    snprintf(xml, sizeof(xml), "<%s action=\"stop\"/>", tag);
    slot->is_recording = false;
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_recording_set_audio_level(avx_rx_instance_t p_instance, float level_db) {
    char xml[128];
    const char* tag = get_record_tag(p_instance);
    snprintf(xml, sizeof(xml), "<%s audio_level=\"%.2f\"/>", tag, (double)level_db);
    return send_recv_metadata_xml(p_instance, xml);
}

bool avx_rx_recording_is_recording(avx_rx_instance_t p_instance) {
    if (!p_instance) return false;
    avx_recv_ctx_t* slot = (avx_recv_ctx_t*)p_instance;
    return slot->is_recording;
}

const char* avx_rx_recording_get_filename(avx_rx_instance_t p_instance) {
    if (!p_instance) return NULL;
    avx_recv_ctx_t* slot = (avx_recv_ctx_t*)p_instance;
    return slot->current_recording_filename[0] ? slot->current_recording_filename : NULL;
}

const char* avx_rx_recording_get_error(avx_rx_instance_t p_instance) {
    if (!p_instance) return NULL;
    avx_recv_ctx_t* slot = (avx_recv_ctx_t*)p_instance;
    return slot->current_recording_error[0] ? slot->current_recording_error : NULL;
}

/* ------------------------------------------------------------------------- */
/* 3. Device Web Configuration Interface                                     */
/* ------------------------------------------------------------------------- */

/* Returns HTTP URL of the remote sender's embedded web configuration interface. */
const char* avx_rx_get_web_control(avx_rx_instance_t p_instance) {
    if (!p_instance) return NULL;
    avx_recv_ctx_t* slot = (avx_recv_ctx_t*)p_instance;
    return slot->web_control_url[0] ? slot->web_control_url : NULL;
}
