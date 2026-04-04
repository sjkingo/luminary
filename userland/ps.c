/* ps — list running tasks in a tree view */

#include "syscall.h"

/* ── minimal helpers ──────────────────────────────────────────────────────── */

static void mywrite(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

static void myutoa(unsigned int v, char *buf)
{
    char tmp[12];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* Right-align v in a field of width w, space-padded on left */
static void print_uint_w(unsigned int v, unsigned int w)
{
    char num[12];
    myutoa(v, num);
    unsigned int len = 0;
    while (num[len]) len++;
    unsigned int pad = (len < w) ? w - len : 0;
    char spaces[12];
    unsigned int i;
    for (i = 0; i < pad && i < 11; i++) spaces[i] = ' ';
    spaces[i] = '\0';
    mywrite(spaces);
    mywrite(num);
}

/* ── task record ──────────────────────────────────────────────────────────── */

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

    /* Parse unsigned int from p, advance past it */
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

/* ── task list ────────────────────────────────────────────────────────────── */

static struct task_info tasks[MAX_TASKS];
static int              ntasks = 0;

/* ── tree rendering ───────────────────────────────────────────────────────── */

static void print_row(struct task_info *ti, const char *prefix)
{
    print_uint_w(ti->pid,       4);
    mywrite(" ");
    print_uint_w(ti->ppid,      4);
    mywrite(" ");
    if (ti->prio < 0) {
        mywrite("-");
        print_uint_w((unsigned int)(-ti->prio), 2);
    } else {
        mywrite(" ");
        print_uint_w((unsigned int)(ti->prio), 2);
    }
    mywrite(" ");
    print_uint_w(ti->created_s, 4);
    mywrite("s ");
    mywrite(prefix);
    mywrite(ti->name);
    mywrite("\n");
}

static void print_children(unsigned int ppid, const char *prefix, int depth)
{
    if (depth > 16) return;

    /* Collect children: tasks with a real (non-zero) ppid matching this pid */
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

        /* Build child prefix: extend by 2 spaces */
        char newpfx[64];
        unsigned int pi = 0;
        for (; prefix[pi] && pi < 62; pi++) newpfx[pi] = prefix[pi];
        newpfx[pi++] = ' '; newpfx[pi++] = ' ';
        newpfx[pi] = '\0';

        print_children(tasks[idx].pid, newpfx, depth + 1);
    }
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    char buf[2048];
    int n = ps(buf, sizeof(buf) - 1);
    if (n <= 0) return 1;
    buf[n] = '\0';

    /* Skip header line */
    const char *p = buf;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;

    /* Parse each line */
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

    /* Banner */
    mywrite(" PID PPID PRI  TIME CMD\n");

    /* Print roots: idle (pid==0) and any task whose ppid is 0 */
    for (int i = 0; i < ntasks; i++) {
        int is_root = (tasks[i].ppid == 0);
        if (is_root && !tasks[i].printed) {
            tasks[i].printed = 1;
            print_row(&tasks[i], "");
            print_children(tasks[i].pid, "  ", 1);
        }
    }

    return 0;
}
