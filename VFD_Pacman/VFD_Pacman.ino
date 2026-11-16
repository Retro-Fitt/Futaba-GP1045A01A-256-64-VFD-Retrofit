/*
  PAC-MAN - Futaba GP1045A01A (256x64 VFD) + Arduino UNO

  VFD WIRING (pinout verified by experiment):
    Connector pins 1,3,5,7,9,11,13,15 -> D2..D9   (pin 1 = D7 ... pin 15 = D0, i.e. REVERSED order)
    pin 17 WR -> A0,  pin 19 C/D -> A1,  pin 21 RD -> A2 (keep HIGH while writing),  pin 23 CS -> A3
    pin 26 SEL -> A4 (HIGH),  pin 25 INT -> A5 (unused),  pin 24 not connected
    Even pins 2-16 = GND, pins 18/20/22 = +5 V (separate supply!), all grounds common.

  CONTROLS (buttons / 4-way joystick to GND, internal pull-ups):
    D10 = LEFT   D11 = RIGHT   D12 = UP   D13 = DOWN
    Serial monitor: a = left, d = right, w = up, s = down (send with Enter)
    With no input the AI plays a DEMO; any button/key starts a real game.

  SCREEN: the top 8 pixels hold score / message / lives. Below that is a 32x7-cell maze
  (cell = 8x8 pixels, 1-pixel thin walls, 7-pixel corridors). The maze is generated randomly
  every level (mirrored left/right, no dead ends, wrap-around tunnel in the middle row).
  Only changed cells are rewritten (no full frame buffer, ~600 bytes of RAM).
  VFD RAM: address = x*8 + block_row, bit7 = top pixel.  Command 06H (clear) does NOT work.
*/

#include <Arduino.h>
#include <avr/pgmspace.h>
#include <stdlib.h>

// ======================= Low-level VFD access =======================
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

// One byte at (column x, block row 0..7)
void putByteAt(uint8_t x, uint8_t row, uint8_t b) {
  setAddr((uint16_t)x * 8 + row);
  vcmd(0x08); vdat(b);
}

// ======================= 5x7 font (ASCII 32..90, column based, bit0 = top) =======================
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

// ======================= Sprites (7x7; in a row byte bit6 = leftmost column) =======================
const uint8_t PACR[3][7] PROGMEM = {
  {0x1C, 0x3E, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C},     // mouth closed
  {0x1C, 0x3E, 0x7C, 0x70, 0x7C, 0x3E, 0x1C},     // half open
  {0x1C, 0x38, 0x70, 0x60, 0x70, 0x38, 0x1C}      // open
};
const uint8_t GHOSTS[4][7] PROGMEM = {              // body + eyes, each ghost has a slightly different skirt
  {0x1C, 0x3E, 0x6B, 0x7F, 0x7F, 0x7F, 0x55},
  {0x1C, 0x3E, 0x6B, 0x7F, 0x7F, 0x7F, 0x2A},
  {0x1C, 0x3E, 0x6B, 0x7F, 0x7F, 0x7F, 0x6B},
  {0x1C, 0x3E, 0x6B, 0x7F, 0x7F, 0x7F, 0x49}
};
const uint8_t FRIGHT[7] PROGMEM = {0x1C, 0x2A, 0x55, 0x2A, 0x55, 0x2A, 0x55};  // frightened (dithered)
const uint8_t EYES[7] PROGMEM   = {0x00, 0x00, 0x24, 0x24, 0x00, 0x00, 0x00};  // eaten: just the eyes

static inline uint8_t srcPix(const uint8_t *rows, int8_t x, int8_t y) {
  return (pgm_read_byte(rows + y) >> (6 - x)) & 1;
}
// Column byte (bit7 = top) of output column sc (0..6) of a sprite. dir: 0=right 1=down 2=left 3=up
uint8_t sprColByte(const uint8_t *rows, uint8_t dir, uint8_t sc) {
  uint8_t b = 0;
  for (uint8_t y = 0; y < 7; y++) {
    uint8_t p;
    switch (dir) {
      case 0: p = srcPix(rows, sc, y); break;
      case 2: p = srcPix(rows, 6 - sc, y); break;
      case 1: p = srcPix(rows, y, sc); break;
      default: p = srcPix(rows, 6 - y, sc); break;
    }
    if (p) b |= 0x80 >> y;
  }
  return b;
}

