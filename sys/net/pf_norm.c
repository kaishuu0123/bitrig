/*	$OpenBSD: pf_norm.c,v 1.129 2011/03/23 18:34:17 bluhm Exp $ */

/*
 * Copyright 2001 Niels Provos <provos@citi.umich.edu>
 * Copyright 2009 Henning Brauer <henning@openbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "pflog.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/filio.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/kernel.h>
#include <sys/time.h>
#include <sys/pool.h>
#include <sys/syslog.h>

#include <dev/rndvar.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/bpf.h>
#include <net/route.h>
#include <net/if_pflog.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_seq.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>

#ifdef INET6
#include <netinet/ip6.h>
#endif /* INET6 */

#include <net/pfvar.h>

struct pf_frent {
	TAILQ_ENTRY(pf_frent) fr_next;
	struct mbuf	*fe_m;
	u_int16_t	 fe_hdrlen;	/* ipv4 header lenght with ip options
					   ipv6, extension, fragment header */
	u_int16_t	 fe_extoff;	/* last extension header offset or 0 */
	u_int16_t	 fe_len;	/* fragment length */
	u_int16_t	 fe_off;	/* fragment offset */
	u_int16_t	 fe_mff;	/* more fragment flag */
};

/* keep synced with struct pf_fragment, used in RB_FIND */
struct pf_fragment_cmp {
	struct pf_addr	fr_src;
	struct pf_addr	fr_dst;
	u_int32_t	fr_id;
	sa_family_t	fr_af;
	u_int8_t	fr_proto;
	u_int8_t	fr_direction;
};

struct pf_fragment {
	struct pf_addr	fr_src;		/* ip source address */
	struct pf_addr	fr_dst;		/* ip destination address */
	u_int32_t	fr_id;		/* fragment id for reassemble */
	sa_family_t	fr_af;		/* address family */
	u_int8_t	fr_proto;	/* protocol of this fragment */
	u_int8_t	fr_direction;	/* pf packet direction */

	RB_ENTRY(pf_fragment) fr_entry;
	TAILQ_ENTRY(pf_fragment) frag_next;
	u_int32_t	fr_timeout;
	u_int16_t	fr_maxlen;	/* maximum length of single fragment */
	TAILQ_HEAD(pf_fragq, pf_frent) fr_queue;
};

struct pf_fragment_tag {
	u_int16_t	 ft_hdrlen;	/* header lenght of reassembled pkt */
	u_int16_t	 ft_extoff;	/* last extension header offset or 0 */
	u_int16_t	 ft_maxlen;	/* maximum fragment payload length */
};

TAILQ_HEAD(pf_fragqueue, pf_fragment)	pf_fragqueue;

static __inline int	 pf_frag_compare(struct pf_fragment *,
			    struct pf_fragment *);
RB_HEAD(pf_frag_tree, pf_fragment)	pf_frag_tree, pf_cache_tree;
RB_PROTOTYPE(pf_frag_tree, pf_fragment, fr_entry, pf_frag_compare);
RB_GENERATE(pf_frag_tree, pf_fragment, fr_entry, pf_frag_compare);

/* Private prototypes */
void			 pf_remove_fragment(struct pf_fragment *);
void			 pf_flush_fragments(void);
void			 pf_free_fragment(struct pf_fragment *);
struct pf_fragment	*pf_find_fragment(struct pf_fragment_cmp *,
			    struct pf_frag_tree *);
struct pf_frent		*pf_create_fragment(u_short *);
struct pf_fragment	*pf_fillup_fragment(struct pf_fragment_cmp *,
			    struct pf_frent *, u_short *);
int			 pf_isfull_fragment(struct pf_fragment *);
struct mbuf		*pf_join_fragment(struct pf_fragment *);
int			 pf_reassemble(struct mbuf **, struct ip *, int,
			    u_short *);

/* Globals */
struct pool		 pf_frent_pl, pf_frag_pl;
struct pool		 pf_state_scrub_pl;
int			 pf_nfrents;

void
pf_normalize_init(void)
{
	pool_init(&pf_frent_pl, sizeof(struct pf_frent), 0, 0, 0, "pffrent",
	    NULL);
	pool_init(&pf_frag_pl, sizeof(struct pf_fragment), 0, 0, 0, "pffrag",
	    NULL);
	pool_init(&pf_state_scrub_pl, sizeof(struct pf_state_scrub), 0, 0, 0,
	    "pfstscr", NULL);

	pool_sethiwat(&pf_frag_pl, PFFRAG_FRAG_HIWAT);
	pool_sethardlimit(&pf_frent_pl, PFFRAG_FRENT_HIWAT, NULL, 0);

	TAILQ_INIT(&pf_fragqueue);
}

static __inline int
pf_frag_compare(struct pf_fragment *a, struct pf_fragment *b)
{
	int	diff;

	if ((diff = a->fr_id - b->fr_id) != 0)
		return (diff);
	if ((diff = a->fr_proto - b->fr_proto) != 0)
		return (diff);
	if ((diff = a->fr_af - b->fr_af) != 0)
		return (diff);
	if ((diff = pf_addr_compare(&a->fr_src, &b->fr_src, a->fr_af)) != 0)
		return (diff);
	if ((diff = pf_addr_compare(&a->fr_dst, &b->fr_dst, a->fr_af)) != 0)
		return (diff);
	return (0);
}

void
pf_purge_expired_fragments(void)
{
	struct pf_fragment	*frag;
	u_int32_t		 expire = time_second -
				    pf_default_rule.timeout[PFTM_FRAG];

	while ((frag = TAILQ_LAST(&pf_fragqueue, pf_fragqueue)) != NULL) {
		if (frag->fr_timeout > expire)
			break;

		DPFPRINTF(LOG_NOTICE, "expiring %d(%p)", frag->fr_id, frag);
		pf_free_fragment(frag);
	}
}

/*
 * Try to flush old fragments to make space for new ones
 */

void
pf_flush_fragments(void)
{
	struct pf_fragment	*frag;
	int			 goal;

	goal = pf_nfrents * 9 / 10;
	DPFPRINTF(LOG_NOTICE, "trying to free > %d frents",
	    pf_nfrents - goal);
	while (goal < pf_nfrents) {
		frag = TAILQ_LAST(&pf_fragqueue, pf_fragqueue);
		if (frag == NULL)
			break;
		pf_free_fragment(frag);
	}
}

/* Frees the fragments and all associated entries */

