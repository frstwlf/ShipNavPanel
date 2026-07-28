# Phase 3 — vanilla blips instead of a drawn arrow (2026-07-28)

The design shift under assessment: **in cruise, hide the ship HUD's own
off-screen blips** (the circle-and-arrow markers on the reticle ring that point
at things outside the view), **keep the named in-view markers**, and when a body
is locked in the panel, **let that one body's vanilla blip back through** — so
the vanilla marker does the pointing and the mod's invented diamond becomes a
fallback rather than the main event.

Verdict up front: **feasible, one honest scope limit, and one specific
mechanism survives contact with the SWF's own code.** Everything below was read
out of the decompiled ActionScript, not guessed. Regenerate the sources any
time with:

    "C:\Program Files (x86)\FFDec\ffdec.bat" -export script <outdir> M:\Starfield\Extracted\interface\spaceshiphudmenu.swf

(~3 seconds, 217 classes. The classes quoted here: `ShipReticle.as`,
`OffScreenIcon.as`, `TargetIconBase.as`, `ShipReticleAnimationEventHandler.as`.)

---

## 1. The two blip kinds live in different containers — the split is already ours

`ShipReticle.RefreshOffScreenIcon`:

```actionscript
_loc6_ = this.GetClip(this.OffScreenIndicators, this.OffScreenIndicatorArray,
                      OffScreenIcon, param1.uniqueID,
                      this.ShipReticle_mc.OffScreenIndicatorParent_mc) as OffScreenIcon;
_loc6_.name = "OffScreenIcon: " + param1.name;
```

`RefreshOnScreenIcon` does the same with parent **`this`** (`Reticle_mc`
itself) and `name = "OnScreenIcon: " + param1.name`.

So:

| kind | class | parent | named |
|---|---|---|---|
| circle-and-arrow (off-screen) | `OffScreenIcon` | `Reticle_mc.ShipReticle_mc.OffScreenIndicatorParent_mc` | `"OffScreenIcon: " + name` |
| named in-view marker | `TargetIconBase` subclasses | `Reticle_mc` | `"OnScreenIcon: " + name` |

**Hiding `OffScreenIndicatorParent_mc` hides exactly the circle-and-arrow set
and nothing else.** The named markers never pass through that container. And
the clips carry the *target's feed name* in their own `name` property, which is
the outside-visible identity Phase 1 said the icons lacked — `uniqueID` still
is not on the clip, but the name is, and the mod holds both id and name for the
locked body from the low-frequency feed.

Two bonuses fell out of the same read:

- **The player's actual E-target never goes dark.** For the info target, an
  off-screen situation gets a *paired* edge-snapped `TargetIconBase` (see
  `SnapIndicatorToEdge`), parented to `Reticle_mc` — outside the hidden
  container. Targeted-thing indication survives untouched.
- **Vanilla itself blanks this exact container** — on boost in cruise:

  ```actionscript
  BSEaze(this.ShipReticle_mc.OffScreenIndicatorParent_mc).FadeOut(this.BOOST_OFFSCREEN_MARKERS_FADE_TIME);
  ```

  So "cruise with no off-screen blips" is a state the HUD already enters by
  design. It also proves the SWF's only writes to the container are **alpha**
  tweens — nothing in the 217 classes ever writes its `visible`. A plugin
  `visible = false` is uncontested, and deliberately picks the property the
  boost fade does *not* use, so the two never fight.

## 2. ★★ The trap: `GetClip` treats `visible == false` as "this clip is free"

The obvious approach — hide individual icons with `visible = false` — corrupts
vanilla's clip pool. `GetClip`:

```actionscript
_loc6_ = param1[param4];            // map hit by uniqueID
if(_loc6_ != null)
{
   if(!_loc6_.visible)              // "invisible" == "pooled, revive it"
   {
      _loc6_.ResetInitialized();
      param2.push(_loc6_);          // pushed into the live array AGAIN
      param5.addChild(_loc6_);
   }
}
else { /* ...scans for ANY !visible clip and RE-KEYS it to the new target... */ }
_loc6_.visible = true;              // reasserted for every live target, every refresh
```

Three separate failure modes for an outside `visible = false` on a *live* icon:

1. The revive path pushes the clip into `OffScreenIndicatorArray` again —
   **duplicate entries accumulate every tick**.
