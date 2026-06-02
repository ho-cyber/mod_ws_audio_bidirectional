/*
 * mod_ws_audio.c  –  Bidirectional WebSocket audio bridge for FreeSWITCH
 *
 * Dialplan usage:
 *   <action application="ws_audio" data="ws://your-media-server:3000/ws?callSid=${uuid}"/>
 *
 * What it does:
 *   1. Attaches a media bug to the call leg (READ_REPLACE frame tap).
 *   2. Converts inbound RTP μ-law → linear16 PCM and sends as raw binary
 *      frames over the WebSocket.
 *   3. Listens for raw binary frames coming back from the media server
 *      (linear16 PCM, 8 kHz, mono) and writes them to the write-replace
 *      buffer so FreeSWITCH plays them to the caller.
 *
 * Dependencies:  libwebsockets >= 3.x   (apt install libwebsockets-dev)
 *                FreeSWITCH dev headers  (apt install freeswitch-dev)
 *
 * Build:
 *   See Makefile in the project root.
 */

#include <switch.h>
#include <libwebsockets.h>
#include <string.h>
#include <stdint.h>

#define MOD_WS_AUDIO_VERSION "1.0.0"
#define WS_WRITE_HEADROOM    LWS_PRE          /* libwebsockets requires this many bytes before payload */
#define OUTBOUND_QUEUE_MAX   200              /* frames sitting in the play-queue before we drop */
#define PCM_FRAME_BYTES      320              /* 20 ms @ 8 kHz 16-bit mono = 160 samples * 2 bytes */
#define MULAW_FRAME_BYTES    160              /* 20 ms @ 8 kHz 8-bit μ-law */

SWITCH_MODULE_LOAD_FUNCTION(mod_ws_audio_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ws_audio_shutdown);
SWITCH_MODULE_DEFINITION(mod_ws_audio, mod_ws_audio_load, mod_ws_audio_shutdown, NULL);

/* ───────────────────────────── μ-law codec helpers ───────────────────────── */

static int16_t mulaw_decode_table[256];

static void build_mulaw_decode_table(void)
{
    for (int i = 0; i < 256; i++) {
        uint8_t u = (uint8_t)(~i) & 0xFF;
        int sign     = (u & 0x80) ? -1 : 1;
        int exponent = (u >> 4) & 0x07;
        int mantissa =  u       & 0x0F;
        int magnitude = ((mantissa << 1) | 0x21) << exponent;
        mulaw_decode_table[i] = (int16_t)(sign * (magnitude - 33));
    }
}

/* Encode one linear-16 sample to μ-law (ITU-T G.711) */
static uint8_t linear16_to_mulaw(int16_t pcm)
{
    const int BIAS   = 33;
    const int CLIP   = 32767;
    int       sign   = (pcm >> 8) & 0x80;
    int       sample = (int)pcm;

    if (sign) sample = -sample;
    if (sample > CLIP) sample = CLIP;
    sample += BIAS;

    int exp = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exp > 0; exp--, mask >>= 1) {}

    int mantissa = (sample >> (exp + 3)) & 0x0F;
    return (uint8_t)(~(sign | (exp << 4) | mantissa) & 0xFF);
}

/* ───────────────────────────── per-session state ──────────────────────────── */

typedef struct {
    /* Outbound queue: PCM frames coming FROM the media server TO the caller */
    switch_queue_t  *out_queue;          /* thread-safe queue of switch_buffer_t * */

    /* WebSocket internals */
    struct lws_context *lws_ctx;
    struct lws         *wsi;
    int                 ws_connected;    /* 1 once handshake completes */
    int                 ws_closing;      /* 1 when we want teardown */

    /* Pending send buffer (filled by session thread, drained by lws service) */
    switch_mutex_t     *send_mutex;
    uint8_t            *send_buf;        /* LWS_PRE + payload */
    size_t              send_len;        /* bytes waiting to send (0 = nothing) */

    /* FreeSWITCH objects */
    switch_core_session_t *session;
    switch_media_bug_t    *bug;
    switch_memory_pool_t  *pool;

    char  ws_url[512];
    char  call_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
} ws_audio_session_t;

