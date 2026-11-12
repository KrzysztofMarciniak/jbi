/* jbi - heuristic C/C++ autobuilder (C99 + POSIX) */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jbi.h"

#define AUTO_PATH 8192
#define AUTO_FILES 5000
#define AUTO_DEPTH 24

typedef struct {
        char* s;
        size_t n, cap;
} strbuf;

typedef struct {
        char* path;
        int cxx, mainfn;
} source;

typedef struct {
        jbi_ctx* ctx;
        source* src;
        int nsrc, csrc, nfiles, cxx;
        jbi_list headers, incdirs, sysheaders, cflags, ldflags, subdirs, artifacts;
        char probe[AUTO_PATH];
        const char* probe_cc;
        const char *probe_flags, *probe_cppflags;
} autobuild;

static void* a_realloc(void* p, size_t n) {
        void* q = realloc(p, n ? n : 1);
        if (!q) {
                fputs("jbi: out of memory\n", stderr);
                exit(1);
        }
        return q;
}

static char* a_strdup(const char* s) {
        size_t n = strlen(s) + 1;
        char* p  = a_realloc(NULL, n);
        memcpy(p, s, n);
        return p;
}

static void list_add(jbi_list* l, const char* s) {
        if (l->n == l->cap) {
                l->cap = l->cap ? l->cap * 2 : 8;
                l->v   = a_realloc(l->v, (size_t)l->cap * sizeof *l->v);
        }
        l->v[l->n++] = a_strdup(s);
}

static void list_free(jbi_list* l) {
        for (int i = 0; i < l->n; i++) free(l->v[i]);
        free(l->v);
        memset(l, 0, sizeof *l);
}

static int has(const jbi_list* l, const char* s) {
        for (int i = 0; i < l->n; i++)
                if (!strcmp(l->v[i], s)) return 1;
        return 0;
}

static void add_unique(jbi_list* l, const char* s) {
        if (*s && !has(l, s)) list_add(l, s);
}

static int path_join(char* out, size_t cap, const char* a, const char* b) {
        size_t x = strlen(a), y = strlen(b);
        if (x + y + 2 > cap) return 0;
        memcpy(out, a, x);
        out[x] = '/';
        memcpy(out + x + 1, b, y + 1);
        return 1;
}

static void cat(strbuf* b, const char* s) {
        size_t n = strlen(s);
        if (b->n + n + 1 > b->cap) {
                b->cap = (b->n + n + 1) * 2;
                b->s   = a_realloc(b->s, b->cap);
        }
        memcpy(b->s + b->n, s, n + 1);
        b->n += n;
}

static void quote(strbuf* b, const char* s) {
        cat(b, "'");
        for (; *s; s++) {
                if (*s == '\'')
                        cat(b, "'\\''");
                else {
                        char t[2] = {*s, 0};
                        cat(b, t);
                }
        }
        cat(b, "'");
}

static const char* env_or(const char* n, const char* d) {
        const char* v = getenv(n);
        return v && *v ? v : d;
}

static int run(const jbi_ctx* c, const char* cmd) {
        pid_t p;
        int st;
        printf("[jbi] %s\n", cmd);
        fflush(stdout);
        if (c->dry) return 1;
        p = fork();
        if (p < 0) return 0;
        if (!p) {
                if (chdir(c->dir)) _exit(126);
                execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
                _exit(127);
        }
        while (waitpid(p, &st, 0) < 0)
                if (errno != EINTR) return 0;
        return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int runbuf(const jbi_ctx* c, strbuf* b) {
        int r = run(c, b->s ? b->s : "");
        free(b->s);
        memset(b, 0, sizeof *b);
        return r;
}

static int empty_dir(const char* p) {
        DIR* d = opendir(p);
        struct dirent* e;
        int empty = 1;
        if (!d) return 1;
        while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                        empty = 0;
                        break;
                }
        }
        closedir(d);
        return empty;
}

static void read_submodules(autobuild* a) {
        char p[AUTO_PATH], line[1024];
        FILE* f;
        if (snprintf(p, sizeof p, "%s/.gitmodules", a->ctx->dir) >=
            (int)sizeof p)
                return;
        if (access(p, F_OK) != 0) {
                puts("[jbi] no .gitmodules; skipping submodule handling");
                return;
        }
        puts("[jbi] .gitmodules found");
        f = fopen(p, "r");
        if (!f) {
                fprintf(stderr, "jbi: .gitmodules exists but cannot be read\n");
                return;
        }
        while (fgets(line, sizeof line, f)) {
                char *s = line, *eq;
                while (isspace((unsigned char)*s)) s++;
                if (*s == '#' || *s == ';' || *s == '[' ||
                    !(eq = strchr(s, '=')))
                        continue;
                *eq++ = 0;
                while (*s && isspace((unsigned char)s[strlen(s) - 1]))
                        s[strlen(s) - 1] = 0;
                if (strcmp(s, "path")) continue;
                while (isspace((unsigned char)*eq)) eq++;
                {
                        size_t n = strlen(eq);
                        while (n && isspace((unsigned char)eq[n - 1]))
                                eq[--n] = 0;
                        if (n) add_unique(&a->subdirs, eq);
                }
        }
        fclose(f);
}

