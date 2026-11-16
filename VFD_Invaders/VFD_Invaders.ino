/*
  SPACE INVADERS - Futaba GP1045A01A (256x64 VFD) + Arduino UNO

  VFD WIRING (pinout verified by experiment):
    Connector pins 1,3,5,7,9,11,13,15 -> D2..D9   (pin 1 = D7 ... pin 15 = D0, i.e. REVERSED order)
    pin 17 WR -> A0,  pin 19 C/D -> A1,  pin 21 RD -> A2 (keep HIGH while writing),  pin 23 CS -> A3
    pin 26 SEL -> A4 (HIGH),  pin 25 INT -> A5 (unused),  pin 24 not connected
    Even pins 2-16 = GND, pins 18/20/22 = +5 V (separate supply!), all grounds common.

  CONTROLS (buttons to GND, internal pull-ups):
    D10 = LEFT   D11 = RIGHT   D12 or D13 = FIRE
    Serial monitor: a = left, d = right, f or space = fire (send with Enter)
    With no input the AI plays a DEMO; any button/key starts a real game.

  SCREEN: y 0..7   = score / message / spare ships (the UFO flies here too)
          y 9..    = 4 rows x 11 invaders (8x6 sprites, 16 px column pitch, 8 px row pitch)
          y 46..51 = 4 shields (eroded pixel by pixel)
          y 58..63 = cannon
  Every frame the screen is composed in 8 STRIPS of 32 columns from ready-made sprite
  columns and then written to the VFD.
  VFD RAM: address = x*8 + y/8, bit7 = top pixel.  Command 06H (clear) does NOT work on this
  module => the RAM is cleared by writing zeros.
*/

#include <Arduino.h>
#include <avr/pgmspace.h>
#include <stdlib.h>

// ======================= Low-level VFD access (direct port I/O) =======================
#define VFD_WR PC0
#define VFD_CD PC1
#define VFD_RD PC2
#define VFD_CS PC3

static inline uint8_t rev8(uint8_t v) {
  v = ((v & 0xF0) >> 4) | ((v & 0x0F) << 4);
  v = ((v & 0xCC) >> 2) | ((v & 0x33) << 2);
  v = ((v & 0xAA) >> 1) | ((v & 0x55) << 1);
  return v;
}
static inline void busWrite(uint8_t v) {
  uint8_t r = rev8(v);
  PORTD = (PORTD & 0x03) | ((r & 0x3F) << 2);
  PORTB = (PORTB & 0xFC) | (r >> 6);
}
static inline void strobe() {
  PORTC &= ~_BV(VFD_CS);
  PORTC &= ~_BV(VFD_WR);
  __asm__ __volatile__("nop\n\tnop\n\tnop\n\t");
  PORTC |= _BV(VFD_WR);                          // rising edge latches the byte
  PORTC |= _BV(VFD_CS);
}
void vcmd(uint8_t c) { PORTC |= _BV(VFD_CD);  busWrite(c); delayMicroseconds(1); strobe(); delayMicroseconds(1); }
void vdat(uint8_t d) { PORTC &= ~_BV(VFD_CD); busWrite(d); delayMicroseconds(1); strobe(); delayMicroseconds(1); }
void setAddr(uint16_t a) { vcmd(0x0E); vdat(a & 0xFF); vcmd(0x0F); vdat(a >> 8); }

// ======================= Strip buffer =======================
uint8_t strip[256];            // 32 columns x 8 bytes: index = (x - sx)*8 + y/8
uint8_t sx;                    // first column of the current strip

void flushStrip(uint8_t s) {
  setAddr((uint16_t)s * 256);
  vcmd(0x08);
  PORTC &= ~_BV(VFD_CD);
  for (uint16_t i = 0; i < 256; i++) { busWrite(strip[i]); strobe(); }
}

