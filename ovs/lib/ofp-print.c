/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013 Nicira, Inc.
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
#include <sys/wait.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>

#include "bundle.h"
#include "byte-order.h"
#include "compiler.h"
#include "dynamic-string.h"
#include "flow.h"
#include "learn.h"
#include "multipath.h"
#include "meta-flow.h"
#include "netdev.h"
#include "nx-match.h"
#include "ofp-actions.h"
#include "ofp-errors.h"
#include "ofp-msgs.h"
#include "ofp-util.h"
#include "ofpbuf.h"
#include "openflow/openflow.h"
#include "openflow/nicira-ext.h"
#include "packets.h"
#include "pcap.h"
#include "type-props.h"
#include "unaligned.h"
#include "util.h"

static void ofp_print_queue_name(struct ds *string, uint32_t port);
static void ofp_print_error(struct ds *, enum ofperr);


/* Returns a string that represents the contents of the Ethernet frame in the
 * 'len' bytes starting at 'data'.  The caller must free the returned string.*/
char *
ofp_packet_to_string(const void *data, size_t len)
{
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct ofpbuf buf;
    struct flow flow;

    ofpbuf_use_const(&buf, data, len);
    flow_extract(&buf, 0, 0, NULL, 0, &flow);
    flow_format(&ds, &flow);

    if (buf.l7) {
        if (flow.nw_proto == IPPROTO_TCP) {
            struct tcp_header *th = buf.l4;
            ds_put_format(&ds, " tcp_csum:%"PRIx16,
                          ntohs(th->tcp_csum));
        } else if (flow.nw_proto == IPPROTO_UDP) {
            struct udp_header *uh = buf.l4;
            ds_put_format(&ds, " udp_csum:%"PRIx16,
                          ntohs(uh->udp_csum));
        }
    }

    ds_put_char(&ds, '\n');

    return ds_cstr(&ds);
}

static void
ofp_print_packet_in(struct ds *string, const struct ofp_header *oh,
                    int verbosity)
{
    struct ofputil_packet_in pin;
    int error;
    int i;

    error = ofputil_decode_packet_in(&pin, oh);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    if (pin.table_id) {
        ds_put_format(string, " table_id=%"PRIu8, pin.table_id);
    }

    if (pin.cookie) {
        ds_put_format(string, " cookie=0x%"PRIx64, ntohll(pin.cookie));
    }

    ds_put_format(string, " total_len=%"PRIu16" in_port=", pin.total_len);
    ofputil_format_port(pin.fmd.in_port, string);

    if (pin.fmd.tun_id != htonll(0)) {
        ds_put_format(string, " tun_id=0x%"PRIx64, ntohll(pin.fmd.tun_id));
    }

    if (pin.fmd.metadata != htonll(0)) {
        ds_put_format(string, " metadata=0x%"PRIx64, ntohll(pin.fmd.metadata));
    }

    for (i = 0; i < FLOW_N_REGS; i++) {
        if (pin.fmd.regs[i]) {
            ds_put_format(string, " reg%d=0x%"PRIx32, i, pin.fmd.regs[i]);
        }
    }

    ds_put_format(string, " (via %s)",
                  ofputil_packet_in_reason_to_string(pin.reason));

    ds_put_format(string, " data_len=%zu", pin.packet_len);
    if (pin.buffer_id == UINT32_MAX) {
        ds_put_format(string, " (unbuffered)");
        if (pin.total_len != pin.packet_len) {
            ds_put_format(string, " (***total_len != data_len***)");
        }
    } else {
        ds_put_format(string, " buffer=0x%08"PRIx32, pin.buffer_id);
        if (pin.total_len < pin.packet_len) {
            ds_put_format(string, " (***total_len < data_len***)");
        }
    }
    ds_put_char(string, '\n');

    if (verbosity > 0) {
        char *packet = ofp_packet_to_string(pin.packet, pin.packet_len);
        ds_put_cstr(string, packet);
        free(packet);
    }
}

static void
ofp_print_packet_out(struct ds *string, const struct ofp_header *oh,
                     int verbosity)
{
    struct ofputil_packet_out po;
    struct ofpbuf ofpacts;
    enum ofperr error;

    ofpbuf_init(&ofpacts, 64);
    error = ofputil_decode_packet_out(&po, oh, &ofpacts);
    if (error) {
        ofpbuf_uninit(&ofpacts);
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " in_port=");
    ofputil_format_port(po.in_port, string);

    ds_put_char(string, ' ');
    ofpacts_format(po.ofpacts, po.ofpacts_len, string);

    if (po.buffer_id == UINT32_MAX) {
        ds_put_format(string, " data_len=%zu", po.packet_len);
        if (verbosity > 0 && po.packet_len > 0) {
            char *packet = ofp_packet_to_string(po.packet, po.packet_len);
            ds_put_char(string, '\n');
            ds_put_cstr(string, packet);
            free(packet);
        }
    } else {
        ds_put_format(string, " buffer=0x%08"PRIx32, po.buffer_id);
    }
    ds_put_char(string, '\n');

    ofpbuf_uninit(&ofpacts);
}

/* qsort comparison function. */
static int
compare_ports(const void *a_, const void *b_)
{
    const struct ofputil_phy_port *a = a_;
    const struct ofputil_phy_port *b = b_;
    uint16_t ap = a->port_no;
    uint16_t bp = b->port_no;

    return ap < bp ? -1 : ap > bp;
}

static void
ofp_print_bit_names(struct ds *string, uint32_t bits,
                    const char *(*bit_to_name)(uint32_t bit),
                    char separator)
{
    int n = 0;
    int i;

    if (!bits) {
        ds_put_cstr(string, "0");
        return;
    }

    for (i = 0; i < 32; i++) {
        uint32_t bit = UINT32_C(1) << i;

        if (bits & bit) {
            const char *name = bit_to_name(bit);
            if (name) {
                if (n++) {
                    ds_put_char(string, separator);
                }
                ds_put_cstr(string, name);
                bits &= ~bit;
            }
        }
    }

    if (bits) {
        if (n) {
            ds_put_char(string, separator);
        }
        ds_put_format(string, "0x%"PRIx32, bits);
    }
}

static const char *
netdev_feature_to_name(uint32_t bit)
{
    enum netdev_features f = bit;

    switch (f) {
    case NETDEV_F_10MB_HD:    return "10MB-HD";
    case NETDEV_F_10MB_FD:    return "10MB-FD";
    case NETDEV_F_100MB_HD:   return "100MB-HD";
    case NETDEV_F_100MB_FD:   return "100MB-FD";
    case NETDEV_F_1GB_HD:     return "1GB-HD";
    case NETDEV_F_1GB_FD:     return "1GB-FD";
    case NETDEV_F_10GB_FD:    return "10GB-FD";
    case NETDEV_F_40GB_FD:    return "40GB-FD";
    case NETDEV_F_100GB_FD:   return "100GB-FD";
    case NETDEV_F_1TB_FD:     return "1TB-FD";
    case NETDEV_F_OTHER:      return "OTHER";
    case NETDEV_F_COPPER:     return "COPPER";
    case NETDEV_F_FIBER:      return "FIBER";
    case NETDEV_F_AUTONEG:    return "AUTO_NEG";
    case NETDEV_F_PAUSE:      return "AUTO_PAUSE";
    case NETDEV_F_PAUSE_ASYM: return "AUTO_PAUSE_ASYM";
    }

    return NULL;
}

static void
ofp_print_port_features(struct ds *string, enum netdev_features features)
{
    ofp_print_bit_names(string, features, netdev_feature_to_name, ' ');
    ds_put_char(string, '\n');
}

static const char *
ofputil_port_config_to_name(uint32_t bit)
{
    enum ofputil_port_config pc = bit;

    switch (pc) {
    case OFPUTIL_PC_PORT_DOWN:    return "PORT_DOWN";
    case OFPUTIL_PC_NO_STP:       return "NO_STP";
    case OFPUTIL_PC_NO_RECV:      return "NO_RECV";
    case OFPUTIL_PC_NO_RECV_STP:  return "NO_RECV_STP";
    case OFPUTIL_PC_NO_FLOOD:     return "NO_FLOOD";
    case OFPUTIL_PC_NO_FWD:       return "NO_FWD";
    case OFPUTIL_PC_NO_PACKET_IN: return "NO_PACKET_IN";
    }

    return NULL;
}

static void
ofp_print_port_config(struct ds *string, enum ofputil_port_config config)
{
    ofp_print_bit_names(string, config, ofputil_port_config_to_name, ' ');
    ds_put_char(string, '\n');
}

static const char *
ofputil_port_state_to_name(uint32_t bit)
{
    enum ofputil_port_state ps = bit;

    switch (ps) {
    case OFPUTIL_PS_LINK_DOWN: return "LINK_DOWN";
    case OFPUTIL_PS_BLOCKED:   return "BLOCKED";
    case OFPUTIL_PS_LIVE:      return "LIVE";

    case OFPUTIL_PS_STP_LISTEN:
    case OFPUTIL_PS_STP_LEARN:
    case OFPUTIL_PS_STP_FORWARD:
    case OFPUTIL_PS_STP_BLOCK:
        /* Handled elsewhere. */
        return NULL;
    }

    return NULL;
}

static void
ofp_print_port_state(struct ds *string, enum ofputil_port_state state)
{
    enum ofputil_port_state stp_state;

    /* The STP state is a 2-bit field so it doesn't fit in with the bitmask
     * pattern.  We have to special case it.
     *
     * OVS doesn't support STP, so this field will always be 0 if we are
     * talking to OVS, so we'd always print STP_LISTEN in that case.
     * Therefore, we don't print anything at all if the value is STP_LISTEN, to
     * avoid confusing users. */
    stp_state = state & OFPUTIL_PS_STP_MASK;
    if (stp_state) {
        ds_put_cstr(string,
                    (stp_state == OFPUTIL_PS_STP_LEARN ? "STP_LEARN"
                     : stp_state == OFPUTIL_PS_STP_FORWARD ? "STP_FORWARD"
                     : "STP_BLOCK"));
        state &= ~OFPUTIL_PS_STP_MASK;
        if (state) {
            ofp_print_bit_names(string, state, ofputil_port_state_to_name,
                                ' ');
        }
    } else {
        ofp_print_bit_names(string, state, ofputil_port_state_to_name, ' ');
    }
    ds_put_char(string, '\n');
}

