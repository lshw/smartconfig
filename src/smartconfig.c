#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <getopt.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_REPEAT 80
#define DEFAULT_DELAY_US 8000
#define DEFAULT_TYPE "v1"
#define DEFAULT_BROADCAST "255.255.255.255"
#define DEFAULT_PORT 7001
#define DEFAULT_ACK_PORT 18266
#define DEFAULT_ACK_WAIT_MS 60000
#define DEFAULT_BSSID "00:00:00:00:00:00"
#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64
#define MAX_BODY_LEN 1200
#define ESPTOUCH_EXTRA_LEN 40
#define ESPTOUCH_EXTRA_HEAD_LEN 5
#define ESPTOUCH_MAX_DATA_CODES 128
#define ESPTOUCH_V2_MAX_LENGTHS 384
#define ACK_PACKET_LEN 11
#define MAX_TX_TARGETS 32

enum protocol_type {
    PROTOCOL_V1,
    PROTOCOL_V2,
};

struct options {
    char iface[IFNAMSIZ];
    const char *ssid;
    const char *password;
    char bssid_text[18];
    char ip_text[INET_ADDRSTRLEN];
    char broadcast_text[INET_ADDRSTRLEN];
    enum protocol_type type;
    uint16_t port;
    uint16_t ack_port;
    unsigned app_port_mark;
    unsigned repeat;
    unsigned delay_us;
    unsigned ack_wait_ms;
    bool have_iface;
    bool have_ip;
    bool have_broadcast;
    bool wait_ack;
    bool dry_run;
    bool verbose;
};

struct esptouch_stream {
    uint16_t lengths[ESPTOUCH_V2_MAX_LENGTHS];
    size_t len;
};

struct tx_target {
    char iface[IFNAMSIZ];
    char ip_text[INET_ADDRSTRLEN];
    char broadcast_text[INET_ADDRSTRLEN];
    int fd;
    struct sockaddr_in dst;
    struct esptouch_stream stream;
};

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s -s SSID -p PASSWORD [options]\n"
            "\n"
            "Options:\n"
            "  -t, --type TYPE         ESP-Touch type: v1 or v2 (default: %s)\n"
            "  -i, --iface IFACE       Outgoing Wi-Fi interface (default: all broadcast-capable IPv4 interfaces)\n"
            "  -s, --ssid SSID         Wi-Fi SSID to provision\n"
            "  -p, --password PASS     Wi-Fi password to provision\n"
            "  -a, --bssid MAC         Target AP BSSID (default: %s)\n"
            "  -I, --ip ADDR           Sender/local IPv4 address (default: auto)\n"
            "  -b, --broadcast ADDR    UDP broadcast address (default: auto, fallback %s)\n"
            "  -P, --port PORT         UDP destination port (default: %u)\n"
            "  -A, --ack-port PORT     UDP ACK listen port (default: %u)\n"
            "  -W, --ack-wait-ms MS    Total ACK wait time (default: %u)\n"
            "  -N, --no-ack            Do not listen for ESP-Touch success ACK\n"
            "  -M, --app-port-mark N   ESP-Touch v2 app port mark 0..3 (default: 0)\n"
            "  -r, --repeat COUNT      Full transmit cycles (default: %u)\n"
            "  -d, --delay-us USEC     Delay between UDP packets (default: %u)\n"
            "  -n, --dry-run           Print the ESP-Touch lengths without sending\n"
            "  -v, --verbose           Print transmit details\n"
            "  -h, --help              Show this help\n",
            prog, DEFAULT_TYPE, DEFAULT_BSSID, DEFAULT_BROADCAST, DEFAULT_PORT,
            DEFAULT_ACK_PORT, DEFAULT_ACK_WAIT_MS, DEFAULT_REPEAT, DEFAULT_DELAY_US);
}

static bool parse_uint(const char *s, unsigned *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value > UINT_MAX) {
        return false;
    }
    *out = (unsigned)value;
    return true;
}

static bool parse_u16(const char *s, uint16_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value == 0 || value > UINT16_MAX) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool parse_protocol_type(const char *s, enum protocol_type *out)
{
    if (strcmp(s, "v1") == 0 || strcmp(s, "esp-touch") == 0) {
        *out = PROTOCOL_V1;
        return true;
    }
    if (strcmp(s, "v2") == 0 || strcmp(s, "esp-touch-v2") == 0) {
        *out = PROTOCOL_V2;
        return true;
    }
    return false;
}

static bool parse_mac(const char *text, uint8_t mac[6])
{
    unsigned values[6];
    int n = sscanf(text, "%x:%x:%x:%x:%x:%x",
                   &values[0], &values[1], &values[2],
                   &values[3], &values[4], &values[5]);
    if (n != 6) {
        return false;
    }

    for (size_t i = 0; i < 6; i++) {
        if (values[i] > 0xff) {
            return false;
        }
        mac[i] = (uint8_t)values[i];
    }
    return true;
}

