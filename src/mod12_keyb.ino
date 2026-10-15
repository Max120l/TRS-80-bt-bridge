#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h> 
#include <WiFi.h>          
#include <WebServer.h>     
#include "soc/gpio_reg.h"

// --- TANDY HARDWARE PINS ---
const int CLOCK_PIN = 16; 
const int DATA_PIN = 17;
const int BUSY_PIN = 21;  
const int BOOT_BUTTON = 0; 

// --- NES CONTROLLER PORT 2 PINS (NES 64 C64 keyboard adapter) ---
// The NES is 5V: level-shift both inputs and buffer the output!
const int NES_LATCH = 18; // Port pin 3: OUT0, latch / frame start
const int NES_CLK   = 19; // Port pin 2: CLK, pulses low once per $4017 read
const int NES_DATA  = 22; // Port pin 4: D0, serial data to the NES

// --- TARGET MACHINE ---
const uint8_t TARGET_TANDY   = 0;
const uint8_t TARGET_NES64   = 1;
uint8_t targetMachine = TARGET_TANDY;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Preferences preferences; 
WebServer server(80);      

// --- BLE & STATE VARIABLES ---
static const NimBLEAdvertisedDevice* advDevice;
static bool doConnect = false;
static bool connected = false;
bool inConfigMode = false; 
std::string targetMAC = ""; 

// --- DYNAMIC KEY MAP VARIABLES ---
uint8_t key_break  = 0x42; // Default: F9
uint8_t key_hold   = 0x43; // Default: F10
uint8_t key_esc    = 0x29; // Default: ESC
uint8_t key_repeat = 0x00; // Default: Disabled
uint8_t key_lock   = 0x47; // Default: Scroll Lock

// --- STATE MEMORY ---
bool defaultCapsOn = false; 
bool capsLockState = false;  
bool shiftLockState = false; 

// --- HARDWARE REPEAT SYSTEM ---
uint8_t last_valid_tandy_char = 0x00; 
bool isRepeatKeyHeld = false;
unsigned long lastRepeatTime = 0;

