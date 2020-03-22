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
/*-----------------------------------------------------*/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"
#include "unistd.h"
#define SEG_HEADER_SIZE sizeof(ctcp_segment_t)
#define SEND_DATA 0
#define SEND_FIN 1
#define SEND_ACK 2
#define SEND_FIN_ACK 3

int ctcp_makeseg(ctcp_state_t *state, int mode);
int ctcp_send(ctcp_state_t *state);
int ctcp_retrans(ctcp_state_t *state);

void print_segment_list(ctcp_state_t *state);


/* Data structure for send window */
typedef struct send_window {
  linked_list_t *segments;  /* segments sent to this connection. */

  int max_size;
  int size;
  int recv_win_size;

  int seqno;

  long timer;
  int retrans_counter;
} send_window_t;


/* Data structure for receive window */
typedef struct recv_window {
  linked_list_t *segments;  /* segments sent to this connection. */

  int max_size;
  int size;

  int ackno;

} recv_window_t;


/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */

  send_window_t *sw;        /* Send Window         */
  recv_window_t *rw;        /* Receive Window      */

  char *send_buf;
  int send_buf_len;

  long timeout_standard;
  long timer_teardown;
};


/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;


/* =====================================================
   Initialize our state structure for a new connection
   ===================================================== */
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


    /* Set fields. */
    state->conn = conn;
    state->send_buf_len = 0;
    state->send_buf = (char *) malloc(MAX_SEG_DATA_SIZE);
    state->timeout_standard = cfg->rt_timeout;
    state->timer_teardown = -1;


    /* -----------------------------
       SEND WINDOW Data structure
    ------------------------------ */
    state->sw = calloc(sizeof(send_window_t), 1);

    state->sw->segments = ll_create();
    state->sw->max_size = cfg->send_window;
    state->sw->size = 0;
    state->sw->recv_win_size = MAX_SEG_DATA_SIZE;

    state->sw->seqno = 1;

    state->sw->timer = -1;
    state->sw->retrans_counter = 0;


    /* -----------------------------
       RECV WINDOW Data structure
    ----------------------------- */
    state->rw = calloc(sizeof(recv_window_t), 1);

    state->rw->segments = ll_create();
    state->rw->max_size = cfg->recv_window;
    state->rw->size = 0;

    state->rw->ackno = 1;

    state_list = state;
    return state;
}

/* =====================================================
   Tear down connection
   ===================================================== */
void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  ll_destroy(state->sw->segments);
  ll_destroy(state->rw->segments);

  free(state);
  end_client();
}

/* =====================================================
   Read input from application and deliver to our socket
   ===================================================== */
void ctcp_read(ctcp_state_t *state) {
  /* Get available window space */
  int remaining_space = state->sw->max_size - state->sw->size;
  int buffer_size = 0;

  /* Compare to receiver side window size as well */
  if(remaining_space > state->sw->recv_win_size)
    remaining_space = state->sw->recv_win_size;

  /* Compute appropriate buffer size */
  if(remaining_space>=MAX_SEG_DATA_SIZE)
    buffer_size = MAX_SEG_DATA_SIZE;
  else
    buffer_size = remaining_space;

  /* Window Full, wait for next call*/
  if(buffer_size<=0)
    return;

  /* Get data from application */
  state->send_buf_len = conn_input(state->conn,state->send_buf,buffer_size);

  /* Make segment accordingly */
  if(state->send_buf_len >= 0){
    ctcp_makeseg(state,SEND_DATA);
    /* Clear out send buffer */
    strcpy(state->send_buf,"");
    state->send_buf_len = 0;
  } else {
    state->send_buf_len = 0;
    ctcp_makeseg(state,SEND_FIN);
  }
}

/* debugging tool*/
void print_segment_list(ctcp_state_t *state){
  int items = ll_length(state->sw->segments);
  printf("[SWND][%i Items, Size: %i, Seqno: %i, Timer: %ld], RecvWin: %i\n",
  items,state->sw->size,state->sw->seqno,state->sw->timer,state->sw->recv_win_size);
  items = ll_length(state->rw->segments);
  printf("[RWND][%i Items, Size: %i, Ackno: %i]\n",
  items,state->rw->size,state->rw->ackno);
}

/* =====================================================
   Piece together a new segment based on a given option
   ===================================================== */
