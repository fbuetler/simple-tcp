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
    buffer_t *send_buffer;
    buffer_t *recv_buffer;

    uint64_t window_max_size;
    uint64_t window_size; // semantically equal to buffer_size(r->send_buffer)
    uint64_t retransmission_timer;

    uint32_t current_seq_no;
    uint32_t current_ack_no;

    int outputBufferFull;
    int send_EOF;
    int recv_EOF;
};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create(conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc)
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

    r->recv_buffer = xmalloc(sizeof(buffer_t));
    r->recv_buffer->head = NULL;

    r->window_max_size = cc->window;
    r->window_size = 0;

    r->retransmission_timer = cc->timeout;

    r->current_seq_no = 1;
    r->current_ack_no = 1;

    r->outputBufferFull = 0;
    r->send_EOF = 0;
    r->recv_EOF = 0;

    return r;
}

long getCurrentTime()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;
    return now_ms;
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
    buffer_clear(r->recv_buffer);
    free(r->recv_buffer);
    // ...
}

// n is the length of the pkt
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n)
{
    // catch impossible packets
    uint16_t len = ntohs(pkt->len);
    if ((n != 8 && n < 12) || len != (uint16_t)n)
    {
        fprintf(stderr, "error: impossible packet size\n");
        return;
    }

    uint16_t checksum = pkt->cksum;
    pkt->cksum = htons(0);

    //catch corrupted packets
    if (cksum(pkt, n) != checksum)
    {
        fprintf(stderr, "error: corrupted paket\n");
        return;
    }

    // ACK PACKET
    if (n == 8)
    {
        int w = buffer_remove(r->send_buffer, ntohl(pkt->ackno));
        r->window_size -= w;
        print_pkt(pkt, "sender: got ack", 8);
        rel_read(r);
        return;
    }

    // drop packet if out of window
    uint32_t seqno = ntohl(pkt->seqno);
    if (seqno < r->current_ack_no || r->current_ack_no + r->window_max_size <= seqno)
    {
        print_pkt(pkt, "receiver: got pkt out of window", n);
        send_ack(r);
        return;
    }

    // Store in the buffer if not already there
    if (!buffer_contains(r->recv_buffer, seqno))
    {
        buffer_insert(r->recv_buffer, pkt, 0);
    }

    // EOF PACKET
    if (n == 12)
    {
        r->recv_EOF = 1;
        print_pkt(pkt, "receiver: got EOF", 12);
    }
    else
    {
        print_pkt(pkt, "receiver: got packet", n);
    }

    // NORMAL DATA PACKET

    // Release data [seqno, RCV.NXT - 1] with rel_output() TODO
    if (seqno == r->current_ack_no)
    {
        rel_output(r);
    }

    // Send back ACK with cumulative ackno = RCV.NXT
    send_ack(r);
}

void send_ack(rel_t *r)
{
    if (!r->outputBufferFull)
    {
        uint32_t ackno = r->current_ack_no;
        struct ack_packet ack_pkt = {htons(0), htons(8), htonl(ackno)};
        ack_pkt.cksum = cksum(&ack_pkt, 8);

        packet_t *ack = (packet_t *)&ack_pkt;

        int e = conn_sendpkt(r->c, ack, 8);
        if (e == -1 || e != 8)
        {
            fprintf(stderr, "error: could not send ack\n");
            return;
        }
        print_pkt(ack, "recevier: send ack", 8);
    }
}

void rel_read(rel_t *s)
{
    while (s->window_size < s->window_max_size && !s->send_EOF)
    {
        // get data from stdin
        char *buf = xmalloc(500);
        int data_size = conn_input(s->c, buf, 500);
        if (data_size == 0) // no data currently available
        {
            free(buf);
            return;
        }
        else if (data_size == -1) // EOF
        {
            // create packet with header
            packet_t *p = xmalloc(sizeof(packet_t));
            p->cksum = htons(0);
            p->len = htons(12);
            p->ackno = htonl(0); // TODO possibly piggy pack ACKs
            p->seqno = htonl(s->current_seq_no);

            // calc checksum (already in network order)
            p->cksum = cksum(p, 12);

            // send packet
            int e = conn_sendpkt(s->c, p, 12);
            if (e == -1 || e != 12)
            {
                fprintf(stderr, "error: could not send pkg\n");
                return;
            }

            s->send_EOF = 1;

            free(buf);
            buf = NULL;

            print_pkt(p, "sender: send EOF", 12);
            return;
        }

        // create packet with header
        packet_t *p = xmalloc(sizeof(packet_t));
        p->cksum = htons(0);
        p->len = htons(data_size + 12);
        p->ackno = htonl(0); // TODO possibly piggy pack ACKs
        p->seqno = htonl(s->current_seq_no);
        for (int i = 0; i < data_size; i++)
        {
            p->data[i] = buf[i];
        }

        free(buf);
        buf = NULL;

        // calc checksum (already in network order)
        p->cksum = cksum(p, data_size + 12);

        // send packet
        int e = conn_sendpkt(s->c, p, data_size + 12);
        if (e == -1 || e != data_size + 12)
        {
            fprintf(stderr, "error: could not send pkg\n");
            return;
        }

        // update state
        buffer_insert(s->send_buffer, p, getCurrentTime());
        s->window_size++;
        s->current_seq_no++;
        print_pkt(p, "sender: send pkt", data_size + 12);
    }
    if (s->window_size >= s->window_max_size)
    {
        fprintf(stderr, "info sender: window full\n");
    }
    else
    {
        fprintf(stderr, "info sender: EOF read\n");
    }
    return;
}

void rel_output(rel_t *r)
{
    buffer_node_t *node = buffer_get_first(r->recv_buffer);
    if (node == NULL)
    {
        return;
    }
    size_t data_size = ntohs(node->packet.len) - 12;
    void *buf = &node->packet.data;

    // check if output_buf has space
    if (data_size <= conn_bufspace(r->c))
    {

        int e = conn_output(r->c, buf, data_size);
        if (e == -1 || e != data_size)
        {
            fprintf(stderr, "error: could not send pkg\n");
            return;
        }
        r->current_ack_no++;
        buf = NULL;

        e = buffer_remove_first(r->recv_buffer);
        if (e != 0)
        {
            fprintf(stderr, "error: could not remove node form buffer\n");
            return;
        }
        r->outputBufferFull = 0;
    }
    else
    {
        r->outputBufferFull = 1;
    }

    return;
}

void rel_timer()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    while (current != NULL)
    {
        buffer_node_t *current_node = buffer_get_first(current->send_buffer);
        uint64_t retransmission_timer = current->retransmission_timer;

        long now_ms = getCurrentTime();

        // go over window (alternatively go over window size)
        while (current_node != NULL)
        {
            if (now_ms - current_node->last_retransmit > retransmission_timer)
            {
                // retransmit packet
                packet_t *packet = &current_node->packet;
                int e = conn_sendpkt(current->c, packet, ntohs(packet->len));
                if (e == -1 || e != ntohs(packet->len))
                {
                    return; // TODO what else ?
                }
                current_node->last_retransmit = now_ms;
            }
            current_node = current_node->next;
        }

        // before rel_destoy: EOF send, EOF received, send_buffer empty, output_buffer empty
        if (buffer_size(current->send_buffer) == 0 && buffer_size(current->recv_buffer) == 0 && current->send_EOF && current->recv_EOF)
        {
            rel_destroy(current);
            fprintf(stderr, "info: connection destroyed\n");
            return;
        }
        current = rel_list->next;
    }

    return;
}
