/* tabbli v1 — a self-contained table: this executable carries its own data.
 *
 * Copyright (c) 2026 Giuseppe Federico <giuseppefeder@gmail.com>
 * SPDX-License-Identifier: MIT — see LICENSE. This notice must be preserved.
 *
 * File layout:
 *   [ELF engine][64-byte header][record area: text lines, one record each]
 *
 * The engine finds its header at its own ELF size, computed from the ELF
 * section-header table (e_shoff + e_shnum * e_shentsize), so no build-time
 * offset patching is needed. All writes rebuild the file in a temp sibling
 * and rename() over the original: the kernel forbids writing an executing
 * inode (ETXTBSY), and the swap is atomic — readers and exec()ers always
 * see either the old or the new file, never a torn state. Writers serialize
 * on a flock()ed sibling lockfile; readers are lock-free (they read the
 * committed data_len of whichever version they opened).
 *
 * Record lines use the same grammar as the CLI:  id=7 title='fix lock' n=2
 * Values are bare or single-quoted ('\'' escapes a quote), shell-safe to
 * copy back into a command. Timestamps are engine-written (never trusted
 * from the agent) unless the table was created with `init <path> no-ts`.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ENGINE_VERSION "tabbli v1"
#define FMT_VERSION 1
#define HDR_SIZE 64
#define MAGIC "TABBLI01" /* 8 bytes */
#define MAX_DATA (64u << 20)
#define MAX_FIELDS 64
#define DEF_LIMIT 100
#define FLAG_NOTS 1u

/* exit codes: 0 ok, 1 not found / empty queue, 2 usage, 3 lock busy,
 * 4 condition (if=) failed, 5 corrupt table */

/* ---------- small utils ---------- */

static void die(int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    exit(code);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die(5, "# error: out of memory");
    return p;
}