static int init_submodules(autobuild* a) {
        char p[AUTO_PATH];
        struct stat st;
        int need = 0;
        for (int i = 0; i < a->subdirs.n; i++) {
                snprintf(p, sizeof p, "%s/%s", a->ctx->dir, a->subdirs.v[i]);
                if (empty_dir(p)) need++;
        }
        if (!need) return 1;
        snprintf(p, sizeof p, "%s/.git", a->ctx->dir);
        if (stat(p, &st)) {
                fprintf(
                    stderr,
                    "jbi: .gitmodules found but this is not a git checkout; "
                    "skipping submodules\n");
                return 1;
        }
        printf("[jbi] %d submodule(s) not initialized\n", need);
        if (a->ctx->dry) {
                puts(
                    "[jbi] dry-run: would run git submodule update --init "
                    "--recursive");
                return 1;
        }
        if (!run(a->ctx, "git submodule update --init --recursive")) {
                fputs("jbi: submodule update failed\n", stderr);
                return 0;
        }
        return 1;
}

static int skipped(const char* s) {
        static const char* const v[] = {
            ".git",       "build", "test",         "tests", "example",
            "examples",   "doc",   "docs",         "bench", "benchmark",
            "benchmarks", "fuzz",  "node_modules", NULL};
        for (int i = 0; v[i]; i++)
                if (!strcmp(s, v[i])) return 1;
        return 0;
}

static int kind(const char* s) {
        const char* e = strrchr(s, '.');
        if (!e) return 0;
        if (!strcmp(e, ".c")) return 1;
        if (!strcmp(e, ".cc") || !strcmp(e, ".cpp") || !strcmp(e, ".cxx") ||
            !strcmp(e, ".c++") || !strcmp(e, ".C"))
                return 2;
        if (!strcmp(e, ".h") || !strcmp(e, ".hpp") || !strcmp(e, ".hh") ||
            !strcmp(e, ".hxx") || !strcmp(e, ".H"))
                return 3;
        return 0;
}

static int in_sub(const autobuild* a, const char* p) {
        for (int i = 0; i < a->subdirs.n; i++) {
                size_t n = strlen(a->subdirs.v[i]);
                if (!strncmp(p, a->subdirs.v[i], n) && p[n] == '/') return 1;
        }
        return 0;
}

static void add_source(autobuild* a, const char* p, int cxx) {
        if (a->nsrc == a->csrc) {
                a->csrc = a->csrc ? a->csrc * 2 : 32;
                a->src  = a_realloc(a->src, (size_t)a->csrc * sizeof *a->src);
        }
        a->src[a->nsrc].path   = a_strdup(p);
        a->src[a->nsrc].cxx    = cxx;
        a->src[a->nsrc].mainfn = 0;
        a->nsrc++;
}

static void walk(autobuild* a, const char* rel, int depth) {
        char dir[AUTO_PATH];
        DIR* d;
        struct dirent* e;
        if (depth > AUTO_DEPTH || a->nfiles >= AUTO_FILES) return;
        if (*rel)
                snprintf(dir, sizeof dir, "%s/%s", a->ctx->dir, rel);
        else
                snprintf(dir, sizeof dir, "%s", a->ctx->dir);
        d = opendir(dir);
        if (!d) return;
        while ((e = readdir(d)) && a->nfiles < AUTO_FILES) {
                char r[AUTO_PATH], f[AUTO_PATH];
                struct stat st;
                int k;
                if (e->d_name[0] == '.') continue;
                if (*rel)
                        snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
                else
                        snprintf(r, sizeof r, "%s", e->d_name);
                if (!path_join(f, sizeof f, a->ctx->dir, r) || lstat(f, &st))
                        continue;
                if (S_ISDIR(st.st_mode)) {
                        if (!skipped(e->d_name)) walk(a, r, depth + 1);
                        continue;
                }
                if (!S_ISREG(st.st_mode) || (k = kind(e->d_name)) == 0)
                        continue;
                a->nfiles++;
                if (k == 3)
                        list_add(&a->headers, r);
                else
                        add_source(a, r, k == 2);
        }
        closedir(d);
}

static void strip_comments(const char* in, char* out, int* block) {
        size_t o = 0;
        char q   = 0; /* inside a string or character literal */
        for (size_t i = 0; in[i]; i++) {
                if (*block) {
                        if (in[i] == '*' && in[i + 1] == '/') {
                                *block = 0;
                                i++;
                        }
                        continue;
                }
                if (q) {
                        out[o++] = in[i];
                        if (in[i] == '\\' && in[i + 1])
                                out[o++] = in[++i];
                        else if (in[i] == q)
                                q = 0;
                        continue;
                }
                if (in[i] == '"' || in[i] == '\'') {
                        q        = in[i];
                        out[o++] = in[i];
                        continue;
                }
                if (in[i] == '/' && in[i + 1] == '*') {
                        *block = 1;
                        i++;
                        continue;
                }
                if (in[i] == '/' && in[i + 1] == '/') break;
                out[o++] = in[i];
        }
        out[o] = 0;
}

