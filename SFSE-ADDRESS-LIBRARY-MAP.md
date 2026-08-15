# SFSE, the Address Library, and CommonLibSF — a working map

Written 2026-08-12 from the actual sources, not from memory:

- **SFSE** — `github.com/ianpatt/sfse`, cloned to `F:\Claude\sfse`. This is the real thing this
  plugin is loaded by.
- **CommonLibSF** — `github.com/libxse/CommonLibSF`, cloned to `F:\Claude\commonlibsf`, with its
  `lib/commonlib-shared` submodule.
- **Address Library for SFSE Plugins** — Nexus mod 3256; its data files are the
  `versionlib-*.bin` set already sitting in `Data/SFSE/Plugins/`.

⚠ **Naming.** This project loads **SFSE** (Starfield), not SKSE (Skyrim). They share an author,
an architecture and most of their vocabulary, and the *shared* relocation library literally
enumerates `Loader::{SKSE, F4SE, SFSE, OBSE}` — but the interfaces and ids below are SFSE's.

---

## 1. The three layers, and which one lies to you

```
   your plugin
       |
   CommonLibSF        RE/ reversed game types   REL/ ids   REX/ platform   SFSE/ interface wrappers
       |                    ^ hand-written, can be WRONG or STALE
   Address Library    id -> offset, per game build   (versionlib-<version>.bin)
       |
   SFSE               loader + trampoline + 4 plugin interfaces
       |
   Starfield.exe
```

The important asymmetry: **SFSE and the Address Library are mechanical, CommonLibSF is
editorial.** SFSE's job is to load you and hand you four function tables; the Address Library's
job is a number-to-number lookup. Neither can be "wrong about a struct" because neither
describes structs. CommonLibSF *does* describe structs, by hand, and that is where the failures
in this project have come from.

---

## 2. SFSE — the plugin contract

### Declaring yourself

`SFSEPluginVersionData` is exported as `SFSEPlugin_Version`. SFSE reads it and decides whether to
load you **without calling any of your code**. Fields that matter:

| field | meaning |
|---|---|
| `kAddressIndependence_Signatures` | you find addresses by byte-pattern scanning |
| `kAddressIndependence_AddressLibrary` | you use Address Library **v1** |
| `kAddressIndependence_AddressLibraryV2` | Address Library for **1.15.216+** |
| structure independence | you claim to survive struct layout changes |
| compatible runtime versions | an explicit list, or a "runs on anything" claim |

This is why the README's line about the Address Library being a hard requirement is literally
true: declare address-library independence and a missing `versionlib-*.bin` is a startup failure,
not a degraded feature.

### `QueryInterface(id)` — the four tables

| id | interface | what it gives |
|---|---|---|
| 1 | `SFSEMessagingInterface` | `RegisterListener(handle, sender, cb)`, `Dispatch(...)` |
| 2 | `SFSETrampolineInterface` | `AllocateFromBranchPool`, `AllocateFromLocalPool` |
| 3 | `SFSEMenuInterface` | `RegisterMenuMovieCreated`, and at v2 `RegisterScaleformManagerCreated` |
| 4 | `SFSETaskInterface` | `AddTask`, `AddTaskPermanent` |

### Messaging — the message types, in order

`kMessage_PostLoad`, `kMessage_PostPostLoad`, `kMessage_PostDataLoad` (**passes
`TESDataHandler*` as data**), `kMessage_PostPostDataLoad`, `kMessage_PreSaveGame`,
`kMessage_PostSaveGame`, `kMessage_PreLoadGame`, `kMessage_PostLoadGame`.

This mod handles only `kPostDataLoad`, and its comment records why the save-lifecycle four never
arrive in practice.

⭐ **`PostDataLoad` hands you the `TESDataHandler` pointer directly.** That is an independent
source for the singleton the case study below is arguing about — worth using as a cross-check
rather than resolving it through an address id.

### Menu — the callback this mod's newest feature rests on

```cpp
typedef void (*MenuMovieCreatedCallback)(IMenu* menu);
void (*RegisterMenuMovieCreated)(MenuMovieCreatedCallback callback);
```

The header's own comment: *"called once for every new menu where its MovieImpl is loaded."*
**Every menu.** That is the mechanical basis for PHASE 8's star-map subscriber — the callback was
always going to fire for `GalaxyStarMapMenu`; this mod simply filtered it to the ship HUD.

### Task — and the comment that is wrong

```cpp
// This task will be executed once on the Main thread, then deleted
void (*AddTask)(ITaskDelegate* task);
// This task will be executed every frame on the Main thread without deleting
void (*AddTaskPermanent)(ITaskDelegate* task);
```