void
pf_free_fragment(struct pf_fragment *frag)
{
	struct pf_frent		*frent;

	/* Free all fragments */
	for (frent = TAILQ_FIRST(&frag->fr_queue); frent;
	    frent = TAILQ_FIRST(&frag->fr_queue)) {
		TAILQ_REMOVE(&frag->fr_queue, frent, fr_next);

		m_freem(frent->fe_m);
		pool_put(&pf_frent_pl, frent);
		pf_nfrents--;
	}

	pf_remove_fragment(frag);
}

struct pf_fragment *
pf_find_fragment(struct pf_fragment_cmp *key, struct pf_frag_tree *tree)
{
	struct pf_fragment	*frag;

	frag = RB_FIND(pf_frag_tree, tree, (struct pf_fragment *)key);
	if (frag != NULL) {
		/* XXX Are we sure we want to update the timeout? */
		frag->fr_timeout = time_second;
		TAILQ_REMOVE(&pf_fragqueue, frag, frag_next);
		TAILQ_INSERT_HEAD(&pf_fragqueue, frag, frag_next);
	}

	return (frag);
}

/* Removes a fragment from the fragment queue and frees the fragment */

void
pf_remove_fragment(struct pf_fragment *frag)
{
	RB_REMOVE(pf_frag_tree, &pf_frag_tree, frag);
	TAILQ_REMOVE(&pf_fragqueue, frag, frag_next);
	pool_put(&pf_frag_pl, frag);
}

struct pf_frent *
pf_create_fragment(u_short *reason)
{
	struct pf_frent	*frent;

	frent = pool_get(&pf_frent_pl, PR_NOWAIT);
	if (frent == NULL) {
		pf_flush_fragments();
		frent = pool_get(&pf_frent_pl, PR_NOWAIT);
		if (frent == NULL) {
			REASON_SET(reason, PFRES_MEMORY);
			return (NULL);
		}
	}
	pf_nfrents++;

	return (frent);
}

struct pf_fragment *
pf_fillup_fragment(struct pf_fragment_cmp *key, struct pf_frent *frent,
    u_short *reason)
{
	struct pf_frent		*after, *next, *prev;
	struct pf_fragment	*frag;
	u_int16_t		 total;

	/* No empty fragments */
	if (frent->fe_len == 0) {
		DPFPRINTF(LOG_NOTICE, "bad fragment: len 0");
		goto bad_fragment;
	}

	/* All fragments are 8 byte aligned */
	if (frent->fe_mff && (frent->fe_len & 0x7)) {
		DPFPRINTF(LOG_NOTICE, "bad fragment: mff and len %d",
		    frent->fe_len);
		goto bad_fragment;
	}

	/* Respect maximum length, IP_MAXPACKET == IPV6_MAXPACKET */
	if (frent->fe_off + frent->fe_len > IP_MAXPACKET) {
		DPFPRINTF(LOG_NOTICE, "bad fragment: max packet %d",
		    frent->fe_off + frent->fe_len);
		goto bad_fragment;
	}

	DPFPRINTF(LOG_NOTICE, key->fr_af == AF_INET ?
	    "reass frag %d @ %d-%d" : "reass frag %#08x @ %d-%d",
	    key->fr_id, frent->fe_off, frent->fe_off + frent->fe_len);

	/* Fully buffer all of the fragments in this fragment queue */
	frag = pf_find_fragment(key, &pf_frag_tree);

	/* Create a new reassembly queue for this packet */
	if (frag == NULL) {
		frag = pool_get(&pf_frag_pl, PR_NOWAIT);
		if (frag == NULL) {
			pf_flush_fragments();
			frag = pool_get(&pf_frag_pl, PR_NOWAIT);
			if (frag == NULL) {
				REASON_SET(reason, PFRES_MEMORY);
				goto drop_fragment;
			}
		}

		*(struct pf_fragment_cmp *)frag = *key;
		frag->fr_timeout = time_second;
		frag->fr_maxlen = frent->fe_len;
		TAILQ_INIT(&frag->fr_queue);

		RB_INSERT(pf_frag_tree, &pf_frag_tree, frag);
		TAILQ_INSERT_HEAD(&pf_fragqueue, frag, frag_next);

		/* We do not have a previous fragment */
		TAILQ_INSERT_HEAD(&frag->fr_queue, frent, fr_next);

		return (frag);
	}

	KASSERT(!TAILQ_EMPTY(&frag->fr_queue));

	/* Remember maximum fragment len for refragmentation */
	if (frent->fe_len > frag->fr_maxlen)
		frag->fr_maxlen = frent->fe_len;

	/* Maximum data we have seen already */
	total = TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_off +
	    TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_len;

	/* Non terminal fragments must have more fragments flag */
	if (frent->fe_off + frent->fe_len < total && !frent->fe_mff)
		goto bad_fragment;

	/* Check if we saw the last fragment already */
	if (!TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_mff) {
		if (frent->fe_off + frent->fe_len > total ||
		    (frent->fe_off + frent->fe_len == total && frent->fe_mff))
			goto bad_fragment;
	} else {
		if (frent->fe_off + frent->fe_len == total && !frent->fe_mff)
			goto bad_fragment;
	}

	/* Find a fragment after the current one */
	prev = NULL;
	TAILQ_FOREACH(after, &frag->fr_queue, fr_next) {
		if (after->fe_off > frent->fe_off)
			break;
		prev = after;
	}

	KASSERT(prev != NULL || after != NULL);

	if (prev != NULL && prev->fe_off + prev->fe_len > frent->fe_off) {
		u_int16_t	precut;

		precut = prev->fe_off + prev->fe_len - frent->fe_off;
		if (precut >= frent->fe_len)
			goto bad_fragment;
		DPFPRINTF(LOG_NOTICE, "overlap -%d", precut);
		m_adj(frent->fe_m, precut);
		frent->fe_off += precut;
		frent->fe_len -= precut;
	}

	for (; after != NULL && frent->fe_off + frent->fe_len > after->fe_off;
	    after = next)
	{
		u_int16_t	aftercut;

		aftercut = frent->fe_off + frent->fe_len - after->fe_off;
		DPFPRINTF(LOG_NOTICE, "adjust overlap %d", aftercut);
		if (aftercut < after->fe_len) {
			m_adj(after->fe_m, aftercut);
			after->fe_off += aftercut;
			after->fe_len -= aftercut;
			break;
		}

		/* This fragment is completely overlapped, lose it */
		next = TAILQ_NEXT(after, fr_next);
		m_freem(after->fe_m);
		TAILQ_REMOVE(&frag->fr_queue, after, fr_next);
		pool_put(&pf_frent_pl, after);
		pf_nfrents--;
	}

	if (prev == NULL)
		TAILQ_INSERT_HEAD(&frag->fr_queue, frent, fr_next);
	else
		TAILQ_INSERT_AFTER(&frag->fr_queue, prev, frent, fr_next);

	return (frag);

 bad_fragment:
	REASON_SET(reason, PFRES_FRAG);
 drop_fragment:
	pool_put(&pf_frent_pl, frent);
	pf_nfrents--;
	return (NULL);
}

