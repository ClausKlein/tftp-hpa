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

#include "common.h"

#include <poll.h>
#include <stdarg.h>
#include <syslog.h>

static char *pktbuf;
static int verbose;

const int SYNC_TIMEOUT = 50; /* ms */

void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "Fatal: ");
    vfprintf(stderr, fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

int format_error(struct tftphdr *tp, char *error)
{
    int r = 0;

    if (error)
        r = snprintf(error, ERROR_MAXLEN, "Error code %d: %s", ntohs(tp->th_code), tp->th_msg);
    return r;
}

void die_on_error(struct tftphdr *tp)
{
    char error[ERROR_MAXLEN];

    snprintf(error, ERROR_MAXLEN, "Error code %d: %s", ntohs(tp->th_code), tp->th_msg);
    fprintf(stderr, "%s\n", error);
    exit(1);
}

void send_error(int sockfd, union sock_addr *to, const char *msg)
{
    char buf[516];
    struct tftphdr *out = (struct tftphdr *)buf;
    int len;

    out->th_opcode = htons(ERROR);
    out->th_code = htons(EUNDEF);

    len = strlen(msg) + 1;
    memset(buf, 0, 516);
    memcpy(out->th_msg, msg, len > 511 ? 511 : len);
    len += 4;

    if (to) {
        if (sendto(sockfd, out, len, 0, &to->sa, SOCKLEN(to)) != len)
            die("send_error: sendto: %s", strerror(errno));
    } else {
        if (send(sockfd, out, len, 0) != len)
            die("send_error: send: %s", strerror(errno));
    }
}

static void _send_ack(int sockfd, union sock_addr *to, unsigned short block, int check_errors)
{
    struct tftphdr out;
    out.th_opcode = htons(ACK);
    out.th_block  = htons(block);

    if (to) {
        if (sendto(sockfd, &out, 4, 0, &to->sa, SOCKLEN(to)) != 4 && check_errors)
            die("send_ack: sendto: %s", strerror(errno));
    } else {
        if (send(sockfd, &out, 4, 0) != 4 && check_errors)
            die("send_ack: send: %s", strerror(errno));
    }
}

void send_ack(int sockfd, union sock_addr *to, unsigned short block)
{
    _send_ack(sockfd, to, block, 1);
}


static size_t read_data(FILE *fp,
                        size_t blocksize,
                        unsigned short block,
                        int seek,
                        struct tftphdr *out)
{
    out->th_opcode = htons(DATA);
    out->th_block  = htons(block);
    if (seek)
        (void) fseek(fp, seek, SEEK_CUR);
    return fread(pktbuf + 4, 1, blocksize, fp);
}

static size_t write_data(FILE *fp, char *buf, size_t count, int convert)
{
    char wbuf[count];
    size_t i = 0, cnt = 0;
    static int was_cr = 0;

    /* TODO: jsynacek: I don't think any conversion should take place...
     * RFC 1350 says: "A host which receives netascii mode data must translate
     * the data to its own format."
     * That basically means nothing. What does "own format" even mean?
     * The original implementation translated \r\n to \n and skipped \0 bytes,
     * which aren't even legal in the netascii format.
     * However, I believe that the file should remain as is before and after
     * the transfer.
     */
    convert = 0;

    if (convert == 0)
        return fwrite(buf, 1, count, fp);

    /* Working conversion. Leave it as dead code for now. */
    if (was_cr && buf[0] == '\n') {
        wbuf[cnt++] = '\n';
        i = 1;
        (void) fseek(fp, -1, SEEK_CUR);
    }

    while(i < count) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            wbuf[cnt++] = '\n';
            ++i;
        } else if (buf[i] == '\0') {
            /* Skip it */
        } else {
            wbuf[cnt++] = buf[i];
        }
        ++i;
    }
    /* Preserve state between data chunks */
    was_cr = buf[i - 1] == '\r';

    if (fwrite(wbuf, 1, cnt, fp) == 0)
        return 0;

    return count;
}

void set_verbose(int v)
{
    verbose = v;
}

int recv_with_timeout(int s, void *in, size_t len, int timeout)
{
    return recvfrom_flags_with_timeout(s, in, len, NULL, timeout, 0);
}

int recvfrom_with_timeout(int s,
                          void *in,
                          size_t len,
                          union sock_addr *from,
                          int timeout)
{
    return recvfrom_flags_with_timeout(s, in, len, from, timeout, 0);
}

int recvfrom_flags_with_timeout(int s,
                                void *in,
                                size_t len,
                                union sock_addr *from,
                                int timeout,
                                int flags)
{
    socklen_t fromlen = sizeof(from);
    struct pollfd pfd;
    int r;

    pfd.fd = s;
    pfd.events = POLLIN;
    pfd.revents = 0;

    r = poll(&pfd, 1, timeout);
    if (r > 0) {
        if (from) {
            r = recvfrom(s, in, len, flags, &from->sa, &fromlen);
            if (r < 0)
                die("recvfrom_flags_with_timeout: recvfrom: %s", strerror(errno));
        } else {
            r = recv(s, in, len, flags);
            if (r < 0)
                die("recvfrom_flags_with_timeout: recv: %s", strerror(errno));
        }
    }

    return r;
}

