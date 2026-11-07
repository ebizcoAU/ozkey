/* ozhostlock — the OZKIE v2 lock engine as a local TCP service, so apps and
 * servers can interoperate before AK1037 hardware exists (ozkey-49 P2).
 *
 *   ozhostlock [--state FILE] [--device-id ID] [--ble PORT] [--relay PORT]
 *
 * Wire, both ports: messages framed as  type(1) ‖ length(2, big-endian) ‖ payload.
 *   type 0 EDHOC, 1 OSCORE/CoAP, 2 error   — ozkey-48 §2.1, one whole message each
 *   type 0x80 keypad: pairing code (4-byte big-endian)        → no reply, or the held response
 *   type 0x81 keypad: PIN digits (ASCII)                      → 0x81 with 1 byte: 1 opened, 0 not
 *   type 0x82 read the GATT `info` characteristic              → 0x82 with the CBOR value
 * The 0x8x frames stand in for the lock's keypad and GATT read; they exist only here.
 *
 * --ble:   every TCP connection is one BLE connection (session context, gone on close).
 *          One at a time, as the real lock allows one central (CONTRACT.md).
 * --relay: all connections share the bridge relay link; contexts persist per bond in the
 *          state file, so restarting the process is a lock reboot (Echo first, B.1.2).
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "oz_lock.h"
#include "oz_port_host.h"

static oz_lock L;

static void hw_actuate(void *app, int unlock)
{
    (void)app;
    printf("[DOOR] %s\n", unlock ? "UNLOCKED" : "LOCKED");
    fflush(stdout);
}

static void hw_prompt(void *app, int on)
{
    (void)app;
    printf("[KEYPAD] %s\n", on ? "lit: enter the 6-digit pairing code" : "off");
    fflush(stdout);
}

static int listen_on(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* local only */
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) {
        perror("listen");
        exit(1);
    }
    return s;
}

typedef struct {
    int fd;
    int relay;
    uint8_t buf[2048];
    size_t len;
} client;

static int send_frame(int fd, uint8_t type, const uint8_t *p, size_t n)
{
    uint8_t h[3] = { type, (uint8_t)(n >> 8), (uint8_t)n };
    if (write(fd, h, 3) != 3 || (n && write(fd, p, n) != (ssize_t)n))
        return -1;
    return 0;
}

static oz_link ble_link, relay_link;
static uint8_t out[2048];

static void handle(client *c, uint8_t type, const uint8_t *p, size_t n)
{
    oz_link *k = c->relay ? &relay_link : &ble_link;
    uint8_t ot = 0;
    size_t ol = 0;
    if (type <= 2) {
        oz_lock_rx(&L, k, type, p, n, &ot, out, sizeof out, &ol);
    } else if (type == 0x80 && n == 4) {
        uint32_t code = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        oz_lock_pair_code(&L, k, code, &ot, out, sizeof out, &ol);
    } else if (type == 0x81 && n < 32) {
        char pin[32];
        memcpy(pin, p, n);
        pin[n] = 0;
        uint8_t r = (uint8_t)oz_lock_keypad_pin(&L, pin);
        send_frame(c->fd, 0x81, &r, 1);
        return;
    } else if (type == 0x82) {
        if (!oz_lock_info(&L, out, sizeof out, &ol))
            send_frame(c->fd, 0x82, out, ol);
        return;
    }
    if (ol)
        send_frame(c->fd, ot, out, ol);
}

int main(int argc, char **argv)
{
    const char *state = "ozhostlock.state", *dev = "ozk-host0001";
    int ble_port = 7001, relay_port = 7002;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--state")) state = argv[i + 1];
        else if (!strcmp(argv[i], "--device-id")) dev = argv[i + 1];
        else if (!strcmp(argv[i], "--ble")) ble_port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--relay")) relay_port = atoi(argv[i + 1]);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    oz_host_kv_file(state);
    oz_lock_hooks hooks = { hw_actuate, hw_prompt, NULL };
    if (oz_lock_init(&L, dev, "ozhostlock-p2", &hooks)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    /* Bind first: a port in use exits here, before the banner and the key, so a
     * harness never mistakes another process's lock for this one (RIG-BLE-FLAKE-1). */
    int lb = listen_on(ble_port), lr = listen_on(relay_port);
    oz_link_open(&ble_link, 0);
    oz_link_open(&relay_link, 1);
    printf("ozhostlock %s  %s  ble 127.0.0.1:%d  relay 127.0.0.1:%d  state %s\n",
           dev, oz_lock_claimed(&L) ? "claimed" : "UNCLAIMED", ble_port, relay_port, state);
    printf("lock key ");
    for (int i = 0; i < 32; i++) printf("%02x", L.pub[i]);
    printf("\n");

    client cl[8];
    int ncl = 0;
    for (;;) {
        struct pollfd pf[10];
        pf[0].fd = lb; pf[0].events = POLLIN;
        pf[1].fd = lr; pf[1].events = POLLIN;
        for (int i = 0; i < ncl; i++) { pf[2 + i].fd = cl[i].fd; pf[2 + i].events = POLLIN; }
        poll(pf, (nfds_t)(2 + ncl), 100);

        uint8_t ot;
        size_t ol;
        oz_lock_tick(&L, &ble_link, &ot, out, sizeof out, &ol);   /* pairing timeout */
        for (int i = 0; ol && i < ncl; i++)
            if (!cl[i].relay) send_frame(cl[i].fd, ot, out, ol);

        /* Reads and disconnects first, then new connections. */
        int nseen = ncl;
        for (int i = 0, j = 0; j < nseen; j++) {
            if (!(pf[2 + j].revents & (POLLIN | POLLHUP))) { i++; continue; }
            client *c = &cl[i];
            ssize_t r = read(c->fd, c->buf + c->len, sizeof c->buf - c->len);
            if (r <= 0) {
                printf("[%s] disconnected\n", c->relay ? "RELAY" : "BLE");
                close(c->fd);
                if (!c->relay)
                    oz_link_close(&ble_link);   /* session context dies with the link */
                memmove(&cl[i], &cl[i + 1], (size_t)(ncl - i - 1) * sizeof cl[0]);
                ncl--;
                continue;
            }
            c->len += (size_t)r;
            while (c->len >= 3) {
                size_t n = ((size_t)c->buf[1] << 8) | c->buf[2];
                if (n > sizeof c->buf - 3) { c->len = 0; break; }
                if (c->len < 3 + n)
                    break;
                handle(c, c->buf[0], c->buf + 3, n);
                memmove(c->buf, c->buf + 3 + n, c->len - 3 - n);
                c->len -= 3 + n;
            }
            i++;
        }
        for (int s = 0; s < 2; s++) {
            if (!(pf[s].revents & POLLIN))
                continue;
            int fd = accept(pf[s].fd, NULL, NULL);
            if (fd < 0)
                continue;
            int busy = 0;
            for (int i = 0; i < ncl; i++)
                if (!s && !cl[i].relay) busy = 1;
            if (ncl >= 8 || (s == 0 && busy)) {
                close(fd);   /* one BLE central at a time */
                continue;
            }
            cl[ncl].fd = fd;
            cl[ncl].relay = s;
            cl[ncl].len = 0;
            if (!s)
                oz_link_open(&ble_link, 0);
            printf("[%s] connected\n", s ? "RELAY" : "BLE");
            ncl++;
        }
    }
}
