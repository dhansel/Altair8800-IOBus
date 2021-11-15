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
the ATMega chip:
1) Connect the Arduino UNO to your PC
2) Start the Arduino IDE
3) In the Arduino IDE, select File->Examples->ArduinoISP
4) Select Sketch->Upload
5) Wire the ATMega chip to the Arduino UNO as shown [in this diagram](doc/BreadboardAVR.png)
6) In the Arduino IDE, load the ACR/ACR.ino file from this repository
7) Select Tools->Board and set it to "Arduino Pro or Pro Mini"
8) Select Tools->Processor and set it to "ATMega328P (3.3V, 8MHz)"
9) Select Tools->Burn Bootloader (this will program the correct fuse settings)
10) Select Sketch->"Upload using Programmer" (this will upload the actual program)
