#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include "sr_arpcache.h"
#include "sr_router.h"
#include "sr_if.h"
#include "sr_rt.h"
#include "sr_protocol.h"
#include "sr_utils.h"

void sr_arp_make_packet(struct sr_instance* sr,struct sr_if* interface,
				  const unsigned char* tha,uint32_t target_ip, int mode){
          /* mode 0 is reply, 1 is request */
  printf("preparing an arp ");
  /*a size 8 int array of size len (header size ether+arp) allocated*/
  unsigned int len = sizeof(sr_ethernet_hdr_t)+sizeof(sr_arp_hdr_t);
  uint8_t *packet = (uint8_t*)malloc(len);
/* setting up the ethernet header*/
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t*)(packet);
  memcpy(eth_hdr->ether_shost,interface->addr,6);
/* specify that it is an arp packets */
  eth_hdr->ether_type = htons(ethertype_arp);
/*setting up the arp header, first line is shifting memory to arp*/
  sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t*)(packet+sizeof(sr_ethernet_hdr_t));
  arp_hdr->ar_pro = htons(ethertype_ip);
  arp_hdr->ar_hrd = htons(arp_hrd_ethernet);
  arp_hdr->ar_hln = 0x06;
  arp_hdr->ar_pln = 0x04;
  arp_hdr->ar_sip = interface->ip;
  arp_hdr->ar_tip = target_ip;
  memcpy(arp_hdr->ar_sha,interface->addr,6);

  if (mode){/*request*/
    arp_hdr->ar_op = htons(arp_op_request);
    /*set target Mac to unknown*/
	  memset(arp_hdr->ar_tha,0x00,6);
		memset(eth_hdr->ether_dhost,0xff,6);
		printf("request packet\n");
  }else{/*reply*/
    arp_hdr->ar_op = htons(arp_op_reply);
    /*set target Mac to tha*/
    memcpy(eth_hdr->ether_dhost,tha,6);
    memcpy(arp_hdr->ar_tha,tha,6);
		printf("reply packet\n");
  }
  sr_send_packet(sr,packet,len,interface->name);
}
void sr_arp_request(struct sr_instance* sr,uint32_t target_ip){
  struct sr_rt *nexthop = longestprefixmatch(sr,target_ip);
  if(!nexthop){
    printf("Destination net unreachable\n");
		/*sends icmp net unreachable*/
  }else{
  /*sends packet broadcast on interface*/
    struct sr_if* interface = sr_get_interface(sr,nexthop->interface);
    sr_arp_make_packet(sr, interface, NULL, target_ip, 1);
  }
}

/*
  This function gets called every second. For each request sent out, we keep
  checking whether we should resend an request or destroy the arp request.
  See the comments in the header file for an idea of what it should look like.
*/
void sr_arpcache_sweepreqs(struct sr_instance *sr) {
    /* Fill this in */
    struct sr_arpcache *cache = &sr->cache;
    struct sr_arpreq *req = cache->requests;
    while (req){ /*we have a request*/
			time_t t = time(NULL);
			if(difftime(t,req->sent)>= 1){ /*send request if one hasn't been sent in last second*/
			    if(req->times_sent >= 5){ /*send maximum 5 times*/
		      printf("ARP 5 tries limit reached, unreachable\n");
		      struct sr_packet * packets = req->packets;
					while(packets){
						sr_ip_hdr_t* iphdr = (sr_ip_hdr_t*)(packets->buf + sizeof(sr_ethernet_hdr_t));
		        /*sends icmp host unreachable*/
						sr_icmp_make_packet(sr,iphdr,3,1);
						packets = packets->next;
					}
					sr_arpreq_destroy(cache,req);
					printf("unreachable request destoried\n");
		    }
				else{
					struct sr_arpentry* entry = sr_arpcache_lookup(&sr->cache,req->ip);
					printf("look up cache\n");
					print_addr_ip_int(req->ip);
					if(entry){
					    printf("found new cached ip in queue\n");
					    struct sr_packet *packet = req->packets;
					    struct sr_if* interface= 0;
					    while(packet){
					      interface = sr_get_interface(sr,packet->iface);
								sr_handlepacket(sr,packet->buf,packet->len,interface->name);
					      packet = packet->next;
					    }
					    sr_arpreq_destroy(cache,req);
					    printf("request destoried\n");

					}else{
						printf("mac address not found, requesting via arp\n");
			      sr_arp_request(sr,req->ip);
			      req->times_sent++;
						req->sent = t;
					}
		    }
			}
			 req = req->next;
		}
}

