// BF6HighPolyPreviews - the object library's pictures, drawn from the game.
// See the header for the laws; this file is the machinery.

#include "BF6HighPolyPreviews.h"
#include "BF6HighPolyControls.h"

#include "BF6SDKExtension.h"
#include "BF6HighPolyDiskCache.h"
#include "BF6HighPolyShared.h"   // the core's shutdown gate

#include "Async/Async.h"
#include "Camera/CameraTypes.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "Styling/SlateBrush.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogBF6HighPolyPreviews, Log, All);

// ---- the seam additions this file relies on ---------------------------------
//
// These three are ADDED to Public/BF6SDKExtension.h by the integration patch
// (03_seam_thumbnail_provider.py). Redeclaring them here is legal C++ and lets
// this file compile before the patch lands; linking without the patch fails
// loudly on exactly these names, which is the right way for a missing seam to
// fail. Remove this block once the header carries them (ApiVersion >= 6).
namespace BF6Ext
{
	BF6UNREALSDK_API void RegisterThumbnailProvider(FName Id,
		TFunction<const FSlateBrush*(const FString& /*Type*/)> Provider);
	BF6UNREALSDK_API void UnregisterThumbnailProvider(FName Id);
	BF6UNREALSDK_API void RegisterThumbnailDetail(FName Id,
		TFunction<FString(const FString& /*Type*/)> Detail);
	BF6UNREALSDK_API void PlaceableTypes(TArray<FString>& Out);
}

namespace
{
	// 256 px, the size the tool's own cards are rendered at, so a high-poly
	// picture and a low-poly one sit side by side without a visible step.
	const int32 kThumbSize = 256;

	// THE CAMERA IS PART OF THE PICTURE, so it is part of the file name. Bump
	// on any change to how an icon is composed, or every user keeps the old
	// pictures for ever while the build reports success over a folder it never
	// rewrote. Continues the Godot plugin's numbering: 2 is the orthographic
	// true isometric framed to the projected box.
	const int32 kIconEpoch = 3; // Material palette colours and variation-scope recovery.

	// (1, 1, 1) is the isometric axis proper: equal foreshortening on all three
	// axes, the angle a parts catalogue uses and the reason the result reads
	// as a product shot rather than a photograph. The Godot plugin's constant
	// is (1, 1, 1) in a Y-up frame; swapping Y and Z leaves it unchanged.
	const FVector kIsoDir = FVector(1.0, 1.0, 1.0);
	const float   kIsoMargin = 1.06f;   // a hairline of air, so nothing touches the edge

	// THE LIGHTS, carried over from the Godot rig and converted to Z-up.
	//
	// Godot: sun rotation_degrees (-45, -30, 0) at energy 1.2, fill (-20, 140,
	// 0) at 0.4, both pointing down their local -Z. Applying those Eulers to
	// (0, 0, -1) gives travel directions of (0.354, -0.707, -0.612) and
	// (-0.604, -0.342, 0.720) in Godot's (right, up, toward-viewer) frame; the
	// same Y/Z swap the geometry gets puts them in Unreal's. The key comes over
	// the object's top-left shoulder as the camera sees it, the fill lifts the
	// face the key leaves dark. Intensities keep the 3:1 ratio against the
	// preview world's default of pi.
	const FVector kKeyTravel  = FVector( 0.354, -0.612, -0.707);
	const FVector kFillTravel = FVector(-0.604,  0.720, -0.342);
	const float   kKeyBrightness  = UE_PI * 1.2f;
	const float   kFillBrightness = UE_PI * 0.4f;

	const TCHAR* kProviderId = TEXT("HighPoly.Previews");

	struct FRig
	{
		TUniquePtr<FPreviewScene>            Scene;
		USceneCaptureComponent2D*            Capture = nullptr;
		UTextureRenderTarget2D*              RT = nullptr;
		UDirectionalLightComponent*          Fill = nullptr;
	};

	// How long one tick may spend drawing pictures. Big enough that cached
	// entries stream past, small enough that the editor keeps its frame.
	float GPreviewBudgetMs = 10.f;

	struct FBuild
	{
		bool                                 bRunning = false;
		bool                                 bMounting = false;
		bool                                 bCancel = false;
		TArray<FString>                      Queue;
		// How far into Queue this tick has got. The consumed head is dropped
		// once at the end of the tick instead of one element at a time.
		int32                                Cursor = 0;
		int32                                Total = 0, Done = 0;
		int32                                Made = 0, Cached = 0, Skipped = 0, Failed = 0;
		double                               T0 = 0.0;
		TFunction<void(int32, int32)>        Progress;
		TFuture<FString>                     Mount;    // empty string = mounted
		FTSTicker::FDelegateHandle           Tick;
	};

	struct FState
	{
		FRig    Rig;
		FBuild  Build;

		// The picture cache, in memory. Keyed by NAME only, so it is dropped
		// wholesale when the mode or the install changes; the on-disk cache is
		// per mode and per install, so that costs PNG reads, not renders.
		FString                                   Tag;         // mode tag the cache holds
		int32                                     ModeHeld = -1;  // ladder position it was filled at
		FString                                   Signature;   // install the cache holds
		TMap<FString, TSharedPtr<FSlateBrush>>    Brushes;
		TArray<TStrongObjectPtr<UTexture2D>>      Held;        // keep brush textures alive

