# PHASE 9 — Reading the binary for the grav jump and the hard lock

**Status (2026-08-13): TOOLING BUILT AND VALIDATED, FIRST FINDINGS IN.** This is the
first time this project has looked inside `Starfield.exe`. Authorised by the user
after every UI-layer and Papyrus route was exhausted (PHASE 8 §3e).

⚠ **This crosses the architecture line the README states** — "no offset found by
hand". That rule protected a *shipped* mod from stale-layout crashes. This is a
local build, so the rule is the user's to relax, and they have. Nothing here has
been put in the DLL: it is all offline analysis of a file on disk.

---

## 1. The tooling, and why it can be trusted

Three scripts in the session scratchpad, all reading `Starfield.exe` as a file —
nothing is injected, hooked, or run:

| script | job |
|---|---|
| `addrlib.py` | parses `versionlib-1-16-244-0.bin` → id → RVA → file offset |
| `vt.py` | dumps a vtable's entries from an Address Library vtable id |
| `fndis.py` / `peek.py` | disassembles (capstone) and reads strings/qwords |

The Address Library parser is written **straight from
`commonlib-shared/src/REL/IDDB.cpp`**, not guessed: format 5 is a flat `uint32`
array indexed directly by id, starting at byte 96 (`sizeof(HEADER_V5)`), and the
file self-reports `game=1.16.244.0 name='Starfield.exe' ptr=8 entries=1274968`.

⭐ **It is validated against an independent measurement.** TODO recorded, from a
live in-game probe, that `ShipHudDataModel`'s `ProcessEvent` is address library id
**89321** at **`0x14155D3E0`**. Feeding 89321 to this resolver returns exactly
`0x14155D3E0`. Two independent routes to the same address is what makes every
number below worth reading.

---

## 2. ⭐ The grav jump input path, read from the binary

`PlayerControls::GravJumpHandler` — vtable id **433573**, at `0x144C41CE8`.

Its vtable matches `BSInputEventUser`'s published layout exactly: slots 2–7 are all
the same shared stub (`0x1402B56D0`), and the overridden ones are 0, 1, **8**, 9,
10, 11. **Slot 8 is `OnButtonEvent`**, at **`0x1412BBAF0`**.

### What OnButtonEvent does

It resolves three `BSFixedString` statics and compares each against the event's own
user event (a virtual at event-vtable `+0x10`, i.e. `QUserEvent`). The literals,
read out of `.rdata` via the magic-static init paths:

```
0x144AF7380 = 'GravJump'
0x144B03518 = 'Accept'
0x144B1388C = 'Cancel'
```

⭐⭐ **So the grav jump's user event is literally named `GravJump`** — and it did
**not** appear anywhere in the cruise key survey, which saw `Accept`, `Cancel`,
`XButton`, `SelectTarget`, `LockCourse` and others. **In cruise, X reports
`XButton`.** That is the binary confirming what the survey only hinted: the jump
binding is not live in cruise, which is why holding X after tracking did nothing.

It then gates on the event's own fields at the offsets CommonLibSF declares —
`[rdi+0x48]` = `value`, `[rdi+0x4c]` = `heldDownSecs` — requiring `value == 0` (a
RELEASE) on the cancel path.

### The two calls, and what they are NOT

Both branches act on one global singleton pointer at **`0x1461DE668`**:

| branch | call | meaning |
|---|---|---|
| press | `0x1422F2480(this, 2, ptr)` | sets `[handler+0x50] = 1`, then pushes |
| release | `0x1422F2630(this, 2, ptr)` | clears `[handler+0x50]`, then pops |

⚠ **Neither is the jump.** `0x1422F2480` manipulates a counted array at
`[this+0x2a8]` / `[this+0x2b0]`, checks a tag byte against `0x11`, takes a
`lock xadd` refcount and pushes an entry tagged with the `2` passed in dl. That is
a **hold-prompt / activation stack**, not a translation. The press starts a hold and
the release cancels it; **the jump fires when the hold COMPLETES, downstream.**

That is a finding worth stating plainly because it kills the obvious plan: calling
the handler, or replaying its input, only ever starts and cancels a prompt.

---

## 3. Where the jump actually completes — the next thread to pull

RTTI names the completion path, and it is a **named factory handler**, not a plain
function:

- `GravJumpInitiateCompleteHandler` — RTTI **861515**
- `AutoRegisterCreator_GravJumpInitiateCompleteHandler_BSTCreateFactoryManager_BSFixedString_IHandlerFunctor_Actor_BSFixedString__256__`
  — vtable **453857**

The template arguments are the whole story: it is an
**`IHandlerFunctor<Actor, BSFixedString>` registered in a factory keyed by
`BSFixedString`**. So the engine looks this handler up **by name** and calls it with
an `Actor`. If that factory can be reached and the name is recoverable, "make the
player grav jump" becomes a lookup and a call with the player — no input, no event,
no UI.

Also unexplored and named:
- `StarMap::Util::ConfirmGravJumpPlotCallback` — vtable **446165** at `0x144C943B0`.
  The name says the star map *plots* a jump and *confirms* it through this callback,
  which is the destination-setting half.
