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

// --- FAMICOM EXPANSION PORT PINS (Family BASIC keyboard emulation) ---
// Inputs come from the Famicom ($4016 writes) and are 5V: level-shift them!
const int FAMI_OUT0 = 18; // Port pin 12: reset row counter
const int FAMI_OUT1 = 19; // Port pin 11: column select (row++ on high->low)
const int FAMI_OUT2 = 23; // Port pin 10: keyboard enable
// Outputs to the Famicom ($4017 bits 1-4). HIGH = key pressed (inverted by the console).
const int FAMI_D1 = 22;   // Port pin 7
const int FAMI_D2 = 25;   // Port pin 6
const int FAMI_D3 = 26;   // Port pin 5
const int FAMI_D4 = 27;   // Port pin 4

// --- TARGET MACHINE ---
const uint8_t TARGET_TANDY   = 0;
const uint8_t TARGET_FAMICOM = 1;
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
                    <option value="1">FAMICOM (FAMILY BASIC KEYBOARD)</option>
                </select>
                <p style="font-size:12px; font-weight:normal;">FAMICOM MODE: KEYS ARE MAPPED BY POSITION LIKE THE JAPANESE KEYBOARD. THE [BREAK] KEY BELOW SENDS [STOP]. CAPS, SHIFT LOCK AND REPEAT ARE TANDY ONLY.</p>
                
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

                <label>TANDY [BREAK] / FAMICOM [STOP] KEY:</label>
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
// FAMICOM FAMILY BASIC KEYBOARD EMULATION
// =========================================================================
// The real keyboard is a CD4017 row counter (9 rows used, 10th is empty) and a
// 1-of-2 column selector, read 4 bits at a time through $4017 bits 1-4.
// We mirror that counter in a GPIO interrupt and drive the 4 data lines from a
// key matrix that the BLE callback keeps up to date. The console waits ~50 CPU
// cycles (~28us) after each column select, so ISR latency is not an issue.

portMUX_TYPE famiMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t famiMatrix[9][2] = {{0}}; // bit0..3 = $4017 D1..D4, 1 = pressed
volatile uint8_t famiRow = 0;
volatile uint8_t famiCol = 0;
volatile bool famiEnabled = false;
volatile bool famiLastCol = false;
DRAM_ATTR uint32_t famiPinMask[16]; // nibble -> GPIO bitmask of the lines to set HIGH

// Matrix position: row (0-8), column (0-1), $4017 bit (1-4)
#define FK(row, col, bit) (uint8_t)(((row) << 3) | ((col) << 2) | ((bit) - 1))
const uint8_t FK_NONE = 0xFF;

const uint8_t FK_LETTERS[26] = {
    FK(6,0,4), FK(4,1,1), FK(5,1,2), FK(5,0,4), FK(6,1,3), FK(5,1,1), FK(4,0,3), // A-G
    FK(4,0,4), FK(3,0,2), FK(3,0,4), FK(2,0,4), FK(2,0,3), FK(3,1,1), FK(3,1,2), // H-N
    FK(2,0,2), FK(2,1,3), FK(7,0,3), FK(5,0,3), FK(6,0,3), FK(5,0,2), FK(3,0,3), // O-U
    FK(4,1,2), FK(6,0,2), FK(6,1,1), FK(4,0,2), FK(6,1,2)                        // V-Z
};
const uint8_t FK_DIGITS[10] = { // 1-9, 0 (HID order)
    FK(7,1,3), FK(7,1,4), FK(6,1,4), FK(5,1,4), FK(5,1,3),
    FK(4,1,4), FK(4,1,3), FK(3,1,4), FK(3,1,3), FK(2,1,4)
};
const uint8_t FK_FKEYS[8] = { // F1-F8
    FK(7,0,1), FK(6,0,1), FK(5,0,1), FK(4,0,1), FK(3,0,1), FK(2,0,1), FK(1,0,1), FK(0,0,1)
};
const uint8_t FK_STOP = FK(0,1,4), FK_KANA = FK(0,1,1), FK_RSHIFT = FK(0,1,2);
const uint8_t FK_CTR = FK(7,0,4), FK_GRPH = FK(7,1,2), FK_LSHIFT = FK(7,1,1);

// Positional mapping: each modern key acts like the key in the same spot on the
// Japanese Family BASIC keyboard (e.g. US [ is @, US ' is :).
uint8_t famiKeyForHID(uint8_t hid) {
    if (hid >= 0x04 && hid <= 0x1D) return FK_LETTERS[hid - 0x04];
    if (hid >= 0x1E && hid <= 0x27) return FK_DIGITS[hid - 0x1E];
    if (hid >= 0x59 && hid <= 0x62) return FK_DIGITS[hid - 0x59];   // Keypad 1-9, 0
    if (hid >= 0x3A && hid <= 0x41) return FK_FKEYS[hid - 0x3A];
    if (hid == key_break) return FK_STOP;
    switch (hid) {
        case 0x28: case 0x58: return FK(0,0,2);   // Enter -> RETURN
        case 0x29: return FK(7,0,2);              // Esc -> ESC
        case 0x2A: case 0x4C: return FK(8,1,3);   // Backspace / Delete -> DEL
        case 0x2C: return FK(8,1,2);              // Space
        case 0x2D: case 0x56: return FK(1,1,3);   // - 
        case 0x2E: return FK(1,1,4);              // = -> ^
        case 0x2F: return FK(1,0,2);              // [ -> @
        case 0x30: return FK(0,0,3);              // ] -> [
        case 0x31: case 0x32: return FK(0,0,4);   // \ -> ]
        case 0x33: return FK(1,0,4);              // ;
        case 0x34: return FK(1,0,3);              // ' -> :
        case 0x35: case 0x89: return FK(0,1,3);   // ` / Intl Yen -> YEN
        case 0x36: return FK(2,1,2);              // ,
        case 0x37: case 0x63: return FK(2,1,1);   // .
        case 0x38: case 0x54: return FK(1,1,2);   // /
        case 0x87: return FK(1,1,1);              // Intl Ro -> _
        case 0x48: return FK_STOP;                // Pause -> STOP
        case 0x49: return FK(8,1,4);              // Insert -> INS
        case 0x4A: return FK(8,0,1);              // Home -> CLR HOME
        case 0x4F: return FK(8,0,3);              // Right
        case 0x50: return FK(8,0,4);              // Left
        case 0x51: return FK(8,1,1);              // Down
        case 0x52: return FK(8,0,2);              // Up
    }
    return FK_NONE;
}

