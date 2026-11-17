# Regression Results

| Check | Command family | Exit | Result/Evidence |
|---|---|---:|---|
| FR06 host renderer | `test_home_volume_ui.py --out <fresh packet native-evidence>` | 0 | 48/48 |
| Item 1 reset/reload | `test_reference_reset_history.py --source-root <candidate> --out <fresh packet native-evidence>` | 0 | 58/58 |
| Item 4 preset profiles | `test_precal_profile_paths.py --source-root <candidate> --out <fresh packet native-evidence>` | 0 | 53/53 |
| Item 4 save failures | `test_calibration_save_recovery.py --source-root <candidate> --out <fresh packet native-evidence>` | 0 | 39/39 |
| Item 6 calibration workflow | `test_calibration_workflow_contract.py` | 0 | PASS |
| Item 3 batch UI contract | `test_batch_ui_contract.py` | 0 | PASS |

No native device or native Windows build claim is made.