static int parse_args(int argc, char **argv, struct options *opt)
{
    static const struct option long_opts[] = {
        {"iface", required_argument, NULL, 'i'},
        {"type", required_argument, NULL, 't'},
        {"ssid", required_argument, NULL, 's'},
        {"password", required_argument, NULL, 'p'},
        {"bssid", required_argument, NULL, 'a'},
        {"ip", required_argument, NULL, 'I'},
        {"broadcast", required_argument, NULL, 'b'},
        {"port", required_argument, NULL, 'P'},
        {"ack-port", required_argument, NULL, 'A'},
        {"ack-wait-ms", required_argument, NULL, 'W'},
        {"no-ack", no_argument, NULL, 'N'},
        {"app-port-mark", required_argument, NULL, 'M'},
        {"repeat", required_argument, NULL, 'r'},
        {"delay-us", required_argument, NULL, 'd'},
        {"dry-run", no_argument, NULL, 'n'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    *opt = (struct options) {
        .type = PROTOCOL_V1,
        .port = DEFAULT_PORT,
        .ack_port = DEFAULT_ACK_PORT,
        .repeat = DEFAULT_REPEAT,
        .delay_us = DEFAULT_DELAY_US,
        .ack_wait_ms = DEFAULT_ACK_WAIT_MS,
        .wait_ack = true,
    };
    snprintf(opt->bssid_text, sizeof(opt->bssid_text), "%s", DEFAULT_BSSID);

    for (;;) {
        int c = getopt_long(argc, argv, "t:i:s:p:a:I:b:P:A:W:NM:r:d:nvh", long_opts, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 't':
            if (!parse_protocol_type(optarg, &opt->type)) {
                fprintf(stderr, "invalid type: %s\n", optarg);
                return -1;
            }
            break;
        case 'i':
            if (strlen(optarg) >= sizeof(opt->iface)) {
                fprintf(stderr, "interface name is too long\n");
                return -1;
            }
            snprintf(opt->iface, sizeof(opt->iface), "%s", optarg);
            opt->have_iface = true;
            break;
        case 's':
            opt->ssid = optarg;
            break;
        case 'p':
            opt->password = optarg;
            break;
        case 'a':
            if (!parse_mac(optarg, (uint8_t[6]){0})) {
                fprintf(stderr, "invalid BSSID: %s\n", optarg);
                return -1;
            }
            snprintf(opt->bssid_text, sizeof(opt->bssid_text), "%s", optarg);
            break;
        case 'I':
            if (inet_pton(AF_INET, optarg, &(struct in_addr){0}) != 1) {
                fprintf(stderr, "invalid IPv4 address: %s\n", optarg);
                return -1;
            }
            snprintf(opt->ip_text, sizeof(opt->ip_text), "%s", optarg);
            opt->have_ip = true;
            break;
        case 'b':
            if (inet_pton(AF_INET, optarg, &(struct in_addr){0}) != 1) {
                fprintf(stderr, "invalid broadcast address: %s\n", optarg);
                return -1;
            }
            snprintf(opt->broadcast_text, sizeof(opt->broadcast_text), "%s", optarg);
            opt->have_broadcast = true;
            break;
        case 'P':
            if (!parse_u16(optarg, &opt->port)) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return -1;
            }
            break;
        case 'A':
            if (!parse_u16(optarg, &opt->ack_port)) {
                fprintf(stderr, "invalid ACK port: %s\n", optarg);
                return -1;
            }
            break;
        case 'W':
            if (!parse_uint(optarg, &opt->ack_wait_ms)) {
                fprintf(stderr, "invalid ACK wait time: %s\n", optarg);
                return -1;
            }
            break;
        case 'N':
            opt->wait_ack = false;
            break;
        case 'M':
            if (!parse_uint(optarg, &opt->app_port_mark) || opt->app_port_mark > 3) {
                fprintf(stderr, "invalid app port mark: %s\n", optarg);
                return -1;
            }
            break;
        case 'r':
            if (!parse_uint(optarg, &opt->repeat)) {
                fprintf(stderr, "invalid repeat count: %s\n", optarg);
                return -1;
            }
            break;
        case 'd':
            if (!parse_uint(optarg, &opt->delay_us)) {
                fprintf(stderr, "invalid delay: %s\n", optarg);
                return -1;
            }
            break;
        case 'n':
            opt->dry_run = true;
            break;
        case 'v':
            opt->verbose = true;
            break;
        case 'h':
            usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(stderr, argv[0]);
            return -1;
        }
    }

    if (!opt->ssid || !opt->password) {
        usage(stderr, argv[0]);
        return -1;
    }

    size_t ssid_len = strlen(opt->ssid);
    size_t password_len = strlen(opt->password);
    if (ssid_len == 0 || ssid_len > MAX_SSID_LEN) {
        fprintf(stderr, "SSID length must be 1..%u bytes\n", MAX_SSID_LEN);
        return -1;
    }
    if (password_len > MAX_PASSWORD_LEN) {
        fprintf(stderr, "password length must be 0..%u bytes\n", MAX_PASSWORD_LEN);
        return -1;
    }
    if (opt->repeat == 0) {
        fprintf(stderr, "repeat count must be greater than zero\n");
        return -1;
    }
    if (opt->wait_ack && opt->ack_wait_ms == 0) {
        fprintf(stderr, "ACK wait time must be greater than zero\n");
        return -1;
    }

    return 0;
}

static bool ipv4_is_private(struct in_addr addr)
{
    uint32_t ip = ntohl(addr.s_addr);
    return (ip >> 24) == 10 ||
           (ip >> 20) == 0xac1 ||
           (ip >> 16) == 0xc0a8 ||
           (ip >> 16) == 0xa9fe;
}

static int iface_priority(const char *name)
{
    if (strcmp(name, "br-lan") == 0) {
        return 100;
    }
    if (strncmp(name, "wlan", 4) == 0 ||
        strncmp(name, "phy", 3) == 0 ||
        strstr(name, "ap") != NULL) {
        return 90;
    }
    if (strncmp(name, "br-", 3) == 0) {
        return 80;
    }
    if (strncmp(name, "eth", 3) == 0 ||
        strncmp(name, "en", 2) == 0) {
        return 50;
    }
    return 10;
}

static bool fill_from_ifaddr(struct options *opt, const struct ifaddrs *ifa)
{
    const struct sockaddr_in *addr = (const struct sockaddr_in *)ifa->ifa_addr;
    if (!opt->have_ip &&
        inet_ntop(AF_INET, &addr->sin_addr, opt->ip_text, sizeof(opt->ip_text)) == NULL) {
        perror("inet_ntop(local ip)");
        return false;
    }
    opt->have_ip = true;

    if (!opt->have_iface) {
        snprintf(opt->iface, sizeof(opt->iface), "%s", ifa->ifa_name);
        opt->have_iface = true;
    }

    if (!opt->have_broadcast && ifa->ifa_broadaddr != NULL) {
        const struct sockaddr_in *bcast =
            (const struct sockaddr_in *)ifa->ifa_broadaddr;
        if (inet_ntop(AF_INET, &bcast->sin_addr, opt->broadcast_text,
                      sizeof(opt->broadcast_text)) == NULL) {
            perror("inet_ntop(broadcast)");
            return false;
        }
        opt->have_broadcast = true;
    }

    return true;
}

static int discover_from_interfaces(struct options *opt)
{
    struct in_addr wanted_ip = {0};
    if (opt->have_ip && inet_pton(AF_INET, opt->ip_text, &wanted_ip) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", opt->ip_text);
        return -1;
    }

    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    const struct ifaddrs *best = NULL;
    int best_score = INT_MIN;

    for (const struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const struct sockaddr_in *addr = (const struct sockaddr_in *)ifa->ifa_addr;
        if ((ntohl(addr->sin_addr.s_addr) >> 24) == 127) {
            continue;
        }
        if (opt->have_iface && strcmp(ifa->ifa_name, opt->iface) != 0) {
            continue;
        }
        if (opt->have_ip && addr->sin_addr.s_addr != wanted_ip.s_addr) {
            continue;
        }

        int score = iface_priority(ifa->ifa_name);
        if (ipv4_is_private(addr->sin_addr)) {
            score += 1000;
        }
        if (ifa->ifa_broadaddr != NULL) {
            score += 100;
        }
        if (score > best_score) {
            best = ifa;
            best_score = score;
        }
    }

    if (best != NULL && !fill_from_ifaddr(opt, best)) {
        freeifaddrs(ifaddr);
        return -1;
    }
    freeifaddrs(ifaddr);

    return best != NULL ? 0 : -1;
}

static int discover_from_default_route(struct options *opt)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        perror("socket(AF_INET)");
        return -1;
    }

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
    };
    if (inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)&dst, sizeof(dst)) == -1) {
        perror("connect(default route probe)");
        close(fd);
        return -1;
    }

    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &local_len) == -1) {
        perror("getsockname(default route probe)");
        close(fd);
        return -1;
    }
    close(fd);

    struct in_addr match_addr = local.sin_addr;
    if (opt->have_ip) {
        if (inet_pton(AF_INET, opt->ip_text, &match_addr) != 1) {
            fprintf(stderr, "invalid IPv4 address: %s\n", opt->ip_text);
            return -1;
        }
    } else {
        if (inet_ntop(AF_INET, &local.sin_addr, opt->ip_text, sizeof(opt->ip_text)) == NULL) {
            perror("inet_ntop(local ip)");
            return -1;
        }
        opt->have_ip = true;
    }

    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    bool matched = false;
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        const struct sockaddr_in *addr = (const struct sockaddr_in *)ifa->ifa_addr;
        if (addr->sin_addr.s_addr != match_addr.s_addr) {
            continue;
        }

        if (!opt->have_iface) {
            snprintf(opt->iface, sizeof(opt->iface), "%s", ifa->ifa_name);
            opt->have_iface = true;
        }
        if (!opt->have_broadcast && ifa->ifa_broadaddr != NULL) {
            const struct sockaddr_in *bcast =
                (const struct sockaddr_in *)ifa->ifa_broadaddr;
            if (inet_ntop(AF_INET, &bcast->sin_addr, opt->broadcast_text,
                          sizeof(opt->broadcast_text)) == NULL) {
                perror("inet_ntop(broadcast)");
                freeifaddrs(ifaddr);
                return -1;
            }
            opt->have_broadcast = true;
        }
        matched = true;
        break;
    }

    freeifaddrs(ifaddr);

    if (!matched && !opt->have_iface) {
        fprintf(stderr, "failed to find interface for local IP %s\n", opt->ip_text);
        return -1;
    }
    if (!opt->have_broadcast) {
        snprintf(opt->broadcast_text, sizeof(opt->broadcast_text), "%s",
                 DEFAULT_BROADCAST);
        opt->have_broadcast = true;
    }

    return 0;
}

