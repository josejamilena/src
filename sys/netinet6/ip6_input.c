/*	$OpenBSD: ip6_input.c,v 1.170 2016/11/28 10:10:53 mpi Exp $	*/
/*	$KAME: ip6_input.c,v 1.188 2001/03/29 05:34:31 itojun Exp $	*/

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
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
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Copyright (c) 1982, 1986, 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)ip_input.c	8.2 (Berkeley) 1/4/94
 */

#include "pf.h"
#include "carp.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/sysctl.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/timeout.h>
#include <sys/kernel.h>
#include <sys/syslog.h>
#include <sys/task.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/route.h>
#include <net/netisr.h>

#include <netinet/in.h>

#include <netinet/ip.h>

#include <netinet/in_pcb.h>
#include <netinet/ip_var.h>
#include <netinet6/in6_var.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet/icmp6.h>
#include <netinet6/nd6.h>

#include <netinet6/ip6protosw.h>

#include "gif.h"
#include "bpfilter.h"

#ifdef MROUTING
#include <netinet6/ip6_mroute.h>
#endif

#if NPF > 0
#include <net/pfvar.h>
#endif

#if NCARP > 0
#include <netinet/ip_carp.h>
#endif

struct in6_ifaddrhead in6_ifaddr;
struct niqueue ip6intrq = NIQUEUE_INITIALIZER(IFQ_MAXLEN, NETISR_IPV6);

struct ip6stat ip6stat;

void ip6_init2(void *);
int ip6_check_rh0hdr(struct mbuf *, int *);

int ip6_hbhchcheck(struct mbuf *, int *, int *, int *);
int ip6_hopopts_input(u_int32_t *, u_int32_t *, struct mbuf **, int *);
struct mbuf *ip6_pullexthdr(struct mbuf *, size_t, int);

static struct mbuf_queue	ip6send_mq;

static void ip6_send_dispatch(void *);
static struct task ip6send_task =
	TASK_INITIALIZER(ip6_send_dispatch, &ip6send_mq);

/*
 * IP6 initialization: fill in IP6 protocol switch table.
 * All protocols not implemented in kernel go to raw IP6 protocol handler.
 */
void
ip6_init(void)
{
	struct ip6protosw *pr;
	int i;

	pr = (struct ip6protosw *)pffindproto(PF_INET6, IPPROTO_RAW, SOCK_RAW);
	if (pr == NULL)
		panic("ip6_init");
	for (i = 0; i < IPPROTO_MAX; i++)
		ip6_protox[i] = pr - inet6sw;
	for (pr = (struct ip6protosw *)inet6domain.dom_protosw;
	    pr < (struct ip6protosw *)inet6domain.dom_protoswNPROTOSW; pr++)
		if (pr->pr_domain->dom_family == PF_INET6 &&
		    pr->pr_protocol && pr->pr_protocol != IPPROTO_RAW &&
		    pr->pr_protocol < IPPROTO_MAX)
			ip6_protox[pr->pr_protocol] = pr - inet6sw;
	TAILQ_INIT(&in6_ifaddr);
	ip6_randomid_init();
	nd6_init();
	frag6_init();
	ip6_init2(NULL);

	mq_init(&ip6send_mq, 64, IPL_SOFTNET);
}

void
ip6_init2(void *dummy)
{

	/* nd6_timer_init */
	bzero(&nd6_timer_ch, sizeof(nd6_timer_ch));
	timeout_set(&nd6_timer_ch, nd6_timer, NULL);
	timeout_add_sec(&nd6_timer_ch, 1);
}

/*
 * IP6 input interrupt handling. Just pass the packet to ip6_input.
 */
void
ip6intr(void)
{
	struct mbuf *m;

	while ((m = niq_dequeue(&ip6intrq)) != NULL)
		ip6_input(m);
}