int
pf_isfull_fragment(struct pf_fragment *frag)
{
	struct pf_frent		*frent, *next;
	u_int16_t		 off, total;

	/* Check if we are completely reassembled */
	if (TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_mff)
		return (0);

	/* Maximum data we have seen already */
	total = TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_off +
	    TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_len;

	/* Check if we have all the data */
	off = 0;
	for (frent = TAILQ_FIRST(&frag->fr_queue); frent; frent = next) {
		next = TAILQ_NEXT(frent, fr_next);

		off += frent->fe_len;
		if (off < total && (next == NULL || next->fe_off != off)) {
			DPFPRINTF(LOG_NOTICE,
			    "missing fragment at %d, next %d, total %d",
			    off, next == NULL ? -1 : next->fe_off, total);
			return (0);
		}
	}
	DPFPRINTF(LOG_NOTICE, "%d < %d?", off, total);
	if (off < total)
		return (0);
	KASSERT(off == total);

	return (1);
}

struct mbuf *
pf_join_fragment(struct pf_fragment *frag)
{
	struct mbuf		*m, *m2;
	struct pf_frent		*frent, *next;

	frent = TAILQ_FIRST(&frag->fr_queue);
	next = TAILQ_NEXT(frent, fr_next);

	/* Magic from ip_input */
	m = frent->fe_m;
	m2 = m->m_next;
	m->m_next = NULL;
	m_cat(m, m2);
	pool_put(&pf_frent_pl, frent);
	pf_nfrents--;
	for (frent = next; frent != NULL; frent = next) {
		next = TAILQ_NEXT(frent, fr_next);

		m2 = frent->fe_m;
		/* Strip off ip header */
		m_adj(m2, frent->fe_hdrlen);
		pool_put(&pf_frent_pl, frent);
		pf_nfrents--;
		m_cat(m, m2);
	}

	/* Remove from fragment queue */
	pf_remove_fragment(frag);

	return (m);
}

int
pf_reassemble(struct mbuf **m0, struct ip *ip, int dir, u_short *reason)
{
	struct mbuf		*m = *m0;
	struct pf_frent		*frent;
	struct pf_fragment	*frag;
	struct pf_fragment_cmp	 key;
	u_int16_t		 total, hdrlen;

	/* Get an entry for the fragment queue */
	if ((frent = pf_create_fragment(reason)) == NULL)
		return (PF_DROP);

	frent->fe_m = m;
	frent->fe_hdrlen = ip->ip_hl << 2;
	frent->fe_extoff = 0;
	frent->fe_len = ntohs(ip->ip_len) - (ip->ip_hl << 2);
	frent->fe_off = (ntohs(ip->ip_off) & IP_OFFMASK) << 3;
	frent->fe_mff = ntohs(ip->ip_off) & IP_MF;

	key.fr_src.v4 = ip->ip_src;
	key.fr_dst.v4 = ip->ip_dst;
	key.fr_af = AF_INET;
	key.fr_proto = ip->ip_p;
	key.fr_id = ip->ip_id;
	key.fr_direction = dir;

	if ((frag = pf_fillup_fragment(&key, frent, reason)) == NULL)
		return (PF_DROP);

	/* The mbuf is part of the fragment entry, no direct free or access */
	m = *m0 = NULL;

	if (!pf_isfull_fragment(frag))
		return (PF_PASS);  /* drop because *m0 is NULL, no error */

	/* We have all the data */
	frent = TAILQ_FIRST(&frag->fr_queue);
	KASSERT(frent != NULL);
	total = TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_off +
	    TAILQ_LAST(&frag->fr_queue, pf_fragq)->fe_len;
	hdrlen = frent->fe_hdrlen;

	m = *m0 = pf_join_fragment(frag);
	frag = NULL;

	if (m->m_flags & M_PKTHDR) {
		int plen = 0;
		for (m = *m0; m; m = m->m_next)
			plen += m->m_len;
		m = *m0;
		m->m_pkthdr.len = plen;
	}

	ip = mtod(m, struct ip *);
	ip->ip_len = htons(hdrlen + total);
	ip->ip_off &= ~(IP_MF|IP_OFFMASK);

	if (hdrlen + total > IP_MAXPACKET) {
		DPFPRINTF(LOG_NOTICE, "drop: too big: %d", total);
		ip->ip_len = 0;
		REASON_SET(reason, PFRES_SHORT);
		/* PF_DROP requires a valid mbuf *m0 in pf_test() */
		return (PF_DROP);
	}

	DPFPRINTF(LOG_NOTICE, "complete: %p(%d)", m, ntohs(ip->ip_len));
	return (PF_PASS);
}

