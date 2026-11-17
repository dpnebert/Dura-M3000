# Held-volume speed tuning — source update, not a new binary release

This supplied `m3000-release-0.2.0/source` tree now implements Daniel's capped-exponential held-button request. **The sibling `binaries/` images, release hashes and original release notes still describe the supplied November 9 release; they do not contain this source change.** No firmware version bump, native build, flash, reset, OTA publication or hardware operation was performed for this update.

## Where to tune

In the project's ESP-IDF 6.0.2 environment, from this `source` directory:

```sh
idf.py menuconfig
```

Open **Dura M3000 board → Held volume adjustment (gallons / liters / ounces)**. There are separate **Gallons**, **Liters** and **Ounces** submenus, each with only these three speed controls:

- **Starting speed (0.01 units/second):** `10` means 0.10 selected units/s; `100` means 1.00.
- **Speed doubling time (ms):** larger means gentler acceleration. `1000` doubles speed every second after the initial hold delay.
- **Maximum speed (0.01 units/second):** the cap. `500` means 5.00 selected units/s. Lower it when sustained scrolling is too fast.

A common **Delay before held-volume movement (ms)** defaults to `400`. The initial press still changes one fine count. Release/repress or direct direction reversal starts a fresh tap, delay and ramp. Settings are **build-time**: save menuconfig and rebuild the app; no settings/calibration erase is required. Existing saved `button_hold_max_rate_x` does not override this quantity ramp. It remains compatible with existing commands/NVS and controls only non-volume setup/calibration repeats.

### Initial tunable defaults

- Gallons: start **0.10 gal/s**, doubling time **1000 ms**, cap **5.00 gal/s**.
- Liters: start **0.40 L/s**, doubling time **1000 ms**, cap **20.00 L/s**.
- Ounces: start **1.00 oz/s**, doubling time **1000 ms**, cap **50.00 oz/s**.

These are provisional tuning defaults, **not measured original-meter parity or customer-approved final rates**. They deliberately expose independent settings instead of hardcoding a conversion ratio. The original-meter matching direction remains a physical comparison goal, not a claim made by this implementation.

Corresponding symbols (replace `GALLON` with `LITER` or `OUNCE`):

```text
CONFIG_DURA_BOARD_HOLD_GALLON_START_CENTI=10
CONFIG_DURA_BOARD_HOLD_GALLON_DOUBLING_MS=1000
CONFIG_DURA_BOARD_HOLD_GALLON_MAX_CENTI=500
CONFIG_DURA_BOARD_HOLD_DELAY_MS=400
```

`Kconfig.projbuild`, `sdkconfig.defaults` and the supplied active `sdkconfig` agree. Changing `sdkconfig.defaults` alone does not override an existing `sdkconfig`; use menuconfig for the active build. Speed ranges are 1–100000 hundredths/s; doubling time 100–30000 ms; delay 100–3000 ms. Start above cap is treated as constant cap speed. The maximum is an upper tuning limit, not a recommendation.

## Behavior and implementation

```text
t = max(0, (held_ms - delay_ms) / 1000)
speed(t) = min(max_speed, start_speed * 2^(t / doubling_seconds))
quantity = post_tap_anchor ± integral(speed, 0..t)
```

Movement is truncated to whole editable counts before applying it to the post-tap anchor. This preserves fractional progress without repeatedly rounding away small changes. Each actual board sample evaluates absolute elapsed time; slow UI servicing does not drop repeat movement or compound a second step-size acceleration. The exponent is limited to time-to-cap before evaluation, then a constant-speed tail is used. No unbounded catch-up loop is added. Displayed values remain discrete at the configured precision; this is not a promise of a continuously moving display or faster button polling.

Scope: Auto, Recirc and the existing batch-set quantity editors. Fine taps, configured precision, amount bounds, gallon-based preset storage/conversion, layouts, Auto Mode gating, calibration adjustment behavior, sleep policy, outputs, BLE/OTA and NVS format are not redesigned. A unit/menu/navigation change cancels an existing quantity gesture; release/repress is required to start again.

Production implementation:

```text
components/dura_board/Kconfig.projbuild
components/dura_board/dura_ui_behavior.c
components/dura_board/include/dura_ui_behavior.h
components/dura_board/dura_board.c
sdkconfig
sdkconfig.defaults
```

## Verification and remaining limits

Evidence is in the sibling `../ramp-update/` directory. Exact originals and a before-file hash inventory are retained there; generated tests use inert hardware/NVS shims, not a connected meter.

- RED: the actual board caller failed the new time/quantity and unit-change requirements before the implementation (`red/RESULTS.json`).
- GREEN: 672 rate comparisons through the actual extracted board press/hold/release branch and real meter, across all three units, both directions, Auto/Recirc, two independent configuration profiles, multiple hold durations and 10/100/333/1000 ms sampling. The expected distance uses an independent numerical-integration oracle.
- Actual-caller lifecycle and saved-config override/failure cases pass: taps, release/repress, direct reversal, unit/menu changes, zero floor, no output activation, saved multipliers 1/100 and failing runtime-config reads.
- Pure behavior/contract test passes with `-Wall -Wextra -Werror`; focused edge checks include precision 0/1/2, cap transitions, bounds, long holds and invalid settings.
- Retained cycle 1/2/3 and home/volume/render checks: **71/71 pass**. Only the intentionally superseded step-stage expectations were updated; unrelated regression assertions remain.
- Full board-owner backlight and sleep-recovery fixtures also pass (118 and 10 cases). Their harnesses were updated to import the actual changed volume helpers and missing platform/OTA declarations; assertions and production sleep/backlight policies were not changed. Initial harness compile failures are retained under `verification/`; final runs are under `final/`.
- The component Kconfig parses with Espressif `esp-idf-kconfig` 3.14.0, allowed by published IDF v6.0 constraints; all ten new defaults match. This is a standalone component parse, not a native menuconfig session.

**Pending:** native ESP32-S3 build/link and device feel/legacy timing comparison. This VPS has no active IDF/compiler installation at the inspected prior locations or in PATH. No toolchain was reinstalled. Host checks do not prove native runtime behavior, electrical/flow accuracy or physical UI acceptance. Build this source in the existing native ESP-IDF environment before any separately authorized deployment. Do not flash the unchanged sibling binaries expecting the new ramp.

Reproduce the focused check (host C compiler plus Python; no device access):

```sh
python scripts/test_held_volume_ramp.py --out ../ramp-update/recheck
python scripts/test_ui_behavior_contract.py
```

## Documentation checked

- ESP-IDF 6.0.2 [ESP Timer, Obtaining Current Time](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-reference/system/esp_timer.html#obtaining-current-time): microseconds since initialization; reset after deep sleep. Existing board timing is reused.
- ESP-IDF 6.0.2 [Build System, Custom Sdkconfig Defaults](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-guides/build-system.html#custom-sdkconfig-defaults): existing active values take precedence over defaults.
- Espressif [Kconfig language](https://docs.espressif.com/projects/esp-idf-kconfig/en/latest/kconfiglib/language.html): int/range/default and menu options. [IDF v6.0 constraints](https://dl.espressif.com/dl/esp-idf/espidf.constraints.v6.0.txt) bind the standalone syntax check, not the unavailable native environment's exact installed parser.
- [Math library exp/log/expm1 contracts](https://sourceware.org/newlib/libm.html): bounded exponent, positive logarithm arguments and accurate exponential-minus-one evaluation. Native linking remains pending.
