#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_REPEAT 80
#define DEFAULT_DELAY_US 8000
#define DEFAULT_IP "192.168.1.2"
#define DEFAULT_TYPE "v1"
#define DEFAULT_SEND "raw"
#define DEFAULT_BROADCAST "255.255.255.255"
#define DEFAULT_PORT 7001
#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64
#define MAX_BODY_LEN 1200
#define RADIOTAP_LEN 8
#define IEEE80211_HDR_LEN 24
#define ESPTOUCH_EXTRA_LEN 40
#define ESPTOUCH_EXTRA_HEAD_LEN 5
#define ESPTOUCH_MAX_DATA_CODES 128
#define ESPTOUCH_V2_MAX_LENGTHS 384

enum protocol_type {
    PROTOCOL_V1,
    PROTOCOL_V2,
};

enum send_mode {
    SEND_RAW,
    SEND_UDP,
};

struct options {
    const char *iface;
    const char *ssid;
    const char *password;
    const char *src_mac_text;
    const char *bssid_text;
    const char *ip_text;
    const char *broadcast_text;
    enum protocol_type type;
    enum send_mode send;
    uint16_t port;
    unsigned app_port_mark;
    unsigned repeat;
    unsigned delay_us;
    bool dry_run;
    bool verbose;
};

struct esptouch_stream {
    uint16_t lengths[ESPTOUCH_V2_MAX_LENGTHS];
    size_t len;
};

struct tx_context {
    int fd;
    struct sockaddr_ll addr;
    uint8_t src_mac[6];
    uint8_t bssid[6];
    uint16_t seq;
};

static const uint8_t broadcast_mac[6] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s -i MON_IFACE -s SSID -p PASSWORD [options]\n"
            "\n"
            "Options:\n"
            "  -t, --type TYPE         ESP-Touch type: v1 or v2 (default: %s)\n"
            "  -S, --send MODE         Send mode: raw or udp (default: %s)\n"
            "  -i, --iface IFACE       Interface: monitor iface for raw, outgoing iface for udp\n"
            "  -s, --ssid SSID         Wi-Fi SSID to provision\n"
            "  -p, --password PASS     Wi-Fi password to provision\n"
            "  -m, --src-mac MAC       Transmitter MAC (default: interface MAC)\n"
            "  -a, --bssid MAC         Target AP BSSID, required for ESP-Touch\n"
            "  -I, --ip ADDR           Sender/local IPv4 address (default: %s)\n"
            "  -b, --broadcast ADDR    UDP broadcast address (default: %s)\n"
            "  -P, --port PORT         UDP destination port (default: %u)\n"
            "  -M, --app-port-mark N   ESP-Touch v2 app port mark 0..3 (default: 0)\n"
            "  -r, --repeat COUNT      Full transmit cycles (default: %u)\n"
            "  -d, --delay-us USEC     Delay between packets/frames (default: %u)\n"
            "  -n, --dry-run           Print the ESP-Touch lengths without sending\n"
            "  -v, --verbose           Print transmit details\n"
            "  -h, --help              Show this help\n",
            prog, DEFAULT_TYPE, DEFAULT_SEND, DEFAULT_IP, DEFAULT_BROADCAST,
            DEFAULT_PORT, DEFAULT_REPEAT, DEFAULT_DELAY_US);
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