// ======================= Maze =======================
#define MW 32
#define MH 7
#define TUN 3                         // tunnel row
// wall[r][c]: bit0 = wall between (c,r) and (c+1,r), bit1 = wall between (c,r) and (c,r+1),
//             bit2 = pellet, bit3 = power pellet
uint8_t wall[MH][MW];
const int8_t DX[4] = {1, 0, -1, 0};
const int8_t DY[4] = {0, 1, 0, -1};   // 0=right 1=down 2=left 3=up

bool blocked(uint8_t c, uint8_t r, uint8_t d) {
  switch (d) {
    case 0: if (c == MW - 1) return r != TUN; return wall[r][c] & 1;
    case 2: if (c == 0) return r != TUN; return wall[r][c - 1] & 1;
    case 1: if (r == MH - 1) return true; return wall[r][c] & 2;
    default: if (r == 0) return true; return wall[r - 1][c] & 2;
  }
}

void openWall(uint8_t c, uint8_t r, uint8_t d) {
  switch (d) {
    case 0: wall[r][c] &= ~1; break;
    case 2: wall[r][c - 1] &= ~1; break;
    case 1: wall[r][c] &= ~2; break;
    default: wall[r - 1][c] &= ~2; break;
  }
}

uint16_t pelletsLeft;

void genMaze() {
  for (uint8_t r = 0; r < MH; r++) for (uint8_t c = 0; c < MW; c++) wall[r][c] = 3;
  // --- randomized depth-first search on the left half ---
  uint16_t vis[MH] = {0};
  uint8_t stk[MH * 16]; uint8_t sp = 0;
  uint8_t c0 = random(16), r0 = random(MH);
  vis[r0] |= 1 << c0; stk[sp++] = r0 * 16 + c0;
  while (sp) {
    uint8_t cur = stk[sp - 1], c = cur & 15, r = cur >> 4;
    uint8_t nb[4], n = 0;
    if (c > 0 && !(vis[r] & (1 << (c - 1)))) nb[n++] = 2;
    if (c < 15 && !(vis[r] & (1 << (c + 1)))) nb[n++] = 0;
    if (r > 0 && !(vis[r - 1] & (1 << c))) nb[n++] = 3;
    if (r < MH - 1 && !(vis[r + 1] & (1 << c))) nb[n++] = 1;
    if (!n) { sp--; continue; }
    uint8_t d = nb[random(n)];
    openWall(c, r, d);
    uint8_t nc = c + DX[d], nr = r + DY[d];
    vis[nr] |= 1 << nc; stk[sp++] = nr * 16 + nc;
  }
  // --- knock out some extra walls (loops) ---
  for (uint8_t r = 0; r < MH; r++) for (uint8_t c = 0; c < 16; c++) {
    if (c < 15 && random(5) == 0) wall[r][c] &= ~1;
    if (r < MH - 1 && random(5) == 0) wall[r][c] &= ~2;
  }
  // --- doors in the middle (connect the two halves; the tunnel row is always open) ---
  for (uint8_t r = 0; r < MH; r++) if (r == TUN || random(2)) wall[r][15] &= ~1;
  // --- open up dead ends ---
  for (uint8_t pass = 0; pass < 2; pass++)
    for (uint8_t r = 0; r < MH; r++) for (uint8_t c = 0; c < 16; c++) {
      uint8_t open = 0; for (uint8_t d = 0; d < 4; d++) if (!blocked(c, r, d)) open++;
      if (open > 1) continue;
      uint8_t cand[4], n = 0;
      if (blocked(c, r, 0)) cand[n++] = 0;
      if (c > 0 && blocked(c, r, 2)) cand[n++] = 2;
      if (r < MH - 1 && blocked(c, r, 1)) cand[n++] = 1;
      if (r > 0 && blocked(c, r, 3)) cand[n++] = 3;
      if (n) openWall(c, r, cand[random(n)]);
    }
  // --- mirror the left half onto the right half ---
  for (uint8_t r = 0; r < MH; r++) {
    for (uint8_t c = 0; c < 16; c++) {
      uint8_t m = 31 - c;
      wall[r][m] = (wall[r][m] & ~2) | (wall[r][c] & 2);                        // wall below
      if (c >= 1) wall[r][m] = (wall[r][m] & ~1) | (wall[r][c - 1] & 1);        // wall to the right
    }
    wall[r][31] &= ~1;                                                           // tunnel / outer border
  }
  // --- pellets ---
  pelletsLeft = 0;
  for (uint8_t r = 0; r < MH; r++) for (uint8_t c = 0; c < MW; c++) {
    bool pen = (r == TUN && c >= 14 && c <= 17) || (r == MH - 1 && c == 15);   // ghost start / pac start: no pellet
    if (!pen) { wall[r][c] |= 4; pelletsLeft++; }
  }
  const uint8_t pc[4][2] = {{0, 0}, {31, 0}, {0, MH - 1}, {31, MH - 1}};        // power pellets in the corners
  for (uint8_t i = 0; i < 4; i++) { wall[pc[i][1]][pc[i][0]] = (wall[pc[i][1]][pc[i][0]] & ~4) | 8; }
}

