# Floppy Disk Controller

 * Using the MITS firmware, this card replicates the function of the 
MITS 88-DCDD 8-inch and 88-MDS (Minidisk) 5.25-inch floppy disk controllers.
 * Using the ICOM firmware, this card replicates the function of the 
ICOM3712 and ICOM3812 floppy disk controllers.

![Floppy Disk Controller Card](diskcontroller.jpg)

The card can be connected to a 5.25-inch disk drive and either use
it in double-density mode to replicate the MITS Minidisk system
or in high-density mode to emulate an 8-inch system. Note
that a 5.25-inch disk in high density mode can hold the 
same amount of data as 8-inch single density disks.
I am using the card with a [Teac FD-55GFR](https://www.vogonswiki.com/index.php/Teac_FD-55GFR)

Since the interface for 3.5-inch drives is identical to the
5.25-inch drives the card also works with 3.5-inch drives.
I have successfully used the card with a [Teac FD-235hg](https://www.cnet.com/products/teac-floppy-drive-fd-235hg-floppy-disk-drive-floppy-series) 3.5" drive.

Additionally the card supports [Shugart SA-800](https://github.com/dhansel/Altair8800-IOBus/blob/master/04-disk-controller/doc/Shugart_SA800_Brochure_Feb78.pdf) 8-inch drives.
It may work with similar drives but I only have tried it with the SA-800. To connect the 34-pin floppy cable to the to the SA-800's
50-pin connector use the adapter from [this folder](https://github.com/dhansel/Altair8800-IOBus/tree/master/04-disk-controller/Shugart50to34adapter)
and strap the drive as drive 0. Refer to the [SA-800 user manual](https://github.com/dhansel/Altair8800-IOBus/blob/master/04-disk-controller/doc/SA800%20OEM%20Manual.pdf) about user-configurable options (straps).

The contoller supports up to two drives connected via a regular PC floppy
disk drive cable (with the [twist](https://www.nostalgianerd.com/why-are-floppy-cables-twisted)). 
Both drives should be configured as drive "B" as was custom for PC drives. 

## MITS firmware

The MITS boot ROM and software expect the disk drive at I/O address 08h-0Ah
so jumper J3 should be set "up", all other address jumpers should be set "down".

When used as a 5.25" Minidisk system or with an 8" drive, the disk
format used by the controller matches the original formats, allowing 
the Altair Simulator to read original Altair disks.

The original MITS controllers required hard-sectored disks to work which
are not easy to come by and fairly expensive these days.
This controller can work with both hard-sectored and soft-sectored disks,
provided that the disk drive used has a stable rotation rate. The disk
format used by MITS packs sectors very tightly so if the rotation rate varies
too much then the single index hole of soft-sectored disks is not enough
to keep controller and disk synchronized, causing the controller to miss 
the the proper time to start reading a sector.

In general belt-driven disk drives will have problems whereas direct-drive drives
should be fine. That said I have used soft-sectored disks on the belt-driven
SA-800 and it worked for the most part (with occasional read errors). 
Hard-sectored disks in the SA-800 and soft-sectored disks the Teac 55-GFR work
without problems.

The 4 DIP switches on the card have the following functions:

DIP | Function when on         | Function when off
----|--------------------------|------------------
1   | Act as 88-MDS controller | Act as 88-DCDD controller
2   | Swap drives A and B      | Do not swap drives
3   | Drive B is Shugart SA-800| Drive B is generic 5.25-inch
4   | Drive A is Shugart SA-800| Drive A is generic 5.25-inch

## ICOM firmware

The ICOM firmware emulates the ICOM3712 and ICOM3812 controllers including
the parallel cards usually used to connect the controllers. 

The ICOM boot ROM and software expect the card at I/O address C0h-C1h so jumpers J7
and J6 should be set "up", the other address jumpers should be set "down".

ICOM boot ROMs for the 3712 and 3812 controllers as well as some disk images 
are available on Mike Douglas' site at
https://deramp.com/downloads/altair/software/icom_floppy.

DIP | Function when on           | Function when off
----|----------------------------|------------------
1   | Act as ICOM3812 controller | Act as ICOM3712 controller
2   | Swap drives A and B        | Do not swap drives
3   | Drive B is Shugart SA-800  | Drive B is generic 5.25-inch
4   | Drive A is Shugart SA-800  | Drive A is generic 5.25-inch

## Common

The serial port present on the card can be connected to a PC via an FTDI
USB-to-serial converter and has two functions:
1. Upload the firmware to the ATmega328P processor on the card. Load the 
firmware into the Arduino programming environment, select the
"Arduino Pro or Pro Mini" board at 16MHz and upload the software.
2. Provide a monitor to interact directly with the disk drive. To enter
the monitor, connect a terminal to the serial port at 115200 baud, then press and
hold the MONITOR button on the board and briefly press RESET. Hold the
MONITOR button until the monitor prompt can be seen on the terminal.
Enter 'h' at the monitor prompt for information about valid commands.

Schematics and PCB as well as a Gerber file for PCB production are in this directory. 
The project is also available on EasyEDA: https://oshwlab.com/hansel72/diskcontrolleruno

## Changes from previous PCB version

In order to support the ICOM firmware, a small change had to be made to the 
pinout of the ATMega328p:

 * The WRITEDATA signal was moved from ATMega pin 15 to pin 17
 * The INDEX signal was moved from ATMega pin 17 to pin 19
 * The WAIT sinal was moved from ATMega pin 19 to pin 15

If you have a V1.PCB you will need to cut/add connections to 
account for these changes in order to use the current firmware. The V1.0 board
has no markings on the PCB, the newer V1.1 version has a V1.1 marking on the bottom left.

Note that nothing in the MITS firmware (other than the pin assignments) was changed
so you can stick with the old board layout and MITS firmware unless you want to also
use the new ICOM firmware.
