/* Luminary init (PID 1) - supervisor only.
 * Forks and execs /bin/sh, waits for it, and respawns on death. */

#include "syscall.h"
#include "libc/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (;;) {
        int pid = fork();
        if (pid == 0) {
            /* child: become the shell */
            char *sh_argv[] = { "/bin/sh", (char *)0 };
            puts("init: starting /bin/sh\n");
            execv("/bin/sh", sh_argv);
            exit(1);
        } else if (pid > 0) {
            waitpid(pid);
        }
        /* shell died, loop and respawn */
    }
}