void
ip6_input(struct mbuf *m)
{
	struct ifnet *ifp;
	struct ip6_hdr *ip6;
	struct sockaddr_in6 sin6;
	struct rtentry *rt = NULL;
	int off, nest;
	u_int16_t src_scope, dst_scope;
	int nxt, ours = 0;
#if NPF > 0
	struct in6_addr odst;
#endif
	int srcrt = 0;

	ifp = if_get(m->m_pkthdr.ph_ifidx);
	if (ifp == NULL)
		goto bad;

	if (m->m_flags & M_EXT) {
		if (m->m_next)
			ip6stat.ip6s_mext2m++;
		else
			ip6stat.ip6s_mext1++;
	} else {
		if (m->m_next) {
			int ifidx = m->m_pkthdr.ph_ifidx;
			if (m->m_flags & M_LOOP)
				ifidx = rtable_loindex(m->m_pkthdr.ph_rtableid);
			if (ifidx < nitems(ip6stat.ip6s_m2m))
				ip6stat.ip6s_m2m[ifidx]++;
			else
				ip6stat.ip6s_m2m[0]++;
		} else
			ip6stat.ip6s_m1++;
	}

	ip6stat.ip6s_total++;

	if (m->m_len < sizeof(struct ip6_hdr)) {
		if ((m = m_pullup(m, sizeof(struct ip6_hdr))) == NULL) {
			ip6stat.ip6s_toosmall++;
			if_put(ifp);
			return;
		}
	}

	ip6 = mtod(m, struct ip6_hdr *);

	if ((ip6->ip6_vfc & IPV6_VERSION_MASK) != IPV6_VERSION) {
		ip6stat.ip6s_badvers++;
		goto bad;
	}

#if NCARP > 0
	if (ifp->if_type == IFT_CARP && ip6->ip6_nxt != IPPROTO_ICMPV6 &&
	    carp_lsdrop(m, AF_INET6, ip6->ip6_src.s6_addr32,
	    ip6->ip6_dst.s6_addr32))
		goto bad;
#endif
	ip6stat.ip6s_nxthist[ip6->ip6_nxt]++;

	/*
	 * Check against address spoofing/corruption.
	 */
	if (IN6_IS_ADDR_MULTICAST(&ip6->ip6_src) ||
	    IN6_IS_ADDR_UNSPECIFIED(&ip6->ip6_dst)) {
		/*
		 * XXX: "badscope" is not very suitable for a multicast source.
		 */
		ip6stat.ip6s_badscope++;
		goto bad;
	}
	if ((IN6_IS_ADDR_LOOPBACK(&ip6->ip6_src) ||
	    IN6_IS_ADDR_LOOPBACK(&ip6->ip6_dst)) &&
	    (ifp->if_flags & IFF_LOOPBACK) == 0) {
		    ip6stat.ip6s_badscope++;
		    goto bad;
	}
	/* Drop packets if interface ID portion is already filled. */
	if (((IN6_IS_SCOPE_EMBED(&ip6->ip6_src) && ip6->ip6_src.s6_addr16[1]) ||
	    (IN6_IS_SCOPE_EMBED(&ip6->ip6_dst) && ip6->ip6_dst.s6_addr16[1])) &&
	    (ifp->if_flags & IFF_LOOPBACK) == 0) {
		ip6stat.ip6s_badscope++;
		goto bad;
	}
	if (IN6_IS_ADDR_MC_INTFACELOCAL(&ip6->ip6_dst) &&
	    !(m->m_flags & M_LOOP)) {
		/*
		 * In this case, the packet should come from the loopback
		 * interface.  However, we cannot just check the if_flags,
		 * because ip6_mloopback() passes the "actual" interface
		 * as the outgoing/incoming interface.
		 */
		ip6stat.ip6s_badscope++;
		goto bad;
	}

	/*
	 * The following check is not documented in specs.  A malicious
	 * party may be able to use IPv4 mapped addr to confuse tcp/udp stack
	 * and bypass security checks (act as if it was from 127.0.0.1 by using
	 * IPv6 src ::ffff:127.0.0.1).  Be cautious.
	 *
	 * This check chokes if we are in an SIIT cloud.  As none of BSDs
	 * support IPv4-less kernel compilation, we cannot support SIIT
	 * environment at all.  So, it makes more sense for us to reject any
	 * malicious packets for non-SIIT environment, than try to do a
	 * partial support for SIIT environment.
	 */
	if (IN6_IS_ADDR_V4MAPPED(&ip6->ip6_src) ||
	    IN6_IS_ADDR_V4MAPPED(&ip6->ip6_dst)) {
		ip6stat.ip6s_badscope++;
		goto bad;
	}

	/*
	 * Reject packets with IPv4 compatible addresses (auto tunnel).
	 *
	 * The code forbids automatic tunneling as per RFC4213.
	 */
	if (IN6_IS_ADDR_V4COMPAT(&ip6->ip6_src) ||
	    IN6_IS_ADDR_V4COMPAT(&ip6->ip6_dst)) {
		ip6stat.ip6s_badscope++;
		goto bad;
	}

	/*
	 * If the packet has been received on a loopback interface it
	 * can be destinated to any local address, not necessarily to
	 * an address configured on `ifp'.
	 */
	if (ifp->if_flags & IFF_LOOPBACK) {
		if (IN6_IS_SCOPE_EMBED(&ip6->ip6_src)) {
			src_scope = ip6->ip6_src.s6_addr16[1];
			ip6->ip6_src.s6_addr16[1] = 0;
		}
		if (IN6_IS_SCOPE_EMBED(&ip6->ip6_dst)) {
			dst_scope = ip6->ip6_dst.s6_addr16[1];
			ip6->ip6_dst.s6_addr16[1] = 0;
		}
	}

#if NPF > 0
        /*
         * Packet filter
         */
	odst = ip6->ip6_dst;
	if (pf_test(AF_INET6, PF_IN, ifp, &m) != PF_PASS)
		goto bad;
	if (m == NULL)
		goto bad;

	ip6 = mtod(m, struct ip6_hdr *);
	srcrt = !IN6_ARE_ADDR_EQUAL(&odst, &ip6->ip6_dst);
#endif

	/*
	 * Without embedded scope ID we cannot find link-local
	 * addresses in the routing table.
	 */
	if (ifp->if_flags & IFF_LOOPBACK) {
		if (IN6_IS_SCOPE_EMBED(&ip6->ip6_src))
			ip6->ip6_src.s6_addr16[1] = src_scope;
		if (IN6_IS_SCOPE_EMBED(&ip6->ip6_dst))
			ip6->ip6_dst.s6_addr16[1] = dst_scope;
	} else {
		if (IN6_IS_SCOPE_EMBED(&ip6->ip6_src))
			ip6->ip6_src.s6_addr16[1] = htons(ifp->if_index);
		if (IN6_IS_SCOPE_EMBED(&ip6->ip6_dst))
			ip6->ip6_dst.s6_addr16[1] = htons(ifp->if_index);
	}

	/*
	 * Be more secure than RFC5095 and scan for type 0 routing headers.
	 * If pf has already scanned the header chain, do not do it twice.
	 */
	if (!(m->m_pkthdr.pf.flags & PF_TAG_PROCESSED) &&
	    ip6_check_rh0hdr(m, &off)) {
		ip6stat.ip6s_badoptions++;
		icmp6_error(m, ICMP6_PARAM_PROB, ICMP6_PARAMPROB_HEADER, off);
		/* m is already freed */
		if_put(ifp);
		return;
	}

	if (IN6_IS_ADDR_LOOPBACK(&ip6->ip6_src) ||
	    IN6_IS_ADDR_LOOPBACK(&ip6->ip6_dst)) {
		ours = 1;
		goto hbhcheck;
	}

#if NPF > 0
	if (pf_ouraddr(m) == 1) {
		ours = 1;
		goto hbhcheck;
	}
#endif

	/*
	 * Multicast check
	 */
	if (IN6_IS_ADDR_MULTICAST(&ip6->ip6_dst)) {
		/*
		 * Make sure M_MCAST is set.  It should theoretically
		 * already be there, but let's play safe because upper
		 * layers check for this flag.
		 */
		m->m_flags |= M_MCAST;

		/*
		 * See if we belong to the destination multicast group on the
		 * arrival interface.
		 */
		if (in6_hasmulti(&ip6->ip6_dst, ifp))
			ours = 1;

#ifdef MROUTING
		if (ip6_mforwarding && ip6_mrouter) {
			if (ip6_hbhchcheck(m, &off, &nxt, &ours)) {
				rtfree(rt);
				if_put(ifp);
				return;	/* m have already been freed */
			}

			ip6 = mtod(m, struct ip6_hdr *);

			/*
			 * If we are acting as a multicast router, all
			 * incoming multicast packets are passed to the
			 * kernel-level multicast forwarding function.
			 * The packet is returned (relatively) intact; if
			 * ip6_mforward() returns a non-zero value, the packet
			 * must be discarded, else it may be accepted below.
			 */
		    	if (ip6_mforward(ip6, ifp, m)) {
				ip6stat.ip6s_cantforward++;
				goto bad;
			}

			if (!ours)
				goto bad;
			goto ours;
		}
#endif
		if (!ours) {
			ip6stat.ip6s_notmember++;
			if (!IN6_IS_ADDR_MC_LINKLOCAL(&ip6->ip6_dst))
				ip6stat.ip6s_cantforward++;
			goto bad;
		}
		goto hbhcheck;
	}


	/*
	 *  Unicast check
	 */
	memset(&sin6, 0, sizeof(struct sockaddr_in6));
	sin6.sin6_len = sizeof(struct sockaddr_in6);
	sin6.sin6_family = AF_INET6;
	sin6.sin6_addr = ip6->ip6_dst;
	rt = rtalloc_mpath(sin6tosa(&sin6), &ip6->ip6_src.s6_addr32[0],
	    m->m_pkthdr.ph_rtableid);

	/*
	 * Accept the packet if the route to the destination is marked
	 * as local.
	 */
	if (rtisvalid(rt) && ISSET(rt->rt_flags, RTF_LOCAL)) {
		struct in6_ifaddr *ia6 = ifatoia6(rt->rt_ifa);
		if (ia6->ia6_flags & IN6_IFF_ANYCAST)
			m->m_flags |= M_ACAST;
		/*
		 * packets to a tentative, duplicated, or somehow invalid
		 * address must not be accepted.
		 */
		if ((ia6->ia6_flags & (IN6_IFF_TENTATIVE|IN6_IFF_DUPLICATED))) {
			char src[INET6_ADDRSTRLEN], dst[INET6_ADDRSTRLEN];

			inet_ntop(AF_INET6, &ip6->ip6_src, src, sizeof(src));
			inet_ntop(AF_INET6, &ip6->ip6_dst, dst, sizeof(dst));
			/* address is not ready, so discard the packet. */
			nd6log((LOG_INFO,
			    "ip6_input: packet to an unready address %s->%s\n",
			    src, dst));

			goto bad;
		} else {
			/* this address is ready */
			ours = 1;
			goto hbhcheck;
		}
	}

#if NCARP > 0
	if (ifp->if_type == IFT_CARP && ip6->ip6_nxt == IPPROTO_ICMPV6 &&
	    carp_lsdrop(m, AF_INET6, ip6->ip6_src.s6_addr32,
	    ip6->ip6_dst.s6_addr32))
		goto bad;
#endif
	/*
	 * Now there is no reason to process the packet if it's not our own
	 * and we're not a router.
	 */
	if (!ip6_forwarding) {
		ip6stat.ip6s_cantforward++;
		goto bad;
	}

  hbhcheck:

	if (ip6_hbhchcheck(m, &off, &nxt, &ours)) {
		rtfree(rt);
		if_put(ifp);
		return;	/* m have already been freed */
	}

	/* adjust pointer */
	ip6 = mtod(m, struct ip6_hdr *);

	/*
	 * Forward if desirable.
	 */
	if (!ours) {
		ip6_forward(m, rt, srcrt);
		if_put(ifp);
		return;
	}
#ifdef MROUTING
  ours:
#endif
	/* pf might have changed things */
	in6_proto_cksum_out(m, NULL);

	ip6 = mtod(m, struct ip6_hdr *);

	/*
	 * Tell launch routine the next header
	 */
	ip6stat.ip6s_delivered++;
	nest = 0;

	while (nxt != IPPROTO_DONE) {
		if (ip6_hdrnestlimit && (++nest > ip6_hdrnestlimit)) {
			ip6stat.ip6s_toomanyhdr++;
			goto bad;
		}

		/*
		 * protection against faulty packet - there should be
		 * more sanity checks in header chain processing.
		 */
		if (m->m_pkthdr.len < off) {
			ip6stat.ip6s_tooshort++;
			goto bad;
		}

		/* draft-itojun-ipv6-tcp-to-anycast */
		if (ISSET(m->m_flags, M_ACAST) && (nxt == IPPROTO_TCP)) {
			if (m->m_len >= sizeof(struct ip6_hdr)) {
				icmp6_error(m, ICMP6_DST_UNREACH,
					ICMP6_DST_UNREACH_ADDR,
					offsetof(struct ip6_hdr, ip6_dst));
				break;
			} else
				goto bad;
		}

		nxt = (*inet6sw[ip6_protox[nxt]].pr_input)(&m, &off, nxt);
	}
	rtfree(rt);
	if_put(ifp);
	return;
 bad:
	rtfree(rt);
	if_put(ifp);
	m_freem(m);
}

