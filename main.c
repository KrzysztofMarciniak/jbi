/* jbi - just build it: command line */
#include <stdio.h>
#include <string.h>

#include "jbi.h"

static void usage(FILE* o) {
        fputs(
            "usage: jbi [command] [-n] [-C dir]\n"
            "commands:\n"
            "  build       detect the build system and build (default)\n"
            "  install     run the install command of the detected system\n"
            "  uninstall   run the uninstall command of the detected system\n"
            "  list        show the ranked detection plan\n"
            "  help        this text\n"
            "options:\n"
            "  -n          dry run: print the first command instead of running "
            "it\n"
            "  -C dir      project directory (default: .)\n"
            "  -V          version\n",
            o);
}

int main(int argc, char** argv) {
        const char *cmd = "build", *dir = ".";
        int dry = 0, rc = 0;
        jbi_ctx* c;

        for (int i = 1; i < argc; i++) {
                const char* a = argv[i];
                if (!strcmp(a, "-n") || !strcmp(a, "--dry-run"))
                        dry = 1;
                else if (!strcmp(a, "-C") && i + 1 < argc)
                        dir = argv[++i];
                else if (!strcmp(a, "-h") || !strcmp(a, "--help") ||
                         !strcmp(a, "help")) {
                        usage(stdout);
                        return 0;
                } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
                        puts("jbi " JBI_VERSION);
                        return 0;
                } else if (!strcmp(a, "build") || !strcmp(a, "install") ||
                           !strcmp(a, "uninstall") || !strcmp(a, "list"))
                        cmd = a;
                else {
                        usage(stderr);
                        return 2;
                }
        }
        c = jbi_open(dir, dry);
        if (!c) {
                fprintf(stderr, "jbi: bad directory: %s\n", dir);
                return 2;
        }

        if (jbi_init_submodules(c) != 0) {
                fprintf(stderr, "jbi: failed to initialize Git submodules\n");
                jbi_close(c);
                return 1;
        }
        if (!c->ncfg && strcmp(cmd, "build") != 0) {
                fprintf(
                    stderr,
                    "jbi: no .jbi configs found (looked in %s/.jbi and %s)\n",
                    c->dir, jbi_sharedir());
                jbi_close(c);
                return 2;
        }
        if (!strcmp(cmd, "build"))
                rc = jbi_build(c);
        else if (!strcmp(cmd, "install"))
                rc = jbi_install(c);
        else if (!strcmp(cmd, "uninstall"))
                rc = jbi_uninstall(c);
        else
                jbi_list_plan(c, stdout);
        if (rc != 0 && !strcmp(cmd, "build"))
                fputs("jbi: build failed, nothing worked\n", stderr);
        jbi_close(c);
        return rc == 0 ? 0 : 1;
}
