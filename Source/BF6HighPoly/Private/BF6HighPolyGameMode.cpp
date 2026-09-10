// BF6HighPolyGameMode - see the header for what this is and why.
//
// THREE LAYERS, top to bottom:
//
//   1. libbf6. bf6_level_gamemodes lists every gameplay entity on every mode
//      subworld; bf6_level_gamemode_layout says what those entities MEAN -
//      the gem classification, the double-authored objective merge, the junk
//      and duplicate filters, the flag lettering, the ground and water
//      marking - all of it read from the install by the laws the Godot
//      plugin's fork settled on real maps. This file binds both by name, on a
//      context of its own, so a mine never shares the main build's context
//      with the worker that is driving it (one read worker at a time).
//   2. Nothing between. The classification used to live here, in a port of
//      highpoly_gmmine.gd; it is data, so it moved into the reader where the
//      test harness can run it against the game without an editor.
//   3. The scene, a port of highpoly_gamemode.gd onto the tool's placeables:
//      one node per mode, and under it the layout a person authors by hand -
//      Play Area, Sectors, Objectives, Vehicles, AA-Defences, Resupply,
//      AI_Spawners - never a flat bag of objects at the root.
//
// The art switch is the fourth thing and the simplest: the main build tags
// every instanced component with its mode and this hides or shows them.

#include "BF6HighPolyGameMode.h"
#include "BF6HighPolyShared.h"   // MakeUnselectable: add-on geometry is never a click target
#include "BF6HighPolyCore.h"   // BF6HP::CoreDllPath
#include "BF6SDKExtension.h"

#include "Algo/Reverse.h"
#include "Async/Async.h"
#include "Misc/ScopeExit.h"
#include <atomic>
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"

THIRD_PARTY_INCLUDES_START
#include "bf6_core.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogBF6HighPolyGM, Log, All);

// ============================================================================
// Structs come from the deployed reader header. A hand-copied grid layout
// drifted when two reader branches were integrated and caused a map-open crash.
// ============================================================================
struct bf6_ctx;

namespace
{
	enum EGmKindAbi : int32
	{
		GM_SPAWN = 0, GM_VOLUME = 1, GM_OBB = 2, GM_COMBAT = 3, GM_CAPTURE = 4,
		GM_SECTOR = 5, GM_OBJECTIVE = 6, GM_VEHICLE_SPAWN = 7, GM_SOLDIER_SPAWN = 8,
		GM_GEM = 9
	};

	// bf6_gm_role
	enum EGmRoleAbi : int32
	{
		GMR_DROPPED = 0, GMR_SPAWN = 1, GMR_CAPTURE = 2, GMR_ZONE = 3, GMR_COMBAT = 4,
		GMR_OBB = 5, GMR_VEHICLE = 6, GMR_RESUPPLY = 7, GMR_MCOM = 8, GMR_BOMB = 9,
		GMR_SPECIALAREA = 10, GMR_SLOT = 11, GMR_UNLINKED = 12
	};

	using FGmEntityAbi = bf6_gm_entity;
	using FGmStatsAbi = bf6_gm_stats;
	using FGmObjectAbi = bf6_gm_object;
	using FGmLayoutAbi = bf6_gm_layout;

	typedef bf6_ctx* (*FnOpen)(const char*, char*, int);
	typedef void     (*FnClose)(bf6_ctx*);
	typedef int      (*FnGameModes)(bf6_ctx*, const char*, FGmEntityAbi*, int, FGmStatsAbi*, char*, int);
	typedef int      (*FnLayout)(bf6_ctx*, const char*, const char*, FGmObjectAbi*, int, FGmLayoutAbi*, char*, int);

	// A context of this file's own. The main build's context is driven from
	// a worker thread while it reads; a mine that shared it would race that.
	// The price is the mount and the type schema again, once per session.
	struct FGmCore
	{
		void*       Dll = nullptr;
		bf6_ctx*    Ctx = nullptr;
		FnOpen      Open = nullptr;
		FnClose     Close = nullptr;
		FnGameModes GameModes = nullptr;
		FnLayout    Layout = nullptr;
		FString     Error;

		bool Bind(const FString& GameDir, const FString& DllPath)
		{
			if (Ctx) return true;
			Error.Reset();
			if (!Dll) Dll = FPlatformProcess::GetDllHandle(*DllPath);
			if (!Dll) { Error = FString::Printf(TEXT("could not load %s"), *DllPath); return false; }
			const auto Abi = reinterpret_cast<decltype(&bf6_abi_version)>(FPlatformProcess::GetDllExport(Dll, TEXT("bf6_abi_version")));
			if (!Abi || Abi() != BF6_ABI_VERSION) { Error = TEXT("Game-mode reader and add-on versions differ. Install the matching reader package."); return false; }
			Open      = (FnOpen)      FPlatformProcess::GetDllExport(Dll, TEXT("bf6_open"));
			Close     = (FnClose)     FPlatformProcess::GetDllExport(Dll, TEXT("bf6_close"));
			GameModes = (FnGameModes) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_gamemodes"));
			Layout    = (FnLayout)    FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_gamemode_layout"));
			if (!Open || !Close) { Error = TEXT("bf6_core.dll has no bf6_open"); return false; }
			if (!GameModes || !Layout)
			{
				Error = TEXT("this bf6_core.dll is older than the add-on: it has no bf6_level_gamemode_layout. ")
				        TEXT("Deploy the dll + header pair from integration/gamemode2/libbf6.");
				return false;
			}
			char err[512] = {0};
			Ctx = Open(TCHAR_TO_UTF8(*GameDir), err, sizeof(err));
			if (!Ctx) { Error = UTF8_TO_TCHAR(err); return false; }
			return true;
		}

		void Shutdown()
		{
			if (Ctx && Close) Close(Ctx);
			Ctx = nullptr;
			// The handle is shared with the tool and the main build; never freed.
			Dll = nullptr;
		}
	};
}

namespace BF6HPGameMode
{
namespace
{
	const TCHAR* kAddon      = TEXT("HighPolyGameMode");   // MarkAddonActor owner
	const TCHAR* kArtOwner   = TEXT("addon:HighPoly");     // the main build's actors
	const TCHAR* kArtTag     = TEXT("BF6GameMode=");       // on instanced components
	const TCHAR* kNodeTag    = TEXT("BF6HPGameMode=");     // on the per-mode node, this session
	const TCHAR* kFolder     = TEXT("High Poly");          // the outliner folder every add-on actor files under
	const FName  kGroupTag(TEXT("BF6Group"));              // the tool's own node tag

	// The fork's numbers, and for the fork's reasons.
	constexpr double kCm             = 100.0;   // a game metre
	constexpr double kFlagGapCheckM  = 20.0;    // two capture points closer than this are one objective built twice
	constexpr double kRsObjectiveM   = 80.0;    // a mixed-group gem this near a flag is an emplacement, not resupply
	constexpr double kAirSlotM       = 200.0;   // a slot this far ABOVE THE TERRAIN is the jet air-spawn
	constexpr double kHqBoxHeightM   = 700.0;
	constexpr double kDeployClusterM = 45.0;
	constexpr double kOnFlagM        = 60.0;
	constexpr double kPadTieM        = 60.0;
	constexpr double kBaseSpawnsMin  = 3;
	constexpr double kAreaLimitM2    = 16640000.0;   // the editor's own ceiling for a volume's area

	// ---- state ------------------------------------------------------------
	FString GLevel;                       // the open map
	FString GChosen;                      // mode key, "all", or "" (off)
	bool    GObjectsOn = true;            // build the gameplay objects, not just the art
	TMap<FString, FMode> GModes;          // mined, keyed by mode key
	bool    GMined = false;
	bool    GMining = false;
	// Reading the map's game modes as soon as it opens is what fills the GAME
	// MODE list before anyone looks for it. It is also a background read of the
	// install at the busiest moment there is - the frame after a map with
	// thousands of objects appears - so it can be turned off while measuring,
	// and on a machine that would rather not.
	bool    GAutoMine = true;
	// Set while a mine was started by opening a map rather than by somebody
	// asking. Same work, no toast: a notification on every map open is noise,
	// and this one runs whether or not anybody wanted game modes today.
	bool    GQuietMine = false;
	bool    GApplyAfterMine = false;
	FString GMineSummary;
	FGmCore GCore;
	TWeakObjectPtr<AActor> GArtActorSeen; // the last main-build actor the ticker applied to
	FTSTicker::FDelegateHandle GTicker;
	bool    GInited = false;

	const TCHAR* KindName(EKind K)
	{
		switch (K)
		{
			case EKind::Spawn: return TEXT("spawn");             case EKind::Capture: return TEXT("capture");
			case EKind::Zone: return TEXT("zone");               case EKind::Combat: return TEXT("combat");
			case EKind::Obb: return TEXT("box");                 case EKind::Vehicle: return TEXT("vehicle");
			case EKind::Resupply: return TEXT("resupply");       case EKind::Mcom: return TEXT("mcom");
			case EKind::Bomb: return TEXT("bomb");               case EKind::SpecialArea: return TEXT("special area");
			case EKind::Slot: return TEXT("slot");               case EKind::Unlinked: return TEXT("unlinked gem");
			default: return TEXT("other");
		}
	}

	FLinearColor Tint(EKind K)
	{
		switch (K)
		{
			case EKind::Capture:     return FLinearColor(1.0f, 0.55f, 0.1f);
			case EKind::Mcom:        return FLinearColor(0.9f, 0.15f, 0.15f);
			case EKind::Bomb:        return FLinearColor(0.95f, 0.4f, 0.1f);
			case EKind::Spawn:       return FLinearColor(0.2f, 0.85f, 0.3f);
			case EKind::Combat:      return FLinearColor(1.0f, 0.85f, 0.2f);
			case EKind::Zone:        return FLinearColor(0.55f, 0.75f, 0.95f);
			case EKind::Obb:         return FLinearColor(0.9f, 0.15f, 0.15f);
			case EKind::Vehicle:     return FLinearColor(0.85f, 0.5f, 0.95f);
			case EKind::Resupply:    return FLinearColor(0.5f, 0.9f, 0.9f);
			case EKind::SpecialArea: return FLinearColor(0.8f, 0.8f, 0.4f);
			default:                 return FLinearColor(0.7f, 0.7f, 0.75f);
		}
	}

