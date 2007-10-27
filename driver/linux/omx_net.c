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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/utsname.h>
#include <linux/if_arp.h>

#include "omx_common.h"
#include "omx_hal.h"

/*
 * Scan the list of physical interfaces and return the
 * one that matches ifname (and take a reference on it).
 */
static struct net_device *
dev_hold_by_name(const char * ifname)
{
	struct net_device * ifp;

	read_lock(&dev_base_lock);
	omx_for_each_netdev(ifp) {
		dev_hold(ifp);
		if (!strcmp(ifp->name, ifname)) {
			read_unlock(&dev_base_lock);
			return ifp;
		}
		dev_put(ifp);
	}
	read_unlock(&dev_base_lock);

	printk(KERN_ERR "Open-MX: Failed to find interface '%s'\n", ifname);
	return NULL;
}

/******************************
 * Managing interfaces
 */

/*
 * Array, number and lock for the list of ifaces
 */
static struct omx_iface ** omx_ifaces;
static unsigned omx_iface_nr = 0;
static rwlock_t omx_iface_lock = RW_LOCK_UNLOCKED;

/*
 * Returns the iface associated to a physical interface.
 * Should be used when an incoming packets has been received by ifp.
 */
struct omx_iface *
omx_iface_find_by_ifp(struct net_device *ifp)
{
	int i;

	/* since iface removal disables incoming packet processing, we don't
	 * need to lock the iface array or to hold a reference on the iface.
	 */
	for (i=0; i<omx_iface_max; i++) {
		struct omx_iface * iface = omx_ifaces[i];
		if (likely(iface && iface->eth_ifp == ifp))
			return iface;
	}

	return NULL;
}

/*
 * Return the number of omx ifaces.
 */
int
omx_ifaces_get_count(void)
{
	int i, count = 0;

	/* no need to lock since the array of iface is always coherent
	 * and we don't access the internals of the ifaces
	 */
	for (i=0; i<omx_iface_max; i++)
		if (omx_ifaces[i] != NULL)
			count++;

	return count;
}

/*
 * Return the address and name of an iface.
 */
int
omx_iface_get_id(uint8_t board_index, uint64_t * board_addr, char * hostname, char * ifacename)
{
	struct omx_iface * iface;
	struct net_device * ifp;
	int ret;

	/* need to lock since we access the internals of the iface */
	read_lock(&omx_iface_lock);

	ret = -EINVAL;
	if (board_index >= omx_iface_max
	    || omx_ifaces[board_index] == NULL)
		goto out_with_lock;

	iface = omx_ifaces[board_index];
	ifp = iface->eth_ifp;

	*board_addr = omx_board_addr_from_netdevice(ifp);
	strncpy(ifacename, ifp->name, OMX_IF_NAMESIZE);
	ifacename[OMX_IF_NAMESIZE-1] = '\0';
	strncpy(hostname, iface->hostname, OMX_HOSTNAMELEN_MAX);
	hostname[OMX_HOSTNAMELEN_MAX-1] = '\0';

	read_unlock(&omx_iface_lock);

	return 0;

 out_with_lock:
	read_unlock(&omx_iface_lock);
	return ret;
}

/******************************
 * Attaching/Detaching interfaces
 */

/*
 * Attach a new iface.
 *
 * Must be called with ifaces lock hold.
 */