static int discover_local_route(struct options *opt)
{
    if (discover_from_interfaces(opt) == 0) {
        if (!opt->have_broadcast) {
            snprintf(opt->broadcast_text, sizeof(opt->broadcast_text), "%s",
                     DEFAULT_BROADCAST);
            opt->have_broadcast = true;
        }
        return 0;
    }

    return discover_from_default_route(opt);
}

static bool should_use_all_interfaces(const struct options *opt)
{
    return !opt->have_iface && !opt->have_ip && !opt->have_broadcast;
}

static bool ifaddr_to_text(const struct sockaddr *sa, char *out, size_t out_len,
                           const char *what)
{
    const struct sockaddr_in *addr = (const struct sockaddr_in *)sa;
    if (inet_ntop(AF_INET, &addr->sin_addr, out, out_len) == NULL) {
        perror(what);
        return false;
    }
    return true;
}

static int collect_tx_targets(struct tx_target targets[MAX_TX_TARGETS])
{
    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    size_t count = 0;
    for (const struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET ||
            ifa->ifa_broadaddr == NULL) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_UP) == 0 ||
            (ifa->ifa_flags & IFF_LOOPBACK) != 0 ||
            (ifa->ifa_flags & IFF_BROADCAST) == 0) {
            continue;
        }

        const struct sockaddr_in *addr = (const struct sockaddr_in *)ifa->ifa_addr;
        const struct sockaddr_in *bcast =
            (const struct sockaddr_in *)ifa->ifa_broadaddr;
        if ((ntohl(addr->sin_addr.s_addr) >> 24) == 127) {
            continue;
        }
        if (addr->sin_addr.s_addr == bcast->sin_addr.s_addr ||
            bcast->sin_addr.s_addr == htonl(INADDR_ANY)) {
            continue;
        }

        bool duplicate = false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(targets[i].iface, ifa->ifa_name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        if (count >= MAX_TX_TARGETS) {
            fprintf(stderr, "too many broadcast-capable IPv4 interfaces, max=%u\n",
                    MAX_TX_TARGETS);
            freeifaddrs(ifaddr);
            return -1;
        }

        struct tx_target *target = &targets[count];
        memset(target, 0, sizeof(*target));
        target->fd = -1;
        snprintf(target->iface, sizeof(target->iface), "%s", ifa->ifa_name);
        if (!ifaddr_to_text(ifa->ifa_addr, target->ip_text,
                            sizeof(target->ip_text), "inet_ntop(local ip)") ||
            !ifaddr_to_text(ifa->ifa_broadaddr, target->broadcast_text,
                            sizeof(target->broadcast_text), "inet_ntop(broadcast)")) {
            freeifaddrs(ifaddr);
            return -1;
        }

        count++;
    }

    freeifaddrs(ifaddr);

    if (count == 0) {
        fprintf(stderr, "no broadcast-capable non-loopback IPv4 interfaces found\n");
        return -1;
    }

    return (int)count;
}

