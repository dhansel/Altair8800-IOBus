I2CBASE EQU     42H             ; base address of I2C card
I2CADDR EQU     3CH             ; I2C address of the SSD1306 display

CREG    EQU     I2CBASE         ; I2C card control register
SREG    EQU     I2CBASE         ; I2C card status register
DREG    EQU     I2CBASE+1       ; I2C card data register

BDOS	EQU	5               ; BDOS entry point
PRINTCH EQU     4               ; BDOS "print character" function
PRINT	EQU	9               ; BDOS "print string" function
RDLINE	EQU	10              ; BDOS "read line" function
        
TXTLEN	EQU	32              ; length of text entry buffer

        ORG     100H

STRT:   CALL    INIT            ; initialize SSD1306
        JNZ     ERROR           ; jump if I2C error
        CALL    CLR             ; clear display
        JNZ     ERROR           ; jump if I2C error
LOOP:	LXI	D,PROMPT	; "Message" prompt
	MVI	C,PRINT
	CALL	BDOS		; print it
	LXI	D,TXTBUF	; input buffer
	MVI	A,TXTLEN	; input buffer length
	STAX	D
	MVI	C,RDLINE	; read line routine
	CALL	BDOS		; execute
	CALL	CLRROW          ; clear current row
	JNZ	ERROR           ; jump if I2C error
	LXI	H,TXTBUF+1      ; set HL to beginning of buffer
	MOV	B,M             ; get length of input into B
        INX     H               ; point to beginning of string
        CALL    PRSTR           ; print string to SSD1306
        JNZ     ERROR           ; jump if I2C error
        CALL    NXTROW          ; go to next row
        JMP	LOOP            ; repeat

        ;; lookup table to double 4 bits to 8 bits
        ;; placed here to guarantee it's not on a page boundary
TBL2X   DB       00h, 03h, 0Ch, 0Fh, 30h, 33h, 3Ch, 3Fh
        DB      0C0h,0C3h,0CCh,0CFh,0F0h,0F3h,0FCh,0FFh

        
        ;; print string pointed to by HL and with length B
        ;; at current cursor position
        ;; returns with Z flag clear if I2C error
PRSTR:  INR	B
PRSLP:  DCR	B		; decrement number of chars
	RZ			; return if done
	MOV     A,M             ; get character
        PUSH    H               ; save string address
        PUSH    B
        CALL    PRCH            ; print character
        POP     B
        POP     H               ; get string address back
        RNZ                     ; return if I2C error
        INX     H               ; next character
        JMP     PRSLP           ; repeat
                
        ;; print string pointed to by HL terminated by NUL
        ;; at current cursor position
        ;; returns with Z flag clear if I2C error
PRSTRT: MOV     A,M             ; get character
        ORA     A               ; is it 0?
        RZ                      ; if so then we're done
        PUSH    H               ; save string address
        CALL    PRCH            ; print character
        POP     H               ; get string address back
        RNZ                     ; return if I2C error
        INX     H               ; next character
        JMP     PRSTR           ; repeat
        
        ;; print character in A at cursor position, advancing cursor
        ;; returns with Z flag clear if I2C error
        ;; returns without printing if A<20H or A>7EH
        ;; clobbers A,B,C,D,E,H,L
PRCH:   CPI     '-'
        JZ      SET1X
        CPI     '+'
        JZ      SET2X
        SUI     20H             ; subtract ASCII ' '
        RC                      ; return if A<0
        RLC                     ; A=A*2
        MOV     C,A             ; save in C
        MVI     A,0BCH
        CMP     C
        RC                      ; return if BCh<C
        PUSH    B               ; save B/C
        CALL    CLIPC           ; set clipping region for this character
        PUSH    A               ; save processor flags
        CALL    ADVCSR          ; advance cursor
        POP     A               ; restore processor flags
        POP     B               ; restore B/C
        RNZ                     ; return if I2C error
        LXI     H,FONTD         ; get pointer to font data
        MVI     B,0             ; HL=HL+3*C
        DAD     B               ; (in total:  HL=HL+6*(ascii-32))
        DAD     B
        DAD     B               ; HL now points to the character's font data
        LDA     CROWS           ; get number of rows per character (1 or 2)
        DCR     A               ; subtract 1
        JNZ     PR2X            ; if not 0 then double-sized chars
        MVI     A,6             ; sending 6 bytes of font data
        MVI     B,40h           ; sending a stream of data (Co=0,C/D=1)
        JMP     I2CSND          ; send data and return

        ;; print character font data pointed to by HL in double size
