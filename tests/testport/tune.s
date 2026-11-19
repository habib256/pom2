; AUDIO-013 end to end: three notes on a Mockingboard in slot 4, a short
; beep on the speaker, then EXIT 0 through the test port (slot 7).
;   ca65 -I tools/testport tests/testport/tune.s -o tune.o
;   ld65 -t none -S 0x0300 -o tests/testport/tune.bin tune.o
; Run twice with --vgm and --wav: the two VGMs and the two WAVs must match.
        .include "pom2_testport.inc"
MB_ORB     = $C400                 ; VIA 1 port B: BC1, BDIR, /RESET
MB_ORA     = $C401                 ; VIA 1 port A: the AY's data bus
MB_DDRB    = $C402
MB_DDRA    = $C403
SPKR    = $C030

        .org $0300
        lda #$FF
        sta MB_DDRA
        lda #$07
        sta MB_DDRB
        lda #$00                ; reset the AY
        sta MB_ORB
        lda #$04
        sta MB_ORB
        ldx #7                  ; mixer: tone A only
        lda #$3E
        jsr ay
        ldx #8                  ; volume A
        lda #$0F
        jsr ay
        ldy #0
note:   ldx #0                  ; tone A fine
        lda notes,y
        jsr ay
        ldx #1                  ; tone A coarse
        lda #$01
        jsr ay
        jsr wait
        iny
        cpy #3
        bne note
        ldx #8                  ; silence
        lda #$00
        jsr ay
        ldy #40                 ; a speaker click train
beep:   bit SPKR
        ldx #60
dly:    dex
        bne dly
        dey
        bne beep
        lda #0
        sta TP_EXIT
        jmp *

ay:     stx MB_ORA                 ; latch the register number
        pha
        lda #$07
        sta MB_ORB
        lda #$04
        sta MB_ORB
        pla
        sta MB_ORA                 ; write the value
        lda #$06
        sta MB_ORB
        lda #$04
        sta MB_ORB
        rts

wait:   txa                     ; about 50 000 cycles
        pha
        tya
        pha
        ldy #40
w1:     ldx #0
w2:     dex
        bne w2
        dey
        bne w1
        pla
        tay
        pla
        tax
        rts

notes:  .byte $DD, $AA, $77