/* ───────────────────────────── lws callbacks ──────────────────────────────── */

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    /* Retrieve our session from the lws user-data pointer set at creation */
    ws_audio_session_t *ws_sess = (ws_audio_session_t *)lws_wsi_user(wsi);
    if (!ws_sess) return 0;

    switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        ws_sess->ws_connected = 1;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
            "[mod_ws_audio] [%s] WebSocket connected to %s\n",
            ws_sess->call_uuid, ws_sess->ws_url);
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "[mod_ws_audio] [%s] Connection error: %s\n",
            ws_sess->call_uuid, in ? (char *)in : "(unknown)");
        ws_sess->ws_connected = 0;
        ws_sess->ws_closing   = 1;
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        ws_sess->ws_connected = 0;
        ws_sess->ws_closing   = 1;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
            "[mod_ws_audio] [%s] WebSocket closed\n", ws_sess->call_uuid);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        /*
         * Inbound binary frame from media server = PCM to play to caller.
         * Enqueue it; the media bug callback will drain it.
         */
        if (lws_frame_is_binary(wsi) && len > 0) {
            void *frame = switch_core_alloc(ws_sess->pool, len);
            memcpy(frame, in, len);

            /* Wrap in a switch_buffer for size bookkeeping */
            switch_buffer_t *sbuf = NULL;
            switch_buffer_create_dynamic(&sbuf, len, len, len);
            switch_buffer_write(sbuf, in, len);

            if (switch_queue_trypush(ws_sess->out_queue, sbuf) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "[mod_ws_audio] [%s] Out-queue full, dropping frame\n",
                    ws_sess->call_uuid);
                switch_buffer_destroy(&sbuf);
            }
        }
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        /* Drain one pending send */
        switch_mutex_lock(ws_sess->send_mutex);
        if (ws_sess->send_len > 0) {
            lws_write(wsi,
                      ws_sess->send_buf + LWS_PRE,
                      ws_sess->send_len,
                      LWS_WRITE_BINARY);
            ws_sess->send_len = 0;
        }
        switch_mutex_unlock(ws_sess->send_mutex);
        break;

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols ws_protocols[] = {
    { "ws-audio", ws_callback, 0, 0 },
    { NULL, NULL, 0, 0 }
};

/* ───────────────────────────── send helper ────────────────────────────────── */

/*
 * Queue a binary payload to be sent on the next LWS_CALLBACK_CLIENT_WRITEABLE.
 * Called from the media-bug thread; must not block.
 */
static void ws_send_audio(ws_audio_session_t *ws_sess, const uint8_t *pcm, size_t bytes)
{
    if (!ws_sess->ws_connected || ws_sess->ws_closing) return;

    switch_mutex_lock(ws_sess->send_mutex);
    if (ws_sess->send_len == 0) {   /* previous send was drained */
        memcpy(ws_sess->send_buf + LWS_PRE, pcm, bytes);
        ws_sess->send_len = bytes;
        lws_callback_on_writable(ws_sess->wsi);
    }
    /* else: drop; the service loop will catch up next frame */
    switch_mutex_unlock(ws_sess->send_mutex);
}

/* ───────────────────────────── media bug callback ─────────────────────────── */

