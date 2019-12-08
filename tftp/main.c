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

/* Many bug fixes are from Jim Guyton <guyton@rand-unix> */

/*
 * TFTP User Program -- Command Interface.
 */
#include <sys/file.h>
#include <ctype.h>
#ifdef WITH_READLINE
#include <readline/readline.h>
#ifdef HAVE_READLINE_HISTORY_H
#include <readline/history.h>
#endif
#endif

#define RTIMEOUT        1       /* secs between rexmt's */
#define LBUFLEN         200     /* size of input buffer */

/* global variables, should be prevented! CK */
union sock_addr g_peeraddr;
size_t g_blocksize = SEGSIZE;
int g_s = -1;
int g_trace_opt;
int g_verbose;

struct modes {
    const char *m_name;
    const char *m_mode;
    int m_openflags;
};

static const struct modes modes[] = {
    {"netascii", "netascii", O_TEXT},
    {"ascii", "netascii", O_TEXT},
    {"octet", "octet", O_BINARY},
    {"binary", "octet", O_BINARY},
    {"image", "octet", O_BINARY},
    {0, 0, 0}
};

#define MODE_OCTET    (&modes[2])
#define MODE_NETASCII (&modes[0])
#define MODE_DEFAULT  MODE_NETASCII

#ifdef HAVE_IPV6
static int ai_fam = AF_UNSPEC;
static int ai_fam_sock = AF_UNSPEC;
#else
static int ai_fam = AF_INET;
static int ai_fam_sock = AF_INET;
#endif

static u_short port;
static int literal;
static int connected;
static const struct modes *mode;
#ifdef WITH_READLINE
static char *line = NULL;
static char *remote_pth = NULL;
#else
static char line[LBUFLEN];
static char remote_pth[LBUFLEN];
#endif
static int margc;
static char **margv;
static int sizeof_margv=0;
static const char *prompt = "tftp> ";
static sigjmp_buf toplevel;
static void intr(int);
static struct servent *sp;
static int portrange = 0;
static unsigned int portrange_from = 0;
static unsigned int portrange_to = 0;
static int windowsize = -1;

static void get(int, char **);
static void help(int, char **);
static void modecmd(int, char **);
static void put(int, char **);
static void quit(int, char **);
static void setascii(int, char **);
static void setbinary(int, char **);
static void setpeer(int, char **);
static void setrexmt(int, char **);
static void settimeout(int, char **);
static void settrace(int, char **);
static void setverbose(int, char **);
static void status(int, char **);
static void setliteral(int, char **);

static void command(void);

static void getusage(char *);
static void makeargv(void);
static void putusage(char *);
static void settftpmode(const struct modes *);

#define HELPINDENT (sizeof("connect"))

struct cmd {
    const char *name;
    const char *help;
    void (*handler) (int, char **);
};

struct cmd cmdtab[] = {
    {"connect",
     "connect to remote tftp",
     setpeer},
    {"mode",
     "set file transfer mode",
     modecmd},
    {"put",
     "send file",
     put},
    {"get",
     "receive file",
     get},
    {"quit",
     "exit tftp",
     quit},
    {"verbose",
     "toggle verbose mode",
     setverbose},
    {"trace",
     "toggle packet tracing",
     settrace},
    {"literal",
     "toggle literal mode, ignore ':' in file name",
     setliteral},
    {"status",
     "show current status",
     status},
    {"binary",
     "set mode to octet",
     setbinary},
    {"ascii",
     "set mode to netascii",
     setascii},
    {"rexmt",
     "set per-packet transmission timeout",
     setrexmt},
    {"timeout",
     "set total retransmission timeout",
     settimeout},
    {"?",
     "print help information",
     help},
    {"help",
     "print help information",
     help},
    {0, 0, 0}
};

struct cmd *getcmd(char *);
char *tail(char *);

char *xstrdup(const char *);

static const char *program;

static void usage(int errcode)
{
    fprintf(stderr,
#ifdef HAVE_IPV6
            "Usage: %s [-4][-6][-v][-V][-l][-m mode][-w size][-B blocksize] "
#else
            "Usage: %s [-v][-V][-l][-m mode][-w size][-B blocksize] "
#endif
            "[-R port:port] [host [port]] [-c command]\n",
            program);
    exit(errcode);
}