int ctcp_makeseg(ctcp_state_t *state, int mode) {
  int data_length = state->send_buf_len;
  int segment_length = SEG_HEADER_SIZE + data_length;

  /* initialize segment */
  ctcp_segment_t *segment = malloc(segment_length);

  segment->len    = htons(segment_length);
  segment->window = htons(state->rw->max_size-state->rw->size);
  segment->cksum  = htons(0x00);
  segment->seqno  = htonl(state->sw->seqno);
  segment->ackno  = htonl(state->rw->ackno);

  /* Set data field accordingly */
  if(mode==SEND_DATA){

    segment->flags  = TH_ACK;
    memcpy(segment->data, state->send_buf, data_length);

    /* Queue the segment to be sent */
    ll_add(state->sw->segments,segment);
    /* Add this segment length to window current size*/
    state->sw->size += data_length;
    /* Advance seqno */
    state->sw->seqno += data_length;
    /* Call send_data to send if available */
    ctcp_send(state);
    return 0;
  }

  if(mode==SEND_ACK){
    /* Set flags for ACK */
    segment->flags  = TH_ACK;
    segment->cksum  = cksum(segment, segment_length);
    /* send directly */
    conn_send(state->conn, segment, segment_length);
    return 0;
  }

  if(mode==SEND_FIN){
    /* Set flags for FIN */
    segment->flags  = TH_FIN;
    /* Send FIN over our data channel */
    ll_add(state->sw->segments,segment);
    /* Call send_data to send if available */
    ctcp_send(state);
    return 0;
  }

  if(mode==SEND_FIN_ACK){
    /* Set flags for FIN ACK */
    segment->flags  = htonl(0x11);
    segment->cksum  = cksum(segment, segment_length);
    /* send directly */
    conn_send(state->conn, segment, segment_length);
    return 0;
  }

  return 0;
}

/* =====================================================
   Send new data segment that have never been sent before
   ===================================================== */
int ctcp_send(ctcp_state_t *state){

  /* start from the last segment sent */
  ll_node_t *node = ll_front(state->sw->segments);
  ctcp_segment_t *segment;

  /* set timer for the first segment in window */
  if(node!=NULL){
    state->sw->timer = current_time();
    state->sw->retrans_counter = 0;
  }

  /* loop through window to send new data */
  while(node!=NULL){
    segment = node->object;
    if(segment->cksum==htons(0x00)){
      /* checksum has not been set before */
      segment->ackno = htonl(state->rw->ackno);
      segment->cksum = cksum(segment, ntohs(segment->len));

      conn_send(state->conn, segment, ntohs(segment->len));
    }
    node = node->next;
  }

  return 0;
}

/* =====================================================
   Retransmit the first packet in send window
   ===================================================== */
int ctcp_retrans(ctcp_state_t *state){

  /* start from the first segment in window */
  ll_node_t *node = ll_front(state->sw->segments);
  ctcp_segment_t *segment;

  /* retransmit only the first segment in the window */
  if(node!=NULL){

    segment = node->object;
    segment->ackno = htonl(state->rw->ackno);

    segment->cksum = cksum(segment, ntohs(segment->len));
    state->sw->timer = current_time();

    conn_send(state->conn, segment, ntohs(segment->len));
  }

  return 0;
}


/* =====================================================
   Segment checksum
   ===================================================== */
bool checksum(ctcp_segment_t *segment, size_t len){
	uint16_t recved = segment->cksum;
	segment->cksum = htons(0x00);
  uint16_t expected = cksum(segment, len);
  return recved == expected;
}

/* =====================================================
   Deal with different types of segments received
   ===================================================== */
