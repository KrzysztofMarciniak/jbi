/* jbi - just build it: library implementation (C99 + POSIX threads) */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "jbi.h"

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef JBI_SHAREDIR
#define JBI_SHAREDIR "/usr/local/share/jbi"
#endif
#define MAXDOCS 64
#define MAXDOCSIZE 1000000L

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------- helpers ---------- */

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("jbi: out of memory\n", stderr); exit(1); }
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xrealloc(NULL, n);
    memcpy(p, s, n);
    return p;
}

static void list_add(jbi_list *l, const char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->v = xrealloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++] = xstrdup(s);
}

static void list_free(jbi_list *l)
{
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

static int ci_has(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++)
        if (strncasecmp(s, needle, n) == 0) return 1;
    return 0;
}

const char *jbi_sharedir(void)
{
    const char *e = getenv("JBI_SHAREDIR");
    return (e && *e) ? e : JBI_SHAREDIR;
}

/* ---------- config loading ---------- */

static int has_cfg(const jbi_ctx *c, const char *name)
{
    for (int i = 0; i < c->ncfg; i++)
        if (strcmp(c->cfg[i].name, name) == 0) return 1;
    return 0;
}

static void cfg_free(jbi_cfg *g)
{
    list_free(&g->files);
    list_free(&g->build);
    list_free(&g->install);
    list_free(&g->uninstall);
}

static void load_file(jbi_ctx *c, const char *path, const char *stem)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    jbi_cfg g;
    char line[1024];
    memset(&g, 0, sizeof g);
    snprintf(g.name, sizeof g.name, "%s", stem);
    g.order = 100;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!*v) continue;
        if (!strcmp(k, "files")) {
            char *save = NULL;
            for (char *t = strtok_r(v, " \t", &save); t; t = strtok_r(NULL, " \t", &save))
                list_add(&g.files, t);
        } else if (!strcmp(k, "order")) g.order = atoi(v);
        else if (!strcmp(k, "build")) list_add(&g.build, v);
        else if (!strcmp(k, "install")) list_add(&g.install, v);
        else if (!strcmp(k, "uninstall")) list_add(&g.uninstall, v);
    }
    fclose(f);
    if (!g.files.n || !g.build.n) { cfg_free(&g); return; }
    c->cfg = xrealloc(c->cfg, (size_t)(c->ncfg + 1) * sizeof *c->cfg);
    c->cfg[c->ncfg++] = g;
}

static void load_dir(jbi_ctx *c, const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        char stem[64], path[8192];
        if (n <= 4 || strcmp(e->d_name + n - 4, ".jbi") != 0) continue;
        if (n - 4 >= sizeof stem) continue;
        memcpy(stem, e->d_name, n - 4);
        stem[n - 4] = 0;
        if (has_cfg(c, stem)) continue;          /* project config overrides system */
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        load_file(c, path, stem);
    }
    closedir(d);
}

jbi_ctx *jbi_open(const char *dir, int dry)
{
    char rp[PATH_MAX], loc[PATH_MAX + 8];
    struct stat st;
    jbi_ctx *c;
    if (!realpath(dir, rp) || stat(rp, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
    c = xrealloc(NULL, sizeof *c);
    memset(c, 0, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%s", rp);
    c->dry = dry;
    snprintf(loc, sizeof loc, "%s/.jbi", c->dir);
    load_dir(c, loc);
    load_dir(c, jbi_sharedir());
    return c;
}

void jbi_close(jbi_ctx *c)
{
    if (!c) return;
    for (int i = 0; i < c->ncfg; i++) cfg_free(&c->cfg[i]);
    free(c->cfg);
    free(c);
}

/* ---------- threaded detection ---------- */

typedef struct { jbi_cfg *g; char **names; int n; } cthr;
typedef struct { jbi_ctx *c; char *path; } dthr;

static void *cfg_worker(void *p)
{
    cthr *t = p;
    for (int i = 0; i < t->g->files.n && !t->g->hit; i++)
        for (int j = 0; j < t->n; j++)
            if (strcasecmp(t->g->files.v[i], t->names[j]) == 0) { t->g->hit = 1; break; }
    return NULL;
}

static int cmd_matches(const char *line, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strcmp(line, cmd) == 0) return 1;
    /* allow trailing flags: "make -j4" confirms "make" */
    return strncmp(line, cmd, n) == 0 && line[n] == ' ' && line[n + 1] == '-';
}

static void *doc_worker(void *p)
{
    dthr *t = p;
    FILE *f = fopen(t->path, "r");
    char line[1024];
    if (!f) return NULL;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        for (;;) {
            while (isspace((unsigned char)*s)) s++;
            if (*s == '$' || *s == '>' || *s == '%' || *s == '`') s++;
            else break;
        }
        size_t n = strlen(s);
        while (n && (isspace((unsigned char)s[n - 1]) || s[n - 1] == '`')) s[--n] = 0;
        if (!n || *s == '#') continue;
        for (int i = 0; i < t->c->ncfg; i++) {
            jbi_cfg *g = &t->c->cfg[i];
            for (int j = 0; j < g->build.n; j++)
                if (cmd_matches(s, g->build.v[j])) {
                    pthread_mutex_lock(&mu);
                    g->doc = 1;
                    pthread_mutex_unlock(&mu);
                }
        }
    }
    fclose(f);
    return NULL;
}

static int doc_ok(const char *n)
{
    const char *dot = strrchr(n, '.');
    if (n[0] == '.' || strcasecmp(n, "CMakeLists.txt") == 0) return 0;
    if (dot) return !strcmp(dot, ".md") || !strcmp(dot, ".txt");
    return ci_has(n, "install") || ci_has(n, "build") || ci_has(n, "readme") ||
           ci_has(n, "hacking") || ci_has(n, "compile");
}

static int read_root(const char *dir, char ***out)
{
    DIR *d = opendir(dir);
    char **v = NULL;
    int n = 0, cap = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (n == cap) { cap = cap ? cap * 2 : 32; v = xrealloc(v, (size_t)cap * sizeof *v); }
            v[n++] = xstrdup(e->d_name);
        }
        closedir(d);
    }
    *out = v;
    return n;
}