// ======================= Entities =======================
enum { GH_WAIT = 0, GH_NORMAL, GH_FRIGHT, GH_EATEN };
struct Ent { int16_t x, y; uint8_t dir, state, want; uint16_t timer; bool vis; };
Ent ents[5];                        // 0 = pac, 1..4 = ghosts

enum { S_READY, S_PLAY, S_DYING, S_OVER };
uint8_t gstate = S_READY;
bool demo = true;
uint32_t score;
uint8_t lives, level;
uint16_t gtimer, modeTimer, frightTimer;
uint8_t eatCombo;
uint8_t pacAnim;
uint32_t frameN;
bool displayOn = false;

const char *hudMsg = "";
bool hudScoreDirty = true, hudMsgDirty = true, hudLivesDirty = true;

// ---- dirty cell tracking ----
uint8_t dirty[MH][4];
void markCell(int8_t c, int8_t r) {
  if (r < 0 || r >= MH) return;
  c &= 31;
  dirty[r][c >> 3] |= 1 << (c & 7);
}
void markEnt(int16_t x, int16_t y) {
  for (int8_t cx = (x - 3) >> 3; cx <= (x + 3) >> 3; cx++)
    for (int8_t ry = ((y - 3) >> 3) - 1; ry <= ((y + 3) >> 3) - 1; ry++) markCell(cx, ry);
}
void markAllEnts() { for (uint8_t i = 0; i < 5; i++) markEnt(ents[i].x, ents[i].y); }

// ---- cell rendering ----
int16_t wrapD(int16_t d, int16_t M) { int16_t h = M >> 1; if (d > h) d -= M; else if (d < -h) d += M; return d; }

