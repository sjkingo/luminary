/* ps — list running tasks in a tree view */

#include "syscall.h"
#include "sys_dev.h"
#include "libc/stdio.h"
#include "libc/string.h"

#define MAX_TASKS 64

struct task_info {
    unsigned int pid;
    unsigned int ppid;
    int          prio;
    unsigned int created_s;
    char         name[32];
    int          printed;
};

/*
 * Parse a tab-delimited line: PID\tPPID\tPRIO\tTIME\tCMD\n
 * Returns 0 on success, -1 on failure.
 */
static int parse_line(const char *line, struct task_info *ti)
{
    const char *p = line;

#define PARSE_UINT(dst) do {                              \
    (dst) = 0;                                            \
    if (*p < '0' || *p > '9') return -1;                 \
    while (*p >= '0' && *p <= '9') {                      \
        (dst) = (dst) * 10 + (unsigned int)(*p - '0');   \
        p++;                                              \
    }                                                     \
    if (*p == '\t') p++;                                  \
} while (0)

    PARSE_UINT(ti->pid);
    PARSE_UINT(ti->ppid);

    /* PRIO may be negative */
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    unsigned int pv = 0;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') { pv = pv * 10 + (unsigned int)(*p - '0'); p++; }
    if (*p == '\t') p++;
    ti->prio = neg ? -(int)pv : (int)pv;

    PARSE_UINT(ti->created_s);

    /* CMD: rest of line up to newline */
    unsigned int ni = 0;
    while (*p && *p != '\n' && *p != '\r' && ni < 31) ti->name[ni++] = *p++;
    ti->name[ni] = '\0';

    ti->printed = 0;
    return 0;

#undef PARSE_UINT
}

static struct task_info tasks[MAX_TASKS];
static int              ntasks = 0;

static void print_row(struct task_info *ti, const char *prefix)
{
    printf("%4u %4u %3d %4us %s%s\n",
           ti->pid, ti->ppid, ti->prio,
           ti->created_s, prefix, ti->name);
}

static void print_children(unsigned int ppid, const char *prefix, int depth)
{
    if (depth > 16) return;

    int children[MAX_TASKS];
    int nc = 0;
    for (int i = 0; i < ntasks; i++) {
        if (!tasks[i].printed && tasks[i].ppid == ppid && tasks[i].ppid != 0)
            children[nc++] = i;
    }

    for (int c = 0; c < nc; c++) {
        int idx = children[c];
        tasks[idx].printed = 1;
        print_row(&tasks[idx], prefix);

        char newpfx[64];
        snprintf(newpfx, sizeof(newpfx), "%s ", prefix);
        print_children(tasks[idx].pid, newpfx, depth + 1);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int sfd = open("/dev/sys", O_RDWR);
    if (sfd < 0) { printf("ps: cannot open /dev/sys\n"); exit(1); }

    char buf[2048];
    int n = sys_ps(sfd, buf, sizeof(buf) - 1);
    vfs_close(sfd);
    if (n <= 0) return 1;
    buf[n] = '\0';

    /* Skip header line */
    const char *p = buf;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;

    while (*p && ntasks < MAX_TASKS) {
        const char *end = p;
        while (*end && *end != '\n') end++;

        char line[128];
        unsigned int ll = (unsigned int)(end - p);
        if (ll >= sizeof(line)) ll = sizeof(line) - 1;
        unsigned int li;
        for (li = 0; li < ll; li++) line[li] = p[li];
        line[li] = '\0';

        struct task_info ti;
        if (parse_line(line, &ti) == 0)
            tasks[ntasks++] = ti;

        p = end;
        if (*p == '\n') p++;
    }

    if (ntasks == 0) return 0;

    printf(" PID PPID PRI  TIME CMD\n");

    for (int i = 0; i < ntasks; i++) {
        if (tasks[i].ppid == 0 && !tasks[i].printed) {
            tasks[i].printed = 1;
            print_row(&tasks[i], "");
            print_children(tasks[i].pid, " ", 1);
        }
    }

    return 0;
}