static uint8_t crc8_esptouch(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t value = data[i] ^ crc;
        for (unsigned bit = 0; bit < 8; bit++) {
            if ((value & 0x01) != 0) {
                value = (uint8_t)((value >> 1) ^ 0x8c);
            } else {
                value >>= 1;
            }
        }
        crc = value;
    }

    return crc;
}

static void split_nibbles(uint8_t value, uint8_t *high, uint8_t *low)
{
    *high = (uint8_t)(value >> 4);
    *low = (uint8_t)(value & 0x0f);
}

static size_t make_data_code_lengths(uint8_t value, uint8_t index,
                                     uint16_t out[3])
{
    uint8_t data_high;
    uint8_t data_low;
    uint8_t crc_high;
    uint8_t crc_low;
    uint8_t crc_input[2] = {value, index};
    uint8_t crc = crc8_esptouch(crc_input, sizeof(crc_input));

    split_nibbles(value, &data_high, &data_low);
    split_nibbles(crc, &crc_high, &crc_low);

    out[0] = (uint16_t)(((uint16_t)(crc_high << 4 | data_high)) + ESPTOUCH_EXTRA_LEN);
    out[1] = (uint16_t)(0x0100u | index);
    out[1] = (uint16_t)(out[1] + ESPTOUCH_EXTRA_LEN);
    out[2] = (uint16_t)(((uint16_t)(crc_low << 4 | data_low)) + ESPTOUCH_EXTRA_LEN);
    return 3;
}

struct datum_entry {
    uint8_t value;
    uint8_t index;
};

static bool insert_entry(struct datum_entry *entries, size_t *count, size_t pos,
                         uint8_t value, uint8_t index)
{
    if (*count >= ESPTOUCH_MAX_DATA_CODES) {
        return false;
    }
    if (pos > *count) {
        pos = *count;
    }

    memmove(&entries[pos + 1], &entries[pos],
            (*count - pos) * sizeof(entries[0]));
    entries[pos] = (struct datum_entry) {
        .value = value,
        .index = index,
    };
    (*count)++;
    return true;
}

static bool append_entry(struct datum_entry *entries, size_t *count,
                         uint8_t value, uint8_t index)
{
    return insert_entry(entries, count, *count, value, index);
}

static bool append_length(struct esptouch_stream *stream, uint16_t length)
{
    if (length > MAX_BODY_LEN || stream->len >= ESPTOUCH_V2_MAX_LENGTHS) {
        return false;
    }
    stream->lengths[stream->len++] = length;
    return true;
}

static bool build_esptouch_v1_stream(const struct options *opt,
                                     struct esptouch_stream *stream)
{
    uint8_t bssid[6];
    struct in_addr ip;
    const uint8_t *ssid = (const uint8_t *)opt->ssid;
    const uint8_t *password = (const uint8_t *)opt->password;
    size_t ssid_len = strlen(opt->ssid);
    size_t pass_len = strlen(opt->password);

    if (!parse_mac(opt->bssid_text, bssid)) {
        fprintf(stderr, "invalid BSSID: %s\n", opt->bssid_text);
        return false;
    }
    if (inet_pton(AF_INET, opt->ip_text, &ip) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", opt->ip_text);
        return false;
    }

    memset(stream, 0, sizeof(*stream));
    if (!append_length(stream, 515) ||
        !append_length(stream, 514) ||
        !append_length(stream, 513) ||
        !append_length(stream, 512)) {
        return false;
    }

    uint8_t total_xor = 0;
    uint8_t ssid_crc = crc8_esptouch(ssid, ssid_len);
    uint8_t bssid_crc = crc8_esptouch(bssid, sizeof(bssid));
    uint8_t total_len = (uint8_t)(ESPTOUCH_EXTRA_HEAD_LEN + 4 + pass_len + ssid_len);
    uint8_t ip_bytes[4];
    memcpy(ip_bytes, &ip.s_addr, sizeof(ip_bytes));

