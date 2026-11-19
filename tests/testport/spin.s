; ARCH-017: a program that never writes EXIT. With --frames 120 the run ends
; on the budget: pom2_headless returns 4 and prints nothing on stdout.
;   ca65 tests/testport/spin.s -o spin.o && ld65 -t none -S 0x0300 -o tests/testport/spin.bin spin.o
        .org $0300
        jmp *
