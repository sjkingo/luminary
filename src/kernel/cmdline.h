#pragma once

/* Kernel command line parser.
 *
 * Parses the multiboot cmdline string (e.g. "root=/dev/hda1 quiet") into
 * key=value pairs. Keys without a value (bare flags) have value "".
 * Call cmdline_init() once early in kernel_main before any other subsystem
 * that may need cmdline parameters. */

/* Initialise from the multiboot cmdline string. Safe to call with NULL. */
void        cmdline_init(const char *cmdline_str);

/* Return the value for key, or NULL if the key is not present.
 * For bare flags (e.g. "quiet") returns "". */
const char *cmdline_get(const char *key);

/* Return the full raw cmdline string (empty string if none was provided). */
const char *cmdline_raw(void);