static char *xstrdup(const char *s) {
    char *p = xmalloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

typedef struct { char *s; size_t len, cap; } SB;

static void sb_grow(SB *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    b->cap = b->cap ? b->cap * 2 : 256;
    while (b->cap < b->len + need + 1) b->cap *= 2;
    b->s = realloc(b->s, b->cap);
    if (!b->s) die(5, "# error: out of memory");
}

static void sb_putc(SB *b, char c) { sb_grow(b, 1); b->s[b->len++] = c; b->s[b->len] = 0; }

static void sb_puts(SB *b, const char *s) {
    size_t n = strlen(s);
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void sb_fmt(SB *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_puts(b, tmp);
}

static ssize_t xpread(int fd, void *buf, size_t n, off_t off) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, off + (off_t)got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static int xwrite(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, (const char *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)r;
    }
    return 0;
}

/* ---------- crc32 (IEEE, table-based) ---------- */

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_buf(const void *buf, size_t len) {
    const unsigned char *p = buf;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------- time ---------- */

static void now_iso(char out[24]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static long iso_epoch(const char *s) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (long)timegm(&tm);
}

/* ---------- ELF self-measurement ---------- */

static off_t elf_size_fd(int fd) {
    unsigned char e[64];
    if (xpread(fd, e, 64, 0) != 64) return -1;
    if (memcmp(e, "\x7f" "ELF", 4) != 0) return -1;
    uint64_t shoff;
    uint16_t shent, shnum;
    memcpy(&shoff, e + 0x28, 8);
    memcpy(&shent, e + 0x3A, 2);
    memcpy(&shnum, e + 0x3C, 2);
    if (!shoff || !shent || !shnum) return -1;
    return (off_t)(shoff + (uint64_t)shent * shnum);
}

/* ---------- header ---------- */

typedef struct {
    uint16_t version, flags;
    uint32_t count;
    uint64_t data_len, last_id;
    uint32_t crc;
} Hdr;

static void hdr_pack(const Hdr *h, unsigned char out[HDR_SIZE]) {
    memset(out, 0, HDR_SIZE);
    memcpy(out, MAGIC, 8);
    memcpy(out + 8, &h->version, 2);
    memcpy(out + 10, &h->flags, 2);
    memcpy(out + 12, &h->count, 4);
    memcpy(out + 16, &h->data_len, 8);
    memcpy(out + 24, &h->last_id, 8);
    memcpy(out + 32, &h->crc, 4);
}

static int hdr_unpack(const unsigned char in[HDR_SIZE], Hdr *h) {
    if (memcmp(in, MAGIC, 8) != 0) return -1;
    memcpy(&h->version, in + 8, 2);
    memcpy(&h->flags, in + 10, 2);
    memcpy(&h->count, in + 12, 4);
    memcpy(&h->data_len, in + 16, 8);
    memcpy(&h->last_id, in + 24, 8);
    memcpy(&h->crc, in + 32, 4);
    return 0;
}

/* ---------- records ---------- */

typedef struct { char *k, *v; } KV;

typedef struct {
    uint64_t id;
    KV f[MAX_FIELDS];
    int nf;
} Rec;

typedef struct {
    char path[PATH_MAX];
    off_t engine_size; /* of the table file on disk */
    int is_seed;       /* bare engine, no header/data yet */
    Hdr h;
    Rec *recs;
    int n, cap;
} Tab;

static const char *ENGINE_KEYS[] = {"created_at", "updated_at", "created_by", "updated_by", NULL};

static int is_engine_key(const char *k) {
    for (int i = 0; ENGINE_KEYS[i]; i++)
        if (strcmp(k, ENGINE_KEYS[i]) == 0) return 1;
    return 0;
}

static int is_reserved_key(const char *k) {
    static const char *words[] = {"id", "by", "if", "set", "limit", "count", "ts", NULL};
    for (int i = 0; words[i]; i++)
        if (strcmp(k, words[i]) == 0) return 1;
    return is_engine_key(k);
}

static int key_ok(const char *k) {
    if (!*k) return 0;
    if (!isalpha((unsigned char)*k) && *k != '_') return 0;
    for (const char *p = k; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    return 1;
}

static int val_is_bare(const char *v) {
    if (!*v) return 0;
    for (const char *p = v; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("_.:+@/-", *p)) return 0;
    return 1;
}

/* shell-safe single-quote encoding: ' becomes '\'' */
static void encode_val(SB *b, const char *v) {
    if (val_is_bare(v)) { sb_puts(b, v); return; }
    sb_putc(b, '\'');
    for (const char *p = v; *p; p++) {
        if (*p == '\'') sb_puts(b, "'\\''");
        else sb_putc(b, *p);
    }
    sb_putc(b, '\'');
}

/* values are canonicalized at input: real newlines become the 2-char "\n" */
static char *sanitize_val(const char *v) {
    SB b = {0};
    for (const char *p = v; *p; p++) {
        if (*p == '\n') sb_puts(&b, "\\n");
        else if (*p != '\r') sb_putc(&b, *p);
    }
    if (!b.s) return xstrdup("");
    return b.s;
}

/* parse one encoded value starting at *pp; returns malloc'd raw value,
 * advances *pp past it. NULL on syntax error. */
static char *decode_val(const char **pp) {
    const char *p = *pp;
    SB b = {0};
    sb_grow(&b, 8);
    if (*p == '\'') {
        p++;
        for (;;) {
            if (!*p) { free(b.s); return NULL; } /* unterminated */
            if (*p == '\'') {
                if (p[1] == '\\' && p[2] == '\'' && p[3] == '\'') { sb_putc(&b, '\''); p += 4; }
                else { p++; break; }
            } else sb_putc(&b, *p++);
        }
    } else {
        while (*p && *p != ' ') sb_putc(&b, *p++);
    }
    *pp = p;
    return b.s;
}

/* parse a stored line "id=N k=v ..." into rec; returns 0 on success */
static int parse_line(const char *line, Rec *r) {
    memset(r, 0, sizeof *r);
    const char *p = line;
    int first = 1;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *ks = p;
        while (*p && *p != '=' && *p != ' ') p++;
        if (*p != '=') return -1;
        char *k = xstrndup(ks, (size_t)(p - ks));
        p++;
        char *v = decode_val(&p);
        if (!v) { free(k); return -1; }
        if (first) {
            if (strcmp(k, "id") != 0) { free(k); free(v); return -1; }
            char *end;
            r->id = strtoull(v, &end, 10);
            if (*end || end == v) { free(k); free(v); return -1; }
            free(k);
            free(v);
            first = 0;
            continue;
        }
        if (r->nf >= MAX_FIELDS) { free(k); free(v); return -1; }
        r->f[r->nf].k = k;
        r->f[r->nf].v = v;
        r->nf++;
    }
    return first ? -1 : 0;
}

static const char *rec_get(const Rec *r, const char *k) {
    for (int i = 0; i < r->nf; i++)
        if (strcmp(r->f[i].k, k) == 0) return r->f[i].v;
    return NULL;
}

/* set/replace a field; new user fields are inserted before engine fields
 * so timestamps stay visually last */
static void rec_set(Rec *r, const char *k, const char *v) {
    for (int i = 0; i < r->nf; i++)
        if (strcmp(r->f[i].k, k) == 0) {
            free(r->f[i].v);
            r->f[i].v = xstrdup(v);
            return;
        }
    if (r->nf >= MAX_FIELDS) die(2, "# error: record full (max %d fields)", MAX_FIELDS);
    int pos = r->nf;
    if (!is_engine_key(k)) {
        for (int i = 0; i < r->nf; i++)
            if (is_engine_key(r->f[i].k)) { pos = i; break; }
    }
    memmove(&r->f[pos + 1], &r->f[pos], (size_t)(r->nf - pos) * sizeof(KV));
    r->f[pos].k = xstrdup(k);
    r->f[pos].v = xstrdup(v);
    r->nf++;
}

static void rec_print(const Rec *r, int show_ts) {
    SB b = {0};
    sb_fmt(&b, "id=%" PRIu64, r->id);
    for (int i = 0; i < r->nf; i++) {
        if (!show_ts && is_engine_key(r->f[i].k)) continue;
        sb_putc(&b, ' ');
        sb_puts(&b, r->f[i].k);
        sb_putc(&b, '=');
        encode_val(&b, r->f[i].v);
    }
    puts(b.s ? b.s : "");
    free(b.s);
}

/* ---------- table load / write ---------- */

static void self_table_path(char out[PATH_MAX]) {
    ssize_t n = readlink("/proc/self/exe", out, PATH_MAX - 1);
    if (n <= 0) die(5, "# error: cannot resolve own path via /proc/self/exe");
    out[n] = 0;
    /* if we were renamed-over while running, the kernel appends " (deleted)" */
    size_t len = strlen(out), suf = strlen(" (deleted)");
    if (len > suf && strcmp(out + len - suf, " (deleted)") == 0) out[len - suf] = 0;
}

static void tab_load(Tab *t) {
    /* idempotent: writers re-load under the lock after main's first load */
    free(t->recs);
    t->recs = NULL;
    t->n = t->cap = 0;
    t->is_seed = 0;
    int fd = open(t->path, O_RDONLY);
    if (fd < 0) die(5, "# error: cannot open %s: %s", t->path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) die(5, "# error: stat failed: %s", strerror(errno));
    off_t esz = elf_size_fd(fd);
    if (esz < 0 || esz > st.st_size)
        die(5, "# error: %s is not a tabbli file (bad ELF layout)", t->path);
    t->engine_size = esz;
    if (st.st_size == esz) { t->is_seed = 1; close(fd); return; }
    if (st.st_size < esz + HDR_SIZE)
        die(5, "# error: %s: truncated header — corrupt table", t->path);
    unsigned char hb[HDR_SIZE];
    if (xpread(fd, hb, HDR_SIZE, esz) != HDR_SIZE || hdr_unpack(hb, &t->h) != 0)
        die(5, "# error: %s: no tabbli header found — not a table, or corrupt", t->path);
    if (t->h.version != FMT_VERSION)
        die(5, "# error: table format v%u, this engine speaks v%u", t->h.version, FMT_VERSION);
    if (t->h.data_len > MAX_DATA)
        die(5, "# error: data_len %" PRIu64 " exceeds the %u MB cap — corrupt header",
            t->h.data_len, MAX_DATA >> 20);
    if ((off_t)(esz + HDR_SIZE + (off_t)t->h.data_len) > st.st_size)
        die(5, "# error: header claims more data than the file holds — corrupt table");
    char *data = xmalloc(t->h.data_len + 1);
    if (xpread(fd, data, t->h.data_len, esz + HDR_SIZE) != (ssize_t)t->h.data_len)
        die(5, "# error: short read of record area");
    data[t->h.data_len] = 0;
    close(fd);
    if (crc32_buf(data, t->h.data_len) != t->h.crc)
        die(5, "# error: crc mismatch — torn write or corruption. if a write just "
               "happened, retry; otherwise the table needs recovery");
    /* split lines and parse */
    t->cap = 64;
    t->recs = xmalloc((size_t)t->cap * sizeof(Rec));
    char *p = data;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (*p) {
            if (t->n == t->cap) {
                t->cap *= 2;
                t->recs = realloc(t->recs, (size_t)t->cap * sizeof(Rec));
                if (!t->recs) die(5, "# error: out of memory");
            }
            if (parse_line(p, &t->recs[t->n]) != 0)
                die(5, "# error: unparseable record line — corrupt table: %.80s", p);
            t->n++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(data);
}

static void serialize(const Tab *t, SB *out) {
    for (int i = 0; i < t->n; i++) {
        const Rec *r = &t->recs[i];
        sb_fmt(out, "id=%" PRIu64, r->id);
        for (int j = 0; j < r->nf; j++) {
            sb_putc(out, ' ');
            sb_puts(out, r->f[j].k);
            sb_putc(out, '=');
            encode_val(out, r->f[j].v);
        }
        sb_putc(out, '\n');
    }
}

/* write a new version of the table next to it and atomically swap it in.
 * caller must hold the write lock. */
static void tab_write(Tab *t) {
    SB data = {0};
    serialize(t, &data);
    if (data.len > MAX_DATA) die(2, "# error: table would exceed the %u MB cap", MAX_DATA >> 20);
    t->h.version = FMT_VERSION;
    t->h.count = (uint32_t)t->n;
    t->h.data_len = data.len;
    t->h.crc = crc32_buf(data.s ? data.s : "", data.len);

    int self = open("/proc/self/exe", O_RDONLY);
    if (self < 0) die(5, "# error: cannot open own engine: %s", strerror(errno));
    off_t self_esz = elf_size_fd(self);
    if (self_esz < 0) die(5, "# error: cannot measure own engine size");

    char tmp[PATH_MAX + 32];
    const char *slash = strrchr(t->path, '/');
    if (slash)
        snprintf(tmp, sizeof tmp, "%.*s/.%s.tmp.XXXXXX", (int)(slash - t->path), t->path, slash + 1);
    else
        snprintf(tmp, sizeof tmp, ".%s.tmp.XXXXXX", t->path);
    int fd = mkstemp(tmp);
    if (fd < 0) die(5, "# error: cannot create temp file in table directory: %s", strerror(errno));

    char buf[65536];
    off_t off = 0;
    while (off < self_esz) {
        size_t want = (size_t)(self_esz - off) < sizeof buf ? (size_t)(self_esz - off) : sizeof buf;
        ssize_t r = xpread(self, buf, want, off);
        if (r <= 0) die(5, "# error: engine read failed");
        if (xwrite(fd, buf, (size_t)r) != 0) die(5, "# error: engine write failed");
        off += r;
    }
    close(self);
    unsigned char hb[HDR_SIZE];
    hdr_pack(&t->h, hb);
    if (xwrite(fd, hb, HDR_SIZE) != 0 || xwrite(fd, data.s ? data.s : "", data.len) != 0)
        die(5, "# error: write failed: %s", strerror(errno));
    if (fdatasync(fd) != 0) die(5, "# error: fdatasync failed: %s", strerror(errno));
    fchmod(fd, 0755);
    close(fd);
    if (rename(tmp, t->path) != 0) {
        unlink(tmp);
        die(5, "# error: atomic swap failed: %s", strerror(errno));
    }
    free(data.s);
}

/* ---------- locking ---------- */

static void take_lock(const char *path) {
    char lp[PATH_MAX + 16];
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(lp, sizeof lp, "%.*s/.%s.lock", (int)(slash - path), path, slash + 1);
    else
        snprintf(lp, sizeof lp, ".%s.lock", path);
    int fd = open(lp, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) die(3, "# error: cannot open lockfile %s: %s", lp, strerror(errno));
    for (int i = 0; i < 50; i++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return; /* held until exit */
        usleep(100000);
    }
    die(3, "# error: lock busy — another agent is writing this table. retry shortly");
}

/* ---------- filters ---------- */

enum Op { OP_EQ, OP_NE, OP_GT, OP_LT, OP_GE, OP_LE, OP_HAS };

typedef struct { char *k; enum Op op; char *v; } Term;

static int parse_term(const char *arg, Term *t) {
    const char *p = arg;
    while (*p) {
        if (p[0] == '!' && p[1] == '=') { t->op = OP_NE; goto found; }
        if (p[0] == '>' && p[1] == '=') { t->op = OP_GE; goto found; }
        if (p[0] == '<' && p[1] == '=') { t->op = OP_LE; goto found; }
        if (*p == '=') { t->op = OP_EQ; goto found; }
        if (*p == '>') { t->op = OP_GT; goto found; }
        if (*p == '<') { t->op = OP_LT; goto found; }
        if (*p == '~') { t->op = OP_HAS; goto found; }
        p++;
    }
    return -1;
found:
    t->k = xstrndup(arg, (size_t)(p - arg));
    t->v = xstrdup(p + ((t->op == OP_NE || t->op == OP_GE || t->op == OP_LE) ? 2 : 1));
    if (!key_ok(t->k)) return -1;
    return 0;
}

static int both_num(const char *a, const char *b, double *x, double *y) {
    char *e;
    *x = strtod(a, &e);
    if (e == a || *e) return 0;
    *y = strtod(b, &e);
    if (e == b || *e) return 0;
    return 1;
}

static int term_match(const Rec *r, const Term *t) {
    const char *v = rec_get(r, t->k);
    if (strcmp(t->k, "id") == 0) { /* allow filtering on id too */
        static char idbuf[24];
        snprintf(idbuf, sizeof idbuf, "%" PRIu64, r->id);
        v = idbuf;
    }
    if (!v) return t->op == OP_NE; /* absent field: only != matches */
    double x, y;
    int cmp;
    if (both_num(v, t->v, &x, &y)) cmp = (x > y) - (x < y);
    else cmp = strcmp(v, t->v) > 0 ? 1 : (strcmp(v, t->v) < 0 ? -1 : 0);
    switch (t->op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_GT: return cmp > 0;
        case OP_LT: return cmp < 0;
        case OP_GE: return cmp >= 0;
        case OP_LE: return cmp <= 0;
        case OP_HAS: return strstr(v, t->v) != NULL;
    }
    return 0;
}

/* ---------- shared command helpers ---------- */

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void check_assign_key(const char *k) {
    if (!key_ok(k))
        die(2, "# error: bad field name '%s' — use letters, digits, _ (must start with a letter)", k);
    if (strcmp(k, "id") == 0)
        die(2, "# error: 'id' is assigned by the engine, never by you. drop it");
    if (is_engine_key(k))
        die(2, "# error: '%s' is engine-owned (the engine has the clock, you don't). drop it", k);
    if (is_reserved_key(k))
        die(2, "# error: '%s' is a reserved word and cannot be a field name", k);
}

/* split "k=v" assignment; dies with a teaching error if malformed */
static void parse_assign(const char *arg, char **k, char **v) {
    const char *eq = strchr(arg, '=');
    if (!eq || eq == arg)
        die(2, "# error: '%s' is not an assignment. use: field=value (quote values "
               "with spaces: title='fix the lock')", arg);
    *k = xstrndup(arg, (size_t)(eq - arg));
    check_assign_key(*k);
    *v = sanitize_val(eq + 1);
}

static uint64_t parse_id_arg(const char *cmd, const char *arg) {
    if (!arg)
        die(2, "# error: %s needs an id. usage: %s <id> ...  example: %s 7", cmd, cmd, cmd);
    char *end;
    uint64_t id = strtoull(arg, &end, 10);
    if (*end || end == arg)
        die(2, "# error: '%s' is not an id (a plain number). example: %s 7", arg, cmd);
    return id;
}

static int find_rec(const Tab *t, uint64_t id) {
    for (int i = 0; i < t->n; i++)
        if (t->recs[i].id == id) return i;
    return -1;
}

static void die_not_found(const Tab *t, uint64_t id) {
    if (id <= t->h.last_id)
        die(1, "# error: id=%" PRIu64 " was deleted (ids are never reused). last id=%" PRIu64,
            id, t->h.last_id);
    die(1, "# error: id=%" PRIu64 " never existed — last id is %" PRIu64, id, t->h.last_id);
}

static void stamp(Rec *r, int creating, const char *by, int no_ts) {
    if (no_ts) return;
    char iso[24];
    now_iso(iso);
    if (creating) {
        rec_set(r, "created_at", iso);
        if (by) rec_set(r, "created_by", by);
    } else {
        rec_set(r, "updated_at", iso);
        if (by) rec_set(r, "updated_by", by);
    }
}

/* ---------- help ---------- */

static void help_table(const Tab *t) {
    const char *n = base_name(t->path);
    printf("# %s - self-contained table: data lives inside this file\n", ENGINE_VERSION);
    printf("#   a     ./%s a title='fix lock' status=open   -> id=7 title='fix lock' status=open\n", n);
    printf("#   q     ./%s q status=open prio>1             -> one record per line, then # N records\n", n);
    printf("#   g     ./%s g 7                              -> the full record, timestamps included\n", n);
    printf("#   s     ./%s s 7 status=done if status=open   -> updates fields ('if' = only when true)\n", n);
    printf("#   d     ./%s d 7\n", n);
    printf("#   i     ./%s i                                -> # N records, fields, last id\n", n);
    printf("#   next  ./%s next status=open set status=claimed by=me   (atomic take, race-free)\n", n);
    printf("#   cursor / diff <cursor>   ./%s cursor; later: ./%s diff <cursor>  -> what changed\n", n, n);
    printf("# filters: = != > < >= <= ~contains    extras: limit=N count ts\n");
    printf("# q alone lists all (default cap %d). free key=value fields, no schema. ids never reused.\n", DEF_LIMIT);
    printf("# timestamps (created_at/updated_at) are engine-written%s; by=name records authorship.\n",
           (t->h.flags & FLAG_NOTS) ? " [DISABLED on this table: no-ts]" : "");
    printf("# writes are atomic+locked: on \"lock busy\" just retry.\n");
    printf("# (c) Giuseppe Federico — MIT license\n");
}

static void help_seed(const char *path) {
    printf("# %s seed - creates self-contained tables (engine+data in one file)\n", ENGINE_VERSION);
    printf("#   init   %s init ./backlog.tbl        create a table\n", path);
    printf("#   init   %s init ./log.tbl no-ts      create it without engine timestamps\n", path);
    printf("# then talk to the file itself:  ./backlog.tbl   (run bare for its help)\n");
    printf("# (c) Giuseppe Federico — MIT license\n");
}

/* ---------- commands ---------- */

static void cmd_init(const char *self, int argc, char **argv) {
    if (argc < 1)
        die(2, "# error: init needs a target. usage: %s init ./name.tbl [no-ts]", self);
    const char *target = argv[0];
    uint16_t flags = 0;
    if (argc >= 2) {
        if (strcmp(argv[1], "no-ts") == 0) flags |= FLAG_NOTS;
        else die(2, "# error: unknown init option '%s'. only: no-ts", argv[1]);
    }
    struct stat st;
    if (stat(target, &st) == 0)
        die(2, "# error: %s already exists — refusing to overwrite. pick another name", target);

    int self_fd = open("/proc/self/exe", O_RDONLY);
    if (self_fd < 0) die(5, "# error: cannot open own engine");
    off_t esz = elf_size_fd(self_fd);
    struct stat sst;
    fstat(self_fd, &sst);
    if (esz < 0 || esz > sst.st_size) die(5, "# error: cannot measure own engine size");
    /* if we are a table, sanity-check that a header sits right after the engine */
    if (sst.st_size > esz) {
        unsigned char hb[HDR_SIZE];
        Hdr h;
        if (xpread(self_fd, hb, HDR_SIZE, esz) != HDR_SIZE || hdr_unpack(hb, &h) != 0)
            die(5, "# error: cannot locate own engine boundary — refusing to spawn");
    }

    int fd = open(target, O_CREAT | O_EXCL | O_WRONLY, 0755);
    if (fd < 0) die(2, "# error: cannot create %s: %s", target, strerror(errno));
    char buf[65536];
    off_t off = 0;
    while (off < esz) {
        size_t want = (size_t)(esz - off) < sizeof buf ? (size_t)(esz - off) : sizeof buf;
        ssize_t r = xpread(self_fd, buf, want, off);
        if (r <= 0) die(5, "# error: engine read failed");
        if (xwrite(fd, buf, (size_t)r) != 0) die(5, "# error: write failed");
        off += r;
    }
    close(self_fd);
    Hdr h = {FMT_VERSION, flags, 0, 0, 0, crc32_buf("", 0)};
    unsigned char hb[HDR_SIZE];
    hdr_pack(&h, hb);
    if (xwrite(fd, hb, HDR_SIZE) != 0) die(5, "# error: write failed");
    if (fdatasync(fd) != 0) die(5, "# error: fdatasync failed");
    close(fd);
    printf("# created %s (%s) — now talk to the file itself: %s\n",
           target, (flags & FLAG_NOTS) ? "no-ts" : "timestamps on", target);
    printf("# run it bare for help: %s\n", target);
}

static void cmd_a(Tab *t, int argc, char **argv) {
    if (argc < 1)
        die(2, "# error: a needs at least one field. example: a title='fix lock' status=open");
    take_lock(t->path);
    tab_load(t); /* fresh state under lock */
    Rec r;
    memset(&r, 0, sizeof r);
    const char *by = NULL;
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "by=", 3) == 0) { by = argv[i] + 3; continue; }
        char *k, *v;
        parse_assign(argv[i], &k, &v);
        if (rec_get(&r, k)) die(2, "# error: field '%s' given twice", k);
        rec_set(&r, k, v);
        free(k);
        free(v);
    }
    if (r.nf == 0) die(2, "# error: a needs at least one field besides by=");
    r.id = ++t->h.last_id;
    stamp(&r, 1, by, t->h.flags & FLAG_NOTS);
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->recs = realloc(t->recs, (size_t)t->cap * sizeof(Rec));
        if (!t->recs) die(5, "# error: out of memory");
    }
    t->recs[t->n++] = r;
    tab_write(t);
    rec_print(&r, 1);
}

