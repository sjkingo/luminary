/* stat — show file information */

#include "syscall.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: stat <path>\n");
        return 1;
    }

    struct vfs_stat st;
    if (vfs_stat(argv[1], &st) < 0) {
        printf("stat: not found: %s\n", argv[1]);
        return 1;
    }
    printf("%s: %s, %u bytes\n", argv[1],
           (st.type & VFS_DIR) ? "directory" : "file", st.size);
    return 0;
}