// Draw one column fragment: bits (bit7 = top) with its top end at (x, y); y 0..63
static inline void putBits(int16_t x, int16_t y, uint8_t bits) {
  if ((uint16_t)(x - sx) >= 32 || (uint16_t)y >= 64) return;
  uint8_t *p = &strip[((uint8_t)(x - sx) << 3) + (y >> 3)];
  uint8_t o = y & 7;
  *p |= bits >> o;
  if (o && (y >> 3) < 7) *(p + 1) |= (uint8_t)(bits << (8 - o));
}
// Blit a prepared array of columns (bit7 = top)
void blitCols(const uint8_t *cols, uint8_t w, int16_t x, int16_t y) {
  if (x + w <= sx || x >= sx + 32) return;
  for (uint8_t i = 0; i < w; i++) putBits(x + i, y, cols[i]);
}

// ======================= Font (ASCII 32..90) =======================
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
  0x63,0x14,0x08,0x14,0x63, 0x03,0x04,0x78,0x04,0x03, 0x61,0x59,0x49,0x4D,0x43
};

// Text in block row 0 (y 0..7)
void textTop(int16_t x, const char *s) {
  for (; *s; s++, x += 6) {
    if (x + 6 <= sx || x >= sx + 32) continue;
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 32 || ch > 90) ch = '?';
    const uint8_t *g = FONT5x7 + (uint16_t)(ch - 32) * 5;
    for (uint8_t gc = 0; gc < 5; gc++) {
      int16_t xx = x + gc;
      if ((uint16_t)(xx - sx) >= 32) continue;
      strip[((uint8_t)(xx - sx) << 3)] |= rev8(pgm_read_byte(g + gc));     // font bit0 = top -> bit7 = top
    }
  }
}

// ======================= Sprite data =======================
// Rows are uint16, leftmost pixel = highest bit (width w)
const uint16_t INVR[3][2][6] PROGMEM = {
  { {0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x24}, {0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x5A} },   // squid   (30 pts)
  { {0x24, 0x7E, 0xDB, 0xFF, 0xA5, 0x24}, {0x24, 0x7E, 0xDB, 0xFF, 0x24, 0x42} },   // crab    (20 pts)
  { {0x3C, 0x7E, 0xDB, 0xFF, 0x24, 0x5A}, {0x3C, 0x7E, 0xDB, 0xFF, 0x5A, 0xA5} }    // octopus (10 pts)
};
const uint16_t SHIPR[6] PROGMEM = {0x010, 0x038, 0x038, 0x0FE, 0x1FF, 0x1FF};                 // 9 wide
const uint16_t UFOR[6]  PROGMEM = {0x07E0, 0x1FF8, 0x3FFC, 0x6DB6, 0xFFFF, 0x399C};            // 16 wide
const uint16_t BOOMR[6] PROGMEM = {0x91, 0x52, 0x3C, 0x3C, 0x52, 0x91};                        // 8 wide

uint8_t invCols[3][2][8];
uint8_t shipCols[9], ufoCols[16], boomCols[8];

void buildCols(uint8_t *dst, const uint16_t *rows, uint8_t w, uint8_t h) {
  for (uint8_t i = 0; i < w; i++) {
    uint8_t col = 0, bit = w - 1 - i;
    for (uint8_t r = 0; r < h; r++) if ((pgm_read_word(rows + r) >> bit) & 1) col |= 0x80 >> r;
    dst[i] = col;
  }
}

// ======================= Game data =======================
#define SH_Y 46                        // top of the shields
#define SHIP_Y 58                      // top of the cannon
const int16_t SHX[4] = {35, 91, 147, 203};   // shield x positions
const uint8_t PT[4] = {30, 20, 20, 10};      // points per invader row
const uint8_t RTYPE[4] = {0, 1, 1, 2};       // sprite type per row

uint8_t shield[4][18];                 // column bytes, bit7 = top (6 rows used)
uint16_t alive[4];                     // per row: bit c = invader in column c is alive
uint8_t aliveCnt;
int16_t fx, fy; int8_t fdir; uint8_t fframe, moveTimer;   // formation origin, direction, anim frame, move timer
int16_t shx;                           // cannon x (left edge)
bool pbAct; int16_t pbx, pby;          // player bullet (top pixel)
struct EB { int16_t x, y; bool act; };
EB eb[3];                              // enemy bullets
bool ufoAct; int16_t ux; int8_t udir; uint16_t ufoTimer;
struct Boom { int16_t x, y; uint8_t t; };
Boom booms[3];                         // explosions

