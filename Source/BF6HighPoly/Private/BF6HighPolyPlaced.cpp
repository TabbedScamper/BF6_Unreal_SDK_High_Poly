#include "BF6HighPolyPlaced.h"
#include "BF6HighPolyControls.h"
#include "BF6HighPolyShared.h"
#include "BF6HighPolyCore.h"
#include "BF6SDKExtension.h"
#include "BF6HighPolyLoadout.h"

#include "Async/Async.h"
#include "Async/ParallelFor.h"      // the placed build describes and commits across cores
#include "Misc/SecureHash.h"          // the cache key is a hash of the type name
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/LightComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Selection.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

THIRD_PARTY_INCLUDES_START
#include "bf6_core.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogBF6HighPolyPlaced, Log, All);

// ============================================================================
// HOW A PLACED OBJECT BECOMES THE REAL THING.
//
// The tool spawns every placed object as an AActor tagged BF6Placed with a
// UProceduralMeshComponent root filled from the SDK's low-poly model, plus a
// "label:<Type>" tag (the placeable's name) and a "mesh:<Model>" tag. The Godot
// plugin's answer to the same object is the game's own pf_portal_<name> prefab
// assembled at identity (highpoly_gamesource.object_node), which is the same
// frame the SDK's model was baked in, so the overlay sits directly on the proxy
// and only a shape check (highpoly_lib._fit_scale) stands between them.
//
// This module does the same with libbf6: bf6_asset_instances walks the prefab
// and returns its member meshes and their prefab-local transforms;
// bf6_asset_lights walks it again for the fixtures it carries. Members are
// merged into ONE UStaticMesh per placeable type (two hundred benches share one
// mesh, exactly as Godot's mesh_for cache shares one Mesh), and every placed
// actor of that type gets a UStaticMeshComponent hung on its own root.
//
// ON THE PLACED ACTOR, NOT ON A COMPANION. Godot hangs the overlay under the
// placed node as an owner=null child, and the reason carries over: a click on
// the high-poly geometry must select the OBJECT. A separate add-on actor under
// the cursor would either swallow the click (it is filtered out of the tool's
// selection) or pass it through to nothing, because the proxy it stands in for
// is hidden. A component on the actor selects its owner, follows its transform
// for free, dies with it on delete and is absent from a duplicate, which the
// poll then dresses. The components carry the add-on tag, nothing of the
// tool's, and are never added to the actor's instance-component list, so they
// stay out of the details panel and out of transactions.
//
// PACED. Resolution walks the install and builds meshes; the Godot swap yields
// on a time budget (highpoly_toggle._apply_scene: 400 ms then adaptive) so the
// editor never stalls. Here one type is resolved per tick at most and the tick
// stops touching records once its budget is spent. The poll that notices new,
// moved-away and deleted objects runs once a second, because the tool exposes
// no placement events through the seam.
//
// NAMED, NOT ANONYMOUS. A unity build concatenates this file with
// BF6HighPoly.cpp, whose anonymous namespace already owns kAddonName and a
// hundred G-prefixed statics; a second anonymous namespace in the same
// translation unit would collide on the first shared spelling.
// ============================================================================
namespace BF6HP
{
namespace PlacedImpl
{
	const TCHAR* kAddonName       = TEXT("HighPoly");
	const FName  kPlacedTag("BF6Placed");           // the tool's tag: read, never written
	const FName  kOurComponentTag("BF6HPPlaced");   // on everything this module makes
	const TCHAR* kPortalPrefix    = TEXT("pf_portal_");
	// The same string for the name-table search, which takes UTF-8. Kept beside
	// its TCHAR twin so the two cannot drift.
	const char*  kPortalPrefixUtf8 = "pf_portal_";
	const TCHAR* kIniSection      = TEXT("BF6HighPoly.Placed");

	// SOME PREFABS CARRY AN ART-CATEGORY PREFIX THE SDK NAME DROPS. The same
	// list Godot surveyed (highpoly_gamesource.NAME_PREFIXES): a match is taken
	// only when exactly one category has it, so this never becomes fuzzy
	// matching that puts a wrong model on a renamed object.
	const TCHAR* kArtPrefixes[] = {
		TEXT("com_"), TEXT("mil_"), TEXT("fed_"), TEXT("naf_"), TEXT("cas_"),
		TEXT("ind_"), TEXT("psd_"), TEXT("ter_"), TEXT("veg_"), TEXT("ob_") };

	// ---- settings -----------------------------------------------------------
	bool  GEnabled         = true;
	bool  GLightsOn        = true;
	bool  GPreviewSelected = false;

	// ---- how far away the real models are worth drawing ----------------------
	//
	// A DISTANCE SWAP, NOT A HOLE.
	//
	// Every placed high-poly object is one draw call carrying its full LOD0
	// geometry: they go through the runtime render path rather than Nanite,
	// because a placed prop is one draw per placement and Nanite's hierarchy
	// buys it little. That is fine for the dozen objects near the camera and
	// expensive for the three thousand that are not - two building types alone
	// merge to 2.5 and 2.3 million triangles and are placed twelve times
	// between them.
	//
	// So past this distance the add-on stops drawing its model AND lets the
	// SDK's own blockout draw again. The creator keeps a complete city either
	// way; what changes is how much of it is the real geometry. Culling to
	// nothing would leave holes in the skyline, which is a different feature
	// and a worse one.
	float GCullMetres  = 100.f;   // the slider, 10 to 1000
	bool  GNoCull      = false;   // "DRAW AT ANY DISTANCE", off by default
	// THE SLIDER IS A DISTANCE TO THE OBJECT'S SURFACE, NOT TO ITS CENTRE.
	//
	// r.DistanceCullToSphereEdge is on by default, and the far test compares
	// the CLOSEST point of the bounds. So a building is still drawn while you
	// stand against its wall no matter how large it is, and this needs no
	// radius correction of its own - an earlier version added one on the
	// assumption the test used the bounds origin, which was simply wrong.
	float CullCentimetres()
	{
		return GCullMetres * 100.f;
	}
	// Work per tick before the rest waits for the next frame.
	float GBudgetMs        = 8.f;
	// What the FIRST dress of a map gets instead. See the note where it is used.
	float GFirstDressBudgetMs = 12.f;
	bool GFinishingBuild = false;
	// How long a type that found the reader busy waits before asking again. A
	// light walk is most of a second, so anything shorter just burns the merge
	// over and over; see NextTryAt on FTypeAsset.
	double GBusyRetrySecs = 0.5;
	// Any type still to be resolved or built this tick. Computed once, read by
	// the deferred light pass so fixtures never compete with geometry.
	bool   GGeometryOutstanding = false;

	// THE GOVERNOR.
	//
	// The budget above is checked BEFORE a resolve and never during one, and a
	// resolve cannot be interrupted: the archive walk and the UStaticMesh build
	// are both game-thread-only and neither takes a deadline. So the budget was
	// only ever advisory, and one prefab that took 38 s took the whole editor
	// with it.
	//
	// What can be controlled is how OFTEN a resolve starts. After each one the
	// resolver stands down for long enough to keep its share of the frame time
	// under GDutyCycle, so a 2.4 s resolve buys about 7 s of ordinary frames
	// before the next. Props still arrive; the editor stays usable while they do.
	// Kept for the console command, which can still slow the resolver down on
	// a machine that needs it. It is no longer a multiplier on every expensive
	// resolve: see the note where GBusyUntil is set.
	float  GDutyCycle      = 0.5f;
	double GBusyUntil      = 0.0;     // no new resolve before this time

	// What the resolving actually cost, so the next decision about pacing is
	// made from measurements rather than from a feeling about how slow it looks.
	// Where the per-tick record pass stopped, so the next one continues rather
	// than starting again at the top of three and a half thousand records.
	int32  GCursor           = 0;
	// The worst tick of each phase since the last report, so a stutter can be
	// attributed instead of argued about.
	// Cold-start accounting: where the seconds go across a whole scene.
	double GPhWalk = 0.0, GPhDecode = 0.0, GPhMerge = 0.0, GPhBuild = 0.0, GPhLights = 0.0, GPhReport = 0.0;
	// The whole of ResolveType, however it exits, so the parts can be checked
	// against the sum rather than assumed to be it.
	double GPhInner = 0.0;
	// How many times a type was put back because the reader was in use. The
	// number that used to be seconds of frozen editor, and is now a retry.
	int32  GDeferredBusy = 0;
	// And what those attempts cost, so the phase table balances: InnerTimer
	// counts every exit including a deferral, but GResolveSecsTotal counts only
	// the ones that concluded, and the difference showed up as negative time.
	double GDeferSecs = 0.0;
	// Wall clock from the map opening to the scene being dressed. Zero until a
	// map opens, so a session that never opened one reports nothing.
	double GMapOpenedAt = 0.0;
	// Naming every texture sheet of every section is a diagnostic, and it is a
	// reader call per binding. Off by default; see where it is used.
	bool   GSectionReports = false;
	double GWorstPollMs = 0.0, GWorstSelMs = 0.0, GWorstRecMs = 0.0, GLastCostSaid = 0.0;
	// How many single objects have run past the limit. One is an object; several
	// is the scene, and only then does the whole module stand down.
	int32  GRunaways         = 0;
	int32  GRunawayLimit     = 3;
	int32  GTypesResolved    = 0;
	double GResolveSecsTotal = 0.0;
	double GSlowestSecs      = 0.0;
	FString GSlowestKey;
	bool   GSummarySaid      = false;
	// A single resolve past this is not slow, it is broken, and standing down
	// politely is the wrong answer: the resolver stops and says so.
	float  GStallSecs      = 12.f;
	bool   GStalled        = false;
	// WHICH TYPE STOPPED IT, so the panel can say and so it can be skipped.
	// A stop that only exists as a log line leaves somebody toggling unrelated
	// switches to find out what happened.
	FString GStalledOn;
	// Types the user chose to give up on. Kept for the session: a type that
	// cost twelve seconds once will cost it again on the next tick, and
	// retrying it forever is how a map never finishes.
	TSet<FString> GSkipped;
	// WHAT IT IS DOING RIGHT NOW. Set before the call that can be slow and
	// cleared after, so a frame spent inside a resolve is attributable rather
	// than anonymous.
	FString GResolving;
	// Godot PROP_LIGHT_CAP: Forward+ clusters 512 lights in view and a lined
	// street passes that without anyone doing anything unreasonable. Unreal's
	// deferred path has no hard cluster cap but the cost is the same shape.
	int32 GPropLightCap    = 8;
	// THE MAP YOU HAVE OPEN IS THE SCOPE.
	//
	// A pf_portal_ prefab lives in the archives of the levels that USE the
	// object, so the current level's mount answers for a fraction of the
	// catalogue (Godot measured 14.8% against 81.2% with every level mounted).
	// This used to default ON: one name that did not resolve made the resolver
	// mount EVERY level in the game - measured at 20.4 seconds, on top of the
	// scope being wrong. A name found only in another map's archives is not
	// evidence that the object belongs on this one.
	//
	// So it is off. The current level's archives answer, a name that is not
	// there is reported as unresolved for this map, and somebody who genuinely
	// wants to search the whole install asks for it:
	//   BF6.HighPoly.Placed.FullCatalogue 1
	bool  GFullCatalogue   = false;
	// Try a named alias when nothing exact matches (see the last tier of
	// FindPrefab). BF6.HighPoly.Placed.Aliases 0 turns it off.
	bool  GNameAliases     = true;
	// A composite building can carry a thousand members. Past this many
	// triangles the rest of the members are dropped and the log says so.
	int32 GMaxMergedTris   = 3000000;

	// ---- what one placeable type resolved to ---------------------------------
	struct FTypeAsset
	{
		// AwaitingBuild: the members are decoded and merged and the type is
		// queued for the batch that describes, creates and commits across cores.
		// It is not Ready - nothing may be attached to a record yet - and it is
		// not Pending either, or the resolve would start again from the walk.
		enum class EState : uint8 { Pending, AwaitingBuild, Ready, Missing };
		EState        State = EState::Pending;
		// The merged geometry waiting for that batch, dropped the moment the
		// mesh exists so a scene does not hold two copies of itself.
		TArray<FCore::FSection> Merged;
		// What the merge found, kept for the line the finish stage logs.
		int32         PendingPlacedMembers = 0, PendingFailed = 0, PendingDropped = 0;
		int32         PendingCacheHits = 0, PendingCacheMisses = 0;
		int32         PendingMemberCount = 0;
		double        PendingStartedAt = 0.0;
		FString       Key;          // as the tag spelled it
		FString       Prefab;       // the candidate that resolved
		UStaticMesh*  Mesh = nullptr;   // rooted while Ready
		TArray<FCore::FLight> Lights;   // prefab-local, GAME space
		FBox          BoundsCm = FBox(ForceInit);
		int32         Tris = 0, Members = 0;
		// What the decode said, one line per merged section and one per member:
		// bindings by name, tint, roughness, flags, the scope they resolved in.
		// Kept so BF6.HighPoly.Placed.Inspect can answer without re-decoding.
		TArray<FString> Report;
		int32         TriedEpoch = -1;  // the mount/build epoch it last failed under
		// A miss taken AFTER the full catalogue mount is final for the session:
		// nothing more can be mounted, so no epoch can change the answer. Only
		// BF6.HighPoly.Placed.Rebuild clears it.
		bool          bNegativeFinal = false;
		bool          bCapLogged = false;
		bool          bShapeVeto = false;
		// How many PLACEMENTS the shape check turned away. The warning above
		// fires once per type, so without this a type that refused three
		// hundred objects reads exactly like one that refused one.
		int32         FitRefusals = 0;
		// Merged sections that bound no albedo sheet. These draw as flat pale
		// surfaces and read, from the viewport, exactly like an SDK blockout.
		int32         SectionsNoAlbedo = 0;
		// The light walk costs about as much as finding out there are none, so
		// it is only done when the lights are actually wanted. This says it has
		// been done, so switching them on reads once rather than every tick.
		bool          bLightsRead = false;
		// IT ASKED FOR THE READER AND THE READER WAS BUSY.
		//
		// Deferring is cheap, but retrying is not: a type that gets partway
		// through its members before it needs the reader throws that merge away
		// each time. Measured on the first try-lock build: 11,859 deferrals and
		// the merge phase up from 0.4 s to 12.3 s, all of it redone work.
		//
		// So a type that has proved it needs the reader stops asking until the
		// mount lands and the epoch turns over.
		bool          bNeedsReader = false;
		// AND NOT BEFORE THIS MOMENT.
		//
		// bNeedsReader alone only holds until the mount lands. After that the
		// reader is still taken from time to time - a light walk is most of a
		// second - and a type that needs it was asking again every single tick,
		// redoing its walk, its member decodes and its merge each time and
		// throwing all of it away. Measured: 1,071 retries costing 12.7 s, with
		// the merge phase back up from 0.5 s to 10.2 s.
		double        NextTryAt = 0.0;
		// Members that did not make it into the mesh: read failures and members
		// dropped past the triangle cap. Nonzero means READY BUT INCOMPLETE.
		// Gameplay logic with no art anywhere: a spawner, a trigger, an area.
		// Not a mapping failure, and counted separately so real ones stand out.
		bool          bGameplayMarker = false;
		bool          bVehicleBase = false; // typed base preview; attachments are not assembled yet
		int32         MembersFailed = 0;
		int32         MembersDropped = 0;
		// Resolved through a NAMED GUESS rather than an exact match, so the
		// shape check has to agree before it may replace anything.
		// ASKED ONCE PER TYPE, NOT ONCE PER PLACEMENT.
		//
		// The question "is everything this type needs already on disk" costs a
		// file read per member. Asked from ApplyRecord it was asked once per
		// RECORD - three and a half thousand times - and one tick spent ten and
		// a half seconds doing it. The answer belongs to the type.
		int8          CacheComplete = -1;   // -1 unknown, 0 no, 1 yes
		bool          bAliasGuess = false;
		bool          bAliasProven = false;
		FString       AliasFrom;
		FString       Why;
	};
	TMap<FString, FTypeAsset> GTypes;   // lower-case key

	// ---- one placed actor -----------------------------------------------------
	struct FRecord
	{
		TWeakObjectPtr<AActor>               Actor;
		FString                              Key, KeyLower;
		TWeakObjectPtr<UStaticMeshComponent> Mesh;
		TArray<TWeakObjectPtr<ULightComponent>> Lights;
		bool  bLightsBuilt     = false;
		bool  bProxyHiddenByUs = false;
		// We gave the SDK proxy a minimum draw distance so it takes over past the
		// cull line. Recorded so it can be put back to zero exactly where we set it.
		bool  bProxyRangedByUs = false;
		// Which half this object is currently wearing: 1 the real model, 0 the
		// SDK blockout, -1 not decided yet. Drives the distance swap so nothing
		// is toggled unless it actually crossed the line.
		int8  bNearShown = -1;
		bool  bSelected        = false;
		bool  bWantHigh        = false;
		// This ONE placement's proxy did not match the high mesh, so this one
		// keeps its blockout. Per placement, not per type: a duplicate somebody
		// scaled must not hide four hundred correct ones.
		bool  bFitRefused      = false;
		int32 MaterialMode     = -1;   // the mode the mesh's materials were last set for
	};
	TArray<FRecord> GRecords;

	// ---- bookkeeping ----------------------------------------------------------
	bool   GStarted = false;
	FTSTicker::FDelegateHandle GTicker;
	FDelegateHandle GSelHandle, GOpenedHandle, GClosingHandle, GPreExitHandle;
	double GLastPoll = 0.0;
	bool   GForcePoll = true;
	bool   GSelectionDirty = true;
	int32  GLastMode = -1;
	bool   GLastBuilding = false;
	// Bumped when the set of mounted archives can have changed (a full mount
	// finished, a build opened a level). Missing types are retried once per
	// epoch, never in a loop.
	int32  GEpoch = 0;
	int32  GLightNameCounter = 0;
	FString GCoreFailSaid;

	// The full-catalogue mount, on a worker. The context is not thread-safe, so
	// the shared busy flag keeps every other caller out while it runs.
	bool   GMountedAll     = false;
	// The last attempt to mount every level failed. Kept apart from GMountedAll
	// so a failure is never read as a completed search.
	bool   GMountFailed    = false;
	// The level whose own archives this module has mounted, and the request in
	// flight to mount one. Kept apart from the full-catalogue state: this is the
	// scope that SHOULD be used, that one is the deliberate widening.
	FString GLevelMountedFor;
	FString GLevelMountFor;      // the level the request in flight is for
	int32   GLevelMountTries   = 0;
	double  GLevelMountNextTry = 0.0;
	bool    GLevelMountRequested = false;
	double  GLevelMountStart = 0.0;
	TFuture<FString> GLevelMountFuture;
	bool   GMountRequested = false;
	double GMountStart     = 0.0;
	TFuture<FString> GMountFuture;

	// ---- the dll exports this module adds to the core's set -------------------
	typedef int         (*FnAssetInstances)(bf6_ctx*, const char*, bf6_instance*, int, char*, int);
	typedef int         (*FnAssetLights)(bf6_ctx*, const char*, bf6_light*, int, bf6_light_stats*, char*, int);
	typedef int         (*FnMountAll)(bf6_ctx*, int, char*, int);
	typedef int         (*FnLoadPlaceables)(bf6_ctx*, const char*, char*, int);
	typedef int         (*FnLevelCount)(bf6_ctx*);
	typedef const char* (*FnLevelName)(bf6_ctx*, int);
	typedef int         (*FnListEbx)(bf6_ctx*, const char*, bf6_asset*, int);
	FnAssetInstances GAssetInstances = nullptr;
	FnAssetLights    GAssetLights    = nullptr;
	FnMountAll       GMountAll       = nullptr;
	FnLoadPlaceables GLoadPlaceables = nullptr;
	FnLevelCount     GLevelCount     = nullptr;
	FnLevelName      GLevelName      = nullptr;
	FnListEbx        GListEbx        = nullptr;
	FnListEbx        GListRes        = nullptr;
	bool GFnsTried = false;

	// ---- the prefab NAME INDEX ------------------------------------------------
	//
	// THE NEGATIVE LOOKUP WAS THE WHOLE HITCH, and it is the negative one that
	// matters because most placed types are not props at all.
	//
	// bf6_asset_instances builds a NEW Walk with build_catalog() over every
	// mounted EBX name on EVERY call, so a candidate that resolves and a
	// candidate that does not cost exactly the same. FindPrefab tries up to 23
	// candidates - three direct plus ten art prefixes times two - and only a
	// type that HAS a prefab stops early, at the first one. A type with no
	// prefab pays for all 23, on the game thread, inside one editor tick.
	//
	// A game-mode build places CombatArea, PolygonVolume, Sector,
	// HQ_PlayerSpawner, SpawnPoint, VehicleSpawner, CapturePoint and their
	// siblings. None of them has a pf_portal_ prefab and none ever will, so the
	// editor paid the full 23 per type while the one placeable that did have a
	// prefab waited behind them in the same one-heavy-thing-per-tick queue.
	//
	// bf6_list_ebx reads the mount's NAME TABLES - no catalogue, no walk - so
	// the entire pf_portal_ namespace comes back in one call. After that a
	// candidate test is a TSet hash lookup and ONLY a candidate that actually
	// exists is allowed to cost a walk. Rebuilt once per epoch, because a mount
	// can add names and the epoch is exactly when that happened.
	TSet<FString> GPrefabLeafs;
	int32 GPrefabIndexEpoch = -1;
	bool  GPrefabIndexUsable = false;
	bool  GNoListEbxSaid = false;
	// Level leaf names, lower case, with and without "mp_". A placeable whose
	// bare name matches one would make Walk::run fall back to walking that
	// LEVEL (its bare-name rule anchors on /levels/<leaf>/<leaf>), which is
	// half a minute and thousands of rows for a candidate Godot's resolver
	// would simply have missed.
	TSet<FString> GLevelLeafs;
	bool GLevelsLoaded = false;

	bool EnsureFns()
	{
		if (GFnsTried) return GAssetInstances != nullptr;
		GFnsTried = true;
		void* Dll = BF6HP::Shared::CoreDll();
		if (!Dll) { GFnsTried = false; return false; }
		GAssetInstances = (FnAssetInstances) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_asset_instances"));
		GAssetLights    = (FnAssetLights)    FPlatformProcess::GetDllExport(Dll, TEXT("bf6_asset_lights"));
		GMountAll       = (FnMountAll)       FPlatformProcess::GetDllExport(Dll, TEXT("bf6_mount_all"));
		GLoadPlaceables = (FnLoadPlaceables) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_load_placeables"));
		GLevelCount     = (FnLevelCount)     FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_count"));
		GLevelName      = (FnLevelName)      FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_name"));
		GListEbx        = (FnListEbx)        FPlatformProcess::GetDllExport(Dll, TEXT("bf6_list_ebx"));
		GListRes        = (FnListEbx)        FPlatformProcess::GetDllExport(Dll, TEXT("bf6_list_res"));
		if (!GAssetInstances)
		{
			UE_LOG(LogBF6HighPolyPlaced, Warning,
				TEXT("placed: this bf6_core.dll has no bf6_asset_instances; placed objects keep the SDK proxy"));
		}
		return GAssetInstances != nullptr;
	}