void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {

  //print_segment_list(state);

  bool force_ack = 0;

  /* 1. Check integrity */
  if (!checksum(segment, ntohs(segment->len))) {
    return;
  }


  /* 2. Set receiver window size */
  state->sw->recv_win_size = ntohs(segment->window);


  /* 3. Received FIN */
  if ((segment->flags & TH_FIN) && (len - SEG_HEADER_SIZE == 0)) {
    /* Send FIN ACK */
    ctcp_makeseg(state, SEND_FIN_ACK);

    if(state->timer_teardown==-1){
      ctcp_makeseg(state, SEND_FIN);
    }
  }


  /* 4. Received FIN ACK */
  if ((segment->flags == (TH_ACK & TH_FIN) ) && (len - SEG_HEADER_SIZE == 0)) {
    /* Just tear down connection */
    force_ack = 1;
    state->timer_teardown = current_time();
  }


  /* 5. Received data segment */
  if (len - SEG_HEADER_SIZE != 0) {
    force_ack = 1;

    /* Got duplicate data segment */
    if(ntohl(segment->seqno) < state->rw->ackno){
      /* Just send ACK */
      ctcp_makeseg(state, SEND_ACK);
    }

    /* Got what we expected */
    if(ntohl(segment->seqno)==state->rw->ackno &&
      len - SEG_HEADER_SIZE <= state->rw->max_size-state->rw->size){
      ll_add(state->rw->segments,segment);
      state->rw->size += ntohs(segment->len) - SEG_HEADER_SIZE;

      /* Get new ackno */
      state->rw->ackno += ntohs(segment->len) - SEG_HEADER_SIZE;
      ll_node_t *node = ll_find(state->rw->segments, segment);
      ctcp_segment_t *next_seg;

      while(node->next!=NULL){
        next_seg = node->next->object;
        if(state->rw->ackno!=next_seg->seqno){
          /* there's a empty space after, stop advancing ackno */
          break;
        } else {
          /* advance ackno */
          state->rw->ackno = next_seg->seqno;
        }
        node = node->next;
      }

      /* Send ACK */
      ctcp_makeseg(state, SEND_ACK);
      ctcp_output(state);
    }

    /* Got out of order data segments for the FUTURE */
    if(ntohl(segment->seqno) > state->rw->ackno &&
       len - SEG_HEADER_SIZE <= state->rw->max_size-state->rw->size){

      ll_node_t *node = ll_front(state->rw->segments);
      ctcp_segment_t *next_seg;
      bool in_the_back = 0;

      /* empty receive window */
      if(node==NULL){
        ll_add(state->rw->segments,segment);
      }

      /* traverse receive window */
      while(node->next!=NULL){
        next_seg = node->next->object;
        if(ntohl(segment->seqno) < next_seg->seqno){
          /* insert segment to correct place */
          ll_add_after(state->rw->segments, node, segment);
          in_the_back = 1;
          break;
        }
        node = node->next;
      }

      /* insert to the back of the send window */
      if(in_the_back){
        ll_add(state->rw->segments,segment);
      }

      state->rw->size += ntohs(segment->len) - SEG_HEADER_SIZE;
    }
    return;
  }


  /* 6. Received ACK or Piggybacked ACK from data segments */
  if ((segment->flags & TH_ACK)||force_ack) {

    ll_node_t *node = ll_front(state->sw->segments);
    ll_node_t *temp_node;

    if(node==NULL) return; /* ignore ACK if send window is empty */

    /* start loop from the start of the window to confirm segment */
    ctcp_segment_t *expected_segment = node->object;
    int received_ackno = ntohl(segment->ackno);
    int expected_ackno = ntohs(expected_segment->len) - SEG_HEADER_SIZE +
                         ntohl(expected_segment->seqno);

    if(received_ackno >= expected_ackno){

      /* ACK is expected or cumulative */
      while(node!=NULL){
        expected_segment = node->object;
        temp_node = node;
        node = node->next;

        if(ntohs(expected_segment->len) - SEG_HEADER_SIZE +
           ntohl(expected_segment->seqno) <= received_ackno){

          /* confirm my segment, slide the window */
          state->sw->size -= ntohs(expected_segment->len) - SEG_HEADER_SIZE;
          ll_remove(state->sw->segments, temp_node);
          free(expected_segment);
        }
      }

      /* Reset timeouts */
      state->sw->timer = -1;
      state->sw->retrans_counter = 0;

      /* Call send_data if there's more data queued */
      ctcp_send(state);

    }
  }



}

/* =====================================================
   Deliver received in-order data to application
   ===================================================== */
void ctcp_output(ctcp_state_t *state) {

  ll_node_t *node = ll_front(state->rw->segments), *temp_node;
  ctcp_segment_t *segment;
  size_t output_available;
  size_t data_available;
  size_t output_size;

  /* traverse from the start of the send window */
  while(node!=NULL){

    temp_node = node;
    segment = node->object;
    node = node->next;

    /* reached the last segment ready for output*/
    if(ntohl(segment->seqno)>=state->rw->ackno){
      break;
    }

    /* Get how much data should we ouput */
    output_available = conn_bufspace(state->conn);
    data_available = ntohs(segment->len) - SEG_HEADER_SIZE;

    if(output_available < data_available)
      output_size = output_available;
    else
      output_size = data_available;

    /* Output to the application with a given length */
    conn_output(state->conn, segment->data, output_size);
    state->rw->size -= ntohs(segment->len) - SEG_HEADER_SIZE;
    ll_remove(state->rw->segments, temp_node);
    //free(segment);

  }

}


/* =====================================================
   Timer checker
   ===================================================== */
void ctcp_timer() {
  ctcp_state_t *state = state_list;
  while(state!=NULL){
    /* connection teardown timer*/
    if(state->timer_teardown!=-1){
      long close_wait = current_time() - state->timer_teardown;
      if(close_wait > (2*state->timeout_standard)){
        ctcp_destroy(state_list);
        return;
      }
    }

    /* retransmit timer */
    if(state->sw->timer!=-1){
      /* It's counting! */
      long time_dif = current_time() - state->sw->timer;
      if(time_dif > state->timeout_standard){
        /* Timer is UP */
        if(state->sw->retrans_counter >= 5){ /* Disconnect after 5 tries */
          ctcp_destroy(state_list);
          return;
        }
        /* counter++ */
        state->sw->retrans_counter++;
        /* call send */
        state->sw->timer = current_time();
        ctcp_retrans(state);
      }
    }
    state = state->next;
  }
}
