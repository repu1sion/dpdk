/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/queue.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>

#include <rte_common.h>
#include <rte_interrupts.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ethdev_pci.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_atomic.h>
#include <rte_malloc.h>
#include <rte_dev.h>
#include <rte_flow.h>
#include <rte_flow_driver.h>

#include "e1000_logs.h"
#include "base/e1000_api.h"
#include "e1000_ethdev.h"

#define NEXT_ITEM_OF_PATTERN(item, pattern, index)		\
	do {							\
		item = (pattern) + (index);			\
		while (item->type == RTE_FLOW_ITEM_TYPE_VOID) {	\
		(index)++;					\
		item = (pattern) + (index);			\
		}						\
	} while (0)

#define NEXT_ITEM_OF_ACTION(act, actions, index)		\
	do {							\
		act = (actions) + (index);			\
		while (act->type == RTE_FLOW_ACTION_TYPE_VOID) {\
		(index)++;					\
		act = (actions) + (index);			\
		}						\
	} while (0)

/**
 * Please aware there's an asumption for all the parsers.
 * rte_flow_item is using big endian, rte_flow_attr and
 * rte_flow_action are using CPU order.
 * Because the pattern is used to describe the packets,
 * normally the packets should use network order.
 */

/**
 * Parse the rule to see if it is a n-tuple rule.
 * And get the n-tuple filter info BTW.
 * pattern:
 * The first not void item can be ETH or IPV4.
 * The second not void item must be IPV4 if the first one is ETH.
 * The third not void item must be UDP or TCP or SCTP
 * The next not void item must be END.
 * action:
 * The first not void action should be QUEUE.
 * The next not void action should be END.
 * pattern example:
 * ITEM		Spec			Mask
 * ETH		NULL			NULL
 * IPV4		src_addr 192.168.1.20	0xFFFFFFFF
 *			dst_addr 192.167.3.50	0xFFFFFFFF
 *			next_proto_id	17	0xFF
 * UDP/TCP/	src_port	80	0xFFFF
 * SCTP		dst_port	80	0xFFFF
 * END
 * other members in mask and spec should set to 0x00.
 * item->last should be NULL.
 */
static int
cons_parse_ntuple_filter(const struct rte_flow_attr *attr,
			 const struct rte_flow_item pattern[],
			 const struct rte_flow_action actions[],
			 struct rte_eth_ntuple_filter *filter,
			 struct rte_flow_error *error)
{
	const struct rte_flow_item *item;
	const struct rte_flow_action *act;
	const struct rte_flow_item_ipv4 *ipv4_spec;
	const struct rte_flow_item_ipv4 *ipv4_mask;
	const struct rte_flow_item_tcp *tcp_spec;
	const struct rte_flow_item_tcp *tcp_mask;
	const struct rte_flow_item_udp *udp_spec;
	const struct rte_flow_item_udp *udp_mask;
	const struct rte_flow_item_sctp *sctp_spec;
	const struct rte_flow_item_sctp *sctp_mask;
	uint32_t index;

	if (!pattern) {
		rte_flow_error_set(error,
			EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_NUM,
			NULL, "NULL pattern.");
		return -rte_errno;
	}

	if (!actions) {
		rte_flow_error_set(error, EINVAL,
				   RTE_FLOW_ERROR_TYPE_ACTION_NUM,
				   NULL, "NULL action.");
		return -rte_errno;
	}
	if (!attr) {
		rte_flow_error_set(error, EINVAL,
				   RTE_FLOW_ERROR_TYPE_ATTR,
				   NULL, "NULL attribute.");
		return -rte_errno;
	}

	/* parse pattern */
	index = 0;

	/* the first not void item can be MAC or IPv4 */
	NEXT_ITEM_OF_PATTERN(item, pattern, index);

