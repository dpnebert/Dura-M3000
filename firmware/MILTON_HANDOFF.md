# Milton Handoff

## State

The authorized customer software work is ready for Milton's independent native build and verification.

- Packet: `C:\Users\Dan\HermesWork\m3000-copilot-handoff-20261106\candidate-native`
- Receipt packet status: `SOFTWARE_READY_FOR_MILTON_INDEPENDENT_VERIFICATION`
- Native Windows build: NOT_RUN; perform independently
- Physical boot/hardware verification: NOT_RUN
- Device flash, network activation, provisioning, OTA, COM/serial/BLE, and installation: NOT_AUTHORIZED

## Scope

### Item 1 - Factory reset

Implemented and host-tested:

- Factory-reset confirmation rendering uses bounded warning lines.
- Shared legacy alignment behavior is restored for unrelated callers.
- Setup confirmation reset/save flow is exercised through the production board handler.
- Reset persistence and fresh-process reload are covered by `test_reference_reset_history.py`.
- Existing lifetime/history and storage behavior remain characterized without rollback claims.

### Item 4 - Fluid preset notices

Implemented and host-tested:

- Empty recalls use `Fluid X has no saved value`.
- Successful saves use `Fluid X has a new saved value`.
- Water uses the corresponding `Water` text.
- Empty selection preserves the active calibration.
- Save failures and cancellations do not claim success.

### Item 6 - Calibration Help and counts

Implemented and host-tested:

- Entering calibration Help clears the active calibration count through the production meter API.
- The pump/output state is not intentionally restarted by entering Help.
- Return does not restore a discarded pre-Help count.
- Insufficient flow uses exactly `Not enough fluid flow detected`.
- Existing threshold, Continue, Cancel, and save behavior remains covered.

### Items 2 and 3 - Operating screens and Manual reset

Preserved and checked with existing host UI and Batch/Total contracts:

- Manual/Auto/Recirc operating UI behavior.
- Manual Batch/Total visibility and reset navigation.
- Existing accounting, metering, precision, navigation, and Back-only terminal behavior.

## Production files changed

- `components/dura_board/dura_board.c`
- `components/dura_meter/dura_meter.c`
- `components/dura_meter/include/dura_meter.h`
- `scripts/test_precal_profile_paths.py`
- `scripts/test_calibration_workflow_contract.py`
- `tests/home_volume_ui.c`

Use [SCOPED_DIFF.patch](SCOPED_DIFF.patch) for the feature-stage change review.

## Available evidence

| Check | Result | Evidence |
|---|---:|---|
| FR06 host renderer | 48/48 | `native-evidence/authorized-item1-render-green2-20261106T141600Z` |
| Integrated FR06 replay | 48/48 | `native-evidence/item1-ui-final-integrated-20261106T141700Z` |
| Factory reset/reload | 58/58 | `native-evidence/authorized-item1-reset-handler-green-20261106T141700Z` |
| Fluid preset paths | 53/53 | `native-evidence/authorized-item4-green2-20261106T141600Z` |
| Calibration save failures | 39/39 | `native-evidence/authorized-item4-save-green-20261106T141530Z` |
| Calibration workflow contract | PASS | `scripts/test_calibration_workflow_contract.py` |
| Manual Batch UI contract | PASS | `scripts/test_batch_ui_contract.py` |

Raw logs, results, fixtures, before bytes, and hashes are retained in the packet and packet-level `native-evidence`.

## Milton verification order

1. Review `IMPLEMENTATION_REPORT.md`, `REGRESSION_RESULTS.md`, and `SCOPED_DIFF.patch`.
2. Validate `.copilot/CUSTOMER_CHECKPOINT.json`, `.copilot/OPERATOR_PLAN.json`, and `ARTIFACT_MANIFEST.json`.
3. Perform the native Windows build using the candidate's established ESP-IDF configuration and documented native toolchain.
4. Record SDK, target, effective configuration, source hashes, binary hash, build command, exit code, and warnings.
5. Address build failures only in this candidate; do not alter or erase unrelated workspace state.
6. Boot-test only under Milton's existing device-verification authorization. A successful host run is not evidence of native startup or hardware behavior.
7. If native startup succeeds, verify:
   - factory-reset warning and both confirmation exits
   - reset persistence and reload
   - all six Fluid recall/save notices
   - calibration Help count reset and insufficient-flow notice
   - Manual/Auto/Recirc and Batch/Total navigation
   - safe pump/output state during calibration Help
8. Keep native and physical evidence separate. Do not reinterpret host checks as device acceptance.

## Important limitations

- Host framebuffer/renderer and NVS simulations do not prove native flash behavior.
- No native Windows build was run by Copilot in this pass.
- No device boot was verified in this pass, so native startup remains a Milton gate.
- The current host evidence may not cover every startup, linker, or board integration combination.
- Packet corrections are complete enough for handoff, but Milton's native review may uncover issues requiring a further source revision.

## Safety boundaries retained

No flash, COM/serial/BLE, provisioning, network activation, OTA publication, installation, SDK/provider/global-setting changes, commits/pushes, hardware pumping, or unrelated product changes are part of this handoff.
