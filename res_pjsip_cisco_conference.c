/* Copyright (C) 2024 Timon Schneider info@timon-schneider.com
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * res_pjsip_cisco_conference.c
 *
 * Intercepts Cisco x-cisco-remotecc Conference REFER requests and creates
 * a ConfBridge conference instead of returning "501 Not Implemented".
 *
 * Cisco CP-8xxx phones send a REFER with
 *   Content-Type: application/x-cisco-remotecc-request+xml
 * when the Conference hardware key is pressed. This module accepts it,
 * finds the two existing call legs by their PJSIP dialog Call-IDs, and
 * redirects all three audio endpoints into one ConfBridge room.
 *
 * Add to /etc/asterisk/extensions_custom.conf:
 *   [cisco-conference]
 *   exten => s,1,NoOp(Cisco Conference Room: ${CISCO_CONF_ROOM})
 *    same => n,Set(CONFBRIDGE(user,announce_only_user)=no)
 *    same => n,ConfBridge(${CISCO_CONF_ROOM},,,)
 *    same => n,Hangup()
 */

/* Required for externally-compiled Asterisk modules */
#define AST_MODULE_SELF_SYM __local_ast_module_self

/*** MODULEINFO
    <depend>pjproject</depend>
    <depend>res_pjsip</depend>
    <depend>res_pjsip_session</depend>
    <support_level>extended</support_level>
 ***/

#include "asterisk.h"
#include "asterisk/module.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/channel.h"
#include "asterisk/bridge.h"
#include "asterisk/pbx.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/logger.h"
#include "asterisk/astobj2.h"
#include "asterisk/time.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pthread.h>

#define CISCO_CONF_CONTEXT "cisco-conference"
#define CISCO_CONF_EXTEN   "s"
#define CISCO_CONF_PRIO    1

/* Data passed to the conference worker thread */
struct cc_task {
    char held_name[AST_CHANNEL_NAME];    /* PJSIP/200-xxx  held party          */
    char phone_name[AST_CHANNEL_NAME];   /* PJSIP/220-yyy  phone (2nd leg)     */
    char consult_name[AST_CHANNEL_NAME]; /* PJSIP/210-zzz  consult party       */
    char drop_name[AST_CHANNEL_NAME];    /* PJSIP/220-aaa  1st outgoing to 220 */
    char room[64];
};

/* ---------- helpers ---------------------------------------------------- */

static int xml_get(const char *xml, const char *tag, char *out, size_t sz)
{
    char open[256], close[256];
    const char *p, *q;
    size_t len;

    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    p = strstr(xml, open);
    if (!p) return -1;
    p += strlen(open);
    q = strstr(p, close);
    if (!q) return -1;

    len = (size_t)(q - p);
    if (len >= sz) return -1;
    memcpy(out, p, len);
    out[len] = '\0';

    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\t'
                       || out[len-1] == '\r' || out[len-1] == '\n'))
        out[--len] = '\0';

    return 0;
}

/*
 * Find the Asterisk channel that owns a specific PJSIP dialog.
 *
 * phone_tag    = XML <localtag>  = phone's own tag  = Asterisk's remote-tag
 * asterisk_tag = XML <remotetag> = Asterisk's tag   = Asterisk's local-tag
 *
 * Call only from a PJSIP-thread context (e.g., on_rx_request callback).
 * Returns a referenced channel; caller must ast_channel_unref() it.
 */
