/*
  ASTEROIDS - Futaba GP1045A01A (256x64 VFD) + Arduino UNO

  VFD WIRING (pinout verified by experiment):
    Connector pins 1,3,5,7,9,11,13,15 -> D2..D9   (pin 1 = D7 ... pin 15 = D0, i.e. REVERSED order)
    pin 17 WR -> A0,  pin 19 C/D -> A1,  pin 21 RD -> A2 (keep HIGH while writing),  pin 23 CS -> A3
    pin 26 SEL -> A4 (HIGH),  pin 25 INT -> A5 (unused),  pin 24 not connected
    Even pins 2-16 = GND, pins 18/20/22 = +5 V (separate supply!), all grounds common.
    (RD and SEL are pulled up on the module itself, so they do not strictly need an Uno pin. INT is an output.)

  CONTROLS (buttons to GND, internal pull-ups):
    D10 = LEFT   D11 = RIGHT   D12 = THRUST   D13 = FIRE
    You can also play from the serial monitor (send with Enter): a = left, d = right, w = thrust, f = fire
    With no input the AI plays a DEMO; any button/key starts a real game.

  VFD RAM: address = x*8 + y/8, bit7 = top pixel.  Command 06H (clear) does NOT work on this module,
  so the RAM is cleared by writing zeros.
  A full frame buffer (2 KB) does not fit into the Uno, so the screen is drawn and written in
  8 STRIPS of 32 columns.
*/

#include <Arduino.h>
#include <avr/pgmspace.h>
#include <math.h>
#include <stdlib.h>
#include <util/delay_basic.h>

#define VERSION "0.1"          // shown on the attract (demo) screen as "Ver:0.1"
#define STRIP_SKIP 1           // 1: do not rewrite strips that did not change. Set to 0 if the display misbehaves.

// ======================= Low-level VFD access (direct port I/O) =======================
#define VFD_WR PC0      // A0
#define VFD_CD PC1      // A1
#define VFD_RD PC2      // A2
#define VFD_CS PC3      // A3
#define VFD_SEL PC4     // A4 (kept HIGH by vfdInit)

static inline uint8_t rev8(uint8_t v) {
  v = ((v & 0xF0) >> 4) | ((v & 0x0F) << 4);
  v = ((v & 0xCC) >> 2) | ((v & 0x33) << 2);
  v = ((v & 0xAA) >> 1) | ((v & 0x55) << 1);
  return v;
}

// D2..D7 = PD2..PD7, D8..D9 = PB0..PB1. Connector pin 1 (D2) = VFD D7 ... pin 15 (D9) = VFD D0
static inline void busWrite(uint8_t v) {
  uint8_t r = rev8(v);
  PORTD = (PORTD & 0x03) | ((r & 0x3F) << 2);
  PORTB = (PORTB & 0xFC) | (r >> 6);
}

static inline __attribute__((always_inline)) void strobe() {
  PORTC &= ~_BV(VFD_CS);
  PORTC &= ~_BV(VFD_WR);
  __asm__ __volatile__("nop\n\tnop\n\tnop\n\t");
  PORTC |= _BV(VFD_WR);          // rising edge latches the byte
  PORTC |= _BV(VFD_CS);
}

void vcmd(uint8_t c) { PORTC |= _BV(VFD_CD);  busWrite(c); delayMicroseconds(1); strobe(); delayMicroseconds(1); }
void vdat(uint8_t d) { PORTC &= ~_BV(VFD_CD); busWrite(d); delayMicroseconds(1); strobe(); delayMicroseconds(1); }

void setAddr(uint16_t a) { vcmd(0x0E); vdat(a & 0xFF); vcmd(0x0F); vdat(a >> 8); }

// ======================= Strip buffer =======================
// Strip bytes are stored DIRECTLY IN PIN LAYOUT: byte = [PD7..PD2 | PB1 PB0]. Pixel row y (0 = top)
// therefore maps to bit (y+2)&7: rows 0..5 -> bits 2..7 (PORTD), rows 6,7 -> bits 0,1 (PORTB).
// No shifting or bit reversal is needed when writing: PORTD = v & 0xFC, PORTB = v & 0x03
// (PD0/PD1 = UART pins are left untouched).
uint8_t strip[256];            // 32 columns x 8 bytes: index = (x - sx)*8 + y/8
uint8_t sx;                    // first column of the current strip
const uint8_t BM[8] = {0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02};   // pixel mask for row y&7

// Extra wait between byte writes (3 CPU cycles per step). Writing too fast makes the module drop bytes
// (the picture turns to garbage). MEASURED: flushDelay=0 (~1.6 us/byte) is BROKEN, flushDelay=2 (~1.9 us/byte)
// works. The default is 5 (~2.5 us/byte, ~30 % safety margin).
// Use ']' on the serial monitor to speed up and '[' to slow down (go back with '[' if the picture breaks).
uint8_t flushDelay = 5;

void flushStrip(uint8_t s) {
  setAddr((uint16_t)s * 256);
  vcmd(0x08);
  PORTC &= ~_BV(VFD_CD);
  uint8_t keepD = PORTD & 0x03, keepB = PORTB & 0xFC;      // keep the serial pins / buttons untouched
  uint8_t d = flushDelay;
  const uint8_t *p = strip;
  for (uint16_t i = 0; i < 256; i++) {
    uint8_t v = *p++;
    PORTD = keepD | (v & 0xFC);
    PORTB = keepB | (v & 0x03);
    strobe();
    if (d) _delay_loop_1(d);                                // d * 3 cycles
  }
}

