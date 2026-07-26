#include "SFSE/SFSE.h"

#include "REX/TIniSetting.h"

#include "RE/Starfield.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

	REX::TIniSetting<bool>          bLogInput{ "Recon", "bLogInput", true };
	REX::TIniSetting<bool>          bLogInputHeldFrames{ "Recon", "bLogInputHeldFrames", false };
	REX::TIniSetting<bool>          bLogInputNonButton{ "Recon", "bLogInputNonButton", false };
	REX::TIniSetting<std::uint32_t> uMaxInputLines{ "Recon", "uMaxInputLines", 20000 };
	REX::TIniSetting<bool>          bLogMenus{ "Recon", "bLogMenus", true };
	REX::TIniSetting<bool>          bLogHeartbeat{ "Recon", "bLogHeartbeat", true };
	REX::TIniSetting<float>         fHeartbeatSeconds{ "Recon", "fHeartbeatSeconds", 5.0f };
	REX::TIniSetting<bool>          bVerifyVTableID{ "Recon", "bVerifyVTableID", false };

	// Scaleform reader: dumps the ship HUD's ActionScript data model on demand.
	REX::TIniSetting<bool>          bScaleformReader{ "Scaleform", "bScaleformReader", false };
	REX::TIniSetting<bool>          bInterposeTargetData{ "Scaleform", "bInterposeTargetData", true };
	REX::TIniSetting<bool>          bScaleformSkipBoilerplate{ "Scaleform", "bScaleformSkipBoilerplate", true };
	REX::TIniSetting<std::uint32_t> uScaleformDepth{ "Scaleform", "uScaleformDepth", 8 };
	REX::TIniSetting<std::uint32_t> uScaleformMaxLines{ "Scaleform", "uScaleformMaxLines", 3000 };
	REX::TIniSetting<std::uint32_t> uScaleformMaxChildren{ "Scaleform", "uScaleformMaxChildren", 60 };

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
	std::atomic<bool> g_interposeInstalled{ false };
	std::atomic<bool> g_interposeFailed{ false };

	constexpr const char* kShipHudMenu = "SpaceshipHudMenu";

	// The user event that requests a data-model dump: the scanner key, i.e. the
	// same trigger the finished panel will use.
	constexpr const char* kDumpTriggerEvent = "SHMonocle";

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

	void LogInputQueue(const RE::InputEvent* a_head)
	{
		const bool logHeld = bLogInputHeldFrames.GetValue();
		const bool logOther = bLogInputNonButton.GetValue();

		for (const RE::InputEvent* event = a_head; event; event = event->next) {
			if (event->eventType != RE::InputEvent::EventType::kButton) {
				if (logOther && InputBudgetOk())
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

			if (down && !firstFrame && !logHeld)
				continue;  // held-down repeat

			// Request a dump/capture on the trigger key's initial press. Only
			// flags are set here - the movie is touched from the per-frame task,
			// and the captured data is read on the UI thread by the interposer.
			if (down && firstFrame) {
				const char* userEvent = button->strUserEvent.c_str();
				if (userEvent && std::strcmp(userEvent, kDumpTriggerEvent) == 0) {
					if (bScaleformReader.GetValue())
						g_dumpRequested.store(true, std::memory_order_release);
					if (bInterposeTargetData.GetValue())
						g_captureRequested.store(true, std::memory_order_release);
				}
			}

			if (!InputBudgetOk())
				return;

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
		if (bLogInput.GetValue())
			LogInputQueue(a_queueHead);

		if (const auto original = g_origPerformInputProcessing.load(std::memory_order_acquire))
			original(a_this, a_queueHead);
	}

	// Called every frame until it succeeds once. The claim is a single-winner
	// exchange: two threads patching the same vtable entry would leave the hook
	// calling itself, so this must not be a plain bool (the SeamlessGravJumps
	// double-fire was exactly this shape of bug).
	void TryInstallInputTap()
	{
		if (!bLogInput.GetValue() || g_inputTapClaimed.load(std::memory_order_acquire))
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
		"mouseY", "name", "numChildren", "opaqueBackground", "parent", "prototype",
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

			REX::INFO("[nav] [{:>2}] uniqueID={} uTargetType={} name={} landing={}",
				a_index, field("uniqueID"), field("uTargetType"),
				field("sTargetName"), field("bLandingAllowed"));

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
		TryInstallInterposer();

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

		REX::INFO("config: bLogInput={} bLogInputHeldFrames={} bLogInputNonButton={} uMaxInputLines={} "
				  "bLogMenus={} bLogHeartbeat={} fHeartbeatSeconds={} bVerifyVTableID={}",
			bLogInput.GetValue(), bLogInputHeldFrames.GetValue(), bLogInputNonButton.GetValue(),
			uMaxInputLines.GetValue(), bLogMenus.GetValue(), bLogHeartbeat.GetValue(),
			fHeartbeatSeconds.GetValue(), bVerifyVTableID.GetValue());

		if (bLogMenus.GetValue()) {
			if (const auto ui = RE::UI::GetSingleton()) {
				ui->RegisterSink(&g_menuSink);
				REX::INFO("[menu] open/close sink registered");
			} else {
				REX::WARN("[menu] UI singleton unavailable; open/close sink not registered");
			}

			if (const auto menus = SFSE::GetMenuInterface()) {
				menus->Register(&OnMenuMovieCreated);
				REX::INFO("[menu] SFSE movie-created callback registered");
			} else {
				REX::WARN("[menu] SFSE menu interface unavailable; movie-created callback not registered");
			}
		}

		SFSE::GetTaskInterface()->AddPermanentTask(OnFrame);
		REX::INFO("recon task registered - this build only observes, it changes nothing");
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
	SFSE::GetMessagingInterface()->RegisterListener(OnMessage);
	REX::INFO("{} loaded", SFSE::GetPluginName());
	return true;
}
