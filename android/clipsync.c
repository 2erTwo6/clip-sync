#define _GNU_SOURCE
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/android/binder.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ------------------------------------------------------------------ */
/* small parcel builder                                               */

struct pbuf {
    uint8_t *d;
    size_t n, cap;
};

static void pb_reserve(struct pbuf *b, size_t need) {
    if (b->n + need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + need) cap *= 2;
    uint8_t *nd = realloc(b->d, cap);
    if (!nd) {
        perror("realloc");
        exit(1);
    }
    b->d = nd;
    b->cap = cap;
}
static void pb_raw(struct pbuf *b, const void *p, size_t n) {
    pb_reserve(b, n);
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void pb_u32(struct pbuf *b, uint32_t v) { pb_raw(b, &v, 4); }
static void pb_u64(struct pbuf *b, uint64_t v) { pb_raw(b, &v, 8); }
static void pb_zeros(struct pbuf *b, size_t n) {
    static const uint8_t z[4] = {0};
    while (n) {
        size_t k = n > 4 ? 4 : n;
        pb_raw(b, z, k);
        n -= k;
    }
}
static size_t align4(size_t n) { return (n + 3) & ~(size_t)3; }

static void pb_string16(struct pbuf *b, const char *s) {
    if (!s) { pb_u32(b, 0xffffffffu); return; }
    size_t n = strlen(s);
    pb_u32(b, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        uint16_t c = (uint16_t)(unsigned char)s[i];
        pb_raw(b, &c, 2);
    }
    uint16_t z = 0;
    pb_raw(b, &z, 2);
    size_t aligned = align4(b->n);
    if (aligned > b->n) pb_zeros(b, aligned - b->n);
}
static void pb_string8_n(struct pbuf *b, const uint8_t *s, size_t n) {
    if (!s) { pb_u32(b, 0xffffffffu); return; }
    pb_u32(b, (uint32_t)n);
    if (n) pb_raw(b, s, n);
    uint8_t z = 0;
    pb_raw(b, &z, 1);
    size_t aligned = align4(b->n);
    if (aligned > b->n) pb_zeros(b, aligned - b->n);
}
static void pb_iface_header(struct pbuf *b) {
    pb_u32(b, 0x80000000u); /* StrictModePolicy | STRICT_MODE_PENALTY_GATHER */
    pb_u32(b, 0xffffffffu); /* kUnsetWorkSource */
    pb_u32(b, 0x53595354u); /* B_PACK_CHARS('S','Y','S','T') */
}

/* ------------------------------------------------------------------ */
/* raw binder client                                                  */

struct binder_state {
    int fd;
    uint8_t *map;
    size_t mapsize;
};

struct binder_reply {
    uint8_t *data;
    size_t size;
    binder_size_t *offsets;
    size_t offsets_size;
};

static void die(const char *m) {
    perror(m);
    exit(1);
}

static int binder_write_read(struct binder_state *bs,
                             const void *w, size_t wlen,
                             void *r, size_t rlen,
                             size_t *wcons, size_t *rcons) {
    struct binder_write_read bwr;
    memset(&bwr, 0, sizeof(bwr));
    bwr.write_size = wlen;
    bwr.write_buffer = (binder_uintptr_t)(uintptr_t)w;
    bwr.read_size = rlen;
    bwr.read_buffer = (binder_uintptr_t)(uintptr_t)r;
    for (;;) {
        if (ioctl(bs->fd, BINDER_WRITE_READ, &bwr) >= 0) break;
        if (errno != EINTR) return -1;
    }
    if (wcons) *wcons = bwr.write_consumed;
    if (rcons) *rcons = bwr.read_consumed;
    return 0;
}

static void binder_inc_ref(struct binder_state *bs, uint32_t handle) {
    uint8_t out[16];
    uint32_t c1 = BC_INCREFS, c2 = BC_ACQUIRE;
    memcpy(out + 0, &c1, 4);
    memcpy(out + 4, &handle, 4);
    memcpy(out + 8, &c2, 4);
    memcpy(out + 12, &handle, 4);
    size_t wc = 0;
    binder_write_read(bs, out, sizeof(out), NULL, 0, &wc, NULL);
}

static void binder_free_buffer(struct binder_state *bs, binder_uintptr_t p) {
    uint8_t out[4 + sizeof(binder_uintptr_t)];
    uint32_t cmd = BC_FREE_BUFFER;
    memcpy(out, &cmd, 4);
    memcpy(out + 4, &p, sizeof(p));
    size_t wc = 0;
    binder_write_read(bs, out, sizeof(out), NULL, 0, &wc, NULL);
}

struct binder_state *binder_open(void) {
    struct binder_state *bs = calloc(1, sizeof(*bs));
    if (!bs) die("calloc");
    bs->fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
    if (bs->fd < 0) die("open /dev/binder");
    struct binder_version ver;
    if (ioctl(bs->fd, BINDER_VERSION, &ver) < 0) die("BINDER_VERSION");
    bs->mapsize = 1024 * 1024 - (size_t)sysconf(_SC_PAGE_SIZE) * 2;
    bs->map = mmap(NULL, bs->mapsize, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, bs->fd, 0);
    if (bs->map == MAP_FAILED) die("mmap binder");
    return bs;
}

static void binder_close(struct binder_state *bs) {
    if (!bs) return;
    if (bs->map && bs->map != MAP_FAILED) munmap(bs->map, bs->mapsize);
    if (bs->fd >= 0) close(bs->fd);
    free(bs);
}

static void binder_reply_free(struct binder_reply *r) {
    free(r->data);
    free(r->offsets);
    r->data = NULL;
    r->offsets = NULL;
    r->size = r->offsets_size = 0;
}

static int binder_call(struct binder_state *bs, uint32_t handle, uint32_t code,
                       const void *data, size_t data_size,
                       struct binder_reply *out) {
    uint8_t outcmd[4 + sizeof(struct binder_transaction_data)];
    uint8_t *readbuf = malloc(1024 * 1024);
    if (!readbuf) return -1;
    memset(out, 0, sizeof(*out));

    struct binder_transaction_data tr;
    memset(&tr, 0, sizeof(tr));
    tr.target.handle = handle;
    tr.code = code;
    tr.flags = TF_ACCEPT_FDS;
    tr.data_size = data_size;
    tr.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)data;
    tr.offsets_size = 0;
    tr.data.ptr.offsets = 0;

    uint32_t cmd = BC_TRANSACTION;
    memcpy(outcmd, &cmd, 4);
    memcpy(outcmd + 4, &tr, sizeof(tr));
    const size_t out_size = sizeof(outcmd);
    size_t out_off = 0;
    int got = 0, err = 0;

    while (!got) {
        size_t wc = 0, rc = 0;
        if (binder_write_read(bs, outcmd + out_off, out_size - out_off,
                              readbuf, 1024 * 1024, &wc, &rc) < 0) {
            err = -1;
            break;
        }
        out_off += wc;
        size_t pos = 0;
        while (pos + 4 <= rc) {
            uint32_t rcmd;
            memcpy(&rcmd, readbuf + pos, 4);
            pos += 4;
            if (rcmd == BR_NOOP || rcmd == BR_TRANSACTION_COMPLETE) continue;
            if (rcmd == BR_REPLY) {
                struct binder_transaction_data rtr;
                if (pos + sizeof(rtr) > rc) { err = -2; goto done; }
                memcpy(&rtr, readbuf + pos, sizeof(rtr));
                pos += sizeof(rtr);
                if (rtr.flags & TF_STATUS_CODE) {
                    int32_t status = 0;
                    memcpy(&status, (void *)(uintptr_t)rtr.data.ptr.buffer, sizeof(status));
                    err = status;
                } else {
                    out->data = malloc(rtr.data_size ? rtr.data_size : 1);
                    if (rtr.data_size)
                        memcpy(out->data, (void *)(uintptr_t)rtr.data.ptr.buffer, rtr.data_size);
                    out->size = rtr.data_size;
                    out->offsets_size = rtr.offsets_size;
                    if (rtr.offsets_size) {
                        out->offsets = malloc(rtr.offsets_size);
                        memcpy(out->offsets, (void *)(uintptr_t)rtr.data.ptr.offsets, rtr.offsets_size);
                    }
                    /* acquire binder handles in the reply before freeing its buffer */
                    for (size_t oi = 0; oi < rtr.offsets_size / sizeof(binder_size_t); oi++) {
                        binder_size_t off = out->offsets[oi];
                        if (off + 12 <= out->size) {
                            uint32_t type = 0, h = 0;
                            memcpy(&type, out->data + off, 4);
                            memcpy(&h, out->data + off + 8, 4);
                            if (type == BINDER_TYPE_HANDLE) binder_inc_ref(bs, h);
                        }
                    }
                }
                binder_free_buffer(bs, rtr.data.ptr.buffer);
                got = 1;
                break;
            } else if (rcmd == BR_DEAD_REPLY || rcmd == BR_FAILED_REPLY ||
                       rcmd == BR_FROZEN_REPLY || rcmd == BR_ONEWAY_SPAM_SUSPECT) {
                err = -3;
                got = 1;
                break;
            } else {
                err = -4;
                got = 1;
                break;
            }
        }
    }
done:
    free(readbuf);
    if (err) binder_reply_free(out);
    return err;
}