// =========================================================================
// WEB CONFIGURATOR
// =========================================================================
const char webpageHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Tandy 12 Bridge Setup</title>
    <style>
        body { font-family: 'Courier New', Courier, monospace; background-color: #808080; color: #000000; text-align: center; padding: 20px 10px; }
        .card { background: #d3d3d3; max-width: 480px; margin: 0 auto; padding: 20px; border: 3px solid #000; box-shadow: 6px 6px 0px #000; }
        h1 { color: #000; font-size: 24px; margin-bottom: 5px; text-transform: uppercase; letter-spacing: 2px; }
        p { font-size: 14px; color: #000; margin-bottom: 20px; font-weight: bold; }
        
        .tab-bar { display: flex; justify-content: center; margin-bottom: 20px; border-bottom: 3px solid #000; }
        .tab-btn { background: none; border: none; padding: 10px 20px; font-size: 16px; cursor: pointer; color: #555; font-weight: bold; transition: 0.2s; font-family: 'Courier New', Courier, monospace; text-transform: uppercase; }
        .tab-btn:hover { color: #000; }
        .tab-btn.active { color: #000; border-bottom: 4px solid #000; }
        .tab-content { display: none; text-align: left; }
        .tab-content.active { display: block; }
        
        label { font-weight: bold; font-size: 14px; display: block; margin-top: 15px; color: #000; text-transform: uppercase; }
        input[type=text], select { width: 100%; padding: 10px; margin: 5px 0 10px 0; border: 2px solid #000; font-size: 16px; box-sizing: border-box; font-family: 'Courier New', Courier, monospace; background: #fff; color: #000; }
        
        button, input[type=submit] { background-color: #000; color: #fff; padding: 14px 20px; border: 2px solid #000; cursor: pointer; font-size: 16px; width: 100%; font-weight: bold; margin-top: 10px; font-family: 'Courier New', Courier, monospace; text-transform: uppercase; transition: 0.1s; }
        button:hover, input[type=submit]:hover { background-color: #444; color: #fff; }
        .scan-btn { background-color: #fff; color: #000; border: 2px solid #000; }
        .scan-btn:hover { background-color: #bbb; color: #000; }
        
        #loader { display: none; font-size: 14px; color: #000; font-weight: bold; margin-bottom: 15px; text-align:center; background: #fff; padding: 5px; border: 1px solid #000; }
        #deviceSelect { display: none; margin-bottom: 15px; }
    </style>
</head>
<body>
    <div class="card">
        <h1>Tandy Bridge</h1>
        
        <div class="tab-bar">
            <button class="tab-btn active" onclick="openTab(event, 'btTab')">Bluetooth</button>
            <button class="tab-btn" onclick="openTab(event, 'keyTab')">Mappings</button>
        </div>

        <form action="/save" method="POST">
            <div id="btTab" class="tab-content active">
                <p style="text-align:center;">*** BLUETOOTH PAIRING ***</p>
                <button type="button" class="scan-btn" onclick="startScan()">[ SCAN FOR KEYBOARDS ]</button>
                <div id="loader">SCANNING AIRWAVES... PLEASE WAIT 3 SECONDS</div>
                <select id="deviceSelect" onchange="document.getElementById('macInput').value = this.value">
                    <option value="">-- SELECT YOUR KEYBOARD --</option>
                </select>
                <label>KEYBOARD MAC ADDRESS:</label>
                <input type="text" id="macInput" name="mac" placeholder="e.g., c0:32:05:6a:61:5a" required>
            </div>

            <div id="keyTab" class="tab-content">
                <p style="text-align:center;">*** SYSTEM CONFIGURATION ***</p>

                <label>TARGET MACHINE:</label>
                <select id="target" name="target">
                    <option value="0">TANDY MODEL II/12/16</option>
                    <option value="1">NES 64 (C64 KEYBOARD ON PORT 2)</option>
                </select>
                <p style="font-size:12px; font-weight:normal;">NES 64 MODE: KEYS FOLLOW THE PC KEYCAPS (SEE README). THE TANDY KEY MAPPINGS BELOW ARE NOT USED.</p>
                
                <label style="display:flex; align-items:center; justify-content:center; gap:10px; margin-top:20px; font-size:16px; background:#fff; border:2px solid #000; padding:10px;">
                    <input type="checkbox" id="capsDef" name="caps_def" value="1" style="width:auto; margin:0; transform:scale(1.2);">
                    START WITH CAPS LOCK ON
                </label>
                
                <hr style="border:0; border-top:2px dashed #000; margin:20px 0;">

                <label>TANDY [SHIFT LOCK] KEY:</label>
                <select id="mapLock" name="k_lck">
                    <option value="0">-- DISABLED --</option>
                    <option value="71">SCROLL LOCK</option>
                    <option value="66">F9</option>
                    <option value="67">F10</option>
                    <option value="68">F11</option>
                    <option value="69">F12</option>
                </select>

                <label>TANDY [REPEAT] KEY:</label>
                <select id="mapRepeat" name="k_rpt">
                    <option value="0">-- DISABLED --</option>
                    <option value="68">F11</option>
                    <option value="69">F12</option>
                    <option value="73">INSERT</option>
                    <option value="74">HOME</option>
                    <option value="75">PAGE UP</option>
                    <option value="76">DELETE</option>
                    <option value="77">END</option>
                    <option value="78">PAGE DOWN</option>
                </select>

                <label>TANDY [BREAK] KEY:</label>
                <select id="mapBreak" name="k_break">
                    <option value="66">F9</option>
                    <option value="67">F10</option>
                    <option value="68">F11</option>
                    <option value="69">F12</option>
                    <option value="73">INSERT</option>
                    <option value="74">HOME</option>
                    <option value="75">PAGE UP</option>
                    <option value="76">DELETE</option>
                    <option value="77">END</option>
                    <option value="78">PAGE DOWN</option>
                    <option value="41">ESCAPE</option>
                </select>

                <label>TANDY [HOLD] KEY:</label>
                <select id="mapHold" name="k_hold">
                    <option value="67">F10</option>
                    <option value="66">F9</option>
                    <option value="68">F11</option>
                    <option value="69">F12</option>
                    <option value="73">INSERT</option>
                    <option value="74">HOME</option>
                    <option value="75">PAGE UP</option>
                    <option value="76">DELETE</option>
                    <option value="77">END</option>
                    <option value="78">PAGE DOWN</option>
                    <option value="41">ESCAPE</option>
                </select>
                
                <label>TANDY [ESC] KEY:</label>
                <select id="mapEsc" name="k_esc">
                    <option value="41">ESCAPE</option>
                    <option value="66">F9</option>
                    <option value="67">F10</option>
                    <option value="68">F11</option>
                    <option value="69">F12</option>
                </select>
            </div>

            <input type="submit" value="[ SAVE ALL AND REBOOT ]">
        </form>
    </div>

    <script>
        function openTab(evt, tabName) {
            let i, tabcontent, tablinks;
            tabcontent = document.getElementsByClassName("tab-content");
            for (i = 0; i < tabcontent.length; i++) { tabcontent[i].classList.remove("active"); }
            tablinks = document.getElementsByClassName("tab-btn");
            for (i = 0; i < tablinks.length; i++) { tablinks[i].classList.remove("active"); }
            document.getElementById(tabName).classList.add("active");
            evt.currentTarget.classList.add("active");
        }

        window.onload = function() {
            fetch('/get_settings')
            .then(response => response.json())
            .then(data => {
                document.getElementById('macInput').value = data.mac;
                if(data.caps_def) document.getElementById('capsDef').checked = true;
                if(data.target !== undefined) document.getElementById('target').value = data.target;
                if(data.k_break !== undefined) document.getElementById('mapBreak').value = data.k_break;
                if(data.k_hold !== undefined) document.getElementById('mapHold').value = data.k_hold;
                if(data.k_esc !== undefined) document.getElementById('mapEsc').value = data.k_esc;
                if(data.k_rpt !== undefined) document.getElementById('mapRepeat').value = data.k_rpt;
                if(data.k_lck !== undefined) document.getElementById('mapLock').value = data.k_lck;
            })
            .catch(err => console.log("Failed to load settings"));
        };

        function startScan() {
            document.getElementById('loader').style.display = 'block';
            document.getElementById('deviceSelect').style.display = 'none';
            
            fetch('/scan')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('loader').style.display = 'none';
                    let select = document.getElementById('deviceSelect');
                    select.innerHTML = '<option value="">-- SELECT YOUR KEYBOARD --</option>'; 
                    
                    if(data.length === 0) {
                        alert("ERROR: NO DEVICES FOUND. ENSURE KEYBOARD IS IN PAIRING MODE.");
                        return;
                    }
                    
                    data.forEach(device => {
                        let opt = document.createElement('option');
                        opt.value = device.mac;
                        opt.innerHTML = device.name + " (" + device.mac + ")";
                        select.appendChild(opt);
                    });
                    select.style.display = 'block';
                })
                .catch(err => {
                    document.getElementById('loader').style.display = 'none';
                    alert("ERROR: SCAN FAILED.");
                });
        }
    </script>
</body>
</html>
)rawliteral";

// =========================================================================
// TANDY BIT-BANGER & TRANSLATOR LOGIC
// =========================================================================

int16_t mapHIDToASCII(uint8_t hidCode, bool isShifted, bool isCapsLocked) {
    if (hidCode >= 0x04 && hidCode <= 0x1D) {
        char baseChar = (hidCode - 0x04) + 'a';
        if (isShifted != isCapsLocked) baseChar -= 32; 
        return baseChar; 
    }
    if (hidCode >= 0x1E && hidCode <= 0x27) {
        const char unshifted[] = "1234567890";
        const char shifted[]   = "!@#$%^&*()";
        int index = hidCode - 0x1E;
        return isShifted ? shifted[index] : unshifted[index];
    }
    if (hidCode == 0x2D) return isShifted ? '_' : '-';
    if (hidCode == 0x2E) return isShifted ? '+' : '=';
    if (hidCode == 0x2F) return isShifted ? '{' : '[';
    if (hidCode == 0x30) return isShifted ? '}' : ']';
    if (hidCode == 0x31) return isShifted ? '|' : '\\';
    if (hidCode == 0x33) return isShifted ? ':' : ';';
    if (hidCode == 0x34) return isShifted ? '"' : '\'';
    if (hidCode == 0x36) return isShifted ? '<' : ',';
    if (hidCode == 0x37) return isShifted ? '>' : '.';
    if (hidCode == 0x38 || hidCode == 0x54) return isShifted ? '?' : '/';
    if (hidCode == 0x28) return 13;   
    if (hidCode == 0x2A) return 8;    
    if (hidCode == 0x2C) return ' ';  
    
    // --- DYNAMIC TANDY KEYS ---
    if (hidCode == key_break) return 0x03; 
    if (hidCode == key_hold)  return 0x00; 
    if (hidCode == key_esc)   return 0x1B; 

    if (hidCode == 0x2B) return 0x09; 
    if (hidCode == 0x4F) return 0x1D; 
    if (hidCode == 0x50) return 0x1C; 
    if (hidCode == 0x51) return 0x1F; 
    if (hidCode == 0x52) return 0x1E;    
    return -1; 
}

void sendTandyChar(uint8_t ascii_val) {
    portENTER_CRITICAL(&mux); 
    digitalWrite(DATA_PIN, ascii_val & 0x01);
    delayMicroseconds(20); 
    for (int i = 0; i < 8; i++) {
        digitalWrite(CLOCK_PIN, HIGH); 
        delayMicroseconds(20); 
        if (i < 7) {
            uint8_t next_bit = (ascii_val >> (i + 1)) & 0x01;
            digitalWrite(DATA_PIN, next_bit);
        }
        delayMicroseconds(20); 
        digitalWrite(CLOCK_PIN, LOW);  
        delayMicroseconds(20); 
    }
    digitalWrite(DATA_PIN, LOW);  
    delayMicroseconds(20);   
    digitalWrite(DATA_PIN, HIGH);  
    delayMicroseconds(40); 
    portEXIT_CRITICAL(&mux);
    delay(20); 
}

// =========================================================================
// NES 64 ADAPTER: C64 KEYBOARD MATRIX ON NES CONTROLLER PORT 2
// =========================================================================
// The NES ROM pulses OUT0 (latch) high then low, then clocks 88 bits out of
// D0, one per $4017 read. The packet is:
//   bytes 0-1  signature $A5 $5A
//   bytes 2-9  C64 matrix rows 0-7 (CIA 1 layout), 0 = pressed
//   byte 10    bit 0 RESTORE, 0 = pressed; bits 1-7 always 1
// Bits go out MSB first like the AVR SPI reference design, and the line is
// driven complemented because the NES input buffer inverts D0. After 88 bits
// we send 1s, like a stock pad after its 8 buttons.
//
// Instead of SPI slave hardware, two GPIO interrupts on core 1 (away from the
// BLE stack) play the 4021 shift register: the latch reloads the packet, each
// CLK rising edge shifts the next bit out. The ROM's read loop leaves ~9us per
// bit and the ISR responds in ~2-3us.

const int NES_PACKET_BYTES = 11;
const int NES_PACKET_BITS  = NES_PACKET_BYTES * 8;

// Matrix position: row (0-7) * 8 + bit (0-7). RESTORE is not in the matrix.
#define CK(row, bit) (uint8_t)((row) * 8 + (bit))
const uint8_t C64_NONE    = 0xFF;
const uint8_t C64_RESTORE = 0xFE;

const uint8_t C64_DEL = CK(0,0), C64_RETURN = CK(0,1), C64_CRSR_RT = CK(0,2), C64_F7 = CK(0,3);
const uint8_t C64_F1 = CK(0,4), C64_F3 = CK(0,5), C64_F5 = CK(0,6), C64_CRSR_DN = CK(0,7);
const uint8_t C64_LSHIFT = CK(1,7), C64_RSHIFT = CK(6,4);
const uint8_t C64_PLUS = CK(5,0), C64_MINUS = CK(5,3), C64_PERIOD = CK(5,4), C64_COLON = CK(5,5);
const uint8_t C64_AT = CK(5,6), C64_COMMA = CK(5,7), C64_POUND = CK(6,0), C64_STAR = CK(6,1);
const uint8_t C64_SEMI = CK(6,2), C64_HOME = CK(6,3), C64_EQUALS = CK(6,5), C64_UPARROW = CK(6,6);
const uint8_t C64_SLASH = CK(6,7), C64_LEFTARROW = CK(7,1), C64_CTRL = CK(7,2), C64_SPACE = CK(7,4);
const uint8_t C64_CBM = CK(7,5), C64_STOP = CK(7,7);

const uint8_t C64_LETTERS[26] = {
    CK(1,2), CK(3,4), CK(2,4), CK(2,2), CK(1,6), CK(2,5), CK(3,2), // A-G
    CK(3,5), CK(4,1), CK(4,2), CK(4,5), CK(5,2), CK(4,4), CK(4,7), // H-N
    CK(4,6), CK(5,1), CK(7,6), CK(2,1), CK(1,5), CK(2,6), CK(3,6), // O-U
    CK(3,7), CK(1,1), CK(2,7), CK(3,1), CK(1,4)                    // V-Z
};
const uint8_t C64_DIGITS[10] = { // 1-9, 0 (HID order)
    CK(7,0), CK(7,3), CK(1,0), CK(1,3), CK(2,0), CK(2,3), CK(3,0), CK(3,3), CK(4,0), CK(4,3)
};

// What a key does to the C64 Shift lines while it is down
const uint8_t SH_PASS = 0; // C64 Shift follows the PC Shift keys (and SHIFT LOCK)
const uint8_t SH_ON   = 1; // Force C64 Left Shift down
const uint8_t SH_OFF  = 2; // Force both C64 Shifts up

struct C64KeyMap {
    uint8_t hid;
    uint8_t posKey;               // Used while C= or CTRL is held: positional, no Shift faking
    uint8_t key,  mode;           // PC Shift up
    uint8_t sKey, sMode;          // PC Shift down
};
bool c64LookupKey(uint8_t hid, C64KeyMap& out);

// Symbolic layout: every symbol stays where the PC keycap prints it.
// Letters, digits 1/3/4/5 and keys that agree on both machines pass Shift through.
const C64KeyMap C64_KEYMAP[] = {
    // Digits whose shifted symbol lives elsewhere on the C64
    { 0x1F, CK(7,3),       CK(7,3),       SH_PASS, C64_AT,        SH_OFF  }, // 2  @
    { 0x23, CK(2,3),       CK(2,3),       SH_PASS, C64_UPARROW,   SH_OFF  }, // 6  ^
    { 0x24, CK(3,0),       CK(3,0),       SH_PASS, CK(2,3),       SH_ON   }, // 7  & = Shift+6
    { 0x25, CK(3,3),       CK(3,3),       SH_PASS, C64_STAR,      SH_OFF  }, // 8  *
    { 0x26, CK(4,0),       CK(4,0),       SH_PASS, CK(3,3),       SH_ON   }, // 9  ( = Shift+8
    { 0x27, CK(4,3),       CK(4,3),       SH_PASS, CK(4,0),       SH_ON   }, // 0  ) = Shift+9
    // Symbol keys
    { 0x2D, C64_PLUS,      C64_MINUS,     SH_PASS, C64_LEFTARROW, SH_OFF  }, // -  _
    { 0x2E, C64_MINUS,     C64_EQUALS,    SH_PASS, C64_PLUS,      SH_OFF  }, // =  +
    { 0x2F, C64_AT,        C64_COLON,     SH_ON,   C64_COLON,     SH_ON   }, // [  {
    { 0x30, C64_STAR,      C64_SEMI,      SH_ON,   C64_SEMI,      SH_ON   }, // ]  }
    { 0x31, C64_POUND,     C64_POUND,     SH_PASS, C64_POUND,     SH_PASS }, // backslash  |
    { 0x32, C64_POUND,     C64_POUND,     SH_PASS, C64_POUND,     SH_PASS }, // ISO # key, same as backslash
    { 0x33, C64_COLON,     C64_SEMI,      SH_PASS, C64_COLON,     SH_OFF  }, // ;  :
    { 0x34, C64_SEMI,      CK(3,0),       SH_ON,   CK(7,3),       SH_ON   }, // '  " = Shift+7, Shift+2
    { 0x35, C64_LEFTARROW, C64_LEFTARROW, SH_PASS, C64_LEFTARROW, SH_PASS }, // `
    { 0x36, C64_COMMA,     C64_COMMA,     SH_PASS, C64_COMMA,     SH_PASS }, // ,  <
    { 0x37, C64_PERIOD,    C64_PERIOD,    SH_PASS, C64_PERIOD,    SH_PASS }, // .  >
    { 0x38, C64_SLASH,     C64_SLASH,     SH_PASS, C64_SLASH,     SH_PASS }, // /  ?
    // Special keys (one table, adjust to taste)
    { 0x29, C64_STOP,      C64_STOP,      SH_PASS, C64_STOP,      SH_PASS }, // Esc -> RUN/STOP
    { 0x4B, C64_RESTORE,   C64_RESTORE,   SH_PASS, C64_RESTORE,   SH_PASS }, // Page Up -> RESTORE
    { 0x2B, C64_CTRL,      C64_CTRL,      SH_PASS, C64_CTRL,      SH_PASS }, // Tab -> CTRL
    { 0x2A, C64_DEL,       C64_DEL,       SH_PASS, C64_DEL,       SH_PASS }, // Backspace -> INST/DEL
    { 0x49, C64_DEL,       C64_DEL,       SH_ON,   C64_DEL,       SH_ON   }, // Insert -> Shift+INST/DEL
    { 0x4A, C64_HOME,      C64_HOME,      SH_PASS, C64_HOME,      SH_PASS }, // Home -> CLR/HOME
    { 0x52, C64_CRSR_DN,   C64_CRSR_DN,   SH_ON,   C64_CRSR_DN,   SH_ON   }, // Up
    { 0x50, C64_CRSR_RT,   C64_CRSR_RT,   SH_ON,   C64_CRSR_RT,   SH_ON   }, // Left
    { 0x51, C64_CRSR_DN,   C64_CRSR_DN,   SH_OFF,  C64_CRSR_DN,   SH_OFF  }, // Down
    { 0x4F, C64_CRSR_RT,   C64_CRSR_RT,   SH_OFF,  C64_CRSR_RT,   SH_OFF  }, // Right
    { 0x3A, C64_F1,        C64_F1,        SH_PASS, C64_F1,        SH_PASS }, // F1
    { 0x3B, C64_F1,        C64_F1,        SH_ON,   C64_F1,        SH_ON   }, // F2 = Shift+F1
    { 0x3C, C64_F3,        C64_F3,        SH_PASS, C64_F3,        SH_PASS }, // F3
    { 0x3D, C64_F3,        C64_F3,        SH_ON,   C64_F3,        SH_ON   }, // F4 = Shift+F3
    { 0x3E, C64_F5,        C64_F5,        SH_PASS, C64_F5,        SH_PASS }, // F5
    { 0x3F, C64_F5,        C64_F5,        SH_ON,   C64_F5,        SH_ON   }, // F6 = Shift+F5
    { 0x40, C64_F7,        C64_F7,        SH_PASS, C64_F7,        SH_PASS }, // F7
    { 0x41, C64_F7,        C64_F7,        SH_ON,   C64_F7,        SH_ON   }, // F8 = Shift+F7
    { 0x28, C64_RETURN,    C64_RETURN,    SH_PASS, C64_RETURN,    SH_PASS }, // Enter
    { 0x58, C64_RETURN,    C64_RETURN,    SH_PASS, C64_RETURN,    SH_PASS }, // Keypad Enter
    { 0x2C, C64_SPACE,     C64_SPACE,     SH_PASS, C64_SPACE,     SH_PASS }, // Space
};
const uint8_t HID_CAPS_LOCK = 0x39;
const uint8_t HID_MOD_LCTRL = 0x01, HID_MOD_LSHIFT = 0x02, HID_MOD_RSHIFT = 0x20;

// Look up a key; letters and plain digits are generated, the rest come from the table
bool c64LookupKey(uint8_t hid, C64KeyMap& out) {
    if (hid >= 0x04 && hid <= 0x1D) {
        uint8_t k = C64_LETTERS[hid - 0x04];
        out = { hid, k, k, SH_PASS, k, SH_PASS };
        return true;
    }
    for (const C64KeyMap& m : C64_KEYMAP) {
        if (m.hid == hid) { out = m; return true; }
    }
    if (hid >= 0x1E && hid <= 0x27) { // 1, 3, 4, 5: same symbols on both machines
        uint8_t k = C64_DIGITS[hid - 0x1E];
        out = { hid, k, k, SH_PASS, k, SH_PASS };
        return true;
    }
    return false;
}

// Double-buffered packet: the BLE side builds into the spare and swaps the
// pointer, the latch ISR copies the current one, so a packet never mixes states.
uint8_t nesPacketBuf[2][NES_PACKET_BYTES];
volatile uint8_t* volatile nesPacketReady = nesPacketBuf[0];
DRAM_ATTR uint8_t nesTxBuf[NES_PACKET_BYTES];
DRAM_ATTR volatile uint8_t nesBitIndex = NES_PACKET_BITS;

// BLE-side key state
uint8_t c64HeldKeys[6];       // PC keys down, oldest press first
uint8_t c64HeldCount = 0;
bool c64ShiftLock = false;

static inline void IRAM_ATTR nesDriveBit() {
    uint8_t idx = nesBitIndex;
    bool bit = (idx >= NES_PACKET_BITS) ? 1 : (nesTxBuf[idx >> 3] >> (7 - (idx & 7))) & 1;
    // The NES inverts D0: drive the line LOW to send a logical 1
    if (bit) REG_WRITE(GPIO_OUT_W1TC_REG, 1UL << NES_DATA);
    else     REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << NES_DATA);
}

// OUT0 high = latch: reload the packet and present its first bit
void IRAM_ATTR nesLatchISR() {
    if (!(REG_READ(GPIO_IN_REG) & (1UL << NES_LATCH))) return;
    volatile uint8_t* src = nesPacketReady;
    for (int i = 0; i < NES_PACKET_BYTES; i++) nesTxBuf[i] = src[i];
    nesBitIndex = 0;
    nesDriveBit();
}

// CLK rising edge = end of a $4017 read: shift out the next bit
void IRAM_ATTR nesClockISR() {
    if (REG_READ(GPIO_IN_REG) & (1UL << NES_LATCH)) return; // Still latching: hold bit 0
    if (nesBitIndex < NES_PACKET_BITS) nesBitIndex = nesBitIndex + 1;
    nesDriveBit();
}

void nesPublishMatrix(const uint8_t rows[8], bool restore) {
    uint8_t* spare = (nesPacketReady == nesPacketBuf[0]) ? nesPacketBuf[1] : nesPacketBuf[0];
    spare[0] = 0xA5;
    spare[1] = 0x5A;
    for (int r = 0; r < 8; r++) spare[2 + r] = rows[r];
    spare[10] = restore ? 0xFE : 0xFF;
    nesPacketReady = spare;
}

static inline void c64Press(uint8_t rows[8], uint8_t key) {
    if (key < 64) rows[key >> 3] &= ~(1 << (key & 7));
}

// Rebuild the whole C64 state from the PC keys that are down
void c64Rebuild(uint8_t mods) {
    uint8_t rows[8];
    memset(rows, 0xFF, sizeof(rows));
    bool restore = false;
    bool pcShift = mods & (HID_MOD_LSHIFT | HID_MOD_RSHIFT);
    bool cbm = mods & HID_MOD_LCTRL;
    bool ctrl = false;
    for (int i = 0; i < c64HeldCount; i++) if (c64HeldKeys[i] == 0x2B) ctrl = true;
    bool positional = cbm || ctrl;

    bool lShift = (mods & HID_MOD_LSHIFT) || c64ShiftLock;
    bool rShift = mods & HID_MOD_RSHIFT;
    uint8_t shiftOverride = SH_PASS; // Most recent key with a Shift need wins

    if (cbm) c64Press(rows, C64_CBM);
    for (int i = 0; i < c64HeldCount; i++) {
        C64KeyMap m;
        if (!c64LookupKey(c64HeldKeys[i], m)) continue;
        uint8_t key = positional ? m.posKey : (pcShift ? m.sKey : m.key);
        uint8_t mode = positional ? SH_PASS : (pcShift ? m.sMode : m.mode);
        if (key == C64_RESTORE) restore = true;
        else c64Press(rows, key);
        if (mode != SH_PASS) shiftOverride = mode;
    }

    if (shiftOverride == SH_ON) lShift = true;
    if (shiftOverride == SH_OFF) lShift = rShift = false;
    if (lShift) c64Press(rows, C64_LSHIFT);
    if (rShift) c64Press(rows, C64_RSHIFT);
    nesPublishMatrix(rows, restore);
}

void c64ApplyReport(const uint8_t* pData, size_t length) {
    if (length < 3 || pData[2] == 0x01) return; // Phantom/rollover report: keep last state

    // Keep press order: keys still down stay in place, new presses go to the end
    uint8_t next[6];
    uint8_t count = 0;
    for (int i = 0; i < c64HeldCount; i++) {
        for (size_t j = 1; j < length; j++) {
            if (pData[j] == c64HeldKeys[i]) { next[count++] = c64HeldKeys[i]; break; }
        }
    }
    for (size_t j = 1; j < length && count < 6; j++) {
        uint8_t k = pData[j];
        if (k == 0x00) continue;
        bool known = false;
        for (int i = 0; i < count; i++) if (next[i] == k) { known = true; break; }
        if (known) continue;
        next[count++] = k;
        if (k == HID_CAPS_LOCK) c64ShiftLock = !c64ShiftLock; // SHIFT LOCK toggles on press
    }
    memcpy(c64HeldKeys, next, count);
    c64HeldCount = count;
    c64Rebuild(pData[0]);
}

void c64ReleaseAll() {
    c64HeldCount = 0;
    c64Rebuild(0); // SHIFT LOCK stays as it was
}

void nesBegin() {
    c64ReleaseAll();
    pinMode(NES_DATA, OUTPUT);
    pinMode(NES_LATCH, INPUT);
    pinMode(NES_CLK, INPUT);
    nesDriveBit(); // Idle: send 1s until the first latch
    // Attached from setup() so the ISRs run on core 1, away from the BLE stack
    attachInterrupt(NES_LATCH, nesLatchISR, RISING);
    attachInterrupt(NES_CLK, nesClockISR, RISING);
}

void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    if (targetMachine == TARGET_NES64) {
        c64ApplyReport(pData, length);
        return;
    }

    static uint8_t lastReport[8] = {0}; 
    
    bool isPhysicalShifted = (pData[0] & 0x02) || (pData[0] & 0x20);

    // 1. HARDWARE REPEATER CHECK
    isRepeatKeyHeld = false;
    if (key_repeat != 0x00) {
        for (int i = 1; i < length; i++) {
            if (pData[i] == key_repeat) {
                isRepeatKeyHeld = true;
                break;
            }
        }
    }

    // 2. PROCESS NORMAL KEYS
    for (int i = 1; i < length; i++) {
        uint8_t currentKey = pData[i];
        if (currentKey != 0x00) {
            bool isNewPress = true;
            for (int j = 1; j < 8; j++) {
                if (lastReport[j] == currentKey) { isNewPress = false; break; }
            }
            if (isNewPress) {
                
                if (currentKey == key_repeat) continue; 
                
                if (currentKey == 0x39) {
                    capsLockState = !capsLockState; 
                } else if (currentKey == key_lock && key_lock != 0x00) {
                    shiftLockState = !shiftLockState; 
                } else {
                    bool effectiveShift = isPhysicalShifted || shiftLockState;
                    
                    int16_t tandyCode = mapHIDToASCII(currentKey, effectiveShift, capsLockState);
                    if (tandyCode != -1) {
                        last_valid_tandy_char = (uint8_t)tandyCode; 
                        sendTandyChar((uint8_t)tandyCode); 
                    }
                }
            }
        }
    }
    memset(lastReport, 0, 8);
    for (int i = 0; i < length && i < 8; i++) { lastReport[i] = pData[i]; }
}

class ScanCallbacks : public NimBLEScanCallbacks { 
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        std::string deviceMAC = advertisedDevice->getAddress().toString();
        if (deviceMAC == targetMAC) {
            Serial.println("TARGET ACQUIRED! Stopping scan...");
            advDevice = advertisedDevice;
            doConnect = true;
            NimBLEDevice::getScan()->stop();
        }
    }
};

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pClient, int reason) override {
        // Don't leave keys stuck down on the NES
        if (targetMachine == TARGET_NES64) c64ReleaseAll();
    }
};

bool connectToKeyboard() {
    NimBLEClient* pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks(), true);
    if (!pClient->connect(advDevice)) return false;
    pClient->secureConnection(); 
    delay(500); 
    NimBLERemoteService* pService = pClient->getService(NimBLEUUID((uint16_t)0x1812));
    if (pService != nullptr) {
        bool foundNotify = false;
        auto pChars = pService->getCharacteristics(true);
        for (auto pChar : pChars) {
            if (pChar->getUUID() == NimBLEUUID((uint16_t)0x2A4D) && pChar->canNotify()) {
                pChar->subscribe(true, notifyCB); 
                Serial.println("Subscribed to a Keystroke channel!");
                foundNotify = true;
            }
        }
        if (foundNotify) return true;
    }
    return false;
}

// =========================================================================
// BOOT SEQUENCE & WEB API ENDPOINTS
// =========================================================================

void setup() {
    Serial.begin(115200);
    
    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    pinMode(BUSY_PIN, INPUT_PULLUP);
    pinMode(CLOCK_PIN, OUTPUT);
    pinMode(DATA_PIN, OUTPUT);
    digitalWrite(CLOCK_PIN, LOW);
    digitalWrite(DATA_PIN, HIGH); 

    // --- LOAD MEMORY SETTINGS EARLY ---
    preferences.begin("tandyBridge", true); 
    String savedMAC = preferences.getString("mac", "c0:32:05:6a:61:5a");
    targetMAC = savedMAC.c_str();
    
    targetMachine = preferences.getUChar("target", TARGET_TANDY);
    defaultCapsOn = preferences.getBool("caps_def", false);
    capsLockState = defaultCapsOn; 
    shiftLockState = false; 
    
    key_break  = preferences.getUChar("k_break", 0x42);
    key_hold   = preferences.getUChar("k_hold", 0x43);
    key_esc    = preferences.getUChar("k_esc", 0x29);
    key_repeat = preferences.getUChar("k_rpt", 0x00);
    key_lock   = preferences.getUChar("k_lck", 0x47); 
    preferences.end(); 

    Serial.println("\n==================================");
    Serial.println("You have 3 seconds to press the BOOT button for Wi-Fi Setup...");
    Serial.println("==================================");
    
    bool buttonPressed = false;
    for (int i = 0; i < 30; i++) { 
        if (digitalRead(BOOT_BUTTON) == LOW) {
            buttonPressed = true;
            break; 
        }
        delay(100);
    }

    if (buttonPressed) {
        inConfigMode = true;
        Serial.println("\nBOOT BUTTON DETECTED! Starting Web Configurator...");
        
        NimBLEDevice::init("");
        WiFi.mode(WIFI_AP); 
        WiFi.softAP("Tandy Setup", "password123"); 
        
        Serial.print("Wi-Fi Network: Tandy Setup\nPassword: password123\n");
        Serial.print("IP Address: ");
        Serial.println(WiFi.softAPIP());

        server.on("/", HTTP_GET, []() {
            server.send(200, "text/html", webpageHTML);
        });

        server.on("/get_settings", HTTP_GET, []() {
            String json = "{\"mac\":\"" + String(targetMAC.c_str()) + "\",";
            json += "\"target\":" + String(targetMachine) + ",";
            json += "\"caps_def\":" + String(defaultCapsOn ? "true" : "false") + ",";
            json += "\"k_break\":" + String(key_break) + ",";
            json += "\"k_hold\":" + String(key_hold) + ",";
            json += "\"k_esc\":" + String(key_esc) + ",";
            json += "\"k_rpt\":" + String(key_repeat) + ",";
            json += "\"k_lck\":" + String(key_lock) + "}";
            server.send(200, "application/json", json);
        });

        server.on("/scan", HTTP_GET, []() {
            Serial.println("Web Request: Scanning for Bluetooth devices...");
            NimBLEScan* pScan = NimBLEDevice::getScan();
            pScan->setActiveScan(true);
            pScan->clearResults(); 
            pScan->start(3000, false); 
            delay(3000); 
            pScan->stop();
            NimBLEScanResults results = pScan->getResults();
            
            String json = "[";
            bool first = true;
            for(int i = 0; i < results.getCount(); i++) {
                const NimBLEAdvertisedDevice* device = results.getDevice(i);
                if(!first) json += ",";
                
                String name = "Unknown Device"; 
                if(device->haveName() && device->getName().length() > 0) {
                    name = device->getName().c_str();
                    name.replace("\"", "'"); 
                }
                String mac = device->getAddress().toString().c_str();
                
                json += "{\"name\":\"" + name + "\", \"mac\":\"" + mac + "\"}";
                first = false;
            }
            json += "]";
            
            pScan->clearResults(); 
            server.send(200, "application/json", json);
            Serial.println("Scan complete.");
        });

        server.on("/save", HTTP_POST, []() {
            preferences.begin("tandyBridge", false);
            
            if (server.hasArg("mac")) {
                String newMAC = server.arg("mac");
                newMAC.trim(); newMAC.toLowerCase(); 
                preferences.putString("mac", newMAC);
            }
            
            if (server.hasArg("target")) preferences.putUChar("target", server.arg("target").toInt());
            preferences.putBool("caps_def", server.hasArg("caps_def"));
            
            if (server.hasArg("k_break")) preferences.putUChar("k_break", server.arg("k_break").toInt());
            if (server.hasArg("k_hold"))  preferences.putUChar("k_hold", server.arg("k_hold").toInt());
            if (server.hasArg("k_esc"))   preferences.putUChar("k_esc", server.arg("k_esc").toInt());
            if (server.hasArg("k_rpt"))   preferences.putUChar("k_rpt", server.arg("k_rpt").toInt());
            if (server.hasArg("k_lck"))   preferences.putUChar("k_lck", server.arg("k_lck").toInt());
            
            preferences.end();
            
            server.send(200, "text/html", "<body style='background:#808080; color:#000; font-family:Courier New,monospace; text-align:center; margin-top:50px;'><h1>SETTINGS SAVED!</h1><p>REBOOTING BRIDGE...</p></body>");
            Serial.println("Settings saved! Rebooting...");
            delay(2000);
            ESP.restart(); 
        });

        server.begin();
        Serial.println("Web server running!");
        
    } else {
        inConfigMode = false;
        Serial.println("\nNO BUTTON PRESSED. Loading Bluetooth Bridge...");
        Serial.print("Target Keyboard MAC is: ");
        Serial.println(targetMAC.c_str());

        if (targetMachine == TARGET_NES64) {
            Serial.println("Target machine: NES 64 (C64 keyboard on controller port 2)");
            nesBegin();
        }

        NimBLEDevice::init("");
        NimBLEDevice::setSecurityAuth(true, true, true); 
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); 
        
        NimBLEScan* pScan = NimBLEDevice::getScan();
        pScan->setScanCallbacks(new ScanCallbacks());
        pScan->setActiveScan(true);
        pScan->start(0, false); 
    }
}

void loop() {
    if (inConfigMode) {
        server.handleClient();
        delay(2);
    } else {
        if (doConnect) {
            doConnect = false;
            if (!connectToKeyboard()) {
                delay(2000);
                NimBLEDevice::getScan()->start(0, false); 
            }
        }
        
        // --- THE TANDY HARDWARE REPEATER ---
        if (isRepeatKeyHeld && last_valid_tandy_char != 0x00) {
            unsigned long currentMillis = millis();
            if (currentMillis - lastRepeatTime >= 80) { 
                lastRepeatTime = currentMillis;
                sendTandyChar(last_valid_tandy_char);
            }
        }
        
        delay(10);
    }
}
