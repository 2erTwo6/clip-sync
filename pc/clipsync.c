#define _GNU_SOURCE
#include "net.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define HEARTBEAT_INTERVAL_MS 5000u
#define HEARTBEAT_TIMEOUT_MS 15000u
#define TEXT_TS_OVERHEAD 8u

struct state {
    int fd;
    int poll_ms;
    int prefer_local_on_tie; /* PC = 1, Android = 0; used for equal timestamps */
    int local_seen;
    int pending_send;
    uint64_t local_ts;
    uint64_t last_remote_ts;
    uint64_t last_rx_ms;
    uint64_t last_ping_ms;
    uint8_t *last_local;
    size_t last_local_len;
    uint8_t *last_remote;
    size_t last_remote_len;
};

static int wayland_session(void) {
    const char *w = getenv("WAYLAND_DISPLAY");
    const char *s = getenv("XDG_SESSION_TYPE");
    if (w && *w) return 1;
    if (s && strcmp(s, "wayland") == 0) return 1;
    return 0;
}

static const char *clip_get_cmd(void) {
    if (wayland_session()) return "wl-paste -n 2>/dev/null";
    if (getenv("DISPLAY")) return "xclip -selection clipboard -o 2>/dev/null";
    return NULL;
}

static const char *clip_set_cmd(void) {
    if (wayland_session()) return "wl-copy 2>/dev/null";
    if (getenv("DISPLAY")) return "xclip -selection clipboard -i 2>/dev/null";
    return NULL;
}

static int pc_clip_get(uint8_t **out, size_t *outlen) {
    *out = NULL;
    *outlen = 0;
    const char *cmd = clip_get_cmd();
    if (!cmd) {
        fprintf(stderr, "no Wayland/X11 clipboard command available\n");
        return -2;
    }
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    int fd = fileno(fp);
    size_t cap = 4096, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { pclose(fp); return -1; }
    for (;;) {
        if (len == cap) {
            if (cap >= CLIPSYNC_MAX_PAYLOAD) { free(buf); buf = NULL; break; }
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf); buf = NULL; break;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    int st = pclose(fp);
    if (!buf || st == -1 || WEXITSTATUS(st) != 0) {
        free(buf);
        *out = NULL;
        *outlen = 0;
        return 0; /* no selection / empty clipboard */
    }
    if (len == 0) {
        free(buf);
        return 0;
    }
    *out = buf;
    *outlen = len;
    return 0;
}

static int pc_clip_set(const uint8_t *data, size_t len) {
    const char *cmd = clip_set_cmd();
    if (!cmd) return -2;
    FILE *fp = popen(cmd, "w");
    if (!fp) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = fwrite(data + off, 1, len - off, fp);
        if (n <= 0) { pclose(fp); return -1; }
        off += (size_t)n;
    }
    int st = pclose(fp);
    if (st == -1 || WEXITSTATUS(st) != 0) return -1;
    return 0;
}

static void state_set_memory(uint8_t **dst, size_t *dstlen, const uint8_t *src, size_t len) {
    free(*dst);
    *dst = NULL;
    *dstlen = 0;
    if (len) {
        uint8_t *p = malloc(len);
        if (p) {
            memcpy(p, src, len);
            *dst = p;
            *dstlen = len;
        }
    }
}

