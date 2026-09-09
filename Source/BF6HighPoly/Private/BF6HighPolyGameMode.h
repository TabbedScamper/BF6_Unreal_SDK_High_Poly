// BF6HighPolyGameMode - the game's own modes, rebuilt as Portal objects.
//
// WHAT THIS IS. A level ships every mode's layout at once: Conquest's flags
// and spawns, Breakthrough's sectors, Obliteration's MCOMs and bomb pickups,
// the vehicle pads and resupply gems, all authored on subworlds under
// _layers_gameplay/<mode>/. The main build reads the ART those layers place;
// this reads the GAMEPLAY they place - straight from the install through
// libbf6, classified by the laws the Godot plugin's fork settled on real maps
// (bf6_level_gamemode_layout) - and rebuilds it in the scene as the tool's own
// placeable objects, in the map conventions a person authors by hand:
//
//   Conquest      Play Area/CombatArea (infantry + air), TEAM_1_HQ / TEAM_2_HQ
//                 with their area and deploy spawns, Objectives/CapturePoint<L>
//                 with Area-<L> and Spawns<L>/Team1|Team2, Vehicles/Team1|Team2,
//                 AA-Defences, Resupply, AI_Spawners, EndGameCamera
//   Breakthrough  Sectors/Sector<n> (area, AreaTrigger, CapturePoint<L> with
//                 spawns, TEAM_1_HQ<n> / TEAM_2_HQ<n> chained one sector back),
//                 CombatArea with the mined outline, AISpawners, Vehicles
//   Obliteration  Play Area, TEAM_<t>_HQ (+ satellites), Objectives/MCOM_<L>
//                 with the defuse bomb beside each, neutral Bomb spawns kept
//                 midfield, Vehicles/Team1|Team2, AA-Defences, Resupply,
//                 AI_Spawners, EndGameCamera
//   other modes   the flat layout, grouped by kind under the mode node
//
// EVERY BUILD IS CHECKED. Duplicate ObjIds, unwired NodePath exports, pads
// that never spawn (P_AutoSpawnEnabled is false by default - they are armed),
// two capture points on one spot, a gameplay node on the world origin, and the
// Breakthrough HQ chain: each check is a bug that shipped in a real map. The
// results and a coverage line ("N gem objects, M carry no type and are not
// placed") go to the log as one block per build.
//
// SWITCHING IS INSTANT. The main build tags every mode's art with its mode
// (BF6GameMode=<key> on the instanced components) and builds all of them, so
// choosing a mode is hide/show, not a rebuild. Gameplay objects go under one
// node per mode, named for the mode; choosing another mode hides that node,
// and choosing it again shows it with everything you edited still there.
// Delete the node and choose the mode again: it is rebuilt from the mined
// data.
//
// OWNERSHIP IS SPLIT, on purpose. The gameplay objects are the user's
// content: placed through the tool's own placement so they carry BF6Placed,
// save with the map and export. Everything else here - the floating names,
// the marker shapes drawn when the tool cannot place (a read-only base map) -
// is an add-on actor (MarkAddonActor) filed under the "High Poly" outliner
// folder and never reaches a save or an export.
//
// This file registers itself. Nothing in BF6HighPoly.cpp has to know it
// exists.
#pragma once

#include "CoreMinimal.h"

class AActor;

namespace BF6HPGameMode
{
	// What a mined thing is, after classification (bf6_gm_role, mirrored).
	enum class EKind : uint8
	{
		Spawn,          // AlternateSpawnEntityData -> SpawnPoint
		Capture,        // a capture polygon -> CapturePoint + its area
		Zone,           // a bare polygon: a sector, a base, a boundary
		Combat,         // a polygon a CombatAreaEntityData claims
		Obb,            // OBBData -> OBBVolume
		Vehicle,        // gem linked to the vehicle/stationary spawner template
		Resupply,       // gem linked to gem_vehicleresupplystation (a MIX in game; position decides)
		Mcom,           // gem linked to gem_objective_mcom, or ObjectiveData
		Bomb,           // gem linked to gem_bomb_pickup
		SpecialArea,    // gem linked to gem_specialcombatarea (rings objectives and bases)
		Slot,           // gem linked to a vectorshapeasset: a vehicle slot on a spawn shape
		Unlinked,       // gem with no link word: placed by no builder
		Other
	};

