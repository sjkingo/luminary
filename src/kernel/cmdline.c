/* Kernel command line parser. */

#include <string.h>
#include "kernel/cmdline.h"
#include "kernel/kernel.h"

#define CMDLINE_MAX  256
#define PARAM_MAX     16

#define MODULE "cmdline: "

static char raw_buf[CMDLINE_MAX];  /* original, unmodified */
static char parse_buf[CMDLINE_MAX]; /* working copy, NUL-patched by parser */

static struct {
    char *key;
    char *value;
} params[PARAM_MAX];

static int param_count = 0;

void cmdline_init(const char *cmdline_str)
{
    if (!cmdline_str || cmdline_str[0] == '\0') {
        raw_buf[0] = '\0';
        DBGK("no cmdline\n");
        return;
    }

    strncpy(raw_buf, cmdline_str, CMDLINE_MAX - 1);
    raw_buf[CMDLINE_MAX - 1] = '\0';
    strncpy(parse_buf, raw_buf, CMDLINE_MAX);

    DBGK("cmdline: %s\n", raw_buf);

    /* Parse in-place on working copy: split on spaces, split each token on '=' */
    char *p = parse_buf;
    while (*p && param_count < PARAM_MAX) {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        char *tok = p;
        while (*p && *p != ' ') p++;
        if (*p == ' ') *p++ = '\0';

        char *eq = tok;
        while (*eq && *eq != '=') eq++;

        params[param_count].key = tok;
        if (*eq == '=') {
            *eq = '\0';
            params[param_count].value = eq + 1;
        } else {
            params[param_count].value = eq; /* points to '\0' — empty string */
        }
        param_count++;
    }
}

const char *cmdline_get(const char *key)
{
    for (int i = 0; i < param_count; i++) {
        if (strcmp(params[i].key, key) == 0)
            return params[i].value;
    }
    return NULL;
}

const char *cmdline_raw(void)
{
    return raw_buf;
}
