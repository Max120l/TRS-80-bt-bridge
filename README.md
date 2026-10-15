
# TRS-80 Model II/12/16 Bluetooth Bridge
A modern adapter that allows you to use any standard Bluetooth Low Energy (BLE) keyboard with vintage Tandy Model II/12/16 computers.


## Features
•	Zero-Install Web Flasher: Install the firmware directly from your browser via USB.

•	Built-in Web Configurator: The ESP32 hosts its own Wi-Fi setup page for easy configuration.

•	Device Scanning: Actively scans the room for Bluetooth keyboards in pairing mode.

•	Dynamic Key Mapping: Remap modern F-keys or Navigation keys to Tandy’s BREAK, HOLD, ESC, REPEAT, and LOCK functions.

## Hardware wiring and pinouts

You will need a standard ESP32 development board. Wire the ESP32 to your Tandy's keyboard port using the following pins (make sure you disconnect the USB cable before powering on the TRS-80 or, alternatively, disconnect the 5V connection from the ESP32):


|ESP32 Pin	| Signal	| Function		| Model II pin	| Model 12/16 pin	|
|:---		|:---		|:---			|:---		|:---           |		
GPIO 16		| CLOCK		| Clock pulse		| 4		| 2		
GPIO 17		| DATA		| Serial ASCII		| 1		| 1		
GND		| GND		| Common Ground		| 3		| 5		
5V / VIN	| 5V / VCC	| Powers the ESP32	| 5		| 4		


## Installation

Installation
1.	Plug the ESP32 into your computer via USB.
2.	Open Google Chrome or Microsoft Edge (Safari/Firefox do not support Web Serial).
3.	Visit the Web Installer Page: https://max120l.github.io/TRS-80-bt-bridge
4.	Click Connect, select your USB port, and click Install.

## Configuration

By default, the ESP32 boots in Run Mode, and actively attempts to connect to your saved Bluetooth keyboard. To change keyboard or remap keys, enter Setup Mode:
1.	Power on (or reset) the ESP32.
2.	Within the first 3 seconds of boot, press the physical BOOT button on the ESP32 board.
3.	The ESP32 will pause Bluetooth and start a Wi-Fi Access Point.
4.	On your phone or computer, connect to the new Wi-Fi network:
o	Network Name: Tandy Setup
o	Password: password123
5.	Open a web browser and navigate to http://192.168.4.1.
6.	Use the web interface to scan for your keyboard and assign the TRS-80 specific keys.
7.	Click Save & Reboot. The bridge will remember your settings permanently and reboot back into standard Bluetooth mode.

## Notes on TRS-80 Specific Keys

This bridge recreates several hardware-level Tandy functions in software:

•	Hardware Repeat: If you map a key to [REPEAT] in the web config, holding that key will repeat the last character you typed.

•	Shift Lock vs. Caps Lock: Standard Caps Lock only capitalizes letters. The Tandy [LOCK] key acts as a true Shift Lock, which forces the entire keyboard into a shifted state (capitalizing letters and turning numbers into symbols).

## Famicom Family BASIC Keyboard Mode

The bridge can also emulate the Nintendo Family BASIC keyboard (HVC-007) on the Famicom's 15-pin expansion port, so a BLE keyboard can be used with Family BASIC and other keyboard-aware Famicom software. Select **Target Machine: Famicom** in the Mappings tab of the web configurator.

### Wiring

The Famicom uses 5V logic and the ESP32 is **not** 5V tolerant, so you need level shifting:

•	Famicom → ESP32 (OUT0/OUT1/OUT2): a resistor divider on each line (e.g. 10kΩ series + 20kΩ to GND) or a level shifter.

•	ESP32 → Famicom (data lines): a 5V-powered 74AHCT125 / 74HCT245 buffer is recommended (a 3.3V signal is below the guaranteed HIGH level of 5V CMOS inputs).

|ESP32 Pin	| Signal		| Famicom expansion pin	|
|:---		|:---			|:---			|
GPIO 18		| OUT0 (row reset)	| 12			|
GPIO 19		| OUT1 (column select)	| 11			|
GPIO 23		| OUT2 (keyboard enable)| 10			|
GPIO 22		| $4017 D1		| 7			|
GPIO 25		| $4017 D2		| 6			|
GPIO 26		| $4017 D3		| 5			|
GPIO 27		| $4017 D4		| 4			|
GND		| GND			| 1			|
5V / VIN	| +5V			| 15			|

### Key mapping

Keys are mapped by **position**, as if your keyboard were the Japanese Family BASIC keyboard: letters, digits, F1–F8, arrows, Enter, Space and Esc map directly, and the punctuation keys map to the key in the same spot (`[`→`@`, `]`→`[`, `\`→`]`, `'`→`:`, `=`→`^`, `` ` ``→`¥`). Shift symbols therefore follow the Japanese layout (e.g. Shift+2 is `"`).

|Modern key		| Family BASIC key	|
|:---			|:---			|
Backspace / Delete	| DEL			|
Insert			| INS			|
Home			| CLR HOME		|
Ctrl			| CTR			|
Left Alt		| GRPH			|
Right Alt		| KANA			|
Pause or the [BREAK] key you configured | STOP	|

Caps Lock, Shift Lock and Repeat are Tandy-only features; the Famicom handles key repeat itself.
