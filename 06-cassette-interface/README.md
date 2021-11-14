## Cassette interface (88-ACR)

This is an adaptation of my separate [ACRModem](https://github.com/dhansel/ACRModem) 
project that can plug directly into the I/O bus. Since it works on the I/O bus it
does not take up a serial port on the Altair 8800 simulator. Disable the emulated
88-ACR card in the simulator by setting it to "not mapped". Altair software generally
expects the ACR card at I/O address 06h/07h so address jumpers J1 and J2 should be "up"
and all others should be "down".

![Cassette interface](cassette.jpg)

Schematics and PCB as well as a Gerber file for PCB production are in this directory. 
The project is also available on EasyEDA: https://oshwlab.com/hansel72/2sio_copy_copy

### Programming the ATMega328P

The preferred method to program the ATMega328P chip is to use a
programmer such as the MiniPro TL866 or other that allows you to
directly program a .HEX file into the ATMega. Use the ACR.hex file
provided in this directory and the following fuse settings:
Low=0xFE, High=0xD7, Extended=0xFD

If you do not have a programmer you can use an Arduino UNO to program
the ATMega chip. First, burn a bootloader onto the ATMega following 
[these instructions](https://www.arduino.cc/en/Tutorial/BuiltInExamples/ArduinoToBreadboard)
but with a couple of changes:
- Use an 8MHz chrystal instead of 16MHz 
- After uploading the ArduinoISP sketch to the Arduino UNO and
  before burning the bootloader onto the ATMega set the board type to
  "Arduino Pro or Pro Mini" and processor to "ATMega328P (3.3V, 8MHz)"

Once you have the bootloader burned, load the ACR/ACR.ino sketch from
this repository into the Arduino IDE and upload it following the steps
in the "Uploading Using an Arduino Board" section in the same instructions
(using the same board/processor settings as shown above).
