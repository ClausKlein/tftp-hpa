/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2001-2006 H. Peter Anvin - All Rights Reserved
 *
 *   This program is free software available under the same license
 *   as the "OpenBSD" operating system, distributed at
 *   http://www.openbsd.org/.
 *
 * ----------------------------------------------------------------------- */

/*
 * recvfrom.h
 *
 * Header for recvfrom substitute
 *
 */

#include "../common/common.h"

int
myrecvfrom(int s, void *buf, int len, unsigned int flags,
           union sock_addr *from, union sock_addr *myaddr);
