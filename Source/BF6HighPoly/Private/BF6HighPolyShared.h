#pragma once

#include "BF6HighPolyControls.h"   // FControl: what the panel and the radial both render

#include "CoreMinimal.h"
#include "BF6HighPolyCore.h"
#include "BF6SDKExtension.h"

class UStaticMesh;
class UMaterialInterface;
class ULightComponent;
class AActor;
class USceneComponent;

// ============================================================================
// The seam between BF6HighPoly.cpp and the other translation units of this
// module.
//
// BF6HighPoly.cpp keeps its state and helpers file-local on purpose: one build,
// one mode ladder, one material cache. The placed-object module in
// BF6HighPolyPlaced.cpp needs a handful of those - the open core, the mesh and
// light constructors, the current mode - and it must not grow a second copy of
// any of them (the Godot plugin's make_light exists precisely so that a level
// lamp and a placed lamp can never drift apart). So the minimum is declared
// here and DEFINED at the bottom of BF6HighPoly.cpp, where it can see the
// anonymous namespace. Nothing here is exported from the module.
//
// Every function is game-thread only unless it says otherwise.
// ============================================================================

// At GLOBAL scope on purpose. Written as "class UPrimitiveComponent* C" inside
// the namespace it declares BF6HP::Shared::UPrimitiveComponent, a brand new
// type that nothing can convert a real component to.
class UPrimitiveComponent;

namespace BF6HP
{
namespace Shared
{
	// The one libbf6 context the add-on owns, and the raw handles for the few
	// exports FCore does not wrap (the per-asset walks, the full mount).
	FCore&   Core();
	bf6_ctx* CoreContext();
	void*    CoreDll();

	// Load the dll and open the install if that has not happened yet. Does NOT
	// open a level; that is the build's business. False with a reason.
	bool EnsureCoreOpen(FString& OutWhy);

	// <install>/bf6.exe, or empty when no install is known.
	FString InstallExePath();

	// A full build is queued or running. The placed module keeps its hands off
	// the core while this is true, and the build keeps its hands off while
	// CoreBusy() is true: the context is not thread-safe and the two halves
	// must never be inside it at once.
	bool IsBuilding();
	void SetCoreBusy(bool bBusy);
	bool CoreBusy();

	// ONE LOCK FOR THE ONE CONTEXT.
	//
	// CoreBusy is a co-operation flag between the build and the placed module,
	// and it only works for the callers that agreed to it. The UI sound decoder
	// never did: it runs on the thread pool, reads through the same bf6_ctx, and
	// on 8 September 2026 it was inside bf6_core at the same moment as the
	// placed module's level mount. That is an access violation reading
	// 0xffffffffffffffff on Background Worker #7, with DecodeClip on the stack -
	// a crash, not a risk.
	//
	// So every mutable native call on this context takes this. It is held for
	// the length of one call, not one job: a mount holds it for its seconds, a
	// clip decode for its milliseconds, and nobody else is inside meanwhile.
	FCriticalSection& CoreMutex();

	// GEOMETRY FIRST, EVERYTHING ELSE AFTER.
	//
	// The lock keeps the context safe, but it says nothing about who deserves
	// it. On 9 September 2026 the UI sound pass took it to run bf6_mount_all -
	// a whole-install mount - and held it for 17.6 seconds in the middle of a
	// level open, while the placed resolver was turned away 556 times and threw
	// away its merge each time. The creator is watching the map appear; the
	// tool's button clicks can wait a few seconds for their sounds.
	//
	// So a background reader job asks this first and stands off while it is
	// true. It is NOT a lock and it is not enforced: it is a courtesy that the
	// optional passes observe. Any thread may read it.
	void SetGeometryPriority(bool bOn);
	bool GeometryHasPriority();

	// ---- THE CACHE THE SCENERY BUILD HAS ALWAYS HAD ------------------------
	//
	// The map build reads a mesh through a disk cache: decode once, and every
	// session after that is a file read. The placed-object path never used it,
	// so the same wall was decoded out of the game archives again on every
	// open - measured at 231 seconds for one scene's 192 types, every time,
	// while the Godot plugin doing the same job is effectively instant on a
	// second open because it keeps what it decoded.
	//
	// Same cache, same version stamp, same install signature. A member mesh is
	// not level-specific - the same crate is the same crate on every map - so
	// these are kept in the shared scope rather than per level.
	bool CacheReadMesh(const FString& ResName, const FString& Bundle,
	                   const FString& Variation, TArray<FCore::FSection>& Out,
	                   bool& bOutFromCache);