static int parse_include(const char* s, char* out, size_t n) {
        char end;
        size_t k = 0;
        while (isspace((unsigned char)*s)) s++;
        if (*s++ != '#') return 0;
        while (isspace((unsigned char)*s)) s++;
        if (strncmp(s, "include", 7)) return 0;
        s += 7;
        if (isalnum((unsigned char)*s) || *s == '_') return 0;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '<')
                end = '>';
        else if (*s == '"')
                end = '"';
        else
                return 0;
        s++;
        while (*s && *s != end && k + 1 < n) out[k++] = *s++;
        if (*s != end || !k) return 0;
        out[k] = 0;
        return end == '>' ? 1 : 2;
}

static int has_main_line(const char* s) {
        const char *p, *q;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '#') return 0;
        for (p = s; (p = strstr(p, "main")); p += 4) {
                if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                              p[-1] == ':'))
                        continue;
                q = p + 4;
                if (isalnum((unsigned char)*q) || *q == '_') continue;
                while (isspace((unsigned char)*q)) q++;
                if (*q == '(') {
                        const char* b = p;
                        while (b > s && isspace((unsigned char)b[-1])) b--;
                        if (b > s && (isalnum((unsigned char)b[-1]) ||
                                      b[-1] == '_' || b[-1] == '*'))
                                return 1;
                        if (b == s) { /* "int\nmain (...)": type on prior line */
                                const char* e = s + strlen(s);
                                while (e > s && isspace((unsigned char)e[-1]))
                                        e--;
                                if (e > s && e[-1] != ';') return 1;
                        }
                }
        }
        return 0;
}

static void note_include(autobuild* a, const char* from, const char* name,
                         int quoted) {
        char local[AUTO_PATH], d[AUTO_PATH];
        const char* slash = strrchr(from, '/');
        snprintf(local, sizeof local, "%.*s%s%s",
                 slash ? (int)(slash - from) : 0, from, slash ? "/" : "", name);
        if (has(&a->headers, local)) return;
        for (int i = 0; i < a->headers.n; i++) {
                const char* h = a->headers.v[i];
                size_t hn = strlen(h), nn = strlen(name);
                if (hn >= nn && !strcmp(h + hn - nn, name) &&
                    (hn == nn || h[hn - nn - 1] == '/')) {
                        if (hn == nn)
                                add_unique(&a->incdirs, ".");
                        else {
                                snprintf(d, sizeof d, "%.*s",
                                         (int)(hn - nn - 1), h);
                                add_unique(&a->incdirs, d);
                        }
                        return;
                }
        }
        if (!quoted && name[0] != '.' && name[0] != '/')
                add_unique(&a->sysheaders, name);
}

static void scan_file(autobuild* a, const char* rel, int* mainfn) {
        char fpath[AUTO_PATH], line[2048], clean[2048], inc[1024];
        int block  = 0;
        long total = 0;
        FILE* f;
        snprintf(fpath, sizeof fpath, "%s/%s", a->ctx->dir, rel);
        f = fopen(fpath, "r");
        if (!f) return;
        while (fgets(line, sizeof line, f)) {
                total += (long)strlen(line);
                if (total > 4000000L) break;
                strip_comments(line, clean, &block);
                int inck = parse_include(clean, inc, sizeof inc);
                if (inck)
                        note_include(a, rel, inc, inck == 2);
                else if (mainfn && !*mainfn && has_main_line(clean))
                        *mainfn = 1;
        }
        fclose(f);
}

static int safe_name(const char* s) {
        if (!*s) return 0;
        for (; *s; s++)
                if (!isalnum((unsigned char)*s) && !strchr("_+.-", *s))
                        return 0;
        return 1;
}

static int shell_ok(const char* s) { return system(s) == 0; }

static int try_flags(const autobuild* a, const char* flags) {
        strbuf b = {0};
        cat(&b, "cd ");
        quote(&b, a->probe);
        cat(&b, " && ");
        cat(&b, a->probe_cc);
        if (a->probe_cppflags && *a->probe_cppflags) {
                cat(&b, " ");
                cat(&b, a->probe_cppflags);
        }
        if (a->probe_flags && *a->probe_flags) {
                cat(&b, " ");
                cat(&b, a->probe_flags);
        }
        cat(&b, " probe.c -o probe.out ");
        cat(&b, flags);
        cat(&b, " >/dev/null 2>&1");
        int ok = shell_ok(b.s);
        free(b.s);
        return ok;
}

static int pkg_exists(const char* name) {
        strbuf b = {0};
        cat(&b, "pkg-config --exists ");
        quote(&b, name);
        cat(&b, " >/dev/null 2>&1");
        int ok = shell_ok(b.s);
        free(b.s);
        return ok;
}

/* Well-known platform dependencies retained from the original autobuilder.
   Unknown headers still go through the generic directory/name heuristic below.
 */
