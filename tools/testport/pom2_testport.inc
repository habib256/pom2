; pom2_testport.inc — the POM2 paravirtual test port, for ca65 programs.
; Reads are the floating bus; only writes mean something. See
; tools/testport/pom2_testport.h and src/TestPortCard.h.
; Copyright (C) 2026 VERHILLE Arnaud — GPL-3.0-or-later, like POM2.

.ifndef POM2_TEST_PORT_SLOT
POM2_TEST_PORT_SLOT = 7
.endif

TP_LOG    = $C080 + 16 * POM2_TEST_PORT_SLOT   ; write: one log character, CR/LF ends the line
TP_EXIT   = TP_LOG + 1                          ; write: end the run with this status
TP_ASSERT = TP_LOG + 2                          ; write: 0 = assertion failed (exit 2)
