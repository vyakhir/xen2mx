/*
 * Open-MX
 * Copyright © INRIA 2007 (see AUTHORS file)
 *
 * The development of this software has been funded by Myricom, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#include <sys/ioctl.h>

#include "omx_lib.h"
#include "omx_request.h"

static inline omx_return_t
omx__submit_isend_tiny(struct omx_endpoint *ep,
		       void *buffer, size_t length,
		       uint64_t match_info,
		       struct omx__partner * partner, omx__seqnum_t seqnum,
		       void *context, union omx_request **requestp)
{
  union omx_request * req;
  struct omx_cmd_send_tiny tiny_param;
  omx_return_t ret;
  int err;

  req = omx__request_alloc(OMX_REQUEST_TYPE_SEND_TINY);
  if (!req) {
    ret = OMX_NO_RESOURCES;
    goto out;
  }

  tiny_param.hdr.dest_addr = partner->board_addr;
  tiny_param.hdr.dest_endpoint = partner->endpoint_index;
  tiny_param.hdr.match_info = match_info;
  tiny_param.hdr.length = length;
  tiny_param.hdr.seqnum = seqnum;
  tiny_param.hdr.session_id = partner->session_id;
  tiny_param.hdr.dest_src_peer_index = partner->dest_src_peer_index;
  memcpy(tiny_param.data, buffer, length);

  err = ioctl(ep->fd, OMX_CMD_SEND_TINY, &tiny_param);
  if (err < 0) {
    ret = omx__errno_to_return("ioctl SEND_TINY");
    goto out_with_req;
  }
  /* no need to wait for a done event, tiny is synchronous */

  req->generic.partner = partner;
  omx__partner_to_addr(partner, &req->generic.status.addr);
  req->send.seqnum = seqnum;
  req->generic.status.context = context;
  req->generic.status.match_info = match_info;
  req->generic.state = OMX_REQUEST_STATE_DONE;
  omx__enqueue_request(&ep->done_req_q, req);

  *requestp = req;
  return OMX_SUCCESS;

 out_with_req:
  omx__request_free(req);
 out:
  return ret;
}

static inline omx_return_t
omx__submit_isend_small(struct omx_endpoint *ep,
			void *buffer, size_t length,
			uint64_t match_info,
			struct omx__partner * partner, omx__seqnum_t seqnum,
			void *context, union omx_request **requestp)
{
  union omx_request * req;
  struct omx_cmd_send_small small_param;
  omx_return_t ret;
  int err;

  req = omx__request_alloc(OMX_REQUEST_TYPE_SEND_SMALL);
  if (!req) {
    ret = OMX_NO_RESOURCES;
    goto out;
  }

  small_param.dest_addr = partner->board_addr;
  small_param.dest_endpoint = partner->endpoint_index;
  small_param.match_info = match_info;
  small_param.length = length;
  small_param.vaddr = (uintptr_t) buffer;
  small_param.seqnum = seqnum;
  small_param.session_id = partner->session_id;
  small_param.dest_src_peer_index = partner->dest_src_peer_index;

  err = ioctl(ep->fd, OMX_CMD_SEND_SMALL, &small_param);
  if (err < 0) {
    ret = omx__errno_to_return("ioctl SEND_SMALL");
    goto out_with_req;
  }
  /* no need to wait for a done event, small is synchronous */

  req->generic.partner = partner;
  omx__partner_to_addr(partner, &req->generic.status.addr);
  req->send.seqnum = seqnum;
  req->generic.status.context = context;
  req->generic.status.match_info = match_info;
  req->generic.state = OMX_REQUEST_STATE_DONE;
  omx__enqueue_request(&ep->done_req_q, req);

  *requestp = req;
  return OMX_SUCCESS;

 out_with_req:
  omx__request_free(req);
 out:
  return ret;
}

static inline omx_return_t
omx__submit_isend_medium(struct omx_endpoint *ep,
			 void *buffer, size_t length,
			 uint64_t match_info,
			 struct omx__partner * partner, omx__seqnum_t seqnum,
			 void *context, union omx_request **requestp)
{
  union omx_request * req;
  struct omx_cmd_send_medium medium_param;
  uint32_t remaining = length;
  uint32_t offset = 0;
  int sendq_index[8];
  int frags;
  int i;
  omx_return_t ret;
  int err;

  frags = OMX_MEDIUM_FRAGS_NR(length);
  omx__debug_assert(frags <= 8); /* for the sendq_index array above */

  req = omx__request_alloc(OMX_REQUEST_TYPE_SEND_MEDIUM);
  if (!req) {
    ret = OMX_NO_RESOURCES;
    goto out;
  }

  if (omx__endpoint_sendq_map_get(ep, frags, req, sendq_index) < 0)
    /* FIXME: queue */
    assert(0);

  medium_param.dest_addr = partner->board_addr;
  medium_param.dest_endpoint = partner->endpoint_index;
  medium_param.match_info = match_info;
  medium_param.frag_pipeline = OMX_MEDIUM_FRAG_PIPELINE;
  medium_param.msg_length = length;
  medium_param.seqnum = seqnum;
  medium_param.session_id = partner->session_id;
  medium_param.dest_src_peer_index = partner->dest_src_peer_index;

  for(i=0; i<frags; i++) {
    unsigned chunk = remaining > OMX_MEDIUM_FRAG_LENGTH_MAX
      ? OMX_MEDIUM_FRAG_LENGTH_MAX : remaining;
    medium_param.frag_length = chunk;
    medium_param.frag_seqnum = i;
    medium_param.sendq_page_offset = sendq_index[i];
    omx__debug_printf("sending medium seqnum %d pipeline 2 length %d of total %ld\n",
		      i, chunk, (unsigned long) length);
    memcpy(ep->sendq + (sendq_index[i] << OMX_MEDIUM_FRAG_LENGTH_MAX_SHIFT), buffer + offset, chunk);

    err = ioctl(ep->fd, OMX_CMD_SEND_MEDIUM, &medium_param);
    if (err < 0) {
      ret = omx__errno_to_return("ioctl SEND_MEDIUM");
      goto out_with_req;
    }

    remaining -= chunk;
    offset += chunk;
  }

  /* need to wait for a done event, since the sendq pages
   * might still be in use
   */
  req->generic.partner = partner;
  omx__partner_to_addr(partner, &req->generic.status.addr);
  req->send.seqnum = seqnum;
  req->send.specific.medium.frags_pending_nr = frags;
  req->generic.status.context = context;
  req->generic.status.match_info = match_info;
  req->generic.state = OMX_REQUEST_STATE_PENDING;
  omx__enqueue_request(&ep->sent_req_q, req);

  *requestp = req;
  return OMX_SUCCESS;

 out_with_req:
  omx__request_free(req);
 out:
  return ret;
}