/* You should not need to touch the rest of this code. */

/* Checks if an IP->MAC mapping is in the cache. IP is in network byte order.
   You must free the returned structure if it is not NULL. */
struct sr_arpentry *sr_arpcache_lookup(struct sr_arpcache *cache, uint32_t ip) {
    pthread_mutex_lock(&(cache->lock));

    struct sr_arpentry *entry = NULL, *copy = NULL;

    int i;
		printf("inside lookup\n");
    for (i = 0; i < SR_ARPCACHE_SZ; i++) {
			if (cache->entries[i].ip){
      	print_addr_ip_int(cache->entries[i].ip);
			}
      if ((cache->entries[i].valid) && (cache->entries[i].ip == ip)) {
					printf("yeah\n");

            entry = &(cache->entries[i]);

			}
    }

    /* Must return a copy b/c another thread could jump in and modify
       table after we return. */
    if (entry) {
        copy = (struct sr_arpentry *) malloc(sizeof(struct sr_arpentry));
        memcpy(copy, entry, sizeof(struct sr_arpentry));
    }

    pthread_mutex_unlock(&(cache->lock));

    return copy;
}

/* Adds an ARP request to the ARP request queue. If the request is already on
   the queue, adds the packet to the linked list of packets for this sr_arpreq
   that corresponds to this ARP request. You should free the passed *packet.

   A pointer to the ARP request is returned; it should not be freed. The caller
   can remove the ARP request from the queue by calling sr_arpreq_destroy. */
struct sr_arpreq *sr_arpcache_queuereq(struct sr_arpcache *cache,
                                       uint32_t ip,
                                       uint8_t *packet,           /* borrowed */
                                       unsigned int packet_len,
                                       char *iface)
{
    pthread_mutex_lock(&(cache->lock));

    struct sr_arpreq *req;
    for (req = cache->requests; req != NULL; req = req->next) {
        if (req->ip == ip) {
            break;
        }
    }

    /* If the IP wasn't found, add it */
    if (!req) {
        req = (struct sr_arpreq *) calloc(1, sizeof(struct sr_arpreq));
        req->ip = ip;
        req->next = cache->requests;
        cache->requests = req;
    }

    /* Add the packet to the list of packets for this request */
    if (packet && packet_len && iface) {
        struct sr_packet *new_pkt = (struct sr_packet *)malloc(sizeof(struct sr_packet));

        new_pkt->buf = (uint8_t *)malloc(packet_len);
        memcpy(new_pkt->buf, packet, packet_len);
        new_pkt->len = packet_len;
		new_pkt->iface = (char *)malloc(sr_IFACE_NAMELEN);
        strncpy(new_pkt->iface, iface, sr_IFACE_NAMELEN);
        new_pkt->next = req->packets;
        req->packets = new_pkt;
    }

    pthread_mutex_unlock(&(cache->lock));

    return req;
}

/* This method performs two functions:
   1) Looks up this IP in the request queue. If it is found, returns a pointer
      to the sr_arpreq with this IP. Otherwise, returns NULL.
   2) Inserts this IP to MAC mapping in the cache, and marks it valid. */