static const struct {
        const char *header, *spec;
} known_deps[] = {{"pthread.h", "-pthread"},
                  {"semaphore.h", "-pthread"},
                  {"threads.h", "-pthread"},
                  {"thread", "-pthread"},
                  {"mutex", "-pthread"},
                  {"condition_variable", "-pthread"},
                  {"future", "-pthread"},
                  {"shared_mutex", "-pthread"},
                  {"semaphore", "-pthread"},
                  {"omp.h", "-fopenmp"},
                  {"math.h", "-lm"},
                  {"complex.h", "-lm"},
                  {"tgmath.h", "-lm"},
                  {"fenv.h", "-lm"},
                  {"dlfcn.h", "-ldl"},
                  {"mqueue.h", "-lrt"},
                  {"aio.h", "-lrt"},
                  {"crypt.h", "-lcrypt"},
                  {"zlib.h", "-lz"},
                  {"bzlib.h", "-lbz2"},
                  {"lzma.h", "-llzma"},
                  {"zstd.h", "-lzstd"},
                  {"curl/", "-lcurl"},
                  {"sqlite3.h", "-lsqlite3"},
                  {"png.h", "-lpng"},
                  {"jpeglib.h", "-ljpeg"},
                  {"ncurses.h", "-lncurses"},
                  {"curses.h", "-lncurses"},
                  {"readline/", "-lreadline"},
                  {"openssl/", "-lssl -lcrypto"},
                  {"X11/", "-lX11"},
                  {"GL/gl.h", "-lGL"},
                  {"GL/glut.h", "-lglut -lGLU -lGL"},
                  {"GLFW/", "-lglfw"},
                  {"SDL2/", "pkg:sdl2"},
                  {"uuid/", "-luuid"},
                  {"ffi.h", "-lffi"},
                  {"pcre.h", "-lpcre"},
                  {"pcre2.h", "-lpcre2-8"},
                  {"yaml.h", "-lyaml"},
                  {"jansson.h", "-ljansson"},
                  {"expat.h", "-lexpat"},
                  {"event2/", "-levent"},
                  {"libxml/", "pkg:libxml-2.0"},
                  {"gmp.h", "-lgmp"},
                  {"gsl/", "-lgsl -lgslcblas -lm"},
                  {"uv.h", "-luv"},
                  {"ev.h", "-lev"},
                  {NULL, NULL}};

static int accept_spec(autobuild* a, const char* token) {
        if (!strncmp(token, "pkg:", 4)) {
                const char* name = token + 4;
                strbuf cf = {0}, lf = {0};
                if (!safe_name(name) || !pkg_exists(name)) return 0;
                cat(&cf, "$(pkg-config --cflags ");
                quote(&cf, name);
                cat(&cf, ")");
                cat(&lf, "$(pkg-config --libs ");
                quote(&lf, name);
                cat(&lf, ")");
                add_unique(&a->cflags, cf.s);
                add_unique(&a->ldflags, lf.s);
                free(cf.s);
                free(lf.s);
                return 1;
        }
        if (token[0] != '-' || !safe_name(token + 1) || !try_flags(a, token))
                return 0;
        if (strncmp(token, "-l", 2)) add_unique(&a->cflags, token);
        add_unique(&a->ldflags, token);
        return 1;
}

static int resolve_known(autobuild* a, const char* h) {
        for (int i = 0; known_deps[i].header; i++) {
                size_t n  = strlen(known_deps[i].header);
                int match = known_deps[i].header[n - 1] == '/'
                                ? !strncmp(h, known_deps[i].header, n)
                                : !strcmp(h, known_deps[i].header);
                if (match) {
                        char specs[160], *save = NULL, *t;
                        snprintf(specs, sizeof specs, "%s", known_deps[i].spec);
                        for (t = strtok_r(specs, " ", &save); t;
                             t = strtok_r(NULL, " ", &save)) {
                                if (accept_spec(a, t))
                                        printf("[jbi] auto: <%s> -> %s\n", h,
                                               t);
                                else
                                        fprintf(stderr,
                                                "jbi: auto: <%s> needs %s but "
                                                "it is unavailable\n",
                                                h, t);
                        }
                        return 1;
                }
        }
        return 0;
}

static int accept_candidate(autobuild* a, const char* name, char* resolved,
                            size_t nr) {
        char flag[256];
        strbuf cf = {0}, lf = {0};
        if (!safe_name(name)) return 0;
        if (pkg_exists(name)) {
                snprintf(resolved, nr, "pkg-config %s", name);
                cat(&cf, "$(pkg-config --cflags ");
                quote(&cf, name);
                cat(&cf, ")");
                cat(&lf, "$(pkg-config --libs ");
                quote(&lf, name);
                cat(&lf, ")");
                add_unique(&a->cflags, cf.s);
                add_unique(&a->ldflags, lf.s);
                free(cf.s);
                free(lf.s);
                return 1;
        }
        snprintf(flag, sizeof flag, "-l%s", name);
        if (!try_flags(a, flag)) return 0;
        snprintf(resolved, nr, "%s", flag);
        add_unique(&a->ldflags, flag);
        return 1;
}

