/**
 * Copyright (c) 2012
 *
 * Gregory Detal <gregory.detal@uclouvain.be>
 * Christoph Paasch <christoph.paasch@uclouvain.be>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the University nor of the Laboratory may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <tcpdump-stdinc.h>

#include <stdio.h>
#include <string.h>

#include "interface.h"
#include "extract.h"
#include "addrtoname.h"

#include "ipproto.h"
#include "tcp.h"

#define MPTCP_SUB_CAPABLE       0x0
#define MPTCP_SUB_JOIN          0x1
#define MPTCP_SUB_DSS           0x2
#define MPTCP_SUB_ADD_ADDR      0x3
#define MPTCP_SUB_REMOVE_ADDR   0x4
#define MPTCP_SUB_PRIO          0x5
#define MPTCP_SUB_FAIL          0x6
#define MPTCP_SUB_FCLOSE        0x7

struct mptcp_option {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub_etc;        /* subtype upper 4 bits, other stuff lower 4 bits */
};

#define MPTCP_OPT_SUBTYPE(sub_etc)      (((sub_etc) >> 4) & 0xF)

struct mp_capable {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub_ver;
        u_int8_t        flags;
        u_int8_t        sender_key[8];
        u_int8_t        receiver_key[8];
};

#define MP_CAPABLE_OPT_VERSION(sub_ver) (((sub_ver) >> 0) & 0xF)
#define MP_CAPABLE_C                    0x80
#define MP_CAPABLE_S                    0x01

struct mp_join {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub_b;
        u_int8_t        addr_id;
        union {
                struct {
                        u_int8_t         token[4];
                        u_int8_t         nonce[4];
                } syn;
                struct {
                        u_int8_t         mac[8];
                        u_int8_t         nonce[4];
                } synack;
                struct {
                        u_int8_t        mac[20];
                } ack;
        } u;
};

#define MP_JOIN_B                       0x01

struct mp_dss {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub;
        u_int8_t        flags;
};

#define MP_DSS_F                        0x10
#define MP_DSS_m                        0x08
#define MP_DSS_M                        0x04
#define MP_DSS_a                        0x02
#define MP_DSS_A                        0x01

struct mp_add_addr {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub_ipver;
        u_int8_t        addr_id;
        union {
                struct {
                        u_int8_t         addr[4];
                        u_int8_t         port[2];
                } v4;
                struct {
                        u_int8_t         addr[16];
                        u_int8_t         port[2];
                } v6;
        } u;
};

#define MP_ADD_ADDR_IPVER(sub_ipver)    (((sub_ipver) >> 0) & 0xF)

struct mp_remove_addr {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub;
        /* list of addr_id */
        u_int8_t        addrs_id;
};

struct mp_fail {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub;
        u_int8_t        resv;
        u_int8_t        data_seq[8];
};

struct mp_close {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub;
        u_int8_t        rsv;
        u_int8_t        key[8];
};

struct mp_prio {
        u_int8_t        kind;
        u_int8_t        len;
        u_int8_t        sub_b;
        u_int8_t        addr_id;
};

#define MP_PRIO_B                       0x01

static int dummy_print(const u_char *opt _U_, u_int opt_len _U_, u_char flags _U_)
{
        return 1;
}

static int mp_capable_print(const u_char *opt, u_int opt_len, u_char flags)
{
        struct mp_capable *mpc = (struct mp_capable *) opt;

        if (!(opt_len == 12 && flags & TH_SYN) &&
            !(opt_len == 20 && (flags & (TH_SYN | TH_ACK)) == TH_ACK))
                return 0;

        if (MP_CAPABLE_OPT_VERSION(mpc->sub_ver) != 0) {
                printf(" Unknown Version (%d)", MP_CAPABLE_OPT_VERSION(mpc->sub_ver));
                return 1;
        }

        if (mpc->flags & MP_CAPABLE_C)
                printf(" csum");
        printf(" {0x%" PRIx64, EXTRACT_64BITS(mpc->sender_key));
        if (opt_len == 20) /* ACK */
                printf(",0x%" PRIx64, EXTRACT_64BITS(mpc->receiver_key));
        printf("}");
        return 1;
}

static int mp_join_print(const u_char *opt, u_int opt_len, u_char flags)
{
        struct mp_join *mpj = (struct mp_join *) opt;

        if (!(opt_len == 12 && flags & TH_SYN) &&
            !(opt_len == 16 && (flags & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK)) &&
            !(opt_len == 24 && flags & TH_ACK))
                return 0;

        if (opt_len != 24) {
                if (mpj->sub_b & MP_JOIN_B)
                        printf(" backup");
                printf(" id %u", mpj->addr_id);
        }

        switch (opt_len) {
        case 12: /* SYN */
                printf(" token 0x%x" " nonce 0x%x",
                        EXTRACT_32BITS(mpj->u.syn.token),
                        EXTRACT_32BITS(mpj->u.syn.nonce));
                break;
        case 16: /* SYN/ACK */
                printf(" hmac 0x%" PRIx64 " nonce 0x%x",
                        EXTRACT_64BITS(mpj->u.synack.mac),
                        EXTRACT_32BITS(mpj->u.synack.nonce));
                break;
        case 24: {/* ACK */
                size_t i;
                printf(" hmac 0x");
                for (i = 0; i < sizeof(mpj->u.ack.mac); ++i)
                        printf("%02x", mpj->u.ack.mac[i]);
        }
        default:
                break;
        }
        return 1;
}