static inline omx_return_t
omx__submit_isend_large(struct omx_endpoint *ep,
			void *buffer, size_t length,
			uint64_t match_info,
			struct omx__partner * partner, omx__seqnum_t seqnum,
			void *context, union omx_request **requestp)
{
  union omx_request * req;
  struct omx_cmd_send_rndv rndv_param;
  struct omx__large_region *region;
  omx_return_t ret;
  int err;

  req = omx__request_alloc(OMX_REQUEST_TYPE_SEND_LARGE);
  if (!req) {
    ret = OMX_NO_RESOURCES;
    goto out;
  }

  ret = omx__register_region(ep, buffer, length, &region);
  if (ret != OMX_SUCCESS)
    goto out_with_req;

  rndv_param.hdr.dest_addr = partner->board_addr;
  rndv_param.hdr.dest_endpoint = partner->endpoint_index;
  rndv_param.hdr.match_info = match_info;
  rndv_param.hdr.length = 8;
  rndv_param.hdr.seqnum = seqnum;
  rndv_param.hdr.session_id = partner->session_id;
  rndv_param.hdr.dest_src_peer_index = partner->dest_src_peer_index;

  *(uint32_t *) &(rndv_param.data[0]) = length;
  *(uint8_t *) &(rndv_param.data[4]) = region->id;
  *(uint8_t *) &(rndv_param.data[5]) = region->seqnum;
  *(uint16_t *) &(rndv_param.data[6]) = region->offset;

  err = ioctl(ep->fd, OMX_CMD_SEND_RNDV, &rndv_param);
  if (err < 0) {
    ret = omx__errno_to_return("ioctl SEND_RNDV");
    goto out_with_reg;
  }
  /* no need to wait for a done event, rndv is synchronous */

  req->send.specific.large.region = region;
  region->user = req;

  req->generic.partner = partner;
  omx__partner_to_addr(partner, &req->generic.status.addr);
  req->send.seqnum = seqnum;
  req->generic.status.context = context;
  req->generic.status.match_info = match_info;
  req->generic.state = OMX_REQUEST_STATE_PENDING;
  omx__enqueue_request(&ep->large_send_req_q, req);

  *requestp = req;
  return OMX_SUCCESS;

 out_with_reg:
  omx__deregister_region(ep, region);
 out_with_req:
  omx__request_free(req);
 out:
  return ret;
}

omx_return_t
omx_isend(struct omx_endpoint *ep,
	  void *buffer, size_t length,
	  uint64_t match_info,
	  omx_endpoint_addr_t dest_endpoint,
	  void *context, union omx_request **requestp)
{
  struct omx__partner * partner;
  omx__seqnum_t seqnum;
  omx_return_t ret;

  partner = omx__partner_from_addr(&dest_endpoint);
  seqnum = partner->next_send_seq;
  omx__debug_printf("sending %ld bytes using seqnum %d\n",
		    (unsigned long) length, seqnum);

  if (length <= OMX_TINY_MAX) {
    ret = omx__submit_isend_tiny(ep,
				 buffer, length,
				 match_info,
				 partner, seqnum,
				 context, requestp);
  } else if (length <= OMX_SMALL_MAX) {
    ret = omx__submit_isend_small(ep,
				  buffer, length,
				  match_info,
				  partner, seqnum,
				  context, requestp);
  } else if (length <= OMX_MEDIUM_MAX) {
    ret = omx__submit_isend_medium(ep,
				   buffer, length,
				   match_info,
				   partner, seqnum,
				   context, requestp);
  } else {
    ret = omx__submit_isend_large(ep,
				  buffer, length,
				  match_info,
				  partner, seqnum,
				  context, requestp);

  }

  if (ret == OMX_SUCCESS) {
    /* increase at the end, to avoid having to decrease back in case of error */
    partner->next_send_seq++;
  }

  /* progress a little bit */
  omx__progress(ep);

  return ret;
}
