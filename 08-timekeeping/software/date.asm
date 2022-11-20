;
; TIME - This CP/M program reads or sets the time in the Altair
;        Simulator RTC I/O cart, see
;        https://github.com/dhansel/Altair8800-IOBus/tree/main/08-timekeeping
;
; When called without parameters, the current time is printed
; If a parameter is given it must be in format HH-MM-SS (24-hour)
; and will set the time in the RTC accordingly
        
CLKBASE EQU     60H             ; base address of RTC card
        
CREG    EQU     CLKBASE         ; clock card control register
SREG    EQU     CLKBASE         ; clock card status register
DREG    EQU     CLKBASE+1       ; clock card data register

DSSEC   EQU     80H             ; DS1302 seconds register
DSMIN   EQU     82H             ; DS1302 minutes register
DSHOUR  EQU     84H             ; DS1302 hours   register
DSDAY   EQU     86H             ; DS1302 day     register
DSMON   EQU     88H             ; DS1302 month   register
DSYEAR  EQU     8CH             ; DS1302 year    register
DSCTL	EQU	8EH		; DS1302 control register

BDOS	EQU	5               ; BDOS entry point
PRINTCH EQU     4               ; BDOS "print character" function
PRINT	EQU	9               ; BDOS "print string" function
        
FCB	EQU	5CH             ; default CP/M file control block
PARAM1	EQU	FCB+1           ; first command-line parameter
PARAM2	EQU	PARAM1+16       ; first command-line parameter

        ORG     100H

        ; check whether we have a command line parameter
	LXI	D, PARAM1
	LDAX	D
	CPI	' '
	JZ	RDCLK           ; no parameter, just read clock

        ; read hours
        CALL	RDBYTE
        JC      ERROR           ; error if invalid number
        CPI     24H
	JNC	ERROR           ; error if hour>=24
        STA     HOUR

        ; check separator
        LDAX	D
	CPI	'-'
	JNZ	ERROR
	INX	D

        ; read minutes
        CALL	RDBYTE
        JC      ERROR           ; error if invalid numner
        CPI     60H
	JNC	ERROR           ; error if minute>=60
        STA     MINUTE

        ; check separator
	LDAX	D
	CPI	'-'
	JNZ	ERROR
	INX	D

        ; read seconds
	CALL	RDBYTE
        JC      ERROR           ; error if invalid number
        CPI     60H
	JNC	ERROR           ; error if second>=60
        STA     SECOND

        ; check whether we have a second command line parameter
	LXI	D, PARAM2
	LDAX	D
	CPI	' '
	JNZ	RDDATE		; if yes then read date
	CALL	PREPWR		; prepare DS1302 for setting time
	JMP	SETTIM		; set time

        ; read month
RDDATE: CALL	RDBYTE
        JC      ERROR           ; error if invalid number
        JZ      ERROR           ; error if month=0
        CPI     13H
	JNC	ERROR           ; error if month>12
        STA     MONTH

        ; check separator
        LDAX	D
	CPI	'-'
	JNZ	ERROR
	INX	D

        ; read day
        CALL	RDBYTE
        JC      ERROR           ; error if invalid number
        JZ      ERROR           ; error if day=0
        CPI     32H
	JNC	ERROR           ; error if day>31
        STA     DAY

        ; check separator
        LDAX	D
	CPI	'-'
	JNZ	ERROR
	INX	D

        ; read year
        CALL	RDBYTE
        JC      ERROR           ; error if invalid number
        STA     YEAR

	CALL	PREPWR		; prepare DS1302 for setting time
        LDA     YEAR            ; set year
        MVI	B, DSYEAR
	CALL	WRREG 
        LDA     MONTH           ; set month
        MVI	B, DSMON
	CALL	WRREG 
        LDA     DAY             ; set day
        MVI	B, DSDAY
	CALL	WRREG 

SETTIM:	LDA     HOUR            ; set hours
        MVI	B, DSHOUR
	CALL	WRREG 
        LDA     MINUTE          ; set minutes
	MVI	B, DSMIN
	CALL	WRREG 
        LDA     SECOND          ; set seconds and restart clock
	MVI	B, DSSEC
	CALL	WRREG

        LXI     D, MSGOK        ; print "time is now"
        MVI     C, PRINT
        CALL    BDOS