2. The recycler scan may **steal the hidden clip for a different target** and
   re-key it mid-flight.
3. `visible = true` is reasserted per refresh anyway, so the write also loses.

Per-icon `alpha = 0` avoids the pool (the clip stays "live") but nothing in the
SWF ever *resets* icon alpha — a pooled clip keeps the mod's zero and comes
back invisible for whatever target inherits it, possibly minutes after the
feature was turned off, including a ship marker in combat. Plus a one-frame
flash whenever a brand-new icon spawns (created visible, hidden one tick
later).

**Both per-icon routes are rejected. The container is the only safe handle.**

## 3. The mechanism: hide the container, *reparent* the keepers out of it

`GetClip` re-`addChild`s a clip **only when reviving it from
`visible == false`**. A live, visible clip is left wherever it currently is —
while still receiving `SetTargetLowInfo` / `SetCombatValues` /
`SetTargetHighInfo` every refresh, because those go through the private
map/array, not the display list. So:

- **Hide:** `OffScreenIndicatorParent_mc.visible = false`, re-asserted each
  high-frequency callback (path resolved fresh each tick — see §5).
- **Reappear the locked body's blip:** enumerate the hidden container's
  children (`numChildren` / `getChildAt` — plain public DisplayObjectContainer
  API), find `name == "OffScreenIcon: " + <locked body's feed name>`, and
  `addChild` it into a small mod-owned holder clip sitting in the same
  coordinate space. The blip keeps rotating, recolouring and re-framing under
  vanilla's own per-tick updates — authentic faction colours, selected state,
  planet distance frames, all of it, for free.
- **Cleanup is self-healing:** if the locked target leaves the feed, vanilla's
  sweep does `_loc2_.parent.removeChild(_loc2_)` — which works from *any*
  parent — and pools the clip. If the pool later revives it for another body it
  gets re-addChild'ed **into the vanilla container** (`param5`), i.e. it leaves
  the holder by itself. The mod notices the holder no longer contains a
  matching name and re-searches. No stale claim is possible.

Timing property worth stating: a newly created or revived icon lands inside the
*hidden* container and only becomes visible when the mod moves it to the
holder. So the wrong state is always "kept blip missing for ≤1 tick", never
"hidden blip flashing" — failures are quiet, not noisy.

Off-screen icons are never positioned by the SWF — `RefreshOffScreenIcon` sets
only `rotation`; a blip is art drawn out at the ring radius swinging round its
parent's origin. So the holder only has to match the container's *transform*,
not follow any motion. Expected identity at `Reticle_mc`'s origin (the space
the arrow and pointer already live in); the holder-creation log prints both
transforms so a wrong assumption shows up as two numbers, not a mystery.

## 4. ⚠ The honest scope limit: vanilla only reappears what vanilla draws

Three gates, all engine- or const-side, none reachable from outside:

- **`bAllowedOffScreen`** (low-freq, per entry) gates the icon's existence.
- **`CruiseModeOffScreenPlanetIconLimit = 5`** — a `private const`. In cruise,
  non-quest **planet** blips beyond the five *nearest* are marked unupdated
  (`SetUpdateFrame(0)`) and swept. Fighting the sweep from outside loses the
  ENTER_FRAME write race or trips §2's pool hazards — traced both ways, not
  viable.
- **Bodies the HUD is not tracking at all** (the `bListWholeSystem` rows with a
  dash) have no feed entry, no icon, nothing to reappear.

So: locked body tracked + allowed off-screen + (within the 5-nearest planet cap
OR quest-flagged) → vanilla blip. Otherwise → **the mod's own diamond stays as
the fallback pointer**, exactly as today. In small systems the cap rarely
bites; in Sol-sized ones it will. The upgrade is automatic in both directions:
a locked-and-waiting body that comes into tracking range gets its icon, the mod
spots the name, and the diamond hands over to the vanilla blip on that tick.

(A fourth route — synthesising feed entries by calling the public
`UpdateLowFrequencyData` with an appended target so vanilla would draw a blip
for an untracked body — exists in principle, but needs a coherent three-array
injection per tick against the engine's own re-push, and undefined fields feed
`gotoAndStop` frame logic. Noted for curiosity, not planned.)

## 5. ⚠ Resolve the container fresh every tick — it is timeline-placed art