static int binder_get_service(struct binder_state *bs, const char *name, uint32_t *handle) {
    struct pbuf req = {0};
    pb_iface_header(&req);
    pb_string16(&req, "android.os.IServiceManager");
    pb_string16(&req, name);

    struct binder_reply rep;
    int rc = binder_call(bs, 0, 1, req.d, req.n, &rep);
    free(req.d);
    if (rc != 0) return rc;
    int ret = -1;
    if (rep.offsets_size >= sizeof(binder_size_t)) {
        binder_size_t off = rep.offsets[0];
        if (off + 12 <= rep.size) {
            uint32_t type = 0;
            memcpy(&type, rep.data + off, 4);
            memcpy(handle, rep.data + off + 8, 4);
            if (type == BINDER_TYPE_HANDLE) ret = 0;
        }
    }
    binder_reply_free(&rep);
    return ret;
}

/* ------------------------------------------------------------------ */
/* clipboard protocol                                                 */

static struct binder_state *g_bs = NULL;
static uint32_t g_clip_handle = 0;

static void phone_clip_shutdown(void) {
    if (g_bs) {
        binder_close(g_bs);
        g_bs = NULL;
    }
    g_clip_handle = 0;
}

static int phone_clip_ensure(void) {
    if (g_bs && g_clip_handle) return 0;
    phone_clip_shutdown();
    g_bs = binder_open();
    if (binder_get_service(g_bs, "clipboard", &g_clip_handle) != 0) {
        phone_clip_shutdown();
        return -1;
    }
    return 0;
}

