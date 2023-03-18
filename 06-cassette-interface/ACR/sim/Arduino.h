#ifndef ARDUINO_H
#define ARDUINO_H

#include <stdio.h>
#include <stdlib.h>

#define ISR(t) void ISR_##t()

ISR(PCINT0_vect);
ISR(PCINT1_vect);
ISR(TIMER2_COMPA_vect);
ISR(TIMER1_CAPT_vect);
ISR(TIMER1_COMPA_vect);

void setup();
void loop();

extern int clockfreq;
extern unsigned int clock;

void setTimer1Capture(bool data);
bool getTimer2OutputA();
void advanceClock();
double getTime();

typedef unsigned char byte;
#define HIGH true
#define LOW false
#define F_CPU 8000000
enum { SERIAL_7E2, SERIAL_7O2, SERIAL_7E1, SERIAL_7O1, SERIAL_8N2, SERIAL_8N1, SERIAL_8E1, SERIAL_8O1 };
void interrupts();
void noInterrupts();
void digitalWrite(int pin, bool v);
byte digitalRead(int pin);
void pinMode(int pin, int mode);

#define min(a, b) ((a)<(b)?(a):(b))
#define max(a, b) ((a)>(b)?(a):(b))
#define bit(n) (1<<n)

#define PCIF0 0
#define PCIF1 1

// TCCR0A
// TCCR0B
#define CS02 2
#define CS01 1
#define CS00 0
// TIMSK0
#define OCIE0B 2
#define OCIE0A 1
// TIFR0
#define TOIE0  0
#define OCF0A  1
#define OCF0B  2

// TCCR1A
// TCCR1B
#define ICNC1 7
#define ICES1 6
#define WGM13 4
#define WGM12 3
#define CS12  2
#define CS11  1
#define CS10  0
// TIMSK1
#define ICIE1  5
#define OCIE1B 2
#define OCIE1A 1
#define TOIE1  0
// TIFR1
#define OCF1A 1
#define OCF1B 2
#define ICF1  1

// TCCR2A
#define COM2A1 7
#define COM2A0 6
#define COM2B1 5
#define COM2B0 4
#define WGM21  1
#define WGM20  0
// TCCR2B
#define FOC2A  7
#define FOC2B  6
#define WGM22  3
#define CS22   2
#define CS21   1
#define CS20   0
// TIFR2
#define OCF2B  2
#define OCF2A  1
#define TOV2   0
// TIMSK2
#define OCIE2A 1
#define OCIE2B 2


#define OUTPUT 0
#define INPUT 1
#define INPUT_PULLUP 2

#define A0 0
#define A1 1
#define A2 2
#define A3 3
#define A4 4
#define A5 5

extern byte TCCR0A, TCCR0B, TCCR1C, TIMSK0, TIFR0, TCNT0, OCR0A, OCR0B;
extern byte TCCR2A, TCCR2B, TCCR2C, TIMSK2, TIFR2, TCNT2, OCR2A, OCR2B;
extern byte TCCR1A, TCCR1B, TCCR1C, TIMSK1, TIFR1;
extern unsigned short TCNT1, ICR1, OCR1A, OCR1B;

extern byte DDRB, DDRC, DDRD, PINB, PINC, PIND, PORTB, PORTC, PORTD;
extern byte PCIFR, PCICR, PCMSK0, PCMSK1, PCMSK2;

#endif