static int
omx_iface_attach(struct net_device * ifp)
{
	struct omx_iface * iface;
	char *hostname;
	unsigned mtu = ifp->mtu;
	int ret;
	int i;

	if (omx_iface_nr == omx_iface_max) {
		printk(KERN_ERR "Open-MX: Too many interfaces already attached\n");
		ret = -EBUSY;
		goto out_with_ifp_hold;
	}

	if (omx_iface_find_by_ifp(ifp)) {
		printk(KERN_ERR "Open-MX: Interface %s already attached\n", ifp->name);
		ret = -EBUSY;
		goto out_with_ifp_hold;
	}

	for(i=0; i<omx_iface_max; i++)
		if (omx_ifaces[i] == NULL)
			break;

	iface = kzalloc(sizeof(struct omx_iface), GFP_KERNEL);
	if (!iface) {
		printk(KERN_ERR "Open-MX: Failed to allocate interface as board %d\n", i);
		ret = -ENOMEM;
		goto out_with_ifp_hold;
	}

	printk(KERN_INFO "Open-MX: Attaching %sEthernet device '%s' as #%i, MTU=%d\n",
	       (ifp->type == ARPHRD_ETHER ? "" : "non-"), ifp->name, i, mtu);
	if (mtu < OMX_MTU_MIN)
		printk(KERN_WARNING "Open-MX: WARNING: Interface '%s' MTU should be at least %d, current value %d might cause problems\n",
		       ifp->name, OMX_MTU_MIN, mtu);

	hostname = kmalloc(OMX_HOSTNAMELEN_MAX, GFP_KERNEL);
	if (!hostname) {
		printk(KERN_ERR "Open-MX: Failed to allocate interface hostname\n");
		ret = -ENOMEM;
		goto out_with_iface;
	}

	snprintf(hostname, OMX_HOSTNAMELEN_MAX, "%s:%d", omx_current_utsname.nodename, i);
	hostname[OMX_HOSTNAMELEN_MAX-1] = '\0';
	iface->hostname = hostname;

	iface->eth_ifp = ifp;
	iface->endpoint_nr = 0;
	iface->endpoints = kzalloc(omx_endpoint_max * sizeof(struct omx_endpoint *), GFP_KERNEL);
	if (!iface->endpoints) {
		printk(KERN_ERR "Open-MX: Failed to allocate interface endpoint pointers\n");
		ret = -ENOMEM;
		goto out_with_iface_hostname;
	}

	init_waitqueue_head(&iface->noendpoint_queue);
	rwlock_init(&iface->endpoint_lock);
	iface->index = i;
	omx_iface_nr++;
	omx_ifaces[i] = iface;

	return 0;

 out_with_iface_hostname:
	kfree(hostname);
 out_with_iface:
	kfree(iface);
 out_with_ifp_hold:
	return ret;
}

/*
 * Detach an existing iface, possibly by force.
 *
 * Must be called with ifaces lock hold.
 * Incoming packets should be disabled (by temporarily
 * removing omx_pt in the caller if necessary)
 * to prevent users while detaching the iface.
 */
static int
__omx_iface_detach(struct omx_iface * iface, int force)
{
	DECLARE_WAITQUEUE(wq, current);
	int ret;
	int i;

	BUG_ON(omx_ifaces[iface->index] == NULL);

	/* mark as closing so that nobody opens a new endpoint,
	 * protected by the ifaces lock
	 */
	iface->status = OMX_IFACE_STATUS_CLOSING;

	/* if force, close all endpoints.
	 * if not force, error if some endpoints are open.
	 */
	write_lock_bh(&iface->endpoint_lock);
	ret = -EBUSY;
	if (!force && iface->endpoint_nr) {
		printk(KERN_INFO "Open-MX: cannot detach interface #%d '%s', still %d endpoints open\n",
		       iface->index, iface->eth_ifp->name, iface->endpoint_nr);
		write_unlock_bh(&iface->endpoint_lock);
		goto out;
	}

	for(i=0; i<omx_endpoint_max; i++) {
		struct omx_endpoint * endpoint = iface->endpoints[i];
		if (!endpoint)
			continue;

		printk(KERN_INFO "Open-MX: forcing close of endpoint #%d attached to iface #%d '%s'\n",
		       i, iface->index, iface->eth_ifp->name);

		/* close the endpoint, with the iface lock hold */
		ret = __omx_endpoint_close(endpoint, 1);
		if (ret < 0) {
			BUG_ON(ret != -EBUSY);
			/* somebody else is already closing this endpoint,
			 * let's forget about it for now, we'll wait later
			 */
		}
	}

	/* wait for concurrent endpoint closers to be done */
	add_wait_queue(&iface->noendpoint_queue, &wq);
	for(;;) {
		set_current_state(force ? TASK_UNINTERRUPTIBLE
				  : TASK_INTERRUPTIBLE);
		if (!iface->endpoint_nr)
			break;
		write_unlock_bh(&iface->endpoint_lock);

		if (signal_pending(current)) {
			set_current_state(TASK_RUNNING);
			remove_wait_queue(&iface->noendpoint_queue, &wq);
			iface->status = OMX_IFACE_STATUS_OK;
			return -EINTR;
		}

		schedule();

		write_lock_bh(&iface->endpoint_lock);
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&iface->noendpoint_queue, &wq);
	write_unlock_bh(&iface->endpoint_lock);

	printk(KERN_INFO "Open-MX: detaching interface #%d '%s'\n", iface->index, iface->eth_ifp->name);

	omx_ifaces[iface->index] = NULL;
	omx_iface_nr--;
	kfree(iface->endpoints);
	kfree(iface->hostname);
	kfree(iface);

	return 0;

 out:
	return ret;
}

static inline int
omx_iface_detach(struct omx_iface * iface)
{
	return __omx_iface_detach(iface, 0);
}

static inline int
omx_iface_detach_force(struct omx_iface * iface)
{
	return __omx_iface_detach(iface, 1);
}

