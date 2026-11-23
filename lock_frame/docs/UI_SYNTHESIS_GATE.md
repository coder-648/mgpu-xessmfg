# UI synthesis gate — CLOSED

> **2026-11-22 update**: the DLSSG-only full-resolution path (render-card synth, P1) is
> implemented behind `UiStream=1 + UiFullRes=1`; the default remains **closed**
> (`UiStream=0` / `UiSource=Off`). See `docs/design/UI_COMPOSE_DLSSG_PLAN.md` and
> `docs/design/UI_COMPUTE_QUALITY.md` before reopening it.

```
UI_SYNTHESIS_GATE = CLOSED
```

UI stream / UI synthesis is intentionally disabled in this release:

```ini
UiStream=0
UiSource=Off
```

## Only reopen when all of these are true

1. The title **always** provides a real UI stream by default, on every normal
   frame, not only in menus or on some frames.
2. The UI source has valid RGB + alpha; it is not a cleared/black texture and
   is not the same resource as BACKBUFFER.
3. The HUDless source (if used) is a stable scene image, not a
   `onlyValidNow` tag that is already black at present time.
4. Loading, menus, cutscenes, resolution changes and 3x/4x cadence have all
   been tested without black video, stalls or UI ghosting.
5. The transport cost is measured and accepted for the target dual-GPU setup.

If any item is false, keep `UiStream=0` and `UiSource=Off`.

## Do not use these as substitutes

- A title that only sometimes tags UI.
- A HUDless tag that is black at present.
- A render-target heuristic (Hudfix/ResTrack prototype) that has not passed the
  checks above.

## Status

The experimental `opti_hudfix` / `hudless_tracker` code has been removed from
`faker_fg`. The v29 UI stream code remains dormant and must stay off until the
gate is explicitly reopened.

## Conclusion (1.2.5, 2026-11-21): no universal solution

After the experiments below, the conclusion recorded for **1.2.5** is that UI
synthesis has **no general solution across titles** with the inputs a game
actually offers (present / HUDless / UI tag, each optional and title-specific).
The gate therefore **stays closed in the release**:

```ini
UiStream=0
UiSource=Off
```

What 1.2.5 does ship is (a) the fix for the exact bug that made the local A/B
unusable - the opaque branch of `unblend_ui.hlsl` now writes black like
OptiScaler instead of keeping the presented (HUD-containing) colour - and
(b) a strictly opt-in local A/B path. Neither changes default behaviour.
See `lock_frame/docs/RELEASE_NOTES_1.2.5.md` §3 for the user-facing summary and
`docs/design/UI_UNBLEND_MODE4.md` §11 for the remaining limitation (2-bit alpha
of the R10G10B10A2 UI stream).

## Local A/B override (2026-11-21)

The gate can be overridden **for a local experiment** without flipping the constant,
in either of two ways:

```ini
; fakerFG.ini (game side, read once per session - restart the game after editing)
UiStream=1
UiSource=Hudless      ; or Game when the title tags UIColorAndAlpha
UiSynthesis=1         ; explicit override (the pair above is enough on its own)
```

```bat
lock_frame.exe ... --ui-unblend --ui-source hudless --streams=mv,depth
```

Two additional requirements that were missing and made the first attempt see
nothing (fixed 2026-11-21):

1. **The consumer must request the UI stream.** `--streams=...` replaces the request
   mask, so `--streams=mv,depth` left `ui=0` and the producer never copied stream 3.
   `--ui-unblend` / `--ui-source` now ORs `kRequestUi` into the mask automatically.
2. **The consumer's `ui_source_now()` no longer short-circuits on the compile-time
   gate.** An explicit `--ui-source game|hudless` is itself the opt-in; Auto follows
   the producer's published `ui_source`.

`UiSynthesis=1` or the explicit `UiStream=1 + UiSource=Game|Hudless` pair logs
`producer: ... opens the synthesis gate ...` and is **never** a release default. The
consumer still needs `--ui-unblend`; with either side missing the old
(interpolate-with-HUD) behaviour is unchanged.

## Known failure being investigated (2026-11-21)

`unblend_ui.hlsl` used to keep the *presented* colour for opaque UI. That baked the
HUD into the "HUDless" handed to XeFG mode 4, so the HUD stayed in the interpolation
base and kept ghosting. OptiScaler's `UiUnblend.hlsl` writes black there ("black
still differs from the UI so FG treats it as UI"); we now do the same. The remaining
known limitation is the 2-bit alpha of the R10G10B10A2 UI stream for semi-transparent
edges - see `docs/design/UI_UNBLEND_MODE4.md` §11 for the next step.
