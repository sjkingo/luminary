/* Kernel symbol table — binary search over generated ksym_table[].
 *
 * symtab_gen.c is produced by tools/gen_symtab.sh from nm output during the
 * build.  It provides ksym_table[] sorted ascending by address and ksym_count.
 */

#include "kernel/symtab.h"

/* Provided by the generated file symtab_gen.c */
extern const struct ksym ksym_table[];
extern const uint32_t    ksym_count;

const struct ksym *ksym_lookup(uint32_t addr)
{
    if (ksym_count == 0)
        return 0;

    uint32_t lo = 0, hi = ksym_count - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (ksym_table[mid].addr <= addr)
            lo = mid;
        else
            hi = mid - 1;
    }

    if (ksym_table[lo].addr <= addr)
        return &ksym_table[lo];
    return 0;
}