	if (item->type != RTE_FLOW_ITEM_TYPE_ETH &&
	    item->type != RTE_FLOW_ITEM_TYPE_IPV4) {
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_ITEM,
			item, "Not supported by ntuple filter");
		return -rte_errno;
	}
	/* Skip Ethernet */
	if (item->type == RTE_FLOW_ITEM_TYPE_ETH) {
		/*Not supported last point for range*/
		if (item->last) {
			rte_flow_error_set(error,
			  EINVAL,
			  RTE_FLOW_ERROR_TYPE_UNSPECIFIED,
			  item, "Not supported last point for range");
			return -rte_errno;
		}
		/* if the first item is MAC, the content should be NULL */
		if (item->spec || item->mask) {
			rte_flow_error_set(error, EINVAL,
				RTE_FLOW_ERROR_TYPE_ITEM,
				item, "Not supported by ntuple filter");
			return -rte_errno;
		}
		/* check if the next not void item is IPv4 */
		index++;
		NEXT_ITEM_OF_PATTERN(item, pattern, index);
		if (item->type != RTE_FLOW_ITEM_TYPE_IPV4) {
			rte_flow_error_set(error,
			  EINVAL, RTE_FLOW_ERROR_TYPE_ITEM,
			  item, "Not supported by ntuple filter");
			return -rte_errno;
		}
	}

	/* get the IPv4 info */
	if (!item->spec || !item->mask) {
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_ITEM,
			item, "Invalid ntuple mask");
		return -rte_errno;
	}
	/* Not supported last point for range */
	if (item->last) {
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_UNSPECIFIED,
			item, "Not supported last point for range");
		return -rte_errno;
	}

	ipv4_mask = (const struct rte_flow_item_ipv4 *)item->mask;
	/**
	 * Only support src & dst addresses, protocol,
	 * others should be masked.
	 */

	if (ipv4_mask->hdr.version_ihl ||
		ipv4_mask->hdr.type_of_service ||
		ipv4_mask->hdr.total_length ||
		ipv4_mask->hdr.packet_id ||
		ipv4_mask->hdr.fragment_offset ||
		ipv4_mask->hdr.time_to_live ||
		ipv4_mask->hdr.hdr_checksum) {
		rte_flow_error_set(error,
			EINVAL, RTE_FLOW_ERROR_TYPE_ITEM,
			item, "Not supported by ntuple filter");
		return -rte_errno;
	}

	filter->dst_ip_mask = ipv4_mask->hdr.dst_addr;
	filter->src_ip_mask = ipv4_mask->hdr.src_addr;
	filter->proto_mask  = ipv4_mask->hdr.next_proto_id;

	ipv4_spec = (const struct rte_flow_item_ipv4 *)item->spec;
	filter->dst_ip = ipv4_spec->hdr.dst_addr;
	filter->src_ip = ipv4_spec->hdr.src_addr;
	filter->proto  = ipv4_spec->hdr.next_proto_id;

	/* check if the next not void item is TCP or UDP or SCTP */
	index++;
	NEXT_ITEM_OF_PATTERN(item, pattern, index);
	if (item->type != RTE_FLOW_ITEM_TYPE_TCP &&
	    item->type != RTE_FLOW_ITEM_TYPE_UDP &&
	    item->type != RTE_FLOW_ITEM_TYPE_SCTP) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_ITEM,
			item, "Not supported by ntuple filter");
		return -rte_errno;
	}

	/* Not supported last point for range */
	if (item->last) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_UNSPECIFIED,
			item, "Not supported last point for range");
		return -rte_errno;
	}

	/* get the TCP/UDP/SCTP info */
	if (item->type == RTE_FLOW_ITEM_TYPE_TCP) {
		if (item->spec && item->mask) {
			tcp_mask = (const struct rte_flow_item_tcp *)item->mask;

			/**
			 * Only support src & dst ports, tcp flags,
			 * others should be masked.
			 */
			if (tcp_mask->hdr.sent_seq ||
				tcp_mask->hdr.recv_ack ||
				tcp_mask->hdr.data_off ||
				tcp_mask->hdr.rx_win ||
				tcp_mask->hdr.cksum ||
				tcp_mask->hdr.tcp_urp) {
				memset(filter, 0,
					sizeof(struct rte_eth_ntuple_filter));
				rte_flow_error_set(error, EINVAL,
					RTE_FLOW_ERROR_TYPE_ITEM,
					item, "Not supported by ntuple filter");
				return -rte_errno;
			}

			filter->dst_port_mask  = tcp_mask->hdr.dst_port;
			filter->src_port_mask  = tcp_mask->hdr.src_port;
			if (tcp_mask->hdr.tcp_flags == 0xFF) {
				filter->flags |= RTE_NTUPLE_FLAGS_TCP_FLAG;
			} else if (!tcp_mask->hdr.tcp_flags) {
				filter->flags &= ~RTE_NTUPLE_FLAGS_TCP_FLAG;
			} else {
				memset(filter, 0,
					sizeof(struct rte_eth_ntuple_filter));
				rte_flow_error_set(error, EINVAL,
					RTE_FLOW_ERROR_TYPE_ITEM,
					item, "Not supported by ntuple filter");
				return -rte_errno;
			}

			tcp_spec = (const struct rte_flow_item_tcp *)item->spec;
			filter->dst_port  = tcp_spec->hdr.dst_port;
			filter->src_port  = tcp_spec->hdr.src_port;
			filter->tcp_flags = tcp_spec->hdr.tcp_flags;
		}
	} else if (item->type == RTE_FLOW_ITEM_TYPE_UDP) {
		if (item->spec && item->mask) {
			udp_mask = (const struct rte_flow_item_udp *)item->mask;

			/**
			 * Only support src & dst ports,
			 * others should be masked.
			 */
			if (udp_mask->hdr.dgram_len ||
			    udp_mask->hdr.dgram_cksum) {
				memset(filter, 0,
					sizeof(struct rte_eth_ntuple_filter));
				rte_flow_error_set(error, EINVAL,
					RTE_FLOW_ERROR_TYPE_ITEM,
					item, "Not supported by ntuple filter");
				return -rte_errno;
			}

			filter->dst_port_mask = udp_mask->hdr.dst_port;
			filter->src_port_mask = udp_mask->hdr.src_port;

			udp_spec = (const struct rte_flow_item_udp *)item->spec;
			filter->dst_port = udp_spec->hdr.dst_port;
			filter->src_port = udp_spec->hdr.src_port;
		}
	} else {
		if (item->spec && item->mask) {
			sctp_mask = (const struct rte_flow_item_sctp *)
					item->mask;

			/**
			 * Only support src & dst ports,
			 * others should be masked.
			 */
			if (sctp_mask->hdr.tag ||
			    sctp_mask->hdr.cksum) {
				memset(filter, 0,
					sizeof(struct rte_eth_ntuple_filter));
				rte_flow_error_set(error, EINVAL,
					RTE_FLOW_ERROR_TYPE_ITEM,
					item, "Not supported by ntuple filter");
				return -rte_errno;
			}

			filter->dst_port_mask = sctp_mask->hdr.dst_port;
			filter->src_port_mask = sctp_mask->hdr.src_port;

			sctp_spec = (const struct rte_flow_item_sctp *)
					item->spec;
			filter->dst_port = sctp_spec->hdr.dst_port;
			filter->src_port = sctp_spec->hdr.src_port;
		}
	}
	/* check if the next not void item is END */
	index++;
	NEXT_ITEM_OF_PATTERN(item, pattern, index);
	if (item->type != RTE_FLOW_ITEM_TYPE_END) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_ITEM,
			item, "Not supported by ntuple filter");
		return -rte_errno;
	}

	/* parse action */
	index = 0;

	/**
	 * n-tuple only supports forwarding,
	 * check if the first not void action is QUEUE.
	 */
	NEXT_ITEM_OF_ACTION(act, actions, index);
	if (act->type != RTE_FLOW_ACTION_TYPE_QUEUE) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_ACTION,
			item, "Not supported action.");
		return -rte_errno;
	}
	filter->queue =
		((const struct rte_flow_action_queue *)act->conf)->index;

	/* check if the next not void item is END */
	index++;
	NEXT_ITEM_OF_ACTION(act, actions, index);
	if (act->type != RTE_FLOW_ACTION_TYPE_END) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_ACTION,
			act, "Not supported action.");
		return -rte_errno;
	}

	/* parse attr */
	/* must be input direction */
	if (!attr->ingress) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
				   RTE_FLOW_ERROR_TYPE_ATTR_INGRESS,
				   attr, "Only support ingress.");
		return -rte_errno;
	}

	/* not supported */
	if (attr->egress) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
				   RTE_FLOW_ERROR_TYPE_ATTR_EGRESS,
				   attr, "Not support egress.");
		return -rte_errno;
	}

	if (attr->priority > 0xFFFF) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
				   RTE_FLOW_ERROR_TYPE_ATTR_PRIORITY,
				   attr, "Error priority.");
		return -rte_errno;
	}
	filter->priority = (uint16_t)attr->priority;

	return 0;
}