		// Names written off, so they are not re-statted from disk on every
		// paint for the rest of the session. Two sets because they answer two
		// questions: NoPng is "no picture for this tag yet" and is cleared
		// when the tag or the install changes; NoSource is "the install cannot
		// draw this at all" and is cleared only by Clear().
		TSet<FString>                             NoPng;
		TSet<FString>                             NoSource;

		// WHAT EACH CARD IS ACTUALLY SHOWING, so the library can say so without
		// asking a second question that could answer differently. Set by the
		// one ask that decided the brush, and dropped with the brushes.
		//   1 the current mode's own picture
		//   2 the other high-poly mode's picture, because this mode's is not built
		//   3 no picture from the game at all; the tool draws its blockout
		TMap<FString, uint8>                      Detail;

		// The last time a card had to fall back, and why. One line, so the
		// console can answer "why is this still low poly" without a rebuild.
		FString                                   LastFallback;

		// The missing count is read live by the ring's label, every frame the
		// wheel is open, and it stats a file per name. Recomputed on a 2 s
		// clock (the Godot timer's period) or when something invalidates it.
		int32   Missing = -1;
		double  MissingAt = 0.0;
		FString MissingFor;    // tag + signature + level it was counted for

		double  SignatureTriedAt = 0.0;
		FString LastSummary;
		bool    bRegistered = false;
	};
	FState GS;

	// ---- keys ---------------------------------------------------------------

	// THE LIBRARY FOLLOWS THE VIEWPORT'S RULE, which is the placed-object
	// module's: high poly is mode != 0. In Low-Poly mode the scene shows the
	// tool's white blockout, so the card shows the tool's white blockout, and
	// the provider answers null for everything.
	bool IsHighPolyMode() { return BF6HP::PreviewsMode() != 0; }

	// The two skins a high poly icon can be rendered in. Clay mode gets clay
	// icons, textured mode gets textured ones: the icon shows what dropping the
	// item puts in the scene. "_game" records which store the picture came
	// from, as the Godot plugin's tag does: this one is always assembled from
	// the install.
	FString TagFor(bool bClay)
	{
		return FString::Printf(TEXT("%s%d_game"), bClay ? TEXT("clay") : TEXT("tex"), kIconEpoch);
	}

	FString ModeTag()  { return TagFor(BF6HP::PreviewsMode() == 1); }
	// The OTHER skin, which is what a card falls back to rather than dropping
	// all the way to the blockout. A clay icon and a textured icon are the same
	// object from the same camera, so showing the built one answers "what is
	// this thing" - the question the card exists for - while the missing one is
	// still only a build away. Without this, switching to Clay turned a fully
	// built library back into a wall of white boxes with nothing to say why.
	FString OtherTag() { return TagFor(BF6HP::PreviewsMode() != 1); }

