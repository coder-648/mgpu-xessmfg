# FG input plan (protocol v13, Streamline-first)

> Status: Phase 1 implemented (Streamline metadata/input source + protocol v13).
> Target experiment: Cyberpunk 2077, `bin/x64/Cyberpunk2077.exe`.

## 1. Goal

Read the game's own frame-generation declarations and feed the existing bridge:

```text
Streamline slSetConstants / slSetTag / slEvaluateFeature
        or DLSS-G NVSDK_NGX_Parameter (DLSSG.*)
        |
        v
FrameInputs (MV, depth, jitter, MV scale, camera, reset, clip matrices)
        |
        v
existing shared slots (MV/depth) + FrameShared v13 (metadata only)
        |
        v
bridge.exe -> XeFG
```

No HUDless/UI, no colour stream, no in-process XeFG, no swap-chain takeover.

## 2. Config

`game_capture.ini`:

```ini
[capture]
Log=1
FgSniff=1          ; diagnostics: sl.const / sl.tags / dlssg.ngx
SlInput=1          ; Streamline is the input source (name "sl")
SlFgOff=0          ; CP2077: slDLSSGSetOptions is not reachable; keep 0
FgSuppressNgx=0    ; CP2077: do NOT short-circuit; it causes black output
SkipMv=0           ; the source needs the MV stream
StreamlineCamera=1
```

Game setting: **DLSS Frame Generation off**; bridge is the only FG producer.
`Input=auto` lets the SL source win when it actually has MV/depth, and allows the
NGX source to win when Streamline exposes only constants. `Input=<other>` still
overrides it. `SlFgOff=1` / `FgSuppressNgx=1` remain experimental for titles where
the DLSS-G entry point is actually reachable; see §6.

`dlssg_sm86.ini` (only an enabler; it must not produce frames while the bridge does):

```ini
[FrameGeneration]
Engine=DLSS
MaxGeneratedFrames=1
```

Do **not** use `Engine=XeSS` for this experiment: that engine already produces
interpolated frames and would duplicate the bridge.

## 3. Data mapping

| Streamline | FrameShared v13 |
|---|---|
| `cameraViewToClip` | `view_to_clip` |
| `clipToPrevClip` | `clip_to_prev_clip` |
| `prevClipToClip` | `prev_clip_to_clip` |
| `jitterOffset` | `jitter_x/y` |
| `mvecScale` | `mv_scale_x/y` (converted to pixels-per-unit) |
| `cameraNear/Far/FOV` | `near_plane/far_plane/vfov_rad` |
| `reset` | `fg_flags` + `kInputFlagResetHistory` |
| `depthInverted` | `kInputFlagInvertedDepth` |
| `motionVectorsJittered` | `kInputFlagMvJittered` |
| `motionVectorsDilated` | `kInputFlagMvDilated` |
| `motionVectors3D` | `kInputFlagMv3D` |
| `cameraMotionIncluded` | `kInputFlagCameraMotion` |
| `FrameToken` | `frame_key` |

`bridge.cpp` uses `kInputFlagResetHistory` for `cd.resetHistory`. The clip matrices
and camera vectors are published for diagnostics/reprojection; they are not handed
to XeFG verbatim.

## 4. Safety rules

- Hooks are read-only except `SlFgOff=1`, which rewrites `slDLSSGSetOptions.mode`
  to `eOff` (the same intervention OptiScaler performs when it owns the FG output).
- Never run the game's DLSS-G and the bridge at the same time.
- Prefer `eValidUntilPresent` tags. `eValidUntilEvaluate` tags are copied at
  `on_begin_effects` in this phase; if a title reuses them earlier, move the copy
  into the SL evaluate hook.
- `record_copy_tracked()` needs `mgpu::input::track()`; the source reports the
  tag's `D3D12_RESOURCE_STATES` as the initial hint.
- The current `SlDlssgState` mirror lacks the 32-byte `BaseStructure` header; do not
  use it for spoofing until that is fixed (not needed for Phase 1).

## 5. Cyberpunk 2077 run

1. Close the game.
2. Deploy `build/game_capture.addon64` to `bin/x64`.
3. Keep ReShade on `dxgi.dll`; dlssg_sm86 on `version.dll`.
4. Set the INI keys above.
5. Start the game; turn **DLSS Super Resolution on** and **DLSS Frame Generation
   off**. Do not rely on `SlFgOff`/`FgSuppressNgx` for CP2077: they do not stop the
   SL present path and can produce a black output.
6. Check `bin/x64/game_capture.log`:
   - `config: SlInput=1`
   - `fg sniffer: SlInput=1`
   - `fg: Streamline hooks active`
   - `fg: sl.const #1 ...`
   - `fg: sl.tags #1 ... DEPTH ... MOTION_VECTORS ...`
   - `fg: SlFgOff=1 - forcing slDLSSGSetOptions mode=eOff`
   - `input source: sl (api 5)`
   - `camera: using the game's own projection matrix ...`
   - `semantics: mv=...`
   - `shared ready`
7. Start the bridge with the usual 2077 script and check:
   - `producer api=sl/streamline`
   - `projection: from the game's own view-to-clip`
   - `resetHistory=1` on scene cuts
   - `input source` is not NGX (if it is, SlInput was not active).

## 6. Cyberpunk 2077 result (2026-11-10)

First live run on `Cyberpunk2077.exe` (RTX 3080 Ti producer + RX 6750 GRE consumer):

