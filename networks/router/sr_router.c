/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

struct sr_icmp_t11_hdr {
  uint8_t icmp_type;
  uint8_t icmp_code;
  uint16_t icmp_sum;
  uint32_t unused;
  uint8_t data[ICMP_DATA_SIZE];

} __attribute__ ((packed)) ;
typedef struct sr_icmp_t11_hdr sr_icmp_t11_hdr_t;

/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);

    /* Add initialization code here! */

} /* -- sr_init -- */

/* =====================================================
   longestprefixmatch
   Executes longest prefix match in order to determine
   a packet's forwarding interface, given the packet's
   target IP.
   ===================================================== */
struct sr_rt* longestprefixmatch(struct sr_instance* sr,uint32_t target_ip){
 struct sr_rt* next = sr->routing_table;
 struct sr_rt* nexthop = 0;
 uint32_t mask,longest = 0;
 while(next){ /*loop through the routing table*/
   mask = next->mask.s_addr;
   if((mask&next->dest.s_addr)==(mask&target_ip)){
        /*pairwise or to determine our forwarding interface*/
       if(!nexthop||(mask > longest)){
         nexthop = next;
         longest = mask;
       }
   }
   next = next->next;
 }
 return nexthop;
}

/* =====================================================
   sr_icmp_make_packet
   Used to create and send an ICMP message, function takes
   as parameters the type and code of the message so that
   we can send exactly what we need to given the circumstance.
   ===================================================== */
