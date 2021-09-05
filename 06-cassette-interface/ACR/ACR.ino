// -----------------------------------------------------------------------------
// 88-ACR Altair Audio Cassette Interface (300 baud) for Altair Simulator I/O bus
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
  Pin  ATMega   Register   Direction   Function
  D0      2     PD0        in/out      data bus bit 0
  D1      3     PD1        in/out      data bus bit 1
  D2      4     PD2        in/out      data bus bit 2
  D3      5     PD3        in/out      data bus bit 3
  D4      6     PD4        in/out      data bus bit 4
  D5     11     PD5        in/out      data bus bit 5
  D6     12     PD6        in/out      data bus bit 6
  D7     13     PD7        in/out      data bus bit 7
  D8     14     PB0        in          audio signal in (ICP1 for timer 1)
  D9     15     PB1        in          bus input request (INP)
  D10    16     PB2        out         "good data" LED
  D11    17     PB3        out         audio signal out (OC2A for timer 2)
  D12    18     PB4        in/out      unused
  D13    19     PB5        in/out      unused
  A0     23     PC0        in          address bus bit 0
  A1     24     PC1        out         WAIT signal
  A2     25     PC2        in          bus output request (OUT)
  A3     26     PC3        out         serial data for audio (debug)
  A4     27     PC4        out         decoded serial data from audio (debug)
  A5     28     PC5        in/out      unused

  // Board     : Arduino Pro or Pro Mini,
  // Processor : ATMega328P (3.3V, 8MHz)
  //  
  // To restore boot loader, compile with Arduino then upload
  // C:\Users\..\AppData\Local\Temp\arduino_build_*\PaperTapeReader.ino.with_bootloader.hex
  // with High Fuse Byte set to 0xD2
  //
  // To eliminate boot loader, compile with Arduino then upload
  // C:\Users\hansel\AppData\Local\Temp\arduino_build_*\ACR.ino.hex
  // with High Fuse Byte set to 0xD7
  //
  // Fuse settings:         Low   High   Ext
  // with    boot loader:  0xFE   0xD2  0xFD
  // without boot loader:  0xFE   0xD7  0xFD
*/

// optimize code for performance (speed)
#pragma GCC optimize ("-O2")

#define PIN_LED 12  // digital pin for "good data" LED

// timing values for audio output:
// 8Mz main clock with /32 pre-scaler, the output toggles on 
// timer compare match so the timer must count up twice for a full wave:
#define OCRA_1850HZ  67 // 8000000/32/2/1850 = 67.57 = 541 microseconds
#define OCRA_2400HZ  52 // 8000000/32/2/2400 = 52.08 = 417 microseconds

// timing values for audio input
#define PERIOD_MAX_US 2000  // 1000Hz = 2000 microseconds
#define PERIOD_MID_US  471  // 2125Hz =  471 microseconds
#define PERIOD_MIN_US  333  // 3000Hz =  333 microseconds

// number of good audio pulses in a row required before entering "send" mode
#define NUM_GOOD_PULSES 100

// number of bad audio pulses in a row required before exiting "send" mode
// at 300 baud and audio frequencies of 1850 and 2400Hz, one bit is made up
// of 6-8 pulses. So if we just keep outputting the previous bit value then
// one or two bad pulses may not result in a receive error.
#define NUM_BAD_PULSES  3

// timer value for current audio output frequency
volatile byte audio_out_timer_value = OCRA_2400HZ, audio_in_data = true;

// status register bits
#define ST_RDRF 0x01  // 0=receiver data register full
#define ST_PE   0x04  // 1=parity error
#define ST_FE   0x08  // 1=framing error
#define ST_ROR  0x10  // 1=receiver overrun
#define ST_TDRE 0x80  // 0=transmitter data register empty

volatile byte regTransmit, regOut[2];
#define regStatus regOut[0]
#define regData   regOut[1]

// are we in "send" mode?
volatile bool send_data = false;

byte bitCounterOut, bitCounterIn, totalBits, dataBits, parity, stopBits, ticksPerBit;
unsigned int regShiftOut, regShiftIn, stopBitMask;

byte parityTable[32] = {0x96, 0x69, 0x69, 0x96, 0x69, 0x96, 0x96, 0x69, 
                        0x69, 0x96, 0x96, 0x69, 0x96, 0x69, 0x69, 0x96, 
                        0x69, 0x96, 0x96, 0x69, 0x96, 0x69, 0x69, 0x96, 
                        0x96, 0x69, 0x69, 0x96, 0x69, 0x96, 0x96, 0x69};