static void spawn(pthread_t *t, int *ok, void *(*fn)(void *), void *arg)
{
    if (pthread_create(t, NULL, fn, arg) == 0) *ok = 1;
    else { *ok = 0; fn(arg); }           /* no thread? run inline */
}

int jbi_detect(jbi_ctx *c)
{
    int hits = 0;
    if (!c->detected) {
        char **names;
        int nn = read_root(c->dir, &names), nc = c->ncfg, nd = 0;
        cthr *ct = xrealloc(NULL, (size_t)nc * sizeof *ct);
        pthread_t *tc = xrealloc(NULL, (size_t)nc * sizeof *tc);
        int *okc = xrealloc(NULL, (size_t)nc * sizeof *okc);
        dthr dt[MAXDOCS];
        pthread_t td[MAXDOCS];
        int okd[MAXDOCS];

        c->detected = 1;
        for (int i = 0; i < nn && nd < MAXDOCS; i++) {
            char path[8192];
            struct stat st;
            if (!doc_ok(names[i])) continue;
            snprintf(path, sizeof path, "%s/%s", c->dir, names[i]);
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > MAXDOCSIZE) continue;
            dt[nd].c = c;
            dt[nd].path = xstrdup(path);
            nd++;
        }
        for (int i = 0; i < nc; i++) {
            ct[i].g = &c->cfg[i]; ct[i].names = names; ct[i].n = nn;
            spawn(&tc[i], &okc[i], cfg_worker, &ct[i]);
        }
        for (int i = 0; i < nd; i++) spawn(&td[i], &okd[i], doc_worker, &dt[i]);
        for (int i = 0; i < nc; i++) if (okc[i]) pthread_join(tc[i], NULL);
        for (int i = 0; i < nd; i++) if (okd[i]) pthread_join(td[i], NULL);

        for (int i = 0; i < nd; i++) free(dt[i].path);
        for (int i = 0; i < nn; i++) free(names[i]);
        free(names); free(ct); free(tc); free(okc);
    }
    for (int i = 0; i < c->ncfg; i++) if (c->cfg[i].hit) hits++;
    return hits;
}

/* ---------- ranking ---------- */

static int tier(const jbi_cfg *g) { return (g->hit && g->doc) ? 0 : g->hit ? 1 : 2; }

static int before(const jbi_ctx *c, int a, int b)
{
    const jbi_cfg *x = &c->cfg[a], *y = &c->cfg[b];
    if (tier(x) != tier(y)) return tier(x) < tier(y);
    if (x->order != y->order) return x->order < y->order;
    return a < b;
}

static int *rank(const jbi_ctx *c)
{
    int *r = xrealloc(NULL, (size_t)c->ncfg * sizeof *r);
    for (int i = 0; i < c->ncfg; i++) r[i] = i;
    for (int i = 1; i < c->ncfg; i++) {      /* stable insertion sort */
        int k = r[i], j = i - 1;
        while (j >= 0 && before(c, k, r[j])) { r[j + 1] = r[j]; j--; }
        r[j + 1] = k;
    }
    return r;
}

/* ---------- running ---------- */