static inline void plot(int16_t x, int16_t y) {
  if ((uint16_t)(x - sx) < 32 && (uint16_t)y < 64)
    strip[((uint8_t)(x - sx) << 3) + (y >> 3)] |= BM[y & 7];
}

// Bresenham line (clipped to the current strip by plot())
void line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  int16_t lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0;
  if (hi < sx || lo >= sx + 32) return;
  if ((y0 < 0 && y1 < 0) || (y0 > 63 && y1 > 63)) return;
  int16_t dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int8_t stx = x0 < x1 ? 1 : -1, sty = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;
  for (;;) {
    plot(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += stx; }
    if (e2 <= dx) { err += dx; y0 += sty; }
  }
}

static inline bool vis(int16_t cx, int16_t cy, int16_t r) {
  return cx + r >= sx && cx - r < sx + 32 && cy + r >= 0 && cy - r < 64;
}

// ======================= Trigonometry =======================
int8_t sinTab[256];
static inline int8_t SIN(uint8_t a) { return sinTab[a]; }
static inline int8_t COS(uint8_t a) { return sinTab[(uint8_t)(a + 64)]; }
// angle 0 = up, increasing = clockwise (256 steps per turn)
static inline void polar(int16_t cx, int16_t cy, uint8_t a, int8_t r, int16_t &x, int16_t &y) {
  x = cx + (((int16_t)SIN(a) * r) >> 7);
  y = cy - (((int16_t)COS(a) * r) >> 7);
}

// ======================= 5x7 font (ASCII 32..122, column based, bit0 = top) =======================
const uint8_t FONT5x7[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00, 0x14,0x7F,0x14,0x7F,0x14,
  0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62, 0x36,0x49,0x56,0x20,0x50, 0x00,0x08,0x07,0x03,0x00,
  0x00,0x1C,0x22,0x41,0x00, 0x00,0x41,0x22,0x1C,0x00, 0x2A,0x1C,0x7F,0x1C,0x2A, 0x08,0x08,0x3E,0x08,0x08,
  0x00,0x80,0x70,0x30,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x00,0x60,0x60,0x00, 0x20,0x10,0x08,0x04,0x02,
  0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00, 0x72,0x49,0x49,0x49,0x46, 0x21,0x41,0x49,0x4D,0x33,
  0x18,0x14,0x12,0x7F,0x10, 0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x31, 0x41,0x21,0x11,0x09,0x07,
  0x36,0x49,0x49,0x49,0x36, 0x46,0x49,0x49,0x29,0x1E, 0x00,0x00,0x14,0x00,0x00, 0x00,0x40,0x34,0x00,0x00,
  0x00,0x08,0x14,0x22,0x41, 0x14,0x14,0x14,0x14,0x14, 0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x59,0x09,0x06,
  0x3E,0x41,0x5D,0x59,0x4E, 0x7C,0x12,0x11,0x12,0x7C, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
  0x7F,0x41,0x41,0x41,0x3E, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01, 0x3E,0x41,0x41,0x51,0x73,
  0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00, 0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41,
  0x7F,0x40,0x40,0x40,0x40, 0x7F,0x02,0x1C,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
  0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46, 0x26,0x49,0x49,0x49,0x32,
  0x03,0x01,0x7F,0x01,0x03, 0x3F,0x40,0x40,0x40,0x3F, 0x1F,0x20,0x40,0x20,0x1F, 0x3F,0x40,0x38,0x40,0x3F,
  0x63,0x14,0x08,0x14,0x63, 0x03,0x04,0x78,0x04,0x03, 0x61,0x59,0x49,0x4D,0x43,                          // X Y Z
  0x00,0x7F,0x41,0x41,0x00, 0x02,0x04,0x08,0x10,0x20, 0x00,0x41,0x41,0x7F,0x00, 0x04,0x02,0x01,0x02,0x04, // [ \ ] ^
  0x40,0x40,0x40,0x40,0x40, 0x00,0x01,0x02,0x04,0x00,                                                    // _ `
  0x20,0x54,0x54,0x54,0x78, 0x7F,0x48,0x44,0x44,0x38, 0x38,0x44,0x44,0x44,0x28, 0x38,0x44,0x44,0x48,0x7F, // a b c d
  0x38,0x54,0x54,0x54,0x18, 0x08,0x7E,0x09,0x01,0x02, 0x08,0x14,0x54,0x54,0x3C, 0x7F,0x08,0x04,0x04,0x78, // e f g h
  0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x44,0x3D,0x00, 0x7F,0x10,0x28,0x44,0x00, 0x00,0x41,0x7F,0x40,0x00, // i j k l
  0x7C,0x04,0x78,0x04,0x78, 0x7C,0x08,0x04,0x04,0x78, 0x38,0x44,0x44,0x44,0x38, 0x7C,0x14,0x14,0x14,0x08, // m n o p
  0x08,0x14,0x14,0x18,0x7C, 0x7C,0x08,0x04,0x04,0x08, 0x48,0x54,0x54,0x54,0x20, 0x04,0x3F,0x44,0x40,0x20, // q r s t
  0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C, 0x3C,0x40,0x30,0x40,0x3C, 0x44,0x28,0x10,0x28,0x44, // u v w x
  0x0C,0x50,0x50,0x50,0x3C, 0x44,0x64,0x54,0x4C,0x44                                                     // y z
};

