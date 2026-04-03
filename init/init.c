/* Luminary OS user-mode shell.
 * Runs as the init task (PID 1), provides a simple REPL.
 * All I/O via syscalls. Programs are loaded from the VFS. */

#include "syscall.h"

/* ── minimal string/print utilities ─────────────────────────────────────── */

static unsigned int mystrlen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void puts(const char *s)
{
    write(s, mystrlen(s));
}

static void putch(char c)
{
    write(&c, 1);
}

static int mystrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int mystrncmp(const char *a, const char *b, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static void utoa(unsigned int val, char *buf)
{
    char tmp[12];
    int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (val > 0) { tmp[i++] = '0' + (char)(val % 10); val /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static unsigned int atou(const char *s)
{
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned int)(*s++ - '0');
    return v;
}

/* Print an unsigned int */
static void putu(unsigned int v)
{
    char buf[12];
    utoa(v, buf);
    puts(buf);
}

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
        /* absolute — copy directly */
        while (path[n] && n < sizeof(tmp) - 1) { tmp[n] = path[n]; n++; }
        tmp[n] = '\0';
    } else {
        /* relative — prepend cwd */
        unsigned int ci = 0;
        while (cwd[ci] && n < sizeof(tmp) - 1) tmp[n++] = cwd[ci++];
        if (n > 0 && tmp[n-1] != '/' && n < sizeof(tmp) - 1) tmp[n++] = '/';
        unsigned int pi = 0;
        while (path[pi] && n < sizeof(tmp) - 1) tmp[n++] = path[pi++];
        tmp[n] = '\0';
    }

    /* Normalise: walk components, handle ".." by backing up */
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
            /* strip last component */
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
    puts("commands:\n");
    puts("  help           - show this message\n");
    puts("  echo <text>    - print text\n");
    puts("  uptime         - show system uptime\n");
    puts("  getpid         - show current PID\n");
    puts("  ps             - list tasks\n");
    puts("  exec <path>    - execute ELF from VFS path as a task\n");
    puts("  kill <pid>     - terminate a task by PID\n");
    puts("  ls [path]      - list directory (default: cwd)\n");
    puts("  cat <path>     - print file contents\n");
    puts("  stat <path>    - show file info\n");
    puts("  cd <path>      - change directory\n");
    puts("  pwd            - print working directory\n");
    puts("  mount          - show mounted filesystems\n");
    puts("  halt           - shut down\n");
    puts("  crash          - dereference a null pointer\n");
}

static void cmd_uptime(void)
{
    unsigned int ms   = (unsigned int)uptime();
    unsigned int secs = ms / 1000;
    unsigned int frac = (ms % 1000) / 100;
    puts("Up ");
    putu(secs);
    putch('.');
    putu(frac);
    puts("s\n");
}

static void cmd_kill(const char *arg)
{
    if (*arg == '\0') { puts("usage: kill <pid>\n"); return; }
    unsigned int pid = atou(arg);
    if (kill(pid) < 0)
        puts("kill: failed (pid not found?)\n");
}

static void cmd_pwd(void)
{
    puts(cwd);
    putch('\n');
}