PR2X:   PUSH    H               ; save font table pointer
        MVI     B,1             ; set "top row" flag
        CALL    PR2XR           ; print upper row
        POP     H               ; get original font table pointer
        RNZ                     ; return if I2C error
        MVI     B,0             ; clear "top row" flag
        JMP     PR2XR           ; print lower row and return

        ;; print top or bottom row (flag in B) of font data pointed
        ;; to by HL in double size
PR2XR:  SHLD    FNTPTR          ; save font pointer
        LXI     D,I2CBUF        ; I2C buffer start
        MVI     C,6             ; 6 bytes of font data
PR2XRL: LHLD    FNTPTR          ; get font table pointer
        MOV     A,M             ; get font byte
        INX     H               ; advance font table pointer
        SHLD    FNTPTR          ; save font table pointer
        DCR     B               ; test "top row" flag
        INR     B
        JNZ     PR2XR1          ; skip shift if top row (LSB is top)
        RRC                     ; shift upper 4 bits to lower 4 bits
        RRC
        RRC
        RRC
PR2XR1: ANI     0Fh             ; isolate lower 4 bits
        LXI     H,TBL2X         ; get 2x expansion table pointer
        ADD     L               ; add A to HL
        MOV     L,A             ; (TBL2X must not be on page boundary)
        MOV     A,M             ; get 2x expansion
        STAX    D               ; store once
        INX     D               ; increment I2C buffer pointer
        STAX    D               ; store again (x2)
        INX     D               ; increment I2C buffer pointer
        DCR     C               ; decrement byte counter
        JNZ     PR2XRL          ; repeat until done
        MVI     A,12            ; sending 12 bytes of font data
        MVI     B,40h           ; sending a stream of data (Co=0,C/D=1)
        LXI     H,I2CBUF        ; data is in I2C buffer
        JMP     I2CSND          ; send data
FNTPTR: DS      2

        
        ;; switch to 2x character size
SET2X:  LXI     H,0C02h
        SHLD    CROWS
        RET
                
        ;; switch to 1x character size
SET1X:  LXI     H,0601h
        SHLD    CROWS
        RET

        ;; set cursor to home position
        ;; returns with Z flag clear if I2C error
        ;; clobbers A
HOME:   XRA     A
        STA     ROW
        STA     COL
        RET

        ;; advance cursor one position
        ;; clobbers A,B,C
ADVCSR: LDA     CCOLS           ; number of cols per character
        MOV     B,A             ; save in B
        LDA     COL             ; get current column
        ADD     B               ; add #cols
        STA     COL             ; store new column
        ADD     B               ; add another character width
        CPI     128             ; is that past 128?
        RC                      ; if not then return

        ;; place cursor at beginning of next row
        ;; clobbers A,B,C
NXTROW: LDA     CROWS           ; number of rows per character
        MOV     B,A             ; save in B
        LDA     NROWS           ; get number of rows on display
        DCR     A               ; create bit mask
        MOV     C,A
        LDA     ROW             ; get current row
        ADD     B               ; add #rows
        ANA     C               ; wrap over
        STA     ROW             ; set row
        XRA     A               ; A=0
        STA     COL             ; set column
        RET
        
        ;; clear screen
        ;; returns with Z flag clear if I2C error
        ;; clobbers A,B,C,D,H,L
CLR:    CALL    HOME            ; set cursor to home position
        LDA     NROWS
        MOV     E,A
        MVI     D,80h
        LXI     H,0000h
        CALL    CLIP
        RNZ
        LDA     NROWS           ; get number of rows
        JMP     SND0            ; send zeros to clear all rows

        ;; clears current ROW
        ;; clobbers A,B,C,D,E,H,L
CLRROW: LDA     ROW             ; get current row
        MOV     L,A             ; into L
        MVI     H,0             ; H=0
        LDA     CROWS           ; get number of rows per character
        MOV     E,A             ; into E
        MVI     D,80h           ; 128 columns
        CALL    CLIP            ; set clipping region
        MOV     A,E             ; number of rows per character

        ;; sends required number of "0" data values to clear A rows
        ;; maximum I2C packet length is 32 but we need to send the
        ;; control byte first so can only send 31 zeros per packet
        ;; each row has 128 bytes so we need to send 4*nrows packets 
        ;; of 31 bytes plus one packet of 4*nrows zeros
        ;; clobbers A,B,C,D,H,L