	// ---- where things are -------------------------------------------------
	FString CacheDir()  { return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("HighPoly")); }
	FString ChoicePath(const FString& Level)
	{
		return FPaths::Combine(CacheDir(), TEXT("gamemode"), Level + TEXT(".txt"));
	}
	void SaveChoice()
	{
		if (GLevel.IsEmpty()) return;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ChoicePath(GLevel)), true);
		FFileHelper::SaveStringToFile(GChosen, *ChoicePath(GLevel));
	}
	FString LoadChoice(const FString& Level)
	{
		FString S;
		if (!FFileHelper::LoadFileToString(S, *ChoicePath(Level))) return FString();
		S.TrimStartAndEndInline();
		return S;
	}
	// The install the High Poly add-on was pointed at, read the way it
	// records it, so the two never disagree about which game is being read.
	FString InstallDir()
	{
		FString Dir;
		if (FFileHelper::LoadFileToString(Dir, *FPaths::Combine(CacheDir(), TEXT("install.txt"))))
		{
			Dir.TrimStartAndEndInline();
			if (FPaths::DirectoryExists(Dir)) return Dir;
		}
		return BF6Ext::GameInstallDir();
	}
	FString DllPath()
	{
		// One resolver, shared with the rest of the add-on: staged path first, so
		// an installed plugin with no Source tree still finds the core.
		return BF6HP::CoreDllPath();
	}

	// ---- game space -> Unreal ---------------------------------------------
	// Y and Z swap and metres become centimetres, exactly as the main build
	// does it, so a flag lands on the ground the build drew.
	FVector ToUE(double gx, double gy, double gz) { return FVector(gx, gz, gy) * kCm; }
	FVector ToUE(const float* g) { return ToUE(g[0], g[1], g[2]); }
	FVector SwapYZ(const FVector& G) { return FVector(G.X, G.Z, G.Y); }

	struct FGameXf { FVector R, U, F, T; };
	FGameXf XfOf(const float* m)
	{
		FGameXf x;
		x.R = FVector(m[0], m[1], m[2]);  x.U = FVector(m[3], m[4], m[5]);
		x.F = FVector(m[6], m[7], m[8]);  x.T = FVector(m[9], m[10], m[11]);
		return x;
	}
	FTransform ToTransform(const FGameXf& x)
	{
		return FTransform(FMatrix(SwapYZ(x.R), SwapYZ(x.F), SwapYZ(x.U), ToUE(x.T.X, x.T.Y, x.T.Z)));
	}
	// A yaw-only, unit-scale placement for objects whose model must not
	// inherit the entity's scale (a spawn arrow drawn at scale 0 is nothing).
	FTransform ToPlacement(const FGameXf& x)
	{
		FTransform T = ToTransform(x);
		T.SetScale3D(FVector::OneVector);
		if (T.ContainsNaN()) T = FTransform(ToUE(x.T.X, x.T.Y, x.T.Z));
		return T;
	}

	EKind KindOfRole(int32 Role)
	{
		switch (Role)
		{
			case GMR_SPAWN:       return EKind::Spawn;
			case GMR_CAPTURE:     return EKind::Capture;
			case GMR_ZONE:        return EKind::Zone;
			case GMR_COMBAT:      return EKind::Combat;
			case GMR_OBB:         return EKind::Obb;
			case GMR_VEHICLE:     return EKind::Vehicle;
			case GMR_RESUPPLY:    return EKind::Resupply;
			case GMR_MCOM:        return EKind::Mcom;
			case GMR_BOMB:        return EKind::Bomb;
			case GMR_SPECIALAREA: return EKind::SpecialArea;
			case GMR_SLOT:        return EKind::Slot;
			case GMR_UNLINKED:    return EKind::Unlinked;
			default:              return EKind::Other;
		}
	}

	// ---- geometry, all in Unreal space unless said otherwise --------------
	FVector Centroid(const TArray<FVector>& P)
	{
		if (P.Num() == 0) return FVector::ZeroVector;
		FVector c = FVector::ZeroVector;
		for (const FVector& p : P) c += p;
		return c / (double)P.Num();
	}

	bool InsidePolygonXY(const TArray<FVector>& P, const FVector& Q)
	{
		bool In = false;
		for (int32 i = 0, j = P.Num() - 1; i < P.Num(); j = i++)
			if (((P[i].Y > Q.Y) != (P[j].Y > Q.Y)) &&
			    (Q.X < (P[j].X - P[i].X) * (Q.Y - P[i].Y) / (P[j].Y - P[i].Y) + P[i].X)) In = !In;
		return In;
	}

	double Dist2D(const FVector& a, const FVector& b) { return FVector2D(a.X - b.X, a.Y - b.Y).Size(); }

	// The tool's containment test wants the loop wound the way its own square
	// volumes are: negative shoelace area in Unreal XY. Data winding is not
	// normalised (32 CW against 20 CCW measured), so it is enforced here and
	// the tool's own safe fix is run afterwards as the second opinion.
	void WindLikeTheTool(TArray<FVector>& P)
	{
		double a = 0.0;
		for (int32 i = 0; i < P.Num(); i++) { const FVector& c = P[i]; const FVector& d = P[(i + 1) % P.Num()]; a += c.X * d.Y - d.X * c.Y; }
		if (a > 0.0) Algo::Reverse(P);
	}

	FString Letter(int32 i)
	{
		return i < 26 ? FString::Chr((TCHAR)(TEXT('A') + i)) : FString::FromInt(i + 1);
	}

	FString SafeName(const FString& In)
	{
		FString Out;
		for (TCHAR c : In) Out.AppendChar(FChar::IsAlnum(c) || c == TEXT('_') || c == TEXT('-') ? c : TEXT('_'));
		return Out.IsEmpty() ? TEXT("Object") : Out;
	}

	// ---- the mine ---------------------------------------------------------
	void FinishMine(TMap<FString, FMode>&& Modes, const FString& Summary, const FString& Error, const FString& Level)
	{
		GMining = false;
		// The native worker can finish while OnEnginePreExit waits for it. Its
		// already-queued game-thread completion must not publish UI state or
		// apply a mode after shutdown has begun.
		if (IsEngineExitRequested()) { GApplyAfterMine = false; return; }
		if (Level != GLevel) return;   // the map changed under the worker
		if (!Error.IsEmpty())
		{
			UE_LOG(LogBF6HighPolyGM, Warning, TEXT("game modes: %s"), *Error);
			BF6Ext::Notify(FString::Printf(TEXT("Game modes: %s"), *Error));
			GApplyAfterMine = false;
			return;
		}
		GModes = MoveTemp(Modes);
		GMined = true;
		GMineSummary = Summary;
		UE_LOG(LogBF6HighPolyGM, Log, TEXT("game modes: %s"), *Summary);
		for (const TPair<FString, FMode>& kv : GModes)
		{
			const FMode& M = kv.Value;
			TMap<EKind, int32> C;
			for (const FObject& O : M.Objects) C.FindOrAdd(O.Kind)++;
			UE_LOG(LogBF6HighPolyGM, Log,
				TEXT("mode %s: %d capture, %d zone, %d combat, %d box, %d spawn, %d vehicle, %d resupply, %d mcom, %d bomb, ")
				TEXT("%d special area, %d slot, %d unlinked (%d layer(s)); dropped junk %d, owned %d, prop box %d, gem %d, dup %d, twin %d; ")
				TEXT("rescued %d%s"),
				*kv.Key, C.FindRef(EKind::Capture), C.FindRef(EKind::Zone), C.FindRef(EKind::Combat), C.FindRef(EKind::Obb),
				C.FindRef(EKind::Spawn), C.FindRef(EKind::Vehicle), C.FindRef(EKind::Resupply), C.FindRef(EKind::Mcom),
				C.FindRef(EKind::Bomb), C.FindRef(EKind::SpecialArea), C.FindRef(EKind::Slot), C.FindRef(EKind::Unlinked),
				M.Layers, M.DroppedJunk, M.DroppedOwned, M.DroppedPropBox, M.DroppedGemOther, M.DroppedDup, M.DroppedTwin,
				M.BigFlagRescued, M.bSanityCapped ? TEXT("; SANITY CAPPED: too many flags claimed, every polygon shipped as a zone") : TEXT(""));
		}
		if (GApplyAfterMine)
		{
			GApplyAfterMine = false;
			SetMode(GChosen);
		}
	}

	// HOW MANY WORKERS ARE INSIDE THIS MODULE'S READER RIGHT NOW.
	//
	// Shutdown closes the context, and on 8 September 2026 it closed it while
	// this miner was still walking it: EXCEPTION_ACCESS_VIOLATION with two
	// bf6_core frames on the stack, as the editor exited. The miner is started
	// and forgotten, so there is no future to wait on; this is the join.
	std::atomic<int32> GMineWorkers{ 0 };
	std::atomic<bool> GMineClosing{ false };

	void MineOffThread(const FString& Level, const FString& Install, const FString& Dll)
	{
		ON_SCOPE_EXIT{ GMineWorkers.fetch_sub(1); };
		if (GMineClosing.load()) return;
		TMap<FString, FMode> Modes;
		FString Summary, Error;
		const double T0 = FPlatformTime::Seconds();
		if (!GCore.Bind(Install, Dll)) Error = GCore.Error;
		else
		{
			char err[512] = {0};
			FGmStatsAbi St{};
			const int n = GCore.GameModes(GCore.Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0, &St, err, sizeof(err));
			if (n <= 0) Error = err[0] ? UTF8_TO_TCHAR(err) : TEXT("no gameplay entities found");
			else
			{
				// The entity rows: the layout refers back to them by index, and
				// only they carry the rotation, the type name and a box's size.
				TArray<FGmEntityAbi> Rows;
				Rows.SetNumZeroed(n);
				GCore.GameModes(GCore.Ctx, TCHAR_TO_UTF8(*Level), Rows.GetData(), n, nullptr, err, sizeof(err));

				struct FEnt { FGameXf Xf; FString TypeName; FVector Half; bool bEnabled; };
				TArray<FEnt> Ents;
				Ents.SetNum(n);
				TArray<FString> ModeKeys;
				TSet<FString>   Seen;
				TMap<FString, TSet<FString>> LayersOf;
				for (int32 i = 0; i < n; i++)
				{
					const FGmEntityAbi& e = Rows[i];
					FEnt& E = Ents[i];
					E.Xf = XfOf(e.xform);
					E.TypeName = e.type_name ? UTF8_TO_TCHAR(e.type_name) : TEXT("");
					E.Half = FVector(e.half_extents[0], e.half_extents[1], e.half_extents[2]);
					E.bEnabled = e.enabled != 0;
					const FString Mode = e.mode ? UTF8_TO_TCHAR(e.mode) : TEXT("");
					if (Mode.IsEmpty()) continue;
					if (!Seen.Contains(Mode)) { Seen.Add(Mode); ModeKeys.Add(Mode); }
					LayersOf.FindOrAdd(Mode).Add(e.layer ? UTF8_TO_TCHAR(e.layer) : TEXT(""));
				}

				// One layout call per mode. Its buffers belong to the context
				// and are good only until the next call, so each is copied out
				// before the next mode is asked for.
				for (const FString& Key : ModeKeys)
				{
					FGmLayoutAbi L{};
					err[0] = 0;
					const int on = GCore.Layout(GCore.Ctx, TCHAR_TO_UTF8(*Level), TCHAR_TO_UTF8(*Key), nullptr, 0, &L, err, sizeof(err));
					if (on <= 0) continue;
					TArray<FGmObjectAbi> Objs;
					Objs.SetNumZeroed(on);
					GCore.Layout(GCore.Ctx, TCHAR_TO_UTF8(*Level), TCHAR_TO_UTF8(*Key), Objs.GetData(), on, &L, err, sizeof(err));

					FMode M;
					M.Key = Key;
					M.Layers = LayersOf.FindRef(Key).Num();
					M.Gems = L.gems; M.GemsUnlinked = L.gems_unlinked;
					M.DroppedJunk = L.dropped_junk; M.DroppedOwned = L.dropped_owned;
					M.DroppedPropBox = L.dropped_prop_box; M.DroppedGemOther = L.dropped_gem_other;
					M.DroppedDup = L.dropped_dup; M.DroppedTwin = L.dropped_twin;
					M.BigFlagRescued = L.big_flag_rescued; M.bSanityCapped = L.sanity_capped != 0;
					M.WaterY = L.water_y; M.bHasTerrain = L.has_terrain != 0;
					if (L.grid_h && L.grid_nx > 1 && L.grid_nz > 1 && int64(L.grid_nx) * L.grid_nz <= 4000000)
					{
						M.Grid.X0 = L.grid_x0; M.Grid.Z0 = L.grid_z0; M.Grid.Step = L.grid_step;
						M.Grid.Nx = L.grid_nx; M.Grid.Nz = L.grid_nz;
						M.Grid.H.Append(L.grid_h, L.grid_nx * L.grid_nz);
					}
					M.Objects.Reserve(on);
					for (const FGmObjectAbi& o : Objs)
					{
						const FGmEntityAbi& e = Rows.IsValidIndex(o.entity) ? Rows[o.entity] : Rows[0];
						const FEnt& E = Ents.IsValidIndex(o.entity) ? Ents[o.entity] : Ents[0];
						FObject O;
						O.Kind = KindOfRole(o.role);
						O.Label = o.label ? UTF8_TO_TCHAR(o.label) : TEXT("");
						O.TypeName = E.TypeName;
						O.GemLink = o.gem_link ? UTF8_TO_TCHAR(o.gem_link) : FString();
						O.GemValue = o.gem_value;
						O.Team = o.team;
						O.bEnabled = E.bEnabled;
						O.AreaM2 = o.area_m2;
						O.FlagIndex = o.flag;
						O.bStationary = o.stationary != 0;
						O.bByData = o.by_data != 0;
						O.HeightM = FMath::Max(o.height, 0.f);
						O.SizeGodot = FVector(e.half_extents[0], e.half_extents[1], e.half_extents[2]) * 2.0;
						O.bHasGround = o.has_ground != 0;
						O.GroundZ = o.ground_y * (float)kCm;
						O.bWater = o.water != 0;
						O.bHasLand = o.has_land != 0;
						O.Land = ToUE(o.land[0], o.land[1], o.land[2]);
						if (o.point_count >= 3 && o.world_points)
						{
							double Lo = TNumericLimits<double>::Max();
							for (int32 k = 0; k < o.point_count; k++)
							{
								O.Points.Add(ToUE(o.world_points[k * 3], o.world_points[k * 3 + 1], o.world_points[k * 3 + 2]));
								Lo = FMath::Min(Lo, (double)o.world_points[k * 3 + 1]);
							}
							// the polygon's own node sits on its lowest point, so a
							// volume rests on the ground rather than in mid-air
							O.Xf = FTransform(ToUE(o.centre[0], Lo, o.centre[2]));
						}
						else if (O.Kind == EKind::Obb) { O.Xf = ToTransform(E.Xf); O.Xf.SetScale3D(FVector::OneVector); }
						else O.Xf = ToPlacement(E.Xf);
						M.Objects.Add(MoveTemp(O));
					}
					Modes.Add(Key, MoveTemp(M));
				}

				FString Others;
				for (int32 i = 0; i < St.other_type_count && i < 8; i++)
					Others += FString::Printf(TEXT("%s%s"), i ? TEXT(", ") : TEXT(""), UTF8_TO_TCHAR(St.other_types[i]));
				Summary = FString::Printf(
					TEXT("%s: %d entities on %d layer(s) (%d skipped) in %d mode(s), %d partition(s), %d instance(s), %.1f s; ")
					TEXT("spawns %d volumes %d boxes %d combat %d gems %d (%d with a template); ")
					TEXT("fake-root control %d; parse fail %d missing %d unresolved types %d; other types on the layers: %s"),
					*Level, n, St.layers, St.layers_skipped, St.modes, St.partitions, St.instances,
					FPlatformTime::Seconds() - T0,
					St.spawns, St.volumes, St.obbs, St.combat, St.gems, St.gems_linked, St.control_fake_root_hits,
					St.parse_fail, St.missing, St.unresolved_types, Others.IsEmpty() ? TEXT("none") : *Others);
				if (St.control_fake_root_hits > 0)
					Summary += TEXT("  WARNING: the fake-root negative control matched, the name index is loose");
			}
		}
		AsyncTask(ENamedThreads::GameThread, [Modes = MoveTemp(Modes), Summary, Error, Level]() mutable
		{
			FinishMine(MoveTemp(Modes), Summary, Error, Level);
		});
	}

	// ---- the art --------------------------------------------------------
	AActor* ArtActor()
	{
		if (!GEditor) return nullptr;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return nullptr;
		const FName Owner(kArtOwner);
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(Owner) && It->GetActorLabel() == TEXT("HighPoly")) return *It;
		return nullptr;
	}

	// The art tag carries the bundle segment as the main build saw it
	// ("conquest", "carrierstrike", "mp_koth0"); the miner folds the same
	// spellings into one key ("koth"). Both sides fold here so a mode is one
	// entry on the ring whichever way the level spelled it.
	FString Fold(const FString& In)
	{
		FString S = In.ToLower();
		int32 Slash;
		if (S.FindChar(TEXT('/'), Slash)) S = S.Left(Slash);
		for (int32 i = 0; i + 1 < S.Len(); i++)
		{
			if (!FChar::IsDigit(S[i])) continue;
			int32 j = i;
			while (j < S.Len() && FChar::IsDigit(S[j])) j++;
			if (j < S.Len() && S[j] == TEXT('_') && i > 0) { S.LeftInline(i); break; }
		}
		while (S.Len() > 1 && FChar::IsDigit(S[S.Len() - 1])) S.LeftChopInline(1);
		if (S.StartsWith(TEXT("mp_")) && S.Len() > 3) S.RightChopInline(3);
		return S;
	}

	FString ArtModeOf(const UActorComponent* C)
	{
		for (const FName& T : C->ComponentTags)
		{
			const FString S = T.ToString();
			if (S.StartsWith(kArtTag)) return Fold(S.Mid(FCString::Strlen(kArtTag)));
		}
		return FString();
	}

	// mode key -> instance count, from the tags the main build left
	void ArtModes(TMap<FString, int32>& Out)
	{
		Out.Reset();
		AActor* A = ArtActor();
		if (!A) return;
		TInlineComponentArray<UPrimitiveComponent*> Prims;
		A->GetComponents(Prims);
		for (UPrimitiveComponent* P : Prims)
		{
			const FString M = ArtModeOf(P);
			if (M.IsEmpty()) continue;
			int32 n = 1;
			if (const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(P)) n = I->GetInstanceCount();
			Out.FindOrAdd(M.ToLower()) += n;
		}
	}

	FString LargestArtMode()
	{
		TMap<FString, int32> M;
		ArtModes(M);
		FString Best; int32 BestN = -1;
		for (const TPair<FString, int32>& kv : M) if (kv.Value > BestN) { BestN = kv.Value; Best = kv.Key; }
		return Best;
	}

	// Hide every mode's art but the chosen one's. Untagged art is shared and
	// stays. "all" stacks them, which is what the old build did.
	int32 ApplyArt()
	{
		AActor* A = ArtActor();
		GArtActorSeen = A;
		if (!A) return 0;
		FString Want = GChosen.IsEmpty() ? LargestArtMode() : GChosen;
		const bool bAll = Want.Equals(TEXT("all"), ESearchCase::IgnoreCase);
		int32 Changed = 0, Shown = 0, Hidden = 0;
		TInlineComponentArray<UPrimitiveComponent*> Prims;
		A->GetComponents(Prims);
		for (UPrimitiveComponent* P : Prims)
		{
			const FString M = ArtModeOf(P);
			if (M.IsEmpty()) continue;
			const bool bShow = bAll || M.Equals(Want, ESearchCase::IgnoreCase);
			if (P->IsVisible() != bShow) { P->SetVisibility(bShow, true); Changed++; }
			bShow ? Shown++ : Hidden++;
		}
		if (Changed) BF6Ext::RedrawBuildViewport();
		UE_LOG(LogBF6HighPolyGM, Log, TEXT("game modes: art set to %s - %d tagged component(s) shown, %d hidden, %d changed"),
			bAll ? TEXT("all modes") : *Want, Shown, Hidden, Changed);
		return Changed;
	}

	// ---- the scene ------------------------------------------------------
	FString GroupLabel(const FString& Key) { return Pretty(Key).Replace(TEXT(" "), TEXT("")); }

	AActor* FindGroup(const FString& Key)
	{
		if (!GEditor) return nullptr;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return nullptr;
		const FName Mine(*(FString(kNodeTag) + Key));
		const FString Label = GroupLabel(Key);
		AActor* ByLabel = nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (It->Tags.Contains(Mine)) return *It;
			// after a reload the session tag is gone; the node keeps its name
			if (!ByLabel && It->Tags.Contains(kGroupTag) && !It->GetAttachParentActor())
			{
				FString L = It->GetActorLabel(); L.RemoveFromStart(TEXT("BF6_"));
				if (L == Label) ByLabel = *It;
			}
		}
		if (ByLabel) ByLabel->Tags.AddUnique(Mine);
		return ByLabel;
	}

	// The add-on actor carrying a mode's names, or markers. Never exported,
	// and filed in one outliner folder so it is never mixed with content.
	AActor* FindAddonActor(const FString& Label)
	{
		if (!GEditor) return nullptr;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return nullptr;
		const FName Owner(*(FString(TEXT("addon:")) + kAddon));
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(Owner) && It->GetActorLabel() == Label) return *It;
		return nullptr;
	}
	AActor* MakeAddonActor(const FString& Label)
	{
		if (AActor* Old = FindAddonActor(Label)) Old->Destroy();
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) return nullptr;
		FActorSpawnParameters SP;
		SP.ObjectFlags = RF_Transient;
		AActor* A = W->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SP);
		if (!A) return nullptr;
		A->SetActorLabel(Label);
		A->SetFolderPath(FName(kFolder));
		BF6Ext::MarkAddonActor(A, kAddon);
		USceneComponent* Root = NewObject<USceneComponent>(A, TEXT("Root"));
		A->SetRootComponent(Root);
		Root->RegisterComponent();
		return A;
	}
	FString LabelsName(const FString& Key)  { return FString(TEXT("HighPolyGameModeLabels_")) + Key; }
	FString MarkersName(const FString& Key) { return FString(TEXT("HighPolyGameModeMarkers_")) + Key; }

	int32 CountObjects(AActor* Node)
	{
		TArray<AActor*> Kids;
		Node->GetAttachedActors(Kids, true, true);
		return Kids.Num();
	}

	// The tool's own marker for "this is not part of what should be exported".
	// It has to be spelled the same on both sides of the seam; the exporter
	// declares it as kNoExportTag.
	const FName kNoExportTag(TEXT("BF6NoExport"));

	void SetSubtreeHidden(AActor* Root, bool bHidden)
	{
		if (!Root) return;
		Root->SetIsTemporarilyHiddenInEditor(bHidden);
		TArray<AActor*> Kids;
		Root->GetAttachedActors(Kids, true, true);
		for (AActor* K : Kids) K->SetIsTemporarilyHiddenInEditor(bHidden);

		// HIDING A VARIANT ALSO TAKES IT OUT OF THE EXPORT.
		//
		// Hiding was doing that job by accident and failing at it: the spatial
		// exporter does not look at visibility, so a map built for Conquest and
		// then Breakthrough exported both sets of objectives, with their ObjIds
		// colliding. Saying it explicitly is what makes the exporter able to act
		// on it without having to guess from the hidden flag, which people also
		// set for their own convenience.
		//
		// Only the group node is marked. The exporter walks up the attachment
		// chain, so everything under it is covered without touching each actor,
		// and un-hiding removes it again in one place.
		if (bHidden) { Root->Tags.AddUnique(kNoExportTag); }
		else         { Root->Tags.Remove(kNoExportTag); }
	}

	// "Conquest: Flag A" over the flag, "Conquest: Flag A Spawn" over each of
	// its spawns. An add-on actor, rebuilt whenever a mode is shown.
	void Relabel(const FString& Key, const FMode& M)
	{
		AActor* A = MakeAddonActor(LabelsName(Key));
		if (!A) return;
		const FString Title = Pretty(Key);
		int32 n = 0;
		for (const FObject& O : M.Objects)
		{
			if (O.Kind == EKind::Other) continue;
			UTextRenderComponent* T = NewObject<UTextRenderComponent>(A, *FString::Printf(TEXT("L_%d"), n++));
			T->SetupAttachment(A->GetRootComponent());
			T->SetText(FText::FromString(Title + TEXT(": ") + O.Label));
			T->SetTextRenderColor(Tint(O.Kind).ToFColor(true));
			T->SetWorldSize(O.Kind == EKind::Capture ? 120.f : 60.f);
			T->SetHorizontalAlignment(EHTA_Center);
			T->SetVerticalAlignment(EVRTA_TextCenter);
			T->SetWorldLocation(O.Xf.GetLocation() + FVector(0, 0, O.Kind == EKind::Capture ? 900.f : 220.f));
			T->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			T->RegisterComponent();
		}
	}

	// A picture of the mode, for when the tool cannot place: a read-only base
	// map, or the placement seam not yet in the tool. Add-on actors; nothing
	// here is content.
	void BuildMarkers(const FString& Key, const FMode& M)
	{
		AActor* A = MakeAddonActor(MarkersName(Key));
		if (!A) return;
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		UStaticMesh* Cyl  = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		UStaticMesh* Sph  = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/BF6UnrealSDK/Materials/M_NeonHighlight.M_NeonHighlight"));
		if (!Cube || !Cyl || !Sph) return;
		int32 n = 0;
		auto Add = [&](UStaticMesh* Mesh, const FTransform& Xf, EKind K)
		{
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(A, *FString::Printf(TEXT("M_%d"), n++));
			C->SetupAttachment(A->GetRootComponent());
			BF6HP::Shared::MakeUnselectable(C);
			C->SetStaticMesh(Mesh);
			C->SetWorldTransform(Xf);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			if (Base)
				if (UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, C))
				{
					Mid->SetVectorParameterValue(TEXT("Color"), Tint(K));
					Mid->SetVectorParameterValue(TEXT("Tint"), Tint(K));
					C->SetMaterial(0, Mid);
				}
			C->RegisterComponent();
		};
		for (const FObject& O : M.Objects)
		{
			switch (O.Kind)
			{
				case EKind::Spawn:
					Add(Sph, FTransform(O.Xf.GetRotation(), O.Xf.GetLocation() + FVector(0, 0, 60), FVector(0.6)), O.Kind);
					Add(Cube, FTransform(O.Xf.GetRotation(), O.Xf.GetLocation() + O.Xf.GetRotation().RotateVector(FVector(70, 0, 60)), FVector(0.8, 0.15, 0.15)), O.Kind);
					break;
				case EKind::Obb:
					Add(Cube, FTransform(O.Xf.GetRotation(), O.Xf.GetLocation(),
						FVector(O.SizeGodot.X, O.SizeGodot.Z, O.SizeGodot.Y)), O.Kind);
					break;
				case EKind::Vehicle: case EKind::Slot:
					Add(Cube, FTransform(O.Xf.GetRotation(), O.Xf.GetLocation() + FVector(0, 0, 100), FVector(4.0, 2.0, 1.5)), O.Kind);
					break;
				case EKind::Mcom: case EKind::Bomb: case EKind::Resupply:
					Add(Cube, FTransform(O.Xf.GetRotation(), O.Xf.GetLocation() + FVector(0, 0, 60), FVector(1.2, 1.2, 1.2)), O.Kind);
					break;
				default: break;
			}
			if (O.Points.Num() >= 3)
			{
				const float H = FMath::Max(O.HeightM, 2.f) * 100.f;
				for (int32 i = 0; i < O.Points.Num(); i++)
				{
					const FVector a = O.Points[i], b = O.Points[(i + 1) % O.Points.Num()];
					const FVector d = b - a;
					const double L = d.Size2D();
					if (L < 1.0) continue;
					const FQuat Q = FRotationMatrix::MakeFromX(FVector(d.X, d.Y, 0)).ToQuat();
					Add(Cube, FTransform(Q, (a + b) * 0.5 + FVector(0, 0, H * 0.5), FVector(L / 100.0, 0.2, H / 100.0)), O.Kind);
				}
				if (O.Kind == EKind::Capture)
					Add(Cyl, FTransform(FQuat::Identity, O.Xf.GetLocation() + FVector(0, 0, 400), FVector(0.3, 0.3, 8.0)), O.Kind);
			}
		}
		UE_LOG(LogBF6HighPolyGM, Log, TEXT("mode %s: drawn as %d marker(s), not placed - %s"),
			*Key, n, BF6Ext::IsEditing() ? TEXT("the placement seam is not in the tool (apply patch 01)")
			                              : TEXT("this is a read-only base map; create a custom map to place them"));
	}

