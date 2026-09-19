# model/archive: obsolete model material

`obsolete-2026-09-19.zip` holds the files the cleanup of 2026-09-19 took out of `model/`. They are scripts whose findings were withdrawn, fits that were superseded, and exports that later exports contain in full. **Nothing in the tree reads these files, and nothing in the tree needed them.**

- **Paths inside the zip are repo-relative** (`model/...`). A path cited in `thermalProfileCampaign.md` or `campaignResults_summer2026.md` has the same name inside the zip.
- **To restore a file,** run this from the repo root; the file lands at its old path:

  ```
  python -c "import zipfile; zipfile.ZipFile('model/archive/obsolete-2026-09-19.zip').extract('model/campaign-summer-2026/m3_event_study.py')"
  ```

  None of these files is in the tree any more, so extracting the whole zip overwrites nothing.
- **To check a restored file,** run `git hash-object <file>`. The result must equal the blob id under Integrity below. The zip holds each file as git stores it, line endings included.
- **They are also in git history.** `git log -1 -- <path>` names the commit that removed a file; the file is in that commit's parent. The hash changes when the branch is rebased, so none is quoted here.

**Kept on purpose:**
- **`campaign-summer-2026/archived_overlap/`**: SD logs from a second, overlapping download chain. `memory/gotcha-log.md` sets them aside there (one log chain per time window) and names the folder, so the folder stays.
- **The three `calibration_input_*.csv`**: they were built on the two-hours-late LoRa data. But `gate-plant`, the June calibrators and `wind_rose_speed_count.py` read them.
- **`calibrate_plant_campaign.py`, `calibrate_plant_dynamic.py` and `calibrate_plant_constrained.py`**: `closed_loop.py` and `closedloop/plant.py` import their functions. Their default inputs stay with them: `plant_calibrated.json`, `plant_calibrated_summer2026.json` and the 06-25 calibration input.
- **The spring toolchain** (`simulation.py`, `calibrate_plant.py`, the `input_S*` scenarios, `campaign-spring-2026/`): it is still the documented settings-verification workflow (`model/README.md`).

## Superseded analysis scripts

Each ran on a calibration input whose outdoor and door columns are two hours late (the LoRa database stamps in UTC; `model/lora_time.py`). So each reproduces a finding that `thermalProfileCampaign.md` §9.12 withdrew. Each file's header says so and names its replacement.

| File | Bytes | Last changed | Why | Instead |
|---|---|---|---|---|
| `model/campaign-summer-2026/m3_event_study.py` | 5 565 | 2026-09-18 | §9.9's event study (the withdrawn "trigger artifact") | `closedloop/campaign_figures.py --only F3` |
| `model/campaign-summer-2026/ns9_direction_stratified.py` | 6 622 | 2026-09-18 | §9.11's per-segment `ach_m3` by wind direction (withdrawn). It also held the reference wind-valid cutoff, which now lives in `closedloop/dataset.py` (`WIND_VALID_FROM`) | `closedloop/refit.py fit --direction`, `campaign_figures.py --only NS9` |
| `model/campaign-summer-2026/m3_ach_polar.py` | 4 987 | 2026-09-18 | Polar plot of §9.11's seven M3-only segment fits (the withdrawn values, hard-coded) | §9.12.6 |
| `model/campaign-summer-2026/m3_ach_polar.png` | 161 851 | 2026-07-12 | Its figure | §9.12.6 |
| `model/campaign-summer-2026/m3_jul11_analysis.py` | 4 837 | 2026-09-18 | §9.10's Jul 11 forced tests (the withdrawn "T_in converged to T_out"). Not runnable: it reads an outdoor export from a session scratchpad outside the repo | `campaign_figures.py --only F2` |

## Superseded single-node fits

Nothing reads these. They are outputs of the June single-node calibrators, plus the one calibrator nothing imports. The adopted plants are the two-node fits in `campaign-summer-2026/plant2/`. Rerun with `--plot`, the calibrators would redraw the figures.