	// One mined object, in Unreal space (cm, Z up) ready to place.
	struct FObject
	{
		EKind    Kind = EKind::Other;
		FString  Label;               // "Flag A", "Flag A Spawn", "Zone 2"
		FString  TypeName;            // the game's class, for the log
		FString  GemLink;             // gems: the template the link word names, "" none
		int32    GemValue = -1;       // gems: the value word (not a vehicle type)
		FTransform Xf;                // world, Unreal; polygons: centroid at the lowest point
		TArray<FVector> Points;       // polygons: world, Unreal, as authored (not flattened)
		float    HeightM = 0.f;       // polygons: extrusion in metres, 0 = unbounded
		FVector  SizeGodot = FVector::ZeroVector;   // boxes: full size, Godot metres (x,y,z)
		int32    Team = 0;
		bool     bEnabled = true;
		float    AreaM2 = 0.f;
		int32    FlagIndex = INDEX_NONE;   // captures: letter index; spawns: the flag they stand at
		bool     bStationary = false;      // Vehicle: gem_stationaryspawner
		bool     bByData = false;          // Combat: decided by a data link
		bool     bHasGround = false;       // gems: the terrain under it was sampled
		float    GroundZ = 0.f;            // Unreal cm
		bool     bWater = false;           // gems: below the water surface + beach band
		bool     bHasLand = false;         // zones/captures: a dry point inside the polygon
		FVector  Land = FVector::ZeroVector;   // Unreal cm, on the terrain
	};

	struct FGrid
	{
		float X0 = 0.f, Z0 = 0.f, Step = 0.f;   // game metres (x, z)
		int32 Nx = 0, Nz = 0;
		TArray<float> H;                        // game metres, row-major nz x nx; -1e9 = no sample
		bool IsValid() const { return Nx > 1 && Nz > 1 && H.Num() == Nx * Nz; }
	};

	struct FMode
	{
		FString  Key;                 // "conquest"
		TArray<FObject> Objects;
		int32    Layers = 0;
		int32    Gems = 0, GemsUnlinked = 0;        // the coverage line
		int32    DroppedJunk = 0, DroppedOwned = 0, DroppedPropBox = 0, DroppedGemOther = 0, DroppedDup = 0;
		int32    DroppedTwin = 0;     // the second polygon of a double-authored objective
		int32    BigFlagRescued = 0;
		bool     bSanityCapped = false;
		FGrid    Grid;
		float    WaterY = -1e9f;      // game metres, -1e9 = none
		bool     bHasTerrain = false;
	};

	// ---- what the patches and the console call --------------------------

	// Pick a mode. Art switches at once; the mode's objects are shown, built
	// if they are not in the scene, mined first if nothing has been mined.
	// "all" shows every mode's art and no objects; "off" hides the objects and
	// leaves the art on the largest mode.
	void    SetMode(const FString& KeyOrPrettyName);
	FString CurrentMode();

	// Take one mode's node and everything under it out of the scene, in one
	// undo step. Nothing else is touched, so a rebuild is remove then choose.
	int32   RemoveMode(const FString& KeyOrPrettyName);

	// The HIGH POLY ring's GAME MODE entry opens this (patch 03); the add-on
	// also registers a pill of its own so it works without the patch.
	void    OpenRing(FVector2D ScreenCenter);

	// Called after a main build finished (patch 02). Re-applies the art
	// visibility for the chosen mode. A ticker catches it without the patch.
	void    OnArtBuilt();

	// "conquest" -> "Conquest", "teamdeathmatch" -> "Team Deathmatch".
	FString Pretty(const FString& Key);

	// The modes as a list for a dropdown, and the current one as an index into
	// it. Index 0 is "largest on this map", which is what no explicit choice
	// means.
	TArray<FString> ModeLabels();
	int32           CurrentModeIndex();
	void            SetModeByIndex(int32 Index);
	// The other way, for whatever spelling a caller has.
	FString KeyOf(const FString& KeyOrPretty);

	// Modes known for the open level: mined ones plus those the art tags say
	// exist, sorted by size.
	void    KnownModes(TArray<FString>& Out);

	// Start mining the open level's layers off the game thread. Safe to call
	// again; a running mine is not restarted.
	void    Mine(bool bThenApplyChoice);
	bool    IsMined();
	bool    IsMining();
	// Whether a map open reads its game modes straight away. See the console
	// command for why somebody might turn it off.
	bool    AutoMine();
	void    SetAutoMine(bool bOn);

	// The headless path: open the level, create a scratch custom map, mine,
	// build the mode, run the checks and compare against the fork's ground
	// truth where the level has any. Progress and PASS/FAIL lines go to the
	// log under LogBF6HighPolyGM; the console command drives it.
	void    RunModeTest(const FString& Level, const FString& Mode);
}
