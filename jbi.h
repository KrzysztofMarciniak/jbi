/* jbi - just build it: library API */
#ifndef JBI_H
#define JBI_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JBI_VERSION "1.0.1"

typedef struct jbi_list {
        char** v;
        int n, cap;
} jbi_list;

/* one build system, loaded from a *.jbi config */
typedef struct jbi_cfg {
        char name[64];
        int order; /* lower = tried earlier */
        jbi_list files, build, install, uninstall;
        int hit; /* one of its files exists in the project */
        int doc; /* a doc file mentions one of its build commands */
} jbi_cfg;

typedef struct jbi_ctx {
        char dir[4096]; /* project dir (absolute) */
        jbi_cfg* cfg;
        int ncfg;
        int dry; /* print first command instead of running */
        int detected;
} jbi_ctx;

const char* jbi_sharedir(void); /* where system configs live */
jbi_ctx* jbi_open(const char* dir,
                  int dry); /* loads configs; NULL if dir is bad */
void jbi_close(jbi_ctx* c);
int jbi_detect(jbi_ctx* c);    /* threaded search; returns number of hits */
int jbi_build(jbi_ctx* c);     /* 0 = success */
int jbi_install(jbi_ctx* c);   /* 0 = success */
int jbi_uninstall(jbi_ctx* c); /* 0 = success */
void jbi_list_plan(jbi_ctx* c, FILE* out); /* ranked plan */
int jbi_init_submodules(jbi_ctx* c);       /* 0 = success */
int jbi_auto_install(jbi_ctx* c);   /* install autobuild output; 0 = success */
int jbi_auto_uninstall(jbi_ctx* c); /* remove it again; 0 = success */

#ifdef __cplusplus
}
#endif
#endif