	// False means the reader was busy and nothing was latched, so ask again.
	// This used to enter the context with no lock beside a mount on a worker.
	bool LoadLevelNames(bf6_ctx* Ctx)
	{
		if (GLevelsLoaded) return true;
		if (!GLoadPlaceables || !GLevelCount || !GLevelName) { GLevelsLoaded = true; return true; }
		BF6HP::Shared::FCoreTryLease Lease;
		if (!Lease.IsHeld()) { return false; }
		GLevelsLoaded = true;
		// The tool unpacks the SDK's FbExportData under its own saved dir.
		const FString Dir = FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("sdkdata"), TEXT("FbExportData"));
		char err[512] = {0};
		if (GLoadPlaceables(Ctx, TCHAR_TO_UTF8(*Dir), err, sizeof(err)) <= 0)
		{
			UE_LOG(LogBF6HighPolyPlaced, Log,
				TEXT("placed: level list unavailable (%hs); bare-name candidates are not guarded"), err);
			return true;
		}
		const int32 N = GLevelCount(Ctx);
		for (int32 i = 0; i < N; i++)
		{
			const char* Nm = GLevelName(Ctx, i);
			if (!Nm) continue;
			FString S = FString(UTF8_TO_TCHAR(Nm)).ToLower();
			GLevelLeafs.Add(S);
			if (S.StartsWith(TEXT("mp_"))) GLevelLeafs.Add(S.Mid(3));
		}
		return true;
	}

	bool CoreReady()
	{
		return BF6HP::Shared::Core().IsOpen() && !BF6HP::Shared::CoreBusy()
			&& (!BF6HP::Shared::IsBuilding() || GFinishingBuild);
	}

	// ---- tags -------------------------------------------------------------------
	FString TagValue(const AActor* A, const TCHAR* Prefix)
	{
		const int32 L = FCString::Strlen(Prefix);
		for (const FName& T : A->Tags)
		{
			const FString S = T.ToString();
			if (S.StartsWith(Prefix)) return S.Mid(L);
		}
		return FString();
	}

	// The placeable's name is the key, as in Godot (the scene's basename). The
	// mesh tag is the SDK model, which usually shares the name and is the
	// fallback for objects the tool spawned by mesh alone.
	FString KeyOf(const AActor* A)
	{
		FString K = TagValue(A, TEXT("label:"));
		if (K.IsEmpty()) K = TagValue(A, TEXT("type:"));
		if (K.IsEmpty()) K = TagValue(A, TEXT("mesh:"));
		return K;
	}

	UPrimitiveComponent* ProxyOf(AActor* A)
	{
		UPrimitiveComponent* P = Cast<UPrimitiveComponent>(A->GetRootComponent());
		if (!P || P->ComponentTags.Contains(kOurComponentTag)) return nullptr;
		return P;
	}

	// ---- the prefab walk --------------------------------------------------------
	bool WalkCandidate(bf6_ctx* Ctx, const FString& Cand, TArray<bf6_instance>& Rows)
	{
		char err[512] = {0};
		const int n = GAssetInstances(Ctx, TCHAR_TO_UTF8(*Cand), nullptr, 0, err, sizeof(err));
		if (n <= 0) return false;
		Rows.SetNumZeroed(n);
		const int got = GAssetInstances(Ctx, TCHAR_TO_UTF8(*Cand), Rows.GetData(), n, err, sizeof(err));
		Rows.SetNum(FMath::Clamp(got, 0, n), EAllowShrinking::No);
		return Rows.Num() > 0;
	}

	// The leaf of an asset name, lower case, without the .ebx suffix.
	FString LeafOf(const char* Utf8)
	{
		FString N = UTF8_TO_TCHAR(Utf8);
		if (N.EndsWith(TEXT(".ebx"), ESearchCase::IgnoreCase)) N.LeftChopInline(4, EAllowShrinking::No);
		int32 Slash = INDEX_NONE;
		if (N.FindLastChar(TEXT('/'), Slash)) N.RightChopInline(Slash + 1, EAllowShrinking::No);
		return N.ToLower();
	}

	// Every pf_portal_ name the mount carries, once per epoch. One name-table
	// pass, no Walk, no catalogue build.
	bool EnsurePrefabIndex(bf6_ctx* Ctx)
	{
		if (GPrefabIndexUsable && GPrefabIndexEpoch == GEpoch) return true;
		if (!GListEbx)
		{
			if (!GNoListEbxSaid)
			{
				GNoListEbxSaid = true;
				UE_LOG(LogBF6HighPolyPlaced, Warning,
					TEXT("placed: this bf6_core.dll has no bf6_list_ebx; every prefab candidate has to be ")
					TEXT("walked, which is seconds per candidate. Update the dll to remove the resolve hitch."));
			}
			return false;
		}
		const double T0 = FPlatformTime::Seconds();
		const int n = GListEbx(Ctx, kPortalPrefixUtf8, nullptr, 0);
		GPrefabLeafs.Reset();
		if (n > 0)
		{
			TArray<bf6_asset> Rows;
			Rows.SetNumZeroed(n);
			const int got = FMath::Clamp(GListEbx(Ctx, kPortalPrefixUtf8, Rows.GetData(), n), 0, n);
			GPrefabLeafs.Reserve(got);
			for (int32 i = 0; i < got; i++)
				if (Rows[i].name) GPrefabLeafs.Add(LeafOf(Rows[i].name));
		}
		GPrefabIndexUsable = true;
		GPrefabIndexEpoch = GEpoch;
		UE_LOG(LogBF6HighPolyPlaced, Log,
			TEXT("placed: prefab index built: %d pf_portal_ name(s) in %.0f ms; a type with no prefab is now a hash lookup"),
			GPrefabLeafs.Num(), (FPlatformTime::Seconds() - T0) * 1000.0);
		return true;
	}

	// Does the mount carry an EBX whose LEAF is exactly this name? For the
	// pf_portal_ namespace the index answers; for the bare-name candidate it is
	// one name-table scan, which is still not a Walk.
	// SOME THINGS HAVE NO MODEL BECAUSE THEY ARE NOT OBJECTS.
	//
	// A spawner, a trigger volume, a combat area and a deploy camera are
	// gameplay logic. The game has no art for them and never will, so reporting
	// them as "needs mapping" alongside a wall that genuinely failed to resolve
	// buries the real failures in noise. Measured on one real scene: 141 of the
	// 141 objects that did not resolve were these, bar a single vehicle.
	//
	// This only changes what the status SAYS. Such a type still keeps its SDK
	// proxy, which is the correct thing to draw for it.
	bool IsGameplayMarker(const FString& Key)
	{
		const FString K = Key.ToLower();
		static const TCHAR* kEndings[] = {
			TEXT("spawner"), TEXT("spawnpoint"), TEXT("trigger"), TEXT("volume"),
			TEXT("combatarea"), TEXT("deploycam"), TEXT("capturepoint"), TEXT("objective")
		};
		for (const TCHAR* E : kEndings) { if (K.EndsWith(E)) { return true; } }
		// A few carry the word in the middle rather than at the end.
		return K.StartsWith(TEXT("areatrigger")) || K.StartsWith(TEXT("polygonvolume"))
			|| K.Contains(TEXT("_spawner"));
	}

	bool NameExists(bf6_ctx* Ctx, const FString& Cand, bool bIndexUsable)
	{
		if (!bIndexUsable) return true;   // no index: let the walk decide, as before
		if (Cand.StartsWith(kPortalPrefix)) return GPrefabLeafs.Contains(Cand);
		const FTCHARToUTF8 CandU(*Cand);
		const int n = GListEbx(Ctx, CandU.Get(), nullptr, 0);
		// A substring that matches an implausible number of names is not worth
		// materialising; let the walk answer that one rather than allocate.
		if (n <= 0) return false;
		if (n > 200000) return true;
		TArray<bf6_asset> Rows;
		Rows.SetNumZeroed(n);
		const int got = FMath::Clamp(GListEbx(Ctx, CandU.Get(), Rows.GetData(), n), 0, n);
		for (int32 i = 0; i < got; i++)
			if (Rows[i].name && LeafOf(Rows[i].name) == Cand) return true;
		return false;
	}

	// Port of highpoly_gamesource._resolve_object + _resolve_prefixed:
	// pf_portal_<name>, the bare name, pf_portal_<name>_a, then one art
	// category and only if exactly one answers. Surveyed there over 4,620 SDK
	// objects: 4,290 direct, 221 only as _a, 16 more with a category prefix.
	//
	// The candidate ORDER and the "exactly one category" rule are unchanged.
	// What changed is that a candidate is only WALKED once the name index says
	// it exists, so the 22 misses a prefab-less type used to pay for are 22 hash
	// lookups.
	bool FindPrefab(bf6_ctx* Ctx, const FString& Key, FString& OutName, TArray<bf6_instance>& OutRows,
	                bool& OutAlias)
	{
		const bool bIndexed = EnsurePrefabIndex(Ctx);
		const FString K = Key.ToLower();
		TArray<FString> Direct;
		Direct.Add(kPortalPrefix + K);
		if (!GLevelLeafs.Contains(K) && !GLevelLeafs.Contains(TEXT("mp_") + K)) Direct.Add(K);
		Direct.Add(kPortalPrefix + K + TEXT("_a"));
		for (const FString& C : Direct)
		{
			if (!NameExists(Ctx, C, bIndexed)) continue;
			if (WalkCandidate(Ctx, C, OutRows)) { OutName = C; return true; }
		}
		TArray<FString> Hits;
		for (const TCHAR* Pre : kArtPrefixes)
		{
			for (const FString& C : { FString(kPortalPrefix) + Pre + K, FString(kPortalPrefix) + Pre + K + TEXT("_a") })
			{
				if (!NameExists(Ctx, C, bIndexed)) continue;
				TArray<bf6_instance> Tmp;
				if (WalkCandidate(Ctx, C, Tmp)) { Hits.Add(C); break; }
			}
		}
		if (Hits.Num() == 1)
		{
			OutName = Hits[0];
			return WalkCandidate(Ctx, OutName, OutRows);
		}
		if (Hits.Num() > 1) { return false; }   // ambiguous: refused, as before

		// ---- the last tier: a NAMED ALIAS, never a silent one --------------
		//
		// Measured on a real scene: 399 objects across three types stayed on
		// their blockouts because the SDK spells them PlasterWall_01_128_B
		// while the game archives hold pf_portal_plasterwall_01_128 and
		// pf_portal_br_plasterwall_01_128. Two things were missing: the br_
		// prefix, and any handling of a trailing single-letter variant suffix.
		//
		// This is a GUESS and is treated as one. It is tried only after every
		// exact route has failed, it is recorded on the type, and the shape
		// check at attach time has to agree before it is allowed to replace
		// anything. A wrong guess keeps the blockout and says so, rather than
		// standing a different wall where the author put theirs.
		// Measured against MP_Aftermath's own archives, which hold exactly:
		//   pf_portal_plasterwall_01_128      and  pf_portal_br_plasterwall_01_128
		//   pf_portal_plasterwall_01_1024     and no br_ variant
		//   pf_portal_br_plasterpillar_01_144 and no plain variant
		// against SDK keys PlasterWall_01_128_B, PlasterWall_01_1024_B and
		// PlasterPillar_01_144. So two independent things are needed: dropping
		// a trailing variant letter, and the br_ prefix, in either combination.
		// The unprefixed name is preferred where both exist.
		if (!GNameAliases) { return false; }
		FString Base = K;
		const int32 Last = Base.Len() >= 2 ? Base.Len() - 2 : -1;
		const bool bLetterSuffix = Last > 0 && Base[Last] == TEXT('_')
			&& FChar::IsAlpha(Base[Base.Len() - 1]);
		if (bLetterSuffix) { Base = Base.Left(Last); }

		TArray<FString> Alias;
		if (bLetterSuffix)
		{
			Alias.Add(kPortalPrefix + Base);
			Alias.Add(FString(kPortalPrefix) + TEXT("br_") + Base);
		}
		// The br_ prefix on the key as it stands, for a name with no variant
		// letter at all - PlasterPillar_01_144 has only a br_ prefab.
		Alias.Add(FString(kPortalPrefix) + TEXT("br_") + K);
		if (bLetterSuffix)
		{
			for (const TCHAR* Pre : kArtPrefixes) { Alias.Add(FString(kPortalPrefix) + Pre + Base); }
		}
		for (const TCHAR* Pre : kArtPrefixes)
		{
			Alias.Add(FString(kPortalPrefix) + TEXT("br_") + Pre + K);
			if (bLetterSuffix) { Alias.Add(FString(kPortalPrefix) + TEXT("br_") + Pre + Base); }
		}
		for (const FString& C : Alias)
		{
			if (!NameExists(Ctx, C, bIndexed)) { continue; }
			if (!WalkCandidate(Ctx, C, OutRows)) { continue; }
			OutName = C;
			OutAlias = true;
			UE_LOG(LogBF6HighPolyPlaced, Log,
				TEXT("placed: %s has no prefab of its own; trying %s as an alias. ")
				TEXT("It is only used if the shapes agree."), *Key, *C);
			return true;
		}
		return false;
	}

	// One key per material identity so members that share a material share a
	// slot. MaterialFor caches by the same facts, so this costs nothing extra
	// and keeps a thousand-member composite from becoming a thousand sections.
	FString MaterialKeyOf(const FCore::FSection& S)
	{
		FString K = FString::Printf(TEXT("%d%d%d%d%d%d|%.4f %.4f %.4f|%.3f"),
			S.bAlphaTest ? 1 : 0, S.bTranslucent ? 1 : 0, S.bAlphaFromAlbedo ? 1 : 0,
			S.bNsm ? 1 : 0, S.bDecal ? 1 : 0, S.bTerrainDecalReceiver ? 1 : 0,
			S.BaseColor.R, S.BaseColor.G, S.BaseColor.B, S.Roughness);
		for (const FCore::FBinding& B : S.Textures)
			K += FString::Printf(TEXT("|%d:%d"), B.Slot, B.Texture);
		return K;
	}

	// Append one member's sections to the merged set, transformed by its
	// prefab-local 3x4 (rows: right, up, forward, origin; GAME space). The
	// merged sections stay in game space; DescribeMesh does the Y/Z swap and
	// the winding flip, so a member placed here is placed exactly as the level
	// build places the same mesh. A mirrored member (negative determinant, the
	// game legitimately mirror-instances props) has its winding reversed here
	// so the flip downstream lands it the right way out.
	void AppendMember(TArray<FCore::FSection>& Merged, TMap<FString, int32>& SlotByKey,
	                  const TArray<FCore::FSection>& Src, const float* X)
	{
		const FVector3f R(X[0], X[1], X[2]), U(X[3], X[4], X[5]), F(X[6], X[7], X[8]), O(X[9], X[10], X[11]);
		const bool bMirror = FVector3f::DotProduct(R, FVector3f::CrossProduct(U, F)) < 0.f;

		// NORMALS DO NOT TRANSFORM LIKE POSITIONS.
		//
		// A normal transformed by the vertex basis stays perpendicular to the
		// surface only while that basis scales every axis equally. Squash a
		// member on one axis - which prefabs do, constantly, to make one wall
		// panel serve at several thicknesses - and every normal leans the wrong
		// way, so the piece lights differently from the identical piece next to
		// it that happened to be unscaled.
		//
		// The correct basis for a normal is the inverse transpose of the linear
		// part. Equal scaling makes it the same matrix again, which is why this
		// was invisible until it was not.
		const FMatrix44f Linear(
			FPlane4f(R.X, R.Y, R.Z, 0.f), FPlane4f(U.X, U.Y, U.Z, 0.f),
			FPlane4f(F.X, F.Y, F.Z, 0.f), FPlane4f(0.f, 0.f, 0.f, 1.f));
		// A degenerate member (a zeroed axis) has no inverse; falling back to
		// the vertex basis keeps the old behaviour rather than producing NaNs.
		const bool bInvertible = FMath::Abs(Linear.Determinant()) > 1.e-8f;
		const FMatrix44f NrmBasis = bInvertible ? Linear.Inverse().GetTransposed() : Linear;
		const FVector3f NR(NrmBasis.M[0][0], NrmBasis.M[0][1], NrmBasis.M[0][2]);
		const FVector3f NU(NrmBasis.M[1][0], NrmBasis.M[1][1], NrmBasis.M[1][2]);
		const FVector3f NF(NrmBasis.M[2][0], NrmBasis.M[2][1], NrmBasis.M[2][2]);
		for (const FCore::FSection& S : Src)
		{
			if (S.Pos.Num() == 0 || S.Idx.Num() < 3) continue;
			const FString K = MaterialKeyOf(S);
			int32 Slot;
			if (const int32* Found = SlotByKey.Find(K)) Slot = *Found;
			else
			{
				Slot = Merged.AddDefaulted();
				FCore::FSection& N = Merged[Slot];
				N.Textures = S.Textures;
				N.bAlphaTest = S.bAlphaTest; N.bTranslucent = S.bTranslucent;
				N.bAlphaFromAlbedo = S.bAlphaFromAlbedo; N.BaseColor = S.BaseColor;
				N.Roughness = S.Roughness; N.bNsm = S.bNsm; N.bDecal = S.bDecal;
				N.bTerrainDecalReceiver = S.bTerrainDecalReceiver;
				SlotByKey.Add(K, Slot);
			}
			FCore::FSection& D = Merged[Slot];
			const int32 Base = D.Pos.Num();
			const bool bHasN = S.Nrm.Num() == S.Pos.Num();
			const bool bHasUV = S.UV.Num() == S.Pos.Num();
			D.Pos.Reserve(Base + S.Pos.Num());
			D.Nrm.Reserve(Base + S.Pos.Num());
			D.UV.Reserve(Base + S.Pos.Num());
			for (int32 v = 0; v < S.Pos.Num(); v++)
			{
				const FVector3f& P = S.Pos[v];
				D.Pos.Add(O + R * P.X + U * P.Y + F * P.Z);
				if (bHasN)
				{
					const FVector3f& Nn = S.Nrm[v];
					D.Nrm.Add((NR * Nn.X + NU * Nn.Y + NF * Nn.Z).GetSafeNormal());
				}
				else
				{
					// Zero asks DescribeMesh's tangent pass to derive one.
					D.Nrm.Add(FVector3f::ZeroVector);
				}
				D.UV.Add(bHasUV ? S.UV[v] : FVector2f::ZeroVector);
			}
			D.Idx.Reserve(D.Idx.Num() + S.Idx.Num());
			for (int32 i = 0; i + 2 < S.Idx.Num(); i += 3)
			{
				const uint32 a = S.Idx[i] + Base, b = S.Idx[i + 1] + Base, c = S.Idx[i + 2] + Base;
				if (bMirror) { D.Idx.Add(a); D.Idx.Add(c); D.Idx.Add(b); }
				else         { D.Idx.Add(a); D.Idx.Add(b); D.Idx.Add(c); }
			}
		}
	}

	// The same field-for-field conversion FCore::ReadLights makes, for the
	// per-asset call the core exposes beside it.
	void ReadAssetLights(bf6_ctx* Ctx, const FString& Prefab, TArray<FCore::FLight>& Out)
	{
		Out.Reset();
		if (!GAssetLights) return;
		bf6_light_stats st{};
		char err[512] = {0};
		const int n = GAssetLights(Ctx, TCHAR_TO_UTF8(*Prefab), nullptr, 0, &st, err, sizeof(err));
		if (n <= 0) return;
		TArray<bf6_light> Raw;
		Raw.SetNumZeroed(n);
		const int got = GAssetLights(Ctx, TCHAR_TO_UTF8(*Prefab), Raw.GetData(), n, &st, err, sizeof(err));
		for (int32 i = 0; i < got && i < Raw.Num(); i++)
		{
			const bf6_light& L = Raw[i];
			FCore::FLight F;
			F.Type    = L.type;
			F.Right   = FVector(L.xform[0], L.xform[1], L.xform[2]);
			F.Up      = FVector(L.xform[3], L.xform[4], L.xform[5]);
			F.Forward = FVector(L.xform[6], L.xform[7], L.xform[8]);
			F.Origin  = FVector(L.xform[9], L.xform[10], L.xform[11]);
			F.Color   = FLinearColor(L.color[0], L.color[1], L.color[2]);
			F.Intensity = L.intensity;
			F.Unit = L.unit;
			F.Dimmer = L.dimmer;
			F.AttenuationRadiusM = L.attenuation_radius;
			F.InnerAngleDeg = L.inner_angle;
			F.OuterAngleDeg = L.outer_angle;
			F.ShapeRadiusM = L.shape_radius;
			F.TubeWidthM = L.tube_width;
			F.RectHeightM = L.rect_height;
			F.RectAspect = L.rect_aspect;
			F.bCastShadows = L.cast_shadows_enable != 0;
			F.Source = L.source ? UTF8_TO_TCHAR(L.source) : TEXT("");
			F.Flags = L.flags;
			Out.Add(MoveTemp(F));
		}
	}

