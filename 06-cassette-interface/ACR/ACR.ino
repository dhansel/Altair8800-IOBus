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
  D10    16     PB2        out         if LOW, do not allow tape format switching (always use MITS)
  D11    17     PB3        out         audio signal out (OC2A for timer 2)
  D12    18     PB4        in/out      "good data" LED
  D13    19     PB5        in/out      unused
  A0     23     PC0        in          address bus bit 0
  A1     24     PC1        out         WAIT signal
  A2     25     PC2        in          bus output request (OUT)
  A3     26     PC3        out         serial data for audio out (debug)
  A4     27     PC4        out         decoded serial data from audio in (debug)
  A5     28     PC5        in          if LOW, do speed skew compensation

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

#include <Arduino.h>

// optimize code for performance (speed)
#pragma GCC optimize ("-O2")

// if defined, show serial data on PC3 (audio out) and PC4 (audio in) 
#define DEBUG 

#define PIN_LED 12  // digital pin for "good data" LED

// timing values for audio output:
// 8Mz main clock with /32 pre-scaler, the output toggles on 
// timer compare match so the timer must count up twice for a full wave:
#define OCRA_2400HZ  51 // 8000000/32/2/2400 =  52.08 =  417 microseconds
#define OCRA_1850HZ  66 // 8000000/32/2/1850 =  67.57 =  541 microseconds
#define OCRA_1200HZ 103 // 8000000/32/2/1200 = 104.17 =  834 microseconds

// timing values for audio input (MITS format, 0-bit=1850Hz, 1-bit=2400Hz)
#define MITS_PERIOD_MAX_US 1000
#define MITS_PERIOD_LNG_US  540  // 1850Hz = 540 microseconds
#define MITS_PERIOD_MID_US  (MITS_PERIOD_LNG_US+MITS_PERIOD_SRT_US)/2
#define MITS_PERIOD_SRT_US  417  // 2400Hz = 417 microseconds
#define MITS_PERIOD_MIN_US  250

// timing values for audio input (KCS format, 0-bit=1200Hz, 1-bit=2400Hz)
#define KCS_PERIOD_MAX_US 1250
#define KCS_PERIOD_LNG_US  833  // 1200Hz =  833 microseconds
#define KCS_PERIOD_MID_US  (KCS_PERIOD_LNG_US+KCS_PERIOD_SRT_US)/2
#define KCS_PERIOD_SRT_US  417  // 2400Hz =  417 microseconds
#define KCS_PERIOD_MIN_US  250

// timing values for audio input (CUTS format, 0-bit=600Hz, 1-bit=1200Hz)
// CUTS uses one full 1200Hz wave for a 1 bit and half of a 600Hz wave for a 0 bit
#define CUTS_PERIOD_MAX_US 1250
#define CUTS_PERIOD_LNG_US  833 //  600Hz half-wave = 833 microseconds
#define CUTS_PERIOD_MID_US  (CUTS_PERIOD_LNG_US+CUTS_PERIOD_SRT_US)/2
#define CUTS_PERIOD_SRT_US  417 // 1200Hz half-wave = 417 microseconds 
#define CUTS_PERIOD_MIN_US  250

// number of good audio pulses in a row required before entering "send" mode
byte min_good_pulses = 250;

// number of bad audio pulses in a row required before exiting "send" mode
// at 300 baud and audio frequencies of 1850 and 2400Hz, one bit is made up
// of 6-8 pulses. So if we just keep outputting the previous bit value then
// one or two bad pulses may not result in a receive error.
byte max_bad_pulses = 3;

// timer value for current audio output frequency
volatile byte audio_out_timer_value = OCRA_2400HZ;

// defines for tape formats
#define TF_MITS 1 // MITS format (1850Hz/2400Hz, 300 baud 8N1)
#define TF_KCS  2 // Kansas City format (1200Hz/2400Hz, 300 baud 8N2)
#define TF_CUTS 3 // CUTS 1200 baud format (1200 baud 8N2)

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
volatile bool send_data = false, wait_first_cycle = true, audio_in_data = true, do_sample_bit = false;
volatile byte tape_format = TF_MITS;
volatile unsigned int period_min_us, period_srt_us, period_mid_us, period_lng_us, period_max_us;
volatile int period_skew_us = 0;