```text
config: Color=0 ... SkipMv=0 ... FgSniff=1 Input=sl
config: SlInput=1 SlFgOff=1
fg: FgSuppressNgx=1 - returning success from the DLSS-G Evaluate without running it
input source: sl (api 5)
camera: using the input source's own view-to-clip (v13, sl)
shared: producer ... api=sl flags=0x31D1 render=2560x1440 output=3840x2160
producer semantics: mv=uv(measured) ... depth=reversed(measured)
projection: from the game's own view-to-clip ... fovY=59.70deg aspect=1.778
frame constants: ... mvscale=(3840.0000, 2160.0000) camera=producer fov=59.7deg
```

- CP2077 hands the same MV texture to NGX and Streamline; the SL `mvecScale=(1,1)`
  is misleading. The NGX `MV.Scale=(3840,2160)` side channel is used instead, and
  the online probe independently reports `mv=uv(measured)`.
- CP2077 puts **degrees** in `cameraFOV` (75.0/60.0), not radians; the SL source
  converts values > 3.2. The projection matrix is valid and is used.
- **`FgSuppressNgx=1` is NOT usable for CP2077.** It logged the short-circuit but the
  game's SL/DLSS-G present path kept running, so the display showed only the OSD /
  black and the game still ran at its FG cadence. `slDLSSGSetOptions` was not reachable
  through the hooked path in this build. The working configuration is: **turn DLSS
  Frame Generation off in the game menu**, leave `FgSuppressNgx=0` and `SlFgOff=0`,
  and let the bridge own frame generation.
- With FG off, the same 4K (`--den 1`) run is healthy on the 6750 GRE: `gpu` 2–9 ms,
  `present` 5–12 ms, 80/80 presents, `compose->photons` 14–22 ms. The earlier 4K
  slowness was the game's FG and the bridge both doing work (plus the failed
  suppression path), not 4K XeFG by itself.
- The first WGC frames can be black (`probe: 0/2304 samples`); the probe returns
  `100.0% ok` once capture is warm. Do not diagnose the pipeline from the first
  second of bridge output.

## 6.1 frame_key / WGC pairing (implemented)

`frame_key` alone is not a clock, so v13 also publishes `frame_qpc_ms` (one extra
`uint64` per slot; no texture stream). The producer stamps it at `slEvaluateFeature`;
WGC's `SystemRelativeTime` is on the same QPC base. The bridge picks the newest slot
whose `frame_qpc_ms` is at or just before the WGC time (window `-2..250 ms`) and
falls back to the old round-robin when no slot matches. `--no-pair-key` disables it.

First live match on CP2077:

```text
frame-key pair: WGC 8150986.6ms -> slot 2 (frame_key=140715473866168, age 15.6ms)
```

This is the only new consumer added for alignment; the remaining v13 metadata is
published but deliberately not used, to keep the pipeline one-to-one with XeFG.

## 6.2 The First Berserker: Khazan result (UE 4.27, 2026-11-10)

Full record: `docs/findings/FG_INPUT_V13_KHAZAN.md`.

```text
input source: ngx (api 1)          ; Streamline is DLSS-G only here (no sl.dlss.dll)
sl.tags: DEPTH + MOTION_VECTORS + HUDLESS_COLOR every frame (all onlyValidNow)
sl.const: zn=0.0100 zf=75000 fovY=76.00deg mvS=1/size mvBad=3.4e38
semantics: mv=pixels depth=reversed  ; probe: 1.155 px / 69.263 px while moving
shared: producer api=ngx flags=0x49 render=2560x1440 output=3840x2160
```

- Streamline is used **only** for DLSS-G in this title (the engine plugin ships
  `sl.dlss_g.dll` but no `sl.dlss.dll`); DLSS-SR goes through the game-local
  `nvngx_dlss.dll`, so `Input=auto` pins the **NGX** source.
- When the game's FG pipeline is still initialised, `slSetConstants`/`slSetTag`
  publish a complete, valid declaration every frame — including a `DEPTH` tag,
  which CP2077 did not provide. That declaration **stops** as soon as the DLSS
  mode is changed and the FG pipeline is rebuilt, so it cannot be the long-term
  source here.
- The SL declaration gave the camera values NGX does not export
  (`fovY=76°`, near `0.01`, far `75000`); the bridge gets them via
  `--fg-fov 76 --fg-zn 0.01` until the host publishes them automatically.
- `MV.Scale=(1,1)` (pixel-space vectors) is **correct** here: the probe measured
  `mean|v|=1.155 px` and `69.263 px` during real movement.
- 4K 2x with a 60 fps input is an exact 120 Hz cadence (`gpu` median 7.3 ms,
  `present` 7.0 ms, 80/80 presents); 3x at a 30 fps input leaves 90 fps
  (`1.33 vblank/frame`), so exact 120 there needs 4x.
- `frame_key` pairing never fires: the field is stamped in `slEvaluateFeature`,
  and a title whose own FG is off never calls it. Filling `frame_qpc_ms` in the
  host for every source would remove that restriction.

## 7. Known limits

- MV scale uses a heuristic (`sl_mv_scale_px`); verify with `probe:` and
  `--mv-shadow`, override with `--fg-mv-scale`.
- No HUDless/UI: the bridge still uses WGC colour and UI interpolation.
- The Streamline source wins `Input=auto` when `SlInput=1`; if it never produces,
  use `Input=ngx` for the baseline.