#if defined(BF6EXT_HAS_PLACEMENT)

	// ========================================================================
	// THE BUILD CONTEXT
	//
	// Everything a builder places goes through this, for three reasons: the
	// scene tree stays organised (every object has a named parent, nothing is
	// left flat at the mode node), the build checks can run over what was
	// placed rather than walking the world, and a type the tool does not have
	// is counted as skipped rather than guessed at.
	// ========================================================================
	struct FPlacedRec
	{
		AActor* A = nullptr;
		FString Type;
		FString Name;                // the LOGICAL name ("TEAM_1_HQ"), not the label
		int32   ObjId = -1;
		bool    bWiredArea = true;   // false = a link field this type must have was left empty
	};

	struct FBuild
	{
		AActor*  Node = nullptr;
		FString  Key;
		FString  Prefix;                               // "Conquest_", see MakeLabel
		TArray<FPlacedRec> Placed;
		TMap<FString, int32> Skipped;
		TArray<FString> Notes;
		int32    Links = 0;
		TMap<FString, AActor*> Groups;                 // "Vehicles/Team1" -> node
		TSet<AActor*> MadeNodes;                       // only the nodes THIS made, for the empty sweep
		TMap<FString, TSet<FString>> PropsOfType;      // type -> field names
		FVector  Centre = FVector::ZeroVector;

		bool Has(const FString& Type) const { return BF6Ext::HasPlaceable(Type); }

		// EVERY LABEL CARRIES ITS MODE, because a label is unique across the
		// whole world and the tool makes it so by suffixing. The base map's own
		// setup already holds TEAM_1_HQ, HQ_Team1, CombatArea and SpawnPoint_1_1,
		// so a mode that asks for those names silently became TEAM_1_HQ2 and
		// HQ_Team3 - the right objects with the wrong names, and two modes built
		// in one scene would collide with each other as well. The prefix is what
		// the mode node gives a Godot build for free. The export does not care:
		// it minifies every name through its own short-name map and resolves
		// links through the same map, and the mode script addresses objects by
		// ObjId, which is set explicitly here.
		FString MakeLabel(const FString& Name) const { return Prefix + Name; }

		bool HasProp(const FString& Type, const FString& Field)
		{
			if (TSet<FString>* S = PropsOfType.Find(Type)) return S->Contains(Field);
			TArray<TPair<FString, FString>> Defs;
			BF6Ext::ObjectPropDefs(Type, Defs);
			TSet<FString>& S = PropsOfType.Add(Type);
			for (const TPair<FString, FString>& D : Defs) S.Add(D.Key);
			return S.Contains(Field);
		}

		// A named node under the mode node, made once and reused. "A/B" nests.
		AActor* Group(const FString& Path)
		{
			if (Path.IsEmpty()) return Node;
			if (AActor** Found = Groups.Find(Path)) return *Found;
			FString Parent, Leaf;
			if (!Path.Split(TEXT("/"), &Parent, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{ Parent.Reset(); Leaf = Path; }
			AActor* Host = Parent.IsEmpty() ? Node : Group(Parent);
			// A "Team1" leaf repeats under every parent, and labels are world-unique,
			// so the tool would suffix it (Team3, Team5...). Carry the parent leaf.
			FString Label = Leaf;
			if (!Parent.IsEmpty() && Leaf.StartsWith(TEXT("Team")))
			{
				FString PP, PL;
				if (!Parent.Split(TEXT("/"), &PP, &PL, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) PL = Parent;
				Label = PL + TEXT("_") + Leaf;
			}
			AActor* N = MakeNode(Label, Host);
			if (!N) return Host;
			Groups.Add(Path, N);
			return N;
		}

		// A node under a specific actor rather than under a group path: the
		// SpawnsA node belongs to CapturePointA, not beside it.
		AActor* GroupUnder(const FString& Key2, AActor* Parent, const FString& Leaf)
		{
			if (AActor** Found = Groups.Find(Key2)) return *Found;
			AActor* N = MakeNode(Leaf, Parent);
			if (!N) return Parent;
			Groups.Add(Key2, N);
			return N;
		}

		AActor* MakeNode(const FString& Leaf, AActor* Parent)
		{
			AActor* N = BF6Ext::PlaceNode(MakeLabel(Leaf), Centre);
			if (!N) { Skipped.FindOrAdd(TEXT("Node3D"))++; return nullptr; }
			// PARENT ON THE WAY IN, ALWAYS. The tool refuses to re-parent an
			// actor that already has one ("never steal one already placed in a
			// tree"), so a second ParentUnder is a silent no-op: the first build
			// asked for the group first and the real owner second, and every
			// area, spawn and bomb came out flat beside its owner instead of
			// under it. Nothing here is ever parented twice.
			BF6Ext::ParentUnder(N, Parent ? Parent : Node);
			MadeNodes.Add(N);
			return N;
		}

		// The one placement call. `Parent` is the FINAL parent, because there is
		// no second chance at it (see MakeNode).
		AActor* Place(const FString& Type, const FTransform& Xf, const FString& Name, AActor* Parent)
		{
			if (!Has(Type)) { Skipped.FindOrAdd(Type)++; return nullptr; }
			AActor* A = BF6Ext::PlaceObject(Type, Xf);
			if (!A) { Skipped.FindOrAdd(Type)++; return nullptr; }
			BF6Ext::SetObjectLabel(A, MakeLabel(Name));
			BF6Ext::ParentUnder(A, Parent ? Parent : Node);
			Placed.Add(FPlacedRec{ A, Type, Name, -1, true });
			return A;
		}
		AActor* PlaceIn(const FString& Type, const FTransform& Xf, const FString& Name, const FString& GroupPath)
		{
			return Place(Type, Xf, Name, Group(GroupPath));
		}
		// the same, for the many sites whose parent IS just a group
		AActor* Place(const FString& Type, const FTransform& Xf, const FString& Name, const FString& GroupPath)
		{
			return Place(Type, Xf, Name, Group(GroupPath));
		}

		// A PolygonVolume carrying a mined outline. Height 0 is not a mistake:
		// on the 1.4.2.0 combat-area model a zero height means everything BELOW
		// the volume's own origin, which is why a combat area's origin is put
		// above the highest thing in the mode rather than on the ground.
		AActor* Volume(const TArray<FVector>& WorldPts, const FVector& Pos, double HeightM,
		               const FString& Name, AActor* Parent)
		{
			if (WorldPts.Num() < 3) return nullptr;
			AActor* V = Place(TEXT("PolygonVolume"), FTransform(Pos), Name, Parent);
			if (!V) return nullptr;
			TArray<FVector> Loop = WorldPts;
			for (FVector& P : Loop) P.Z = Pos.Z;
			WindLikeTheTool(Loop);
			BF6Ext::SetVolumeLoop(V, Loop);
			Prop(V, TEXT("height"), FString::Printf(TEXT("%g"), HeightM));
			return V;
		}
		AActor* Volume(const TArray<FVector>& WorldPts, const FVector& Pos, double HeightM,
		               const FString& Name, const FString& GroupPath)
		{
			return Volume(WorldPts, Pos, HeightM, Name, Group(GroupPath));
		}

		void Prop(AActor* A, const FString& Field, const FString& Value)
		{
			if (!A) return;
			const FPlacedRec* R = Rec(A);
			if (R && !HasProp(R->Type, Field)) return;
			BF6Ext::SetObjectProp(A, Field, Value);
			if (Field == TEXT("ObjId")) SetObjId(A, FCString::Atoi(*Value));
		}
		void PropInt(AActor* A, const FString& Field, int32 V) { Prop(A, Field, FString::FromInt(V)); }
		void PropBool(AActor* A, const FString& Field, bool b) { Prop(A, Field, b ? TEXT("true") : TEXT("false")); }

		// A link field takes the target's link name; an array field takes them
		// comma-joined. Wiring is counted, and a field this type must have but
		// could not be filled is what check 2 reports.
		bool Link(AActor* A, const FString& Field, AActor* Target)
		{
			if (!A || !Target) return false;
			const FPlacedRec* R = Rec(A);
			if (R && !HasProp(R->Type, Field)) return false;
			BF6Ext::SetObjectProp(A, Field, BF6Ext::ObjectLinkName(Target));
			Links++;
			return true;
		}
		bool LinkList(AActor* A, const FString& Field, const TArray<AActor*>& Targets)
		{
			if (!A || Targets.Num() == 0) return false;
			const FPlacedRec* R = Rec(A);
			if (R && !HasProp(R->Type, Field)) return false;
			TArray<FString> Names;
			for (AActor* T : Targets) if (T) Names.Add(BF6Ext::ObjectLinkName(T));
			if (Names.Num() == 0) return false;
			BF6Ext::SetObjectProp(A, Field, FString::Join(Names, TEXT(",")));
			Links += Names.Num();
			return true;
		}

		FPlacedRec* Rec(AActor* A)
		{
			for (FPlacedRec& R : Placed) if (R.A == A) return &R;
			return nullptr;
		}
		const FPlacedRec* Rec(AActor* A) const
		{
			for (const FPlacedRec& R : Placed) if (R.A == A) return &R;
			return nullptr;
		}
		void SetObjId(AActor* A, int32 Id) { if (FPlacedRec* R = Rec(A)) R->ObjId = Id; }
		void MarkUnwired(AActor* A) { if (FPlacedRec* R = Rec(A)) R->bWiredArea = false; }
		void Note(const FString& S) { Notes.Add(S); }

		// A group that ended up holding nothing is scaffolding the creator did
		// not ask for (Vehicles/Team1 on a map whose pads all belong to flags).
		// Only nodes this build made are ever touched.
		int32 SweepEmptyGroups()
		{
			UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!W) return 0;
			int32 n = 0;
			for (auto It = Groups.CreateIterator(); It; ++It)
			{
				AActor* N = It.Value();
				if (!N || !MadeNodes.Contains(N)) continue;
				TArray<AActor*> Kids;
				N->GetAttachedActors(Kids, true, true);
				if (Kids.Num() > 0) continue;
				N->Modify();
				W->EditorDestroyActor(N, true);
				MadeNodes.Remove(N);
				It.RemoveCurrent();
				n++;
			}
			return n;
		}
	};

	// ---- the mined objects, sorted into the lists a builder works from ----
	struct FPoly
	{
		const FObject* O = nullptr;
		TArray<FVector> Pts;      // world, Unreal
		FVector C = FVector::ZeroVector;
		double  Area = 0.0;       // game m2, as mined
		TArray<int32> Flags;      // indices into the flag list, filled where wanted
		bool    bSynth = false;
	};

	struct FSorted
	{
		TArray<FPoly> Flags, Zones;
		TArray<const FObject*> Spawns, Vehicles, Slots, Resupplies, Unlinked, Areas, Mcoms, Bombs, Boxes;
		TArray<FPoly> Combat;     // polygons a combat entity claimed
		double CeilingCm = 25000.0;
	};

	void Sort(const FMode& M, FSorted& S)
	{
		double HiZ = -TNumericLimits<double>::Max();
		for (const FObject& O : M.Objects)
		{
			HiZ = FMath::Max(HiZ, O.Xf.GetLocation().Z);
			switch (O.Kind)
			{
				case EKind::Capture: case EKind::Zone: case EKind::Combat:
				{
					FPoly P; P.O = &O; P.Pts = O.Points; P.C = Centroid(O.Points); P.Area = O.AreaM2;
					if (O.Kind == EKind::Capture) S.Flags.Add(MoveTemp(P));
					else if (O.Kind == EKind::Zone) S.Zones.Add(MoveTemp(P));
					else S.Combat.Add(MoveTemp(P));
					break;
				}
				case EKind::Spawn:       S.Spawns.Add(&O); break;
				case EKind::Vehicle:     S.Vehicles.Add(&O); break;
				case EKind::Slot:        S.Slots.Add(&O); break;
				case EKind::Resupply:    S.Resupplies.Add(&O); break;
				case EKind::Unlinked:    S.Unlinked.Add(&O); break;
				case EKind::SpecialArea: S.Areas.Add(&O); break;
				case EKind::Mcom:        S.Mcoms.Add(&O); break;
				case EKind::Bomb:        S.Bombs.Add(&O); break;
				case EKind::Obb:         S.Boxes.Add(&O); break;
				default: break;
			}
		}
		// HEIGHT 0 IS "EVERYTHING BELOW THE ORIGIN", so a combat volume's own
		// origin has to sit above the highest gameplay object on the map. No
		// per-map table: the mined objects say where the top is.
		S.CeilingCm = FMath::Max(FMath::CeilToDouble((HiZ + 150.0 * kCm) / (10.0 * kCm)) * 10.0 * kCm, 250.0 * kCm);
	}

	// ---- the pieces every builder shares ---------------------------------

	// The game's deploy area inside a polygon: loose deploy spawns inside it,
	// greedily clustered at 45 m; the cluster nearest `Toward` (the sector
	// being fought over) wins, a vehicle pad within 60 m breaking ties in its
	// favour. False when no loose spawn lies inside the polygon.
	bool DeployClusterIn(const TArray<FVector>& Poly, const TArray<const FObject*>& Loose,
	                     const TArray<const FObject*>& Vehicles, const FVector& Toward,
	                     const TArray<FPoly>& Flags, FVector& Out)
	{
		struct FCl { FVector C; TArray<FVector> M; };
		TArray<FCl> Cl;
		for (const FObject* S : Loose)
		{
			const FVector P = S->Xf.GetLocation();
			if (!InsidePolygonXY(Poly, P)) continue;
			bool bOnFlag = false;
			for (const FPoly& F : Flags) if (Dist2D(P, F.C) < kOnFlagM * kCm) { bOnFlag = true; break; }
			if (bOnFlag) continue;                       // flag-side spawns are not an HQ deploy
			bool bPlaced = false;
			for (FCl& C : Cl)
				if (Dist2D(P, C.C) < kDeployClusterM * kCm)
				{
					C.M.Add(P);
					FVector Sum = FVector::ZeroVector;
					for (const FVector& m : C.M) Sum += m;
					C.C = Sum / (double)C.M.Num();
					bPlaced = true;
					break;
				}
			if (!bPlaced) { FCl N; N.C = P; N.M.Add(P); Cl.Add(N); }
		}
		if (Cl.Num() == 0) return false;
		double Best = TNumericLimits<double>::Max();
		for (const FCl& C : Cl)
		{
			double Score = Dist2D(C.C, Toward);
			for (const FObject* V : Vehicles)
				if (Dist2D(V->Xf.GetLocation(), C.C) < kPadTieM * kCm) { Score -= 150.0 * kCm; break; }
			if (Score < Best) { Best = Score; Out = C.C; }
		}
		return true;
	}

	// A team's vehicles, guns and resupply stations, built the same way for
	// every mode: the link word is not trusted where position disagrees with
	// it (the user checked this in game, 2026-09-04).
	struct FVehicleSort
	{
		TArray<const FObject*> Pads;         // built as VehicleSpawner
		TArray<const FObject*> Guns;         // built as StationaryEmplacementSpawner
		TArray<const FObject*> Stations;     // left as VehicleResupplyStation
		int32 FromResupplyVehicle = 0, FromResupplyGun = 0, GunInBase = 0;
	};

	// `InBase` answers "does this point stand inside a team's base", 0 = no.
	void SortVehicles(const FSorted& S, TFunctionRef<int32(const FVector&)> InBase,
	                  const TArray<FPoly>& Flags, FVehicleSort& Out)
	{
		for (const FObject* R : S.Resupplies)
		{
			const FVector P = R->Xf.GetLocation();
			if (InBase(P) != 0) { Out.Pads.Add(R); Out.FromResupplyVehicle++; continue; }
			double Near = TNumericLimits<double>::Max();
			for (const FPoly& F : Flags) Near = FMath::Min(Near, Dist2D(P, F.C));
			if (Near <= kRsObjectiveM * kCm) { Out.Guns.Add(R); Out.FromResupplyGun++; continue; }
			Out.Stations.Add(R);
		}
		for (const FObject* V : S.Vehicles)
		{
			if (!V->bStationary) { Out.Pads.Add(V); continue; }
			// a spawner inside a base is a vehicle pad whatever gem it came from
			if (InBase(V->Xf.GetLocation()) != 0) { Out.Pads.Add(V); Out.GunInBase++; }
			else Out.Guns.Add(V);
		}
	}

	// ---- build checks ----------------------------------------------------
	//
	// Every check here is a bug that shipped in a real map, so each one names
	// the failure it exists to stop. They run on what was just placed, need
	// neither the game nor a per-map table, and go in FRONT of the report: a
	// creator should never be the one who finds these.
	TArray<FString> RunChecks(FBuild& B, const FString& Key)
	{
		TArray<FString> Bad;

		// 1. ObjIds must be unique: the mode script addresses objects by ObjId,
		//    so a collision breaks capture or scoring with nothing on screen.
		TMap<int32, FString> Seen;
		for (const FPlacedRec& R : B.Placed)
		{
			if (R.ObjId <= 0) continue;
			if (const FString* Other = Seen.Find(R.ObjId))
				Bad.Add(FString::Printf(TEXT("ObjId %d is on both %s and %s"), R.ObjId, **Other, *R.Name));
			else Seen.Add(R.ObjId, R.Name);
		}

		// 2. Unwired link exports. An empty CombatVolume plays fine for one
		//    round, then deserts every bot at its own HQ after the side switch.
		for (const FPlacedRec& R : B.Placed)
		{
			if (R.bWiredArea) continue;
			const TCHAR* Field = R.Type == TEXT("CombatArea") ? TEXT("CombatVolume")
				: R.Type == TEXT("Sector") ? TEXT("SectorArea")
				: R.Type == TEXT("CapturePoint") ? TEXT("CaptureArea")
				: R.Type == TEXT("AreaTrigger") ? TEXT("Area")
				: TEXT("HQArea");
			Bad.Add(FString::Printf(TEXT("%s.%s is not wired"), *R.Name, Field));
		}

		// 3. P_AutoSpawnEnabled defaults to FALSE, so an unset pad is a vehicle
		//    or a gun that never appears in game. Three maps shipped like it,
		//    so rather than set it at each of the six places a spawner is
		//    emitted, every one of them is armed here, once.
		int32 Armed = 0;
		for (FPlacedRec& R : B.Placed)
		{
			if (R.Type != TEXT("VehicleSpawner") && R.Type != TEXT("StationaryEmplacementSpawner")) continue;
			if (!B.HasProp(R.Type, TEXT("P_AutoSpawnEnabled"))) continue;
			if (BF6Ext::ObjectProp(R.A, TEXT("P_AutoSpawnEnabled")).Equals(TEXT("true"), ESearchCase::IgnoreCase)) continue;
			BF6Ext::SetObjectProp(R.A, TEXT("P_AutoSpawnEnabled"), TEXT("true"));
			Armed++;
		}
		if (Armed > 0)
			B.Note(FString::Printf(TEXT("armed %d spawner(s) that would never have spawned"), Armed));

		// 4. Two capture points on one spot = one objective built twice, which
		//    is what overlapping mined zones produce.
		TArray<const FPlacedRec*> Caps;
		for (const FPlacedRec& R : B.Placed) if (R.Type == TEXT("CapturePoint")) Caps.Add(&R);
		for (int32 i = 0; i < Caps.Num(); i++)
			for (int32 j = i + 1; j < Caps.Num(); j++)
			{
				if (!Caps[i]->A || !Caps[j]->A) continue;
				const double d = Dist2D(Caps[i]->A->GetActorLocation(), Caps[j]->A->GetActorLocation());
				if (d < kFlagGapCheckM * kCm)
					Bad.Add(FString::Printf(TEXT("%s and %s are %.1f m apart - one objective built twice?"),
						*Caps[i]->Name, *Caps[j]->Name, d / kCm));
			}

		// 5. Nothing gameplay belongs on the world origin: that is a transform
		//    that was never set, and it is how a bomb ends up under the map.
		for (const FPlacedRec& R : B.Placed)
		{
			if (R.Type == TEXT("PolygonVolume") || R.Type == TEXT("Node3D") || !R.A) continue;
			if (R.A->GetActorLocation().IsNearlyZero(1.0))
				Bad.Add(FString::Printf(TEXT("%s sits at 0,0,0 - transform never set?"), *R.Name));
		}

		// 6. The Breakthrough HQ chain: TEAM_2_HQ<n> and TEAM_1_HQ<n+2> are the
		//    same deploy area, so they must stand on the same spot. Obliteration
		//    names mean something else entirely and are kilometres apart by
		//    design, so this is Breakthrough only.
		if (Key.Contains(TEXT("breakthrough")))
		{
			TMap<FString, FVector> Hq;
			for (const FPlacedRec& R : B.Placed)
				if (R.Type == TEXT("HQ_PlayerSpawner") && R.A) Hq.Add(R.Name, R.A->GetActorLocation());
			for (const TPair<FString, FVector>& kv : Hq)
			{
				if (!kv.Key.StartsWith(TEXT("TEAM_2_HQ")) || kv.Key.Len() <= 9) continue;
				const int32 n = FCString::Atoi(*kv.Key.Mid(9));
				const FString Mate = FString::Printf(TEXT("TEAM_1_HQ%d"), n + 2);
				if (const FVector* P = Hq.Find(Mate))
				{
					const double Gap = FVector::Dist(kv.Value, *P) / kCm;
					if (Gap > 1.0)
						Bad.Add(FString::Printf(TEXT("%s and %s should share a deploy area but stand %.0f m apart"),
							*kv.Key, *Mate, Gap));
				}
			}
		}
		return Bad;
	}

	// How much of the layer the build could actually place. Roughly half the
	// gems in a mode ship with no type in the game data (25-61% across four
	// maps, and 70-100% on Rush); those are not placed by anything, so a
	// creator has to be told the map is partly recovered, not complete.
	FString CoverageLine(const FMode& M)
	{
		if (M.Gems <= 0) return FString();
		return FString::Printf(
			TEXT("coverage: %d gem object(s) in this layer, %d of them (%d%%) carry NO type in the game data and are not placed - expect to add objects by hand"),
			M.Gems, M.GemsUnlinked, FMath::RoundToInt(100.0 * (double)M.GemsUnlinked / (double)M.Gems));
	}

	// ========================================================================
	// CONQUEST
	//
	//   Play Area/CombatArea      Infantry Area (the smallest zone holding
	//                             every flag) and Air Combat (the next one up)
	//   Play Area/Sector          ObjId 100, every capture point wired into it
	//   TEAM_1_HQ / TEAM_2_HQ     the two flagless zones with deploy spawns in
	//                             them, north = team 1; HQArea a solid box,
	//                             deploy spawns parented and wired
	//   Objectives/CapturePoint<L>  Area-<L> plus Spawns<L>/Team1|Team2
	//   Vehicles/Team1|Team2      HQ pads, boats where the ground is under
	//                             water, the helipad, the jet air-spawn
	//   Vehicles/<L>-Vehicle      the flag pads (objective rewards: not armed)
	//   AA-Defences, Resupply, AI_Spawners, EndGameCamera
	// ========================================================================
	void BuildConquest(FBuild& B, const FMode& M)
	{
		FSorted S;
		Sort(M, S);
		if (S.Flags.Num() == 0) { B.Note(TEXT("no flags in the mined layer")); return; }

		// which zone is what: one holding every flag is a combat area, one with
		// no flag but deploy spawns inside it is an HQ
		TArray<const FObject*> Loose;
		TArray<TArray<const FObject*>> FlagSpawns;
		FlagSpawns.SetNum(S.Flags.Num());
		for (const FObject* Sp : S.Spawns)
		{
			if (Sp->FlagIndex >= 0 && Sp->FlagIndex < S.Flags.Num()) FlagSpawns[Sp->FlagIndex].Add(Sp);
			else Loose.Add(Sp);
		}

		TArray<FPoly*> Combat, HqZones;
		TMap<const FPoly*, int32> LooseIn;
		for (FPoly& Z : S.Zones)
		{
			int32 nf = 0, ns = 0;
			for (const FPoly& F : S.Flags) if (InsidePolygonXY(Z.Pts, F.C)) nf++;
			for (const FObject* Sp : Loose) if (InsidePolygonXY(Z.Pts, Sp->Xf.GetLocation())) ns++;
			if (nf == S.Flags.Num()) Combat.Add(&Z);
			else if (nf == 0 && ns > 0) { LooseIn.Add(&Z, ns); HqZones.Add(&Z); }
		}
		HqZones.Sort([&LooseIn](const FPoly& a, const FPoly& b) { return LooseIn.FindRef(&a) > LooseIn.FindRef(&b); });
		if (HqZones.Num() > 2) HqZones.SetNum(2);
		// team 1 is the NORTHERN HQ zone (higher game z, which is Unreal Y)
		HqZones.Sort([](const FPoly& a, const FPoly& b) { return a.C.Y > b.C.Y; });
		Combat.Sort([](const FPoly& a, const FPoly& b) { return a.Area < b.Area; });

		// ---- Play Area ----
		AActor* Ca = nullptr;
		if (Combat.Num() == 0 && S.Combat.Num() == 0)
			B.Note(TEXT("no zone encloses every flag - draw the Combat Area by hand"));
		else
		{
			const FPoly* Inf = Combat.Num() ? Combat[0] : &S.Combat[0];
			Ca = B.PlaceIn(TEXT("CombatArea"), FTransform(FVector(Inf->C.X, Inf->C.Y, S.CeilingCm)), TEXT("CombatArea"), TEXT("Play Area"));
			if (Ca)
			{
				AActor* Ip = B.Volume(Inf->Pts, FVector(Inf->C.X, Inf->C.Y, S.CeilingCm), 0.0, TEXT("Infantry Area"), Ca);
				if (Ip) { if (!B.Link(Ca, TEXT("CombatVolume"), Ip)) B.MarkUnwired(Ca); }
				else B.MarkUnwired(Ca);
				if (Inf->Area > kAreaLimitM2)
					B.Note(FString::Printf(TEXT("the infantry area is %d m2, over the editor's %d limit - shrink it"),
						(int32)Inf->Area, (int32)kAreaLimitM2));
				if (Combat.Num() > 1)
				{
					const FPoly* Air = Combat[1];
					AActor* Ap = B.Volume(Air->Pts, FVector(Air->C.X, Air->C.Y, S.CeilingCm + 1500.0 * kCm), 0.0,
						TEXT("Air Combat"), Ca);
					if (Ap) B.Link(Ca, TEXT("SurroundingVolume"), Ap);
				}
			}
		}
		AActor* Sector = B.PlaceIn(TEXT("Sector"), FTransform(B.Centre), TEXT("Sector"), TEXT("Play Area"));
		if (Sector) B.PropInt(Sector, TEXT("ObjId"), 100);

		// ---- HQs ----
		TArray<AActor*> HqNodes;
		TArray<FVector> HqPos;
		if (HqZones.Num() < 2)
			B.Note(FString::Printf(TEXT("only %d HQ zone(s) found - add the missing TEAM HQ by hand"), HqZones.Num()));
		for (int32 t = 0; t < FMath::Min(2, HqZones.Num()); t++)
		{
			const FPoly& Z = *HqZones[t];
			const int32 Team = t + 1;
			TArray<const FObject*> Inside;
			for (const FObject* Sp : Loose) if (InsidePolygonXY(Z.Pts, Sp->Xf.GetLocation())) Inside.Add(Sp);
			// on the terrain and on land: the reader's land point is the nearest
			// spot to the centroid inside the polygon and clear of the sea. An
			// HQ dropped on a centroid over the water had players parachuting in.
			FVector Pos = Z.C;
			if (Z.O && Z.O->bHasLand) Pos = Z.O->Land;
			else if (Inside.Num())
			{
				double Z2 = 0.0;
				for (const FObject* Sp : Inside) Z2 += Sp->Xf.GetLocation().Z;
				Pos.Z = Z2 / (double)Inside.Num();
			}
			AActor* Hq = B.Place(TEXT("HQ_PlayerSpawner"), FTransform(Pos), FString::Printf(TEXT("TEAM_%d_HQ"), Team), nullptr);
			if (!Hq) continue;
			if (Team == 2) B.PropInt(Hq, TEXT("Team"), 2);
			B.PropInt(Hq, TEXT("ObjId"), Team);
			B.PropBool(Hq, TEXT("VehicleSpawnersEnabled"), true);
			// A SOLID BOX for the HQ area: origin 100 m under the HQ node and
			// 700 m tall, so the beach, the water and the helicopter are all
			// inside it. Height 0 here drew a paper-thin sheet 500 m up.
			AActor* Hp = B.Volume(Z.Pts, Pos - FVector(0, 0, 100.0 * kCm), kHqBoxHeightM,
				FString::Printf(TEXT("HQ_Team%d"), Team), Hq);
			if (Hp) { if (!B.Link(Hq, TEXT("HQArea"), Hp)) B.MarkUnwired(Hq); }
			else B.MarkUnwired(Hq);
			TArray<AActor*> Sps;
			for (int32 k = 0; k < Inside.Num(); k++)
			{
				AActor* Sp = B.Place(TEXT("SpawnPoint"), Inside[k]->Xf,
					FString::Printf(TEXT("TEAM_%d_HQ_Spawn%d"), Team, k + 1), Hq);
				if (Sp) Sps.Add(Sp);
			}
			B.LinkList(Hq, TEXT("InfantrySpawns"), Sps);
			for (const FObject* Sp : Inside) Loose.Remove(Sp);
			HqNodes.Add(Hq);
			HqPos.Add(Pos);
		}

		auto TeamOf = [&HqZones, &HqPos](const FVector& P, bool bStrict) -> int32
		{
			for (int32 t = 0; t < FMath::Min(2, HqZones.Num()); t++)
				if (InsidePolygonXY(HqZones[t]->Pts, P)) return t + 1;
			if (bStrict || HqPos.Num() < 2) return 0;
			return Dist2D(P, HqPos[0]) <= Dist2D(P, HqPos[1]) ? 1 : 2;
		};

		// ---- Objectives ----
		FVector2D Axis = FVector2D::ZeroVector;
		if (HqPos.Num() == 2) Axis = FVector2D(HqPos[1].X - HqPos[0].X, HqPos[1].Y - HqPos[0].Y).GetSafeNormal();
		// whatever deploy spawn is left over serves the flag it is nearest
		for (const FObject* Sp : Loose)
		{
			int32 Best = INDEX_NONE; double Bd = TNumericLimits<double>::Max();
			for (int32 f = 0; f < S.Flags.Num(); f++)
			{
				const double d = Dist2D(Sp->Xf.GetLocation(), S.Flags[f].C);
				if (d < Bd) { Bd = d; Best = f; }
			}
			if (Best != INDEX_NONE) FlagSpawns[Best].Add(Sp);
		}
		TArray<AActor*> CapNodes;
		TArray<AActor*> FlagActor;
		FlagActor.Init(nullptr, S.Flags.Num());
		for (int32 i = 0; i < S.Flags.Num(); i++)
		{
			const FPoly& F = S.Flags[i];
			const FString L = Letter(i);
			AActor* Cp = B.PlaceIn(TEXT("CapturePoint"), FTransform(F.C), FString::Printf(TEXT("CapturePoint%s"), *L), TEXT("Objectives"));
			if (!Cp) continue;
			FlagActor[i] = Cp;
			B.PropInt(Cp, TEXT("ObjId"), 200 + i);
			B.PropBool(Cp, TEXT("OutlineAbove16Points"), true);
			B.PropInt(Cp, TEXT("Spawning_ObjectiveMaxDistance"), 400);
			// the area and the spawns are the flag's OWN children
			AActor* Fp = B.Volume(F.Pts, F.C, 30.0, FString::Printf(TEXT("Area-%s"), *L), Cp);
			if (Fp) { if (!B.Link(Cp, TEXT("CaptureArea"), Fp)) B.MarkUnwired(Cp); }
			else B.MarkUnwired(Cp);
			const FString SpawnsKey = FString::Printf(TEXT("Objectives/Spawns%s"), *L);
			AActor* SpawnsNode = FlagSpawns[i].Num()
				? B.GroupUnder(SpawnsKey, Cp, FString::Printf(TEXT("Spawns%s"), *L)) : nullptr;
			TArray<AActor*> T1, T2;
			int32 k = 1;
			for (const FObject* Sp : FlagSpawns[i])
			{
				int32 Side = Sp->Team;
				if (Side != 1 && Side != 2)
				{
					const FVector2D D(Sp->Xf.GetLocation().X - F.C.X, Sp->Xf.GetLocation().Y - F.C.Y);
					Side = Axis.IsNearlyZero() ? (k % 2 ? 1 : 2) : (FVector2D::DotProduct(D, Axis) > 0.0 ? 2 : 1);
				}
				// Labels are world-unique, so a bare "Team1" under every flag would be
				// suffixed to Team3, Team5... by the tool; carry the flag letter.
				AActor* TeamNode = B.GroupUnder(FString::Printf(TEXT("%s/Team%d"), *SpawnsKey, Side), SpawnsNode,
					FString::Printf(TEXT("Spawns%s_Team%d"), *L, Side));
				AActor* N = B.Place(TEXT("SpawnPoint"), Sp->Xf, FString::Printf(TEXT("SpawnPoint_%s_%d"), *L, k++), TeamNode);
				if (!N) continue;
				(Side == 1 ? T1 : T2).Add(N);
			}
			B.LinkList(Cp, TEXT("InfantrySpawnPoints_Team1"), T1);
			B.LinkList(Cp, TEXT("InfantrySpawnPoints_Team2"), T2);
			if (FlagSpawns[i].Num() == 0)
				B.Note(FString::Printf(TEXT("CapturePoint%s: no spawns near it - add by hand"), *L));
			CapNodes.Add(Cp);
		}
		if (Sector) B.LinkList(Sector, TEXT("CapturePoints"), CapNodes);

		// ---- Vehicles ----
		FVehicleSort V;
		SortVehicles(S, [&TeamOf](const FVector& P) { return TeamOf(P, true); }, S.Flags, V);
		if (V.FromResupplyVehicle || V.FromResupplyGun)
			B.Note(FString::Printf(
				TEXT("the %d gem_vehicleresupplystation gems sorted by position: %d at an HQ became team vehicles, %d beside an objective became emplacements, %d left as resupply stations (the link word calls all of them resupply and is wrong)"),
				S.Resupplies.Num(), V.FromResupplyVehicle, V.FromResupplyGun, V.Stations.Num()));
		if (V.GunInBase)
			B.Note(FString::Printf(TEXT("%d emplacement gem(s) stand inside a base and were built as VEHICLES, not guns"), V.GunInBase));

		// the shape-bound slots: inside a base they are that team's pads, far
		// above the TERRAIN they are the jet air-spawn, otherwise a flag pad
		TArray<const FObject*> Air;
		for (const FObject* Sl : S.Slots)
		{
			const FVector P = Sl->Xf.GetLocation();
			if (TeamOf(P, true) > 0) { V.Pads.Add(Sl); continue; }
			if (Sl->bHasGround && P.Z - Sl->GroundZ > kAirSlotM * kCm) Air.Add(Sl);
			else V.Pads.Add(Sl);
		}

		int32 TCount[3] = { 0, 0, 0 };
		TMap<int32, int32> FlagPad;
		for (const FObject* Vp : V.Pads)
		{
			const FVector P = Vp->Xf.GetLocation();
			const int32 Team = TeamOf(P, true);
			AActor* N = nullptr;
			if (Team > 0)
			{
				TCount[Team]++;
				N = B.PlaceIn(TEXT("VehicleSpawner"), Vp->Xf,
					FString::Printf(TEXT("%s%d"), Vp->bWater ? TEXT("Boat") : TEXT("VehicleSpawner"), TCount[Team]),
					FString::Printf(TEXT("Vehicles/Team%d"), Team));
				if (!N) continue;
				B.PropInt(N, TEXT("ObjId"), 210 + Team * 100 + TCount[Team]);
				// ConquestV16 discovers HQ spawners from the vehicles they
				// spawn, so those autospawn; a flag pad is an objective reward
				// the mode script spawns on capture and stays off.
				B.PropBool(N, TEXT("P_AutoSpawnEnabled"), true);
			}
			else
			{
				int32 Best = INDEX_NONE; double Bd = TNumericLimits<double>::Max();
				for (int32 f = 0; f < S.Flags.Num(); f++)
				{
					const double d = Dist2D(P, S.Flags[f].C);
					if (d < Bd) { Bd = d; Best = f; }
				}
				const int32 nth = ++FlagPad.FindOrAdd(Best);
				const FString L = Best == INDEX_NONE ? TEXT("Field") : Letter(Best);
				// A FLAG PAD BELONGS TO ITS FLAG. It is the objective reward the
				// mode script spawns on capture, so it hangs under the capture
				// point rather than in a flat pile under Vehicles.
				AActor* Host = FlagActor.IsValidIndex(Best) && FlagActor[Best] ? FlagActor[Best] : B.Group(TEXT("Vehicles"));
				N = B.Place(TEXT("VehicleSpawner"), Vp->Xf,
					FString::Printf(TEXT("%s-%s%s"), *L, Vp->bWater ? TEXT("Boat") : TEXT("Vehicle"),
						nth > 1 ? *FString::FromInt(nth) : TEXT("")),
					Host);
				if (!N) continue;
				if (Best != INDEX_NONE) B.PropInt(N, TEXT("ObjId"), 600 + 10 * Best + nth - 1);
				B.PropBool(N, TEXT("P_AutoSpawnEnabled"), false);
			}
			// THE GROUND DECIDES THE CLASS: boats do not go on land and tanks do
			// not spawn in the sea. The gem's number is not a vehicle type (the
			// same value sits on a gun and a tank pad), so only the water flag
			// is acted on and everything else is left for the creator to set.
			if (Vp->bWater) B.PropInt(N, TEXT("VehicleType"), 21);   // RHIB
			B.PropInt(N, TEXT("P_DefaultRespawnTime"), 45);
		}
		for (const FObject* A : Air)
		{
			const int32 Team = TeamOf(A->Xf.GetLocation(), false);
			if (Team == 0) continue;
			TCount[Team]++;
			AActor* N = B.Place(TEXT("VehicleSpawner"), A->Xf, FString::Printf(TEXT("AirSlot%d"), TCount[Team]),
				FString::Printf(TEXT("Vehicles/Team%d"), Team));
			if (!N) continue;
			B.PropInt(N, TEXT("ObjId"), 210 + Team * 100 + TCount[Team]);
			B.PropInt(N, TEXT("P_DefaultRespawnTime"), 45);
			B.PropBool(N, TEXT("P_AutoSpawnEnabled"), true);
			B.PropInt(N, TEXT("VehicleType"), Team == 1 ? 15 : 16);   // F22 / SU57
		}
		if (Air.Num())
			B.Note(FString::Printf(TEXT("%d slot(s) stand over %d m above the terrain and were built as the jet air-spawn (F22 / SU57) - retype if this map flies something else"),
				Air.Num(), (int32)kAirSlotM));

		if (V.Guns.Num())
		{
			int32 SCount[3] = { 0, 0, 0 };
			for (const FObject* G : V.Guns)
			{
				const int32 t = TeamOf(G->Xf.GetLocation(), false);
				SCount[t]++;
				AActor* N = B.Place(TEXT("StationaryEmplacementSpawner"), G->Xf,
					FString::Printf(TEXT("StationaryEmplacement_T%d_%d"), t, SCount[t]), TEXT("AA-Defences"));
				if (N) B.PropInt(N, TEXT("ObjId"), 280 + t * 5 + SCount[t]);
			}
			B.Note(FString::Printf(TEXT("%d stationary emplacement(s) under AA-Defences (team 1: %d, team 2: %d) - set each TYPE by hand, the data has none"),
				V.Guns.Num(), SCount[1], SCount[2]));
		}
		if (V.Stations.Num())
		{
			for (int32 i = 0; i < V.Stations.Num(); i++)
				B.Place(TEXT("VehicleResupplyStation"), V.Stations[i]->Xf,
					i == 0 ? FString(TEXT("VehicleResupplyStation")) : FString::Printf(TEXT("VehicleResupplyStation%d"), i + 1),
					TEXT("Resupply"));
			B.Note(FString::Printf(TEXT("%d station(s) under Resupply from the gem_vehicleresupplystation link - THIS GROUP IS A MIX: on Golmud it holds vehicles and emplacement spawners as well as real resupply points. The positions are right, the KIND is a guess - retype what is wrong"),
				V.Stations.Num()));
		}

		// ---- AI spawners and the end camera ----
		for (int32 t = 0; t < HqNodes.Num(); t++)
		{
			AActor* Sp = B.Place(TEXT("AI_Spawner"), FTransform(HqPos[t]), FString::Printf(TEXT("AI_Spawner_Team%d"), t + 1), TEXT("AI_Spawners"));
			if (!Sp) continue;
			B.PropInt(Sp, TEXT("ObjId"), 901 + t);
			TArray<AActor*> Alt;
			TArray<AActor*> Kids;
			HqNodes[t]->GetAttachedActors(Kids, true, true);
			for (AActor* K : Kids) if (K->GetActorLabel().Contains(TEXT("Spawn"))) Alt.Add(K);
			B.LinkList(Sp, TEXT("AlternateSpawns"), Alt);
		}
		FVector All = FVector::ZeroVector;
		for (const FPoly& F : S.Flags) All += F.C;
		All /= (double)FMath::Max(1, S.Flags.Num());
		if (AActor* Cam = B.Place(TEXT("FixedCamera"), FTransform(FRotator(-90, 0, 0).Quaternion(), All + FVector(0, 0, 350.0 * kCm)),
			TEXT("EndGameCamera"), FString()))
			B.PropInt(Cam, TEXT("ObjId"), 950);

		// the boxes the layer ships, kept rather than dropped
		for (int32 i = 0; i < S.Boxes.Num(); i++)
		{
			AActor* Bx = B.Place(TEXT("OBBVolume"), S.Boxes[i]->Xf, FString::Printf(TEXT("Box%d"), i + 1), TEXT("ExtraZones"));
			if (Bx) B.Prop(Bx, TEXT("size"), FString::Printf(TEXT("%g,%g,%g"),
				S.Boxes[i]->SizeGodot.X, S.Boxes[i]->SizeGodot.Y, S.Boxes[i]->SizeGodot.Z));
		}
		// and every zone the derivation did not use
		int32 Zn = 0;
		for (const FPoly& Z : S.Zones)
		{
			if (Combat.Contains(&Z) || HqZones.Contains(&Z)) continue;
			B.Volume(Z.Pts, Z.C, Z.O ? Z.O->HeightM : 0.f, FString::Printf(TEXT("Zone%d"), ++Zn), TEXT("ExtraZones"));
		}
	}

	// ========================================================================
	// BREAKTHROUGH
	//
	//   Sectors/Sector<n>         its polygon (SectorArea), an AreaTrigger,
	//                             CapturePoint<L> with Spawns<L>/Team1|Team2,
	//                             and the staggered HQ pair TEAM_1_HQ<n> /
	//                             TEAM_2_HQ<n> whose HQArea is the NEIGHBOURING
	//                             sector's polygon
	//   CombatArea                the mode's own boundary zone where it has
	//                             one, else a box around the play space
	//   AISpawners, ExtraZones, DeploySpawns
	//
	// Derived, never tabled: each phase authors a zone PAIR over the same
	// flags and the broader one is the sector; a zone whose flag set strictly
	// contains another's is the container those sectors sit in, not a sector
	// (the mined zones overlap, and building both put one objective in two
	// sectors); the attack axis is the farthest-apart flag pair.
	// ========================================================================
	void BuildBreakthrough(FBuild& B, const FMode& M)
	{
		FSorted S;
		Sort(M, S);
		if (S.Flags.Num() == 0) { B.Note(TEXT("no capture points mined - nothing to structure")); return; }

		// A small flagless zone nested in a zone at least 3x its size is an
		// OBJECTIVE the layer did not classify (the carrier assault authors its
		// final phase this way), promoted here so the phase logic finds it.
		TArray<FPoly> Zones;
		int32 Promoted = 0;
		for (FPoly& Z : S.Zones)
		{
			bool bHost = false;
			if (Z.Area <= 8000.0)
				for (const FPoly& O : S.Zones)
					if (&O != &Z && O.Area >= Z.Area * 3.0 && InsidePolygonXY(O.Pts, Z.C)) { bHost = true; break; }
			if (bHost) { FPoly F = Z; Promoted++; S.Flags.Add(MoveTemp(F)); }
			else Zones.Add(Z);
		}
		S.Zones = MoveTemp(Zones);
		if (Promoted)
			B.Note(FString::Printf(TEXT("%d small zone(s) nested in a much larger one were promoted to objectives - verify them in game (a final phase can be MCOM based)"), Promoted));

		// the attack axis: the farthest-apart flag pair
		FVector A = S.Flags[0].C, Bv = S.Flags.Last().C;
		double Best = -1.0;
		for (int32 i = 0; i < S.Flags.Num(); i++)
			for (int32 j = i + 1; j < S.Flags.Num(); j++)
			{
				const double d = FVector2D(S.Flags[i].C.X - S.Flags[j].C.X, S.Flags[i].C.Y - S.Flags[j].C.Y).SizeSquared();
				if (d > Best) { Best = d; A = S.Flags[i].C; Bv = S.Flags[j].C; }
			}
		const FVector2D Axis = FVector2D(Bv.X - A.X, Bv.Y - A.Y).GetSafeNormal();
		auto Along = [&Axis](const FVector& P) { return P.X * Axis.X + P.Y * Axis.Y; };

		// phase pairs: a flag set -> the broadest zone holding it
		for (FPoly& Z : S.Zones)
		{
			Z.Flags.Reset();
			for (int32 f = 0; f < S.Flags.Num(); f++) if (InsidePolygonXY(Z.Pts, S.Flags[f].C)) Z.Flags.Add(f);
		}
		TMap<FString, int32> BySet;
		auto KeyOfSet = [](const TArray<int32>& F)
		{
			TArray<int32> C = F; C.Sort();
			FString K;
			for (int32 i : C) K += FString::Printf(TEXT("%d|"), i);
			return K;
		};
		for (int32 z = 0; z < S.Zones.Num(); z++)
		{
			if (S.Zones[z].Flags.Num() == 0) continue;
			const FString K = KeyOfSet(S.Zones[z].Flags);
			int32* Have = BySet.Find(K);
			if (!Have || S.Zones[*Have].Area < S.Zones[z].Area) BySet.Add(K, z);
		}
		// CONTAINERS ARE NOT SECTORS. A zone whose flag set strictly contains
		// another zone's is the combat area those sectors sit in; keeping it
		// built the same objective in two sectors.
		TArray<FString> Containers;
		for (const TPair<FString, int32>& kv : BySet)
			for (const TPair<FString, int32>& kv2 : BySet)
			{
				if (kv2.Key == kv.Key) continue;
				const TArray<int32>& Mine = S.Zones[kv.Value].Flags;
				const TArray<int32>& Other = S.Zones[kv2.Value].Flags;
				if (Other.Num() >= Mine.Num()) continue;
				bool bSubset = true;
				for (int32 f : Other) if (!Mine.Contains(f)) { bSubset = false; break; }
				if (bSubset) { Containers.AddUnique(kv.Key); break; }
			}
		for (const FString& K : Containers) BySet.Remove(K);

		TArray<int32> Playable;
		for (const TPair<FString, int32>& kv : BySet) Playable.Add(kv.Value);
		if (Playable.Num() == 0) { B.Note(TEXT("no mined zone contains a flag - cannot structure")); return; }
		Playable.Sort([&S, &Along](int32 a, int32 b) { return Along(S.Zones[a].C) < Along(S.Zones[b].C); });
		for (int32 z : Playable)
			S.Zones[z].Flags.Sort([&S, &Along](int32 a, int32 b) { return Along(S.Flags[a].C) < Along(S.Flags[b].C); });

		// flagless zones: the dead ends, the deploy areas between phases, extras
		TArray<int32> Before, Between, After;
		const double First = Along(S.Zones[Playable[0]].C), Last = Along(S.Zones[Playable.Last()].C);
		for (int32 z = 0; z < S.Zones.Num(); z++)
		{
			if (S.Zones[z].Flags.Num() > 0 || Playable.Contains(z)) continue;
			const double At = Along(S.Zones[z].C);
			(At < First ? Before : At > Last ? After : Between).Add(z);
		}
		Before.Sort([&S, &Along](int32 a, int32 b) { return Along(S.Zones[a].C) < Along(S.Zones[b].C); });
		After.Sort([&S, &Along](int32 a, int32 b) { return Along(S.Zones[a].C) < Along(S.Zones[b].C); });

		auto RectAround = [](const FVector& C)
		{
			const double H = 150.0 * kCm;
			TArray<FVector> P;
			P.Add(C + FVector(-H, -H, 0)); P.Add(C + FVector(-H, H, 0));
			P.Add(C + FVector(H, H, 0));   P.Add(C + FVector(H, -H, 0));
			return P;
		};

		TArray<FPoly> Sectors;
		{
			FPoly S0;
			if (Before.Num()) { S0 = S.Zones[Before.Last()]; Before.Pop(); }
			else
			{
				S0.C = S.Zones[Playable[0]].C - FVector(Axis.X, Axis.Y, 0) * 180.0 * kCm;
				S0.Pts = RectAround(S0.C); S0.bSynth = true;
			}
			Sectors.Add(MoveTemp(S0));
		}
		for (int32 z : Playable) Sectors.Add(S.Zones[z]);
		{
			FPoly SN;
			if (After.Num()) { SN = S.Zones[After[0]]; After.RemoveAt(0); }
			else
			{
				SN.C = S.Zones[Playable.Last()].C + FVector(Axis.X, Axis.Y, 0) * 180.0 * kCm;
				SN.Pts = RectAround(SN.C); SN.bSynth = true;
			}
			Sectors.Add(MoveTemp(SN));
		}
		const int32 LastIdx = Sectors.Num() - 1;

		// flag spawns vs the loose deploy pool
		TArray<const FObject*> Loose;
		TMap<int32, TArray<const FObject*>> FlagSpawns;
		for (const FObject* Sp : S.Spawns)
		{
			if (Sp->FlagIndex >= 0 && Sp->FlagIndex < S.Flags.Num()) FlagSpawns.FindOrAdd(Sp->FlagIndex).Add(Sp);
			else Loose.Add(Sp);
		}

		// ---- the tree ----
		TArray<AActor*> SectorPoly;
		SectorPoly.Init(nullptr, Sectors.Num());
		for (int32 n = 0; n < Sectors.Num(); n++)
		{
			const FString Path = FString::Printf(TEXT("Sectors/Sector%d"), n);
			AActor* Sn = B.Place(TEXT("Sector"), FTransform(Sectors[n].C), FString::Printf(TEXT("Sector%d"), n), TEXT("Sectors"));
			if (!Sn) { B.Note(TEXT("the Sector placeable is missing - is this the Portal library?")); return; }
			B.PropInt(Sn, TEXT("ObjId"), 100 + n);
			AActor* Poly = B.Volume(Sectors[n].Pts, Sectors[n].C, 100.0, FString::Printf(TEXT("PolygonVolume%d"), n), Sn);
			if (Poly) { if (!B.Link(Sn, TEXT("SectorArea"), Poly)) B.MarkUnwired(Sn); }
			else B.MarkUnwired(Sn);
			SectorPoly[n] = Poly;
			AActor* Trig = B.Place(TEXT("AreaTrigger"), FTransform(Sectors[n].C + FVector(0, 0, 20.0 * kCm)),
				FString::Printf(TEXT("AreaTrigger%d"), n), Sn);
			if (Trig)
			{
				B.PropInt(Trig, TEXT("ObjId"), 600 + n);
				if (!B.Link(Trig, TEXT("Area"), Poly)) B.MarkUnwired(Trig);
			}
			B.Groups.Add(Path, Sn);
			if (Sectors[n].bSynth) B.Note(FString::Printf(TEXT("Sector%d area is a PLACEHOLDER - draw the real one"), n));
		}

		for (int32 n = 1; n < LastIdx; n++)
		{
			const FString Path = FString::Printf(TEXT("Sectors/Sector%d"), n);
			AActor* Sn = B.Groups.FindRef(Path);
			TArray<AActor*> Caps;
			for (int32 i = 0; i < Sectors[n].Flags.Num(); i++)
			{
				const int32 fi = Sectors[n].Flags[i];
				const FPoly& F = S.Flags[fi];
				const FString L = Letter(i);
				AActor* Cp = B.Place(TEXT("CapturePoint"), FTransform(F.C),
					FString::Printf(TEXT("Sector%d_CapturePoint%s"), n, *L), Sn);
				if (!Cp) continue;
				B.PropInt(Cp, TEXT("InitialOwner"), 2);
				B.PropInt(Cp, TEXT("ObjId"), 1000 + n * 100 + i);
				// 30 m tall: at 15 an objective on a slope could not be captured
				AActor* Fp = B.Volume(F.Pts, F.C, 30.0, FString::Printf(TEXT("Sector%d_Area-%s"), n, *L), Cp);
				if (Fp) { if (!B.Link(Cp, TEXT("CaptureArea"), Fp)) B.MarkUnwired(Cp); }
				else B.MarkUnwired(Cp);
				const FString SpawnsKey = FString::Printf(TEXT("%s/Spawns%s"), *Path, *L);
				const TArray<const FObject*>* List = FlagSpawns.Find(fi);
				AActor* SpawnsNode = (List && List->Num())
					? B.GroupUnder(SpawnsKey, Cp, FString::Printf(TEXT("Spawns%s"), *L)) : nullptr;
				TArray<AActor*> T1, T2;
				int32 k = 1;
				if (List)
					for (const FObject* Sp : *List)
					{
						int32 Team = Sp->Team;
						if (Team != 1 && Team != 2) Team = Along(Sp->Xf.GetLocation()) < Along(F.C) ? 1 : 2;
						AActor* TeamNode = B.GroupUnder(FString::Printf(TEXT("%s/Team%d"), *SpawnsKey, Team), SpawnsNode,
							FString::Printf(TEXT("Sector%d_Spawns%s_Team%d"), n, *L, Team));
						AActor* N = B.Place(TEXT("SpawnPoint"), Sp->Xf,
							FString::Printf(TEXT("Sector%d_Spawn%s_%d"), n, *L, k++), TeamNode);
						if (N) (Team == 1 ? T1 : T2).Add(N);
					}
				B.LinkList(Cp, TEXT("InfantrySpawnPoints_Team1"), T1);
				B.LinkList(Cp, TEXT("InfantrySpawnPoints_Team2"), T2);
				if (T1.Num() == 0 || T2.Num() == 0)
					B.Note(FString::Printf(TEXT("Sector%d/CapturePoint%s: no mined spawns for team %d - add by hand"),
						n, *L, T1.Num() == 0 ? 1 : 2));
				Caps.Add(Cp);
			}
			B.LinkList(Sn, TEXT("CapturePoints"), Caps);

			// HQs: the attacker keyed to the previous sector, the defender to
			// the next, so sector n is always the fight and never holds an HQ.
			const int32 Sides[2][3] = { { 1, n - 1, 300 + n }, { 2, n + 1, 400 + n } };
			for (const int32* Side : Sides)
			{
				const int32 Team = Side[0], Ref = Side[1], ObjId = Side[2];
				if (!Sectors.IsValidIndex(Ref)) continue;
				FVector Cluster = Sectors[Ref].C;
				if (!DeployClusterIn(Sectors[Ref].Pts, Loose, S.Vehicles, Sectors[n].C, S.Flags, Cluster))
					B.Note(FString::Printf(TEXT("TEAM_%d_HQ%d: no deploy spawns inside Sector%d's polygon - placed at its centre, reposition"), Team, n, Ref));
				// the node stands on the centre of its area, on the terrain and
				// on land: taking the cluster's height put the HQ metres up in
				// the air and players parachuted in
				FVector Pos = Sectors[Ref].C;
				Pos.Z = Cluster.Z;
				if (Sectors[Ref].O && Sectors[Ref].O->bHasLand) Pos = Sectors[Ref].O->Land;
				else B.Note(FString::Printf(TEXT("TEAM_%d_HQ%d: no dry ground found inside Sector%d - placed on the polygon centre, check the height"), Team, n, Ref));
				AActor* Hq = B.Place(TEXT("HQ_PlayerSpawner"), FTransform(Pos),
					FString::Printf(TEXT("TEAM_%d_HQ%d"), Team, n), Sn);
				if (!Hq) continue;
				if (Team == 2) B.PropInt(Hq, TEXT("Team"), 2);
				B.PropInt(Hq, TEXT("ObjId"), ObjId);
				if (SectorPoly.IsValidIndex(Ref) && SectorPoly[Ref]) { if (!B.Link(Hq, TEXT("HQArea"), SectorPoly[Ref])) B.MarkUnwired(Hq); }
				else B.MarkUnwired(Hq);
			}
		}

		// COMBAT AREA. The layer usually has the real outline: a zone holding
		// every sector is the map boundary, not a sector. Largest is the air
		// boundary, the next one in the combat volume.
		{
			TArray<const FPoly*> Bounds;
			for (const FPoly& Z : S.Zones)
			{
				bool bHoldsAll = true;
				for (const FPoly& Sc : Sectors) if (!InsidePolygonXY(Z.Pts, Sc.C)) { bHoldsAll = false; break; }
				if (bHoldsAll) Bounds.Add(&Z);
			}
			Bounds.Sort([](const FPoly& a, const FPoly& b) { return a.Area > b.Area; });
			TArray<FVector> Outline;
			const FPoly* AirZone = nullptr;
			if (S.Combat.Num()) Outline = S.Combat[0].Pts;
			else if (Bounds.Num() >= 2) { AirZone = Bounds[0]; Outline = Bounds[1]->Pts; }
			else if (Bounds.Num() == 1)
			{
				if (Bounds[0]->Area <= kAreaLimitM2) Outline = Bounds[0]->Pts;
				else AirZone = Bounds[0];
			}
			if (Outline.Num() == 0)
			{
				FVector Lo(TNumericLimits<double>::Max()), Hi(-TNumericLimits<double>::Max());
				for (const FPoly& Sc : Sectors) for (const FVector& P : Sc.Pts) { Lo = Lo.ComponentMin(P); Hi = Hi.ComponentMax(P); }
				const double Margin = 300.0 * kCm;
				Outline.Add(FVector(Lo.X - Margin, Lo.Y - Margin, 0)); Outline.Add(FVector(Hi.X + Margin, Lo.Y - Margin, 0));
				Outline.Add(FVector(Hi.X + Margin, Hi.Y + Margin, 0)); Outline.Add(FVector(Lo.X - Margin, Hi.Y + Margin, 0));
				B.Note(TEXT("CombatArea is a box around the play space (this mode's layer has no combat outline) - redraw it if you want the coastline"));
			}
			else B.Note(TEXT("CombatArea outline came from the mode's own boundary zone"));
			const FVector Oc = Centroid(Outline);
			AActor* Ca = B.Place(TEXT("CombatArea"), FTransform(FVector(Oc.X, Oc.Y, S.CeilingCm)), TEXT("CombatArea"), TEXT("Play Area"));
			if (Ca)
			{
				AActor* Cv = B.Volume(Outline, FVector(Oc.X, Oc.Y, S.CeilingCm), 0.0, TEXT("CombatVolume"), Ca);
				if (Cv) { if (!B.Link(Ca, TEXT("CombatVolume"), Cv)) B.MarkUnwired(Ca); }
				else B.MarkUnwired(Ca);
				if (AirZone)
				{
					AActor* Av = B.Volume(AirZone->Pts, FVector(AirZone->C.X, AirZone->C.Y, S.CeilingCm + 1500.0 * kCm), 0.0,
						TEXT("AirBoundry"), Ca);
					if (Av) B.Link(Ca, TEXT("SurroundingVolume"), Av);
				}
			}
		}

		// AI spawners: the base pair, then one pair per playable sector
		for (int32 i = 0; i < 2; i++)
			if (AActor* Sp = B.Place(TEXT("AI_Spawner"), FTransform(Sectors[0].C + FVector(i ? -1.0 : 1.0, 0, 0)),
				i == 0 ? FString(TEXT("AI_Spawner")) : FString(TEXT("AI_Spawner2")), TEXT("AISpawners")))
				B.PropInt(Sp, TEXT("ObjId"), 901 + i);
		for (int32 n = 1; n < LastIdx; n++)
			for (int32 t = 1; t <= 2; t++)
			{
				FVector At = Sectors[n].C;
				for (const FPlacedRec& R : B.Placed)
					if (R.Name == FString::Printf(TEXT("TEAM_%d_HQ%d"), t, n) && R.A) At = R.A->GetActorLocation();
				if (AActor* Sp = B.Place(TEXT("AI_Spawner"), FTransform(At), FString::Printf(TEXT("AI_Spawner_S%d_T%d"), n, t), TEXT("AISpawners")))
					B.PropInt(Sp, TEXT("ObjId"), (t == 1 ? 910 : 920) + n);
			}

		// NOTHING IS DROPPED: what the derivation could not place is grouped
		// rather than thrown away, so the build stays what the game ships.
		int32 Zn = 0;
		for (int32 z : Before) B.Volume(S.Zones[z].Pts, S.Zones[z].C, 0.0, FString::Printf(TEXT("Zone%d"), ++Zn), TEXT("ExtraZones"));
		for (int32 z : Between) B.Volume(S.Zones[z].Pts, S.Zones[z].C, 0.0, FString::Printf(TEXT("Zone%d"), ++Zn), TEXT("ExtraZones"));
		for (int32 z : After) B.Volume(S.Zones[z].Pts, S.Zones[z].C, 0.0, FString::Printf(TEXT("Zone%d"), ++Zn), TEXT("ExtraZones"));
		int32 Dn = 0;
		for (const FObject* Sp : Loose)
			B.Place(TEXT("SpawnPoint"), Sp->Xf, FString::Printf(TEXT("DeploySpawn%d"), ++Dn), TEXT("DeploySpawns"));
		int32 Vn = 0;
		for (const FObject* V : S.Vehicles)
		{
			AActor* N = B.Place(TEXT("VehicleSpawner"), V->Xf, FString::Printf(TEXT("VehicleSpawner%d"), ++Vn), TEXT("Vehicles"));
			if (N) { B.PropInt(N, TEXT("P_DefaultRespawnTime"), 45); B.PropBool(N, TEXT("P_AutoSpawnEnabled"), true); }
		}
	}

	// ========================================================================
	// OBLITERATION
	//
	//   Play Area/CombatArea      the smallest zone holding every objective
	//                             (+ AirBoundry), Play Area/Sector ObjId 100
	//   TEAM_1_HQ / TEAM_2_HQ     ObjId 1 / 2, HQArea the base zone, deploy
	//                             spawns parented and wired
	//   Objectives/MCOM_<L>       31-33 (team 1) and 41-43 (team 2), with the
	//                             defuse bomb BombM<n> beside each
	//   Objectives/Bomb<n>        the neutral bombs, kept midfield
	//   Vehicles/Team1|Team2, AA-Defences, Resupply, AI_Spawners, EndGameCamera
	//
	// Team 1 is the base with the lower coordinate on the axis the bases are
	// separated along.
	// ========================================================================
	void BuildObliteration(FBuild& B, const FMode& M)
	{
		FSorted S;
		Sort(M, S);
		if (S.Mcoms.Num() == 0 && S.Unlinked.Num() == 0) { B.Note(TEXT("the mined layer carries no objectives")); return; }

		// BASES: the two smallest zones holding a cluster of deploy spawns.
		// "No objective inside" is not the test - a base zone can be 500x800 m
		// and reach far enough to swallow the nearest objective.
		TArray<FPoly*> Bases;
		for (FPoly& Z : S.Zones)
		{
			int32 ns = 0;
			for (const FObject* Sp : S.Spawns) if (InsidePolygonXY(Z.Pts, Sp->Xf.GetLocation())) ns++;
			if (ns >= kBaseSpawnsMin) Bases.Add(&Z);
		}
		Bases.Sort([](const FPoly& a, const FPoly& b) { return a.Area < b.Area; });
		if (Bases.Num() > 2) Bases.SetNum(2);
		if (Bases.Num() == 2)
		{
			const bool bByX = FMath::Abs(Bases[0]->C.X - Bases[1]->C.X) >= FMath::Abs(Bases[0]->C.Y - Bases[1]->C.Y);
			if ((bByX && Bases[0]->C.X > Bases[1]->C.X) || (!bByX && Bases[0]->C.Y > Bases[1]->C.Y)) Swap(Bases[0], Bases[1]);
		}
		else B.Note(FString::Printf(TEXT("only %d base zone(s) found - add the missing TEAM HQ by hand"), Bases.Num()));

		auto InBase = [&Bases](const FVector& P) -> int32
		{
			for (int32 i = 0; i < Bases.Num(); i++) if (InsidePolygonXY(Bases[i]->Pts, P)) return i + 1;
			return 0;
		};

		// THE OBJECTIVES. gem_objective_mcom is not the whole story: what marks
		// an objective is the ring of special combat areas around it, so an
		// objective is a gem outside both bases with a special area within 80 m,
		// taking the largest group that shares one parameter word. A tight knot
		// of gems is not an objective set, so a group whose extent is under a
		// quarter of the distance between the bases is thrown away.
		TArray<const FObject*> Mcoms = S.Mcoms;
		if (S.Areas.Num())
		{
			TMap<int32, TArray<const FObject*>> Cand;
			TArray<const FObject*> Pool = S.Unlinked;
			Pool.Append(S.Mcoms);
			for (const FObject* G : Pool)
			{
				const FVector P = G->Xf.GetLocation();
				if (InBase(P) != 0) continue;
				bool bNear = false;
				for (const FObject* Ar : S.Areas) if (Dist2D(P, Ar->Xf.GetLocation()) <= 80.0 * kCm) { bNear = true; break; }
				if (bNear) Cand.FindOrAdd(G->GemValue).Add(G);
			}
			TArray<const FObject*> BestSet;
			for (const TPair<int32, TArray<const FObject*>>& kv : Cand) if (kv.Value.Num() > BestSet.Num()) BestSet = kv.Value;
			double Extent = 0.0;
			for (int32 i = 0; i < BestSet.Num(); i++)
				for (int32 j = i + 1; j < BestSet.Num(); j++)
					Extent = FMath::Max(Extent, Dist2D(BestSet[i]->Xf.GetLocation(), BestSet[j]->Xf.GetLocation()));
			const double BaseSpan = Bases.Num() == 2 ? Dist2D(Bases[0]->C, Bases[1]->C) : 0.0;
			if (BestSet.Num() >= 4 && (BaseSpan <= 0.0 || Extent >= BaseSpan * 0.25))
			{
				Mcoms = BestSet;
				B.Note(FString::Printf(TEXT("%d objective(s) found by the special-combat-area rings around them"), BestSet.Num()));
			}
			else if (BestSet.Num() >= 4)
				B.Note(FString::Printf(TEXT("the ring test found %d gems only %d m apart on a %d m map - ignored as too tightly packed to be the objectives"),
					BestSet.Num(), (int32)(Extent / kCm), (int32)(BaseSpan / kCm)));
		}
		// an MCOM-linked gem inside a team's own base is one of that side's
		// vehicle pads, and several can sit within a few metres of one spot
		TArray<const FObject*> Uniq, ExtraPads;
		for (const FObject* Mc : Mcoms)
		{
			const FVector P = Mc->Xf.GetLocation();
			if (InBase(P) != 0) { ExtraPads.Add(Mc); continue; }
			bool bDup = false;
			for (const FObject* U : Uniq) if (Dist2D(P, U->Xf.GetLocation()) < 15.0 * kCm) { bDup = true; break; }
			if (!bDup) Uniq.Add(Mc);
		}
		if (Uniq.Num() != Mcoms.Num())
			B.Note(FString::Printf(TEXT("the layer names %d objective gems: %d stand inside a team base and were placed as vehicle pads, %d merged as duplicates, leaving %d objective(s) - VERIFY these against the game"),
				Mcoms.Num(), ExtraPads.Num(), Mcoms.Num() - ExtraPads.Num() - Uniq.Num(), Uniq.Num()));
		Mcoms = Uniq;
		if (Mcoms.Num() == 0) B.Note(TEXT("no objectives could be identified - place the MCOMs by hand"));

		// ---- Play Area ----
		TArray<const FPoly*> Holding;
		for (const FPoly& Z : S.Zones)
		{
			if (Bases.Contains(&Z)) continue;
			bool bAll = Mcoms.Num() > 0;
			for (const FObject* Mc : Mcoms) if (!InsidePolygonXY(Z.Pts, Mc->Xf.GetLocation())) { bAll = false; break; }
			if (bAll) Holding.Add(&Z);
		}
		Holding.Sort([](const FPoly& a, const FPoly& b) { return a.Area < b.Area; });
		AActor* Cv = nullptr;
		if (Holding.Num())
		{
			const FPoly* Inf = Holding[0];
			AActor* Ca = B.Place(TEXT("CombatArea"), FTransform(FVector(Inf->C.X, Inf->C.Y, S.CeilingCm)), TEXT("CombatArea"), TEXT("Play Area"));
			if (Ca)
			{
				Cv = B.Volume(Inf->Pts, FVector(Inf->C.X, Inf->C.Y, S.CeilingCm), 0.0, TEXT("CombatVolume"), Ca);
				if (Cv) { if (!B.Link(Ca, TEXT("CombatVolume"), Cv)) B.MarkUnwired(Ca); }
				else B.MarkUnwired(Ca);
				if (Holding.Num() > 1)
				{
					const FPoly* Air = Holding[1];
					AActor* Av = B.Volume(Air->Pts, FVector(Air->C.X, Air->C.Y, S.CeilingCm + 1500.0 * kCm), 0.0, TEXT("AirBoundry"), Ca);
					if (Av) B.Link(Ca, TEXT("SurroundingVolume"), Av);
				}
			}
		}
		else B.Note(TEXT("no zone holds every objective - draw the Combat Area by hand"));
		AActor* Sector = B.Place(TEXT("Sector"), FTransform(B.Centre), TEXT("Sector"), TEXT("Play Area"));
		if (Sector) B.PropInt(Sector, TEXT("ObjId"), 100);

		// ---- HQs ----
		TArray<AActor*> HqNodes;
		TArray<FVector> HqPos;
		for (int32 t = 0; t < Bases.Num(); t++)
		{
			const FPoly& Z = *Bases[t];
			const int32 Team = t + 1;
			TArray<const FObject*> Inside;
			for (const FObject* Sp : S.Spawns) if (InsidePolygonXY(Z.Pts, Sp->Xf.GetLocation())) Inside.Add(Sp);
			FVector Pos = Z.O && Z.O->bHasLand ? Z.O->Land : Z.C;
			AActor* Hq = B.Place(TEXT("HQ_PlayerSpawner"), FTransform(Pos), FString::Printf(TEXT("TEAM_%d_HQ"), Team), nullptr);
			if (!Hq) continue;
			if (Team == 2) B.PropInt(Hq, TEXT("Team"), 2);
			B.PropInt(Hq, TEXT("ObjId"), Team);
			B.PropBool(Hq, TEXT("VehicleSpawnersEnabled"), true);
			AActor* Hp = B.Volume(Z.Pts, Pos - FVector(0, 0, 100.0 * kCm), kHqBoxHeightM, FString::Printf(TEXT("HQ_Team%d"), Team), Hq);
			if (Hp) { if (!B.Link(Hq, TEXT("HQArea"), Hp)) B.MarkUnwired(Hq); }
			else B.MarkUnwired(Hq);
			TArray<AActor*> Sps;
			for (int32 k = 0; k < Inside.Num(); k++)
			{
				AActor* Sp = B.Place(TEXT("SpawnPoint"), Inside[k]->Xf, FString::Printf(TEXT("TEAM_%d_HQ_Spawn%d"), Team, k + 1), Hq);
				if (Sp) Sps.Add(Sp);
			}
			B.LinkList(Hq, TEXT("InfantrySpawns"), Sps);
			HqNodes.Add(Hq);
			HqPos.Add(Pos);
		}

		// ---- Objectives ----
		// each side defends its own half: the objective nearer base 1 is team
		// 1's, so the MCOM bands are 31+ and 41+
		TArray<AActor*> McomNodes;
		int32 Count[3] = { 0, 0, 0 };
		for (int32 i = 0; i < Mcoms.Num(); i++)
		{
			const FObject* Mc = Mcoms[i];
			int32 Team = 1;
			if (Bases.Num() == 2)
				Team = Dist2D(Mc->Xf.GetLocation(), Bases[0]->C) <= Dist2D(Mc->Xf.GetLocation(), Bases[1]->C) ? 1 : 2;
			const int32 nth = ++Count[Team];
			const FString L = Letter(i);
			AActor* Mn = B.Place(TEXT("MCOM"), Mc->Xf, FString::Printf(TEXT("MCOM_%s"), *L), TEXT("Objectives"));
			if (!Mn) continue;
			B.PropInt(Mn, TEXT("ObjId"), (Team == 1 ? 30 : 40) + nth);
			B.PropInt(Mn, TEXT("StartingOwnerTeamID"), Team);
			B.PropBool(Mn, TEXT("RequiresCarriableToArm"), true);
			McomNodes.Add(Mn);
			// the defuse bomb stands 2.5 m beside its MCOM
			FTransform Bt = Mc->Xf;
			Bt.SetLocation(Bt.GetLocation() + Bt.GetRotation().RotateVector(FVector(250.0, 0, 0)));
			if (AActor* Bn = B.Place(TEXT("Bomb"), Bt, FString::Printf(TEXT("BombM%d"), (Team == 1 ? 70 : 80) + nth), Mn))
				B.PropInt(Bn, TEXT("ObjId"), (Team == 1 ? 70 : 80) + nth);
		}
		if (Sector) B.LinkList(Sector, TEXT("CapturePoints"), McomNodes);
		for (int32 i = 0; i < S.Bombs.Num(); i++)
		{
			AActor* Bn = B.Place(TEXT("Bomb"), S.Bombs[i]->Xf,
				i == 0 ? FString(TEXT("Bomb")) : FString::Printf(TEXT("Bomb%d"), i + 1), TEXT("Objectives"));
			if (!Bn) continue;
			B.PropInt(Bn, TEXT("ObjId"), 5 + i);
			B.PropInt(Bn, TEXT("Attacker_TeamID"), 0);
			B.PropBool(Bn, TEXT("SpawnAtStart"), false);
			if (Cv) B.Link(Bn, TEXT("AllowedBombPlayspace"), Cv);
		}

		// ---- vehicles, guns, resupply ----
		FVehicleSort V;
		SortVehicles(S, InBase, S.Flags, V);
		for (const FObject* P : ExtraPads) V.Pads.Add(P);
		for (const FObject* Sl : S.Slots) V.Pads.Add(Sl);
		int32 VCount[3] = { 0, 0, 0 };
		int32 Field = 0;
		for (const FObject* Vp : V.Pads)
		{
			const int32 Team = InBase(Vp->Xf.GetLocation());
			AActor* N = nullptr;
			if (Team > 0)
			{
				VCount[Team]++;
				N = B.Place(TEXT("VehicleSpawner"), Vp->Xf,
					FString::Printf(TEXT("%s%d"), Vp->bWater ? TEXT("Boat") : TEXT("VehicleSpawner"), VCount[Team]),
					FString::Printf(TEXT("Vehicles/Team%d"), Team));
				if (N) B.PropInt(N, TEXT("ObjId"), 210 + Team * 100 + VCount[Team]);
			}
			else
			{
				N = B.Place(TEXT("VehicleSpawner"), Vp->Xf, FString::Printf(TEXT("VehicleSpawner%d"), ++Field), TEXT("Vehicles"));
				if (N) B.PropInt(N, TEXT("ObjId"), 600 + Field);
			}
			if (!N) continue;
			if (Vp->bWater) B.PropInt(N, TEXT("VehicleType"), 21);
			B.PropInt(N, TEXT("P_DefaultRespawnTime"), 45);
			B.PropBool(N, TEXT("P_AutoSpawnEnabled"), Team > 0);
		}
		int32 GCount = 0;
		for (const FObject* G : V.Guns)
		{
			AActor* N = B.Place(TEXT("StationaryEmplacementSpawner"), G->Xf,
				FString::Printf(TEXT("StationaryEmplacement%d"), ++GCount), TEXT("AA-Defences"));
			if (N) B.PropInt(N, TEXT("ObjId"), 280 + GCount);
		}
		for (int32 i = 0; i < V.Stations.Num(); i++)
			B.Place(TEXT("VehicleResupplyStation"), V.Stations[i]->Xf,
				i == 0 ? FString(TEXT("VehicleResupplyStation")) : FString::Printf(TEXT("VehicleResupplyStation%d"), i + 1),
				TEXT("Resupply"));
		if (V.Stations.Num())
			B.Note(FString::Printf(TEXT("%d station(s) under Resupply from the gem_vehicleresupplystation link - the group is a MIX in game: the positions are right, the KIND is a guess"), V.Stations.Num()));

		// ---- AI spawners and the end camera ----
		for (int32 t = 0; t < HqNodes.Num(); t++)
		{
			if (AActor* Sp = B.Place(TEXT("AI_Spawner"), FTransform(HqPos[t]), FString::Printf(TEXT("AI_Spawner_Team%d"), t + 1), TEXT("AI_Spawners")))
				B.PropInt(Sp, TEXT("ObjId"), 901 + t);
			for (int32 k = 0; k < 3; k++)
				if (AActor* Sp = B.Place(TEXT("AI_Spawner"), FTransform(HqPos[t] + FVector(500.0 * (k + 1), 0, 0)),
					FString::Printf(TEXT("AI_Spawner_T%d_%d"), t + 1, k + 1), TEXT("AI_Spawners")))
					B.PropInt(Sp, TEXT("ObjId"), (t == 0 ? 910 : 920) + k + 1);
		}
		FVector All = FVector::ZeroVector;
		for (const FObject* Mc : Mcoms) All += Mc->Xf.GetLocation();
		if (Mcoms.Num()) All /= (double)Mcoms.Num();
		if (AActor* Cam = B.Place(TEXT("FixedCamera"), FTransform(FRotator(-90, 0, 0).Quaternion(), All + FVector(0, 0, 350.0 * kCm)),
			TEXT("EndGameCamera"), FString()))
			B.PropInt(Cam, TEXT("ObjId"), 950);

		int32 Zn = 0;
		for (const FPoly& Z : S.Zones)
		{
			if (Bases.Contains(&Z) || Holding.Contains(&Z)) continue;
			B.Volume(Z.Pts, Z.C, 0.0, FString::Printf(TEXT("Zone%d"), ++Zn), TEXT("ExtraZones"));
		}
	}

	// ========================================================================
	// EVERY OTHER MODE. The flat build, but grouped: a mode nobody has worked
	// out still gets one node per kind rather than a few hundred objects piled
	// at the mode node.
	// ========================================================================
	void BuildFlat(FBuild& B, const FMode& M)
	{
		FSorted S;
		Sort(M, S);
		TArray<AActor*> Caps;
		TArray<TArray<AActor*>> FlagT1, FlagT2;
		FlagT1.SetNum(S.Flags.Num()); FlagT2.SetNum(S.Flags.Num());
		for (int32 i = 0; i < S.Flags.Num(); i++)
		{
			const FString L = Letter(i);
			AActor* Cp = B.Place(TEXT("CapturePoint"), FTransform(S.Flags[i].C), FString::Printf(TEXT("CapturePoint%s"), *L), TEXT("Objectives"));
			Caps.Add(Cp);
			if (!Cp) continue;
			B.PropInt(Cp, TEXT("ObjId"), 200 + i);
			AActor* Fp = B.Volume(S.Flags[i].Pts, S.Flags[i].C, 30.0, FString::Printf(TEXT("Area-%s"), *L), Cp);
			if (Fp) { if (!B.Link(Cp, TEXT("CaptureArea"), Fp)) B.MarkUnwired(Cp); }
			else B.MarkUnwired(Cp);
		}
		int32 n = 0;
		for (const FObject* Sp : S.Spawns)
		{
			const bool bAtFlag = Sp->FlagIndex >= 0 && Sp->FlagIndex < Caps.Num() && Caps[Sp->FlagIndex];
			AActor* Host = B.Group(TEXT("DeploySpawns"));
			if (bAtFlag)
			{
				const FString L = Letter(Sp->FlagIndex);
				const int32 Side = Sp->Team == 2 ? 2 : 1;
				AActor* SpawnsNode = B.GroupUnder(FString::Printf(TEXT("Objectives/Spawns%s"), *L),
					Caps[Sp->FlagIndex], FString::Printf(TEXT("Spawns%s"), *L));
				Host = B.GroupUnder(FString::Printf(TEXT("Objectives/Spawns%s/Team%d"), *L, Side), SpawnsNode,
					FString::Printf(TEXT("Spawns%s_Team%d"), *L, Side));
			}
			AActor* N = B.Place(TEXT("SpawnPoint"), Sp->Xf, FString::Printf(TEXT("SpawnPoint%d"), ++n), Host);
			if (!N || !bAtFlag) continue;
			// a spawn with no team serves whoever holds the flag, which is what
			// a flag's spawns do in game, so it goes into BOTH lists
			if (Sp->Team != 2) FlagT1[Sp->FlagIndex].Add(N);
			if (Sp->Team != 1) FlagT2[Sp->FlagIndex].Add(N);
		}
		for (int32 i = 0; i < Caps.Num(); i++)
		{
			if (!Caps[i]) continue;
			B.LinkList(Caps[i], TEXT("InfantrySpawnPoints_Team1"), FlagT1[i]);
			B.LinkList(Caps[i], TEXT("InfantrySpawnPoints_Team2"), FlagT2[i]);
		}
		for (int32 i = 0; i < S.Combat.Num(); i++)
		{
			const FPoly& C = S.Combat[i];
			AActor* Ca = B.Place(TEXT("CombatArea"), FTransform(FVector(C.C.X, C.C.Y, S.CeilingCm)),
				i == 0 ? FString(TEXT("CombatArea")) : FString::Printf(TEXT("CombatArea%d"), i + 1), TEXT("Play Area"));
			AActor* Cv = B.Volume(C.Pts, FVector(C.C.X, C.C.Y, S.CeilingCm), 0.0,
				i == 0 ? FString(TEXT("CombatVolume")) : FString::Printf(TEXT("CombatVolume%d"), i + 1),
				Ca ? Ca : B.Group(TEXT("Play Area")));
			if (Ca) { if (Cv) { if (!B.Link(Ca, TEXT("CombatVolume"), Cv)) B.MarkUnwired(Ca); } else B.MarkUnwired(Ca); }
		}
		int32 Zn = 0;
		for (const FPoly& Z : S.Zones)
			B.Volume(Z.Pts, Z.C, Z.O ? Z.O->HeightM : 0.f, FString::Printf(TEXT("Zone%d"), ++Zn), TEXT("Zones"));
		int32 Bn = 0;
		for (const FObject* Bx : S.Boxes)
		{
			AActor* N = B.Place(TEXT("OBBVolume"), Bx->Xf, FString::Printf(TEXT("Box%d"), ++Bn), TEXT("Zones"));
			if (N) B.Prop(N, TEXT("size"), FString::Printf(TEXT("%g,%g,%g"), Bx->SizeGodot.X, Bx->SizeGodot.Y, Bx->SizeGodot.Z));
		}
		int32 Vn = 0;
		for (const FObject* V : S.Vehicles)
		{
			AActor* N = B.Place(V->bStationary ? TEXT("StationaryEmplacementSpawner") : TEXT("VehicleSpawner"), V->Xf,
				FString::Printf(TEXT("%s%d"), V->bStationary ? TEXT("StationaryEmplacement") : TEXT("VehicleSpawner"), ++Vn),
				V->bStationary ? TEXT("AA-Defences") : TEXT("Vehicles"));
			if (N) { B.PropInt(N, TEXT("P_DefaultRespawnTime"), 45); B.PropBool(N, TEXT("P_AutoSpawnEnabled"), true); }
		}
		int32 Sn = 0;
		for (const FObject* Sl : S.Slots)
		{
			AActor* N = B.Place(TEXT("VehicleSpawner"), Sl->Xf, FString::Printf(TEXT("Slot%d"), ++Sn), TEXT("Vehicles"));
			if (N) B.PropBool(N, TEXT("P_AutoSpawnEnabled"), true);
		}
		int32 Rn = 0;
		for (const FObject* R : S.Resupplies)
			B.Place(TEXT("VehicleResupplyStation"), R->Xf, FString::Printf(TEXT("VehicleResupplyStation%d"), ++Rn), TEXT("Resupply"));
		int32 Mn = 0;
		for (const FObject* Mc : S.Mcoms)
		{
			AActor* N = B.Place(TEXT("MCOM"), Mc->Xf, FString::Printf(TEXT("MCOM_%s"), *Letter(Mn)), TEXT("Objectives"));
			if (N) B.PropInt(N, TEXT("ObjId"), 400 + Mn);
			Mn++;
		}
		int32 Bo = 0;
		for (const FObject* Bm : S.Bombs)
		{
			AActor* N = B.Place(TEXT("Bomb"), Bm->Xf, FString::Printf(TEXT("Bomb%d"), ++Bo), TEXT("Objectives"));
			if (N) B.PropInt(N, TEXT("ObjId"), 5 + Bo - 1);
		}
	}

	// One mode, as real objects under one node, in the layout a person authors.
	AActor* BuildGroup(const FString& Key, const FMode& M)
	{
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) return nullptr;
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Build game mode %s"), *Pretty(Key))));
		W->GetCurrentLevel()->Modify();

		FVector Centre = FVector::ZeroVector; int32 n = 0;
		for (const FObject& O : M.Objects) if (O.Kind != EKind::Other) { Centre += O.Xf.GetLocation(); n++; }
		if (n) Centre /= n;

		FBuild B;
		B.Key = Key;
		B.Prefix = SafeName(GroupLabel(Key)) + TEXT("_");
		B.Centre = Centre;
		B.Node = BF6Ext::PlaceNode(GroupLabel(Key), Centre);
		if (!B.Node) return nullptr;
		B.Node->Tags.AddUnique(FName(*(FString(kNodeTag) + Key)));

		if (Key.Contains(TEXT("breakthrough")))       BuildBreakthrough(B, M);
		else if (Key.Contains(TEXT("obliteration")))  BuildObliteration(B, M);
		else if (Key == TEXT("conquest") || Key == TEXT("carrierstrike") || Key == TEXT("escalation")) BuildConquest(B, M);
		else                                          BuildFlat(B, M);

		const int32 Swept = B.SweepEmptyGroups();
		const TArray<FString> Problems = RunChecks(B, Key);

		BF6Ext::SelectNone();
		BF6Ext::RefreshSceneTree();
		// the tool's own safe fixes (polygon winding) as a second opinion
		const int32 Fixed = BF6Ext::FixSafeValidationIssues(false);

		FString Skipped;
		for (const TPair<FString, int32>& kv : B.Skipped)
			Skipped += FString::Printf(TEXT("%s%s x%d"), Skipped.IsEmpty() ? TEXT("") : TEXT(", "), *kv.Key, kv.Value);
		int32 Objects = 0;
		for (const FPlacedRec& R : B.Placed) if (R.Type != TEXT("Node3D")) Objects++;

		// The checks go FIRST: they are the plugin's own mistakes and a creator
		// must not be the one who finds them.
		if (Problems.Num())
			UE_LOG(LogBF6HighPolyGM, Warning, TEXT("mode %s: CHECKS FAILED (%d) - %s"),
				*Key, Problems.Num(), *FString::Join(Problems, TEXT(" ; ")));
		UE_LOG(LogBF6HighPolyGM, Log,
			TEXT("mode %s: %d object(s) placed, %d link(s) wired, %d group node(s) (%d empty one(s) removed); ")
			TEXT("%d polygon(s) rewound by the validator%s%s"),
			*Key, Objects, B.Links, B.Groups.Num(), Swept, Fixed,
			Skipped.IsEmpty() ? TEXT("") : TEXT("; the library has no "), *Skipped);
		const FString Cov = CoverageLine(M);
		if (!Cov.IsEmpty()) UE_LOG(LogBF6HighPolyGM, Log, TEXT("mode %s: %s"), *Key, *Cov);
		for (const FString& N : B.Notes)
			UE_LOG(LogBF6HighPolyGM, Log, TEXT("mode %s: hand-finish - %s"), *Key, *N);

		BF6Ext::Notify(FString::Printf(TEXT("%s: %d object(s) built, %d link(s) wired.%s One Ctrl+Z removes them all."),
			*Pretty(Key), Objects, B.Links,
			Problems.Num() ? *FString::Printf(TEXT(" %d CHECK(S) FAILED - see the log."), Problems.Num()) : TEXT("")));
		return B.Node;
	}
#endif

	// Show one mode's objects, building them if they are not there.
	void ShowObjects(const FString& Key)
	{
		// hide every other mode first
		for (const TPair<FString, FMode>& kv : GModes)
		{
			if (kv.Key == Key) continue;
			if (AActor* N = FindGroup(kv.Key)) SetSubtreeHidden(N, true);
			if (AActor* L = FindAddonActor(LabelsName(kv.Key))) L->SetIsTemporarilyHiddenInEditor(true);
			if (AActor* Mk = FindAddonActor(MarkersName(kv.Key))) Mk->SetIsTemporarilyHiddenInEditor(true);
		}
		const FMode* M = GModes.Find(Key);
		if (!M) return;
		if (AActor* N = FindGroup(Key))
		{
			SetSubtreeHidden(N, false);
			Relabel(Key, *M);
			if (AActor* Mk = FindAddonActor(MarkersName(Key))) Mk->Destroy();
			BF6Ext::Notify(FString::Printf(TEXT("%s: shown, %d object(s) already in the scene"), *Pretty(Key), CountObjects(N)));
			UE_LOG(LogBF6HighPolyGM, Log, TEXT("mode %s: shown, %d object(s) already in the scene"), *Key, CountObjects(N));
			return;
		}
#if defined(BF6EXT_HAS_PLACEMENT)
		if (BF6Ext::IsEditing())
		{
			if (AActor* Mk = FindAddonActor(MarkersName(Key))) Mk->Destroy();
			if (BuildGroup(Key, *M)) { Relabel(Key, *M); return; }
		}
#endif
		BuildMarkers(Key, *M);
		Relabel(Key, *M);
	}

	void HideAllObjects()
	{
		for (const TPair<FString, FMode>& kv : GModes)
		{
			if (AActor* N = FindGroup(kv.Key)) SetSubtreeHidden(N, true);
			if (AActor* L = FindAddonActor(LabelsName(kv.Key))) L->SetIsTemporarilyHiddenInEditor(true);
			if (AActor* Mk = FindAddonActor(MarkersName(kv.Key))) Mk->SetIsTemporarilyHiddenInEditor(true);
		}
	}

	// Remove one mode's node and everything under it, in one transaction.
	int32 RemoveGroup(const FString& Key)
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return 0;
		AActor* Node = FindGroup(Key);
		if (!Node) return 0;
		TArray<AActor*> Doomed;
		Node->GetAttachedActors(Doomed, true, true);
		Doomed.Add(Node);
		FScopedTransaction Tx(FText::FromString(TEXT("Remove game mode objects")));
		for (AActor* A : Doomed) { A->Modify(); W->EditorDestroyActor(A, true); }
		return Doomed.Num();
	}

	// ---- the ring -------------------------------------------------------
	FString StatusOf(const FString& Key)
	{
		const bool bChosen = GChosen.Equals(Key, ESearchCase::IgnoreCase);
		if (!GMined) return bChosen ? TEXT("art shown") : TEXT("art only");
		const FMode* M = GModes.Find(Key);
		const int32 n = M ? M->Objects.Num() : 0;
		if (FindGroup(Key)) return bChosen ? FString::Printf(TEXT("shown, %d objects"), n) : TEXT("built, hidden");
		return bChosen ? FString::Printf(TEXT("%d objects"), n) : FString::Printf(TEXT("%d mined"), n);
	}

	bool Tick(float)
	{
		if (!GEditor) return true;
		AActor* Art = ArtActor();
		if (Art != GArtActorSeen.Get())
		{
			GArtActorSeen = Art;
			if (Art) ApplyArt();
		}
		return true;
	}

	void OnMapOpened(const FString& Level, const FString& /*Save*/)
	{
		if (Level != GLevel)
		{
			GModes.Reset();
			GMined = false;
			GMineSummary.Reset();
		}
		GLevel = Level;
		GChosen = LoadChoice(Level);
		GArtActorSeen = nullptr;
		UE_LOG(LogBF6HighPolyGM, Log, TEXT("game modes: %s opened, remembered mode \"%s\""),
			*Level, GChosen.IsEmpty() ? TEXT("(largest)") : *GChosen);

		// READ THE MAP'S GAME MODES AS SOON AS IT OPENS.
		//
		// Mining is what fills the GAME MODE dropdown, and it used to happen
		// only when somebody picked a mode - so the list was empty exactly when
		// they went looking for it, and getting it meant turning High Poly on
		// first. Somebody who only wants to lay out a base game mode should not
		// have to build scenery to see which modes the map has.
		//
		// Off the game thread and quiet: it costs a background read and says
		// nothing unless it finds something.
		if (GAutoMine && !GMined && !GMining && !InstallDir().IsEmpty())
		{
			GQuietMine = true;
			Mine(false);
			GQuietMine = false;
		}
		else if (!GAutoMine)
		{
			UE_LOG(LogBF6HighPolyGM, Log,
				TEXT("game modes: not reading %s automatically (BF6.HighPoly.GameModeAutoMine is off); ")
				TEXT("pick a mode or run BF6.HighPoly.ModeMine when you want the list."), *Level);
		}
	}

	void OnMapClosing(const FString& /*Level*/)
	{
		BF6Ext::ClearAddonActors(kAddon);
		GLevel.Reset();
		GArtActorSeen = nullptr;
	}

	void Shutdown()
	{
		GMineClosing.store(true);
		BF6Ext::UnregisterPieEntry(FName("HighPoly.GameMode"));
		if (GTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTicker); GTicker.Reset(); }

		// NOBODY IS INSIDE THE READER WHEN IT CLOSES.
		//
		// Ten seconds is insufficient on restricted CPUs. Leaving the context
		// allocated after a timeout did not keep its DLL loaded: a quick-exit
		// dump caught a worker executing inside unloaded bf6_core.dll. Finish
		// the reader before allowing module teardown, including queued workers.
		const double JoinStarted = FPlatformTime::Seconds();
		bool ReportedWait = false;
		while (GMineWorkers.load() > 0)
		{
			if (!ReportedWait && FPlatformTime::Seconds() - JoinStarted > 10.0)
			{
				ReportedWait = true;
				UE_LOG(LogBF6HighPolyGM, Warning,
					TEXT("game modes: waiting for the active reader before unloading its DLL"));
			}
			FPlatformProcess::Sleep(0.01f);
		}
		// close while the dll is still loaded, never from a static destructor
		GCore.Shutdown();
	}

	void Init()
	{
		if (GInited) return;
		GInited = true;

		BF6Ext::FPieEntry E;
		E.Id    = FName("HighPoly.GameMode");
		E.Label = TEXT("GAME MODE");
		E.Sub   = TEXT("the game's own layouts");
		E.Order = 910;   // right after HIGH POLY
		E.IsAvailable = []{ return !BF6Ext::CurrentLevel().IsEmpty(); };
		E.OnPick = [](FVector2D Center){ OpenRing(Center); };
		BF6Ext::RegisterPieEntry(E);

		BF6Ext::OnMapOpened().AddStatic(&OnMapOpened);
		BF6Ext::OnMapClosing().AddStatic(&OnMapClosing);
		FCoreDelegates::OnEnginePreExit.AddStatic(&Shutdown);
		GTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.5f);

		// a map may already be open when the module comes up late
		const FString Now = BF6Ext::CurrentLevel();
		if (!Now.IsEmpty()) OnMapOpened(Now, BF6Ext::CurrentSave());