struct sr_arpreq *sr_arpcache_insert(struct sr_arpcache *cache,
                                     unsigned char *mac,
                                     uint32_t ip)
{
    pthread_mutex_lock(&(cache->lock));

    struct sr_arpreq *req, *prev = NULL, *next = NULL;
    for (req = cache->requests; req != NULL; req = req->next) {
        if (req->ip == ip) {
            if (prev) {
                next = req->next;
                prev->next = next;
            }
            else {
                next = req->next;
                cache->requests = next;
            }

            break;
        }
        prev = req;
    }

    int i;
    for (i = 0; i < SR_ARPCACHE_SZ; i++) {
        if (!(cache->entries[i].valid))
            break;
    }

    if (i != SR_ARPCACHE_SZ) {
        memcpy(cache->entries[i].mac, mac, 6);
        cache->entries[i].ip = ip;
        cache->entries[i].added = time(NULL);
        cache->entries[i].valid = 1;
    }

    pthread_mutex_unlock(&(cache->lock));

    return req;
}

/* Frees all memory associated with this arp request entry. If this arp request
   entry is on the arp request queue, it is removed from the queue. */
void sr_arpreq_destroy(struct sr_arpcache *cache, struct sr_arpreq *entry) {
    pthread_mutex_lock(&(cache->lock));

    if (entry) {
        struct sr_arpreq *req, *prev = NULL, *next = NULL;
        for (req = cache->requests; req != NULL; req = req->next) {
            if (req == entry) {
                if (prev) {
                    next = req->next;
                    prev->next = next;
                }
                else {
                    next = req->next;
                    cache->requests = next;
                }

                break;
            }
            prev = req;
        }

        struct sr_packet *pkt, *nxt;

        for (pkt = entry->packets; pkt; pkt = nxt) {
            nxt = pkt->next;
            if (pkt->buf)
                free(pkt->buf);
            if (pkt->iface)
                free(pkt->iface);
            free(pkt);
        }

        free(entry);
    }

    pthread_mutex_unlock(&(cache->lock));
}

/* Prints out the ARP table. */
void sr_arpcache_dump(struct sr_arpcache *cache) {
    fprintf(stderr, "\nMAC            IP         ADDED                      VALID\n");
    fprintf(stderr, "-----------------------------------------------------------\n");

    int i;
    for (i = 0; i < SR_ARPCACHE_SZ; i++) {
        struct sr_arpentry *cur = &(cache->entries[i]);
        unsigned char *mac = cur->mac;
        fprintf(stderr, "%.1x%.1x%.1x%.1x%.1x%.1x   %.8x   %.24s   %d\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ntohl(cur->ip), ctime(&(cur->added)), cur->valid);
    }

    fprintf(stderr, "\n");
}

/* Initialize table + table lock. Returns 0 on success. */
int sr_arpcache_init(struct sr_arpcache *cache) {
    /* Seed RNG to kick out a random entry if all entries full. */
    srand(time(NULL));

    /* Invalidate all entries */
    memset(cache->entries, 0, sizeof(cache->entries));
    cache->requests = NULL;

    /* Acquire mutex lock */
    pthread_mutexattr_init(&(cache->attr));
    pthread_mutexattr_settype(&(cache->attr), PTHREAD_MUTEX_RECURSIVE);
    int success = pthread_mutex_init(&(cache->lock), &(cache->attr));

    return success;
}

/* Destroys table + table lock. Returns 0 on success. */
int sr_arpcache_destroy(struct sr_arpcache *cache) {
    return pthread_mutex_destroy(&(cache->lock)) && pthread_mutexattr_destroy(&(cache->attr));
}

/* Thread which sweeps through the cache and invalidates entries that were added
   more than SR_ARPCACHE_TO seconds ago. */
void *sr_arpcache_timeout(void *sr_ptr) {
    struct sr_instance *sr = sr_ptr;
    struct sr_arpcache *cache = &(sr->cache);

    while (1) {
        sleep(1.0);

        pthread_mutex_lock(&(cache->lock));

        time_t curtime = time(NULL);

        int i;
        for (i = 0; i < SR_ARPCACHE_SZ; i++) {
            if ((cache->entries[i].valid) && (difftime(curtime,cache->entries[i].added) > SR_ARPCACHE_TO)) {
                cache->entries[i].valid = 0;
            }
        }

        sr_arpcache_sweepreqs(sr);

        pthread_mutex_unlock(&(cache->lock));
    }

    return NULL;
}