/******************************
 * Attribute-based attaching/detaching of interfaces
 */

/*
 * Format a buffer containing the list of attached ifaces.
 */
int
omx_ifaces_show(char *buf)
{
	int total = 0;
	int i;

	/* need to lock since we access the internals of the ifaces */
	read_lock(&omx_iface_lock);

	for (i=0; i<omx_iface_max; i++) {
		struct omx_iface * iface = omx_ifaces[i];
		if (iface) {
			char * ifname = iface->eth_ifp->name;
			int length = strlen(ifname);
			/* FIXME: check total+length+2 <= PAGE_SIZE ? */
			strcpy(buf, ifname);
			buf += length;
			strcpy(buf, "\n");
			buf += 1;
			total += length+1;
		}
	}

	read_unlock(&omx_iface_lock);

	return total + 1;
}

/*
 * Attach/Detach ifaces depending on the given string.
 *
 * +name adds an iface
 * -name removes one
 */
int
omx_ifaces_store(const char *buf, size_t size)
{
	char copy[IFNAMSIZ];
	char * ptr;

	/* remove the ending \n if required, so copy first since buf is const */
	strncpy(copy, buf+1, IFNAMSIZ);
	copy[IFNAMSIZ-1] = '\0';
	ptr = strchr(copy, '\n');
	if (ptr)
		*ptr = '\0';

	if (buf[0] == '-') {
		int force = 0;
		int i;
		/* if none matches, we return -EINVAL.
		 * if one matches, it sets ret accordingly.
		 */
		int ret = -EINVAL;

		if (copy[0] == '-')
			force = 1;

		write_lock(&omx_iface_lock);
		for(i=0; i<omx_iface_max; i++) {
			struct omx_iface * iface = omx_ifaces[i];
			struct net_device * ifp;

			if (iface == NULL)
				continue;

			ifp = iface->eth_ifp;
			if (strcmp(ifp->name, copy+force))
				continue;

			/* disable incoming packets while removing the iface
			 * to prevent races
			 */
			dev_remove_pack(&omx_pt);
			ret = __omx_iface_detach(iface, force);
			dev_add_pack(&omx_pt);

			/* release the interface now */
			dev_put(ifp);
			break;
		}
		write_unlock(&omx_iface_lock);

		if (ret == -EINVAL) {
			printk(KERN_ERR "Open-MX: Cannot find any attached interface '%s' to detach\n", copy);
			return -EINVAL;
		}
		return size;

	} else if (buf[0] == '+') {
		struct net_device * ifp;
		int ret;

		ifp = dev_hold_by_name(copy);
		if (!ifp)
			return -EINVAL;

		write_lock(&omx_iface_lock);
		ret = omx_iface_attach(ifp);
		write_unlock(&omx_iface_lock);
		if (ret < 0) {
			dev_put(ifp);
			return ret;
		}

		return size;

	} else {
		printk(KERN_ERR "Open-MX: Unrecognized command passed in the ifaces file, need either +name or -name\n");
		return -EINVAL;
	}
}

/******************************
 * Attaching/Detaching endpoints to ifaces
 */

/*
 * Attach a new endpoint
 */
int
omx_iface_attach_endpoint(struct omx_endpoint * endpoint)
{
	struct omx_iface * iface;
	int ret;

	BUG_ON(endpoint->status != OMX_ENDPOINT_STATUS_INITIALIZING);

	ret = -EINVAL;
	if (endpoint->endpoint_index >= omx_endpoint_max)
		goto out;

	/* lock the list of ifaces */
	read_lock(&omx_iface_lock);

	/* find the iface */
	ret = -EINVAL;
	if (endpoint->board_index >= omx_iface_max)
		goto out_with_ifaces_locked;

	ret = -ENODEV;
	if ((iface = omx_ifaces[endpoint->board_index]) == NULL
	    || iface->status != OMX_IFACE_STATUS_OK) {
		printk(KERN_ERR "Open-MX: Cannot open endpoint on unexisting board %d\n",
		       endpoint->board_index);
		goto out_with_ifaces_locked;
	}
	iface = omx_ifaces[endpoint->board_index];

	/* lock the list of endpoints in the iface */
	write_lock_bh(&iface->endpoint_lock);

	/* add the endpoint */
	ret = -EBUSY;
	if (iface->endpoints[endpoint->endpoint_index] != NULL) {
		printk(KERN_ERR "Open-MX: endpoint already open\n");
		goto out_with_endpoints_locked;
	}

	iface->endpoints[endpoint->endpoint_index] = endpoint ;
	iface->endpoint_nr++;
	endpoint->iface = iface;

	/* mark the endpoint as open here so that anybody removing this
	 * iface never sees any endpoint in status INIT in the iface list
	 * (only OK and CLOSING are allowed there)
	 */
	endpoint->status = OMX_ENDPOINT_STATUS_OK;

	write_unlock_bh(&iface->endpoint_lock);
	read_unlock(&omx_iface_lock);

	return 0;

 out_with_endpoints_locked:
	write_unlock_bh(&iface->endpoint_lock);
 out_with_ifaces_locked:
	read_unlock(&omx_iface_lock);
 out:
	return ret;
}