- `Spaceship::GravJumpEvent::GetEventSource` — **93876, and it is LIVE**, not a
  `{0}` placeholder like most `GetEventSource` ids in this SDK. It cannot start a
  jump, but it is a working ORACLE: sink it and any attempt reports success or
  failure immediately instead of being inferred from what the ship does.

---

## 3a. ⭐⭐ THE DESTINATION HALF — `StarMap::Util::ConfirmGravJumpPlotCallback`

Found by xref: two RIP-relative references to its vtable (`0x144C943B0`), both
inside large star-map functions (`0x14168BD80..0x14168C713` and
`0x1416BA600..0x1416BAE89`, bounds from `.pdata`). The construction site reads:

```
mov  ecx, 0x18          ; the object is 0x18 BYTES
call <allocator>
mov  [r9+8],  r15d
lea  rcx, [rip+...]     ; the vtable
mov  [r9],    rcx
mov  eax, [rsi+0xc90]   ; <- from the star map object
mov  [r9+0x10], eax
mov  eax, [rbp+0xa0]
mov  [r9+0x14], eax
...
lea  rdx, [rip+...]     ; '$TRAVEL'  <- the confirm prompt
```

So the star map builds a **"TRAVEL?" confirmation** carrying a callback with two
dwords. The callback's vtable has only **two entries** (slot 2 already lands in
`.rdata`): a destructor and the Call at `0x1416D3CF0`, which is 15 bytes of
prologue jumping into the body:

```
test dl, dl            ; dl != 0 -> return; so dl == 0 means CONFIRMED
cmp  dword [this+0x10], 0
jne  <pair path>       ; builds {+0x14, +0x10} on the stack and calls 0x140C83790
<zero path>            ; passes only the one id, tail-jumps 0x1425557E0
```

⭐ **So the destination is the pair of dwords at `+0x10` and `+0x14`**, one of which
may be zero — which reads exactly like *(body, system)* with "system only" as the
zero case. `r8d = 7` is passed as a constant on both paths.

**Why this matters:** the object is **0x18 bytes, ONE vtable, three dwords**. That
is enormously simpler than the `ButtonEvent` this project refused to fabricate -
no multiple inheritance, no refcount, no `BSFixedString`. Constructing one and
calling slot 1 with `dl = 0` is a plausible "plot and jump to X".

⚠ **UNKNOWN, and it is the whole thing: what the two dwords ARE.** One comes from
the star map object at `+0xc90`. Until that is identified they cannot be filled in,
and a wrong pair is a jump to somewhere unintended.

⚠ **And it would still be a hand-built engine object** - the hazard class this
project exists to avoid. The vtable comes from an Address Library id (vtable ids
have no placeholders, unlike the 505 function ids), which makes it survivable
across patches, but the FIELD LAYOUT is read off one build's disassembly and
nothing checks it at runtime.

---

## 3b. ❌ POWER IS NOT THE DIFFERENCE — a theory, measured and dropped

The first working jump charged for 26 s where vanilla's takes 10, and the drive was
reading one pip (`SpaceshipGravJumpCurrentPower = 0.111`). The obvious guess was
that vanilla's initiate action assigns full power and we, entering downstream of it,
get none. Wrong on both halves, and the 2026-08-13 17:42 log says so twice:

```
17:42:58.644  we asked  CurrentPower 0.000 -> 1.000
17:43:15.613  engine    CurrentPower 1.000 -> 0.111    put straight back

17:44:29.256  vanilla hold-X: Initiated 0 -> 1, power sitting at 0.111
17:44:38.928                  Initiated 1 -> 0         9.67 s AT ONE PIP
```

Two conclusions, both worth keeping:

- **Vanilla does not run at full power.** It jumps in 9.67 s with the same 0.111 our
  slow jump had, so power is not what made ours slow.
- **The AV is a readback, not a control.** Writing it desyncs the number for ~17 s
  and then the engine recomputes over it; the drive stays unpowered underneath,
  which is why the player still had to assign power by hand.

Left in behind `bMissionJumpPower`, **default off**. Do not switch it on expecting a
fix - it writes a false number into ship state and changes nothing else.

⚠ So the real gap between our jump and vanilla's is **only** the destination. Power
was a distraction.

## 3c. Plot capture — installed, verified in place, NOT YET EXERCISED

`TryInstallPlotCapture` hooks slot 1 of the callback's class vtable and chains to the
original. The install is confirmed correct by an independent check: the logged live
addresses rebase exactly onto the static ones.

```
live vtable  00007FF607D443B0   static 0x144C943B0
live call    00007FF604783CF0   static 0x1416D3CF0   (same module base, both)
```

⚠ **It has not fired yet, and that is not evidence of anything.** The 17:42 session
did a panel jump and a vanilla hold-X mission jump; neither opens the star map, and
this callback belongs to the star map's "$TRAVEL" confirmation box. The test it needs
is: open the star map, pick a system, confirm.

⭐ Worth noting for when it does fire: the mission tab already resolves a
**(system, planet-index)** pair per mission - `Volii Alpha (system 64720, planet 2)`,
`Jemison (system 71456, planet 3)`. If the callback's two dwords are that pair rather
than form ids, every value needed is already in hand. The log names each dword as a
form when it can, so which scheme it is will be obvious on the first capture.

