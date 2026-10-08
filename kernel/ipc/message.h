/* IPC message queue. 64 slots is enough for mouse bursts + slow redraws. */
#ifndef RXOS_MESSAGE_H
#define RXOS_MESSAGE_H

#include "rogex.h"

#define MSG_PAYLOAD 64
#define MSG_SLOTS   64            /* 200 Hz mouse bursts + slow redraws fit */

/* Well-known senders (rx_message_t.from). */
#define MSG_FROM_KERNEL   0u
#define MSG_FROM_KEYBOARD 1u      /* payload: 1 byte, Latin-1 char           */
#define MSG_FROM_MOUSE    2u      /* payload: rx_mouse_event                 */

typedef struct ROGEX_PACKED {
    int16_t dx, dy;               /* screen-space deltas (y grows downward)  */
    uint8_t buttons;              /* bit0 left, bit1 right, bit2 middle      */
} rx_mouse_event;

typedef struct {
    uint32_t from;                /* sender id (MSG_FROM_*)   */
    uint32_t len;
    uint8_t  data[MSG_PAYLOAD];
} rx_message_t;

void msg_init(void);
bool msg_send(uint32_t from, const void *data, uint32_t len);  /* false = full  */
bool msg_recv(rx_message_t *out);                              /* false = empty */
int  msg_pending(void);
bool msg_selftest(void);           /* send/recv/order/overflow — true = PASS */

#endif /* RXOS_MESSAGE_H */