static int header_exists(const autobuild* a, const char* h) {
        char p[AUTO_PATH];
        strbuf b = {0};
        FILE* f;
        int ok;
        if (!path_join(p, sizeof p, a->probe, "inc.c")) return 0;
        f = fopen(p, "w");
        if (!f) return 0;
        fprintf(f, "#include <%s>\nint main(void){return 0;}\n", h);
        fclose(f);
        cat(&b, "cd ");
        quote(&b, a->probe);
        cat(&b, " && ");
        cat(&b, a->probe_cc);
        if (a->probe_cppflags && *a->probe_cppflags) {
                cat(&b, " ");
                cat(&b, a->probe_cppflags);
        }
        if (a->probe_flags && *a->probe_flags) {
                cat(&b, " ");
                cat(&b, a->probe_flags);
        }
        cat(&b, " -fsyntax-only inc.c >/dev/null 2>&1");
        ok = shell_ok(b.s);
        free(b.s);
        return ok;
}

static void guess_dependency(autobuild* a, const char* h) {
        char dir[256], stem[256], low[256];
        const char *slash = strchr(h, '/'), *base = strrchr(h, '/'), *dot;
        jbi_list cand = {0};
        size_t n;
        base = base ? base + 1 : h;
        dot  = strchr(base, '.');
        n    = dot ? (size_t)(dot - base) : strlen(base);
        if (n >= sizeof stem) return;
        memcpy(stem, base, n);
        stem[n] = 0;
        dir[0]  = 0;
        if (slash && (size_t)(slash - h) < sizeof dir) {
                memcpy(dir, h, (size_t)(slash - h));
                dir[slash - h] = 0;
        }
        /* Directory is the strongest clue: yaml-cpp/yaml.h => -lyaml-cpp. */
        if (dir[0]) {
                snprintf(low, sizeof low, "%s", dir);
                for (char* p = low; *p; p++)
                        *p = (char)tolower((unsigned char)*p);
                add_unique(&cand, low);
        }
        snprintf(low, sizeof low, "%s", stem);
        for (char* p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
        add_unique(&cand, low);
        if (dir[0]) add_unique(&cand, dir);
        add_unique(&cand, stem);
        for (int i = 0; i < cand.n; i++) {
                char resolved[320];
                if (accept_candidate(a, cand.v[i], resolved, sizeof resolved)) {
                        printf("[jbi] auto: <%s> -> %s\n", h, resolved);
                        goto done;
                }
        }
        if (!header_exists(a, h))
                fprintf(stderr,
                        "jbi: auto: <%s> not found (install its development "
                        "package?)\n",
                        h);
        else
                printf(
                    "[jbi] auto: <%s> found, no matching library flag "
                    "(possibly "
                    "header-only)\n",
                    h);
done:
        list_free(&cand);
}

static int is_system_header(const char* h) {
        static const char* const libc[] = {
            "assert.h", "ctype.h",   "errno.h",  "float.h",  "inttypes.h",
            "limits.h", "locale.h",  "math.h",   "setjmp.h", "signal.h",
            "stdarg.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
            "stdlib.h", "string.h",  "time.h",   "unistd.h", "fcntl.h",
            "dirent.h", "sys/",      "netinet/", "arpa/",    "linux/",
            "net/",     NULL};
        for (int i = 0; libc[i]; i++) {
                size_t n = strlen(libc[i]);
                if (libc[i][n - 1] == '/' ? !strncmp(h, libc[i], n)
                                          : !strcmp(h, libc[i]))
                        return 1;
        }
        if (!strchr(h, '.')) return 1;
        return 0;
}

static void resolve_header(autobuild* a, const char* h) {
        if (resolve_known(a, h)) return;
        if (!is_system_header(h)) guess_dependency(a, h);
}

static void add_flags(const autobuild* a, strbuf* b) {
        for (int i = 0; i < a->incdirs.n; i++) {
                cat(b, " -I");
                quote(b, a->incdirs.v[i]);
        }
        for (int i = 0; i < a->cflags.n; i++) {
                cat(b, " ");
                cat(b, a->cflags.v[i]);
        }
}

static void flatten(const char* p, char* o, size_t n) {
        size_t k = 0;
        for (; *p && k + 3 < n; p++) {
                if (*p == '/') {
                        o[k++] = '_';
                        o[k++] = '_';
                } else
                        o[k++] = *p;
        }
        o[k] = 0;
}

static void add_artifact(autobuild* a, const char* kind, const char* src,
                         const char* dst) {
        strbuf b = {0};
        cat(&b, kind);
        cat(&b, "\t");
        cat(&b, src);
        if (dst) {
                cat(&b, "\t");
                cat(&b, dst);
        }
        list_add(&a->artifacts, b.s);
        free(b.s);
}

static void proj_name(const jbi_ctx* c, char* name, size_t n) {
        const char* slash = strrchr(c->dir, '/');
        snprintf(name, n, "%s", slash && slash[1] ? slash + 1 : "a.out");
}

static int compile_link(autobuild* a) {
        const char *cc = env_or("CC", "cc"), *cxx = env_or("CXX", "c++"),
                   *cf  = env_or("CFLAGS", "-O2"),
                   *xf  = env_or("CXXFLAGS", "-O2"),
                   *pp  = env_or("CPPFLAGS", "");
        const char *ldf = getenv("LDFLAGS"), *ar = env_or("AR", "ar");
        jbi_list objs = {0}, outs = {0};
        char out[AUTO_PATH], name[256];
        int mains = 0, linked = 0, rc = -1;
        strbuf b = {0};
        proj_name(a->ctx, name, sizeof name);
        if (!run(a->ctx, "mkdir -p build/obj")) goto done;
        for (int i = 0; i < a->nsrc; i++) {
                char flat[AUTO_PATH], obj[AUTO_PATH], leaf[AUTO_PATH];
                flatten(a->src[i].path, flat, sizeof flat);
                if (snprintf(leaf, sizeof leaf, "%s.o", flat) >=
                        (int)sizeof leaf ||
                    !path_join(obj, sizeof obj, "build/obj", leaf))
                        goto done;
                cat(&b, a->src[i].cxx ? cxx : cc);
                cat(&b, " ");
                cat(&b, a->src[i].cxx ? xf : cf);
                if (*pp) {
                        cat(&b, " ");
                        cat(&b, pp);
                }
                add_flags(a, &b);
                cat(&b, " -c -o ");
                quote(&b, obj);
                cat(&b, " ");
                quote(&b, a->src[i].path);
                if (!runbuf(a->ctx, &b)) {
                        fprintf(stderr, "jbi: auto: failed to compile %s\n",
                                a->src[i].path);
                        goto done;
                }
                list_add(&objs, obj);
        }
        for (int i = 0; i < a->nsrc; i++) mains += a->src[i].mainfn;
        if (!mains) {
                snprintf(out, sizeof out, "build/lib%s.a", name);
                cat(&b, ar);
                cat(&b, " rcs ");
                quote(&b, out);
                for (int i = 0; i < objs.n; i++) {
                        cat(&b, " ");
                        quote(&b, objs.v[i]);
                }
                if (!runbuf(a->ctx, &b)) goto done;
                printf("[jbi] auto: no main() found, built %s\n", out);
                add_artifact(a, "lib", out, NULL);
                rc = 0;
                goto done;
        }
        for (int m = 0; m < a->nsrc; m++)
                if (a->src[m].mainfn) {
                        if (mains == 1)
                                snprintf(out, sizeof out, "build/%s", name);
                        else {
                                const char* base = strrchr(a->src[m].path, '/');
                                const char* dot;
                                base = base ? base + 1 : a->src[m].path;
                                dot  = strrchr(base, '.');
                                {
                                        size_t n = dot ? (size_t)(dot - base)
                                                       : strlen(base);
                                        if (n > sizeof out - 7)
                                                n = sizeof out - 7;
                                        memcpy(out, "build/", 6);
                                        memcpy(out + 6, base, n);
                                        out[6 + n] = 0;
                                }
                                if (has(&outs, out)) { /* same basename twice */
                                        char flat[AUTO_PATH - 16];
                                        flatten(a->src[m].path, flat,
                                                sizeof flat);
                                        snprintf(out, sizeof out, "build/%s",
                                                 flat);
                                }
                        }
                        list_add(&outs, out);
                        cat(&b, a->cxx ? cxx : cc);
                        cat(&b, " -o ");
                        quote(&b, out);
                        for (int i = 0; i < objs.n; i++)
                                if (i == m || !a->src[i].mainfn) {
                                        cat(&b, " ");
                                        quote(&b, objs.v[i]);
                                }
                        for (int i = 0; i < a->ldflags.n; i++) {
                                cat(&b, " ");
                                cat(&b, a->ldflags.v[i]);
                        }
                        if (ldf && *ldf) {
                                cat(&b, " ");
                                cat(&b, ldf);
                        }
                        if (!runbuf(a->ctx, &b)) {
                                fprintf(stderr,
                                        "jbi: auto: link failed for %s\n", out);
                                continue;
                        }
                        printf("[jbi] auto: built %s\n", out);
                        add_artifact(a, "bin", out, NULL);
                        linked++;
                }
        if (linked) rc = 0;
done:
        free(b.s);
        list_free(&objs);
        list_free(&outs);
        return rc;
}

static void cleanup(autobuild* a) {
        for (int i = 0; i < a->nsrc; i++) free(a->src[i].path);
        free(a->src);
        list_free(&a->headers);
        list_free(&a->incdirs);
        list_free(&a->sysheaders);
        list_free(&a->cflags);
        list_free(&a->ldflags);
        list_free(&a->subdirs);
        list_free(&a->artifacts);
        if (a->probe[0]) {
                strbuf b = {0};
                cat(&b, "rm -rf ");
                quote(&b, a->probe);
                (void)shell_ok(b.s);
                free(b.s);
        }
}


static int write_manifest(const jbi_ctx* c, const char* file,
                          const jbi_list* l) {
        char p[AUTO_PATH];
        FILE* f;
        snprintf(p, sizeof p, "%s/.jbi", c->dir);
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/.jbi/%s", c->dir, file);
        f = fopen(p, "w");
        if (!f) return 0;
        for (int i = 0; i < l->n; i++) fprintf(f, "%s\n", l->v[i]);
        return fclose(f) == 0;
}

static int read_manifest(const jbi_ctx* c, const char* file, jbi_list* l) {
        char p[AUTO_PATH], line[2 * AUTO_PATH];
        FILE* f;
        snprintf(p, sizeof p, "%s/.jbi/%s", c->dir, file);
        f = fopen(p, "r");
        if (!f) return 0;
        while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (*line) list_add(l, line);
        }
        fclose(f);
        return l->n > 0;
}

