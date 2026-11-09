; End-to-end check of the Uthernet II card with IP65's own Uthernet II
; driver (drivers/w5100.s), run on ReplicApple2Plus's 6502 by Ip65Test.
;
; Leases an address over DHCP, looks a name up through the name server,
; opens a TCP connection, sends a line and waits for it to come back.
;
; Mailbox, set and read by the test:
;   SLOT      ($0300)  in:  the card's slot
;   PORT      ($0301)  in:  the echo server's port, low byte first
;   STAGE     ($0303)  out: last step reached; $FF when everything worked
;   LEASED    ($0310)  out: the address DHCP gave us
;   RESOLVED  ($0314)  out: the address the name lookup gave
;   ECHOED    ($0320)  out: what came back, NUL-terminated

.import ip65_init, ip65_process, dhcp_init
.import dns_set_hostname, dns_resolve, dns_ip
.import tcp_connect, tcp_connect_ip, tcp_callback, tcp_send_string, tcp_close
.import tcp_inbound_data_ptr, tcp_inbound_data_length
.import cfg_ip

.exportzp ptr1, ptr2, ptr3, ptr4, tmp1, tmp2, tmp3, tmp4, sreg

SLOT     = $0300
PORT     = $0301
STAGE    = $0303
LEASED   = $0310
RESOLVED = $0314
ECHOED   = $0320

.zeropage
ptr1:   .res 2
ptr2:   .res 2
ptr3:   .res 2
ptr4:   .res 2
tmp1:   .res 1
tmp2:   .res 1
tmp3:   .res 1
tmp4:   .res 1
sreg:   .res 2
src:    .res 2

.segment "STARTUP"
start:
        cld
        ldx #$FF
        txs
        lda #1
        sta STAGE
        lda SLOT
        jsr ip65_init
        bcs halt

        lda #2
        sta STAGE
        jsr dhcp_init
        bcs halt
        ldx #3
:       lda cfg_ip,x
        sta LEASED,x
        dex
        bpl :-

        lda #3
        sta STAGE
        lda #<hostname
        ldx #>hostname
        jsr dns_set_hostname
        bcs halt
        jsr dns_resolve
        bcs halt
        ldx #3
:       lda dns_ip,x
        sta RESOLVED,x
        sta tcp_connect_ip,x
        dex
        bpl :-

        lda #4
        sta STAGE
        lda #<received
        sta tcp_callback
        lda #>received
        sta tcp_callback+1
        lda PORT
        ldx PORT+1
        jsr tcp_connect
        bcs halt

        lda #5
        sta STAGE
        lda #<message
        ldx #>message
        jsr tcp_send_string
        bcs halt

        lda #6
        sta STAGE
:       jsr ip65_process
        lda done
        beq :-

        lda #7
        sta STAGE
        jsr tcp_close
        lda #$FF
        sta STAGE
halt:   jmp halt

; tcp_callback: copies what arrived to ECHOED
received:
        lda tcp_inbound_data_length+1
        cmp #$FF
        beq @out                ; the connection closed
        lda tcp_inbound_data_ptr
        sta src
        lda tcp_inbound_data_ptr+1
        sta src+1
        ldy #0
@copy:  cpy tcp_inbound_data_length
        beq @end
        lda (src),y
        sta ECHOED,y
        iny
        bne @copy
@end:   lda #0
        sta ECHOED,y
        lda #1
        sta done
@out:   rts

.rodata
hostname:
        .byte "localhost", 0
message:
        .byte "HELLO FROM THE APPLE II+", 0

.bss
done:   .res 1
