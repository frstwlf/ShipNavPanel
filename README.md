# ShipNavPanel

An Elite-Dangerous-style navigation panel for Starfield's cruise mode. Press
the ship scanner key while cruising and a panel lists **every body in the
current system** — planets, moons (nested under their planet), stations and
points of interest — wearing the game's own map icons. The mouse wheel moves
the highlight, a confirm key locks a body, and the game's **own HUD marker**
points at it, updating live as you steer. Close the panel without confirming
and nothing changes — which is also how you clear a locked target without
picking another. Outside cruise the mod is idle and the scanner key keeps its
vanilla job.

The mod **points rather than targets**: Starfield's UI layer has no by-id
"set target", so the panel steers your eyes and you acquire the target
yourself (E, by default). That reframing is what makes the whole thing
possible — the full story is in the phase documents below.

## Controls (defaults)

| key | user event | what it does |
|---|---|---|
| ship scanner | `SHMonocle` | open / close the panel (cruise only) |
| mouse wheel | `ZoomIn` / `ZoomOut` | move the highlight (hidden from the camera while the panel is open) |
| POV toggle | `TogglePOV` | lock the highlighted body — or clear it, if already locked |

Every key is matched by **user-event name**, so rebinding just works; the
panel's hint pills render your actual bindings. The confirm key is
configurable (`sConfirmEvent` — names or raw `#id` codes, comma-separated).

## What it looks like

The panel wears the ship HUD's own dress: the loot-panel plate and header
strip, vanilla map icons per row (station badges, settlement diamonds, the
in-POV circle for gas giants), the game's own "…" truncation, a slim
scrollbar, the scanner monocle's open/close sounds, a fade-and-grow animation
tuned to them, and the cockpit displays' slight 3D tilt. Undiscovered
stations and POIs show the game's own masked generic labels ("Starstation",
…) in your language, unmasking live on discovery.

On the HUD, the mod manages the cruise blip clutter: while the panel is open
(or a body is locked) the off-screen ring blips stand down except the ones
that matter, the locked body's marker wins overlaps, and stations get the
same blip-to-icon handover planets have. All of it drives vanilla UI pieces —
no SWF is patched.

## Requirements and install

- Starfield 1.16.244+ and a matching [SFSE](https://www.nexusmods.com/starfield/mods/106)
- Install with your mod manager (or drop `SFSE/Plugins/` into `Data/`)

The plugin is a single DLL + INI (+ PDB while in beta, so crash logs come back
symbolised). **No plugin file (ESM/ESP), nothing written to your save, and —
since 0.17.0 — no files written at all**: the planet/moon hierarchy is parsed
from your own load order into memory each launch (a few hundred ms, on a
background thread, DLC and mod-added systems included). Uninstalling leaves
nothing behind.

Settings live in `Data\SFSE\Plugins\ShipNavPanel.ini`; override in
`ShipNavPanelCustom.ini` so your changes survive updates. If the marker ever
points wrongly on your setup, `fArrowAngleOffset` / `bArrowInvertAngle` are
the first thing to try. The log is at
`Documents\My Games\Starfield\SFSE\Logs\ShipNavPanel.log`.

## How it works, briefly

An SFSE plugin, no Address Library ids, no Ghidra, no SWF patching. It
subscribes native functions to the ship HUD's own Scaleform data feeds
(`TargetLowFrequencyProvider` / `TargetHighFrequencyProvider`), injects the
panel into the HUD movie via SFSE's menu interface, and drives vanilla UI
components (`OffScreenIcon`, `DynamicPoiIcon`, hotkey pills) with their own
public APIs. The moon hierarchy comes from parsing PNDT records straight out
of the load order — the one piece of galaxy data the engine exposes nowhere
at runtime (see `PHASE5-STARMAP-DATA.md` for why the star map's own data
pipeline is unreachable).

Repo guide: **`TODO.md` is the current state** — quick reference, open work,
release checklist, and the settled-do-not-re-derive list.
`PHASE0-FINDINGS.md` → `PHASE5-STARMAP-DATA.md` are the dated investigation
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
camera-wheel splice) and HUD clip properties. Full audit in `TODO.md`
("Save safety").

## License

GPL-3.0-or-later, per CommonLibSF. Publishing a build obliges publishing the
source.
