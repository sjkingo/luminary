/* x — GUI session launcher.
 *
 * Reads /etc/xstart and executes each line sequentially (waiting for each
 * command to finish before starting the next). x stays alive as the parent
 * until the last launched process exits.
 *
 * If /etc/xstart does not exist, x exits immediately with no error.
 *
 * File format:
 *   - One command per line, space-separated tokens
 *   - Blank lines and lines starting with '#' are ignored
 *   - No shell features (pipes, redirects, quoting)
 *
 * Command resolution: if the first token contains '/', used as-is.
 * Otherwise prepend /bin/.
 */

#include "syscall.h"
#include "libc/stdio.h"

#define XSTART  "/etc/xstart"

static char buf[1024];

static int is_space(char c) { return c == ' ' || c == '\t'; }

static void run_line(char *line)
{
    char *xargv[16];
    int xargc = 0;
    char resolved[256];

    /* skip leading whitespace */
    while (is_space(*line)) line++;

    /* skip blank lines and comments */
    if (*line == '\0' || *line == '#') return;

    /* tokenise in-place */
    char *t = line;
    while (*t && xargc < 15) {
        xargv[xargc++] = t;
        while (*t && !is_space(*t)) t++;
        if (*t) { *t++ = '\0'; while (is_space(*t)) t++; }
    }
    xargv[xargc] = (char *)0;
    if (xargc == 0) return;

    /* resolve path */
    const char *cmd = xargv[0];
    int has_slash = 0;
    for (int i = 0; cmd[i]; i++) if (cmd[i] == '/') { has_slash = 1; break; }

    if (has_slash) {
        int i = 0;
        while (cmd[i] && i < 255) { resolved[i] = cmd[i]; i++; }
        resolved[i] = '\0';
    } else {
        resolved[0] = '/'; resolved[1] = 'b'; resolved[2] = 'i';
        resolved[3] = 'n'; resolved[4] = '/';
        int off = 5;
        for (int i = 0; cmd[i] && off < 255; i++) resolved[off++] = cmd[i];
        resolved[off] = '\0';
    }

    int pid = fork();
    if (pid == 0) {
        execv(resolved, xargv);
        printf("x: exec failed: %s\n", resolved);
        exit(1);
    } else if (pid > 0) {
        waitpid(pid, (int *)0, 0);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int fd = open(XSTART, O_RDONLY);
    if (fd < 0) return 0;

    int n = read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        int has_more = (*p == '\n');
        *p = '\0';
        run_line(line);
        if (!has_more) break;
        p++;
    }

    return 0;
}