void sr_icmp_make_packet(struct sr_instance* sr,sr_ip_hdr_t* siphdr,
  uint8_t type, uint8_t code){
 /* calculate length of icmp packet */
  uint16_t ip_len = ntohs(siphdr->ip_len);
  unsigned int len;
  if (type == 0){
    printf("sending icmp echo reply\n");
    len = sizeof(sr_ethernet_hdr_t)+ip_len;
  }else if (type == 11){
    printf("sending icmp time to live exceeded\n");
    len = sizeof(sr_ethernet_hdr_t)+sizeof(sr_ip_hdr_t)+
	   sizeof(sr_icmp_t11_hdr_t);
  }else if (type == 3){
    len = sizeof(sr_ethernet_hdr_t)+sizeof(sr_ip_hdr_t)+
	   sizeof(sr_icmp_t3_hdr_t);
  }
  else{
    printf("icmp type not supported\n");
    return;
  }

  /*gets next hop*/
  struct sr_rt *nexthop = longestprefixmatch(sr,siphdr->ip_src);
  if(!nexthop){
    printf("Destination net unreachable\n");
    return;
  }
  struct sr_if* interface = sr_get_interface(sr,nexthop->interface);

  /*Create our ethernet frame*/
  uint8_t* packet = (uint8_t*)malloc(len);
  sr_ethernet_hdr_t* eth_hdr =(sr_ethernet_hdr_t*)(packet);
  eth_hdr->ether_type = htons(ethertype_ip);
	memset(eth_hdr->ether_shost,0x00,6);
	memset(eth_hdr->ether_dhost,0x00,6);

  /*Initialize IP datagram header*/
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t*)(packet+sizeof(sr_ethernet_hdr_t));
	ip_hdr->ip_sum = 0;

  if (type == 0){ /*Echo Reply*/
    memcpy(ip_hdr,siphdr,ip_len);
    ip_hdr->ip_dst = siphdr->ip_src;
    ip_hdr->ip_src = siphdr->ip_dst;
    ip_hdr->ip_sum = cksum(packet+sizeof(sr_ethernet_hdr_t),sizeof(sr_ip_hdr_t));

    /*ICMP part*/
    sr_icmp_hdr_t *icmp_hdr = (sr_icmp_hdr_t*)(packet+sizeof(sr_ethernet_hdr_t)
      +sizeof(sr_ip_hdr_t));
    icmp_hdr->icmp_type = type;
    icmp_hdr->icmp_code = code;
    icmp_hdr->icmp_sum  = 0;
    icmp_hdr->icmp_sum  = cksum((uint8_t*)icmp_hdr,ip_len-sizeof(sr_ip_hdr_t));

  }else{ /*Set all values of IP datagram*/
    ip_hdr->ip_len = htons(len-sizeof(sr_ethernet_hdr_t));
    ip_hdr->ip_hl = 5;
  	ip_hdr->ip_v = 4;
  	ip_hdr->ip_id = siphdr->ip_id;
    ip_hdr->ip_tos = siphdr->ip_tos;
  	ip_hdr->ip_off = 0;
  	ip_hdr->ip_ttl = 64;
  	ip_hdr->ip_p = 1;
    ip_hdr->ip_dst = siphdr->ip_src;
    if(code==3) /*meaning (traceroute), thus replying my own ip */
      ip_hdr->ip_src = siphdr->ip_dst;
  	else
      ip_hdr->ip_src = interface->ip;
    if (type == 3){ /*ICMP: Destination Unreachable*/
      ip_hdr->ip_sum = cksum((uint8_t*)ip_hdr,sizeof(sr_ip_hdr_t));
      /*ICMP part*/
        sr_icmp_t3_hdr_t *icmp_hdr = (sr_icmp_t3_hdr_t*)((uint8_t*)ip_hdr+sizeof(sr_ip_hdr_t));
        icmp_hdr->icmp_type = type;
        icmp_hdr->icmp_code = code;
        memcpy(icmp_hdr->data,(uint8_t*)siphdr,ICMP_DATA_SIZE);
        icmp_hdr->icmp_sum  = 0;
        icmp_hdr->icmp_sum  = cksum((uint8_t*)icmp_hdr,len
                              -sizeof(sr_ethernet_hdr_t)-sizeof(sr_ip_hdr_t));

      /*Determine code of Dest Unreachable message*/
      if(code==0){
        printf("sending icmp net unreachable\n");
      }
      else if(code==1){
        printf("sending icmp host unreachable\n");
      }
      else if(code==3){
        printf("sending icmp port unreachable\n");
      }
    }else{
      ip_hdr->ip_sum = cksum(packet+sizeof(sr_ethernet_hdr_t),sizeof(sr_ip_hdr_t));
      /*ICMP part*/
        sr_icmp_t11_hdr_t *icmp_hdr = (sr_icmp_t11_hdr_t*)(packet+sizeof(sr_ethernet_hdr_t)+sizeof(sr_ip_hdr_t));
        icmp_hdr->icmp_type = type;
        icmp_hdr->icmp_code = code;
        memcpy(icmp_hdr->data,(uint8_t*)siphdr,ICMP_DATA_SIZE);
        icmp_hdr->icmp_sum  = 0;
        icmp_hdr->icmp_sum  = cksum((uint8_t*)icmp_hdr,len
                              -sizeof(sr_ethernet_hdr_t)-sizeof(sr_ip_hdr_t));
    }
  }
  printf("made packet forwarded\n" );
  sr_ForwardPacket(sr,packet,nexthop->gw.s_addr,len,interface);
}
/* =====================================================
   IPcheck
   Determines whether a packet's target IP matches one of
   our router's IPs, if so, returns interface.
   ===================================================== */
struct sr_if* IPcheck(uint32_t tar, struct sr_instance* sr){
  struct sr_if* if_walker = 0;
  if_walker = sr->if_list;

  while(if_walker != NULL){ /*iterate through the interface linked list*/
    if(tar == if_walker->ip){
      return if_walker; /*Found a match, return the interface*/
    }
    if_walker = if_walker->next;
  }
  return 0;
}
/* =====================================================
    ip_checksum
    Computes checksum of IP datagram, given IP header.
   ===================================================== */
int ip_checksum(sr_ip_hdr_t *iphdr){
	uint16_t recved = iphdr->ip_sum;
	iphdr->ip_sum = 0;
  uint16_t expected = cksum(iphdr, sizeof(sr_ip_hdr_t));
  return recved == expected;
}
/* =====================================================
   icmp_checksum
   Computes checksum of ICMP packet, given ICMP header.
   ===================================================== */
