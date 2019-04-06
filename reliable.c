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

    uint64_t window_max_size;
    uint64_t window_size;
    uint64_t retransmission_timer;

    uint32_t current_seq_no;
    uint32_t current_ack_no;

    uint32_t current_expt_seq_no;

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

    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;

    r->window_max_size = cc->window;
    r->window_size = 0;

    r->retransmission_timer = cc->timeout;

    r->current_seq_no = 1;
    r->current_ack_no = 1;

    r->current_expt_seq_no = 1;

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
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    // ...
}

// n is the length of the pkt
void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n)
{
    if (n != 8 && n < 12)
    {
        fprintf(stderr, "error: impossible packet size\n");
        return;
    }
    fprintf(stderr, "info: n = %ld\n", n);

    uint16_t checksum = pkt->cksum;
    pkt->cksum = htons(0);

    //catch corrupted packets
    if (cksum(pkt, n) != checksum)
    {
        fprintf(stderr, "error: corrupted paket\n");
        return; // TODO uncomment this, but first fix checksum
    }

    uint16_t len = ntohs(pkt->len);
    uint32_t ackno = ntohl(pkt->ackno);

    // ACK PACKET
    if (n == 8)
    {
        //check if expteced one
        if (ackno == r->current_ack_no)
        {
            // slide window
            r->window_size++;
            r->current_ack_no++;
            fprintf(stderr, "info: ack received\n");
            return;
        }
        // TODO store out of order ACKs?
        fprintf(stderr, "error: not expected one\n");
        return;
    }

    // end-of-file tranmission
    if (n == 12)
    {
        r->recv_EOF = 1;
        //check if all receivec packets are written to stdout
        // then rel_destroy()
        fprintf(stderr, "info: end of connection packet\n");
        return;
    }

    // NORMAL DATA PACKET

    // check if output_buf has space
    if (len > conn_bufspace(r->c))
    {
        fprintf(stderr, "error: no space in output buf\n");
        return;
    }

    // drop packet if out of window
    uint32_t seqno = ntohl(pkt->seqno);
    if (seqno < r->current_expt_seq_no || r->current_expt_seq_no + r->window_max_size <= seqno)
    {
        fprintf(stderr, "error: received packet is out of window\n");
        return;
    }

    // Store in the buffer if not already there
    if (!buffer_contains(r->rec_buffer, seqno))
    {
        buffer_insert(r->rec_buffer, pkt, 0);
    }

    //set recv_next to highest seqno consecutively stored in the buffer + 1
    buffer_node_t *current = buffer_get_first(r->rec_buffer);
    if (seqno == ntohl(r->current_expt_seq_no))
    {
        int max_seqno = seqno;
        while (current != NULL && ntohl(current->packet.seqno) == max_seqno + 1)
        {
            max_seqno = ntohl((current->packet.seqno));
            current = current->next;
        }
        r->current_expt_seq_no = current->packet.seqno;
    }

    // Release data [seqno, RCV.NXT - 1] with rel_output() TODO
    rel_output(r);

    // Send back ACK with cumulative ackno = RCV.NXT
    ackno = r->current_ack_no;
    packet_t ack_packet = {htons(0), htons(0), htonl(ackno)};
    ack_packet.cksum = cksum(&ack_packet, 8);

    packet_t *ack = &ack_packet;

    //TODO how to send ACK packets
    int e = conn_sendpkt(r->c, ack, 8);
    if (e == -1 || e != 8)
    {
        fprintf(stderr, "error: could not send ack\n");
        return;
    }
    r->current_ack_no++;
    fprintf(stderr, "info: ack sent\n");
}

void rel_read(rel_t *s)
{
    // before rel_destoy: EOF send, EOF received, send_buffer empty, output_buffer empty

    while (s->window_size > 0 || !s->send_EOF)
    {
        // get data from stdin
        char *buf = xmalloc(500);
        int data_size = conn_input(s->c, buf, 500);
        if (data_size == 0) // no data currently available
        {
            free(buf);
            fprintf(stderr, "error: no data available to read\n");
            return;
        }
        else if (data_size == -1) // EOF
        {
            free(buf);
            // TODO send EOF
            fprintf(stderr, "info: send EOF\n");
            return;
        }

        // create packet with header
        packet_t *p = xmalloc(sizeof(packet_t));
        p->cksum = htons(0);
        p->len = htons(data_size);
        p->ackno = htonl(0);
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
        s->window_size--;
        s->current_seq_no++;
        fprintf(stderr, "info: packet sent\n");
    }
    fprintf(stderr, "info: window full or EOF read\n");
    return;
}

void rel_output(rel_t *r)
{
    buffer_node_t *node = buffer_get_first(r->rec_buffer);
    size_t len = ntohs(node->packet.len);
    void *buf = &node->packet.data;

    int e = buffer_remove_first(r->rec_buffer);
    if (e != 0)
    {
        fprintf(stderr, "error: could not remove node form buffer\n");
        return;
    }

    e = conn_output(r->c, buf, len);
    if (e == -1 || e != len)
    {
        fprintf(stderr, "error: could not send pkg\n");
        return;
    }
    buf = NULL;

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
                int e = conn_sendpkt(current->c, packet, packet->len);
                if (e == -1 || e != packet->len)
                {
                    return; // TODO what else ?
                }
                current_node->last_retransmit = now_ms;
            }
            current_node = current_node->next;
        }
        current = rel_list->next;
    }
}