// Doubles a group of 4 pixel rows vertically (bit r -> rows 2r and 2r+1) in strip (pin) layout:
// DBLR[n] = rot2(DBL[n]) with DBL = {00,03,0C,0F,30,33,3C,3F,C0,C3,CC,CF,F0,F3,FC,FF}. No division, no loops.
const uint8_t DBLR[16] = {0x00, 0x0C, 0x30, 0x3C, 0xC0, 0xCC, 0xF0, 0xFC, 0x03, 0x0F, 0x33, 0x3F, 0xC3, 0xCF, 0xF3, 0xFF};

// Text into the strip buffer: x in pixels, yb = block row (0..7), scale sc (1 or 2)
void textStrip(int16_t x, uint8_t yb, const char *s, uint8_t sc) {
  uint8_t adv = 6 * sc;
  for (; *s; s++, x += adv) {
    if (x + adv <= sx || x >= sx + 32) continue;         // not in this strip: skip
    char ch = *s;
    if (ch < 32 || ch > 122) ch = '?';                   // ASCII 32..122 (lower case included)
    const uint8_t *g = FONT5x7 + (uint16_t)(ch - 32) * 5;
    for (uint8_t gc = 0; gc < 5; gc++) {
      uint8_t col = pgm_read_byte(g + gc);                // font: bit0 = top pixel (7 rows)
      if (sc == 1) {
        int16_t xx = x + gc;
        if ((uint16_t)(xx - sx) < 32) strip[((uint8_t)(xx - sx) << 3) + yb] |= (uint8_t)((col << 2) | (col >> 6));   // rotate into strip (pin) layout
      } else {                                            // 2x: upper/lower block bytes come from a table
        uint8_t o0 = DBLR[col & 15], o1 = DBLR[col >> 4];
        for (uint8_t k = 0; k < 2; k++) {
          int16_t xx = x + gc * 2 + k;
          if ((uint16_t)(xx - sx) >= 32) continue;
          uint8_t *p = &strip[((uint8_t)(xx - sx) << 3) + yb];
          p[0] |= o0;
          if (yb + 1 < 8) p[1] |= o1;
        }
      }
    }
  }
}

// Wrap-around copies: for an object centred at c (0..M-1) with radius R, which copies (-1, 0, +1) are needed?
// Not all nine - only the ones for objects close to an edge (usually just one).
static inline void wrapRange(int16_t c, int16_t R, int16_t M, int8_t &lo, int8_t &hi) {
  lo = (c >= M - R) ? -1 : 0;
  hi = (c < R) ? 1 : 0;
}

// ======================= Game data =======================
// Positions are in 1/16 pixel; x: 0..4095, y: 0..1023 (powers of two => wrap-around with a mask)
#define XM 0x0FFF
#define YM 0x03FF

struct Ship {
  int32_t x, y;           // 1/256 pixel (x: 0..65535, y: 0..16383)
  int16_t vx, vy;         // 1/256 pixel per frame
  uint8_t ang;
  bool alive;
  uint8_t invul, respawn, cool;
} ship;

struct Ast { int16_t x, y, vx, vy; uint8_t sz, shape; };      // sz 0xFF = free slot
struct Bul { int16_t x, y, vx, vy; uint8_t life; };
struct Prt { int16_t x, y, vx, vy; uint8_t life; };
#define MAXA 24
#define MAXB 4
#define MAXP 24
Ast ast[MAXA];
Bul bul[MAXB];
Prt prt[MAXP];

// ---- UFO (like the original: large = random fire, small = aimed fire) ----
struct Ufo { int16_t x, y, vx, vy; bool active, small; uint8_t vyTimer, fireTimer; } ufo;
Bul ub[2];                       // UFO bullets
uint16_t ufoTimer;               // frames until the next UFO

// 3 shapes x 8 vertex radii (large asteroid, in pixels)
const uint8_t SHAPE[3][8] PROGMEM = {
  {12, 9, 12, 10, 13, 9, 11, 10},
  {10, 13, 9, 12, 10, 12, 8, 11},
  {13, 10, 11, 13, 9, 11, 12, 9}
};
const uint8_t COLR[3] = {11, 5, 3};          // collision radius per size
const uint16_t PTS[3] = {20, 50, 100};       // points per size

enum { ST_DEMO, ST_PLAY, ST_OVER };
uint8_t state = ST_DEMO;
uint16_t score;
uint8_t lives, wave;
uint8_t overTimer;
uint32_t frameN;
bool displayOn = false;

// ---- input ----
struct In { bool l, r, t, f; } in;
uint8_t kl, kr, kt, kf;          // serial key "hold" counters
bool anyInput;

void readInput() {
  in.l = !digitalRead(10); in.r = !digitalRead(11); in.t = !digitalRead(12); in.f = !digitalRead(13);
  bool got = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == ']' || c == '[') {                          // write speed adjustment (does not start the game)
      if (c == ']') flushDelay = (flushDelay >= 2) ? flushDelay - 2 : 0;
      else flushDelay = (flushDelay <= 250) ? flushDelay + 2 : 252;
      Serial.print(F("flushDelay=")); Serial.print(flushDelay);
      Serial.print(F(" (~")); Serial.print((25 + 3 * (uint16_t)flushDelay) * 10UL / 16); Serial.println(F(" x0.1us/byte)"));
      continue;
    }
    if (c == 'a') kl = 6; else if (c == 'd') kr = 6; else if (c == 'w') kt = 6; else if (c == 'f' || c == ' ') kf = 6;
    if (c > ' ') got = true;
  }
  if (kl) { in.l = true; kl--; }
  if (kr) { in.r = true; kr--; }
  if (kt) { in.t = true; kt--; }
  if (kf) { in.f = true; kf--; }
  anyInput = got || in.l || in.r || in.t || in.f;
}