int icmp_checksum(sr_icmp_hdr_t *icmp_hdr,sr_ip_hdr_t *iphdr){
	uint16_t recved = icmp_hdr->icmp_sum;
	icmp_hdr->icmp_sum = 0;
  uint16_t expected = cksum(icmp_hdr, ntohs(iphdr->ip_len)-sizeof(sr_ip_hdr_t));
  return recved == expected;
}
/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

 void sr_handlepacket(struct sr_instance* sr,
         uint8_t * packet/* lent */,
         unsigned int len,
         char* interface/* lent */)
 {
   /* REQUIRES */
   assert(sr);
   assert(packet);
   assert(interface);

   printf("*** -> Received packet of length %d \n",len);

   /*ARP packet (arp ethertype is 0x0806 (2054 in dec))*/
   if(ethertype(packet) == 2054){
     printf("got an arp ");
     if (len < (sizeof(sr_ethernet_hdr_t)+sizeof(sr_arp_hdr_t))) /*validate  ARP packet size*/
       fprintf(stderr, "Corruption: packet too small\n");
     else{
       sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t*)(packet+sizeof(sr_ethernet_hdr_t));
       uint32_t target_ip = arp_hdr->ar_tip;
       struct sr_if *found_interface = IPcheck(target_ip,sr);

       if(ntohs(arp_hdr->ar_op) == 2){ /*ARP reply*/
         printf("reply packet.\n");
         if(found_interface){
           if (strncmp((const char*)found_interface->addr,
             (const char*)arp_hdr->ar_tha,ETHER_ADDR_LEN)){
             printf("bad mac\n");
             return;
           }
           sr_arpcacheinsert(&sr->cache,arp_hdr->ar_sha,arp_hdr->ar_sip);
         }else{
           struct sr_rt *nexthop = longestprefixmatch(sr,target_ip);
           if(!nexthop){
             printf("Destination net unreachable\n");
             return;
           }
           struct sr_if* sinterface = sr_get_interface(sr,interface);
           printf("reply forwarded to nexthop\n");
           uint8_t* buf = (uint8_t*)malloc(len);
           memcpy(buf, packet, len);
           sr_ForwardPacket(sr,buf,nexthop->gw.s_addr,len,sinterface);
         }
       }
       else if(ntohs(arp_hdr->ar_op) == 1){ /*ARP request*/
         printf("request packet .\n");
         struct sr_if* sinterface = sr_get_interface(sr,interface);
         if(found_interface){ /*tar ip is one of ours*/
           /*send ARP reply, return*/
           sr_arp_make_packet(sr, sinterface, arp_hdr->ar_sha, arp_hdr->ar_sip, 0);
           sr_arpcacheinsert(&sr->cache,arp_hdr->ar_sha,arp_hdr->ar_sip);
          }else{ /*target IP not ours*/
            struct sr_arpentry* entry = sr_arpcache_lookup(&sr->cache,target_ip);
            if (entry){
              sr_arp_make_packet(sr, sinterface, entry->mac, arp_hdr->ar_sip, 0);
            /*forward ARP request (using routing table), return*/
              free(entry);
            }
            else{
              struct sr_rt *nexthop = longestprefixmatch(sr,target_ip);
              if(!nexthop){
                printf("Destination net unreachable\n");
                return;
              }
              printf("arp req mac address not found, request queued\n");
              uint8_t* buf = (uint8_t*)malloc(len);
              memcpy(buf, packet, len);
              sr_arpcache_queuereq(&sr->cache,nexthop->gw.s_addr,buf,
                sizeof(sr_ethernet_hdr_t)+sizeof(sr_arp_hdr_t),nexthop->interface);
            }
          }
        }
        else
          return;
      }
  }
   else if(ethertype(packet) == ethertype_ip){ /*IP Packet*/
     printf("got an ip ");
     if(len >= 20){ /*valid size (large enough to hold an IP header)*/
       sr_ip_hdr_t *iphdr = (sr_ip_hdr_t *)(packet+sizeof(sr_ethernet_hdr_t)); /*access IP header*/
       if (!ip_checksum(iphdr)){ /*Recompute checksum before we do anything*/
         fprintf(stderr,"Corruption: bad ip checksum\n");
         return;
       }
       struct sr_if *found_interface = IPcheck(iphdr->ip_dst,sr);
       if (found_interface){
         if(iphdr->ip_p == 1){ /* ICMP protocol */
           printf("icmp packet \n");
           sr_icmp_hdr_t *icmp_hdr =(sr_icmp_hdr_t*)(packet
             +sizeof(sr_ethernet_hdr_t)+sizeof(sr_ip_hdr_t));
             if (!icmp_checksum(icmp_hdr,iphdr)){
               fprintf(stderr,"Corruption: bad icmp checksum\n");
               return;
             }
             if(icmp_hdr->icmp_type==8){ /*Ping request*/
               sr_icmp_make_packet(sr,iphdr, 0, 0);
             }
          }
         /*DETECT for UDP or TCP payload*/
         else if(iphdr->ip_p == 17 || iphdr->ip_p == 6){ /*detect ip protocol*/
             sr_icmp_make_packet(sr,iphdr, 3, 3); /*ICMP: Port Unreachable*/
         }
     }else{
       if(iphdr->ip_ttl == 1){ /*ICMP time exceeded*/
         sr_icmp_make_packet(sr,iphdr, 11, 0);
         return;
       }
       iphdr->ip_ttl-=2; /*decrement TTL*/
       iphdr->ip_sum = 0;
       iphdr->ip_sum = cksum(iphdr,iphdr->ip_hl*4);
       struct sr_rt *nexthop = longestprefixmatch(sr,iphdr->ip_dst);
       if(!nexthop){
         printf("no match in LPM,send net unreachable\n");
         sr_icmp_make_packet(sr,iphdr, 3, 0);
       }else{
         struct sr_if* sinterface = sr_get_interface(sr,nexthop->interface);
         printf("ip forwarded to nexthop\n");
         uint8_t* buf = (uint8_t*)malloc(len);
         memcpy(buf, packet, len);
         sr_ForwardPacket(sr,buf,nexthop->gw.s_addr,len,sinterface);
       }
     }
   }
 }
}

