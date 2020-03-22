/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/
//----------------------------------------------------------------------------//
//sender events:
//if get call from above
//call ctcp_init
//build buffer via ctcp_read
//call ctcp_makeseg_send
//if get ack
//check if ack == seq
//if equal
//free buffer
//obtain new buffer from queue?
//call ctcp_makeseg_send
//if not equal
// resend call ctcp_makeseg_send
// if timeout
// resend call ctcp_makeseg_send
//
//reciver events
// if get data with seq
// send ack with same seq
// if data corrupted
// send different ack
//----------------------------------------------------------------------------//
#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"
#include "unistd.h"
#define SEGSIZE sizeof(ctcp_segment_t)
#define SEND_DATA 0
#define SEND_FIN 1
#define SEND_ACK 2

int ctcp_makeseg_send(ctcp_state_t *state, int mode);
int ctcp_send_data(ctcp_state_t *state, bool retrans);


/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  linked_list_t *send_segments;  /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */

  /* FIXME: Add other needed fields. */
  char *send_buf; // buffer
  int send_buf_len; // length of buffer

  /* SENDER */
  int send_seq; /* acknowledged max segment number */
  int send_window_start; /* Send window start index */
  long lastsent_timeout; /* Timeout for the last packet */
  long timeout_standard; // standard timeout time
  int retransmit_counter; // counter for retransmit time max 5

  ll_node_t *max_seg_acked;
  ll_node_t *last_seg_sent;

  /* RECEIVER */
  int recv_ack; /* ACK number to Acknowledge */
  int recv_window_start;

  ctcp_segment_t *segment_lastsent;
  ctcp_segment_t *segment_lastreceived;
  //int state_num = 0;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */


ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  state->send_buf_len = 0;
  state->send_buf = (char *) malloc(1500);
  state->segment_lastsent = malloc(1500);
  state->segment_lastreceived = malloc(1500);

  state->send_segments = ll_create();
  state->max_seg_acked = NULL;
  state->last_seg_sent = NULL;

  state->lastsent_timeout = -1; /* Timeout for the last packet */
  state->timeout_standard = cfg->rt_timeout;
  state->retransmit_counter = 0;

  state->send_window_start = 1;
  state->send_seq = 1;

  state->recv_ack = 1;
  state->recv_window_start = 1;

  state_list = state;


  /* FIXME: Do any other initialization here. */

  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */
  free(state->send_buf);
  free(state->segment_lastsent);
  free(state);

  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* Get input */
  state->send_buf_len = conn_input(state->conn,state->send_buf,MAX_SEG_DATA_SIZE);

  /* Sent data accordingly */
  if(state->send_buf_len>=0){
    ctcp_makeseg_send(state,SEND_DATA);
  } else {
    state->send_buf_len = 0;
    ctcp_makeseg_send(state,SEND_FIN);
  }
}

int ctcp_makeseg_send(ctcp_state_t *state, int mode) {
  int seglen = SEGSIZE + state->send_buf_len;
  ctcp_segment_t *segment = malloc(seglen);

  /* initialize segment headers */
  segment->len    = htons(seglen);
  segment->window = htons(MAX_SEG_DATA_SIZE);
  segment->cksum  = htons(0x00);
  segment->seqno  = htonl(0x01);
  segment->ackno  = htonl(0x01);

  /* Set data field accordingly */
  if(mode==SEND_DATA){
    segment->seqno  = htonl(state->send_seq);
    segment->ackno  = htonl(state->recv_ack);
    segment->flags  = htonl(0x018);
    segment->cksum  = cksum(segment, seglen);

    memcpy(segment->data, state->send_buf, state->send_buf_len);

    /* Clear out send buffer */
    strcpy(state->send_buf,"");
    state->send_buf_len = 0;

    /* Queue the segment to be sent */
    ll_add(state->send_segments,segment);

    /* Call send_data to send if available */
    ctcp_send_data(state,0);

    return 0;
  }

  if(mode==SEND_ACK){
    segment->seqno  = htonl(state->send_seq);
    segment->ackno  = htonl(state->recv_ack);
    segment->flags  = TH_ACK;
    segment->cksum  = cksum(segment, seglen);
  }

  if(mode==SEND_FIN){
    segment->flags  = TH_FIN;
    segment->cksum  = cksum(segment, seglen);
  }

  /* Insert to send queue and call send */
  conn_send(state->conn, segment, seglen);
  state->segment_lastsent = segment;

  /* Tear down connection if FIN */
  if(mode==SEND_FIN){
    ctcp_destroy(state);
  }

  print_hdr_ctcp(segment);
  return 0;
}