static u_int mp_dss_len(struct mp_dss *m, int csum)
{
        u_int len;

        len = 4;
        if (m->flags & MP_DSS_A) {
                /* Ack present - 4 or 8 octets */
                len += (m->flags & MP_DSS_a) ? 8 : 4;
        }
        if (m->flags & MP_DSS_M) {
                /*
                 * Data Sequence Number (DSN), Subflow Sequence Number (SSN),
                 * Data-Level Length present, and Checksum possibly present.
                 * All but the Checksum are 10 bytes if the m flag is
                 * clear (4-byte DSN) and 14 bytes if the m flag is set
                 * (8-byte DSN).
                 */
                len += (m->flags & MP_DSS_m) ? 14 : 10;

                /*
                 * The Checksum is present only if negotiated.
                 */
                if (csum)
                        len += 2;
	}
	return len;
}

static int mp_dss_print(const u_char *opt, u_int opt_len, u_char flags)
{
        struct mp_dss *mdss = (struct mp_dss *) opt;

        if ((opt_len != mp_dss_len(mdss, 1) &&
             opt_len != mp_dss_len(mdss, 0)) || flags & TH_SYN)
                return 0;

        if (mdss->flags & MP_DSS_F)
                printf(" fin");

        opt += 4;
        if (mdss->flags & MP_DSS_A) {
                printf(" ack ");
                if (mdss->flags & MP_DSS_a) {
                        printf("%" PRIu64, EXTRACT_64BITS(opt));
                        opt += 8;
                } else {
                        printf("%u", EXTRACT_32BITS(opt));
                        opt += 4;
                }
        }

        if (mdss->flags & MP_DSS_M) {
                printf(" seq ");
                if (mdss->flags & MP_DSS_m) {
                        printf("%" PRIu64, EXTRACT_64BITS(opt));
                        opt += 8;
                } else {
                        printf("%u", EXTRACT_32BITS(opt));
                        opt += 4;
                }
                printf(" subseq %u", EXTRACT_32BITS(opt));
                opt += 4;
                printf(" len %u", EXTRACT_16BITS(opt));
                opt += 2;

                if (opt_len == mp_dss_len(mdss, 1))
                        printf(" csum 0x%x", EXTRACT_16BITS(opt));
        }
        return 1;
}

static int add_addr_print(const u_char *opt, u_int opt_len, u_char flags _U_)
{
        struct mp_add_addr *add_addr = (struct mp_add_addr *) opt;
        u_int ipver = MP_ADD_ADDR_IPVER(add_addr->sub_ipver);

        if (!((opt_len == 8 || opt_len == 10) && ipver == 4) &&
            !((opt_len == 20 || opt_len == 22) && ipver == 6))
                return 0;

        printf(" id %u", add_addr->addr_id);
        switch (ipver) {
        case 4:
                printf(" %s", ipaddr_string(add_addr->u.v4.addr));
                if (opt_len == 10)
                        printf(":%u", EXTRACT_16BITS(add_addr->u.v4.port));
                break;
        case 6:
#ifdef INET6
                printf(" %s", ip6addr_string(add_addr->u.v6.addr));
#endif
                if (opt_len == 22)
                        printf(":%u", EXTRACT_16BITS(add_addr->u.v6.port));
                break;
        default:
                return 0;
        }

        return 1;
}

static int remove_addr_print(const u_char *opt, u_int opt_len, u_char flags _U_)
{
        struct mp_remove_addr *remove_addr = (struct mp_remove_addr *) opt;
        u_int8_t *addr_id = &remove_addr->addrs_id;

        if (opt_len < 4)
                return 0;

        opt_len -= 3;
        printf(" id");
        while (opt_len--)
                printf(" %u", *addr_id++);
        return 1;
}

static int mp_prio_print(const u_char *opt, u_int opt_len, u_char flags _U_)
{
        struct mp_prio *mpp = (struct mp_prio *) opt;

        if (opt_len != 3 && opt_len != 4)
                return 0;

        if (mpp->sub_b & MP_PRIO_B)
                printf(" backup");
        else
                printf(" non-backup");
        if (opt_len == 4)
                printf(" id %u", mpp->addr_id);

        return 1;
}

static int mp_fail_print(const u_char *opt, u_int opt_len, u_char flags _U_)
{
        if (opt_len != 12)
                return 0;

        printf(" seq %" PRIu64, EXTRACT_64BITS(opt + 4));
        return 1;
}

static int mp_fast_close_print(const u_char *opt, u_int opt_len, u_char flags _U_)
{
        if (opt_len != 12)
                return 0;

        printf(" key 0x%" PRIx64, EXTRACT_64BITS(opt + 4));
        return 1;
}

static const struct {
        const char *name;
        int (*print)(const u_char *, u_int, u_char);
} mptcp_options[] = {
        { "capable", mp_capable_print},
        { "join",       mp_join_print },
        { "dss",        mp_dss_print },
        { "add-addr",   add_addr_print },
        { "rem-addr",   remove_addr_print },
        { "prio",       mp_prio_print },
        { "fail",       mp_fail_print },
        { "fast-close", mp_fast_close_print },
        { "unknown",    dummy_print },
};

int mptcp_print(const u_char *cp, u_int len, u_char flags)
{
        struct mptcp_option *opt;
        u_int subtype;

        if (len < 3)
                return 0;

        opt = (struct mptcp_option *) cp;
        subtype = min(MPTCP_OPT_SUBTYPE(opt->sub_etc), MPTCP_SUB_FCLOSE + 1);

        printf(" %s", mptcp_options[subtype].name);
        return mptcp_options[subtype].print(cp, len, flags);
}