#if defined(BF6EXT_HAS_PLACEMENT)
		const TCHAR* Seam = TEXT("present (objects will be placed)");
#else
		const TCHAR* Seam = TEXT("absent (objects drawn as markers until patch 01 is applied)");
#endif
		UE_LOG(LogBF6HighPolyGM, Log, TEXT("game modes attached: GAME MODE pill, BF6.HighPoly.Mode, seam %s"), Seam);
	}

	// Registered from a static initializer. The module loads in the Default
	// phase, before OnPostEngineInit fires, so that delegate is the normal
	// path; if it has already fired (a later reload) the ticker below catches
	// it on the next frame.
	struct FAutoRegister
	{
		FAutoRegister()
		{
			FCoreDelegates::OnPostEngineInit.AddStatic(&Init);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				if (GEngine && GIsRunning) Init();
				return !GInited;   // keep polling until it has run
			}), 1.0f);
		}
	} GAutoRegister;
}

// ============================================================================
// public
// ============================================================================

FString Pretty(const FString& Key)
{
	static const TMap<FString, FString> Special = {
		{ TEXT("teamdeathmatch"), TEXT("Team Deathmatch") }, { TEXT("kingofthehill"), TEXT("King of the Hill") },
		{ TEXT("strikepoint"), TEXT("Strike Point") },       { TEXT("battleroyale"), TEXT("Battle Royale") },
		{ TEXT("squaddeathmatch"), TEXT("Squad Deathmatch") }, { TEXT("frontline"), TEXT("Frontline") },
		{ TEXT("carrierstrike"), TEXT("Carrier Strike") },   { TEXT("all"), TEXT("All modes") },
	};
	if (const FString* S = Special.Find(Key.ToLower())) return *S;
	// "winter_domination" -> "Winter Domination"
	FString Out;
	bool bUp = true;
	for (TCHAR c : Key)
	{
		if (c == TEXT('_')) { Out.AppendChar(TEXT(' ')); bUp = true; continue; }
		Out.AppendChar(bUp ? FChar::ToUpper(c) : c);
		bUp = false;
	}
	return Out;
}

