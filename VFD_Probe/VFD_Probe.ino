/*
  Futaba GP1045A01A (256x64 VFD, board marking 1P00A293-01 Rev.C) - Arduino UNO pin discovery / test tool

  This is the sketch that was used to reverse-engineer the module's pinout and protocol (see README.md).
  It is slow (digitalWrite based) but easy to read and it prints what it does. Serial monitor: 115200 baud,
  line ending "Newline" (or "Both NL & CR").

  VERIFIED PINOUT (connector J1, 2x13 header, odd pins = one row, even pins = other row):
    J1 pin  1,3,5,7,9,11,13,15 -> Uno D2..D9   (pin 1 = data bit D7 ... pin 15 = D0: REVERSED order)
    J1 pin 17 = WR   -> A0     J1 pin 19 = C/D -> A1     (C/D: HIGH = command, LOW = data)
    J1 pin 21 = RD   -> A2     J1 pin 23 = CS  -> A3
    J1 pin 26 = SEL  -> A4     (HIGH / open = parallel interface, LOW = synchronous serial mode: keep it HIGH!)
    J1 pin 25 = INT  -> A5     (output: one ~75 us pulse per display frame, about every 10.3 ms)
    J1 pin 24 = not connected
    Even pins 2..16 = GND, pins 18/20/22 = +5 V.
    All signal pins have 10k pull-ups to VCC on the module (except INT). A 330 ohm series resistor in every
    Uno->module signal line is a good idea while experimenting. Power the module from a SEPARATE 5 V supply
    (1.2 A idle, several amps for a moment at switch-on) and connect the grounds.

  The command set is the one of the Futaba GP9002A01A (see its application note AN-E-3237A), with these
  differences found on this module:
    - 06H (clear screen) has no effect  -> clear the RAM by writing 2048 zero bytes (command 'z')
    - 13H (luminance) is inverted: 13H,00H switches the display off, 13H,FFH is full brightness
    - 15H (INT mode) does not change the INT pin
    - 00H = display off, 01H = display on, 14H,10H = display mode (no gray scale), 0EH/0FH = RAM address
      low/high, 08H = write data, 09H = read data (dummy read first)
    - RAM layout: address = x*8 + y/8 (x = 0..255, y = 0..63), bit7 = top pixel of the 8-pixel block

  COMMANDS (type 's' first, then try 'z', 't', 'P HELLO'):
    ?            help
    s            show the state of the lines and the INT pin
    z            clear RAM (writes zeros) and switch the display on (screen should be black)
    t            edge test: lines at the screen borders (shows address layout and bit order)
    P text       print text with the built-in 5x7 font (P 2 text = 2x size, P 3 text = 3x, '|' = new line)
    v            write/read verification without looking at the screen (clear, write 11..88, read back)
    R            RAM read test: tries the 4 possible CS/WR/C-D/RD assignments, prints the bytes read
    k HH [HH..]  send command byte(s) (C/D = HIGH)       y HH [HH..]  send data byte(s) (C/D = LOW)
    l n / h n    hold control line n LOW / release it HIGH (n = 1..5 = J1 pin 17, 19, 21, 23, 26)
    W n / S n    set which control line is WR / CS;  C n = C/D line (0 = none);  D n = RD line
    r            toggle data bit order (REVERSED = J1 pin 1 is D7 (default), STRAIGHT = pin 1 is D0)
    w            walk single data lines (display starts OFF; lines 15, 13 turn it on = commands 01H, 02H)
    p            walk all pairs of data lines (display ON + noise; shows which pair does what)
    m / M        watch pin 25 (INT) for 200 ms (m: input, M: with internal pull-up)
*/

#include <Arduino.h>

// ---- Hardware map ----
const uint8_t DP[8]   = {2, 3, 4, 5, 6, 7, 8, 9};      // DP[0] = J1 pin 1 ... DP[7] = J1 pin 15
const uint8_t CP[5]   = {A0, A1, A2, A3, A4};          // control lines (outputs, idle HIGH)
const uint8_t CPIN[5] = {17, 19, 21, 23, 26};          // J1 pin numbers of the control lines
const uint8_t INTP    = A5;                            // J1 pin 25 (input)

// indices into CP[] / CPIN[] (verified assignment)
enum { I_WR = 0, I_CD = 1, I_RD = 2, I_CS = 3, I_SEL = 4 };

