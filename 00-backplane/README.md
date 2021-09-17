# I/O Bus Backplane

The backplane provides four slots to plug in extension cards.
It also contains a 5V regulator. 

![IOBus Backplane](backplane.jpg)

The [Altair 8800 simulator](https://github.com/dhansel/Altair8800) 
is driven by an Arduino Due. The I/O bus
picks up all signals and voltages directly on the Arduino.

The bus carries the Arduino's 
+5V and +3.3V output voltages which can be used to power the
extension cards. However, some cards (e.g. the 88-PIO parallel 
port) can draw a significant current on the 5V line due to the
older-technology ICs used.

To prevent overloading the Arduino's on-board 5V regulator when
using multiple cards, the backplane has its own 5V regulator
which uses the input voltage switched by the AltairDuino's
power switch.

A jumper setting allows to switch the 5V supply for the cards
between the Arduino's on-board regulator and the 5V regulator
on the backplane. This is useful when the Arduino is powered
directly via the USB port, in which case no RAW voltage is
available and the 5V is directly taken from USB.

As implemented here, the backplane connects to the Arduino
via a DB-25 connector and a 25-wire ribbon cable.

Schematics and PCB as well as a Gerber file for PCB production are in this directory. 
The project is also available on EasyEDA: https://oshwlab.com/hansel72/backplane

The pinout of the DB-25 connector is shown here:

![IOBus pinout](IOBusPinout.png)

All signals for the I/O bus can be picked up directly on the
pins of the Arduino Due that drives the Altair 8800 simulator:

Bus Signal | Arduino pin | DB25 Pin
-----------|-------------|------------
D0         | D25         | 1     
D1         | D26         | 2     
D2         | D27         | 3     
D3         | D28         | 4     
D4         | D14         | 5     
D5         | D15         | 6     
D6         | D29         | 7     
D7         | D11         | 8     
A0         | D34         | 14    
A1         | D35         | 15    
A2         | D36         | 16    
A3         | D37         | 17    
A4         | D38         | 18    
A5         | D39         | 19    
A6         | D40         | 20    
A7         | D41         | 21    
OUT        | D6          | 9    
INP        | D8          | 10   
WAIT       | D10         | 11    
RESET      | D52         | 22    
CLR        | D53         | 23    
+3.3V      | +3.3V       | 12      
+5V        | +5V         | 25    
RAW voltage| RAW/Vin     | 13    
Ground     | GND         | 24    
