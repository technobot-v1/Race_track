// Race_track -- a Watchy watchface.
//
// An hour car and a minute car creep around a spiral circuit: the minute car
// moves one waypoint every 5 minutes, the hour car one every hour, and the
// time is printed in the box at the centre.
//
// Notes on how it is built:
//   track art     200x200 1-bit, stored as row-XOR + run lengths, 5000 B -> 630 B
//   trig          91-byte integer sine table, so libm is never linked
//   digits        plain char arithmetic instead of snprintf
//   car rotation  inverse mapping (destination -> source), which cannot leave
//                 the gaps a forward map does at non-right angles
//
// See README.md for build requirements.

#include <Watchy.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "bitmaps.h"
#include "settings.h"

// Draw a 1px white halo around each car so it stays readable where it crosses
// a kerb line.  Costs about 460 B of flash when enabled.
#define CAR_OUTLINE 0

// ---------------------------------------------------------------------------
// Waypoints, placed by hand against the track artwork.  M has one entry per
// 5-minute step (Minute / 5) and H one per hour (Hour % 12).  a is the heading
// in degrees: 0 points right and grows clockwise.
// ---------------------------------------------------------------------------
struct Step {
  int16_t x, y, a;
};

// constexpr Step M[12] = {{100, 125, 0},  {130, 90, 270}, {178, 20, 45},  {187, 100, 90},
//                         {187, 155, 100}, {140, 185, 170}, {100, 187, 180}, {42, 167, 200},
//                         {28, 137, 270}, {28, 100, 270}, {30, 33, 315},  {74, 120, 20}};

// constexpr Step H[12] = {{100, 145, 0},  {146, 54, 280}, {175, 60, 90},  {172, 100, 90},
//                         {168, 155, 130}, {140, 170, 180}, {100, 171, 180}, {24, 179, 225},
//                         {15, 143, 270}, {15, 100, 270}, {16, 31, 320},  {59, 141, 40}};

constexpr Step M[12] = {{100, 125, 0},  {130, 90, 270}, {178, 20, 45},  {187, 100, 90},
                        {187, 155, 100}, {140, 170, 180}, {100, 171, 180}, {42, 167, 200},
                        {28, 137, 270}, {28, 100, 270}, {30, 33, 315},  {74, 120, 20}};

constexpr Step H[12] = {{100, 145, 0},  {146, 54, 280}, {175, 60, 90},  {172, 100, 90},
                        {168, 155, 110}, {140, 185, 180}, {100, 187, 180}, {24, 179, 225},
                        {15, 143, 270}, {15, 100, 270}, {16, 31, 320},  {59, 141, 40}};


// sin(0..90 degrees) * 127.  Everything else comes out of this by symmetry,
// which is a lot cheaper in flash than pulling in libm for two calls.
const int8_t SIN_TABLE[91] PROGMEM = {
      0,   2,   4,   7,   9,  11,  13,  15,  18,  20,  22,  24,  26,
     29,  31,  33,  35,  37,  39,  41,  43,  46,  48,  50,  52,  54,
     56,  58,  60,  62,  63,  65,  67,  69,  71,  73,  75,  76,  78,
     80,  82,  83,  85,  87,  88,  90,  91,  93,  94,  96,  97,  99,
    100, 101, 103, 104, 105, 107, 108, 109, 110, 111, 112, 113, 114,
    115, 116, 117, 118, 119, 119, 120, 121, 121, 122, 123, 123, 124,
    124, 125, 125, 125, 126, 126, 126, 127, 127, 127, 127, 127, 127};

// Rotated-car scratch box.  The longest sprite is 25x12, whose diagonal is
// 27.8 px, so 32 covers it with room for the outline and keeps the row stride
// a whole number of bytes.
constexpr int16_t BOX = 32;

// One decoded track row.  Kept at file scope so drawTrack() doesn't put 25
// bytes on the stack on every refresh.
static uint8_t trackRow[(TRACK_WIDTH + 7) / 8];

class RaceTrackWatch : public Watchy {
  using Watchy::Watchy;

 public:
  void drawWatchFace() override;

 private:
  static int16_t isin(int16_t deg);
  static int16_t icos(int16_t deg) { return isin(deg + 90); }
  void centerChar(char c, int16_t x, int16_t y);
  void drawTrack();
  void drawTimeBox();
  void drawCar(int16_t cx, int16_t cy, const uint8_t *bmp, int16_t w, int16_t h, int16_t a,
               bool allowMirror);
};