int
ip6_hbhchcheck(struct mbuf *m, int *offp, int *nxtp, int *oursp)
{
	struct ip6_hdr *ip6;
	u_int32_t plen, rtalert = ~0;

	ip6 = mtod(m, struct ip6_hdr *);

	/*
	 * Process Hop-by-Hop options header if it's contained.
	 * m may be modified in ip6_hopopts_input().
	 * If a JumboPayload option is included, plen will also be modified.
	 */
	plen = (u_int32_t)ntohs(ip6->ip6_plen);
	*offp = sizeof(struct ip6_hdr);
	if (ip6->ip6_nxt == IPPROTO_HOPOPTS) {
		struct ip6_hbh *hbh;

		if (ip6_hopopts_input(&plen, &rtalert, &m, offp)) {
			return (-1);	/* m have already been freed */
		}

		/* adjust pointer */
		ip6 = mtod(m, struct ip6_hdr *);

		/*
		 * if the payload length field is 0 and the next header field
		 * indicates Hop-by-Hop Options header, then a Jumbo Payload
		 * option MUST be included.
		 */
		if (ip6->ip6_plen == 0 && plen == 0) {
			/*
			 * Note that if a valid jumbo payload option is
			 * contained, ip6_hopopts_input() must set a valid
			 * (non-zero) payload length to the variable plen.
			 */
			ip6stat.ip6s_badoptions++;
			icmp6_error(m, ICMP6_PARAM_PROB,
				    ICMP6_PARAMPROB_HEADER,
				    (caddr_t)&ip6->ip6_plen - (caddr_t)ip6);
			return (-1);
		}
		IP6_EXTHDR_GET(hbh, struct ip6_hbh *, m, sizeof(struct ip6_hdr),
			sizeof(struct ip6_hbh));
		if (hbh == NULL) {
			ip6stat.ip6s_tooshort++;
			return (-1);
		}
		*nxtp = hbh->ip6h_nxt;

		/*
		 * accept the packet if a router alert option is included
		 * and we act as an IPv6 router.
		 */
		if (rtalert != ~0 && ip6_forwarding)
			*oursp = 1;
	} else
		*nxtp = ip6->ip6_nxt;

	/*
	 * Check that the amount of data in the buffers
	 * is as at least much as the IPv6 header would have us expect.
	 * Trim mbufs if longer than we expect.
	 * Drop packet if shorter than we expect.
	 */
	if (m->m_pkthdr.len - sizeof(struct ip6_hdr) < plen) {
		ip6stat.ip6s_tooshort++;
		m_freem(m);
		return (-1);
	}
	if (m->m_pkthdr.len > sizeof(struct ip6_hdr) + plen) {
		if (m->m_len == m->m_pkthdr.len) {
			m->m_len = sizeof(struct ip6_hdr) + plen;
			m->m_pkthdr.len = sizeof(struct ip6_hdr) + plen;
		} else {
			m_adj(m,
			    sizeof(struct ip6_hdr) + plen - m->m_pkthdr.len);
		}
	}

	return (0);
}