int main(int argc, char *argv[])
{
    union sock_addr sa;
    int arg;
    static int pargc, peerargc;
    static int iscmd = 0;
    char **pargv;
    const char *optx;
    char *peerargv[3];

    program = argv[0];

    mode = MODE_DEFAULT;

    peerargv[0] = argv[0];
    peerargc = 1;

    for (arg = 1; !iscmd && arg < argc; arg++) {
        if (argv[arg][0] == '-') {
            for (optx = &argv[arg][1]; *optx; optx++) {
                switch (*optx) {
                case '4':
                    ai_fam = AF_INET;
                    break;
#ifdef HAVE_IPV6
                case '6':
                    ai_fam = AF_INET6;
                    break;
#endif
                case 'v':
                    g_verbose = 1;
                    break;
                case 'V':
                    /* Print version and configuration to stdout and exit */
                    printf("%s\n", TFTP_CONFIG_STR);
                    exit(0);
                case 'l':
                    literal = 1;
                    break;
                case 'm':
                    if (++arg >= argc)
                        usage(EX_USAGE);
                    {
                        const struct modes *p;

                        for (p = modes; p->m_name; p++) {
                            if (!strcmp(argv[arg], p->m_name))
                                break;
                        }
                        if (p->m_name) {
                            settftpmode(p);
                        } else {
                            fprintf(stderr, "%s: invalid mode: %s\n",
                                    argv[0], argv[arg]);
                            exit(EX_USAGE);
                        }
                    }
                    break;
                case 'c':
                    iscmd = 1;
                    break;
                case 'R':
                    if (++arg >= argc)
                        usage(EX_USAGE);
                    if (sscanf
                        (argv[arg], "%u:%u", &portrange_from,
                         &portrange_to) != 2
                        || portrange_from > portrange_to
                        || portrange_to > 65535) {
                        fprintf(stderr, "Bad port range: %s\n", argv[arg]);
                        exit(EX_USAGE);
                    }
                    portrange = 1;
                    break;
                case 'w':
                    if (++arg >= argc)
                        usage(EX_USAGE);
                    windowsize = atoi(argv[arg]);
                    if (windowsize <= 0 || windowsize > 64) {
                        fprintf(stderr, "Bad window size: %s (1-64)\n", argv[arg]);
                        exit(EX_USAGE);
                    }
                    break;
                case 'B':
                    if (++arg >= argc)
                        usage(EX_USAGE);
                    g_blocksize = atoi(argv[arg]);
                    if (g_blocksize < SEGSIZE || g_blocksize > MAX_SEGSIZE) {
                        fprintf(stderr, "Bad block size: %s (%u-%u)\n", argv[arg],
                                SEGSIZE, MAX_SEGSIZE);
                        exit(EX_USAGE);
                    }
                    break;
                case 'h':
                default:
                    usage(*optx == 'h' ? 0 : EX_USAGE);
                }
            }
        } else {
            if (peerargc >= 3)
                usage(EX_USAGE);

            peerargv[peerargc++] = argv[arg];
        }
    }

    ai_fam_sock = ai_fam;

    pargv = argv + arg;
    pargc = argc - arg;

    sp = getservbyname("tftp", "udp");
    if (sp == 0) {
        /* Use canned values */
        if (g_verbose)
            fprintf(stderr,
                    "tftp: tftp/udp: unknown service, faking it...\n");
        sp = xmalloc(sizeof(struct servent));
        sp->s_name = (char *)"tftp";
        sp->s_aliases = NULL;
        sp->s_port = htons(IPPORT_TFTP);
        sp->s_proto = (char *)"udp";
    }

    bsd_signal(SIGINT, intr);

    if (peerargc) {
        /* Set peer */
        if (sigsetjmp(toplevel, 1) != 0)
            exit(EX_NOHOST);
        setpeer(peerargc, peerargv);
    }

    if (ai_fam_sock == AF_UNSPEC)
        ai_fam_sock = AF_INET;

    g_s = socket(ai_fam_sock, SOCK_DGRAM, 0);
    if (g_s < 0) {
        perror("tftp: socket");
        exit(EX_OSERR);
    }
    bzero(&sa, sizeof(sa));
    sa.sa.sa_family = ai_fam_sock;
    if (pick_port_bind(g_s, &sa, portrange_from, portrange_to)) {
        perror("tftp: bind");
        exit(EX_OSERR);
    }

    if (iscmd && pargc) {
        /* -c specified; execute command and exit */
        struct cmd *c;

        if (sigsetjmp(toplevel, 1) != 0)
            exit(EX_UNAVAILABLE);

        c = getcmd(pargv[0]);
        if (c == (struct cmd *)-1 || c == (struct cmd *)0) {
            fprintf(stderr, "%s: invalid command: %s\n", argv[0],
                    pargv[1]);
            exit(EX_USAGE);
        }
        (*c->handler) (pargc, pargv);
        exit(0);
    }
#ifdef WITH_READLINE
#ifdef HAVE_READLINE_HISTORY_H
    using_history();
#endif
#endif

    if (sigsetjmp(toplevel, 1) != 0)
        (void)putchar('\n');
    command();

    return 0;                   /* Never reached */
}

