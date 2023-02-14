## Example programs for SPI/I2C card

All examples here assume the I/O address for the card is 64.

MCP3002.BAS is a BASIC program demonstrating how to read data from a [MCP3002
analog-to-digital converter](https://www.farnell.com/datasheets/1599363.pdf) via SPI.

HTU21D.BAS is a BASIC program demonstrating how to read data from a [HTU21D
temperature/humidity sensor](https://www.digikey.com/htmldatasheets/production/1800203/0/0/1/HTU21D-F-Temperature-Humidity-Sensor.pdf) via I2C.

SSD1306.BAS is a BASIC program demonstrating how to display text on a [SSD1306
OLED display](https://www.digikey.com/en/products/detail/universal-solder-electronics-ltd/OLED%2520128X64%25200.96%2522%2520SPI/16822115) via I2C.

SSD1306.COM is a CP/M program displaying text on a SSD1306 OLED display. The user
can enter text line-by-line which is shown on the OLED. A '+' character will switch
to large (2x size) character set and a '-' character will switch back to small characters.

SSD1306.ASM is the assembly source code for SSD1306.COM
