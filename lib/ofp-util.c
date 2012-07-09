/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "ofp-print.h"
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <stdlib.h>
#include "autopath.h"
#include "bundle.h"
#include "byte-order.h"
#include "classifier.h"
#include "dynamic-string.h"
#include "learn.h"
#include "meta-flow.h"
#include "multipath.h"
#include "netdev.h"
#include "nx-match.h"
#include "ofp-actions.h"
#include "ofp-errors.h"
#include "ofp-util.h"
#include "ofpbuf.h"
#include "packets.h"
#include "random.h"
#include "unaligned.h"
#include "type-props.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(ofp_util);

/* Rate limit for OpenFlow message parse errors.  These always indicate a bug
 * in the peer and so there's not much point in showing a lot of them. */
static struct vlog_rate_limit bad_ofmsg_rl = VLOG_RATE_LIMIT_INIT(1, 5);

/* Given the wildcard bit count in the least-significant 6 of 'wcbits', returns
 * an IP netmask with a 1 in each bit that must match and a 0 in each bit that
 * is wildcarded.
 *
 * The bits in 'wcbits' are in the format used in enum ofp_flow_wildcards: 0
 * is exact match, 1 ignores the LSB, 2 ignores the 2 least-significant bits,
 * ..., 32 and higher wildcard the entire field.  This is the *opposite* of the
 * usual convention where e.g. /24 indicates that 8 bits (not 24 bits) are
 * wildcarded. */
ovs_be32
ofputil_wcbits_to_netmask(int wcbits)
{
    wcbits &= 0x3f;
    return wcbits < 32 ? htonl(~((1u << wcbits) - 1)) : 0;
}

/* Given the IP netmask 'netmask', returns the number of bits of the IP address
 * that it wildcards, that is, the number of 0-bits in 'netmask', a number
 * between 0 and 32 inclusive.
 *
 * If 'netmask' is not a CIDR netmask (see ip_is_cidr()), the return value will
 * still be in the valid range but isn't otherwise meaningful. */
int
ofputil_netmask_to_wcbits(ovs_be32 netmask)
{
    return 32 - ip_count_cidr_bits(netmask);
}

/* A list of the FWW_* and OFPFW10_ bits that have the same value, meaning, and
 * name. */
#define WC_INVARIANT_LIST \
    WC_INVARIANT_BIT(IN_PORT) \
    WC_INVARIANT_BIT(DL_TYPE) \
    WC_INVARIANT_BIT(NW_PROTO)

/* Verify that all of the invariant bits (as defined on WC_INVARIANT_LIST)
 * actually have the same names and values. */
#define WC_INVARIANT_BIT(NAME) BUILD_ASSERT_DECL(FWW_##NAME == OFPFW10_##NAME);
    WC_INVARIANT_LIST
#undef WC_INVARIANT_BIT

/* WC_INVARIANTS is the invariant bits (as defined on WC_INVARIANT_LIST) all
 * OR'd together. */
static const flow_wildcards_t WC_INVARIANTS = 0
#define WC_INVARIANT_BIT(NAME) | FWW_##NAME
    WC_INVARIANT_LIST
#undef WC_INVARIANT_BIT
;

/* Converts the OpenFlow 1.0 wildcards in 'ofpfw' (OFPFW10_*) into a
 * flow_wildcards in 'wc' for use in struct cls_rule.  It is the caller's
 * responsibility to handle the special case where the flow match's dl_vlan is
 * set to OFP_VLAN_NONE. */
void
ofputil_wildcard_from_ofpfw10(uint32_t ofpfw, struct flow_wildcards *wc)
{
    BUILD_ASSERT_DECL(FLOW_WC_SEQ == 13);

    /* Initialize most of rule->wc. */
    flow_wildcards_init_catchall(wc);
    wc->wildcards = (OVS_FORCE flow_wildcards_t) ofpfw & WC_INVARIANTS;

    /* Wildcard fields that aren't defined by ofp10_match or tun_id. */
    wc->wildcards |= (FWW_ARP_SHA | FWW_ARP_THA | FWW_NW_ECN | FWW_NW_TTL
                      | FWW_IPV6_LABEL | FWW_MPLS_LABEL | FWW_MPLS_TC
                      | FWW_MPLS_STACK | FWW_VLAN_TPID | FWW_VLAN_QINQ_VID
                      | FWW_VLAN_QINQ_PCP);

    if (ofpfw & OFPFW10_NW_TOS) {
        /* OpenFlow 1.0 defines a TOS wildcard, but it's much later in
         * the enum than we can use. */
        wc->wildcards |= FWW_NW_DSCP;
    }

    wc->nw_src_mask = ofputil_wcbits_to_netmask(ofpfw >> OFPFW10_NW_SRC_SHIFT);
    wc->nw_dst_mask = ofputil_wcbits_to_netmask(ofpfw >> OFPFW10_NW_DST_SHIFT);

    if (!(ofpfw & OFPFW10_TP_SRC)) {
        wc->tp_src_mask = htons(UINT16_MAX);
    }
    if (!(ofpfw & OFPFW10_TP_DST)) {
        wc->tp_dst_mask = htons(UINT16_MAX);
    }

    if (!(ofpfw & OFPFW10_DL_SRC)) {
        memset(wc->dl_src_mask, 0xff, ETH_ADDR_LEN);
    }
    if (!(ofpfw & OFPFW10_DL_DST)) {
        memset(wc->dl_dst_mask, 0xff, ETH_ADDR_LEN);
    }

    /* VLAN TCI mask. */
    if (!(ofpfw & OFPFW10_DL_VLAN_PCP)) {
        wc->vlan_tci_mask |= htons(VLAN_PCP_MASK | VLAN_CFI);
    }
    if (!(ofpfw & OFPFW10_DL_VLAN)) {
        wc->vlan_tci_mask |= htons(VLAN_VID_MASK | VLAN_CFI);
    }
}

/* Converts the ofp10_match in 'match' into a cls_rule in 'rule', with the
 * given 'priority'. */
void
ofputil_cls_rule_from_ofp10_match(const struct ofp10_match *match,
                                  unsigned int priority, struct cls_rule *rule)
{
    uint32_t ofpfw = ntohl(match->wildcards) & OFPFW10_ALL;

    /* Initialize rule->priority, rule->wc. */
    rule->priority = !ofpfw ? UINT16_MAX : priority;
    ofputil_wildcard_from_ofpfw10(ofpfw, &rule->wc);

    /* Initialize most of rule->flow. */
    rule->flow.nw_src = match->nw_src;
    rule->flow.nw_dst = match->nw_dst;
    rule->flow.in_port = ntohs(match->in_port);
    rule->flow.dl_type = ofputil_dl_type_from_openflow(match->dl_type);
    rule->flow.tp_src = match->tp_src;
    rule->flow.tp_dst = match->tp_dst;
    memcpy(rule->flow.dl_src, match->dl_src, ETH_ADDR_LEN);
    memcpy(rule->flow.dl_dst, match->dl_dst, ETH_ADDR_LEN);
    rule->flow.nw_tos = match->nw_tos & IP_DSCP_MASK;
    rule->flow.nw_proto = match->nw_proto;

    /* Translate VLANs. */
    if (!(ofpfw & OFPFW10_DL_VLAN) &&
        match->dl_vlan == htons(OFP10_VLAN_NONE)) {
        /* Match only packets without 802.1Q header.
         *
         * When OFPFW10_DL_VLAN_PCP is wildcarded, this is obviously correct.
         *
         * If OFPFW10_DL_VLAN_PCP is matched, the flow match is contradictory,
         * because we can't have a specific PCP without an 802.1Q header.
         * However, older versions of OVS treated this as matching packets
         * withut an 802.1Q header, so we do here too. */
        rule->flow.vlan_tci = htons(0);
        rule->wc.vlan_tci_mask = htons(0xffff);
    } else {
        ovs_be16 vid, pcp, tci;

        vid = match->dl_vlan & htons(VLAN_VID_MASK);
        pcp = htons((match->dl_vlan_pcp << VLAN_PCP_SHIFT) & VLAN_PCP_MASK);
        tci = vid | pcp | htons(VLAN_CFI);
        rule->flow.vlan_tci = tci & rule->wc.vlan_tci_mask;
    }

    /* Clean up. */
    cls_rule_zero_wildcarded_fields(rule);
}

/* Convert 'rule' into the OpenFlow 1.0 match structure 'match'. */
void
ofputil_cls_rule_to_ofp10_match(const struct cls_rule *rule,
                                struct ofp10_match *match)
{
    const struct flow_wildcards *wc = &rule->wc;
    uint32_t ofpfw;

    /* Figure out most OpenFlow wildcards. */
    ofpfw = (OVS_FORCE uint32_t) (wc->wildcards & WC_INVARIANTS);
    ofpfw |= (ofputil_netmask_to_wcbits(wc->nw_src_mask)
              << OFPFW10_NW_SRC_SHIFT);
    ofpfw |= (ofputil_netmask_to_wcbits(wc->nw_dst_mask)
              << OFPFW10_NW_DST_SHIFT);
    if (wc->wildcards & FWW_NW_DSCP) {
        ofpfw |= OFPFW10_NW_TOS;
    }
    if (!wc->tp_src_mask) {
        ofpfw |= OFPFW10_TP_SRC;
    }
    if (!wc->tp_dst_mask) {
        ofpfw |= OFPFW10_TP_DST;
    }
    if (eth_addr_is_zero(wc->dl_src_mask)) {
        ofpfw |= OFPFW10_DL_SRC;
    }
    if (eth_addr_is_zero(wc->dl_dst_mask)) {
        ofpfw |= OFPFW10_DL_DST;
    }

    /* Translate VLANs. */
    match->dl_vlan = htons(0);
    match->dl_vlan_pcp = 0;
    if (rule->wc.vlan_tci_mask == htons(0)) {
        ofpfw |= OFPFW10_DL_VLAN | OFPFW10_DL_VLAN_PCP;
    } else if (rule->wc.vlan_tci_mask & htons(VLAN_CFI)
               && !(rule->flow.vlan_tci & htons(VLAN_CFI))) {
        match->dl_vlan = htons(OFP10_VLAN_NONE);
    } else {
        if (!(rule->wc.vlan_tci_mask & htons(VLAN_VID_MASK))) {
            ofpfw |= OFPFW10_DL_VLAN;
        } else {
            match->dl_vlan = htons(vlan_tci_to_vid(rule->flow.vlan_tci));
        }

        if (!(rule->wc.vlan_tci_mask & htons(VLAN_PCP_MASK))) {
            ofpfw |= OFPFW10_DL_VLAN_PCP;
        } else {
            match->dl_vlan_pcp = vlan_tci_to_pcp(rule->flow.vlan_tci);
        }
    }

    /* Compose most of the match structure. */
    match->wildcards = htonl(ofpfw);
    match->in_port = htons(rule->flow.in_port);
    memcpy(match->dl_src, rule->flow.dl_src, ETH_ADDR_LEN);
    memcpy(match->dl_dst, rule->flow.dl_dst, ETH_ADDR_LEN);
    match->dl_type = ofputil_dl_type_to_openflow(rule->flow.dl_type);
    match->nw_src = rule->flow.nw_src;
    match->nw_dst = rule->flow.nw_dst;
    match->nw_tos = rule->flow.nw_tos & IP_DSCP_MASK;
    match->nw_proto = rule->flow.nw_proto;
    match->tp_src = rule->flow.tp_src;
    match->tp_dst = rule->flow.tp_dst;
    memset(match->pad1, '\0', sizeof match->pad1);
    memset(match->pad2, '\0', sizeof match->pad2);
}

static enum ofperr
__ofputil_pull_ofp11_match(struct ofpbuf *buf, unsigned int priority,
                           struct cls_rule *rule, ovs_be64 *cookie,
                           ovs_be64 *cookie_mask, uint16_t *padded_match_len,
                           uint8_t max_version)
{
    struct ofp11_match_header *omh = buf->data;
    uint16_t match_len;

    if (buf->size < sizeof(struct ofp11_match_header)) {
        return OFPERR_OFPBMC_BAD_LEN;
    }

    match_len = ntohs(omh->length);

    switch (ntohs(omh->type)) {
    case OFPMT_STANDARD: {
        struct ofp11_match *om;

        if (match_len != sizeof *om || buf->size < sizeof *om) {
            return OFPERR_OFPBMC_BAD_LEN;
        }
        om = ofpbuf_pull(buf, sizeof *om);
        if (padded_match_len) {
            *padded_match_len = match_len;
        }
        return ofputil_cls_rule_from_ofp11_match(om, priority, rule);
    }

    case OFPMT_OXM: {
        enum ofperr error;

        if (max_version < OFP12_VERSION) {
            error = OFPERR_OFPBMC_BAD_TYPE;
        } else {
            if (padded_match_len) {
                *padded_match_len =
                    nx_padded_match_len(match_len - sizeof *omh, sizeof *omh) +
                    sizeof *omh;
            }
            ofpbuf_pull(buf, sizeof *omh);
            error = nx_pull_match(buf, match_len - sizeof *omh, sizeof *omh,
                                  priority, rule, cookie, cookie_mask);
        }
        return error;
    }

    default:
        return OFPERR_OFPBMC_BAD_TYPE;
    }
}

enum ofperr
ofputil_pull_ofp11_match(struct ofpbuf *buf, unsigned int priority,
                         struct cls_rule *rule)
{
    return __ofputil_pull_ofp11_match(buf, priority, rule, NULL, NULL,
                                      NULL, OFP11_VERSION);
}

enum ofperr
ofputil_pull_ofp12_match(struct ofpbuf *buf, unsigned int priority,
                         struct cls_rule *rule, ovs_be64 *cookie,
                         ovs_be64 *cookie_mask, uint16_t *padded_match_len)
{
    return __ofputil_pull_ofp11_match(buf, priority, rule, cookie,
                                      cookie_mask, padded_match_len,
                                      OFP12_VERSION);
}

/* Converts the ofp11_match in 'match' into a cls_rule in 'rule', with the
 * given 'priority'.  Returns 0 if successful, otherwise an OFPERR_* value. */
enum ofperr
ofputil_cls_rule_from_ofp11_match(const struct ofp11_match *match,
                                  unsigned int priority,
                                  struct cls_rule *rule)
{
    uint16_t wc = ntohl(match->wildcards);
    uint8_t dl_src_mask[ETH_ADDR_LEN];
    uint8_t dl_dst_mask[ETH_ADDR_LEN];
    bool ipv4, arp;
    int i;

    cls_rule_init_catchall(rule, priority);

    if (!(wc & OFPFW11_IN_PORT)) {
        uint16_t ofp_port;
        enum ofperr error;

        error = ofputil_port_from_ofp11(match->in_port, &ofp_port);
        if (error) {
            return OFPERR_OFPBMC_BAD_VALUE;
        }
        cls_rule_set_in_port(rule, ofp_port);
    }

    for (i = 0; i < ETH_ADDR_LEN; i++) {
        dl_src_mask[i] = ~match->dl_src_mask[i];
    }
    cls_rule_set_dl_src_masked(rule, match->dl_src, dl_src_mask);

    for (i = 0; i < ETH_ADDR_LEN; i++) {
        dl_dst_mask[i] = ~match->dl_dst_mask[i];
    }
    cls_rule_set_dl_dst_masked(rule, match->dl_dst, dl_dst_mask);

    if (!(wc & OFPFW11_DL_VLAN)) {
        if (match->dl_vlan == htons(OFPVID11_NONE)) {
            /* Match only packets without a VLAN tag. */
            rule->flow.vlan_tci = htons(0);
            rule->wc.vlan_tci_mask = htons(UINT16_MAX);
        } else {
            if (match->dl_vlan == htons(OFPVID11_ANY)) {
                /* Match any packet with a VLAN tag regardless of VID. */
                rule->flow.vlan_tci = htons(VLAN_CFI);
                rule->wc.vlan_tci_mask = htons(VLAN_CFI);
            } else if (ntohs(match->dl_vlan) < 4096) {
                /* Match only packets with the specified VLAN VID. */
                rule->flow.vlan_tci = htons(VLAN_CFI) | match->dl_vlan;
                rule->wc.vlan_tci_mask = htons(VLAN_CFI | VLAN_VID_MASK);
            } else {
                /* Invalid VID. */
                return OFPERR_OFPBMC_BAD_VALUE;
            }

            if (!(wc & OFPFW11_DL_VLAN_PCP)) {
                if (match->dl_vlan_pcp <= 7) {
                    rule->flow.vlan_tci |= htons(match->dl_vlan_pcp
                                                 << VLAN_PCP_SHIFT);
                    rule->wc.vlan_tci_mask |= htons(VLAN_PCP_MASK);
                } else {
                    /* Invalid PCP. */
                    return OFPERR_OFPBMC_BAD_VALUE;
                }
            }
        }
    }

    if (!(wc & OFPFW11_DL_TYPE)) {
        cls_rule_set_dl_type(rule,
                             ofputil_dl_type_from_openflow(match->dl_type));
    }

    ipv4 = rule->flow.dl_type == htons(ETH_TYPE_IP);
    arp = rule->flow.dl_type == htons(ETH_TYPE_ARP);

    if (ipv4 && !(wc & OFPFW11_NW_TOS)) {
        if (match->nw_tos & ~IP_DSCP_MASK) {
            /* Invalid TOS. */
            return OFPERR_OFPBMC_BAD_VALUE;
        }

        cls_rule_set_nw_dscp(rule, match->nw_tos);
    }

    if (ipv4 || arp) {
        if (!(wc & OFPFW11_NW_PROTO)) {
            cls_rule_set_nw_proto(rule, match->nw_proto);
        }
        cls_rule_set_nw_src_masked(rule, match->nw_src, ~match->nw_src_mask);
        cls_rule_set_nw_dst_masked(rule, match->nw_dst, ~match->nw_dst_mask);
    }

#define OFPFW11_TP_ALL (OFPFW11_TP_SRC | OFPFW11_TP_DST)
    if (ipv4 && (wc & OFPFW11_TP_ALL) != OFPFW11_TP_ALL) {
        switch (rule->flow.nw_proto) {
        case IPPROTO_ICMP:
            /* "A.2.3 Flow Match Structures" in OF1.1 says:
             *
             *    The tp_src and tp_dst fields will be ignored unless the
             *    network protocol specified is as TCP, UDP or SCTP.
             *
             * but I'm pretty sure we should support ICMP too, otherwise
             * that's a regression from OF1.0. */
            if (!(wc & OFPFW11_TP_SRC)) {
                uint16_t icmp_type = ntohs(match->tp_src);
                if (icmp_type < 0x100) {
                    cls_rule_set_icmp_type(rule, icmp_type);
                } else {
                    return OFPERR_OFPBMC_BAD_FIELD;
                }
            }
            if (!(wc & OFPFW11_TP_DST)) {
                uint16_t icmp_code = ntohs(match->tp_dst);
                if (icmp_code < 0x100) {
                    cls_rule_set_icmp_code(rule, icmp_code);
                } else {
                    return OFPERR_OFPBMC_BAD_FIELD;
                }
            }
            break;

        case IPPROTO_TCP:
        case IPPROTO_UDP:
            if (!(wc & (OFPFW11_TP_SRC))) {
                cls_rule_set_tp_src(rule, match->tp_src);
            }
            if (!(wc & (OFPFW11_TP_DST))) {
                cls_rule_set_tp_dst(rule, match->tp_dst);
            }
            break;

        case IPPROTO_SCTP:
            /* We don't support SCTP and it seems that we should tell the
             * controller, since OF1.1 implementations are supposed to. */
            return OFPERR_OFPBMC_BAD_FIELD;

        default:
            /* OF1.1 says explicitly to ignore this. */
            break;
        }
    }

    if (rule->flow.dl_type == htons(ETH_TYPE_MPLS) ||
        rule->flow.dl_type == htons(ETH_TYPE_MPLS_MCAST)) {
        enum { OFPFW11_MPLS_ALL = OFPFW11_MPLS_LABEL | OFPFW11_MPLS_TC };

        if ((wc & OFPFW11_MPLS_ALL) != OFPFW11_MPLS_ALL) {
            /* MPLS not supported. */
            return OFPERR_OFPBMC_BAD_TAG;
        }
    }

    if (match->metadata_mask != htonll(UINT64_MAX)) {
        /* Metadata field not yet supported because we haven't decided how to
         * map it onto our existing fields (or whether to add a new field). */
        return OFPERR_OFPBMC_BAD_FIELD;
    }

    return 0;
}

/* Convert 'rule' into the OpenFlow 1.1 match structure 'match'. */
void
ofputil_cls_rule_to_ofp11_match(const struct cls_rule *rule,
                                struct ofp11_match *match)
{
    uint32_t wc = 0;
    int i;

    memset(match, 0, sizeof *match);
    match->omh.type = htons(OFPMT_STANDARD);
    match->omh.length = htons(OFPMT11_STANDARD_LENGTH);

    if (rule->wc.wildcards & FWW_IN_PORT) {
        wc |= OFPFW11_IN_PORT;
    } else {
        match->in_port = ofputil_port_to_ofp11(rule->flow.in_port);
    }


    memcpy(match->dl_src, rule->flow.dl_src, ETH_ADDR_LEN);
    for (i = 0; i < ETH_ADDR_LEN; i++) {
        match->dl_src_mask[i] = ~rule->wc.dl_src_mask[i];
    }

    memcpy(match->dl_dst, rule->flow.dl_dst, ETH_ADDR_LEN);
    for (i = 0; i < ETH_ADDR_LEN; i++) {
        match->dl_dst_mask[i] = ~rule->wc.dl_dst_mask[i];
    }

    if (rule->wc.vlan_tci_mask == htons(0)) {
        wc |= OFPFW11_DL_VLAN | OFPFW11_DL_VLAN_PCP;
    } else if (rule->wc.vlan_tci_mask & htons(VLAN_CFI)
               && !(rule->flow.vlan_tci & htons(VLAN_CFI))) {
        match->dl_vlan = htons(OFPVID11_NONE);
        wc |= OFPFW11_DL_VLAN_PCP;
    } else {
        if (!(rule->wc.vlan_tci_mask & htons(VLAN_VID_MASK))) {
            match->dl_vlan = htons(OFPVID11_ANY);
        } else {
            match->dl_vlan = htons(vlan_tci_to_vid(rule->flow.vlan_tci));
        }

        if (!(rule->wc.vlan_tci_mask & htons(VLAN_PCP_MASK))) {
            wc |= OFPFW11_DL_VLAN_PCP;
        } else {
            match->dl_vlan_pcp = vlan_tci_to_pcp(rule->flow.vlan_tci);
        }
    }

    if (rule->wc.wildcards & FWW_DL_TYPE) {
        wc |= OFPFW11_DL_TYPE;
    } else {
        match->dl_type = ofputil_dl_type_to_openflow(rule->flow.dl_type);
    }

    if (rule->wc.wildcards & FWW_NW_DSCP) {
        wc |= OFPFW11_NW_TOS;
    } else {
        match->nw_tos = rule->flow.nw_tos & IP_DSCP_MASK;
    }

    if (rule->wc.wildcards & FWW_NW_PROTO) {
        wc |= OFPFW11_NW_PROTO;
    } else {
        match->nw_proto = rule->flow.nw_proto;
    }

    match->nw_src = rule->flow.nw_src;
    match->nw_src_mask = ~rule->wc.nw_src_mask;
    match->nw_dst = rule->flow.nw_dst;
    match->nw_dst_mask = ~rule->wc.nw_dst_mask;

    if (!rule->wc.tp_src_mask) {
        wc |= OFPFW11_TP_SRC;
    } else {
        match->tp_src = rule->flow.tp_src;
    }

