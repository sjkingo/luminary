/* ls — list directory contents */

#include "syscall.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "fb_dev.h"

#define MAX_ENTRIES 256

/* Return a pointer to the basename of path (no allocation). */
static const char *basename(const char *path)
{
    const char *p = path;
    const char *last = path;
    while (*p) {
        if (*p == '/' && *(p + 1) != '\0')
            last = p + 1;
        p++;
    }
    return last;
}

static int term_cols(void)
{
    struct fb_info fbi;
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) return 80;
    int ok = fb_get_info(fbfd, &fbi);
    vfs_close(fbfd);
    if (ok < 0) return 80;
    int cols = (int)(fbi.width / 8); /* 8px-wide font */
    return cols > 0 ? cols : 80;
}

/* Collect and print directory entries in tab-separated columns. */
static void ls_dir(const char *path, int print_header)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        printf("ls: cannot open '%s'\n", path);
        return;
    }

    if (print_header)
        printf("%s:\n", path);

    /* Collect names into a heap buffer so we can column-format them. */
    char  *names[MAX_ENTRIES];
    char   types[MAX_ENTRIES];
    int    count = 0;

    struct vfs_dirent de;
    while (vfs_readdir(fd, &de) == 1 && count < MAX_ENTRIES) {
        unsigned int nlen = 0;
        while (de.name[nlen]) nlen++;
        char *s = (char *)malloc(nlen + 2); /* +2 for possible '/' and NUL */
        if (!s) break;
        for (unsigned int i = 0; i < nlen; i++) s[i] = de.name[i];
        s[nlen] = '\0';
        names[count] = s;
        types[count] = (de.type & VFS_DIR) ? 1 : 0;
        count++;
    }
    vfs_close(fd);

    /* Simple insertion sort by name */
    for (int i = 1; i < count; i++) {
        char *kn = names[i];
        char  kt = types[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], kn) > 0) {
            names[j + 1] = names[j];
            types[j + 1] = types[j];
            j--;
        }
        names[j + 1] = kn;
        types[j + 1] = kt;
    }

    /* Find longest name to compute column width */
    int maxlen = 0;
    for (int i = 0; i < count; i++) {
        int len = (int)strlen(names[i]) + types[i]; /* +1 if trailing / */
        if (len > maxlen) maxlen = len;
    }
    int colw = maxlen + 2; /* 2-space gap */

    int ncols = term_cols() / colw;
    if (ncols < 1) ncols = 1;

    for (int i = 0; i < count; i++) {
        int len = (int)strlen(names[i]) + types[i];
        printf("%s%s", names[i], types[i] ? "/" : "");
        if ((i + 1) % ncols == 0 || i == count - 1) {
            printf("\n");
        } else {
            /* pad to column width */
            for (int p = len; p < colw; p++) putchar(' ');
        }
        free(names[i]);
    }
}

int main(int argc, char **argv)
{
    if (argc <= 1) {
        ls_dir(".", 0);
        return 0;
    }

    /* Count how many arguments are directories vs plain files */
    int ndirs = 0;
    for (int i = 1; i < argc; i++) {
        struct vfs_stat st;
        if (vfs_stat(argv[i], &st) == 0 && (st.type & VFS_DIR))
            ndirs++;
    }

    int multi = (ndirs > 1) || (ndirs == 1 && argc > 2);
    int first = 1;

    for (int i = 1; i < argc; i++) {
        struct vfs_stat st;
        if (vfs_stat(argv[i], &st) < 0) {
            printf("ls: cannot stat '%s'\n", argv[i]);
            continue;
        }

        if (st.type & VFS_DIR) {
            if (!first) printf("\n");
            ls_dir(argv[i], multi);
            first = 0;
        } else {
            printf("%s\n", basename(argv[i]));
            first = 0;
        }
    }

    return 0;
}
