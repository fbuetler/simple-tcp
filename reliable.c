#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"
#include "buffer.h"

struct reliable_state
{
    rel_t *next; /* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c; /* This is the connection object */

    /* Add your own data fields below this */
    // ...
    buffer_t *send_buffer;
    // ...
    buffer_t *rec_buffer;

    size_t window_size;
    size_t current_seq_num;
};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create(conn_t *c, const struct sockaddr_storage *ss,
           const struct config_common *cc)
{
    rel_t *r;

    r = xmalloc(sizeof(*r));
    memset(r, 0, sizeof(*r));

    if (!c)
    {
        c = conn_create(r, ss);
        if (!c)
        {
            free(r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
        rel_list->prev = &r->next;
    rel_list = r;

    /* Do any other initialization you need here... */
    // ...
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    // ...
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;

    r->window_size = cc->window;
    r->current_seq_num = 0;

    return r;
}

void rel_destroy(rel_t *r)
{
    if (r->next)
    {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy(r->c);

    /* Free any other allocated memory here */
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    // ...
}

// n is the expected length of pkt
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n)
{
    /* Your logic implementation here */
}

void rel_read(rel_t *s)
{
    size_t maxSizeOfPacket = 500;
    size_t inputBufSize = 5000; // TODO how big ?

    // read data from stdin in bytes
    void *buf = malloc(inputBufSize);
    int d = conn_input(s->c, buf, inputBufSize);
    if (d == 0)
    {
        free(buf);
        return;
    }

    // integer div to get how many packets we have
    // check if remaining bytes with mod; possible small packet
    size_t number_packets = d / maxSizeOfPacket;
    size_t extra_packet_size = 0;
    if (d % maxSizeOfPacket != 0)
    {
        number_packets++;
        extra_packet_size = d % maxSizeOfPacket;
    }

    // check if buffer has enough cap for new packets
    if (buffer_size(s->send_buffer) + number_packets > s->window_size)
    {
        free(buf);
        return;
    }

    // split and prepend for each packet a header
    unsigned char *ptr = buf;
    for (int i = 0; i < number_packets - 1; i++)
    {
        char *data[maxSizeOfPacket];
        for (int j = 0; j < maxSizeOfPacket; j++)
        {
            data[j] = ptr[i * maxSizeOfPacket + j];
        }
        packet_t *p = {0,
                       maxSizeOfPacket,
                       0, // TODO ACK is 0 here ?
                       s->current_seq_num,
                       data};
        s->current_seq_num++;
        // put packets in buffer
        buffer_insert(s->send_buffer, p, 0);
        free(buf);
    }
    // same for extra packet
    if (extra_packet_size != 0)
    {
        char *data[extra_packet_size];
        for (int j = 0; j < extra_packet_size; j++)
        {
            data[j] = ptr[(number_packets - 1) * maxSizeOfPacket + j];
        }
        packet_t *p = {0,
                       extra_packet_size,
                       0,
                       s->current_seq_num,
                       data};
        s->current_seq_num++;
        // put packets in buffer
        buffer_insert(s->send_buffer, p, 0);
        free(buf);
    }

    //get first from buffer
    buffer_node_t *node = buffer_get_first(s->send_buffer);
    packet_t *packet = &node->packet;

    // TODO introduce a new window_buf/pointers on send_buf
    // bc now only one packet can be sent at any time

    //get current time
    struct timeval now;
    gettimeofday(&now, NULL);
    long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;

    // send first packet
    int e = conn_sendpkt(s, packet, packet->len);
    if (e == -1 || e != packet->len)
    {
        return; // TODO what else ?
    }

    //set last retransmit of packet to now
    node->last_retransmit = now_ms;
    return;
}

void rel_output(rel_t *r)
{
    /* Your logic implementation here */
}

void rel_timer()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    while (current != NULL)
    {
        // ...
        current = rel_list->next;
    }
}
