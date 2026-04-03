/* Luminary OS user-mode shell.
 * Runs as the init task, provides a simple REPL with built-in commands.
 * All I/O via syscalls - keyboard read and VGA write. */

#include "syscall.h"

static unsigned int strlen(const char *s)
{
    unsigned int len = 0;
    while (s[len])
        len++;
    return len;
}

static void puts(const char *s)
{
    write(s, strlen(s));
}

static int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int strncmp(const char *a, const char *b, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

static void utoa(unsigned int val, char *buf)
{
    char tmp[12];
    int i = 0;

    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }

    int j = 0;
    while (i > 0)
        buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static unsigned int atou(const char *s)
{
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned int)(*s++ - '0');
    return v;
}

static void cmd_kill(const char *arg)
{
    if (*arg == '\0') {
        puts("usage: kill <pid>\n");
        return;
    }
    unsigned int pid = atou(arg);
    int ret = kill(pid);
    if (ret < 0)
        puts("kill: failed (pid not found?)\n");
}

static void cmd_run(const char *arg)
{
    if (*arg == '\0') {
        puts("usage: run <module_index>\n");
        return;
    }
    unsigned int idx = atou(arg);
    int pid = exec(idx);
    if (pid < 0) {
        puts("run: failed (invalid module index?)\n");
    } else {
        char buf[12];
        puts("run: spawned pid ");
        utoa((unsigned int)pid, buf);
        puts(buf);
        puts("\n");
    }
}

static void cmd_help(void)
{
    puts("commands:\n");
    puts("  help       - show this message\n");
    puts("  echo       - print text\n");
    puts("  uptime     - show system uptime\n");
    puts("  getpid     - show current PID\n");
    puts("  ps         - list tasks\n");
    puts("  run <n>    - execute multiboot module n as a task\n");
    puts("  kill <pid> - terminate a task by PID\n");
    puts("  halt       - shut down\n");
    puts("  crash      - dereference a null pointer\n");
}

static void cmd_uptime(void)
{
    unsigned int ms = (unsigned int)uptime();
    unsigned int secs = ms / 1000;
    unsigned int frac = (ms % 1000) / 100;
    char buf[12];

    puts("Up ");
    utoa(secs, buf);
    puts(buf);
    puts(".");
    utoa(frac, buf);
    puts(buf);
    puts("s\n");
}

static void dispatch(char *cmd)
{
    if (cmd[0] == '\0')
        return;

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strncmp(cmd, "echo ", 5) == 0) {
        puts(cmd + 5);
        puts("\n");
    } else if (strcmp(cmd, "echo") == 0) {
        puts("\n");
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "getpid") == 0) {
        char buf[12];
        utoa((unsigned int)getpid(), buf);
        puts(buf);
        puts("\n");
    } else if (strcmp(cmd, "ps") == 0) {
        ps();
    } else if (strcmp(cmd, "halt") == 0) {
        halt();
    } else if (strncmp(cmd, "kill ", 5) == 0) {
        cmd_kill(cmd + 5);
    } else if (strcmp(cmd, "kill") == 0) {
        cmd_kill("");
    } else if (strncmp(cmd, "run ", 4) == 0) {
        cmd_run(cmd + 4);
    } else if (strcmp(cmd, "run") == 0) {
        cmd_run("");
    } else if (strcmp(cmd, "crash") == 0) {
        puts("dereferencing NULL...\n");
        volatile int *p = (volatile int *)0x0;
        (void)*p;
    } else {
        puts(cmd);
        puts(": unknown command\n");
    }
}

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
            puts("\n");
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