    if (!rule->wc.tp_dst_mask) {
        wc |= OFPFW11_TP_DST;
    } else {
        match->tp_dst = rule->flow.tp_dst;
    }

    /* MPLS not supported. */
    wc |= OFPFW11_MPLS_LABEL;
    wc |= OFPFW11_MPLS_TC;

    /* Metadata field not yet supported */
    match->metadata_mask = htonll(UINT64_MAX);

    match->wildcards = htonl(wc);
}

/* Given a 'dl_type' value in the format used in struct flow, returns the
 * corresponding 'dl_type' value for use in an ofp10_match or ofp11_match
 * structure. */
ovs_be16
ofputil_dl_type_to_openflow(ovs_be16 flow_dl_type)
{
    return (flow_dl_type == htons(FLOW_DL_TYPE_NONE)
            ? htons(OFP_DL_TYPE_NOT_ETH_TYPE)
            : flow_dl_type);
}

/* Given a 'dl_type' value in the format used in an ofp10_match or ofp11_match
 * structure, returns the corresponding 'dl_type' value for use in struct
 * flow. */
ovs_be16
ofputil_dl_type_from_openflow(ovs_be16 ofp_dl_type)
{
    return (ofp_dl_type == htons(OFP_DL_TYPE_NOT_ETH_TYPE)
            ? htons(FLOW_DL_TYPE_NONE)
            : ofp_dl_type);
}

/* Returns a transaction ID to use for an outgoing OpenFlow message. */
static ovs_be32
alloc_xid(void)
{
    static uint32_t next_xid = 1;
    return htonl(next_xid++);
}

struct ofputil_raw_msg_type {
    uint8_t version;            /* From ofp_header. */
    uint8_t type;               /* From ofp_header. */
    uint16_t stat;              /* From ofp10_stats_msg or ofp11_stats_msg. */
    uint32_t vendor;            /* From ofp_vendor_header,
                                 * ofp10_vendor_stats_msg, or
                                 * ofp11_vendor_stats_msg. */
    uint32_t subtype;           /* From nicira_header, nicira10_stats_msg, or
                                 * nicira11_stats_msg. */
};

static enum ofperr
ofputil_decode_raw_msg_type(const struct ofp_header *oh, size_t length,
                            struct ofputil_raw_msg_type *raw)
{
    memset(raw, 0, sizeof *raw);
    if (length < sizeof *oh) {
        return OFPERR_OFPBRC_BAD_LEN;
    }

    /* Get base message version and type (OFPT_*). */
    raw->version = oh->version;
    raw->type = oh->type;

    if (raw->type == OFPT_VENDOR) {
        /* Get vendor. */
        const struct ofp_vendor_header *ovh;

        if (length < sizeof *ovh) {
            return OFPERR_OFPBRC_BAD_LEN;
        }

        ovh = (const struct ofp_vendor_header *) oh;
        raw->vendor = ntohl(ovh->vendor);
        if (raw->vendor == NX_VENDOR_ID) {
            /* Get Nicira message subtype (NXT_*). */
            const struct nicira_header *nh;

            if (length < sizeof *nh) {
                return OFPERR_OFPBRC_BAD_LEN;
            }
            nh = (const struct nicira_header *) oh;
            raw->subtype = ntohl(nh->subtype);
        } else {
            return OFPERR_OFPBRC_BAD_VENDOR;
        }
    } else if (raw->version == OFP10_VERSION
               && (raw->type == OFPT10_STATS_REQUEST ||
                   raw->type == OFPT10_STATS_REPLY)) {
        const struct ofp10_stats_msg *osm;

        /* Get statistic type (OFPST_*). */
        if (length < sizeof *osm) {
            return OFPERR_OFPBRC_BAD_LEN;
        }
        osm = (const struct ofp10_stats_msg *) oh;
        raw->stat = ntohs(osm->type);

        if (raw->stat == OFPST_VENDOR) {
            /* Get vendor. */
            const struct ofp10_vendor_stats_msg *ovsm;

            if (length < sizeof *ovsm) {
                return OFPERR_OFPBRC_BAD_LEN;
            }

            ovsm = (const struct ofp10_vendor_stats_msg *) oh;
            raw->vendor = ntohl(ovsm->vendor);
            if (raw->vendor == NX_VENDOR_ID) {
                /* Get Nicira statistic type (NXST_*). */
                const struct nicira10_stats_msg *nsm;

                if (length < sizeof *nsm) {
                    return OFPERR_OFPBRC_BAD_LEN;
                }
                nsm = (const struct nicira10_stats_msg *) oh;
                raw->subtype = ntohl(nsm->subtype);
            } else {
                return OFPERR_OFPBRC_BAD_VENDOR;
            }
        }
    } else if ((raw->version == OFP11_VERSION ||
                raw->version == OFP12_VERSION)
               && (raw->type == OFPT11_STATS_REQUEST ||
                   raw->type == OFPT11_STATS_REPLY)) {
        const struct ofp11_stats_msg *osm;

        /* Get statistic type (OFPST_*). */
        if (length < sizeof *osm) {
            return OFPERR_OFPBRC_BAD_LEN;
        }
        osm = (const struct ofp11_stats_msg *) oh;
        raw->stat = ntohs(osm->type);

        if (raw->stat == OFPST_VENDOR) {
            /* Get vendor. */
            const struct ofp11_vendor_stats_msg *ovsm;

            if (length < sizeof *ovsm) {
                return OFPERR_OFPBRC_BAD_LEN;
            }

            ovsm = (const struct ofp11_vendor_stats_msg *) oh;
            raw->vendor = ntohl(ovsm->vendor);
            if (raw->vendor == NX_VENDOR_ID) {
                /* Get Nicira statistic type (NXST_*). */
                const struct nicira11_stats_msg *nsm;

                if (length < sizeof *nsm) {
                    return OFPERR_OFPBRC_BAD_LEN;
                }
                nsm = (const struct nicira11_stats_msg *) oh;
                raw->subtype = ntohl(nsm->subtype);
            } else {
                return OFPERR_OFPBRC_BAD_VENDOR;
            }
        }
    }

    return 0;
}

/* Basic parsing of OpenFlow messages. */

struct ofputil_msg_type {
    enum ofputil_msg_code code; /* OFPUTIL_*. */
    struct ofputil_raw_msg_type raw;
    const char *name;           /* e.g. "OFPT_FLOW_REMOVED". */
    unsigned int min_size;      /* Minimum total message size in bytes. */
    /* 0 if 'min_size' is the exact size that the message must be.  Otherwise,
     * the message may exceed 'min_size' by an even multiple of this value. */
    unsigned int extra_multiple;
};

static const struct ofputil_msg_type ofputil_msg_types[] = {
#define OFPT(TYPE, VERSION, MIN_SIZE, EXTRA_MULTIPLE)   \
    {                                                   \
        OFPUTIL_OFPT_##TYPE,                            \
        { 0, OFPT_##TYPE, 0, 0, 0 },                    \
        "OFPT_" #TYPE,                                  \
        MIN_SIZE,                                       \
        EXTRA_MULTIPLE                                  \
    }
    OFPT(ERROR, 0, sizeof(struct ofp_error_msg), 1),
#undef OFPT

#define OFPT10(TYPE, RAW_TYPE, MIN_SIZE, EXTRA_MULTIPLE)    \
    {                                                       \
        OFPUTIL_##TYPE,                                     \
        { OFP10_VERSION, RAW_TYPE, 0, 0, 0 },               \
        #TYPE,                                              \
        MIN_SIZE,                                           \
        EXTRA_MULTIPLE                                      \
    }
    OFPT10(OFPT_HELLO,              OFPT_HELLO,
           sizeof(struct ofp_hello), 1),
    OFPT10(OFPT_ECHO_REQUEST,       OFPT_ECHO_REQUEST,
           sizeof(struct ofp_header), 1),
    OFPT10(OFPT_ECHO_REPLY,         OFPT_ECHO_REPLY,
           sizeof(struct ofp_header), 1),
    OFPT10(OFPT_FEATURES_REQUEST,   OFPT_FEATURES_REQUEST,
           sizeof(struct ofp_header), 0),
    OFPT10(OFPT_FEATURES_REPLY,     OFPT_FEATURES_REPLY,
           sizeof(struct ofp_switch_features), sizeof(struct ofp10_phy_port)),
    OFPT10(OFPT_GET_CONFIG_REQUEST, OFPT_GET_CONFIG_REQUEST,
           sizeof(struct ofp_header), 0),
    OFPT10(OFPT_GET_CONFIG_REPLY,   OFPT_GET_CONFIG_REPLY,
           sizeof(struct ofp_switch_config), 0),
    OFPT10(OFPT_SET_CONFIG,         OFPT_SET_CONFIG,
           sizeof(struct ofp_switch_config), 0),
    OFPT10(OFPT_PACKET_IN,          OFPT_PACKET_IN,
           offsetof(struct ofp_packet_in, data), 1),
    OFPT10(OFPT_FLOW_REMOVED,       OFPT_FLOW_REMOVED,
           sizeof(struct ofp_flow_removed), 0),
    OFPT10(OFPT_PORT_STATUS,        OFPT_PORT_STATUS,
           sizeof(struct ofp_port_status) + sizeof(struct ofp10_phy_port), 0),
    OFPT10(OFPT_PACKET_OUT,         OFPT10_PACKET_OUT,
           sizeof(struct ofp_packet_out), 1),
    OFPT10(OFPT10_FLOW_MOD,         OFPT10_FLOW_MOD,
           sizeof(struct ofp10_flow_mod), 1),
    OFPT10(OFPT_PORT_MOD,           OFPT10_PORT_MOD,
           sizeof(struct ofp10_port_mod), 0),
    OFPT10(OFPT_BARRIER_REQUEST,    OFPT10_BARRIER_REQUEST,
           sizeof(struct ofp_header), 0),
    OFPT10(OFPT_BARRIER_REPLY,      OFPT10_BARRIER_REPLY,
           sizeof(struct ofp_header), 0),

#define OFPT11(TYPE, RAW_TYPE, MIN_SIZE, EXTRA_MULTIPLE) \
    {                                           \
        OFPUTIL_##TYPE,                         \
        { OFP11_VERSION, RAW_TYPE, 0, 0, 0 },   \
        #TYPE,                                  \
        MIN_SIZE,                               \
        EXTRA_MULTIPLE                          \
    }
    OFPT11(OFPT_FEATURES_REPLY, OFPT_FEATURES_REPLY,
           sizeof(struct ofp_switch_features), sizeof(struct ofp11_port)),
    OFPT11(OFPT_PORT_STATUS,    OFPT_PORT_STATUS,
           sizeof(struct ofp_port_status) + sizeof(struct ofp11_port), 0),
    OFPT11(OFPT_PACKET_OUT,     OFPT11_PACKET_OUT,
           sizeof(struct ofp11_packet_out), 1),
    OFPT11(OFPT11_FLOW_MOD,     OFPT11_FLOW_MOD,
           sizeof(struct ofp11_flow_mod), 1),
    OFPT11(OFPT_PORT_MOD,       OFPT11_PORT_MOD,
           sizeof(struct ofp11_port_mod), 0),
#undef OPFT11

#define OFPT12(TYPE, RAW_TYPE, MIN_SIZE, EXTRA_MULTIPLE) \
    {                                           \
        OFPUTIL_##TYPE,                         \
        { OFP12_VERSION, RAW_TYPE, 0, 0, 0 },   \
        #TYPE,                                  \
        MIN_SIZE,                               \
        EXTRA_MULTIPLE                          \
    }
    OFPT12(OFPT_HELLO,              OFPT_HELLO,
           sizeof(struct ofp_hello), 1),
    OFPT12(OFPT_ECHO_REQUEST,       OFPT_ECHO_REQUEST,
           sizeof(struct ofp_header), 1),
    OFPT12(OFPT_ECHO_REPLY,         OFPT_ECHO_REPLY,
           sizeof(struct ofp_header), 1),
    OFPT12(OFPT_FEATURES_REQUEST,   OFPT_FEATURES_REQUEST,
           sizeof(struct ofp_header), 0),
    OFPT12(OFPT_FEATURES_REPLY, OFPT_FEATURES_REPLY,
           sizeof(struct ofp_switch_features), sizeof(struct ofp11_port)),
    OFPT12(OFPT_GET_CONFIG_REQUEST, OFPT_GET_CONFIG_REQUEST,
           sizeof(struct ofp_header), 0),
    OFPT12(OFPT_GET_CONFIG_REPLY,   OFPT_GET_CONFIG_REPLY,
           sizeof(struct ofp_switch_config), 0),
    OFPT12(OFPT_SET_CONFIG,         OFPT_SET_CONFIG,
           sizeof(struct ofp_switch_config), 0),
    OFPT12(OFPT_FLOW_REMOVED,       OFPT_FLOW_REMOVED,
           sizeof(struct ofp12_flow_removed), 0),
    OFPT12(OFPT_PACKET_IN,          OFPT_PACKET_IN,
           offsetof(struct ofp_packet_in, data), 1),
    OFPT12(OFPT_PACKET_OUT,     OFPT11_PACKET_OUT,
           sizeof(struct ofp11_packet_out), 1),
    OFPT12(OFPT_SET_CONFIG,         OFPT_SET_CONFIG,
           sizeof(struct ofp_switch_config), 0),
    OFPT12(OFPT11_FLOW_MOD,     OFPT11_FLOW_MOD,
           sizeof(struct ofp11_flow_mod), 1),
    OFPT12(OFPT_PORT_MOD,       OFPT11_PORT_MOD,
           sizeof(struct ofp11_port_mod), 0),
    OFPT12(OFPT_BARRIER_REQUEST,    OFPT11_BARRIER_REQUEST,
           sizeof(struct ofp_header), 0),
    OFPT12(OFPT_BARRIER_REPLY,      OFPT11_BARRIER_REPLY,
           sizeof(struct ofp_header), 0),
#undef OPFT12

#define OFPST10_REQUEST(STAT, RAW_STAT, MIN_SIZE, EXTRA_MULTIPLE)  \
    {                                                           \
        OFPUTIL_##STAT##_REQUEST,                               \
        { OFP10_VERSION, OFPT10_STATS_REQUEST, RAW_STAT, 0, 0 },\
        #STAT " request",                                       \
        sizeof(struct ofp10_stats_msg) + (MIN_SIZE),            \
        EXTRA_MULTIPLE                                          \
    }
    OFPST10_REQUEST(OFPST_DESC, OFPST_DESC, 0, 0),
    OFPST10_REQUEST(OFPST10_FLOW, OFPST_FLOW,
                    sizeof(struct ofp10_flow_stats_request), 0),
    OFPST10_REQUEST(OFPST10_AGGREGATE, OFPST_AGGREGATE,
                    sizeof(struct ofp10_flow_stats_request), 0),
    OFPST10_REQUEST(OFPST_TABLE, OFPST_TABLE, 0, 0),
    OFPST10_REQUEST(OFPST_PORT, OFPST_PORT,
                    sizeof(struct ofp10_port_stats_request), 0),
    OFPST10_REQUEST(OFPST_QUEUE, OFPST_QUEUE,
                    sizeof(struct ofp10_queue_stats_request), 0),
    OFPST10_REQUEST(OFPST_PORT_DESC, OFPST_PORT_DESC, 0, 0),
#undef OFPST10_REQUEST

#define OFPST11_REQUEST(STAT, RAW_STAT, MIN_SIZE, EXTRA_MULTIPLE)  \
    {                                                           \
        OFPUTIL_##STAT##_REQUEST,                               \
        { OFP11_VERSION, OFPT11_STATS_REQUEST, RAW_STAT, 0, 0 },\
        #STAT " request",                                       \
        sizeof(struct ofp11_stats_msg) + (MIN_SIZE),            \
        EXTRA_MULTIPLE                                          \
    }
    OFPST11_REQUEST(OFPST_DESC, OFPST_DESC, 0, 0),
    OFPST11_REQUEST(OFPST_TABLE, OFPST_TABLE, 0, 0),
    OFPST11_REQUEST(OFPST_PORT, OFPST_PORT,
                    sizeof(struct ofp11_port_stats_request), 0),
    OFPST11_REQUEST(OFPST_QUEUE, OFPST_QUEUE,
                    sizeof(struct ofp11_queue_stats_request), 0),
    OFPST11_REQUEST(OFPST_PORT_DESC, OFPST_PORT_DESC, 0, 0),
#undef OFPST11_REQUEST

#define OFPST12_REQUEST(STAT, RAW_STAT, MIN_SIZE, EXTRA_MULTIPLE)  \
    {                                                           \
        OFPUTIL_##STAT##_REQUEST,                               \
        { OFP12_VERSION, OFPT11_STATS_REQUEST, RAW_STAT, 0, 0 },\
        #STAT " request",                                       \
        sizeof(struct ofp11_stats_msg) + (MIN_SIZE),            \
        EXTRA_MULTIPLE                                          \
    }
    OFPST12_REQUEST(OFPST_DESC, OFPST_DESC, 0, 0),
    OFPST12_REQUEST(OFPST11_FLOW, OFPST_FLOW,
                    sizeof(struct ofp11_flow_stats_request), 1),
    OFPST12_REQUEST(OFPST11_AGGREGATE, OFPST_AGGREGATE,
                    sizeof(struct ofp11_flow_stats_request), 1),
    OFPST12_REQUEST(OFPST_TABLE, OFPST_TABLE, 0, 0),
    OFPST12_REQUEST(OFPST_PORT, OFPST_PORT,
                    sizeof(struct ofp11_port_stats_request), 0),
    OFPST12_REQUEST(OFPST_QUEUE, OFPST_QUEUE,
                    sizeof(struct ofp11_queue_stats_request), 0),
    OFPST12_REQUEST(OFPST_PORT_DESC, OFPST_PORT_DESC, 0, 0),
#undef OFPST12_REQUEST

#define OFPST10_REPLY(STAT, RAW_STAT, MIN_SIZE, EXTRA_MULTIPLE)  \
    {                                                       \
        OFPUTIL_##STAT##_REPLY,                             \
        { OFP10_VERSION, OFPT10_STATS_REPLY, RAW_STAT, 0, 0 },  \
        #STAT " reply",                                     \
        sizeof(struct ofp10_stats_msg) + (MIN_SIZE),        \
        EXTRA_MULTIPLE                                      \
    }
    OFPST10_REPLY(OFPST_DESC, OFPST_DESC,
                  sizeof(struct ofp_desc_stats), 0),
    OFPST10_REPLY(OFPST10_FLOW, OFPST_FLOW, 0, 1),
    OFPST10_REPLY(OFPST10_AGGREGATE, OFPST_AGGREGATE,
                  sizeof(struct ofp10_aggregate_stats_reply), 0),
    OFPST10_REPLY(OFPST_TABLE, OFPST_TABLE,
                  0, sizeof(struct ofp10_table_stats)),
    OFPST10_REPLY(OFPST_PORT, OFPST_PORT,
                  0, sizeof(struct ofp10_port_stats)),
    OFPST10_REPLY(OFPST_QUEUE, OFPST_QUEUE,
                  0, sizeof(struct ofp10_queue_stats)),
    OFPST10_REPLY(OFPST_PORT_DESC, OFPST_PORT_DESC,
                  0, sizeof(struct ofp10_phy_port)),
#undef OFPST10_REPLY

#define OFPST11_REPLY(STAT, RAW_STAT, MIN_SIZE, EXTRA_MULTIPLE)  \
    {                                                       \
        OFPUTIL_##STAT##_REPLY,                             \
        { OFP11_VERSION, OFPT11_STATS_REPLY, RAW_STAT, 0, 0 },  \
        #STAT " reply",                                     \
        sizeof(struct ofp11_stats_msg) + (MIN_SIZE),        \
        EXTRA_MULTIPLE                                      \
    }
    OFPST11_REPLY(OFPST_DESC, OFPST_DESC,
                  sizeof(struct ofp_desc_stats), 0),
    OFPST11_REPLY(OFPST11_AGGREGATE, OFPST_AGGREGATE,
                  sizeof(struct ofp11_aggregate_stats_reply), 0),
    OFPST11_REPLY(OFPST_TABLE, OFPST_TABLE,
                  0, sizeof(struct ofp11_table_stats)),
    OFPST11_REPLY(OFPST_PORT, OFPST_PORT,
                  0, sizeof(struct ofp11_port_stats)),
    OFPST11_REPLY(OFPST_QUEUE, OFPST_QUEUE,
                  0, sizeof(struct ofp11_queue_stats)),
    OFPST11_REPLY(OFPST_PORT_DESC, OFPST_PORT_DESC,
                  0, sizeof(struct ofp11_port)),
#undef OFPST11_REPLY

#define OFPST12_REPLY(STAT, RAW_STAT, MIN_SIZE, EXTRA_MULTIPLE)  \
    {                                                       \
        OFPUTIL_##STAT##_REPLY,                             \
        { OFP12_VERSION, OFPT11_STATS_REPLY, RAW_STAT, 0, 0 },  \
        #STAT " reply",                                     \
        sizeof(struct ofp11_stats_msg) + (MIN_SIZE),        \
        EXTRA_MULTIPLE                                      \
    }
    OFPST12_REPLY(OFPST_DESC, OFPST_DESC,
                  sizeof(struct ofp_desc_stats), 0),
    OFPST12_REPLY(OFPST11_FLOW, OFPST_FLOW, 0, 1),
    OFPST12_REPLY(OFPST11_AGGREGATE, OFPST_AGGREGATE,
                  sizeof(struct ofp11_aggregate_stats_reply), 0),
    OFPST12_REPLY(OFPST_TABLE, OFPST_TABLE,
                  0, sizeof(struct ofp12_table_stats)),
    OFPST12_REPLY(OFPST_PORT, OFPST_PORT,
                  0, sizeof(struct ofp11_port_stats)),
    OFPST12_REPLY(OFPST_QUEUE, OFPST_QUEUE,
                  0, sizeof(struct ofp11_queue_stats)),
    OFPST12_REPLY(OFPST_PORT_DESC, OFPST_PORT_DESC,
                  0, sizeof(struct ofp11_port)),
#undef OFPST12_REPLY

#define NXT(SUBTYPE, MIN_SIZE, EXTRA_MULTIPLE)                          \
    {                                                                   \
        OFPUTIL_NXT_##SUBTYPE,                                          \
        { OFP10_VERSION, OFPT_VENDOR, 0, NX_VENDOR_ID, NXT_##SUBTYPE }, \
        "NXT_" #SUBTYPE,                                                \
        MIN_SIZE,                                                       \
        EXTRA_MULTIPLE                                                  \
    }
    NXT(ROLE_REQUEST, sizeof(struct nx_role_request), 0),
    NXT(ROLE_REPLY, sizeof(struct nx_role_request), 0),
    NXT(SET_FLOW_FORMAT, sizeof(struct nx_set_flow_format), 0),
    NXT(SET_PACKET_IN_FORMAT, sizeof(struct nx_set_packet_in_format), 0),
    NXT(PACKET_IN, sizeof(struct nx_packet_in), 1),
    NXT(FLOW_MOD, sizeof(struct nx_flow_mod), 8),
    NXT(FLOW_REMOVED, sizeof(struct nx_flow_removed), 8),
    NXT(FLOW_MOD_TABLE_ID, sizeof(struct nx_flow_mod_table_id), 0),
    NXT(FLOW_AGE, sizeof(struct nicira_header), 0),
    NXT(SET_ASYNC_CONFIG, sizeof(struct nx_async_config), 0),
    NXT(SET_CONTROLLER_ID, sizeof(struct nx_controller_id), 0),
#undef NXT

#define NXST_REQUEST(SUBTYPE, MIN_SIZE, EXTRA_MULTIPLE)         \
    {                                                           \
        OFPUTIL_NXST_##SUBTYPE##_REQUEST,                       \
        {                                                       \
            OFP10_VERSION, OFPT10_STATS_REQUEST, OFPST_VENDOR,  \
            NX_VENDOR_ID, NXST_##SUBTYPE                        \
        },                                                      \
        "NXST_" #SUBTYPE " request",                            \
        sizeof(struct nicira10_stats_msg) + (MIN_SIZE),     \
        EXTRA_MULTIPLE                                          \
    }
    NXST_REQUEST(FLOW, sizeof(struct nx_flow_stats_request), 8),
    NXST_REQUEST(AGGREGATE, sizeof(struct nx_aggregate_stats_request), 8),
#undef NXST_REQUEST

#define NXST_REPLY(SUBTYPE, MIN_SIZE, EXTRA_MULTIPLE)       \
    {                                                       \
        OFPUTIL_NXST_##SUBTYPE##_REPLY,                     \
        {                                                   \
            OFP10_VERSION, OFPT10_STATS_REPLY, OFPST_VENDOR,    \
            NX_VENDOR_ID, NXST_##SUBTYPE                    \
        },                                                  \
        "NXST_" #SUBTYPE " reply",                          \
        sizeof(struct nicira10_stats_msg) + (MIN_SIZE),     \
        EXTRA_MULTIPLE                                      \
    }
    NXST_REPLY(FLOW, 0, 8),
    NXST_REPLY(AGGREGATE, sizeof(struct ofp11_aggregate_stats_reply), 0),
#undef NXST_REPLY
};

/* Represents a malformed OpenFlow message. */
static const struct ofputil_msg_type ofputil_invalid_type = {
    OFPUTIL_MSG_INVALID, { 0, 0, 0, 0, 0 }, "OFPUTIL_MSG_INVALID", 0, 0
};

static enum ofperr
ofputil_check_length(const struct ofputil_msg_type *type, unsigned int size)
{
    switch (type->extra_multiple) {
    case 0:
        if (size != type->min_size) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "received %s with incorrect "
                         "length %u (expected length %u)",
                         type->name, size, type->min_size);
            return OFPERR_OFPBRC_BAD_LEN;
        }
        return 0;

    case 1:
        if (size < type->min_size) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "received %s with incorrect "
                         "length %u (expected length at least %u bytes)",
                         type->name, size, type->min_size);
            return OFPERR_OFPBRC_BAD_LEN;
        }
        return 0;

    default:
        if (size < type->min_size
            || (size - type->min_size) % type->extra_multiple) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "received %s with incorrect "
                         "length %u (must be exactly %u bytes or longer "
                         "by an integer multiple of %u bytes)",
                         type->name, size,
                         type->min_size, type->extra_multiple);
            return OFPERR_OFPBRC_BAD_LEN;
        }
        return 0;
    }
}

