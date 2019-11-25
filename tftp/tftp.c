/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
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

#include <poll.h>
#include <stdarg.h>
#include "common/tftpsubs.h"
#include "extern.h"

/* TODO: This 'peeraddr' global should be removed. */
extern union sock_addr peeraddr; /* filled in by main */
extern int f;                    /* the opened socket */
extern int trace_opt;
extern int verbose;
/* TODO: Adjust when blocksize is implemented. */
#define PKTSIZE    SEGSIZE+4
static char pktbuf[PKTSIZE];

static void printstats(const char *, unsigned long);
static void startclock(void);
static void stopclock(void);
static void timed_out(void)
{
    printf("client: timed out");
    exit(1);
}

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "fatal: client: ");
    vfprintf(stderr, fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

static size_t make_request(unsigned short opcode,
                           const char *name,
                           const char *mode,
                           int blocksize,
                           int windowsize,
                           struct tftphdr *out)
{
    char *cp, buf[16];
    size_t len;

    cp = (char *)&(out->th_stuff);

    out->th_opcode = htons(opcode);

    len = strlen(name) + 1;
    memcpy(cp, name, len);
    cp += len;

    len = strlen(mode) + 1;
    memcpy(cp, mode, len);
    cp += len;

    /* Don't include options with default values. */

    /* TODO: TBI in a separate patch. */
    (void) blocksize;
    /*if (blocksize != SEGSIZE) {
        len = strlen("blksize") + 1;
        memcpy(cp, "blksize", len);
        cp += len;
        if (snprintf(buf, 16, "%u", blocksize) < 0)
            die("out of memory");
        len = strlen(buf) + 1;
        memcpy(cp, buf, len);
        cp += len;
    }*/

    if (windowsize > 0) {
        len = strlen("windowsize") + 1;
        memcpy(cp, "windowsize", len);
        cp += len;
        if (snprintf(buf, 16, "%u", windowsize) < 0)
            die("out of memory");
        len = strlen(buf) + 1;
        memcpy(cp, buf, len);
        cp += len;
    }

    return (cp - (char *)out);
}

static void send_request(int sock,
                         union sock_addr *to,
                         short request,
                         const char *name,
                         const char *mode,
                         unsigned blocksize,
                         unsigned windowsize)
{
    struct tftphdr *out;
    size_t size;

    out = (struct tftphdr *)pktbuf;
    size = make_request(request, name, mode, blocksize, windowsize, out);

    if (sendto(sock, out, size, 0, &to->sa, SOCKLEN(to)) != (unsigned)size)
        die("send_request: sendto: %m");
}

static int wait_for_oack(int sock, union sock_addr *from, char **options, int *optlen)
{
    unsigned short in_opcode;
    struct tftphdr *in;
    int r;

    r = recvfrom_with_timeout(sock, pktbuf, sizeof(pktbuf), from, TIMEOUT);
    if (r == 0)
        return r;

    in = (struct tftphdr *)pktbuf;
    in_opcode = ntohs(in->th_opcode);

    if (in_opcode == ERROR)
        die_on_error(in);
    if (in_opcode != OACK)
        return -in_opcode;

    *options = pktbuf + 2;
    *optlen = r - 2;

    return 1;
}

/*
 * Send the requested file.
 */
void tftp_sendfile(int fd, const char *name, const char *mode, int windowsize)
{
    union sock_addr server = peeraddr;
    unsigned long amount = 0;
    int blocksize = SEGSIZE;
    char *options;
    int optlen;
    int retries;
    FILE *fp;
    int n, r;

    set_verbose(trace_opt + verbose);

    startclock();
    send_request(f, &server, WRQ, name, mode, blocksize, windowsize);

    /* If no windowsize was specified on the command line,
     * don't bother with options.
     * When blocksize is supported, this should actually only be called
     * if no options were sent in the RRQ.
     */
    if (windowsize < 0)
        goto no_options;
    retries = RETRIES;
    do {
        r = wait_for_oack(f, &server, &options, &optlen);
        if (r < 0) {
            return;
        }

        /* Parse returned options. */
        n = 0;
        if (r != 0) {
            char *opt, *val;
            int got_ws = 0;
            int v;

            while (n < optlen) {
                opt = options + n;
                n += strlen(opt) + 1;
                val = options + n;
                if (str_equal(opt, "windowsize") && windowsize != 1) {
                    v = atoi(val);
                    if (v != windowsize)
                        printf("client: server negotiated different windowsize: %d", v);
                    /* Assumes v > 0, it probably shouldn't. */
                    windowsize = v;
                    got_ws = 1;
                }
                n += strlen(val) + 1;
            }

            if (got_ws == 0 && windowsize != 1) {
                windowsize = 1;
                printf("client: server didn't negotiate windowsize, continuing with windowsize=1");
            }
        }
    } while (r == 0 && --retries > 0);
    if (retries <= 0)
        timed_out();

no_options:
    if (windowsize < 0) {
        struct tftphdr *tp = (struct tftphdr *)pktbuf;

        retries = RETRIES;
        do {
            r = recvfrom_with_timeout(f, pktbuf, PKTSIZE, &server, TIMEOUT);
            if (r == 0) {
                /* Timed out. */
                continue;
            }
            if (ntohs(tp->th_opcode) == ACK && ntohs(tp->th_block) == 0) {
                break;
            } else if (ntohs(tp->th_opcode) == ERROR) {
                die_on_error(tp);
            }
        } while (r == 0 && --retries > 0);
        if (retries <= 0)
            timed_out();
    }
    fp = fdopen(fd, "r");
    r = sender(f, &server, blocksize, windowsize, TIMEOUT, 0, fp, &amount);
    if (r < 0)
        exit(1);

    stopclock();
    if (amount > 0)
        printstats("Sent", amount);
}


/*
 * Receive a file.
 */
void tftp_recvfile(int fd, const char *name, const char *mode, int windowsize)
{
    union sock_addr server = peeraddr;
    unsigned long amount = 0;
    int blocksize = SEGSIZE;
    char *options, error[ERROR_MAXLEN];
    int optlen;
    int retries;
    FILE *fp;
    int n, r;

    set_verbose(trace_opt + verbose);

    startclock();

    send_request(f, &server, RRQ, name, mode, blocksize, windowsize);
    /* If no windowsize was specified on the command line,
     * don't bother with options.
     * When blocksize is supported, this should actually only be called
     * if no options were sent in the RRQ.
     */
    if (windowsize < 0)
        goto no_options;
    retries = RETRIES;
    do {
        r = wait_for_oack(f, &server, &options, &optlen);
        if (r < 0) {
            return;
        }

        /* Parse returned options. */
        n = 0;
        if (r != 0) {
            char *opt, *val;
            int got_ws = 0;
            int v;

            while (n < optlen) {
                opt = options + n;
                n += strlen(opt) + 1;
                val = options + n;
                if (str_equal(opt, "windowsize") && windowsize != 1) {
                    v = atoi(val);
                    if (v != windowsize)
                        printf("client: server negotiated different windowsize: %d", v);
                    /* Assumes v > 0, it probably shouldn't. */
                    windowsize = v;
                    got_ws = 1;
                }
                n += strlen(val) + 1;
            }

            if (got_ws == 0 && windowsize != 1) {
                windowsize = 1;
                printf("client: server didn't negotiate windowsize, continuing with windowsize=1");
            }
        }
    } while (r == 0 && --retries > 0);
    if (retries <= 0)
        timed_out();

    send_ack(f, &server, 0);

no_options:
    fp = fdopen(fd, "w");
    r = receiver(f, &server, blocksize, windowsize, TIMEOUT, fp, &amount, error);
    if (r < 0) {
        fprintf(stderr, "%s\n", error);
        exit(1);
    }

    stopclock();
    if (amount > 0)
        printstats("Received", amount);
    fclose(fp);
}

struct timeval tstart;
struct timeval tstop;

static void startclock(void)
{
    (void)gettimeofday(&tstart, NULL);
}

static void stopclock(void)
{

    (void)gettimeofday(&tstop, NULL);
}

static void printstats(const char *direction, unsigned long amount)
{
    double delta;

    delta = (tstop.tv_sec + (tstop.tv_usec / 1000000.0)) -
        (tstart.tv_sec + (tstart.tv_usec / 1000000.0));
    if (verbose) {
        printf("%s %lu bytes in %.1f seconds", direction, amount, delta);
        /* TODO: Change the statistics in a separate patch (bits???)! */
        printf(" [%.0f bit/s]", (amount * 8.) / delta);
        putchar('\n');
    }
}