/* scan packet for RH0 routing header. Mostly stolen from pf.c:pf_test() */
int
ip6_check_rh0hdr(struct mbuf *m, int *offp)
{
	struct ip6_hdr *ip6 = mtod(m, struct ip6_hdr *);
	struct ip6_rthdr rthdr;
	struct ip6_ext opt6;
	u_int8_t proto = ip6->ip6_nxt;
	int done = 0, lim, off, rh_cnt = 0;

	off = ((caddr_t)ip6 - m->m_data) + sizeof(struct ip6_hdr);
	lim = min(m->m_pkthdr.len, ntohs(ip6->ip6_plen) + sizeof(*ip6));
	do {
		switch (proto) {
		case IPPROTO_ROUTING:
			*offp = off;
			if (rh_cnt++) {
				/* more then one rh header present */
				return (1);
			}

			if (off + sizeof(rthdr) > lim) {
				/* packet to short to make sense */
				return (1);
			}

			m_copydata(m, off, sizeof(rthdr), (caddr_t)&rthdr);

			if (rthdr.ip6r_type == IPV6_RTHDR_TYPE_0) {
				*offp += offsetof(struct ip6_rthdr, ip6r_type);
				return (1);
			}

			off += (rthdr.ip6r_len + 1) * 8;
			proto = rthdr.ip6r_nxt;
			break;
		case IPPROTO_AH:
		case IPPROTO_HOPOPTS:
		case IPPROTO_DSTOPTS:
			/* get next header and header length */
			if (off + sizeof(opt6) > lim) {
				/*
				 * Packet to short to make sense, we could
				 * reject the packet but as a router we
				 * should not do that so forward it.
				 */
				return (0);
			}

			m_copydata(m, off, sizeof(opt6), (caddr_t)&opt6);

			if (proto == IPPROTO_AH)
				off += (opt6.ip6e_len + 2) * 4;
			else
				off += (opt6.ip6e_len + 1) * 8;
			proto = opt6.ip6e_nxt;
			break;
		case IPPROTO_FRAGMENT:
		default:
			/* end of header stack */
			done = 1;
			break;
		}
	} while (!done);

	return (0);
}

/*
 * Hop-by-Hop options header processing. If a valid jumbo payload option is
 * included, the real payload length will be stored in plenp.
 *
 * rtalertp - XXX: should be stored in a more smart way
 */
int
ip6_hopopts_input(u_int32_t *plenp, u_int32_t *rtalertp, struct mbuf **mp,
    int *offp)
{
	struct mbuf *m = *mp;
	int off = *offp, hbhlen;
	struct ip6_hbh *hbh;

	/* validation of the length of the header */
	IP6_EXTHDR_GET(hbh, struct ip6_hbh *, m,
		sizeof(struct ip6_hdr), sizeof(struct ip6_hbh));
	if (hbh == NULL) {
		ip6stat.ip6s_tooshort++;
		return -1;
	}
	hbhlen = (hbh->ip6h_len + 1) << 3;
	IP6_EXTHDR_GET(hbh, struct ip6_hbh *, m, sizeof(struct ip6_hdr),
		hbhlen);
	if (hbh == NULL) {
		ip6stat.ip6s_tooshort++;
		return -1;
	}
	off += hbhlen;
	hbhlen -= sizeof(struct ip6_hbh);

	if (ip6_process_hopopts(m, (u_int8_t *)hbh + sizeof(struct ip6_hbh),
				hbhlen, rtalertp, plenp) < 0)
		return (-1);

	*offp = off;
	*mp = m;
	return (0);
}