void renderCell(uint8_t c, uint8_t r, uint8_t *b) {
  for (uint8_t i = 0; i < 8; i++) b[i] = 0;
  // walls (1 pixel): left edge = x 8c, top edge = y 8(r+1)
  if (blocked(c, r, 2)) b[0] |= 0xFF;
  if (c == MW - 1 && r != TUN) b[7] |= 0xFF;
  if (blocked(c, r, 3)) for (uint8_t i = 0; i < 8; i++) b[i] |= 0x80;
  if (r == MH - 1) for (uint8_t i = 0; i < 8; i++) b[i] |= 0x01;
  // pellet / power pellet
  uint8_t w = wall[r][c];
  if (w & 4) { b[3] |= 0x18; b[4] |= 0x18; }
  else if ((w & 8) && (frameN & 8)) { for (uint8_t i = 2; i < 6; i++) b[i] |= 0x3C; }
  // sprites
  for (uint8_t k = 0; k < 5; k++) {
    Ent &e = ents[k];
    if (!e.vis) continue;
    const uint8_t *rows; uint8_t dr = 0;
    if (k == 0) { static const uint8_t am[4] = {0, 1, 2, 1}; rows = PACR[am[(pacAnim >> 1) & 3]]; dr = e.dir; }
    else if (e.state == GH_EATEN) rows = EYES;
    else if (e.state == GH_FRIGHT) rows = (frightTimer < 45 && (frameN & 4)) ? GHOSTS[k - 1] : FRIGHT;
    else rows = GHOSTS[k - 1];
    int16_t oy = (e.y - 3) - 8 * (r + 1);
    if (oy >= 8 || oy <= -7) continue;
    for (int8_t off = -1; off <= 1; off++) {
      int16_t sx0 = e.x - 3 + off * 256;
      for (uint8_t i = 0; i < 8; i++) {
        int16_t sc = (int16_t)c * 8 + i - sx0;
        if (sc < 0 || sc > 6) continue;
        uint8_t bits = sprColByte(rows, dr, sc);
        b[i] |= (oy >= 0) ? (bits >> oy) : (bits << (-oy));
      }
    }
  }
}

void writeCell(uint8_t c, uint8_t r) {
  uint8_t b[8];
  renderCell(c, r, b);
  for (uint8_t i = 0; i < 8; i++) putByteAt(c * 8 + i, r + 1, b[i]);
}

void flushDirty() {
  for (uint8_t r = 0; r < MH; r++) for (uint8_t g = 0; g < 4; g++) {
    uint8_t m = dirty[r][g];
    if (!m) continue;
    dirty[r][g] = 0;
    for (uint8_t i = 0; i < 8; i++) if (m & (1 << i)) writeCell(g * 8 + i, r);
  }
}

void fullRedraw() {
  for (uint8_t r = 0; r < MH; r++) for (uint8_t c = 0; c < MW; c++) { writeCell(c, r); }
  for (uint8_t r = 0; r < MH; r++) for (uint8_t g = 0; g < 4; g++) dirty[r][g] = 0;
}

// ---- HUD (block row 0) ----
char scoreTxt[14];
uint8_t hudByte(uint8_t x) {
  const char *s = 0; uint8_t pos = 0;
  if (x < 78) { s = scoreTxt; pos = x; }
  else if (x >= 100 && x < 100 + 66 && hudMsg[0]) { s = hudMsg; pos = x - 100; }
  if (s) {
    uint8_t ci = pos / 6, k = pos % 6;
    if (k == 5) return 0;
    uint8_t len = 0; while (s[len]) len++;
    if (ci >= len) return 0;
    char ch = s[ci]; if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 32 || ch > 90) ch = '?';
    return rev8(pgm_read_byte(FONT5x7 + (uint16_t)(ch - 32) * 5 + k));
  }
  if (x >= 200) {
    uint8_t j = (x - 200) / 9, k = (x - 200) % 9;
    if (j < lives && k < 7) return sprColByte(PACR[1], 0, k);
  }
  return 0;
}
void drawHud(uint8_t x0, uint16_t x1) {
  for (uint16_t x = x0; x < x1; x++) putByteAt(x, 0, hudByte(x));
}
void refreshHud(bool all) {
  if (all || hudScoreDirty) {
    char t[10]; ultoa(score, t, 10);
    strcpy(scoreTxt, "SCORE "); strcat(scoreTxt, t);
    drawHud(0, 78); hudScoreDirty = false;
  }
  if (all || hudMsgDirty) { drawHud(100, 166); hudMsgDirty = false; }
  if (all || hudLivesDirty) { drawHud(200, 256); hudLivesDirty = false; }
}
void setMsg(const char *m) { if (m != hudMsg) { hudMsg = m; hudMsgDirty = true; } }