SND0:   RLC                     ; number of full packets is 4*nrows
        RLC
        MOV     D,A             ; number of packets
        LXI     H,I2CBUF        ; get I2C buffer address
        XRA     A               ; A=0
        MVI     C,32            ; fill whole buffer
CLRLP1: MOV     M,A             ; write to buffer
        INX     H               ; increment buffer pointer
        DCR     C
        JNZ     CLRLP1          ; repeat until done
        MOV     A,D             ; send 4*nrows zeros first
        INR     D               ; add one for initial (non-complete) packet
CLRLP2: CPI     32              ; do we still need to send more than 31 zeros?
        JC      CLRLP3          ; if not then we're good to go
        INR     D               ; send one more full packet
        SUI     31              ; subtract 31 zeros
        JMP     CLRLP2          ; repeat
CLRLP3: MVI     B,40h           ; sending stream of data (Co=0,C/D=1)
        LXI     H,I2CBUF        ; get I2C buffer address
        CALL    I2CSND          ; send data
        RNZ                     ; return if I2C error
        DCR     D
        MVI     A,31            ; send 31 zeroes next
        JNZ     CLRLP3          ; repeat until all sent
        RET
        
        ;; set clipping region for character at current
        ;; cursor position specified in ROW/COL, 
        ;; returns with Z flag clear if I2C error
        ;; clobbers A,B,C,D,E,H,L
CLIPC:  LHLD    CROWS           ; get #cols/char in H and #rows/char in L
        XCHG                    ; move HL to DE
        LHLD    ROW             ; get current col in H and current col in L

        ;; set clipping region (col=H,row=L,width=D,height=E)
        ;; returns with Z flag clear if I2C error
        ;; clobbers A,B,C,H,L
CLIP:   MVI     A,21H 
        STA     I2CBUF          ; "set column start and end address" command
        MOV     A,H
        STA     I2CBUF+1        ; first column
        ADD     D               ; add length
        DCR     A               ; subtract 1
        STA     I2CBUF+2        ; last column
        MVI     A,22H
        STA     I2CBUF+3        ; "set row start and end address" command
        MOV     A,L
        STA     I2CBUF+4        ; first row
        ADD     E               ; add length
        DCR     A               ; subtract 1
        STA     I2CBUF+5        ; last row
        MVI     A,6             ; 6 bytes to send from buffer
        MVI     B,00h           ; sending command bytes stream (Co=0,C/D=0)
        LXI     H,I2CBUF
        JMP     I2CSND          ; send command and return
        
        ;; send init sequence to SSD1306
        ;; returns with Z flag clear if I2C error
INIT:   ; make sure I2C card is not in "write" mode
        XRA     A
        OUT     DREG
        OUT     CREG
        CALL    SNDWT
        ; configure I2C speed (500k)
        MVI     A,1             ; one byte of config data to send
        OUT     DREG
        MVI     A,80h           ; send data to device 0 (card config)
        OUT     CREG
        MVI     A,7             ; 7=max speed (500k)
        OUT     DREG
        ; send init sequence
        LXI     H,INITD         ; get pointer to init sequence
INITL1: MOV     A,M             ; get length of next command
        ORA     A               ; is it 0?
        RZ                      ; if so then we're done
        INX     H               ; point to first command byte
        MVI     B,00h           ; sending command bytes stream (Co=0,C/D=0)
        CALL    I2CSND          ; send command
        RNZ                     ; return if error
        JMP     INITL1          ; next command
        
        ;; Send byte in B, followed by A (possibly 0) bytes from
        ;; the buffer pointed to by HL
        ;; returns with Z flag clear if I2C error
        ;; clobbers A,B,C,H,L (HL=HL+A)
I2CSND: INR     A               ; one more byte (for value in B)
        MOV     C,A             ; save buffer length
        OUT     DREG            ; write length to data register
        MVI     A,I2CADDR+128   ; I2C address plus WRITE flag
        OUT     CREG            ; write to command register
        MOV     A,B
        OUT     DREG            ; send byte in B
SNDLP:  DCR     C               ; decrement byte count
        JZ      SNDWT           ; jump when done
        MOV     A,M             ; get next byte from buffer
        OUT     DREG            ; write to data register
        INX     H
        JMP     SNDLP           ; repeat
SNDWT:  IN      SREG            ; get status
        RLC                     ; "BUSY" bit into carry
        JC      SNDWT           ; wait if busy
        CPI     0               ; set Z flag if no error
        RET

        ;; Receive data from I2C device (length in A) and store in I2CBUF
        ;; returns with Z flag clear if I2C error
        ;; clobbers A,B,H,L