byte bitCounterOut, bitCounterIn, totalBits, dataBits, parity, stopBits, ticksPerBit, totalBitsRecv;
unsigned int regShiftOut, regShiftIn, stopBitMask, stopBitMaskRecv;

byte parityTable[32] = {0x96, 0x69, 0x69, 0x96, 0x69, 0x96, 0x96, 0x69, 
                        0x69, 0x96, 0x96, 0x69, 0x96, 0x69, 0x69, 0x96, 
                        0x69, 0x96, 0x96, 0x69, 0x96, 0x69, 0x69, 0x96, 
                        0x96, 0x69, 0x69, 0x96, 0x69, 0x96, 0x96, 0x69};

#define GET_EVEN_PARITY(d) (parityTable[(d)/8] & (1<<((d)&7)))

void setTapeFormat(byte format);
void set_audio_in(bool data);
void set_audio_out(bool data);


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
  else if( reg==0 && (PINB & 0x04)!=0 )
    {
      // writing control register
      // for the MITS ACR the control register only uses bits 0+1 to enable 
      // transmit/receive interrupts which we don't support here
      // for the CUTS ACR board the control register bits 6+7 determine which
      // unit is used (only have one unit here) and bit 5 switches tape 
      // speed (0=1200 baud,1=300 baud)

      if( (data & 0xC0)==0 )
        {
          // unit selection bits 6+7 are both 0 (no unit selected) 
          // => this write is NOT coming from CUTER => select MITS format
          setTapeFormat(TF_MITS);
        }
      else if( (data & 0x20)==0 )
        {
          // write is coming from CUTER and bit 5 is clear => select CUTS format
          setTapeFormat(TF_CUTS);
        }
      else
        {
          // write is coming from CUTER and bit 5 is set (300 baud KCS format)
          // if unit 1 is selected (bit 7 set) then use KCS format
          // otherwise use MITS format. That allows selecting MITS format
          // in CUTER by issuing "SET TAPE 1" (set 300 baud KCS format) 
          // followed by "GET /2" to load from the second unit
          setTapeFormat((data & 0x80)!=0 ? TF_KCS : TF_MITS);
        }
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
  ticksPerBit = (((float) F_CPU) / 256.0) / baud;
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

  // for receiving we don't require two stop bits, one is enough
  // and some KCS recordings only have one stop bit
  totalBitsRecv = stopBits==2 ? totalBits-1 : totalBits;
  stopBitMaskRecv = 1 << (dataBits + (parity>0));
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
  else if( (TIFR0 & bit(OCF0A))!=0 )
    {
      // send next data bit

      // set audio data output
      if( bitCounterOut>0 ) set_audio_out((regShiftOut & 0x01)!=0);

      // set up timer 0 for compare match at next data bit
      OCR0A += ticksPerBit;
      TIFR0 |= bit(OCF0A);

      // prepare next data bit
      regShiftOut >>= 1;
      bitCounterOut--;

      // timer will be off at this point when in CUTS mode
      // if there is not more data to send then we need to turn the timer back 
      // on now to produce the "idle tone"
      if( (tape_format==TF_CUTS) && bitCounterOut==0xFF && (regStatus & ST_TDRE)==0 )
        TCCR2B = bit(FOC2A) | bit(CS21) | bit(CS20); 
    }

  // note: period_skew_us is detected speed skew in microseconds
  // MITS/KCS: 8 short pulses per bit and 32 microseconds per tick => /4
  // CUTS: timer is not used

  // check whether we can see the the beginning of a start bit
  // (for CUTS format only check if do_sample_bit is set)
  if( bitCounterIn==0xff && audio_in_data==false && (tape_format!=TF_CUTS || do_sample_bit) )
    {
      // set up timer 0 for compare match (middle of bit)
      OCR0B = TCNT0 + (ticksPerBit+period_skew_us/4)/2;

      // prepare for reading data bits
      regShiftIn = 0;
      bitCounterIn = totalBitsRecv+1;

      // clear OCF0B (timer output compare match) flag
      TIFR0 |= bit(OCF0B);
    }

  // for CUTS format, do_sample_bit is set in TIMER1_CAPT_vect interrupt routine,
  // otherwise set it true here if the timer output compare match happened
  if( tape_format!=TF_CUTS ) do_sample_bit = (TIFR0 & bit(OCF0B))!=0;
  
  // receive next data bit
  if( bitCounterIn<0xff && do_sample_bit )
    {
      // read current audio data bit
      bool data = audio_in_data;

      // set up timer 0 for compare match at next data bit
      OCR0B += ticksPerBit+period_skew_us/4;
      TIFR0 |= bit(OCF0B);
      do_sample_bit = false;
      PINC |= 0x08;

      // prepare for next data bit
      regShiftIn >>= 1;
      regShiftIn |= (data ? 1 : 0) << (totalBitsRecv-1);
      bitCounterIn--;

      if( bitCounterIn==0 )
        {
          noInterrupts();
          if( (regStatus & ST_RDRF)==0 )
            regStatus |= ST_ROR;
          else if( send_data )
            {
              regData = regShiftIn & ((1 << dataBits)-1);
              
              if( (regShiftIn & stopBitMaskRecv) != stopBitMaskRecv )
                regStatus |= ST_FE;
              else if( (parity==1 && (((GET_EVEN_PARITY(regData))==0) != ((regShiftIn & (1<<dataBits))!=0))) || 
                       (parity==2 && (((GET_EVEN_PARITY(regData))!=0) != ((regShiftIn & (1<<dataBits))!=0))) )
                regStatus |= ST_PE;

              regStatus &= ~ST_RDRF;
            }

          bitCounterIn = 0xff;
          interrupts();
        }
      else if( bitCounterIn==totalBitsRecv && data ) 
        {
          // start bit value was "1" => not a proper start bit => ignore
          bitCounterIn = 0xff;
        }
    }
}