/*
 * Search header for all Hop-by-hop options and process each option.
 * This function is separate from ip6_hopopts_input() in order to
 * handle a case where the sending node itself process its hop-by-hop
 * options header. In such a case, the function is called from ip6_output().
 *
 * The function assumes that hbh header is located right after the IPv6 header
 * (RFC2460 p7), opthead is pointer into data content in m, and opthead to
 * opthead + hbhlen is located in continuous memory region.
 */
int
ip6_process_hopopts(struct mbuf *m, u_int8_t *opthead, int hbhlen,
    u_int32_t *rtalertp, u_int32_t *plenp)
{
	struct ip6_hdr *ip6;
	int optlen = 0;
	u_int8_t *opt = opthead;
	u_int16_t rtalert_val;
	u_int32_t jumboplen;
	const int erroff = sizeof(struct ip6_hdr) + sizeof(struct ip6_hbh);

	for (; hbhlen > 0; hbhlen -= optlen, opt += optlen) {
		switch (*opt) {
		case IP6OPT_PAD1:
			optlen = 1;
			break;
		case IP6OPT_PADN:
			if (hbhlen < IP6OPT_MINLEN) {
				ip6stat.ip6s_toosmall++;
				goto bad;
			}
			optlen = *(opt + 1) + 2;
			break;
		case IP6OPT_ROUTER_ALERT:
			/* XXX may need check for alignment */
			if (hbhlen < IP6OPT_RTALERT_LEN) {
				ip6stat.ip6s_toosmall++;
				goto bad;
			}
			if (*(opt + 1) != IP6OPT_RTALERT_LEN - 2) {
				/* XXX stat */
				icmp6_error(m, ICMP6_PARAM_PROB,
				    ICMP6_PARAMPROB_HEADER,
				    erroff + opt + 1 - opthead);
				return (-1);
			}
			optlen = IP6OPT_RTALERT_LEN;
			bcopy((caddr_t)(opt + 2), (caddr_t)&rtalert_val, 2);
			*rtalertp = ntohs(rtalert_val);
			break;
		case IP6OPT_JUMBO:
			/* XXX may need check for alignment */
			if (hbhlen < IP6OPT_JUMBO_LEN) {
				ip6stat.ip6s_toosmall++;
				goto bad;
			}
			if (*(opt + 1) != IP6OPT_JUMBO_LEN - 2) {
				/* XXX stat */
				icmp6_error(m, ICMP6_PARAM_PROB,
				    ICMP6_PARAMPROB_HEADER,
				    erroff + opt + 1 - opthead);
				return (-1);
			}
			optlen = IP6OPT_JUMBO_LEN;

			/*
			 * IPv6 packets that have non 0 payload length
			 * must not contain a jumbo payload option.
			 */
			ip6 = mtod(m, struct ip6_hdr *);
			if (ip6->ip6_plen) {
				ip6stat.ip6s_badoptions++;
				icmp6_error(m, ICMP6_PARAM_PROB,
				    ICMP6_PARAMPROB_HEADER,
				    erroff + opt - opthead);
				return (-1);
			}

			/*
			 * We may see jumbolen in unaligned location, so
			 * we'd need to perform bcopy().
			 */
			bcopy(opt + 2, &jumboplen, sizeof(jumboplen));
			jumboplen = (u_int32_t)htonl(jumboplen);

#if 1
			/*
			 * if there are multiple jumbo payload options,
			 * *plenp will be non-zero and the packet will be
			 * rejected.
			 * the behavior may need some debate in ipngwg -
			 * multiple options does not make sense, however,
			 * there's no explicit mention in specification.
			 */
			if (*plenp != 0) {
				ip6stat.ip6s_badoptions++;
				icmp6_error(m, ICMP6_PARAM_PROB,
				    ICMP6_PARAMPROB_HEADER,
				    erroff + opt + 2 - opthead);
				return (-1);
			}
#endif

			/*
			 * jumbo payload length must be larger than 65535.
			 */
			if (jumboplen <= IPV6_MAXPACKET) {
				ip6stat.ip6s_badoptions++;
				icmp6_error(m, ICMP6_PARAM_PROB,
				    ICMP6_PARAMPROB_HEADER,
				    erroff + opt + 2 - opthead);
				return (-1);
			}
			*plenp = jumboplen;

			break;
		default:		/* unknown option */
			if (hbhlen < IP6OPT_MINLEN) {
				ip6stat.ip6s_toosmall++;
				goto bad;
			}
			optlen = ip6_unknown_opt(opt, m,
			    erroff + opt - opthead);
			if (optlen == -1)
				return (-1);
			optlen += 2;
			break;
		}
	}

	return (0);

  bad:
	m_freem(m);
	return (-1);
}

/*
 * Unknown option processing.
 * The third argument `off' is the offset from the IPv6 header to the option,
 * which allows returning an ICMPv6 error even if the IPv6 header and the
 * option header are not continuous.
 */
int
ip6_unknown_opt(u_int8_t *optp, struct mbuf *m, int off)
{
	struct ip6_hdr *ip6;

	switch (IP6OPT_TYPE(*optp)) {
	case IP6OPT_TYPE_SKIP: /* ignore the option */
		return ((int)*(optp + 1));
	case IP6OPT_TYPE_DISCARD:	/* silently discard */
		m_freem(m);
		return (-1);
	case IP6OPT_TYPE_FORCEICMP: /* send ICMP even if multicasted */
		ip6stat.ip6s_badoptions++;
		icmp6_error(m, ICMP6_PARAM_PROB, ICMP6_PARAMPROB_OPTION, off);
		return (-1);
	case IP6OPT_TYPE_ICMP: /* send ICMP if not multicasted */
		ip6stat.ip6s_badoptions++;
		ip6 = mtod(m, struct ip6_hdr *);
		if (IN6_IS_ADDR_MULTICAST(&ip6->ip6_dst) ||
		    (m->m_flags & (M_BCAST|M_MCAST)))
			m_freem(m);
		else
			icmp6_error(m, ICMP6_PARAM_PROB,
				    ICMP6_PARAMPROB_OPTION, off);
		return (-1);
	}

	m_freem(m);		/* XXX: NOTREACHED */
	return (-1);
}