// ---- Settings (changeable at run time) ----
uint8_t idxWR = I_WR;    // which CP[] entry acts as WR
uint8_t idxCS = I_CS;    // ... as CS
uint8_t idxCD = I_CD;    // ... as C/D (255 = none: every byte is written without touching C/D)
uint8_t idxRD = I_RD;    // ... as RD
bool    rev   = true;    // true: J1 pin 1 = D7 (the order that works)

// ---- Helpers ----
void allHigh() { for (uint8_t i = 0; i < 5; i++) digitalWrite(CP[i], HIGH); }

void putByte(uint8_t v) {
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t bit = rev ? (7 - i) : i;
    digitalWrite(DP[i], (v >> bit) & 1);
  }
}

bool intNow() { return digitalRead(INTP); }

// Raw data bus: bit i of mask = level of DP[i] (J1 pin 1+2i). The bit order is not reversed here.
void putRaw(uint8_t mask) { for (uint8_t i = 0; i < 8; i++) digitalWrite(DP[i], (mask >> i) & 1); }

// Write one byte that is already on the bus: CS low, WR low, WR high (the rising edge latches), CS high.
void strobeWrite() {
  delayMicroseconds(1);
  digitalWrite(CP[idxCS], LOW);
  delayMicroseconds(1);
  digitalWrite(CP[idxWR], LOW);
  delayMicroseconds(2);
  digitalWrite(CP[idxWR], HIGH);
  delayMicroseconds(1);
  digitalWrite(CP[idxCS], HIGH);
  delayMicroseconds(50);                               // conservative gap between bytes
}

void wrByte(uint8_t v) { putByte(v); strobeWrite(); }

// ---- C/D handling: command = HIGH, data = LOW (does nothing when no C/D line is assigned) ----
void cdWrite(bool level) { if (idxCD != 255) digitalWrite(CP[idxCD], level); }
void cmdB(uint8_t v) { cdWrite(HIGH); wrByte(v); }
void datB(uint8_t v) { cdWrite(LOW);  wrByte(v); cdWrite(HIGH); }
void setAddr(uint16_t a) { cmdB(0x0E); datB(a & 0xFF); cmdB(0x0F); datB(a >> 8); }

// Fill the whole RAM (command 06H does not clear on this module): 2048 bytes, the address auto-increments
void fillRam(uint8_t pat) {
  setAddr(0); cmdB(0x08);
  cdWrite(LOW);
  for (uint16_t i = 0; i < 2048; i++) wrByte(pat);
  cdWrite(HIGH);
}

// Write the 8 bytes of one screen column (x: 0..255). Address = x*8 + y/8
void writeColumn(uint16_t x, const uint8_t* d8) {
  setAddr(x * 8); cmdB(0x08);
  cdWrite(LOW);
  for (uint8_t i = 0; i < 8; i++) wrByte(d8[i]);
  cdWrite(HIGH);
}

// Edge test: clear RAM, vertical lines at the left/right edge, horizontal lines at the top/bottom, switch on.
// Byte 0 bit 0 and byte 7 bit 7 are drawn: if the bit order is the other way round the horizontal lines
// appear 7 pixels away from the border (that is how we found out that bit7 = TOP pixel).
void edgeTest() {
  cmdB(0x14); datB(0x10);          // display mode: no gray scale
  cmdB(0x00);                      // display off while writing
  fillRam(0x00);
  for (uint16_t x = 0; x < 256; x++) {
    uint8_t c[8] = {0x01, 0, 0, 0, 0, 0, 0, 0x80};
    if (x == 0 || x == 255) for (uint8_t i = 0; i < 8; i++) c[i] = 0xFF;
    writeColumn(x, c);
  }
  cmdB(0x01);                      // display on
}

void printState() {
  Serial.print(F("Control lines (J1 pin:level): "));
  for (uint8_t i = 0; i < 5; i++) {
    Serial.print(CPIN[i]); Serial.print(':'); Serial.print(digitalRead(CP[i])); Serial.print(' ');
  }
  Serial.print(F("| INT(pin 25)=")); Serial.println(intNow());
  Serial.print(F("WR=pin")); Serial.print(CPIN[idxWR]);
  Serial.print(F("  CS=pin")); Serial.print(CPIN[idxCS]);
  Serial.print(F("  C/D=")); if (idxCD == 255) Serial.print(F("none")); else { Serial.print(F("pin")); Serial.print(CPIN[idxCD]); }
  Serial.print(F("  RD=pin")); Serial.print(CPIN[idxRD]);
  Serial.print(F("  data order: ")); Serial.println(rev ? F("REVERSED (pin1=D7)") : F("STRAIGHT (pin1=D0)"));
}

