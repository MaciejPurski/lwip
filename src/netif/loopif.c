/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 * 
 * Author: Adam Dunkels <adam@sics.se>
 *
 */
#include "lwip/opt.h"

#if LWIP_HAVE_LOOPIF

#include "netif/loopif.h"
#include "lwip/pbuf.h"

#include <string.h>

#if !LWIP_LOOPIF_MULTITHREADING

#include "lwip/sys.h"
#include "lwip/mem.h"

/* helper struct for the linked list of pbufs */
struct loopif_private {
  struct pbuf *first;
  struct pbuf *last;
};

/**
 * Call loopif_poll() in the main loop of your application. This is to prevent
 * reentering non-reentrant functions like tcp_input(). Packets passed to
 * loopif_output() are put on a list that is passed to netif->input() by
 * loopif_poll().
 *
 * @param netif the lwip network interface structure for this loopif
 */
void
loopif_poll(struct netif *netif)
{
  SYS_ARCH_DECL_PROTECT(lev);
  struct pbuf *in = NULL;
  struct loopif_private *priv = (struct loopif_private*)netif->state;

  LWIP_ASSERT("priv != NULL", priv != NULL);
  if(priv == NULL) {
    return;
  }

  do {
    /* Get a packet from the list. With SYS_LIGHTWEIGHT_PROT=1, this is protected */
    SYS_ARCH_PROTECT(lev);
    in = priv->first;
    if(priv->first) {
      if(priv->first == priv->last) {
        /* this was the last pbuf in the list */
        priv->first = priv->last = NULL;
      } else {
        /* pop the pbuf off the list */
        priv->first = priv->first->next;
        LWIP_ASSERT("should not be null since first != last!", priv->first != NULL);
      }
    }
    SYS_ARCH_UNPROTECT(lev);
  
    if(in != NULL) {
      if(in->next != NULL) {
        /* De-queue the pbuf from its successors on the 'priv' list. */
        in->next = NULL;
        /* This is built on the assumption that PBUF_RAM pbufs are in one piece! */
        LWIP_ASSERT("packet must not consist of multiple pbufs!", in->len == in->tot_len);
      }
      if(netif->input(in, netif) != ERR_OK) {
        pbuf_free(in);
      }
      /* Don't reference the packet any more! */
      in = NULL;
    }
  /* go on while there is a packet on the list */
  } while(priv->first != NULL);
}
#endif /* LWIP_LOOPIF_MULTITHREADING */

/**
 * Send an IP packet over the loopback interface.
 * The pbuf is simply copied and handed back to netif->input.
 * In multithreaded mode, this is done directly since netif->input must put
 * the packet on a queue.
 * In callback mode, the packet is put on an internal queue and is fed to
 * netif->input by loopif_poll().
 *
 * @param netif the lwip network interface structure for this loopif
 * @param p the (IP) packet to 'send'
 * @param ipaddr the ip address to send the packet to (not used for loopif)
 * @return ERR_OK if the packet has been sent
 *         ERR_MEM if the pbuf used to copy the packet couldn't be allocated
 */
static err_t
loopif_output(struct netif *netif, struct pbuf *p,
       struct ip_addr *ipaddr)
{
#if !LWIP_LOOPIF_MULTITHREADING
  SYS_ARCH_DECL_PROTECT(lev);
  struct loopif_private *priv;
#endif /* LWIP_LOOPIF_MULTITHREADING */
  struct pbuf *q, *r;
  u8_t *ptr;

  LWIP_UNUSED_ARG(ipaddr);

  /* Allocate a new pbuf */
  r = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
  if (r == NULL) {
    return ERR_MEM;
  }

  /* Copy the whole pbuf queue p into the single pbuf r */
  /* @todo: - extend this so that r can be a pbuf list also
   *        - could use pbuf_copy() for that!
   */
  ptr = r->payload;
  for(q = p; q != NULL; q = q->next) {
    MEMCPY(ptr, q->payload, q->len);
    ptr += q->len;
  }

#if LWIP_LOOPIF_MULTITHREADING
  /* Multithreading environment, netif->input() is supposed to put the packet
     into a mailbox, so we can safely call it here without risking to re-enter
     functions that are not reentrant (TCP!!!) */
  if(netif->input(r, netif) != ERR_OK) {
    pbuf_free(r);
    r = NULL;
  }
#else /* LWIP_LOOPIF_MULTITHREADING */
  /* Raw API without threads: put the packet on a linked list which gets emptied
     through calling loopif_poll(). */
  priv = (struct loopif_private*)netif->state;

  /* This is built on the assumption that PBUF_RAM pbufs are in one piece! */
  LWIP_ASSERT("packet must not consist of multiple pbufs!", r->len == r->tot_len);

  SYS_ARCH_PROTECT(lev);
  if(priv->first != NULL) {
    LWIP_ASSERT("if first!=NULL, last must also be != NULL", priv->last != NULL);
    priv->last->next = r;
    priv->last = r;
  } else {
    priv->first = priv->last = r;
  }
  SYS_ARCH_UNPROTECT(lev);
#endif /* LWIP_LOOPIF_MULTITHREADING */

  return ERR_OK;    
}

/**
 * Initialize a lwip network interface structure for a loopback interface
 *
 * @param netif the lwip network interface structure for this loopif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 */
err_t
loopif_init(struct netif *netif)
{
#if !LWIP_LOOPIF_MULTITHREADING
  struct loopif_private *priv;

  priv = (struct loopif_private*)mem_malloc(sizeof(struct loopif_private));
  if(priv == NULL) 
    return ERR_MEM;
  priv->first = priv->last = NULL;
  netif->state = priv;
#endif /* LWIP_LOOPIF_MULTITHREADING */

  netif->name[0] = 'l';
  netif->name[1] = 'o';
  netif->output = loopif_output;
  return ERR_OK;
}

#endif /* LWIP_HAVE_LOOPIF */
