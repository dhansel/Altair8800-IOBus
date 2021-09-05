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
