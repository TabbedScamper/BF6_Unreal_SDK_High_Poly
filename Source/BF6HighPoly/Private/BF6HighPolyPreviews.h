// BF6HighPolyPreviews - the object library's pictures, drawn from the game.
//
// The tool's library cards show the SDK's white blockout of each placeable.
// This service renders the REAL object - the pf_portal_<name> prefab assembled
// out of the install, with its decoded materials - into a 256 px PNG per
// placeable, and hands the tool a brush for it through the thumbnail seam.
// Ported from the Godot plugin's highpoly_previews.gd, and it keeps that
// file's laws:
//
//  - THE CAMERA IS PART OF THE PICTURE. A true isometric down the (1,1,1)
//    axis, orthographic, framed to the box's eight projected corners with a
//    hairline of air. Every icon shares one angle and one scale rule, which is
//    what makes a grid of them readable.
//  - THE DETAIL MODE IS PART OF THE PICTURE. Clay mode gets clay icons, so the
//    card shows what dropping the item will actually put in the scene.
//  - RENDERING IS THE BUILD'S JOB AND NOBODY ELSE'S. Serving an icon costs a
//    map lookup or one PNG read. Making a new one happens only inside
//    BuildAll, one object per tick, so the editor never hitches for a picture
//    nobody asked for.
//  - THE KEY CARRIES EVERYTHING THAT CHANGES THE PICTURE: the install
//    signature (a patch invalidates), the object name, the mode, and an icon
//    epoch bumped whenever the composition rule changes.
//
// Self-registering: a static initializer defers to OnPostEngineInit, so
// BF6HighPoly.cpp does not have to know this file exists. What it DOES need
// from that file is exposed by the BF6HP bridge below and supplied by the
// integration patch (scratchpad/integration/previews).
#pragma once

#include "BF6HighPolyControls.h"   // FControl: what the panel and the radial both render

#include "CoreMinimal.h"
#include "BF6SDKExtension.h"

class UStaticMesh;
class UMaterialInterface;
struct FSlateBrush;

namespace BF6HP
{
	// ---- the shared contract ---------------------------------------------
	//
	// Declared identically in BF6HighPolyShared.h (another change proposes
	// that header); C++ permits the redeclaration and the integrator keeps
	// one. One decoded game mesh, built with the add-on's real materials,
	// READY TO RENDER: no pending Nanite batch build, because a thumbnail is
	// captured the same tick the mesh is made. Null when the install cannot
	// supply it.
	UStaticMesh* BuildGameMesh(const FString& MeshRes, UObject* Outer);

	// ---- what the previews need from BF6HighPoly.cpp -----------------------
	//
	// One member of an assembled Portal object: a mesh resource and where it
	// sits in the OBJECT's space, already in Unreal units (Z-up, centimetres).
	struct FObjectPart
	{
		FString    MeshRes;
		FString    Bundle;
		FString    Variation;
		FTransform LocalToObject;
	};

	// The prefab behind an SDK placeable, resolved with the Godot plugin's name
	// law (pf_portal_<name>, bare, _a variant, then a unique art-category
	// prefix) and walked with an identity root so parts come back object-local.
	// True with an EMPTY Out means the install has no geometry for the type -
	// gameplay logic such as spawners and capture points - which is not an
	// error. False means the core refused, with the reason in OutError.
	bool PortalObjectParts(const FString& Type, TArray<FObjectPart>& Out, FString& OutError);

	// The derived cache configured for the chosen install, so its signature can
	// key thumbnails BEFORE the core is opened (serving needs no decode).
	bool PreviewsPrepareCache();
	// The core open on the chosen install. Same path StartRead takes.
	bool PreviewsEnsureCore(FString& OutError);
	// Every level's archives mounted so the WHOLE placeable catalogue resolves.
	// OFF by default now: this is the same reader the open map is drawn
	// through, so widening it for a thumbnail changes what the map renderer can
	// see, and it costs about 20 seconds. Returns false with an explanation
	// unless BF6.HighPoly.Previews.FullCatalogue has been turned on.
	bool PreviewsMountCatalogue(FString& OutError);
	// Whether that widening is allowed. Read and written by the console command.
	bool& PreviewsFullCatalogueFlag();
	// The current detail mode's skin.
	bool PreviewsIsClay();
	UMaterialInterface* PreviewsClayMaterial();

	// THE MODE LADDER ITSELF: 0 Low-Poly, 1 Clay, 2 Textured, the same int the
	// placed-object module reads. The icons follow the VIEWPORT'S rule - high
	// poly is "not 0" - and a card must never claim a detail the scene is not
	// showing, so the previews need the whole ladder and not just "is it clay".
	int32 PreviewsMode();
}

namespace BF6HPPreviews
{
	// The brush for a placeable's card, or null when there is none - the tool
	// then shows its own low-poly thumbnail. Cheap: it is called from the
	// library's paint path.
	//
	// Null on purpose in Low-Poly mode: the card shows what dropping the item
	// puts in the scene, and in that mode the scene shows the tool's blockout.
	const FSlateBrush* ThumbFor(const FString& Type);

	// What that brush IS, for the label the library prints on the card:
	// "high poly", "clay", "textured icon", "low poly". Empty only before the
	// install is known. A map lookup after the first ask for a name.
	FString Detail(const FString& Type);

	// Render every missing icon for the open map's catalogue, one object per
	// tick, with something to watch. Progress is called as (done, total) on the
	// game thread; the module owns no UI. Returns false with a reason when a
	// build is already running or the install is not available.
	bool BuildAll(TFunction<void(int32, int32)> Progress, FString& OutError);
	void CancelBuild();
	bool IsBuilding();
	// Block until the mount worker this module started has left the core. The
	// add-on's ShutdownModule calls it before closing the context; see the
	// shutdown gate in BF6HighPolyShared.h. Returns at once when idle.
	void JoinCoreWorkers();
	// True while the catalogue mount or a build is using the core.
	bool IsBusy();

	// Drop every cached picture, in memory and on disk, for every install.
	int32 Clear();

	// How many icons the current mode is missing, without rendering any.
	// Counts names with no PNG; it does not test whether the install could
	// draw them, because that is the build's question.
	int32 MissingCount();

	// One line for a ring label or the console: "312 / 4620", "all built",
	// "1204 missing", "no map open".
	FString StatusLine();

	// The PREVIEWS pill for the HIGH POLY sub-ring. Called by the ring builder
	// in BF6HighPoly.cpp (integration patch) with the array it is filling.
	void AddPieEntries(TArray<BF6Ext::FPieSubEntry>& Ring);

	// The same, as controls the panel can draw properly rather than as pills.
	void AddControls(TArray<FControl>& R);
}