	// THIS MAP'S OWN ARCHIVES, WHICH NOBODY WAS MOUNTING.
	//
	// The resolver read through whatever the shared core happened to have
	// mounted, which is this map's archives only if the scenery build has
	// already run. Open a saved experience and go straight to Textured without
	// pressing BUILD and the answer for every single type was "not in this
	// map's archives" - in 21 milliseconds, because there was nothing to look
	// in. The old code hid that by falling back to mounting every level in the
	// game, which is 20 seconds and the wrong scope.
	//
	// Measured with the reader on its own: opening the install is 1.1 s and
	// mounting one level is 5.4 s, after which the whole scene resolves. So it
	// is asked for once, on a worker, exactly like the full mount.
	void RequestLevelMount()
	{
		const FString Level = BF6Ext::CurrentLevel();
		if (Level.IsEmpty()) { return; }
		if (GLevelMountedFor == Level || GLevelMountRequested) { return; }
		// NOT EVERY TICK. Observed live: the map load is reading through the
		// same context while this asks, the mount loses that race and returns
		// "the level could not be mounted", and asking again a frame later
		// loses it again - ten times in fifteen seconds, each one a second of
		// worker time and a line of log. It backs off instead, and the wait
		// grows, so a genuinely broken mount is quiet rather than a stream.
		if (FPlatformTime::Seconds() < GLevelMountNextTry) { return; }
		if (BF6HP::Shared::CoreShuttingDown()) { return; }
		if (!CoreReady()) { return; }
		if (!BF6HP::Shared::CoreContext()) { return; }

		GLevelMountRequested = true;
		// THE LEVEL THIS REQUEST IS FOR, remembered now. Read again when the
		// worker lands it can be empty for a frame or already be the next map,
		// and then the mount is recorded against the wrong name - or against no
		// name, which starts the whole thing again on the next tick. Observed
		// live: eleven mounts requested in fifteen seconds.
		GLevelMountFor = Level;
		GLevelMountStart = FPlatformTime::Seconds();
		BF6HP::Shared::SetCoreBusy(true);
		UE_LOG(LogBF6HighPolyPlaced, Log,
			TEXT("placed: mounting %s's own archives so its objects can be read (worker thread)"), *Level);
		const FString Exe = BF6HP::Shared::InstallExePath();
		GLevelMountFuture = Async(EAsyncExecution::Thread, [Level, Exe]() -> FString
		{
			// Nobody else inside the context while this mounts. The UI sound
			// decoder was, and that was the crash.
			FScopeLock Lock(&BF6HP::Shared::CoreMutex());
			FCore& Core = BF6HP::Shared::Core();
			// ARCHIVES, NOT A WALK.
			//
			// This called OpenLevel, which traverses the whole level - measured
			// at 26.0 s on MP_Aftermath - to reach something this module never
			// asks for. It reads prefabs BY NAME; the SDK scene already says
			// where they go. bf6_mount_level_archives is the core's own door for
			// that, and it explicitly does not walk or decode the level.
			//
			// OpenLevel stays as the fallback for an install whose bf6_core.dll
			// predates that export, so an older reader still works.
			if (Core.MountLevelArchives(Level)) { return FString(); }
			UE_LOG(LogBF6HighPolyPlaced, Log,
				TEXT("placed: narrow archive mount unavailable (%s); walking the level instead"),
				*Core.Error);
			if (Core.OpenLevel(Level, Exe)) { return FString(); }
			return Core.Error.IsEmpty()
				? FString(TEXT("the level could not be mounted")) : Core.Error;
		});
	}

	void PollLevelMount()
	{
		if (!GLevelMountRequested || !GLevelMountFuture.IsReady()) { return; }
		const FString Err = GLevelMountFuture.Get();
		GLevelMountRequested = false;
		BF6HP::Shared::SetCoreBusy(false);
		const FString Level = GLevelMountFor;
		if (Err.IsEmpty())
		{
			GLevelMountedFor = Level;
			GLevelMountTries = 0;
			GLevelMountNextTry = 0.0;
			GEpoch++;               // the name tables just grew: retry the misses
			GForcePoll = true;
			// The reader is free and full. Everything that stood aside for it
			// may ask again.
			for (TPair<FString, FTypeAsset>& P : GTypes) { P.Value.bNeedsReader = false; }
			UE_LOG(LogBF6HighPolyPlaced, Log,
				TEXT("placed: %s mounted in %.1fs; its objects can be read now"),
				*Level, FPlatformTime::Seconds() - GLevelMountStart);
		}
		else
		{
			// Not remembered as mounted, so it will be tried again rather than
			// leaving every object unresolved for the session with no reason -
			// but later each time, and quietly after the first, because the
			// commonest cause is simply that the map is still loading through
			// the same reader.
			GLevelMountTries++;
			const double Wait = FMath::Min(30.0, 2.0 * GLevelMountTries);
			GLevelMountNextTry = FPlatformTime::Seconds() + Wait;
			const ELogVerbosity::Type How = GLevelMountTries <= 1
				? ELogVerbosity::Warning : ELogVerbosity::Verbose;
			if (How == ELogVerbosity::Warning)
			{
				UE_LOG(LogBF6HighPolyPlaced, Warning,
					TEXT("placed: %s could not be mounted after %.1fs: %s. Trying again in %.0fs."),
					*Level, FPlatformTime::Seconds() - GLevelMountStart, *Err, Wait);
			}
			else
			{
				UE_LOG(LogBF6HighPolyPlaced, Verbose,
					TEXT("placed: %s mount attempt %d failed (%s); next try in %.0fs"),
					*Level, GLevelMountTries, *Err, Wait);
			}
		}
	}

	void RequestFullMount()
	{
		if (GMountedAll || GMountRequested || !GFullCatalogue || !GMountAll) return;
		// Never start core work once the module has begun going down: JoinCoreWorkers
		// waits for the mount below, and a wait it can lose a race with is no wait.
		if (BF6HP::Shared::CoreShuttingDown()) return;
		if (!CoreReady()) return;
		bf6_ctx* Ctx = BF6HP::Shared::CoreContext();
		if (!Ctx) return;
		GMountRequested = true;
		GMountStart = FPlatformTime::Seconds();
		BF6HP::Shared::SetCoreBusy(true);
		BF6Ext::Notify(TEXT("High Poly: reading the full object catalogue once. Placed objects follow when it is done."));
		UE_LOG(LogBF6HighPolyPlaced, Log,
			TEXT("placed: a prefab was not in the mounted archives; mounting every level's archives once (worker thread)"));
		GMountFuture = Async(EAsyncExecution::Thread, [Ctx]() -> FString
		{
			char err[512] = {0};
			const int ok = GMountAll(Ctx, 1, err, sizeof(err));
			return ok ? FString() : FString(UTF8_TO_TCHAR(err));
		});
	}

	// SERVICING A FINISHED MOUNT IS NOT A DISPLAY CONCERN.
	//
	// RequestFullMount takes the shared core lease with SetCoreBusy(true) and
	// this is the only place that gives it back. Tick used to return on
	// !GEnabled before calling it, so turning placed High Poly off while the
	// catalogue was mounting stranded the lease: the mount finished, nobody
	// collected it, and every later full build refused with "the object
	// catalogue is still being read" for the rest of the session. Tick now polls
	// before any display gate; see the note there.
	void PollMount()
	{
		if (!GMountRequested || !GMountFuture.IsReady()) return;
		const FString Err = GMountFuture.Get();
		GMountRequested = false;
		BF6HP::Shared::SetCoreBusy(false);
		GEpoch++;
		if (Err.IsEmpty())
		{
			// A MOUNT THAT FAILED IS NOT A MOUNT.
			//
			// This was set whatever happened, on the reasoning that asking
			// again would fail the same way. Two things followed from that: the
			// status said the catalogue was "full" when nothing had been
			// mounted, and every name that could not be found afterwards was
			// marked FINAL - permanently missing, on the strength of a search
			// that never happened.
			GMountedAll = true;
			GMountFailed = false;
			UE_LOG(LogBF6HighPolyPlaced, Log, TEXT("placed: full catalogue mounted in %.1fs; retrying %d pending type(s)"),
				FPlatformTime::Seconds() - GMountStart, GTypes.Num());
			BF6Ext::Notify(TEXT("High Poly: object catalogue ready."));
		}
		else
		{
			GMountFailed = true;
			UE_LOG(LogBF6HighPolyPlaced, Warning,
				TEXT("placed: full catalogue mount FAILED after %.1fs: %s. Nothing extra was mounted, ")
				TEXT("so a name that is not in this map's archives is still just unresolved here."),
				FPlatformTime::Seconds() - GMountStart, *Err);
			BF6Ext::Notify(FString::Printf(TEXT("High Poly: catalogue mount failed: %s"), *Err));
		}
	}

	// One line that says what a section will draw with: every binding by slot
	// and sheet name, the record tint, roughness and the kind flags. This is
	// the placed path's half of a material comparison; the level path's half
	// is the same record read with the level's placing bundle.
	FString SectionReport(FCore& Core, const FCore::FSection& S, int32 Index)
	{
		FString Line = FString::Printf(TEXT("sec%d %d tris%s%s%s%s%s tint=(%.3f %.3f %.3f) rough=%.2f"),
			Index, S.Idx.Num() / 3,
			S.bTranslucent ? TEXT(" translucent") : TEXT(""),
			S.bAlphaTest ? TEXT(" alpha-test") : TEXT(""),
			S.bAlphaFromAlbedo ? TEXT(" cutout-from-albedo") : TEXT(""),
			S.bDecal ? TEXT(" decal") : TEXT(""),
			S.bNsm ? TEXT(" nsm") : TEXT(""),
			S.BaseColor.R, S.BaseColor.G, S.BaseColor.B, S.Roughness);
		if (S.Textures.Num() == 0) Line += TEXT(" NO SHEETS");
		// A GLASS SECTION IS SUPPOSED TO LOOK LIKE THIS, and this line is where
		// you find out whether it can. Verified over 1,952 records on five
		// levels: a glass record binds no base colour, no normal and no opacity
		// sheet, so NO SHEETS on a translucent section is correct data, not a
		// resolution failure. What it does carry - the pane tint, the authored
		// opacity, the smoothness - has no field in bf6_material_desc, so it
		// arrives white here and the pane falls back to the measured shipped
		// tint. White plus NO SHEETS therefore means "the dll told us nothing
		// about this pane", and a non-white tint means a record was read.
		if (S.bTranslucent && S.Textures.Num() == 0
			&& S.BaseColor.Equals(FLinearColor::White, KINDA_SMALL_NUMBER))
		{
			Line += TEXT(" (glass: no record values from this dll; measured shipped tint)");
		}
		for (const FCore::FBinding& B : S.Textures)
		{
			static const TCHAR* SlotNames[] = { TEXT("albedo"), TEXT("normal"), TEXT("wo"), TEXT("emissive"), TEXT("mask") };
			const TCHAR* SlotName = (B.Slot >= 0 && B.Slot < 5) ? SlotNames[B.Slot] : TEXT("slot?");
			const FString Name = B.Texture >= 0 ? Core.TextureNameAt(B.Texture) : FString();
			Line += FString::Printf(TEXT(" %s=%s"), SlotName,
				Name.IsEmpty() ? *FString::Printf(TEXT("#%d"), B.Texture) : *FPaths::GetBaseFilename(Name));
		}
		return Line;
	}
	// ---- REMEMBERING THE WALK ------------------------------------------------
	//
	// What FindPrefab produces for a type: the prefab it settled on, whether
	// that was a guess, and the members with their transforms. All of it is a
	// pure function of the installed game, so it is the same on every open, and
	// it costs about six tenths of a second to work out again each time.
	//
	// Stored against the same install signature the rest of the disk cache uses,
	// so a game update invalidates it along with everything else. The version
	// below is bumped when the candidate order changes, because a cached answer
	// from an older rule is not the answer the current rule would give.
	constexpr uint32 kWalkCacheVer = 3;

	FString WalkCacheName(const FString& Key)
	{
		return TEXT("placedwalk_") + FMD5::HashAnsiString(*Key.ToLower()).Left(24);
	}

	// bf6_instance carries raw pointers into the reader's own memory, so what is
	// stored is the copy the caller needs rather than the struct itself.
	struct FWalkMember
	{
		FString Res, Bundle, Variation;
		float   X[12] = {};
	};

	// Playable vehicles share a gameplay spawner instead of a prop prefab.
	// Match the SDK identity to an exact, unique vehicle art directory already
	// mounted for this level. No prefix/fuzzy match and no extra archive mount.
	bool FindVehicleBase(bf6_ctx* Ctx, const FString& Key, FString& OutSource,
		TArray<FWalkMember>& OutMembers, FString& OutWhy)
	{
		if (!GListRes)
		{
			OutWhy = TEXT("the reader cannot enumerate vehicle resources");
			return false;
		}
		TArray<FString> Allowed;
		BF6Ext::PlaceableTypes(Allowed);
		if (BF6Ext::CurrentLevel().IsEmpty() || !Allowed.ContainsByPredicate(
			[&Key](const FString& S) { return S.Equals(Key, ESearchCase::IgnoreCase); }))
		{
			OutWhy = TEXT("the SDK does not list this vehicle for the open map");
			return false;
		}
		const char* Prefix = "common/hardware/vehicles/";
		const int32 Count = GListRes(Ctx, Prefix, nullptr, 0);
		if (Count < 0 || Count > 1000000)
		{
			OutWhy = TEXT("the reader returned an invalid vehicle resource count");
			return false;
		}
		TArray<bf6_asset> Assets;
		Assets.SetNumZeroed(Count);
		const int32 Got = FMath::Clamp(GListRes(Ctx, Prefix, Assets.GetData(), Count), 0, Count);
		FString Want = Key.Mid(4).ToLower().Replace(TEXT("_"), TEXT(""));
		TArray<FString> Matches;
		for (int32 i = 0; i < Got; ++i)
		{
			if (!Assets[i].name) continue;
			const FString Name = UTF8_TO_TCHAR(Assets[i].name);
			TArray<FString> Parts;
			Name.ParseIntoArray(Parts, TEXT("/"));
			if (Parts.Num() != 7 || Parts[5] != TEXT("art")) continue;
			if (Parts[4].Replace(TEXT("_"), TEXT("")) != Want) continue;
			const FString BaseName = TEXT("ob_veh_") + Parts[3] + TEXT("_") + Parts[4] + TEXT("_base_mesh");
			if (Parts[6] == BaseName) Matches.AddUnique(Name);
		}
		if (Matches.Num() != 1)
		{
			OutWhy = Matches.IsEmpty()
				? TEXT("no exact vehicle base match in the mounted map resources; SDK preview retained")
				: TEXT("multiple vehicle base matches; SDK preview retained rather than guessing");
			return false;
		}
		FWalkMember Member;
		Member.Res = Matches[0];
		// The model-definition bundle owns the base appearance, whereas the
		// resource's own bundle can carry geometry with no material records.
		// Only use this exact base scope if the mounted reader confirms it.
		// Cosmetic variation bundles and similarly named vehicles are excluded.
		TArray<FString> Parts;
		Member.Res.ParseIntoArray(Parts, TEXT("/"));
		const FString BaseScope = TEXT("md_veh_") + Parts[3] + TEXT("_") + Parts[4] + TEXT("_bundle_3p");
		using FnScopeExists = int (*)(bf6_ctx*, const char*);
		const FnScopeExists ScopeExists = (FnScopeExists)FPlatformProcess::GetDllExport(
			BF6HP::Shared::CoreDll(), TEXT("bf6_material_scope_exists"));
		if (ScopeExists && ScopeExists(Ctx, TCHAR_TO_UTF8(*BaseScope)) == 1) Member.Bundle = BaseScope;
		else UE_LOG(LogBF6HighPolyPlaced, Warning,
			TEXT("placed: %s base material scope %s is unavailable; using the resource's fallback material"), *Key, *BaseScope);
		Member.X[0] = Member.X[4] = Member.X[8] = 1.f;
		OutMembers.Add(MoveTemp(Member));
		OutSource = TEXT("vehicle-base:") + Matches[0];
		return true;
	}

	// The fixtures a prefab carries, stored with its walk. Reading them costs
	// 0.6 s a type and 103 of the 219 seconds a cold scene takes - nearly half,
	// spent almost entirely on discovering that a wall has no lights.
	void SerialiseLights(FArchive& Ar, TArray<FCore::FLight>& L)
	{
		int32 N = L.Num();
		Ar << N;
		if (Ar.IsLoading())
		{
			if (Ar.IsError() || N < 0 || N > 4096) { Ar.SetError(); return; }
			L.SetNum(N);
		}
		for (int32 i = 0; i < N && !Ar.IsError(); i++)
		{
			FCore::FLight& X = L[i];
			Ar << X.Type << X.Right << X.Up << X.Forward << X.Origin << X.Color
			   << X.Intensity << X.Unit << X.Dimmer << X.AttenuationRadiusM
			   << X.InnerAngleDeg << X.OuterAngleDeg << X.ShapeRadiusM << X.TubeWidthM
			   << X.RectHeightM << X.RectAspect << X.bCastShadows << X.Source << X.Flags;
		}
	}

	void SerialiseWalk(FArchive& Ar, FString& Prefab, bool& bAlias, TArray<FWalkMember>& Members)
	{
		int32 Count = Members.Num();
		uint8 AliasByte = bAlias ? 1 : 0;
		Ar << Prefab << AliasByte << Count;
		if (Ar.IsLoading())
		{
			if (Ar.IsError() || Count < 0 || Count > 8192) { Ar.SetError(); return; }
			bAlias = AliasByte != 0;
			Members.SetNum(Count);
		}
		for (int32 i = 0; i < Count && !Ar.IsError(); i++)
		{
			FWalkMember& M = Members[i];
			Ar << M.Res << M.Bundle << M.Variation;
			Ar.Serialize(M.X, sizeof(M.X));
		}
	}

	constexpr uint32 kLightCacheVer = 1;

	FString LightCacheName(const FString& Prefab)
	{
		return TEXT("placedlights_") + FMD5::HashAnsiString(*Prefab.ToLower()).Left(24);
	}

	// Keyed by the PREFAB rather than the SDK type, so two types that resolve to
	// the same prefab share one entry.
	bool LoadPrefabLights(const FString& Prefab, TArray<FCore::FLight>& Out)
	{
		TArray<uint8> Blob;
		if (!BF6HP::Shared::CacheLoadBlob(LightCacheName(Prefab), kLightCacheVer, Blob)) { return false; }
		FMemoryReader R(Blob);
		SerialiseLights(R, Out);
		if (R.IsError()) { Out.Reset(); return false; }
		return true;
	}

	void SavePrefabLights(const FString& Prefab, const TArray<FCore::FLight>& L)
	{
		if (Prefab.IsEmpty()) { return; }
		TArray<uint8> Blob;
		FMemoryWriter W(Blob);
		SerialiseLights(W, const_cast<TArray<FCore::FLight>&>(L));
		// An empty list is worth storing: "this prefab has no fixtures" is the
		// answer that costs 0.6 s to work out and is true for most props.
		if (W.IsError()) { return; }
		BF6HP::Shared::CacheSaveBlob(LightCacheName(Prefab), kLightCacheVer, MoveTemp(Blob));
	}

	// READ ONCE PER SESSION, NOT TWICE PER TYPE.
	//
	// The "can this come from cache" check and the resolve itself both want the
	// walk, so it was fetched and unpacked twice for every type. It is small and
	// there are a couple of hundred of them.
	struct FWalkMemo { FString Prefab; bool bAlias = false; TArray<FWalkMember> Members; };
	TMap<FString, FWalkMemo> GWalkMemo;

	bool LoadWalkMembers(const FString& Key, FString& OutPrefab, bool& bOutAlias,
	                     TArray<FWalkMember>& OutMembers)
	{
		if (const FWalkMemo* M = GWalkMemo.Find(Key.ToLower()))
		{
			OutPrefab = M->Prefab;
			bOutAlias = M->bAlias;
			OutMembers = M->Members;
			return true;
		}
		TArray<uint8> Blob;
		if (!BF6HP::Shared::CacheLoadBlob(WalkCacheName(Key), kWalkCacheVer, Blob)) { return false; }
		FMemoryReader R(Blob);
		SerialiseWalk(R, OutPrefab, bOutAlias, OutMembers);
		if (R.IsError() || OutPrefab.IsEmpty() || OutMembers.Num() == 0)
		{
			OutPrefab.Reset();
			OutMembers.Reset();
			return false;
		}
		FWalkMemo& M = GWalkMemo.Add(Key.ToLower());
		M.Prefab = OutPrefab;
		M.bAlias = bOutAlias;
		M.Members = OutMembers;
		return true;
	}

	void SaveWalkMembers(const FString& Key, const FString& Prefab, bool bAlias,
	                     const TArray<FWalkMember>& Members)
	{
		if (Prefab.IsEmpty() || Members.Num() == 0) { return; }
		TArray<uint8> Blob;
		FMemoryWriter W(Blob);
		FString P = Prefab;
		bool A = bAlias;
		SerialiseWalk(W, P, A, const_cast<TArray<FWalkMember>&>(Members));
		if (W.IsError() || Blob.Num() == 0) { return; }
		BF6HP::Shared::CacheSaveBlob(WalkCacheName(Key), kWalkCacheVer, MoveTemp(Blob));
	}



	// Resolve one placeable type: find its prefab, decode and merge its members,
	// build the mesh, read its lights. Heavy; called at most once per tick.
	// EVERYTHING THIS TYPE NEEDS IS ALREADY ON DISK.
	//
	// True when the walk was cached AND every member mesh it names was cached
	// too. Then the resolve is file reads and a mesh build, the reader is never
	// touched, and the seventeen second level mount is not needed at all.
	//
	// It asks the cache rather than assuming: a cache that was cleared, or an
	// install that changed under it, has to fall back to reading the game.
	bool CanResolveFromCache(const FString& KeyLower)
	{
		FString Prefab;
		bool bAlias = false;
		TArray<FWalkMember> Members;
		if (!LoadWalkMembers(KeyLower, Prefab, bAlias, Members)) { return false; }
		for (const FWalkMember& M : Members)
		{
			if (!BF6HP::Shared::CacheHasMesh(M.Res, M.Bundle, M.Variation)) { return false; }
		}
		return true;
	}

	// EVERY EXIT COUNTED. ResolveType returns from a dozen places, so the phase
	// totals only add up if the whole function is measured the same way - and
	// twice now a plausible story about the missing seconds has been wrong.
	struct FScopeAdd
	{
		double& Total; double Start;
		explicit FScopeAdd(double& T) : Total(T), Start(FPlatformTime::Seconds()) {}
		~FScopeAdd() { Total += FPlatformTime::Seconds() - Start; }
	};

