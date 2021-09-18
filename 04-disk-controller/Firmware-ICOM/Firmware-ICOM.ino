// -----------------------------------------------------------------------------
// Copyright (C) 2021 David Hansel
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
// -----------------------------------------------------------------------------

/*

Program as ATMega328P @16MHz
Fuse bytes (Arduino standard): LOW=0xFF, HIGH=0xDA, EXTENDED=0xFD


Arduino   Atmega Register   Direction  Function
RESET     1      PC6        in         RESET
D0        2      PD0        in/out     D0
D1        3      PD1        in/out     D1
D2        4      PD2        in/out     D2
D3        5      PD3        in/out     D3
D4        6      PD4        in/out     D4
VIn       7                            VCC
GND       8                            GND
-         9      PB6                   Crystal
-         10     PB7                   Crystal
D5        11     PD5        in/out     D5
D6        12     PD6        in/out     D6
D7        13     PD7        in/out     D7
D8        14     PB0        in         READ      (ICP1)
D9        15     PB1        out        WAIT
D10       16     PB2        out        SR latch  (inputs)
D11       17     PB3        out        WRITE     (SPIMOSI+OC2A)
D12       18     PB4        out        SR Latch  (outputs)
D13       19     PB5        in         INDEX     (PCINT5/PCMSK0)
VIn       20                           AVCC
ARef      21                           AREF
GND       22                           GND
A0        23     PC0        in         A0
A1        24     PC1        in         A1
A2        25     PC2        out        SR Clock  (inputs+outputs)
A3        26     PC3        out        SR Data   (inputs+outputs)
A4        27     PC4        in         INP       (PCINT12/PCMSK1)
A5        28     PC5        in         OUT       (PCINT13/PCMSK1)

Shift register (74HC595, output):
Output  Pin  Function
QA      15   DENSITY
QB      1    MOTOR 0 (HEADLOAD for Shugart SA800)
QC      2    SELECT 1
QD      3    SELECT 0
QE      4    MOTOR 1 (HEADLOAD for Shugart SA800)
QF      5    STEP
QG      6    WRITEGATE
QH      7    SIDE

Shift register (74HC165, input)
Input   Pin  Function
D0      11   DIP0
D1      12   DIP1
D2      13   DIP2
D3      14   DIP3
D4       3   MONITOR
D5       4   WRITEPROTECT
D6       5   DISKCHANGE
D7       6   TRACK0

DIP switch settings (A/B refer to physical drives on ribbon cable):
DIP1     on=Minidisk (5.25") system, off=8" disk system
DIP2     on=Swap drives A and B, off=don't swap
DIP3     on=drive B is SA-800, off=drive B is standard 5.25inch
DIP4     on=drive A is SA-800, off=drive A is standard 5.25inch

*/

// optimize code for performance (speed)
//#pragma GCC optimize ("-O2")


// 0 = monitor disabled
// 1 = always enter monitor at boot
// 2 = enter monitor if MONITOR button is pressed at boot
#define MONITOR 2


// maximum number of drives is 2, could be increased but that will require
// changing the function of the SELECTx/MOTORx output signals
#define MAX_DRIVES 2


// uncommenting this will show debug information on the serial connection
// while attempting to read data
//#define DEBUG

// uncommenting this will enable a debugging signal on the WRITEDATA line
// (PB3, ATMega pin 17) while attemoting to read data.
//#define DEBUGSIG


// pins 0-7 are hardwired to PD0-7 in functions busINP() and busOUT()
#undef PIN_A0
#undef PIN_A1
#define PIN_READDATA      8   // must be pin 8 (ICP1 for timer1)
#define PIN_WAIT          9   // hardwired to PB1 in function handle_bus_communication() and read_sector_data_sd()
#define PIN_SRILATCH     10   // hardwired to PB2 in function shift_in()
#define PIN_WRITEDATA    11   // hardwired to PB3 (SPI-MOSI) in function write_sector_data_dd()
#define PIN_SROLATCH     12   // hardwired to PB4 in function shift_out()
#define PIN_INDEX        13   // hardwired to PB5 (PCINT5)
#define PIN_A0           A0   // hardwired to PC0 in functions busINP() and busOUT()
#define PIN_A1           A1   // hardwired to PC1 in functions busINP() and busOUT()
#define PIN_SRCLOCK      A2   // hardwired to PC2 in function shift_out() 
#define PIN_SRDATA       A3   // hardwired to PC3 in function shift_out()
#define PIN_INP          A4   // hardwired to PC4 (PCINT12)
#define PIN_OUT          A5   // hardwired to PC5 (PCINT13)



// output shift register pins
#define PIN_SRO_DENSITY    0
#define PIN_SRO_MOTOR0     1
#define PIN_SRO_SELECT1    2
#define PIN_SRO_SELECT0    3
#define PIN_SRO_MOTOR1     4
#define PIN_SRO_STEP       5
#define PIN_SRO_WRITEGATE  6
#define PIN_SRO_SIDE1      7
#define PIN_SRO_SELECT     pinSelect[selDrive]
#define PIN_SRO_MOTOR      pinMotor[selDrive]

// input shift register pins
#define PIN_SRI_DIP1      0
#define PIN_SRI_DIP2      1
#define PIN_SRI_DIP3      2
#define PIN_SRI_DIP4      3
#define PIN_SRI_MONITOR   4
#define PIN_SRI_WRTPROT   5
#define PIN_SRI_DSKCHG    6
#define PIN_SRI_TRACK0    7
#define SRI_BIT_SET(pin)  (shift_in(8-(pin)) & 1)


// SELECT/MOTOR bits in shift register for drive 0/1
static byte pinSelect[2], pinMotor[2];

// common data buffer (for monitor and controller)
#define DATA_BUFFER_SIZE 1024
static byte dataBuffer[DATA_BUFFER_SIZE]; // must hold at least 3 sectors of 256+3 bytes 
static byte header[7];


// current content of shift register for drive control
static byte drivectrl;

// currently selected drive (0/1)
static byte selDrive = 0;

#define NUM_TRACKS  77
#define NUM_SECTORS 26


// -------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------   Input/Output shift registers   ---------------------------------------------
// -------------------------------------------------------------------------------------------------------------------------


// define WAIT assembler macro (creates assembly instructions to wait a specific number of cycles)
// IMPORTANT: clobbers r19!
asm (".macro WAIT cycles:req\n"
     "  .if \\cycles < 9\n"
     "    .rept \\cycles\n"
     "      nop\n" 
     "    .endr\n"
     "  .else\n"
     "    ldi   r19, \\cycles/3 \n"
     "    dec  	r19\n"
     "    brne	.-4\n"
     "    .if (\\cycles % 3)>0\n"
     "      nop\n"
     "      .if (\\cycles % 3)>1\n"
     "        nop\n"
     "      .endif\n"
     "    .endif\n"
     "  .endif\n"
     ".endm\n");


// to use the WAIT macro from within C
#define WAIT(cycles) asm volatile("WAIT " #cycles : : : "r19")


static byte shift_in(byte n = 8)
{
  // this implementation takes about 8us to read all 8 bits
  byte res = 0;
  DDRC  &= ~0x08;  // switch "SR Data" pin to input
  PORTC &= ~0x04;  // set clock low
  PORTB |=  0x04;  // latch inputs

  for(byte i=0; i<n; i++)
    {
      WAIT(4);                       // 4 cycles = 250ns delay
      res = res * 2;                 // shift result
      if( PINC & 0x08 ) res |= 1;    // read bit
      PORTC |= 0x04; PORTC &= ~0x04; // pulse clock
    }
  
  PORTB &= ~0x04;  // release inputs
  DDRC  |=  0x08;  // switch "SR Data" pin back to output
  PORTC |=  0x04;  // set clock back HIGH
  return res;
}


static void shift_out(byte data, bool delay1us = false)
{
  // this implementation takes about 2us to write the shift register (3 with delay)
  register byte cd = PORTC & ~0x0C;  // cd = clock LOW  data LOW
  register byte cD = cd | 0x08;      // cD = clock LOW  data HIGH
  register byte Cd = cd | 0x04;      // Cd = clock HIGH data LOW
  register byte CD = cd | 0x0C;      // CD = clock HIGH data HIGH
  
  // the shift register reads data on the clock LOW->HIGH edge
  PORTB &= ~0x10;
  if( data & 0x80 ) { PORTC = cD; PORTC = CD; } else { PORTC = cd; PORTC = Cd; }
  if( data & 0x40 ) { PORTC = cD; PORTC = CD; } else { PORTC = cd; PORTC = Cd; }
  if( data & 0x20 ) { PORTC = cD; PORTC = CD; } else { PORTC = cd; PORTC = Cd; }
  if( data & 0x10 ) { PORTC = cD; PORTC = CD; } else { PORTC = cd; PORTC = Cd; }
  if( data & 0x08 ) { PORTC = cD; PORTC = CD; } else { PORTC = cd; PORTC = Cd; }
  if( data & 0x04 ) { PORTC = cD; PORTC = CD; } else { PORTC = cd; PORTC = Cd; }
  if( data & 0x02 ) { PORTC = cD; PORTC = CD; } else { PORTC = cd; PORTC = Cd; }
  if( data & 0x01 ) { PORTC = cD; PORTC = CD; } else { PORTC = cd; PORTC = Cd; }
  if( delay1us ) WAIT(16);  // 16 cycles = 1us delay
  PORTB |= 0x10;
}


inline void drivectrl_set(byte pin, byte state)
{
  if( state )
    drivectrl |= 1<<pin;
  else
    drivectrl &= ~(1<<pin);
  
  shift_out(drivectrl);
}


static void drivectrl_step_pulse(bool stepOut)
{
  // Take STEP line LOW
  shift_out(drivectrl & ~bit(PIN_SRO_STEP));

  // Push the step direction into the first bit of the shift register.
  // After the 8 more shifts done by shift_out() this will end up in the
  // flip-flop that controls the STEPDIR line to the drive
  PORTC &= ~0x04; // clock low
  if( stepOut ) PORTC |= 0x08; else PORTC &= ~0x08;
  PORTC |=  0x04; // clock high

  // Take the STEP line back HIGH, the LOW->HIGH edge on STEP will 
  // trigger the drive to step.
  // Calling shift_out with "wait"=true introduces a 1us delay
  // between the final shift and switching the shift register LATCH high.
  // With the final shift, the 7474 flip-flop (STEPDIR) receives its 
  // output value. The shift register updates its outputs when LATCH
  // goes high. The delay ensures that the STEPDIR signal is stable
  // 1us before the STEP signal edge, as per drive requirements.
  shift_out(drivectrl | bit(PIN_SRO_STEP), true);

  // The STEPDIR flip-flop will change its output during any shift
  // register operation. The drive requires that the step direction 
  // be stable for 5us after initiating the step pulse
  // => delay here to be sure.
  WAIT(5*16); // 5*16 cycles = 5us delay
}


// -------------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------  Drive parameters  -------------------------------------------------
// -------------------------------------------------------------------------------------------------------------------------


// supported drive types
#define DT_NONE      0  // no drive
#define DT_5INCH_HD  1  // generic 5.25" drive in HD mode (emulating 8")
#define DT_SA800     2  // 8" Shugart SA-800 drive

byte driveType[MAX_DRIVES];              // drive type (5 inch/8 inch)


void set_drive_type(byte drive, byte tp)
{
  driveType[drive] = tp;
  if( Serial ) print_drive_type_drive(drive);
}


void print_drive_type(byte tp)
{
  switch( tp )
    {
    case DT_NONE     : Serial.print(F("none"));   break;
    case DT_5INCH_HD : Serial.print(F("5.25\"")); break;
    case DT_SA800    : Serial.print(F("Shugart SA800 (8\")"));    break;
    default          : Serial.print(F("unknown"));   break;
    }
}


void print_drive_type_drive(byte drive)
{
  Serial.print(F("Drive ")); Serial.print(drive);
  Serial.print(F(" is type "));
  Serial.print(driveType[drive]); Serial.print(F(": "));
  print_drive_type(driveType[drive]);
  Serial.println();
}



// -------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------- low-level FM read/write functions ----------------------------------------
// -------------------------------------------------------------------------------------------------------------------------

#define S_OK         0  // no error
#define S_NOINDEX    1  // No index hole detected
#define S_NOTREADY   2  // Drive is not ready (no disk or power)
#define S_NOSYNC     3  // No sync marks found
#define S_NOHEADER   4  // Sector header not found
#define S_INVALIDID  5  // Sector data record has invalid id
#define S_CRC        6  // Sector data checksum error
#define S_NOTRACK0   7  // No track0 signal
#define S_VERIFY     8  // Verify after write failed
#define S_READONLY   9  // Attempt to write to a write-protected disk
#define S_ABORT   0xFF  // Command aborted



// did we receive a "clear strobe" command (i.e. a command byte with bit 0 cleared) while reading?
volatile byte clrStb = 0;


// These tables are indexed by a four bit value of transmit data bits
// in a shift register in which the 8's bit is two bits old, the 2's
// bit is the value about to be written, and the 1's bit is the future
// bit. The value retrieved is the bit pattern to write to the SPI to
// send the proper pulses to the floppy write data line. Each SPI bit
// is 125ns, so 16 SPI bits encode the 2us MFM cell time. One table
// includes write pre-compensation for higher tracks, the other table
// has no pre-comp.   
byte preComp000[32] __attribute__((aligned (32))) =
  {0xe7, 0xff,	// 0000
   0xe7, 0xff,  // 0001  
   0xff, 0xe7,  // 0010
   0xff, 0xe7,  // 0011
   0xff, 0xff,  // 0100
   0xff, 0xff,  // 0101
   0xff, 0xe7,  // 0110
   0xff, 0xe7,  // 0111
   0xe7, 0xff,  // 1000
   0xe7, 0xff,  // 1001
   0xff, 0xe7,  // 1010
   0xff, 0xe7,  // 1011  
   0xff, 0xff,  // 1100
   0xff, 0xff,  // 1101
   0xff, 0xe7,  // 1110
   0xff, 0xe7}; // 1111		


byte preComp125[32] __attribute__((aligned (32))) =
  {0xe7, 0xff,  // 0000
   0xcf, 0xff,	// 0001 early 125ns
   0xff, 0xe7,	// 0010
   0xff, 0xf3,	// 0011 late 125ns
   0xff, 0xff,	// 0100
   0xff, 0xff,	// 0101
   0xff, 0xcf,	// 0110 early 125ns
   0xff, 0xe7,	// 0111
   0xf3, 0xff,	// 1000 late 125ns
   0xe7, 0xff,	// 1001
   0xff, 0xe7,	// 1010
   0xff, 0xf3,	// 1011 late 125ns
   0xff, 0xff,	// 1100
   0xff, 0xff,	// 1101
   0xff, 0xcf,	// 1110 early 125ns
   0xff, 0xe7};	// 1111


static const uint16_t PROGMEM crc16_table[256] =
{
 0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
 0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6, 0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
 0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485, 0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
 0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4, 0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
 0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823, 0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
 0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12, 0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
 0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41, 0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
 0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70, 0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
 0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F, 0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
 0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E, 0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
 0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D, 0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
 0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C, 0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
 0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB, 0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
 0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A, 0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
 0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9, 0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
 0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8, 0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};


static uint16_t calc_crc(byte *buf, int n, bool DD)
{
  uint16_t crc = DD ? 0xCDB4 : 0xFFFF;

  // compute CRC of remaining data
  while( n-- > 0 )
    crc = pgm_read_word_near(crc16_table + (((crc >> 8) ^ *buf++) & 0xff)) ^ (crc << 8);

  return crc;
}


static bool check_pulse()
{
  // reset timer and capture/overrun flags
  TCNT1 = 0;
  TIFR1 = bit(ICF1) | bit(TOV1);

  // wait for either input capture or timer overrun
  while( !(TIFR1 & (bit(ICF1) | bit(TOV1))) );

  // if there was an input capture then we are ok
  bool res = (TIFR1 & bit(ICF1))!=0;

  // reset input capture and timer overun flags
  TIFR1 = bit(ICF1) | bit(TOV1);

  return res;
}


static void select_head(bool upper)
{
  bool isUpper = (drivectrl & bit(PIN_SRO_SIDE1))==0;

  if( upper && !isUpper )
    drivectrl_set(PIN_SRO_SIDE1, LOW);
  else if( !upper && isUpper )
    drivectrl_set(PIN_SRO_SIDE1, HIGH);
}


static bool have_disk(unsigned int wait_time = 166)
{
  TCCR1B = bit(CS12);              // prescaler /256
  TCNT1 = 0;                       // reset timer
  OCR1A = wait_time * 63;          // expire after wait_time milliseconds
  TIFR1 = bit(OCF1A);              // clear OCF1A
  PCIFR |= bit(PCIF0);             // clear INDEX change signal
  while( (TIFR1 & bit(OCF1A))==0 && (PCIFR & bit(PCIF0))==0 );

  return (PCIFR & bit(PCIF0))!=0;
}


