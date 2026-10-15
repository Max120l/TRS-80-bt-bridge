
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

## NES 64 Mode (C64 keyboard on NES controller port 2)

The bridge can act as the keyboard adapter for the NES 64 ROM (`nes64_c64kb.nes`): it presents a Commodore 64 keyboard matrix to a stock NES through controller port 2, using your BLE keyboard. A normal controller stays in port 1. Select **Target Machine: NES 64** in the Mappings tab of the web configurator. Full protocol and design notes are in [docs/NES64_ADAPTER.md](docs/NES64_ADAPTER.md).

### Wiring

The NES uses 5V logic and the ESP32 is **not** 5V tolerant:

•	NES → ESP32 (OUT0, CLK): a resistor divider on each line (e.g. 10kΩ series + 20kΩ to GND), or a 3.3V-powered 74LVC125 (5V-tolerant inputs).

•	ESP32 → NES (D0): a 5V-powered 74AHCT125 buffer (non-inverting).

|ESP32 Pin	| Signal			| NES controller port 2 pin	|
|:---		|:---				|:---				|
GPIO 18		| OUT0 (latch)			| 3				|
GPIO 19		| CLK				| 2				|
GPIO 22		| D0 (data to NES)		| 4				|
GND		| GND				| 1				|
5V / VIN	| +5V				| 7				|

Pin numbers follow the NESdev wiki; other sources number them differently. Check with a meter on a real extension cable before soldering. The ESP32 draws more than a pad (up to ~250 mA with Bluetooth active), so don't plug USB in while it's on the NES unless you cut the 5V lead.

### Key mapping

Symbols stay where the PC keycap prints them: Shift+2 types `@`, Shift+; types `:`, `'` types an apostrophe. The bridge fakes the C64 Shift state to make that work. Letters pass Shift through, so Shift+letter still gives the C64 graphics. While C= (Left Ctrl) or CTRL (Tab) is held, symbol keys map by position with no Shift faking, so every C= and CTRL graphic can be reached.

|PC key			| C64 key			|
|:---			|:---				|
Esc			| RUN/STOP			|
Page Up			| RESTORE			|
Left Ctrl		| C=				|
Tab			| CTRL				|
Caps Lock		| SHIFT LOCK (toggle)		|
Backspace / Insert	| INST/DEL / Shift+INST/DEL	|
Home			| CLR/HOME			|
Arrows			| CRSR keys (Up/Left add Shift)	|
F1–F8			| F1/F3/F5/F7, even keys add Shift |
`\`			| £				|
`` ` ``			| ←				|
