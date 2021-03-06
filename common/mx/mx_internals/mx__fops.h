/*
 * Open-MX
 * Copyright © inria 2007-2009 (see AUTHORS file)
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

/*
 * This file mimics mx__fops.h from Myricom's MX distribution.
 * It is used to build applications on top of Open-MX using the MX internal API.
 */

#ifndef MX__FOPS_H
#define MX__FOPS_H

#include <stddef.h>

#ifdef __cpluscplus
extern "C" {
#endif

/* Prototype needed so that ugly OpenMPI >= 1.3 enables internal symbols at configure */
extern mx_return_t mx_open_board(int i, mx_endpt_handle_t *handle);

#ifdef __cpluscplus
}
#endif

#endif /* MX__FOPS_H */