static byte wait_index_hole(byte status = 0)
{
  // reset timer and overrun flags
  TCNT1 = 0;
  TIFR1 = bit(TOV1);
  byte ctr = 4, waitfor = 0x20;

  while( true )
    {
      if( (PINB & 0x20)==waitfor )
        {
          if( waitfor==0 )
            break;
          else
            waitfor = 0;
        }

      if( TIFR1 & bit(TOV1) )
        {
          // timer overflow happens every 262.144ms (65536 cycles at 16MHz with /64 prescaler)
          // meaning we haven't found an index hole in that amount of time
          if( ctr-- == 0 )
            {
              // we have tried for 4*262 ms = ~1 second to find an index hole
              // one rotation is 200ms so we have tried for 5 full rotations => give up
              return S_NOINDEX;
            }
          
          // clear overflow flag
          TIFR1 = bit(TOV1);
        }

      if( (PINC & 0x10) && ((PINC & 0x03)==0) )
        {
          // react to STATUS input request => return status
          PORTD = status;             // output status
          DDRD = 0xFF;                // switch data bus pins to output
          PORTB &= ~0x02;             // set WAIT signal to LOW
          while( (PINC & 0x10) );     // wait until INP signal ends
          DDRD = 0x00;                // switch data bus pins back to input
          PORTB |= 0x02;              // set WAIT signal back to HIGH
          PCIFR |= bit(PCIF1);        // reset INP/OUT pin change flag
        }
      else if( (PINC & 0x20) && ((PINC & 0x03)==0) )
        {
          byte data = PIND;
          if( data==0x00 || data==0x81 )
            {
              // received "STATUS" command => ignore
              PORTB &= ~0x02;             // set WAIT signal to LOW
              while( (PINC & 0x20) );     // wait until OUT signal ends
              PORTB |= 0x02;              // set WAIT signal back to HIGH
              PCIFR |= bit(PCIF1);        // reset INP/OUT pin change flag
              if( data==0x81 )
                return S_ABORT;           // received a CLEAR command => abort with S_ABORT
              else
                clrStb = 1;               // we received a "clear strobe" command (command with bit 0=false)
            }
        }
    }

  return S_OK;
}