static char *hostname;

/* Called when a command is incomplete; modifies
   the global variable "line" */
static void getmoreargs(const char *partial, const char *mprompt)
{
#ifdef WITH_READLINE
    char *eline;
    int len, elen;

    len = strlen(partial);
    eline = readline(mprompt);
    if (!eline)
        exit(0);                /* EOF */

    elen = strlen(eline);

    if (line) {
        free(line);
        line = NULL;
    }
    if (remote_pth) {
        free(remote_pth);
        remote_pth = NULL;
    }
    line = xmalloc(len + elen + 1);
    strcpy(line, partial);
    strcpy(line + len, eline);
    free(eline);

#ifdef HAVE_READLINE_HISTORY_H
    add_history(line);
#endif
#else
    int len = strlen(partial);

    strcpy(line, partial);
    fputs(mprompt, stdout);
    if (fgets(line + len, LBUFLEN - len, stdin) == 0)
        if (feof(stdin))
            exit(0);            /* EOF */
#endif
}

void setpeer(int argc, char *argv[])
{
    int err;

    if (argc < 2) {
        getmoreargs("connect ", "(to) ");
        makeargv();
        argc = margc;
        argv = margv;
    }
    if ((argc < 2) || (argc > 3)) {
        printf("usage: %s host-name [port]\n", argv[0]);
        return;
    }

    g_peeraddr.sa.sa_family = ai_fam;
    err = set_sock_addr(argv[1], &g_peeraddr, &hostname);
    if (err) {
        printf("Error: %s\n", gai_strerror(err));
        printf("%s: unknown host\n", argv[1]);
        connected = 0;
        return;
    }
    ai_fam = g_peeraddr.sa.sa_family;
    if (g_s == -1) { /* socket not open */
        ai_fam_sock = ai_fam;
    } else { /* socket was already open */
        if (ai_fam_sock != ai_fam) { /* need reopen socken for new family */
            union sock_addr sa;

            close(g_s);
            ai_fam_sock = ai_fam;
            g_s = socket(ai_fam_sock, SOCK_DGRAM, 0);
            if (g_s < 0) {
                perror("tftp: socket");
                exit(EX_OSERR);
            }
            bzero((char *)&sa, sizeof (sa));
            sa.sa.sa_family = ai_fam_sock;
            if (pick_port_bind(g_s, &sa, portrange_from, portrange_to)) {
                perror("tftp: bind");
                exit(EX_OSERR);
            }
        }
    }
    port = sp->s_port;
    if (argc == 3) {
        struct servent *usp;
        usp = getservbyname(argv[2], "udp");
        if (usp) {
            port = usp->s_port;
        } else {
            unsigned long myport;
            char *ep;
            myport = strtoul(argv[2], &ep, 10);
            if (*ep || myport > 65535UL) {
                printf("%s: bad port number\n", argv[2]);
                connected = 0;
                return;
            }
            port = htons((u_short) myport);
        }
    }

    if (g_verbose) {
        char tmp[INET6_ADDRSTRLEN], *tp;
        tp = (char *)inet_ntop(g_peeraddr.sa.sa_family, SOCKADDR_P(&g_peeraddr),
                               tmp, INET6_ADDRSTRLEN);
        if (!tp)
            tp = (char *)"???";
        printf("Connected to %s (%s), port %u\n",
               hostname, tp, (unsigned int)ntohs(port));
    }
    connected = 1;
}

