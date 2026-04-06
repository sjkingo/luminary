/* Luminary shell.
 *
 * Interactive REPL with pipeline support, I/O redirection, and background jobs.
 *
 * cd and exit are the only commands that truly must run in-process (they modify
 * shell state). The others are built-in as a convenience to avoid process
 * overhead for common operations. When any built-in appears with I/O
 * redirections it is still forked (except cd/exit, which save/restore fds).
 *
 * Background jobs (&):
 *   Trailing & on a command line runs the pipeline without waiting. The job is
 *   added to the job table (JOB_MAX slots). Finished jobs are reaped at each
 *   prompt. fg [n] brings job n (or the most recent) to the foreground.
 */

#include "syscall.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"

static char cwd[256] = "/";

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

#define PIPE_MAX  8
#define JOB_MAX   8
#define HIST_MAX  100
#define HIST_LEN  256

struct job {
    int  used;
    int  pids[PIPE_MAX];
    int  npids;
    char cmd[128];
};

static struct job jobs[JOB_MAX];
static int        job_next = 1; /* 1-based job numbers */

/* Allocate a job slot. Returns slot index or -1 if full. */
static int job_alloc(void)
{
    for (int i = 0; i < JOB_MAX; i++)
        if (!jobs[i].used) return i;
    return -1;
}

/* Add a completed pipeline as a background job. */
static void job_add(int pids[], int npids, const char *cmd)
{
    int i = job_alloc();
    if (i < 0) { printf("sh: job table full\n"); return; }
    jobs[i].used  = 1;
    jobs[i].npids = npids;
    for (int j = 0; j < npids; j++) jobs[i].pids[j] = pids[j];
    strncpy(jobs[i].cmd, cmd, sizeof(jobs[i].cmd) - 1);
    jobs[i].cmd[sizeof(jobs[i].cmd) - 1] = '\0';
    printf("[%d] %d\n", i + 1, pids[npids - 1]);
}

/* Reap any background jobs that have finished. Called before each prompt. */
static void job_reap(void)
{
    for (int i = 0; i < JOB_MAX; i++) {
        if (!jobs[i].used) continue;
        int done = 1;
        for (int j = 0; j < jobs[i].npids; j++) {
            if (!task_done(jobs[i].pids[j])) { done = 0; break; }
        }
        if (done) {
            printf("[%d] done    %s\n", i + 1, jobs[i].cmd);
            jobs[i].used = 0;
        }
    }
}

static void cmd_help(void)
{
    printf("built-in commands:\n"
           "  help           - show this message\n"
           "  echo <text>    - print text\n"
           "  getpid         - show current PID\n"
           "  cd <path>      - change directory\n"
           "  pwd            - print working directory\n"
           "  jobs           - list background jobs\n"
           "  fg [n]         - bring job n (or most recent) to foreground\n"
           "  crash          - dereference a null pointer\n");
}

static void cmd_cd(const char *arg)
{
    if (*arg == '\0') { printf("usage: cd <path>\n"); return; }
    if (chdir(arg) < 0) {
        printf("cd: %s: no such directory\n", arg);
        return;
    }
    getcwd(cwd, sizeof(cwd));
}

static void cmd_jobs(void)
{
    int any = 0;
    for (int i = 0; i < JOB_MAX; i++) {
        if (!jobs[i].used) continue;
        printf("[%d] running  %s\n", i + 1, jobs[i].cmd);
        any = 1;
    }
    if (!any) printf("no background jobs\n");
}

/* Wait for a job in the foreground; Ctrl+C kills it. */
static void job_fg(int slot)
{
    struct job *j = &jobs[slot];
    printf("[%d] %s\n", slot + 1, j->cmd);

    for (int i = 0; i < j->npids; i++) {
        while (!task_done(j->pids[i])) {
            char c;
            if (read_nb(0, &c, 1) > 0 && c == '\x03') {
                for (int k = i; k < j->npids; k++)
                    kill((unsigned int)j->pids[k]);
                printf("^C\n");
                j->used = 0;
                return;
            }
            yield();
        }
    }
    j->used = 0;
}