static int run_cmd(const jbi_ctx *c, const char *cmd)
{
    pid_t p;
    int st;
    printf("[jbi] %s\n", cmd);
    fflush(stdout);
    if (c->dry) return 1;
    p = fork();
    if (p < 0) return 0;
    if (p == 0) {
        if (chdir(c->dir) != 0) _exit(126);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    while (waitpid(p, &st, 0) < 0) {
        if (errno == EINTR) continue;
        return 0;
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void save_state(const jbi_ctx *c, const jbi_cfg *g, const char *cmd)
{
    char path[8192];
    FILE *f;
    snprintf(path, sizeof path, "%s/.jbi", c->dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/.jbi/state", c->dir);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n", g->name, cmd);
    fclose(f);
}

static void clear_state(const jbi_ctx *c)
{
    char path[8192];
    snprintf(path, sizeof path, "%s/.jbi/state", c->dir);
    if (unlink(path) != 0 && errno != ENOENT)
        fprintf(stderr, "jbi: cannot remove state file %s: %s\n", path, strerror(errno));
}

static int load_state(const jbi_ctx *c, char *name, size_t n)
{
    char path[8192];
    FILE *f;
    snprintf(path, sizeof path, "%s/.jbi/state", c->dir);
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(name, (int)n, f)) { fclose(f); return 0; }
    fclose(f);
    name[strcspn(name, "\r\n")] = 0;
    return name[0] != 0;
}

static int try_list(const jbi_ctx *c, const jbi_cfg *g, const jbi_list *l, int save)
{
    for (int i = 0; i < l->n; i++)
        if (run_cmd(c, l->v[i])) {
            if (save && !c->dry) save_state(c, g, l->v[i]);
            return 1;
        }
    return 0;
}

int jbi_build(jbi_ctx *c)
{
    int rc = -1, *r;
    jbi_detect(c);
    r = rank(c);
    for (int i = 0; i < c->ncfg && rc != 0; i++) {            /* found by files / docs */
        jbi_cfg *g = &c->cfg[r[i]];
        if (tier(g) > 1) break;
        printf("[jbi] %s%s\n", g->name, g->doc ? " (files + docs)" : " (files)");
        if (try_list(c, g, &g->build, 1)) rc = 0;
    }
    if (rc != 0) {                                            /* brute force */
        puts("[jbi] nothing detected worked, brute force");
        for (int i = 0; i < c->ncfg && rc != 0; i++) {
            jbi_cfg *g = &c->cfg[r[i]];
            if (tier(g) <= 1) continue;
            printf("[jbi] brute: %s\n", g->name);
            if (try_list(c, g, &g->build, 1)) rc = 0;
        }
    }
    free(r);
    return rc;
}

static int phase(jbi_ctx *c, int uninstall)
{
    const char *what = uninstall ? "uninstall" : "install";
    char st[64];
    int rc = -1, *r;
    jbi_detect(c);
    r = rank(c);
    if (load_state(c, st, sizeof st)) {                       /* built by jbi: use that system only */
        for (int i = 0; i < c->ncfg; i++) {
            jbi_cfg *g = &c->cfg[i];
            const jbi_list *l = uninstall ? &g->uninstall : &g->install;
            if (strcmp(g->name, st) != 0) continue;
            if (!l->n) fprintf(stderr, "jbi: %s has no %s command\n", g->name, what);
            else {
                printf("[jbi] %s\n", g->name);
                rc = try_list(c, g, l, 0) ? 0 : -1;
                if (rc == 0 && uninstall && !c->dry) clear_state(c);
            }
            free(r);
            return rc;
        }
    }
    for (int i = 0; i < c->ncfg && rc != 0; i++) {            /* otherwise: detected systems */
        jbi_cfg *g = &c->cfg[r[i]];
        const jbi_list *l = uninstall ? &g->uninstall : &g->install;
        if (tier(g) > 1) break;
        if (!l->n) continue;
        printf("[jbi] %s\n", g->name);
        if (try_list(c, g, l, 0)) rc = 0;
    }
    free(r);
    if (rc != 0) fprintf(stderr, "jbi: no %s command worked\n", what);
    return rc;
}

int jbi_install(jbi_ctx *c)   { return phase(c, 0); }
int jbi_uninstall(jbi_ctx *c) { return phase(c, 1); }

void jbi_list_plan(jbi_ctx *c, FILE *out)
{
    int *r;
    static const char *const label[] = { "files+docs", "files", "brute force" };
    jbi_detect(c);
    r = rank(c);
    for (int i = 0; i < c->ncfg; i++) {
        const jbi_cfg *g = &c->cfg[r[i]];
        fprintf(out, "%2d. %-10s [%s] order %d\n", i + 1, g->name, label[tier(g)], g->order);
        for (int j = 0; j < g->build.n; j++)     fprintf(out, "      build:     %s\n", g->build.v[j]);
        for (int j = 0; j < g->install.n; j++)   fprintf(out, "      install:   %s\n", g->install.v[j]);
        for (int j = 0; j < g->uninstall.n; j++) fprintf(out, "      uninstall: %s\n", g->uninstall.v[j]);
    }
    free(r);
}
