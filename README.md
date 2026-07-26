# ShipNavPanel

An Elite-Dangerous-style target list for Starfield: a small panel down one side
of the screen, populated with the current system's targetable objects, navigated
with W/S and confirmed with the target key — opened with the ship scanner key,
which the game leaves unused while cruise mode is active.

**This repository is currently at Phase 0: a recon build that changes nothing in
game.** It only writes observations to a log. Nothing is targeted, no menu is
drawn, no input is consumed. The panel itself does not exist yet, by design —
Phase 0 exists to answer the questions that decide whether it can be built, and
to answer them before any effort is spent on Scaleform.

> ### ✅ Phase 0 is answered — see [PHASE0-FINDINGS.md](PHASE0-FINDINGS.md)
>
> Test run 2026-07-26. **The mod is buildable.** The ship scanner key
> (`SHMonocle`, id 84) reaches the input chain during cruise mode with
> `disabled=false`, so the panel can be opened straight from the input tap and
> matched by user-event name — rebinding- and layout-safe. `SelectTarget`
> (id 69) is live while piloting, and `SpaceshipHudMenu` loads a Scaleform
> movie, confirming the Phase 2 injection target.
>
> One new requirement fell out: the key is *also* undisabled in normal flight,
> so the mod must detect cruise state before consuming it.
>
> **And the feasibility question is settled.** In cruise the vanilla target
> cycle only reaches targets near the ship's heading — but a target acquired
> beforehand **survives entering cruise**. The cone restricts *acquisition*, not
> *possession*: the engine will happily hold a target the cycle could never
> reach. The panel is therefore providing a second route to a state the game
> already supports, not fighting it. What Phase 1 needs is a *set-target-to-X*
> call, not the vanilla cycle.

---

## Why a recon build first

Three things had to be settled, and none of them can be answered by reading
CommonLibSF or the SFSE source:

1. **Does the scanner key still reach the game's input chain while cruising?**
   This is the mod's foundation. A bound action the game has switched off still
   travels the input chain with its `disabled` flag set, so the log can tell
   *"the key never arrives"* apart from *"the key arrives and is rejected"* —
   two answers that need completely different designs.
2. **What are the exact menu names while piloting, and does the ship HUD's
   Scaleform movie actually load?** Phase 2 injects the panel into that movie.
3. **What does ship and location state look like** across normal flight, cruise,
   and target cycling — for correlating with the input log.

## What it observes

| Log prefix | Source | Answers |
|---|---|---|
| `[input]` | `UI::PerformInputProcessing` (vtable tap) | key name, id code, disabled flag, press/release, hold time |
| `[menu]` | `MenuOpenCloseEvent` sink | menus opening and closing, by real name |
| `[menu-movie]` | SFSE menu interface callback | which menus have a Scaleform movie loaded |
| `[state]` | 5-second heartbeat | ship form id, in-space/docked/landed, current planet, open menus |

The input tap takes the vtable address from the live `UI` singleton rather than
from an Address Library id, so it does not depend on any of the ids CommonLibSF
currently leaves unmapped.

## Install and run

Build output is a Vortex-ready archive:

```bash
xmake -y && xmake package -y
```

`build\packages\ShipNavPanel-0.0.1.zip` installs through Vortex like any other
mod. Log lands at:

```
C:\Users\<you>\Documents\My Games\Starfield\SFSE\Logs\ShipNavPanel.log
```

Settings live in `Data\SFSE\Plugins\ShipNavPanel.ini` — override them in
`ShipNavPanelCustom.ini` rather than editing the shipped file. Input logging is
capped at 20,000 lines per session so a stuck key cannot fill the disk.

## Test protocol

Do this in one sitting so the timestamps line up, and keep a note of roughly
when each step happened. Every step matters — the negative results are as
informative as the positive ones.

1. **On foot, indoors.** Press the scanner key. Confirms the log works and shows
   the scanner's user-event name while it is fully enabled.
2. **In the pilot seat, in space, not cruising.** Press the scanner key. Press
   the target key a few times to cycle through targets.
3. **Enter cruise mode.** Press the scanner key several times. *This is the
   decisive step.*
4. **Still cruising:** press the target key a few times. Does the vanilla cycle
   still work while cruising at all?
5. **Leave cruise**, press the scanner key once more to confirm it returns.

Then read the log and check what the `[input]` lines say for the scanner key in
step 3 versus steps 2 and 5.

## Reading the result

The scanner key's line in step 3 decides the design:

- **A line appears with `disabled=false`** — best case. The action reaches the
  chain normally; the panel can be opened straight from the input tap, and it
  stays correct across rebinding and non-QWERTY layouts because the match is on
  the user-event name, not a scan code.
- **A line appears with `disabled=true`** — good. The key press is visible; the
  game is only refusing to act on it. Same design, matching on the name and
  ignoring the disabled flag.
- **A line appears with an empty `user=''` but a real `id=`** — usable. The
  binding is stripped in this mode, so match on the id code and read the
  player's actual binding elsewhere to stay rebinding-aware.
- **No line at all** — the tap does not cover this input path. Not fatal, but it
  means the next step is finding the path that does: `MenuControls` and
  `ControlMap` both exist in the current CommonLibSF address tables (RTTI 864851
  and 867481, vtables 460734/460736 and 469954) and are the natural second tap.

If the `[input]` log is empty for *every* step, the tap is on the wrong class
entirely and `MenuControls` should be tried before anything else is concluded.

Also worth capturing from the same run: the exact name the ship HUD reports in
`[menu-movie]` (expected `SpaceshipHudMenu`), and whether its movie is `yes`.

## Safety notes

Carried over from ShipHullRegen's four crashes, and deliberately observed here:

- No iteration of engine collections anywhere — every read is a targeted read of
  a known-good object. Broad iteration is where every previous crash lived.
- Plain member reads instead of virtual calls where both exist (`strUserEvent`
  and `disabled` rather than `QUserEvent()`, `menuName` rather than `GetName()`).
- The per-frame path does nothing at all while `LoadingMenu` or `MainMenu` is
  open.
- The one vtable write is claimed with a single-winner atomic exchange, because
  two threads patching the same slot would leave the hook calling itself.
- The PDB ships next to the DLL, so Starfield Engine Fixes symbolizes any crash
  backtrace with function names and source lines.

## Roadmap

- **Phase 0 — recon (this build).** Answer the three questions above.
- **Phase 1 — targeting.** Map the `ShipHud_*` event id cluster (old ids
  137011–137033 are recorded in CommonLibSF's `IDs.h` comments as anchors), then
  observe the vanilla target cycle and drive it. `BSTEventSource::Notify` and
  `RegisterSink` are already live, so publishing works once an id resolves —
  but verify each event's real payload first: CommonLibSF declares the
  `ShipHud_*` structs as empty, and publishing a wrong-shaped payload is exactly
  the bug class that broke SeamlessGravJumps. Ships something usable on its own:
  scanner key cycles to the next target, name shown in a notification.
- **Phase 2 — the panel.** Inject into the ship HUD's movie via SFSE's menu
  interface (`kInterface_Menu` v2, live in SFSE 0.2.21 — `Hooks_Scaleform_Apply`
  is *not* compiled out, unlike the serialization hooks). Reuse the game's own
  `fonts_en.swf` and button clips; the drawing is flat rectangles and a
  highlight bar, so the work is layout and ActionScript, not art.

## License

GPL-3.0-or-later, per CommonLibSF. Publishing a build obliges publishing the
source.