static void cmd_fg(const char *arg)
{
    int slot = -1;

    if (arg && *arg) {
        int n = atoi(arg);
        if (n >= 1 && n <= JOB_MAX && jobs[n - 1].used)
            slot = n - 1;
        else { printf("fg: no such job: %s\n", arg); return; }
    } else {
        /* find most recently added job (highest used slot) */
        for (int i = JOB_MAX - 1; i >= 0; i--) {
            if (jobs[i].used) { slot = i; break; }
        }
        if (slot < 0) { printf("fg: no current job\n"); return; }
    }

    job_fg(slot);
}

#define ARGV_MAX 32

/* Split line into whitespace-delimited tokens in-place.
 * Single-quoted spans preserve interior whitespace.
 * Returns token count; argv is NULL-terminated. */
static int tokenise(char *line, char *argv[], int argv_max)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < argv_max) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        char *tok = p;
        char *dst = p;
        char quote = 0;

        while (*p) {
            if (quote) {
                if (*p == quote) { quote = 0; p++; }
                else             { *dst++ = *p++; }
            } else {
                if (*p == ' ' || *p == '\t') { p++; break; }
                if (*p == '\'' || *p == '"') { quote = *p++; }
                else                         { *dst++ = *p++; }
            }
        }
        *dst = '\0';
        argv[argc++] = tok;
    }
    argv[argc] = (char *)0;
    return argc;
}

#define REDIR_MAX 8

struct redir {
    int  fd;
    int  flags;
    char path[256];
    int  heredoc;
};

static char heredoc_buf[2048];

static int parse_redirs(char *argv[], int *argc_p, struct redir redirs[], int redir_max)
{
    int nredir = 0;
    int out    = 0;
    int argc   = *argc_p;

    for (int i = 0; i < argc; i++) {
        char *tok = argv[i];
        int is_redir = 0;
        struct redir r;
        r.heredoc = 0;

        if (strcmp(tok, ">") == 0 || strcmp(tok, "1>") == 0) {
            if (i + 1 >= argc) { printf("sh: expected filename after >\n"); return -1; }
            r.fd = 1; r.flags = O_WRONLY | O_CREAT | O_TRUNC;
            strncpy(r.path, argv[++i], sizeof(r.path) - 1);
            r.path[sizeof(r.path)-1] = '\0';
            is_redir = 1;
        } else if (strcmp(tok, ">>") == 0 || strcmp(tok, "1>>") == 0) {
            if (i + 1 >= argc) { printf("sh: expected filename after >>\n"); return -1; }
            r.fd = 1; r.flags = O_WRONLY | O_CREAT | O_APPEND;
            strncpy(r.path, argv[++i], sizeof(r.path) - 1);
            r.path[sizeof(r.path)-1] = '\0';
            is_redir = 1;
        } else if (strcmp(tok, "2>") == 0) {
            if (i + 1 >= argc) { printf("sh: expected filename after 2>\n"); return -1; }
            r.fd = 2; r.flags = O_WRONLY | O_CREAT | O_TRUNC;
            strncpy(r.path, argv[++i], sizeof(r.path) - 1);
            r.path[sizeof(r.path)-1] = '\0';
            is_redir = 1;
        } else if (strcmp(tok, "<") == 0) {
            if (i + 1 >= argc) { printf("sh: expected filename after <\n"); return -1; }
            r.fd = 0; r.flags = O_RDONLY;
            strncpy(r.path, argv[++i], sizeof(r.path) - 1);
            r.path[sizeof(r.path)-1] = '\0';
            is_redir = 1;
        } else if (strcmp(tok, "<<") == 0) {
            if (i + 1 >= argc) { printf("sh: expected delimiter after <<\n"); return -1; }
            const char *delim = argv[++i];
            unsigned int pos = 0;
            char linebuf[256];
            unsigned int li;
            char c;
            while (1) {
                printf("> ");
                li = 0;
                while (read(0, &c, 1) == 1) {
                    if (c == '\n') { putchar('\n'); break; }
                    if (c == '\b') {
                        if (li > 0) { li--; printf("\b \b"); }
                        continue;
                    }
                    if (li < sizeof(linebuf) - 1) { linebuf[li++] = c; printf("%c", c); }
                }
                linebuf[li] = '\0';
                if (strcmp(linebuf, delim) == 0) break;
                if (pos + li + 1 < sizeof(heredoc_buf)) {
                    memcpy(heredoc_buf + pos, linebuf, li);
                    pos += li;
                    heredoc_buf[pos++] = '\n';
                }
            }
            heredoc_buf[pos] = '\0';
            r.fd = 0; r.flags = O_RDONLY;
            r.path[0] = '\0';
            r.heredoc = 1;
            is_redir = 1;
        }

        if (is_redir) {
            if (nredir >= redir_max) { printf("sh: too many redirections\n"); return -1; }
            redirs[nredir++] = r;
        } else {
            argv[out++] = tok;
        }
    }

    argv[out] = (char *)0;
    *argc_p   = out;
    return nredir;
}

