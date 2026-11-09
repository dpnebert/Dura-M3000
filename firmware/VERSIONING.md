# Versioning (Dura M3000)

To release, change **only** `DURA_METER_FIRMWARE_VERSION` in:

`source/components/dura_meter/include/dura_meter.h`

Do **not** bump `CONFIG_BLE_SHELL_FIRMWARE_VERSION` in sdkconfig for product releases (it is a reusable-shell fallback set to `UNSET`). `app_main` always passes the meter define into the BLE shell.

Do **not** change `version.txt` / the handoff watermark (`M3000-HO-…`) unless the handoff contract itself changes. That string is `PROJECT_VER` / App version in the ESP-IDF descriptor, not the customer-facing firmware version.

QR on-screen app names live in one header: `source/components/dura_board/include/dura_qr_help.h` (`DURA_OTA_QR_HELP_1` is the iPhone **ESP BLE Prov** label).

## OTA note
The on-device OTA gate accepts any new build whose `app_elf_sha256` differs from the running image (and from the last-invalid slot). It does **not** compare `version.txt` or the customer version string. A rebuild after changing `DURA_METER_FIRMWARE_VERSION` produces a new ELF and can be hosted for field OTA without a cable, once the previous image is confirmed valid.