| File | Bytes | Last changed | Why | Instead |
|---|---|---|---|---|
| `model/calibrate_plant_staged.py` | 21 137 | 2026-06-28 | §9.6 approach B, the two-stage fit. It was not adopted: per-bitmask `ach_m3` was not identifiable open loop. Nothing imports it | none; §9.6 keeps its numbers |
| `model/campaign-summer-2026/plant_calibrated_staged_summer2026.json` | 655 | 2026-06-28 | Its output | none |
| `model/campaign-summer-2026/plant_calibrated_dynamic_summer2026.json` | 525 | 2026-06-28 | §9.6 approach A, the joint 7-parameter fit, not adopted (written by `calibrate_plant_dynamic.py`) | none |
| `model/campaign-summer-2026/plant_calibrated_constrained_summer2026.json` | 966 | 2026-07-05 | §9.7's bounded 6-parameter fit as refitted in §9.9, where the `_freem3` variant replaced it and it was kept for comparison | `plant_calibrated_constrained_summer2026_freem3.json` |
| `model/campaign-summer-2026/calibration_compare_summer2026.png` | 757 717 | 2026-06-28 | §9.5's first-pass fit plot (NS-4), on the two-hours-late input | none |
| `model/campaign-summer-2026/calibration_dynamic_summer2026.png` | 887 407 | 2026-06-28 | §9.6's fit plot, same input | none |
| `model/campaign-summer-2026/calibration_constrained_summer2026.png` | 1 035 739 | 2026-07-05 | Its fit plot (§9.9), same input | none |
| `model/campaign-summer-2026/calibration_constrained_summer2026_freem3.png` | 1 028 787 | 2026-07-05 | §9.9's fit plot of the free-m3 plant, same input. The plant itself stays | none |

## Redundant LoRa exports

Each is an exact subset of the `_2026-09-17` export of the same sensor: same header, and every row present (checked 2026-09-19). They were the inputs of the June and July `calibration_input_*.csv`, which stay.

| File | Bytes | Last changed | Why | Instead |
|---|---|---|---|---|
| `model/campaign-summer-2026/lht65_20_2026-06-04_2026-06-25.csv` | 188 741 | 2026-06-27 | Every row is in `lht65_20_2026-06-04_2026-09-17.csv` | `lht65_20_2026-06-04_2026-09-17.csv` |
| `model/campaign-summer-2026/lht65_20_2026-06-04_2026-07-04.csv` | 272 770 | 2026-07-05 | Every row is in `lht65_20_2026-06-04_2026-09-17.csv` | `lht65_20_2026-06-04_2026-09-17.csv` |
| `model/campaign-summer-2026/lht65_20_2026-06-04_2026-07-12.csv` | 339 215 | 2026-07-12 | Every row is in `lht65_20_2026-06-04_2026-09-17.csv` | `lht65_20_2026-06-04_2026-09-17.csv` |
| `model/campaign-summer-2026/lds01_5_2026-06-04_2026-06-25.csv` | 1 897 | 2026-06-27 | Every row is in `lds01_5_2026-06-01_2026-09-17.csv` | `lds01_5_2026-06-01_2026-09-17.csv` |
| `model/campaign-summer-2026/lds01_5_2026-06-04_2026-07-04.csv` | 3 266 | 2026-07-05 | Every row is in `lds01_5_2026-06-01_2026-09-17.csv` | `lds01_5_2026-06-01_2026-09-17.csv` |
| `model/campaign-summer-2026/lds01_5_2026-06-04_2026-07-12.csv` | 3 798 | 2026-07-12 | Every row is in `lds01_5_2026-06-01_2026-09-17.csv` | `lds01_5_2026-06-01_2026-09-17.csv` |
| `model/campaign-summer-2026/lds01_6_2026-06-04_2026-06-25.csv` | 10 889 | 2026-06-27 | Every row is in `lds01_6_2026-06-01_2026-09-17.csv` | `lds01_6_2026-06-01_2026-09-17.csv` |
| `model/campaign-summer-2026/lds01_6_2026-06-04_2026-07-04.csv` | 17 912 | 2026-07-05 | Every row is in `lds01_6_2026-06-01_2026-09-17.csv` | `lds01_6_2026-06-01_2026-09-17.csv` |
| `model/campaign-summer-2026/lds01_6_2026-06-04_2026-07-12.csv` | 21 815 | 2026-07-12 | Every row is in `lds01_6_2026-06-01_2026-09-17.csv` | `lds01_6_2026-06-01_2026-09-17.csv` |

