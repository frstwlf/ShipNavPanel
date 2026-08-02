# ShipNavPanel — state and next steps

Rewritten 2026-07-27; state refreshed 2026-07-30 — same reason both times: a
handoff doc that accretes its own history stops being one. The dated
investigation records are
[PHASE0-FINDINGS.md](PHASE0-FINDINGS.md), [PHASE1-SWF-FINDINGS.md](PHASE1-SWF-FINDINGS.md),
[PHASE2-PANEL-PLAN.md](PHASE2-PANEL-PLAN.md), [PHASE3-BLIP-PLAN.md](PHASE3-BLIP-PLAN.md),
[PHASE4-CHROME-HUNT.md](PHASE4-CHROME-HUNT.md) and
[PHASE5-STARMAP-DATA.md](PHASE5-STARMAP-DATA.md); git history holds the
version-by-version story this section used to accumulate.

## Where it is

**⭐⭐ COURSE LOCK — CONFIRMED IN GAME, and it changes what this mod is.**
Highlight a **planet or moon**, press the course key, and the cruise autopilot
takes the ship there. **Nothing has to be targeted first**, which is the part
that matters: `Reticle_OnCruiseLockCourse` accepts a `uBodyID` the base game
never sends — one that is not the current info target — so the panel finally has
a verb that reaches the engine directly. `bLockCourse` (default **on**) and
`sLockCourseEvent` (default **`LockCourse`** — the player's own course key) live
in `[Panel]` with the rest of the controls. **Vanilla needs a target before it
will lock a course; with the panel open the mod aims the same key at the
highlighted row instead**, and with the panel closed the key is untouched. One
event, both devices (RB on a pad), no hint pill needed because the game already
prompts with it.

**Scope, measured rather than assumed (2026-08-03): planets and moons only.**
Stations, POIs and ships cannot take a course by id, so on those rows the mod
**leaves the press to vanilla**, whose own route goes through the info target and
reaches them. Getting there took four flights and four gates; the account is in
Settled and the mechanism is at `IsCourseableType`.

Still **no version bump** — head is v1.1.2 plus this, pending a release
candidate.

**Its sibling `bAcquireTarget` was built, flown and DELETED the same day** —
see Settled. It worked exactly as specified and the specification was the
problem.

**v1.1.2 — THE TAKEOFF CRASH, FIXED AND CONFIRMED IN GAME 2026-08-02, and
PACKAGED for release.** Taking off from a planetary surface crashed the game.
The ship HUD movie is destroyed and rebuilt inside ~20 ms during the transition,
and the subscribe bootstrap — which runs on a BSJobs worker, not the main
thread — called into the replacement's AS3 VM while it was still registering its
ABC file. `WorldSettled` could not catch it and never could: it measures time
since `LoadingMenu` closed, and a surface takeoff rebuilds the HUD during the
cutscene, *before* any loading screen, so it read "settled" for the whole
dangerous window. The 2026-07-28 freeze was the same shape; the settle timer
added for it was measuring the wrong clock.

Three parts, all in `TryInstallSubscriber`: `g_movieGeneration` (bumped by
`OnMenuMovieCreated` ahead of the re-arm stores) gives the gates an identity to
key off; `MovieSettled` holds a movie 1500 ms from first sighting, checked
before the probe budget is touched; `StillSameMovie` revalidates generation and
live root ahead of every VM call on the path. On top of those, `ReadyToFly`
skips the probe while landed or docked — **sitting in the pilot seat is not
flying**, and a subscription made on the pad is discarded by the takeoff rebuild
seconds later, made in the window a rebuild is most likely to land on.

⚠ SFSE offers **no main-thread bounce**. `AddTask` and `AddPermanentTask` are
both dispatched from the same `Command_Process` hook, which the crash backtrace
shows running on BSJobs workers — the "executed on the Main thread" comment in
`PluginAPI.h` is wrong for Starfield. So the fix removes the *window*, not the
thread. It is not a lock and does not pretend to be one; Scaleform exposes none.

`ReadyToFly` deliberately uses only calls this build already ran (`GetSpaceship`
/ `IsSpaceshipLanded` / `IsSpaceshipDocked`). `GetSpaceshipPilot` (id 119876)
would add passenger-vs-pilot discrimination but has never been called here, and
a subtly wrong relocation id is a crash — a poor trade for a distinction the
ship HUD being open already mostly makes. `IsInSpace` is out for its own reason:
its bool argument has no documented meaning.

**It fixed the function too, not just the stability.** The pre-fix "route
exhausted" failure — which left the mod silently inert — was an artifact of
probing a half-built movie, not a missing route. Post-fix, every generation
subscribes to both feeds on the first path that resolves.

Verified across two sessions: 3 and 4 HUD rebuilds, covering takeoff,
landing/docking and grav jumps, no crash. `bVerboseLog` now defaults **on** with
this release (a report filed without it usually has to be filed twice, and it
costs tens of lines a session, not thousands), and the startup line that
announced "diagnostics all off" no longer contradicts itself when the trace is
running.

Archive `build/packages/ShipNavPanel-1.1.2.zip` (3.71 MB,
`Data/SFSE/Plugins/{dll,ini,pdb}`, DLL stamped 1.1.2.0), same shape as the 1.0.0
and 1.1.1 packages. Pre-package `GetValue()` grep re-run and clean: `Register`
and `AddPermanentTask` are both unconditional, and the flags touching machinery
are now three — `bInputTap`, `bWheelFilter` and the new `bGateOnFlightState` —
all defaulting `true` in source *and* in the shipped ini. Note that
`bGateOnFlightState` fails in the safer direction than the other two: `false`
removes the gate and restores the older, more permissive behaviour rather than
producing an inert build.

**LIVE ON NEXUS with a changelog, 2026-08-02** — the third release of the mod,
and the first that fixes a crash rather than adding to it. No Patreon/Ko-fi
post: those are one-offs for a new mod, never for an update.

**v1.1.1 — CONTROLLER BROWSING, BOTH HALVES CONFIRMED IN GAME 2026-08-02 and
PACKAGED for release.** v1.1.0 landed the feature ("it works"); v1.1.1 fixed the
one thing the tester found — the browse pill did not follow a device swap made
WHILE the panel was up, functionality unaffected, display only ("it swaps
properly now"). See the pill note below for why that was a different problem
from the one v1.1.0 solved.

Archive `build/packages/ShipNavPanel-1.1.1.zip` (3.68 MB,
`Data/SFSE/Plugins/{dll,ini,pdb}`, DLL stamped 1.1.1.0), same shape as the
1.0.0 package. Pre-package `GetValue()` grep re-run and clean: the only two
flags touching machinery are `bInputTap` and `bWheelFilter`, both documented
escape hatches and both defaulting `true` in source *and* in the shipped ini.
**LIVE ON NEXUS with a changelog, 2026-08-02** — the second release of the mod,
and the first update to it.

Reported by users and reproduced by the tester: the panel could not be browsed
with a controller. Not a bug — an absence. The browse pair was hardcoded to
`ZoomIn`/`ZoomOut`, which are **mouse** bindings, so on a pad the branch was
never reached. Their own log shows what the D-pad reports instead:

```
[panel] key id=1 reports 'Up' and is not one of the panel's controls
[panel] key id=2 reports 'Down' and is not one of the panel's controls
```

- **`sBrowseUpEvent` / `sBrowseDownEvent`** (new), same list shape and rules as
  `sConfirmEvent`, defaulting to `ZoomIn,Up` and `ZoomOut,Down`. **One list
  serves both devices** because a user event is device-agnostic — the engine
  resolves it against whatever the player is holding — so the wheel browses on
  KBM, the D-pad browses on a pad, and neither needs a setting changed. No
  device gating, by the user's call. The list walk was lifted out of
  `MatchesConfirmEvent` into `MatchesEventList`, and all three lists are now
  read **once per input-queue walk** rather than once per event.
- **The D-pad is free in cruise, and this is why**: vanilla spends `Up`/`Down`
  on power allocation, and `SpaceshipHudMenu` wires
  `Reticle_CruiseModeInitiate` → `PowerAllocationComponent.InitiateCruiseMode`,
  which calls `EnableInput(false)`; `MinimalButton.HandleButtonHit` then returns
  `Enabled && bEnabled` and gates the callback on the same test. So the whole of
  cruise — the only state this panel exists in — has the D-pad switched off
  already. **Confirmed in game by the tester: browsing moves no power bar.**
- **The browse pill is now dressed per device.** It showed *nothing* on a
  controller, because a pill's cap is resolved by the vanilla component against
  the current device and `ZoomIn` has no pad binding to resolve.
  `sPanelBrowsePillEvent` / `sPanelBrowsePillEventPad` (`ZoomIn` / `Up,Down`),
  chosen by reading vanilla's own `uiController` off `Reticle_mc` and re-driven
  with `SetButtonData`. The pad entry is a list, which `ButtonBaseData` takes as
  an Array of `UserEventData` — vanilla's own two-way-hint idiom
  (`$SELECT SYSTEM` is driven by `[Left, Right]`) — so the cap reads as the
  D-pad pair.
  - ⚠ **v1.1.1: it has to follow a swap made mid-panel, so it rides the 0.25 s
    text cadence, not the panel-open transition.** v1.1.0 re-dressed once per
    open, reasoning from v0.9.1 that VM work on the live list is what costs the
    wheel its smoothness — but the tester swaps device with the panel *up*, and
    the confirm pill follows that while ours did not. **The two are not
    comparable work:** v0.9.1 was per-notch work on every row of a live list;
    this is one `GetVariable` four times a second, on the same tick that already
    does per-row `SetText` and `textWidth` measures, with the `SetButtonData`
    firing only on an actual change. Being right about a hazard is not the same
    as that hazard applying.
  - **Why the confirm pill needed none of this**: it is driven by ONE event both
    devices bind, so the component re-resolves its own cap off its own
    `ControlMapData` subscription. **A component that re-renders is not a
    component that re-chooses** — the browse pill needs a *different event* per
    device, and only the mod can decide that.
- Also confirmed by the tester: the **confirm pill already resolved correctly**
  on a pad, so `TogglePOV` has a controller binding and locking works.

**v1.0.0 — the release build.** The v0.18.x arc closed 2026-07-31
(duplicate-name work on the baked two-contact save; reveal-state labels across
several random POI spawns in normal play, every one wearing its proper name),
and Phase 6's survey marks closed 2026-08-01 with save exposure measured at
nil.

**Release pass, 2026-08-01** — version 1.0.0, and three changes with it:

- **`bVerboseLog` (new, `[Recon]`, default off) is the log-volume gate.**
  Everything that repeated with a player's actions — every wheel notch's
  `[panel] highlight`, `[camera] hid`, `[blip] kept`/`returned`, the
  `[blip-dbg]` reticle census, the `[arrow]` bearing, the `[galaxy]`
  per-body dump, the blip container census — now prints only with it on. A
  quiet session logs startup, settings, once-only state and warnings: the
  sample session that produced 159 lines produces about 55, and no longer
  scales with the wheel. **Rule for anything added later: if a line repeats
  when the player does something, it goes behind that flag.**
- **The ini was rewritten for players** (577 → ~430 lines): same 108 keys,
  same defaults, grouped start-here / list / survey / look / HUD /
  conflict-chasing, with the build narrative dropped. Colours are `0xRRGGBB`
  now — **SimpleIni parses `0x` (base 16 when prefixed, base 10 otherwise)**.
  ⚠ And it requires the WHOLE value to be consumed, so a same-line `;`
  comment makes the setting fall back to its default *silently*. The first
  draft of this rewrite had exactly that bug in it. Never put a comment after
  a value.
- The startup config dump now prints the diagnostics block only when
  something is on, and says where `bVerboseLog` is when nothing is.

- **The panel**: in cruise the scanner key opens/closes it, the wheel or the
  D-pad moves the highlight (spliced away from the camera), `TogglePOV` — or anything in
  the `sConfirmEvent` list — locks the highlighted body or clears an existing
  lock. Closing without confirming changes nothing, which is how a target is
  cleared without picking another. It lists the whole system, moons nested
  under their planets, localised names, a dash for distance on bodies the HUD
  is not tracking.
- **The dress** (v0.10–0.16 arc, all tester-approved): loot-panel plate
  colour, solid `0x218286` header strip with a token-composed title (default
  `$CRUISE| - |$Outpost_AvailableTargets`), vanilla map icons per row
  (`DynamicPoiIcon` badges, the settlement diamond, `PlanetIconCircle` + ring
  line for giants), grey `0xB7B7B7` row text brightening to white under the
  vanilla `0xEFF3DC` highlight bar, "…" truncation by C++ measure-and-cut,
  drawn scrollbar, the monocle's own open/close sounds, a fade+grow animation
  on an openness state machine, and the cockpit-glass Matrix3D tilt.
- **Hint pills**, all resolving the player's real bindings: the scanner pill
  on the HUD (panel closed), the confirm pill in the footer (first named
  `sConfirmEvent` entry), and the browse pill — wearing the game's own
  MOUSEWHEELUP cap on keyboard and mouse (kept by the user's call, drawn glyph
  as automatic fallback) and the D-pad pair on a controller (v1.1.0). ⚠ **A
  pill can only draw a binding the CURRENT DEVICE has**; an event the device
  does not bind renders an empty cap, which is how the browse hint came to be
  invisible on a pad for a whole release. Anything pill-driven needs a
  per-device answer, unlike the events themselves.
- **Blip management** (v0.8 arc): ring blips hide only while the panel is
  open (highlight+lock kept) or a lock exists (only it kept) — idle cruise is
  fully vanilla. The selection wins overlaps both directions (quest and
  E-target icons deliberately still outrank it), stations get planets'
  blip-to-icon handover, the fallback marker is a real `OffScreenIcon` with
  the entry's own POI art, and masked rows follow the marker's REVEAL
  STATE (`uLocationMarkerState`, v0.18.0 — the same field the HUD icon
  reads) wearing the game's generic labels via localisation tokens,
  unmasking the moment the marker does.
- **The data** (v0.17.0): the planet/moon hierarchy — PNDT GNAM, LCTN
  settlement join, Localization.ba2 names — is parsed from the whole load
  order into memory on a background thread each launch. **423 ms measured**;
  no files written; a leftover pre-0.17 cache file is deleted on sight. Live
  tracking rides the HUD's two target feeds (quick reference below).

The star-map data pipeline was mapped end to end on 2026-07-30 and is a
closed door — every provider is menu-scoped ([PHASE5-STARMAP-DATA.md](PHASE5-STARMAP-DATA.md)) —
with one usable exception (`InfoTargetProvider`) listed under Open work.

Build and deploy: `xmake -y` installs straight into the game
(`XSE_SF_GAME_PATH` is set at User scope). `xmake package -y` only when handing
it to someone. Confirm which build actually loaded from the plugin's own version
line, first line of the log.

## Quick reference — the mechanism that works

Everything below is verified in game. No Ghidra and no SWF patching, and no
offset found by hand: the engine side is only what CommonLibSF already
publishes. ⚠ That is **not** "no Address Library ids" — the phrase this
section used to carry. `RE::UI::GetSingleton`, `TESForm::LookupByID` and
`GameVM::Singleton` are all `REL::ID` lookups, so **Address Library for SFSE
Plugins is a hard requirement from startup**, and a missing or mismatched
`versionlib-*.bin` is a `REX::FAIL` dialog, not a degraded feature. It has to
be listed on the mod page.

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

- [x] ~~**PHASE 6 — survey state on the rows.**~~ **CONFIRMED IN GAME 2026-08-01
      (v0.20.1).** Survey a gas giant from the pilot seat → the banner appears;
      quickload → it is gone. Banners read well, the distance number stays
      legible on the plate, zero warnings across ~10 systems.
      **Save exposure MEASURED at nil** (7783 → 7623 KB over ten swept systems,
      i.e. it went *down*) and **a DLL-removed save loads clean**, so the
      Papyrus bindings neither reach the save nor orphan it — the mod's
      no-save-state promise survives intact. The whole pre-ship list is closed.
      [PHASE6-SURVEY-STATE.md](PHASE6-SURVEY-STATE.md) §0e is **the reference**.
      Only open item: the tester's colour call in the seat, which is an ini
      swap and needs no rebuild. In short: a body's fully-surveyed state
      shows as a colour filling the row's 20 px **icon cell**, behind the icon
      when there is one — only the 100 % state draws, incomplete and partial
      stay blank.

      **Feasible; the render half is nearly free and the data half hangs on one
      unproven ABI.** `RefreshPanel` already runs on the HIGH feed and already
      reads mod-side stores at render time, so a `formID -> bool` map needs no
      invalidation at all and "several rows flip at once" costs nothing. The
      only data source that covers a whole system is the native Papyrus
      `Planet.GetSurveyPercent()` — every UI feed that carries survey state
      describes exactly ONE body.

      **v0.19.0 ships the two probes that gate it** (both default OFF,
      read-only, off the shipping path — see the ini):
      `bProbeSurveyVM` dispatches `Planet.GetSurveyPercent` for every listed
      body and logs which step of the chain breaks; `bProbeStarmapFeed` +
      `sStarmapFeed=InfoTargetProvider` watches the body dossier the vanilla
      planet card draws. Log tag `[surveyed]` — `[survey]` was already taken by
      the cruise-key survey.

      **The single decisive unknown:** can this plugin dispatch
      `IVirtualMachine::DispatchMethodCall` at all? Its vtable ordinal comes
      from a comment, its argument functor is a `BSTThreadScrapFunction`
      aliased to `std::function` with no size assertion, and no SFSE code here
      has ever exercised it. If probe A answers with a float for a **moon** as
      well as a planet, build the feature; if step 2 fails, the Papyrus route is
      dead and the answer is Ghidra.

- [ ] **Remaining blip-pass eyeballs** — cases the passes cover by design
      (v0.8.6–0.8.13, checklist in
      [PHASE3-BLIP-PLAN.md §7–10](PHASE3-BLIP-PLAN.md)) that were never
      individually isolated on screen; nothing has ever been seen
      misbehaving: the exempt-cover (Staryard locked, E-target Earth over
      it — no redundant mod marker), the REVERSE overlap (lock Earth with
      the Staryard near — the station's marker fades), the
      moon-behind-parent overlap (lock Luna with Earth crowding it), the
      plain **on-screen yield**, **quest blips** surviving the cull, and
      the **interdiction tripwire**.
- [ ] **Watch for the v0.7.5 startup race recurring.** Fixed rather than
      worked around, but it is a race, so absence of a crash is weak evidence.
      The tell in a crash log is `ShipNavPanel.dll` appearing below AS3 VM
      frames in `Starfield.exe` on a `BSJobs` thread.

      It surfaced as "crashes when `ShipNavPanelCustom.ini` exists with
      `bPanelIcons=false`, fine without it", which is a real and repeatable
      observation but **not a causal one** — the setting is read long before the
      crashing code and touches nothing it uses. Reading a second ini shifts
      startup timing, and timing is the whole of it. Worth remembering the shape
      of that: a config that reliably reproduces a race is still only evidence
      about timing.
- [x] ~~Reposition the panel to sit with the HUD.~~ Resolved 2026-07-30: the
      tester's in-game values are **(−780, −180)** from screen centre, shipped
      as defaults in v0.17.2 (an earlier note blessing the old −540/−160
      guess was the tester's slip, corrected the same day). Width was already
      the loot panel's own 425 (v0.12.1); `fPanelOffsetX/Y`,
      `fPanelRowHeight`, `uPanelMaxRows` remain per-user knobs.

- [ ] **The panel can outlive a forced exit from cruise.** Seen when a random
      combat event dropped the ship out: the panel stayed up until the scanner
      key closed it. Cruise is detected only from `Reticle_mc.CruiseModeHUDActive`,
      and on an interrupted exit that flag evidently stays set for a while — a
      failed *read* closes the panel, so it cannot be the path resolution. Minor
      (one keypress clears it) but it means the panel can sit over the HUD just
      as combat starts. Wants a second, independent signal for "still cruising".
      (The 2026-07-29 scanner-drop observation is NOT this — it reproduces with
      the DLL removed; see Settled.)

- [x] ~~Duplicate feed names break blip identity.~~ **CONFIRMED FIXED in
      game (v0.18.2, 2026-07-30, the tester's baked two-contact save:
      "everything behaves properly now").** Two spawned POIs both riding
      the feed as "Sensor Contact" were one identity to the name-keyed
      passes; v0.18.1 added bearing/position disambiguation (off-screen
      blips confirmed), v0.18.2 fixed its own fallback hole (an in-FOV
      contact's icon "covered" the off-screen selection). Durable facts
      and the fallback lesson moved to Settled.

Later, nice-to-have:

- [ ] **Per-body detail from `InfoTargetProvider`** — the Phase 5 find: the
      ship HUD's own feed (our movie, subscribed by vanilla at
      `SpaceshipHudMenu.as:416`) carries `TargetOnlyData.PlanetCardInfo`, the
      full dossier for the CURRENT info target — `iType` incl. `BT_MOON`,
      `sParentBodyName`, `sSystemName`, terrain, gravity, temperature,
      atmosphere, magnetosphere, flora/fauna/water, `ResourcesA`, `TraitsA`,
      scan level, survey % — live in cruise. Could power a detail readout for
      the targeted row, or cross-check the parse's moon nesting at runtime.
      Publish cadence UNVERIFIED; the probe is free:
      `bProbeStarmapFeed=true` + `sStarmapFeed=InfoTargetProvider` in the
      Custom ini dumps the payload to the log on each publish.

- [ ] Distance formatting: the LS/km switch is abrupt, and untracked bodies show
      a dash where a real distance could be computed from orbital data.

- [x] ~~A hint pill for the course key.~~ **Not wanted** — the key is now
      vanilla's own `LockCourse`, which the game already prompts with in cruise,
      so a second prompt for the same key is noise. The user's call, and it is
      also why the shared key beat a free one.

- [ ] **Show the course on the row.** `Candidate::courseLocked` is now captured
      per body, so the panel knows which row the autopilot is flying to and could
      mark it — the engine's own state rather than the mod's belief. Cheap; the
      question is what it should look like next to the survey banner in the
      distance cell. ⚠ Worth more now than when it was first listed: with a
      SHARED key the engine's course can be set by vanilla's press as well as the
      mod's, so the row mark is the only place the two agree in front of the
      player.

- [x] ~~Flight-test the splice.~~ **CONFIRMED IN GAME 2026-08-02 ("it works").**
      Unlinking at `RE::UI` *does* hide an event from a menu's
      `ProcessUserEvent` — so the receiver that feeds the menus is a real
      chokepoint for Scaleform input, which was not known before. The camera
      tap's technique generalises.

- [x] ~~Is the splice the variable?~~ **NO — A/B run 2026-08-03
      (`bCourseSplice=false` + `sLockCourseEvent=WeaponGroup1`, i.e. the
      pre-splice build exactly): behaviour identical.** The tester called it:
      *"It was a case of bad memory."* So the splice is cleared, the key was
      already cleared by the diff, and what is left is the mechanism — see
      Settled.

- [ ] **⚗ FLY THE FAR-TRAVEL PROBE — the last untried by-id verb in this layer.**
      `[Recon] bProbeFarTravel=true`. On a row the course cannot take, the course
      key instead dispatches `ShipHud_FarTravel {uValue: <row id>}` — vanilla's
      `$JUMP TO` action (`ShipReticle.as:747`), which nobody has ever sent with a
      destination of their own choosing.

      ⛔ **Its first cut never dispatched once, and the reason is the lesson.**
      It was gated on `bFarTravelAllowed`, claimed here as a field "the engine
      publishes per row" — it is not. That pair belongs to `TargetOnlyData`, the
      **info-target payload**, which is where the dump that named them came from.
      [PHASE1-SWF-FINDINGS.md:264](PHASE1-SWF-FINDINGS.md:264) has recorded the
      per-entry list since July — `bLandingAllowed`/`bLandingDisabled`, no
      far-travel pair — and it was never checked before the claim was made. Same
      mistake as expecting the dossier's `uBodyID` on a feed entry, twice in two
      days. ⭐ **A probe gated on an unverified field is a probe that cannot run,
      and it fails silently — the worst way for an experiment to fail.** The flag
      is now the only gate; whether an entry carries those fields is merely
      reported beside the dispatch.

      ⚠ **Save first: it moves the ship**, which nothing else in this mod does.
      ⚠ **No feed reports a far travel**, so unlike the course there is no audit —
      the `[fartravel] sent` line plus what the ship does is the whole
      measurement.

      Two ways it can fail that should be recognised rather than re-derived:
      vanilla sends `TargetOnlyData.uniqueID` (the info target's id, which has
      been seen to differ from the row's — 386531 vs 385501), and vanilla
      dispatches only at the end of `SetState(STATE_FAR_TRAVEL)` plus an
      animation, so the engine may expect that state. The probe sends the row's
      id, cold.

      Outcomes: it works → the panel gains "jump to that station", a different
      and possibly better verb than a course, and the question becomes whether it
      belongs on the same key. It does nothing → the vocabulary is exhausted and
      `Spaceship::TargetingMode` is the only route left for non-body rows.

- [x] ~~TT_STATION — the only row type still unmeasured.~~ **MEASURED
      2026-08-03: stations, POIs and ships all fail; planets and moons are the
      only working destinations.** `IsCourseableType` is now `TT_PLANET` alone
      (moons ride as `TT_PLANET`), and it is the first gate on this feature where
      the prediction and the measurement agreed — which is the only reason it can
      be trusted. `TT_STAR` is deliberately not included: no star has ever been
      course-locked, the system's own does not appear to reach the feed, and a
      star is an `STDT` record rather than the `PNDT` the by-id route resolves.
      Delegating it costs nothing.

- [x] ~~THE LEAD: `uniqueID` may be the wrong number to send.~~ **DEAD, measured
      2026-08-03:** `[course] the feed does NOT carry a per-entry uBodyID`.
      `uniqueID` is the only id a low-feed entry has, so there was never another
      field to prefer. Kept below for the retraction it forced.

<details><summary>The lead, and why it looked good</summary>

The
      `InfoTargetProvider` dump caught the two ids disagreeing on one target —
      `payload.uniqueID` = **386531** against `PlanetCardInfo.uBodyID` =
      **385501** ("Masada IV"). So the id a target is *known by* and the id of
      the *body it is* are different numbers, and the event's parameter is
      spelled **uBodyID**. Vanilla's far-travel button sends
      `TargetOnlyData.uniqueID`; the mod sends the low feed entry's `uniqueID`;
      nobody has established that either is a body id.
      ⚠ This also **re-opens** a question I closed a day too early. Two earlier
      dumps showed `payload.uniqueID == pci.uBodyID`, and I wrote "closed — the
      mod is not sending a wrong id". Both samples were a *bare planet*, where
      the target and the body are the same thing. One landable target was enough
      to break it. **Two agreeing samples are not a rule.**

      The build reads a per-entry `uBodyID` if the feed has one and prefers it,
      saying which on the first dispatch — and the answer came back **does NOT
      carry** one. The dossier's `uBodyID` and a feed entry's `uniqueID` simply
      live on different payloads.

</details>

- [x] ~~Characterise the substitution.~~ **CLOSED 2026-08-03 — there is no
      substitution.** Three flights and two dead theories; the resolution is in
      Settled and in the header above `g_highlightCourseable`. Short version: the
      engine takes the course with an **unresolved destination**. Nothing holds
      it, nothing draws it — the tester's own eye is what settled it, *"there is
      no orange autopilot indicator; the ship points that way is the only visible
      indication"* — and the heading that falls out is the system's origin, which
      is where the star sits. "It locked onto the star" was the appearance, not
      the mechanism, and every theory built on picking a *substitute body* was
      chasing something that never happened.

- [ ] **⚠ VERIFY "press again clears".** It is in the ini and the README on the
      strength of a code reading — `FarTravelIconBase.UpdateButton` flips its
      LABEL between `$CruiseCourseLock` and `$CruiseCourseClear` while dispatching
      the identical event with the identical id. The `{0} = clear` finding above
      supports the toggle reading but does not prove it for a repeated *id*. The
      `[course] the engine reports ...` line (verbose, logs on change) is the
      readout.


## Release checklist

- [x] ~~Discard `build/packages/ShipNavPanel-0.2.0.zip`~~ — deleted
      2026-07-30 (the inert v0.2.0 build; `build/` is gitignored, so nothing
      to scrub from history on its account).
- [x] ~~Rewrite `README.md`~~ — rewritten to the current mod 2026-07-30,
      refreshed for 1.0.0 on 2026-08-01 (survey marks, the one address id
      stated plainly rather than "no Address Library ids", the PDB decision,
      `bVerboseLog`, the same-line-comment trap).
- [x] ~~**Pre-package grep: `GetValue()` guarding a `Register`, an install or
      a hook.**~~ Run 2026-08-01 for the 1.0.0 package: every diagnostic flag
      gates logging only. The two that touch machinery are documented as such
      — `bLogMenus` gates the open/close sink, which only ever logs (the
      movie-created callback next to it registers unconditionally, with the
      v0.2.0 lesson written above it), and `bSuppressThrottleTest` gates the
      dead throttle experiment. Re-run it on every packaging commit.
- [x] ~~Scan history for `C:\Users\...` paths and log excerpts~~ — scanned
      AND scrubbed, 2026-07-30. The scan (all revisions) found the local
      username in two doc blob lines and the personal email in every
      commit's author/committer metadata; nothing else. History was then
      rewritten (`git filter-branch`: identity → the GitHub noreply
      address, username path → `C:\Users\<you>`) and force-pushed with the
      user's approval. Verified post-rewrite: one identity across all 123
      commits, zero sensitive strings in any revision, HEAD tree
      byte-identical. The noreply is also the global `user.email` now, so
      new commits stay clean. **History is flip-ready.** (Lesson: an
      INLINE filter-branch tree-filter silently no-opped under MSYS
      argument mangling — put filter scripts in a FILE and verify content
      hits, not just ref-rewritten messages.)
- [x] ~~Flip `frstwlf/ShipNavPanel` public~~ — **GPL obligation** once a DLL
      is distributed (CommonLibSF is GPL-3.0-or-later). Flipped 2026-08-01
      once the Nexus page went up, and the flip added the missing `LICENSE`:
      the repo claimed GPL-3.0 in `xmake.lua` and the README but shipped no
      licence text, which is the one thing distributing a build actually
      obliges. (`gh repo edit --visibility public` needs
      `--accept-visibility-change-consequences` and prints nothing on
      success.)
- [x] ~~Decide on the PDB~~ — **it ships** (user's call, 2026-08-01). A first
      public release of a native plugin is exactly when symbolised crash logs
      are worth the ~14 MB.
- [x] ~~Mod page copy~~ — written and published 2026-08-02. The drafts are
      **kept privately, outside this repo** (`release/` is gitignored now);
      the published versions are the mod page itself and the two posts.
      Points carried in: cruise-mode only; **points rather than targets** (the
      UI layer has no by-id set target); whole system listed — planets, moons,
      stations, POIs; the settlement mark means *there is somewhere to go
      here*, not "a city is down there" (Deimos's mark is the staryard);
      `fArrowAngleOffset` / `bArrowInvertAngle` first if anyone reports the
      marker pointing wrongly.

## Save safety

**The mod writes nothing into your save, and creates no forms.** Worth stating
plainly, since it holds form ids and Starfield has a history of runtime forms
accumulating in saves. Audited 2026-07-27:

- **No forms are ever created** — no `PlaceAtMe`, no ref handles, no spawning.
  Every id comes from `TESForm::LookupByID`, i.e. reading a record the game
  already has.
- **No co-save data.** No serialization callbacks are registered, and SFSE
  0.2.21 compiles those hooks out regardless.
- **The selections are plain C++ atomics**, gone when the process exits — which
  is also why a lock does not survive a restart.
- **No files are written.** The body table is parsed from the load order into
  memory each launch on a background thread; the log prints the measured time
  (**423 ms on the tester's machine, confirmed in game on 0.17.0's first
  launch**). Versions up to 0.16.x cached it to `ShipNavPanelBodies.txt` in
  `Data\SFSE\Plugins` — mod managers never track a runtime-generated file, so
  it survived uninstalls as clutter; 0.17.0 removed the cache and deletes that
  leftover file if it finds one (removal confirmed the same launch).
- **Engine writes are two vtable slots** (in-memory, never serialised) and, only
  with the default-off throttle test on, the `disabled` flag of a transient
  input event.

The one real hazard of holding an id is **FF-prefixed runtime forms** — ships
and spawned POIs, whose ids the engine can recycle. A lock on one is dropped
after 60 seconds out of the feed (v0.4.4) so it cannot silently follow whatever
inherits the number. Static bodies are unaffected.

## Controls

| key | user event(s) | what it does |
|---|---|---|
| scanner | `SHMonocle` | open / close the panel |
| mouse wheel | `ZoomIn` / `ZoomOut` | move the highlight (spliced away from the camera while open) |
| D-pad up / down | `Up` / `Down` | move the highlight, on a controller (v1.1.0) |
| POV toggle (**Q** here) | `TogglePOV` | lock the highlighted body, or clear it if already locked |
| the player's course-lock key (**RB** on a pad) | `LockCourse` | aim the cruise autopilot at the highlighted body, or clear it |

`sConfirmEvent`, `sBrowseUpEvent`, `sBrowseDownEvent` and `sLockCourseEvent` are
comma-separated lists sharing one walk (`MatchesEventList`); an entry is a
user-event **name** or `#<id>`, a raw key code for a key the game leaves
nameless. `sLockCourseEvent` shares that walk and the camera splice, and **reads
as empty while `bLockCourse` is off** — a switched-off feature must not take a
key away from the camera, from the game, or from the "that key is not one of the
panel's controls" advice the log prints.

**The course key is vanilla's own, and while the panel is open the mod TAKES it
rather than sharing it.** With the panel closed the key is untouched. That is
also why it is the only control with **no hint pill** — the game already prompts
with `$CruiseCourseLock` in cruise, and a second prompt for the same key is noise
(item closed, not deferred).

⚠⚠ **Sharing it was tried first and is WRONG — the ordering is the opposite of
what it looks like.** The reasoning was: the input thread only stores an atomic
while the dispatch waits for the next high-feed tick, so the mod's lands after
vanilla's synchronous ones and last write wins. **In game it lost.** Two
symptoms from the tester:

- target on A, panel highlighting B → the course went to **A**;
- **no target**, panel highlighting B → the autopilot came on and **switched
  straight back off**.

The second is the diagnostic. Nothing but the mod could have turned it ON —
vanilla cannot lock a course without a target — so the OFF is vanilla's
`{uBodyID: 0}` arriving *after* it. **That also names what 0 means: CLEAR.** And
the earlier `WeaponGroup1` build is the control that rules out the alternative
reading: a course set on an untargeted body persisted perfectly there, so the
engine is not cancelling it for want of a target.

**So the press is spliced out of the UI's input queue** while the panel is open
in cruise with a row highlighted — the camera tap's technique, on the receiver
that feeds the menus: unlink, call through, relink immediately. The SWF never
sees the press, the mod's dispatch is the only one, and both symptoms should go.
⚠ Consequence worth stating because it bit the first version: **with the press
taken away, the mod must handle EVERY case**, including the highlight already
being the info target. The stand-aside rule that covered that case is gone —
standing aside now means nothing happens at all.

Earlier choice, kept for its reasoning: `WeaponGroup1` (primary fire — one event
on both devices, idle in cruise) worked perfectly but made the game print its own
"weapons unavailable" toast on every press. **It is the fallback** if taking
`LockCourse` from the UI proves worse than the toast.

**The browse lists carry both devices at once and that is deliberate.** A user
event is not tied to a device — the engine resolves it against whatever is in
the player's hands — so `ZoomIn,Up` means "the wheel, or the D-pad, whichever
you have". No device check anywhere, by the user's call: if someone has bound
a keyboard key to the ship HUD's `Up`, browsing with it is the right answer,
not a collision.

⚠ **On a gamepad an `#id` is NOT a virtual-key code.** It is Bethesda's own pad
code, and the tester's log maps the ship set exactly: D-pad up **1**, down
**2** (so left **4**, right **8**), LS click **64** (`Boosters`), RB **512**
(`LockCourse`), A **4096** (`SelectTarget`), X **16384** (`XButton`), Y
**32768** (`WeaponGroup3`) — and **RT is 10** (`WeaponGroup1`), which is the
tell: `0x000A` is not a producible button mask, so the triggers occupy the two
values a mask cannot reach (LT 9, RT 10). Nothing in the matcher checks
`deviceType`, so an id matches any device reporting that number; the
unnamed-press-only rule is what keeps that harmless, and every event shipped is
named.

**The default is `TogglePOV`, and it is chosen for being a name.** A key that
already drives the camera is a fair candidate because the confirm key is spliced
out of `PlayerCamera`'s queue while the panel is open, exactly as the wheel is —
so it locks without swinging the view. That is the wheel's own argument applied
to the confirm key, and it means the worst a failed splice can do is move the
camera.

⚠ **`LShoulder` is not the POV toggle**, despite sharing a key with it here
(id 81 = Q). It appeared in a v0.7.1 log and was written into these notes as the
POV toggle on that basis alone; setting it does nothing. Reading an event name
off a log line is not the same as knowing what the key does — the log says a
name was reported, not which action it belongs to.

### Why not an id, given `#67` worked

`#67` (C) was the default in v0.7.2–0.7.3 and did work, on the strength of C
carrying no user event in cruise — so nothing could collide with it.

**An `#id` entry matches only an UNNAMED press**, and that restriction is what
makes allowing an id safe at all. An id is a physical key and cannot follow a
rebind, so without it a player who bound a ship action to C would have the mod
fire on the same keystroke the game acts on — nothing here consumes the key. A
press with no user event is the **engine** saying nothing is bound there; the
moment something is, the name appears and the game's binding wins.

But that safety is also the flaw: such a player loses the confirm key outright,
with nothing on screen to say why. A name resolves wherever they have put it.
**Silent total loss of a feature is a worse failure mode than a swinging
camera** — which is the same trade the wheel made, and it points the same way.
Prefer a name always; reach for an id only for a key the game leaves nameless.

The mod itself still bakes in no id.

Confirmed free in cruise besides the above: `Quickkey2`, `Quickkey3`. Confirmed
NOT free: `SelectTarget` (E, still cycles targets), `RepairShip` (4), and
`XButton` (R, which opens the planet map once a target is selected). W/S is
permanently out.

Diagnosing a key that does nothing: press it with the panel open and read the
log. Every unmatched press reports its id and its name — or says it has none —
and prints the entry to paste into `sConfirmEvent`.

## Settled — do not re-derive

Each of these cost real time; the reasoning is in the findings docs.

- **Controller facts, from the tester's log and the exported AS3 (2026-08-02).**
  - The ship HUD's D-pad is the user events **`Up`/`Down`/`Left`/`Right`**, and
    the exe carries the hint keys `!Up_ShipHUD`/`!Down_ShipHUD`/… to prove it
    (`!<UserEvent>_<Context>` at file offsets 81108720–81108768, beside
    `!RepairShip_ShipHUD` and `!Cancel_ShipHUD`). Their only ship-HUD consumers
    are `PowerAllocationComponent` — **off for the whole of cruise** — and
    `ShipHudQuickContainer`, which only lives while the loot panel is open.
  - ⚠ **On a gamepad, vanilla's EXIT CRUISE is bound to `SHMonocle` — this
    mod's own open key** (`ShipReticle.as:401`, enabled whenever
    `CruiseModeHUDActive`, `:1291-1293`; on KBM it is the separate `Cruise`
    event instead, `:402`). It is a **`HoldButton`**, and `HoldButton
    .HandleUserEvent` only acts once the hold timer completes — so a *tap* is
    free and opens the panel, while a *hold* drops cruise. Nothing to fix, but
    do not "free up" the scanner key on a pad without knowing this.
  - `ButtonBaseData(label, param2)` takes a `UserEventData` **or an Array** of
    them (`:16-31`) — the two-way hint idiom, and the only way to get a paired
    cap.
- **The known-better route for "is this event bound, and on what?"** is the
  **`ControlMapData` feed**: `BSDisplayObject.as:101` subscribes it through the
  same `BSUIDataManager.Subscribe` this mod already uses, and it carries
  `vMappedEvents` (`strUserEventName`, `strButtonName`, `aButtonName`,
  `sContextName`) plus `uiController`. `ButtonKeyHelper.GetButtonNameForEvent`
  answers off exactly that and returns **empty for an unbound event**. v1.1.0
  did *not* take this route — per-device ini keys plus vanilla's `uiController`
  were proportionate for one pill, and a third subscription on the load path is
  not free (see the v0.7.4 race and the load freeze). Reach for the feed if a
  future feature needs to *discover* bindings rather than pick between two.

- **The ship HUD's target feed cannot tell a moon from a planet.** Moons arrive
  typed `TT_PLANET`, identical to planets, and nothing in the entry names a
  parent. `BGSPlanet::PlanetData` has no relational field either — temperature,
  density, a surface tree, an orbital angle.
  - **`bIsCelestialParentBody` does NOT mean "has moons".** Ground truth for
    Alpha Centauri: Jemison (moon Kurtz), Bondar (Grissom, Curbeam), Gagarin
    (none), Olivas (Lovell, Chawla, Hawley, Voss, Zamka). The flag was true
    only on Jemison, and Kurtz was the only moon in the feed at all — so it
    reads closer to "is the parent of an entry currently in this list".
  - **Feed order does not group families.** Order was Jemison, Bondar, Gagarin,
    Kurtz, Olivas: Kurtz is two entries from its own parent. A v0.3.3 rule
    built on that assumption indented everything after Jemison and was removed.
  - **The feed also lists only some moons** — one of eight here — so the panel
    is inherently a partial list of the system.
  - **★ The hierarchy is in the PNDT record: GNAM "Galaxy Data" = star system
    id, PARENT planet id, planet id.** Found in xEdit by the tester. Earth =
    (Sol 0, parent 0, planet 3); Luna = (Sol 0, parent 3, planet 11) — a planet
    carries parent 0, a moon carries its planet's id.
  - **★★ GNAM IS NOT IN THE RUNTIME RECORD — that is why four searches failed.**
    The v0.3.8 dump settled it: PNDT objects are allocated back to back with a
    stride of exactly **0x58**, and every word of a record is spoken for —
    `surfaceTree` 0x38, a float 0x40, `temperatureCelcius` 0x44 (20.0 Jemison,
    −83.0 Olivas), `density` 0x48, `periAngleInDegrees` 0x4C (186.0),
    `resourceCreationSpeed` 0x50, **form id 0x54**. Anything past 0x58 is the
    *next planet's record*, which is why every scan turned up pointers and float
    bit patterns. **Do not go looking for it in memory again.** The plugin reads
    GNAM out of `Starfield.esm` itself instead.
  - **Read the WHOLE load order, not just the master.** Shattered Space adds 11
    bodies in system 119226 — Va'ruun'kai is a *moon* of Kavnyk I — and mods can
    add more. Plugin list and indices come from
    `TESDataHandler::compiledFileCollection.files`; a record's top byte indexes
    that file's own master list, where "one past the last master" means the file
    itself, so the runtime id is that slot swapped for the owner's load-order
    index. **MAST names are matched case-insensitively** — Shattered Space calls
    its master `starfield.esm` while the game says `Starfield.esm`, and an exact
    compare silently fails to resolve every override in the file. Every resolved
    id is checked with `LookupByID` → `kPNDT`, so bad arithmetic or a stale
    `TESFile` offset loses entries instead of inventing them. The cache is
    fingerprinted with the load order because it stores *runtime* ids.
  - **Names come from the archives, not from the record.** `FULL` is a localised
    string id and the strings are not loose — they are in
    `<Plugin> - Localization.ba2`. That archive is **BTDX v2 `GNRL`, 32-byte
    header, 36-byte entries**, names in a table at the tail in entry order. The
    string table inside is count, data size, `{id, offset}` pairs, then
    null-terminated UTF-8. Verified before implementing: 43005 → Jemison,
    42692 → Kurtz. Editor-id names remain the fallback where a plugin ships no
    archive.
  - **"Has a name" and "belongs in the list" are separate questions.** Keep them
    apart, because they *look* interchangeable: generated bodies had no editor-id
    name, so filtering on "no name" happened to exclude them — until v0.5.0
    resolved `FULL` and gave them names, at which point The Eye appeared nested
    under Jemison as a moon. Listing is decided by the editor-id convention
    (`BodyEntry::authored`), never by whether a name exists.
  - **A body can exist twice as two different form types.** The HUD offers The
    Eye as a `kREFR` (`0x28FBA9`); its record is a `kPNDT` (`0x2900AC`). No
    amount of form-id matching will dedupe those, so the panel must avoid
    listing the record rather than hope to catch the collision.
  - **Locations join to planets by id, not by name.** `LCTN` carries `XNAM`
    (Star ID) and `YNAM` (Planet ID), which are the same numbers as `PNDT`'s
    GNAM — `SAlphaCentauri_PJemison_Surface` is XNAM 71456, YNAM 3, and Jemison
    is system 71456, planet 3. Climb `PNAM` from any location until **`YNAM` is
    non-zero** and you have the body. Neither `PNDT` nor `WRLD` points at a
    location, so this is the join; do not go looking for a form reference.
  - **Sol is star system 0, so a zero star id is DATA, not "absent".** Any test
    of the form "both ids non-zero" quietly loses the whole home system —
    Cydonia on Mars, New Homestead on Titan, the Deimos staryard. Planet ids
    are 1-based, so `YNAM` alone is the presence test, and `XNAM` is read
    alongside it rather than validated. This was written into the settlement
    recipe as "climb until XNAM/YNAM are non-zero" and caught only because the
    offline check listed the bodies it matched instead of counting them.
    **THIRD STRIKE (v0.8.6):** `AppendSystemBodies` shipped with
    `systemID != 0` as its presence test and the whole-system list was
    silently dead in Sol for five versions — every earlier whole-system test
    happened to run elsewhere, and the tester caught it hunting for Luna.
    Presence is a separate bool (`haveGalaxy`), never the value itself. Any
    new code touching `systemID` gets audited for this on sight.
  - **A count is not a verification; print the names.** Every parse in this
    project that went wrong went wrong *plausibly* — 631 of 1765 records looked
    like a working parse, and "45 settlements resolved" would look equally fine
    with Sol missing. `tools/Check-Settlements.ps1` re-implements the join
    against the file directly and lists what it matched, which is the only
    reason the Sol case surfaced before an in-game test.
  - **A record can carry the SAME subrecord signature twice, meaning different
    things.** A planet has two `GNAM`s — a 4-byte float and the 12-byte galaxy
    data — and two each of `FNAM` and `CNAM`, because signatures are reused
    freely between component (`BFCB`/`BFCE`) blocks. Matching on signature alone
    takes the float and reads a hierarchy out of nonsense. The parser's
    `size >= 12` check is what makes it right, and is load-bearing rather than
    defensive. `drawCircle`, incidentally, does work.
  - **Parsing the ESM: two things that will bite.** Every PNDT record is
    zlib-compressed (1765 of 1765), the stream starting 4 bytes in, after a
    `uint32` inflated size. And **`XXXX` carries the real 32-bit length of the
    *next* subrecord**, whose own 16-bit size field then reads 0 — miss that and
    the walk desyncs into the middle of a large payload. It silently cost 1134
    of 1765 records in the prototype, Kurtz among them, while still *looking*
    like a working parse. `tools/ExportBodies.pas` (an xEdit-side alternative
    from that era) was deleted in 0.17.0 along with the cache file it fed —
    its instructions would have users place a file the plugin now deletes at
    launch; git history keeps it.
  - **`BGSPlanet::PlanetData`'s member comments are stale AND the struct is
    incomplete.** The comments start at `0x30`, but they were written against a
    `0x30`-byte `TESForm` and `TESForm` is now `0x38` — so the compiler places
    every member eight bytes later than documented (`surfaceTree` 0x38 …
    `resourceCreationSpeed` 0x50), and `static_assert(sizeof == 0x58)` is
    satisfied by padding. Reading `0x4C` as GNAM therefore returned
    `periAngleInDegrees`: v0.3.5 logged "system ids" of 186.0, 77.0, 151.0,
    218.0, 209.0 reinterpreted as integers, and `0x54` returned each body's own
    form id. **GNAM lies past the declared end of the record**, so no offset
    derived from that struct can ever be right. Another instance of the stale
    CommonLibSF layout hazard in `STARFIELD-NOTES.md`.
  - **The offset AND the field order are discovered at runtime, not hardcoded** —
    the record's tail is scanned for a triple that behaves like GNAM (one shared
    system id, small distinct planet ids, at least one body with parent 0, and —
    decisively — at least one body whose parent equals another's planet id). All
    six slot orderings are tried, since xEdit's display order is no guarantee of
    the memory order. Bounded by `VirtualQuery`, as it reads past a declared
    struct.
  - **Anything per-body must be cached, and any scan rate-limited.** v0.3.6 ran
    the whole scan on every low-frequency feed callback because it never
    succeeded, doing a `VirtualQuery` per candidate offset per body on the UI
    thread — the game became a slideshow. Each attempt now snapshots each form
    once and scans plain memory; attempts are 3 s apart and capped at 8; and
    every form's GNAM is cached by form id, since it never changes.
  - **The star map providers are a dead end from the ship HUD — mechanism
    proven 2026-07-30 ([PHASE5-STARMAP-DATA.md](PHASE5-STARMAP-DATA.md)).**
    `StarmapSystemBodyInfoProvider` subscribes fine but never fires with the
    map closed — zero callbacks in a full cruise — and Phase 5 showed why, for
    ALL of them: every galaxy/system/POI provider is menu-scoped engine-push;
    the native `_Watch` accepts any name from any movie, but the publishers
    live and die with their menu, and `GetDataFromClient` only re-reads the
    calling movie's own (never-filled) buffer. Do not re-probe. The one
    exception worth having is `InfoTargetProvider` (Open work). The AS3
    `BSGalaxyTypes` `BT_*` enum is NOT the feed's `TT_*` enum — never mix
    them. Extracted pool: `M:\Starfield\Extracted\vanilla-interface\` (the
    full Interface BA2, verified name-for-name), script exports under
    `M:\Starfield\Extracted\scripts\`.
- **An event name in a log tells you a name was reported, not what the key
  does.** `LShoulder` was seen once in a v0.7.1 log, matched to id 81 = Q, and
  written up as "the POV toggle" — it is not, and setting it does nothing. The
  POV toggle is `TogglePOV`. The only way to learn what a key does is to press
  it and watch the game, which is what the tester did.
- **A key can carry NO user event in a given context, and that press is a null
  name, not an empty one.** `BSFixedString::c_str()` hands back a null pointer,
  so any `if (... && userEvent)` guard drops the event entirely — it never
  reaches matching *or* logging, and the key looks like it was never pressed.
  C in cruise is exactly this. Read `idCode` when the name is absent, and never
  gate a diagnostic on the thing being diagnosed.
- **The id codes in `PHASE0-FINDINGS.md` are virtual-key codes.** 67 = C,
  84 = T, 81 = Q, 13 = Enter, 9 = Tab, 192 = `~`. That was not stated when the
  table was written and it is what makes `#67` portable rather than a magic
  number: it means "the C key", not "this tester's binding".
- **A `g_somethingReady.load()` guard does NOT make a builder run once.** It is
  check-then-act: two threads read false, both build. The SFSE per-frame task
  and the data-feed callbacks land on whatever BSJobs worker is free — one log
  second shows the same logical work reporting from five thread ids — so this
  is routine, not a corner case. For a builder that only makes a clip the cost
  is a duplicate; for one that enters the **AS3 VM it is an access violation**,
  because the VM is not thread-safe. That is the v0.7.5 fix: `TryInstallSubscriber`,
  `TryCreatePanel` and `TryCreateArrow` now take a `SingleWinner` claim, released
  on exit rather than latched so a probe that finds no movie can retry.
  - The input and camera taps had `compare_exchange` from the start, so the
    hazard was known and the pattern existed in the same file. **A pattern
    applied in some places and not others is worse than one nobody knows**: the
    `OnFrame` comment saying "this task lands on two threads in the same frame"
    is three lines below the unguarded call that crashed.
  - **It survived seven versions of working perfectly**, because losing a race
    needs two threads inside a ~microsecond window on one specific frame. A
    crash that reproduces under one config and not another is evidence about
    TIMING, not about the config.
- **`endFill` ends the run, so every drawn shape needs its own `beginFill`.**
  One fill up front draws the first shape and silently leaves the rest
  unfilled — v0.7.0's settlement glyph shipped as a bare ground line with three
  invisible towers above it. `ringedGiant` had it right from the start, which is
  the only reason the giants were unaffected.
- **A borrowed `TextFormat` carries the DONOR's alignment.** The donor is the
  HUD's centred lock-on caption, so any field using it is centred until `align`
  is set explicitly. Cost one build: the panel shipped with centred names.
- **To suppress an input, hook the receiver that consumes it and splice the
  event out of the queue — do not flag it and hope.** Proven both ways on the
  same day: flagging failed for the throttle, splicing worked for the mouse
  wheel at `PlayerCamera::PerformInputProcessing` (v0.2.3, view unchanged, mouse
  look unaffected). `PlayerCamera` is a `BSInputEventReceiver` with a real
  singleton id, so it takes the same live-vtable hook as `RE::UI`. Relink before
  returning and every other receiver still sees the chain whole.
- **Ship flight input cannot be suppressed by marking events `disabled`**
  (tested v0.2.1). The write lands and even persists — the engine pools the
  event objects, so a later press arrives still carrying the flag — and the ship
  accelerates regardless. Either the flight consumer runs before `RE::UI` in the
  receiver chain, or it ignores the flag; the log cannot tell which, and it does
  not matter.

  ⚠ **This line used to end "the panel must use keys the game already ignores in
  cruise", and that was an overstatement** — written after the throttle failure
  and before the wheel splice succeeded a day later. It is not what the
  experiment showed. Two variables changed at once between the two tests:

  |  | throttle (failed) | wheel (worked) |
  |---|---|---|
  | technique | set `disabled` | splice out of the queue |
  | hooked | `RE::UI` | `PlayerCamera` — the actual consumer |

  So the throttle test shows only that **flagging at a non-consumer does
  nothing**, which is the same lesson the wheel taught positively. A splice on
  W/S was never attempted, because its consumer was never found.

  The wheel also proves the rule wrong by example: `ZoomIn`/`ZoomOut` are *not*
  keys the game ignores — it acts on them, and the mod takes that function away
  while the panel is open. The real rule is **a key needs either the game to
  ignore it, or a hookable consumer to splice it away from.**

  **W/S is therefore "not done", not "impossible" — but leave it alone anyway:**
  - The consumer is findable. `PlayerControls` is not a class CommonLibSF
    defines, but its handlers carry real `IDs_VTABLE.h` ids —
    `PlayerControls__FlightMovementHandler` **433534**,
    `PlayerControls__StandardFlightControlMode` **433532**,
    `PlayerControls__ShipEquipmentHandler` 433616.
  - **The failure modes are not symmetric.** A wheel filter stuck on means the
    POV stops changing — invisible, harmless, gone when the panel closes. A
    throttle splice stuck on means the ship will not accelerate or decelerate.
    That asymmetry is why `bSuppressThrottleTest` is default-off and gated twice
    on cruise-and-panel-open, and it does not improve with a better technique.
  - POV is decoration; throttle is flight. The prize is small and the downside
    is the worst this mod could do to someone.
- **E (`SelectTarget`) is settled on design, not on feasibility.** It faces the
  same unknown consumer as W/S, but that is not the reason to leave it: the mod
  **points** because targeting by id is impossible from the UI layer, so E is
  the only way the player can actually acquire what the panel is pointing at.
  Taking E away removes the mechanism the whole design rests on. Self-defeating
  rather than hard. (The route that would change that is
  `Spaceship::TargetingMode`, vtable mapped at 450764 / 450766 — a different
  project from stealing a key.)
- **`disableplayercontrols` is not an alternative** — it drops the ship out of
  cruise without the hidden loading screen, i.e. outside the cruise state
  machine's normal teardown. Not worth the risk.
- **Vtable ids are healthy; it is the *function* ids that are placeholders.**
  `IDs_VTABLE.h` has zero `{ 0 }` entries, `IDs.h` has 505. So a vtable-based
  hook can use its Address Library id directly — the live-object trick is only
  needed where a *function* id is missing.

- **The candidate list only rebuilds when the LOW feed publishes, and the feed
  publishes on target-set CHANGES.** A mod-side state change (clearing a lock,
  say) does not make the engine say anything, so any candidate row whose
  presence depends on mod state must be evicted or patched by the code that
  changes that state — waiting for "the next rebuild" waits for unrelated
  traffic. Caught in v0.8.9: the auto-cleared moon's appended row sat in the
  panel indefinitely.

- **Never hold a plugin mutex across a Scaleform call, and menus-closed is not
  world-settled.** Feed callbacks take plugin mutexes from inside the engine's
  dispatch and run concurrently across the BSJobs pool, so a mutex held while
  entering the VM is a lock-order inversion — a silent freeze, not a crash.
  `RefreshPanel` carried exactly that for ~20 versions; it now snapshots under
  the lock and renders outside. And the 2026-07-28 freeze log proved the
  menus-closed gate passes while the load transition is still on screen, the
  movie mid-init and about to be rebuilt — `WorldSettled` therefore requires
  2.5 s of continuous menus-closed. Full account in `STARFIELD-NOTES.md`
  ("A load-time FREEZE"); also there: `Subscribe` fires the new handler
  synchronously with current data, and each feed subscribes separately with
  its own failure to track.

- **Never hide an individual HUD icon with `visible=false`, and never cache the
  off-screen container's handle.** `ShipReticle.GetClip` uses `visible==false`
  as its "pooled, free to recycle" test — an outside write corrupts the pool
  (duplicate live-array entries, clips re-keyed to other targets mid-flight).
  And `OffScreenIndicatorParent_mc` is timeline-placed art the reticle's
  animations can re-create, so the path is resolved fresh every tick. Both
  facts read out of the decompiled SWF; full trace in
  [PHASE3-BLIP-PLAN.md](PHASE3-BLIP-PLAN.md), which also records why per-icon
  `alpha=0` and fighting `CruiseModeOffScreenPlanetIconLimit` (a private
  const) were rejected.

- **Targeting a body by id is impossible from the UI layer.** No such event
  exists; `ShipHud_Target` is parameterless ("target what is hovered") and
  `iInfoTargetIndex` is read-only to the SWF. Would need
  `Spaceship::TargetingMode` (vtable **is** mapped: 450764, 450766 — try
  vtable-observation before Ghidra). ⚠ Still true of TARGETING, and it is not
  worked around by shaping the candidate data: the feed is a PUBLICATION, not an
  input, so removing entries changes what the HUD draws and nothing about what
  the engine considers. Anything that could filter the engine's candidates lives
  inside `TargetingMode` — the same place "set target to X" lives, and set-target
  is the smaller ask. The two things that ARE reachable from here are drafted
  under `[Experimental]` above: press vanilla's own key for the player, and
  `Reticle_OnCruiseLockCourse`, which takes a `uBodyID` but drives the AUTOPILOT.
- **⭐ The mod can dispatch UI→engine events, and the engine acts on them.**
  CONFIRMED in game 2026-08-02 by the course lock. Route: `GetVariable` the
  class, `Invoke` the static — the `Shared.GlobalFunc.PlayMenuSound` route
  exactly, no hook, no patch, no address id. **`flash.events.Event` DOES come
  back from `CreateObject`** (log: `[dispatch] route: new Event(...) ->
  BSUIDataManager.dispatchEvent`), which had been the open question, since every
  other class this mod constructs is a game class. Kept as a fallback because it
  costs nothing: `BSUIDataManager.dispatchCustomEvent(type, params = null)` is a
  public static that builds the `CustomEvent` itself, so a dispatch needs no
  event class constructed at all.
- **⭐⭐ `Reticle_OnCruiseLockCourse` accepts a body id vanilla never sends.**
  The base game only ever passes `0` (reticle handler) or the CURRENT info
  target's `uniqueID` (far-travel icon). Passing an arbitrary highlighted body's
  id works: the autopilot engages and the ship turns to it, **with nothing
  targeted**. This is the one by-id verb in the UI layer and it is now proven.
  Confirmed to work on **stations and POIs** as well as planets.
- **⚠⚠ `uBodyID` MEANS uBodyID: only a CELESTIAL BODY takes a course by id.**
  Given anything else, `Reticle_OnCruiseLockCourse` neither refuses nor
  substitutes — it takes the course with an **unresolved destination**. No entry
  reports `bIsCruiseTargetLock`, **no orange course indicator draws**, and the
  ship simply heads for the system's origin, which is where the star sits. So a
  POI, ship or station cannot be reached this way at all; vanilla gets to them
  through the info target (`{uBodyID: 0}`), a route this mod cannot take because
  setting the info target is the dead end Phase 0 closed.
  **Three flights, two theories buried, and both were mine:**
  1. *"Runtime FF-prefixed ids are unusable."* Dead — the same contact targeted
     the vanilla way produced `the engine reports a course locked on 'Sensor
     Contact' (FF015BCB)`, exactly the id the row carries. The number was right.
     (The "signed int32" reading dies with it; it only ever existed to explain a
     boundary that was not where the fault was.)
  2. *"`uniqueID` is the wrong FIELD, the event wants the dossier's `uBodyID`."*
     Dead — measured: **the low feed carries no per-entry `uBodyID`**.
     `uniqueID` is the only id an entry has.
  ⭐⭐ **What actually settled it was the tester noticing what was NOT on screen**
  — no orange indicator — which reframed "it locked onto the star" from a
  mechanism into an appearance. Every theory above was an attempt to explain a
  *substitute body* that was never chosen. **When a symptom is described by what
  something looks like, find out what is missing before theorising about what is
  there.**
  ⭐ And the posture that survives all of it: **when an engine call takes an id,
  audit what came back rather than assuming it refused what it could not use.**
- **⛔⛔ WHICH PAYLOAD A FIELD LIVES ON IS NOT GUESSABLE, AND I HAVE GUESSED IT
  TWICE.** `uBodyID` was read off `PlanetCardInfo` and expected on a low-feed
  entry (it is not there). `bFarTravelAllowed`/`bFarTravelDisabled` were read off
  the `InfoTargetProvider` payload and expected on a low-feed entry (also not
  there) — and that one silently disabled a probe, so a whole flight measured
  nothing. **The per-entry list has been written down since July**
  ([PHASE1-SWF-FINDINGS.md:264](PHASE1-SWF-FINDINGS.md:264)): `uniqueID`,
  `uTargetType`, `bLandingAllowed`, `bLandingDisabled`, `bIsCruiseTargetLock`,
  `bHasQuestTarget`, `bMarkerDiscovered`, `bDetectedByPlayer`, `hostile`,
  `iFaction`, `iLevel`, `fMinArrivalDistance`, `handle`. ⭐ **Check that list
  before writing code against a field name, and if the field is not on it, make
  the code REPORT its presence rather than depend on it.** The `uBodyID` pass got
  that right by accident and answered its question in one flight; the far-travel
  pass got it wrong and answered nothing.
- **⭐ THE MEASURED SCOPE: planets and moons take a by-id course; stations, POIs
  and ships do not** (every type tried, 2026-08-03). Exactly what the two-route
  mechanism predicts — the first time on this feature that a prediction and a
  measurement agreed, and the reason `IsCourseableType` can be trusted where four
  earlier gates could not. `TT_STAR` is excluded for want of evidence rather than
  against it: no star has ever reached the list to be tried, and a star is an
  `STDT` record, not the `PNDT` the by-id route resolves.
- **⭐⭐ TWO ROUTES TO A COURSE, resolving DIFFERENT THINGS — this is the whole
  story and it took four flights.** `{uBodyID: 0}` (vanilla's key) means "use the
  **current info target**", resolved through the targeting system, so it reaches
  anything targetable — a POI, a contact, a ship. `{uBodyID: <id>}` (the mod, and
  vanilla's far-travel button) is resolved as a **body**; hand it anything else
  and the engine takes the course with nothing to fly to. The mod cannot use the
  first route, because setting the info target is the dead end Phase 0 closed.
  So: **by-id means bodies, and that is structural, not a bug to fix.**
- **⚠⚠ THEREFORE THE GATE IS A DELEGATION, NOT A DEAD KEY — and that reframing
  is what makes it correct.** On a row the mod cannot course, the press is left
  in the queue and vanilla's `{0}` handles it: with that POI targeted, the course
  lands through the route the mod does not have. **Taking the press on those rows
  does not merely fail, it breaks a flow that works without the mod** — a worse
  bug than the one the gate was for, and the reason the gate is right after
  having been withdrawn once.
- **⛔⛔ FOUR GATES ON ONE FEATURE. The lesson is about the KIND of evidence, not
  the amount.** FF-prefixed ids (dead: the engine holds a course on that id when
  it is targeted), the wrong id field (dead: measured, no per-entry `uBodyID`),
  "only a celestial body" (withdrawn on a tester report), then the same rule
  again once that report was retracted as *"a case of bad memory"*. Each of the
  first three took one row's behaviour and generalised it; the fourth rests on a
  **mechanism** — two routes, one of which the mod cannot reach — which is why it
  should stand. ⭐ **A rule that explains why the boundary is where it is
  survives; a rule fitted to where the boundary appeared to be does not.**
  ⭐ And the process lesson, paid for twice: **a recollection is data with no
  timestamp.** "It worked before" outranked a diff and two measurements for three
  rounds. The way out was not argument but an A/B that could actually move the
  suspected variable — and note the tester's own A/B could not, because adding a
  second key to `sLockCourseEvent` brought it under the splice as well. **A
  control applied by the same setting it is meant to control is not a control.**
- **⚠ `TargetOnlyData.uniqueID` and `PlanetCardInfo.uBodyID` are NOT the same
  number** — `payload.uniqueID` **386531** vs `pci.uBodyID` **385501**
  ("Masada IV", a landable target) in the 2026-08-02 dump. They agree for a
  *bare planet* (385509 and 385507 in the previous session), which is exactly why
  this was written up as "closed, same number" and had to be retracted a day
  later. **Two agreeing samples are not a rule — pick the sample that can
  disagree.** Live question: which of the two the course-lock actually wants.
- **⚠⚠ THE AUDIT'S ONLY READBACK IS PER-ENTRY, so it is blind to a course on
  anything the feed does not carry** — the system's own star included, which is
  precisely the failure being chased. Caught when a flight with the guard lifted
  dispatched twice and the log said *nothing at all*: no course reported, no
  mismatch, silence. **Third check-that-could-not-pass in this project and the
  second on this feature**: the timeout that covers it was written for the first
  flight, deleted as scaffolding once the feature looked solved, and is now the
  only instrument that can see the fault. **A diagnostic is not scaffolding
  because the happy path stopped needing it.**
- **`Reticle_OnCruiseLockCourse` with `uBodyID: 0` most likely means "use the
  CURRENT INFO TARGET"**, which merely *looks* like CLEAR when nothing is
  targeted. First written up here as "clear" on the strength of the shared-key
  flicker (the mod set a course on an untargeted body, vanilla's `{0}` arrived
  after and switched it off) — but the sensor-contact finding makes the weaker
  claim the better one: vanilla can put a course on a POI, and if `{0}` were
  literally "clear" then the only path left would be the far-travel button's
  by-id dispatch, which is the very path that fails for a runtime id. "Use the
  current target" explains both. **Not proven either way**; what is proven is the
  observed behaviour with no target, which both readings predict.
  ⚠ A third possibility this raises and nobody has closed: the far-travel button
  sends **`TargetOnlyData.uniqueID`**, and `TargetOnlyData` is a *different
  object* from the low-feed entry (`SetTargetLowInfo`'s param2 vs param1 — it is
  the `InfoTargetProvider` payload). If those two `uniqueID`s differ for a POI,
  then the mod has simply been sending the wrong number and the FF boundary is a
  coincidence. Cheap to settle: target a contact, and compare the
  `InfoTargetProvider` probe's `uniqueID` with the panel row's id.
- **The SWF's dispatch lands AFTER a mod dispatch queued from the same
  keypress**, even though the mod's input hook runs first — do not assume a
  deferred dispatch wins a race with the SWF, it does not. That is what forced
  the splice.
- **Vanilla cannot lock a course without a target, and the engine draws the
  course as an ORANGE RING around the body's HUD blip** (tester, 2026-08-02).
  Both matter to this mod: the first is why sharing the `LockCourse` key is
  mostly collision-free (vanilla's press does nothing when nothing is
  targeted), and the second is a trap — **the mod HIDES vanilla blips in
  cruise**, so a course set on a body that is neither the highlight nor the lock
  would have its ring hidden with its blip. Two things keep that from biting:
  the panel keeps highlight+lock blips while it is open, and the course is
  normally set on the highlight. If the course and the lock are ever allowed to
  diverge for longer, the course body has to join the blip pass's exempt set
  beside the quest markers and the E-target.
- **Dash rows are effectively extinct** (tester, 2026-08-02): since moons stopped
  being listed under their parent unconditionally, an untracked row is
  essentially never seen — the feed carries the system's planets. Any argument
  of the form "the marker covers bodies the feed does not" is therefore
  defending an empty set; do not lean on it again.
- **`Cancel` is NOT usable as a panel control.** It reads as free in the AS3 —
  `ShipReticle.ProcessUserEvent` consumes it only in `STATE_MONOCLE`, not in
  cruise — but on keyboard and mouse it is taken above the reticle (Escape), so
  a design resting on it works on a pad and dies on KBM. Tested 2026-08-02, and
  it killed an otherwise good three-state scheme (confirm locks, confirm again
  sets course, Cancel unlocks). **If a "back out" control is ever needed again,
  the answer is a HOLD on a key the mod already owns**, not a second key: the
  input walk already reads `heldDownSecs`, and vanilla has hold-hint components
  (`ReleaseHoldComboButtonData`, which drives the Cruise button itself).
- **⛔ Driving vanilla's target cycle is a DEAD END — built, flown, deleted
  (2026-08-02). Do not rebuild it.** `bAcquireTarget` dispatched the same
  parameterless `ShipHud_Target` the player's own key sends, once per settle
  window, until the info target became the selection. It behaved exactly as
  designed, and the log is the whole finding:

  ```
  [acquire] run requested for 0005E1DA (confirm key)
  [acquire] step 1 moved the info target to 0005E1DB
  [acquire] the cycle did not move - nothing acquirable from this heading
            - wanted 0005E1DA, info target 0005E1DB, 2 press(es) sent
  ```

  One press moved the target to the NEIGHBOUR; the next moved nothing at all.
  **That is Phase 0 §6b/§6c answered from the cockpit**: the restriction is not
  an enumeration filter you can walk past, and there is no list to walk — cruise
  acquires by POINTING, so pressing the key faster reaches nothing the key could
  not already reach. The corollary is the answer to "could we make everything
  else an invalid target so only one remains to cycle": no, and for a reason
  deeper than mechanism — with acquisition geometric rather than list-based,
  emptying the set empties it to *zero* exactly when the selection is
  off-centre. Course lock is what to reach for instead; it needs no targeting at
  all.
- **`bIsCruiseTargetLock` is PER-ENTRY on the low feed** — it rides `TargetLow`,
  the object handed to `SetTargetLowInfo`, not the payload root. So "which body
  is the autopilot flying to" is answerable per row, which is what gives the
  experimental lock-course key a readback. It is the AUTOPILOT's state and has
  never meant "is targeted".
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
- **The 2026-07-29 scanner-mode drop is NOT the mod.** Reproduced 2026-07-30
  with the DLL removed: an NPC ship spooling its grav drive during normal
  flight can drop scanner mode in vanilla (alert events pull the player out,
  as combat does). The code audit that preceded the A/B is in git history;
  the stale-`CruiseModeHUDActive` concern it raised lives on as its own open
  item (the panel outliving a forced cruise exit).
- **The ESM groups records by TYPE, not by system.** A "current system only"
  parse still inflates all ~1765 PNDT records to read each one's GNAM, so the
  parse is scoped in TIME — once per launch, background thread, **423 ms
  measured** — never in space. v0.17.0 dropped the on-disk cache entirely on
  this basis; a runtime-generated file is invisible to mod managers and
  outlives an uninstall.
- **Settlement keyword membership is the UNION across every version of a
  location record.** Overrides replace records wholesale, and this layout
  drops `LocTypeSettlement` from overriding masters (xEdit flags it yellow) —
  reading only the winning version loses the marking. The id fields
  (`XNAM`/`YNAM`) take the last version that STATES one instead: blank there
  means "not said here". Stated precedence if both ever apply: settled beats
  giant.
- **Body class is the PNDT record's `KWDA` resolved against `KYWD`**
  (`PlanetType00Asteroid` … `PlanetType07Rock`, exactly eight), not anything
  in `BGSPlanet::PlanetData`.
- **Test a candidate key WITH A TARGET LOCKED, not in an empty sky.**
  `XButton` (R) was the confirm key for six versions and passed every test —
  because it opens the planet map only once a target is *selected*, the exact
  state the panel exists to produce. The collision was with the mod's own
  happy path and only showed in ordinary play.
- **A debug flag must never gate anything the mod needs to work.** v0.2.0
  shipped inert: packaging flipped the recon defaults off, and both the input
  tap and the movie-created callback sat behind them. When promoting recon
  code to infrastructure, move it out from behind its flag in the same
  commit — and run the release checklist's `GetValue()` grep before every
  packaging.
- **Per-notch VM work on a live list broke wheel scrolling once** (v0.9.1,
  never root-caused; deleting the machinery in v0.9.2 fixed it). If a feature
  ever adds per-notch Scaleform work again, watch the wheel first.
- **Feed names are NOT unique, and clip identity must survive that.** Any
  two unresolved contacts in a system ride the feed as "Sensor Contact"
  (proven: the same clip name kept twice in one tick, two distinct FF
  ids), while their DISPLAYED labels can differ (masking, marker text).
  Name-keyed clip matching therefore needs geometry when — and only
  when — the selection's name is shared by 2+ feed entries: a ring
  blip's ROOT `rotation` is exactly `angleToCrosshair + 180`
  (OffScreenIcon.as:163; 15° tolerance), and an on-screen icon sits AT
  `ConvertScreenPercentsToLocalPoint(screenPositionX, screenPositionY,
  container)` — y percentage runs BOTTOM-UP, the converter flips it, and
  −1 is the "unprojectable" sentinel (= off-screen). Confirmed fixed in
  v0.18.2 on a reproducible save.
- **In an ambiguity, a fallback must never take the OPTIMISTIC branch.**
  v0.18.1's icon lookup fell back to first-match exactly when the
  selection's screen position was the −1 sentinel — which IS the
  off-screen case — so the wrong contact's icon vouched for coverage and
  the selection went unmarked entirely. Degrade toward the VISIBLE
  failure (blip and icon both showing, vanilla's own stock station
  look), never the silent one (an unmarked selection): every
  no-confirmation road answers "no icon". Same family as the overlap
  pass's skip-paths lesson: an exemption from a write is also an
  exemption from the restore.
- **`bMarkerDiscovered` and `uLocationMarkerState` can DISAGREE, and the
  STATE is the naming authority.** A runtime-spawned encounter ("Ecliptic
  Satellite", tester 2026-07-30) arrives `LMS_FULL_REVEAL` — the HUD names
  it from the first frame — while `bMarkerDiscovered` stays false; masking
  on the flag printed "Unknown" beside a named marker. Vanilla's recipe
  (`POIIcon.TryUpdateName` → `DynamicPoiIcon.GetLocationPOIName(name,
  uLocationMarkerState, uPoiCategory)`): FULL_REVEAL → name;
  ONLY_TYPE_KNOWN → the category's generic word, **falling back to the
  REAL NAME when no generic exists** (categories NONE=0 and SIMPLE=9 have
  none — not to a placeholder); LMS_UNKNOWN → "$Unknown Location".
  v0.18.0 adopts it verbatim for row labels AND feeds the real state to
  the row badge and faux marker (`SetLocation`), instead of synthesizing
  from the flag. The flag remains only the fallback reading for entries
  without the state field. (The starmap's nameplates use the flag —
  `GetSpacePOIName` — so the two vanilla surfaces genuinely differ; the
  panel mirrors the HUD, its own surface.) **Confirmed in game
  2026-07-31: several random POI spawns across normal play, every row
  wearing its proper name.**