byte read_sector_data_sd(byte *buffer, byte n, byte status)
{
  // calculate threshold between short pulse (2 microseconds) and long pulse (4 microseconds)
  byte res;

  asm volatile 
    (// define READPULSE macro (wait for pulse)
     // macro arguments: 
     //         length: none => just wait for pulse, don't check         ( 9 cycles)
     //                 1    => wait for pulse and jump if NOT short  (12/14 cycles)
     //                 2    => wait for pulse and jump if NOT long   (12/14 cycles)
     //         dst:    label to jump to if DIFFERENT pulse found
     // 
     // We also check for unfinished bus activity and finish it if possible.
     // This adds 12 cycles in the worst case.
     //
     // on entry: r13 contains threshold between short and long pulse
     //           r18 contains time of previous pulse
     // on exit:  r18 is updated to the time of this pulse
     //           r22 contains the pulse length in timer ticks (=processor cycles)     
     // CLOBBERS: r19
     ".macro READPULSE length=0,dst=undefined\n"
     "rp0\\@: sbrs    r16, 1\n"        // (1/2) do we have unfinished bus activity?
     "        rjmp    rp1\\@\n"        // (2)   no => go on to check for timer capture
     "        in      r19, 0x06\n"     // (1)   read PINC
     "        andi    r19, 0x30\n"     // (1)   mask bits 4+5
     "        brne    rp1\\@\n"        // (1/2) if either one is set then skip
     "        ldi     r19,  0\n"       // (1)   switch PORTD...
     "        out     0x0A, r19\n"     // (1)   back to input
     "        sbi     0x05, 1\n"       // (2)   set WAIT signal back to HIGH
     "        sbi     0x1b, 1\n"       // (2)   reset INP/OUT pin change flag (PCIFR & bit(PCI1F1))
     "        andi    r16,  253\n"     // (1)   clear flag for unfinished bus activity
     "rp1\\@: sbis    0x16, 5\n"       // (1/2) skip next instruction if timer input capture seen
     "        rjmp    rp0\\@\n"        // (2)   wait more
     "        lds     r19, 0x86\n"     // (2)   get time of input capture (ICR1L, lower 8 bits only)
     "        sbi     0x16, 5\n "      // (2)   clear input capture flag
     "        mov     r22, r19\n"      // (1)   calculate time since previous capture...
     "        sub     r22, r18\n"      // (1)   ...into r22
     "        mov     r18, r19\n"      // (1)   set r18 to time of current capture
     "  .if \\length == 1\n"           //       waiting for short pule?
     "        cp      r22, r13\n"      // (1)   compare r22 to threshold
     "        brlo   .+2\n"            // (1/2) skip jump if less
     "        rjmp   \\dst\n"          // (3)   not the expected pulse => jump to dst
     "  .else \n"
     "      .if \\length == 2\n" 
     "        cp      r22, r13\n"      // (1)   threshold < r22?
     "        brsh   .+2\n"            // (1/2) skip jump if greater
     "        rjmp   \\dst\n"          // (3)   not the expected pulse => jump to dst
     "      .endif\n"
     "  .endif\n"
     ".endm\n"

     // initialize
     "        mov     r21, %2\n"        // get number of bytes to read
     "        ldi     r19, 48\n"        // threshold value betwee short/long pulse (3 microseconds)
     "        mov     r13, r19\n"       // into r13
     "        ldi     r19, 0xFF\n"      // used when changing bus to OUTPUT
     "        mov     r15, r19\n"       // into r15
     "        ldi     r16, 0\n"         // bit 1: unfinished bus activity, bit 2: expecting clock bit, bit 3: "clear strobe" received
     "        ldi     r23, 8\n"         // read 8 bits
     "        sbi     0x16, 5\n"        // clear input capture flag
     "        ldi     %0, 0\n"          // default return status is S_OK
     "        ldi     r19, 41\n"        // timeout for NOSYNC (41*4.096ms = 167ms => no SYNC fond at all in full rotation)
     "        mov     r17, r19\n"       // into r17
     "        sbi     0x16, 0\n"        // (2)   reset timer 1 overflow flag

     // wait for long pulses followed by short pulse
     "ws0:    ldi         r20, 0\n"    // (1)   initialize long pulse counter
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"   // (2)   toggle "WRITEDATA" (debug)
#endif
     "ws1:    sbis        0x16, 0\n"   // (1/2) skip next instruction if timer overflow occurred
     "        rjmp        ws2\n"       // (2)   continue (no overflow)
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"   // (2)   toggle "WRITEDATA" (debug)
#endif
     "        sbi         0x16, 0\n"   // (2)   reset timer 1 overflow flag
     "        dec         r17\n"       // (1)   overflow happens every 4.096ms, decrement overflow counter
     "        brne        ws2\n"       // (1/2) continue if fewer than 41 overflows
     "        ldi         %0, 3\n"     // (1)   no sync found in 167ms => return status is is S_NOSYNC
     "        rjmp        rdone\n"     // (2)   done
     "ws2:    sbrs        r20, 3\n"    // (1/2) skip the following if we have already seen 8 long pulses
     "        inc         r20\n"       // (1)   increment "long pulse" counter

     "ws3a:   sbic    0x16, 5\n"       // (1/2) skip next instruction if NO timer 1 input capture seen
     "        rjmp    ws3b\n"          // (2)   input capture seen => handle it
     "        sbrc    r16, 1\n"        // (1/2) is there unfinished bus activity?
     "        rjmp    ws3d\n"          // (2)   yes, try to finish it
     "        sbic   0x06, 4\n"        // (1/2) skip next instruction if PC4 clear
     "        rjmp   ws3c\n"           // (2)   PC4 set => found INP
     "        sbis   0x06, 5\n"        // (1/2) skip next instruction if PC5 set
     "        rjmp   ws3a\n"           // (2)   PC4+5 clear => no bus activity => back to start (9 cycles)
     "        sbic   0x06, 1\n"        // (1/2) found OUT => skip next instruction if address bit 1 (PC0) clear
     "        rjmp   ws3e\n"           // (2)   address bit 1 set => writing data register
     "        in     r19, 0x09\n"      // (2)   writing control register => read data from bus
     "        cbi    0x05, 1\n"        // (2)   set WAIT signal to LOW
     "        ori    r16, 2\n"         // (1)   set flag for unfinished bus activity
     "        sbrs   r19, 0\n"         // (1/2) skip if "strobe" bit is set
     "        ori    r16, 8\n"         // (1)   set bit for "strobe cleared"
     "        cpi    r19, 0x81\n"      // (1)   is the command a CLEAR?
     "        brne   ws3a\n"           // (1/2) back to start if not (18 cycles)
     "        ldi    %0, 255\n"        // (1)   received CLEAR => return status is S_ABORT
     "        rjmp   rdone\n"          // (2)   done reading
     "ws3e:   cbi    0x05, 1\n"        // (2)   writing data register, ignore => set WAIT signal to LOW
     "        ori    r16, 2\n"         // (1)   set flag for unfinished bus activity
     "        rjmp   ws3a\n"           // (2)   back to start (16 cycles)
     "ws3c:   out    0x0B, %3\n"       // (1)   found INP => set status value on PORTD
     "        out    0x0A, r15\n"      // (1)   switch PORTD to output
     "        cbi    0x05, 1\n"        // (2)   set WAIT signal to LOW
     "        ori    r16,  2\n"        // (1)   set flag for unfinished bus activity
     "        rjmp    ws3a\n"          // (2)   back to start (14 cycles)
     "ws3d:   in     r19, 0x06\n"      // (1)   have unfinished bus activity, read PINC
     "        andi   r19, 0x30\n"      // (1)   get bits 4+5 (INP and OUT signals)
     "        brne   ws3a\n"           // (1/2) either INP or OUT still set => back to start (9 cycles)
     "        ldi    r19,  0\n"        // (1)   switch PORTD...
     "        out    0x0A, r19\n"      // (1)   back to input
     "        sbi    0x05, 1\n"        // (2)   set WAIT signal back to HIGH
     "        sbi    0x1b, 1\n"        // (2)   reset INP/OUT pin change flag (PCIFR & bit(PCI1F1))
     "        andi   r16,  253\n"      // (1)   clear flag for unfinished bus activity
     "        rjmp   ws3a\n"           // (2)   back to start (17 cycles)

     "ws3b:   lds     r19, 0x86\n"     // (2)   get time of input capture (ICR1L, lower 8 bits only)
     "        sbi     0x16, 5\n "      // (2)   clear input capture flag
     "        mov     r22, r19\n"      // (1)   calculate time since previous capture...
     "        sub     r22, r18\n"      // (1)   ...into r22
     "        mov     r18, r19\n"      // (1)   set r18 to time of current capture
     "        cp      r22, r13\n"      // (1)   compare r22 to threshold
     "        brge    ws1\n"           // (3)   count and repeat if long pulse
     "        cpi         r20, 8\n"    // (1)   did we see at least 8 long pulses?
     "        brlo        ws0\n"       // (1/2) restart if not
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"   // (2)   toggle "WRITEDATA" (debug)
#endif

     // expect ID mark 0xFE(0xC7 clock) (S)SSLLLSSSSSL (have already seen first short pulse)
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        READPULSE   2,ws0\n"     // (12)  expect long  pulse
     "        READPULSE   2,ws0\n"     // (12)  expect long  pulse
     "        READPULSE   2,ws0\n"     // (12)  expect long  pulse
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        READPULSE   1,ws3\n"     // (12)  expect short pulse (if long then expect data mark)
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        READPULSE   2,ws0\n"     // (12)  expect long  pulse
     "        ldi         r19, 254\n"  // (1)   found ID mark (0xFE)
     "        rjmp        ws5\n"

     // expect data mark 0xFB(0xC7 clock) (SSSLLLSL)SSSS (have already seen the beginning)
     "ws3:    READPULSE   1,ws4\n"     // (12)  expect short pulse (if long then expect deleted data mark)
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        READPULSE   1,ws0\n"     // (12)  expect short pulse
     "        ldi         r19, 251\n"  // (1)   found data mark (0xFB)
     "        rjmp        ws5\n"

     // expect deleted data mark 0xF8(0xC7 clock) (SSSLLLSLL)L (have already seen the beginning)
     "ws4:    READPULSE   2,ws0\n"     // (12)  expect long pulse
     "        ldi         r19, 248\n"  // (1)   found deleted data mark (0xF8)

     // found index/data/deleted data mark
     "ws5:    st          Z+, r19\n"   // store mark byte in buffer
     "        subi        r21, 1\n"    // (1)   decrement byte counter
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"   // (2)   toggle "WRITEDATA" (debug)
#endif

      // ======================  start of read loop
      
      "LR1:    sbis    0x16, 5\n"       // (1/2) skip next instruction if timer 1 input capture seen
      "        rjmp    LR2\n"           // (2)   no capture => handle bus communication

      // ======================  input capture => handle disk input (max 23 cycles)
      
      // read clock bit : 11 cycles
      // read data bit  : 18 cycles (23 cycles if new byte)
      "        lds     r19, 0x86\n"     // (2)   get time of input capture (ICR1L, lower 8 bits only)
      "        sbi     0x16, 5\n "      // (2)   clear input capture flag
      "        sbrc    r16, 2\n"        // (1/2) skip if we are expecting a data bit
      "        rjmp    LR3\n"           // (2)   expecting a clock bit

      // expecting a data bit (6 cycles so far)
      "        add     r25, r25\n"      // (1)   shift received byte left by one bit
      "        mov     r22, r19\n"      // (1)   calculate time since previous capture...
      "        sub     r22, r18\n"      // (1)   ...into r22
      "        mov     r18, r19\n"      // (1)   set r18 to time of current capture
      "        cp      r22, r13\n"      // (1)   time since previous capture < clock/data threshold?
      "        brcs    LR4\n"           // (1/2) yes => data "1" bit
      "        rjmp    rnxtbi\n"        // (2)   received "0" bit

      // found data "1" bit (13 cycles so far)
      "LR4:    ori     r16, 4\n"        // (1)   expecting clock bit next
      "        ori     r25, 1\n"        // (1)   received "1" bit
      
      // --- next bit (15 cycles so far)
      "rnxtbi: subi    r23, 1\n"        // (1)   decrement bit counter
      "        brne    LR1\n"           // (1/2) not all bits received => back to start, checking timeout (18+2 cycles)

      // --- next byte (17 cycles so far)
      "rnxtby: st      Z+, r25\n"       // (2)   store received byte
      "        ldi     r23, 8\n"        // (1)   reset bit counter
#ifdef DEBUGSIG
      "        sbi     0x03, 3\n"       // (2)   toggle "WRITEDATA" (debug)
#endif
      "        subi    r21, 1\n"        // (1)   decrement byte counter
      "        brne    LR1\n"           // (1/2) repeat until all bytes received (23 cycles)
      "        rjmp    rdone\n"         // (2)   done

      // --- expecting a clock bit (7 cycles so far)
      "LR3:    andi    r16, 251\n"      // (1)   next bit should be a data bit
      "        mov     r18, r19\n"      // (1)   set r18 to time of current capture
      "        rjmp    LR1\n"           // (2)   back to start (11+2 cycles)

      // ======================  no input capture => handle bus activity (max 20 cycles)

      "LR2:    sbrc   r16, 1\n"         // (1/2) is there unfinished bus activity?
      "        rjmp   LR9\n"            // (2)   yes, try to finish it
      "        sbic   0x06, 4\n"        // (1/2) skip next instruction if PC4 clear
      "        rjmp   LR5\n"            // (2)   PC4 set => found INP
      "        sbis   0x06, 5\n"        // (1/2) skip next instruction if PC5 set
      "        rjmp   LR1\n"            // (2)   PC4+PC5 clear => no bus activity

      // PR5 set => found OUT signal (6 cycles so far)
      "        sbic   0x06, 1\n"        // (1/2) found OUT => skip next instruction if address bit 1 (PC0) clear
      "        rjmp   LR6\n"            // (2)   bit 1 set => writing data register
      "        in     r19, 0x09\n"      // (2)   writing control register => read data from bus
      "        cbi    0x05, 1\n"        // (2)   set WAIT signal to LOW
      "        ori    r16, 2\n"         // (1)   set flag for unfinished bus activity
      "        sbrs   r19, 0\n"         // (1/2) skip if "strobe" bit is set
      "        ori    r16, 8\n"         // (1)   set bit for "strobe cleared"
      "        cpi    r19, 0x81\n"      // (1)   is the command a CLEAR?
      "        brne   LR1\n"            // (1/2) back to start if not (16 cycles)
      "        ldi    %0, 255\n"        // (1)   received CLEAR => return status is S_ABORT
      "        rjmp   rdone\n"          // (2)   done reading
      "LR6:    cbi    0x05, 1\n"        // (2)   writing data register, ignore => set WAIT signal to LOW
      "        ori    r16, 2\n"         // (1)   set flag for unfinished bus activity
      "        rjmp   LR1\n"            // (2)   back to start (14 cycles)

      // PC4 set => found INP signal (5 cycles so far)
      "LR5:    out    0x0B, %3\n"       // (1)   found INP => set status value on PORTD
      "        out    0x0A, r15\n"      // (1)   switch PORTD to output
      "        cbi    0x05, 1\n"        // (2)   set WAIT signal to LOW
      "        ori    r16,  2\n"        // (1)   set flag for unfinished bus activity
      "        rjmp    LR1\n"           // (2)   back to start (14 cycles)

      // found unfinished bus activity (3 cycles so far)
      "LR9:    in     r19, 0x06\n"      // (1)   read PINC
      "        andi   r19, 0x30\n"      // (1)   get bits 4+5 (INP and OUT signals)
      "        breq   LR9a\n"           // (1/2) if both are 0 then finish now
      "        rjmp   LR1\n"            // (2)   can't finish yet (8+2 cycles)
      "LR9a:   ldi    r19,  0\n"        // (1)   switch PORTD...
      "        out    0x0A, r19\n"      // (1)   back to input
      "        sbi    0x05, 1\n"        // (2)   set WAIT signal back to HIGH
      "        sbi    0x1b, 1\n"        // (2)   reset INP/OUT pin change flag (PCIFR & bit(PCI1F1))
      "        andi   r16,  253\n"      // (1)   clear flag for unfinished bus activity
      "        rjmp   LR1\n"            // (2)   back to start (16 cycles)

      // ======================  done reading - check for unfinished bus activity
      "rdone:  sbrs    r16, 1\n"        // (1/2) do we have unfinished bus activity?
      "        rjmp    rdon2\n"         // (2)   no => done
      "wtbus:  in      r19, 0x06\n"     // (1)   read PINC
      "        andi    r19, 0x30\n"     // (1)   mask bits 4+5
      "        brne    wtbus\n"         // (1/2) if either one is set then wait
      "        ldi     r19,  0\n"       // (1)   switch PORTD...
      "        out     0x0A, r19\n"     // (1)   back to input
      "        sbi     0x05, 1\n"       // (2)   set WAIT signal back to HIGH
      "        sbi     0x1b, 1\n"       // (2)   reset INP/OUT pin change flag (PCIFR & bit(PCI1F1))
      "rdon2:  andi    r16, 8\n"        // (1)   isolate "strobe received" status bit
      "        lds     r19, clrStb\n"   // (1)   get current "clear strobe" variable
      "        or      r19, r16\n"      // (1)   OR "clear strobe" status bit into variable
      "        sts     clrStb, r19\n"   // (1)   store variable
#ifdef DEBUGSIG
      "        sbi     0x05, 3\n"       // (2)   set "WRITEDATA" high (debug)
#endif

      : "=r"(res)        // outputs
      : "z"(buffer), "d"(n), "l"(status)  // inputs  (z=r30/r31)
     : "r13", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r25", "r26", "r27"); // clobbers (x=r26/r27)

  return res;
}


byte read_sector_data_dd(byte *buffer, unsigned int n, byte status)
{
  byte res;

  // expect at least 10 bytes of 0x00 followed by three sync marks (0xA1 with one missing clock bit)
  // Data bits :     0 0 ...0  1 0 1 0 0*0 0 1  1 0 1 0 0*0 0 1  1 0 1 0 0*0 0 1
  // In MFM    : (0)1010...10 0100010010001001 0100010010001001 0100010010001001

  asm volatile 
    (
     // define READPULSE macro (wait for pulse)
     // macro arguments: 
     //         length: none => just wait for pulse, don't check         ( 9 cycles)
     //                 1    => wait for pulse and jump if NOT short  (12/14 cycles)
     //                 2    => wait for pulse and jump if NOT medium (14/16 cycles)
     //                 3    => wait for pulse and jump if NOT long   (12/14 cycles)
     //         dst:    label to jump to if DIFFERENT pulse found
     //
     // if length>0 then we also check for unfinished bus activity and
     // finish it if possible. This adds 12 cycles in the worst case.
     //
     // on entry: r16 contains minimum length of medium pulse
     //           r17 contains minimum length of long   pulse
     //           r18 contains time of previous pulse
     // on exit:  r18 is updated to the time of this pulse
     //           r22 contains the pulse length in timer ticks (=processor cycles)     
     // CLOBBERS: r19
     ".macro READPULSEDD length=0,dst=undefined\n"
     "  .if \\length>0\n"
     "rpd0\\@: sbrs    r23, 1\n"       // (1/2) do we have unfinished bus activity?
     "         rjmp    rpd1\\@\n"      // (2)   no => go on to check for timer capture
     "         in      r19, 0x06\n"    // (1)   read PINC
     "         andi    r19, 0x30\n"    // (1)   mask bits 4+5
     "         brne    rpd1\\@\n"      // (1/2) if either one is set then skip
     "         ldi     r19,  0\n"      // (1)   switch PORTD...
     "         out     0x0A, r19\n"    // (1)   back to input
     "         sbi     0x05, 1\n"      // (2)   set WAIT signal back to HIGH
     "         sbi     0x1b, 1\n"      // (2)   reset INP/OUT pin change flag (PCIFR & bit(PCI1F1))
     "         andi    r23,  253\n"    // (1)   clear flag for unfinished bus activity
     "rpd1\\@: sbis    0x16, 5\n"      // (1/2) skip next instruction if timer input capture seen
     "         rjmp    rpd0\\@\n"      // (2)   wait more
     "  .else\n"
     "        sbis    0x16, 5\n"       // (1/2) skip next instruction if timer input capture seen
     "        rjmp    .-4\n"           // (2)   wait more 
     "  .endif\n"
     "        lds     r19, 0x86\n"     // (2)   get time of input capture (ICR1L, lower 8 bits only)
     "        sbi     0x16, 5\n "      // (2)   clear input capture flag
     "        mov     r22, r19\n"      // (1)   calculate time since previous capture...
     "        sub     r22, r18\n"      // (1)   ...into r22
     "        mov     r18, r19\n"      // (1)   set r18 to time of current capture
     "  .if \\length == 1\n"           //       waiting for short pulse?
     "        cp      r22, r16\n"      // (1)   compare r22 to min medium pulse
     "        brlo   .+2\n"            // (1/2) skip jump if less
     "        rjmp   \\dst\n"          // (2)   not the expected pulse => jump to dst
     "  .else \n"
     "    .if \\length == 2\n"         // waiting for medium pulse?
     "        cp      r16, r22\n"      // (1)   min medium pulse < r22? => carry set if so
     "        brcc    .+2\n"           // (1/2) skip next instruction if carry is clear
     "        cp      r22, r17\n"      // (1)   r22 < min long pulse? => carry set if so
     "        brcs   .+2\n"            // (1/2) skip jump if greater
     "        rjmp   \\dst\n"          // (2)   not the expected pulse => jump to dst
     "    .else\n"
     "      .if \\length == 3\n" 
     "        cp      r22, r17\n"      // (1)   min long pulse < r22?
     "        brsh   .+2\n"            // (1/2) skip jump if greater
     "        rjmp   \\dst\n"          // (2)   not the expected pulse => jump to dst
     "      .endif\n"
     "    .endif\n"
     "  .endif\n"
     ".endm\n"

     // define STOREBIT macro for storing data bit 
     // 5/12 cycles for "1", 4/11 cycles for "0"
     ".macro STOREBIT data:req,done:req\n"
     "        lsl     r20\n"           // (1)   shift received data
     ".if \\data != 0\n"
     "        ori     r20, 1\n"        // (1)   store "1" bit
     ".endif\n"
     "        dec     r21\n"           // (1)   decrement bit counter
     "        brne    stb\\@\n"        // (1/2) skip if bit counter >0
     "        st      Z+, r20\n"       // (2)   store received data byte
#ifdef DEBUGSIG
     "        sbi     0x03, 3\n"       // (2)   toggle "WRITEDATA" (debug)
#endif
     "        ldi     r21, 8\n"        // (1)   re-initialize bit counter
     "        subi    r26, 1\n"        // (1)   subtract one from byte counter
     "        sbci    r27, 0\n"        // (1) 
     "        brmi    \\done\n"        // (1/2) done if byte counter<0
     "stb\\@:\n"
     ".endm\n"

     // prepare for reading SYNC
     "        ldi         r16, 40\n"    // (1)   r16 = 2.5 * (MFM bit len) = minimum length of medium pulse
     "        ldi         r17, 56\n"    // (1)   r17 = 3.5 * (MFM bit len) = minimum length of long pulse
     "        ldi         %0, 0\n"      // (1)   default return status is S_OK
     "        ldi         r19, 41\n"    // (1)   timeout for NOSYNC (41*4.096ms = 167ms => no SYNC fond at all in full rotation)
     "        mov         r15, r19\n"   // (1)   into r15
     "        ldi         r19, 0xFF\n"  // (1)   used when changing bus to OUTPUT
     "        mov         r14, r19\n"   // (1)   into r14
     "        sbi         0x16, 0\n"    // (2)   reset timer overflow flag
     "        ldi         r23, 0\n"     // (1)   bit 1: unfinished bus activity, bit 3: "clear strobe" received

     // wait for at least 8x "10" (short) pulse followed by "100" (medium) pulse
     "wsd0:   ldi         r20, 0\n"     // (1)   initialize "short pulse" counter
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"    // (2)   toggle "WRITEDATA" (debug)
#endif
     "wsd1:   sbis        0x16, 0\n"    // (1/2) skip next instruction if timer overflow occurred
     "        rjmp        wsd2\n"       // (2)   continue (no overflow)
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"    // (2)   toggle "WRITEDATA" (debug)
#endif
     "        sbi         0x16, 0\n"    // (2)   reset timer overflow flag
     "        dec         r15\n"        // (1)   overflow happens every 4.096ms, decrement overflow counter
     "        brne        wsd2\n"       // (1/2) continue if fewer than 41 overflows
     "        ldi         %0, 3\n"      // (1)   no sync found in 167ms => return status is is S_NOSYNC
     "        rjmp        rdoned\n"     // (2)   done
     "wsd2:   sbrs        r20, 3\n"     // (1/2) skip the following if we have already seen 8 short pulses
     "        inc         r20\n"        // (1)   increment "short pulse" counter

     "wsd3a:  sbic    0x16, 5\n"        // (1/2) skip next instruction if NO timer 1 input capture seen
     "        rjmp    wsd3b\n"          // (2)   input capture seen => handle it
     "        sbrc    r23, 1\n"         // (1/2) is there unfinished bus activity?
     "        rjmp    wsd3d\n"          // (2)   yes, try to finish it
     "        sbic   0x06, 4\n"         // (1/2) skip next instruction if PC4 clear
     "        rjmp   wsd3c\n"           // (2)   PC4 set => found INP
     "        sbis   0x06, 5\n"         // (1/2) skip next instruction if PC5 set
     "        rjmp   wsd3a\n"           // (2)   PC4+5 clear => no bus activity => back to start (9 cycles)
     "        sbic   0x06, 1\n"         // (1/2) found OUT => skip next instruction if address bit 1 (PC0) clear
     "        rjmp   wsd3e\n"           // (2)   address bit 1 set => writing data register
     "        in     r19, 0x09\n"       // (2)   writing control register => read data from bus
     "        cbi    0x05, 1\n"         // (2)   set WAIT signal to LOW
     "        ori    r23, 2\n"          // (1)   set flag for unfinished bus activity
     "        sbrs   r19, 0\n"          // (1/2) skip if "strobe" bit is set
     "        ori    r23, 8\n"          // (1)   set bit for "strobe cleared"
     "        cpi    r19, 0x81\n"       // (1)   is the command a CLEAR?
     "        brne   wsd3a\n"           // (1/2) back to start if not (18 cycles)
     "        ldi    %0, 255\n"         // (1)   received CLEAR => return status is S_ABORT
     "        rjmp   rdoned\n"          // (2)   done reading
     "wsd3e:  cbi    0x05, 1\n"         // (2)   writing data register, ignore => set WAIT signal to LOW
     "        ori    r23, 2\n"          // (1)   set flag for unfinished bus activity
     "        rjmp   wsd3a\n"           // (2)   back to start (16 cycles)
     "wsd3c:  out    0x0B, %3\n"        // (1)   found INP => set status value on PORTD
     "        out    0x0A, r14\n"       // (1)   switch PORTD to output
     "        cbi    0x05, 1\n"         // (2)   set WAIT signal to LOW
     "        ori    r23,  2\n"         // (1)   set flag for unfinished bus activity
     "        rjmp    wsd3a\n"          // (2)   back to start (14 cycles)
     "wsd3d:  in     r19, 0x06\n"       // (1)   have unfinished bus activity, read PINC
     "        andi   r19, 0x30\n"       // (1)   get bits 4+5 (INP and OUT signals)
     "        brne   wsd3a\n"           // (1/2) either INP or OUT still set => back to start (9 cycles)
     "        ldi    r19,  0\n"         // (1)   switch PORTD...
     "        out    0x0A, r19\n"       // (1)   back to input
     "        sbi    0x05, 1\n"         // (2)   set WAIT signal back to HIGH
     "        sbi    0x1b, 1\n"         // (2)   reset INP/OUT pin change flag (PCIFR & bit(PCI1F1))
     "        andi   r23,  253\n"       // (1)   clear flag for unfinished bus activity
     "        rjmp   wsd3a\n"           // (2)   back to start (17 cycles)

     "wsd3b:  lds     r19, 0x86\n"     // (2)   get time of input capture (ICR1L, lower 8 bits only)
     "        sbi     0x16, 5\n "      // (2)   clear input capture flag
     "        mov     r22, r19\n"      // (1)   calculate time since previous capture...
     "        sub     r22, r18\n"      // (1)   ...into r22
     "        mov     r18, r19\n"      // (1)   set r18 to time of current capture
     "        cp      r22, r16\n"      // (1)   compare r22 to "medium pulse" threshold
     "        brlo    wsd1\n"          // (2)   count and repeat if short pulse
     "        cp      r22, r17\n"      // (1)   compare r22 to "long pulse" threshold
     "        brsh    wsd0\n"          // (2)   restart if long pulse (expecting medium)
     "        cpi     r20, 8\n"        // (1)   did we see at least 8 short pulses?
     "        brlo    wsd0\n"          // (1/2) restart if not

     // expect remaining part of first sync mark (..00010010001001)
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"      // (2)   toggle "WRITEDATA" (debug)
#endif
     "        READPULSEDD   3,wsd0\n"     // (12)  expect long pulse (0001)
     "        READPULSEDD   2,wsd0\n"     // (14)  expect medium pulse (001)
     "        READPULSEDD   3,wsd0\n"     // (12)  expect long pulse (0001)
     "        READPULSEDD   2,wsd0\n"     // (14)  expect medium pulse (001)

     // expect second sync mark (0100010010001001)
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"      // (2)   toggle "WRITEDATA" (debug)
#endif
     "        READPULSEDD   1,wsd0\n"     // (12)  expect short pulse (01)
     "        READPULSEDD   3,wsd0\n"     // (12)  expect long pulse (0001)
     "        READPULSEDD   2,wsd0\n"     // (14)  expect medium pulse (001)
     "        READPULSEDD   3,wsd0\n"     // (12)  expect long pulse (0001)
     "        READPULSEDD   2,wsd0\n"     // (14)  expect medium pulse (001)

     // expect third sync mark (0100010010001001)
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"      // (2)   toggle "WRITEDATA" (debug)
#endif
     "        READPULSEDD   1,wsd0\n"     // (12)  expect short pulse (01)
     "        READPULSEDD   3,wsd0\n"     // (12)  expect long pulse (0001)
     "        READPULSEDD   2,wsd0\n"     // (14)  expect medium pulse (001)
     "        READPULSEDD   3,wsd0\n"     // (12)  expect long pulse (0001)
     "        READPULSEDD   2,wsd0\n"     // (14)  expect medium pulse (001)

     // found SYNC => prepare for reading data
#ifdef DEBUGSIG
     "        sbi         0x03, 3\n"   // (2)   toggle "WRITEDATA" (debug)
#endif

     // we can't handle bus activity while reading data so if there still
     // is unfinished activity now we have no choice but to start over,
     // since it has been >40us since any activity was STARTED this should
     // never happen in regular operation
     "        sbrc    r23, 1\n"           // (1/2) do we still have unfinished bus activity?
     "        rjmp    wsd0\n"             // (2)   yes, restart => MUST wait longer for INP/OUT to be released

     "        ldi     r21, 8\n"        // (1)   initialize bit counter (8 bits per byte)

     // odd section (previous data bit was "1", no unprocessed MFM bit)
     // shortest path: 19 cycles, longest path: 34 cycles
     // (longest path only happens when finishing a byte, about every 5-6 pulses)
     "rdo:    READPULSEDD\n"             // (9)   wait for pulse
     "        cp      r22, r16\n"      // (1)   pulse length >= min medium pulse?
     "        brlo    rdos\n"          // (1/2) jump if not
     "        cp      r22, r17\n"      // (1)   pulse length >= min long pulse?
     "        brlo    rdom\n"          // (1/2) jump if not

     // long pulse (0001) => read "01", still odd
     "        STOREBIT 0,rddone\n"      // (4/13) store "0" bit
     "        STOREBIT 1,rddone\n"      // (5/14) store "1" bit
     "        rjmp    rdo\n"            // (2)    back to start (still odd)

     // jump target for relative conditional jumps in STOREBIT macro
     "rddone:  rjmp    rdoned\n"
     
     // medium pulse (001) => read "0", now even
     "rdom:   STOREBIT 0,rddone\n"      // (4/13) store "0" bit
     "        rjmp    rde\n"            // (2)   back to start (now even)

     // short pulse (01) => read "1", still odd
     "rdos:   STOREBIT 1,rddone\n"      // (5/14) store "1" bit
     "        rjmp    rdo\n"            // (2)    back to start (still odd)

     // even section (previous data bit was "0", previous MFM "1" bit not yet processed)
     // shortest path: 19 cycles, longest path: 31 cycles
     "rde:    READPULSEDD\n"             // (9)   wait for pulse
     "        cp      r22, r16\n"      // (1)   pulse length >= min medium pulse?
     "        brlo    rdes\n"          // (1/2) jump if not

     // either medium pulse (1001) or long pulse (10001) => read "01"
     // (a long pulse should never occur in this section but it may just be a 
     //  slightly too long medium pulse so count it as medium)
     "        STOREBIT 0,rdoned\n"      // (4/13) store "0" bit
     "        STOREBIT 1,rdoned\n"      // (5/14) store "1" bit
     "        rjmp    rdo\n"           // (2)    back to start (now odd)

     // short pulse (101) => read "0"
     "rdes:   STOREBIT 0,rdoned\n"      // (5/14) store "0" bit
     "        rjmp    rde\n"           // (2)    back to start (still even)

     // ======================  done reading - check for unfinished bus activity

     "rdoned:  andi    r23, 8\n"        // (1)   isolate "strobe received" status bit
     "         lds     r19, clrStb\n"   // (1)   get current "clear strobe" variable
     "         or      r19, r23\n"      // (1)   OR "clear strobe" status bit into variable
     "         sts     clrStb, r19\n"   // (1)   store variable
#ifdef DEBUGSIG
     "         sbi     0x05, 3\n"       // (2)   set "WRITEDATA" high (debug)
#endif
     
     : "=r"(res)                            // outputs
     : "x"(n-1), "z"(buffer), "l"(status)   // inputs  (x=r26/r27, z=r30/r31)
     : "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23");  // clobbers

  return res;
}



// macros and subroutines used by write_sector_data_sd() and format_track_sd()
asm volatile
    (".macro    WRTPS\n"
     "          sts   0xB3, r16\n"
     "          call  waitp\n"    
     ".endm\n"
     ".macro    WRTPL\n"
     "          sts   0xB3, r18\n"
     "          call  waitp\n"
     ".endm\n"

     // write short pulses
     //  r20 contains number of short pulses to write
     //  => takes 6 cycles until timer is initialized (including call)
     //  => returns 20 cycles (max) after final pulse is written (including return statement)
     "wrtshort: WRTPS\n"
     "          dec r20\n"                 // (1)
     "          brne wrtshort\n"           // (1/2)
     "          ret\n"                     // (4)

     // write long pulses
     //  r20 contains number of short pulses to write
     //  => takes 6 cycles until timer is initialized (including call)
     //  => returns 20 cycles (max) after final pulse is written (including return statement)
     "wrtlong:  WRTPL\n"
     "          dec r20\n"                 // (1)
     "          brne wrtlong\n"            // (1/2)
     "          ret\n"                     // (4)

     // wait for pulse to be written
     // => returns 14 cycles (max) after pulse is written (including return statement)
     "waitp:    sbis  0x17, 1\n"          // (1/2) skip next instruction if OCF2A is set
     "          rjmp  .-4\n"              // (2)   wait more
     "          ldi   r19, 0x81\n"        // (1)   
     "          sts   0xB1, r19\n"        // (2)   set OCP back HIGH (was set LOW when timer expired)
     "          sbi   0x17, 1\n"          // (2)   reset OCF2A (output compare flag)
     "          ret\n"                    // (4)   return
     );


static void write_sector_data_sd(const byte *buffer, byte status)
{
  // make sure OC2A is high before we enable WRITE_GATE
  TCCR2B = bit(CS20);                  // prescaler 1
  DDRB   &= ~0x08;                     // disable OC2A pin
  TCCR2A  = bit(COM2A1) | bit(COM2A0); // set OC2A on compare match
  TCCR2B |= bit(FOC2A);                // force compare match
  TCCR2A  = 0;                         // disable OC2A control by timer
  DDRB   |= 0x08;                      // enable OC2A pin

  // wait through beginning of header gap (22 bytes of 0x4F)
  TCCR1B = bit(CS20);               // prescaler /1
  TCNT1 = 0;                        // reset timer
  OCR1A = 11*8*4*16 - 20*16;        // 88 bit lengths (11 bytes * 8 bits/byte * 4 us/bit * 16 cycles/us - 20us (overhead))
  TIFR1 = bit(OCF1A);               // clear OCFx
  while( !(TIFR1 & bit(OCF1A)) );   // wait for OCFx

  // enable write gate (takes about 2 microseconds)
  drivectrl_set(PIN_SRO_WRITEGATE, LOW);

  // enable OC2A output pin control by timer (WRITE_DATA), initially high
  TCNT2  = 0;
  OCR2A  = 255;
  TIFR2  = bit(OCF2A);    // clear output compare flag
  TCCR2A = bit(COM2A0) | bit(WGM21);   // COM2A1:0 =  01 => toggle OC2A on compare match, WGMx2:10 = 010 => clear-timer-on-compare (CTC) mode

  asm volatile
    (// initialize
     "          ldi    r16, 31\n"        // r16 = 2us for short pulse
     "          ldi    r18, 63\n"        // r18 = 4us for long pulse

     // write 6x 0x00 = 6 * 8 = 48 long pulses (6*8*4 = 192us)
     "          ldi   r20, 48\n"
     "          call  wrtlong\n"

     "          ld     r20, Z+\n"        // load data mark
     "          cpi    r20, 248\n"       // is it DELETED data mark (0xF8)
     "          breq   wddm\n"           // jump if so

     // write data mark (0xFB data bit pattern, 0xC7 clock bit pattern) (32us)
     // 
     // CLK : (1)  1   0   0   0   1   1   1  (1)
     // DATA:    1   1   1   1   1   0   1   1
     //          1 1 1 0 1 0 1 0 1 1 0 1 1 1 1 1
     //          S S S   L   L   L S   L S S S S
     // => SSSLLLSLSSSS
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          rjmp  wi\n"

     // write DELETED data mark (0xF8 data bit pattern, 0xC7 clock bit pattern) (32us)
     // 
     // CLK : (1)  1   0   0   0   1   1   1  (1)
     // DATA:    1   1   1   1   1   0   0   0
     //          1 1 1 0 1 0 1 0 1 1 0 1 0 1 0 1
     //          S S S   L   L   L S   L   L   L
     // => SSSLLLSLLL
     "wddm:     WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPL\n"

     // write data
     "wi:       ldi    r21, 0\n"         // (1)   initialize bit counter to fetch next byte
     "          ldi    r26, 131\n"       // (1)   initialize byte counter (130 bytes to write)
     "wil:      dec     r21\n"           // (1)   decrement bit counter
     "          brpl    wib\n"           // (1/2) skip the following if bit counter >=  0
     "          subi    r26, 1\n"        // (1)   subtract one from byte counter
     "          breq    widone\n"        // (1/2) done if byte counter =0
     "          ld	r20, Z+\n"       // (2)   get next byte
     "          ldi     r21, 7\n"        // (1)   reset bit counter (7 more bits after this first one)
     "wib:      rol     r20\n"           // (1)   get next data bit into carry
     "          brcc    wi0\n"           // (1/2) jump if bit is "0"
     "          WRTPS\n"                 //       write data bit
     "          WRTPS\n"                 //       write clock bit
     "          rjmp   wil\n"            // (2)   next bit
     "wi0:      WRTPL\n"                 //       write clock bit
     "          rjmp   wil\n"            // (2)   next bit
     "widone:   "

     :                                   // no outputs
     : "z"(buffer)                       // inputs  (z=r30/r31)
     : "r16", "r18", "r19", "r20", "r21", "r26", "r27"); // clobbers

  // COM2A1:0 = 00 => disconnect OC1A (will go high)
  TCCR2A = 0;

  // disable write gate
  drivectrl_set(PIN_SRO_WRITEGATE, HIGH);

  // COM2A1:0 = 00 => disconnect OC2A (will go high)
  TCCR2A = 0;
}


static void write_sector_data_dd(byte *buffer, byte track, byte status)
{
  //byte *precomp = preComp000;
  byte *precomp = track < 60 ? preComp000 : preComp125;

  // make sure WRITEDATA pin is high before we enable WRITEGATE
  PORTB |= 0x08;

  // wait through beginning of header gap (22 bytes of 0x4F)
  TCCR1B |= bit(WGM12);             // WGMx2:10 = 010 => clear-timer-on-compare (CTC) mode 
  TCNT1 = 0;                        // reset timer
  OCR1A = 11*8*4*16 - 24*16;        // 88 bit lengths (11 bytes * 8 bits/byte * 4 us/bit * 16 cycles/us - 24us (overhead))
  TIFR1 = bit(OCF1A);               // clear OCFx
  while( !(TIFR1 & bit(OCF1A)) );   // wait for OCFx

  // enable write gate (takes about 2 microseconds)
  drivectrl_set(PIN_SRO_WRITEGATE, LOW);

  // Enabling SPI configures MISO (PB4) as input. We have an external 10k pull-down resistor
  // on PB4. Currently PORTB4 is HIGH which will make PB4 use an internal pull-up resistor
  // when configured as an input, causing it to float somewhere between 0-5V. 
  // Setting PORTB4 to LOW now will disable the internal pull-up resistor, and keep PB4 low. 
  // Note that the latch of the 74HC595 shift register to which PB4 is connected triggers 
  // on the LOW->HIGH edge which means that leaving it LOW does not do anything. PB4 will
  // be set HIGH during the drivectrl_set() call to disable the write gate after writing the data.
  PORTB &= ~0x10;

  // Enable SPI in master mode with a clock rate of fck/2
  SPCR = (1<<SPE)|(1<<MSTR);
  SPSR = bit(SPI2X);

  // The assembly code below uses SPI to output the pulses to the drive. SPI is set to a
  // clock rate of fcp/2=8 MHz, i.e. 1us per byte. MFM sequences are organized in pairs
  // of two MFM bits with each MFM bit taking one microsecond (16 cycles).
  // The SPI data byte patterns are read from the preCompXXX tables above allowing for 
  // write precompenation. SPI on the ATMega328 can not output a continuous waveform
  // since it is only single-buffered. The CPU has to wait until one SPI transmission is 
  // finished before writing the next to the SPI output register. Doing so takes time so 
  // the timing between the last bit of a byte and the first bit of the following byte is
  // disrupted. However, in MFM, pulses are never closer than 2us so there is always
  // at least a 1us (one byte) long break in between two pulses. The timing of the code 
  // below is organized such that it initiates a pulse output (precomp table value != 0xFF)
  // at the proper interval while skipping SPI transmissions for a no-pulse intervals
  // (precomp table value == 0xFF). Timing (P=pulse, N=no pulse):
  // PN -> NP       : 48
  // PN -> PN       : 32
  // PN -> NN -> NP : invalid (too long)
  // PN -> NN -> PN : 64
  // PN -> NN -> NN : invalid (too long)
  // NP -> NP       : 32
  // NP -> PN       : invalid (too short)
  // NP -> NN -> NP : 64
  // NP -> NN -> PN : 48
  // NP -> NN -> NN : invalid (too long)

  asm volatile
    (".macro GETNEXTSEQ\n" // 19 cycles
     "          dec     r17         \n"        // (1)   decrement bit counter
     "          brmi    gns1\\@     \n"        // (1/2) get next byte if bit counter <  0
     "          WAIT    5           \n"        // (5)   equalize timing
     "          rjmp    gns2\\@     \n"        // (2)   get MFM squence
     "gnsd\\@:  rjmp    wdone       \n"        // (2)   "hop" for relative jump below
     "gns1\\@:  subi    r22, 1      \n"        // (1)   subtract one from byte counter
     "          sbci    r23, 0      \n"        // (1) 
     "          brmi    gnsd\\@     \n"        // (1/2) done if byte counter <0
     "          ld      r16, Z+     \n"        // (2)   get next byte
     "          ldi     r17, 7      \n"        // (1)   reset bit counter (7 more bits after this first one)
     "gns2\\@:  rol     r16         \n"        // (1)   get next data bit into carry
     "          rol     r18         \n"        // (1)   get next data bit from carry into shift register
     "          andi    r18, 0x0F   \n"        // (1)   zero-out old bits in shift register
     "          movw    Y,   X      \n"        // (1)   get precomp table address (must be 32-byte aligned)
     "          add     r28, r18    \n"        // (1)   add 2x shift register content (r28=low-byte of Y)
     "          add     r28, r18    \n"        // (1) 
     "          ld      r20, Y+     \n"        // (2)   get first  MFM sequence
     "          ld      r21, Y      \n"        // (2)   get second MFM sequence
     ".endm                         \n"

     // initialize registera
     "          push    r28\n"
     "          push    r29\n"
     "          ldi     r17,  0     \n"        // (1)   initialize bit counter to fetch next byte
     "          ldi     r22,  4     \n"        // (1)   initialize byte counter low byte
     "          ldi     r23,  1     \n"        // (1)   initialize byte counter high byte
     "          ldi     r19,  0xFF  \n"        // (1)
     "          mov     r15,  r19   \n"        // (1)   used in "cpse" instruction below

     // initialize SPI pulse waveforms for writing SYNC
     "          ldi     r20, 0xE7   \n"         // regular timing
     "          movw    Y, Z        \n"
     "          movw    Z, X        \n"
     "          ldd     r21, Z+7    \n"         // get precomp timing for 00(1)1
     "          ldd     r18, Z+13   \n"         // get precomp timing for 01(1)0
     "          movw    Z, Y        \n"

     // write 12 bytes (96 bits) of "0" (i.e. 96 "10" sequences)
     "          ldi     r25, 95     \n"
     "wlp1:     WAIT    28          \n"         // (28)
     "          out     0x2E, r20   \n"         // (1)
     "          subi    r25, 1      \n"         // (1)
     "          brne    wlp1        \n"         // (1/2)
     
     //  0x00             0xA1             0xA1             0xA1
     //  0 0 0 0 0 0 0 0  1 0 1 0 0*0 0 1  1 0 1 0 0*0 0 1  1 0 1 0 0*0 0 1
     // 1010101010101010 0100010010001001 0100010010001001 0100010010001001
     // S S S S S S S S   M   L  M   L  M  S   L  M   L  M  S   L  M   L  M

     "          WAIT    45          \n"         // (45) M  1
     "          out     0x2E, r20   \n"         // (1)
     "          WAIT    63          \n"         // (63) L  01
     "          out     0x2E, r20   \n"         // (1)
     "          WAIT    47          \n"         // (47) M  0
     "          out     0x2E, r20   \n"         // (1)
     "          WAIT    63          \n"         // (63) L  00
     "          out     0x2E, r20   \n"         // (1)
     "          WAIT    47          \n"         // (47) M  01
     "          out     0x2E, r21   \n"         // (1)  use precomp timing for 00(1)1

     "          WAIT    31          \n"         // (31) S  1
     "          out     0x2E, r18   \n"         // (1)  use precomp timing for 01(1)0
     "          WAIT    63          \n"         // (63) L  01
     "          out     0x2E, r20   \n"         // (1)
     "          WAIT    47          \n"         // (47) M  0
     "          out     0x2E, r20   \n"         // (1)
     "          WAIT    63          \n"         // (63) L  00
     "          out     0x2E, r20   \n"         // (1)
     "          WAIT    47          \n"         // (47) M  01
     "          out     0x2E, r21   \n"         // (1)  use precomp timing for 00(1)1

     "          WAIT    31          \n"         // (31) S  1
     "          out     0x2E, r18   \n"         // (1)  use precomp timing for 01(1)0
     "          WAIT    63          \n"         // (63) L  01
     "          out     0x2E, r20   \n"         // (1)
     "          WAIT    47          \n"         // (47) M  0
     "          out     0x2E, r20   \n"         // (1)

     // prepare for writing final two bits of SYNC byte ("01")
     "          ldi     r18, 1      \n"         // (1)  previous bit was 0, current bit is 0, next bit is 1
     "          WAIT    60          \n"         // (60) L 00
     "          rjmp    PN          \n"         // (2)  current (second-to-last bit of SYNC) is PN

     // pulse in second half (NP)
     "NPc:      WAIT 1              \n"         // (1)
     "NPb:      WAIT 5              \n"         // (5)
     "NPa:      WAIT 9              \n"         // (9)
     "          out  0x2E, r21      \n"         // (1)   output pulse
     "          GETNEXTSEQ          \n"         // (19)
     "          cpi  r21, 0xFF      \n"         // (1)   is next pulse NP?
     "          brne NPa            \n"         // (1/2) jump if so, otherwise NN since NP->PN is invalid

     // no pulse (NN) following pulse in second half (NP), 22 cycles since previous pulse
     "          GETNEXTSEQ          \n"         // (19)
     "          WAIT 4              \n"         // (4)
     "          cpi  r20, 0xFF      \n"         // (1)   is next pulse PN?
     "          brne PN             \n"         // (1/2) jump if so
     "          rjmp NPc            \n"         // (2)   otherwise NP since NP->NN->NN is invalid

     // pulse in first half (PN)
     "PN:       out  0x2E, r20      \n"         // (1)   output pulse
     "          GETNEXTSEQ          \n"         // (19)
     "          WAIT 9              \n"         // (9)
     "          cpi  r20, 0xFF      \n"         // (1)   is next pulse PN?
     "          brne PN             \n"         // (1/2) jump if so
     "          cpse r21, r15       \n"         // (1/2) is next pulse NN? skip next instruction if so
     "          rjmp NPb            \n"         // (2)   next pulse is NP

     // no pulse (NN) following pulse in first half (PN), 33 cycles since previous pulse
     "          GETNEXTSEQ          \n"         // (19)
     "          WAIT 10             \n"         // (10)
     "          rjmp PN             \n"         // (2)   next must be PN since both PN->NN->NN and PN->NN->NP are invalid

     "wdone:    pop r29\n"
     "          pop r28\n"
     : 
     : "x"(precomp), "z"(buffer)
     : "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r25");

  // disable SPI
  SPCR = 0;

  // disable write gate
  drivectrl_set(PIN_SRO_WRITEGATE, HIGH);

  // WGMx2:10 = 000 => Normal timer mode
  TCCR1B &= ~bit(WGM12);
}



static bool format_track_sd(byte *buffer, byte track, byte status = 0)
{
  byte i;

  // pre-compute ID records
  byte *ptr = buffer;
  for(i=0; i<NUM_SECTORS; i++)
    {
      *ptr++ = 0xFE;      // ID mark
      *ptr++ = track;     // cylinder number
      *ptr++ = 0;         // side number
      *ptr++ = i+1;       // sector number
      *ptr++ = 0;         // sector length (0=128 bytes)
      uint16_t crc = calc_crc(ptr-5, 5, false);
      *ptr++ = crc / 256; // CRC
      *ptr++ = crc & 255; // CRC
    }

  // status=0 only if called from monitor, must disable interrupts
  if( status==0 ) noInterrupts();

  // wait for start of index hole
  TCCR1A = 0;
  TCCR1C = 0;
  TCCR1B = bit(CS10) | bit(CS11);  // prescaler /64
  byte res = wait_index_hole(status);
  if( res!=S_OK ) { TCCR1B = 0; if( status==0 ) interrupts(); return res; }

  // initialize timer 2 (OC2A is the same pin as SPI-MOSI used for HD)
  TCCR2A = 0;

  // make sure OC2A is high before we enable WRITE_GATE
  TCCR2B = bit(CS20);                  // prescaler 1
  DDRB   &= ~0x08;                     // disable OC2A pin
  TCCR2A  = bit(COM2A1) | bit(COM2A0); // set OC2A on compare match
  TCCR2B |= bit(FOC2A);                // force compare match
  TCCR2A  = 0;                         // disable OC2A control by timer
  DDRB   |= 0x08;                      // enable OC2A pin

  // enable write gate (takes about 2 microseconds)
  drivectrl_set(PIN_SRO_WRITEGATE, LOW);

  // enable OC2A output pin control by timer (WRITE_DATA), initially high
  TCNT2  = 0;
  OCR2A  = 255;
  TIFR2  = bit(OCF2A);    // clear output compare flag
  TCCR2A = bit(COM2A0) | bit(WGM21);   // COM2A1:0 =  01 => toggle OC2A on compare match

  asm volatile
    (// initialize
     "          mov    r22, %0\n"
     "          ldi    r16, 31\n"        // r16 = 2us for short pulse
     "          ldi    r18, 63\n"        // r18 = 4us for long pulse

     // 1) ---------- 40x 0x00 (pre-index gap) = 40 * 8 = 320 long pulses = 240+80 (40*8*4 = 1280us)
     "          ldi    r20, 240\n"        
     "          call   wrtlong\n"
     "          ldi    r20, 80\n"
     "          call   wrtlong\n"

     // 2) ---------- 6x 0x00 = 6 * 8 = 48 long pulses (6*8*4 = 192us)
     "          ldi   r20, 48\n"
     "          call  wrtlong\n"

     // 3) ---------- index address mark (0xFC data bit pattern, 0xD7 clock bit pattern) (32us)
     // 
     // CLK : (1)  1   0   1   0   1   1   1  (1)
     // DATA:    1   1   1   1   1   1   0   0
     //          1 1 1 0 1 1 1 0 1 1 1 1 0 1 0 1
     //          S S S   L S S   L S S S   L   L
     // => SSSLSSLSSSLL
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"

     // 4) ---------- 26x 0x00 (post-index gap) = 26 * 8 = 208 short pulses (26*8*4 = 832us)
     "          ldi    r20, 208\n"        
     "          call   wrtlong\n"

     // 5) ---------- 6x 0x00 = 6 * 8 = 48 long pulses (6*8*4 = 192us)
     "secstart: ldi   r20, 48\n"
     "          call  wrtlong\n"

     // 6) ---------- ID mark (0xFE data bit pattern, 0xC7 clock bit pattern) (32us)
     // 
     // CLK : (1)  1   0   0   0   1   1   1  (1)
     // DATA:    1   1   1   1   1   1   1   0
     //          1 1 1 0 1 0 1 0 1 1 1 1 1 1 0 1
     //          S S S   L   L   L S S S S S   L
     // => SSSLLLSSSSSL
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"

     // 7) ---------- write ID record (6*8*4 = 192us)
     "          ld     r20, Z+\n"        // skip ID address mark (already written)
     "          ldi    r21, 0\n"         // (1)   initialize bit counter to fetch next byte
     "          ldi    r26, 6\n"         // (1)   initialize byte counter (6 bytes to write)
     "fi:       dec     r21\n"           // (1)   decrement bit counter
     "          brpl    fib\n"           // (1/2) skip the following if bit counter >=  0
     "          subi    r26, 1\n"        // (1)   subtract one from byte counter
     "          brmi    fidone\n"        // (1/2) done if byte counter <0
     "          ld	r20, Z+\n"       // (2)   get next byte
     "          ldi     r21, 7\n"        // (1)   reset bit counter (7 more bits after this first one)
     "fib:      rol     r20\n"           // (1)   get next data bit into carry
     "          brcc    fi0\n"           // (1/2) jump if bit is "0"
     "          WRTPS\n"                 //       write data bit
     "          WRTPS\n"                 //       write clock bit
     "          rjmp   fi\n"             // (2)   next bit
     "fi0:      WRTPL\n"                 //       write clock bit
     "          rjmp   fi\n"             // (2)   next bit
     "fidone:   "

     // 8) ---------- 11x 0x00 (post-ID gap) = 11 * 8 = 88 long pulses (11*8*4 = 352us)
     "          ldi    r20, 88\n"
     "          call   wrtlong\n"

     // 9) ---------- 6x 0x00 = 6 * 8 = 48 long pulses (6*8*4 = 192us)
     "          ldi   r20, 48\n"
     "          call  wrtlong\n"

     // 6) ---------- data mark (0xFB data bit pattern, 0xC7 clock bit pattern) (32us)
     // 
     // CLK : (1)  1   0   0   0   1   1   1  (1)
     // DATA:    1   1   1   1   1   0   1   1
     //          1 1 1 0 1 0 1 0 1 1 0 1 1 1 1 1
     //          S S S   L   L   L S   L S S S S
     // => SSSLLLSLSSSS
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"

     // 7) ---------- sector data (128 x 0xE5) (128*8*4 = 4096us)
     //         
     // CLK : (1)  1   1   1   1   1   1   1  (1)
     // DATA:    1   1   1   0   0   1   0   1
     //          1 1 1 1 1 1 0 1 0 1 1 1 0 1 1 1
     //          S S S S S S   L   L S S   L S S
     // => SSSSSSLLSSLSS
     "          ldi     r20, 128\n"
     "secdata:  WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          dec     r20\n"
     "          brne    secdata\n"

     // 8) ---------- sector data CRC (0x5D, 0x30) (64us)
     //          0x5D                            0x30
     // CLK : (1)  1   1   1   1   1   1   1   1   1   1   1   1   1   1   1  (1)
     // DATA:    0   1   0   1   1   1   0   1   0   0   1   1   0   0   0   0
     //          0 1 1 1 0 1 1 1 1 1 1 1 0 1 1 1 0 1 0 1 1 1 1 1 0 1 0 1 0 1 0 1
     //            L S S   L S S S S S S   L S S   L   L S S S S   L   L   L   L
     // => LSSLSSSSSSLSSLLSSSSLLLL
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPS\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPL\n"
     "          WRTPL\n"

     // if this was the last sector then the track is done
     "          dec    r22\n"             // (1)    decrement sector counter
     "          breq   ftdone\n"          // (1/2)  jump if done

     // 9) ---------- 27x 0x00 (post-data gap) = 27 * 8 = 216 long pulses (27*8*4=864us)
     "          ldi    r20, 216\n"        
     "          call   wrtlong\n"
     "          rjmp   secstart\n"

     // ---------- track format done => write 0xFF bytes until INDEX hole seen
     "ftdone:   WRTPS\n"
     "          lds    r20, 0x23\n"     // (2)   read INDEX signal
     "          sbrc   r20, 5\n"        // (1/2) skip next instruction if PINB5 (INDEX) is LOW
     "          rjmp   ftdone\n"        // (2)   write more gap bytes

     :                                    // no outputs
     : "r"(NUM_SECTORS), "z"(buffer)      // inputs  (z=r30/r31)
     : "r16", "r18", "r19", "r20", "r21", "r22", "r26", "r27"); // clobbers

  // COM2A1:0 = 00 => disconnect OC2A (will go high)
  TCCR2A = 0;

  // disable write gate
  drivectrl_set(PIN_SRO_WRITEGATE, HIGH);

  if( status==0 ) interrupts();

  return S_OK;
}


static bool format_track_dd(byte *buffer, byte track, byte status = 0)
{
  byte i;
  byte side       = (drivectrl & bit(PIN_SRO_SIDE1))==0 ? 1 : 0;
  byte crcdata[5] = {0xFE, track, side, 0x00, 0x01};
  byte *precomp   = track < 60 ? preComp000 : preComp125;

  byte *ptr = buffer;
  *ptr++ = 80;
  *ptr++ = 0x4E;      // pre-index gap
  *ptr++ = 12;
  *ptr++ = 0x00;      // 12x 00
  *ptr++ = 3 | 0x80;
  *ptr++ = 0xC2;      // index SYNC
  *ptr++ = 1;
  *ptr++ = 0xFC;      // index mark
  *ptr++ = 50;        
  *ptr++ = 0x4E;      // post-index gap

  for(i=0; i<NUM_SECTORS; i++)
    {
      *ptr++ = 12;
      *ptr++ = 0x00;  // 12x 00
      *ptr++ = 3 | 0x80;
      *ptr++ = 0xA1;  // SYNC
      *ptr++ = 1;
      *ptr++ = 0xFE;  // ID mark
      *ptr++ = 1;
      *ptr++ = track; // cylinder number
      *ptr++ = 1;
      *ptr++ = side;  // side number (0=lower, 1=upper)
      *ptr++ = 1;
      *ptr++ = i+1;   // sector number
      *ptr++ = 1;
      *ptr++ = 1;     // sector length (1=256 bytes)
      crcdata[3] = i+1;
      uint16_t crc = calc_crc(crcdata, 5, true);
      *ptr++ = 1;
      *ptr++ = crc / 256; // CRC
      *ptr++ = 1;
      *ptr++ = crc & 255; // CRC
      *ptr++ = 21;
      *ptr++ = 0x4E;  // post-id gap
      *ptr++ = 12;
      *ptr++ = 0x00;  // 12x 00
      *ptr++ = 3 | 0x80;
      *ptr++ = 0xA1;  // SYNC
      *ptr++ = 1;
      *ptr++ = 0xFB;  // data mark
      *ptr++ = 127;
      *ptr++ = 0xE5;  // data (256x E5)
      *ptr++ = 127;
      *ptr++ = 0xE5;
      *ptr++ = 2;
      *ptr++ = 0xE5;
      *ptr++ = 1;
      *ptr++ = 0x78;  // CRC
      *ptr++ = 1;
      *ptr++ = 0x27;  // CRC
      *ptr++ = 54;
      *ptr++ = 0x4E;  // post-data gap
    }

  // Track-end gap - make sure to have enough data to write for a 167ms track. 
  // The formatting code will keep writing data until it sees the INDEX hole.
  *ptr++ = 127;
  *ptr++ = 0x4E;
  *ptr++ = 127;
  *ptr++ = 0x4E;
  *ptr++ = 127;
  *ptr++ = 0x4E;
  *ptr++ = 127;
  *ptr++ = 0x4E;
  *ptr++ = 127;
  *ptr++ = 0x4E;
  *ptr++ = 127;
  *ptr++ = 0x4E;

  // status=0 only if called from monitor, must disable interrupts
  if( status==0 ) noInterrupts();

  // set up timer
  TCCR1A = 0;
  TCCR1C = 0;

  // wait for start of index hole
  TCCR1B = bit(CS10) | bit(CS11);  // prescaler /64
  byte res = wait_index_hole(status);
  if( res!=S_OK ) { TCCR1B = 0; if( status==0 ) interrupts(); return res; }

  // make sure WRITEDATA pin is high before we enable WRITEGATE
  PORTB |= 0x08;

  // enable write gate (takes about 2 microseconds)
  drivectrl_set(PIN_SRO_WRITEGATE, LOW);

  // Enabling SPI configures MISO (PB4) as input. We have an external 10k pull-down resistor
  // on PB4. Currently PORTB4 is HIGH which will make PB4 use an internal pull-up resistor
  // when configured as an input, causing it to float somewhere between 0-5V. 
  // Setting PORTB4 to LOW now will disable the internal pull-up resistor, and keep PB4 low. 
  // Note that the latch of the 74HC595 shift register to which PB4 is connected triggers 
  // on the LOW->HIGH edge which means that leaving it LOW does not do anything. PB4 will
  // be set HIGH during the drivectrl_set() call to disable write gate after writing the data.
  PORTB &= ~0x10;

  // Enable SPI in master mode with a clock rate of fck/2
  SPCR = (1<<SPE)|(1<<MSTR);
  SPSR = bit(SPI2X);

  // see comment in write_sector_data_dd()
  asm volatile
    (".macro FGETNEXTBYTE\n" // 12 cycles
     "          dec     r17         \n"         // (1)   decrement bit counter
     "          brmi    gnb\\@      \n"         // (1/2) get next byte if bit counter <  0
     "          cpi     r17, 3      \n"         // (1)   bit counter >= 3 ?
     "          brge    gw1\\@      \n"         // (1/2) skip the following if so
     "          cpi     r24, 129    \n"         // (1)   repeat counter == 129 (i.e. 1 + 0x80) ?
     "          brne    .+2         \n"         // (1/2) skip the following if so
     "          andi    r24, 0x7f   \n"         // (1)   clear bit 7 of repeat counter
     "          rjmp    gw2\\@      \n"         // (2)   
     "gw1\\@:   WAIT    4           \n"         // (4)   equalize timing for previous branches
     "gw2\\@:   WAIT    1           \n"         // (1)   equalize timing for previous branches
     "          rjmp    gnb3\\@     \n"         // (2)   done, don't need to get new byte
     "gnb\\@:   dec     r24         \n"         // (1)   decrement byte repeat counter
     "          brne    gnb4\\@     \n"         // (1/2) jump if more repeats
     "          ld      r24, Z+     \n"         // (2)   get next byte count
     "          ld      r16, Z+     \n"         // (2)   get next data byte
     "          rjmp    gnb2\\@     \n"         // (2)   reset bit counter and done
     "gnb4\\@:  ld      r16, -Z     \n"         // (2)   re-read previous data byte
     "          ld      r16, Z+     \n"         // (2)   increment Z again (also waste two cycles)
     "          WAIT    1           \n"         // (1)   equalize timing
     "gnb2\\@:  ldi     r17, 7      \n"         // (1)   reset bit counter (7 more bits after this first one)
     "gnb3\\@:                      \n"
     ".endm                         \n"
     
     ".macro FGETNEXTSEQ\n" // 10 cycles
     "          rol    r16          \n"        // (1) get next data bit into carry
     "          rol    r18          \n"        // (1) get next data bit from carry into shift register
     "          andi   r18, 0x0F    \n"        // (1) zero-out old bits in shift register
     "          movw   Y,   X       \n"        // (1) get precomp table address (must be 32-byte aligned)
     "          add    r28, r18     \n"        // (1) add 2x shift register content (r28=low-byte of Y)
     "          add    r28, r18     \n"        // (1) 
     "          ld     r20, Y+      \n"        // (2) get first  MFM sequence
     "          ld     r21, Y       \n"        // (2) get second MFM sequence
     ".endm                         \n"

     // initialize registers
     "          push    r28\n"
     "          push    r29\n"
     "          ldi     r17,  0     \n"        // (1)   initialize bit counter to fetch next byte
     "          ldi     r18,  0     \n"        // (1)   initialize shift register
     "          ldi     r24,  1     \n"        // (1)   initialize byte repeat counter
     "          ldi     r19,  0xFF  \n"        // (1)
     "          mov     r15,  r19   \n"        // (1)   used in "cpse" instruction below
     "          ld      r13,  X     \n"        // (2)   save precompTable[0] element (to restore later)
     "          sbi     0x1b, 0     \n"        // (2)   clear PCIF0 by setting it high
     "          rjmp    fNN         \n"        // (2)   start by getting first bit

     // pulse in second half (NP)
     "fNPc:     WAIT 1              \n"         // (1)
     "fNPb:     WAIT 5              \n"         // (5)
     "fNPa:                         \n"
     // copy of FGETNEXTBYTE with "out" instruction inserted after 9 cycles, 
     // also sets and resets precomp[0] for SYNC bytes (total 15 cycles: 9-OUT-5)
     "          dec     r17         \n"         // (1)   decrement bit counter
     "          brmi    gnb         \n"         // (1/2) get next byte if bit counter <  0
     "          mov     r19, r13    \n"         // (1)   get original precomp[0] into r19
     "          cpi     r24, 0x80   \n"         // (1)   bit 7 of byte repeat counter set (SYNC byte)?
     "          brlo    .+2         \n"         // (1/2) skip following instruction if not
     "          ldi     r19, 0xFF   \n"         // (1)   get 0xFF (no pulse, removed clock bit) into r19
     "          st      X, r19      \n"         // (2)   store in precomp[0] (0000)
     "          WAIT    1           \n"         // (1)
     "          out     0x2E, r21   \n"         // (1)   output pulse
     "          WAIT    3           \n"         // (3)
     "          rjmp    gnb3        \n"         // (2)
     "gnb:      dec     r24         \n"         // (1)   decrement byte repeat counter
     "          brne    gnb4        \n"         // (1/2) jump if more repeats
     "          ld      r24, Z+     \n"         // (2)   get next byte count
     "          ld      r16, Z+     \n"         // (2)   get next byte
     "          out     0x2E, r21   \n"         // (1)   output pulse
     "          WAIT    2           \n"         // (2)   
     "          rjmp    gnb2        \n"         // (2)   reset bit counter and done
     "gnb4:     ld      r16, -Z     \n"         // (2)   read previous data byte
     "          WAIT    1           \n"         // (1)   output pulse must happen after exactly 9 cycles
     "          out     0x2E, r21   \n"         // (1)   output pulse
     "          ld      r16, Z+     \n"         // (2)   increment Z again
     "          WAIT    2           \n"         // (2)
     "gnb2:     ldi     r17, 7      \n"         // (1)   reset bit counter (7 more bits after this first one)

     "gnb3:     FGETNEXTSEQ         \n"         // (10)
     "          WAIT    4           \n"         // (4)
     "          cpi     r21, 0xFF   \n"         // (1)   is next pulse NP?
     "          brne    fNPa        \n"         // (1/2) jump if so, otherwise NN since NP->PN is invalid

     // no pulse (NN) following pulse in second half (NP), 22 cycles since previous pulse
     "fNN:      FGETNEXTBYTE        \n"         // (12)
     "          FGETNEXTSEQ         \n"         // (10)
     "          WAIT 1              \n"         // (1)
     "          cpi  r20, 0xFF      \n"         // (1)   is next pulse PN?
     "          brne fPN            \n"         // (1/2) jump if so
     "          rjmp fNPc           \n"         // (2)   otherwise NP since NP->NN->NN is invalid

     // pulse in first half (PN)
     "fPN:      out     0x2E, r20   \n"         // (1)   output pulse
     "          FGETNEXTBYTE        \n"         // (12)
     "          FGETNEXTSEQ         \n"         // (10)
     "          sbis    0x1b, 0     \n"         // (1/2) skip next instruction if PCIF0 (INDEX pin change) is set
     "          rjmp    fPNW        \n"         // (2)   
     "          sbi     0x1b, 0     \n"         // (2)   clear PCIF0 by setting it high
     "          sbis    0x03, 5     \n"         // (1/2) skip next instruction if INDEX signal is HIGH
     "          rjmp    fdone       \n"         // (2)   end-of-track => done
     "fPN2:     cpi  r20, 0xFF      \n"         // (1)   is next pulse PN?
     "          brne fPN            \n"         // (1/2) jump if so 
     "          cpse r21, r15       \n"         // (1/2) is next pulse NN? skip next instruction if so
     "          rjmp fNPb           \n"         // (2)   next pulse is NP

     // no pulse (NN) following pulse in first half (PN), 33 cycles since previous pulse
     "          FGETNEXTBYTE        \n"         // (12)
     "          FGETNEXTSEQ         \n"         // (10)
     "          WAIT 8              \n"         // (7)
     "          rjmp fPN            \n"         // (2)   next must be PN since both PN->NN->NN and PN->NN->NP are invalid

     // time-wasting jump target if no INDEX pin change
     "fPNW:     WAIT    1           \n"         // (1)   equalize timing
     "          rjmp    fPN2        \n"         // (2)   continue

     "fdone:    pop r29\n"
     "          pop r28\n"

     : 
     : "x"(precomp), "z"(buffer)
     : "r13", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r24");

  // disable SPI
  SPCR = 0;

  // disable write gate
  drivectrl_set(PIN_SRO_WRITEGATE, HIGH);

  // status=0 only if called from monitor, must disable interrupts
  if( status==0 ) interrupts();

  return S_OK;
}


// -------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- monitor functions --------------------------------------------------
// -------------------------------------------------------------------------------------------------------------------------


static unsigned long motor_timeout[MAX_DRIVES];
static bool motor_auto[MAX_DRIVES];


inline void print_hex(byte b)
{
  if( b<16 ) Serial.write('0');
  Serial.print(b, HEX);
}


inline void print_header(char reject)
{
  static const char hex[17] = "0123456789ABCDEF";
  Serial.write('H');
  for(byte i=0; i<7; i++) { Serial.write(hex[header[i]/16]); Serial.write(hex[header[i]&15]); }
  Serial.write('-');
  Serial.write(reject);
  Serial.write(10);
}



void print_error(byte n)
{
  Serial.print(F("Error: "));
  switch( n )
    {
    case S_OK        : Serial.print(F("No error")); break;
    case S_NOTREADY  : Serial.print(F("Drive not ready")); break;
    case S_NOSYNC    : Serial.print(F("No sync marks found")); break;
    case S_NOHEADER  : Serial.print(F("Sector header not found")); break;
    case S_INVALIDID : Serial.print(F("Data record has unexpected id")); break;
    case S_CRC       : Serial.print(F("Data checksum error")); break;
    case S_NOTRACK0  : Serial.print(F("No track 0 signal detected")); break;
    case S_VERIFY    : Serial.print(F("Verify after write failed")); break;
    case S_READONLY  : Serial.print(F("Disk is write protected")); break;
    default          : { Serial.print(F("Unknonwn error: ")); Serial.print(n); break; }
    }
}


static void dump_buffer(byte *buf, int n)
{
  int offset = 0;
  while( offset<n )
    {
      print_hex(offset/256); 
      print_hex(offset&255); 
      Serial.write(':');

      for(int i=0; i<16; i++)
        {
          if( (i&7)==0  ) Serial.write(' ');
          print_hex(buf[offset+i]);
          Serial.write(' ');
        }

      Serial.write(' ');
      for(int i=0; i<16; i++)
        {
          if( (i&7)==0  ) Serial.write(' ');
          Serial.write(isprint(buf[offset+i]) ? buf[offset+i] : '.');
        }

      Serial.println();
      offset += 16;
    }
}


static void step_track(bool stepOut)
{
  if( !stepOut || SRI_BIT_SET(PIN_SRI_TRACK0) )
    drivectrl_step_pulse(stepOut);

  // wait 10ms (minimum time between steps is 3ms for
  // same direction, 10ms for direction change)
  delay(10);
}


static void step_tracks(int tracks)
{
  // if tracks<0 then step outward (towards track 0) otherwise step inward
  bool stepOut = tracks<0;
  tracks = abs(tracks);
  while( tracks-->0 ) step_track(stepOut);
  delay(90); 
}



static bool step_to_track0()
{
  byte n = 82;

  // step outward until TRACK0 line goes low
  while( --n > 0 && SRI_BIT_SET(PIN_SRI_TRACK0) ) 
    { drivectrl_step_pulse(true); delay(driveType[selDrive]==DT_SA800 ? 10 : 3); }

  if( n==0 )
    {
      // we have stpped for more than 80 tracks and are still not 
      // seeing the TRACK0 signal
      return false;
    }

  return true;
}


static byte step_to_track(byte track)
{
  if( step_to_track0() )
    {
      step_tracks(track);
      return S_OK;
    }
  else
    return S_NOTRACK0;
}


static byte wait_header(byte track, byte sector, bool DD, byte status = 0)
{
  byte attempts = 100;
  byte head = (drivectrl & bit(PIN_SRO_SIDE1))==0 ? 1 : 0;

  // check whether we can see any data pulses from the drive at all
  if( !check_pulse() )
    {
#ifdef DEBUG
      Serial.println(F("Drive not ready!")); Serial.flush();
#endif
      return S_NOTREADY;
    }

  while( --attempts>0 )
    {
      // wait for sync sequence and read 7 bytes of data
      byte res = DD ? read_sector_data_dd(header, 7, status) : read_sector_data_sd(header, 7, status);
      if( res!=S_OK )
        {
          // fatal read error
          return res;
        }
      else if( header[0]!=0xFE )
        {
          // not an ID record
#ifdef DEBUG
          print_header('R');
#endif
        }
      else if( calc_crc(header, 5, DD) != 256u*header[5]+header[6] )
        {
#ifdef DEBUG
          print_header('C');
#endif
        }
      else if( (track!=0xFF && track!=header[1]) || head!=header[2] )
        {
          // incorrect track or side => can not succeed
#ifdef DEBUG
          print_header('T');
#endif
          return S_NOHEADER;
        }
      else if( sector!=0xFF && sector!=header[3] )
        {
          // incorrect sector => try more
#ifdef DEBUG
          print_header('S');
#endif
        }
      else
        {
          // found header => no debug output since it would disrupt timing
          return S_OK;
        }
    }
  
#ifdef DEBUG
  if( attempts==0 )
    { Serial.println(F("Unable to find header!")); Serial.flush(); }
#endif

  return S_NOHEADER;
}


static byte seek_track(byte track, bool DD)
{
  // set up timer
  TCCR1A = 0;
  TCCR1B = bit(CS10); // prescaler 1
  TCCR1C = 0;

  noInterrupts();

  // wait for a sector header
  byte res = wait_header(0xFF, 0xFF, DD);

  // if we found a sector header, check if it's the correct track
  if( res==S_OK && header[1]!=track )
    {
      // make sure that the track number in the header we read is sensible
      if( header[1]<NUM_TRACKS )
        {
          // need interrupts for delay() when stepping
          interrupts();
          step_tracks(track-header[1]);
          noInterrupts();
          
          res = wait_header(track, 0xFF, DD);
        }
      else
        res = S_NOHEADER;
    }
  
  // if we couldn't find the header then step to correct track by going to track 0 and then stepping out and check again
  if( res!=S_OK )
    {
      // need interrupts for delay() when stepping
      interrupts();
      if( step_to_track0() )
        {
          step_tracks(track);
          noInterrupts();
          res = wait_header(track, 0xFF, DD);
        }
      else
        {
          noInterrupts();
          res = S_NOTRACK0;
        }
    }

  interrupts();

  return res;
}


byte get_current_track(byte *track, bool DD, byte status)
{
  // set up timer
  TCCR1A = 0;
  TCCR1B = bit(CS10); // prescaler 1
  TCCR1C = 0;

  byte res = wait_header(0xFF, 0xFF, DD, status);
  *track = header[1];

  return res;
}


byte write_sector(byte track, byte sector, bool DD, byte *buffer, byte status = 0)
{
  // set up timer
  TCCR1A = 0;
  TCCR1B = bit(CS10); // prescaler 1
  TCCR1C = 0;

  // status=0 only if called from monitor, must disable interrupts
  if( status==0 ) noInterrupts();

  // calculate CRC for the sector data
  int ssize = DD ? 256 : 128;
  uint16_t crc = calc_crc(buffer, ssize+1, DD);
  buffer[ssize+1] = crc/256;
  buffer[ssize+2] = crc&255;
  buffer[ssize+3] = 0x4E; // first byte of post-data gap (only used for DD)

  // find the requested sector
  byte res = wait_header(track, sector, DD, status);
  
  // if we found the sector then write the data
  if( res==S_OK ) 
    {
      if( DD ) 
        write_sector_data_dd(buffer, track, status);
      else
        write_sector_data_sd(buffer, status);
    }

  if( status==0 ) interrupts();

  TCCR1B = 0; // turn off timer 1

  return res;
}


byte read_sector(byte track, byte sector, bool DD, byte *buffer, byte status = 0)
{
  // set up timer
  TCCR1A = 0;
  TCCR1B = bit(CS10); // prescaler 1
  TCCR1C = 0;

  // status=0 only if called from monitor, must disable interrupts
  if( status==0 ) noInterrupts();

  // find the requested sector
  byte res = wait_header(track, sector, DD, status);
  
  // if we found the sector then read the data
  if( res==S_OK )
    {
      // wait for sync mark and read data
      res = DD ? read_sector_data_dd(buffer, 259, status) : read_sector_data_sd(buffer, 131, status);
      if( res==S_OK )
        {
          int ssize = DD ? 256 : 128;

          if( buffer[0]!=0xFB && buffer[0]!=0xF8 )
            { 
#ifdef DEBUG
              Serial.println(F("Unexpected record identifier")); 
              for(int i=0; i<7; i++) { Serial.print(buffer[i], HEX); Serial.write(' '); }
              Serial.println();
#endif
              res = S_INVALIDID;
            }
          else if( DD && calc_crc(buffer, ssize+1, DD) != 256u*buffer[ssize+1]+buffer[ssize+2] )
            { 
#ifdef DEBUG
              Serial.print(F("Data CRC error. Found: ")); Serial.print(256u*buffer[ssize+1]+buffer[ssize+2], HEX); Serial.print(", expected: "); Serial.println(calc_crc(buffer, ssize+1, DD), HEX);
#else
              res = S_CRC; 
#endif
            }
        }
    }

  if( status==0 ) interrupts();
  TCCR1B = 0; // turn off timer 1

  return res;
}


byte verify_sector(byte track, byte sector, bool DD, const byte *buffer)
{
  byte b[260];
  byte res = read_sector(track, sector, DD, b);
  if( res==S_OK && memcmp(buffer, b+1, DD ? 256 : 128)!=0 )
    res = S_VERIFY; 
  
  return res;
}


static String read_user_cmd()
{
  String s;
  do
    {
      int i = Serial.read();

      if( i==13 || i==10 ) 
        { Serial.println(); break; }
      else if( i==27 )
        { s = ""; Serial.println(); break; }
      else if( i==8 )
        { 
          if( s.length()>0 )
            { Serial.write(8); Serial.write(' '); Serial.write(8); s = s.substring(0, s.length()-1); }
        }
      else if( isprint(i) )
        { s = s + String((char )i); Serial.write(i); }

      if( motor_timeout[0]>0 && millis() > motor_timeout[0] ) { drivectrl_set(PIN_SRO_MOTOR0, HIGH); motor_timeout[0] = 0; }
      if( motor_timeout[1]>0 && millis() > motor_timeout[1] ) { drivectrl_set(PIN_SRO_MOTOR1, HIGH); motor_timeout[1] = 0; }
    }
  while(true);

  return s;
}


void clear_diskchange()
{
  if( !SRI_BIT_SET(PIN_SRI_DSKCHG) )
    {
      if( driveType[selDrive]==DT_SA800 )
        { 
          // "disk change" flag cleared by de-selecting drive
          drivectrl_set(PIN_SRO_SELECT, HIGH);
          drivectrl_set(PIN_SRO_SELECT, LOW);
        }
      else
        {
          // "disk change" flag cleared by moving head
          if( SRI_BIT_SET(PIN_SRI_TRACK0) )
            { step_tracks(-1); step_tracks(1); }
          else
            { step_tracks(1); step_tracks(-1); }
        }
    }
}


void motor(bool on)
{
  if( on )
    {
      drivectrl_set(PIN_SRO_MOTOR, LOW);

      // the motors of 8 inch drives are always on and we map
      // the MOTOR signal to HEADLOAD (which is not present on 5.25" drives)
      // HEADLOAD only requres a 35 millisecond delay before data can be read/written
      delay(driveType[selDrive]==DT_SA800 ? 35 : 600);

      if( !SRI_BIT_SET(PIN_SRI_DSKCHG) ) { clear_diskchange(); }
    }
  else
    {
      drivectrl_set(PIN_SRO_MOTOR, HIGH);
    }
}


bool check_motor()
{
  if( (drivectrl & bit(PIN_SRO_MOTOR)) )
    { motor(true); delay(300); return true; }

  return false;
}


void print_help()
{
  Serial.println();
  Serial.println(F("r [t,]s       Read track t (default current) sector s into buffer"));
  Serial.println(F("w [t,]s[,0/1] Write buffer content to track t (default current) sector s (1=and compare)"));
  Serial.println(F("b             Print buffer content"));
  Serial.println(F("B d           Fill buffer with decimal value d"));
  Serial.println(F("m [0/1]       Turn drive motor on or off or show current status"));
  Serial.println(F("i [n]         Step n (default 1) tracks in"));
  Serial.println(F("o [n]         Step n (default 1) tracks out"));
  Serial.println(F("0             Step to track 0"));
  Serial.println(F("d 0/1         Select drive 0 or 1"));
  Serial.println(F("I [0/1]       Print information about (current) drive"));
  Serial.println(F("T [n]         Print or set type of current drive"));
  Serial.println(F("x             Exit to controller mode"));
  Serial.println();
}


byte formatDisk(byte fromTrack, byte toTrack, bool DD)
{
  bool upperHead = (drivectrl & bit(PIN_SRO_SIDE1))==0;
  byte res = step_to_track(fromTrack);
  if( toTrack>=NUM_TRACKS ) toTrack = NUM_TRACKS-1;

  if( fromTrack==toTrack )
    { Serial.print(F("Formatting track ")); Serial.print(fromTrack); }
  else
    { Serial.print(F("Formatting tracks ")); Serial.print(fromTrack); Serial.print('-'); Serial.print(toTrack); }

  if( DD )
    {
      Serial.print(upperHead ? F(" side 2") : F(" side 1"));
      Serial.print(F(" in DOUBLE density "));
    }
  else
    Serial.print(F(" in SINGLE density "));
  Serial.flush();

  for(byte track=fromTrack; res==S_OK && track<=toTrack; track++)
    {
      res = DD ? format_track_dd(dataBuffer, track) : format_track_sd(dataBuffer, track);
      Serial.print('.');
      if( track+1<=toTrack ) step_tracks(1);
    }

  Serial.println();
  return res;
}



void print_rotation(unsigned long t)
{
  char c;
  Serial.print(t/1000); Serial.print('.'); 
  c = '0' + ((t/100) % 10); Serial.print(c);
  c = '0' + ((t/ 10) % 10); Serial.print(c);
  c = '0' + ((t/  1) % 10); Serial.print(c);
}


unsigned long measure_rotation()
{
  TCCR1A = 0;
  TCCR1B = bit(CS10) | bit(CS11);  // start timer 1 with /64 prescaler
  TCCR1C = 0;

  // return 0 if motor not running
  if( wait_index_hole()!=S_OK ) return 0;

  // build average cycle count (4us/cycle) over 4 revolutions
  unsigned long l = 0;
  for(byte i=0; i<4; i++)
    {
      if( wait_index_hole()!=S_OK ) return 0;
      l += TCNT1;
    }

  TCCR1B = 0; // turn off timer 1
  return l;
}



void monitor()
{
  char cmd;
  byte track;
  int a1, a2, a3, sector, n;
  bool DD = true;

  Serial.println(F("\r\n\nEntering disk monitor."));

  byte dip      = (~shift_in()) & 0x0F;

  // set drive types from DIP switches
  set_drive_type(0, (dip & 1)!=0 ? DT_SA800 : DT_5INCH_HD);
  set_drive_type(1, (dip & 2)!=0 ? DT_SA800 : DT_5INCH_HD);

  // no drive swapping in monitor
  pinSelect[0] = PIN_SRO_SELECT0; pinMotor[0] = PIN_SRO_MOTOR0;
  pinSelect[1] = PIN_SRO_SELECT1; pinMotor[1] = PIN_SRO_MOTOR1;
  if( (dip & 0x04)!=0 ) Serial.println(F("IGNORING DRIVE SWAP SETTING IN MONITOR"));
  
  selDrive = 1;
  drivectrl_set(PIN_SRO_SELECT, LOW);
  motor_timeout[0] = 0;
  motor_timeout[1] = 0;
  motor_auto[0] = true;
  motor_auto[1] = true;

  while( true )
    {
      Serial.print(F("\r\n\r\n["));
      Serial.print(selDrive);
      Serial.print(':');
      Serial.print(DD ? F("DD") : F("SD"));
      Serial.print(SRI_BIT_SET(PIN_SRI_WRTPROT) ? F("") : F("-WP"));
      Serial.print(SRI_BIT_SET(PIN_SRI_TRACK0)  ? F("") : F("-T0"));
      Serial.print(SRI_BIT_SET(PIN_SRI_DSKCHG)  ? F("") : F("-DC"));
      Serial.print(F("] Command: "));

      String s = read_user_cmd();

      n = sscanf(s.c_str(), "%c%i,%i,%i", &cmd, &a1, &a2, &a3);
      if( n<=0 || isspace(cmd) ) { print_help(); continue; }

      if( cmd=='r' && n>=2 )
        {
          if( n>=3 )
            { track=a1; sector=a2; }
          else
            { track=0xFF; sector=a1; }

          if( (track==0xFF || track<NUM_TRACKS) && sector>0 && sector<=NUM_SECTORS )
            {
              byte status = S_OK;
              check_motor();
              memset(dataBuffer, 0, 259);
              if( track!=0xFF )  status = seek_track(track, DD);
              if( status==S_OK ) status = read_sector(track, sector, DD, dataBuffer);
              if( status==S_OK ) 
                {
#ifdef DEBUG
                  dump_buffer(dataBuffer, DD ? 259 : 131);
#else
                  dump_buffer(dataBuffer+1, DD ? 256 : 128);
#endif
                }
              else
                print_error(status);
            }
        }
      else if( cmd=='w' && n>=2 )
        {
          if( n>=3 )
            { track=a1; sector=a2; }
          else
            { track=0xFF; sector=a1; }

          if( (track==0xFF || track<NUM_TRACKS) && sector>0 && sector<=NUM_SECTORS )
            {
              byte status = S_OK;
              check_motor();
              dataBuffer[0] = 0xFB;
              if( track!=0xFF  ) status = seek_track(track, DD);
              if( status==S_OK ) status = write_sector(track, sector, DD, dataBuffer);
              if( status==S_OK && n>3 && a3!=0 ) status = verify_sector(track, sector, DD, dataBuffer+1);
              
              if( status==S_OK ) 
                Serial.println(F("Ok."));
              else
                print_error(status);
            }
        }
      else if( cmd=='R' )
        {
          int fromTrack, toTrack;
          check_motor();

          if( n==1 )
            { fromTrack = 0; toTrack = NUM_TRACKS-1; }
          else if( n==2 )
            { fromTrack = a1; toTrack = a1; }
          else
            { fromTrack = a1; toTrack = a2; }
            
          for(track=fromTrack; track<=toTrack; track++)
            {
              if( seek_track(track, DD)!=S_OK ) break;
              sector = 1;
              for(byte i=0; i<NUM_SECTORS; i++)
                {
                  byte attempts = 0;
                  while( true )
                    {
                      Serial.print(F("Reading track ")); Serial.print(track); 
                      Serial.print(F(" sector ")); Serial.print(sector);
                      Serial.flush();
                      byte status = read_sector(track, sector, DD, dataBuffer);
                      if( status==S_OK )
                        {
                          Serial.println(F(" => ok"));
                          break;
                        }
                      else if( (status==S_INVALIDID || status==S_CRC) && (attempts++ < 10) )
                        Serial.println(F(" => CRC error, trying again"));
                      else
                        {
                          Serial.print(F(" => "));
                          print_error(status);
                          Serial.println();
                          break;
                        }
                    }
                  
                  sector+=2;
                  if( sector>NUM_SECTORS ) sector = 2;
                }
            }
        }
      else if( cmd=='W' )
        {
          int fromTrack, toTrack;

          if( n==1 )
            { fromTrack = 0; toTrack = NUM_TRACKS-1; }
          else if( n==2 )
            { fromTrack = a1; toTrack = a1; }
          else
            { fromTrack = a1; toTrack = a2; }
            
          bool verify = true; //n>1 && a2>0;
          char c;
          Serial.print(F("Write current buffer to all sectors in drive "));
          Serial.print(selDrive);
          Serial.println(F(". Continue (y/n)?"));
          while( (c=Serial.read())<0 );
          if( c=='y' )
            {
              check_motor();
              dataBuffer[0] = 0xFB;
              for(track=fromTrack; track<=toTrack; track++)
                {
                  sector = 1;
                  if( seek_track(track, DD)!=S_OK ) break;
                  for(byte i=0; i<NUM_SECTORS; i++)
                    {
                      Serial.print(F("Writing track ")); Serial.print(track);
                      Serial.print(F(" sector ")); Serial.print(sector);
                      Serial.flush();
                      byte status = write_sector(track, sector, DD, dataBuffer);
                      if( status==S_OK )
                        Serial.println(F(" => ok"));
                      else
                        {
                          Serial.print(F(" => "));
                          print_error(status);
                          Serial.println();
                        }
                      
                      sector+=2;
                      if( sector>NUM_SECTORS ) sector = 2;
                  }

                  if( verify )
                    {
                      sector = 1;
                      for(byte i=0; i<NUM_SECTORS; i++)
                        {
                          Serial.print(F("Verifying track ")); Serial.print(track);
                          Serial.print(F(" sector ")); Serial.print(sector);
                          Serial.flush();
                          byte status = verify_sector(track, sector, DD, dataBuffer+1);
                          if( status==S_OK )
                            Serial.println(F(" => ok"));
                          else
                            {
                              Serial.print(F(" => "));
                              print_error(status);
                              Serial.println();
                            }
                          
                          sector+=2;
                          if( sector>NUM_SECTORS ) sector = 2;
                        }
                    }
                }
            }
        }
      else if( cmd=='b' )
        {
          Serial.println(F("Buffer contents:"));
#ifdef DEBUG
          dump_buffer(dataBuffer, DD ? 259 : 131);
#else
          dump_buffer(dataBuffer+1, DD ? 256 : 128);
#endif
        }
      else if( cmd=='B' )
        {
          int nb = DD ? 256 : 128;
          Serial.print(F("Filling buffer"));
          if( n==1 )
            {
              for(int i=0; i<nb; i++) dataBuffer[i+1] = i;
            }
          else
            {
              Serial.print(F(" with 0x"));
              Serial.print(a1, HEX);
              for(int i=0; i<nb; i++) dataBuffer[i+1] = a1;
            }
          Serial.println();
        }
      else if( cmd=='m' )
        {
          if( n==1 )
            {
              if( drivectrl & bit(PIN_SRO_MOTOR) )
                Serial.print(F("Motor is NOT running"));
              else
                Serial.print(F("Motor is running"));

              if( motor_auto[selDrive] ) Serial.print(F(" (auto-off mode)"));
              Serial.println();
            }
          else if( (drivectrl & bit(PIN_SRO_MOTOR))!=0 && a1!=0 )
            {
              Serial.println(F("Turning motor ON."));
              motor(true);
              motor_auto[selDrive] = false;
            }
          else if( (drivectrl & bit(PIN_SRO_MOTOR))==0 && a1==0 )
            {
              Serial.println(F("Turning motor OFF."));
              motor(false);
              motor_auto[selDrive] = false;
            }
          else if( a1==2 )
            {
              Serial.println(F("Setting motor to AUTO mode."));
              motor_auto[selDrive] = true;
            }
        }
      else if( cmd=='M' )
        {
          while( Serial.available() ) Serial.read();
          Serial.println(F("Measuring spindle rotation, press any key to stop..."));
          while( !Serial.available() )
            {
              Serial.print(F("Rotation period: "));
              print_rotation(measure_rotation());
              Serial.println(F("ms"));
            }
          Serial.read();
        }
      else if( cmd=='i' )
        {
          int i = n>=2 ? abs(a1) : 1;
          step_tracks(i);
        }
      else if( cmd=='o' )
        {
          int i = n>=2 ? abs(a1) : 1;
          step_tracks(-i);
        }
      else if( cmd=='0' )
        {
          if( !step_to_track0() )
            { print_error(S_NOTRACK0); Serial.println(); }
        }
      else if( cmd=='h' )
        print_help();
      else if( cmd=='d' && n>=2 && a1<MAX_DRIVES )
        {
          Serial.print(F("Selecting drive ")); Serial.println(a1);
          motor(false);
          drivectrl_set(PIN_SRO_SELECT, HIGH);
          selDrive = a1;
          drivectrl_set(PIN_SRO_SELECT, LOW);
        }
      else if( cmd=='D' )
        {
          if( n>=2 )
            { Serial.print(F("Setting density to ")); DD = (a1!=0); }
          else
            { Serial.print(F("Density is ")); }

          Serial.println(DD ? F("HIGH") : F("LOW"));
        }
      else if( cmd=='s' )
        {
          if( n>1 )
            {
              drivectrl_set(PIN_SRO_SIDE1, a1!=2);
              Serial.print(F("Selecting side "));
              Serial.println((drivectrl & bit(PIN_SRO_SIDE1)) ? '1' : '2');
            }
          else
            {
              Serial.print(F("Side "));
              Serial.print((drivectrl & bit(PIN_SRO_SIDE1)) ? '1' : '2');
              Serial.println(F(" is selected."));
            }
        }
      else if( cmd=='I' )
        {
          if( !SRI_BIT_SET(PIN_SRI_DSKCHG) )
            {
              bool motorOn = (drivectrl & bit(PIN_SRO_MOTOR))==0;
              motor(motorOn);
            }

          Serial.println();
          if( n==1 ) { Serial.print(F("Current Drive     : ")); Serial.println(selDrive); }
          Serial.print(F("Current Track     : ")); 
          if( get_current_track(&track, DD, 0)==S_OK ) Serial.println(track); else Serial.println(F("unknown"));
          Serial.println();
        }
      else if( cmd=='T' )
        {
          if( n==1 )
            print_drive_type_drive(selDrive);
          else
            set_drive_type(selDrive, a1);

          Serial.println();
        }
      else if( cmd=='f' )
        {
          int fromTrack, toTrack;

          if( n==1 )
            { fromTrack = 0; toTrack = NUM_TRACKS-1; }
          else if( n==2 )
            { fromTrack = a1; toTrack = a1; }
          else
            { fromTrack = a1; toTrack = a2; }
            
          check_motor();
          byte status = formatDisk(fromTrack, toTrack, n > 3 ? a3!=0 : DD);
          if( status!=S_OK ) print_error(status);
        }
      else if( cmd=='x' )
        break;

      if( motor_auto[selDrive] && (drivectrl & bit(PIN_SRO_MOTOR))==0 )
        motor_timeout[selDrive] = millis() + (driveType[selDrive]==DT_SA800 ? 100 : 5000);
    }
  
  drivectrl_set(PIN_SRO_SELECT, HIGH);
  drivectrl_set(PIN_SRO_MOTOR,  HIGH);
}


// ----------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- controller functions --------------------------------------------------
// ----------------------------------------------------------------------------------------------------------------------------

#define CMD_READ                0x03
#define CMD_WRITE               0x05
#define CMD_READ_CRC            0x07
#define CMD_SEEK                0x09
#define CMD_CLEAR_ERROR_FLAGS   0x0B
#define CMD_HOME                0x0D
#define CMD_WRITE_DDAM          0x0F
#define CMD_LOAD_TRACK_ADDRESS  0x11
#define CMD_LOAD_CONFIGURATION  0x15
#define CMD_LOAD_UNIT_SECTOR    0x21
#define CMD_LOAD_WRITE_BUFFER   0x31
#define CMD_SHIFT_READ_BUFFER   0x41
#define CMD_CLEAR               0x81

#define CS_BUSY                 0x01
#define CS_UNIT0                0x02
#define CS_UNIT1                0x04
#define CS_MEDIA_OR_CRC_ERROR   0x08
#define CS_WRITEPROTECT         0x10
#define CS_NOTREADY             0x20
#define CS_SINGLE_SIDED         0x40
#define CS_FOUND_DDAM           0x80

#define CFG_DOUBLE_DENSITY      0x10
#define CFG_FORMAT_MODE         0x20

#define US_SECTOR               0x1F
#define US_HEAD                 0x20
#define US_UNIT                 0xC0


byte regCmd = 0, regSeekTrack = 0, regSector = 1, regStatus = 0x00, regReadBufPtr = 0, regWriteBufPtr = 0, regDataOut = 0, regConfig = 0, regCurrentTrack;
byte curCmd, curCmdSubStep;
unsigned int curCmdNextStepTime = 0, motorTimeout[2];
bool is3812 = false, readBufferEnabled = false, headReady[2] = {false, false}, driveReady[2] = {false, false};
byte driveReadyTimer = 180;

#define HEAD_REST_TIME           22   // milliseconds
#define HEAD_LOAD_TIME           37   // milliseconds
#define MOTOR_ON_TIME           600   // milliseconds

#define MOTOR_OFF_DELAY        6400   // milliseconds
#define HEAD_UNLOAD_DELAY      2000   // milliseconds
#define MOTOR_RUN_TIME         (driveType[selDrive]==DT_SA800 ? HEAD_UNLOAD_DELAY : MOTOR_OFF_DELAY)
#define HEAD_READY_TIME        (driveType[selDrive]==DT_SA800 ? HEAD_LOAD_TIME : MOTOR_ON_TIME)
#define HEAD_SETTLE_TIME         20

#define readBuffer     dataBuffer
#define writeBuffer   (dataBuffer+1*260)
#define tempBuffer    (dataBuffer+2*260)

// ----------------------------------------------------- handle commands ------------------------------------------------------


static void clear_registers()
{
  regReadBufPtr = 0;
  regWriteBufPtr = 0;
  readBufferEnabled = false;
  regStatus &= ~(CS_MEDIA_OR_CRC_ERROR|CS_BUSY|CS_FOUND_DDAM);
}


static bool check_writeprotect()
{
  if( SRI_BIT_SET(PIN_SRI_WRTPROT) )
    regStatus &= ~CS_WRITEPROTECT;
  else
    regStatus |=  CS_WRITEPROTECT;

  // return true if write protected
  return (regStatus & CS_WRITEPROTECT)!=0;
}


void select_drive(byte drive)
{
  if( drive != selDrive )
    {
      if( selDrive<2 )
        {
          // remember "drive ready" status for current drive
          driveReady[selDrive] = (regStatus & CS_NOTREADY)==0;
          drivectrl_set(PIN_SRO_SELECT, HIGH);
        }

      selDrive = drive;

      if( selDrive<2 )
        {
          drivectrl_set(PIN_SRO_SELECT, LOW);
          check_writeprotect();
                        
          // recall "drive ready" status for new drive
          if( driveReady[selDrive] )
            regStatus &= ~CS_NOTREADY; 
          else 
            regStatus |= CS_NOTREADY;
                        
          // if there was a disk change then assume drive is not ready
          if( !SRI_BIT_SET(PIN_SRI_DSKCHG) ) 
            { regStatus |= CS_NOTREADY; clear_diskchange(); }
                        
          // check whether we have a disk now
          if( (regStatus & CS_NOTREADY) )
            {
              unsigned int wait_time = 166;
                            
              // if motor of 5.25" drive is not running then turn it on now
              if( driveType[selDrive]==DT_5INCH_HD && (drivectrl & bit(PIN_SRO_MOTOR)) )
                { 
                  motorTimeout[selDrive]=MOTOR_RUN_TIME; 
                  drivectrl_set(PIN_SRO_MOTOR, LOW); 
                  wait_time += 500;
                }
                            
              // if we have a disk then the drive is ready
              if( have_disk(wait_time) )
                regStatus &= ~CS_NOTREADY;
                            
              // if 5.25" disk is not ready then keep motor running
              // to detect INDEX signal once a disk is inserted
              if( driveType[selDrive]==DT_5INCH_HD )
                motorTimeout[selDrive]=0;
            }

          // reset INDEX pin change flag
          PCIFR |= bit(PCIF0); 
                        
          // reset timer to check for "drive ready"
          driveReadyTimer = 180;

          if( driveType[selDrive]==DT_SA800 && is3812 )
            regStatus |= CS_SINGLE_SIDED;
          else
            regStatus &= ~CS_SINGLE_SIDED;

        }
      else
        {
          // can only support two drives
          regStatus |= CS_NOTREADY;
        }
    }

  regStatus = (regStatus & ~0x06) | (drive << 1);
}
        


static void handle_command()
{
  byte stat = S_OK;

  // we only get here once the head is ready
  headReady[selDrive] = true;

  switch( curCmd )
    {
    case CMD_SEEK:
      {
        if( curCmdSubStep==0 )
          {
            // read track number from disk if not in FORMAT mode and not already at track 0
            if( !SRI_BIT_SET(PIN_SRI_TRACK0) )
              regCurrentTrack = 0;
            else if( (regConfig & CFG_FORMAT_MODE)==0 )
              stat = get_current_track(&regCurrentTrack, (regConfig & CFG_DOUBLE_DENSITY)!=0, regStatus);

            if( stat==S_OK )
              {
                if( regCurrentTrack == regSeekTrack )
                  regStatus &= ~CS_BUSY; // at correct track => command done
                else
                  curCmdSubStep = 1; // start stepping
              }
            else
              {
                // error encountered => command done
                regStatus &= ~CS_BUSY;
              }
          }
        else if( curCmdSubStep==1 )
          {
            // currently stepping
            if( regCurrentTrack==regSeekTrack )
              { 
                // wait HEAD_SETTLE_TIME after reaching desired track,
                // then confirm current track
                curCmdSubStep = 0; 
                curCmdNextStepTime = HEAD_SETTLE_TIME+1; 
              }
            else
              {
                if( regCurrentTrack < regSeekTrack )
                  { drivectrl_step_pulse(false); regCurrentTrack++; }
                else if( SRI_BIT_SET(PIN_SRI_TRACK0) )
                  { drivectrl_step_pulse(true); regCurrentTrack--; }

                curCmdNextStepTime = (driveType[selDrive]==DT_SA800 ? 11 : 4);
              }
          }
        
        break;
      }

    case CMD_HOME:
      {
        if( curCmdSubStep==0 )
          {
            if( !SRI_BIT_SET(PIN_SRI_TRACK0) )
              {
                // wait HEAD_SETTLE_TIME after reaching track 0
                regCurrentTrack = 0;
                curCmdSubStep = 1;
                curCmdNextStepTime = HEAD_SETTLE_TIME+1;
                
              }
            else
              {
                // step out
                drivectrl_step_pulse(true);
                curCmdNextStepTime = (driveType[selDrive]==DT_SA800 ? 11 : 4);
              }
          }
        else
          {
            // command done
            regStatus &= ~CS_BUSY;
          }

        break;
      }

    case CMD_READ:
    case CMD_READ_CRC:
      {
        byte *buffer = (curCmd==CMD_READ) ? readBuffer : tempBuffer;
        stat = read_sector(regCurrentTrack, regSector, (regConfig & CFG_DOUBLE_DENSITY)!=0, buffer, regStatus);
        if( buffer[0]==0xF8 ) regStatus |= CS_FOUND_DDAM;
        regReadBufPtr = 0;
        regStatus &= ~CS_BUSY;
        break;
      }

    case CMD_WRITE:
    case CMD_WRITE_DDAM:
      {
        if( check_writeprotect() )
          {
            // write protected => do nothing
          }
        else if( (regConfig & CFG_FORMAT_MODE)==0 )
          {
            // regular (write) operation
            writeBuffer[0] = (curCmd==CMD_WRITE) ? 0xFB : 0xF8;
            stat = write_sector(regCurrentTrack, regSector, (regConfig & CFG_DOUBLE_DENSITY)!=0, writeBuffer, regStatus);
            regWriteBufPtr = 0;
          }
        else
          {
            // format mode => format track
            if( (regConfig & CFG_DOUBLE_DENSITY)!=0 )
              stat = format_track_dd(dataBuffer, regCurrentTrack, regStatus);
            else
              stat = format_track_sd(tempBuffer, regCurrentTrack, regStatus);

            if( stat==S_NOINDEX ) stat = S_NOTREADY;
          }
        
        // command done
        regStatus &= ~CS_BUSY;
        break;
      }

    default:
      regStatus &= ~CS_BUSY;
      break;
    }

  if( stat==S_ABORT )
    clear_registers();
  else if( stat==S_NOTREADY )
    regStatus |= CS_NOTREADY;
  else if( stat!=S_OK && (regStatus & CS_NOTREADY)==0 )
    regStatus |= CS_MEDIA_OR_CRC_ERROR;

  // if we saw a "clear strobe" command (bit0 = 0) while reading/writing the disk then clear 
  // the strobe bit in the command register
  if( clrStb!=0 ) { regCmd &= ~0x01;  clrStb = 0; }

  // keep motor running / head loaded
  motorTimeout[selDrive] = MOTOR_RUN_TIME;
}


// -------------------------------------------- handle bus activity (INP or OUT) ----------------------------------------------


void write_command_register(byte data)
{
  byte prevCmd = regCmd;
  regCmd = data;

  // only execute a command if strobe (bit 0 of command) is going LOW->HIGH
  if( (prevCmd&1)==0 && (regCmd&1)!=0 )
    {
      switch( regCmd )
        {
        case CMD_CLEAR_ERROR_FLAGS:
          {
            regStatus &= ~(CS_MEDIA_OR_CRC_ERROR|CS_FOUND_DDAM);
            break;
          }

        case CMD_CLEAR:
          {
            clear_registers();
            break;
          }
              
        case CMD_LOAD_TRACK_ADDRESS:
          {
            regSeekTrack = regDataOut;
            break;
          }

        case CMD_LOAD_CONFIGURATION:
          {
            // only 3812 supports LOAD_CONFIGURATION command
            if( is3812 ) regConfig = regDataOut;
            break;
          }

        case CMD_LOAD_UNIT_SECTOR:
          {
            // bit 6+7: unit
            select_drive((regDataOut & US_UNIT) >> 6);
            // bit 5: head (3812 only)
            if( is3812 ) select_head((regDataOut & US_HEAD)!=0);
            // bits 0-4: sector
            regSector = regDataOut & US_SECTOR;
            break;
          }

        case CMD_SHIFT_READ_BUFFER:
          {
            regReadBufPtr++;
            if( !is3812 ) regReadBufPtr &= 0x7F;
            break;
          }

        case CMD_LOAD_WRITE_BUFFER:
          {
            writeBuffer[regWriteBufPtr+1] = regDataOut;
            regWriteBufPtr++;
            if( !is3812 ) regWriteBufPtr &= 0x7F;
            break;
          }

        case CMD_SEEK:
        case CMD_HOME:
        case CMD_READ:
        case CMD_READ_CRC:
        case CMD_WRITE:
        case CMD_WRITE_DDAM:
          {
            // do not execute command if busy or CRC ERROR flag is set
            if( (regStatus & (CS_BUSY|CS_MEDIA_OR_CRC_ERROR))==0 && selDrive<2 )
              {
                curCmd = regCmd;
                curCmdSubStep = 0; 
                curCmdNextStepTime = 0; 
                regStatus |= CS_BUSY; 

                // if head not loaded then load before executing command
                if( !headReady[selDrive] )
                  {
                    drivectrl_set(PIN_SRO_MOTOR, LOW);
                    curCmdNextStepTime = HEAD_READY_TIME;
                  }

                // keep motor running / head loaded
                motorTimeout[selDrive] = MOTOR_RUN_TIME;
              }
            else if( is3812 )
              {
                // can't execute command => just be busy for a while to satisfy 3812diag
                curCmd = 0xFF;
                curCmdNextStepTime = 20;
                regStatus |= CS_BUSY; 
              }

            break;
          }
        }
    }

  // 3812 automatically resets the strobe bit to 0
  if( is3812 ) regCmd &= ~0x01;

  // send either status register or read buffer to data output
  readBufferEnabled = (regCmd & 0x40)!=0;
}


void write_register(byte reg, byte data)
{
  if( reg==0 )
    write_command_register(data);
  else if( reg==1 )
    {
      regDataOut = data;

      // 3812 automatically sets (and clears) strobe bit when writing data register
      // => if the previous command was LOAD_WRITE_BUFFER then immediately
      // (re-)execute the command
      if( is3812 && (regCmd|0x01)==CMD_LOAD_WRITE_BUFFER )
        {
          writeBuffer[regWriteBufPtr+1] = regDataOut;
          regWriteBufPtr++;
          if( !is3812 ) regWriteBufPtr &= 0x7F;
        }
    }
}


byte read_register(byte reg)
{
  byte res = 0xFF;

  if( reg==0 )
    {
      if( readBufferEnabled )
        {
          res = readBuffer[regReadBufPtr+1];
          // 3812 automatically shifts the read buffer after reading reading it
          if( is3812 ) regReadBufPtr++;
        }
      else
        res = regStatus;
    }

  return res;
}


void handle_bus_communication()
{
  // this must be either an INP (PC4 high) or an OUT (PC5 high) request, 
  // never both together (CPU can only do one at a time)
  // we always wait for the PC4/PC5 signals to go low again and then
  // reset the notification flag, so at this point they must have gone high
  if( PINC & 0x10 )
    {
      // bus input (controller -> CPU)
      byte reg = PINC & 0x03;     // get register address
      PORTD = read_register(reg); // read register and write value to PORTD
      DDRD = 0xFF;                // switch data bus pins to output
      PORTB &= ~0x02;             // set WAIT signal to LOW
      while( (PINC & 0x10) );     // wait until INP signal ends
      DDRD = 0x00;                // switch data bus pins back to input
      PORTB |= 0x02;              // set WAIT signal back to HIGH
      PCIFR |= bit(PCIF1);        // reset INP/OUT pin change flag
    }
  else if( PINC & 0x20 )
    {
      // bus output (CPU -> controller)
      byte reg = PINC & 0x03;     // get register address
      byte data = PIND;           // get input data
      PORTB &= ~0x02;             // set WAIT signal to LOW
      while( (PINC & 0x20) );     // wait until OUT signal ends
      PORTB |= 0x02;              // set WAIT signal back to HIGH
      PCIFR |= bit(PCIF1);        // reset INP/OUT pin change flag
      write_register(reg, data);  // write register
    }
}


// -----------------------------------------------  handle low-resolution timer  ------------------------------------------------


void handle_lowres_timer()
{
  static byte writeProtCheckTime = 1;

  if( selDrive<2 )
    {
      if( --writeProtCheckTime==0 )
        { check_writeprotect(); writeProtCheckTime = 250; }
      
      // check whether drive is ready
      if( driveType[selDrive]==DT_5INCH_HD )
        {
          // 5.25" drive motor is generally off => use DISKCHANGE signal to check whether
          // disk was removed, use INDEX signal to confirm that new disk was inserted
          if( --driveReadyTimer==0 )
            {
              driveReadyTimer = 50;
              if( (regStatus & CS_NOTREADY)!=0 )
                { if( PCIFR & bit(PCIF0) ) { PCIFR |= bit(PCIF0); regStatus &= ~CS_NOTREADY; motorTimeout[selDrive] = MOTOR_RUN_TIME; } }
              else 
                { if ( !SRI_BIT_SET(PIN_SRI_DSKCHG) ) { regStatus |= CS_NOTREADY; clear_diskchange(); motorTimeout[selDrive]=0; drivectrl_set(PIN_SRO_MOTOR, LOW); PCIFR |= bit(PCIF0); } }
            }
        }
      else
        {
          // 8" drive always spins => go by INDEX signal
          if( PCIFR & bit(PCIF0) )
            { driveReadyTimer = 180; PCIFR |= bit(PCIF0); regStatus &= ~CS_NOTREADY; }
          else if( driveReadyTimer>0 && --driveReadyTimer==0 )
            regStatus |= CS_NOTREADY;
        }
    }

  // decrement command ready timer
  if( curCmdNextStepTime>0 )
    curCmdNextStepTime--;
  
  // motor turn-off timeout drive 0
  if( motorTimeout[0]>0 && --motorTimeout[0]==0 )
    { 
      drivectrl_set(pinMotor[0], HIGH); 
      headReady[0] = false;
    }
  
  // motor turn-off timeout drive 1
  if( motorTimeout[1]>0 && --motorTimeout[1]==0 ) 
    { 
      drivectrl_set(pinMotor[1], HIGH);
      headReady[1] = false;
    }
  
  // reset timer compare match flag
  TIFR0 |= bit(OCF0A);
}


// -----------------------------------------------------  controller loop ------------------------------------------------------


void controller()
{
  noInterrupts();

  // WAIT was initially set to LOW to release a potentially waiting CPU
  // => set it to HIGH now for proper operation
  digitalWrite(PIN_WAIT, HIGH);

  // determine drive parameters
  byte dip        = (~shift_in()) & 0x0F;
  bool swapDrives = (dip & 0x04)!=0;

  // set controller type
  is3812 = (dip & 0x08)!=0;

  // swap drives if requested
  byte d0 = 0, d1 = 1;
  if( swapDrives ) { d0 = 1; d1 = 0; }

  // set drive types
  set_drive_type(d0, (dip & 1)!=0 ? DT_SA800 : DT_5INCH_HD);
  set_drive_type(d1, (dip & 2)!=0 ? DT_SA800 : DT_5INCH_HD);
  pinSelect[d0] = PIN_SRO_SELECT0; pinMotor[d0] = PIN_SRO_MOTOR0;
  pinSelect[d1] = PIN_SRO_SELECT1; pinMotor[d1] = PIN_SRO_MOTOR1;

  // initialize registers
  regCmd = 0;
  regConfig = 0;
  regStatus = CS_NOTREADY;

  // no drive selected initially
  selDrive = 0xFF;
  headReady[0] = false;
  headReady[1] = false;

  DDRD = 0x00; // make sure data bus pins are set to input
  PCIFR = bit(PCIF0) | bit(PCIF1); // reset any pin-change flags

  readBufferEnabled = false;

  // set timer 0 for a 1000Hz frequency (1 millisecond resolution)
  TCNT0  = 0; // reset timer 0
  TIMSK0 = 0; // disable interrupts for timer 0
  TCCR0A = bit(WGM01); // clear-timer-on-compare-match mode
  TCCR0B = bit(CS00) | bit(CS01); // /64 prescaler (16MHz clock => 250000 timer ticks per second)
  OCR0A  = 249; // output compare match after 250 ticks
  TIFR0 |= bit(OCF0A); // clear timer compare match flag

  // main controller loop
  while( true )
    {
      // check for bus activity pin change (INP or OUT)
      if( PCIFR & bit(PCIF1) )
        handle_bus_communication();

      // if a command is executing and ready for next step then do that now
      if( (regStatus & CS_BUSY)!=0 && curCmdNextStepTime==0 )
        handle_command();

      // check for low-resolution timer overflow (timer 0, overflows after 1ms)
      if( TIFR0 & bit(OCF0A) )
        handle_lowres_timer();
    }
}


// ----------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- main functions -----------------------------------------------------
// ----------------------------------------------------------------------------------------------------------------------------


void setup() 
{
  TCCR1A = 0;  
  TCCR1B = 0;
  TCCR1C = 0;  
  TCCR2A = 0;
  TCCR2B = 0;

  // set up pin modes 
  digitalWrite(PIN_SRDATA,    HIGH);
  digitalWrite(PIN_SRCLOCK,   HIGH);
  digitalWrite(PIN_SROLATCH,  LOW);
  digitalWrite(PIN_SRILATCH,  LOW);
  digitalWrite(PIN_WRITEDATA, HIGH);
  digitalWrite(PIN_WAIT,      LOW);
  pinMode(PIN_SRDATA,    OUTPUT);
  pinMode(PIN_SRCLOCK,   OUTPUT);
  pinMode(PIN_SROLATCH,  OUTPUT);
  pinMode(PIN_WRITEDATA, OUTPUT);
  pinMode(PIN_INDEX,     INPUT);
  pinMode(PIN_SRILATCH,  OUTPUT);
  pinMode(PIN_READDATA,  INPUT);
  pinMode(PIN_A0,        INPUT);
  pinMode(PIN_A1,        INPUT);
  pinMode(PIN_WAIT,      OUTPUT);
  pinMode(PIN_INP,       INPUT);
  pinMode(PIN_OUT,       INPUT);
  DDRD = 0x00;  // set digital pins 0-7 (PORTD) to input

  // set all shift register output pins to HIGH (i.e. disabled)
  drivectrl = 0xFF;
  shift_out(drivectrl);

  // set up pin change notifications for OUT, INP and INDEX signals
  PCMSK1 |= bit(PCINT12);    // INP
  PCMSK1 |= bit(PCINT13);    // OUT
  PCMSK0 |= bit(PCINT5);     // INDEX
  PCIFR = bit(PCIF0) | bit(PCIF1);
}
 

void loop() 
{
#if MONITOR>0
  Serial.begin(115200);

#if MONITOR==1
  monitor();
#else
  if( !SRI_BIT_SET(PIN_SRI_MONITOR) ) monitor();
#endif
  
  // the data bus pins include D0 and D1 which are used for serial
  // communication => must disable Serial for controller operation
  Serial.println();
  Serial.flush();
  Serial.end();
#endif
 
  // enter controller (infinite loop)
  controller();
}