static void cmd_cd(const char *arg)
{
    if (*arg == '\0') { puts("usage: cd <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));
    /* Verify it exists and is a directory */
    struct vfs_stat st;
    if (vfs_stat(resolved, &st) < 0 || !(st.type & VFS_DIR)) {
        puts("cd: not a directory: ");
        puts(resolved);
        putch('\n');
        return;
    }
    unsigned int i = 0;
    while (resolved[i] && i < sizeof(cwd) - 1) { cwd[i] = resolved[i]; i++; }
    cwd[i] = '\0';
}

static void cmd_exec(const char *arg)
{
    if (*arg == '\0') { puts("usage: exec <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));
    int pid = exec(resolved);
    if (pid < 0) {
        puts("exec: failed: ");
        puts(resolved);
        putch('\n');
    } else {
        puts("exec: spawned pid ");
        putu((unsigned int)pid);
        putch('\n');
    }
}

static void cmd_ls(const char *arg)
{
    char resolved[256];
    resolve_path(*arg ? arg : ".", resolved, sizeof(resolved));

    int fd = vfs_open(resolved);
    if (fd < 0) {
        puts("ls: cannot open '");
        puts(resolved);
        puts("'\n");
        return;
    }

    struct vfs_stat st;
    if (vfs_stat(resolved, &st) == 0 && (st.type & VFS_FILE)) {
        puts(resolved);
        putch('\n');
        vfs_close(fd);
        return;
    }

    struct vfs_dirent de;
    while (vfs_readdir(fd, &de) == 1) {
        puts(de.name);
        if (de.type & VFS_DIR) putch('/');
        putch('\n');
    }
    vfs_close(fd);
}

static void cmd_cat(const char *arg)
{
    if (*arg == '\0') { puts("usage: cat <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));

    int fd = vfs_open(resolved);
    if (fd < 0) {
        puts("cat: cannot open '");
        puts(resolved);
        puts("'\n");
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
    if (*arg == '\0') { puts("usage: stat <path>\n"); return; }
    char resolved[256];
    resolve_path(arg, resolved, sizeof(resolved));

    struct vfs_stat st;
    if (vfs_stat(resolved, &st) < 0) {
        puts("stat: not found: ");
        puts(resolved);
        putch('\n');
        return;
    }
    puts(resolved);
    puts(": ");
    puts((st.type & VFS_DIR) ? "directory" : "file");
    puts(", ");
    putu(st.size);
    puts(" bytes\n");
}

/* ── command dispatch ────────────────────────────────────────────────────── */

static void dispatch(char *cmd)
{
    if (cmd[0] == '\0') return;

    if (mystrcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (mystrncmp(cmd, "echo ", 5) == 0) {
        puts(cmd + 5);
        putch('\n');
    } else if (mystrcmp(cmd, "echo") == 0) {
        putch('\n');
    } else if (mystrcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (mystrcmp(cmd, "getpid") == 0) {
        putu((unsigned int)getpid());
        putch('\n');
    } else if (mystrcmp(cmd, "ps") == 0) {
        ps();
    } else if (mystrcmp(cmd, "halt") == 0) {
        halt();
    } else if (mystrncmp(cmd, "kill ", 5) == 0) {
        cmd_kill(cmd + 5);
    } else if (mystrcmp(cmd, "kill") == 0) {
        cmd_kill("");
    } else if (mystrncmp(cmd, "exec ", 5) == 0) {
        cmd_exec(cmd + 5);
    } else if (mystrcmp(cmd, "exec") == 0) {
        cmd_exec("");
    } else if (mystrcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (mystrncmp(cmd, "cd ", 3) == 0) {
        cmd_cd(cmd + 3);
    } else if (mystrcmp(cmd, "cd") == 0) {
        cmd_cd("");
    } else if (mystrncmp(cmd, "ls ", 3) == 0) {
        cmd_ls(cmd + 3);
    } else if (mystrcmp(cmd, "ls") == 0) {
        cmd_ls(".");
    } else if (mystrncmp(cmd, "cat ", 4) == 0) {
        cmd_cat(cmd + 4);
    } else if (mystrcmp(cmd, "cat") == 0) {
        cmd_cat("");
    } else if (mystrncmp(cmd, "stat ", 5) == 0) {
        cmd_stat(cmd + 5);
    } else if (mystrcmp(cmd, "stat") == 0) {
        cmd_stat("");
    } else if (mystrcmp(cmd, "mount") == 0) {
        mount();
    } else if (mystrcmp(cmd, "crash") == 0) {
        puts("dereferencing NULL...\n");
        volatile int *p = (volatile int *)0x0;
        (void)*p;
    } else {
        puts(cmd);
        puts(": unknown command\n");
    }
}

/* ── entry point ─────────────────────────────────────────────────────────── */

void _start(void)
{
    static char cmd[128];
    int idx = 0;
    char c;

    puts("Luminary OS shell\nType 'help' for commands.\n\n");
    puts("$ ");

    for (;;) {
        if (read(&c, 1) == 0)
            continue;

        if (c == '\n') {
            putch('\n');
            cmd[idx] = '\0';
            dispatch(cmd);
            idx = 0;
            puts("$ ");
        } else if (c == '\b') {
            if (idx > 0) {
                idx--;
                puts("\b \b");
            }
        } else {
            if (idx < (int)(sizeof(cmd) - 1)) {
                cmd[idx++] = c;
                write(&c, 1);
            }
        }
    }
}