/* a specific function for igb because the flags is specific */
static int
igb_parse_ntuple_filter(struct rte_eth_dev *dev,
			  const struct rte_flow_attr *attr,
			  const struct rte_flow_item pattern[],
			  const struct rte_flow_action actions[],
			  struct rte_eth_ntuple_filter *filter,
			  struct rte_flow_error *error)
{
	struct e1000_hw *hw = E1000_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int ret;

	MAC_TYPE_FILTER_SUP(hw->mac.type);

	ret = cons_parse_ntuple_filter(attr, pattern, actions, filter, error);

	if (ret)
		return ret;

	/* Igb doesn't support many priorities. */
	if (filter->priority > E1000_2TUPLE_MAX_PRI) {
		memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
		rte_flow_error_set(error, EINVAL,
			RTE_FLOW_ERROR_TYPE_ITEM,
			NULL, "Priority not supported by ntuple filter");
		return -rte_errno;
	}

	if (hw->mac.type == e1000_82576) {
		if (filter->queue >= IGB_MAX_RX_QUEUE_NUM_82576) {
			memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
			rte_flow_error_set(error, EINVAL,
				RTE_FLOW_ERROR_TYPE_ITEM,
				NULL, "queue number not "
				"supported by ntuple filter");
			return -rte_errno;
		}
		filter->flags |= RTE_5TUPLE_FLAGS;
	} else {
		if (filter->src_ip_mask || filter->dst_ip_mask ||
			filter->src_port_mask) {
			memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
			rte_flow_error_set(error, EINVAL,
				RTE_FLOW_ERROR_TYPE_ITEM,
				NULL, "only two tuple are "
				"supported by this filter");
			return -rte_errno;
		}
		if (filter->queue >= IGB_MAX_RX_QUEUE_NUM) {
			memset(filter, 0, sizeof(struct rte_eth_ntuple_filter));
			rte_flow_error_set(error, EINVAL,
				RTE_FLOW_ERROR_TYPE_ITEM,
				NULL, "queue number not "
				"supported by ntuple filter");
			return -rte_errno;
		}
		filter->flags |= RTE_2TUPLE_FLAGS;
	}

	return 0;
}

/**
 * Check if the flow rule is supported by igb.
 * It only checkes the format. Don't guarantee the rule can be programmed into
 * the HW. Because there can be no enough room for the rule.
 */
static int
igb_flow_validate(__rte_unused struct rte_eth_dev *dev,
		const struct rte_flow_attr *attr,
		const struct rte_flow_item pattern[],
		const struct rte_flow_action actions[],
		struct rte_flow_error *error)
{
	struct rte_eth_ntuple_filter ntuple_filter;
	int ret;

	memset(&ntuple_filter, 0, sizeof(struct rte_eth_ntuple_filter));
	ret = igb_parse_ntuple_filter(dev, attr, pattern,
				actions, &ntuple_filter, error);
	if (!ret)
		return 0;

	return ret;
}

const struct rte_flow_ops igb_flow_ops = {
	igb_flow_validate,
	NULL,
	NULL,
	NULL,
	NULL,
};