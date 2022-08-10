/*
 * Copyright (c) 1983, 1993
 *      The Regents of the University of California.  All rights reserved.
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

#include "extern.h"

#include <stdarg.h>
#include <poll.h>

static char pktbuf[PKTSIZE];

static void printstats(const char *, unsigned long);
static void startclock(void);
static void stopclock(void);
static void timed_out(void)
{
    printf("client: timed out\n");
    exit(1);
}

static size_t make_request(unsigned short opcode,
                           const char *name,
                           const char *mode,
                           size_t blocksize,
                           int windowsize,
                           size_t tsize,
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

    if (blocksize != SEGSIZE) {
        len = strlen("blksize") + 1;
        memcpy(cp, "blksize", len);
        cp += len;
        if (snprintf(buf, 16, "%lu", blocksize) < 0)
            die("out of memory");
        len = strlen(buf) + 1;
        memcpy(cp, buf, len);
        cp += len;
    }

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

    {
        len = strlen("tsize") + 1;
        memcpy(cp, "tsize", len);
        cp += len;
        if (snprintf(buf, 16, "%zu", tsize) < 0)
            die("out of memory");
        printf("client: option request tsize:%s\n", buf);
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
                         size_t blocksize,
                         unsigned windowsize,
                         size_t tsize)
{
    struct tftphdr *out;
    size_t size;

    out = (struct tftphdr *)pktbuf;
    size = make_request(request, name, mode, blocksize, windowsize, tsize, out);

    if (sendto(sock, out, size, 0, &to->sa, SOCKLEN(to)) != (unsigned)size)
        die("send_request: sendto: %s", strerror(errno));
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

static size_t get_tsize(int fd) {
    struct stat stbuf = {};
    size_t tsize = 0;
    if (fstat(fd, &stbuf) >= 0) {
        tsize = stbuf.st_size;
    } else {
        perror("fstat()");
    }

    return tsize;
}

struct option_values {
    int windowsize;
    size_t blocksize;
    size_t tsize;
};

static int tftp_parse_oack(int sock, union sock_addr *from, struct option_values *def)
{
    char *options;
    int optlen;
    int retries = RETRIES;
    int r;
    do {
        r = wait_for_oack(sock, from, &options, &optlen);
        if (r < 0) {
            return 0;   // ACK received! CK
        }

        /* Parse returned options. */
        int n = 0;
        if (r != 0) {
            char *opt, *val;

            while (n < optlen) {
                opt = options + n;
                n += strlen(opt) + 1;
                val = options + n;

                if (str_equal(opt, "windowsize") && def->windowsize != 1) {
                    int v = atoi(val);  // TODO Use strtoul(() or strtoumax()
                    if (v != def->windowsize)
                        printf("client: server negotiated different windowsize: %d\n", v);
                    /* FIXME Assumes v > 0, it probably shouldn't. */
                    def->windowsize = v;
                }

                if (str_equal(opt, "blocksize") && def->blocksize != SEGSIZE) {
                    size_t bs = strtoul(val, NULL, 10);  // TBD Use strtoumax()
                    if (bs != def->blocksize)
                        printf("client: server negotiated different blocksize: %zu\n", bs);
                    if ((bs < SEGSIZE) || (bs > MAX_SEGSIZE))
                        die("client: server didn't negotiate valid blocksize");

                    def->blocksize = bs;
                }

                if (str_equal(opt, "tsize")) {
                    size_t ts = strtoumax(val, NULL, 10);
                    printf("client: server negotiated tsize: %zu\n", ts);
                    if (ts != def->tsize)
                        def->tsize = ts;
                }

                n += strlen(val) + 1;
            }

            if (def->windowsize < 1) {
                def->windowsize = 1;
                printf("client: server didn't negotiate windowsize, continuing with windowsize=1\n");
            }
        }
    } while (r == 0 && --retries > 0);

    if (retries <= 0)
        timed_out();

    return 1;   // OK
}


/*
 * Send the requested file.
 */
void tftp_sendfile(int fd, const char *name, const char *mode, int windowsize)
{
    union sock_addr server = g_peeraddr;
    unsigned long amount = 0;
    size_t blocksize = g_blocksize;
    FILE *fp;
    size_t tsize = get_tsize(fd);  // TODO only useable for mode == "octet" ! CK

    set_verbose(g_trace_opt + g_verbose);

    startclock();
    send_request(g_s, &server, WRQ, name, mode, blocksize, windowsize, tsize);

#if 0
    /* If no windowsize was specified on the command line,
     * don't bother with options.
     * When blocksize is supported, this should actually only be called
     * if no options were sent in the WRQ.
     */
    if ((windowsize < 0) && (blocksize == SEGSIZE))
        goto no_options;
#endif

    struct option_values values = {windowsize, blocksize, tsize};
    int ok = tftp_parse_oack(g_s, &server, &values);
    if (!ok) return;

#if 0
//XXX no_options:
    if (!ok) {
        int r;
        struct tftphdr *tp = (struct tftphdr *)pktbuf;
        int retries = RETRIES;
        do {
            r = recvfrom_with_timeout(g_s, pktbuf, sizeof(pktbuf), &server, TIMEOUT);
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
#endif

    fp = fdopen(fd, "r");
    int r = sender(g_s, &server, values.blocksize, values.windowsize, TIMEOUT, 0, fp, &amount);
    if (r < 0)
        exit(1);

    stopclock();
    if (amount > 0)
        printstats("Sent", amount);
    fclose(fp);
}

/*
 * Receive a file.
 */
void tftp_recvfile(int fd, const char *name, const char *mode, int windowsize)
{
    union sock_addr server = g_peeraddr;
    unsigned long amount = 0;
    size_t blocksize = g_blocksize;
    char error[ERROR_MAXLEN] = {};
    FILE *fp;
    size_t tsize = 0;

    set_verbose(g_trace_opt + g_verbose);

    startclock();
    send_request(g_s, &server, RRQ, name, mode, blocksize, windowsize, tsize);

#if 0
    /* If no windowsize was specified on the command line,
     * don't bother with options.
     * When blocksize is supported, this should actually only be called
     * if no options were sent in the RRQ.
     */
    if ((windowsize < 0) && (blocksize == SEGSIZE))
        goto no_options;
#endif

    struct option_values values = {windowsize, blocksize, tsize};
    int ok = tftp_parse_oack(g_s, &server, &values);
    if (!ok) return;

#if 0
//XXX no_options:
    if (!ok) {
        int r;
        struct tftphdr *tp = (struct tftphdr *)pktbuf;
        int retries = RETRIES;
        do {
            r = recvfrom_with_timeout(g_s, pktbuf, sizeof(pktbuf), &server, TIMEOUT);
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
#endif

    send_ack(g_s, &server, 0);

    fp = fdopen(fd, "w");
    int r = receiver(g_s, &server, values.blocksize, values.windowsize, TIMEOUT, fp, &amount, error);
    if (r < 0) {
        fprintf(stderr, "client: %s\n", error);
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
    if (g_verbose) {
        printf("%s %lu bytes in %.1f seconds\n", direction, amount, delta);
        /* TODO: Change the statistics in a separate patch (bits???)! */
        printf(" [%.0f bit/s]\n", (amount * 8.) / delta);
        putchar('\n');
    }
}

