# SmokeMachineTimer

A PlatformIO/Arduino project for Seeed XIAO ESP32-C3 with SSD1315 128x64 OLED and 4 buttons.

## Features
- On/Off timer (0000.01...9999.99 sec) for relay output
- Editable On/Off times (digit-by-digit, inverted digit in edit mode)
- 4 buttons: Up, Down, # (edit/next/reset), * (manual output)
- SSD1315 128x64 OLED (I2C)
- EEPROM save/load for timer values

## Pin Mapping
| Function      | Pin   |
|--------------|-------|
| Relay Output  | D0 / GPIO2  |
| Up Button     | D1 / GPIO3  |
| Down Button   | D2 / GPIO4  |
| # Button      | D9 / GPIO9  |
| * Button      | D10 / GPIO10|
| OLED SDA      | D4 / GPIO6  |
| OLED SCL      | D5 / GPIO7  |

## Usage
- Use Up/Down to enter edit mode and change timer digits
- # cycles to next digit (after all, exits edit mode)
- * enables relay output as long as held
- # in run mode resets timer
- Timer values are saved to EEPROM

## Libraries
- Adafruit SSD1306
- Adafruit GFX
- EEPROM

## License
MIT