static void
ofp_print_phy_port(struct ds *string, const struct ofputil_phy_port *port)
{
    char name[sizeof port->name];
    int j;

    memcpy(name, port->name, sizeof name);
    for (j = 0; j < sizeof name - 1; j++) {
        if (!isprint((unsigned char) name[j])) {
            break;
        }
    }
    name[j] = '\0';

    ds_put_char(string, ' ');
    ofputil_format_port(port->port_no, string);
    ds_put_format(string, "(%s): addr:"ETH_ADDR_FMT"\n",
                  name, ETH_ADDR_ARGS(port->hw_addr));

    ds_put_cstr(string, "     config:     ");
    ofp_print_port_config(string, port->config);

    ds_put_cstr(string, "     state:      ");
    ofp_print_port_state(string, port->state);

    if (port->curr) {
        ds_put_format(string, "     current:    ");
        ofp_print_port_features(string, port->curr);
    }
    if (port->advertised) {
        ds_put_format(string, "     advertised: ");
        ofp_print_port_features(string, port->advertised);
    }
    if (port->supported) {
        ds_put_format(string, "     supported:  ");
        ofp_print_port_features(string, port->supported);
    }
    if (port->peer) {
        ds_put_format(string, "     peer:       ");
        ofp_print_port_features(string, port->peer);
    }
    ds_put_format(string, "     speed: %"PRIu32" Mbps now, "
                  "%"PRIu32" Mbps max\n",
                  port->curr_speed / UINT32_C(1000),
                  port->max_speed / UINT32_C(1000));
}

/* Given a buffer 'b' that contains an array of OpenFlow ports of type
 * 'ofp_version', writes a detailed description of each port into
 * 'string'. */
static void
ofp_print_phy_ports(struct ds *string, uint8_t ofp_version,
                    struct ofpbuf *b)
{
    size_t n_ports;
    struct ofputil_phy_port *ports;
    enum ofperr error;
    size_t i;

    n_ports = ofputil_count_phy_ports(ofp_version, b);

    ports = xmalloc(n_ports * sizeof *ports);
    for (i = 0; i < n_ports; i++) {
        error = ofputil_pull_phy_port(ofp_version, b, &ports[i]);
        if (error) {
            ofp_print_error(string, error);
            goto exit;
        }
    }
    qsort(ports, n_ports, sizeof *ports, compare_ports);
    for (i = 0; i < n_ports; i++) {
        ofp_print_phy_port(string, &ports[i]);
    }

exit:
    free(ports);
}

static const char *
ofputil_capabilities_to_name(uint32_t bit)
{
    enum ofputil_capabilities capabilities = bit;

    switch (capabilities) {
    case OFPUTIL_C_FLOW_STATS:   return "FLOW_STATS";
    case OFPUTIL_C_TABLE_STATS:  return "TABLE_STATS";
    case OFPUTIL_C_PORT_STATS:   return "PORT_STATS";
    case OFPUTIL_C_IP_REASM:     return "IP_REASM";
    case OFPUTIL_C_QUEUE_STATS:  return "QUEUE_STATS";
    case OFPUTIL_C_ARP_MATCH_IP: return "ARP_MATCH_IP";
    case OFPUTIL_C_STP:          return "STP";
    case OFPUTIL_C_GROUP_STATS:  return "GROUP_STATS";
    case OFPUTIL_C_PORT_BLOCKED: return "PORT_BLOCKED";
    }

    return NULL;
}

static const char *
ofputil_action_bitmap_to_name(uint32_t bit)
{
    enum ofputil_action_bitmap action = bit;

    switch (action) {
    case OFPUTIL_A_OUTPUT:         return "OUTPUT";
    case OFPUTIL_A_SET_VLAN_VID:   return "SET_VLAN_VID";
    case OFPUTIL_A_SET_VLAN_PCP:   return "SET_VLAN_PCP";
    case OFPUTIL_A_STRIP_VLAN:     return "STRIP_VLAN";
    case OFPUTIL_A_SET_DL_SRC:     return "SET_DL_SRC";
    case OFPUTIL_A_SET_DL_DST:     return "SET_DL_DST";
    case OFPUTIL_A_SET_NW_SRC:     return "SET_NW_SRC";
    case OFPUTIL_A_SET_NW_DST:     return "SET_NW_DST";
    case OFPUTIL_A_SET_NW_ECN:     return "SET_NW_ECN";
    case OFPUTIL_A_SET_NW_TOS:     return "SET_NW_TOS";
    case OFPUTIL_A_SET_TP_SRC:     return "SET_TP_SRC";
    case OFPUTIL_A_SET_TP_DST:     return "SET_TP_DST";
    case OFPUTIL_A_SET_FIELD:      return "SET_FIELD";
    case OFPUTIL_A_ENQUEUE:        return "ENQUEUE";
    case OFPUTIL_A_COPY_TTL_OUT:   return "COPY_TTL_OUT";
    case OFPUTIL_A_COPY_TTL_IN:    return "COPY_TTL_IN";
    case OFPUTIL_A_SET_MPLS_LABEL: return "SET_MPLS_LABEL";
    case OFPUTIL_A_SET_MPLS_TC:    return "SET_MPLS_TC";
    case OFPUTIL_A_SET_MPLS_TTL:   return "SET_MPLS_TTL";
    case OFPUTIL_A_DEC_MPLS_TTL:   return "DEC_MPLS_TTL";
    case OFPUTIL_A_PUSH_VLAN:      return "PUSH_VLAN";
    case OFPUTIL_A_POP_VLAN:       return "POP_VLAN";
    case OFPUTIL_A_PUSH_MPLS:      return "PUSH_MPLS";
    case OFPUTIL_A_POP_MPLS:       return "POP_MPLS";
    case OFPUTIL_A_SET_QUEUE:      return "SET_QUEUE";
    case OFPUTIL_A_GROUP:          return "GROUP";
    case OFPUTIL_A_SET_NW_TTL:     return "SET_NW_TTL";
    case OFPUTIL_A_DEC_NW_TTL:     return "DEC_NW_TTL";
    }

    return NULL;
}

static void
ofp_print_switch_features(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_switch_features features;
    enum ofperr error;
    struct ofpbuf b;

    error = ofputil_decode_switch_features(oh, &features, &b);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_format(string, " dpid:%016"PRIx64"\n", features.datapath_id);

    ds_put_format(string, "n_tables:%"PRIu8", n_buffers:%"PRIu32,
                  features.n_tables, features.n_buffers);
    if (features.auxiliary_id) {
        ds_put_format(string, ", auxiliary_id:%"PRIu8, features.auxiliary_id);
    }
    ds_put_char(string, '\n');

    ds_put_cstr(string, "capabilities: ");
    ofp_print_bit_names(string, features.capabilities,
                        ofputil_capabilities_to_name, ' ');
    ds_put_char(string, '\n');

    switch ((enum ofp_version)oh->version) {
    case OFP10_VERSION:
        ds_put_cstr(string, "actions: ");
        ofp_print_bit_names(string, features.actions,
                            ofputil_action_bitmap_to_name, ' ');
        ds_put_char(string, '\n');
        break;
    case OFP11_VERSION:
    case OFP12_VERSION:
        break;
    case OFP13_VERSION:
        return; /* no ports in ofp13_switch_features */
    default:
        NOT_REACHED();
    }

    ofp_print_phy_ports(string, oh->version, &b);
}

static void
ofp_print_switch_config(struct ds *string, const struct ofp_switch_config *osc)
{
    enum ofp_config_flags flags;

    flags = ntohs(osc->flags);

    ds_put_format(string, " frags=%s", ofputil_frag_handling_to_string(flags));
    flags &= ~OFPC_FRAG_MASK;

    if (flags & OFPC_INVALID_TTL_TO_CONTROLLER) {
        ds_put_format(string, " invalid_ttl_to_controller");
        flags &= ~OFPC_INVALID_TTL_TO_CONTROLLER;
    }

    if (flags) {
        ds_put_format(string, " ***unknown flags 0x%04"PRIx16"***", flags);
    }

    ds_put_format(string, " miss_send_len=%"PRIu16"\n", ntohs(osc->miss_send_len));
}

static void print_wild(struct ds *string, const char *leader, int is_wild,
            int verbosity, const char *format, ...)
            __attribute__((format(printf, 5, 6)));

static void print_wild(struct ds *string, const char *leader, int is_wild,
                       int verbosity, const char *format, ...)
{
    if (is_wild && verbosity < 2) {
        return;
    }
    ds_put_cstr(string, leader);
    if (!is_wild) {
        va_list args;

        va_start(args, format);
        ds_put_format_valist(string, format, args);
        va_end(args);
    } else {
        ds_put_char(string, '*');
    }
    ds_put_char(string, ',');
}

static void
print_wild_port(struct ds *string, const char *leader, int is_wild,
                int verbosity, uint16_t port)
{
    if (is_wild && verbosity < 2) {
        return;
    }
    ds_put_cstr(string, leader);
    if (!is_wild) {
        ofputil_format_port(port, string);
    } else {
        ds_put_char(string, '*');
    }
    ds_put_char(string, ',');
}

static void
print_ip_netmask(struct ds *string, const char *leader, ovs_be32 ip,
                 uint32_t wild_bits, int verbosity)
{
    if (wild_bits >= 32 && verbosity < 2) {
        return;
    }
    ds_put_cstr(string, leader);
    if (wild_bits < 32) {
        ds_put_format(string, IP_FMT, IP_ARGS(ip));
        if (wild_bits) {
            ds_put_format(string, "/%d", 32 - wild_bits);
        }
    } else {
        ds_put_char(string, '*');
    }
    ds_put_char(string, ',');
}

void
ofp10_match_print(struct ds *f, const struct ofp10_match *om, int verbosity)
{
    char *s = ofp10_match_to_string(om, verbosity);
    ds_put_cstr(f, s);
    free(s);
}

