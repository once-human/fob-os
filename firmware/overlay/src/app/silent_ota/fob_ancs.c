/* fob: Apple Notification Center Service client (see fob_ancs.h) */
#include <string.h>
#include <trace.h>
#include <os_sync.h>
#include "gap_bond_le.h"
#include "gap_le.h"
#include "gap_conn_le.h"
#include "profile_client.h"
#include "ancs_client.h"
#include "fob_ancs.h"

#define TITLE_MAX 40
#define MSG_MAX   100
#define PEND_MAX  6

typedef struct { uint8_t state; uint8_t attr_id; uint16_t attr_len, got; uint8_t cmd; uint8_t uidb[4]; uint8_t k; uint8_t want, have; char buf[MSG_MAX + 1]; } parse_t;
typedef struct { uint32_t uid; uint8_t cat, flags; } pend_t;

static T_CLIENT_ID ancs_client;
static volatile uint8_t  st;
static uint8_t  phone_conn = 0xFF;
static parse_t  ps;
static pend_t   pend[PEND_MAX]; static uint8_t n_pend;
static bool     busy; static uint32_t busy_since;
static fob_note_t cur;                       /* the notification being filled in */
static fob_note_t notes[FOB_NOTES]; static volatile uint8_t n_notes;
static volatile uint32_t gen;
static fob_note_t call; static volatile bool call_on;
extern uint32_t os_sys_time_get(void);

/* ---------- text: the watch fonts are lowercase ASCII only, so anything else is mapped or dropped ---------- */
static void put_ascii(char *dst, uint16_t max, const uint8_t *s, uint16_t n)
{
    uint16_t o = 0; bool q = false;
    for (uint16_t i = 0; i < n && o + 1 < max; i++)
    {
        uint8_t c = s[i];
        if (c >= 'A' && c <= 'Z') { c = (uint8_t)(c + 32); }
        if (c == '\n' || c == '\r' || c == '\t') { c = ' '; }
        if (c == '"') { c = '\''; }
        if (c == '_') { c = '-'; }
        if (c >= 0x80)
        {
            if (c == 0xE2 && i + 2 < n && s[i + 1] == 0x80 && (s[i + 2] == 0x99 || s[i + 2] == 0x98)) { dst[o++] = '\''; i += 2; q = false; continue; }
            if (c == 0xE2 && i + 2 < n && s[i + 1] == 0x80 && (s[i + 2] == 0x9C || s[i + 2] == 0x9D)) { dst[o++] = '\''; i += 2; q = false; continue; }
            if (c >= 0xC0 && !q) { dst[o++] = '?'; q = true; }          /* one ? for a run of other characters (emoji, other scripts) */
            continue;
        }
        q = false;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == ',' || c == ':' || c == ';' || c == '%' || c == '+' || c == '-' || c == '/' || c == '(' || c == ')' || c == '\'' || c == '?' || c == '!' || c == '&' || c == '<' || c == '>' || c == '=') { dst[o++] = (char)c; }
        else if (c == '@' ) { dst[o++] = 'a'; }
        else if (c == '#' ) { dst[o++] = 'n'; }
        else { dst[o++] = ' '; }
    }
    dst[o] = 0;
}
static void friendly(char *dst, const uint8_t *id, uint16_t n)
{
    char b[64]; uint16_t l = n < 63 ? n : 63; memcpy(b, id, l); b[l] = 0;
    static const char *const MAP[][2] = {{"MobileSMS", "messages"}, {"mobilephone", "phone"}, {"mobilemail", "mail"}, {"WhatsApp", "whatsapp"}, {"instagram", "instagram"}, {"Gmail", "gmail"},
                                         {"Telegraph", "telegram"}, {"Tweetie", "x"}, {"LinkedIn", "linkedin"}, {"spotify", "spotify"}, {"mobilecal", "calendar"}, {"reminders", "reminders"},
                                         {"facetime", "facetime"}, {"Slack", "slack"}, {"discord", "discord"}, {"gpay", "gpay"}, {"paytm", "paytm"}, {"phonepe", "phonepe"}};
    for (unsigned i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) { if (strstr(b, MAP[i][0])) { strncpy(dst, MAP[i][1], 13); dst[13] = 0; return; } }
    const char *last = b, *p; for (p = b; *p; p++) { if (*p == '.') { last = p + 1; } }
    uint8_t o = 0; for (; *last && o < 12; last++) { char c = *last; if (c >= 'A' && c <= 'Z') { c = (char)(c + 32); } dst[o++] = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '-'; }
    dst[o] = 0;
}