// Waypoints whose sprite must NOT be mirrored, even though their heading falls
// in the 90..270 range that normally triggers it.  Bit i = waypoint i, and it
// applies to both cars.  Edit this rather than fudging the angle: the angle is
// the heading, and the arrows in tools/render_all_times.py read it as such.
constexpr uint16_t NO_MIRROR = (1u << 4) | (1u << 7);

static inline bool mirrorAllowed(uint8_t i) { return !((NO_MIRROR >> i) & 1u); }

int16_t RaceTrackWatch::isin(int16_t deg) {
  deg %= 360;
  if (deg < 0) deg += 360;
  if (deg <= 90) return (int8_t)pgm_read_byte(&SIN_TABLE[deg]);
  if (deg <= 180) return (int8_t)pgm_read_byte(&SIN_TABLE[180 - deg]);
  if (deg <= 270) return -(int8_t)pgm_read_byte(&SIN_TABLE[deg - 180]);
  return -(int8_t)pgm_read_byte(&SIN_TABLE[360 - deg]);
}

// ---------------------------------------------------------------------------
// Track
//
// The art is stored as run lengths over the row-XOR of the image: every row
// holds (row ^ row_above), which erases the long vertical strokes that make up
// most of a race track.  Rebuilding a row is therefore just XOR-ing this row's
// residual into the previous row, which is what trackRow accumulates.
// ---------------------------------------------------------------------------
void RaceTrackWatch::drawTrack() {
  memset(trackRow, 0, sizeof(trackRow));

  int16_t x = 0, y = 0;
  bool bit = false;  // the stream starts on a run of zeroes
  for (uint16_t i = 0; i < TRACK_RLE_LEN && y < TRACK_HEIGHT; i++) {
    uint16_t n = pgm_read_byte(&TRACK_RLE[i]);
    while (n) {
      int16_t run = TRACK_WIDTH - x;
      if (n < (uint16_t)run) run = n;
      if (bit) {
        for (int16_t k = x; k < x + run; k++) trackRow[k >> 3] ^= 0x80 >> (k & 7);
      }
      x += run;
      n -= run;
      if (x == TRACK_WIDTH) {
        // trackRow now holds the real row: paint its black runs.
        for (int16_t bx = 0; bx < TRACK_WIDTH;) {
          if (!trackRow[bx >> 3] && (bx & 7) == 0) {  // whole empty byte, skip 8
            bx += 8;
            continue;
          }
          if (trackRow[bx >> 3] & (0x80 >> (bx & 7))) {
            int16_t start = bx;
            while (bx < TRACK_WIDTH && (trackRow[bx >> 3] & (0x80 >> (bx & 7)))) bx++;
            display.drawFastHLine(start, y, bx - start, GxEPD_BLACK);
          } else {
            bx++;
          }
        }
        x = 0;
        if (++y == TRACK_HEIGHT) break;
      }
    }
    if (pgm_read_byte(&TRACK_RLE[i]) != 255) bit = !bit;
  }
}

// ---------------------------------------------------------------------------
// Time box
// ---------------------------------------------------------------------------
void RaceTrackWatch::centerChar(char c, int16_t x, int16_t y) {
  char s[2] = {c, 0};
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(x - (w / 2) - x1, y - (h / 2) - y1);
  display.print(s);
}

void RaceTrackWatch::drawTimeBox() {
  display.fillRect(85, 20, 30, 75, GxEPD_WHITE);
  display.drawRect(85, 20, 30, 75, GxEPD_BLACK);
  display.setFont(&FreeSansBold9pt7b);
  display.setTextColor(GxEPD_BLACK);

  centerChar('0' + currentTime.Hour / 10, 100, 33);
  centerChar('0' + currentTime.Hour % 10, 100, 47);
  // Separator bar rather than a colon: the digits stack vertically, and a
  // colon's upper dot ends up touching the bottom of the hour digit.
  display.fillRect(92, 59, 17, 2, GxEPD_BLACK);
  centerChar('0' + currentTime.Minute / 10, 100, 72);
  centerChar('0' + currentTime.Minute % 10, 100, 86);
}