#define GET_EVEN_PARITY(d) (parityTable[(d)/8] & (1<<((d)&7)))


void busINP()
{
  byte reg  = PINC & 0x01;   // read register number
  PORTD = regOut[reg];       // set output data
  DDRD = 0xFF;               // switch data bus pins to output
  PORTC &= ~0x02;            // set WAIT to LOW
  while( (PINB & 0x02)==0 ); // wait until INP signal ends
  DDRD = 0;                  // switch data bus pins to input
  PORTC |= 0x02;             // set WAIT back to HIGH

  if( reg==1 )
    {
      regStatus &= ~(ST_ROR|ST_FE|ST_PE);
      regStatus |= ST_RDRF;
    }
}


void busOUT()
{
  // read input data
  byte reg  = PINC & 0x01;   // read register number
  byte data = PIND;          // read input data
  PORTC &= ~0x02;            // set WAIT to LOW
  while( (PINC & 0x04)==0 ); // wait until OUT signal ends
  PORTC |= 0x02;             // set WAIT back to HIGH

  if( reg==1 && (regStatus & ST_TDRE)==0 )
    {
      regTransmit = data;
      regStatus |= ST_TDRE;
    }
}


ISR(PCINT0_vect)
{
  // pin change on port B, only PB1 (bus input request) is enabled
  busINP();
  PCIFR |= bit(PCIF0);
}


ISR(PCINT1_vect)
{
  // pin change on port C, only PC2 (bus output request) is enabled
  busOUT();
  PCIFR |= bit(PCIF1);
}


void beginSoftSerial(float baud, int params)
{
  ticksPerBit   = (((float) F_CPU) / 256.0) / baud;
  bitCounterOut = 0xff;
  bitCounterIn  = 0xff;

  switch( params )
    {
    case SERIAL_7E2: dataBits = 7; parity = 2; stopBits = 2; break;
    case SERIAL_7O2: dataBits = 7; parity = 1; stopBits = 2; break;
    case SERIAL_7E1: dataBits = 7; parity = 2; stopBits = 1; break;
    case SERIAL_7O1: dataBits = 7; parity = 1; stopBits = 1; break;
    case SERIAL_8N2: dataBits = 8; parity = 0; stopBits = 2; break;
    case SERIAL_8N1: dataBits = 8; parity = 0; stopBits = 1; break;
    case SERIAL_8E1: dataBits = 8; parity = 2; stopBits = 1; break;
    case SERIAL_8O1: dataBits = 8; parity = 1; stopBits = 1; break;
    default        : dataBits = 8; parity = 0; stopBits = 1; break;
    }

  totalBits = dataBits + stopBits + (parity>0);
  stopBitMask = ((1<<stopBits)-1) << (dataBits + (parity>0));
}


void handleSoftSerial()
{
  // check whether we should start sending a new byte
  if( bitCounterOut==0xff )
    {
      noInterrupts();
      if( (regStatus & ST_TDRE)!=0 )
        {
          // copy data to shift register
          regShiftOut = regTransmit & ((1<<dataBits)-1) | stopBitMask;
          regStatus &= ~ST_TDRE;
          bitCounterOut = totalBits;

          // send start bit
          set_audio_out(0);

          // set timer for compare match at first data bit
          OCR0A = TCNT0 + ticksPerBit;
          TIFR0 |= bit(OCF0A);
        }
      interrupts();
    }

  // send next data bit
  if( bitCounterOut<0xff && (TIFR0 & bit(OCF0A))!=0 )
    {
      // set audio data output
      if( bitCounterOut>0 ) set_audio_out((regShiftOut & 0x01)!=0);

      // set up timer 0 for compare match at next data bit
      OCR0A += ticksPerBit;
      TIFR0 |= bit(OCF0A);

      // prepare next data bit
      regShiftOut >>= 1;
      bitCounterOut--;
    }

  // check whether we can see the the beginning of a start bit
  if( bitCounterIn==0xff && audio_in_data==false )
    {
      // set up timer 0 for compare match (middle of bit)
      OCR0B = TCNT0 + ticksPerBit/2;
      TIFR0 |= bit(OCF0B);

      // prepare for reading data bits
      regShiftIn = 0;
      bitCounterIn = totalBits + 1;
    }

  // receive next data bit
  if( bitCounterIn<0xff && (TIFR0 & bit(OCF0B))!=0 )
    {
      // read current audio data bit
      bool data = audio_in_data;

      // set up timer 0 for compare match at next data bit
      OCR0B += ticksPerBit;
      TIFR0 |= bit(OCF0B);

      // prepare for next data bit
      regShiftIn >>= 1;
      regShiftIn |= (data ? 1 : 0) << (totalBits-1);
      bitCounterIn--;

      if( bitCounterIn==0 )
        {
          noInterrupts();
          if( (regStatus & ST_RDRF)==0 )
            regStatus |= ST_ROR;
          else if( send_data )
            {
              regData = regShiftIn & ((1 << dataBits)-1);
              
              if( (regShiftIn & stopBitMask) != stopBitMask )
                regStatus |= ST_FE;
              else if( (parity==1 && (((GET_EVEN_PARITY(regData))==0) != ((regShiftIn & (1<<dataBits))!=0))) || 
                       (parity==2 && (((GET_EVEN_PARITY(regData))!=0) != ((regShiftIn & (1<<dataBits))!=0))) )
                regStatus |= ST_PE;

              regStatus &= ~ST_RDRF;
            }

          bitCounterIn = 0xff;
          interrupts();
        }
      else if( bitCounterIn==totalBits && data ) 
        {
          // start bit value was "1" => not a proper start bit => ignore
          bitCounterIn = 0xff;
        }
    }
}