/* Libraries (no main) install their public headers: everything under
   include/ keeps its path below it (include/foo/a.h -> INCDIR/foo/a.h, a bare
   include/a.h -> INCDIR/NAME/a.h); without include/, top-level headers go to
   INCDIR/NAME/. */
static void collect_headers(autobuild* a, const char* name) {
        for (int i = 0; i < a->headers.n; i++) {
                const char* h = a->headers.v[i];
                char dst[AUTO_PATH];
                if (!strncmp(h, "include/", 8)) {
                        if (strchr(h + 8, '/'))
                                snprintf(dst, sizeof dst, "%s", h + 8);
                        else
                                snprintf(dst, sizeof dst, "%s/%s", name, h + 8);
                } else if (!strchr(h, '/'))
                        snprintf(dst, sizeof dst, "%s/%s", name, h);
                else
                        continue;
                add_artifact(a, "inc", h, dst);
        }
}

static void inst_dir(char* out, size_t n, const char* var, const char* sub) {
        const char* dd = env_or("DESTDIR", "");
        const char* v  = getenv(var);
        if (v && *v)
                snprintf(out, n, "%s%s", dd, v);
        else
                snprintf(out, n, "%s%s/%s", dd, env_or("PREFIX", "/usr/local"),
                         sub);
}