static void cmd_q(Tab *t, int argc, char **argv) {
    Term terms[64];
    int nt = 0, count_only = 0, show_ts = 0;
    long limit = DEF_LIMIT;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "count") == 0) { count_only = 1; continue; }
        if (strcmp(argv[i], "ts") == 0) { show_ts = 1; continue; }
        if (strncmp(argv[i], "limit=", 6) == 0) {
            char *end;
            limit = strtol(argv[i] + 6, &end, 10);
            if (*end || limit < 0) die(2, "# error: limit wants a number. example: limit=200 (0 = no cap)");
            continue;
        }
        if (strncmp(argv[i], "by=", 3) == 0)
            die(2, "# error: by= belongs to writes (a/s/next), not to q");
        if (nt >= 64) die(2, "# error: too many filters (max 64)");
        if (parse_term(argv[i], &terms[nt]) != 0)
            die(2, "# error: bad filter '%s'. filters look like: status=open prio>1 title~lock",
                argv[i]);
        nt++;
    }
    int matched = 0, printed = 0;
    for (int i = 0; i < t->n; i++) {
        int ok = 1;
        for (int j = 0; j < nt && ok; j++) ok = term_match(&t->recs[i], &terms[j]);
        if (!ok) continue;
        matched++;
        if (!count_only && (limit == 0 || printed < limit)) {
            rec_print(&t->recs[i], show_ts);
            printed++;
        }
    }
    if (count_only) { printf("# %d record%s\n", matched, matched == 1 ? "" : "s"); return; }
    if (matched == 0) { printf("# 0 records%s\n", nt ? " match" : ""); return; }
    if (matched > printed)
        printf("# showing %d of %d records — refine filters or add limit=N (0 = no cap)\n",
               printed, matched);
    else
        printf("# %d record%s\n", matched, matched == 1 ? "" : "s");
}