	// Thumbnails live beside the derived cache's blobs, under the same install
	// signature, so a game patch retires them with everything else and
	// BF6.HighPoly.CacheClear's neighbour Previews clear finds them by path.
	FString DerivedRoot()
	{
		return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("HighPoly"), TEXT("derived"));
	}

	FString ThumbDir(const FString& Signature)
	{
		return FPaths::Combine(DerivedRoot(), Signature, TEXT("thumbs"));
	}

	FString ThumbPathTagged(const FString& Type, const FString& Tag)
	{
		return ThumbDir(GS.Signature) / (FPaths::MakeValidFileName(Type) + TEXT("__") + Tag + TEXT(".png"));
	}

	FString ThumbPath(const FString& Type) { return ThumbPathTagged(Type, GS.Tag); }

	// The install signature, once the add-on knows where the game is. Serving
	// needs no decode, so the core is not opened for it; the derived cache is
	// configured, which hashes the install's toc set once per session. Retried
	// on a 2 s clock while unknown, so a fresh install choice is picked up.
	bool EnsureSignature()
	{
		if (!GS.Signature.IsEmpty()) return true;
		const double Now = FPlatformTime::Seconds();
		if (Now - GS.SignatureTriedAt < 2.0) return false;
		GS.SignatureTriedAt = Now;
		if (!BF6HP::PreviewsPrepareCache()) return false;
		GS.Signature = BF6HP::DiskCache::InstallSignature();
		return !GS.Signature.IsEmpty();
	}

	void DropMemoryCache()
	{
		GS.Brushes.Reset();
		GS.Held.Reset();
		GS.NoPng.Reset();
		GS.Detail.Reset();
		GS.Missing = -1;
	}

	// A mode switch or an install change means every icon should look
	// different. Cheap to call: two string compares when nothing changed.
	void SyncKeys()
	{
		// THE MODE MOVING IS ENOUGH ON ITS OWN. Low-Poly and Textured share a
		// tag (a textured icon is the right picture for both once high poly is
		// back on), so keying only on the tag left the library labelling cards
		// for a mode it was no longer in, and left a fallback decision standing
		// that the new mode would have taken differently. A mode switch is a
		// deliberate, rare act; re-deciding every visible card once is free.
		const int32 M = BF6HP::PreviewsMode();
		if (M != GS.ModeHeld)
		{
			GS.ModeHeld = M;
			GS.Detail.Reset();
			GS.NoPng.Reset();
			GS.Missing = -1;
		}
		const FString Tag = ModeTag();
		if (Tag != GS.Tag)
		{
			GS.Tag = Tag;
			DropMemoryCache();
		}
		const FString Before = GS.Signature;
		if (GS.Signature.IsEmpty()) EnsureSignature();
		if (GS.Signature != Before) DropMemoryCache();
	}

	// PNG on disk -> live texture + brush, or null.
	TSharedPtr<FSlateBrush> BrushFromPng(const FString& Path)
	{
		UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(Path);
		if (!Tex) return nullptr;
		GS.Held.Add(TStrongObjectPtr<UTexture2D>(Tex));
		TSharedPtr<FSlateBrush> Brush = MakeShared<FSlateBrush>();
		Brush->SetResourceObject(Tex);
		Brush->ImageSize = FVector2D(kThumbSize, kThumbSize);
		return Brush;
	}

	// ---- the rig --------------------------------------------------------------

	bool EnsureRig()
	{
		FRig& R = GS.Rig;
		if (R.Scene.IsValid()) return true;

		R.Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(true)
			.SetLightRotation(kKeyTravel.Rotation())
			.SetLightBrightness(kKeyBrightness)
			.SetSkyBrightness(1.0f));

		R.Fill = NewObject<UDirectionalLightComponent>(GetTransientPackage());
		R.Fill->Intensity = kFillBrightness;
		R.Fill->SetCastShadows(false);
		R.Fill->bAffectsWorld = true;
		R.Scene->AddComponent(R.Fill, FTransform(kFillTravel.Rotation()));

		R.RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		R.RT->AddToRoot();
		R.RT->RenderTargetFormat = RTF_RGBA8;
		R.RT->InitAutoFormat(kThumbSize, kThumbSize);

		R.Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
		R.Capture->CaptureSource = SCS_FinalColorLDR;
		R.Capture->TextureTarget = R.RT;
		R.Capture->bCaptureEveryFrame = false;
		R.Capture->bCaptureOnMovement = false;
		R.Capture->ProjectionType = ECameraProjectionMode::Orthographic;
		// Fixed planes. The automatic pair is derived from OrthoWidth, and a
		// long thin object (a fence) has a small width and a deep extent along
		// the isometric axis, which is exactly the case that gets clipped.
		R.Capture->bAutoCalculateOrthoPlanes = false;
		R.Capture->bUpdateOrthoPlanes = false;
		R.Scene->AddComponent(R.Capture, FTransform::Identity);
		return true;
	}

	void ReleaseRig()
	{
		FRig& R = GS.Rig;
		if (R.RT) { R.RT->RemoveFromRoot(); R.RT = nullptr; }
		R.Capture = nullptr;
		R.Fill = nullptr;
		R.Scene.Reset();
	}

	// A TRUE ISOMETRIC, FRAMED TO THE OBJECT.
	//
	// Orthographic takes the field of view out of the question, and the extent
	// is measured by projecting the eight corners of the box into camera space
	// rather than by a sphere, so a fence is framed as a fence instead of as
	// the ball that would contain it. The camera sits one box diagonal back
	// along the axis, which an orthographic projection does not care about
	// except that the near plane must not sit inside the object.
	void FrameIsometric(const FBox& Box)
	{
		USceneCaptureComponent2D* Cam = GS.Rig.Capture;
		const FVector Center = Box.GetCenter();
		const FVector Dir = kIsoDir.GetSafeNormal();
		const double Span = FMath::Max(Box.GetSize().Size(), 0.1);
		const FRotator Rot = (-Dir).Rotation();
		Cam->SetWorldLocationAndRotation(Center + Dir * Span, Rot);

		// The box's true extent as this camera sees it: the matrix's Y and Z
		// axes are the camera's right and up in world space, so a dot with each
		// gives the half extent along that screen axis.
		const FRotationMatrix M(Rot);
		const FVector Right = M.GetScaledAxis(EAxis::Y);
		const FVector Up    = M.GetScaledAxis(EAxis::Z);
		double Hx = 0.0, Hy = 0.0;
		for (int32 i = 0; i < 8; i++)
		{
			const FVector C(
				(i & 1) ? Box.Max.X : Box.Min.X,
				(i & 2) ? Box.Max.Y : Box.Min.Y,
				(i & 4) ? Box.Max.Z : Box.Min.Z);
			const FVector Rel = C - Center;
			Hx = FMath::Max(Hx, FMath::Abs(FVector::DotProduct(Rel, Right)));
			Hy = FMath::Max(Hy, FMath::Abs(FVector::DotProduct(Rel, Up)));
		}
		// OrthoWidth is the horizontal extent of a square target, so the wider
		// of the two axes has to drive it or a long object is cropped.
		Cam->OrthoWidth = (float)FMath::Max(FMath::Max(Hx, Hy) * 2.0 * kIsoMargin, 1.0);
	}

	bool CaptureTo(const FString& PngPath)
	{
		FRig& R = GS.Rig;
		R.Capture->CaptureScene();
		TArray<FColor> Pixels;
		FTextureRenderTargetResource* Res = R.RT->GameThread_GetRenderTargetResource();
		if (!Res || !Res->ReadPixels(Pixels)) return false;
		for (FColor& C : Pixels) C.A = 255;
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(kThumbSize, kThumbSize, Pixels, Png);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(PngPath), true);
		return FFileHelper::SaveArrayToFile(Png, *PngPath);
	}

	// ---- one picture --------------------------------------------------------

	// Make the PNG for one placeable. Counts into the build's totals; the
	// caller has already checked that no PNG exists for this tag.
	void RenderOne(const FString& Type)
	{
		FBuild& B = GS.Build;
		if (GS.NoSource.Contains(Type)) { B.Skipped++; return; }

		TArray<BF6HP::FObjectPart> Parts;
		FString Err;
		if (!BF6HP::PortalObjectParts(Type, Parts, Err))
		{
			B.Failed++;
			UE_LOG(LogBF6HighPolyPreviews, Warning, TEXT("%s: %s"), *Type, *Err);
			return;
		}
		if (Parts.Num() == 0)
		{
			// Gameplay logic with no geometry, correctly absent. Not a failure.
			GS.NoSource.Add(Type);
			B.Skipped++;
			return;
		}

		EnsureRig();
		UMaterialInterface* Clay = BF6HP::PreviewsIsClay() ? BF6HP::PreviewsClayMaterial() : nullptr;

		// THE SAME BUILD THE OVERLAY MAKES for a placed object, so the icon and
		// the thing you drop come out of one path and cannot disagree.
		TArray<UStaticMeshComponent*> Comps;
		FBox Box(ForceInit);
		for (const BF6HP::FObjectPart& P : Parts)
		{
			UStaticMesh* SM = BF6HP::BuildGameMesh(P.MeshRes, GetTransientPackage());
			if (!SM) continue;
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(GetTransientPackage());
			C->SetStaticMesh(SM);
			BF6HP::Shared::MakeUnselectable(C);
			C->SetMobility(EComponentMobility::Movable);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			// Skin the render the way the scene is skinned.
			if (Clay)
				for (int32 i = 0; i < C->GetNumMaterials(); i++) C->SetMaterial(i, Clay);
			GS.Rig.Scene->AddComponent(C, P.LocalToObject);
			Box += C->CalcBounds(P.LocalToObject).GetBox();
			Comps.Add(C);
		}

		bool bOk = false;
		if (Comps.Num() > 0 && Box.IsValid && Box.GetSize().Size() > 0.01)
		{
			FrameIsometric(Box);
			bOk = CaptureTo(ThumbPath(Type));
		}
		for (UStaticMeshComponent* C : Comps) GS.Rig.Scene->RemoveComponent(C);

		if (!bOk)
		{
			B.Failed++;
			UE_LOG(LogBF6HighPolyPreviews, Warning,
				TEXT("%s: %d part(s) resolved, %d built, nothing captured"), *Type, Parts.Num(), Comps.Num());
			return;
		}
		B.Made++;
		// Serve it from memory from now on: the card is probably on screen.
		GS.NoPng.Remove(Type);
		if (TSharedPtr<FSlateBrush> Brush = BrushFromPng(ThumbPath(Type)))
		{
			GS.Brushes.Add(Type, Brush);
			GS.Detail.Add(Type, 1);
		}
	}

	// ---- the build --------------------------------------------------------------

	void FinishBuild(const FString& Why)
	{
		FBuild& B = GS.Build;
		if (B.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(B.Tick); B.Tick.Reset(); }
		const double Secs = FPlatformTime::Seconds() - B.T0;
		GS.LastSummary = Why.IsEmpty()
			? FString::Printf(TEXT("%d rendered, %d already cached, %d without geometry, %d failed, %d total in %.1f s%s"),
				B.Made, B.Cached, B.Skipped, B.Failed, B.Total, Secs, B.bCancel ? TEXT(" (stopped)") : TEXT(""))
			: Why;
		UE_LOG(LogBF6HighPolyPreviews, Display, TEXT("previews: %s"), *GS.LastSummary);
		B.bRunning = false;
		B.bMounting = false;
		B.Queue.Reset();
		B.Progress = nullptr;
		GS.Missing = -1;
		// Every card decides again on its next paint: a build is exactly the
		// event that can turn a stand-in picture into this mode's own.
		GS.Detail.Reset();
	}

	bool TickBuild(float)
	{
		FBuild& B = GS.Build;
		if (!B.bRunning) return false;

		if (B.bMounting)
		{
			if (!B.Mount.IsReady()) return true;
			const FString Err = B.Mount.Get();
			B.bMounting = false;
			if (!Err.IsEmpty())
			{
				FinishBuild(FString::Printf(TEXT("could not read the install: %s"), *Err));
				BF6Ext::Notify(FString::Printf(TEXT("High Poly previews: %s"), *Err));
				return false;
			}
			// The mount may have taken a minute; keys can have moved meanwhile.
			SyncKeys();
			if (GS.Signature.IsEmpty())
			{
				FinishBuild(TEXT("the install signature is unknown, nothing to key the pictures by"));
				return false;
			}
		}

		if (B.bCancel || B.Queue.Num() == 0)
		{
			FinishBuild(FString());
			BF6Ext::Notify(FString::Printf(TEXT("High Poly previews: %s"), *GS.LastSummary));
			return false;
		}

		// A TIME BUDGET PER TICK, NOT ONE OBJECT PER TICK.
		//
		// Assembling an object out of the install and capturing it is
		// main-thread work, so this has to yield - but yielding after exactly
		// one object made the run cost one FRAME per placeable no matter how
		// little that placeable cost. A catalogue of 4,600 therefore took 77
		// seconds at 60 fps in the best imaginable case, and the common case is
		// worse than the best: most entries are already on disk and their whole
		// cost is a FileExists, so the run was spending almost all of its time
		// waiting for the next frame between stat() calls.
		//
		// Now it works until the budget is spent. Cheap cached entries stream
		// past hundreds per tick; a genuinely expensive render still ends the
		// tick on its own, because the budget is checked after each one and one
		// object is always allowed through. Same responsiveness, minus the
		// waiting.
		const double Deadline = FPlatformTime::Seconds() + (double)GPreviewBudgetMs * 0.001;
		do
		{
			const FString Type = B.Queue[B.Cursor++];
			if (FPaths::FileExists(ThumbPath(Type)))
			{
				B.Cached++;
				GS.NoPng.Remove(Type);
				// It was probably being served from the other mode's picture;
				// make the card show this mode's the next time it paints.
				GS.Brushes.Remove(Type);
				GS.Detail.Remove(Type);
			}
			else
			{
				RenderOne(Type);
			}
			B.Done++;
		}
		while (B.Cursor < B.Queue.Num() && !B.bCancel
			&& FPlatformTime::Seconds() < Deadline);

		// Drop what has been consumed in one go, rather than RemoveAt(0) per
		// item: that is a whole-array move each time, and on a 4,600 entry
		// catalogue it is quadratic for no reason.
		if (B.Cursor > 0)
		{
			B.Queue.RemoveAt(0, B.Cursor, EAllowShrinking::No);
			B.Cursor = 0;
		}
		if (B.Progress) B.Progress(B.Done, B.Total);
		return true;
	}

	// ---- the missing count --------------------------------------------------

	int32 CountMissing()
	{
		if (!EnsureSignature()) return -1;
		TArray<FString> Names;
		BF6Ext::PlaceableTypes(Names);
		int32 N = 0;
		for (const FString& T : Names)
		{
			if (GS.NoSource.Contains(T)) continue;
			// Only THIS mode's own picture counts as built. A card being served
			// the other mode's icon is exactly what a build here would replace,
			// so counting it as done would report "all built" over a library
			// that is still wearing the wrong skin.
			const uint8* D = GS.Detail.Find(T);
			if (D && *D == 1) continue;
			if (!FPaths::FileExists(ThumbPath(T))) N++;
		}
		return N;
	}

	// WHAT THE LIBRARY IS SHOWING RIGHT NOW, counted over the open map's list.
	// One disk stat per name, so this is for the console and the ring's own
	// pill, never for a paint.
	struct FServing
	{
		int32 Total = 0;      // types the library is listing
		int32 Exact = 0;      // this mode's own picture
		int32 Other = 0;      // the other mode's picture, standing in
		int32 Blockout = 0;   // no picture from the game; the tool's own thumbnail
		int32 NoGeometry = 0; // of those, the ones known to have none to draw
	};

	FServing Summarise()
	{
		FServing S;
		if (!EnsureSignature()) return S;
		TArray<FString> Names;
		BF6Ext::PlaceableTypes(Names);
		const FString Other = OtherTag();
		for (const FString& T : Names)
		{
			S.Total++;
			if (FPaths::FileExists(ThumbPath(T)))            { S.Exact++; continue; }
			if (FPaths::FileExists(ThumbPathTagged(T, Other))) { S.Other++; continue; }
			S.Blockout++;
			if (GS.NoSource.Contains(T)) S.NoGeometry++;
		}
		return S;
	}

	// ---- lifetime ---------------------------------------------------------------

	void Startup()
	{
		BF6Ext::RegisterThumbnailProvider(FName(kProviderId),
			[](const FString& Type) { return BF6HPPreviews::ThumbFor(Type); });
		// The label under the same Id: the tool prints it on the card, so a
		// creator can see whether they are looking at the real object, at the
		// other mode's picture standing in for it, or at the tool's blockout.
		BF6Ext::RegisterThumbnailDetail(FName(kProviderId),
			[](const FString& Type) { return BF6HPPreviews::Detail(Type); });
		GS.bRegistered = true;
		UE_LOG(LogBF6HighPolyPreviews, Log, TEXT("object library previews attached"));
	}

	// THE MOUNT WORKER HOLDS THE CORE, AND FinishBuild DOES NOT TOUCH IT.
	//
	// FinishBuild drops the ticker and clears FBuild, which threw the TFuture
	// away while its thread was still inside bf6_mount_all on a context the
	// module was about to close. Waiting is the only correct move: the worker
	// takes no lock this thread holds and touches no UObject, so it cannot
	// deadlock against a game thread that is simply blocked here, and the
	// alternative - closing under it - is a use-after-free.
	//
	// This is called from ShutdownModule as well as from Shutdown below,
	// because a plugin unload with the engine still running fires neither
	// OnEnginePreExit nor OnPreExit.
	void JoinMountWorker()
	{
		FBuild& B = GS.Build;
		if (!B.Mount.IsValid()) return;
		if (!B.Mount.IsReady())
		{
			UE_LOG(LogBF6HighPolyPreviews, Log,
				TEXT("previews: waiting for the catalogue mount to leave the core before it closes"));
		}
		B.Mount.Wait();
		B.Mount = TFuture<FString>();
		B.bMounting = false;
	}

	void Shutdown()
	{
		JoinMountWorker();
		if (GS.Build.bRunning) FinishBuild(TEXT("editor closing"));
		if (GS.bRegistered) { BF6Ext::UnregisterThumbnailProvider(FName(kProviderId)); GS.bRegistered = false; }
		ReleaseRig();
		DropMemoryCache();
	}

	// Self-registration: nothing in BF6HighPoly.cpp has to know this file
	// exists. The seam is a module export, so registering waits for the
	// engine rather than running at DLL load.
	struct FAutoRegister
	{
		FAutoRegister()
		{
			FCoreDelegates::OnPostEngineInit.AddStatic(&Startup);
			FCoreDelegates::OnEnginePreExit.AddStatic(&Shutdown);
		}
	} GAutoRegister;
}