FString KeyOf(const FString& In)
{
	const FString S = In.TrimStartAndEnd();
	if (S.IsEmpty() || S.Equals(TEXT("off"), ESearchCase::IgnoreCase)) return FString();
	if (S.Equals(TEXT("all"), ESearchCase::IgnoreCase)) return TEXT("all");
	TArray<FString> Known;
	KnownModes(Known);
	for (const FString& K : Known)
		if (K.Equals(S, ESearchCase::IgnoreCase) || Pretty(K).Equals(S, ESearchCase::IgnoreCase)
		    || GroupLabel(K).Equals(S, ESearchCase::IgnoreCase)) return K;
	return S.ToLower();
}

void KnownModes(TArray<FString>& Out)
{
	TMap<FString, int32> Weight;
	ArtModes(Weight);
	for (const TPair<FString, FMode>& kv : GModes) Weight.FindOrAdd(kv.Key) += kv.Value.Objects.Num();
	Out.Reset();
	for (const TPair<FString, int32>& kv : Weight) Out.Add(kv.Key);
	Out.Sort([&Weight](const FString& a, const FString& b)
	{
		const int32 wa = Weight[a], wb = Weight[b];
		return wa != wb ? wa > wb : a < b;
	});
}

FString CurrentMode() { return GChosen; }
bool IsMined()  { return GMined; }
bool IsMining() { return GMining; }
bool AutoMine() { return GAutoMine; }
void SetAutoMine(bool bOn) { GAutoMine = bOn; }