// Watch pin 25 for 200 ms without touching any other line: count edges, high percentage, half period.
// On this module it is the frame (INT) output: ~75 us pulse every ~10.3 ms (GP9002A: 65 us every 8.4 ms).
void monitorInt(bool pullup) {
  pinMode(INTP, pullup ? INPUT_PULLUP : INPUT);
  delay(5);
  allHigh();
  uint32_t t0 = micros(), lastEdge = 0, minP = 0xFFFFFFFF, maxP = 0, hi = 0, n = 0, edges = 0;
  bool prev = intNow();
  while (micros() - t0 < 200000UL) {
    bool v = intNow(); n++; if (v) hi++;
    if (v != prev) {
      uint32_t now = micros() - t0;
      if (edges > 0) { uint32_t p = now - lastEdge; if (p < minP) minP = p; if (p > maxP) maxP = p; }
      lastEdge = now; edges++; prev = v;
    }
  }
  Serial.print(F("Pin 25 ")); Serial.print(pullup ? F("(pull-up ON) ") : F("(pull-up off) "));
  Serial.print(F("edges=")); Serial.print(edges);
  Serial.print(F(" HIGH%=")); Serial.print(n ? (hi * 100UL / n) : 0);
  if (edges > 1) { Serial.print(F(" half-period min/max us=")); Serial.print(minP); Serial.print('/'); Serial.print(maxP); }
  Serial.println();
  pinMode(INTP, INPUT);
}

// ---- Data line experiments (these are the tests used to decode the bus) ----

// Test 1: with the display OFF, set every data line HIGH on its own and write that as a COMMAND.
// GP9002A commands: bit0 = 01H "display on", bit1 = 02H "2nd screen on". On this module J1 pins 15 and 13
// switch the display on.
void walkSingles() {
  Serial.println(F("--- single line walk: the display should switch ON for J1 pin 15 (bit0) and pin 13 (bit1) ---"));
  for (uint8_t i = 0; i < 8; i++) {
    putRaw(0x00); strobeWrite();               // 00H: display off
    delay(700);
    Serial.print(F("J1 pin ")); Serial.print(1 + 2 * i); Serial.println(F(" HIGH, others LOW: writing command"));
    putRaw(1 << i); strobeWrite();
    delay(1500);
  }
  putRaw(0x00); strobeWrite();
  Serial.println(F("--- done (display is off) ---"));
}

// Test 2: with the display ON and showing noise, write every PAIR of data lines as a command.
// Each pair starts from a known state (00H = off, then 01H = on) so that mode commands do not pile up.
void walkPairs() {
  Serial.println(F("--- pair walk: note the pair where the noise changes / the display goes off ---"));
  for (uint8_t i = 0; i < 8; i++) for (uint8_t j = i + 1; j < 8; j++) {
    putRaw(0x00); strobeWrite(); delay(300);
    putRaw(0x80); strobeWrite(); delay(700);      // 0x80 = DP[7] = J1 pin 15 = command 01H (display on)
    Serial.print(F("pin ")); Serial.print(1 + 2 * i); Serial.print(F(" + pin ")); Serial.println(1 + 2 * j);
    putRaw((1 << i) | (1 << j)); strobeWrite();
    delay(1500);
  }
  Serial.println(F("--- done ---"));
}

void printHex2(uint8_t v) { if (v < 16) Serial.print('0'); Serial.print(v, HEX); }