⚠ Separately: the hold-X mission jump did **not** pass through this callback. If the
star map test shows the hook works and hold-X still misses it, the mission-jump path
plots by some other route and that route is the one to chase.

## 3d. FOUR THEORIES ABOUT THE DESTINATION, ALL KILLED BY MEASUREMENT

Slot 1 of `GravJumpInitiateCompleteHandler` is confirmed: a vanilla hold-X mission
jump runs it (`slot 1 fired - actor 00000014 (THE PLAYER)`), and calling it ourselves
produces the identical signature - handler call, `Initiated 0 -> 1` about 70 ms later,
jump 9.6 s after that. **Executing a jump is solved.** Every failure since has been
the destination, and four separate explanations died:

| # | theory | how it died |
|---|---|---|
| 1 | full grav-drive power is missing | vanilla jumps at 0.111 (one pip) in 9.67 s; the AV is a readback and the engine overwrites it |
| 2 | the destination is in the object slot 1 reads | two jumps to two different systems (Volii, Mars/Sol) dumped it **byte for byte identical**; `+0x28` is the SHIP (`Frontier_ModularREF`) |
| 3 | `StarMap::Util::ConfirmGravJumpPlotCallback` sets it | hook installed and address-verified; **never fired** across two real star map jumps |
| 4 | it is derived from the tracked quest | a 4 s delay between tracking and firing changed nothing |

And a fifth, on the plot setter itself: `0x140C83790` (id 67119) has exactly three
callers; two were patched at their call sites and watched across a hold-X jump that
**did** travel. It never fired. **Plotting a jump is not a call into that function** -
it is star-map-only.

⚠ The missing animation is not a separate problem. With no destination there is
nothing to travel to, so the engine runs the actor-value cycle and skips the sequence.
Fix the destination and the animation returns.

## 3e. ⭐⭐ THE EVENT THAT WAS NEVER TRIED — `ShipHud_JumpToQuestMarker`

`SelectTarget` appears **once** in the whole binary, inside the control-map name table
(`RightStick`, `Scan`, `SelectTarget`, `Snap`), with no BSFixedString static and no
handler comparing against it - unlike `GravJump`, which has its own. So the lock is
**not a native input handler**, and every hour spent in `PlayerControls` was spent in
the wrong layer.

It is a UI event. And the engine's registered UI event vocabulary contains, by name:

```
ShipHud_JumpToQuestMarker      <- this is "X Mission"
ShipHud_Target                 <- very likely the active lock
ShipHud_SetTargetMode
ShipHud_Deselect
ShipHud_AbortJump
```

⚠ **This mod has never dispatched `ShipHud_JumpToQuestMarker`.** In its entire history
it has sent exactly two UI events: `Reticle_OnCruiseLockCourse` and `ShipHud_FarTravel`.
PHASE 8 closed "the UI event vocabulary" as a dead route after `ShipHud_FarTravel`
turned out to be fast travel and did nothing - a conclusion drawn from one wrong verb
without ever enumerating the list.

Why it fits every measurement above: it needs no lock, no selection and no star map;
it names the exact action the vanilla prompt offers; and if the engine runs its own
mission-jump path then the destination AND the animation come with it, because that is
the path hold-X uses.

⭐ And it is a call this mod already knows how to make - `DispatchHudEvent` works today.

Unknown: its parameters. `Reticle_OnCruiseLockCourse` takes `{uBodyID}`. This one may
take a quest id, a marker id, or nothing at all if it reads the tracked quest. Dispatch
with no arguments first and let the engine's answer say.

## 3f. ✅ SOLVED — `ShipHud_Target` then `ShipHud_JumpToQuestMarker`

Working as of 2026-08-13 18:51. Two jumps, both correct, both with the vanilla
animation:

```
18:51:34.065  sent ShipHud_Target with bValue=true
18:51:34.066  dispatched ShipHud_JumpToQuestMarker
18:51:37.997  Initiated 0 -> 1          <- the engine's own sequence, not ours
```

**Two dispatches. No hooks, no constructed objects, no hand-carried offsets.** The
same `DispatchHudEvent` the mod has always had.

Why it took so long is worth writing down, because the mistake was structural rather
than technical: PHASE 8 declared "the UI event vocabulary" a dead route after trying
ONE wrong verb (`ShipHud_FarTravel` - fast travel), and PHASE 9 inherited that
conclusion and went into the binary instead. The event list was never enumerated. When
it finally was, the answer was sitting in it under its own obvious name.

The intermediate step mattered too. The bare jump event dispatched cleanly and the
engine ignored it - which is not "wrong verb", it is "right verb, nothing selected".
The `.rdata` string table settles the selection: `bValue` sits immediately beside
`ShipHud_Target`, so it is a BOOL, not an id - it turns targeting on for whatever the
reticle is hovering, exactly as vanilla does when the player presses A.

⚠ **The 4 s delay is gone** (`uMissionJumpDelayMs` now 0). It was added for theory 4
in §3d and disproved by its own test. Tracking happens on the CONFIRM press, which is
a separate keypress well before the jump.

⚠ **What is still owed by the binary work: nothing.** Everything in §2, §3, §3a-§3e is
now diagnostic history. The shipped path touches none of it. The capture hooks
(`bCaptureJumpHandler`, `bCapturePlotSetter`, `bCapturePlotConfirm`) and the executor
routes (`bMissionJumpViaHandler`, the actor value) remain only as fallbacks and
evidence, and should be considered for removal once this route has flown a while.