enum { S_READY, S_PLAY, S_DYING, S_OVER };
uint8_t gstate = S_READY;
bool demo = true;
uint32_t score;
uint8_t lives, level;
uint16_t gtimer;
uint32_t frameN;
bool displayOn = false;
const char *hudMsg = "";

// ---- input ----
struct In { bool l, r, f; } in;
uint8_t kl, kr, kf;                    // serial key "hold" counters
bool anyInput;
void readInput() {
  in.l = !digitalRead(10); in.r = !digitalRead(11); in.f = !digitalRead(12) || !digitalRead(13);
  bool got = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'a') kl = 6; else if (c == 'd') kr = 6; else if (c == 'f' || c == ' ') kf = 6;
    if (c > ' ') got = true;
  }
  if (kl) { in.l = true; kl--; }
  if (kr) { in.r = true; kr--; }
  if (kf) { in.f = true; kf--; }
  anyInput = got || in.l || in.r || in.f;
}

// ---- shields ----
void buildShields() {
  for (uint8_t k = 0; k < 4; k++) for (uint8_t i = 0; i < 18; i++) {
    uint8_t col = 0xFC;                                  // 6 rows
    if (i == 0 || i == 17) col &= 0x3C;                  // cut the top 2 rows
    else if (i == 1 || i == 16) col &= 0x7C;             // cut the top row
    if (i >= 7 && i <= 10) col &= 0xF0;                  // cut the bottom 2 rows (notch in the middle)
    shield[k][i] = col;
  }
}
bool shieldPixel(int16_t x, int16_t y) {
  if (y < SH_Y || y >= SH_Y + 6) return false;
  for (uint8_t k = 0; k < 4; k++) if (x >= SHX[k] && x < SHX[k] + 18)
    return shield[k][x - SHX[k]] & (0x80 >> (y - SH_Y));
  return false;
}
void clearShieldPixel(int16_t x, int16_t y) {
  if (y < SH_Y || y >= SH_Y + 6) return;
  for (uint8_t k = 0; k < 4; k++) if (x >= SHX[k] && x < SHX[k] + 18)
    shield[k][x - SHX[k]] &= ~(0x80 >> (y - SH_Y));
}
void erodeAt(int16_t x, int16_t y) {
  for (int8_t dx = -1; dx <= 1; dx++) for (int8_t dy = -2; dy <= 1; dy++)
    if (random(5) != 0) clearShieldPixel(x + dx, y + dy);
}
// Invaders crush the shields when they reach them
void erodeByInvaders() {
  for (uint8_t r = 0; r < 4; r++) for (uint8_t c = 0; c < 11; c++) if (alive[r] & (1 << c)) {
    int16_t ix = fx + c * 16, iy = fy + r * 8;
    if (iy + 6 <= SH_Y || iy >= SH_Y + 6) continue;
    for (int16_t xx = ix; xx < ix + 8; xx++) for (int16_t yy = iy; yy < iy + 6; yy++) clearShieldPixel(xx, yy);
  }
}

// ---- explosions ----
void boomAdd(int16_t x, int16_t y) {
  for (uint8_t i = 0; i < 3; i++) if (!booms[i].t) { booms[i].x = x; booms[i].y = y; booms[i].t = 8; return; }
}

// ---- shooting an invader ----
bool hitInvaderAt(int16_t bx, int16_t by) {
  if (by < fy || bx < fx) return false;
  int16_t ry = by - fy, rx = bx - fx;
  uint8_t row = ry >> 3, col = rx >> 4;
  if (row > 3 || col > 10 || (ry & 7) >= 6 || (rx & 15) >= 8) return false;
  if (!(alive[row] & (1 << col))) return false;
  alive[row] &= ~(1 << col); aliveCnt--;
  score += PT[row];
  boomAdd(fx + col * 16, fy + row * 8);
  return true;
}