static bool
raw_msg_match(const struct ofputil_raw_msg_type *want,
              const struct ofputil_raw_msg_type *have)
{
    return ((!want->version || want->version == have->version)
            && want->type == have->type
            && want->stat == have->stat
            && want->vendor == have->vendor
            && want->subtype == have->subtype);
}

static enum ofperr
ofputil_decode_msg_type__(const struct ofp_header *oh, size_t length,
                          const struct ofputil_msg_type **typep)
{
    struct ofputil_raw_msg_type raw;
    const struct ofputil_msg_type *type;
    enum ofperr error;

    error = ofputil_decode_raw_msg_type(oh, length, &raw);
    if (error) {
        return error;
    }

    for (type = ofputil_msg_types;
         type < &ofputil_msg_types[ARRAY_SIZE(ofputil_msg_types)];
         type++) {
        if (raw_msg_match(&type->raw, &raw)) {
            *typep = type;
            return 0;
        }
    }

    return (raw.vendor ? OFPERR_OFPBRC_BAD_SUBTYPE
            : raw.stat ? OFPERR_OFPBRC_BAD_STAT
            : OFPERR_OFPBRC_BAD_TYPE);
}

/* Decodes the message type represented by 'oh'.  Returns 0 if successful or an
 * OpenFlow error code on failure.  Either way, stores in '*typep' a type
 * structure that can be inspected with the ofputil_msg_type_*() functions.
 *
 * oh->length must indicate the correct length of the message (and must be at
 * least sizeof(struct ofp_header)).
 *
 * Success indicates that 'oh' is at least as long as the minimum-length
 * message of its type. */
enum ofperr
ofputil_decode_msg_type(const struct ofp_header *oh,
                        const struct ofputil_msg_type **typep)
{
    size_t length = ntohs(oh->length);
    enum ofperr error;

    error = ofputil_decode_msg_type__(oh, length, typep);
    if (!error) {
        error = ofputil_check_length(*typep, length);
    }
    if (error) {
        *typep = &ofputil_invalid_type;
    }
    return error;
}

/* Decodes the message type represented by 'oh', of which only the first
 * 'length' bytes are available.  Returns 0 if successful or an OpenFlow error
 * code on failure.  Either way, stores in '*typep' a type structure that can
 * be inspected with the ofputil_msg_type_*() functions.  */
enum ofperr
ofputil_decode_msg_type_partial(const struct ofp_header *oh, size_t length,
                                const struct ofputil_msg_type **typep)
{
    enum ofperr error;

    error = (length >= sizeof *oh
             ? ofputil_decode_msg_type__(oh, length, typep)
             : OFPERR_OFPBRC_BAD_LEN);
    if (error) {
        *typep = &ofputil_invalid_type;
    }
    return error;
}

/* Returns an OFPUTIL_* message type code for 'type'. */
enum ofputil_msg_code
ofputil_msg_type_code(const struct ofputil_msg_type *type)
{
    return type->code;
}

/* Protocols. */

struct proto_abbrev {
    enum ofputil_protocol protocol;
    const char *name;
};

/* Most users really don't care about some of the differences between
 * protocols.  These abbreviations help with that. */
static const struct proto_abbrev proto_abbrevs[] = {
    { OFPUTIL_P_ANY,      "any" },
    { OFPUTIL_P_OF10_ANY, "OpenFlow10" },
    { OFPUTIL_P_NXM_ANY,  "NXM" },
};
#define N_PROTO_ABBREVS ARRAY_SIZE(proto_abbrevs)

enum ofputil_protocol ofputil_flow_dump_protocols[] = {
    OFPUTIL_P_OF12,
    OFPUTIL_P_NXM,
    OFPUTIL_P_OF10,
};
size_t ofputil_n_flow_dump_protocols = ARRAY_SIZE(ofputil_flow_dump_protocols);

static enum ofputil_protocol ofputil_usable_protocols_with_actions(
    const struct ofpact *ofpacts);

/* Returns the ofputil_protocol that is initially in effect on an OpenFlow
 * connection that has negotiated the given 'version'.  'version' should
 * normally be an 8-bit OpenFlow version identifier (e.g. 0x01 for OpenFlow
 * 1.0, 0x02 for OpenFlow 1.1).  Returns 0 if 'version' is not supported or
 * outside the valid range.  */
enum ofputil_protocol
ofputil_protocol_from_ofp_version(int version)
{
    switch (version) {
    case OFP10_VERSION: return OFPUTIL_P_OF10;
    case OFP12_VERSION: return OFPUTIL_P_OF12;
    default: return 0;
    }
}

/* Returns the OpenFlow protocol version number (e.g. OFP10_VERSION,
 * OFP11_VERSION or OFP12_VERSION) that corresponds to 'protocol'. */
uint8_t
ofputil_protocol_to_ofp_version(enum ofputil_protocol protocol)
{
    switch (protocol) {
    case OFPUTIL_P_OF10:
    case OFPUTIL_P_OF10_TID:
    case OFPUTIL_P_NXM:
    case OFPUTIL_P_NXM_TID:
        return OFP10_VERSION;
    case OFPUTIL_P_OF12:
        return OFP12_VERSION;
    }

    NOT_REACHED();
}

/* Returns true if 'protocol' is a single OFPUTIL_P_* value, false
 * otherwise. */
bool
ofputil_protocol_is_valid(enum ofputil_protocol protocol)
{
    return protocol & OFPUTIL_P_ANY && is_pow2(protocol);
}

/* Returns the equivalent of 'protocol' with the Nicira flow_mod_table_id
 * extension turned on or off if 'enable' is true or false, respectively.
 *
 * This extension is only useful for protocols whose "standard" version does
 * not allow specific tables to be modified.  In particular, this is true of
 * OpenFlow 1.0.  In later versions of OpenFlow, a flow_mod request always
 * specifies a table ID and so there is no need for such an extension.  When
 * 'protocol' is such a protocol that doesn't need a flow_mod_table_id
 * extension, this function just returns its 'protocol' argument unchanged
 * regardless of the value of 'enable'.  */
enum ofputil_protocol
ofputil_protocol_set_tid(enum ofputil_protocol protocol, bool enable)
{
    switch (protocol) {
    case OFPUTIL_P_OF10:
    case OFPUTIL_P_OF10_TID:
        return enable ? OFPUTIL_P_OF10_TID : OFPUTIL_P_OF10;

    case OFPUTIL_P_NXM:
    case OFPUTIL_P_NXM_TID:
        return enable ? OFPUTIL_P_NXM_TID : OFPUTIL_P_NXM;

    case OFPUTIL_P_OF12:
        return OFPUTIL_P_OF12;

    default:
        NOT_REACHED();
    }
}

/* Returns the "base" version of 'protocol'.  That is, if 'protocol' includes
 * some extension to a standard protocol version, the return value is the
 * standard version of that protocol without any extension.  If 'protocol' is a
 * standard protocol version, returns 'protocol' unchanged. */
enum ofputil_protocol
ofputil_protocol_to_base(enum ofputil_protocol protocol)
{
    return ofputil_protocol_set_tid(protocol, false);
}

/* Returns 'new_base' with any extensions taken from 'cur'. */
enum ofputil_protocol
ofputil_protocol_set_base(enum ofputil_protocol cur,
                          enum ofputil_protocol new_base)
{
    bool tid = (cur & OFPUTIL_P_TID) != 0;

    switch (new_base) {
    case OFPUTIL_P_OF10:
    case OFPUTIL_P_OF10_TID:
        return ofputil_protocol_set_tid(OFPUTIL_P_OF10, tid);

    case OFPUTIL_P_NXM:
    case OFPUTIL_P_NXM_TID:
        return ofputil_protocol_set_tid(OFPUTIL_P_NXM, tid);

    case OFPUTIL_P_OF12:
        return ofputil_protocol_set_tid(OFPUTIL_P_OF12, tid);

    default:
        NOT_REACHED();
    }
}

/* Returns a string form of 'protocol', if a simple form exists (that is, if
 * 'protocol' is either a single protocol or it is a combination of protocols
 * that have a single abbreviation).  Otherwise, returns NULL. */
const char *
ofputil_protocol_to_string(enum ofputil_protocol protocol)
{
    const struct proto_abbrev *p;

    /* Use a "switch" statement for single-bit names so that we get a compiler
     * warning if we forget any. */
    switch (protocol) {
    case OFPUTIL_P_NXM:
        return "NXM-table_id";

    case OFPUTIL_P_NXM_TID:
        return "NXM+table_id";

    case OFPUTIL_P_OF10:
        return "OpenFlow10-table_id";

    case OFPUTIL_P_OF10_TID:
        return "OpenFlow10+table_id";

    case OFPUTIL_P_OF12:
        return "OpenFlow12";
    }

    /* Check abbreviations. */
    for (p = proto_abbrevs; p < &proto_abbrevs[N_PROTO_ABBREVS]; p++) {
        if (protocol == p->protocol) {
            return p->name;
        }
    }

    return NULL;
}

/* Returns a string that represents 'protocols'.  The return value might be a
 * comma-separated list if 'protocols' doesn't have a simple name.  The return
 * value is "none" if 'protocols' is 0.
 *
 * The caller must free the returned string (with free()). */
char *
ofputil_protocols_to_string(enum ofputil_protocol protocols)
{
    struct ds s;

    assert(!(protocols & ~OFPUTIL_P_ANY));
    if (protocols == 0) {
        return xstrdup("none");
    }

    ds_init(&s);
    while (protocols) {
        const struct proto_abbrev *p;
        int i;

        if (s.length) {
            ds_put_char(&s, ',');
        }

        for (p = proto_abbrevs; p < &proto_abbrevs[N_PROTO_ABBREVS]; p++) {
            if ((protocols & p->protocol) == p->protocol) {
                ds_put_cstr(&s, p->name);
                protocols &= ~p->protocol;
                goto match;
            }
        }

        for (i = 0; i < CHAR_BIT * sizeof(enum ofputil_protocol); i++) {
            enum ofputil_protocol bit = 1u << i;

            if (protocols & bit) {
                ds_put_cstr(&s, ofputil_protocol_to_string(bit));
                protocols &= ~bit;
                goto match;
            }
        }
        NOT_REACHED();

    match: ;
    }
    return ds_steal_cstr(&s);
}

static enum ofputil_protocol
ofputil_protocol_from_string__(const char *s, size_t n)
{
    const struct proto_abbrev *p;
    int i;

    for (i = 0; i < CHAR_BIT * sizeof(enum ofputil_protocol); i++) {
        enum ofputil_protocol bit = 1u << i;
        const char *name = ofputil_protocol_to_string(bit);

        if (name && n == strlen(name) && !strncasecmp(s, name, n)) {
            return bit;
        }
    }

    for (p = proto_abbrevs; p < &proto_abbrevs[N_PROTO_ABBREVS]; p++) {
        if (n == strlen(p->name) && !strncasecmp(s, p->name, n)) {
            return p->protocol;
        }
    }

    return 0;
}

/* Returns the nonempty set of protocols represented by 's', which can be a
 * single protocol name or abbreviation or a comma-separated list of them.
 *
 * Aborts the program with an error message if 's' is invalid. */
enum ofputil_protocol
ofputil_protocols_from_string(const char *s)
{
    const char *orig_s = s;
    enum ofputil_protocol protocols;

    protocols = 0;
    while (*s) {
        enum ofputil_protocol p;
        size_t n;

        n = strcspn(s, ",");
        if (n == 0) {
            s++;
            continue;
        }

        p = ofputil_protocol_from_string__(s, n);
        if (!p) {
            ovs_fatal(0, "%.*s: unknown flow protocol", (int) n, s);
        }
        protocols |= p;

        s += n;
    }

    if (!protocols) {
        ovs_fatal(0, "%s: no flow protocol specified", orig_s);
    }
    return protocols;
}

bool
ofputil_packet_in_format_is_valid(enum nx_packet_in_format packet_in_format)
{
    switch (packet_in_format) {
    case NXPIF_OPENFLOW10:
    case NXPIF_NXM:
        return true;
    }

    return false;
}

const char *
ofputil_packet_in_format_to_string(enum nx_packet_in_format packet_in_format)
{
    switch (packet_in_format) {
    case NXPIF_OPENFLOW10:
        return "openflow10";
    case NXPIF_NXM:
        return "nxm";
    default:
        NOT_REACHED();
    }
}

int
ofputil_packet_in_format_from_string(const char *s)
{
    return (!strcmp(s, "openflow10") ? NXPIF_OPENFLOW10
            : !strcmp(s, "nxm") ? NXPIF_NXM
            : -1);
}