// RAM READ test: write command 09H, then read with RD. If the module drives the bus you see changing bytes
// (the random power-on RAM content), otherwise a constant 0xFF/0x00. This was THE breakthrough: it proves
// which lines are CS/WR/C-D/RD without looking at the screen. 4 combinations are tried:
// (C/D,RD) in {(19,21),(21,19)} x (CS) in {pin 23, pin 17}. Bit i of a byte read = J1 pin 1+2i.
// Expected on this module: only "CS=pin23 WR=pin17 C/D=pin19 RD=pin21" returns changing bytes.
void readTest() {
  Serial.println(F("--- RAM read test (09H + RD). constant FF/00 => no path; changing bytes => PATH FOUND ---"));
  const uint8_t csList[2] = {idxCS, idxWR};
  for (uint8_t ci = 0; ci < 2; ci++) for (uint8_t p = 0; p < 2; p++) {
    uint8_t cs = csList[ci], wrI = (ci == 0) ? idxWR : idxCS;
    uint8_t cdI = p ? I_RD : I_CD, rdI = p ? I_CD : I_RD;     // p=0: C/D=19 RD=21 ; p=1: C/D=21 RD=19
    allHigh();
    // write command 09H (C/D HIGH), CS and WR as chosen for this combination
    putByte(0x09); delayMicroseconds(2);
    digitalWrite(CP[cs], LOW); digitalWrite(CP[wrI], LOW); delayMicroseconds(2);
    digitalWrite(CP[wrI], HIGH); delayMicroseconds(1); digitalWrite(CP[cs], HIGH); delayMicroseconds(5);
    for (uint8_t i = 0; i < 8; i++) pinMode(DP[i], INPUT);   // release the data bus
    digitalWrite(CP[cdI], LOW); delayMicroseconds(2);        // C/D LOW: data read
    Serial.print(F("CS=pin")); Serial.print(CPIN[cs]); Serial.print(F(" WR=pin")); Serial.print(CPIN[wrI]);
    Serial.print(F(" C/D=pin")); Serial.print(CPIN[cdI]); Serial.print(F(" RD=pin")); Serial.print(CPIN[rdI]); Serial.print(F(" :"));
    for (uint8_t k = 0; k < 14; k++) {
      digitalWrite(CP[cs], LOW); digitalWrite(CP[rdI], LOW); delayMicroseconds(2);
      uint8_t v = 0; for (uint8_t i = 0; i < 8; i++) v |= (uint8_t)digitalRead(DP[i]) << i;
      digitalWrite(CP[rdI], HIGH); digitalWrite(CP[cs], HIGH); delayMicroseconds(3);
      Serial.print(' '); printHex2(v);
    }
    Serial.println();
    allHigh();
    for (uint8_t i = 0; i < 8; i++) { pinMode(DP[i], OUTPUT); digitalWrite(DP[i], HIGH); }
    delay(100);
  }
  Serial.println(F("--- done ---"));
}

uint8_t revBits(uint8_t v) { uint8_t r = 0; for (uint8_t i = 0; i < 8; i++) if (v & (1 << i)) r |= 1 << (7 - i); return r; }

// Read n bytes from the RAM (the first byte read is a dummy and is not printed).
// The output is the LOGICAL value (converted according to the 'rev' setting).
void readRam(uint8_t n) {
  cmdB(0x09);
  delayMicroseconds(5);
  for (uint8_t i = 0; i < 8; i++) pinMode(DP[i], INPUT);
  cdWrite(LOW); delayMicroseconds(2);
  for (uint8_t k = 0; k <= n; k++) {
    digitalWrite(CP[idxCS], LOW); digitalWrite(CP[idxRD], LOW); delayMicroseconds(2);
    uint8_t v = 0; for (uint8_t i = 0; i < 8; i++) v |= (uint8_t)digitalRead(DP[i]) << i;
    digitalWrite(CP[idxRD], HIGH); digitalWrite(CP[idxCS], HIGH); delayMicroseconds(3);
    if (k == 0) continue;                       // dummy read
    Serial.print(' '); printHex2(rev ? revBits(v) : v);
  }
  Serial.println();
  cdWrite(HIGH);
  for (uint8_t i = 0; i < 8; i++) { pinMode(DP[i], OUTPUT); digitalWrite(DP[i], HIGH); }
}