// ---- helpers ----
int16_t wrapD(int16_t d, int16_t M) { int16_t h = M >> 1; if (d > h) d -= M; else if (d < -h) d += M; return d; }
// squared distance in pixel coordinates, wrap-around aware
int16_t dist2(int16_t ax, int16_t ay, int16_t bx, int16_t by) {
  int16_t dx = wrapD(ax - bx, 256), dy = wrapD(ay - by, 64);
  return dx * dx + dy * dy;
}

void debris(int16_t x, int16_t y, uint8_t n) {
  for (uint8_t i = 0; i < MAXP && n; i++) if (!prt[i].life) {
    prt[i].x = x; prt[i].y = y;
    uint8_t a = random(256), sp = random(6, 22);
    prt[i].vx = ((int16_t)SIN(a) * sp) >> 7; prt[i].vy = -(((int16_t)COS(a) * sp) >> 7);
    prt[i].life = random(10, 26);
    n--;
  }
}

void randVel(uint8_t sz, int16_t &vx, int16_t &vy) {
  static const uint8_t lo[3] = {4, 8, 12}, hi[3] = {12, 18, 26};
  uint8_t a = random(256), sp = random(lo[sz], hi[sz] + 1);
  vx = ((int16_t)SIN(a) * sp) >> 7; vy = -(((int16_t)COS(a) * sp) >> 7);
}

int8_t freeAst() { for (uint8_t i = 0; i < MAXA; i++) if (ast[i].sz == 0xFF) return i; return -1; }

void addAst(int16_t x, int16_t y, uint8_t sz) {
  int8_t j = freeAst(); if (j < 0) return;
  ast[j].x = x; ast[j].y = y; ast[j].sz = sz; ast[j].shape = random(3);
  randVel(sz, ast[j].vx, ast[j].vy);
}

void newWave() {
  wave++;
  uint8_t n = 3 + wave; if (n > 6) n = 6;
  int16_t shx = (int16_t)(ship.x >> 8), shy = (int16_t)(ship.y >> 8);
  for (uint8_t i = 0; i < n; i++) {
    int16_t px, py; uint8_t tries = 0;
    do { px = random(256); py = random(64); tries++; } while (dist2(px, py, shx, shy) < 2200 && tries < 30);   // not too close to the ship
    addAst(px << 4, py << 4, 0);
  }
}

void shipRespawn() {
  ship.x = 128L << 8; ship.y = 32L << 8; ship.vx = ship.vy = 0; ship.ang = 0;
  ship.alive = true; ship.invul = 90; ship.cool = 0;
}

void resetGame(uint8_t st) {
  state = st; score = 0; lives = 3; wave = 0; overTimer = 0;
  for (uint8_t i = 0; i < MAXA; i++) ast[i].sz = 0xFF;
  for (uint8_t i = 0; i < MAXB; i++) bul[i].life = 0;
  for (uint8_t i = 0; i < MAXP; i++) prt[i].life = 0;
  ufo.active = false; ufoTimer = random(180, 360);
  for (uint8_t u = 0; u < 2; u++) ub[u].life = 0;
  shipRespawn();
  ship.invul = (st == ST_DEMO) ? 255 : 90;               // 255 = invulnerable forever (demo)
  newWave();
}

void breakAst(uint8_t i, bool award) {
  Ast a = ast[i];
  ast[i].sz = 0xFF;
  if (award && state != ST_OVER) score += PTS[a.sz];
  debris(a.x, a.y, 5 + (2 - a.sz) * 2);
  if (a.sz < 2) { addAst(a.x, a.y, a.sz + 1); addAst(a.x, a.y, a.sz + 1); }
}

void killShip() {
  ship.alive = false;
  debris((int16_t)(ship.x >> 4), (int16_t)(ship.y >> 4), 14);
  if (state == ST_PLAY) {
    if (lives > 0) lives--;
    if (lives == 0) { state = ST_OVER; overTimer = 120; }
    else ship.respawn = 50;
  } else ship.respawn = 20;
}

void fireBullet() {
  for (uint8_t i = 0; i < MAXB; i++) if (!bul[i].life) {
    int16_t spx = (int16_t)(ship.x >> 8), spy = (int16_t)(ship.y >> 8);
    int16_t nx = spx + (((int16_t)SIN(ship.ang) * 7) >> 7), ny = spy - (((int16_t)COS(ship.ang) * 7) >> 7);
    bul[i].x = (nx << 4) & XM; bul[i].y = (ny << 4) & YM;
    bul[i].vx = (ship.vx >> 4) + (((int16_t)SIN(ship.ang) * 64) >> 7);
    bul[i].vy = (ship.vy >> 4) - (((int16_t)COS(ship.ang) * 64) >> 7);
    bul[i].life = 45;
    ship.cool = 9;
    return;
  }
}

// ---- engine sparks (behind the ship while thrusting) ----
void spark() {
  for (uint8_t i = 0; i < MAXP; i++) if (!prt[i].life) {
    uint8_t a = ship.ang + 128 + (int8_t)random(-14, 15);          // narrow cone pointing backwards
    uint8_t sp = random(14, 28);
    prt[i].x = ((int16_t)(ship.x >> 4) + SIN(ship.ang + 128)) & XM;  // ~8 px behind the ship centre
    prt[i].y = ((int16_t)(ship.y >> 4) - COS(ship.ang + 128)) & YM;
    prt[i].vx = (ship.vx >> 4) + (((int16_t)SIN(a) * sp) >> 7);
    prt[i].vy = (ship.vy >> 4) - (((int16_t)COS(a) * sp) >> 7);
    prt[i].life = random(5, 10);
    return;
  }
}

