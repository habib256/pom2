/* pom2_testport.h — the POM2 paravirtual test port, for cc65 programs.
 *
 * Run a 6502 unit test on a complete Apple II:
 *
 *   cl65 -t apple2 -I tools/testport -o mytest mytest.c
 *   pom2_headless --test-port 7 --load mytest
 *
 * pom2_headless prints each TP_LOG line on stdout and returns the status
 * given to tp_exit() (2 when a TP_ASSERT fails). The port exists only when
 * pom2_headless is given --test-port; elsewhere these addresses are an empty
 * slot. Define POM2_TEST_PORT_SLOT before including to use another slot.
 *
 * Copyright (C) 2026 VERHILLE Arnaud — GPL-3.0-or-later, like POM2.
 */
#ifndef POM2_TESTPORT_H
#define POM2_TESTPORT_H

#ifndef POM2_TEST_PORT_SLOT
#define POM2_TEST_PORT_SLOT 7
#endif

#define TP_BASE   (0xC080u + 16u * (POM2_TEST_PORT_SLOT))
#define TP_LOG    (*(volatile unsigned char*)(TP_BASE + 0u))
#define TP_EXIT   (*(volatile unsigned char*)(TP_BASE + 1u))
#define TP_ASSERT_REG (*(volatile unsigned char*)(TP_BASE + 2u))

/* One character, a string, a whole line. */
#define tp_putc(c) (TP_LOG = (unsigned char)(c))
static void tp_puts(const char* s) { while (*s) tp_putc(*s++); }
static void tp_line(const char* s) { tp_puts(s); tp_putc('\r'); }

/* End the run with this status. Never returns. */
static void tp_exit(unsigned char status) { TP_EXIT = status; for (;;) {} }

/* A failed assertion ends the run with status 2, the condition as its text. */
#define TP_ASSERT(cond) \
    do { if (cond) TP_ASSERT_REG = 1; else { tp_puts(#cond); TP_ASSERT_REG = 0; for (;;) {} } } while (0)

#endif /* POM2_TESTPORT_H */