## 3g. WHERE IT ACTUALLY LANDED — working, but aim-gated

The mission jump works. Verified sequence, twice:

```
sent ShipHud_Target          the A-press
info target is now 'Volii'   ~25 ms later, off the feed
dispatched ShipHud_JumpToQuestMarker
GravJumpCurrentPower 0 -> 0.111   the engine arms and jumps
```

⭐ **The only condition is that the destination is what the reticle selects.** Every
other candidate cause was measured and eliminated: `bHasQuestTarget` (never true on
this feed), the course lock (`courseLocked=no` on a successful jump), quest tracking
timing, distance caps, and the target-lock angle settings.

### Why it cannot currently be aimed by the mod

Three verbs are reachable, and none combines "by id" with "selection":

| verb | takes | sets |
|---|---|---|
| `Reticle_OnCruiseLockCourse` | `uBodyID` | the autopilot COURSE (`bIsCruiseTargetLock`) |
| `ShipHud_Target` | `bValue` (a BOOL) | the SELECTION - but of whatever the reticle is on |
| replayed `SelectTarget` | nothing | steps the selection, cannot aim it |

`ShipHud_Target` carries no id at all. That single fact is the whole limitation.

### Closed this round

- **`SetInfoTarget` by id** - not in the named surface. No targeting-manager class; both
  `BSTEventSink_ShipHud_Target_` and `BSTEventSink_ShipHud_JumpToQuestMarker_` exist in
  RTTI but have **no vtable id**, so neither concrete handler is reachable by name.
- **`iInfoTargetIndex`** - published from `<data model>+0xC0`, a MIRROR rewritten every
  update. Writing it changes one frame of UI and nothing in the engine.
- **The angle settings** - `fTargetLockTargetAngle` (default **30.0**, read from the exe;
  it is in no ini file) and friends sit among `fCombatTargetSelector*` and aim-assist
  values. Combat lock, almost certainly not cruise marker selection. An ini edit changed
  nothing observable.
- **`StarMapMenu_FocusSystem`** - found by sweeping the interface archive for payloads
  carrying `uBodyID`; a genuine by-id verb, and it DISPATCHES cleanly from the ship HUD
  movie. ✅ **Retested with a real system id and it is a clean negative.** The mission
  row now carries `GalaxyData::systemID` end to end, and the event went out as
  `uBodyID=0005E3A5 uSystemID=64720` - the correct Volii system. Nothing moved: not the
  info target, not the course. The only selection change in the window was the A-press
  5 ms later picking up Neptune, which is what the reticle was near.

  **Conclusion: the star map verbs are MAP-SCOPED.** They reach BSUIDataManager from any
  movie, but nothing acts on them unless that menu is open. `StarMapMenu_ExecuteRoute`
  inherits the same verdict and was never worth enabling.

  ⭐ The system-id plumbing is kept regardless: quest state → mission row → panel row,
  which is the first time the panel has known a destination SYSTEM rather than a body.

### Still open

- The pending jump request has no deadline: if selection never lands it survives and can
  fire late, when the player happens to aim near something.

## 4. What this does and does not change

- ✅ The `GravJump` user event name is now known for certain, and so is the fact
  that it is absent in cruise.
- ✅ The input handler is fully mapped, and ruled out as the trigger.
- ✅ There is a validated id → address pipeline, so any further id in this SDK can
  be turned into code to read.
- ❌ No callable "do a grav jump" function has been found yet. The completion
  handler is the live lead and it is reached by NAME through a factory.

⚠ **Nothing here should go into the DLL yet.** Every address above is a
**1.16.244-specific** file offset; the moment any of it is used at runtime it must
go through `REL::ID`, not a literal, or the next game patch turns it into a crash.

## 3h. The SFSE trigger surface (2026-08-14) — NAMED, and never searched

The whole session hunted the Scaleform event layer and the anonymous sinks behind
it. A plain string scan of the exe turns up a *named* grav jump surface that was
never looked at. This is the live lead.

| symbol | id / note |
|---|---|
| `PlayerControls::GravJumpHandler` | **vtable id 433573, va 0x144C41CE8** — the hold-X input handler |
| `GravJumpInitiateCompleteHandler` | `IHandlerFunctor<Actor, BSFixedString>`, registered via `BSTDerivedCreator` (creatable by name) |
| `BSTGlobalEvent::EventSource<Spaceship::GravJumpEvent>` | sinkable — observe instead of guess |
| `GravJumpSearch`, `MovementMessageGravJump` | pathing side |

BGSAction default objects — the four stages hold-X actually drives, resolved by
name through the default-object manager:

    GravJumpInitiateAction_DO   GravJumpExecuteAction_DO
    GravJumpFinishAction_DO     GravJumpCancelAction_DO

`GravJumpHandler` vtable slots: 0-13 are mostly shared `PlayerInputHandler`
boilerplate at 0x1402B56D0. The class-specific ones are slot 8 (0x1412BBAF0),
slot 10 (**0x1412BD650**, outside the 0x1412BBxxx cluster - most likely the real
override) and slot 11 (0x1402B75F0).