void got_pulse(bool good)
{
 static volatile byte good_pulses = 0, bad_pulses = 0;

 if( good )
  {
    bad_pulses = 0;
    if( good_pulses<NUM_GOOD_PULSES ) 
      {
        good_pulses++;
        if( good_pulses==NUM_GOOD_PULSES ) 
          { 
            digitalWrite(PIN_LED, HIGH); 
            send_data = true; 
            set_audio_in(true);
          }
        else
          {
            // enable timer 1 overflow interrupt (checks for too-long pulse)
            TIFR1  |= bit(OCF1A);
            TIMSK1 |= bit(OCIE1A);
          }
      }
  }
 else if( bad_pulses<NUM_BAD_PULSES )
   {
     bad_pulses++;
     if( bad_pulses==NUM_BAD_PULSES )
       {
         good_pulses = 0;    
         if( send_data )
           {
             digitalWrite(PIN_LED, LOW); 
             set_audio_in(true);
             send_data = false;
             regStatus &= ~(ST_ROR|ST_FE|ST_PE);
             regStatus |= ST_RDRF;
           }

         // disable timer 1 overflow interrupt (only needed while decoding)
         TIMSK1 &= ~bit(OCIE1A);
       }
   }
}


// called when timer 2 reaches its maximum value (compare match) 
// this interrupt only gets enabled when needed
ISR(TIMER2_COMPA_vect)
{
  // set audio out frequency for next wave
  OCR2A = audio_out_timer_value;  

  // disable interrupts for audio output timer (i.e. this ISR)
  TIMSK2 = 0;
}


void set_audio_in(bool data)
{
  // set debug output according to data
  if( data ) PORTC |= 0x10; else PORTC &= ~0x10;
  audio_in_data = data;
}


void set_audio_out(bool data)
{
  static bool prevData = true;
  if( data != prevData )
    {
      // set debug output according to data
      if( data ) PORTC |= 0x08; else PORTC &= ~0x08;
      
      // reset compare match interrupt flag so we react to interrupts that
      // happen while (but not before) processing the serial input change
      TIFR2 = bit(OCIE2A);
      
      // set audio out frequency (wave length) depending on serial input
      audio_out_timer_value = data ? OCRA_2400HZ : OCRA_1850HZ;
      
      // if the timer counter is smaller than target value (minus a small safety margin)
      // then set the new new target value now, otherwise enable compare match
      // interrupt and set the new target value in the interrupt handler
      if( TCNT2 < audio_out_timer_value-1 ) 
        OCR2A = audio_out_timer_value;
      else 
        TIMSK2 = bit(OCIE2A);

      prevData = data;
    }
}