	// A READ THAT REFUSES TO WAIT.
	//
	// The game thread must never block on the reader lock. On 9 September 2026
	// one forty-four triangle doorway - ConstructionSetDoorwayConcrete_01 - cost
	// 25.96 seconds, because its member missed the cache, took a blocking
	// FScopeLock, and the level mount was holding the same lock on a worker for
	// 26.0 seconds. A second type spent 8.94 seconds the same way. That is the
	// thirty-two seconds the phase table could not explain.
	//
	//   Ok     Out is filled; bOutFromCache says whether the reader was needed.
	//   Failed the resource is not readable, and the caller should record that.
	//   Busy   somebody else is inside the context RIGHT NOW. Nothing was read
	//          and nothing is wrong; ask again on a later tick.
	enum class EReadResult : uint8 { Ok, Failed, Busy };

	EReadResult CacheReadMeshNoWait(const FString& ResName, const FString& Bundle,
	                                const FString& Variation, TArray<FCore::FSection>& Out,
	                                bool& bOutFromCache);

	// The same refusal for the callers that enter the reader directly (the
	// prefab walk, the light walk). Take it, ask IsHeld, and do the reader work
	// only when it says yes; the lock is released with the object.
	struct FCoreTryLease
	{
		FCoreTryLease();
		~FCoreTryLease();
		bool IsHeld() const { return bHeld; }
		FCoreTryLease(const FCoreTryLease&) = delete;
		FCoreTryLease& operator=(const FCoreTryLease&) = delete;
	private:
		bool bHeld = false;
	};

	// For a caller that wants to keep something of its own beside them: the
	// placed module remembers which prefab a type resolved to and what its
	// members were, which is the other half of the cost.
	// Is a member mesh already on disk? Asked before a resolve begins, so a
	// type whose every answer is cached never needs the reader - and a scene
	// where that is true of every type never mounts the level at all.
	bool CacheHasMesh(const FString& ResName, const FString& Bundle, const FString& Variation);

	bool CacheLoadBlob(const FString& Name, uint32 Version, TArray<uint8>& Out);
	void CacheSaveBlob(const FString& Name, uint32 Version, TArray<uint8>&& Blob);

	// ---- the shutdown gate ------------------------------------------------
	//
	// The core is a raw libbf6 context, and the module closes it in
	// ShutdownModule. Every catalogue worker in this add-on runs on a thread of
	// its own holding that bare bf6_ctx*, and nothing used to join them: closing
	// the editor while previews or the placed-object catalogue were mounting
	// left a worker inside a context that had just been freed, or inside a
	// bf6_core.dll the module manager had already unloaded.
	//
	// So there are two halves and both are needed. BeginCoreShutdown latches the
	// gate, and every path that would START core work asks CoreShuttingDown
	// first, because joining a worker that is free to spawn another is a race
	// with no end. Then each owner's JoinCoreWorkers blocks until its own
	// futures are done. Only after both is GCore.Close safe.
	//
	// Callable from any thread; the flag is atomic and never clears - a module
	// that has begun shutting down does not come back.
	void BeginCoreShutdown();
	bool CoreShuttingDown();

	// The last build produced something (the flag the mode pass gates on).
	bool BuiltAnything();

	// The mode ladder: 0 Low-Poly, 1 Clay, 2 Textured. Mirrors EMode in
	// BF6HighPoly.cpp; kept as an int so the enum stays file-local there.
	int32 Mode();

	// NOTHING THE ADD-ON DRAWS IS A CLICK TARGET.
	//
	// Every mesh this add-on puts in the world is a VIEW of something the SDK
	// already owns - the scenery it built, the prop it dressed, a preview. The
	// creator's object is the SDK's, so a click has exactly one right answer
	// and it is never the overlay. Left as selectable, the high-poly geometry
	// sits in front of the thing it is standing in for and takes the click:
	// right-clicking scenery selected a transient component that cannot be
	// moved, saved or exported, and the context menu that opened belonged to
	// nothing the creator could act on.
	//
	// Call on every primitive the add-on creates, before or after registering.
	void MakeUnselectable(::UPrimitiveComponent* C);

	// The study-grey material Clay mode wears.
	UMaterialInterface* ClayMaterial();

	// THE PARENT MATERIALS ARE NOT LEVEL WORK, SO THEY SHOULD NOT BE IN THE
	// LEVEL'S BUDGET.
	//
	// There are seven of them and each is an expression graph assembled through
	// the editor API and then compiled. Built on demand they were discovered one
	// at a time in the middle of dressing a scene - measured on MP_Aftermath as
	// 18.8 seconds of game thread spread across the whole load, with gaps of up
	// to 9.2 seconds between two of them.
	//
	// Nothing about them depends on which map is open, so they are built once
	// when the add-on starts and the level open finds them already there.
	// Returns how many it built (zero when they were all there already).
	int32 WarmParentMaterials();