	// False means NOTHING HAPPENED AND NOTHING IS WRONG: the reader was in use
	// by the level mount or the sound decoder, so this type was put back and
	// will be asked again on a later tick. It is not an attempt, it does not
	// count towards the resolve totals, and it must not set TriedEpoch.
	bool ResolveType(FTypeAsset& T)
	{
		const double T0 = FPlatformTime::Seconds();
		FScopeAdd InnerTimer(GPhInner);
		// WHERE A COLD RESOLVE SPENDS ITS SECOND.
		//
		// Four things happen here and they have very different costs: asking the
		// reader which prefab this is, decoding its member meshes, merging them,
		// and asking Unreal to build a static mesh. Optimising the wrong one is
		// the usual way to spend a day and gain nothing, so each is timed and the
		// totals are reported when the scene finishes.
		double PhWalk = 0.0, PhDecode = 0.0, PhMerge = 0.0, PhBuild = 0.0, PhLights = 0.0, PhReport = 0.0;
		bf6_ctx* Ctx = BF6HP::Shared::CoreContext();
		if (!Ctx) return true;
		// THE LOCK IS TAKEN WHERE THE READER IS USED, NOT AT THE DOOR.
		//
		// Held for the whole function it was held across the cached path too,
		// which touches no reader at all - so a scene whose every answer was on
		// disk still waited behind the twenty-five second level mount, on the
		// game thread, in multi-second blocks. Measured as thirty-four seconds
		// of a fifty-seven second pass with nothing to show for it.
		//
		// Each of the three places that actually enters the context takes it
		// below: the prefab walk, a member decode that missed the cache, and the
		// light walk.
		if (!LoadLevelNames(Ctx)) { GDeferredBusy++; T.bNeedsReader = true; T.NextTryAt = FPlatformTime::Seconds() + GBusyRetrySecs; GDeferSecs += FPlatformTime::Seconds() - T0; return false; }

		FString Prefab;
		TArray<bf6_instance> Rows;
		// THE WALK IS THE OTHER HALF OF THE COST, AND IT IS THE SAME EVERY TIME.
		//
		// Asking the reader which prefab a type resolves to, and what its
		// members are, is about six tenths of a second and produces the same
		// answer on every open of every session: the game's archives do not
		// change between them. So the answer is written down beside the meshes,
		// against the same install signature the rest of the cache uses, and a
		// second open skips straight to building.
		bool bAlias = false;
		TArray<FWalkMember> CachedMembers;
		const double W0 = FPlatformTime::Seconds();
		T.bVehicleBase = T.Key.StartsWith(TEXT("VEH_"), ESearchCase::IgnoreCase);
		// Vehicle eligibility is checked against the current SDK shelf each time.
		// Its base read is small; do not reuse a prop walk or another map's route.
		bool bWalkFromCache = !T.bVehicleBase && LoadWalkMembers(T.Key, Prefab, bAlias, CachedMembers);
		bool bWalkFound = bWalkFromCache;
		if (!bWalkFound)
		{
			BF6HP::Shared::FCoreTryLease Lease;
			if (!Lease.IsHeld())
			{
				// The mount has the reader. Put this type back rather than
				// stand here holding the frame; nothing has been decided.
				GDeferredBusy++; T.bNeedsReader = true; T.NextTryAt = FPlatformTime::Seconds() + GBusyRetrySecs; GDeferSecs += FPlatformTime::Seconds() - T0;
				return false;
			}
			if (T.bVehicleBase)
			{
				bWalkFound = FindVehicleBase(Ctx, T.Key, Prefab, CachedMembers, T.Why);
				bWalkFromCache = bWalkFound; // owned members, rather than native walk pointers
			}
			else bWalkFound = FindPrefab(Ctx, T.Key, Prefab, Rows, bAlias);
		}
		PhWalk = FPlatformTime::Seconds() - W0;
		if (!bWalkFound)
		{
			T.TriedEpoch = GEpoch;
			if (T.bVehicleBase)
			{
				T.State = FTypeAsset::EState::Missing;
				UE_LOG(LogBF6HighPolyPlaced, Warning, TEXT("placed: %s keeps the SDK preview: %s"), *T.Key, *T.Why);
				return true;
			}
			if (GFullCatalogue && !GMountedAll && GMountAll)
			{
				RequestFullMount();   // stays Pending; retried when the mount lands
				return true;
			}
			T.State = FTypeAsset::EState::Missing;
			// FINAL once every level's archives are mounted. bf6_mount_all is
			// first-mount-wins and there is nothing left to mount after it, so
			// the name tables cannot grow and re-asking can only produce the
			// same answer. Before the full mount the epoch retry still applies.
			// Only a mount that actually SUCCEEDED settles the question, and
			// only when every level was searched. With the scope on this map -
			// which is the default now - "not here" is about this map, and
			// another map may be where it lives.
			T.bGameplayMarker = IsGameplayMarker(T.Key);
			// A gameplay marker is settled the moment it fails: there is no art
			// for it anywhere, so nothing that could be mounted would change the
			// answer, and retrying it every epoch is work for nothing.
			T.bNegativeFinal = T.bGameplayMarker || (GMountedAll && !GMountFailed);
			if (T.bGameplayMarker)
			{
				T.Why = TEXT("gameplay logic, so the game has no model for it. The SDK marker is what to draw.");
				UE_LOG(LogBF6HighPolyPlaced, Verbose,
					TEXT("placed: %s is a gameplay marker; no game model exists for it"), *T.Key);
				return true;
			}
			T.Why = (GMountedAll && !GMountFailed)
				? TEXT("no pf_portal prefab exists for this type anywhere in the game")
				: GMountFailed
					? TEXT("not in this map's archives, and the full search failed, so this is not settled")
					: TEXT("no matching prop prefab in the mounted map resources; this does not establish that the game has no art for it");
			UE_LOG(LogBF6HighPolyPlaced, Log,
				TEXT("placed: %s keeps the SDK proxy (%s, %.0f ms)"),
				*T.Key, *T.Why, (FPlatformTime::Seconds() - T0) * 1000.0);
			return true;
		}
		T.Prefab = Prefab;
		if (T.bVehicleBase)
		{
			T.bLightsRead = true; // the base resource is not a fixture prefab
			T.Why = TEXT("vehicle base preview; separate wheels, rotors and other attachments are not assembled");
			UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("placed: %s -> %s (%s)"), *T.Key, *Prefab, *T.Why);
		}
		T.bAliasGuess = bAlias;
		if (bAlias) { T.AliasFrom = Prefab; }

		// Rows point into the walk; copy what is needed before the next call
		// that might replace it. From the cache the copy is already made.
		TArray<FWalkMember> Members;
		if (bWalkFromCache)
		{
			Members = MoveTemp(CachedMembers);
		}
		else
		{
		Members.Reserve(Rows.Num());
		for (const bf6_instance& r : Rows)
		{
			if (!r.res_name) continue;
			FWalkMember M;
			M.Res       = FCore::MeshResourceFor(UTF8_TO_TCHAR(r.res_name));
			M.Bundle    = r.placing_bundle ? UTF8_TO_TCHAR(r.placing_bundle) : TEXT("");
			M.Variation = r.variation ? UTF8_TO_TCHAR(r.variation) : TEXT("");
			FMemory::Memcpy(M.X, r.xform, sizeof(M.X));
			Members.Add(MoveTemp(M));
		}
		// Written down so the next open does not have to ask again.
		SaveWalkMembers(T.Key, Prefab, bAlias, Members);
		}
		T.Report.Reset();
		for (int32 i = 0; i < Members.Num(); i++)
			T.Report.Add(FString::Printf(TEXT("member%d %s bundle=%s variation=%s"), i, *Members[i].Res,
				Members[i].Bundle.IsEmpty() ? TEXT("(own)") : *Members[i].Bundle,
				Members[i].Variation.IsEmpty() ? TEXT("(none)") : *Members[i].Variation));

		FCore& Core = BF6HP::Shared::Core();
		TArray<FCore::FSection> Merged;
		TMap<FString, int32> SlotByKey;
		TMap<FString, TArray<FCore::FSection>> Decoded;
		int64 Tris = 0;
		int32 Placed = 0, Failed = 0, Dropped = 0;
		int32 CacheHits = 0, CacheMisses = 0;
		for (const FWalkMember& M : Members)
		{
			const FString DK = M.Res + TEXT("|") + M.Bundle + TEXT("|") + M.Variation;
			TArray<FCore::FSection>* Secs = Decoded.Find(DK);
			if (!Secs)
			{
				// THROUGH THE CACHE THE SCENERY BUILD HAS ALWAYS USED.
				//
				// This called the decoder directly, so every member of every
				// placed type was read out of the game archives again on every
				// session - the same crate, the same wall, every time the map
				// was opened. The map build has cached exactly this since it was
				// written. Now they share it: decode once, and every open after
				// is a file read.
				TArray<FCore::FSection> S;
				bool bHit = false;
				const double D0 = FPlatformTime::Seconds();
				const BF6HP::Shared::EReadResult RR =
					BF6HP::Shared::CacheReadMeshNoWait(M.Res, M.Bundle, M.Variation, S, bHit);
				PhDecode += FPlatformTime::Seconds() - D0;
				if (RR == BF6HP::Shared::EReadResult::Busy)
				{
					// A MISS WHILE THE MOUNT IS RUNNING IS NOT A FAILURE.
					//
					// This is where the twenty-six seconds went. Everything
					// decoded so far is thrown away and the type goes back in
					// the queue: on the next tick either the cache answers or
					// the reader is free, and either way the frame is not held.
					GDeferredBusy++; T.bNeedsReader = true; T.NextTryAt = FPlatformTime::Seconds() + GBusyRetrySecs; GDeferSecs += FPlatformTime::Seconds() - T0;
					GPhWalk += PhWalk; GPhDecode += PhDecode; GPhMerge += PhMerge;
					return false;
				}
				const bool bRead = (RR == BF6HP::Shared::EReadResult::Ok);
				if (!bRead)
				{
					// The member's read failed. Recorded so a partly decoded
					// object is not reported as a whole one.
					Failed++;
					continue;
				}
				if (bHit) { CacheHits++; } else { CacheMisses++; }
				Secs = &Decoded.Add(DK, MoveTemp(S));
			}
			int64 t = 0;
			for (const FCore::FSection& S : *Secs) t += S.Idx.Num() / 3;
			if (Tris + t > GMaxMergedTris) { Dropped++; continue; }
			const double G0 = FPlatformTime::Seconds();
			AppendMember(Merged, SlotByKey, *Secs, M.X);
			PhMerge += FPlatformTime::Seconds() - G0;
			Tris += t;
			Placed++;
		}
		if (Merged.Num() == 0)
		{
			T.State = FTypeAsset::EState::Missing;
			// A DECODE FAILURE IS AN ATTEMPT, AND HAS TO BE RECORDED AS ONE.
			// Without this the epoch stays at -1, every later epoch bump looks
			// newer, and the same failing type is retried for the rest of the
			// session - taking its seconds from everything still waiting.
			T.TriedEpoch = GEpoch;
			T.Why = FString::Printf(TEXT("%s resolved but none of its %d member mesh(es) decoded (%s)"),
				*Prefab, Members.Num(), *Core.Error);
			UE_LOG(LogBF6HighPolyPlaced, Warning, TEXT("placed: %s keeps the SDK proxy: %s"), *T.Key, *T.Why);
			return true;
		}