char *
ofp10_match_to_string(const struct ofp10_match *om, int verbosity)
{
    struct ds f = DS_EMPTY_INITIALIZER;
    uint32_t w = ntohl(om->wildcards);
    bool skip_type = false;
    bool skip_proto = false;

    if (!(w & OFPFW10_DL_TYPE)) {
        skip_type = true;
        if (om->dl_type == htons(ETH_TYPE_IP)) {
            if (!(w & OFPFW10_NW_PROTO)) {
                skip_proto = true;
                if (om->nw_proto == IPPROTO_ICMP) {
                    ds_put_cstr(&f, "icmp,");
                } else if (om->nw_proto == IPPROTO_TCP) {
                    ds_put_cstr(&f, "tcp,");
                } else if (om->nw_proto == IPPROTO_UDP) {
                    ds_put_cstr(&f, "udp,");
                } else {
                    ds_put_cstr(&f, "ip,");
                    skip_proto = false;
                }
            } else {
                ds_put_cstr(&f, "ip,");
            }
        } else if (om->dl_type == htons(ETH_TYPE_ARP)) {
            ds_put_cstr(&f, "arp,");
        } else if (om->dl_type == htons(ETH_TYPE_RARP)){
            ds_put_cstr(&f, "rarp,");
        } else if (om->dl_type == htons(ETH_TYPE_MPLS)) {
            ds_put_cstr(&f, "mpls,");
        } else if (om->dl_type == htons(ETH_TYPE_MPLS_MCAST)) {
            ds_put_cstr(&f, "mplsm,");
        } else {
            skip_type = false;
        }
    }
    print_wild_port(&f, "in_port=", w & OFPFW10_IN_PORT, verbosity,
                    ntohs(om->in_port));
    print_wild(&f, "dl_vlan=", w & OFPFW10_DL_VLAN, verbosity,
               "%d", ntohs(om->dl_vlan));
    print_wild(&f, "dl_vlan_pcp=", w & OFPFW10_DL_VLAN_PCP, verbosity,
               "%d", om->dl_vlan_pcp);
    print_wild(&f, "dl_src=", w & OFPFW10_DL_SRC, verbosity,
               ETH_ADDR_FMT, ETH_ADDR_ARGS(om->dl_src));
    print_wild(&f, "dl_dst=", w & OFPFW10_DL_DST, verbosity,
               ETH_ADDR_FMT, ETH_ADDR_ARGS(om->dl_dst));
    if (!skip_type) {
        print_wild(&f, "dl_type=", w & OFPFW10_DL_TYPE, verbosity,
                   "0x%04x", ntohs(om->dl_type));
    }
    print_ip_netmask(&f, "nw_src=", om->nw_src,
                     (w & OFPFW10_NW_SRC_MASK) >> OFPFW10_NW_SRC_SHIFT,
                     verbosity);
    print_ip_netmask(&f, "nw_dst=", om->nw_dst,
                     (w & OFPFW10_NW_DST_MASK) >> OFPFW10_NW_DST_SHIFT,
                     verbosity);
    if (!skip_proto) {
        if (om->dl_type == htons(ETH_TYPE_ARP) ||
            om->dl_type == htons(ETH_TYPE_RARP)) {
            print_wild(&f, "arp_op=", w & OFPFW10_NW_PROTO, verbosity,
                       "%u", om->nw_proto);
        } else {
            print_wild(&f, "nw_proto=", w & OFPFW10_NW_PROTO, verbosity,
                       "%u", om->nw_proto);
        }
    }
    print_wild(&f, "nw_tos=", w & OFPFW10_NW_TOS, verbosity,
               "%u", om->nw_tos);
    if (om->nw_proto == IPPROTO_ICMP) {
        print_wild(&f, "icmp_type=", w & OFPFW10_ICMP_TYPE, verbosity,
                   "%d", ntohs(om->tp_src));
        print_wild(&f, "icmp_code=", w & OFPFW10_ICMP_CODE, verbosity,
                   "%d", ntohs(om->tp_dst));
    } else {
        print_wild(&f, "tp_src=", w & OFPFW10_TP_SRC, verbosity,
                   "%d", ntohs(om->tp_src));
        print_wild(&f, "tp_dst=", w & OFPFW10_TP_DST, verbosity,
                   "%d", ntohs(om->tp_dst));
    }
    if (ds_last(&f) == ',') {
        f.length--;
    }
    return ds_cstr(&f);
}

static void
ofp_print_flow_flags(struct ds *s, uint16_t flags)
{
    if (flags & OFPFF_SEND_FLOW_REM) {
        ds_put_cstr(s, "send_flow_rem ");
    }
    if (flags & OFPFF_CHECK_OVERLAP) {
        ds_put_cstr(s, "check_overlap ");
    }
    if (flags & OFPFF12_RESET_COUNTS) {
        ds_put_cstr(s, "reset_counts ");
    }
    if (flags & OFPFF13_NO_PKT_COUNTS) {
        ds_put_cstr(s, "no_packet_counts ");
    }
    if (flags & OFPFF13_NO_BYT_COUNTS) {
        ds_put_cstr(s, "no_byte_counts ");
    }

    flags &= ~(OFPFF_SEND_FLOW_REM | OFPFF_CHECK_OVERLAP
               | OFPFF12_RESET_COUNTS
               | OFPFF13_NO_PKT_COUNTS | OFPFF13_NO_BYT_COUNTS);
    if (flags) {
        ds_put_format(s, "flags:0x%"PRIx16" ", flags);
    }
}

static void
ofp_print_flow_mod(struct ds *s, const struct ofp_header *oh, int verbosity)
{
    struct ofputil_flow_mod fm;
    struct ofpbuf ofpacts;
    bool need_priority;
    enum ofperr error;
    enum ofpraw raw;
    enum ofputil_protocol protocol;

    protocol = ofputil_protocol_from_ofp_version(oh->version);
    protocol = ofputil_protocol_set_tid(protocol, true);

    ofpbuf_init(&ofpacts, 64);
    error = ofputil_decode_flow_mod(&fm, oh, protocol, &ofpacts);
    if (error) {
        ofpbuf_uninit(&ofpacts);
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, ' ');
    switch (fm.command) {
    case OFPFC_ADD:
        ds_put_cstr(s, "ADD");
        break;
    case OFPFC_MODIFY:
        ds_put_cstr(s, "MOD");
        break;
    case OFPFC_MODIFY_STRICT:
        ds_put_cstr(s, "MOD_STRICT");
        break;
    case OFPFC_DELETE:
        ds_put_cstr(s, "DEL");
        break;
    case OFPFC_DELETE_STRICT:
        ds_put_cstr(s, "DEL_STRICT");
        break;
    default:
        ds_put_format(s, "cmd:%d", fm.command);
    }
    if (fm.table_id != 0) {
        ds_put_format(s, " table:%d", fm.table_id);
    }

    ds_put_char(s, ' ');
    ofpraw_decode(&raw, oh);
    if (verbosity >= 3 && raw == OFPRAW_OFPT10_FLOW_MOD) {
        const struct ofp10_flow_mod *ofm = ofpmsg_body(oh);
        ofp10_match_print(s, &ofm->match, verbosity);

        /* ofp_print_match() doesn't print priority. */
        need_priority = true;
    } else if (verbosity >= 3 && raw == OFPRAW_NXT_FLOW_MOD) {
        const struct nx_flow_mod *nfm = ofpmsg_body(oh);
        const void *nxm = nfm + 1;
        char *nxm_s;

        nxm_s = nx_match_to_string(nxm, ntohs(nfm->match_len));
        ds_put_cstr(s, nxm_s);
        free(nxm_s);

        /* nx_match_to_string() doesn't print priority. */
        need_priority = true;
    } else {
        match_format(&fm.match, s, fm.priority);

        /* match_format() does print priority. */
        need_priority = false;
    }

    if (ds_last(s) != ' ') {
        ds_put_char(s, ' ');
    }
    if (fm.new_cookie != htonll(0) && fm.new_cookie != htonll(UINT64_MAX)) {
        ds_put_format(s, "cookie:0x%"PRIx64" ", ntohll(fm.new_cookie));
    }
    if (fm.cookie_mask != htonll(0)) {
        ds_put_format(s, "cookie:0x%"PRIx64"/0x%"PRIx64" ",
                ntohll(fm.cookie), ntohll(fm.cookie_mask));
    }
    if (fm.idle_timeout != OFP_FLOW_PERMANENT) {
        ds_put_format(s, "idle:%"PRIu16" ", fm.idle_timeout);
    }
    if (fm.hard_timeout != OFP_FLOW_PERMANENT) {
        ds_put_format(s, "hard:%"PRIu16" ", fm.hard_timeout);
    }
    if (fm.priority != OFP_DEFAULT_PRIORITY && need_priority) {
        ds_put_format(s, "pri:%"PRIu16" ", fm.priority);
    }
    if (fm.buffer_id != UINT32_MAX) {
        ds_put_format(s, "buf:0x%"PRIx32" ", fm.buffer_id);
    }
    if (fm.out_port != OFPP_ANY) {
        ds_put_format(s, "out_port:");
        ofputil_format_port(fm.out_port, s);
        ds_put_char(s, ' ');
    }
    if (fm.flags != 0) {
        ofp_print_flow_flags(s, fm.flags);
    }

    ofpacts_format(fm.ofpacts, fm.ofpacts_len, s);
    ofpbuf_uninit(&ofpacts);
}

static void
ofp_print_duration(struct ds *string, unsigned int sec, unsigned int nsec)
{
    ds_put_format(string, "%u", sec);
    if (nsec > 0) {
        ds_put_format(string, ".%09u", nsec);
        while (string->string[string->length - 1] == '0') {
            string->length--;
        }
    }
    ds_put_char(string, 's');
}

static const char *
ofp_flow_removed_reason_to_string(enum ofp_flow_removed_reason reason)
{
    static char s[32];

    switch (reason) {
    case OFPRR_IDLE_TIMEOUT:
        return "idle";
    case OFPRR_HARD_TIMEOUT:
        return "hard";
    case OFPRR_DELETE:
        return "delete";
    case OFPRR_GROUP_DELETE:
        return "group_delete";
    case OFPRR_EVICTION:
        return "eviction";
    default:
        sprintf(s, "%d", (int) reason);
        return s;
    }
}

static void
ofp_print_flow_removed(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_flow_removed fr;
    enum ofperr error;

    error = ofputil_decode_flow_removed(&fr, oh);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_char(string, ' ');
    match_format(&fr.match, string, fr.priority);

    ds_put_format(string, " reason=%s",
                  ofp_flow_removed_reason_to_string(fr.reason));

    if (fr.table_id != 255) {
        ds_put_format(string, " table_id=%"PRIu8, fr.table_id);
    }

    if (fr.cookie != htonll(0)) {
        ds_put_format(string, " cookie:0x%"PRIx64, ntohll(fr.cookie));
    }
    ds_put_cstr(string, " duration");
    ofp_print_duration(string, fr.duration_sec, fr.duration_nsec);
    ds_put_format(string, " idle%"PRIu16, fr.idle_timeout);
    if (fr.hard_timeout) {
        /* The hard timeout was only added in OF1.2, so only print it if it is
         * actually in use to avoid gratuitous change to the formatting. */
        ds_put_format(string, " hard%"PRIu16, fr.hard_timeout);
    }
    ds_put_format(string, " pkts%"PRIu64" bytes%"PRIu64"\n",
                  fr.packet_count, fr.byte_count);
}

static void
ofp_print_port_mod(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_port_mod pm;
    enum ofperr error;

    error = ofputil_decode_port_mod(oh, &pm);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, "port: ");
    ofputil_format_port(pm.port_no, string);
    ds_put_format(string, ": addr:"ETH_ADDR_FMT"\n",
                  ETH_ADDR_ARGS(pm.hw_addr));

    ds_put_cstr(string, "     config: ");
    ofp_print_port_config(string, pm.config);

    ds_put_cstr(string, "     mask:   ");
    ofp_print_port_config(string, pm.mask);

    ds_put_cstr(string, "     advertise: ");
    if (pm.advertise) {
        ofp_print_port_features(string, pm.advertise);
    } else {
        ds_put_cstr(string, "UNCHANGED\n");
    }
}