static switch_bool_t audio_bug_callback(switch_media_bug_t *bug,
                                        void *user_data,
                                        switch_abc_type_t type)
{
    ws_audio_session_t *ws_sess = (ws_audio_session_t *)user_data;

    switch (type) {

    case SWITCH_ABC_TYPE_INIT:
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
            "[mod_ws_audio] [%s] Media bug initialised\n", ws_sess->call_uuid);
        break;

    case SWITCH_ABC_TYPE_CLOSE:
        ws_sess->ws_closing = 1;
        break;

    case SWITCH_ABC_TYPE_READ_REPLACE: {
        /*
         * READ_REPLACE: we get the frame coming FROM the caller (their voice).
         * Convert μ-law → PCM and ship it to the media server over the WebSocket.
         */
        switch_frame_t *frame = switch_core_media_bug_get_read_replace_frame(bug);
        if (!frame || !frame->data || frame->datalen == 0) break;

        /* Frame is already linear16 because FreeSWITCH normalises before the bug */
        ws_send_audio(ws_sess, (const uint8_t *)frame->data, frame->datalen);

        /* Pump the lws service loop (non-blocking, 0ms timeout) */
        lws_service(ws_sess->lws_ctx, 0);
        break;
    }

    case SWITCH_ABC_TYPE_WRITE_REPLACE: {
        /*
         * WRITE_REPLACE: we can inject audio TO the caller here.
         * Dequeue one PCM frame from out_queue and swap it in.
         */
        switch_frame_t *frame = switch_core_media_bug_get_write_replace_frame(bug);
        if (!frame) break;

        void *qitem = NULL;
        if (switch_queue_trypop(ws_sess->out_queue, &qitem) == SWITCH_STATUS_SUCCESS && qitem) {
            switch_buffer_t *sbuf = (switch_buffer_t *)qitem;
            switch_size_t   avail = switch_buffer_inuse(sbuf);
            switch_size_t   need  = frame->datalen;

            if (avail >= need) {
                switch_buffer_read(sbuf, frame->data, need);
                /* If there are leftover bytes, push back to front of queue
                   by re-enqueuing the partially-drained buffer */
                if (switch_buffer_inuse(sbuf) > 0) {
                    switch_queue_trypush(ws_sess->out_queue, sbuf);
                } else {
                    switch_buffer_destroy(&sbuf);
                }
            } else {
                /* Frame from server was smaller than FreeSWITCH's frame;
                   copy what we have, zero-pad the rest (comfort noise) */
                switch_buffer_read(sbuf, frame->data, avail);
                memset((uint8_t *)frame->data + avail, 0, need - avail);
                switch_buffer_destroy(&sbuf);
            }

            switch_core_media_bug_set_write_replace_frame(bug, frame);
        }
        /* No data → leave frame untouched (silence / previous audio) */
        break;
    }

    default:
        break;
    }

    return SWITCH_TRUE;
}

/* ───────────────────────────── application entry point ────────────────────── */

