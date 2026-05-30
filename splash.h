#pragma once
/*
  splash.h — EcoFlow Pip-Boy boot splash
  Call drawSplashScreen() from setup() instead of bootScreen() for the initial power-on.
  Call drawSplashScreen("CONNECTING...", "EcoFlow-Dial") etc. for status updates during boot.
*/

// Draw the EF logomark centered at (cx, cy), scaled by `sz` (1.0 = full size ~80px tall)
void drawEFMark(int cx, int cy, float sz, uint32_t col) {
  // The EF mark is two columns of paired horizontal bars
  // Left column  = "E" shape: 3 bars (top pair, middle, bottom pair)
  // Right column = "F" shape: 2 bars (top pair, middle)

  int bh  = (int)(5  * sz);   // bar height
  int bw1 = (int)(28 * sz);   // left bar width  (E)
  int bw2 = (int)(20 * sz);   // right bar width (F, slightly shorter)
  int gap = (int)(4  * sz);   // gap between the two bars in each pair
  int col_gap = (int)(8 * sz);// gap between E and F columns
  int row_gap = (int)(14 * sz);// gap between top pair and bottom pair

  // Left column origin
  int lx = cx - (int)((bw1 + col_gap/2 + bw2/2) * 1.0f) + (int)(bw2/2);
  // Right column origin
  int rx = lx + bw1 + col_gap;

  // Top of the whole mark
  int ty = cy - row_gap - bh - gap/2;

  // ── Left column (E): top pair ─────────────────────────────────────────────
  M5Dial.Display.fillRect(lx, ty,           bw1, bh, col);
  M5Dial.Display.fillRect(lx, ty+bh+gap,    bw1, bh, col);

  // ── Left column (E): bottom pair ─────────────────────────────────────────
  M5Dial.Display.fillRect(lx, ty+row_gap+bh+gap,          bw1, bh, col);
  M5Dial.Display.fillRect(lx, ty+row_gap+bh+gap+bh+gap,   bw1, bh, col);

  // ── Right column (F): top pair ────────────────────────────────────────────
  M5Dial.Display.fillRect(rx, ty,           bw2, bh, col);
  M5Dial.Display.fillRect(rx, ty+bh+gap,    bw2, bh, col);

  // ── Right column (F): bottom pair ────────────────────────────────────────
  M5Dial.Display.fillRect(rx, ty+row_gap+bh+gap,          bw2, bh, col);
  M5Dial.Display.fillRect(rx, ty+row_gap+bh+gap+bh+gap,   bw2, bh, col);
}

// Dotted circle (approximated with short arcs/points every N degrees)
void drawDottedCircle(int cx, int cy, int r, uint32_t col, int dotEvery=6) {
  for (int a = 0; a < 360; a += dotEvery) {
    float rad = a * DEG_TO_RAD;
    int x = cx + (int)(r * cosf(rad));
    int y = cy + (int)(r * sinf(rad));
    M5Dial.Display.fillCircle(x, y, 1, col);
  }
}

// Scanline overlay — draw every other row semi-transparent (dark lines)
void drawScanlines(uint32_t col) {
  for (int y = 0; y < 240; y += 3)
    M5Dial.Display.drawLine(0, y, 239, y, col);
}

// ── Main splash screen ────────────────────────────────────────────────────────
// line1 / line2: status text shown at bottom (pass "" to leave blank)
void drawSplashScreen(const String& line1 = "", const String& line2 = "") {
  const uint32_t BG      = 0x0000;
  const uint32_t BRIGHT  = 0x37E6;   // bright green
  const uint32_t MID     = 0x2703;   // mid green
  const uint32_t DIM     = 0x1382;   // dim green
  const uint32_t DARK    = 0x09A0;   // very dark green (scanlines, ring)
  const int cx = 120, cy = 108;      // logo center (slightly above middle)

  M5Dial.Display.fillScreen(BG);

  // Outer dotted ring
  drawDottedCircle(cx, cy, 88, DIM, 5);

  // Inner ring (thin solid)
  M5Dial.Display.drawCircle(cx, cy, 80, DARK);

  // EF logomark
  drawEFMark(cx - 4, cy - 10, 1.0f, BRIGHT);

  // "ECOFLOW" text
  M5Dial.Display.setFont(&fonts::Font4);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(MID);
  M5Dial.Display.drawString("ECOFLOW", cx, cy + 36);

  // Thin separator line below logo area
  M5Dial.Display.drawLine(30, cy + 52, 210, cy + 52, DARK);

  // Status lines
  M5Dial.Display.setFont(&fonts::Font2);
  M5Dial.Display.setTextColor(DIM);
  if (line1.length())
    M5Dial.Display.drawString(line1, cx, cy + 66);
  if (line2.length()) {
    M5Dial.Display.setTextColor(BRIGHT);
    M5Dial.Display.drawString(line2, cx, cy + 80);
  }

  // Scanlines on top of everything
  drawScanlines(0x0820);   // very dark, just enough to feel CRT-ish
}