    struct datum_entry entries[ESPTOUCH_MAX_DATA_CODES];
    size_t entry_count = 0;

#define ADD_ENTRY(v, idx) do { \
        uint8_t _v = (uint8_t)(v); \
        if (!append_entry(entries, &entry_count, _v, (uint8_t)(idx))) { \
            return false; \
        } \
        total_xor ^= _v; \
    } while (0)

    ADD_ENTRY(total_len, 0);
    ADD_ENTRY(pass_len, 1);
    ADD_ENTRY(ssid_crc, 2);
    ADD_ENTRY(bssid_crc, 3);

    for (size_t i = 0; i < sizeof(ip_bytes); i++) {
        ADD_ENTRY(ip_bytes[i], i + ESPTOUCH_EXTRA_HEAD_LEN);
    }
    for (size_t i = 0; i < pass_len; i++) {
        ADD_ENTRY(password[i], i + ESPTOUCH_EXTRA_HEAD_LEN + sizeof(ip_bytes));
    }
    for (size_t i = 0; i < ssid_len; i++) {
        ADD_ENTRY(ssid[i], i + ESPTOUCH_EXTRA_HEAD_LEN + sizeof(ip_bytes) + pass_len);
    }

#undef ADD_ENTRY

    if (!insert_entry(entries, &entry_count, 4, total_xor, 4)) {
        return false;
    }

    size_t insert_pos = ESPTOUCH_EXTRA_HEAD_LEN;
    for (size_t i = 0; i < sizeof(bssid); i++) {
        if (!insert_entry(entries, &entry_count, insert_pos, bssid[i],
                          (uint8_t)(total_len + i))) {
            return false;
        }
        insert_pos += 4;
    }

    for (size_t i = 0; i < entry_count; i++) {
        uint16_t lengths[3];
        make_data_code_lengths(entries[i].value, entries[i].index, lengths);
        if (!append_length(stream, lengths[0]) ||
            !append_length(stream, lengths[1]) ||
            !append_length(stream, lengths[2])) {
            return false;
        }
    }

    return true;
}

static bool data_needs_encode(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if ((data[i] & 0x80) != 0) {
            return true;
        }
    }
    return false;
}

static void random_padding(uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)(rand() % 127);
    }
}

static bool v2_add_packet_6bytes(struct esptouch_stream *stream,
                                 const uint8_t buf[6], int sequence,
                                 uint8_t seq_crc, bool tail_is_crc)
{
    if (sequence == -1) {
        if (!append_length(stream, 1048) ||
            !append_length(stream, 0) ||
            !append_length(stream, 1048) ||
            !append_length(stream, 0)) {
            return false;
        }
    } else {
        uint16_t seq_len = (uint16_t)(128 + sequence);
        if (!append_length(stream, seq_len) ||
            !append_length(stream, seq_len) ||
            !append_length(stream, seq_len)) {
            return false;
        }
    }

    int bit_count = tail_is_crc ? 7 : 8;
    for (int i = 0; i < bit_count; i++) {
        int data = ((buf[5] >> i) & 1)
                 | (((buf[4] >> i) & 1) << 1)
                 | (((buf[3] >> i) & 1) << 2)
                 | (((buf[2] >> i) & 1) << 3)
                 | (((buf[1] >> i) & 1) << 4)
                 | (((buf[0] >> i) & 1) << 5);
        if (!append_length(stream, (uint16_t)((i << 7) | (1 << 6) | data))) {
            return false;
        }
    }

    if (tail_is_crc) {
        if (!append_length(stream, (uint16_t)((7 << 7) | (1 << 6) | seq_crc))) {
            return false;
        }
    }

    return true;
}

static bool v2_write_chunk(struct esptouch_stream *stream, const uint8_t *data,
                           size_t data_len, size_t *offset, size_t expect_len,
                           int sequence, bool crc_in_packet)
{
    uint8_t buf[6] = {0};
    size_t remain = data_len - *offset;
    size_t read_len = remain < expect_len ? remain : expect_len;
    memcpy(buf, data + *offset, read_len);
    *offset += read_len;

    uint8_t seq_crc = crc8_esptouch(buf, read_len);
    if (expect_len < sizeof(buf)) {
        buf[sizeof(buf) - 1] = seq_crc;
    }

    return v2_add_packet_6bytes(stream, buf, sequence, seq_crc, !crc_in_packet);
}

static bool build_esptouch_v2_stream(const struct options *opt,
                                     struct esptouch_stream *stream)
{
    uint8_t bssid[6];
    if (!parse_mac(opt->bssid_text, bssid)) {
        fprintf(stderr, "invalid BSSID: %s\n", opt->bssid_text);
        return false;
    }

    const uint8_t *ssid = (const uint8_t *)opt->ssid;
    const uint8_t *password_in = (const uint8_t *)opt->password;
    size_t ssid_len = strlen(opt->ssid);
    size_t password_len = strlen(opt->password);
    bool password_encode = data_needs_encode(password_in, password_len);
    bool ssid_encode = data_needs_encode(ssid, ssid_len);
    size_t password_factor = password_encode ? 5 : 6;
    size_t ssid_factor = ssid_encode ? 5 : 6;
    size_t password_pad_len = password_factor - password_len % password_factor;
    size_t ssid_pad_len = ssid_factor - ssid_len % ssid_factor;

    if (password_pad_len == password_factor) {
        password_pad_len = 0;
    }
    if (ssid_pad_len == ssid_factor) {
        ssid_pad_len = 0;
    }

