#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#define NOB_WARN_DEPRECATED
#include "nob.h"

static bool pkg_config(Cmd *cmd, const char *mode, const char *pkgs)
{
    char shell_cmd[256];
    snprintf(shell_cmd, sizeof(shell_cmd),
             "pkg-config %s %s 2>/dev/null", mode, pkgs);

    FILE *f = popen(shell_cmd, "r");
    if (!f) return false;

    char buf[1024] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    int status = pclose(f);

    if (status != 0 || n == 0) return false;

    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';

    char *tok = strtok(buf, " \t");
    while (tok) {
        cmd_append(cmd, temp_sprintf("%s", tok));
        tok = strtok(NULL, " \t");
    }
    return true;
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    Cmd cmd = {0};

    cmd_append(&cmd, "cc");
    cmd_append(&cmd, "-std=gnu11", "-Wall", "-Wextra", "-O2");

    pkg_config(&cmd, "--cflags", "flac ncurses");

    cmd_append(&cmd, "-o", "./flec", "./main.c");

    if (!pkg_config(&cmd, "--libs", "flac ncurses"))
        cmd_append(&cmd, "-lFLAC", "-lncurses");

    if (!cmd_run(&cmd, .dont_reset = true)) return 1;
    return 0;
}
