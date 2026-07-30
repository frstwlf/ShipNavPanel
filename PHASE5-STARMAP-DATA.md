# Phase 5 — How the star map gathers system data, and what of it we can reach

2026-07-30. Offline analysis: decompiled every starmap-family SWF out of
`Starfield - Interface.ba2` (pool verified current and complete against the
BA2's own file table — 426/426 names match), read the vanilla Papyrus sources,
and searched CommonLibSF's RTTI/ID inventories for the engine side. Nothing
here is an in-game observation unless marked as one; the one open runtime
question has a zero-rebuild probe (§6).

Script exports now live under `M:\Starfield\Extracted\scripts\` alongside the
HUD ones: `galaxystarmapmenu`, `systemview`, `systeminfopanel`,
`planetinfocard`, `starmaptemplate`, `galaxymarkermenu`, `galaxy2dmap`,
`galaxystarmapmarkers`, `surfacemap`, `almanacmenu`.

## 1. The pipeline: engine-push over menu-scoped providers

The star map draws nothing from data it computes in AS3. Every piece arrives
through `BSUIDataManager.Subscribe(name, fn)` — the same shuttle the mod
already rides — and every provider is **published by native model objects that
exist only while their menu does**. The movie-side plumbing
(`UIDataShuttleConnector`): the engine injects a native `_Watch(name, obj)`;
`Subscribe` registers `obj`, the engine writes fields into it and calls
`onFlush(name)` to dispatch. `_Watch` accepts ANY name from ANY movie — which
is why the v0.4.x probe "subscribed fine" — but flushes only come from a live
publisher, which is why it then never fired. There is also a pull API,
`BSUIDataManager.GetDataFromClient(name).data`, used by the markers class to
re-read **its own movie's** watched object; it returns the local object, so
from the HUD movie it pulls from the same never-filled buffer. Pull-after-map-
close is a dead end by construction, not by missing luck.

## 2. The system (orrery) view's providers and payloads

Subscribed in `GalaxyStarMapMenu.as` / `GalaxyStarMapMarkers.as` /
`SystemInfoPanel(Mini).as`:

| provider | payload (fields the SWFs read) |
|---|---|
| `StarMapMenuData` | `iCurrentMenuView` (`ViewTypes`: galaxy/system/surface/inspect), `uSystemLocationID`, `uBodyLocationID`, cursor/grid flags |
| `StarMapMenuSystemBodyInfoData` | **the nested body tree**: recursive `{bodyID, bodyType (BT_*), sunRadiusPercent, scanned, color, childInfoA[]}` + `focusedBodyID`, `focusedBodyType` — star→planets→moons, exactly the hierarchy our ESM parse builds |
| `StarMapMenuMarkersData` | `aMarkersData[]`: `sMarkerText` (localised name), `uBodyID`, `uBodyType`, `bMarkerDiscovered`, `bHasLife`, `bHasIncursion`, `bHasUndiscoveredPOI`, `bHasQuestTarget`, `bIsInHighlightRadius`, `bIsFocused`, `fMarkerWidth/Height`, `uPoiType`, `uPoiCategory` (space POIs arrive as `BT_SATELLITE`) |
| `StarMapMenuMarkerPositions` | engine-projected 2D positions per marker — the 3D bodies are engine-rendered; GFx only overlays plates |
| `StarmapSystemBodyInfoProvider` | focused body's **full dossier** → `BodyDataInfo.SetBodyInfo`: `uBodyID`, `iType`, `sBodyName`, `sSystemName`, **`sParentBodyName`** (moon subtitle), `sTerrain`, `fGravity`+`sGravityDescriptor`, `sTempDescriptor`, `sAtmospherePressure/Type/Toxicity`, `sMagnetosphere`, flora/fauna/water descriptors+counts+probabilities, `ResourcesA[]`, `TraitsA[]`+counts, `iScanLevel` (SL_*), `fSurveyPercent`, `bPlayerVisited`, `bPlayerEnteredSystem` |
| `StarMapMenuSystemNameHeaderData` | system header: `spectralClass`, `catalogueID`, `temperatureKelvin`, `solarMass`, `radius`, `magnitude`, `planets`, `moons`, `outposts`, `surveyPercent`, `entered` |
| `StarMapMenuBodyPOIDefs` / `...Positions` / `...Groups` | inspect-view POI markers: `nameText`, `type` (landing/location/ship), `uLocationType`, `genericType`, `discovered`, `isVisible`, `markerHandleBits` |
| `SurfaceMapInfo` / `SurfaceMapBodyInfoProvider` / `SurfaceMarkerList` | surface map cell range, dossier again, surface markers |
| `DataMenuData` | carries `CurrentSystemBodyInfo` (SWFs read only `focusedCelestialBodyID`) — data-menu scoped |
| `AlmanacSystemBodyListData` (almanacmenu) | per-body list: `sName`, `fDistance`, `bSettled`, `bHasOutpost`, `bFavorited`, `bSurveyCompleted`, `bHasIncursion`, `bIsPlayerBody` |

Widget flow for the schematic: `StarMapMenuSystemBodyInfoData` →
`SystemInfoPanel.SetSystemBodyInfo` → `SystemViewHolder.UpdateSystemView(tree)`
→ `BodyView.UpdateInfo` recurses `childInfoA`, one clip per body
(`SystemView`→`PlanetView`→`MoonView`). Names in the system view come from the
markers feed, not the tree.

Engine-side model types exist for all of it (CommonLibSF `IDs_RTTI.h`):
`StarMap::BodyInfoToUI`, `StarMap::PlanetInfoToUI`, `StarMap::BodyPOIDef`,
`StarMap::StarMapMenuMarkerData`, `StarMap::SurfaceMarkerStaticData`,
`StarMap::Outpost_DS`, `InfoTargetData`, plus a singleton
`UIDataShuttleManager`.

## 3. Where the engine gets it: the BSGalaxy component DB

RTTI names the true source — a `BSComponentDB2` store with per-body
components: **`BSGalaxy::BodyChild`** (the parent/child relation),
`OrbitalData`/`OrbitedData`/`OrbitOffset`/`OrbitState`, `StarData`,
`CTBodyType`, `CTPlanetLocationIDs`, `CTPlanetarySystemLocationID`,
`CTProxyFormPtr` (link to the PNDT/STDT form), `CTStarDistance`,
`CTSystemParsecLocation`, `PlayerKnowledge`, `PlayerAtBody`. The
`Unused*CSVData` components confirm the CSV pipeline is dev-time leftovers;
the shipped source of truth is the ESM. (`Starfield - PlanetData.ba2` is 1438
biome-map rasters — terrain, not layout.)

The local CommonLibSF fork already reaches ONE corner of this DB —
`RE/P/PlayerKnowledge.h` (knowledge manager via `ID::BSGalaxy::*` address-lib
ids, hard offsets like `kKnowledgeDbOffset = 0x8B0`) — which proves access is
*possible*, and also shows the cost: address ids + hand-carried offsets, the
exact stale-layout hazard class that produced four crashes in Phase 0/3.
General traversal (enumerate a system's bodies with types and parents) has no
CommonLibSF definitions at all; it would be a Ghidra-grade job.

## 4. Papyrus

`Planet.psc`: `GetLocation`, `GetGravity`, `GetTemperature`, `GetPressure`,
`GetSurveyPercent`, `GetDayLength`, `GetAtmosphereType`, `GetType`,
`GetKeywordTypeList` (keyword types 35–48 cover planet type / atmosphere /
toxicity / gravity class / water / magnetosphere / flora / fauna / traits /
temperature class / pressure class), `IsTraitKnown`/`SetTraitKnown`.
`Location.psc`: `GetCurrentPlanet`, `GetParentLocations`, `IsChild`,
`HasCommonParent`, `IsExplored`. So per-body stats and location-climbing
hierarchy exist — but there is **no "enumerate bodies of system X"** (vanilla
does it with quest alias machinery, see `MissionGetAllPlanetsScript.psc`), no
POI/marker access, no reach into any UI provider, and all of it needs a
shipped ESM + script + a native↔VM bridge the mod deliberately doesn't have.
Papyrus offers nothing the ESM parse doesn't already provide more cheaply.

## 5. Verdict on the ESM parse

The star map confirms the mod's architecture rather than obsoleting it: the
engine's own UI is fed from a runtime DB built out of the same records the mod
parses, serialized per menu, unreachable outside it. The GNAM triple
(system id, parent id, planet id) IS the hierarchy the tree payload encodes.
Nothing found offline replaces the parse for the whole-system list. The parse
also remains the only load-order-aware source (DLC + mod bodies) that needs no
ESM, no address ids, no menu open.

## 6. The one genuinely new, usable find: `InfoTargetProvider`

`ScanDetails.as` (spaceshiphudmenu — OUR movie) receives, from
`InfoTargetProvider` (native `InfoTargetData`), a `TargetOnlyData` object
whose **`PlanetCardInfo`** member is the same full dossier structure as §2's
`StarmapSystemBodyInfoProvider` — fed verbatim to `BodyDataInfo.SetBodyInfo`
for the ship scanner's planet card. That is: **type incl. `BT_MOON` (3),
parent body name, system name, terrain, gravity, temperature, atmosphere,
magnetosphere, flora/fauna/water, resources, traits, scan level, survey %,
for the current info target, live in cruise, in the movie we already
subscribe from.** The HUD subscribes at `SpaceshipHudMenu.as:416`; the payload
also carries `bTargetModeActive`, `canBeHailed`, `uShipCrew`.

Limits: it describes ONE body — the E-target — not the system; and the mod's
panel exists precisely because E-cycling is the thing being replaced. So it
cannot replace the ESM parse either. What it CAN do:

- per-row detail readout (gravity/temp/survey/traits) for the info-targeted
  body, with zero new reverse engineering;
- a second, vanilla-sourced authority for moon-vs-planet and parent name to
  cross-check the parse (a moon the parse mis-parents would show up here).

**Unverified (runtime): publish cadence and whether `PlanetCardInfo` is
populated for every planet info target or only in scanner mode.** The probe is
free: the recon machinery from the v0.4.x hunt is still in the DLL —
`bProbeStarmapFeed=true` + `sStarmapFeed=InfoTargetProvider` in
`ShipNavPanelCustom.ini` [Recon] subscribes the dump handler and prints the
whole payload on each publish (`[starmap]` log lines). E-target a planet, then
a moon, in and out of scanner mode, and read the log.

## 7. Settled by this phase — do not re-derive

- Every galaxy/system/POI provider is menu-scoped push; none publish to the
  ship HUD movie, and `GetDataFromClient` cannot cross movies. The v0.4.x
  "never fires with the map closed" result now has its mechanism.
- The starmap's 3D bodies are engine-rendered; the SWFs only place markers at
  engine-projected coordinates (`StarMapMenuMarkerPositions`). There is no
  drawing recipe to borrow for an orrery, only plates and nameplates.
- `BSGalaxyTypes` AS3 enum: BT_UNDEFINED 0, BT_STAR 1, BT_PLANET 2, BT_MOON 3,
  BT_SATELLITE 4, BT_ASTEROID_BELT 5, BT_STATION 6; scan levels
  SL_NOT_SCANNED −1 … SL_COMPLETE 3. (Matches the Phase-0 table; the feed's
  `uTargetType` TT_* is a DIFFERENT enum — do not mix them.)
- `almanacmenu.swf` is the data-menu resource catalog; its body list is
  menu-scoped like the rest.
- `galaxymarkermenu.swf` is a bare engine-driven debug/legacy overlay (the
  engine writes `Markers[]` and calls `UpdateMarkers()` directly) — no
  provider to borrow.
- `starmaptemplate.swf` is a style/widget library (meters, data lists,
  biome rows), not a data consumer.
