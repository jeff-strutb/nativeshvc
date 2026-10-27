/*
 * mark.c: nshvc_mark, checked on a cartridge of a few lines of C: with
 * NSHVC_CYCLOG its M lines land in the stream between the lines of the
 * accesses around them; with or without it, no time passes.
 *
 *   mark              runs the checks; exit 0 when every one holds
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

extern uint64_t snes_clock;

static void abi_reset(void)
{
    uint64_t t0;
    int bad = 0;
    wr8(0x004200, 0x00);
    wr8(0x7E1000, 0x11);
    t0 = snes_clock;
    nshvc_mark(7);
    nshvc_mark(4000000000u);
    bad |= snes_clock != t0;
    cyc(12);
    nshvc_mark(8);
    wr8(0x7E1001, 0x22);
    exit(bad ? 3 : 0);
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, abi_reset, "reset", 0 } };

/* the cartridge run in a child (its cyclog is written when it exits): its
   exit status */
static int run(const char *cyclog)
{
    int st;
    pid_t p;
    fflush(stdout);
    p = fork();
    if (p == 0) {
        nshvc_cart cart;
        if (cyclog)
            setenv("NSHVC_CYCLOG", cyclog, 1);
        else
            unsetenv("NSHVC_CYCLOG");
        memset(&cart, 0, sizeof cart);
        cart.title = "mark";
        cart.map = NSHVC_LOROM;
        cart.rom = rom;
        cart.rom_size = sizeof rom;
        cart.routines = routines;
        cart.routine_count = 1;
        nshvc_power(&cart);
        for (;;)
            nshvc_run_frame();
    }
    if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

int main(void)
{
    char path[] = "/tmp/nshvc_markXXXXXX", line[256], got[1024] = "";
    int fd = mkstemp(path), fails = 0, n;
    FILE *f;
    if (fd < 0)
        return 2;
    close(fd);
    n = run(NULL);
    printf("%s no cyclog: a mark takes no time (exit %d)\n", n == 0 ? "ok  " : "FAIL", n);
    fails += n != 0;
    n = run(path);
    printf("%s cyclog: a mark takes no time (exit %d)\n", n == 0 ? "ok  " : "FAIL", n);
    fails += n != 0;
    /* the stream from the first write: W, M 7, M 4000000000, C, M 8, W */
    f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "W 7E1000", 8))
            got[0] = 0;
        char tok[32];
        unsigned long id;
        if (line[0] == 'M' && sscanf(line, "M %lu", &id) == 1)
            snprintf(tok, sizeof tok, "M%lu,", id);
        else if (line[0] == 'W' || line[0] == 'C')
            snprintf(tok, sizeof tok, "%c,", line[0]);
        else
            continue;
        strncat(got, tok, sizeof got - strlen(got) - 1);
    }
    if (f)
        fclose(f);
    unlink(path);
    n = !strcmp(got, "W,M7,M4000000000,C,M8,W,");
    printf("%s cyclog: the marks in stream order (%s)\n", n ? "ok  " : "FAIL", got);
    fails += !n;
    printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
