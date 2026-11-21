# UI synthesis gate — CLOSED

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
