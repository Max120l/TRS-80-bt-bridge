# C64 keyboard on a stock NES (ESP32 adapter)

An ESP32 presents a real Commodore 64 keyboard matrix to the NES through one
ordinary controller port. No Famicom keyboard and no expansion port are
needed. The first adapter reads a BLE keyboard. A later one can scan a real
C64 keyboard with the same ESP32. Both send the same packet, so the ROM does
not care which one is plugged in.

This is the adapter side of the NES 64 ROM's `nes64_c64kb.nes` build. The
firmware lives in `src/mod12_keyb.ino` as the "NES 64" target of the bridge.

## C64 matrix

Same layout as the C64's CIA 1 scan. Row N is the byte read with column
select line PA N low. Bit M is port B line PB M. 0 means pressed.

| Row | Bit 0 | Bit 1 | Bit 2 | Bit 3 | Bit 4 | Bit 5 | Bit 6 | Bit 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | INST/DEL | RETURN | CRSR Right | F7 | F1 | F3 | F5 | CRSR Down |
| 1 | 3 | W | A | 4 | Z | S | E | Left Shift |
| 2 | 5 | R | D | 6 | C | F | T | X |
| 3 | 7 | Y | G | 8 | B | H | U | V |
| 4 | 9 | I | J | 0 | M | K | O | N |
| 5 | + | P | L | - | . | : | @ | , |
| 6 | Pound | * | ; | CLR/HOME | Right Shift | = | Up Arrow | / |
| 7 | 1 | Left Arrow | CTRL | 2 | Space | C= | Q | RUN/STOP |

SHIFT LOCK is a latching switch wired in parallel with Left Shift, so it has
no matrix position of its own. RESTORE is not in the matrix. On a C64 it
drives the NMI. Here it travels as a separate bit.

## Link: one standard controller port

Port 2 is used, so a normal controller stays in port 1.

| NES pin | Signal | Use | ESP32 |
|---|---|---|---|
| 1 | GND | Ground | GND |
| 2 | CLK | Pulses low once per `$4017` read. Shift clock | GPIO 19 (via divider) |
| 3 | OUT0 | `$4016` bit 0, shared by both ports. Latch / frame start | GPIO 18 (via divider) |
| 4 | D0 | Serial data to the NES (`$4017` bit 0) | GPIO 22 (via 74AHCT125) |
| 7 | +5V | Powers the adapter | VIN / 5V |

Pin numbers follow the NESdev wiki. Other sources number the pins in a
different order. Check with a meter on a real cable before soldering.

The NES input buffer inverts D0. The adapter drives each bit complemented so
the ROM sees the logical value without an extra `EOR`.

### Packet

The ROM pulses OUT0 high then low, then clocks 88 bits, MSB first:

| Bytes | Content |
|---|---|
| 0-1 | Signature `$A5 $5A` |
| 2-9 | C64 matrix rows 0-7, 0 = pressed |
| 10 | Bit 0 RESTORE, 0 = pressed. Bits 1-7 reserved, send 1 |

After bit 88 the adapter sends 1s, like a stock pad after its 8 buttons. A
standard pad's second byte is therefore always `$FF` and never `$5A`. With
nothing plugged in every bit reads 0. Either case means "no adapter".

Note: `$A5` and `$5A` read the same in either bit order, so the signature
cannot catch a bit order mismatch. The firmware sends MSB first, matching the
AVR SPI default of the original design; the ROM must shift bits in with
`ROL` (first bit read ends up in bit 7).

## Adapter hardware (ESP32)

- Any ESP32 dev board (ESP32-WROOM, 3.3 V). The NES is 5 V, so:
  - OUT0 and CLK into the ESP32 through a divider (10k series, 20k to GND)
    or a 3.3 V-powered 74LVC125, whose inputs tolerate 5 V.
  - D0 out through a 5 V-powered 74AHCT125 (TTL-level inputs, so 3.3 V
    reads as high). Tie its output enable low.
- The input is a BLE keyboard. The USB port is for flashing and the serial
  log only.
- Power from NES pin 7. The ESP32 with Bluetooth active draws roughly
  100-250 mA, more than a pad. Measure before trusting the port with it.
  Do not plug USB in while the adapter is on the NES, or cut the 5 V lead
  from the NES first.

## Adapter firmware

Instead of SPI slave hardware (as on the AVR), the ESP32 plays the 4021
shift register from two GPIO interrupts pinned to core 1, away from the
Bluetooth stack on core 0:

1. BLE HID reports arrive from the keyboard (8-byte boot-style report:
   modifiers, reserved, 6 key codes). A report full of `$01` (rollover) is
   ignored.
2. Track which PC keys are down, in press order.
3. After every report, rebuild the 9-byte C64 state (8 rows plus the
   RESTORE byte) from the PC keys that are down, using the layout below.
   Caps Lock toggles SHIFT LOCK, which holds C64 Left Shift down.
4. Build the state into a spare buffer and swap pointers. The latch
   interrupt copies the current buffer, so a packet never mixes two key
   states.
5. On OUT0 rising: copy the packet, reset the bit index, drive bit 0.
6. On each CLK rising edge with OUT0 low: advance the bit index and drive
   the next bit.
7. On Bluetooth disconnect, release every key so nothing stays stuck.

### Timing