### Papyrus is NOT the route (checked, not assumed)
This plugin already has working VM dispatch (Phase 6, `Planet.GetSurveyPercent`),
so Papyrus was the obvious guess. The `SpaceshipReference` native cluster at
0x4D12900 contains no jump-to-target function:
- `GetGravJumpRange` - read-only
- `EnableWithGravJump` / `DisableWithGravJump` (+`NoWait`) - VFX helpers for
  popping refs in and out, nothing to do with the player jumping
Do not re-try Papyrus for this.

### What this does and does not buy
It buys a **trigger**, and the trigger was never the broken part -
`ShipHud_JumpToQuestMarker` already fires a real jump. `GravJumpInitiateAction_DO`
will most likely read whatever is already plotted, i.e. the same aim-gating.

The actual value: `GravJumpHandler` is now **inspectable**. Hook it during a
working hold-X and watch where it reads the destination from. That read is the
one thing this phase has never obtained, and it is what would close the
destination problem for good.

NEXT: hook id 433573 slot 10, log arguments during a real hold-X mission jump.

## 3i. THE ID BUG - and what it invalidates (2026-08-14)

`RE::VTABLE::...` resolved to addresses that are **not vtables in this build**.
CommonLibSF's IDs_VTABLE.h is generated against a different game version than the
versionlib-1-16-244 bin loaded at runtime. Verified by reading the RTTI
complete-object-locator at `vtable-8`:

| class | CommonLibSF | this exe (RTTI-verified) |
|---|---|---|
| `StarMap::Util::ConfirmGravJumpPlotCallback` | 417674 → no locator | **446165** |
| `GravJumpInitiateCompleteHandler` | 424879 → no locator | **453901** |
| `PlayerControls::GravJumpHandler` | 407270 → no locator | **433573** |

`ResolveVerifiedVTable()` now refuses any id whose RTTI name does not match, so
this fails loudly at install instead of silently writing into unrelated .rdata.

### What this invalidates
§3c/§3d closed the plot-callback route because "the hook never fired across two
real map jumps". **That measured this bug, not the engine** - the hook was never
on the function. Any conclusion resting on a hook that did not fire is void.
Same for the `GravJumpInitiateCompleteHandler` capture, described as
"installed/unexercised": it was installed on garbage.

## 3j. SOLVED: slot 1 IS the trigger

With the correct id, the first ever real capture (21:17:41):

    [jumphandler] slot 1 (initiate complete) fired - actor 00000014 (THE PLAYER)
    [gravjump] SpaceshipGravJumpInitiated  : 0.000 -> 1.000
    [gravjump] SpaceshipGravJumpCalculation: 0.000 -> 0.010   (then ramps)

`GravJumpInitiateCompleteHandler::Call` slot 1 = `0x141AE89F0`, signature
`(this, Actor*)`. Disassembly confirms **`rcx` is never read** - it does
`mov r8, rdx` and works only off the Actor - so `TriggerGravJumpViaHandler()`
passing a stack placeholder as `this` is legitimate, not luck.

    mov  eax,[rdx+0x37c]; shr eax,4; test al,1   ; gated on an ACTOR FLAG BIT.
                                                 ; clear -> returns 1, does nothing
    call 0x142116840 (actor, 1)      -> rbx      ; the player's ship object
    mov  ecx,[rbx+0x28]                          ; ship formID (Frontier_ModularREF)
    call 0x1423ff640                 -> rax      ; a singleton
    mov  rcx,[rax+0x8b0]                         ; its jump subsystem
    call 0x14214de90 (subsys,&out,&shipID)       ; THE ACTUAL JUMP CALL

**No destination is an argument anywhere in this chain.** It lives inside the
subsystem at `singleton(0x1423ff640) + 0x8b0`. That object is the next read, and
it is now reachable by a named path.

Note the actor flag gate at `[actor+0x37c] bit 4`: if clear, slot 1 silently
returns success and does nothing. A "no jump" result is not proof the call failed.

## 3k. PlayerControls::GravJumpHandler is NOT the mission hold - WRONG GUESS
§3h called it "the hold-X handler" off the RTTI name. The log says otherwise:
through a **successful** hold-X mission jump, slot 8 (ProcessButton) never fired
once, and slot 10 fired 1871 times with all-zero state. It is the star map /
general jump hold. Capture turned off (`bCaptureGravJumpInput=false`).

NEXT: the panel RB path (`bMissionJumpViaHandler`, on by default) calls slot 1 and
has NEVER run against the correct address. That is the immediate test.

## 3l. THE TRIGGER IS SOLVED. The destination is the whole remaining problem.

Route 2 (`TriggerGravJumpViaHandler` -> the real slot 1) run for the first time
against the CORRECT vtable, side by side with a vanilla hold-X in the same
session (2026-08-14):

    OUR CALL                            VANILLA HOLD-X
    gate[0x37C bit4] = SET               gate[0x37C bit4] = SET
    52.689 Initiated   0 -> 1            39.583 CurrentPower 0 -> 0.111
    55.310 Calculation 0 -> 0.009        43.451 Initiated    0 -> 1
    55.310 CurrentPower 0 -> 0.111       43.451 Calculation  0 -> 0.002
  27:04.932 Initiated  1 -> 0          27:53.132 Initiated   1 -> 0
    => 9.6 s of calculation              => 9.68 s of calculation
    => went NOWHERE                      => jumped