static void
ofp_print_error(struct ds *string, enum ofperr error)
{
    if (string->length) {
        ds_put_char(string, ' ');
    }
    ds_put_format(string, "***decode error: %s***\n", ofperr_get_name(error));
}

static void
ofp_print_hello(struct ds *string, const struct ofp_header *oh)
{
    uint32_t allowed_versions;
    bool ok;

    ok = ofputil_decode_hello(oh, &allowed_versions);

    ds_put_cstr(string, "\n version bitmap: ");
    ofputil_format_version_bitmap(string, allowed_versions);

    if (!ok) {
        ds_put_cstr(string, "\n unknown data in hello:\n");
        ds_put_hex_dump(string, oh, ntohs(oh->length), 0, true);
    }
}

static void
ofp_print_error_msg(struct ds *string, const struct ofp_header *oh)
{
    size_t len = ntohs(oh->length);
    struct ofpbuf payload;
    enum ofperr error;
    char *s;

    error = ofperr_decode_msg(oh, &payload);
    if (!error) {
        ds_put_cstr(string, "***decode error***");
        ds_put_hex_dump(string, oh + 1, len - sizeof *oh, 0, true);
        return;
    }

    ds_put_format(string, " %s\n", ofperr_get_name(error));

    if (error == OFPERR_OFPHFC_INCOMPATIBLE || error == OFPERR_OFPHFC_EPERM) {
        ds_put_printable(string, payload.data, payload.size);
    } else {
        s = ofp_to_string(payload.data, payload.size, 1);
        ds_put_cstr(string, s);
        free(s);
    }
}

static void
ofp_print_port_status(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_port_status ps;
    enum ofperr error;

    error = ofputil_decode_port_status(oh, &ps);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    if (ps.reason == OFPPR_ADD) {
        ds_put_format(string, " ADD:");
    } else if (ps.reason == OFPPR_DELETE) {
        ds_put_format(string, " DEL:");
    } else if (ps.reason == OFPPR_MODIFY) {
        ds_put_format(string, " MOD:");
    }

    ofp_print_phy_port(string, &ps.desc);
}

static void
ofp_print_ofpst_desc_reply(struct ds *string, const struct ofp_header *oh)
{
    const struct ofp_desc_stats *ods = ofpmsg_body(oh);

    ds_put_char(string, '\n');
    ds_put_format(string, "Manufacturer: %.*s\n",
            (int) sizeof ods->mfr_desc, ods->mfr_desc);
    ds_put_format(string, "Hardware: %.*s\n",
            (int) sizeof ods->hw_desc, ods->hw_desc);
    ds_put_format(string, "Software: %.*s\n",
            (int) sizeof ods->sw_desc, ods->sw_desc);
    ds_put_format(string, "Serial Num: %.*s\n",
            (int) sizeof ods->serial_num, ods->serial_num);
    ds_put_format(string, "DP Description: %.*s\n",
            (int) sizeof ods->dp_desc, ods->dp_desc);
}

static void
ofp_print_flow_stats_request(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_flow_stats_request fsr;
    enum ofperr error;

    error = ofputil_decode_flow_stats_request(&fsr, oh);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    if (fsr.table_id != 0xff) {
        ds_put_format(string, " table=%"PRIu8, fsr.table_id);
    }

    if (fsr.out_port != OFPP_ANY) {
        ds_put_cstr(string, " out_port=");
        ofputil_format_port(fsr.out_port, string);
    }

    ds_put_char(string, ' ');
    match_format(&fsr.match, string, OFP_DEFAULT_PRIORITY);
}

void
ofp_print_flow_stats(struct ds *string, struct ofputil_flow_stats *fs)
{
    ds_put_format(string, " cookie=0x%"PRIx64", duration=",
                  ntohll(fs->cookie));

    ofp_print_duration(string, fs->duration_sec, fs->duration_nsec);
    ds_put_format(string, ", table=%"PRIu8", ", fs->table_id);
    ds_put_format(string, "n_packets=%"PRIu64", ", fs->packet_count);
    ds_put_format(string, "n_bytes=%"PRIu64", ", fs->byte_count);
    if (fs->idle_timeout != OFP_FLOW_PERMANENT) {
        ds_put_format(string, "idle_timeout=%"PRIu16", ", fs->idle_timeout);
    }
    if (fs->hard_timeout != OFP_FLOW_PERMANENT) {
        ds_put_format(string, "hard_timeout=%"PRIu16", ", fs->hard_timeout);
    }
    if (fs->flags) {
        ofp_print_flow_flags(string, fs->flags);
    }
#ifndef _OFP_CENTEC_
    /* idle_age is not defined in spec, we don't support it since it will result poor performance if there are many flows */
    if (fs->idle_age >= 0) {
        ds_put_format(string, "idle_age=%d, ", fs->idle_age);
    }
#endif
    if (fs->hard_age >= 0 && fs->hard_age != fs->duration_sec) {
        ds_put_format(string, "hard_age=%d, ", fs->hard_age);
    }

    match_format(&fs->match, string, fs->priority);
    if (string->string[string->length - 1] != ' ') {
        ds_put_char(string, ' ');
    }

    ofpacts_format(fs->ofpacts, fs->ofpacts_len, string);
}

static void
ofp_print_flow_stats_reply(struct ds *string, const struct ofp_header *oh)
{
    struct ofpbuf ofpacts;
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpbuf_init(&ofpacts, 64);
    for (;;) {
        struct ofputil_flow_stats fs;
        int retval;

        retval = ofputil_decode_flow_stats_reply(&fs, &b, true, &ofpacts);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(string, " ***parse error***");
            }
            break;
        }
        ds_put_char(string, '\n');
        ofp_print_flow_stats(string, &fs);
     }
    ofpbuf_uninit(&ofpacts);
}

static void
ofp_print_aggregate_stats_reply(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_aggregate_stats as;
    enum ofperr error;

    error = ofputil_decode_aggregate_stats_reply(&as, oh);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_format(string, " packet_count=%"PRIu64, as.packet_count);
    ds_put_format(string, " byte_count=%"PRIu64, as.byte_count);
    ds_put_format(string, " flow_count=%"PRIu32, as.flow_count);
}

static void
print_port_stat(struct ds *string, const char *leader, uint64_t stat, int more)
{
    ds_put_cstr(string, leader);
    if (stat != UINT64_MAX) {
        ds_put_format(string, "%"PRIu64, stat);
    } else {
        ds_put_char(string, '?');
    }
    if (more) {
        ds_put_cstr(string, ", ");
    } else {
        ds_put_cstr(string, "\n");
    }
}

static void
ofp_print_ofpst_port_request(struct ds *string, const struct ofp_header *oh)
{
    uint16_t ofp10_port;
    enum ofperr error;

    error = ofputil_decode_port_stats_request(oh, &ofp10_port);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " port_no=");
    ofputil_format_port(ofp10_port, string);
}

static void
ofp_print_ofpst_port_reply(struct ds *string, const struct ofp_header *oh,
                           int verbosity)
{
    struct ofpbuf b;

    ds_put_format(string, " %zu ports\n", ofputil_count_port_stats(oh));
    if (verbosity < 1) {
        return;
    }

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_port_stats ps;
        int retval;

        retval = ofputil_decode_port_stats(&ps, &b);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(string, " ***parse error***");
            }
            return;
        }

        ds_put_cstr(string, "  port ");
        if (ps.port_no < 10) {
            ds_put_char(string, ' ');
        }
        ofputil_format_port(ps.port_no, string);

        ds_put_cstr(string, ": rx ");
        print_port_stat(string, "pkts=", ps.stats.rx_packets, 1);
        print_port_stat(string, "bytes=", ps.stats.rx_bytes, 1);
        print_port_stat(string, "drop=", ps.stats.rx_dropped, 1);
        print_port_stat(string, "errs=", ps.stats.rx_errors, 1);
        print_port_stat(string, "frame=", ps.stats.rx_frame_errors, 1);
        print_port_stat(string, "over=", ps.stats.rx_over_errors, 1);
        print_port_stat(string, "crc=", ps.stats.rx_crc_errors, 0);

        ds_put_cstr(string, "           tx ");
        print_port_stat(string, "pkts=", ps.stats.tx_packets, 1);
        print_port_stat(string, "bytes=", ps.stats.tx_bytes, 1);
        print_port_stat(string, "drop=", ps.stats.tx_dropped, 1);
        print_port_stat(string, "errs=", ps.stats.tx_errors, 1);
        print_port_stat(string, "coll=", ps.stats.collisions, 0);
    }
}

static void
ofp_print_one_ofpst_table_reply(struct ds *string, enum ofp_version ofp_version,
                                const char *name, struct ofp12_table_stats *ts)
{
    char name_[OFP_MAX_TABLE_NAME_LEN + 1];

    /* ofp13_table_stats is different */
    if (ofp_version > OFP12_VERSION) {
        return;
    }

    ovs_strlcpy(name_, name, sizeof name_);

    ds_put_format(string, "  %d: %-8s: ", ts->table_id, name_);
    ds_put_format(string, "wild=0x%05"PRIx64", ", ntohll(ts->wildcards));
    ds_put_format(string, "max=%6"PRIu32", ", ntohl(ts->max_entries));
    ds_put_format(string, "active=%"PRIu32"\n", ntohl(ts->active_count));
    ds_put_cstr(string, "               ");
    ds_put_format(string, "lookup=%"PRIu64", ", ntohll(ts->lookup_count));
    ds_put_format(string, "matched=%"PRIu64"\n", ntohll(ts->matched_count));

    if (ofp_version < OFP11_VERSION) {
        return;
    }

    ds_put_cstr(string, "               ");
    ds_put_format(string, "match=0x%08"PRIx64", ", ntohll(ts->match));
    ds_put_format(string, "instructions=0x%08"PRIx32", ",
                  ntohl(ts->instructions));
    ds_put_format(string, "config=0x%08"PRIx32"\n", ntohl(ts->config));
    ds_put_cstr(string, "               ");
    ds_put_format(string, "write_actions=0x%08"PRIx32", ",
                  ntohl(ts->write_actions));
    ds_put_format(string, "apply_actions=0x%08"PRIx32"\n",
                  ntohl(ts->apply_actions));

    if (ofp_version < OFP12_VERSION) {
        return;
    }

    ds_put_cstr(string, "               ");
    ds_put_format(string, "write_setfields=0x%016"PRIx64"\n",
                  ntohll(ts->write_setfields));
    ds_put_cstr(string, "               ");
    ds_put_format(string, "apply_setfields=0x%016"PRIx64"\n",
                  ntohll(ts->apply_setfields));
    ds_put_cstr(string, "               ");
    ds_put_format(string, "metadata_match=0x%016"PRIx64"\n",
                  ntohll(ts->metadata_match));
    ds_put_cstr(string, "               ");
    ds_put_format(string, "metadata_write=0x%016"PRIx64"\n",
                  ntohll(ts->metadata_write));
}

