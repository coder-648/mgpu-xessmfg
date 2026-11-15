# FG backend: our own frame-generation provider

> Status: design, 2026-11-12. Replaces the "interrupt someone else's backend and fill its output"
> approach, which cannot work (see §1).
>
> **Superseded in part (2026-11-12, later the same day).** The role analysis in §1-§7 stands, but
> the *placement* does not: the provider and the XeFG backend live in a separate module the game
> loads first (`fakerFG.dll`), not inside the ReShade add-on - the add-on cannot win the
> `GetProcAddress` race (§7 of the new doc). §9-§10 below are history.
> Current architecture: [`FAKER_FG.md`](FAKER_FG.md).

## 1. Why this shape, and why the previous one could not work

What we tried first: let the title's own FG runtime run, short-circuit its evaluate, then write the
native frame into `DLSSG.OutputInterpolated` so the picture survives. Measured on CP2077:

* the fill itself worked exactly once (`wrote the native frame into DLSSG.OutputInterpolated`);
* it then removed the device, because the only states available to us were ReShade's `general`
  pseudo-state (`0x80000000`) and our own restored value (`0x800`), not the state the title's
  swap-chain textures are actually in.

That is not a bug to fix - it is the consequence of *not owning the frame*. An observer cannot
legitimately transition resources whose lifecycle it does not control, and it cannot provide the
picture that the runtime is supposed to produce.

OptiScaler solves it structurally: it **is** the FG backend. `framegen/nvngx/Nvngx_FG.cpp` proxies
the DLSS-G NGX entry points, so the title's `CreateFeature`/`EvaluateFeature` calls land in
OptiScaler; its backends then either forward to NVIDIA
(`framegen/dlssg/DLSSG_Dx12.cpp`), run FSR-FG (`Nvngx_FFX.cpp`) or run XeSS-FG and **own the
swapchain** (`XeFG_Dx12.cpp::CreateSwapchainInternal`). Two details are the whole answer:

* `Nvngx_FFX.cpp` around line 812 does exactly the "put the real frame where the presenter looks"
  copy we attempted - `api.CopyTexture(cmdList, &outputRealFfx, &dispatchDesc.presentColor)` - but
  it can, because it hands its own backend the resources with **declared** states
  (`FFX_API_RESOURCE_STATE_UNORDERED_ACCESS` / `COPY_DEST`) and the backend emits the barriers. The
  authority over the transition is ours, not the title's.
* `XeFG_Dx12` additionally creates the swap chain itself, so the present path is OptiScaler's. The
  picture never depends on what the title does with its own buffers.

Conclusion: to get (a) complete inputs and (b) a generation-free "idle" that still shows a correct
picture, we have to be the provider. Everything else is a workaround around a missing ownership.

## 2. What "being the provider" means concretely

We already intercept every relevant export; the change is that we stop *forwarding* and start
*answering*.

