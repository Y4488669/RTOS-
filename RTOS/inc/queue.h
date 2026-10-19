#ifndef __RTOS_QUEUE_H
#define __RTOS_QUEUE_H

#include "list.h"
#include <stdint.h>

typedef struct{

    uint8_t *buf;
    uint32_t item_size;
    uint32_t depth;
    uint32_t head;
    uint32_t count;
    list_t send_wait;
    list_t recv_wait;
}queue_t;


void queue_init(queue_t *q, void *buf, uint32_t item_size, uint32_t depth);

int queue_send(queue_t *q, const void *item, uint32_t timeout_ticks);
int queue_recv(queue_t *q, void *item, uint32_t timeout_ticks);

uint32_t queue_count(const queue_t *q);



typedef struct {
    queue_t q;
}mailbox_t;

void mailbox_init(mailbox_t *m, void *buf, uint32_t item_size);

int mailbox_put(mailbox_t *m, const void *item);

int mailbox_get(mailbox_t *m, void *item, uint32_t timeout_ticks);

#endif //RTOS_QUEUE
