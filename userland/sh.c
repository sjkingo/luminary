/* Luminary shell.
 * Provides a simple interactive REPL.
 * All I/O via syscalls. Programs are loaded from the VFS. */

#include "syscall.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"

/* ── CWD and path resolution ─────────────────────────────────────────────── */

static char cwd[256] = "/";

/* Resolve a user-supplied path against cwd into out (size outsz).
 * Absolute paths are used as-is. Relative paths are joined to cwd.
 * Handles ".." by stripping the last component. */
static void resolve_path(const char *path, char *out, unsigned int outsz)
{
    char tmp[256];
    unsigned int n = 0;

    if (path[0] == '/') {
        while (path[n] && n < sizeof(tmp) - 1) { tmp[n] = path[n]; n++; }
        tmp[n] = '\0';
    } else {
        unsigned int ci = 0;
        while (cwd[ci] && n < sizeof(tmp) - 1) tmp[n++] = cwd[ci++];
        if (n > 0 && tmp[n-1] != '/' && n < sizeof(tmp) - 1) tmp[n++] = '/';
        unsigned int pi = 0;
        while (path[pi] && n < sizeof(tmp) - 1) tmp[n++] = path[pi++];
        tmp[n] = '\0';
    }

    char norm[256];
    unsigned int ni = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '/') end++;
        unsigned int len = (unsigned int)(end - p);

        if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (ni > 1) { ni--; while (ni > 1 && norm[ni-1] != '/') ni--; }
        } else if (!(len == 1 && p[0] == '.')) {
            if (ni == 0 || norm[ni-1] != '/') { if (ni < outsz-1) norm[ni++] = '/'; }
            for (unsigned int j = 0; j < len && ni < outsz-1; j++) norm[ni++] = p[j];
        }
        p = end;
    }

    if (ni == 0) { out[0] = '/'; out[1] = '\0'; return; }
    norm[ni] = '\0';
    unsigned int i = 0;
    while (norm[i] && i < outsz-1) { out[i] = norm[i]; i++; }
    out[i] = '\0';
}

/* ── command implementations ─────────────────────────────────────────────── */

static void cmd_help(void)
{
    printf("commands:\n"
           "  help           - show this message\n"
           "  echo <text>    - print text\n"
           "  uptime         - show system uptime\n"
           "  getpid         - show current PID\n"
           "  ps             - list tasks\n"
           "  exec <path>    - execute ELF (fork+exec, wait for completion)\n"
           "  spawn <path>   - spawn ELF as background task\n"
           "  kill <pid>     - terminate a task by PID\n"
           "  ls [path]      - list directory (default: cwd)\n"
           "  cat <path>     - print file contents\n"
           "  stat <path>    - show file info\n"
           "  cd <path>      - change directory\n"
           "  pwd            - print working directory\n"
           "  mount          - show mounted filesystems\n"
           "  exit           - exit the shell\n"
           "  halt           - shut down\n"
           "  crash          - dereference a null pointer\n");
}

static void cmd_uptime(void)
{
    unsigned int ms   = (unsigned int)uptime();
    unsigned int secs = ms / 1000;
    unsigned int frac = (ms % 1000) / 100;
    printf("Up %u.%us\n", secs, frac);
}

static void cmd_kill(const char *arg)
{
    if (*arg == '\0') { printf("usage: kill <pid>\n"); return; }
    unsigned int pid = atou(arg);
    if (kill(pid) < 0)
        printf("kill: failed (pid not found?)\n");
}

static void cmd_pwd(void)
{
    printf("%s\n", cwd);
}

