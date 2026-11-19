; ARCH-017: a failed assertion, from a raw binary loaded at an address.
;   ca65 -I tools/testport tests/testport/assert.s -o assert.o
;   ld65 -t none -S 0x0300 -o tests/testport/assert.bin assert.o
; Logs one line, passes one assertion, fails the second with a message:
; pom2_headless returns 2 and prints "ASSERT #2 FAILED: X != 1".
        .include "pom2_testport.inc"
        .org $0300
        ldx #0
loop:   lda msg,x
        beq done
        sta TP_LOG
        inx
        bne loop
done:   lda #1
        sta TP_ASSERT           ; #1 passes
        ldx #0
loop2:  lda why,x
        beq fail
        sta TP_LOG
        inx
        bne loop2
fail:   lda #0
        sta TP_ASSERT           ; #2 fails: the run ends with status 2
        jmp *
msg:    .byte "RAW AT $0300", $0D, 0
why:    .byte "X != 1", 0
