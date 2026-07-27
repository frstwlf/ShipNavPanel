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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <zlib.h>
#include <string>
#include <unordered_map>
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

	// Stations and landing sites in the list, below the bodies. Ships are off by
	// default: in traffic that would be a list of everything flying past rather
	// than of destinations.
	REX::TIniSetting<bool> bIncludePOI{ "Panel", "bIncludePOI", true };
	REX::TIniSetting<bool> bIncludeShips{ "Panel", "bIncludeShips", false };
	REX::TIniSetting<bool>  bPanelRowSeparators{ "Panel", "bPanelRowSeparators", true };
	// Indent moons under their planet. Has no effect yet: nothing the ship HUD
	// feed carries identifies a moon's parent, and the guess v0.3.3 shipped was
	// wrong. Kept wired up because the data exists elsewhere - see the probe.
	REX::TIniSetting<bool> bNestMoons{ "Panel", "bNestMoons", true };

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
	REX::TIniSetting<bool>          bDumpPlanetRecords{ "Recon", "bDumpPlanetRecords", false };
	REX::TIniSetting<std::uint32_t> uDumpPlanetBytes{ "Recon", "uDumpPlanetBytes", 0x400 };

	REX::TIniSetting<bool>        bProbeStarmapFeed{ "Recon", "bProbeStarmapFeed", false };
	REX::TIniSetting<std::string> sStarmapFeed{ "Recon", "sStarmapFeed", "StarmapSystemBodyInfoProvider" };
	REX::TIniSetting<float> fPanelMoonIndent{ "Panel", "fPanelMoonIndent", 16.0f };

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
	std::atomic<bool>          g_captureRequested{ false };
	std::atomic<bool>          g_captureHighRequested{ false };
	std::atomic<bool>          g_starmapDumpRequested{ false };
	std::atomic<bool>          g_dumpPlanetsRequested{ false };
	std::atomic<std::uint32_t> g_starmapCallbacks{ 0 };
	std::atomic<bool> g_interposeInstalled{ false };
	std::atomic<bool> g_interposeFailed{ false };
	std::atomic<bool> g_subscribed{ false };
	std::atomic<bool> g_subscribeFailed{ false };

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
		// False for bodies added from the master file rather than offered by the
		// HUD: they have a name and a place in the tree, but no bearing and no
		// distance, so the arrow cannot point at one.
		bool fromFeed{ true };
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
	// So the hierarchy is loaded from a table generated out of the ESM instead,
	// where GNAM plainly is. `tools/ExportBodies.pas` is the xEdit script that
	// writes it. That also makes the mod independent of engine layout entirely,
	// which four builds of memory archaeology argue is worth something.
	constexpr const char* kBodyTablePath = "Data/SFSE/Plugins/ShipNavPanelBodies.txt";

	struct BodyEntry
	{
		GalaxyData  galaxy;
		std::string name;
		// False for bodies the game generates rather than authors. They are kept
		// for their place in the hierarchy - a body the HUD offers may be one of
		// them - but never listed in their own right.
		bool authored{ true };
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

	// Walks one record's subrecords looking for GNAM.
	bool FindGnam(const std::byte* a_data, std::size_t a_size, GalaxyData& a_out, std::string& a_name,
		std::uint32_t& a_fullID, bool& a_authored)
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
			// own size field then reads zero. Skipping that rule desyncs the
			// walk into the middle of a large payload - in the prototype it
			// silently lost 1134 of 1765 records, Kurtz among them.
			if (std::memcmp(sig, "XXXX", 4) == 0 && size == 4) {
				if (offset + 4 > a_size)
					return false;
				std::memcpy(&pending, a_data + offset, 4);
				offset += 4;
				continue;
			}
			if (pending != 0) {
				size = pending;
				pending = 0;
			}
			if (offset + size > a_size)
				return false;

			if (std::memcmp(sig, "EDID", 4) == 0 && size > 1) {
				const std::string_view editorID{ reinterpret_cast<const char*>(a_data + offset), size - 1 };
				a_authored = IsAuthoredBody(editorID);
				a_name = NameFromEditorID(editorID);
			} else if (std::memcmp(sig, "FULL", 4) == 0 && size == 4 && a_fullID == 0) {
				// The localised name's id. It sits inside a component block but
				// reads as an ordinary subrecord, and comes before GNAM.
				std::memcpy(&a_fullID, a_data + offset, 4);
			} else if (std::memcmp(sig, "GNAM", 4) == 0 && size >= 12) {
				std::uint32_t values[3];
				std::memcpy(values, a_data + offset, sizeof(values));
				a_out = GalaxyData{ values[0], values[1], values[2] };
				return true;  // EDID precedes GNAM, so the name is already in hand
			}
			offset += size;
		}
		return false;
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

	bool ParsePluginBodies(const std::string& a_path, const std::vector<std::uint8_t>& a_masterIndices,
		std::uint8_t a_selfIndex, bool a_validate, const std::unordered_map<std::uint32_t, std::string>& a_strings,
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

			const std::byte* body = raw.data();
			std::size_t      bodySize = raw.size();

			if ((record.flagsOrLabel & kRecordCompressed) != 0) {
				if (raw.size() < 5)
					continue;
				std::uint32_t inflatedSize = 0;
				std::memcpy(&inflatedSize, raw.data(), sizeof(inflatedSize));
				if (inflatedSize == 0 || inflatedSize > 64u * 1024u * 1024u)
					continue;

				inflated.resize(inflatedSize);
				uLongf produced = inflatedSize;
				if (::uncompress(reinterpret_cast<Bytef*>(inflated.data()), &produced,
						reinterpret_cast<const Bytef*>(raw.data() + 4),
						static_cast<uLong>(raw.size() - 4)) != Z_OK)
					continue;
				body = inflated.data();
				bodySize = produced;
			}

			GalaxyData data;
			std::string   name;
			std::uint32_t fullID = 0;
			bool          authored = false;
			if (!FindGnam(body, bodySize, data, name, fullID, authored))
				continue;

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
			a_out.insert_or_assign(runtimeID, BodyEntry{ data, std::move(name), authored });
			++records;
		}

		if (records != 0)
			REX::INFO("[bodies] {} bodies from {}", records, a_path);
		return true;
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

		std::unordered_map<std::string, std::uint8_t> indexByName;
		for (const auto& plugin : plugins)
			indexByName.emplace(fold(plugin.name), plugin.index);

		for (const auto& plugin : plugins) {
			const auto path = std::format("Data/{}", plugin.name);

			std::vector<std::string>                      masterNames;
			std::unordered_map<std::uint32_t, std::string> strings;
			if (!ParsePluginBodies(path, {}, plugin.index, false, strings, a_out, &masterNames)) {
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

			ParsePluginBodies(path, masterIndices, plugin.index, validate, strings, a_out, nullptr);
		}

		REX::INFO("[bodies] read {} bodies from {} plugin(s)", a_out.size(), plugins.size());
		return !a_out.empty();
	}

	// The cache holds RUNTIME form ids, which depend on where each plugin sits
	// in the load order - so the fingerprint covers the order and the file sizes
	// together. Install a mod, move one, or update the game, and it rebuilds.
	std::string LoadOrderFingerprint(const std::vector<PluginInfo>& a_plugins)
	{
		std::string print;
		for (const auto& plugin : a_plugins) {
			std::error_code error;
			const auto      size = std::filesystem::file_size(std::format("Data/{}", plugin.name), error);
			print += std::format("{}:{}:{}|", plugin.name, plugin.index, error ? 0 : size);
		}
		return print.empty() ? std::string{ "none" } : print;
	}

	void WriteBodyTable(const std::unordered_map<std::uint32_t, BodyEntry>& a_table,
		const std::string& a_fingerprint)
	{
		std::ofstream file{ kBodyTablePath, std::ios::trunc };
		if (!file) {
			REX::WARN("[bodies] could not write {} - it will be rebuilt next launch", kBodyTablePath);
			return;
		}
		file << "# ShipNavPanel body table - generated from the load order\n";
		file << "# Delete this to have it rebuilt.\n";
		file << "# formID,systemID,parentPlanetID,planetID,authored,name\n";
		file << "# authored=0 means the game generates that body rather than authoring it - an orbital\n";
		file << "# marker, a grav-jump arrival point, a station's anchor. Kept for its place in the\n";
		file << "# hierarchy, since the HUD may offer it, but never listed as a body in its own right.\n";
		file << "# order " << a_fingerprint << "\n";
		for (const auto& [formID, entry] : a_table)
			file << std::format("{:08X},{},{},{},{},{}\n", formID, entry.galaxy.systemID,
				entry.galaxy.parentPlanetID, entry.galaxy.planetID, entry.authored ? 1 : 0, entry.name);
		REX::INFO("[bodies] cached {} bodies to {}", a_table.size(), kBodyTablePath);
	}

	// `formID,systemID,parentPlanetID,planetID` per line, `#` for comments and
	// blank lines ignored. Returns false when the cache is missing or was built
	// against a different master file.
	bool ReadBodyTable(std::unordered_map<std::uint32_t, BodyEntry>& a_out, const std::string& a_fingerprint)
	{
		std::ifstream file{ kBodyTablePath };
		if (!file)
			return false;

		std::size_t loaded = 0;
		std::size_t line = 0;
		std::size_t rejected = 0;
		bool        sawFingerprint = false;
		std::string text;

		while (std::getline(file, text)) {
			++line;
			if (const auto hash = text.find('#'); hash != std::string::npos) {
				// The cache holds runtime form ids, so it is only valid for the
				// load order that produced it. Anything installed, moved or
				// updated rebuilds it rather than leaving it quietly wrong.
				if (const auto marker = text.find("order "); marker != std::string::npos) {
					if (text.substr(marker + 6) != a_fingerprint) {
						REX::INFO("[bodies] {} was built for a different load order - rebuilding",
							kBodyTablePath);
						return false;
					}
					sawFingerprint = true;
				}
				text.erase(hash);
			}
			if (text.find_first_not_of(" \t\r\n") == std::string::npos)
				continue;

			// Split on the first four commas only: a real name can contain
			// spaces ("New Atlantis") and tokenising on whitespace would cut it
			// in half - which the editor-id names never would have shown.
			std::string fields[5];
			std::string name;
			{
				std::size_t start = 0;
				std::size_t which = 0;
				for (; which < 5; ++which) {
					const auto comma = text.find(',', start);
					if (comma == std::string::npos)
						break;
					fields[which] = text.substr(start, comma - start);
					start = comma + 1;
				}
				if (which == 5)
					name = text.substr(start);
				while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
					name.pop_back();
			}

			std::istringstream parse{ std::format("{} {} {} {}", fields[1], fields[2], fields[3], fields[4]) };

			const std::string& idText = fields[0];
			std::uint32_t      system = 0;
			std::uint32_t      parent = 0;
			std::uint32_t      planet = 0;
			int                authored = 1;
			if (idText.empty() || !(parse >> system >> parent >> planet >> authored)) {
				if (++rejected <= 3)
					REX::WARN("[bodies] {}:{} could not be read - expected "
							  "formID,system,parent,planet",
						kBodyTablePath, line);
				continue;
			}

			std::uint32_t formID = 0;
			try {
				formID = static_cast<std::uint32_t>(std::stoul(idText, nullptr, 16));
			} catch (...) {
				if (++rejected <= 3)
					REX::WARN("[bodies] {}:{} '{}' is not a hex form id", kBodyTablePath, line, idText);
				continue;
			}
			if (formID == 0 || planet == 0)
				continue;

			a_out.insert_or_assign(formID,
				BodyEntry{ GalaxyData{ system, parent, planet }, name, authored != 0 });
			++loaded;
		}

		// A cache with no fingerprint predates load-order awareness, so its ids
		// cannot be trusted against anything but the base game.
		if (loaded == 0 || !sawFingerprint)
			return false;

		REX::INFO("[bodies] loaded {} bodies from {}{}", loaded, kBodyTablePath,
			rejected ? std::format(" ({} lines rejected)", rejected) : "");
		return true;
	}

	// Off the main thread: reaching the planets means seeking most of the way
	// through a 1.4 GB file and inflating ~1700 records, which is a second or
	// two the game should not be made to wait for. Nesting simply starts working
	// shortly after load, and the table is cached so later launches are instant.
	void LoadBodyTable()
	{
		if (g_bodyTableLoaded.exchange(true, std::memory_order_acq_rel))
			return;

		std::thread{ [] {
			std::unordered_map<std::uint32_t, BodyEntry> table;
			const auto fingerprint = LoadOrderFingerprint(CollectPlugins());

			if (!ReadBodyTable(table, fingerprint)) {
				table.clear();
				if (!ParseAllBodies(table)) {
					REX::WARN("[bodies] could not read the planet hierarchy - moons will not be nested");
					return;
				}
				WriteBodyTable(table, fingerprint);
			}

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
	// Name and distance are separate fields so one can sit left and the other
	// right - a single field cannot align part of its text.
	RE::Scaleform::GFx::Value g_panelRows[kPanelMaxRowsHard];
	RE::Scaleform::GFx::Value g_panelDists[kPanelMaxRowsHard];
	RE::Scaleform::GFx::Value g_panelFormat;
	RE::Scaleform::GFx::Value g_panelDistFormat;
	RE::Scaleform::GFx::Value g_panelHint;
	RE::Scaleform::GFx::Value g_panelHintRight;
	RE::Scaleform::GFx::Value g_panelHintFormat;
	RE::Scaleform::GFx::Value g_panelHintRightFormat;
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

	constexpr std::uint32_t kTargetTypeStar = 1;
	constexpr std::uint32_t kTargetTypePOI = 4;
	constexpr std::uint32_t kTargetTypeShip = 5;
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
		if (bProbeStarmapFeed.GetValue())
			g_starmapDumpRequested.store(true, std::memory_order_release);
		if (bDumpPlanetRecords.GetValue())
			g_dumpPlanetsRequested.store(true, std::memory_order_release);

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
			g_panelDistFormat = RE::Scaleform::GFx::Value{};
			g_panelHint = RE::Scaleform::GFx::Value{};
			g_panelHintRight = RE::Scaleform::GFx::Value{};
			g_panelHintFormat = RE::Scaleform::GFx::Value{};
			g_panelHintRightFormat = RE::Scaleform::GFx::Value{};
			for (auto& dist : g_panelDists)
				dist = RE::Scaleform::GFx::Value{};
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

		std::uint32_t system = 0;
		for (const auto& row : a_rows) {
			if (row.haveGalaxy && row.galaxy.systemID != 0) {
				system = row.galaxy.systemID;
				break;
			}
		}
		if (system == 0)
			return;

		std::lock_guard lock{ g_bodyTableMutex };
		const auto      found = g_bodiesBySystem.find(system);
		if (found == g_bodiesBySystem.end())
			return;

		for (const auto formID : found->second) {
			const auto entry = g_bodyTable.find(formID);
			if (entry == g_bodyTable.end() || !entry->second.authored || entry->second.name.empty())
				continue;  // generated bodies are the HUD's business, not ours

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

	// Logs the whole shape of whatever the star map feed delivers, once per
	// press of the capture trigger rather than continuously - it fires as often
	// as any other feed and would otherwise bury the log.
	class StarmapProbeHandler : public RE::Scaleform::GFx::FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			const auto seen = g_starmapCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
			if (!g_starmapDumpRequested.exchange(false, std::memory_order_acq_rel)) {
				if (seen == 1)
					REX::INFO("[starmap] feed is LIVE outside the map - {} callback(s) so far. "
							  "Press the scanner key to dump its contents.",
						seen);
				return;
			}

			REX::INFO("[starmap] ==== dump after {} callback(s) ====", seen);
			if (a_params.argCount < 1 || !a_params.args) {
				REX::WARN("[starmap] callback carried no argument");
				return;
			}

			RE::Scaleform::GFx::Value event = a_params.args[0];
			RE::Scaleform::GFx::Value data;
			if (!event.IsObject() || !event.GetMember("data", &data))
				data = event;

			LevelCollector top{ "[starmap] payload", nullptr };
			data.VisitMembers(&top);

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
					// The marker is placed on the circle rather than rotated, so
					// there is no orientation to be wrong at any bearing.
					const double bearing = selectedAngle * (bArrowInvertAngle.GetValue() ? -1.0 : 1.0) +
					                       static_cast<double>(fArrowAngleOffset.GetValue());
					const double rotation = bearing;
					const double markerRadians = bearing * 3.14159265358979323846 / 180.0;
					const double markerRadius = static_cast<double>(fArrowRadius.GetValue());
					g_arrowClip.SetMember("x", V{ markerRadius * std::sin(markerRadians) });
					g_arrowClip.SetMember("y", V{ -markerRadius * std::cos(markerRadians) });

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

						// The star map's body feed, if the probe is on. A
						// failure here is informative and must not disturb the
						// two subscriptions above, which the mod depends on.
						if (bProbeStarmapFeed.GetValue()) {
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

		// A hairline between rows. Static, because the rows are at fixed
		// positions - only their contents change.
		if (bPanelRowSeparators.GetValue() && rows > 1) {
			V sepFill[]{ V{ static_cast<std::uint32_t>(0x66CCFF) }, V{ 0.10 } };
			for (std::size_t i = 1; i < rows; ++i) {
				const double y = 6.0 + rowHeight * static_cast<double>(i);
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

			// A mouse body with a wheel in it. The triangles alone were read as
			// generic up/down rather than as a wheel, so the glyph now says
			// which device it means and the triangles say which way it turns.
			rect(12.0, midY - 8.0, 23.0, midY + 8.0, 0x99D6FF, 0.30);
			rect(16.5, midY - 5.5, 18.5, midY - 0.5, 0xCCE6FF, 0.95);

			// Stacked beside the mouse, so the pair reads as one symbol.
			const auto triangle = [&](double a_cx, double a_cy, bool a_up) {
				V fill[]{ V{ static_cast<std::uint32_t>(0x99D6FF) }, V{ 0.85 } };
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
			g_panelDistFormat.SetMember("color", V{ static_cast<std::uint32_t>(0x8FB8D4) });
			g_panelDistFormat.SetMember("align", V{ "right" });
		}

		constexpr double kNamePad = 10.0;
		constexpr double kDistWidth = 96.0;
		const double     nameWidth = std::max(40.0, width - kNamePad * 2.0 - kDistWidth - 6.0);

		std::size_t made = 0;
		for (std::size_t i = 0; i < rows; ++i) {
			const double rowY = 6.0 + rowHeight * static_cast<double>(i);

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
					a_field.SetMember("textColor", V{ static_cast<std::uint32_t>(0xCCE6FF) });
				}

				RE::Scaleform::GFx::Value added;
				if (!g_panelClip.Invoke("addChild", &added, &a_field, 1))
					return false;
				a_field.SetMember("visible", V{ false });
				return true;
			};

			if (!makeField(g_panelRows[i], kNamePad, nameWidth, false))
				break;
			if (!makeField(g_panelDists[i], width - kNamePad - kDistWidth, kDistWidth, true))
				break;
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
			// Left hint sits right against the wheel symbol; the confirm hint is
			// right-aligned to the panel edge, mirroring the name/distance
			// columns above it.
			const auto makeHint = [&](RE::Scaleform::GFx::Value& a_field, RE::Scaleform::GFx::Value& a_format,
									   double a_x, double a_w, const char* a_align, const std::string& a_text,
									   const char* a_tag) {
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
				a_field.SetMember("y", V{ hintTop + 2.0 });

				if (BorrowTextFormat(root, rootPath, a_format, "[panel-hint]")) {
					a_format.SetMember("size", V{ 14.0 });
					a_format.SetMember("bold", V{ false });
					a_format.SetMember("color", V{ static_cast<std::uint32_t>(0x7FA8C4) });
					a_format.SetMember("align", V{ a_align });
					a_field.SetMember("embedFonts", V{ true });
					a_field.SetMember("defaultTextFormat", a_format);
				} else {
					a_field.SetMember("textColor", V{ static_cast<std::uint32_t>(0x7FA8C4) });
				}

				a_field.SetMember("text", V{ a_text.c_str() });
				if (a_format.IsObject())
					a_field.Invoke("setTextFormat", nullptr, &a_format, 1);

				RE::Scaleform::GFx::Value added;
				if (!g_panelClip.Invoke("addChild", &added, &a_field, 1))
					REX::WARN("[panel] {} hint addChild failed", a_tag);
			};

			// Sized from the right hint's needs rather than a percentage split:
			// the 45/55 split in v0.3.2 left the browse hint too narrow and it
			// truncated mid-word.
			const double rightWidth = std::min(150.0, std::max(90.0, width * 0.36));
			const double leftWidth = std::max(60.0, width - hintTextX - rightWidth - 14.0);
			makeHint(g_panelHint, g_panelHintFormat, hintTextX, leftWidth, "left",
				std::string{ "wheel to browse" }, "left");
			makeHint(g_panelHintRight, g_panelHintRightFormat, width - rightWidth - 10.0, rightWidth, "right",
				std::format("{}  lock / clear", sConfirmKeyLabel.GetValue()), "right");
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
				auto&             nameField = g_panelRows[r];
				auto&             distField = g_panelDists[r];
				if (n >= local.size()) {
					nameField.SetMember("visible", V{ false });
					distField.SetMember("visible", V{ false });
					continue;
				}

				const auto& row = g_candidates[local[n]];
				if (row.id == highlight) {
					highlightRow = r;
					haveHighlightRow = true;
				}

				if (refreshText) {
					// Moons sit indented under their planet. Done by moving the
					// field rather than padding the string, so it does not
					// depend on the width of a space in a borrowed font.
					nameField.SetMember("x",
						V{ 10.0 + (row.isMoon ? static_cast<double>(fPanelMoonIndent.GetValue()) : 0.0) });

					// The locked body is marked in the list itself, so the panel
					// says what the HUD is showing without having to be closed.
					const bool  isLocked = locked != 0 && row.id == locked;
					const char* mark = isLocked ? "> " : "  ";
					const auto  name = std::format("{}{}", mark, row.name);
					nameField.SetMember("text", V{ name.c_str() });
					// defaultTextFormat only applies to text present when it was
					// set, so re-apply after every assignment or the new glyphs
					// fall back to no font.
					if (g_panelFormat.IsObject())
						nameField.Invoke("setTextFormat", nullptr, &g_panelFormat, 1);

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
				nameField.SetMember("visible", V{ true });
				distField.SetMember("visible", V{ true });
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