		// THE MERGE IS WHERE ONE TYPE'S TURN ENDS.
		//
		// Describing the geometry and building its render buffers are the two
		// worker-safe thirds of the build - 15.4 of the 26.1 seconds a scene
		// spends here - and doing them inside a per-type call means doing them
		// one at a time on the game thread. So the merged sections are queued
		// and FlushPendingBuilds takes the whole waiting set across cores.
		T.Merged = MoveTemp(Merged);
		T.PendingPlacedMembers = Placed;
		T.PendingFailed  = Failed;
		T.PendingDropped = Dropped;
		T.PendingCacheHits = CacheHits;
		T.PendingCacheMisses = CacheMisses;
		T.PendingMemberCount = Members.Num();
		T.PendingStartedAt = T0;
		T.State = FTypeAsset::EState::AwaitingBuild;
		GPhWalk += PhWalk; GPhDecode += PhDecode; GPhMerge += PhMerge;
		return true;
	}

	// The second half of a resolve, for the whole waiting set at once: describe
	// across cores, make the objects one at a time because UObjects have to be,
	// then commit the render buffers across cores again. This is the placed
	// path finally using the split the scenery build has always had.
	void FinishType(FTypeAsset& T, UStaticMesh* Mesh, int32 OutTris, double PhBuild)
	{
		bf6_ctx* Ctx = BF6HP::Shared::CoreContext();
		FCore& Core = BF6HP::Shared::Core();
		const FString& Prefab = T.Prefab;
		const TArray<FCore::FSection>& Merged = T.Merged;
		const int32 Placed = T.PendingPlacedMembers;
		const int32 Failed = T.PendingFailed;
		const int32 Dropped = T.PendingDropped;
		const int32 CacheHits = T.PendingCacheHits;
		const int32 CacheMisses = T.PendingCacheMisses;
		double PhLights = 0.0, PhReport = 0.0;
		if (!Mesh)
		{
			T.State = FTypeAsset::EState::Missing;
			T.TriedEpoch = GEpoch;   // an attempt, so it is not retried every epoch
			T.Why = TEXT("mesh build failed");
			T.Merged.Empty();
			UE_LOG(LogBF6HighPolyPlaced, Warning, TEXT("placed: %s keeps the SDK proxy: %s"), *T.Key, *T.Why);
			return;
		}
		Mesh->AddToRoot();   // shared by every placement of the type; released by RebuildAll/Stop
		T.Mesh = Mesh;
		T.Tris = OutTris;
		T.Members = Placed;
		// PARTLY DECODED IS NOT THE SAME AS DONE.
		//
		// A member that failed to read, or that was dropped for taking the
		// aggregate past the triangle cap, leaves a hole in the object - and the
		// SDK blockout that WOULD have covered it is hidden the moment anything
		// at all decoded. A building missing its roof looked exactly like a
		// building that had upgraded cleanly.
		T.MembersFailed = Failed;
		T.MembersDropped = Dropped;
		T.BoundsCm = Mesh->GetBoundingBox();
		// Counted here because this is the last point the merged sections still
		// exist; they are dropped a few lines below.
		// GLASS IS NOT A MISSING TEXTURE.
		//
		// A windscreen, a shop window and a glass railing all bind no albedo on
		// purpose: they are translucent records carrying a tint and a smoothness
		// and nothing else, and the glass path is what draws them. Counting them
		// as untextured said every car in the scene had two broken sections and
		// buried the handful that are actually wrong. Decals are the same story.
		T.SectionsNoAlbedo = 0;
		for (const FCore::FSection& S : Merged)
		{
			if (S.bTranslucent || S.bDecal) { continue; }
			bool bAlbedo = false;
			for (const FCore::FBinding& B : S.Textures)
				if (B.Slot == 0 && B.Texture >= 0) { bAlbedo = true; break; }
			if (!bAlbedo) { T.SectionsNoAlbedo++; }
		}

		// LIGHTS ARE OPTIONAL AND THEY ARE NOT FREE.
		//
		// Measured on the shipped reader: 0.64 to 0.69 seconds to discover that
		// a small prop has NO lights at all. This ran for every type, including
		// in clay mode and with the LIGHTS layer switched off, so a scene of
		// 192 types paid around two minutes for fixtures nobody had asked to
		// see. Geometry is what somebody pressed BUILD for; the fixtures can
		// follow when they are wanted.
		//
		// T.bLightsRead records that the walk has been done, so turning LIGHTS
		// on later reads them once and never again.
		if (GLightsOn && GPropLightCap > 0 && !T.bVehicleBase)
		{
			const double L0 = FPlatformTime::Seconds();
			// The cached answer is free and always worth taking. Entering the
			// reader is not, and it waits until no geometry is outstanding; see
			// the note on the deferred light pass.
			bool bLit = LoadPrefabLights(Prefab, T.Lights);
			if (!bLit && !GGeometryOutstanding)
			{
				// The geometry is already built, so a busy reader must NOT throw
				// it away. The fixtures stay unread and the deferred light pass
				// below picks them up on a later tick.
				BF6HP::Shared::FCoreTryLease Lease;
				if (Lease.IsHeld())
				{
					ReadAssetLights(Ctx, Prefab, T.Lights);
					SavePrefabLights(Prefab, T.Lights);
					bLit = true;
				}
				else { GDeferredBusy++; }
			}
			PhLights = FPlatformTime::Seconds() - L0;
			T.bLightsRead = bLit;
		}
		T.State = FTypeAsset::EState::Ready;
		// THE DIAGNOSTIC COSTS MORE THAN THE WORK IT DESCRIBES.
		//
		// One line per section naming every bound texture sheet, so a wrong look
		// can be read off the log. Each name is a reader call, and a scene of two
		// hundred types has thousands of sections: measured at more than the
		// mesh building it sits next to, on every open, to write a string almost
		// nobody reads.
		//
		// It is off unless asked for. BF6.HighPoly.Placed.Reports 1 turns it on
		// and BF6.HighPoly.Placed.Inspect says so when it has nothing to show.
		const double RP0 = FPlatformTime::Seconds();
		if (GSectionReports)
		{
			for (int32 si = 0; si < Merged.Num(); si++)
			{
				const FString Line = SectionReport(Core, Merged[si], si);
				T.Report.Add(Line);
				if (si < 8)
					UE_LOG(LogBF6HighPolyPlaced, Log, TEXT("placed: %s %s"), *T.Key, *Line);
			}
		}
		PhReport = FPlatformTime::Seconds() - RP0;
		GPhBuild += PhBuild; GPhLights += PhLights; GPhReport += PhReport;
		UE_LOG(LogBF6HighPolyPlaced, Log,
			TEXT("placed: %s -> %s: %d member(s) in %d section(s), %d tris, %d light(s)%s%s%s, %.2fs"),
			*T.Key, *Prefab, Placed, Merged.Num(), OutTris, T.Lights.Num(),
			CacheHits > 0 ? *FString::Printf(TEXT(", %d of %d member(s) from cache"), CacheHits, CacheHits + CacheMisses) : TEXT(""),
			Failed > 0 ? *FString::Printf(TEXT(", %d member(s) failed to decode"), Failed) : TEXT(""),
			Dropped > 0 ? *FString::Printf(TEXT(", %d member(s) dropped past %d tris"), Dropped, GMaxMergedTris) : TEXT(""),
			FPlatformTime::Seconds() - T.PendingStartedAt);
		// The scene does not need two copies of itself: the sections are in the
		// mesh now, and the report above is the only thing that read them.
		T.Merged.Empty();
	}

	// EVERY TYPE THAT IS WAITING, IN ONE PASS.
	//
	// Describe across cores, create the UObjects in a row because that is the
	// only thread allowed to, then commit render buffers across cores.
	//
	// THE CHUNK IS LARGE ON PURPOSE. A batch is only as fast as its slowest
	// member - one 2.5 million triangle building holds up every small type
	// describing beside it - so chunks of 24 paid that stall eight times over:
	// 16.7 seconds of worker CPU took about 14.9 seconds of wall, which is
	// barely parallel at all. The whole waiting set in one ParallelFor lets the
	// cheap types fill in around the expensive ones.
	//
	// The point is to get the scene dressed and hand the editor back, not to
	// dribble one object per frame for a minute.
	int32 GBuildChunk = 512;

	// A batch of six is a batch of one with five idle cores. bDrain is the tick
	// saying it got all the way round the records without running out of time,
	// so there is nothing else coming and however few are waiting should go now.
	int32 GBuildBatchMin = 48;

	void FlushPendingBuilds(bool bDrain)
	{
		TArray<FTypeAsset*> Batch;
		for (TPair<FString, FTypeAsset>& P : GTypes)
		{
			if (P.Value.State != FTypeAsset::EState::AwaitingBuild) { continue; }
			Batch.Add(&P.Value);
			if (Batch.Num() >= GBuildChunk) { break; }
		}
		if (Batch.Num() == 0) { return; }
		if (!bDrain && Batch.Num() < GBuildBatchMin) { return; }   // wait for company

		const double B0 = FPlatformTime::Seconds();
		TArray<TSharedPtr<BF6HP::Shared::FMeshWork>> Work;
		Work.SetNum(Batch.Num());
		ParallelFor(Batch.Num(), [&](int32 i)
		{
			Work[i] = BF6HP::Shared::DescribeGameMesh(Batch[i]->Merged);
		});

		TArray<UStaticMesh*> Meshes;
		Meshes.SetNumZeroed(Batch.Num());
		for (int32 i = 0; i < Batch.Num(); i++)
		{
			if (!Work[i]) { continue; }
			Meshes[i] = BF6HP::Shared::CreateGameMeshObject(
				TEXT("HPPlaced_") + Batch[i]->Key, Batch[i]->Merged, Work[i]);
		}

		TArray<int32> Tris;
		Tris.SetNumZeroed(Batch.Num());
		TArray<uint8> Ok;
		Ok.SetNumZeroed(Batch.Num());
		ParallelFor(Batch.Num(), [&](int32 i)
		{
			if (!Meshes[i]) { return; }
			Ok[i] = BF6HP::Shared::CommitGameMesh(Meshes[i], Work[i], Tris[i]) ? 1 : 0;
		});

		// One share of the batch's wall time each, so the phase total stays a
		// phase total rather than counting the same seconds once per type.
		const double Elapsed = FPlatformTime::Seconds() - B0;
		const double Share = Elapsed / (double)Batch.Num();
		for (int32 i = 0; i < Batch.Num(); i++)
		{
			FinishType(*Batch[i], Ok[i] ? Meshes[i] : nullptr, Tris[i], Share);
		}
	}

	// ---- the fitter (highpoly_lib._fit_scale, in centimetres) ------------------
	//
	// The prefab and the SDK model were baked in the same frame, so this is a
	// CHECK far more often than a correction: agree within the spread and the
	// overlay sits at identity. A shape that disagrees after every axis
	// permutation is the wrong asset for this proxy, and the proxy stays.
	void FitEval(const FVector& pd, const FVector& rd, double& OutSpread, double& OutScale)
	{
		const double pmax = FMath::Max3(pd.X, pd.Y, pd.Z);
		const double thin = FMath::Max(5.0, 0.12 * pmax);   // 0.05 m
		const double rthin = FMath::Max(UE_DOUBLE_SMALL_NUMBER, 0.12 * rd.GetMax());
		TArray<double> Ratios;
		for (int32 i = 0; i < 3; i++)
		{
			// Do not improve a fit by rotating a substantial dimension onto a
			// thin axis and dropping it from the comparison. DoorRural's malformed
			// 508cm mesh passed on width alone and was laid flat 251cm above the SDK.
			if ((pd[i] > thin) != (rd[i] > rthin))
			{
				OutSpread = TNumericLimits<double>::Max(); OutScale = 1.0; return;
			}
			if (pd[i] > thin && rd[i] > rthin) Ratios.Add(pd[i] / rd[i]);
		}
		if (Ratios.Num() == 0) { OutSpread = 1.0; OutScale = 1.0; return; }
		double lo = Ratios[0], hi = Ratios[0], sum = 0.0;
		for (double r : Ratios) { lo = FMath::Min(lo, r); hi = FMath::Max(hi, r); sum += r; }
		OutSpread = hi / lo;
		OutScale = sum / Ratios.Num();
	}

	void PermBases(TArray<FMatrix>& Out)
	{
		const FVector x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
		const FVector P[6][3] = { {x,y,z}, {x,z,y}, {y,x,z}, {y,z,x}, {z,x,y}, {z,y,x} };
		for (const auto& p : P)
		{
			FVector a = p[0];
			FMatrix B(a, p[1], p[2], FVector::ZeroVector);
			if (B.Determinant() < 0.0) B = FMatrix(-a, p[1], p[2], FVector::ZeroVector);
			Out.Add(B);
		}
	}

	// False when the shapes disagree beyond repair.
	bool FitTransform(AActor* A, const FTypeAsset& T, FTransform& Out)
	{
		Out = FTransform::Identity;
		UPrimitiveComponent* Proxy = ProxyOf(A);
		if (!Proxy || !T.BoundsCm.IsValid) return true;
		const FBox PA = Proxy->CalcBounds(FTransform::Identity).GetBox();
		const FBox HA = T.BoundsCm;
		const FVector PD = PA.GetSize(), HD = HA.GetSize();
		if (PD.Size() < 2.0 || HD.Size() < 2.0) return true;   // 0.02 m: a marker, nothing to fit to
		double Spread = 1.0, Scale = 1.0;
		FitEval(PD, HD, Spread, Scale);
		FMatrix BestBasis = FMatrix::Identity;
		bool bRotated = false;
		if (Spread > 1.35)
		{
			TArray<FMatrix> Bases;
			PermBases(Bases);
			for (const FMatrix& B : Bases)
			{
				double s = 0, sc = 0;
				FitEval(PD, FVector(B.TransformVector(HD)).GetAbs(), s, sc);
				if (s < Spread) { Spread = s; Scale = sc; BestBasis = B; bRotated = true; }
			}
		}
		// Exact prefab metadata is stronger evidence than a simplified SDK
		// silhouette. Foliage proxies routinely omit most of the canopy. Keep
		// the authored local transform when fitting is inconclusive; guessed
		// aliases still have to pass the shape check.
		if (Spread > 1.35) return !T.bAliasGuess;
		const bool bNeedScale = Scale < 0.9 || Scale > 1.1;
		if (!bRotated && !bNeedScale) return true;
		FTransform Xf = FTransform::Identity;
		if (bRotated) Xf.SetRotation(BestBasis.ToQuat());
		if (bNeedScale) Xf.SetScale3D(FVector(Scale));
		const FBox HA2 = HA.TransformBy(Xf);
		FVector Off = PA.GetCenter() - HA2.GetCenter();
		const bool bThin = PD.GetMin() < 0.15 * PD.GetMax();
		// A flat thing (a mat, a sign) sits on its top face rather than its
		// centre. Godot aligned the Y extent; up is Z here.
		if (bThin) Off.Z = PA.Max.Z - HA2.Max.Z;
		Xf.AddToTranslation(Off);
		Out = Xf;
		return true;
	}

	// ---- components on the placed actor -------------------------------------------
	void TagOurs(UActorComponent* C)
	{
		C->ComponentTags.Add(BF6Ext::AddonTag());
		C->ComponentTags.Add(FName(*(FString(TEXT("addon:")) + kAddonName)));
		C->ComponentTags.Add(kOurComponentTag);
	}

	bool AttachMesh(FRecord& R, AActor* A, FTypeAsset& T)
	{
		FTransform Fit;
		if (!FitTransform(A, T, Fit))
		{
			// A GUESSED NAME HAS TO EARN ITS PLACE; AN EXACT ONE DOES NOT LOSE
			// ITS PLACE FOR ONE ODD PLACEMENT.
			//
			// Two different situations were being treated as one. An alias that
			// does not fit is the wrong asset and should be dropped for the
			// whole type. An EXACT prefab that does not fit one particular
			// actor - a duplicate someone scaled, a proxy that was replaced -
			// says nothing about the other four hundred placements of that
			// type, and vetoing all of them for it is how one odd object hid a
			// whole wall.
			if (T.bAliasGuess)
			{
				T.State = FTypeAsset::EState::Missing;
				T.TriedEpoch = GEpoch;
				T.bNegativeFinal = true;
				T.Why = FString::Printf(
					TEXT("no prefab of its own; %s was tried as an alias and is the wrong shape"),
					*T.AliasFrom);
				UE_LOG(LogBF6HighPolyPlaced, Warning,
					TEXT("placed: %s keeps the SDK model: %s"), *T.Key, *T.Why);
				return false;
			}
			R.bFitRefused = true;
			T.FitRefusals++;
			if (!T.bShapeVeto)
			{
				T.bShapeVeto = true;   // kept for the status line, no longer a gate
				UE_LOG(LogBF6HighPolyPlaced, Warning,
					TEXT("placed: %s -> %s did not fit one placement's proxy, so that one keeps the ")
					TEXT("SDK model. Other placements of this type are unaffected."),
					*T.Key, *T.Prefab);
			}
			return false;
		}
		if (T.bAliasGuess && !T.bAliasProven)
		{
			T.bAliasProven = true;
			UE_LOG(LogBF6HighPolyPlaced, Display,
				TEXT("placed: %s is being drawn as %s. The names differ, and the shapes agree ")
				TEXT("within the fitter's tolerance."), *T.Key, *T.AliasFrom);
		}
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(A,
			MakeUniqueObjectName(A, UStaticMeshComponent::StaticClass(), TEXT("HPPlaced")), RF_Transient);
		if (!C) return false;
		TagOurs(C);
		// Set before attaching: UStaticMeshComponent defaults to Static, and a
		// static child of the tool's movable root is refused.
		C->SetMobility(EComponentMobility::Movable);
		C->SetStaticMesh(T.Mesh);
		C->SetReceivesDecals(false);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetGenerateOverlapEvents(false);
		// Big things cast, tiny things do not: the cost is per caster, and a
		// bottle's shadow is a few pixels nobody asked for (Godot _set_textured).
		const FVector Size = T.BoundsCm.GetSize();
		C->SetCastShadow(FMath::Max3(Size.X, Size.Y, Size.Z) >= 200.f);
		C->SetupAttachment(A->GetRootComponent());
		// This component belongs to the editable object itself. It must receive
		// hit proxies while its SDK representation is hidden. Respect the host's
		// temporary selection restrictions (link picking / focus / ghosting).
		if (const UPrimitiveComponent* Proxy = ProxyOf(A)) C->bSelectable = Proxy->bSelectable;
		C->SetRelativeTransform(Fit);
		C->RegisterComponent();
		R.Mesh = C;
		R.bNearShown = -1;
		R.MaterialMode = -1;
		// A FRESH MESH MEANS FRESH FIXTURES. The lights are parented to the
		// component this call replaces, so the old ones went with it; leaving
		// bLightsBuilt set here is how a prop that lost and regained its
		// high-poly component (a Live Coding reload, a collected weak pointer)
		// came back permanently dark.
		R.Lights.Reset();
		R.bLightsBuilt = false;
		return true;
	}

	void AttachLights(FRecord& R, AActor* A, FTypeAsset& T)
	{
		USceneComponent* Parent = R.Mesh.Get();
		// Marked built only once there is something to hang them on: setting the
		// flag before this check retires the fixtures for the rest of the session
		// on the one tick where the mesh component was not there yet.
		if (!Parent) return;
		R.bLightsBuilt = true;
		int32 n = 0;
		for (const FCore::FLight& L : T.Lights)
		{
			if (n >= GPropLightCap) break;
			float Authored = 0.f;
			ULightComponent* C = BF6HP::Shared::MakeAssetLight(A, Parent, L, GLightNameCounter++,
				TEXT("HPPlacedLight"), Authored);
			if (!C) continue;
			TagOurs(C);
			C->SetVisibility(GLightsOn && R.bWantHigh, false);
			R.Lights.Add(C);
			n++;
		}
		if (T.Lights.Num() > GPropLightCap && !T.bCapLogged)
		{
			T.bCapLogged = true;
			UE_LOG(LogBF6HighPolyPlaced, Log,
				TEXT("placed: %s carries %d light(s), capped at %d per object (BF6.HighPoly.Placed.LightCap)"),
				*T.Key, T.Lights.Num(), GPropLightCap);
		}
	}

	void SetProxyHidden(FRecord& R, AActor* A, bool bHidden)
	{
		UPrimitiveComponent* P = ProxyOf(A);
		if (!P) return;
		// THE RANGE COMES OFF BEFORE ANYTHING ELSE.
		//
		// This is the SDK's own component, not ours. Whatever we did to it has
		// to be undone whenever we stop standing in for it, or a creator who
		// switches to Low poly - or closes the add-on - is left with scenery
		// that mysteriously will not draw close up.
		if (!bHidden && R.bProxyRangedByUs)
		{
			P->MinDrawDistance = 0.f;
			P->MarkRenderStateDirty();
			R.bProxyRangedByUs = false;
		}
		if (bHidden)
		{
			if (P->IsVisible()) { P->SetVisibility(false, false); R.bProxyHiddenByUs = true; }
		}
		else if (R.bProxyHiddenByUs)
		{
			P->SetVisibility(true, false);
			R.bProxyHiddenByUs = false;
		}
	}

	void ApplyMaterials(FRecord& R, int32 Mode)
	{
		UStaticMeshComponent* C = R.Mesh.Get();
		if (!C || R.MaterialMode == Mode) return;
		R.MaterialMode = Mode;
		if (Mode == 1)
		{
			if (UMaterialInterface* Grey = BF6HP::Shared::ClayMaterial())
				for (int32 i = 0; i < C->GetNumMaterials(); i++) C->SetMaterial(i, Grey);
		}
		else if (C->OverrideMaterials.Num() > 0)
		{
			C->EmptyOverrideMaterials();
		}
	}

	void SetLightsVisible(FRecord& R, bool bOn)
	{
		for (const TWeakObjectPtr<ULightComponent>& W : R.Lights)
			if (ULightComponent* L = W.Get()) if (L->IsVisible() != bOn) L->SetVisibility(bOn, false);
	}

	// THE HAND-OVER POINT, applied to both halves so there is never a frame with
	// neither of them drawn and never one with both. Ours stops at the
	// distance; the SDK's blockout starts at it.
	// THE ENGINE WILL NOT DO THIS FOR US IN AN EDITOR VIEWPORT.
	//
	// The first version of this set LDMaxDrawDistance on our mesh and
	// MinDrawDistance on the blockout and let the renderer pick. It does not,
	// and SceneVisibility.cpp says so outright:
	//
	//     // If cull distance is disabled, always show the primitive (except foliage)
	//     if (View.Family->EngineShowFlags.DistanceCulledPrimitives
	//         && !Scene.PrimitiveSceneProxies[Index]->IsDetailMesh())
	//     { bShouldDistanceCull = false; }
	//
	// IsDetailMesh is overridden to true by exactly one class in the whole
	// engine - InstancedStaticMesh - and its own comment is explicit: "Detail
	// meshes are distance culled even if distance culling is normally disabled
	// for the view. (e.g. in editor)". Our components are plain static meshes,
	// so in a level viewport BOTH limits are ignored and both models draw at
	// once, everywhere. That is the low-poly-through-high-poly overlap, and no
	// amount of adjusting the two thresholds against each other can fix it.
	//
	// So the swap is driven here instead, from the distance to the viewport
	// camera. It behaves the same in every view, it is exact rather than a
	// band, and it costs a squared distance per object with a visibility call
	// only when an object actually crosses the line.
	void SetShownHalf(FRecord& R, AActor* A, bool bHigh)
	{
		// Reconcile actual component state too: the component may have been
		// recreated or the host may have changed selection restrictions without
		// a distance crossing. Setters still run only when a value changes.
		R.bNearShown = bHigh ? 1 : 0;
		if (UStaticMeshComponent* C = R.Mesh.Get())
		{
			if (const UPrimitiveComponent* Proxy = ProxyOf(A))
			{
				if (C->bSelectable != Proxy->bSelectable)
				{
					C->bSelectable = Proxy->bSelectable;
					C->MarkRenderStateDirty();
				}
			}
			if (C->IsVisible() != bHigh) { C->SetVisibility(bHigh, false); }
		}
		SetProxyHidden(R, A, bHigh);
		SetLightsVisible(R, bHigh && GLightsOn);
	}

	bool WantsDetailedMesh(const FBoxSphereBounds& Bounds, const TArray<FVector>& Cameras,
		float CullCm, bool bWasHigh, bool bNoCull)
	{
		if (bNoCull) return true;
		if (Cameras.IsEmpty()) return bWasHigh;
		const double Limit = FMath::Max(0.0, double(Bounds.SphereRadius))
			+ CullCm * (bWasHigh ? 1.03 : 0.97);
		for (const FVector& Cam : Cameras)
		{
			if (FVector::DistSquared(Bounds.Origin, Cam) <= Limit * Limit) return true;
		}
		return false;
	}

	// Every dressed object measured against the cameras, once a tick. Cheap: a
	// subtraction and a dot product each, and nothing touched unless it changed
	// side. The hysteresis stops an object sitting exactly on the line from
	// toggling its render state every frame.
	void UpdateDistanceSwap()
	{
		if (!GEnabled || GRecords.Num() == 0) { return; }
		TArray<FVector> Cameras;
		BF6Ext::GetBuildViewportLocations(Cameras);
		// Keep the last representation while all views are hidden. Drawing at
		// any distance and selection restrictions still work without a camera.

		const float Cull = CullCentimetres();
		for (FRecord& R : GRecords)
		{
			AActor* A = R.Actor.Get();
			if (!A || !R.Mesh.IsValid()) { continue; }
			if (!R.bWantHigh) { continue; }
			// Measured to the object's SURFACE, not its centre, so a building
			// you are standing against is near however large it is.
			const UStaticMeshComponent* C = R.Mesh.Get();
			const bool bHigh = WantsDetailedMesh(C->Bounds, Cameras, Cull, R.bNearShown == 1, GNoCull);
			SetShownHalf(R, A, bHigh);
		}
	}

	// Kept as a no-op seam: the engine distances are not used any more, and
	// anything still set from an earlier session is cleared here so a component
	// cannot carry a stale limit.
	void ApplyCullDistance(FRecord& R, AActor* A)
	{
		if (UStaticMeshComponent* C = R.Mesh.Get())
		{
			if (C->LDMaxDrawDistance != 0.f) { C->SetCullDistance(0.f); }
		}
		if (UPrimitiveComponent* P = ProxyOf(A))
		{
			if (R.bProxyRangedByUs)
			{
				P->MinDrawDistance = 0.f;
				P->MarkRenderStateDirty();
				R.bProxyRangedByUs = false;
			}
		}
	}

	void ShowHigh(FRecord& R, AActor* A, int32 Mode)
	{
		ApplyMaterials(R, Mode);
		// Clears any engine draw distance left over from the version that tried
		// to let the renderer do this; harmless when there is none.
		ApplyCullDistance(R, A);
		// WHICH HALF IS SHOWN IS THE DISTANCE SWAP'S DECISION, NOT THIS ONE.
		//
		// This used to switch our mesh on and hide the blockout unconditionally,
		// which fought UpdateDistanceSwap every tick for any object past the
		// line: one of them turned it on and the other turned it off. So it
		// only decides the FIRST time, and by the same rule.
		if (R.bNearShown < 0)
		{
			bool bHigh = true;
			if (!GNoCull)
			{
				FVector Cam = FVector::ZeroVector;
				FRotator Rot = FRotator::ZeroRotator;
				const UStaticMeshComponent* C = R.Mesh.Get();
				if (C && BF6Ext::GetBuildViewportCamera(Cam, Rot))
				{
					bHigh = (FVector::Dist(C->Bounds.Origin, Cam) - C->Bounds.SphereRadius)
					        <= CullCentimetres();
				}
			}
			SetShownHalf(R, A, bHigh);
		}
	}

	void ShowLow(FRecord& R, AActor* A)
	{
		if (UStaticMeshComponent* C = R.Mesh.Get())
			if (C->IsVisible()) C->SetVisibility(false, false);
		SetProxyHidden(R, A, false);
		SetLightsVisible(R, false);
		// Undecided again, so re-entering a high-poly mode measures the distance
		// afresh rather than inheriting whichever half was last on screen.
		R.bNearShown = -1;
	}

	void DestroyOurComponents(AActor* A)
	{
		TArray<UActorComponent*> Comps;
		A->GetComponents<UActorComponent>(Comps);
		for (UActorComponent* C : Comps)
			if (C && C->ComponentTags.Contains(kOurComponentTag)) C->DestroyComponent();
	}

	void ReleaseRecord(FRecord& R)
	{
		if (AActor* A = R.Actor.Get())
		{
			SetProxyHidden(R, A, false);
			DestroyOurComponents(A);
		}
		R.Mesh = nullptr;
		R.bNearShown = -1;
		R.Lights.Reset();
		R.bLightsBuilt = false;
		R.MaterialMode = -1;
	}

	void ClearAll()
	{
		for (FRecord& R : GRecords) ReleaseRecord(R);
		GRecords.Reset();
	}

	void DropTypes()
	{
		for (TPair<FString, FTypeAsset>& P : GTypes)
			if (P.Value.Mesh) { P.Value.Mesh->RemoveFromRoot(); P.Value.Mesh = nullptr; }
		GTypes.Reset();
	}

	// ---- the poll: who is placed right now ---------------------------------------
	void PollActors(UWorld* W)
	{
		TSet<AActor*> Seen;
		TSet<AActor*> Known;
		for (const FRecord& R : GRecords) if (AActor* A = R.Actor.Get()) Known.Add(A);
		const FName Addon = BF6Ext::AddonTag();
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (!IsValid(A) || !A->Tags.Contains(kPlacedTag) || A->Tags.Contains(Addon)) continue;
			if (BF6HP::Loadout::Handles(KeyOf(A))) continue;
			Seen.Add(A);
			if (Known.Contains(A)) continue;
			const FString Key = KeyOf(A);
			if (Key.IsEmpty()) continue;   // a volume or a marker: nothing to dress
			FRecord R;
			R.Actor = A;
			R.Key = Key;
			R.KeyLower = Key.ToLower();
			GRecords.Add(MoveTemp(R));
		}
		// Gone: destroyed with the actor, or no longer one of the tool's.
		for (int32 i = GRecords.Num() - 1; i >= 0; i--)
		{
			AActor* A = GRecords[i].Actor.Get();
			if (!A) { GRecords.RemoveAtSwap(i); continue; }
			if (!Seen.Contains(A)) { ReleaseRecord(GRecords[i]); GRecords.RemoveAtSwap(i); }
		}
	}

	void RefreshSelection()
	{
		TArray<AActor*> Sel;
		BF6Ext::Selection(Sel);
		TSet<AActor*> Set(Sel);
		for (FRecord& R : GRecords) R.bSelected = Set.Contains(R.Actor.Get());
	}

	bool TryOpenCore()
	{
		if (BF6HP::Shared::Core().IsOpen()) return true;
		FString Why;
		if (BF6HP::Shared::EnsureCoreOpen(Why)) return true;
		if (GCoreFailSaid != Why)
		{
			GCoreFailSaid = Why;
			UE_LOG(LogBF6HighPolyPlaced, Warning, TEXT("placed: cannot open the install: %s"), *Why);
		}
		return false;
	}

	// One record, one step towards what it should look like. Returns after the
	// first heavy thing it does so the caller can check the budget.
	void ApplyRecord(FRecord& R, AActor* A, bool bWantHigh, int32 Mode, double Deadline, bool& bDidHeavy)
	{
		R.bWantHigh = bWantHigh;
		if (!bWantHigh) { ShowLow(R, A); return; }

		FTypeAsset* T = GTypes.Find(R.KeyLower);
		if (!T)
		{
			T = &GTypes.Add(R.KeyLower);
			T->Key = R.Key;
		}
		if (T->State == FTypeAsset::EState::Missing && !T->bNegativeFinal && T->TriedEpoch < GEpoch)
			T->State = FTypeAsset::EState::Pending;   // something was mounted since: ask once more

		if (T->State == FTypeAsset::EState::Pending)
		{
			ShowLow(R, A);
			// ONE RESOLVE PER TICK WAS THE WRONG RULE.
			//
			// It paced on COUNT, and the thing worth pacing is COST. A type
			// that resolves in four milliseconds was made to wait a whole tick
			// for the next one, and then to stand down three more ticks under
			// the duty cycle, so a map whose props are all cheap still took
			// four ticks each and crawled through them. Godot does the same
			// work almost instantly and this is why it looked nothing like it.
			//
			// The tick budget below is the real guard: resolve until the frame
			// has had its 40 ms, then stop. A single expensive prefab blows
			// through the deadline on its own and the loop breaks after it,
			// which is the same protection the count rule was giving, arrived
			// at by measuring instead of guessing.
			if (FPlatformTime::Seconds() > Deadline) return;
			// Stood down after the last resolve, or stopped because one of them
			// ran away. Either way the proxy stays and the frame is given back.
			if (GStalled || FPlatformTime::Seconds() < GBusyUntil) return;
			// A type that already ran away once is not tried again this
			// session. Measuring a stall can only ever be after the fact, so
			// the only way to bound the SECOND one is to refuse it.
			if (GSkipped.Contains(R.KeyLower)) { return; }
			if (GMountRequested) return;               // the catalogue is on its way
			// THE READER IS NOT NEEDED FOR A TYPE WE ALREADY KNOW.
			//
			// Mounting this map's archives is seventeen seconds, and it used to
			// be demanded before anything was tried - so a scene whose every
			// answer was already on disk still sat waiting for a reader it never
			// used. The cached path is tried first now, and the mount is only
			// asked for when something actually misses.
			// IT ALREADY TRIED AND THE READER WAS BUSY. Asking again this tick
			// costs a walk, a stack of member decodes and a merge, all of it
			// thrown away at the same place. So it waits for the mount if the
			// mount is what it is waiting for, and otherwise for a breath.
			if (T->bNeedsReader)
			{
				if (GLevelMountedFor != BF6Ext::CurrentLevel())
				{
					RequestLevelMount();
					return;
				}
				if (FPlatformTime::Seconds() < T->NextTryAt) { return; }
			}
			if (T->CacheComplete < 0)
			{
				T->CacheComplete = CanResolveFromCache(R.KeyLower) ? 1 : 0;
			}
			if (T->CacheComplete == 1)
			{
				const double CachedStart = FPlatformTime::Seconds();
				GResolving = T->Key;
				const bool bConcluded = ResolveType(*T);
				GResolving.Reset();
				// A deferral is not an attempt: it costs microseconds and the
				// type is still Pending, so counting it would put a hundred
				// phantom resolves in the report - and calling it heavy would
				// end the tick, so one busy reader would stop the whole scene
				// instead of letting the cached types through.
				if (!bConcluded) { return; }
				bDidHeavy = true;
				GTypesResolved++;
				GResolveSecsTotal += FPlatformTime::Seconds() - CachedStart;
				if (T->State != FTypeAsset::EState::Ready) { return; }
				// Straight on to attaching it, below.
			}
			else
			{
			if (GLevelMountedFor != BF6Ext::CurrentLevel())
			{
				RequestLevelMount();
				return;                                 // stays Pending; the mount bumps the epoch
			}
			if (GLevelMountRequested) return;
			if (!TryOpenCore() || !CoreReady()) return;
			if (!EnsureFns())
			{
				T->State = FTypeAsset::EState::Missing;
				T->TriedEpoch = GEpoch;
				T->Why = TEXT("bf6_core.dll has no asset walk");
				return;
			}
			const double ResolveStart = FPlatformTime::Seconds();
			GResolving = T->Key;
			const bool bConcluded = ResolveType(*T);
			GResolving.Reset();
			if (!bConcluded) { return; }   // reader busy; still Pending, ask later
			// KEEPING THE BUDGET HONEST. A miss is now a hash lookup and fits
			// inside a tick with room to spare, but a HIT still walks the asset
			// graph and builds a UStaticMesh, and neither can move off the game
			// thread: bf6_ctx is not thread safe and UStaticMesh creation is
			// game-thread only. So an overrun is reported rather than hidden -
			// silence would make the 40 ms claim a lie the first time a big
			// prefab lands.
			const double ResolveSecs = FPlatformTime::Seconds() - ResolveStart;
			const double ResolveMs = ResolveSecs * 1000.0;
			GTypesResolved++;
			GResolveSecsTotal += ResolveSecs;
			if (ResolveSecs > GSlowestSecs) { GSlowestSecs = ResolveSecs; GSlowestKey = T->Key; }

			// STAND DOWN in proportion to what that cost, but ONLY when it cost
			// something. This used to fire after every resolve, so four
			// milliseconds of work bought twelve milliseconds of deliberate
			// idleness and the cheap case - which is nearly all of them - paid
			// the price set for the expensive one.
			//
			// A resolve that fits inside the tick budget has already proved it
			// is not the thing that freezes an editor.
			const bool bCostly = ResolveMs > GBudgetMs;
			if (bCostly)
			{
				bDidHeavy = true;
				// THE COOLDOWN CANNOT PREVENT THE STALL IT IS PAYING FOR.
				//
				// Measured over a real session: 97 resolves, median 2.5 s, 248
				// seconds of work, and then 248 seconds of deliberate idleness
				// on top. The idea was to give the editor its frames back, but
				// the freeze happens INSIDE the resolve, which is synchronous
				// and cannot be interrupted; standing down afterwards does not
				// shorten it by a millisecond. It only made a four minute job
				// take eight.
				//
				// What is worth keeping is a breath between long jobs, so the
				// editor is usable while a big scene fills in. One frame's
				// worth, not a multiple of the work.
				GBusyUntil = FPlatformTime::Seconds() + FMath::Min(0.25, ResolveSecs * 0.1);
			}
			if (ResolveSecs > GStallSecs)
			{
				// ONE SLOW OBJECT IS ONE SLOW OBJECT.
				//
				// This used to stop the whole module: every unrelated type still
				// waiting was refused for the rest of the session because one
				// prefab was expensive. And the limit is measured after the
				// fact, so it also caught resolves that SUCCEEDED - a type that
				// worked, took its time, and then blocked a hundred others.
				//
				// The runaway is set aside and everything else carries on. Only
				// a scene where this keeps happening stops the module, because
				// at that point it is the scene, not the object.
				GSkipped.Add(R.KeyLower);
				GStalledOn = T->Key;
				GRunaways++;
				UE_LOG(LogBF6HighPolyPlaced, Warning,
					TEXT("placed: resolving %s took %.0f s, past the %.0f s limit. It is set aside ")
					TEXT("(TRY SKIPPED AGAIN brings it back) and the other types carry on."),
					*T->Key, ResolveSecs, GStallSecs);
				if (GRunaways >= GRunawayLimit)
				{
					GStalled = true;
					UE_LOG(LogBF6HighPolyPlaced, Warning,
						TEXT("placed: %d objects have now run past the %.0f s limit, so placed High Poly has ")
						TEXT("stopped rather than keep freezing the editor. The proxies stay. ")
						TEXT("Choose a mode again, or run BF6.HighPoly.Placed.Rebuild, to try once more."),
						GRunaways, GStallSecs);
				}
			}
			else if (ResolveMs > GBudgetMs)
			{
				UE_LOG(LogBF6HighPolyPlaced, Log,
					TEXT("placed: resolving %s took %.0f ms, over the %.0f ms tick budget ")
					TEXT("(the walk and the mesh build are game-thread only). ")
					TEXT("A %.1f s breath before the next one."),
					*T->Key, ResolveMs, GBudgetMs,
					FMath::Max(0.0, GBusyUntil - FPlatformTime::Seconds()));
			}
			if (T->State != FTypeAsset::EState::Ready) return;
			}   // the reader path
		}
		// Decoded and merged, but its mesh does not exist yet: the batch that
		// makes it runs once a tick. Keep showing the SDK proxy until it does,
		// because attaching here would attach a null.
		if (T->State == FTypeAsset::EState::AwaitingBuild) { ShowLow(R, A); return; }
		// The type-wide shape veto is gone: it is now per placement, so one
		// mismatched proxy stops that object and no others.
		if (T->State == FTypeAsset::EState::Missing || R.bFitRefused) { ShowLow(R, A); return; }

		if (!R.Mesh.IsValid())
		{
			if (FPlatformTime::Seconds() > Deadline) return;
			if (!AttachMesh(R, A, *T)) { ShowLow(R, A); return; }
		}
		// The fixtures, once somebody wants them. Read here rather than during
		// the resolve so geometry never waits behind an optional walk, and once
		// per type rather than per placement.
		// NOT DEFERRED UNTIL THE GEOMETRY IS DONE, AND HERE IS WHY.
		//
		// The reader on its own takes 0.6 s to report that a prop has no lights,
		// which looked like 115 of the 232 seconds this scene costs. So the
		// walks were held back until nothing was pending, to dress the map at
		// twice the speed. Measured in the editor across two full runs of the
		// same scene: 231.8 s with the walks inline, 241.6 s with them deferred.
		// No gain, and a plausible loss - the light walk right after the mesh
		// walk for the SAME asset reuses a warm catalogue, and a walk postponed
		// is a walk gone cold.
		//
		// So it happens here, next to the geometry it belongs to, and the saving
		// that IS real stands: nothing at all is read when the lights are off or
		// the scene is in clay.
		// AND NOT WHILE GEOMETRY IS STILL ARRIVING.
		//
		// A light walk is most of a second inside the reader, and while it is in
		// there every type that needs a member decoded is turned away and throws
		// its merge out. That is what put 1,071 deferrals and 12.7 s into a load
		// that had just had its material cost removed. Fixtures are optional and
		// geometry is not, so the walks wait until the scene is dressed. Nothing
		// is lost: the pass runs on the ticker and picks them all up after.
		if (GLightsOn && GPropLightCap > 0 && !T->bLightsRead && !T->Prefab.IsEmpty()
			&& !GGeometryOutstanding)
		{
			if (FPlatformTime::Seconds() <= Deadline && !GStalled && CoreReady())
			{
				bf6_ctx* LCtx = BF6HP::Shared::CoreContext();
				if (LCtx)
				{
					const double L0 = FPlatformTime::Seconds();
					bool bLit = LoadPrefabLights(T->Prefab, T->Lights);
					if (!bLit)
					{
						// This entered the context with NO lock at all, beside a
						// mount running on a worker. Now it asks, and walks away
						// when the answer is no.
						BF6HP::Shared::FCoreTryLease Lease;
						if (Lease.IsHeld())
						{
							ReadAssetLights(LCtx, T->Prefab, T->Lights);
							SavePrefabLights(T->Prefab, T->Lights);
							bLit = true;
						}
						else { GDeferredBusy++; }
					}
					T->bLightsRead = bLit;
					bDidHeavy = bLit;
					UE_LOG(LogBF6HighPolyPlaced, Verbose,
						TEXT("placed: %s light walk %.2fs, %d fixture(s)"),
						*T->Key, FPlatformTime::Seconds() - L0, T->Lights.Num());
				}
			}
		}
		if (!R.bLightsBuilt && T->Lights.Num() > 0) AttachLights(R, A, *T);
		ShowHigh(R, A, Mode);
	}

	void LoadLevelSettings(const FString& Level)
	{
		bool b = true;
		if (GConfig && GConfig->GetBool(kIniSection, *(TEXT("Lights_") + Level), b, GEditorPerProjectIni))
			GLightsOn = b;
		else
			GLightsOn = true;
	}

	void SaveLevelSettings()
	{
		const FString Level = BF6Ext::CurrentLevel();
		if (Level.IsEmpty() || !GConfig) return;
		GConfig->SetBool(kIniSection, *(TEXT("Lights_") + Level), GLightsOn, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	bool Tick(float)
	{
		// BEFORE THE ENABLED GATE, ALWAYS. Outstanding core work is owed a
		// completion whatever the view is doing; the switch below hides objects,
		// it does not cancel a mount that is already inside the core. This line
		// used to sit under the gate and a mount that landed while placed High
		// Poly was off never released the core's busy lease.
		PollMount();
		PollLevelMount();
		if (!GEnabled || !GEditor) return true;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return true;
		const FString Level = BF6Ext::CurrentLevel();
		if (Level.IsEmpty())
		{
			if (GRecords.Num() > 0) ClearAll();
			return true;
		}

		const double Now = FPlatformTime::Seconds();
		const bool bBuilding = BF6HP::Shared::IsBuilding() && !GFinishingBuild;
		if (GLastBuilding && !bBuilding)
		{
			// THE BUILD AND THE PLACED PROPS ARE THE SAME WORK (Godot
			// _swap_placed_after_build): a build mounted the level's archives,
			// so a prefab that was missing may resolve now. Retry once.
			GEpoch++;
			GForcePoll = true;
			// AND THE SWITCH HAS TO BE RE-ASSERTED. A build makes the map's
			// local lights from scratch and they arrive VISIBLE, so with the
			// LIGHTS pill off the map lit itself back up while the pill still
			// said off. The placed fixtures survive a build and are already in
			// step, so only the map's half needs re-applying.
			if (!GLightsOn)
			{
				const int32 MapN = BF6HP::Shared::SetMapLocalLightsVisible(false);
				if (MapN > 0)
				{
					UE_LOG(LogBF6HighPolyPlaced, Log,
						TEXT("placed: build finished with lights off; %d new map light(s) hidden"), MapN);
				}
			}
		}
		GLastBuilding = bBuilding;

		// WHERE THE TIME ACTUALLY GOES, so "it stutters" can be answered with a
		// number instead of a guess. Three phases, each timed, and the worst of
		// each reported every ten seconds if anything crossed a millisecond.
		// Costs two clock reads per phase and nothing else.
		double PollMs = 0.0, SelMs = 0.0, RecMs = 0.0;

		if (GForcePoll || Now - GLastPoll >= 1.0)
		{
			const double P0 = FPlatformTime::Seconds();
			PollActors(W);
			PollMs = (FPlatformTime::Seconds() - P0) * 1000.0;
			GLastPoll = Now;
			GForcePoll = false;
			GSelectionDirty = true;
		}
		const int32 Mode = BF6HP::Shared::Mode();
		if (Mode != GLastMode) { GLastMode = Mode; GSelectionDirty = true; }
		if (GSelectionDirty)
		{
			const double S0 = FPlatformTime::Seconds();
			RefreshSelection();
			SelMs = (FPlatformTime::Seconds() - S0) * 1000.0;
			GSelectionDirty = false;
		}

		// DRESS IT, THEN GET OUT OF THE WAY.
		//
		// The 40 ms budget is the right rule for a scene that is already up and
		// having one object added to it. It is the wrong rule for the first
		// pass, where the creator is waiting for the map to appear and would
		// rather it took every core for a few seconds than trickled in for a
		// minute. So the first dress of a map gets a much larger slice, and the
		// ordinary budget comes back the moment the scene is finished.
		const float BudgetMs = GSummarySaid ? GBudgetMs : FMath::Max(GBudgetMs, GFirstDressBudgetMs);
		const double Deadline = Now + BudgetMs * 0.001;
		bool bDidHeavy = false;
		const bool bHighMode = Mode != 0;
		// A CURSOR, SO THE END OF THE LIST IS SERVED TOO.
		//
		// This loop used to start at record zero every tick and stop when the
		// budget ran out. With three and a half thousand records the ones near
		// the front spent the budget on their visibility and material checks,
		// and the ones at the back could wait indefinitely - which is exactly
		// where an object somebody just placed ends up.
		//
		// It now carries on from where it stopped and wraps, so every record is
		// reached within a bounded number of passes however long the list is.
		const double R0 = FPlatformTime::Seconds();
		if (GCursor >= GRecords.Num()) { GCursor = 0; }
		const int32 Start = GCursor;
		// Is anything still waiting to be resolved or built? Asked once, before
		// the records are walked, so every record this tick gets the same
		// answer and the light pass below can stay out of the reader's way.
		GGeometryOutstanding = false;
		for (const TPair<FString, FTypeAsset>& P : GTypes)
		{
			if (P.Value.State == FTypeAsset::EState::Pending
				|| P.Value.State == FTypeAsset::EState::AwaitingBuild)
			{
				GGeometryOutstanding = true;
				break;
			}
		}
		bool bSawEveryRecord = true;
		for (int32 n = 0; n < GRecords.Num(); n++)
		{
			const int32 Index = (Start + n) % GRecords.Num();
			FRecord& R = GRecords[Index];
			GCursor = Index + 1;
			AActor* A = R.Actor.Get();
			if (!A) continue;
			// The override flips the selection to the OTHER side of the ladder,
			// whichever side the scene is on (Godot _reoverride_selection).
			const bool bWantHigh = (GPreviewSelected && R.bSelected) ? !bHighMode : bHighMode;
			ApplyRecord(R, A, bWantHigh, Mode, Deadline, bDidHeavy);
			if (FPlatformTime::Seconds() > Deadline) { bSawEveryRecord = false; break; }
		}
		RecMs = (FPlatformTime::Seconds() - R0) * 1000.0;

		// AND THEN BUILD EVERYTHING THAT IS WAITING, ACROSS CORES.
		//
		// After the records have had their turn, so a tick decodes and merges
		// first and the batch has as much to work with as possible. A tick that
		// got all the way round has nothing more coming, so it drains whatever
		// is waiting however few that is.
		FlushPendingBuilds(bSawEveryRecord);

		// AND THEN DECIDE WHICH HALF OF EACH OBJECT IS ON SCREEN.
		//
		// Every record, every tick, not the cursor slice: an object the cursor
		// will not reach for another twenty ticks is still one the camera can
		// fly past now. It is a distance and a compare each, and it writes
		// nothing unless an object changed side.
		UpdateDistanceSwap();

		// The worst of each phase over the last ten seconds, said once, and only
		// when something actually cost a millisecond. A quiet scene prints
		// nothing at all.
		GWorstPollMs = FMath::Max(GWorstPollMs, PollMs);
		GWorstSelMs  = FMath::Max(GWorstSelMs, SelMs);
		GWorstRecMs  = FMath::Max(GWorstRecMs, RecMs);
		if (Now - GLastCostSaid >= 10.0)
		{
			GLastCostSaid = Now;
			if (GWorstPollMs + GWorstSelMs + GWorstRecMs >= 1.0)
			{
				UE_LOG(LogBF6HighPolyPlaced, Log,
					TEXT("placed cost over 10s (worst tick of each): scan %.1f ms, selection %.1f ms, ")
					TEXT("records %.1f ms, over %d record(s), mode %d"),
					GWorstPollMs, GWorstSelMs, GWorstRecMs, GRecords.Num(), Mode);
			}
			GWorstPollMs = GWorstSelMs = GWorstRecMs = 0.0;
		}

		// HOW LONG IT REALLY TOOK, said once when the last one lands. "It feels
		// slow" and "62 types in 9.4 s, the slowest 1.8 s of it" are different
		// conversations, and only the second one says what to fix next.
		if (GTypesResolved > 0 && !GSummarySaid)
		{
			// EVERY RECORD SERVED, not merely every type in the table. The table
			// fills as records are visited, so "nothing is pending" is true after
			// the very first tick and this used to announce a finished scene
			// having resolved one object.
			// EVERY OBJECT SETTLED, NOT EVERY TYPE RESOLVED.
			//
			// A type being Ready means its mesh exists. It does NOT mean the
			// records of that type are wearing it: attaching happens on a later
			// pass through the records. Announcing here counted the scene as
			// dressed with 54 objects still holding their SDK proxy, and the
			// census that followed could not make its own numbers add up - 3,307
			// showing plus 141 with a reason left 54 in no bucket at all, which
			// is exactly the objects that had not been reached yet.
			//
			// So a record is settled when it is wearing a high poly mesh, or
			// there is a reason it never will.
			bool bPending = false;
			for (const FRecord& R : GRecords)
			{
				if (!R.bWantHigh) { continue; }
				const FTypeAsset* Ty = GTypes.Find(R.KeyLower);
				if (!Ty || Ty->State == FTypeAsset::EState::Pending
				        || Ty->State == FTypeAsset::EState::AwaitingBuild)
				{
					bPending = true; break;
				}
				if (Ty->State == FTypeAsset::EState::Missing) { continue; }   // settled: no art
				if (R.bFitRefused) { continue; }                              // settled: wrong shape
				if (!R.Mesh.IsValid()) { bPending = true; break; }            // built, not yet worn
			}
			if (!bPending)
			{
				GSummarySaid = true;
				// Dressed. Whatever was standing off may go now.
				BF6HP::Shared::SetGeometryPriority(false);
				if (GMapOpenedAt > 0.0)
				{
					const double Wall = FPlatformTime::Seconds() - GMapOpenedAt;
					UE_LOG(LogBF6HighPolyPlaced, Display,
						TEXT("PLACED OBJECTS DRESSED %.1f s AFTER THE MAP OPENED ")
						TEXT("(%d object(s), %d type(s)). Geometry settled, including SDK fallbacks; ")
						TEXT("lighting and full visual readiness are separate."),
						Wall, GRecords.Num(), GTypesResolved);
				}
				// AND HOW MANY OF THEM ARE ACTUALLY WEARING IT.
				//
				// "192 types resolved" is not the same claim as "3,502 objects
				// upgraded", and the difference is where the honest answer to
				// "why is that one still a blockout" lives. Every reason a
				// placement can keep the SDK model is counted here, once, when
				// the scene settles - the shape veto especially, because it is
				// warned once per TYPE and applied per PLACEMENT, so three
				// warnings could be three objects or three hundred.
				{
					int32 Showing = 0, Refused = 0, TypeMissing = 0, Marker = 0;
					int32 Partial = 0, NoType = 0, NotWanted = 0;
					for (const FRecord& R : GRecords)
					{
						if (!R.bWantHigh) { NotWanted++; continue; }
						const FTypeAsset* Ty = GTypes.Find(R.KeyLower);
						if (!Ty) { NoType++; continue; }
						if (R.bFitRefused) { Refused++; continue; }
						if (Ty->State == FTypeAsset::EState::Missing)
						{
							if (Ty->bGameplayMarker) { Marker++; } else { TypeMissing++; }
							continue;
						}
						if (R.Mesh.IsValid())
						{
							Showing++;
							if (Ty->MembersFailed > 0 || Ty->MembersDropped > 0) { Partial++; }
						}
					}
					UE_LOG(LogBF6HighPolyPlaced, Display,
						TEXT("placed census: %d of %d object(s) are showing a high poly model ")
						TEXT("(%d of those are missing at least one member). Still on the SDK model: ")
						TEXT("%d refused by the shape check, %d whose type has no prefab, ")
						TEXT("%d gameplay markers with no art, %d not yet typed, %d not asked for."),
						Showing, GRecords.Num(), Partial,
						Refused, TypeMissing, Marker, NoType, NotWanted);
					// DID THE DISTANCE SWAP ACTUALLY TAKE? Counted rather than
					// assumed: a rendering change that silently does nothing is
					// the easiest kind to ship and the hardest to notice.
					{
						// COUNTED FROM WHAT IS ON SCREEN, not from a draw
						// distance we asked the engine for. The first version of
						// this counted LDMaxDrawDistance and happily reported
						// "3354 objects stop drawing past it" while the editor
						// viewport was ignoring every one of those limits and
						// drawing both halves. A number that cannot be wrong is
						// not a measurement.
						int32 High = 0, Blockout = 0;
						for (const FRecord& R : GRecords)
						{
							if (!R.bWantHigh || !R.Mesh.IsValid()) { continue; }
							if (R.bNearShown == 1) { High++; }
							else if (R.bNearShown == 0) { Blockout++; }
						}
						UE_LOG(LogBF6HighPolyPlaced, Display,
							TEXT("placed detail distance: %s; %d object(s) wearing the real model ")
							TEXT("and %d on the SDK blockout right now"),
							GNoCull ? TEXT("off, drawing at any distance")
							        : *FString::Printf(TEXT("%.0f m"), GCullMetres),
							High, Blockout);
					}
					if (Refused > 0)
					{
						TArray<FString> RefusedNames;
						for (const TPair<FString, FTypeAsset>& P : GTypes)
							if (P.Value.FitRefusals > 0)
								RefusedNames.Add(FString::Printf(TEXT("%s(%d)"),
									*P.Value.Key, P.Value.FitRefusals));
						RefusedNames.Sort();
						UE_LOG(LogBF6HighPolyPlaced, Display,
							TEXT("placed shape check refused: %s"),
							*FString::Join(RefusedNames, TEXT(", ")));
					}
					// UPGRADED AND UNTEXTURED IS ITS OWN FAILURE.
					//
					// A prop with geometry and no albedo binding draws as a flat
					// pale surface, which in the viewport is the same thing the
					// SDK blockout looks like - so "it did not get its high poly
					// model" and "it did, and it has no textures" arrive as the
					// same report. Counted separately so they can be told apart.
					int32 Untextured = 0, TypesUntextured = 0;
					TArray<FString> UntexturedNames;
					for (const TPair<FString, FTypeAsset>& P : GTypes)
					{
						if (P.Value.State != FTypeAsset::EState::Ready) { continue; }
						if (P.Value.SectionsNoAlbedo <= 0) { continue; }
						TypesUntextured++;
						Untextured += P.Value.SectionsNoAlbedo;
						UntexturedNames.Add(FString::Printf(TEXT("%s(%d)"),
							*P.Value.Key, P.Value.SectionsNoAlbedo));
					}
					if (TypesUntextured > 0)
					{
						UntexturedNames.Sort();
						UE_LOG(LogBF6HighPolyPlaced, Display,
							TEXT("placed materials: %d type(s) have %d section(s) with no albedo ")
							TEXT("bound, so they draw untextured; %d mesh read(s) were recovered ")
							TEXT("through the unscoped read this session. They are: %s"),
							TypesUntextured, Untextured,
							BF6HP::Shared::Core().MaterialsRecovered,
							*FString::Join(UntexturedNames, TEXT(", ")));
					}
				}
				UE_LOG(LogBF6HighPolyPlaced, Display,
					TEXT("placed: %d type(s) resolved in %.1f s of work; slowest was %s at %.2f s"),
					GTypesResolved, GResolveSecsTotal,
					GSlowestKey.IsEmpty() ? TEXT("none") : *GSlowestKey, GSlowestSecs);
				UE_LOG(LogBF6HighPolyPlaced, Display,
					TEXT("placed cold cost: %.1f s asking which prefab, %.1f s decoding members, ")
					TEXT("%.1f s merging, %.1f s building meshes, %.1f s reading lights, %.1f s naming sheets; ")
					TEXT("%.1f s inside the resolve, %.1f s around it; ")
					TEXT("%d attempt(s) put back while the reader was busy, costing %.1f s"),
					GPhWalk, GPhDecode, GPhMerge, GPhBuild, GPhLights, GPhReport,
					GPhInner - GDeferSecs, GResolveSecsTotal - (GPhInner - GDeferSecs),
					GDeferredBusy, GDeferSecs);
				double MDsec = 0.0, OBsec = 0.0, MTsec = 0.0, RNsec = 0.0;
				BF6HP::Shared::BuildGameMeshCost(MDsec, OBsec, MTsec, RNsec);
				double PMsec = 0.0, KEYsec = 0.0, MIDsec = 0.0, GLOWsec = 0.0;
				int32 MatHits = 0, MatMisses = 0, GlowMisses = 0;
				BF6HP::Shared::MaterialCost(PMsec, KEYsec, MIDsec, GLOWsec, GlowMisses,
					MatHits, MatMisses);
				UE_LOG(LogBF6HighPolyPlaced, Display,
					TEXT("placed mesh build split: %.1f s describing geometry and %.1f s building ")
					TEXT("render buffers (both summed across workers), %.1f s making mesh objects, ")
					TEXT("%.1f s binding materials on the game thread"),
					MDsec, RNsec, OBsec, MTsec);
				UE_LOG(LogBF6HighPolyPlaced, Display,
					TEXT("placed material cost: %.1f s on the shared parents, %.1f s deciding which ")
					TEXT("material each section wants (%.1f s of that asking the reader about ")
					TEXT("%d glow sheet(s)), %.1f s making the instances; ")
					TEXT("%d section(s) asked and %d instance(s) were made"),
					PMsec, KEYsec, GLOWsec, GlowMisses, MIDsec, MatHits + MatMisses, MatMisses);
			}
		}
		return true;
	}

	void OnSelectionChanged(UObject*) { GSelectionDirty = true; }

	// WHAT BELONGS TO A MAP GOES WITH THE MAP.
	//
	// ClearAll only ever dropped the actor records. The built meshes, the
	// decisions about which names could not be found, the list of objects set
	// aside for being slow and the prefab index all survived into the next map,
	// keyed by nothing but a lowercased label. Two maps can hold the same label
	// for different art, and a name that is absent from one map's archives says
	// nothing about another's, so those answers were being reused where they
	// had never been true.
	void DropPerMapState(const TCHAR* Why)
	{
		DropTypes();
		GSkipped.Empty();
		GStalled = false;
		GStalledOn.Reset();
		GRunaways = 0;
		GBusyUntil = 0.0;
		GCursor = 0;
		GTypesResolved = 0;
		GResolveSecsTotal = 0.0;
		GDeferredBusy = 0;
		GDeferSecs = 0.0;
		BF6HP::Shared::ResetBuildGameMeshCost();
		GPhWalk = GPhDecode = GPhMerge = GPhBuild = GPhLights = GPhReport = GPhInner = 0.0;
		GSlowestSecs = 0.0;
		GSlowestKey.Reset();
		GSummarySaid = false;
		GWalkMemo.Empty();
		// The mount belongs to the map that is going, so the next map mounts its
		// own rather than reading through the last one's archives.
		GLevelMountedFor.Reset();
		GLevelMountTries = 0;
		GLevelMountNextTry = 0.0;
		GEpoch++;              // invalidates the prefab index, which is per mount
		UE_LOG(LogBF6HighPolyPlaced, Log,
			TEXT("placed: dropped this map's built types and lookup decisions (%s)"), Why);
	}

	void OnMapOpened(const FString& Level, const FString&)
	{
		ClearAll();
		DropPerMapState(TEXT("a map was opened"));
		// THE CLOCK THE TARGET IS ACTUALLY MEASURED AGAINST.
		//
		// "Ten seconds" is from the moment the level is opened to the moment the
		// scene is dressed, not from the moment the resolver happens to start.
		// Summing the phase totals answers a different question: they are CPU
		// spent, they overlap the mount, and they begin after the map load. This
		// is one wall clock, started here, read once when the last object lands.
		// The opened event arrives AFTER the SDK loaded terrain and saved actors.
		// Measuring from this callback excluded those seconds from the target.
		GMapOpenedAt = BF6Ext::MapOpenStartedAt();
		if (GMapOpenedAt <= 0.0) GMapOpenedAt = FPlatformTime::Seconds();
		// The scene is what the creator is waiting for. Optional reader work
		// stands off until it is dressed; see SetGeometryPriority.
		BF6HP::Shared::SetGeometryPriority(true);
		GPreviewSelected = false;   // a scene change drops the override, as the Godot panel does
		GLastMode = -1;
		GForcePoll = true;
		LoadLevelSettings(Level);
		UE_LOG(LogBF6HighPolyPlaced, Log, TEXT("placed: map %s opened; lights %s"),
			*Level, GLightsOn ? TEXT("on") : TEXT("off"));
	}

	void OnMapClosing(const FString&)
	{
		ClearAll();
		DropPerMapState(TEXT("the map was closed"));
		// Nothing is waiting for a scene any more, so nothing should be standing
		// off for one. Also the release valve if a dress never finishes.
		BF6HP::Shared::SetGeometryPriority(false);
	}

	// ---- registration -------------------------------------------------------------
	// Deferred to engine init so the tool's seam and the editor exist. Live
	// Coding can load this module after that delegate has already fired; then
	// the engine already exists and a one-shot ticker starts it instead.
	struct FAutoStart
	{
		FAutoStart()
		{
			if (GEngine)
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
				{
					BF6HP::Placed::Start();
					return false;
				}), 0.5f);
			}
			else
			{
				FCoreDelegates::GetOnPostEngineInit().AddStatic(&BF6HP::Placed::Start);
			}
		}
	};
	FAutoStart GAutoStart;
}
}