**The call is not refused and does not fail.** It sets the same flag vanilla
sets and runs a full, normal-length calculation cycle. The ship goes nowhere
because the destination is empty. Exactly the §3j prediction.

The other half of the diff: vanilla had `info target is now 'Sol'` at 21:27:38,
before the hold. Our run had no info target. **The selection is what plots the
destination** - which is the same aim-gating wall, now precisely located.

| piece | status |
|---|---|
| trigger | **SOLVED** - slot 1, callable, no destination arg, runs a full cycle |
| destination | the ONLY unknown - set by the selection, lives in the subsystem |

### Ordering note (not yet tested as a cause)
Vanilla powers the grav drive BEFORE initiating (power at 39.583, initiate at
43.451). Ours initiated first and power came up 2.6 s later. Both still ran a
full cycle, so this is not obviously the blocker - but it is an untested
difference and should not be assumed harmless.

### The next read - now instrumented
`DumpJumpSubsystem()` dumps `singleton(id 126578) + 0x8B0`, the object slot 1
hands to the jump call (id 120359), at both sites. The point is the DIFF:
a field populated on the vanilla hold-X and zero on the panel route is the
destination. Non-zero words are resolved through `TESForm::LookupByID` and named.

Reverse-looked-up ids, all patch-safe:
    0x1423ff640 singleton getter         id 126578
    0x14214de90 the jump call            id 120359   (subsys, &out, &shipID)
    0x142116840 get player's ship object id 119881   (already in use)

## 3m. THE DESTINATION, FOUND. It is a route array of {star, planet} form ids.

Slot 1's real shape (0x141AE89F0), all ids reverse-looked-up and patch-safe:

    mov  ecx,[ship+0x28]      ; ship form id (Frontier_ModularREF, 0003F7E8)
    call 0x1423ff640          ; singleton                       id 126578
    mov  rcx,[rax+0x8b0]      ; jump subsystem
    call 0x14214de90          ; lookup(subsys,&out,UNUSED,&shipID)  id 120359
    mov  rdx,[rsp+0x20]
    test rdx,rdx / je <skip>  ; NULL -> DO NOTHING (our empty jump)
    mov  rcx,rbx
    call 0x14210ea50          ; DoJump(ship, payload)           id 119843

`out` is NOT a string. The 0x20 header + refcount is BSFixedString-SHAPED but it is
a **BSTArray header**: `{uint32 size, uint32 capacity, T* data}`.

Measured on a working vanilla hold-X (2026-08-14), size 2 capacity 4:

    data +0x00 = 0005E605  'OlympusStar'       entry 0 = ORIGIN
    data +0x04 = 0005E294  'NesoiPlanetData'
    data +0x08 = 0005E5CB  'SolStar'           entry 1 = DESTINATION
    data +0x0C = 0005DECE  'TritonPlanetData'

Element stride is **8 bytes**: `{uint32 starFormID, uint32 planetDataFormID}`.
size 2 * 8 = 0x10, so the array is exactly +0x00..+0x0F; the `cTypeShip.ture`
ASCII at +0x10 is unrelated heap and must not be read as data.

Sol/Triton is precisely where the hold-X mission jumps have been landing, which
independently confirms entry[last] is the destination. Capacity 4 implies the
engine supports multi-hop routes.

### THE SPOOF (what the user asked for - "just insert location")
Build our own header + 8-byte pair array and call `DoJump(ship, &header)`
(id 119843) directly, OR hook the lookup (id 120359) and substitute the header.
Either way the destination is two form ids we can resolve, not opaque state.

The mod already parses all 1765 PNDT records and carries systemID per body, so
the planetData id is in hand. The remaining piece is the **star form id per
system** (formType BF) - not yet collected by the body table.

### Two wrong reads on the way here, both caught by the log, both recorded so
### they are not repeated:
1. Declared the lookup with THREE parameters. It takes FOUR and reads the ship id
   from **r9**. Three args put it in r8, the callee hashed garbage, the lookup
   missed, and it logged "NO DESTINATION STRING" on a hold-X that jumped fine.
2. Read the payload as `char*`. Printed '' and looked like an empty destination.
   It was a BSTArray header whose first byte is the low byte of `size`.
Also: `LookupByID` answers for tiny ids - the array's size(2)/capacity(4) resolved
as 'TravelMarker' and '-'. Ignore form ids below 0x800.

## 3r. SOLVED (the question, not the feature): WHAT PLOTS THE ROUTE

Measured 2026-08-14 with a 2 Hz route watcher, one vanilla hold-X:

    09:16:29.998  mission 'Absolute Power' TRACKED -> Volii Alpha   route still EMPTY
    09:16:37.213  [acquire] info target is now 'Volii' (0005E614)   the SELECTION
    09:16:37.645  key 'XButton'                                     hold begins
    09:16:38.685  GravJumpCurrentPower 0 -> 0.444
    09:16:38.799  [route] CHANGED -> size 2 capacity 4              PLOTTED HERE
                    [0] OlympusStar / NesoiPlanetData
                    [1] VoliiStar   / VoliiAlphaPlanetData
    09:16:42.486  slot 1 fires                                      hold completes