// ---- UFO ----
void spawnUfo() {
  uint16_t chance = score / 150; if (chance > 60) chance = 60;         // small UFOs appear more often as the score rises
  ufo.small = (score >= 2500) && (random(100) < chance);
  bool fromLeft = random(2);
  ufo.x = fromLeft ? -12 * 16 : 268 * 16;
  ufo.vx = (ufo.small ? 36 : 28) * (fromLeft ? 1 : -1);
  ufo.y = (int16_t)random(10, 54) << 4; ufo.vy = 0;
  ufo.vyTimer = random(30, 70); ufo.fireTimer = random(25, 50);
  ufo.active = true;
}
void ufoKill() {
  if (state == ST_PLAY) { score += ufo.small ? 1000 : 200; }
  debris(ufo.x & XM, ufo.y, 12);
  ufo.active = false; ufoTimer = random(250, 520);
}
void ufoFire() {
  if (ufo.x < 0 || ufo.x > 4095) return;                                   // do not fire while off screen
  for (uint8_t u = 0; u < 2; u++) if (!ub[u].life) {
    uint8_t a = random(256);                                             // large UFO: random direction
    if (ufo.small && ship.alive) {                                       // small UFO: aim at the ship
      int16_t tx = wrapD((int16_t)(ship.x >> 8) - (ufo.x >> 4), 256), ty = wrapD((int16_t)(ship.y >> 8) - (ufo.y >> 4), 64);
      int32_t bestDot = -2147483647L; uint8_t ba = 0;
      for (uint16_t k = 0; k < 256; k++) {                               // find the angle that points best at the ship
        int32_t d = (int32_t)SIN(k) * tx - (int32_t)COS(k) * ty;
        if (d > bestDot) { bestDot = d; ba = k; }
      }
      uint8_t err = score < 3000 ? 28 : (score < 8000 ? 14 : 5);         // aim error shrinks as the score rises
      a = ba + (int8_t)random(-(int8_t)err, (int8_t)err + 1);
    }
    ub[u].x = (ufo.x & XM); ub[u].y = ufo.y;
    ub[u].vx = ((int16_t)SIN(a) * 56) >> 7; ub[u].vy = -(((int16_t)COS(a) * 56) >> 7);
    ub[u].life = 60;
    return;
  }
}
void stepUfo() {
  ufo.x += ufo.vx;
  ufo.y = (ufo.y + ufo.vy) & YM;
  if (ufo.x < -320 || ufo.x > 4416) { ufo.active = false; ufoTimer = random(250, 520); return; }   // left through the far side
  if (--ufo.vyTimer == 0) {                                              // change vertical direction at random
    ufo.vyTimer = random(30, 70);
    uint8_t r = random(3); ufo.vy = (r == 0) ? -14 : (r == 1 ? 0 : 14);
  }
  if (--ufo.fireTimer == 0) { ufo.fireTimer = ufo.small ? random(30, 55) : random(45, 90); ufoFire(); }
}

// DEMO AI: turn towards the nearest asteroid (or a nearby UFO) and fire
void aiInput() {
  in.l = in.r = in.t = in.f = false;
  int16_t shx = (int16_t)(ship.x >> 8), shy = (int16_t)(ship.y >> 8);
  int16_t best = 32767; int8_t bi = -1;
  for (uint8_t i = 0; i < MAXA; i++) if (ast[i].sz != 0xFF) {
    int16_t d = dist2(ast[i].x >> 4, ast[i].y >> 4, shx, shy);
    if (d < best) { best = d; bi = i; }
  }
  in.t = (frameN % 110) < 14;                           // thrust now and then so the engine flame shows up in the demo
  int16_t tgx, tgy;
  if (ufo.active && dist2(ufo.x >> 4, ufo.y >> 4, shx, shy) < 9000) { tgx = ufo.x >> 4; tgy = ufo.y >> 4; }
  else if (bi >= 0) { tgx = ast[bi].x >> 4; tgy = ast[bi].y >> 4; }
  else return;
  int16_t tx = wrapD(tgx - shx, 256), ty = wrapD(tgy - shy, 64);
  int16_t hx = SIN(ship.ang), hy = -COS(ship.ang);
  int32_t cross = (int32_t)hx * ty - (int32_t)hy * tx;
  int32_t dot = (int32_t)hx * tx + (int32_t)hy * ty;
  int32_t mag = (int32_t)(abs(tx) + abs(ty)) * 127;
  if (cross > mag / 10) in.r = true; else if (cross < -mag / 10) in.l = true;
  if (dot > 0 && abs(cross) < mag / 6 && (frameN & 3) == 0) in.f = true;
}

