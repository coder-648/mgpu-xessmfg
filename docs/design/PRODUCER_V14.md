# Producer v14 — back to ReShade, by decision

> Status: step 1 landed 2026-11-12. The producer compiles and runs as a pure ReShade add-on
> again; the own-host layer (`src/legacy/own_host`, `src/legacy/version_proxy`) is retired and
> nothing in `src/game_capture` refers to it any more.

## 1. Why the own host was abandoned

The own host existed for exactly one reason: **the architecture answer Streamline asks before
`sID3D12Device` exists** (`NvAPI_GPU_GetArchInfo`). A ReShade add-on loads at swap-chain creation,
i.e. after `slInit`, so it could not answer it, and a `version.dll` proxy could.

That reason is gone. `third_party/dlssg_for_sm86-main` is itself a `version.dll` that (a) unlocks
DLSS-G on RTX 20/30 and (b) already rewrites the reported architecture for Streamline and the game
(its own release notes: NVIDIA components are deliberately excluded from the rewrite). Two proxies
cannot both be `version.dll`, and ours was the weaker of the two: it could only *claim* support,
while sm86 makes the title actually create a frame-generation feature - which is what an
NGX-stream title needs before it publishes any MV/depth at all.

Everything else the own host did was a liability:

| own host | consequence |
|---|---|
| wrapped device/queue/list objects handed to the game | Wuchang crashed, hung, then refused to start (`CAPTURE_RULES` 1) |
| MinHook installed during engine start-up | thread suspension deadlocks (`CAPTURE_RULES` 3) |
| no barrier events | "state unknown" meant "never seen", so every transition was a guess |
| a timer thread driving allocation and drains | two paths per frame, neither of them ReShade's |

The ReShade add-on gets all of it for free and *observed*: `addon_event::barrier` is a real
transition the title issued, `reshade_begin_effects` is a point in the frame that is off the
submission path, and the objects stay the driver's.

## 2. What step 1 changed in code

`src/game_capture/game_capture.cpp` had eight `#ifdef MGPU_OWN_HOST` sites. All eight are gone:

| site | before | after |
|---|---|---|
| `tracked_state()` | own host refused the hint and returned 0 | hint allowed: ReShade *does* observe barriers, so "declared but never observed" means the title never transitions the resource (CP2077 keeps MV in UAV all session) |
| `notify_sources()` | own host drained from a timer thread | queued only; `on_begin_effects` drains (loader lock never held) |
| shared allocation | frame *asked* a host thread, frames skipped meanwhile | `ensure_shared()` inline - `reshade_begin_effects` is already off the submit path |
| module rescan | 600-frame cadence plus a host drain | 120-frame cadence when `LoaderHook=0`, otherwise event-driven |
| `AddonInit` | own host scanned at init | scan stays deferred to the first frame (never a host concern) |
| host exports (9 functions) | `MgpuHostDrainModules`, `MgpuHostRecordPresentCopy`, `MgpuCaptureRecordPending`, `MgpuCaptureHasPending`, `MgpuHostEnsureShared`, … | deleted |
| parked copies (`CopyAtFrameEnd`) | park here, host drains at submit | deleted; the inline tag path is the only one |
| `g_shared_wanted` / `NoTick` / `NoShared` / `TransferEveryN` | host handshake keys | deleted |

What was **promoted out of the ifdef** instead of deleted is the tag lane:

```text
in_fg.cpp  (slSetTag hook)
   -> MgpuCaptureRecordFgInputs(list, mv, depth, hudless, ui, declared_state)
        barrier(src: declared -> COPY_SOURCE)   /* only with an observed or declared state */
        CopyTextureRegion -> shared slot (MV/depth, optional colour/UI)
        barrier(src: COPY_SOURCE -> declared)
        note_stream_copy(...)                   /* byte accounting */
```

It never needed a host: `slSetTag` hands over the resource *and* the open command list it will be
consumed on, so recording inline into that list is what keeps ordering and lifetime the game's
business. It is also the path with the CP2077 mileage (34 fps at 4K, 0 drops, 0 crashes).

## 3. Target architecture

```text
ReShade (dxgi.dll)                              our add-on (game_capture.addon64)
  device / queue / list wrappers  ──events──►   barrier / begin_effects / present
                                                       │
  sl.interposer (slSetTag) ──MinHook──►  in_fg.cpp ────┤ FrameInputs: MV, depth, camera, jitter,
  nvngx (DLSS-SR Evaluate) ──────────►  in_ngx.cpp    │ matrices, reset, validity
  ffx / libxess_fg ─────────────────►  in_ffx/xess    │
                                                       ▼
                                     downsample (StreamPercent) -> shared slots -> publish
                                                       │
dlssg_sm86 (version.dll)                              ▼
  arch unlock / DLSS-G enabler            bridge.exe (second GPU, XeFG) + WGC colour
```

Split of responsibility, by decision (2026-11-12):

| data | where it is produced | why |
|---|---|---|
| MV, depth | producer, `StreamPercent` tier | the only inputs XeFG consumes; cost is `bytes / ~13 GB/s` |
| camera / jitter / clip matrices / reset | producer (metadata only) | free, removes the consumer's guessing |
| colour | **consumer, WGC** | +56 MB/frame in the game's own list cost ~2 fps and bought nothing |
| UI | consumer (WGC, plus HUDless when a title offers it) | same reason |
| interpolated frames | consumer (second GPU) | the whole point of the split |