**Tracking does not plot.** The quest was tracked 8 s earlier and the route stayed
empty. The plot happens at the hold-X KEYPRESS, and the destination is composed:
  system  <- the current INFO TARGET (VoliiStar, what the reticle selected)
  body    <- the TRACKED mission's target (VoliiAlphaPlanetData)

### Why every spoof was declined
A filled array is not a plotted route. The route pointer is interior to a larger
object (two exe vtables in the 32 bytes ahead of it) which carries the rest of the
plot state. Writing the 16-byte header - even INSIDE the engine's own object, with
correct ids, verified by the DoJump capture - leaves that object unplotted, and
DoJump declines. Confirmed A/B:

    engine's EMPTY route  -> Initiated flips, full 9.6 s calculation, goes nowhere
    our FILLED route      -> DoJump does nothing at all

### Dead ends closed here (do not retry)
- Calling DoJump (id 119843) directly with a synthetic BSTArray. Arguments were
  proven byte-correct against the engine's own call; it is the OBJECT that is
  rejected.
- Filling the engine's header in place. Same result.
- Range. Sol -> Volii is the standing test case and vanilla hold-X does it.
- Lifetime/async. The destination is read at call time, not after the calculation.

### Where this leaves the feature
`bMissionJumpSpoof=false` in the ini: with it on, RB returns "handled" and never
falls through to the aim-gated route that works, so it is strictly worse. The
machinery stays in the DLL behind the flag.

NEXT, and now well-posed: find what runs between the XButton press (09:16:37.645)
and the route appearing (09:16:38.799). That is the PLOT function. Calling it with
a chosen system id is the whole feature - and unlike every route we synthesised, it
would leave the engine's own object in the plotted state it requires.

## 3s. CORRECTION to §3r: THE SPOOF WORKS. Volii specifically does not.

§3r concluded the spoof was declined outright. That is WRONG. Per the user, many RB
presses jumped correctly in the same period - Volii is the one destination that
fails. The route watcher log used for §3r contained **no RB presses at all** (one
[spoof] line, the hook install, and zero `mission RB ->` attempts), so it was never
evidence about the spoof either way. Logs are overwritten per session and the
working run is gone, so this is recorded on the user's report.

What survives from §3r is only the plotting MEASUREMENT, which stands on its own
timestamps: tracking does not plot, the hold-X keypress does, and the destination is
composed of the info target's system + the tracked mission's body.

What does NOT survive: "a filled route is always declined". It is accepted often
enough to jump.

### Standing hypothesis for Volii - distance, not mechanism
The spoof always builds a ONE-HOP route: [0] where we are, [1] where we are going.
The engine's own array has **capacity 4**, so vanilla can plot multi-hop.
- Volii is the farthest star on the feed: the log's own fallback line measures it at
  2.37e17 m.
- The vanilla Volii jump captured on 2026-08-14 had origin **OlympusStar**, NOT Sol.
  That is a different, shorter leg than the Sol -> Volii the spoof was attempting.
So a single hop beyond max jump distance being refused fits every observation,
including that vanilla can reach Volii (it plots the legs) while our one-hop cannot.

NEXT: test the hypothesis before building for it - RB on a mission whose target is a
FAR system other than Volii. If far targets fail as a class and near ones work, it is
distance, and the fix is multi-hop route construction (capacity 4 is the hint).

## 3t. THE RANGE, from the engine - and routing that reaches. WORKING.

### How the engine decides "out of range"
Found by xref from the Papyrus registration string "GetGravJumpRange" (0x144D13528,
referenced once at 0x141F24B16; the paired `lea r9` gives the native). The native
0x141F222C0 is a three-line wrapper:

    mov    rcx,[r8]                ; the ship reference
    xor    edx,edx
    call   0x142110B80             ; THE REAL RANGE FN     id 119854
    vmulss xmm0,xmm0,[0x144F048F8] ; * 3.2615560
    ret

**3.2615560 is parsecs -> light years.** So Papyrus reports light years for the UI
and the underlying function returns **PARSECS** - the same unit as STDT's BNAM star
positions. Nothing to convert, and no constant to guess.

Called as `float f(TESObjectREFR* ship, 0)`. The ship reference comes from the jump
object's +0x28 form id (the same field slot 1 reads) via LookupByID.

Measured live: **6.04 pc (19.7 ly)**, which lands inside the bracket this phase had
established the hard way - Olympus->Sol at 5.84 works, Sol->Volii at 8.56 does not.

### Routing
`fMaxJumpParsecs` now defaults to **0 = ask the engine**; a number only overrides it
for testing. Legs are chosen by breadth-first search over the 123 systems, edges
where the leg fits the range, taking the first hop of a FEWEST-HOPS path. Fewest
hops, not shortest distance: every hop is a keypress and a jump.

    greedy (first attempt)  Sol->Volii  Wolf 2.39 -> Aranae 5.39 -> Volii 3.91
                            3 presses, 11.69 pc
    BFS     (now)           Volii->Sol  Olympus 3.95 -> Sol 5.84
                            2 presses, minimum

Unreachable destinations are refused with a reason rather than sent as a leg that
goes nowhere useful.

