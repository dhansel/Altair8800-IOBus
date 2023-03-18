#include "Arduino.h"

#define CLOCKSCALE 8
int clockfreq = 8000000 / CLOCKSCALE;
unsigned int clock = 0;

byte TCCR0A, TCCR0B, TCCR0C, TIMSK0, TIFR0, TCNT0, OCR0A, OCR0B;
byte TCCR2A, TCCR2B, TCCR2C, TIMSK2, TIFR2, TCNT2, OCR2A, OCR2B;
byte TCCR1A, TCCR1B, TCCR1C, TIMSK1, TIFR1;
unsigned short TCNT1, ICR1, OCR1A, OCR1B;

byte DDRB, DDRC, DDRD, PINB, PINC, PIND, PORTB, PORTC, PORTD;
byte PCIFR, PCICR, PCMSK0, PCMSK1, PCMSK2;

static bool timer1capt = false, timer2outputA = false;

__attribute__((weak)) ISR(TIMER0_COMPA_vect) {}
__attribute__((weak)) ISR(TIMER0_COMPB_vect) {}
__attribute__((weak)) ISR(TIMER1_CAPT_vect)  {}
__attribute__((weak)) ISR(TIMER1_COMPA_vect) {}
__attribute__((weak)) ISR(TIMER1_COMPB_vect) {}
__attribute__((weak)) ISR(TIMER2_COMPA_vect) {}
__attribute__((weak)) ISR(TIMER2_COMPB_vect) {}


void interrupts() 
{}


void noInterrupts() 
{}


void pinMode(int pin, int mode)
{}


void setTimer1Capture(bool data)
{
  static unsigned int timer1captPrevClock = 0;
  static bool timer1captPrev = false;

  if( data != timer1captPrev && ((TCCR1B & bit(ICNC1))==0 || (clock-timer1captPrevClock)>=20) )
    {
      ICR1 = TCNT1;
      if( (TCCR1B & bit(ICES1))==0 && timer1captPrev && !data )
        ISR_TIMER1_CAPT_vect();      
      else if( (TCCR1B & bit(ICES1))!=0 && !timer1captPrev && data )
        ISR_TIMER1_CAPT_vect();
      
      timer1captPrev = data;
      timer1captPrevClock = clock;
    }
}


bool getTimer2OutputA()
{
  return timer2outputA;
}



void advanceClock()
{
  static const unsigned int prescaler01[8] = {0, 1, 8, 64, 256, 1024, 0, 0};
  static const unsigned int prescaler2[8]  = {0, 1, 8, 32, 64, 128, 256, 1024};
  unsigned int prescale;

  clock++;
  
  // ------- timer 0
  TIFR0 &= ~(bit(OCF0A) | bit(OCF0B));
  prescale = prescaler01[TCCR0B & 7] / CLOCKSCALE;
  if( prescale>0 && (clock & (prescale-1))==0 )
    {
      TCNT0++;
      if( TCNT0==OCR0A ) TIFR0 |= bit(OCF0A);
      if( TCNT0==OCR0B ) TIFR0 |= bit(OCF0B);

      if( (TIFR0 & bit(OCF0A)) && (TIMSK0 & bit(OCIE0A)) ) { ISR_TIMER0_COMPA_vect(); TIFR0 &= ~bit(OCF0A); }
      if( (TIFR0 & bit(OCF0B)) && (TIMSK0 & bit(OCIE0B)) ) { ISR_TIMER0_COMPB_vect(); TIFR0 &= ~bit(OCF0B); }
    }

  // ------- timer 1
  TIFR1 &= ~(bit(OCF1A) | bit(OCF1B));
  prescale = prescaler01[TCCR1B & 7] / CLOCKSCALE;
  if( prescale>0 && (clock & (prescale-1))==0 )
    {
      TCNT1++;
      if( TCNT1==OCR1A ) TIFR1 |= bit(OCF1A);
      if( TCNT1==OCR1B ) TIFR1 |= bit(OCF1B);

      if( (TIFR1 & bit(OCF1A)) && (TIMSK1 & bit(OCIE1A)) ) { ISR_TIMER1_COMPA_vect(); TIFR1 &= ~bit(OCF1A); }
      if( (TIFR1 & bit(OCF1B)) && (TIMSK1 & bit(OCIE1B)) ) { ISR_TIMER1_COMPB_vect(); TIFR1 &= ~bit(OCF1B); }
    }
  
  // ------- timer 2
  bool ocmatchA = false;
  TIFR2  &= ~(bit(OCF2A) | bit(OCF2B));
  prescale = prescaler2[TCCR2B & 7] / CLOCKSCALE;
  if( prescale>0 && (clock & (prescale-1))==0 )
    {
      TCNT2++;
      if( TCNT2==OCR2A ) { TIFR2 |= bit(OCF2A); ocmatchA = true; }
      if( TCNT2==OCR2B ) { TIFR2 |= bit(OCF2B); ocmatchA = true; }

      if( (TCNT2>=OCR2A+1) && (TCCR2A & (bit(WGM22)|bit(WGM21)|bit(WGM20))) == bit(WGM21) )
        TCNT2 = 0; // clear timer on compare match

      if( (TIFR2 & bit(OCF2A)) && (TIMSK2 & bit(OCIE2A)) ) { ISR_TIMER2_COMPA_vect(); TIFR2 &= ~bit(OCF2A); }
      if( (TIFR2 & bit(OCF2B)) && (TIMSK2 & bit(OCIE2B)) ) { ISR_TIMER2_COMPB_vect(); TIFR2 &= ~bit(OCF2B); }
    }

  loop();

  if( (TCCR2B & bit(FOC2A)) || ocmatchA )
    {
      TCCR2B &= ~bit(FOC2A);
      if( (TCCR2A & bit(COM2A1)) && (TCCR2A & bit(COM2A0)) )
        timer2outputA = true;
      else if( (TCCR2A & bit(COM2A1)) )
        timer2outputA = false;
      else if( (TCCR2A & bit(COM2A0)) )
        timer2outputA = !timer2outputA;
    }
}