int jbi_auto_install(jbi_ctx* c) {
        jbi_list m = {0}, done = {0};
        char bin[AUTO_PATH], lib[AUTO_PATH], inc[AUTO_PATH];
        int rc = -1;
        if (!read_manifest(c, "auto.build", &m)) {
                fputs("jbi: auto: nothing to install (run jbi build first)\n",
                      stderr);
                goto out;
        }
        inst_dir(bin, sizeof bin, "BINDIR", "bin");
        inst_dir(lib, sizeof lib, "LIBDIR", "lib");
        inst_dir(inc, sizeof inc, "INCDIR", "include");
        for (int i = 0; i < m.n; i++) {
                char *e = a_strdup(m.v[i]), *kind = e, *src, *dst = NULL;
                char p[AUTO_PATH], d[AUTO_PATH];
                const char *base, *mode = "644", *root;
                strbuf b = {0};
                int ok;
                src = strchr(e, '\t');
                if (!src) {
                        free(e);
                        continue;
                }
                *src++ = 0;
                if (!strcmp(kind, "inc")) {
                        dst = strchr(src, '\t');
                        if (!dst) {
                                free(e);
                                continue;
                        }
                        *dst++ = 0;
                        root   = inc;
                } else {
                        base = strrchr(src, '/');
                        dst  = (char*)(base ? base + 1 : src);
                        root = !strcmp(kind, "bin") ? bin : lib;
                        if (!strcmp(kind, "bin")) mode = "755";
                }
                if (!path_join(p, sizeof p, root, dst)) {
                        free(e);
                        goto out;
                }
                snprintf(d, sizeof d, "%s", p);
                *strrchr(d, '/') = 0;
                cat(&b, "install -d ");
                quote(&b, d);
                cat(&b, " && install -m ");
                cat(&b, mode);
                cat(&b, " ");
                quote(&b, src);
                cat(&b, " ");
                quote(&b, p);
                ok = runbuf(c, &b);
                free(e);
                if (!ok) {
                        fprintf(stderr, "jbi: auto: install failed for %s\n", p);
                        goto out;
                }
                add_unique(&done, p);
        }
        if (!c->dry && !write_manifest(c, "auto.installed", &done))
                fputs("jbi: warning: cannot record installed files\n", stderr);
        rc = 0;
out:
        list_free(&m);
        list_free(&done);
        return rc;
}