// ======================= Update =======================
void update() {
  frameN++;

  if (state == ST_DEMO && anyInput) { resetGame(ST_PLAY); return; }
  if (state == ST_DEMO) aiInput();
  if (state == ST_OVER) {
    if (overTimer) overTimer--;
    if (!overTimer) { resetGame(ST_DEMO); return; }
    in.l = in.r = in.t = in.f = false;
  }

  // ---- ship ----
  if (ship.alive) {
    if (in.l) ship.ang -= 5;
    if (in.r) ship.ang += 5;
    if (in.t) {
      ship.vx += ((int16_t)SIN(ship.ang) * 26) >> 7;
      ship.vy -= ((int16_t)COS(ship.ang) * 26) >> 7;
      if (ship.vx > 700) ship.vx = 700; if (ship.vx < -700) ship.vx = -700;
      if (ship.vy > 700) ship.vy = 700; if (ship.vy < -700) ship.vy = -700;
    }
    ship.vx -= ship.vx >> 7; ship.vy -= ship.vy >> 7;      // a little drag
    ship.x = (ship.x + ship.vx) & 0xFFFFL;
    ship.y = (ship.y + ship.vy) & 0x3FFFL;
    if (ship.cool) ship.cool--;
    if (in.f && !ship.cool) fireBullet();
    if (ship.invul && ship.invul != 255) ship.invul--;
  } else if (ship.respawn && --ship.respawn == 0 && state != ST_OVER) {
    shipRespawn();
    if (state == ST_DEMO) ship.invul = 255;
  }

  // ---- bullets ----
  for (uint8_t i = 0; i < MAXB; i++) if (bul[i].life) {
    bul[i].x = (bul[i].x + bul[i].vx) & XM; bul[i].y = (bul[i].y + bul[i].vy) & YM;
    bul[i].life--;
  }
  // ---- asteroids ----
  for (uint8_t i = 0; i < MAXA; i++) if (ast[i].sz != 0xFF) {
    ast[i].x = (ast[i].x + ast[i].vx) & XM; ast[i].y = (ast[i].y + ast[i].vy) & YM;
  }
  // ---- particles ----
  for (uint8_t i = 0; i < MAXP; i++) if (prt[i].life) {
    prt[i].x = (prt[i].x + prt[i].vx) & XM; prt[i].y = (prt[i].y + prt[i].vy) & YM; prt[i].life--;
  }
  // ---- engine sparks ----
  if (ship.alive && in.t && state != ST_OVER) { spark(); spark(); }
  // ---- UFO ----
  if (!ufo.active) {
    if (ufoTimer) ufoTimer--;
    else if (state != ST_OVER) spawnUfo();
  } else stepUfo();
  for (uint8_t u = 0; u < 2; u++) if (ub[u].life) {
    ub[u].x = (ub[u].x + ub[u].vx) & XM; ub[u].y = (ub[u].y + ub[u].vy) & YM; ub[u].life--;
  }

  // ---- bullet x asteroid / UFO ----
  for (uint8_t b = 0; b < MAXB; b++) if (bul[b].life) {
    int16_t bx = bul[b].x >> 4, by = bul[b].y >> 4;
    for (uint8_t i = 0; i < MAXA; i++) if (ast[i].sz != 0xFF) {
      int16_t r = COLR[ast[i].sz] + 1;
      if (dist2(bx, by, ast[i].x >> 4, ast[i].y >> 4) <= r * r) { bul[b].life = 0; breakAst(i, true); break; }
    }
    if (bul[b].life && ufo.active && dist2(bx, by, ufo.x >> 4, ufo.y >> 4) <= (ufo.small ? 25 : 49)) {   // bullet x UFO
      bul[b].life = 0; ufoKill();
    }
  }
  // ---- UFO bullet x ship / asteroid ----
  for (uint8_t u = 0; u < 2; u++) if (ub[u].life) {
    int16_t bx = ub[u].x >> 4, by = ub[u].y >> 4;
    if (ship.alive && !ship.invul && dist2(bx, by, (int16_t)(ship.x >> 8), (int16_t)(ship.y >> 8)) <= 16) { ub[u].life = 0; killShip(); continue; }
    for (uint8_t i = 0; i < MAXA; i++) if (ast[i].sz != 0xFF) {
      int16_t r = COLR[ast[i].sz] + 1;
      if (dist2(bx, by, ast[i].x >> 4, ast[i].y >> 4) <= r * r) { ub[u].life = 0; breakAst(i, false); break; }   // no score for UFO kills
    }
  }
  // ---- ship x UFO ----
  if (ufo.active && ship.alive && !ship.invul) {
    int16_t r = (ufo.small ? 4 : 6) + 3;
    if (dist2((int16_t)(ship.x >> 8), (int16_t)(ship.y >> 8), ufo.x >> 4, ufo.y >> 4) <= r * r) { killShip(); ufoKill(); }
  }
  // ---- ship x asteroid ----
  if (ship.alive && !ship.invul) {
    int16_t shx = (int16_t)(ship.x >> 8), shy = (int16_t)(ship.y >> 8);
    for (uint8_t i = 0; i < MAXA; i++) if (ast[i].sz != 0xFF) {
      int16_t r = COLR[ast[i].sz] + 3;
      if (dist2(shx, shy, ast[i].x >> 4, ast[i].y >> 4) <= r * r) { killShip(); break; }
    }
  }
  // ---- wave cleared? ----
  bool any = false;
  for (uint8_t i = 0; i < MAXA; i++) if (ast[i].sz != 0xFF) { any = true; break; }
  if (!any) newWave();
}