int
pf_normalize_ip(struct mbuf **m0, int dir, struct pfi_kif *kif,
    u_short *reason, struct pf_pdesc *pd)
{
	struct mbuf		*m = *m0;
	struct ip		*h = mtod(m, struct ip *);
	int			 hlen = h->ip_hl << 2;
	u_int16_t		 fragoff = (ntohs(h->ip_off) & IP_OFFMASK) << 3;
	u_int16_t		 mff = (ntohs(h->ip_off) & IP_MF);

	/* Check for illegal packets */
	if (hlen < (int)sizeof(struct ip))
		goto drop;

	if (hlen > ntohs(h->ip_len))
		goto drop;

	/* Clear IP_DF if we're in no-df mode */
	if (pf_status.reass & PF_REASS_NODF && h->ip_off & htons(IP_DF)) {
		u_int16_t ip_off = h->ip_off;

		h->ip_off &= htons(~IP_DF);
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_off, h->ip_off, 0);
	}

	/* We will need other tests here */
	if (!fragoff && !mff)
		goto no_fragment;

	/* We're dealing with a fragment now. Don't allow fragments
	 * with IP_DF to enter the cache. If the flag was cleared by
	 * no-df above, fine. Otherwise drop it.
	 */
	if (h->ip_off & htons(IP_DF)) {
		DPFPRINTF(LOG_NOTICE, "bad fragment: IP_DF");
		REASON_SET(reason, PFRES_FRAG);
		return (PF_DROP);
	}

	/* Returns PF_DROP or *m0 is NULL or completely reassembled mbuf */
	if (pf_reassemble(m0, h, dir, reason) != PF_PASS)
		return (PF_DROP);
	m = *m0;
	if (m == NULL)
		return (PF_PASS);  /* packet has been reassembled, no error */

	h = mtod(m, struct ip *);

 no_fragment:
	/* At this point, only IP_DF is allowed in ip_off */
	if (h->ip_off & ~htons(IP_DF)) {
		u_int16_t ip_off = h->ip_off;

		h->ip_off &= htons(IP_DF);
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_off, h->ip_off, 0);
	}

	pd->flags |= PFDESC_IP_REAS;
	return (PF_PASS);

 drop:
	REASON_SET(reason, PFRES_NORM);
	return (PF_DROP);
}

#ifdef INET6
int
pf_normalize_ip6(struct mbuf **m0, int dir, struct pfi_kif *kif,
    u_short *reason, struct pf_pdesc *pd)
{
	struct mbuf		*m = *m0;
	struct ip6_hdr		*h = mtod(m, struct ip6_hdr *);
	int			 off;
	struct ip6_ext		 ext;
	struct ip6_opt		 opt;
	struct ip6_opt_jumbo	 jumbo;
	struct ip6_frag		 frag;
	u_int32_t		 jumbolen = 0, plen;
	u_int16_t		 fragoff = 0;
	int			 optend;
	int			 ooff;
	u_int8_t		 proto;
	int			 terminal;

	/* Check for illegal packets */
	if (sizeof(struct ip6_hdr) + IPV6_MAXPACKET < m->m_pkthdr.len)
		goto drop;

	off = sizeof(struct ip6_hdr);
	proto = h->ip6_nxt;
	terminal = 0;
	do {
		switch (proto) {
		case IPPROTO_FRAGMENT:
			goto fragment;
			break;
		case IPPROTO_AH:
		case IPPROTO_ROUTING:
		case IPPROTO_DSTOPTS:
			if (!pf_pull_hdr(m, off, &ext, sizeof(ext), NULL,
			    NULL, AF_INET6))
				goto shortpkt;
			if (proto == IPPROTO_AH)
				off += (ext.ip6e_len + 2) * 4;
			else
				off += (ext.ip6e_len + 1) * 8;
			proto = ext.ip6e_nxt;
			break;
		case IPPROTO_HOPOPTS:
			if (!pf_pull_hdr(m, off, &ext, sizeof(ext), NULL,
			    NULL, AF_INET6))
				goto shortpkt;
			optend = off + (ext.ip6e_len + 1) * 8;
			ooff = off + sizeof(ext);
			do {
				if (!pf_pull_hdr(m, ooff, &opt.ip6o_type,
				    sizeof(opt.ip6o_type), NULL, NULL,
				    AF_INET6))
					goto shortpkt;
				if (opt.ip6o_type == IP6OPT_PAD1) {
					ooff++;
					continue;
				}
				if (!pf_pull_hdr(m, ooff, &opt, sizeof(opt),
				    NULL, NULL, AF_INET6))
					goto shortpkt;
				if (ooff + sizeof(opt) + opt.ip6o_len > optend)
					goto drop;
				switch (opt.ip6o_type) {
				case IP6OPT_JUMBO:
					if (h->ip6_plen != 0)
						goto drop;
					if (!pf_pull_hdr(m, ooff, &jumbo,
					    sizeof(jumbo), NULL, NULL,
					    AF_INET6))
						goto shortpkt;
					memcpy(&jumbolen, jumbo.ip6oj_jumbo_len,
					    sizeof(jumbolen));
					jumbolen = ntohl(jumbolen);
					if (jumbolen <= IPV6_MAXPACKET)
						goto drop;
					if (sizeof(struct ip6_hdr) + jumbolen !=
					    m->m_pkthdr.len)
						goto drop;
					break;
				default:
					break;
				}
				ooff += sizeof(opt) + opt.ip6o_len;
			} while (ooff < optend);

			off = optend;
			proto = ext.ip6e_nxt;
			break;
		default:
			terminal = 1;
			break;
		}
	} while (!terminal);

	/* jumbo payload option must be present, or plen > 0 */
	if (ntohs(h->ip6_plen) == 0)
		plen = jumbolen;
	else
		plen = ntohs(h->ip6_plen);
	if (plen == 0)
		goto drop;
	if (sizeof(struct ip6_hdr) + plen > m->m_pkthdr.len)
		goto shortpkt;

	return (PF_PASS);

 fragment:
	if (ntohs(h->ip6_plen) == 0 || jumbolen)
		goto drop;
	plen = ntohs(h->ip6_plen);

	if (!pf_pull_hdr(m, off, &frag, sizeof(frag), NULL, NULL, AF_INET6))
		goto shortpkt;
	fragoff = ntohs(frag.ip6f_offlg & IP6F_OFF_MASK);
	if (fragoff + (sizeof(struct ip6_hdr) + plen - off - sizeof(frag)) >
	    IPV6_MAXPACKET)
		goto badfrag;

	/* do something about it */
	/* remember to set pd->flags |= PFDESC_IP_REAS */
	return (PF_PASS);

 shortpkt:
	REASON_SET(reason, PFRES_SHORT);
	return (PF_DROP);

 drop:
	REASON_SET(reason, PFRES_NORM);
	return (PF_DROP);

 badfrag:
	REASON_SET(reason, PFRES_FRAG);
	return (PF_DROP);
}
#endif /* INET6 */

