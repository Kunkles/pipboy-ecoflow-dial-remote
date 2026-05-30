#pragma once

/*
  secrets.example.h — TEMPLATE

  Copy this file to `secrets.h` and fill in your keys for a PERSONAL build.
  `secrets.h` is gitignored so your real keys never get committed.

      cp secrets.example.h secrets.h

  ── PERSONAL build (your own unit) ───────────────────────────────────────
    Leave `#define PERSONAL_BUILD` UNCOMMENTED and paste your keys below.
    Your keys get compiled into the firmware; the dial never prompts for them.

  ── DISTRIBUTION build (a unit you hand to someone else) ──────────────────
    COMMENT OUT `#define PERSONAL_BUILD`. The keys compile to empty strings,
    so NOTHING sensitive is baked into the binary. On first boot the dial
    prompts the user to enter their OWN keys via the WiFi captive portal,
    stored only in that unit's NVS.

  Get your keys (requires an approved EcoFlow developer account):
  https://developer.ecoflow.com/us/document/generalInfo
*/

// ── Toggle this line to switch build modes ──────────────────────────────────
#define PERSONAL_BUILD

#ifdef PERSONAL_BUILD
  #define SECRET_ACCESS_KEY  "PASTE_YOUR_ACCESS_KEY_HERE"
  #define SECRET_SECRET_KEY  "PASTE_YOUR_SECRET_KEY_HERE"
#else
  // Distribution build — intentionally blank. Keys entered on-device.
  #define SECRET_ACCESS_KEY  ""
  #define SECRET_SECRET_KEY  ""
#endif