// Put the currently selected row/column on the data lines. Call with famiMux held.
static inline void IRAM_ATTR famiDrive() {
    uint8_t nibble;
    if (!famiEnabled)      nibble = 0x0F; // Matrix disabled: all lines high (reads as 0)
    else if (famiRow >= 9) nibble = 0x00; // 10th row has no keys
    else                   nibble = famiMatrix[famiRow][famiCol];
    REG_WRITE(GPIO_OUT_W1TS_REG, famiPinMask[nibble]);
    REG_WRITE(GPIO_OUT_W1TC_REG, famiPinMask[nibble ^ 0x0F]);
}

// Fires on any change of OUT0/OUT1/OUT2: mirrors the keyboard's CD4017 counter.
void IRAM_ATTR famiStrobeISR() {
    uint32_t in = REG_READ(GPIO_IN_REG);
    bool reset = in & (1UL << FAMI_OUT0);
    bool col   = in & (1UL << FAMI_OUT1);
    portENTER_CRITICAL_ISR(&famiMux);
    if (reset) famiRow = 0;
    else if (famiLastCol && !col) famiRow = (famiRow + 1) % 10;
    famiLastCol = col;
    famiCol = col ? 1 : 0;
    famiEnabled = in & (1UL << FAMI_OUT2);
    famiDrive();
    portEXIT_CRITICAL_ISR(&famiMux);
}

void famiSetMatrix(const uint8_t newMatrix[9][2]) {
    portENTER_CRITICAL(&famiMux);
    for (int r = 0; r < 9; r++) {
        famiMatrix[r][0] = newMatrix[r][0];
        famiMatrix[r][1] = newMatrix[r][1];
    }
    famiDrive();
    portEXIT_CRITICAL(&famiMux);
}

void famiPress(uint8_t m[9][2], uint8_t fk) {
    if (fk == FK_NONE) return;
    m[fk >> 3][(fk >> 2) & 1] |= 1 << (fk & 3);
}

// The Famicom scans the whole matrix itself, so we just mirror what is held down.
void famiApplyReport(const uint8_t* pData, size_t length) {
    if (length < 3 || pData[2] == 0x01) return; // Phantom/rollover report: keep last state
    uint8_t m[9][2] = {{0}};
    uint8_t mods = pData[0];
    if (mods & 0x02) famiPress(m, FK_LSHIFT);
    if (mods & 0x20) famiPress(m, FK_RSHIFT);
    if (mods & 0x11) famiPress(m, FK_CTR);   // Either Ctrl
    if (mods & 0x04) famiPress(m, FK_GRPH);  // Left Alt
    if (mods & 0x40) famiPress(m, FK_KANA);  // Right Alt
    for (size_t i = 1; i < length; i++) {
        if (pData[i] != 0x00) famiPress(m, famiKeyForHID(pData[i]));
    }
    famiSetMatrix(m);
}

void famiReleaseAll() {
    uint8_t m[9][2] = {{0}};
    famiSetMatrix(m);
}

void famiBegin() {
    const int dataPins[4] = { FAMI_D1, FAMI_D2, FAMI_D3, FAMI_D4 };
    for (int n = 0; n < 16; n++) {
        famiPinMask[n] = 0;
        for (int b = 0; b < 4; b++) if (n & (1 << b)) famiPinMask[n] |= 1UL << dataPins[b];
    }
    for (int b = 0; b < 4; b++) pinMode(dataPins[b], OUTPUT);
    pinMode(FAMI_OUT0, INPUT);
    pinMode(FAMI_OUT1, INPUT);
    pinMode(FAMI_OUT2, INPUT);
    famiStrobeISR(); // Sync with the current line levels
    // Attached from setup() so the ISR runs on core 1, away from the BLE stack
    attachInterrupt(FAMI_OUT0, famiStrobeISR, CHANGE);
    attachInterrupt(FAMI_OUT1, famiStrobeISR, CHANGE);
    attachInterrupt(FAMI_OUT2, famiStrobeISR, CHANGE);
}

void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    if (targetMachine == TARGET_FAMICOM) {
        famiApplyReport(pData, length);
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
        // Don't leave keys stuck down on the Famicom
        if (targetMachine == TARGET_FAMICOM) famiReleaseAll();
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

        if (targetMachine == TARGET_FAMICOM) {
            Serial.println("Target machine: Famicom (Family BASIC keyboard)");
            famiBegin();
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