RDCLK:  MVI     B, DSSEC        ; read seconds
        CALL    RDREG
        MOV     C, A
        STA     SECOND
        MVI     B, DSMIN        ; read minutes
        CALL    RDREG
        STA     MINUTE
        MVI     B, DSHOUR       ; read hours
        CALL    RDREG
        STA     HOUR
        MVI     B, DSDAY        ; read day
        CALL    RDREG
        STA     DAY
        MVI     B, DSMON        ; read month
        CALL    RDREG
        STA     MONTH
        MVI     B, DSYEAR       ; read year
        CALL    RDREG
        STA     YEAR
        MVI     B, DSSEC        ; read seconds again
        CALL    RDREG
        CMP     C               ; compare to previous
        JC      RDCLK           ; if less now then rollover => repeat

        LDA     HOUR            ; print hours
        CALL    PRBYTE
        MVI     A, ':'          ; print ':'
        CALL    PRCH
        LDA     MINUTE          ; print minutes
        CALL    PRBYTE
        MVI     A, ':'          ; print ':'
        CALL    PRCH
        LDA     SECOND          ; print seconds
        CALL    PRBYTE
        MVI     A, ' '          ; print ' '
        CALL    PRCH
        LDA     MONTH           ; print month
        CALL    PRBYTE
        MVI     A, '/'          ; print '/'
        CALL    PRCH
        LDA     DAY             ; print day
        CALL    PRBYTE
        MVI     A, '/'          ; print '/'
        CALL    PRCH
        LDA     YEAR            ; print year
        CALL    PRBYTE
        
        LXI     D, CRLF         ; print newline
        MVI     C, PRINT
        CALL    BDOS
        RET

        ; read DS1302 register B, return value in A
RDREG:  MOV     A, B
        ORI     01H             ; set "read" flag
        OUT     CREG            ; issue command
RDREGW: IN      SREG            ; read status
        RLC                     ; bit 7 into carry
        JC      RDREGW          ; wait if still busy
        IN      DREG            ; read data
        RET

        ; write A to DS1302 register B
WRREG:	OUT	DREG            ; set data
	MOV	A, B
	OUT	CREG            ; issue command
WRREGW:	IN	SREG            ; read status
	RLC                     ; bit 7 into carry
	JC	WRREGW          ; wait if still busy
	RET

	; prepare DS1302 for setting date/time
PREPWR:	MVI	A, 0		; enable write access to registers
	MVI	B, DSCTL
	CALL	WRREG
	MVI	A, 80H		; stop clock
	MVI	B, DSSEC
	JMP	WRREG

        ; print BCD-encoded number in A
PRBYTE: PUSH    A               ; save A
        RRC                     ; move bits 4-7 to bits 0-3
        RRC
        RRC
        RRC
        CALL    PRDIG           ; print high digit
        POP     A               ; get A back (print low digit)

        ; print BCD digit in bits 0-3 of A
PRDIG:  ANI     0FH             ; isolate bits 0-3
        ADI     '0'             ; add ASCII '0'

        ; print character in A
PRCH:   MOV     E,A
        MVI     C,PRINTCH
        JMP     BDOS

        ; read two-digit number from location pointed
        ; to by DE, return the BCD-encoded value in A
        ; return with carry set if invalid number
RDBYTE: CALL    RDDIG           ; read a digit
        RC                      ; return if invalid
	RLC                     ; move to bits 3-7
	RLC
	RLC
	RLC
	MOV	B,A             ; save value
        CALL    RDDIG           ; read a digit
	RC                      ; return if invalid
	ORA	B               ; combine with previous
	RET

        ; read a single digit from location in DE
        ; return with carry set if invalid digit
RDDIG:  LDAX    D               ; get character
	INX	D               ; increment pointer
	SUI     '0'             ; subtract ASCII '0'
	RC                      ; return with carry set if A<0
        CPI     10              ; sets carry if A<10
	CMC                     ; invert carry (i.e. error if A>=10)
	RET
        
                        
ERROR:  LXI     D, MSGERR
        MVI     C, PRINT
        JMP     BDOS
        
LF	EQU	10
CR	EQU	13
            
HOUR:   DB      0
MINUTE: DB      0
SECOND: DB      0
YEAR:   DB      0
MONTH:  DB      0
DAY:    DB      0
        
MSGERR: DB      'Usage: date [HH-MM-SS [MM-DD-YY]]',CR,LF
	DB	'Shows current time and date if no parameters are given.',CR,LF
	DB	'If parameters are given then the time [and date] is set.'
CRLF:   DB      CR,LF,'$'
MSGOK:  DB      'Time is now: $'

        END
