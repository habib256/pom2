/* ARCH-017's acceptance program: two lines, an assertion, exit 3.
 * Built with cc65 2.19, from tests/testport/:
 *     cl65 -t apple2 -g -O -I ../../tools/testport -Wl --dbgfile,hello.dbg \
 *         -o hello.as hello.c
 * The AppleSingle and its debug file are tracked, so the tests run on a
 * machine without cc65 (UI-017 profiles it with the .dbg's symbols). */
#include "pom2_testport.h"

static unsigned add(unsigned a, unsigned b) { return a + b; }

int main(void)
{
    tp_line("HELLO FROM THE 6502");
    TP_ASSERT(add(2, 3) == 5);
    tp_line("2 + 3 = 5");
    tp_exit(3);
    return 0;
}
