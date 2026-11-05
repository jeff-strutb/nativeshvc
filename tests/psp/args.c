/*
 * args.c: a test program's arguments on the PSP, which starts a program
 * with none: args.txt beside the program, an argument a line ("env
 * NAME=VALUE" lines set the environment instead), then the test's own main
 * (built as nshvc_test_main on the PSP). At exit, the time from here, by
 * the system's clock ("ran N.NNN ms").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspthreadman.h>

#undef main
int nshvc_test_main(int argc, char **argv);
/* (tests/ref/check.c: its hooks, read again from the environment set here,
   after the constructors; a program without the reference has none) */
void nshvc_check_env(void) __attribute__((weak));

static SceInt64 s_start;

static void ran(void)
{
    printf("ran %.3f ms\n", (double)(sceKernelGetSystemTimeWide() - s_start) / 1000.0);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    static char *av[64];
    static char lines[64][512];
    char path[512];
    int n = 0;
    FILE *f;
    av[n++] = argc > 0 ? argv[0] : (char *)"test";
    snprintf(path, sizeof path, "%s", av[0]);
    if (strrchr(path, '/'))
        strcpy(strrchr(path, '/') + 1, "args.txt");
    else
        snprintf(path, sizeof path, "args.txt");
    if ((f = fopen(path, "r"))) {
        while (n < 63 && fgets(lines[n], sizeof lines[n], f)) {
            char *l = lines[n];
            l[strcspn(l, "\r\n")] = 0;
            if (!l[0])
                continue;
            if (!strncmp(l, "env ", 4)) {
                char *eq = strchr(l + 4, '=');
                if (eq) {
                    *eq = 0;
                    setenv(l + 4, eq + 1, 1);
                }
                continue;
            }
            av[n] = l;
            n++;
        }
        fclose(f);
    }
    av[n] = NULL;
    if (nshvc_check_env)
        nshvc_check_env();
    s_start = sceKernelGetSystemTimeWide();
    atexit(ran);
    return nshvc_test_main(n, av);
}