/*
 * Create the "control" list for this pcb.
 *
 * The routine will be called from upper layer handlers like tcp6_input().
 * Thus the routine assumes that the caller (tcp6_input) have already
 * called IP6_EXTHDR_CHECK() and all the extension headers are located in the
 * very first mbuf on the mbuf chain.
 * We may want to add some infinite loop prevention or sanity checks for safety.
 * (This applies only when you are using KAME mbuf chain restriction, i.e.
 * you are using IP6_EXTHDR_CHECK() not m_pulldown())
 */
void
ip6_savecontrol(struct inpcb *in6p, struct mbuf *m, struct mbuf **mp)
{
	struct ip6_hdr *ip6 = mtod(m, struct ip6_hdr *);

#ifdef SO_TIMESTAMP
	if (in6p->inp_socket->so_options & SO_TIMESTAMP) {
		struct timeval tv;

		microtime(&tv);
		*mp = sbcreatecontrol((caddr_t) &tv, sizeof(tv),
		    SCM_TIMESTAMP, SOL_SOCKET);
		if (*mp)
			mp = &(*mp)->m_next;
	}
#endif

	/* RFC 2292 sec. 5 */
	if ((in6p->inp_flags & IN6P_PKTINFO) != 0) {
		struct in6_pktinfo pi6;
		bcopy(&ip6->ip6_dst, &pi6.ipi6_addr, sizeof(struct in6_addr));
		if (IN6_IS_SCOPE_EMBED(&pi6.ipi6_addr))
			pi6.ipi6_addr.s6_addr16[1] = 0;
		pi6.ipi6_ifindex = m ? m->m_pkthdr.ph_ifidx : 0;
		*mp = sbcreatecontrol((caddr_t) &pi6,
		    sizeof(struct in6_pktinfo),
		    IPV6_PKTINFO, IPPROTO_IPV6);
		if (*mp)
			mp = &(*mp)->m_next;
	}

	if ((in6p->inp_flags & IN6P_HOPLIMIT) != 0) {
		int hlim = ip6->ip6_hlim & 0xff;
		*mp = sbcreatecontrol((caddr_t) &hlim, sizeof(int),
		    IPV6_HOPLIMIT, IPPROTO_IPV6);
		if (*mp)
			mp = &(*mp)->m_next;
	}

	if ((in6p->inp_flags & IN6P_TCLASS) != 0) {
		u_int32_t flowinfo;
		int tclass;

		flowinfo = (u_int32_t)ntohl(ip6->ip6_flow & IPV6_FLOWINFO_MASK);
		flowinfo >>= 20;

		tclass = flowinfo & 0xff;
		*mp = sbcreatecontrol((caddr_t)&tclass, sizeof(tclass),
		    IPV6_TCLASS, IPPROTO_IPV6);
		if (*mp)
			mp = &(*mp)->m_next;
	}

	/*
	 * IPV6_HOPOPTS socket option.  Recall that we required super-user
	 * privilege for the option (see ip6_ctloutput), but it might be too
	 * strict, since there might be some hop-by-hop options which can be
	 * returned to normal user.
	 * See also RFC 2292 section 6 (or RFC 3542 section 8).
	 */
	if ((in6p->inp_flags & IN6P_HOPOPTS) != 0) {
		/*
		 * Check if a hop-by-hop options header is contained in the
		 * received packet, and if so, store the options as ancillary
		 * data. Note that a hop-by-hop options header must be
		 * just after the IPv6 header, which is assured through the
		 * IPv6 input processing.
		 */
		struct ip6_hdr *ip6 = mtod(m, struct ip6_hdr *);
		if (ip6->ip6_nxt == IPPROTO_HOPOPTS) {
			struct ip6_hbh *hbh;
			int hbhlen = 0;
			struct mbuf *ext;

			ext = ip6_pullexthdr(m, sizeof(struct ip6_hdr),
			    ip6->ip6_nxt);
			if (ext == NULL) {
				ip6stat.ip6s_tooshort++;
				return;
			}
			hbh = mtod(ext, struct ip6_hbh *);
			hbhlen = (hbh->ip6h_len + 1) << 3;
			if (hbhlen != ext->m_len) {
				m_freem(ext);
				ip6stat.ip6s_tooshort++;
				return;
			}

			/*
			 * XXX: We copy the whole header even if a
			 * jumbo payload option is included, the option which
			 * is to be removed before returning according to
			 * RFC2292.
			 * Note: this constraint is removed in RFC3542.
			 */
			*mp = sbcreatecontrol((caddr_t)hbh, hbhlen,
			    IPV6_HOPOPTS,
			    IPPROTO_IPV6);
			if (*mp)
				mp = &(*mp)->m_next;
			m_freem(ext);
		}
	}

	/* IPV6_DSTOPTS and IPV6_RTHDR socket options */
	if ((in6p->inp_flags & (IN6P_RTHDR | IN6P_DSTOPTS)) != 0) {
		struct ip6_hdr *ip6 = mtod(m, struct ip6_hdr *);
		int nxt = ip6->ip6_nxt, off = sizeof(struct ip6_hdr);

		/*
		 * Search for destination options headers or routing
		 * header(s) through the header chain, and stores each
		 * header as ancillary data.
		 * Note that the order of the headers remains in
		 * the chain of ancillary data.
		 */
		while (1) {	/* is explicit loop prevention necessary? */
			struct ip6_ext *ip6e = NULL;
			int elen;
			struct mbuf *ext = NULL;

			/*
			 * if it is not an extension header, don't try to
			 * pull it from the chain.
			 */
			switch (nxt) {
			case IPPROTO_DSTOPTS:
			case IPPROTO_ROUTING:
			case IPPROTO_HOPOPTS:
			case IPPROTO_AH: /* is it possible? */
				break;
			default:
				goto loopend;
			}

			ext = ip6_pullexthdr(m, off, nxt);
			if (ext == NULL) {
				ip6stat.ip6s_tooshort++;
				return;
			}
			ip6e = mtod(ext, struct ip6_ext *);
			if (nxt == IPPROTO_AH)
				elen = (ip6e->ip6e_len + 2) << 2;
			else
				elen = (ip6e->ip6e_len + 1) << 3;
			if (elen != ext->m_len) {
				m_freem(ext);
				ip6stat.ip6s_tooshort++;
				return;
			}

			switch (nxt) {
			case IPPROTO_DSTOPTS:
				if (!(in6p->inp_flags & IN6P_DSTOPTS))
					break;

				*mp = sbcreatecontrol((caddr_t)ip6e, elen,
				    IPV6_DSTOPTS,
				    IPPROTO_IPV6);
				if (*mp)
					mp = &(*mp)->m_next;
				break;

			case IPPROTO_ROUTING:
				if (!(in6p->inp_flags & IN6P_RTHDR))
					break;

				*mp = sbcreatecontrol((caddr_t)ip6e, elen,
				    IPV6_RTHDR,
				    IPPROTO_IPV6);
				if (*mp)
					mp = &(*mp)->m_next;
				break;

			case IPPROTO_HOPOPTS:
			case IPPROTO_AH: /* is it possible? */
				break;

			default:
				/*
				 * other cases have been filtered in the above.
				 * none will visit this case.  here we supply
				 * the code just in case (nxt overwritten or
				 * other cases).
				 */
				m_freem(ext);
				goto loopend;

			}

			/* proceed with the next header. */
			off += elen;
			nxt = ip6e->ip6e_nxt;
			ip6e = NULL;
			m_freem(ext);
			ext = NULL;
		}
loopend:
		;
	}
}

