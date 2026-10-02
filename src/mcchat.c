/*
 * mcchat - a simple multicast IPv6 UDP chat client.
 *
 * Every instance joins the same IPv6 multicast group on a chosen network
 * interface and both sends and receives chat messages over UDP. There is
 * no central server: each participant multicasts its messages to the
 * group and every other member on that link receives them directly.
 */

/* Feature-test macros needed to expose POSIX/BSD declarations (getopt,
 * addrinfo, struct ipv6_mreq, ...) under -std=c11, which defines
 * __STRICT_ANSI__ and otherwise hides them.
 *
 * On glibc/Linux, _POSIX_C_SOURCE plus _DEFAULT_SOURCE is enough.
 * On macOS, <netinet6/in6.h> instead hides struct ipv6_mreq (and other
 * advanced IPv6 API bits) whenever _POSIX_C_SOURCE is defined at all, so
 * we use _DARWIN_C_SOURCE there instead; __APPLE_USE_RFC_3542 additionally
 * exposes related RFC 3542 socket option definitions. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#define __APPLE_USE_RFC_3542
#else
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_GROUP "ff12::1234:5678"
#define DEFAULT_PORT  "12345"
#define MAX_MSG       1024
#define MAX_NICK      64

static volatile sig_atomic_t g_running = 1;
static int g_sock = -1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
    /* Unblock a pending recvfrom() by closing the socket. */
    if (g_sock >= 0) {
        shutdown(g_sock, SHUT_RDWR);
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -i <interface> [-g <group>] [-p <port>] [-n <nickname>] [-f <path>]\n"
        "\n"
        "  -i  network interface to use for multicast (required), e.g. en0\n"
        "  -g  IPv6 multicast group address (default: %s)\n"
        "  -p  UDP port (default: %s)\n"
        "  -n  nickname to prefix messages with (default: your username)\n"
        "  -f  send the contents of <path> as chat messages, one per line,\n"
        "      and exit immediately instead of starting interactive chat\n"
        "\n"
        "Each option can also be set via an environment variable; an explicit\n"
        "command-line option always overrides the matching environment variable:\n"
        "\n"
        "  MCCHAT_INTERFACE  same as -i\n"
        "  MCCHAT_GROUP      same as -g\n"
        "  MCCHAT_PORT       same as -p\n"
        "  MCCHAT_NICKNAME   same as -n\n",
        prog, DEFAULT_GROUP, DEFAULT_PORT);
}

/* Build "<nickname>: <line>" into msg (truncating to fit MAX_MSG, same as
 * the UDP payload limit) and send it to dst. Returns what sendto() returns. */
static ssize_t send_chat_line(int sock, const struct sockaddr_in6 *dst,
                               const char *nickname, const char *line) {
    char msg[MAX_MSG];
    int mlen = snprintf(msg, sizeof(msg), "%s: %s", nickname, line);
    if (mlen < 0) {
        mlen = 0;
    }
    if ((size_t)mlen >= sizeof(msg)) {
        mlen = sizeof(msg) - 1;
    }
    return sendto(sock, msg, (size_t)mlen, 0,
                   (const struct sockaddr *)dst, sizeof(*dst));
}