`StreamPercent` stays a producer key because the downsample happens where the data is: 100 = full,
200 = half (default), 400 = quarter. The copy engine cannot scale, so the pass runs on the render
GPU - and that is a *saving*, because the bytes it removes never cross the bus.

## 4. The scenario that was tried and what it proved (2026-11-12, CP2077)

The intended flow is: game FG on -> capture the FG inputs -> interrupt the game's generation ->
second GPU produces the frames.

Measured on CP2077 with `dlssg_sm86` present:

| step | result |
|---|---|
| the declaration with FG on | complete: 415 tag sets, `MOTION_VECTORS`/`SCALING_INPUT`, and the DLSS-G parameter block **with a real MV pointer** (20 of 46 evaluates; the 2026-11-12 survey's `dlssg_mv=0x0` was an artifact of FG never running) |
| the interrupt (`FgMode=consume`) | fires (`short-circuit = 1`), copies keep flowing at 300/300, 15.8 MB/frame |
| the picture | **black** |

Why: the title's present path *is* the DLSS-G runtime. CP2077 hands the runtime `outInterp`
(`NVSDK_NGX_DLSSG_Parameter_OutputInterpolated`, 3840x2160) and presents whatever comes back.
Returning success without running the evaluate therefore presents a texture nobody wrote.

So "keep the declaration, stop the generation" is not available for a title whose FG runtime owns
present. It could only work by writing the real frame into `outInterp` ourselves, which needs the
observed states of two swap-chain resources - allowed by rule 4, but not implemented.

The three modes and where each one is valid:

| FgMode | the title's FG | who generates | valid on |
|---|---|---|---|
| `off` (default) | off | the bridge | everything; on CP2077 the tag lane is still complete (measured twice: identical MV/depth bytes) |
| `observe` | on | the title | everything; the render card pays for generation (33 -> 48 fps at 4K DLAA) |
| `consume` | on | nobody after the interrupt | only titles whose FG runtime does not own present (FFX-FG / XeSS-FG class); black on DLSS-G-owns-present titles |

The `Claim` / `FgCut` / `FgSuppressNgx` / `SlFgOff` keys still override what the mode implies, and an
explicit value in the ini always wins - a title that misbehaves under a mode is one edit away from
the other two, not a rebuild.



## 5. Coexistence rules (the part that is easy to get wrong)

* **One proxy name per file.** ReShade = `dxgi.dll`, sm86 = `version.dll`. sm86 ships
  `alternatives\{dxgi,d3d12,winmm,dbghelp,dinput8}.dll` for titles that do not load `version.dll`;
  using `alternatives\dxgi.dll` would displace ReShade and the add-on would silently never load.
* **sm86's own frame generation must be off.** Its `[FrameGeneration] Engine=XeSS` produces
  interpolated frames itself, which would duplicate the bridge. `deploy_producer.sh` warns.
* **MinHook is first-come-first-served per address.** A second `MH_CreateHook` on the same
  `NVSDK_NGX_D3D12_EvaluateFeature` fails (`MH_ERROR_ALREADY_CREATED`). sm86 hooks the DLSS-G
  library in the same process; our NGX source tolerates a pre-existing hook by borrowing it
  (`in_ngx.cpp`, `evaluate_thunk_for`), and `FgNgxHooks=0` is the escape hatch if a title's load
  order makes the two fight.
* **Load order** is ReShade's: `AddonInit` → first frame → module scan. That is why the scan is
  deferred: doing it inside add-on init wedged Wuchang on 2026-11-10.

## 6. Performance notes carried over

* Cost is **per frame that copies**, linear in bytes: `cost ≈ bytes / 13 GB/s`. Two streams in the
  same frame are not additive in any other way, and queue/position/synchronisation make no
  difference (measured; `docs/design/TRANSFER_COST.md`).
* The producer stays off the submission path: `ensure_shared()` and the module scan run in
  `reshade_begin_effects`; the tag lane only records into a list the title already opened.
* `LoaderHook=0` exists because patching `kernel32!LoadLibrary*` is the prime suspect for slow
  start-up on RE Engine titles; it trades immediacy for a 120-frame rescan.

## 7. What is left

| # | item | note |
|---|---|---|
| 0 | **the DLSS-G parameter block as a source** (`kInputApiNvngxFg`) | the id exists, no source implements it. Today a title that declares MV/depth only through the NGX DLSS-G params has no FG lane, which is exactly the "reliable data while the game generates" case the policy in §4 asks for |
| 1 | rule 5 (our own list appended through `addon_event::execute_command_list`) | the retired `CopyAtFrameEnd` design, now implementable with observed states and ReShade's flush point |
| 2 | `frame_qpc_ms` for every source, not just `slEvaluateFeature` | `frame_key` pairing never fires on a title whose own FG is off |
| 3 | colour stream removal from the code path | stays as an off-by-default key until the UI/HUDless work needs it |
| 4 | NGX-stream titles (Wuchang) with sm86 actually enabled | the survey ran without it, which is why those titles showed zero tags |
| 5 | verify on CP2077: tags, streams, 34 fps | the add-on path has not been run end-to-end since the own host was retired |