static int same_text(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
    return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

static int send_text(struct state *st, const uint8_t *data, size_t len, uint64_t ts) {
    if (len == 0) return 0;
    if (len > CLIPSYNC_MAX_PAYLOAD - TEXT_TS_OVERHEAD) return 0;
    uint8_t *buf = malloc(TEXT_TS_OVERHEAD + len);
    if (!buf) return -1;
    net_put_be64(buf, ts);
    memcpy(buf + TEXT_TS_OVERHEAD, data, len);
    int r = net_send_msg(st->fd, CLIPSYNC_MSG_TEXT_TS, buf,
                         (uint32_t)(TEXT_TS_OVERHEAD + len));
    free(buf);
    if (r == 0)
        fprintf(stderr, "pc -> phone: %zu bytes (ts=%llu)\n",
                len, (unsigned long long)ts);
    return r;
}

/* Read the local clipboard and update state.  Returns:
   -2 : clipboard backend unavailable
    0 : no change
    1 : local content changed and must be sent to the peer */
static int refresh_local_state(struct state *st) {
    uint8_t *cur = NULL;
    size_t curlen = 0;
    if (pc_clip_get(&cur, &curlen) < 0) return -2;
    int first = !st->local_seen;
    st->local_seen = 1;

    if (curlen == 0) {
        state_set_memory(&st->last_local, &st->last_local_len, NULL, 0);
        st->pending_send = 0;
        free(cur);
        return 0;
    }
    if (st->last_remote && same_text(cur, curlen, st->last_remote, st->last_remote_len)) {
        if (!(st->last_local && same_text(cur, curlen, st->last_local, st->last_local_len))) {
            state_set_memory(&st->last_local, &st->last_local_len, cur, curlen);
            st->local_ts = st->last_remote_ts;
        }
        st->pending_send = 0;
        free(cur);
        return 0;
    }
    if (st->last_local && same_text(cur, curlen, st->last_local, st->last_local_len)) {
        free(cur);
        return 0;
    }

    uint64_t ts = first ? 0 : net_now_ms();
    st->local_ts = ts;
    state_set_memory(&st->last_local, &st->last_local_len, cur, curlen);
    st->pending_send = 1;
    free(cur);
    return 1;
}

static int send_current_local(struct state *st) {
    if (!st->last_local || st->last_local_len == 0) {
        st->pending_send = 0;
        return 0;
    }
    int r = send_text(st, st->last_local, st->last_local_len, st->local_ts);
    if (r == 0) st->pending_send = 0;
    return r;
}

static int send_local_if_changed(struct state *st) {
    int r = refresh_local_state(st);
    if (r == -2) return -2; /* clipboard backend error */
    if (r == 0 && !st->pending_send) return 0;
    return send_current_local(st);
}

static int handle_text_from_phone(struct state *st, uint64_t ts,
                                  const uint8_t *text, size_t len) {
    if (len == 0) return 0;

    /* If the user copied something on the PC just before this packet
       arrived, make sure its newer timestamp is considered. */
    int rr = refresh_local_state(st);
    if (rr == 1) {
        if (send_current_local(st) < 0) return -1;
    }

    if (st->last_local && same_text(st->last_local, st->last_local_len, text, len)) {
        state_set_memory(&st->last_remote, &st->last_remote_len, text, len);
        st->last_remote_ts = ts;
        return 0;
    }

    int accept;
    if (!st->last_local || st->last_local_len == 0) {
        accept = 1;
    } else if (ts > st->local_ts) {
        accept = 1;
    } else if (ts < st->local_ts) {
        accept = 0;
    } else {
        /* Equal timestamp and different content: PC wins on tie. */
        accept = !st->prefer_local_on_tie;
    }

    if (!accept && st->last_local && st->last_local_len) {
        /* Keep PC's value and make sure the phone receives it too. */
        if (send_current_local(st) < 0) return -1;
    }

    if (accept) {
        if (pc_clip_set(text, len) < 0) {
            fprintf(stderr, "failed to set local clipboard\n");
            return -1;
        }
        state_set_memory(&st->last_remote, &st->last_remote_len, text, len);
        state_set_memory(&st->last_local, &st->last_local_len, text, len);
        st->local_ts = ts;
        st->last_remote_ts = ts;
        st->pending_send = 0;
        fprintf(stderr, "phone -> pc: %zu bytes (ts=%llu)\n",
                len, (unsigned long long)ts);
    } else {
        state_set_memory(&st->last_remote, &st->last_remote_len, text, len);
        st->last_remote_ts = ts;
        fprintf(stderr, "ignored older phone text (ts=%llu < %llu)\n",
                (unsigned long long)ts, (unsigned long long)st->local_ts);
    }
    return 0;
}

static int handle_connection(int fd, struct state *st) {
    st->fd = fd;
    net_set_timeout(fd, 10);

    uint8_t ver[4] = {0, 0, 0, 2};
    if (net_send_msg(fd, CLIPSYNC_MSG_HELLO, ver, sizeof(ver)) < 0) return -1;
    st->last_rx_ms = net_now_ms();
    st->last_ping_ms = st->last_rx_ms;

    int sr = send_local_if_changed(st);
    if (sr == -1) return -1;
    if (sr == -2) fprintf(stderr, "warning: unable to read local clipboard\n");

    for (;;) {
        int sr2 = send_local_if_changed(st);
        if (sr2 == -1) return -1;

        uint64_t now = net_now_ms();
        if (now - st->last_rx_ms > HEARTBEAT_TIMEOUT_MS) {
            fprintf(stderr, "heartbeat timeout\n");
            return -1;
        }
        if (now - st->last_ping_ms >= HEARTBEAT_INTERVAL_MS) {
            if (net_send_msg(fd, CLIPSYNC_MSG_PING, NULL, 0) < 0) return -1;
            st->last_ping_ms = now;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, st->poll_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            if (pfd.revents & POLLIN) {
                uint32_t type = 0, len = 0;
                uint8_t *payload = NULL;
                int r = net_recv_msg(fd, &type, &payload, &len);
                if (r < 0) return -1;
                st->last_rx_ms = net_now_ms();

                if (type == CLIPSYNC_MSG_TEXT_TS) {
                    if (len < TEXT_TS_OVERHEAD) {
                        free(payload);
                        return -1;
                    }
                    uint64_t ts = net_get_be64(payload);
                    if (handle_text_from_phone(st, ts, payload + TEXT_TS_OVERHEAD,
                                               len - TEXT_TS_OVERHEAD) < 0) {
                        free(payload);
                        return -1;
                    }
                } else if (type == CLIPSYNC_MSG_TEXT) {
                    if (handle_text_from_phone(st, 0, payload, len) < 0) {
                        free(payload);
                        return -1;
                    }
                } else if (type == CLIPSYNC_MSG_HELLO) {
                    /* version check ignored */
                } else if (type == CLIPSYNC_MSG_PING) {
                    if (net_send_msg(fd, CLIPSYNC_MSG_PONG, NULL, 0) < 0) {
                        free(payload);
                        return -1;
                    }
                } else if (type == CLIPSYNC_MSG_PONG) {
                    /* nothing */
                } else if (type == CLIPSYNC_MSG_BYE) {
                    free(payload);
                    return -1;
                }
                free(payload);
            }
        }
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [--listen HOST:PORT] [--poll-ms N]\n"
            "  %s --get\n"
            "  %s --set TEXT\n"
            "\n"
            "Defaults: --listen 0.0.0.0:52345 --poll-ms 300\n", prog, prog, prog);
}

static int parse_listen(const char *arg, char **host, char **port) {
    const char *colon = strrchr(arg, ':');
    if (!colon) return -1;
    size_t hlen = (size_t)(colon - arg);
    *host = strndup(arg, hlen);
    *port = strdup(colon + 1);
    if (!*host || !*port) { free(*host); free(*port); return -1; }
    if (hlen == 0) { free(*host); *host = strdup("0.0.0.0"); }
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const char *listen = "0.0.0.0:52345";
    int poll_ms = 300;
    int do_get = 0;
    const char *do_set = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--get") == 0) {
            do_get = 1;
        } else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
            do_set = argv[++i];
        } else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen = argv[++i];
        } else if (strcmp(argv[i], "--poll-ms") == 0 && i + 1 < argc) {
            poll_ms = atoi(argv[++i]);
            if (poll_ms < 50) poll_ms = 50;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (do_get) {
        uint8_t *p = NULL;
        size_t n = 0;
        if (pc_clip_get(&p, &n) < 0) return 1;
        if (n) fwrite(p, 1, n, stdout);
        free(p);
        return 0;
    }
    if (do_set) {
        return pc_clip_set((const uint8_t *)do_set, strlen(do_set)) == 0 ? 0 : 1;
    }

    char *host = NULL, *port = NULL;
    if (parse_listen(listen, &host, &port) < 0) {
        fprintf(stderr, "bad --listen address: %s\n", listen);
        return 2;
    }
    int lfd = net_listen_tcp(host, port);
    if (lfd < 0) {
        fprintf(stderr, "listen %s:%s failed\n", host, port);
        free(host); free(port);
        return 1;
    }
    fprintf(stderr, "clipsync pc listening on %s:%s (poll %d ms)\n", host, port, poll_ms);
    free(host); free(port);

    /* Keep the sync state across phone disconnects so we do not blindly
       re-send an unchanged PC clipboard every time the phone reconnects. */
    struct state st;
    memset(&st, 0, sizeof(st));
    st.poll_ms = poll_ms;
    st.prefer_local_on_tie = 1;

    for (;;) {
        /* While no phone is connected, keep watching the PC clipboard so a
           copy made during the outage is timestamped and queued for the next
           successful connection. */
        int rr = refresh_local_state(&st);
        if (rr == -2) {
            /* No clipboard backend yet; retry later. */
        }

        struct pollfd lpoll;
        lpoll.fd = lfd;
        lpoll.events = POLLIN;
        lpoll.revents = 0;
        int lpr = poll(&lpoll, 1, 1000);
        if (lpr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (lpr == 0) continue;
        if (lpoll.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(lpoll.revents & POLLIN)) continue;

        int cfd = net_accept(lfd);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        fprintf(stderr, "phone connected\n");
        handle_connection(cfd, &st);
        close(cfd);
        fprintf(stderr, "phone disconnected\n");
    }
    free(st.last_local);
    free(st.last_remote);
    close(lfd);
    return 0;
}
