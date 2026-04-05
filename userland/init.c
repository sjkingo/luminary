/* Luminary init (PID 1) - supervisor only.
 * Forks and execs /bin/sh, waits for it, and respawns on death. */

#include "syscall.h"
#include "libc/stdio.h"

#define INIT_CHILD "/bin/sh"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (;;) {
        int pid = fork();
        if (pid == 0) {
            /* child: become the shell */
            char *sh_argv[] = { INIT_CHILD, (char *)0 };
            printf("init: starting " INIT_CHILD " as pid %d\n\n", getpid());
            execv(INIT_CHILD, sh_argv);
            exit(1);
        } else if (pid > 0) {
            waitpid(pid, NULL, 0);
        }
        /* shell died, loop and respawn */
        printf("init: " INIT_CHILD " (pid %d) died, respawning\n\n", pid);
    }
}