static bool
regs_fully_wildcarded(const struct flow_wildcards *wc)
{
    int i;

    for (i = 0; i < FLOW_N_REGS; i++) {
        if (wc->reg_masks[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Returns a bit-mask of ofputil_protocols that can be used for sending 'rule'
 * to a switch (e.g. to add or remove a flow).  Only NXM can handle tunnel IDs,
 * registers, or fixing the Ethernet multicast bit.  Otherwise, it's better to
 * use OpenFlow 1.0 protocol for backward compatibility. */
enum ofputil_protocol
ofputil_usable_protocols(const struct cls_rule *rule)
{
    const struct flow_wildcards *wc = &rule->wc;

    BUILD_ASSERT_DECL(FLOW_WC_SEQ == 13);

    /* NXM and OF1.1+ supports bitwise matching on ethernet addresses. */
    if (!eth_mask_is_exact(wc->dl_src_mask)
        && !eth_addr_is_zero(wc->dl_src_mask)) {
        return OFPUTIL_P_NXM_ANY;
    }
    if (!eth_mask_is_exact(wc->dl_dst_mask)
        && !eth_addr_is_zero(wc->dl_dst_mask)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching ARP hardware addresses. */
    if (!(wc->wildcards & FWW_ARP_SHA) || !(wc->wildcards & FWW_ARP_THA)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching IPv6 traffic. */
    if (!(wc->wildcards & FWW_DL_TYPE)
            && (rule->flow.dl_type == htons(ETH_TYPE_IPV6))) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching registers. */
    if (!regs_fully_wildcarded(wc)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching tun_id. */
    if (wc->tun_id_mask != htonll(0)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching fragments. */
    if (wc->nw_frag_mask) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching IPv6 flow label. */
    if (!(wc->wildcards & FWW_IPV6_LABEL)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching IP ECN bits. */
    if (!(wc->wildcards & FWW_NW_ECN)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching IP TTL/hop limit. */
    if (!(wc->wildcards & FWW_NW_TTL)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports non-CIDR IPv4 address masks. */
    if (!ip_is_cidr(wc->nw_src_mask) || !ip_is_cidr(wc->nw_dst_mask)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports bitwise matching on transport port. */
    if ((wc->tp_src_mask && wc->tp_src_mask != htons(UINT16_MAX)) ||
        (wc->tp_dst_mask && wc->tp_dst_mask != htons(UINT16_MAX))) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching mpls label */
    if (!(wc->wildcards & FWW_MPLS_LABEL)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching mpls tc */
    if (!(wc->wildcards & FWW_MPLS_TC)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching mpls stack */
    if (!(wc->wildcards & FWW_MPLS_STACK)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching vlan tpid */
    if (!(wc->wildcards & FWW_VLAN_TPID)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching vlan qinq vid */
    if (!(wc->wildcards & FWW_VLAN_QINQ_VID)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Only NXM supports matching vlan qinq pcp */
    if (!(wc->wildcards & FWW_VLAN_QINQ_PCP)) {
        return OFPUTIL_P_NXM_ANY;
    }

    /* Other formats can express this rule. */
    return OFPUTIL_P_ANY;
}

static enum ofputil_protocol
ofputil_usable_protocols_with_action(const struct ofpact *ofpact)
{
    /* FIXME: OF12 + nicira case */
    enum ofputil_protocol protocols = OFPUTIL_P_ANY | OFPUTIL_P_TID;
    struct ofpact_inst_actions *oia;

    if (ofpact_is_instruction(ofpact)) {
        protocols &= OFPUTIL_P_NXM_ANY | OFPUTIL_P_OF12; /* XXX: OF11 */
    }
    switch (ofpact->type) {
    case OFPACT_END:
        break;

    /* instructions */
    case OFPACT_APPLY_ACTIONS:
        oia = ofpact_get_APPLY_ACTIONS(ofpact);
        protocols &= ofputil_usable_protocols_with_actions(oia->ofpacts);
        break;

    case OFPACT_WRITE_ACTIONS:
        oia = ofpact_get_APPLY_ACTIONS(ofpact);
        protocols &= ofputil_usable_protocols_with_actions(oia->ofpacts);
        break;

    case OFPACT_CLEAR_ACTIONS:
        break;

    case OFPACT_RESUBMIT:
        if (ofpact_is_instruction(ofpact)) {
            protocols &= OFPUTIL_P_OF12; /* XXX OF11 */
            break;
        }
        protocols &= OFPUTIL_P_NXM_ANY | OFPUTIL_P_OF12;
        break;

    case OFPACT_REG_LOAD:
        if (ofpact->compat == OFPUTIL_OFPAT12_SET_FIELD) {
            protocols &= OFPUTIL_P_OF12;
            break;
        }
        protocols &= OFPUTIL_P_NXM_ANY | OFPUTIL_P_OF12;
        break;

    case OFPACT_OUTPUT:
    case OFPACT_ENQUEUE:
    case OFPACT_SET_VLAN_VID:
    case OFPACT_SET_VLAN_PCP:
    case OFPACT_STRIP_VLAN:
    case OFPACT_SET_ETH_SRC:
    case OFPACT_SET_ETH_DST:
    case OFPACT_SET_IPV4_SRC:
    case OFPACT_SET_IPV4_DST:
    case OFPACT_SET_IPV4_DSCP:
        break;

    case OFPACT_COPY_TTL_OUT:
    case OFPACT_COPY_TTL_IN:
    case OFPACT_POP_VLAN:
        protocols &= OFPUTIL_P_OF12; /* XXX: OF11 */
        break;

    case OFPACT_PUSH_MPLS:
    case OFPACT_POP_MPLS:
    case OFPACT_PUSH_VLAN:
    case OFPACT_SET_MPLS_LABEL:
    case OFPACT_SET_MPLS_TC:
    case OFPACT_SET_MPLS_TTL:
    case OFPACT_DEC_MPLS_TTL:
        protocols &= (OFPUTIL_P_OF12 | OFPUTIL_P_NXM_ANY); /* XXX: OF11 */
        break;

    case OFPACT_SET_L4_SRC_PORT:
    case OFPACT_SET_L4_DST_PORT:
        /* OF12 doesn't support this */
        protocols &= (OFPUTIL_P_OF10 | OFPUTIL_P_NXM_ANY); /* XXX: OF11 */
        break;

    case OFPACT_CONTROLLER:
    case OFPACT_OUTPUT_REG:
    case OFPACT_BUNDLE:
    case OFPACT_REG_MOVE:
    case OFPACT_DEC_TTL:
    case OFPACT_SET_TUNNEL:
    case OFPACT_SET_QUEUE:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_LEARN:
    case OFPACT_MULTIPATH:
    case OFPACT_AUTOPATH:
    case OFPACT_NOTE:
    case OFPACT_EXIT:
        protocols &= OFPUTIL_P_NXM_ANY | OFPUTIL_P_OF12;
        break;
    }

    assert(protocols);
    return protocols;
}

static enum ofputil_protocol
ofputil_usable_protocols_with_actions(const struct ofpact *ofpacts)
{
    const struct ofpact *a;
    enum ofputil_protocol protocols = OFPUTIL_P_ANY;
    if (ofpacts) {
        OFPACT_FOR_EACH(a, ofpacts) {
            protocols &= ofputil_usable_protocols_with_action(a);
        }
    }
    assert(protocols);
    return protocols;
}

/* Returns an OpenFlow message that, sent on an OpenFlow connection whose
 * protocol is 'current', at least partly transitions the protocol to 'want'.
 * Stores in '*next' the protocol that will be in effect on the OpenFlow
 * connection if the switch processes the returned message correctly.  (If
 * '*next != want' then the caller will have to iterate.)
 *
 * If 'current == want', returns NULL and stores 'current' in '*next'. */
struct ofpbuf *
ofputil_encode_set_protocol(enum ofputil_protocol current,
                            enum ofputil_protocol want,
                            enum ofputil_protocol *next)
{
    enum ofputil_protocol cur_base, want_base;
    bool cur_tid, want_tid;

    cur_base = ofputil_protocol_to_base(current);
    want_base = ofputil_protocol_to_base(want);
    if (cur_base != want_base) {
        *next = ofputil_protocol_set_base(current, want_base);

        switch (want_base) {
        case OFPUTIL_P_NXM:
            return ofputil_encode_nx_set_flow_format(NXFF_NXM);

        case OFPUTIL_P_OF10:
            return ofputil_encode_nx_set_flow_format(NXFF_OPENFLOW10);

        case OFPUTIL_P_OF12:
            return ofputil_encode_nx_set_flow_format(NXFF_OPENFLOW12);

        case OFPUTIL_P_OF10_TID:
        case OFPUTIL_P_NXM_TID:
            NOT_REACHED();
        }
    }

    cur_tid = (current & OFPUTIL_P_TID) != 0;
    want_tid = (want & OFPUTIL_P_TID) != 0;
    if (cur_tid != want_tid) {
        *next = ofputil_protocol_set_tid(current, want_tid);
        return ofputil_make_flow_mod_table_id(want_tid);
    }

    assert(current == want);

    *next = current;
    return NULL;
}

/* Returns an NXT_SET_FLOW_FORMAT message that can be used to set the flow
 * format to 'nxff'.  */
struct ofpbuf *
ofputil_encode_nx_set_flow_format(enum nx_flow_format nxff)
{
    struct nx_set_flow_format *sff;
    struct ofpbuf *msg;

    assert(ofputil_nx_flow_format_is_valid(nxff));

    sff = make_nxmsg(sizeof *sff, NXT_SET_FLOW_FORMAT, &msg);
    sff->format = htonl(nxff);

    return msg;
}

/* Returns the base protocol if 'flow_format' is a valid NXFF_* value, false
 * otherwise. */
enum ofputil_protocol
ofputil_nx_flow_format_to_protocol(enum nx_flow_format flow_format)
{
    switch (flow_format) {
    case NXFF_OPENFLOW10:
        return OFPUTIL_P_OF10;

    case NXFF_NXM:
        return OFPUTIL_P_NXM;

    case NXFF_OPENFLOW12:
        return OFPUTIL_P_OF12;

    default:
        return 0;
    }
}

/* Returns true if 'flow_format' is a valid NXFF_* value, false otherwise. */
bool
ofputil_nx_flow_format_is_valid(enum nx_flow_format flow_format)
{
    return ofputil_nx_flow_format_to_protocol(flow_format) != 0;
}

/* Returns a string version of 'flow_format', which must be a valid NXFF_*
 * value. */
const char *
ofputil_nx_flow_format_to_string(enum nx_flow_format flow_format)
{
    switch (flow_format) {
    case NXFF_OPENFLOW10:
        return "openflow10";
    case NXFF_NXM:
        return "nxm";
    case NXFF_OPENFLOW12:
        return "openflow12";
    default:
        NOT_REACHED();
    }
}

struct ofpbuf *
ofputil_make_set_packet_in_format(enum nx_packet_in_format packet_in_format)
{
    struct nx_set_packet_in_format *spif;
    struct ofpbuf *msg;

    spif = make_nxmsg(sizeof *spif, NXT_SET_PACKET_IN_FORMAT, &msg);
    spif->format = htonl(packet_in_format);

    return msg;
}

/* Returns an OpenFlow message that can be used to turn the flow_mod_table_id
 * extension on or off (according to 'flow_mod_table_id'). */
struct ofpbuf *
ofputil_make_flow_mod_table_id(bool flow_mod_table_id)
{
    struct nx_flow_mod_table_id *nfmti;
    struct ofpbuf *msg;

    nfmti = make_nxmsg(sizeof *nfmti, NXT_FLOW_MOD_TABLE_ID, &msg);
    nfmti->set = flow_mod_table_id;
    return msg;
}

static int
ofputil_put_match(struct ofpbuf *msg, const struct cls_rule *cr,
                  ovs_be64 cookie, ovs_be64 cookie_mask,
                  enum ofputil_protocol protocol)
{
    int match_len;

    switch (protocol) {
    case OFPUTIL_P_OF10:
    case OFPUTIL_P_OF10_TID:
    default:
        NOT_REACHED();

    case OFPUTIL_P_NXM:
    case OFPUTIL_P_NXM_TID:
        match_len = nx_put_match(msg, false, cr, cookie, cookie_mask);
        break;

    case OFPUTIL_P_OF12: {
        struct ofp11_match_header *omh;
        size_t start_len = msg->size;

        ofpbuf_put_uninit(msg, sizeof *omh);
        match_len = nx_put_match(msg, true, cr, cookie, cookie_mask) +
            sizeof *omh;
        omh = (struct ofp11_match_header *)((char *)msg->data + start_len);
        omh->type = htons(OFPMT_OXM);
        omh->length = htons(match_len);
        break;
    }
    }

    return match_len;
}

/* Converts an OFPT_FLOW_MOD or NXT_FLOW_MOD message 'oh' into an abstract
 * flow_mod in 'fm'.  Returns 0 if successful, otherwise an OpenFlow error
 * code.
 *
 * Uses 'ofpacts' to store the abstract OFPACT_* version of 'oh''s actions.
 * The caller must initialize 'ofpacts' and retains ownership of it.
 * 'fm->ofpacts' will point into the 'ofpacts' buffer.
 *
 * Does not validate the flow_mod actions.  The caller should do that, with
 * ofpacts_check(). */
enum ofperr
ofputil_decode_flow_mod(struct ofputil_flow_mod *fm,
                        const struct ofp_header *oh,
                        enum ofputil_protocol protocol,
                        struct ofpbuf *ofpacts)
{
    const struct ofputil_msg_type *type;
    uint16_t command;
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));

    ofputil_decode_msg_type(oh, &type);
    if (ofputil_msg_type_code(type) == OFPUTIL_OFPT11_FLOW_MOD) {
        /* Standard OpenFlow 1.1 flow_mod. */
        const struct ofp11_flow_mod *ofm;
        enum ofperr error;

        ofm = ofpbuf_pull(&b, sizeof *ofm);

        error = __ofputil_pull_ofp11_match(&b, ntohs(ofm->priority),
                                           &fm->cr, &fm->cookie,
                                           &fm->cookie_mask, NULL,
                                           oh->version);
        if (error) {
            return error;
        }

        error = ofpacts_pull_openflow11_instructions(oh->version,
                                                     &b, b.size, ofpacts);
        if (error) {
            return error;
        }

        /* Translate the message. */
        if (ofm->command == OFPFC_ADD) {
            fm->cookie = htonll(0);
            fm->cookie_mask = htonll(0);
            fm->new_cookie = ofm->cookie;
        } else {
            /* XXX */
            fm->cookie = ofm->cookie;
            fm->cookie_mask = ofm->cookie_mask;
            fm->new_cookie = htonll(UINT64_MAX);
        }
        fm->command = ofm->command;
        fm->table_id = ofm->table_id;
        fm->idle_timeout = ntohs(ofm->idle_timeout);
        fm->hard_timeout = ntohs(ofm->hard_timeout);
        fm->buffer_id = ntohl(ofm->buffer_id);
        error = ofputil_port_from_ofp11(ofm->out_port, &fm->out_port);
        if (error) {
            return error;
        }
        if (ofm->out_group != htonl(OFPG_ANY)) {
            return OFPERR_NXFMFC_GROUPS_NOT_SUPPORTED;
        }
        fm->flags = ntohs(ofm->flags);
    } else {
        if (ofputil_msg_type_code(type) == OFPUTIL_OFPT10_FLOW_MOD) {
            /* Standard OpenFlow 1.0 flow_mod. */
            const struct ofp10_flow_mod *ofm;
            uint16_t priority;
            enum ofperr error;

            /* Get the ofp10_flow_mod. */
            ofm = ofpbuf_pull(&b, sizeof *ofm);

            /* Set priority based on original wildcards.  Normally we'd allow
             * ofputil_cls_rule_from_match() to do this for us, but
             * ofputil_normalize_rule() can put wildcards where the original flow
             * didn't have them. */
            priority = ntohs(ofm->priority);
            if (!(ofm->match.wildcards & htonl(OFPFW10_ALL))) {
                priority = UINT16_MAX;
            }

            /* Translate the rule. */
            ofputil_cls_rule_from_ofp10_match(&ofm->match, priority, &fm->cr);
            ofputil_normalize_rule(&fm->cr);

            /* Now get the actions. */
            error = ofpacts_pull_openflow10(&b, b.size, ofpacts);
            if (error) {
                return error;
            }

            /* Translate the message. */
            command = ntohs(ofm->command);
            fm->cookie = htonll(0);
            fm->cookie_mask = htonll(0);
            fm->new_cookie = ofm->cookie;
            fm->idle_timeout = ntohs(ofm->idle_timeout);
            fm->hard_timeout = ntohs(ofm->hard_timeout);
            fm->buffer_id = ntohl(ofm->buffer_id);
            fm->out_port = ntohs(ofm->out_port);
            fm->flags = ntohs(ofm->flags);
        } else if (ofputil_msg_type_code(type) == OFPUTIL_NXT_FLOW_MOD) {
            /* Nicira extended flow_mod. */
            const struct nx_flow_mod *nfm;
            enum ofperr error;

            /* Dissect the message. */
            nfm = ofpbuf_pull(&b, sizeof *nfm);
            error = nx_pull_match(&b, ntohs(nfm->match_len), 0,
                                  ntohs(nfm->priority), &fm->cr,
                                  &fm->cookie, &fm->cookie_mask);
            if (error) {
                return error;
            }
            error = ofpacts_pull_openflow10(&b, b.size, ofpacts);
            if (error) {
                return error;
            }

            /* Translate the message. */
            command = ntohs(nfm->command);
            if ((command & 0xff) == OFPFC_ADD && fm->cookie_mask) {
                /* Flow additions may only set a new cookie, not match an
                 * existing cookie. */
                return OFPERR_NXBRC_NXM_INVALID;
            }
            fm->new_cookie = nfm->cookie;
            fm->idle_timeout = ntohs(nfm->idle_timeout);
            fm->hard_timeout = ntohs(nfm->hard_timeout);
            fm->buffer_id = ntohl(nfm->buffer_id);
            fm->out_port = ntohs(nfm->out_port);
            fm->flags = ntohs(nfm->flags);
        } else {
            NOT_REACHED();
        }

        if (protocol & OFPUTIL_P_TID) {
            fm->command = command & 0xff;
            fm->table_id = command >> 8;
        } else {
            fm->command = command;
            fm->table_id = 0xff;
        }
    }

    fm->ofpacts = ofpacts->data;
    fm->ofpacts_len = ofpacts->size;

    return 0;
}

static ovs_be16
ofputil_tid_command(const struct ofputil_flow_mod *fm,
                    enum ofputil_protocol protocol)
{
    return htons(protocol & OFPUTIL_P_TID ?
                 (fm->command & 0xff) | (fm->table_id << 8)
                 : fm->command);
}

/* Converts 'fm' into an OFPT_FLOW_MOD or NXT_FLOW_MOD message according to
 * 'protocol' and returns the message. */
struct ofpbuf *
ofputil_encode_flow_mod(const struct ofputil_flow_mod *fm,
                        enum ofputil_protocol protocol)
{
    struct ofpbuf *msg;
    uint8_t ofp_version = ofputil_protocol_to_ofp_version(protocol);

    switch (protocol) {
    case OFPUTIL_P_OF12: {
        struct ofp11_flow_mod *ofm;

        msg = ofpbuf_new(sizeof *ofm + NXM_TYPICAL_LEN + fm->ofpacts_len);
        ofm = put_openflow(sizeof *ofm, ofp_version, OFPT11_FLOW_MOD, msg);
        ofm->cookie = fm->new_cookie;
        ofm->cookie_mask = fm->cookie_mask;
        ofm->table_id = fm->table_id;
        ofm->command = fm->command;
        ofm->idle_timeout = htons(fm->idle_timeout);
        ofm->hard_timeout = htons(fm->hard_timeout);
        ofm->priority = htons(fm->cr.priority);
        ofm->buffer_id = htonl(fm->buffer_id);
        ofm->out_port = ofputil_port_to_ofp11(fm->out_port);
        ofm->out_group = htonl(OFPG11_ANY);
        ofm->flags = htons(fm->flags);
        memset(ofm->pad, 0, sizeof ofm->pad);
        ofputil_put_match(msg, &fm->cr, fm->cookie, fm->cookie_mask, protocol);
        if (fm->ofpacts) {
            ofpacts_insts_to_openflow11(ofp_version, fm->ofpacts, msg);
        }
        break;
    }

    case OFPUTIL_P_OF10:
    case OFPUTIL_P_OF10_TID: {
        struct ofp10_flow_mod *ofm;

        msg = ofpbuf_new(sizeof *ofm + fm->ofpacts_len);
        ofm = put_openflow(sizeof *ofm, ofp_version, OFPT10_FLOW_MOD, msg);
        ofputil_cls_rule_to_ofp10_match(&fm->cr, &ofm->match);
        ofm->cookie = fm->new_cookie;
        ofm->command = ofputil_tid_command(fm, protocol);
        ofm->idle_timeout = htons(fm->idle_timeout);
        ofm->hard_timeout = htons(fm->hard_timeout);
        ofm->priority = htons(fm->cr.priority);
        ofm->buffer_id = htonl(fm->buffer_id);
        ofm->out_port = htons(fm->out_port);
        ofm->flags = htons(fm->flags);
        if (fm->ofpacts) {
            ofpacts_to_openflow10(fm->ofpacts, msg);
        }
        break;
    }

    case OFPUTIL_P_NXM:
    case OFPUTIL_P_NXM_TID: {
        struct nx_flow_mod *nfm;
        int match_len;

        msg = ofpbuf_new(sizeof *nfm + NXM_TYPICAL_LEN + fm->ofpacts_len);
        put_nxmsg(sizeof *nfm, NXT_FLOW_MOD, msg);
        nfm = msg->data;
        nfm->command = ofputil_tid_command(fm, protocol);
        nfm->cookie = fm->new_cookie;
        match_len = ofputil_put_match(msg, &fm->cr, fm->cookie,
                                      fm->cookie_mask, OFPUTIL_P_NXM);
        nfm->idle_timeout = htons(fm->idle_timeout);
        nfm->hard_timeout = htons(fm->hard_timeout);
        nfm->priority = htons(fm->cr.priority);
        nfm->buffer_id = htonl(fm->buffer_id);
        nfm->out_port = htons(fm->out_port);
        nfm->flags = htons(fm->flags);
        nfm->match_len = htons(match_len);
        if (fm->ofpacts) {
            ofpacts_to_openflow10(fm->ofpacts, msg);
        }
        break;
    }

    default:
        NOT_REACHED();
    }

    update_openflow_length(msg);
    return msg;
}

/* Returns a bitmask with a 1-bit for each protocol that could be used to
 * send all of the 'n_fm's flow table modification requests in 'fms', and a
 * 0-bit for each protocol that is inadequate.
 *
 * (The return value will have at least one 1-bit.) */
enum ofputil_protocol
ofputil_flow_mod_usable_protocols(const struct ofputil_flow_mod *fms,
                                  size_t n_fms)
{
    enum ofputil_protocol usable_protocols;
    size_t i;

    usable_protocols = OFPUTIL_P_ANY;
    for (i = 0; i < n_fms; i++) {
        const struct ofputil_flow_mod *fm = &fms[i];

        usable_protocols &= ofputil_usable_protocols(&fm->cr);
        if (fm->table_id != 0xff) {
            usable_protocols &= OFPUTIL_P_TID;
        }

        /* Matching of the cookie is only supported through NXM. */
        if (fm->cookie_mask != htonll(0)) {
            usable_protocols &= OFPUTIL_P_NXM_ANY;
        }

        usable_protocols |= OFPUTIL_P_OF12;
        usable_protocols &=
            ofputil_usable_protocols_with_actions(fm->ofpacts);
    }
    assert(usable_protocols);

    return usable_protocols;
}

static enum ofperr
ofputil_decode_ofpst_flow_request(struct ofputil_flow_stats_request *fsr,
                                  uint8_t ofp_version, struct ofpbuf *b,
                                  bool aggregate)
{
    fsr->aggregate = aggregate;

    if (ofp_version == OFP12_VERSION) {
        const struct ofp11_flow_stats_request *ofsr;
        enum ofperr error;

        ofsr = ofpbuf_pull(b, sizeof *ofsr);
        fsr->table_id = ofsr->table_id;
        error = ofputil_port_from_ofp11(ofsr->out_port, &fsr->out_port);
        if (error) {
            return error;
        }
        if (ofsr->out_group != htonl(OFPG11_ANY)) {
            return OFPERR_NXFMFC_GROUPS_NOT_SUPPORTED;
        }
        fsr->cookie = ofsr->cookie;
        fsr->cookie_mask = ofsr->cookie_mask;
        error = ofputil_pull_ofp12_match(b, 0, &fsr->match, NULL, NULL, NULL);
        if (error) {
            return error;
        }
    } else if (ofp_version == OFP10_VERSION) {
        const struct ofp10_flow_stats_request *ofsr = b->data;

        ofputil_cls_rule_from_ofp10_match(&ofsr->match, 0, &fsr->match);
        fsr->out_port = ntohs(ofsr->out_port);
        fsr->table_id = ofsr->table_id;
        fsr->cookie = fsr->cookie_mask = htonll(0);
    } else {
        NOT_REACHED();
    }

    return 0;
}

static enum ofperr
ofputil_decode_nxst_flow_request(struct ofputil_flow_stats_request *fsr,
                                 struct ofpbuf *b, bool aggregate)
{
    const struct nx_flow_stats_request *nfsr;
    enum ofperr error;

    nfsr = ofpbuf_pull(b, sizeof *nfsr);
    error = nx_pull_match(b, ntohs(nfsr->match_len), 0, 0, &fsr->match,
                          &fsr->cookie, &fsr->cookie_mask);
    if (error) {
        return error;
    }
    if (b->size) {
        return OFPERR_OFPBRC_BAD_LEN;
    }

    fsr->aggregate = aggregate;
    fsr->out_port = ntohs(nfsr->out_port);
    fsr->table_id = nfsr->table_id;

    return 0;
}

/* Converts an OFPST_FLOW, OFPST_AGGREGATE, NXST_FLOW, or NXST_AGGREGATE
 * request 'oh', into an abstract flow_stats_request in 'fsr'.  Returns 0 if
 * successful, otherwise an OpenFlow error code. */
enum ofperr
ofputil_decode_flow_stats_request(struct ofputil_flow_stats_request *fsr,
                                  const struct ofp_header *oh)
{
    const struct ofputil_msg_type *type;
    struct ofpbuf b;
    int code;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofputil_pull_stats_msg(&b);

    ofputil_decode_msg_type(oh, &type);
    code = ofputil_msg_type_code(type);
    switch (code) {
    case OFPUTIL_OFPST10_FLOW_REQUEST:
    case OFPUTIL_OFPST11_FLOW_REQUEST:
        return ofputil_decode_ofpst_flow_request(fsr, oh->version, &b, false);

    case OFPUTIL_OFPST10_AGGREGATE_REQUEST:
    case OFPUTIL_OFPST11_AGGREGATE_REQUEST:
        return ofputil_decode_ofpst_flow_request(fsr, oh->version, &b, true);

    case OFPUTIL_NXST_FLOW_REQUEST:
        return ofputil_decode_nxst_flow_request(fsr, &b, false);

    case OFPUTIL_NXST_AGGREGATE_REQUEST:
        return ofputil_decode_nxst_flow_request(fsr, &b, true);

    default:
        /* Hey, the caller lied. */
        NOT_REACHED();
    }
}

/* Converts abstract flow_stats_request 'fsr' into an OFPST_FLOW,
 * OFPST_AGGREGATE, NXST_FLOW, or NXST_AGGREGATE request 'oh' according to
 * 'protocol', and returns the message. */
struct ofpbuf *
ofputil_encode_flow_stats_request(const struct ofputil_flow_stats_request *fsr,
                                  enum ofputil_protocol protocol)
{
    struct ofpbuf *msg;
    uint8_t ofp_version = ofputil_protocol_to_ofp_version(protocol);

    switch (protocol) {
    case OFPUTIL_P_OF12: {
        struct ofp11_flow_stats_request *ofsr;
        int type;

        type = fsr->aggregate ? OFPST_AGGREGATE : OFPST_FLOW;
        ofsr = ofputil_make_stats_request(sizeof *ofsr, ofp_version,
                                          type, 0, &msg);
        ofsr->table_id = fsr->table_id;
        memset(ofsr->pad, 0, sizeof ofsr->pad);
        ofsr->out_port = ofputil_port_to_ofp11(fsr->out_port);
        ofsr->out_group = htonl(OFPG11_ANY);
        memset(ofsr->pad2, 0, sizeof ofsr->pad2);
        ofsr->cookie = fsr->cookie;
        ofsr->cookie_mask = fsr->cookie_mask;
        ofputil_put_match(msg, &fsr->match, fsr->cookie, fsr->cookie_mask,
                          protocol);
        break;
    }

    case OFPUTIL_P_OF10:
    case OFPUTIL_P_OF10_TID: {
        struct ofp10_flow_stats_request *ofsr;
        int type;

        type = fsr->aggregate ? OFPST_AGGREGATE : OFPST_FLOW;
        ofsr = ofputil_make_stats_request(sizeof *ofsr, ofp_version,
                                          type, 0, &msg);
        ofputil_cls_rule_to_ofp10_match(&fsr->match, &ofsr->match);
        ofsr->table_id = fsr->table_id;
        ofsr->out_port = htons(fsr->out_port);
        break;
    }

    case OFPUTIL_P_NXM:
    case OFPUTIL_P_NXM_TID: {
        struct nx_flow_stats_request *nfsr;
        int match_len;
        int subtype;

        subtype = fsr->aggregate ? NXST_AGGREGATE : NXST_FLOW;
        ofputil_make_stats_request(sizeof *nfsr, ofp_version,
                                   OFPST_VENDOR, subtype, &msg);
        match_len = ofputil_put_match(msg, &fsr->match, fsr->cookie,
                                      fsr->cookie_mask, OFPUTIL_P_NXM);

        nfsr = ofputil_stats_msg_body(msg->data);
        nfsr->out_port = htons(fsr->out_port);
        nfsr->match_len = htons(match_len);
        nfsr->table_id = fsr->table_id;
        break;
    }

    default:
        NOT_REACHED();
    }

    return msg;
}

/* Returns a bitmask with a 1-bit for each protocol that could be used to
 * accurately encode 'fsr', and a 0-bit for each protocol that is inadequate.
 *
 * (The return value will have at least one 1-bit.) */
enum ofputil_protocol
ofputil_flow_stats_request_usable_protocols(
    const struct ofputil_flow_stats_request *fsr)
{
    enum ofputil_protocol usable_protocols;

    usable_protocols = ofputil_usable_protocols(&fsr->match);
    if (fsr->cookie_mask != htonll(0)) {
        usable_protocols &= OFPUTIL_P_NXM_ANY;
    }
    return usable_protocols;
}

/* Converts an OFPST_FLOW or NXST_FLOW reply in 'msg' into an abstract
 * ofputil_flow_stats in 'fs'.
 *
 * Multiple OFPST_FLOW or NXST_FLOW replies can be packed into a single
 * OpenFlow message.  Calling this function multiple times for a single 'msg'
 * iterates through the replies.  The caller must initially leave 'msg''s layer
 * pointers null and not modify them between calls.
 *
 * Most switches don't send the values needed to populate fs->idle_age and
 * fs->hard_age, so those members will usually be set to 0.  If the switch from
 * which 'msg' originated is known to implement NXT_FLOW_AGE, then pass
 * 'flow_age_extension' as true so that the contents of 'msg' determine the
 * 'idle_age' and 'hard_age' members in 'fs'.
 *
 * Uses 'ofpacts' to store the abstract OFPACT_* version of the flow stats
 * reply's actions.  The caller must initialize 'ofpacts' and retains ownership
 * of it.  'fs->ofpacts' will point into the 'ofpacts' buffer.
 *
 * Returns 0 if successful, EOF if no replies were left in this 'msg',
 * otherwise a positive errno value. */
int
ofputil_decode_flow_stats_reply(struct ofputil_flow_stats *fs,
                                struct ofpbuf *msg,
                                bool flow_age_extension,
                                struct ofpbuf *ofpacts)
{
    const struct ofputil_msg_type *type;
    const struct ofp_header *oh = msg->l2 ? msg->l2 : msg->data;
    int code;

    ofputil_decode_msg_type(oh, &type);
    code = ofputil_msg_type_code(type);
    if (!msg->l2) {
        msg->l2 = msg->data;
        ofputil_pull_stats_msg(msg);
    }

    if (!msg->size) {
        return EOF;
    } else if (code == OFPUTIL_OFPST11_FLOW_REPLY) {
        const struct ofp11_flow_stats *ofs;
        size_t length;
        uint16_t padded_match_len;

        ofs = ofpbuf_try_pull(msg, sizeof *ofs);
        if (!ofs) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "OFPST_FLOW reply has %zu leftover "
                         "bytes at end", msg->size);
            return EINVAL;
        }

        length = ntohs(ofs->length);
        if (length < sizeof *ofs) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "OFPST_FLOW reply claims invalid "
                         "length %zu", length);
            return EINVAL;
        }

        if (ofputil_pull_ofp12_match(msg, ntohs(ofs->priority), &fs->rule,
                                     NULL, NULL, &padded_match_len)) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "OFPST_FLOW reply bad match");
            return EINVAL;
        }

        if (ofpacts_pull_openflow11_instructions(oh->version, msg,
                                                 length - sizeof *ofs -
                                                 padded_match_len, ofpacts)) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "OFPST_FLOW reply bad instructions");
            return EINVAL;
        }

        fs->table_id = ofs->table_id;
        fs->duration_sec = ntohl(ofs->duration_sec);
        fs->duration_nsec = ntohl(ofs->duration_nsec);
        fs->idle_timeout = ntohs(ofs->idle_timeout);
        fs->hard_timeout = ntohs(ofs->hard_timeout);
        fs->idle_age = -1;
        fs->hard_age = -1;
        fs->cookie = ofs->cookie;
        fs->packet_count = ntohll(ofs->packet_count);
        fs->byte_count = ntohll(ofs->byte_count);
    } else if (code == OFPUTIL_OFPST10_FLOW_REPLY) {
        const struct ofp10_flow_stats *ofs;
        size_t length;

        ofs = ofpbuf_try_pull(msg, sizeof *ofs);
        if (!ofs) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "OFPST_FLOW reply has %zu leftover "
                         "bytes at end", msg->size);
            return EINVAL;
        }

        length = ntohs(ofs->length);
        if (length < sizeof *ofs) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "OFPST_FLOW reply claims invalid "
                         "length %zu", length);
            return EINVAL;
        }

        if (ofpacts_pull_openflow10(msg, length - sizeof *ofs, ofpacts)) {
            return EINVAL;
        }

        fs->cookie = get_32aligned_be64(&ofs->cookie);
        ofputil_cls_rule_from_ofp10_match(&ofs->match, ntohs(ofs->priority),
                                          &fs->rule);
        fs->table_id = ofs->table_id;
        fs->duration_sec = ntohl(ofs->duration_sec);
        fs->duration_nsec = ntohl(ofs->duration_nsec);
        fs->idle_timeout = ntohs(ofs->idle_timeout);
        fs->hard_timeout = ntohs(ofs->hard_timeout);
        fs->idle_age = -1;
        fs->hard_age = -1;
        fs->packet_count = ntohll(get_32aligned_be64(&ofs->packet_count));
        fs->byte_count = ntohll(get_32aligned_be64(&ofs->byte_count));
    } else if (code == OFPUTIL_NXST_FLOW_REPLY) {
        const struct nx_flow_stats *nfs;
        size_t match_len, actions_len, length;

        nfs = ofpbuf_try_pull(msg, sizeof *nfs);
        if (!nfs) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "NXST_FLOW reply has %zu leftover "
                         "bytes at end", msg->size);
            return EINVAL;
        }

        length = ntohs(nfs->length);
        match_len = ntohs(nfs->match_len);
        if (length < sizeof *nfs + ROUND_UP(match_len, 8)) {
            VLOG_WARN_RL(&bad_ofmsg_rl, "NXST_FLOW reply with match_len=%zu "
                         "claims invalid length %zu", match_len, length);
            return EINVAL;
        }
        if (nx_pull_match(msg, match_len, 0, ntohs(nfs->priority), &fs->rule,
                          NULL, NULL)) {
            return EINVAL;
        }

        actions_len = length - sizeof *nfs - ROUND_UP(match_len, 8);
        if (ofpacts_pull_openflow10(msg, actions_len, ofpacts)) {
            return EINVAL;
        }

        fs->cookie = nfs->cookie;
        fs->table_id = nfs->table_id;
        fs->duration_sec = ntohl(nfs->duration_sec);
        fs->duration_nsec = ntohl(nfs->duration_nsec);
        fs->idle_timeout = ntohs(nfs->idle_timeout);
        fs->hard_timeout = ntohs(nfs->hard_timeout);
        fs->idle_age = -1;
        fs->hard_age = -1;
        if (flow_age_extension) {
            if (nfs->idle_age) {
                fs->idle_age = ntohs(nfs->idle_age) - 1;
            }
            if (nfs->hard_age) {
                fs->hard_age = ntohs(nfs->hard_age) - 1;
            }
        }
        fs->packet_count = ntohll(nfs->packet_count);
        fs->byte_count = ntohll(nfs->byte_count);
    } else {
        NOT_REACHED();
    }

    fs->ofpacts = ofpacts->data;
    fs->ofpacts_len = ofpacts->size;

    return 0;
}

/* Returns 'count' unchanged except that UINT64_MAX becomes 0.
 *
 * We use this in situations where OVS internally uses UINT64_MAX to mean
 * "value unknown" but OpenFlow 1.0 does not define any unknown value. */
static uint64_t
unknown_to_zero(uint64_t count)
{
    return count != UINT64_MAX ? count : 0;
}

/* Appends an OFPST_FLOW or NXST_FLOW reply that contains the data in 'fs' to
 * those already present in the list of ofpbufs in 'replies'.  'replies' should
 * have been initialized with ofputil_start_stats_reply(). */
void
ofputil_append_flow_stats_reply(uint8_t ofp_version,
                                const struct ofputil_flow_stats *fs,
                                struct list *replies)
{
    struct ofpbuf *reply = ofpbuf_from_list(list_back(replies));
    const struct ofp10_stats_msg *osm = reply->data;
    size_t start_ofs = reply->size;

    if (osm->type == htons(OFPST_FLOW) && ofp_version == OFP12_VERSION) {
        struct ofp11_flow_stats *ofs;

        ofs = ofpbuf_put_uninit(reply, sizeof *ofs);
        ofs->table_id = fs->table_id;
        ofs->pad = 0;
        ofs->duration_sec = htonl(fs->duration_sec);
        ofs->duration_nsec = htonl(fs->duration_nsec);
        ofs->priority = htons(fs->rule.priority);
        ofs->idle_timeout = htons(fs->idle_timeout);
        ofs->hard_timeout = htons(fs->hard_timeout);
        memset(ofs->pad2, 0, sizeof ofs->pad2);
        ofs->cookie = fs->cookie;
        ofs->packet_count = htonll(unknown_to_zero(fs->packet_count));
        ofs->byte_count = htonll(unknown_to_zero(fs->byte_count));
        ofputil_put_match(reply, &fs->rule, 0, 0, OFPUTIL_P_OF12);
        ofpacts_insts_to_openflow11(ofp_version, fs->ofpacts, reply);
        ofs = ofpbuf_at_assert(reply, start_ofs, sizeof *ofs);
        ofs->length = htons(reply->size - start_ofs);
    } else if (osm->type == htons(OFPST_FLOW) && ofp_version == OFP10_VERSION) {
        struct ofp10_flow_stats *ofs;

        ofs = ofpbuf_put_uninit(reply, sizeof *ofs);
        ofs->table_id = fs->table_id;
        ofs->pad = 0;
        ofputil_cls_rule_to_ofp10_match(&fs->rule, &ofs->match);
        ofs->duration_sec = htonl(fs->duration_sec);
        ofs->duration_nsec = htonl(fs->duration_nsec);
        ofs->priority = htons(fs->rule.priority);
        ofs->idle_timeout = htons(fs->idle_timeout);
        ofs->hard_timeout = htons(fs->hard_timeout);
        memset(ofs->pad2, 0, sizeof ofs->pad2);
        put_32aligned_be64(&ofs->cookie, fs->cookie);
        put_32aligned_be64(&ofs->packet_count,
                           htonll(unknown_to_zero(fs->packet_count)));
        put_32aligned_be64(&ofs->byte_count,
                           htonll(unknown_to_zero(fs->byte_count)));
        ofpacts_to_openflow10(fs->ofpacts, reply);

        ofs = ofpbuf_at_assert(reply, start_ofs, sizeof *ofs);
        ofs->length = htons(reply->size - start_ofs);
    } else if (osm->type == htons(OFPST_VENDOR)) {
        struct nx_flow_stats *nfs;

        nfs = ofpbuf_put_uninit(reply, sizeof *nfs);
        nfs->table_id = fs->table_id;
        nfs->pad = 0;
        nfs->duration_sec = htonl(fs->duration_sec);
        nfs->duration_nsec = htonl(fs->duration_nsec);
        nfs->priority = htons(fs->rule.priority);
        nfs->idle_timeout = htons(fs->idle_timeout);
        nfs->hard_timeout = htons(fs->hard_timeout);
        nfs->idle_age = htons(fs->idle_age < 0 ? 0
                              : fs->idle_age < UINT16_MAX ? fs->idle_age + 1
                              : UINT16_MAX);
        nfs->hard_age = htons(fs->hard_age < 0 ? 0
                              : fs->hard_age < UINT16_MAX ? fs->hard_age + 1
                              : UINT16_MAX);
        nfs->match_len = htons(ofputil_put_match(reply, &fs->rule,
                                                 0, 0, OFPUTIL_P_NXM));
        nfs->cookie = fs->cookie;
        nfs->packet_count = htonll(fs->packet_count);
        nfs->byte_count = htonll(fs->byte_count);
        ofpacts_to_openflow10(fs->ofpacts, reply);

        nfs = ofpbuf_at_assert(reply, start_ofs, sizeof *nfs);
        nfs->length = htons(reply->size - start_ofs);
    } else {
        NOT_REACHED();
    }

    ofputil_postappend_stats_reply(start_ofs, replies);
}

static void
ofputil_encode_aggregate_stats_reply__(
    const struct ofputil_aggregate_stats *stats,
    const struct ofp_header *request, struct ofpbuf **msg)
{
    struct ofp11_aggregate_stats_reply *asr;

    asr = ofputil_make_stats_reply(sizeof *asr, request, msg);
    asr->packet_count = htonll(unknown_to_zero(stats->packet_count));
    asr->byte_count = htonll(unknown_to_zero(stats->byte_count));
    asr->flow_count = htonl(stats->flow_count);
}

/* Converts abstract ofputil_aggregate_stats 'stats' into an OFPST_AGGREGATE or
 * NXST_AGGREGATE reply according to 'protocol', and returns the message. */
struct ofpbuf *
ofputil_encode_aggregate_stats_reply(
    const struct ofputil_aggregate_stats *stats,
    const struct ofp_header *request)
{
    const struct ofputil_msg_type *type;
    enum ofputil_msg_code code;
    struct ofpbuf *msg;

    ofputil_decode_msg_type(request, &type);
    code = ofputil_msg_type_code(type);
    if (code == OFPUTIL_OFPST11_AGGREGATE_REQUEST ||
        code == OFPUTIL_NXST_AGGREGATE_REQUEST) {
        ofputil_encode_aggregate_stats_reply__(stats, request, &msg);
    } else if (code == OFPUTIL_OFPST10_AGGREGATE_REQUEST) {
        struct ofp10_aggregate_stats_reply *asr;

        asr = ofputil_make_stats_reply(sizeof *asr, request, &msg);
        put_32aligned_be64(&asr->packet_count,
                           htonll(unknown_to_zero(stats->packet_count)));
        put_32aligned_be64(&asr->byte_count,
                           htonll(unknown_to_zero(stats->byte_count)));
        asr->flow_count = htonl(stats->flow_count);
    } else {
        NOT_REACHED();
    }

    return msg;
}

/* Converts an OFPT_FLOW_REMOVED or NXT_FLOW_REMOVED message 'oh' into an
 * abstract ofputil_flow_removed in 'fr'.  Returns 0 if successful, otherwise
 * an OpenFlow error code. */
enum ofperr
ofputil_decode_flow_removed(struct ofputil_flow_removed *fr,
                            const struct ofp_header *oh)
{
    const struct ofputil_msg_type *type;
    enum ofputil_msg_code code;

    ofputil_decode_msg_type(oh, &type);
    code = ofputil_msg_type_code(type);
    if (code == OFPUTIL_OFPT_FLOW_REMOVED && oh->version == OFP12_VERSION) {
        const struct ofp12_flow_removed *ofr;
        struct ofpbuf b;
        int error;

        ofpbuf_use_const(&b, oh, ntohs(oh->length));

        ofr = ofpbuf_pull(&b, sizeof *ofr);
        error = ofputil_pull_ofp12_match(&b, ntohs(ofr->priority),
                                         &fr->rule, NULL, NULL, NULL);
        if (error) {
            return error;
        }

        fr->cookie = ofr->cookie;
        fr->reason = ofr->reason;
        /* FIXMIE: table_id is ignored */
        fr->duration_sec = ntohl(ofr->duration_sec);
        fr->duration_nsec = ntohl(ofr->duration_nsec);
        fr->idle_timeout = ntohs(ofr->idle_timeout);
        /* FIXMIE: hard_timeout is ignored */
        fr->packet_count = ntohll(ofr->packet_count);
        fr->byte_count = ntohll(ofr->byte_count);
    } else if (code == OFPUTIL_OFPT_FLOW_REMOVED &&
               oh->version == OFP10_VERSION) {
        const struct ofp_flow_removed *ofr;

        ofr = (const struct ofp_flow_removed *) oh;
        ofputil_cls_rule_from_ofp10_match(&ofr->match, ntohs(ofr->priority),
                                          &fr->rule);
        fr->cookie = ofr->cookie;
        fr->reason = ofr->reason;
        fr->duration_sec = ntohl(ofr->duration_sec);
        fr->duration_nsec = ntohl(ofr->duration_nsec);
        fr->idle_timeout = ntohs(ofr->idle_timeout);
        fr->packet_count = ntohll(ofr->packet_count);
        fr->byte_count = ntohll(ofr->byte_count);
    } else if (code == OFPUTIL_NXT_FLOW_REMOVED) {
        struct nx_flow_removed *nfr;
        struct ofpbuf b;
        int error;

        ofpbuf_use_const(&b, oh, ntohs(oh->length));

        nfr = ofpbuf_pull(&b, sizeof *nfr);
        error = nx_pull_match(&b, ntohs(nfr->match_len), 0,
                              ntohs(nfr->priority), &fr->rule, NULL, NULL);
        if (error) {
            return error;
        }
        if (b.size) {
            return OFPERR_OFPBRC_BAD_LEN;
        }

        fr->cookie = nfr->cookie;
        fr->reason = nfr->reason;
        fr->duration_sec = ntohl(nfr->duration_sec);
        fr->duration_nsec = ntohl(nfr->duration_nsec);
        fr->idle_timeout = ntohs(nfr->idle_timeout);
        fr->packet_count = ntohll(nfr->packet_count);
        fr->byte_count = ntohll(nfr->byte_count);
    } else {
        NOT_REACHED();
    }

    return 0;
}

/* Converts abstract ofputil_flow_removed 'fr' into an OFPT_FLOW_REMOVED or
 * NXT_FLOW_REMOVED message 'oh' according to 'protocol', and returns the
 * message. */
struct ofpbuf *
ofputil_encode_flow_removed(const struct ofputil_flow_removed *fr,
                            enum ofputil_protocol protocol)
{
    struct ofpbuf *msg;
    uint8_t ofp_version = ofputil_protocol_to_ofp_version(protocol);

    switch (protocol) {
    case OFPUTIL_P_OF12: {
        struct ofp12_flow_removed *ofr;

        ofr = make_openflow_xid(sizeof *ofr, ofp_version,
                                OFPT_FLOW_REMOVED, 0, &msg);
        ofr->cookie = fr->cookie;
        ofr->priority = htons(fr->rule.priority);
        ofr->reason = fr->reason;
        ofr->table_id = 0;
        ofr->duration_sec = htonl(fr->duration_sec);
        ofr->duration_nsec = htonl(fr->duration_nsec);
        ofr->idle_timeout = htons(fr->idle_timeout);
        ofr->packet_count = htonll(fr->packet_count);
        ofr->byte_count = htonll(fr->byte_count);
        ofputil_put_match(msg, &fr->rule, 0, 0, protocol);
        break;
    }

    case OFPUTIL_P_OF10:
    case OFPUTIL_P_OF10_TID: {
        struct ofp_flow_removed *ofr;

        ofr = make_openflow_xid(sizeof *ofr, ofp_version,
                                OFPT_FLOW_REMOVED, htonl(0), &msg);
        ofputil_cls_rule_to_ofp10_match(&fr->rule, &ofr->match);
        ofr->cookie = fr->cookie;
        ofr->priority = htons(fr->rule.priority);
        ofr->reason = fr->reason;
        ofr->duration_sec = htonl(fr->duration_sec);
        ofr->duration_nsec = htonl(fr->duration_nsec);
        ofr->idle_timeout = htons(fr->idle_timeout);
        ofr->packet_count = htonll(unknown_to_zero(fr->packet_count));
        ofr->byte_count = htonll(unknown_to_zero(fr->byte_count));
        break;
    }

    case OFPUTIL_P_NXM:
    case OFPUTIL_P_NXM_TID: {
        struct nx_flow_removed *nfr;
        int match_len;

        make_nxmsg_xid(sizeof *nfr, NXT_FLOW_REMOVED, htonl(0), &msg);
        match_len = nx_put_match(msg, false, &fr->rule, 0, 0);

        nfr = msg->data;
        nfr->cookie = fr->cookie;
        nfr->priority = htons(fr->rule.priority);
        nfr->reason = fr->reason;
        nfr->duration_sec = htonl(fr->duration_sec);
        nfr->duration_nsec = htonl(fr->duration_nsec);
        nfr->idle_timeout = htons(fr->idle_timeout);
        nfr->match_len = htons(match_len);
        nfr->packet_count = htonll(fr->packet_count);
        nfr->byte_count = htonll(fr->byte_count);
        break;
    }

    default:
        NOT_REACHED();
    }

    return msg;
}

static void
ofputil_decode_packet_in_finish(struct ofputil_packet_in *pin,
                                struct cls_rule *rule,
                                struct ofpbuf *b)
{
    pin->packet = b->data;
    pin->packet_len = b->size;

    pin->fmd.in_port = rule->flow.in_port;

    pin->fmd.tun_id = rule->flow.tun_id;
    pin->fmd.tun_id_mask = rule->wc.tun_id_mask;

    memcpy(pin->fmd.regs, rule->flow.regs, sizeof pin->fmd.regs);
    memcpy(pin->fmd.reg_masks, rule->wc.reg_masks,
           sizeof pin->fmd.reg_masks);
}

enum ofperr
ofputil_decode_packet_in(struct ofputil_packet_in *pin,
                         const struct ofp_header *oh)
{
    const struct ofputil_msg_type *type;
    enum ofputil_msg_code code;

    ofputil_decode_msg_type(oh, &type);
    code = ofputil_msg_type_code(type);
    memset(pin, 0, sizeof *pin);

    if (code == OFPUTIL_OFPT_PACKET_IN && oh->version == OFP12_VERSION) {
        const struct ofp11_packet_in *opi;
        struct cls_rule rule;
        struct ofpbuf b;
        int error;

        ofpbuf_use_const(&b, oh, ntohs(oh->length));

        opi = ofpbuf_pull(&b, sizeof *opi);
        error = ofputil_pull_ofp12_match(&b, 0, &rule, NULL, NULL, NULL);
        if (error) {
            return error;
        }

        if (!ofpbuf_try_pull(&b, 2)) {
            return OFPERR_OFPBRC_BAD_LEN;
        }

        pin->reason = opi->reason;
        pin->table_id = opi->table_id;

        pin->buffer_id = ntohl(opi->buffer_id);
        pin->total_len = ntohs(opi->total_len);

        ofputil_decode_packet_in_finish(pin, &rule, &b);
    } else if (code == OFPUTIL_OFPT_PACKET_IN && oh->version == OFP10_VERSION) {
        const struct ofp_packet_in *opi = (const struct ofp_packet_in *) oh;

        pin->packet = opi->data;
        pin->packet_len = ntohs(opi->header.length)
            - offsetof(struct ofp_packet_in, data);

        pin->fmd.in_port = ntohs(opi->in_port);
        pin->reason = opi->reason;
        pin->buffer_id = ntohl(opi->buffer_id);
        pin->total_len = ntohs(opi->total_len);
    } else if (code == OFPUTIL_NXT_PACKET_IN) {
        const struct nx_packet_in *npi;
        struct cls_rule rule;
        struct ofpbuf b;
        int error;

        ofpbuf_use_const(&b, oh, ntohs(oh->length));

        npi = ofpbuf_pull(&b, sizeof *npi);
        error = nx_pull_match_loose(&b, ntohs(npi->match_len), 0, 0,
                                    &rule, NULL, NULL);
        if (error) {
            return error;
        }

        if (!ofpbuf_try_pull(&b, 2)) {
            return OFPERR_OFPBRC_BAD_LEN;
        }

        pin->reason = npi->reason;
        pin->table_id = npi->table_id;
        pin->cookie = npi->cookie;

        pin->buffer_id = ntohl(npi->buffer_id);
        pin->total_len = ntohs(npi->total_len);

        ofputil_decode_packet_in_finish(pin, &rule, &b);
    } else {
        NOT_REACHED();
    }

    return 0;
}

static size_t
ofputil_encode_packet_in_tail(const struct ofputil_packet_in *pin,
                              struct ofpbuf **packet, size_t hdr_len,
                              enum ofputil_protocol protocol)
{
    size_t send_len = MIN(pin->send_len, pin->packet_len);
    size_t match_len = 0, i;
    struct cls_rule rule;

    /* Estimate of required PACKET_IN length includes the
     * head portion of the packet in message, space for the match (2 times
     * sizeof the metadata seems like enough), 2 bytes for padding, and the
     * packet length. */
    *packet = ofpbuf_new(hdr_len + sizeof(struct flow_metadata) * 2 +
                        2 + send_len);

    cls_rule_init_catchall(&rule, 0);
    cls_rule_set_tun_id_masked(&rule, pin->fmd.tun_id,
                               pin->fmd.tun_id_mask);

    for (i = 0; i < FLOW_N_REGS; i++) {
        cls_rule_set_reg_masked(&rule, i, pin->fmd.regs[i],
                                pin->fmd.reg_masks[i]);
    }

    cls_rule_set_in_port(&rule, pin->fmd.in_port);

    ofpbuf_put_zeros(*packet, hdr_len);
    match_len = ofputil_put_match(*packet, &rule, 0, 0, protocol);
    ofpbuf_put_zeros(*packet, 2);
    ofpbuf_put(*packet, pin->packet, send_len);

    return match_len;
}

/* Converts abstract ofputil_packet_in 'pin' into a PACKET_IN message
 * in the format specified by 'packet_in_format'.  */
struct ofpbuf *
ofputil_encode_packet_in(const struct ofputil_packet_in *pin,
                         enum ofputil_protocol protocol,
                         enum nx_packet_in_format packet_in_format)
{
    size_t send_len = MIN(pin->send_len, pin->packet_len);
    struct ofpbuf *packet;

    /* Add OFPT_PACKET_IN. */
    if (protocol == OFPUTIL_P_OF12) {
        struct ofp11_packet_in *opi;

        ofputil_encode_packet_in_tail(pin, &packet, sizeof *opi,
                                      OFPUTIL_P_OF12);

        opi = packet->data;
        opi->header.version = OFP12_VERSION;
        opi->header.type = OFPT_PACKET_IN;
        opi->buffer_id = htonl(pin->buffer_id);
        opi->total_len = htons(pin->total_len);
        opi->reason = pin->reason;
        opi->table_id = pin->table_id;
   } else if (packet_in_format == NXPIF_OPENFLOW10) {
        size_t header_len = offsetof(struct ofp_packet_in, data);
        struct ofp_packet_in *opi;

        packet = ofpbuf_new(send_len + header_len);
        opi = ofpbuf_put_zeros(packet, header_len);
        opi->header.version = OFP10_VERSION;
        opi->header.type = OFPT_PACKET_IN;
        opi->total_len = htons(pin->total_len);
        opi->in_port = htons(pin->fmd.in_port);
        opi->reason = pin->reason;
        opi->buffer_id = htonl(pin->buffer_id);

        ofpbuf_put(packet, pin->packet, send_len);
    } else if (packet_in_format == NXPIF_NXM) {
        struct nx_packet_in *npi;
        size_t match_len;

        match_len = ofputil_encode_packet_in_tail(pin, &packet, sizeof *npi,
                                                  OFPUTIL_P_NXM);

        npi = packet->data;
        npi->nxh.header.version = OFP10_VERSION;
        npi->nxh.header.type = OFPT_VENDOR;
        npi->nxh.vendor = htonl(NX_VENDOR_ID);
        npi->nxh.subtype = htonl(NXT_PACKET_IN);

        npi->buffer_id = htonl(pin->buffer_id);
        npi->total_len = htons(pin->total_len);
        npi->reason = pin->reason;
        npi->table_id = pin->table_id;
        npi->cookie = pin->cookie;
        npi->match_len = htons(match_len);
    } else {
        NOT_REACHED();
    }
    update_openflow_length(packet);

    return packet;
}

const char *
ofputil_packet_in_reason_to_string(enum ofp_packet_in_reason reason)
{
    static char s[INT_STRLEN(int) + 1];

    switch (reason) {
    case OFPR_NO_MATCH:
        return "no_match";
    case OFPR_ACTION:
        return "action";
    case OFPR_INVALID_TTL:
        return "invalid_ttl";

    case OFPR_N_REASONS:
    default:
        sprintf(s, "%d", (int) reason);
        return s;
    }
}

bool
ofputil_packet_in_reason_from_string(const char *s,
                                     enum ofp_packet_in_reason *reason)
{
    int i;

    for (i = 0; i < OFPR_N_REASONS; i++) {
        if (!strcasecmp(s, ofputil_packet_in_reason_to_string(i))) {
            *reason = i;
            return true;
        }
    }
    return false;
}

/* Converts an OFPT_PACKET_OUT in 'opo' into an abstract ofputil_packet_out in
 * 'po'.
 *
 * Uses 'ofpacts' to store the abstract OFPACT_* version of the packet out
 * message's actions.  The caller must initialize 'ofpacts' and retains
 * ownership of it.  'po->ofpacts' will point into the 'ofpacts' buffer.
 *
 * Returns 0 if successful, otherwise an OFPERR_* value. */
enum ofperr
ofputil_decode_packet_out(struct ofputil_packet_out *po,
                          const struct ofp_header *oh,
                          struct ofpbuf *ofpacts)
{
    struct ofpbuf b;

    memset(po, 0, sizeof *po);

    ofpbuf_use_const(&b, oh, ntohs(oh->length));

    if (oh->version == OFP11_VERSION || oh->version == OFP12_VERSION) {
        const struct ofp11_packet_out *opo = (const struct ofp11_packet_out *) oh;
        enum ofperr error;

        ofpbuf_pull(&b, sizeof *opo);

        po->buffer_id = ntohl(opo->buffer_id);
        error = ofputil_port_from_ofp11(opo->in_port, &po->in_port);
        if (error) {
            return error;
        }

        error = ofpacts_pull_openflow11_instructions(oh->version, &b,
                                                     ntohs(opo->actions_len),
                                                     ofpacts);
        if (error) {
            return error;
        }
    } else if (oh->version == OFP10_VERSION) {
        const struct ofp_packet_out *opo = (const struct ofp_packet_out *) oh;
        enum ofperr error;

        ofpbuf_pull(&b, sizeof *opo);

        po->buffer_id = ntohl(opo->buffer_id);
        po->in_port = ntohs(opo->in_port);

        error = ofpacts_pull_openflow10(&b, ntohs(opo->actions_len), ofpacts);
        if (error) {
            return error;
        }
    } else {
        NOT_REACHED();
    }

    if (po->in_port >= OFPP_MAX && po->in_port != OFPP_LOCAL
        && po->in_port != OFPP_NONE && po->in_port != OFPP_CONTROLLER) {
        VLOG_WARN_RL(&bad_ofmsg_rl, "packet-out has bad input port %#"PRIx16,
                    po->in_port);
        return OFPERR_NXBRC_BAD_IN_PORT;
    }

    po->ofpacts = ofpacts->data;
    po->ofpacts_len = ofpacts->size;

    if (po->buffer_id == UINT32_MAX) {
        po->packet = b.data;
        po->packet_len = b.size;
    } else {
        po->packet = NULL;
        po->packet_len = 0;
    }

    return 0;
}

/* ofputil_phy_port */

/* NETDEV_F_* to and from OFPPF_* and OFPPF10_*. */
BUILD_ASSERT_DECL((int) NETDEV_F_10MB_HD    == OFPPF_10MB_HD);  /* bit 0 */
BUILD_ASSERT_DECL((int) NETDEV_F_10MB_FD    == OFPPF_10MB_FD);  /* bit 1 */
BUILD_ASSERT_DECL((int) NETDEV_F_100MB_HD   == OFPPF_100MB_HD); /* bit 2 */
BUILD_ASSERT_DECL((int) NETDEV_F_100MB_FD   == OFPPF_100MB_FD); /* bit 3 */
BUILD_ASSERT_DECL((int) NETDEV_F_1GB_HD     == OFPPF_1GB_HD);   /* bit 4 */
BUILD_ASSERT_DECL((int) NETDEV_F_1GB_FD     == OFPPF_1GB_FD);   /* bit 5 */
BUILD_ASSERT_DECL((int) NETDEV_F_10GB_FD    == OFPPF_10GB_FD);  /* bit 6 */

/* NETDEV_F_ bits 11...15 are OFPPF10_ bits 7...11: */
BUILD_ASSERT_DECL((int) NETDEV_F_COPPER == (OFPPF10_COPPER << 4));
BUILD_ASSERT_DECL((int) NETDEV_F_FIBER == (OFPPF10_FIBER << 4));
BUILD_ASSERT_DECL((int) NETDEV_F_AUTONEG == (OFPPF10_AUTONEG << 4));
BUILD_ASSERT_DECL((int) NETDEV_F_PAUSE == (OFPPF10_PAUSE << 4));
BUILD_ASSERT_DECL((int) NETDEV_F_PAUSE_ASYM == (OFPPF10_PAUSE_ASYM << 4));

static enum netdev_features
netdev_port_features_from_ofp10(ovs_be32 ofp10_)
{
    uint32_t ofp10 = ntohl(ofp10_);
    return (ofp10 & 0x7f) | ((ofp10 & 0xf80) << 4);
}

static ovs_be32
netdev_port_features_to_ofp10(enum netdev_features features)
{
    return htonl((features & 0x7f) | ((features & 0xf800) >> 4));
}

BUILD_ASSERT_DECL((int) NETDEV_F_10MB_HD    == OFPPF_10MB_HD);     /* bit 0 */
BUILD_ASSERT_DECL((int) NETDEV_F_10MB_FD    == OFPPF_10MB_FD);     /* bit 1 */
BUILD_ASSERT_DECL((int) NETDEV_F_100MB_HD   == OFPPF_100MB_HD);    /* bit 2 */
BUILD_ASSERT_DECL((int) NETDEV_F_100MB_FD   == OFPPF_100MB_FD);    /* bit 3 */
BUILD_ASSERT_DECL((int) NETDEV_F_1GB_HD     == OFPPF_1GB_HD);      /* bit 4 */
BUILD_ASSERT_DECL((int) NETDEV_F_1GB_FD     == OFPPF_1GB_FD);      /* bit 5 */
BUILD_ASSERT_DECL((int) NETDEV_F_10GB_FD    == OFPPF_10GB_FD);     /* bit 6 */
BUILD_ASSERT_DECL((int) NETDEV_F_40GB_FD    == OFPPF11_40GB_FD);   /* bit 7 */
BUILD_ASSERT_DECL((int) NETDEV_F_100GB_FD   == OFPPF11_100GB_FD);  /* bit 8 */
BUILD_ASSERT_DECL((int) NETDEV_F_1TB_FD     == OFPPF11_1TB_FD);    /* bit 9 */
BUILD_ASSERT_DECL((int) NETDEV_F_OTHER      == OFPPF11_OTHER);     /* bit 10 */
BUILD_ASSERT_DECL((int) NETDEV_F_COPPER     == OFPPF11_COPPER);    /* bit 11 */
BUILD_ASSERT_DECL((int) NETDEV_F_FIBER      == OFPPF11_FIBER);     /* bit 12 */
BUILD_ASSERT_DECL((int) NETDEV_F_AUTONEG    == OFPPF11_AUTONEG);   /* bit 13 */
BUILD_ASSERT_DECL((int) NETDEV_F_PAUSE      == OFPPF11_PAUSE);     /* bit 14 */
BUILD_ASSERT_DECL((int) NETDEV_F_PAUSE_ASYM == OFPPF11_PAUSE_ASYM);/* bit 15 */

static enum netdev_features
netdev_port_features_from_ofp11(ovs_be32 ofp11)
{
    return ntohl(ofp11) & 0xffff;
}

static ovs_be32
netdev_port_features_to_ofp11(enum netdev_features features)
{
    return htonl(features & 0xffff);
}

static enum ofperr
ofputil_decode_ofp10_phy_port(struct ofputil_phy_port *pp,
                              const struct ofp10_phy_port *opp)
{
    memset(pp, 0, sizeof *pp);

    pp->port_no = ntohs(opp->port_no);
    memcpy(pp->hw_addr, opp->hw_addr, OFP_ETH_ALEN);
    ovs_strlcpy(pp->name, opp->name, OFP_MAX_PORT_NAME_LEN);

    pp->config = ntohl(opp->config) & OFPPC10_ALL;
    pp->state = ntohl(opp->state) & OFPPS10_ALL;

    pp->curr = netdev_port_features_from_ofp10(opp->curr);
    pp->advertised = netdev_port_features_from_ofp10(opp->advertised);
    pp->supported = netdev_port_features_from_ofp10(opp->supported);
    pp->peer = netdev_port_features_from_ofp10(opp->peer);

    pp->curr_speed = netdev_features_to_bps(pp->curr) / 1000;
    pp->max_speed = netdev_features_to_bps(pp->supported) / 1000;

    return 0;
}

static enum ofperr
ofputil_decode_ofp11_port(struct ofputil_phy_port *pp,
                          const struct ofp11_port *op)
{
    enum ofperr error;

    memset(pp, 0, sizeof *pp);

    error = ofputil_port_from_ofp11(op->port_no, &pp->port_no);
    if (error) {
        return error;
    }
    memcpy(pp->hw_addr, op->hw_addr, OFP_ETH_ALEN);
    ovs_strlcpy(pp->name, op->name, OFP_MAX_PORT_NAME_LEN);

    pp->config = ntohl(op->config) & OFPPC11_ALL;
    pp->state = ntohl(op->state) & OFPPC11_ALL;

    pp->curr = netdev_port_features_from_ofp11(op->curr);
    pp->advertised = netdev_port_features_from_ofp11(op->advertised);
    pp->supported = netdev_port_features_from_ofp11(op->supported);
    pp->peer = netdev_port_features_from_ofp11(op->peer);

    pp->curr_speed = ntohl(op->curr_speed);
    pp->max_speed = ntohl(op->max_speed);

    return 0;
}

static size_t
ofputil_get_phy_port_size(uint8_t ofp_version)
{
    return ofp_version == OFP10_VERSION ? sizeof(struct ofp10_phy_port)
                                        : sizeof(struct ofp11_port);
}

static void
ofputil_encode_ofp10_phy_port(const struct ofputil_phy_port *pp,
                              struct ofp10_phy_port *opp)
{
    memset(opp, 0, sizeof *opp);

    opp->port_no = htons(pp->port_no);
    memcpy(opp->hw_addr, pp->hw_addr, ETH_ADDR_LEN);
    ovs_strlcpy(opp->name, pp->name, OFP_MAX_PORT_NAME_LEN);

    opp->config = htonl(pp->config & OFPPC10_ALL);
    opp->state = htonl(pp->state & OFPPS10_ALL);

    opp->curr = netdev_port_features_to_ofp10(pp->curr);
    opp->advertised = netdev_port_features_to_ofp10(pp->advertised);
    opp->supported = netdev_port_features_to_ofp10(pp->supported);
    opp->peer = netdev_port_features_to_ofp10(pp->peer);
}

static void
ofputil_encode_ofp11_port(const struct ofputil_phy_port *pp,
                          struct ofp11_port *op)
{
    memset(op, 0, sizeof *op);

    op->port_no = ofputil_port_to_ofp11(pp->port_no);
    memcpy(op->hw_addr, pp->hw_addr, ETH_ADDR_LEN);
    ovs_strlcpy(op->name, pp->name, OFP_MAX_PORT_NAME_LEN);

    op->config = htonl(pp->config & OFPPC11_ALL);
    op->state = htonl(pp->state & OFPPS11_ALL);

    op->curr = netdev_port_features_to_ofp11(pp->curr);
    op->advertised = netdev_port_features_to_ofp11(pp->advertised);
    op->supported = netdev_port_features_to_ofp11(pp->supported);
    op->peer = netdev_port_features_to_ofp11(pp->peer);

    op->curr_speed = htonl(pp->curr_speed);
    op->max_speed = htonl(pp->max_speed);
}

static void
ofputil_put_phy_port(uint8_t ofp_version, const struct ofputil_phy_port *pp,
                     struct ofpbuf *b)
{
    if (ofp_version == OFP10_VERSION) {
        struct ofp10_phy_port *opp;
        if (b->size + sizeof *opp <= UINT16_MAX) {
            opp = ofpbuf_put_uninit(b, sizeof *opp);
            ofputil_encode_ofp10_phy_port(pp, opp);
        }
    } else {
        struct ofp11_port *op;
        if (b->size + sizeof *op <= UINT16_MAX) {
            op = ofpbuf_put_uninit(b, sizeof *op);
            ofputil_encode_ofp11_port(pp, op);
        }
    }
}

void
ofputil_append_port_desc_stats_reply(uint8_t ofp_version,
                                     const struct ofputil_phy_port *pp,
                                     struct list *replies)
{
    if (ofp_version == OFP10_VERSION) {
        struct ofp10_phy_port *opp;

        opp = ofputil_append_stats_reply(sizeof *opp, replies);
        ofputil_encode_ofp10_phy_port(pp, opp);
    } else {
        struct ofp11_port *op;

        op = ofputil_append_stats_reply(sizeof *op, replies);
        ofputil_encode_ofp11_port(pp, op);
    }
}

/* ofputil_switch_features */

#define OFPC_COMMON (OFPC_FLOW_STATS | OFPC_TABLE_STATS | OFPC_PORT_STATS | \
                     OFPC_IP_REASM | OFPC_QUEUE_STATS)