// ======================= Game logic =======================
void resetPositions() {
  ents[0].x = 15 * 8 + 4; ents[0].y = 8 * (MH - 1 + 1) + 4; ents[0].dir = 2; ents[0].want = 2; ents[0].vis = true; ents[0].state = 1;
  const uint16_t wt[4] = {0, 40, 90, 150};          // frames before each ghost leaves its start cell
  for (uint8_t i = 1; i < 5; i++) {
    ents[i].x = (13 + i) * 8 + 4; ents[i].y = 8 * (TUN + 1) + 4;
    ents[i].dir = 3; ents[i].state = GH_WAIT; ents[i].timer = wt[i - 1]; ents[i].vis = true;
  }
  frightTimer = 0; pacAnim = 2;
}

void startLevel() {
  genMaze();
  resetPositions();
  fullRedraw();
  gstate = S_READY; gtimer = 45; setMsg("READY!");
  modeTimer = 0;
}

void startGame(bool d) {
  demo = d; score = 0; lives = 3; level = 1;
  hudScoreDirty = hudLivesDirty = true;
  startLevel();
  refreshHud(true);
  if (!displayOn) { vcmd(0x01); displayOn = true; }
}

// ---- input ----
uint8_t padDir = 255;
bool anyInput;
void readInput() {
  padDir = 255;
  if (!digitalRead(11)) padDir = 0;
  if (!digitalRead(13)) padDir = 1;
  if (!digitalRead(10)) padDir = 2;
  if (!digitalRead(12)) padDir = 3;
  bool got = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd') { padDir = 0; got = true; } else if (c == 's') { padDir = 1; got = true; }
    else if (c == 'a') { padDir = 2; got = true; } else if (c == 'w') { padDir = 3; got = true; }
    else if (c > ' ') got = true;
  }
  anyInput = got || padDir != 255;
}

// (no struct parameters: the Arduino IDE's auto-generated prototypes come before the type definitions)
static inline bool atCenter(int16_t x, int16_t y) { return (x & 7) == 4 && (y & 7) == 4; }

void startFright() {
  frightTimer = level < 8 ? 180 - level * 15 : 60;
  eatCombo = 0;
  for (uint8_t i = 1; i < 5; i++) if (ents[i].state == GH_NORMAL || ents[i].state == GH_FRIGHT) {
    ents[i].state = GH_FRIGHT; ents[i].dir ^= 2;
  }
}

// ---- demo AI ----
uint8_t aiChoose() {
  uint8_t c = ents[0].x >> 3, r = (ents[0].y >> 3) - 1;
  // nearest pellet
  int16_t bd = 32767; int8_t bc = -1, br = -1;
  for (uint8_t rr = 0; rr < MH; rr++) for (uint8_t cc = 0; cc < MW; cc++) if (wall[rr][cc] & 12) {
    int16_t d = abs((int16_t)cc - c) + abs((int16_t)rr - r) * 2;
    if (d < bd) { bd = d; bc = cc; br = rr; }
  }
  int16_t bestS = -32767; uint8_t best = ents[0].dir;
  for (uint8_t d = 0; d < 4; d++) {
    if (blocked(c, r, d)) continue;
    int8_t nc = (c + DX[d]) & 31, nr = r + DY[d];
    int16_t s = random(0, 4);
    if (wall[nr][nc] & 4) s += 6;
    if (wall[nr][nc] & 8) s += 10;
    if (bc >= 0) s += 24 - (abs(nc - bc) + abs(nr - br) * 2);
    if (d == (ents[0].dir ^ 2)) s -= 5;
    for (uint8_t g = 1; g < 5; g++) {
      if (ents[g].state != GH_NORMAL && ents[g].state != GH_FRIGHT) continue;
      int16_t gd = abs((ents[g].x >> 3) - nc) + abs(((ents[g].y >> 3) - 1) - nr);
      if (ents[g].state == GH_NORMAL && gd < 5) s -= (5 - gd) * 12;
      if (ents[g].state == GH_FRIGHT && gd < 6) s += (6 - gd) * 2;
    }
    if (s > bestS) { bestS = s; best = d; }
  }
  return best;
}