### Why multi-hop is NOT a 4-entry route
The engine's array has capacity 4, which invited exactly that. But the captured
hold-X plot was `size 2` even for a far target: multi-leg routing is a STAR MAP
operation, and from the cockpit the ship jumps ONE LEG PER PRESS. Building a 4-entry
route would have meant inventing an intermediate-entry format never observed.

## 3u. ⭐ WORKING SYSTEM - the whole design in one place

Press RB on a mission row and the ship grav jumps to that mission's destination.
No reticle, no selection, no A-press, any distance. This is the fallback point.

### The chain
1. **Destination ids.** Panel row -> `{star, planet}` form id pair.
   - planet: the row's feed id, checked to be a `kPNDT`
   - star:   STDT parsed from the ESM, `DNAM` = systemID, keyed by the body's
             `galaxy.systemID` (Sol is systemID **0** - never test a system id for
             truthiness)
2. **Range.** id **119854** `float f(TESObjectREFR* ship, 0)` returns **parsecs**.
   (Papyrus `GetGravJumpRange` = this * 3.2615560, i.e. light years, for the UI.)
   Star positions are STDT `BNAM`, three floats, also parsecs. 6.04 pc on the test
   ship.
3. **Routing.** Breadth-first over the 123 systems, edges where a leg fits the
   range, fewest hops. Unreachable -> refuse with a reason.
4. **⭐ Origin = the START OF THE FINAL LEG, not where the ship is.**
   This is the key insight and it came from the user reading the captures:
   at 09:27:50 the ship was at Volii and an 8.56 pc Volii->Sol was refused; at
   09:28:09, **with no jump in between**, the engine plotted
   `[0] OlympusStar/Nesoi [1] SolStar/Triton` and jumped. Entry 0 said Olympus while
   the ship sat at Volii. Hand the engine the LAST leg and it flies the earlier ones
   itself - one press, whole trip. Every captured hold-X route has this shape.
5. **Delivery.** Patch the lookup call inside slot 1 (id **104005** + 0x45), fill
   the ENGINE'S OWN header in place (`data`/`size`/`capacity`), leave `out`
   untouched so its cleanup and refcounting are the engine's. Then run slot 1
   (`GravJumpInitiateCompleteHandler` vtable id **453901**, slot 1), which calls
   DoJump itself with its own ship pointer in its own state.

### Address Library ids (all RTTI- or xref-verified against 1.16.244)
    453901  GravJumpInitiateCompleteHandler vtable   ⚠ NOT RE::VTABLE's 424879
    446165  StarMap::Util::ConfirmGravJumpPlotCallback vtable  ⚠ NOT 417674
    433573  PlayerControls::GravJumpHandler vtable    ⚠ NOT 407270
    104005  slot 1 (the initiate-complete Call); lookup call at +0x45
    120359  the route lookup (4 args, ship id is the 4th, in r9)
    119843  DoJump(ship, route)
    119881  get the player's ship object (+0x28 is its form id)
    119854  grav jump range, in parsecs
    126578  the singleton whose +0x8B0 is the jump subsystem
⚠ `RE::VTABLE::` is generated against a DIFFERENT build and its ids are not
vtables here. `ResolveVerifiedVTable()` refuses any id whose RTTI name mismatches.

### OPEN: the jump animation
The travel animation is MISSING on this path. It appeared only on the old A-press
injection, which was reticle gated. Working theory (the user's): it is tied to
WHERE we inject - the animation likely hangs off the selection/HUD path we now
bypass entirely, not off slot 1. Not yet investigated.

### OPEN: mission menu cleanup
Objective text now takes the NEWEST displayed objective (highest index) rather than
the oldest; deployed, not yet confirmed in game.

## 3v. The animation: two leads checked, both closed

### Default objects - NOT callable
Every grav-jump `*_DO` string has exactly two references and neither consumes it:
the static initializer, and a destructor thunk in a long run of them.

    SpaceshipGravJumpCameraPath_DO  string 0x144B62AF0  holder 0x145EA06F8
      0x1400873DD  static initializer (atexit)
      0x1438C69A7  lea rcx,[holder]; jmp <dtor>

Same for GravJumpBeginMusic_DO and all four GravJump*Action_DO. They are resolved
through the DO manager by index at runtime, so there is nothing statically callable
- the same wall as §3h.

### Routing the spoof through the HUD action - DOES NOT WORK
The animation belongs to `ShipHud_JumpToQuestMarker`, and the route substitution
lives inside slot 1, so dispatching the HUD action with the route armed *should*
have given animation + arbitrary destination. Arming was made a 3 s deadline so it
would survive the event's async dispatch.

Measured: the event dispatches, and **the lookup hook never fires**. The action is
gated BEFORE slot 1 - with no real selection it does nothing at all, so there is
nothing for the route to attach to. Zero GravJumpInitiated transitions in the run.
Arming earlier or longer cannot help; slot 1 is never reached.

`bMissionJumpAnimated` defaults to **false** and is kept only to stop the attempt
being repeated.

### What is left
The animation is staged somewhere between the HUD action and slot 1, on a path that
requires the selection. Finding it means identifying what that action does BEFORE
it reaches the handler - which is the anonymous engine sink from §3e, still the one
piece of this phase never opened.