static bool parse_send_mode(const char *s, enum send_mode *out)
{
    if (strcmp(s, "raw") == 0 || strcmp(s, "wifi") == 0) {
        *out = SEND_RAW;
        return true;
    }
    if (strcmp(s, "udp") == 0) {
        *out = SEND_UDP;
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

static void format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int parse_args(int argc, char **argv, struct options *opt)
{
    static const struct option long_opts[] = {
        {"iface", required_argument, NULL, 'i'},
        {"type", required_argument, NULL, 't'},
        {"send", required_argument, NULL, 'S'},
        {"ssid", required_argument, NULL, 's'},
        {"password", required_argument, NULL, 'p'},
        {"src-mac", required_argument, NULL, 'm'},
        {"bssid", required_argument, NULL, 'a'},
        {"ip", required_argument, NULL, 'I'},
        {"broadcast", required_argument, NULL, 'b'},
        {"port", required_argument, NULL, 'P'},
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
        .send = SEND_RAW,
        .broadcast_text = DEFAULT_BROADCAST,
        .port = DEFAULT_PORT,
        .repeat = DEFAULT_REPEAT,
        .delay_us = DEFAULT_DELAY_US,
    };

    for (;;) {
        int c = getopt_long(argc, argv, "t:S:i:s:p:m:a:I:b:P:M:r:d:nvh", long_opts, NULL);
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
        case 'S':
            if (!parse_send_mode(optarg, &opt->send)) {
                fprintf(stderr, "invalid send mode: %s\n", optarg);
                return -1;
            }
            break;
        case 'i':
            opt->iface = optarg;
            break;
        case 's':
            opt->ssid = optarg;
            break;
        case 'p':
            opt->password = optarg;
            break;
        case 'm':
            opt->src_mac_text = optarg;
            break;
        case 'a':
            opt->bssid_text = optarg;
            break;
        case 'I':
            opt->ip_text = optarg;
            break;
        case 'b':
            opt->broadcast_text = optarg;
            break;
        case 'P':
            if (!parse_u16(optarg, &opt->port)) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return -1;
            }
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

    if (!opt->iface || !opt->ssid || !opt->password || !opt->bssid_text) {
        usage(stderr, argv[0]);
        return -1;
    }
    if (!opt->ip_text) {
        opt->ip_text = DEFAULT_IP;
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
    if (strlen(opt->iface) >= IFNAMSIZ) {
        fprintf(stderr, "interface name is too long\n");
        return -1;
    }
    struct in_addr ip;
    if (inet_pton(AF_INET, opt->ip_text, &ip) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", opt->ip_text);
        return -1;
    }
    struct in_addr broadcast;
    if (inet_pton(AF_INET, opt->broadcast_text, &broadcast) != 1) {
        fprintf(stderr, "invalid broadcast address: %s\n", opt->broadcast_text);
        return -1;
    }

    return 0;
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

static int get_interface_mac(int fd, const char *iface, uint8_t mac[6])
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
        return -1;
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

static int make_raw_socket(const struct options *opt, struct tx_context *tx)
{
    memset(tx, 0, sizeof(*tx));
    memcpy(tx->bssid, broadcast_mac, sizeof(tx->bssid));

    tx->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (tx->fd == -1) {
        perror("socket(AF_PACKET)");
        return -1;
    }

    unsigned ifindex = if_nametoindex(opt->iface);
    if (ifindex == 0) {
        fprintf(stderr, "unknown interface: %s\n", opt->iface);
        close(tx->fd);
        return -1;
    }

    tx->addr = (struct sockaddr_ll) {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(ETH_P_ALL),
        .sll_ifindex = (int)ifindex,
        .sll_halen = 6,
    };
    memcpy(tx->addr.sll_addr, broadcast_mac, sizeof(broadcast_mac));

    if (opt->src_mac_text) {
        if (!parse_mac(opt->src_mac_text, tx->src_mac)) {
            fprintf(stderr, "invalid source MAC: %s\n", opt->src_mac_text);
            close(tx->fd);
            return -1;
        }
    } else if (get_interface_mac(tx->fd, opt->iface, tx->src_mac) == -1) {
        const uint8_t fallback[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        memcpy(tx->src_mac, fallback, sizeof(fallback));
        fprintf(stderr, "warning: failed to read interface MAC, using 02:00:00:00:00:01\n");
    }

    if (opt->bssid_text && !parse_mac(opt->bssid_text, tx->bssid)) {
        fprintf(stderr, "invalid BSSID: %s\n", opt->bssid_text);
        close(tx->fd);
        return -1;
    }

    return 0;
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

    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, opt->iface,
                   strlen(opt->iface) + 1) == -1) {
        perror("setsockopt(SO_BINDTODEVICE)");
        close(fd);
        return -1;
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

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static size_t build_80211_frame(struct tx_context *tx, uint8_t *frame,
                                size_t frame_cap, size_t body_len)
{
    if (body_len > MAX_BODY_LEN ||
        frame_cap < RADIOTAP_LEN + IEEE80211_HDR_LEN + body_len) {
        return 0;
    }

    memset(frame, 0, RADIOTAP_LEN + IEEE80211_HDR_LEN + body_len);

    /* Minimal radiotap header: version 0, length 8, no present fields. */
    frame[2] = RADIOTAP_LEN;

    uint8_t *hdr = frame + RADIOTAP_LEN;

    /* IEEE 802.11 data frame, ToDS=0, FromDS=0. */
    put_le16(hdr + 0, 0x0008);
    put_le16(hdr + 2, 0x0000);
    memcpy(hdr + 4, broadcast_mac, 6);
    memcpy(hdr + 10, tx->src_mac, 6);
    memcpy(hdr + 16, tx->bssid, 6);
    put_le16(hdr + 22, (uint16_t)((tx->seq++ & 0x0fff) << 4));

    uint8_t *body = hdr + IEEE80211_HDR_LEN;
    for (size_t i = 0; i < body_len; i++) {
        body[i] = (uint8_t)(i ^ body_len ^ 0xa5);
    }

    return RADIOTAP_LEN + IEEE80211_HDR_LEN + body_len;
}

static int send_len_frame(struct tx_context *tx, size_t body_len,
                          unsigned delay_us)
{
    uint8_t frame[RADIOTAP_LEN + IEEE80211_HDR_LEN + MAX_BODY_LEN];
    size_t frame_len = build_80211_frame(tx, frame, sizeof(frame), body_len);
    if (frame_len == 0) {
        fprintf(stderr, "internal error: invalid frame body length %zu\n", body_len);
        return -1;
    }

    ssize_t sent = sendto(tx->fd, frame, frame_len, 0,
                          (const struct sockaddr *)&tx->addr,
                          sizeof(tx->addr));
    if (sent == -1) {
        perror("sendto");
        return -1;
    }
    if ((size_t)sent != frame_len) {
        fprintf(stderr, "short send: %zd/%zu\n", sent, frame_len);
        return -1;
    }

    return sleep_us(delay_us);
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

static int transmit_raw(struct tx_context *tx, const struct options *opt,
                        const struct esptouch_stream *stream)
{
    for (unsigned cycle = 0; cycle < opt->repeat; cycle++) {
        for (size_t i = 0; i < stream->len; i++) {
            if (send_len_frame(tx, stream->lengths[i], opt->delay_us) == -1) {
                return -1;
            }
        }

        if (opt->verbose) {
            fprintf(stderr, "cycle %u/%u sent\n", cycle + 1, opt->repeat);
        }
    }

    return 0;
}

static int transmit_udp(int fd, const struct sockaddr_in *dst,
                        const struct options *opt,
                        const struct esptouch_stream *stream)
{
    for (unsigned cycle = 0; cycle < opt->repeat; cycle++) {
        for (size_t i = 0; i < stream->len; i++) {
            if (send_len_udp(fd, dst, stream->lengths[i], opt->delay_us) == -1) {
                return -1;
            }
        }

        if (opt->verbose) {
            fprintf(stderr, "cycle %u/%u sent\n", cycle + 1, opt->repeat);
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct options opt;
    if (parse_args(argc, argv, &opt) == -1) {
        return EXIT_FAILURE;
    }

    srand((unsigned)time(NULL));

    struct esptouch_stream stream;
    if (!build_esptouch_stream(&opt, &stream)) {
        fprintf(stderr, "failed to build ESP-Touch stream\n");
        return EXIT_FAILURE;
    }

    if (opt.dry_run) {
        dump_stream(&stream);
        return EXIT_SUCCESS;
    }

    int rc;

    if (opt.send == SEND_UDP) {
        struct sockaddr_in dst;
        int fd = make_udp_socket(&opt, &dst);
        if (fd == -1) {
            return EXIT_FAILURE;
        }

        if (opt.verbose) {
            fprintf(stderr,
                    "sending ESP-Touch %s stream over UDP iface=%s dst=%s:%u ip=%s lengths=%zu\n",
                    opt.type == PROTOCOL_V2 ? "v2" : "v1",
                    opt.iface, opt.broadcast_text, opt.port, opt.ip_text, stream.len);
        }

        rc = transmit_udp(fd, &dst, &opt, &stream);
        close(fd);
    } else {
        struct tx_context tx;
        if (make_raw_socket(&opt, &tx) == -1) {
            return EXIT_FAILURE;
        }

        if (opt.verbose) {
            char src[18];
            char bssid[18];
            format_mac(tx.src_mac, src);
            format_mac(tx.bssid, bssid);
            fprintf(stderr,
                    "injecting ESP-Touch %s stream on %s src=%s bssid=%s ip=%s lengths=%zu\n",
                    opt.type == PROTOCOL_V2 ? "v2" : "v1",
                    opt.iface, src, bssid, opt.ip_text, stream.len);
        }

        rc = transmit_raw(&tx, &opt, &stream);
        close(tx.fd);
    }

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
