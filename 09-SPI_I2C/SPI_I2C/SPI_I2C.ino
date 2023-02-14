// -----------------------------------------------------------------------------
// Altair I/O bus SPI/I2C card
// Copyright (C) 2022 David Hansel
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
  D8     14     PB0        in/out      address bus bit 1
  D9     15     PB1        in          bus input request (INP)
  D10    16     PB2        out         SPI CS1
  D11    17     PB3        out         SPI SDO
  D12    18     PB4        in          SPI SDI
  D13    19     PB5        out         SPI SCK
  A0     23     PC0        in          address bus bit 0
  A1     24     PC1        out         WAIT signal
  A2     25     PC2        in          bus output request (OUT)
  A3     26     PC3        out         SPI CS2
  A4     27     PC4        in/out      I2C SDA
  A5     28     PC5        out         I2C SCL

  (for SPI naming seE: https://www.oshwa.org/a-resolution-to-redefine-spi-signal-names)

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

#include <SPI.h>
extern "C" { 
#include "twi_master.h" 
}

// pins 0-7: data bus D0-D7
#define PIN_ADDR1     8
#define PIN_INP       9
#define PIN_SPI_CS1  10
#define PIN_SPI_SDO  11
#define PIN_SPI_CDI  12
#define PIN_SPI_CLK  13
#define PIN_ADDR0    A0
#define PIN_WAIT     A1
#define PIN_OUT      A2
#define PIN_SPI_CS2  A3
#define PIN_I2C_SDA  A4
#define PIN_I2C_SCK  A5


// optimize code for performance (speed)
#pragma GCC optimize ("-O2")

volatile byte regOut[4], regIn[4];
volatile byte i2c_bufPtr, i2c_count, i2c_addr;
byte i2c_buffer[256];

#define SPI_REG_STATUS  0
#define SPI_REG_COMMAND 0
#define SPI_REG_DATAIN  1
#define SPI_REG_DATAOUT 1

#define I2C_REG_STATUS  2
#define I2C_REG_COMMAND 2
#define I2C_REG_DATAIN  3
#define I2C_REG_DATAOUT 3

#define i2c_regCommand (regIn[I2C_REG_COMMAND])
#define i2c_regDataIn  (regIn[I2C_REG_DATAIN])
#define i2c_regStatus  (regOut[I2C_REG_STATUS])
#define i2c_regDataOut (regOut[I2C_REG_DATAOUT])
#define spi_regCommand (regIn[SPI_REG_COMMAND])
#define spi_regDataIn  (regIn[SPI_REG_DATAIN])
#define spi_regStatus  (regOut[SPI_REG_STATUS])
#define spi_regDataOut (regOut[SPI_REG_DATAOUT])

#define I2C_ST_BUSY    0x80
#define I2C_ST_WRITING 0x40
#define I2C_ST_READING 0x20

// clock speed (assuming 8MHz system clock, config bits 0-2):
// 0:   62500
// 1:  125000
// 2:  250000
// 3:  500000
// 4: 1000000
// 5: 2000000
// 6: 4000000
// 7: invalid


#define SPI_CFG_CLOCK  0x07  // clock speed (0-7)
#define SPI_CFG_MODE   0x18  // mode (SPI mode 0-3)
#define SPI_CFG_FIRST  0x20  // 0=LSB first, 1=MSB first
#define SPI_CFG_CS1    0x40  // client 1 select (CS1) line low/high
#define SPI_CFG_CS2    0x80  // client 2 select (CS2) line low/high

void busINP()
{
  byte reg  = (PINC & 0x01) | ((PINB & 0x01)*2);   // read register number

  byte data;
  if( reg==I2C_REG_STATUS  )
    data = i2c_regStatus;
  else if( reg==I2C_REG_DATAIN )
    {
      if( (i2c_regStatus & I2C_ST_READING)!=0 && (i2c_regStatus & I2C_ST_BUSY)==0 )
        {
          data = i2c_buffer[i2c_bufPtr++];

          // reset I2C_ST_READING flag once we have transferred all the data
          if( i2c_bufPtr==i2c_count )
            i2c_regStatus &= ~I2C_ST_READING;
        }
      else
        data = 0xFF;
    }
  else // SPI register
    data = regOut[reg];

  PORTD = data;              // set output data
  DDRD = 0xFF;               // switch data bus pins to output
  PORTC &= ~0x02;            // set WAIT to LOW
  while( (PINB & 0x02)==0 ); // wait until INP signal ends
  DDRD = 0;                  // switch data bus pins to input
  PORTC |= 0x02;             // set WAIT back to HIGH
}


void busOUT()
{
  // read input data
  byte reg  = (PINC & 0x01) | ((PINB & 0x01)*2);   // read register number
  byte data = PIND;          // read input data

  if( reg==SPI_REG_COMMAND )
    {
      // bit 0-2: speed: 0=62.5k, 1=125k, 2=250k, 3=500k, 4=1M, 5=2M, 6=4M
      // bit 3  : clock phase (CPHA), 0=send data on trailing edge, 1=send data on leading edge
      // bit 4  : clock polarity (CPOL), 0=clock idles low, 1=clock idles high
      // bit 5  : 0=LSB first, 1=MSB first
      // bit 6  : chip 1 select (CS1) line status
      // bit 7  : chip 2 select (CS2) line status
      unsigned long speed = min((F_CPU/128) * (1<<(data&SPI_CFG_CLOCK)), F_CPU/2);
      bool first = (data & SPI_CFG_FIRST)!=0 ? MSBFIRST : LSBFIRST;
      byte mode;
      switch( (data&SPI_CFG_MODE)>>3 )
        {
        case 0: mode = SPI_MODE0; break;
        case 1: mode = SPI_MODE1; break;
        case 2: mode = SPI_MODE2; break;
        case 3: mode = SPI_MODE3; break;
        }
        
      SPI.beginTransaction(SPISettings(speed, first, mode));
      digitalWrite(PIN_SPI_CS1, (data&0x40)!=0);
      digitalWrite(PIN_SPI_CS2, (data&0x80)!=0);
    }
  else if( reg==SPI_REG_DATAOUT )
    regOut[SPI_REG_DATAIN] = SPI.transfer(data);
  else 
    {
      // I2C register
      if( (i2c_regStatus & I2C_ST_BUSY)==0 )
        {
          if( reg==I2C_REG_COMMAND )
            {
              i2c_regStatus = 0;
              i2c_addr = data & 0x7F;
              i2c_count = i2c_regDataIn;
              i2c_bufPtr = 0;

              if( i2c_count==0 )
                { /* nothing to do */ }
              else if( (data & 0x80)==0 )
                i2c_regStatus |= (I2C_ST_BUSY | I2C_ST_READING);
              else
                i2c_regStatus |= I2C_ST_WRITING;
            }
          else if( reg==I2C_REG_DATAOUT && (i2c_regStatus & I2C_ST_WRITING) && i2c_count>0 )
            {
              i2c_buffer[i2c_bufPtr++] = data;

              if( i2c_bufPtr==i2c_count )
                {
                  if( i2c_addr==0 )
                    {
                      // write to address 0 => set I2C config
                      // (assuming 8MHz CPU clock)
                      // 0=50k, 1=62.5k, 2=100k, 3=125k, 4=200k, 5=250k, 6=400k, 7=500k
                      byte div[8] = {160, 128, 80, 64, 40, 32, 20, 16};
                      twi_setFrequency(F_CPU / div[i2c_buffer[0]]);
                      i2c_regStatus &= ~I2C_ST_WRITING;
                    }
                  else 
                    {
                      // finished receiving data to send via I2C => send now (in loop())
                      i2c_regStatus |= I2C_ST_BUSY;
                    }
                }
            }
          else
            regIn[reg] = data;
        }
    }

  PORTC &= ~0x02;            // set WAIT to LOW
  while( (PINC & 0x04)==0 ); // wait until OUT signal ends
  PORTC |= 0x02;             // set WAIT back to HIGH
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


void setup() 
{
  // set all data bus pins to INPUT mode
  DDRD = 0;

  pinMode(PIN_ADDR0, INPUT);     // address bus bit 0
  pinMode(PIN_ADDR1, INPUT);     // address bus bit 0
  pinMode(PIN_WAIT,  OUTPUT);  // WAIT signal
  pinMode(PIN_INP,   INPUT);
  pinMode(PIN_OUT,   INPUT);
  
  // set WAIT output high
  digitalWrite(PIN_WAIT, HIGH);

  // set up pin change interrupts for OUT and INP
  PCICR  |= 0x03; // turn on pin change interrupts for port b+c
  PCMSK0 |= 0x02; // turn on pin PB1 (INP)
  PCMSK1 |= 0x04; // turn on pin PC2 (OUT)

  // initialize SPI and set parameters
  SPI.begin();
  digitalWrite(PIN_SPI_CS1, HIGH);
  digitalWrite(PIN_SPI_CS2, HIGH);
  pinMode(PIN_SPI_CS1, OUTPUT);
  pinMode(PIN_SPI_CS2, OUTPUT);

  // initialize I2C and set parameters
  twi_init();

  // 100ms I2C timeout is enough to transfer 256 bytes 
  // at the slowest frequency (50kHz)
  twi_setTimeoutInMicros(100000, true /* reset on timeout */);

  // initial I2C frequency
  twi_setFrequency(100000);
  i2c_regStatus = 0;
}




void loop() 
{
  if( i2c_regStatus & I2C_ST_BUSY )
    {
      if( i2c_regStatus & I2C_ST_READING )
        {
          // set error code, keep I2C_ST_READING flag set, the flag will
          // be reset once all data has been transferred over the bus
          if( twi_readFrom(i2c_addr, i2c_buffer, i2c_count, 1)==i2c_count )
            i2c_regStatus |= 0; // success
          else if( twi_manageTimeoutFlag(true) )
            i2c_regStatus |= 5; // timeout
          else
            i2c_regStatus |= 4; // unspecified error
        }
      else if( i2c_regStatus & I2C_ST_WRITING )
        {
          // Output   0 .. success
          //          1 .. length to long for buffer
          //          2 .. address send, NACK received
          //          3 .. data send, NACK received
          //          4 .. other twi error (lost bus arbitration, bus error, ..)
          //          5 .. timeout
          byte status = twi_writeTo(i2c_addr, i2c_buffer, i2c_count, 1, 1);
          if( twi_manageTimeoutFlag(true) )
            i2c_regStatus = 5; // timeout
          else
            i2c_regStatus = status;
        }

      // done talking over I2C => no longer busy
      i2c_regStatus &= ~I2C_ST_BUSY;
    }
}
