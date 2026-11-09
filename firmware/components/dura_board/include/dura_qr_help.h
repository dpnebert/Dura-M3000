#pragma once

/* QR on-screen help — edit these strings only when the target app name changes.
 * Scout claim QR (boot): dura://claim?...  (see render_provisioning_qr comments).
 * OTA Wi-Fi QR: Espressif network_prov JSON. Daniel confirmed the iPhone app name
 * is exactly "ESP BLE Prov" (DURA_OTA_QR_HELP_1). Keep that the single source.
 *
 * Layout: OTA QR is drawn at (0,0) scale=1 quiet=2. Real payload encodes as
 * 33 modules → 37 px square. Help text is left-aligned at qr_px+2 so every
 * line ends at least 2 px before the right edge (host test asserts this). */
#define DURA_SCOUT_QR_HELP_1 "Use Scout"
#define DURA_SCOUT_QR_HELP_2 "TO ADD"
#define DURA_SCOUT_QR_HELP_3 "METER"
#define DURA_OTA_QR_HELP_1   "ESP BLE Prov"
#define DURA_OTA_QR_HELP_2   "app, Wi-Fi:"
#define DURA_OTA_QR_HELP_3   "PROV_M3000"
#define DURA_OTA_QR_HELP_FAIL "Use " DURA_OTA_QR_HELP_1
