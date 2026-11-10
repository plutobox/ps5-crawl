/* Crawl for PS5: process entry and exit.
 *
 * The link wraps the game's main() (--wrap=main) so this runs first, and
 * every exit() (--wrap=exit): a title can't exit (exit() raises SIGSYS), so
 * the title asks the system to close it instead. The game's own code keeps
 * its usual main() and exit() calls.
 *
 * Follows Apotris-PS5 (ps5port/app/ps5_entry.c, GPL-3.0-or-later).
 */
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_PATH "/data/crawl-ps5.log"
#define SAVE_ROOT "/download0/crawl"

int __real_main(int argc, char **argv, char **envp);
int sceSystemServiceLoadExec(const char *path, const char **arguments);
int sceKernelUsleep(unsigned int microseconds);

/* LoadExec "exit" ends the title as closing it from the home screen does. It
 * is asked again until the shell takes the title down. */
static void __attribute__((noreturn)) close_title(int status)
{
    printf("Crawl has exited (status %d); closing the title.\n", status);
    fflush(NULL);
    for (;;)
    {
        const int result = sceSystemServiceLoadExec("exit", NULL);
        if (result)
            printf("sceSystemServiceLoadExec(\"exit\"): 0x%08x\n", (unsigned) result);
        sceKernelUsleep(1000000);
    }
}

void __wrap_exit(int status)
{
    close_title(status);
}

/* The startup code calls this when main() returns, before exit(). */
void catchReturnFromMain(int status)
{
    close_title(status);
}

int __wrap_main(int argc, char **argv, char **envp)
{
    /* A title has no console: its output goes to a file that can be read
     * back over FTP. Unbuffered, so a crash loses nothing. */
    if (freopen(LOG_PATH, "w", stdout))
    {
        setvbuf(stdout, NULL, _IONBF, 0);
        dup2(fileno(stdout), fileno(stderr));
    }
    printf("Dungeon Crawl Stone Soup starting\n");

    /* /download0 is the title's writable storage (param.json
     * downloadDataSize); Crawl's saves and settings go to SAVE_ROOT. */
    mkdir(SAVE_ROOT, 0777);

    return __real_main(argc, argv, envp);
}