    uint8_t password[MAX_PASSWORD_LEN + 6];
    uint8_t ssid_buf[MAX_SSID_LEN + 6];
    memcpy(password, password_in, password_len);
    random_padding(password + password_len, password_pad_len);
    memcpy(ssid_buf, ssid, ssid_len);
    random_padding(ssid_buf + ssid_len, ssid_pad_len);

    uint8_t head[6];
    head[0] = (uint8_t)(ssid_len | (ssid_encode ? 0x80 : 0));
    head[1] = (uint8_t)(password_len | (password_encode ? 0x80 : 0));
    head[2] = 0;
    head[3] = crc8_esptouch(bssid, sizeof(bssid));
    head[4] = (uint8_t)(1 | ((opt->app_port_mark & 0x03) << 3));
    head[5] = crc8_esptouch(head, 5);

    uint8_t bytes[sizeof(head) + sizeof(password) + sizeof(ssid_buf)];
    size_t bytes_len = 0;
    memcpy(bytes + bytes_len, head, sizeof(head));
    bytes_len += sizeof(head);
    memcpy(bytes + bytes_len, password, password_len + password_pad_len);
    bytes_len += password_len + password_pad_len;
    size_t ssid_begin = bytes_len;
    memcpy(bytes + bytes_len, ssid_buf, ssid_len + ssid_pad_len);
    bytes_len += ssid_len + ssid_pad_len;

    memset(stream, 0, sizeof(*stream));
    size_t offset = 0;
    int sequence = -1;
    int count = 0;

    if (!v2_write_chunk(stream, bytes, bytes_len, &offset, 6, sequence, true)) {
        return false;
    }
    sequence++;
    count++;

    while (offset < bytes_len) {
        bool in_ssid = offset >= ssid_begin;
        size_t factor = in_ssid ? ssid_factor : password_factor;
        bool crc_in_packet = in_ssid ? ssid_encode : password_encode;
        if (!v2_write_chunk(stream, bytes, bytes_len, &offset, factor,
                            sequence, crc_in_packet)) {
            return false;
        }
        sequence++;
        count++;
    }

    uint16_t sequence_size_len = (uint16_t)(1072 + count - 1);
    if (sequence_size_len > MAX_BODY_LEN) {
        return false;
    }
    stream->lengths[1] = sequence_size_len;
    stream->lengths[3] = sequence_size_len;

    return true;
}

static bool build_esptouch_stream(const struct options *opt,
                                  struct esptouch_stream *stream)
{
    if (opt->type == PROTOCOL_V2) {
        return build_esptouch_v2_stream(opt, stream);
    }
    return build_esptouch_v1_stream(opt, stream);
}

static void dump_stream(const struct esptouch_stream *stream)
{
    printf("length_count=%zu\n", stream->len);
    for (size_t i = 0; i < stream->len; i++) {
        printf("%u%s", stream->lengths[i],
               ((i + 1) % 12 == 0 || i + 1 == stream->len) ? "\n" : " ");
    }
}

static int sleep_us(unsigned delay_us)
{
    struct timespec req = {
        .tv_sec = delay_us / 1000000U,
        .tv_nsec = (long)(delay_us % 1000000U) * 1000L,
    };

    while (nanosleep(&req, &req) == -1) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int make_udp_socket(const struct options *opt, struct sockaddr_in *dst)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        perror("socket(AF_INET)");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) == -1) {
        perror("setsockopt(SO_BROADCAST)");
        close(fd);
        return -1;
    }

    if (opt->have_iface) {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, opt->iface,
                       strlen(opt->iface) + 1) == -1) {
            perror("setsockopt(SO_BINDTODEVICE)");
            close(fd);
            return -1;
        }
    }

    *dst = (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_port = htons(opt->port),
    };
    if (inet_pton(AF_INET, opt->broadcast_text, &dst->sin_addr) != 1) {
        fprintf(stderr, "invalid broadcast address: %s\n", opt->broadcast_text);
        close(fd);
        return -1;
    }

    return fd;
}

static int make_target_udp_socket(struct tx_target *target, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        perror("socket(AF_INET)");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) == -1) {
        perror("setsockopt(SO_BROADCAST)");
        close(fd);
        return -1;
    }

    target->dst = (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, target->broadcast_text, &target->dst.sin_addr) != 1) {
        fprintf(stderr, "invalid broadcast address for %s: %s\n",
                target->iface, target->broadcast_text);
        close(fd);
        return -1;
    }

    target->fd = fd;
    return 0;
}

static int make_ack_socket(const struct options *opt)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        perror("socket(AF_INET ACK)");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(opt->ack_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind(ACK)");
        close(fd);
        return -1;
    }

    return fd;
}