// ============================================================================
// Public surface
// ============================================================================
namespace BF6HP
{
namespace Placed
{
	using namespace BF6HP::PlacedImpl;

	void Start()
	{
		if (GStarted || !GIsEditor) return;
		GStarted = true;
		GTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 1.0f / 30.0f);
		GSelHandle     = USelection::SelectionChangedEvent.AddStatic(&OnSelectionChanged);
		GOpenedHandle  = BF6Ext::OnMapOpened().AddStatic(&OnMapOpened);
		GClosingHandle = BF6Ext::OnMapClosing().AddStatic(&OnMapClosing);
		GPreExitHandle = FCoreDelegates::OnPreExit.AddStatic(&Stop);
		const FString Level = BF6Ext::CurrentLevel();
		if (!Level.IsEmpty()) LoadLevelSettings(Level);
		UE_LOG(LogBF6HighPolyPlaced, Log, TEXT("placed-object high poly attached (poll 1 s, budget %.0f ms/tick, light cap %d)"),
			GBudgetMs, GPropLightCap);
		// THE SHARED PARENTS, NOW, NOT WHEN A MAP NEEDS THEM.
		//
		// Inline rather than on a short ticker, and that distinction is not
		// cosmetic: everything from engine start to the first map open happens
		// inside frame zero, so a ticker - even one asking for the very next
		// tick - does not run until after the map is already open. Measured:
		// the parents were still being discovered three seconds INTO the level
		// they were supposed to be ready for.
		//
		// Here it costs the editor a few seconds of its startup, once, and
		// every level open afterwards finds them built.
		BF6HP::Shared::WarmParentMaterials();
	}