static struct ast_channel *channel_for_dialog(const char *call_id,
    const char *phone_tag, const char *asterisk_tag)
{
    pj_str_t cid  = { (char *)call_id,     (pj_ssize_t)strlen(call_id) };
    pj_str_t ltag = { (char *)asterisk_tag, (pj_ssize_t)strlen(asterisk_tag) };
    pj_str_t rtag = { (char *)phone_tag,    (pj_ssize_t)strlen(phone_tag) };
    pjsip_dialog *dlg;
    struct ast_sip_session *session;
    struct ast_channel *chan = NULL;

    /* local_tag = Asterisk's tag, remote_tag = phone's tag */
    dlg = pjsip_ua_find_dialog(&cid, &ltag, &rtag, PJ_TRUE);
    if (!dlg)   /* try swapped in case perspective differs */
        dlg = pjsip_ua_find_dialog(&cid, &rtag, &ltag, PJ_TRUE);
    if (!dlg) {
        ast_log(LOG_WARNING, "CiscoConf: dialog not found for call-id='%s'\n", call_id);
        return NULL;
    }

    session = ast_sip_dialog_get_session(dlg);
    pjsip_dlg_dec_lock(dlg);   /* release lock acquired by find_dialog */

    if (!session) {
        ast_log(LOG_WARNING, "CiscoConf: no session for call-id='%s'\n", call_id);
        return NULL;
    }

    if (session->channel)
        chan = ast_channel_ref(session->channel);
    ao2_ref(session, -1);

    if (!chan)
        ast_log(LOG_WARNING, "CiscoConf: no channel in session for call-id='%s'\n", call_id);

    return chan;
}

/* ---------- conference worker thread ----------------------------------- */

static void *cc_conference_thread(void *data)
{
    struct cc_task *t = data;
    struct ast_channel *ch_held, *ch_phone, *ch_consult, *ch_drop;

    /* Let the 202 response leave the wire before touching bridges */
    usleep(300000);

    ch_held = t->held_name[0] ? ast_channel_get_by_name(t->held_name) : NULL;
    ch_phone   = ast_channel_get_by_name(t->phone_name);
    ch_consult = t->consult_name[0] ? ast_channel_get_by_name(t->consult_name) : NULL;
    ch_drop    = t->drop_name[0] ? ast_channel_get_by_name(t->drop_name) : NULL;

    if (!ch_phone || (!ch_held && !ch_consult)) {
        ast_log(LOG_ERROR, "CiscoConf: channel(s) gone before conference could start\n");
        goto done;
    }

    ast_log(LOG_NOTICE,
        "CiscoConf: room='%s' held=%s phone=%s consult=%s\n",
        t->room, t->held_name[0] ? t->held_name : "(none)", t->phone_name, t->consult_name[0] ? t->consult_name : "(none)");

    if (ch_held) pbx_builtin_setvar_helper(ch_held, "CISCO_CONF_ROOM", t->room);
    pbx_builtin_setvar_helper(ch_phone,   "CISCO_CONF_ROOM", t->room);
    if (ch_consult) pbx_builtin_setvar_helper(ch_consult, "CISCO_CONF_ROOM", t->room);

    /*
     * Redirect channels to ConfBridge.
     *
     * ch_phone (220 A-leg) and ch_consult (210 B-leg) are coupled by Dial().
     * Whichever is redirected first causes Dial() to softhangup the other —
     * UNLESS both redirect flags are set before either thread wakes up and
     * runs Dial()'s cleanup code.
     *
     * Strategy: set all three redirects with NO sleep between them so that
     * every channel has AST_FLAG_ASYNC_GOTO set before any thread runs.
     * Order: ch_consult first (210 B-leg), ch_phone second (220 A-leg).
     * When ch_consult leaves the bridge on its own redirect, Dial() on
     * ch_phone sees "callee gone" but also finds its own async_goto flag set
     * and follows to cisco-conference rather than running the hangup handler.
     */
    if (ch_consult) ast_async_goto(ch_consult, CISCO_CONF_CONTEXT, CISCO_CONF_EXTEN, CISCO_CONF_PRIO);
    ast_async_goto(ch_phone,   CISCO_CONF_CONTEXT, CISCO_CONF_EXTEN, CISCO_CONF_PRIO);
    if (ch_held) ast_async_goto(ch_held, CISCO_CONF_CONTEXT, CISCO_CONF_EXTEN, CISCO_CONF_PRIO);

    /*
     * ch_drop is the outgoing leg toward phone 220 for the first (held) call.
     * It becomes orphaned once ch_held leaves the simple_bridge.  Give the
     * three async_gotos above a moment to be processed, then force it down.
     */
    if (ch_drop) {
        usleep(300000);
        ast_softhangup(ch_drop, AST_SOFTHANGUP_DEV);
    }

done:
    if (ch_held)    ast_channel_unref(ch_held);
    if (ch_phone)   ast_channel_unref(ch_phone);
    if (ch_consult) ast_channel_unref(ch_consult);
    if (ch_drop)    ast_channel_unref(ch_drop);
    ast_free(t);
    return NULL;
}

