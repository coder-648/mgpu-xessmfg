# fakerFG input/output pipeline

This follows OptiScaler's split: an input lane only produces a `MgpuFgFrame`; an
output sink only consumes it. The two are selected independently.

```ini
FgInput=Upscaler   ; Upscaler | DLSSG | NVNGX | FSR
FgOutput=Capture   ; Capture (producer/consumer) | XeFG (local presenter)
Passthrough=0      ; 1 = hand everything to the real runtime
```

`FgOutput` is optional. An explicit `FgInput` without `FgOutput` resolves to
`Capture`, so the lock_frame producer is the only consumer. `FgOutput=XeFG`
enables the local XeFG presenter.

There is no user-facing `Mode`. `Passthrough=1` replaces the old off/observe
modes. `MgpuFgFrame.mode` remains on the API wire for `producer.cpp`'s
`MGPU_FG_MODE_OFF` check and is derived from `Passthrough` + `FgOutput`.

## Flow

```text
proxy_streamline.cpp  (DLSSG)     \
provider.cpp          (NVNGX)      >-- MgpuFgFrame --> publish_record() --> producer
proxy_ffx.cpp         (FSR)       /                    \
proxy_nvngx.cpp       (Upscaler)                        `-> fg_output_submit()
                                                              |
                                                    FgOutput=XeFG -> xefg_tag()
                                                    FgOutput=Capture -> no game FG
```

- `publish_record()` is the transport sink and is always fed when publishing is on.
- `fg_output_submit()` is the only place an input lane is allowed to tag XeFG.
- `xefg_wants_generation()` requires `FgOutput=XeFG`; `Capture` can never activate
  the XeFG generator.
- `Capture` is input-only: the NVNGX provider keeps `OutputReal` valid but never
  writes `OutputInterpolated`, so the render GPU does not execute a second FG.
- `api_mode()` maps `Passthrough=1` to `MGPU_FG_MODE_OFF`, `FgOutput=XeFG` to
  `MGPU_FG_MODE_LOCAL`, and `FgOutput=Capture` to `MGPU_FG_MODE_BRIDGE`.

## Current output adapters

- `Capture`: producer/consumer only, no game-side frame generation.
- `XeFG`: local XeFG presenter (`backend_xefg.cpp`).

New outputs (DLSSG / FSRFG) belong behind the same `fg_output_submit()` route; they
must not be added as conditions inside the input lanes.

## Stream 3 (UI) in v29

`producer.cpp` publishes one optional auxiliary stream (`kStreamUi`, index 3). Its
payload is selected by `fakerFG.ini`:

```ini
UiStream=1
UiSource=Auto      ; Auto | Game | Hudless | Off
```

- `Game`: the title's `UI_COLOR` / `UI_COLOR_AND_ALPHA` tag, downsampled on the render
  card; the consumer unblends it with the WGC present.
- `Hudless`: a downsampled `HUDLESS` image; the consumer synthesises an approximate UI
  (`d = present - hudless`, `m = smoothstep(t, 2t, max|d|)`, `U = P - (1-m)H`, `a = m`)
  and then unblends. Used when the title has no UI tag.
- `Off`: no stream 3.

The producer only runs the existing single `downsample_ui` pass; it never adds a second
full-screen pass and never transports full-resolution HUDless. The consumer tags the
rebuilt HUDless and XeFG uses `BACKBUFFER_HUDLESS` (mode 4). See
[`UI_SOURCE_V29.md`](UI_SOURCE_V29.md).