// Write/read verification: proves the whole write path (data bus, C/D, addressing) without looking at the screen.
// NOTE: the "clear" line is expected to FAIL on this module (06H does not clear): you will still see noise there.
void verifyPath() {
  idxWR = I_WR; idxCS = I_CS; idxCD = I_CD; idxRD = I_RD; allHigh();
  Serial.println(F("--- write/read verification (CS=23 WR=17 C/D=19 RD=21) ---"));
  cmdB(0x14); datB(0x10);                       // display mode
  cmdB(0x06); delay(5);                         // clear (does nothing on this module)
  setAddr(0);
  Serial.print(F("after 06H clear (00 x8 would mean it works; it does not):"));  readRam(8);
  setAddr(0); cmdB(0x08);
  const uint8_t pat[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  for (uint8_t i = 0; i < 8; i++) datB(pat[i]);
  setAddr(0);
  Serial.print(F("after writing (expected 11 22 33 44 55 66 77 88):")); readRam(8);
  cmdB(0x01);
  Serial.println(F("--- done (display switched on) ---"));
}

// ---- 5x7 font (ASCII 32..90, column based, bit0 = top pixel). Lower case is converted to upper case ----
const uint8_t FONT5x7[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00, 0x14,0x7F,0x14,0x7F,0x14, // sp ! " #
  0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62, 0x36,0x49,0x56,0x20,0x50, 0x00,0x08,0x07,0x03,0x00, // $ % & '
  0x00,0x1C,0x22,0x41,0x00, 0x00,0x41,0x22,0x1C,0x00, 0x2A,0x1C,0x7F,0x1C,0x2A, 0x08,0x08,0x3E,0x08,0x08, // ( ) * +
  0x00,0x80,0x70,0x30,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x00,0x60,0x60,0x00, 0x20,0x10,0x08,0x04,0x02, // , - . /
  0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00, 0x72,0x49,0x49,0x49,0x46, 0x21,0x41,0x49,0x4D,0x33, // 0 1 2 3
  0x18,0x14,0x12,0x7F,0x10, 0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x31, 0x41,0x21,0x11,0x09,0x07, // 4 5 6 7
  0x36,0x49,0x49,0x49,0x36, 0x46,0x49,0x49,0x29,0x1E, 0x00,0x00,0x14,0x00,0x00, 0x00,0x40,0x34,0x00,0x00, // 8 9 : ;
  0x00,0x08,0x14,0x22,0x41, 0x14,0x14,0x14,0x14,0x14, 0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x59,0x09,0x06, // < = > ?
  0x3E,0x41,0x5D,0x59,0x4E, 0x7C,0x12,0x11,0x12,0x7C, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22, // @ A B C
  0x7F,0x41,0x41,0x41,0x3E, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01, 0x3E,0x41,0x41,0x51,0x73, // D E F G
  0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00, 0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41, // H I J K
  0x7F,0x40,0x40,0x40,0x40, 0x7F,0x02,0x1C,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E, // L M N O
  0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46, 0x26,0x49,0x49,0x49,0x32, // P Q R S
  0x03,0x01,0x7F,0x01,0x03, 0x3F,0x40,0x40,0x40,0x3F, 0x1F,0x20,0x40,0x20,0x1F, 0x3F,0x40,0x38,0x40,0x3F, // T U V W
  0x63,0x14,0x08,0x14,0x63, 0x03,0x04,0x78,0x04,0x03, 0x61,0x59,0x49,0x4D,0x43                           // X Y Z
};

// Write one byte at (column x, block row). Uses the verified cmdB/datB path.
void putColB(uint16_t x, uint8_t row, uint8_t b) {
  setAddr(x * 8 + row);
  cmdB(0x08); datB(b);
}

// One character: x in pixels, top block row 'row', scale s (1..3). Each source column produces s block bytes.
void drawChar(uint16_t x, uint8_t row, char ch, uint8_t s) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  if (ch < 32 || ch > 90) ch = '?';
  const uint8_t* g = FONT5x7 + (uint16_t)(ch - 32) * 5;
  for (uint8_t gc = 0; gc < 6; gc++) {                      // 5 columns + 1 spacing column
    uint8_t col = (gc < 5) ? pgm_read_byte(g + gc) : 0;
    for (uint8_t sx = 0; sx < s; sx++) {
      uint16_t xx = x + gc * s + sx;
      if (xx > 255) return;
      for (uint8_t blk = 0; blk < s; blk++) {
        uint8_t out = 0;
        for (uint8_t j = 0; j < 8; j++) {
          uint8_t srcRow = (blk * 8 + j) / s;               // 0..7 source row
          if (srcRow < 8 && (col & (1 << srcRow))) out |= (0x80 >> j);   // bit7 = top
        }
        if (row + blk < 8) putColB(xx, row + blk, out);
      }
    }
  }
}

void drawText(const char* s, uint8_t sc) {
  cmdB(0x14); datB(0x10); cmdB(0x00); fillRam(0x00);
  uint16_t x = 0; uint8_t row = 0;
  for (; *s; s++) {
    if (*s == '|' || x + 6 * sc > 256) { x = 0; row += sc; if (row >= 8) break; if (*s == '|') continue; }
    drawChar(x, row, *s, sc);
    x += 6 * sc;
  }
  cmdB(0x01);
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F(" ?          help                            s    state of the lines"));
  Serial.println(F(" z          clear RAM + display on          t    edge test (border lines)"));
  Serial.println(F(" P text     print text (P 2 text = 2x, P 3 text = 3x, '|' = new line)"));
  Serial.println(F(" v          write/read verification         R    RAM read test (finds CS/WR/C-D/RD)"));
  Serial.println(F(" k HH..     command byte(s) (C/D=H)         y HH..  data byte(s) (C/D=L)"));
  Serial.println(F(" l n / h n  hold line n LOW / release HIGH (n=1..5 = J1 pin 17,19,21,23,26)"));
  Serial.println(F(" W n / S n  WR line / CS line   C n  C/D line (0=none)   D n  RD line"));
  Serial.println(F(" r          toggle data bit order           m / M  watch pin 25 (INT)"));
  Serial.println(F(" w          single data line walk (display starts OFF)"));
  Serial.println(F(" p          data line pair walk (display ON + noise)"));
  Serial.println(F("NOTE: pin 25 = INT (frame pulse), pin 26 = SEL (keep HIGH / open! LOW = serial mode)"));
}