const uint16_t UFOPTS[4] = {50, 100, 150, 300};

// ======================= Game flow =======================
void startLevel() {
  fx = 44; fy = 9 + 2 * (level > 5 ? 4 : level - 1); fdir = 1; fframe = 0; moveTimer = 0;
  for (uint8_t r = 0; r < 4; r++) alive[r] = 0x7FF;
  aliveCnt = 44;
  buildShields();
  pbAct = false; for (uint8_t i = 0; i < 3; i++) eb[i].act = false;
  for (uint8_t i = 0; i < 3; i++) booms[i].t = 0;
  ufoAct = false; ufoTimer = random(450, 900);
  shx = 123;
  gstate = S_READY; gtimer = 45;
}
void startGame(bool d) {
  demo = d; score = 0; lives = 3; level = 1;
  startLevel();
  if (!displayOn) { vcmd(0x01); displayOn = true; }
}

// Demo AI: track the nearest invader column, shoot, dodge enemy bullets
void aiInput() {
  in.l = in.r = in.f = false;
  int16_t sc = shx + 4, best = 32767, bx = sc;
  uint16_t m = alive[0] | alive[1] | alive[2] | alive[3];
  for (uint8_t c = 0; c < 11; c++) if (m & (1 << c)) {
    int16_t cx = fx + c * 16 + 4, d = abs(cx - sc);
    if (d < best) { best = d; bx = cx; }
  }
  if (bx > sc + 2) in.r = true; else if (bx < sc - 2) in.l = true;
  if (abs(bx - sc) < 4 && !pbAct) in.f = true;
  for (uint8_t i = 0; i < 3; i++) if (eb[i].act && eb[i].y > 34 && abs(eb[i].x - sc) < 8) {   // dodge
    in.f = false;
    if (eb[i].x >= sc) { in.l = true; in.r = false; } else { in.r = true; in.l = false; }
  }
}

void stepFormation() {
  uint16_t m = alive[0] | alive[1] | alive[2] | alive[3];
  if (!m) return;
  uint8_t minC = 0, maxC = 10;
  while (!(m & (1 << minC))) minC++;
  while (!(m & (1 << maxC))) maxC--;
  int16_t le = fx + minC * 16, re = fx + maxC * 16 + 8;
  if (fdir > 0 && re + 2 > 254) { fy += 2; fdir = -1; }
  else if (fdir < 0 && le - 2 < 1) { fy += 2; fdir = 1; }
  else fx += 2 * fdir;
  fframe ^= 1;
  erodeByInvaders();
}

void spawnEnemyBullet() {
  int8_t slot = -1;
  for (uint8_t i = 0; i < 3; i++) if (!eb[i].act) { slot = i; break; }
  if (slot < 0) return;
  uint16_t m = alive[0] | alive[1] | alive[2] | alive[3];
  if (!m) return;
  int8_t c = (shx + 4 - fx) >> 4;                        // column closest to the player
  if (c < 0) c = 0; if (c > 10) c = 10;
  if (random(3) != 0 || !(m & (1 << c))) {
    uint8_t tries = 0;
    do { c = random(11); tries++; } while (!(m & (1 << c)) && tries < 40);
    if (!(m & (1 << c))) return;
  }
  for (int8_t r = 3; r >= 0; r--) if (alive[r] & (1 << c)) {
    eb[slot].x = fx + c * 16 + 4; eb[slot].y = fy + r * 8 + 6; eb[slot].act = true;
    return;
  }
}

void killShip() {
  gstate = S_DYING; gtimer = 60; hudMsg = "";
  pbAct = false;
}

