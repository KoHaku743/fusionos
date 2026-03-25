/*
 * fusion-net — FusionOS networking shell command
 *
 * Implements the NET command dispatched by COMMAND.COM++:
 *   NET STATUS              — list network interfaces and addresses
 *   NET PING   <host>       — test reachability (ICMP or TCP probe)
 *   NET CONNECT <host> <port> — test TCP connectivity
 *
 * Wraps Linux kernel networking through the HAL (POSIX socket API).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>

/* ── ICMP helpers ────────────────────────────────────────────────────── */

static uint16_t icmp_checksum(void *data, size_t len) {
    uint32_t sum = 0;
    uint16_t *p  = (uint16_t *)data;
    while (len > 1)  { sum += *p++; len -= 2; }
    if (len > 0)     sum += *(uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── NET STATUS ──────────────────────────────────────────────────────── */

static void cmd_status(void) {
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0) {
        perror("getifaddrs");
        return;
    }

    printf("Network interfaces:\n\n");
    char prev_name[IF_NAMESIZE] = "";

    for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;

        /* Print interface name header once per interface */
        if (strcmp(ifa->ifa_name, prev_name) != 0) {
            if (prev_name[0]) printf("\n");
            printf("  %s  [%s]\n", ifa->ifa_name,
                   (ifa->ifa_flags & IFF_UP) ? "UP" : "DOWN");
            snprintf(prev_name, sizeof(prev_name), "%s", ifa->ifa_name);
        }

        char addr_str[INET6_ADDRSTRLEN];
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str));
            printf("    IPv4 : %s\n", addr_str);

        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str));
            printf("    IPv6 : %s\n", addr_str);
        }
    }
    printf("\n");
    freeifaddrs(ifap);
}

/* ── NET PING ────────────────────────────────────────────────────────── */

/*
 * ICMP echo ping.  Falls back to a TCP probe on port 80 if raw sockets
 * require elevated privileges that are unavailable.
 */
