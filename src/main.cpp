#include "SFSE/SFSE.h"

#include "REX/TIniSetting.h"

#include "RE/Starfield.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
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
	REX::TIniSetting<bool>        bPanel{ "Panel", "bPanel", true };
	REX::TIniSetting<std::string> sConfirmEvent{ "Panel", "sConfirmEvent", "XButton" };

	// The control hint along the bottom. The label is a separate setting because
	// the mod knows the confirm key's user-event NAME, not which physical key it
	// is bound to - `XButton` happens to be R on the bindings this was built
	// against, and anyone who has moved it needs to say so here.
	REX::TIniSetting<bool>        bPanelHints{ "Panel", "bPanelHints", true };
	REX::TIniSetting<std::string> sConfirmKeyLabel{ "Panel", "sConfirmKeyLabel", "R" };

	// Hide the mouse wheel from the camera while the panel is open, so scrolling
	// the list does not swing the point of view. Verified in game (v0.2.3); the
	// switch remains as an escape hatch, not because it is experimental.
	REX::TIniSetting<bool> bWheelFilter{ "Panel", "bWheelFilter", true };

	REX::TIniSetting<float>         fPanelOffsetX{ "Panel", "fPanelOffsetX", -540.0f };
	REX::TIniSetting<float>         fPanelOffsetY{ "Panel", "fPanelOffsetY", -160.0f };
	REX::TIniSetting<float>         fPanelWidth{ "Panel", "fPanelWidth", 340.0f };
	REX::TIniSetting<float>         fPanelRowHeight{ "Panel", "fPanelRowHeight", 26.0f };
	REX::TIniSetting<std::uint32_t> uPanelMaxRows{ "Panel", "uPanelMaxRows", 10 };

	// The pointer arrow.
	REX::TIniSetting<bool>  bArrow{ "Panel", "bArrow", true };
	REX::TIniSetting<float> fArrowRadius{ "Panel", "fArrowRadius", 150.0f };
	REX::TIniSetting<float> fMaxTargetLightSeconds{ "Panel", "fMaxTargetLightSeconds", 20000.0f };
	REX::TIniSetting<float> fArrowAngleOffset{ "Panel", "fArrowAngleOffset", 0.0f };
	REX::TIniSetting<bool>  bArrowInvertAngle{ "Panel", "bArrowInvertAngle", false };
	REX::TIniSetting<bool>  bLabel{ "Panel", "bLabel", true };

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
	std::atomic<bool> g_captureRequested{ false };
	std::atomic<bool> g_captureHighRequested{ false };
	std::atomic<bool> g_interposeInstalled{ false };
	std::atomic<bool> g_interposeFailed{ false };
	std::atomic<bool> g_subscribed{ false };
	std::atomic<bool> g_subscribeFailed{ false };

	constexpr const char* kShipHudMenu = "SpaceshipHudMenu";

	// One entry per feed slot, index-aligned with both feeds: the low-frequency
	// one supplies id/type/name, the high-frequency one distance and angle. That
	// alignment is how a name gets matched to a bearing.
	struct Candidate
	{
		std::uint32_t id{ 0 };
		std::uint32_t type{ 0 };
		double        distance{ 0.0 };
		std::string   name;
	};

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
	RE::Scaleform::GFx::Value  g_labelField;
	RE::Scaleform::GFx::Value  g_labelFormat;
	std::atomic<bool>          g_labelReady{ false };

	// Cruise state. The scanner key keeps its vanilla job outside cruise, so the
	// panel must stay out of the way there - the gate identified back in Phase 0.
	std::atomic<bool> g_inCruise{ false };

	// The panel is open. While this is set the wheel is hidden from the camera
	// and drives the highlight instead.
	std::atomic<bool>          g_panelOpen{ false };
	std::atomic<std::uint32_t> g_suppressedCount{ 0 };
	std::atomic<std::uint32_t> g_wheelRemovedCount{ 0 };

	// The drawn list. Row count is fixed at creation - growing it would mean
	// building TextFields from a feed callback, and every AS3 construction is a
	// risk worth taking exactly once, at startup.
	constexpr std::size_t      kPanelMaxRowsHard = 16;
	RE::Scaleform::GFx::Value  g_panelClip;
	RE::Scaleform::GFx::Value  g_panelHighlight;
	RE::Scaleform::GFx::Value  g_panelRows[kPanelMaxRowsHard];
	RE::Scaleform::GFx::Value  g_panelFormat;
	RE::Scaleform::GFx::Value  g_panelHint;
	RE::Scaleform::GFx::Value  g_panelHintFormat;
	std::atomic<bool>          g_panelReady{ false };
	std::atomic<bool>          g_panelFailed{ false };
	std::atomic<std::uint32_t> g_panelRowCount{ 0 };

	// Defined further down, but called from the data-feed callbacks above them.
	bool WorldSettled();
	void TryCreateArrow();
	void TryCreatePanel();
	void RefreshPanel();
	void RefreshCruiseState();

	// A TT_STAR entry is not necessarily *this* system's star: a quest-marked one
	// showed up at 8.21e17 m, about 87 light-years. Type alone is not a filter.
	constexpr double kMetersPerLightSecond = 299792458.0;

	bool IsLocalBody(std::uint32_t a_type, double a_distanceMeters)
	{
		if (a_type != 7 /* TT_PLANET */ && a_type != 1 /* TT_STAR */)
			return false;
		return a_distanceMeters <= static_cast<double>(fMaxTargetLightSeconds.GetValue()) * kMetersPerLightSecond;
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

	// The mouse wheel, which the cruise survey found arriving undisabled. It
	// drives the camera's point of view rather than anything about flight.
	constexpr const char* kWheelUpEvent = "ZoomIn";
	constexpr const char* kWheelDownEvent = "ZoomOut";

	bool IsWheelEvent(const char* a_userEvent)
	{
		return a_userEvent && (std::strcmp(a_userEvent, kWheelUpEvent) == 0 ||
								  std::strcmp(a_userEvent, kWheelDownEvent) == 0);
	}

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
	// The panel cannot take W/S from the ship - v0.2.1 settled that - so it has
	// to be built on keys the game already ignores in cruise. `SHMonocle` is one
	// such, found by accident in Phase 0. This looks for the others.
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

	// Caller holds g_candidateMutex.
	void CollectLocalRows(std::vector<std::size_t>& a_out)
	{
		a_out.clear();
		for (std::size_t i = 0; i < g_candidates.size(); ++i) {
			const auto& row = g_candidates[i];
			if (IsLocalBody(row.type, row.distance))
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
			g_wheelRemovedCount.store(0, std::memory_order_release);
			REX::INFO("[panel] opened - wheel moves the highlight, '{}' locks or clears it",
				sConfirmEvent.GetValue());
			if (bSuppressThrottleTest.GetValue())
				REX::INFO("[panel]   throttle-suppression test armed");
		} else {
			// Nothing is committed on close - that is the whole point of the
			// confirm key.
			REX::INFO("[panel] closed (locked stays {:08X}, {} wheel events hidden)",
				g_lockedID.load(std::memory_order_acquire),
				g_wheelRemovedCount.load(std::memory_order_acquire));
		}
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

			if (down && firstFrame && userEvent) {
				if (std::strcmp(userEvent, kDumpTriggerEvent) == 0) {
					OnTriggerPressed();
				} else if (g_panelOpen.load(std::memory_order_acquire) &&
						   g_inCruise.load(std::memory_order_acquire)) {
					// The wheel is spliced away from the camera elsewhere; here
					// it is simply read. One notch is one step.
					if (std::strcmp(userEvent, kWheelUpEvent) == 0) {
						MoveHighlight(-1);
						REX::INFO("[panel] highlight up -> {:08X}", g_highlightID.load(std::memory_order_acquire));
					} else if (std::strcmp(userEvent, kWheelDownEvent) == 0) {
						MoveHighlight(1);
						REX::INFO("[panel] highlight down -> {:08X}", g_highlightID.load(std::memory_order_acquire));
					} else if (sConfirmEvent.GetValue() == userEvent) {
						ConfirmHighlight();
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

	void PerformInputProcessingHook(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queueHead)
	{
		ProcessInputQueue(a_queueHead);

		if (const auto original = g_origPerformInputProcessing.load(std::memory_order_acquire))
			original(a_this, a_queueHead);
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

		// The wheel is hidden from the camera whenever the panel is open - this
		// is the shipped behaviour now, not a test. The setting survives only as
		// an escape hatch if the camera hook ever misbehaves.
		const bool filtering = bWheelFilter.GetValue() &&
		                       g_panelOpen.load(std::memory_order_acquire) &&
		                       g_inCruise.load(std::memory_order_acquire);

		if (!filtering) {
			if (original)
				original(a_this, a_queueHead);
			return;
		}

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
				drop = IsWheelEvent(button->strUserEvent.c_str());
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
			const auto total = g_wheelRemovedCount.fetch_add(removed, std::memory_order_relaxed) + removed;
			REX::INFO("[wheel] hid {} wheel event(s) from the camera (total {})", removed, total);
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
			g_interposeInstalled.store(false, std::memory_order_release);
			g_interposeFailed.store(false, std::memory_order_release);
			g_subscribed.store(false, std::memory_order_release);
			g_subscribeFailed.store(false, std::memory_order_release);
			g_subscribeAttempts.store(0, std::memory_order_release);

			// The arrow belonged to the old movie. Without this the plugin keeps
			// writing rotation to a clip whose movie has been destroyed - the log
			// showed exactly that, two subscription rounds against one arrow.
			g_arrowReady.store(false, std::memory_order_release);
			g_arrowFailed.store(false, std::memory_order_release);
			g_arrowClip = RE::Scaleform::GFx::Value{};
			g_labelField = RE::Scaleform::GFx::Value{};
			g_labelFormat = RE::Scaleform::GFx::Value{};
			g_labelReady.store(false, std::memory_order_release);

			// The list belonged to the old movie too.
			g_panelReady.store(false, std::memory_order_release);
			g_panelFailed.store(false, std::memory_order_release);
			g_panelClip = RE::Scaleform::GFx::Value{};
			g_panelHighlight = RE::Scaleform::GFx::Value{};
			g_panelFormat = RE::Scaleform::GFx::Value{};
			g_panelHint = RE::Scaleform::GFx::Value{};
			g_panelHintFormat = RE::Scaleform::GFx::Value{};
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

			// The entry schema is not documented anywhere, so spell the first
			// one out in full - that is how the remaining field names are found.
			if (a_index == 0) {
				REX::INFO("[nav] --- full schema of entry 0 ---");
				LevelCollector visitor{ "[nav] entry0", nullptr };
				entry.VisitMembers(&visitor);
				REX::INFO("[nav] --- end schema ---");
			}
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
			if (rows.size() <= a_index)
				rows.resize(a_index + 1);
			rows[a_index] = std::move(row);
		}
	};

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
				RefreshCruiseState();
			}

			// Rebuild the candidate list on every update - this is what the arrow
			// points at, so it cannot be gated on a capture request. The list is
			// index-aligned with the high-frequency feed, which is how a name gets
			// matched to an angle.
			RE::Scaleform::GFx::Value entries;
			if (GetEntryArray(data, entries)) {
				CandidateCollector collector;
				entries.VisitElements(&collector);
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
			if (rows.size() <= a_index)
				rows.resize(a_index + 1);
			rows[a_index] = row;
		}
	};

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

			bool        haveSelected = false;
			double      selectedAngle = 0.0;
			double      selectedDistance = 0.0;
			std::string selectedName;
			std::string labelText;

			{
				std::lock_guard lock{ g_candidateMutex };
				const auto      count = std::min(g_candidates.size(), bearings.rows.size());
				for (std::size_t i = 0; i < count; ++i)
					g_candidates[i].distance = bearings.rows[i].distance;

				// The arrow follows the highlight while the panel is open - that
				// is the preview - and falls back to the locked body once it
				// closes. Closing without confirming therefore reverts it, which
				// is exactly what the confirm key is for.
				const auto selected = g_panelOpen.load(std::memory_order_acquire) ?
				                          g_highlightID.load(std::memory_order_acquire) :
				                          g_lockedID.load(std::memory_order_acquire);

				for (std::size_t i = 0; i < count; ++i) {
					if (selected != 0 && g_candidates[i].id == selected) {
						haveSelected = true;
						selectedAngle = bearings.rows[i].angle;
						selectedDistance = bearings.rows[i].distance;
						selectedName = g_candidates[i].name;
						break;
					}
				}
			}

			if (g_arrowReady.load(std::memory_order_acquire)) {
				using V = RE::Scaleform::GFx::Value;
				haveSelected = haveSelected && g_inCruise.load(std::memory_order_acquire);
				g_arrowClip.SetMember("visible", V{ haveSelected });
				if (haveSelected) {
					// Vanilla uses `angle + 180` because its icon art points the
					// other way by default; this triangle is drawn tip-up, so a
					// target dead ahead (angle 0) wants rotation 0, not 180.
					// Kept tunable because the sign convention is unverified.
					const double rotation = selectedAngle * (bArrowInvertAngle.GetValue() ? -1.0 : 1.0) +
					                        static_cast<double>(fArrowAngleOffset.GetValue());
					g_arrowClip.SetMember("rotation", V{ rotation });

					if (g_labelReady.load(std::memory_order_acquire)) {
						// Place the label just beyond the arrow tip, on the same
						// bearing, but never rotate it - rotated text is unreadable.
						const double radians = rotation * 3.14159265358979323846 / 180.0;
						const double radius = static_cast<double>(fArrowRadius.GetValue()) + 34.0;
						const double lightSeconds = selectedDistance / kMetersPerLightSecond;
						labelText = lightSeconds >= 1.0 ?
						                std::format("{}  {:.0f} LS", selectedName, lightSeconds) :
						                std::format("{}  {:.0f} km", selectedName, selectedDistance / 1000.0);

						g_labelField.SetMember("text", V{ labelText.c_str() });
						// defaultTextFormat only applies to text present when it
						// was set, so re-apply after each assignment or the new
						// characters fall back to no font.
						if (g_labelFormat.IsObject())
							g_labelField.Invoke("setTextFormat", nullptr, &g_labelFormat, 1);
						g_labelField.SetMember("x", V{ radius * std::sin(radians) });
						g_labelField.SetMember("y", V{ -radius * std::cos(radians) });
					}

					// Rate-limited so the bearing can be correlated with what is
					// actually on screen without flooding the log.
					static std::atomic<std::uint32_t> s_tick{ 0 };
					if ((s_tick.fetch_add(1, std::memory_order_relaxed) % 120) == 0)
						REX::INFO("[arrow] angleToCrosshair={:.1f} -> rotation={:.1f}", selectedAngle, rotation);
				}
				if (g_labelReady.load(std::memory_order_acquire))
					g_labelField.SetMember("visible", V{ haveSelected });
			}

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

	void TryInstallSubscriber()
	{
		if (!bInterposeTargetData.GetValue() ||
			g_subscribed.load(std::memory_order_acquire) ||
			g_subscribeFailed.load(std::memory_order_acquire))
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
			const bool available = a_root->IsAvailable(path);
			RE::Scaleform::GFx::Value manager;
			const bool resolved = a_root->GetVariable(&manager, path);

			if (verbose)
				REX::INFO("[nav] probe '{}': IsAvailable={} GetVariable={} value={}",
					path, available, resolved, resolved ? DescribeValue(manager) : "-");

			if (resolved && (manager.IsObject() || manager.IsDisplayObject())) {
				anyResolved = true;
				if (manager.HasMember("Subscribe")) {
					RE::Scaleform::GFx::Value args[2];
					a_root->CreateString(&args[0], kTargetFeed);
					a_root->CreateFunction(&args[1], &g_feedHandler);
					if (manager.Invoke("Subscribe", nullptr, args, 2)) {
						g_subscribed.store(true, std::memory_order_release);
						REX::INFO("[nav] SUBSCRIBED to '{}' via {}.Subscribe", kTargetFeed, path);

						// The screen positions live on the other feed.
						RE::Scaleform::GFx::Value hiArgs[2];
						a_root->CreateString(&hiArgs[0], kHighFeed);
						a_root->CreateFunction(&hiArgs[1], &g_highFeedHandler);
						REX::INFO("[nav] {} to '{}'",
							manager.Invoke("Subscribe", nullptr, hiArgs, 2) ? "SUBSCRIBED" : "FAILED to subscribe",
							kHighFeed);
						return;
					}
					REX::WARN("[nav] '{}.Subscribe' rejected the call", path);
				} else if (verbose) {
					REX::INFO("[nav]   no 'Subscribe' member; its members follow:");
					LevelCollector visitor{ std::string{ "[nav] " } + path, nullptr };
					RE::Scaleform::GFx::Value copy = manager;
					copy.VisitMembers(&visitor);
				}
			}

			// Path-based Invoke resolves differently from GetVariable, so a
			// class the latter cannot see may still be callable this way.
			RE::Scaleform::GFx::Value args[2];
			a_root->CreateString(&args[0], kTargetFeed);
			a_root->CreateFunction(&args[1], &g_feedHandler);
			const std::string method = std::string{ path } + ".Subscribe";
			if (a_root->Invoke(method.c_str(), nullptr, args, 2)) {
				g_subscribed.store(true, std::memory_order_release);
				REX::INFO("[nav] SUBSCRIBED to '{}' via path-invoke '{}'", kTargetFeed, method);
				return;
			}
			if (verbose)
				REX::INFO("[nav]   path-invoke '{}' failed too", method);
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
		return ui && !ui->IsMenuOpen(s_loadingMenu) && !ui->IsMenuOpen(s_mainMenu);
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
							  "{} wheel events hidden)",
						g_suppressedCount.load(std::memory_order_acquire),
						g_wheelRemovedCount.load(std::memory_order_acquire));
				// Recap the survey while the cruise it describes is still fresh.
				if (bSurveyCruiseKeys.GetValue())
					SurveyDump();
			}
		}
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

	void TryCreatePanel()
	{
		if (!bPanel.GetValue() || g_panelReady.load(std::memory_order_acquire) ||
			g_panelFailed.load(std::memory_order_acquire))
			return;
		if (!g_subscribed.load(std::memory_order_acquire))
			return;  // no feed yet, so nothing to list

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

		using V = RE::Scaleform::GFx::Value;

		// Depth 20001 puts the list above the arrow's 20000.
		if (!reticle.CreateEmptyMovieClip(&g_panelClip, "ShipNavPanelList", 20001)) {
			giveUp("CreateEmptyMovieClip refused a container for the list");
			return;
		}

		const auto   rows = std::clamp<std::size_t>(uPanelMaxRows.GetValue(), 1, kPanelMaxRowsHard);
		const double rowHeight = static_cast<double>(fPanelRowHeight.GetValue());
		const double width = static_cast<double>(fPanelWidth.GetValue());

		const bool   hints = bPanelHints.GetValue();
		const double listBottom = 6.0 + rowHeight * static_cast<double>(rows);
		const double hintHeight = 22.0;
		const double hintTop = listBottom + 6.0;
		const double height = hints ? (hintTop + hintHeight + 4.0) : (listBottom + 6.0);

		// Background: a flat panel at 55% so the starfield still reads through.
		RE::Scaleform::GFx::Value gfx;
		if (!g_panelClip.GetMember("graphics", &gfx)) {
			giveUp("the list container has no 'graphics' member to draw into");
			return;
		}
		V bgFill[]{ V{ static_cast<std::uint32_t>(0x0A1420) }, V{ 0.55 } };
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

			// Two small triangles, up then down: the wheel, both ways.
			const double midY = hintTop + hintHeight * 0.5;
			const auto   triangle = [&](double a_cx, bool a_up) {
                V fill[]{ V{ static_cast<std::uint32_t>(0x99D6FF) }, V{ 0.85 } };
                gfx.Invoke("beginFill", nullptr, fill, 2);
                const double tip = a_up ? midY - 4.5 : midY + 4.5;
                const double base = a_up ? midY + 3.0 : midY - 3.0;
                V p0[]{ V{ a_cx }, V{ tip } };
                gfx.Invoke("moveTo", nullptr, p0, 2);
                V p1[]{ V{ a_cx - 5.0 }, V{ base } };
                gfx.Invoke("lineTo", nullptr, p1, 2);
                V p2[]{ V{ a_cx + 5.0 }, V{ base } };
                gfx.Invoke("lineTo", nullptr, p2, 2);
                gfx.Invoke("lineTo", nullptr, p0, 2);
                gfx.Invoke("endFill", nullptr, nullptr, 0);
			};
			triangle(17.0, true);
			triangle(32.0, false);
			hintTextX = 44.0;
		}

		// The highlight is its own clip so moving it is one property write per
		// wheel notch rather than a redraw.
		if (g_panelClip.CreateEmptyMovieClip(&g_panelHighlight, "Highlight", 1)) {
			RE::Scaleform::GFx::Value hlGfx;
			if (g_panelHighlight.GetMember("graphics", &hlGfx)) {
				V hlFill[]{ V{ static_cast<std::uint32_t>(0x66CCFF) }, V{ 0.28 } };
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

		const bool haveFormat = BorrowTextFormat(root, rootPath, g_panelFormat, "[panel]");
		if (haveFormat) {
			g_panelFormat.SetMember("size", V{ 17.0 });
			g_panelFormat.SetMember("bold", V{ false });
			g_panelFormat.SetMember("color", V{ static_cast<std::uint32_t>(0xCCE6FF) });
		} else {
			REX::WARN("[panel] no donor TextField found - rows will likely render as boxes");
		}

		std::size_t made = 0;
		for (std::size_t i = 0; i < rows; ++i) {
			auto& field = g_panelRows[i];
			root->CreateObject(&field, "flash.text.TextField");
			if (!field.IsObject() && !field.IsDisplayObject())
				break;

			field.SetMember("selectable", V{ false });
			field.SetMember("mouseEnabled", V{ false });
			field.SetMember("multiline", V{ false });
			field.SetMember("width", V{ width - 16.0 });
			field.SetMember("height", V{ rowHeight });
			field.SetMember("x", V{ 10.0 });
			field.SetMember("y", V{ 6.0 + rowHeight * static_cast<double>(i) });
			if (haveFormat) {
				field.SetMember("embedFonts", V{ true });
				field.SetMember("defaultTextFormat", g_panelFormat);
			} else {
				field.SetMember("textColor", V{ static_cast<std::uint32_t>(0xCCE6FF) });
			}

			RE::Scaleform::GFx::Value added;
			if (!g_panelClip.Invoke("addChild", &added, &field, 1))
				break;
			field.SetMember("visible", V{ false });
			++made;
		}

		if (made == 0) {
			giveUp("could not create a single row TextField");
			return;
		}
		if (made < rows)
			REX::WARN("[panel] only {} of {} rows could be created", made, rows);

		// The hint is static text, so it is written once here rather than on
		// every refresh.
		if (hints) {
			root->CreateObject(&g_panelHint, "flash.text.TextField");
			if (g_panelHint.IsObject() || g_panelHint.IsDisplayObject()) {
				g_panelHint.SetMember("selectable", V{ false });
				g_panelHint.SetMember("mouseEnabled", V{ false });
				g_panelHint.SetMember("multiline", V{ false });
				g_panelHint.SetMember("width", V{ width - hintTextX - 8.0 });
				g_panelHint.SetMember("height", V{ hintHeight });
				g_panelHint.SetMember("x", V{ hintTextX });
				g_panelHint.SetMember("y", V{ hintTop + 2.0 });

				if (BorrowTextFormat(root, rootPath, g_panelHintFormat, "[panel-hint]")) {
					g_panelHintFormat.SetMember("size", V{ 14.0 });
					g_panelHintFormat.SetMember("bold", V{ false });
					g_panelHintFormat.SetMember("color", V{ static_cast<std::uint32_t>(0x7FA8C4) });
					g_panelHint.SetMember("embedFonts", V{ true });
					g_panelHint.SetMember("defaultTextFormat", g_panelHintFormat);
				} else {
					g_panelHint.SetMember("textColor", V{ static_cast<std::uint32_t>(0x7FA8C4) });
				}

				const auto hintText = std::format("browse     {}  lock / clear", sConfirmKeyLabel.GetValue());
				g_panelHint.SetMember("text", V{ hintText.c_str() });
				if (g_panelHintFormat.IsObject())
					g_panelHint.Invoke("setTextFormat", nullptr, &g_panelHintFormat, 1);

				RE::Scaleform::GFx::Value added;
				if (!g_panelClip.Invoke("addChild", &added, &g_panelHint, 1))
					REX::WARN("[panel] hint addChild failed - the list will run without it");
			} else {
				REX::WARN("[panel] could not create the hint TextField - the list will run without it");
			}
		}

		g_panelClip.SetMember("x", V{ static_cast<double>(fPanelOffsetX.GetValue()) });
		g_panelClip.SetMember("y", V{ static_cast<double>(fPanelOffsetY.GetValue()) });
		g_panelClip.SetMember("visible", V{ false });

		g_panelRowCount.store(static_cast<std::uint32_t>(made), std::memory_order_release);
		g_panelReady.store(true, std::memory_order_release);
		REX::INFO("[panel] ready - {} rows at ({}, {})", made,
			fPanelOffsetX.GetValue(), fPanelOffsetY.GetValue());
	}

	// Called from the high-frequency feed, on the UI thread, with distances
	// already refreshed. Row text is rebuilt at a few hertz rather than every
	// tick - distances crawl, and ten TextField writes per frame is a cost with
	// nothing to show for it. The highlight moves immediately, because that is
	// the part the player is waiting on.
	void RefreshPanel()
	{
		if (!g_panelReady.load(std::memory_order_acquire))
			return;

		using V = RE::Scaleform::GFx::Value;

		const bool open = g_panelOpen.load(std::memory_order_acquire) &&
		                  g_inCruise.load(std::memory_order_acquire);
		g_panelClip.SetMember("visible", V{ open });
		if (!open)
			return;

		using clock = std::chrono::steady_clock;
		static auto s_lastText = clock::time_point{};
		const auto  now = clock::now();
		const bool  refreshText = std::chrono::duration<float>(now - s_lastText).count() >= 0.25f;
		if (refreshText)
			s_lastText = now;

		const auto    rowCount = static_cast<std::size_t>(g_panelRowCount.load(std::memory_order_acquire));
		const auto    highlight = g_highlightID.load(std::memory_order_acquire);
		const auto    locked = g_lockedID.load(std::memory_order_acquire);
		const double  rowHeight = static_cast<double>(fPanelRowHeight.GetValue());
		std::size_t   highlightRow = 0;
		bool          haveHighlightRow = false;

		{
			std::lock_guard          lock{ g_candidateMutex };
			std::vector<std::size_t> local;
			CollectLocalRows(local);

			// Scroll so the highlight stays on screen once the system has more
			// bodies than the panel has rows.
			std::size_t first = 0;
			for (std::size_t n = 0; n < local.size(); ++n) {
				if (g_candidates[local[n]].id == highlight) {
					if (n >= rowCount)
						first = n - rowCount + 1;
					break;
				}
			}

			for (std::size_t r = 0; r < rowCount; ++r) {
				const std::size_t n = first + r;
				auto&             field = g_panelRows[r];
				if (n >= local.size()) {
					field.SetMember("visible", V{ false });
					continue;
				}

				const auto& row = g_candidates[local[n]];
				if (row.id == highlight) {
					highlightRow = r;
					haveHighlightRow = true;
				}

				if (refreshText) {
					// The locked body is marked in the list itself, so the
					// panel says what the HUD is showing without the player
					// having to close it and look.
					const char* mark = (locked != 0 && row.id == locked) ? "> " : "  ";
					const auto  text = row.distance > 0.0 ?
					                       std::format("{}{}  {:.0f} km", mark, row.name, row.distance / 1000.0) :
					                       std::format("{}{}", mark, row.name);
					field.SetMember("text", V{ text.c_str() });
					// defaultTextFormat only applies to text present when it was
					// set, so re-apply after every assignment or the new glyphs
					// fall back to no font.
					if (g_panelFormat.IsObject())
						field.Invoke("setTextFormat", nullptr, &g_panelFormat, 1);
				}
				field.SetMember("visible", V{ true });
			}
		}

		if (g_panelHighlight.IsObject() || g_panelHighlight.IsDisplayObject()) {
			g_panelHighlight.SetMember("visible", V{ haveHighlightRow });
			if (haveHighlightRow)
				g_panelHighlight.SetMember("y", V{ 6.0 + rowHeight * static_cast<double>(highlightRow) });
		}
	}

	void TryCreateArrow()
	{
		if (!bArrow.GetValue() || g_arrowReady.load(std::memory_order_acquire) || g_arrowFailed.load(std::memory_order_acquire))
			return;
		if (!g_subscribed.load(std::memory_order_acquire))
			return;  // no feed yet, so nothing to point at

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

		// A triangle with its tip out at radius R, so rotating the clip about its
		// own origin sweeps it around the crosshair pointing outward - the same
		// arrangement the vanilla off-screen indicators use.
		const double r = static_cast<double>(fArrowRadius.GetValue());
		using V = RE::Scaleform::GFx::Value;

		V fill[]{ V{ static_cast<std::uint32_t>(0x66CCFF) }, V{ 1.0 } };
		gfx.Invoke("beginFill", nullptr, fill, 2);
		V p0[]{ V{ 0.0 }, V{ -r } };
		gfx.Invoke("moveTo", nullptr, p0, 2);
		V p1[]{ V{ -11.0 }, V{ -r + 24.0 } };
		gfx.Invoke("lineTo", nullptr, p1, 2);
		V p2[]{ V{ 11.0 }, V{ -r + 24.0 } };
		gfx.Invoke("lineTo", nullptr, p2, 2);
		gfx.Invoke("lineTo", nullptr, p0, 2);
		gfx.Invoke("endFill", nullptr, nullptr, 0);

		g_arrowClip.SetMember("x", V{ 0.0 });
		g_arrowClip.SetMember("y", V{ 0.0 });
		g_arrowClip.SetMember("visible", V{ false });

		// The label rides just outside the arrow tip and is never rotated, so it
		// stays readable however the ship is oriented.
		//
		// OFF BY DEFAULT. Constructing `flash.text.TextField` asks the AS3 VM to
		// resolve a class the SWF may never link, and the v0.1.3 crash died in
		// the VM's TypeError path - so this stays opt-in until it has been shown
		// safe on its own, separately from the threading fix.
		if (!bLabel.GetValue()) {
			g_arrowReady.store(true, std::memory_order_release);
			REX::INFO("[arrow] ready (radius {}), label disabled", r);
			return;
		}
		root->CreateObject(&g_labelField, "flash.text.TextField");
		if (g_labelField.IsObject() || g_labelField.IsDisplayObject()) {
			g_labelField.SetMember("selectable", V{ false });
			g_labelField.SetMember("mouseEnabled", V{ false });
			g_labelField.SetMember("autoSize", V{ "center" });

			if (BorrowTextFormat(root, rootPath, g_labelFormat, "[arrow]")) {
				g_labelFormat.SetMember("size", V{ 22.0 });
				g_labelFormat.SetMember("bold", V{ true });
				g_labelFormat.SetMember("color", V{ static_cast<std::uint32_t>(0x66CCFF) });
				g_labelField.SetMember("embedFonts", V{ true });
				g_labelField.SetMember("defaultTextFormat", g_labelFormat);
			} else {
				g_labelField.SetMember("textColor", V{ static_cast<std::uint32_t>(0x66CCFF) });
				REX::WARN("[arrow] no donor TextField found - the label will likely render as boxes");
			}

			RE::Scaleform::GFx::Value added;
			if (reticle.Invoke("addChild", &added, &g_labelField, 1)) {
				g_labelField.SetMember("visible", V{ false });
				g_labelReady.store(true, std::memory_order_release);
				REX::INFO("[arrow] label created");
			} else {
				REX::WARN("[arrow] label addChild failed - arrow will run without it");
			}
		} else {
			REX::WARN("[arrow] could not create a TextField - arrow will run without a label");
		}

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

		REX::INFO("config: bInputTap={} bArrow={} bLabel={} bPanel={} bWheelFilter={}",
			bInputTap.GetValue(), bArrow.GetValue(), bLabel.GetValue(), bPanel.GetValue(),
			bWheelFilter.GetValue());
		REX::INFO("config: sConfirmEvent='{}' shown as '{}' (bPanelHints={}) - if the hint names the wrong "
				  "key, correct sConfirmKeyLabel; it cannot be derived from the event name",
			sConfirmEvent.GetValue(), sConfirmKeyLabel.GetValue(), bPanelHints.GetValue());
		REX::INFO("config: bLogInput={} bLogInputHeldFrames={} bLogInputNonButton={} uMaxInputLines={} "
				  "bLogMenus={} bLogHeartbeat={} fHeartbeatSeconds={} bVerifyVTableID={} bSuppressThrottleTest={}",
			bLogInput.GetValue(), bLogInputHeldFrames.GetValue(), bLogInputNonButton.GetValue(),
			uMaxInputLines.GetValue(), bLogMenus.GetValue(), bLogHeartbeat.GetValue(),
			fHeartbeatSeconds.GetValue(), bVerifyVTableID.GetValue(), bSuppressThrottleTest.GetValue());
		REX::INFO("config: bSurveyCruiseKeys={}", bSurveyCruiseKeys.GetValue());

		if (!bWheelFilter.GetValue())
			REX::WARN("bWheelFilter is off - scrolling the list will also swing your point of view");

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