// ======================= Drawing =======================
void drawShip(int16_t cx, int16_t cy, uint8_t a, bool flame) {
  int8_t xl, xh, yl, yh; wrapRange(cx, 10, 256, xl, xh); wrapRange(cy, 10, 64, yl, yh);
  for (int8_t ix = xl; ix <= xh; ix++) for (int8_t iy = yl; iy <= yh; iy++) {
    int16_t px = cx + ix * 256, py = cy + iy * 64;
    if (!vis(px, py, 10)) continue;
    int16_t x0, y0, x1, y1, x2, y2, xm, ym;
    polar(px, py, a, 6, x0, y0);                 // nose
    polar(px, py, a + 104, 5, x1, y1);           // rear left
    polar(px, py, a + 152, 5, x2, y2);           // rear right
    polar(px, py, a + 128, 3, xm, ym);           // rear notch
    line(x0, y0, x1, y1); line(x0, y0, x2, y2);
    line(x1, y1, xm, ym); line(x2, y2, xm, ym);
    if (flame) {
      // like the original: a "V" shaped flame from the middle of the rear edges, flickering in length every frame
      int16_t bx1 = (x1 + xm) >> 1, by1 = (y1 + ym) >> 1, bx2 = (x2 + xm) >> 1, by2 = (y2 + ym) >> 1;
      int8_t tipR = 8 + (int8_t)((frameN & 1) * 4) + (int8_t)((frameN >> 1) % 3);   // 8..14 px flickering length (same for all strips)
      int16_t tx, ty; polar(px, py, a + 128, tipR, tx, ty);
      line(bx1, by1, tx, ty); line(bx2, by2, tx, ty);
      if (frameN & 2) {                                                         // shorter inner flame, every other pair of frames
        int16_t fx2, fy2; polar(px, py, a + 128, tipR - 4, fx2, fy2);
        line((bx1 + xm) >> 1, (by1 + ym) >> 1, fx2, fy2);
      }
    }
  }
}

// ---- UFO drawing (classic saucer): the small UFO is drawn at 2/3 scale ----
const int8_t UFOSEG[10][4] PROGMEM = {
  {-2, -3, 2, -3}, {-2, -3, -3, -1}, {2, -3, 3, -1},      // dome
  {-3, -1, 3, -1}, {-3, -1, -6, 1}, {3, -1, 6, 1},          // upper hull
  {-6, 1, 6, 1},                                            // middle rim
  {-6, 1, -3, 3}, {6, 1, 3, 3}, {-3, 3, 3, 3}               // lower hull
};
void drawUfo() {
  int16_t cx0 = ufo.x >> 4, cy0 = ufo.y >> 4;
  int8_t yl, yh; wrapRange(cy0, 8, 64, yl, yh);               // the UFO does not wrap horizontally, only vertically
  for (int8_t iy = yl; iy <= yh; iy++) {
    int16_t cy = cy0 + iy * 64;
    if (!vis(cx0, cy, 8)) continue;
    for (uint8_t k = 0; k < 10; k++) {
      int8_t a0 = pgm_read_byte(&UFOSEG[k][0]), b0 = pgm_read_byte(&UFOSEG[k][1]);
      int8_t a1 = pgm_read_byte(&UFOSEG[k][2]), b1 = pgm_read_byte(&UFOSEG[k][3]);
      if (ufo.small) { a0 = a0 * 2 / 3; b0 = b0 * 2 / 3; a1 = a1 * 2 / 3; b1 = b1 * 2 / 3; }
      line(cx0 + a0, cy + b0, cx0 + a1, cy + b1);
    }
  }
}

void drawAst(uint8_t idx) {            // (takes an index, not a struct: the IDE's auto-generated prototype comes before the type definition)
  const Ast &a = ast[idx];
  int16_t cx0 = a.x >> 4, cy0 = a.y >> 4;
  int16_t R = (14 >> a.sz) + 1;
  int8_t xl, xh, yl, yh; wrapRange(cx0, R, 256, xl, xh); wrapRange(cy0, R, 64, yl, yh);
  for (int8_t ix = xl; ix <= xh; ix++) for (int8_t iy = yl; iy <= yh; iy++) {
    int16_t cx = cx0 + ix * 256, cy = cy0 + iy * 64;
    if (!vis(cx, cy, R)) continue;
    int16_t vx[8], vy[8];
    for (uint8_t k = 0; k < 8; k++) {
      int8_t r = pgm_read_byte(&SHAPE[a.shape][k]) >> a.sz;
      polar(cx, cy, k * 32, r, vx[k], vy[k]);
    }
    for (uint8_t k = 0; k < 8; k++) line(vx[k], vy[k], vx[(k + 1) & 7], vy[(k + 1) & 7]);
  }
}

