# ShipNavPanel

**Released as [Cruise Navigation Panel](https://www.nexusmods.com/starfield/mods/17836)
on Nexus Mods.** `ShipNavPanel` is the plugin's own name and stays on the DLL,
the ini and the log; the two names are the same mod.

An Elite-Dangerous-style navigation panel for Starfield's cruise mode. Press
the ship scanner key while cruising and a panel lists **every body in the
current system** — planets, moons (nested under their planet), stations and
points of interest — wearing the game's own map icons. The mouse wheel (or the
D-pad on a controller) moves the highlight, a confirm key locks a body, and the
game's **own HUD marker**
points at it, updating live as you steer. Close the panel without confirming
and nothing changes — which is also how you clear a locked target without
picking another. Outside cruise the mod is idle and the scanner key keeps its
vanilla job.

**Or just set a course.** Highlight a **planet or moon** and press your normal
**course-lock key** — the one the game already prompts you with in cruise — and
the autopilot flies you there. Vanilla needs something targeted before it will
lock a course; with the panel open it aims at the highlighted row instead. Press
again on the same body to clear. With the panel closed the key is untouched.

Stations, ships and spawned contacts are left alone: the autopilot is aimed at a
*body*, and those are not bodies. Fly to them the way you always did — target
them, then use the same key with the panel closed.

For everything else the mod **points rather than targets**: Starfield's UI layer
has no by-id "set target", so the panel steers your eyes and you acquire the
target yourself (E, by default). That reframing is what made the whole thing
possible — the full story is in the phase documents below.

## Controls (defaults)

| key | user event | what it does |
|---|---|---|
| ship scanner | `SHMonocle` | open / close the panel (cruise only) |
| mouse wheel | `ZoomIn` / `ZoomOut` | move the highlight (hidden from the camera while the panel is open) |
| D-pad up / down | `Up` / `Down` | move the highlight, on a controller |
| POV toggle | `TogglePOV` | lock the highlighted body — or clear it, if already locked |
| your course-lock key (RB on a pad) | `LockCourse` | set the cruise autopilot on the highlighted body — or clear it |

Every key is matched by **user-event name**, so rebinding just works, and one
list serves both devices because a user event is not tied to one — the engine
resolves it against whatever you are holding. The panel's hint pills render
your actual bindings. Every control is configurable (`sConfirmEvent`,
`sBrowseUpEvent`, `sBrowseDownEvent`, `sLockCourseEvent` — names or raw `#id`
codes, comma-separated), and `bLockCourse=false` leaves the fire key alone
entirely.

The course key is the one control with **no hint pill**, on purpose: the game
already prompts you with it in cruise, so a second prompt would be noise. While
the panel is open the mod takes that key outright — the game does not also act
on it, so you get one course change per press, aimed at your highlighted row.
Close the panel and it is the game's key again, unchanged.

The D-pad is free to borrow because vanilla spends it on **power allocation**
and switches that off for the whole of cruise
(`PowerAllocationComponent.InitiateCruiseMode` → `EnableInput(false)`), which
is the only state this panel exists in. Confirmed in game: browsing the list
does not move a power bar.

## What it looks like

The panel wears the ship HUD's own dress: the loot-panel plate and header
strip, vanilla map icons per row (station badges, settlement diamonds, the
in-POV circle for gas giants), the game's own "…" truncation, a slim
scrollbar, the scanner monocle's open/close sounds, a fade-and-grow animation
tuned to them, and the cockpit displays' slight 3D tilt. Undiscovered
stations and POIs show the game's own masked generic labels ("Starstation",
…) in your language, unmasking live on discovery.

Each row's distance cell doubles as a **survey meter**: a slim grey bar for a
partly surveyed body, and the vanilla SURVEYED banner at 100%, both lifted
from the planet card. Bodies not yet read show nothing, so a late reading
only ever reads as late.

On the HUD, the mod manages the cruise blip clutter: while the panel is open
(or a body is locked) the off-screen ring blips stand down except the ones
that matter, the locked body's marker wins overlaps, and stations get the
same blip-to-icon handover planets have. All of it drives vanilla UI pieces —
no SWF is patched.