void Mine(bool bThenApplyChoice)
{
	if (GMineClosing.load() || IsEngineExitRequested() || BF6HP::Shared::CoreShuttingDown()) return;
	if (GMining) { GApplyAfterMine |= bThenApplyChoice; return; }
	// Never start core work once the add-on has begun going down: the join in
	// Shutdown waits for what is already running, and a wait it can lose a race
	// with is no wait.
	const FString Level = BF6Ext::CurrentLevel();
	if (Level.IsEmpty()) { BF6Ext::Notify(TEXT("Game modes: open a map first.")); return; }
	const FString Install = InstallDir();
	if (Install.IsEmpty())
	{
		BF6Ext::Notify(TEXT("Game modes: High Poly needs to know where Battlefield 6 is installed. Open HIGH POLY once."));
		return;
	}
	GLevel = Level;
	GMining = true;
	GApplyAfterMine = bThenApplyChoice;
	UE_LOG(LogBF6HighPolyGM, Log, TEXT("game modes: mining %s from %s"), *Level, *Install);
	if (!GQuietMine)
	{
		BF6Ext::Notify(FString::Printf(TEXT("Reading %s's game modes from the game..."), *Level));
	}
	const FString Dll = DllPath();
	// Count the job before dispatch, so shutdown also sees work that has not
	// yet entered its worker function.
	GMineWorkers.fetch_add(1);
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Level, Install, Dll]{ MineOffThread(Level, Install, Dll); });
}