BUILD_ASSERT_DECL((int) OFPUTIL_C_FLOW_STATS == OFPC_FLOW_STATS);
BUILD_ASSERT_DECL((int) OFPUTIL_C_TABLE_STATS == OFPC_TABLE_STATS);
BUILD_ASSERT_DECL((int) OFPUTIL_C_PORT_STATS == OFPC_PORT_STATS);
BUILD_ASSERT_DECL((int) OFPUTIL_C_IP_REASM == OFPC_IP_REASM);
BUILD_ASSERT_DECL((int) OFPUTIL_C_QUEUE_STATS == OFPC_QUEUE_STATS);
BUILD_ASSERT_DECL((int) OFPUTIL_C_ARP_MATCH_IP == OFPC_ARP_MATCH_IP);

struct ofputil_action_bit_translation {
    enum ofputil_action_bitmap ofputil_bit;
    int of_bit;
};

static const struct ofputil_action_bit_translation of10_action_bits[] = {
    { OFPUTIL_A_OUTPUT,       OFPAT10_OUTPUT },
    { OFPUTIL_A_SET_VLAN_VID, OFPAT10_SET_VLAN_VID },
    { OFPUTIL_A_SET_VLAN_PCP, OFPAT10_SET_VLAN_PCP },
    { OFPUTIL_A_STRIP_VLAN,   OFPAT10_STRIP_VLAN },
    { OFPUTIL_A_SET_DL_SRC,   OFPAT10_SET_DL_SRC },
    { OFPUTIL_A_SET_DL_DST,   OFPAT10_SET_DL_DST },
    { OFPUTIL_A_SET_NW_SRC,   OFPAT10_SET_NW_SRC },
    { OFPUTIL_A_SET_NW_DST,   OFPAT10_SET_NW_DST },
    { OFPUTIL_A_SET_NW_TOS,   OFPAT10_SET_NW_TOS },
    { OFPUTIL_A_SET_TP_SRC,   OFPAT10_SET_TP_SRC },
    { OFPUTIL_A_SET_TP_DST,   OFPAT10_SET_TP_DST },
    { OFPUTIL_A_ENQUEUE,      OFPAT10_ENQUEUE },
    { 0, 0 },
};