## Requirements and install

- Starfield 1.16.244+ and a matching [SFSE](https://www.nexusmods.com/starfield/mods/106)
- [Address Library for SFSE Plugins](https://www.nexusmods.com/starfield/mods/3256) —
  CommonLibSF resolves the UI singleton, `TESForm::LookupByID` and the script
  VM through it, so it is required, not optional
- Install with your mod manager (or drop `SFSE/Plugins/` into `Data/`)

The plugin is a single DLL + INI, with the PDB alongside so crash logs come
back symbolised. **No plugin file (ESM/ESP), nothing written to your save, and
— since 0.17.0 — no files written at all**: the planet/moon hierarchy is
parsed from your own load order into memory each launch (a few hundred ms, on
a background thread, DLC and mod-added systems included). Uninstalling leaves
nothing behind.

Settings live in `Data\SFSE\Plugins\ShipNavPanel.ini`; override in
`ShipNavPanelCustom.ini` so your changes survive updates. Note that the ini
parser reads a same-line `;` comment as part of the value, so keep comments on
their own line. If the marker ever points wrongly on your setup,
`fArrowAngleOffset` / `bArrowInvertAngle` are the first thing to try. The log
is at `Documents\My Games\Starfield\SFSE\Logs\ShipNavPanel.log`. Since 1.1.2
`bVerboseLog` is on by default, so that log already carries the per-action
trace a bug report needs — attach it whole, no settings to change first. Set it
to `false` for a near-silent log; the `[Recon]` switches below it are the
investigation instrumentation and are far louder again.

## How it works, briefly

An SFSE plugin, no Ghidra and no SWF patching: the engine side is whatever
CommonLibSF already publishes (UI singleton, form lookup, and for survey state
the script VM), never an offset found by hand. It subscribes native functions to the ship HUD's own Scaleform data feeds
(`TargetLowFrequencyProvider` / `TargetHighFrequencyProvider`), injects the
panel into the HUD movie via SFSE's menu interface, and drives vanilla UI
components (`OffScreenIcon`, `DynamicPoiIcon`, hotkey pills) with their own
public APIs. The moon hierarchy comes from parsing PNDT records straight out
of the load order — the one piece of galaxy data the engine exposes nowhere
at runtime (see `PHASE5-STARMAP-DATA.md` for why the star map's own data
pipeline is unreachable). Survey state is the one place a game *function* is
called rather than a feed read: native Papyrus `Planet.GetSurveyPercent()`
through `IVirtualMachine::DispatchMethodCall`, dispatched against a script
object the mod binds itself — see `PHASE6-SURVEY-STATE.md`.

Repo guide: **`TODO.md` is the current state** — quick reference, open work,
release checklist, and the settled-do-not-re-derive list.
`PHASE0-FINDINGS.md` → `PHASE6-SURVEY-STATE.md` are the dated investigation
records; `STARFIELD-NOTES.md` (one directory up, not in this repo) holds the
cross-project engine facts.

## Building from source

[xmake](https://xmake.io) + MSVC, C++23, CommonLibSF as a sibling checkout
(see `xmake.lua`):

```bash
xmake -y
```

With `XSE_SF_GAME_PATH` set, the build installs the DLL/PDB/INI straight into
the game; without it, into `build/deploy/Data`. `xmake package -y` builds the
distributable archive. Confirm which build loaded from the version line at
the top of the log.

## Save safety

No forms are created, no serialization is registered, selections are plain
process-lifetime state. The engine writes are two vtable hooks (input tap,
camera-wheel splice) and HUD clip properties. The survey read binds a Papyrus
`Planet` object to a body where the game has not already bound one, which is
runtime VM bookkeeping only: measured across ten swept systems, save size did
not grow, and a save made with the mod loads clean with the DLL removed.
`bPanelSurveyBind=false` turns even that off. Full audit in `TODO.md`
("Save safety").

## License

GPL-3.0-or-later, per CommonLibSF. Publishing a build obliges publishing the
source.
