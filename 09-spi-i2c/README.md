## SPI and I2C communication card

This card allows the Altair 8800 Simulator to communicate with I2C and SPI devices
using IN/OUT operations in BASIC or assembly language.

![SPI and I2C card](spi_i2c.jpg)

Schematics and PCB as well as a Gerber file for PCB production are in this directory. 
The project is also [available on EasyEDA](https://oshwlab.com/hansel72/2sio_copy_copy_copy).

### Card registers

The card provides several registers (accessible via IN/OUT instructions) to facilitate
I2C/SPI communication. The card's base address BA is set via the jumpers on its top left corner.

The registers provided are:
  - BA+0 (write): SPI control register
  - BA+1 (read and write): SPI data register
  - BA+2 (write): I2C command register
  - BA+2 (read): I2C status register
  - BA+3 (read and write): I2C data register

The following two sections describe how these registers are used to initiate communication.

### SPI communication

SPI communication happens one byte at a time. Data is always sent and received at the same time.
A transfer is initiated by writing a data byte to the SPI data register. The Altair stops until 
the byte transfer is finished. The received byte can then be read by reading the SPI data register.

The card offers two SPI connectors. Both are controlled by the same register. Use the CS signals
(bits 6+7 of the control register) to select which connected device should be addressed

The SPI protocol offers a number of configuration options which can be set by writing to the
SPI control register. The control register bits have the following function:

  - bit 0-2: SPI clock frequency: 0=62.5kHz, 1=125kHz, 2=250kHz, 3=500kHz, 4=1MHz, 5=2MHz, 6=4MHz
  - bit 3  : clock phase (CPHA), 0=send data on trailing edge, 1=send data on leading edge
  - bit 4  : clock polarity (CPOL), 0=clock idles low, 1=clock idles high
  - bit 5  : 0=LSB first, 1=MSB first
  - bit 6  : chip 1 select (CS on SPI port 1) line status
  - bit 7  : chip 2 select (CS on SPI port 2) line status

Bits 6 and 7 control the voltage on the the "CS" lines for the corresponding SPI connectors
on the card. Setting the bit to "1" will output 3.3V, setting it to "0" will output 0V.

To send and receive one byte via SPI, use the following BASIC code (assuming the card's base address is 64):
```
OUT 64,39:REM 4MHz clock, MSB first, mode 0
OUT 65,85:REM send "85" 
A=INP(65):REM read data that was sent by the device
```

See also the [MCP3002.BAS](programs/MCP3002.BAS) example in the "programs" folder.

### I2C communication

I2C communication is transaction based. Each transaction consists of three steps:
  - set the number of data bytes to be transferred (1-255)
  - set the device address and operation (read/write)
  - read or write the transation data.

To send data to a device:
  - Write the number of bytes to transfer (N) into the I2C data register
  - Write the device's 7-bit I2C address PLUS 128 to the I2C command register.
  - Write N bytes of data to the I2C data register
  - Wait until bit 7 of the I2C status register is 0
  - Read the I2C status register. If it is not 0 then an I2C error has occurred. Bits 0-4 contain the error code.

To read data from a device:
  - Write the number of bytes to transfer (N) into the I2C data register
  - Write the device's 7-bit I2C address to the I2C command register.
  - Wait until bit 7 of the I2C status register is 0
  - Read the I2C status register, if bit 5 is NOT set (i.e. the card is not waiting for the Altair to read the received data) then an I2C error has occurred. Bits 0-4 contain the error code.
  - Read N bytes of data from the I2C data register.

I2C command register (write-only):
  - bit 7: 1=write operation (send data to device), 0=read operation (receive data from device)
  - bits 6-0: I2C device address to communicate with

I2C status register (read-only):
  - bit 7: If set then the card is currently communicating with the device. No other operation can be performed until communication has finished.
  - bit 6: If set then the card is waiting for the Altair to write data to the I2C data register.
  - bit 5: If set then the card is waiting for the Altair to read data from the I2C data register.
  - bits 4-0: I2C status code (after a I2C transaction has finished):
     - 0: success
     - 2: address send, NACK received
     - 3: data send, NACK received
     - 4: other I2C error (lost bus arbitration, bus error, ..)
     - 5: timeout

Device address 0 can be used to configure the I2C clock speed used by the card:
  - Write 1 to the I2C data register (sending 1 byte of configuration data)
  - Write 128 to the I2C command register (device address 0, writing data)
  - Write one byte specifying the clockHz speed to the I2C data register: 0=50kHz, 1=62.5kHz, 2=100kHz, 3=125kHz, 4=200kHz, 5=250kHz, 6=400kHz, 7=500kHz
The clock speed defaults to 100k if not set explicitly.

The following code demonstrates how to send 3 bytes of data (50, 100, 150)
to a device with I2C address 34 (and the SPI/I2C card's base I/O address is 64):

```
10 OUT 67,3:OUT 66,34+128
20 OUT 67,50:OUT 67,100:OUT 67,150
20 IF INP(66) AND 128 THEN 20
30 IF INP(66)<>0 THEN PRINT "I2C WRITE ERROR:";INP(66) AND 31:END
```

The following code demonstrates how to read ten bytes of data from a device
with I2C address 34 (and the SPI/I2C card's base I/O address is 64):

```
10 OUT 67,10:OUT 66,34
20 IF INP(66) AND 128 THEN 20
30 IF INP(66)<>32 THEN PRINT "I2C READ ERROR:";INP(66) AND 31:END
40 FOR i=1 TO 10:PRINT INP(67):NEXT I
```

See also the [HTU21D.BAS](programs/HTU21D.BAS) and [SSD1306.BAS](programs/SSD1306.BAS) examples in the "programs" folder.

### Programming the ATMega328P

After building the card you need to program its ATMega328P chip. The preferred method 
is to use a programmer such as the MiniPro TL866 or other that allows you to
directly program a .HEX file into the ATMega. Use the [SPI_I2C.hex](SPI_I2C.hex) file
provided in this directory. **Important:** Program the fuse settings
("Config" button in the TL866 interface) to
Fuse Low=0xFE, Fuse High=0xD7, Extended Fuse Byte=0xFD

If you do not have a programmer you can use an Arduino UNO to program
the ATMega chip:
1) Connect the Arduino UNO to your PC
2) Start the Arduino IDE
3) In the Arduino IDE, select File->Examples->ArduinoISP
4) Select Tools->Board and set it to "Arduino UNO"
5) Select Tools->Port and select the serial port under which your Arduino UNO shows up
6) Select Sketch->Upload
7) Wire the ATMega chip to the Arduino UNO as shown [in this diagram](https://github.com/dhansel/Altair8800-IOBus/blob/main/06-cassette-interface/doc/BreadboardAVR.png)
8) In the Arduino IDE, load the [SPI_I2C.ino](SPI_I2C/SPI_I2C.ino) file from this repository
9) Select Tools->Board and set it to "Arduino Pro or Pro Mini"
10) Select Tools->Processor and set it to "ATMega328P (3.3V, 8MHz)"
11) Select Tools->Programmer and set it to "Arduino as ISP" (**not** ArduinoISP!)
12) Select Tools->Burn Bootloader (this will program the correct fuse settings)
13) Select Sketch->"Upload using Programmer" (**not** Sketch->Upload)
