# DLSSG for SM86 (proxy) - 0.4.1 Version

[中文](README.md) · **English**

Enables NVIDIA DLSS Frame Generation (DLSS-G) on RTX 30-series (SM86) and RTX 20-series (SM75), and accelerates DLSS Super Resolution with INT8 on RTX 30 / 20. Windows x64 / D3D12; the runtime files are `version.dll`, `dlssg_sm86.ini` and the `dlssg_sm86\` directory.

> **Upgrading from 0.3.x? Read this first: 0.4.0 is a major upgrade. The file layout and the install steps changed; do not copy it over the old version.**
>
> 1. Remove the old version: quit the game, then delete every proxy DLL of this project from the game directory (`version.dll`, plus any `winmm.dll` / `dbghelp.dll` / `dinput8.dll` / `dxgi.dll` / `d3d12.dll` you placed before) and `dlssg_sm86.ini`.
> 2. Copy the three items of the new version: `version.dll`, `dlssg_sm86.ini` and the whole `dlssg_sm86\` directory. The runtime, backend and kernel packs live in `dlssg_sm86\`; copying only the DLL does nothing.
> 3. Only one proxy at any time; if the game does not load `version.dll`, swap in one from `alternatives\` (swap, do not add).
> 4. Use the new `dlssg_sm86.ini`: keys moved (`Optimized` and `Preset` are now in `[DLSSG]`); an old INI is still read but logs warnings, so redo your changes in the new INI.
>
> The complete package (with the super-resolution files) is the release asset `dlssg-release-0.4.1-x64.zip`; for a first install follow `MANUAL.en.html` step by step. The 0.3.5 files are in `archive\0.3.5\`.

## Changes in this release


### 0.4.1

- **XeSS-FG engine (optional)**:
  - **Multi-frame generation (MFG)**: `[FrameGeneration] Engine=XeSS` goes to **2X / 3X / 4X** (`MaxGeneratedFrames` 1-3). The model is Intel's **model 15** as ported by MFG-Unlocker 1.6.0: 2X still runs model 18's graph (4-5 % more than before), 3X / 4X run model 15's own multi-frame path with the i-th generated frame of a real frame at t = i / (N + 1). The `[XeSSFG] Model` key is removed (one model); an old INI that writes it gets one warning.
  - **Tensor-core CUDA backend**: the port's trunk layers (3x3 INT8 convolutions) are hand-written INT8 IMMA kernels (`mma.sync.m16n8k16`) with `cp.async` double buffering; the whole graph runs through this project's own D3D12 + NVAPI CUDA runner on the game's command list and **never loads** Intel's `libxess_fg.dll`.
  - **`[XeSSFG] FlowResolution=CoarseAt4K`**: `Coarse` automatically when the output has at least 3840 x 2160 pixels, `Middle` at 1440p.
  - **`[XeSSFG] UseHudless` is now three-state**: `Auto` (factory, decided per frame) / `1` (strict: a frame without a usable HUD-less image is refused) / `0` (never).
  - On RTX 30 it is 8-15 % faster than DLSS-G at 1440p and below, and 27-30 % slower from 4K up (see "Performance").
- **`[SuperResolution] Mode` is now explicit presets**: `Off` (factory) / `M-INT8` / `L-INT8` / `M-FP16` / `L-FP16`; 0.4.0's `Mode=On` is removed, and an invalid value is `configuration_invalid` with super resolution not enabled.
- **`[DLSSG] Preset` is A / B / C only**: `Auto` is removed (it runs as `A`), factory `A`.
- **Keys removed and no longer read**: `Optimized` / `Preset` in their old places, `[Compatibility] AccelerateNewerGpus` / `ForceGeneratedFrames` / `SM75Family` / `HardwareBilinear` / `LaunchChains` / `PlainVariant`, `[Backends]`, and more; writing one logs a warning and the factory value applies. RTX 40 / 50 always run this project's kernels.
- **The R580 driver floor is now hard**: a CUDA 13 cubin the driver refuses is an error that refuses frame generation (`driver_below_floor` / `kernel_image_unavailable`), no longer a quiet fall back to PTX.
- **Super-resolution INT8 first-frame hitch fixed**: the twelve weight-image uploads moved off the first observed Evaluate (that Evaluate's CPU time used to be 5-6 ms higher than the same preset in FP16).
- **An explicit switch that cannot be carried out is refused**: a `[Runtime] Path` / `SRPath` naming a wrong file, a missing packaged file, `KernelImage=Cubin` on RTX 40 / 50 and the like are refused with the reason in the log, never replaced by another path.
- **Logging**: one text log, `dlssg_sm86\logs\dlssg_sm86.log` (0.4.0's per-process `frames_<pid>.jsonl` is gone); `[Logging] FrameTiming=1` writes `frame_timing.txt`.

### 0.4.0

- Super-resolution INT8 (RTX 30 / 20): with `[SuperResolution] Mode=On`, the super-resolution modes the game runs with preset L or M (Performance and Ultra Performance in most games) use this project's INT8 kernels; no preset is changed. One super-resolution evaluate takes about L −5..−17 %, M −7..−24 % GPU time against the same preset in FP16 (RTX 3070 / 3080 Ti). Off by default. The two super-resolution files are only in the complete package attached to the release (see "Install & upgrade").
- DLSS-G preset C (FG Lite, optional): `[DLSSG] Preset=C`, about −30..−37 % GPU time per generated frame at 2X and about −46 % at 4X, with slightly lower image quality (mostly on the HUD and at occlusion edges).
- XeSS-FG engine (optional): `[FrameGeneration] Engine=XeSS` generates frames with Intel's XeSS-FG model, 2X only; on RTX 30 it is slower than DLSS-G, see "Known issues". DLSS-G stays the default.
- The bundled runtime is now 310.9.16 (bit-identical output to 310.9.1 under the default preset), the only build, up to 6X; the 310.1 build is no longer provided. The factory `Optimized` changes from `1` to `2` (adds lossy image kernels; side by side there is no visible difference from `1`).
- File layout: the runtime, backend and kernel packs moved out of the proxy DLL into the `dlssg_sm86\` directory (the proxy is under 1 MB); the INI gains `[DLSSG]` / `[XeSSFG]` sections; `[Logging] FrameTiming=1` logs per-frame GPU time for performance reports.

### 0.3.5

- Fixes a defect present since 0.3.0: after the game re-creates the frame-generation feature (menu changes, a resolution or quality change, toggling frame generation; some games re-create it often while running), the wrong optimized inference kernel could be used, seen as corrupted generated frames, and possibly as a random crash / GPU driver reset after playing for a while (issue #561 may be related). `Optimized=0` is not affected. In an offline test on an RTX 3070 with 15 re-creations, the old build produced wrong generated frames in three of four runs; the new build is bit-identical in every run. On 0.3.x, just replace `version.dll`.

### 0.3.4

- Fixes the 0.3.3 crash on RTX 30 (GPU driver reset at startup or once DLSS is on; Forza Horizon 6 shows FHC01; issues #535 / #538 / #540 / #542). Cause: with the NVIDIA App DLSS override or an NGX OTA model update active, the DLSS Super Resolution model also received the GPU-architecture rewrite this project makes for Streamline, took a path that does not belong to an RTX 30 and hung the GPU. NVIDIA's own components now always get the real architecture; the rewrite applies to Streamline and the game only. On 0.3.3, just replace `version.dll`.

### 0.3.3

- RTX 20 / 30: the GPU-architecture rewrite shown to the game is now armed at game start and reports RTX 50. Games on Streamline 2.8 (e.g. Final Fantasy VII Rebirth) used to decide "this GPU does not support DLSS-G" during startup and drop the frame-generation plugin (issues #509 / #528); the rewrite is now in place before that check, and the game's 3X / 4X / 6X options unlock as on an RTX 50.
- `Optimized=1` no longer skips the repeated real-frame copy within a group (`SkipRepeatedRealCopy`; off at every level, set it to `1` yourself if wanted). It had never been validated in a live game, and a flicker report followed on 0.3.2 (issue #532). Level `1` remains bit-identical to the official image.

### 0.3.2

- Part of the inference kernels rewritten (310.9 build): the generated image now matches the official DLSS-G exactly (bit-identical in tests on an RTX 3080 Ti and an RTX 5070; an RTX 2080 Ti's output is bit-identical to the 3080 Ti's), no longer lossy, with a small speed-up (0–8% on the 3080 Ti).
- Optimization levels reworked: `[FrameGeneration] Optimized` is now `0`–`3`. `0` stock kernels, no acceleration; `1` every acceleration, image bit-identical to the official one (factory default); `2` adds lossy image kernels, faster, about 50 dB PSNR or better against the official image (310.9 build only); `3` everything lossy, fastest. See [`docs/INSTALL.en.md`](docs/INSTALL.en.md).

### 0.3.1

- Fixed frame generation not enabling on RTX 20-series (Turing). RTX 20 now works with the factory `dlssg_sm86.ini` like RTX 30, including 6X (310.9 build, `MaxGeneratedFrames=5`, game plugin permitting).
- Factory `MaxGeneratedFrames` is now `3` (4X); set `5` for 6X.
### 0.3.0

- Reverted from the native build to the proxy build. The native approach (a self-built NGX host) had game-compatibility problems that were hard to fix; this build uses a proxy DLL around the unmodified factory runtime, leaving the game's calls to NGX unchanged, which is more compatible.
- The 310.9 runtime adds 6X (`MaxGeneratedFrames` ceiling raised from 3 to 5). On games that themselves support Dynamic MFG, selecting "Dynamic / Auto" frame generation reaches 6X.
- Optimized kernel set (~19–32% in the offline benchmark), a two-switch factory INI, capture/replay, and diagnostic logging — see below and `docs/`.


## Differences from the public 0.4.0 (summary)

| Area | 0.4.0 (public) | 0.4.1 (this build) |
|---|---|---|
| XeSS-FG multiplier | 2X (model 18) | 2X / 3X / 4X (model 15 port; 2X is still model 18's graph) |
| XeSS-FG model choice | `[XeSSFG] Model=15` / `18` | one model, the key is removed |
| XeSS-FG backend | first D3D12 / CUDA runner | tensor-core INT8 IMMA kernels + `cp.async` double buffering |
| Driver floor | R580, refused cubin fell back to PTX | hard R580, refused cubin is an error |
| Super-resolution INT8 first frame | twelve weight images built during the first observed Evaluate | moved before the game starts |
| Configuration surface | several keys with no benefit or conflicting | cleaned up, explicit switches refused when impossible |
| RTX 20 / 30 numbers and performance | - | unchanged (same kernels, same bit-identical tier) |

The full ledger is `docs/COMPAT.md` C95-C131: each entry has the symptom, root cause, the fix, and the test that pins it.


## Statement & roadmap

- Kernel-level work on frame generation (DLSS-G) has essentially reached the best this project can do at this stage; kernel optimization is paused, and later releases will carry compatibility and bug fixes only.
- Super-resolution L / M INT8 ships in this release (RTX 30 / 20).
- The XeSS-FG engine (optional): the model-15 port at 2X-4X on this project's own tensor-core CUDA backend. Performance and HDR (RGBA16F) back buffers are listed under "Performance" and "Known issues".
- Vulkan support is hard to continue in the project's current state: the wrapper layer itself still has weak game compatibility and little coverage testing, and adding Vulkan on top would only bring more compatibility problems. If you need it, use one of the community patch builds.

## Requirements

- OS/game: Windows 10/11 x64, D3D12.
- GPU: RTX 30-series (SM86) or RTX 20-series (Turing / SM75). Development validation and all in-game tests on an RTX 3070; the full 0.4.0 test kit also ran on an RTX 3080 Ti; RTX 20 was tested on a 2080 Ti for 0.3.x frame generation and one smoke run of this release's super-resolution INT8.
- Driver: an NVIDIA driver **R580 or newer** (CUDA 13.0); tested on 591.86, 610.74, 610.88 and 617.14. On an older driver frame generation is not enabled and the log has one `driver_below_floor` (since 0.4.1 a refused cubin no longer falls back to PTX).
- No CUDA Toolkit and no Python.

This release has a single build (bundled runtime 310.9.16, up to 6X). The two 0.3.5 builds (310.9 / 310.1) are in `archive\0.3.5\`.


## Extra VRAM by configuration

Frame generation's own local VRAM delta (after warm-up minus before feature creation, rounded up to 10 MiB, 310.9 build, optimized). It tracks the output resolution only and is independent of the multiplier: 2X and 6X use the same amount; the multiplier costs no extra VRAM.

| Aspect | Output resolution | Extra VRAM |
|---|---|---|
| 16:9 | 720p | ~230 MiB |
| 16:9 | 1080p | ~350 MiB |
| 16:9 | 1440p | ~540 MiB |
| 16:9 | 4K / 3840×2160 | ~810 MiB |
| 21:9 | 2560×1080 | ~440 MiB |
| 21:9 | 3440×1440 | ~700 MiB |
| 21:9 | 5120×2160 | ~1050 MiB |
| 32:9 | 3840×1080 | ~610 MiB |
| 32:9 | 5120×1440 | ~990 MiB |
| 4:3 | 1920×1440 | ~430 MiB |

The same height at different widths is similar (it tracks output pixel count). This is frame generation's own increment, excluding the game itself and the synthetic inputs. With `Engine=XeSS` the dedicated VRAM in use is about 410–530 MiB higher than with DLSS-G (RTX 3070, 4K output, five games).

## Install & upgrade

For a first install, follow the manual `MANUAL.en.html` (Chinese `MANUAL.html`) at the root of the complete package step by step. In short:

1. Exit the game completely.
2. Go to the game's rendering-EXE directory (e.g. Black Myth: Wukong is `...\b1\Binaries\Win64\`).
3. Copy `version.dll`, `dlssg_sm86.ini` and the whole `dlssg_sm86\` directory; if a DLL of the same name already exists, back it up first. The easiest way is to download the release asset `dlssg-release-0.4.1-x64.zip` and unzip it: it equals this repository's root plus the two super-resolution files in `dlssg_sm86\sr\` (`sr_kernels.dll` exceeds GitHub's per-file limit and is not in the repository; without super resolution you can skip them).
4. Launch the game, enable DLSS Frame Generation in the graphics settings, and select 2X / 3X / 4X (for 6X see "6X").
5. If the game does not load `version.dll` (no `install` record in `dlssg_sm86\logs\dlssg_sm86.log` with `[Logging] Level=2` and frame generation on): delete it and try the proxies in `alternatives\` one at a time in the order `winmm.dll` → `dbghelp.dll` → `dinput8.dll` → `dxgi.dll` → `d3d12.dll`; only one at any time.
6. Upgrade: exit the game, delete the old proxy DLLs (0.3.x may have placed several; delete them all), `dlssg_sm86.ini` and `dlssg_sm86\`, then copy the three new items; redo your changes in the new INI.
7. Uninstall: delete the proxy DLL, `dlssg_sm86.ini` and the `dlssg_sm86\` directory, and put back any original DLL you backed up.

Switches in the factory `dlssg_sm86.ini` a player may want to change:

- `[DLSSG] Optimized`: optimization level `0`–`3`. Factory `2` = every acceleration plus lossy image kernels, no visible difference from `1` side by side; `1` every acceleration, image bit-identical to the official one; `0` stock kernels, no acceleration; `3` everything lossy.
- `[DLSSG] Preset`: factory `Auto`; `C` = FG Lite (faster, slightly lower quality), also on this project's accelerated kernels.
- `[FrameGeneration] MaxGeneratedFrames`: factory `3` = 4X; `5` = 6X; the actual count is requested by the game and clamped to the runtime's ceiling.
- `[FrameGeneration] Engine`: factory `DLSS`; `XeSS` = Intel's XeSS-FG model (model 15; 2X runs model 18's graph): `MaxGeneratedFrames` 1-3 (2X-4X). `[XeSSFG] FlowResolution` (factory `Middle`, finer is slower; `CoarseAt4K` opt-in) and `UseHudless` (factory `Auto`) apply to XeSS only.
- `[SuperResolution] Mode`: factory `Off`; on RTX 30 / 20, `On` runs the game's L / M super-resolution modes on this project's INT8 kernels; within one run only the first of those modes used is accelerated. Needs `dlssg_sm86\sr\` from the complete package.

Every other diagnostic/compatibility knob takes a safe default and is omitted; the full list is in [`docs/INSTALL.en.md`](docs/INSTALL.en.md). A mistyped key only falls back to its own default and logs one `configuration_warning`; it never turns the whole mod off.

## Antivirus & signing

This project's own DLLs are code-signed: `version.dll` at the root, the five alternative proxies in `alternatives\`, and the backend and three resource packs (frame-generation kernels, super-resolution kernels, XeSS model) in `dlssg_sm86\`, 10 in all; NVIDIA's runtime and super-resolution files in `dlssg_sm86\` keep NVIDIA's own signatures. A self-signed certificate only verifies the signer's identity and file integrity; it gives no default Windows trust, so Windows SmartScreen may still prompt "unknown publisher" on first run — that is a reputation prompt, not an antivirus detection. The certificate is self-signed as `CN=DLSSG for SM86 (self-signed)`, SHA-1 thumbprint `85BA66762F851E49148D706915D09026281418E6`; verify the signer and thumbprint via the file's Properties → Digital Signatures tab or `signtool verify /pa`.

## Performance

Every number below comes from this project's own timing (a D3D12 timestamp before and after each Evaluate, or the `[Logging] FrameTiming` log). Offline GPU milliseconds measure the cost of frame generation / super resolution itself; they are not an in-game frame-rate gain.

### Frame generation: real game captures (RTX 3070, offline replay)

RTX 3070, driver 617.14, the 0.4.0 test kit's full run (run_all). Inputs recorded in the games (Cyberpunk 2077, Black Myth: Wukong) replayed through the whole route (proxy → backend → NVIDIA runtime 310.9.16 or the XeSS engine); the first 4 real frames of each run dropped, median over all frames of 4 rounds. GPU milliseconds per generated frame; in brackets the change against DLSS default (negative = faster). DLSS default = the factory INI (`Optimized=2`, `Preset=Auto`); stock kernels = `Optimized=0`.

**2X**

| Capture | DLSS default | Preset C | XeSS Middle | XeSS Coarse | XeSS Fine | Stock kernels |
|---|---:|---:|---:|---:|---:|---:|
| Cyberpunk 2077 1440p | 2.554 | 1.694 (−33.7 %) | 2.974 (+16.4 %) | 2.344 (−8.2 %) | 3.828 (+49.9 %) | 3.639 (+42.4 %) |
| Black Myth: Wukong 1440p | 2.581 | 1.745 (−32.4 %) | 3.266 (+26.5 %) | 2.634 (+2.0 %) | 4.164 (+61.3 %) | 3.657 (+41.7 %) |
| Cyberpunk 2077 4K | 3.557 | 2.499 (−29.7 %) | 6.379 (+79.3 %) | 5.059 (+42.2 %) | 8.296 (+133.2 %) | 4.407 (+23.9 %) |
| Black Myth: Wukong 4K | 3.756 | 2.679 (−28.7 %) | 6.974 (+85.7 %) | 5.682 (+51.3 %) | 9.025 (+140.3 %) | 4.560 (+21.4 %) |

**4X** (per generated frame = the sum of a real frame's 3 Evaluates ÷ 3): Cyberpunk 2077 4K default 2.532 ms, preset C 1.392 ms (−45.0 %); Black Myth: Wukong 4K default 2.733 ms, preset C 1.517 ms (−44.5 %).

### Frame generation: real game captures (RTX 3080 Ti, offline replay)

RTX 3080 Ti, driver 610.88, a full run of the same test kit (a tester's machine); method as above. During these two timing stages the GPU held 1665 MHz (below the 1950 MHz of the synthetic timing), so the absolute values are conservative; the comparisons hold.

**2X**

| Capture | DLSS default | Preset C | XeSS Middle | XeSS Coarse | XeSS Fine | Stock kernels |
|---|---:|---:|---:|---:|---:|---:|
| Cyberpunk 2077 1440p | 1.651 | 1.032 (−37.5 %) | 2.235 (+35.4 %) | 1.786 (+8.2 %) | 2.791 (+69.1 %) | 2.381 (+44.2 %) |
| Black Myth: Wukong 1440p | 1.663 | 1.062 (−36.1 %) | 2.359 (+41.9 %) | 1.911 (+14.9 %) | 2.936 (+76.6 %) | 2.391 (+43.8 %) |
| Cyberpunk 2077 4K | 2.171 | 1.473 (−32.2 %) | 4.640 (+113.7 %) | 3.668 (+68.9 %) | 5.908 (+172.1 %) | 2.860 (+31.7 %) |
| Black Myth: Wukong 4K | 2.275 | 1.577 (−30.7 %) | 4.763 (+109.3 %) | 3.822 (+68.0 %) | 6.060 (+166.3 %) | 2.935 (+29.0 %) |

**4X**: Cyberpunk 2077 4K default 1.567 ms, preset C 0.822 ms (−47.5 %); Black Myth: Wukong 4K default 1.666 ms, preset C 0.897 ms (−46.2 %).

### Super-resolution INT8 against the same preset in FP16 (RTX 3070, offline)

Same test kit. GPU time of one super-resolution Evaluate in ms: median of 64 measured frames per run, median of 3 rounds. Same runtime (310.7.129), same preset; only INT8 against FP16. The two kernel families correspond to The Witcher 3 / Resonance / Forza Horizon 6 / Spider-Man (mvlo) and Black Myth: Wukong / Hogwarts Legacy (mvhi).

| Family | Output | Mode | L FP16 | L INT8 | L change | M FP16 | M INT8 | M change |
|---|---|---|---:|---:|---:|---:|---:|---:|
| mvlo | 3840×2160 | Performance | 7.443 | 6.459 | −13.2 % | 6.065 | 4.905 | −19.1 % |
| mvlo | 2560×1440 | Performance | 3.415 | 2.993 | −12.4 % | 2.787 | 2.294 | −17.7 % |
| mvlo | 1920×1080 | Performance | 1.965 | 1.736 | −11.7 % | 1.615 | 1.353 | −16.2 % |
| mvlo | 1920×1080 | Ultra Performance | 1.258 | 1.179 | −6.3 % | 1.066 | 0.967 | −9.3 % |
| mvhi | 3840×2160 | Performance | 7.305 | 6.127 | −16.1 % | 5.914 | 4.623 | −21.8 % |
| mvhi | 2560×1440 | Performance | 3.346 | 2.845 | −15.0 % | 2.719 | 2.171 | −20.2 % |
| mvhi | 1920×1080 | Performance | 1.934 | 1.656 | −14.4 % | 1.581 | 1.278 | −19.2 % |
| mvhi | 1920×1080 | Ultra Performance | 1.235 | 1.106 | −10.4 % | 1.057 | 0.914 | −13.5 % |

### Super-resolution INT8 against the same preset in FP16 (RTX 3080 Ti, offline)

Same test kit and method, GPU at 1665 MHz.

| Family | Output | Mode | L FP16 | L INT8 | L change | M FP16 | M INT8 | M change |
|---|---|---|---:|---:|---:|---:|---:|---:|
| mvlo | 3840×2160 | Performance | 5.338 | 4.578 | −14.2 % | 4.341 | 3.455 | −20.4 % |
| mvlo | 2560×1440 | Performance | 2.484 | 2.151 | −13.4 % | 2.028 | 1.644 | −18.9 % |
| mvlo | 1920×1080 | Performance | 1.465 | 1.333 | −9.0 % | 1.208 | 1.047 | −13.4 % |
| mvlo | 1920×1080 | Ultra Performance | 1.000 | 0.948 | −5.2 % | 0.849 | 0.791 | −6.9 % |
| mvhi | 3840×2160 | Performance | 5.262 | 4.355 | −17.2 % | 4.252 | 3.239 | −23.8 % |
| mvhi | 2560×1440 | Performance | 2.437 | 2.045 | −16.1 % | 1.991 | 1.545 | −22.4 % |
| mvhi | 1920×1080 | Performance | 1.441 | 1.272 | −11.8 % | 1.186 | 0.994 | −16.1 % |
| mvhi | 1920×1080 | Ultra Performance | 0.979 | 0.892 | −8.9 % | 0.848 | 0.746 | −12.0 % |

### In game (RTX 3070, 4K output, 2X)

RTX 3070, driver 617.14, logs of `[Logging] FrameTiming=1`, median of a 60 s still window after entering the world, tier 2, XeSS at `Middle`. Per generated frame = frame-generation GPU time; real frame rate = frames the game renders (the displayed rate is twice that).

| Game | DLSS default (ms) | XeSS (ms) | XeSS real fps | Preset C (ms) | Preset C real fps |
|---|---:|---:|---:|---:|---:|
| Cyberpunk 2077 | 3.755 | 6.733 (+79 %) | −8.6..−9.7 % | 2.748 (−27 %) | +3.6 % |
| Black Myth: Wukong | 4.035 | 7.308 (+81 %) | −11.0 % | 3.085 (−24 %) | +4.3 % |
| The Witcher 3 | 3.973 | 7.166 (+80 %) | −15.0 % | 2.974 (−25 %) | −0.8 % |
| A Plague Tale: Resonance | 3.943 | 6.968 (+77 %) | −28.1 % | 2.947 (−25 %) | +9.5 % |
| Forza Horizon 6 | 3.856 | 6.847 (+78 %) | −8.0 % | 2.803 (−27 %) | +7.4 % |

Cyberpunk 2077: path tracing + ray reconstruction, DLSS Ultra Performance; Black Myth: Wukong: DLSS 50 %. How the real frame rate moves depends on the game's bottleneck: in The Witcher 3 preset C saves 1 ms per generated frame and the frame rate does not change.

### Super-resolution INT8 in game (RTX 3070, The Witcher 3)

RTX 3070, driver 617.14, frame generation off, frame rate over a still window after entering the world; the baseline is the same preset in FP16 (`Mode=M-FP16` / `L-FP16`), INT8 and FP16 alternated 2–4 runs each, means in the table, gain from the medians. The preset M rows use the 0.4.0 shipped kernel sets (measured 10-10); the L rows were measured 10-09 (this family's L set has not changed since).

| Output | Mode (render) | Preset | FP16 fps | INT8 fps | Gain | Per frame |
|---|---|---|---:|---:|---:|---|
| 3840×2160 | Performance (1920×1080) | M | 47.15 | 50.85 | +7.85 % | 21.21 → 19.67 ms (−1.54) |
| 2560×1440 | Performance (1280×720) | M | 85.48 | 91.34 | +6.86 % | 11.70 → 10.95 ms (−0.75) |
| 1920×1080 | Performance (960×540) | M | 122.14 | 128.35 | +5.08 % | 8.19 → 7.79 ms (−0.40) |
| 3840×2160 | Ultra Performance (1280×720) | L | 63.56 | 66.13 | +4.0 % | 15.73 → 15.12 ms (−0.61) |
| 2560×1440 | Ultra Performance (853×480) | L | 109.42 | 111.80 | +2.2 % | 9.14 → 8.94 ms (−0.19) |
| 1920×1080 | Ultra Performance (640×360) | L | 145.83 | 147.57 | +1.2 % | 6.86 → 6.78 ms (−0.08) |

Every gain but the last exceeds twice the same-configuration repeat spread; 1080p Ultra Performance L is within one run's spread, but all 4 INT8 runs are above all 4 FP16 runs. Image quality: preset M flickers slightly more than FP16 on still content (frame-to-frame stability of still pixels in game 61.8 vs 62.5 dB) with PSNR against ground truth no lower than FP16; preset L is within 0.6 dB of FP16.

### XeSS-FG multi-frame generation (RTX 3070, offline full-route replay)

GPU milliseconds per real frame together with its generated frames (`Middle`, DIRECT):

| Output | XeSS-FG 4X | DLSS-G default | Difference |
|---|---|---|---|
| 1440p | 4.5-5.0 ms | 5.3-5.4 ms | **8-15 % faster** |
| 4K | 10.3-10.8 ms | 7.9-8.5 ms | 27-30 % slower |

- Preset C is the fastest at every size; 2X runs model 18's graph and costs 4-5 % more than the old 2X (in game on a 5070 +1.7 %, bit-identical output).
- Black Myth: Wukong, 4K in game: 3X 32.0 fps (DLSS default 34.0), 4X 28.5 fps (DLSS default 31.5).
- These are offline GPU times, not game frame rates; the real frame rate depends on whether the bottleneck is the CPU or the GPU.


### Whole generation group GPU time (RTX 3080 Ti, synthetic scene)

RTX 3080 Ti, driver 610.88, the 0.4.0 test kit (a tester's machine). GPU milliseconds of the whole frame-generation group per real frame (the span of one group's Evaluates in one submission): median of 256 groups per run, median of 4 rounds; reduction = (tier0 − current) / tier0. tier0 = stock kernels (`Optimized=0`), tier1 = bit-identical acceleration (`1`), tier2 = factory default (`2`). The same kit's consistency checks match the RTX 3070 reference bit for bit: frame generation 18/18, real captures 28/28, super resolution 32/32, super resolution `Mode=On` 4/4.

| Resolution | Multiplier | tier0 (ms) | tier1 (ms) | Reduction | tier2 (ms) | Reduction |
|---|---|---:|---:|---:|---:|---:|
| 1080p | 2X | 1.364 | 0.926 | 32.1 % | 0.917 | 32.8 % |
| 1080p | 3X | 1.942 | 1.423 | 26.7 % | 1.400 | 27.9 % |
| 1080p | 4X | 2.520 | 1.914 | 24.0 % | 1.888 | 25.1 % |
| 1080p | 5X | 3.089 | 2.411 | 21.9 % | 2.375 | 23.1 % |
| 1080p | 6X | 3.677 | 2.911 | 20.8 % | 2.867 | 22.0 % |
| 1440p | 2X | 2.094 | 1.459 | 30.3 % | 1.443 | 31.1 % |
| 1440p | 3X | 3.030 | 2.238 | 26.1 % | 2.205 | 27.2 % |
| 1440p | 4X | 3.963 | 3.008 | 24.1 % | 2.960 | 25.3 % |
| 1440p | 5X | 4.900 | 3.786 | 22.7 % | 3.719 | 24.1 % |
| 1440p | 6X | 5.841 | 4.558 | 22.0 % | 4.472 | 23.4 % |
| 4K | 2X | 2.472 | 1.901 | 23.1 % | 1.864 | 24.6 % |
| 4K | 3X | 3.792 | 3.002 | 20.8 % | 2.924 | 22.9 % |
| 4K | 4X | 5.112 | 4.106 | 19.7 % | 3.987 | 22.0 % |
| 4K | 5X | 6.437 | 5.212 | 19.0 % | 5.054 | 21.5 % |
| 4K | 6X | 7.760 | 6.319 | 18.6 % | 6.123 | 21.1 % |

The gain is larger at lower resolution and lower multiplier (closer to launch/latency bound).

## Estimating FPS after frame generation

First, in the same scene, output resolution, DLSS upscaling mode, and quality settings, turn frame generation off and read the frame rate `F_off`. Convert to a base frame time with `1000 / F_off`, then take the whole-group generation time `T_FG` (ms) from the table above for that resolution, multiplier, and kernel setting.

```text
base frame time    T_base (ms) = 1000 / F_off
group time         T_group (ms) ≈ T_base + T_FG
real-frame/group rate G (grp/s) ≈ 1000 / T_group
output frame rate  F_out (FPS)  ≈ G × M = 1000 × M / (1000 / F_off + T_FG)
```

`M` is the multiplier (2X…6X → 2…6). A group is one real frame plus `M − 1` generated frames; `T_FG` already covers the whole group and the shared preprocessing, so do not multiply it by `M − 1`. With generation on, the real-frame/group rate `G` is lower than `F_off`.

Example: with frame generation off, ~**50 FPS** (20 ms base frame time), using the **4K 4X** `T_FG` from the table:

| Kernels | T_FG (ms) | group time (ms) | group rate (grp/s) | estimated FPS |
|---|---|---|---|---|
| Stock | 5.112 | 25.112 | 39.8 | 159.3 FPS |
| Factory default | 3.987 | 23.987 | 41.7 | 166.8 FPS |

For other resolutions/multipliers use the matching row and your own measured `F_off` for that setting. This is a rough estimate — the base render time plus the generation-group cost; GPU contention, synchronization, CPU overhead, frame caps, and the monitor refresh rate all affect the real result, and the estimate is not guaranteed to equal a counter reading or the actual displayed rate.

## 6X

6X (5 generated frames per real frame) is NVIDIA's DLSS 4.5 Dynamic Multi Frame Generation. The bundled runtime supports it and this project makes it run on Ampere (with `Engine=DLSS` only); whether you get 6X depends on the game:

- The game itself supports 6X (ships a newer Streamline frame-gen plugin and offers 6X or Dynamic MFG in its menu): set `MaxGeneratedFrames=5`; 6X runs stably in testing.
- The game only supports 4X (ships an older 4X plugin, as most current games do): the ceiling is set by the game's plugin and this project cannot raise it to 6X. That plugin sizes its internal present queue for 4X at init, so forcing extra frames overruns it and disables frame generation or crashes. Use 4X for these games.

## Real-world feedback

Black Myth: Wukong, Cyberpunk 2077, and FH6 run 4X normally in testing; Resonance A Plague Tale Legacy runs 6X by default with "Dynamic / Auto" frame generation. 0.4.0 ran DLSS default, preset C and XeSS (2X, 4K output) in these four games and The Witcher 3 on an RTX 3070 with no crash, TDR or device removal. Image quality depends on the base frame rate: at a low base rate, generated frames can show breakup and edge artifacts, more so at higher multipliers (6X is more demanding than 4X), and some games need graphics settings lowered, to raise the base rate, even at 4X for a good result. This is inherent to frame generation when there is little frame-rate headroom, not a defect of this project.

## Known issues

- XeSS-FG (`Engine=XeSS`) is optional; the default is still DLSS-G. On RTX 30 (RTX 3070, offline full-route replay) it is **8-15 % faster than DLSS default at 1440p and below and 27-30 % slower from 4K up** (see "Performance"); the cost of one generated frame is still higher than DLSS-G and the real frame rate depends on the bottleneck. About 0.4-0.5 GB more VRAM. `MaxGeneratedFrames` 1-3 (2X-4X); 4 or more refuses frame generation (`max_generated_frames_unsupported`). An RGBA16F (HDR) back buffer is refused (`xefg_feature_refused`), it does not switch to DLSS-G. `[Debug] Capture` records nothing under XeSS.
- `[DLSSG] Preset=B` only takes effect when the game provides both a HUD-less image and a UI plane, which most games do not; the log then has one `preset_ineffective` record.
- Preset C has no comparison against the official preset C on RTX 40 / 50.
- Super resolution `[SuperResolution] Mode` is explicit presets: `Off` (factory) / `M-INT8` / `L-INT8` / `M-FP16` / `L-FP16`; every quality mode uses preset L / M, so Quality / Balanced / DLAA are slower than the game's own K.
- RTX 20: 310.9.16's three new kernels and XeSS were verified only by forward JIT of the sm_75 kernels on the RTX 3070 and have not run on a real Turing card; super-resolution INT8 had one smoke run on a 2080 Ti. RTX 40: the PTX frame-generation route still has no on-hardware data; RTX 50: frame generation and XeSS-FG were tested on a 5070 / 5070 Ti.

## Diagnostics & boundaries

- The log is `dlssg_sm86\logs\dlssg_sm86.log` in the game directory: every module and every process appends to this one text file, each line starting with the local time, pid, EXE name and a level letter, then the JSON record. Default `[Logging] Level=1` records errors only; use `2` or `3` when diagnosing. `FrameTiming=1` also writes `frame_timing.txt`.
- If frame generation does nothing, check `backend_*.jsonl` for an `install` line with `route active=true`; if it is absent, the driver/runtime usually did not match — the reason is logged and the factory path is used.
- This release optimizes frame generation's GPU compute cost; do not read the offline time reduction as an in-game FPS gain — the real frame-rate change depends on the game and where the bottleneck is.
- All INI keys, log fields, and capture/replay are in [`docs/INSTALL.en.md`](docs/INSTALL.en.md) and [`docs/CAPTURE.md`](docs/CAPTURE.md).

## SM75 source & credits

- Coldwood1026 for the RTX 20-series / SM75 adaptation (the kernel family behind the experimental SM75 route — see `THIRD_PARTY_NOTICES.txt`).
- NVIDIA for the DLSS-G runtime, models, and pre/post-processing (bundled, unmodified).
- Intel for the XeSS-FG shaders and model weights (MIT license, used by the `XeSS` engine; Intel's `libxess_fg.dll` is neither shipped nor loaded).
- dashdogy's [RTX40MFG-Unlock v1.4.2](https://github.com/dashdogy/RTX40MFG-Unlock/releases/tag/v1.4.2) for the method that natively extracts the MFG (model 15) model; the model-15 port and weights this build ships were obtained that way.

## License & third-party

- The project source is GPLv3.
- The bundled `nvngx_dlssg.dll` (310.9.16, SHA prefix `cadee1aa…`) and `nvngx_dlss.dll` (310.9.16, SHA prefix `858ccff9…`), the extracted/recompiled kernel resources and the SM75 kernel family are NVIDIA and upstream third-party material, not re-licensed under GPL; the XeSS-FG shaders and weights are distributed under Intel's MIT license — see `THIRD_PARTY_NOTICES.txt`.