void update() {
  frameN++;
  readInput();
  if (demo && anyInput) { startGame(false); return; }
  if (demo) aiInput();

  // ---- explosions always advance ----
  for (uint8_t i = 0; i < 3; i++) if (booms[i].t) booms[i].t--;

  switch (gstate) {
    case S_READY:
      hudMsg = (demo && (frameN & 32)) ? "PRESS KEY" : "READY";
      if (in.l) shx -= 3; else if (in.r) shx += 3;
      if (shx < 1) shx = 1; if (shx > 246) shx = 246;
      if (gtimer && --gtimer == 0) { gstate = S_PLAY; hudMsg = ""; }
      return;

    case S_DYING:
      if (gtimer && --gtimer == 0) {
        if (lives) lives--;
        if (lives == 0) { gstate = S_OVER; gtimer = 120; hudMsg = "GAME OVER"; }
        else { shx = 123; for (uint8_t i = 0; i < 3; i++) eb[i].act = false; gstate = S_READY; gtimer = 30; }
      }
      return;

    case S_OVER:
      if (gtimer && --gtimer == 0) startGame(true);
      return;
  }

  // ================= S_PLAY =================
  if (demo) hudMsg = (frameN & 32) ? "PRESS KEY" : "";

  // cannon
  if (in.l) shx -= 3; else if (in.r) shx += 3;
  if (shx < 1) shx = 1; if (shx > 246) shx = 246;
  if (in.f && !pbAct) { pbAct = true; pbx = shx + 4; pby = SHIP_Y - 4; }

  // player bullet: 4 px/frame, collision checked in 1-pixel steps
  if (pbAct) {
    for (uint8_t s = 0; s < 4 && pbAct; s++) {
      pby--;
      if (pby < 0) { pbAct = false; break; }
      if (ufoAct && pby >= 1 && pby <= 6 && pbx >= ux && pbx < ux + 16) {
        ufoAct = false; ufoTimer = random(450, 900);
        score += UFOPTS[random(4)]; boomAdd(ux + 4, 1); pbAct = false; break;
      }
      if (shieldPixel(pbx, pby)) { erodeAt(pbx, pby); pbAct = false; break; }
      if (hitInvaderAt(pbx, pby)) { pbAct = false; break; }
    }
  }

  // invader formation (speeds up as invaders die)
  uint8_t interval = 1 + aliveCnt / 15;
  if (level > 2 && interval > 1) interval--;
  if (++moveTimer >= interval) { moveTimer = 0; stepFormation(); }

  // enemy fire
  uint8_t den = level < 9 ? 44 - level * 3 : 16;
  if (random(den) == 0) spawnEnemyBullet();
  for (uint8_t i = 0; i < 3; i++) if (eb[i].act) {
    for (uint8_t s = 0; s < 2 && eb[i].act; s++) {
      eb[i].y++;
      int16_t by = eb[i].y + 3;
      if (by >= 64) { eb[i].act = false; break; }
      if (shieldPixel(eb[i].x, by)) { erodeAt(eb[i].x, by); eb[i].act = false; break; }
      if (by >= SHIP_Y && eb[i].x >= shx && eb[i].x < shx + 9) { eb[i].act = false; killShip(); return; }
    }
  }

  // UFO
  if (!ufoAct) {
    if (ufoTimer) ufoTimer--;
    else if (aliveCnt >= 8) { ufoAct = true; udir = random(2) ? 1 : -1; ux = udir > 0 ? -16 : 256; }
    else ufoTimer = 100;
  } else {
    ux += udir;
    if (ux < -16 || ux > 256) { ufoAct = false; ufoTimer = random(450, 900); }
  }

  // level clear / invasion
  if (aliveCnt == 0) { level++; startLevel(); return; }
  for (int8_t r = 3; r >= 0; r--) if (alive[r]) {
    if (fy + r * 8 + 6 >= SHIP_Y - 2) { lives = 1; killShip(); }      // invasion: lose the last life
    break;
  }
}