/* ---------- the list ---------- */
static void list_add(const fob_note_t *n)
{
    uint32_t sv = os_lock();
    for (int i = FOB_NOTES - 1; i > 0; i--) { notes[i] = notes[i - 1]; }
    notes[0] = *n; notes[0].unread = 1;
    if (n_notes < FOB_NOTES) { n_notes++; }
    gen++;
    os_unlock(sv);
}
static void list_remove(uint32_t uid)
{
    uint32_t sv = os_lock();
    for (uint8_t i = 0; i < n_notes; i++)
    {
        if (notes[i].uid == uid) { for (uint8_t j = i; j + 1 < n_notes; j++) { notes[j] = notes[j + 1]; } n_notes--; gen++; break; }
    }
    if (call_on && call.uid == uid) { call_on = false; gen++; }
    os_unlock(sv);
}

/* ---------- fetching a notification's text ---------- */
static const uint8_t WANT[] = {0, 1, TITLE_MAX, 0, 3, MSG_MAX, 0};        /* app identifier, title (max 40), message (max 100) */
static void request_next(void)
{
    if (busy || !n_pend || phone_conn == 0xFF) { return; }
    pend_t p = pend[0]; for (uint8_t i = 0; i + 1 < n_pend; i++) { pend[i] = pend[i + 1]; } n_pend--;
    memset(&cur, 0, sizeof(cur)); cur.uid = p.uid; cur.cat = p.cat; cur.flags = p.flags; cur.t_ms = os_sys_time_get();
    memset(&ps, 0, sizeof(ps)); ps.want = 3;
    if (ancs_get_notification_attr(phone_conn, p.uid, (uint8_t *)WANT, sizeof(WANT))) { busy = true; busy_since = os_sys_time_get(); }
}
static void commit(void)
{
    busy = false;
    if (cur.title[0] || cur.msg[0] || cur.app[0])
    {
        if (cur.cat == 1) { call = cur; call_on = true; }
        list_add(&cur);
    }
    request_next();
}
static void attr_done(void)         /* one attribute fully received */
{
    switch (ps.attr_id)
    {
    case 0: friendly(cur.app, (const uint8_t *)ps.buf, ps.got); break;
    case 1: put_ascii(cur.title, TITLE_MAX + 1, (const uint8_t *)ps.buf, ps.got); break;
    case 3: put_ascii(cur.msg, MSG_MAX + 1, (const uint8_t *)ps.buf, ps.got); break;
    default: break;
    }
    if (++ps.have >= ps.want) { memset(&ps, 0, sizeof(ps)); commit(); }
    else { ps.state = 5; }
}
static void parse_ds(const uint8_t *d, uint16_t n)       /* Data Source: cmd, uid[4], then (id, len16, text)* , possibly across several packets */
{
    for (uint16_t i = 0; i < n; i++)
    {
        uint8_t c = d[i];
        switch (ps.state)
        {
        case 0: ps.cmd = c; ps.state = 1; ps.k = 0; break;
        case 1: ps.uidb[ps.k++] = c; if (ps.k == 4) { ps.state = 5; } break;
        case 5: ps.attr_id = c; ps.state = 6; break;
        case 6: ps.attr_len = c; ps.state = 7; break;
        case 7: ps.attr_len |= (uint16_t)(c << 8); ps.got = 0; ps.buf[0] = 0;
                if (ps.attr_len == 0) { attr_done(); } else { ps.state = 8; }
                break;
        case 8: if (ps.got < MSG_MAX) { ps.buf[ps.got] = (char)c; } ps.got++;
                if (ps.got >= ps.attr_len) { if (ps.got > MSG_MAX) { ps.got = MSG_MAX; } ps.buf[ps.got] = 0; attr_done(); }
                break;
        default: break;
        }
        if (ps.state == 0 && !busy) { return; }
    }
}
static void parse_ns(const uint8_t *d, uint16_t n)       /* Notification Source: event, flags, category, count, uid[4] */
{
    if (n != 8) { return; }
    uint8_t ev = d[0], fl = d[1], cat = d[2]; uint32_t uid = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    if (ev == 2) { list_remove(uid); return; }
    if (ev != 0 || (fl & 0x04)) { return; }              /* only brand-new notifications (not the ones already waiting when we connected) */
    if (n_pend < PEND_MAX) { pend[n_pend].uid = uid; pend[n_pend].cat = cat; pend[n_pend].flags = fl; n_pend++; }
    request_next();
}