| export | what we do |
|---|---|
| `NVSDK_NGX_D3D12_GetCapabilityParameters` | report `FrameGeneration.Available=1` and the `MultiFrameCountMax` we support (1 unless MFG is wanted); read-only for everything else |
| `NVSDK_NGX_D3D12_GetFeatureRequirements` | DLSS-G supported on this adapter |
| DLSS-G `PopulateParameters` | the callbacks a runtime must provide (`GetCurrentSettingsCallback`, `EstimateVRAMCallback`; OptiScaler's values are the reference) |
| `NVSDK_NGX_D3D12_CreateFeature` | allocate **our** handle (magic-tagged, outside any real runtime's range), remember the device |
| `NVSDK_NGX_D3D12_EvaluateFeature` | the whole pipeline, see §3 |
| `NVSDK_NGX_D3D12_ReleaseFeature` | free our handle |
| `slGetPluginFunction` / `slDLSSGSetOptions` / `slDLSSGGetState` | keep answering, so the title's menu keeps showing FG "on" |

The title then *must* supply the contract data - which is exactly our input side:

```text
DLSSG.Backbuffer            colour of this frame (and, for us, the colour source we never had)
DLSSG.MVecs / Depth         the two streams we transfer
DLSSG.HUDLess / UI+UIAlpha  when the title has them
DLSSG.OutputInterpolated    where the generated frame must land
DLSSG.OutputReal            the real frame the presenter shows
DLSSG.CameraViewToClip / ClipToPrevClip / PrevClipToClip
DLSSG.JitterOffsetX/Y, MvecScaleX/Y, DepthInverted, ColorBuffersHDR, Reset,
DLSSG.MultiFrameCount / MultiFrameIndex
```

We already log all of these (`fg: eval params`, the DLSSG struct dump); nothing here is new
information, only a new role.

## 3. Evaluate: the three things it must do, in order

```text
1. harvest        copy MV/depth (+ metadata) into the shared block for the second GPU  [works today]
2. produce        generate locally (single-GPU mode) or generate nothing (idle mode)
3. present-ready  write the frame the presenter must show into OutputReal /
                  OutputInterpolated - a plain copy of Backbuffer when we generate nothing
```

Because we are the provider, step 3 is legal and safe: the resources arrive through the parameter
block with the title's promise about their use, and we are the one recording the pass, so the
transition is ours to declare (the same pattern as OptiScaler's `api.GetResource(..., STATE)`).
It is also cheap: one backbuffer->output copy, ~0.1-0.2 ms at 4K.

`FgMode` keeps its meaning and now maps onto our own behaviour instead of the title's:

| FgMode | our Evaluate | who owns the picture |
|---|---|---|
| `off` | not called (title's FG off) | the title |
| `observe` | forward to the real runtime (we are a proxy) | the real runtime |
| `consume` (idle) | harvest + copy Backbuffer -> OutputReal/OutputInterpolated, generate nothing | **us** - correct picture, zero generated frames |
| `local` (new) | harvest + generate with XeFG on this GPU | us |

`local` is worth having even though it is not the product: it is a single-GPU end-to-end test of
exactly the code path the bridge will take later.


## 4. What this buys, and what it costs

Buys:

* a complete input contract (§2) instead of opportunistic hooks;
* a generation-free idle that still presents a correct picture - what three attempts at
  "interrupt and fill" could not achieve;
* the colour stream we never had: `DLSSG.Backbuffer` arrives every frame, so the WGC dependency can
  be dropped later, and with swap-chain ownership the picture becomes ours end to end;
* a natural place for the second-GPU hand-off: step 2 is the only thing that differs between
  `local`, `idle` and "consume from the bridge".

Costs and risks:

* the NGX ABI surface has to be faithful; titles probe it and behave differently (OptiScaler's
  `Nvngx_*` files are the reference, and they are in our tree);
* handle semantics must be distinguishable from any real runtime's (OptiScaler offsets its ids for
  exactly this reason);
* a module named like the real one (`nvngx_dlssg.dll`) is the usual practice for this class of
  tool, but game/anti-cheat integrity checks are a real consideration;
* swap-chain ownership (§5, phase 3) brings resize / HDR / tearing / VRR handling;
* a fallback must stay: one switch that hands the title back to the real runtime for an A/B.

## 5. Phases, each with an acceptance test

| phase | work | acceptance (CP2077, 4K DLAA) |
|---|---|---|
| 0 | inventory: which parameters the title fills, which exports it calls, in what order | a table from an existing session; no new code |
| 1 | provider skeleton: our handle, Create/Release, Evaluate that harvests and copies Backbuffer -> OutputReal/OutputInterpolated, generates nothing | picture correct, fps back to native (~33), log says `generated=0` + `wrote real frame` |
| 2 | `local` mode: XeFG on the render GPU through our module | ~2x with our own present path, title FG not involved |
| 3 | swap-chain ownership (port the shape of `XeFG_Dx12::CreateSwapchainInternal`) | picture and pacing are ours; colour no longer needs WGC |
| 4 | second GPU: step 2 hands the inputs to the bridge and presents what comes back | render card at native pace, frames produced on the other GPU |

Phase 1 is the decisive one: it proves the ownership claim, and everything after it is incremental.

## 6. What we keep from the current producer

Unchanged and reusable as-is: the tag/parameter harvest (`in_fg.cpp`, `in_ngx.cpp`), the capability
model (`shared_safe_format` + per-stream absent slots), the shared block and its 1:1 transport, the
`streams:` byte accounting, and the rules in `CAPTURE_RULES.md`. The list-type rule becomes largely
moot once we own the pass, but it stays as a guard for anything still recorded on a title's list.


## 7. Reference implementation in the tree (and the licence caveat)

OptiScaler already ships exactly the pipeline we need: **DLSS-G / Streamline as the input side, XeFG
as the output side**. It is in `temp/optiscaler_src/OptiScaler-master/OptiScaler/`:

| file | role | our equivalent |
|---|---|---|
| `inputs/FG/Streamline_Inputs.cpp`, `Streamline_Inputs_Sl1_Dx12.cpp` | hook `slSetTag` / `slEvaluateFeature`, turn tags into resources with a validity class | `in_fg.cpp` (tag lane) - already ours |
| `inputs/FG/Upscaler_Inputs_Dx12.cpp` | hook the upscaler (DLSS-SR / FSR / XeSS) for MV + depth | `in_ngx.cpp`, `in_ffx.cpp`, `in_xess.cpp` |
| `inputs/FG/XeFG_Inputs_Dx12.cpp` | adapter: pushes harvested resources into XeFG's swap-chain tags (`xefgSwapChainD3D12TagFrameResource`) | new: `FgBackendInputs` |
| `framegen/IFGFeature.h` | the seam: `FG_Constants`, `FG_ResourceType`, `FG_ResourceValidity`, the backend interface | new: `FgBackend.h` |
| `framegen/xefg/XeFG_Dx12.cpp` | the output side: owns the swap chain (`CreateSwapchainInternal`), `CreateContext`, `EvaluateState`, `Present()`, dynamic interpolation count | new: `FgBackendXeFG.cpp` |
| `framegen/nvngx/Nvngx_FFX.cpp` | reference for "declare the state you want, let your own backend barrier it" (`api.GetResource(..., FFX_API_RESOURCE_STATE_...)`) | pattern for our Evaluate |
| `framegen/nvngx/Nvngx_FG.cpp` | reference for provider semantics: own handle range, forward or answer, `PopulateParameters` | our provider skeleton (phase 1) |

The two ideas worth taking verbatim (as *technique*, not as code):

1. **Validity classes** (`ValidNow`, `UntilPresent`, `ValidButMakeCopy`, `UntilPresentFromDispatch`) -
   a harvested resource is only usable inside a window, and the backend must know which. We already
   carry `onlyValidNow` / `validUntilPresent` in the tag lane; making it a first-class field next to
   the resource is what lets the output side decide "copy now" vs "skip".
2. **The output side owns the swap chain.** `XeFG_Dx12::CreateSwapchainInternal` creates the chain
   and `Present()` drives it; the title's own present never participates. That is the only way the
   picture is guaranteed, and it is also where the colour comes from without WGC.

### Licence

`third_party/optiscaler/LICENSE` is **GPL-3.0**, and this project is fully open source, so porting
the code is allowed - but it is a binding choice, not a detail: anything ported from OptiScaler
stays GPL-3.0 and the combined work has to satisfy that licence. Keep the provenance visible:
files derived from OptiScaler carry a header saying which file they came from, and
`third_party/optiscaler/LICENSE` stays in the tree.

What this changes in practice: the port can be literal (adapt to our names, keep the logic), which
is exactly what a validated pipeline deserves. It also means we do not have to re-derive behavioural
details - `FG_ResourceValidity`, the swap-chain ownership and the per-frame tag flow come with
working code instead of from memory.

## 8. Phase 0 inventory (what CP2077 actually uses)

From every CP2077 session log we have (`game_capture*.log`, 5 files):

| item | observed |
|---|---|
| `CreateFeature` | called **once** per session, `feature=11`, output 3840x2160 |
| `EvaluateFeature` | every frame; the parameter block carries the full input set |
| resources in the block | `Backbuffer`, `MVecs`, `Depth`, `HUDLess`, `UI`, `UIAlpha`, `OutputInterpolated` |
| scalars in the block | `CameraViewToClip`, `ClipToPrevClip`, `PrevClipToClip`, `JitterOffsetX/Y`, `MvecScaleX/Y`, `CameraNear/Far/FOV`, `DepthInverted`, `ColorBuffersHDR`, `Reset` |
| exports the title reaches | `NVSDK_NGX_D3D12_{GetCapabilityParameters,GetFeatureRequirements,CreateFeature,EvaluateFeature,ReleaseFeature}` on the DLSS-G module, and `slGetFeatureFunction(slDLSSSetOptions)` / `slGetPluginFunction` on the Streamline side |
| **not** observed | `slDLSSGSetOptions` was never resolved on CP2077 (`sl.interposer does not export slDLSSGGetState`), so `Claim`/`SlFgOff` cannot act here - the title simply never asks |
| unknown yet | `OutputReal`, `BidirectionalDistortionField`, `OutputDisableInterpolation`, `MultiFrameCount`/`MultiFrameIndex`, `ResourceAlwaysProvidedFlags` |

To close the "unknown" line the producer now prints one **inventory** line per session, listing
every candidate parameter that arrived and the multi-frame scalars:

```text
fg inventory: resources=[Backbuffer MVecs Depth HUDLess UI UIAlpha OutputInterpolated ...]
              MultiFrameCount=? MultiFrameIndex=? AlwaysProvided=0x? NeverProvided=0x?
```

That line is the input to the provider's minimal surface: implement what is listed, ignore what is
not, and no title-specific guesswork enters the design.

## 9. Complete implementation plan (porting, not re-deriving)

The project is open source, so OptiScaler's code may be ported as-is (GPL-3.0, see 7). The order
below follows the dependency graph, and each step is a compiling tree.

### 9.1 What is already in place

| piece | state |
|---|---|
| `src/game_capture/fg_provider.{h,cpp}` | **new**: provider handle space, `Create/Release/Evaluate`, `ModeFromConfig`, `IBackend` seam, provenance note |
| wiring in `in_ngx.cpp` | **new**: `CreateFeature`/`EvaluateFeature`/`ReleaseFeature` answer through the provider when `FgMode` is idle/local/bridge; everything else still forwards |
| XeFG API + loader | already in tree: `third_party/xess/inc/xess_fg/*`, `src/xefg_dyn.h` (full `xefgSwapChain*` surface), `xefg_unlock.h`, `xefg_dynamic_count.h` |
| inputs | already in tree: tag lane (`in_fg.cpp`), NGX/FFX/XeSS lanes, the shared block, the 1:1 transport |

### 9.2 The mechanism that removes the state problem

`xefg_swapchain_d3d12_resource_data_t` carries `incomingState` (a plain `D3D12_RESOURCE_STATES`):

```c
xefg_swapchain_result_t xefgSwapChainD3D12TagFrameResource(hSwapChain, pCmdList, presentId,
                                                           const xefg_swapchain_d3d12_resource_data_t*);
```

So the backend is *told* what state each resource is in and barriers from there - the same "declare,
never guess" model OptiScaler uses with `FFX_API_RESOURCE_STATE_*`. This is the piece our earlier
`MgpuCaptureFillInterpolated` experiment lacked: it had to read states back out of the title.

### 9.3 Port order

| # | port from | to | notes |
|---|---|---|---|
| 1 | `framegen/xefg/XeFG_Dx12.cpp` (context, `EvaluateState`, `Present`, `SetNumInterpolatedFrames`, `SetEnabled`) | `src/game_capture/fg_backend_xefg.cpp` | uses our `xefg_dyn.h` Api struct instead of their proxy class |
| 2 | `inputs/FG/XeFG_Inputs_Dx12.cpp` (`TagFrameResource`) | same file | maps our `FrameInputs` (mv/depth/hudless/ui + validity) to `xefg_swapchain_d3d12_resource_data_t` |
| 3 | present interception | `fg_backend_xefg.cpp` via ReShade `addon_event::present` (we already register it) | XeFG presents; the title's own present becomes a no-op for the FG path |
| 4 | `framegen/IFGFeature.h` remainder (`FG_Flags`, dispatch desc helpers) | `fg_provider.h` | only what the backend actually needs |
| 5 | swap-chain creation (`XeFG_Dx12::CreateSwapchainInternal`) | optional last step | only needed if `InitFromSwapChain` on the title's own chain is not enough |

### 9.4 What each mode needs

| mode | requires | picture comes from |
|---|---|---|
| `idle` | backend #1-#3 with `SetEnabled(false)` | XeFG presents the tagged real frame (no interpolation) |
| `local` | same, `SetNumInterpolatedFrames(n)` | XeFG generates on this GPU |
| `bridge` | #1-#3 plus the shared block | the second GPU generates; XeFG presents what arrives |

So `idle` is not a separate mechanism - it is the same output path with generation switched off. That
is why `MgpuCaptureFillInterpolated` is no longer the plan: it was trying to do, weakly and from the
outside, what the backend does cleanly from the inside.

### 9.5 Current honest state

Provider skeleton is in place and compiling (`ce69c35db763`), but the XeFG backend is not written yet,
so `FgMode=idle` would answer the title and then present nothing. Both titles therefore stay on
`FgMode=off` / `observe` until backend step #1 lands.

## 10. First complete shape (what is in the build now)

`1b4866f21c9d` compiles and ships the whole chain, with one deliberate safety property:

```text
provider (fg_provider.cpp)      answers CreateFeature/EvaluateFeature/ReleaseFeature for DLSS-G
                                (our handle space, magic-tagged)
execution side (fg_backend_xefg.cpp)
                                libxess_fg loaded at runtime, context from the title's own
                                device + direct queue + swap chain,
                                per-frame TagFrameResource with the state WE declare,
                                SetEnabled(on/off) + SetNumInterpolatedFrames(n)
mode mapping                    idle  -> XeFG with generation OFF (real frame presented)
                                local -> XeFG with generation ON  (single-GPU test path)
safety                          if the presenter cannot start (DLL / chain / queue missing),
                                CreateFeature is forwarded to the title's own runtime instead
                                of taking over and showing nothing
```

Objects captured from the add-on events (no hooks of our own): `g_game_swapchain`
(`init_swapchain`), `g_fg_queue` (`init_command_queue`, direct only), `g_game_device`.

### What is still missing before `FgMode=idle` is usable

1. **present interception.** XeFG must be the one presenting. Today we tag in Evaluate and let the
   title present; XeFG needs `xefgSwapChainSetPresentId` before that present and the library must be
   the presenter for the chain (OptiScaler calls it from its present hook). ReShade gives us
   `addon_event::present` - the tag+SetPresentId call belongs there, right before the title's present.
2. **resource sizes.** MV/depth are taken from `g_inputs.valid_*` today; for a title that declares
   only extents in the tag, that is the same number, but it must be the *stream* size once the
   producer narrows or the backend will read the wrong rectangle.
3. **validation order.** `D3D12InitFromSwapChain` wants the chain idle; calling it from
   `CreateFeature` (mid-frame) has not been proven on either title yet - the first run answers it.

### How to test, in order

| step | ini | expected log | expected picture |
|---|---|---|---|
| a | `FgMode=local` | `libxess_fg loaded`, `XeFG version`, `max interpolations`, `context ready`, `generation ON (local)` | correct, ~2x |
| b | `FgMode=idle` | same but `generation OFF (idle)` | correct, native fps |
| c | back to `off` | `provider NOT taking over` never appears | correct, native fps |

If step a shows `provider NOT taking over`, the log names which object was missing - that is the
next thing to fix, and the title keeps its own FG meanwhile (no black screen).