int
pf_normalize_tcp(int dir, struct pfi_kif *kif, struct mbuf *m, int ipoff,
    int off, void *h, struct pf_pdesc *pd)
{
	struct tcphdr	*th = pd->hdr.tcp;
	u_short		 reason;
	u_int8_t	 flags;
	u_int		 rewrite = 0;

	flags = th->th_flags;
	if (flags & TH_SYN) {
		/* Illegal packet */
		if (flags & TH_RST)
			goto tcp_drop;

		if (flags & TH_FIN)
			flags &= ~TH_FIN;
	} else {
		/* Illegal packet */
		if (!(flags & (TH_ACK|TH_RST)))
			goto tcp_drop;
	}

	if (!(flags & TH_ACK)) {
		/* These flags are only valid if ACK is set */
		if ((flags & TH_FIN) || (flags & TH_PUSH) || (flags & TH_URG))
			goto tcp_drop;
	}

	/* Check for illegal header length */
	if (th->th_off < (sizeof(struct tcphdr) >> 2))
		goto tcp_drop;

	/* If flags changed, or reserved data set, then adjust */
	if (flags != th->th_flags || th->th_x2 != 0) {
		u_int16_t	ov, nv;

		ov = *(u_int16_t *)(&th->th_ack + 1);
		th->th_flags = flags;
		th->th_x2 = 0;
		nv = *(u_int16_t *)(&th->th_ack + 1);

		th->th_sum = pf_cksum_fixup(th->th_sum, ov, nv, 0);
		rewrite = 1;
	}

	/* Remove urgent pointer, if TH_URG is not set */
	if (!(flags & TH_URG) && th->th_urp) {
		th->th_sum = pf_cksum_fixup(th->th_sum, th->th_urp, 0, 0);
		th->th_urp = 0;
		rewrite = 1;
	}

	/* copy back packet headers if we sanitized */
	if (rewrite)
		m_copyback(m, off, sizeof(*th), th, M_NOWAIT);

	return (PF_PASS);

 tcp_drop:
	REASON_SET(&reason, PFRES_NORM);
	return (PF_DROP);
}

int
pf_normalize_tcp_init(struct mbuf *m, int off, struct pf_pdesc *pd,
    struct tcphdr *th, struct pf_state_peer *src, struct pf_state_peer *dst)
{
	u_int32_t tsval, tsecr;
	u_int8_t hdr[60];
	u_int8_t *opt;

	KASSERT(src->scrub == NULL);

	src->scrub = pool_get(&pf_state_scrub_pl, PR_NOWAIT);
	if (src->scrub == NULL)
		return (1);
	bzero(src->scrub, sizeof(*src->scrub));

	switch (pd->af) {
#ifdef INET
	case AF_INET: {
		struct ip *h = mtod(m, struct ip *);
		src->scrub->pfss_ttl = h->ip_ttl;
		break;
	}
#endif /* INET */
#ifdef INET6
	case AF_INET6: {
		struct ip6_hdr *h = mtod(m, struct ip6_hdr *);
		src->scrub->pfss_ttl = h->ip6_hlim;
		break;
	}
#endif /* INET6 */
	}


	/*
	 * All normalizations below are only begun if we see the start of
	 * the connections.  They must all set an enabled bit in pfss_flags
	 */
	if ((th->th_flags & TH_SYN) == 0)
		return (0);


	if (th->th_off > (sizeof(struct tcphdr) >> 2) && src->scrub &&
	    pf_pull_hdr(m, off, hdr, th->th_off << 2, NULL, NULL, pd->af)) {
		/* Diddle with TCP options */
		int hlen;
		opt = hdr + sizeof(struct tcphdr);
		hlen = (th->th_off << 2) - sizeof(struct tcphdr);
		while (hlen >= TCPOLEN_TIMESTAMP) {
			switch (*opt) {
			case TCPOPT_EOL:	/* FALLTHROUGH */
			case TCPOPT_NOP:
				opt++;
				hlen--;
				break;
			case TCPOPT_TIMESTAMP:
				if (opt[1] >= TCPOLEN_TIMESTAMP) {
					src->scrub->pfss_flags |=
					    PFSS_TIMESTAMP;
					src->scrub->pfss_ts_mod =
					    htonl(arc4random());

					/* note PFSS_PAWS not set yet */
					memcpy(&tsval, &opt[2],
					    sizeof(u_int32_t));
					memcpy(&tsecr, &opt[6],
					    sizeof(u_int32_t));
					src->scrub->pfss_tsval0 = ntohl(tsval);
					src->scrub->pfss_tsval = ntohl(tsval);
					src->scrub->pfss_tsecr = ntohl(tsecr);
					getmicrouptime(&src->scrub->pfss_last);
				}
				/* FALLTHROUGH */
			default:
				hlen -= MAX(opt[1], 2);
				opt += MAX(opt[1], 2);
				break;
			}
		}
	}

	return (0);
}

void
pf_normalize_tcp_cleanup(struct pf_state *state)
{
	if (state->src.scrub)
		pool_put(&pf_state_scrub_pl, state->src.scrub);
	if (state->dst.scrub)
		pool_put(&pf_state_scrub_pl, state->dst.scrub);

	/* Someday... flush the TCP segment reassembly descriptors. */
}

int
pf_normalize_tcp_stateful(struct mbuf *m, int off, struct pf_pdesc *pd,
    u_short *reason, struct tcphdr *th, struct pf_state *state,
    struct pf_state_peer *src, struct pf_state_peer *dst, int *writeback)
{
	struct timeval uptime;
	u_int32_t tsval, tsecr;
	u_int tsval_from_last;
	u_int8_t hdr[60];
	u_int8_t *opt;
	int copyback = 0;
	int got_ts = 0;

	KASSERT(src->scrub || dst->scrub);

	/*
	 * Enforce the minimum TTL seen for this connection.  Negate a common
	 * technique to evade an intrusion detection system and confuse
	 * firewall state code.
	 */
	switch (pd->af) {
#ifdef INET
	case AF_INET: {
		if (src->scrub) {
			struct ip *h = mtod(m, struct ip *);
			if (h->ip_ttl > src->scrub->pfss_ttl)
				src->scrub->pfss_ttl = h->ip_ttl;
			h->ip_ttl = src->scrub->pfss_ttl;
		}
		break;
	}
#endif /* INET */
#ifdef INET6
	case AF_INET6: {
		if (src->scrub) {
			struct ip6_hdr *h = mtod(m, struct ip6_hdr *);
			if (h->ip6_hlim > src->scrub->pfss_ttl)
				src->scrub->pfss_ttl = h->ip6_hlim;
			h->ip6_hlim = src->scrub->pfss_ttl;
		}
		break;
	}
#endif /* INET6 */
	}