/* Receiver thread: prints any datagram that arrives on the multicast group. */
static void *receiver_thread(void *arg) {
    int sock = *(int *)arg;
    char buf[MAX_MSG + 1];

    while (g_running) {
        struct sockaddr_in6 src;
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&src, &srclen);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* Socket was shut down for exit, or a real error occurred. */
            break;
        }
        buf[n] = '\0';
        /* Move to the start of the line so it doesn't clash with our prompt. */
        printf("\r%s\n> ", buf);
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char **argv) {
    /* Environment variables provide defaults; explicit command-line options
     * (parsed below) override them on a per-option basis. */
    const char *ifname = getenv("MCCHAT_INTERFACE");
    const char *group = getenv("MCCHAT_GROUP");
    if (!group || group[0] == '\0') {
        group = DEFAULT_GROUP;
    }
    const char *port = getenv("MCCHAT_PORT");
    if (!port || port[0] == '\0') {
        port = DEFAULT_PORT;
    }
    char nickname[MAX_NICK] = {0};
    const char *env_nickname = getenv("MCCHAT_NICKNAME");
    if (env_nickname && env_nickname[0] != '\0') {
        strncpy(nickname, env_nickname, sizeof(nickname) - 1);
    }
    const char *file_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "i:g:p:n:f:h")) != -1) {
        switch (opt) {
            case 'i': ifname = optarg; break;
            case 'g': group = optarg; break;
            case 'p': port = optarg; break;
            case 'n':
                memset(nickname, 0, sizeof(nickname));
                strncpy(nickname, optarg, sizeof(nickname) - 1);
                break;
            case 'f': file_path = optarg; break;
            case 'h':
            default:
                usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    if (!ifname || ifname[0] == '\0') {
        fprintf(stderr,
                "error: -i <interface> is required "
                "(or set MCCHAT_INTERFACE)\n\n");
        usage(argv[0]);
        return 1;
    }

    if (nickname[0] == '\0') {
        const char *user = getenv("USER");
        strncpy(nickname, user ? user : "anon", sizeof(nickname) - 1);
    }

    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "error: unknown interface '%s'\n", ifname);
        return 1;
    }

    /* Resolve the group address/port into a usable sockaddr_in6. */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;

    int rc = getaddrinfo(group, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "error: invalid group/port '%s'/'%s': %s\n",
                group, port, gai_strerror(rc));
        return 1;
    }

    struct sockaddr_in6 group_addr;
    memcpy(&group_addr, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    /* Link-local (ff02::/16 etc. scoped) addresses need the scope id set
     * so the kernel knows which interface to send/receive on. */
    if (IN6_IS_ADDR_MC_LINKLOCAL(&group_addr.sin6_addr)) {
        group_addr.sin6_scope_id = ifindex;
    }

    int sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

    /* Bind to the wildcard address on the chosen port so we receive
     * datagrams sent to the multicast group. */
    struct sockaddr_in6 bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_addr = in6addr_any;
    bind_addr.sin6_port = group_addr.sin6_port;

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    /* Join the multicast group on the requested interface. */
    struct ipv6_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.ipv6mr_multiaddr = group_addr.sin6_addr;
    mreq.ipv6mr_interface = ifindex;

    if (setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt(IPV6_JOIN_GROUP)");
        close(sock);
        return 1;
    }

    /* Make sure outgoing multicast packets leave via the same interface. */
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex,
                   sizeof(ifindex)) < 0) {
        perror("setsockopt(IPV6_MULTICAST_IF)");
        close(sock);
        return 1;
    }

    /* Don't loop our own sent packets back to us. */
    int loop = 0;
    setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof(loop));

    char group_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &group_addr.sin6_addr, group_str, sizeof(group_str));

    /* File mode: send each line of <path> as a chat message and exit,
     * without starting the receiver thread or interactive prompt. */
    if (file_path) {
        FILE *f = fopen(file_path, "r");
        if (!f) {
            fprintf(stderr, "error: cannot open '%s': %s\n",
                    file_path, strerror(errno));
            close(sock);
            return 1;
        }

        printf("Joined [%s%%%s]:%s as '%s'. Sending '%s'...\n",
               group_str, ifname, port, nickname, file_path);

        int exit_code = 0;
        char line[MAX_MSG];
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
                len--;
            }
            if (len == 0) {
                continue;
            }

            errno = 0;
            ssize_t sent = send_chat_line(sock, &group_addr, nickname, line);
            if (sent < 0) {
                fprintf(stderr, "error: sendto failed while sending '%s': %s\n",
                        file_path, strerror(errno));
                exit_code = 1;
                break;
            }
        }
        if (exit_code == 0 && ferror(f)) {
            fprintf(stderr, "error: reading '%s' failed: %s\n",
                    file_path, strerror(errno));
            exit_code = 1;
        }

        fclose(f);
        close(sock);
        if (exit_code == 0) {
            printf("Sent '%s'.\n", file_path);
        }
        return exit_code;
    }

    printf("Joined [%s%%%s]:%s as '%s'. Type a message and press enter.\n",
           group_str, ifname, port, nickname);

    g_sock = sock;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    pthread_t rx_tid;
    if (pthread_create(&rx_tid, NULL, receiver_thread, &sock) != 0) {
        perror("pthread_create");
        close(sock);
        return 1;
    }

    char line[MAX_MSG];
    printf("> ");
    fflush(stdout);
    while (g_running && fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        if (len == 0) {
            printf("> ");
            fflush(stdout);
            continue;
        }

        ssize_t sent = send_chat_line(sock, &group_addr, nickname, line);
        if (sent < 0) {
            perror("sendto");
        }
        printf("> ");
        fflush(stdout);
    }

    g_running = 0;
    shutdown(sock, SHUT_RDWR);
    pthread_join(rx_tid, NULL);
    close(sock);
    printf("\nDisconnected.\n");
    return 0;
}
