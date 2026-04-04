/* ls — list directory contents */

#include "syscall.h"
#include "libc/stdio.h"
#include "libc/string.h"

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : ".";

    int fd = vfs_open(path);
    if (fd < 0) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }

    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0 && (st.type & VFS_FILE)) {
        printf("%s\n", path);
        vfs_close(fd);
        return 0;
    }

    struct vfs_dirent de;
    int first = 1;
    while (vfs_readdir(fd, &de) == 1) {
        if (!first)
            printf("\t");
        printf("%s%s", de.name, (de.type & VFS_DIR) ? "/" : "");
        first = 0;
    }
    if (!first)
        printf("\n");
    vfs_close(fd);
    return 0;
}
