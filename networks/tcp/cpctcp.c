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
//how did stop and wait implement wait until getting an ack
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
//defining modes
#define SEND_DATA 0
#define SEND_FIN 1
#define SEND_ACK 2
#define SEND_FIN_ACK 3

int ctcp_makeseg_send(ctcp_state_t *state, int mode);
int ctcp_send_data(ctcp_state_t *state, bool resend);


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

  linked_list_t *send_segments;  /* Segments sent to this connection. */

  linked_list_t *recv_segments; /* Buffer received segments */


  char *send_buf;//buffer
  int send_buf_len;//buffer length

  /* SENDER */
  int send_win_size;
  int send_win_index;
  int send_seq; /* acknowledged max segment number */
  int send_window_start; /* Send window start index */
  long lastsent_timeout; /* Timeout for the last packet */
  long timeout_standard; // standard timeout time
  int retransmit_counter; // counter for retransmit time max 5

  ll_node_t *max_seg_acked;//for recording acked segment
  ll_node_t *last_seg_sent;//for recording sent segment

  /* RECEIVER */
  int recv_win_size;
  int recv_ack; /* ACK number to Acknowledge */
  int recv_window_start;/* receive window start index */



};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;


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

  state->recv_segments = ll_create();

  state->send_segments = ll_create();
  state->max_seg_acked = NULL;
  state->last_seg_sent = NULL;

  state->lastsent_timeout = -1; /* Timeout for the last packet */
  state->timeout_standard = cfg->rt_timeout;
  state->retransmit_counter = 0;

  state->send_window_start = 1;
  state->send_seq = 1;
  state->send_win_size = cfg->send_window / MAX_SEG_DATA_SIZE;
  state->send_win_index = 0;

  state->recv_ack = 1;
  state->recv_window_start = 1;
  state->recv_win_size = cfg->recv_window / MAX_SEG_DATA_SIZE;

  state_list = state;

  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);



  free(state->send_buf);
  ll_destroy(state->recv_segments);

  if(state->last_seg_sent!=NULL){
    free(state->last_seg_sent->object);
    ll_remove(state->send_segments,state->last_seg_sent);
  }

  ll_destroy(state->send_segments);

  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* Get input */
  // not the case that no file is received
  state->send_buf_len = conn_input(state->conn,state->send_buf,MAX_SEG_DATA_SIZE);
  if(state->send_buf_len>=0){
    /* Make segment and add to queue */
    printf("read data\n" );
    ctcp_makeseg_send(state,SEND_DATA);

  } else {
    //get EOF
    state->send_buf_len = 0;
    ctcp_makeseg_send(state,SEND_FIN);
  }
}

int ctcp_makeseg_send(ctcp_state_t *state, int mode) {
  //total seg length
  int seglen = SEGSIZE + state->send_buf_len;
  ctcp_segment_t *segment = malloc(seglen);

  /* initialize segment headers */
  segment->len    = htons(seglen);
  segment->window = htons(MAX_SEG_DATA_SIZE * state->recv_win_size);
  segment->cksum  = htons(0x00);
  segment->seqno  = htonl(state->send_seq);
  segment->ackno  = htonl(state->recv_ack);

  /* Set data field accordingly */
  if(mode==SEND_DATA){

    segment->flags  = htonl(0x018);
    memcpy(segment->data, state->send_buf, state->send_buf_len);

    /* Clear out send buffer */
    strcpy(state->send_buf,"");
    state->send_buf_len = 0;

    /* Queue the segment to be sent */
    ll_add(state->send_segments,segment);
    printf("data\n" );

    /* Call send_data to send if available */
    ctcp_send_data(state,0);

    return 0;
  }

  if(mode==SEND_ACK){

    segment->flags  = TH_ACK;
    segment->cksum  = cksum(segment, seglen);
  }

  if(mode==SEND_FIN){

    segment->flags  = TH_FIN;

    /* Send FIN over our data channel */
    ll_add(state->send_segments,segment);

    /* Call send_data to send if available */
    ctcp_send_data(state,0);
    return 0;
  }


  if(mode==SEND_FIN_ACK){
    segment->flags  = htonl(0x11);
    segment->cksum  = cksum(segment, seglen);
  }

  segment->cksum  = cksum(segment, seglen);
  /* send directly */
  conn_send(state->conn, segment, seglen);

  return 0;
}


