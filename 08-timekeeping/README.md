## Timekeeping card

This card adds battery-backed Real-Time-Clock/timekeeping capabilities via the [DS1302](doc/DS1302.pdf) chip.

The DS1302 is a widely used chip which provides the current time and date in
several easy-to-read registers. The chip itself is a small 8-pin package intended
for use with modern microcontrollers that can easily implement the custom serial
protocol used by the chip. Accessing the data through the byte-based (parallel)
architecture of the I/O bus is a bit more tricky. As a challenge to myself I decided
to use only discrete ICs and no microcontroller on the card. This also has the
advantage of not needing to program an ATMega or similar in order to make the card.
The downside is a larger chip count to build it.

![Timekeeping card](clock.jpg)

### Interacting with the DS1302

The Altair can communicate with the DS1302 chip via regular IN/OUT commands, either 
from BASIC or assembly language. First, choose an I/O address for the card by configuring
the J1-J7 jumpers. I have mine set to address 96.

The card uses two registers, the command/status register (at the base I/O address) and
the data register (at base I/O address +1). To write a byte of data to a register on
the DS1302 chip do the following:

1) Put the data to be written into the data register (e.g. OUT 97, data)
2) Issue a "write" command in the command register (e.g. OUT 96, 128)

To read a byte of data from a DS1302 register do the following:

1) Issue a "read" command in the command register (e.g. OUT 96, 129)
2) Read the status register (e.g. INP(96)). Bit 7 is 1 while the card is busy reading from the DS1302 and will be 0 when reading has finished.
This step is only necessary when using assembly language as the read process if very fast.
3) Read the data from the data register (e.g. INP(97))

Table 3 from the [DS1302 datasheet](doc/DS1302.pdf), reproduced here, shows the DS1302
registers associated with timekeeping:
![DS1302 registers](doc/registers.png)
To issue a "read" command for a register, write the value in the table's READ
column to the card's command register. To issue a "write" command, use the value
in the WRITE column of the table. For example, the BASIC commands
`OUT 96,131:PRINT INP(97)`
will read the minutes register. 
`OUT 97,5:OUT 96,128`
will set the seconds register to 5.