static void cmd_g(Tab *t, int argc, char **argv) {
    uint64_t id = parse_id_arg("g", argc >= 1 ? argv[0] : NULL);
    int i = find_rec(t, id);
    if (i < 0) die_not_found(t, id);
    rec_print(&t->recs[i], 1);
}

static void cmd_s(Tab *t, int argc, char **argv) {
    uint64_t id = parse_id_arg("s", argc >= 1 ? argv[0] : NULL);
    take_lock(t->path);
    tab_load(t);
    int idx = find_rec(t, id);
    if (idx < 0) die_not_found(t, id);
    Rec *r = &t->recs[idx];

    KV sets[MAX_FIELDS];
    int ns = 0;
    Term conds[16];
    int nc = 0;
    const char *by = NULL;
    int in_if = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "if") == 0) { in_if = 1; continue; }
        if (strncmp(argv[i], "by=", 3) == 0) { by = argv[i] + 3; continue; }
        if (in_if) {
            if (nc >= 16) die(2, "# error: too many if conditions (max 16)");
            if (parse_term(argv[i], &conds[nc]) != 0)
                die(2, "# error: bad condition '%s'. example: s 7 status=done if status=open",
                    argv[i]);
            nc++;
        } else {
            if (ns >= MAX_FIELDS) die(2, "# error: too many assignments");
            parse_assign(argv[i], &sets[ns].k, &sets[ns].v);
            ns++;
        }
    }
    if (ns == 0)
        die(2, "# error: s changes fields. usage: s %" PRIu64 " field=value [if field=value] [by=name]",
            id);
    for (int i = 0; i < nc; i++) {
        if (!term_match(r, &conds[i])) {
            const char *cur = rec_get(r, conds[i].k);
            printf("# condition failed: %s is %s%s%s — your view of this record is stale\n",
                   conds[i].k, cur ? "'" : "", cur ? cur : "absent", cur ? "'" : "");
            printf("# reload it first: g %" PRIu64 "\n", id);
            exit(4);
        }
    }
    for (int i = 0; i < ns; i++) rec_set(r, sets[i].k, sets[i].v);
    stamp(r, 0, by, t->h.flags & FLAG_NOTS);
    tab_write(t);
    rec_print(r, 1);
}