`OffScreenIndicatorParent_mc` is a named timeline child of `ShipReticle_mc`
(`ShipReticleAnimationEventHandler`, a `dynamic` MovieClip with frame scripts at
1/50/145/165/176 — the open/close/monocle/far-travel animations). If the
playhead crosses frames where the container is not placed, Flash re-creates it
— a fresh instance, `visible = true` again, old handles stale. Whether that
actually happens here is invisible in the decompiled AS (it is timeline data);
vanilla's own code is indifferent because it re-resolves
`this.ShipReticle_mc.OffScreenIndicatorParent_mc` on every access.

The plugin does the same: **never cache the container's `GFx::Value` across
ticks; walk the path fresh and re-assert.** Worst case is one frame of blips
during a reticle open/close animation — masked by the animation itself. The
holder is immune (it hangs off `Reticle_mc`, the stable home the arrow proved),
and a reparented blip is likewise out of the timeline's reach.

## 6. Quest blips, and a second cruise signal for free

- **Quest markers ride the same container** and are exempt from the planet cap
  (`icon.HasQuestTarget` short-circuits it) — hiding the container hides active
  mission direction, which is real wayfinding loss. `HasQuestTarget` is a
  **public getter** on the icon (`QuestMarker_mc.visible` underneath), readable
  from outside per child. Default policy: quest-flagged icons are reparented
  and kept too (`bKeepQuestBlips=true`).
- **`QLastTargetType` is also a public getter** — and ships do not get
  off-screen icons in cruise (Phase 1 census: ship targets drop out of the set
  entirely). So *a TT_SHIP (5) off-screen icon existing at all means cruise is
  over*, whatever `CruiseModeHUDActive` still claims. That is the independent
  "still cruising?" signal the TODO wanted for the forced-exit case: while
  hidden, if any container child reports type 5, restore immediately. It only
  fires when ships are around — which is exactly the case where hidden blips
  would hurt (combat interdiction).

## 7. What v0.8.0 does with all this

New `[Panel]` settings, all live:

| setting | default | |
|---|---|---|
| `bHideVanillaBlips` | `true` | hide the off-screen container while cruising |
| `bKeepQuestBlips` | `true` | reparent quest-flagged blips so missions stay pointed |
| `bShowLockedBlip` | `true` | reparent the locked body's blip back into view |

Behaviour rules:

- Cruise only, decided by a **fresh read of `CruiseModeHUDActive` inside the
  blip pass itself** (not the cached `g_inCruise`, which the low-frequency feed
  refreshes late — restore must be frame-accurate because the same container
  serves normal flight's ship blips). Plus the §6 ship-type tripwire.
- ~~When the locked body's vanilla blip is visible, **the mod's diamond is
  suppressed for it** (no double marker); the label stays — the blip has no
  text, and name + live distance is the part vanilla cannot provide. The
  diamond still previews the highlight while the panel is open, since browsing
  needs a pointer for bodies with no blip.~~
  **Superseded in v0.8.1 after the first in-game session, on the tester's
  call:** when a body's vanilla blip is showing, the mod draws **nothing** for
  it — no diamond *and no label* — and the blip also serves as the browse
  preview: the panel highlight's blip is let through while the panel is open,
  not just the locked body's. Diamond and name now exist purely as the
  fallback for bodies vanilla is not blipping (beyond the cap, or dash-row
  untracked). The "name + distance is worth keeping" argument above lost to
  how it actually looked in game: the vanilla blip alone is cleaner, and the
  panel row already shows name and distance.
- Restore = container `visible = true` + every holder child `addChild`'d back
  into the container. Runs on cruise exit, on the tripwire, and when the
  feature is off. A movie rebuild resets everything anyway
  (`OnMenuMovieCreated` drops the new handles exactly as it drops the arrow's).
- All Scaleform work stays inside the feed callbacks (engine UI thread), and
  holder creation takes a `SingleWinner` claim like every other one-shot
  builder — the v0.7.5 lesson stands.
- Per tick in cruise this adds roughly: one path walk, one `SetMember`,
  `numChildren`, and name/type reads over ≤~8 children. Noise next to what the
  arrow and panel already do per tick.

### To verify in game (first cruise with the build)

1. `[blip]` holder-creation log: container found, transforms printed (expect
   identity/zero), child census with names as `OffScreenIcon: <name>`.
2. Blips vanish in cruise; named in-view markers unaffected; E-target edge
   indicator unaffected.
3. Lock a tracked body → its vanilla blip appears on the ring, diamond gone,
   ~~label still up~~ (v0.8.1: label gone too); rotation tracks while
   steering. Clear the lock → blip gone. Browsing with the panel open behaves
   the same per highlighted body: blip when vanilla has one, diamond+name when
   it does not. **v0.8.0's core confirmed in game 2026-07-28** — blips hidden,
   locked blip reappearing, named markers untouched — in a short session;
   the v0.8.1 behaviour and the remaining items below are still open.
4. Lock a dash-row body → diamond behaves exactly as v0.7.x (fallback path).
5. Quest marker in system → its blip survives the cull.
6. Boost in cruise → no fight (container hidden regardless; holder deliberately
   does not mirror the boost fade — the locked blip staying up while boosting
   is a feature, noted here so it is not reported as a bug).
7. Leave cruise (normally and by interdiction) → ship blips back instantly.
8. Reticle open/close animations → at worst a one-frame blip flash.

## 8. v0.8.2 — vanilla presence in the FOV counts too, and the marker goes native

Two more of the tester's calls after the v0.8.1 session, both implemented:

- **The on-screen icon is also "vanilla covers it".** In cruise a body in view
  gets the named on-screen marker instead of an off-screen blip
  (`RefreshTargets` makes them mutually exclusive there), and the mod was
  drawing diamond+name on top of it. Now the blip pass also asks vanilla's own
  display list — `Reticle_mc.getChildByName("OnScreenIcon: <name>")` — and
  stands down when the icon is there and visible. A hit is verified against
  `Name_tf.text` (rewritten from the *current* target every refresh), because
  pooled clips keep stale instance names: the edge-snapped indicator revived
  for the info target is one path that never renames the clip. An
  overlap-hidden icon (`HideOverlappingClipsForCruiseMode` sets `visible =
  false` without unparenting) counts as NOT covering — the mod's marker may
  briefly coexist with a cluster vanilla is decluttering, which was preferred
  over showing nothing at all for a marked body.

- **The fallback marker is now a real `OffScreenIcon`.** `CreateObject` on the
  class name (default package, so the bare name is qualified) instantiates the
  library symbol exactly as the SWF's own `new OffScreenIcon()` does — art,
  timeline frames, faction wrapper. The mod adds it to its holder (same
  coordinate space as the real ring), and drives it through the same public
  methods the reticle uses: `SetTargetLowInfo` once per body with a synthetic
  low object (every field the method reads set explicitly, faction neutral,
  `isInfoTarget=false` so it never lies about being the E-target), then
  `SetTargetHighInfo` per tick with `{angleToCrosshair, distance}` — which
  does the `rotation = angle + 180` swing itself, plus the distance-based
  planet frames. **Planets and stars only**: the POI/ship/station icon path
  feeds `uPoiType`/`uPoiCategory` into `MapIcons.SetLocation`, fields the mod
  cannot fill truthfully (still the open POI-kind question), so those types
  keep the drawn diamond. Construction failure at any step latches
  `g_fauxFailed` and the diamond quietly returns.

  The faux blip lives inside the holder, so both holder loops (the per-tick
  return pass and `RestoreVanillaBlips`) skip any child not named
  `OffScreenIcon: *` — without that, restore would push the mod's own marker
  into vanilla's container, where it would vanish under the next hide.

- **The label wears vanilla's text styling**: the borrowed `TextFormat` is now
  applied verbatim (it comes from the HUD's own lock-on caption) instead of
  being recoloured cyan and resized. `bVanillaStyleMarker=false` restores the
  pre-v0.8.2 diamond and cyan label wholesale.

~~Open cosmetic question for the next session: the label still rides at
`fArrowRadius + 34` while the faux blip sits at the art's own ring radius —
if they crowd each other, the fix is a measured radius (`getBounds` on the
faux blip), not a guessed constant.~~ **Resolved in v0.8.4 by removing the
label outright** — the tester's call after v0.8.3 confirmed the faux marker
in game: vanilla blips carry no names, the panel row already shows name and
distance, and the mod's whole purpose is a quieter HUD. `bLabel` and all
label code are gone; nothing the mod draws on the HUD carries text.

## 9. v0.8.5 — the selection wins screen-overlap fights against planets

The tester's case: Nova Galactic Staryard locked in the panel, working
perfectly — until Earth slid into view and the station's marker vanished in
favour of the planet's, dropping the mod back to its own marker. E-targeting
the station flipped it back, which was the clue that the precedence is
dynamic. The mechanism, read from `HideOverlappingClipsForCruiseMode`:

```actionscript
param1.sort(TargetIconBase.Compare);          // earlier icon = blocker
if(_loc6_.visible && _loc6_.alpha >= MinBlockingAlpha)   // blocker gate
...
_loc9_.visible = !this.CruiseModeHUDActive || _loc17_ > 0.05;  // loser hidden
```

and `TargetIconBase.UpdateBSV`, the sort key, recomputed every pass:

| rank | condition |
|---|---|
| −2 | `isInfoTarget` — the E-target (the tester's observation, explained) |
| −1 | `bIsCruiseTargetLock` — the cruise autopilot body |
| 0 | has a quest target |
| distance, **capped at 1 LS** | `TT_PLANET` in cruise |
| raw distance | everything else |

The cap is why planets beat stations: Earth at 300 LS sorts as 1 LS, ahead of
any station beyond one light-second. `_bsv` is private and recomputed per
pass, so the sort cannot be influenced — but the **blocker gate can**: a
blocker must pass `alpha >= MinBlockingAlpha`, and **nothing in the SWF ever
writes an on-screen icon's root alpha** (`SetBlockedClipAlpha` dims
`Internal_mc`, a child). Zeroing the planet icon's root alpha therefore hides
it AND disqualifies it as a blocker in one uncontested write — vanilla's own
overlap pass then leaves the selection's named icon visible, exactly the
E-target visual. The clip pool is untouched: `GetClip` keys on `visible`,
which is never written.

Implementation: while the selected body's on-screen icon exists, every
tracked TT_PLANET body's icon (moons included, stale-name-verified via
`Name_tf.text`) is tested for rect overlap against it — the same public
`GetPositionAdjustedBounds` vanilla intersects — and gets `alpha = 0` when
crowding, `alpha = 1` otherwise, re-asserted per tick. Level-based, so no
oscillation: a faded icon keeps its geometry. Quest icons and the info
target's icon are deliberately never faded — missions and the player's own
E-target continue to outrank the panel. Restores run on deselect, separation,
cruise exit, the ship-type tripwire, and movie teardown; a clip pooled
*while* faded is unreachable until it revives, at which point the per-tick
pass or the next rebuild squares it — planet-class icons only, so the
residual worst case is a briefly invisible planet marker, never a ship's.

One deliberate coverage nuance: a freshly faded blocker leaves the
selection's icon hidden until vanilla's next overlap pass, so "covered"
counts `fadedBlocker` for that tick and the mod's marker does not flash into
the gap.

**v0.8.6 — the fade covers both directions.** The first session proved the
planet-only scope wrong by symmetry: with Earth *selected* and the Staryard
near, the station sat on top — legitimately, because the 1 LS planet cap only
beats FAR non-planets, and anything *nearer* than the planet outranks it by
raw distance. The pass now enumerates the reticle's `OnScreenIcon: *`
children and fades whatever crowds the selection regardless of type,
stale-name-verified per child. The exemptions survive the generalisation:
quest-marked icons (public `HasQT`) and the E-target's icon — the info
target's identity now captured from the low feed's payload
(`iInfoTargetIndex`, resolved through the index-aligned candidate list) —
are re-asserted to full alpha instead of faded, so missions and the player's
own targeting still outrank the panel. Restore likewise enumerates children
rather than probing a name list. The pooled-while-faded residual widens from
planet icons to any icon class; same healing (next cruise tick or rebuild).

The same session also caught the whole-system list silently dead in Sol —
`AppendSystemBodies` tested `systemID != 0`, and **Sol is system 0**. Third
strike for that trap; the settled list in TODO.md now says presence is a
separate bool, never the value.

### Failure modes accepted

- Plugin dies mid-hide → blips return on the next HUD movie rebuild (map
  open/close is enough). Display-only state, nothing in a save.
- Two feed entries sharing a name (seen: "Ship"×2) would both match a lock on
  that name — locks are bodies, whose names are unique per system in practice;
  worst case two blips show. Cosmetic.
- A HUD replacer that renames the reticle paths breaks this feature the same
  way it breaks the arrow and panel — same risk profile as the rest of the mod,
  no SWF is patched.