void got_pulse(bool good, bool shortPulse = false, unsigned int len = 0)
{
 static volatile byte good_pulses = 0, bad_pulses = 0;

 if( good )
   {
     bad_pulses = 0;
     if( good_pulses<min_good_pulses ) 
       {
         good_pulses++;
         if( good_pulses==min_good_pulses ) 
           { 
             digitalWrite(PIN_LED, HIGH); 
             send_data = true; 
             wait_first_cycle = true;
             set_audio_in(true);
           }
         else
           {
             // enable timer 1 overflow interrupt (checks for too-long pulse)
             TIFR1  |= bit(OCF1A);
             TIMSK1 |= bit(OCIE1A);
           }
       }

     // if PC5 is tied to ground then use speed skew comensation
     if( (PINC & 0x20)==0 )
       {
         // in KCS and CUTS format, a long pulse is twice the length of a short pulse
         // in MITS format the two are much more similar
         unsigned int expected_len;
         if( shortPulse )
           expected_len = period_srt_us + period_skew_us;
         else if( tape_format==TF_MITS )
           expected_len = period_lng_us + period_skew_us + period_skew_us/4;
         else
           expected_len = period_lng_us + period_skew_us*2;

         if( len>expected_len )
           period_skew_us++;
         else if( len<expected_len )
           period_skew_us--;
       }
   }
 else if( bad_pulses<max_bad_pulses )
   {
     bad_pulses++;
     if( bad_pulses==max_bad_pulses )
       {
         good_pulses = 0;    
         if( send_data )
           {
             digitalWrite(PIN_LED, LOW); 
             set_audio_in(true);
             send_data = false;
             regStatus &= ~(ST_ROR|ST_FE|ST_PE);
             regStatus |= ST_RDRF;
             period_skew_us = 0;
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
  if( tape_format==TF_CUTS )
    {
      // stop timer - we need to avoid the case where the following bit comes
      // in just a tad late and the timer hits again right before we process
      // the bit (which would then toggle the output twice)
      TCCR2B = 0;
    }
  else
    {
      // set audio out frequency for next wave
      OCR2A = audio_out_timer_value;  
    }
      
  // disable interrupts for audio output timer (i.e. this ISR)
  TIMSK2 = 0;
}


void set_audio_in(bool data)
{
#ifdef DEBUG
  // set debug output according to data
  if( data ) PORTC |= 0x10; else PORTC &= ~0x10;
#endif
  audio_in_data = data;
}


// called when producing audio output (at the beginning of each bit)
void set_audio_out(bool data)
{
  static bool prevData = true;

  if( tape_format==TF_CUTS )
    {
      // for CUTS 1200 baud mode, a "1" bit is one full cycle of a 1200Hz wave
      // and a "0" bit is one HALF cycle of a 600Hz wave
      if( data ) 
        {
          // "1" bit => toggle output, start timer (1200Hz) and enable interrupt
          // we need the timer to toggle the output once in the middle of the bit
          // (to produce a 1200Hz wave)
          TCNT2 = 0; 
          TCCR2B = bit(FOC2A) | bit(CS21) | bit(CS20); 
          TIFR2  = bit(OCF2A);
          TIMSK2 = bit(OCIE2A);
        }
      else if( TCCR2B==0 )
        {
          // "0" bit and timer is not running => toggle output (next toggle will
          // be at the beginning of the next bit producing a  600Hz half wave)
          TCCR2B = bit(FOC2A);
        }
      else
        {
          // "0" bit and timer is still running => wait until timer resets (and toggles output)
          // (this happens when sending the first start bit after inactivity)
#if !(defined(_WIN32) || defined(__linux__))
          // this "while" loop would block indefinitely in the simulation environment
          while( TCNT2 );
#endif
          TCCR2B = 0;
        }

#ifdef DEBUG
      // set debug output according to data
      if( data ) PORTC |= 0x08; else PORTC &= ~0x08;
#endif
    }
  else if( data != prevData )
    {
      // MITS or KCS format

#ifdef DEBUG
      // set debug output according to data
      if( data ) PORTC |= 0x08; else PORTC &= ~0x08;
#endif      
      // reset compare match interrupt flag so we react to interrupts that
      // happen while (but not before) processing the serial input change
      TIFR2 = bit(OCF2A);
      
      // set audio out frequency (wave length) depending on serial input
      if( data )
        audio_out_timer_value = OCRA_2400HZ;
      else if( tape_format==TF_MITS )
        audio_out_timer_value = OCRA_1850HZ;
      else
        audio_out_timer_value = OCRA_1200HZ;
      
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

  // (we can't get here with ticks>period_max_us
  // because the COMPA interrupt would have been triggered before that)
  if( ticks<period_min_us )
    got_pulse(false); // pulse too short => bad pulse
  else if( tape_format==TF_CUTS )
    {
      // CUTS 1200 baud format works on half-waves instead of full waves
      // => flip trigger edge for input capture
      TCCR1B = TCCR1B ^ bit(ICES1);

      // detected valid pulse 
      static unsigned int prev_ticks = 0;
      static bool start_of_cycle = true;

      // CUTS format is very sensitive to imbalances in duty cycle so we
      // add extra ticks from the previous pulse to the current one
      bool shortpulse = ticks < (CUTS_PERIOD_MID_US+period_skew_us);

      //printf("pulse: %.6f %i %i %i %i %i => %i\n", getTime(), start_of_cycle, period_skew_us, ticks, ticks+period_skew_us, CUTS_PERIOD_MID_US, shortpulse);

      if( start_of_cycle && shortpulse )
        {
          // we were at the start of a cycle and received a short pulse
          // => wait until end of cycle
          start_of_cycle = false;
          prev_ticks = ticks;
        }
      else
        {
          // received a pulse
          // if we are not yet sending (i.e. still within the leader), require a reasonable duty cycle
          // to count a pulse as good, that way the LED can be used as an indicator to help calibrate
          // the volume (greater volume generally improves the duty cycle)
          // if we are already sending then ignore the duty cycle and hope for the best
          if( shortpulse )
            got_pulse(send_data || abs((int) ticks-(int) prev_ticks)<100, true, (prev_ticks+ticks)/2);
          else
            got_pulse(true, false, ticks);

          if( send_data )
            { 
              // set audio input data
              set_audio_in(shortpulse);

              // tell handleSoftSerial() to sample the bit
              do_sample_bit = true;
            }
          
          // at beginning of a cycle now
          start_of_cycle = true;
        }
    }
  else
    {
      // MITS or KCS format (300 baud), valid pulse detected
      static byte shortpulses = 0, longpulses = 0;

      // long pulses are twice as long as short pulses so we have to treat
      // them differently to get an even bit length for the serial receive
      if( ticks<(period_mid_us+period_skew_us) )
        {
          // short pulse
          got_pulse(true, true, ticks);
          if( shortpulses<3 )
            shortpulses++;
          else
            {
              // we have seen four consecutive short pulses => "1" bit detected
              longpulses = 0;
              if( send_data ) set_audio_in(true);
            }
        }
      else
        {
          // long pulse
          got_pulse(true, false, ticks);
          if( longpulses<(tape_format==TF_KCS ? 1 : 2) )
            longpulses++;
          else
            {
              // we have seen two/three consecutive long pulses => "0" bit detected
              shortpulses = 0;
              if( send_data ) set_audio_in(false);
            }
        }
    }

  // ICF1 gets reset automatically when the interrupt routine is invoked
  // sometimes noise can set it again immediately so we reset it here again
  TIFR1 = bit(ICF1);
}


// called when timer1 reaches its maximum value at period_max_us
ISR(TIMER1_COMPA_vect)
{  
  // reset timer 
  TCNT1 = 0;

  // reset input capture interrupt in case it occurred simultaneously
  TIFR1 = bit(ICF1);
  ICR1  = 0;
  
  // we haven't seen a rising edge for period_max_us microseconds => bad pulse
  got_pulse(false);
}


void setTapeFormat(byte format)
{
  switch( format )
    {
    case TF_KCS:  
      tape_format = TF_KCS;
      period_min_us = KCS_PERIOD_MIN_US;
      period_srt_us = KCS_PERIOD_SRT_US;
      period_mid_us = KCS_PERIOD_MID_US;
      period_lng_us = KCS_PERIOD_LNG_US;
      period_max_us = KCS_PERIOD_MAX_US;
      audio_out_timer_value = OCRA_2400HZ;
      max_bad_pulses = 3;
      beginSoftSerial(300, SERIAL_8N2); 
      break;

    case TF_CUTS: 
      tape_format = TF_CUTS;
      period_min_us = CUTS_PERIOD_MIN_US;
      period_srt_us = CUTS_PERIOD_SRT_US;
      period_mid_us = CUTS_PERIOD_MID_US;
      period_lng_us = CUTS_PERIOD_LNG_US;
      period_max_us = CUTS_PERIOD_MAX_US;
      audio_out_timer_value = OCRA_1200HZ;
      max_bad_pulses = 1;
      beginSoftSerial(1200, SERIAL_8N2); 
      break;

    case TF_MITS:
    default:
      tape_format = TF_MITS;
      period_min_us = MITS_PERIOD_MIN_US;
      period_srt_us = MITS_PERIOD_SRT_US;
      period_mid_us = MITS_PERIOD_MID_US;
      period_lng_us = MITS_PERIOD_LNG_US;
      period_max_us = MITS_PERIOD_MAX_US;
      audio_out_timer_value = OCRA_2400HZ;
      max_bad_pulses = 3;
      beginSoftSerial(300,  SERIAL_8N1); 
      break;
    }

  // reset speed skew value
  period_skew_us = 0;

  // set maximum pulse length
  OCR1A = period_max_us;

  // set line idle frequency;
  OCR2A = audio_out_timer_value;

  // set rising edge for input compare (might have been changed)
  //TCCR1B |= bit(ICES1);
  TCCR1B &= ~bit(ICES1);

  // make sure timer 2 is running (might have been disabled)
  TCCR2B = bit(CS21) | bit(CS20);
}


void setup() 
{
  pinMode(11, OUTPUT);      // audio output
  pinMode(13, OUTPUT);
  pinMode(A0, INPUT);       // address bus bit 0
  pinMode(A1, OUTPUT);      // WAIT signal
#ifdef DEBUG
  pinMode(A3, OUTPUT);      // serial data out for audio (debug)
  pinMode(A4, OUTPUT);      // serial data in from audio (debug)
#endif
  pinMode(10, INPUT_PULLUP);
  pinMode(A5, INPUT_PULLUP);
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
  // rising edge at the ICP1 input (pin 8) or when reaching period_max_us
  TCCR1A = 0;  
  TCCR1B = bit(ICNC1) | bit(ICES1) | bit(CS11); // enable input capture noise canceler, select rising edge and prescaler 8
  TCCR1C = 0;  
  OCR1A  = MITS_PERIOD_MAX_US; // initial output compare match A after MITS_PERIOD_MAX_US microseconds
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

  // set initial tape format
  setTapeFormat(TF_MITS);
}


void loop() 
{
  handleSoftSerial();
}
