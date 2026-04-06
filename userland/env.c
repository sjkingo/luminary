#include "env_dev.h"
#include "libc/stdio.h"

int main(void)
{
    char buf[128];
    for (int i = 0; getenv_by_index(i, buf, sizeof(buf)) == 0; i++)
        printf("%s\n", buf);
    return 0;
}