static bool parse_ack_packet(const struct options *opt, const uint8_t *buf,
                             ssize_t len, char mac[18], char ip[INET_ADDRSTRLEN])
{
    uint8_t expected_first = (uint8_t)(strlen(opt->ssid) + strlen(opt->password) + 9);
    if (len != ACK_PACKET_LEN || buf[0] != expected_first) {
        return false;
    }

    snprintf(mac, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

    struct in_addr addr;
    memcpy(&addr.s_addr, &buf[7], sizeof(addr.s_addr));
    if (inet_ntop(AF_INET, &addr, ip, INET_ADDRSTRLEN) == NULL) {
        snprintf(ip, INET_ADDRSTRLEN, "-");
    }

    return true;
}

static int poll_ack(int ack_fd, const struct options *opt, int timeout_ms)
{
    if (ack_fd == -1 || timeout_ms < 0) {
        return 0;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(ack_fd, &read_fds);

    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    int rc = select(ack_fd + 1, &read_fds, NULL, NULL, &timeout);
    if (rc == -1) {
        if (errno == EINTR) {
            return 0;
        }
        perror("select(ACK)");
        return -1;
    }
    if (rc == 0) {
        return 0;
    }

    uint8_t buf[64];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    ssize_t received = recvfrom(ack_fd, buf, sizeof(buf), 0,
                                (struct sockaddr *)&peer, &peer_len);
    if (received == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        perror("recvfrom(ACK)");
        return -1;
    }

    char mac[18];
    char ip[INET_ADDRSTRLEN];
    if (!parse_ack_packet(opt, buf, received, mac, ip)) {
        if (opt->verbose) {
            char peer_ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
                snprintf(peer_ip, sizeof(peer_ip), "?");
            }
            fprintf(stderr, "ignored UDP ACK candidate from %s len=%zd\n",
                    peer_ip, received);
        }
        return 0;
    }

    printf("ACK success: mac=%s ip=%s\n", mac, ip);
    return 1;
}

static int send_len_udp(int fd, const struct sockaddr_in *dst, size_t len,
                        unsigned delay_us)
{
    static uint8_t packet[MAX_BODY_LEN];

    if (len > sizeof(packet)) {
        fprintf(stderr, "internal error: invalid UDP payload length %zu\n", len);
        return -1;
    }

    memset(packet, 0xa5, len);
    ssize_t sent = sendto(fd, packet, len, 0, (const struct sockaddr *)dst,
                          sizeof(*dst));
    if (sent == -1) {
        perror("sendto");
        return -1;
    }
    if ((size_t)sent != len) {
        fprintf(stderr, "short send: %zd/%zu\n", sent, len);
        return -1;
    }

    return sleep_us(delay_us);
}

static int transmit_udp(int fd, int ack_fd, const struct sockaddr_in *dst,
                        const struct options *opt,
                        const struct esptouch_stream *stream)
{
    int64_t start_ms = monotonic_ms();
    if (start_ms == -1) {
        perror("clock_gettime");
        return -1;
    }

    for (unsigned cycle = 0; cycle < opt->repeat; cycle++) {
        for (size_t i = 0; i < stream->len; i++) {
            if (send_len_udp(fd, dst, stream->lengths[i], opt->delay_us) == -1) {
                return -1;
            }

            if (ack_fd != -1) {
                int64_t now_ms = monotonic_ms();
                if (now_ms == -1) {
                    perror("clock_gettime");
                    return -1;
                }
                int64_t elapsed_ms = now_ms - start_ms;
                if (elapsed_ms >= (int64_t)opt->ack_wait_ms) {
                    fprintf(stderr, "timed out waiting for ESP-Touch ACK\n");
                    return 1;
                }

                int wait_ms = (int)((int64_t)opt->ack_wait_ms - elapsed_ms);
                if (wait_ms > (int)(opt->delay_us / 1000U + 1U)) {
                    wait_ms = (int)(opt->delay_us / 1000U + 1U);
                }
                int ack = poll_ack(ack_fd, opt, wait_ms);
                if (ack != 0) {
                    return ack > 0 ? 0 : -1;
                }
            }
        }

        if (opt->verbose) {
            fprintf(stderr, "cycle %u/%u sent\n", cycle + 1, opt->repeat);
        }
    }

    if (ack_fd != -1) {
        for (;;) {
            int64_t now_ms = monotonic_ms();
            if (now_ms == -1) {
                perror("clock_gettime");
                return -1;
            }
            int64_t elapsed_ms = now_ms - start_ms;
            if (elapsed_ms >= (int64_t)opt->ack_wait_ms) {
                fprintf(stderr, "timed out waiting for ESP-Touch ACK\n");
                return 1;
            }
            int wait_ms = (int)((int64_t)opt->ack_wait_ms - elapsed_ms);
            if (wait_ms > 1000) {
                wait_ms = 1000;
            }
            int ack = poll_ack(ack_fd, opt, wait_ms);
            if (ack != 0) {
                return ack > 0 ? 0 : -1;
            }
        }
    }

    return 0;
}

static int transmit_targets(struct tx_target targets[MAX_TX_TARGETS],
                            size_t target_count, int ack_fd,
                            const struct options *opt)
{
    int64_t start_ms = monotonic_ms();
    if (start_ms == -1) {
        perror("clock_gettime");
        return -1;
    }

    for (unsigned cycle = 0; cycle < opt->repeat; cycle++) {
        for (size_t i = 0; i < targets[0].stream.len; i++) {
            for (size_t t = 0; t < target_count; t++) {
                if (send_len_udp(targets[t].fd, &targets[t].dst,
                                 targets[t].stream.lengths[i],
                                 opt->delay_us) == -1) {
                    fprintf(stderr, "send failed on %s\n", targets[t].iface);
                    return -1;
                }

                if (ack_fd != -1) {
                    int64_t now_ms = monotonic_ms();
                    if (now_ms == -1) {
                        perror("clock_gettime");
                        return -1;
                    }
                    int64_t elapsed_ms = now_ms - start_ms;
                    if (elapsed_ms >= (int64_t)opt->ack_wait_ms) {
                        fprintf(stderr, "timed out waiting for ESP-Touch ACK\n");
                        return 1;
                    }

                    int wait_ms = (int)((int64_t)opt->ack_wait_ms - elapsed_ms);
                    if (wait_ms > (int)(opt->delay_us / 1000U + 1U)) {
                        wait_ms = (int)(opt->delay_us / 1000U + 1U);
                    }
                    int ack = poll_ack(ack_fd, opt, wait_ms);
                    if (ack != 0) {
                        return ack > 0 ? 0 : -1;
                    }
                }
            }
        }

        if (opt->verbose) {
            fprintf(stderr, "cycle %u/%u sent on %zu interfaces\n",
                    cycle + 1, opt->repeat, target_count);
        }
    }

    if (ack_fd != -1) {
        for (;;) {
            int64_t now_ms = monotonic_ms();
            if (now_ms == -1) {
                perror("clock_gettime");
                return -1;
            }
            int64_t elapsed_ms = now_ms - start_ms;
            if (elapsed_ms >= (int64_t)opt->ack_wait_ms) {
                fprintf(stderr, "timed out waiting for ESP-Touch ACK\n");
                return 1;
            }
            int wait_ms = (int)((int64_t)opt->ack_wait_ms - elapsed_ms);
            if (wait_ms > 1000) {
                wait_ms = 1000;
            }
            int ack = poll_ack(ack_fd, opt, wait_ms);
            if (ack != 0) {
                return ack > 0 ? 0 : -1;
            }
        }
    }

    return 0;
}

static void close_targets(struct tx_target targets[MAX_TX_TARGETS], size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (targets[i].fd != -1) {
            close(targets[i].fd);
            targets[i].fd = -1;
        }
    }
}