	// Where the material seconds went since the last cost reset: building the
	// shared parents, working out which material a section wants, and making
	// the instances. Plus how many sections asked and how many instances that
	// actually came to. Read by the placed report, because "3,000 sections
	// asked" and "400 materials were made" are very different problems.
	void MaterialCost(double& OutParents, double& OutKeys, double& OutInstances,
	                  double& OutGlow, int32& OutGlowMisses,
	                  int32& OutHits, int32& OutMisses);

	// Sections (already in prefab-local GAME space) to a renderable UStaticMesh
	// with the add-on's real materials bound per section. Built through the
	// runtime render path: immediately drawable, no batch build, no Nanite.
	// Outer is the transient package; the caller roots it if it must outlive
	// the next GC. Null on failure (logged).
	UStaticMesh* BuildGameMesh(const FString& Name, const TArray<FCore::FSection>& Sections,
	                           int32& OutTris);

	// THE SAME BUILD, IN THREE STAGES, SO TWO OF THEM CAN GO WIDE.
	//
	// BuildGameMesh does all three in a row on the game thread, and measured on
	// a 192 type scene that is 10.3 s describing geometry, 10.7 s making the
	// object and binding its materials, and 5.1 s building render buffers.
	// Describing and building render buffers are worker-safe - the scenery
	// build has run both in a ParallelFor since it was written - and the middle
	// one is not, so the caller batches: describe every pending type across
	// cores, create the objects one at a time, then commit across cores again.
	//
	// FMeshWork is opaque on purpose: the FMeshDescription it carries belongs to
	// BF6HighPoly.cpp and nothing else needs to see it.
	struct FMeshWork;

	// ANY THREAD. Null when the sections describe no geometry.
	TSharedPtr<FMeshWork> DescribeGameMesh(const TArray<FCore::FSection>& Sections);

	// GAME THREAD. The UObject, its material slots and the BodySetup that must
	// exist before a worker may build render data into it.
	UStaticMesh* CreateGameMeshObject(const FString& Name,
	                                  const TArray<FCore::FSection>& Sections,
	                                  const TSharedPtr<FMeshWork>& Work);

	// ANY THREAD, after CreateGameMeshObject returned non-null for this Work.
	bool CommitGameMesh(UStaticMesh* Mesh, const TSharedPtr<FMeshWork>& Work, int32& OutTris);

	// The three halves of that call, in seconds since the last reset:
	// describing the geometry, making the UObject and its material slots, and
	// building the render buffers. Two of the three are worker-safe; the middle
	// one is not, so knowing the split is what decides whether threads help.
	void BuildGameMeshCost(double& OutDescribe, double& OutObject, double& OutMaterial,
	                       double& OutRender);
	void ResetBuildGameMeshCost();

	// ONE LIGHT FROM ONE DECODED RECORD, the same constructor the map's lights
	// go through: lumens conversion, cone halving, beam along minus forward,
	// shadows off, the lumen ceiling and the shared photometric scale. The
	// record is in GAME space relative to Parent's frame and the component is
	// placed with a RELATIVE transform, so a prefab-local fixture lands on the
	// prop that carries it. Null when the record is dark (zero energy or reach)
	// or the component could not be made. OutAuthoredLm is the pre-clamp power.
	ULightComponent* MakeAssetLight(AActor* Owner, USceneComponent* Parent,
	                                const FCore::FLight& L, int32 Index,
	                                const TCHAR* NamePrefix, float& OutAuthoredLm);

	// Show or hide every local light the LAST BUILD placed (the lamps, spots,
	// tubes and panels; not the sun or the sky). Returns how many it touched.
	int32 SetMapLocalLightsVisible(bool bOn);
}

namespace Placed
{
	// The placed-object module's pills for the HIGH POLY sub-ring. Called by
	// the ring builder in BF6HighPoly.cpp so the entries sit beside the mode
	// and layer toggles rather than on a second wheel.
	void AddPieEntries(TArray<BF6Ext::FPieSubEntry>& R);

	// The same contributions described as controls, so the panel can draw a
	// switch as a switch rather than as a button that flips one.
	void AddControls(TArray<FControl>& R);

	// Block until this module's full-catalogue mount worker has left the core.
	// Called by ShutdownModule before GCore.Close; see the shutdown gate above.
	// Returns at once when no worker is running.
	void JoinCoreWorkers();
}
}
