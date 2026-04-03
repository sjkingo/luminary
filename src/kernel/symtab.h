#pragma once
#include <stdint.h>

struct ksym {
    uint32_t    addr;
    const char *name;
};

/* Returns the closest symbol at or before addr, or NULL if none. */
const struct ksym *ksym_lookup(uint32_t addr);
