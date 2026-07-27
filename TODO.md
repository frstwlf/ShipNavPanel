# ShipNavPanel — state and next steps

Rewritten 2026-07-27. The previous version had accreted every superseded plan
in order, with completed and abandoned items still unticked — misleading to
anyone picking this up cold. Full narrative lives in
[PHASE0-FINDINGS.md](PHASE0-FINDINGS.md), [PHASE1-SWF-FINDINGS.md](PHASE1-SWF-FINDINGS.md)
and [PHASE2-PANEL-PLAN.md](PHASE2-PANEL-PLAN.md).

## Where it is

**v0.2.0 beta — working, packaged, not released.** In cruise, the scanner key
cycles the system's planets; an arrow points at the selected one with its name
and distance, updating live as the ship steers. Outside cruise the mod is idle
and the scanner key keeps its vanilla job.

Build and deploy: `xmake -y` installs straight into the game
(`XSE_SF_GAME_PATH` is set at User scope). `xmake package -y` only when handing
it to someone. Confirm which build actually loaded from the plugin's own version
line, first line of the log.

## Quick reference — the mechanism that works

Everything below is verified in game. No Address Library ids, no Ghidra, no SWF
patching.

| what | where |
|---|---|
| Ship HUD root path | `root1.Menu_mc` (from `IMenu::GetRootPath()`) |
| Reticle | `root1.Menu_mc.Reticle_mc` (`ShipReticle_mc` is a *child* of it) |
| Cruise state | `Reticle_mc.CruiseModeHUDActive` — public getter, true while cruising |
| Data manager | `Shared.AS3.Data.BSUIDataManager` — **fully-qualified path only**; the bare name resolves to nothing |
| Subscribe | `manager.Invoke("Subscribe", …, [feedName, nativeFn])` |
| Feeds used | `TargetLowFrequencyProvider` (name, `uniqueID`, `uTargetType`), `TargetHighFrequencyProvider` (`angleToCrosshair`, `distance`, `screenPositionX/Y`) |
| Feed payload | callback arg is a `FromClientDataEvent`; entries at `.data.targetArray.dataA[]`, index-aligned across both feeds |
| `uniqueID` | a **form id** — `TESForm::LookupByID` gives kPNDT (planet), kSTDT (star), kREFR (POI) |
| `uTargetType` | `TT_STAR`=1, `TT_POI`=4, `TT_SHIP`=5, `TT_PLANET`=7 (full enum in `TargetIconFrameContainer`) |
| Arrow bearing | `rotation = angleToCrosshair` — a **2D screen bearing**, valid for bodies behind the ship |
| Arrow clip | `reticle.CreateEmptyMovieClip(...)` + graphics API (`beginFill`/`moveTo`/`lineTo`/`endFill`) |
| Label font | borrow a whole `TextFormat` via `donor.Invoke("getTextFormat", …)` from `Reticle_mc.ShipReticle_mc.LockOn_mc.LockText_tf` (yields `$MAIN_Font_Bold`); set `embedFonts`, and re-apply `setTextFormat` after every text change |

**Threading:** all Scaleform work must happen inside the data-feed callbacks
(the engine's own UI thread) and be gated on `LoadingMenu`/`MainMenu`. Doing it
from the SFSE per-frame task crashed v0.1.3 inside the AS3 VM. The per-frame
task only bootstraps the subscription.

**Movie rebuilds** happen more often than expected — re-create the arrow, label
and subscription whenever the movie-created callback fires, and drop stale
`GFx::Value` handles.

## Open work

- [ ] **Phase 2 panel** — list, icons, W/S navigation. Specced and graded in
      [PHASE2-PANEL-PLAN.md](PHASE2-PANEL-PLAN.md). Start with the one piece that
      can fail outright: suppressing throttle input by setting `disabled = true`
      on `ButtonEvent`s inside the input tap we already own.
- [ ] **Lock-course as a separate opt-in key.** Fully specified, never the
      default confirm — it engages the cruise **autopilot**. Build a params
      object `{uBodyID: <uniqueID>}`, construct `Shared.AS3.Events.CustomEvent`
      (payload is the **2nd** ctor arg, lands in `params`), dispatch via
      `BSUIDataManager.dispatchEvent`.
- [ ] Gas-giant / settlement icons — needs the `uPoiType`/`uPoiCategory` enums
      (sample known locations) and a body-class field that is not in the feed.
- [ ] Cosmetics: arrow size and colour, distance formatting.

## Release checklist

- [ ] Flip `frstwlf/ShipNavPanel` public — **GPL obligation** once a DLL is
      distributed (CommonLibSF is GPL-3.0-or-later).
- [ ] Scan history first for `C:\Users\<you>\...` paths and log excerpts; deleting
      a file later does not remove it from history.
- [ ] Decide on the PDB. It ships now deliberately so tester crash logs come back
      symbolised; drop it for a stable release.
- [ ] Mod page should say: cruise-mode only, planets and stars only, and it
      **points rather than targets** — the game's UI layer has no by-id set
      target. Mention `fArrowAngleOffset` / `bArrowInvertAngle` as the first
      thing to try if anyone reports the arrow pointing wrongly.

## Settled — do not re-derive

Each of these cost real time; the reasoning is in the findings docs.

- **Targeting a body by id is impossible from the UI layer.** No such event
  exists; `ShipHud_Target` is parameterless ("target what is hovered") and
  `iInfoTargetIndex` is read-only to the SWF. Would need
  `Spaceship::TargetingMode` (vtable **is** mapped: 450764, 450766 — try
  vtable-observation before Ghidra).
- **Targeting picks whatever is nearest screen centre**, and that reticle is
  fixed; the mouse circle steers only. Free-look does **not** allow targeting.
- **No cruise targeting-cone setting exists** — all 2426 GMSTs swept
  (`..\tools\Dump-Gmst.ps1`); `setgs` on the candidates changed nothing.
- **Interposing on `ShipReticle` methods is impossible** — sealed AS3 class,
  methods are read-only fixed traits. Its data members are `private` and so
  unreadable from outside.
- **Whole-class AS3 replacement in JPEXS 10.0.0 silently drops code** — proven by
  a no-op round trip. Use P-code on a single method body if a patch is ever
  needed. (Their JPEXS is from 2016; a newer build may behave.)
- **The star-map route subsystem is galaxy-map/grav-jump machinery**, unrelated
  to cruise.
- **A `TT_STAR` entry may be in another system** (one showed at ~87 ly), so
  filter by type **and** distance.
- **Do not enumerate the space cell** — `TESObjectCELL::ForEachReference` on
  cell `0x18343` crashed every attempt during the SeamlessGravJumps triage.
