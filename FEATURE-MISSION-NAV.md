# Mission navigation: press one key on a mission, fly to it

An optional feature branch. Adds a **missions tab** to the cruise panel where a
single keypress travels to a mission's objective — a course lock if it is in this
system, a grav jump if it is not, hopping through intermediate systems when the
destination is out of the ship's range.

Everything is behind ini switches and can be turned off. The existing bodies tab is
unchanged.

---

## What it does

| you press RB on a mission | what happens |
|---|---|
| objective is in this system | the autopilot takes a course to it |
| objective is in another system | the ship grav jumps there |
| destination beyond jump range | it flies the reachable leg, and the engine covers the rest |
| no path within range | it declines, and says why |

The tab is one row per mission: the **current objective** on the left, the
destination body on the right, and vanilla's own faction or mission-type symbol
beside it. A mission with more than one destination — "Into the Unknown" sends you
to two planets — lists the extras nested under the first, each independently
selectable.

---

## How it works

### The destination is a route, not a marker

The engine keeps the jump destination as a `BSTArray` of
`{starFormID, planetDataFormID}` pairs, looked up by the player's ship form id.
`GravJumpInitiateCompleteHandler` slot 1 reads that array and performs the jump. A
null lookup is why earlier attempts ran a full jump calculation and then went
nowhere.

Rather than synthesising a route — which does not work, the engine rejects a
fabricated object even with byte-correct fields — this patches the lookup call
*inside* slot 1 and fills the engine's **own** header in place. The engine keeps its
object, its ship pointer and its state; only the two form ids change.

### Entry 0 is the start of the final leg

Not the ship's position. Observed: the engine plotted `[0] Olympus` while the ship
sat at Volii, with no jump in between. Handing it the last leg makes it fly the
earlier ones itself, so a trip of any length is one keypress.

### Range comes from the engine

`GetGravJumpRange`'s Papyrus native is a three-line wrapper around the real
function, with a `× 3.2615560` on the end — parsecs to light years. So the
underlying call returns **parsecs**, the same unit as `STDT`'s `BNAM` star
positions. No conversion and no guessed constant. Legs are chosen by breadth-first
search over the 123 systems, fewest hops.

### Random-planet quests resolve from their stored aliases

Quests like "Into the Unknown" and the radiant board missions pick their planets
**when the quest is received**, and keep them in named location aliases saved with
the game. That is why the choice survives a reload while nothing about the location
object reveals it — the runtime `FF` location is a stub, and the association lives
on the QUEST.

    Quest.GetAlias(idx) -> LocationAlias.GetLocation() -> the LCTN table

Both named Papyrus functions, no offsets. The alias indices come from the ESM
(`ALLS` + `ALID`), so only planet-bearing aliases are ever asked, and it runs on the
mission sweep — so these resolve at load rather than only once tracked. Measured:
225 quests, 491 aliases; "Into the Unknown" resolves to Piazzi I and Niira, both of
the locations Vladimir names.

**Where the association is NOT stored**, each ruled out by measurement and recorded
so it is not retried:

- the location's parent field — no form pointer at any offset in the object
- `Location.GetCurrentPlanet` — nothing, for a system-level location
- `Location.GetParentLocations` — empty
- the reference's worldspace — `<none>`, or itself runtime and unnamed
- the reference's position — no scale factor lands it near any star

### Star and body ids

- `STDT.DNAM` is the system id (Sol is **0** — never test a system id for
  truthiness)
- `STDT.BNAM` is the galaxy position, three floats, parsecs

### Which missions are listed

Dropped only when a quest has **no target reference at all** — no objectives, or no
bound `Quest` script, so it is never dispatched and never answers. A quest whose
targets exist but did not resolve stays listed: it is a live mission, and hiding it
makes it look as though it has vanished.

### Icons

From `Factions.swf`, which `shipreticle.swf` already references. Each symbol is its
own class in `<name>Icon` (black and white) and `<name>ColorIcon` variants,
including `MissionsIcon` and `ActivitiesIcon` — which the embedded
`ShipReticle_fla.Icon_Faction_66` strip does not have. That strip remains as a
fallback.

Faction comes from the `MissionBoardFaction_*` keyword where present, and otherwise
from the quest's editor id (`CF06`, `MQ104A`, `UC01_…`), since only board quests
carry the keyword.

---

## Notes for review

**Address Library ids are RTTI-verified against 1.16.244.** `RE::VTABLE::` could not
be used for the grav jump classes: CommonLibSF's `IDs_VTABLE.h` is generated against
a different build and its ids do not point at vtables here. Two capture hooks sat on
wrong addresses because of it. `ResolveVerifiedVTable()` reads the RTTI name at
`vtable - 8` and refuses a mismatch rather than writing into unrelated `.rdata`.

**`Reticle_OnCruiseLockCourse` toggles.** Dispatching it twice in one press locks and
then clears the course. The in-system check therefore runs *first*, before any other
lock or jump path, and requests the course through `g_pendingCourseID` — the same
route the bodies tab uses, so it inherits that path's cruise check, single dispatch
and 1.5 s audit.

**A course lock is not validated by the engine.** An id the HUD feed does not carry
sends the autopilot toward the system origin, so ids are checked against the feed
before use.

**The ESM record merge is field by field.** A new field on `QuestRecord` is parsed
correctly and then silently dropped unless it is also added to the merge into
`g_questRecords`.

## Known gap

The travel animation does not play on this path. It appears only on the
reticle-gated HUD action, which is gated before the handler this uses. Both leads
investigated — the grav jump default objects have no consumer sites, and routing
through `ShipHud_JumpToQuestMarker` never reaches the handler — are recorded in
`PHASE9-BINARY-GRAVJUMP.md` §3v.

## Settings

| ini key | default | what it does |
|---|---|---|
| `bMissionTab` | true | the missions tab at all |
| `bMissionJump` | true | RB travels to the objective |
| `bMissionJumpSpoof` | true | the route-substitution path |
| `fMaxJumpParsecs` | 0 | 0 = ask the engine for the ship's range |
| `bPanelMissionIcons` | true | faction / mission-type symbols |
| `bMissionJumpAnimated` | false | does not work; kept so it is not retried |