// =============================================================================
// public surface
// =============================================================================
namespace BF6HPPreviews
{
	const FSlateBrush* ThumbFor(const FString& Type)
	{
		if (Type.IsEmpty() || Type.StartsWith(TEXT("block::"))) return nullptr;
		SyncKeys();
		// LOW-POLY MODE DRAWS THE TOOL'S PICTURE, because that is what the
		// viewport is drawing. The card and the scene answer to one rule.
		if (!IsHighPolyMode()) return nullptr;
		if (GS.Signature.IsEmpty()) return nullptr;   // no install chosen yet
		if (const TSharedPtr<FSlateBrush>* B = GS.Brushes.Find(Type)) return B->Get();
		if (GS.NoPng.Contains(Type) || GS.NoSource.Contains(Type)) return nullptr;
		// ASKED ONCE PER NAME PER TAG. A disk stat for every card on every paint
		// was the Godot plugin's single clearest piece of waste; the answer is
		// remembered either way, and a build clears the miss when it makes one.
		//
		// Two stats, not one: this mode's picture, then the other mode's. The
		// second is what keeps a mode switch from emptying the library.
		const FString Path = ThumbPath(Type);
		if (FPaths::FileExists(Path))
		{
			if (TSharedPtr<FSlateBrush> Brush = BrushFromPng(Path))
			{
				GS.Brushes.Add(Type, Brush);
				GS.Detail.Add(Type, 1);
				return Brush.Get();
			}
		}
		const FString Other = ThumbPathTagged(Type, OtherTag());
		if (FPaths::FileExists(Other))
		{
			if (TSharedPtr<FSlateBrush> Brush = BrushFromPng(Other))
			{
				GS.Brushes.Add(Type, Brush);
				GS.Detail.Add(Type, 2);
				GS.LastFallback = FString::Printf(
					TEXT("%s showed its %s icon: no %s icon is built for it yet (run BF6.HighPoly.Previews build in this mode)"),
					*Type, *OtherTag(), *GS.Tag);
				return Brush.Get();
			}
		}
		GS.NoPng.Add(Type);
		GS.Detail.Add(Type, 3);
		GS.LastFallback = GS.NoSource.Contains(Type)
			? FString::Printf(TEXT("%s fell back to the tool's blockout: the install has no geometry for it"), *Type)
			: FString::Printf(TEXT("%s fell back to the tool's blockout: no icon on disk for %s or %s"),
				*Type, *GS.Tag, *OtherTag());
		return nullptr;
	}

