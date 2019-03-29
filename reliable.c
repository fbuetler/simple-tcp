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

    uint64_t window_size;
    uint64_t retransmission_timer;
    uint32_t current_seq_no;

    uint32_t current_ack_no;

    buffer_node_t *send_unack;
    buffer_node_t *send_next;

    buffer_node_t *recv_next;
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
    r->retransmission_timer = cc->timeout;
    r->current_seq_no = 1;

    r->send_next = r->send_buffer->head;
    r->send_unack = r->send_buffer->head;

    r->recv_next = r->rec_buffer->head;
    r->current_ack_no = 1;

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
    if (n != 8 && n < 12)
    {
        //oopsi
        return;
    }

    uint16_t checksum = ltons(pkt->cksum);
    uint16_t len = ltohs(pkt->len);
    uint32_t ackno = ltohl(pkt->ackno);

    //catch corrupted packets
    if (len != checksum)
    {
        //oopsi
        return;
    }

    // ACK PACKET
    if (n == 8)
    {
        //check if expteced one
        if (ackno == r->send_unack)
        {
            // slide window
            r->send_unack = r->send_unack->next;
            return;
        }
        // TODO store out of order ACKs?
        return;
    }

    // end-of-file tranmission
    if (n == 12)
    {
        //check if all receivec packets are written to stdout
        // then rel_destroy()
        return;
    }

    // NORMAL DATA PACKET
    // check if output_buf has space
    if (len > conn_bufspace(r->c))
    {
        return;
    }

    // get seqno and extract data from packet
    uint32_t seqno = ltohl(pkt->seqno);
    char *data[len - 12];
    for (int i = 0; i < len - 12; i++)
    {
        data[i] = pkt->data[i];
    }

    // drop packet if out of window
    if (seqno >= r->recv_next + r->window_size)
    {
        return;
    }
    // Store in the buffer if not already there
    if (!buffer_contains(r->rec_buffer, seqno))
    {
        buffer_insert(r->rec_buffer, pkt, 0);
    }
    //set recv_next to highest seqno consecutively stored in the buffer + 1
    if (seqno == r->recv_next)
    {
        int max_seqno = seqno;
        buffer_node_t *current = r->recv_next->next;
        while (current != NULL && current->packet.seqno == max_seqno + 1)
        {
            max_seqno = current->packet.seqno;
            current = current->next;
        }
        r->recv_next = current;
        r->current_ack_no = max_seqno;
    }

    // Release data [seqno, RCV.NXT - 1] with rel_output()
    rel_ouput(r);

    // Send back ACK with cumulative ackno = RCV.NXT
    struct ack_packet *ack = {htons(0), htons(8), htonl(r->current_ack_no)};
}

void rel_read(rel_t *s)
{
    int maxSizeOfPacket = 500;
    //TODO assumption: return value is in bytes
    size_t inputBufSize = s->window_size - buffer_size(s->send_buffer);

    // check if buffer has enough cap for new packets
    if (inputBufSize <= 0)
    {
        return;
    }

    // read data from stdin in bytes
    void *buf = malloc(inputBufSize);
    int d = conn_input(s->c, buf, inputBufSize);
    if (d == 0) // no data currently available
    {
        free(buf);
        return;
    }
    // read an EOF or error from input and ALL packets sent are acknowledged
    // TODO add other termination constraints
    else if (d == -1 && s->send_unack == s->send_next)
    {
        free(buf);
        rel_destroy(s);
    }

    // integer div to get how many packets we have
    // check if remaining bytes with mod; possible small packet
    int number_packets = d / maxSizeOfPacket;
    int extra_packet_size = 0;
    if (d % maxSizeOfPacket != 0)
    {
        number_packets++;
        extra_packet_size = d % maxSizeOfPacket;
    }

    // split and prepend for each packet a header and put into buffer
    // TODO ACK is 0 here ?
    unsigned char *ptr = buf;
    for (int i = 0; i < number_packets - 1; i++)
    {
        char data[maxSizeOfPacket];
        for (int j = 0; j < maxSizeOfPacket; j++)
        {
            data[j] = ptr[i * maxSizeOfPacket + j];
        }
        uint16_t checksum = cksum(*data, maxSizeOfPacket);
        packet_t *p = {checksum, // already in network order
                       htons(maxSizeOfPacket),
                       htonl(0),
                       htonl(s->current_seq_no),
                       data};
        s->current_seq_no++;
        buffer_insert(s->send_buffer, p, 0);
        free(buf);
    }
    // same for extra packet (smaller than 500 b)
    if (extra_packet_size != 0)
    {
        char data[extra_packet_size];
        for (int j = 0; j < extra_packet_size; j++)
        {
            data[j] = ptr[(number_packets - 1) * maxSizeOfPacket + j];
        }
        uint16_t checksum = cksum(*data, extra_packet_size);
        packet_t *p = {checksum, // already in network order
                       htons(extra_packet_size),
                       htonl(0),
                       htonl(s->current_seq_no),
                       data};
        s->current_seq_no++;
        buffer_insert(s->send_buffer, p, 0);
        free(buf);
    }

    // check if window is at max size
    if (s->send_next - s->send_unack >= s->window_size)
    {
        return;
    }

    //get current time
    struct timeval now;
    gettimeofday(&now, NULL);
    long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;

    // send next packet
    packet_t *packet = &s->send_next->packet;
    int e = conn_sendpkt(s, packet, packet->len);
    if (e == -1 || e != packet->len)
    {
        return; // TODO what else ?
    }

    //set last retransmit of packet to now and update send_next
    s->send_next->last_retransmit = now_ms;
    s->send_next = s->send_next->next;
    return;
}

void rel_output(rel_t *r)
{
    // check if bufspace is enough for taking next package
    size_t space = conn_bufspace(r->c);
    size_t len = buffer_get_first(r->rec_buffer)->packet.len;
    if (space < len)
    {
        return;
    }

    void *buf = malloc(len);
    buf = (void *)buffer_get_first(r->rec_buffer)->packet.data;
    int e = conn_output(r->c, buf, len);
    free(buf);
    return;
}

void rel_timer()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    while (current != NULL)
    {
        buffer_node_t *current_node = current->send_unack;
        buffer_node_t *send_next = current->send_next;
        uint64_t retransmission_timer = current->retransmission_timer;

        //get current time
        struct timeval now;
        gettimeofday(&now, NULL);
        long now_ms = now.tv_sec * 1000 + now.tv_usec / 1000;

        // go over window
        while (current_node != send_next)
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
            }
            current_node = current_node->next;
        }
        current = rel_list->next;
    }
}