/* =====================================================
    sr_arpcache_insert
    Iterates through and adds to our ARP cache.
   ===================================================== */
void sr_arpcacheinsert(struct sr_arpcache *cache, unsigned char *mac,uint32_t ip){
  /*insert mac into cache */
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
      printf("ip cached\n" );
      print_addr_ip_int(cache->entries[i].ip);
  }
}

/* =====================================================
    sr_ForwardPacket
    Forwards packet to the interface determined by
    longestprefixmatch.
   ===================================================== */
void sr_ForwardPacket(struct sr_instance* sr,uint8_t* packet,
  uint32_t nexthop_ip,unsigned int len, struct sr_if* interface){

  struct sr_arpentry* entry = sr_arpcache_lookup(&sr->cache,nexthop_ip);
  if(entry){
    sr_ethernet_hdr_t* eth_hdr =(sr_ethernet_hdr_t*)(packet);
    memcpy(eth_hdr->ether_shost,interface->addr,6);
    memcpy(eth_hdr->ether_dhost,entry->mac,6);
    printf("%s \n",interface->name);
    sr_send_packet(sr,packet,len,interface->name);
    free(entry);
  }
  else{
    printf("forward mac address not found, request queued\n");
    printf("nexthop_ip\n");
    print_addr_ip_int(nexthop_ip);
    sr_arpcache_queuereq(&sr->cache,nexthop_ip,packet,len,interface->name);
  }
}
/* end sr_ForwardPacket */