static void cmd_cd(const char *arg)
{
    if (*arg == '\0') { printf("usage: cd <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));
    struct vfs_stat st;
    if (vfs_stat(resolved, &st) < 0 || !(st.type & VFS_DIR)) {
        printf("cd: not a directory: %s\n", resolved);
        return;
    }
    strncpy(cwd, resolved, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = '\0';
}

static void cmd_exec(const char *arg)
{
    if (*arg == '\0') { printf("usage: exec <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));

    int pid = fork();
    if (pid == 0) {
        char *argv[] = { resolved, (char *)0 };
        execv(resolved, argv);
        printf("exec: failed\n");
        exit(1);
    } else if (pid > 0) {
        waitpid(pid);
    } else {
        printf("exec: fork failed\n");
    }
}

static void cmd_spawn(const char *arg)
{
    if (*arg == '\0') { printf("usage: spawn <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));
    int pid = spawn(resolved);
    if (pid < 0)
        printf("spawn: failed: %s\n", resolved);
    else
        printf("spawn: launched pid %d\n", pid);
}

static void cmd_ls(const char *arg)
{
    char resolved[256];
    resolve_path(*arg ? arg : ".", resolved, sizeof(resolved));

    int fd = vfs_open(resolved);
    if (fd < 0) {
        printf("ls: cannot open '%s'\n", resolved);
        return;
    }

    struct vfs_stat st;
    if (vfs_stat(resolved, &st) == 0 && (st.type & VFS_FILE)) {
        printf("%s\n", resolved);
        vfs_close(fd);
        return;
    }

    struct vfs_dirent de;
    while (vfs_readdir(fd, &de) == 1)
        printf("%s%s\n", de.name, (de.type & VFS_DIR) ? "/" : "");
    vfs_close(fd);
}

static void cmd_cat(const char *arg)
{
    if (*arg == '\0') { printf("usage: cat <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));

    int fd = vfs_open(resolved);
    if (fd < 0) {
        printf("cat: cannot open '%s'\n", resolved);
        return;
    }

    char buf[256];
    int n;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0)
        write(buf, (unsigned int)n);
    vfs_close(fd);
}

static void cmd_stat(const char *arg)
{
    if (*arg == '\0') { printf("usage: stat <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));

    struct vfs_stat st;
    if (vfs_stat(resolved, &st) < 0) {
        printf("stat: not found: %s\n", resolved);
        return;
    }
    printf("%s: %s, %u bytes\n", resolved,
           (st.type & VFS_DIR) ? "directory" : "file", st.size);
}

/* ── command dispatch ────────────────────────────────────────────────────── */

static void dispatch(char *cmd)
{
    if (cmd[0] == '\0') return;

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strncmp(cmd, "echo ", 5) == 0) {
        printf("%s\n", cmd + 5);
    } else if (strcmp(cmd, "echo") == 0) {
        putchar('\n');
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "getpid") == 0) {
        printf("%d\n", getpid());
    } else if (strcmp(cmd, "ps") == 0) {
        ps();
    } else if (strcmp(cmd, "halt") == 0) {
        halt();
    } else if (strncmp(cmd, "kill ", 5) == 0) {
        cmd_kill(cmd + 5);
    } else if (strcmp(cmd, "kill") == 0) {
        cmd_kill("");
    } else if (strncmp(cmd, "exec ", 5) == 0) {
        cmd_exec(cmd + 5);
    } else if (strcmp(cmd, "exec") == 0) {
        cmd_exec("");
    } else if (strncmp(cmd, "spawn ", 6) == 0) {
        cmd_spawn(cmd + 6);
    } else if (strcmp(cmd, "spawn") == 0) {
        cmd_spawn("");
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (strncmp(cmd, "cd ", 3) == 0) {
        cmd_cd(cmd + 3);
    } else if (strcmp(cmd, "cd") == 0) {
        cmd_cd("");
    } else if (strncmp(cmd, "ls ", 3) == 0) {
        cmd_ls(cmd + 3);
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(".");
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        cmd_cat(cmd + 4);
    } else if (strcmp(cmd, "cat") == 0) {
        cmd_cat("");
    } else if (strncmp(cmd, "stat ", 5) == 0) {
        cmd_stat(cmd + 5);
    } else if (strcmp(cmd, "stat") == 0) {
        cmd_stat("");
    } else if (strcmp(cmd, "mount") == 0) {
        mount();
    } else if (strcmp(cmd, "exit") == 0) {
        exit(0);
    } else if (strcmp(cmd, "crash") == 0) {
        printf("dereferencing NULL...\n");
        volatile int *p = (volatile int *)0x0;
        (void)*p;
    } else if (cmd[0] == '/' || (cmd[0] == '.' && cmd[1] == '/') ||
               (cmd[0] == '.' && cmd[1] == '.' && cmd[2] == '/')) {
        /* ./prog, ../prog, /path/prog style direct execution */
        cmd_exec(cmd);
    } else {
        printf("%s: unknown command\n", cmd);
    }
}

/* ── entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    static char cmd[128];
    int idx = 0;
    char c;

    printf("Luminary shell\nType 'help' for commands.\n\n$ ");

    for (;;) {
        if (read(&c, 1) == 0)
            continue;

        if (c == '\n') {
            putchar('\n');
            cmd[idx] = '\0';
            dispatch(cmd);
            idx = 0;
            printf("$ ");
        } else if (c == '\b') {
            if (idx > 0) {
                idx--;
                write("\b \b", 3);
            }
        } else {
            if (idx < (int)(sizeof(cmd) - 1)) {
                cmd[idx++] = c;
                write(&c, 1);
            }
        }
    }
}