	// THE MOUNT WORKER HOLDS A RAW bf6_ctx*, so it has to be out of the core
	// before the core closes. bf6_mount_all offers no cancellation, so the wait
	// is the whole guarantee; it takes no game-thread lock and touches no
	// UObject, so blocking here cannot deadlock against it.
	//
	// Separate from Stop because ShutdownModule needs it on the plugin-unload
	// path too, where OnPreExit never fires and Stop is never called.
	void JoinCoreWorkers()
	{
		if (!GMountFuture.IsValid()) return;
		if (!GMountFuture.IsReady())
		{
			UE_LOG(LogBF6HighPolyPlaced, Log,
				TEXT("placed: waiting for the catalogue mount to leave the core before it closes"));
		}
		GMountFuture.Wait();
		GMountFuture = TFuture<FString>();
		// The lease goes back here rather than through PollMount: this runs on
		// the way down, when there will be no further tick to collect it.
		if (GMountRequested)
		{
			GMountRequested = false;
			BF6HP::Shared::SetCoreBusy(false);
		}
	}

	void Stop()
	{
		if (!GStarted) return;
		GStarted = false;
		// Nothing of ours is resolving any more; release anything standing off.
		BF6HP::Shared::SetGeometryPriority(false);
		if (GTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTicker); GTicker.Reset(); }
		USelection::SelectionChangedEvent.Remove(GSelHandle);
		BF6Ext::OnMapOpened().Remove(GOpenedHandle);
		BF6Ext::OnMapClosing().Remove(GClosingHandle);
		FCoreDelegates::OnPreExit.Remove(GPreExitHandle);
		// The worker first: everything below assumes nothing else is in the core.
		// No core calls of our own from here: the dll may already be going.
		JoinCoreWorkers();
		ClearAll();
		DropTypes();
	}

	void SetLightsOn(bool bOn)
	{
		GLightsOn = bOn;
		const int32 MapN = BF6HP::Shared::SetMapLocalLightsVisible(bOn);
		int32 PropN = 0;
		for (FRecord& R : GRecords)
		{
			SetLightsVisible(R, bOn && R.bWantHigh);
			PropN += R.Lights.Num();
		}
		SaveLevelSettings();
		UE_LOG(LogBF6HighPolyPlaced, Log, TEXT("lights %s: %d map light(s), %d placed fixture(s)"),
			bOn ? TEXT("on") : TEXT("off"), MapN, PropN);
	}

	bool Enabled()  { return GEnabled; }
	bool LightsOn() { return GLightsOn; }

	void SetPreviewSelected(bool bOn)
	{
		GPreviewSelected = bOn;
		GSelectionDirty = true;
		UE_LOG(LogBF6HighPolyPlaced, Log, TEXT("preview selected in the other detail: %s"),
			bOn ? TEXT("on") : TEXT("off"));
	}

	bool PreviewSelected() { return GPreviewSelected; }

	// A NEW DISTANCE APPLIES TO WHAT IS ALREADY THERE.
	//
	// Both of these change how three thousand components are drawn, and none of
	// those components is revisited by the resolve loop once it is Ready. So
	// they are pushed out here rather than waiting for something to happen to
	// each record; it is a couple of setters per object and no render state is
	// touched unless the value actually moved.
	void ReapplyCullToAll()
	{
		// The swap re-measures everything on the next tick anyway; this only
		// has to make sure nothing is left holding a stale engine limit from an
		// older build of the add-on.
		for (FRecord& R : GRecords)
		{
			AActor* A = R.Actor.Get();
			if (!A || !R.Mesh.IsValid()) { continue; }
			ApplyCullDistance(R, A);
		}
		UpdateDistanceSwap();
	}

	void SetCullMetres(float Metres)
	{
		const float Clamped = FMath::Clamp(Metres, 10.f, 1000.f);
		if (FMath::IsNearlyEqual(Clamped, GCullMetres, 0.01f)) { return; }
		GCullMetres = Clamped;
		ReapplyCullToAll();
		UE_LOG(LogBF6HighPolyPlaced, Log,
			TEXT("placed detail distance: %.0f m%s"), GCullMetres,
			GNoCull ? TEXT(" (ignored while drawing at any distance)") : TEXT(""));
	}

	float CullMetres() { return GCullMetres; }

	void SetNoCull(bool bOn)
	{
		if (GNoCull == bOn) { return; }
		GNoCull = bOn;
		ReapplyCullToAll();
		UE_LOG(LogBF6HighPolyPlaced, Log,
			TEXT("placed objects draw at any distance: %s"), bOn ? TEXT("on") : TEXT("off"));
	}

	bool NoCull() { return GNoCull; }

	// Let the skipped ones be tried once more. GSkipped is otherwise permanent
	// for the session on purpose - it is what stops the same twelve-second
	// prefab freezing the editor twice - so only an explicit press clears it.
	void RetrySkipped()
	{
		const int32 N = GSkipped.Num();
		GSkipped.Empty();
		GStalled = false;
		GStalledOn.Reset();
		GBusyUntil = 0.0;
		GEpoch++;
		GForcePoll = true;
		GSummarySaid = false;
		UE_LOG(LogBF6HighPolyPlaced, Display,
			TEXT("placed: %d skipped type(s) will be tried again"), N);
		BF6Ext::Notify(N > 0
			? FString::Printf(TEXT("High Poly: trying %d skipped object(s) again."), N)
			: FString(TEXT("High Poly: nothing was skipped.")));
	}

	void RebuildAll()
	{
		for (FRecord& R : GRecords) ReleaseRecord(R);
		DropTypes();
		GTypesResolved = 0;
		GResolveSecsTotal = 0.0;
		GDeferredBusy = 0;
		GDeferSecs = 0.0;
		BF6HP::Shared::ResetBuildGameMeshCost();
		GPhWalk = GPhDecode = GPhMerge = GPhBuild = GPhLights = GPhReport = GPhInner = 0.0;
		GSlowestSecs = 0.0;
		GSlowestKey.Reset();
		GSummarySaid = false;
		GEpoch++;
		GForcePoll = true;
		// The user asked for this one, so a stop from an earlier runaway resolve
		// is lifted: refusing their own rebuild would leave no way back.
		GStalled = false;
		GStalledOn.Reset();
		GBusyUntil = 0.0;
		// GSkipped is deliberately KEPT: rebuilding is "carry on with the
		// rest", not "try the thing that froze the editor again".
		UE_LOG(LogBF6HighPolyPlaced, Log,
			TEXT("placed: dropped every built type; re-resolving (%d type(s) skipped as too slow)"),
			GSkipped.Num());
	}

	// The master switch, as a switch. It had a console command and nothing in
	// the panel, so an editor where placed high poly had been turned off - by
	// the command, or by the resolver stopping itself after a runaway - offered
	// no way to turn it back on. "I cannot activate the button" is exactly what
	// that looks like from the outside.
	void SetEnabled(bool bOn)
	{
		if (!bOn)
		{
			if (!GEnabled) { return; }
			GEnabled = false;
			ClearAll();
			return;
		}

		// ASKING FOR THEM AGAIN IS NOT A NO-OP.
		//
		// This returned early when the switch was already on, and the switch is
		// on nearly all the time: picking Clay or Textured turns it on, and so
		// does a build. So every later "give me the high poly props" - changing
		// mode, finishing a build, pressing BUILD - hit that early return and
		// did nothing, while a stalled resolver or a type marked missing before
		// this map's archives were mounted stayed exactly as it was. The props
		// sat on their SDK blockouts with the panel saying Textured.
		//
		// Now it always lifts the stop, bumps the epoch so types that were
		// missing are tried once more, and forces a poll. All three are cheap
		// and paced by the resolve budget.
		const bool bWasOff = !GEnabled;
		GEnabled = true;
		GStalled = false;
		GStalledOn.Reset();
		GBusyUntil = 0.0;
		GEpoch++;
		GForcePoll = true;
		GSummarySaid = false;   // a fresh round of resolving deserves its own line
		if (!bWasOff)
		{
			UE_LOG(LogBF6HighPolyPlaced, Verbose,
				TEXT("placed: re-asserted; retrying missing types at epoch %d"), GEpoch);
		}
	}

	// WHY NOTHING IS HAPPENING, before the counts.
	//
	// This line only ever reported totals, so every reason a placed object can
	// stay on its SDK model - the switch off, the scene in low-poly mode, the
	// resolver stopped, the catalogue still mounting - produced the same
	// "0 of 412 in high-poly" and no clue which of them it was.
	FString Resolving() { return GResolving; }
	bool FinishBuildStep(int32& Done, int32& Total, FString& Error)
	{
		TGuardValue<bool> Guard(GFinishingBuild,true);
		Tick(0.f);
		Done=Total=0;
		if(GStalled) { Error=TEXT("Placed-object preparation stopped on ")+GStalledOn; return false; }
		if(!GEnabled) return true;
		for(const FRecord& R:GRecords)
		{
			if(!R.Actor.IsValid()) continue;
			const bool Wanted=(GPreviewSelected&&R.bSelected)?BF6HP::Shared::Mode()==0:BF6HP::Shared::Mode()!=0;
			if(!Wanted) continue;
			++Total;
			const FTypeAsset* T=GTypes.Find(R.KeyLower);
			if(!T) continue;
			if(R.bFitRefused||T->State==FTypeAsset::EState::Missing) { ++Done; continue; }
			if(T->State!=FTypeAsset::EState::Ready||!R.Mesh.IsValid()) continue;
			if(GLightsOn&&GPropLightCap>0&&!T->Prefab.IsEmpty()
				&&(!T->bLightsRead||(!T->Lights.IsEmpty()&&!R.bLightsBuilt))) continue;
			++Done;
		}
		return Done==Total&&!GForcePoll&&!GMountRequested&&!GLevelMountRequested;
	}

	FString StatusLine()
	{
		int32 High = 0, SdkReady = 0, Lights = 0;
		for (const FRecord& R : GRecords)
		{
			if (const UStaticMeshComponent* C = R.Mesh.Get())
			{
				if (C->IsVisible()) High++;
				else if (AActor* A = R.Actor.Get())
				{
					if (const UPrimitiveComponent* P = ProxyOf(A))
						SdkReady += P->IsVisible();
				}
			}
			Lights += R.Lights.Num();
		}
		int32 Ready = 0, Missing = 0, Pending = 0;
		for (const TPair<FString, FTypeAsset>& P : GTypes)
		{
			switch (P.Value.State)
			{
			case FTypeAsset::EState::Ready:   Ready++; break;
			case FTypeAsset::EState::Missing: Missing++; break;
			default:                          Pending++; break;
			}
		}
		if (!GEnabled)  return TEXT("off - switch this on to use the real game models");
		if (GMountRequested) return TEXT("reading the object catalogue");
		if (GStalled)
		{
			// It used to say "REBUILD PLACED skips it and carries on", and that
			// button no longer exists: the panel now asks for placed objects by
			// the mode. An instruction pointing at a control that is not there
			// is worse than no instruction, because the reader assumes they are
			// the one who cannot find it.
			return FString::Printf(
				TEXT("stopped on %s - it took over %.0fs. Choose a mode again to carry on without it."),
				GStalledOn.IsEmpty() ? TEXT("one object") : *GStalledOn, GStallSecs);
		}
		if (BF6HP::Shared::Mode() == 0 && !GPreviewSelected)
		{
			// The mode gate is the one that catches people out, because the
			// switch is on, the count is zero, and nothing in the panel said
			// the whole ladder was on its bottom rung.
			return FString::Printf(
				TEXT("waiting for a high-poly MODE - %d object(s) ready to swap"), GRecords.Num());
		}
		// WHAT IS ON SCREEN, WHAT IS COMING, AND WHAT NEEDS A DECISION.
		//
		// "0 of 412 in high-poly" was one number for four different situations.
		// These are counted in OBJECTS rather than types, because objects are
		// what somebody is looking at, and the three states answer three
		// different questions: is it working, is it still going, is it stuck.
		int32 NeedMapping = 0, Queued = 0, Markers = 0, FitFallback = 0, VehicleBases = 0;
		for (const FRecord& R : GRecords)
		{
			const FTypeAsset* T = GTypes.Find(R.KeyLower);
			if (!T) { Queued++; continue; }
			if (R.bFitRefused) { FitFallback++; continue; }
			if (T->bVehicleBase && R.Mesh.IsValid()) VehicleBases++;
			if (T->State == FTypeAsset::EState::Missing)
			{
				if (T->bGameplayMarker) { Markers++; } else { NeedMapping++; }
			}
			else if (T->State != FTypeAsset::EState::Ready || !R.Mesh.IsValid()) { Queued++; }
		}
		int32 PartialTypes = 0, AliasTypes = 0;
		for (const TPair<FString, FTypeAsset>& P : GTypes)
		{
			if (P.Value.State != FTypeAsset::EState::Ready) { continue; }
			if (P.Value.MembersFailed > 0 || P.Value.MembersDropped > 0) { PartialTypes++; }
			if (P.Value.bAliasGuess) { AliasTypes++; }
		}
		FString Line = FString::Printf(TEXT("%d high-poly visible, %d built but showing SDK, %d queued, %d need mapping"),
			High, SdkReady, Queued, NeedMapping);
		if (FitFallback > 0) Line += FString::Printf(TEXT(", %d SDK fallback(s) after shape check"), FitFallback);
		if (VehicleBases > 0) Line += FString::Printf(TEXT(", %d vehicle base preview(s), attachments incomplete"), VehicleBases);
		if (PartialTypes > 0)
		{
			Line += FString::Printf(TEXT(", %d type(s) missing pieces"), PartialTypes);
		}
		if (AliasTypes > 0)
		{
			Line += FString::Printf(TEXT(", %d drawn under another name"), AliasTypes);
		}
		if (Markers > 0)
		{
			// Said, but said as what it is. These are spawners and triggers,
			// and they are supposed to stay as markers.
			Line += FString::Printf(TEXT(", %d gameplay marker(s) with no model"), Markers);
		}
		if (Lights > 0) { Line += FString::Printf(TEXT(", %d light(s)"), Lights); }
		if (GSkipped.Num() > 0)
		{
			Line += FString::Printf(TEXT(", %d set aside for being slow"), GSkipped.Num());
		}
		// The scope, so it is visible that only this map is being read.
		Line += GMountedAll ? TEXT(" [every level mounted]") : TEXT(" [this map only]");
		return Line;
	}

	// The switches and the one thing that actually happens. LIGHTS, PLACED
	// THE DETAIL DISTANCE, ON ITS OWN.
	//
	// Kept apart from the placed-object controls because the panel lifts it
	// above the BUILD button: it governs how much of the whole view is drawn
	// as the real thing, not one feature's behaviour.
	void AddDistanceControls(TArray<FControl>& R)
	{
		{
			FControl E;
			E.Kind = FControl::EKind::Slider;
			E.Label = TEXT("DETAIL DISTANCE");
			E.Tip = TEXT("How far away the real game models are drawn. Past this the SDK blockout "
			             "takes over, so the city stays complete either way. Lower is faster: every "
			             "placed object is its own draw call carrying its full detail.");
			E.Min = 10.f; E.Max = 1000.f; E.Step = 10.f;
			E.GetValue = []{ return GCullMetres; };
			E.SetValue = [](float V) { SetCullMetres(V); };
			E.Sub = []
			{
				if (GNoCull) return FString(TEXT("ignored - drawing at any distance"));
				return FString::Printf(TEXT("%.0f m, then the blockout"), GCullMetres);
			};
			R.Add(MoveTemp(E));
		}
		{
			FControl E;
			E.Kind = FControl::EKind::Toggle;
			E.Label = TEXT("DRAW AT ANY DISTANCE");
			E.Tip = TEXT("Ignore the detail distance and draw every placed object in full, however "
			             "far away it is. Truest to the game and the most expensive thing the "
			             "add-on can be asked to do.");
			E.Get = []{ return GNoCull; };
			E.Set = [](bool b){ SetNoCull(b); };
			E.Sub = []
			{
				if (!GNoCull) return FString::Printf(TEXT("off - real models within %.0f m"), GCullMetres);
				return FString(TEXT("on - every object in full"));
			};
			R.Add(MoveTemp(E));
		}
	}

	// OBJECTS and PREVIEW SELECTED are switches; rebuilding is an action.
	void AddControls(TArray<FControl>& R)
	{
		// NO SWITCH OF THEIR OWN. Placed objects follow the detail mode, the
		// same as everything else the add-on draws: choosing a high-poly mode
		// is what asks for them, and a second switch saying "but really" was a
		// way for the panel to contradict itself. The master GEnabled survives
		// as a console escape hatch (BF6.HighPoly.Placed.Enabled) and is turned
		// back on by picking a mode, so it can never strand anybody.
		// LIGHTS is not here any more: it is a LAYER, drawn or not drawn like
		// the terrain, the roads and the water, and it belongs beside them
		// rather than buried under the placed objects it is not limited to. It
		// was only ever in this module because this module owned the switch.
		{
			FControl E;
			E.Kind = FControl::EKind::Toggle;
			E.Label = TEXT("PREVIEW SELECTED");
			E.Tip = TEXT("Draw the selected object at the other detail level, to compare.");
			E.Get = []{ return GPreviewSelected; };
			E.Set = [](bool b){ SetPreviewSelected(b); };
			E.Sub = []
			{
				const bool bLow = BF6HP::Shared::Mode() == 0;
				if (!GPreviewSelected) return FString(bLow ? TEXT("off - selection in high-poly") : TEXT("off - selection in low-poly"));
				return FString(bLow ? TEXT("selection in high-poly") : TEXT("selection in low-poly"));
			};
			R.Add(MoveTemp(E));
		}
		// NO REBUILD BUTTON EITHER. It did what picking a mode is now supposed
		// to do, which made it a second way to ask for the same thing and a
		// place for the two to disagree. Choosing Low poly, Clay or Textured
		// re-asserts the module, lifts a stop and retries missing types; BUILD
		// does it again once the map's archives are mounted.
		// BF6.HighPoly.Placed.Rebuild is still there for a wedged session.
		//
		// THE ONE THING A MODE CANNOT DO is bring back an object that was
		// skipped for taking too long. That refusal is deliberate and lasts the
		// session, so without this there was no way back at all except a
		// console command nobody outside this file knows the name of.
		{
			FControl E;
			E.Kind = FControl::EKind::Action;
			E.Label = TEXT("TRY SKIPPED AGAIN");
			E.Tip = TEXT("An object that took too long is left on its blockout for the rest of the session. "
			             "Press this after a build or a game update to give it another go.");
			E.Sub = []
			{
				if (GSkipped.Num() == 0) { return FString(TEXT("nothing was skipped")); }
				return FString::Printf(TEXT("%d skipped for being too slow"), GSkipped.Num());
			};
			E.OnAct = []{ RetrySkipped(); };
			R.Add(MoveTemp(E));
		}
	}

	void AddPieEntries(TArray<BF6Ext::FPieSubEntry>& R)
	{
		{
			BF6Ext::FPieSubEntry E;
			E.Label = TEXT("LIGHTS");
			E.Sub = []{ return FString(GLightsOn ? TEXT("on") : TEXT("off")); };
			E.OnPick = []{ SetLightsOn(!GLightsOn); };
			R.Add(E);
		}
		{
			BF6Ext::FPieSubEntry E;
			E.Label = TEXT("PREVIEW SELECTED");
			E.Sub = []
			{
				const bool bLow = BF6HP::Shared::Mode() == 0;
				if (!GPreviewSelected) return FString(bLow ? TEXT("off - selection in high-poly") : TEXT("off - selection in low-poly"));
				return FString(bLow ? TEXT("selection in high-poly") : TEXT("selection in low-poly"));
			};
			E.OnPick = []{ SetPreviewSelected(!GPreviewSelected); };
			R.Add(E);
		}
		{
			BF6Ext::FPieSubEntry E;
			E.Label = TEXT("PLACED OBJECTS");
			E.Sub = []{ return StatusLine(); };
			E.OnPick = []{ RebuildAll(); };
			R.Add(E);
		}
	}
}
}

