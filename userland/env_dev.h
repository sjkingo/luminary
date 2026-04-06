/* env_dev.h — userland /dev/env ioctl API for environment variables. */
#pragma once

#include "syscall.h"

#define ENV_GET     1
#define ENV_SET     2
#define ENV_UNSET   3
#define ENV_GET_IDX 4

struct env_op {
    int  index;
    int  overwrite;
    char name[64];
    char val[128];
};

/* Open /dev/env once; fd is cached for the process lifetime. */
static inline int env_fd_get(void)
{
    static int efd = -1;
    if (efd < 0) efd = open("/dev/env", O_RDONLY);
    return efd;
}

static inline int getenv_r(const char *name, char *buf, unsigned int buflen)
{
    int fd = env_fd_get();
    if (fd < 0) return -1;
    struct env_op op;
    op.index = 0; op.overwrite = 0;
    unsigned int i = 0;
    while (name[i] && i < sizeof(op.name) - 1) { op.name[i] = name[i]; i++; }
    op.name[i] = '\0';
    op.val[0] = '\0';
    if (ioctl(fd, ENV_GET, &op) < 0) return -1;
    unsigned int j = 0;
    while (op.val[j] && j < buflen - 1) { buf[j] = op.val[j]; j++; }
    buf[j] = '\0';
    return 0;
}

static inline int setenv(const char *name, const char *val, int overwrite)
{
    int fd = env_fd_get();
    if (fd < 0) return -1;
    struct env_op op;
    op.index = 0; op.overwrite = overwrite;
    unsigned int i = 0;
    while (name[i] && i < sizeof(op.name) - 1) { op.name[i] = name[i]; i++; }
    op.name[i] = '\0';
    i = 0;
    while (val[i] && i < sizeof(op.val) - 1) { op.val[i] = val[i]; i++; }
    op.val[i] = '\0';
    return ioctl(fd, ENV_SET, &op);
}

static inline int unsetenv(const char *name)
{
    int fd = env_fd_get();
    if (fd < 0) return -1;
    struct env_op op;
    op.index = 0; op.overwrite = 0;
    unsigned int i = 0;
    while (name[i] && i < sizeof(op.name) - 1) { op.name[i] = name[i]; i++; }
    op.name[i] = '\0';
    op.val[0] = '\0';
    return ioctl(fd, ENV_UNSET, &op);
}

/* Returns 0 and fills buf with "NAME=VAL", or -1 if index is out of range. */
static inline int getenv_by_index(int idx, char *buf, unsigned int buflen)
{
    int fd = env_fd_get();
    if (fd < 0) return -1;
    struct env_op op;
    op.index = idx; op.overwrite = 0;
    op.name[0] = '\0'; op.val[0] = '\0';
    if (ioctl(fd, ENV_GET_IDX, &op) < 0) return -1;
    unsigned int i = 0;
    while (op.val[i] && i < buflen - 1) { buf[i] = op.val[i]; i++; }
    buf[i] = '\0';
    return 0;
}
