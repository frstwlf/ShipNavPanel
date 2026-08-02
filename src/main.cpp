#include "SFSE/SFSE.h"

#include "REX/TIniSetting.h"

#include "RE/Starfield.h"

// For VirtualQuery: the GNAM scan reads past a declared struct, so it checks
// the page is committed and readable before touching it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include <zlib.h>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	// ---------------------------------------------------------------------------
	// ShipNavPanel - Phase 0 (recon).
	//
	// This build changes nothing in game. It observes, to settle the three
	// questions that decide whether the nav panel is buildable at all:
	//
	//  1. Does the ship scanner key still reach the game's input chain while
	//     cruise mode is active, and under what user-event name? A bound action
	//     the game has switched off still travels the chain with its 'disabled'
	//     flag set, so the log can tell "never arrives" apart from "arrives,
	//     but rejected" - those two answers need different mod designs.
	//  2. What are the exact menu names in play while piloting, and does the
	//     ship HUD's Scaleform movie load? Phase 2 injects the panel into it.
	//  3. What do ship and location state look like across normal flight,
	//     cruise, and target cycling?
	//
	// Design constraints carried over from ShipHullRegen's four crashes:
	// no iteration of engine collections, no virtual calls on game forms where a
	// plain member read will do, and every per-frame path gated on the loading
	// and main menus. The only write this plugin performs anywhere is a single
	// vtable entry for the input tap.
	// ---------------------------------------------------------------------------

	// The one diagnostic switch worth a player's attention, and the release
	// gate on log volume. OFF, the log keeps startup, the settings in force,
	// state that changed once, and every warning - a page or two a bug report
	// can carry whole. ON, it adds the per-action trace: every wheel notch, the
	// blips taken and given back, the bearing, the census dumps. That trace is
	// what built the mod, so it stays reachable; it is just not what a player's
	// log should be made of.
	//
	// Rule for anything added later: if a line repeats when the player does
	// something, it belongs behind this flag.
	// On by default since 1.1.2. It is the level that makes a bug report useful
	// without making the log expensive - a line per wheel notch and per blip, not
	// the thousands the switches below it produce. Turning it off is a choice, not
	// the starting point.
	REX::TIniSetting<bool> bVerboseLog{ "Recon", "bVerboseLog", true };

	// The recon taps default OFF in a shipped build - they were built to answer
	// questions that are now answered, and left on they write thousands of lines
	// a session. Testers turn them back on when filing a bug.
	REX::TIniSetting<bool>          bLogInput{ "Recon", "bLogInput", false };
	REX::TIniSetting<bool>          bLogInputHeldFrames{ "Recon", "bLogInputHeldFrames", false };
	REX::TIniSetting<bool>          bLogInputNonButton{ "Recon", "bLogInputNonButton", false };
	REX::TIniSetting<std::uint32_t> uMaxInputLines{ "Recon", "uMaxInputLines", 20000 };
	REX::TIniSetting<bool>          bLogMenus{ "Recon", "bLogMenus", false };
	REX::TIniSetting<bool>          bLogHeartbeat{ "Recon", "bLogHeartbeat", false };
	REX::TIniSetting<float>         fHeartbeatSeconds{ "Recon", "fHeartbeatSeconds", 5.0f };
	REX::TIniSetting<bool>          bVerifyVTableID{ "Recon", "bVerifyVTableID", false };

	// Phase 2's one real unknown: can the panel stop the ship acting on W/S by
	// marking those button events disabled as they pass the tap? While this is
	// on, the scanner key toggles a bare "panel up" state instead of cycling, so
	// the answer is not tangled up with the arrow's behaviour.
	REX::TIniSetting<bool> bSuppressThrottleTest{ "Recon", "bSuppressThrottleTest", false };

	// Survey which button events reach the mod during cruise, one compact line
	// per distinct event rather than the flood bLogInput produces.
	REX::TIniSetting<bool> bSurveyCruiseKeys{ "Recon", "bSurveyCruiseKeys", false };

	// (bWheelFilter lives in [Panel] with the rest of the shipped settings.)

	// Scaleform reader: dumps the ship HUD's ActionScript data model on demand.
	REX::TIniSetting<bool>          bScaleformReader{ "Scaleform", "bScaleformReader", false };
	REX::TIniSetting<bool>          bInterposeTargetData{ "Scaleform", "bInterposeTargetData", true };
	REX::TIniSetting<bool>          bGateOnFlightState{ "Scaleform", "bGateOnFlightState", true };
	REX::TIniSetting<bool>          bScaleformSkipBoilerplate{ "Scaleform", "bScaleformSkipBoilerplate", true };
	REX::TIniSetting<std::uint32_t> uScaleformDepth{ "Scaleform", "uScaleformDepth", 8 };
	REX::TIniSetting<std::uint32_t> uScaleformMaxLines{ "Scaleform", "uScaleformMaxLines", 3000 };
	REX::TIniSetting<std::uint32_t> uScaleformMaxChildren{ "Scaleform", "uScaleformMaxChildren", 60 };
	REX::TIniSetting<bool>          bLogTargetCaptures{ "Scaleform", "bLogTargetCaptures", false };

	// The input tap. This is not diagnostics: the scanner key reaches the mod
	// through it, so with it off nothing selects a body and the arrow never
	// appears. It is a setting at all only so a tester chasing a conflict can
	// take the mod's one vtable write out of the picture without unloading it.
	REX::TIniSetting<bool> bInputTap{ "Panel", "bInputTap", true };

	// The list panel, and the key that locks the highlighted body or clears it
	// again. The confirm key is a NAME, matched against the user event, so it
	// follows a rebind - never write an id code here.
	REX::TIniSetting<bool> bPanel{ "Panel", "bPanel", true };

	// A comma-separated LIST of user-event names and `#<id>` key codes - see
	// MatchesEventList for the syntax and for why it is a list at all.
	//
	// **The POV toggle, and it is chosen for being a NAME.** The confirm key is
	// spliced out of the camera's queue while the panel is open, exactly as the
	// wheel is, so it locks a body without swinging the view - and the view is
	// the only thing at stake if that splice ever fails.
	//
	// It is deliberately not `#67` (C), which worked and was safer in one
	// respect: C carries no user event in cruise, so nothing could collide with
	// it. But an id cannot follow a rebind, and `MatchesEventList` therefore
	// stands aside the moment the game names that key - which means a player who
	// binds anything to C loses the confirm entirely, with nothing on screen to
	// say why. A name resolves wherever the player has put it. The tester made
	// that call, and it is the right trade: silent total loss of a feature beats
	// a swinging camera as a failure mode to avoid.
	//
	// Two earlier choices, kept because each cost a build:
	//   `XButton` (R) - "the game ignores this key in cruise" is not the same as
	//   "this key is free". R does nothing while merely flying, which is how it
	//   passed, but it opens the planet map once a target is SELECTED, the state
	//   the panel exists to create. Try a candidate with a target locked.
	//   `StarbornPower` - the name C reports on its RELEASE. The panel acts on
	//   the press, which reports `ExitShip`, and in cruise neither appears.
	REX::TIniSetting<std::string> sConfirmEvent{ "Panel", "sConfirmEvent", "TogglePOV" };

	// The browse keys, in the same list shape as sConfirmEvent (v1.1.0). Until
	// now the pair was hardcoded to the mouse wheel, which is why the panel
	// could not be browsed on a controller AT ALL: `ZoomIn`/`ZoomOut` are mouse
	// bindings, so on a pad they are never reported and the branch could never
	// run. Nothing was broken - there was simply no event.
	//
	// `Up`/`Down` are the ship HUD's own D-pad events. Vanilla spends them on
	// power allocation and DISABLES that for the whole of cruise
	// (SpaceshipHudMenu wires Reticle_CruiseModeInitiate to
	// PowerAllocationComponent.InitiateCruiseMode, which calls
	// EnableInput(false); MinimalButton.HandleButtonHit then returns
	// `Enabled && bEnabled` and gates the callback on the same test). So in
	// cruise they are free in exactly the way `SHMonocle` is - confirmed in
	// game: the D-pad does not touch power allocation while cruising.
	//
	// One list serves both devices because a user event is device-agnostic -
	// the engine resolves it against whatever the player is holding - so this
	// follows rebinds on either without the mod needing to know which is in
	// use. Entries take the same `#<id>` form as sConfirmEvent, with the same
	// unnamed-press-only rule; on a gamepad an id is the Bethesda pad code
	// (D-pad up 1, down 2, left 4, right 8), NOT a virtual-key code.
	REX::TIniSetting<std::string> sBrowseUpEvent{ "Panel", "sBrowseUpEvent", "ZoomIn,Up" };
	REX::TIniSetting<std::string> sBrowseDownEvent{ "Panel", "sBrowseDownEvent", "ZoomOut,Down" };

	// The control hint along the bottom. The label is a separate setting because
	// the mod knows the confirm key's user-event NAME, not which physical key it
	// is bound to, and anyone who has moved it needs to say so here.
	REX::TIniSetting<bool> bPanelHints{ "Panel", "bPanelHints", true };
	// Q is the POV toggle on the bindings this was built against. Whether that
	// is also the VANILLA default is unverified - see the release checklist,
	// because a hint naming the wrong key is worse than no hint.
	REX::TIniSetting<std::string> sConfirmKeyLabel{ "Panel", "sConfirmKeyLabel", "Q" };

	// Panel open/close sounds (v0.12.2) - the ship scanner's own pair, per
	// the tester, played through vanilla's PlayMenuSound dispatcher (the
	// same GlobalFunc static the menus themselves use). Any sound
	// descriptor editor id works; an empty string silences that side.
	// The open/close animation's length (v0.13.0). Tune by ear against the
	// scanner sound; 0 disables it and the panel snaps as it always did.
	REX::TIniSetting<float> fPanelAnimSeconds{ "Panel", "fPanelAnimSeconds", 0.30f };

	// The scanner-key hint on the cruise HUD (v0.14.0): a real vanilla
	// button pill - the same component family the game's own cruise hint
	// uses - showing the player's ACTUAL bound scanner key (the component
	// resolves it itself, so it follows rebinds and keyboard/controller
	// swaps). Shown only in cruise while the panel is fully closed. The
	// label takes a localisation token or plain text; $SCAN is the
	// scanner's own word in every language.
	REX::TIniSetting<bool>        bScannerHint{ "Panel", "bScannerHint", true };
	REX::TIniSetting<std::string> sScannerHintLabel{ "Panel", "sScannerHintLabel", "$SCAN" };
	REX::TIniSetting<float>       fScannerHintOffsetX{ "Panel", "fScannerHintOffsetX", 0.0f };
	// 440 sits the pill just above the game's own EXIT CRUISE hint at ~478
	// (measured off the tester's screenshot; v0.15.0).
	REX::TIniSetting<float>       fScannerHintOffsetY{ "Panel", "fScannerHintOffsetY", 440.0f };
	REX::TIniSetting<float>       fScannerHintScale{ "Panel", "fScannerHintScale", 1.0f };

	// The footer, tokenised (v0.15.0, the tester's composition): the browse
	// label sits by the wheel glyph, and the confirm hint became the same
	// vanilla pill as the HUD's scanner hint, driven by the first NAMED
	// entry of sConfirmEvent - the key cap shows the player's REAL binding,
	// so sConfirmKeyLabel stops being able to lie and demotes to the drawn
	// fallback (a names-free, all-#id config cannot resolve a cap).
	REX::TIniSetting<std::string> sPanelBrowseLabel{ "Panel", "sPanelBrowseLabel", "$CycleTarget" };
	REX::TIniSetting<std::string> sPanelConfirmLabel{ "Panel", "sPanelConfirmLabel", "$ShipHUD_SelectTarget" };
	REX::TIniSetting<float>       fPanelHintPillScale{ "Panel", "fPanelHintPillScale", 0.8f };
	// The pill anchors on its origin with the label extending LEFT and the
	// key cap right, so edge alignment is a pad from the panel's right edge
	// to that origin. 40 lands the cap's edge at the plate's (v0.16.2; 130
	// read as name-column alignment, 75 still left a visible gap).
	REX::TIniSetting<float> fPanelConfirmPillRightPad{ "Panel", "fPanelConfirmPillRightPad", 40.0f };
	// The wheel pill is BACK by choice (v0.16.2): its cap can only render
	// the binding NAME ("MOUSEWHEELUP" - the component has no wheel art),
	// but the tester keeps it as the honest "whatever someone bound POV
	// cycling to". The drawn grey glyph remains its fallback.
	REX::TIniSetting<float> fPanelBrowsePillX{ "Panel", "fPanelBrowsePillX", 100.0f };
	// Which event the browse pill WEARS, per device (v1.1.0). The cap is
	// resolved by the vanilla component against whatever the player is
	// currently holding, so an event with no binding there renders an EMPTY
	// cap - which is why the hardcoded `ZoomIn` pill showed nothing at all on
	// a controller. These are separate from sBrowseUpEvent/sBrowseDownEvent
	// because that list is deliberately device-agnostic and a pill cannot be:
	// it has to name one device's binding to draw anything.
	//
	// The pad entry is a LIST, which `ButtonBaseData` accepts as an Array of
	// UserEventData - vanilla's own idiom for a two-way hint, as its
	// "$SELECT SYSTEM" pill is driven by [Left, Right] - so the cap reads as
	// the D-pad's up/down pair rather than a lone arrow.
	REX::TIniSetting<std::string> sPanelBrowsePillEvent{ "Panel", "sPanelBrowsePillEvent", "ZoomIn" };
	REX::TIniSetting<std::string> sPanelBrowsePillEventPad{ "Panel", "sPanelBrowsePillEventPad",
		"Up,Down" };
	// The highlighted row's text steps up to this colour while the bar is
	// on it - the same trick vanilla's Selected frame plays (v0.16.2).
	REX::TIniSetting<std::uint32_t> uPanelTextColorHighlight{ "Panel", "uPanelTextColorHighlight",
		0xFFFFFF };

	// The header wears the vanilla loot panel's own dress (v0.16.3, the
	// tester's ask): a SOLID strip in its measured teal with the title in
	// its measured light-teal - the strip itself is the separator, so the
	// old hairline is gone.
	REX::TIniSetting<std::uint32_t> uPanelHeaderColor{ "Panel", "uPanelHeaderColor", 0x218286 };
	REX::TIniSetting<float>         fPanelHeaderAlpha{ "Panel", "fPanelHeaderAlpha", 1.0f };
	REX::TIniSetting<std::uint32_t> uPanelTitleColor{ "Panel", "uPanelTitleColor", 0x76C0C4 };

	// The cockpit tilt (v0.16.4, the tester's ask): vanilla eases its quick
	// container into a Matrix3D whose rotation decomposes to ~5 deg of
	// pitch (top edge away) and ~5 deg of yaw turning the panel toward
	// screen centre. The yaw is MIRRORED here for the left side; both
	// angles to taste. Applied once at build - the animation's x/y/scale
	// writes flow through a 3D matrix's translation and scale without
	// touching its rotation.
	REX::TIniSetting<bool>  bPanelTilt{ "Panel", "bPanelTilt", true };
	REX::TIniSetting<float> fPanelTiltPitch{ "Panel", "fPanelTiltPitch", -5.0f };
	REX::TIniSetting<float> fPanelTiltYaw{ "Panel", "fPanelTiltYaw", -5.0f };
	// Every text in the panel except the header wears the pills' own label
	// colour (v0.16.0, measured off the SWF: the filled button's Label_tf
	// is 0xB7B7B7). The header keeps its cyan. The wheel hint went back to
	// the drawn glyph in this same colour (v0.16.1): a ZoomIn-driven pill
	// renders its cap as the binding NAME - "MOUSEWHEELUP", same as the
	// bindings menu - because the component has no wheel art.
	REX::TIniSetting<std::uint32_t> uPanelTextColor{ "Panel", "uPanelTextColor", 0xB7B7B7 };

	// The highlight bar, defaulting to vanilla's own selection colour: the
	// loot rows' Selected state is a colorTransform to flat 0xEFF3DC at
	// ~40% (measured off the SWF). The drawn panel's old invented pair was
	// 0x66CCFF at 0.28, one ini edit away if the pale bar reads worse.
	REX::TIniSetting<std::uint32_t> uPanelHighlightColor{ "Panel", "uPanelHighlightColor", 0xEFF3DC };
	REX::TIniSetting<float>         fPanelHighlightAlpha{ "Panel", "fPanelHighlightAlpha", 0.40f };

	REX::TIniSetting<bool>        bPanelSounds{ "Panel", "bPanelSounds", true };
	REX::TIniSetting<std::string> sPanelOpenSound{ "Panel", "sPanelOpenSound", "UICockpitHUDMonocleOpen" };
	REX::TIniSetting<std::string> sPanelCloseSound{ "Panel", "sPanelCloseSound", "UICockpitHUDMonocleClose" };

	// The title strip across the top, mirroring the hint bar along the bottom
	// (v0.10.0, the tester's call after the donor comparison: the drawn panel
	// stays, wearing vanilla's plate colour and a proper header). An empty
	// title disables the strip as surely as the flag does. A value starting
	// with '$' is a localisation token resolved through the game's own
	// translation at panel build - the default is the word the HUD's cruise
	// hint itself uses, so the title arrives in the player's language.
	REX::TIniSetting<bool>        bPanelHeader{ "Panel", "bPanelHeader", true };
	REX::TIniSetting<std::string> sPanelTitle{ "Panel", "sPanelTitle",
		"$CRUISE| - |$Outpost_AvailableTargets" };

	// Stations and landing sites in the list, below the bodies. Ships are off by
	// default: in traffic that would be a list of everything flying past rather
	// than of destinations.
	REX::TIniSetting<bool> bIncludePOI{ "Panel", "bIncludePOI", true };
	REX::TIniSetting<bool> bIncludeShips{ "Panel", "bIncludeShips", false };
	REX::TIniSetting<bool>  bPanelRowSeparators{ "Panel", "bPanelRowSeparators", true };

	// ---------------------------------------------------------------------------
	// Survey state in the distance cell (Phase 6, PHASE6-SURVEY-STATE.md).
	//
	// Two states, both vanilla's own - the planet card encodes progress as a
	// METER's length and completion as a separate BANNER, and this is the same
	// design at row scale:
	//
	//   0 < pct < 1   a grey progress bar along the cell's bottom edge
	//   pct >= 1      the SURVEYED banner - a near-white plate under four
	//                 diagonal bands, the distance text flipped to the banner's
	//                 own dark label colour
	//
	// The icon cell was the first idea and it does not work: 20 x 26 px is
	// PORTRAIT against a banner that is 5.7:1 landscape, so four bands would be
	// 4 px each and would sit under an icon besides. The distance cell is 96 x 26
	// (3.7:1), vanilla anchors its bands to the LEFT edge, and the distance
	// number is right-aligned - so they share the cell without fighting.
	//
	// Every colour is measured off planetinfocard.swf sprite 35 (the banner) and
	// the STARMAP card's meter. Not the ship card's meter: a parent CXFORM zeroes
	// its multipliers, so its authored colours never reach the screen.
	REX::TIniSetting<bool> bPanelSurveyMarks{ "Panel", "bPanelSurveyMarks", true };
	// The banner, measured: plate #EBECEC with gold/orange/crimson/navy bands
	// drawn in that order (navy ends up on top and widest), label #152C4E.
	REX::TIniSetting<std::uint32_t> uPanelSurveyPlate{ "Panel", "uPanelSurveyPlate", 0xEBECEC };
	REX::TIniSetting<std::uint32_t> uPanelSurveyBand1{ "Panel", "uPanelSurveyBand1", 0xE0B460 };
	REX::TIniSetting<std::uint32_t> uPanelSurveyBand2{ "Panel", "uPanelSurveyBand2", 0xEA7A49 };
	REX::TIniSetting<std::uint32_t> uPanelSurveyBand3{ "Panel", "uPanelSurveyBand3", 0xC7233B };
	REX::TIniSetting<std::uint32_t> uPanelSurveyBand4{ "Panel", "uPanelSurveyBand4", 0x2D4E7B };
	REX::TIniSetting<std::uint32_t> uPanelSurveyLabel{ "Panel", "uPanelSurveyLabel", 0x152C4E };
	REX::TIniSetting<float>         fPanelSurveyPlateAlpha{ "Panel", "fPanelSurveyPlateAlpha", 1.0f };
	// Fraction of the 96 px cell the banner covers, anchored left. 1.0 is the
	// vanilla look - plate under the whole cell, dark number on top. Drop it to
	// ~0.5 for bands only on the left with the number on the plain plate, which
	// is also the escape hatch if the banner ever renders OVER the number:
	// relative z-order among the panel's own script-added children has never
	// been proven, only assumed from creation order.
	REX::TIniSetting<float> fPanelSurveyBannerWidth{ "Panel", "fPanelSurveyBannerWidth", 1.0f };
	REX::TIniSetting<bool>  bPanelSurveyBannerText{ "Panel", "bPanelSurveyBannerText", true };
	// The progress bar. Colours are the STARMAP planet card's meter, which is
	// untinted and therefore real: fill #C5C5C5 over a #4E4E4E track. (Its third
	// constant, #B7B7B7, is already this panel's row-text colour - the palette
	// was always going to fit.)
	REX::TIniSetting<std::uint32_t> uPanelSurveyBarFill{ "Panel", "uPanelSurveyBarFill", 0xC5C5C5 };
	REX::TIniSetting<std::uint32_t> uPanelSurveyBarTrack{ "Panel", "uPanelSurveyBarTrack", 0x4E4E4E };
	REX::TIniSetting<float>         fPanelSurveyBarHeight{ "Panel", "fPanelSurveyBarHeight", 3.0f };
	REX::TIniSetting<float>         fPanelSurveyBarAlpha{ "Panel", "fPanelSurveyBarAlpha", 0.85f };
	REX::TIniSetting<float>         fPanelSurveyBarTrackAlpha{ "Panel", "fPanelSurveyBarTrackAlpha", 0.55f };
	// Below this, draw nothing at all. A track on every untouched body in the
	// system is noise, and 0 % is the overwhelmingly common case.
	REX::TIniSetting<float> fPanelSurveyMinPercent{ "Panel", "fPanelSurveyMinPercent", 0.01f };
	// How often the sweep re-reads the listed bodies. Measured cost: ten bodies
	// queue in 0.4 ms of this thread and settle in ~86 ms, so this is generous
	// rather than tight. Vanilla's own survey quests poll the same function at 15.
	REX::TIniSetting<float> fSurveySweepSeconds{ "Panel", "fSurveySweepSeconds", 5.0f };
	// Whether the sweep may BIND a Papyrus script object to a planet form when
	// the VM has none - which it must, to call a method on it at all.
	//
	// This is a [Panel] key on purpose. It began life as bProbeSurveyBind, a
	// [Recon] switch, and the sweep was reading THAT - so a user who turned the
	// recon flag off to keep the mod's no-write promise would have silently lost
	// every survey mark, with nothing in the [Panel] section hinting at the
	// connection. A debug flag gating a shipping path is the v0.2.0 mistake that
	// shipped an inert build; the release checklist greps for it.
	REX::TIniSetting<bool> bPanelSurveyBind{ "Panel", "bPanelSurveyBind", true };
	// Indent moons under their planet. Has no effect yet: nothing the ship HUD
	// feed carries identifies a moon's parent, and the guess v0.3.3 shipped was
	// wrong. Kept wired up because the data exists elsewhere - see the probe.
	REX::TIniSetting<bool> bNestMoons{ "Panel", "bNestMoons", true };

	// A small drawn glyph per body, showing what kind of world it is.
	REX::TIniSetting<bool> bPanelIcons{ "Panel", "bPanelIcons", true };

	// Vanilla art in that column (v0.11.0): each row hosts one of the game's
	// own map icons - a DynamicPoiIcon, the exact component the HUD's markers
	// use - driven by the entry's own uPoiType/uPoiCategory for POIs,
	// stations and ships, and by the surface-settlement marker for the
	// seventeen settled bodies. Undiscovered entries show the GENERIC kind
	// badge through vanilla's own masking states, matching the panel's
	// masked labels. The drawn glyphs keep the gas/ice giants (no vanilla
	// equivalent at row size) and remain the fallback everywhere else.
	REX::TIniSetting<bool>  bPanelVanillaIcons{ "Panel", "bPanelVanillaIcons", true };
	REX::TIniSetting<float> fPanelVanillaIconScale{ "Panel", "fPanelVanillaIconScale", 0.28f };
	// The giants' icon (v0.11.1, the tester's design): the in-POV marker's
	// own circle with a ring-line drawn across it - one icon for every giant
	// class. The circle is ~15 px at natural scale; 1.25 matched the badges
	// but read too big in game (the ring-line tips widen it), so v0.12.0
	// sits a third smaller on the tester's call.
	REX::TIniSetting<float> fPanelGiantIconScale{ "Panel", "fPanelGiantIconScale", 0.85f };

	// List every body in the system, not just the ones the HUD happens to be
	// offering. Bodies the game is not tracking have no distance and cannot be
	// pointed at, but they show the system's actual shape.
	REX::TIniSetting<bool> bListWholeSystem{ "Panel", "bListWholeSystem", true };

	// Probe: the star map's own body feed carries `uBodyType` (BT_PLANET,
	// BT_MOON, BT_SATELLITE...) and `uBodyID`, which is exactly the hierarchy
	// the ship HUD feed lacks. Whether it holds anything while merely flying -
	// rather than only with the map open - is the open question, and this
	// subscribes and logs whatever turns up.
	// Dumps each planet record's raw words side by side, so the layout can be
	// read off the log instead of guessed at. Three guesses have now been wrong;
	// this is the diagnostic that should have come first.
	// Does `graphics.clear()` work here? It decides how per-body icons can be
	// drawn: if a clip's drawing can be wiped and replaced, one clip per row is
	// enough and it redraws when the row's body changes. If not, every class
	// needs its own pre-drawn clip and the row toggles visibility between them -
	// eight times the clips, created up front.
	REX::TIniSetting<bool>          bTestGraphicsClear{ "Recon", "bTestGraphicsClear", false };

	// Phase 4 chrome probe (PHASE4-CHROME-HUNT.md): instantiate the ship HUD's
	// own loot panel - ShipHudQuickContainer - beside the drawn panel, with
	// four hardcoded rows, the wheel highlight mirrored into its selection,
	// and the decoration pass on top. It answered everything it was built to
	// ask, and then the tester compared the two and kept the DRAWN panel - so
	// it now defaults OFF, retired to a reference: flip it on to compare
	// again, or to probe the next borrowed part against live vanilla art.
	REX::TIniSetting<bool>  bProbeVanillaChrome{ "Recon", "bProbeVanillaChrome", false };
	REX::TIniSetting<float> fProbeChromeOffsetX{ "Recon", "fProbeChromeOffsetX", 220.0f };
	REX::TIniSetting<float> fProbeChromeOffsetY{ "Recon", "fProbeChromeOffsetY", -160.0f };

	// v0.9.1 added the decoration pass; v0.9.2 dropped row resizing from it -
	// the donor's ~31 px rows centre their text and 26 px did not (tester's
	// verdict), so vanilla density is the design now, not a default. The
	// header tint stays: an exact colour with vanilla's own mul-0-add-target
	// idiom (0 keeps the authentic teal).
	REX::TIniSetting<std::uint32_t> uProbeHeaderTint{ "Recon", "uProbeHeaderTint", 0 };

	REX::TIniSetting<bool>          bDumpPlanetRecords{ "Recon", "bDumpPlanetRecords", false };
	REX::TIniSetting<std::uint32_t> uDumpPlanetBytes{ "Recon", "uDumpPlanetBytes", 0x400 };

	REX::TIniSetting<bool>        bProbeStarmapFeed{ "Recon", "bProbeStarmapFeed", false };
	REX::TIniSetting<std::string> sStarmapFeed{ "Recon", "sStarmapFeed", "StarmapSystemBodyInfoProvider" };

	// Phase 6 probe A (PHASE6-SURVEY-STATE.md). One question decides whether the
	// panel can show a body's fully-surveyed state at all: can this plugin
	// dispatch the native Papyrus `Planet.GetSurveyPercent()` and get a float
	// back? It is the only per-body survey read that covers a whole system, and
	// nothing about the call is verified. On: the scanner key runs the probe.
	REX::TIniSetting<bool> bProbeSurveyVM{ "Recon", "bProbeSurveyVM", false };
	// How many bodies one press dispatches for. ⚠ Defaults to ONE on purpose.
	// DispatchMethodCall is reached through a vtable slot the compiler derives
	// from CommonLibSF's declaration of IVirtualMachine - if that declaration is
	// missing or has gained a virtual above slot 0x30, the call lands somewhere
	// else entirely with the wrong arguments. Finding that out once is a
	// diagnosis; finding it out twenty times in a frame is a crash. Prove one
	// call is survivable, then set this to 0 for the whole system.
	REX::TIniSetting<std::uint32_t> uProbeSurveyMaxBodies{ "Recon", "uProbeSurveyMaxBodies", 1 };
	// Whether the probe may BIND a script object to a planet form when the VM
	// has none. It is the only part of the probe that is not purely read-only -
	// see the note in DispatchSurveyPercent - so it gets its own switch even
	// though the whole probe is already opt-in.
	REX::TIniSetting<bool> bProbeSurveyBind{ "Recon", "bProbeSurveyBind", true };
	REX::TIniSetting<float> fPanelMoonIndent{ "Panel", "fPanelMoonIndent", 16.0f };

	// Hide the mouse wheel - and the confirm key - from the camera while the
	// panel is open, so browsing the list does not swing the point of view.
	// Verified in game for the wheel (v0.2.3); the switch remains as an escape
	// hatch, not because it is experimental.
	REX::TIniSetting<bool> bWheelFilter{ "Panel", "bWheelFilter", true };

	REX::TIniSetting<float>         fPanelOffsetX{ "Panel", "fPanelOffsetX", -780.0f };
	REX::TIniSetting<float>         fPanelOffsetY{ "Panel", "fPanelOffsetY", -180.0f };
	// 425 = the original 340 plus the tester's quarter (v0.12.0), close to
	// the vanilla loot panel's own 423.
	REX::TIniSetting<float>         fPanelWidth{ "Panel", "fPanelWidth", 425.0f };
	REX::TIniSetting<float>         fPanelRowHeight{ "Panel", "fPanelRowHeight", 26.0f };
	REX::TIniSetting<std::uint32_t> uPanelMaxRows{ "Panel", "uPanelMaxRows", 10 };

	// The pointer marker. (Its name label was removed in v0.8.4 - vanilla
	// shows no names on blips, and the panel row already carries the text.)
	REX::TIniSetting<bool>  bArrow{ "Panel", "bArrow", true };
	REX::TIniSetting<float> fArrowRadius{ "Panel", "fArrowRadius", 150.0f };
	REX::TIniSetting<float> fMaxTargetLightSeconds{ "Panel", "fMaxTargetLightSeconds", 80000.0f };
	REX::TIniSetting<float> fArrowAngleOffset{ "Panel", "fArrowAngleOffset", 0.0f };
	REX::TIniSetting<bool>  bArrowInvertAngle{ "Panel", "bArrowInvertAngle", false };

	// Vanilla blip management (Phase 3, PHASE3-BLIP-PLAN.md). In cruise, hide
	// the HUD's own off-screen circle-and-arrow blips and let the locked body's
	// one back through, so the game's own marker does the pointing. The named
	// in-view markers are in a different container and are never touched.
	REX::TIniSetting<bool> bHideVanillaBlips{ "Panel", "bHideVanillaBlips", true };
	REX::TIniSetting<bool> bKeepQuestBlips{ "Panel", "bKeepQuestBlips", true };
	REX::TIniSetting<bool> bShowLockedBlip{ "Panel", "bShowLockedBlip", true };
	// The fallback marker wears vanilla's own clothes: the mod instantiates the
	// HUD's OffScreenIcon class and drives it through the same public methods
	// the reticle uses, instead of drawing its invented diamond (v0.8.2).
	REX::TIniSetting<bool> bVanillaStyleMarker{ "Panel", "bVanillaStyleMarker", true };
	// Undiscovered stations and POIs list under a generic label, as the HUD's
	// own icons show them - the feed's name field leaks the real name early
	// (v0.8.14). The label normally comes from the game's own localisation
	// (the $MapMarkerGenericType* tokens, v0.8.17); these two are the
	// fallback for categories with no token or a failed translation, and
	// with bUseCustomUndiscoveredLabels they REPLACE the game's words
	// outright for anyone preferring their own.
	REX::TIniSetting<std::string> sUndiscoveredStationLabel{ "Panel", "sUndiscoveredStationLabel", "Starstation" };
	REX::TIniSetting<std::string> sUndiscoveredPoiLabel{ "Panel", "sUndiscoveredPoiLabel", "Unknown" };
	REX::TIniSetting<bool>        bUseCustomUndiscoveredLabels{ "Panel", "bUseCustomUndiscoveredLabels", false };

	// The panel selection wins screen-overlap fights against PLANET markers
	// (v0.8.5). Vanilla sorts overlapping on-screen icons by priority and
	// hides the losers, and planets' sort distance is capped at one
	// light-second, so a planet in view suppresses a station's marker behind
	// it. While the selected body's icon is being crowded, the overlapping
	// planet icons are faded to nothing - which also disqualifies them as
	// blockers, so vanilla shows the selection's own named marker instead.
	REX::TIniSetting<bool> bSelectionWinsOverlap{ "Panel", "bSelectionWinsOverlap", true };

	// ---------------------------------------------------------------------------
	// COURSE LOCK - the panel's selection reaching the ENGINE, by id.
	//
	// **CONFIRMED IN GAME 2026-08-02: this works, straight off the highlighted
	// row.** No targeting involved, nothing to select first - the key sets the
	// cruise autopilot on the body under the highlight and the ship turns to face
	// it. It is the one by-id verb this layer has, and finding that it accepts an
	// id vanilla never sends is the single biggest result since the blip pivot.
	//
	// `Reticle_OnCruiseLockCourse` carries a `uBodyID`, and vanilla sends it two
	// ways: the reticle's own LockCourse handler with 0 (ShipReticle.as:2128) and
	// the far-travel icon with a real `TargetOnlyData.uniqueID`
	// (FarTravelIconBase.as:99). What the base game never sends is an id that is
	// not the current info target - which is exactly what this sends, and the
	// engine honours it.
	//
	// ⚠ This is the AUTOPILOT, not the info target. `bIsCruiseTargetLock` is that
	// autopilot's state and has never meant "is targeted"; the two were confused
	// once here and it cost a design. Pressing the key again on the same body
	// clears the course, which is vanilla's own behaviour - its far-travel button
	// flips its LABEL between $CruiseCourseLock and $CruiseCourseClear while
	// dispatching the identical event with the identical id.
	//
	// (Its sibling experiment, `bAcquireTarget` - press the game's own target key
	// for the player until the info target IS the selection - was built, flown and
	// REMOVED the same day. It worked exactly as designed and the design was the
	// problem: one press moved the target to a neighbour, the next moved nothing,
	// and the run reported `nothing acquirable from this heading`. That is Phase 0
	// 6b/6c answered from the cockpit - cruise acquires by POINTING, so pressing
	// the key faster reaches nothing the key could not already reach. Do not
	// rebuild it; the finding is in TODO's Settled list.)
	// ---------------------------------------------------------------------------
	REX::TIniSetting<bool> bLockCourse{ "Panel", "bLockCourse", true };
	// **The game's OWN course key**, which makes this the one panel control that
	// needs no hint of its own: vanilla already draws its `$CruiseCourseLock` prompt
	// in cruise, and the player already knows the key. It is a name, so it follows
	// a rebind, and it is bound on both devices (RB on a pad), so one line serves
	// each - the same property the browse pair has.
	//
	// It is NOT a free key, and the mod does not try to share it: while the panel
	// is open the press is SPLICED OUT of the UI's input queue, so the SWF never
	// sees it and the mod's dispatch is the only one. See
	// PerformInputProcessingHook for why sharing it does not work - the ordering
	// is the opposite of what it looks like, and vanilla's `{uBodyID: 0}` is a
	// CLEAR that lands after. Vanilla keeps the key whole in every other state.
	//
	// The alternative tried first, `WeaponGroup1` (primary fire, idle in cruise),
	// worked but made the game print its own "weapons unavailable" toast on every
	// press. It stays the fallback if taking this key ever proves worse.
	REX::TIniSetting<std::string> sLockCourseEvent{ "Panel", "sLockCourseEvent", "LockCourse" };


	// Menu names probed once per heartbeat. Only "SpaceshipHudMenu" is confirmed
	// (it has an RTTI entry in CommonLibSF); the rest are guesses kept because a
	// hit costs nothing and a miss teaches nothing. The authoritative list of
	// real names comes from the [menu] open/close lines, not from this array.
	constexpr const char* kProbeMenus[]{
		"SpaceshipHudMenu",
		"HUDMenu",
		"ScannerMenu",
		"StarMapMenu",
		"DialogueMenu",
		"PauseMenu",
	};

	const char* SafeStr(const char* a_str)
	{
		return (a_str && a_str[0]) ? a_str : "-";
	}

	// ---------------------------------------------------------------------------
	// Tap 1: the input chain.
	//
	// RE::UI is a BSInputEventReceiver, so its PerformInputProcessing (vtable
	// index 1) sees the frame's whole input event queue as a linked list. The
	// vtable address is taken from the live singleton rather than from an
	// Address Library id, which keeps this working regardless of which ids the
	// current CommonLibSF happens to have mapped.
	// ---------------------------------------------------------------------------

	using PerformInputProcessing_t = void (*)(RE::BSInputEventReceiver*, const RE::InputEvent*);

	std::atomic<PerformInputProcessing_t> g_origPerformInputProcessing{ nullptr };
	std::atomic<std::uint32_t>            g_inputLinesLogged{ 0 };
	std::atomic<bool>                     g_inputTapClaimed{ false };

	// Set from the input hook, consumed elsewhere: the tree dump by the per-frame
	// task (tap 4), the capture by the interposer on the UI thread (tap 5). The
	// hook must not touch the movie itself - it runs on whichever thread the
	// input queue is being drained on.
	std::atomic<bool> g_dumpRequested{ false };
	std::atomic<bool>          g_captureRequested{ false };
	std::atomic<bool>          g_captureHighRequested{ false };
	std::atomic<bool>          g_starmapDumpRequested{ false };
	std::atomic<bool>          g_dumpPlanetsRequested{ false };
	std::atomic<bool>          g_surveyVmProbeRequested{ false };
	// Last body dossier the info-target feed published, kept so probe A's float
	// can be checked against the number the vanilla planet card would draw for
	// the same body. That the two are the same quantity is an inference from a
	// shared name and a shared >= 1 test; this is what turns it into a reading.
	// ⚠ ONE atomic, not two: the body id in the high word and the percent's bit
	// pattern in the low one. As two independent atomics the pair could TEAR -
	// one body's id read alongside another body's reading - and the 2026-08-01
	// flight caught exactly that twice in 24 samples, reporting a bogus
	// "ORACLE MISMATCH" against a body that was fine. A diagnostic that cries
	// wolf is worse than no diagnostic, because the next real mismatch gets
	// waved away. 0 means "nothing sampled yet".
	std::atomic<std::uint64_t> g_cardSample{ 0 };

	// Survey state per body, keyed by FORM ID - which is every id this feature
	// has: the panel row's, the body table's, the VM handle's low word, and the
	// dossier's uBodyID are all the same number (measured, flight 3).
	//
	// Written from the Papyrus callback, which lands on threads that are neither
	// the caller nor each other, and read at render time in RefreshPanel - the
	// same shape rowClass/rowSettled already have, which is why several rows
	// flipping in one instant needs no invalidation machinery at all.
	std::mutex                               g_surveyedMutex;
	std::unordered_map<std::uint32_t, float> g_surveyedPercent;
	// Bumped whenever the world goes UNSETTLED (a load screen). Survey state is
	// per-save: the DLL and the panel survive a quickload, the world's progress
	// does not, and a mark left over from the previous save is the one way this
	// feature can be WRONG rather than merely late.
	std::atomic<std::uint32_t> g_unsettledEpoch{ 0 };
	// Which unsettled-episode the map's contents belong to. ⚠ The READER checks
	// this, not just the sweep - the difference matters. Relying on the sweep to
	// clear the map before anything draws is relying on the writer to win a
	// race, and the two run on unordered threads: RefreshPanel rides the HIGH
	// feed while the sweep rides the per-frame task, and nothing sequences them.
	// With the epoch on the read path, a map belonging to a previous save reads
	// as UNKNOWN - which draws nothing - rather than as data.
	//
	// That also closes the async hole: a dispatch issued before a load whose
	// answer lands after the clear would otherwise re-poison the map, so the
	// callback carries the epoch it was issued under and drops its own result if
	// the world has reloaded underneath it.
	std::atomic<std::uint32_t> g_surveyedEpoch{ 0 };
	// Ticks of the last survey sweep, 0 for "never". At namespace scope rather
	// than a function static so opening the panel can zero it - the sweep only
	// runs while the panel is open, so without the re-arm the first marks would
	// be up to a whole interval late every time it is opened.
	std::atomic<std::int64_t> g_lastSweepTicks{ 0 };
	std::atomic<std::uint32_t> g_starmapCallbacks{ 0 };
	std::atomic<bool> g_interposeInstalled{ false };
	std::atomic<bool> g_interposeFailed{ false };
	std::atomic<bool> g_subscribed{ false };
	// The high-frequency feed is subscribed SEPARATELY and can fail on its own:
	// the 2026-07-28 freeze log caught it failing while the low-frequency
	// subscribe on the same manager succeeded 50 ms earlier - a mid-init movie
	// where the provider was not yet registered. The old code logged that
	// result and never looked at it, which would have silently cost the whole
	// session its bearings. Each round now subscribes only what is missing.
	std::atomic<bool> g_subscribedHigh{ false };
	std::atomic<bool> g_subscribeFailed{ false };

	// ---------------------------------------------------------------------------
	// Movie identity, for the settle gate below.
	//
	// Bumped by OnMenuMovieCreated for the ship HUD only. A movie that EXISTS is
	// not a movie that is safe to call into, and this counter is how the
	// difference gets measured - see MovieSettled.
	//
	// The SFSE menu interface can be unavailable (Init warns when it is), in which
	// case this never moves and the root pointer carries the gate alone. That is
	// weaker - an allocator is free to hand the new movie the old one's address,
	// which is exactly why the resets here key off the callback rather than a
	// pointer comparison - but it degrades to "no worse than before" instead of
	// "silently ungated".
	std::atomic<std::uint32_t> g_movieGeneration{ 0 };

	// ---------------------------------------------------------------------------
	// Mutual exclusion for the one-shot builders.
	//
	// The SFSE per-frame task and the data-feed callbacks both land on whatever
	// BSJobs worker is free - the log shows the same logical work reporting from
	// five different thread ids in one second - so two of them can be inside the
	// same builder in the same frame.
	//
	// A `g_somethingReady.load()` guard does NOT prevent that. It is
	// check-then-act: both threads read false, both build. For a builder that
	// only creates a clip that would be a wasted duplicate; for one that calls
	// into the AS3 VM it is an ACCESS VIOLATION, because the VM is not
	// thread-safe. That is the v0.7.4 crash - two threads inside
	// `BSUIDataManager.Subscribe` at once, faulting in `Starfield.exe` five
	// frames below `TryInstallSubscriber`.
	//
	// Released on destruction rather than latched, because most calls find no
	// movie yet and must be free to try again next frame. The `Ready`/`Failed`
	// flags remain the real "is it done" answer; this only serialises the
	// attempt.
	//
	// The input and camera taps got this right with `compare_exchange` from the
	// start. The three Scaleform builders did not.
	class SingleWinner
	{
	public:
		explicit SingleWinner(std::atomic<bool>& a_inFlight) :
			m_inFlight(a_inFlight),
			m_won(!a_inFlight.exchange(true, std::memory_order_acq_rel))
		{}

		~SingleWinner()
		{
			if (m_won)
				m_inFlight.store(false, std::memory_order_release);
		}

		SingleWinner(const SingleWinner&) = delete;
		SingleWinner& operator=(const SingleWinner&) = delete;

		[[nodiscard]] bool Won() const { return m_won; }

	private:
		std::atomic<bool>& m_inFlight;
		bool               m_won;
	};

	std::atomic<bool> g_subscribeInFlight{ false };
	std::atomic<bool> g_panelBuildInFlight{ false };
	std::atomic<bool> g_arrowBuildInFlight{ false };
	std::atomic<bool> g_chromeProbeBuildInFlight{ false };

	constexpr const char* kShipHudMenu = "SpaceshipHudMenu";

	// One entry per feed slot, index-aligned with both feeds: the low-frequency
	// one supplies id/type/name, the high-frequency one distance and angle. That
	// alignment is how a name gets matched to a bearing.
	// PNDT's GNAM "Galaxy Data", read straight off the record. Confirmed in
	// xEdit: Earth = (Sol 0, parent 0, planet 3), Luna = (Sol 0, parent 3,
	// planet 11). A planet carries parent 0; a moon carries its planet's id.
	// This is the exact hierarchy the HUD feed never exposes.
	struct GalaxyData
	{
		std::uint32_t systemID{ 0 };
		std::uint32_t parentPlanetID{ 0 };
		std::uint32_t planetID{ 0 };
	};

	struct Candidate
	{
		std::uint32_t id{ 0 };
		std::uint32_t type{ 0 };
		double        distance{ 0.0 };
		std::string   name;
		// From the feed. NOT "has moons" - see the settled list in TODO.md;
		// kept only because it is cheap and may yet mean something useful.
		bool isParentBody{ false };
		// From the PNDT record.
		GalaxyData galaxy;
		bool       haveGalaxy{ false };
		// POI icon identity, straight from the feed entry - what MapIcons'
		// SetLocation draws. havePoi records whether the fields were actually
		// there, since 0 is a value - and PRESENT does not mean MEANINGFUL:
		// the feed pools its entry objects, so a planet's slot can carry a
		// station's leftover fields (caught in v0.11.1: Venus wore a badge).
		// Anything consuming these must gate on uTargetType, the authority.
		std::uint32_t poiType{ 0 };
		std::uint32_t poiCategory{ 0 };
		// Captured independently of the uPoiType pair: the HUD's own name
		// recipe (GetLocationPOIName) runs off THIS field, and it can be
		// present when the POI pair is not. haveLocState records presence,
		// since LMS_UNKNOWN is 0 and therefore a value.
		std::uint32_t locMarkerState{ 0 };
		bool          haveLocState{ false };
		bool          havePoi{ false };
		// The feed's name field carries the REAL name even for markers the
		// player has not discovered (the HUD's own icons show a masked
		// generic). ⚠ This flag and uLocationMarkerState can DISAGREE: a
		// runtime-spawned encounter arrives FULL_REVEAL (the HUD names it
		// from the first frame) while bMarkerDiscovered stays false. The
		// label follows the STATE, exactly as vanilla's POIIcon does; this
		// flag is only the fallback reading when the state field is absent.
		bool discovered{ true };
		// False for bodies added from the master file rather than offered by the
		// HUD: they have a name and a place in the tree, but no bearing and no
		// distance, so the arrow cannot point at one.
		bool fromFeed{ true };
		// ⚠ A SECOND ID, and the reason this exists: `uniqueID` is not
		// necessarily what a course-lock wants. The InfoTargetProvider dump
		// caught the two disagreeing on one target - `payload.uniqueID`
		// **386531** against `PlanetCardInfo.uBodyID` **385501** ("Masada IV") -
		// so the id a target is KNOWN by and the id of the BODY it is are
		// different numbers, and `Reticle_OnCruiseLockCourse`'s parameter is
		// spelled uBodyID. If the feed entry carries such a field, that is the
		// one to send. `haveBodyID` records presence, because 0 is a value.
		std::uint32_t bodyID{ 0 };
		bool          haveBodyID{ false };
		// The cruise AUTOPILOT's current course - the per-entry
		// bIsCruiseTargetLock the far-travel icon reads to decide whether its
		// button offers LOCK or CLEAR (FarTravelIconBase.UpdateButton). ⚠ NOT
		// "is targeted": that misreading cost a design early on. This is the
		// engine's own word for where the course-lock key put the autopilot,
		// which is the only thing that can tell a dispatch that landed from one
		// that was quietly dropped.
		bool courseLocked{ false };
		// Derived at display time.
		bool isMoon{ false };
	};

	// Where GNAM sits is FOUND, not assumed. v0.3.5 assumed 0x4C, on the reading
	// that CommonLibSF maps PlanetData to 0x4C and asserts 0x58, leaving twelve
	// spare bytes. That was wrong twice over:
	//
	//   * PlanetData's member comments say 0x30 onwards, but they were written
	//     against a 0x30-byte TESForm and TESForm is 0x38. The compiler places
	//     the members eight bytes later than documented, so 0x4C is actually
	//     `periAngleInDegrees` - which is why the "system id" came back as
	//     186.0, 77.0, 151.0, 218.0 and 209.0 reinterpreted as integers.
	//   * With that shift the declared members reach 0x54 and the assert is
	//     satisfied by padding, so the struct is not merely mis-commented, it is
	//     INCOMPLETE. GNAM lies past the declared end of the record.
	//
	// So the offset is discovered by scanning the record's tail for a triple
	// that behaves like GNAM, using what the tester established: every body in a
	// system shares a system id, planet ids are small and distinct, planets
	// carry parent 0, and a moon's parent equals its planet's id (Luna 3 -> Earth
	// 3). That last one is the clincher and is required before an offset is
	// accepted.
	//
	// Reading past a declared struct means reading memory the type system is not
	// vouching for, so the scan is bounded by VirtualQuery: it never leaves the
	// committed, readable region the form itself sits in.
	// GNAM IS NOT IN THE RUNTIME RECORD. Four searches failed because there was
	// nothing to find: the dump in v0.3.8 showed PNDT objects allocated
	// contiguously with a stride of exactly 0x58, and every word of Jemison's
	// own record accounted for - surfaceTree (0x38), a float (0x40),
	// temperatureCelcius (0x44; 20.0 for Jemison, -83.0 for Olivas), density
	// (0x48), periAngleInDegrees (0x4C; 186.0), resourceCreationSpeed (0x50) and
	// the form id (0x54). Anything past 0x58 is the NEXT planet's record, which
	// is why the scans kept turning up pointers and float bit patterns.
	//
	// So the hierarchy is parsed out of the ESM instead, where GNAM plainly is.
	// That also makes the mod independent of engine layout entirely, which four
	// builds of memory archaeology argue is worth something.
	//
	// Versions up to 0.16.x cached the parse here between launches. The cache is
	// gone - a runtime-generated file is invisible to mod managers and survives
	// an uninstall as clutter, and the parse it saved measures in the hundreds
	// of milliseconds on a background thread nothing waits for. The path remains
	// only so a launch can delete what an older version left behind.
	constexpr const char* kBodyTablePath = "Data/SFSE/Plugins/ShipNavPanelBodies.txt";

	// From the record's PlanetType keyword. The full set is exactly these eight -
	// confirmed by listing every KYWD editor id beginning "PlanetType".
	enum class PlanetClass : std::uint8_t
	{
		kUnknown = 0,
		kAsteroid,
		kAsteroidBelt,
		kBarren,
		kGasGiant,
		kHotGasGiant,
		kIce,
		kIceGiant,
		kRock,
	};

	// Matched on the name rather than the "NN" in the editor id, so a plugin
	// numbering its own types differently cannot quietly shift everything.
	PlanetClass ClassFromKeyword(std::string_view a_editorID)
	{
		constexpr std::string_view kPrefix = "PlanetType";
		if (!a_editorID.starts_with(kPrefix))
			return PlanetClass::kUnknown;
		a_editorID.remove_prefix(kPrefix.size());
		while (!a_editorID.empty() && a_editorID.front() >= '0' && a_editorID.front() <= '9')
			a_editorID.remove_prefix(1);

		if (a_editorID == "AsteroidBelt")
			return PlanetClass::kAsteroidBelt;
		if (a_editorID == "Asteroid")
			return PlanetClass::kAsteroid;
		if (a_editorID == "Barren")
			return PlanetClass::kBarren;
		if (a_editorID == "HotGasGiant")
			return PlanetClass::kHotGasGiant;
		if (a_editorID == "GasGiant")
			return PlanetClass::kGasGiant;
		if (a_editorID == "IceGiant")
			return PlanetClass::kIceGiant;
		if (a_editorID == "Ice")
			return PlanetClass::kIce;
		if (a_editorID == "Rock")
			return PlanetClass::kRock;
		return PlanetClass::kUnknown;
	}

	struct BodyEntry
	{
		GalaxyData  galaxy;
		std::string name;
		PlanetClass planetClass{ PlanetClass::kUnknown };
		// False for bodies the game generates rather than authors. They are kept
		// for their place in the hierarchy - a body the HUD offers may be one of
		// them - but never listed in their own right.
		bool authored{ true };
		// Somewhere on this body is a major settlement. Comes from the LCTN
		// group rather than from the planet record, which names no location -
		// see MarkSettlements.
		bool settled{ false };
	};

	std::mutex                                   g_bodyTableMutex;
	std::unordered_map<std::uint32_t, BodyEntry> g_bodyTable;
	// Same entries indexed by star system, which is what listing a whole system
	// needs - and the HUD only ever offers a handful of a system's bodies.
	std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_bodiesBySystem;
	std::atomic<bool>                                             g_bodyTableLoaded{ false };
	constexpr std::size_t                         kGalaxyScanFirst = 0x38;

	std::size_t ReadableBytesFrom(const void* a_base, std::size_t a_want)
	{
		::MEMORY_BASIC_INFORMATION info{};
		if (::VirtualQuery(a_base, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
			return 0;

		constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ((info.Protect & kReadable) == 0 || (info.Protect & PAGE_GUARD) != 0)
			return 0;

		const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
		const auto available = regionEnd - reinterpret_cast<std::uintptr_t>(a_base);
		return static_cast<std::size_t>(std::min<std::uintptr_t>(available, a_want));
	}

	const RE::TESForm* LookupPlanet(std::uint32_t a_formID)
	{
		const auto form = RE::TESForm::LookupByID(a_formID);
		if (!form || form->GetFormType() != RE::FormType::kPNDT)
			return nullptr;
		return form;
	}

	// One VirtualQuery per form, then a plain memory copy. The scan works over
	// these snapshots rather than probing the live object once per candidate
	// offset - the difference between five page queries and several hundred.
	std::vector<std::byte> SnapshotForm(const RE::TESForm* a_form, std::size_t a_want)
	{
		const auto* base = reinterpret_cast<const std::byte*>(a_form);
		const auto  readable = ReadableBytesFrom(base, a_want);
		std::vector<std::byte> bytes;
		if (readable >= kGalaxyScanFirst + 12) {
			bytes.resize(readable);
			std::memcpy(bytes.data(), base, readable);
		}
		return bytes;
	}

	// ---------------------------------------------------------------------------
	// Reading the hierarchy out of the master file.
	//
	// GNAM is not in the runtime record, but it is plainly in the ESM - so the
	// plugin reads it there itself rather than asking anyone to generate a
	// table. Validated offline against the tester's own layout before a line of
	// this was written: all 1765 PNDT records parse, Jemison comes back planet 3
	// with no parent, and Kurtz comes back parent 3.
	//
	// The file is only walked as far as the PNDT group. Top-level groups carry
	// their own size, so reaching it is a hundred seeks rather than a 1.4 GB
	// read, and the result is cached beside the plugin so even that happens once
	// rather than once per launch.
	// ---------------------------------------------------------------------------

	constexpr std::uint32_t kRecordCompressed = 0x00040000;

	// One loaded plugin, as the game has it. Shattered Space adds planets
	// (Va'ruun'kai among them), so reading Starfield.esm alone misses whole
	// systems - the whole load order has to be walked.
	struct PluginInfo
	{
		std::string  name;
		std::uint8_t index{ 0 };
	};

	// The load order in the game's own words, which is the only account of it
	// that is certainly right: Plugins.txt can be redirected by a mod manager,
	// and reconstructing index rules by hand is how this sort of thing goes
	// quietly wrong.
	//
	// TESFile's offsets are hand-mapped in CommonLibSF and this project has been
	// bitten twice by stale layouts, so the names are sanity-checked before any
	// of them is trusted. A failure here costs nesting, not correctness.
	std::vector<PluginInfo> CollectPlugins()
	{
		std::vector<PluginInfo> plugins;

		const auto handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			REX::WARN("[bodies] no data handler - falling back to Starfield.esm alone");
			return plugins;
		}

		for (const auto* file : handler->compiledFileCollection.files) {
			if (!file)
				continue;

			const char* raw = file->fileName;
			const auto  length = ::strnlen(raw, sizeof(file->fileName));
			if (length == 0 || length >= sizeof(file->fileName))
				break;

			std::string name{ raw, length };
			const bool  looksLikePlugin = name.ends_with(".esm") || name.ends_with(".esp") ||
			                             name.ends_with(".esl");
			if (!looksLikePlugin) {
				REX::WARN("[bodies] plugin list gave '{}', which is not a plugin name - the file layout "
						  "is not what was expected, so only Starfield.esm will be read",
					name);
				return {};
			}

			plugins.push_back(PluginInfo{ std::move(name), file->compileIndex });
		}

		return plugins;
	}

	struct RecordHeader
	{
		char          signature[4];
		std::uint32_t dataSize;
		std::uint32_t flagsOrLabel;  // a GRUP keeps its label here
		std::uint32_t formID;
		std::uint32_t unk10;
		std::uint32_t unk14;
	};
	static_assert(sizeof(RecordHeader) == 24);

	bool ReadExact(std::ifstream& a_file, void* a_dest, std::size_t a_size)
	{
		a_file.read(reinterpret_cast<char*>(a_dest), static_cast<std::streamsize>(a_size));
		return a_file.gcount() == static_cast<std::streamsize>(a_size);
	}

	// "BondarPlanetData" -> "Bondar". The record's FULL is a localised string id
	// rather than text, so the editor id is all there is - and for an authored
	// body it is simply the name with a suffix.
	//
	// The suffix is REQUIRED, not merely stripped when present. Across
	// Starfield.esm the split is exact: 1693 records end in "PlanetData", 72
	// begin with an underscore (the game's own generated markers -
	// _TheEyePlanetData, _JemisonGravJumpArrivalPlanetData), and none are
	// neither. So the convention identifies an authored name reliably, and
	// anything outside it is an internal id that has no business in the panel -
	// a plugin in the tester's load order offered "scSFTERLC03UnifierOrbital",
	// which the earlier rule passed through raw and displayed.
	//
	// Returning nothing is not the same as discarding the body: it stays in the
	// table so its place in the hierarchy can still be read. It is only kept out
	// of the list, and if the HUD offers it, the HUD names it properly.
	// Whether this is a body the game AUTHORED, as opposed to one it generates
	// to hang something off - an orbital marker, a grav-jump arrival point, a
	// station's anchor. Only authored bodies belong in the list.
	//
	// This is deliberately kept apart from whether a name is available. v0.5.0
	// conflated the two - a generated body had no name, so filtering on "no
	// name" happened to filter out generated bodies - and then resolving FULL
	// gave them names and they walked straight into the list. The Eye turned up
	// nested under Jemison as though it were a moon while the station itself
	// was still listed below, which is also a reminder that the two are
	// different forms entirely: the HUD offers The Eye as a kREFR, the record is
	// a kPNDT, so no amount of id matching would have caught the duplicate.
	bool IsAuthoredBody(std::string_view a_editorID)
	{
		constexpr std::string_view kSuffix = "PlanetData";
		return !a_editorID.empty() && a_editorID.front() != '_' &&
		       a_editorID.size() > kSuffix.size() && a_editorID.ends_with(kSuffix);
	}

	std::string NameFromEditorID(std::string_view a_editorID)
	{
		constexpr std::string_view kSuffix = "PlanetData";
		if (!IsAuthoredBody(a_editorID))
			return {};
		a_editorID.remove_suffix(kSuffix.size());
		return std::string{ a_editorID };
	}

	// ---------------------------------------------------------------------------
	// Proper names, out of the archives.
	//
	// A record's FULL is a localised string id, and the strings themselves are
	// not loose - they live in "<Plugin> - Localization.ba2". So the name comes
	// from reading that archive and its string table. Both formats were pinned
	// down offline first: the BA2 is BTDX v2 GNRL with a 32-byte header and
	// 36-byte entries, and the table is a count, a data size, id/offset pairs,
	// then null-terminated UTF-8. Verified against known ids before a line of
	// this existed - 43005 is Jemison, 42692 is Kurtz.
	// ---------------------------------------------------------------------------

	constexpr std::size_t kBA2HeaderSize = 32;
	constexpr std::size_t kBA2EntrySize = 36;

	// Pulls one file out of a general-purpose BA2 by name, case-insensitively.
	bool ExtractFromArchive(const std::string& a_archive, const std::string& a_wanted,
		std::vector<std::byte>& a_out)
	{
		std::ifstream file{ a_archive, std::ios::binary };
		if (!file)
			return false;

		char          magic[4]{};
		std::uint32_t version = 0;
		char          type[4]{};
		std::uint32_t count = 0;
		std::uint64_t nameTableOffset = 0;
		if (!ReadExact(file, magic, 4) || !ReadExact(file, &version, 4) || !ReadExact(file, type, 4) ||
			!ReadExact(file, &count, 4) || !ReadExact(file, &nameTableOffset, 8))
			return false;
		if (std::memcmp(magic, "BTDX", 4) != 0 || std::memcmp(type, "GNRL", 4) != 0 || count == 0)
			return false;

		// Names sit at the tail, in the same order as the entries.
		file.seekg(static_cast<std::streamoff>(nameTableOffset), std::ios::beg);
		std::size_t wantedIndex = count;
		for (std::uint32_t i = 0; i < count; ++i) {
			std::uint16_t length = 0;
			if (!ReadExact(file, &length, 2))
				return false;
			std::string name(length, '\0');
			if (length != 0 && !ReadExact(file, name.data(), length))
				return false;
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
			if (name == a_wanted) {
				wantedIndex = i;
				break;
			}
		}
		if (wantedIndex >= count)
			return false;

		file.seekg(static_cast<std::streamoff>(kBA2HeaderSize + wantedIndex * kBA2EntrySize), std::ios::beg);
		std::uint32_t skip[4]{};
		std::uint64_t offset = 0;
		std::uint32_t packed = 0;
		std::uint32_t unpacked = 0;
		if (!ReadExact(file, skip, sizeof(skip)) || !ReadExact(file, &offset, 8) ||
			!ReadExact(file, &packed, 4) || !ReadExact(file, &unpacked, 4))
			return false;
		if (unpacked == 0 || unpacked > 64u * 1024u * 1024u)
			return false;

		std::vector<std::byte> raw(packed != 0 ? packed : unpacked);
		file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		if (!ReadExact(file, raw.data(), raw.size()))
			return false;

		if (packed == 0) {
			a_out = std::move(raw);
			return true;
		}

		a_out.resize(unpacked);
		uLongf produced = unpacked;
		return ::uncompress(reinterpret_cast<Bytef*>(a_out.data()), &produced,
				   reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size())) == Z_OK &&
		       produced == unpacked;
	}

	// count, data size, then id/offset pairs, then the text.
	void ParseStringTable(const std::vector<std::byte>& a_data,
		std::unordered_map<std::uint32_t, std::string>&  a_out)
	{
		if (a_data.size() < 8)
			return;
		std::uint32_t count = 0;
		std::memcpy(&count, a_data.data(), 4);
		if (count == 0 || count > 1'000'000)
			return;

		const std::size_t directory = 8;
		const std::size_t text = directory + static_cast<std::size_t>(count) * 8;
		if (text > a_data.size())
			return;

		for (std::uint32_t i = 0; i < count; ++i) {
			std::uint32_t id = 0;
			std::uint32_t offset = 0;
			std::memcpy(&id, a_data.data() + directory + i * 8, 4);
			std::memcpy(&offset, a_data.data() + directory + i * 8 + 4, 4);

			const std::size_t start = text + offset;
			if (start >= a_data.size())
				continue;
			const auto* chars = reinterpret_cast<const char*>(a_data.data());
			const auto  length = ::strnlen(chars + start, a_data.size() - start);
			if (length != 0)
				a_out.emplace(id, std::string{ chars + start, length });
		}
	}

	// "Starfield.esm" -> the strings from "Starfield - Localization.ba2".
	// Language follows the archive: English is tried first, and whatever the
	// archive actually carries is used otherwise, so a non-English install gets
	// its own names rather than nothing.
	void LoadPluginStrings(const std::string& a_plugin,
		std::unordered_map<std::uint32_t, std::string>& a_out)
	{
		auto base = a_plugin;
		if (const auto dot = base.rfind('.'); dot != std::string::npos)
			base.erase(dot);

		const auto archive = std::format("Data/{} - Localization.ba2", base);
		if (!std::filesystem::exists(archive))
			return;

		auto lowered = base;
		std::transform(lowered.begin(), lowered.end(), lowered.begin(),
			[](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });

		static constexpr const char* kLanguages[]{ "en", "de", "fr", "es", "esmx", "it", "ja", "pl", "ptbr",
			"zhhans" };

		std::vector<std::byte> data;
		for (const auto* language : kLanguages) {
			if (ExtractFromArchive(archive, std::format("strings/{}_{}.strings", lowered, language), data)) {
				const auto before = a_out.size();
				ParseStringTable(data, a_out);
				REX::INFO("[bodies] {} names from {} ({})", a_out.size() - before, archive, language);
				return;
			}
		}
	}

	// Walks a record's subrecords, handing each signature and payload to the
	// callback; returning false from it stops the walk.
	//
	// One copy of this rather than one per record type, because of the XXXX rule
	// below: it is invisible when missed - the walk desyncs into the middle of a
	// payload and simply finds fewer records - and it cost 1134 of 1765 records
	// the one time it was.
	template <class F>
	void ForEachSubrecord(const std::byte* a_data, std::size_t a_size, F&& a_fn)
	{
		std::size_t   offset = 0;
		std::uint32_t pending = 0;

		while (offset + 6 <= a_size) {
			char sig[4];
			std::memcpy(sig, a_data + offset, 4);
			std::uint16_t size16 = 0;
			std::memcpy(&size16, a_data + offset + 4, 2);
			offset += 6;

			std::uint32_t size = size16;

			// XXXX carries the real 32-bit length of the NEXT subrecord, whose
			// own size field then reads zero.
			if (std::memcmp(sig, "XXXX", 4) == 0 && size == 4) {
				if (offset + 4 > a_size)
					return;
				std::memcpy(&pending, a_data + offset, 4);
				offset += 4;
				continue;
			}
			if (pending != 0) {
				size = pending;
				pending = 0;
			}
			if (offset + size > a_size)
				return;

			if (!a_fn(std::string_view{ sig, 4 }, a_data + offset, static_cast<std::size_t>(size)))
				return;
			offset += size;
		}
	}

	// Records are routinely zlib-compressed - every PNDT in Starfield.esm is -
	// with the stream starting four bytes in, after a uint32 inflated size.
	// Returns the body to read, which is the raw bytes themselves when the
	// record was never compressed, or nothing if it cannot be inflated.
	std::span<const std::byte> RecordBody(std::uint32_t a_flags, const std::vector<std::byte>& a_raw,
		std::vector<std::byte>& a_scratch, std::size_t a_maxInflated)
	{
		if ((a_flags & kRecordCompressed) == 0)
			return { a_raw.data(), a_raw.size() };
		if (a_raw.size() < 5)
			return {};

		std::uint32_t inflatedSize = 0;
		std::memcpy(&inflatedSize, a_raw.data(), sizeof(inflatedSize));
		if (inflatedSize == 0 || inflatedSize > a_maxInflated)
			return {};

		a_scratch.resize(inflatedSize);
		uLongf produced = inflatedSize;
		if (::uncompress(reinterpret_cast<Bytef*>(a_scratch.data()), &produced,
				reinterpret_cast<const Bytef*>(a_raw.data() + 4),
				static_cast<uLong>(a_raw.size() - 4)) != Z_OK)
			return {};
		return { a_scratch.data(), produced };
	}

	// Walks one record's subrecords looking for GNAM.
	bool FindGnam(const std::byte* a_data, std::size_t a_size, GalaxyData& a_out, std::string& a_name,
		std::uint32_t& a_fullID, bool& a_authored, std::vector<std::uint32_t>& a_keywords)
	{
		bool found = false;

		ForEachSubrecord(a_data, a_size,
			[&](std::string_view a_sig, const std::byte* a_payload, std::size_t a_length) {
				if (a_sig == "EDID" && a_length > 1) {
					const std::string_view editorID{ reinterpret_cast<const char*>(a_payload), a_length - 1 };
					a_authored = IsAuthoredBody(editorID);
					a_name = NameFromEditorID(editorID);
				} else if (a_sig == "FULL" && a_length == 4 && a_fullID == 0) {
					// The localised name's id. It sits inside a component block
					// but reads as an ordinary subrecord, and comes before GNAM.
					std::memcpy(&a_fullID, a_payload, 4);
				} else if (a_sig == "KWDA" && a_length >= 4) {
					// The body's keywords, as form ids. Kept whole and resolved
					// by the caller, the only place that knows the load order.
					a_keywords.assign(a_length / 4, 0u);
					std::memcpy(a_keywords.data(), a_payload, a_keywords.size() * 4);
				} else if (a_sig == "GNAM" && a_length >= 12) {
					// The SIZE CHECK IS LOAD-BEARING, not defensive. A planet
					// record carries TWO subrecords called GNAM - a 4-byte float
					// earlier on and the 12-byte galaxy data later - and
					// signatures are reused freely between component blocks
					// (FNAM and CNAM appear twice each too). Matching on the
					// signature alone would take the float and read a hierarchy
					// out of nonsense.
					std::uint32_t values[3];
					std::memcpy(values, a_payload, sizeof(values));
					a_out = GalaxyData{ values[0], values[1], values[2] };
					found = true;
					return false;  // EDID precedes GNAM, so the name is in hand
				}
				return true;
			});

		return found;
	}

	// A record's file-local form id keeps its owner in the top byte: an index
	// into this file's master list, where "one past the last master" means the
	// file itself. Turning that into the runtime id is a matter of swapping that
	// index for the owner's place in the load order.
	std::uint32_t ResolveFormID(std::uint32_t a_localID, const std::vector<std::uint8_t>& a_masterIndices,
		std::uint8_t a_selfIndex)
	{
		const auto slot = static_cast<std::size_t>(a_localID >> 24);
		const auto owner = slot < a_masterIndices.size() ? a_masterIndices[slot] : a_selfIndex;
		return (static_cast<std::uint32_t>(owner) << 24) | (a_localID & 0x00FFFFFFu);
	}

	// Seeks to a top-level group and returns where it ends, or 0. The file is
	// left positioned at the group's first record.
	std::uint64_t SeekGroup(std::ifstream& a_file, const char (&a_label)[5])
	{
		while (a_file) {
			const auto   start = static_cast<std::uint64_t>(a_file.tellg());
			RecordHeader group{};
			if (!ReadExact(a_file, &group, sizeof(group)) || std::memcmp(group.signature, "GRUP", 4) != 0)
				return 0;
			if (std::memcmp(&group.flagsOrLabel, a_label, 4) == 0)
				return start + group.dataSize;
			a_file.seekg(static_cast<std::streamoff>(start + group.dataSize), std::ios::beg);
		}
		return 0;
	}

	// The keyword that marks a location as a major settlement. Resolved by
	// editor id rather than hardcoded, for the same reason the PlanetType
	// keywords are: a form id is a fact about one load order, a name is a fact
	// about the game. In the base game it comes out as 0x00022611, which the
	// log prints so a change is visible rather than silent.
	constexpr std::string_view kSettlementKeyword = "LocTypeSettlement";

	// The eight PlanetType keywords, by runtime form id, plus the settlement
	// keyword's id. Everything else in the group is skipped, so this stays tiny
	// however many keywords a game has.
	void ParsePluginKeywords(const std::string& a_path, const std::vector<std::uint8_t>& a_masterIndices,
		std::uint8_t a_selfIndex, std::unordered_map<std::uint32_t, PlanetClass>& a_out,
		std::uint32_t& a_settlementKeyword)
	{
		std::ifstream file{ a_path, std::ios::binary };
		if (!file)
			return;

		RecordHeader header{};
		if (!ReadExact(file, &header, sizeof(header)) || std::memcmp(header.signature, "TES4", 4) != 0)
			return;
		file.seekg(header.dataSize, std::ios::cur);

		const auto groupEnd = SeekGroup(file, "KYWD");
		if (groupEnd == 0)
			return;

		std::vector<std::byte> raw;
		std::vector<std::byte> inflated;

		while (file && static_cast<std::uint64_t>(file.tellg()) + sizeof(RecordHeader) < groupEnd) {
			const auto   start = static_cast<std::uint64_t>(file.tellg());
			RecordHeader record{};
			if (!ReadExact(file, &record, sizeof(record)))
				return;
			if (std::memcmp(record.signature, "GRUP", 4) == 0) {
				file.seekg(static_cast<std::streamoff>(start + record.dataSize), std::ios::beg);
				continue;
			}
			if (std::memcmp(record.signature, "KYWD", 4) != 0) {
				file.seekg(record.dataSize, std::ios::cur);
				continue;
			}

			raw.resize(record.dataSize);
			if (record.dataSize != 0 && !ReadExact(file, raw.data(), raw.size()))
				return;

			const auto body = RecordBody(record.flagsOrLabel, raw, inflated, 1024u * 1024u);

			// EDID is the first subrecord on a keyword, so there is no need to
			// walk the rest.
			if (body.size() < 7 || std::memcmp(body.data(), "EDID", 4) != 0)
				continue;
			std::uint16_t size = 0;
			std::memcpy(&size, body.data() + 4, 2);
			if (size <= 1 || 6u + size > body.size())
				continue;

			const std::string_view editorID{ reinterpret_cast<const char*>(body.data()) + 6, size - 1u };

			if (const auto planetClass = ClassFromKeyword(editorID); planetClass != PlanetClass::kUnknown) {
				a_out.insert_or_assign(ResolveFormID(record.formID, a_masterIndices, a_selfIndex), planetClass);
			} else if (editorID == kSettlementKeyword) {
				a_settlementKeyword = ResolveFormID(record.formID, a_masterIndices, a_selfIndex);
				REX::INFO("[bodies] {} is {:08X}", kSettlementKeyword, a_settlementKeyword);
			}
		}
	}

	bool ParsePluginBodies(const std::string& a_path, const std::vector<std::uint8_t>& a_masterIndices,
		std::uint8_t a_selfIndex, bool a_validate, const std::unordered_map<std::uint32_t, std::string>& a_strings,
		const std::unordered_map<std::uint32_t, PlanetClass>& a_keywords,
		std::unordered_map<std::uint32_t, BodyEntry>& a_out,
		std::vector<std::string>* a_masterNames)
	{
		std::ifstream file{ a_path, std::ios::binary };
		if (!file)
			return false;

		RecordHeader header{};
		if (!ReadExact(file, &header, sizeof(header)) || std::memcmp(header.signature, "TES4", 4) != 0)
			return false;

		// The header names this file's masters, in the order its own form ids
		// index them.
		if (a_masterNames) {
			std::vector<std::byte> headerData(header.dataSize);
			if (header.dataSize != 0 && !ReadExact(file, headerData.data(), headerData.size()))
				return false;

			std::size_t offset = 0;
			while (offset + 6 <= headerData.size()) {
				char sig[4];
				std::memcpy(sig, headerData.data() + offset, 4);
				std::uint16_t size = 0;
				std::memcpy(&size, headerData.data() + offset + 4, 2);
				offset += 6;
				if (offset + size > headerData.size())
					break;
				if (std::memcmp(sig, "MAST", 4) == 0 && size > 1) {
					a_masterNames->emplace_back(reinterpret_cast<const char*>(headerData.data() + offset),
						::strnlen(reinterpret_cast<const char*>(headerData.data() + offset), size));
				}
				offset += size;
			}
			return true;  // header pass only; the caller comes back for the records
		}

		file.seekg(header.dataSize, std::ios::cur);

		// Top-level groups, until the planets.
		std::uint64_t groupEnd = 0;
		while (file) {
			const auto   groupStart = static_cast<std::uint64_t>(file.tellg());
			RecordHeader group{};
			if (!ReadExact(file, &group, sizeof(group)) || std::memcmp(group.signature, "GRUP", 4) != 0)
				break;
			// A group's size counts its own header; its label sits where a
			// record keeps its flags.
			if (std::memcmp(&group.flagsOrLabel, "PNDT", 4) == 0) {
				groupEnd = groupStart + group.dataSize;
				break;
			}
			file.seekg(static_cast<std::streamoff>(groupStart + group.dataSize), std::ios::beg);
		}
		if (groupEnd == 0)
			return true;  // a plugin with no planets is perfectly normal

		std::vector<std::byte> raw;
		std::vector<std::byte> inflated;
		std::size_t            records = 0;

		while (file && static_cast<std::uint64_t>(file.tellg()) + sizeof(RecordHeader) < groupEnd) {
			const auto   recordStart = static_cast<std::uint64_t>(file.tellg());
			RecordHeader record{};
			if (!ReadExact(file, &record, sizeof(record)))
				break;

			if (std::memcmp(record.signature, "GRUP", 4) == 0) {
				file.seekg(static_cast<std::streamoff>(recordStart + record.dataSize), std::ios::beg);
				continue;
			}
			if (std::memcmp(record.signature, "PNDT", 4) != 0) {
				file.seekg(record.dataSize, std::ios::cur);
				continue;
			}

			raw.resize(record.dataSize);
			if (record.dataSize != 0 && !ReadExact(file, raw.data(), raw.size()))
				break;

			const auto body = RecordBody(record.flagsOrLabel, raw, inflated, 64u * 1024u * 1024u);

			GalaxyData data;
			std::string                name;
			std::uint32_t              fullID = 0;
			bool                       authored = false;
			std::vector<std::uint32_t> keywords;
			if (!FindGnam(body.data(), body.size(), data, name, fullID, authored, keywords))
				continue;

			auto planetClass = PlanetClass::kUnknown;
			for (const auto keyword : keywords) {
				const auto found =
					a_keywords.find(ResolveFormID(keyword, a_masterIndices, a_selfIndex));
				if (found != a_keywords.end()) {
					planetClass = found->second;
					break;
				}
			}

			// A real, localised name beats one derived from the editor id - but
			// having a name says nothing about whether the body belongs in the
			// list, which is what `authored` is for.
			if (fullID != 0) {
				if (const auto found = a_strings.find(fullID); found != a_strings.end())
					name = found->second;
			}

			const auto runtimeID = ResolveFormID(record.formID, a_masterIndices, a_selfIndex);

			// Every id is checked against the game before it is kept. If the
			// load-order arithmetic or a hand-mapped offset is wrong, this
			// yields nothing rather than something plausible and false - which,
			// on the evidence of this project, is the failure mode that costs
			// the least time.
			if (a_validate) {
				const auto form = RE::TESForm::LookupByID(runtimeID);
				if (!form || form->GetFormType() != RE::FormType::kPNDT)
					continue;
			}

			// Later plugins override earlier ones, which is what the load order
			// means.
			a_out.insert_or_assign(runtimeID, BodyEntry{ data, std::move(name), planetClass, authored });
			++records;
		}

		if (records != 0)
			REX::INFO("[bodies] {} bodies from {}", records, a_path);
		return true;
	}

	// ---------------------------------------------------------------------------
	// Settlements, out of the location tree.
	//
	// Nothing on a planet record names a location, and nothing on a worldspace
	// names a planet - both were checked and ruled out. The join runs the other
	// way and by id, not by name: an LCTN carries `XNAM` (Star ID) and `YNAM`
	// (Planet ID), which are the same two numbers as a body's GNAM. Settlement
	// locations leave both empty, so the body is found by climbing `PNAM` to the
	// ancestor that fills them in:
	//
	//     CityNewAtlantisLocation                     (LocTypeSettlement)
	//       -> SAlphaCentauri_PJemison_Surface        XNAM 71456, YNAM 3
	//         -> SAlphaCentauri_PJemison
	//           -> SAlphaCentauri
	//             -> Universe
	//
	// and Jemison is system 71456, planet 3.
	// ---------------------------------------------------------------------------

	// One location, as far as this mod cares.
	struct LocationEntry
	{
		std::uint32_t parent{ 0 };    // PNAM, as a runtime form id
		std::uint32_t starID{ 0 };    // XNAM
		std::uint32_t planetID{ 0 };  // YNAM
		std::string   editorID;
		bool          settlement{ false };
	};

	void ParsePluginLocations(const std::string& a_path, const std::vector<std::uint8_t>& a_masterIndices,
		std::uint8_t a_selfIndex, std::uint32_t a_settlementKeyword,
		std::unordered_map<std::uint32_t, LocationEntry>& a_out)
	{
		if (a_settlementKeyword == 0)
			return;  // nothing to look for yet

		std::ifstream file{ a_path, std::ios::binary };
		if (!file)
			return;

		RecordHeader header{};
		if (!ReadExact(file, &header, sizeof(header)) || std::memcmp(header.signature, "TES4", 4) != 0)
			return;
		file.seekg(header.dataSize, std::ios::cur);

		const auto groupEnd = SeekGroup(file, "LCTN");
		if (groupEnd == 0)
			return;  // a plugin with no locations is perfectly normal

		std::vector<std::byte> raw;
		std::vector<std::byte> inflated;

		while (file && static_cast<std::uint64_t>(file.tellg()) + sizeof(RecordHeader) < groupEnd) {
			const auto   start = static_cast<std::uint64_t>(file.tellg());
			RecordHeader record{};
			if (!ReadExact(file, &record, sizeof(record)))
				return;
			if (std::memcmp(record.signature, "GRUP", 4) == 0) {
				file.seekg(static_cast<std::streamoff>(start + record.dataSize), std::ios::beg);
				continue;
			}
			if (std::memcmp(record.signature, "LCTN", 4) != 0) {
				file.seekg(record.dataSize, std::ios::cur);
				continue;
			}

			raw.resize(record.dataSize);
			if (record.dataSize != 0 && !ReadExact(file, raw.data(), raw.size()))
				return;

			const auto body = RecordBody(record.flagsOrLabel, raw, inflated, 4u * 1024u * 1024u);

			LocationEntry entry;
			bool          hasPlanet = false;

			ForEachSubrecord(body.data(), body.size(),
				[&](std::string_view a_sig, const std::byte* a_payload, std::size_t a_length) {
					if (a_sig == "EDID" && a_length > 1) {
						entry.editorID.assign(reinterpret_cast<const char*>(a_payload), a_length - 1);
					} else if (a_sig == "PNAM" && a_length == 4) {
						std::uint32_t parent = 0;
						std::memcpy(&parent, a_payload, 4);
						entry.parent = ResolveFormID(parent, a_masterIndices, a_selfIndex);
					} else if (a_sig == "XNAM" && a_length == 4) {
						std::memcpy(&entry.starID, a_payload, 4);
					} else if (a_sig == "YNAM" && a_length == 4) {
						std::memcpy(&entry.planetID, a_payload, 4);
						hasPlanet = true;
					} else if (a_sig == "KWDA" && a_length >= 4) {
						for (std::size_t at = 0; at + 4 <= a_length; at += 4) {
							std::uint32_t keyword = 0;
							std::memcpy(&keyword, a_payload + at, 4);
							if (ResolveFormID(keyword, a_masterIndices, a_selfIndex) == a_settlementKeyword)
								entry.settlement = true;
						}
					}
					return true;
				});

			const auto runtimeID = ResolveFormID(record.formID, a_masterIndices, a_selfIndex);
			auto&      slot = a_out[runtimeID];

			// Merged field by field rather than replaced wholesale, which is the
			// OPPOSITE of the rule the body table uses, and deliberate.
			//
			// An override replaces a record entirely, so a version that simply
			// omits the keyword would drop the marking - and the tester has
			// exactly that, base records carrying `LocTypeSettlement` with an
			// override that does not, which xEdit flags yellow. Settlement is
			// therefore the union across every version of the location. The ids
			// take the last version that states one, since a blank there means
			// "not said here" far more often than it means "deliberately none".
			slot.settlement = slot.settlement || entry.settlement;
			if (!entry.editorID.empty())
				slot.editorID = std::move(entry.editorID);
			if (entry.parent != 0)
				slot.parent = entry.parent;
			if (hasPlanet && entry.planetID != 0) {
				// Taken as a pair: a star id of zero is meaningful (Sol) but only
				// alongside the planet id that came with it.
				slot.starID = entry.starID;
				slot.planetID = entry.planetID;
			}
		}
	}

	std::uint64_t BodyKey(std::uint32_t a_systemID, std::uint32_t a_planetID)
	{
		return (static_cast<std::uint64_t>(a_systemID) << 32) | a_planetID;
	}

	// Climbs from every settlement to the body it sits on and marks it.
	void MarkSettlements(const std::unordered_map<std::uint32_t, LocationEntry>& a_locations,
		std::unordered_map<std::uint32_t, BodyEntry>& a_bodies)
	{
		std::unordered_set<std::uint64_t> settled;
		std::size_t                       settlements = 0;
		std::size_t                       unresolved = 0;

		for (const auto& [id, location] : a_locations) {
			if (!location.settlement)
				continue;
			++settlements;

			// Four or five deep in practice, rooted at Universe. The hop cap is
			// what guards against a cycle, so no set of visited ids is needed.
			const LocationEntry* at = &location;
			for (int hop = 0; hop < 16; ++hop) {
				// A planet id of zero means this location is above the planets -
				// a system, or the universe. Note that the STAR id may
				// legitimately be zero: Sol IS system 0, so requiring both to be
				// non-zero would quietly lose every settlement in the home
				// system, Cydonia and New Homestead among them.
				if (at->planetID != 0) {
					settled.insert(BodyKey(at->starID, at->planetID));
					break;
				}
				const auto found = a_locations.find(at->parent);
				if (at->parent == 0 || found == a_locations.end()) {
					// Ran out of chain without reaching a body. Expected for
					// interiors that hang off no planet at all.
					++unresolved;
					break;
				}
				at = &found->second;
			}
		}

		std::size_t              marked = 0;
		std::vector<std::string> named;
		for (auto& [formID, body] : a_bodies) {
			if (!settled.contains(BodyKey(body.galaxy.systemID, body.galaxy.planetID)))
				continue;
			body.settled = true;
			++marked;
			if (body.authored && !body.name.empty() && named.size() < 12)
				named.push_back(body.name);
		}

		// A handful of names in the log is what makes a bad chain obvious: a
		// count alone looks equally healthy whether it found the cities or a
		// dozen asteroids.
		std::sort(named.begin(), named.end());
		std::string sample;
		for (const auto& name : named)
			sample += (sample.empty() ? ": " : ", ") + name;

		REX::INFO("[bodies] {} locations, {} settlements ({} reached no body), {} settled bodies{}",
			a_locations.size(), settlements, unresolved, marked, sample);
	}

	// Walks the whole load order. Falls back to the master alone if the plugin
	// list could not be read, which keeps the base game working regardless.
	bool ParseAllBodies(std::unordered_map<std::uint32_t, BodyEntry>& a_out)
	{
		auto plugins = CollectPlugins();
		bool validate = true;

		if (plugins.empty()) {
			plugins.push_back(PluginInfo{ "Starfield.esm", 0 });
			validate = false;  // no runtime list means no forms to check against
		}

		// Matched case-insensitively: a file's MAST entries carry whatever case
		// its author typed - Shattered Space names its master "starfield.esm"
		// while the game calls it "Starfield.esm" - and an exact comparison
		// silently fails to resolve every override record in the file.
		const auto fold = [](std::string a_text) {
			std::transform(a_text.begin(), a_text.end(), a_text.begin(),
				[](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
			return a_text;
		};

		// Shared across the whole order, since a plugin's bodies routinely use
		// the base game's keywords.
		std::unordered_map<std::uint32_t, PlanetClass> keywords;
		std::uint32_t                                  settlementKeyword = 0;

		// Likewise shared: a settlement's chain up to its planet can cross
		// plugins, so nothing can be resolved until the whole order is read.
		std::unordered_map<std::uint32_t, LocationEntry> locations;

		std::unordered_map<std::string, std::uint8_t> indexByName;
		for (const auto& plugin : plugins)
			indexByName.emplace(fold(plugin.name), plugin.index);

		for (const auto& plugin : plugins) {
			const auto path = std::format("Data/{}", plugin.name);

			std::vector<std::string>                      masterNames;
			std::unordered_map<std::uint32_t, std::string> strings;
			if (!ParsePluginBodies(path, {}, plugin.index, false, strings, {}, a_out, &masterNames)) {
				REX::WARN("[bodies] could not read {}", path);
				continue;
			}

			// Only worth opening the archive for a plugin that has planets, and
			// only that plugin's own strings: the ids are per-file.
			LoadPluginStrings(plugin.name, strings);

			// A master this file names but the game has not loaded cannot be
			// resolved; index 0 is a harmless placeholder because any record
			// pointing at it will fail validation.
			std::vector<std::uint8_t> masterIndices;
			masterIndices.reserve(masterNames.size());
			for (const auto& master : masterNames) {
				const auto found = indexByName.find(fold(master));
				masterIndices.push_back(found != indexByName.end() ? found->second : std::uint8_t{ 0 });
			}

			// Keywords first: a body's class is one of them, a location's
			// settlement marking is another, and they may belong to any file in
			// the order.
			ParsePluginKeywords(path, masterIndices, plugin.index, keywords, settlementKeyword);
			ParsePluginBodies(path, masterIndices, plugin.index, validate, strings, keywords, a_out, nullptr);
			ParsePluginLocations(path, masterIndices, plugin.index, settlementKeyword, locations);
		}

		REX::INFO("[bodies] read {} bodies from {} plugin(s)", a_out.size(), plugins.size());

		// Only once the whole order is in hand: a settlement in one plugin can
		// climb through a location another one added.
		if (settlementKeyword != 0)
			MarkSettlements(locations, a_out);
		else
			REX::WARN("[bodies] no {} keyword found - settlements will not be marked", kSettlementKeyword);

		return !a_out.empty();
	}

	// Off the main thread: reaching the planets means seeking most of the way
	// through a 1.4 GB file and inflating ~1700 records - a few hundred
	// milliseconds the game should not be made to wait for. Nesting simply
	// starts working shortly after load. The parse runs every launch: versions
	// up to 0.16.x cached it to ShipNavPanelBodies.txt, but a runtime-generated
	// file is invisible to mod managers and outlives an uninstall, and the
	// saving never justified that. The log prints the measured duration.
	void LoadBodyTable()
	{
		if (g_bodyTableLoaded.exchange(true, std::memory_order_acq_rel))
			return;

		std::thread{ [] {
			// One launch of a version that no longer writes the cache is enough
			// to clean up what an older one left; absence is the normal case and
			// stays silent. The file has no reader anymore, so removing even a
			// hand-made one (tools/ExportBodies.pas could write it) loses nothing.
			std::error_code stale;
			if (std::filesystem::remove(kBodyTablePath, stale))
				REX::INFO("[bodies] removed the obsolete cache {}", kBodyTablePath);

			const auto parseStarted = std::chrono::steady_clock::now();

			std::unordered_map<std::uint32_t, BodyEntry> table;
			if (!ParseAllBodies(table)) {
				REX::WARN("[bodies] could not read the planet hierarchy - moons will not be nested");
				return;
			}

			const auto parseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - parseStarted)
			                         .count();
			REX::INFO("[bodies] load-order parse took {} ms ({} bodies)", parseMs, table.size());

			std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> bySystem;
			for (const auto& [formID, entry] : table)
				bySystem[entry.galaxy.systemID].push_back(formID);

			// Parent before child, then by planet id, so a system reads as a
			// tree without any sorting at display time.
			for (auto& [system, ids] : bySystem) {
				std::sort(ids.begin(), ids.end(), [&table](std::uint32_t a_lhs, std::uint32_t a_rhs) -> bool {
					const GalaxyData& lhs = table.at(a_lhs).galaxy;
					const GalaxyData& rhs = table.at(a_rhs).galaxy;
					const std::uint32_t lhsRoot = lhs.parentPlanetID ? lhs.parentPlanetID : lhs.planetID;
					const std::uint32_t rhsRoot = rhs.parentPlanetID ? rhs.parentPlanetID : rhs.planetID;
					if (lhsRoot != rhsRoot)
						return lhsRoot < rhsRoot;
					if ((lhs.parentPlanetID == 0) != (rhs.parentPlanetID == 0))
						return lhs.parentPlanetID == 0;
					return lhs.planetID < rhs.planetID;
				});
			}

			std::lock_guard lock{ g_bodyTableMutex };
			g_bodyTable = std::move(table);
			g_bodiesBySystem = std::move(bySystem);
		} }.detach();
	}

	bool ReadGalaxyData(std::uint32_t a_formID, GalaxyData& a_out)
	{
		std::lock_guard lock{ g_bodyTableMutex };
		const auto      hit = g_bodyTable.find(a_formID);
		if (hit == g_bodyTable.end())
			return false;
		a_out = hit->second.galaxy;
		return true;
	}

	std::mutex             g_candidateMutex;
	std::vector<Candidate> g_candidates;

	// Two selections, deliberately distinct. `locked` is the committed one: it
	// is what the arrow points at once the panel is closed, and it only changes
	// when the confirm key is pressed. `highlight` is the browse cursor, live
	// only while the panel is open. Closing the panel without confirming throws
	// the highlight away, which is what makes "clear without picking another"
	// possible at all.
	std::atomic<std::uint32_t> g_lockedID{ 0 };
	std::atomic<std::uint32_t> g_highlightID{ 0 };
	std::atomic<bool>          g_cycleRequested{ false };
	std::atomic<bool>          g_arrowReady{ false };
	std::atomic<bool>          g_arrowFailed{ false };
	std::atomic<std::uint32_t> g_subscribeAttempts{ 0 };
	RE::Scaleform::GFx::Value  g_arrowClip;

	// Cruise state. The scanner key keeps its vanilla job outside cruise, so the
	// panel must stay out of the way there - the gate identified back in Phase 0.
	std::atomic<bool> g_inCruise{ false };

	// Vanilla blip management. The holder is the mod's container for blips it
	// keeps visible while the vanilla one is hidden; the vanilla container's
	// handle is deliberately NOT here - it is timeline-placed art that the
	// reticle's animations can re-create, so it is resolved fresh every tick
	// (PHASE3-BLIP-PLAN.md, section 5).
	RE::Scaleform::GFx::Value g_blipHolder;
	std::atomic<bool>         g_blipHolderReady{ false };
	std::atomic<bool>         g_blipHolderFailed{ false };
	std::atomic<bool>         g_blipHolderBuildInFlight{ false };
	// True while the mod believes it has the vanilla container hidden. Used for
	// transition logging and for restore-on-exit; the hide itself is re-asserted
	// per tick, so a stale false here costs one log line, nothing more.
	std::atomic<bool> g_blipsHidden{ false };

	// True once any on-screen icon has been faded for the selection - i.e. a
	// restore sweep may be owed. Cleared by the sweep.
	std::atomic<bool> g_iconsFaded{ false };

	// The engine's current info target (the E-target), as an index into the
	// feed's target array, captured from the low-frequency payload. -1 when
	// none. The selection-wins-overlap pass exempts its icon: the player's
	// own targeting outranks the panel.
	std::atomic<std::int32_t> g_infoTargetIndex{ -1 };

	// ---------------------------------------------------------------------------
	// ⚠ NO PREDICTION ABOUT WHICH ROWS THE ENGINE WILL TAKE. Dispatch, then AUDIT.
	//
	// Three gates were written here and all three were guesses dressed as rules,
	// each built on the single row that had misbehaved and then generalised:
	//
	//   1. "runtime (FF-prefixed) ids are unusable" - killed by the tester's log,
	//      which showed the engine holding a course on that very id once the
	//      contact was targeted the vanilla way. The number was always right.
	//   2. "`uniqueID` is the wrong field, it wants a body id" - killed by
	//      measurement: the low feed carries no per-entry `uBodyID` at all.
	//   3. "only a celestial body can take a course by id" - killed by the
	//      tester, who had already reported POIs and stations course-locking
	//      fine. A gate that contradicts a direct observation is not a rule.
	//
	// ⚠ The third one REMOVED WORKING FUNCTIONALITY, which is worse than the bug
	// it was meant to contain: a wrong heading is recoverable in one keypress, a
	// capability that is gone is not. **Do not gate this on a theory again.** The
	// engine's answer is readable - `bIsCruiseTargetLock` on the body's own feed
	// entry - so the mod asks, watches, and says plainly when nothing came back.
	//
	// What is actually known: a course on a **sensor contact** does not take (no
	// entry reports it, no orange indicator draws, the ship heads for the system
	// origin), while planets do, and POIs and stations did when they were last
	// tried. A "Sensor Contact" is vanilla's name for a contact it has not
	// resolved yet, so "unresolved" is the obvious next hypothesis - and it stays
	// a hypothesis until the audit has named the failing rows across a few
	// sessions. Gate on the evidence when there is enough of it, not before.
	// ---------------------------------------------------------------------------

	// The id of the last course the mod asked for, held until the engine reports
	// a course - so the report can be compared with the request. 0 when nothing
	// is outstanding, with the steady-clock tick it was asked at.
	//
	// ⚠ THE AUDIT HAS A BLIND SPOT AND IT NEEDS A TIMEOUT TO SEE IT. The only
	// readback is `bIsCruiseTargetLock` on a low-feed ENTRY, so a course the
	// engine puts on something the feed does not carry - the system's own star,
	// for one - reports as *no course at all*, and a comparison that only runs
	// when a course appears would then never run. That is this project's third
	// check-that-could-not-pass, and the second on this very feature: the same
	// timeout was written for the first flight, deleted as scaffolding when the
	// feature looked solved, and is now the only thing that can see the failure.
	// **A diagnostic is not scaffolding just because the happy path stopped
	// needing it.**
	std::atomic<std::uint32_t> g_courseAskedID{ 0 };
	std::atomic<std::int64_t>  g_courseAskedTicks{ 0 };

	// A course-lock dispatch waiting to be made, by body id; 0 means none. The
	// input thread only ever stores here - no VM, no Scaleform, the rule that
	// side of the mod has always kept - and the dispatch itself happens on the UI
	// thread, exactly as the panel sounds do. Vanilla toggles a course by
	// re-sending the same id, so 0 is never a value this needs to carry.
	std::atomic<std::uint32_t> g_pendingCourseID{ 0 };

	// The locked body's feed presence, CONFIRMED since the last movie
	// teardown - the id of the lock that has actually been seen in a live
	// payload. Moon-lock auto-clear is edge-triggered on this: only a
	// present-then-absent moon can clear, so the empty-then-refilling
	// candidate list after a load or map rebuild can never eat a lock, with
	// no timer doing the guarding. Reset with the movie.
	std::atomic<std::uint32_t> g_lockSeenInFeed{ 0 };

	// The game's own generic labels for undiscovered markers ("Starstation",
	// "Asteroids", ...), fetched through the game's own localisation: the SWF
	// maps each uPoiCategory (MapMarkerUtils.MARKER_TYPE_*) to a translation
	// token via GetGenericTypeLocString - "$MapMarkerGenericTypeStation" and
	// friends - and GlobalFunc.SetText resolves such tokens into whatever
	// language the game runs. The mod sets the token on a scratch TextField,
	// reads the translated word back, and caches it per category. This
	// replaced two generations of "learn it off a marker" machinery: the
	// token is available the moment the category is (with the feed entry),
	// needs nothing on screen, and is localisation-correct by construction.
	std::mutex                                     g_genericLabelMutex;
	std::unordered_map<std::uint32_t, std::string> g_genericLabels;
	RE::Scaleform::GFx::Value                      g_translatorField;
	std::atomic<bool>                              g_translatorReady{ false };
	std::atomic<bool>                              g_translatorFailed{ false };

	// uPoiCategory values are MapMarkerUtils.MARKER_TYPE_*, EnumHelper-
	// sequential from 0 (verified: The Eye sampled category 7 = STATION).
	const char* GenericLabelToken(std::uint32_t a_category)
	{
		switch (a_category) {
		case 1:
			return "$MapMarkerGenericTypeLandmark";
		case 2:
			return "$MapMarkerGenericTypeStructure";
		case 3:
			return "$MapMarkerGenericTypeLifeSigns";
		case 4:
			return "$MapMarkerGenericTypeHazard";
		case 5:
			return "$MapMarkerGenericTypeSpaceLandmark";
		case 6:
			return "$MapMarkerGenericTypeShip";
		case 7:
			return "$MapMarkerGenericTypeStation";
		case 8:
			return "$MapMarkerGenericTypeAsteroids";
		default:
			return nullptr;  // NONE, SIMPLE and anything newer: no generic word
		}
	}

	// The faux blip: a real OffScreenIcon instance the mod owns, wearing
	// vanilla's art and driven through the same public methods the reticle
	// calls. It replaces the drawn diamond for planet and star targets; other
	// types keep the diamond, because their icon path reads POI fields the mod
	// cannot fill truthfully. Lives inside the holder; the holder's
	// return-to-container loops skip it by name prefix.
	RE::Scaleform::GFx::Value  g_fauxBlip;
	RE::Scaleform::GFx::Value  g_fauxLow;
	RE::Scaleform::GFx::Value  g_fauxHigh;
	std::atomic<bool>          g_fauxReady{ false };
	std::atomic<bool>          g_fauxFailed{ false };
	std::atomic<bool>          g_fauxBuildInFlight{ false };
	std::atomic<std::uint32_t> g_fauxLastID{ 0 };

	// The panel is open. While this is set the wheel is hidden from the camera
	// and drives the highlight instead.
	std::atomic<bool>          g_panelOpen{ false };
	std::atomic<std::uint32_t> g_suppressedCount{ 0 };
	// Counts everything the camera tap splices out, which since v0.7.3 is the
	// wheel AND the confirm key - so the messages say "input events", not
	// "wheel events". With the confirm key bound to the POV toggle, a confirm
	// press lands in this count, and that is the only visible evidence the
	// splice is working.
	std::atomic<std::uint32_t> g_cameraRemovedCount{ 0 };

	// The drawn list. Row count is fixed at creation - growing it would mean
	// building TextFields from a feed callback, and every AS3 construction is a
	// risk worth taking exactly once, at startup.
	constexpr std::size_t      kPanelMaxRowsHard = 16;
	RE::Scaleform::GFx::Value  g_panelClip;
	RE::Scaleform::GFx::Value  g_panelHighlight;
	// Name and distance are separate fields so one can sit left and the other
	// right - a single field cannot align part of its text.
	RE::Scaleform::GFx::Value g_panelRows[kPanelMaxRowsHard];
	RE::Scaleform::GFx::Value g_panelDists[kPanelMaxRowsHard];
	// One icon clip per row, redrawn only when that row's body changes -
	// `graphics.clear()` was verified to work before this was built on.
	RE::Scaleform::GFx::Value g_panelIcons[kPanelMaxRowsHard];
	PlanetClass               g_panelIconClass[kPanelMaxRowsHard]{};
	bool                      g_panelIconSettled[kPanelMaxRowsHard]{};
	bool                      g_panelIconDrawn[kPanelMaxRowsHard]{};
	// And one VANILLA map icon per row (v0.11.0) - a real DynamicPoiIcon.
	// The key caches the (type, category, state) last driven into the slot,
	// so a refresh tick showing the same thing costs no VM call. A failed
	// class construction latches per movie and the drawn glyphs carry on.
	RE::Scaleform::GFx::Value g_panelPoiIcons[kPanelMaxRowsHard];
	std::uint64_t             g_panelPoiIconKey[kPanelMaxRowsHard]{};
	std::atomic<bool>         g_panelPoiIconsFailed{ false };
	// The giants' icon: the in-POV marker circle with the mod's ring-line
	// child inside it (one scale drives both). Static art - built once,
	// only visibility changes per refresh.
	RE::Scaleform::GFx::Value g_panelGiantIcons[kPanelMaxRowsHard];
	std::atomic<bool>         g_panelGiantIconsFailed{ false };
	// The scrollbar (v0.12.0): a drawn track and thumb down the left edge,
	// shown only while the list outgrows the panel. The thumb is 1 px art
	// scaled to length, so tracking it costs two property writes. And the
	// resolved Shared.GlobalFunc class object, cached per movie for the
	// vanilla-style name truncation (TruncateSingleLineText).
	RE::Scaleform::GFx::Value g_panelScrollTrack;
	RE::Scaleform::GFx::Value g_panelScrollThumb;
	std::atomic<double>       g_panelNameWidth{ 0.0 };
	// The survey mark, per row, in the distance cell (Phase 6). All three clips
	// are drawn ONCE at build time and driven by transform and visibility
	// afterwards - the scrollbar's discipline, and the reason a survey change
	// costs two property writes rather than a redraw:
	//   Bar     the track, full width; parent of the fill
	//   Fill    1 px of art at the bar's left edge, scaleX = the percentage
	//   Banner  the finished plate + four bands, visibility toggled
	// The fill is a CHILD of the bar because a container's own graphics render
	// BELOW its children - the one z-order rule this project has proven - so the
	// fill sits over the track for free and only it is scaled.
	RE::Scaleform::GFx::Value g_panelSurveyBars[kPanelMaxRowsHard];
	RE::Scaleform::GFx::Value g_panelSurveyFills[kPanelMaxRowsHard];
	RE::Scaleform::GFx::Value g_panelSurveyBanners[kPanelMaxRowsHard];
	// Last value driven into each SLOT (not each body - that is what makes
	// scrolling correct). -2 means "never driven", which no percentage is.
	float g_panelSurveyDrawn[kPanelMaxRowsHard]{};
	constexpr float kSurveyNeverDrawn = -2.0f;
	// The toggle marks which sound to play; the UI thread consumes it. The
	// input thread must never enter the VM itself (the v0.1.3 lesson).
	// 0 = none, 1 = open, 2 = close.
	std::atomic<std::uint32_t> g_pendingPanelSound{ 0 };

	// The open/close animation (v0.13.0): the plate grows from a small
	// rectangle and the content waits for it to finish; the close mirrors
	// it. State advances inside RefreshPanel on the UI thread - atomic only
	// so the movie teardown can reset it from its own thread.
	enum class PanelAnim : std::uint32_t
	{
		kClosed = 0,
		kOpening,
		kOpen,
		kClosing,
	};
	std::atomic<PanelAnim> g_panelAnimState{ PanelAnim::kClosed };
	std::atomic<double>    g_panelHeight{ 0.0 };

	// The scanner-key hint: a vanilla button pill the mod owns, visible in
	// cruise while the panel is fully closed. Failure latches per movie.
	RE::Scaleform::GFx::Value g_scannerHint;
	std::atomic<bool>         g_scannerHintFailed{ false };
	// And its siblings inside the panel footer: the confirm-key pill and
	// the wheel/browse pill (kept despite its name-cap - the tester's
	// call; the drawn glyph is its fallback).
	RE::Scaleform::GFx::Value g_panelConfirmPill;
	RE::Scaleform::GFx::Value g_panelBrowsePill;
	// Which device the browse pill is currently DRESSED for (v1.1.0): 0 none
	// yet, 1 keyboard and mouse, 2 gamepad. Atomic because the pill is built
	// from the per-frame task and re-dressed from the feed's refresh, which
	// are not the same thread; it holds a choice, never a Scaleform value.
	std::atomic<int> g_panelBrowsePillDevice{ 0 };
	// Which rows currently wear the highlight text colour, so a moved
	// highlight recolours exactly the two rows that changed.
	bool g_panelRowBright[kPanelMaxRowsHard]{};
	RE::Scaleform::GFx::Value g_panelFormat;
	RE::Scaleform::GFx::Value g_panelDistFormat;
	RE::Scaleform::GFx::Value g_panelHint;
	RE::Scaleform::GFx::Value g_panelHintRight;
	RE::Scaleform::GFx::Value g_panelHintFormat;
	RE::Scaleform::GFx::Value g_panelHintRightFormat;
	RE::Scaleform::GFx::Value g_panelTitle;
	RE::Scaleform::GFx::Value g_panelTitleFormat;
	std::atomic<bool>          g_panelReady{ false };
	std::atomic<bool>          g_panelFailed{ false };
	std::atomic<std::uint32_t> g_panelRowCount{ 0 };
	// Where the rows start, below the header when there is one. Written at
	// panel build, read by the highlight positioning in RefreshPanel.
	std::atomic<double>        g_panelListTop{ 6.0 };

	// The Phase 4 chrome probe: a real ShipHudQuickContainer the mod owns.
	// The list handle is kept separately because every wheel move drives its
	// selectedIndex. Rows are hardcoded - this instance tests the CHROME, the
	// candidate feed stays with the drawn panel until the donor graduates.
	constexpr std::size_t       kChromeProbeRows = 4;
	RE::Scaleform::GFx::Value   g_chromeProbe;
	RE::Scaleform::GFx::Value   g_chromeProbeList;
	std::atomic<bool>           g_chromeProbeReady{ false };
	std::atomic<bool>           g_chromeProbeFailed{ false };
	std::atomic<std::int32_t>   g_chromeProbeLastSel{ -1 };
	// v0.9.1 decorations: per-row mod-owned children (script-added, so
	// timeline navigation can never touch them) and the row text format
	// cloned for the distance column.
	RE::Scaleform::GFx::Value g_chromeProbeIcons[kChromeProbeRows];
	RE::Scaleform::GFx::Value g_chromeProbeDists[kChromeProbeRows];
	RE::Scaleform::GFx::Value g_chromeProbeRowFormat;

	// Geometry for telling SAME-NAMED feed entries apart (v0.18.1): two
	// unresolved contacts both ride the feed as "Sensor Contact", and vanilla
	// names its clips with that string - so a clip name alone cannot say
	// WHICH contact a blip or icon belongs to (the tester's Ship+Anomaly
	// pair; the log kept the same clip name twice in one tick). Filled from
	// the key body's own high-feed row; `ambiguous` is set only when 2+ feed
	// candidates share the name, which is the only case any of it is read.
	struct BlipGeometry
	{
		bool   ambiguous{ false };
		bool   haveRow{ false };
		double angle{ 0.0 };     // angleToCrosshair, degrees
		double screenX{ -1.0 };  // screenPositionX/Y percentages, y bottom-up;
		double screenY{ -1.0 };  // -1 = the engine's "unprojectable" sentinel
	};

	// A ring blip's ROOT rotation is exactly angleToCrosshair + 180
	// (OffScreenIcon.as:163) - within this many degrees counts as "the same
	// contact". Two same-named contacts inside the tolerance degrade to the
	// pre-v0.18.1 behavior: both kept.
	constexpr double kDupBearingToleranceDeg = 15.0;

	// An on-screen icon sits AT its body's converted screen point, so the
	// selection's own icon lands within a few pixels of it - a same-named icon
	// further away than this (reticle space) is the OTHER contact's.
	constexpr double kDupIconMatchTolerancePx = 60.0;

	// Defined further down, but called from the data-feed callbacks above them.
	bool WorldSettled();
	void TryCreateArrow();
	void TryCreatePanel();
	void TryCreateChromeProbe();
	void RefreshPanel();
	std::string TranslateToken(const char* a_token);
	void RefreshCruiseState();
	bool ManageVanillaBlips(std::uint32_t a_selectedID, const std::string& a_selectedName,
		std::uint32_t a_lockedID, const std::string& a_lockedName,
		const std::string& a_infoTargetName,
		const BlipGeometry& a_selGeom, const BlipGeometry& a_lockGeom);

	// A TT_STAR entry is not necessarily *this* system's star: a quest-marked one
	// showed up at 8.21e17 m, about 87 light-years. Type alone is not a filter.
	constexpr double kMetersPerLightSecond = 299792458.0;

	// MapMarkerUtils' MARKER_* enum (= the feed's uPoiType) is sequential
	// from 0 and ends in a count sentinel: MARKER_SHIP_COUNT = 83. The
	// category enum's count is 10. Jemison's landing site sampled (83, 10) -
	// both sentinels at once, the engine's "no marker" - which is the gate:
	// only types BELOW the count carry real art. The Eye's sampled (43, 7)
	// lands exactly on MARKER_UNIQUE_THE_EYE / MARKER_TYPE_STATION, which is
	// what confirms the numbering.
	constexpr std::uint32_t kMarkerTypeCount = 83;
	constexpr std::uint32_t kMarkerSurfaceSettlement = 48;  // MARKER_UNIQUE_SURFACE_SETTLEMENT
	constexpr std::uint32_t kLmsOnlyTypeKnown = 1;          // MapMarkerUtils.LMS_*: generic kind badge
	constexpr std::uint32_t kLmsFullReveal = 2;             // the specific marker's own art

	constexpr std::uint32_t kTargetTypeStar = 1;
	constexpr std::uint32_t kTargetTypePOI = 4;
	constexpr std::uint32_t kTargetTypeShip = 5;
	constexpr std::uint32_t kTargetTypeStation = 6;
	constexpr std::uint32_t kTargetTypePlanet = 7;

	bool IsLocalBody(std::uint32_t a_type, double a_distanceMeters)
	{
		const bool wanted = a_type == kTargetTypePlanet || a_type == kTargetTypeStar ||
		                    (a_type == kTargetTypePOI && bIncludePOI.GetValue()) ||
		                    (a_type == kTargetTypeShip && bIncludeShips.GetValue());
		if (!wanted)
			return false;
		return a_distanceMeters <= static_cast<double>(fMaxTargetLightSeconds.GetValue()) * kMetersPerLightSecond;
	}

	// Stations, landing sites and the like sort below the bodies, so the planets
	// stay together at the top where they are the point of the list.
	bool IsSecondaryRow(std::uint32_t a_type)
	{
		return a_type == kTargetTypePOI || a_type == kTargetTypeShip;
	}

	// The user event that requests a data-model dump: the scanner key, i.e. the
	// same trigger the finished panel will use.
	constexpr const char* kDumpTriggerEvent = "SHMonocle";

	// Throttle in the pilot seat is W/S, which arrive under the in-ship names
	// `Forward` and `Back` (PHASE0-FINDINGS.md, section 2). Matched by NAME and
	// never by id code: the ids in that table are one tester's own rebinds, and
	// the same id carries different names depending on the active context.
	constexpr const char* kThrottleUpEvent = "Forward";
	constexpr const char* kThrottleDownEvent = "Back";

	bool IsThrottleEvent(const char* a_userEvent)
	{
		return a_userEvent && (std::strcmp(a_userEvent, kThrottleUpEvent) == 0 ||
								  std::strcmp(a_userEvent, kThrottleDownEvent) == 0);
	}

	// `sConfirmEvent` is a LIST, any entry of which confirms. An entry is either
	// a user-event NAME or `#<id>`, a raw key id code.
	//
	// It has to be a list because ONE PHYSICAL KEY CARRIES SEVERAL USER EVENTS
	// and which name an event reports is resolved against the active context.
	// Phase 0 logged a press arriving as `ExitShip` and its own release as
	// `StarbornPower` - the same C key, one keystroke, two names. The panel acts
	// on the press, so naming only the release gives a key that is bound,
	// documented, hinted, and silently dead.
	//
	// And it has to accept an id because **while piloting, a key may carry NO
	// user event at all**. C is exactly that key: it reports `ExitShip` and
	// `StarbornPower` elsewhere in the game and nothing whatsoever in cruise, so
	// no name can ever match it there. `#67` does.
	//
	// This does NOT retract "match on names, never ids". That rule is about the
	// MOD not baking one tester's bindings into its source, and it stands: this
	// id lives in the player's own ini, where their own bindings are precisely
	// the right thing to describe. A name still wins wherever the engine
	// supplies one, which is why the default carries both.
	//
	// On a KEYBOARD ids are virtual-key codes - 67 is C, 84 is T, 9 is Tab, 13
	// is Enter - so `#67` means the C key on any layout that agrees with that,
	// which is more portable than a name the context refuses to give. On a
	// GAMEPAD they are Bethesda's own pad codes instead (D-pad up 1, down 2,
	// left 4, right 8, LS click 64, RB 512, A 4096, X 16384, Y 32768, and the
	// triggers at 9 and 10 - values a real button mask cannot produce, which is
	// how the triggers fit). ⚠ Nothing here checks the DEVICE, so an id entry
	// matches any device reporting that number: `#1` is D-pad up AND whatever
	// else reports 1. The unnamed-press-only rule below is what keeps that from
	// mattering in practice, and every event this mod ships with is named.
	//
	// The list walk is shared with the browse keys (v1.1.0) - same shape, same
	// rules - so it takes the configured string rather than reading one.
	bool MatchesEventList(std::string_view a_configured, const char* a_userEvent,
		std::uint32_t a_idCode)
	{
		const bool named = a_userEvent && a_userEvent[0];

		std::string_view rest{ a_configured };
		while (!rest.empty()) {
			const auto comma = rest.find(',');
			auto       entry = rest.substr(0, comma);
			rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);

			constexpr std::string_view kSpace = " \t";
			if (const auto from = entry.find_first_not_of(kSpace); from != std::string_view::npos)
				entry.remove_prefix(from);
			else
				continue;  // all spaces
			if (const auto to = entry.find_last_not_of(kSpace); to != std::string_view::npos)
				entry = entry.substr(0, to + 1);

			if (entry.front() == '#') {
				// **An id entry matches only an UNNAMED press**, and that
				// restriction is the whole reason an id is safe to allow.
				//
				// `#67` does not mean "the C key" flatly - it means "the C key
				// WHEN THE GAME HAS NOTHING BOUND THERE", which is exactly the
				// case it was added for. An id is a physical key rather than an
				// action, so it cannot follow a rebind: if a player binds a ship
				// action to C, the mod would fire on their keystroke and the game
				// would act on it too, since nothing here consumes the key.
				//
				// Deferring on a named press removes that collision by
				// construction. A key carrying no user event is the ENGINE
				// telling us nothing is bound there; the moment something is,
				// the name appears and the game's own binding wins. Nothing is
				// lost, either: a named event can always be matched by its name.
				if (named)
					continue;

				entry.remove_prefix(1);
				std::uint32_t id = 0;
				const auto*   first = entry.data();
				const auto*   last = first + entry.size();
				const auto    parsed = std::from_chars(first, last, id);
				if (parsed.ec == std::errc{} && parsed.ptr == last && id == a_idCode)
					return true;
			} else if (named && entry == a_userEvent) {
				return true;
			}
		}
		return false;
	}

	// The three configured lists, read ONCE per input-queue walk rather than
	// once per event. They were per-event GetValue() string copies before the
	// browse keys became configurable, and three of those on every button of
	// every frame is a cost worth not paying on the path the wheel rides.
	struct EventLists
	{
		std::string browseUp;
		std::string browseDown;
		std::string confirm;
		// Read as EMPTY while the feature is off. An empty list matches nothing,
		// so a switched-off feature cannot take a key away from the camera, from
		// the game, or from the "that key is not one of the panel's controls"
		// advice below.
		std::string lockCourse;

		static EventLists Read()
		{
			return EventLists{ sBrowseUpEvent.GetValue(), sBrowseDownEvent.GetValue(),
				sConfirmEvent.GetValue(),
				bLockCourse.GetValue() ? sLockCourseEvent.GetValue() : std::string{} };
		}

		// The mouse wheel by default, and on a controller the D-pad. Matching
		// one list against both is what lets a single ini line serve either
		// device - see sBrowseUpEvent.
		bool IsPanelControl(const char* a_userEvent, std::uint32_t a_idCode) const
		{
			return MatchesEventList(browseUp, a_userEvent, a_idCode) ||
			       MatchesEventList(browseDown, a_userEvent, a_idCode) ||
			       MatchesEventList(confirm, a_userEvent, a_idCode) ||
			       MatchesEventList(lockCourse, a_userEvent, a_idCode);
		}
	};

	const char* DeviceName(RE::InputEvent::DeviceType a_type)
	{
		switch (a_type) {
		case RE::InputEvent::DeviceType::kKeyboard:
			return "keyboard";
		case RE::InputEvent::DeviceType::kMouse:
			return "mouse";
		case RE::InputEvent::DeviceType::kGamepad:
			return "gamepad";
		case RE::InputEvent::DeviceType::kKinect:
			return "kinect";
		default:
			return "none";
		}
	}

	const char* EventTypeName(RE::InputEvent::EventType a_type)
	{
		switch (a_type) {
		case RE::InputEvent::EventType::kButton:
			return "button";
		case RE::InputEvent::EventType::kMouseMove:
			return "mousemove";
		case RE::InputEvent::EventType::kCursorMove:
			return "cursormove";
		case RE::InputEvent::EventType::kChar:
			return "char";
		case RE::InputEvent::EventType::kThumbstick:
			return "thumbstick";
		case RE::InputEvent::EventType::kDeviceConnect:
			return "deviceconnect";
		case RE::InputEvent::EventType::kKinect:
			return "kinect";
		default:
			return "none";
		}
	}

	// Budgeted so a key stuck down (or a controller drifting) cannot fill the
	// disk while the game is left running.
	bool InputBudgetOk()
	{
		const auto cap = uMaxInputLines.GetValue();
		if (cap == 0)
			return true;

		const auto used = g_inputLinesLogged.fetch_add(1, std::memory_order_relaxed);
		if (used < cap)
			return true;
		if (used == cap)
			REX::WARN("[input] line cap ({}) reached - no further input lines this session. "
					  "Raise uMaxInputLines in ShipNavPanelCustom.ini to keep going.",
				cap);
		return false;
	}

	// ---------------------------------------------------------------------------
	// The cruise key survey.
	//
	// Written when v0.2.1 failed to take W/S from the ship, on the assumption
	// that the panel therefore had to be built on keys the game already ignores
	// in cruise. `SHMonocle` is one such, found by accident in Phase 0, and this
	// looks for the others.
	//
	// That assumption turned out to be too strong, and the survey is worth
	// keeping in spite of it. A day later the mouse wheel was taken successfully
	// - and `ZoomIn`/`ZoomOut` are NOT ignored, the game acts on them. What made
	// the difference was hooking the consumer (`PlayerCamera`) and splicing the
	// event out of the queue, rather than flagging it at `RE::UI`, which is not
	// the consumer of anything the ship does. So the real requirement is: a key
	// needs either the game to ignore it, OR a hookable consumer to splice it
	// away from. This survey answers the first half; see TODO.md for why W/S is
	// still left alone regardless.
	//
	// The plugin can only answer half the question: which events REACH us during
	// cruise, and whether they arrive already disabled. Whether the game then
	// acts on one is only visible on screen, so the output is a short candidate
	// list to test by hand - not an answer. One line per distinct event, printed
	// the moment it is first seen, so it can be read against what just happened.
	// ---------------------------------------------------------------------------

	struct SurveyEntry
	{
		std::string   name;
		std::uint32_t idCode{ 0 };
		std::uint32_t presses{ 0 };
		bool          seenEnabled{ false };
		bool          seenDisabled{ false };
	};

	std::mutex               g_surveyMutex;
	std::vector<SurveyEntry> g_survey;

	constexpr std::size_t kSurveyMaxEntries = 64;

	// What is already known, so nobody is sent off to re-test a key whose
	// behaviour in cruise is settled. Anything NOT listed here is the
	// interesting case - that is the whole point of the survey.
	const char* SurveyNote(const char* a_name)
	{
		struct Known
		{
			const char* name;
			const char* note;
		};
		static constexpr Known kKnown[]{
			{ "SHMonocle", "INERT in cruise - the mod's existing trigger" },
			{ "Forward", "active - throttle, and cannot be suppressed" },
			{ "Back", "active - throttle, and cannot be suppressed" },
			{ "StrafeLeft", "active - strafe" },
			{ "StrafeRight", "active - strafe" },
			{ "Cruise", "active - toggles cruise, leave alone" },
			{ "SelectTarget", "active - cycles target" },
			{ "QuickMap", "active - opens the star map" },
			{ "DataMenu", "active - opens the data menu" },
			{ "Console", "active - opens the console" },
		};
		for (const auto& known : kKnown) {
			if (std::strcmp(a_name, known.name) == 0)
				return known.note;
		}
		return "UNKNOWN - press it in cruise and watch whether anything happens";
	}

	void SurveyRecord(const char* a_name, std::uint32_t a_idCode, bool a_disabled)
	{
		if (!a_name || !a_name[0])
			return;  // unbound in this context; nothing to name it by

		std::lock_guard lock{ g_surveyMutex };

		for (auto& entry : g_survey) {
			if (entry.name == a_name) {
				++entry.presses;
				(a_disabled ? entry.seenDisabled : entry.seenEnabled) = true;
				return;
			}
		}

		if (g_survey.size() >= kSurveyMaxEntries)
			return;

		SurveyEntry entry;
		entry.name = a_name;
		entry.idCode = a_idCode;
		entry.presses = 1;
		(a_disabled ? entry.seenDisabled : entry.seenEnabled) = true;
		g_survey.push_back(entry);

		REX::INFO("[survey] NEW in cruise: '{}' id={} disabled={}  <- {}",
			a_name, a_idCode, a_disabled, SurveyNote(a_name));
	}

	void SurveyDump()
	{
		std::lock_guard lock{ g_surveyMutex };
		if (g_survey.empty())
			return;

		REX::INFO("[survey] --- {} distinct button events seen in cruise ---", g_survey.size());
		for (const auto& entry : g_survey) {
			const char* arrival = (entry.seenEnabled && entry.seenDisabled) ? "enabled+disabled" :
			                      entry.seenEnabled                         ? "enabled" :
			                                                                  "disabled";
			REX::INFO("[survey]   '{}' id={} presses={} arrived {}  <- {}",
				entry.name, entry.idCode, entry.presses, arrival, SurveyNote(entry.name.c_str()));
		}
		REX::INFO("[survey] --- an UNKNOWN line that arrived 'enabled' and does nothing on screen "
				  "is a free verb for the panel ---");
	}

	// ---------------------------------------------------------------------------
	// Panel selection.
	//
	// The list the player sees is the local bodies, in feed order - which is
	// stable enough to walk with a wheel, and is the same order the arrow's
	// cycling used. Everything here runs under the candidate mutex because the
	// feed callbacks rewrite that vector from another thread.
	// ---------------------------------------------------------------------------

	// One entry per line the player sees, in display order: bodies first in feed
	// order, then stations and landing sites. Caller holds g_candidateMutex.
	//
	// Moons are grouped under their planet from GNAM, not from feed order - the
	// feed interleaves them (Alpha Centauri came through as Jemison, Bondar,
	// Gagarin, Kurtz, Olivas, with Kurtz two entries from its own parent), which
	// is what defeated the v0.3.3 guess. With the real ids there is no guessing:
	// a body whose parentPlanetID matches another body's planetID is its moon.
	//
	// A moon whose planet is not in the feed stays at the top level rather than
	// disappearing - the feed lists only some moons, so this is normal.
	void CollectLocalRows(std::vector<std::size_t>& a_out)
	{
		a_out.clear();

		const bool nest = bNestMoons.GetValue();

		const auto isChildOf = [&](const Candidate& a_child, const Candidate& a_parent) {
			return nest && a_child.haveGalaxy && a_parent.haveGalaxy &&
			       a_child.galaxy.parentPlanetID != 0 &&
			       a_child.galaxy.systemID == a_parent.galaxy.systemID &&
			       a_child.galaxy.parentPlanetID == a_parent.galaxy.planetID;
		};

		const auto wanted = [&](const Candidate& a_row) {
			return IsLocalBody(a_row.type, a_row.distance);
		};

		// Pass 1: bodies, each followed by whichever of its moons are present.
		std::vector<bool> placed(g_candidates.size(), false);

		for (std::size_t i = 0; i < g_candidates.size(); ++i) {
			auto& row = g_candidates[i];
			if (!wanted(row) || IsSecondaryRow(row.type) || placed[i])
				continue;

			// Anything with a parent in this list is emitted by that parent.
			bool hasParentHere = false;
			for (std::size_t p = 0; p < g_candidates.size() && !hasParentHere; ++p) {
				if (p != i && wanted(g_candidates[p]) && isChildOf(row, g_candidates[p]))
					hasParentHere = true;
			}
			if (hasParentHere)
				continue;

			row.isMoon = false;
			a_out.push_back(i);
			placed[i] = true;

			for (std::size_t c = 0; c < g_candidates.size(); ++c) {
				if (c == i || placed[c] || !wanted(g_candidates[c]) || IsSecondaryRow(g_candidates[c].type))
					continue;
				if (!isChildOf(g_candidates[c], row))
					continue;
				g_candidates[c].isMoon = true;
				a_out.push_back(c);
				placed[c] = true;
			}
		}

		// Pass 2: stations and landing sites, below the bodies.
		for (std::size_t i = 0; i < g_candidates.size(); ++i) {
			auto& row = g_candidates[i];
			if (!wanted(row) || !IsSecondaryRow(row.type))
				continue;
			row.isMoon = false;
			a_out.push_back(i);
		}
	}

	// a_delta of 0 means "settle onto something valid": used when the panel opens
	// and when the highlighted body drops out of the feed.
	void MoveHighlight(int a_delta)
	{
		std::lock_guard          lock{ g_candidateMutex };
		std::vector<std::size_t> local;
		CollectLocalRows(local);
		if (local.empty()) {
			g_highlightID.store(0, std::memory_order_release);
			return;
		}

		const auto  current = g_highlightID.load(std::memory_order_acquire);
		std::size_t pos = 0;
		bool        found = false;
		for (std::size_t n = 0; n < local.size(); ++n) {
			if (g_candidates[local[n]].id == current) {
				pos = n;
				found = true;
				break;
			}
		}

		if (found) {
			const auto count = static_cast<int>(local.size());
			int        next = (static_cast<int>(pos) + a_delta) % count;
			if (next < 0)
				next += count;
			pos = static_cast<std::size_t>(next);
		}
		// If it was not found the highlight has gone stale (or the panel just
		// opened with nothing selected), so pos stays 0 and it lands on the
		// first local body regardless of the delta.

		g_highlightID.store(g_candidates[local[pos]].id, std::memory_order_release);
	}

	void TogglePanel()
	{
		bool wasOpen = g_panelOpen.load(std::memory_order_acquire);
		while (!g_panelOpen.compare_exchange_weak(wasOpen, !wasOpen, std::memory_order_acq_rel))
			;

		if (!wasOpen) {
			// Open onto whatever is locked, so the list shows where you already
			// are rather than jumping to the top.
			g_highlightID.store(g_lockedID.load(std::memory_order_acquire), std::memory_order_release);
			MoveHighlight(0);
			g_suppressedCount.store(0, std::memory_order_release);
			g_cameraRemovedCount.store(0, std::memory_order_release);
			// The survey sweep only runs while the panel is open, so re-arm its
			// throttle here: otherwise the first marks would be up to a whole
			// interval late every time the panel is opened. This is a plain
			// atomic store from the INPUT thread - no VM, no Scaleform, which is
			// the rule this function has always kept.
			g_lastSweepTicks.store(0, std::memory_order_release);
			if (bPanelSounds.GetValue())
				g_pendingPanelSound.store(1, std::memory_order_release);
			REX::INFO("[panel] opened - wheel moves the highlight, '{}' locks or clears it",
				sConfirmEvent.GetValue());
			if (bSuppressThrottleTest.GetValue())
				REX::INFO("[panel]   throttle-suppression test armed");
		} else {
			// Nothing is committed on close - that is the whole point of the
			// confirm key.
			if (bPanelSounds.GetValue())
				g_pendingPanelSound.store(2, std::memory_order_release);
			REX::INFO("[panel] closed (locked stays {:08X}, {} input events hidden from the camera)",
				g_lockedID.load(std::memory_order_acquire),
				g_cameraRemovedCount.load(std::memory_order_acquire));
		}
	}

	// Queue a cruise-autopilot course change for the UI thread to dispatch.
	//
	// ⚠ The press this came from is SPLICED OUT of the UI's queue (see
	// PerformInputProcessingHook), so the SWF never sees it and the mod's is the
	// only dispatch that press produces. That is what makes this a plain "set the
	// course" rather than a race - and it also means the mod must handle EVERY
	// case, including the highlighted body already being the info target. An
	// earlier version stood aside there on the theory that vanilla's own press
	// would do the job; with the press taken away, standing aside meant nothing
	// happened at all.
	void RequestLockCourse(std::uint32_t a_id)
	{
		if (!bLockCourse.GetValue() || a_id == 0)
			return;

		// Belt and braces with the splice gate, which already declines to claim
		// the key for these rows - if that ever stops matching this, the mod must
		// still not send an id the engine will silently swap for another body.
		//
		// Logged once per body rather than once per press: it is the explanation
		// for a key that does nothing, so a player pressing it repeatedly has to
		// be able to find out why - but the fifth identical line teaches nobody
		// anything, and the tester's log filled with them.
		g_pendingCourseID.store(a_id, std::memory_order_release);
		if (bVerboseLog.GetValue())
			REX::INFO("[course] requested for {:08X}", a_id);
	}

	// The confirm key is a toggle on the highlighted row: lock it, or clear it
	// if it is already the locked one. Clearing without picking another is the
	// behaviour this key exists for.
	void ConfirmHighlight()
	{
		const auto highlight = g_highlightID.load(std::memory_order_acquire);
		if (!highlight) {
			REX::INFO("[panel] confirm ignored - nothing highlighted");
			return;
		}

		if (g_lockedID.load(std::memory_order_acquire) == highlight) {
			g_lockedID.store(0, std::memory_order_release);
			REX::INFO("[panel] cleared {:08X} - no target on the HUD", highlight);
		} else {
			g_lockedID.store(highlight, std::memory_order_release);
			REX::INFO("[panel] locked {:08X}", highlight);
		}
	}

	void SuppressThrottleEvent(const RE::ButtonEvent* a_button, bool a_down, bool a_firstFrame)
	{
		// The engine mutates these events in place as they travel the chain -
		// `disabled` is the same flag it sets itself for a binding it has
		// switched off - so this writes a field the game expects to be written.
		// It is the only engine memory this plugin touches outside its own
		// vtable slot.
		const bool was = a_button->disabled;
		const_cast<RE::ButtonEvent*>(a_button)->disabled = true;

		const auto n = g_suppressedCount.fetch_add(1, std::memory_order_relaxed) + 1;

		// A held key produces one event per frame, so only the edges are logged;
		// the count carries the middle.
		if (a_down && a_firstFrame)
			REX::INFO("[suppress] '{}' PRESS - disabled {} -> true",
				SafeStr(a_button->strUserEvent.c_str()), was);
		else if (!a_down)
			REX::INFO("[suppress] '{}' RELEASE after {} suppressed events",
				SafeStr(a_button->strUserEvent.c_str()), n);
	}

	// The scanner key's initial press. Only flags are set here - the movie is
	// touched from the per-frame task, and the captured data is read on the UI
	// thread by the interposer.
	void OnTriggerPressed()
	{
		if (bScaleformReader.GetValue())
			g_dumpRequested.store(true, std::memory_order_release);
		if (bLogTargetCaptures.GetValue()) {
			g_captureRequested.store(true, std::memory_order_release);
			g_captureHighRequested.store(true, std::memory_order_release);
		}
		if (bProbeStarmapFeed.GetValue())
			g_starmapDumpRequested.store(true, std::memory_order_release);
		if (bDumpPlanetRecords.GetValue())
			g_dumpPlanetsRequested.store(true, std::memory_order_release);
		if (bProbeSurveyVM.GetValue())
			g_surveyVmProbeRequested.store(true, std::memory_order_release);

		// Only hijack the scanner key while cruising; outside cruise it still
		// opens the vanilla ship scanner.
		if (!g_inCruise.load(std::memory_order_acquire))
			return;

		// The panel-open state is toggled regardless of whether the list is
		// being drawn, so the input tests still work with bPanel off.
		TogglePanel();
	}

	// Walks the frame's input queue. This runs on every build, logging or not:
	// the scanner key reaches the mod through here, and so does the throttle
	// suppression. Everything below is ordered so that the functional work
	// happens before any logging filter can skip an event.
	void ProcessInputQueue(const RE::InputEvent* a_head)
	{
		const bool logInput = bLogInput.GetValue();
		const bool logHeld = bLogInputHeldFrames.GetValue();
		const bool logOther = bLogInputNonButton.GetValue();
		const auto lists = EventLists::Read();

		for (const RE::InputEvent* event = a_head; event; event = event->next) {
			if (event->eventType != RE::InputEvent::EventType::kButton) {
				if (logInput && logOther && InputBudgetOk())
					REX::INFO("[input] {:<9} {}", EventTypeName(event->eventType), DeviceName(event->deviceType));
				continue;
			}

			// ButtonEvent derives IDEvent, so the user-event name, the id code
			// and the disabled flag are plain members - read them directly
			// rather than through QUserEvent(), which folds the two apart cases
			// ("no binding" and "binding disabled") into one string.
			const auto* button = static_cast<const RE::ButtonEvent*>(event);
			const bool  down = button->value != 0.0f;
			const bool  firstFrame = button->heldDownSecs == 0.0f;
			const char* userEvent = button->strUserEvent.c_str();

			// --- functional work: never skipped by a logging filter ---

			// Recorded before the suppression block below, so the survey never
			// reads back this plugin's own write to `disabled`.
			if (down && firstFrame && bSurveyCruiseKeys.GetValue() &&
				g_inCruise.load(std::memory_order_acquire))
				SurveyRecord(userEvent, button->idCode, button->disabled);

			// Suppression has to see HELD frames. Throttle is a key held down,
			// so disabling only its first frame would still let the ship
			// accelerate. The cruise check is deliberately redundant with the
			// one that raises the panel: a throttle stuck off is the worst
			// failure this mod could have, so it is gated twice.
			if (bSuppressThrottleTest.GetValue() &&
				g_panelOpen.load(std::memory_order_acquire) &&
				g_inCruise.load(std::memory_order_acquire) &&
				IsThrottleEvent(userEvent))
				SuppressThrottleEvent(button, down, firstFrame);

			// NOT gated on `userEvent` being present. A key with no binding in
			// the current context arrives with an EMPTY name - and BSFixedString
			// hands back a null pointer for that, so the old `&& userEvent` here
			// dropped those events before anything could see them, logging
			// included. That is precisely what hid C: it carries no user event at
			// all while piloting, so it was invisible rather than merely
			// unmatched, and the log said nothing at all rather than saying so.
			if (down && firstFrame) {
				const bool named = userEvent && userEvent[0];

				if (named && std::strcmp(userEvent, kDumpTriggerEvent) == 0) {
					OnTriggerPressed();
				} else if (g_panelOpen.load(std::memory_order_acquire) &&
						   g_inCruise.load(std::memory_order_acquire)) {
					// The browse keys are spliced away from the camera
					// elsewhere; here they are simply read. One press - or one
					// wheel notch - is one step, because this whole block only
					// runs on `firstFrame`, so holding a D-pad direction does
					// NOT scroll the list. That is deliberate: vanilla has a
					// RepeatingButtonData for hold-to-repeat and this is not
					// it.
					if (MatchesEventList(lists.browseUp, userEvent, button->idCode)) {
						MoveHighlight(-1);
						if (bVerboseLog.GetValue())
							REX::INFO("[panel] highlight up -> {:08X}", g_highlightID.load(std::memory_order_acquire));
					} else if (MatchesEventList(lists.browseDown, userEvent, button->idCode)) {
						MoveHighlight(1);
						if (bVerboseLog.GetValue())
							REX::INFO("[panel] highlight down -> {:08X}", g_highlightID.load(std::memory_order_acquire));
					} else if (MatchesEventList(lists.confirm, userEvent, button->idCode)) {
						ConfirmHighlight();
					} else if (MatchesEventList(lists.lockCourse, userEvent, button->idCode)) {
						// The HIGHLIGHT, not the lock: setting a course is its
						// own verb and needs no target chosen first. That is the
						// whole reason it is worth having - straight off the row
						// under the bar.
						RequestLockCourse(g_highlightID.load(std::memory_order_acquire));
					} else {
						// Everything else pressed while the panel is open, once
						// per key and capped. This is the answer to "I bound my
						// key and nothing happens", so it reports the id as well
						// as the name: the name can be absent, or differ between
						// a press and its own release, but the id is the key.
						static std::mutex                       s_seenMutex;
						static std::unordered_set<std::uint32_t> s_seen;

						std::lock_guard lock{ s_seenMutex };
						if (s_seen.size() < 16 && s_seen.emplace(button->idCode).second) {
							const std::string reports = named ? std::format("'{}'", userEvent) :
							                                    std::string{ "no user event in this context" };
							const std::string suggest = named ? userEvent : std::format("#{}", button->idCode);
							REX::INFO("[panel] key id={} reports {} and is not one of the panel's controls - "
									  "if that is the key you meant, add {} to sConfirmEvent",
								button->idCode, reports, suggest);
						}
					}
				}
			}

			// --- logging only, from here down ---

			if (!logInput)
				continue;
			if (down && !firstFrame && !logHeld)
				continue;  // held-down repeat
			if (!InputBudgetOk())
				continue;  // `continue`, not `return`: the walk must go on

			REX::INFO("[input] button   {:<8} user='{}' id={:<4} disabled={} {} value={:.2f} held={:.3f}s",
				DeviceName(button->deviceType),
				SafeStr(button->strUserEvent.c_str()),
				button->idCode,
				button->disabled,
				down ? (firstFrame ? "PRESS " : "repeat") : "RELEASE",
				button->value,
				button->heldDownSecs);
		}
	}

	// ---------------------------------------------------------------------------
	// ⚠ THE COURSE KEY HAS TO BE TAKEN FROM THE SWF, NOT OUT-ORDERED.
	//
	// The first cut let vanilla and the mod both dispatch and assumed the mod's
	// would land last, because the input thread only stores an atomic while the
	// dispatch waits for the next high-feed tick. **In game the ordering is the
	// other way round**, and two symptoms said so:
	//
	//   * with a target on A and the panel highlighting B, the course went to A;
	//   * with NO target and the panel highlighting B, the autopilot came on and
	//     switched straight back off.
	//
	// The second one is the diagnostic. Nothing but the mod could have turned it
	// ON (vanilla cannot lock a course without a target - the tester's own
	// finding), so the OFF is vanilla's `{uBodyID: 0}` arriving AFTER it. That
	// also names what 0 means: **clear**. And the earlier `WeaponGroup1` build is
	// the control that rules out the alternative reading - a course set on an
	// untargeted body persisted perfectly there, so the engine is not cancelling
	// it for want of a target.
	//
	// So the fix is not a longer delay - it is to make sure the SWF never sees the
	// press at all. Same technique as the camera tap below, on the receiver that
	// feeds the menus: unlink, call through, relink immediately.
	//
	// Deliberately narrow. Only the course key, only in cruise, only while the
	// panel is open, and only when there is a row for it to act on - so an empty
	// list does not silently eat the key, and vanilla keeps it whole in every
	// other state.
	// ---------------------------------------------------------------------------
	void PerformInputProcessingHook(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queueHead)
	{
		ProcessInputQueue(a_queueHead);

		const auto original = g_origPerformInputProcessing.load(std::memory_order_acquire);
		if (!original)
			return;

		// Any highlighted row may be asked for - see the header above
		// RequestLockCourse for why the mod no longer tries to predict which the
		// engine will take. The only requirement is that there IS a row, so an
		// empty list does not silently eat the key.
		const bool claiming = bLockCourse.GetValue() &&
		                      g_panelOpen.load(std::memory_order_acquire) &&
		                      g_inCruise.load(std::memory_order_acquire) &&
		                      g_highlightID.load(std::memory_order_acquire) != 0;
		if (!claiming) {
			original(a_this, a_queueHead);
			return;
		}

		const std::string courseEvents = sLockCourseEvent.GetValue();

		// Twin of the camera tap's splice, written out rather than shared: that
		// one is load-bearing and proven, and a refactor of it to serve this is
		// risk with no return. Every link changed is recorded with the value it
		// held and put back in reverse order.
		struct LinkFix
		{
			RE::InputEvent* node;
			RE::InputEvent* previousNext;
		};
		constexpr std::size_t kMaxFixes = 16;
		LinkFix               fixes[kMaxFixes]{};
		std::size_t           fixCount = 0;

		const RE::InputEvent* head = a_queueHead;
		RE::InputEvent*       prev = nullptr;
		std::uint32_t         removed = 0;

		for (const RE::InputEvent* event = a_queueHead; event;) {
			RE::InputEvent* nextEvent = event->next;

			bool drop = false;
			if (event->eventType == RE::InputEvent::EventType::kButton) {
				const auto* button = static_cast<const RE::ButtonEvent*>(event);
				// Presses AND releases AND held frames: the SWF's own handler
				// reads the press/release flag itself, so leaving half the
				// keystroke in the chain would let the fallback fire on the
				// half we left behind.
				drop = MatchesEventList(courseEvents, button->strUserEvent.c_str(), button->idCode);
			}

			if (drop && fixCount < kMaxFixes) {
				if (prev) {
					fixes[fixCount++] = { prev, prev->next };
					prev->next = nextEvent;
				} else {
					head = nextEvent;
				}
				++removed;
				// `prev` deliberately not advanced - the dropped node is out of
				// the chain, so the last survivor remains the predecessor.
			} else {
				prev = const_cast<RE::InputEvent*>(event);
			}

			event = nextEvent;
		}

		original(a_this, head);

		// Relink before anything else walks the chain.
		for (std::size_t i = fixCount; i-- > 0;)
			fixes[i].node->next = fixes[i].previousNext;

		if (removed && bVerboseLog.GetValue())
			REX::INFO("[course] hid {} '{}' event(s) from the UI - the panel owns that key while it "
					  "is open",
				removed, courseEvents);
	}

	// ---------------------------------------------------------------------------
	// Tap 6: the camera's own input receiver.
	//
	// `PlayerCamera` is a `BSInputEventReceiver` in its own right, so it can be
	// hooked with the same live-vtable trick as `RE::UI`. The point is to hide
	// the wheel from it while the panel is up, so the wheel can drive a list
	// without swinging the point of view.
	//
	// This does NOT set `disabled` - that is precisely what failed for the
	// throttle. It unlinks the wheel events from the queue for the duration of
	// the camera's call and relinks them immediately after, so the camera cannot
	// act on events it never sees, and every other receiver still gets the whole
	// chain intact.
	// ---------------------------------------------------------------------------

	std::atomic<PerformInputProcessing_t> g_origCameraInputProcessing{ nullptr };
	std::atomic<bool>                     g_cameraTapClaimed{ false };

	void PerformCameraInputProcessingHook(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queueHead)
	{
		const auto original = g_origCameraInputProcessing.load(std::memory_order_acquire);

		// The wheel and the confirm key are hidden from the camera whenever the
		// panel is open - this is the shipped behaviour now, not a test. The
		// setting survives only as an escape hatch if the camera hook ever
		// misbehaves.
		const bool filtering = bWheelFilter.GetValue() &&
		                       g_panelOpen.load(std::memory_order_acquire) &&
		                       g_inCruise.load(std::memory_order_acquire);

		if (!filtering) {
			if (original)
				original(a_this, a_queueHead);
			return;
		}

		// Read once, not once per event - see EventLists.
		const auto lists = EventLists::Read();

		// Every link changed is recorded with the value it held, and they are
		// put back in reverse order - which is what makes repeated edits to the
		// same node (consecutive wheel notches) unwind correctly.
		struct LinkFix
		{
			RE::InputEvent* node;
			RE::InputEvent* previousNext;
		};
		constexpr std::size_t kMaxFixes = 32;
		LinkFix               fixes[kMaxFixes]{};
		std::size_t           fixCount = 0;

		const RE::InputEvent* head = a_queueHead;
		RE::InputEvent*       prev = nullptr;
		std::uint32_t         removed = 0;

		for (const RE::InputEvent* event = a_queueHead; event;) {
			RE::InputEvent* nextEvent = event->next;

			bool drop = false;
			if (event->eventType == RE::InputEvent::EventType::kButton) {
				const auto* button = static_cast<const RE::ButtonEvent*>(event);
				const char* name = button->strUserEvent.c_str();

				// The confirm key goes too, so a key that ALREADY drives the
				// camera can be used to confirm without also swinging the view.
				// That is what makes a POV key a legitimate candidate: the
				// wheel's own argument, applied to the confirm.
				//
				// A no-op for a confirm key the camera never wanted - C is
				// nameless here and does nothing to the view - so this costs
				// nothing when it is not needed. The same is true of the D-pad
				// browse events, which the camera has no interest in either;
				// they go through this list for the same reason the confirm
				// does, so that a player who moves browse ONTO a camera key
				// gets the wheel's own protection rather than a swinging view.
				drop = lists.IsPanelControl(name, button->idCode);
			}

			if (drop && fixCount < kMaxFixes) {
				if (prev) {
					fixes[fixCount++] = { prev, prev->next };
					prev->next = nextEvent;
				} else {
					head = nextEvent;  // the head itself is being dropped
				}
				++removed;
				// `prev` deliberately not advanced: the dropped node is out of
				// the chain, so the last survivor remains the predecessor.
			} else {
				prev = const_cast<RE::InputEvent*>(event);
			}

			event = nextEvent;
		}

		if (original)
			original(a_this, head);

		// Relink before anything else walks the chain.
		for (std::size_t i = fixCount; i-- > 0;)
			fixes[i].node->next = fixes[i].previousNext;

		if (removed) {
			// The running total is what matters, and the panel prints it once on
			// close - a line per wheel notch is a verbose-only trace.
			const auto total = g_cameraRemovedCount.fetch_add(removed, std::memory_order_relaxed) + removed;
			if (bVerboseLog.GetValue())
				REX::INFO("[camera] hid {} input event(s) from PlayerCamera (total {})", removed, total);
		}
	}

	void TryInstallCameraTap()
	{
		if (!bWheelFilter.GetValue() || g_cameraTapClaimed.load(std::memory_order_acquire))
			return;

		const auto camera = RE::PlayerCamera::GetSingleton();
		if (!camera)
			return;  // too early - try again next frame

		bool claimed = false;
		if (!g_cameraTapClaimed.compare_exchange_strong(claimed, true, std::memory_order_acq_rel))
			return;

		// PlayerCamera inherits TESCamera first and BSInputEventReceiver at
		// 0x48, so the upcast - not a hand-written offset - is what finds the
		// receiver subobject whose first qword is its vtable.
		auto*      receiver = static_cast<RE::BSInputEventReceiver*>(camera);
		const auto vtableAddr = *reinterpret_cast<std::uintptr_t*>(receiver);
		if (!vtableAddr) {
			REX::WARN("[wheel] PlayerCamera has no vtable pointer - camera tap not installed");
			return;
		}

		constexpr std::size_t kPerformInputProcessing = 1;

		const auto slot = vtableAddr + sizeof(void*) * kPerformInputProcessing;
		const auto original = *reinterpret_cast<std::uintptr_t*>(slot);
		g_origCameraInputProcessing.store(
			reinterpret_cast<PerformInputProcessing_t>(original), std::memory_order_release);

		REL::Relocation<std::uintptr_t> vtable{ vtableAddr };
		vtable.write_vfunc(kPerformInputProcessing, &PerformCameraInputProcessingHook);

		REX::INFO("[wheel] camera tap installed on PlayerCamera::PerformInputProcessing "
				  "(vtable {:016X}, original {:016X})",
			vtableAddr, original);
	}

	// Called every frame until it succeeds once. The claim is a single-winner
	// exchange: two threads patching the same vtable entry would leave the hook
	// calling itself, so this must not be a plain bool (the SeamlessGravJumps
	// double-fire was exactly this shape of bug).
	//
	// This used to be gated on `bLogInput`, from Phase 0 when the tap existed
	// only to log. Once the scanner key started arriving through it the gate was
	// a live bug, and flipping that default off for the v0.2.0 package shipped
	// it: no tap, so nothing ever set the cycle request, so no body was ever
	// selected and the arrow stayed invisible. The tap is infrastructure now and
	// installs on its own setting; logging is a separate question.
	void TryInstallInputTap()
	{
		if (!bInputTap.GetValue() || g_inputTapClaimed.load(std::memory_order_acquire))
			return;

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;  // too early - try again next frame

		bool claimed = false;
		if (!g_inputTapClaimed.compare_exchange_strong(claimed, true, std::memory_order_acq_rel))
			return;

		// The upcast, not a hand-written +0x10, is what finds the
		// BSInputEventReceiver subobject; its first qword is the vtable.
		auto*      receiver = static_cast<RE::BSInputEventReceiver*>(ui);
		const auto vtableAddr = *reinterpret_cast<std::uintptr_t*>(receiver);
		if (!vtableAddr) {
			REX::WARN("[input] UI has no vtable pointer - input tap not installed");
			return;
		}

		constexpr std::size_t kPerformInputProcessing = 1;

		// Publish the original before redirecting the vtable, so a call that
		// lands between the two still reaches the engine.
		const auto slot = vtableAddr + sizeof(void*) * kPerformInputProcessing;
		const auto original = *reinterpret_cast<std::uintptr_t*>(slot);
		g_origPerformInputProcessing.store(
			reinterpret_cast<PerformInputProcessing_t>(original), std::memory_order_release);

		REL::Relocation<std::uintptr_t> vtable{ vtableAddr };
		vtable.write_vfunc(kPerformInputProcessing, &PerformInputProcessingHook);

		REX::INFO("[input] tap installed on UI::PerformInputProcessing (vtable {:016X}, original {:016X})",
			vtableAddr, original);

		if (bVerifyVTableID.GetValue()) {
			// Informational only, and off by default: if the Address Library
			// lacks this id the lookup is fatal, which is not a risk worth
			// taking on a diagnostic build's default path.
			const auto fromID = RE::UI::VTABLE[1].address();
			if (fromID == vtableAddr)
				REX::INFO("[input] Address Library UI::VTABLE[1] matches the live vtable");
			else
				REX::WARN("[input] Address Library UI::VTABLE[1] is {:016X} but the live vtable is {:016X} "
						  "- VTABLE[1] is not BSInputEventReceiver; do not use that index for Phase 1 hooks",
					fromID, vtableAddr);
		}
	}

	// ---------------------------------------------------------------------------
	// Tap 2: menus.
	// ---------------------------------------------------------------------------

	class MenuOpenCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent&                 a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (bLogMenus.GetValue())
				REX::INFO("[menu] {:<28} {}", SafeStr(a_event.menuName.c_str()), a_event.opening ? "opened" : "closed");
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	MenuOpenCloseSink g_menuSink;

	// SFSE calls this once per menu whose Scaleform movie is created. Unlike the
	// open/close sink it fires even for menus that were already up before this
	// plugin registered, and it proves whether a movie actually exists to inject
	// into later. Only plain members are read here - no virtual calls.
	void OnMenuMovieCreated(RE::IMenu* a_menu)
	{
		if (!a_menu)
			return;

		// A new movie means our replaced function went with the old one, so the
		// interposer has to be reinstalled. Cheaper and far more reliable than
		// comparing movie pointers, which an allocator is free to recycle.
		const char* name = a_menu->menuName.c_str();
		if (name && std::strcmp(name, kShipHudMenu) == 0) {
			// FIRST, before any of the re-arming below: this is the only moment
			// the plugin is told a movie changed, and every gate that keeps a
			// worker thread out of a half-built AS3 VM hangs off this counter.
			// Re-arming the subscriber without also invalidating the generation
			// is what let the 2026-08-02 takeoff crash happen - the reset made
			// the mod eager again while nothing recorded that the movie it was
			// about to poke was 16 ms old.
			g_movieGeneration.fetch_add(1, std::memory_order_acq_rel);

			g_interposeInstalled.store(false, std::memory_order_release);
			g_interposeFailed.store(false, std::memory_order_release);
			g_subscribed.store(false, std::memory_order_release);
			g_subscribedHigh.store(false, std::memory_order_release);
			g_subscribeFailed.store(false, std::memory_order_release);
			g_subscribeAttempts.store(0, std::memory_order_release);

			// A new movie means cruise state is unknown until read fresh. Left
			// stale-true across an in-cruise reload, the blip pass would run
			// against the next HALF-BUILT movie during the load transition.
			g_inCruise.store(false, std::memory_order_release);

			// The arrow belonged to the old movie. Without this the plugin keeps
			// writing rotation to a clip whose movie has been destroyed - the log
			// showed exactly that, two subscription rounds against one arrow.
			g_arrowReady.store(false, std::memory_order_release);
			g_arrowFailed.store(false, std::memory_order_release);
			g_arrowClip = RE::Scaleform::GFx::Value{};

			// The blip holder too - and the hidden container went down with the
			// movie, so the new one starts visible on its own.
			g_blipHolderReady.store(false, std::memory_order_release);
			g_blipHolderFailed.store(false, std::memory_order_release);
			g_blipHolder = RE::Scaleform::GFx::Value{};
			g_blipsHidden.store(false, std::memory_order_release);

			// The faux blip was a child of the holder, in the same movie.
			g_fauxReady.store(false, std::memory_order_release);
			g_fauxFailed.store(false, std::memory_order_release);
			g_fauxBlip = RE::Scaleform::GFx::Value{};
			g_fauxLow = RE::Scaleform::GFx::Value{};
			g_fauxHigh = RE::Scaleform::GFx::Value{};
			g_fauxLastID.store(0, std::memory_order_release);

			// Any faded on-screen icons went down with the movie too.
			g_iconsFaded.store(false, std::memory_order_release);
			g_infoTargetIndex.store(-1, std::memory_order_release);

			// A queued keypress is a keypress the movie it was meant for no
			// longer exists to receive, and an outstanding request has nothing
			// left to be audited against.
			g_pendingCourseID.store(0, std::memory_order_release);
			g_courseAskedID.store(0, std::memory_order_release);

			// The lock's feed presence must be re-confirmed against the NEW
			// movie's payloads before absence may clear a moon lock.
			g_lockSeenInFeed.store(0, std::memory_order_release);

			// The translation scratch field belonged to the old movie; the
			// cached words survive - the language does not change mid-game.
			g_translatorField = RE::Scaleform::GFx::Value{};
			g_translatorReady.store(false, std::memory_order_release);
			g_translatorFailed.store(false, std::memory_order_release);

			// The chrome probe was a child of the old movie's reticle.
			g_chromeProbeReady.store(false, std::memory_order_release);
			g_chromeProbeFailed.store(false, std::memory_order_release);
			g_chromeProbe = RE::Scaleform::GFx::Value{};
			g_chromeProbeList = RE::Scaleform::GFx::Value{};
			g_chromeProbeLastSel.store(-1, std::memory_order_release);
			for (auto& icon : g_chromeProbeIcons)
				icon = RE::Scaleform::GFx::Value{};
			for (auto& dist : g_chromeProbeDists)
				dist = RE::Scaleform::GFx::Value{};
			g_chromeProbeRowFormat = RE::Scaleform::GFx::Value{};

			// The list belonged to the old movie too.
			g_panelReady.store(false, std::memory_order_release);
			g_panelFailed.store(false, std::memory_order_release);
			g_panelClip = RE::Scaleform::GFx::Value{};
			g_panelHighlight = RE::Scaleform::GFx::Value{};
			g_panelFormat = RE::Scaleform::GFx::Value{};
			g_panelDistFormat = RE::Scaleform::GFx::Value{};
			g_panelHint = RE::Scaleform::GFx::Value{};
			g_panelHintRight = RE::Scaleform::GFx::Value{};
			g_panelHintFormat = RE::Scaleform::GFx::Value{};
			g_panelHintRightFormat = RE::Scaleform::GFx::Value{};
			g_panelTitle = RE::Scaleform::GFx::Value{};
			g_panelTitleFormat = RE::Scaleform::GFx::Value{};
			g_panelScrollTrack = RE::Scaleform::GFx::Value{};
			g_panelScrollThumb = RE::Scaleform::GFx::Value{};
			g_panelNameWidth.store(0.0, std::memory_order_release);
			g_panelListTop.store(6.0, std::memory_order_release);
			g_panelHeight.store(0.0, std::memory_order_release);
			g_panelAnimState.store(PanelAnim::kClosed, std::memory_order_release);
			g_pendingPanelSound.store(0, std::memory_order_release);
			g_scannerHint = RE::Scaleform::GFx::Value{};
			g_scannerHintFailed.store(false, std::memory_order_release);
			g_panelConfirmPill = RE::Scaleform::GFx::Value{};
			g_panelBrowsePill = RE::Scaleform::GFx::Value{};
			g_panelBrowsePillDevice.store(0, std::memory_order_release);
			for (auto& bright : g_panelRowBright)
				bright = false;
			for (auto& dist : g_panelDists)
				dist = RE::Scaleform::GFx::Value{};
			for (std::size_t i = 0; i < kPanelMaxRowsHard; ++i) {
				g_panelIcons[i] = RE::Scaleform::GFx::Value{};
				g_panelIconDrawn[i] = false;
				g_panelIconClass[i] = PlanetClass::kUnknown;
				g_panelIconSettled[i] = false;
				g_panelPoiIcons[i] = RE::Scaleform::GFx::Value{};
				g_panelPoiIconKey[i] = 0;
				g_panelGiantIcons[i] = RE::Scaleform::GFx::Value{};
				// The survey marks belonged to the old movie's clips too, and
				// their last-drawn values described art that no longer exists.
				g_panelSurveyBanners[i] = RE::Scaleform::GFx::Value{};
				g_panelSurveyBars[i] = RE::Scaleform::GFx::Value{};
				g_panelSurveyFills[i] = RE::Scaleform::GFx::Value{};
				g_panelSurveyDrawn[i] = kSurveyNeverDrawn;
			}
			g_panelPoiIconsFailed.store(false, std::memory_order_release);
			g_panelGiantIconsFailed.store(false, std::memory_order_release);
			for (auto& row : g_panelRows)
				row = RE::Scaleform::GFx::Value{};
			g_panelRowCount.store(0, std::memory_order_release);

			{
				std::lock_guard lock{ g_candidateMutex };
				g_candidates.clear();
			}

			// The panel closes and the browse cursor goes, but the LOCKED body
			// survives: it is a form id, not a Scaleform handle, and rebuilds
			// happen often enough that dropping it would feel like the mod
			// forgetting your target at random. If the id never comes back in
			// the feed the arrow simply stays hidden.
			g_panelOpen.store(false, std::memory_order_release);
			g_highlightID.store(0, std::memory_order_release);
		}

		if (!bLogMenus.GetValue())
			return;

		REX::INFO("[menu-movie] {:<28} movie={} vtable={:016X}",
			SafeStr(a_menu->menuName.c_str()),
			a_menu->uiMovie ? "yes" : "no",
			*reinterpret_cast<std::uintptr_t*>(a_menu));
	}

	// ---------------------------------------------------------------------------
	// Tap 4: the ship HUD's ActionScript data model.
	//
	// The decompiled SWF shows the engine pushing the whole target list into the
	// HUD (`LowFreqTargetData.targetArray`, with `iHoverTargetIndex`,
	// `iInfoTargetIndex`, per-entry `uniqueID` and `uTargetType`), and cruise
	// state alongside it (`CruiseModeHUDActive`). `uniqueID` is the id the mod
	// will need: vanilla's own `FarTravelIconBase.OnLockCourse()` dispatches
	// `Reticle_OnCruiseLockCourse` with `{uBodyID: TargetOnlyData.uniqueID}`.
	//
	// The question this answers: in cruise, does `targetArray` hold the whole
	// system or only the forward cone? That decides whether the panel can list
	// a planet it cannot currently see.
	//
	// Every id needed here is mapped: `Value::ObjectInterface::*` are live in
	// CommonLibSF, and `ASMovieRootBase`'s own methods are pure virtuals called
	// through the object's vtable, so they need no ids at all.
	// ---------------------------------------------------------------------------

	std::atomic<std::uint32_t> g_scaleformLines{ 0 };

	// Standard AS3 DisplayObject / MovieClip / loader properties. The first run
	// of this reader spent its entire budget on these and never reached a single
	// custom member: every display object carries ~40 of them, and `loaderInfo`,
	// `parent`, `root` and `stage` are cycles straight back up the tree
	// (`loaderInfo.content.name` came back as "root1", the root itself).
	// Skipping them is what makes the walk reach the game's own data.
	constexpr const char* kBoilerplateMembers[]{
		"accessibilityProperties", "alpha", "blendMode", "blendShader", "buttonMode",
		"cacheAsBitmap", "constructor", "contextMenu", "currentFrame", "currentFrameLabel",
		"currentLabel", "currentLabels", "currentScene", "doubleClickEnabled", "dropTarget",
		"filters", "focusRect", "framesLoaded", "graphics", "height", "hitArea",
		"loaderInfo", "mask", "metaData", "mouseChildren", "mouseEnabled", "mouseX",
		// "name" deliberately NOT skipped: it is a DisplayObject property, but it
		// is also the target's actual name on the engine's data entries
		// (`TargetLow.name` is what the on-screen icons render), and skipping it
		// hid that from the first schema dump entirely.
		"mouseY", "numChildren", "opaqueBackground", "parent", "prototype",
		"root", "rotation", "rotationX", "rotationY", "rotationZ", "scale9Grid",
		"scaleX", "scaleY", "scaleZ", "scenes", "scrollRect", "soundTransform",
		"stage", "tabChildren", "tabEnabled", "tabIndex", "textSnapshot", "totalFrames",
		"trackAsMenu", "transform", "useHandCursor", "visible", "width", "x", "y", "z",
	};

	bool IsBoilerplateMember(const char* a_name)
	{
		if (!a_name)
			return true;

		// Adobe Animate motion-tween artifacts: `__animFactory_*` / `__animArray_*`
		// carry keyframe lists with colour transforms and matrices per frame, and
		// they buried the second run exactly as the display properties buried the
		// first. Nothing authored by the game lives behind a `__` prefix.
		if (a_name[0] == '_' && a_name[1] == '_')
			return true;

		for (const auto* skip : kBoilerplateMembers) {
			if (std::strcmp(a_name, skip) == 0)
				return true;
		}
		return false;
	}

	// Names worth shouting about, so the answer is greppable in a large dump.
	// The data model itself turned out to be unreachable - LowFreqTargetData and
	// friends are declared `private` in ShipReticle, and AS3 private members are
	// not enumerable (which is precisely why the public `CruiseModeHUDActive`
	// getter showed up and they did not). The target *icons* are display
	// children with public members though, so `Name_tf` / `text` is the way to
	// see which targets the HUD actually knows about.
	constexpr const char* kInterestingMembers[]{
		"targetArray", "uniqueID", "uBodyID", "iInfoTargetIndex", "iHoverTargetIndex",
		"uTargetType", "CruiseMode", "bIsCruiseTargetLock", "LowFreq", "HighFreq",
		"TargetData", "TargetOnly", "Reticle", "Name_tf", "Distance_tf", "text",
		"Indicator", "TargetIcon", "OffScreen", "OnScreen",
	};

	bool IsInterestingMember(const char* a_name)
	{
		if (!a_name)
			return false;
		for (const auto* want : kInterestingMembers) {
			if (std::strstr(a_name, want) != nullptr)
				return true;
		}
		return false;
	}

	bool ScaleformBudgetOk()
	{
		const auto cap = uScaleformMaxLines.GetValue();
		if (cap == 0)
			return true;
		const auto used = g_scaleformLines.fetch_add(1, std::memory_order_relaxed);
		if (used < cap)
			return true;
		if (used == cap)
			REX::WARN("[sf] line cap ({}) reached - raise uScaleformMaxLines to see more", cap);
		return false;
	}

	// Feed values arrive as int, uint or number depending on the field, and the
	// typed getters assert if asked for the wrong one.
	double AsNumber(const RE::Scaleform::GFx::Value& a_value)
	{
		if (a_value.IsNumber())
			return a_value.GetNumber();
		if (a_value.IsInt())
			return static_cast<double>(a_value.GetInt());
		if (a_value.IsUInt())
			return static_cast<double>(a_value.GetUInt());
		return 0.0;
	}

	std::string DescribeValue(const RE::Scaleform::GFx::Value& a_value)
	{
		// Order matters: IsObject() is also true for arrays and display objects.
		if (a_value.IsUndefined())
			return "undefined";
		if (a_value.IsBoolean())
			return a_value.GetBoolean() ? "true" : "false";
		if (a_value.IsInt())
			return std::to_string(a_value.GetInt());
		if (a_value.IsUInt())
			return std::to_string(a_value.GetUInt());
		if (a_value.IsNumber()) {
			char buf[32]{};
			std::snprintf(buf, sizeof(buf), "%g", a_value.GetNumber());
			return buf;
		}
		if (a_value.IsString()) {
			const char* str = a_value.GetString();
			return std::string{ "\"" } + (str ? str : "") + "\"";
		}
		if (a_value.IsArray())
			return "[array]";
		if (a_value.IsDisplayObject())
			return "{display}";
		if (a_value.IsObject())
			return "{object}";
		return "<other>";
	}

	struct SfNode
	{
		std::string               path;
		RE::Scaleform::GFx::Value value;
	};

	bool SkipMember(const char* a_name)
	{
		return bScaleformSkipBoilerplate.GetValue() && IsBoilerplateMember(a_name);
	}

	void EmitNode(std::string a_path, const RE::Scaleform::GFx::Value& a_value, bool a_interesting, std::vector<SfNode>* a_next)
	{
		if (!ScaleformBudgetOk())
			return;

		// "[sf*]" marks a hit on a name we are hunting for, so the answer stays
		// greppable however large the dump gets.
		REX::INFO("{} {} = {}", a_interesting ? "[sf*]" : "[sf] ", a_path, DescribeValue(a_value));

		if (a_next && (a_value.IsArray() || a_value.IsObject() || a_value.IsDisplayObject()))
			a_next->push_back(SfNode{ std::move(a_path), a_value });
	}

	class LevelCollector : public RE::Scaleform::GFx::Value::ObjectVisitor
	{
	public:
		LevelCollector(std::string a_base, std::vector<SfNode>* a_next) :
			_base(std::move(a_base)), _next(a_next)
		{}

		// Without this the visitor sees nothing on an AS3 object - the base
		// returns false and AS3 members are all "public".
		bool IncludeAS3PublicMembers() const override { return true; }

		void Visit(const char* a_name, const RE::Scaleform::GFx::Value& a_value) override
		{
			if (SkipMember(a_name))
				return;
			if (_seen++ >= uScaleformMaxChildren.GetValue())
				return;
			EmitNode(_base + "." + (a_name ? a_name : "?"), a_value, IsInterestingMember(a_name), _next);
		}

	private:
		std::string          _base;
		std::vector<SfNode>* _next;
		std::uint32_t        _seen{ 0 };
	};

	class ElementCollector : public RE::Scaleform::GFx::Value::ArrayVisitor
	{
	public:
		ElementCollector(std::string a_base, std::vector<SfNode>* a_next) :
			_base(std::move(a_base)), _next(a_next)
		{}

		void Visit(std::uint32_t a_index, const RE::Scaleform::GFx::Value& a_value) override
		{
			if (_seen++ >= uScaleformMaxChildren.GetValue())
				return;
			EmitNode(_base + "[" + std::to_string(a_index) + "]", a_value, false, _next);
		}

	private:
		std::string          _base;
		std::vector<SfNode>* _next;
		std::uint32_t        _seen{ 0 };
	};

	// Breadth-first. The depth-first version spent its whole budget inside the
	// first child it descended into, so nothing at the top level after that
	// child was ever seen. Level order guarantees every member of a level is
	// logged before anything below it - which is what discovery needs.
	void WalkBreadthFirst(const std::string& a_rootPath, const RE::Scaleform::GFx::Value& a_root, std::uint32_t a_maxDepth)
	{
		std::vector<SfNode> current;
		EmitNode(a_rootPath, a_root, false, &current);

		const auto cap = uScaleformMaxLines.GetValue();
		for (std::uint32_t level = 0; level < a_maxDepth && !current.empty(); ++level) {
			std::vector<SfNode> next;
			for (auto& node : current) {
				if (cap != 0 && g_scaleformLines.load(std::memory_order_relaxed) > cap)
					break;
				// The visitors are non-const, and Value is refcounted, so copy.
				RE::Scaleform::GFx::Value value = node.value;
				if (value.IsArray()) {
					ElementCollector visitor{ node.path, &next };
					value.VisitElements(&visitor);
				} else {
					LevelCollector visitor{ node.path, &next };
					value.VisitMembers(&visitor);
				}
			}
			REX::INFO("[sf] ---- level {} done, {} nodes queued ----", level + 1, next.size());
			current = std::move(next);
		}
	}

	// Runs from the per-frame task, never from the input hook: the movie is not
	// ours to touch from an arbitrary thread, and a user-triggered dump keeps
	// the exposure to a few frames a session rather than every frame.
	void DumpShipHudDataModel()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;

		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud)) {
			REX::INFO("[sf] {} is not open - dump skipped", kShipHudMenu);
			return;
		}

		const auto menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot) {
			REX::WARN("[sf] {} has no movie root - dump skipped", kShipHudMenu);
			return;
		}

		g_scaleformLines.store(0, std::memory_order_relaxed);
		const auto depth = uScaleformDepth.GetValue();
		auto*      root = menu->uiMovie->asMovieRoot.get();

		const char* rootPath = menu->GetRootPath();
		REX::INFO("[sf] ==== dump begin (root path '{}', depth {}) ====", rootPath ? rootPath : "-", depth);

		// Cruise state is a public getter and reads straight off the reticle -
		// this is piece 1, and it costs one lookup.
		const std::string reticlePath = std::string{ rootPath ? rootPath : "root" } + ".Reticle_mc";
		for (const auto* name : { "CruiseModeHUDActive", "CanActivateCruiseMode" }) {
			const std::string path = reticlePath + "." + name;
			RE::Scaleform::GFx::Value flag;
			if (root->GetVariable(&flag, path.c_str()))
				REX::INFO("[sf*] {} = {}", path, DescribeValue(flag));
			else
				REX::WARN("[sf] {} - unreadable", path);
		}

		// Walk only the FIRST path that resolves, starting at the reticle. The
		// whole-menu walk spent its budget on sibling components; the target
		// icons all hang off the reticle, so start there. (An earlier version
		// walked every candidate, and since 'root1.Menu_mc', 'root' and
		// 'root.Menu_mc' are overlapping views of one tree, two thirds of the
		// budget went on re-dumping the same objects.)
		const char* candidates[]{
			reticlePath.c_str(),
			rootPath ? rootPath : "root",
			"root",
		};

		for (const auto* path : candidates) {
			if (!path || !path[0])
				continue;
			if (!root->IsAvailable(path)) {
				REX::INFO("[sf] path '{}' - not available", path);
				continue;
			}
			RE::Scaleform::GFx::Value value;
			if (root->GetVariable(&value, path)) {
				REX::INFO("[sf] path '{}' - resolved, walking breadth-first (boilerplate {})",
					path, bScaleformSkipBoilerplate.GetValue() ? "skipped" : "included");
				WalkBreadthFirst(path, value, depth);
				break;
			}
			REX::INFO("[sf] path '{}' - available but GetVariable failed", path);
		}

		REX::INFO("[sf] ==== dump end ({} lines) ====", g_scaleformLines.load(std::memory_order_relaxed));
	}

	// ---------------------------------------------------------------------------
	// Tap 5: interposing on the HUD's data-model entry point.
	//
	// The target list cannot be read from outside - `LowFreqTargetData` is a
	// private member of ShipReticle - and `uniqueID`, which the panel's confirm
	// action needs as `uBodyID`, is only ever the *key* of a private array, so
	// it is not on the icon clips either.
	//
	// But the engine hands the data in through a PUBLIC function, and public
	// members can be replaced. Stash the original under another name, put a
	// native function in its place, read the argument on the way past, then call
	// the original so the HUD behaves exactly as before. This is the same
	// CreateFunction/SetMember pattern CommonLibSF already uses for
	// `GameMenuBase::RegisterNativeFunction`, and it needs no SWF patch.
	//
	// The handler runs on the UI thread every time the engine refreshes the
	// list, so it does nothing at all unless a capture has been requested.
	// ---------------------------------------------------------------------------

	constexpr const char* kInterposeFunc = "UpdateLowFrequencyData";
	constexpr const char* kOriginalStash = "ShipNavPanel_OrigUpdateLowFreq";

	class TargetRowVisitor : public RE::Scaleform::GFx::Value::ArrayVisitor
	{
	public:
		void Visit(std::uint32_t a_index, const RE::Scaleform::GFx::Value& a_value) override
		{
			RE::Scaleform::GFx::Value entry = a_value;
			const auto                field = [&](const char* a_name) -> std::string {
                RE::Scaleform::GFx::Value member;
                return entry.GetMember(a_name, &member) ? DescribeValue(member) : "-";
			};

			// The low-frequency payload carries no name, but the ids look like
			// form ids: the five planets came back sequential (0x5E30E..0x5E313,
			// sibling records) and a dynamically spawned POI came back
			// FF-prefixed. If that holds, names resolve in C++ and the panel
			// needs nothing more from Scaleform.
			RE::Scaleform::GFx::Value idValue;
			std::string               formInfo = "-";
			if (entry.GetMember("uniqueID", &idValue)) {
				const auto id = static_cast<std::uint32_t>(idValue.IsUInt() ? idValue.GetUInt() : static_cast<std::uint32_t>(idValue.GetNumber()));
				if (const auto form = RE::TESForm::LookupByID(id))
					formInfo = std::format("formType={:02X}", std::to_underlying(form->GetFormType()));
				else
					formInfo = "no form";
			}

			REX::INFO("[nav] [{:>2}] name={} uniqueID={} ({}) type={} onScreenOK={} offScreenOK={}",
				a_index, field("name"), field("uniqueID"), formInfo, field("uTargetType"),
				field("bAllowedOnScreen"), field("bAllowedOffScreen"));

			// The entry schema is not documented anywhere, so spell every entry
			// out in full. This used to dump entry 0 alone, which was enough to
			// find the field names but not to answer questions that only some
			// entries can answer - the open one being whether a moon carries any
			// reference to the planet it orbits. Entry 0 was a POI, so it could
			// never have shown that either way. A capture is a one-shot manual
			// trigger; verbosity is the entire point of it.
			REX::INFO("[nav] --- full schema of entry {} ---", a_index);
			LevelCollector visitor{ "[nav] entry", nullptr };
			entry.VisitMembers(&visitor);
			REX::INFO("[nav] --- end schema ---");
		}
	};

	void CaptureTargetData(const RE::Scaleform::GFx::Value& a_data)
	{
		RE::Scaleform::GFx::Value data = a_data;
		if (!data.IsObject()) {
			REX::WARN("[nav] capture: argument is {}, not an object", DescribeValue(data));
			return;
		}

		g_scaleformLines.store(0, std::memory_order_relaxed);
		REX::INFO("[nav] ==== target data capture ====");

		for (const auto* name : { "iInfoTargetIndex", "iHoverTargetIndex" }) {
			RE::Scaleform::GFx::Value value;
			if (data.GetMember(name, &value))
				REX::INFO("[nav] {} = {}", name, DescribeValue(value));
		}

		RE::Scaleform::GFx::Value targetArray;
		if (!data.GetMember("targetArray", &targetArray)) {
			REX::WARN("[nav] no 'targetArray' member - dumping what the argument does have:");
			LevelCollector visitor{ "[nav] arg", nullptr };
			data.VisitMembers(&visitor);
			return;
		}

		// LowFreq wraps the entries in `.dataA`; other feeds pass a bare array.
		RE::Scaleform::GFx::Value entries;
		if (!targetArray.GetMember("dataA", &entries) || !entries.IsArray())
			entries = targetArray;

		if (!entries.IsArray()) {
			REX::WARN("[nav] targetArray is {} - expected an array", DescribeValue(entries));
			return;
		}

		TargetRowVisitor visitor;
		entries.VisitElements(&visitor);
		REX::INFO("[nav] ==== capture end ====");
	}

	// The original method, parked on a plain AS3 Object. `ShipReticle` is a
	// sealed class (`public class ShipReticle extends BSDisplayObject`, no
	// `dynamic`), so it rejects new members - which is exactly why stashing the
	// original *on the reticle itself* failed. A bare Object is dynamic and
	// accepts them. An AS3 method reference is a bound closure, so it keeps the
	// reticle as its `this` and still behaves correctly when called from here.
	RE::Scaleform::GFx::Value g_originalHolder;

	class TargetDataInterposer : public RE::Scaleform::GFx::FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			if (g_captureRequested.exchange(false, std::memory_order_acq_rel) && a_params.argCount > 0 && a_params.args)
				CaptureTargetData(a_params.args[0]);

			// Always hand off to the original, whatever happened above - if this
			// stops running, the HUD's target display freezes.
			if (g_originalHolder.IsObject())
				g_originalHolder.Invoke(kOriginalStash, nullptr, a_params.args, a_params.argCount);
		}
	};

	// Static: FunctionHandler is refcounted starting at 1, so Scaleform's
	// AddRef/Release around the created function never drops it to zero.
	TargetDataInterposer g_interposer;

	// ---------------------------------------------------------------------------
	// The engine publishes NAMED DATA FEEDS and the SWF subscribes to them:
	//
	//   BSUIDataManager.Subscribe("TargetLowFrequencyProvider",
	//       function(e:FromClientDataEvent):* { TargetsLowFreqDataPayload = e.data; ... });
	//
	// `Subscribe` is a public static, so subscribing a native function to the
	// same feed gets the payload handed to us the way the game intends -
	// no interposition (blocked by the sealed class), no hooking, no patching.
	// The catch is reaching a *class* object from C++, which is not in the
	// display tree, so the path has to be found by probing.
	// ---------------------------------------------------------------------------

	constexpr const char* kTargetFeed = "TargetLowFrequencyProvider";

	constexpr const char* kDataManagerPaths[]{
		"BSUIDataManager",
		"Shared.AS3.Data.BSUIDataManager",
		"_global.BSUIDataManager",
		"root1.BSUIDataManager",
		"root.BSUIDataManager",
	};

	// Pulls the entry array out of a feed payload; LowFreq nests it under
	// `.dataA`, the others pass a bare array.
	bool GetEntryArray(RE::Scaleform::GFx::Value& a_data, RE::Scaleform::GFx::Value& a_out)
	{
		if (!a_data.GetMember("targetArray", &a_out))
			return false;
		RE::Scaleform::GFx::Value inner;
		if (a_out.GetMember("dataA", &inner) && inner.IsArray())
			a_out = inner;
		return a_out.IsArray();
	}

	class CandidateCollector : public RE::Scaleform::GFx::Value::ArrayVisitor
	{
	public:
		std::vector<Candidate> rows;

		void Visit(std::uint32_t a_index, const RE::Scaleform::GFx::Value& a_value) override
		{
			RE::Scaleform::GFx::Value entry = a_value;
			RE::Scaleform::GFx::Value member;
			Candidate                 row;
			if (entry.GetMember("uniqueID", &member))
				row.id = static_cast<std::uint32_t>(AsNumber(member));
			if (entry.GetMember("uTargetType", &member))
				row.type = static_cast<std::uint32_t>(AsNumber(member));
			if (entry.GetMember("name", &member) && member.IsString()) {
				const char* str = member.GetString();
				row.name = str ? str : "";
			}
			if (entry.GetMember("bIsCelestialParentBody", &member))
				row.isParentBody = member.IsBoolean() && member.GetBoolean();
			// The POI icon identity rides the same entry; capture it so the
			// faux marker can wear real station/POI art (v0.8.12).
			if (entry.GetMember("uPoiType", &member) &&
				(member.IsNumber() || member.IsInt() || member.IsUInt())) {
				row.poiType = static_cast<std::uint32_t>(AsNumber(member));
				row.havePoi = true;
				if (entry.GetMember("uPoiCategory", &member))
					row.poiCategory = static_cast<std::uint32_t>(AsNumber(member));
			}
			// Independent of the pair above: the marker STATE is what names
			// the HUD's own icon (GetLocationPOIName), so the label logic
			// needs it even when the type/category pair is absent (v0.18.0).
			if (entry.GetMember("uLocationMarkerState", &member) &&
				(member.IsNumber() || member.IsInt() || member.IsUInt())) {
				row.locMarkerState = static_cast<std::uint32_t>(AsNumber(member));
				row.haveLocState = true;
			}
			if (entry.GetMember("bMarkerDiscovered", &member) && member.IsBoolean())
				row.discovered = member.GetBoolean();
			// See Candidate::bodyID. Read whether or not it turns out to exist:
			// if it does not, nothing changes and the log says so once, which is
			// the answer to "is uniqueID the wrong number for a course".
			if (entry.GetMember("uBodyID", &member) &&
				(member.IsNumber() || member.IsInt() || member.IsUInt())) {
				row.bodyID = static_cast<std::uint32_t>(AsNumber(member));
				row.haveBodyID = true;
			}
			// The autopilot's course, per body - read whether or not the
			// course-lock key is on, because it costs one GetMember and it is
			// the only readback that feature has (see Candidate::courseLocked).
			if (entry.GetMember("bIsCruiseTargetLock", &member) && member.IsBoolean())
				row.courseLocked = member.GetBoolean();
			// Galaxy data is filled in after collection, from the body table.
			if (rows.size() <= a_index)
				rows.resize(a_index + 1);
			rows[a_index] = std::move(row);
		}
	};

	// Prints the planet records as a table - one row per word offset, one column
	// per body - so the hierarchy can be found by looking rather than by
	// predicting where it ought to be. With the bodies side by side, the system
	// id is the column that never varies, the planet id is the one that is small
	// and distinct, and the parent is the one that is mostly zero.
	void DumpPlanetRecords(const std::vector<Candidate>& a_rows)
	{
		struct Entry
		{
			std::string            name;
			std::uint32_t          id;
			std::vector<std::byte> bytes;
		};

		std::vector<Entry> entries;
		for (const auto& row : a_rows) {
			if (row.type != kTargetTypePlanet || entries.size() >= 8)
				continue;
			const auto* form = LookupPlanet(row.id);
			if (!form)
				continue;
			auto bytes = SnapshotForm(form, static_cast<std::size_t>(uDumpPlanetBytes.GetValue()));
			if (bytes.empty())
				continue;
			entries.push_back(Entry{ row.name, row.id, std::move(bytes) });
		}

		if (entries.empty()) {
			REX::WARN("[dump] no planet records could be read");
			return;
		}

		REX::INFO("[dump] ==== planet records ====");
		std::string header = std::format("[dump] {:>6}", "offset");
		for (const auto& entry : entries)
			header += std::format("  {:>10.10}", entry.name);
		REX::INFO("{}", header);

		std::string ids = std::format("[dump] {:>6}", "formID");
		std::size_t smallest = entries.front().bytes.size();
		for (const auto& entry : entries) {
			ids += std::format("  {:>10X}", entry.id);
			smallest = std::min(smallest, entry.bytes.size());
		}
		REX::INFO("{}", ids);
		REX::INFO("[dump] readable from each record: {} bytes (common {})",
			entries.front().bytes.size(), smallest);

		for (std::size_t offset = 0x30; offset + 4 <= smallest; offset += 4) {
			std::string line = std::format("[dump] +{:04X}", offset);
			for (const auto& entry : entries) {
				std::uint32_t value{};
				std::memcpy(&value, entry.bytes.data() + offset, sizeof(value));
				line += std::format("  {:10}", value);
			}
			REX::INFO("{}", line);
		}
		REX::INFO("[dump] ==== end ====");
	}

	// ---------------------------------------------------------------------------
	// Phase 6 probe A: can this plugin call Papyrus? (PHASE6-SURVEY-STATE.md §7)
	//
	// The whole "fully surveyed" feature rests on one question no amount of file
	// reading can answer. `Planet.GetSurveyPercent()` (Planet.psc:105) is the
	// native the vanilla survey quests themselves poll, and the ONLY per-body
	// survey read that covers a whole system - every UI feed that carries survey
	// state describes exactly one body. So: can it be dispatched from C++?
	//
	// Nothing about that call is verified. `DispatchMethodCall` is a pure virtual
	// at vtable slot 0x30 whose ordinal comes from a comment, not a symbol; its
	// argument functor is a `BSTThreadScrapFunction`, which CommonLibSF aliases
	// to std::function with NO size assertion; and its result arrives
	// asynchronously through an `IStackCallbackFunctor` that has to be written by
	// hand. The probe therefore walks the chain one step at a time and names the
	// step that breaks - "it did not work" is worth nothing, "step 2 returned
	// false" names the next move.
	//
	// ⚠ It runs from the per-frame task, NOT from a feed callback, and the
	// shipping sweep must do the same. A feed callback is already inside
	// Scaleform's locks, and reaching into a second engine subsystem from there
	// is the lock-order inversion that froze v0.8.x. Reading the candidate list
	// needs g_candidateMutex, so the rows are SNAPSHOTTED under the lock and the
	// lock released before any dispatch - the discipline RefreshPanel uses.
	//
	// It is also strictly read-only. GetSurveyPercent reports; it writes nothing,
	// which is what keeps the mod's "touches no save state" guarantee intact.
	// ---------------------------------------------------------------------------

	std::string ThreadIdString()
	{
		std::ostringstream out;
		out << std::this_thread::get_id();
		return out.str();
	}

	// The dispatch is asynchronous: the VM runs the call on its own schedule and
	// hands the return value to this functor, so the answer cannot be read at the
	// call site. What it logs is deliberately more than the float - the thread it
	// arrives on decides whether the shipping sweep has to marshal, and the round
	// trip decides whether a sweep can ride a refresh tick or must wait for a
	// trigger.
	class SurveyProbeCallback : public RE::BSScript::IStackCallbackFunctor
	{
	public:
		SurveyProbeCallback(std::string a_label, std::uint32_t a_formID,
			std::chrono::steady_clock::time_point a_sent, bool a_verbose) :
			_label(std::move(a_label)), _formID(a_formID), _sent(a_sent),
			// The generation this question was ASKED in. The answer is only
			// filed if the map still belongs to it - see operator().
			_epoch(g_surveyedEpoch.load(std::memory_order_acquire)), _verbose(a_verbose)
		{}

		void CallQueued() override {}
		void CallCanceled() override
		{
			if (_verbose)
				REX::WARN("[surveyed] {} - CANCELED by the VM", _label);
		}
		void StartMultiDispatch() override {}
		void EndMultiDispatch() override {}
		bool CanSave() override { return false; }

		void operator()(RE::BSScript::Variable a_result) override
		{
			const auto ms = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - _sent)
			                    .count();

			if (a_result.is<float>()) {
				const float pct = RE::BSScript::get<float>(a_result);

				// The store first, whether or not anyone is watching the log:
				// this runs on a VM thread, so the panel reads it under the
				// lock at render time rather than being touched from here.
				//
				// ⚠ Unless the world reloaded while this call was in flight -
				// ~86 ms of it - in which case the answer describes the PREVIOUS
				// save and writing it would re-poison a map that was just
				// cleared. Dropping a reading costs one sweep interval; keeping
				// a wrong one is the only way this feature can lie.
				bool discarded = false;
				if (_formID != 0) {
					// Compared against the MAP's epoch, under the map's lock:
					// that is the generation these contents belong to, so an
					// answer from an older one is rejected without any race
					// against the clear. Testing g_unsettledEpoch outside the
					// lock instead would leave the same window the clear itself
					// had - check passes, load happens, clear runs, then this
					// writes a stale value into a map now stamped as current.
					std::lock_guard lock{ g_surveyedMutex };
					if (g_surveyedEpoch.load(std::memory_order_acquire) == _epoch)
						g_surveyedPercent[_formID] = pct;
					else
						discarded = true;
				}
				if (discarded && _verbose)
					REX::INFO("[surveyed] {} - answer discarded, the world reloaded while it was "
							  "in flight",
						_label);

				if (!_verbose)
					return;

				REX::INFO("[surveyed] {} -> {:.4f} = {} ({:.1f} ms, thread {})",
					_label, pct, pct >= 1.0f ? "FULLY SURVEYED" : "incomplete",
					ms, ThreadIdString());

				// The oracle. If the info-target feed happens to be describing
				// this same body, its fSurveyPercent is the number the vanilla
				// planet card draws - so agreeing with it is what proves the
				// Papyrus native reads the same quantity the card does, rather
				// than merely something with a similar name.
				const auto sample = g_cardSample.load(std::memory_order_acquire);
				if (_formID != 0 && static_cast<std::uint32_t>(sample >> 32) == _formID) {
					float card{};
					const auto bits = static_cast<std::uint32_t>(sample & 0xFFFFFFFFu);
					std::memcpy(&card, &bits, sizeof(card));
					const bool agree = std::fabs(card - pct) < 0.001f;
					REX::INFO("[surveyed] ORACLE {}: the card says {:.4f} for the same body, "
							  "Papyrus says {:.4f}{}",
						agree ? "MATCH" : "MISMATCH", card, pct,
						agree ? " - same quantity, confirmed" :
								" - DIFFERENT QUANTITIES, the whole plan rests on these agreeing");
				}
			} else if (_verbose) {
				// A non-float answer is as interesting as a float one: it means
				// the call was accepted and returned something else, which is a
				// different failure from silence.
				REX::WARN("[surveyed] {} -> raw type {}, expected float ({:.1f} ms, thread {})",
					_label, static_cast<std::uint32_t>(a_result.GetType().GetRawType()),
					ms, ThreadIdString());
			}
		}

	private:
		std::string                           _label;
		std::uint32_t                         _formID{ 0 };
		std::chrono::steady_clock::time_point _sent;
		std::uint32_t                         _epoch{ 0 };
		bool                                  _verbose{ true };
	};

	// One dispatch. The return says whether the VM ACCEPTED the call, not whether
	// it answered - that arrives later, in the callback.
	//
	// v0.19.1: the first flight got a clean `false` here, and the reason was a
	// step this function skipped. A handle is not a callable target - the VM
	// dispatches against a script OBJECT BOUND to that handle, and nothing binds
	// one for a `Native Hidden` type by itself. That bind dance is exactly what
	// `PackVariable` does whenever CommonLibSF passes a form to Papyrus
	// (BSScriptUtil.h:494-544), and it was the missing move:
	//
	//     FindBoundObject -> (if absent) CreateObject -> BindObject -> dispatch
	//
	// Both dispatch overloads are tried, handle first then object, because they
	// are different vtable slots (0x30 and 0x31) and a failure in one is a
	// different fact from a failure in both.
	bool DispatchSurveyPercent(const RE::TESForm* a_form, std::uint32_t a_formID,
		const char* a_scriptType, std::string a_label, bool a_verbose, bool a_mayBind)
	{
		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		if (!vm)
			return false;

		auto&      handles = vm->GetObjectHandlePolicy();
		const auto handle = handles.GetHandleForObject(
			RE::BSScript::GetVMTypeID<RE::BGSPlanet::PlanetData>(), a_form);
		if (handle == handles.EmptyHandle()) {
			if (a_verbose)
				REX::WARN("[surveyed] {} - no VM handle for the form (EmptyHandle)", a_label);
			return false;
		}
		if (a_verbose)
			REX::INFO("[surveyed] {} - handle {:#x} (loaded={} available={})", a_label, handle,
				handles.IsHandleLoaded(handle), handles.IsHandleObjectAvailable(handle));

		// Step 3a - is a script object already bound? For a type vanilla calls
		// on itself (OutpostBeaconScript.psc:59 does GetCurrentPlanet().
		// GetSurveyPercent()) one may well exist already, and finding it means
		// the probe creates NOTHING.
		RE::BSTSmartPointer<RE::BSScript::Object> object;
		bool                                      bound = vm->FindBoundObject(handle, a_scriptType, false, object, false) && object;
		if (a_verbose)
			REX::INFO("[surveyed] {} - FindBoundObject: {}", a_label,
				bound ? "already bound (nothing created)" : "no object bound to this handle");

		// Step 3b - bind one. ⚠ This is the ONLY part of the probe that can add
		// anything to the VM's tables. `Planet` is Native Hidden with no script
		// variables and no properties, and vanilla binds Planet objects itself
		// whenever its own quests call this very function - but it is still a
		// write-shaped act in a mod whose whole guarantee is that it writes
		// nothing, so it has its own switch.
		if (!bound) {
			if (!a_mayBind) {
				// NOT gated on a_verbose: the sweep is silent by design, and a
				// silently dead feature with no line naming the cause is exactly
				// what this project's release checklist exists to prevent. Once
				// per session is enough to be diagnosable.
				static std::atomic<bool> s_saidWhy{ false };
				if (a_verbose || !s_saidWhy.exchange(true, std::memory_order_acq_rel))
					REX::WARN("[surveyed] {} - nothing is bound to this form and binding is turned "
							  "off, so its survey state cannot be read. Most rows will stay blank; "
							  "any body the GAME has already bound still reads normally, since "
							  "that costs nothing. Set bPanelSurveyBind (or bProbeSurveyBind, for "
							  "the probe) to mark the rest.",
						a_label);
				return false;
			}
			const auto vmInternal = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vmInternal) {
				if (a_verbose)
					REX::WARN("[surveyed] {} - no Internal::VirtualMachine, cannot bind", a_label);
				return false;
			}
			if (!vm->CreateObject(RE::BSFixedString(a_scriptType), object) || !object) {
				if (a_verbose)
					REX::WARN("[surveyed] {} - CreateObject('{}') FAILED", a_label, a_scriptType);
				return false;
			}
			vmInternal->BindObject(object, handle, false);
			bound = static_cast<bool>(object);
			if (a_verbose)
				REX::INFO("[surveyed] {} - CreateObject + BindObject: {}", a_label,
					bound ? "ok" : "left no object");
			if (!bound)
				return false;
		}

		// GetSurveyPercent takes no arguments, but the argument functor still has
		// to exist and still has to return true or the VM treats the call as
		// malformed.
		const auto args = [](RE::BSScrapArray<RE::BSScript::Variable>&) { return true; };

		{
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{
				new SurveyProbeCallback(a_label + " [by handle]", a_formID,
					std::chrono::steady_clock::now(), a_verbose)
			};
			if (vm->DispatchMethodCall(handle, RE::BSFixedString(a_scriptType),
					RE::BSFixedString("GetSurveyPercent"), args, callback, 0)) {
				if (a_verbose)
					REX::INFO("[surveyed] {} - dispatch BY HANDLE accepted (vtable slot 0x30)", a_label);
				return true;
			}
		}

		// Slot 0x30 said no. The object overload is a different slot and a
		// different lookup path, so it is worth its own attempt rather than one
		// shared verdict.
		{
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{
				new SurveyProbeCallback(a_label + " [by object]", a_formID,
					std::chrono::steady_clock::now(), a_verbose)
			};
			if (vm->DispatchMethodCall(object, RE::BSFixedString("GetSurveyPercent"),
					args, callback, 0)) {
				if (a_verbose)
					REX::INFO("[surveyed] {} - dispatch BY OBJECT accepted (vtable slot 0x31) - "
							  "the handle overload is the one to avoid",
						a_label);
				return true;
			}
		}

		if (a_verbose)
			REX::WARN("[surveyed] {} - BOTH dispatch overloads returned false with an object bound. "
					  "That is no longer a 'we forgot to bind' failure - suspect the vtable ordinals "
					  "or the argument functor ABI.",
				a_label);
		return false;
	}

	// ---------------------------------------------------------------------------
	// The shipping sweep.
	//
	// Re-reads every listed body's survey percent into g_surveyedPercent, from the
	// per-frame task - never a feed callback, for the lock-order reason above.
	// Measured cost (flight 3): ten bodies queue in 0.4 ms of this thread and the
	// answers land within ~86 ms, because the latency is the VM's own update tick
	// rather than anything per-call. So the interval below is comfort, not need.
	//
	// A body already reading 1.0 is never re-read: survey progress only goes up,
	// and skipping it saves both a dispatch and - more to the point - a bind.
	//
	// The cache is thrown away whenever the world goes unsettled, because survey
	// state is per-save and the panel is not. A mark carried across a quickload
	// would be the one way this feature can be WRONG rather than merely late.
	// ---------------------------------------------------------------------------
	std::atomic<bool> g_surveySweepInFlight{ false };

	void SweepSurveyState()
	{
		using clock = std::chrono::steady_clock;

		// ⚠ OnFrame lands on whatever BSJobs worker is free and can run on two
		// threads in the same frame - the log has shown the same logical work
		// reporting from five thread ids in one second. Without this claim, two
		// sweeps would dispatch and BIND the same bodies concurrently, which is
		// the shape of the v0.7.4 crash: two threads inside the same one-shot
		// builder, entering an engine VM. The throttle below is check-then-act
		// and cannot serve as the gate - that is the whole lesson of SingleWinner.
		//
		// Released on exit rather than latched, so a sweep that finds nothing is
		// free to try again next frame.
		const SingleWinner winner{ g_surveySweepInFlight };
		if (!winner.Won())
			return;

		// Per-save state, and the panel outlives the save. Drop it on any load.
		// The stamp is g_surveyedEpoch itself, not a private static: the READER
		// checks it too, so the map cannot be believed merely because the sweep
		// has not got round to clearing it yet.
		{
			const auto  epoch = g_unsettledEpoch.load(std::memory_order_acquire);
			std::size_t dropped = 0;
			bool        cleared = false;
			{
				// ⚠ The stamp and the clear happen TOGETHER, under the lock the
				// reader also takes, and the stamp comes AFTER the clear. Stamping
				// first - as the first cut did - leaves a window in which the
				// epoch already says "current" while the map still holds the
				// previous save's readings, which is precisely the stale mark the
				// epoch exists to prevent. A guard with a hole in it is worse than
				// none, because it stops anyone looking.
				std::lock_guard lock{ g_surveyedMutex };
				if (g_surveyedEpoch.load(std::memory_order_acquire) != epoch) {
					dropped = g_surveyedPercent.size();
					g_surveyedPercent.clear();
					g_surveyedEpoch.store(epoch, std::memory_order_release);
					cleared = true;
				}
			}
			// Logged OUTSIDE the lock: a Papyrus callback thread blocked on this
			// mutex should never be waiting on a file write.
			//
			// ⚠ Logged on EVERY clear, including one that found the map already
			// empty. PHASE6 tells the next person to look for this line after a
			// quickload to confirm the guard is running - and the first cut hid
			// it behind `dropped != 0`, so on the 2026-08-01 flight it never
			// printed once despite the guard working perfectly. A check that
			// cannot pass is not a check.
			if (cleared)
				REX::INFO("[surveyed] world reloaded (epoch {}) - dropped {} cached survey reading(s)",
					epoch, dropped);
		}

		// Steady-clock ticks rather than a time_point, so the throttle can live
		// in an atomic. 0 is the "never swept" sentinel, and opening the panel
		// restores it so the first sweep of a session is immediate.
		const auto  now = clock::now();
		const auto  nowTicks = now.time_since_epoch().count();
		const float interval = std::max(fSurveySweepSeconds.GetValue(), 0.5f);
		{
			const auto last = g_lastSweepTicks.load(std::memory_order_acquire);
			if (last != 0 &&
				std::chrono::duration<float>(clock::duration{ nowTicks - last }).count() < interval)
				return;
		}
		// Stamped HERE, not at each exit. Every path below is a completed
		// attempt, and the early ones (no rows, all complete, no VM) are exactly
		// the paths that would otherwise re-run every frame - locking the hot
		// candidate mutex at frame rate for nothing.
		g_lastSweepTicks.store(nowTicks, std::memory_order_release);

		// ★ Gate on the feed's uTargetType, never on "the row has an id" - a POI
		// or station row carries a REFR id, not a PNDT one. Snapshot under the
		// lock, release, and only then touch the VM.
		std::vector<std::pair<std::uint32_t, std::string>> targets;
		{
			std::lock_guard lock{ g_candidateMutex };
			targets.reserve(g_candidates.size());
			for (const auto& row : g_candidates) {
				if (row.type != kTargetTypePlanet || row.id == 0)
					continue;
				targets.emplace_back(row.id, row.name);
			}
		}
		if (targets.empty())
			return;

		// Drop the ones already known complete before entering the VM at all.
		const auto listed = targets.size();
		{
			std::lock_guard lock{ g_surveyedMutex };
			std::erase_if(targets, [](const auto& a_target) {
				const auto found = g_surveyedPercent.find(a_target.first);
				return found != g_surveyedPercent.end() && found->second >= 1.0f;
			});
		}
		const auto complete = listed - targets.size();
		if (targets.empty())
			return;

		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		if (!vm)
			return;

		RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
		if (!vm->GetScriptObjectType(RE::BSScript::GetVMTypeID<RE::BGSPlanet::PlanetData>(), typeInfo) ||
			!typeInfo) {
			// The type is bound in 1.16.244 (measured); if a future build ever
			// stops binding it, say so once and leave the marks unmarked rather
			// than retrying into the VM every interval forever.
			static std::atomic<bool> s_warned{ false };
			if (!s_warned.exchange(true, std::memory_order_acq_rel))
				REX::WARN("[surveyed] the VM binds no script type to PNDT - survey marks are off "
						  "for this session");
			return;
		}
		const std::string scriptType = SafeStr(typeInfo->name.c_str());

		const bool    mayBind = bPanelSurveyBind.GetValue();
		std::uint32_t sent = 0;
		for (const auto& [formID, name] : targets) {
			const auto* form = LookupPlanet(formID);
			if (!form)
				continue;
			if (DispatchSurveyPercent(form, formID, scriptType.c_str(), name, false, mayBind))
				++sent;
		}

		// One line per sweep, and only when the count moves - this runs for as
		// long as the player cruises.
		static std::atomic<std::uint32_t> s_lastSent{ 0xFFFFFFFF };
		if (s_lastSent.exchange(sent, std::memory_order_acq_rel) != sent)
			REX::INFO("[surveyed] sweep: {} of {} listed body/bodies queried ({} already complete, "
					  "never re-read)",
				sent, listed, complete);
	}

	void ProbeSurveyVM()
	{
		REX::INFO("[surveyed] ==== probe A: Papyrus Planet.GetSurveyPercent ====");
		REX::INFO("[surveyed] probe thread {}", ThreadIdString());

		// Step 1 - the VM. GameVM::GetSingleton() is the ONE Address Library id
		// this route costs (ID::GameVM::Singleton, 937585). It is not free and
		// must not be written up as if it were.
		const auto game = RE::GameVM::GetSingleton();
		if (!game) {
			REX::WARN("[surveyed] step 1 FAILED: GameVM singleton is null - the address id "
					  "did not resolve for this build");
			return;
		}
		const auto vm = game->GetVM();
		if (!vm) {
			REX::WARN("[surveyed] step 1 FAILED: GameVM holds no IVirtualMachine");
			return;
		}
		// A frozen VM refuses dispatches, and it would refuse them the same way a
		// wrong vtable slot does - with a bare `false`. Rule it out here rather
		// than wondering later.
		REX::INFO("[surveyed] step 1 OK: GameVM {} -> IVirtualMachine {} (frozen={}, completely={})",
			static_cast<const void*>(game), static_cast<const void*>(vm),
			game->frozen, vm->IsCompletelyFrozen());

		// Whatever the previous batch did to the VM shows up here, not in the run
		// that caused it - the dispatches are asynchronous, so the aftermath is
		// only visible next time round.
		{
			static std::uint32_t s_lastFlags{ 0 };
			static bool          s_haveLast{ false };
			if (s_haveLast)
				REX::INFO("[surveyed] VM since the previous probe: overflowFlags {} -> {} ({}), "
						  "overage suspend/running/stack = {}/{}/{}",
					s_lastFlags, game->overflowFlags,
					s_lastFlags == game->overflowFlags ? "unchanged - the batch did not stress it" :
														 "CHANGED - the batch stressed it, keep the sweep rate low",
					game->initialSuspendOverageTime, game->initialRunningOverageTime,
					game->initialStackMemoryOverageTime);
			s_lastFlags = game->overflowFlags;
			s_haveLast = true;
		}

		// Step 2 - does the VM bind a script type to the PNDT form type? `Planet`
		// is declared `Native Hidden` and Planet.pex ships in Starfield - Misc,
		// so the type should be bound. If it is not, every step below is
		// unreachable and the Papyrus route is dead.
		const auto                                        typeID = RE::BSScript::GetVMTypeID<RE::BGSPlanet::PlanetData>();
		RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
		if (!vm->GetScriptObjectType(typeID, typeInfo) || !typeInfo) {
			REX::WARN("[surveyed] step 2 FAILED: no script object type bound to PNDT (typeID {}). "
					  "The VM does not bind native-hidden types this way - the Papyrus route is "
					  "dead, see PHASE6-SURVEY-STATE.md route 3.",
				typeID);
			return;
		}
		const std::string scriptType = SafeStr(typeInfo->name.c_str());
		REX::INFO("[surveyed] step 2 OK: PNDT typeID {} binds script type '{}' (expected 'Planet')",
			typeID, scriptType);

		// Steps 3-5 - a handle and a dispatch per listed body.
		//
		// ★ Gate on the feed's uTargetType, never on "the row has an id". A POI
		// or station row carries a REFR id; LookupPlanet would reject it anyway,
		// but the gate is what records the rule. This is the v0.11.1 class of bug
		// - Venus wore a station's badge because presence was read as identity.
		struct Target
		{
			std::uint32_t id{ 0 };
			std::string   name;
			bool          isMoon{ false };
		};
		std::vector<Target> targets;
		{
			std::lock_guard lock{ g_candidateMutex };
			targets.reserve(g_candidates.size());
			for (const auto& row : g_candidates) {
				if (row.type != kTargetTypePlanet || row.id == 0)
					continue;
				// A moon is a body whose GNAM names a parent planet - Sol is
				// system 0, so presence is haveGalaxy, never a non-zero id.
				targets.push_back(Target{ row.id, row.name,
					row.haveGalaxy && row.galaxy.parentPlanetID != 0 });
			}
		}
		// The lock is released HERE, before anything below touches the VM.

		if (targets.empty()) {
			REX::WARN("[surveyed] no planet rows to probe - run this in cruise, with the system listed");
			return;
		}

		const auto moons = std::count_if(targets.begin(), targets.end(),
			[](const Target& a_target) { return a_target.isMoon; });

		// The cap exists because the first call is the dangerous one - see the
		// note on uProbeSurveyMaxBodies. Moons sort to the front once the cap is
		// lifted off 1, so a small cap still reaches the case the feature is for
		// rather than spending itself on whichever planet happened to be first.
		const auto cap = uProbeSurveyMaxBodies.GetValue();
		if (cap != 1)
			std::stable_partition(targets.begin(), targets.end(),
				[](const Target& a_target) { return a_target.isMoon; });
		const auto planned = cap == 0 ? targets.size() : std::min<std::size_t>(cap, targets.size());

		REX::INFO("[surveyed] step 3-5: {} listed body/bodies ({} moon(s)); dispatching for {}. "
				  "A MOON must answer too - the multi-body case is what the feature exists for, "
				  "so once one call is proven survivable set uProbeSurveyMaxBodies=0 for all.",
			targets.size(), moons, planned);

		targets.resize(planned);

		const auto    batchStart = std::chrono::steady_clock::now();
		std::uint32_t accepted = 0;
		for (const auto& target : targets) {
			const auto* form = LookupPlanet(target.id);
			if (!form) {
				REX::WARN("[surveyed] {:08X} '{}' is not a PNDT form - skipped", target.id, target.name);
				continue;
			}
			if (DispatchSurveyPercent(form, target.id, scriptType.c_str(),
					std::format("{:08X} {}{}", target.id, target.isMoon ? "moon " : "", target.name),
					true, bProbeSurveyBind.GetValue()))
				++accepted;
			else
				REX::WARN("[surveyed] {:08X} '{}' - DispatchMethodCall returned FALSE",
					target.id, target.name);
		}

		REX::INFO("[surveyed] {} of {} dispatch(es) accepted, queued in {:.1f} ms. Results follow "
				  "ASYNCHRONOUSLY - if none arrive at all, the BSTThreadScrapFunction ABI is wrong "
				  "and this is a CommonLibSF question, not a game one.",
			accepted, targets.size(),
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - batchStart).count());
		REX::INFO("[surveyed] ==== probe A end (dispatch phase) ====");
	}

	// The HUD offers only the handful of bodies it considers relevant - sitting
	// in Jemison's gravity well it listed one of Alpha Centauri's eight moons -
	// which makes for a list that is really "what is near", not "what is here".
	// The master file knows the whole system, so the rest are added from it.
	//
	// They are marked `fromFeed = false`: they can be seen and scrolled past,
	// but the game gives no bearing or distance for them, so the arrow cannot
	// point at one and the list says so by leaving the distance blank.
	//
	// The entries must go on the END, because everything up to `bearings.rows`
	// stays index-aligned with the high-frequency feed.
	void AppendSystemBodies(std::vector<Candidate>& a_rows)
	{
		if (!bListWholeSystem.GetValue())
			return;

		// ★ Sol is star system 0, so zero is DATA here, not "absent" - the
		// presence test is haveGalaxy alone. This function shipped with
		// `systemID != 0` and the whole-system list was silently dead in Sol
		// for five versions: every earlier whole-system test happened to run
		// in other systems, and the settlement recipe had already made the
		// identical mistake. Third strike for this trap - see the settled
		// list in TODO.md.
		std::uint32_t system = 0;
		bool          haveSystem = false;
		for (const auto& row : a_rows) {
			if (row.haveGalaxy) {
				system = row.galaxy.systemID;
				haveSystem = true;
				break;
			}
		}
		if (!haveSystem)
			return;

		std::lock_guard lock{ g_bodyTableMutex };
		const auto      found = g_bodiesBySystem.find(system);
		if (found == g_bodiesBySystem.end())
			return;

		for (const auto formID : found->second) {
			const auto entry = g_bodyTable.find(formID);
			if (entry == g_bodyTable.end() || !entry->second.authored || entry->second.name.empty())
				continue;  // generated bodies are the HUD's business, not ours

			// Moons list only once the HUD actually tracks them (v0.8.7, the
			// tester's call): a dash-row moon cannot be pointed at, and it
			// only fills in right next to its parent - by which point the
			// feed offers it anyway - so listing it from across the system
			// was noise. Planets keep their dash rows: locking one across
			// the system and flying at it is the whole point of the list.
			// The one exception is the LOCKED moon: its row stays listed
			// wherever you are, or the lock could not be cleared - and a
			// lock is what keeps the blips hidden, so an unclearable one
			// would hide them forever.
			if (entry->second.galaxy.parentPlanetID != 0 &&
				formID != g_lockedID.load(std::memory_order_acquire))
				continue;

			const auto already = std::find_if(a_rows.begin(), a_rows.end(),
				[&](const Candidate& a_row) { return a_row.id == formID; });
			if (already != a_rows.end())
				continue;

			Candidate row;
			row.id = formID;
			row.type = kTargetTypePlanet;
			row.name = entry->second.name;
			row.galaxy = entry->second.galaxy;
			row.haveGalaxy = true;
			row.fromFeed = false;
			a_rows.push_back(std::move(row));
		}
	}

	// Logged once per system so any bug report carries the hierarchy the mod
	// believes in, without costing anything in normal play.
	void ReportGalaxyData(const std::vector<Candidate>& a_rows)
	{
		static std::atomic<std::uint32_t> s_lastSystem{ 0xFFFFFFFF };
		std::uint32_t                     system = 0;
		bool                              haveSystem = false;
		for (const auto& row : a_rows) {
			if (row.haveGalaxy) {
				system = row.galaxy.systemID;
				haveSystem = true;
				break;
			}
		}
		if (!haveSystem || s_lastSystem.exchange(system, std::memory_order_acq_rel) == system)
			return;

		// A line per body, once per system entered - a trace of what the parse
		// produced, and verbose-only for the same reason.
		if (!bVerboseLog.GetValue())
			return;

		REX::INFO("[galaxy] system {} - from the body table", system);
		for (const auto& row : a_rows) {
			if (row.haveGalaxy)
				REX::INFO("[galaxy]   {:<28} system={} parent={} planet={}",
					row.name, row.galaxy.systemID, row.galaxy.parentPlanetID, row.galaxy.planetID);
		}
	}

	class DataFeedHandler : public RE::Scaleform::GFx::FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			if (a_params.argCount < 1 || !a_params.args)
				return;

			// The callback receives a FromClientDataEvent; the payload is `.data`.
			RE::Scaleform::GFx::Value event = a_params.args[0];
			RE::Scaleform::GFx::Value data;
			if (!event.IsObject() || !event.GetMember("data", &data))
				data = event;

			// This callback runs on the engine's own UI thread, so it is the safe
			// place to create objects and read menu state - unlike the per-frame
			// task, which crashed v0.1.3 by doing it from elsewhere.
			if (WorldSettled()) {
				TryCreateArrow();
				TryCreatePanel();
				TryCreateChromeProbe();
				RefreshCruiseState();
			}

			// The info target rides the payload, not the entries - captured for
			// the overlap pass's exemption. GetMember failing (or the index
			// arriving as -1) both mean "none".
			{
				RE::Scaleform::GFx::Value idxVal;
				g_infoTargetIndex.store(
					data.IsObject() && data.GetMember("iInfoTargetIndex", &idxVal) ?
						static_cast<std::int32_t>(AsNumber(idxVal)) :
						-1,
					std::memory_order_release);
			}

			// Rebuild the candidate list on every update - this is what the arrow
			// points at, so it cannot be gated on a capture request. The list is
			// index-aligned with the high-frequency feed, which is how a name gets
			// matched to an angle.
			RE::Scaleform::GFx::Value entries;
			if (GetEntryArray(data, entries)) {
				CandidateCollector collector;
				entries.VisitElements(&collector);

				// Find GNAM if it is not yet known, then read it - in that
				// order, since the search needs the whole set as evidence.
				if (g_dumpPlanetsRequested.exchange(false, std::memory_order_acq_rel))
					DumpPlanetRecords(collector.rows);

				for (auto& row : collector.rows) {
					if (row.id)
						row.haveGalaxy = ReadGalaxyData(row.id, row.galaxy);
				}
				ReportGalaxyData(collector.rows);
				AppendSystemBodies(collector.rows);

				std::lock_guard lock{ g_candidateMutex };
				const auto      previous = std::move(g_candidates);
				g_candidates = std::move(collector.rows);
				// Carry distances over; they come from the other feed.
				for (auto& row : g_candidates) {
					for (const auto& old : previous) {
						if (old.id == row.id) {
							row.distance = old.distance;
							break;
						}
					}
				}

				// Which body the autopilot is actually flying to, straight from
				// the engine - its own word rather than the mod's belief.
				//
				// ⚠ This is also the AUDIT. The engine does not refuse an id it
				// cannot use: it SUBSTITUTES, and the tester caught it doing so
				// (a sensor contact's id came back as a course on the system's
				// star). A wrong destination that says nothing is the worst
				// outcome this feature has, so a substitution is a WARN even
				// though it repeats with a player action, while agreement stays
				// behind the verbose flag with the rest of the trace.
				{
					std::uint32_t    course = 0;
					std::string_view courseName;
					for (const auto& row : g_candidates) {
						if (row.fromFeed && row.courseLocked) {
							course = row.id;
							courseName = row.name;
							break;
						}
					}

					if (const auto asked = g_courseAskedID.load(std::memory_order_acquire);
						asked != 0 && course != 0) {
						g_courseAskedID.store(0, std::memory_order_release);
						if (course != asked)
							REX::WARN("[course] asked the engine for {:08X} and it locked a course on "
									  "'{}' ({:08X}) instead - it substituted a body it could use for "
									  "one it could not",
								asked, courseName, course);
					}

					if (bVerboseLog.GetValue()) {
						static std::uint32_t s_lastCourse = 0;
						if (course != s_lastCourse) {
							s_lastCourse = course;
							if (course != 0)
								REX::INFO("[course] the engine reports a course locked on '{}' ({:08X})",
									courseName, course);
							else
								REX::INFO("[course] the engine reports no course locked");
						}
					}
				}
			}

			if (g_captureRequested.exchange(false, std::memory_order_acq_rel))
				CaptureTargetData(data);
		}
	};

	DataFeedHandler g_feedHandler;

	// The high-frequency feed carries each target's SCREEN POSITION, which is
	// what a blip-labelling overlay needs: `screenPositionX/Y` are percentages,
	// converted by the SWF with GlobalFunc.ConvertScreenPercentsToLocalPoint.
	constexpr const char* kHighFeed = "TargetHighFrequencyProvider";

	class HighFeedRowVisitor : public RE::Scaleform::GFx::Value::ArrayVisitor
	{
	public:
		void Visit(std::uint32_t a_index, const RE::Scaleform::GFx::Value& a_value) override
		{
			RE::Scaleform::GFx::Value entry = a_value;
			const auto                field = [&](const char* a_name) -> std::string {
                RE::Scaleform::GFx::Value member;
                return entry.GetMember(a_name, &member) ? DescribeValue(member) : "-";
			};
			REX::INFO("[nav] hi[{:>2}] screenX={} screenY={} onScreen={} distance={}",
				a_index, field("screenPositionX"), field("screenPositionY"),
				field("onScreen"), field("distance"));
			if (a_index == 0) {
				REX::INFO("[nav] --- full schema of high-freq entry 0 ---");
				LevelCollector visitor{ "[nav] hi0", nullptr };
				entry.VisitMembers(&visitor);
				REX::INFO("[nav] --- end schema ---");
			}
		}
	};

	class BearingCollector : public RE::Scaleform::GFx::Value::ArrayVisitor
	{
	public:
		struct Row
		{
			double angle{ 0.0 };
			double distance{ 0.0 };
			// Percentages, y bottom-up; -1 is the engine's "unprojectable"
			// sentinel. Only read by the duplicate-name disambiguation.
			double screenX{ -1.0 };
			double screenY{ -1.0 };
		};
		std::vector<Row> rows;

		void Visit(std::uint32_t a_index, const RE::Scaleform::GFx::Value& a_value) override
		{
			RE::Scaleform::GFx::Value entry = a_value;
			RE::Scaleform::GFx::Value member;
			Row                       row;
			if (entry.GetMember("angleToCrosshair", &member))
				row.angle = AsNumber(member);
			if (entry.GetMember("distance", &member))
				row.distance = AsNumber(member);
			if (entry.GetMember("screenPositionX", &member))
				row.screenX = AsNumber(member);
			if (entry.GetMember("screenPositionY", &member))
				row.screenY = AsNumber(member);
			if (rows.size() <= a_index)
				rows.resize(a_index + 1);
			rows[a_index] = row;
		}
	};

	// ---------------------------------------------------------------------------
	// Phase 6 probe B: the body dossier riding InfoTargetProvider.
	//
	// `TargetOnlyData.PlanetCardInfo` is the one structure in the ship HUD movie
	// that carries fSurveyPercent, and vanilla's own test for fully surveyed is
	// `fSurveyPercent >= 1` (BodyDataInfo.as:164). It describes ONE body - the
	// info target - so it cannot power the feature, but it is a free ORACLE: if
	// probe A's float agrees with this one for the same body, the Papyrus native
	// and the planet card are reading the same quantity. That is currently an
	// inference from a shared name and nothing more.
	//
	// It also settles the ID SPACE. uBodyID is the star map's and the almanac's
	// identity key, while the ship feed's is uniqueID - and uniqueID is a form id
	// (PHASE1). Whether they are the same number decides whether a row join can
	// be written at all, so both are logged side by side. ⚠ Do not write that
	// join before reading this log: a small dense integer means uBodyID is a
	// galaxy body index, and every join against it would be wrong.
	//
	// Turn it on with bProbeStarmapFeed=true and sStarmapFeed=InfoTargetProvider.
	// ---------------------------------------------------------------------------
	bool GetPlanetCardInfo(RE::Scaleform::GFx::Value& a_data, RE::Scaleform::GFx::Value& a_card)
	{
		if (!a_data.IsObject())
			return false;
		// Vanilla subscribes the provider straight into its TargetOnlyData
		// member (SpaceshipHudMenu.as:416), so the dossier should sit at the top
		// of the payload - but that nesting is read off decompiled source, so
		// try the wrapped shape too rather than concluding "absent".
		if (a_data.GetMember("PlanetCardInfo", &a_card) && a_card.IsObject())
			return true;
		RE::Scaleform::GFx::Value only;
		if (a_data.GetMember("TargetOnlyData", &only) && only.IsObject() &&
			only.GetMember("PlanetCardInfo", &a_card) && a_card.IsObject())
			return true;
		return false;
	}

	// Logs the dossier's decisive fields, and only when they CHANGE. The feed
	// publishes at UI rate; a line per publish would bury the log and say nothing
	// a line per change does not - and the question this answers is whether
	// fSurveyPercent MOVES live while a scan completes from the pilot seat.
	void WatchPlanetCard(RE::Scaleform::GFx::Value& a_data)
	{
		RE::Scaleform::GFx::Value card;
		if (!GetPlanetCardInfo(a_data, card))
			return;

		const auto field = [&](const char* a_name) -> std::string {
			RE::Scaleform::GFx::Value member;
			return card.GetMember(a_name, &member) ? DescribeValue(member) : "-";
		};

		// Kept numerically as well, for probe A's oracle check. uBodyID is a
		// FORM ID - measured 2026-08-01, not assumed: the values land in the
		// same block as the PNDT ids the mod already resolves, where a galaxy
		// body index would have been a single digit.
		{
			RE::Scaleform::GFx::Value idVal;
			RE::Scaleform::GFx::Value pctVal;
			// Published together or not at all - a half-sample is what tore.
			if (card.GetMember("uBodyID", &idVal) && card.GetMember("fSurveyPercent", &pctVal)) {
				const auto  id = static_cast<std::uint32_t>(AsNumber(idVal));
				const float pct = static_cast<float>(AsNumber(pctVal));
				std::uint32_t bits{};
				std::memcpy(&bits, &pct, sizeof(bits));
				g_cardSample.store((static_cast<std::uint64_t>(id) << 32) | bits,
					std::memory_order_release);
			}
		}

		auto line = std::format("uBodyID={} sBodyName={} fSurveyPercent={} iType={} iScanLevel={}",
			field("uBodyID"), field("sBodyName"), field("fSurveyPercent"),
			field("iType"), field("iScanLevel"));

		// The feed callbacks land on whatever worker is free, so the last-seen
		// line is shared state like any other.
		static std::mutex  s_mutex;
		static std::string s_last;
		{
			std::lock_guard lock{ s_mutex };
			if (line == s_last)
				return;
			s_last = std::move(line);
			REX::INFO("[surveyed] card {}", s_last);
		}
	}

	// Reads one element of a feed array. GFx::Value exposes no GetElement, but
	// VisitElements takes an index and a count, so a one-element visit is the
	// public way to reach a single entry.
	class SingleElement : public RE::Scaleform::GFx::Value::ArrayVisitor
	{
	public:
		RE::Scaleform::GFx::Value value;
		bool                      found{ false };

		void Visit(std::uint32_t, const RE::Scaleform::GFx::Value& a_value) override
		{
			value = a_value;
			found = true;
		}
	};

	// Logs the whole shape of whatever the star map feed delivers, once per
	// press of the capture trigger rather than continuously - it fires as often
	// as any other feed and would otherwise bury the log.
	class StarmapProbeHandler : public RE::Scaleform::GFx::FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			const auto seen = g_starmapCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;

			// The payload comes out first: the card watch runs on EVERY publish
			// (it logs only on change), while the full dump waits for a keypress.
			RE::Scaleform::GFx::Value data;
			bool                      haveData = false;
			if (a_params.argCount >= 1 && a_params.args) {
				RE::Scaleform::GFx::Value event = a_params.args[0];
				if (!event.IsObject() || !event.GetMember("data", &data))
					data = event;
				haveData = true;
				WatchPlanetCard(data);
			}

			if (!g_starmapDumpRequested.exchange(false, std::memory_order_acq_rel)) {
				if (seen == 1)
					REX::INFO("[starmap] feed is LIVE outside the map - {} callback(s) so far. "
							  "Press the scanner key to dump its contents.",
						seen);
				return;
			}

			REX::INFO("[starmap] ==== dump after {} callback(s) ====", seen);
			if (!haveData) {
				REX::WARN("[starmap] callback carried no argument");
				return;
			}

			LevelCollector top{ "[starmap] payload", nullptr };
			data.VisitMembers(&top);

			// ★ That dump is FLAT: LevelCollector only queues children when it is
			// given somewhere to queue them, and this one is not. No capture this
			// project has ever taken could see a nested field - which is why
			// PlanetCardInfo has to be descended into by name rather than waited
			// for.
			RE::Scaleform::GFx::Value card;
			if (GetPlanetCardInfo(data, card)) {
				REX::INFO("[starmap] --- PlanetCardInfo: the body dossier ---");
				LevelCollector cardVisitor{ "[starmap] pci", nullptr };
				card.VisitMembers(&cardVisitor);
			} else {
				REX::INFO("[starmap] no PlanetCardInfo on this payload - either the subscribed feed "
						  "is not InfoTargetProvider, or the engine leaves the member empty here "
						  "(which would kill the dossier route outright)");
			}

			// The info target's index and the entry it points at, side by side
			// with the dossier above: this pairing is what says whether uBodyID
			// and uniqueID are the same number for the same body.
			{
				RE::Scaleform::GFx::Value idxVal;
				const auto                index = data.GetMember("iInfoTargetIndex", &idxVal) ?
				                                      static_cast<std::int32_t>(AsNumber(idxVal)) :
				                                      -1;
				RE::Scaleform::GFx::Value entries;
				if (index >= 0 && GetEntryArray(data, entries) && entries.IsArray()) {
					SingleElement one;
					entries.VisitElements(&one, static_cast<std::uint32_t>(index), 1);
					if (one.found && one.value.IsObject()) {
						RE::Scaleform::GFx::Value member;
						const auto                entryField = [&](const char* a_name) -> std::string {
                            return one.value.GetMember(a_name, &member) ? DescribeValue(member) : "-";
						};
						REX::INFO("[starmap] iInfoTargetIndex={} -> entry uniqueID={} uTargetType={} name={}",
							index, entryField("uniqueID"), entryField("uTargetType"),
							entryField("name"));
					} else {
						REX::INFO("[starmap] iInfoTargetIndex={} but the entry could not be read", index);
					}
				} else {
					REX::INFO("[starmap] iInfoTargetIndex={} (no entry to pair it with)", index);
				}
			}

			// The bodies are expected to sit in an array much like the target
			// feed's; dump every entry, since which one is a moon is the point.
			RE::Scaleform::GFx::Value entries;
			if (GetEntryArray(data, entries) && entries.IsArray()) {
				class EntryDump : public RE::Scaleform::GFx::Value::ArrayVisitor
				{
				public:
					void Visit(std::uint32_t a_index, const RE::Scaleform::GFx::Value& a_value) override
					{
						RE::Scaleform::GFx::Value entry = a_value;
						REX::INFO("[starmap] --- entry {} ---", a_index);
						LevelCollector visitor{ "[starmap] e", nullptr };
						entry.VisitMembers(&visitor);
					}
				};
				EntryDump dump;
				entries.VisitElements(&dump);
			}
			REX::INFO("[starmap] ==== dump end ====");
		}
	};

	StarmapProbeHandler g_starmapProbeHandler;

	// ---------------------------------------------------------------------------
	// SPEAKING to the engine.
	//
	// Everything else this plugin does is a read or a Scaleform-side draw. This is
	// the one place it asks the game to change the player's state, and the route
	// is the SWF's own: BSUIDataManager is a class of public statics, reached
	// exactly the way Shared.GlobalFunc is reached for PlayMenuSound - GetVariable
	// the class, Invoke the method. Nothing hooked, nothing patched, no address
	// ids, and what comes out is indistinguishable from the event the reticle
	// dispatches when the player presses the key themselves.
	//
	// ⚠ It runs on the UI thread, driven from the high-frequency feed handler
	// beside RefreshPanel. Not the per-frame task (a cross-thread Scaleform call
	// is the v0.1.3 crash) and not the input thread (same reason, and the reason
	// the input side does nothing but store an atomic).
	// ---------------------------------------------------------------------------

	// Which route the last dispatch took: 0 none yet, 1 = constructing the event
	// class and calling dispatchEvent (vanilla's own shape), 2 = the
	// dispatchCustomEvent helper. **Route 1 is CONFIRMED in game** - a
	// `flash.events.Event` does come back from CreateObject, which had been the
	// open question, since every other class this mod constructs is a GAME class.
	// The fallback stays because it costs nothing and it is latched: a class that
	// will not construct is not re-attempted on every press.
	std::atomic<std::uint32_t> g_dispatchRoute{ 0 };

	bool DispatchHudEvent(RE::Scaleform::GFx::ASMovieRootBase* a_root, const char* a_type,
		const RE::Scaleform::GFx::Value* a_params)
	{
		using V = RE::Scaleform::GFx::Value;

		V manager;
		if (!a_root->GetVariable(&manager, "Shared.AS3.Data.BSUIDataManager") ||
			!(manager.IsObject() || manager.IsDisplayObject())) {
			REX::WARN("[dispatch] '{}' not sent - BSUIDataManager did not resolve", a_type);
			return false;
		}

		V type;
		a_root->CreateString(&type, a_type);

		V args[2];
		args[0] = type;
		if (a_params)
			args[1] = *a_params;
		const std::uint32_t argc = a_params ? 2 : 1;

		// Vanilla's exact shape first. Every class this mod has constructed so
		// far has been a GAME class (UserEventData, ButtonBaseData, Matrix3D),
		// so a flash builtin arriving through CreateObject is unproven - which
		// is what the fallback and the route line are for.
		if (g_dispatchRoute.load(std::memory_order_acquire) != 2) {
			V ev;
			if (a_params)
				a_root->CreateObject(&ev, "Shared.AS3.Events.CustomEvent", args, 2);
			else
				a_root->CreateObject(&ev, "flash.events.Event", args, 1);

			if (ev.IsObject() && manager.Invoke("dispatchEvent", nullptr, &ev, 1)) {
				if (g_dispatchRoute.exchange(1, std::memory_order_acq_rel) == 0)
					REX::INFO("[dispatch] route: new {}(...) -> BSUIDataManager.dispatchEvent "
							  "(vanilla's own shape)",
						a_params ? "CustomEvent" : "Event");
				return true;
			}
		}

		// dispatchCustomEvent(type, params = null) builds the CustomEvent itself,
		// so this needs no class construction at all. CustomEvent extends Event
		// and the engine's sink for a payload-free type reads the TYPE, so it
		// should arrive in the same place; if the two ever behave differently,
		// the route line is what says which was in use.
		if (!manager.Invoke("dispatchCustomEvent", nullptr, args, argc)) {
			REX::WARN("[dispatch] '{}' not sent - both routes refused", a_type);
			return false;
		}
		if (g_dispatchRoute.exchange(2, std::memory_order_acq_rel) != 2)
			REX::INFO("[dispatch] route: BSUIDataManager.dispatchCustomEvent('{}') - constructing "
					  "the event class directly did not work",
				a_type);
		return true;
	}


	// ---------------------------------------------------------------------------
	// The course-lock dispatch: the one event in this layer that takes a body id,
	// and the mod's only outward word to the engine.
	//
	// Vanilla sends it two ways - the reticle's own LockCourse handler with
	// uBodyID 0 (ShipReticle.as:2128), and the far-travel icon with the info
	// target's real uniqueID (FarTravelIconBase.as:99) - so the handler was known
	// to read the id. **The unknown was whether it would accept an id vanilla
	// never sends, i.e. a body that is not the current info target. It does**
	// (confirmed in game 2026-08-02): the ship turns and flies to whatever row the
	// panel was sitting on, with nothing targeted first.
	// ---------------------------------------------------------------------------
	void RunLockCourse()
	{
		using V = RE::Scaleform::GFx::Value;

		// The audit's timeout, on the HIGH feed's tick because it does not stop -
		// unlike the low feed, which publishes on target-set changes and can stay
		// silent for exactly the dispatch that went wrong. See g_courseAskedID.
		if (const auto asked = g_courseAskedID.load(std::memory_order_acquire); asked != 0) {
			const auto askedAt = std::chrono::steady_clock::time_point{
				std::chrono::steady_clock::duration{ g_courseAskedTicks.load(std::memory_order_acquire) }
			};
			// 1.5 s. A course that lands republishes the low feed almost at once
			// (it is a target-set change), so this only has to outlast a slow
			// tick - and it is now the ONLY thing standing between a row the
			// engine will not take and a player wondering why the ship is
			// drifting, so it should not take its time about it.
			if (std::chrono::duration<float>(std::chrono::steady_clock::now() - askedAt).count() > 1.5f) {
				g_courseAskedID.store(0, std::memory_order_release);
				REX::WARN("[course] the game did not take a course on {:08X} - no body reports one "
						  "1.5 s later, and there will be no course marker. The ship may be "
						  "drifting toward the middle of the system; pick another row and press "
						  "again, or target it and use the key with the panel closed.",
					asked);
			}
		}

		const auto rowID = g_pendingCourseID.exchange(0, std::memory_order_acq_rel);
		if (rowID == 0)
			return;
		if (!g_inCruise.load(std::memory_order_acquire)) {
			REX::INFO("[course] dropped {:08X} - not in cruise", rowID);
			return;
		}

		// Which number to send. `uniqueID` is what a target is KNOWN by; the
		// event's parameter is spelled uBodyID, and the InfoTargetProvider dump
		// caught those two disagreeing (386531 vs 385501 for Masada IV). So if
		// the feed hands out a body id per entry, prefer it - and say so, because
		// "the field does not exist" is just as much the answer as a number is.
		std::uint32_t id = rowID;
		std::uint32_t type = 0;
		std::string   name;
		bool          haveBodyID = false;
		{
			std::lock_guard lock{ g_candidateMutex };
			for (const auto& row : g_candidates) {
				if (row.id != rowID)
					continue;
				type = row.type;
				name = row.name;
				haveBodyID = row.haveBodyID;
				if (row.haveBodyID)
					id = row.bodyID;
				break;
			}
		}

		{
			static std::atomic<bool> s_saidWhichField{ false };
			if (!s_saidWhichField.exchange(true, std::memory_order_acq_rel))
				REX::INFO("[course] the feed {} a per-entry uBodyID, so courses are sent by {}",
					haveBodyID ? "DOES carry" : "does NOT carry",
					haveBodyID ? "it" : "uniqueID");
		}
		if (!WorldSettled())
			return;  // dropped rather than queued: it was a keypress, and a stale one helps nobody

		const auto                     ui = RE::UI::GetSingleton();
		static const RE::BSFixedString s_hud{ kShipHudMenu };
		const auto                     menu = ui ? ui->GetMenu(s_hud) : nullptr;
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot) {
			REX::WARN("[course] no ship HUD movie to dispatch through");
			return;
		}
		auto* root = menu->uiMovie->asMovieRoot.get();

		// A bare Object with one member, which is all the SWF ever builds:
		// {"uBodyID": <uniqueID>}. CreateObject with no class name is the plain
		// Object constructor.
		V params;
		root->CreateObject(&params);
		if (!params.IsObject()) {
			REX::WARN("[course] could not build the event params");
			return;
		}
		params.SetMember("uBodyID", V{ static_cast<double>(id) });

		// One line per press, so it goes behind the verbose flag with the rest of
		// the per-action trace. The engine's own answer - which body it says the
		// course is on - is logged on change beside the candidate rebuild, and
		// audited against this id there.
		if (DispatchHudEvent(root, "Reticle_OnCruiseLockCourse", &params)) {
			// The AUDIT expects the course to come back on the ROW's id, which is
			// how the feed marks it, whichever number went out.
			g_courseAskedID.store(rowID, std::memory_order_release);
			g_courseAskedTicks.store(
				std::chrono::steady_clock::now().time_since_epoch().count(),
				std::memory_order_release);
			if (bVerboseLog.GetValue())
				REX::INFO("[course] sent uBodyID={:08X} for row {:08X} '{}' (uTargetType={})",
					id, rowID, name, type);
		}
	}

	class HighFeedHandler : public RE::Scaleform::GFx::FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			if (a_params.argCount < 1 || !a_params.args)
				return;

			RE::Scaleform::GFx::Value event = a_params.args[0];
			RE::Scaleform::GFx::Value data;
			if (!event.IsObject() || !event.GetMember("data", &data))
				data = event;

			RE::Scaleform::GFx::Value entries;
			if (!GetEntryArray(data, entries))
				return;

			BearingCollector bearings;
			entries.VisitElements(&bearings);

			bool          haveSelected = false;
			double        selectedAngle = 0.0;
			double        selectedDistance = 0.0;
			std::uint32_t selectedID = 0;
			std::uint32_t selectedType = 0;
			std::uint32_t selectedPoiType = 0;
			std::uint32_t selectedPoiCategory = 0;
			std::uint32_t selectedLocMarkerState = 0;
			bool          selectedHavePoi = false;
			std::string   selectedBlipName;
			std::uint32_t lockedForBlips = 0;
			std::string   lockedName;
			std::string   infoTargetName;
			bool          feedAlive = false;
			bool          lockedInFeed = false;
			BlipGeometry  selGeom{};
			BlipGeometry  lockGeom{};

			{
				std::lock_guard lock{ g_candidateMutex };
				const auto      count = std::min(g_candidates.size(), bearings.rows.size());
				for (std::size_t i = 0; i < count; ++i)
					g_candidates[i].distance = bearings.rows[i].distance;

				// The arrow follows the highlight while the panel is open - that
				// is the preview - and falls back to the locked body once it
				// closes. Closing without confirming therefore reverts it, which
				// is exactly what the confirm key is for.
				selectedID = g_panelOpen.load(std::memory_order_acquire) ?
				                 g_highlightID.load(std::memory_order_acquire) :
				                 g_lockedID.load(std::memory_order_acquire);
				const auto selected = selectedID;

				// Feed names are the key that finds a body's vanilla blip - the
				// SWF names the icon clips "OffScreenIcon: <name>". Two bodies
				// matter: the selected one (highlight while browsing, lock
				// otherwise) and the locked one. The info target's name feeds
				// the overlap pass's exemption - candidates are index-aligned
				// with the feed, so the payload's index resolves here. Read
				// under the same lock as everything else candidate-shaped.
				lockedForBlips = g_lockedID.load(std::memory_order_acquire);
				for (const auto& row : g_candidates) {
					if (lockedForBlips != 0 && lockedName.empty() && row.id == lockedForBlips)
						lockedName = row.name;
					if (selectedID != 0 && selectedBlipName.empty() && row.id == selectedID)
						selectedBlipName = row.name;
					if (row.fromFeed) {
						feedAlive = true;
						if (row.id == lockedForBlips)
							lockedInFeed = true;
					}
				}
				const auto infoIdx = g_infoTargetIndex.load(std::memory_order_acquire);
				if (infoIdx >= 0 && static_cast<std::size_t>(infoIdx) < g_candidates.size() &&
					g_candidates[infoIdx].fromFeed)
					infoTargetName = g_candidates[infoIdx].name;

				// Same-named contacts make a clip name ambiguous, so each key
				// body carries its own bearing and screen position for the
				// blip pass to tell the clips apart (v0.18.1). Only the
				// duplicate case ever reads these - a unique name keeps the
				// pure name match, byte for byte.
				const auto geometryFor = [&](std::uint32_t a_id, const std::string& a_name) {
					BlipGeometry geom{};
					if (a_id == 0 || a_name.empty())
						return geom;
					std::size_t sharing = 0;
					for (const auto& row : g_candidates)
						if (row.fromFeed && row.name == a_name)
							++sharing;
					geom.ambiguous = sharing >= 2;
					if (!geom.ambiguous)
						return geom;
					for (std::size_t i = 0; i < count; ++i) {
						if (g_candidates[i].id == a_id) {
							geom.haveRow = true;
							geom.angle = bearings.rows[i].angle;
							geom.screenX = bearings.rows[i].screenX;
							geom.screenY = bearings.rows[i].screenY;
							break;
						}
					}
					return geom;
				};
				selGeom = geometryFor(selectedID, selectedBlipName);
				lockGeom = geometryFor(lockedForBlips, lockedName);

				// A lock on a RUNTIME form is the one case where holding an id
				// can go wrong. FF-prefixed ids belong to things the game
				// created on the fly - a ship, a spawned point of interest - and
				// once the engine lets one go the id can be handed to something
				// else entirely. A lock left on it would then quietly follow
				// whatever inherited the number.
				//
				// Static bodies never expire, so this touches nothing that comes
				// out of the master file: planets and moons keep their locks
				// indefinitely, including the "locked and waiting" case.
				{
					const auto lockedID = g_lockedID.load(std::memory_order_acquire);
					if (lockedID >= 0xFF000000) {
						bool present = false;
						for (std::size_t i = 0; i < count && !present; ++i)
							present = g_candidates[i].id == lockedID;

						using clock = std::chrono::steady_clock;
						static clock::time_point s_lastSeen{};
						const auto               now = clock::now();
						if (present || s_lastSeen == clock::time_point{}) {
							s_lastSeen = now;
						} else if (std::chrono::duration<float>(now - s_lastSeen).count() > 60.0f) {
							g_lockedID.store(0, std::memory_order_release);
							s_lastSeen = clock::time_point{};
							REX::INFO("[panel] cleared the lock on {:08X} - a runtime form the game has "
									  "let go of, whose id could be reused",
								lockedID);
						}
					}
				}

				for (std::size_t i = 0; i < count; ++i) {
					if (selected != 0 && g_candidates[i].id == selected) {
						haveSelected = true;
						selectedAngle = bearings.rows[i].angle;
						selectedDistance = bearings.rows[i].distance;
						selectedType = g_candidates[i].type;
						selectedPoiType = g_candidates[i].poiType;
						selectedPoiCategory = g_candidates[i].poiCategory;
						selectedLocMarkerState = g_candidates[i].locMarkerState;
						selectedHavePoi = g_candidates[i].havePoi;
						break;
					}
				}
			}

			// A locked MOON that leaves tracking range clears itself (v0.8.8;
			// v0.8.9 made it edge-triggered on the tester's question): a moon
			// only fills in beside its parent, so once the feed drops it the
			// lock would sit as "..." indefinitely - and in the v0.8.7 model
			// a lock is what keeps the blips hidden. Planets keep the
			// lock-and-wait behaviour: flying at a distant dash-row planet is
			// the list's whole point.
			//
			// "Absent from the feed" is the same fact the panel's dash shows -
			// the signal is fine; what needs care is trusting a single
			// reading of it. Candidates rebuild EMPTY and then refill after
			// every movie teardown (map, load screen), so absence alone must
			// never clear anything. Hence the edge trigger: only a moon
			// CONFIRMED present since the last teardown (g_lockSeenInFeed)
			// can be cleared by absence - loads cannot eat a lock by
			// construction, timer not involved. The short debounce that
			// remains only covers the engine momentarily omitting a body
			// from a live payload, which is unverified but cheap to survive.
			if (lockedInFeed)
				g_lockSeenInFeed.store(lockedForBlips, std::memory_order_release);
			{
				using clock = std::chrono::steady_clock;
				static clock::time_point  s_moonAbsentSince{};
				static std::uint32_t      s_moonWatchID{ 0 };
				bool                      watching = false;
				if (lockedForBlips != 0 && feedAlive && !lockedInFeed &&
					g_lockSeenInFeed.load(std::memory_order_acquire) == lockedForBlips) {
					GalaxyData galaxy{};
					if (ReadGalaxyData(lockedForBlips, galaxy) && galaxy.parentPlanetID != 0) {
						watching = true;
						const auto now = clock::now();
						if (s_moonWatchID != lockedForBlips) {
							s_moonWatchID = lockedForBlips;
							s_moonAbsentSince = now;
						} else if (std::chrono::duration<float>(now - s_moonAbsentSince).count() > 3.0f) {
							g_lockedID.store(0, std::memory_order_release);
							g_lockSeenInFeed.store(0, std::memory_order_release);
							lockedForBlips = 0;
							lockedName.clear();
							std::string moonName;
							{
								std::lock_guard bodies{ g_bodyTableMutex };
								if (const auto body = g_bodyTable.find(s_moonWatchID);
									body != g_bodyTable.end())
									moonName = body->second.name;
							}
							REX::INFO("[panel] cleared the lock on '{}' - the moon left "
									  "tracking range",
								moonName.empty() ? std::format("{:08X}", s_moonWatchID) : moonName);

							// The row the lock kept listed will not leave by
							// itself: candidates only rebuild when the LOW
							// feed publishes, and it publishes on target-set
							// changes - a mod-side clear is not one, so the
							// appended row would sit there until unrelated
							// traffic (the tester caught it parked under the
							// highlight). Evict it directly, and settle the
							// highlight if it was on that row.
							{
								std::lock_guard rows{ g_candidateMutex };
								std::erase_if(g_candidates, [&](const Candidate& a_row) {
									return a_row.id == s_moonWatchID && !a_row.fromFeed;
								});
							}
							if (g_panelOpen.load(std::memory_order_acquire) &&
								g_highlightID.load(std::memory_order_acquire) == s_moonWatchID)
								MoveHighlight(0);
							watching = false;
						}
					}
				}
				if (!watching)
					s_moonWatchID = 0;
			}

			// The vanilla blip pass: hides the off-screen container in cruise
			// and lets the selected and locked bodies' own blips back through.
			// True means vanilla covers the selected body this tick - kept
			// off-screen blip or visible on-screen icon.
			const bool selectedCovered = ManageVanillaBlips(selectedID, selectedBlipName,
				lockedForBlips, lockedName, infoTargetName, selGeom, lockGeom);

			if (g_arrowReady.load(std::memory_order_acquire)) {
				using V = RE::Scaleform::GFx::Value;
				haveSelected = haveSelected && g_inCruise.load(std::memory_order_acquire);
				// When vanilla marks the selected body - blip on the ring or
				// icon in the view - the mod draws NOTHING for it (v0.8.1 and
				// v0.8.2, the tester's calls). The mod's marker and name exist
				// only for bodies with no vanilla presence.
				const bool blipCovers = selectedCovered;
				const bool showMarker = haveSelected && !blipCovers;
				// The faux blip wears vanilla's art. Planets and stars have no
				// extra icon data; POIs, ships and stations feed the MapIcons
				// SetLocation path, which needs the entry's own
				// uPoiType/uPoiCategory - carried by the feed and captured
				// with the candidate (v0.8.12), so those types now get the
				// real art too. Only a POI-typed body whose entry carried no
				// icon fields keeps the diamond.
				const bool fauxPoiOK = selectedHavePoi &&
				                       (selectedType == kTargetTypePOI ||
				                           selectedType == kTargetTypeShip ||
				                           selectedType == kTargetTypeStation);
				const bool fauxActive = bVanillaStyleMarker.GetValue() &&
				                        g_fauxReady.load(std::memory_order_acquire) &&
				                        (selectedType == kTargetTypePlanet ||
				                            selectedType == kTargetTypeStar || fauxPoiOK);
				g_arrowClip.SetMember("visible", V{ showMarker && !fauxActive });
				if (g_fauxReady.load(std::memory_order_acquire))
					g_fauxBlip.SetMember("visible", V{ showMarker && fauxActive });
				if (showMarker) {
					// The marker is placed on the circle rather than rotated, so
					// there is no orientation to be wrong at any bearing.
					const double bearing = selectedAngle * (bArrowInvertAngle.GetValue() ? -1.0 : 1.0) +
					                       static_cast<double>(fArrowAngleOffset.GetValue());
					const double rotation = bearing;
					const double markerRadians = bearing * 3.14159265358979323846 / 180.0;
					const double markerRadius = static_cast<double>(fArrowRadius.GetValue());
					if (fauxActive) {
						// Drive the real icon exactly as the reticle would:
						// state on body change, bearing and distance per tick.
						// SetTargetHighInfo does the rotation itself. The POI
						// fields ride along - the planet/star frames never
						// read them, and the POI/station frames need them for
						// their MapIcons art.
						if (g_fauxLastID.exchange(selectedID, std::memory_order_acq_rel) != selectedID) {
							g_fauxLow.SetMember("uTargetType", V{ selectedType });
							g_fauxLow.SetMember("uPoiType", V{ selectedPoiType });
							g_fauxLow.SetMember("uPoiCategory", V{ selectedPoiCategory });
							g_fauxLow.SetMember("uLocationMarkerState", V{ selectedLocMarkerState });
							V lowArgs[]{ g_fauxLow, V{}, V{ false }, V{ true } };
							g_fauxBlip.Invoke("SetTargetLowInfo", nullptr, lowArgs, 4);
						}
						g_fauxHigh.SetMember("angleToCrosshair", V{ bearing });
						g_fauxHigh.SetMember("distance", V{ selectedDistance });
						g_fauxBlip.Invoke("SetTargetHighInfo", nullptr, &g_fauxHigh, 1);
					} else {
						g_arrowClip.SetMember("x", V{ markerRadius * std::sin(markerRadians) });
						g_arrowClip.SetMember("y", V{ -markerRadius * std::cos(markerRadians) });
					}

					// Rate-limited even when asked for, so the bearing can be
					// correlated with what is actually on screen without
					// flooding the log.
					if (bVerboseLog.GetValue()) {
						static std::atomic<std::uint32_t> s_tick{ 0 };
						if ((s_tick.fetch_add(1, std::memory_order_relaxed) % 120) == 0)
							REX::INFO("[arrow] angleToCrosshair={:.1f} -> rotation={:.1f}", selectedAngle, rotation);
					}
				}
			}

			// The course-lock dispatch, a no-op unless a key has asked for one.
			// Here rather than in RefreshPanel because that returns early when
			// the drawn panel is absent or closed, and a press made on the last
			// frame before a close still has to go out. No lock is held at this
			// point, which is the requirement for entering the VM.
			if (bLockCourse.GetValue())
				RunLockCourse();

			// Distances are current by this point, which is what the list shows.
			RefreshPanel();

			if (g_captureHighRequested.exchange(false, std::memory_order_acq_rel)) {
				REX::INFO("[nav] ==== high-frequency (bearings) ====");
				HighFeedRowVisitor visitor;
				entries.VisitElements(&visitor);
				REX::INFO("[nav] ==== end ====");
			}
		}
	};

	HighFeedHandler g_highFeedHandler;

	// ---------------------------------------------------------------------------
	// The movie settle gate.
	//
	// 2026-08-02, taking off from a planetary surface: the ship HUD movie was
	// created, probed, torn down and rebuilt inside 21 ms. The probe round that
	// landed on the replacement called Subscribe on it from a BSJobs worker and
	// faulted six frames deep in the AS3 VM, resolving the method through an ABC
	// file the new movie had not finished registering. The log shows the whole
	// verbose probe block printed TWICE 16 ms apart, which is only possible if
	// OnMenuMovieCreated reset the attempt counter between them.
	//
	// WorldSettled cannot catch this and never could. It measures time since
	// LoadingMenu closed, and a surface takeoff rebuilds the HUD during the
	// cutscene - BEFORE any loading screen - so it reads "settled" for the entire
	// dangerous window. The 2026-07-28 freeze was the same shape ("mid-init ...
	// REBUILT within 50 ms") and the settle timer added for it measured the wrong
	// clock. This one measures the movie.
	//
	// Neither gate replaces the other: WorldSettled says the WORLD is not mid-load,
	// this says the MOVIE in front of us is not mid-life. Both must hold.
	//
	// Note what this is NOT: it is not a lock. Scaleform exposes none, and SFSE's
	// task interface cannot help - AddTask and AddPermanentTask are dispatched
	// from the same Command_Process hook, which the crash backtrace shows running
	// on BSJobs workers, so "bounce it to the main thread" is not on the menu.
	// What this does is remove the window in which the engine is provably mutating
	// the VM underneath us. That window is where the crash lives.

	// Two orders of magnitude clear of the observed rebuild gaps (21 ms here, 50 ms
	// on 2026-07-28), and invisible in practice: nothing reads a subscription until
	// cruise, which is many seconds after any transition that trips this.
	constexpr std::int64_t kMovieSettleMs = 1500;

	std::atomic<std::uint32_t> g_settleSeenGen{ 0 };
	std::atomic<const void*>   g_settleSeenRoot{ nullptr };
	std::atomic<std::int64_t>  g_settleSinceMs{ 0 };

	// Has THIS movie - this generation, this root - been alive and unchanged for
	// kMovieSettleMs? Takes the root the caller is about to use, so the answer is
	// about that movie and not merely about "some movie being open".
	//
	// Only ever called under the SingleWinner claim, so the exchanges below cannot
	// interleave with another probe round; they are atomics to stay honest about
	// being read from whichever worker the task landed on.
	bool MovieSettled(const void* a_root, std::uint32_t a_gen)
	{
		using clock = std::chrono::steady_clock;
		const auto nowMs =
			std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();

		// Both exchanges, unconditionally - `||` would short-circuit past the
		// second and leave it stale, restarting the clock again next round.
		const bool genChanged = g_settleSeenGen.exchange(a_gen, std::memory_order_acq_rel) != a_gen;
		const bool rootChanged = g_settleSeenRoot.exchange(a_root, std::memory_order_acq_rel) != a_root;

		if (genChanged || rootChanged) {
			// First sighting of this movie. Start its clock and refuse.
			g_settleSinceMs.store(nowMs, std::memory_order_release);
			// Once per movie, not once per frame of waiting - and NOT behind
			// bVerboseLog. Two of these 16 ms apart is the signature of the
			// takeoff rebuild, and a crash log is worth far more with that line
			// in it than a shipped build is hurt by a handful of lines a session.
			REX::INFO("[nav] ship HUD movie gen {} (root {:016X}) - holding {} ms before probing it",
				a_gen, reinterpret_cast<std::uintptr_t>(a_root), kMovieSettleMs);
			return false;
		}
		return (nowMs - g_settleSinceMs.load(std::memory_order_acquire)) >= kMovieSettleMs;
	}

	// ---------------------------------------------------------------------------
	// The flight-state gate.
	//
	// Orthogonal to the movie gate, and NOT a substitute for it: this one says
	// "the feeds are worth having yet", the movie gate says "this movie is safe to
	// ask". Every rebuild in the 2026-08-02 test run - gens 2, 3 and 4 - happened
	// while the player was flying, which is exactly a state this gate approves. It
	// buys cost, not safety.
	//
	// ⚠ Sitting in the pilot seat is NOT flying. A landed ship has its HUD up and
	// a pilot in the seat, and taking off destroys that movie and builds another -
	// so a subscription made while landed is thrown away seconds later, and it is
	// made in the window where a rebuild is most likely to land on top of it.
	// The log bears this out: gen 1 subscribed at 11:13:45 and gen 2 replaced it
	// 41 seconds later on takeoff, before anything had read a single payload.
	//
	// Only calls this build already runs: GetSpaceship, IsSpaceshipLanded and
	// IsSpaceshipDocked are what LogHeartbeat has been printing all along.
	// Deliberately NOT used:
	//   GetSpaceshipPilot (ID 119876) - would add passenger-vs-pilot discrimination,
	//     but has never been called here, and a relocation ID that is subtly wrong
	//     is a crash. Poor trade for a distinction the ship HUD being open at all
	//     already mostly makes.
	//   IsInSpace - its bool argument has no documented meaning; LogHeartbeat
	//     prints it BOTH ways rather than guess, which is not a thing to gate on.
	//
	// Fails OPEN. If the state reads wrong the mod idles and says so rather than
	// dying, and bGateOnFlightState=false in the INI takes the layer back out
	// without touching the movie gate underneath it.

	// -1 not yet read, 0 holding, 1 clear. Logged on transition only - once per
	// takeoff, not once per frame of sitting on a landing pad.
	std::atomic<std::int32_t> g_flightGateState{ -1 };

	bool ReadyToFly()
	{
		if (!bGateOnFlightState.GetValue())
			return true;

		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto ship = player ? player->GetSpaceship() : nullptr;
		const bool clear = ship && !ship->IsSpaceshipLanded() && !ship->IsSpaceshipDocked();

		const std::int32_t state = clear ? 1 : 0;
		if (g_flightGateState.exchange(state, std::memory_order_acq_rel) != state)
			REX::INFO("[nav] flight gate: {}",
				clear ? "clear - flying, probing the HUD movie" :
						"holding - no ship, or landed/docked; the feeds would be discarded on takeoff");

		return clear;
	}

	// The movie can still be swapped between resolving a value out of it and
	// invoking on that value. Re-reading the live root and comparing narrows the
	// window to the width of the call itself - as tight as this gets without a
	// lock the engine does not expose. Cheap enough to do before every VM call on
	// this path, which runs at most once a frame and stops entirely once
	// subscribed.
	bool StillSameMovie(const void* a_root, std::uint32_t a_gen)
	{
		if (g_movieGeneration.load(std::memory_order_acquire) != a_gen)
			return false;

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return false;
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud))
			return false;

		const auto menu = ui->GetMenu(s_shipHud);
		return menu && menu->uiMovie && menu->uiMovie->asMovieRoot &&
			   static_cast<const void*>(menu->uiMovie->asMovieRoot.get()) == a_root;
	}

	void TryInstallSubscriber()
	{
		if (!bInterposeTargetData.GetValue() ||
			(g_subscribed.load(std::memory_order_acquire) &&
				g_subscribedHigh.load(std::memory_order_acquire)) ||
			g_subscribeFailed.load(std::memory_order_acquire))
			return;

		// Ahead of the claim: it is three cheap native reads with no shared state,
		// and leaving it here means the gate's log line reflects what is actually
		// true rather than only the frames on which this thread won the exchange.
		if (!ReadyToFly())
			return;

		// The check above is not enough on its own - see SingleWinner. Two
		// threads reaching `Subscribe` together is what crashed v0.7.4.
		const SingleWinner winner{ g_subscribeInFlight };
		if (!winner.Won())
			return;

		// Re-read now the claim is held: the thread that just finished may have
		// subscribed between our load above and this line.
		if (g_subscribed.load(std::memory_order_acquire) &&
			g_subscribedHigh.load(std::memory_order_acquire))
			return;

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud))
			return;
		const auto menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return;
		auto* a_root = menu->uiMovie->asMovieRoot.get();

		// ⚠ Before the attempt counter, deliberately. A movie we are still
		// waiting on must not cost a probe - otherwise a run of transitions
		// burns the whole 60-probe budget on movies we never actually looked
		// at, which is the v0.0.8 mistake in a new coat.
		const auto  gen = g_movieGeneration.load(std::memory_order_acquire);
		const auto* rootID = static_cast<const void*>(a_root);
		if (!MovieSettled(rootID, gen))
			return;

		// Count only probes that actually had a movie to look at. v0.0.8
		// incremented on every frame including those before the ship HUD
		// existed, so it burned its whole budget during the loading screen and
		// reported "not found" without ever having looked.
		const auto attempt = g_subscribeAttempts.fetch_add(1, std::memory_order_relaxed);
		if (attempt > 60) {
			g_subscribeFailed.store(true, std::memory_order_release);
			REX::WARN("[nav] BSUIDataManager not reachable after 60 probes of a live movie");
			return;
		}
		const bool verbose = (attempt == 0);  // report the detail once

		bool anyResolved = false;
		for (const auto* path : kDataManagerPaths) {
			// Five paths, each of them several VM calls. A rebuild landing
			// part-way through the list would leave the remaining iterations
			// reading a dead movie, so the check is per-iteration and not just
			// once on the way in.
			if (!StillSameMovie(rootID, gen))
				return;

			const bool available = a_root->IsAvailable(path);
			RE::Scaleform::GFx::Value manager;
			const bool resolved = a_root->GetVariable(&manager, path);

			if (verbose)
				REX::INFO("[nav] probe '{}': IsAvailable={} GetVariable={} value={}",
					path, available, resolved, resolved ? DescribeValue(manager) : "-");

			if (resolved && (manager.IsObject() || manager.IsDisplayObject())) {
				anyResolved = true;
				if (manager.HasMember("Subscribe")) {
					// Each feed is subscribed at most ONCE per movie, whatever
					// order the rounds succeed in. Re-subscribing an already
					// live feed would register the handler twice - every later
					// dispatch would then run it twice, all session.
					bool lowOK = g_subscribed.load(std::memory_order_acquire);
					if (!lowOK) {
						// This exact call is the 2026-08-02 crash frame. The
						// GetVariable that produced `manager` says nothing about
						// whether the movie still exists now.
						if (!StillSameMovie(rootID, gen))
							return;

						RE::Scaleform::GFx::Value args[2];
						a_root->CreateString(&args[0], kTargetFeed);
						a_root->CreateFunction(&args[1], &g_feedHandler);
						if (manager.Invoke("Subscribe", nullptr, args, 2)) {
							lowOK = true;
							g_subscribed.store(true, std::memory_order_release);
							REX::INFO("[nav] SUBSCRIBED to '{}' via {}.Subscribe", kTargetFeed, path);
						} else {
							REX::WARN("[nav] '{}.Subscribe' rejected the call", path);
						}
					}

					// The screen positions live on the other feed. It can fail
					// on its own on a mid-init movie (seen 2026-07-28: the
					// provider was not yet registered), so its result is
					// TRACKED and retried next round rather than shrugged at.
					if (lowOK && !g_subscribedHigh.load(std::memory_order_acquire)) {
						if (!StillSameMovie(rootID, gen))
							return;

						RE::Scaleform::GFx::Value hiArgs[2];
						a_root->CreateString(&hiArgs[0], kHighFeed);
						a_root->CreateFunction(&hiArgs[1], &g_highFeedHandler);
						if (manager.Invoke("Subscribe", nullptr, hiArgs, 2)) {
							g_subscribedHigh.store(true, std::memory_order_release);
							REX::INFO("[nav] SUBSCRIBED to '{}'", kHighFeed);
						} else {
							REX::WARN("[nav] FAILED to subscribe to '{}' - will retry", kHighFeed);
						}
					}

					if (lowOK && g_subscribedHigh.load(std::memory_order_acquire)) {
						// The star map's body feed, if the probe is on. A
						// failure here is informative and must not disturb the
						// two subscriptions above, which the mod depends on.
						// Diagnostic only, so a movie that went away just skips
						// it and falls through to the return below - both feeds
						// this mod depends on are already live at this point.
						if (bProbeStarmapFeed.GetValue() && StillSameMovie(rootID, gen)) {
							const auto&               feed = sStarmapFeed.GetValue();
							RE::Scaleform::GFx::Value mapArgs[2];
							a_root->CreateString(&mapArgs[0], feed.c_str());
							a_root->CreateFunction(&mapArgs[1], &g_starmapProbeHandler);
							REX::INFO("[starmap] {} to '{}'",
								manager.Invoke("Subscribe", nullptr, mapArgs, 2) ? "SUBSCRIBED" : "FAILED to subscribe",
								feed);
						}
						return;
					}
					if (lowOK)
						return;  // high retries next round; do not fall through
					         // to the path-invoke fallback and double-subscribe
				} else if (verbose) {
					REX::INFO("[nav]   no 'Subscribe' member; its members follow:");
					LevelCollector visitor{ std::string{ "[nav] " } + path, nullptr };
					RE::Scaleform::GFx::Value copy = manager;
					copy.VisitMembers(&visitor);
				}
			}

			// Path-based Invoke resolves differently from GetVariable, so a
			// class the latter cannot see may still be callable this way. Only
			// ever for a low feed that is NOT yet subscribed - this route must
			// never produce a second registration of a live handler.
			if (!g_subscribed.load(std::memory_order_acquire)) {
				if (!StillSameMovie(rootID, gen))
					return;

				RE::Scaleform::GFx::Value args[2];
				a_root->CreateString(&args[0], kTargetFeed);
				a_root->CreateFunction(&args[1], &g_feedHandler);
				const std::string method = std::string{ path } + ".Subscribe";
				if (a_root->Invoke(method.c_str(), nullptr, args, 2)) {
					g_subscribed.store(true, std::memory_order_release);
					REX::INFO("[nav] SUBSCRIBED to '{}' via path-invoke '{}'", kTargetFeed, method);
					return;  // high follows via the manager route next round
				}
				if (verbose)
					REX::INFO("[nav]   path-invoke '{}' failed too", method);
			}
		}

		if (anyResolved) {
			g_subscribeFailed.store(true, std::memory_order_release);
			REX::WARN("[nav] reached a data manager but could not subscribe - route exhausted");
		}
	}

	void TryInstallInterposer()
	{
		// Give up permanently after a real failure. v0.0.6 retried every frame
		// and wrote the same warning ~60 times a second.
		if (!bInterposeTargetData.GetValue() ||
			g_interposeInstalled.load(std::memory_order_acquire) ||
			g_interposeFailed.load(std::memory_order_acquire))
			return;

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud))
			return;

		const auto menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return;

		auto*             root = menu->uiMovie->asMovieRoot.get();
		const char*       rootPath = menu->GetRootPath();
		const std::string reticlePath = std::string{ rootPath ? rootPath : "root" } + ".Reticle_mc";

		RE::Scaleform::GFx::Value reticle;
		if (!root->GetVariable(&reticle, reticlePath.c_str()))
			return;  // menu up but not built yet - try again next frame

		// Each step is reported separately. The previous build bundled the stash
		// and the replacement into one failure path, so when the stash failed it
		// never learned whether the replacement - the step that actually decides
		// whether this approach can work at all - would have succeeded.
		const auto giveUp = [&](const char* a_why) {
			REX::WARN("[nav] not interposing: {}", a_why);
			g_interposeFailed.store(true, std::memory_order_release);
		};

		RE::Scaleform::GFx::Value original;
		if (!reticle.GetMember(kInterposeFunc, &original)) {
			giveUp("the reticle has no such member");
			return;
		}
		REX::INFO("[nav] step 1 OK: original '{}' is {}", kInterposeFunc, DescribeValue(original));

		// A plain Object is dynamic, unlike the sealed ShipReticle class.
		root->CreateObject(&g_originalHolder);
		if (!g_originalHolder.IsObject() || !g_originalHolder.SetMember(kOriginalStash, original)) {
			giveUp("could not park the original on a plain Object");
			return;
		}
		REX::INFO("[nav] step 2 OK: original parked on a dynamic holder");

		// The decisive step. AS3 sealed classes expose their methods as fixed
		// traits, which are normally read-only - if this fails, replacing the
		// method is impossible and the approach is dead rather than mistuned.
		RE::Scaleform::GFx::Value replacement;
		root->CreateFunction(&replacement, &g_interposer);
		if (!reticle.SetMember(kInterposeFunc, replacement)) {
			giveUp("could not replace the method on the sealed class - "
				   "AS3 fixed traits are read-only, so interposition is out; "
				   "next route is hooking Value::ObjectInterface::Invoke in C++");
			return;
		}

		g_interposeInstalled.store(true, std::memory_order_release);
		REX::INFO("[nav] step 3 OK: interposed on {}.{}", reticlePath, kInterposeFunc);
	}

	// ---------------------------------------------------------------------------
	// The pointer arrow.
	//
	// `angleToCrosshair` is a 2D screen bearing, not a cone angle - vanilla's own
	// off-screen blips do `rotation = angleToCrosshair + 180` and nothing else
	// (OffScreenIcon.SetTargetHighInfo). So an arrow that points at a chosen body
	// is that one line, recomputed on every high-frequency update, which makes it
	// live while the ship steers. It also works for targets *behind* the player,
	// where `screenPositionX/Y` is the -1 "unprojectable" sentinel - the case a
	// screen-position overlay could never have handled.
	// ---------------------------------------------------------------------------

	// Nothing may touch the movie while a load screen or the main menu is up:
	// the world is in flux and the Scaleform VM is rebuilding menus on its own
	// threads. Omitting this is what crashed v0.1.3 - an access violation inside
	// the AS3 VM's TypeError path, on the IOManager thread, mid movie-load.
	bool WorldSettled()
	{
		static const RE::BSFixedString s_loadingMenu{ "LoadingMenu" };
		static const RE::BSFixedString s_mainMenu{ "MainMenu" };
		const auto                     ui = RE::UI::GetSingleton();
		if (!ui)
			return false;

		// Menus-closed alone is NOT settled. The 2026-07-28 freeze log shows a
		// subscribe round running - so this returned true - while the load
		// transition was still on screen: the ship HUD movie was mid-init (the
		// high-frequency feed was not yet registered) and was then REBUILT
		// within 50 ms. Poking a movie in that state from a worker thread is
		// the prime suspect for the hang. So: LoadingMenu/MainMenu must have
		// been closed for a couple of seconds continuously, the settle-timer
		// pattern ShipHullRegen already uses. This function is called from the
		// per-frame task and the feed callbacks, so the unsettled timestamp
		// stays fresh while any load is up.
		using clock = std::chrono::steady_clock;
		static std::atomic<std::int64_t> s_lastUnsettledMs{ 0 };
		const auto                       nowMs =
			std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
		static std::atomic<bool> s_wasUnsettled{ false };
		if (ui->IsMenuOpen(s_loadingMenu) || ui->IsMenuOpen(s_mainMenu)) {
			s_lastUnsettledMs.store(nowMs, std::memory_order_release);
			// Once per unsettled EPISODE, not once per frame of it: anything
			// holding per-save state watches this counter to know it must throw
			// what it has away.
			if (!s_wasUnsettled.exchange(true, std::memory_order_acq_rel))
				g_unsettledEpoch.fetch_add(1, std::memory_order_acq_rel);
			return false;
		}
		s_wasUnsettled.store(false, std::memory_order_release);
		const auto last = s_lastUnsettledMs.load(std::memory_order_acquire);
		// First call of the session lands here with 0: treat process start as
		// the unsettled moment, which the timer then measures from.
		if (last == 0) {
			s_lastUnsettledMs.store(nowMs, std::memory_order_release);
			return false;
		}
		return (nowMs - last) > 2500;
	}

	void RefreshCruiseState()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud)) {
			g_inCruise.store(false, std::memory_order_release);
			return;
		}
		const auto menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return;

		auto*             root = menu->uiMovie->asMovieRoot.get();
		const char*       rootPath = menu->GetRootPath();
		const std::string path = std::string{ rootPath ? rootPath : "root" } + ".Reticle_mc.CruiseModeHUDActive";

		RE::Scaleform::GFx::Value flag;
		const bool                cruising = root->GetVariable(&flag, path.c_str()) && flag.IsBoolean() && flag.GetBoolean();
		const bool                was = g_inCruise.exchange(cruising, std::memory_order_acq_rel);
		if (was != cruising) {
			REX::INFO("[arrow] cruise {}", cruising ? "entered - panel active" : "left - panel idle");
			if (!cruising) {
				// The lock survives leaving cruise - the arrow is hidden outside
				// cruise anyway, so keeping it means a brief drop out to
				// manoeuvre does not cost you your target.
				g_highlightID.store(0, std::memory_order_release);
				// Closing on exit must never strand the throttle suppressed.
				if (g_panelOpen.exchange(false, std::memory_order_acq_rel))
					REX::INFO("[panel] forced closed - left cruise ({} throttle events marked, "
							  "{} input events hidden from the camera)",
						g_suppressedCount.load(std::memory_order_acquire),
						g_cameraRemovedCount.load(std::memory_order_acquire));
				// Recap the survey while the cruise it describes is still fresh.
				if (bSurveyCruiseKeys.GetValue())
					SurveyDump();
			}
		}
	}

	// ---------------------------------------------------------------------------
	// Vanilla blip management (Phase 3 - PHASE3-BLIP-PLAN.md).
	//
	// The circle-and-arrow blips are OffScreenIcon clips, every one a child of
	// Reticle_mc.ShipReticle_mc.OffScreenIndicatorParent_mc and named
	// "OffScreenIcon: <feed name>" by the SWF itself. Hiding that container hides
	// exactly the off-screen set - the named in-view markers live on Reticle_mc.
	//
	// Individual icons must NEVER be hidden with `visible=false`: the SWF's
	// GetClip uses that as its "pooled, free to recycle" test, so an outside
	// write corrupts the pool (duplicate live-array entries, clips re-keyed to
	// other targets). The container is the only safe handle, and a blip is kept
	// on screen by MOVING it into the mod's own holder - GetClip re-parents a
	// clip only when reviving it, so a live one stays in the holder while the
	// SWF keeps feeding it rotation, faction frames and selection state.
	// ---------------------------------------------------------------------------

	constexpr const char* kOffScreenIconPrefix = "OffScreenIcon: ";

	// One-shot builder, per movie. The holder hangs off Reticle_mc - the stable
	// home the arrow proved - NOT off the animated ShipReticle_mc, whose timeline
	// can re-create its children. It mirrors the vanilla container's transform so
	// a reparented blip lands exactly where it would have drawn: icons are never
	// positioned by the SWF, only rotated about their parent's origin.
	void TryCreateBlipHolder(RE::Scaleform::GFx::ASMovieRootBase* a_root, const std::string& a_base,
		RE::Scaleform::GFx::Value& a_container)
	{
		if (g_blipHolderReady.load(std::memory_order_acquire) ||
			g_blipHolderFailed.load(std::memory_order_acquire))
			return;

		// Enters the AS3 VM, so it is serialised like every other one-shot
		// builder - see SingleWinner.
		const SingleWinner winner{ g_blipHolderBuildInFlight };
		if (!winner.Won())
			return;
		if (g_blipHolderReady.load(std::memory_order_acquire))
			return;

		RE::Scaleform::GFx::Value reticle;
		if (!a_root->GetVariable(&reticle, (a_base + ".Reticle_mc").c_str()))
			return;  // transient - retry next tick rather than latch a failure

		using V = RE::Scaleform::GFx::Value;

		// A log that ENDS here names the frozen call.
		REX::INFO("[blip] building holder - entering the VM");
		bool made = reticle.CreateEmptyMovieClip(&g_blipHolder, "ShipNavPanelBlips", 19990);
		if (!made) {
			a_root->CreateObject(&g_blipHolder, "flash.display.Sprite");
			if (!g_blipHolder.IsObject() && !g_blipHolder.IsDisplayObject()) {
				g_blipHolderFailed.store(true, std::memory_order_release);
				REX::WARN("[blip] holder not created - locked-blip reappearance disabled, "
						  "hide-all still works");
				return;
			}
			RE::Scaleform::GFx::Value added;
			if (!reticle.Invoke("addChild", &added, &g_blipHolder, 1)) {
				g_blipHolder = RE::Scaleform::GFx::Value{};
				g_blipHolderFailed.store(true, std::memory_order_release);
				REX::WARN("[blip] holder addChild failed - locked-blip reappearance disabled");
				return;
			}
		}

		// Compose the container's placement relative to Reticle_mc. Expected all
		// zeros and unit scales; read and logged rather than assumed, so a wrong
		// expectation shows up as a number in the log instead of a mystery.
		const auto num = [](RE::Scaleform::GFx::Value& a_obj, const char* a_name, double a_default) {
			RE::Scaleform::GFx::Value v;
			return a_obj.GetMember(a_name, &v) ? AsNumber(v) : a_default;
		};
		RE::Scaleform::GFx::Value shipReticle;
		if (!a_root->GetVariable(&shipReticle, (a_base + ".Reticle_mc.ShipReticle_mc").c_str()))
			shipReticle = RE::Scaleform::GFx::Value{};

		double srX = 0.0, srY = 0.0, srSX = 1.0, srSY = 1.0, srRot = 0.0;
		if (shipReticle.IsObject() || shipReticle.IsDisplayObject()) {
			srX = num(shipReticle, "x", 0.0);
			srY = num(shipReticle, "y", 0.0);
			srSX = num(shipReticle, "scaleX", 1.0);
			srSY = num(shipReticle, "scaleY", 1.0);
			srRot = num(shipReticle, "rotation", 0.0);
		}
		const double cX = num(a_container, "x", 0.0);
		const double cY = num(a_container, "y", 0.0);
		double       cSX = num(a_container, "scaleX", 1.0);
		double       cSY = num(a_container, "scaleY", 1.0);
		const double cRot = num(a_container, "rotation", 0.0);
		if (srSX == 0.0)
			srSX = 1.0;
		if (srSY == 0.0)
			srSY = 1.0;
		if (cSX == 0.0)
			cSX = 1.0;
		if (cSY == 0.0)
			cSY = 1.0;

		g_blipHolder.SetMember("x", V{ srX + cX * srSX });
		g_blipHolder.SetMember("y", V{ srY + cY * srSY });
		g_blipHolder.SetMember("scaleX", V{ srSX * cSX });
		g_blipHolder.SetMember("scaleY", V{ srSY * cSY });
		g_blipHolder.SetMember("rotation", V{ srRot + cRot });

		REX::INFO("[blip] holder ready ({}) - ShipReticle_mc at ({:.1f},{:.1f}) scale ({:.2f},{:.2f}) "
				  "rot {:.1f}; container at ({:.1f},{:.1f}) scale ({:.2f},{:.2f}) rot {:.1f}",
			made ? "movie clip" : "sprite", srX, srY, srSX, srSY, srRot, cX, cY, cSX, cSY, cRot);

		// One-time census of what the container holds, for checking the naming
		// assumption against the live movie - the in-game test's first item.
		RE::Scaleform::GFx::Value count;
		if (bVerboseLog.GetValue() && a_container.GetMember("numChildren", &count)) {
			const int n = static_cast<int>(AsNumber(count));
			REX::INFO("[blip] container census: {} children", n);
			for (int i = 0; i < n; ++i) {
				V idx{ static_cast<double>(i) };
				V child;
				if (a_container.Invoke("getChildAt", &child, &idx, 1)) {
					V name;
					const char* text = child.GetMember("name", &name) && name.IsString() ?
					                       name.GetString() :
					                       "<unnamed>";
					REX::INFO("[blip]   [{}] {}", i, SafeStr(text));
				}
			}
		}

		g_blipHolderReady.store(true, std::memory_order_release);
	}

	// One-shot builder for the faux blip. `new OffScreenIcon()` is exactly how
	// the SWF's own GetClip makes the real ones, so CreateObject on the class
	// gives the full library symbol - art, timeline frames, faction wrapper.
	// The class lives in the movie's DEFAULT package, so the bare name is its
	// qualified name (unlike BSUIDataManager, which needed the full path).
	//
	// The synthetic data objects are created once and mutated per use. Fields
	// the icon reads that the mod cannot fill truthfully stay false. The POI
	// path (uPoiType/uPoiCategory into MapIcons.SetLocation) is entered only
	// for entries whose feed data carried those fields - captured with the
	// candidates since v0.8.12 - so the art it draws is the entry's own.
	void TryCreateFauxBlip(RE::Scaleform::GFx::ASMovieRootBase* a_root)
	{
		if (g_fauxReady.load(std::memory_order_acquire) ||
			g_fauxFailed.load(std::memory_order_acquire))
			return;
		if (!g_blipHolderReady.load(std::memory_order_acquire))
			return;  // needs the holder's coordinate space; try again next tick

		// Constructs through the AS3 VM - serialised like every other builder.
		const SingleWinner winner{ g_fauxBuildInFlight };
		if (!winner.Won())
			return;
		if (g_fauxReady.load(std::memory_order_acquire))
			return;

		using V = RE::Scaleform::GFx::Value;

		const auto giveUp = [&](const char* a_why) {
			REX::WARN("[blip] faux blip not created ({}) - the drawn diamond stays", a_why);
			g_fauxBlip = RE::Scaleform::GFx::Value{};
			g_fauxLow = RE::Scaleform::GFx::Value{};
			g_fauxHigh = RE::Scaleform::GFx::Value{};
			g_fauxFailed.store(true, std::memory_order_release);
		};

		// A log that ENDS here names the frozen call.
		REX::INFO("[blip] constructing OffScreenIcon - entering the VM");
		a_root->CreateObject(&g_fauxBlip, "OffScreenIcon");
		if (!g_fauxBlip.IsDisplayObject() && !g_fauxBlip.IsObject()) {
			giveUp("class did not construct");
			return;
		}
		RE::Scaleform::GFx::Value added;
		if (!g_blipHolder.Invoke("addChild", &added, &g_fauxBlip, 1)) {
			giveUp("addChild rejected it");
			return;
		}
		// The name is the contract with the holder loops: anything not named
		// like a real vanilla blip is the mod's own and is never returned to
		// the vanilla container.
		g_fauxBlip.SetMember("name", V{ "ShipNavPanelFauxBlip" });
		g_fauxBlip.SetMember("visible", V{ false });

		a_root->CreateObject(&g_fauxLow);
		a_root->CreateObject(&g_fauxHigh);
		if (!g_fauxLow.IsObject() || !g_fauxHigh.IsObject()) {
			giveUp("payload objects did not construct");
			return;
		}
		// Everything SetTargetLowInfo reads, stated explicitly rather than left
		// undefined. uTargetType and the POI icon fields are set per body at
		// use time.
		g_fauxLow.SetMember("uTargetType", V{ static_cast<std::uint32_t>(kTargetTypePlanet) });
		g_fauxLow.SetMember("uPoiType", V{ static_cast<std::uint32_t>(0) });
		g_fauxLow.SetMember("uPoiCategory", V{ static_cast<std::uint32_t>(0) });
		g_fauxLow.SetMember("uLocationMarkerState", V{ static_cast<std::uint32_t>(0) });
		g_fauxLow.SetMember("hostile", V{ false });
		g_fauxLow.SetMember("bAlly", V{ false });
		g_fauxLow.SetMember("isInfoTarget", V{ false });
		g_fauxLow.SetMember("bHasQuestTarget", V{ false });
		g_fauxLow.SetMember("bIsFreelanesPOI", V{ false });
		g_fauxLow.SetMember("bIsCelestialParentBody", V{ false });
		g_fauxLow.SetMember("bHasUndiscoveredPoi", V{ false });
		g_fauxHigh.SetMember("angleToCrosshair", V{ 0.0 });
		g_fauxHigh.SetMember("distance", V{ 0.0 });

		g_fauxLastID.store(0, std::memory_order_release);
		g_fauxReady.store(true, std::memory_order_release);
		REX::INFO("[blip] faux blip ready - vanilla OffScreenIcon art, mod-driven");
	}

	// Put everything back: every kept blip returns to the vanilla container,
	// then the container is unhidden. Children first, so no blip renders in the
	// mod's holder after the vanilla set is already showing.
	void RestoreVanillaBlips(RE::Scaleform::GFx::Value& a_container)
	{
		using V = RE::Scaleform::GFx::Value;

		if (g_blipHolderReady.load(std::memory_order_acquire)) {
			RE::Scaleform::GFx::Value count;
			if (g_blipHolder.GetMember("numChildren", &count)) {
				for (int i = static_cast<int>(AsNumber(count)) - 1; i >= 0; --i) {
					V idx{ static_cast<double>(i) };
					V child;
					if (!g_blipHolder.Invoke("getChildAt", &child, &idx, 1) ||
						!(child.IsObject() || child.IsDisplayObject()))
						continue;
					// Only real vanilla blips go back; the faux blip and any
					// other mod-owned child stay ours.
					RE::Scaleform::GFx::Value nameVal;
					if (!child.GetMember("name", &nameVal) || !nameVal.IsString())
						continue;
					const std::string childName = nameVal.GetString();
					if (childName.rfind(kOffScreenIconPrefix, 0) != 0)
						continue;
					a_container.Invoke("addChild", nullptr, &child, 1);
				}
			}
		}

		if (g_blipsHidden.exchange(false, std::memory_order_acq_rel)) {
			a_container.SetMember("visible", V{ true });
			REX::INFO("[blip] restored - off-screen blips back to vanilla");
		}
	}

	// Undo the selection-wins-overlap fades: every on-screen icon currently in
	// the display list gets its root alpha back. Reaches children only - a
	// clip pooled while faded is out of range until it revives, at which
	// point the per-tick pass (in cruise) or the next movie rebuild squares
	// it. The residual worst case is a briefly invisible marker for whatever
	// body inherits the clip, healed on the next cruise tick.
	void RestoreFadedIcons(RE::Scaleform::GFx::ASMovieRootBase* a_root, const std::string& a_base)
	{
		if (!g_iconsFaded.exchange(false, std::memory_order_acq_rel))
			return;

		RE::Scaleform::GFx::Value reticle;
		if (!a_root->GetVariable(&reticle, (a_base + ".Reticle_mc").c_str()) ||
			!(reticle.IsObject() || reticle.IsDisplayObject()))
			return;

		using V = RE::Scaleform::GFx::Value;
		V count;
		if (!reticle.GetMember("numChildren", &count))
			return;
		for (int i = static_cast<int>(AsNumber(count)) - 1; i >= 0; --i) {
			V idx{ static_cast<double>(i) };
			V child;
			if (!reticle.Invoke("getChildAt", &child, &idx, 1) ||
				!(child.IsObject() || child.IsDisplayObject()))
				continue;
			V nameVal;
			if (!child.GetMember("name", &nameVal) || !nameVal.IsString())
				continue;
			const std::string childName = nameVal.GetString();
			if (childName.rfind("OnScreenIcon: ", 0) != 0)
				continue;
			child.SetMember("alpha", V{ 1.0 });
		}
		REX::INFO("[blip] crowded markers restored");
	}

	// The per-tick pass, from the high-frequency callback (the engine's UI
	// thread). Two bodies can have their blips let through: the SELECTED one
	// (the panel highlight while browsing, the locked body otherwise) and the
	// LOCKED one, which stays marked while the player browses elsewhere. The
	// two collapse to the same clip whenever they are the same body.
	//
	// Returns whether VANILLA COVERS the selected body this tick - its
	// off-screen blip kept visible, or its on-screen icon showing where the
	// body actually is. Either way the mod draws nothing for that body
	// (v0.8.1/v0.8.2, the tester's calls): the mod's own marker and name are
	// only for bodies with no vanilla presence at all.
	//
	// The selection-wins-overlap pass (v0.8.5, generalised in v0.8.6): vanilla
	// sorts overlapping on-screen icons by UpdateBSV - info target -2,
	// cruise-autopilot lock -1, quest 0, then distance with PLANETS CAPPED AT
	// ONE LIGHT-SECOND - and hides the losers. So a planet in view suppresses
	// a station's marker, and a NEAR station suppresses a far planet's, both
	// against the panel's intent. Zeroing a crowding icon's ROOT alpha both
	// hides it and fails vanilla's own `alpha >= MinBlockingAlpha` blocker
	// gate, so the selection's icon stays visible by vanilla's own rules. The
	// root alpha is written nowhere in the SWF (SetBlockedClipAlpha dims
	// Internal_mc, a child), and the clip pool only keys on `visible`, which
	// is never touched - the trap from the off-screen work does not apply.
	// Deliberately NOT faded: quest-marked icons and the info target's icon
	// (a_infoTargetName, the E-target) - missions and the player's own
	// targeting keep outranking the panel.
	bool ManageVanillaBlips(std::uint32_t a_selectedID, const std::string& a_selectedName,
		std::uint32_t a_lockedID, const std::string& a_lockedName,
		const std::string& a_infoTargetName,
		const BlipGeometry& a_selGeom, const BlipGeometry& a_lockGeom)
	{
		if (!bHideVanillaBlips.GetValue())
			return false;

		// Nothing in here may touch a movie that is loading or freshly loaded -
		// same settle gate as the builders (see WorldSettled for the 2026-07-28
		// evidence). The container's own state needs no unwinding across a
		// load: it goes down with the movie, and the rebuilt one starts
		// visible.
		if (!WorldSettled())
			return false;

		// Cheap gates, in order - this runs every high-frequency tick, and in
		// the v0.8.7 model idle cruising (no panel, no lock, nothing to undo)
		// must cost nothing: the vanilla HUD is genuinely untouched then.
		const bool stateDirty = g_blipsHidden.load(std::memory_order_acquire) ||
		                        g_iconsFaded.load(std::memory_order_acquire);
		if (!stateDirty) {
			if (!g_inCruise.load(std::memory_order_acquire))
				return false;  // out of cruise, nothing to undo
			if (!g_panelOpen.load(std::memory_order_acquire) && a_lockedID == 0)
				return false;  // cruising idle - stay entirely hands-off
		}

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return false;
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud)) {
			// The container went down with the movie; the next one starts
			// visible on its own, and its icons start at full alpha.
			g_blipsHidden.store(false, std::memory_order_release);
			g_iconsFaded.store(false, std::memory_order_release);
			return false;
		}
		const auto menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return false;

		auto*             root = menu->uiMovie->asMovieRoot.get();
		const char*       rootPath = menu->GetRootPath();
		const std::string base = std::string{ rootPath ? rootPath : "root" };

		// Resolved fresh EVERY tick - the container is timeline-placed art the
		// reticle's animations can re-create, so yesterday's handle may be a
		// detached orphan. During such an animation the path can also briefly
		// fail to resolve; skip the tick rather than latch anything.
		RE::Scaleform::GFx::Value container;
		if (!root->GetVariable(&container,
				(base + ".Reticle_mc.ShipReticle_mc.OffScreenIndicatorParent_mc").c_str()) ||
			!(container.IsObject() || container.IsDisplayObject()))
			return false;

		// Cruise read fresh rather than from g_inCruise: the cached flag follows
		// the low-frequency feed and lags a forced exit, and this container also
		// serves normal flight's ship blips - restore must be frame-accurate.
		RE::Scaleform::GFx::Value flag;
		const bool                cruising =
			root->GetVariable(&flag, (base + ".Reticle_mc.CruiseModeHUDActive").c_str()) &&
			flag.IsBoolean() && flag.GetBoolean();

		using V = RE::Scaleform::GFx::Value;

		// The v0.8.7 model, the tester's design: cruise alone leaves the
		// vanilla HUD untouched. The blips hide only while the mod has a
		// stake - the panel is up (its hovered body and the lock stay
		// visible), or a lock exists (only it stays). Clear the lock with the
		// panel closed and everything is vanilla again: the decluttering
		// follows intent instead of the flight mode.
		const bool wantHidden = cruising &&
		                        (g_panelOpen.load(std::memory_order_acquire) || a_lockedID != 0);
		if (!wantHidden) {
			RestoreFadedIcons(root, base);
			RestoreVanillaBlips(container);
			return false;
		}

		// Hide, re-asserted every tick: a timeline rebuild hands back a fresh
		// container with visible=true, and one SetMember is cheap. The SWF only
		// ever writes this container's ALPHA (the boost fade), never visible, so
		// there is no per-frame fight to lose.
		container.SetMember("visible", V{ false });
		if (!g_blipsHidden.exchange(true, std::memory_order_acq_rel))
			REX::INFO("[blip] off-screen blips hidden - panel open or target locked");

		if (WorldSettled()) {
			TryCreateBlipHolder(root, base, container);
			if (bArrow.GetValue() && bVanillaStyleMarker.GetValue())
				TryCreateFauxBlip(root);
		}

		const bool holderReady = g_blipHolderReady.load(std::memory_order_acquire);
		const bool wantBlips = bShowLockedBlip.GetValue() && holderReady;
		const bool keepQuest = bKeepQuestBlips.GetValue() && holderReady;
		// An empty name never matches: every real child name carries the prefix.
		const std::string selectedClipName =
			wantBlips && a_selectedID != 0 && !a_selectedName.empty() ?
				std::string{ kOffScreenIconPrefix } + a_selectedName :
				std::string{};
		const std::string lockedClipName =
			wantBlips && a_lockedID != 0 && !a_lockedName.empty() ?
				std::string{ kOffScreenIconPrefix } + a_lockedName :
				std::string{};

		bool selectedShown = false;
		bool sawShipType = false;

		const auto readName = [](RE::Scaleform::GFx::Value& a_child) -> std::string {
			RE::Scaleform::GFx::Value name;
			if (a_child.GetMember("name", &name) && name.IsString())
				return name.GetString();
			return {};
		};
		const auto isQuest = [](RE::Scaleform::GFx::Value& a_child) {
			RE::Scaleform::GFx::Value quest;
			return a_child.GetMember("HasQuestTarget", &quest) && quest.IsBoolean() &&
			       quest.GetBoolean();
		};

		// Duplicate-name disambiguation (v0.18.1): when the selection's name
		// is shared by 2+ feed entries, a name match alone would keep BOTH
		// contacts' blips (proven in the tester's log - the same clip name
		// kept twice in one tick). The clip's ROOT rotation is exactly
		// angleToCrosshair + 180 (OffScreenIcon.as:163) and vanilla keeps
		// driving kept clips wherever they are parented, so agreement with
		// the candidate's own bearing says WHICH contact a clip is - and a
		// kept clip the pool re-keys to the other contact drifts out of
		// tolerance and pass 2 returns it by itself. Unique names never
		// reach any of this.
		const auto bearingAgrees = [](RE::Scaleform::GFx::Value& a_child, double a_angle) {
			RE::Scaleform::GFx::Value rot;
			if (!a_child.GetMember("rotation", &rot))
				return true;  // unreadable clip: fall back to the name match
			double diff = std::fmod(std::fabs(AsNumber(rot) - (a_angle + 180.0)), 360.0);
			if (diff > 180.0)
				diff = 360.0 - diff;
			return diff <= kDupBearingToleranceDeg;
		};
		const auto blipIsSelected = [&](RE::Scaleform::GFx::Value& a_child) {
			return !a_selGeom.ambiguous || !a_selGeom.haveRow ||
			       bearingAgrees(a_child, a_selGeom.angle);
		};
		const auto blipIsLocked = [&](RE::Scaleform::GFx::Value& a_child) {
			return !a_lockGeom.ambiguous || !a_lockGeom.haveRow ||
			       bearingAgrees(a_child, a_lockGeom.angle);
		};
		if (a_selGeom.ambiguous) {
			// Once per selection, so the log names the situation without
			// ticking - the same gate style as the census.
			static std::atomic<std::uint32_t> s_dupLoggedFor{ 0 };
			if (s_dupLoggedFor.exchange(a_selectedID, std::memory_order_acq_rel) != a_selectedID)
				REX::INFO("[blip] '{}' names more than one contact - telling their clips "
						  "apart by bearing and screen position",
					a_selectedName);
		}

		// The reticle and the selection's/lock's ON-screen icons are resolved
		// BEFORE the keep passes, because visibility there decides the blips'
		// fate: a body whose on-screen icon is visible gets its ring blip
		// culled exactly as vanilla culls a planet's (v0.8.13, the tester's
		// parity call - the census proved vanilla itself does NOT cull it for
		// stations, whose combat-values onScreen stays false, so blip and
		// icon coexist in vanilla).
		//
		// Identity is the INSTANCE name, which vanilla rewrites for the
		// icon's current target every refresh. The census killed text
		// verification: a genuine undiscovered station's icon showed
		// text='Starstation' - the masked generic - against feed name 'Deimos
		// Staryard'. The one clip that skips the instance rename is the info
		// target's edge-snapped paired indicator, and ITS text always names
		// the info target - so that exact signature is the only rejection.
		RE::Scaleform::GFx::Value reticle;
		const bool haveReticle =
			(a_selectedID != 0 || a_lockedID != 0) &&
			root->GetVariable(&reticle, (base + ".Reticle_mc").c_str()) &&
			(reticle.IsObject() || reticle.IsDisplayObject());

		const auto iconIs = [&](RE::Scaleform::GFx::Value& a_icon, const std::string& a_name) {
			if (a_infoTargetName.empty() || a_name == a_infoTargetName)
				return true;
			RE::Scaleform::GFx::Value nameField, text;
			if (a_icon.GetMember("Name_tf", &nameField) &&
				(nameField.IsObject() || nameField.IsDisplayObject()) &&
				nameField.GetMember("text", &text) && text.IsString() &&
				a_infoTargetName == text.GetString())
				return false;  // the paired indicator wearing a stale name
			return true;
		};
		const auto findIcon = [&](const std::string& a_name, const BlipGeometry& a_geom,
								  RE::Scaleform::GFx::Value& a_out) {
			const std::string childName = std::string{ "OnScreenIcon: " } + a_name;
			if (!a_geom.ambiguous) {
				// Unique name: the pre-v0.18.1 path, byte for byte.
				RE::Scaleform::GFx::Value arg{ childName.c_str() };
				if (!reticle.Invoke("getChildByName", &a_out, &arg, 1) ||
					!(a_out.IsObject() || a_out.IsDisplayObject()))
					return false;
				return iconIs(a_out, a_name);
			}
			// Ambiguous name: an icon may only vouch for the selection with
			// POSITIVE geometric confirmation. v0.18.1 fell back to the
			// first-match path when the selection's screen position was the
			// -1 sentinel - which is precisely the off-screen case, so with
			// one contact in-FOV and the OTHER selected, the in-FOV icon
			// "covered" the off-screen selection into invisibility (the
			// tester's second round with the baked save). Every road without
			// confirmation now answers "no icon": the worst that follows is
			// a blip AND an icon both showing - vanilla's own stock look for
			// stations - never an unmarked selection.
			if (!a_geom.haveRow)
				return false;
			if (a_geom.screenX < 0.0 || a_geom.screenX > 1.0 ||
				a_geom.screenY < 0.0 || a_geom.screenY > 1.0)
				return false;  // sentinel or outside the view: no icon is the selection's
			// The expected point, through vanilla's own converter - the exact
			// transform the SWF positions icons with (y percentage runs
			// bottom-up; Extensions.visibleRect handles the safe rect).
			bool   havePoint = false;
			double wantX = 0.0;
			double wantY = 0.0;
			{
				RE::Scaleform::GFx::Value globalFunc;
				if (root->GetVariable(&globalFunc, "Shared.GlobalFunc") &&
					(globalFunc.IsObject() || globalFunc.IsDisplayObject())) {
					RE::Scaleform::GFx::Value args[3];
					args[0] = V{ a_geom.screenX };
					args[1] = V{ a_geom.screenY };
					args[2] = reticle;
					RE::Scaleform::GFx::Value pt;
					RE::Scaleform::GFx::Value m;
					if (globalFunc.Invoke("ConvertScreenPercentsToLocalPoint", &pt, args, 3) &&
						pt.IsObject() && pt.GetMember("x", &m)) {
						wantX = AsNumber(m);
						if (pt.GetMember("y", &m))
							wantY = AsNumber(m);
						havePoint = true;
					}
				}
			}
			if (!havePoint)
				return false;
			V count;
			if (!reticle.GetMember("numChildren", &count))
				return false;
			bool   found = false;
			double bestDist = 0.0;
			for (int i = 0; i < static_cast<int>(AsNumber(count)); ++i) {
				V idx{ static_cast<double>(i) };
				RE::Scaleform::GFx::Value child;
				if (!reticle.Invoke("getChildAt", &child, &idx, 1) ||
					!(child.IsObject() || child.IsDisplayObject()))
					continue;
				RE::Scaleform::GFx::Value nameVal;
				if (!child.GetMember("name", &nameVal) || !nameVal.IsString() ||
					childName != nameVal.GetString())
					continue;
				if (!iconIs(child, a_name))
					continue;
				RE::Scaleform::GFx::Value m;
				const double cx = child.GetMember("x", &m) ? AsNumber(m) : 0.0;
				const double cy = child.GetMember("y", &m) ? AsNumber(m) : 0.0;
				const double d = (cx - wantX) * (cx - wantX) + (cy - wantY) * (cy - wantY);
				if (!found || d < bestDist) {
					bestDist = d;
					a_out = child;
					found = true;
				}
			}
			// Nearest is not enough: with the selection's own icon absent the
			// nearest same-named icon is simply the OTHER contact's. It must
			// actually sit at the expected point.
			return found &&
			       bestDist <= kDupIconMatchTolerancePx * kDupIconMatchTolerancePx;
		};
		const auto isVisible = [](RE::Scaleform::GFx::Value& a_icon) {
			RE::Scaleform::GFx::Value vis;
			return a_icon.GetMember("visible", &vis) && vis.IsBoolean() && vis.GetBoolean();
		};

		RE::Scaleform::GFx::Value selIcon;
		bool                      selFound = false;
		bool                      selVisible = false;
		bool                      lockIconVisible = false;
		if (haveReticle && a_selectedID != 0 && !a_selectedName.empty() &&
			findIcon(a_selectedName, a_selGeom, selIcon)) {
			selFound = true;
			selVisible = isVisible(selIcon);
		}
		if (a_lockedID != 0 && a_lockedID == a_selectedID) {
			lockIconVisible = selVisible;
		} else if (haveReticle && a_lockedID != 0 && !a_lockedName.empty()) {
			RE::Scaleform::GFx::Value lockIcon;
			if (findIcon(a_lockedName, a_lockGeom, lockIcon))
				lockIconVisible = isVisible(lockIcon);
		}

		// Pass 1: the container. Keepers move into the holder; everything else
		// stays hidden with its parent. Descending index, because a move
		// reindexes the children above the removed slot.
		RE::Scaleform::GFx::Value count;
		if (container.GetMember("numChildren", &count)) {
			for (int i = static_cast<int>(AsNumber(count)) - 1; i >= 0; --i) {
				V idx{ static_cast<double>(i) };
				V child;
				if (!container.Invoke("getChildAt", &child, &idx, 1) ||
					!(child.IsObject() || child.IsDisplayObject()))
					continue;
				const std::string childName = readName(child);
				if (childName.rfind(kOffScreenIconPrefix, 0) != 0)
					continue;

				// Ships never get off-screen icons in cruise - one existing
				// means cruise is over, whatever the flag still says. This is
				// the independent "still cruising?" signal, and it fires
				// exactly when hidden blips would hurt: combat.
				RE::Scaleform::GFx::Value type;
				if (child.GetMember("QLastTargetType", &type) &&
					static_cast<std::uint32_t>(AsNumber(type)) == kTargetTypeShip)
					sawShipType = true;

				// A body whose on-screen icon is visible does not get a ring
				// blip on top - the icon marks it, cull the blip like a
				// planet's. With a duplicated name the bearing joins the
				// test, else BOTH same-named contacts' blips get kept.
				const bool selectedMatch = !selectedClipName.empty() &&
				                           childName == selectedClipName && !selVisible &&
				                           blipIsSelected(child);
				const bool lockedMatch = !lockedClipName.empty() &&
				                         childName == lockedClipName && !lockIconVisible &&
				                         blipIsLocked(child);
				if (selectedMatch || lockedMatch || (keepQuest && isQuest(child))) {
					if (g_blipHolder.Invoke("addChild", nullptr, &child, 1)) {
						if (selectedMatch)
							selectedShown = true;
						if (bVerboseLog.GetValue())
							REX::INFO("[blip] kept '{}'{}", childName,
								selectedMatch ? (lockedMatch ? " (locked body)" : " (panel highlight)") :
								lockedMatch   ? " (locked body)" :
								                " (quest target)");
					}
				}
			}
		}

		// Pass 2: the holder. A kept blip whose reason lapsed - lock moved,
		// quest done - goes back to the (hidden) container for vanilla to pool
		// or re-show as it pleases. A pooled blip never appears here: the SWF's
		// sweep removes it from whatever parent it has.
		if (holderReady && g_blipHolder.GetMember("numChildren", &count)) {
			for (int i = static_cast<int>(AsNumber(count)) - 1; i >= 0; --i) {
				V idx{ static_cast<double>(i) };
				V child;
				if (!g_blipHolder.Invoke("getChildAt", &child, &idx, 1) ||
					!(child.IsObject() || child.IsDisplayObject()))
					continue;
				const std::string childName = readName(child);
				if (childName.rfind(kOffScreenIconPrefix, 0) != 0)
					continue;  // the faux blip and other mod-owned children stay
				// Same bearing test as pass 1: a kept clip the pool re-keys
				// to the OTHER same-named contact drifts out of tolerance
				// and goes home on its own.
				const bool selectedMatch = !selectedClipName.empty() &&
				                           childName == selectedClipName && !selVisible &&
				                           blipIsSelected(child);
				const bool lockedMatch = !lockedClipName.empty() &&
				                         childName == lockedClipName && !lockIconVisible &&
				                         blipIsLocked(child);
				if (selectedMatch || lockedMatch) {
					if (selectedMatch)
						selectedShown = true;
				} else if (!(keepQuest && isQuest(child))) {
					container.Invoke("addChild", nullptr, &child, 1);
					if (bVerboseLog.GetValue())
						REX::INFO("[blip] returned '{}'", childName);
				}
			}
		}

		if (sawShipType) {
			RestoreFadedIcons(root, base);
			RestoreVanillaBlips(container);
			REX::INFO("[blip] ship-type blip seen while 'cruising' - trusting the data "
					  "over the flag and restoring");
			return false;
		}

		// The other kind of vanilla presence: the ON-screen icon - the circle
		// sitting where the body actually is, with its name - which vanilla
		// draws instead of an off-screen blip once the body is in view (in
		// cruise the two are mutually exclusive). getChildByName asks
		// vanilla's own display list, which is the authoritative answer to
		// "is it marked on screen".
		//
		// Pooled clips keep stale instance names - the edge-snapped indicator
		// for the info target is renamed by a different path - so every hit
		// is verified against Name_tf.text, which SetTargetLowInfo rewrites
		// from the CURRENT target every refresh.
		bool onScreenCovered = false;
		{
			struct Rect
			{
				double x{ 0.0 }, y{ 0.0 }, w{ 0.0 }, h{ 0.0 };
			};
			const auto readBounds = [](RE::Scaleform::GFx::Value& a_icon, Rect& a_out) {
				RE::Scaleform::GFx::Value rect;
				if (!a_icon.Invoke("GetPositionAdjustedBounds", &rect, nullptr, 0) ||
					!rect.IsObject())
					return false;
				RE::Scaleform::GFx::Value m;
				a_out.x = rect.GetMember("x", &m) ? AsNumber(m) : 0.0;
				a_out.y = rect.GetMember("y", &m) ? AsNumber(m) : 0.0;
				a_out.w = rect.GetMember("width", &m) ? AsNumber(m) : 0.0;
				a_out.h = rect.GetMember("height", &m) ? AsNumber(m) : 0.0;
				return a_out.w > 0.0 && a_out.h > 0.0;
			};

			// selIcon / selFound / selVisible were resolved before the keep
			// passes - visibility there is what culls the ring blip.

			// Diagnostic, once per selection: when the selection has neither a
			// kept blip nor a findable on-screen icon, dump what on-screen
			// icons DO exist, so the log answers "what was vanilla actually
			// showing" instead of the next theory guessing it. One selection's
			// dump is ~a dozen lines, and it only fires on the failure case.
			if (haveReticle && !selFound && !selectedShown && bVerboseLog.GetValue()) {
				static std::atomic<std::uint32_t> s_dumpedFor{ 0 };
				if (s_dumpedFor.exchange(a_selectedID, std::memory_order_acq_rel) != a_selectedID) {
					REX::INFO("[blip-dbg] no icon accepted for '{}' ({:08X}) - reticle census:",
						a_selectedName, a_selectedID);
					V dbgCount;
					if (reticle.GetMember("numChildren", &dbgCount)) {
						for (int i = 0; i < static_cast<int>(AsNumber(dbgCount)); ++i) {
							V idx{ static_cast<double>(i) };
							V child;
							if (!reticle.Invoke("getChildAt", &child, &idx, 1) ||
								!(child.IsObject() || child.IsDisplayObject()))
								continue;
							V nameVal;
							if (!child.GetMember("name", &nameVal) || !nameVal.IsString())
								continue;
							const std::string childName = nameVal.GetString();
							if (childName.rfind("OnScreenIcon: ", 0) != 0)
								continue;
							V           vis, alpha, nameMc, nameShown, nameField, text;
							const bool  visB = child.GetMember("visible", &vis) &&
							                  vis.IsBoolean() && vis.GetBoolean();
							const double alphaN =
								child.GetMember("alpha", &alpha) ? AsNumber(alpha) : -1.0;
							const bool nameVis =
								child.GetMember("Name_mc", &nameMc) &&
								(nameMc.IsObject() || nameMc.IsDisplayObject()) &&
								nameMc.GetMember("visible", &nameShown) &&
								nameShown.IsBoolean() && nameShown.GetBoolean();
							const char* shownText =
								child.GetMember("Name_tf", &nameField) &&
								        (nameField.IsObject() || nameField.IsDisplayObject()) &&
								        nameField.GetMember("text", &text) && text.IsString() ?
									text.GetString() :
									"<none>";
							REX::INFO("[blip-dbg]   '{}' visible={} alpha={:.2f} nameShown={} "
									  "text='{}'",
								childName, visB, alphaN, nameVis, SafeStr(shownText));
						}
					}
					REX::INFO("[blip-dbg] census end");
				}
			}

			// Selection-wins-overlap (v0.8.5, generalised v0.8.6): while the
			// selected body's icon exists, fade whatever on-screen icon is
			// crowding it - planet over station AND station over planet, the
			// tester hit both directions - and unfade the rest. Level-based,
			// so it cannot oscillate: a faded icon keeps its geometry while
			// invisible, and the overlap is re-tested from fresh rectangles
			// every tick, using the same GetPositionAdjustedBounds vanilla's
			// own overlap pass intersects. Quest-marked icons and the
			// E-target's icon are exempt and re-asserted to full alpha - and
			// when an EXEMPT icon is the one crowding the selection, that
			// counts as coverage (v0.8.8): vanilla is deliberately showing
			// the E-target or mission marker on top of where the selection
			// is, and the mod adding its own marker beside it was clutter.
			bool fadedBlocker = false;
			bool exemptCovers = false;
			if (haveReticle && selFound && bSelectionWinsOverlap.GetValue()) {
				const auto intersects = [](const Rect& a_a, const Rect& a_b) {
					return a_a.x < a_b.x + a_b.w && a_b.x < a_a.x + a_a.w &&
					       a_a.y < a_b.y + a_b.h && a_b.y < a_a.y + a_a.h;
				};
				Rect selRect;
				V    childCount;
				if (readBounds(selIcon, selRect) && reticle.GetMember("numChildren", &childCount)) {
					constexpr const char* kOnScreenPrefix = "OnScreenIcon: ";
					for (int i = static_cast<int>(AsNumber(childCount)) - 1; i >= 0; --i) {
						V idx{ static_cast<double>(i) };
						V child;
						if (!reticle.Invoke("getChildAt", &child, &idx, 1) ||
							!(child.IsObject() || child.IsDisplayObject()))
							continue;
						V nameVal;
						if (!child.GetMember("name", &nameVal) || !nameVal.IsString())
							continue;
						const std::string childName = nameVal.GetString();
						if (childName.rfind(kOnScreenPrefix, 0) != 0)
							continue;
						const std::string bodyName = childName.substr(std::strlen(kOnScreenPrefix));
						if (bodyName == a_selectedName) {
							// The selection's own icon is HEALED here, not
							// skipped: it can carry alpha 0 from a tick when it
							// was the CROWDER (hover Venus and overlapping
							// Mercury fades; now wheel to Mercury). The old
							// plain `continue` left that alpha in place - the
							// level-based restore below never reaches the
							// selection - so the selection showed nothing,
							// selVisible (which reads `visible`, not alpha)
							// still said "marked" and stood the faux marker
							// down, and the neighbour faded as a blocker: both
							// bodies invisible until a third was selected (the
							// tester's Venus/Mercury trap, 2026-07-29).
							if (iconIs(child, bodyName))
								child.SetMember("alpha", V{ 1.0 });
							continue;
						}

						// Identity-checked like the selection lookup: the
						// displayed name when shown, the instance name when
						// the icon hides its name (undiscovered markers).
						if (!iconIs(child, bodyName))
							continue;

						// Missions and the player's own E-target keep their
						// priority - never fade those, and heal them if an
						// earlier tick faded them.
						V          quest;
						const bool exempt =
							(child.GetMember("HasQT", &quest) && quest.IsBoolean() &&
								quest.GetBoolean()) ||
							(!a_infoTargetName.empty() && bodyName == a_infoTargetName);
						Rect childRect;
						if (exempt) {
							child.SetMember("alpha", V{ 1.0 });
							if (!exemptCovers && readBounds(child, childRect) &&
								intersects(selRect, childRect))
								exemptCovers = true;
							continue;
						}

						const bool overlap = readBounds(child, childRect) &&
						                     intersects(selRect, childRect);
						child.SetMember("alpha", V{ overlap ? 0.0 : 1.0 });
						if (overlap) {
							fadedBlocker = true;
							if (!g_iconsFaded.exchange(true, std::memory_order_acq_rel))
								REX::INFO("[blip] fading '{}' - it was crowding the selection",
									bodyName);
						}
					}
				}
			} else if (g_iconsFaded.load(std::memory_order_acquire)) {
				// Selection gone, off screen, or the feature is off - give
				// the crowded markers back.
				RestoreFadedIcons(root, base);
			}

			// Covered: visibly marked, about to be (a freshly faded blocker
			// leaves the selection's icon hidden until vanilla's next overlap
			// pass runs, and the mod's marker must not flash into that
			// one-tick gap), or deliberately marked-over by an exempt icon.
			onScreenCovered = selFound && (selVisible || fadedBlocker || exemptCovers);
		}

		return selectedShown || onScreenCovered;
	}

	// Borrow a TextFormat from a field the HUD already owns. A TextField built at
	// runtime carries no font and renders every glyph as a placeholder box; the
	// HUD's own fields have fonts embedded at author time and it never names one
	// in code, so there is nothing to copy but the whole format object.
	//
	// Each caller needs its OWN borrowed copy - the format is a live object, so
	// setting `size` on a shared one would resize the arrow label too.
	bool BorrowTextFormat(RE::Scaleform::GFx::ASMovieRootBase* a_root, const char* a_rootPath,
		RE::Scaleform::GFx::Value& a_format, const char* a_logTag)
	{
		const std::string base = std::string{ a_rootPath ? a_rootPath : "root" };
		const char*       donors[]{
            ".Reticle_mc.ShipReticle_mc.LockOn_mc.LockText_tf",
            ".Reticle_mc.ShipReticle_mc.Distance_tf",
            ".DebugText_tf",
		};

		for (const auto* suffix : donors) {
			RE::Scaleform::GFx::Value donor;
			if (!a_root->GetVariable(&donor, (base + suffix).c_str()))
				continue;
			if (!donor.Invoke("getTextFormat", &a_format) || !a_format.IsObject())
				continue;

			RE::Scaleform::GFx::Value fontName;
			if (a_format.GetMember("font", &fontName) && fontName.IsString())
				REX::INFO("{} borrowed font '{}' from {}", a_logTag, SafeStr(fontName.GetString()), suffix);
			else
				REX::INFO("{} borrowed a text format from {}", a_logTag, suffix);
			return true;
		}
		return false;
	}

	// ---------------------------------------------------------------------------
	// The list panel.
	//
	// Built once, with a fixed number of rows, and then only ever updated: text,
	// visibility and the highlight's y. Constructing AS3 objects from a feed
	// callback is the risk this mod has been most careful about, so it happens
	// exactly once per movie.
	//
	// Parented to Reticle_mc rather than the menu root, because the reticle's
	// origin is screen centre and that is a coordinate space already proven by
	// the arrow. The offsets are from there, which makes them resolution-relative
	// in the way a guessed stage coordinate would not be.
	// ---------------------------------------------------------------------------

	// Draws a red square, calls clear(), then draws a blue square somewhere else.
	// Both the call's own answer and the screen answer the question, and they
	// are worth having separately: Invoke returning true only says the method
	// existed and was callable, not that it did anything.
	//
	//   only the BLUE square  -> clear() works; one icon clip per row, redrawn
	//                            when that row's body changes.
	//   BOTH squares          -> it does not; every class needs its own clip and
	//                            the row toggles which is visible.
	void RunGraphicsClearTest(RE::Scaleform::GFx::Value& a_parent)
	{
		using V = RE::Scaleform::GFx::Value;

		RE::Scaleform::GFx::Value clip;
		if (!a_parent.CreateEmptyMovieClip(&clip, "ShipNavPanelClearTest", 20050)) {
			REX::WARN("[cleartest] could not create the test clip");
			return;
		}

		RE::Scaleform::GFx::Value gfx;
		if (!clip.GetMember("graphics", &gfx)) {
			REX::WARN("[cleartest] test clip has no graphics");
			return;
		}

		const auto square = [&](double a_x0, double a_y0, double a_x1, double a_y1, std::uint32_t a_colour) {
			V fill[]{ V{ a_colour }, V{ 1.0 } };
			gfx.Invoke("beginFill", nullptr, fill, 2);
			V p0[]{ V{ a_x0 }, V{ a_y0 } };
			gfx.Invoke("moveTo", nullptr, p0, 2);
			V p1[]{ V{ a_x1 }, V{ a_y0 } };
			gfx.Invoke("lineTo", nullptr, p1, 2);
			V p2[]{ V{ a_x1 }, V{ a_y1 } };
			gfx.Invoke("lineTo", nullptr, p2, 2);
			V p3[]{ V{ a_x0 }, V{ a_y1 } };
			gfx.Invoke("lineTo", nullptr, p3, 2);
			gfx.Invoke("lineTo", nullptr, p0, 2);
			gfx.Invoke("endFill", nullptr, nullptr, 0);
		};

		square(-80.0, -40.0, -30.0, 10.0, 0xFF3333);

		RE::Scaleform::GFx::Value result;
		const bool                called = gfx.Invoke("clear", &result, nullptr, 0);

		square(30.0, -40.0, 80.0, 10.0, 0x3388FF);

		clip.SetMember("x", V{ 0.0 });
		clip.SetMember("y", V{ 0.0 });
		clip.SetMember("visible", V{ true });

		REX::INFO("[cleartest] graphics.clear() invoke returned {} ({})", called,
			called ? "the method exists and was callable" : "no such method - clear() is unavailable");
		REX::INFO("[cleartest] now LOOK at the middle of the screen, left of the crosshair:");
		REX::INFO("[cleartest]   only a BLUE square  -> clear() works, one icon clip per row will do");
		REX::INFO("[cleartest]   a RED and a BLUE    -> it does not, each class needs its own clip");
	}

	// Each class as a small drawn glyph. Polygons rather than circles, and drawn
	// rather than typed, for the reason the wheel symbols are: the borrowed font
	// carries only the glyphs its author embedded, and `drawCircle` is one more
	// method than needs proving. Size and colour do most of the work - giants
	// are large and ringed, rock is a solid diamond, ice is pale, asteroids are
	// a scatter of small marks.
	// Whether `graphics.drawCircle` exists here. Unknown until the first attempt,
	// then remembered - and if it is missing, a polygon stands in, so a wrong
	// guess costs a slightly less round circle rather than a blank icon.
	std::atomic<int> g_drawCircleWorks{ -1 };  // -1 unknown, 0 no, 1 yes

	void DrawRowIcon(RE::Scaleform::GFx::Value& a_graphics, PlanetClass a_class, bool a_settled)
	{
		using V = RE::Scaleform::GFx::Value;

		const auto fill = [&](std::uint32_t a_colour, double a_alpha) {
			V args[]{ V{ a_colour }, V{ a_alpha } };
			a_graphics.Invoke("beginFill", nullptr, args, 2);
		};

		// A real circle if the player's Scaleform has one; a 16-gon otherwise,
		// which at this size is indistinguishable.
		const auto disc = [&](double a_radius) {
			if (g_drawCircleWorks.load(std::memory_order_acquire) != 0) {
				V args[]{ V{ 0.0 }, V{ 0.0 }, V{ a_radius } };
				const bool ok = a_graphics.Invoke("drawCircle", nullptr, args, 3);
				if (g_drawCircleWorks.exchange(ok ? 1 : 0, std::memory_order_acq_rel) == -1)
					REX::INFO("[panel] graphics.drawCircle {} - icons use {}",
						ok ? "is available" : "is NOT available", ok ? "real circles" : "polygons");
				if (ok) {
					a_graphics.Invoke("endFill", nullptr, nullptr, 0);
					return;
				}
			}

			constexpr int kSides = 16;
			for (int i = 0; i <= kSides; ++i) {
				const double angle = 6.28318530717958647692 * i / kSides;
				V            point[]{ V{ a_radius * std::cos(angle) }, V{ a_radius * std::sin(angle) } };
				a_graphics.Invoke(i == 0 ? "moveTo" : "lineTo", nullptr, point, 2);
			}
			a_graphics.Invoke("endFill", nullptr, nullptr, 0);
		};
		const auto poly = [&](std::initializer_list<std::pair<double, double>> a_points) {
			bool first = true;
			for (const auto& [x, y] : a_points) {
				V point[]{ V{ x }, V{ y } };
				a_graphics.Invoke(first ? "moveTo" : "lineTo", nullptr, point, 2);
				first = false;
			}
			const auto& start = *a_points.begin();
			V           close[]{ V{ start.first }, V{ start.second } };
			a_graphics.Invoke("lineTo", nullptr, close, 2);
			a_graphics.Invoke("endFill", nullptr, nullptr, 0);
		};
		const auto diamond = [&](double a_r) {
			poly({ { 0.0, -a_r }, { a_r, 0.0 }, { 0.0, a_r }, { -a_r, 0.0 } });
		};
		const auto bar = [&](double a_halfWidth, double a_halfHeight) {
			poly({ { -a_halfWidth, -a_halfHeight },
				{ a_halfWidth, -a_halfHeight },
				{ a_halfWidth, a_halfHeight },
				{ -a_halfWidth, a_halfHeight } });
		};

		// ONLY the exceptions get a mark. A glyph on every row was accurate and
		// unhelpful - a column of icons beside a column of names is noise, since
		// the names already say which body is which. What earns the space is a
		// body you cannot land on, or one worth going to.
		const auto ringedGiant = [&](std::uint32_t a_body, std::uint32_t a_ring) {
			fill(a_body, 0.95);
			disc(5.5);
			fill(a_ring, 0.85);
			bar(9.5, 1.0);
		};

		// A skyline: three towers on a strip of ground, in the panel's own marker
		// colour. Drawn rather than typed for the reason the wheel symbols are -
		// the borrowed font carries only the glyphs its author embedded. The
		// clip's origin is the middle of the row, and y grows downwards, so the
		// ground sits below zero and the towers rise above it.
		const auto settlement = [&] {
			constexpr double kGround = 3.0;

			// EVERY shape opens its own fill. `poly` closes with endFill, which
			// ends the run - so one beginFill up front draws the first shape and
			// silently leaves the rest unfilled. That is exactly what v0.7.0
			// shipped: a bare ground line with three invisible towers above it.
			// `ringedGiant` gets this right by filling before each of its two
			// shapes, which is why only the new glyph was wrong.
			const auto slab = [&](double a_left, double a_top, double a_right, double a_bottom) {
				fill(0x66CCFF, 0.95);  // the panel's own marker colour
				poly({ { a_left, a_top }, { a_right, a_top }, { a_right, a_bottom }, { a_left, a_bottom } });
			};

			slab(-6.0, kGround, 6.0, kGround + 1.0);
			slab(-5.5, kGround - 4.0, -2.5, kGround);
			slab(-1.5, kGround - 7.0, 1.5, kGround);
			slab(2.5, kGround - 5.0, 5.5, kGround);
		};

		// Settlement wins over the giant glyph, and the precedence is stated
		// rather than left to fall out of the switch. The two should never meet -
		// a gas giant cannot be landed on, so it cannot hold a city - and if a
		// record ever claims both, the one that says "there is something here"
		// is worth more to a pilot than the one that says "keep out".
		if (a_settled) {
			settlement();
			return;
		}

		switch (a_class) {
		case PlanetClass::kGasGiant:
			ringedGiant(0xE0B77A, 0xF2DCB4);
			break;
		case PlanetClass::kHotGasGiant:
			ringedGiant(0xE8895A, 0xF7C39E);
			break;
		case PlanetClass::kIceGiant:
			ringedGiant(0x8FD3F0, 0xCFEBFF);
			break;
		default:
			// Everything else draws nothing: the row is unremarkable, and saying
			// so with a symbol only crowds the ones that are not.
			(void)diamond;
			break;
		}
	}

	// Whether a body's icon is worth a clip at all - the rows that draw nothing
	// keep theirs hidden.
	bool HasRowIcon(PlanetClass a_class, bool a_settled)
	{
		return a_settled || a_class == PlanetClass::kGasGiant || a_class == PlanetClass::kHotGasGiant ||
		       a_class == PlanetClass::kIceGiant;
	}

	// ---------------------------------------------------------------------------
	// Which device is the player actually holding (v1.1.0).
	//
	// `uiController` is a public getter on `Shared.AS3.BSDisplayObject`, so every
	// vanilla clip in the movie carries one and the engine keeps it current
	// across a device swap - it is fed by the same `ControlMapData` subscription
	// every button uses. This is vanilla's OWN authority for the question: the
	// reticle branches on exactly this expression to choose between its gamepad
	// and keyboard cruise hints (ShipReticle.CruiseReticleButtonBaseData).
	//
	// Read off `Reticle_mc`, the stable clip the rest of this mod already leans
	// on, and by the same GetVariable-a-getter route as `CruiseModeHUDActive`.
	// `PLATFORM_PC_KB_MOUSE` is 0 and `PLATFORM_INVALID` is uint.MAX_VALUE; an
	// invalid or unreadable value returns nothing so the caller keeps whatever it
	// already chose rather than guessing a device and dressing the pill wrong.
	std::optional<bool> UsingGamepad(RE::Scaleform::GFx::ASMovieRootBase* a_root,
		const std::string& a_reticlePath)
	{
		RE::Scaleform::GFx::Value v;
		if (!a_root->GetVariable(&v, (a_reticlePath + ".uiController").c_str()))
			return std::nullopt;
		// It is an AS3 `uint`, and GFx hands those back as kUInt rather than
		// kNumber - testing IsNumber() alone would have rejected every read and
		// left the pill permanently in whatever dress it was built with, which
		// is exactly the silent half-dead feature this mod keeps learning to
		// avoid. AsNumber takes all three numeric types.
		if (!v.IsNumber() && !v.IsInt() && !v.IsUInt())
			return std::nullopt;
		const double raw = AsNumber(v);
		if (!(raw >= 0.0) || raw >= 4294967295.0)
			return std::nullopt;  // PLATFORM_INVALID, or nothing sensible
		return raw > 0.0;         // PLATFORM_PC_KB_MOUSE is 0; everything else is a pad
	}

	// The `ButtonBaseData` a pill is driven by. `a_events` is a comma-separated
	// list: one entry passes a bare `UserEventData`, several pass an Array of
	// them, which is what vanilla's own two-way hints do and what makes a D-pad
	// cap read as up+down rather than a single arrow.
	bool BuildPillData(RE::Scaleform::GFx::ASMovieRootBase* a_root, const std::string& a_label,
		const std::string& a_events, RE::Scaleform::GFx::Value& a_out)
	{
		using V = RE::Scaleform::GFx::Value;

		std::vector<V> events;
		std::string_view rest{ a_events };
		while (!rest.empty()) {
			const auto comma = rest.find(',');
			auto       entry = rest.substr(0, comma);
			rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);

			constexpr std::string_view kSpace = " \t";
			if (const auto from = entry.find_first_not_of(kSpace); from != std::string_view::npos)
				entry.remove_prefix(from);
			else
				continue;
			if (const auto to = entry.find_last_not_of(kSpace); to != std::string_view::npos)
				entry = entry.substr(0, to + 1);
			// A `#id` entry has no binding for the component to resolve, so it
			// can never produce a cap - skip it rather than draw an empty one.
			if (entry.front() == '#')
				continue;

			V name;
			a_root->CreateString(&name, std::string{ entry }.c_str());
			V ued;
			a_root->CreateObject(&ued,
				"Shared.Components.ButtonControls.ButtonData.UserEventData", &name, 1);
			if (ued.IsObject())
				events.push_back(ued);
		}

		if (events.empty())
			return false;

		V userEvents = events.front();
		if (events.size() > 1) {
			// Filled with AS3's own `push` rather than `Value::PushBack`: the
			// latter is one of the few Value methods routed through an Address
			// Library id, and this mod's rule is to spend ids only where there
			// is no vtable or script route. `CreateArray` is a plain vtable
			// slot beside `CreateObject`, so only the fill needed rethinking.
			//
			// Falls back to the single first event if either step fails - a
			// one-key cap is a smaller loss than no pill.
			V arr;
			a_root->CreateArray(&arr);
			if (arr.IsArray()) {
				bool filled = true;
				for (const auto& e : events)
					filled = arr.Invoke("push", nullptr, &e, 1) && filled;
				if (filled)
					userEvents = arr;
			}
		}

		V args[2];
		a_root->CreateString(&args[0], a_label.c_str());
		args[1] = userEvents;
		a_root->CreateObject(&a_out,
			"Shared.Components.ButtonControls.ButtonData.ButtonBaseData", args, 2);
		return a_out.IsObject();
	}

	// The browse pill's event list for the device in use.
	std::string BrowsePillEvents(bool a_gamepad)
	{
		return a_gamepad ? sPanelBrowsePillEventPad.GetValue() : sPanelBrowsePillEvent.GetValue();
	}

	// Its label, tokenised, falling back to the English phrase the drawn glyph
	// has always carried. Shared so that a pill re-dressed mid-session cannot
	// drift from the one built at startup.
	std::string BrowsePillLabel()
	{
		std::string label = sPanelBrowseLabel.GetValue();
		if (!label.empty() && label.front() == '$') {
			const std::string translated = TranslateToken(label.c_str());
			label = translated.empty() ? std::string{ "wheel to browse" } : translated;
		}
		return label;
	}

	// ---------------------------------------------------------------------------
	// Keep the browse pill wearing the device the player is actually holding.
	//
	// **The confirm pill needs none of this, and that difference is the whole
	// point.** It is driven by ONE event that both devices bind, so the vanilla
	// component re-resolves its own cap when the control map changes and follows
	// a device swap by itself - it subscribes `ControlMapData` in its own ctor.
	// The browse pill cannot: the wheel and the D-pad are DIFFERENT events, so
	// following a swap means handing the component a different event, and only
	// the mod can decide that. A component that re-renders is not the same as a
	// component that re-chooses.
	//
	// First cut did this once per panel OPEN, which left a swap made WHILE the
	// panel was up showing the other device's cap until it was closed and
	// reopened (the tester caught it: the confirm pill changed on the fly, this
	// one did not). It now rides the panel's 0.25 s text cadence instead - one
	// GetVariable four times a second while the panel is open, beside the
	// per-row SetText and textWidth work already on that tick - and the
	// SetButtonData fires only on an actual change, so a session that never
	// swaps device pays nothing but the read.
	// ---------------------------------------------------------------------------
	void RefreshBrowsePillDevice()
	{
		if (!(g_panelBrowsePill.IsObject() || g_panelBrowsePill.IsDisplayObject()))
			return;

		const auto                     ui = RE::UI::GetSingleton();
		static const RE::BSFixedString s_hud{ kShipHudMenu };
		const auto                     menu = ui ? ui->GetMenu(s_hud) : nullptr;
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return;

		auto*             root = menu->uiMovie->asMovieRoot.get();
		const char*       rootPath = menu->GetRootPath();
		const std::string reticlePath = std::string{ rootPath ? rootPath : "root" } + ".Reticle_mc";

		const auto pad = UsingGamepad(root, reticlePath);
		if (!pad)
			return;  // unreadable or PLATFORM_INVALID: keep the dress it has

		const int   want = *pad ? 2 : 1;
		const char* which = *pad ? "the controller" : "keyboard and mouse";
		if (g_panelBrowsePillDevice.load(std::memory_order_acquire) == want)
			return;

		// Stamped whether or not the re-dress lands. The settings are read once
		// per session, so a failure here would fail identically every time, and
		// retrying it on a 0.25 s cadence is VM work that can never come good.
		g_panelBrowsePillDevice.store(want, std::memory_order_release);

		RE::Scaleform::GFx::Value data;
		if (BuildPillData(root, BrowsePillLabel(), BrowsePillEvents(*pad), data) &&
			g_panelBrowsePill.Invoke("SetButtonData", nullptr, &data, 1))
			REX::INFO("[panel] browse pill re-dressed for {}", which);
		else
			REX::WARN("[panel] browse pill could not be re-dressed for {} - it keeps the cap it had",
				which);
	}

	void TryCreatePanel()
	{
		if (!bPanel.GetValue() || g_panelReady.load(std::memory_order_acquire) ||
			g_panelFailed.load(std::memory_order_acquire))
			return;
		if (!g_subscribed.load(std::memory_order_acquire))
			return;  // no feed yet, so nothing to list

		// Builds TextFields and clips through the AS3 VM, so it is serialised
		// for the same reason the subscriber is - see SingleWinner.
		const SingleWinner winner{ g_panelBuildInFlight };
		if (!winner.Won())
			return;
		if (g_panelReady.load(std::memory_order_acquire))
			return;  // built while we were waiting for the claim

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud))
			return;
		const auto menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return;

		auto*             root = menu->uiMovie->asMovieRoot.get();
		const char*       rootPath = menu->GetRootPath();
		const std::string reticlePath = std::string{ rootPath ? rootPath : "root" } + ".Reticle_mc";

		RE::Scaleform::GFx::Value reticle;
		if (!root->GetVariable(&reticle, reticlePath.c_str()))
			return;

		const auto giveUp = [&](const char* a_why) {
			REX::WARN("[panel] not created: {}", a_why);
			g_panelFailed.store(true, std::memory_order_release);
		};

		if (bTestGraphicsClear.GetValue())
			RunGraphicsClearTest(reticle);

		using V = RE::Scaleform::GFx::Value;

		// Depth 20001 puts the list above the arrow's 20000. The pre-line means
		// a log that ENDS here names the frozen call.
		REX::INFO("[panel] building - entering the VM");
		if (!reticle.CreateEmptyMovieClip(&g_panelClip, "ShipNavPanelList", 20001)) {
			giveUp("CreateEmptyMovieClip refused a container for the list");
			return;
		}

		const auto   rows = std::clamp<std::size_t>(uPanelMaxRows.GetValue(), 1, kPanelMaxRowsHard);
		const double rowHeight = static_cast<double>(fPanelRowHeight.GetValue());
		const double width = static_cast<double>(fPanelWidth.GetValue());

		const bool hints = bPanelHints.GetValue();
		// The title, composed of segments split on '|': every '$' segment
		// translates through the game's own tables, everything else is
		// literal - so the default "$CRUISE| - |$Outpost_AvailableTargets"
		// renders as the two words in the player's language around a plain
		// dash (v0.15.0, the tester's composition). Any segment failing to
		// translate fails the whole title to the English fallback rather
		// than showing a bare token.
		std::string title;
		{
			const std::string raw = sPanelTitle.GetValue();
			bool              failed = false;
			std::size_t       start = 0;
			while (start <= raw.size()) {
				const auto        sep = raw.find('|', start);
				const std::string seg =
					raw.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
				if (!seg.empty() && seg.front() == '$') {
					const std::string translated = TranslateToken(seg.c_str());
					if (translated.empty()) {
						failed = true;
						break;
					}
					title += translated;
				} else {
					title += seg;
				}
				if (sep == std::string::npos)
					break;
				start = sep + 1;
			}
			if (failed) {
				REX::WARN("[panel] title '{}' did not fully translate - using the fallback words", raw);
				title = "NAVIGATION TARGETS";
			} else if (title != raw && !raw.empty()) {
				REX::INFO("[panel] title '{}' from '{}'", title, raw);
			}
		}
		// The header mirrors the footer: a title where the hints have their
		// text, a hairline between it and the rows, and the rows' 6.0 pad
		// below its rule matching the pad above the footer's. An empty title
		// disables it as surely as the flag does. Taller than the footer and
		// in brighter, larger type - v0.10.0's 24 px strip of hint-coloured
		// 14 px text did not read as a title (the tester's call).
		const bool   header = bPanelHeader.GetValue() && !title.empty();
		const double headerHeight = header ? 30.0 : 0.0;
		const double listTop = 6.0 + headerHeight;
		const double listBottom = listTop + rowHeight * static_cast<double>(rows);
		const double hintHeight = 22.0;
		const double hintTop = listBottom + 6.0;
		const double height = hints ? (hintTop + hintHeight + 4.0) : (listBottom + 6.0);

		// Background: the vanilla loot panel's own plate - pure black at half
		// alpha, measured off the SWF placement (black art, alpha mult
		// 128/256). The old navy 0x0A1420 @ 0.55 was the invented colour the
		// whole chrome hunt existed to retire.
		RE::Scaleform::GFx::Value gfx;
		if (!g_panelClip.GetMember("graphics", &gfx)) {
			giveUp("the list container has no 'graphics' member to draw into");
			return;
		}
		V bgFill[]{ V{ static_cast<std::uint32_t>(0x000000) }, V{ 0.50 } };
		gfx.Invoke("beginFill", nullptr, bgFill, 2);
		V bg0[]{ V{ 0.0 }, V{ 0.0 } };
		gfx.Invoke("moveTo", nullptr, bg0, 2);
		V bg1[]{ V{ width }, V{ 0.0 } };
		gfx.Invoke("lineTo", nullptr, bg1, 2);
		V bg2[]{ V{ width }, V{ height } };
		gfx.Invoke("lineTo", nullptr, bg2, 2);
		V bg3[]{ V{ 0.0 }, V{ height } };
		gfx.Invoke("lineTo", nullptr, bg3, 2);
		gfx.Invoke("lineTo", nullptr, bg0, 2);
		gfx.Invoke("endFill", nullptr, nullptr, 0);

		// A hairline between rows. Static, because the rows are at fixed
		// positions - only their contents change.
		if (bPanelRowSeparators.GetValue() && rows > 1) {
			V sepFill[]{ V{ static_cast<std::uint32_t>(0x66CCFF) }, V{ 0.10 } };
			for (std::size_t i = 1; i < rows; ++i) {
				const double y = listTop + rowHeight * static_cast<double>(i);
				gfx.Invoke("beginFill", nullptr, sepFill, 2);
				V s0[]{ V{ 12.0 }, V{ y } };
				gfx.Invoke("moveTo", nullptr, s0, 2);
				V s1[]{ V{ width - 12.0 }, V{ y } };
				gfx.Invoke("lineTo", nullptr, s1, 2);
				V s2[]{ V{ width - 12.0 }, V{ y + 1.0 } };
				gfx.Invoke("lineTo", nullptr, s2, 2);
				V s3[]{ V{ 12.0 }, V{ y + 1.0 } };
				gfx.Invoke("lineTo", nullptr, s3, 2);
				gfx.Invoke("lineTo", nullptr, s0, 2);
				gfx.Invoke("endFill", nullptr, nullptr, 0);
			}
		}

		// The header strip, solid in vanilla's own teal (v0.16.3): the loot
		// panel's header is flat 0x218286 with its title in 0x76C0C4, both
		// measured off the SWF. The strip is the separator, so the old
		// hairline went with it; the title text is created further down
		// with the same builder the hints use.
		if (header) {
			V headFill[]{ V{ uPanelHeaderColor.GetValue() },
				V{ std::clamp(static_cast<double>(fPanelHeaderAlpha.GetValue()), 0.0, 1.0) } };
			gfx.Invoke("beginFill", nullptr, headFill, 2);
			V t0[]{ V{ 0.0 }, V{ 0.0 } };
			gfx.Invoke("moveTo", nullptr, t0, 2);
			V t1[]{ V{ width }, V{ 0.0 } };
			gfx.Invoke("lineTo", nullptr, t1, 2);
			V t2[]{ V{ width }, V{ headerHeight } };
			gfx.Invoke("lineTo", nullptr, t2, 2);
			V t3[]{ V{ 0.0 }, V{ headerHeight } };
			gfx.Invoke("lineTo", nullptr, t3, 2);
			gfx.Invoke("lineTo", nullptr, t0, 2);
			gfx.Invoke("endFill", nullptr, nullptr, 0);
		}

		// One recipe for the footer pills (v0.16.0): the HUD's own filled
		// button driven by a user event, parented to the panel, display-only.
		// The component centres its label+key assembly on its origin.
		const auto makePill = [&](RE::Scaleform::GFx::Value& a_out, const std::string& a_label,
								   const std::string& a_events, double a_x, double a_y) {
			RE::Scaleform::GFx::Value data;
			if (!BuildPillData(root, a_label, a_events, data))
				return false;
			root->CreateObject(&a_out, "BasicButton_Filled");
			if (!(a_out.IsObject() || a_out.IsDisplayObject())) {
				a_out = RE::Scaleform::GFx::Value{};
				return false;
			}
			RE::Scaleform::GFx::Value added;
			if (!g_panelClip.Invoke("addChild", &added, &a_out, 1)) {
				a_out = RE::Scaleform::GFx::Value{};
				return false;
			}
			const double ps = static_cast<double>(fPanelHintPillScale.GetValue());
			a_out.SetMember("x", V{ a_x });
			a_out.SetMember("y", V{ a_y });
			a_out.SetMember("scaleX", V{ ps });
			a_out.SetMember("scaleY", V{ ps });
			a_out.SetMember("mouseEnabled", V{ false });
			a_out.SetMember("mouseChildren", V{ false });
			if (!a_out.Invoke("SetButtonData", nullptr, &data, 1)) {
				a_out.SetMember("visible", V{ false });
				a_out = RE::Scaleform::GFx::Value{};
				return false;
			}
			return true;
		};

		// The browse hint as a pill. On keyboard and mouse its cap can only
		// render the binding NAME - "MOUSEWHEELUP", the component has no
		// wheel art - and that stays, KEPT with eyes open (v0.16.2, the
		// tester's call), as the honest display for whatever someone bound
		// POV cycling to. On a controller the same pill was BLANK, because
		// `ZoomIn` has no pad binding for the component to resolve; v1.1.0
		// dresses it for the device instead, and the D-pad has real art.
		const std::string browse = BrowsePillLabel();
		bool              browsePillOk = false;
		if (hints) {
			// Unknown at build time means keyboard and mouse, which is what
			// the pill has always assumed; RefreshPanel re-dresses it on the
			// first open either way, so a wrong guess here lasts no time.
			const bool pad = UsingGamepad(root, reticlePath).value_or(false);
			browsePillOk = makePill(g_panelBrowsePill, browse, BrowsePillEvents(pad),
				static_cast<double>(fPanelBrowsePillX.GetValue()), hintTop + hintHeight * 0.5);
			if (browsePillOk)
				g_panelBrowsePillDevice.store(pad ? 2 : 1, std::memory_order_release);
			else
				REX::WARN("[panel] browse pill could not be built - the drawn wheel glyph stays");
		}

		// The control hint's symbols are DRAWN, not typed. The font here is
		// borrowed from the HUD and embedded, which means it carries only the
		// glyphs its author included - arrows like U+25B2 would very likely come
		// out as blank boxes. Triangles from the graphics API cannot.
		double hintTextX = 12.0;
		if (hints) {
			// A hairline above the hint, to set it apart from the list.
			V ruleFill[]{ V{ static_cast<std::uint32_t>(0x66CCFF) }, V{ 0.20 } };
			gfx.Invoke("beginFill", nullptr, ruleFill, 2);
			V r0[]{ V{ 10.0 }, V{ listBottom + 2.0 } };
			gfx.Invoke("moveTo", nullptr, r0, 2);
			V r1[]{ V{ width - 10.0 }, V{ listBottom + 2.0 } };
			gfx.Invoke("lineTo", nullptr, r1, 2);
			V r2[]{ V{ width - 10.0 }, V{ listBottom + 3.0 } };
			gfx.Invoke("lineTo", nullptr, r2, 2);
			V r3[]{ V{ 10.0 }, V{ listBottom + 3.0 } };
			gfx.Invoke("lineTo", nullptr, r3, 2);
			gfx.Invoke("lineTo", nullptr, r0, 2);
			gfx.Invoke("endFill", nullptr, nullptr, 0);
		}
		if (hints && !browsePillOk) {
			const double midY = hintTop + hintHeight * 0.5;

			const auto rect = [&](double a_x0, double a_y0, double a_x1, double a_y1,
								   std::uint32_t a_colour, double a_alpha) {
				V fill[]{ V{ a_colour }, V{ a_alpha } };
				gfx.Invoke("beginFill", nullptr, fill, 2);
				V p0[]{ V{ a_x0 }, V{ a_y0 } };
				gfx.Invoke("moveTo", nullptr, p0, 2);
				V p1[]{ V{ a_x1 }, V{ a_y0 } };
				gfx.Invoke("lineTo", nullptr, p1, 2);
				V p2[]{ V{ a_x1 }, V{ a_y1 } };
				gfx.Invoke("lineTo", nullptr, p2, 2);
				V p3[]{ V{ a_x0 }, V{ a_y1 } };
				gfx.Invoke("lineTo", nullptr, p3, 2);
				gfx.Invoke("lineTo", nullptr, p0, 2);
				gfx.Invoke("endFill", nullptr, nullptr, 0);
			};

			// A mouse body with a wheel in it, in the panel's text colour
			// (v0.16.1). The triangles alone were read as generic up/down
			// rather than as a wheel, so the glyph says which device it
			// means and the triangles say which way it turns.
			rect(12.0, midY - 8.0, 23.0, midY + 8.0, uPanelTextColor.GetValue(), 0.30);
			rect(16.5, midY - 5.5, 18.5, midY - 0.5, uPanelTextColor.GetValue(), 0.95);

			// Stacked beside the mouse, so the pair reads as one symbol.
			const auto triangle = [&](double a_cx, double a_cy, bool a_up) {
				V fill[]{ V{ uPanelTextColor.GetValue() }, V{ 0.85 } };
				gfx.Invoke("beginFill", nullptr, fill, 2);
				const double tip = a_up ? a_cy - 3.5 : a_cy + 3.5;
				const double base = a_up ? a_cy + 1.5 : a_cy - 1.5;
				V            p0[]{ V{ a_cx }, V{ tip } };
				gfx.Invoke("moveTo", nullptr, p0, 2);
				V p1[]{ V{ a_cx - 4.0 }, V{ base } };
				gfx.Invoke("lineTo", nullptr, p1, 2);
				V p2[]{ V{ a_cx + 4.0 }, V{ base } };
				gfx.Invoke("lineTo", nullptr, p2, 2);
				gfx.Invoke("lineTo", nullptr, p0, 2);
				gfx.Invoke("endFill", nullptr, nullptr, 0);
			};
			triangle(31.0, midY - 4.0, true);
			triangle(31.0, midY + 4.0, false);

			hintTextX = 41.0;
		}

		// The highlight is its own clip so moving it is one property write per
		// wheel notch rather than a redraw.
		if (g_panelClip.CreateEmptyMovieClip(&g_panelHighlight, "Highlight", 1)) {
			RE::Scaleform::GFx::Value hlGfx;
			if (g_panelHighlight.GetMember("graphics", &hlGfx)) {
				// Vanilla's own selection colour by default (v0.16.1): the
				// loot rows' Selected cxform, flat 0xEFF3DC at ~40%.
				V hlFill[]{ V{ uPanelHighlightColor.GetValue() },
					V{ std::clamp(static_cast<double>(fPanelHighlightAlpha.GetValue()), 0.0, 1.0) } };
				hlGfx.Invoke("beginFill", nullptr, hlFill, 2);
				V h0[]{ V{ 4.0 }, V{ 0.0 } };
				hlGfx.Invoke("moveTo", nullptr, h0, 2);
				V h1[]{ V{ width - 4.0 }, V{ 0.0 } };
				hlGfx.Invoke("lineTo", nullptr, h1, 2);
				V h2[]{ V{ width - 4.0 }, V{ rowHeight } };
				hlGfx.Invoke("lineTo", nullptr, h2, 2);
				V h3[]{ V{ 4.0 }, V{ rowHeight } };
				hlGfx.Invoke("lineTo", nullptr, h3, 2);
				hlGfx.Invoke("lineTo", nullptr, h0, 2);
				hlGfx.Invoke("endFill", nullptr, nullptr, 0);
			}
			g_panelHighlight.SetMember("visible", V{ false });
		} else {
			REX::WARN("[panel] no highlight clip - the list will run without one");
		}

		// The scrollbar (v0.12.0, drawn - the tester's call): a hairline
		// track down the left edge, a brighter thumb whose 1 px art is
		// scaled to length per refresh. Both hidden until the list actually
		// outgrows the panel.
		if (g_panelClip.CreateEmptyMovieClip(&g_panelScrollTrack, "ScrollTrack", 3)) {
			RE::Scaleform::GFx::Value trackGfx;
			if (g_panelScrollTrack.GetMember("graphics", &trackGfx)) {
				V trackFill[]{ V{ static_cast<std::uint32_t>(0x66CCFF) }, V{ 0.15 } };
				trackGfx.Invoke("beginFill", nullptr, trackFill, 2);
				V t0[]{ V{ 4.0 }, V{ listTop + 2.0 } };
				trackGfx.Invoke("moveTo", nullptr, t0, 2);
				V t1[]{ V{ 6.0 }, V{ listTop + 2.0 } };
				trackGfx.Invoke("lineTo", nullptr, t1, 2);
				V t2[]{ V{ 6.0 }, V{ listBottom - 2.0 } };
				trackGfx.Invoke("lineTo", nullptr, t2, 2);
				V t3[]{ V{ 4.0 }, V{ listBottom - 2.0 } };
				trackGfx.Invoke("lineTo", nullptr, t3, 2);
				trackGfx.Invoke("lineTo", nullptr, t0, 2);
				trackGfx.Invoke("endFill", nullptr, nullptr, 0);
			}
			g_panelScrollTrack.SetMember("visible", V{ false });
		}
		if (g_panelClip.CreateEmptyMovieClip(&g_panelScrollThumb, "ScrollThumb", 4)) {
			RE::Scaleform::GFx::Value thumbGfx;
			if (g_panelScrollThumb.GetMember("graphics", &thumbGfx)) {
				// 1 px tall at origin: scaleY becomes the thumb's length.
				V thumbFill[]{ V{ static_cast<std::uint32_t>(0x99D6FF) }, V{ 0.70 } };
				thumbGfx.Invoke("beginFill", nullptr, thumbFill, 2);
				V h0[]{ V{ 3.5 }, V{ 0.0 } };
				thumbGfx.Invoke("moveTo", nullptr, h0, 2);
				V h1[]{ V{ 6.5 }, V{ 0.0 } };
				thumbGfx.Invoke("lineTo", nullptr, h1, 2);
				V h2[]{ V{ 6.5 }, V{ 1.0 } };
				thumbGfx.Invoke("lineTo", nullptr, h2, 2);
				V h3[]{ V{ 3.5 }, V{ 1.0 } };
				thumbGfx.Invoke("lineTo", nullptr, h3, 2);
				thumbGfx.Invoke("lineTo", nullptr, h0, 2);
				thumbGfx.Invoke("endFill", nullptr, nullptr, 0);
			}
			g_panelScrollThumb.SetMember("visible", V{ false });
		}

		const bool haveFormat = BorrowTextFormat(root, rootPath, g_panelFormat, "[panel]");
		if (haveFormat) {
			g_panelFormat.SetMember("size", V{ 17.0 });
			g_panelFormat.SetMember("bold", V{ false });
			// The pills' own label grey (v0.16.0) - every text in the panel
			// wears it now except the header's cyan.
			g_panelFormat.SetMember("color", V{ uPanelTextColor.GetValue() });
			// A borrowed format brings the DONOR's alignment with it, and the
			// donor here is the HUD's centred lock-on caption. Without this the
			// names sit centred in their column, which is what v0.3.2 shipped.
			g_panelFormat.SetMember("align", V{ "left" });
		} else {
			REX::WARN("[panel] no donor TextField found - rows will likely render as boxes");
		}

		// The distance column is the same borrowed format with align="right", on
		// its own object so setting that does not right-align the names too.
		const bool haveDistFormat = BorrowTextFormat(root, rootPath, g_panelDistFormat, "[panel-dist]");
		if (haveDistFormat) {
			g_panelDistFormat.SetMember("size", V{ 17.0 });
			g_panelDistFormat.SetMember("bold", V{ false });
			g_panelDistFormat.SetMember("color", V{ uPanelTextColor.GetValue() });
			g_panelDistFormat.SetMember("align", V{ "right" });
		}

		constexpr double kNamePad = 10.0;
		constexpr double kDistWidth = 96.0;
		const double     iconColumn = bPanelIcons.GetValue() ? 20.0 : 0.0;
		const double     nameWidth =
			std::max(40.0, width - kNamePad * 2.0 - kDistWidth - 6.0 - iconColumn);
		// The refresh adjusts moon rows' field width alongside their indent,
		// so the vanilla truncation measures against the real edge.
		g_panelNameWidth.store(nameWidth, std::memory_order_release);

		std::size_t made = 0;
		for (std::size_t i = 0; i < rows; ++i) {
			const double rowY = listTop + rowHeight * static_cast<double>(i);

			const auto makeField = [&](RE::Scaleform::GFx::Value& a_field, double a_x, double a_w,
									   bool a_useDist) {
				root->CreateObject(&a_field, "flash.text.TextField");
				if (!a_field.IsObject() && !a_field.IsDisplayObject())
					return false;

				a_field.SetMember("selectable", V{ false });
				a_field.SetMember("mouseEnabled", V{ false });
				a_field.SetMember("multiline", V{ false });
				a_field.SetMember("width", V{ a_w });
				a_field.SetMember("height", V{ rowHeight });
				a_field.SetMember("x", V{ a_x });
				a_field.SetMember("y", V{ rowY });

				const bool have = a_useDist ? haveDistFormat : haveFormat;
				if (have) {
					a_field.SetMember("embedFonts", V{ true });
					a_field.SetMember("defaultTextFormat", a_useDist ? g_panelDistFormat : g_panelFormat);
				} else {
					a_field.SetMember("textColor", V{ uPanelTextColor.GetValue() });
				}

				RE::Scaleform::GFx::Value added;
				if (!g_panelClip.Invoke("addChild", &added, &a_field, 1))
					return false;
				a_field.SetMember("visible", V{ false });
				return true;
			};

			// The survey marks go in BEFORE the text fields, so that whatever
			// orders the panel's script-added children puts them underneath the
			// distance number rather than over it. Creation order is the only
			// lever there is here - relative z among script-added siblings has
			// never been proven in this project, only assumed - which is why
			// fPanelSurveyBannerWidth exists as the escape hatch: shrink the
			// banner off the number and the question stops mattering.
			const double distX = width - kNamePad - kDistWidth;
			if (bPanelSurveyMarks.GetValue()) {
				// Clamped at BOTH ends: a bar taller than the row would be drawn
				// at a negative y offset, i.e. into the row above (or into the
				// header, for row 0).
				const double barH = std::clamp(
					static_cast<double>(fPanelSurveyBarHeight.GetValue()), 1.0,
					std::max(1.0, rowHeight - 4.0));
				const double bannerW = kDistWidth *
				                       std::clamp(static_cast<double>(fPanelSurveyBannerWidth.GetValue()), 0.1, 1.0);

				// The completed banner: vanilla's plate with the four bands over
				// it, anchored left exactly as sprite 35 draws them. Static art -
				// built once, only `visible` moves afterwards.
				if (g_panelClip.CreateEmptyMovieClip(&g_panelSurveyBanners[i],
						std::format("SurveyBanner{}", i).c_str(),
						static_cast<std::uint32_t>(10 + i))) {
					RE::Scaleform::GFx::Value gfxB;
					if (g_panelSurveyBanners[i].GetMember("graphics", &gfxB)) {
						const double top = rowY + 1.0;
						const double bot = rowY + rowHeight - 1.0;
						const auto   quad = [&](double a_x0Top, double a_x1Top, double a_x0Bot,
									  double a_x1Bot, std::uint32_t a_colour, double a_alpha) {
                            V fill[]{ V{ a_colour }, V{ a_alpha } };
                            gfxB.Invoke("beginFill", nullptr, fill, 2);
                            V p0[]{ V{ a_x0Top }, V{ top } };
                            gfxB.Invoke("moveTo", nullptr, p0, 2);
                            V p1[]{ V{ a_x1Top }, V{ top } };
                            gfxB.Invoke("lineTo", nullptr, p1, 2);
                            V p2[]{ V{ a_x1Bot }, V{ bot } };
                            gfxB.Invoke("lineTo", nullptr, p2, 2);
                            V p3[]{ V{ a_x0Bot }, V{ bot } };
                            gfxB.Invoke("lineTo", nullptr, p3, 2);
                            gfxB.Invoke("lineTo", nullptr, p0, 2);
                            gfxB.Invoke("endFill", nullptr, nullptr, 0);
						};

						const double plateAlpha =
							std::clamp(static_cast<double>(fPanelSurveyPlateAlpha.GetValue()), 0.0, 1.0);
						quad(distX, distX + bannerW, distX, distX + bannerW,
							uPanelSurveyPlate.GetValue(), plateAlpha);

						// Four nested bands, each starting at the left edge and
						// leaning right, drawn gold -> orange -> crimson -> navy
						// so the last is on top and widest - which is the order
						// and the outcome measured off the vanilla shape.
						const std::uint32_t bands[]{ uPanelSurveyBand1.GetValue(),
							uPanelSurveyBand2.GetValue(), uPanelSurveyBand3.GetValue(),
							uPanelSurveyBand4.GetValue() };
						// Widths as fractions of the banner, mirroring the
						// vanilla areas (navy widest, gold narrowest).
						const double widths[]{ 0.62, 0.50, 0.37, 0.24 };
						// ⚠ The lean must not exceed the NARROWEST band, or that
						// band's bottom edge crosses its own left edge: the quad
						// self-intersects and paints into the 6 px gutter beside
						// the name field. At the ini's own recommended
						// fPanelSurveyBannerWidth=0.5 a flat 0.55*rowHeight lean
						// did exactly that. Scaled here AND clamped per band,
						// because the width is user-editable and a knob that can
						// draw outside its own cell eventually will.
						const double lean = std::min(rowHeight * 0.55, bannerW * widths[3]);
						for (int b = 0; b < 4; ++b) {
							const double w = bannerW * widths[b];
							const double xBot = std::max(distX, distX + w - lean);
							quad(distX, distX + w, distX, xBot, bands[b], plateAlpha);
						}
					}
					g_panelSurveyBanners[i].SetMember("visible", V{ false });
				}

				// The progress bar's TRACK, full width along the cell's bottom
				// edge. Its own graphics render below its children, so the fill
				// child sits over it without any z-order question at all.
				if (g_panelClip.CreateEmptyMovieClip(&g_panelSurveyBars[i],
						std::format("SurveyBar{}", i).c_str(),
						static_cast<std::uint32_t>(40 + i))) {
					RE::Scaleform::GFx::Value gfxT;
					if (g_panelSurveyBars[i].GetMember("graphics", &gfxT)) {
						const double y0 = rowY + rowHeight - barH - 2.0;
						V fill[]{ V{ uPanelSurveyBarTrack.GetValue() },
							V{ std::clamp(static_cast<double>(fPanelSurveyBarTrackAlpha.GetValue()), 0.0, 1.0) } };
						gfxT.Invoke("beginFill", nullptr, fill, 2);
						V p0[]{ V{ distX }, V{ y0 } };
						gfxT.Invoke("moveTo", nullptr, p0, 2);
						V p1[]{ V{ distX + kDistWidth }, V{ y0 } };
						gfxT.Invoke("lineTo", nullptr, p1, 2);
						V p2[]{ V{ distX + kDistWidth }, V{ y0 + barH } };
						gfxT.Invoke("lineTo", nullptr, p2, 2);
						V p3[]{ V{ distX }, V{ y0 + barH } };
						gfxT.Invoke("lineTo", nullptr, p3, 2);
						gfxT.Invoke("lineTo", nullptr, p0, 2);
						gfxT.Invoke("endFill", nullptr, nullptr, 0);
					}
					// The fill: full-width art whose scaleX becomes the
					// percentage. Drawn from x=0 in the child's own space and
					// the child placed at the cell's left edge, so scaling grows
					// it rightwards from there - the scrollbar's thumb trick,
					// turned on its side.
					if (g_panelSurveyBars[i].CreateEmptyMovieClip(&g_panelSurveyFills[i],
							"Fill", 1)) {
						RE::Scaleform::GFx::Value gfxF;
						if (g_panelSurveyFills[i].GetMember("graphics", &gfxF)) {
							const double y0 = rowY + rowHeight - barH - 2.0;
							V fill[]{ V{ uPanelSurveyBarFill.GetValue() },
								V{ std::clamp(static_cast<double>(fPanelSurveyBarAlpha.GetValue()), 0.0, 1.0) } };
							gfxF.Invoke("beginFill", nullptr, fill, 2);
							V p0[]{ V{ 0.0 }, V{ y0 } };
							gfxF.Invoke("moveTo", nullptr, p0, 2);
							V p1[]{ V{ kDistWidth }, V{ y0 } };
							gfxF.Invoke("lineTo", nullptr, p1, 2);
							V p2[]{ V{ kDistWidth }, V{ y0 + barH } };
							gfxF.Invoke("lineTo", nullptr, p2, 2);
							V p3[]{ V{ 0.0 }, V{ y0 + barH } };
							gfxF.Invoke("lineTo", nullptr, p3, 2);
							gfxF.Invoke("lineTo", nullptr, p0, 2);
							gfxF.Invoke("endFill", nullptr, nullptr, 0);
						}
						g_panelSurveyFills[i].SetMember("x", V{ distX });
					}
					g_panelSurveyBars[i].SetMember("visible", V{ false });
				}
			}
			g_panelSurveyDrawn[i] = kSurveyNeverDrawn;

			if (!makeField(g_panelRows[i], kNamePad + iconColumn, nameWidth, false))
				break;
			if (!makeField(g_panelDists[i], distX, kDistWidth, true))
				break;

			// The icon clip sits in its own column and is drawn into later, when
			// the row is known. Its origin is the middle of the row so the
			// glyphs can be written symmetrically about (0, 0).
			if (iconColumn > 0.0 &&
				g_panelClip.CreateEmptyMovieClip(&g_panelIcons[i], std::format("Icon{}", i).c_str(),
					static_cast<std::uint32_t>(100 + i))) {
				g_panelIcons[i].SetMember("x", V{ kNamePad + iconColumn * 0.5 });
				g_panelIcons[i].SetMember("y", V{ rowY + rowHeight * 0.5 });
				g_panelIcons[i].SetMember("visible", V{ false });
			}
			g_panelIconClass[i] = PlanetClass::kUnknown;
			g_panelIconSettled[i] = false;
			g_panelIconDrawn[i] = false;

			// The vanilla icon shares the column and they swap per row:
			// whichever fits the body shows, the other hides. The class name
			// is package-qualified (like BSUIDataManager); its first
			// construction also brings up vanilla's own shared MapIcons
			// loader - normally long since loaded by the HUD's own markers,
			// and the icon self-completes off the load event if not. The
			// symbols are authored centred on their origin (measured from
			// the SWF), so the drawn glyphs' anchor is the right one as is.
			if (iconColumn > 0.0 && bPanelVanillaIcons.GetValue() &&
				!g_panelPoiIconsFailed.load(std::memory_order_acquire)) {
				root->CreateObject(&g_panelPoiIcons[i], "Components.Icons.DynamicPoiIcon");
				if (g_panelPoiIcons[i].IsObject() || g_panelPoiIcons[i].IsDisplayObject()) {
					RE::Scaleform::GFx::Value poiAdded;
					if (g_panelClip.Invoke("addChild", &poiAdded, &g_panelPoiIcons[i], 1)) {
						g_panelPoiIcons[i].SetMember("x", V{ kNamePad + iconColumn * 0.5 });
						g_panelPoiIcons[i].SetMember("y", V{ rowY + rowHeight * 0.5 });
						g_panelPoiIcons[i].SetMember("visible", V{ false });
						V scale{ static_cast<double>(fPanelVanillaIconScale.GetValue()) };
						g_panelPoiIcons[i].Invoke("SetMarkerScale", nullptr, &scale, 1);
					} else {
						g_panelPoiIcons[i] = RE::Scaleform::GFx::Value{};
					}
				} else {
					g_panelPoiIcons[i] = RE::Scaleform::GFx::Value{};
					if (!g_panelPoiIconsFailed.exchange(true, std::memory_order_acq_rel))
						REX::WARN("[icons] Components.Icons.DynamicPoiIcon did not construct - "
								  "the drawn glyphs stay for everything");
				}
			}
			g_panelPoiIconKey[i] = 0;

			// The giants' icon: the in-POV marker's own circle (a 3-state
			// symbol whose every frame stops; it parks on the plain ring)
			// with the ring-line drawn across it in a child clip - script-
			// added children render ABOVE the timeline art, so the line
			// sits over the circle. Both centred on origin (measured
			// ±7.4 px), so the column's anchor fits unchanged.
			if (iconColumn > 0.0 && bPanelVanillaIcons.GetValue() &&
				!g_panelGiantIconsFailed.load(std::memory_order_acquire)) {
				root->CreateObject(&g_panelGiantIcons[i], "ShipReticle_fla.PlanetIconCircle_37");
				bool giantOk = false;
				if (g_panelGiantIcons[i].IsObject() || g_panelGiantIcons[i].IsDisplayObject()) {
					RE::Scaleform::GFx::Value giantAdded;
					if (g_panelClip.Invoke("addChild", &giantAdded, &g_panelGiantIcons[i], 1)) {
						RE::Scaleform::GFx::Value ringLine;
						if (g_panelGiantIcons[i].CreateEmptyMovieClip(&ringLine, "NavRingLine", 10)) {
							RE::Scaleform::GFx::Value lineGfx;
							if (ringLine.GetMember("graphics", &lineGfx)) {
								// Local units: the instance scale drives the
								// line with the circle. Wider than the ring
								// (±9.5 vs ±7.4) so the tips read as rings
								// seen edge-on, not as a strike-through.
								V lineFill[]{ V{ static_cast<std::uint32_t>(0xFFFFFF) }, V{ 0.85 } };
								lineGfx.Invoke("beginFill", nullptr, lineFill, 2);
								V p0[]{ V{ -9.5 }, V{ -0.8 } };
								lineGfx.Invoke("moveTo", nullptr, p0, 2);
								V p1[]{ V{ 9.5 }, V{ -0.8 } };
								lineGfx.Invoke("lineTo", nullptr, p1, 2);
								V p2[]{ V{ 9.5 }, V{ 0.8 } };
								lineGfx.Invoke("lineTo", nullptr, p2, 2);
								V p3[]{ V{ -9.5 }, V{ 0.8 } };
								lineGfx.Invoke("lineTo", nullptr, p3, 2);
								lineGfx.Invoke("lineTo", nullptr, p0, 2);
								lineGfx.Invoke("endFill", nullptr, nullptr, 0);
								giantOk = true;
							}
						}
						if (giantOk) {
							const double gs = static_cast<double>(fPanelGiantIconScale.GetValue());
							g_panelGiantIcons[i].SetMember("x", V{ kNamePad + iconColumn * 0.5 });
							g_panelGiantIcons[i].SetMember("y", V{ rowY + rowHeight * 0.5 });
							g_panelGiantIcons[i].SetMember("scaleX", V{ gs });
							g_panelGiantIcons[i].SetMember("scaleY", V{ gs });
							g_panelGiantIcons[i].SetMember("visible", V{ false });
						}
					}
				}
				if (!giantOk) {
					g_panelGiantIcons[i] = RE::Scaleform::GFx::Value{};
					if (!g_panelGiantIconsFailed.exchange(true, std::memory_order_acq_rel))
						REX::WARN("[icons] PlanetIconCircle did not construct - the giants "
								  "keep the drawn ring");
				}
			}

			++made;
		}

		if (made == 0) {
			giveUp("could not create a single row TextField");
			return;
		}
		if (made < rows)
			REX::WARN("[panel] only {} of {} rows could be created", made, rows);

		// Hint and title text are static, written once here rather than on
		// every refresh - one builder for all three fields.
		const auto makeHint = [&](RE::Scaleform::GFx::Value& a_field, RE::Scaleform::GFx::Value& a_format,
								   double a_x, double a_w, const char* a_align, const std::string& a_text,
								   const char* a_tag, double a_y, std::uint32_t a_colour, double a_size) {
			root->CreateObject(&a_field, "flash.text.TextField");
			if (!a_field.IsObject() && !a_field.IsDisplayObject()) {
				REX::WARN("[panel] could not create the {} hint field", a_tag);
				return;
			}

			a_field.SetMember("selectable", V{ false });
			a_field.SetMember("mouseEnabled", V{ false });
			a_field.SetMember("multiline", V{ false });
			a_field.SetMember("width", V{ a_w });
			a_field.SetMember("height", V{ hintHeight });
			a_field.SetMember("x", V{ a_x });
			a_field.SetMember("y", V{ a_y });

			if (BorrowTextFormat(root, rootPath, a_format, "[panel-hint]")) {
				a_format.SetMember("size", V{ a_size });
				a_format.SetMember("bold", V{ false });
				a_format.SetMember("color", V{ a_colour });
				a_format.SetMember("align", V{ a_align });
				a_field.SetMember("embedFonts", V{ true });
				a_field.SetMember("defaultTextFormat", a_format);
			} else {
				a_field.SetMember("textColor", V{ a_colour });
			}

			a_field.SetMember("text", V{ a_text.c_str() });
			if (a_format.IsObject())
				a_field.Invoke("setTextFormat", nullptr, &a_format, 1);

			RE::Scaleform::GFx::Value added;
			if (!g_panelClip.Invoke("addChild", &added, &a_field, 1))
				REX::WARN("[panel] {} hint addChild failed", a_tag);
		};

		if (hints) {
			const double rightWidth = std::min(150.0, std::max(90.0, width * 0.36));
			const double leftWidth = std::max(60.0, width - hintTextX - rightWidth - 14.0);

			if (!browsePillOk)
				makeHint(g_panelHint, g_panelHintFormat, hintTextX, leftWidth, "left",
					browse, "left", hintTop + 2.0, uPanelTextColor.GetValue(), 14.0);

			// The confirm hint: the same pill, driven by the first NAMED
			// entry of sConfirmEvent - the key cap shows the player's real
			// binding, which the old drawn "Q lock / clear" could never
			// promise. An all-#id config has no name for the cap to
			// resolve, so the drawn text (with sConfirmKeyLabel, now
			// fallback-only) returns for it.
			bool        confirmPillOk = false;
			std::string confirmEventName;
			{
				const std::string list = sConfirmEvent.GetValue();
				std::size_t       p = 0;
				while (p <= list.size()) {
					const auto  c = list.find(',', p);
					std::string entry =
						list.substr(p, c == std::string::npos ? std::string::npos : c - p);
					while (!entry.empty() && entry.front() == ' ')
						entry.erase(0, 1);
					while (!entry.empty() && entry.back() == ' ')
						entry.pop_back();
					if (!entry.empty() && entry.front() != '#') {
						confirmEventName = entry;
						break;
					}
					if (c == std::string::npos)
						break;
					p = c + 1;
				}
			}
			if (!confirmEventName.empty()) {
				confirmPillOk = makePill(g_panelConfirmPill, sPanelConfirmLabel.GetValue(),
					confirmEventName,
					width - static_cast<double>(fPanelConfirmPillRightPad.GetValue()),
					hintTop + hintHeight * 0.5);
				if (!confirmPillOk)
					REX::WARN("[panel] confirm pill could not be built - the drawn hint text stays");
			}
			if (!confirmPillOk)
				makeHint(g_panelHintRight, g_panelHintRightFormat, width - rightWidth - 10.0,
					rightWidth, "right",
					std::format("{}  lock / clear", sConfirmKeyLabel.GetValue()), "right",
					hintTop + 2.0, uPanelTextColor.GetValue(), 14.0);
		}

		// The title, in vanilla's own header text colour and near-vanilla
		// metrics (their strip is 31 px with 18 px text at (13, 7); ours is
		// 30 px).
		if (header)
			makeHint(g_panelTitle, g_panelTitleFormat, 13.0, width - 26.0, "left",
				title, "title", 5.0, uPanelTitleColor.GetValue(), 17.0);

		// The scanner-key hint (v0.14.0): vanilla's own button component,
		// driven with the scanner's user event so the key cap shows the
		// player's real binding and follows rebinds and input-device swaps
		// by itself (its KeyHelper is built in its own ctor). Data classes
		// are package-qualified; the button symbol is default-package in
		// the reticle's SWF - the OffScreenIcon distance. SetButtonData
		// stashes data until the instance is ON STAGE, so the order is
		// addChild first, data second. Display-only: no callback in the
		// event data, and the mouse is switched off outright.
		if (bScannerHint.GetValue() && !g_scannerHintFailed.load(std::memory_order_acquire)) {
			bool                      hintOk = false;
			RE::Scaleform::GFx::Value ued;
			{
				V eventName;
				root->CreateString(&eventName, "SHMonocle");
				root->CreateObject(&ued,
					"Shared.Components.ButtonControls.ButtonData.UserEventData", &eventName, 1);
			}
			RE::Scaleform::GFx::Value data;
			if (ued.IsObject()) {
				V dataArgs[2];
				root->CreateString(&dataArgs[0], sScannerHintLabel.GetValue().c_str());
				dataArgs[1] = ued;
				root->CreateObject(&data,
					"Shared.Components.ButtonControls.ButtonData.ButtonBaseData", dataArgs, 2);
			}
			if (data.IsObject()) {
				root->CreateObject(&g_scannerHint, "BasicButton_Filled");
				if (g_scannerHint.IsObject() || g_scannerHint.IsDisplayObject()) {
					RE::Scaleform::GFx::Value hintAdded;
					if (reticle.Invoke("addChild", &hintAdded, &g_scannerHint, 1)) {
						g_scannerHint.SetMember("name", V{ "ShipNavPanelScannerHint" });
						g_scannerHint.SetMember("x",
							V{ static_cast<double>(fScannerHintOffsetX.GetValue()) });
						g_scannerHint.SetMember("y",
							V{ static_cast<double>(fScannerHintOffsetY.GetValue()) });
						const double hs = static_cast<double>(fScannerHintScale.GetValue());
						g_scannerHint.SetMember("scaleX", V{ hs });
						g_scannerHint.SetMember("scaleY", V{ hs });
						g_scannerHint.SetMember("mouseEnabled", V{ false });
						g_scannerHint.SetMember("mouseChildren", V{ false });
						g_scannerHint.SetMember("visible", V{ false });
						hintOk = g_scannerHint.Invoke("SetButtonData", nullptr, &data, 1);
					}
				}
			}
			if (!hintOk) {
				g_scannerHint = RE::Scaleform::GFx::Value{};
				if (!g_scannerHintFailed.exchange(true, std::memory_order_acq_rel))
					REX::WARN("[panel] scanner hint could not be built - the HUD shows none");
			} else {
				REX::INFO("[panel] scanner hint ready - vanilla pill at ({}, {}), label '{}'",
					fScannerHintOffsetX.GetValue(), fScannerHintOffsetY.GetValue(),
					sScannerHintLabel.GetValue());
			}
		}

		g_panelClip.SetMember("x", V{ static_cast<double>(fPanelOffsetX.GetValue()) });
		g_panelClip.SetMember("y", V{ static_cast<double>(fPanelOffsetY.GetValue()) });
		g_panelClip.SetMember("visible", V{ false });

		// The cockpit tilt (v0.16.4): the same Matrix3D treatment vanilla
		// gives its quick container, rebuilt through the engine's own
		// appendRotation - pitch about X first, yaw about Y second,
		// vanilla's composition order - with the yaw mirrored for the left
		// side. Assigning matrix3D zeroes the translation, so the offsets
		// are re-asserted right after (and the animation keeps writing
		// x/y/scale into the 3D matrix without touching the rotation).
		if (bPanelTilt.GetValue()) {
			bool                      tilted = false;
			RE::Scaleform::GFx::Value m3d;
			root->CreateObject(&m3d, "flash.geom.Matrix3D");
			if (m3d.IsObject()) {
				const auto rotate = [&](double a_deg, double a_x, double a_y, double a_z) {
					RE::Scaleform::GFx::Value axis;
					V                         axisArgs[]{ V{ a_x }, V{ a_y }, V{ a_z } };
					root->CreateObject(&axis, "flash.geom.Vector3D", axisArgs, 3);
					if (!axis.IsObject())
						return false;
					V rotArgs[2];
					rotArgs[0] = V{ a_deg };
					rotArgs[1] = axis;
					return m3d.Invoke("appendRotation", nullptr, rotArgs, 2);
				};
				if (rotate(static_cast<double>(fPanelTiltPitch.GetValue()), 1.0, 0.0, 0.0) &&
					rotate(static_cast<double>(fPanelTiltYaw.GetValue()), 0.0, 1.0, 0.0)) {
					RE::Scaleform::GFx::Value transform;
					if (g_panelClip.GetMember("transform", &transform) && transform.IsObject() &&
						transform.SetMember("matrix3D", m3d))
						tilted = true;
				}
			}
			if (tilted) {
				g_panelClip.SetMember("x", V{ static_cast<double>(fPanelOffsetX.GetValue()) });
				g_panelClip.SetMember("y", V{ static_cast<double>(fPanelOffsetY.GetValue()) });
				REX::INFO("[panel] cockpit tilt applied (pitch {}, yaw {})",
					fPanelTiltPitch.GetValue(), fPanelTiltYaw.GetValue());
			} else {
				REX::WARN("[panel] cockpit tilt could not be applied - the panel stays flat");
			}
		}

		g_panelListTop.store(listTop, std::memory_order_release);
		g_panelHeight.store(height, std::memory_order_release);
		g_panelRowCount.store(static_cast<std::uint32_t>(made), std::memory_order_release);
		g_panelReady.store(true, std::memory_order_release);
		REX::INFO("[panel] ready - {} rows at ({}, {})", made,
			fPanelOffsetX.GetValue(), fPanelOffsetY.GetValue());
	}

	// Phase 4 chrome probe (PHASE4-CHROME-HUNT.md): construct the chosen donor -
	// the ship HUD's own loot panel - and drive it with hardcoded rows, so one
	// cruise answers whether the whole-panel route works. Every step logs before
	// entering the VM, so a log that ends mid-probe names the frozen call, and
	// every failure gives up cleanly without touching the drawn panel.
	//
	// The class also exists art-less in spaceshiphudmenu.swf's own ABC; the
	// art-bound copy lives in the IMPORTED ShipHudQuickContainer.swf. That
	// ambiguity is already resolved in the mod's favour by precedent:
	// OffScreenIcon has the same split (art in imported shipreticle.swf) and
	// CreateObject built the art-bound one, confirmed in game (v0.8.2).
	void TryCreateChromeProbe()
	{
		if (!bProbeVanillaChrome.GetValue() ||
			g_chromeProbeReady.load(std::memory_order_acquire) ||
			g_chromeProbeFailed.load(std::memory_order_acquire))
			return;

		// Constructs through the AS3 VM - serialised like every other builder.
		const SingleWinner winner{ g_chromeProbeBuildInFlight };
		if (!winner.Won())
			return;
		if (g_chromeProbeReady.load(std::memory_order_acquire))
			return;

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud))
			return;
		const auto menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return;

		auto*             root = menu->uiMovie->asMovieRoot.get();
		const char*       rootPath = menu->GetRootPath();
		const std::string reticlePath = std::string{ rootPath ? rootPath : "root" } + ".Reticle_mc";

		RE::Scaleform::GFx::Value reticle;
		if (!root->GetVariable(&reticle, reticlePath.c_str()))
			return;

		using V = RE::Scaleform::GFx::Value;

		const auto giveUp = [&](const char* a_why) {
			REX::WARN("[chrome] probe not created ({}) - the drawn panel is unaffected", a_why);
			g_chromeProbe = RE::Scaleform::GFx::Value{};
			g_chromeProbeList = RE::Scaleform::GFx::Value{};
			g_chromeProbeFailed.store(true, std::memory_order_release);
		};

		// Step 1 of the checklist: does the whole-panel ctor survive? A log that
		// ENDS here names the frozen call.
		REX::INFO("[chrome] constructing ShipHudQuickContainer - entering the VM");
		root->CreateObject(&g_chromeProbe, "ShipHudQuickContainer");
		if (!g_chromeProbe.IsDisplayObject() && !g_chromeProbe.IsObject()) {
			giveUp("class did not construct");
			return;
		}
		REX::INFO("[chrome] constructed - censusing children");

		// The census answers "did the art come with it" child by child, and is
		// worth having in the log even if a later step fails.
		const auto child = [&](const char* a_name, V& a_out) {
			const bool have = g_chromeProbe.GetMember(a_name, &a_out) &&
			                  (a_out.IsObject() || a_out.IsDisplayObject());
			REX::INFO("[chrome]   {} {}", a_name, have ? "present" : "MISSING");
			return have;
		};
		V header, invData, buttonBar, titleField;
		const bool haveList = child("List_mc", g_chromeProbeList);
		const bool haveHeader = child("Header_mc", header);
		const bool haveTitle = child("TargetName_tf", titleField);
		const bool haveInv = child("PlayerInvData_mc", invData);
		const bool haveBar = child("ButtonBar_mc", buttonBar);
		if (!haveList) {
			giveUp("no List_mc - nothing to drive");
			return;
		}

		// On the reticle like the drawn panel, so the offsets mean the same
		// thing. Named so no holder loop can ever mistake it for a vanilla blip;
		// invisible before anything else can render a half-configured panel.
		RE::Scaleform::GFx::Value added;
		if (!reticle.Invoke("addChild", &added, &g_chromeProbe, 1)) {
			giveUp("addChild rejected it");
			return;
		}
		g_chromeProbe.SetMember("visible", V{ false });
		g_chromeProbe.SetMember("name", V{ "ShipNavPanelChromeProbe" });
		g_chromeProbe.SetMember("x", V{ static_cast<double>(fProbeChromeOffsetX.GetValue()) });
		g_chromeProbe.SetMember("y", V{ static_cast<double>(fProbeChromeOffsetY.GetValue()) });

		// The safety pin: disableInput gates every mouse and keyboard path in
		// BSScrollingContainer while leaving MoveSelection/selectedIndex free,
		// so the mod stays the only driver. Configure() already ran in the ctor
		// and is once-only; this public setter is the supported post-hoc path.
		if (!g_chromeProbeList.SetMember("disableInput", V{ true }))
			REX::WARN("[chrome] disableInput did not set - mouse hover may fight the wheel");

		// Loot-specific chrome off for the probe: capacity meter and the
		// TAKE/TRANSFER button bar. (The bar could carry the mod's own key hints
		// one day - not this build.)
		if (haveInv)
			invData.SetMember("visible", V{ false });
		if (haveBar)
			buttonBar.SetMember("visible", V{ false });

		// The header title, through vanilla's own SetText path (it also resolves
		// $-tokens, which a plain text write would not).
		if (haveTitle) {
			RE::Scaleform::GFx::Value globalFunc;
			if (root->GetVariable(&globalFunc, "Shared.GlobalFunc") &&
				(globalFunc.IsObject() || globalFunc.IsDisplayObject())) {
				V args[2];
				args[0] = titleField;
				root->CreateString(&args[1], "NAV PROBE");
				if (!globalFunc.Invoke("SetText", nullptr, args, 2))
					titleField.SetMember("text", V{ "NAV PROBE" });
			} else {
				titleField.SetMember("text", V{ "NAV PROBE" });
			}
		}

		// Four rows, hardcoded: the probe tests chrome, not data. Names are
		// pre-capped the way production will cap them (the entry recomputes
		// its own character budget every update, so feeding short strings
		// beats fighting it) - the fourth stays long enough to show the cap
		// AND to prove it clears the new distance column. Icon classes give
		// the column something to draw where the drawn panel would: only the
		// exceptions get a glyph, so two rows carry one and two are bare.
		// Every field SetEntryText reads is stated explicitly; fWeight/uCount
		// also feed the selection-change listener the ctor registered
		// (capacity maths on a hidden meter).
		struct ProbeRow
		{
			const char* name;
			const char* dist;
			PlanetClass cls;
			bool        settled;
		};
		static constexpr ProbeRow kProbeRows[kChromeProbeRows] = {
			{ "Jemison", "1.2 LS", PlanetClass::kRock, true },
			{ "Olivas", "870 km", PlanetClass::kGasGiant, false },
			{ "Kurtz", "-", PlanetClass::kBarren, false },
			{ "Undiscovered Deep Space Manu...", "...", PlanetClass::kUnknown, false },
		};
		V items;
		root->CreateArray(&items);
		for (const auto& probeRow : kProbeRows) {
			V row;
			root->CreateObject(&row);
			if (!row.IsObject()) {
				giveUp("row object did not construct");
				return;
			}
			V nameVal;
			root->CreateString(&nameVal, probeRow.name);
			row.SetMember("sName", nameVal);
			row.SetMember("uCount", V{ static_cast<std::uint32_t>(1) });
			row.SetMember("uRarity", V{ static_cast<std::uint32_t>(0) });
			row.SetMember("bContraband", V{ false });
			row.SetMember("bStolen", V{ false });
			row.SetMember("bIsTagged", V{ false });
			row.SetMember("fWeight", V{ 0.0 });
			row.SetMember("uHandleID", V{ static_cast<std::uint32_t>(0) });
			items.PushBack(row);
		}
		V payload;
		root->CreateObject(&payload);
		payload.SetMember("aItems", items);

		REX::INFO("[chrome] OnItemsChanged with {} rows - entering the VM", kChromeProbeRows);
		if (!g_chromeProbe.Invoke("OnItemsChanged", nullptr, &payload, 1)) {
			giveUp("OnItemsChanged failed - likely the art-less class copy won the domain");
			return;
		}

		// Read back what the list built. Row clips are lazily created by the
		// container's own Update, so their existence IS the verdict on the
		// entry class binding; currentLabel says which frame art each row wears
		// (row 0 should sit on its Selected label).
		V count;
		const auto clips = g_chromeProbeList.GetMember("totalEntryClips", &count) ?
		                       static_cast<int>(AsNumber(count)) :
		                       -1;
		V selVal;
		const auto sel = g_chromeProbeList.GetMember("selectedIndex", &selVal) ?
		                     static_cast<int>(AsNumber(selVal)) :
		                     -99;
		REX::INFO("[chrome] list built: {} entry clips, selectedIndex {}", clips, sel);
		for (int i = 0; i < clips && i < static_cast<int>(kChromeProbeRows); ++i) {
			V idx{ static_cast<double>(i) };
			V clip;
			if (!g_chromeProbeList.Invoke("GetClipByIndex", &clip, &idx, 1) ||
				!(clip.IsObject() || clip.IsDisplayObject())) {
				REX::INFO("[chrome]   clip {}: not returned", i);
				continue;
			}
			V nameV, labelV;
			const std::string clipName =
				clip.GetMember("name", &nameV) && nameV.IsString() ? nameV.GetString() : "?";
			const std::string label =
				clip.GetMember("currentLabel", &labelV) && labelV.IsString() ? labelV.GetString() : "?";
			REX::INFO("[chrome]   clip {}: name='{}' label='{}'", i, clipName, label);
		}

		// --- v0.9.1: the decoration pass. Script-ADDED children survive
		// timeline navigation by rule; script-SET properties are restamped
		// after every selection change the mod drives, which self-heals the
		// one open question (does a backward goto re-apply the authored row
		// matrix) and logs the answer if it ever fires.

		// The rows' own text format, cloned so the distance column wears the
		// donor's exact face rather than the borrowed HUD format.
		{
			V idx0{ 0.0 };
			V clip0, textMc0, textTf0;
			if (g_chromeProbeList.Invoke("GetClipByIndex", &clip0, &idx0, 1) &&
				(clip0.IsObject() || clip0.IsDisplayObject()) &&
				clip0.GetMember("Text_mc", &textMc0) &&
				(textMc0.IsObject() || textMc0.IsDisplayObject()) &&
				textMc0.GetMember("Text_tf", &textTf0) &&
				(textTf0.IsObject() || textTf0.IsDisplayObject()) &&
				textTf0.Invoke("getTextFormat", &g_chromeProbeRowFormat) &&
				g_chromeProbeRowFormat.IsObject()) {
				g_chromeProbeRowFormat.SetMember("align", V{ "right" });
				REX::INFO("[chrome] row text format cloned for the distance column");
			} else {
				g_chromeProbeRowFormat = RE::Scaleform::GFx::Value{};
				REX::WARN("[chrome] could not clone the row text format - distances go plain white");
			}
		}

		// Vanilla row height stays (v0.9.2): the donor's ~31 px rows centre
		// their 18 px text; 26 px left it sitting low. Half of it centres the
		// icon glyphs on the row.
		constexpr double kRowCentreY = 15.5;
		for (std::size_t i = 0; i < kChromeProbeRows; ++i) {
			V idx{ static_cast<double>(i) };
			V clip;
			if (!g_chromeProbeList.Invoke("GetClipByIndex", &clip, &idx, 1) ||
				!(clip.IsObject() || clip.IsDisplayObject()))
				continue;

			// Open the icon column: Text_mc is authored at x=9 and is MOVEd
			// only by rarity frames, which the probe never enters, so this
			// shift survives selection changes.
			V textMc;
			if (clip.GetMember("Text_mc", &textMc) &&
				(textMc.IsObject() || textMc.IsDisplayObject()))
				textMc.SetMember("x", V{ 29.0 });

			// The mod's own glyph in the vacated space - same painter as the
			// drawn panel, so the giants' ring and the settlement skyline
			// carry over unchanged.
			if (clip.CreateEmptyMovieClip(&g_chromeProbeIcons[i], "NavProbeIcon", 200)) {
				RE::Scaleform::GFx::Value gfx;
				if (g_chromeProbeIcons[i].GetMember("graphics", &gfx))
					DrawRowIcon(gfx, kProbeRows[i].cls, kProbeRows[i].settled);
				g_chromeProbeIcons[i].SetMember("x", V{ 19.0 });
				g_chromeProbeIcons[i].SetMember("y", V{ kRowCentreY });
			}

			// The distance column, right-aligned at the row's edge (row art is
			// ~372 wide; the field ends 10 short of it, clear of the capped
			// names on the left).
			root->CreateObject(&g_chromeProbeDists[i], "flash.text.TextField");
			if (g_chromeProbeDists[i].IsObject() || g_chromeProbeDists[i].IsDisplayObject()) {
				auto& dist = g_chromeProbeDists[i];
				dist.SetMember("selectable", V{ false });
				dist.SetMember("mouseEnabled", V{ false });
				dist.SetMember("multiline", V{ false });
				dist.SetMember("width", V{ 90.0 });
				dist.SetMember("height", V{ 24.0 });
				dist.SetMember("x", V{ 272.0 });
				dist.SetMember("y", V{ 4.0 });
				if (g_chromeProbeRowFormat.IsObject()) {
					dist.SetMember("embedFonts", V{ true });
					dist.SetMember("defaultTextFormat", g_chromeProbeRowFormat);
				} else {
					dist.SetMember("textColor", V{ static_cast<std::uint32_t>(0xFFFFFF) });
				}
				RE::Scaleform::GFx::Value distAdded;
				clip.Invoke("addChild", &distAdded, &dist, 1);
				dist.SetMember("text", V{ kProbeRows[i].dist });
				if (g_chromeProbeRowFormat.IsObject())
					dist.Invoke("setTextFormat", nullptr, &g_chromeProbeRowFormat, 1);
			}
		}

		// The header tint sample: exact colour via vanilla's own idiom -
		// mul 0 + add target, the same trick the Selected frame plays on the
		// row bar. 0 keeps the authentic teal.
		if (const auto tint = uProbeHeaderTint.GetValue(); tint != 0 && haveHeader) {
			V ct;
			root->CreateObject(&ct, "flash.geom.ColorTransform");
			RE::Scaleform::GFx::Value transform;
			if (ct.IsObject() &&
				ct.SetMember("color", V{ static_cast<std::uint32_t>(tint & 0xFFFFFF) }) &&
				header.GetMember("transform", &transform) && transform.IsObject() &&
				transform.SetMember("colorTransform", ct))
				REX::INFO("[chrome] header tinted to #{:06X}", tint & 0xFFFFFF);
			else
				REX::WARN("[chrome] header tint failed - the vanilla teal stays");
		}

		g_chromeProbeLastSel.store(sel, std::memory_order_release);
		g_chromeProbeReady.store(true, std::memory_order_release);
		REX::INFO("[chrome] probe ready at ({}, {}) - decorated: icon column open, "
				  "distances in the row's own format, rows at vanilla height",
			fProbeChromeOffsetX.GetValue(), fProbeChromeOffsetY.GetValue());
	}

	// Called from the high-frequency feed, on the UI thread, with distances
	// already refreshed. Row text is rebuilt at a few hertz rather than every
	// tick - distances crawl, and ten TextField writes per frame is a cost with
	// nothing to show for it. The highlight moves immediately, because that is
	// the part the player is waiting on.
	// Resolve ANY "$token" through the game's own localisation: set it on the
	// scratch TextField with GlobalFunc.SetText - the call vanilla itself uses
	// for its "$ENGINES"-style labels - and read the translated text back.
	// Returns empty when the token does not translate (the readback still
	// starts with '$') or the machinery is unavailable. Callers cache.
	std::string TranslateToken(const char* a_token)
	{
		if (!a_token || !*a_token)
			return {};
		if (g_translatorFailed.load(std::memory_order_acquire))
			return {};

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return {};
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		const auto                     menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return {};
		auto* root = menu->uiMovie->asMovieRoot.get();

		using V = RE::Scaleform::GFx::Value;
		if (!g_translatorReady.load(std::memory_order_acquire)) {
			// Constructs through the AS3 VM - single-winner like every other
			// builder; a losing thread just falls back until next refresh.
			static std::atomic<bool> s_buildInFlight{ false };
			const SingleWinner       winner{ s_buildInFlight };
			if (!winner.Won())
				return {};
			if (!g_translatorReady.load(std::memory_order_acquire)) {
				root->CreateObject(&g_translatorField, "flash.text.TextField");
				if (!g_translatorField.IsObject() && !g_translatorField.IsDisplayObject()) {
					g_translatorFailed.store(true, std::memory_order_release);
					REX::WARN("[panel] no scratch TextField - undiscovered labels use the "
							  "ini words");
					return {};
				}
				g_translatorReady.store(true, std::memory_order_release);
			}
		}

		// The proven route to a static: resolve the class object, Invoke on it.
		RE::Scaleform::GFx::Value globalFunc;
		if (!root->GetVariable(&globalFunc, "Shared.GlobalFunc") ||
			!(globalFunc.IsObject() || globalFunc.IsDisplayObject()))
			return {};

		V args[2];
		args[0] = g_translatorField;
		root->CreateString(&args[1], a_token);
		if (!globalFunc.Invoke("SetText", nullptr, args, 2))
			return {};

		V           text;
		std::string word;
		if (g_translatorField.GetMember("text", &text) && text.IsString() && text.GetString())
			word = text.GetString();
		if (!word.empty() && word.front() == '$')
			word.clear();  // came back untranslated - not a display string
		return word;
	}

	// The undiscovered-marker wrapper: category -> token -> word, cached per
	// category for the session - failures too, so a broken translator costs
	// one attempt, not one per refresh. The scratch field dies with the movie
	// and rebuilds on demand.
	std::string TranslateGenericLabel(std::uint32_t a_category)
	{
		{
			std::lock_guard labels{ g_genericLabelMutex };
			if (const auto hit = g_genericLabels.find(a_category); hit != g_genericLabels.end())
				return hit->second;
		}
		const char* token = GenericLabelToken(a_category);
		if (!token)
			return {};

		const std::string word = TranslateToken(token);
		{
			// Cache even the failure: one attempt per category per session.
			std::lock_guard labels{ g_genericLabelMutex };
			g_genericLabels[a_category] = word;
		}
		if (word.empty())
			REX::WARN("[panel] token '{}' did not translate - category {} uses the ini word",
				token, a_category);
		else
			REX::INFO("[panel] undiscovered label for category {}: '{}'", a_category, word);
		return word;
	}

	// "$Unknown Location" - the word vanilla prints for LMS_UNKNOWN markers
	// (DynamicPoiIcon.GetLocationPOIName's default arm). Same translator, same
	// cache-even-the-failure rule, parked under a key no real category uses.
	std::string TranslateUnknownLocation()
	{
		constexpr std::uint32_t kCacheKey = 0xFFFFFFFFu;
		{
			std::lock_guard labels{ g_genericLabelMutex };
			if (const auto hit = g_genericLabels.find(kCacheKey); hit != g_genericLabels.end())
				return hit->second;
		}
		const std::string word = TranslateToken("$Unknown Location");
		{
			std::lock_guard labels{ g_genericLabelMutex };
			g_genericLabels[kCacheKey] = word;
		}
		if (word.empty())
			REX::WARN("[panel] '$Unknown Location' did not translate - LMS_UNKNOWN rows use the ini word");
		else
			REX::INFO("[panel] LMS_UNKNOWN label: '{}'", word);
		return word;
	}

	void RefreshPanel()
	{
		if (!g_panelReady.load(std::memory_order_acquire))
			return;

		using V = RE::Scaleform::GFx::Value;

		const bool open = g_panelOpen.load(std::memory_order_acquire) &&
		                  g_inCruise.load(std::memory_order_acquire);

		// The open/close animation (v0.13.0): a state machine on wall time,
		// advanced here because this runs every feed tick - the cadence that
		// keeps the arrow smooth. The plate grows from a small rectangle
		// about its centre and the content waits for it to finish; closing
		// mirrors it, and a toggle mid-flight reverses from the current
		// progress rather than jumping. Duration 0 collapses everything to
		// the old instant show/hide.
		using animClock = std::chrono::steady_clock;
		static animClock::time_point s_animStart{};
		static PanelAnim             s_prevAnim = PanelAnim::kClosed;
		const double dur = std::max(0.0, static_cast<double>(fPanelAnimSeconds.GetValue()));
		const auto   nowAnim = animClock::now();
		auto         anim = g_panelAnimState.load(std::memory_order_acquire);
		const auto   elapsed = [&] {
			return std::chrono::duration<double>(nowAnim - s_animStart).count();
		};
		const auto backdate = [&](double a_alreadyElapsed) {
			s_animStart = nowAnim - std::chrono::duration_cast<animClock::duration>(
										std::chrono::duration<double>(a_alreadyElapsed));
		};

		if (open && (anim == PanelAnim::kClosed || anim == PanelAnim::kClosing)) {
			// Openness carries across a reversal: a plate half-shrunk starts
			// growing from half, not from scratch.
			double openness = 0.0;
			if (anim == PanelAnim::kClosing && dur > 0.0)
				openness = std::clamp(1.0 - elapsed() / dur, 0.0, 1.0);
			anim = PanelAnim::kOpening;
			backdate(openness * dur);
		} else if (!open && (anim == PanelAnim::kOpen || anim == PanelAnim::kOpening)) {
			double openness = 1.0;
			if (anim == PanelAnim::kOpening && dur > 0.0)
				openness = std::clamp(elapsed() / dur, 0.0, 1.0);
			anim = PanelAnim::kClosing;
			backdate((1.0 - openness) * dur);
		}
		if (anim == PanelAnim::kOpening && (dur <= 0.0 || elapsed() >= dur))
			anim = PanelAnim::kOpen;
		if (anim == PanelAnim::kClosing && (dur <= 0.0 || elapsed() >= dur))
			anim = PanelAnim::kClosed;
		g_panelAnimState.store(anim, std::memory_order_release);
		const bool animEntered = anim != s_prevAnim;
		s_prevAnim = anim;

		g_panelClip.SetMember("visible", V{ anim != PanelAnim::kClosed });

		if (anim == PanelAnim::kOpening || anim == PanelAnim::kClosing) {
			const double openness = anim == PanelAnim::kOpening ?
			                            std::clamp(elapsed() / std::max(dur, 1e-6), 0.0, 1.0) :
			                            std::clamp(1.0 - elapsed() / std::max(dur, 1e-6), 0.0, 1.0);
			// Fade and grow run TOGETHER across the whole timeline (v0.13.2):
			// v0.13.1 sequenced a short fade before the grow, and at this
			// time scale it was imperceptible - four-odd frames on a plate
			// twelve percent of its size reads as a pop. Concurrent, the
			// fade spans the full duration and cannot be missed. One
			// openness value still drives both, so reversals carry.
			const double scale = 0.12 + 0.88 * openness;
			const double w = static_cast<double>(fPanelWidth.GetValue());
			const double h = g_panelHeight.load(std::memory_order_acquire);
			g_panelClip.SetMember("alpha", V{ openness });
			g_panelClip.SetMember("scaleX", V{ scale });
			g_panelClip.SetMember("scaleY", V{ scale });
			g_panelClip.SetMember("x",
				V{ static_cast<double>(fPanelOffsetX.GetValue()) + (1.0 - scale) * w * 0.5 });
			g_panelClip.SetMember("y",
				V{ static_cast<double>(fPanelOffsetY.GetValue()) + (1.0 - scale) * h * 0.5 });
		} else if (animEntered && anim == PanelAnim::kOpen) {
			g_panelClip.SetMember("alpha", V{ 1.0 });
			g_panelClip.SetMember("scaleX", V{ 1.0 });
			g_panelClip.SetMember("scaleY", V{ 1.0 });
			g_panelClip.SetMember("x", V{ static_cast<double>(fPanelOffsetX.GetValue()) });
			g_panelClip.SetMember("y", V{ static_cast<double>(fPanelOffsetY.GetValue()) });
		}

		// Content belongs to the settled-open state alone. Entering a moving
		// state hides everything (title and hints included - static children
		// the normal refresh never touches); entering open re-shows the
		// statics, and the rows, scrollbar and highlight restore themselves
		// through the normal pass below.
		if (animEntered) {
			const auto setVis = [&](RE::Scaleform::GFx::Value& a_v, bool a_show) {
				if (a_v.IsObject() || a_v.IsDisplayObject())
					a_v.SetMember("visible", V{ a_show });
			};
			if (anim == PanelAnim::kOpen) {
				setVis(g_panelTitle, true);
				setVis(g_panelHint, true);
				setVis(g_panelHintRight, true);
				setVis(g_panelConfirmPill, true);
				setVis(g_panelBrowsePill, true);
			} else if (anim == PanelAnim::kOpening || anim == PanelAnim::kClosing) {
				setVis(g_panelTitle, false);
				setVis(g_panelHint, false);
				setVis(g_panelHintRight, false);
				setVis(g_panelConfirmPill, false);
				setVis(g_panelBrowsePill, false);
				setVis(g_panelHighlight, false);
				setVis(g_panelScrollTrack, false);
				setVis(g_panelScrollThumb, false);
				for (std::size_t i = 0; i < kPanelMaxRowsHard; ++i) {
					setVis(g_panelRows[i], false);
					setVis(g_panelDists[i], false);
					setVis(g_panelIcons[i], false);
					setVis(g_panelPoiIcons[i], false);
					setVis(g_panelGiantIcons[i], false);
					// Hidden with the rest during the animation, and forgotten
					// so the settled-open pass re-asserts them rather than
					// believing a stale "already drawn".
					setVis(g_panelSurveyBanners[i], false);
					setVis(g_panelSurveyBars[i], false);
					g_panelSurveyDrawn[i] = kSurveyNeverDrawn;
				}
			}
		}

		// The chrome probe rides the panel - and its visibility is written on
		// the same tick, BEFORE the closed-panel return below.
		const bool probeReady = g_chromeProbeReady.load(std::memory_order_acquire);
		if (probeReady)
			g_chromeProbe.SetMember("visible", V{ open });

		// The scanner-key hint is the panel's inverse: it shows in cruise
		// while the plate is fully closed - the pill that says which key
		// would open it - and leaves the moment the plate starts growing.
		if (g_scannerHint.IsObject() || g_scannerHint.IsDisplayObject())
			g_scannerHint.SetMember("visible",
				V{ anim == PanelAnim::kClosed &&
					g_inCruise.load(std::memory_order_acquire) });

		// The toggle sound, borrowed from the scanner (v0.12.2): marked on
		// the input thread, played here through vanilla's own PlayMenuSound
		// dispatcher - the same GlobalFunc static the menus themselves use.
		// BEFORE the closed-panel return, or the close sound never fires.
		if (const auto pending = g_pendingPanelSound.exchange(0, std::memory_order_acq_rel);
			pending != 0) {
			const std::string id = pending == 1 ? sPanelOpenSound.GetValue() :
			                                      sPanelCloseSound.GetValue();
			if (!id.empty()) {
				const auto                     ui = RE::UI::GetSingleton();
				static const RE::BSFixedString s_hud{ kShipHudMenu };
				const auto                     menu = ui ? ui->GetMenu(s_hud) : nullptr;
				if (menu && menu->uiMovie && menu->uiMovie->asMovieRoot) {
					auto*                     sndRoot = menu->uiMovie->asMovieRoot.get();
					RE::Scaleform::GFx::Value gf;
					if (sndRoot->GetVariable(&gf, "Shared.GlobalFunc") &&
						(gf.IsObject() || gf.IsDisplayObject())) {
						V arg;
						sndRoot->CreateString(&arg, id.c_str());
						gf.Invoke("PlayMenuSound", nullptr, &arg, 1);
					}
				}
			}
		}

		// Everything below is content, and content belongs to the settled-
		// open state: while the plate is closed or in motion there is
		// nothing else to update.
		if (anim != PanelAnim::kOpen)
			return;

		using clock = std::chrono::steady_clock;
		static auto s_lastText = clock::time_point{};
		const auto  now = clock::now();
		const bool  refreshText = std::chrono::duration<float>(now - s_lastText).count() >= 0.25f;
		if (refreshText) {
			s_lastText = now;
			// Rides this cadence so a device swap made WHILE the panel is up
			// is followed, the way the confirm pill follows one by itself.
			// The panel has been closed for longer than the interval by the
			// time it opens, so the first tick after an open also lands here
			// and the pill is dressed before it is looked at.
			RefreshBrowsePillDevice();
		}

		const auto    rowCount = static_cast<std::size_t>(g_panelRowCount.load(std::memory_order_acquire));
		const auto    highlight = g_highlightID.load(std::memory_order_acquire);
		const auto    locked = g_lockedID.load(std::memory_order_acquire);
		const double  rowHeight = static_cast<double>(fPanelRowHeight.GetValue());
		std::size_t   highlightRow = 0;
		bool          haveHighlightRow = false;

		// Snapshot under the lock, render OUTSIDE it. The row loop below calls
		// into the Scaleform VM, and a VM call must never happen while
		// g_candidateMutex is held: the feed callbacks take this mutex from
		// inside the engine's own dispatch, and they run concurrently across
		// the BSJobs pool - so a thread rendering under the mutex while
		// another waits for it inside the VM is a lock-order inversion. That
		// deadlocks silently: no exception, no crash log, the game just stops
		// responding. Prime suspect for the 2026-07-28 load-time freeze, and
		// wrong regardless. Copying up to sixteen rows is nothing next to one
		// SetMember.
		std::vector<Candidate> visibleRows;
		std::size_t            highlightPos = 0;
		bool                   haveHighlightPos = false;
		std::size_t            scrollFirst = 0;
		std::size_t            scrollTotal = 0;
		{
			std::lock_guard          lock{ g_candidateMutex };
			std::vector<std::size_t> local;
			CollectLocalRows(local);
			scrollTotal = local.size();

			// Scroll so the highlight stays on screen once the system has more
			// bodies than the panel has rows.
			std::size_t first = 0;
			for (std::size_t n = 0; n < local.size(); ++n) {
				if (g_candidates[local[n]].id == highlight) {
					if (n >= rowCount)
						first = n - rowCount + 1;
					highlightPos = n;
					haveHighlightPos = true;
					break;
				}
			}
			scrollFirst = first;

			visibleRows.reserve(rowCount);
			for (std::size_t r = 0; r < rowCount; ++r) {
				const std::size_t n = first + r;
				if (n >= local.size())
					break;
				visibleRows.push_back(g_candidates[local[n]]);
			}
		}

		// The scrollbar tracks the window over the local list, and only
		// exists on screen while there is somewhere to scroll to.
		if (g_panelScrollThumb.IsObject() || g_panelScrollThumb.IsDisplayObject()) {
			const bool overflow = scrollTotal > rowCount && rowCount > 0;
			g_panelScrollTrack.SetMember("visible", V{ overflow });
			g_panelScrollThumb.SetMember("visible", V{ overflow });
			if (overflow) {
				const double trackTop = g_panelListTop.load(std::memory_order_acquire) + 2.0;
				const double trackH = rowHeight * static_cast<double>(rowCount) - 4.0;
				const double thumbH = std::max(8.0,
					trackH * static_cast<double>(rowCount) / static_cast<double>(scrollTotal));
				const auto   denom = scrollTotal - rowCount;
				const double t = denom > 0 ?
				                     static_cast<double>(scrollFirst) / static_cast<double>(denom) :
				                     0.0;
				g_panelScrollThumb.SetMember("y", V{ trackTop + (trackH - thumbH) * t });
				g_panelScrollThumb.SetMember("scaleY", V{ thumbH });
			}
		}

		// Checklist step 3: the wheel drives the donor's own highlight. The
		// real highlight's position among the local rows maps onto the probe's
		// four hardcoded rows; the selectedIndex setter is vanilla's own path
		// (scroll adjust + Selected/unselected frames on the row clips), called
		// only when the value actually changes. VM write outside the mutex,
		// like everything else here.
		if (probeReady && haveHighlightPos) {
			const auto sel = static_cast<std::int32_t>(highlightPos % kChromeProbeRows);
			if (g_chromeProbeLastSel.exchange(sel, std::memory_order_acq_rel) != sel) {
				const bool wrote =
					g_chromeProbeList.SetMember("selectedIndex", V{ static_cast<double>(sel) });

				// v0.9.2 wheel diagnosis, one line per wheel notch: the sel
				// the mirror computed, whether the write was accepted, what
				// the list reports back, and which frame the target row
				// landed on ('NormalSelected' = the visual actually moved).
				// If wheeling produces NO such lines, the break is upstream
				// of the mirror - highlight id / input - which the drawn
				// panel's own bar confirms or clears on sight.
				V readback;
				const double listSel =
					g_chromeProbeList.GetMember("selectedIndex", &readback) ? AsNumber(readback) :
					                                                          -99.0;
				V           idx{ static_cast<double>(sel) };
				V           clip, labelV;
				std::string label = "?";
				if (g_chromeProbeList.Invoke("GetClipByIndex", &clip, &idx, 1) &&
					(clip.IsObject() || clip.IsDisplayObject()) &&
					clip.GetMember("currentLabel", &labelV) && labelV.IsString() &&
					labelV.GetString())
					label = labelV.GetString();
				REX::INFO("[chrome] mirror sel -> {} (write {}, list says {:.0f}, row label '{}')",
					sel, wrote ? "ok" : "REFUSED", listSel, label);
			}
		}

		{
			for (std::size_t r = 0; r < rowCount; ++r) {
				auto& nameField = g_panelRows[r];
				auto& distField = g_panelDists[r];
				auto& iconClip = g_panelIcons[r];
				auto& poiIcon = g_panelPoiIcons[r];
				auto& giantIcon = g_panelGiantIcons[r];
				const bool haveIcon = iconClip.IsObject() || iconClip.IsDisplayObject();
				const bool havePoiIcon = poiIcon.IsObject() || poiIcon.IsDisplayObject();
				const bool haveGiantIcon = giantIcon.IsObject() || giantIcon.IsDisplayObject();

				if (r >= visibleRows.size()) {
					nameField.SetMember("visible", V{ false });
					distField.SetMember("visible", V{ false });
					if (haveIcon)
						iconClip.SetMember("visible", V{ false });
					if (havePoiIcon)
						poiIcon.SetMember("visible", V{ false });
					if (haveGiantIcon)
						giantIcon.SetMember("visible", V{ false });
					// ⚠ Every EXEMPTION from a level-based write is also an
					// exemption from the RESTORE - the v0.10.2 lesson. An empty
					// slot has to stand its survey mark down explicitly and
					// forget what it drew, or the mark rides a scroll onto a
					// body that never earned it.
					if (g_panelSurveyDrawn[r] != kSurveyNeverDrawn) {
						if (g_panelSurveyBanners[r].IsObject() || g_panelSurveyBanners[r].IsDisplayObject())
							g_panelSurveyBanners[r].SetMember("visible", V{ false });
						if (g_panelSurveyBars[r].IsObject() || g_panelSurveyBars[r].IsDisplayObject())
							g_panelSurveyBars[r].SetMember("visible", V{ false });
						g_panelSurveyDrawn[r] = kSurveyNeverDrawn;
					}
					continue;
				}

				const auto& row = visibleRows[r];
				const bool  rowBright = row.id == highlight;
				if (rowBright) {
					highlightRow = r;
					haveHighlightRow = true;
				}

				if (refreshText) {
					// Moons sit indented under their planet. Done by moving the
					// field rather than padding the string, so it does not
					// depend on the width of a space in a borrowed font.
					const double indent =
						row.isMoon ? static_cast<double>(fPanelMoonIndent.GetValue()) : 0.0;
					nameField.SetMember("x",
						V{ 10.0 + (bPanelIcons.GetValue() ? 20.0 : 0.0) + indent });
					// The field's right edge stays put when the left one
					// indents, so the truncation below measures honestly and
					// moon names cannot run under the distance column.
					const double baseWidth = g_panelNameWidth.load(std::memory_order_acquire);
					const double fieldWidth =
						baseWidth > 0.0 ? std::max(40.0, baseWidth - indent) : 0.0;
					if (fieldWidth > 0.0)
						nameField.SetMember("width", V{ fieldWidth });

					// The locked body is marked in the list itself, so the panel
					// says what the HUD is showing without having to be closed.
					// Station/POI labels follow the SAME recipe as the icon the
					// player sees (POIIcon.TryUpdateName ->
					// DynamicPoiIcon.GetLocationPOIName), driven by
					// uLocationMarkerState - NOT bMarkerDiscovered. The two can
					// disagree: a runtime-spawned encounter ("Ecliptic
					// Satellite") arrives FULL_REVEAL, named on the HUD from
					// the first frame, while the discovered flag stays false -
					// masking on the flag printed "Unknown" beside a named
					// marker (tester, v0.18.0). FULL_REVEAL -> the real name;
					// ONLY_TYPE_KNOWN -> the category's generic word, or the
					// REAL NAME when no generic exists (vanilla's own
					// fallback); LMS_UNKNOWN -> the game's "$Unknown Location".
					// The ini words remain the failed-translation fallback and
					// the whole story under bUseCustomUndiscoveredLabels; an
					// entry without the state field keeps the old
					// discovered-based reading.
					const bool  isLocked = locked != 0 && row.id == locked;
					const char* mark = isLocked ? "> " : "  ";
					const bool  poiLike = row.type == kTargetTypeStation ||
					                     row.type == kTargetTypePOI;
					std::string display = row.name;
					if (poiLike) {
						const std::uint32_t state =
							row.haveLocState ? row.locMarkerState :
							                   (row.discovered ? kLmsFullReveal : kLmsOnlyTypeKnown);
						const bool custom = bUseCustomUndiscoveredLabels.GetValue();
						if (state == kLmsOnlyTypeKnown) {
							display.clear();
							if (!custom) {
								if (GenericLabelToken(row.havePoi ? row.poiCategory : 0) == nullptr)
									// No generic word exists for this kind
									// (NONE, SIMPLE, absent fields): vanilla
									// shows the name, not a placeholder.
									display = row.name;
								else
									display = TranslateGenericLabel(row.poiCategory);
							}
							if (display.empty())
								display = row.type == kTargetTypeStation ?
								              sUndiscoveredStationLabel.GetValue() :
								              sUndiscoveredPoiLabel.GetValue();
						} else if (state != kLmsFullReveal) {
							// LMS_UNKNOWN and anything newer.
							display.clear();
							if (!custom)
								display = TranslateUnknownLocation();
							if (display.empty())
								display = row.type == kTargetTypeStation ?
								              sUndiscoveredStationLabel.GetValue() :
								              sUndiscoveredPoiLabel.GetValue();
						}
					}
					const auto name = std::format("{}{}", mark, display);
					nameField.SetMember("text", V{ name.c_str() });
					// defaultTextFormat only applies to text present when it was
					// set, so re-apply after every assignment or the new glyphs
					// fall back to no font.
					if (g_panelFormat.IsObject())
						nameField.Invoke("setTextFormat", nullptr, &g_panelFormat, 1);
					// The ellipsis (v0.12.1). Vanilla's TruncateSingleLineText
					// looked right but no-ops here: it leans on
					// getCharIndexAtPoint, and its only vanilla caller sits
					// behind a truncateToFit flag no shipping menu sets - the
					// truncation the game actually ships is SetText's CHARACTER
					// budget. This does the same job but measured: read the
					// styled text's real pixel width (textWidth - a primitive
					// vanilla's own row code reads in paths that provably run),
					// cut proportionally at a UTF-8 boundary so localised
					// names cannot split mid-glyph, and append the same
					// single-glyph ellipsis - which the v0.9.0 probe proved
					// the embedded font carries.
					if (fieldWidth > 0.0 && name.size() > 3) {
						RE::Scaleform::GFx::Value twVal;
						if (nameField.GetMember("textWidth", &twVal)) {
							const double tw = AsNumber(twVal);
							const double target = fieldWidth - 6.0;  // the field's gutters
							if (tw > target) {
								// Reserve ~12 px for the ellipsis glyph, cut
								// proportionally, then walk back onto a UTF-8
								// boundary.
								std::size_t cut = static_cast<std::size_t>(
									static_cast<double>(name.size()) *
									std::max(0.1, (target - 12.0) / tw));
								if (cut >= name.size())
									cut = name.size() - 1;
								while (cut > 0 &&
									   (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80)
									--cut;
								const std::string trimmed =
									name.substr(0, cut) + "\xE2\x80\xA6";
								nameField.SetMember("text", V{ trimmed.c_str() });
								if (g_panelFormat.IsObject())
									nameField.Invoke("setTextFormat", nullptr, &g_panelFormat, 1);
							}
						}
					}

					// Light-seconds once kilometres stop being readable. A dash
					// marks a body the game is not currently tracking: it is
					// really there, but there is no distance to give and the
					// pointer cannot aim at it.
					//
					// Locking one of those is allowed and useful - you can set a
					// destination before the game will follow it - so it gets
					// "..." rather than a dash, to say the lock is real and
					// waiting rather than doing nothing. The lock is held as a
					// form id and re-resolved against the feed every update, so
					// it starts guiding the moment the body is tracked, with no
					// need to lock it again.
					const double lightSeconds = row.distance / kMetersPerLightSecond;
					const auto   dist = !row.fromFeed       ? std::string{ isLocked ? "..." : "-" } :
					                    row.distance <= 0.0 ? std::string{} :
					                    lightSeconds >= 1.0 ? std::format("{:.0f} LS", lightSeconds) :
					                                          std::format("{:.0f} km", row.distance / 1000.0);
					distField.SetMember("text", V{ dist.c_str() });
					if (g_panelDistFormat.IsObject())
						distField.Invoke("setTextFormat", nullptr, &g_panelDistFormat, 1);
				}
				// Redrawn only when this row's class actually changes, which is
				// rare - scrolling a list, not every frame.
				if (haveIcon || havePoiIcon || haveGiantIcon) {
					PlanetClass rowClass = PlanetClass::kUnknown;
					bool        rowSettled = false;
					{
						std::lock_guard bodies{ g_bodyTableMutex };
						if (const auto body = g_bodyTable.find(row.id); body != g_bodyTable.end()) {
							rowClass = body->second.planetClass;
							rowSettled = body->second.settled;
						}
					}

					// Which art fits the row: the game's own badge for a POI,
					// station or ship carrying a REAL marker type (the count
					// sentinel means "no marker" - landing sites arrive that
					// way), and for the settled bodies; undiscovered entries
					// get the generic kind badge through vanilla's own
					// masking state, matching the row's masked label.
					//
					// uPoiType/uPoiCategory LINGER on the feed's pooled entry
					// objects: a planet's slot can carry a station's leftover
					// fields (v0.11.1's session: Venus wore a badge and the
					// giants' circles never showed - each stale type stole
					// the badge path). havePoi means "the fields were
					// present", never "this is a POI" - the entry's own
					// uTargetType is the authority, exactly as it is inside
					// vanilla's OffScreenIcon, which is why the faux marker
					// never had this bug.
					std::uint32_t poiType = 0;
					std::uint32_t poiCat = 0;
					std::uint32_t poiState = 0;
					bool          wantVanilla = false;
					const bool    poiKind = row.type == kTargetTypePOI ||
					                     row.type == kTargetTypeShip ||
					                     row.type == kTargetTypeStation;
					if (havePoiIcon) {
						if (row.havePoi && poiKind && row.poiType < kMarkerTypeCount) {
							poiType = row.poiType;
							poiCat = row.poiCategory;
							// The entry's own state, exactly what vanilla
							// hands SetLocation (POIIcon.as:28); synthesized
							// from `discovered` only when the feed omitted
							// the field (the pre-v0.18.0 reading).
							poiState = row.haveLocState ?
							               row.locMarkerState :
							               (row.discovered ? kLmsFullReveal : kLmsOnlyTypeKnown);
							wantVanilla = true;
						} else if (rowSettled) {
							poiType = kMarkerSurfaceSettlement;
							poiCat = 0;
							poiState = kLmsFullReveal;
							wantVanilla = true;
						}
					}

					bool vanillaShown = false;
					if (wantVanilla) {
						const std::uint64_t key = (std::uint64_t{ 1 } << 63) |
						                          (std::uint64_t{ poiType } << 32) |
						                          (std::uint64_t{ poiCat } << 8) |
						                          std::uint64_t{ poiState };
						if (g_panelPoiIconKey[r] != key) {
							V args[3]{ V{ poiType }, V{ poiCat }, V{ poiState } };
							if (poiIcon.Invoke("SetLocation", nullptr, args, 3)) {
								// SetLocation does not scale a fresh child on
								// its own (only the load-event path does), so
								// the scale is re-asserted after every swap.
								V scale{ static_cast<double>(fPanelVanillaIconScale.GetValue()) };
								poiIcon.Invoke("SetMarkerScale", nullptr, &scale, 1);
								g_panelPoiIconKey[r] = key;
							}
						}
						vanillaShown = g_panelPoiIconKey[r] == key;
					}
					if (havePoiIcon)
						poiIcon.SetMember("visible", V{ vanillaShown });

					// The giants wear the in-POV circle with the ring-line -
					// one icon for every giant class, the tester's design.
					const bool giantClass = rowClass == PlanetClass::kGasGiant ||
					                        rowClass == PlanetClass::kHotGasGiant ||
					                        rowClass == PlanetClass::kIceGiant;
					const bool giantShown = giantClass && haveGiantIcon && !vanillaShown;
					if (haveGiantIcon)
						giantIcon.SetMember("visible", V{ giantShown });

					// The drawn glyphs are the fallback wherever a vanilla
					// piece is missing or failed.
					if (haveIcon) {
						if (!g_panelIconDrawn[r] || g_panelIconClass[r] != rowClass ||
							g_panelIconSettled[r] != rowSettled) {
							RE::Scaleform::GFx::Value gfx;
							if (iconClip.GetMember("graphics", &gfx)) {
								gfx.Invoke("clear", nullptr, nullptr, 0);
								DrawRowIcon(gfx, rowClass, rowSettled);
								g_panelIconClass[r] = rowClass;
								g_panelIconSettled[r] = rowSettled;
								g_panelIconDrawn[r] = true;
							}
						}
						iconClip.SetMember("visible",
							V{ !vanillaShown && !giantShown && HasRowIcon(rowClass, rowSettled) });
					}
				}

				// The survey mark in the distance cell (Phase 6). Read from the
				// mod-side store at render time, the way rowClass and rowSettled
				// already are - which is the whole reason "several rows flip at
				// once" needs no machinery: this runs on the HIGH feed, while the
				// candidate list only rebuilds on the LOW one.
				//
				// ★ Gated on uTargetType, never on "the row has an id": a POI or
				// station row carries a REFR id, and a survey percent keyed by it
				// would be a different body's answer. Nothing is drawn for a body
				// the sweep has not reached yet - unknown and unsurveyed look the
				// same, which is what keeps a late read merely late.
				//
				// ⚠ The epoch test is the one that matters. Without it, "the map
				// is current" would rest on the sweep having cleared it before
				// this ran - and the two are unordered threads (this rides the
				// HIGH feed, the sweep the per-frame task). Whether that race is
				// winnable today depends on a chain of unrelated guards that a
				// later change could quietly remove; with the test, a map from
				// another save simply reads as UNKNOWN, and unknown draws
				// nothing. The reader is the authority, not the writer.
				float surveyPct = -1.0f;
				if (bPanelSurveyMarks.GetValue() && row.type == kTargetTypePlanet && row.id != 0) {
					// Epoch tested INSIDE the lock, with the same ordering the
					// clear uses, so "the map is current" and "these are the
					// map's contents" are one observation rather than two.
					std::lock_guard lock{ g_surveyedMutex };
					if (g_surveyedEpoch.load(std::memory_order_acquire) ==
						g_unsettledEpoch.load(std::memory_order_acquire)) {
						const auto found = g_surveyedPercent.find(row.id);
						if (found != g_surveyedPercent.end())
							surveyPct = found->second;
					}
				}
				const bool surveyChanged = g_panelSurveyDrawn[r] != surveyPct;
				const bool bannerShown = surveyPct >= 1.0f;
				if (surveyChanged) {
					const float minPct = std::max(fPanelSurveyMinPercent.GetValue(), 0.0f);
					const bool  barShown = surveyPct > minPct && surveyPct < 1.0f;
					if (g_panelSurveyBanners[r].IsObject() || g_panelSurveyBanners[r].IsDisplayObject())
						g_panelSurveyBanners[r].SetMember("visible", V{ bannerShown });
					if (g_panelSurveyBars[r].IsObject() || g_panelSurveyBars[r].IsDisplayObject()) {
						g_panelSurveyBars[r].SetMember("visible", V{ barShown });
						if (barShown && (g_panelSurveyFills[r].IsObject() ||
											g_panelSurveyFills[r].IsDisplayObject()))
							g_panelSurveyFills[r].SetMember("scaleX",
								V{ static_cast<double>(std::clamp(surveyPct, 0.0f, 1.0f)) });
					}
					g_panelSurveyDrawn[r] = surveyPct;
				}

				// The highlighted row's text steps up to the bright colour
				// while the bar is on it - vanilla's Selected trick
				// (v0.16.2). textColor overrides the format's colour, so it
				// is re-asserted after every text refresh (whose
				// setTextFormat resets it) and on the tick the highlight
				// arrives at or leaves this row - or the survey mark does.
				if (refreshText || rowBright != g_panelRowBright[r] || surveyChanged) {
					const V rowColour{ rowBright ? uPanelTextColorHighlight.GetValue() :
					                               uPanelTextColor.GetValue() };
					nameField.SetMember("textColor", rowColour);
					// On a surveyed row the number sits ON vanilla's near-white
					// plate, so it wears the banner's own dark label colour -
					// which is exactly what the banner does with its own text.
					// This wins over the highlight: bright-on-near-white would
					// be nothing at all.
					const bool darkOnPlate = bannerShown && bPanelSurveyBannerText.GetValue();
					distField.SetMember("textColor",
						darkOnPlate ? V{ uPanelSurveyLabel.GetValue() } : rowColour);
				}
				g_panelRowBright[r] = rowBright;

				nameField.SetMember("visible", V{ true });
				distField.SetMember("visible", V{ true });
			}
		}

		if (g_panelHighlight.IsObject() || g_panelHighlight.IsDisplayObject()) {
			g_panelHighlight.SetMember("visible", V{ haveHighlightRow });
			if (haveHighlightRow)
				g_panelHighlight.SetMember("y",
				V{ g_panelListTop.load(std::memory_order_acquire) +
					rowHeight * static_cast<double>(highlightRow) });
		}
	}

	void TryCreateArrow()
	{
		if (!bArrow.GetValue() || g_arrowReady.load(std::memory_order_acquire) || g_arrowFailed.load(std::memory_order_acquire))
			return;
		if (!g_subscribed.load(std::memory_order_acquire))
			return;  // no feed yet, so nothing to point at

		// Creates a clip and borrows a TextFormat through the AS3 VM, so it is
		// serialised for the same reason the subscriber is - see SingleWinner.
		const SingleWinner winner{ g_arrowBuildInFlight };
		if (!winner.Won())
			return;
		if (g_arrowReady.load(std::memory_order_acquire))
			return;  // built while we were waiting for the claim

		const auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;
		static const RE::BSFixedString s_shipHud{ kShipHudMenu };
		if (!ui->IsMenuOpen(s_shipHud))
			return;
		const auto menu = ui->GetMenu(s_shipHud);
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot)
			return;

		auto*             root = menu->uiMovie->asMovieRoot.get();
		const char*       rootPath = menu->GetRootPath();
		const std::string reticlePath = std::string{ rootPath ? rootPath : "root" } + ".Reticle_mc";

		RE::Scaleform::GFx::Value reticle;
		if (!root->GetVariable(&reticle, reticlePath.c_str()))
			return;

		const auto giveUp = [&](const char* a_why) {
			REX::WARN("[arrow] not created: {}", a_why);
			g_arrowFailed.store(true, std::memory_order_release);
		};

		// CreateEmptyMovieClip is the AS2-era call and may not exist on an AS3
		// display object, so fall back to constructing a Sprite and adding it.
		// The pre-line means a log that ENDS here names the frozen call.
		REX::INFO("[arrow] building - entering the VM");
		bool made = reticle.CreateEmptyMovieClip(&g_arrowClip, "ShipNavPanelArrow", 20000);
		REX::INFO("[arrow] CreateEmptyMovieClip: {}", made ? "ok" : "failed, trying Sprite");
		if (!made) {
			root->CreateObject(&g_arrowClip, "flash.display.Sprite");
			if (!g_arrowClip.IsObject() && !g_arrowClip.IsDisplayObject()) {
				giveUp("neither CreateEmptyMovieClip nor flash.display.Sprite produced an object");
				return;
			}
			RE::Scaleform::GFx::Value added;
			if (!reticle.Invoke("addChild", &added, &g_arrowClip, 1)) {
				giveUp("addChild rejected the sprite");
				return;
			}
			REX::INFO("[arrow] Sprite created and added to the reticle");
		}

		RE::Scaleform::GFx::Value gfx;
		if (!g_arrowClip.GetMember("graphics", &gfx)) {
			giveUp("the clip has no 'graphics' member to draw into");
			return;
		}

		// A DIAMOND, drawn about the clip's own origin, and moved around an
		// invisible circle rather than rotated.
		//
		// It used to be a triangle whose tip sat out at the radius, swept round
		// by rotating the clip. That reads wrongly as a target approaches the
		// centre of the screen: an outward-pointing arrow says "keep turning"
		// exactly when there is nothing left to turn. A symmetric marker has no
		// orientation to get wrong - its POSITION alone carries the direction,
		// which is what an invisible reticle circle needs. Top-right means steer
		// up and right, and it means that at every angle.
		const double r = static_cast<double>(fArrowRadius.GetValue());
		using V = RE::Scaleform::GFx::Value;

		constexpr double kHalf = 13.0;
		V fill[]{ V{ static_cast<std::uint32_t>(0x66CCFF) }, V{ 1.0 } };
		gfx.Invoke("beginFill", nullptr, fill, 2);
		V p0[]{ V{ 0.0 }, V{ -kHalf } };
		gfx.Invoke("moveTo", nullptr, p0, 2);
		V p1[]{ V{ kHalf * 0.62 }, V{ 0.0 } };
		gfx.Invoke("lineTo", nullptr, p1, 2);
		V p2[]{ V{ 0.0 }, V{ kHalf } };
		gfx.Invoke("lineTo", nullptr, p2, 2);
		V p3[]{ V{ -kHalf * 0.62 }, V{ 0.0 } };
		gfx.Invoke("lineTo", nullptr, p3, 2);
		gfx.Invoke("lineTo", nullptr, p0, 2);
		gfx.Invoke("endFill", nullptr, nullptr, 0);

		g_arrowClip.SetMember("x", V{ 0.0 });
		g_arrowClip.SetMember("y", V{ 0.0 });
		g_arrowClip.SetMember("visible", V{ false });

		// No name label. It existed until v0.8.3 and was removed on the
		// tester's call: vanilla shows no names on blips, the panel row
		// already carries name and distance, and the whole point of the blip
		// pivot is a quieter HUD - one marker, no text riding it.

		g_arrowReady.store(true, std::memory_order_release);
		REX::INFO("[arrow] ready (radius {}) - press the scanner key to cycle targets", r);
	}

	// ---------------------------------------------------------------------------
	// Tap 3: context heartbeat, for correlating with the input log.
	// ---------------------------------------------------------------------------

	void LogHeartbeat()
	{
		// The statics below are shared if this task ever runs on both the main
		// and a renderer thread (both have been observed). A torn read only
		// shifts the log cadence, which is harmless for a diagnostic - unlike
		// the input tap's claim flag, this does not need to be atomic.
		using clock = std::chrono::steady_clock;
		static auto s_lastBeat = clock::now();

		const auto  now = clock::now();
		const float interval = std::max(fHeartbeatSeconds.GetValue(), 1.0f);
		if (std::chrono::duration<float>(now - s_lastBeat).count() < interval)
			return;
		s_lastBeat = now;

		// While either menu is up the world is in flux; touch nothing.
		static const RE::BSFixedString s_loadingMenu{ "LoadingMenu" };
		static const RE::BSFixedString s_mainMenu{ "MainMenu" };

		const auto ui = RE::UI::GetSingleton();
		if (!ui || ui->IsMenuOpen(s_loadingMenu) || ui->IsMenuOpen(s_mainMenu))
			return;

		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto ship = player ? player->GetSpaceship() : nullptr;

		std::uint32_t planetID = 0;
		if (const auto planets = RE::BGSPlanet::Manager::GetSingleton())
			planetID = planets->currentPlanetFormId;

		if (ship) {
			// IsInSpace takes a flag whose meaning is undocumented, so log it
			// both ways once per beat rather than guess which one is wanted.
			REX::INFO("[state] ship={:08X} inSpace(true)={} inSpace(false)={} docked={} landed={} planet={:08X}",
				ship->GetFormID(),
				ship->IsInSpace(true),
				ship->IsInSpace(false),
				ship->IsSpaceshipDocked(),
				ship->IsSpaceshipLanded(),
				planetID);
		} else {
			REX::INFO("[state] not piloting a ship, planet={:08X}", planetID);
		}

		for (const auto* name : kProbeMenus) {
			const RE::BSFixedString menuName{ name };
			if (ui->IsMenuOpen(menuName))
				REX::INFO("[state] menu open: {}", name);
		}
	}

	void OnFrame()
	{
		TryInstallInputTap();
		TryInstallCameraTap();

		// Only the subscription bootstrap happens here, and only once the world
		// has settled. Everything else that touches the movie - creating the
		// arrow, reading cruise state, moving the label - now runs inside the
		// data-feed callbacks instead, on the thread the engine itself uses to
		// drive the UI. Doing that work from this task was a cross-thread
		// access on Scaleform objects, which are not thread-safe.
		if (WorldSettled())
			TryInstallSubscriber();

		// Single-winner exchange: the dump is expensive and must not run twice
		// concurrently if this task lands on two threads in the same frame.
		if (g_dumpRequested.exchange(false, std::memory_order_acq_rel)) {
			static const RE::BSFixedString s_loadingMenu{ "LoadingMenu" };
			static const RE::BSFixedString s_mainMenu{ "MainMenu" };
			const auto ui = RE::UI::GetSingleton();
			if (ui && !ui->IsMenuOpen(s_loadingMenu) && !ui->IsMenuOpen(s_mainMenu))
				DumpShipHudDataModel();
		}

		// Phase 6 probe A. Here rather than in a feed callback on purpose - see
		// the header above ProbeSurveyVM. The single-winner exchange is the same
		// protection the dump gets: this task can land on two threads in one
		// frame, and two batches of VM dispatches at once is not a thing to find
		// out about the hard way.
		if (g_surveyVmProbeRequested.exchange(false, std::memory_order_acq_rel) && WorldSettled())
			ProbeSurveyVM();

		// The survey sweep (Phase 6). Same place as the probe and for the same
		// reason - a feed callback is inside Scaleform's locks and this reaches
		// into the Papyrus VM.
		//
		// ⚠ Gated on the panel being OPEN, not merely on cruising. RefreshPanel
		// is the only consumer of this data and it returns before the row loop
		// unless the panel is open, so a cruise-only gate meant the mod bound a
		// Papyrus object to every planet of every system it passed through to
		// fill a map nothing would ever read - turning the mod's single
		// save-state exception from "the cost of using the feature" into "the
		// cost of having the mod installed". PHASE6's own plan said "panel
		// open"; the first cut dropped it. Opening the panel zeroes the throttle
		// (see TogglePanel), so the first sweep still lands immediately.
		if (bPanelSurveyMarks.GetValue() && bPanel.GetValue() &&
			g_panelOpen.load(std::memory_order_acquire) &&
			g_inCruise.load(std::memory_order_acquire) && WorldSettled())
			SweepSurveyState();

		if (bLogHeartbeat.GetValue())
			LogHeartbeat();
	}

	void OnMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		// Only the data-load messages are ever dispatched: SFSE 0.2.21 compiles
		// out its serialization hooks, so the save-lifecycle messages (types
		// 4-7) exist in the API but never arrive.
		if (a_msg->type != SFSE::MessagingInterface::kPostDataLoad)
			return;

		const auto iniStore = REX::FIniSettingStore::GetSingleton();
		iniStore->Init("Data/SFSE/Plugins/ShipNavPanel.ini", "Data/SFSE/Plugins/ShipNavPanelCustom.ini");
		iniStore->Load();

		// bGateOnFlightState is in here because a DISABLED gate logs nothing at
		// all - without this line, "no flight gate lines" is ambiguous between
		// "switched off" and "never reached".
		REX::INFO("config: bPanel={} bInputTap={} bWheelFilter={} bHideVanillaBlips={} "
				  "bPanelSurveyMarks={} bVerboseLog={} bGateOnFlightState={}",
			bPanel.GetValue(), bInputTap.GetValue(), bWheelFilter.GetValue(),
			bHideVanillaBlips.GetValue(), bPanelSurveyMarks.GetValue(), bVerboseLog.GetValue(),
			bGateOnFlightState.GetValue());
		REX::INFO("config: sConfirmEvent='{}' (bPanelHints={}) - the footer pill resolves the key from "
				  "the binding itself; sConfirmKeyLabel='{}' is only used if that pill cannot be built",
			sConfirmEvent.GetValue(), bPanelHints.GetValue(), sConfirmKeyLabel.GetValue());

		// The diagnostics dump earns its space only when something is on. A
		// quiet build says so in one line, and says where the switch is.
		const bool anyRecon = bLogInput.GetValue() || bLogInputHeldFrames.GetValue() ||
		                      bLogInputNonButton.GetValue() || bLogMenus.GetValue() ||
		                      bLogHeartbeat.GetValue() || bVerifyVTableID.GetValue() ||
		                      bSuppressThrottleTest.GetValue() || bSurveyCruiseKeys.GetValue() ||
		                      bScaleformReader.GetValue() || bLogTargetCaptures.GetValue() ||
		                      bDumpPlanetRecords.GetValue() || bProbeStarmapFeed.GetValue() ||
		                      bProbeSurveyVM.GetValue() || bProbeVanillaChrome.GetValue() ||
		                      bTestGraphicsClear.GetValue();
		if (anyRecon) {
			REX::INFO("config: bLogInput={} bLogInputHeldFrames={} bLogInputNonButton={} uMaxInputLines={} "
					  "bLogMenus={} bLogHeartbeat={} fHeartbeatSeconds={} bVerifyVTableID={} bSuppressThrottleTest={}",
				bLogInput.GetValue(), bLogInputHeldFrames.GetValue(), bLogInputNonButton.GetValue(),
				uMaxInputLines.GetValue(), bLogMenus.GetValue(), bLogHeartbeat.GetValue(),
				fHeartbeatSeconds.GetValue(), bVerifyVTableID.GetValue(), bSuppressThrottleTest.GetValue());
			REX::INFO("config: bSurveyCruiseKeys={} bScaleformReader={} bLogTargetCaptures={} "
					  "bDumpPlanetRecords={} bProbeStarmapFeed={} bProbeSurveyVM={} bProbeVanillaChrome={} "
					  "bTestGraphicsClear={}",
				bSurveyCruiseKeys.GetValue(), bScaleformReader.GetValue(), bLogTargetCaptures.GetValue(),
				bDumpPlanetRecords.GetValue(), bProbeStarmapFeed.GetValue(), bProbeSurveyVM.GetValue(),
				bProbeVanillaChrome.GetValue(), bTestGraphicsClear.GetValue());
		} else if (bVerboseLog.GetValue()) {
			// The default since 1.1.2. bVerboseLog is a [Recon] switch but is
			// deliberately NOT part of anyRecon above - it is the one level meant
			// to be left on, so it must not drag the heavy dump in with it.
			REX::INFO("config: per-action trace on, heavy [Recon] instrumentation off. This is the "
					  "default and it is what a bug report wants - attach this log whole.");
		} else {
			// Only reachable if someone turned the trace off by hand.
			REX::INFO("config: diagnostics all off, including the per-action trace. For a bug report, "
					  "set bVerboseLog=true in ShipNavPanelCustom.ini and reproduce - that restores the "
					  "trace without the thousands of lines the [Recon] switches produce.");
		}

		if (bProbeSurveyVM.GetValue())
			REX::INFO("[surveyed] probe A ON - in cruise, press the scanner key to dispatch "
					  "Planet.GetSurveyPercent for every listed body. Read the '[surveyed]' lines: "
					  "step 2 must name 'Planet', and a MOON must answer with a float.");
		if (bProbeStarmapFeed.GetValue() && sStarmapFeed.GetValue() == "InfoTargetProvider")
			REX::INFO("[surveyed] probe B ON - '[surveyed] card' lines print whenever the info "
					  "target's dossier changes; the scanner key dumps PlanetCardInfo in full "
					  "beside the entry it belongs to.");

		if (!bWheelFilter.GetValue())
			REX::WARN("bWheelFilter is off - scrolling the list will also swing your point of view");

		// The course lock is the mod's only outward word to the engine -
		// everything else it does is a read or a draw - so the log says plainly
		// that it is armed and on which key.
		if (bLockCourse.GetValue())
			REX::INFO("[course] course lock ON ('{}') - in cruise WITH THE PANEL OPEN that key aims "
					  "the autopilot at the HIGHLIGHTED body instead of the info target. With the "
					  "panel closed it is the game's own key doing the game's own job.",
				sLockCourseEvent.GetValue());
		else
			REX::INFO("[course] course lock OFF - the panel points, and steering stays manual");

		if (bSurveyCruiseKeys.GetValue())
			REX::INFO("[survey] cruise key survey ON - enter cruise, then press every key you can spare. "
					  "Each new one prints a line; the recap prints when you leave cruise.");

		if (!bInputTap.GetValue())
			REX::WARN("bInputTap is off - the scanner key cannot reach the mod, so no body will be "
					  "selected and the arrow will never appear");
		if (bSuppressThrottleTest.GetValue())
			REX::INFO("[suppress] throttle-suppression test ON - in cruise, the scanner key toggles the "
					  "panel state instead of cycling targets");

		// The movie-created callback is NOT diagnostics, whatever its position
		// in this function once suggested: it drops the stale Scaleform handles
		// when the HUD's movie is rebuilt, which happens often. Registering it
		// only when menu logging was on - as this did - meant a shipped build
		// kept writing rotation into a destroyed movie's clip. It registers
		// always now, and logs only if asked.
		if (const auto menus = SFSE::GetMenuInterface()) {
			menus->Register(&OnMenuMovieCreated);
			REX::INFO("[menu] SFSE movie-created callback registered");
		} else {
			REX::WARN("[menu] SFSE menu interface unavailable; movie-created callback not registered "
					  "- the arrow will not survive a HUD movie rebuild");
		}

		// The open/close sink, by contrast, only ever logs.
		if (bLogMenus.GetValue()) {
			if (const auto ui = RE::UI::GetSingleton()) {
				ui->RegisterSink(&g_menuSink);
				REX::INFO("[menu] open/close sink registered");
			} else {
				REX::WARN("[menu] UI singleton unavailable; open/close sink not registered");
			}
		}

		LoadBodyTable();

		SFSE::GetTaskInterface()->AddPermanentTask(OnFrame);
		REX::INFO("per-frame task registered");
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
	SFSE::GetMessagingInterface()->RegisterListener(OnMessage);
	REX::INFO("{} loaded", SFSE::GetPluginName());
	return true;
}