void stepPac() {
  Ent &p = ents[0];
  if (p.want < 4 && p.want == (p.dir ^ 2)) p.dir = p.want;           // reversing is allowed at any time
  bool moved = true;
  if (atCenter(p.x, p.y)) {
    uint8_t c = p.x >> 3, r = (p.y >> 3) - 1;
    uint8_t &w = wall[r][c];
    if (w & 4) { w &= ~4; score += 10; pelletsLeft--; hudScoreDirty = true; markCell(c, r); }
    else if (w & 8) { w &= ~8; score += 50; pelletsLeft--; hudScoreDirty = true; markCell(c, r); startFright(); }
    if (demo) p.want = aiChoose();
    if (p.want < 4 && !blocked(c, r, p.want)) p.dir = p.want;
    if (blocked(c, r, p.dir)) moved = false;
  }
  if (moved) { p.x = (p.x + 2 * DX[p.dir]) & 255; p.y += 2 * DY[p.dir]; pacAnim++; }
}

uint8_t chooseGhostDir(uint8_t gi) {
  Ent &g = ents[gi];
  uint8_t c = g.x >> 3, r = (g.y >> 3) - 1;
  int8_t pcx = ents[0].x >> 3, pcy = (ents[0].y >> 3) - 1;
  int8_t tx = pcx, ty = pcy;
  bool scatter = (modeTimer % 750) < 150;
  if (g.state == GH_EATEN) { tx = 15; ty = TUN; }                    // eyes head back to the ghost house
  else if (scatter) {
    static const int8_t sc[4][2] = {{31, 0}, {0, 0}, {31, MH - 1}, {0, MH - 1}};   // one corner per ghost
    tx = sc[gi - 1][0]; ty = sc[gi - 1][1];
  } else {
    uint8_t pd = ents[0].dir;
    switch (gi) {
      case 2: tx = pcx + 4 * DX[pd]; ty = pcy + 4 * DY[pd]; break;                  // "Pinky": 4 cells ahead of pac
      case 3: { int8_t ax = pcx + 2 * DX[pd], ay = pcy + 2 * DY[pd];                // "Inky": reflect Blinky around a point ahead of pac
                tx = 2 * ax - (ents[1].x >> 3); ty = 2 * ay - ((ents[1].y >> 3) - 1); } break;
      case 4: { int16_t dx = (int16_t)c - pcx, dy = (int16_t)r - pcy;               // "Clyde": chase when far, flee to a corner when close
                if (dx * dx + dy * dy < 64) { tx = 0; ty = MH - 1; } } break;
      default: break;                                                                // "Blinky": chase pac directly
    }
  }
  uint8_t cand[4], n = 0; int16_t bestD = 32767; uint8_t best = 255;
  for (uint8_t d = 0; d < 4; d++) {
    if (d == (g.dir ^ 2)) continue;                                  // ghosts never reverse on their own
    if (blocked(c, r, d)) continue;
    cand[n++] = d;
    int8_t nc = (c + DX[d]) & 31, nr = r + DY[d];
    int16_t dd = (int16_t)(tx - nc) * (tx - nc) + (int16_t)(ty - nr) * (ty - nr);
    if (dd < bestD) { bestD = dd; best = d; }
  }
  if (!n) return g.dir ^ 2;
  if (g.state == GH_FRIGHT) return cand[random(n)];
  return best;
}

void stepGhost(uint8_t gi) {
  Ent &g = ents[gi];
  if (g.state == GH_WAIT) { if (g.timer) g.timer--; else g.state = GH_NORMAL; return; }
  uint8_t steps = 1;
  if (g.state == GH_EATEN) steps = 2;                                // eyes move fast
  else if (g.state == GH_FRIGHT) { if (frameN & 1) return; }         // frightened ghosts are slow
  else if ((frameN & 7) == 0) return;                                // normal ghosts are slightly slower than pac
  for (uint8_t s = 0; s < steps; s++) {
    if (atCenter(g.x, g.y)) {
      uint8_t c = g.x >> 3, r = (g.y >> 3) - 1;
      if (g.state == GH_EATEN && r == TUN && (c == 15 || c == 16)) { g.state = GH_NORMAL; }
      g.dir = chooseGhostDir(gi);
    }
    g.x = (g.x + 2 * DX[g.dir]) & 255; g.y += 2 * DY[g.dir];
  }
}

