## Example programs for RTC/timekeeping card

All examples here assume the I/O address for the card is 96.

DATE.COM is a CP/M program to read and set the time on the card.
  - Called without arguments it will read and print the current time. 
  - Called as `DATE HH-MM-SS` it will set the current time (24-hour format).
  - Called as `DATE HH-MM-SS MM-DD-YY` it will set the current time and date.

DATE.ASM is the source for DATE.COM

DATE.BAS is a BASIC program that will read the current time and say it
using the SPO256 card.
