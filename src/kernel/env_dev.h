/* env_dev.h — /dev/env ioctl request codes and argument struct.
 *
 * Userland mirrors these in userland/env_dev.h.
 */
#pragma once

#include <stdint.h>

#define ENV_GET     1   /* arg: struct env_op * — get value by name */
#define ENV_SET     2   /* arg: struct env_op * — set name=val */
#define ENV_UNSET   3   /* arg: struct env_op * — unset by name */
#define ENV_GET_IDX 4   /* arg: struct env_op * — get NAME=VAL by index */

struct env_op {
    int  index;      /* ENV_GET_IDX: input index */
    int  overwrite;  /* ENV_SET: 0 = do not overwrite existing */
    char name[64];
    char val[128];
};

void init_dev_env(void);