static int rd_u32(const uint8_t *d, size_t n, size_t *p, uint32_t *v) {
    if (*p + 4 > n) return -1;
    memcpy(v, d + *p, 4);
    *p += 4;
    return 0;
}
static int rd_skip(const uint8_t *d, size_t n, size_t *p, size_t len) {
    (void)d;
    if (*p + len > n) return -1;
    *p += len;
    return 0;
}
static int rd_string8(const uint8_t *d, size_t n, size_t *p, uint8_t **out, size_t *outlen) {
    uint32_t l;
    if (rd_u32(d, n, p, &l) != 0) return -1;
    if (l == 0xffffffffu) {
        *out = NULL;
        *outlen = 0;
        return 0;
    }
    size_t raw = (size_t)l + 1;
    size_t padded = align4(raw);
    if (*p + padded > n) return -1;
    uint8_t *copy = malloc(l ? l : 1);
    if (!copy) return -1;
    if (l) memcpy(copy, d + *p, l);
    *p += padded;
    *out = copy;
    *outlen = l;
    return 0;
}
static int rd_string16_skip(const uint8_t *d, size_t n, size_t *p) {
    uint32_t l;
    if (rd_u32(d, n, p, &l) != 0) return -1;
    if (l == 0xffffffffu) return 0;
    size_t raw = (size_t)l * 2 + 2;
    size_t padded = align4(raw);
    return rd_skip(d, n, p, padded);
}
static int rd_charsequence_label_skip(const uint8_t *d, size_t n, size_t *p) {
    uint32_t kind;
    if (rd_u32(d, n, p, &kind) != 0) return -1;
    uint8_t *s = NULL;
    size_t sl = 0;
    if (rd_string8(d, n, p, &s, &sl) != 0) return -1;
    free(s);
    if (kind == 1) return 0;
    if (kind != 0) return -1;
    /* Spanned label: not needed for our text clips. Be conservative. */
    return -1;
}
static int skip_persistable_bundle(const uint8_t *d, size_t n, size_t *p) {
    uint32_t len;
    if (rd_u32(d, n, p, &len) != 0) return -1;
    if (len == 0xffffffffu || len == 0) return 0;
    /* length is inner-map byte count; magic + map + trailing bool */
    return rd_skip(d, n, p, 4 + (size_t)len + 4);
}
static int skip_bundle(const uint8_t *d, size_t n, size_t *p) {
    return skip_persistable_bundle(d, n, p);
}