// ---------------------------------------------------------------------------
// Cars
//
// FIX 1: rotate by inverse mapping.  The original walked the *source* pixels
//        and scattered them into the destination, which leaves unwritten gaps
//        at any angle that isn't a multiple of 90 -- the cars came out as
//        confetti.  Walking the destination and asking "which source pixel
//        lands here" cannot leave a gap.
//
// FIX 2: mirror, don't over-rotate.  The sprites are side-on profiles, so a
//        heading between 90 and 270 degrees has to be drawn flipped about the
//        vertical axis, otherwise the car drives along on its roof.  The
//        original hardcoded this for two waypoints only (indices 4 and 5), so
//        waypoints 6 and 7 -- 6:30 and 7:35 -- rendered upside down.  (8 and 9
//        are exactly 270 deg, which correctly does not flip.)
// ---------------------------------------------------------------------------
void RaceTrackWatch::drawCar(int16_t cx, int16_t cy, const uint8_t *bmp, int16_t w, int16_t h,
                             int16_t a, bool allowMirror) {
  a %= 360;
  if (a < 0) a += 360;
  bool flip = allowMirror && (a > 90 && a < 270);
  if (flip) a = 180 - a;

  const int16_t c = icos(a), s = isin(a);
  const int16_t stride = (w + 7) >> 3;
  uint8_t box[(BOX * BOX) / 8] = {0};

  for (int16_t by = 0; by < BOX; by++) {
    // 2 * (by - centre), so the /2 at the end is the only rounding step.
    const int32_t py2 = 2 * by - (BOX - 1);
    for (int16_t bx = 0; bx < BOX; bx++) {
      const int32_t px2 = 2 * bx - (BOX - 1);
      // Inverse rotation, then back into sprite coordinates.  +1024 keeps the
      // shifted value positive so >>1 is a plain floor.
      const int32_t sx2 = (px2 * c + py2 * s) >> 7;
      const int32_t sy2 = (py2 * c - px2 * s) >> 7;
      int16_t sx = (int16_t)((sx2 + w + 1024) >> 1) - 512;
      const int16_t sy = (int16_t)((sy2 + h + 1024) >> 1) - 512;
      if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;
      if (flip) sx = w - 1 - sx;
      if (!(pgm_read_byte(&bmp[sy * stride + (sx >> 3)]) & (0x80 >> (sx & 7)))) continue;
      const int16_t bi = by * BOX + bx;
      box[bi >> 3] |= 0x80 >> (bi & 7);
    }
  }

  const int16_t ox = cx - BOX / 2, oy = cy - BOX / 2;
#if CAR_OUTLINE
  for (int16_t by = 0; by < BOX; by++)
    for (int16_t bx = 0; bx < BOX; bx++) {
      const int16_t bi = by * BOX + bx;
      if (!(box[bi >> 3] & (0x80 >> (bi & 7)))) continue;
      for (int16_t dy = -1; dy <= 1; dy++)
        for (int16_t dx = -1; dx <= 1; dx++)
          display.drawPixel(ox + bx + dx, oy + by + dy, GxEPD_WHITE);
    }
#endif
  display.drawBitmap(ox, oy, box, BOX, BOX, GxEPD_BLACK);
}

// ---------------------------------------------------------------------------
void RaceTrackWatch::drawWatchFace() {
  display.fillScreen(GxEPD_WHITE);
  drawTrack();
  drawTimeBox();

  const uint8_t ms = currentTime.Minute / 5, hs = currentTime.Hour % 12;
  const Step &m = M[ms];
  const Step &h = H[hs];

  // At the start/finish the cars sit on the chequered strip, so wipe it first.
  if (ms == 0)
    display.fillRect(m.x - 13, m.y - 7, MINUTE_CAR_WIDTH + 2, MINUTE_CAR_HEIGHT + 2, GxEPD_WHITE);
  if (hs == 0)
    display.fillRect(h.x - 13, h.y - 7, HOUR_CAR_WIDTH + 2, HOUR_CAR_HEIGHT + 2, GxEPD_WHITE);

  drawCar(m.x, m.y, MINUTE_CAR, MINUTE_CAR_WIDTH, MINUTE_CAR_HEIGHT, m.a, mirrorAllowed(ms));
  drawCar(h.x, h.y, HOUR_CAR, HOUR_CAR_WIDTH, HOUR_CAR_HEIGHT, h.a, mirrorAllowed(hs));
}

RaceTrackWatch watchy(settings);

void setup() { watchy.init(); }

void loop() {}
