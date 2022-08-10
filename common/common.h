/*
 * Copyright (c) 2017 Jan Synáček
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by the University of
 *      California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 */

#ifndef _COMMON_H
#define _COMMON_H

#include "../config.h"

#include <netinet/in.h>

#define TIMEOUT 1000 /* ms */
#define RETRIES 5

#define E_TIMED_OUT -1
#define E_RECEIVED_ERROR -2
#define E_UNEXPECTED_PACKET -3
#define E_FAILED_TO_READ -4
#define E_FAILED_TO_WRITE -5
#define E_SYSTEM_ERROR -6
#define ERROR_MAXLEN 511

union sock_addr {
    struct sockaddr     sa;
    struct sockaddr_in  si;
#ifdef HAVE_IPV6
    struct sockaddr_in6 s6;
#endif
};

#define SOCKLEN(sock) \
    (((union sock_addr*)sock)->sa.sa_family == AF_INET ? \
    (sizeof(struct sockaddr_in)) : \
    (sizeof(union sock_addr)))

const char *opcode_to_str(unsigned short opcode);
int str_equal(const char *s1, const char *s2);
void set_verbose(int v);

int format_error(struct tftphdr *tp, char *error);
void die(const char *fmt, ...);
void die_on_error(struct tftphdr *tp);
void send_error(int sockfd, union sock_addr *to, const char *msg);
void send_ack(int sockfd, union sock_addr *to, unsigned short block);
int recv_with_timeout(int s, void *in, size_t len, int timeout);
int recvfrom_with_timeout(int s, void *in, size_t len, union sock_addr *from, int timeout);
int recvfrom_flags_with_timeout(int s,
                                void *in,
                                size_t len,
                                union sock_addr *from,
                                int timeout,
                                int flags);
int receiver(int sockfd,
             union sock_addr *server,
             size_t blocksize,
             int windowsize,
             int timeout,
             FILE *fp,
             unsigned long *received,
             char *error);

int sender(int sockfd,
           union sock_addr *server,
           size_t blocksize,
           int windowsize,
           int timeout,
           int rollover,
           FILE *fp,
           unsigned long *sent);
#endif