static const struct ofputil_action_bit_translation of11_action_bits[] = {
    { OFPUTIL_A_OUTPUT,         OFPAT11_OUTPUT },
    { OFPUTIL_A_SET_VLAN_VID,   OFPAT11_SET_VLAN_VID },
    { OFPUTIL_A_SET_VLAN_PCP,   OFPAT11_SET_VLAN_PCP },
    { OFPUTIL_A_SET_DL_SRC,     OFPAT11_SET_DL_SRC },
    { OFPUTIL_A_SET_DL_DST,     OFPAT11_SET_DL_DST },
    { OFPUTIL_A_SET_NW_SRC,     OFPAT11_SET_NW_SRC },
    { OFPUTIL_A_SET_NW_DST,     OFPAT11_SET_NW_DST },
    { OFPUTIL_A_SET_NW_TOS,     OFPAT11_SET_NW_TOS },
    { OFPUTIL_A_SET_NW_ECN,     OFPAT11_SET_NW_ECN },
    { OFPUTIL_A_SET_TP_SRC,     OFPAT11_SET_TP_SRC },
    { OFPUTIL_A_SET_TP_DST,     OFPAT11_SET_TP_DST },
    { OFPUTIL_A_COPY_TTL_OUT,   OFPAT11_COPY_TTL_OUT },
    { OFPUTIL_A_COPY_TTL_IN,    OFPAT11_COPY_TTL_IN },
    { OFPUTIL_A_SET_MPLS_LABEL, OFPAT11_SET_MPLS_LABEL },
    { OFPUTIL_A_SET_MPLS_TC,    OFPAT11_SET_MPLS_TC },
    { OFPUTIL_A_SET_MPLS_TTL,   OFPAT11_SET_MPLS_TTL },
    { OFPUTIL_A_DEC_MPLS_TTL,   OFPAT11_DEC_MPLS_TTL },
    { OFPUTIL_A_PUSH_VLAN,      OFPAT11_PUSH_VLAN },
    { OFPUTIL_A_POP_VLAN,       OFPAT11_POP_VLAN },
    { OFPUTIL_A_PUSH_MPLS,      OFPAT11_PUSH_MPLS },
    { OFPUTIL_A_POP_MPLS,       OFPAT11_POP_MPLS },
    { OFPUTIL_A_SET_QUEUE,      OFPAT11_SET_QUEUE },
    { OFPUTIL_A_GROUP,          OFPAT11_GROUP },
    { OFPUTIL_A_SET_NW_TTL,     OFPAT11_SET_NW_TTL },
    { OFPUTIL_A_DEC_NW_TTL,     OFPAT11_DEC_NW_TTL },
    { 0, 0 },
};

static const struct ofputil_action_bit_translation of12_action_bits[] = {
    { OFPUTIL_A_OUTPUT,         OFPAT12_OUTPUT },
    { OFPUTIL_A_COPY_TTL_OUT,   OFPAT12_COPY_TTL_OUT },
    { OFPUTIL_A_COPY_TTL_IN,    OFPAT12_COPY_TTL_IN },
    { OFPUTIL_A_SET_MPLS_TTL,   OFPAT12_SET_MPLS_TTL },
    { OFPUTIL_A_DEC_MPLS_TTL,   OFPAT12_DEC_MPLS_TTL },
    { OFPUTIL_A_PUSH_VLAN,      OFPAT12_PUSH_VLAN },
    { OFPUTIL_A_POP_VLAN,       OFPAT12_POP_VLAN },
    { OFPUTIL_A_PUSH_MPLS,      OFPAT12_PUSH_MPLS },
    { OFPUTIL_A_POP_MPLS,       OFPAT12_POP_MPLS },
    { OFPUTIL_A_SET_QUEUE,      OFPAT12_SET_QUEUE },
    { OFPUTIL_A_GROUP,          OFPAT12_GROUP },
    { OFPUTIL_A_SET_NW_TTL,     OFPAT12_SET_NW_TTL },
    { OFPUTIL_A_DEC_NW_TTL,     OFPAT12_DEC_NW_TTL },
    { OFPUTIL_A_SET_FIELD,      OFPAT12_SET_FIELD },
    { 0, 0 },
};

static enum ofputil_action_bitmap
decode_action_bits(ovs_be32 of_actions,
                   const struct ofputil_action_bit_translation *x)
{
    enum ofputil_action_bitmap ofputil_actions;

    ofputil_actions = 0;
    for (; x->ofputil_bit; x++) {
        if (of_actions & htonl(1u << x->of_bit)) {
            ofputil_actions |= x->ofputil_bit;
        }
    }
    return ofputil_actions;
}

static uint32_t
ofputil_capabilities_mask(uint8_t ofp_version)
{
    /* Handle capabilities whose bit is unique for all Open Flow versions */
    switch (ofp_version) {
    case OFP10_VERSION:
    case OFP11_VERSION:
        return OFPC_COMMON | OFPUTIL_C_ARP_MATCH_IP;
    case OFP12_VERSION:
        return OFPC_COMMON | OFPUTIL_C_PORT_BLOCKED;
    default:
        /* Caller needs to check osf->header.version itself */
        return 0;
    }
}

/* Decodes an OpenFlow 1.0 or 1.1 "switch_features" structure 'osf' into an
 * abstract representation in '*features'.  Initializes '*b' to iterate over
 * the OpenFlow port structures following 'osf' with later calls to
 * ofputil_pull_phy_port().  Returns 0 if successful, otherwise an
 * OFPERR_* value.  */
enum ofperr
ofputil_decode_switch_features(const struct ofp_switch_features *osf,
                               struct ofputil_switch_features *features,
                               struct ofpbuf *b)
{
    ofpbuf_use_const(b, osf, ntohs(osf->header.length));
    ofpbuf_pull(b, sizeof *osf);

    features->datapath_id = ntohll(osf->datapath_id);
    features->n_buffers = ntohl(osf->n_buffers);
    features->n_tables = osf->n_tables;

    features->capabilities = ntohl(osf->capabilities) &
        ofputil_capabilities_mask(osf->header.version);