	FString Detail(const FString& Type)
	{
		if (Type.IsEmpty() || Type.StartsWith(TEXT("block::"))) return FString();
		SyncKeys();
		if (GS.Signature.IsEmpty()) return FString();   // nothing to say yet
		if (!IsHighPolyMode()) return TEXT("low poly");
		const uint8* D = GS.Detail.Find(Type);
		if (!D)
		{
			// The card asked for a label before it asked for a picture. Deciding
			// is one map lookup after the first ask, so decide now rather than
			// leave the label a frame behind the thing it describes.
			ThumbFor(Type);
			D = GS.Detail.Find(Type);
			if (!D) return FString();
		}
		const bool bClay = BF6HP::PreviewsMode() == 1;
		switch (*D)
		{
		case 1:  return bClay ? TEXT("clay") : TEXT("high poly");
		case 2:  return bClay ? TEXT("textured icon") : TEXT("clay icon");
		default: return TEXT("low poly");
		}
	}

	bool BuildAll(TFunction<void(int32, int32)> Progress, FString& OutError)
	{
		FBuild& B = GS.Build;
		if (B.bRunning) { OutError = TEXT("a preview build is already running"); return false; }
		// No new core work once the module has begun going down: the join below
		// in JoinCoreWorkers has to be able to finish, and it cannot if a mount
		// can still be started behind it.
		if (BF6HP::Shared::CoreShuttingDown())
		{
			OutError = TEXT("the editor is closing");
			return false;
		}

		TArray<FString> Names;
		BF6Ext::PlaceableTypes(Names);
		if (Names.Num() == 0) { OutError = TEXT("no map open, so there is no object library to draw"); return false; }
		Names.Sort();   // deterministic, so stopping and resuming covers the same ground in the same order

		SyncKeys();
		B = FBuild();
		B.bRunning = true;
		B.bMounting = true;
		B.Queue = MoveTemp(Names);
		B.Total = B.Queue.Num();
		B.T0 = FPlatformTime::Seconds();
		B.Progress = MoveTemp(Progress);
		// Everything written off as "no picture yet" is exactly what this was
		// pressed to make.
		GS.NoPng.Reset();

		// THE INSTALL, OPENED AND FULLY MOUNTED, OFF THE GAME THREAD. Opening
		// reads a 176 MB executable and mounting every level's archives is the
		// better part of a minute cold; the core touches no UObjects doing it.
		B.Mount = Async(EAsyncExecution::Thread, []() -> FString
		{
			FString Err;
			if (!BF6HP::PreviewsEnsureCore(Err)) return Err.IsEmpty() ? FString(TEXT("the install could not be opened")) : Err;
			if (!BF6HP::PreviewsMountCatalogue(Err)) return Err.IsEmpty() ? FString(TEXT("the catalogue could not be mounted")) : Err;
			return FString();
		});
		B.Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickBuild));
		UE_LOG(LogBF6HighPolyPreviews, Display, TEXT("previews: building %d icon(s) for %s"), B.Total, *GS.Tag);
		return true;
	}

	void CancelBuild() { if (GS.Build.bRunning) GS.Build.bCancel = true; }
	// Cancel first so the ticker stops queueing work, then block on the worker
	// that is actually inside the core. bCancel is not seen by the mount thread
	// itself - libbf6 has no cancel for a mount in progress - so the wait is the
	// whole of the guarantee, not a hint.
	//
	// FinishBuild last, and not only for the summary: it removes the build
	// ticker. This runs from ShutdownModule, so a ticker left registered here
	// would be a delegate into a module that is about to be unloaded.
	void JoinCoreWorkers()
	{
		CancelBuild();
		JoinMountWorker();
		if (GS.Build.bRunning) FinishBuild(TEXT("the editor is closing"));
	}
	bool IsBuilding()  { return GS.Build.bRunning; }
	bool IsBusy()      { return GS.Build.bRunning; }

	int32 Clear()
	{
		int32 N = 0;
		TArray<FString> Sigs;
		IFileManager::Get().FindFiles(Sigs, *(DerivedRoot() / TEXT("*")), false, true);
		for (const FString& S : Sigs)
		{
			const FString Dir = ThumbDir(S);
			TArray<FString> Pngs;
			IFileManager::Get().FindFiles(Pngs, *(Dir / TEXT("*.png")), true, false);
			N += Pngs.Num();
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
		}
		DropMemoryCache();
		// Whatever changed on disk is exactly the thing that could give a
		// source to a name that had none.
		GS.NoSource.Reset();
		return N;
	}

	int32 MissingCount()
	{
		SyncKeys();
		const FString For = GS.Tag + TEXT("|") + GS.Signature + TEXT("|") + BF6Ext::CurrentLevel();
		const double Now = FPlatformTime::Seconds();
		if (GS.Missing < 0 || For != GS.MissingFor || Now - GS.MissingAt > 2.0)
		{
			GS.Missing = CountMissing();
			GS.MissingAt = Now;
			GS.MissingFor = For;
		}
		return GS.Missing;
	}

	FString StatusLine()
	{
		const FBuild& B = GS.Build;
		if (B.bRunning)
		{
			if (B.bMounting) return TEXT("reading the install...");
			return FString::Printf(TEXT("%d / %d, pick to stop"), B.Done, B.Total);
		}
		if (BF6Ext::CurrentLevel().IsEmpty()) return TEXT("no map open");
		// Say WHY the library is white rather than leaving it to be discovered:
		// in Low-Poly mode the cards are the tool's blockout on purpose.
		if (!IsHighPolyMode()) return TEXT("low poly mode, cards show the blockout");
		const int32 M = MissingCount();
		if (M < 0)  return TEXT("choose the game folder first");
		if (M == 0) return TEXT("all built");
		return FString::Printf(TEXT("%d missing, pick to build"), M);
	}

	// An explicit start/stop action keeps expensive thumbnails out of map build.
	void AddControls(TArray<FControl>& Out)
	{
		FControl C;
		C.Label = TEXT("Library pictures");
		C.Tip = TEXT("Build missing game-model thumbnails for this map's object library. This can interrupt editing while pictures render. Press again to stop; completed pictures are kept.");
		C.Sub = [] { return GS.Build.bRunning
			? FString::Printf(TEXT("%d / %d; press to stop"), GS.Build.Done, GS.Build.Total)
			: FString(TEXT("optional; press to build")); };
		C.OnAct = []
		{
			if (GS.Build.bRunning) { CancelBuild(); return; }
			FString Error;
			if (!BuildAll([](int32, int32) {}, Error)) BF6Ext::Notify(Error);
		};
		Out.Add(MoveTemp(C));
	}
	void AddPieEntries(TArray<BF6Ext::FPieSubEntry>&) {}
}