	if (th->th_off > (sizeof(struct tcphdr) >> 2) &&
	    ((src->scrub && (src->scrub->pfss_flags & PFSS_TIMESTAMP)) ||
	    (dst->scrub && (dst->scrub->pfss_flags & PFSS_TIMESTAMP))) &&
	    pf_pull_hdr(m, off, hdr, th->th_off << 2, NULL, NULL, pd->af)) {
		/* Diddle with TCP options */
		int hlen;
		opt = hdr + sizeof(struct tcphdr);
		hlen = (th->th_off << 2) - sizeof(struct tcphdr);
		while (hlen >= TCPOLEN_TIMESTAMP) {
			switch (*opt) {
			case TCPOPT_EOL:	/* FALLTHROUGH */
			case TCPOPT_NOP:
				opt++;
				hlen--;
				break;
			case TCPOPT_TIMESTAMP:
				/* Modulate the timestamps.  Can be used for
				 * NAT detection, OS uptime determination or
				 * reboot detection.
				 */

				if (got_ts) {
					/* Huh?  Multiple timestamps!? */
					if (pf_status.debug >= LOG_NOTICE) {
						log(LOG_NOTICE,
						    "pf: %s: multiple TS??",
						    __func__);
						pf_print_state(state);
						addlog("\n");
					}
					REASON_SET(reason, PFRES_TS);
					return (PF_DROP);
				}
				if (opt[1] >= TCPOLEN_TIMESTAMP) {
					memcpy(&tsval, &opt[2],
					    sizeof(u_int32_t));
					if (tsval && src->scrub &&
					    (src->scrub->pfss_flags &
					    PFSS_TIMESTAMP)) {
						tsval = ntohl(tsval);
						pf_change_a(&opt[2],
						    &th->th_sum,
						    htonl(tsval +
						    src->scrub->pfss_ts_mod),
						    0);
						copyback = 1;
					}

					/* Modulate TS reply iff valid (!0) */
					memcpy(&tsecr, &opt[6],
					    sizeof(u_int32_t));
					if (tsecr && dst->scrub &&
					    (dst->scrub->pfss_flags &
					    PFSS_TIMESTAMP)) {
						tsecr = ntohl(tsecr)
						    - dst->scrub->pfss_ts_mod;
						pf_change_a(&opt[6],
						    &th->th_sum, htonl(tsecr),
						    0);
						copyback = 1;
					}
					got_ts = 1;
				}
				/* FALLTHROUGH */
			default:
				hlen -= MAX(opt[1], 2);
				opt += MAX(opt[1], 2);
				break;
			}
		}
		if (copyback) {
			/* Copyback the options, caller copys back header */
			*writeback = 1;
			m_copyback(m, off + sizeof(struct tcphdr),
			    (th->th_off << 2) - sizeof(struct tcphdr), hdr +
			    sizeof(struct tcphdr), M_NOWAIT);
		}
	}


	/*
	 * Must invalidate PAWS checks on connections idle for too long.
	 * The fastest allowed timestamp clock is 1ms.  That turns out to
	 * be about 24 days before it wraps.  XXX Right now our lowerbound
	 * TS echo check only works for the first 12 days of a connection
	 * when the TS has exhausted half its 32bit space
	 */
#define TS_MAX_IDLE	(24*24*60*60)
#define TS_MAX_CONN	(12*24*60*60)	/* XXX remove when better tsecr check */

	getmicrouptime(&uptime);
	if (src->scrub && (src->scrub->pfss_flags & PFSS_PAWS) &&
	    (uptime.tv_sec - src->scrub->pfss_last.tv_sec > TS_MAX_IDLE ||
	    time_second - state->creation > TS_MAX_CONN))  {
		if (pf_status.debug >= LOG_NOTICE) {
			log(LOG_NOTICE, "pf: src idled out of PAWS ");
			pf_print_state(state);
			addlog("\n");
		}
		src->scrub->pfss_flags = (src->scrub->pfss_flags & ~PFSS_PAWS)
		    | PFSS_PAWS_IDLED;
	}
	if (dst->scrub && (dst->scrub->pfss_flags & PFSS_PAWS) &&
	    uptime.tv_sec - dst->scrub->pfss_last.tv_sec > TS_MAX_IDLE) {
		if (pf_status.debug >= LOG_NOTICE) {
			log(LOG_NOTICE, "pf: dst idled out of PAWS ");
			pf_print_state(state);
			addlog("\n");
		}
		dst->scrub->pfss_flags = (dst->scrub->pfss_flags & ~PFSS_PAWS)
		    | PFSS_PAWS_IDLED;
	}

