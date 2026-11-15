# Capture-side rules (ReShade model) - binding for this repo

Written 2026-11-12 after a day of CP2077 working while Wuchang Fallen Feathers crashed, then hung, then
failed to start. Every one of those failures traced back to the same habit: **we replaced objects the
game owns instead of hooking functions**. These rules are the fix, and they are not optional.

## The rules

1. **Never hand the game an object of ours.** No wrapped device, queue, command list, swapchain,
   factory. Whatever the driver creates is what the game gets. (Our `Wrapped*` classes are dead code
   in this configuration; they must not be returned from any creation path.)

2. **Hook functions, not objects.** Two allowed techniques:
   * a function hook on an export (`D3D12CreateDevice`, `CreateDXGIFactory2`, ...),
   * replacing one vtable *entry* with a plain pointer write (`VirtualProtect` + store), which is
     what ReShade does for COM objects.
   Anything that rewrites whole vtables, adds interfaces, or proxies unknown IIDs is forbidden.

3. **Never suspend threads.** `MH_EnableHook` suspends the process to rewrite code. Installing a hook
   in the middle of the engine's device/swapchain creation deadlocked twice on Wuchang. Pointer
   writes need no suspension; MinHook is only acceptable for a hook installed during our own early
   init, on our own thread, before the engine starts creating objects.

4. **Only observed or declared states drive a transition.** No guessing. Streamline titles declare
   the state with the tag (`slSetTag(..., cmdList)`), so that declaration is used; a resource whose
   state is unknown is skipped, not transitioned.

5. **Our work goes on our own command list.** Transfers are recorded on a list we allocate, then
   submitted by appending it to the game's `ExecuteCommandLists` inside the detour - ReShade's flush
   model. Recording into the game's live list is what left barriers in a list the engine had already
   reasoned about.

6. **No GPU work on a background thread.** Our threads may do discovery and allocation; anything that
   touches the queue happens inside the hook, on the game's thread, and must be microseconds.

7. **Config is read once at start.** No polling of ini files from the render thread. A transport
   size or hook set changes on the next launch.

8. **The declaration is the truth - no second-guessing the content.** A resource is usable when the
   API declared it for this frame, its extent and format match, its state is observed or declared,
   and its lifetime covers the moment we read it. That is the whole test. We do **not** decide that
   a declared resource "looks wrong" and refuse it:

   * a `probe:` measurement (mean|v|, zero/invalid share, sentinel 3.4e38) is a **log line**, never a
     gate and never a flag for the consumer;
   * "this lane's data is empty by design" is not a rule. The 2026-11-12 survey reported
     `dlssg_mv=0x0` in every DLSS-G evaluate - but no title in that run had frame generation
     actually working (sm86 was not installed, so `sIsFeatureSupported` said no and the FG feature
     was never created). One failed experiment does not become an API property;
   * what IS allowed to remove a candidate is a *contract* violation: no declaration, extent 0
     after asking the resource itself, format that cannot be copied, unknown state (rule 4), or a
     lifetime that already ended.

   The reason is practical: every one of those "we know better" gates cost a session. CP2077's MV is
   declared once and reused for the whole session (a per-frame freshness test calls that stale),
   and the ReShade-era `kInputFlagMvSuspect` made the consumer throw away a perfectly good field.


9. **What may be recorded depends on the command list type.** A `ResourceBarrier` is illegal on a
   COPY list, and a draw/dispatch is illegal on anything but DIRECT. Getting this wrong is not a
   soft failure: the *title's* `Close()` fails with `E_INVALIDARG` and the process dies inside its
   own RHI. So every recorded operation asks `list->GetType()` first:

   | list type | copy | barrier | graphics pass (downsample / depth convert) |
   |---|---|---|---|
   | DIRECT | yes | yes | yes |
   | COMPUTE | yes | yes | no |
   | COPY | yes | **no** | **no** |
   | BUNDLE | yes | no | no (not executable anyway) |

   Measured on Wuchang (UE5.1, 2026-11-12): its `slSetTag` hands over a **COPY** list, so the
   depth conversion pass we had just added killed the game with
   `Interfaces.CopyCommandList->Close() failed ... E_INVALIDARG`. The fix is not "don't support
   UE5": it is to skip the operations the list cannot take, and to refuse a narrowed slot rather
   than silently send a crop of the field.

## Current conformance

> 2026-11-12: the own host (`version.dll` proxy + wrapped objects) is retired and lives in
> `src/legacy/`. The producer is a ReShade add-on again; see `docs/design/PRODUCER_V14.md`.

| area | mechanism | conforms |
|---|---|---|
| device creation | none - ReShade creates and wraps it | yes (rule 1) |
| `CreateCommandQueue` / `CreateCommandList` | none - ReShade's wrappers, events only | yes (rule 1/2) |
| `ExecuteCommandLists` | none - `addon_event::execute_command_list` when needed | yes (rule 1/2) |
| `Present` / `Present1` / `ResizeBuffers` | none - `addon_event::present` / `init_swapchain` | yes (rule 1/2) |
| DXGI factory | no patches, observation only | yes |
| dedicated COPY queue | retired with the own host | yes (rule 1) |
| `WrappedDevice` / `WrappedCommandQueue` / `WrappedCommandList` | live in `src/legacy/own_host`, no build path uses them | yes (rule 1) |
| shared segment | created on our thread from `reshade_begin_effects`, consumes no game objects | yes |
| transfers | recorded on the list `slSetTag` hands us | **open** (rule 5) |

## The one open item

Rule 5 is the last deviation: the tag-lane copies go into the title's own command list instead of a
list of ours submitted through the flush model. It is the path with the most mileage on CP2077
(34 fps at 4K, 0 drops, 0 crashes), and the earlier own-list attempt crashed for a reason that is
now understood: the list must be submitted *after* the title's, and only with observed states.
ReShade gives both (`addon_event::execute_command_list` fires after the title's submission), so the
flush model will be re-attempted there - see `docs/design/PRODUCER_V14.md` §7.