static int apply_redirs(struct redir redirs[], int nredir)
{
    for (int i = 0; i < nredir; i++) {
        struct redir *r = &redirs[i];

        if (r->heredoc) {
            int pfd[2];
            if (pipe(pfd) < 0) return -1;
            unsigned int hlen = (unsigned int)strlen(heredoc_buf);
            write(pfd[1], heredoc_buf, hlen);
            vfs_close(pfd[1]);
            dup2(pfd[0], r->fd);
            vfs_close(pfd[0]);
        } else {
            char resolved[256];
            resolve_path(r->path, resolved, sizeof(resolved));
            int newfd = open(resolved, r->flags);
            if (newfd < 0) { printf("sh: cannot open '%s'\n", r->path); return -1; }
            dup2(newfd, r->fd);
            vfs_close(newfd);
        }
    }
    return 0;
}

static int split_pipe(char *line, char *segs[], int max_segs)
{
    int n = 0;
    char *p = line;
    segs[n++] = p;

    while (*p && n < max_segs) {
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            while (*p && *p != q) p++;
            if (*p) p++;
        } else if (*p == '|') {
            *p++ = '\0';
            while (*p == ' ' || *p == '\t') p++;
            segs[n++] = p;
        } else {
            p++;
        }
    }
    return n;
}

/* Detect and strip trailing & from the last pipeline segment.
 * Returns 1 if & was present, 0 otherwise. Modifies seg in-place. */
static int strip_background(char *seg)
{
    unsigned int len = (unsigned int)strlen(seg);
    while (len > 0 && (seg[len-1] == ' ' || seg[len-1] == '\t')) len--;
    if (len > 0 && seg[len-1] == '&') {
        seg[len-1] = '\0';
        return 1;
    }
    return 0;
}

/* cd and exit must stay in-process */
static int must_inprocess(const char *cmd)
{
    return strcmp(cmd, "cd")   == 0 ||
           strcmp(cmd, "exit") == 0;
}

static void run_builtin(char *argv[], int argc)
{
    const char *cmd = argv[0];
    (void)argc;

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "echo") == 0) {
        for (int i = 1; argv[i]; i++) {
            if (i > 1) putchar(' ');
            printf("%s", argv[i]);
        }
        putchar('\n');
    } else if (strcmp(cmd, "getpid") == 0) {
        printf("%d\n", getpid());
    } else if (strcmp(cmd, "pwd") == 0) {
        printf("%s\n", cwd);
    } else if (strcmp(cmd, "cd") == 0) {
        cmd_cd(argv[1] ? argv[1] : "");
    } else if (strcmp(cmd, "jobs") == 0) {
        cmd_jobs();
    } else if (strcmp(cmd, "fg") == 0) {
        cmd_fg(argv[1]);
    } else if (strcmp(cmd, "crash") == 0) {
        printf("dereferencing NULL...\n");
        volatile int *p = (volatile int *)0x0;
        (void)*p;
    } else if (strcmp(cmd, "exit") == 0) {
        exit(argv[1] ? atoi(argv[1]) : 0);
    }
}

static int is_builtin(const char *cmd)
{
    return (strcmp(cmd, "help")   == 0 ||
            strcmp(cmd, "echo")   == 0 ||
            strcmp(cmd, "getpid") == 0 ||
            strcmp(cmd, "pwd")    == 0 ||
            strcmp(cmd, "cd")     == 0 ||
            strcmp(cmd, "jobs")   == 0 ||
            strcmp(cmd, "fg")     == 0 ||
            strcmp(cmd, "crash")  == 0 ||
            strcmp(cmd, "exit")   == 0);
}