/*
 * Detach an existing endpoint
 *
 * Must be called while endpoint is status CLOSING.
 *
 * ifacelocked is set when detaching an iface and thus removing all endpoints
 * by force.
 * It is not (and thus the iface lock has to be taken) when the endpoint is
 * normally closed from the application.
 */
void
omx_iface_detach_endpoint(struct omx_endpoint * endpoint,
			   int ifacelocked)
{
	struct omx_iface * iface = endpoint->iface;

	BUG_ON(endpoint->status != OMX_ENDPOINT_STATUS_CLOSING);

	/* lock the list of endpoints in the iface, if needed */
	if (!ifacelocked)
		write_lock_bh(&iface->endpoint_lock);

	BUG_ON(iface->endpoints[endpoint->endpoint_index] != endpoint);
	iface->endpoints[endpoint->endpoint_index] = NULL;
	/* decrease the number of endpoints and wakeup the iface detacher if needed */
	if (!--iface->endpoint_nr
	    && iface->status == OMX_IFACE_STATUS_CLOSING)
		wake_up(&iface->noendpoint_queue);

	if (!ifacelocked)
		write_unlock_bh(&iface->endpoint_lock);
}

/*
 * Return some info about an endpoint.
 */
int
omx_endpoint_get_info(uint32_t board_index, uint32_t endpoint_index,
		      uint32_t * closed, uint32_t * pid, char * command, size_t len)
{
	struct omx_iface * iface;
	struct omx_endpoint * endpoint;
	int ret;

	/* need to lock since we access the internals of the iface */
	read_lock(&omx_iface_lock);

	ret = -EINVAL;
	if (board_index >= omx_iface_max
	    || omx_ifaces[board_index] == NULL)
		goto out_with_lock;
	iface = omx_ifaces[board_index];

        read_lock(&iface->endpoint_lock);
        if (endpoint_index >= omx_endpoint_max)
                goto out_with_iface_lock;

        endpoint = iface->endpoints[endpoint_index];
        if (endpoint) {
		*closed = 0;
		*pid = endpoint->opener_pid;
		strncpy(command, endpoint->opener_comm, len);
	} else {
		*closed = 1;
	}

        read_unlock(&iface->endpoint_lock);
	read_unlock(&omx_iface_lock);

	return 0;

 out_with_iface_lock:
        read_unlock(&iface->endpoint_lock);
 out_with_lock:
	return ret;
}

/******************************
 * Netdevice notifier
 */

static int
omx_netdevice_notifier_cb(struct notifier_block *unused,
			   unsigned long event, void *ptr)
{
	struct net_device *ifp = (struct net_device *) ptr;

	if (event == NETDEV_UNREGISTER) {
		struct omx_iface * iface;

		write_lock(&omx_iface_lock);
		iface = omx_iface_find_by_ifp(ifp);
		if (iface) {
			int ret;
			printk(KERN_INFO "Open-MX: interface '%s' being unregistered, forcing closing of endpoints...\n",
			       ifp->name);
			/* there is no need to disable incoming packets since
			 * the ethernet ifp is already disabled before the notifier is called
			 */
			ret = omx_iface_detach_force(iface);
			BUG_ON(ret);
			dev_put(ifp);
		}
		write_unlock(&omx_iface_lock);
	}

	return NOTIFY_DONE;
}

static struct notifier_block omx_netdevice_notifier = {
	.notifier_call = omx_netdevice_notifier_cb,
};

/************************
 * Memory Copy Benchmark
 */

#define OMX_COPYBENCH_BUFLEN (4*1024*1024UL)
#define OMX_COPYBENCH_ITERS 1024

