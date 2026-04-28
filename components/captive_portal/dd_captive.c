// Mini DNS server for first-run captive portal. Listens on UDP/53 and
// answers every A-record query with the SoftAP gateway IP — that gets the
// phone's connectivity probe (Apple's `captive.apple.com`, Google's
// `connectivitycheck.gstatic.com`, etc.) routed to our HTTP server, which
// in turn returns a 302 → /, and the OS pops up its captive-portal mini
// browser pointed at the setup page.
//
// Intentionally tiny — only handles A queries with a single question, no
// EDNS, no recursion. Anything fancy gets a SERVFAIL response and the OS
// falls back to its own DNS, which in SoftAP-only mode goes nowhere → still
// triggers captive-portal detection. So even malformed-by-our-standards
// queries serve our purpose.

#include "dd_captive.h"

#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "captive";

#define DNS_PORT 53
#define DNS_BUF_MAX 512   // RFC 1035 UDP DNS message limit

// Header flags / opcodes
#define DNS_HDR_QR       0x8000  // response
#define DNS_HDR_AA       0x0400  // authoritative
#define DNS_HDR_RCODE_OK 0x0000
#define DNS_HDR_RCODE_FAIL 0x0002

#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1

static TaskHandle_t s_task = NULL;
static uint32_t     s_answer_ip = 0;   // network-byte-order

// DNS message header (12 bytes, big-endian on wire).
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;

// Skip a QNAME starting at *pos (length-prefixed labels terminated by 0).
// Returns new offset, or -1 on malformed/overflow.
static int skip_qname(const uint8_t *buf, int len, int pos)
{
    while (pos < len) {
        uint8_t l = buf[pos++];
        if (l == 0) return pos;
        if ((l & 0xC0) != 0) return -1;  // pointer/EDNS — we don't bother
        pos += l;
    }
    return -1;
}

static int build_response(const uint8_t *q, int qlen, uint8_t *out, int outcap)
{
    if (qlen < (int)sizeof(dns_hdr_t)) return -1;

    const dns_hdr_t *qh = (const dns_hdr_t *)q;
    int qdcount = ntohs(qh->qdcount);
    if (qdcount != 1) return -1;

    int qpos = sizeof(dns_hdr_t);
    int qend = skip_qname(q, qlen, qpos);
    if (qend < 0 || qend + 4 > qlen) return -1;

    uint16_t qtype = (uint16_t)(q[qend] << 8 | q[qend + 1]);
    int question_total = qend + 4 - qpos;  // qname + qtype(2) + qclass(2)

    // Output: copy header + original question + (optional) one answer RR.
    // Smallest fixed-RR layout: 0xC0 0x0C (pointer to qname at offset 12)
    // + type(2) + class(2) + ttl(4) + rdlength(2) + rdata(4) = 16 bytes.
    int answer_rr_size = (qtype == DNS_TYPE_A) ? 16 : 0;
    int total = sizeof(dns_hdr_t) + question_total + answer_rr_size;
    if (total > outcap) return -1;

    dns_hdr_t *rh = (dns_hdr_t *)out;
    rh->id      = qh->id;
    rh->flags   = htons(DNS_HDR_QR | DNS_HDR_AA |
                        (qtype == DNS_TYPE_A ? DNS_HDR_RCODE_OK : DNS_HDR_RCODE_FAIL));
    rh->qdcount = htons(1);
    rh->ancount = htons(qtype == DNS_TYPE_A ? 1 : 0);
    rh->nscount = 0;
    rh->arcount = 0;

    memcpy(out + sizeof(dns_hdr_t), q + qpos, question_total);

    if (qtype == DNS_TYPE_A) {
        uint8_t *a = out + sizeof(dns_hdr_t) + question_total;
        a[0] = 0xC0; a[1] = 0x0C;          // ptr to qname
        a[2] = 0; a[3] = DNS_TYPE_A;
        a[4] = 0; a[5] = DNS_CLASS_IN;
        a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 60;  // TTL 60s
        a[10] = 0; a[11] = 4;               // rdlength
        memcpy(a + 12, &s_answer_ip, 4);
    }
    return total;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind: errno %d", errno);
        close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    {
        uint8_t *o = (uint8_t *)&s_answer_ip;
        ESP_LOGI(TAG, "DNS listening on UDP/53, redirecting all A -> %u.%u.%u.%u",
                 o[0], o[1], o[2], o[3]);
    }

    uint8_t buf[DNS_BUF_MAX];
    uint8_t resp[DNS_BUF_MAX];
    for (;;) {
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                        (struct sockaddr *)&src, &srclen);
        if (n <= 0) continue;

        int rn = build_response(buf, n, resp, sizeof(resp));
        if (rn <= 0) continue;
        sendto(sock, resp, rn, 0, (struct sockaddr *)&src, srclen);
    }
}

esp_err_t dd_captive_start(uint32_t gateway_ip)
{
    s_answer_ip = gateway_ip;
    if (s_task) return ESP_OK;  // already running; new IP applied above
    BaseType_t ok = xTaskCreate(dns_task, "captive_dns", 4096, NULL, 5, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