The NES clocks the bits. A tight read loop takes about 16 CPU cycles, about
9 us, per bit. The CLK low pulse lasts only for the read cycle, and the next
bit is needed before the next read. The ESP32 GPIO interrupt responds in
about 2-3 us, so it has a comfortable margin per bit, and no per-byte
reload is needed. The ROM's ~10 us pause between bytes (needed by the AVR
SPI design) is harmless but not required by this adapter. The 20 us latch
hold is plenty for the packet copy.

Nothing else runs on core 1 with interrupts disabled while in NES 64 mode
(no flash writes, no Tandy bit-banging). If reads still go bad at the bench,
the next step is moving the shift loop to a dedicated busy-polling task on
core 1.

### Symbolic layout

Every symbol stays where the PC keycap prints it. Shift+2 types `@`, Shift+;
types `:`, and `'` types an apostrophe, even though the C64 puts those on
other keys. The firmware fakes the C64 Shift state to make that work:

- A PC key whose C64 character needs Shift sets C64 Left Shift in the
  matrix while it is down, even if no PC Shift is held. `'` sends C64
  Shift+7.
- A PC key whose C64 character must not have Shift clears both C64 Shifts
  while it is down, even with PC Shift held. Shift+2 sends C64 `@` alone.
- Letters and keys that agree on both machines pass PC Shift through
  unchanged, so Shift+letter still gives the C64 graphics.
- When two keys with conflicting Shift needs are down, the most recent key
  wins.
- While C= or CTRL is held, the firmware maps positionally, with no Shift
  faking, so every C= and CTRL graphic on the symbol keys can be reached.

| PC key | Unshifted -> C64 | Shifted -> C64 | Positional (C=/CTRL held) |
| --- | --- | --- | --- |
| 1 | 1 | `!` Shift+1 | 1 |
| 2 | 2 | `@` @ | 2 |
| 3 | 3 | `#` Shift+3 | 3 |
| 4 | 4 | `$` Shift+4 | 4 |
| 5 | 5 | `%` Shift+5 | 5 |
| 6 | 6 | `^` Up Arrow | 6 |
| 7 | 7 | `&` Shift+6 | 7 |
| 8 | 8 | `*` * | 8 |
| 9 | 9 | `(` Shift+8 | 9 |
| 0 | 0 | `)` Shift+9 | 0 |
| - | - | `_` Left Arrow | + |
| = | = | `+` + | - |
| [ | Shift+: | Shift+: | @ |
| ] | Shift+; | Shift+; | * |
| \ | Pound | Shift+Pound | Pound |
| ; | ; | `:` : | : |
| ' | Shift+7 | `"` Shift+2 | ; |
| , . / | , . / | `<` Shift+, `>` Shift+. `?` Shift+/ | , . / |
| ` | Left Arrow | Shift+Left Arrow | Left Arrow |

The positional column follows the physical C64 layout (`+ -` right of `0`,
`@ *` right of `P`, `: ;` right of `L`), which puts every key that carries
C= graphics within reach.

Special keys:

| PC key | C64 key |
| --- | --- |
| Esc | RUN/STOP |
| Page Up | RESTORE |
| Left Ctrl | C= |
| Tab | CTRL |
| Caps Lock | SHIFT LOCK, toggles a held Left Shift |
| Backspace | INST/DEL |
| Insert | Shift+INST/DEL |
| Home | CLR/HOME |
| Up, Left | Shift+CRSR Down, Shift+CRSR Right |
| Down, Right | CRSR Down, CRSR Right |
| F1, F3, F5, F7 | F1, F3, F5, F7 |
| F2, F4, F6, F8 | Shift+F1, Shift+F3, Shift+F5, Shift+F7 |
| Enter, Keypad Enter | RETURN |

The special key picks are a starting point and live in one table
(`C64_KEYMAP`) in the firmware.

## Real C64 keyboard adapter (ESP32, later)

The same ESP32 can scan a real C64 keyboard in place of the Arduino Mega: 8
column outputs, 8 row inputs with pull-ups, and RESTORE on its own input.
The keyboard is passive switches, so it runs fine at 3.3 V. It builds the
same 9 bytes and sends the same packet over the same three NES lines. No
symbolic layout, the keyboard is already a C64 keyboard.

Suggested pins, leaving 18/19/22 for the NES link and avoiding the flash
and strapping pins where possible:

| Function | ESP32 GPIO |
|---|---|
| Columns PA0-PA7 (outputs, open drain) | 4, 13, 14, 16, 17, 21, 23, 25 |
| Rows PB0-PB7 (inputs) | 26, 27, 32, 33, 34, 35, 36, 39 |
| RESTORE (input, pull-up) | 15 |

GPIO 34-39 are input-only and have no internal pull-ups, so rows PB4-PB7
need external 10k pull-ups to 3.3 V. GPIO 15 is a strapping pin; a pull-up
keeps it in its normal boot state.

## Phases

0. ROM reaches READY on the EverDrive N8 clone. Done on the ROM side.
1. C64 keyboard build plus a probe ROM that prints the raw 88 bits per
   frame, tested in Mesen 2 first.
2. BLE keyboard on the bench. The packet and layout logic have been checked
   with a host-side simulation of the NES reads; a serial debug print of
   the matrix is not in the firmware yet.
3. Wiring to a controller extension cable, with level shifting.
4. Bring-up with the probe ROM, then NES 64.
5. Real C64 keyboard adapter on the ESP32.