	if (got_ts && src->scrub && dst->scrub &&
	    (src->scrub->pfss_flags & PFSS_PAWS) &&
	    (dst->scrub->pfss_flags & PFSS_PAWS)) {
		/* Validate that the timestamps are "in-window".
		 * RFC1323 describes TCP Timestamp options that allow
		 * measurement of RTT (round trip time) and PAWS
		 * (protection against wrapped sequence numbers).  PAWS
		 * gives us a set of rules for rejecting packets on
		 * long fat pipes (packets that were somehow delayed 
		 * in transit longer than the time it took to send the
		 * full TCP sequence space of 4Gb).  We can use these
		 * rules and infer a few others that will let us treat
		 * the 32bit timestamp and the 32bit echoed timestamp
		 * as sequence numbers to prevent a blind attacker from
		 * inserting packets into a connection.
		 *
		 * RFC1323 tells us:
		 *  - The timestamp on this packet must be greater than
		 *    or equal to the last value echoed by the other
		 *    endpoint.  The RFC says those will be discarded
		 *    since it is a dup that has already been acked.
		 *    This gives us a lowerbound on the timestamp.
		 *        timestamp >= other last echoed timestamp
		 *  - The timestamp will be less than or equal to
		 *    the last timestamp plus the time between the
		 *    last packet and now.  The RFC defines the max
		 *    clock rate as 1ms.  We will allow clocks to be
		 *    up to 10% fast and will allow a total difference
		 *    or 30 seconds due to a route change.  And this
		 *    gives us an upperbound on the timestamp.
		 *        timestamp <= last timestamp + max ticks
		 *    We have to be careful here.  Windows will send an
		 *    initial timestamp of zero and then initialize it
		 *    to a random value after the 3whs; presumably to
		 *    avoid a DoS by having to call an expensive RNG
		 *    during a SYN flood.  Proof MS has at least one
		 *    good security geek.
		 *
		 *  - The TCP timestamp option must also echo the other
		 *    endpoints timestamp.  The timestamp echoed is the
		 *    one carried on the earliest unacknowledged segment
		 *    on the left edge of the sequence window.  The RFC
		 *    states that the host will reject any echoed
		 *    timestamps that were larger than any ever sent.
		 *    This gives us an upperbound on the TS echo.
		 *        tescr <= largest_tsval
		 *  - The lowerbound on the TS echo is a little more
		 *    tricky to determine.  The other endpoint's echoed
		 *    values will not decrease.  But there may be
		 *    network conditions that re-order packets and
		 *    cause our view of them to decrease.  For now the
		 *    only lowerbound we can safely determine is that
		 *    the TS echo will never be less than the original
		 *    TS.  XXX There is probably a better lowerbound.
		 *    Remove TS_MAX_CONN with better lowerbound check.
		 *        tescr >= other original TS
		 *
		 * It is also important to note that the fastest
		 * timestamp clock of 1ms will wrap its 32bit space in
		 * 24 days.  So we just disable TS checking after 24
		 * days of idle time.  We actually must use a 12d
		 * connection limit until we can come up with a better
		 * lowerbound to the TS echo check.
		 */
		struct timeval delta_ts;
		int ts_fudge;


		/*
		 * PFTM_TS_DIFF is how many seconds of leeway to allow
		 * a host's timestamp.  This can happen if the previous
		 * packet got delayed in transit for much longer than
		 * this packet.
		 */
		if ((ts_fudge = state->rule.ptr->timeout[PFTM_TS_DIFF]) == 0)
			ts_fudge = pf_default_rule.timeout[PFTM_TS_DIFF];


		/* Calculate max ticks since the last timestamp */
#define TS_MAXFREQ	1100		/* RFC max TS freq of 1Khz + 10% skew */
#define TS_MICROSECS	1000000		/* microseconds per second */
		timersub(&uptime, &src->scrub->pfss_last, &delta_ts);
		tsval_from_last = (delta_ts.tv_sec + ts_fudge) * TS_MAXFREQ;
		tsval_from_last += delta_ts.tv_usec / (TS_MICROSECS/TS_MAXFREQ);


		if ((src->state >= TCPS_ESTABLISHED &&
		    dst->state >= TCPS_ESTABLISHED) &&
		    (SEQ_LT(tsval, dst->scrub->pfss_tsecr) ||
		    SEQ_GT(tsval, src->scrub->pfss_tsval + tsval_from_last) ||
		    (tsecr && (SEQ_GT(tsecr, dst->scrub->pfss_tsval) ||
		    SEQ_LT(tsecr, dst->scrub->pfss_tsval0))))) {
			/* Bad RFC1323 implementation or an insertion attack.
			 *
			 * - Solaris 2.6 and 2.7 are known to send another ACK
			 *   after the FIN,FIN|ACK,ACK closing that carries
			 *   an old timestamp.
			 */

			DPFPRINTF(LOG_NOTICE, "Timestamp failed %c%c%c%c",
			    SEQ_LT(tsval, dst->scrub->pfss_tsecr) ? '0' : ' ',
			    SEQ_GT(tsval, src->scrub->pfss_tsval +
			    tsval_from_last) ? '1' : ' ',
			    SEQ_GT(tsecr, dst->scrub->pfss_tsval) ? '2' : ' ',
			    SEQ_LT(tsecr, dst->scrub->pfss_tsval0)? '3' : ' ');
			DPFPRINTF(LOG_NOTICE,
			    " tsval: %lu  tsecr: %lu  +ticks: %lu  "
			    "idle: %lus %lums",
			    tsval, tsecr, tsval_from_last, delta_ts.tv_sec,
			    delta_ts.tv_usec / 1000);
			DPFPRINTF(LOG_NOTICE,
			    " src->tsval: %lu  tsecr: %lu",
			    src->scrub->pfss_tsval, src->scrub->pfss_tsecr);
			DPFPRINTF(LOG_NOTICE,
			    " dst->tsval: %lu  tsecr: %lu  tsval0: %lu",
			    dst->scrub->pfss_tsval, dst->scrub->pfss_tsecr,
			    dst->scrub->pfss_tsval0);
			if (pf_status.debug >= LOG_NOTICE) {
				log(LOG_NOTICE, "pf: ");
				pf_print_state(state);
				pf_print_flags(th->th_flags);
				addlog("\n");
			}
			REASON_SET(reason, PFRES_TS);
			return (PF_DROP);
		}

		/* XXX I'd really like to require tsecr but it's optional */

	} else if (!got_ts && (th->th_flags & TH_RST) == 0 &&
	    ((src->state == TCPS_ESTABLISHED && dst->state == TCPS_ESTABLISHED)
	    || pd->p_len > 0 || (th->th_flags & TH_SYN)) &&
	    src->scrub && dst->scrub &&
	    (src->scrub->pfss_flags & PFSS_PAWS) &&
	    (dst->scrub->pfss_flags & PFSS_PAWS)) {
		/* Didn't send a timestamp.  Timestamps aren't really useful
		 * when:
		 *  - connection opening or closing (often not even sent).
		 *    but we must not let an attacker to put a FIN on a
		 *    data packet to sneak it through our ESTABLISHED check.
		 *  - on a TCP reset.  RFC suggests not even looking at TS.
		 *  - on an empty ACK.  The TS will not be echoed so it will
		 *    probably not help keep the RTT calculation in sync and
		 *    there isn't as much danger when the sequence numbers
		 *    got wrapped.  So some stacks don't include TS on empty
		 *    ACKs :-(
		 *
		 * To minimize the disruption to mostly RFC1323 conformant
		 * stacks, we will only require timestamps on data packets.
		 *
		 * And what do ya know, we cannot require timestamps on data
		 * packets.  There appear to be devices that do legitimate
		 * TCP connection hijacking.  There are HTTP devices that allow
		 * a 3whs (with timestamps) and then buffer the HTTP request.
		 * If the intermediate device has the HTTP response cache, it
		 * will spoof the response but not bother timestamping its
		 * packets.  So we can look for the presence of a timestamp in
		 * the first data packet and if there, require it in all future
		 * packets.
		 */

		if (pd->p_len > 0 && (src->scrub->pfss_flags & PFSS_DATA_TS)) {
			/*
			 * Hey!  Someone tried to sneak a packet in.  Or the
			 * stack changed its RFC1323 behavior?!?!
			 */
			if (pf_status.debug >= LOG_NOTICE) {
				log(LOG_NOTICE,
				    "pf: did not receive expected RFC1323 "
				    "timestamp");
				pf_print_state(state);
				pf_print_flags(th->th_flags);
				addlog("\n");
			}
			REASON_SET(reason, PFRES_TS);
			return (PF_DROP);
		}
	}