static int phone_clip_get(uint8_t **out, size_t *outlen) {
    *out = NULL;
    *outlen = 0;
    if (phone_clip_ensure() != 0) return -1;

    struct pbuf req = {0};
    pb_iface_header(&req);
    pb_string16(&req, "android.content.IClipboard");
    pb_string16(&req, "com.android.shell");
    pb_string16(&req, "");
    pb_u32(&req, 0);
    pb_u32(&req, 0);

    struct binder_reply rep;
    int rc = binder_call(g_bs, g_clip_handle, 4, req.d, req.n, &rep);
    free(req.d);
    if (rc != 0) {
        phone_clip_shutdown();
        return -1;
    }

    size_t p = 0;
    uint32_t exc = 0, present = 0;
    if (rd_u32(rep.data, rep.size, &p, &exc) != 0) goto bad;
    if (exc != 0) goto bad;
    if (rd_u32(rep.data, rep.size, &p, &present) != 0) goto bad;
    if (present == 0) {
        binder_reply_free(&rep);
        return 0; /* empty clipboard */
    }

    /* ClipDescription */
    if (rd_charsequence_label_skip(rep.data, rep.size, &p) != 0) goto bad;
    uint32_t n_mime = 0;
    if (rd_u32(rep.data, rep.size, &p, &n_mime) != 0) goto bad;
    for (uint32_t i = 0; i < n_mime; i++) {
        if (rd_string16_skip(rep.data, rep.size, &p) != 0) goto bad;
    }
    if (skip_persistable_bundle(rep.data, rep.size, &p) != 0) goto bad;
    if (rd_skip(rep.data, rep.size, &p, 8) != 0) goto bad; /* timestamp */
    if (rd_skip(rep.data, rep.size, &p, 4) != 0) goto bad; /* isStyledText */
    if (rd_skip(rep.data, rep.size, &p, 4) != 0) goto bad; /* classification */
    if (skip_bundle(rep.data, rep.size, &p) != 0) goto bad;

    /* ClipData */
    uint32_t icon = 0, items = 0;
    if (rd_u32(rep.data, rep.size, &p, &icon) != 0) goto bad;
    if (icon != 0) goto bad; /* icons are rare on text clips */
    if (rd_u32(rep.data, rep.size, &p, &items) != 0) goto bad;
    if (items == 0) {
        binder_reply_free(&rep);
        return 0;
    }
    uint32_t kind = 0;
    if (rd_u32(rep.data, rep.size, &p, &kind) != 0) goto bad;
    if (kind != 0 && kind != 1) goto bad;
    if (rd_string8(rep.data, rep.size, &p, out, outlen) != 0) goto bad;
    binder_reply_free(&rep);
    return 0;

bad:
    binder_reply_free(&rep);
    return -1;
}

static void build_clipdata_text(struct pbuf *b, const uint8_t *text, size_t len) {
    pb_u32(b, 1);                 /* non-null ClipData */
    pb_u32(b, 1);                 /* label: plain String */
    pb_u32(b, 0xffffffffu);       /* label = null */
    pb_u32(b, 1);                 /* one MIME type */
    pb_string16(b, "text/plain");
    pb_u32(b, 0xffffffffu);       /* PersistableBundle extras = null */
    pb_u64(b, 0);                 /* timestamp */
    pb_u32(b, 0);                 /* isStyledText */
    pb_u32(b, 1);                 /* CLASSIFICATION_NOT_COMPLETE */
    pb_u32(b, 0);                 /* empty confidence Bundle */
    pb_u32(b, 0);                 /* icon = null */
    pb_u32(b, 1);                 /* one item */
    pb_u32(b, 1);                 /* TextUtils plain String */
    pb_string8_n(b, text, len);
    pb_u32(b, 0xffffffffu);       /* htmlText = null */
    pb_u32(b, 0);                 /* Intent */
    pb_u32(b, 0);                 /* IntentSender */
    pb_u32(b, 0);                 /* Uri */
    pb_u32(b, 0);                 /* ActivityInfo */
    pb_u32(b, 0);                 /* TextLinks */
}

