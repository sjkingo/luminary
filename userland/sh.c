/* Luminary shell.
 *
 * Interactive REPL with pipeline support (|) and I/O redirection
 * (>, >>, <, <<).
 *
 * Built-in commands (run in-process): help, echo, getpid, exec, spawn, cd,
 * pwd, crash.  Everything else is looked up in /bin and fork+exec'd.
 *
 * cd is the only command that truly must run in-process (it modifies cwd).
 * The others are built-in as a convenience to avoid process overhead for
 * common operations.  When any built-in appears with I/O redirections it is
 * still forked (except cd, which saves/restores fds around the redirect).
 *
 * Redirection parsing strips >, >>, <, << tokens from argv before dispatch.
 * Pipeline stages are connected with kernel pipes; each stage is forked.
 */

#include "syscall.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"

/* ── working directory ───────────────────────────────────────────────────── */

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

/* ── built-in command implementations ───────────────────────────────────── */

static void cmd_help(void)
{
    printf("built-in commands:\n"
           "  help           - show this message\n"
           "  echo <text>    - print text\n"
           "  getpid         - show current PID\n"
           "  exec <path>    - execute ELF (fork+exec, wait for completion)\n"
           "  spawn <path>   - spawn ELF as background task\n"
           "  cd <path>      - change directory\n"
           "  pwd            - print working directory\n"
           "  crash          - dereference a null pointer\n"
           "\n"
           "external commands (in /bin): uptime ps kill ls cat stat mount halt exit\n");
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

/* ── tokeniser ───────────────────────────────────────────────────────────── */

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

        argv[argc++] = p;

        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') p++;
                if (*p == '\'') p++;
            } else {
                p++;
            }
        }
        if (*p) { *p = '\0'; p++; }
    }
    argv[argc] = (char *)0;
    return argc;
}

/* ── redirection ─────────────────────────────────────────────────────────── */

#define REDIR_MAX 8

struct redir {
    int  fd;
    int  flags;     /* O_* flags */
    char path[256]; /* target file, or '\0' for heredoc */
    int  heredoc;   /* 1 if << was used */
};

/* Heredoc content lives here; only one heredoc per command line supported. */
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
                write(1, "> ", 2);
                li = 0;
                while (read(0, &c, 1) == 1) {
                    if (c == '\n') { putchar('\n'); break; }
                    if (c == '\b') {
                        if (li > 0) { li--; write(1, "\b \b", 3); }
                        continue;
                    }
                    if (li < sizeof(linebuf) - 1) { linebuf[li++] = c; write(1, &c, 1); }
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

/* ── pipeline builder ────────────────────────────────────────────────────── */

#define PIPE_MAX 8

static int split_pipe(char *line, char *segs[], int max_segs)
{
    int n = 0;
    char *p = line;
    segs[n++] = p;

    while (*p && n < max_segs) {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') p++;
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

/* cd is the only command that truly must stay in-process */
static int must_inprocess(const char *cmd)
{
    return strcmp(cmd, "cd") == 0;
}

/* Run a built-in command with the current fd table already set up. */
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
    } else if (strcmp(cmd, "exec") == 0) {
        if (!argv[1]) { printf("usage: exec <path>\n"); return; }
        char resolved[256];
        resolve_path(argv[1], resolved, sizeof(resolved));
        int pid = fork();
        if (pid == 0) { execv(resolved, argv + 1); exit(1); }
        else if (pid > 0) waitpid(pid);
        else printf("exec: fork failed\n");
    } else if (strcmp(cmd, "spawn") == 0) {
        if (!argv[1]) { printf("usage: spawn <path>\n"); return; }
        char resolved[256];
        resolve_path(argv[1], resolved, sizeof(resolved));
        int pid = spawn(resolved);
        if (pid < 0) printf("spawn: failed: %s\n", resolved);
        else printf("spawn: launched pid %d\n", pid);
    } else if (strcmp(cmd, "crash") == 0) {
        printf("dereferencing NULL...\n");
        volatile int *p = (volatile int *)0x0;
        (void)*p;
    }
}

/* Is this command a shell built-in? */
static int is_builtin(const char *cmd)
{
    return (strcmp(cmd, "help")   == 0 ||
            strcmp(cmd, "echo")   == 0 ||
            strcmp(cmd, "getpid") == 0 ||
            strcmp(cmd, "pwd")    == 0 ||
            strcmp(cmd, "cd")     == 0 ||
            strcmp(cmd, "exec")   == 0 ||
            strcmp(cmd, "spawn")  == 0 ||
            strcmp(cmd, "crash")  == 0);
}

/* Resolve an external command to its full path.
 * Tries the literal path first, then /bin/<cmd>. */
static int resolve_external(const char *cmd, char *out, unsigned int outsz)
{
    struct vfs_stat st;

    /* Absolute or relative path given explicitly */
    if (cmd[0] == '/' || cmd[0] == '.') {
        resolve_path(cmd, out, outsz);
        return vfs_stat(out, &st) == 0 ? 0 : -1;
    }

    /* Search /bin */
    char trypath[256];
    snprintf(trypath, sizeof(trypath), "/bin/%s", cmd);
    if (vfs_stat(trypath, &st) == 0) {
        strncpy(out, trypath, outsz - 1);
        out[outsz - 1] = '\0';
        return 0;
    }
    return -1;
}

/* ── pipeline executor ───────────────────────────────────────────────────── */

static void run_pipeline(char *line)
{
    static char linecopy[256];
    strncpy(linecopy, line, sizeof(linecopy) - 1);
    linecopy[sizeof(linecopy)-1] = '\0';

    char *segs[PIPE_MAX];
    int nsegs = split_pipe(linecopy, segs, PIPE_MAX);

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

        /* cd stays in-process; no fork unless it's in a pipeline */
        int inproc = (nsegs == 1) && must_inprocess(argv[0]);

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
                if (pipe_w >= 0) { dup2(pipe_w, 1); vfs_close(pipe_w); }
                if (pipe_r >= 0) vfs_close(pipe_r);

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

    /* Wait for all pipeline children, but allow Ctrl+C to kill them. */
    for (int i = 0; i < npids; i++) {
        while (!task_done(pids[i])) {
            char c;
            if (read_nb(0, &c, 1) > 0 && c == '\x03') {
                /* Ctrl+C: kill remaining pipeline children and abort wait */
                for (int j = i; j < npids; j++)
                    kill((unsigned int)pids[j]);
                write(1, "^C\n", 3);
                return;
            }
            yield();
        }
    }
}

/* ── line dispatcher ─────────────────────────────────────────────────────── */

static void dispatch(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;
    run_pipeline(line);
}

/* ── entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    static char cmd[256];
    int idx = 0;
    char c;

    printf("Luminary shell\nType 'help' for commands.\n\n$ ");

    for (;;) {
        if (read(0, &c, 1) == 0)
            continue;

        if (c == '\x03') {
            /* Ctrl+C: discard current line, print ^C and new prompt */
            write(1, "^C\n$ ", 5);
            idx = 0;
        } else if (c == '\n') {
            putchar('\n');
            cmd[idx] = '\0';
            dispatch(cmd);
            idx = 0;
            printf("$ ");
        } else if (c == '\b') {
            if (idx > 0) {
                idx--;
                write(1, "\b \b", 3);
            }
        } else {
            if (idx < (int)(sizeof(cmd) - 1)) {
                cmd[idx++] = c;
                write(1, &c, 1);
            }
        }
    }
}