// ---- Serial command parsing ----
char lineBuf[64];

uint8_t hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 255;
}

// parse hex bytes from arg and send them as commands (cmd = true) or data (cmd = false)
void sendHexBytes(char* arg, bool cmd) {
  uint8_t n = 0;
  while (*arg) {
    while (*arg == ' ') arg++;
    if (hexVal(arg[0]) == 255) break;
    uint8_t v = hexVal(arg[0]); arg++;
    if (hexVal(*arg) != 255) { v = (v << 4) | hexVal(*arg); arg++; }
    if (cmd) cmdB(v); else datB(v);
    n++;
  }
  Serial.print(n); Serial.println(cmd ? F(" command byte(s) sent") : F(" data byte(s) sent"));
}

// "W n" style role assignment: n = 1..5 selects CP[n-1]; the role must not collide with the other roles.
bool assignRole(uint8_t &role, int n) {
  if (n < 1 || n > 5) { Serial.println(F("invalid")); return false; }
  role = n - 1;
  printState();
  return true;
}

void doLine(char* s) {
  while (*s == ' ') s++;
  char c = *s;
  if (!c) return;
  char* arg = s + 1; while (*arg == ' ') arg++;

  switch (c) {
    case '?': printHelp(); break;
    case 's': printState(); break;
    case 'z': cmdB(0x14); datB(0x10); cmdB(0x00); fillRam(0x00); cmdB(0x01); Serial.println(F("RAM cleared, display on (should be black)")); break;
    case 't': edgeTest(); Serial.println(F("edge test sent: expect thin lines along all four screen borders")); break;
    case 'P': {
      uint8_t sc = 1;
      if (*arg >= '1' && *arg <= '3' && arg[1] == ' ') { sc = *arg - '0'; arg += 2; }
      drawText(arg, sc);
      Serial.println(F("text written"));
    } break;
    case 'v': verifyPath(); break;
    case 'R': readTest(); break;
    case 'k': sendHexBytes(arg, true); break;
    case 'y': sendHexBytes(arg, false); break;
    case 'l': { int n = atoi(arg); if (n >= 1 && n <= 5) { digitalWrite(CP[n - 1], LOW); printState(); } } break;
    case 'h': { int n = atoi(arg); if (n >= 1 && n <= 5) { digitalWrite(CP[n - 1], HIGH); printState(); } } break;
    case 'W': assignRole(idxWR, atoi(arg)); break;
    case 'S': assignRole(idxCS, atoi(arg)); break;
    case 'D': assignRole(idxRD, atoi(arg)); break;
    case 'C': { int n = atoi(arg); if (n == 0) { idxCD = 255; printState(); } else assignRole(idxCD, n); } break;
    case 'r': rev = !rev; printState(); break;
    case 'w': walkSingles(); break;
    case 'p': walkPairs(); break;
    case 'm': monitorInt(false); break;
    case 'M': monitorInt(true); break;
    default: Serial.println(F("? for help")); break;
  }
}

void setup() {
  Serial.begin(115200);
  for (uint8_t i = 0; i < 8; i++) { pinMode(DP[i], OUTPUT); digitalWrite(DP[i], HIGH); }
  for (uint8_t i = 0; i < 5; i++) { pinMode(CP[i], OUTPUT); digitalWrite(CP[i], HIGH); }
  pinMode(INTP, INPUT);               // pin 25: no pull-up on the module, the VFD drives it
  delay(300);
  Serial.println(F("\nFutaba GP1045A01A probe ready."));
  printState();
  printHelp();
}

void loop() {
  static uint8_t n = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') { lineBuf[n] = 0; n = 0; doLine(lineBuf); }
    else if (n < sizeof(lineBuf) - 1) lineBuf[n++] = ch;
  }
}