void drawAll() {
  char buf[8];
  // HUD
  utoa(score, buf, 10);
  textStrip(3, 0, buf, 1);
  for (uint8_t i = 0; i < lives; i++) {            // life icons
    int16_t ix = 6 + i * 9, iy = 18;
    line(ix, iy - 4, ix - 3, iy + 3); line(ix, iy - 4, ix + 3, iy + 3); line(ix - 3, iy + 3, ix + 3, iy + 3);
  }
  // asteroids
  for (uint8_t i = 0; i < MAXA; i++) if (ast[i].sz != 0xFF) drawAst(i);
  // ship
  if (ship.alive && !(ship.invul && ship.invul != 255 && (frameN & 4))) drawShip((int16_t)(ship.x >> 8), (int16_t)(ship.y >> 8), ship.ang, in.t && state != ST_OVER);
  // bullets
  for (uint8_t i = 0; i < MAXB; i++) if (bul[i].life) {
    int16_t cx0 = bul[i].x >> 4, cy0 = bul[i].y >> 4;
    int8_t xl, xh, yl, yh; wrapRange(cx0, 2, 256, xl, xh); wrapRange(cy0, 2, 64, yl, yh);
    for (int8_t ix = xl; ix <= xh; ix++) for (int8_t iy = yl; iy <= yh; iy++) {
      int16_t cx = cx0 + ix * 256, cy = cy0 + iy * 64;
      if (!vis(cx, cy, 2)) continue;
      plot(cx, cy); plot(cx + 1, cy); plot(cx, cy + 1); plot(cx + 1, cy + 1);
    }
  }
  // UFO and its bullets
  if (ufo.active) drawUfo();
  for (uint8_t u = 0; u < 2; u++) if (ub[u].life) {
    int16_t cx0 = ub[u].x >> 4, cy0 = ub[u].y >> 4;
    int8_t xl, xh, yl, yh; wrapRange(cx0, 2, 256, xl, xh); wrapRange(cy0, 2, 64, yl, yh);
    for (int8_t ix = xl; ix <= xh; ix++) for (int8_t iy = yl; iy <= yh; iy++) {
      int16_t cx = cx0 + ix * 256, cy = cy0 + iy * 64;
      if (!vis(cx, cy, 2)) continue;
      plot(cx, cy); plot(cx + 1, cy); plot(cx, cy + 1); plot(cx + 1, cy + 1);
    }
  }
  // particles
  for (uint8_t i = 0; i < MAXP; i++) if (prt[i].life) plot(prt[i].x >> 4, prt[i].y >> 4);
  // texts
  if (state == ST_DEMO) {
    textStrip(74, 2, "ASTEROIDS", 2);
    if (frameN & 32) {                                        // blinks together with PRESS FIRE
      textStrip(98, 5, "PRESS FIRE", 1);
      textStrip(107, 7, "Ver:" VERSION, 1);                  // bottom centre: version
    }
    textStrip(130, 0, "github.com/Retro-Fitt", 1);           // top right: project address (126 px wide)
  } else if (state == ST_OVER) {
    textStrip(74, 2, "GAME OVER", 2);
  }
}

uint32_t accU, accC, accF;                  // timing: update / compose strips / write to the VFD (microseconds)
uint16_t lastSum[8];                        // checksum of each strip as last written (0 = empty strip = VFD RAM is clear)

// Fletcher-16 style, position sensitive checksum
static uint16_t stripSum() {
  uint8_t a = 0, b = 0;
  for (uint16_t i = 0; i < 256; i++) { a += strip[i]; b += a; }
  return ((uint16_t)b << 8) | a;
}

void renderFrame() {
  bool force = (frameN & 31) == 0;          // rewrite everything about once a second so that a glitch can never stay
  for (uint8_t s = 0; s < 8; s++) {
    sx = s * 32;
    uint32_t t0 = micros();
    memset(strip, 0, sizeof(strip));
    drawAll();
    uint16_t sum = stripSum();
    uint32_t t1 = micros();
    if (!STRIP_SKIP || sum != lastSum[s] || force) { flushStrip(s); lastSum[s] = sum; }   // do not rewrite unchanged strips
    accC += t1 - t0; accF += micros() - t1;
  }
  if (!displayOn) { vcmd(0x01); displayOn = true; }    // switch on after the first frame so no garbage is visible
}

// ======================= Startup =======================
void vfdInit() {
  DDRC |= 0x1F; PORTC |= 0x1F;               // A0..A4 outputs, all HIGH (A4 = SEL = parallel mode)
  DDRC &= ~_BV(PC5);                         // A5 = INT input
  DDRD |= 0xFC; DDRB |= 0x03;                // data bus
  PORTD |= 0xFC; PORTB |= 0x03;
  delay(1500);                               // let the module start up
  vcmd(0x14); vdat(0x10);                    // display mode: no gray scale
  vcmd(0x00);                                // display off while we write
  memset(strip, 0, sizeof(strip));
  for (uint8_t s = 0; s < 8; s++) flushStrip(s);   // clear the RAM (command 06H does not work)
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 256; i++) sinTab[i] = (int8_t)lroundf(127.0f * sinf(i * 6.2831853f / 256.0f));
  pinMode(10, INPUT_PULLUP); pinMode(11, INPUT_PULLUP); pinMode(12, INPUT_PULLUP); pinMode(13, INPUT_PULLUP);
  vfdInit();
  randomSeed(micros() ^ analogRead(A5));     // A5 is the INT output of the module; still fine as a seed source
  resetGame(ST_DEMO);
  Serial.println(F("ASTEROIDS v" VERSION " ready. Demo running. Start: button or a/d/w/f + Enter"));
}

#define FRAME_MS 33                         // target frame time: 33 ms = ~30 frames/s (game speed is per frame, so this also sets the game speed)

uint32_t nextFrame = 0, fpsT = 0; uint16_t fpsN = 0;

// print an average time as "x.y" ms
void printMs(uint32_t acc, uint16_t n) {
  uint32_t t = acc / n / 100;               // in units of 0.1 ms
  Serial.print(t / 10); Serial.print('.'); Serial.print(t % 10);
}

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - nextFrame) < 0) return;
  nextFrame = now + FRAME_MS;
  uint32_t t0 = micros();
  readInput();
  update();
  accU += micros() - t0;
  renderFrame();
  fpsN++;
  if (now - fpsT >= 2000) {
    Serial.print(F("fps=")); Serial.print(fpsN / 2);
    Serial.print(F("  ms/frame: update=")); printMs(accU, fpsN);
    Serial.print(F(" compose=")); printMs(accC, fpsN);
    Serial.print(F(" flush=")); printMs(accF, fpsN);
    Serial.println();
    fpsN = 0; fpsT = now; accU = accC = accF = 0;
  }
}