int ctcp_send_data(ctcp_state_t *state, bool retrans){
  printf("CURRENT SENDLIST LENGTH: %i\n", ll_length(state->send_segments));
  /* Check for send availability */
  if(state->last_seg_sent==NULL){

    /* Nothing sent yet, send the first queued segment */
    ctcp_segment_t *segment = ll_front(state->send_segments)->object;
    conn_send(state->conn, segment, ntohs(segment->len));
    state->lastsent_timeout = current_time(); /* Record time */

    /* Set last segment sent point */
    state->last_seg_sent = ll_front(state->send_segments);
    print_hdr_ctcp(segment);

  } else if(retrans) {

    /* Retransmit last sent segment */
    ctcp_segment_t *segment = state->last_seg_sent->object;
    conn_send(state->conn, segment, ntohs(segment->len));
    state->lastsent_timeout = current_time(); /* Record time */

    print_hdr_ctcp(segment);

  } else if(state->last_seg_sent==state->max_seg_acked){

    /* Queue is up to date, send next in queue */
    ctcp_segment_t *segment = state->last_seg_sent->next->object;
    conn_send(state->conn, segment, ntohs(segment->len));
    state->lastsent_timeout = current_time(); /* Record time */

    /* Set last segment sent point */
    state->last_seg_sent = state->last_seg_sent->next;
    print_hdr_ctcp(segment);

  } else {

    /* Still waiting on previous segments */
    printf("Hey, send window is busy! wait a little bit\n");
    printf("LASTSEGSENT: %i\n", state->last_seg_sent);
    printf("MAXSEGACKED: %i\n", state->max_seg_acked);
    return 1;
  }
  return 0;
}




bool checksum(ctcp_segment_t *segment, size_t len){

	uint16_t recved = segment->cksum;
	segment->cksum = htons(0x00);
  uint16_t expected = cksum(segment, len);

  return recved == expected;
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {

  print_hdr_ctcp(segment);

  /* Check integrity
  if (!checksum(segment, ntohs(segment->len))) {
    printf("BAD CHECKSUM");
    return;
  }*/


  /* Deal with ACK */
  if ((segment->flags & TH_ACK) && (len - SEGSIZE == 0)) {

    /* received ack */
    state->send_seq = ntohl(segment->ackno);

    state->segment_lastsent = state->last_seg_sent->object;



    /* check ACK num to see if expected */
    if(state->send_seq == ntohl(state->segment_lastsent->seqno)
                          + ntohs(state->segment_lastsent->len)
                          - SEGSIZE){

      /* ACK is good, confirm my last sent segment */
      state->max_seg_acked = state->last_seg_sent;


      /* Reset timeouts */
      state->lastsent_timeout = -1;
      state->retransmit_counter = 0;

    } else {
      /* ACK is NOT EXPECTED */
      printf("WRONG ACK, keep waiting. \n");
      printf("EXPECTED ACK with ackno %i\n",ntohl(state->segment_lastsent->seqno)+ ntohs(state->segment_lastsent->len)-SEGSIZE);
      printf("Received ACK with ackno %i\n",ntohl(segment->ackno));
      return;
    }
  }

  /* Deal with FIN */
  if ((segment->flags & TH_FIN) && (len - SEGSIZE == 0)) {
    /* Tear down connection */
    ctcp_destroy(state);
    return;
  }


  /* Deal with Data packet */
  if (len - SEGSIZE != 0) {

    /* Save segment */
    state->segment_lastreceived = segment;
    state->recv_ack = ntohl(segment->seqno) + ntohs(segment->len) - SEGSIZE;
    ctcp_makeseg_send(state, SEND_ACK);
    ctcp_output(state);

  }


}

void ctcp_output(ctcp_state_t *state) {
  size_t output_available = conn_bufspace(state->conn);
  size_t data_available = ntohs(state->segment_lastreceived->len) - 20;
  size_t output_size;
  if(output_available < data_available)
    output_size = output_available;
  else
    output_size = data_available;
  conn_output(state->conn, state->segment_lastreceived->data, output_size);
}

void ctcp_timer() {
  if(state_list!=NULL){
    if(state_list->lastsent_timeout!=-1){ /* It's counting! */

      long time_dif = current_time() - state_list->lastsent_timeout;

  //    if(time_dif>state_list->timeout_standard){ /* TIMEOUT */
      if(time_dif>1000){ /* TIMEOUT */
        if(state_list->retransmit_counter >= 5){ /* Disconnect after 5 tries */
          ctcp_destroy(state_list);
          return;
        }

        /* counter++ */
        state_list->retransmit_counter++;

        printf("RETRANSMITTING \n");
        /* call send */
        state_list->lastsent_timeout = current_time();
        ctcp_send_data(state_list,1);
      }

    }
  }
}