SWITCH_STANDARD_APP(ws_audio_function)
{
    switch_channel_t          *channel  = switch_core_session_get_channel(session);
    switch_memory_pool_t      *pool     = switch_core_session_get_pool(session);
    ws_audio_session_t        *ws_sess  = NULL;
    struct lws_context_creation_info info;
    struct lws_client_connect_info    conn_info;

    if (zstr(data)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "[mod_ws_audio] No WebSocket URL provided\n");
        return;
    }

    /* ── Allocate session state ── */
    ws_sess = switch_core_session_alloc(session, sizeof(ws_audio_session_t));
    memset(ws_sess, 0, sizeof(ws_audio_session_t));
    ws_sess->session = session;
    ws_sess->pool    = pool;
    strncpy(ws_sess->ws_url,       data,                               sizeof(ws_sess->ws_url) - 1);
    strncpy(ws_sess->call_uuid,    switch_core_session_get_uuid(session), SWITCH_UUID_FORMATTED_LENGTH);

    switch_queue_create(&ws_sess->out_queue, OUTBOUND_QUEUE_MAX, pool);
    switch_mutex_init(&ws_sess->send_mutex, SWITCH_MUTEX_NESTED, pool);

    /* LWS_PRE headroom + max PCM frame */
    ws_sess->send_buf = switch_core_alloc(pool, LWS_PRE + PCM_FRAME_BYTES * 4);

    /* ── Parse URL ── */
    /*   Expected forms: ws://host:port/path  or  wss://host:port/path  */
    char url_copy[512];
    strncpy(url_copy, data, sizeof(url_copy) - 1);

    int   use_ssl = 0;
    char *host_start = url_copy;
    if (strncmp(url_copy, "wss://", 6) == 0) {
        use_ssl    = 1;
        host_start = url_copy + 6;
    } else if (strncmp(url_copy, "ws://", 5) == 0) {
        host_start = url_copy + 5;
    }

    char host[256] = {0};
    int  port      = 80;
    char path[256] = "/";

    char *slash = strchr(host_start, '/');
    if (slash) {
        strncpy(path, slash, sizeof(path) - 1);
        *slash = '\0';
    }

    char *colon = strchr(host_start, ':');
    if (colon) {
        port = atoi(colon + 1);
        *colon = '\0';
    }
    strncpy(host, host_start, sizeof(host) - 1);

    /* ── Create lws context ── */
    memset(&info, 0, sizeof(info));
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = ws_protocols;
    info.options  |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    ws_sess->lws_ctx = lws_create_context(&info);
    if (!ws_sess->lws_ctx) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "[mod_ws_audio] [%s] Failed to create lws context\n", ws_sess->call_uuid);
        return;
    }

    /* ── Connect ── */
    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.context        = ws_sess->lws_ctx;
    conn_info.address        = host;
    conn_info.port           = port;
    conn_info.path           = path;
    conn_info.host           = host;
    conn_info.origin         = host;
    conn_info.protocol       = ws_protocols[0].name;
    conn_info.userdata        = ws_sess;
    conn_info.ssl_connection = use_ssl ? LCCSCF_USE_SSL : 0;

    ws_sess->wsi = lws_client_connect_via_info(&conn_info);
    if (!ws_sess->wsi) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "[mod_ws_audio] [%s] lws_client_connect_via_info failed\n", ws_sess->call_uuid);
        lws_context_destroy(ws_sess->lws_ctx);
        return;
    }

    /* Wait up to 3s for connection */
    for (int i = 0; i < 150 && !ws_sess->ws_connected && !ws_sess->ws_closing; i++) {
        lws_service(ws_sess->lws_ctx, 20);
    }

    if (!ws_sess->ws_connected) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "[mod_ws_audio] [%s] Timed out waiting for WS connection\n", ws_sess->call_uuid);
        lws_context_destroy(ws_sess->lws_ctx);
        return;
    }

    /* ── Attach media bug: READ_REPLACE + WRITE_REPLACE ── */
    switch_status_t status = switch_core_media_bug_add(
        session,
        "ws_audio",
        data,
        audio_bug_callback,
        ws_sess,
        0,
        SMBF_READ_REPLACE | SMBF_WRITE_REPLACE | SMBF_NO_PAUSE,
        &ws_sess->bug
    );

    if (status != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "[mod_ws_audio] [%s] Failed to attach media bug\n", ws_sess->call_uuid);
        ws_sess->ws_closing = 1;
    }

    /* ── Block until the channel hangs up or the WS closes ── */
    while (switch_channel_ready(channel) && !ws_sess->ws_closing) {
        lws_service(ws_sess->lws_ctx, 20);
        switch_yield(1000);   /* 1 ms yield to avoid spinning */
    }

    /* ── Teardown ── */
    if (ws_sess->bug) {
        switch_core_media_bug_remove(session, &ws_sess->bug);
        ws_sess->bug = NULL;
    }

    lws_context_destroy(ws_sess->lws_ctx);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "[mod_ws_audio] [%s] Session ended, resources released\n", ws_sess->call_uuid);
}

/* ───────────────────────────── module lifecycle ───────────────────────────── */

SWITCH_MODULE_LOAD_FUNCTION(mod_ws_audio_load)
{
    switch_application_interface_t *app_interface;

    build_mulaw_decode_table();

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    SWITCH_ADD_APP(app_interface,
        "ws_audio",
        "Bidirectional WebSocket audio bridge",
        "Stream call audio to/from a WebSocket media server",
        ws_audio_function,
        "<ws://host:port/path>",
        SAF_MEDIA_TAP
    );

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "[mod_ws_audio] Loaded version %s\n", MOD_WS_AUDIO_VERSION);

    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ws_audio_shutdown)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "[mod_ws_audio] Shutting down\n");
    return SWITCH_STATUS_SUCCESS;
}