static void cmd_d(Tab *t, int argc, char **argv) {
    uint64_t id = parse_id_arg("d", argc >= 1 ? argv[0] : NULL);
    take_lock(t->path);
    tab_load(t);
    int idx = find_rec(t, id);
    if (idx < 0) die_not_found(t, id);
    printf("# deleted: ");
    rec_print(&t->recs[idx], 1);
    memmove(&t->recs[idx], &t->recs[idx + 1], (size_t)(t->n - idx - 1) * sizeof(Rec));
    t->n--;
    tab_write(t);
}

static void cmd_i(Tab *t) {
    /* field histogram */
    char *names[256];
    int counts[256], nn = 0;
    for (int i = 0; i < t->n; i++)
        for (int j = 0; j < t->recs[i].nf; j++) {
            const char *k = t->recs[i].f[j].k;
            int found = 0;
            for (int x = 0; x < nn; x++)
                if (strcmp(names[x], k) == 0) { counts[x]++; found = 1; break; }
            if (!found && nn < 256) { names[nn] = t->recs[i].f[j].k; counts[nn] = 1; nn++; }
        }
    SB b = {0};
    sb_fmt(&b, "# %d records, last id=%" PRIu64 ", %" PRIu64 " bytes data, %s",
           t->n, t->h.last_id, t->h.data_len,
           (t->h.flags & FLAG_NOTS) ? "no-ts" : "timestamps on");
    if (nn) {
        sb_puts(&b, ", fields:");
        for (int x = 0; x < nn; x++) sb_fmt(&b, " %s(%d)", names[x], counts[x]);
    }
    puts(b.s);
    free(b.s);
}