## One-off

| File | Bytes | Last changed | Why | Instead |
|---|---|---|---|---|
| `model/plot_lds_doors.py` | 3 275 | 2026-06-27 | Quick-look plot of the two door sensors, straight from the LoRa database (2026-06-27). Nothing cites it, and it plots the raw UTC stamps. Door data now comes through `fetch_lora_data.py` exports into `closedloop/dataset.py` | `fetch_lora_data.py`, `closedloop/dataset.py` |
| `model/plot_lds_doors.png` | 90 730 | 2026-06-27 | Its figure, drawn 2026-06-27 | none |

## Removed from a file that stays

- **`closedloop/logdata.py`: `OutdoorRow`, `load_outdoor()` and `CAL_INPUT`.** Nothing called them, and they read the merged calibration input, whose outdoor columns are two hours late. `closedloop/dataset.py` joins the raw exports instead. They are in the history of `closedloop/logdata.py`.

## Integrity

24 files, 4 871 103 bytes before compression.

| File | git blob | sha256 |
|---|---|---|
| `model/campaign-summer-2026/m3_event_study.py` | `92f4333eae3b9bf65ffe69a7c72e1a423c757b20` | `fa7941787a44659f528f6617dfca4b27197c182cc650fe546e94d1bcfc34c700` |
| `model/campaign-summer-2026/ns9_direction_stratified.py` | `e007260353d9c17e3bf100fefc6a76f80fff2ce9` | `f981e70a69bf1cd4e177cae64e0d2b5c7b6311040125c16cc5973de92c10f528` |
| `model/campaign-summer-2026/m3_ach_polar.py` | `dac6aa1be00f6f4526855b96df6ccc0cb41bd65b` | `f428bf2520ec3915f47a8db84a9aa56ca6566f70cd26cff0b4c0b27edce51690` |
| `model/campaign-summer-2026/m3_ach_polar.png` | `a33bfb57f24977b18191c37d1461d5c70a4075ac` | `3ad48e0d090ed013c35d2b9a1e46e96031d4ae7883dc09e24ff67e253dc12829` |
| `model/campaign-summer-2026/m3_jul11_analysis.py` | `aa0d4148feec82eb7f25444c5a984dba7cc6cd2e` | `dfd7855103caa3db10767e97ff10e97c4c51688ffd7e5c5d4554a3385b9018ff` |
| `model/calibrate_plant_staged.py` | `33f742df1c562c1983df15378aae09cd1b458ded` | `f6680826a805cca1181959cbfe3726a761b6b09c535009ea79b26c0e0d370250` |
| `model/campaign-summer-2026/plant_calibrated_staged_summer2026.json` | `acbf75fcac2593a6c6cc85fff0643d720941ad5b` | `0962cac4cf29094e9dc82aa88287a76ed0d50274668bf2c29e4a2d037722ff3f` |
| `model/campaign-summer-2026/plant_calibrated_dynamic_summer2026.json` | `d9514365e9c9e81564819787c8852dd9b7e4d94e` | `41ecd62d0974e932101bde54bdfc743f8932e7c21f1c081d1687982ffad7b23e` |
| `model/campaign-summer-2026/plant_calibrated_constrained_summer2026.json` | `3cecb952ba78e7c34a3b2fee06d34275241dab17` | `42915b378d47d3b5cfc06d9230cc5a3e53999e56f128099450dbd20c132d90f2` |
| `model/campaign-summer-2026/calibration_compare_summer2026.png` | `c9c5de5bb05149445e265b25d0bfa3c7c9e46140` | `b249b335fe9f893aa5b832803d98f659debfa8b6299957af122944300bb32c1d` |
| `model/campaign-summer-2026/calibration_dynamic_summer2026.png` | `674dfaa4584b34fc4d3568ca131b72a907ac375a` | `f51d5822cbf55cdb8fb86c87bfe893e896aca1153810a278abb9fa7e3fee7acc` |
| `model/campaign-summer-2026/calibration_constrained_summer2026.png` | `4bf3b664a9e8c76b50eaaeef16d272a1bac43068` | `39e9039b3c75eaf5cd9b395d4df2eb619fbaa6d7a3c4fc8c1cfd41967b36d122` |
| `model/campaign-summer-2026/calibration_constrained_summer2026_freem3.png` | `c7dfb856f4621318997b2f032e266ec219f865a9` | `d6ce78f7596be1195e47596a2e2fb8ab02c149a117231edb90a19d4a0cce357a` |
| `model/campaign-summer-2026/lht65_20_2026-06-04_2026-06-25.csv` | `969444e2e3cafee5c21f674888be7fdb8438a92f` | `6f20f3abf5076b53bfd94daff40bb9e120feb8430a44c9dca58acb999ffdf81e` |
| `model/campaign-summer-2026/lht65_20_2026-06-04_2026-07-04.csv` | `c14563e2464fb9c291f46b7f57af4eae50bfe4bf` | `dbaccd645ed6b17306aa6a1d2948f70a53fab31aa11034a31513c98a736b8feb` |
| `model/campaign-summer-2026/lht65_20_2026-06-04_2026-07-12.csv` | `c92697be5e2692e58373c3b8197aad7732cd8dc6` | `5154b1d95733b14c27f9b70ae10bd597ebaf749e7951d451480527c27f40a888` |
| `model/campaign-summer-2026/lds01_5_2026-06-04_2026-06-25.csv` | `c486d8f86c9a34340208b47cf32a34033a551514` | `e376761be8e8cea0717ae6a54d9fa3dab21629e334625b459faeafaac75d40d7` |
| `model/campaign-summer-2026/lds01_5_2026-06-04_2026-07-04.csv` | `9db4a0dc56852ab0866a338db634f78c5ff2831c` | `6f7d8d47ae6db7ff43e5568efa00d5bfc92a53a4a859645a59504dcccbe1f14e` |
| `model/campaign-summer-2026/lds01_5_2026-06-04_2026-07-12.csv` | `03354c56e8bd6e74f4b40b4fc2fd0f049923c8cb` | `9628869f6f54ebd939484919d1b6674ac0d933c33f9b3e557e7619e3cf8f057c` |
| `model/campaign-summer-2026/lds01_6_2026-06-04_2026-06-25.csv` | `ef73b8a830fab6b3980a61e05166d6d586c01253` | `cebaea2820a7f7f9ee5fe0aae6bbdddbad39d5fc48daf01c68086de6e2ea182a` |
| `model/campaign-summer-2026/lds01_6_2026-06-04_2026-07-04.csv` | `eff6325c14e3300e6c845a55476dec96aed9bf31` | `860b7fdfe4eca8cbe5487403d029b9f0d111a81dcfc17491a4f8e47911086015` |
| `model/campaign-summer-2026/lds01_6_2026-06-04_2026-07-12.csv` | `1f6e012b2b8a08ad99f1d5e170ed9f0be62408d3` | `2b9ed40fb978716fe7d1a298ec22764e8a47bb2006f0b6af7ddb0e933f12b032` |
| `model/plot_lds_doors.py` | `e73df82eb69e7a22f27bfec7c614beac0f1b8f48` | `424a5060c8e13f7f95d45a9acc5df300fba0954bea52b982d793e89a30148aa1` |
| `model/plot_lds_doors.png` | `77c61d7501bcf5007fc893c1f22b050f5ab4855f` | `849abb576d08a2b01075a61d9ad13f23d4c5a99808b83822ac9744a3f1eac445` |