static int run_all_interfaces(const struct options *opt)
{
    struct tx_target targets[MAX_TX_TARGETS];
    int target_count = collect_tx_targets(targets);
    if (target_count == -1) {
        return -1;
    }

    for (int i = 0; i < target_count; i++) {
        struct options target_opt = *opt;
        memcpy(target_opt.iface, targets[i].iface, sizeof(target_opt.iface));
        memcpy(target_opt.ip_text, targets[i].ip_text, sizeof(target_opt.ip_text));
        memcpy(target_opt.broadcast_text, targets[i].broadcast_text,
               sizeof(target_opt.broadcast_text));
        target_opt.have_iface = true;
        target_opt.have_ip = true;
        target_opt.have_broadcast = true;

        if (!build_esptouch_stream(&target_opt, &targets[i].stream)) {
            fprintf(stderr, "failed to build ESP-Touch stream for %s\n",
                    targets[i].iface);
            return -1;
        }
        if (i > 0 && targets[i].stream.len != targets[0].stream.len) {
            fprintf(stderr, "internal error: stream length mismatch for %s\n",
                    targets[i].iface);
            return -1;
        }
    }

    if (opt->dry_run) {
        for (int i = 0; i < target_count; i++) {
            printf("iface=%s ip=%s broadcast=%s\n",
                   targets[i].iface, targets[i].ip_text, targets[i].broadcast_text);
            dump_stream(&targets[i].stream);
        }
        return 0;
    }

    int ack_fd = -1;
    if (opt->wait_ack) {
        ack_fd = make_ack_socket(opt);
        if (ack_fd == -1) {
            return -1;
        }
    }

    for (int i = 0; i < target_count; i++) {
        if (make_target_udp_socket(&targets[i], opt->port) == -1) {
            close_targets(targets, (size_t)target_count);
            if (ack_fd != -1) {
                close(ack_fd);
            }
            return -1;
        }
    }

    if (opt->verbose) {
        fprintf(stderr,
                "sending ESP-Touch %s stream over all broadcast-capable IPv4 interfaces bssid=%s lengths=%zu ack=%s:%u\n",
                opt->type == PROTOCOL_V2 ? "v2" : "v1",
                opt->bssid_text, targets[0].stream.len,
                opt->wait_ack ? "yes" : "no", opt->ack_port);
        for (int i = 0; i < target_count; i++) {
            fprintf(stderr, "  iface=%s ip=%s dst=%s:%u\n",
                    targets[i].iface, targets[i].ip_text,
                    targets[i].broadcast_text, opt->port);
        }
    }

    int rc = transmit_targets(targets, (size_t)target_count, ack_fd, opt);
    close_targets(targets, (size_t)target_count);
    if (ack_fd != -1) {
        close(ack_fd);
    }
    return rc;
}

int main(int argc, char **argv)
{
    struct options opt;
    if (parse_args(argc, argv, &opt) == -1) {
        return EXIT_FAILURE;
    }

    srand((unsigned)time(NULL));

    if (should_use_all_interfaces(&opt)) {
        int rc = run_all_interfaces(&opt);
        return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if ((!opt.have_iface || !opt.have_ip || !opt.have_broadcast) &&
        discover_local_route(&opt) == -1) {
        return EXIT_FAILURE;
    }
    struct esptouch_stream stream;
    if (!build_esptouch_stream(&opt, &stream)) {
        fprintf(stderr, "failed to build ESP-Touch stream\n");
        return EXIT_FAILURE;
    }

    if (opt.dry_run) {
        dump_stream(&stream);
        return EXIT_SUCCESS;
    }

    struct sockaddr_in dst;
    int fd = make_udp_socket(&opt, &dst);
    if (fd == -1) {
        return EXIT_FAILURE;
    }

    int ack_fd = -1;
    if (opt.wait_ack) {
        ack_fd = make_ack_socket(&opt);
        if (ack_fd == -1) {
            close(fd);
            return EXIT_FAILURE;
        }
    }

    if (opt.verbose) {
        fprintf(stderr,
                "sending ESP-Touch %s stream over UDP iface=%s dst=%s:%u ip=%s bssid=%s lengths=%zu ack=%s:%u\n",
                opt.type == PROTOCOL_V2 ? "v2" : "v1",
                opt.have_iface ? opt.iface : "-",
                opt.broadcast_text, opt.port, opt.ip_text, opt.bssid_text,
                stream.len, opt.wait_ack ? "yes" : "no", opt.ack_port);
    }

    int rc = transmit_udp(fd, ack_fd, &dst, &opt, &stream);
    if (ack_fd != -1) {
        close(ack_fd);
    }
    close(fd);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