    if (b->size % ofputil_get_phy_port_size(osf->header.version)) {
        return OFPERR_OFPBRC_BAD_LEN;
    }

    if (osf->header.version == OFP10_VERSION) {
        if (osf->capabilities & htonl(OFPC10_STP)) {
            features->capabilities |= OFPUTIL_C_STP;
        }
        features->actions = decode_action_bits(osf->actions, of10_action_bits);
    } else if (osf->header.version == OFP11_VERSION ||
               osf->header.version == OFP12_VERSION) {
        if (osf->capabilities & htonl(OFPC11_GROUP_STATS)) {
            features->capabilities |= OFPUTIL_C_GROUP_STATS;
        }
        if (osf->header.version == OFP11_VERSION) {
            features->actions = decode_action_bits(osf->actions,
                                                   of11_action_bits);
        } else if (osf->header.version == OFP12_VERSION) {
            features->actions = decode_action_bits(osf->actions,
                                                   of12_action_bits);
        }
    } else {
        return OFPERR_OFPBRC_BAD_VERSION;
    }

    return 0;
}

/* Returns true if the maximum number of ports are in 'osf'. */
static bool
max_ports_in_features(const struct ofp_switch_features *osf)
{
    size_t pp_size = ofputil_get_phy_port_size(osf->header.version);
    return ntohs(osf->header.length) + pp_size > UINT16_MAX;
}

/* Given a buffer 'b' that contains a Features Reply message, checks if
 * it contains the maximum number of ports that will fit.  If so, it
 * returns true and removes the ports from the message.  The caller
 * should then send an OFPST_PORT_DESC stats request to get the ports,
 * since the switch may have more ports than could be represented in the
 * Features Reply.  Otherwise, returns false.
 */
bool
ofputil_switch_features_ports_trunc(struct ofpbuf *b)
{
    struct ofp_switch_features *osf = b->data;

    if (max_ports_in_features(osf)) {
        /* Remove all the ports. */
        b->size = sizeof(*osf);
        update_openflow_length(b);

        return true;
    }

    return false;
}

static ovs_be32
encode_action_bits(enum ofputil_action_bitmap ofputil_actions,
                   const struct ofputil_action_bit_translation *x)
{
    uint32_t of_actions;

    of_actions = 0;
    for (; x->ofputil_bit; x++) {
        if (ofputil_actions & x->ofputil_bit) {
            of_actions |= 1 << x->of_bit;
        }
    }
    return htonl(of_actions);
}

/* Returns a buffer owned by the caller that encodes 'features' in the format
 * required by 'protocol' with the given 'xid'.  The caller should append port
 * information to the buffer with subsequent calls to
 * ofputil_put_switch_features_port(). */
struct ofpbuf *
ofputil_encode_switch_features(const struct ofputil_switch_features *features,
                               enum ofputil_protocol protocol, ovs_be32 xid)
{
    struct ofp_switch_features *osf;
    struct ofpbuf *b;

    osf = make_openflow_xid(sizeof *osf,
                            ofputil_protocol_to_ofp_version(protocol),
                            OFPT_FEATURES_REPLY, xid, &b);
    osf->header.version = ofputil_protocol_to_ofp_version(protocol);
    osf->datapath_id = htonll(features->datapath_id);
    osf->n_buffers = htonl(features->n_buffers);
    osf->n_tables = features->n_tables;

    osf->capabilities = htonl(features->capabilities &
                              ofputil_capabilities_mask(osf->header.version));
    if (osf->header.version == OFP10_VERSION) {
        if (features->capabilities & OFPUTIL_C_STP) {
            osf->capabilities |= htonl(OFPC10_STP);
        }
        osf->actions = encode_action_bits(features->actions, of10_action_bits);
    } else {
        if (features->capabilities & OFPUTIL_C_GROUP_STATS) {
            osf->capabilities |= htonl(OFPC11_GROUP_STATS);
        }
        if (osf->header.version == OFP11_VERSION) {
            osf->actions = encode_action_bits(features->actions,
                                              of11_action_bits);
        } else if (osf->header.version == OFP12_VERSION) {
            osf->actions = encode_action_bits(features->actions,
                                              of12_action_bits);
        }
    }

    return b;
}

/* Encodes 'pp' into the format required by the switch_features message already
 * in 'b', which should have been returned by ofputil_encode_switch_features(),
 * and appends the encoded version to 'b'. */
void
ofputil_put_switch_features_port(const struct ofputil_phy_port *pp,
                                 struct ofpbuf *b)
{
    const struct ofp_switch_features *osf = b->data;

    ofputil_put_phy_port(osf->header.version, pp, b);
}

/* ofputil_port_status */

/* Decodes the OpenFlow "port status" message in '*ops' into an abstract form
 * in '*ps'.  Returns 0 if successful, otherwise an OFPERR_* value. */
enum ofperr
ofputil_decode_port_status(const struct ofp_port_status *ops,
                           struct ofputil_port_status *ps)
{
    struct ofpbuf b;
    int retval;

    if (ops->reason != OFPPR_ADD &&
        ops->reason != OFPPR_DELETE &&
        ops->reason != OFPPR_MODIFY) {
        return OFPERR_NXBRC_BAD_REASON;
    }
    ps->reason = ops->reason;

    ofpbuf_use_const(&b, ops, ntohs(ops->header.length));
    ofpbuf_pull(&b, sizeof *ops);
    retval = ofputil_pull_phy_port(ops->header.version, &b, &ps->desc);
    assert(retval != EOF);
    return retval;
}

/* Converts the abstract form of a "port status" message in '*ps' into an
 * OpenFlow message suitable for 'protocol', and returns that encoded form in
 * a buffer owned by the caller. */
struct ofpbuf *
ofputil_encode_port_status(const struct ofputil_port_status *ps,
                           enum ofputil_protocol protocol)
{
    struct ofp_port_status *ops;
    struct ofpbuf *b;

    b = ofpbuf_new(sizeof *ops + sizeof(struct ofp11_port));
    ops = put_openflow_xid(sizeof *ops,
                           ofputil_protocol_to_ofp_version(protocol),
                           OFPT_PORT_STATUS, htonl(0), b);
    ops->reason = ps->reason;
    ofputil_put_phy_port(ops->header.version, &ps->desc, b);
    update_openflow_length(b);
    return b;
}

/* ofputil_port_mod */

/* Decodes the OpenFlow "port mod" message in '*oh' into an abstract form in
 * '*pm'.  Returns 0 if successful, otherwise an OFPERR_* value. */
enum ofperr
ofputil_decode_port_mod(const struct ofp_header *oh,
                        struct ofputil_port_mod *pm)
{
    if (oh->version == OFP10_VERSION) {
        const struct ofp10_port_mod *opm = (const struct ofp10_port_mod *) oh;

        if (oh->length != htons(sizeof *opm)) {
            return OFPERR_OFPBRC_BAD_LEN;
        }

        pm->port_no = ntohs(opm->port_no);
        memcpy(pm->hw_addr, opm->hw_addr, ETH_ADDR_LEN);
        pm->config = ntohl(opm->config) & OFPPC10_ALL;
        pm->mask = ntohl(opm->mask) & OFPPC10_ALL;
        pm->advertise = netdev_port_features_from_ofp10(opm->advertise);
    } else if (oh->version == OFP11_VERSION || oh->version == OFP12_VERSION) {
        const struct ofp11_port_mod *opm = (const struct ofp11_port_mod *) oh;
        enum ofperr error;

        if (oh->length != htons(sizeof *opm)) {
            return OFPERR_OFPBRC_BAD_LEN;
        }

        error = ofputil_port_from_ofp11(opm->port_no, &pm->port_no);
        if (error) {
            return error;
        }

        memcpy(pm->hw_addr, opm->hw_addr, ETH_ADDR_LEN);
        pm->config = ntohl(opm->config) & OFPPC11_ALL;
        pm->mask = ntohl(opm->mask) & OFPPC11_ALL;
        pm->advertise = netdev_port_features_from_ofp11(opm->advertise);
    } else {
        return OFPERR_OFPBRC_BAD_VERSION;
    }

    pm->config &= pm->mask;
    return 0;
}

/* Converts the abstract form of a "port mod" message in '*pm' into an OpenFlow
 * message suitable for 'protocol', and returns that encoded form in a buffer
 * owned by the caller. */
struct ofpbuf *
ofputil_encode_port_mod(const struct ofputil_port_mod *pm,
                        enum ofputil_protocol protocol)
{
    uint8_t ofp_version = ofputil_protocol_to_ofp_version(protocol);
    struct ofpbuf *b;

    if (ofp_version == OFP10_VERSION) {
        struct ofp10_port_mod *opm;

        opm = make_openflow(sizeof *opm, ofp_version, OFPT10_PORT_MOD, &b);
        opm->port_no = htons(pm->port_no);
        memcpy(opm->hw_addr, pm->hw_addr, ETH_ADDR_LEN);
        opm->config = htonl(pm->config & OFPPC10_ALL);
        opm->mask = htonl(pm->mask & OFPPC10_ALL);
        opm->advertise = netdev_port_features_to_ofp10(pm->advertise);
    } else if (ofp_version == OFP11_VERSION || ofp_version == OFP12_VERSION) {
        struct ofp11_port_mod *opm;

        opm = make_openflow(sizeof *opm, ofp_version, OFPT11_PORT_MOD, &b);
        opm->port_no = htonl(pm->port_no);
        memcpy(opm->hw_addr, pm->hw_addr, ETH_ADDR_LEN);
        opm->config = htonl(pm->config & OFPPC11_ALL);
        opm->mask = htonl(pm->mask & OFPPC11_ALL);
        opm->advertise = netdev_port_features_to_ofp11(pm->advertise);
    } else {
        NOT_REACHED();
    }

    return b;
}

struct ofpbuf *
ofputil_encode_packet_out(const struct ofputil_packet_out *po,
                          enum ofputil_protocol protocol)
{
    uint8_t ofp_version = ofputil_protocol_to_ofp_version(protocol);
    struct ofpbuf *msg;
    size_t packet_len = 0;

    if (po->buffer_id == UINT32_MAX) {
        packet_len = po->packet_len;
    }

    if (ofp_version == OFP11_VERSION || ofp_version == OFP12_VERSION) {
        struct ofp11_packet_out *opo;

        msg = ofpbuf_new(packet_len + sizeof *opo);
        opo = put_openflow(sizeof *opo, ofp_version, OFPT11_PACKET_OUT, msg);
        opo->buffer_id = htonl(po->buffer_id);
        opo->in_port = ofputil_port_to_ofp11(po->in_port);
        opo->actions_len = htons(msg->size - sizeof *opo);
    } else if (ofp_version == OFP10_VERSION) {
        struct ofp_packet_out *opo;

        msg = ofpbuf_new(packet_len + sizeof *opo);
        put_openflow(sizeof *opo, ofp_version, OFPT10_PACKET_OUT, msg);
        opo = msg->data;
        opo->buffer_id = htonl(po->buffer_id);
        opo->in_port = htons(po->in_port);
        opo->actions_len = htons(msg->size - sizeof *opo);
    } else {
        NOT_REACHED();
    }

    ofpacts_to_openflow10(po->ofpacts, msg);

    if (po->buffer_id == UINT32_MAX) {
        ofpbuf_put(msg, po->packet, po->packet_len);
    }

    update_openflow_length(msg);

    return msg;
}

/* Returns a string representing the message type of 'type'.  The string is the
 * enumeration constant for the type, e.g. "OFPT_HELLO".  For statistics
 * messages, the constant is followed by "request" or "reply",
 * e.g. "OFPST_AGGREGATE reply". */
const char *
ofputil_msg_type_name(const struct ofputil_msg_type *type)
{
    return type->name;
}

/* Allocates and stores in '*bufferp' a new ofpbuf with a size of
 * 'openflow_len', starting with an OpenFlow header with the given
 * 'version' and 'type', and an arbitrary transaction id.  Allocated bytes
 * beyond the header, if any, are zeroed.
 *
 * The caller is responsible for freeing '*bufferp' when it is no longer
 * needed.
 *
 * The OpenFlow header length is initially set to 'openflow_len'; if the
 * message is later extended, the length should be updated with
 * update_openflow_length() before sending.
 *
 * Returns the header. */
void *
make_openflow(size_t openflow_len, uint8_t version, uint8_t type,
              struct ofpbuf **bufferp)
{
    *bufferp = ofpbuf_new(openflow_len);
    return put_openflow_xid(openflow_len, version, type, alloc_xid(), *bufferp);
}

/* Similar to make_openflow() but creates a Nicira vendor extension message
 * with the specific 'subtype'.  'subtype' should be in host byte order. */
void *
make_nxmsg(size_t openflow_len, uint32_t subtype, struct ofpbuf **bufferp)
{
    return make_nxmsg_xid(openflow_len, subtype, alloc_xid(), bufferp);
}

/* Allocates and stores in '*bufferp' a new ofpbuf with a size of
 * 'openflow_len', starting with an OpenFlow header with the given 'type' and
 * transaction id 'xid'.  Allocated bytes beyond the header, if any, are
 * zeroed.
 *
 * The caller is responsible for freeing '*bufferp' when it is no longer
 * needed.
 *
 * The OpenFlow header length is initially set to 'openflow_len'; if the
 * message is later extended, the length should be updated with
 * update_openflow_length() before sending.
 *
 * Returns the header. */
void *
make_openflow_xid(size_t openflow_len, uint8_t version, uint8_t type,
                  ovs_be32 xid, struct ofpbuf **bufferp)
{
    *bufferp = ofpbuf_new(openflow_len);
    return put_openflow_xid(openflow_len, version, type, xid, *bufferp);
}

/* Similar to make_openflow_xid() but creates a Nicira vendor extension message
 * with the specific 'subtype'.  'subtype' should be in host byte order. */
void *
make_nxmsg_xid(size_t openflow_len, uint32_t subtype, ovs_be32 xid,
               struct ofpbuf **bufferp)
{
    *bufferp = ofpbuf_new(openflow_len);
    return put_nxmsg_xid(openflow_len, subtype, xid, *bufferp);
}

/* Appends 'openflow_len' bytes to 'buffer', starting with an OpenFlow header
 * with the given 'type' and an arbitrary transaction id.  Allocated bytes
 * beyond the header, if any, are zeroed.
 *
 * The OpenFlow header length is initially set to 'openflow_len'; if the
 * message is later extended, the length should be updated with
 * update_openflow_length() before sending.
 *
 * Returns the header. */
void *
put_openflow(size_t openflow_len, uint8_t version, uint8_t type,
             struct ofpbuf *buffer)
{
    return put_openflow_xid(openflow_len, version, type,
                            alloc_xid(), buffer);
}

/* Appends 'openflow_len' bytes to 'buffer', starting with an OpenFlow header
 * with the given 'type' and an transaction id 'xid'.  Allocated bytes beyond
 * the header, if any, are zeroed.
 *
 * The OpenFlow header length is initially set to 'openflow_len'; if the
 * message is later extended, the length should be updated with
 * update_openflow_length() before sending.
 *
 * Returns the header. */
void *
put_openflow_xid(size_t openflow_len, uint8_t version,
                 uint8_t type, ovs_be32 xid, struct ofpbuf *buffer)
{
    struct ofp_header *oh;

    assert(openflow_len >= sizeof *oh);
    assert(openflow_len <= UINT16_MAX);

    oh = ofpbuf_put_uninit(buffer, openflow_len);
    oh->version = version;
    oh->type = type;
    oh->length = htons(openflow_len);
    oh->xid = xid;
    memset(oh + 1, 0, openflow_len - sizeof *oh);
    return oh;
}

/* Similar to put_openflow() but append a Nicira vendor extension message with
 * the specific 'subtype'.  'subtype' should be in host byte order. */
void *
put_nxmsg(size_t openflow_len, uint32_t subtype, struct ofpbuf *buffer)
{
    return put_nxmsg_xid(openflow_len, subtype, alloc_xid(), buffer);
}

/* Similar to put_openflow_xid() but append a Nicira vendor extension message
 * with the specific 'subtype'.  'subtype' should be in host byte order. */
void *
put_nxmsg_xid(size_t openflow_len, uint32_t subtype, ovs_be32 xid,
              struct ofpbuf *buffer)
{
    struct nicira_header *nxh;

    nxh = put_openflow_xid(openflow_len, OFP10_VERSION, OFPT_VENDOR,
                           xid, buffer);
    nxh->vendor = htonl(NX_VENDOR_ID);
    nxh->subtype = htonl(subtype);
    return nxh;
}

/* Updates the 'length' field of the OpenFlow message in 'buffer' to
 * 'buffer->size'. */
void
update_openflow_length(struct ofpbuf *buffer)
{
    struct ofp_header *oh = ofpbuf_at_assert(buffer, 0, sizeof *oh);
    oh->length = htons(buffer->size);
}

static void
put_stats__(ovs_be32 xid, uint8_t ofp_version, uint8_t ofp_type,
            ovs_be16 ofpst_type, ovs_be32 nxst_subtype,
            struct ofpbuf *msg)
{
    if (ofpst_type == htons(OFPST_VENDOR)) {
        struct nicira10_stats_msg *nsm;

        nsm = put_openflow_xid(sizeof *nsm, OFP10_VERSION, ofp_type, xid, msg);
        nsm->vsm.osm.type = ofpst_type;
        nsm->vsm.vendor = htonl(NX_VENDOR_ID);
        nsm->subtype = nxst_subtype;
    } else {
        if (ofp_version == OFP10_VERSION) {
            struct ofp10_stats_msg *osm;

            osm = put_openflow_xid(sizeof *osm, ofp_version, ofp_type,
                                   xid, msg);
            osm->type = ofpst_type;
        } else {
            struct ofp11_stats_msg *osm;

            osm = put_openflow_xid(sizeof *osm, ofp_version, ofp_type,
                                   xid, msg);
            osm->type = ofpst_type;
        }
    }
}

/* Creates a statistics request message with the given 'ofpst_type', and stores
 * the buffer containing the new message in '*bufferp'.  If 'ofpst_type' is
 * OFPST_VENDOR then 'nxst_subtype' is used as the Nicira vendor extension
 * statistics subtype (otherwise 'nxst_subtype' is ignored).
 *
 * Appends 'body_len' bytes of zeroes to the reply as the body and returns the
 * first byte of the body. */
void *
ofputil_make_stats_request(size_t body_len, uint8_t ofp_version,
                           uint16_t ofpst_type, uint32_t nxst_subtype,
                           struct ofpbuf **bufferp)
{
    enum {
        HEADER_LEN = MAX(MAX(sizeof(struct ofp10_stats_msg),
                             sizeof(struct ofp11_stats_msg)),
                         sizeof(struct nicira10_stats_msg))
    };
    struct ofpbuf *msg;
    uint8_t ofp_type;

    switch (ofp_version) {
    case OFP12_VERSION:
    case OFP11_VERSION:
        ofp_type = OFPT11_STATS_REQUEST;
        break;

    case OFP10_VERSION:
        ofp_type = OFPT10_STATS_REQUEST;
        break;

    default:
        NOT_REACHED();
    }

    msg = *bufferp = ofpbuf_new(HEADER_LEN + body_len);
    put_stats__(alloc_xid(), ofp_version, ofp_type,
                htons(ofpst_type), htonl(nxst_subtype), msg);

    return ofpbuf_put_zeros(msg, body_len);
}

static void
put_stats_reply__(const struct ofp_header *request, struct ofpbuf *msg)
{
    const struct ofp10_stats_msg *osm;
    uint8_t ofp_type;

    switch (request->version) {
    case OFP12_VERSION:
    case OFP11_VERSION:
        ofp_type = OFPT11_STATS_REPLY;
        assert(request->type == OFPT11_STATS_REQUEST ||
               request->type == OFPT11_STATS_REPLY);
        break;

    case OFP10_VERSION:
        assert(request->type == OFPT10_STATS_REQUEST ||
               request->type == OFPT10_STATS_REPLY);
        ofp_type = OFPT10_STATS_REPLY;
        break;

    default:
        NOT_REACHED();
    }

    /* This is fine because the non-pad elements of
     * struct ofp10_stats_msg and struct ofp11_stats_msg
     * are at the same offsets */
    osm = (const struct ofp10_stats_msg *) request;
    put_stats__(request->xid, request->version, ofp_type, osm->type,
                (osm->type != htons(OFPST_VENDOR)
                 ? htonl(0)
                 : ((const struct nicira10_stats_msg *) request)->subtype),
                msg);
}

/* Creates a statistics reply message with the same type (either a standard
 * OpenFlow statistics type or a Nicira extension type and subtype) as
 * 'request', and stores the buffer containing the new message in '*bufferp'.
 *
 * Appends 'body_len' bytes of zeroes to the reply as the body and returns the
 * first byte of the body. */
void *
ofputil_make_stats_reply(size_t body_len,
                         const struct ofp_header *request,
                         struct ofpbuf **bufferp)
{
    struct ofpbuf *msg;

    msg = *bufferp = ofpbuf_new(24 + body_len);
    put_stats_reply__(request, msg);

    return ofpbuf_put_zeros(msg, body_len);
}

/* Initializes 'replies' as a list of ofpbufs that will contain a series of
 * replies to 'request', which should be an OpenFlow or Nicira extension
 * statistics request.  Initially 'replies' will have a single reply message
 * that has only a header.  The functions ofputil_reserve_stats_reply() and
 * ofputil_append_stats_reply() may be used to add to the reply. */
void
ofputil_start_stats_reply(const struct ofp_header *request,
                          struct list *replies)
{
    struct ofpbuf *msg;

    msg = ofpbuf_new(1024);
    put_stats_reply__(request, msg);

    list_init(replies);
    list_push_back(replies, &msg->list_node);
}

/* Prepares to append up to 'len' bytes to the series of statistics replies in
 * 'replies', which should have been initialized with
 * ofputil_start_stats_reply().  Returns an ofpbuf with at least 'len' bytes of
 * tailroom.  (The 'len' bytes have not actually be allocated; the caller must
 * do so with e.g. ofpbuf_put_uninit().) */
struct ofpbuf *
ofputil_reserve_stats_reply(size_t len, struct list *replies)
{
    struct ofpbuf *msg = ofpbuf_from_list(list_back(replies));
    struct ofp10_stats_msg *osm = msg->data;

    if (msg->size + len <= UINT16_MAX) {
        ofpbuf_prealloc_tailroom(msg, len);
    } else {
        osm->flags |= htons(OFPSF_REPLY_MORE);

        msg = ofpbuf_new(MAX(1024, sizeof(struct nicira10_stats_msg) + len));
        put_stats_reply__(&osm->header, msg);
        list_push_back(replies, &msg->list_node);
    }
    return msg;
}

/* Appends 'len' bytes to the series of statistics replies in 'replies', and
 * returns the first byte. */
void *
ofputil_append_stats_reply(size_t len, struct list *replies)
{
    return ofpbuf_put_uninit(ofputil_reserve_stats_reply(len, replies), len);
}

void
ofputil_postappend_stats_reply(size_t start_ofs, struct list *replies)
{
    struct ofpbuf *msg = ofpbuf_from_list(list_back(replies));

    assert(start_ofs <= UINT16_MAX);
    if (msg->size > UINT16_MAX) {
        size_t len = msg->size - start_ofs;
        memcpy(ofputil_append_stats_reply(len, replies),
               (const uint8_t *) msg->data + start_ofs, len);
        msg->size = start_ofs;
    }
}

bool
ofputil_is_stats_msg(const struct ofp_header *oh)
{
    return (
        oh->version == OFP10_VERSION
        ? oh->type == OFPT10_STATS_REQUEST || oh->type == OFPT10_STATS_REPLY
        : oh->type == OFPT11_STATS_REQUEST || oh->type == OFPT11_STATS_REPLY);
}