static void
ofp_print_ofpst_table_reply13(struct ds *string, const struct ofp_header *oh,
                              int verbosity)
{
    struct ofp13_table_stats *ts;
    struct ofpbuf b;
    size_t n;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpraw_pull_assert(&b);

    n = b.size / sizeof *ts;
    ds_put_format(string, " %zu tables\n", n);
    if (verbosity < 1) {
        return;
    }

    for (;;) {
        ts = ofpbuf_try_pull(&b, sizeof *ts);
        if (!ts) {
            return;
        }
        ds_put_format(string,
                      "  %d: active=%"PRIu32", lookup=%"PRIu64  \
                      ", matched=%"PRIu64"\n",
                      ts->table_id, ntohl(ts->active_count),
                      ntohll(ts->lookup_count), ntohll(ts->matched_count));
    }
}

static void
ofp_print_ofpst_table_reply12(struct ds *string, const struct ofp_header *oh,
                              int verbosity)
{
    struct ofp12_table_stats *ts;
    struct ofpbuf b;
    size_t n;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpraw_pull_assert(&b);

    n = b.size / sizeof *ts;
    ds_put_format(string, " %zu tables\n", n);
    if (verbosity < 1) {
        return;
    }

    for (;;) {
        ts = ofpbuf_try_pull(&b, sizeof *ts);
        if (!ts) {
            return;
        }

        ofp_print_one_ofpst_table_reply(string, OFP12_VERSION, ts->name, ts);
     }
}

static void
ofp_print_ofpst_table_reply11(struct ds *string, const struct ofp_header *oh,
                              int verbosity)
{
    struct ofp11_table_stats *ts;
    struct ofpbuf b;
    size_t n;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpraw_pull_assert(&b);

    n = b.size / sizeof *ts;
    ds_put_format(string, " %zu tables\n", n);
    if (verbosity < 1) {
        return;
    }

    for (;;) {
        struct ofp12_table_stats ts12;

        ts = ofpbuf_try_pull(&b, sizeof *ts);
        if (!ts) {
            return;
        }

        ts12.table_id = ts->table_id;
        ts12.wildcards = htonll(ntohl(ts->wildcards));
        ts12.max_entries = ts->max_entries;
        ts12.active_count = ts->active_count;
        ts12.lookup_count = ts->lookup_count;
        ts12.matched_count = ts->matched_count;
        ts12.match = htonll(ntohl(ts->match));
        ts12.instructions = ts->instructions;
        ts12.config = ts->config;
        ts12.write_actions = ts->write_actions;
        ts12.apply_actions = ts->apply_actions;
        ofp_print_one_ofpst_table_reply(string, OFP11_VERSION, ts->name, &ts12);
     }
}

static void
ofp_print_ofpst_table_reply10(struct ds *string, const struct ofp_header *oh,
                              int verbosity)
{
    struct ofp10_table_stats *ts;
    struct ofpbuf b;
    size_t n;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpraw_pull_assert(&b);

    n = b.size / sizeof *ts;
    ds_put_format(string, " %zu tables\n", n);
    if (verbosity < 1) {
        return;
    }

    for (;;) {
        struct ofp12_table_stats ts12;

        ts = ofpbuf_try_pull(&b, sizeof *ts);
        if (!ts) {
            return;
        }

        ts12.table_id = ts->table_id;
        ts12.wildcards = htonll(ntohl(ts->wildcards));
        ts12.max_entries = ts->max_entries;
        ts12.active_count = ts->active_count;
        ts12.lookup_count = get_32aligned_be64(&ts->lookup_count);
        ts12.matched_count = get_32aligned_be64(&ts->matched_count);
        ofp_print_one_ofpst_table_reply(string, OFP10_VERSION, ts->name, &ts12);
     }
}

static void
ofp_print_ofpst_table_reply(struct ds *string, const struct ofp_header *oh,
                            int verbosity)
{
    switch ((enum ofp_version)oh->version) {
    case OFP13_VERSION:
        ofp_print_ofpst_table_reply13(string, oh, verbosity);
        break;

    case OFP12_VERSION:
        ofp_print_ofpst_table_reply12(string, oh, verbosity);
        break;

    case OFP11_VERSION:
        ofp_print_ofpst_table_reply11(string, oh, verbosity);
        break;

    case OFP10_VERSION:
        ofp_print_ofpst_table_reply10(string, oh, verbosity);
        break;

    default:
        NOT_REACHED();
    }
}

static void
ofp_print_queue_name(struct ds *string, uint32_t queue_id)
{
    if (queue_id == OFPQ_ALL) {
        ds_put_cstr(string, "ALL");
    } else {
        ds_put_format(string, "%"PRIu32, queue_id);
    }
}

static void
ofp_print_ofpst_queue_request(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_queue_stats_request oqsr;
    enum ofperr error;

    error = ofputil_decode_queue_stats_request(oh, &oqsr);
    if (error) {
        ds_put_format(string, "***decode error: %s***\n", ofperr_get_name(error));
        return;
    }

    ds_put_cstr(string, "port=");
    ofputil_format_port(oqsr.port_no, string);

    ds_put_cstr(string, " queue=");
    ofp_print_queue_name(string, oqsr.queue_id);
}

static void
ofp_print_ofpst_queue_reply(struct ds *string, const struct ofp_header *oh,
                            int verbosity)
{
    struct ofpbuf b;

    ds_put_format(string, " %zu queues\n", ofputil_count_queue_stats(oh));
    if (verbosity < 1) {
        return;
    }

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_queue_stats qs;
        int retval;

        retval = ofputil_decode_queue_stats(&qs, &b);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(string, " ***parse error***");
            }
            return;
        }

        ds_put_cstr(string, "  port ");
        ofputil_format_port(qs.port_no, string);
        ds_put_cstr(string, " queue ");
        ofp_print_queue_name(string, qs.queue_id);
        ds_put_cstr(string, ": ");

#ifdef _OFP_CENTEC_
        if (OFP13_VERSION == (enum ofp_version)oh->version) {
            ds_put_format(string, "duration=");
            ofp_print_duration(string, qs.stats.duration_sec,qs.stats.duration_nsec);
        }
#endif /* _OFP_CENTEC_ */    

        print_port_stat(string, ", bytes=", qs.stats.tx_bytes, 1);
        print_port_stat(string, "pkts=", qs.stats.tx_packets, 1);
        print_port_stat(string, "errors=", qs.stats.tx_errors, 0);
    }
}

static void
ofp_print_ofpst_port_desc_reply(struct ds *string,
                                const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpraw_pull_assert(&b);
    ds_put_char(string, '\n');
    ofp_print_phy_ports(string, oh->version, &b);
}

static void
ofp_print_stats_request(struct ds *string, const struct ofp_header *oh)
{
    uint16_t flags = ofpmp_flags(oh);

    if (flags) {
        ds_put_format(string, " ***unknown flags 0x%04"PRIx16"***", flags);
    }
}

static void
ofp_print_stats_reply(struct ds *string, const struct ofp_header *oh)
{
    uint16_t flags = ofpmp_flags(oh);

    if (flags) {
        ds_put_cstr(string, " flags=");
        if (flags & OFPSF_REPLY_MORE) {
            ds_put_cstr(string, "[more]");
            flags &= ~OFPSF_REPLY_MORE;
        }
        if (flags) {
            ds_put_format(string, "[***unknown flags 0x%04"PRIx16"***]",
                          flags);
        }
    }
}

static void
ofp_print_echo(struct ds *string, const struct ofp_header *oh, int verbosity)
{
    size_t len = ntohs(oh->length);

    ds_put_format(string, " %zu bytes of payload\n", len - sizeof *oh);
    if (verbosity > 1) {
        ds_put_hex_dump(string, oh + 1, len - sizeof *oh, 0, true);
    }
}

static void
ofp_print_role_message(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_role_request rr;
    enum ofperr error;

    error = ofputil_decode_role_message(oh, &rr);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " role=");

    switch (rr.role) {
    case OFPCR12_ROLE_NOCHANGE:
        ds_put_cstr(string, "nochange");
        break;
    case OFPCR12_ROLE_EQUAL:
        ds_put_cstr(string, "equal"); /* OF 1.2 wording */
        break;
    case OFPCR12_ROLE_MASTER:
        ds_put_cstr(string, "master");
        break;
    case OFPCR12_ROLE_SLAVE:
        ds_put_cstr(string, "slave");
        break;
    default:
        NOT_REACHED();
    }

    if (rr.have_generation_id) {
        ds_put_format(string, " generation_id=%"PRIu64, rr.generation_id);
    }
}

static void
ofp_print_nxt_flow_mod_table_id(struct ds *string,
                                const struct nx_flow_mod_table_id *nfmti)
{
    ds_put_format(string, " %s", nfmti->set ? "enable" : "disable");
}

static void
ofp_print_nxt_set_flow_format(struct ds *string,
                              const struct nx_set_flow_format *nsff)
{
    uint32_t format = ntohl(nsff->format);

    ds_put_cstr(string, " format=");
    if (ofputil_nx_flow_format_is_valid(format)) {
        ds_put_cstr(string, ofputil_nx_flow_format_to_string(format));
    } else {
        ds_put_format(string, "%"PRIu32, format);
    }
}

static void
ofp_print_nxt_set_packet_in_format(struct ds *string,
                                   const struct nx_set_packet_in_format *nspf)
{
    uint32_t format = ntohl(nspf->format);

    ds_put_cstr(string, " format=");
    if (ofputil_packet_in_format_is_valid(format)) {
        ds_put_cstr(string, ofputil_packet_in_format_to_string(format));
    } else {
        ds_put_format(string, "%"PRIu32, format);
    }
}

static const char *
ofp_port_reason_to_string(enum ofp_port_reason reason)
{
    static char s[32];

    switch (reason) {
    case OFPPR_ADD:
        return "add";

    case OFPPR_DELETE:
        return "delete";

    case OFPPR_MODIFY:
        return "modify";

    default:
        sprintf(s, "%d", (int) reason);
        return s;
    }
}

