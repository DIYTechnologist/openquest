/*
 * mempeek — read-only inspector for another process's memory, from /proc/<pid>/mem.
 * No ptrace stop: opening /proc/pid/mem only needs PTRACE_MODE_READ (root + SELinux permissive).
 *
 * Usage:
 *   mempeek <pid> maps
 *   mempeek <pid> dump <addr> <len>
 *   mempeek <pid> read <addr> <nwords>            # print nwords 8-byte LE integers
 *   mempeek <pid> scan <hexbytes> [maxhits]      # scan all readable regions for pattern
 *   mempeek <pid> scanptr <value> [maxhits]      # scan for 8-byte LE pointer value
 *
 * Regions are listed with size; scan prints hit address + owning region + 32-byte context.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define MAXREG 4096

typedef struct {
    uint64_t start, end;
    char perms[5];
    char name[256];
} Region;

static int g_memfd = -1;
static Region g_regs[MAXREG];
static int g_nreg = 0;

static void die(const char *m) { perror(m); exit(1); }

static Region *find_reg(uint64_t a) {
    for (int i = 0; i < g_nreg; i++)
        if (a >= g_regs[i].start && a < g_regs[i].end) return &g_regs[i];
    return NULL;
}

static void load_maps(const char *pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) die("open maps");
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        uint64_t s, e; char perms[8]; uint64_t off; unsigned long devmaj, devmin; unsigned long ino;  // %lu writes 8 bytes on LP64 --
                                                          // an `unsigned` here corrupted the stack
        char name[512] = {0};
        int n = sscanf(line, "%lx-%lx %4s %lx %lx:%lx %lu %511[^\n]",
                       &s, &e, perms, &off, &devmaj, &devmin, &ino, name);
        if (n < 7) continue;
        if (n == 8) { while (name[0]==' '||name[0]=='\t') memmove(name, name+1, strlen(name)); }
        Region *r = &g_regs[g_nreg++];
        r->start = s; r->end = e;
        strncpy(r->perms, perms, 4); r->perms[4] = 0;
        strncpy(r->name, name, sizeof r->name - 1);
    }
    fclose(f);
}

static void open_mem(const char *pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/mem", pid);
    g_memfd = open(path, O_RDONLY);
    if (g_memfd < 0) die("open /proc/pid/mem (need root + selinux permissive?)");
}

static void hexdump(void *p, size_t n) {
    unsigned char *b = p;
    for (size_t i = 0; i < n; i += 16) {
        printf("%04zx  ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) printf("%02x ", b[i+j]); else printf("   ");
            if (j == 7) printf(" ");
        }
        printf("\n");
    }
}

static uint64_t parse_u64(const char *s) { return strtoull(s, NULL, 0); }

static int parse_hexbytes(const char *s, unsigned char *out, size_t cap) {
    size_t n = 0;
    const char *p = s;
    while (*p && n < cap) {
        while (*p == ' ' || *p == ':') p++;
        if (!*p) break;
        char h[3] = {p[0], p[1], 0};
        char *end;
        unsigned long v = strtoul(h, &end, 16);
        if (end == h) return -1;
        out[n++] = (unsigned char)v;
        p = end;
    }
    return (int)n;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: mempeek <pid> <maps|dump|read|scan|scanptr> ...\n"); return 2; }
    const char *pid = argv[1];
    const char *cmd = argv[2];
    load_maps(pid);
    open_mem(pid);

    if (!strcmp(cmd, "maps")) {
        for (int i = 0; i < g_nreg; i++) {
            uint64_t sz = g_regs[i].end - g_regs[i].start;
            if (sz >= 0x100000)
                printf("%lx-%lx %s %8.1fMB %s\n", g_regs[i].start, g_regs[i].end,
                       g_regs[i].perms, sz / (1048576.0), g_regs[i].name);
            else
                printf("%lx-%lx %s %10llu B %s\n", g_regs[i].start, g_regs[i].end,
                       g_regs[i].perms, (unsigned long long)sz, g_regs[i].name);
        }
        return 0;
    }

    if (!strcmp(cmd, "dump")) {
        uint64_t a = parse_u64(argv[3]);
        size_t n = (size_t)parse_u64(argv[4]);
        unsigned char *buf = malloc(n);
        ssize_t r = pread(g_memfd, buf, n, (off_t)a);
        if (r != (ssize_t)n) { fprintf(stderr, "short read at %lx: %zd (%s)\n", a, r, r<0?strerror(errno):"EOF"); return 1; }
        hexdump(buf, n);
        return 0;
    }

    if (!strcmp(cmd, "read")) {
        uint64_t a = parse_u64(argv[3]);
        long nw = atol(argv[4]);
        uint64_t *buf = malloc(8 * (size_t)nw);
        ssize_t r = pread(g_memfd, buf, 8 * (size_t)nw, (off_t)a);
        if (r != 8 * (size_t)nw) { fprintf(stderr, "short read: %zd\n", r); return 1; }
        for (long i = 0; i < nw; i++)
            printf("%016lx  0x%016lx  %lld\n", a + 8*i, buf[i], (long long)buf[i]);
        return 0;
    }

    if (!strcmp(cmd, "scan") || !strcmp(cmd, "scanptr")) {
        unsigned char pat[64];
        int plen;
        long maxhits = 100;
        if (!strcmp(cmd, "scanptr")) {
            uint64_t v = parse_u64(argv[3]);
            for (int i = 0; i < 8; i++) pat[i] = (v >> (8*i)) & 0xff;
            plen = 8;
            if (argc > 4) maxhits = atol(argv[4]);
        } else {
            plen = parse_hexbytes(argv[3], pat, sizeof pat);
            if (plen <= 0) { fprintf(stderr, "bad pattern\n"); return 2; }
            if (argc > 4) maxhits = atol(argv[4]);
        }
        long hits = 0;
        for (int i = 0; i < g_nreg && hits < maxhits; i++) {
            if (g_regs[i].perms[1] != 'r') continue;
            uint64_t sz = g_regs[i].end - g_regs[i].start;
            if (sz > (uint64_t)1 << 30) continue;
            size_t chunk = 65536;
            unsigned char *buf = malloc(chunk + (size_t)plen - 1);
            uint64_t pos = g_regs[i].start;
            while (pos < g_regs[i].end && hits < maxhits) {
                size_t want = (size_t)(g_regs[i].end - pos);
                if (want > chunk + (size_t)plen - 1) want = chunk + (size_t)plen - 1;
                ssize_t r = pread(g_memfd, buf, want, (off_t)pos);
                if (r <= (ssize_t)(plen - 1)) break;
                for (ssize_t off = 0; off + plen <= r; off++) {
                    if (memcmp(buf + off, pat, plen) == 0) {
                        uint64_t hit = pos + (uint64_t)off;
                        printf("HIT %016lx  [%s %s]\n", hit, g_regs[i].perms, g_regs[i].name);
                        uint64_t cstart = hit & ~7ULL;
                        unsigned char ctx[40];
                        ssize_t rc = pread(g_memfd, ctx, 32, (off_t)cstart);
                        if (rc == 32) {
                            printf("  ctx@%016lx: ", cstart);
                            for (int j = 0; j < 32; j++) printf("%02x", ctx[j]);
                            printf("\n");
                        }
                        hits++;
                        if (hits >= maxhits) break;
                    }
                }
                pos += (uint64_t)(r - (plen - 1)); /* overlap by plen-1 for boundary matches */
            }
            free(buf);
        }
        printf("--- %ld hits (max %ld)\n", hits, maxhits);
        return 0;
    }

    fprintf(stderr, "unknown cmd %s\n", cmd);
    return 2;
}