void modecmd(int argc, char *argv[])
{
    const struct modes *p;
    const char *sep;

    if (argc < 2) {
        printf("Using %s mode to transfer files.\n", mode->m_mode);
        return;
    }
    if (argc == 2) {
        for (p = modes; p->m_name; p++)
            if (strcmp(argv[1], p->m_name) == 0)
                break;
        if (p->m_name) {
            settftpmode(p);
            return;
        }
        printf("%s: unknown mode\n", argv[1]);
        /* drop through and print usage message */
    }

    printf("usage: %s [", argv[0]);
    sep = " ";
    for (p = modes; p->m_name; p++) {
        printf("%s%s", sep, p->m_name);
        if (*sep == ' ')
            sep = " | ";
    }
    printf(" ]\n");
    return;
}

void setbinary(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    settftpmode(MODE_OCTET);
}

void setascii(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    settftpmode(MODE_NETASCII);
}

static void settftpmode(const struct modes *newmode)
{
    mode = newmode;
    if (g_verbose)
        printf("mode set to %s\n", mode->m_mode);
}

/*
 * Send file(s).
 */
void put(int argc, char *argv[])
{
    int fd;
    int n, err;
    char *cp, *targ;
    long dirlen;

#ifdef WITH_READLINE
    long namelen;
    long lastlen=0;
#endif

    if (argc < 2) {
        getmoreargs("send ", "(file) ");
        makeargv();
        argc = margc;
        argv = margv;
    }
    if (argc < 2) {
        putusage(argv[0]);
        return;
    }
    targ = argv[argc - 1];
    if (!literal && strchr(argv[argc - 1], ':')) {
        for (n = 1; n < argc - 1; n++)
            if (strchr(argv[n], ':')) {
                putusage(argv[0]);
                return;
            }
        cp = argv[argc - 1];
        targ = strchr(cp, ':');
        *targ++ = 0;
        g_peeraddr.sa.sa_family = ai_fam;
        err = set_sock_addr(cp, &g_peeraddr,&hostname);
        if (err) {
            printf("Error: %s\n", gai_strerror(err));
            printf("%s: unknown host\n", argv[1]);
            connected = 0;
            return;
        }
        ai_fam = g_peeraddr.sa.sa_family;
        connected = 1;
    }
    if (!connected) {
        printf("No target machine specified.\n");
        return;
    }
    if (argc < 4) {
        cp = argc == 2 ? tail(targ) : argv[1];
        fd = open(cp, O_RDONLY | mode->m_openflags);
        if (fd < 0) {
            fprintf(stderr, "tftp: ");
            perror(cp);
            return;
        }
        if (g_verbose)
            printf("putting %s to %s:%s [%s]\n",
                   cp, hostname, targ, mode->m_mode);
        sa_set_port(&g_peeraddr, port);
        tftp_sendfile(fd, targ, mode->m_mode, windowsize);
        return;
    }
    /* this assumes the target is a directory */
    /* on a remote unix system.  hmmmm.  */
    dirlen = strlen(targ)+1;
#ifdef WITH_READLINE
    remote_pth = xmalloc(dirlen+1);
#endif
    strcpy(remote_pth, targ);
    remote_pth[dirlen-1] = '/';
    cp = remote_pth + dirlen;
    for (n = 1; n < argc - 1; n++) {
#ifdef WITH_READLINE
        namelen = strlen(tail(argv[n])) + 1;
        if (namelen > lastlen) {
            remote_pth = xrealloc(remote_pth, dirlen + namelen + 1);
            cp = remote_pth + dirlen;
            lastlen = namelen;
        }
#endif
        strcpy(cp, tail(argv[n]));
        fd = open(argv[n], O_RDONLY | mode->m_openflags);
        if (fd < 0) {
            fprintf(stderr, "tftp: ");
            perror(argv[n]);
            continue;
        }
        if (g_verbose)
            printf("putting %s to %s:%s [%s]\n",
                   argv[n], hostname, remote_pth, mode->m_mode);
        sa_set_port(&g_peeraddr, port);
        tftp_sendfile(fd, remote_pth, mode->m_mode, windowsize);
    }
}