static int
omx_net_copy_bench(void)
{
	void * srcbuf, * dstbuf;
	struct timeval tv1, tv2;
	unsigned long usecs;
	unsigned long nsecs_per_iter;
	unsigned long MB_per_sec;
	int i, err;

	err = -ENOMEM;
	srcbuf = vmalloc(OMX_COPYBENCH_BUFLEN);
	if (!srcbuf)
		goto out;
	dstbuf = vmalloc(OMX_COPYBENCH_BUFLEN);
	if (!dstbuf)
		goto out_with_srcbuf;

	printk("Open-MX: running copy benchmark...\n");

	do_gettimeofday(&tv1);
	for(i=0; i<OMX_COPYBENCH_ITERS; i++)
		memcpy(dstbuf, srcbuf, OMX_COPYBENCH_BUFLEN);
	do_gettimeofday(&tv2);

	usecs = (tv2.tv_sec - tv1.tv_sec)*1000000UL
		+ (tv2.tv_usec - tv1.tv_usec);
	nsecs_per_iter = (usecs * 1000ULL) /OMX_COPYBENCH_ITERS;
	MB_per_sec = OMX_COPYBENCH_BUFLEN / (nsecs_per_iter / 1000);
	printk("Open-MX: memcpy of %ld bytes %d times took %ld us\n",
	       OMX_COPYBENCH_BUFLEN, OMX_COPYBENCH_ITERS, usecs);
	printk("Open-MX: memcpy of %ld bytes took %ld ns (%ld MB/s)\n",
	       OMX_COPYBENCH_BUFLEN, nsecs_per_iter, MB_per_sec);

	err = 0;

	vfree(dstbuf);
 out_with_srcbuf:
	vfree(srcbuf);
 out:
	return err;
}

/******************************
 * Initialization and termination
 */

int
omx_net_init(const char * ifnames)
{
	int ret = 0;

	if (omx_copybench)
		omx_net_copy_bench();

	omx_pkt_type_handlers_init();

	dev_add_pack(&omx_pt);

	ret = register_netdevice_notifier(&omx_netdevice_notifier);
	if (ret < 0) {
		printk(KERN_ERR "Open-MX: failed to register netdevice notifier\n");
		goto out_with_pack;
	}

	omx_ifaces = kzalloc(omx_iface_max * sizeof(struct omx_iface *), GFP_KERNEL);
	if (!omx_ifaces) {
		printk(KERN_ERR "Open-MX: failed to allocate interface array\n");
		ret = -ENOMEM;
		goto out_with_notifier;
	}

	if (ifnames) {
		/* attach ifaces whose name are in ifnames (limited to omx_iface_max) */
		char * copy = kstrdup(ifnames, GFP_KERNEL);
		char * ifname;

		while ((ifname = strsep(&copy, ",")) != NULL) {
			struct net_device * ifp;
			ifp = dev_hold_by_name(ifname);
			if (ifp)
				if (omx_iface_attach(ifp) < 0) {
					dev_put(ifp);
					break;
				}
		}

		kfree(copy);

	} else {
		/* attach everything (limited to omx_iface_max) */
		struct net_device * ifp;

		read_lock(&dev_base_lock);
		omx_for_each_netdev(ifp) {
			dev_hold(ifp);
			if (omx_iface_attach(ifp) < 0) {
				dev_put(ifp);
				break;
			}
		}
		read_unlock(&dev_base_lock);
	}

	printk(KERN_INFO "Open-MX: attached %d interfaces\n", omx_iface_nr);
	return 0;

 out_with_notifier:
	unregister_netdevice_notifier(&omx_netdevice_notifier);
 out_with_pack:
	dev_remove_pack(&omx_pt);
	return ret;
}

void
omx_net_exit(void)
{
	int i, nr = 0;

	/* module unloading cannot happen before all users exit
	 * since they hold a reference on the chardev.
	 * so all endpoints are closed once we arrive here.
	 */

	dev_remove_pack(&omx_pt);
	/* now, no iface may be used by any incoming packet */

	/* prevent omx_netdevice_notifier from removing an iface now */
	write_lock(&omx_iface_lock);

	for (i=0; i<omx_iface_max; i++) {
		struct omx_iface * iface = omx_ifaces[i];
		if (iface != NULL) {
			struct net_device * ifp = iface->eth_ifp;

			/* detach the iface now.
			 * all endpoints are closed, no need to force
			 */
			BUG_ON(omx_iface_detach(iface) < 0);
			dev_put(ifp);
			nr++;
		}
	}
	printk(KERN_INFO "Open-MX: detached %d interfaces\n", nr);

	/* release the lock to let omx_netdevice_notifier finish
	 * in case it has been invoked during our loop
	 */
	write_unlock(&omx_iface_lock);
	/* unregister the notifier then */
	unregister_netdevice_notifier(&omx_netdevice_notifier);

	/* free structures now that the notifier is gone */
	kfree(omx_ifaces);
}

/*
 * Local variables:
 *  tab-width: 8
 *  c-basic-offset: 8
 *  c-indent-level: 8
 * End:
 */