// =============================================================================
// console
// =============================================================================
static FAutoConsoleCommand GHighPolyPreviewScopeCmd(
	TEXT("BF6.HighPoly.Previews.FullCatalogue"),
	TEXT("Let the shelf thumbnails and SFX auditions mount EVERY level's archives (default 0, off). ")
	TEXT("They share the reader your open map is drawn through, so this widens what the map renderer can see ")
	TEXT("and takes about 20 seconds."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		bool& Flag = BF6HP::PreviewsFullCatalogueFlag();
		if (Args.Num() >= 1)
		{
			const FString A = Args[0].ToLower();
			Flag = (A == TEXT("1") || A == TEXT("on") || A == TEXT("true"));
		}
		UE_LOG(LogBF6HighPolyPreviews, Display,
			TEXT("preview catalogue scope: %s"),
			Flag ? TEXT("every level (the map renderer sees them too)") : TEXT("this map only"));
	}));

static FAutoConsoleCommand GHighPolyPreviewBudgetCmd(
	TEXT("BF6.HighPoly.PreviewBudget"),
	TEXT("Milliseconds of picture work per tick while previews build (default 10). ")
	TEXT("Higher finishes sooner and costs more frame; this used to be one object ")
	TEXT("per tick, which on a 4,600 object catalogue was a frame each whether the ")
	TEXT("picture had to be drawn or was already on disk."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() >= 1)
		{
			GPreviewBudgetMs =
				FMath::Clamp(FCString::Atof(*Args[0]), 1.f, 500.f);
		}
		UE_LOG(LogBF6HighPolyPreviews, Display,
			TEXT("preview budget %.0f ms per tick"), GPreviewBudgetMs);
	}));