/* ---------- PJSIP module callback -------------------------------------- */

static pj_bool_t cisco_cc_on_rx_request(pjsip_rx_data *rdata)
{
    pjsip_msg *msg = rdata->msg_info.msg;
    pjsip_tx_data *resp;
    char body[8192], event[64], section[2048];
    char dlg_callid[256], dlg_ltag[128], dlg_rtag[128];
    char con_callid[256], con_ltag[128], con_rtag[128];
    const char *p, *q;
    size_t slen;
    int blen;
    struct ast_channel *ch_first, *ch_held, *ch_second, *ch_consult;
    struct cc_task *task;
    pthread_attr_t attr;
    pthread_t thr;

    /* ---- filter: REFER only ----------------------------------------- */
    if (pjsip_method_cmp(&msg->line.req.method, &pjsip_refer_method) != 0)
        return PJ_FALSE;

    /* ---- filter: Content-Type application/x-cisco-remotecc-request+xml
     * Read directly from msg->body->content_type (pjsip_media_type) to
     * avoid struct name differences across pjproject versions.         */
    if (!msg->body || !msg->body->data || !msg->body->len)
        return PJ_FALSE;

    if (pj_stricmp2(&msg->body->content_type.type,    "application") != 0 ||
        pj_stricmp2(&msg->body->content_type.subtype, "x-cisco-remotecc-request+xml") != 0)
        return PJ_FALSE;

    blen = (int)msg->body->len < (int)(sizeof(body) - 1)
           ? (int)msg->body->len : (int)(sizeof(body) - 1);
    memcpy(body, msg->body->data, blen);
    body[blen] = '\0';

    /* ---- get event type -------------------------------------------- */
    if (xml_get(body, "softkeyevent", event, sizeof(event)) != 0)
        event[0] = '\0';

    /*
     * Non-Conference events (e.g. the follow-up "Cancel" the phone sends
     * after getting 501): accept silently so the phone stops retrying.
     */
    if (strcasecmp(event, "Conference") != 0) {
        ast_log(LOG_DEBUG,
            "CiscoConf: accepting non-Conference remotecc REFER (event='%s')\n", event);
        if (pjsip_endpt_create_response(ast_sip_get_pjsip_endpoint(),
                rdata, 200, NULL, &resp) == PJ_SUCCESS)
            pjsip_endpt_send_response2(ast_sip_get_pjsip_endpoint(),
                rdata, resp, NULL, NULL);
        return PJ_TRUE;
    }

    /* ---- parse <dialogid> ------------------------------------------ */
    p = strstr(body, "<dialogid>");
    q = strstr(body, "</dialogid>");
    if (!p || !q) goto malformed;
    slen = (size_t)(q - p) + strlen("</dialogid>");
    if (slen >= sizeof(section)) slen = sizeof(section) - 1;
    memcpy(section, p, slen); section[slen] = '\0';

    if (xml_get(section, "callid",    dlg_callid, sizeof(dlg_callid)) ||
        xml_get(section, "localtag",  dlg_ltag,   sizeof(dlg_ltag))   ||
        xml_get(section, "remotetag", dlg_rtag,   sizeof(dlg_rtag)))
        goto malformed;

    /* ---- parse <consultdialogid> ------------------------------------ */
    p = strstr(body, "<consultdialogid>");
    q = strstr(body, "</consultdialogid>");
    if (!p || !q) goto malformed;
    slen = (size_t)(q - p) + strlen("</consultdialogid>");
    if (slen >= sizeof(section)) slen = sizeof(section) - 1;
    memcpy(section, p, slen); section[slen] = '\0';

    if (xml_get(section, "callid",    con_callid, sizeof(con_callid)) ||
        xml_get(section, "localtag",  con_ltag,   sizeof(con_ltag))   ||
        xml_get(section, "remotetag", con_rtag,   sizeof(con_rtag)))
        goto malformed;

    ast_log(LOG_NOTICE, "CiscoConf: Conference REFER — dialog='%s' consult='%s'\n",
        dlg_callid, con_callid);

    char existing_conf[64] = "";
    const char *ev;

    /* ---- channel lookup (done in PJSIP thread) --------------------- */
    /*
     * dialogid = held call (Asterisk outdialled phone 220).
     *   dlg_ltag = phone's local tag  = Asterisk's remote-tag
     *   dlg_rtag = Asterisk's local tag
     */
    ch_first = channel_for_dialog(dlg_callid, dlg_ltag, dlg_rtag);
    if (!ch_first) goto lookup_fail;

    ast_channel_lock(ch_first);
    ev = pbx_builtin_getvar_helper(ch_first, "CISCO_CONF_ROOM");
    if (ev && ev[0] != '\0') {
        ast_copy_string(existing_conf, ev, sizeof(existing_conf));
    }
    ast_channel_unlock(ch_first);

    if (existing_conf[0] == '\0') {
        ch_held = ast_channel_bridge_peer(ch_first);
        if (!ch_held) {
            ast_log(LOG_ERROR, "CiscoConf: no bridge peer for '%s'\n",
                ast_channel_name(ch_first));
            ast_channel_unref(ch_first);
            goto lookup_fail;
        }
    } else {
        ch_held = NULL; /* phone is already in a conference bridge */
    }

    /*
     * consultdialogid = consult call (phone 220 called Asterisk).
     *   con_ltag = phone's local tag  = Asterisk's remote-tag
     *   con_rtag = Asterisk's local tag
     */
    ch_second = channel_for_dialog(con_callid, con_ltag, con_rtag);
    if (!ch_second) {
        ast_channel_unref(ch_first);
        if (ch_held) ast_channel_unref(ch_held);
        goto lookup_fail;
    }

    ast_channel_lock(ch_second);
    ev = pbx_builtin_getvar_helper(ch_second, "CISCO_CONF_ROOM");
    if (ev && ev[0] != '\0' && existing_conf[0] == '\0') {
        ast_copy_string(existing_conf, ev, sizeof(existing_conf));
    }
    ast_channel_unlock(ch_second);

    if (ev && ev[0] != '\0') {
        ch_consult = NULL; /* phone is already in a conference bridge */
    } else {
        ch_consult = ast_channel_bridge_peer(ch_second);
        if (!ch_consult) {
            ast_log(LOG_ERROR, "CiscoConf: no bridge peer for '%s'\n",
                ast_channel_name(ch_second));
            ast_channel_unref(ch_first);
            if (ch_held) ast_channel_unref(ch_held);
            ast_channel_unref(ch_second);
            goto lookup_fail;
        }
    }

    /* ---- build thread task ----------------------------------------- */
    task = ast_calloc(1, sizeof(*task));
    if (!task) {
        ast_channel_unref(ch_first);
        if (ch_held) ast_channel_unref(ch_held);
        ast_channel_unref(ch_second);
        if (ch_consult) ast_channel_unref(ch_consult);
        goto lookup_fail;
    }

    if (ch_held) {
        ast_copy_string(task->held_name, ast_channel_name(ch_held), sizeof(task->held_name));
    }
    ast_copy_string(task->phone_name,   ast_channel_name(ch_second),  sizeof(task->phone_name));
    if (ch_consult) {
        ast_copy_string(task->consult_name, ast_channel_name(ch_consult), sizeof(task->consult_name));
    }
    ast_copy_string(task->drop_name,    ast_channel_name(ch_first),   sizeof(task->drop_name));
    
    if (existing_conf[0] != '\0') {
        ast_copy_string(task->room, existing_conf, sizeof(task->room));
    } else {
        snprintf(task->room, sizeof(task->room), "cc%ld", (long)ast_tvnow().tv_sec);
    }

    ast_log(LOG_NOTICE,
        "CiscoConf: room=%s held=%s phone=%s consult=%s drop=%s\n",
        task->room, task->held_name[0] ? task->held_name : "(none)", task->phone_name,
        task->consult_name[0] ? task->consult_name : "(none)", task->drop_name);

    ast_channel_unref(ch_first);
    if (ch_held) ast_channel_unref(ch_held);
    ast_channel_unref(ch_second);
    if (ch_consult) ast_channel_unref(ch_consult);
    /* ---- send 202 Accepted ----------------------------------------- */
    if (pjsip_endpt_create_response(ast_sip_get_pjsip_endpoint(),
            rdata, 202, NULL, &resp) == PJ_SUCCESS)
        pjsip_endpt_send_response2(ast_sip_get_pjsip_endpoint(),
            rdata, resp, NULL, NULL);
    else
        ast_log(LOG_ERROR, "CiscoConf: could not create 202 response\n");

    /* ---- spawn detached conference worker -------------------------- */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thr, &attr, cc_conference_thread, task) != 0) {
        ast_log(LOG_ERROR, "CiscoConf: pthread_create failed\n");
        ast_free(task);
    }
    pthread_attr_destroy(&attr);

    return PJ_TRUE;