static void putusage(char *s)
{
    printf("usage: %s file ... host:target, or\n", s);
    printf("       %s file ... target (when already connected)\n", s);
}

/*
 * Receive file(s).
 */
void get(int argc, char *argv[])
{
    int fd;
    int n;
    char *cp;
    char *src;

    if (argc < 2) {
        getmoreargs("get ", "(files) ");
        makeargv();
        argc = margc;
        argv = margv;
    }
    if (argc < 2) {
        getusage(argv[0]);
        return;
    }
    if (!connected) {
        for (n = 1; n < argc; n++)
            if (literal || strchr(argv[n], ':') == 0) {
                getusage(argv[0]);
                return;
            }
    }
    for (n = 1; n < argc; n++) {
        src = strchr(argv[n], ':');
        if (literal || src == NULL)
            src = argv[n];
        else {
            int err;

            *src++ = 0;
            g_peeraddr.sa.sa_family = ai_fam;
            err = set_sock_addr(argv[n], &g_peeraddr, &hostname);
            if (err) {
                printf("Warning: %s\n", gai_strerror(err));
                printf("%s: unknown host\n", argv[1]);
                continue;
            }
            ai_fam = g_peeraddr.sa.sa_family;
            connected = 1;
        }
        if (argc < 4) {
            cp = argc == 3 ? argv[2] : tail(src);
            fd = open(cp, O_WRONLY | O_CREAT | O_TRUNC | mode->m_openflags,
                      0666);
            if (fd < 0) {
                fprintf(stderr, "tftp: ");
                perror(cp);
                return;
            }
            if (g_verbose)
                printf("getting from %s:%s to %s [%s]\n",
                       hostname, src, cp, mode->m_mode);
            sa_set_port(&g_peeraddr, port);
            tftp_recvfile(fd, src, mode->m_mode, windowsize);
            break;
        }
        cp = tail(src);         /* new .. jdg */
        fd = open(cp, O_WRONLY | O_CREAT | O_TRUNC | mode->m_openflags,
                  0666);
        if (fd < 0) {
            fprintf(stderr, "tftp: ");
            perror(cp);
            continue;
        }
        if (g_verbose)
            printf("getting from %s:%s to %s [%s]\n",
                   hostname, src, cp, mode->m_mode);
        sa_set_port(&g_peeraddr, port);
        tftp_recvfile(fd, src, mode->m_mode, windowsize);
    }
}

static void getusage(char *s)
{
    printf("usage: %s host:file host:file ... file, or\n", s);
    printf("       %s file file ... file if connected\n", s);
}

static int rexmtval = RTIMEOUT;

void setrexmt(int argc, char *argv[])
{
    int t;

    if (argc < 2) {
        getmoreargs("rexmt-timeout ", "(value) ");
        makeargv();
        argc = margc;
        argv = margv;
    }
    if (argc != 2) {
        printf("usage: %s value\n", argv[0]);
        return;
    }
    t = atoi(argv[1]);
    if (t < 0)
        printf("%s: bad value\n", argv[1]);
    else
        rexmtval = t;
}

static int maxtimeout = RETRIES * RTIMEOUT;

void settimeout(int argc, char *argv[])
{
    int t;

    if (argc < 2) {
        getmoreargs("maximum-timeout ", "(value) ");
        makeargv();
        argc = margc;
        argv = margv;
    }
    if (argc != 2) {
        printf("usage: %s value\n", argv[0]);
        return;
    }
    t = atoi(argv[1]);
    if (t < 0)
        printf("%s: bad value\n", argv[1]);
    else
        maxtimeout = t;
}

void setliteral(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    literal = !literal;
    printf("Literal mode %s.\n", literal ? "on" : "off");
}

void status(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    if (connected)
        printf("Connected to %s.\n", hostname);
    else
        printf("Not connected.\n");
    printf("Mode: %s Verbose: %s Tracing: %s Literal: %s\n", mode->m_mode,
           g_verbose ? "on" : "off", g_trace_opt ? "on" : "off",
           literal ? "on" : "off");
    printf("Rexmt-interval: %d seconds, Max-timeout: %d seconds\n",
           rexmtval, maxtimeout);
    printf("Blocksize: %lu, windowsize: %d\n", g_blocksize, (windowsize > 0 ? windowsize : 1));
}

