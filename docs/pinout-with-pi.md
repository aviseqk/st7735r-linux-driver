## How did I choose CS and GPIO Pins for my ST7735R LCD Display

### LCD Pins : LED, SCK, SDA, A0, RESET, CS, GND, VCC

### GPIO Pins <-> LCD SPI Mapping

`
LED     Backlight       GPIO or Regulator
SCK     SPI Clock       SPI SCLK
SDA     SPI Data        SPI MOSI
A0      Data/Command    GPIO(DC)
RESET   Reset           GPIO
CS      Chip Select     SPI CS
GND     Ground          Ground
VCC     Power           3.3V
`

NOTE: 
a) SDA != I2C Pin, it's SPI MOSI here
b) SPI's MISO not present on Display Pins, as this is a write-only device (very common in write only devices)

#### Part1: Mapping for SPI pins for SPI Signals

-- SPI0 is chosen for this, so it's pin mapped against device are:
-> MOSI     -       GPIO10 (SPI0 MOSI)
-> MISO     -       GPIO9  (SPI0 MISO)
-> SCLK     -       GPIO11 (SPI0 SCLK)
-> CS0      -       GPIO8  (SPI0 CE0)
-> CS1      -       GPIO9  (SPI0 CE1)

##### we have two CS lines here, but the decision is simple. since we have one SPI device and it'll be the first child, `we use CS0`

so, LCD's CS pin connected to GPIO8 (SPI0 CE0)

#### Part2: Mapping for GPIO Pins for non-SPI Signals

-- since some of the control and device's electrical and other requirements are not SPI signals, and are hence provided by GPIO pins that are not specific to SPI, so

-> A0(DC)   -       GPIO25      -   free
-> RESET    -       GPIO24      -   adjacent, hence convenient
-> LED      -       GPIO18      -   can do PMW later (for backlight)