malformed:
    ast_log(LOG_WARNING, "CiscoConf: malformed x-cisco-remotecc body\n");
    if (pjsip_endpt_create_response(ast_sip_get_pjsip_endpoint(),
            rdata, 400, NULL, &resp) == PJ_SUCCESS)
        pjsip_endpt_send_response2(ast_sip_get_pjsip_endpoint(),
            rdata, resp, NULL, NULL);
    return PJ_TRUE;

lookup_fail:
    ast_log(LOG_ERROR, "CiscoConf: channel lookup failed; conference not created\n");
    if (pjsip_endpt_create_response(ast_sip_get_pjsip_endpoint(),
            rdata, 500, NULL, &resp) == PJ_SUCCESS)
        pjsip_endpt_send_response2(ast_sip_get_pjsip_endpoint(),
            rdata, resp, NULL, NULL);
    return PJ_TRUE;
}

/* ---------- module registration --------------------------------------- */

static pjsip_module cisco_cc_pjsip_module = {
    .name     = { "mod-cisco-conference", 20 },
    /*
     * Lower number = higher priority in PJSIP.
     * PJSIP_MOD_PRIORITY_APPLICATION is 32; we run at 31 so we intercept
     * x-cisco-remotecc REFERs before res_pjsip_refer sees them.
     */
    .priority = PJSIP_MOD_PRIORITY_APPLICATION - 1,
    .on_rx_request = cisco_cc_on_rx_request,
};

static int load_module(void)
{
    if (ast_sip_register_service(&cisco_cc_pjsip_module)) {
        ast_log(LOG_ERROR, "CiscoConf: failed to register PJSIP service\n");
        return AST_MODULE_LOAD_DECLINE;
    }
    ast_log(LOG_NOTICE, "CiscoConf: Cisco conference module loaded\n");
    return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
    ast_sip_unregister_service(&cisco_cc_pjsip_module);
    return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT,
    "Cisco x-cisco-remotecc Conference Handler",
    .support_level = AST_MODULE_SUPPORT_EXTENDED,
    .load   = load_module,
    .unload = unload_module,
    .requires = "res_pjsip,res_pjsip_session",
);