static int resolve_external(const char *cmd, char *out, unsigned int outsz)
{
    struct vfs_stat st;

    if (cmd[0] == '/' || cmd[0] == '.') {
        resolve_path(cmd, out, outsz);
        return vfs_stat(out, &st) == 0 ? 0 : -1;
    }

    char trypath[256];
    snprintf(trypath, sizeof(trypath), "/bin/%s", cmd);
    if (vfs_stat(trypath, &st) == 0) {
        strncpy(out, trypath, outsz - 1);
        out[outsz - 1] = '\0';
        return 0;
    }
    return -1;
}

static void run_pipeline(char *line)
{
    static char linecopy[256];
    strncpy(linecopy, line, sizeof(linecopy) - 1);
    linecopy[sizeof(linecopy)-1] = '\0';

    char *segs[PIPE_MAX];
    int nsegs = split_pipe(linecopy, segs, PIPE_MAX);

    /* Detect & strip & from the last segment before parsing anything else */
    int background = strip_background(segs[nsegs - 1]);

    int prev_read = -1;
    int pids[PIPE_MAX];
    int npids = 0;

    for (int s = 0; s < nsegs; s++) {
        static char seg_copy[256];
        strncpy(seg_copy, segs[s], sizeof(seg_copy) - 1);
        seg_copy[sizeof(seg_copy)-1] = '\0';

        char *argv[ARGV_MAX + 1];
        int   argc = tokenise(seg_copy, argv, ARGV_MAX);
        if (argc == 0) {
            if (prev_read >= 0) { vfs_close(prev_read); prev_read = -1; }
            continue;
        }

        struct redir redirs[REDIR_MAX];
        int nredir = parse_redirs(argv, &argc, redirs, REDIR_MAX);
        if (nredir < 0) {
            if (prev_read >= 0) { vfs_close(prev_read); prev_read = -1; }
            return;
        }
        if (argc == 0) {
            if (prev_read >= 0) { vfs_close(prev_read); prev_read = -1; }
            continue;
        }

        int pipe_r = -1, pipe_w = -1;
        if (s < nsegs - 1) {
            int pfd[2];
            if (pipe(pfd) < 0) {
                printf("sh: pipe failed\n");
                if (prev_read >= 0) vfs_close(prev_read);
                return;
            }
            pipe_r = pfd[0];
            pipe_w = pfd[1];
        }

        /* cd/exit stay in-process only for single-command, foreground lines */
        int inproc = (nsegs == 1) && !background && must_inprocess(argv[0]);

        if (inproc) {
            if (nredir > 0) {
                int saved[3] = {0, 0, 0};
                for (int i = 0; i < nredir; i++) {
                    int fd = redirs[i].fd;
                    if (fd >= 0 && fd <= 2 && !saved[fd]) {
                        dup2(fd, 30 + fd);
                        saved[fd] = 1;
                    }
                }
                apply_redirs(redirs, nredir);
                run_builtin(argv, argc);
                for (int i = 0; i < 3; i++) {
                    if (saved[i]) { dup2(30 + i, i); vfs_close(30 + i); }
                }
            } else {
                run_builtin(argv, argc);
            }
        } else {
            int pid = fork();
            if (pid == 0) {
                if (prev_read >= 0) { dup2(prev_read, 0); vfs_close(prev_read); }
                if (pipe_w >= 0)    { dup2(pipe_w, 1);    vfs_close(pipe_w); }
                if (pipe_r >= 0)    vfs_close(pipe_r);

                if (apply_redirs(redirs, nredir) < 0) exit(1);

                if (is_builtin(argv[0])) {
                    run_builtin(argv, argc);
                    exit(0);
                }

                char resolved[256];
                if (resolve_external(argv[0], resolved, sizeof(resolved)) < 0) {
                    printf("sh: %s: not found\n", argv[0]);
                    exit(1);
                }
                execv(resolved, argv);
                printf("sh: exec failed: %s\n", resolved);
                exit(1);
            } else if (pid > 0) {
                pids[npids++] = pid;
            } else {
                printf("sh: fork failed\n");
            }
        }

        if (pipe_w >= 0) vfs_close(pipe_w);
        if (prev_read >= 0) vfs_close(prev_read);
        prev_read = pipe_r;
    }

    if (prev_read >= 0) vfs_close(prev_read);

    if (background) {
        /* Store the original command line (before split_pipe modified it) */
        job_add(pids, npids, line);
        return;
    }

    /* Foreground: wait for all pipeline children; Ctrl+C kills only them */
    for (int i = 0; i < npids; i++) {
        while (!task_done(pids[i])) {
            char c;
            if (read_nb(0, &c, 1) > 0 && c == '\x03') {
                for (int j = i; j < npids; j++)
                    kill((unsigned int)pids[j]);
                printf("^C\n");
                return;
            }
            yield();
        }
    }
}