void SetMode(const FString& In)
{
	const FString Key = KeyOf(In);
	GChosen = Key;
	SaveChoice();
	ApplyArt();
	if (Key.IsEmpty() || Key == TEXT("all") || !GObjectsOn)
	{
		if (GMined) HideAllObjects();
		BF6Ext::Notify(Key == TEXT("all") ? TEXT("Every mode's art shown, stacked. Gameplay objects hidden.")
		             : Key.IsEmpty() ? TEXT("Game mode off: art on the largest mode, gameplay objects hidden.")
		             : FString::Printf(TEXT("%s: art shown. Gameplay objects are off on the ring."), *Pretty(Key)));
		return;
	}
	if (!GMined) { Mine(true); return; }
	if (!GModes.Contains(Key))
	{
		HideAllObjects();
		BF6Ext::Notify(FString::Printf(TEXT("%s: art shown; the game authors no gameplay entities for it."), *Pretty(Key)));
		return;
	}
	ShowObjects(Key);
}

int32 RemoveMode(const FString& In)
{
	const FString Key = KeyOf(In);
	if (Key.IsEmpty() || Key == TEXT("all")) return 0;
	const int32 n = RemoveGroup(Key);
	if (n) UE_LOG(LogBF6HighPolyGM, Display, TEXT("game mode %s: removed %d actor(s) before rebuilding"), *Key, n);
	return n;
}