I2CRCV: ORA     A               ; check A
        RZ                      ; return immediately if A=0
        OUT     DREG            ; write length to data register
        MOV     B,A             ; save length
        MVI     A,I2CADDR       ; I2C address
        OUT     CREG            ; write to command register (executes the read)
RCVWT:  IN      SREG            ; get status
        RLC                     ; "BUSY" bit into carry
        JC      RCVWT           ; wait if busy
        CPI     40H             ; expected status (before RLC) was 20H
        RNZ                     ; return with Z=0 if different (i.e. error)
        LXI     H,I2CBUF        ; get I2C buffer address
RCVLP:  IN      DREG            ; get data byte
        MOV     M,A             ; store in buffer
        INX     H
        DCR     B
        JNZ     RCVLP           ; repeat until all read
        RET                     ; return with Z=1
        
        ;; an error occurred in I2C communication, print to screen
ERROR:  LXI     D,ERRMSG        ; print "I2C error"
        MVI     C,PRINT
        CALL    BDOS
        IN      SREG            ; get I2C status and print it

        ;; print byte in A as HEX to screen (stdout)
PRHEX:  PUSH    A               ; save byte
        RRC                     ; shift high nibble
        RRC
        RRC
        RRC
        CALL    PRDIG           ; print digit
        POP     A               ; get byte back and fall into print
PRDIG:  ANI     0FH             ; isolate 4 bottom bits
        LXI     H,HEXDIG        ; get hex digit base
        MVI     B,0
        MOV     C,A
        DAD     B               ; HL now poits to ASCII hex digit
        MOV     A,M             ; get ASCII hex digit
        MOV     E,A             
        MOV     C,PRINTCH       ; print character
        JMP     BDOS
HEXDIG: DB      '0123456789ABCDEF'
        

I2CBUF: DS      32
TXTBUF:	DS	TXTLEN+2

ROW:    DB      0               ; current row number
COL:    DB      0               ; current column number
CROWS:  DB      1               ; number of rows per character
CCOLS:  DB      6               ; number of columns per character
NROWS:  DB      8               ; number of rows on display
        
PROMPT:	DB	13,10,'Message : $'
ERRMSG: DB      'I2C error $'

        ;; SSD1306 initialization sequence
INITD:  DB      1,0AEh           ; display off
        DB      2,0A8h, 3Fh      ; multiplex ratio: 3Fh
        DB      2,0D3h, 00h      ; display offset: 0
        DB      2, 20h, 00h      ; addressing mode: horizontal
        DB      3, 21h, 00h, 7Fh ; column start/end: 0/127
	DB	3, 22h, 00h, 07h ; row start/end: 0/7
        DB      1, 40h           ; display RAM start line: 0
        DB      2, 81h,0CFh      ; contrast: CFh
        DB      1,0A1h           ; segment remap
        DB      1,0C8h           ; COM output scan direction: remap
        DB      1,0A6h           ; normal display
        DB      2,0D5h, 80h      ; clock divide ratio: 80h
        DB      2,0D9h,0F1h      ; precharge period: F1h
        DB      2,0DAh, 12h      ; COM pins hardware config: 2
        DB      2,0DBh, 30h      ; VCOMH deselect level: 30h
        DB      1, 2Eh           ; deactivate scroll
        DB      2, 8Dh, 14h      ; enable charge pump
        DB      1,0AFh           ; display on
        DB      0                ; init sequence done

        ;; 95 characters of 6x8 font data (for ASCII 32-126)