⛔ **"on the Main thread" is not true for Starfield**, and this project paid for that in the
v1.1.2 takeoff crash: both are dispatched from the same `Command_Process` hook, which crash
backtraces show running on **BSJobs workers**. Everything in this plugin that says "there is no
main-thread bounce" traces to exactly these two lines being wrong. Treat the SFSE headers as
authoritative about *shape* and unreliable about *threading*.

---

## 3. The Address Library — what an id actually is

An id is a **stable name for a location** across game builds. `versionlib-<runtime>.bin` maps id
→ offset for one build; ship a new game patch, ship a new file, and plugins keep working without
recompiling.

Resolution, straight out of `commonlib-shared/include/REL/ID.h`:

```cpp
std::size_t ID::offset() const  { return IDDB::GetSingleton()->offset(m_id); }
std::uintptr_t ID::address() const
{
    return REX::FModule::GetExecutingModule().GetBaseAddress() + offset();
}
```

So `REL::Relocation<T> x{ ID::Foo::Bar }` is *module base + table lookup*, resolved lazily on
first use. `IDDB` (in `REL/IDDB.h`) memory-maps the file and supports formats `V0/V1/V2/V5` and
loaders `SKSE/F4SE/SFSE/OBSE` — one shared implementation for the whole family.

### ⚠ The failure mode nobody warns you about

`IDs.h` carries entries that are **placeholders**:

```cpp
inline constexpr REL::ID HasQuestObjectAlias{ 0 };  // 83336
```

The comment holds the real id; the value is `0`. An unmapped id does **not** behave like a null
pointer — `offset()` returns whatever the table says for 0, and `address()` is a perfectly
well-formed pointer into the wrong place. TODO.md records the same shape for `GetEventSource`
(id 89264, carried as 0). **Before using any `ID::`, read its value, not its name.**

---

## 4. How to verify a CommonLibSF declaration

The method this project should have been using, in order of authority:

1. **`sfse/Game*.h`** — the SFSE author's own hand-reversed definitions (`GameData.h`,
   `GameForms.h`, `GameUI.h`, `GameScript.h`, `GameRTTI.h`, …). Maintained against the same
   builds, independently of CommonLibSF. Two independent reversers agreeing is real evidence.
2. **`static_assert(offsetof(...))`** in the CommonLibSF header — an assert that fires is a
   compile error, so any struct carrying them is at least self-consistent.
3. **A runtime read with a known answer** — the strongest of the three. This mod has an unusually
   good one available: its own ESM parse counts **1765 PNDT records in Starfield.esm** and logs
   it every launch, so any engine-side enumeration can be checked against a number the plugin
   derived independently.

---

## 5. Case study: `TESDataHandler` form arrays (PHASE 8, unresolved)

**The measurement:** `formArrays` reports **0 of 215 arrays non-empty**, PNDT included, against
1765 PNDT records parsed from the same file on the same launch.

**Both headers agree on the layout**, which rules out the first thing to suspect:

| | CommonLibSF | SFSE |
|---|---|---|
| array member | `FormArray formArrays[kTotal]` @ **0x0070** | `FormItem pFormArray[kTotal]` @ **0x70** |
| entry | `{ BSReadWriteLock lock; BSTArray<NiPointer<TESForm>> }`, `sizeof == 0x18` | `{ u64 unk00; BSTArray<TESForm*> }` = 8 + 0x10 = 0x18 |
| array size/capacity | `_size` @ 0, `_capacity` @ 4 | `uiSize` @ 0, `uiAllocSize` @ 4 |

And the singleton id is **not** a placeholder: `ID::TESDataHandler::Singleton{ 937572 }`.

**So the remaining hypotheses are:**

1. **Id 937572 is absent or wrong in `versionlib-1.16.244`.** The pointer would then be
   well-formed garbage. Weak evidence against: 215 lock/unlock cycles ran without faulting, which
   a junk pointer usually would not survive.
2. **Starfield does not populate `pFormArray` at all**, and both headers describe a member that
   exists but is vestigial. The `BSService::Detail::TService<...>` comment sitting at `unk28` in
   SFSE's own declaration is a hint that this class was refactored toward a service registry.
3. Something about *when* it is read.

**The next diagnostic, and it is cheap:** compare the singleton obtained three ways — the
address-library id, the `TESDataHandler*` that `kMessage_PostDataLoad` hands over for free, and a
sanity field (`regionList` @ 0x14E8) read off each. Agreement narrows it to hypothesis 2;
disagreement proves hypothesis 1 outright. **The message route needs no address id at all**,
which is what makes it the control.

⚠ Note for whoever picks this up: hypothesis 2 being true would mean **no enumeration of forms by
type is available in this game**, which is what pushed PHASE 8 onto the load-order parse
(`ParsePluginQuests`). That route works and needs none of this — so this case study is worth
finishing for the knowledge, not because the feature is blocked on it.