void OnArtBuilt()
{
	GArtActorSeen = nullptr;
	ApplyArt();
}

// The modes as a list, for the panel's dropdown. The radial builds its pills
// from the same KnownModes call, so the two cannot end up offering different
// modes.
//
// Index 0 is "no mode chosen", which is a real state rather than a blank: it
// means the largest mode the map has. Leaving it out would make the dropdown
// unable to show what the map is actually doing before anyone has chosen.
TArray<FString> ModeLabels()
{
	TArray<FString> Keys;
	KnownModes(Keys);
	TArray<FString> Out;
	Out.Reserve(Keys.Num() + 1);
	Out.Add(TEXT("Largest on this map"));
	for (const FString& K : Keys) { Out.Add(Pretty(K)); }
	return Out;
}

int32 CurrentModeIndex()
{
	const FString Now = CurrentMode();
	if (Now.IsEmpty()) { return 0; }
	TArray<FString> Keys;
	KnownModes(Keys);
	const int32 At = Keys.IndexOfByKey(Now);
	return At == INDEX_NONE ? 0 : At + 1;
}

void SetModeByIndex(int32 Index)
{
	TArray<FString> Keys;
	KnownModes(Keys);
	if (Index <= 0) { SetMode(FString()); return; }
	if (Keys.IsValidIndex(Index - 1)) { SetMode(Keys[Index - 1]); }
}

void OpenRing(FVector2D Center)
{
	TArray<BF6Ext::FPieSubEntry> R;
	TArray<FString> Modes;
	KnownModes(Modes);
	for (const FString& K : Modes)
	{
		BF6Ext::FPieSubEntry E;
		E.Label = Pretty(K).ToUpper();
		E.Sub = [K]{ return StatusOf(K); };
		E.OnPick = [K]{ SetMode(K); };
		R.Add(E);
	}
	if (Modes.Num() == 0)
	{
		BF6Ext::FPieSubEntry E;
		E.Label = TEXT("NO MODES YET");
		E.Sub = []{ return FString(GMining ? TEXT("reading the game...") : TEXT("build once, or mine below")); };
		E.OnPick = []{};
		R.Add(E);
	}
	{
		BF6Ext::FPieSubEntry E;
		E.Label = TEXT("ALL ART");
		E.Sub = []{ return FString(GChosen == TEXT("all") ? TEXT("stacked, current") : TEXT("stack every mode")); };
		E.OnPick = []{ SetMode(TEXT("all")); };
		R.Add(E);
	}
	{
		BF6Ext::FPieSubEntry E;
		E.Label = TEXT("OBJECTS");
		E.Sub = []{ return FString(GObjectsOn ? TEXT("placed with the mode") : TEXT("art only")); };
		E.OnPick = []{ GObjectsOn = !GObjectsOn; if (!GObjectsOn) HideAllObjects(); else if (!GChosen.IsEmpty() && GChosen != TEXT("all")) SetMode(GChosen); };
		R.Add(E);
	}
	{
		BF6Ext::FPieSubEntry E;
		E.Label = TEXT("MINE");
		E.Sub = []{ return FString(GMining ? TEXT("reading the game...") : GMined ? TEXT("read again") : TEXT("read the layers")); };
		E.OnPick = []{ GMined = false; GModes.Reset(); Mine(!GChosen.IsEmpty() && GChosen != TEXT("all")); };
		R.Add(E);
	}
	{
		BF6Ext::FPieSubEntry E;
		E.Label = TEXT("OFF");
		E.Sub = []{ return FString(GChosen.IsEmpty() ? TEXT("current") : TEXT("largest art, no objects")); };
		E.OnPick = []{ SetMode(FString()); };
		R.Add(E);
	}
	BF6Ext::OpenPieSubRing(R, Center);
}

// The headless path. It cannot open a level - the add-on has no seam for that
// and will not reach past the tool to do it - so it runs on the open map, and
// says so when it is asked for another. Everything else is the real path: the
// real mine, the real build, the real checks, and where the fork published a
// count for this level and mode, a comparison against it.
//
// THE TABLE BELOW IS A TEST ORACLE. No builder reads it; it exists so this
// harness can say "the fork measured 12 here and so did we" and nothing else.
void RunModeTest(const FString& Level, const FString& Mode)
{
	struct FOracle { const TCHAR* Level; const TCHAR* Mode; int32 Objectives; };
	static const FOracle kOracles[] = {
		{ TEXT("mp_isolated"),      TEXT("conquest"),     9 },   // the big-flag rescue
		{ TEXT("mp_golmudrailway"), TEXT("conquest"),     7 },   // 12 authored, five pairs merged
		{ TEXT("mp_contaminated"),  TEXT("breakthrough"), 11 },
	};

	const FString Open = BF6Ext::CurrentLevel();
	if (Open.IsEmpty()) { UE_LOG(LogBF6HighPolyGM, Warning, TEXT("mode test: open a map first")); return; }
	if (!Level.IsEmpty() && !Level.Equals(Open, ESearchCase::IgnoreCase))
	{
		UE_LOG(LogBF6HighPolyGM, Warning,
			TEXT("mode test: %s is open and the add-on cannot open another map (no seam for it). Open %s and run this again."),
			*Open, *Level);
		return;
	}
	if (!GMined)
	{
		UE_LOG(LogBF6HighPolyGM, Display, TEXT("mode test: nothing mined yet, reading %s now - run this again when the mine finishes"), *Open);
		Mine(false);
		return;
	}
	const FString Key = KeyOf(Mode);
	const FMode* M = GModes.Find(Key);
	if (!M) { UE_LOG(LogBF6HighPolyGM, Warning, TEXT("mode test: %s has no mode \"%s\""), *Open, *Key); return; }

	int32 Fails = 0;
	int32 Captures = 0;
	for (const FObject& O : M->Objects) if (O.Kind == EKind::Capture) Captures++;
	for (const FOracle& Or : kOracles)
		if (Open.Equals(Or.Level, ESearchCase::IgnoreCase) && Key.Equals(Or.Mode, ESearchCase::IgnoreCase))
		{
			const bool bOk = Captures == Or.Objectives;
			if (!bOk) Fails++;
			UE_LOG(LogBF6HighPolyGM, Display, TEXT("mode test: %s %s objectives %d, fork measured %d - %s"),
				*Open, *Key, Captures, Or.Objectives, bOk ? TEXT("ok") : TEXT("FAIL"));
		}
	UE_LOG(LogBF6HighPolyGM, Display, TEXT("mode test: %s"), *CoverageLine(*M));

#if defined(BF6EXT_HAS_PLACEMENT)
	if (!BF6Ext::IsEditing())
	{
		UE_LOG(LogBF6HighPolyGM, Warning, TEXT("mode test: this is a read-only base map, so nothing can be placed; the mine was checked and the build was not"));
		return;
	}
	const int32 Removed = RemoveGroup(Key);
	if (Removed) UE_LOG(LogBF6HighPolyGM, Display, TEXT("mode test: removed %d actor(s) from an earlier build"), Removed);
	AActor* Node = BuildGroup(Key, *M);
	if (!Node) { UE_LOG(LogBF6HighPolyGM, Warning, TEXT("mode test: the build placed nothing - FAIL")); return; }
	UE_LOG(LogBF6HighPolyGM, Display, TEXT("mode test: built %d object(s) under %s"), CountObjects(Node), *Node->GetActorLabel());
#endif
	UE_LOG(LogBF6HighPolyGM, Display, TEXT("mode test: %s"), Fails ? TEXT("FAIL") : TEXT("PASS"));
}

}  // namespace BF6HPGameMode

// ============================================================================
// console
// ============================================================================

static FAutoConsoleCommand GBF6HPModeCmd(
	TEXT("BF6.HighPoly.Mode"),
	TEXT("Show one game mode: its art at once, its gameplay objects placed and linked (mined first if needed). "
	     "BF6.HighPoly.Mode <name|all|off>; no argument lists what the open map has."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			TArray<FString> M;
			BF6HPGameMode::KnownModes(M);
			UE_LOG(LogBF6HighPolyGM, Display, TEXT("game mode: current \"%s\"; known: %s; mined: %s"),
				BF6HPGameMode::CurrentMode().IsEmpty() ? TEXT("(largest)") : *BF6HPGameMode::CurrentMode(),
				M.Num() ? *FString::Join(M, TEXT(", ")) : TEXT("none yet"),
				BF6HPGameMode::IsMined() ? TEXT("yes") : BF6HPGameMode::IsMining() ? TEXT("in progress") : TEXT("no"));
			return;
		}
		BF6HPGameMode::SetMode(FString::Join(Args, TEXT(" ")));
	}));

static FAutoConsoleCommand GBF6HPAutoMineCmd(
	TEXT("BF6.HighPoly.GameModeAutoMine"),
	TEXT("Read a map's game modes from the game as soon as it opens (default 1). Off means the GAME MODE ")
	TEXT("list fills when you first ask for it instead, which keeps the busiest moment - the frame after a ")
	TEXT("big map appears - free of a background read of the install."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		BF6HPGameMode::SetAutoMine(Args.Num() >= 1
			? (Args[0] == TEXT("1") || Args[0].Equals(TEXT("on"), ESearchCase::IgnoreCase))
			: BF6HPGameMode::AutoMine());
		UE_LOG(LogBF6HighPolyGM, Display, TEXT("game modes read on map open: %s"),
			BF6HPGameMode::AutoMine() ? TEXT("yes") : TEXT("no"));
	}));

static FAutoConsoleCommand GBF6HPModeMineCmd(
	TEXT("BF6.HighPoly.ModeMine"),
	TEXT("Read the open map's gameplay layers from the game again and log a census per mode."),
	FConsoleCommandDelegate::CreateStatic([]{ BF6HPGameMode::Mine(false); }));

static FAutoConsoleCommand GBF6HPModeBuildCmd(
	TEXT("BF6.HighPoly.ModeBuild"),
	TEXT("Rebuild a mode's gameplay objects from the mined data: removes the mode's node and everything under it, "
	     "then places them again. BF6.HighPoly.ModeBuild [name] (default: the current mode)."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const FString Key = BF6HPGameMode::KeyOf(Args.Num() ? FString::Join(Args, TEXT(" ")) : BF6HPGameMode::CurrentMode());
		if (Key.IsEmpty() || Key == TEXT("all")) { UE_LOG(LogBF6HighPolyGM, Display, TEXT("usage: BF6.HighPoly.ModeBuild <name>")); return; }
		BF6HPGameMode::RemoveMode(Key);
		BF6HPGameMode::SetMode(Key);
	}));

static FAutoConsoleCommand GBF6HPModeTestCmd(
	TEXT("BF6.HighPoly.ModeTest"),
	TEXT("Mine the open map, build one mode, run the build checks and compare the objective count against the "
	     "fork's published numbers where there are any. BF6.HighPoly.ModeTest <mode> [level]."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const FString Mode = Args.Num() > 0 ? Args[0] : BF6HPGameMode::CurrentMode();
		const FString Level = Args.Num() > 1 ? Args[1] : FString();
		BF6HPGameMode::RunModeTest(Level, Mode);
	}));