static void cmd_ping(const char *host) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;   /* IPv4 for ICMP */
    hints.ai_socktype = SOCK_RAW;
    hints.ai_protocol = IPPROTO_ICMP;

    printf("Pinging %s ...\n", host);

    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Cannot resolve %s: %s\n", host, gai_strerror(rc));
        return;
    }

    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET,
              &((struct sockaddr_in *)res->ai_addr)->sin_addr,
              addr_str, sizeof(addr_str));
    printf("Resolved %s to %s\n\n", host, addr_str);

    /* Try raw ICMP */
    int raw_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    int use_icmp = (raw_fd >= 0);
    if (!use_icmp) {
        if (errno == EPERM || errno == EACCES) {
            printf("(No CAP_NET_RAW — using TCP probe on port 80.\n"
                   " Hint: sudo setcap cap_net_raw+ep fusion-net for ICMP)\n\n");
        } else {
            printf("(Raw socket unavailable — using TCP probe on port 80)\n\n");
        }
    }

    int sent = 0, nrecv = 0;
    for (int seq = 1; seq <= 4; seq++) {
        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        long rtt_ms = -1;

        if (use_icmp) {
            /* Build ICMP echo request */
            struct {
                struct icmphdr hdr;
                uint8_t        payload[32];
            } pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.hdr.type             = ICMP_ECHO;
            pkt.hdr.code             = 0;
            pkt.hdr.un.echo.id       = (uint16_t)getpid();
            pkt.hdr.un.echo.sequence = (uint16_t)seq;
            memset(pkt.payload, 'F', sizeof(pkt.payload));
            pkt.hdr.checksum = icmp_checksum(&pkt, sizeof(pkt));

            sent++;
            if (sendto(raw_fd, &pkt, sizeof(pkt), 0,
                       res->ai_addr, res->ai_addrlen) < 0) {
                perror("sendto");
                break;
            }

            /* Wait for reply */
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(raw_fd, &rfds);
            struct timeval tv = {2, 0};
            int sr = select(raw_fd + 1, &rfds, NULL, NULL, &tv);
            if (sr > 0) {
                uint8_t rbuf[256];
                ssize_t n = recv(raw_fd, rbuf, sizeof(rbuf), 0);
                if (n >= (ssize_t)(sizeof(struct iphdr) +
                                   sizeof(struct icmphdr))) {
                    struct icmphdr *rh =
                        (struct icmphdr *)(rbuf + sizeof(struct iphdr));
                    if (rh->type == ICMP_ECHOREPLY &&
                        rh->un.echo.id == (uint16_t)getpid() &&
                        rh->un.echo.sequence == (uint16_t)seq) {
                        clock_gettime(CLOCK_MONOTONIC, &ts_end);
                        rtt_ms = (ts_end.tv_sec  - ts_start.tv_sec)  * 1000 +
                                 (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
                        nrecv++;
                    }
                }
            }

        } else {
            /* TCP probe on port 80 */
            struct addrinfo *tcp_res = NULL;
            struct addrinfo tcp_hints;
            memset(&tcp_hints, 0, sizeof(tcp_hints));
            tcp_hints.ai_family   = AF_INET;
            tcp_hints.ai_socktype = SOCK_STREAM;
            int gr = getaddrinfo(host, "80", &tcp_hints, &tcp_res);
            if (gr != 0) { fprintf(stderr, "%s\n", gai_strerror(gr)); break; }

            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (fd < 0) { freeaddrinfo(tcp_res); break; }

            sent++;
            int cr = connect(fd, tcp_res->ai_addr, tcp_res->ai_addrlen);
            if (cr < 0 && errno == EINPROGRESS) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                struct timeval tv = {2, 0};
                cr = select(fd + 1, NULL, &wfds, NULL, &tv);
                if (cr > 0) {
                    int err = 0;
                    socklen_t elen = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
                    cr = (err == 0) ? 0 : -1;
                } else {
                    cr = -1;
                }
            } else if (cr == 0) {
                /* immediate connect (loopback) */
            } else {
                cr = -1;
            }
            close(fd);
            freeaddrinfo(tcp_res);

            if (cr == 0) {
                clock_gettime(CLOCK_MONOTONIC, &ts_end);
                rtt_ms = (ts_end.tv_sec  - ts_start.tv_sec)  * 1000 +
                         (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
                nrecv++;
            }
        }

        if (rtt_ms >= 0) {
            printf("Reply from %s: seq=%d time=%ldms\n",
                   addr_str, seq, rtt_ms);
        } else {
            printf("Request timeout (seq=%d)\n", seq);
        }

        if (seq < 4) sleep(1);
    }

    if (use_icmp) close(raw_fd);
    freeaddrinfo(res);

    printf("\n--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           sent, nrecv, sent > 0 ? (sent - nrecv) * 100 / sent : 0);
}

/* ── NET CONNECT ─────────────────────────────────────────────────────── */

static void cmd_connect(const char *host, const char *port_str) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Cannot resolve %s: %s\n", host, gai_strerror(rc));
        return;
    }

    char addr_str[INET6_ADDRSTRLEN];
    if (res->ai_family == AF_INET) {
        inet_ntop(AF_INET,
                  &((struct sockaddr_in *)res->ai_addr)->sin_addr,
                  addr_str, sizeof(addr_str));
    } else {
        inet_ntop(AF_INET6,
                  &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr,
                  addr_str, sizeof(addr_str));
    }

    printf("Connecting to %s (%s) port %s ...\n", host, addr_str, port_str);

    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { perror("socket"); freeaddrinfo(res); return; }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int cr = connect(fd, res->ai_addr, res->ai_addrlen);
    int connected = 0;
    if (cr < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = {5, 0};
        cr = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (cr > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            connected = (err == 0);
        }
    } else if (cr == 0) {
        connected = 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    close(fd);
    freeaddrinfo(res);

    if (connected) {
        long ms = (ts_end.tv_sec  - ts_start.tv_sec)  * 1000 +
                  (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
        printf("Connected successfully (RTT %ld ms)\n", ms);
    } else {
        printf("Connection refused or timed out.\n");
    }
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("FusionOS NET -- Network commands\n\n");
        printf("Usage:\n");
        printf("  NET STATUS              List network interfaces\n");
        printf("  NET PING   <host>       Test reachability\n");
        printf("  NET CONNECT <host> <port>  Test TCP connection\n");
        return 1;
    }

    if (strcasecmp(argv[1], "STATUS") == 0) {
        cmd_status();
    } else if (strcasecmp(argv[1], "PING") == 0) {
        if (argc < 3) {
            fprintf(stderr, "NET PING: missing host\n");
            return 1;
        }
        cmd_ping(argv[2]);
    } else if (strcasecmp(argv[1], "CONNECT") == 0) {
        if (argc < 4) {
            fprintf(stderr, "NET CONNECT: usage: NET CONNECT <host> <port>\n");
            return 1;
        }
        cmd_connect(argv[2], argv[3]);
    } else {
        fprintf(stderr, "NET: unknown subcommand '%s'\n", argv[1]);
        fprintf(stderr, "  Valid: STATUS, PING, CONNECT\n");
        return 1;
    }

    return 0;
}