static void cmd_next(Tab *t, int argc, char **argv) {
    Term terms[64];
    int nt = 0;
    KV sets[MAX_FIELDS];
    int ns = 0;
    const char *by = NULL;
    int in_set = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "set") == 0) { in_set = 1; continue; }
        if (strncmp(argv[i], "by=", 3) == 0) { by = argv[i] + 3; continue; }
        if (!in_set) {
            if (nt >= 64) die(2, "# error: too many filters");
            if (parse_term(argv[i], &terms[nt]) != 0)
                die(2, "# error: bad filter '%s'. usage: next status=open set status=claimed by=me",
                    argv[i]);
            nt++;
        } else {
            if (ns >= MAX_FIELDS) die(2, "# error: too many assignments");
            parse_assign(argv[i], &sets[ns].k, &sets[ns].v);
            ns++;
        }
    }
    if (ns == 0)
        die(2, "# error: next takes work atomically, so it must mark it. usage: "
               "next <filters> set field=value [by=me]");
    take_lock(t->path);
    tab_load(t);
    for (int i = 0; i < t->n; i++) {
        int ok = 1;
        for (int j = 0; j < nt && ok; j++) ok = term_match(&t->recs[i], &terms[j]);
        if (!ok) continue;
        Rec *r = &t->recs[i];
        for (int x = 0; x < ns; x++) rec_set(r, sets[x].k, sets[x].v);
        stamp(r, 0, by, t->h.flags & FLAG_NOTS);
        tab_write(t);
        rec_print(r, 1);
        return;
    }
    die(1, "# 0 records match — queue empty");
}

