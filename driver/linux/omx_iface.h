/*
 * Open-MX
 * Copyright © INRIA 2007 (see AUTHORS file)
 *
 * The development of this software has been funded by Myricom, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License in COPYING.GPL for more details.
 */

#ifndef __omx_iface_h__
#define __omx_iface_h__

enum omx_iface_status {
	/* iface is ready to be used */
	OMX_IFACE_STATUS_OK,
	/* iface is being closed by somebody else, no new endpoint may be open */
	OMX_IFACE_STATUS_CLOSING,
};

struct omx_iface {
	int index;

	struct net_device * eth_ifp;
	char * hostname;

	rwlock_t endpoint_lock;
	enum omx_iface_status status;
	struct kref refcount;
	int endpoint_nr;
	struct omx_endpoint ** endpoints;

	uint32_t counters[OMX_COUNTER_INDEX_MAX];
};

extern int omx_ifaces_show(char *buf);
extern int omx_ifaces_store(const char *buf, size_t size);
extern int omx_ifaces_get_count(void);
extern int omx_iface_get_id(uint8_t board_index, uint64_t * board_addr, char * hostname, char * ifacename);
extern struct omx_iface * omx_iface_find_by_ifp(struct net_device *ifp);
extern int omx_iface_get_counters(uint8_t board_index, int clear, uint64_t buffer_addr, uint32_t buffer_length);

/* counters */
#define omx_counter_inc(iface, index)		\
do {						\
	iface->counters[OMX_COUNTER_##index]++;	\
} while (0)

#endif /* __omx_iface_h__ */

/*
 * Local variables:
 *  tab-width: 8
 *  c-basic-offset: 8
 *  c-indent-level: 8
 * End:
 */