static void
ofp_print_nxt_set_async_config(struct ds *string,
                               const struct nx_async_config *nac)
{
    int i;

    for (i = 0; i < 2; i++) {
        int j;

        ds_put_format(string, "\n %s:\n", i == 0 ? "master" : "slave");

        ds_put_cstr(string, "       PACKET_IN:");
        for (j = 0; j < 32; j++) {
            if (nac->packet_in_mask[i] & htonl(1u << j)) {
                ds_put_format(string, " %s",
                              ofputil_packet_in_reason_to_string(j));
            }
        }
        if (!nac->packet_in_mask[i]) {
            ds_put_cstr(string, " (off)");
        }
        ds_put_char(string, '\n');

        ds_put_cstr(string, "     PORT_STATUS:");
        for (j = 0; j < 32; j++) {
            if (nac->port_status_mask[i] & htonl(1u << j)) {
                ds_put_format(string, " %s", ofp_port_reason_to_string(j));
            }
        }
        if (!nac->port_status_mask[i]) {
            ds_put_cstr(string, " (off)");
        }
        ds_put_char(string, '\n');

        ds_put_cstr(string, "    FLOW_REMOVED:");
        for (j = 0; j < 32; j++) {
            if (nac->flow_removed_mask[i] & htonl(1u << j)) {
                ds_put_format(string, " %s",
                              ofp_flow_removed_reason_to_string(j));
            }
        }
        if (!nac->flow_removed_mask[i]) {
            ds_put_cstr(string, " (off)");
        }
        ds_put_char(string, '\n');
    }
}

static void
ofp_print_nxt_set_controller_id(struct ds *string,
                                const struct nx_controller_id *nci)
{
    ds_put_format(string, " id=%"PRIu16, ntohs(nci->controller_id));
}

static void
ofp_print_nxt_flow_monitor_cancel(struct ds *string,
                                  const struct ofp_header *oh)
{
    ds_put_format(string, " id=%"PRIu32,
                  ofputil_decode_flow_monitor_cancel(oh));
}

static const char *
nx_flow_monitor_flags_to_name(uint32_t bit)
{
    enum nx_flow_monitor_flags fmf = bit;

    switch (fmf) {
    case NXFMF_INITIAL: return "initial";
    case NXFMF_ADD: return "add";
    case NXFMF_DELETE: return "delete";
    case NXFMF_MODIFY: return "modify";
    case NXFMF_ACTIONS: return "actions";
    case NXFMF_OWN: return "own";
    }

    return NULL;
}

static void
ofp_print_nxst_flow_monitor_request(struct ds *string,
                                    const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_flow_monitor_request request;
        int retval;

        retval = ofputil_decode_flow_monitor_request(&request, &b);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(string, retval);
            }
            return;
        }

        ds_put_format(string, "\n id=%"PRIu32" flags=", request.id);
        ofp_print_bit_names(string, request.flags,
                            nx_flow_monitor_flags_to_name, ',');

        if (request.out_port != OFPP_NONE) {
            ds_put_cstr(string, " out_port=");
            ofputil_format_port(request.out_port, string);
        }

        if (request.table_id != 0xff) {
            ds_put_format(string, " table=%"PRIu8, request.table_id);
        }

        ds_put_char(string, ' ');
        match_format(&request.match, string, OFP_DEFAULT_PRIORITY);
        ds_chomp(string, ' ');
    }
}

static void
ofp_print_nxst_flow_monitor_reply(struct ds *string,
                                  const struct ofp_header *oh)
{
    uint64_t ofpacts_stub[1024 / 8];
    struct ofpbuf ofpacts;
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpbuf_use_stub(&ofpacts, ofpacts_stub, sizeof ofpacts_stub);
    for (;;) {
        struct ofputil_flow_update update;
        struct match match;
        int retval;

        update.match = &match;
        retval = ofputil_decode_flow_update(&update, &b, &ofpacts);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(string, retval);
            }
            ofpbuf_uninit(&ofpacts);
            return;
        }

        ds_put_cstr(string, "\n event=");
        switch (update.event) {
        case NXFME_ADDED:
            ds_put_cstr(string, "ADDED");
            break;

        case NXFME_DELETED:
            ds_put_format(string, "DELETED reason=%s",
                          ofp_flow_removed_reason_to_string(update.reason));
            break;

        case NXFME_MODIFIED:
            ds_put_cstr(string, "MODIFIED");
            break;

        case NXFME_ABBREV:
            ds_put_format(string, "ABBREV xid=0x%"PRIx32, ntohl(update.xid));
            continue;
        }

        ds_put_format(string, " table=%"PRIu8, update.table_id);
        if (update.idle_timeout != OFP_FLOW_PERMANENT) {
            ds_put_format(string, " idle_timeout=%"PRIu16,
                          update.idle_timeout);
        }
        if (update.hard_timeout != OFP_FLOW_PERMANENT) {
            ds_put_format(string, " hard_timeout=%"PRIu16,
                          update.hard_timeout);
        }
        ds_put_format(string, " cookie=%#"PRIx64, ntohll(update.cookie));

        ds_put_char(string, ' ');
        match_format(update.match, string, OFP_DEFAULT_PRIORITY);

        if (update.ofpacts_len) {
            if (string->string[string->length - 1] != ' ') {
                ds_put_char(string, ' ');
            }
            ofpacts_format(update.ofpacts, update.ofpacts_len, string);
        }
    }
}

void
ofp_print_version(const struct ofp_header *oh,
                  struct ds *string)
{
    switch (oh->version) {
    case OFP10_VERSION:
        break;
    case OFP11_VERSION:
        ds_put_cstr(string, " (OF1.1)");
        break;
    case OFP12_VERSION:
        ds_put_cstr(string, " (OF1.2)");
        break;
    case OFP13_VERSION:
        ds_put_cstr(string, " (OF1.3)");
        break;
    default:
        ds_put_format(string, " (OF 0x%02"PRIx8")", oh->version);
        break;
    }
    ds_put_format(string, " (xid=0x%"PRIx32"):", ntohl(oh->xid));
}

static void
ofp_header_to_string__(const struct ofp_header *oh, enum ofpraw raw,
                       struct ds *string)
{
    ds_put_cstr(string, ofpraw_get_name(raw));
    ofp_print_version(oh, string);
}

static void
ofp_print_not_implemented(struct ds *string)
{
    ds_put_cstr(string, "NOT IMPLEMENTED YET!\n");
}

#ifdef _OFP_CENTEC_
extern void ofpact_format(const struct ofpact *a, struct ds *s);

static void
ofp_print_group(struct ds *s, uint32_t group_id, uint8_t type, struct list* p_buckets)
{
    struct ofputil_bucket *bucket;
    char* type_str[] ={"ALL", "SELECT", "INDIRECT", "FF", "UNKNOW"};

    ds_put_format(s, "group_id=%"PRIu32",type=%s", group_id, type_str[type>3?4:type]);

    if (!p_buckets) {
        return;
    }

    LIST_FOR_EACH (bucket, list_node, p_buckets) {
        const struct ofpact *a;

        ds_put_cstr(s, ",bucket=");

        if (bucket->weight != 1) {
            ds_put_format(s, "weight:%"PRIu16",", bucket->weight);
        }
        if (bucket->watch_port != OFPP_NONE) {
            ds_put_format(s, "watch_port:%"PRIu32",", bucket->watch_port);
        }
        if (bucket->watch_group != OFPG11_ANY) {
            ds_put_format(s, "watch_group:%"PRIu32",", bucket->watch_group);
        }
    
        OFPACT_FOR_EACH (a, bucket->ofpacts, bucket->ofpacts_len) {
            if (a != bucket->ofpacts) {
                ds_put_cstr(s, ",");
            }

            /* XXX write-actions */
            ofpact_format(a, s);
        }
    }
}

static void
ofp_print_group_desc(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf b;
    enum ofpraw raw;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    raw = ofpraw_pull_assert(&b);
    
    if (raw != OFPRAW_OFPST11_GROUP_DESC_REPLY) {
        ds_put_cstr(s, " ***error reply type***");
        return;
    }

    for (;;) {
        struct ofputil_group_desc gd;
        int retval;

        memset(&gd, 0, sizeof(gd));
        retval = ofputil_decode_group_desc_reply(&gd, &b);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(s, " ***parse error***");
            }
            break;
        }
        
        ds_put_char(s, '\n');
        ds_put_char(s, ' ');
        ofp_print_group(s, gd.group_id, gd.type, &gd.buckets);
     }
}

static void
ofp_print_group_stats(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf b;
    enum ofpraw raw;
    size_t bucket_i;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    raw = ofpraw_pull_assert(&b);
    
    if (raw != OFPRAW_OFPST11_GROUP_REPLY && 
        raw != OFPRAW_OFPST13_GROUP_REPLY) {
        ds_put_cstr(s, " ***error group reply type***");
        return;
    }

    for (;;) {
        struct ofputil_group_stats gs;
        int retval;

        memset(&gs, 0, sizeof(gs));
        retval = ofputil_decode_group_stats_reply(raw, &gs, &b);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(s, " ***parse error***");
            }
            break;
        }
        
        ds_put_char(s, '\n');
        
        ds_put_char(s, ' ');
        ds_put_format(s, "group_id=%"PRIu32",", gs.group_id);

        if (oh->version >= OFP13_VERSION) {
            ds_put_cstr(s, "duration=");
            ofp_print_duration(s, gs.duration_sec, gs.duration_nsec);
        }
        ds_put_format(s, ",ref_count=%"PRIu32",", gs.ref_count);
        ds_put_format(s, "packet_count=%"PRIu64",", gs.packet_count);
        ds_put_format(s, "byte_count=%"PRIu64"", gs.byte_count);
        
        for (bucket_i = 0; bucket_i < gs.n_buckets; bucket_i++) {
            if (ntohll(gs.bucket_stats[bucket_i].packet_count) != UINT64_MAX) {
                ds_put_format(s, ",bucket%"PRIu32":", bucket_i);
                ds_put_format(s, "packet_count=%"PRIu64",", gs.bucket_stats[bucket_i].packet_count);
                ds_put_format(s, "byte_count=%"PRIu64"", gs.bucket_stats[bucket_i].byte_count);
            }
        }
     }
}

static void
ofp_print_group_features(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_group_features features;
    enum ofperr error;
    struct ofpbuf b;

    error = ofputil_decode_group_features_reply(oh, &features, &b);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_format(string, "\n Group table:\n");
    ds_put_format(string, "    Types:  0x%"PRIx32"\n", features.types);
    ds_put_format(string, "    Capabilities:  0x%"PRIx32"\n", features.capabilities);

    if (features.types & (1u << OFPGT11_ALL)) {
        ds_put_format(string, "    All group :\n");
        ds_put_format(string, "        max_groups = 0x%"PRIx32" actions=0x%08"PRIx32"\n",
            features.max_groups[0], features.actions[0]);
    }

    if (features.types & (1u << OFPGT11_SELECT)) {
        ds_put_format(string, "    Select group :\n");
        ds_put_format(string, "        max_groups = 0x%"PRIx32" actions=0x%08"PRIx32"\n", 
            features.max_groups[1], features.actions[1]);
    }

    if (features.types & (1u << OFPGT11_INDIRECT)) {
        ds_put_format(string, "    Indirect group :\n");
        ds_put_format(string, "        max_groups = 0x%"PRIx32" actions=0x%08"PRIx32"\n", 
            features.max_groups[2], features.actions[2]);
    }

    if (features.types & (1u << OFPGT11_FF)) {
        ds_put_format(string, "    Fast Failover group : \n");
        ds_put_format(string, "        max_groups = 0x%"PRIx32" actions=0x%08"PRIx32"\n", 
            features.max_groups[3], features.actions[3]);
    }
}