static T_APP_RESULT ancs_cb(T_CLIENT_ID client_id, uint8_t conn_id, void *p_data)
{
    (void)client_id;
    T_ANCS_CB_DATA *cb = (T_ANCS_CB_DATA *)p_data;
    switch (cb->cb_type)
    {
    case ANCS_CLIENT_CB_TYPE_DISC_STATE:
        if (cb->cb_content.disc_state == DISC_ANCS_DONE) { ancs_set_data_source_notify(conn_id, true); }
        else if (cb->cb_content.disc_state == DISC_ANCS_FAILED) { st = FA_DENIED; }
        break;
    case ANCS_CLIENT_CB_TYPE_NOTIF_IND_RESULT:
        if (cb->cb_content.notify_data.type == ANCS_FROM_DATA_SOURCE) { parse_ds(cb->cb_content.notify_data.p_value, cb->cb_content.notify_data.value_size); }
        else if (cb->cb_content.notify_data.type == ANCS_FROM_NOTIFICATION_SOURCE) { parse_ns(cb->cb_content.notify_data.p_value, cb->cb_content.notify_data.value_size); }
        break;
    case ANCS_CLIENT_CB_TYPE_WRITE_RESULT:
        if (cb->cb_content.write_result.type == ANCS_WRITE_DATA_SOURCE_NOTIFY_ENABLE) { if (cb->cb_content.write_result.cause == 0) { ancs_set_notification_source_notify(conn_id, true); } else { st = FA_DENIED; } }
        else if (cb->cb_content.write_result.type == ANCS_WRITE_NOTIFICATION_SOURCE_NOTIFY_ENABLE) { if (cb->cb_content.write_result.cause == 0) { st = FA_READY; gen++; } else { st = FA_DENIED; } }
        break;
    case ANCS_CLIENT_CB_TYPE_DISCONNECT_INFO:
        fob_ancs_link_down(conn_id);
        break;
    default: break;
    }
    return APP_RESULT_SUCCESS;
}

/* ---------- public ---------- */
void fob_ancs_init(void) { client_init(2); ancs_client = ancs_add_client(ancs_cb, 2); }
void fob_ancs_start(uint8_t conn_id)
{
    if (st == FA_DISCOVERING || st == FA_READY) { return; }
    phone_conn = conn_id; st = FA_DISCOVERING; busy = false; n_pend = 0; gen++;
    if (!ancs_start_discovery(conn_id)) { st = FA_DENIED; }
}
void fob_ancs_link_down(uint8_t conn_id)
{
    if (conn_id != phone_conn) { return; }
    phone_conn = 0xFF; st = FA_NONE; busy = false; n_pend = 0; call_on = false; gen++;
}
bool fob_ancs_is_phone(uint8_t conn_id) { return conn_id == phone_conn; }
uint8_t fob_ancs_state(void)
{
    if (busy && os_sys_time_get() - busy_since > 3000) { busy = false; request_next(); }     /* a lost reply must not stall the queue */
    return st;
}
uint8_t fob_ancs_count(void) { return n_notes; }
bool fob_ancs_get(uint8_t i, fob_note_t *out) { if (i >= n_notes) { return false; } uint32_t sv = os_lock(); *out = notes[i]; os_unlock(sv); return true; }
uint32_t fob_ancs_gen(void) { return gen; }
uint8_t fob_ancs_unread(void) { uint8_t u = 0; for (uint8_t i = 0; i < n_notes; i++) { u += notes[i].unread; } return u; }
void fob_ancs_mark_read(void) { for (uint8_t i = 0; i < n_notes; i++) { notes[i].unread = 0; } gen++; }
bool fob_ancs_call_active(fob_note_t *out) { if (!call_on) { return false; } *out = call; return true; }
bool fob_ancs_action(uint32_t uid, bool positive) { return phone_conn != 0xFF && ancs_perform_notification_action(phone_conn, uid, positive ? 0 : 1); }

void fob_ancs_forget(void)          /* remove the pairing: the iPhone has to be paired again */
{
    le_bond_clear_all_keys();
    if (phone_conn != 0xFF) { le_disconnect(phone_conn); }
    phone_conn = 0xFF; st = FA_NONE; busy = false; n_pend = 0; n_notes = 0; call_on = false; gen++;
}