static void cmd_cursor(Tab *t) {
    printf("# cursor=%" PRIu64 "-%u-%08x-%ld\n", t->h.last_id, t->h.count, t->h.crc,
           (long)time(NULL));
    printf("# later: diff %" PRIu64 "-%u-%08x-%ld   -> only what changed since now\n",
           t->h.last_id, t->h.count, t->h.crc, (long)time(NULL));
}

static void cmd_diff(Tab *t, int argc, char **argv) {
    if (argc < 1)
        die(2, "# error: diff needs a cursor. get one first: cursor");
    const char *tok = argv[0];
    if (strncmp(tok, "cursor=", 7) == 0) tok += 7;
    uint64_t c_last;
    unsigned c_count;
    unsigned c_crc;
    long c_epoch;
    if (sscanf(tok, "%" SCNu64 "-%u-%x-%ld", &c_last, &c_count, &c_crc, &c_epoch) != 4)
        die(2, "# error: '%s' is not a cursor. a cursor looks like 57-42-a3f2c1b0-1784301125 "
               "(from the cursor command)", argv[0]);
    int show_ts = argc >= 2 && strcmp(argv[1], "ts") == 0;
    if (c_last > t->h.last_id)
        die(2, "# error: cursor is ahead of this table (last id %" PRIu64 " vs cursor %" PRIu64
               ") — cursor from another table?", t->h.last_id, c_last);
    if (c_last == t->h.last_id && c_count == t->h.count && c_crc == t->h.crc) {
        printf("# no changes since cursor\n");
        return;
    }
    int nnew = 0, nmod = 0, survivors = 0;
    int no_ts = (t->h.flags & FLAG_NOTS) != 0;
    for (int i = 0; i < t->n; i++) {
        Rec *r = &t->recs[i];
        if (r->id <= c_last) survivors++;
        if (r->id > c_last) {
            rec_print(r, show_ts);
            nnew++;
        } else if (!no_ts) {
            const char *u = rec_get(r, "updated_at");
            if (u) {
                long e = iso_epoch(u);
                if (e >= 0 && e >= c_epoch) {
                    rec_print(r, show_ts);
                    nmod++;
                }
            }
        }
    }
    int ndel = (int)c_count - survivors;
    if (ndel < 0) ndel = 0;
    printf("# %d new, %d modified, %d deleted since cursor%s\n", nnew, nmod, ndel,
           no_ts ? " (no-ts table: modified records not tracked)" : "");
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    crc_init();
    Tab t;
    memset(&t, 0, sizeof t);
    self_table_path(t.path);

    const char *cmd = argc >= 2 ? argv[1] : NULL;

    if (cmd && strcmp(cmd, "init") == 0) {
        cmd_init(base_name(t.path), argc - 2, argv + 2);
        return 0;
    }

    tab_load(&t);

    if (t.is_seed) {
        if (!cmd) { help_seed(base_name(t.path)); return 0; }
        die(2, "# error: this is a bare seed (engine, no data). it only creates tables:\n"
               "#   %s init ./name.tbl [no-ts]", base_name(t.path));
    }
    if (!cmd || strcmp(cmd, "help") == 0) { help_table(&t); return 0; }

    if (strcmp(cmd, "a") == 0) cmd_a(&t, argc - 2, argv + 2);
    else if (strcmp(cmd, "q") == 0) cmd_q(&t, argc - 2, argv + 2);
    else if (strcmp(cmd, "g") == 0) cmd_g(&t, argc - 2, argv + 2);
    else if (strcmp(cmd, "s") == 0) cmd_s(&t, argc - 2, argv + 2);
    else if (strcmp(cmd, "d") == 0) cmd_d(&t, argc - 2, argv + 2);
    else if (strcmp(cmd, "i") == 0) cmd_i(&t);
    else if (strcmp(cmd, "next") == 0) cmd_next(&t, argc - 2, argv + 2);
    else if (strcmp(cmd, "cursor") == 0) cmd_cursor(&t);
    else if (strcmp(cmd, "diff") == 0) cmd_diff(&t, argc - 2, argv + 2);
    else
        die(2, "# error: unknown command '%s'. commands: a q g s d i next cursor diff "
               "(run with no args for help)", cmd);
    return 0;
}