	/*
	 * We will note if a host sends his data packets with or without
	 * timestamps.  And require all data packets to contain a timestamp
	 * if the first does.  PAWS implicitly requires that all data packets be
	 * timestamped.  But I think there are middle-man devices that hijack
	 * TCP streams immediately after the 3whs and don't timestamp their
	 * packets (seen in a WWW accelerator or cache).
	 */
	if (pd->p_len > 0 && src->scrub && (src->scrub->pfss_flags &
	    (PFSS_TIMESTAMP|PFSS_DATA_TS|PFSS_DATA_NOTS)) == PFSS_TIMESTAMP) {
		if (got_ts)
			src->scrub->pfss_flags |= PFSS_DATA_TS;
		else {
			src->scrub->pfss_flags |= PFSS_DATA_NOTS;
			if (pf_status.debug >= LOG_NOTICE && dst->scrub &&
			    (dst->scrub->pfss_flags & PFSS_TIMESTAMP)) {
				/* Don't warn if other host rejected RFC1323 */
				log(LOG_NOTICE,
				    "pf: broken RFC1323 stack did not "
				    "timestamp data packet. Disabled PAWS "
				    "security.");
				pf_print_state(state);
				pf_print_flags(th->th_flags);
				addlog("\n");
			}
		}
	}


	/*
	 * Update PAWS values
	 */
	if (got_ts && src->scrub && PFSS_TIMESTAMP == (src->scrub->pfss_flags &
	    (PFSS_PAWS_IDLED|PFSS_TIMESTAMP))) {
		getmicrouptime(&src->scrub->pfss_last);
		if (SEQ_GEQ(tsval, src->scrub->pfss_tsval) ||
		    (src->scrub->pfss_flags & PFSS_PAWS) == 0)
			src->scrub->pfss_tsval = tsval;

		if (tsecr) {
			if (SEQ_GEQ(tsecr, src->scrub->pfss_tsecr) ||
			    (src->scrub->pfss_flags & PFSS_PAWS) == 0)
				src->scrub->pfss_tsecr = tsecr;

			if ((src->scrub->pfss_flags & PFSS_PAWS) == 0 &&
			    (SEQ_LT(tsval, src->scrub->pfss_tsval0) ||
			    src->scrub->pfss_tsval0 == 0)) {
				/* tsval0 MUST be the lowest timestamp */
				src->scrub->pfss_tsval0 = tsval;
			}

			/* Only fully initialized after a TS gets echoed */
			if ((src->scrub->pfss_flags & PFSS_PAWS) == 0)
				src->scrub->pfss_flags |= PFSS_PAWS;
		}
	}

	/* I have a dream....  TCP segment reassembly.... */
	return (0);
}

int
pf_normalize_mss(struct mbuf *m, int off, struct pf_pdesc *pd, u_int16_t maxmss)
{
	struct tcphdr	*th = pd->hdr.tcp;
	u_int16_t	 mss;
	int		 thoff;
	int		 opt, cnt, optlen = 0;
	u_char		 opts[MAX_TCPOPTLEN];
	u_char		*optp = opts;

	thoff = th->th_off << 2;
	cnt = thoff - sizeof(struct tcphdr);

	if (cnt > 0 && !pf_pull_hdr(m, off + sizeof(*th), opts, cnt,
	    NULL, NULL, pd->af))
		return (0);

	for (; cnt > 0; cnt -= optlen, optp += optlen) {
		opt = optp[0];
		if (opt == TCPOPT_EOL)
			break;
		if (opt == TCPOPT_NOP)
			optlen = 1;
		else {
			if (cnt < 2)
				break;
			optlen = optp[1];
			if (optlen < 2 || optlen > cnt)
				break;
		}
		switch (opt) {
		case TCPOPT_MAXSEG:
			bcopy((caddr_t)(optp + 2), (caddr_t)&mss, 2);
			if (ntohs(mss) > maxmss) {
				th->th_sum = pf_cksum_fixup(th->th_sum,
				    mss, htons(maxmss), 0);
				mss = htons(maxmss);
				m_copyback(m,
				    off + sizeof(*th) + optp + 2 - opts,
				    2, &mss, M_NOWAIT);
				m_copyback(m, off, sizeof(*th), th, M_NOWAIT);
			}
			break;
		default:
			break;
		}
	}



	return (0);
}

void
pf_scrub_ip(struct mbuf **m0, u_int16_t flags, u_int8_t min_ttl, u_int8_t tos)
{
	struct mbuf		*m = *m0;
	struct ip		*h = mtod(m, struct ip *);

	/* Clear IP_DF if no-df was requested */
	if (flags & PFSTATE_NODF && h->ip_off & htons(IP_DF)) {
		u_int16_t ip_off = h->ip_off;

		h->ip_off &= htons(~IP_DF);
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_off, h->ip_off, 0);
	}

	/* Enforce a minimum ttl, may cause endless packet loops */
	if (min_ttl && h->ip_ttl < min_ttl) {
		u_int16_t ip_ttl = h->ip_ttl;

		h->ip_ttl = min_ttl;
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_ttl, h->ip_ttl, 0);
	}

	/* Enforce tos */
	if (flags & PFSTATE_SETTOS) {
		u_int16_t	ov, nv;

		ov = *(u_int16_t *)h;
		h->ip_tos = tos;
		nv = *(u_int16_t *)h;

		h->ip_sum = pf_cksum_fixup(h->ip_sum, ov, nv, 0);
	}

	/* random-id, but not for fragments */
	if (flags & PFSTATE_RANDOMID && !(h->ip_off & ~htons(IP_DF))) {
		u_int16_t ip_id = h->ip_id;

		h->ip_id = htons(ip_randomid());
		h->ip_sum = pf_cksum_fixup(h->ip_sum, ip_id, h->ip_id, 0);
	}
}

#ifdef INET6
void
pf_scrub_ip6(struct mbuf **m0, u_int8_t min_ttl)
{
	struct mbuf		*m = *m0;
	struct ip6_hdr		*h = mtod(m, struct ip6_hdr *);

	/* Enforce a minimum ttl, may cause endless packet loops */
	if (min_ttl && h->ip6_hlim < min_ttl)
		h->ip6_hlim = min_ttl;
}
#endif