// called when a rising edge is detected at the ICP1 input (pin 8) 
ISR(TIMER1_CAPT_vect) 
{ 
  // reset timer 
  TCNT1 = 0;

  // reset output compare A interrupt in case it occurred simultaneously
  TIFR1 = bit(OCF1A);

  // timer ticks equals microseconds since previous rising edge
  // (8Mz clock with prescaler 8 => 1 microsecond per tick)
  unsigned int ticks = ICR1;

  if( ticks<PERIOD_MIN_US )
    got_pulse(false); // too short pulse => bad pulse
  else 
    {
      // detected pulse with PERIOD_MIN_US <= ticks <= PERIOD_MAX_US
      // (we can't get here with ticks>PERIOD_MAX_US
      // because the COMPA interrupt would have been triggered before that)
      static bool p1 = true, p2 = true;
      bool p3 = ticks<PERIOD_MID_US;
      
      if( p1!=p2 && p2!=p3 )
        {
          // of the three most recent pulses, the middle one was different
          // from the first and last => bad pulse (one bit is made up of 
          // about six pulses so there should never be a short->long->short
          // or long->short->long sequence)
          got_pulse(false);
          p2 = p3;
        }
      else
        {
          // previous pulse was good => send it.
          // note that what is being sent out the serial interface is two
          // pulses (or about 1/3 bit) behind the incoming audio: first the
          // incoming audio pulse has to complete before the computer can see
          // its length and then we stay one more pulse behind so we can
          // detect single-pulse errors (see above).
          got_pulse(true);
          if( send_data ) set_audio_in(p2);
          p1 = p2; p2 = p3;
        }
    }
}


// called when timer1 reaches its maximum value at PERIOD_MAX_US (2000)
ISR(TIMER1_COMPA_vect)
{  
  // reset timer 
  TCNT1 = 0;

  // reset input capture interrupt in case it occurred simultaneously
  TIFR1 = bit(ICF1);
  ICR1  = 0;
  
  // we haven't seen a rising edge for PERIOD_MAX_US microseconds => bad pulse
  got_pulse(false);
}


void setup() 
{
  pinMode(11, OUTPUT);      // audio output
  pinMode(13, OUTPUT);
  pinMode(A0, INPUT);       // address bus bit 0
  pinMode(A1, OUTPUT);      // WAIT signal
  pinMode(A3, OUTPUT);      // serial data out for audio (debug)
  pinMode(A4, OUTPUT);      // serial data in from audio (debug)
  pinMode(PIN_LED, OUTPUT); // "good data" LED

  // set all data bus pins to INPUT mode
  DDRD = 0;
  
  // set WAIT output high
  digitalWrite(A1, HIGH);

  // set up timer2 to produce 1850/2400Hz output wave on OC2A (pin 17)
  // WGM22:20 = 010 => clear-timer-on-compare (CTC) mode 
  // COM2A1:0 =  01 => toggle OC2A on compare match
  // CS22:20  = 011 => CLK/32 prescaler
  TCCR2A = bit(COM2A0) | bit(WGM21);
  TCCR2B = bit(CS21) | bit(CS20);

  // set initial audio output frequency
  audio_out_timer_value = OCRA_2400HZ;
  OCR2A  = audio_out_timer_value;
  digitalWrite(A3, HIGH);
  
  // set up timer1 to create an input capture interrupt when detecting a 
  // rising edge at the ICP1 input (pin 8) or when reaching PERIOD_MAX_US
  TCCR1A = 0;  
  TCCR1B = bit(ICNC1) | bit(ICES1) | bit(CS11); // enable input capture noise canceler, select rising edge and prescaler 8
  TCCR1C = 0;  
  OCR1A  = PERIOD_MAX_US; // set up for output compare match A after PERIOD_MAX_US microseconds
  TIMSK1 = bit(ICF1); // enable interrupt on input capture

  // set up LED to signal whether we are receiving good data from audio
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // set up timer0 with /256 prescaler and disable its overflow interrupts
  TIMSK0 &= ~bit(TOIE0);
  TCCR0A = 0;
  TCCR0B = bit(CS02); // /256 prescaler
  TCNT0  = 0;

  // set up pin change interrupts for OUT and INP
  PCICR  |= 0x03; // turn on pin change interrupts for port b+c
  PCMSK0 |= 0x02; // turn on pin PB1 (INP)
  PCMSK1 |= 0x04; // turn on pin PC2 (OUT)

  // initialize status registers
  regStatus = ST_RDRF;
  
  // always use 300 baud 8N1 for ACR
  beginSoftSerial(300, SERIAL_8N1);
}


void loop() 
{
  while(1) handleSoftSerial();
}