/*
 * pull single extension header from mbuf chain.  returns single mbuf that
 * contains the result, or NULL on error.
 */
struct mbuf *
ip6_pullexthdr(struct mbuf *m, size_t off, int nxt)
{
	struct ip6_ext ip6e;
	size_t elen;
	struct mbuf *n;

#ifdef DIAGNOSTIC
	switch (nxt) {
	case IPPROTO_DSTOPTS:
	case IPPROTO_ROUTING:
	case IPPROTO_HOPOPTS:
	case IPPROTO_AH: /* is it possible? */
		break;
	default:
		printf("ip6_pullexthdr: invalid nxt=%d\n", nxt);
	}
#endif

	m_copydata(m, off, sizeof(ip6e), (caddr_t)&ip6e);
	if (nxt == IPPROTO_AH)
		elen = (ip6e.ip6e_len + 2) << 2;
	else
		elen = (ip6e.ip6e_len + 1) << 3;

	MGET(n, M_DONTWAIT, MT_DATA);
	if (n && elen >= MLEN) {
		MCLGET(n, M_DONTWAIT);
		if ((n->m_flags & M_EXT) == 0) {
			m_free(n);
			n = NULL;
		}
	}
	if (!n)
		return NULL;

	n->m_len = 0;
	if (elen >= M_TRAILINGSPACE(n)) {
		m_free(n);
		return NULL;
	}

	m_copydata(m, off, elen, mtod(n, caddr_t));
	n->m_len = elen;
	return n;
}

/*
 * Get pointer to the previous header followed by the header
 * currently processed.
 * XXX: This function supposes that
 *	M includes all headers,
 *	the next header field and the header length field of each header
 *	are valid, and
 *	the sum of each header length equals to OFF.
 * Because of these assumptions, this function must be called very
 * carefully. Moreover, it will not be used in the near future when
 * we develop `neater' mechanism to process extension headers.
 */
u_int8_t *
ip6_get_prevhdr(struct mbuf *m, int off)
{
	struct ip6_hdr *ip6 = mtod(m, struct ip6_hdr *);

	if (off == sizeof(struct ip6_hdr))
		return (&ip6->ip6_nxt);
	else {
		int len, nxt;
		struct ip6_ext *ip6e = NULL;

		nxt = ip6->ip6_nxt;
		len = sizeof(struct ip6_hdr);
		while (len < off) {
			ip6e = (struct ip6_ext *)(mtod(m, caddr_t) + len);

			switch (nxt) {
			case IPPROTO_FRAGMENT:
				len += sizeof(struct ip6_frag);
				break;
			case IPPROTO_AH:
				len += (ip6e->ip6e_len + 2) << 2;
				break;
			default:
				len += (ip6e->ip6e_len + 1) << 3;
				break;
			}
			nxt = ip6e->ip6e_nxt;
		}
		if (ip6e)
			return (&ip6e->ip6e_nxt);
		else
			return NULL;
	}
}

/*
 * get next header offset.  m will be retained.
 */