static void
ofp_print_table_features(struct ds *string, const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_table_features otf;
        int retval;

        retval = ofputil_decode_table_feature_reply(&otf, &b);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(string, " ***parse error***");
            }
            return;
        }

        ds_put_format(string, "\n Table ID = %"PRIu8"\n", otf.table_id);
        ds_put_format(string, " Table name = %s\n", otf.name);
        ds_put_format(string, " metadata_match = 0x%"PRIx64"  metadata_write = 0x%"PRIx64"\n",
            otf.metadata_match, otf.metadata_write);
        ds_put_format(string, " config = 0x%"PRIx32" max_entries = %"PRIu32"\n", 
            otf.config, otf.max_entries);
    }
}

static void
ofp_print_group_mod(struct ds *s, const struct ofp_header *oh)
{
    struct ofputil_group_mod gm;
    int error;

    memset(&gm, 0, sizeof(gm));
    gm.buckets = malloc(sizeof(struct list));
    list_init(gm.buckets);
    error = ofputil_decode_group_mod(oh, &gm);
    if (error) {
        free(gm.buckets);
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, '\n');

    ds_put_char(s, ' ');
    switch (gm.command) {
    case OFPGC11_ADD:
        ds_put_cstr(s, "ADD");
        break;
        
    case OFPGC11_MODIFY:
        ds_put_cstr(s, "MOD");
        break;
        
    case OFPGC11_DELETE:
        ds_put_cstr(s, "DEL");
        break;
        
    default:
        ds_put_format(s, "cmd:%"PRIu16"", gm.command);
    }
    ds_put_char(s, ' ');

    ofp_print_group(s, gm.group_id, gm.type, gm.buckets);
    free(gm.buckets);
}

/* Meter */
static void
ofp_print_meter_flags(struct ds *s, uint16_t flags)
{
    if (flags & OFPMF13_KBPS) {
        ds_put_cstr(s, "KBPS");
    }
    if (flags & OFPMF13_PKTPS) {
        ds_put_cstr(s, "-PKTPS");
    }
    if (flags & OFPMF13_BURST) {
        ds_put_cstr(s, "-BURST");
    }
    if (flags & OFPMF13_STATS) {
        ds_put_cstr(s, "-STATS");
    }
}

static void
ofp_print_meter_mod(struct ds *s, const struct ofp_header *oh)
{
    size_t band_i;
    struct ofputil_meter_mod mm;
    int error;

    memset(&mm, 0, sizeof(mm));
    error = ofputil_decode_meter_mod(&mm, oh);
    if (error) {
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, '\n');

    ds_put_char(s, ' ');
    switch (mm.command) {
    case OFPMC13_ADD:
        ds_put_cstr(s, "ADD");
        break;

    case OFPMC13_MODIFY:
        ds_put_cstr(s, "MOD");
        break;

    case OFPMC13_DELETE:
        ds_put_cstr(s, "DEL");
        break;

    default:
        ds_put_format(s, "cmd:%"PRIu16"", mm.command);
    }
    ds_put_char(s, ' ');

    ds_put_format(s, "meter_id=%"PRIu32"", mm.meter_id);
    ds_put_format(s, ",flags=");
    ofp_print_meter_flags(s, mm.flags);

    for (band_i = 0; band_i < mm.n_bands; band_i++) {
        ds_put_format(s, ",band_type=%s,", ntohs(mm.bands[band_i].type) == OFPMBT13_DROP ? "DROP" : "KNOWN");
        ds_put_format(s, "rate=%"PRIu32"", ntohl(mm.bands[band_i].rate));

        if (mm.flags & OFPMF13_BURST) {
            ds_put_format(s, ",burst=%"PRIu32"", ntohl(mm.bands[band_i].burst_size));
        }
    }
}

static void
ofp_print_meter_request(struct ds *s, const struct ofp_header *oh)
{
    struct ofputil_meter_request omr;
    enum ofperr error;

    error = ofputil_decode_meter_request(&omr, oh);
    if (error) {
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, ' ');
    ds_put_cstr(s, "METER_STATS");
    ds_put_char(s, ' ');
    if (OFPM13_ALL == omr.meter_id) {
        ds_put_format(s, "meter_id=all");
    } else {
        ds_put_format(s, "meter_id=%"PRIu32"", omr.meter_id);
    }
}

static void
ofp_print_meter_reply(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf b;
    size_t band_i;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_meter_stats_reply omsr;
        enum ofperr error;

        error = ofputil_decode_meter_reply(&omsr, &b);
        if (error) {
            if (error != EOF) {
                ofp_print_error(s, error);
            }
            break;
        }
        ds_put_char(s, '\n');

        ds_put_char(s, ' ');
        ds_put_format(s, "meter_id=%"PRIu32",", omsr.meter_id);

        ds_put_cstr(s, "duration=");
        ofp_print_duration(s, omsr.duration_sec, omsr.duration_nsec);
        ds_put_format(s, ",flow_count=%"PRIu32",", omsr.flow_count);
        ds_put_format(s, "packet_in_count=%"PRIu64",", omsr.packet_in_count);
        ds_put_format(s, "byte_in_count=%"PRIu64",", omsr.byte_in_count);

        for (band_i = 0; band_i < omsr.n_bands; band_i++) {
            ds_put_format(s, "packet_band_count=%"PRIu64",", ntohll(omsr.bands_stats[band_i].packet_band_count));
            ds_put_format(s, "byte_band_count=%"PRIu64"", ntohll(omsr.bands_stats[band_i].byte_band_count));
        }
    }
}

static void
ofp_print_meter_config_request(struct ds *s, const struct ofp_header *oh)
{
    struct ofputil_meter_request omr;
    enum ofperr error;

    error = ofputil_decode_meter_request(&omr, oh);
    if (error) {
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, ' ');
    ds_put_cstr(s, "METER_CONFIG");
    ds_put_char(s, ' ');
    if (OFPM13_ALL == omr.meter_id) {
        ds_put_format(s, "meter_id=all");
    } else {
        ds_put_format(s, "meter_id=%"PRIu32"", omr.meter_id);
    }
}

static void
ofp_print_meter_config_reply(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf b;
    size_t band_i;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_meter_config_reply omcr;
        enum ofperr error;

        error = ofputil_decode_meter_config_reply(&omcr, &b);
        if (error) {
            if (error != EOF) {
                ofp_print_error(s, error);
            }
            break;
        }

        ds_put_char(s, '\n');
        ds_put_char(s, ' ');
        ds_put_format(s, "meter_id=%"PRIu32",flags=", omcr.meter_id);
        ofp_print_meter_flags(s, omcr.flags);
        ds_put_char(s, ',');

        for (band_i = 0; band_i < omcr.n_bands; band_i++) {
            ds_put_format(s, "band_type=%s,", ntohs(omcr.bands[band_i].type) == OFPMBT13_DROP ? "DROP" : "KNOWN");
            ds_put_format(s, "rate=%"PRIu32",", ntohl(omcr.bands[band_i].rate));
            ds_put_format(s, "burst=%"PRIu32"", ntohl(omcr.bands[band_i].burst_size));
        }
    }
}

static void
ofp_print_meter_feature_reply(struct ds *s, const struct ofp13_meter_features *nmf)
{
    ds_put_char(s, '\n');

    ds_put_format(s, "max_meter=%"PRIu32",", ntohl(nmf->max_meter));
    ds_put_format(s, " band_types=%s\n", ntohl(nmf->band_types) == OFPMBT13_DROP ? "DROP" : "KNOWN");
    ds_put_cstr(s,   "capabilities=");
    ofp_print_meter_flags(s, ntohl(nmf->capabilities));

    ds_put_char(s, '\n');
    ds_put_format(s, "max_bands=%"PRIu8",", nmf->max_bands);
    ds_put_format(s, " max_color=%"PRIu32"\n", nmf->max_color);
}

static size_t
ofputil_count_queue_config(const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpraw_pull_assert(&b);

    return (b.size / (sizeof(struct ofputil_queue_get_config_reply) 
        + (sizeof (struct ofp_packet_queue)  + sizeof(struct ofp_queue_prop_header) * 2) 
        * OFUTIL_MAX_QUEUE_VALUE) )* OFUTIL_MAX_QUEUE_VALUE;
}

static void
ofp_print_queue_config_reply(struct ds *string, const struct ofp_header *oh,
                            int verbosity)
{
    struct ofpbuf b;
    struct ofputil_queue_config_s  ofp_queue_config;
    int qid = 0;
    
    ds_put_format(string, " %zu queues\n", ofputil_count_queue_config(oh));
    if (verbosity < 1) {
        return;
    }

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        int retval;

        retval = ofputil_decode_queue_config(&ofp_queue_config, &b);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(string, " ***parse error***");
            }
            return;
        }

        for (qid =0; qid < 8; qid++) {
            ds_put_cstr(string, "  port ");
            ofputil_format_port(ofp_queue_config.port_id, string);
            ds_put_cstr(string, " queue ");
            ofp_print_queue_name(string, qid);
            ds_put_cstr(string, ": ");

            if (ofp_queue_config.min_rate[qid] > 100) {
                ds_put_cstr(string, " min=?%, ");
            } else {
                ds_put_format(string, " min=%"PRIu16, ofp_queue_config.min_rate[qid]);
                ds_put_cstr(string, "%, ");
            }

            if (ofp_queue_config.max_rate[qid] > 100) {
                ds_put_cstr(string, " max=?%\n");
            } else {
                ds_put_format(string, " max=%"PRIu16, ofp_queue_config.max_rate[qid]);
                ds_put_cstr(string, "%\n");
            }
        }
    }
}

#endif

static void
ofp_to_string__(const struct ofp_header *oh, enum ofpraw raw,
                struct ds *string, int verbosity)
{
    const void *msg = oh;