FONTD:  DB	000h,000h,000h,000h,000h,000h,000h,000h,05Fh,000h,000h,000h
	DB	000h,007h,000h,007h,000h,000h,014h,07Fh,014h,07Fh,014h,000h
	DB	024h,02Ah,07Fh,02Ah,012h,000h,023h,013h,008h,064h,062h,000h
	DB	036h,049h,056h,020h,050h,000h,000h,008h,007h,003h,000h,000h
	DB	000h,01Ch,022h,041h,000h,000h,000h,041h,022h,01Ch,000h,000h
	DB	02Ah,01Ch,07Fh,01Ch,02Ah,000h,008h,008h,03Eh,008h,008h,000h
	DB	000h,080h,070h,030h,000h,000h,008h,008h,008h,008h,008h,000h
	DB	000h,000h,060h,060h,000h,000h,020h,010h,008h,004h,002h,000h
	DB	03Eh,051h,049h,045h,03Eh,000h,000h,042h,07Fh,040h,000h,000h
	DB	072h,049h,049h,049h,046h,000h,021h,041h,049h,04Dh,033h,000h
	DB	018h,014h,012h,07Fh,010h,000h,027h,045h,045h,045h,039h,000h
	DB	03Ch,04Ah,049h,049h,031h,000h,041h,021h,011h,009h,007h,000h
	DB	036h,049h,049h,049h,036h,000h,046h,049h,049h,029h,01Eh,000h
	DB	000h,000h,014h,000h,000h,000h,000h,040h,034h,000h,000h,000h
	DB	000h,008h,014h,022h,041h,000h,014h,014h,014h,014h,014h,000h
	DB	000h,041h,022h,014h,008h,000h,002h,001h,059h,009h,006h,000h
	DB	03Eh,041h,05Dh,059h,04Eh,000h,07Ch,012h,011h,012h,07Ch,000h
	DB	07Fh,049h,049h,049h,036h,000h,03Eh,041h,041h,041h,022h,000h
	DB	07Fh,041h,041h,041h,03Eh,000h,07Fh,049h,049h,049h,041h,000h
	DB	07Fh,009h,009h,009h,001h,000h,03Eh,041h,041h,051h,073h,000h
	DB	07Fh,008h,008h,008h,07Fh,000h,000h,041h,07Fh,041h,000h,000h
	DB	020h,040h,041h,03Fh,001h,000h,07Fh,008h,014h,022h,041h,000h
	DB	07Fh,040h,040h,040h,040h,000h,07Fh,002h,01Ch,002h,07Fh,000h
	DB	07Fh,004h,008h,010h,07Fh,000h,03Eh,041h,041h,041h,03Eh,000h
	DB	07Fh,009h,009h,009h,006h,000h,03Eh,041h,051h,021h,05Eh,000h
	DB	07Fh,009h,019h,029h,046h,000h,026h,049h,049h,049h,032h,000h
	DB	003h,001h,07Fh,001h,003h,000h,03Fh,040h,040h,040h,03Fh,000h
	DB	01Fh,020h,040h,020h,01Fh,000h,03Fh,040h,038h,040h,03Fh,000h
	DB	063h,014h,008h,014h,063h,000h,003h,004h,078h,004h,003h,000h
	DB	061h,059h,049h,04Dh,043h,000h,000h,07Fh,041h,041h,041h,000h
	DB	002h,004h,008h,010h,020h,000h,000h,041h,041h,041h,07Fh,000h
	DB	004h,002h,001h,002h,004h,000h,040h,040h,040h,040h,040h,000h
	DB	000h,003h,007h,008h,000h,000h,020h,054h,054h,078h,040h,000h
	DB	07Fh,028h,044h,044h,038h,000h,038h,044h,044h,044h,028h,000h
	DB	038h,044h,044h,028h,07Fh,000h,038h,054h,054h,054h,018h,000h
	DB	000h,008h,07Eh,009h,002h,000h,018h,0A4h,0A4h,09Ch,078h,000h
	DB	07Fh,008h,004h,004h,078h,000h,000h,044h,07Dh,040h,000h,000h
	DB	020h,040h,040h,03Dh,000h,000h,07Fh,010h,028h,044h,000h,000h
	DB	000h,041h,07Fh,040h,000h,000h,07Ch,004h,078h,004h,078h,000h
	DB	07Ch,008h,004h,004h,078h,000h,038h,044h,044h,044h,038h,000h
	DB	0FCh,018h,024h,024h,018h,000h,018h,024h,024h,018h,0FCh,000h
	DB	07Ch,008h,004h,004h,008h,000h,048h,054h,054h,054h,024h,000h
	DB	004h,004h,03Fh,044h,024h,000h,03Ch,040h,040h,020h,07Ch,000h
	DB	01Ch,020h,040h,020h,01Ch,000h,03Ch,040h,030h,040h,03Ch,000h
	DB	044h,028h,010h,028h,044h,000h,04Ch,090h,090h,090h,07Ch,000h
	DB	044h,064h,054h,04Ch,044h,000h,000h,008h,036h,041h,000h,000h
	DB	000h,000h,077h,000h,000h,000h,000h,041h,036h,008h,000h,000h
	DB	002h,001h,002h,004h,002h,000h,009h,044h,042h,009h,000h,000h