int
ip6_nexthdr(struct mbuf *m, int off, int proto, int *nxtp)
{
	struct ip6_hdr ip6;
	struct ip6_ext ip6e;
	struct ip6_frag fh;

	/* just in case */
	if (m == NULL)
		panic("ip6_nexthdr: m == NULL");
	if ((m->m_flags & M_PKTHDR) == 0 || m->m_pkthdr.len < off)
		return -1;

	switch (proto) {
	case IPPROTO_IPV6:
		if (m->m_pkthdr.len < off + sizeof(ip6))
			return -1;
		m_copydata(m, off, sizeof(ip6), (caddr_t)&ip6);
		if (nxtp)
			*nxtp = ip6.ip6_nxt;
		off += sizeof(ip6);
		return off;

	case IPPROTO_FRAGMENT:
		/*
		 * terminate parsing if it is not the first fragment,
		 * it does not make sense to parse through it.
		 */
		if (m->m_pkthdr.len < off + sizeof(fh))
			return -1;
		m_copydata(m, off, sizeof(fh), (caddr_t)&fh);
		if ((fh.ip6f_offlg & IP6F_OFF_MASK) != 0)
			return -1;
		if (nxtp)
			*nxtp = fh.ip6f_nxt;
		off += sizeof(struct ip6_frag);
		return off;

	case IPPROTO_AH:
		if (m->m_pkthdr.len < off + sizeof(ip6e))
			return -1;
		m_copydata(m, off, sizeof(ip6e), (caddr_t)&ip6e);
		if (nxtp)
			*nxtp = ip6e.ip6e_nxt;
		off += (ip6e.ip6e_len + 2) << 2;
		if (m->m_pkthdr.len < off)
			return -1;
		return off;

	case IPPROTO_HOPOPTS:
	case IPPROTO_ROUTING:
	case IPPROTO_DSTOPTS:
		if (m->m_pkthdr.len < off + sizeof(ip6e))
			return -1;
		m_copydata(m, off, sizeof(ip6e), (caddr_t)&ip6e);
		if (nxtp)
			*nxtp = ip6e.ip6e_nxt;
		off += (ip6e.ip6e_len + 1) << 3;
		if (m->m_pkthdr.len < off)
			return -1;
		return off;

	case IPPROTO_NONE:
	case IPPROTO_ESP:
	case IPPROTO_IPCOMP:
		/* give up */
		return -1;

	default:
		return -1;
	}

	return -1;
}

/*
 * get offset for the last header in the chain.  m will be kept untainted.
 */
int
ip6_lasthdr(struct mbuf *m, int off, int proto, int *nxtp)
{
	int newoff;
	int nxt;

	if (!nxtp) {
		nxt = -1;
		nxtp = &nxt;
	}
	while (1) {
		newoff = ip6_nexthdr(m, off, proto, nxtp);
		if (newoff < 0)
			return off;
		else if (newoff < off)
			return -1;	/* invalid */
		else if (newoff == off)
			return newoff;

		off = newoff;
		proto = *nxtp;
	}
}

/*
 * System control for IP6
 */

u_char	inet6ctlerrmap[PRC_NCMDS] = {
	0,		0,		0,		0,
	0,		EMSGSIZE,	EHOSTDOWN,	EHOSTUNREACH,
	EHOSTUNREACH,	EHOSTUNREACH,	ECONNREFUSED,	ECONNREFUSED,
	EMSGSIZE,	EHOSTUNREACH,	0,		0,
	0,		0,		0,		0,
	ENOPROTOOPT
};

int *ipv6ctl_vars[IPV6CTL_MAXID] = IPV6CTL_VARS;

int
ip6_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
#ifdef MROUTING
	extern int ip6_mrtproto;
	extern struct mrt6stat mrt6stat;
#endif
	int error, s;

	/* Almost all sysctl names at this level are terminal. */
	if (namelen != 1 && name[0] != IPV6CTL_IFQUEUE)
		return (ENOTDIR);

	switch (name[0]) {
	case IPV6CTL_DAD_PENDING:
		return sysctl_rdint(oldp, oldlenp, newp, ip6_dad_pending);
	case IPV6CTL_STATS:
		if (newp != NULL)
			return (EPERM);
		return (sysctl_struct(oldp, oldlenp, newp, newlen,
		    &ip6stat, sizeof(ip6stat)));
#ifdef MROUTING
	case IPV6CTL_MRTSTATS:
		if (newp != NULL)
			return (EPERM);
		return (sysctl_struct(oldp, oldlenp, newp, newlen,
		    &mrt6stat, sizeof(mrt6stat)));
	case IPV6CTL_MRTPROTO:
		return sysctl_rdint(oldp, oldlenp, newp, ip6_mrtproto);
	case IPV6CTL_MRTMIF:
		if (newp)
			return (EPERM);
		return mrt6_sysctl_mif(oldp, oldlenp);
	case IPV6CTL_MRTMFC:
		if (newp)
			return (EPERM);
		return mrt6_sysctl_mfc(oldp, oldlenp);
#else
	case IPV6CTL_MRTSTATS:
	case IPV6CTL_MRTPROTO:
	case IPV6CTL_MRTMIF:
	case IPV6CTL_MRTMFC:
		return (EOPNOTSUPP);
#endif
	case IPV6CTL_MTUDISCTIMEOUT:
		error = sysctl_int(oldp, oldlenp, newp, newlen,
		   &ip6_mtudisc_timeout);
		if (icmp6_mtudisc_timeout_q != NULL) {
			s = splsoftnet();
			rt_timer_queue_change(icmp6_mtudisc_timeout_q,
					      ip6_mtudisc_timeout);
			splx(s);
		}
		return (error);
	case IPV6CTL_IFQUEUE:
		return (sysctl_niq(name + 1, namelen - 1,
		    oldp, oldlenp, newp, newlen, &ip6intrq));
	default:
		if (name[0] < IPV6CTL_MAXID)
			return (sysctl_int_arr(ipv6ctl_vars, name, namelen,
			    oldp, oldlenp, newp, newlen));
		return (EOPNOTSUPP);
	}
	/* NOTREACHED */
}

void
ip6_send_dispatch(void *xmq)
{
	struct mbuf_queue *mq = xmq;
	struct mbuf *m;
	struct mbuf_list ml;
	int s;

	mq_delist(mq, &ml);
	KERNEL_LOCK();
	s = splsoftnet();
	while ((m = ml_dequeue(&ml)) != NULL) {
		ip6_output(m, NULL, NULL, IPV6_MINMTU, NULL, NULL);
	}
	splx(s);
	KERNEL_UNLOCK();
}

void
ip6_send(struct mbuf *m)
{
	mq_enqueue(&ip6send_mq, m);
	task_add(softnettq, &ip6send_task);
}