// ======================= Drawing =======================
void drawAll() {
  char buf[8];
  // HUD
  textTop(2, "SCORE");
  utoa(score, buf, 10);
  textTop(2 + 6 * 6, buf);
  if (hudMsg[0]) textTop(104, hudMsg);
  for (uint8_t i = 0; i + 1 < lives; i++) blitCols(shipCols, 9, 214 + i * 12, 1);   // spare ships

  // shields
  for (uint8_t k = 0; k < 4; k++) {
    if (SHX[k] + 18 <= sx || SHX[k] >= sx + 32) continue;
    for (uint8_t i = 0; i < 18; i++) if (shield[k][i]) putBits(SHX[k] + i, SH_Y, shield[k][i]);
  }
  // invaders
  for (uint8_t c = 0; c < 11; c++) {
    int16_t x = fx + c * 16;
    if (x + 8 <= sx || x >= sx + 32) continue;
    for (uint8_t r = 0; r < 4; r++) if (alive[r] & (1 << c)) blitCols(invCols[RTYPE[r]][fframe], 8, x, fy + r * 8);
  }
  // UFO
  if (ufoAct) blitCols(ufoCols, 16, ux, 1);
  // explosions
  for (uint8_t i = 0; i < 3; i++) if (booms[i].t) blitCols(boomCols, 8, booms[i].x, booms[i].y);
  // cannon
  if (gstate == S_DYING) {
    if (gtimer & 4) for (uint8_t i = 0; i < 9; i++) putBits(shx + i, SHIP_Y, (uint8_t)((frameN * 37 + i * 91) & 0xFC));
  } else if (gstate != S_OVER) blitCols(shipCols, 9, shx, SHIP_Y);
  // bullets
  if (pbAct) putBits(pbx, pby, 0xF0);
  for (uint8_t i = 0; i < 3; i++) if (eb[i].act) {
    putBits(eb[i].x, eb[i].y, 0xC0);
    putBits(eb[i].x + ((frameN >> 1) & 1), eb[i].y + 2, 0xC0);
  }
}

void renderFrame() {
  for (uint8_t s = 0; s < 8; s++) {
    sx = s * 32;
    memset(strip, 0, sizeof(strip));
    drawAll();
    flushStrip(s);
  }
}

// ======================= Startup =======================
void vfdInit() {
  DDRC |= 0x1F; PORTC |= 0x1F;                   // A0..A4 outputs, all HIGH (A4 = SEL = parallel mode)
  DDRC &= ~_BV(PC5);                             // A5 = INT input
  DDRD |= 0xFC; DDRB |= 0x03;                    // data bus
  PORTD |= 0xFC; PORTB |= 0x03;
  delay(1500);                                   // let the module start up
  vcmd(0x14); vdat(0x10);                        // display mode: no gray scale
  vcmd(0x00);                                    // display off while we write
  memset(strip, 0, sizeof(strip));
  for (uint8_t s = 0; s < 8; s++) flushStrip(s); // clear the RAM (command 06H does not work)
}

void setup() {
  Serial.begin(115200);
  pinMode(10, INPUT_PULLUP); pinMode(11, INPUT_PULLUP); pinMode(12, INPUT_PULLUP); pinMode(13, INPUT_PULLUP);
  for (uint8_t t = 0; t < 3; t++) for (uint8_t f = 0; f < 2; f++) buildCols(invCols[t][f], INVR[t][f], 8, 6);
  buildCols(shipCols, SHIPR, 9, 6);
  buildCols(ufoCols, UFOR, 16, 6);
  buildCols(boomCols, BOOMR, 8, 6);
  vfdInit();
  randomSeed(micros() ^ analogRead(A5));
  startGame(true);
  renderFrame();
  vcmd(0x01); displayOn = true;                  // switch the display on after the first frame was written
  Serial.println(F("SPACE INVADERS ready. Demo running. Start: button or a/d/f + Enter"));
}

uint32_t nextFrame = 0, fpsT = 0; uint16_t fpsN = 0;

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - nextFrame) < 0) return;
  nextFrame = now + 33;                          // ~30 frames/s
  update();
  renderFrame();
  fpsN++;
  if (now - fpsT >= 2000) {
    Serial.print(F("fps=")); Serial.print(fpsN / 2); Serial.print(F(" lvl=")); Serial.print(level);
    Serial.print(F(" alive=")); Serial.println(aliveCnt);
    fpsN = 0; fpsT = now;
  }
}