static void dispatch(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;

    /* fg and jobs are in-process builtins that need to run directly */
    char tmp[256];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp)-1] = '\0';
    char *argv[ARGV_MAX + 1];
    int argc = tokenise(tmp, argv, ARGV_MAX);
    if (argc > 0 && (strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "jobs") == 0)) {
        run_builtin(argv, argc);
        return;
    }

    run_pipeline(line);
}

static void run_script(const char *path)
{
    static char sbuf[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("sh: %s: not found\n", path); return; }
    int n = read(fd, sbuf, sizeof(sbuf) - 1);
    vfs_close(fd);
    if (n <= 0) return;
    sbuf[n] = '\0';

    char *p = sbuf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        int has_more = (*p == '\n');
        *p = '\0';
        if (line[0] != '#')
            dispatch(line);
        if (!has_more) break;
        p++;
    }
}

/* CR-reprint: emit \r, prompt, cmd[0..len-1], spaces to erase old tail,
 * then backspaces to place cursor at position 'cursor'.
 * All output is assembled into a single buffer and written in one syscall. */
static void reprint_line(const char *prompt, const char *cmd, int len,
                         int cursor, int prev_len)
{
    char buf[768];
    int pos = 0;
    int i;
    int tail_spaces = prev_len - len;
    int backs = (tail_spaces > 0 ? tail_spaces : 0) + (len - cursor);
    int plen = (int)strlen(prompt);

    buf[pos++] = '\r';
    for (i = 0; i < plen && pos < (int)sizeof(buf) - 1; i++)
        buf[pos++] = prompt[i];
    for (i = 0; i < len && pos < (int)sizeof(buf) - 1; i++)
        buf[pos++] = cmd[i];
    for (i = 0; i < tail_spaces && pos < (int)sizeof(buf) - 1; i++)
        buf[pos++] = ' ';
    for (i = 0; i < backs && pos < (int)sizeof(buf) - 1; i++)
        buf[pos++] = '\b';
    write(1, buf, (unsigned int)pos);
}