static int phone_clip_set(const uint8_t *text, size_t len) {
    if (phone_clip_ensure() != 0) return -1;
    struct pbuf req = {0};
    pb_iface_header(&req);
    pb_string16(&req, "android.content.IClipboard");
    build_clipdata_text(&req, text, len);
    pb_string16(&req, "com.android.shell");
    pb_string16(&req, "");
    pb_u32(&req, 0);
    pb_u32(&req, 0);

    struct binder_reply rep;
    int rc = binder_call(g_bs, g_clip_handle, 1, req.d, req.n, &rep);
    free(req.d);
    if (rc != 0) {
        phone_clip_shutdown();
        return -1;
    }
    int ret = 0;
    if (rep.size >= 4) {
        uint32_t exc = 0;
        memcpy(&exc, rep.data, 4);
        if (exc != 0) ret = -1;
    } else {
        ret = -1;
    }
    binder_reply_free(&rep);
    if (ret) phone_clip_shutdown();
    return ret;
}

/* ------------------------------------------------------------------ */
/* sync loop                                                          */

#define HEARTBEAT_INTERVAL_MS 5000u
#define HEARTBEAT_TIMEOUT_MS 15000u
#define TEXT_TS_OVERHEAD 8u

struct sync_state {
    int fd;
    int poll_ms;
    int prefer_local_on_tie; /* Android = 0, PC = 1 */
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

static int send_text(struct sync_state *st, const uint8_t *data, size_t len, uint64_t ts) {
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
        fprintf(stderr, "phone -> pc: %zu bytes (ts=%llu)\n",
                len, (unsigned long long)ts);
    return r;
}

/* Read the local clipboard and update state.  Returns:
   -2 : clipboard/binder backend unavailable
    0 : no change
    1 : local content changed and must be sent to the peer */
static int refresh_local_state(struct sync_state *st) {
    uint8_t *cur = NULL;
    size_t curlen = 0;
    if (phone_clip_get(&cur, &curlen) != 0) return -2;
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

static int send_current_local(struct sync_state *st) {
    if (!st->last_local || st->last_local_len == 0) {
        st->pending_send = 0;
        return 0;
    }
    int r = send_text(st, st->last_local, st->last_local_len, st->local_ts);
    if (r == 0) st->pending_send = 0;
    return r;
}

static int send_local_if_changed(struct sync_state *st) {
    int r = refresh_local_state(st);
    if (r == -2) return -2; /* clipboard backend error */
    if (r == 0 && !st->pending_send) return 0;
    return send_current_local(st);
}

static int handle_text_from_pc(struct sync_state *st, uint64_t ts,
                               const uint8_t *text, size_t len) {
    if (len == 0) return 0;

    /* If the user copied something on the phone just before this packet
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

    if (accept) {
        if (phone_clip_set(text, len) != 0) {
            fprintf(stderr, "failed to set phone clipboard\n");
            return -1;
        }
        state_set_memory(&st->last_remote, &st->last_remote_len, text, len);
        state_set_memory(&st->last_local, &st->last_local_len, text, len);
        st->local_ts = ts;
        st->last_remote_ts = ts;
        st->pending_send = 0;
        fprintf(stderr, "pc -> phone: %zu bytes (ts=%llu)\n",
                len, (unsigned long long)ts);
    } else {
        state_set_memory(&st->last_remote, &st->last_remote_len, text, len);
        st->last_remote_ts = ts;
        fprintf(stderr, "ignored older pc text (ts=%llu < %llu)\n",
                (unsigned long long)ts, (unsigned long long)st->local_ts);
    }
    return 0;
}

static int run_session(struct sync_state *st) {
    net_set_timeout(st->fd, 10);

    uint8_t ver[4] = {0, 0, 0, 2};
    if (net_send_msg(st->fd, CLIPSYNC_MSG_HELLO, ver, sizeof(ver)) < 0) return -1;
    st->last_rx_ms = net_now_ms();
    st->last_ping_ms = st->last_rx_ms;

    int sr = send_local_if_changed(st);
    if (sr == -1) return -1;
    if (sr == -2) fprintf(stderr, "warning: unable to read phone clipboard\n");

    for (;;) {
        int sr2 = send_local_if_changed(st);
        if (sr2 == -1) return -1;

        uint64_t now = net_now_ms();
        if (now - st->last_rx_ms > HEARTBEAT_TIMEOUT_MS) {
            fprintf(stderr, "heartbeat timeout\n");
            return -1;
        }
        if (now - st->last_ping_ms >= HEARTBEAT_INTERVAL_MS) {
            if (net_send_msg(st->fd, CLIPSYNC_MSG_PING, NULL, 0) < 0) return -1;
            st->last_ping_ms = now;
        }

        struct pollfd pfd;
        pfd.fd = st->fd;
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
                int r = net_recv_msg(st->fd, &type, &payload, &len);
                if (r < 0) return -1;
                st->last_rx_ms = net_now_ms();

                if (type == CLIPSYNC_MSG_TEXT_TS) {
                    if (len < TEXT_TS_OVERHEAD) {
                        free(payload);
                        return -1;
                    }
                    uint64_t ts = net_get_be64(payload);
                    if (handle_text_from_pc(st, ts, payload + TEXT_TS_OVERHEAD,
                                            len - TEXT_TS_OVERHEAD) < 0) {
                        free(payload);
                        return -1;
                    }
                } else if (type == CLIPSYNC_MSG_TEXT) {
                    if (handle_text_from_pc(st, 0, payload, len) < 0) {
                        free(payload);
                        return -1;
                    }
                } else if (type == CLIPSYNC_MSG_PING) {
                    if (net_send_msg(st->fd, CLIPSYNC_MSG_PONG, NULL, 0) < 0) {
                        free(payload);
                        return -1;
                    }
                } else if (type == CLIPSYNC_MSG_PONG ||
                           type == CLIPSYNC_MSG_HELLO) {
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

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static void phone_client_loop(const char *host, const char *port, int poll_ms) {
    struct sync_state st;
    memset(&st, 0, sizeof(st));
    st.prefer_local_on_tie = 0;

    int retry_ms = 1000;
    for (;;) {
        /* Keep detecting local copies while PC is unreachable, so the
           reconnect sends the latest text with a useful timestamp. */
        (void)refresh_local_state(&st);

        int fd = net_connect_tcp(host, port, 5000);
        if (fd < 0) {
            sleep_ms(retry_ms);
            if (retry_ms < 10000) retry_ms *= 2;
            continue;
        }
        retry_ms = 1000;

        fprintf(stderr, "connected to pc %s:%s\n", host, port);
        st.fd = fd;
        st.poll_ms = poll_ms;
        run_session(&st);
        close(fd);
        fprintf(stderr, "pc disconnected, retrying\n");
        sleep_ms(1000);
    }
}

/* ------------------------------------------------------------------ */
/* config / main                                                      */

static void drop_to_shell_uid(void) {
    /* KernelSU starts us as root; binder needs uid 2000 / com.android.shell
       for clipboard package ownership and READ_CLIPBOARD_IN_BACKGROUND. */
    if (setresgid(0, 2000, 0) != 0) perror("setresgid");
    if (setresuid(0, 2000, 0) != 0) perror("setresuid");
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}
static void load_config(const char *path, char **host, char **port, int *poll_ms) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!*line || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (strcmp(k, "host") == 0) { free(*host); *host = strdup(v); }
        else if (strcmp(k, "port") == 0) { free(*port); *port = strdup(v); }
        else if (strcmp(k, "poll_ms") == 0) { int x = atoi(v); if (x >= 50) *poll_ms = x; }
    }
    fclose(f);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [--config FILE] [--host HOST] [--port PORT] [--poll-ms N]\n"
            "  %s --get\n"
            "  %s --set TEXT\n"
            "\n"
            "Default config: /data/adb/modules/clipsync/clipsync.conf\n", prog, prog, prog);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    drop_to_shell_uid();

    const char *config = "/data/adb/modules/clipsync/clipsync.conf";
    char *host = strdup("192.168.31.92");
    char *port = strdup("52345");
    int poll_ms = 3000;
    int do_get = 0;
    const char *do_set = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config = argv[++i];
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) { free(host); host = strdup(argv[++i]); }
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) { free(port); port = strdup(argv[++i]); }
        else if (strcmp(argv[i], "--poll-ms") == 0 && i + 1 < argc) { poll_ms = atoi(argv[++i]); if (poll_ms < 50) poll_ms = 50; }
        else if (strcmp(argv[i], "--get") == 0) do_get = 1;
        else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) do_set = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    if (!do_get && !do_set) load_config(config, &host, &port, &poll_ms);

    if (do_get) {
        uint8_t *p = NULL;
        size_t n = 0;
        if (phone_clip_get(&p, &n) != 0) return 1;
        if (n) fwrite(p, 1, n, stdout);
        free(p);
        phone_clip_shutdown();
        return 0;
    }
    if (do_set) {
        int r = phone_clip_set((const uint8_t *)do_set, strlen(do_set));
        phone_clip_shutdown();
        return r == 0 ? 0 : 1;
    }

    fprintf(stderr, "clipsync phone -> %s:%s poll=%d ms\n", host, port, poll_ms);
    phone_client_loop(host, port, poll_ms);
    phone_clip_shutdown();
    free(host);
    free(port);
    return 0;
}