static FAutoConsoleCommand GHighPolyPreviewsCmd(
	TEXT("BF6.HighPoly.Previews"),
	TEXT("Object library previews rendered from the game. build renders every "
	     "missing icon for the open map, one per tick; clear deletes every "
	     "cached icon; status reports what is built; cancel stops a build."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const FString Verb = Args.Num() ? Args[0].ToLower() : TEXT("status");
		if (Verb == TEXT("build"))
		{
			FString Err;
			const bool bOk = BF6HPPreviews::BuildAll([](int32 Done, int32 Total)
			{
				if (Done % 100 == 0 || Done == Total)
					UE_LOG(LogBF6HighPolyPreviews, Display, TEXT("previews: %d / %d"), Done, Total);
			}, Err);
			if (!bOk) UE_LOG(LogBF6HighPolyPreviews, Warning, TEXT("previews: %s"), *Err);
		}
		else if (Verb == TEXT("clear"))
		{
			UE_LOG(LogBF6HighPolyPreviews, Display, TEXT("previews: %d icon(s) removed"), BF6HPPreviews::Clear());
		}
		else if (Verb == TEXT("cancel"))
		{
			BF6HPPreviews::CancelBuild();
		}
		else
		{
			UE_LOG(LogBF6HighPolyPreviews, Display, TEXT("previews: %s%s%s"),
				*BF6HPPreviews::StatusLine(),
				GS.LastSummary.IsEmpty() ? TEXT("") : TEXT("; last build: "),
				*GS.LastSummary);

			// WHAT THE CARDS ARE DRAWING, which is the question the status
			// command is actually asked. A count of files on disk answers
			// "did the build work"; this answers "why is the library white".
			SyncKeys();
			static const TCHAR* kLadder[] = { TEXT("Low-Poly"), TEXT("Clay"), TEXT("Textured") };
			const int32 Mode = FMath::Clamp(BF6HP::PreviewsMode(), 0, 2);
			UE_LOG(LogBF6HighPolyPreviews, Display,
				TEXT("previews: mode %s, so the library draws %s"), kLadder[Mode],
				IsHighPolyMode() ? TEXT("the game's own pictures") : TEXT("the tool's blockout, the same as the viewport"));

			if (IsHighPolyMode())
			{
				const FServing S = Summarise();
				if (S.Total == 0)
				{
					UE_LOG(LogBF6HighPolyPreviews, Display,
						TEXT("previews: nothing to count (no map open, or no game folder chosen)"));
				}
				else
				{
					UE_LOG(LogBF6HighPolyPreviews, Display,
						TEXT("previews: %d of %d type(s) have a %s icon; %d show the %s icon instead; %d fall back to the tool's blockout (%d of those have no geometry in the install)"),
						S.Exact, S.Total, *GS.Tag, S.Other, *OtherTag(), S.Blockout, S.NoGeometry);
				}
			}
			UE_LOG(LogBF6HighPolyPreviews, Display, TEXT("previews: last fallback: %s"),
				GS.LastFallback.IsEmpty() ? TEXT("none since the icons were last keyed") : *GS.LastFallback);
		}
	}));