// ============================================================================
// Console
// ============================================================================
namespace BF6HP
{
namespace PlacedImpl
{
static bool BF6_PlacedParseOnOff(const TArray<FString>& Args, bool& Out)
{
	if (Args.Num() < 1) return false;
	const FString A = Args[0].ToLower();
	if (A == TEXT("1") || A == TEXT("on") || A == TEXT("true"))  { Out = true;  return true; }
	if (A == TEXT("0") || A == TEXT("off") || A == TEXT("false")) { Out = false; return true; }
	return false;
}

static FAutoConsoleCommand GBF6PlacedVerifySwapCmd(
	TEXT("BF6.HighPoly.Placed.VerifySwap"),
	TEXT("Read actual component visibility and picking eligibility for dressed objects. No scene changes."),
	FConsoleCommandDelegate::CreateStatic([]
	{
		int32 Dressed = 0, Both = 0, Neither = 0, High = 0, Low = 0, PickMismatch = 0;
		TArray<FVector> Cameras;
		BF6Ext::GetBuildViewportLocations(Cameras);
		for (const FRecord& R : GRecords)
		{
			AActor* A = R.Actor.Get();
			UStaticMeshComponent* C = R.Mesh.Get();
			UPrimitiveComponent* P = A ? ProxyOf(A) : nullptr;
			if (!C || !P) continue;
			Dressed++;
			const bool bHigh = C->IsVisible(), bLow = P->IsVisible();
			High += bHigh; Low += bLow;
			Both += bHigh && bLow; Neither += !bHigh && !bLow;
			PickMismatch += bHigh && (C->bSelectable != P->bSelectable);
		}
		// These are component states, not a claim that offscreen or occluded
		// objects contributed pixels. Compare screenshots / hit proxies too.
		UE_LOG(LogBF6HighPolyPlaced, Display,
			TEXT("swap verification: dressed=%d high=%d low=%d both=%d neither=%d ")
			TEXT("picking_mismatch=%d visible_perspective_views=%d (component states, not rendered pixels)"),
			Dressed, High, Low, Both, Neither, PickMismatch, Cameras.Num());
	}));

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBF6PlacedFitDimensionsTest, "BF6.HighPoly.Placed.FitDimensions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBF6PlacedFitDimensionsTest::RunTest(const FString& Parameters)
{
	// Measured local bounds from the user's Bridge doors. No permutation may
	// hide the doubled height by comparing the unchanged width alone.
	const FVector ProxySize(168.0, 17.57336235, 260.0);
	const FVector BrokenSize(167.96875, 18.0053711, 508.0078125);
	TArray<FMatrix> Bases;
	PermBases(Bases);
	for (int32 i = 0; i < Bases.Num(); ++i)
	{
		double Spread, Scale;
		FitEval(ProxySize, FVector(Bases[i].TransformVector(BrokenSize)).GetAbs(), Spread, Scale);
		TestTrue(FString::Printf(TEXT("Malformed door rejected in orientation %d"), i), Spread > 1.35);
	}
	double Spread, Scale;
	FitEval(ProxySize, FVector(167.96875, 18.0053711, 259.9609375), Spread, Scale);
	TestTrue(TEXT("Aligned door geometry is accepted"), Spread <= 1.35);
	FitEval(ProxySize, ProxySize * 0.01, Spread, Scale);
	TestTrue(TEXT("Uniform unit conversion remains possible"), Spread <= 1.35 && FMath::IsNearlyEqual(Scale, 100.0));
	FitEval(FVector(1, 1, 100), FVector(2, 2, 200), Spread, Scale);
	TestTrue(TEXT("Matching thin poles remain valid"), Spread <= 1.35 && FMath::IsNearlyEqual(Scale, 0.5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBF6PlacedSwapTest, "BF6.HighPoly.Placed.Representation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBF6PlacedSwapTest::RunTest(const FString& Parameters)
{
	// Two distant observers, offset bounds, hysteresis and no visible view.
	const FBoxSphereBounds Bounds(FVector(2000, 0, 0), FVector(100), 100);
	TArray<FVector> Cameras;
	Cameras.Add(FVector(20000, 0, 0));
	TestFalse(TEXT("Distant view keeps blockout"), WantsDetailedMesh(Bounds, Cameras, 1000, false, false));
	Cameras.Add(FVector(2000, 0, 0));
	TestTrue(TEXT("Second nearby view retains detail"), WantsDetailedMesh(Bounds, Cameras, 1000, false, false));
	Cameras = { FVector(3100, 0, 0) };
	TestFalse(TEXT("Inside hysteresis retains low"), WantsDetailedMesh(Bounds, Cameras, 1000, false, false));
	TestTrue(TEXT("Inside hysteresis retains high"), WantsDetailedMesh(Bounds, Cameras, 1000, true, false));
	Cameras = { FVector(3060, 0, 0) };
	TestTrue(TEXT("Crossing inner surface threshold shows detail"), WantsDetailedMesh(Bounds, Cameras, 1000, false, false));
	Cameras = { FVector(3140, 0, 0) };
	TestFalse(TEXT("Crossing outer threshold restores blockout"), WantsDetailedMesh(Bounds, Cameras, 1000, true, false));
	TestTrue(TEXT("Unrestricted detail ignores distance"), WantsDetailedMesh(Bounds, Cameras, 1000, false, true));
	Cameras.Reset();
	TestTrue(TEXT("Hidden views retain high"), WantsDetailedMesh(Bounds, Cameras, 1000, true, false));
	TestFalse(TEXT("Hidden views retain low"), WantsDetailedMesh(Bounds, Cameras, 1000, false, false));

	UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world"), W)) return false;
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	AActor* A = W->SpawnActor<AActor>(Spawn);
	if (!TestNotNull(TEXT("Transient fixture"), A)) return false;
	ON_SCOPE_EXIT { W->DestroyActor(A); };
	UStaticMeshComponent* Proxy = NewObject<UStaticMeshComponent>(A, NAME_None, RF_Transient);
	A->SetRootComponent(Proxy);
	Proxy->SetMobility(EComponentMobility::Movable);
	Proxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Proxy->RegisterComponent();
	FTypeAsset Type;
	Type.Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Fixture mesh"), Type.Mesh)) return false;
	Type.BoundsCm = Type.Mesh->GetBoundingBox();
	FRecord R;
	R.Actor = A;
	if (!TestTrue(TEXT("Attach through production path"), AttachMesh(R, A, Type))) return false;
	UStaticMeshComponent* High = R.Mesh.Get();
	SetShownHalf(R, A, true);
	TestTrue(TEXT("Detail visible and pickable"), High->IsVisible() && High->bSelectable);
	TestFalse(TEXT("SDK hidden near"), Proxy->IsVisible());
	TestTrue(TEXT("Replacement selects original owner"), High->GetOwner() == A);
	Proxy->bSelectable = false;
	SetShownHalf(R, A, true);
	TestFalse(TEXT("Host selection restrictions propagate without crossing"), High->bSelectable);
	Proxy->bSelectable = true;
	SetShownHalf(R, A, true);
	TestTrue(TEXT("Picking restored without crossing"), High->bSelectable);
	SetShownHalf(R, A, false);
	TestTrue(TEXT("Far has only SDK"), Proxy->IsVisible() && !High->IsVisible());
	High->DestroyComponent();
	TestTrue(TEXT("Recreate through production path"), AttachMesh(R, A, Type));
	SetShownHalf(R, A, false);
	TestTrue(TEXT("Recreated far component stays hidden"), Proxy->IsVisible() && !R.Mesh->IsVisible());
	SetShownHalf(R, A, true);
	ShowLow(R, A);
	TestTrue(TEXT("Mode off restores SDK"), Proxy->IsVisible() && !R.Mesh->IsVisible());
	SetShownHalf(R, A, true);
	ReleaseRecord(R);
	TestTrue(TEXT("Release restores SDK"), Proxy->IsVisible());
	return true;
}
#endif

static FAutoConsoleCommand GBF6PlacedLightsCmd(
	TEXT("BF6.HighPoly.Lights"),
	TEXT("Every light the High Poly add-on owns, on or off at once: the map's own lamps and the fixtures on placed objects. Remembered per level. Usage: BF6.HighPoly.Lights 0|1"),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		bool b = false;
		if (!BF6_PlacedParseOnOff(Args, b))
		{
			UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("lights are %s (usage: BF6.HighPoly.Lights 0|1)"),
				BF6HP::Placed::LightsOn() ? TEXT("on") : TEXT("off"));
			return;
		}
		BF6HP::Placed::SetLightsOn(b);
	}));

static FAutoConsoleCommand GBF6PlacedPreviewCmd(
	TEXT("BF6.HighPoly.PreviewSelected"),
	TEXT("Selected placed objects show the OTHER detail level: high-poly while the scene is low-poly, low-poly while it is high-poly. Follows the selection live. Usage: BF6.HighPoly.PreviewSelected 0|1"),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		bool b = false;
		if (!BF6_PlacedParseOnOff(Args, b))
		{
			UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("preview selected is %s (usage: BF6.HighPoly.PreviewSelected 0|1)"),
				BF6HP::Placed::PreviewSelected() ? TEXT("on") : TEXT("off"));
			return;
		}
		BF6HP::Placed::SetPreviewSelected(b);
	}));

static FAutoConsoleCommand GBF6PlacedStatusCmd(
	TEXT("BF6.HighPoly.Placed.Status"),
	TEXT("What the placed-object module holds: objects, built types, misses and why."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("placed: %s; mode %d, lights %s, preview-selected %s, catalogue %s"),
			*BF6HP::Placed::StatusLine(), BF6HP::Shared::Mode(),
			GLightsOn ? TEXT("on") : TEXT("off"), GPreviewSelected ? TEXT("on") : TEXT("off"),
			GMountedAll ? TEXT("full") : (GMountRequested ? TEXT("mounting") : TEXT("current level only")));
		for (const TPair<FString, FTypeAsset>& P : GTypes)
		{
			const FTypeAsset& T = P.Value;
			int32 Users = 0;
			for (const FRecord& R : GRecords) if (R.KeyLower == P.Key) Users++;
			if (T.State == FTypeAsset::EState::Ready)
			{
				UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("  %-40s %s  %d member(s) %d tris %d light(s) x%d%s"),
					*T.Key, *T.Prefab, T.Members, T.Tris, T.Lights.Num(), Users,
					T.bVehicleBase ? TEXT("  BASE PREVIEW (attachments incomplete)")
						: T.bShapeVeto ? TEXT("  SHAPE VETO") : TEXT(""));
			}
			else
			{
				UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("  %-40s %s x%d  %s"), *T.Key,
					T.State == FTypeAsset::EState::Missing ? TEXT("missing") : TEXT("waiting"), Users, *T.Why);
			}
		}
	}));

static FAutoConsoleCommand GBF6PlacedInspectCmd(
	TEXT("BF6.HighPoly.Placed.Inspect"),
	TEXT("Every material fact the placed path decoded for one placeable type: members with their scope, and per section the sheets by name, the record tint, roughness and flags. Usage: BF6.HighPoly.Placed.Inspect <Type> (substring, case-insensitive; no argument lists every built type)."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const FString Want = Args.Num() > 0 ? Args[0] : FString();
		int32 Shown = 0;
		for (const TPair<FString, FTypeAsset>& P : GTypes)
		{
			const FTypeAsset& T = P.Value;
			if (!Want.IsEmpty() && !T.Key.Contains(Want, ESearchCase::IgnoreCase)) continue;
			Shown++;
			UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("%s -> %s [%s]%s"), *T.Key,
				T.Prefab.IsEmpty() ? TEXT("(unresolved)") : *T.Prefab,
				T.State == FTypeAsset::EState::Ready ? TEXT("ready")
					: T.State == FTypeAsset::EState::Missing ? TEXT("missing") : TEXT("waiting"),
				T.Why.IsEmpty() ? TEXT("") : *(TEXT(" ") + T.Why));
			// A type with no report is not a silent type. It is either still in
			// the queue or it resolved to nothing, and saying so is the whole
			// point of the command: printing the header and then nothing reads
			// as "Inspect is broken".
			// UE_LOG wants a literal format string, so this is two calls rather
			// than one with a ternary.
			if (T.Report.Num() == 0 && T.State == FTypeAsset::EState::Missing)
			{
				UE_LOG(LogBF6HighPolyPlaced, Display,
					TEXT("   nothing to draw: this type keeps the SDK proxy"));
			}
			else if (T.Report.Num() == 0)
			{
				UE_LOG(LogBF6HighPolyPlaced, Display,
					TEXT("   not built yet (waiting on resolve)"));
			}
			for (const FString& Line : T.Report)
				UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("   %s"), *Line);
		}
		if (Shown == 0)
		{
			// It may be placed and simply not have reached the queue yet, which
			// is a different answer from "no such type".
			int32 Placed = 0;
			for (const FRecord& R : GRecords)
				if (Want.IsEmpty() || R.Key.Contains(Want, ESearchCase::IgnoreCase))
				{
					UE_LOG(LogBF6HighPolyPlaced, Display,
						TEXT("%s -> (unresolved) [not built yet (waiting on resolve)]"), *R.Key);
					Placed++;
				}
			if (Placed == 0)
			{
				UE_LOG(LogBF6HighPolyPlaced, Display,
					TEXT("no built or placed type matches '%s'"), *Want);
			}
		}
	}));

static FAutoConsoleCommand GBF6PlacedRebuildCmd(
	TEXT("BF6.HighPoly.Placed.Rebuild"),
	TEXT("Forget every built placed-object type and resolve them again."),
	FConsoleCommandDelegate::CreateStatic([]{ BF6HP::Placed::RebuildAll(); }));

static FAutoConsoleCommand GBF6PlacedDistanceCmd(
	TEXT("BF6.HighPoly.Placed.Distance"),
	TEXT("Metres within which placed objects are drawn as the real game models, 10 to 1000 ")
	TEXT("(default 100). Past this the SDK blockout draws instead, so the scene stays whole."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() >= 1) BF6HP::Placed::SetCullMetres(FCString::Atof(*Args[0]));
		UE_LOG(LogBF6HighPolyPlaced, Display,
			TEXT("placed detail distance %.0f m%s"), BF6HP::Placed::CullMetres(),
			BF6HP::Placed::NoCull() ? TEXT(" (ignored: drawing at any distance)") : TEXT(""));
	}));

static FAutoConsoleCommand GBF6PlacedNoCullCmd(
	TEXT("BF6.HighPoly.Placed.NoCull"),
	TEXT("1 draws every placed object in full at any distance, ignoring the detail distance. ")
	TEXT("0 (default) hands distant objects back to the SDK blockout."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() >= 1) BF6HP::Placed::SetNoCull(FCString::Atoi(*Args[0]) != 0);
		UE_LOG(LogBF6HighPolyPlaced, Display,
			TEXT("placed objects draw at any distance: %s"),
			BF6HP::Placed::NoCull() ? TEXT("on") : TEXT("off"));
	}));

static FAutoConsoleCommand GBF6PlacedBudgetCmd(
	TEXT("BF6.HighPoly.Placed.Budget"),
	TEXT("Milliseconds of placed-object work per editor tick (default 40)."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() >= 1) GBudgetMs = FMath::Clamp(FCString::Atof(*Args[0]), 1.f, 2000.f);
		UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("placed budget %.0f ms per tick"), GBudgetMs);
	}));

static FAutoConsoleCommand GBF6PlacedDutyCmd(
	TEXT("BF6.HighPoly.Placed.Duty"),
	TEXT("Share of wall time the placed resolver may spend, 0.05 to 1 (default 0.25). ")
	TEXT("Lower keeps the editor smoother and props arrive slower."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() >= 1) GDutyCycle = FMath::Clamp(FCString::Atof(*Args[0]), 0.05f, 1.f);
		if (Args.Num() >= 1) { GStalled = false; GBusyUntil = 0.0; }
		UE_LOG(LogBF6HighPolyPlaced, Display,
			TEXT("placed duty cycle %.0f%% of wall time%s"), GDutyCycle * 100.f,
			GStalled ? TEXT(" (stopped after a runaway resolve)") : TEXT(""));
	}));

static FAutoConsoleCommand GBF6PlacedLightCapCmd(
	TEXT("BF6.HighPoly.Placed.LightCap"),
	TEXT("Most light components one placed object spawns (default 8). Applies to objects dressed after the change; Rebuild to re-dress."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() >= 1) GPropLightCap = FMath::Clamp(FCString::Atoi(*Args[0]), 0, 256);
		UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("placed light cap %d"), GPropLightCap);
	}));

static FAutoConsoleCommand GBF6PlacedCatalogueCmd(
	TEXT("BF6.HighPoly.Placed.FullCatalogue"),
	TEXT("Search EVERY level's archives when a placed object's prefab is not in this map's (default 0, off). ")
	TEXT("Mounting every level takes about 20 seconds and a name found only in another map's archives is not ")
	TEXT("evidence that the object belongs on this one."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		bool b = GFullCatalogue;
		if (BF6_PlacedParseOnOff(Args, b)) GFullCatalogue = b;
		UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("placed full catalogue %s%s"), GFullCatalogue ? TEXT("on") : TEXT("off"),
			GMountedAll ? TEXT(" (already mounted this session; turning it off does not unmount)") : TEXT(""));
	}));

static FAutoConsoleCommand GBF6PlacedReportsCmd(
	TEXT("BF6.HighPoly.Placed.Reports"),
	TEXT("Record what every section of every placed type is bound to, for BF6.HighPoly.Placed.Inspect ")
	TEXT("(default 0). Each texture name is a read from the game, and a scene of two hundred types spends ")
	TEXT("more on writing this than on building the meshes it describes."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		bool b = GSectionReports;
		if (BF6_PlacedParseOnOff(Args, b)) { GSectionReports = b; }
		UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("placed section reports %s%s"),
			GSectionReports ? TEXT("on") : TEXT("off"),
			GSectionReports ? TEXT(" - rebuild (choose a mode again) to fill them in") : TEXT(""));
	}));

static FAutoConsoleCommand GBF6PlacedAliasCmd(
	TEXT("BF6.HighPoly.Placed.Aliases"),
	TEXT("Try a named alias when an object has no prefab of its own - the br_ prefix, and a trailing ")
	TEXT("single-letter variant suffix (default 1). An alias is only used if the shapes agree, and it is named in the log."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		bool b = GNameAliases;
		if (BF6_PlacedParseOnOff(Args, b)) { GNameAliases = b; GEpoch++; GForcePoll = true; }
		UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("placed name aliases %s"), GNameAliases ? TEXT("on") : TEXT("off"));
	}));

static FAutoConsoleCommand GBF6PlacedEnabledCmd(
	TEXT("BF6.HighPoly.Placed.Enabled"),
	TEXT("Master switch for placed-object high poly (default 1). 0 returns every placed object to the SDK model."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		bool b = GEnabled;
		if (BF6_PlacedParseOnOff(Args, b))
		{
			// Through the same setter the panel switch uses, so turning it back
			// on from the console also lifts a stop from a runaway resolve.
			BF6HP::Placed::SetEnabled(b);
		}
		UE_LOG(LogBF6HighPolyPlaced, Display, TEXT("placed high poly %s"), GEnabled ? TEXT("on") : TEXT("off"));
	}));
}
}
