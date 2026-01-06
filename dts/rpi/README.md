### BCM2711 - the Rpi4 Model B CPU has many SPI controllers, but most are not pinned out - hence not usable for a display LCD that is pinned/connected like a peripheral

#### on Raspberry Pi GPIO Header, only spi0 and spi1 are usable
#### HENCE WE LOCK SPI CONTROLLER = spi0

## TL;DR  :  There is an ST7735R LCD connected to SPI0, using CS0 line, and it uses three GPIOs for DC, RESET and BACKLIGHT, as available in the DT overlay `st7735r-spi-overlay.dts` , and this DT overlay adds a node for the same