    ofp_header_to_string__(oh, raw, string);
    switch (ofptype_from_ofpraw(raw)) {

        /* FIXME: Change the following once they are implemented: */
#ifdef _OFP_CENTEC_
    case OFPTYPE_QUEUE_GET_CONFIG_REQUEST:
        break;
    case OFPTYPE_QUEUE_GET_CONFIG_REPLY:
        ofp_print_queue_config_reply(string, oh, verbosity);
        break;
    case OFPTYPE_GET_ASYNC_REQUEST:
    case OFPTYPE_GET_ASYNC_REPLY:
    case OFPTYPE_TABLE_MOD:
    case OFPTYPE_TABLE_FEATURES_REQUEST:
        ofp_print_not_implemented(string);
        break;
        
    case OFPTYPE_GROUP_REQUEST:
        ofp_print_stats_request(string, oh);
        break;
        
    case OFPTYPE_GROUP_REPLY:
        ofp_print_group_stats(string, oh);
        break;
        
    case OFPTYPE_GROUP_DESC_REQUEST:
        ofp_print_stats_request(string, oh);
        break;
        
    case OFPTYPE_GROUP_DESC_REPLY:
        ofp_print_group_desc(string, oh);
        break;
        
    case OFPTYPE_GROUP_FEATURES_REQUEST:
        ofp_print_stats_request(string, oh);
        break;
        
    case OFPTYPE_GROUP_FEATURES_REPLY:
        ofp_print_group_features(string, oh);
        break;
        
    case OFPTYPE_GROUP_MOD:
        ofp_print_group_mod(string, oh);
        break;

    case OFPTYPE_TABLE_FEATURES_REPLY:
        ofp_print_table_features(string, oh);
        break;

    /* Meter implementation */
    case OFPTYPE_METER_MOD:
        ofp_print_meter_mod(string, oh);
        break;

    case OFPTYPE_METER_CONFIG_REQUEST:
        ofp_print_meter_config_request(string, oh);
        break;

    case OFPTYPE_METER_FEATURES_REQUEST:
        ofp_print_stats_request(string, oh);
        break;

    case OFPTYPE_METER_REPLY:
        ofp_print_stats_reply(string, oh);
        ofp_print_meter_reply(string, oh);
        break;

    case OFPTYPE_METER_REQUEST:
        ofp_print_stats_request(string, oh);
        ofp_print_meter_request(string, oh);
        break;

    case OFPTYPE_METER_CONFIG_REPLY:
        ofp_print_meter_config_reply(string, oh);
        break;

    case OFPTYPE_METER_FEATURES_REPLY:
        ofp_print_meter_feature_reply(string, ofpmsg_body(oh));
        break;
#else
    case OFPTYPE_QUEUE_GET_CONFIG_REQUEST:
    case OFPTYPE_QUEUE_GET_CONFIG_REPLY:
    case OFPTYPE_GET_ASYNC_REQUEST:
    case OFPTYPE_GET_ASYNC_REPLY:
    case OFPTYPE_METER_MOD:
    case OFPTYPE_GROUP_REQUEST:
    case OFPTYPE_GROUP_REPLY:
    case OFPTYPE_GROUP_DESC_REQUEST:
    case OFPTYPE_GROUP_DESC_REPLY:
    case OFPTYPE_GROUP_FEATURES_REQUEST:
    case OFPTYPE_GROUP_FEATURES_REPLY:
    case OFPTYPE_METER_REQUEST:
    case OFPTYPE_METER_REPLY:
    case OFPTYPE_METER_CONFIG_REQUEST:
    case OFPTYPE_METER_CONFIG_REPLY:
    case OFPTYPE_METER_FEATURES_REQUEST:
    case OFPTYPE_METER_FEATURES_REPLY:
    case OFPTYPE_TABLE_FEATURES_REQUEST:
    case OFPTYPE_TABLE_FEATURES_REPLY:
        ofp_print_not_implemented(string);
        break;
#endif

    case OFPTYPE_HELLO:
        ofp_print_hello(string, oh);
        break;

    case OFPTYPE_ERROR:
        ofp_print_error_msg(string, oh);
        break;

    case OFPTYPE_ECHO_REQUEST:
    case OFPTYPE_ECHO_REPLY:
        ofp_print_echo(string, oh, verbosity);
        break;

    case OFPTYPE_FEATURES_REQUEST:
        break;

    case OFPTYPE_FEATURES_REPLY:
        ofp_print_switch_features(string, oh);
        break;

    case OFPTYPE_GET_CONFIG_REQUEST:
        break;

    case OFPTYPE_GET_CONFIG_REPLY:
    case OFPTYPE_SET_CONFIG:
        ofp_print_switch_config(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_PACKET_IN:
        ofp_print_packet_in(string, oh, verbosity);
        break;

    case OFPTYPE_FLOW_REMOVED:
        ofp_print_flow_removed(string, oh);
        break;

    case OFPTYPE_PORT_STATUS:
        ofp_print_port_status(string, oh);
        break;

    case OFPTYPE_PACKET_OUT:
        ofp_print_packet_out(string, oh, verbosity);
        break;

    case OFPTYPE_FLOW_MOD:
        ofp_print_flow_mod(string, oh, verbosity);
        break;

    case OFPTYPE_PORT_MOD:
        ofp_print_port_mod(string, oh);
        break;

    case OFPTYPE_BARRIER_REQUEST:
    case OFPTYPE_BARRIER_REPLY:
        break;

    case OFPTYPE_ROLE_REQUEST:
    case OFPTYPE_ROLE_REPLY:
        ofp_print_role_message(string, oh);
        break;

    case OFPTYPE_DESC_STATS_REQUEST:
    case OFPTYPE_PORT_DESC_STATS_REQUEST:
        ofp_print_stats_request(string, oh);
        break;

    case OFPTYPE_FLOW_STATS_REQUEST:
    case OFPTYPE_AGGREGATE_STATS_REQUEST:
        ofp_print_stats_request(string, oh);
        ofp_print_flow_stats_request(string, oh);
        break;

    case OFPTYPE_TABLE_STATS_REQUEST:
        ofp_print_stats_request(string, oh);
        break;

    case OFPTYPE_PORT_STATS_REQUEST:
        ofp_print_stats_request(string, oh);
        ofp_print_ofpst_port_request(string, oh);
        break;

    case OFPTYPE_QUEUE_STATS_REQUEST:
        ofp_print_stats_request(string, oh);
        ofp_print_ofpst_queue_request(string, oh);
        break;

    case OFPTYPE_DESC_STATS_REPLY:
        ofp_print_stats_reply(string, oh);
        ofp_print_ofpst_desc_reply(string, oh);
        break;

    case OFPTYPE_FLOW_STATS_REPLY:
        ofp_print_stats_reply(string, oh);
        ofp_print_flow_stats_reply(string, oh);
        break;

    case OFPTYPE_QUEUE_STATS_REPLY:
        ofp_print_stats_reply(string, oh);
        ofp_print_ofpst_queue_reply(string, oh, verbosity);
        break;

    case OFPTYPE_PORT_STATS_REPLY:
        ofp_print_stats_reply(string, oh);
        ofp_print_ofpst_port_reply(string, oh, verbosity);
        break;

    case OFPTYPE_TABLE_STATS_REPLY:
        ofp_print_stats_reply(string, oh);
        ofp_print_ofpst_table_reply(string, oh, verbosity);
        break;

    case OFPTYPE_AGGREGATE_STATS_REPLY:
        ofp_print_stats_reply(string, oh);
        ofp_print_aggregate_stats_reply(string, oh);
        break;

    case OFPTYPE_PORT_DESC_STATS_REPLY:
        ofp_print_stats_reply(string, oh);
        ofp_print_ofpst_port_desc_reply(string, oh);
        break;

    case OFPTYPE_FLOW_MOD_TABLE_ID:
        ofp_print_nxt_flow_mod_table_id(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_SET_FLOW_FORMAT:
        ofp_print_nxt_set_flow_format(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_SET_PACKET_IN_FORMAT:
        ofp_print_nxt_set_packet_in_format(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_FLOW_AGE:
        break;

    case OFPTYPE_SET_CONTROLLER_ID:
        ofp_print_nxt_set_controller_id(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_SET_ASYNC_CONFIG:
        ofp_print_nxt_set_async_config(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_FLOW_MONITOR_CANCEL:
        ofp_print_nxt_flow_monitor_cancel(string, msg);
        break;

    case OFPTYPE_FLOW_MONITOR_PAUSED:
    case OFPTYPE_FLOW_MONITOR_RESUMED:
        break;

    case OFPTYPE_FLOW_MONITOR_STATS_REQUEST:
        ofp_print_nxst_flow_monitor_request(string, msg);
        break;

    case OFPTYPE_FLOW_MONITOR_STATS_REPLY:
        ofp_print_nxst_flow_monitor_reply(string, msg);
        break;
    }
}

/* Composes and returns a string representing the OpenFlow packet of 'len'
 * bytes at 'oh' at the given 'verbosity' level.  0 is a minimal amount of
 * verbosity and higher numbers increase verbosity.  The caller is responsible
 * for freeing the string. */
char *
ofp_to_string(const void *oh_, size_t len, int verbosity)
{
    struct ds string = DS_EMPTY_INITIALIZER;
    const struct ofp_header *oh = oh_;

    if (!len) {
        ds_put_cstr(&string, "OpenFlow message is empty\n");
    } else if (len < sizeof(struct ofp_header)) {
        ds_put_format(&string, "OpenFlow packet too short (only %zu bytes):\n",
                      len);
    } else if (ntohs(oh->length) > len) {
        enum ofperr error;
        enum ofpraw raw;

        error = ofpraw_decode_partial(&raw, oh, len);
        if (!error) {
            ofp_header_to_string__(oh, raw, &string);
            ds_put_char(&string, '\n');
        }

        ds_put_format(&string,
                      "(***truncated to %zu bytes from %"PRIu16"***)\n",
                      len, ntohs(oh->length));
    } else if (ntohs(oh->length) < len) {
        ds_put_format(&string,
                      "(***only uses %"PRIu16" bytes out of %zu***)\n",
                      ntohs(oh->length), len);
    } else {
        enum ofperr error;
        enum ofpraw raw;

        error = ofpraw_decode(&raw, oh);
        if (!error) {
            ofp_to_string__(oh, raw, &string, verbosity);
            if (verbosity >= 5) {
                if (ds_last(&string) != '\n') {
                    ds_put_char(&string, '\n');
                }
                ds_put_hex_dump(&string, oh, len, 0, true);
            }
            if (ds_last(&string) != '\n') {
                ds_put_char(&string, '\n');
            }
            return ds_steal_cstr(&string);
        }

        ofp_print_error(&string, error);
    }
    ds_put_hex_dump(&string, oh, len, 0, true);
    return ds_steal_cstr(&string);
}

static void
print_and_free(FILE *stream, char *string)
{
    fputs(string, stream);
    free(string);
}

/* Pretty-print the OpenFlow packet of 'len' bytes at 'oh' to 'stream' at the
 * given 'verbosity' level.  0 is a minimal amount of verbosity and higher
 * numbers increase verbosity. */
void
ofp_print(FILE *stream, const void *oh, size_t len, int verbosity)
{
    print_and_free(stream, ofp_to_string(oh, len, verbosity));
}

/* Dumps the contents of the Ethernet frame in the 'len' bytes starting at
 * 'data' to 'stream'. */
void
ofp_print_packet(FILE *stream, const void *data, size_t len)
{
    print_and_free(stream, ofp_packet_to_string(data, len));
}