void killPac() {
  gstate = S_DYING; gtimer = 50; setMsg("");
}

void checkCollisions() {
  Ent &p = ents[0];
  for (uint8_t i = 1; i < 5; i++) {
    Ent &g = ents[i];
    if (g.state != GH_NORMAL && g.state != GH_FRIGHT) continue;
    int16_t dx = wrapD(p.x - g.x, 256), dy = p.y - g.y;
    if (abs(dx) < 5 && abs(dy) < 5) {
      if (g.state == GH_FRIGHT) { g.state = GH_EATEN; score += 200UL << (eatCombo < 3 ? eatCombo : 3); eatCombo++; hudScoreDirty = true; }
      else { killPac(); return; }
    }
  }
}

void update() {
  frameN++;
  readInput();
  if (demo && anyInput) { startGame(false); return; }
  if (!demo && padDir != 255) ents[0].want = padDir;

  // power pellets blink
  if ((frameN & 7) == 0) { markCell(0, 0); markCell(31, 0); markCell(0, MH - 1); markCell(31, MH - 1); }
  markAllEnts();                                   // old positions

  switch (gstate) {
    case S_READY:
      if (demo && (frameN & 32)) setMsg("PRESS KEY"); else setMsg("READY!");
      if (gtimer && --gtimer == 0) { gstate = S_PLAY; setMsg(demo ? "PRESS KEY" : ""); }
      break;
    case S_PLAY:
      if (demo) setMsg((frameN & 32) ? "PRESS KEY" : "");
      if (frightTimer) { if (--frightTimer == 0) for (uint8_t i = 1; i < 5; i++) if (ents[i].state == GH_FRIGHT) ents[i].state = GH_NORMAL; }
      else modeTimer++;
      stepPac();
      for (uint8_t i = 1; i < 5; i++) stepGhost(i);
      checkCollisions();
      if (gstate == S_PLAY && pelletsLeft == 0) { level++; startLevel(); return; }
      break;
    case S_DYING:
      ents[0].vis = !(gtimer & 4);
      if (gtimer && --gtimer == 0) {
        ents[0].vis = true;
        if (lives) lives--;
        hudLivesDirty = true;
        if (lives == 0) { gstate = S_OVER; gtimer = 120; setMsg("GAME OVER"); }
        else { resetPositions(); gstate = S_READY; gtimer = 45; setMsg("READY!"); }
      }
      break;
    case S_OVER:
      if (gtimer && --gtimer == 0) { startGame(true); return; }
      break;
  }
  markAllEnts();                                   // new positions
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
  setAddr(0); vcmd(0x08);
  PORTC &= ~_BV(VFD_CD);
  for (uint16_t i = 0; i < 2048; i++) { busWrite(0); strobe(); }   // clear the RAM (command 06H does not work)
}

void setup() {
  Serial.begin(115200);
  pinMode(10, INPUT_PULLUP); pinMode(11, INPUT_PULLUP); pinMode(12, INPUT_PULLUP); pinMode(13, INPUT_PULLUP);
  vfdInit();
  randomSeed(micros() ^ analogRead(A5));
  startGame(true);
  Serial.println(F("PAC-MAN ready. Demo running. Start: button or a/d/w/s + Enter"));
}

uint32_t nextFrame = 0, fpsT = 0; uint16_t fpsN = 0;

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - nextFrame) < 0) return;
  nextFrame = now + 33;                          // ~30 frames/s
  update();
  flushDirty();
  refreshHud(false);
  fpsN++;
  if (now - fpsT >= 2000) {
    Serial.print(F("fps=")); Serial.print(fpsN / 2); Serial.print(F(" lvl=")); Serial.print(level);
    Serial.print(F(" pellets=")); Serial.println(pelletsLeft);
    fpsN = 0; fpsT = now;
  }
}