int jbi_auto_uninstall(jbi_ctx* c) {
        jbi_list m = {0}, dirs = {0};
        char bin[AUTO_PATH], lib[AUTO_PATH], inc[AUTO_PATH], name[256];
        char p[AUTO_PATH];
        strbuf b = {0};
        int rc = -1;
        inst_dir(bin, sizeof bin, "BINDIR", "bin");
        inst_dir(lib, sizeof lib, "LIBDIR", "lib");
        inst_dir(inc, sizeof inc, "INCDIR", "include");
        proj_name(c, name, sizeof name);
        if (read_manifest(c, "auto.installed", &m)) {
                size_t il = strlen(inc);
                cat(&b, "rm -f");
                for (int i = 0; i < m.n; i++) {
                        cat(&b, " ");
                        quote(&b, m.v[i]);
                        if (!strncmp(m.v[i], inc, il) && m.v[i][il] == '/') {
                                char* sl;
                                snprintf(p, sizeof p, "%s", m.v[i]);
                                while ((sl = strrchr(p, '/')) &&
                                       (size_t)(sl - p) > il) {
                                        *sl = 0;
                                        add_unique(&dirs, p);
                                }
                        }
                }
        } else {
                if (!safe_name(name) || !strcmp(name, ".") ||
                    !strcmp(name, "..")) {
                        fputs("jbi: auto: unsafe project name\n", stderr);
                        goto out;
                }
                char leaf[300];
                snprintf(leaf, sizeof leaf, "lib%s.a", name);
                cat(&b, "rm -f ");
                if (!path_join(p, sizeof p, bin, name)) goto out;
                quote(&b, p);
                cat(&b, " ");
                if (!path_join(p, sizeof p, lib, leaf)) goto out;
                quote(&b, p);
                cat(&b, " && rm -rf ");
                if (!path_join(p, sizeof p, inc, name)) goto out;
                quote(&b, p);
        }
        if (!runbuf(c, &b)) goto out;
        for (int i = 0; i < dirs.n; i++) { /* deepest first, best effort */
                cat(&b, "rmdir ");
                quote(&b, dirs.v[i]);
                cat(&b, " 2>/dev/null; true");
                (void)runbuf(c, &b);
        }
        if (!c->dry) {
                snprintf(p, sizeof p, "%s/.jbi/auto.installed", c->dir);
                (void)unlink(p);
        }
        rc = 0;
out:
        free(b.s);
        list_free(&m);
        list_free(&dirs);
        return rc;
}

int jbi_autobuild(jbi_ctx* ctx) {
        autobuild a;
        char p[AUTO_PATH];
        FILE* f;
        int rc = -1;
        memset(&a, 0, sizeof a);
        a.ctx = ctx;
        read_submodules(&a);
        if (ctx->dry) {
                puts(
                    "[jbi] auto: dry-run; source scan and compilation skipped");
                rc = 0;
                goto done;
        }
        if (!init_submodules(&a)) goto done;
        walk(&a, "", 0);
        if (!a.nsrc) {
                fputs("jbi: auto: no C/C++ sources found\n", stderr);
                goto done;
        }
        for (int i = 0; i < a.nsrc; i++) {
                scan_file(&a, a.src[i].path, &a.src[i].mainfn);
                if (a.src[i].cxx) a.cxx = 1;
        }
        for (int i = 0; i < a.headers.n; i++)
                scan_file(&a, a.headers.v[i], NULL);
        for (int i = 0; i < a.nsrc;) {
                if (a.src[i].mainfn && in_sub(&a, a.src[i].path)) {
                        free(a.src[i].path);
                        memmove(&a.src[i], &a.src[i + 1],
                                (size_t)(a.nsrc - i - 1) * sizeof *a.src);
                        a.nsrc--;
                } else
                        i++;
        }
        if (!a.nsrc) goto done;
        printf("[jbi] auto: %s project, %d source file(s)\n",
               a.cxx ? "C++" : "C", a.nsrc);
        for (int i = 0; i < a.nsrc; i++)
                if (a.src[i].mainfn)
                        printf("[jbi] auto: entry point %s\n", a.src[i].path);
        a.probe_cc = a.cxx ? env_or("CXX", "c++") : env_or("CC", "cc");
        a.probe_flags =
            a.cxx ? env_or("CXXFLAGS", "-O2") : env_or("CFLAGS", "-O2");
        a.probe_cppflags = env_or("CPPFLAGS", "");
        snprintf(a.probe, sizeof a.probe, "%s/jbi-XXXXXX",
                 env_or("TMPDIR", "/tmp"));
        if (!mkdtemp(a.probe)) {
                fprintf(stderr, "jbi: auto: cannot create probe directory\n");
                a.probe[0] = 0;
                goto done;
        }
        if (!path_join(p, sizeof p, a.probe, "probe.c")) goto done;
        f = fopen(p, "w");
        if (!f) goto done;
        fputs("int main(void){return 0;}\n", f);
        fclose(f);
        for (int i = 0; i < a.sysheaders.n; i++)
                resolve_header(&a, a.sysheaders.v[i]);
        rc = compile_link(&a);
        if (rc == 0) {
                char name[256];
                proj_name(ctx, name, sizeof name);
                if (a.artifacts.n && !strncmp(a.artifacts.v[0], "lib\t", 4))
                        collect_headers(&a, name);
                if (!write_manifest(ctx, "auto.build", &a.artifacts))
                        fputs("jbi: warning: cannot record build artifacts\n",
                              stderr);
        }
done:
        cleanup(&a);
        return rc;
}