int main(int argc, char **argv)
{
    char *history = (char *)malloc(HIST_MAX * HIST_LEN);
    int hist_head = 0;
    int hist_count = 0;
    int hist_idx = -1;    /* -1 = not browsing history */
    char cmd[256];
    int idx = 0;          /* length of current line */
    int cursor = 0;       /* insertion point within cmd */
    char prompt[320];
    /* ANSI escape state machine */
    int esc = 0;          /* 0=normal, 1=got ESC, 2=got ESC [ */
    char csi_buf[8];
    int csi_len = 0;

    getcwd(cwd, sizeof(cwd));

    if (argc >= 2) {
        char resolved[256];
        resolve_path(argv[1], resolved, sizeof(resolved));
        run_script(resolved);
        return 0;
    }

    printf("Luminary shell\nType 'help' for commands.\n\n");
    job_reap();
    snprintf(prompt, sizeof(prompt), "%s $ ", cwd);
    printf("%s", prompt);

    for (;;) {
        char c;
        int prev_len;
        int i;

        if (read(0, &c, 1) == 0)
            continue;

        /* ANSI escape state machine */
        if (esc == 1) {
            if (c == '[') { esc = 2; csi_len = 0; continue; }
            esc = 0;
            /* unrecognised ESC sequence — ignore */
            continue;
        }
        if (esc == 2) {
            if (csi_len < (int)(sizeof(csi_buf) - 1))
                csi_buf[csi_len++] = c;
            /* Final byte is in range 0x40-0x7E */
            if (c >= 0x40 && c <= 0x7E) {
                csi_buf[csi_len] = '\0';
                esc = 0;

                if (c == 'A') {         /* Up — prev history */
                    if (hist_count == 0) continue;
                    if (hist_idx < 0) hist_idx = hist_count;
                    if (hist_idx > 0) hist_idx--;
                    prev_len = idx;
                    int hslot = (hist_head + hist_idx) % HIST_MAX;
                    int newlen = 0;
                    while ((history + hslot * HIST_LEN)[newlen]) newlen++;
                    for (i = 0; i < newlen; i++) cmd[i] = (history + hslot * HIST_LEN)[i];
                    idx = newlen; cursor = newlen;
                    reprint_line(prompt, cmd, idx, cursor, prev_len);
                } else if (c == 'B') {  /* Down — next history / blank */
                    if (hist_idx < 0) continue;
                    hist_idx++;
                    prev_len = idx;
                    if (hist_idx >= hist_count) {
                        hist_idx = -1;
                        idx = 0; cursor = 0;
                        reprint_line(prompt, cmd, 0, 0, prev_len);
                    } else {
                        int hslot = (hist_head + hist_idx) % HIST_MAX;
                        int newlen = 0;
                        while ((history + hslot * HIST_LEN)[newlen]) newlen++;
                        for (i = 0; i < newlen; i++) cmd[i] = (history + hslot * HIST_LEN)[i];
                        idx = newlen; cursor = newlen;
                        reprint_line(prompt, cmd, idx, cursor, prev_len);
                    }
                } else if (c == 'D') {  /* Left */
                    if (cursor > 0) { cursor--; putchar('\b'); }
                } else if (c == 'C') {  /* Right */
                    if (cursor < idx) { putchar(cmd[cursor]); cursor++; }
                } else if (c == 'H') {  /* Home */
                    while (cursor > 0) { putchar('\b'); cursor--; }
                } else if (c == 'F') {  /* End */
                    while (cursor < idx) { putchar(cmd[cursor]); cursor++; }
                } else if (csi_len >= 2 && csi_buf[0] == '3' && c == '~') { /* Delete */
                    if (cursor < idx) {
                        prev_len = idx;
                        for (i = cursor; i < idx - 1; i++) cmd[i] = cmd[i + 1];
                        idx--;
                        /* reprint from cursor to end, erase last char, reposition */
                        for (i = cursor; i < idx; i++) putchar(cmd[i]);
                        putchar(' ');
                        for (i = cursor; i < idx + 1; i++) putchar('\b');
                        (void)prev_len;
                    }
                }
            }
            continue;
        }

        /* Normal character processing */
        if (c == '\x1b') {
            esc = 1;
            continue;
        }

        if (c == '\x03') {              /* Ctrl+C */
            printf("^C\n");
            idx = 0; cursor = 0; hist_idx = -1;
            job_reap();
            snprintf(prompt, sizeof(prompt), "%s $ ", cwd);
            printf("%s", prompt);
            continue;
        }

        if (c == '\n') {
            putchar('\n');
            cmd[idx] = '\0';
            /* Push to history if non-empty and not duplicate of last */
            if (idx > 0) {
                int is_dup = 0;
                if (hist_count > 0) {
                    int last = (hist_head + hist_count - 1) % HIST_MAX;
                    int j;
                    is_dup = 1;
                    for (j = 0; j <= idx; j++) {
                        if ((history + last * HIST_LEN)[j] != cmd[j]) { is_dup = 0; break; }
                    }
                }
                if (!is_dup) {
                    int slot;
                    if (hist_count < HIST_MAX) {
                        slot = (hist_head + hist_count) % HIST_MAX;
                        hist_count++;
                    } else {
                        slot = hist_head;
                        hist_head = (hist_head + 1) % HIST_MAX;
                    }
                    for (i = 0; i <= idx; i++) (history + slot * HIST_LEN)[i] = cmd[i];
                }
            }
            hist_idx = -1;
            dispatch(cmd);
            idx = 0; cursor = 0;
            job_reap();
            snprintf(prompt, sizeof(prompt), "%s $ ", cwd);
            printf("%s", prompt);
            continue;
        }

        if (c == '\b' || c == '\x7f') { /* Backspace */
            if (cursor > 0) {
                prev_len = idx;
                cursor--;
                for (i = cursor; i < idx - 1; i++) cmd[i] = cmd[i + 1];
                idx--;
                putchar('\b');
                for (i = cursor; i < idx; i++) putchar(cmd[i]);
                putchar(' ');
                for (i = cursor; i < idx + 1; i++) putchar('\b');
            }
            continue;
        }

        /* Sentinel bytes from fbdev console (non-ANSI path) */
        if ((unsigned char)c == 0x10) { /* KEY_UP sentinel */
            /* treat as ANSI up — synthesise */
            char fake[] = "\x1b[A";
            int fi;
            for (fi = 0; fake[fi]; fi++) {
                char fc = fake[fi];
                /* re-inject by pushing directly: recurse via goto would be
                 * messy, so just duplicate the history-up logic */
                (void)fc;
            }
            /* Simpler: just inline the up-history logic */
            if (hist_count > 0) {
                if (hist_idx < 0) hist_idx = hist_count;
                if (hist_idx > 0) hist_idx--;
                prev_len = idx;
                int hslot = (hist_head + hist_idx) % HIST_MAX;
                int newlen = 0;
                while ((history + hslot * HIST_LEN)[newlen]) newlen++;
                for (i = 0; i < newlen; i++) cmd[i] = (history + hslot * HIST_LEN)[i];
                idx = newlen; cursor = newlen;
                reprint_line(prompt, cmd, idx, cursor, prev_len);
            }
            continue;
        }
        if ((unsigned char)c == 0x11) { /* KEY_DOWN sentinel */
            if (hist_idx >= 0) {
                hist_idx++;
                prev_len = idx;
                if (hist_idx >= hist_count) {
                    hist_idx = -1;
                    idx = 0; cursor = 0;
                    reprint_line(prompt, cmd, 0, 0, prev_len);
                } else {
                    int hslot = (hist_head + hist_idx) % HIST_MAX;
                    int newlen = 0;
                    while ((history + hslot * HIST_LEN)[newlen]) newlen++;
                    for (i = 0; i < newlen; i++) cmd[i] = (history + hslot * HIST_LEN)[i];
                    idx = newlen; cursor = newlen;
                    reprint_line(prompt, cmd, idx, cursor, prev_len);
                }
            }
            continue;
        }
        if ((unsigned char)c == 0x12) { /* KEY_LEFT sentinel */
            if (cursor > 0) { cursor--; putchar('\b'); }
            continue;
        }
        if ((unsigned char)c == 0x13) { /* KEY_RIGHT sentinel */
            if (cursor < idx) { putchar(cmd[cursor]); cursor++; }
            continue;
        }
        if ((unsigned char)c == 0x14) { /* KEY_HOME sentinel */
            while (cursor > 0) { putchar('\b'); cursor--; }
            continue;
        }
        if ((unsigned char)c == 0x15) { /* KEY_END sentinel */
            while (cursor < idx) { putchar(cmd[cursor]); cursor++; }
            continue;
        }
        if ((unsigned char)c == 0x16) { /* KEY_DEL sentinel */
            if (cursor < idx) {
                for (i = cursor; i < idx - 1; i++) cmd[i] = cmd[i + 1];
                idx--;
                for (i = cursor; i < idx; i++) putchar(cmd[i]);
                putchar(' ');
                for (i = cursor; i < idx + 1; i++) putchar('\b');
            }
            continue;
        }

        /* Ignore other control bytes */
        if ((unsigned char)c < 32)
            continue;

        /* Insert printable character at cursor */
        if (idx < (int)(sizeof(cmd) - 1)) {
            for (i = idx; i > cursor; i--) cmd[i] = cmd[i - 1];
            cmd[cursor] = c;
            idx++;
            /* Print char + suffix, then reposition */
            for (i = cursor; i < idx; i++) putchar(cmd[i]);
            cursor++;
            for (i = cursor; i < idx; i++) putchar('\b');
        }
    }
}