bool
ofputil_is_vendor_stats_msg(const struct ofp_header *oh)
{
    int min_len = (oh->version == OFP10_VERSION
                   ? sizeof(struct ofp10_vendor_stats_msg)
                   : sizeof(struct ofp11_vendor_stats_msg));
    return (ofputil_is_stats_msg(oh)
            && ntohs(oh->length) >= min_len
            && ofputil_decode_stats_msg_type(oh) == OFPST_VENDOR);
}

bool
ofputil_is_nx_stats_msg(const struct ofp_header *oh)
{
    BUILD_ASSERT_DECL(sizeof(struct nicira10_stats_msg) ==
                      sizeof(struct nicira11_stats_msg));

    return (ofputil_is_vendor_stats_msg(oh)
            && ntohs(oh->length) >= sizeof(struct nicira10_stats_msg)
            && ofputil_decode_stats_msg_vendor(oh) == NX_VENDOR_ID);
}

size_t
ofputil_stats_msg_len(const struct ofp_header *oh)
{
    if (ofputil_decode_stats_msg_type(oh) == OFPST_VENDOR) {
        return (oh->version == OFP10_VERSION
                ? sizeof(struct nicira10_stats_msg)
                : sizeof(struct nicira11_stats_msg));
    } else {
        return (oh->version == OFP10_VERSION
                ? sizeof(struct ofp10_stats_msg)
                : sizeof(struct ofp11_stats_msg));
    }
}

void
ofputil_pull_stats_msg(struct ofpbuf *msg)
{
    ofpbuf_pull(msg, ofputil_stats_msg_len(msg->data));
}

void *
ofputil_stats_msg_body(const struct ofp_header *oh)
{
    return (uint8_t *) oh + ofputil_stats_msg_len(oh);
}

uint16_t
ofputil_decode_stats_msg_type(const struct ofp_header *oh)
{
    BUILD_ASSERT_DECL(offsetof(struct ofp10_stats_msg, type) ==
                      offsetof(struct ofp11_stats_msg, type));
    assert(ofputil_is_stats_msg(oh));
    return ntohs(((const struct ofp10_stats_msg *) oh)->type);
}

uint32_t
ofputil_decode_stats_msg_vendor(const struct ofp_header *oh)
{
    assert(ofputil_is_vendor_stats_msg(oh));
    return ntohl(oh->version == OFP10_VERSION
                 ? ((const struct ofp10_vendor_stats_msg *) oh)->vendor
                 : ((const struct ofp11_vendor_stats_msg *) oh)->vendor);
}

uint32_t
ofputil_decode_stats_msg_subtype(const struct ofp_header *oh)
{
    assert(ofputil_is_nx_stats_msg(oh));
    return ntohl(oh->version == OFP10_VERSION
                 ? ((const struct nicira10_stats_msg *) oh)->subtype
                 : ((const struct nicira11_stats_msg *) oh)->subtype);
}

uint16_t
ofputil_decode_stats_msg_flags(const struct ofp_header *oh)
{
    BUILD_ASSERT_DECL(offsetof(struct ofp10_stats_msg, type) ==
                      offsetof(struct ofp11_stats_msg, type));
    assert(ofputil_is_stats_msg(oh));
    return ntohs(((const struct ofp10_stats_msg *) oh)->flags);
}

/* Creates and returns an OFPT_ECHO_REQUEST message with an empty payload. */
struct ofpbuf *
make_echo_request(uint8_t ofp_version)
{
    struct ofp_header *rq;
    struct ofpbuf *out = ofpbuf_new(sizeof *rq);
    rq = ofpbuf_put_uninit(out, sizeof *rq);
    rq->version = ofp_version;
    rq->type = OFPT_ECHO_REQUEST;
    rq->length = htons(sizeof *rq);
    rq->xid = htonl(0);
    return out;
}

/* Creates and returns an OFPT_ECHO_REPLY message matching the
 * OFPT_ECHO_REQUEST message in 'rq'. */
struct ofpbuf *
make_echo_reply(const struct ofp_header *rq)
{
    size_t size = ntohs(rq->length);
    struct ofpbuf *out = ofpbuf_new(size);
    struct ofp_header *reply = ofpbuf_put(out, rq, size);
    reply->type = OFPT_ECHO_REPLY;
    return out;
}

struct ofpbuf *
ofputil_encode_barrier_request(uint8_t ofp_version)
{
    struct ofpbuf *msg;
    uint8_t ofp_type;

    switch (ofp_version) {
    case OFP12_VERSION:
    case OFP11_VERSION:
        ofp_type = OFPT11_BARRIER_REQUEST;
        break;

    case OFP10_VERSION:
        ofp_type = OFPT10_BARRIER_REQUEST;
        break;

    default:
        NOT_REACHED();
    }

    make_openflow(sizeof(struct ofp_header), ofp_version, ofp_type, &msg);
    return msg;
}

void *
make_barrier_reply(uint8_t ofp_version, ovs_be32 xid, struct ofpbuf **bufferp)
{
    uint8_t ofp_type;

    switch (ofp_version) {
    case OFP12_VERSION:
    case OFP11_VERSION:
        ofp_type = OFPT11_BARRIER_REPLY;
        break;

    case OFP10_VERSION:
        ofp_type = OFPT10_BARRIER_REPLY;
        break;

    default:
        NOT_REACHED();
    }

    return make_openflow_xid(sizeof(struct ofp_header),
                             ofp_version, ofp_type, xid, bufferp);
}

const char *
ofputil_frag_handling_to_string(enum ofp_config_flags flags)
{
    switch (flags & OFPC_FRAG_MASK) {
    case OFPC_FRAG_NORMAL:   return "normal";
    case OFPC_FRAG_DROP:     return "drop";
    case OFPC_FRAG_REASM:    return "reassemble";
    case OFPC_FRAG_NX_MATCH: return "nx-match";
    }

    NOT_REACHED();
}

bool
ofputil_frag_handling_from_string(const char *s, enum ofp_config_flags *flags)
{
    if (!strcasecmp(s, "normal")) {
        *flags = OFPC_FRAG_NORMAL;
    } else if (!strcasecmp(s, "drop")) {
        *flags = OFPC_FRAG_DROP;
    } else if (!strcasecmp(s, "reassemble")) {
        *flags = OFPC_FRAG_REASM;
    } else if (!strcasecmp(s, "nx-match")) {
        *flags = OFPC_FRAG_NX_MATCH;
    } else {
        return false;
    }
    return true;
}

/* Converts the OpenFlow 1.1+ port number 'ofp11_port' into an OpenFlow 1.0
 * port number and stores the latter in '*ofp10_port', for the purpose of
 * decoding OpenFlow 1.1+ protocol messages.  Returns 0 if successful,
 * otherwise an OFPERR_* number.
 *
 * See the definition of OFP11_MAX for an explanation of the mapping. */
enum ofperr
ofputil_port_from_ofp11(ovs_be32 ofp11_port, uint16_t *ofp10_port)
{
    uint32_t ofp11_port_h = ntohl(ofp11_port);

    if (ofp11_port_h < OFPP_MAX) {
        *ofp10_port = ofp11_port_h;
        return 0;
    } else if (ofp11_port_h >= OFPP11_MAX) {
        *ofp10_port = ofp11_port_h - OFPP11_OFFSET;
        return 0;
    } else {
        VLOG_WARN_RL(&bad_ofmsg_rl, "port %"PRIu32" is outside the supported "
                     "range 0 through %d or 0x%"PRIx32" through 0x%"PRIx32,
                     ofp11_port_h, OFPP_MAX - 1,
                     (uint32_t) OFPP11_MAX, UINT32_MAX);
        return OFPERR_OFPBAC_BAD_OUT_PORT;
    }
}

/* Returns the OpenFlow 1.1+ port number equivalent to the OpenFlow 1.0 port
 * number 'ofp10_port', for encoding OpenFlow 1.1+ protocol messages.
 *
 * See the definition of OFP11_MAX for an explanation of the mapping. */
ovs_be32
ofputil_port_to_ofp11(uint16_t ofp10_port)
{
    return htonl(ofp10_port < OFPP_MAX
                 ? ofp10_port
                 : ofp10_port + OFPP11_OFFSET);
}

/* Checks that 'port' is a valid output port for the OFPAT10_OUTPUT action, given
 * that the switch will never have more than 'max_ports' ports.  Returns 0 if
 * 'port' is valid, otherwise an OpenFlow return code. */
enum ofperr
ofputil_check_output_port(uint16_t port, int max_ports)
{
    switch (port) {
    case OFPP_IN_PORT:
    case OFPP_TABLE:
    case OFPP_NORMAL:
    case OFPP_FLOOD:
    case OFPP_ALL:
    case OFPP_CONTROLLER:
    case OFPP_NONE:
    case OFPP_LOCAL:
        return 0;

    default:
        if (port < max_ports) {
            return 0;
        }
        return OFPERR_OFPBAC_BAD_OUT_PORT;
    }
}

#define OFPUTIL_NAMED_PORTS                     \
        OFPUTIL_NAMED_PORT(IN_PORT)             \
        OFPUTIL_NAMED_PORT(TABLE)               \
        OFPUTIL_NAMED_PORT(NORMAL)              \
        OFPUTIL_NAMED_PORT(FLOOD)               \
        OFPUTIL_NAMED_PORT(ALL)                 \
        OFPUTIL_NAMED_PORT(CONTROLLER)          \
        OFPUTIL_NAMED_PORT(LOCAL)               \
        OFPUTIL_NAMED_PORT(NONE)

/* Checks whether 's' is the string representation of an OpenFlow port number,
 * either as an integer or a string name (e.g. "LOCAL").  If it is, stores the
 * number in '*port' and returns true.  Otherwise, returns false. */
bool
ofputil_port_from_string(const char *name, uint16_t *port)
{
    struct pair {
        const char *name;
        uint16_t value;
    };
    static const struct pair pairs[] = {
#define OFPUTIL_NAMED_PORT(NAME) {#NAME, OFPP_##NAME},
        OFPUTIL_NAMED_PORTS
#undef OFPUTIL_NAMED_PORT
    };
    static const int n_pairs = ARRAY_SIZE(pairs);
    int i;

    if (str_to_int(name, 0, &i) && i >= 0 && i < UINT16_MAX) {
        *port = i;
        return true;
    }

    for (i = 0; i < n_pairs; i++) {
        if (!strcasecmp(name, pairs[i].name)) {
            *port = pairs[i].value;
            return true;
        }
    }
    return false;
}

/* Appends to 's' a string representation of the OpenFlow port number 'port'.
 * Most ports' string representation is just the port number, but for special
 * ports, e.g. OFPP_LOCAL, it is the name, e.g. "LOCAL". */
void
ofputil_format_port(uint16_t port, struct ds *s)
{
    const char *name;

    switch (port) {
#define OFPUTIL_NAMED_PORT(NAME) case OFPP_##NAME: name = #NAME; break;
        OFPUTIL_NAMED_PORTS
#undef OFPUTIL_NAMED_PORT

    default:
        ds_put_format(s, "%"PRIu16, port);
        return;
    }
    ds_put_cstr(s, name);
}

/* Given a buffer 'b' that contains an array of OpenFlow ports of type
 * 'ofp_version', tries to pull the first element from the array.  If
 * successful, initializes '*pp' with an abstract representation of the
 * port and returns 0.  If no ports remain to be decoded, returns EOF.
 * On an error, returns a positive OFPERR_* value. */
int
ofputil_pull_phy_port(uint8_t ofp_version, struct ofpbuf *b,
                      struct ofputil_phy_port *pp)
{
    if (ofp_version == OFP10_VERSION) {
        const struct ofp10_phy_port *opp = ofpbuf_try_pull(b, sizeof *opp);
        return opp ? ofputil_decode_ofp10_phy_port(pp, opp) : EOF;
    } else {
        const struct ofp11_port *op = ofpbuf_try_pull(b, sizeof *op);
        return op ? ofputil_decode_ofp11_port(pp, op) : EOF;
    }
}

/* Given a buffer 'b' that contains an array of OpenFlow ports of type
 * 'ofp_version', returns the number of elements. */
size_t ofputil_count_phy_ports(uint8_t ofp_version, struct ofpbuf *b)
{
    return b->size / ofputil_get_phy_port_size(ofp_version);
}

/* Returns the 'enum ofputil_action_code' corresponding to 'name' (e.g. if
 * 'name' is "output" then the return value is OFPUTIL_OFPAT10_OUTPUT), or -1 if
 * 'name' is not the name of any action.
 *
 * ofp-util.def lists the mapping from names to action. */
int
ofputil_action_code_from_name(const char *name)
{
    static const char *names[OFPUTIL_N_ACTIONS] = {
        NULL,
#define OFPAT10_ACTION(ENUM, STRUCT, NAME)           NAME,
#define OFPAT11_ACTION(ENUM, STRUCT, NAME)           NAME,
#define OFPIT11_ACTION(ENUM, STRUCT, NAME)           NAME,
#define OFPAT12_ACTION(ENUM, STRUCT, NAME)           NAME,
#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME) NAME,
#include "ofp-util.def"
    };

    const char **p;

    for (p = names; p < &names[ARRAY_SIZE(names)]; p++) {
        if (*p && !strcasecmp(name, *p)) {
            return p - names;
        }
    }
    return -1;
}

/* Appends an action of the type specified by 'code' to 'buf' and returns the
 * action.  Initializes the parts of 'action' that identify it as having type
 * <ENUM> and length 'sizeof *action' and zeros the rest.  For actions that
 * have variable length, the length used and cleared is that of struct
 * <STRUCT>.  */
void *
ofputil_put_action(enum ofputil_action_code code, struct ofpbuf *buf)
{
    switch (code) {
    case OFPUTIL_ACTION_INVALID:
        NOT_REACHED();

#define OFPAT10_ACTION(ENUM, STRUCT, NAME)                    \
    case OFPUTIL_##ENUM: return ofputil_put_##ENUM(buf);
#define OFPAT11_ACTION OFPAT10_ACTION
#define OFPIT11_ACTION OFPAT10_ACTION
#define OFPAT12_ACTION OFPAT10_ACTION
#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME)        \
    case OFPUTIL_##ENUM: return ofputil_put_##ENUM(buf);
#include "ofp-util.def"
    }
    NOT_REACHED();
}

#define OFPAT10_ACTION(ENUM, STRUCT, NAME)                        \
    void                                                        \
    ofputil_init_##ENUM(struct STRUCT *s)                       \
    {                                                           \
        memset(s, 0, sizeof *s);                                \
        s->type = htons(ENUM);                                  \
        s->len = htons(sizeof *s);                              \
    }                                                           \
                                                                \
    struct STRUCT *                                             \
    ofputil_put_##ENUM(struct ofpbuf *buf)                      \
    {                                                           \
        struct STRUCT *s = ofpbuf_put_uninit(buf, sizeof *s);   \
        ofputil_init_##ENUM(s);                                 \
        return s;                                               \
    }
#define OFPAT11_ACTION OFPAT10_ACTION
#define OFPIT11_ACTION OFPAT10_ACTION
#define OFPAT12_ACTION OFPAT10_ACTION
#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME)            \
    void                                                        \
    ofputil_init_##ENUM(struct STRUCT *s)                       \
    {                                                           \
        memset(s, 0, sizeof *s);                                \
        s->type = htons(OFPAT10_VENDOR);                        \
        s->len = htons(sizeof *s);                              \
        s->vendor = htonl(NX_VENDOR_ID);                        \
        s->subtype = htons(ENUM);                               \
    }                                                           \
                                                                \
    struct STRUCT *                                             \
    ofputil_put_##ENUM(struct ofpbuf *buf)                      \
    {                                                           \
        struct STRUCT *s = ofpbuf_put_uninit(buf, sizeof *s);   \
        ofputil_init_##ENUM(s);                                 \
        return s;                                               \
    }
#include "ofp-util.def"

/* "Normalizes" the wildcards in 'rule'.  That means:
 *
 *    1. If the type of level N is known, then only the valid fields for that
 *       level may be specified.  For example, ARP does not have a TOS field,
 *       so nw_tos must be wildcarded if 'rule' specifies an ARP flow.
 *       Similarly, IPv4 does not have any IPv6 addresses, so ipv6_src and
 *       ipv6_dst (and other fields) must be wildcarded if 'rule' specifies an
 *       IPv4 flow.
 *
 *    2. If the type of level N is not known (or not understood by Open
 *       vSwitch), then no fields at all for that level may be specified.  For
 *       example, Open vSwitch does not understand SCTP, an L4 protocol, so the
 *       L4 fields tp_src and tp_dst must be wildcarded if 'rule' specifies an
 *       SCTP flow.
 */
void
ofputil_normalize_rule(struct cls_rule *rule)
{
    enum {
        MAY_NW_ADDR     = 1 << 0, /* nw_src, nw_dst */
        MAY_TP_ADDR     = 1 << 1, /* tp_src, tp_dst */
        MAY_NW_PROTO    = 1 << 2, /* nw_proto */
        MAY_IPVx        = 1 << 3, /* tos, frag, ttl */
        MAY_ARP_SHA     = 1 << 4, /* arp_sha */
        MAY_ARP_THA     = 1 << 5, /* arp_tha */
        MAY_IPV6        = 1 << 6, /* ipv6_src, ipv6_dst, ipv6_label */
        MAY_ND_TARGET   = 1 << 7, /* nd_target */
        MAY_MPLS        = 1 << 8, /* mpls label and tc */
        MAY_VLAN_QINQ   = 1 << 9, /* vlan qinq tci */
    } may_match;

    struct flow_wildcards wc;

    /* Figure out what fields may be matched. */
    if (rule->flow.dl_type == htons(ETH_TYPE_IP)) {
        may_match = MAY_NW_PROTO | MAY_IPVx | MAY_NW_ADDR;
        if (rule->flow.nw_proto == IPPROTO_TCP ||
            rule->flow.nw_proto == IPPROTO_UDP ||
            rule->flow.nw_proto == IPPROTO_ICMP) {
            may_match |= MAY_TP_ADDR;
        }
    } else if (rule->flow.dl_type == htons(ETH_TYPE_IPV6)) {
        may_match = MAY_NW_PROTO | MAY_IPVx | MAY_IPV6;
        if (rule->flow.nw_proto == IPPROTO_TCP ||
            rule->flow.nw_proto == IPPROTO_UDP) {
            may_match |= MAY_TP_ADDR;
        } else if (rule->flow.nw_proto == IPPROTO_ICMPV6) {
            may_match |= MAY_TP_ADDR;
            if (rule->flow.tp_src == htons(ND_NEIGHBOR_SOLICIT)) {
                may_match |= MAY_ND_TARGET | MAY_ARP_SHA;
            } else if (rule->flow.tp_src == htons(ND_NEIGHBOR_ADVERT)) {
                may_match |= MAY_ND_TARGET | MAY_ARP_THA;
            }
        }
    } else if (rule->flow.dl_type == htons(ETH_TYPE_ARP)) {
        may_match = MAY_NW_PROTO | MAY_NW_ADDR | MAY_ARP_SHA | MAY_ARP_THA;
    } else if (rule->flow.dl_type == htons(ETH_TYPE_MPLS) ||
               rule->flow.dl_type == htons(ETH_TYPE_MPLS_MCAST)) {
        may_match = MAY_MPLS;
    } else if ((rule->flow.vlan_tpid == htons(ETH_TYPE_VLAN) ||
                rule->flow.vlan_tpid == htons(ETH_TYPE_VLAN_8021AD)) &&
               rule->flow.vlan_qinq_tci != htons(0)) {
        may_match = MAY_VLAN_QINQ;
    } else {
        may_match = 0;
    }

    /* Clear the fields that may not be matched. */
    wc = rule->wc;
    if (!(may_match & MAY_NW_ADDR)) {
        wc.nw_src_mask = wc.nw_dst_mask = htonl(0);
    }
    if (!(may_match & MAY_TP_ADDR)) {
        wc.tp_src_mask = wc.tp_dst_mask = htons(0);
    }
    if (!(may_match & MAY_NW_PROTO)) {
        wc.wildcards |= FWW_NW_PROTO;
    }
    if (!(may_match & MAY_IPVx)) {
        wc.wildcards |= FWW_NW_DSCP;
        wc.wildcards |= FWW_NW_ECN;
        wc.wildcards |= FWW_NW_TTL;
    }
    if (!(may_match & MAY_ARP_SHA)) {
        wc.wildcards |= FWW_ARP_SHA;
    }
    if (!(may_match & MAY_ARP_THA)) {
        wc.wildcards |= FWW_ARP_THA;
    }
    if (!(may_match & MAY_IPV6)) {
        wc.ipv6_src_mask = wc.ipv6_dst_mask = in6addr_any;
        wc.wildcards |= FWW_IPV6_LABEL;
    }
    if (!(may_match & MAY_ND_TARGET)) {
        wc.nd_target_mask = in6addr_any;
    }
    if (!(may_match & MAY_MPLS)) {
        wc.wildcards |= FWW_MPLS_LABEL;
        wc.wildcards |= FWW_MPLS_TC;
        wc.wildcards |= FWW_MPLS_STACK;
    }

    /* Log any changes. */
    if (!flow_wildcards_equal(&wc, &rule->wc)) {
        bool log = !VLOG_DROP_INFO(&bad_ofmsg_rl);
        char *pre = log ? cls_rule_to_string(rule) : NULL;

        rule->wc = wc;
        cls_rule_zero_wildcarded_fields(rule);

        if (log) {
            char *post = cls_rule_to_string(rule);
            VLOG_INFO("normalization changed ofp_match, details:");
            VLOG_INFO(" pre: %s", pre);
            VLOG_INFO("post: %s", post);
            free(pre);
            free(post);
        }
    }
}

/* Parses a key or a key-value pair from '*stringp'.
 *
 * On success: Stores the key into '*keyp'.  Stores the value, if present, into
 * '*valuep', otherwise an empty string.  Advances '*stringp' past the end of
 * the key-value pair, preparing it for another call.  '*keyp' and '*valuep'
 * are substrings of '*stringp' created by replacing some of its bytes by null
 * terminators.  Returns true.
 *
 * If '*stringp' is just white space or commas, sets '*keyp' and '*valuep' to
 * NULL and returns false. */
bool
ofputil_parse_key_value(char **stringp, char **keyp, char **valuep)
{
    char *pos, *key, *value;
    size_t key_len;

    pos = *stringp;
    pos += strspn(pos, ", \t\r\n");
    if (*pos == '\0') {
        *keyp = *valuep = NULL;
        return false;
    }

    key = pos;
    key_len = strcspn(pos, ":=(, \t\r\n");
    if (key[key_len] == ':' || key[key_len] == '=') {
        /* The value can be separated by a colon. */
        size_t value_len;

        value = key + key_len + 1;
        value_len = strcspn(value, ", \t\r\n");
        pos = value + value_len + (value[value_len] != '\0');
        value[value_len] = '\0';
    } else if (key[key_len] == '(') {
        /* The value can be surrounded by balanced parentheses.  The outermost
         * set of parentheses is removed. */
        int level = 1;
        size_t value_len;

        value = key + key_len + 1;
        for (value_len = 0; level > 0; value_len++) {
            switch (value[value_len]) {
            case '\0':
                level = 0;
                break;

            case '(':
                level++;
                break;

            case ')':
                level--;
                break;
            }
        }
        value[value_len - 1] = '\0';
        pos = value + value_len;
    } else {
        /* There might be no value at all. */
        value = key + key_len;  /* Will become the empty string below. */
        pos = key + key_len + (key[key_len] != '\0');
    }
    key[key_len] = '\0';

    *stringp = pos;
    *keyp = key;
    *valuep = value;
    return true;
}
