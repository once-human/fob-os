/* fob: the iPhone link. Notifications come from Apple's built-in notification service (ANCS); no app on the phone.
   The watch is the client: once the iPhone has paired with it and is sharing notifications, every notification is
   fetched (app, title, message) and kept in a short list for the Inbox. */
#ifndef FOB_ANCS_H
#define FOB_ANCS_H
#include <stdint.h>
#include <stdbool.h>

#define FOB_NOTES 8
typedef struct
{
    uint32_t uid;
    uint8_t  cat;           /* ANCS category: 1 incoming call, 2 missed call, 4 social, 5 schedule, 6 email ... */
    uint8_t  flags;
    uint8_t  unread;
    char     app[14];       /* friendly lowercase name: whatsapp, messages, mail ... */
    char     title[40];     /* lowercase ASCII (the watch fonts have no other letters yet) */
    char     msg[100];
    uint32_t t_ms;
} fob_note_t;

enum { FA_NONE, FA_LINKED, FA_DISCOVERING, FA_READY, FA_DENIED };    /* denied: paired, but the phone is not sharing notifications */

void     fob_ancs_init(void);                    /* after the stack is up */
void     fob_ancs_start(uint8_t conn_id);        /* the link is encrypted (paired): find the notification service */
void     fob_ancs_link_down(uint8_t conn_id);
bool     fob_ancs_is_phone(uint8_t conn_id);
uint8_t  fob_ancs_state(void);
uint8_t  fob_ancs_count(void);
bool     fob_ancs_get(uint8_t i, fob_note_t *out);       /* 0 = newest */
uint32_t fob_ancs_gen(void);                     /* changes whenever the list changes */
uint8_t  fob_ancs_unread(void);
void     fob_ancs_mark_read(void);
bool     fob_ancs_call_active(fob_note_t *out);  /* an incoming call is ringing */
bool     fob_ancs_action(uint32_t uid, bool positive);   /* positive: answer / open, negative: decline / dismiss */
void     fob_ancs_forget(void);
#endif