int receiver(int sockfd,
             union sock_addr *server,
             size_t blocksize,
             int windowsize,
             int timeout,
             FILE *fp,
             unsigned long *received,
             char *error)
{
    struct tftphdr *tp;
    unsigned short tp_opcode, tp_block;
    unsigned short block = 1;
    unsigned long amount = 0;
    int pktsize = blocksize + 4;
    int window = 1;
    size_t size;
    int retries;
    int n, r = 0;

    pktbuf = calloc(pktsize, 1);
    if (!pktbuf)
        die("Out of memory!");
    tp = (struct tftphdr *)pktbuf;

    retries = RETRIES;
    do {
        n = recvfrom_with_timeout(sockfd, tp, pktsize, server, timeout);
        if (n == 0) {
            if (--retries <= 0) {
                r = E_TIMED_OUT;
                snprintf(error, ERROR_MAXLEN, "Timeout");
                goto abort;
            }
            continue;
        }
        retries = RETRIES;

        tp_opcode = ntohs(tp->th_opcode);
        tp_block  = ntohs(tp->th_block);

        if (tp_opcode == DATA) {
            if (tp_block == block) {
                size = write_data(fp, tp->th_data, n - 4, 0);
                if (size == 0 && ferror(fp)) {
                    send_error(sockfd, server, "Failed to write data");
                    r = E_FAILED_TO_WRITE;
                    snprintf(error, ERROR_MAXLEN, "Failed to write data");
                    goto abort;
                }
                amount += size;

                if (window++ >= windowsize || size != blocksize) {
                    send_ack(sockfd, server, block);
                    window = 1;
                }

                ++block;
            } else {
                do {
                    n = recvfrom_with_timeout(sockfd, pktbuf, pktsize, server, SYNC_TIMEOUT);
                } while (n > 0);
                if (windowsize > 0) {
                    send_ack(sockfd, server, block - 1);
                }
                window = 1;
                n = 0;
            }
        } else if (tp_opcode == ERROR) {
            format_error(tp, error);
            r = E_RECEIVED_ERROR;
            goto abort;
        } else {
            snprintf(error, ERROR_MAXLEN, "Unexpeted packet");
            r = E_UNEXPECTED_PACKET;
            goto abort;
        }
    } while (size == blocksize || n == 0);

    /* Last ack can get lost, let's try and resend it twice
     * to make it more likely that the ack gets to the sender.
     */
    --block;
    for (n = 0; n < 2; ++n) {
        usleep(SYNC_TIMEOUT * 1000);
        _send_ack(sockfd, server, block, 0);
    }

    if (received)
        *received = amount;
abort:
    free(pktbuf);
    return r;
}

int sender(int sockfd,
           union sock_addr *server,
           size_t blocksize,
           int windowsize,
           int timeout,
           int rollover,
           FILE *fp,
           unsigned long *sent)
{
    struct tftphdr *tp;
    unsigned short tp_opcode, tp_block;
    unsigned short block = 1;
    unsigned long amount = 0;
    int l_timeout = timeout;
    int pktsize = blocksize + 4;
    int window = 1;
    int seek = 0;
    int retries;
    size_t size;
    size_t done = 0;
    int n, r = 0;

    pktbuf = calloc(pktsize, 1);
    if (!pktbuf)
        die("Out of memory!");
    tp = (struct tftphdr *)pktbuf;

    retries = RETRIES;
    do {
        size = read_data(fp, blocksize, block, seek, tp);
        if (size == 0 && ferror(fp)) {
            send_error(sockfd, server, "Error while reading the file");
            r = E_FAILED_TO_READ;
            goto abort;
        }
        amount += size;
        seek = 0;

        if (server)
            n = sendto(sockfd, tp, size + 4, 0, &server->sa, SOCKLEN(server));
        else
            n = send(sockfd, tp, size + 4, 0);
        if (n != (int)(size + 4)) {
            syslog(LOG_WARNING, "tftpd: send: %m");
            r = E_SYSTEM_ERROR;
            goto abort;
        }

        if (window++ < windowsize) {
            /* Even if the entire window is not sent, the server should check for incoming packets
             * and react to out of order ACKs, or ERRORs.
             */
            l_timeout = 0;
        } else {
            l_timeout = timeout;
            window = 1;
        }
        done = size != blocksize;
        if (done) {
            l_timeout = timeout;
            window = 1;
        }

        n = recvfrom_with_timeout(sockfd, pktbuf, pktsize, server, l_timeout);
        if (n < 0) {
            syslog(LOG_WARNING, "tftpd: recv: %m");
            r = E_SYSTEM_ERROR;
            goto abort;
        } else if (l_timeout > 0 && n == 0) {
            seek = -size;
            if (--retries <= 0) {
                r = E_TIMED_OUT;
                goto abort;
            }
            done = 0;
            continue;
        }

        if (++block == 0)
            block = rollover;

        tp_opcode = ntohs(tp->th_opcode);
        tp_block  = ntohs(tp->th_block);

        if (tp_opcode == ACK) {
            if (tp_block != (unsigned short)(block - 1)) {
                int offset = tp_block;

                done = 0;
                do {
                    n = recvfrom_with_timeout(sockfd, pktbuf, pktsize, server, SYNC_TIMEOUT);
                } while (n > 0);
                /* This is a bit of a hack. Mismatched packets that are "over the edge" of the unsigned short
                 * have to be handled and a correct seek has to be issued. In theory, overflowing block number
                 * should not even be supported, as it is impossible to distinguish correctly if more than 2^16
                 * packets are sent, but the first ones are received later than the last ones.
                 */
                if (tp_block > 65000 && tp_block > (unsigned short)(block - 1))
                    offset -= 65535;
                seek = (offset - block + 2) * blocksize - size;
            }
            block = tp_block + 1;
            retries = RETRIES;
        } else if (tp_opcode == ERROR) {
            r = E_RECEIVED_ERROR;
            goto abort;
        }
    } while (!done);

    if (sent)
        *sent = amount;
abort:
    free(pktbuf);
    return r;
}
