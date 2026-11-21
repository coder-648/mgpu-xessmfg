# Vendored MFG-Unlocker shim

The following files are adapted from **MFG-Unlocker V1.7.1**
(https://github.com/dashdogy/MFG-Unlocker, Copyright (c) 2026 Michael Robles,
MIT License):

- `runtime_layout.h`, `runtime_layout.generated.h`
- `port/model_port.h`, `port/shader_port.h`

Our changes are limited to:

- C++17 compatibility (`std::string::starts_with` / `std::map::contains` /
  `std::erase_if` replaced with C++17 equivalents);
- a directory-based payload loader (`runtime_payload.h`) instead of the
  RCDATA-embedded 54 programs;
- tensor capture file writes removed (the log still reports the spans);
- the `logEvent()` bridge to `lock_frame`'s own log.

**The 54 portable DXIL programs are not included here.** They are a separately
prepared input to MFG-Unlocker; 46 of them are derived from Intel's XeSS SDK
programs and remain subject to Intel's licence terms (see MFG-Unlocker
`NOTICE.md` / `XEFG-PROGRAMS-NOTICE.txt`). This project does not ship Intel
runtime DLLs or the program data. For a local experiment, place:

```text
<lock_frame.exe dir>\xefg_programs\programs.tsv
<lock_frame.exe dir>\xefg_programs\programs\<key>.dxil
```

Without that directory `--mfg-model=auto` logs `programs_tsv_missing` and falls
back to the legacy 2x-model unlock.

To populate it from the release DLL that is already in the tree:

```bash
python3 scripts/extract_mfgunlocker_assets.py
# -> build/xefg_programs/programs.tsv + 54 programs/<key>.dxil
#    build/xefg_programs/shaders.tsv  + 7 shaders/<name>.dxil
```

The script verifies every DXBC container and its SHA-256 against the asset
tables in `MFGUnlocker.dll` (packed program group SHA-256
`c1641e692184c278a4afb4cf0b74b8df93bf8c114cd13b1c1f55bf83cf4ddd30`).

**Do not ship `build/xefg_programs` in a release package** without reviewing
Intel's XeSS SDK terms for the derived program data; it is a local research
asset, not part of this project's MIT/GPL source.