/* check send window and send data accordingly */
int ctcp_send_data(ctcp_state_t *state, bool resend){

  ll_node_t *nod = ll_front(state->send_segments);
  ctcp_segment_t *segment;
  int window_index = 0;

  while (nod!=NULL && window_index<state->send_win_size){
    segment = nod->object;

    if(segment->cksum==htons(0x00)){
      /* sent new data only */
      /* set ackno again to sync with receiving side */
      segment->ackno = htonl(state->recv_ack);
      segment->cksum = cksum(segment, ntohs(segment->len));
      conn_send(state->conn, segment, ntohs(segment->len));
      state->lastsent_timeout = current_time();
      
    } else if(resend) {
      /* resending already sent data */
      /* set ackno again to sync with receiving side */
      segment->cksum  = htons(0x00);
      segment->ackno = htonl(state->recv_ack);
      segment->cksum = cksum(segment, ntohs(segment->len));
      conn_send(state->conn, segment, ntohs(segment->len));
      state->lastsent_timeout = current_time();
    }

    nod = nod->next;
    window_index++;
  }

  /* Record time */

  return 0;
}




bool checksum(ctcp_segment_t *segment, size_t len){

	uint16_t recved = segment->cksum;
	segment->cksum = htons(0x00);
  uint16_t expected = cksum(segment, len);

  return recved == expected;
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {

  /* Check integrity */
  if (!checksum(segment, ntohs(segment->len))) {
    return;
  }

  /* Deal with Data packet */
  else if (len - SEGSIZE != 0) {
    printf("got packet\n" );
    if(ntohl(segment->seqno)!=state->recv_ack){
      /* Duplicate or previous segments */
      ctcp_segment_t *temp_seg = ll_back(state->recv_segments)->object;
      state->recv_ack = temp_seg->seqno;
      ctcp_makeseg_send(state, SEND_ACK);
    }
    else {
      /* Save segment */
      ll_add(state->recv_segments,segment);
      state->recv_ack = ntohl(segment->seqno) + ntohs(segment->len) - SEGSIZE;

      /* Send ACK */
      ctcp_makeseg_send(state, SEND_ACK);
      ctcp_output(state);

      return;
    }
  }


  /* Deal with ACK */
  else if ((segment->flags & TH_ACK) && (len - SEGSIZE == 0)) {
    printf("got ack\n" );
    ll_node_t *inorder_seg = ll_front(state->send_segments);
    ctcp_segment_t *temp_seg = inorder_seg->object;
    state->send_seq = ntohl(segment->ackno);

    /* check ACK num to see if expected */
    if(state->send_seq == ntohl(temp_seg->seqno)
                          + ntohs(temp_seg->len)
                          - SEGSIZE){

      /* ACK is good, confirm my last sent segment */
      //state->max_seg_acked = inorder_seg;

      /* Reset timeouts */
      state->lastsent_timeout = -1;
      state->retransmit_counter = 0;
      free(inorder_seg->object);
      ll_remove(state->send_segments,inorder_seg);
      /* Clear buffered segment before this segment */
//      if(state->last_seg_sent->prev!=NULL){
  //      free(state->last_seg_sent->prev->object);
    //    ll_remove(state->send_segments,state->last_seg_sent->prev);

      /*** Call send_data if there's more data queued ***/
      //ctcp_send_data(state, 0);

    } else {
      /* ACK is NOT EXPECTED, send again */
      printf("bad ack\n" );
      ctcp_send_data(state,0);
      /* Reset timeouts */
      state->lastsent_timeout = -1;
      state->retransmit_counter = 0;
      return;
    }
  }

  /* Deal with FIN */
  else if ((segment->flags & TH_FIN) && (len - SEGSIZE == 0)) {
    /* Send FIN ACK */
    ctcp_makeseg_send(state, SEND_FIN_ACK);

    /* Tear down connection */
    ctcp_destroy(state);
    return;
  }

  /* Deal with FIN ACK */
  else if ((segment->flags == (TH_ACK & TH_FIN) ) && (len - SEGSIZE == 0)) {
    /* Just tear down connection */
    ctcp_destroy(state);
    return;
  }

}


void ctcp_output(ctcp_state_t *state) {
  /* Get how much data should we ouput */
  ll_node_t * nod = ll_back(state->recv_segments);
  ctcp_segment_t *segment = nod->object;

  size_t output_available = conn_bufspace(state->conn);
  size_t data_available = ntohs(segment->len) - 20;
  size_t output_size;
  if(output_available < data_available)
    output_size = output_available;
  else
    output_size = data_available;

  /* Output to the application with a given length */
  conn_output(state->conn, segment->data, output_size);
}



void ctcp_timer() {
  if(state_list!=NULL){
    if(state_list->lastsent_timeout!=-1){ /* It's counting! */

      long time_dif = current_time() - state_list->lastsent_timeout;//clock clicking

      if(time_dif>state_list->timeout_standard){ /* TIMEOUT */

        if(state_list->retransmit_counter >= 5){ /* Disconnect after 5 tries */
          ctcp_destroy(state_list);
          return;
        }

        /* counter++ */
        state_list->retransmit_counter++;

        /* call send */
        state_list->lastsent_timeout = current_time();
        ctcp_send_data(state_list,1);
      }

    }
  }
}
