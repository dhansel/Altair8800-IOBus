## Floppy Disk Controller

This card replicates the function of the MITS 88-DCDD 8-inch 
and 88-MDS (Minidisk) 5.25-inch floppy disk controllers.

![Floppy Disk Controller Card](diskcontroller.jpg)

It can be connected to a 5.25-inch disk drive and either use
it in double-density mode to replicate the Minidisk system
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

When used as a 5.25" Minidisk system or with an 8" drive, the disk
format used by the controller matches the original formats, allowing 
the Altair Simulator to read original Altair disks.

The contoller supports up to two drives connected via a regular PC floppy
disk drive cable (with the [twist](https://www.nostalgianerd.com/why-are-floppy-cables-twisted)). Both drives should be configured as drive "B" as was
custom for PC drives. 

The 4 DIP switches on the card have the following functions:

DIP | Function when on         | Function when off
----|--------------------------|------------------
1   | Act as 88-MDS controller | Act as 88-DCDD controller
2   | Swap drives A and B      | Do not swap drives
3   | Drive B is Shugart SA-800| Drive B is generic 5.25-inch
4   | Drive A is Shugart SA-800| Drive A is generic 5.25-inch

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