void intr(int sig)
{
    (void)sig;                  /* Quiet unused warning */

    bsd_signal(SIGALRM, SIG_IGN);
    alarm(0);
    siglongjmp(toplevel, -1);
}

char *tail(char *filename)
{
    char *s;

    while (*filename) {
        s = strrchr(filename, '/');
        if (s == NULL)
            break;
        if (s[1])
            return (s + 1);
        *s = '\0';
    }
    return (filename);
}

/*
 * Command parser.
 */
static void command(void)
{
    struct cmd *c;

    for (;;) {
#ifdef WITH_READLINE
        if (line) {
            free(line);
            line = NULL;
        }
        if (remote_pth) {
            free(remote_pth);
            remote_pth = NULL;
        }
        line = readline(prompt);
        if (!line)
            exit(0);            /* EOF */
#else
        fputs(prompt, stdout);
        if (fgets(line, LBUFLEN, stdin) == 0) {
            if (feof(stdin)) {
                exit(0);
            } else {
                continue;
            }
        }
#endif
        if ((line[0] == 0) || (line[0] == '\n'))
            continue;
#ifdef WITH_READLINE
#ifdef HAVE_READLINE_HISTORY_H
        add_history(line);
#endif
#endif
        makeargv();
        if (margc == 0)
            continue;

        c = getcmd(margv[0]);
        if (c == (struct cmd *)-1) {
            printf("?Ambiguous command\n");
            continue;
        }
        if (c == 0) {
            printf("?Invalid command\n");
            continue;
        }
        (*c->handler) (margc, margv);
    }
}

struct cmd *getcmd(char *name)
{
    const char *p;
    char *q;
    struct cmd *c, *found;
    int nmatches, longest;

    longest = 0;
    nmatches = 0;
    found = 0;
    for (c = cmdtab; (p = c->name) != NULL; c++) {
        for (q = name; *q == *p++; q++)
            if (*q == 0)        /* exact match? */
                return (c);
        if (!*q) {              /* the name was a prefix */
            if (q - name > longest) {
                longest = q - name;
                nmatches = 1;
                found = c;
            } else if (q - name == longest)
                nmatches++;
        }
    }
    if (nmatches > 1)
        return ((struct cmd *)-1);
    return (found);
}

/*
 * Slice a string up into argc/argv.
 */
static void makeargv(void)
{
    char *cp;
    char **argp;

    if (!sizeof_margv) {
        sizeof_margv = 20;
        margv = xmalloc(sizeof_margv * sizeof(char *));
    }
    argp = margv;

    margc = 0;
    for (cp = line; *cp;) {
        while (isspace(*cp))
            cp++;
        if (*cp == '\0')
            break;
        *argp++ = cp;
        margc += 1;
        if (margc == sizeof_margv) {
            sizeof_margv += 20;
            margv = xrealloc(margv, sizeof_margv * sizeof(char *));
            argp = margv + margc;
        }
        while (*cp != '\0' && !isspace(*cp))
            cp++;
        if (*cp == '\0')
            break;
        *cp++ = '\0';
    }
    *argp++ = 0;
}

void quit(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    exit(0);
}

/*
 * Help command.
 */
void help(int argc, char *argv[])
{
    struct cmd *c;

    printf("%s\n", VERSION);

    if (argc == 1) {
        printf("Commands may be abbreviated.  Commands are:\n\n");
        for (c = cmdtab; c->name; c++)
            printf("%-*s\t%s\n", (int)HELPINDENT, c->name, c->help);
        return;
    }
    while (--argc > 0) {
        char *arg;
        arg = *++argv;
        c = getcmd(arg);
        if (c == (struct cmd *)-1)
            printf("?Ambiguous help command %s\n", arg);
        else if (c == (struct cmd *)0)
            printf("?Invalid help command %s\n", arg);
        else
            printf("%s\n", c->help);
    }
}

void settrace(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */

    g_trace_opt = !g_trace_opt;
    printf("Packet tracing %s.\n", g_trace_opt ? "on" : "off");
}

void setverbose(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */

    g_verbose = !g_verbose;
    printf("Verbose mode %s.\n", g_verbose ? "on" : "off");
}
