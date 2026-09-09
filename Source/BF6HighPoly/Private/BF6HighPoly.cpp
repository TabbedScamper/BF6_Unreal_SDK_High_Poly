#include "BF6HighPoly.h"
#include "Misc/ScopeLock.h"   // serialises the libbf6 open/mount off the game thread

#include "BF6SDKExtension.h"
#include "BF6HighPolyCore.h"
#include "BF6HighPolyLoadout.h"
#include "BF6HighPolyTerrainBridge.h"
#include "BF6HighPolyControlBridge.h"
#include "BF6HighPolyWaterFFT.h"
#include "BF6HighPolyWaterGPU.h"
#include "BF6HighPolyWaterLab.h"
#include "BF6HighPolyWaterMaterial.h"

#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include <atomic>
#include "Misc/ScopedSlowTask.h"
#include "Framework/Application/SlateApplication.h"
#include "TimerManager.h"
#include "Containers/Ticker.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/DecalComponent.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/DirectionalLight.h"
#include "TextureResource.h"   // FTexture2DMipMap, for the mip BulkData walk
#include "PixelFormat.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstantBiasScale.h"
#include "Materials/MaterialExpressionDeriveNormalZ.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "BF6HighPolyDiskCache.h"
#include "BF6HighPolyGameMode.h"
#include "BF6HighPolyUiSounds.h"   // ---- BF6UiSound ----
#include "BF6HighPolyPreviews.h"
#include "BF6HighPolyPlaced.h"   // SetEnabled: BUILD switches the placed objects on
#include "BF6HighPolyShared.h"
#include "BF6HighPolyTheme.h"
#include "BF6HighPolySection.h"   // the centred title over a fading line   // the season palette, shared with the Godot plugin
#include "BF6HighPolySplash.h"  // the waves backdrop and the logo entrance
#include "BF6HighPolyControls.h" // what a control is, shared with the modules that add them
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionThinTranslucentMaterialOutput.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialFunction.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectBase.h"
#include "UObject/UObjectGlobals.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SScrollBox.h"   // the control panel scrolls: it is taller than a popup
#include "Widgets/Input/SComboBox.h"    // a choice is a dropdown, not one button per option
#include "Widgets/Input/SSlider.h"
#include "MaterialDomain.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "AssetCompilingManager.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "SLevelViewport.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"

#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Base64.h"
#include "Misc/MessageDialog.h"
#include "ImageUtils.h"
#include "HAL/PlatformProcess.h"

#include "Modules/ModuleManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogBF6HighPoly, Log, All);

#define LOCTEXT_NAMESPACE "BF6HighPoly"

namespace
{
	const TCHAR* kAddonName = TEXT("HighPoly");

	// One core for the add-on's lifetime: opening it mounts archives and reads
	// a 176 MB executable, so it is not something to do per click.
	BF6HP::FCore GCore;
	FString      GStatus = TEXT("");
	int32        GLastCount = 0;
	std::atomic<bool> GBuildQueuedOrRunning{ false };
	// DID THE BUILD PRODUCE ANYTHING AT ALL.
	//
	// Not the same question as GLastCount, which counts placed OBJECT
	// instances only. A creator who turns every layer off except Water gets
	// a map with water and no objects: GLastCount is 0, and anything gated
	// on it then hides the water that was just built, reports "nothing yet",
	// and greys out CLEAR. Every layer that can build sets this instead.
	bool         GBuiltAnything = false;

	// The game is Y-up in metres; Unreal is Z-up in centimetres. Swapping Y and
	// Z is what carries a placement across, and the scale is the unit change.
	// NOT yet checked against the tool's own low-poly placement of the same
	// map, so treat the handedness as provisional until it is.
	FVector ToUnreal(const FVector& v, float Scale = 100.f)
	{
		return FVector(v.X, v.Z, v.Y) * Scale;
	}

	// No cap on how many distinct meshes a map builds.
	//
	// There was one, at 250, on the assumption that decoding was expensive.
	// Measured on mp_dumbo it is not: all 2,727 of the map's distinct assets
	// decode in 3.1 seconds, about a millisecond each, for 11.8M vertices and
	// 9.7M triangles. The cap was covering 29,733 of 48,127 placements and
	// leaving the rest of the map missing for no gain worth having.
	//
	// The remaining cost is Unreal's own mesh build, which is why the meshes are
	// still taken busiest-first: if a build is ever interrupted, what exists is
	// whatever covers the most ground.
	constexpr int32 kMeshBudget = MAX_int32;

	// ---- textures and materials -------------------------------------------

	// The core hands blocks over still compressed, so this is an upload rather
	// than a decode. Formats follow bf6_fmt.
	EPixelFormat PixelFormatOf(int32 Fmt)
	{
		switch (Fmt)
		{
		case 1: return PF_DXT1;      // BC1
		case 2: return PF_DXT5;      // BC3
		case 3: return PF_BC4;
		case 4: return PF_BC5;
		case 5: return PF_BC7;
		case 6: case 7: return PF_BC6H;
		case 0: return PF_B8G8R8A8;
		case 8: return PF_G8;
		case 9: return PF_FloatRGBA;
		default: return PF_Unknown;
		}
	}

	// Bytes one mip occupies. The same block rules the core decodes with, kept
	// here rather than pulled from RenderCore so this module stays a thin
	// editor-side binding.
	int32 MipBytes(EPixelFormat PF, int32 W, int32 H)
	{
		const int32 BlocksX = FMath::Max(1, (W + 3) / 4);
		const int32 BlocksY = FMath::Max(1, (H + 3) / 4);
		switch (PF)
		{
		case PF_DXT1: case PF_BC4:            return BlocksX * BlocksY * 8;
		case PF_DXT5: case PF_BC5:
		case PF_BC7:  case PF_BC6H:           return BlocksX * BlocksY * 16;
		case PF_G8:                           return W * H;
		case PF_B8G8R8A8:                     return W * H * 4;
		case PF_FloatRGBA:                    return W * H * 8;
		default:                              return 0;
		}
	}

	TMap<uint64, UTexture2D*> GTextureCache;
	int32 GTexUploaded = 0, GTexHighQuality = 0, GTexRefused = 0;
	int32 GMidsMade = 0, GBindingsSeen = 0, GBindingsBound = 0;
	double GSecObjectMaterials = 0.0, GSecObjectPrepare = 0.0;
	// WHERE THE MATERIAL TIME GOES, split so the levers are separable: the
	// texture payload (cache prefetch, or the core's serial decode), the
	// UTexture2D creation and mip upload, the parent materials (a shader
	// compile each, once a session) and, by subtraction, the instances.
	double GSecTexDecode = 0.0, GSecTexPrefetch = 0.0, GSecTexUpload = 0.0;
	double GSecParentMaterials = 0.0;
	// Working out WHICH material a section wants (the kind, the glow facts and
	// the string key) as against making one. A scene has thousands of sections
	// and only hundreds of distinct materials, so these are very different bills.
	double GSecMaterialKey = 0.0, GSecMidBuild = 0.0;
	// And of the key work, how much is asking the reader what a glow binding is
	// (memoised, so this should be near zero after the first few hundred).
	double GSecGlowFacts = 0.0;
	int32  GGlowFactsMisses = 0;
	int32 GTexFromCache = 0;
	int64 GTexCacheBytesWritten = 0;   // raw bytes handed to the cache this build
	// This is an editor preview, not a texture export.  Retail source sheets can
	// be 4K or 8K and there are thousands of them on a full map.  Starting at
	// the first authored mip no larger than 1K keeps the complete mip chain and
	// every material binding while avoiding the upload that drove Tsuru above
	// 60 GB.  The data still comes directly from the mounted game at runtime.
	int32 GPreviewTextureMax = 1024;
	// Paint is a tiny, visually critical subset. Keep its complete authored top
	// mip rather than paying the whole-map memory cost. 16384 means "full" for
	// the runtime API; it does not upscale a sheet whose authored top is smaller.
	int32 GMarkingTextureMax = 16384;

	// ---- DISK-CACHED TEXTURE PAYLOADS -------------------------------------
	//
	// The core's capped decode is a CAS read, an Oodle decompress and a copy of
	// the mip tail from the first authored level that fits, serial through the
	// one context. Its OUTPUT is a pure function of (install, resource name,
	// max dim), so it is memoised under the shared level - a sheet is the same
	// resource on every map - keyed by the resource NAME, never the id: an id
	// is the order this session's core first met the name in and means nothing
	// next session. Hits are loaded ahead of each object batch, in parallel
	// (PrefetchTexturePayloads); misses go through the core exactly as before
	// and are written behind the build. A refusal is never memoised.
	constexpr uint32 kCacheVerTexture = 1;

	struct FTexturePayload
	{
		int32 Width = 0, Height = 0, Format = 0, MipCount = 1;
		bool  bSrgb = false;
		TArray<uint8> Data;
	};
	// Keyed like GTextureCache, for GCore's ids only: they belong to that context.
	TMap<uint64, FTexturePayload> GTexPrefetched;

	FString TextureCacheName(const FString& ResName, int32 MaxDim)
	{
		return FString::Printf(TEXT("tex_%d_%s"), MaxDim,
			*FMD5::HashAnsiString(*ResName).Left(24));
	}

	void SerialiseTexturePayload(FArchive& Ar, FTexturePayload& P)
	{
		int32 Srgb = P.bSrgb ? 1 : 0, Len = P.Data.Num();
		Ar << P.Width << P.Height << P.Format << P.MipCount << Srgb << Len;
		if (Ar.IsLoading())
		{
			P.bSrgb = Srgb != 0;
			if (Ar.IsError() || P.Width <= 0 || P.Height <= 0 || P.MipCount <= 0 ||
			    Len <= 0 || (int64)Len > Ar.TotalSize() - Ar.Tell())
			{
				Ar.SetError();
				P = FTexturePayload();
				return;
			}
			P.Data.SetNumUninitialized(Len);
		}
		Ar.Serialize(P.Data.GetData(), Len);
	}

	// Pure: file read and unpack only, so it may run on a worker.
	bool LoadTexturePayload(const FString& ResName, int32 MaxDim, FTexturePayload& P)
	{
		TArray<uint8> Blob;
		if (!BF6HP::DiskCache::LoadPacked(TEXT("shared"), TextureCacheName(ResName, MaxDim),
		                                  kCacheVerTexture, Blob))
			return false;
		FMemoryReader R(Blob);
		SerialiseTexturePayload(R, P);
		return !R.IsError() && P.Data.Num() > 0;
	}

	// Load what the cache holds for these (id << 32 | max dim) keys across every
	// core and park the payloads for TextureFor. Names are resolved here, on the
	// calling thread, because the core is not thread-safe; the file reads are.
	void PrefetchTexturePayloads(const TArray<uint64>& Keys)
	{
		if (Keys.Num() == 0) return;
		const double T0 = FPlatformTime::Seconds();
		TArray<FString> Names;
		Names.SetNum(Keys.Num());
		for (int32 i = 0; i < Keys.Num(); i++)
		{
			if (GTextureCache.Contains(Keys[i]) || GTexPrefetched.Contains(Keys[i])) continue;
			Names[i] = GCore.TextureNameAt((int32)(Keys[i] >> 32));
		}
		TArray<FTexturePayload> Loaded;
		Loaded.SetNum(Keys.Num());
		ParallelFor(Keys.Num(), [&](int32 i)
		{
			if (Names[i].IsEmpty()) return;
			LoadTexturePayload(Names[i], (int32)(uint32)Keys[i], Loaded[i]);
		});
		for (int32 i = 0; i < Keys.Num(); i++)
			if (Loaded[i].Data.Num() > 0) GTexPrefetched.Add(Keys[i], MoveTemp(Loaded[i]));
		GSecTexPrefetch += FPlatformTime::Seconds() - T0;
	}

	// One UTexture2D per core texture id. Cached because the same sheet is
	// bound by many materials, and a 4K BC7 upload per binding is minutes of
	// nothing.
	UTexture2D* TextureFor(BF6HP::FCore& SourceCore, int32 Id, int32 RequestedMax = 0)
	{
		if (Id < 0) return nullptr;
		const int32 MaxDim = RequestedMax > 0 ? RequestedMax : GPreviewTextureMax;
		const uint64 CacheKey = ((uint64)(uint32)Id << 32) | (uint32)MaxDim;
		if (UTexture2D** hit = GTextureCache.Find(CacheKey)) return *hit;

		// THE PAYLOAD: the batch prefetch, else the disk cache, else the core.
		const double DecodeStart = FPlatformTime::Seconds();
		FTexturePayload P;
		bool bHave = false;
		if (&SourceCore == &GCore)
		{
			if (FTexturePayload* Pre = GTexPrefetched.Find(CacheKey))
			{
				P = MoveTemp(*Pre);
				GTexPrefetched.Remove(CacheKey);
				bHave = P.Data.Num() > 0;
			}
		}
		const FString ResName = SourceCore.TextureNameAt(Id);
		if (!bHave && !ResName.IsEmpty()) bHave = LoadTexturePayload(ResName, MaxDim, P);
		if (bHave)
		{
			GTexFromCache++;
		}
		else
		{
			BF6HP::FCore::FTexture T;
			if (!SourceCore.TextureAt(Id, T, MaxDim))
			{
				GSecTexDecode += FPlatformTime::Seconds() - DecodeStart;
				GTexRefused++; GTextureCache.Add(CacheKey, nullptr); return nullptr;
			}
			P.Width = T.Width; P.Height = T.Height; P.Format = T.Format;
			P.MipCount = T.MipCount; P.bSrgb = T.bSrgb;
			P.Data.Append(T.Data, T.DataLen);
			// Written behind the build, packed on the worker. An older core that
			// cannot name the resource gets no blob rather than an unkeyable one.
			if (!ResName.IsEmpty() && P.Data.Num() > 0)
			{
				TArray<uint8> Blob;
				FMemoryWriter W(Blob);
				SerialiseTexturePayload(W, P);
				GTexCacheBytesWritten += Blob.Num();
				BF6HP::DiskCache::SaveAsync(TEXT("shared"), TextureCacheName(ResName, MaxDim),
					kCacheVerTexture, MoveTemp(Blob), /*bPack*/ true);
			}
		}
		GSecTexDecode += FPlatformTime::Seconds() - DecodeStart;

		const double UploadStart = FPlatformTime::Seconds();
		const EPixelFormat PF = PixelFormatOf(P.Format);
		if (PF == PF_Unknown) { GTexRefused++; GTextureCache.Add(CacheKey, nullptr); return nullptr; }

		// THE WHOLE CHAIN, not just the top level.
		//
		// Foliage drawn with a hard alpha test against a mask with NO mip chain
		// speckles every frame and crawls with the camera - the "lacy,
		// moth-eaten" look. It is invariant to mask resolution, so shrinking the
		// texture never fixes it and the mask gets blamed instead.
		UTexture2D* Tex = UTexture2D::CreateTransient(P.Width, P.Height, PF, NAME_None);
		if (!Tex) { GTextureCache.Add(CacheKey, nullptr); return nullptr; }
		GTexUploaded++;
		if (MaxDim > GPreviewTextureMax) GTexHighQuality++;

		// SRGB IS THE TEXTURE'S, NOT THE FORMAT'S. The same BC7 blocks carry a
		// colour sheet and a normal map, and the header bit says which. Reading
		// a linear normal as sRGB is the classic "lighting looks wrong and
		// nothing is obviously broken" bug.
		Tex->SRGB = P.bSrgb;
		// BC6H IS HDR, and it is neither a colour sheet nor a mask.
		//
		// The sky panorama arrives here as BC6H and the two-way choice below
		// would file it as TC_Masks, which is the setting for a packed
		// non-colour sheet. TC_HDR is what a float format wants, and SRGB must
		// be off or the values are decoded twice.
		const EPixelFormat Pf = PixelFormatOf(P.Format);
		if (Pf == PF_BC6H || Pf == PF_FloatRGBA)
		{
			Tex->SRGB = false;
		}
		// These runtime textures carry their complete authored mip chain, so let
		// Unreal keep only the mips demanded by visible components. Marking every
		// transient sheet NeverStream forced all thousands of object textures into
		// VRAM at once and exhausted a 16 GB card on Aftermath. Streaming changes
		// residency only; the bytes still come straight from the mounted game and
		// the near view can request the same top mip.
		Tex->NeverStream = false;
		Tex->LODGroup = TEXTUREGROUP_World;
		Tex->Filter = TF_Default;       // World group = anisotropic sampling
		Tex->AddressX = TA_Wrap;
		Tex->AddressY = TA_Wrap;
		Tex->CompressionSettings =
			(Pf == PF_BC6H || Pf == PF_FloatRGBA) ? TC_HDR
			: (P.bSrgb ? TC_Default : TC_Masks);

		FTexturePlatformData* PD = Tex->GetPlatformData();

		// CreateTransient makes one mip; the rest are added here and filled from
		// the chain the core handed over, which is contiguous and largest-first.
		int32 Off = 0, W = P.Width, H = P.Height;
		for (int32 m = 0; m < P.MipCount; m++)
		{
			if (m >= PD->Mips.Num())
			{
				FTexture2DMipMap* NewMip = new FTexture2DMipMap();
				NewMip->SizeX = W;
				NewMip->SizeY = H;
				NewMip->BulkData.Lock(LOCK_READ_WRITE);
				NewMip->BulkData.Realloc(MipBytes(PF, W, H));
				NewMip->BulkData.Unlock();
				PD->Mips.Add(NewMip);
			}
			FTexture2DMipMap& Mip = PD->Mips[m];
			void* Dst = Mip.BulkData.Lock(LOCK_READ_WRITE);
			const int32 Have = (int32)Mip.BulkData.GetBulkDataSize();
			const int32 Left = P.Data.Num() - Off;
			if (Left <= 0)
			{
				// The chain ran out early: keep the mips we filled and drop the
				// rest, rather than uploading a level of uninitialised memory.
				Mip.BulkData.Unlock();
				while (PD->Mips.Num() > m) PD->Mips.RemoveAt(PD->Mips.Num() - 1);
				break;
			}
			FMemory::Memcpy(Dst, P.Data.GetData() + Off, FMath::Min(Have, Left));
			Mip.BulkData.Unlock();
			Off += Have;
			W = FMath::Max(1, W / 2);
			H = FMath::Max(1, H / 2);
		}
		Tex->UpdateResource();
		GSecTexUpload += FPlatformTime::Seconds() - UploadStart;

		GTextureCache.Add(CacheKey, Tex);
		return Tex;
	}

	UTexture2D* TextureFor(int32 Id, int32 RequestedMax = 0)
	{
		return TextureFor(GCore, Id, RequestedMax);
	}

	// BF6's grading resource is a raw 33 x 33 x 33 R10G10B10A2 cube. Unreal's
	// post-process combiner consumes the same texels as a 2D strip whose width is
	// size squared and whose height is size. This only changes the resource
	// SHAPE: the installed bytes are copied unchanged and no colour conversion,
	// resampling, export or generated intermediate exists.
	TMap<int32, UTexture2D*> GColorLutCache;
	UTexture2D* TextureForColorLut(int32 Id)
	{
		if (Id < 0) return nullptr;
		if (UTexture2D** Hit = GColorLutCache.Find(Id)) return *Hit;
		BF6HP::FCore::FTexture T;
		if (!GCore.TextureAt(Id, T, 0) || T.Width <= 1 || T.Width != T.Height)
		{
			GColorLutCache.Add(Id, nullptr);
			return nullptr;
		}
		const int64 Side = T.Width;
		const int64 RawBytes = Side * Side * Side * 4;
		if (T.DataLen != RawBytes)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("grading LUT %d is %dx%d/%d bytes, not a raw R10G10B10A2 cube"),
				Id, T.Width, T.Height, T.DataLen);
			GColorLutCache.Add(Id, nullptr);
			return nullptr;
		}
		UTexture2D* Lut = UTexture2D::CreateTransient(
			T.Width * T.Width, T.Height, PF_A2B10G10R10, NAME_None);
		if (!Lut) { GColorLutCache.Add(Id, nullptr); return nullptr; }
		Lut->SRGB = false;
		Lut->NeverStream = true;
		Lut->Filter = TF_Bilinear;
		Lut->AddressX = TA_Clamp;
		Lut->AddressY = TA_Clamp;
		Lut->LODGroup = TEXTUREGROUP_ColorLookupTable;
		Lut->CompressionSettings = TC_HDR;
		FTexturePlatformData* PD = Lut->GetPlatformData();
		if (!PD || PD->Mips.IsEmpty())
		{
			GColorLutCache.Add(Id, nullptr);
			return nullptr;
		}
		FTexture2DMipMap& Mip = PD->Mips[0];
		void* Dst = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Dst, T.Data, T.DataLen);
		Mip.BulkData.Unlock();
		Lut->UpdateResource();
		GColorLutCache.Add(Id, Lut);
		return Lut;
	}

	// THREE PARENTS, BECAUSE BLEND MODE IS NOT AN INSTANCE PARAMETER.
	//
	// A material instance can change textures and scalars but not whether the
	// surface is opaque, cut out, or see-through - that is compiled into the
	// material. One parent per kind is the only way to have leaves cut out and
	// glass be transparent while everything else stays opaque.
	// MaskedVeg is its own kind because the cutout comes from a DIFFERENT
	// place: vegetation masks from its base colour's alpha, everything else
	// from a separate sheet. That is a wiring difference in the graph, and a
	// graph cannot be rewired per instance.
	// ROAD IS ITS OWN KIND rather than a reuse of Masked, because a street
	// marking blends rather than cuts. Roughly a tenth of decal vertices carry an
	// alpha strictly between 0 and 1 - the mud and dirt edge fades - and a masked
	// material can only keep or drop a pixel, so every decal would end on a hard
	// boundary instead of easing into the terrain.
	// VISTA IS ITS OWN KIND because its normal texture is not a normal map.
	// The backdrop building's "_nsm" packs RG normal / B wetness / A smoothness
	// (the recovered Aftermath M_Vista shader), and unpacking is graph wiring,
	// which cannot change per instance. Opaque on purpose: that shader reads
	// RGB only and never alpha-tests, however binary its atlas alpha looks.
	enum class EKind : uint8 { Opaque, Masked, MaskedVeg, Translucent, Decal, Puddle, Road, Vista, Eye, FxCard, CarPaint, Count };
	UMaterial* GParents[(int32)EKind::Count] = {};
	// Emissive fixtures use a separate parent permutation. Sampling an emissive
	// sheet on every surface would add a texture fetch to the entire map for the
	// sake of the small set of records that bind the game's real glow slot.
	UMaterial* GEmissiveParents[(int32)EKind::Count] = {};
	TMap<FString, UMaterialInstanceDynamic*> GMaterialCache;
	int32 GMaterialCacheHits = 0, GMaterialCacheMisses = 0;

	// ONE PARENT MATERIAL, BUILT ONCE, AT RUNTIME.
	//
	// The add-on ships no content on purpose - it is a folder you delete - so
	// there is no .uasset to point at. Building the graph here keeps that true.
	// Three texture parameters is all a first pass needs; the constants the
	// depot also carries (tints, smoothness) come later.
	// A LINEAR DEFAULT FOR THE LINEAR PARAMETERS, and the reason it has to exist.
	//
	// Unreal validates a texture parameter's sampler type against the texture
	// sitting in it AT COMPILE TIME - the default, not whatever a material
	// instance binds later. Every white texture the engine ships is sRGB, so a
	// SAMPLERTYPE_LinearColor parameter defaulted to WhiteSquareTexture fails the
	// check, and the failure is not confined to that one node: the WHOLE material
	// refuses to compile and every section using it silently renders with the
	// engine default. A compile error that presents as "vegetation has no
	// materials", which is a long way from its cause:
	//
	//   (Node TextureSampleParameter2D) Sampler type is Linear Color, should be
	//   Color for /Engine/EngineResources/WhiteSquareTexture
	//
	// So the linear parameters get a linear white of our own: 1x1, uncompressed,
	// SRGB off. It is never sampled once a real mask is bound - it exists to
	// satisfy the type check.
	UTexture2D* GLinearWhite = nullptr;
	UTexture2D* GLinearFlatNormal = nullptr;

	UTexture2D* LinearWhite()
	{
		if (GLinearWhite) return GLinearWhite;
		UTexture2D* T = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
		if (!T) return nullptr;
		T->SRGB = false;
		T->CompressionSettings = TC_VectorDisplacementmap;   // stays RGBA8, linear
		T->AddressX = TA_Wrap;
		T->AddressY = TA_Wrap;
		if (uint8* P = (uint8*)T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))
		{
			P[0] = P[1] = P[2] = P[3] = 255;
			T->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		T->UpdateResource();
		// A parameter default is held by the material, and the material lives in a
		// package we never save, so nothing else keeps this alive.
		T->AddToRoot();
		GLinearWhite = T;
		return T;
	}

	UTexture2D* LinearFlatNormal()
	{
		if (GLinearFlatNormal) return GLinearFlatNormal;
		UTexture2D* T = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
		if (!T) return nullptr;
		T->SRGB = false;
		T->CompressionSettings = TC_VectorDisplacementmap;
		T->AddressX = TA_Wrap;
		T->AddressY = TA_Wrap;
		if (uint8* P = (uint8*)T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))
		{
			// PF_B8G8R8A8 byte order. Packed BF6 normals reconstruct Z from RG,
			// so neutral is R=G=0.5. Linear white is an extreme (1,1,0)
			// tangent normal and was incorrectly dimming colour-only road paint.
			P[0] = 255; P[1] = 128; P[2] = 128; P[3] = 255;
			T->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		T->UpdateResource();
		T->AddToRoot();
		GLinearFlatNormal = T;
		return T;
	}

	UMaterial* EnsureParentMaterial(EKind Kind, bool bEmissive = false)
	{
		UMaterial*& CachedParent = bEmissive
			? GEmissiveParents[(int32)Kind]
			: GParents[(int32)Kind];
		if (CachedParent) return CachedParent;

		// BUILT THROUGH THE EDITOR'S OWN API, not by hand.
		//
		// Assembling a material by NewObject-ing expressions and assigning them
		// straight onto the editor-only data leaves it half-registered: the
		// first attempt here produced a material with its two expressions
		// present and "complete=0, compiling=0" - no shaders, and not even
		// trying to build any, so every instance silently fell back to the
		// engine default. That is grey, and indistinguishable from a texture
		// that failed to load.
		//
		// UMaterialEditingLibrary does the bookkeeping the editor does when a
		// human wires a node, and RecompileMaterial is what actually asks for
		// shaders.
		// A REAL PACKAGE, NOT THE TRANSIENT ONE.
		//
		// The first attempt built this in the transient package and it never
		// compiled (complete=0, compiling=0); forcing the issue with
		// RecompileMaterial then crashed inside the material editor reading a
		// null. Both have the same cause: a material assembled outside a
		// package is only half a material to the editor, and the compile path
		// does package-level work on it.
		//
		// The package is created but NEVER SAVED, so nothing appears on disk
		// and the add-on still ships no content - it exists for the lifetime of
		// the editor session and goes with it.
		const TCHAR* KindName =
			Kind == EKind::Masked ? TEXT("Masked")
			: Kind == EKind::MaskedVeg ? TEXT("MaskedVeg")
			: Kind == EKind::Translucent ? TEXT("Translucent")
			: Kind == EKind::Decal ? TEXT("Decal")
			: Kind == EKind::Puddle ? TEXT("Puddle")
			: Kind == EKind::Road ? TEXT("Road")
			: Kind == EKind::FxCard ? TEXT("FxCard")
			: Kind == EKind::CarPaint ? TEXT("CarPaint")
			: Kind == EKind::Eye ? TEXT("Eye")
			: Kind == EKind::Vista ? TEXT("Vista") : TEXT("Opaque");

		const FString ParentName = FString::Printf(TEXT("%s%s"), KindName,
			bEmissive ? TEXT("Emissive") : TEXT(""));
		UPackage* Pkg = CreatePackage(*FString::Printf(TEXT("/Temp/BF6HighPoly_%s"), *ParentName));
		if (!Pkg) return nullptr;
		Pkg->SetFlags(RF_Transient);

		UMaterial* M = NewObject<UMaterial>(Pkg,
			*FString::Printf(TEXT("M_BF6HighPoly_%s"), *ParentName), RF_Transient);
		M->MaterialDomain = Kind == EKind::Puddle ? MD_DeferredDecal : MD_Surface;
		M->SetShadingModel(MSM_DefaultLit);
		if (Kind == EKind::Masked || Kind == EKind::MaskedVeg)
		{
			M->BlendMode = BLEND_Masked;
			// FOLIAGE IS LIT THROUGH. The game's vegetation is material model 1
			// (Translucency): light passes through the leaf scaled by the
			// ProfileOATS translucency amount, with no screen-space blur. Unreal's
			// counterpart is Two Sided Foliage with SubsurfaceColor. Research:
			// scattering.md 2.7 / 4.B2. DefaultLit made every leaf a slab that
			// went black against the sun.
			if (Kind == EKind::MaskedVeg) M->SetShadingModel(MSM_TwoSidedFoliage);
			// CUT AT 0.5, NOT UNREAL'S DEFAULT 0.333.
			//
			// Measured over all 168 vegetation masks on one map: 168 of 168
			// cross 0.5 and none vanishes there. They come in two families -
			// 109 hard masks and 59 distance-field ramps that top out near
			// 0.698 - and a threshold BELOW 0.5 cuts the ramp family early and
			// dilates every leaf into a blob. A previous consumer that scaled
			// its threshold to the observed maximum landed on 0.31 and did
			// exactly that, and it reads as a texture problem rather than a
			// threshold one.
			M->OpacityMaskClipValue = 0.5f;
			// Foliage is a shell with no inside, so a cut-out leaf seen from
			// behind must still be lit rather than vanish.
			M->TwoSided = true;
		}
		else if (Kind == EKind::Translucent)
		{
			M->BlendMode = BLEND_TranslucentColoredTransmittance;
			M->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
			M->TwoSided = true;
			// GLASS IS A THIN SLAB, not a faded solid.
			//
			// A plain translucent surface at opacity 0.25 is a grey film: it
			// scales the whole shaded result down, highlight included, and it
			// has no idea which way you are looking through it. Thin
			// Translucent is the model for exactly this case and the engine
			// already ships it (ThinTranslucentCommon.ush):
			//     PathLength   = 1 / NoV
			//     Transmittance = exp(log(TransmittanceColor) * PathLength)
			//     Transmittance *= (1 - Fresnel) twice, once per interface
			// so it gives COLOURED transmission, thickness that grows at
			// grazing angles, and Fresnel on both faces, with dual source
			// blending so the specular highlight is not dimmed by the
			// transparency. That is what glass does and a constant opacity is
			// not.
			//
			// The engine enforces four things for this model
			// (MaterialShared.cpp:6451-6470): a translucent blend, per pixel
			// surface lighting, this shading model ALONE, and the presence of
			// a ThinTranslucentMaterialOutput node. All four are set here.
			M->SetShadingModel(MSM_ThinTranslucent);
		}
		else if (Kind == EKind::Road || Kind == EKind::Decal || Kind == EKind::Puddle)
		{
			M->BlendMode = BLEND_Translucent;
			M->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
			// PAINT ON THE GROUND CASTS NO SHADOW and receives no self-shadow;
			// a stripe that shadowed itself would draw a dark outline around
			// every marking on the map.
			M->bCastDynamicShadowAsMasked = false;
		}

		auto AddParam = [&](const TCHAR* Name, EMaterialSamplerType Sampler, int32 Y)
			-> UMaterialExpressionTextureSampleParameter2D*
		{
			UMaterialExpression* E = UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, Y);
			UMaterialExpressionTextureSampleParameter2D* P =
				Cast<UMaterialExpressionTextureSampleParameter2D>(E);
			if (!P) return nullptr;
			P->ParameterName = Name;
			// The sampler type has to match what is plugged in: a normal map
			// read as colour shades as though every surface faced the camera,
			// and Unreal refuses to compile the mismatch rather than guessing.
			P->SamplerType = Sampler;
			return P;
		};

		// The verified glass family has no base-colour or normal surface sheet:
		// 1,952 records over five levels bind neither one. Creating those samples
		// for the translucent parent made the engine's white/default-normal
		// compile fallbacks part of the visible glass, producing the opaque grey
		// "missing bump map" seen on the Aftermath sedan. Glass takes its colour
		// from the record's transmission palette below, so omit both surface
		// samples from this parent altogether.
		UMaterialExpressionTextureSampleParameter2D* Base =
			(Kind == EKind::Translucent || Kind == EKind::Eye || Kind == EKind::FxCard) ? nullptr
			: AddParam(TEXT("BaseColor"), SAMPLERTYPE_Color, 0);
		UMaterialExpressionTextureSampleParameter2D* Norm =
			(Kind == EKind::Translucent || Kind == EKind::Eye || Kind == EKind::FxCard) ? nullptr
			: AddParam(TEXT("Normal"),
				// ROADS TAKE THE VISTA TREATMENT TOO. A road decal binds an
				// "_nhs" sheet - normal, height, smoothness - which is the
				// same packing as the vista's "_nsm" for the two channels
				// either of them reads: RG is the tangent normal's XY and A is
				// smoothness. Only the unread middle channel differs, height
				// against wetness. Sampled as a NORMAL map the whole RGB goes
				// into the normal pin, so the height channel is read as Z and
				// the smoothness is thrown away.
				// A DECAL BELONGS IN THIS LIST TOO, and its absence was a
				// compile failure rather than a wrong look: the default
				// texture below already gives a Decal the LINEAR white, so
				// leaving the sampler as Normal binds a linear texture to a
				// normal sampler and Unreal refuses the material outright -
				//   "Sampler type is Normal, should be Linear Color"
				// - and falls back to the Default Material, which draws as
				// flat engine grey and reads as bad decal data rather than as
				// a broken shader. Same reasoning as the road: a decal's
				// normal sheet is two-channel, so it is sampled linear and Z
				// is reconstructed.
				(Kind == EKind::Vista || Kind == EKind::Road || Kind == EKind::Decal || Kind == EKind::Puddle)
					? SAMPLERTYPE_LinearColor : SAMPLERTYPE_Normal, 300);
		UMaterialExpressionTextureSampleParameter2D* Emissive = nullptr;
		UMaterialExpressionScalarParameter* EmissiveEnable = nullptr;
		UMaterialExpressionVectorParameter* EmissiveTint = nullptr;
		if (bEmissive)
		{
			// The installed-game reader maps both the misleading named slot
			// 0xD405B0E5 and the verified fixture-glow slot 0x407055FD to slot 3.
			// Only this parent permutation samples it. The zero gate keeps a failed
			// texture decode black instead of turning the whole fixture white.
			Emissive = AddParam(TEXT("Emissive"), SAMPLERTYPE_Color, 560);
			EmissiveEnable = Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionScalarParameter::StaticClass(), -700, 640));
			EmissiveTint = Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -700, 760));
			if (Emissive)
				Emissive->Texture = LoadObject<UTexture2D>(nullptr,
					TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
			if (EmissiveEnable)
			{
				EmissiveEnable->ParameterName = TEXT("EmissiveEnable");
				EmissiveEnable->DefaultValue = 0.f;
			}
			if (EmissiveTint)
			{
				EmissiveTint->ParameterName = TEXT("EmissiveTint");
				EmissiveTint->DefaultValue = FLinearColor::White;
			}
			if (Emissive && EmissiveEnable && EmissiveTint)
			{
				UMaterialExpressionMultiply* EmissiveTintMul =
					Cast<UMaterialExpressionMultiply>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionMultiply::StaticClass(), -180, 580));
				UMaterialExpressionMultiply* EmissiveMul =
					Cast<UMaterialExpressionMultiply>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionMultiply::StaticClass(), 60, 640));
				if (EmissiveTintMul && EmissiveMul)
				{
					UMaterialEditingLibrary::ConnectMaterialExpressions(
						Emissive, TEXT(""), EmissiveTintMul, TEXT("A"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(
						EmissiveTint, TEXT(""), EmissiveTintMul, TEXT("B"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(
						EmissiveTintMul, TEXT(""), EmissiveMul, TEXT("A"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(
						EmissiveEnable, TEXT(""), EmissiveMul, TEXT("B"));
					UMaterialEditingLibrary::ConnectMaterialProperty(
						EmissiveMul, TEXT(""), MP_EmissiveColor);
				}
			}
		}
		UMaterialExpressionScalarParameter* RoadMipBias = nullptr;
		if (Kind == EKind::Road)
		{
			RoadMipBias = Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionScalarParameter::StaticClass(), -700, -100));
			if (RoadMipBias)
			{
				// Per-record instances leave this at zero for ordinary road detail.
				// Markings set -4, retaining sixteen times the linear texel density
				// while still allowing the normal mip chain to take over at distance.
				RoadMipBias->ParameterName = TEXT("RoadMipBias");
				RoadMipBias->DefaultValue = 0.f;
				for (UMaterialExpressionTextureSampleParameter2D* Sample : {Base, Norm})
				{
					if (!Sample) continue;
					Sample->MipValueMode = TMVM_MipBias;
					UMaterialEditingLibrary::ConnectMaterialExpressions(
						RoadMipBias, TEXT(""), Sample, TEXT("MipBias"));
				}
			}
		}

		// A parameter with no default texture compiles to nothing useful, so
		// each gets a neutral one: white for colour, flat for the normal.
		if (Base) Base->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		// The vista NSM is sampled linear, and a linear sampler with the
		// normal-compressed default fails the same compile-time check the
		// sRGB white does - so it defaults to our linear white instead. It is
		// never sampled once a real sheet is bound.
		if (Norm)
		{
			const bool bPackedNormal =
				Kind == EKind::Vista || Kind == EKind::Road || Kind == EKind::Decal || Kind == EKind::Puddle;
			Norm->Texture = bPackedNormal
				? LinearFlatNormal()
				: LoadObject<UTexture2D>(nullptr,
					TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal"));
			// Assigning Texture can auto-select a sampler from the texture's
			// compression settings. Set the declared type LAST so the transient
			// linear default cannot turn this packed-RG node back into Normal.
			Norm->SamplerType = bPackedNormal
				? SAMPLERTYPE_LinearColor : SAMPLERTYPE_Normal;
		}

		// THE RECORD'S OWN COLOUR, multiplied over the sheet.
		//
		// Not an aesthetic control: for car paint and the tile-paint kits the
		// depot record carries the colour and binds NO base-colour texture at all,
		// so without this multiply every car on the map is white and every painted
		// kit is grey. Where a sheet IS bound the same parameter carries the tint
		// the record applies to it, which is where the washed-out and too-dark
		// mismatches live.
		//
		// Default white, so a material that says nothing is unchanged.
		UMaterialExpressionVectorParameter* Tint =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -700, 120));
		UMaterialExpressionMultiply* Mul = nullptr;
		if (Tint)
		{
			Tint->ParameterName = TEXT("BaseColorTint");
			Tint->DefaultValue = FLinearColor::White;
			Mul = Cast<UMaterialExpressionMultiply>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionMultiply::StaticClass(), -200, 60));
		}
		if (Kind != EKind::Puddle && Base && Mul && Tint)
		{
			UMaterialEditingLibrary::ConnectMaterialExpressions(Base, TEXT(""), Mul, TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Mul, TEXT("B"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Mul, TEXT(""), MP_BaseColor);
		}
		else if (Kind != EKind::Puddle && Base)
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);
		}
		if (Kind == EKind::MaskedVeg && Base)
		{
			// SubsurfaceColor = BaseColor * translucency amount. The amount is the
			// profile's BaseColorAmount: 0.25 is the fleet default and Eastwood
			// authors 0.75. libbf6 has no SubSurfaceScatteringComponentData reader
			// yet, so the default is exposed as a parameter and logged as a
			// default, not passed off as a read.
			UMaterialExpressionScalarParameter* Amount = Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionScalarParameter::StaticClass(), -700, 260));
			UMaterialExpressionMultiply* SubMul = Cast<UMaterialExpressionMultiply>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionMultiply::StaticClass(), -200, 260));
			if (Amount && SubMul)
			{
				Amount->ParameterName = TEXT("BF6FoliageTranslucency");
				Amount->DefaultValue = 0.25f;
				UMaterialExpression* Lit = Mul ? (UMaterialExpression*)Mul : (UMaterialExpression*)Base;
				UMaterialEditingLibrary::ConnectMaterialExpressions(Lit, TEXT(""), SubMul, TEXT("A"));
				UMaterialEditingLibrary::ConnectMaterialExpressions(Amount, TEXT(""), SubMul, TEXT("B"));
				UMaterialEditingLibrary::ConnectMaterialProperty(SubMul, TEXT(""), MP_SubsurfaceColor);
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("vegetation: two-sided foliage with SubsurfaceColor = BaseColor * BF6FoliageTranslucency ")
					TEXT("(0.25 fleet default until the VE SSS profile is read live)"));
			}
		}
		if (Norm && (Kind == EKind::Vista || Kind == EKind::Road || Kind == EKind::Decal || Kind == EKind::Puddle))
		{
			// REBUILT FROM RG, ROUGHNESS FROM A. The sheet is linear RGBA:
			// RG holds the tangent normal's XY, so Z is derived; A is
			// SMOOTHNESS, so roughness is one minus it; B is a wetness
			// response and deliberately goes nowhere - reading it as metallic
			// is the classic viewer error the finding calls out.
			UMaterialExpressionComponentMask* RG =
				Cast<UMaterialExpressionComponentMask>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionComponentMask::StaticClass(), -220, 300));
			UMaterialExpressionConstantBiasScale* BS =
				Cast<UMaterialExpressionConstantBiasScale>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionConstantBiasScale::StaticClass(), -100, 300));
			UMaterialExpressionDeriveNormalZ* DZ =
				Cast<UMaterialExpressionDeriveNormalZ>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionDeriveNormalZ::StaticClass(), 20, 300));
			if (RG && BS && DZ)
			{
				RG->R = 1; RG->G = 1; RG->B = 0; RG->A = 0;
				BS->Bias = -0.5f; BS->Scale = 2.f;
				UMaterialEditingLibrary::ConnectMaterialExpressions(Norm, TEXT(""), RG, TEXT(""));
				UMaterialEditingLibrary::ConnectMaterialExpressions(RG, TEXT(""), BS, TEXT(""));
				UMaterialEditingLibrary::ConnectMaterialExpressions(BS, TEXT(""), DZ, TEXT("InXY"));
				UMaterialEditingLibrary::ConnectMaterialProperty(DZ, TEXT(""), MP_Normal);
			}
			UMaterialExpressionOneMinus* OM =
				Cast<UMaterialExpressionOneMinus>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionOneMinus::StaticClass(), -100, 460));
			if (OM)
			{
				UMaterialEditingLibrary::ConnectMaterialExpressions(
					Norm, Kind == EKind::Puddle ? TEXT("B") : TEXT("A"), OM, TEXT(""));
				UMaterialEditingLibrary::ConnectMaterialProperty(OM, TEXT(""), MP_Roughness);
			}
			if (Kind == EKind::Puddle)
			{
				// _nsa is normal/smoothness/alpha: unlike the _nms family above,
				// B is smoothness and A is the puddle's coverage. No BaseColor is
				// connected, so UE 5.8 writes only normal and roughness to the GBuffer.
				UMaterialEditingLibrary::ConnectMaterialProperty(Norm, TEXT("A"), MP_Opacity);
			}
		}
		else if (Norm) UMaterialEditingLibrary::ConnectMaterialProperty(Norm, TEXT(""), MP_Normal);

		// Roughness, likewise from the record. Car paint carries a smoothness and
		// a car that is not glossy does not read as a car. NOT on vista: there
		// the roughness is per-pixel from the NSM alpha, wired above, and a
		// second connection here would overwrite it.
		if (Kind != EKind::Vista && Kind != EKind::Road && Kind != EKind::Decal && Kind != EKind::Puddle)
		if (UMaterialExpressionScalarParameter* Rough =
				Cast<UMaterialExpressionScalarParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionScalarParameter::StaticClass(), -700, 460)))
		{
			Rough->ParameterName = TEXT("Roughness");
			Rough->DefaultValue = 0.5f;
			UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		}

		// WIND, on the vegetation parent only, off until the pill asks.
		//
		// The Godot plugin learned this the expensive way: its first wind
		// swap replaced the whole leaf material and downgraded every tree
		// even with wind OFF. Here the sway is a WorldPositionOffset branch
		// on the SAME material, gated by a scalar that defaults to zero - at
		// zero the offset is exactly nothing and the material is unchanged.
		if (Kind == EKind::MaskedVeg)
		{
			UMaterialFunction* GrassWind = LoadObject<UMaterialFunction>(nullptr,
				TEXT("/Engine/Functions/Engine_MaterialFunctions01/WorldPositionOffset/SimpleGrassWind.SimpleGrassWind"));
			UMaterialExpressionMaterialFunctionCall* Call = GrassWind
				? Cast<UMaterialExpressionMaterialFunctionCall>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionMaterialFunctionCall::StaticClass(), -400, 900))
				: nullptr;
			if (Call)
			{
				Call->SetMaterialFunction(GrassWind);
				UMaterialExpressionScalarParameter* WInt =
					Cast<UMaterialExpressionScalarParameter>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionScalarParameter::StaticClass(), -700, 860));
				UMaterialExpressionScalarParameter* WSpd =
					Cast<UMaterialExpressionScalarParameter>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionScalarParameter::StaticClass(), -700, 940));
				if (WInt && WSpd)
				{
					WInt->ParameterName = TEXT("WindIntensity");
					WInt->DefaultValue = 0.f;
					WSpd->ParameterName = TEXT("WindSpeed");
					WSpd->DefaultValue = 1.f;
					UMaterialEditingLibrary::ConnectMaterialExpressions(WInt, TEXT(""), Call, TEXT("WindIntensity"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(WInt, TEXT(""), Call, TEXT("WindWeight"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(WSpd, TEXT(""), Call, TEXT("WindSpeed"));
					// ADDITIONALWPO IS REQUIRED, and leaving it off cost every
					// map its vegetation.
					//
					// SimpleGrassWind declares this input with no default, so
					// an unconnected pin is not "use zero" - it is a compile
					// ERROR, "Missing function input 'AdditionalWPO'", and the
					// whole material then falls back to Unreal's default:
					//   Failed to compile Material for platform PCD3D_SM6,
					//   Default Material will be used in game.
					// Which is to say every masked vegetation card in the map
					// was drawing as the engine default rather than as a leaf.
					// It went unnoticed for as long as it did because nothing
					// ever asked this material whether it had compiled.
					//
					// Zero is the right value: it is the extra displacement to
					// add on top of the wind, and we have none.
					UMaterialExpressionConstant3Vector* NoExtraWPO =
						Cast<UMaterialExpressionConstant3Vector>(
							UMaterialEditingLibrary::CreateMaterialExpression(
								M, UMaterialExpressionConstant3Vector::StaticClass(), -700, 1020));
					if (NoExtraWPO)
					{
						NoExtraWPO->Constant = FLinearColor(0.f, 0.f, 0.f);
						UMaterialEditingLibrary::ConnectMaterialExpressions(
							NoExtraWPO, TEXT(""), Call, TEXT("AdditionalWPO"));
					}
					UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT(""), MP_WorldPositionOffset);
				}
			}
		}

		// THE CUTOUT COMES FROM ITS OWN SHEET, not from the base colour's
		// alpha. A "_cs" base colour's alpha is SMOOTHNESS, which is why
		// cutouts ship as separate single-channel sheets - masking by the base
		// alpha would punch holes wherever a surface happened to be glossy.
		if (Kind == EKind::MaskedVeg)
		{
			// STRAIGHT OFF THE BASE COLOUR'S ALPHA. No second sampler: the veg
			// sheet already carries coverage there, and adding one would sample
			// a texture that does not exist for these materials.
			if (Base) UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT("A"), MP_OpacityMask);
		}
		else if (Kind == EKind::Road)
		{
			// COVERAGE TIMES VERTEX ALPHA, and both halves are load-bearing.
			//
			// The sheet is where the MARKINGS live - a lane line is coverage,
			// not colour, so a road bound only through its base colour draws
			// the asphalt with none of the paint on it and still reads as an
			// empty street. The vertex alpha is the authored edge fade, which
			// is what lets mud and wear ease into the terrain instead of
			// ending on a rectangle.
			UMaterialExpressionTextureSampleParameter2D* Cov =
				AddParam(TEXT("Opacity"), SAMPLERTYPE_LinearColor, 600);
			UMaterialExpressionVertexColor* VC =
				Cast<UMaterialExpressionVertexColor>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionVertexColor::StaticClass(), -700, 760));
			if (Cov) Cov->Texture = LinearWhite();
			if (Cov && RoadMipBias)
			{
				Cov->MipValueMode = TMVM_MipBias;
				UMaterialEditingLibrary::ConnectMaterialExpressions(
					RoadMipBias, TEXT(""), Cov, TEXT("MipBias"));
			}
			if (Cov && VC)
			{
				UMaterialExpressionMultiply* OpMul =
					Cast<UMaterialExpressionMultiply>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionMultiply::StaticClass(), -200, 660));
				if (OpMul)
				{
					// THE MASK IS A PACKED ATLAS, so red is not always the
					// coverage. One sheet holds three or four painted road
					// words or arrows and each record picks one by an authored
					// index; the sheets that use three or four values are all
					// named "_rgb" and no single-mask sheet ever reaches 3.
					// Taking R unconditionally is right about one record in
					// three. MaskSelect defaults to red, so a record with no
					// index behaves exactly as before.
					UMaterialExpressionVectorParameter* Sel =
						Cast<UMaterialExpressionVectorParameter>(
							UMaterialEditingLibrary::CreateMaterialExpression(
								M, UMaterialExpressionVectorParameter::StaticClass(), -700, 840));
					UMaterialExpressionCustom* Pick =
						Cast<UMaterialExpressionCustom>(
							UMaterialEditingLibrary::CreateMaterialExpression(
								M, UMaterialExpressionCustom::StaticClass(), -420, 700));
					if (Sel && Pick)
					{
						Sel->ParameterName = TEXT("MaskSelect");
						Sel->DefaultValue = FLinearColor(1.f, 0.f, 0.f, 0.f);
						Pick->Description = TEXT("BF6 packed mask channel");
						Pick->OutputType = CMOT_Float1;
						// FOUR SCALARS, NOT A SWIZZLE. A vector parameter's
						// default output pin is RGB, a float3, so "Sel.a" is
						// an out-of-bounds swizzle and the whole material
						// fails to compile - which sends every road and decal
						// to the engine default. Taking R, G, B and A as
						// separate pins has no such ambiguity.
						Pick->Code = TEXT("return R * SelR + G * SelG + B * SelB + A * SelA;");
						Pick->Inputs.Empty();
						// Output indices 1..4 on a texture sample are R, G, B
						// and A; index 0 is RGB.
						FCustomInput In;
						In.InputName = TEXT("R"); In.Input.Expression = Cov; In.Input.OutputIndex = 1; Pick->Inputs.Add(In);
						In.InputName = TEXT("G"); In.Input.OutputIndex = 2; Pick->Inputs.Add(In);
						In.InputName = TEXT("B"); In.Input.OutputIndex = 3; Pick->Inputs.Add(In);
						In.InputName = TEXT("A"); In.Input.OutputIndex = 4; Pick->Inputs.Add(In);
						FCustomInput SIn;
						SIn.Input.Expression = Sel;
						SIn.InputName = TEXT("SelR"); SIn.Input.OutputIndex = 1; Pick->Inputs.Add(SIn);
						SIn.InputName = TEXT("SelG"); SIn.Input.OutputIndex = 2; Pick->Inputs.Add(SIn);
						SIn.InputName = TEXT("SelB"); SIn.Input.OutputIndex = 3; Pick->Inputs.Add(SIn);
						SIn.InputName = TEXT("SelA"); SIn.Input.OutputIndex = 4; Pick->Inputs.Add(SIn);
						UMaterialEditingLibrary::ConnectMaterialExpressions(Pick, TEXT(""), OpMul, TEXT("A"));
					}
					else
					{
						UMaterialEditingLibrary::ConnectMaterialExpressions(Cov, TEXT("R"), OpMul, TEXT("A"));
					}
					UMaterialEditingLibrary::ConnectMaterialExpressions(VC, TEXT("A"), OpMul, TEXT("B"));
					UMaterialExpressionScalarParameter* OpacityScale =
						Cast<UMaterialExpressionScalarParameter>(
							UMaterialEditingLibrary::CreateMaterialExpression(
								M, UMaterialExpressionScalarParameter::StaticClass(), 20, 680));
					UMaterialExpressionMultiply* ScaledOpacity =
						Cast<UMaterialExpressionMultiply>(
							UMaterialEditingLibrary::CreateMaterialExpression(
								M, UMaterialExpressionMultiply::StaticClass(), 210, 660));
					if (OpacityScale && ScaledOpacity)
					{
						OpacityScale->ParameterName = TEXT("OpacityScale");
						OpacityScale->DefaultValue = 1.f;
						UMaterialEditingLibrary::ConnectMaterialExpressions(OpMul, TEXT(""), ScaledOpacity, TEXT("A"));
						UMaterialEditingLibrary::ConnectMaterialExpressions(OpacityScale, TEXT(""), ScaledOpacity, TEXT("B"));
						UMaterialEditingLibrary::ConnectMaterialProperty(ScaledOpacity, TEXT(""), MP_Opacity);
					}
					else
					{
						UMaterialEditingLibrary::ConnectMaterialProperty(OpMul, TEXT(""), MP_Opacity);
					}
				}
			}
			else if (VC)
			{
				UMaterialEditingLibrary::ConnectMaterialProperty(VC, TEXT("A"), MP_Opacity);
			}
		}
		else if (Kind == EKind::Decal)
		{
			// Placeable decal colour sheets pack coverage in alpha. The paired
			// normal sheet is unpacked above as RG normal and A smoothness.
			// Colourless puddles/modulators are filtered before material creation:
			// a surface blend cannot write only roughness/normal honestly.
			if (Base) UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT("A"), MP_Opacity);
		}
		else if (Kind == EKind::Masked)
		{
			UMaterialExpressionTextureSampleParameter2D* Cut =
				AddParam(TEXT("Opacity"), SAMPLERTYPE_LinearColor, 600);
			if (Cut)
			{
				Cut->Texture = LinearWhite();
				UMaterialEditingLibrary::ConnectMaterialProperty(Cut, TEXT("R"), MP_OpacityMask);
			}
		}
		else if (Kind == EKind::Translucent)
		{
			// WHAT THE GLASS LETS THROUGH, as a colour rather than a fraction.
			//
			// TransmittanceColor is what survives a view PERPENDICULAR to the
			// surface; the shader takes its log and scales by 1/NoV, so a
			// grazing view goes darker on its own. Tinted glass is this
			// parameter's whole job, and the depot's constant maps onto it as
			// 1 minus the old opacity rather than onto MP_Opacity.
			//
			// SurfaceCoverage stays 1: it is how much of the pixel is glass at
			// all, which is a cutout question, and glass carries no cutout
			// sheet.
			UMaterialExpressionThinTranslucentMaterialOutput* TT =
				Cast<UMaterialExpressionThinTranslucentMaterialOutput>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionThinTranslucentMaterialOutput::StaticClass(),
						100, 600));
			UMaterialExpressionVectorParameter* Trans =
				Cast<UMaterialExpressionVectorParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionVectorParameter::StaticClass(), -400, 600));
			if (Trans)
			{
				Trans->ParameterName = TEXT("TransmittanceColor");
				// THE MEASURED SHIPPED PANE TINT, not a placeholder.
				//
				// 0.75 grey was the old flat 0.25 opacity written as what gets
				// through - invented, never read. Over the 33 distinct glass
				// parameter blobs in one portal_gameplay depot, 20 carry the SAME
				// authored tint (0.791376, 0.856377, 0.890005); it is the value
				// every architecture-glass palette holds, the storage box lid and
				// the vending machine pane included, and the next most common is
				// the vehicle windscreen's 0.441237 grey. A decoded per-record
				// value still replaces this whenever one arrives.
				Trans->DefaultValue = FLinearColor(0.791376f, 0.856377f, 0.890005f);
			}
			if (TT && Trans)
			{
				UMaterialEditingLibrary::ConnectMaterialExpressions(
					Trans, TEXT(""), TT, TEXT("TransmittanceColor"));
			}
			UMaterialExpressionScalarParameter* Cov =
				Cast<UMaterialExpressionScalarParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionScalarParameter::StaticClass(), -400, 700));
			if (Cov)
			{
				Cov->ParameterName = TEXT("SurfaceCoverage");
				Cov->DefaultValue = 1.f;
				if (TT)
				{
					UMaterialEditingLibrary::ConnectMaterialExpressions(
						Cov, TEXT(""), TT, TEXT("SurfaceCoverage"));
				}
			}
			// SubstrateLegacyConversion.ush mixes transmission toward BLACK by
			// root Opacity. It is the opaque surface layer, not pane coverage.
			// Connecting coverage=1 here made every pane fully light-blocking.
			// Coverage belongs exclusively on the thin-translucent output above.
			if (UMaterialExpressionConstant* OpaqueLayer = Cast<UMaterialExpressionConstant>(
				UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant::StaticClass(), -180, 800)))
			{
				OpaqueLayer->R = 0.f;
				UMaterialEditingLibrary::ConnectMaterialProperty(OpaqueLayer, TEXT(""), MP_Opacity);
			}
			// THE PANE'S OWN COLOUR.
			//
			// The architecture-glass family is not dual source: it draws one
			// premultiplied-alpha target whose rgb is an ordinary lit surface
			// colour, so its pane colour belongs on BaseColor and its authored
			// opacity on SurfaceCoverage above. Default BLACK, which is exactly
			// what the unconnected BaseColor gave before, so a record with no
			// decoded glass constants renders identically to yesterday.
			if (UMaterialExpressionVectorParameter* GlassTint =
					Cast<UMaterialExpressionVectorParameter>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionVectorParameter::StaticClass(), -400, 800)))
			{
				GlassTint->ParameterName = TEXT("GlassTint");
				GlassTint->DefaultValue = FLinearColor::Black;
				UMaterialEditingLibrary::ConnectMaterialProperty(
					GlassTint, TEXT(""), MP_BaseColor);
			}
			if (!TT)
			{
				UE_LOG(LogBF6HighPoly, Error,
					TEXT("glass: no ThinTranslucentMaterialOutput node. The engine ")
					TEXT("refuses to compile this shading model without one and the ")
					TEXT("material will fall back to the default grey."));
			}
		}

		if (Kind == EKind::Eye)
		{
			// The decoded eye shader uses separate iris and sclera sheets, joined
			// by saturate(1 - 2*mask.r). The mask is not ordinary white coverage.
			auto* Iris = AddParam(TEXT("EyeIris"), SAMPLERTYPE_Color, 900);
			auto* Sclera = AddParam(TEXT("EyeSclera"), SAMPLERTYPE_Color, 1100);
			auto* Mask = AddParam(TEXT("EyeMask"), SAMPLERTYPE_LinearColor, 1300);
			auto* IrisN = AddParam(TEXT("EyeIrisNormal"), SAMPLERTYPE_Normal, 1500);
			auto* ScleraN = AddParam(TEXT("EyeScleraNormal"), SAMPLERTYPE_Normal, 1700);
			for (auto* Sample : {Iris, Sclera}) Sample->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
			Mask->Texture = LinearWhite(); Mask->SamplerType = SAMPLERTYPE_LinearColor;
			for (auto* Sample : {IrisN, ScleraN}) { Sample->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal")); Sample->SamplerType = SAMPLERTYPE_Normal; }
			auto* Mix = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionCustom::StaticClass(), 0, 1100));
			Mix->OutputType = CMOT_Float4;
			Mix->Inputs.Reset();
			Mix->Code = TEXT("return lerp(Sclera, Iris, saturate(1.0 - 2.0 * Mask));");
			for (const TCHAR* Name : {TEXT("Sclera"),TEXT("Iris"),TEXT("Mask")}) { FCustomInput Input; Input.InputName = Name; Mix->Inputs.Add(Input); }
			UMaterialEditingLibrary::ConnectMaterialExpressions(Sclera,TEXT("RGBA"),Mix,TEXT("Sclera"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Iris,TEXT("RGBA"),Mix,TEXT("Iris"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Mask,TEXT("R"),Mix,TEXT("Mask"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Mix,TEXT(""),MP_BaseColor);
			auto* Normal = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionCustom::StaticClass(), 0, 1500));
			Normal->OutputType = CMOT_Float3;
			Normal->Inputs.Reset();
			Normal->Code = TEXT("float2 xy = lerp(Sclera.xy,Iris.xy,saturate(1.0-2.0*Mask)); return float3(xy,sqrt(saturate(1.0-dot(xy,xy))));");
			for (const TCHAR* Name : {TEXT("Sclera"),TEXT("Iris"),TEXT("Mask")}) { FCustomInput Input; Input.InputName = Name; Normal->Inputs.Add(Input); }
			UMaterialEditingLibrary::ConnectMaterialExpressions(ScleraN,TEXT("RGB"),Normal,TEXT("Sclera"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(IrisN,TEXT("RGB"),Normal,TEXT("Iris"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Mask,TEXT("R"),Normal,TEXT("Mask"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Normal,TEXT(""),MP_Normal);
			auto* Rough = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionCustom::StaticClass(), 300, 1100));
			Rough->OutputType = CMOT_Float1; Rough->Inputs.Reset(); Rough->Code = TEXT("return max(0.04,1.0-Colour.a);");
			FCustomInput Input; Input.InputName = TEXT("Colour"); Rough->Inputs.Add(Input);
			UMaterialEditingLibrary::ConnectMaterialExpressions(Mix,TEXT(""),Rough,TEXT("Colour"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Rough,TEXT(""),MP_Roughness);
		}

		if (Kind == EKind::CarPaint)
		{
			auto* Wrap = AddParam(TEXT("CarWrap"),SAMPLERTYPE_Color,1800);
			Wrap->Texture = LoadObject<UTexture>(nullptr,TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
			auto* Enabled = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(M,UMaterialExpressionScalarParameter::StaticClass(),-400,2000));
			Enabled->ParameterName = TEXT("CarWrapEnabled"); Enabled->DefaultValue = 0.f;
			auto* UV = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(M,UMaterialExpressionTextureCoordinate::StaticClass(),-400,2100));
			auto* Paint = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(M,UMaterialExpressionCustom::StaticClass(),0,1800));
			Paint->OutputType = CMOT_Float3; Paint->Inputs.Reset();
			// Unwrapped panels can sit outside the wrap atlas. Preserve their
			// paint and composite the vinyl through its own coverage, without tinting it.
			Paint->Code = TEXT("float inside=step(0.0,UV.x)*step(UV.x,1.0)*step(0.0,UV.y)*step(UV.y,1.0); return lerp(Paint.rgb,Wrap.rgb,saturate(Wrap.a*Enabled*inside));");
			for (const TCHAR* Name : {TEXT("Paint"),TEXT("Wrap"),TEXT("Enabled"),TEXT("UV")}) { FCustomInput I; I.InputName=Name; Paint->Inputs.Add(I); }
			UMaterialEditingLibrary::ConnectMaterialExpressions(Tint,TEXT(""),Paint,TEXT("Paint"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Wrap,TEXT("RGBA"),Paint,TEXT("Wrap"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Enabled,TEXT(""),Paint,TEXT("Enabled"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(UV,TEXT(""),Paint,TEXT("UV"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Paint,TEXT(""),MP_BaseColor);
		}

		if (Kind == EKind::FxCard)
		{
			M->BlendMode = BLEND_Translucent;
			M->SetShadingModel(MSM_Unlit); M->TwoSided = true;
			auto* Sheet = AddParam(TEXT("FxSheet"),SAMPLERTYPE_Color,1800);
			Sheet->Texture = LoadObject<UTexture>(nullptr,TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
			auto* Time = Cast<UMaterialExpressionTime>(UMaterialEditingLibrary::CreateMaterialExpression(M,UMaterialExpressionTime::StaticClass(),-400,2000));
			auto* Animate = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(M,UMaterialExpressionScalarParameter::StaticClass(),-400,2100));
			Animate->ParameterName = TEXT("FxAnimate"); Animate->DefaultValue = 0.f;
			auto* Visible = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(M,UMaterialExpressionScalarParameter::StaticClass(),-400,2200));
			Visible->ParameterName = TEXT("FxVisible"); Visible->DefaultValue = 0.f;
			auto* Colour = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(M,UMaterialExpressionCustom::StaticClass(),0,1800));
			Colour->OutputType = CMOT_Float3; Colour->Inputs.Reset();
			// Match the established preview's R-G-B-G loop. The playback rate
			// is a preview default until the authored material graph is decoded.
			Colour->Code = TEXT("float t=Clock*0.25; float leg=fmod(floor(t),4.0); float f=smoothstep(0.0,1.0,frac(t)); float a=leg<0.5?Sheet.r:(leg<1.5?Sheet.g:(leg<2.5?Sheet.b:Sheet.g)); float b=leg<0.5?Sheet.g:(leg<1.5?Sheet.b:(leg<2.5?Sheet.g:Sheet.r)); return lerp(Sheet.rgb,lerp(a,b,f).xxx,Animate);");
			for (const TCHAR* Name : {TEXT("Sheet"),TEXT("Clock"),TEXT("Animate")}) { FCustomInput I; I.InputName=Name; Colour->Inputs.Add(I); }
			UMaterialEditingLibrary::ConnectMaterialExpressions(Sheet,TEXT("RGBA"),Colour,TEXT("Sheet"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Time,TEXT(""),Colour,TEXT("Clock"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Animate,TEXT(""),Colour,TEXT("Animate"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Colour,TEXT(""),MP_EmissiveColor);
			auto* Opacity = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(M,UMaterialExpressionMultiply::StaticClass(),0,2100));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Sheet,TEXT("A"),Opacity,TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Visible,TEXT(""),Opacity,TEXT("B"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Opacity,TEXT(""),MP_Opacity);
		}

		// PostEditChange is what a properly-packaged material needs to cache its
		// shaders. RecompileMaterial is deliberately NOT called: it crashed
		// here, and it is the heavyweight path meant for an asset being edited.
		// Full-map objects are emitted through HISM components. Compile that
		// permutation with the reusable parent instead of letting the first placed
		// instance request it after the map has already started rendering.
		M->PreEditChange(nullptr);
		M->PostEditChange();
		// SetMaterialUsage triggers a compile immediately. Calling it before
		// PostEditChange made the compiler inspect an unfinished expression
		// collection and reject even valid engine/default textures as unresolved.
		// Finalize the graph first, then queue the HISM permutation while this
		// synchronous build still has ample work left to overlap with it.
		M->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);

		UE_LOG(LogBF6HighPoly, Log,
			TEXT("parent material %s: %d expression(s), complete=%d (compiling=%d)"), *ParentName,
			M->GetExpressionCollection().Expressions.Num(),
			M->IsComplete() ? 1 : 0, M->IsCompiling() ? 1 : 0);

		// GParents is a native raw-pointer cache.  Actor-owned MIDs are destroyed
		// on a rebuild, so without a root the transient parent can be collected
		// while this pointer remains non-null.  A second build then crashes inside
		// UMaterialInstanceDynamic::Create (RoadMaterialFor was the first observed
		// caller).  These parents are map-independent and are released at module
		// shutdown, matching the already-rooted ground/water parent policy.
		M->AddToRoot();
		CachedParent = M;
		return M;
	}

	// ---------------------------------------------------------------- glass ---
	//
	// WHAT A GLASS RECORD ACTUALLY CARRIES, and why none of it arrives yet.
	//
	// Decoded on 2026-09-05 from the shipped programs, not inferred. The storage
	// box lid's section state key 0x548F7D3EFDC2D06C selects pixel program
	// 27e2a33b-356e-2391-12ce-837a883a476a, which writes ONE SV_Target, and its
	// permutation's render state is src One / dst InvSourceAlpha - premultiplied
	// alpha, NOT the dual-source composite the vehicle-glass family uses. Its tail:
	//
	//     m       = saturate(mask.r * grunge.r + mask.g * grunge.g)
	//     colour  = lerp(paneColour, 0x6BB97444, m)
	//     rough   = 1 - saturate(0x752DA01C - m)
	//     opacity = saturate(0x77DC2A4C + m) * vertexFade
	//
	// paneColour comes from the per-instance buffer, whose material-side default is
	// 0x5F4C0C68, when the lane selector 0xB01E5565 is zero, and from the eight
	// entry 0xA0106346 palette indexed by vertex element usage 51 when it is not.
	// The vehicle family is the dual-source case the parent's Thin Translucent
	// model already matches (findings/glass-is-forward-dualsource-with-a-palette-tint).
	//
	// NONE of those constants reaches this code today: libbf6's resolve_colour
	// never looks at 0xA0106346 or 0x5F4C0C68, and 0x77DC2A4C / 0x6BB97444 /
	// 0x752DA01C have no field in bf6_material_desc, so a glass section arrives
	// white with no bindings at all. The read is requested as an ADDITIVE export so
	// an older dll keeps working: until it exists ReadGlassSection returns false and
	// the parent's measured defaults stand.
	struct FGlassDesc
	{
		// 0 none, 1 vehicle/prop glass (dual source), 2 window/architecture glass.
		int32 Family = 0;
		FLinearColor Tint = FLinearColor::White;
		// False when the eight palette lanes disagree; Tint is then lane 0 only and
		// a consumer with no per-vertex lane must not paint the whole section with it.
		bool  bTintUniform = true;
		float Opacity = -1.f;                      // 0x77DC2A4C, < 0 when absent
		FLinearColor Dirt = FLinearColor::White;   // 0x6BB97444
		float Smoothness = -1.f;                   // 0x752DA01C, < 0 when absent
	};

	// The C mirror of what the requested export writes. Declared HERE rather than
	// in bf6_core.h: that header belongs to the tool, and this compiles against
	// every version of it, including the ones with no glass read at all.
	struct FBf6GlassDesc
	{
		int32 family;
		float tint[3];
		int32 tint_is_uniform;
		float opacity;
		float dirt_color[3];
		float smoothness;
		int32 grunge_texture;
		int32 mask_texture;
	};
	typedef int (*FnMaterialGlass)(bf6_ctx*, const char*, int, const char*, const char*,
	                               int, FBf6GlassDesc*);
	FnMaterialGlass GMaterialGlass = nullptr;
	bool GMaterialGlassTried = false;

	bool GlassReadAvailable()
	{
		if (!GMaterialGlassTried)
		{
			GMaterialGlassTried = true;
			if (void* Dll = GCore.DllHandle())
				GMaterialGlass = (FnMaterialGlass)
					FPlatformProcess::GetDllExport(Dll, TEXT("bf6_material_glass"));
			// Braced: UE_LOG is not a single statement, so an unbraced if/else
			// around two of them does not compile.
			if (GMaterialGlass)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("glass: this bf6_core.dll exposes bf6_material_glass; panes take their own record"));
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("glass: this bf6_core.dll has no bf6_material_glass; panes take the measured shipped tint"));
			}
		}
		return GMaterialGlass != nullptr;
	}

	// One section's glass record, in the SAME scope the mesh was read in: a state
	// key is unique only within a bundle, so the bundle and variation the read used
	// have to come back with the question.
	bool ReadGlassSection(const FString& Res, int32 Lod, const FString& Bundle,
	                      const FString& Variation, int32 Section, FGlassDesc& Out)
	{
		if (!GlassReadAvailable()) return false;
		bf6_ctx* Ctx = GCore.Handle();
		if (!Ctx) return false;
		FBf6GlassDesc D;
		FMemory::Memzero(&D, sizeof(D));
		const FTCHARToUTF8 ResU(*Res), BundleU(*Bundle), VarU(*Variation);
		if (GMaterialGlass(Ctx, ResU.Get(), Lod,
		                   Bundle.IsEmpty() ? nullptr : BundleU.Get(),
		                   Variation.IsEmpty() ? nullptr : VarU.Get(),
		                   Section, &D) != 1)
			return false;
		Out.Family = D.family;
		Out.Tint = FLinearColor(D.tint[0], D.tint[1], D.tint[2], 1.f);
		Out.bTintUniform = D.tint_is_uniform != 0;
		Out.Opacity = D.opacity;
		Out.Dirt = FLinearColor(D.dirt_color[0], D.dirt_color[1], D.dirt_color[2], 1.f);
		Out.Smoothness = D.smoothness;
		return Out.Family != 0;
	}

	// ONE READER CALL PER TEXTURE ID, EVER.
	//
	// MaterialFor asks for a binding's resource name to decide whether the glow
	// slot holds a placeholder, and it does that BEFORE it looks in the material
	// cache - so a scene of thousands of sections paid a native call per glow
	// binding on every hit as well as every miss. The name for an id cannot
	// change inside a mount, so it is remembered.
	//
	// It also stops that call happening on the game thread beside a mount on a
	// worker, which was a race on the same unlocked context.
	// The two answers the glow slot actually needs, not the name they came from,
	// so the string work is done once per id as well as the native call.
	struct FGlowFacts { bool bPlaceholder = false; bool bReal = false; };
	TMap<int32, FGlowFacts> GGlowFactsMemo;

	FGlowFacts GlowFactsFor(int32 Id, const FString& KnownName)
	{
		if (const FGlowFacts* Hit = GGlowFactsMemo.Find(Id)) { return *Hit; }
		FString Name = KnownName;
		if (Name.IsEmpty())
		{
			// ONLY WHEN NOTHING UPSTREAM KEPT IT. bf6_texture_name_at measured
			// at about 0.8 seconds a call on a narrow archive mount, and
			// sixteen of them were 12.8 s of a 26 s level open. Every cached
			// read carries the name now, so this is the live-decode fallback.
			FScopeLock CoreLock(&BF6HP::Shared::CoreMutex());
			Name = GCore.TextureNameAt(Id);
		}
		const FString Leaf = FPaths::GetBaseFilename(Name);
		// WallLamp_Rect_03 proves the two adjacent Frostbite slots are
		// shader-family dependent: its real glow is *_ea while the collapsed
		// public slot currently exposes *_h. A one-channel height sheet sampled
		// as emissive becomes a solid red lamp. It is a mask/placeholder here,
		// never an RGB emission source; an active exact light-source join may
		// still illuminate the dedicated glow geometry with its decoded colour.
		const bool bHeightOnly = Leaf.EndsWith(TEXT("_h"), ESearchCase::IgnoreCase);
		FGlowFacts F;
		// Thirty of the sampled bindings are the flat t_red placeholder. The
		// runtime read path gives us its real resource name, so keep it out
		// without inventing a luminance threshold for actual glow sheets.
		F.bPlaceholder = Name.Contains(TEXT("/textures/default/t_red"), ESearchCase::IgnoreCase)
			|| bHeightOnly;
		F.bReal = !Name.Contains(TEXT("/textures/default/"), ESearchCase::IgnoreCase)
			&& !Name.Contains(TEXT("/textures/debug/"), ESearchCase::IgnoreCase)
			&& !bHeightOnly;
		GGlowFactsMemo.Add(Id, F);
		GGlowFactsMisses++;
		return F;
	}

	// One material instance per section, bound to what the depot said.
	UMaterialInstanceDynamic* MaterialFor(UObject* Outer, const BF6HP::FCore::FSection& S,
	                                      const FLinearColor* RuntimeGlowColor = nullptr,
	                                      float RuntimeGlowStrength = 0.f,
	                                      const FGlassDesc* Glass = nullptr)
	{
		const double KeyStart = FPlatformTime::Seconds();
		bool bHasAlbedo = false, bHasNormal = false;
		bool bHasRealEmissive = false, bHasPlaceholderEmissive = false;
		for (const BF6HP::FCore::FBinding& B : S.Textures)
		{
			bHasAlbedo |= B.Slot == 0 && B.Texture >= 0;
			bHasNormal |= B.Slot == 1 && B.Texture >= 0;
			if (B.Slot == 3 && B.Texture >= 0)
			{
				const double GlowStart = FPlatformTime::Seconds();
				const FGlowFacts F = GlowFactsFor(B.Texture, B.Name);
				GSecGlowFacts += FPlatformTime::Seconds() - GlowStart;
				bHasPlaceholderEmissive |= F.bPlaceholder;
				bHasRealEmissive        |= F.bReal;
			}
		}
		// Fixture glow is a separate runtime state in Frostbite.  The static
		// shader depot deliberately binds t_red in the verified glow slot; it is
		// not a red texture to render.  Promote that slot only when this mesh's
		// source partition also produced a non-zero live light record.  A fixture
		// with the same placeholder but no active source remains the dark control.
		const bool bRuntimeGlow = RuntimeGlowColor && RuntimeGlowStrength > 0.f
			&& bHasPlaceholderEmissive;
		const bool bHasEmissive = bHasRealEmissive || bRuntimeGlow;
		const EKind Kind =
			S.bDecal ? (!bHasAlbedo && bHasNormal ? EKind::Puddle : EKind::Decal)
			: S.bNsm ? EKind::Vista
			: S.Textures.ContainsByPredicate([](const BF6HP::FCore::FBinding& B){ return B.Slot == 10; })
			  && S.Textures.ContainsByPredicate([](const BF6HP::FCore::FBinding& B){ return B.Slot == 11; })
			  && S.Textures.ContainsByPredicate([](const BF6HP::FCore::FBinding& B){ return B.Slot == 14; }) ? EKind::Eye
			: S.Textures.ContainsByPredicate([](const BF6HP::FCore::FBinding& B){ return B.Slot == 15 || B.Slot == 16; }) ? EKind::FxCard
			: S.Textures.ContainsByPredicate([](const BF6HP::FCore::FBinding& B){ return B.Slot == 17; }) ? EKind::CarPaint
			: S.bTranslucent ? EKind::Translucent
			: (S.bAlphaTest ? (S.bAlphaFromAlbedo ? EKind::MaskedVeg : EKind::Masked)
			                : EKind::Opaque);
		// An exact runtime key for every parameter this generic material consumes.
		// Float bit patterns are used rather than rounded text, and binding order is
		// retained because duplicate slots deliberately resolve last-write-wins.
		auto Bits = [](float V)
		{
			uint32 U = 0;
			FMemory::Memcpy(&U, &V, sizeof(U));
			return U;
		};
		FString Key = FString::Printf(TEXT("%d:%d|%08x,%08x,%08x,%08x|%08x"),
			(int32)Kind, bHasEmissive ? 1 : 0, Bits(S.BaseColor.R), Bits(S.BaseColor.G),
			Bits(S.BaseColor.B), Bits(S.BaseColor.A), Bits(S.Roughness));
		for (const BF6HP::FCore::FBinding& B : S.Textures)
			Key += FString::Printf(TEXT("|%d:%d"), B.Slot, B.Texture);
		if (bRuntimeGlow)
			Key += FString::Printf(TEXT("|runtime_glow:%08x,%08x,%08x:%08x"),
				Bits(RuntimeGlowColor->R), Bits(RuntimeGlowColor->G),
				Bits(RuntimeGlowColor->B), Bits(RuntimeGlowStrength));
		// Two panes that differ only in their authored opacity are two
		// materials, so the decoded record has to be part of the key.
		if (Glass && Glass->Family != 0)
			Key += FString::Printf(TEXT("|glass:%d%d:%08x,%08x,%08x:%08x:%08x"),
				Glass->Family, Glass->bTintUniform ? 1 : 0,
				Bits(Glass->Tint.R), Bits(Glass->Tint.G), Bits(Glass->Tint.B),
				Bits(Glass->Opacity), Bits(Glass->Smoothness));
		GSecMaterialKey += FPlatformTime::Seconds() - KeyStart;
		if (UMaterialInstanceDynamic** Cached = GMaterialCache.Find(Key))
		{
			GMaterialCacheHits++;
			return *Cached;
		}
		GMaterialCacheMisses++;
		const double ParentStart = FPlatformTime::Seconds();
		UMaterial* Parent = EnsureParentMaterial(Kind, bHasEmissive);
		GSecParentMaterials += FPlatformTime::Seconds() - ParentStart;
		if (!Parent) return nullptr;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return nullptr;
		// The mesh outlives the call that made it, so the instance has to be
		// rooted to the mesh rather than left for the next collection.
		MID->SetFlags(RF_Transient);
		GMidsMade++;
		// White and 0.5 are the parent's own defaults, so setting them anyway
		// costs a parameter write and keeps the two sides in step.
		if (Kind == EKind::Translucent)
		{
			// The vehicle-glass family composites as dst = radiance + dst * transmission,
			// and Thin Translucent is Unreal's matching dual-source path: feed the
			// tint to that path, never to BaseColor, or the pane becomes a lit white
			// surface. S.BaseColor is white on EVERY glass record today because
			// libbf6's resolve_colour has no rule for the glass palette at all - it
			// is not a collapse the ABI declined to make - so white here means
			// "nothing was read", and the parent's measured default stands.
			const bool bDecodedTransmission =
				!S.BaseColor.Equals(FLinearColor::White, KINDA_SMALL_NUMBER);
			if (bDecodedTransmission)
				MID->SetVectorParameterValue(TEXT("TransmittanceColor"), S.BaseColor);
			float Coverage = 1.f;
			if (Glass && Glass->Family != 0)
			{
				// A non-uniform palette is a per-vertex lane this material cannot
				// index, and painting the whole section with lane 0 makes head
				// lights and tail lights the same grey as the windscreen. Leave
				// those on the measured default rather than spread lane 0.
				if (Glass->bTintUniform)
				{
					MID->SetVectorParameterValue(TEXT("TransmittanceColor"), Glass->Tint);
					// Family 2 is lit as a surface, so the same decoded tint is also
					// its base colour; family 1 only transmits and leaves BaseColor black.
					if (Glass->Family == 2)
						MID->SetVectorParameterValue(TEXT("GlassTint"), Glass->Tint);
				}
				// The authored opacity is exactly what the family-2 program writes to
				// SV_Target0.a before its premultiplied blend, and Thin Translucent's
				// surface coverage is the same quantity: how much of the pixel is
				// glass rather than background. Measured range on one portal depot is
				// 0.04 to 0.50; the storage box lid authors 0.35.
				if (Glass->Family == 2 && Glass->Opacity >= 0.f)
					Coverage = FMath::Clamp(Glass->Opacity, 0.f, 1.f);
			}
			MID->SetScalarParameterValue(TEXT("SurfaceCoverage"), Coverage);
		}
		else
		{
			MID->SetVectorParameterValue(TEXT("BaseColorTint"), S.BaseColor);
		}
		// Glass carries its own smoothness scalar; the generic 0.5 makes a
		// pane look sandblasted. The per-pixel grunge term the program
		// subtracts from it needs the two mask sheets, which the ABI does
		// not expose either, so this is the clean-glass value.
		float RoughnessOut = S.Roughness;
		if (Glass && Glass->Family == 2 && Glass->Smoothness >= 0.f)
			RoughnessOut = FMath::Clamp(1.f - Glass->Smoothness, 0.f, 1.f);
		MID->SetScalarParameterValue(TEXT("Roughness"), RoughnessOut);
		if (bRuntimeGlow)
		{
			MID->SetVectorParameterValue(TEXT("EmissiveTint"), *RuntimeGlowColor);
			MID->SetScalarParameterValue(TEXT("EmissiveEnable"), RuntimeGlowStrength);
		}
		for (const BF6HP::FCore::FBinding& B : S.Textures)
		{
			GBindingsSeen++;
			UTexture2D* T = TextureFor(B.Texture);
			if (!T) continue;
			GBindingsBound++;
			switch (B.Slot)
			{
			case 0: MID->SetTextureParameterValue(TEXT("BaseColor"), T); break;
			case 1: MID->SetTextureParameterValue(TEXT("Normal"), T); break;
			case 3:
			{
				// The same question GlowFactsFor already answered for this
				// binding, so it is asked the same memoised way rather than
				// through another bf6_texture_name_at.
				if (bHasRealEmissive && GlowFactsFor(B.Texture, B.Name).bReal)
				{
					MID->SetTextureParameterValue(TEXT("Emissive"), T);
					MID->SetScalarParameterValue(TEXT("EmissiveEnable"), 1.f);
				}
				break;
			}
			case 10: MID->SetTextureParameterValue(TEXT("EyeIris"), T); break;
			case 11: MID->SetTextureParameterValue(TEXT("EyeSclera"), T); break;
			case 12: MID->SetTextureParameterValue(TEXT("EyeIrisNormal"), T); break;
			case 13: MID->SetTextureParameterValue(TEXT("EyeScleraNormal"), T); break;
			case 14: MID->SetTextureParameterValue(TEXT("EyeMask"), T); break;
            case 15: case 16: MID->SetTextureParameterValue(TEXT("FxSheet"), T); MID->SetScalarParameterValue(TEXT("FxVisible"),1.f); MID->SetScalarParameterValue(TEXT("FxAnimate"), B.Slot == 16 ? 1.f : 0.f); break;
			case 17: MID->SetTextureParameterValue(TEXT("CarWrap"), T); MID->SetScalarParameterValue(TEXT("CarWrapEnabled"),1.f); break;
			case 4: MID->SetTextureParameterValue(TEXT("Opacity"), T); break;   // the cutout sheet
			default: break;   // mro: packed-channel parameters follow
			}
		}
		GMaterialCache.Add(MoveTemp(Key), MID);
		GSecMidBuild += FPlatformTime::Seconds() - ParentStart;
		return MID;
	}

	// The material for one road record. Its sheets come straight off the record
	// rather than through a shader state key, which is why this does not go
	// through MaterialFor.
	// Decal tints above 1.0 over a colour sheet, clamped rather than passed
	// through. See RoadMaterialFor.
	int32 GRoadTintClamped = 0;
	int32 GRoadMarkingsPromoted = 0;
	int32 GRoadWearSoftened = 0;
	int32 GRoadBaseSurfaceBlended = 0;
	int32 GRoadSurfaceBlended = 0;
	int32 GRoadSecondColourUsed = 0;

	bool RoadNameHas(const FString& Name, const TCHAR* Needle)
	{
		return Name.Contains(Needle, ESearchCase::IgnoreCase);
	}

	bool IsRoadMarking(const FString& Name)
	{
		// Semantic asset-family names read from the current install, never
		// per-patch record ids or an exported table.
		return RoadNameHas(Name, TEXT("airfield_text"))
			|| RoadNameHas(Name, TEXT("airstrip"))
			|| RoadNameHas(Name, TEXT("roadmark"))
			|| RoadNameHas(Name, TEXT("road_text"))
			|| RoadNameHas(Name, TEXT("arrow"))
			|| RoadNameHas(Name, TEXT("crosswalk"));
	}

	bool IsRoadWear(const FString& Name)
	{
		return RoadNameHas(Name, TEXT("wear"))
			|| RoadNameHas(Name, TEXT("stain"))
			|| RoadNameHas(Name, TEXT("scraped"))
			|| RoadNameHas(Name, TEXT("crack"));
	}

	UMaterialInstanceDynamic* RoadMaterialFor(UObject* Outer, const BF6HP::FCore::FDecal& D)
	{
		UMaterial* Parent = EnsureParentMaterial(EKind::Road);
		if (!Parent) return nullptr;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return nullptr;
		MID->SetFlags(RF_Transient);
		const FString OpacityName = GCore.TextureNameAt(D.Opacity);
		const bool bMarking = IsRoadMarking(OpacityName);
		const int32 TextureMax = bMarking ? GMarkingTextureMax : GPreviewTextureMax;
		if (UTexture2D* T = TextureFor(D.Albedo, TextureMax))  MID->SetTextureParameterValue(TEXT("BaseColor"), T);
		if (UTexture2D* T = TextureFor(D.Normal, TextureMax))  MID->SetTextureParameterValue(TEXT("Normal"), T);
		if (UTexture2D* T = TextureFor(D.Opacity, TextureMax)) MID->SetTextureParameterValue(TEXT("Opacity"), T);
		if (bMarking)
			MID->SetScalarParameterValue(TEXT("RoadMipBias"), -4.f);

		// THE AUTHORED COLOUR, used two different ways.
		//
		// With a sheet bound the constant MULTIPLIES it, which is what the
		// distribution says: a quarter to four fifths of those records push a
		// channel past 1.0 and one reaches 61. With no sheet the BaseColor
		// sampler stays at its white default, so the same constant becomes the
		// colour itself and the mask decides where it lands.
		if (D.bHasTint || D.bHasTint2)
		{
			FLinearColor C = D.bHasTint ? D.Tint : D.Tint2;
			// The mask-blend class can carry two colour endpoints. The blanket
			// "first Vec3 wins" rule made the airfield outline white even though
			// its live outline record carries dark grey in the second endpoint.
			// Track-wear records with a white identity first endpoint have the same
			// shape. Keep the verified first-colour fallback for every other family
			// while the complete shader equation remains open.
			if (D.Albedo < 0 && D.bHasTint2 &&
				(RoadNameHas(OpacityName, TEXT("outline")) ||
				 RoadNameHas(OpacityName, TEXT("trackwear"))) &&
				(!D.bHasTint ||
				 (D.Tint.R > 0.99f && D.Tint.G > 0.99f && D.Tint.B > 0.99f)))
			{
				C = D.Tint2;
				GRoadSecondColourUsed++;
			}
			// A MULTIPLIER ABOVE 1 CANNOT BE PASSED THROUGH, and passing it
			// through is what turned the carrier decks white.
			//
			// With a colour sheet bound the constant multiplies it, and the
			// authored values run well past 1: on MP_Isolated 108 of the 448
			// tinted records exceed 1.0 and the largest channel on the level
			// is 61.1. Every one of those 108 is above y = 90 - the carrier
			// decks - so a wet-deck sheet was being multiplied by 2.5 to 3.6
			// and saturating to a solid white rectangle.
			//
			// A decal writes into the GBuffer's base colour, which is 0..1, so
			// an unbounded brighten has nowhere to go here regardless. Clamped
			// rather than renormalised: clamping costs the brightening and
			// keeps the hue, where scaling the triple down would keep the
			// brightening and change the colour.
			//
			// OPEN: what the game actually does with a multiplier of 61. That
			// it is a multiplier at all is inferred from its distribution -
			// 40.6% of values above 1.0 with a sheet bound against 1.2%
			// without - not from the shader. If it turns out to drive
			// something other than base colour, this clamp is the wrong shape
			// rather than the wrong value.
			if (D.Albedo >= 0)
			{
				C.R = FMath::Min(C.R, 1.f);
				C.G = FMath::Min(C.G, 1.f);
				C.B = FMath::Min(C.B, 1.f);
				if (C.R < D.Tint.R || C.G < D.Tint.G || C.B < D.Tint.B) GRoadTintClamped++;
			}
			MID->SetVectorParameterValue(TEXT("BaseColorTint"), C);
		}

		float OpacityScale = 1.f;
		// `c-na` records are the base-surface fills: a colour and material sheet,
		// but deliberately no opacity sheet. Frostbite composites these through
		// the terrain's 2D pass BEFORE the ordinary splat layers. Drawing them as
		// fully opaque translucent geometry after the ground replaces all of that
		// later terrain work. Until this proxy is folded into the ground bake,
		// preserve the live surface sheet at a measured partial contribution.
		// This is a rendering calibration against the matched helicopter frame,
		// not a claim that 0.18 is stored in the game data.
		if (D.Albedo >= 0 && D.Opacity < 0)
		{
			OpacityScale = 0.18f;
			GRoadBaseSurfaceBlended++;
		}

		// A zero-colour stain in `lerpblend_top` is not an opaque black
		// replacement for the pavement. Preserve the live mask and vertex fade,
		// but reduce this named near-black family to a surface variation.
		if (D.Albedo < 0 && D.bHasTint &&
			D.Tint.R <= 0.01f && D.Tint.G <= 0.01f && D.Tint.B <= 0.01f &&
			RoadNameHas(OpacityName, TEXT("stain")))
		{
			OpacityScale = 0.12f;
		}
		// `singleline_full` is a spline-built ASPHALT SURFACE, not a painted
		// single line. Treating the word "line" as a marking promoted this dark
		// replacement sheet over the actual runway arrows. Frostbite composites
		// it with the existing terrain surface; our late translucent mesh was an
		// almost-opaque replacement. The matched helicopter view calibrates the
		// contribution while keeping the live sheet, mask and geometry.
		if (RoadNameHas(OpacityName, TEXT("singleline")))
		{
			OpacityScale = 0.25f;
			GRoadSurfaceBlended++;
		}
		// Crack, scraped-plate and track-wear sheets modify a receiving surface;
		// they are not replacement paving. Their shader-specific GBuffer blend is
		// still open, so the translucent proxy must remain a restrained detail
		// layer or the runway becomes the bright crack lattice seen in the failed
		// control frame.
		if (IsRoadWear(OpacityName))
		{
			OpacityScale = RoadNameHas(OpacityName, TEXT("stain")) ? 0.12f : 0.18f;
			GRoadWearSoftened++;
		}
		if (OpacityScale < 1.f)
			MID->SetScalarParameterValue(TEXT("OpacityScale"), OpacityScale);

		// WHICH CHANNEL OF THE MASK IS THIS RECORD'S COVERAGE. Unset means
		// red, which is what the material already defaults to.
		if (D.MaskChannel >= 0 && D.MaskChannel <= 3)
		{
			FLinearColor Sel(0.f, 0.f, 0.f, 0.f);
			(&Sel.R)[D.MaskChannel] = 1.f;
			MID->SetVectorParameterValue(TEXT("MaskSelect"), Sel);
		}
		return MID;
	}

	// NANITE, and why it is not just a flag.
	//
	// BuildFromMeshDescriptions has two paths. bFastBuild goes straight to
	// render buffers and produces a mesh with ONE LOD, no Nanite, and nothing
	// culling its triangles - which is what this used to do, and why a map of
	// 9.7 million triangles across 48,000 instances got choppy. Setting
	// bFastBuild false makes the editor branch call Build(true), the real
	// builder, which is the only thing that ever produces Nanite data.
	//
	// This is the lever the whole port was aimed at. The Godot plugin's own
	// finding is that draw calls are the root cost at about 2.6 microseconds
	// each and that the LOD ladder is a memory lever rather than a draw-call
	// one, which is why distance culling had to stay off there. Nanite removes
	// the constraint instead of working around it.
	//
	// It is not free: the full builder is far slower per mesh than the fast
	// path, and it runs for every distinct asset. That trade is the creator's
	// to make, so it is a switch rather than a decision made here.
	bool GNanite = true;

	// Set the mesh up WITHOUT building it. Building is what costs, and building
	// one mesh at a time is what costs most: BatchBuild takes the whole set and
	// parallelises across it, which is the difference between one core and all
	// of them for the bulk of a map's load.
	// NANITE IS NOT WORTH IT ON A SMALL MESH, and the map is mostly small meshes.
	//
	// Measured on MP_Battery: of 1,447 distinct assets, 948 are under two
	// thousand triangles and those 948 carry 13% of the map's triangles. A
	// Nanite cluster is 128 triangles, so below a couple of thousand there is
	// barely a hierarchy to build - but the build runs anyway, and 1,435 of them
	// cost 292 seconds of CPU. Skipping the small ones drops two thirds of the
	// builds while keeping Nanite on 87% of the geometry.
	// Ten thousand triangles is still only 79 Nanite clusters.  Keeping Nanite
	// for the large assets and every 32K-triangle terrain tile preserves its
	// useful work while avoiding thousands of tiny asynchronous builds that
	// keep the editor busy long after the map first appears.
	int32 GNaniteMinTris = 10000;
	int32 GNaniteCountThisBuild = 0;
	int32 GRuntimeMeshCountThisBuild = 0;
	int32 GNoNaniteTranslucent = 0;
	// Atomic: PrepareRuntimeRenderMesh adds to the first from ParallelFor
	// workers (terrain tiles, props, roads); the second only from the game
	// thread, but they are read together.
	std::atomic<int64> GPreparedTrisThisBuild{ 0 };
	std::atomic<int64> GNaniteTrisThisBuild{ 0 };
	int32 GTerrainTilesBuilt = 0;
	int64 GTerrainVerticesBuilt = 0;
	int64 GTerrainTrianglesBuilt = 0;
	double GTerrainBaseSpacingM = 0.0;
	double GTerrainMinSpacingM = 0.0;
	TArray<int64> GTerrainCellsByLevel;
	TArray<uint8> GTerrainCellLevels;
	int32 GTerrainBaseCells = 0;
	int32 GTerrainNativeStep = 1;

	// Unreal's static-mesh builder validates every supplied tangent basis even
	// when a source mesh has collapsed UV islands.  Frostbite legitimately ships
	// those islands, but a zero tangent/binormal makes UE emit thousands of
	// warnings and then ask MikkTSpace to reconstruct data that cannot be derived
	// from a zero-area UV triangle. Preserve every valid decoded basis and give
	// only invalid vectors a stable orthonormal fallback.
	void SanitizeTangentBasis(FMeshDescription& MD)
	{
		FStaticMeshAttributes Attr(MD);
		TVertexInstanceAttributesRef<FVector3f> Normals = Attr.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attr.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attr.GetVertexInstanceBinormalSigns();
		for (const FVertexInstanceID VI : MD.VertexInstances().GetElementIDs())
		{
			FVector3f N = Normals[VI];
			if (N.ContainsNaN() || N.SizeSquared() < 1.e-8f)
				N = FVector3f(0.f, 0.f, 1.f);
			else
				N.Normalize();

			FVector3f T = Tangents[VI];
			if (!T.ContainsNaN()) T -= N * FVector3f::DotProduct(N, T);
			if (T.ContainsNaN() || T.SizeSquared() < 1.e-8f)
			{
				const FVector3f Axis = FMath::Abs(N.Z) < 0.999f
					? FVector3f(0.f, 0.f, 1.f) : FVector3f(0.f, 1.f, 0.f);
				T = FVector3f::CrossProduct(Axis, N);
			}
			T.Normalize();
			Normals[VI] = N;
			Tangents[VI] = T;
			const float Sign = Signs[VI];
			Signs[VI] = FMath::IsFinite(Sign) && FMath::Abs(Sign) >= 0.5f
				? (Sign < 0.f ? -1.f : 1.f) : 1.f;
		}
	}

	// NANITE CANNOT DRAW TRANSLUCENCY, and asking it to is not a no-op.
	//
	// A mesh with even ONE translucent section - which for this map means any
	// vehicle, because vehicles have glass - is rejected per section by the
	// renderer:
	//
	//   Invalid material used on Nanite static mesh. Only opaque or masked blend
	//   modes are currently supported, BLEND_Translucent was specified.
	//
	// and the section falls back to the ordinary path, which draws the FALLBACK
	// mesh. With the fallback set to nothing (below) that is geometry with its
	// triangles collapsed, which is what a car body pulled into streaks and
	// spikes actually is. Nanite is per mesh, not per section, so the only
	// correct answer is to leave it off for the whole asset.
	void PrepareMesh(UStaticMesh* Mesh, FMeshDescription& MD, int32 Tris,
	                 bool bAllowNanite = true)
	{
		SanitizeTangentBasis(MD);
		GPreparedTrisThisBuild += Tris;
		if (GNanite && bAllowNanite && Tris >= GNaniteMinTris)
		{
			GNaniteCountThisBuild++;
			GNaniteTrisThisBuild += Tris;
			FMeshNaniteSettings N = Mesh->GetNaniteSettings();
			N.bEnabled = true;
			// A REAL FALLBACK, which this deliberately used to skip.
			//
			// The reasoning for skipping it was that a fallback is what a machine
			// without Nanite draws, and this preview always has Nanite - so
			// generating one was pure build cost for geometry never shown. That is
			// wrong about when the fallback is reached. It is not only the
			// no-Nanite path: ray tracing, distance fields, "Disallow Nanite" on a
			// component and any section Nanite refuses all reach for it, and an
			// empty fallback in those paths is not an absence, it is corrupt
			// geometry - collapsed triangles that read as stretching and spikes.
			// Auto lets the builder size it, which is what every ordinary asset in
			// the engine does.
			N.FallbackTarget = ENaniteFallbackTarget::Auto;
			Mesh->SetNaniteSettings(N);
		}
		Mesh->SetNumSourceModels(1);
		FMeshBuildSettings& BuildSettings = Mesh->GetSourceModel(0).BuildSettings;
		BuildSettings.bRecomputeNormals = false;
		BuildSettings.bRecomputeTangents = false;
		BuildSettings.bUseMikkTSpace = false;
		// Several shipped atlas islands and the 16K terrain UV grid have deltas
		// below half-float resolution near 1.0. Quantising before Nanite/DDC makes
		// distinct UVs coincide and recreates the zero tangent we just repaired.
		BuildSettings.bUseFullPrecisionUVs = true;
		BuildSettings.bUseHighPrecisionTangentBasis = true;
		Mesh->CreateMeshDescription(0, MoveTemp(MD));

		UStaticMesh::FCommitMeshDescriptionParams C;
		C.bMarkPackageDirty = false;        // these meshes are transient
		C.bUseHashAsGuid    = true;         // see below
		Mesh->CommitMeshDescription(0, C);
	}

	// Transient collisionless geometry that will not use Nanite does not need an
	// editor asset/DDC build. The runtime path consumes the exact same
	// MeshDescription and materials and creates only its render buffers. Terrain
	// uses it because the tiles already provide culling; small/translucent props
	// use it because the full path would not build Nanite for them anyway. Large
	// opaque props retain the editor/Nanite path where its hierarchy is useful.
	bool PrepareRuntimeRenderMesh(UStaticMesh* Mesh, FMeshDescription& MD, int32 Tris)
	{
		SanitizeTangentBasis(MD);
		GPreparedTrisThisBuild += Tris;
		UStaticMesh::FBuildMeshDescriptionsParams P;
		P.bMarkPackageDirty = false;
		P.bUseHashAsGuid = false;
		P.bBuildSimpleCollision = false;
		P.bCommitMeshDescription = false;
		P.bFastBuild = true;
		P.bAllowCpuAccess = false;
		UStaticMesh::FBuildMeshDescriptionsLODParams LOD;
		LOD.bUseFullPrecisionUVs = true;
		LOD.bUseHighPrecisionTangentBasis = true;
		P.PerLODOverrides.Add(LOD);
		const TArray<const FMeshDescription*> Descriptions{ &MD };
		const bool bBuilt = Mesh->BuildFromMeshDescriptions(Descriptions, P);
		// BuildFromMeshDescriptions creates a BodySetup even when collision was
		// not requested. Registration otherwise asks that empty setup to cook the
		// render triangles, where the fast path intentionally discarded CPU index
		// data. These preview meshes never collide, so explicitly select the empty
		// simple representation and prevent that contradictory cook request.
		if (UBodySetup* Body = Mesh->GetBodySetup())
			Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		return bBuilt;
	}

	// Build everything at once.
	//
	// bUseHashAsGuid above is the other half of this. A mesh's derived data is
	// keyed on its guid, so a fresh guid per build means every rebuild recomputes
	// Nanite from scratch. Keyed on a hash of the CONTENT instead, a rebuild of
	// the same map finds what it built last time already in the cache, and so
	// does the next session.
	void BuildAll(TArray<UStaticMesh*>& Meshes)
	{
		if (Meshes.Num() == 0) return;
		UStaticMesh::FBuildParameters BP;
		BP.bInSilent = true;
		UStaticMesh::BatchBuild(Meshes, BP);
	}

	// One decoded asset turned into a UStaticMesh.
	//
	// Built through MeshDescription rather than a procedural component, because
	// that is the only kind of mesh Unreal will instance - and the only kind
	// Nanite can ever be turned on for. A ProceduralMeshComponent would draw
	// this today and be a dead end tomorrow.
	// PURE, so it can run on any thread. This is where the time went: measured on
	// MP_Battery, decoding cost 1.3 s and Unreal's own builder was dispatched in
	// under half a second, while turning the decode into mesh descriptions took
	// 9.9 s on one core. It is also the phase that parallelises perfectly,
	// because each description is built alone and shares nothing.
	bool DescribeMesh(const TArray<BF6HP::FCore::FSection>& Sections, FMeshDescription& MD, int32& OutTris)
	{
		OutTris = 0;
		FStaticMeshAttributes Attr(MD);
		Attr.Register();

		TVertexAttributesRef<FVector3f>       VPos = Attr.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector2f> VUV  = Attr.GetVertexInstanceUVs();

		// ONE POLYGON GROUP PER SECTION. A section is the unit the depot binds
		// a material to, so merging them into one group throws that away: the
		// whole prop would get whichever material happened to be first, and a
		// building would render as one big sheet of its doorframe.
		for (int32 si = 0; si < Sections.Num(); si++)
		{
			const BF6HP::FCore::FSection& S = Sections[si];
			const FPolygonGroupID Group = MD.CreatePolygonGroup();
			Attr.GetPolygonGroupMaterialSlotNames()[Group] =
				FName(*FString::Printf(TEXT("S%d"), si));
			TArray<FVertexID> Verts;
			Verts.Reserve(S.Pos.Num());
			for (const FVector3f& P : S.Pos)
			{
				const FVertexID V = MD.CreateVertex();
				// Y-up metres to Z-up centimetres, the same swap the transforms
				// get, so geometry and placement cannot disagree.
				VPos[V] = FVector3f(P.X, P.Z, P.Y) * 100.f;
				Verts.Add(V);
			}

			for (int32 i = 0; i + 2 < S.Idx.Num(); i += 3)
			{
				const uint32 a = S.Idx[i], b = S.Idx[i + 1], c = S.Idx[i + 2];
				if (!Verts.IsValidIndex(a) || !Verts.IsValidIndex(b) || !Verts.IsValidIndex(c)) continue;

				// WOUND BACKWARDS ON PURPOSE. Swapping two axes mirrors the
				// space, and a mirrored triangle faces inward: leave the winding
				// alone and every prop is inside out, which reads as "the model
				// is broken" rather than "the handedness changed".
				const uint32 tri[3] = { a, c, b };
				TArray<FVertexInstanceID> Corners;
				for (uint32 idx : tri)
				{
					const FVertexInstanceID VI = MD.CreateVertexInstance(Verts[idx]);
					if (S.Nrm.IsValidIndex((int32)idx))
					{
						const FVector3f& N = S.Nrm[(int32)idx];
						VNrm[VI] = FVector3f(N.X, N.Z, N.Y);
					}
					if (S.UV.IsValidIndex((int32)idx)) VUV.Set(VI, 0, S.UV[(int32)idx]);
					Corners.Add(VI);
				}
				MD.CreatePolygon(Group, Corners);
			}
		}
		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
		FStaticMeshOperations::ComputeTangentsAndNormals(MD,
			EComputeNTBsFlags::Tangents | EComputeNTBsFlags::IgnoreDegenerateTriangles);
		OutTris = MD.Triangles().Num();
		return MD.Vertices().Num() > 0;
	}

	// The UObject half, which has to be on the game thread: the mesh object,
	// its slot materials and the choice between the Nanite build and the
	// runtime render path. For the runtime path the BodySetup is created HERE
	// so that the render-data build can run on a worker afterwards -
	// BuildFromMeshDescriptions would otherwise NewObject one from that thread.
	UStaticMesh* CreateMeshObject(UObject* Outer, const FString& Name, int32 Tris,
	                              const TArray<BF6HP::FCore::FSection>& Sections,
	                              bool& bOutNanite,
	                              const FLinearColor* RuntimeGlowColor = nullptr,
	                              float RuntimeGlowStrength = 0.f,
	                              const TArray<FGlassDesc>* Glass = nullptr)
	{
		bOutNanite = false;
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, *Name, RF_Transient);

		// ONE SLOT PER SECTION, in the same order DescribeMesh created the
		// polygon groups, so slot i is section i. The two are kept in step by
		// the slot NAME ("S<i>"), which both sides build the same way - a
		// mismatch here does not error, it just renders the wrong material on
		// the wrong triangles.
		const double MaterialStart = FPlatformTime::Seconds();
		for (int32 si = 0; si < Sections.Num(); si++)
		{
			FStaticMaterial SM;
			SM.MaterialSlotName  = FName(*FString::Printf(TEXT("S%d"), si));
			SM.ImportedMaterialSlotName = SM.MaterialSlotName;
			SM.MaterialInterface = MaterialFor(
				Mesh, Sections[si], RuntimeGlowColor, RuntimeGlowStrength,
				(Glass && Glass->IsValidIndex(si)) ? &(*Glass)[si] : nullptr);
			Mesh->GetStaticMaterials().Add(SM);
		}
		if (Mesh->GetStaticMaterials().Num() == 0)
			Mesh->GetStaticMaterials().Add(FStaticMaterial());
		GSecObjectMaterials += FPlatformTime::Seconds() - MaterialStart;

		// Glass is what costs a vehicle its Nanite, and it is worth knowing how
		// often: reported once per build rather than per mesh.
		bool bTranslucent = false;
		for (const BF6HP::FCore::FSection& S : Sections)
			if (S.bTranslucent || S.bDecal) { bTranslucent = true; break; }
		if (bTranslucent) GNoNaniteTranslucent++;

		bOutNanite = GNanite && !bTranslucent && Tris >= GNaniteMinTris;
		if (!bOutNanite) Mesh->CreateBodySetup();
		return Mesh;
	}

	// The geometry half. A Nanite mesh commits its description for BatchBuild
	// on the game thread (it touches the source model); a runtime mesh builds
	// its render buffers, which may run on a worker once the BodySetup exists.
	bool CommitMeshGeometry(UStaticMesh* Mesh, FMeshDescription& MD, int32 Tris, bool bNanite)
	{
		if (bNanite)
		{
			PrepareMesh(Mesh, MD, Tris, true);
			return true;
		}
		return PrepareRuntimeRenderMesh(Mesh, MD, Tris);
	}

	// Both halves in one call, serial: what the small passes (scatter) use.
	UStaticMesh* MakeMesh(UObject* Outer, const FString& Name, FMeshDescription& MD, int32 Tris,
	                      const TArray<BF6HP::FCore::FSection>& Sections,
	                      bool& bOutNeedsBatchBuild,
	                      const FLinearColor* RuntimeGlowColor = nullptr,
	                      float RuntimeGlowStrength = 0.f,
	                      const TArray<FGlassDesc>* Glass = nullptr)
	{
		bOutNeedsBatchBuild = false;
		bool bNanite = false;
		UStaticMesh* Mesh = CreateMeshObject(Outer, Name, Tris, Sections, bNanite,
			RuntimeGlowColor, RuntimeGlowStrength, Glass);
		if (!Mesh) return nullptr;
		const double PrepareStart = FPlatformTime::Seconds();
		const bool bOk = CommitMeshGeometry(Mesh, MD, Tris, bNanite);
		GSecObjectPrepare += FPlatformTime::Seconds() - PrepareStart;
		if (!bOk)
		{
			UE_LOG(LogBF6HighPoly, Error, TEXT("runtime render build failed for %s"), *Name);
			return nullptr;
		}
		bOutNeedsBatchBuild = bNanite;
		if (!bNanite) GRuntimeMeshCountThisBuild++;
		return Mesh;
	}

	// THE LAYERS, and what they mean.
	//
	// The Godot plugin's dock is a list of these with a switch on each, and the
	// point of it is that a creator can take the real world apart while looking
	// at it. Only layers that actually exist are listed: an inert switch for
	// something unported would be a lie told in the interface.
	enum class ELayer : uint8 { Terrain, Roads, Objects, Scatter, Water, Lighting, Count };

	struct FLayer
	{
		const TCHAR* Name;
		const TCHAR* Hint;
		bool         bOn = true;
	};


	FLayer GLayers[(int32)ELayer::Count] = {
		{ TEXT("Terrain"), TEXT("The real ground, at the shape the game ships. Preview only: it is never saved into your map or exported.") },
		{ TEXT("Roads"), TEXT("Lane markings, crossings, mud and wear, draped on the ground. The road surface itself is the terrain; this is what is painted on it.") },
		{ TEXT("Objects"), TEXT("Every prop, building and fixture the level places, at its real transform. Preview only.") },
		{ TEXT("Scatter"), TEXT("Ground clutter from the level's shipped mesh catalogue, reconstructed deterministically over its live terrain paint. Species and distances are exact; positions are approximate because retail data does not contain them.") },
		{ TEXT("Water"), TEXT("The level's water surfaces, at the game's own heights and colours, drawn with Unreal's real water shading - depth absorption, reflections, refraction.") },
		{ TEXT("Lighting"), TEXT("The map's own lighting: the authored sun angle, colour and illuminance, its sky and fog, and every lamp, spot and lit panel the level places. Preview only.") },
	};

	// THE LOW-POLY MAP IS HIDDEN BECAUSE SOMETHING REPLACED IT, not because
	// somebody ticked a box. It used to be a switch in the panel, which asked
	// the wrong question: nobody wants "hide the blockout", they want "show me
	// the real thing", and the blockout going away is a consequence of that.
	//
	// So it follows the build: once the terrain or the objects have been read,
	// the tool's stand-ins for them have been replaced and are hidden. The
	// actors are only HIDDEN, never removed, so anyone who wants one back can
	// turn it on in the scene tree like any other actor.
	bool HideLowPolyNow()
	{
		if (!GBuiltAnything) { return false; }
		return GLayers[(int32)ELayer::Terrain].bOn || GLayers[(int32)ELayer::Objects].bOn;
	}

	// Components are named by layer so a switch can find them again without
	// holding pointers across a rebuild, which is what would dangle when a map
	// closes underneath us.
	const TCHAR* LayerPrefix(ELayer L)
	{
		return L == ELayer::Terrain  ? TEXT("Terrain")
			 : L == ELayer::Roads    ? TEXT("RoadMesh_")
			 : L == ELayer::Scatter  ? TEXT("Scatter_")
			 : L == ELayer::Water    ? TEXT("Water_")
			 : L == ELayer::Lighting ? TEXT("Light_") : TEXT("I_");
	}

	void ApplyLayer(ELayer L)
	{
		if (!GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return;
		const FName Owner(*(FString(TEXT("addon:")) + kAddonName));
		const bool bOn = GLayers[(int32)L].bOn;
		const FString Prefix = LayerPrefix(L);

		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(Owner)) continue;
			TArray<USceneComponent*> Comps;
			It->GetComponents<USceneComponent>(Comps);
			for (USceneComponent* C : Comps)
				if (C && C->GetName().StartsWith(Prefix))
					C->SetVisibility(bOn, false);
		}
	}

	// The low-poly map is the tool's, so it is asked rather than reached into.
	void ApplyLowPoly()
	{
		BF6Ext::SetLowPolyMapHidden(HideLowPolyNow());
	}

	// How finely the ground is rebuilt.
	//
	// The streaming tree holds 16,385 samples a side on a Portal map, which is
	// 268 million vertices and not a thing to hand Unreal. 1,025 gives a metre
	// every 8 across an 8 km map, which reads correctly at every distance a
	// creator actually works at. The full resolution is still THERE, and this
	// is the only thing standing between the two, so raising it is one number.
	// HOW FINE THE GROUND IS BUILT, and why 1025 was not enough.
	//
	// This is a vertex count per side across the WHOLE map, so what it really
	// sets is metres per vertex, and the map's size is the other half of that
	// sum. On a 4 km map 1025 is a vertex every 4 metres. Measured against the
	// tool's own ground on MP_Battery, over 140,855 of its vertices:
	//
	//   built   m/vertex   median error   within 0.5 m
	//   1025      4.00        -0.01 m         89.5%
	//   2049      2.00        -0.01 m         96.4%
	//   4097      1.00        -0.01 m         98.5%
	//
	// The MEDIAN never moves, which is the useful part: the rebuild is not
	// sitting low, it is sitting coarse. Point-sampling every fourth metre cuts
	// across anything narrower than the step, and a narrow raised feature - a
	// berm, a kerb, a raised emplacement - is left at the level of the ground
	// beside it. That is one-sided in the built-up parts of a map, where the
	// fine structure is what was added, so it reads as ground that sits too low
	// exactly where there is most to look at.
	//
	// 2049 is now the BASE grid. The builder compares every native sample in a
	// base cell with that cell's bilinear surface and locally restores the
	// game's 2 m, 1 m and 0.5 m height samples where the coarse plane is wrong.
	// Flat ground stays cheap; kerbs, berms and retaining-wall transitions do
	// not get averaged across a four-metre triangle.
	int32 GTerrainSide = 2049;

	// The ground, as one static mesh.
	//
	// A grid rather than a Landscape actor on purpose: a Landscape is an
	// editable, owned thing with its own layers and its own file footprint, and
	// this is a VIEW of the game's data that has to be able to disappear when
	// the add-on does.
	// ONE MESH PER TILE, NOT ONE MESH FOR THE MAP.
	//
	// At 2049 a side the ground is 8.4 million triangles, and as a single
	// static mesh that is one Nanite build: a serial job on one core that the
	// editor waits out with the interface locked, which is what "the terrain
	// took forever and lagged like crazy" is.
	//
	// Cut into tiles the same triangles cost the same total work and finish in
	// a fraction of the time, because BatchBuild runs them across every core -
	// the props already go through it and 1,447 of them dispatch in half a
	// second. Tiles also give the renderer something to cull, where one
	// map-sized mesh is all-or-nothing.
	//
	// TILES MUST SHARE THEIR EDGE VERTICES or the seams crack open. Tile k
	// spans samples [k*per, (k+1)*per] INCLUSIVE at both ends, so the last row
	// of one tile is the first row of the next, computed from the same sample
	// and landing on the same position bit for bit.
	int32 GTerrainTile = 128;          // quads per tile side

	// Ground bake resolution. 2048 over a 4 km map is about two metres a
	// texel, which reads correctly from the air and softens underfoot; the
	// cost is 16 MB of RGBA per sheet.
	int32 GGroundBakeSize = 2048;   // replaced per map by SizeForExtent()

	// ---- the ground, from the game's own layer materials -----------------
	//
	// The terrain mesh used to draw with no material at all, which is the
	// flat grey. The bake gives a top-down albedo and normal for a world
	// window, so the material samples them by WORLD POSITION rather than by
	// any UV the mesh carries - the ground is a grid built from a
	// heightfield and its UVs are an implementation detail, while the bake's
	// world rectangle is exact.
	UMaterial* GGroundParent = nullptr;
	UTexture2D* GGroundAlbedo = nullptr;

	// THE GROUND BEYOND THE PLAYABLE BOX.
	//
	// The coverage raster and the near bake are both cropped to the playable
	// box, which is right - it is what buys 0.62 m a texel instead of 2.00.
	// But the terrain MESH is built over the whole heightfield: on MP_Isolated
	// that is 8,192 m of ground against a 2,555 m box, so by AREA about 90% of
	// the terrain lies outside the textured window, samples outside 0..1, and
	// clamps to a border texel. It reads as the whole map being smeared.
	//
	// So the far field gets its own bake over the full footprint. It is coarse
	// on purpose - 8 m a texel on a 4 km map - because it is only ever seen
	// past the box edge, and the near path keeps every bit of its density.
	UTexture2D* GGroundFar = nullptr;
	FVector2D   GFarLo = FVector2D::ZeroVector;
	FVector2D   GFarSpan = FVector2D(1, 1);
	// The playable box in world XZ, so the shader knows where to cross over.
	FVector2D   GBoxCentre = FVector2D::ZeroVector;
	FVector2D   GBoxHalf = FVector2D(1, 1);
	UTexture2D* GGroundNormal = nullptr;

	UMaterial* EnsureGroundMaterial()
	{
		if (GGroundParent) return GGroundParent;
		UPackage* Pkg = CreatePackage(TEXT("/Temp/BF6HighPoly_Ground"));
		if (!Pkg) return nullptr;
		Pkg->SetFlags(RF_Transient);
		UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_BF6HighPoly_Ground"), RF_Transient);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_DefaultLit);
		M->BlendMode = BLEND_Opaque;
		M->TwoSided = false;

		UMaterialExpressionTextureSampleParameter2D* Alb =
			Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, 0));
		UMaterialExpressionTextureSampleParameter2D* Nrm =
			Cast<UMaterialExpressionTextureSampleParameter2D>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, 300));
		if (Alb)
		{
			Alb->ParameterName = TEXT("GroundAlbedo");
			Alb->SamplerType = SAMPLERTYPE_Color;
			Alb->Texture = LoadObject<UTexture2D>(nullptr,
				TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		}
		if (Nrm)
		{
			Nrm->ParameterName = TEXT("GroundNormal");
			Nrm->SamplerType = SAMPLERTYPE_LinearColor;   // it is not a compressed normal
			Nrm->Texture = LinearWhite();
		}

		// WORLD-POSITION UVs. uv = (worldXY_metres - Lo) / Span, with the
		// rectangle handed in as parameters so one material serves every map
		// and every window.
		UMaterialExpressionWorldPosition* WP =
			Cast<UMaterialExpressionWorldPosition>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionWorldPosition::StaticClass(), -900, 120));
		UMaterialExpressionVectorParameter* Lo =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, 200));
		UMaterialExpressionVectorParameter* Span =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, 260));
		if (Lo) { Lo->ParameterName = TEXT("GroundLo"); Lo->DefaultValue = FLinearColor::Black; }
		if (Span) { Span->ParameterName = TEXT("GroundSpan"); Span->DefaultValue = FLinearColor(1,1,1,1); }

		UMaterialExpressionCustom* UV =
			Cast<UMaterialExpressionCustom>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCustom::StaticClass(), -650, 150));
		if (UV && WP && Lo && Span)
		{
			UV->Code = TEXT("return (WPos.xy * 0.01 - Lo.xy) / max(Span.xy, 1.0);");
			UV->OutputType = CMOT_Float2;
			UV->Description = TEXT("BF6 ground world UV");
			UV->Inputs.Empty();
			auto In = [&UV](const TCHAR* Nm, UMaterialExpression* E)
			{ FCustomInput I; I.InputName = Nm; I.Input.Expression = E; UV->Inputs.Add(I); };
			In(TEXT("WPos"), WP);
			In(TEXT("Lo"), Lo);
			In(TEXT("Span"), Span);
			if (Alb) UMaterialEditingLibrary::ConnectMaterialExpressions(UV, TEXT(""), Alb, TEXT("UVs"));
			if (Nrm) UMaterialEditingLibrary::ConnectMaterialExpressions(UV, TEXT(""), Nrm, TEXT("UVs"));
		}
		if (Alb) UMaterialEditingLibrary::ConnectMaterialProperty(Alb, TEXT(""), MP_BaseColor);
		// The bake's normal is 0.5n+0.5 in TANGENT space, stored uncompressed,
		// so it is unpacked here rather than sampled as a normal map.
		if (Nrm)
		{
			UMaterialExpressionConstantBiasScale* BS =
				Cast<UMaterialExpressionConstantBiasScale>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionConstantBiasScale::StaticClass(), -150, 300));
			if (BS)
			{
				BS->Bias = -0.5f; BS->Scale = 2.f;
				UMaterialEditingLibrary::ConnectMaterialExpressions(Nrm, TEXT(""), BS, TEXT(""));
				UMaterialEditingLibrary::ConnectMaterialProperty(BS, TEXT(""), MP_Normal);
			}
		}
		UMaterialExpressionScalarParameter* Rg =
			Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionScalarParameter::StaticClass(), -400, 500));
		if (Rg)
		{
			Rg->ParameterName = TEXT("GroundRoughness");
			Rg->DefaultValue = 0.9f;   // ground is not shiny
			UMaterialEditingLibrary::ConnectMaterialProperty(Rg, TEXT(""), MP_Roughness);
		}

		M->PreEditChange(nullptr);
		M->PostEditChange();
		// ROOTED, because this is a raw global pointing at a transient object.
		// Every texture below is already rooted; the materials were not, so a
		// garbage collection between two builds - a map load is enough - left
		// both globals dangling and the next build read freed memory. That is
		// an access violation on 0xffffffffffffffff, and it took a crash in
		// the self-check to notice.
		M->AddToRoot();
		GGroundParent = M;
		return M;
	}

	// A square RGBA8 texture WITH A MIP CHAIN, from tightly packed source.
	//
	// UTexture2D::CreateTransient allocates exactly one mip and there is no
	// parameter to ask for more, so anything built through it has no mip to
	// fall to under minification. On a full-screen blit that is invisible; on
	// ground seen from a distance it is not, because a 2048-texel raster
	// stretched over four kilometres puts many texels under one screen pixel
	// and the sampler just picks one of them. That reads as PIXELATION that
	// appears as you back away and is absent up close, which is exactly the
	// symptom this fixed.
	//
	// `Stride` is the source bytes per texel: 4 for RGBA, 3 for tight RGB.
	// Channels are written out in the platform's BGRA order.
	UTexture2D* MakeMippedTexture(const uint8* Src, int32 N, int32 Stride, bool bSrgb)
	{
		if (!Src || N <= 0 || (Stride != 3 && Stride != 4)) return nullptr;
		const int32 NumMips = FMath::Max(1, (int32)FMath::FloorLog2((uint32)N) + 1);

		UTexture2D* Tex = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!Tex) return nullptr;
		Tex->SetPlatformData(new FTexturePlatformData());
		Tex->GetPlatformData()->SizeX = N;
		Tex->GetPlatformData()->SizeY = N;
		Tex->GetPlatformData()->PixelFormat = PF_B8G8R8A8;
		Tex->bNotOfflineProcessed = true;
		Tex->SRGB = bSrgb;
		Tex->CompressionSettings = bSrgb ? TC_Default : TC_VectorDisplacementmap;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		Tex->Filter = TF_Trilinear;
		Tex->NeverStream = true;

		// Working copy in BGRA, halved as the chain goes down.
		TArray<uint8> Cur;
		Cur.SetNumUninitialized(N * N * 4);
		for (int32 i = 0; i < N * N; i++)
		{
			Cur[i * 4 + 0] = Src[i * Stride + 2];
			Cur[i * 4 + 1] = Src[i * Stride + 1];
			Cur[i * 4 + 2] = Src[i * Stride + 0];
			Cur[i * 4 + 3] = Stride == 4 ? Src[i * Stride + 3] : 255;
		}

		int32 Dim = N;
		for (int32 m = 0; m < NumMips; m++)
		{
			FTexture2DMipMap* Mip = new FTexture2DMipMap(Dim, Dim, 1);
			Tex->GetPlatformData()->Mips.Add(Mip);
			Mip->BulkData.Lock(LOCK_READ_WRITE);
			void* Out = Mip->BulkData.Realloc((int64)Dim * Dim * 4);
			FMemory::Memcpy(Out, Cur.GetData(), (SIZE_T)Dim * Dim * 4);
			Mip->BulkData.Unlock();

			if (m + 1 < NumMips)
			{
				const int32 Half = FMath::Max(1, Dim / 2);
				TArray<uint8> Next;
				Next.SetNumUninitialized(Half * Half * 4);
				for (int32 y = 0; y < Half; y++)
				{
					for (int32 x = 0; x < Half; x++)
					{
						const int32 x0 = FMath::Min(x * 2, Dim - 1);
						const int32 x1 = FMath::Min(x * 2 + 1, Dim - 1);
						const int32 y0 = FMath::Min(y * 2, Dim - 1);
						const int32 y1 = FMath::Min(y * 2 + 1, Dim - 1);
						for (int32 c = 0; c < 4; c++)
						{
							const int32 Sum =
								Cur[(y0 * Dim + x0) * 4 + c] + Cur[(y0 * Dim + x1) * 4 + c] +
								Cur[(y1 * Dim + x0) * 4 + c] + Cur[(y1 * Dim + x1) * 4 + c];
							Next[(y * Half + x) * 4 + c] = (uint8)(Sum / 4);
						}
					}
				}
				Cur = MoveTemp(Next);
				Dim = Half;
			}
		}

		Tex->UpdateResource();
		Tex->AddToRoot();
		return Tex;
	}

	// One RGBA8 texture from a bake buffer.
	UTexture2D* MakeBakeTexture(const uint8* Pixels, int32 N, bool bSrgb)
	{
		return MakeMippedTexture(Pixels, N, /*stride*/ 4, bSrgb);
	}

	// ---- the ground PER PIXEL ---------------------------------------------
	//
	// The bake above is one raster for the whole map. Over four kilometres
	// that is two to four metres a texel, while the ground materials
	// themselves repeat every one to seven metres - so every material is
	// averaged away before the renderer ever sees it, and the ground reads as
	// a low-resolution photograph of ground. That is the "low res image" the
	// terrain has looked like.
	//
	// The fix is to stop baking the materials and bake only the MIXING
	// WEIGHTS, which vary slowly and rasterise happily at a few metres. The
	// sheets then get sampled per pixel at their own tiling, so all the detail
	// arrives at full resolution.
	//
	// The bake is still built and still used: it is the correct FAR field.
	// Past a hundred metres a screen pixel covers more than a coverage texel
	// and the honest answer is the average, which is exactly what the bake
	// holds - and it has a mip chain, which the coverage rasters cannot have
	// (the mip of an index is not an index).
	// Coverage and bake resolution are chosen PER MAP, not fixed.
	//
	// A constant 2048 means a 4 km map gets 2 metres a texel and an 8 km map
	// gets 4, so the bigger maps were drawing at half the density for no
	// reason other than the number being hard-coded. The initial choice aims at
	// about two metres a texel and is capped because the cost is quadratic.
	int32 GGroundTexelM = 2;         // metres per texel to aim for
	// The coverage API then crops that raster to the smaller playable box.
	// The flattened far-field bake does not need the same density as the live
	// material coverage.  Keep its speed-pass cap, but preserve sub-metre IDs
	// and weights near the camera: at 2048 Tsuru's 2,555 m coverage box visibly
	// quantizes transitions into 1.25 m cells; 4096 restores 0.62 m cells.
	int32 GGroundBakeSizeMax     = 2048;
	int32 GGroundCoverageSizeMax = 4096;
	int32 GSheetSize    = 512;       // one array slice
	int32 GBenchGroundDebug = INDEX_NONE;
	int32 GBenchStochastic = INDEX_NONE;

	// Keep the authored-sheet evaluator near the camera and fade to the flattened
	// CPU bake at distance.  Extending the generic evaluator over the whole map
	// exposes its known layer-order error as a square quilt; it is a diagnostic,
	// not a substitute for Frostbite's final virtual-texture compositor.
	float GBlendNear    = 0.f;
	float GBlendFar     = 1.f;

	int32 SizeForExtent(double ExtentM, int32 MaxSize)
	{
		const int32 Want = (int32)FMath::RoundUpToPowerOfTwo(
			(uint32)FMath::Max(1.0, ExtentM / FMath::Max(1, GGroundTexelM)));
		return FMath::Clamp(Want, 2048, MaxSize);
	}

	UMaterial*       GGroundBlendParent = nullptr;
	UTexture2D*      GCovIdxTex = nullptr;
	UTexture2D*      GCovWTex   = nullptr;
	UTexture2D*      GCovIdxTex2 = nullptr;
	UTexture2D*      GCovWTex2   = nullptr;
	UTexture2D*      GCovParamTex = nullptr;
	UTexture2D*      GCovColourTex = nullptr;
	UTexture2DArray* GSheetArray  = nullptr;
	UTexture2DArray* GHeightArray = nullptr;
	UTexture2DArray* GMaskArray   = nullptr;
	// Optional diagnostic page produced by the shipped terrain DXIL in our
	// standalone D3D12 harness. It is never loaded from a staged image: the
	// harness reads the current Steam install and returns base-colour plus packed
	// normal AOVs over stdout, which Unreal uploads to transient textures.
	UTexture2D*      GExactPageTex = nullptr;
	UTexture2D*      GExactMaterialTex = nullptr;
	UTexture2D*      GExactNormalTex = nullptr;
	bool             GExactPageBusy = false;
	FString          GExactPageStatus = TEXT("disabled; approximate terrain is active");
	uint64           GMapEpoch = 0;

	// An RGBA8 raster that must be read EXACTLY as authored: no sRGB curve, no
	// filtering, no mips. A bilinear read of a layer index is a different
	// layer, so this is the one texture in the material that must be point
	// sampled.
	UTexture2D* MakeRawTexture(const uint8* Px, int32 N, int32 SourceSlots,
	                           int32 FirstSlot, uint8 Fill, bool bPoint)
	{
		if (!Px || N <= 0 || SourceSlots <= 0) return nullptr;
		UTexture2D* Tex = UTexture2D::CreateTransient(N, N, PF_B8G8R8A8);
		if (!Tex) return nullptr;
		Tex->SRGB = false;
		Tex->CompressionSettings = TC_VectorDisplacementmap;   // uncompressed RGBA8
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		Tex->Filter = bPoint ? TF_Nearest : TF_Bilinear;
		Tex->NeverStream = true;
		if (uint8* D = (uint8*)Tex->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))
		{
			for (int32 i = 0; i < N * N; i++)
			{
				uint8 V[4] = {Fill, Fill, Fill, Fill};
				for (int32 c = 0; c < 4; c++)
					if (FirstSlot + c < SourceSlots)
						V[c] = Px[(int64)i * SourceSlots + FirstSlot + c];
				D[i * 4 + 0] = V[2];
				D[i * 4 + 1] = V[1];
				D[i * 4 + 2] = V[0];
				D[i * 4 + 3] = V[3];
			}
			Tex->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		Tex->UpdateResource();
		Tex->AddToRoot();
		return Tex;
	}

	// The aerial colour map arrives as tight RGB, three bytes a texel, because
	// that is how it lies in the streaming tree. A texture wants four.
	UTexture2D* MakeColourTexture(const uint8* Rgb, int32 N)
	{
		// NOT sRGB, and the old comment had it exactly backwards.
		//
		// The colour map ships _UNORM, so the game's shader reads the RAW
		// BYTE, and the Overlay blend is authored against that byte. De-gamma
		// on the way in turns Overlay's 0.500 identity into 0.216 and the
		// ground comes out dark.
		//
		// Measured on MP_Isolated's land: a layer colour of (125.9, 119.3,
		// 109.6) should composite to (135.0, 128.0, 117.7) and was producing
		// (98.7, 93.4, 85.6). That is 27% darker on land and 62% darker over
		// the reef. The control is libbf6's own bake of the same ground with
		// the same colour map, which measures (136.1, 128.5, 117.8) - the
		// predicted correct value to about one part in 255.
		//
		// The fallback three functions down already gets this right and says
		// so ("0.5 LINEAR, not 0.5 sRGB"). The two paths were contradicting
		// each other.
		return MakeMippedTexture(Rgb, N, /*stride*/ 3, /*sRGB*/ false);
	}

	// The per-material constants as a lookup, four rows deep:
	//
	//   row 0  metres per repeat, rotation (radians), colour-map overlay, mask ramp exponent
	//   row 1  tint rgb, height-blend strength
	//   row 2  base height, displace range, coord scale xy
	//   row 3  uv offset xy, runtime-name grass flag, shuffled control flag
	//
	// A texture rather than a set of shader parameters because the material
	// must serve any map, and maps carry anywhere from a dozen to forty ground
	// layers.
	static bool IsGrassGroundMaterial(const BF6HP::FCore::FGroundMaterial& Mat)
	{
		// Regenerated from the current-install material resource names on every
		// build; no layer-number table or exported intermediate is consumed.
		const FString Name = Mat.Albedo.ToLower();
		// `seaweed` contains the generic vegetation token but is an underwater
		// debris layer on Tsuru, not the terrestrial grass surface requested here.
		if (Name.Contains(TEXT("seaweed"))) return false;
		return Name.Contains(TEXT("grass"))
			|| Name.Contains(TEXT("weed"))
			|| Name.Contains(TEXT("groundcover"))
			|| Name.Contains(TEXT("clover"))
			|| Name.Contains(TEXT("meadow"))
			|| Name.Contains(TEXT("lawn"))
			|| Name.Contains(TEXT("turf"));
	}

	UTexture2D* MakeParamTexture(const TArray<BF6HP::FCore::FGroundMaterial>& Mats)
	{
		const int32 N = Mats.Num();
		if (N <= 0) return nullptr;
		UTexture2D* Tex = UTexture2D::CreateTransient(N, 4, PF_A32B32G32R32F);
		if (!Tex) return nullptr;
		Tex->SRGB = false;
		Tex->CompressionSettings = TC_HDR;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		Tex->Filter = TF_Nearest;
		Tex->NeverStream = true;
		if (float* D = (float*)Tex->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))
		{
			for (int32 i = 0; i < N; i++)
			{
				D[i * 4 + 0] = FMath::Max(0.05f, Mats[i].MetresPerRepeat);
				D[i * 4 + 1] = FMath::DegreesToRadians(Mats[i].RotationDeg);
				// Authored as 0.5 or 1.0 on the fleet and applied unclamped by the
				// compositor (terrain.md D16); only a non-finite value is refused.
				D[i * 4 + 2] = FMath::IsFinite(Mats[i].Overlay) ? Mats[i].Overlay : 1.f;
				D[i * 4 + 3] = FMath::Max(0.01f, Mats[i].MaskRampExp);
			}
			float* R1 = D + N * 4;
			for (int32 i = 0; i < N; i++)
			{
				R1[i * 4 + 0] = Mats[i].Tint.R;
				R1[i * 4 + 1] = Mats[i].Tint.G;
				R1[i * 4 + 2] = Mats[i].Tint.B;
				R1[i * 4 + 3] = Mats[i].HeightBlend;
			}
			float* R2 = D + N * 8;
			for (int32 i = 0; i < N; i++)
			{
				R2[i * 4 + 0] = Mats[i].BaseHeight;
				R2[i * 4 + 1] = Mats[i].DisplaceRange;
				R2[i * 4 + 2] = Mats[i].CoordScale.X;
				R2[i * 4 + 3] = Mats[i].CoordScale.Y;
			}
			float* R3 = D + N * 12;
			for (int32 i = 0; i < N; i++)
			{
				R3[i * 4 + 0] = Mats[i].UvOffset.X;
				R3[i * 4 + 1] = Mats[i].UvOffset.Y;
				R3[i * 4 + 2] = IsGrassGroundMaterial(Mats[i]) ? 1.f : 0.f;
				// Same number of flags deliberately paired with the wrong material.
				// GroundGrassMode=2 selects this shuffled null control.
				const int32 Shuffled = (i + FMath::Max(1, N / 2)) % N;
				R3[i * 4 + 3] = IsGrassGroundMaterial(Mats[Shuffled]) ? 1.f : 0.f;
			}
			Tex->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		Tex->UpdateResource();
		Tex->AddToRoot();
		return Tex;
	}

	// Every ground sheet as one array, WITH A MIP CHAIN.
	//
	// UTexture2DArray::CreateTransient allocates a single mip, which is fine
	// for a full-screen blit and wrong for ground: a 512 sheet repeating every
	// three metres puts eight texels under a screen pixel at a hundred metres,
	// and with no mip to fall to that is a field of crawling static. The chain
	// is built here by box-halving, which is what the sampler would have used.
	UTexture2DArray* MakeSheetArray(const TArray<TArray<uint8>>& Sheets, int32 SheetSize,
	                                bool bSRGB, uint8 MissingRgb = 128)
	{
		const int32 N = Sheets.Num();
		if (N <= 0 || SheetSize <= 0) return nullptr;
		const int32 NumMips = FMath::Max(1, (int32)FMath::FloorLog2((uint32)SheetSize) + 1);

		UTexture2DArray* Tex = NewObject<UTexture2DArray>(
			GetTransientPackage(), NAME_None, RF_Transient);
		if (!Tex) return nullptr;
		Tex->SetPlatformData(new FTexturePlatformData());
		Tex->GetPlatformData()->SizeX = SheetSize;
		Tex->GetPlatformData()->SizeY = SheetSize;
		Tex->GetPlatformData()->SetNumSlices(N);
		Tex->GetPlatformData()->PixelFormat = PF_B8G8R8A8;
		Tex->bNotOfflineProcessed = true;
		Tex->SRGB = bSRGB;
		Tex->CompressionSettings = bSRGB ? TC_Default : TC_Masks;
		Tex->AddressX = TA_Wrap;
		Tex->AddressY = TA_Wrap;
		Tex->Filter = TF_Trilinear;
		Tex->NeverStream = true;

		// Working copy in BGRA, one buffer per slice, halved as the chain goes
		// down. A sheet that failed to decode is left as flat mid grey rather
		// than dropped: removing a slice would shift every index after it.
		TArray<TArray<uint8>> Cur;
		Cur.SetNum(N);
		for (int32 s = 0; s < N; s++)
		{
			Cur[s].SetNumZeroed(SheetSize * SheetSize * 4);
			if (Sheets[s].Num() >= SheetSize * SheetSize * 4)
			{
				const uint8* Src = Sheets[s].GetData();
				uint8* Dst = Cur[s].GetData();
				for (int32 i = 0; i < SheetSize * SheetSize; i++)
				{
					Dst[i * 4 + 0] = Src[i * 4 + 2];
					Dst[i * 4 + 1] = Src[i * 4 + 1];
					Dst[i * 4 + 2] = Src[i * 4 + 0];
					Dst[i * 4 + 3] = Src[i * 4 + 3];
				}
			}
			else
			{
				for (int32 i = 0; i < SheetSize * SheetSize; i++)
				{
					Cur[s][i * 4 + 0] = MissingRgb; Cur[s][i * 4 + 1] = MissingRgb;
					Cur[s][i * 4 + 2] = MissingRgb; Cur[s][i * 4 + 3] = 255;
				}
			}
		}

		int32 Dim = SheetSize;
		for (int32 m = 0; m < NumMips; m++)
		{
			FTexture2DMipMap* Mip = new FTexture2DMipMap(Dim, Dim, N);
			Tex->GetPlatformData()->Mips.Add(Mip);
			Mip->BulkData.Lock(LOCK_READ_WRITE);
			uint8* Out = (uint8*)Mip->BulkData.Realloc((int64)Dim * Dim * 4 * N);
			for (int32 s = 0; s < N; s++)
			{
				FMemory::Memcpy(Out + (int64)s * Dim * Dim * 4,
				                Cur[s].GetData(), (SIZE_T)Dim * Dim * 4);
			}
			Mip->BulkData.Unlock();

			if (m + 1 < NumMips)
			{
				const int32 Half = FMath::Max(1, Dim / 2);
				for (int32 s = 0; s < N; s++)
				{
					TArray<uint8> Next;
					Next.SetNumUninitialized(Half * Half * 4);
					const uint8* Src = Cur[s].GetData();
					for (int32 y = 0; y < Half; y++)
					{
						for (int32 x = 0; x < Half; x++)
						{
							const int32 x0 = FMath::Min(x * 2, Dim - 1);
							const int32 x1 = FMath::Min(x * 2 + 1, Dim - 1);
							const int32 y0 = FMath::Min(y * 2, Dim - 1);
							const int32 y1 = FMath::Min(y * 2 + 1, Dim - 1);
							for (int32 c = 0; c < 4; c++)
							{
								const int32 Sum =
									Src[(y0 * Dim + x0) * 4 + c] + Src[(y0 * Dim + x1) * 4 + c] +
									Src[(y1 * Dim + x0) * 4 + c] + Src[(y1 * Dim + x1) * 4 + c];
								Next[(y * Half + x) * 4 + c] = (uint8)(Sum / 4);
							}
						}
					}
					Cur[s] = MoveTemp(Next);
				}
				Dim = Half;
			}
		}

		Tex->UpdateResource();
		Tex->AddToRoot();
		return Tex;
	}

	// A one-texel array, so the PARENT material has an array to compile
	// against.
	//
	// A texture object parameter is typed by whatever texture it holds when
	// the material is translated, and the engine's stock default is a plain
	// Texture2D. Leaving it there would declare the Custom node's input as
	// Texture2D and then hand it to Texture2DArraySample, which does not
	// compile - and the instance's real array arrives far too late to change
	// that, because by then the parent is already built.
	UTexture2DArray* GDefaultSheetArray = nullptr;
	UTexture2DArray* DefaultSheetArray()
	{
		if (GDefaultSheetArray) return GDefaultSheetArray;
		UTexture2DArray* T = UTexture2DArray::CreateTransient(4, 4, 1, PF_B8G8R8A8);
		if (!T) return nullptr;
		T->SRGB = true;
		T->AddressX = TA_Wrap;
		T->AddressY = TA_Wrap;
		T->NeverStream = true;
		if (uint8* D = (uint8*)T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))
		{
			FMemory::Memset(D, 128, 4 * 4 * 4);
			T->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		T->UpdateResource();
		T->AddToRoot();
		GDefaultSheetArray = T;
		return T;
	}

	// Mid grey, which is the Overlay identity: blend(base, 0.5) == base for
	// every base. The right stand-in when a level ships no aerial map.
	UTexture2D* GMidGrey = nullptr;
	UTexture2D* MidGreyTexture()
	{
		if (GMidGrey) return GMidGrey;
		UTexture2D* T = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
		if (!T) return nullptr;
		T->SRGB = false;         // 0.5 LINEAR, not 0.5 sRGB
		T->CompressionSettings = TC_VectorDisplacementmap;
		T->NeverStream = true;
		if (uint8* D = (uint8*)T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))
		{
			FMemory::Memset(D, 128, 4 * 4 * 4);
			T->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		T->UpdateResource();
		T->AddToRoot();
		GMidGrey = T;
		return T;
	}

	UMaterial* EnsureGroundBlendMaterial()
	{
		if (GGroundBlendParent) return GGroundBlendParent;
		UPackage* Pkg = CreatePackage(TEXT("/Temp/BF6HighPoly_GroundBlend"));
		if (!Pkg) return nullptr;
		Pkg->SetFlags(RF_Transient);
		UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_BF6HighPoly_GroundBlend"), RF_Transient);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_DefaultLit);
		M->BlendMode = BLEND_Opaque;
		M->TwoSided = false;
		// The shipped terrain page stores its final normal in world space across
		// U2/U3. The exact branch therefore writes a world-space Normal value.
		M->bTangentSpaceNormal = false;

		auto Make = [&M](UClass* C, int32 X, int32 Y)
		{ return UMaterialEditingLibrary::CreateMaterialExpression(M, C, X, Y); };

		UMaterialExpressionWorldPosition* WP = Cast<UMaterialExpressionWorldPosition>(
			Make(UMaterialExpressionWorldPosition::StaticClass(), -1100, 0));
		UMaterialExpressionVertexNormalWS* GeoNormal = Cast<UMaterialExpressionVertexNormalWS>(
			Make(UMaterialExpressionVertexNormalWS::StaticClass(), -1100, 40));
		UMaterialExpressionCameraPositionWS* CamP = Cast<UMaterialExpressionCameraPositionWS>(
			Make(UMaterialExpressionCameraPositionWS::StaticClass(), -1100, 80));

		auto Vec = [&](const TCHAR* Nm, FLinearColor Def, int32 Y)
		{
			UMaterialExpressionVectorParameter* V = Cast<UMaterialExpressionVectorParameter>(
				Make(UMaterialExpressionVectorParameter::StaticClass(), -1100, Y));
			if (V) { V->ParameterName = Nm; V->DefaultValue = Def; }
			return V;
		};
		auto Scal = [&](const TCHAR* Nm, float Def, int32 Y)
		{
			UMaterialExpressionScalarParameter* S = Cast<UMaterialExpressionScalarParameter>(
				Make(UMaterialExpressionScalarParameter::StaticClass(), -1100, Y));
			if (S) { S->ParameterName = Nm; S->DefaultValue = Def; }
			return S;
		};
		// The sampler type is not decoration. SAMPLERTYPE_Color says "this
		// carries an sRGB image"; the coverage rasters and the parameter
		// lookup carry indices, weights and metres, and saying otherwise
		// invites a decode that would quietly change every number.
		auto TexObj = [&](const TCHAR* Nm, UTexture* Def, EMaterialSamplerType S, int32 Y)
		{
			UMaterialExpressionTextureObjectParameter* T =
				Cast<UMaterialExpressionTextureObjectParameter>(
					Make(UMaterialExpressionTextureObjectParameter::StaticClass(), -1100, Y));
			if (T)
			{
				T->ParameterName = Nm;
				T->SamplerType = S;
				if (Def) T->Texture = Def;
			}
			return T;
		};

		UMaterialExpressionVectorParameter* Lo = Vec(TEXT("GroundLo"), FLinearColor::Black, 160);
		// The far window and the playable box, both in world XZ metres.
		UMaterialExpressionVectorParameter* FarLoP =
			Vec(TEXT("FarLo"), FLinearColor::Black, 1400);
		UMaterialExpressionVectorParameter* FarSpanP =
			Vec(TEXT("FarSpan"), FLinearColor(1, 1, 1, 1), 1460);
		UMaterialExpressionVectorParameter* BoxCentreP =
			Vec(TEXT("BoxCentre"), FLinearColor::Black, 1520);
		UMaterialExpressionVectorParameter* BoxHalfP =
			Vec(TEXT("BoxHalf"), FLinearColor(1, 1, 1, 1), 1580);
		UMaterialExpressionVectorParameter* Span = Vec(TEXT("GroundSpan"), FLinearColor(1,1,1,1), 220);
		UMaterialExpressionScalarParameter* CovN = Scal(TEXT("CoverageSize"), 2048.f, 280);
		UMaterialExpressionScalarParameter* MatN = Scal(TEXT("MaterialCount"), 1.f, 340);
		UMaterialExpressionScalarParameter* NearP = Scal(TEXT("BlendNear"), 0.f, 400);
		UMaterialExpressionScalarParameter* FarP  = Scal(TEXT("BlendFar"), 1.f, 460);
		// A DEBUG CHANNEL, so the ground can be interrogated by looking at it.
		// Every mode below answers one question that is otherwise a guess.
		UMaterialExpressionScalarParameter* Dbg = Scal(TEXT("DebugMode"), 0.f, 520);
		// PHOTO BASE. How much of the ground's COLOUR comes from the level's own
		// aerial map rather than from the layer sheets. See the shader.
		UMaterialExpressionScalarParameter* Photo = Scal(TEXT("PhotoMix"), 0.f, 580);
		// RETIRED Portal-map input. Keep the neutral graph socket for material
		// compatibility, but runtime instances lock its blend to zero below. The
		// Portal editor JPG is a low-poly alignment aid, not game terrain data.
		UMaterialExpressionScalarParameter* ReferenceMix =
			Scal(TEXT("ReferenceMix"), 0.f, 595);
		// Break up obvious world-aligned repetition by blending transformed samples
		// from three neighbouring cells. This is a live approximation/control pair;
		// it is not claimed to be the exact Frostbite transform law until the
		// current-install shader path is decoded and independently verified.
		UMaterialExpressionScalarParameter* Stoch =
			Scal(TEXT("StochasticTiling"), 1.f, 610);
		// A heightfield's top projection collapses along cliffs. Blend the same
		// material in the three world planes there; the game has case-specific
		// side projections, while this is the engine-neutral visual fallback.
		UMaterialExpressionScalarParameter* SlopeProj =
			Scal(TEXT("SlopeProjection"), 1.f, 625);
		// Reconstruct the union of the four neighbouring sparse layer stacks
		// before evaluating coverage.  The old nearest-stack shortcut changes
		// the available material IDs at every half-texel and is the source of the
		// square MP_Isolated quilt.  Keep the shortcut as a measured A/B control.
		UMaterialExpressionScalarParameter* CoverageUnion =
			Scal(TEXT("CoverageUnion"), 1.f, 640);
		// Apply only the authored sheets' sub-metre/high-frequency residual to
		// the continuous CPU-composited ground. This is map-wide and has no
		// camera-centred rectangle; the generic layer colour remains a debug view
		// because its incomplete evaluator order produces the visible quilt.
		UMaterialExpressionScalarParameter* MapDetailStrength =
			Scal(TEXT("MapDetailStrength"), 0.75f, 655);
		// Restore the chromatic/albedo identity of grass-family sheets without
		// re-enabling the rejected all-layer colour compositor. 0=off control,
		// 1=current-install name pairing, 2=shuffled pairing control.
		UMaterialExpressionScalarParameter* GroundGrassMode =
			Scal(TEXT("GroundGrassMode"), 1.f, 670);
		UMaterialExpressionScalarParameter* GroundGrassStrength =
			Scal(TEXT("GroundGrassStrength"), 1.f, 685);
		// A small, opt-in comparison rectangle evaluated by the game's shipped
		// compute shader in the sidecar. Disabled is the control/current renderer.
		UMaterialExpressionVectorParameter* ExactLoP =
			Vec(TEXT("ExactPageLo"), FLinearColor::Black, 1640);
		UMaterialExpressionVectorParameter* ExactSpanP =
			Vec(TEXT("ExactPageSpan"), FLinearColor(1, 1, 1, 1), 1700);
		UMaterialExpressionScalarParameter* ExactEnabledP =
			Scal(TEXT("ExactPageEnabled"), 0.f, 1760);
		// U1/base-colour still depends on the unresolved runtime layer ordering.
		// A fixed-camera paired control on MP_Isolated showed that it is the
		// source of the pink/green quilt, while U2/U3 remain spatially coherent.
		// Keep U1 independently gated so the raw material/normal detail can be
		// evaluated without presenting the diagnostic colour as game output.
		UMaterialExpressionScalarParameter* ExactBaseColorEnabledP =
			Scal(TEXT("ExactBaseColorEnabled"), 0.f, 1775);
		UMaterialExpressionScalarParameter* BakedNormalEnabledP =
			Scal(TEXT("GroundNormalEnabled"), 0.f, 1790);
		// The Portal map image is an alignment oracle, not the terrain answer. It
		// is deliberately never loaded by this runtime tool.

		UTexture2D* White = LoadObject<UTexture2D>(nullptr,
			TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		UMaterialExpressionTextureObjectParameter* CovIdx =
			TexObj(TEXT("CovIdx"), White, SAMPLERTYPE_LinearColor, 520);
		UMaterialExpressionTextureObjectParameter* CovW =
			TexObj(TEXT("CovW"), White, SAMPLERTYPE_LinearColor, 580);
		UMaterialExpressionTextureObjectParameter* CovIdx2 =
			TexObj(TEXT("CovIdx2"), White, SAMPLERTYPE_LinearColor, 610);
		UMaterialExpressionTextureObjectParameter* CovW2 =
			TexObj(TEXT("CovW2"), White, SAMPLERTYPE_LinearColor, 625);
		UMaterialExpressionTextureObjectParameter* ParamT =
			TexObj(TEXT("CovParams"), White, SAMPLERTYPE_LinearColor, 640);
		UMaterialExpressionTextureObjectParameter* Baked =
			TexObj(TEXT("GroundAlbedo"), White, SAMPLERTYPE_Color, 700);
		// The CPU compositor already evaluates every decoded normal/height sheet
		// over the playable box.  The blend material previously discarded that
		// output and used only the geometric terrain normal, which made correctly
		// painted rock, sand and gravel read as one flat photographed surface.
		UMaterialExpressionTextureObjectParameter* BakedNormal =
			TexObj(TEXT("GroundNormal"), LinearWhite(), SAMPLERTYPE_LinearColor, 730);
		// The far bake: the whole footprint at low density, for the ground
		// beyond the playable box. sRGB like the near bake it stands in for.
		UMaterialExpressionTextureObjectParameter* FarBaked =
			TexObj(TEXT("FarBake"), MidGreyTexture(), SAMPLERTYPE_Color, 1340);
		UMaterialExpressionTextureObjectParameter* ColourM =
			TexObj(TEXT("GroundColour"), White, SAMPLERTYPE_Color, 820);
		UMaterialExpressionTextureObjectParameter* ReferenceM =
			TexObj(TEXT("GroundReference"), White, SAMPLERTYPE_Color, 850);
		UMaterialExpressionTextureObjectParameter* Sheets =
			TexObj(TEXT("Sheets"), DefaultSheetArray(), SAMPLERTYPE_Color, 760);
		// The normal/height sheets. Raw, not sRGB: the blue channel is a
		// HEIGHT and putting a display curve through it changes the blend.
		UMaterialExpressionTextureObjectParameter* Heights =
			TexObj(TEXT("Heights"), DefaultSheetArray(), SAMPLERTYPE_LinearColor, 880);
		// Optional authored opacity/coverage maps. A white slice is the identity
		// for layers whose evaluator uses the stored terrain mask directly.
		UMaterialExpressionTextureObjectParameter* Masks =
			TexObj(TEXT("Masks"), DefaultSheetArray(), SAMPLERTYPE_LinearColor, 940);
		// U1 is an R32G32B32A32_FLOAT UAV whose epilogue explicitly IEC-sRGB
		// encodes RGB. The transient texture is marked sRGB, so this Color sampler
		// decodes it exactly once before feeding Unreal's linear BaseColor input.
		UMaterialExpressionTextureObjectParameter* ExactPage =
			TexObj(TEXT("ExactPage"), White, SAMPLERTYPE_Color, 1820);
		UMaterialExpressionTextureObjectParameter* ExactMaterialPage =
			TexObj(TEXT("ExactMaterialPage"), White, SAMPLERTYPE_LinearColor, 1850);
		UMaterialExpressionTextureObjectParameter* ExactNormalPage =
			TexObj(TEXT("ExactNormalPage"), White, SAMPLERTYPE_LinearColor, 1880);

		UMaterialExpressionCustom* Blend = Cast<UMaterialExpressionCustom>(
			Make(UMaterialExpressionCustom::StaticClass(), -500, 300));
		if (!Blend || !WP || !GeoNormal || !CamP || !Lo || !Span || !CovN || !MatN || !NearP || !FarP
			|| !CovIdx || !CovW || !CovIdx2 || !CovW2 || !ParamT || !Baked || !BakedNormal || !Sheets || !ColourM || !Heights || !Masks
			|| !FarBaked || !FarLoP || !FarSpanP || !BoxCentreP || !BoxHalfP
			|| !ReferenceM || !Dbg || !Photo || !ReferenceMix || !Stoch || !SlopeProj
			|| !CoverageUnion || !MapDetailStrength || !GroundGrassMode || !GroundGrassStrength
			|| !ExactLoP || !ExactSpanP || !ExactEnabledP || !ExactBaseColorEnabledP || !BakedNormalEnabledP || !ExactPage
			|| !ExactMaterialPage
			|| !ExactNormalPage)
		{
			return nullptr;
		}
		if (!Sheets->Texture || !Sheets->Texture->IsA<UTexture2DArray>())
		{
			// Without an array here the Custom node's input translates as a
			// Texture2D and the whole material fails to compile, which shows up
			// as flat grey ground and a line in the log rather than anything
			// pointing at this.
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("ground blend: no default texture array, so the material would not compile"));
			return nullptr;
		}

		Blend->Description = TEXT("BF6 ground layer blend");
		Blend->OutputType = CMOT_Float3;
		Blend->Code = TEXT(R"HLSL(
// Four coverage taps, each carrying two RGBA groups (eight ordered masks).
// Weights blend bilinearly while indices stay point
// sampled. Sampling an index bilinearly gives a different index, so instead
// the four neighbours are read whole and each one's weight for a reference
// layer is accumulated by its bilinear share. Boundaries then fade over a
// coverage texel instead of stepping.
float2 pm  = WPos.xy * 0.01;
float2 cuv = saturate((pm - Lo.xy) / max(Span.xy, 1.0));
float2 tc  = cuv * CovSize - 0.5;
float2 fr  = frac(tc);
float  inv = 1.0 / CovSize;
float2 b0  = (floor(tc) + 0.5) * inv;

float4 IDS[4], WS[4], IDS2[4], WS2[4];
float2 CTO[4] = { b0, b0 + float2(inv, 0), b0 + float2(0, inv), b0 + float2(inv, inv) };
for (int c0 = 0; c0 < 4; c0++)
{
    IDS[c0]  = Texture2DSampleLevel(CovIdx,  CovIdxSampler,  CTO[c0], 0) * 255.0;
    WS[c0]   = Texture2DSampleLevel(CovW,    CovWSampler,    CTO[c0], 0);
    IDS2[c0] = Texture2DSampleLevel(CovIdx2, CovIdx2Sampler, CTO[c0], 0) * 255.0;
    WS2[c0]  = Texture2DSampleLevel(CovW2,   CovW2Sampler,   CTO[c0], 0);
}

float BW[4];
BW[0] = (1.0 - fr.x) * (1.0 - fr.y);
BW[1] = fr.x * (1.0 - fr.y);
BW[2] = (1.0 - fr.x) * fr.y;
BW[3] = fr.x * fr.y;

// A sparse indexed raster cannot be reconstructed by choosing one corner's
// ID list and merely interpolating weights.  At a boundary, another corner can
// carry a real layer absent from that list.  The old shortcut dropped it until
// the nearest-corner choice flipped at the half texel, making a discontinuous
// square grid even though every authored mask page is continuous.
//
// Build the union, accumulate each ID's bilinear mask, and retain its strongest
// eight. Eight is the coverage ABI's measured budget; selecting after the
// interpolation is continuous when the weakest candidate changes, unlike
// selecting an entire corner stack. Block-7 base entries remain first and
// paint is restored to ascending evaluator order.
float rid[8];
float acc[8];
int evalN = 0;
int baseN = 0;

if (CoverageUnion > 0.5)
{
	// Base candidates first. Enumerate the sparse source directly; a loop over
	// every possible material made Unreal's translator unroll 64 copies despite
	// only 32 source entries existing here.
	for (int c0 = 0; c0 < 4; c0++)
	{
		float candidate = IDS[c0].x;
		if (WS[c0].x < (254.5 / 255.0) || candidate >= 254.5) continue;
		bool already = false;
		for (int j = 0; j < evalN; j++) already = already || abs(rid[j] - candidate) < 0.5;
		if (already) continue;
		float mw = 0.0;
		for (int c = 0; c < 4; c++)
		{
			float ic[8] = { IDS[c].x, IDS[c].y, IDS[c].z, IDS[c].w,
			                IDS2[c].x, IDS2[c].y, IDS2[c].z, IDS2[c].w };
			float wc[8] = { WS[c].x, WS[c].y, WS[c].z, WS[c].w,
			                WS2[c].x, WS2[c].y, WS2[c].z, WS2[c].w };
			for (int k2 = 0; k2 < 8; k2++)
				if (wc[k2] > 0.0 && abs(ic[k2] - candidate) < 0.5)
					mw += BW[c] * wc[k2];
		}
		if (mw > 0.0 && evalN < 8)
		{
			rid[evalN] = candidate; acc[evalN] = mw; evalN++;
		}
	}
	baseN = evalN;

	// Paint candidates. When the four-corner union exceeds the eight-layer ABI,
	// replace only its weakest paint mask. A crossing therefore swaps two equal,
	// small contributions rather than jumping to a different corner's ID set.
	for (int c0 = 0; c0 < 4; c0++)
	{
		float srcId[8] = { IDS[c0].x, IDS[c0].y, IDS[c0].z, IDS[c0].w,
		                   IDS2[c0].x, IDS2[c0].y, IDS2[c0].z, IDS2[c0].w };
		float srcW[8] = { WS[c0].x, WS[c0].y, WS[c0].z, WS[c0].w,
		                  WS2[c0].x, WS2[c0].y, WS2[c0].z, WS2[c0].w };
		for (int k0 = 0; k0 < 8; k0++)
		{
			float candidate = srcId[k0];
			if (srcW[k0] <= 0.0 || candidate >= 254.5) continue;
			bool already = false;
			for (int j = 0; j < evalN; j++) already = already || abs(rid[j] - candidate) < 0.5;
			if (already) continue;

			float mw = 0.0;
			for (int c = 0; c < 4; c++)
			{
				float ic[8] = { IDS[c].x, IDS[c].y, IDS[c].z, IDS[c].w,
				                IDS2[c].x, IDS2[c].y, IDS2[c].z, IDS2[c].w };
				float wc[8] = { WS[c].x, WS[c].y, WS[c].z, WS[c].w,
				                WS2[c].x, WS2[c].y, WS2[c].z, WS2[c].w };
				for (int k2 = 0; k2 < 8; k2++)
					if (wc[k2] > 0.0 && abs(ic[k2] - candidate) < 0.5)
						mw += BW[c] * wc[k2];
			}
			if (mw <= 0.0) continue;
			if (evalN < 8)
			{
				rid[evalN] = candidate; acc[evalN] = mw; evalN++;
			}
			else if (baseN < 8)
			{
				int weak = baseN;
				for (int j = baseN + 1; j < 8; j++) if (acc[j] < acc[weak]) weak = j;
				if (mw > acc[weak]) { rid[weak] = candidate; acc[weak] = mw; }
			}
		}
	}
	// The strongest-N replacement is weight ordered; the evaluator is layer
	// ordered. Restore that order without moving the base prefix.
	for (int a = baseN; a < 8; a++)
	{
		if (a >= evalN) break;
		for (int b = a + 1; b < 8; b++)
		{
			if (b >= evalN) break;
			if (rid[b] < rid[a])
			{
				float ti = rid[a]; rid[a] = rid[b]; rid[b] = ti;
				float tw = acc[a]; acc[a] = acc[b]; acc[b] = tw;
			}
		}
	}
}
else
{
	// Control: the historical nearest-corner list. It is intentionally kept so
	// the discontinuity can be turned back on and compared in the same scene.
	int refc = (fr.x < 0.5 ? 0 : 1) + (fr.y < 0.5 ? 0 : 2);
	float oldId[8] = { IDS[refc].x, IDS[refc].y, IDS[refc].z, IDS[refc].w,
	                   IDS2[refc].x, IDS2[refc].y, IDS2[refc].z, IDS2[refc].w };
	float oldW[8] = { WS[refc].x, WS[refc].y, WS[refc].z, WS[refc].w,
	                  WS2[refc].x, WS2[refc].y, WS2[refc].z, WS2[refc].w };
	for (int k2 = 0; k2 < 8; k2++) if (oldW[k2] > 0.0 && oldId[k2] < 254.5)
	{
		rid[evalN] = oldId[k2]; acc[evalN] = 0.0; evalN++;
	}
	baseN = evalN > 0 && oldW[0] >= (254.5 / 255.0) ? 1 : 0;
}

// The corrected path accumulated masks while selecting its union. The control
// still needs the old reconstruction against its nearest-corner ID list.
if (CoverageUnion <= 0.5) for (int c = 0; c < 4; c++)
{
	float ic[8] = { IDS[c].x, IDS[c].y, IDS[c].z, IDS[c].w,
	                IDS2[c].x, IDS2[c].y, IDS2[c].z, IDS2[c].w };
	float wc[8] = { WS[c].x, WS[c].y, WS[c].z, WS[c].w,
	                WS2[c].x, WS2[c].y, WS2[c].z, WS2[c].w };
	for (int s = 0; s < evalN; s++) for (int k2 = 0; k2 < 8; k2++)
		if (wc[k2] > 0.0 && abs(ic[k2] - rid[s]) < 0.5)
			acc[s] += BW[c] * wc[k2];
}

// THE GAME'S OWN COVERAGE EVALUATOR, per pixel.
//
// What the raster carries is the raw MASK, not coverage. The difference is
// not cosmetic: measured on MP_Dumbo the dominant layer holds only a third
// of the mask, so normalising the four and blending gives every texel a
// four-way average and no material ever reads - which is precisely the
// "everything is mush" look. The kernel turns mask into coverage using each
// layer's HEIGHT, so a gravel with tall stones punches through a sand that
// sits low instead of cross-fading with it.
//
//   coverage = saturate(mask + (hiRef - loRef) * heightBlend)
//
// with both height references pulled toward the running composite by
// pow(maskRamp, rampExp), and mask >= 1 or <= 0 short-circuiting.
// Transcribed from the DXIL disassembly of the terrain evaluator kernels.
float3 col = float3(0.0, 0.0, 0.0);
// Neutral, high-frequency sheet detail accumulated separately from colour.
// This lets the aligned Portal tile own the map-scale palette without turning
// the ground into a soft photograph or allowing a wrong sheet's 3-13 m colour
// variation to survive as the visible quilt.
float3 detail = float3(1.0, 1.0, 1.0);
// Grass is the one material family whose sheet colour must survive the safe
// map-wide path. Accumulate it as premultiplied colour plus union coverage so
// partial paint masks do not darken the base twice. The flag comes from the
// current-install resource name in Params row 3, never a fixed layer index.
float3 grassPremul = float3(0.0, 0.0, 0.0);
float  grassAlpha = 0.0;
float  du  = 1.0 / max(MatCount, 1.0);
// One tap, shared by every layer: the aerial map is a property of the PLACE,
// not of the material sitting on it.
float3 aerial = Texture2DSample(Aerial, AerialSampler, cuv).rgb;

// WORLD derivatives, taken ONCE and out here where they are continuous.
//
// The sheet UV depends on which layer this pixel drew, through that layer's
// repeat length. Across a boundary between two materials with different repeat
// lengths the UV jumps, so an implicitly-differentiated sample sees an enormous
// derivative, drops to the coarsest mip, and paints a blurred stripe along
// every layer transition on the map. World position does not jump, so the
// honest derivative is this one pushed through the same transform the UV took.
float2 dpx = ddx(pm);
float2 dpy = ddy(pm);

float accH = 0.0, accLoMax = 0.0, accHiMax = 0.0;
bool  touched = false;
int   dbg = (int)(DebugMode + 0.5);

for (int s2 = 0; s2 < evalN; s2++)
{
	// Controlled test for the block-7 interpretation.  The core orders the
	// base entry first; mode 6 omits only that entry while preserving every
	// decoded paint mask and the normal height evaluator.  It deliberately
	// returns the near result below so the baked copy cannot reintroduce the
	// base pattern and invalidate the A/B.
	if (dbg == 6 && s2 < baseN) continue;

	float slice = clamp(rid[s2], 0.0, max(MatCount - 1.0, 0.0));
	float2 pu = float2((slice + 0.5) * du, 0.0);
	float4 P0 = Texture2DSampleLevel(Params, ParamsSampler, float2(pu.x, 0.125), 0);
	float4 P1 = Texture2DSampleLevel(Params, ParamsSampler, float2(pu.x, 0.375), 0);
	float4 P2 = Texture2DSampleLevel(Params, ParamsSampler, float2(pu.x, 0.625), 0);
	float4 P3 = Texture2DSampleLevel(Params, ParamsSampler, float2(pu.x, 0.875), 0);

	// UV: world metres over the layer's own repeat, its coordinate scale, its
	// offset, then its rotation. This is the thing that makes ground read as
	// ground rather than as a photograph of ground.
	float  rep = max(P0.r, 0.05);
	float2 uv  = (pm / rep) * P2.zw + P3.xy;
	float2 ddu = (dpx / rep) * P2.zw;
	float2 ddv = (dpy / rep) * P2.zw;
	float  rot = P0.g;
	if (abs(rot) > 0.001)
	{
		float cs = cos(rot), sn = sin(rot);
		uv  = float2(uv.x  * cs - uv.y  * sn, uv.x  * sn + uv.y  * cs);
		ddu = float2(ddu.x * cs - ddu.y * sn, ddu.x * sn + ddu.y * cs);
		ddv = float2(ddv.x * cs - ddv.y * sn, ddv.x * sn + ddv.y * cs);
	}

	float4 sh4 = Texture2DArraySampleGrad(Sheets, SheetsSampler,
	                                      float3(uv, slice), ddu, ddv);
	float4 shLow4 = Texture2DArraySampleLevel(Sheets, SheetsSampler,
	                                          float3(uv, slice), 6);
	float4 nh4 = Texture2DArraySampleGrad(Heights, HeightsSampler,
	                                      float3(uv, slice), ddu, ddv);
	float4 mk4 = Texture2DArraySampleGrad(Masks, MasksSampler,
	                                      float3(uv, slice), ddu, ddv);

	// SLOPE-PROJECTION CONTROL (debug mode 7).
	//
	// The normal path above is the current engine-neutral approximation: every
	// sheet is projected down the terrain heightfield's XY plane. On a near-
	// vertical cliff that collapses one surface direction and stretches one
	// 13-metre terrain repeat over tens of metres of rock face. The result is
	// exactly the rectangular, facade-like panels visible on MP_Isolated.
	//
	// Frostbite's generated evaluators contain case-specific side-projection
	// programs, but MP_Isolated's complete per-case programs have not been
	// ported. This generic blend is therefore an explicit visual fallback, not a
	// claim that every game layer is triplanar. Mode 7 isolates its near result;
	// mode 3 remains the ordinary full-material comparison.
	if (SlopeProjection > 0.5 || dbg == 7)
	{
		float3 posm = WPos * 0.01;
		float3 nx = ddx(posm);
		float3 ny = ddy(posm);
		float2 uvX = (posm.yz / rep) * P2.zw + P3.xy;
		float2 uvY = (posm.xz / rep) * P2.zw + P3.xy;
		float2 dxX = (nx.yz / rep) * P2.zw;
		float2 dyX = (ny.yz / rep) * P2.zw;
		float2 dxY = (nx.xz / rep) * P2.zw;
		float2 dyY = (ny.xz / rep) * P2.zw;
		if (abs(rot) > 0.001)
		{
			float cs = cos(rot), sn = sin(rot);
			uvX = float2(uvX.x * cs - uvX.y * sn, uvX.x * sn + uvX.y * cs);
			uvY = float2(uvY.x * cs - uvY.y * sn, uvY.x * sn + uvY.y * cs);
			dxX = float2(dxX.x * cs - dxX.y * sn, dxX.x * sn + dxX.y * cs);
			dyX = float2(dyX.x * cs - dyX.y * sn, dyX.x * sn + dyX.y * cs);
			dxY = float2(dxY.x * cs - dxY.y * sn, dxY.x * sn + dxY.y * cs);
			dyY = float2(dyY.x * cs - dyY.y * sn, dyY.x * sn + dyY.y * cs);
		}
		float3 triW = pow(abs(normalize(GeoNormal)), 8.0);
		triW /= max(triW.x + triW.y + triW.z, 1e-5);
		float4 shX = Texture2DArraySampleGrad(Sheets, SheetsSampler,
		                                      float3(uvX, slice), dxX, dyX);
		float4 shY = Texture2DArraySampleGrad(Sheets, SheetsSampler,
		                                      float3(uvY, slice), dxY, dyY);
		float4 shLowX = Texture2DArraySampleLevel(Sheets, SheetsSampler,
		                                           float3(uvX, slice), 6);
		float4 shLowY = Texture2DArraySampleLevel(Sheets, SheetsSampler,
		                                           float3(uvY, slice), 6);
		float4 nhX = Texture2DArraySampleGrad(Heights, HeightsSampler,
		                                      float3(uvX, slice), dxX, dyX);
		float4 nhY = Texture2DArraySampleGrad(Heights, HeightsSampler,
		                                      float3(uvY, slice), dxY, dyY);
		sh4 = shX * triW.x + shY * triW.y + sh4 * triW.z;
		shLow4 = shLowX * triW.x + shLowY * triW.y + shLow4 * triW.z;
		nh4 = nhX * triW.x + nhY * triW.y + nh4 * triW.z;
	}

	// STOCHASTIC TILE BREAKUP.  A direct periodic sample is the source of the
	// map-scale "quilt": a valid rock sheet repeats with identical features on
	// every authored tile. This triangular three-cell approximation preserves the
	// authored repeat length while removing the shared phase. Albedo at both
	// frequency bands, normal/height, and mask use identical transforms so the
	// high-pass detail and height blend cannot drift away from their colour.
	if (StochasticTiling > 0.5)
	{
		float2 cf = frac(uv);
		float2 cb = floor(uv);
		float2 v0, v1, v2;
		float3 sw;
		if (cf.x + cf.y <= 1.0)
		{
			v0 = cb;                       v1 = cb + float2(1.0, 0.0);
			v2 = cb + float2(0.0, 1.0);   sw = float3(1.0-cf.x-cf.y, cf.x, cf.y);
		}
		else
		{
			v0 = cb + float2(1.0, 1.0);   v1 = cb + float2(0.0, 1.0);
			v2 = cb + float2(1.0, 0.0);   sw = float3(cf.x+cf.y-1.0, 1.0-cf.x, 1.0-cf.y);
		}

		float3 h0 = frac(sin(float3(dot(v0,float2(127.11,311.80)),
		                            dot(v0,float2(269.52,183.38)),
		                            dot(v0,float2(419.27,371.93)))) * 43758.5);
		float3 h1 = frac(sin(float3(dot(v1,float2(127.11,311.80)),
		                            dot(v1,float2(269.52,183.38)),
		                            dot(v1,float2(419.27,371.93)))) * 43758.5);
		float3 h2 = frac(sin(float3(dot(v2,float2(127.11,311.80)),
		                            dot(v2,float2(269.52,183.38)),
		                            dot(v2,float2(419.27,371.93)))) * 43758.5);

		float a0=(h0.x-0.5)*6.2831855, a1=(h1.x-0.5)*6.2831855, a2=(h2.x-0.5)*6.2831855;
		float2 cs0=float2(cos(a0),sin(a0)), cs1=float2(cos(a1),sin(a1)), cs2=float2(cos(a2),sin(a2));
		float2 u0=float2(uv.x*cs0.x-uv.y*cs0.y,uv.x*cs0.y+uv.y*cs0.x)+h0.yz;
		float2 u1=float2(uv.x*cs1.x-uv.y*cs1.y,uv.x*cs1.y+uv.y*cs1.x)+h1.yz;
		float2 u2=float2(uv.x*cs2.x-uv.y*cs2.y,uv.x*cs2.y+uv.y*cs2.x)+h2.yz;
		float2 dx0=float2(ddu.x*cs0.x-ddu.y*cs0.y,ddu.x*cs0.y+ddu.y*cs0.x);
		float2 dy0=float2(ddv.x*cs0.x-ddv.y*cs0.y,ddv.x*cs0.y+ddv.y*cs0.x);
		float2 dx1=float2(ddu.x*cs1.x-ddu.y*cs1.y,ddu.x*cs1.y+ddu.y*cs1.x);
		float2 dy1=float2(ddv.x*cs1.x-ddv.y*cs1.y,ddv.x*cs1.y+ddv.y*cs1.x);
		float2 dx2=float2(ddu.x*cs2.x-ddu.y*cs2.y,ddu.x*cs2.y+ddu.y*cs2.x);
		float2 dy2=float2(ddv.x*cs2.x-ddv.y*cs2.y,ddv.x*cs2.y+ddv.y*cs2.x);
		sh4 = Texture2DArraySampleGrad(Sheets,SheetsSampler,float3(u0,slice),dx0,dy0)*sw.x
		    + Texture2DArraySampleGrad(Sheets,SheetsSampler,float3(u1,slice),dx1,dy1)*sw.y
		    + Texture2DArraySampleGrad(Sheets,SheetsSampler,float3(u2,slice),dx2,dy2)*sw.z;
		shLow4 = Texture2DArraySampleLevel(Sheets,SheetsSampler,float3(u0,slice),6)*sw.x
		       + Texture2DArraySampleLevel(Sheets,SheetsSampler,float3(u1,slice),6)*sw.y
		       + Texture2DArraySampleLevel(Sheets,SheetsSampler,float3(u2,slice),6)*sw.z;
		nh4 = Texture2DArraySampleGrad(Heights,HeightsSampler,float3(u0,slice),dx0,dy0)*sw.x
		    + Texture2DArraySampleGrad(Heights,HeightsSampler,float3(u1,slice),dx1,dy1)*sw.y
		    + Texture2DArraySampleGrad(Heights,HeightsSampler,float3(u2,slice),dx2,dy2)*sw.z;
		mk4 = Texture2DArraySampleGrad(Masks,MasksSampler,float3(u0,slice),dx0,dy0)*sw.x
		    + Texture2DArraySampleGrad(Masks,MasksSampler,float3(u1,slice),dx1,dy1)*sw.y
		    + Texture2DArraySampleGrad(Masks,MasksSampler,float3(u2,slice),dx2,dy2)*sw.z;
	}

	// The layer's own height, and the range it can move within.
	float ht      = saturate(nh4.b);
	float baseH   = P2.r;
	float disp    = P2.g;
	float hLayer  = baseH + (ht - 0.5) * disp;
	float hMax    = baseH + 0.5 * disp;
	float hMin    = baseH - 0.5 * disp;

	// Several generated cases (including MP_Isolated's large-rock family)
	// gate the authored paint page by a material `_op` sheet before the shared
	// height overlap. The old raw-mask path made those late layers opaque and
	// produced map-scale quilt patches. Missing sheets are white identity slices.
	float m       = saturate(acc[s2] * mk4.r);
	float rampExp = max(P0.a, 0.01);
	float hb      = P1.a;
	float wHi     = pow(saturate((m - 0.9) / -0.85), rampExp);
	float wLo     = pow(saturate((m - 0.05) / 0.95), rampExp);
	float hiRef   = hLayer + (accHiMax - hMax) * wHi;
	float loRef   = accH + (hMin - accLoMax) * wLo;
	float rawc    = m + (hiRef - loRef) * hb;
	float c = (m >= 1.0) ? 1.0 : ((m <= 0.0) ? 0.0 : saturate(rawc));

	float3 lc = sh4.rgb * P1.rgb;

	// The colour map Overlay, per layer and at its own authored strength.
	// This is what puts a map's real palette on stock ground materials: the
	// sheets are shot in a studio, and a map that skips this reads grey or
	// plainly the wrong colour for where it is.
	if (P0.b > 0.001)
	{
		// Overlay, branch-free and PER COMPONENT.
		//
		// The obvious spelling of this is a ternary on `lc < 0.5`, and it does
		// not compile: on a float3 that comparison is a bool3, and HLSL 2021 -
		// which is what SM6 goes through - refuses a non-scalar ternary
		// condition. It builds fine on the older path, so the failure lands on
		// one Nanite/Lumen permutation and the whole material silently falls
		// back to the default grey. step() sidesteps the whole question.
		float3 loB = 2.0 * lc * aerial;
		float3 hiB = 1.0 - 2.0 * (1.0 - lc) * (1.0 - aerial);
		float3 useLo = step(lc, 0.5);        // 1 where lc <= 0.5
		lc = lerp(lc, lerp(hiB, loB, useLo), P0.b);
	}

	// The first layer to reach a texel is promoted to full coverage for the
	// COLOUR lerp only, so a texel is never left showing the initial value;
	// the height chain below still uses the honest coverage.
	float cc = (!touched && c > 0.0) ? 1.0 : c;
	col = lerp(col, lc, cc);
	float grassFlag = GroundGrassMode < 0.5 ? 0.0
		: (GroundGrassMode < 1.5 ? P3.z : P3.w);
	float grassC = saturate(c * grassFlag);
	grassPremul = lerp(grassPremul, lc, grassC);
	grassAlpha = lerp(grassAlpha, 1.0, grassC);
	// Mip 6 of a 512 sheet carries the local low-frequency colour at 8x8.
	// Dividing it out keeps authored sub-metre grain while removing the broad
	// feature that visibly repeats once per material tile. The clamp prevents a
	// near-black texel from becoming an HDR spike.
	float3 hiDetail = clamp(sh4.rgb / max(shLow4.rgb, 0.04), 0.78, 1.22);
	detail = lerp(detail, hiDetail, cc);

	if (c > 0.0)
	{
		// The shipped kernel lerps the height chain by the SAME clamped
		// coverage it lerps colour by (terrain.md 2.4): every branch of the
		// coverage is in [0,1] before it reaches an accumulator. The unclamped
		// value extrapolated past the reference heights in deep stacks.
		accH     = clamp(lerp(loRef, hiRef, c), -1.0, 10.0);
		accLoMax = lerp(accLoMax, hMin, c);
		accHiMax = lerp(accHiMax, hMax, c);
		touched  = true;
	}
}

// Nothing resolved here: the aerial photograph of this spot is by far the
// closest answer available, and it is real shipped data rather than a guess.
// LINEARISED HERE, because this is the one place the map is used as a
// COLOUR rather than as the Overlay's operand. The texture is raw now, so
// anything consuming it as light has to de-gamma explicitly.
if (!touched) col = pow(max(aerial, 0.0), 2.2);

// COLOUR FROM THE SHIPPED MAP.
//
// Assembling ground colour out of layer sheets depends on picking the right
// sheet for every layer, and where that join is wrong the ground is wrong in a
// way no amount of blending fixes. The level's own aerial colour map does not
// have that problem: it is a photograph of this exact ground, correct by
// construction, and merely low frequency - now 0.62 m a texel over the
// playable box rather than the 2 m it was over the whole footprint.
//
// Do not multiply it by detail from the decoded sheet selection. That was the
// last route by which the bad MP_Isolated coverage mosaic survived even with
// PhotoMix at one: the wrong sheet became a large dark or light rectangle.
// The official Portal map-image decal can project over this surface as usual;
// this base is deliberately clean so the two sources do not fight each other.
{
    // The texture must remain raw for Overlay above. It is explicitly
    // linearised only here, where its bytes are being used as display colour.
    float3 photo = pow(max(aerial, 0.0), 2.2);
    col = lerp(col, photo, saturate(PhotoMix));
}

// RETIRED PORTAL MAP TILE SOCKET. Runtime instances force ReferenceMix to zero;
// the low-poly Portal editor JPG must never become rendered terrain content.
float3 reference = Texture2DSample(Reference, ReferenceSampler, cuv).rgb;
float3 referencedDetail = saturate(reference * detail);
col = lerp(col, referencedDetail, saturate(ReferenceMix));

// Keep the historical distance values as an A/B diagnostic, but do not let
// them decide where terrain detail exists. They used to create the misleading
// impression that one camera-centred square had been "finished".
// Terrain LOD follows ground-plane distance. Full 3D distance makes an editor
// camera 120 m above the island consume almost the entire near-detail budget
// before horizontal movement is considered, visually reducing the near field
// to one terrain tile.
float dist = length(WPos.xy - CamPos.xy) * 0.01;
float k = saturate((dist - NearM) / max(FarM - NearM, 1.0));
float3 baked = Texture2DSample(Bake, BakeSampler, cuv).rgb;

// OUTSIDE THE PLAYABLE BOX THERE IS NO COVERAGE AND NO NEAR BAKE.
//
// Both are rasterised over the box, so a texel beyond it samples outside
// 0..1 and clamps to whatever sits on the border - which on an 8 km map
// with a 2.5 km box is about 90% of the ground by area, smeared.
//
// The far bake covers the whole footprint, so past the box edge the colour
// comes from there instead. Crossed over by POSITION rather than by camera
// distance, because the boundary is a property of the ground, not of where
// anyone is standing. The 8% band inside the edge keeps the join from
// reading as a straight line.
// .xy ON EVERY VECTOR PARAMETER. They arrive as float3, and float2 minus
// float3 is a dimension mismatch that fails the whole material - the same
// way an out-of-bounds swizzle did. `pm` is the world XZ in metres, already
// computed above, so it is reused rather than recomputed.
float2 bx = abs((pm - BoxCentre.xy) / max(BoxHalf.xy, 1.0));
float outside = saturate((max(bx.x, bx.y) - 0.92) / 0.08);
if (outside > 0.0)
{
    float2 fuv = saturate((pm - FarLo.xy) / max(FarSpan.xy, 1.0));
    float3 farCol = Texture2DSample(FarBake, FarBakeSampler, fuv).rgb;
    col   = lerp(col, farCol, outside);
    baked = lerp(baked, farCol, outside);
}

// DEBUG CHANNELS. Each isolates one suspect, so a look answers a question
// instead of starting an argument:
//   1  flat colour per DOMINANT LAYER - if the pattern follows these blocks
//      the fault is the coverage or the join, and if it does not it is a
//      sheet's own content
//   2  the aerial colour map alone - is the smear the photograph?
//   3  the near-field blend alone, no far bake
//   4  the far bake alone, no per-pixel blend
//   5  the layer COUNT per texel, so mush is visible as brightness
//   6  paint-only control: omit the ordered block-7 base entry
//   7  slope-projection control: triplanar sheets, near result only
// 100 + N isolates layer N: it draws white where that material won the texel
// and near black everywhere else, so a wrong sheet can be pinned to a layer
// INDEX by looking rather than by reasoning about the join.
if (dbg >= 100)
{
    float want = (float)(dbg - 100);
    return abs(rid[0] - want) < 0.5 ? float3(1.0, 1.0, 1.0) : float3(0.02, 0.02, 0.02);
}
if (dbg == 1)
{
    float h = frac(rid[0] * 0.6180339887);
    return saturate(float3(abs(h * 6.0 - 3.0) - 1.0,
                           2.0 - abs(h * 6.0 - 2.0),
                           2.0 - abs(h * 6.0 - 4.0)));
}
if (dbg == 2) return aerial;
if (dbg == 3) return col;
if (dbg == 4) return baked;
if (dbg == 6) return col;
if (dbg == 7) return col;
if (dbg == 5)
{
    float used = 0.0;
    for (int d = 0; d < evalN; d++) if (acc[d] > 0.0) used += 1.0;
    return float3(used * 0.125, used * 0.125, used * 0.125);
}

// MAP-WIDE DETAIL PATH.
//
// The flattened compositor bake above is only 2048 px over MP_Isolated's
// 2555-m playable box (1.25 m/texel).  Using it as the production base made
// the entire terrain look pixelated even though the shipped colour/coverage
// field is 4096 px (0.62 m/texel).  Reconstruct the low-frequency base from
// that higher-resolution current-install field instead.  A 3x3 tent removes
// its residual texel grid; the authored high-frequency sheet ratio below puts
// grass/sand/rock grain back without resurrecting the bad large-area layer
// colours from the incomplete generic evaluator.
float3 mapRaw =
    Texture2DSample(Aerial, AerialSampler, cuv + inv * float2(-1,-1)).rgb * 1.0 +
    Texture2DSample(Aerial, AerialSampler, cuv + inv * float2( 0,-1)).rgb * 2.0 +
    Texture2DSample(Aerial, AerialSampler, cuv + inv * float2( 1,-1)).rgb * 1.0 +
    Texture2DSample(Aerial, AerialSampler, cuv + inv * float2(-1, 0)).rgb * 2.0 +
    Texture2DSample(Aerial, AerialSampler, cuv                         ).rgb * 4.0 +
    Texture2DSample(Aerial, AerialSampler, cuv + inv * float2( 1, 0)).rgb * 2.0 +
    Texture2DSample(Aerial, AerialSampler, cuv + inv * float2(-1, 1)).rgb * 1.0 +
    Texture2DSample(Aerial, AerialSampler, cuv + inv * float2( 0, 1)).rgb * 2.0 +
    Texture2DSample(Aerial, AerialSampler, cuv + inv * float2( 1, 1)).rgb * 1.0;
float3 mapBase = pow(max(mapRaw * (1.0 / 16.0), 0.0), 2.2);

// `detail` is only a high-pass residual from the installed game's authored
// sheets: mip 0 divided by mip 6, accumulated with decoded coverage. Keeping
// the ratio bounded prevents an incorrectly classified sheet from painting a
// brown/white quilt while retaining material-scale breakup everywhere inside
// the decoded playable box. The far footprint has no matching coverage and
// therefore keeps its own bake unchanged.
float3 detailRatio = clamp(detail, 0.82, 1.18);
float detailAmount = saturate(MapDetailStrength) * (1.0 - outside);
float3 distantCol = saturate(mapBase * lerp(float3(1.0, 1.0, 1.0), detailRatio, detailAmount));

// Reintroduce only the decoded grass-family colour at its evaluated mask.
// Sand, rock, dirt and coral remain on the continuous map field, so the
// previously measured brown/white all-layer quilt stays outside production.
float3 grassCol = grassPremul / max(grassAlpha, 1.0e-4);
float grassBlend = saturate(grassAlpha * GroundGrassStrength) * (1.0 - outside);
distantCol = lerp(distantCol, saturate(grassCol), grassBlend);

// The generic `col` evaluator remains available through DebugMode, but it is
// not a production near field: expanding it was the measured source of the
// brown/white quilt. The production path stays on the stable map field until
// the shipped-DXIL material/normal pages are consumed by the tiled clipmap.
float3 playableCol = distantCol;
float3 finalCol = lerp(playableCol, baked, outside);

// EXACT SHIPPED-DXIL PAGE, DIAGNOSTIC ONLY. This is intentionally the final
// base-colour operation: running it through the approximate near/far material
// again would no longer be a comparison against the game's evaluator. The
// one-texel feather prevents a false seam from dominating the visual test.
float2 exactUV = (pm - ExactPageLo.xy) / max(ExactPageSpan.xy, 0.001);
float exactEdge = min(min(exactUV.x, exactUV.y),
                      min(1.0 - exactUV.x, 1.0 - exactUV.y));
float4 exactSample = Texture2DSampleLevel(
    ExactPage, ExactPageSampler, saturate(exactUV), 0);
// The evaluator's explicit unhandled-work sentinel is opaque magenta.  It is
// evidence that this texel is not closed, not a colour the game displays.
// Leave the existing renderer visible there rather than turning a diagnostic
// sentinel into pink terrain.
// Test a small neighbourhood around the sentinel because bilinear filtering
// blends a one-pixel hole into pink before this branch sees it.
float exactUnhandled = step(0.70, exactSample.r)
                     * step(exactSample.g, 0.30)
                     * step(0.70, exactSample.b);
float exactMask = saturate(exactEdge * 64.0)
                * saturate(ExactPageEnabled)
				* saturate(ExactBaseColorEnabled)
                * (1.0 - exactUnhandled);
float3 exactCol = exactSample.rgb;
return lerp(finalCol, exactCol, exactMask);
)HLSL");

		Blend->Inputs.Empty();
		auto In = [&Blend](const TCHAR* Nm, UMaterialExpression* E)
		{ FCustomInput I; I.InputName = Nm; I.Input.Expression = E; Blend->Inputs.Add(I); };
		In(TEXT("WPos"), WP);
		In(TEXT("GeoNormal"), GeoNormal);
		In(TEXT("CamPos"), CamP);
		In(TEXT("Lo"), Lo);
		In(TEXT("Span"), Span);
		In(TEXT("CovSize"), CovN);
		In(TEXT("MatCount"), MatN);
		In(TEXT("NearM"), NearP);
		In(TEXT("FarM"), FarP);
		In(TEXT("CovIdx"), CovIdx);
		In(TEXT("CovW"), CovW);
		In(TEXT("CovIdx2"), CovIdx2);
		In(TEXT("CovW2"), CovW2);
		In(TEXT("Params"), ParamT);
		In(TEXT("Sheets"), Sheets);
		In(TEXT("Bake"), Baked);
		In(TEXT("FarBake"), FarBaked);
		In(TEXT("FarLo"), FarLoP);
		In(TEXT("FarSpan"), FarSpanP);
		In(TEXT("BoxCentre"), BoxCentreP);
		In(TEXT("BoxHalf"), BoxHalfP);
		In(TEXT("Aerial"), ColourM);
		In(TEXT("Reference"), ReferenceM);
		In(TEXT("DebugMode"), Dbg);
		In(TEXT("PhotoMix"), Photo);
		In(TEXT("ReferenceMix"), ReferenceMix);
		In(TEXT("StochasticTiling"), Stoch);
		In(TEXT("SlopeProjection"), SlopeProj);
		In(TEXT("CoverageUnion"), CoverageUnion);
		In(TEXT("MapDetailStrength"), MapDetailStrength);
		In(TEXT("GroundGrassMode"), GroundGrassMode);
		In(TEXT("GroundGrassStrength"), GroundGrassStrength);
		In(TEXT("Heights"), Heights);
		In(TEXT("Masks"), Masks);
		In(TEXT("ExactPageLo"), ExactLoP);
		In(TEXT("ExactPageSpan"), ExactSpanP);
		In(TEXT("ExactPageEnabled"), ExactEnabledP);
		In(TEXT("ExactBaseColorEnabled"), ExactBaseColorEnabledP);
		In(TEXT("ExactPage"), ExactPage);

		UMaterialEditingLibrary::ConnectMaterialProperty(Blend, TEXT(""), MP_BaseColor);

		// The whole playable surface gets the decoded layer-composite normal from
		// the runtime bake.  Where a shipped-DXIL page is active, U2/U3 replace it
		// with the game's final world normal.  The geometric normal remains the
		// explicit fallback/control for missing or invalid decoded data.
		UMaterialExpressionCustom* ExactNormal = Cast<UMaterialExpressionCustom>(
			Make(UMaterialExpressionCustom::StaticClass(), -360, 1260));
		if (ExactNormal)
		{
			ExactNormal->Description = TEXT("BF6 decoded terrain normal, exact U2/U3 override");
			ExactNormal->OutputType = CMOT_Float3;
			ExactNormal->Code = TEXT(R"HLSL(
float3 geometricWorld = normalize(GeoNormal);

// The CPU compositor stores the layer normal in tangent space as 0.5*n+0.5.
// Rebuild a stable world tangent frame from the actual terrain triangle.
float2 groundUV = (WPos.xy * 0.01 - GroundLo.xy) / max(GroundSpan.xy, 1.0);
float insideGround = step(0.0, groundUV.x) * step(groundUV.x, 1.0)
                   * step(0.0, groundUV.y) * step(groundUV.y, 1.0)
                   * saturate(GroundNormalEnabled);
float3 tangentNormal = Texture2DSampleLevel(
    GroundNormal, GroundNormalSampler, saturate(groundUV), 0).rgb * 2.0 - 1.0;
float3 tangentX = float3(1.0, 0.0, 0.0)
                - geometricWorld * geometricWorld.x;
float tangentLength2 = dot(tangentX, tangentX);
tangentX = tangentLength2 > 1.0e-6
    ? tangentX * rsqrt(tangentLength2)
    : float3(0.0, 1.0, 0.0);
float3 tangentY = normalize(cross(geometricWorld, tangentX));
float3 bakedWorld = normalize(tangentX * tangentNormal.x
                            + tangentY * tangentNormal.y
                            + geometricWorld * max(tangentNormal.z, 0.05));
float bakedValid = step(0.05, dot(bakedWorld, geometricWorld));
float3 fallbackWorld = normalize(lerp(
    geometricWorld, bakedWorld, insideGround * bakedValid));

float2 uv = (WPos.xy * 0.01 - ExactPageLo.xy) / max(ExactPageSpan.xy, 0.001);
float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
float mask = saturate(edge * 64.0) * saturate(ExactPageEnabled);
float3 basePage = Texture2DSampleLevel(
    ExactPage, ExactPageSampler, saturate(uv), 0).rgb;
float unhandled = step(0.70, basePage.r)
                * step(basePage.g, 0.30)
                * step(0.70, basePage.b);
mask *= 1.0 - unhandled;
float2 horizontal = Texture2DSampleLevel(
    ExactNormalPage, ExactNormalPageSampler, saturate(uv), 0).rg * 2.0 - 1.0;
float vertical = Texture2DSampleLevel(
    ExactMaterialPage, ExactMaterialPageSampler, saturate(uv), 0).g * 2.0 - 1.0;
float3 packedWorld = float3(horizontal.x, horizontal.y, vertical);
float packedLength2 = dot(packedWorld, packedWorld);
float3 unrealWorld = packedLength2 > 1.0e-6
    ? packedWorld * rsqrt(packedLength2)
    : fallbackWorld;
// A decoded terrain normal must point into the same hemisphere as the actual
// heightfield triangle.  Rejecting the opposite hemisphere is a transport
// control: it prevents an invalid/default UAV texel from turning a whole
// triangle black without inventing a replacement normal.
float validNormal = step(0.05, dot(unrealWorld, geometricWorld));
return normalize(lerp(fallbackWorld, unrealWorld, mask * validNormal));
)HLSL");
			auto NormalIn = [&ExactNormal](const TCHAR* Nm, UMaterialExpression* E)
			{ FCustomInput I; I.InputName = Nm; I.Input.Expression = E; ExactNormal->Inputs.Add(I); };
			NormalIn(TEXT("WPos"), WP);
			NormalIn(TEXT("GroundLo"), Lo);
			NormalIn(TEXT("GroundSpan"), Span);
			NormalIn(TEXT("GroundNormalEnabled"), BakedNormalEnabledP);
			NormalIn(TEXT("GroundNormal"), BakedNormal);
			NormalIn(TEXT("ExactPageLo"), ExactLoP);
			NormalIn(TEXT("ExactPageSpan"), ExactSpanP);
			NormalIn(TEXT("ExactPageEnabled"), ExactEnabledP);
			NormalIn(TEXT("ExactPage"), ExactPage);
			NormalIn(TEXT("ExactMaterialPage"), ExactMaterialPage);
			NormalIn(TEXT("ExactNormalPage"), ExactNormalPage);
			NormalIn(TEXT("GeoNormal"), GeoNormal);
			UMaterialEditingLibrary::ConnectMaterialProperty(ExactNormal, TEXT(""), MP_Normal);
		}

		UMaterialExpressionScalarParameter* Rg = Scal(TEXT("GroundRoughness"), 0.9f, 900);
		UMaterialExpressionCustom* ExactRoughness = Cast<UMaterialExpressionCustom>(
			Make(UMaterialExpressionCustom::StaticClass(), -100, 1500));
		if (Rg && ExactRoughness)
		{
			ExactRoughness->Description = TEXT("BF6 U2 smoothness -> Unreal roughness");
			ExactRoughness->OutputType = CMOT_Float1;
			ExactRoughness->Code = TEXT(R"HLSL(
float2 uv = (WPos.xy * 0.01 - ExactPageLo.xy) / max(ExactPageSpan.xy, 0.001);
float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
float mask = saturate(edge * 64.0) * saturate(ExactPageEnabled);
float3 basePage = Texture2DSampleLevel(
    ExactPage, ExactPageSampler, saturate(uv), 0).rgb;
float unhandled = step(0.70, basePage.r)
                * step(basePage.g, 0.30)
                * step(0.70, basePage.b);
mask *= 1.0 - unhandled;
float smoothness = Texture2DSampleLevel(
    ExactMaterialPage, ExactMaterialPageSampler, saturate(uv), 0).a;
return lerp(saturate(GroundRoughness), saturate(1.0 - smoothness), mask);
)HLSL");
			auto RoughnessIn = [&ExactRoughness](const TCHAR* Nm, UMaterialExpression* E)
			{ FCustomInput I; I.InputName = Nm; I.Input.Expression = E; ExactRoughness->Inputs.Add(I); };
			RoughnessIn(TEXT("WPos"), WP);
			RoughnessIn(TEXT("ExactPageLo"), ExactLoP);
			RoughnessIn(TEXT("ExactPageSpan"), ExactSpanP);
			RoughnessIn(TEXT("ExactPageEnabled"), ExactEnabledP);
			RoughnessIn(TEXT("ExactPage"), ExactPage);
			RoughnessIn(TEXT("ExactMaterialPage"), ExactMaterialPage);
			RoughnessIn(TEXT("GroundRoughness"), Rg);
			UMaterialEditingLibrary::ConnectMaterialProperty(
				ExactRoughness, TEXT(""), MP_Roughness);
		}
		else if (Rg)
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Rg, TEXT(""), MP_Roughness);
		}

		M->PreEditChange(nullptr);
		M->PostEditChange();
		M->AddToRoot();   // see EnsureGroundMaterial: a raw global must be rooted
		GGroundBlendParent = M;
		return M;
	}


	// ---- DISK-CACHED CORE READS ------------------------------------------
	//
	// Each wrapper asks the cache first and the core second, and writes what
	// the core answered so the next open skips the work. The cache is a memo
	// of the core's own output - the game stays the source - keyed on the
	// install signature so a patch invalidates it by itself. The VERSION on
	// each blob is the layout of what is serialised below: bump it whenever a
	// field is added or its meaning changes, or an old blob loads cleanly and
	// lies (see the staleness ladder).
	constexpr uint32 kCacheVerBake = 1, kCacheVerCoverage = 1;
	constexpr uint32 kCacheVerSheet = 1, kCacheVerTerrain = 1;

	struct FBakeHold { TArray<uint8> Albedo, Normal; };

	bool CachedBakeGround(const FString& Level, const FVector2D& RectMin, float RectSize,
	                      int32 Size, BF6HP::FCore::FGroundBake& B, FBakeHold& Hold,
	                      bool& bFromCache)
	{
		const FString Name = FString::Printf(TEXT("ground_bake_%d_%.1f_%.1f_%.1f"),
			Size, RectMin.X, RectMin.Y, RectSize);
		bFromCache = false;
		TArray<uint8> Blob;
		if (BF6HP::DiskCache::Load(Level, Name, kCacheVerBake, Blob))
		{
			FMemoryReader R(Blob);
			int32 HasNormal = 0;
			R << B.Size << B.Lo << B.Hi << B.MetresPerTexel << B.LayersUsed
			  << B.LayersTextured << B.FallbackFraction << HasNormal;
			const int64 N = (int64)B.Size * B.Size * 4;
			if (B.Size > 0 && !R.IsError() &&
				(int64)Blob.Num() >= R.Tell() + N * (HasNormal ? 2 : 1))
			{
				Hold.Albedo.SetNumUninitialized(N);
				R.Serialize(Hold.Albedo.GetData(), N);
				B.Albedo = Hold.Albedo.GetData();
				B.Normal = nullptr;
				if (HasNormal)
				{
					Hold.Normal.SetNumUninitialized(N);
					R.Serialize(Hold.Normal.GetData(), N);
					B.Normal = Hold.Normal.GetData();
				}
				bFromCache = true;
				return true;
			}
		}
		if (!GCore.BakeGround(Level, RectMin, RectSize, Size, B)) return false;
		if (B.Size > 0 && B.Albedo)
		{
			TArray<uint8> Out;
			FMemoryWriter W(Out);
			int32 HasNormal = B.Normal ? 1 : 0;
			W << B.Size << B.Lo << B.Hi << B.MetresPerTexel << B.LayersUsed
			  << B.LayersTextured << B.FallbackFraction << HasNormal;
			const int64 N = (int64)B.Size * B.Size * 4;
			W.Serialize((void*)B.Albedo, N);
			if (B.Normal) W.Serialize((void*)B.Normal, N);
			BF6HP::DiskCache::Save(Level, Name, kCacheVerBake, Out);
		}
		return true;
	}

	struct FCoverageHold { TArray<uint8> Idx, Weight, Colour; };

	void SerialiseGroundMaterial(FArchive& Ar, BF6HP::FCore::FGroundMaterial& M)
	{
		Ar << M.Layer << M.Albedo << M.Normal << M.Coverage << M.MetresPerRepeat
		   << M.RotationDeg << M.Tint << M.Overlay << M.BaseHeight << M.DisplaceRange
		   << M.MaskRampExp << M.HeightBlend << M.CoordScale << M.UvOffset;
	}

	bool CachedGroundCoverage(const FString& Level, int32 Size,
	                          BF6HP::FCore::FGroundCoverage& C, FCoverageHold& Hold,
	                          bool& bFromCache)
	{
		const FString Name = FString::Printf(TEXT("ground_coverage_%d"), Size);
		bFromCache = false;
		TArray<uint8> Blob;
		if (BF6HP::DiskCache::Load(Level, Name, kCacheVerCoverage, Blob))
		{
			FMemoryReader R(Blob);
			int32 HasColour = 0, MatCount = 0;
			R << C.Size << C.SlotCount << C.Lo << C.Hi << C.EmptyFraction << HasColour << MatCount;
			if (C.Size > 0 && C.SlotCount > 0 && MatCount >= 0 && MatCount < 4096 && !R.IsError())
			{
				C.Materials.SetNum(MatCount);
				for (BF6HP::FCore::FGroundMaterial& M : C.Materials) SerialiseGroundMaterial(R, M);
				const int64 N = (int64)C.Size * C.Size * C.SlotCount;
				const int64 NC = HasColour ? (int64)C.Size * C.Size * 3 : 0;
				if (!R.IsError() && (int64)Blob.Num() >= R.Tell() + 2 * N + NC)
				{
					Hold.Idx.SetNumUninitialized(N);    R.Serialize(Hold.Idx.GetData(), N);
					Hold.Weight.SetNumUninitialized(N); R.Serialize(Hold.Weight.GetData(), N);
					C.Idx = Hold.Idx.GetData();
					C.Weight = Hold.Weight.GetData();
					C.Colour = nullptr;
					if (HasColour)
					{
						Hold.Colour.SetNumUninitialized(NC);
						R.Serialize(Hold.Colour.GetData(), NC);
						C.Colour = Hold.Colour.GetData();
					}
					bFromCache = true;
					return true;
				}
			}
			C = BF6HP::FCore::FGroundCoverage();
		}
		if (!GCore.GroundCoverage(Level, Size, C)) return false;
		if (C.Size > 0 && C.Idx && C.Weight)
		{
			TArray<uint8> Out;
			FMemoryWriter W(Out);
			int32 HasColour = C.Colour ? 1 : 0, MatCount = C.Materials.Num();
			W << C.Size << C.SlotCount << C.Lo << C.Hi << C.EmptyFraction << HasColour << MatCount;
			for (BF6HP::FCore::FGroundMaterial& M : C.Materials) SerialiseGroundMaterial(W, M);
			const int64 N = (int64)C.Size * C.Size * C.SlotCount;
			W.Serialize((void*)C.Idx, N);
			W.Serialize((void*)C.Weight, N);
			if (C.Colour) W.Serialize((void*)C.Colour, (int64)C.Size * C.Size * 3);
			BF6HP::DiskCache::Save(Level, Name, kCacheVerCoverage, Out);
		}
		return true;
	}

	// Sheets belong to a material resource, not a map, so they are shared.
	bool CachedLayerSheet(const FString& ResName, int32 Size, TArray<uint8>& Out)
	{
		const FString Name = FString::Printf(TEXT("sheet_%d_%s"), Size,
			*FMD5::HashAnsiString(*ResName.ToLower()).Left(20));
		if (BF6HP::DiskCache::Load(TEXT("shared"), Name, kCacheVerSheet, Out) && Out.Num() > 0)
			return true;
		Out.Reset();
		if (!GCore.LayerSheet(ResName, Size, Out)) return false;
		if (Out.Num() > 0) BF6HP::DiskCache::Save(TEXT("shared"), Name, kCacheVerSheet, Out);
		return true;
	}

	bool CachedReadTerrain(const FString& Level, BF6HP::FCore::FTerrain& T, bool& bFromCache)
	{
		bFromCache = false;
		TArray<uint8> Blob;
		if (BF6HP::DiskCache::Load(Level, TEXT("terrain"), kCacheVerTerrain, Blob))
		{
			FMemoryReader R(Blob);
			int32 Count = 0;
			R << T.Size << T.HeightScale << T.WorldMin << T.WorldMax << Count;
			// THE DIMENSIONS AND THE SAMPLE COUNT HAVE TO AGREE.
			//
			// This checked that Size and Count were positive and that the bytes
			// were present, but never that Count actually describes a Size by
			// Size grid. Everything downstream indexes it as one, so a record
			// where they disagree - a truncated write, a half flushed cache
			// after a crash, an older format - passes here and reads off the
			// end of the array later, a long way from the cause.
			//
			// The upper bound is a sanity limit rather than a real one: a
			// Size of tens of thousands is a corrupt header, and multiplying it
			// out before rejecting it is how the check itself overflows.
			const bool bSaneSize = T.Size > 1 && T.Size <= 32768;
			const bool bCountMatches = bSaneSize &&
				(int64)Count >= (int64)T.Size * (int64)T.Size;
			if (bSaneSize && bCountMatches && Count > 0 && !R.IsError() &&
				(int64)Blob.Num() >= R.Tell() + (int64)Count * 2)
			{
				T.Heights.SetNumUninitialized(Count);
				R.Serialize(T.Heights.GetData(), (int64)Count * 2);
				bFromCache = true;
				return true;
			}
			T = BF6HP::FCore::FTerrain();
		}
		if (!GCore.ReadTerrain(Level, T)) return false;
		if (T.Size > 0 && T.Heights.Num() > 0)
		{
			TArray<uint8> Out;
			FMemoryWriter W(Out);
			int32 Count = T.Heights.Num();
			W << T.Size << T.HeightScale << T.WorldMin << T.WorldMax << Count;
			W.Serialize(T.Heights.GetData(), (int64)Count * 2);
			BF6HP::DiskCache::Save(Level, TEXT("terrain"), kCacheVerTerrain, Out);
		}
		return true;
	}

	// ---- DISK-CACHED MESH SECTIONS ----------------------------------------
	//
	// ReadMesh is a CAS read, an Oodle decompress and a vertex-format decode,
	// serial through the one core context at about a millisecond a mesh - and
	// the receiver pass used to pay it for the whole map a second time. The
	// sections are a pure function of (install, resource, placing bundle,
	// variation), so they are memoised per level, one blob a group, loaded a
	// batch at a time across cores. Texture BINDINGS travel as resource NAMES
	// and are resolved back to this session's ids on the game thread, because
	// an id is the order the core first met a name in and means nothing next
	// session. A decode failure is never memoised.
	// 2: the unscoped material recovery in FCore::ReadMesh. Version 1 blobs were
	// written from scoped reads that bound nothing, so the meshes they hold are
	// untextured and no recovery can fire behind a cache hit. They have to be
	// read again once.
	constexpr uint32 kCacheVerMesh = 4;

	FString MeshCacheName(const FString& ResName, const FString& Bundle, const FString& Variation)
	{
		return TEXT("mesh_") + FMD5::HashAnsiString(
			*(ResName + TEXT("|") + Bundle + TEXT("|") + Variation)).Left(24);
	}

	// Symmetric layout. Writing never mutates the sections; the non-const
	// reference is what loading needs.
	void SerialiseMeshSections(FArchive& Ar, TArray<BF6HP::FCore::FSection>& Sections,
	                           TArray<TArray<FString>>& BindingNames)
	{
		int32 Count = Sections.Num();
		Ar << Count;
		if (Ar.IsLoading())
		{
			if (Ar.IsError() || Count <= 0 || Count > 65536) { Ar.SetError(); return; }
			Sections.SetNum(Count);
			BindingNames.SetNum(Count);
		}
		for (int32 si = 0; si < Count && !Ar.IsError(); si++)
		{
			BF6HP::FCore::FSection& S = Sections[si];
			TArray<FString>& Names = BindingNames[si];
			int32 NPos = S.Pos.Num(), NNrm = S.Nrm.Num(), NUV = S.UV.Num();
			int32 NIdx = S.Idx.Num(), NTex = S.Textures.Num();
			uint8 Flags = (S.bAlphaTest ? 1 : 0) | (S.bTranslucent ? 2 : 0)
			            | (S.bAlphaFromAlbedo ? 4 : 0) | (S.bNsm ? 8 : 0)
			            | (S.bDecal ? 16 : 0) | (S.bTerrainDecalReceiver ? 32 : 0);
			Ar << NPos << NNrm << NUV << NIdx << NTex << Flags << S.BaseColor << S.Roughness;
			if (Ar.IsLoading())
			{
				const int64 Need = (int64)NPos * 12 + (int64)NNrm * 12 + (int64)NUV * 8 + (int64)NIdx * 4;
				if (Ar.IsError() || NPos < 0 || NNrm < 0 || NUV < 0 || NIdx < 0 ||
				    NTex < 0 || NTex > 64 || Need > Ar.TotalSize() - Ar.Tell())
				{
					Ar.SetError();
					return;
				}
				S.bAlphaTest = (Flags & 1) != 0;
				S.bTranslucent = (Flags & 2) != 0;
				S.bAlphaFromAlbedo = (Flags & 4) != 0;
				S.bNsm = (Flags & 8) != 0;
				S.bDecal = (Flags & 16) != 0;
				S.bTerrainDecalReceiver = (Flags & 32) != 0;
				S.Pos.SetNumUninitialized(NPos);
				S.Nrm.SetNumUninitialized(NNrm);
				S.UV.SetNumUninitialized(NUV);
				S.Idx.SetNumUninitialized(NIdx);
				S.Textures.SetNum(NTex);
				Names.SetNum(NTex);
			}
			Ar.Serialize(S.Pos.GetData(), (int64)NPos * sizeof(FVector3f));
			Ar.Serialize(S.Nrm.GetData(), (int64)NNrm * sizeof(FVector3f));
			Ar.Serialize(S.UV.GetData(), (int64)NUV * sizeof(FVector2f));
			Ar.Serialize(S.Idx.GetData(), (int64)NIdx * sizeof(uint32));
			for (int32 b = 0; b < NTex && !Ar.IsError(); b++)
				Ar << S.Textures[b].Slot << Names[b];
		}
	}

	// Game thread: the core's name for every bound texture. False when the
	// core cannot name one (an older dll) - then nothing is written, because a
	// blob with an unnamed binding could only be resolved by an id that lies.
	bool NameMeshBindings(const TArray<BF6HP::FCore::FSection>& Sections,
	                      TArray<TArray<FString>>& OutNames)
	{
		OutNames.SetNum(Sections.Num());
		for (int32 si = 0; si < Sections.Num(); si++)
		{
			OutNames[si].SetNum(Sections[si].Textures.Num());
			for (int32 b = 0; b < Sections[si].Textures.Num(); b++)
			{
				const BF6HP::FCore::FBinding& B = Sections[si].Textures[b];
				if (B.Texture < 0) { OutNames[si][b].Reset(); continue; }
				// Already known on anything that came through the cache; only a
				// live decode has to pay bf6_texture_name_at for it.
				if (!B.Name.IsEmpty()) { OutNames[si][b] = B.Name; continue; }
				FString N = GCore.TextureNameAt(B.Texture);
				if (N.IsEmpty()) return false;
				OutNames[si][b] = MoveTemp(N);
			}
		}
		return true;
	}

	// Pure, so it runs on a worker beside DescribeMesh.
	bool SerialiseMeshForCache(const TArray<BF6HP::FCore::FSection>& Sections,
	                           const TArray<TArray<FString>>& Names, TArray<uint8>& Out)
	{
		Out.Reset();
		if (Sections.Num() == 0 || Names.Num() != Sections.Num()) return false;
		FMemoryWriter W(Out);
		SerialiseMeshSections(W, const_cast<TArray<BF6HP::FCore::FSection>&>(Sections),
		                      const_cast<TArray<TArray<FString>>&>(Names));
		return !W.IsError() && Out.Num() > 0;
	}

	struct FMeshBlob
	{
		TArray<BF6HP::FCore::FSection> Sections;
		TArray<TArray<FString>>        BindingNames;
	};

	// Pure: file read, unpack and deserialise, so it runs on a worker. The ids
	// are resolved afterwards by ResolveMeshBlobBindings on the game thread.
	bool LoadMeshBlob(const FString& Level, const FString& ResName, const FString& Bundle,
	                  const FString& Variation, FMeshBlob& Out)
	{
		TArray<uint8> Blob;
		if (!BF6HP::DiskCache::LoadPacked(Level, MeshCacheName(ResName, Bundle, Variation),
		                                  kCacheVerMesh, Blob))
			return false;
		FMemoryReader R(Blob);
		SerialiseMeshSections(R, Out.Sections, Out.BindingNames);
		if (R.IsError() || Out.Sections.Num() == 0) { Out = FMeshBlob(); return false; }
		return true;
	}

	// Game thread: names back to this session's ids through the core's own
	// registry, which is the same map the live decode registers into. False
	// when a name is not in the mount, and the caller decodes live instead.
	bool ResolveMeshBlobBindings(FMeshBlob& B)
	{
		if (B.BindingNames.Num() != B.Sections.Num()) return false;
		for (int32 si = 0; si < B.Sections.Num(); si++)
		{
			if (B.BindingNames[si].Num() != B.Sections[si].Textures.Num()) return false;
			for (int32 b = 0; b < B.Sections[si].Textures.Num(); b++)
			{
				const FString& Name = B.BindingNames[si][b];
				int32 Id = -1;
				if (!Name.IsEmpty())
				{
					Id = GCore.TextureIdByName(Name);
					if (Id < 0) return false;
				}
				B.Sections[si].Textures[b].Texture = Id;
				// Kept so nothing has to ask the reader for it back; see FBinding.
				B.Sections[si].Textures[b].Name = Name;
			}
		}
		return true;
	}

	// Cache first, core second, written behind the build. bNoWait turns the
	// reader lock into a try-lock and answers Busy rather than standing on the
	// game thread for the length of somebody else's mount; see the note on
	// EReadResult in BF6HighPolyShared.h for what that cost when measured.
	BF6HP::Shared::EReadResult CachedReadMeshImpl(const FString& Level, const FString& ResName,
	                                              const FString& Bundle, const FString& Variation,
	                                              TArray<BF6HP::FCore::FSection>& Out,
	                                              bool& bFromCache, bool bNoWait)
	{
		using EReadResult = BF6HP::Shared::EReadResult;
		bFromCache = false;
		FMeshBlob B;
		if (LoadMeshBlob(Level, ResName, Bundle, Variation, B) && ResolveMeshBlobBindings(B))
		{
			Out = MoveTemp(B.Sections);
			bFromCache = true;
			return EReadResult::Ok;
		}
		bool bRead = false;
		{
			// Only a MISS enters the reader, so a cached read never waits behind
			// a mount or a sound decode.
			FCriticalSection& CS = BF6HP::Shared::CoreMutex();
			if (bNoWait)
			{
				if (!CS.TryLock()) { return EReadResult::Busy; }
			}
			else
			{
				CS.Lock();
			}
			bRead = GCore.ReadMesh(ResName, Out, Bundle, Variation);
			CS.Unlock();
		}
		if (!bRead) { return EReadResult::Failed; }
		TArray<TArray<FString>> Names;
		TArray<uint8> Blob;
		if (NameMeshBindings(Out, Names) && SerialiseMeshForCache(Out, Names, Blob))
			BF6HP::DiskCache::SaveAsync(Level, MeshCacheName(ResName, Bundle, Variation),
				kCacheVerMesh, MoveTemp(Blob), /*bPack*/ true);
		return EReadResult::Ok;
	}

	// Serial convenience for the passes that read a few meshes (receivers,
	// scatter). These run inside the build, which owns the reader for its
	// duration anyway, so they still wait.
	bool CachedReadMesh(const FString& Level, const FString& ResName, const FString& Bundle,
	                    const FString& Variation, TArray<BF6HP::FCore::FSection>& Out,
	                    bool& bFromCache)
	{
		return CachedReadMeshImpl(Level, ResName, Bundle, Variation, Out, bFromCache,
			/*bNoWait*/ false) == BF6HP::Shared::EReadResult::Ok;
	}

	// The per-pixel ground, or null to fall back on the flattened bake.
	UMaterialInstanceDynamic* MakeGroundBlendMaterial(UObject* Outer, const FString& Level,
	                                                  double ExtentM)
	{
		BF6HP::FCore::FGroundCoverage C;
		FCoverageHold CoverageHold;
		bool bCoverageCached = false;
		const double T0 = FPlatformTime::Seconds();
		if (!CachedGroundCoverage(Level, SizeForExtent(ExtentM, GGroundCoverageSizeMax), C,
		                          CoverageHold, bCoverageCached))
		{
			UE_LOG(LogBF6HighPoly, Warning, TEXT("ground coverage: %s"), *GCore.Error);
			return nullptr;
		}

		// Score the real runtime-name pairing against an equal-population shuffled
		// pairing before any other core call can replace the context-owned raster.
		int32 GrassMaterialCount = 0;
		TArray<FString> GrassResources;
		for (const BF6HP::FCore::FGroundMaterial& Mat : C.Materials)
		{
			if (!IsGrassGroundMaterial(Mat)) continue;
			GrassMaterialCount++;
			GrassResources.Add(FPaths::GetCleanFilename(Mat.Albedo));
		}
		double TotalMask = 0.0, GrassMask = 0.0, ShuffledMask = 0.0;
		const int32 SlotCount = FMath::Max(1, C.SlotCount);
		const int32 MatCount = C.Materials.Num();
		// A deterministic 256x256 lattice is enough to score the null control.
		// Walking all 134 million 4096x4096x8 entries added a measured minute to
		// startup while changing no runtime texture, so do not repeat that mistake.
		const int32 SampleStep = FMath::Max(1, C.Size / 256);
		for (int32 Y = 0; Y < C.Size; Y += SampleStep)
		for (int32 X = 0; X < C.Size; X += SampleStep)
		for (int32 Slot = 0; Slot < SlotCount; Slot++)
		{
			const int64 Sample = ((int64)Y * C.Size + X) * SlotCount + Slot;
			const int32 Id = C.Idx ? (int32)C.Idx[Sample] : 255;
			const double W = C.Weight ? (double)C.Weight[Sample] : 0.0;
			if (Id < 0 || Id >= MatCount || W <= 0.0) continue;
			TotalMask += W;
			if (IsGrassGroundMaterial(C.Materials[Id])) GrassMask += W;
			const int32 Shuffled = (Id + FMath::Max(1, MatCount / 2)) % MatCount;
			if (IsGrassGroundMaterial(C.Materials[Shuffled])) ShuffledMask += W;
		}
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("ground grass read path: %d/%d runtime material name(s) [%s]; ")
			TEXT("real mask %.3f%%, shuffled-pair control %.3f%%"),
			GrassMaterialCount, MatCount, *FString::Join(GrassResources, TEXT(", ")),
			TotalMask > 0.0 ? 100.0 * GrassMask / TotalMask : 0.0,
			TotalMask > 0.0 ? 100.0 * ShuffledMask / TotalMask : 0.0);

		// Decode every bound sheet to one size so they can share an array.
		// The height sheets go into a second array of the same shape: the
		// evaluator needs a height per layer at the same point, and without it
		// the blend has nothing to sharpen with.
		TArray<TArray<uint8>> Sheets, Heights, Masks;
		Sheets.SetNum(C.Materials.Num());
		Heights.SetNum(C.Materials.Num());
		Masks.SetNum(C.Materials.Num());
		int32 Decoded = 0, HeightsDecoded = 0, MasksDecoded = 0;
		for (int32 i = 0; i < C.Materials.Num(); i++)
		{
			if (!C.Materials[i].Albedo.IsEmpty())
			{
				if (CachedLayerSheet(C.Materials[i].Albedo, GSheetSize, Sheets[i])) Decoded++;
				else Sheets[i].Reset();
			}
			if (!C.Materials[i].Normal.IsEmpty())
			{
				if (CachedLayerSheet(C.Materials[i].Normal, GSheetSize, Heights[i])) HeightsDecoded++;
				else Heights[i].Reset();
			}
			if (!C.Materials[i].Coverage.IsEmpty())
			{
				if (CachedLayerSheet(C.Materials[i].Coverage, GSheetSize, Masks[i])) MasksDecoded++;
				else Masks[i].Reset();
			}
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("ground coverage: %d px over %.0f m (%.2f m/texel), %d material(s), ")
			TEXT("%d albedo + %d height + %d coverage sheet(s) decoded, %.1f%% empty, %.1fs%s"),
			C.Size, C.Hi.X - C.Lo.X, (float)(C.Hi.X - C.Lo.X) / (float)FMath::Max(1, C.Size),
			C.Materials.Num(), Decoded, HeightsDecoded, MasksDecoded, C.EmptyFraction * 100.f,
			FPlatformTime::Seconds() - T0, bCoverageCached ? TEXT(" (raster from cache)") : TEXT(""));
		if (Decoded == 0)
		{
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("no ground sheet decoded, so the per-pixel blend would draw flat; ")
				TEXT("falling back to the bake"));
			return nullptr;
		}

		GCovIdxTex = MakeRawTexture(C.Idx, C.Size, C.SlotCount, 0, 255, /*point*/ true);
		GCovWTex = MakeRawTexture(C.Weight, C.Size, C.SlotCount, 0, 0, /*point*/ true);
		GCovIdxTex2 = MakeRawTexture(C.Idx, C.Size, C.SlotCount, 4, 255, /*point*/ true);
		GCovWTex2 = MakeRawTexture(C.Weight, C.Size, C.SlotCount, 4, 0, /*point*/ true);
		GCovParamTex = MakeParamTexture(C.Materials);
		GCovColourTex = C.Colour ? MakeColourTexture(C.Colour, C.Size) : nullptr;
		GSheetArray = MakeSheetArray(Sheets, GSheetSize, /*sRGB*/ true);
		GHeightArray = MakeSheetArray(Heights, GSheetSize, /*sRGB*/ false);
		GMaskArray = MakeSheetArray(Masks, GSheetSize, /*sRGB*/ false, /*missing*/ 255);
		if (!GCovIdxTex || !GCovWTex || !GCovIdxTex2 || !GCovWTex2 || !GCovParamTex || !GSheetArray || !GHeightArray || !GMaskArray)
		{
			UE_LOG(LogBF6HighPoly, Warning, TEXT("ground blend: a texture would not build"));
			return nullptr;
		}

		UMaterial* Parent = EnsureGroundBlendMaterial();
		if (!Parent) return nullptr;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return nullptr;
		MID->SetFlags(RF_Transient);
		MID->SetTextureParameterValue(TEXT("CovIdx"), GCovIdxTex);
		MID->SetTextureParameterValue(TEXT("CovW"), GCovWTex);
		MID->SetTextureParameterValue(TEXT("CovIdx2"), GCovIdxTex2);
		MID->SetTextureParameterValue(TEXT("CovW2"), GCovWTex2);
		MID->SetTextureParameterValue(TEXT("CovParams"), GCovParamTex);
		if (GCovColourTex)
		{
			MID->SetTextureParameterValue(TEXT("GroundColour"), GCovColourTex);
		}
		else
		{
			// White is the Overlay identity only where the layer colour is
			// already 1, so a missing map has to switch the blend OFF rather
			// than feed it a neutral - which the strength in the parameter
			// lookup cannot do from here. Mid grey IS the identity: Overlay
			// with 0.5 returns the base unchanged.
			MID->SetTextureParameterValue(TEXT("GroundColour"), MidGreyTexture());
		}
		// The Portal editor MapImage is not game terrain and must not be a runtime
		// input. The neutral texture plus a locked zero blend also keeps old parent
		// materials safe until their transient graph is rebuilt.
		MID->SetTextureParameterValue(TEXT("GroundReference"), MidGreyTexture());
		MID->SetScalarParameterValue(TEXT("ReferenceMix"), 0.f);
		MID->SetTextureParameterValue(TEXT("Sheets"), GSheetArray);
		MID->SetTextureParameterValue(TEXT("Heights"), GHeightArray);
		MID->SetTextureParameterValue(TEXT("Masks"), GMaskArray);
		if (GGroundAlbedo) MID->SetTextureParameterValue(TEXT("GroundAlbedo"), GGroundAlbedo);
		if (GGroundNormal)
		{
			MID->SetTextureParameterValue(TEXT("GroundNormal"), GGroundNormal);
			MID->SetScalarParameterValue(TEXT("GroundNormalEnabled"), 1.f);
		}
		else
		{
			MID->SetScalarParameterValue(TEXT("GroundNormalEnabled"), 0.f);
		}
		// THE FAR FIELD. Bound only when it baked: with no far texture the
		// crossover still runs, so the fallback must be the near bake rather
		// than mid grey, and binding GroundAlbedo here makes the lerp a no-op.
		MID->SetTextureParameterValue(TEXT("FarBake"),
			GGroundFar ? GGroundFar : GGroundAlbedo);
		MID->SetVectorParameterValue(TEXT("FarLo"),
			FLinearColor((float)GFarLo.X, (float)GFarLo.Y, 0, 0));
		MID->SetVectorParameterValue(TEXT("FarSpan"),
			FLinearColor((float)GFarSpan.X, (float)GFarSpan.Y, 1, 1));
		// With no far bake the box is reported as enormous, which drives
		// `outside` to zero everywhere and leaves the old behaviour intact.
		const bool bHaveFar = GGroundFar != nullptr;
		MID->SetVectorParameterValue(TEXT("BoxCentre"),
			FLinearColor((float)GBoxCentre.X, (float)GBoxCentre.Y, 0, 0));
		MID->SetVectorParameterValue(TEXT("BoxHalf"),
			bHaveFar ? FLinearColor((float)GBoxHalf.X, (float)GBoxHalf.Y, 1, 1)
			         : FLinearColor(1e9f, 1e9f, 1, 1));
		MID->SetVectorParameterValue(TEXT("GroundLo"),
			FLinearColor((float)C.Lo.X, (float)C.Lo.Y, 0, 0));
		MID->SetVectorParameterValue(TEXT("GroundSpan"),
			FLinearColor(FMath::Max(1.f, (float)(C.Hi.X - C.Lo.X)),
			             FMath::Max(1.f, (float)(C.Hi.Y - C.Lo.Y)), 1, 1));
		MID->SetScalarParameterValue(TEXT("CoverageSize"), (float)C.Size);
		MID->SetScalarParameterValue(TEXT("MaterialCount"), (float)C.Materials.Num());
		MID->SetScalarParameterValue(TEXT("BlendNear"), GBlendNear);
		MID->SetScalarParameterValue(TEXT("BlendFar"), GBlendFar);
		// Keep the live terrain-layer evaluation visible. PhotoMix=1 replaces the
		// decoded grass/rock/sand sheets with the low-frequency aerial colour map;
		// on MP_Isolated that is a 0.62 m/texel raster and is exactly the blocky,
		// textureless ground failure. The aerial map still participates in each
		// layer through its authored Overlay amount inside the evaluator above.
		MID->SetScalarParameterValue(TEXT("PhotoMix"), 0.f);
		MID->SetScalarParameterValue(TEXT("CoverageUnion"), 1.f);
		MID->SetScalarParameterValue(TEXT("MapDetailStrength"), 0.75f);
		MID->SetScalarParameterValue(TEXT("GroundGrassMode"), 1.f);
		MID->SetScalarParameterValue(TEXT("GroundGrassStrength"), 1.f);
		MID->SetTextureParameterValue(TEXT("ExactPage"), LinearWhite());
		MID->SetTextureParameterValue(TEXT("ExactMaterialPage"), LinearWhite());
		MID->SetTextureParameterValue(TEXT("ExactNormalPage"), LinearWhite());
		MID->SetScalarParameterValue(TEXT("ExactPageEnabled"), 0.f);
		MID->SetScalarParameterValue(TEXT("ExactBaseColorEnabled"), 0.f);
		return MID;
	}

	// Bake the ground and hand back a material instance for the terrain tiles.
	// Null when there is nothing to bake, which leaves the terrain as it was.
	UMaterialInstanceDynamic* MakeGroundMaterial(UObject* Outer, const FString& Level,
	                                             double ExtentM,
	                                             const FVector2D& FarLo, float FarSize)
	{
		BF6HP::FCore::FGroundBake B;
		FBakeHold BakeHold;
		bool bBakeCached = false;
		const double T0 = FPlatformTime::Seconds();
		GGroundBakeSize = SizeForExtent(ExtentM, GGroundBakeSizeMax);
		if (!CachedBakeGround(Level, FVector2D::ZeroVector, 0.f, GGroundBakeSize, B,
		                      BakeHold, bBakeCached))
		{
			UE_LOG(LogBF6HighPoly, Warning, TEXT("ground bake: %s"), *GCore.Error);
			return nullptr;
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("ground bake: %d px over %.0f m (%.2f m/texel), %d layer(s), %d textured, ")
			TEXT("%.1f%% untextured, %.1fs%s"),
			B.Size, B.Hi.X - B.Lo.X, B.MetresPerTexel, B.LayersUsed, B.LayersTextured,
			B.FallbackFraction * 100.f, FPlatformTime::Seconds() - T0,
			bBakeCached ? TEXT(" (from cache)") : TEXT(""));

		GGroundAlbedo = MakeBakeTexture(B.Albedo, B.Size, /*sRGB*/ true);
		GGroundNormal = MakeBakeTexture(B.Normal, B.Size, /*sRGB*/ false);
		// Remember the near window: it is the box, and the shader crosses over
		// at its edge.
		GBoxCentre = FVector2D((B.Lo.X + B.Hi.X) * 0.5, (B.Lo.Y + B.Hi.Y) * 0.5);
		GBoxHalf   = FVector2D(FMath::Max(1.0, (B.Hi.X - B.Lo.X) * 0.5),
		                       FMath::Max(1.0, (B.Hi.Y - B.Lo.Y) * 0.5));

		// THE FAR BAKE, over the whole footprint. An explicit rect overrides
		// the playable-box default inside the core, which is the only reason
		// this needs no change down there.
		if (FarSize > 1.f)
		{
			BF6HP::FCore::FGroundBake F;
			FBakeHold FarHold;
			bool bFarCached = false;
			const double TF = FPlatformTime::Seconds();
			if (CachedBakeGround(Level, FarLo, FarSize, 1024, F, FarHold, bFarCached))
			{
				GGroundFar = MakeBakeTexture(F.Albedo, F.Size, /*sRGB*/ true);
				GFarLo   = FVector2D(F.Lo.X, F.Lo.Y);
				GFarSpan = FVector2D(FMath::Max(1.0, F.Hi.X - F.Lo.X),
				                     FMath::Max(1.0, F.Hi.Y - F.Lo.Y));
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("ground far: %d px over %.0f m (%.2f m/texel), %.1fs - the ")
					TEXT("terrain outside the %.0f m playable box was sampling a ")
					TEXT("clamped border texel"),
					F.Size, F.Hi.X - F.Lo.X, F.MetresPerTexel,
					FPlatformTime::Seconds() - TF, GBoxHalf.X * 2.0);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Display,
					TEXT("ground far: %s - terrain beyond the playable box will ")
					TEXT("keep smearing its border texel"), *GCore.Error);
			}
		}
		// THE PER-PIXEL PATH FIRST. It needs the bake anyway - that is its far
		// field - so this runs after the bake and falls back to it whole if the
		// coverage or the sheets do not come out.
		if (UMaterialInstanceDynamic* Blend = MakeGroundBlendMaterial(Outer, Level, ExtentM))
		{
			return Blend;
		}

		UMaterial* Parent = EnsureGroundMaterial();
		if (!Parent || !GGroundAlbedo) return nullptr;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return nullptr;
		MID->SetFlags(RF_Transient);
		MID->SetTextureParameterValue(TEXT("GroundAlbedo"), GGroundAlbedo);
		if (GGroundNormal) MID->SetTextureParameterValue(TEXT("GroundNormal"), GGroundNormal);
		MID->SetVectorParameterValue(TEXT("GroundLo"),
			FLinearColor((float)B.Lo.X, (float)B.Lo.Y, 0, 0));
		MID->SetVectorParameterValue(TEXT("GroundSpan"),
			FLinearColor(FMath::Max(1.f, (float)(B.Hi.X - B.Lo.X)),
			             FMath::Max(1.f, (float)(B.Hi.Y - B.Lo.Y)), 1, 1));
		return MID;
	}

	// Held across a build so every tile shares one instance.
	UMaterialInstanceDynamic* GroundMat = nullptr;

	int32 BuildTerrain(AActor* A, USceneComponent* Root, const BF6HP::FCore::FTerrain& T,
	                   TArray<UStaticMesh*>& OutPending)
	{
		if (T.Size <= 1) return 0;

		// Bake the ground once for the whole map, before any tile is made.
		// The map's own footprint decides how dense the ground needs to be.
		// The FULL footprint goes in as the far window, because that is what
		// the mesh below actually covers.
		GroundMat = MakeGroundMaterial(A, BF6Ext::CurrentLevel(),
			FMath::Max(T.WorldMax.X - T.WorldMin.X, T.WorldMax.Z - T.WorldMin.Z),
			FVector2D(T.WorldMin.X, T.WorldMin.Z),
			(float)FMath::Max(T.WorldMax.X - T.WorldMin.X,
			                  T.WorldMax.Z - T.WorldMin.Z));

		const int32 N = FMath::Min(GTerrainSide, T.Size);
		const int32 Step = FMath::Max(1, (T.Size - 1) / (N - 1));

		// y = u16 / 65536 * HeightScale, straight from the tree's own header.
		// This used to fit a line from the AABB's y range onto the raw range,
		// which gives the same answer to within a centimetre while the AABB is
		// tight to the data - but it is the data being tidy, not a rule, and the
		// rule costs nothing to follow.
		const double YScale = T.HeightScale > 0.f
			? (double)T.HeightScale / 65536.0
			: FMath::Max(0.001, T.WorldMax.Y - T.WorldMin.Y) / 65535.0;
		const double XLo = T.WorldMin.X, XSpan = T.WorldMax.X - T.WorldMin.X;
		const double ZLo = T.WorldMin.Z, ZSpan = T.WorldMax.Z - T.WorldMin.Z;
		const double NativeSpacingM = FMath::Max(XSpan, ZSpan) / (T.Size - 1);

		const int32 Per   = FMath::Clamp(GTerrainTile, 16, N - 1);
		const int32 Tiles = FMath::DivideAndRoundUp(N - 1, Per);
		const int32 BaseCells = N - 1;
		const bool bAdaptive = Step > 1
			&& (Step & (Step - 1)) == 0
			&& BaseCells * Step == T.Size - 1;
		int32 MaxLevel = 0;
		for (int32 s = Step; s > 1; s >>= 1) MaxLevel++;

		// Frostbite keeps the height tree as a concentric LOD pyramid and lets the
		// GPU tessellate/displace the fine patches. A transient Unreal static mesh
		// cannot use that renderer, so make the equivalent decision once here:
		// retain a coarse cell only when ALL of its native samples stay close to
		// the bilinear plane. Thresholds are in metres and tighten with spacing.
		// They were measured against MP_Isolated's 16,385-square native field:
		// 4 m uniform cells had p99 1.03 m and max 56.82 m error. The selected
		// 0.50 / 0.25 / 0.10 m ladder produced 15.52 M triangles before stitching,
		// versus 8.39 M uniform; shuffling height blocks raised median error from
		// 0.006 m to 171.985 m, the negative control expected for real locality.
		TArray<uint8> CellLevels;
		CellLevels.SetNumZeroed(BaseCells * BaseCells);
		const double TerrainAnalyseStart = FPlatformTime::Seconds();
		if (bAdaptive)
		{
			ParallelFor(BaseCells, [&](int32 bz)
			{
				auto MaxPlaneErrorM = [&](int32 sx0, int32 sz0, int32 Span)
				{
					const double h00 = T.Heights[ sz0         * T.Size + sx0];
					const double h10 = T.Heights[ sz0         * T.Size + sx0 + Span];
					const double h01 = T.Heights[(sz0 + Span) * T.Size + sx0];
					const double h11 = T.Heights[(sz0 + Span) * T.Size + sx0 + Span];
					double MaxError = 0.0;
					const double InvSpan = 1.0 / Span;
					for (int32 dz = 0; dz <= Span; dz++)
					{
						const double fz = dz * InvSpan;
						for (int32 dx = 0; dx <= Span; dx++)
						{
							const double fx = dx * InvSpan;
							const double Top = FMath::Lerp(h00, h10, fx);
							const double Bottom = FMath::Lerp(h01, h11, fx);
							const double Plane = FMath::Lerp(Top, Bottom, fz);
							const double Actual = T.Heights[(sz0 + dz) * T.Size + sx0 + dx];
							MaxError = FMath::Max(MaxError, FMath::Abs(Actual - Plane) * YScale);
						}
					}
					return MaxError;
				};

				auto RequiredStep = [&](auto&& Self, int32 sx0, int32 sz0, int32 Span) -> int32
				{
					if (Span <= 1) return 1;
					const double CellM = NativeSpacingM * Span;
					const double MaxErrorM = CellM > 2.01 ? 0.50 : (CellM > 1.01 ? 0.25 : 0.10);
					if (MaxPlaneErrorM(sx0, sz0, Span) <= MaxErrorM) return Span;
					const int32 Half = Span / 2;
					int32 Finest = Half;
					Finest = FMath::Min(Finest, Self(Self, sx0,        sz0,        Half));
					Finest = FMath::Min(Finest, Self(Self, sx0 + Half, sz0,        Half));
					Finest = FMath::Min(Finest, Self(Self, sx0,        sz0 + Half, Half));
					Finest = FMath::Min(Finest, Self(Self, sx0 + Half, sz0 + Half, Half));
					return Finest;
				};

				for (int32 bx = 0; bx < BaseCells; bx++)
				{
					const int32 FineStep = RequiredStep(RequiredStep, bx * Step, bz * Step, Step);
					int32 Level = 0;
					for (int32 s = Step; s > FineStep; s >>= 1) Level++;
					CellLevels[bz * BaseCells + bx] = (uint8)FMath::Clamp(Level, 0, MaxLevel);
				}
			});
		}
		const double TerrainAnalyseS = FPlatformTime::Seconds() - TerrainAnalyseStart;
		GTerrainCellsByLevel.Init(0, MaxLevel + 1);
		for (uint8 Level : CellLevels) GTerrainCellsByLevel[FMath::Min<int32>(Level, MaxLevel)]++;
		GTerrainBaseSpacingM = NativeSpacingM * Step;
		GTerrainMinSpacingM = NativeSpacingM * (Step >> MaxLevel);
		GTerrainCellLevels = CellLevels;
		GTerrainBaseCells = BaseCells;
		GTerrainNativeStep = Step;

		// One vertex of the native grid. Adjacent patches and tiles ask for the
		// same native coordinate, so their boundaries land bit-for-bit together.
		auto VertexAt = [&](int32 sx, int32 sz)
		{
			// Game space is X across, Z along, Y up. Unreal wants Z up, so the
			// same swap the props get, and the same unit change.
			const double gx = XLo + XSpan * ((double)sx / (T.Size - 1));
			const double gz = ZLo + ZSpan * ((double)sz / (T.Size - 1));
			const double gy = (double)T.Heights[sz * T.Size + sx] * YScale;
			return FVector3f((float)(gx * 100.0), (float)(gz * 100.0), (float)(gy * 100.0));
		};

		// Building a tile description touches no UObjects and shares only the
		// read-only heightfield.  These 256 jobs used to run serially even though
		// object descriptions below already use ParallelFor for the same reason.
		// Keep UObject creation/registration on the game thread after this pass.
		const int32 TileCount = Tiles * Tiles;
		TArray<FMeshDescription> TileDescriptions;
		TileDescriptions.SetNum(TileCount);
		TArray<uint8> TileValid;
		TileValid.SetNumZeroed(TileCount);
		TArray<int64> TileVertices;
		TileVertices.SetNumZeroed(TileCount);
		TArray<int64> TileTriangles;
		TileTriangles.SetNumZeroed(TileCount);
		const double TerrainDescribeStart = FPlatformTime::Seconds();
		ParallelFor(TileCount, [&](int32 TileIndex)
		{
			const int32 tx = TileIndex % Tiles;
			const int32 tz = TileIndex / Tiles;
			const int32 x0 = tx * Per, x1 = FMath::Min(x0 + Per, N - 1);
			const int32 z0 = tz * Per, z1 = FMath::Min(z0 + Per, N - 1);
			if (x1 <= x0 || z1 <= z0) return;

			FMeshDescription& MD = TileDescriptions[TileIndex];
			FStaticMeshAttributes Attr(MD);
			Attr.Register();
			TVertexAttributesRef<FVector3f>         VPos = Attr.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector2f> VUV  = Attr.GetVertexInstanceUVs();
			const FPolygonGroupID Group = MD.CreatePolygonGroup();
			Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("BF6Terrain");

			TMap<uint64, FVertexID> VertexByNative;
			MD.ReserveNewVertices((x1 - x0 + 1) * (z1 - z0 + 1));
			auto GetVertex = [&](int32 sx, int32 sz)
			{
				const uint64 Key = (uint64)(uint32)sx | ((uint64)(uint32)sz << 32);
				if (const FVertexID* Existing = VertexByNative.Find(Key)) return *Existing;
				const FVertexID Id = MD.CreateVertex();
				VPos[Id] = VertexAt(sx, sz);
				VertexByNative.Add(Key, Id);
				return Id;
			};
			auto SubdivAt = [&](int32 bx, int32 bz)
			{
				if (bx < 0 || bz < 0 || bx >= BaseCells || bz >= BaseCells) return 1;
				return 1 << CellLevels[bz * BaseCells + bx];
			};
			auto Corner = [&](int32 sx, int32 sz)
			{
				const FVertexInstanceID Vi = MD.CreateVertexInstance(GetVertex(sx, sz));
				VUV.Set(Vi, 0, FVector2f(
					(float)sx / (T.Size - 1), (float)sz / (T.Size - 1)));
				return Vi;
			};

			for (int32 bz = z0; bz < z1; bz++)
				for (int32 bx = x0; bx < x1; bx++)
				{
					const int32 Subdiv = SubdivAt(bx, bz);
					const int32 CellStep = Step / Subdiv;
					const int32 LeftSubdiv   = FMath::Max(Subdiv, SubdivAt(bx - 1, bz));
					const int32 RightSubdiv  = FMath::Max(Subdiv, SubdivAt(bx + 1, bz));
					const int32 TopSubdiv    = FMath::Max(Subdiv, SubdivAt(bx, bz - 1));
					const int32 BottomSubdiv = FMath::Max(Subdiv, SubdivAt(bx, bz + 1));
					const int32 NativeX = bx * Step, NativeZ = bz * Step;

					for (int32 lz = 0; lz < Step; lz += CellStep)
						for (int32 lx = 0; lx < Step; lx += CellStep)
						{
							const int32 lx1 = lx + CellStep, lz1 = lz + CellStep;
							TArray<FIntPoint, TInlineAllocator<36>> Ring;
							Ring.Add(FIntPoint(lx, lz));
							if (lx == 0)
								for (int32 q = lz + Step / LeftSubdiv; q < lz1; q += Step / LeftSubdiv)
									Ring.Add(FIntPoint(lx, q));
							Ring.Add(FIntPoint(lx, lz1));
							if (lz1 == Step)
								for (int32 q = lx + Step / BottomSubdiv; q < lx1; q += Step / BottomSubdiv)
									Ring.Add(FIntPoint(q, lz1));
							Ring.Add(FIntPoint(lx1, lz1));
							if (lx1 == Step)
								for (int32 q = lz1 - Step / RightSubdiv; q > lz; q -= Step / RightSubdiv)
									Ring.Add(FIntPoint(lx1, q));
							Ring.Add(FIntPoint(lx1, lz));
							if (lz == 0)
								for (int32 q = lx1 - Step / TopSubdiv; q > lx; q -= Step / TopSubdiv)
									Ring.Add(FIntPoint(q, lz));

							// Fan the projected-convex ring. Added edge points are the
							// exact points requested by the finer neighbour, so unlike a
							// T-junction both sides describe the same 3-D boundary.
							for (int32 i = 1; i + 1 < Ring.Num(); i++)
							{
								const FIntPoint& P0 = Ring[0];
								const FIntPoint& P1 = Ring[i];
								const FIntPoint& P2 = Ring[i + 1];
								MD.CreatePolygon(Group, TArray<FVertexInstanceID>{
									Corner(NativeX + P0.X, NativeZ + P0.Y),
									Corner(NativeX + P1.X, NativeZ + P1.Y),
									Corner(NativeX + P2.X, NativeZ + P2.Y) });
							}
						}
				}

			FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
			FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals);
			TileVertices[TileIndex] = MD.Vertices().Num();
			TileTriangles[TileIndex] = MD.Triangles().Num();
			TileValid[TileIndex] = 1;
		});
		const double TerrainDescribeS = FPlatformTime::Seconds() - TerrainDescribeStart;

		int32 Built = 0;
		const double TerrainCommitStart = FPlatformTime::Seconds();
		// THE COMMIT IN THREE PHASES, NOT ONE LOOP. The 256 tiles used to be
		// created, built and registered one after another on the game thread,
		// and the render-data build in the middle was 8.6 s of the 13.5 s
		// warm terrain phase. Object creation and component registration must
		// stay on the game thread; the render-data build (vertex/index buffers
		// from the description) does not touch UObjects once a BodySetup
		// exists, so it runs across cores. BodySetups are created up front for
		// exactly that reason - BuildFromMeshDescriptions would otherwise
		// NewObject one from a worker thread.
		TArray<UStaticMesh*> TileMeshes;
		TileMeshes.SetNumZeroed(TileCount);
		for (int32 TileIndex = 0; TileIndex < TileCount; TileIndex++)
		{
			if (!TileValid[TileIndex]) continue;
			const int32 tx = TileIndex % Tiles;
			const int32 tz = TileIndex / Tiles;
			UStaticMesh* SM = NewObject<UStaticMesh>(
				A, *FString::Printf(TEXT("Terrain_%d_%d"), tx, tz), RF_Transient);
			// THE GROUND'S REAL MATERIALS. Baked once for the whole map and
			// shared by every tile: the material addresses the bake by WORLD
			// POSITION, so one instance serves the lot and no tile needs UVs
			// of its own. Null leaves the tile unmaterialled, which is the
			// flat grey this replaces.
			FStaticMaterial GMat;
			GMat.MaterialInterface = GroundMat;
			SM->GetStaticMaterials().Add(GMat);
			SM->CreateBodySetup();
			TileMeshes[TileIndex] = SM;
		}
		TArray<uint8> TileBuiltOk;
		TileBuiltOk.SetNumZeroed(TileCount);
		ParallelFor(TileCount, [&](int32 TileIndex)
		{
			UStaticMesh* SM = TileMeshes[TileIndex];
			if (!SM) return;
			TileBuiltOk[TileIndex] = PrepareRuntimeRenderMesh(
				SM, TileDescriptions[TileIndex], (int32)TileTriangles[TileIndex]) ? 1 : 0;
		});
		for (int32 TileIndex = 0; TileIndex < TileCount; TileIndex++)
		{
			UStaticMesh* SM = TileMeshes[TileIndex];
			if (!SM) continue;
			const int32 tx = TileIndex % Tiles;
			const int32 tz = TileIndex / Tiles;
			if (!TileBuiltOk[TileIndex])
			{
				UE_LOG(LogBF6HighPoly, Error,
					TEXT("terrain tile %d,%d runtime render build failed"), tx, tz);
				continue;
			}

			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(
				A, *FString::Printf(TEXT("TerrainMesh_%d_%d"), tx, tz));
			C->SetupAttachment(Root);
			BF6HP::Shared::MakeUnselectable(C);
			C->SetMobility(EComponentMobility::Static);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetStaticMesh(SM);
			// The SDK's official map-image aid is a deferred decal. State this
			// explicitly so it continues to drape over the replacement terrain
			// even if an engine default or component template changes.
			C->SetReceivesDecals(true);
			C->RegisterComponent();
			Built++;
		}
		const double TerrainCommitS = FPlatformTime::Seconds() - TerrainCommitStart;
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("terrain prepare: analyse %.1fs, describe %.1fs parallel, commit %.1fs"),
			TerrainAnalyseS, TerrainDescribeS, TerrainCommitS);
		GTerrainTilesBuilt = Built;
		GTerrainVerticesBuilt = 0;
		GTerrainTrianglesBuilt = 0;
		for (int32 i = 0; i < TileCount; i++)
		{
			GTerrainVerticesBuilt += TileVertices[i];
			GTerrainTrianglesBuilt += TileTriangles[i];
		}
		FString Levels;
		for (int32 Level = 0; Level < GTerrainCellsByLevel.Num(); Level++)
		{
			if (!Levels.IsEmpty()) Levels += TEXT(", ");
			Levels += FString::Printf(TEXT("%.2fm=%lld"),
				GTerrainBaseSpacingM / (1 << Level), GTerrainCellsByLevel[Level]);
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("terrain adaptive cells: %s; %lld vertices, %lld triangles"),
			*Levels, GTerrainVerticesBuilt, GTerrainTrianglesBuilt);
		if (Built > 0) GBuiltAnything = true;
		return N;
	}

	// ---- ROADS AND STREET MARKINGS --------------------------------------
	//
	// The street surface is already there: the heightfield IS the asphalt. What
	// this adds is everything painted onto it - lane lines, crossings, mud,
	// wear, tyre tracks - which is the difference between ground and a road.
	//
	// A decal carries world X and Z and NO Y, so it has to be draped on the
	// ground we built rather than on the ground it was compiled against. Those
	// two are not the same surface, and the difference is the whole difficulty.
	float GRoadLift = 6.f;             // centimetres above the ground
	int32 GRoadRecords = 0, GRoadTris = 0, GRoadElevated = 0;
	int32 GRoadElevatedCandidates = 0, GRoadBandOnlyRejected = 0;
	int32 GRoadColourless = 0;   // records that bind no colour sheet at all
	int32 GRoadPainted = 0;      // ...of which carry an authored colour instead
	int32 GRoadReceiverVerts = 0, GRoadPoolHoleTris = 0, GRoadPoolShiftControl = 0;
	int64 GRoadGroundSamples = 0, GRoadGroundDeltaOverLift = 0;
	double GRoadGroundDeltaSumCm = 0.0, GRoadGroundDeltaMaxCm = 0.0;

	// THE UV ORDER IS THE ONE OPEN QUESTION HERE, and it cannot be settled by
	// arithmetic. The edge metric that validates the SCALE is symmetric in its
	// two terms, so it pins the tiling and cannot see a 90 degree rotation;
	// correlating v against a record's long axis has the same blind spot. Only
	// looking at it settles it: under the wrong order, tyre tracks run ACROSS
	// the direction of travel instead of along it. The research hub's corrected
	// reading is (u, v), so that is what this uses, and this switch exists so
	// the other order is one line away rather than a rebuild of the reasoning.
	bool GRoadUvSwapped = false;

	// Where the road phase spends its time, for the "roads:" line.
	double GSecRoadDrape = 0.0, GSecRoadMaterials = 0.0, GSecRoadCommit = 0.0;
	double GSecRoadComponents = 0.0;

	// Height of the ground at a world XZ, from the same grid the terrain mesh
	// was built from. Bilinear, because sampling the nearest vertex leaves a
	// decal stepping across a surface that is drawn smooth.
	static double GroundAt(const BF6HP::FCore::FTerrain& T, double YScale, double x, double z)
	{
		const double sx = T.WorldMax.X - T.WorldMin.X;
		const double sz = T.WorldMax.Z - T.WorldMin.Z;
		if (T.Size <= 1 || sx <= 0.0 || sz <= 0.0) return 0.0;
		double fx = (x - T.WorldMin.X) / sx;
		double fz = (z - T.WorldMin.Z) / sz;
		fx = FMath::Clamp(fx, 0.0, 1.0);
		fz = FMath::Clamp(fz, 0.0, 1.0);
		const double gx = fx * (T.Size - 1), gz = fz * (T.Size - 1);
		const int32 x0 = FMath::Min((int32)gx, T.Size - 1), z0 = FMath::Min((int32)gz, T.Size - 1);
		const int32 x1 = FMath::Min(x0 + 1, T.Size - 1),    z1 = FMath::Min(z0 + 1, T.Size - 1);
		const double tx = gx - x0, tz = gz - z0;
		auto H = [&](int32 a, int32 b) { return (double)T.Heights[b * T.Size + a] * YScale; };
		return H(x0, z0) * (1 - tx) * (1 - tz) + H(x1, z0) * tx * (1 - tz)
		     + H(x0, z1) * (1 - tx) * tz       + H(x1, z1) * tx * tz;
	}

	// Height on the triangles Unreal actually received.  GroundAt samples the
	// native heightfield, but the adaptive builder is deliberately allowed to
	// replace a quiet block by two much larger triangles.  A decal sampled from
	// the native grid can therefore hover above (or disappear below) the visible
	// mesh even though both answers came from the same heightfield.  Repeating
	// the builder's TL--BR diagonal here makes the decal and terrain identical.
	static double RenderedGroundAt(const BF6HP::FCore::FTerrain& T, double YScale,
	                               double x, double z)
	{
		if (T.Size <= 1 || GTerrainCellLevels.Num() == 0 || GTerrainBaseCells <= 0 ||
		    GTerrainNativeStep <= 0)
			return GroundAt(T, YScale, x, z);

		const double sx = T.WorldMax.X - T.WorldMin.X;
		const double sz = T.WorldMax.Z - T.WorldMin.Z;
		if (sx <= 0.0 || sz <= 0.0) return GroundAt(T, YScale, x, z);
		const double gx = FMath::Clamp((x - T.WorldMin.X) / sx, 0.0, 1.0) * (T.Size - 1);
		const double gz = FMath::Clamp((z - T.WorldMin.Z) / sz, 0.0, 1.0) * (T.Size - 1);
		const int32 bx = FMath::Clamp(FMath::FloorToInt(gx / GTerrainNativeStep),
		                                   0, GTerrainBaseCells - 1);
		const int32 bz = FMath::Clamp(FMath::FloorToInt(gz / GTerrainNativeStep),
		                                   0, GTerrainBaseCells - 1);
		const int32 Level = GTerrainCellLevels[bz * GTerrainBaseCells + bx];
		const int32 Subdiv = 1 << Level;
		const double CellStep = (double)GTerrainNativeStep / (double)Subdiv;
		const int32 subX = FMath::Clamp(FMath::FloorToInt((gx - bx * GTerrainNativeStep) / CellStep),
		                                     0, Subdiv - 1);
		const int32 subZ = FMath::Clamp(FMath::FloorToInt((gz - bz * GTerrainNativeStep) / CellStep),
		                                     0, Subdiv - 1);
		const double x0f = bx * GTerrainNativeStep + subX * CellStep;
		const double z0f = bz * GTerrainNativeStep + subZ * CellStep;
		const int32 x0 = FMath::Clamp(FMath::RoundToInt(x0f), 0, T.Size - 1);
		const int32 z0 = FMath::Clamp(FMath::RoundToInt(z0f), 0, T.Size - 1);
		const int32 x1 = FMath::Clamp(FMath::RoundToInt(x0f + CellStep), 0, T.Size - 1);
		const int32 z1 = FMath::Clamp(FMath::RoundToInt(z0f + CellStep), 0, T.Size - 1);
		const double fx = FMath::Clamp((gx - x0f) / CellStep, 0.0, 1.0);
		const double fz = FMath::Clamp((gz - z0f) / CellStep, 0.0, 1.0);
		auto H = [&](int32 a, int32 b) { return (double)T.Heights[b * T.Size + a] * YScale; };
		const double h00 = H(x0, z0), h10 = H(x1, z0);
		const double h01 = H(x0, z1), h11 = H(x1, z1);
		return fz >= fx
			? h00 * (1.0 - fz) + h01 * (fz - fx) + h11 * fx
			: h00 * (1.0 - fx) + h11 * fz + h10 * (fx - fz);
	}

	struct FDecalReceiverTriangle
	{
		FVector A, B, C; // game space, metres (Y up)
	};

	struct FDecalReceiverPatch
	{
		FVector2D Lo = FVector2D(DBL_MAX, DBL_MAX);
		FVector2D Hi = FVector2D(-DBL_MAX, -DBL_MAX);
		TArray<FDecalReceiverTriangle> Triangles;
		TArray<FVector2D> Hull;
		bool bClipHoles = false;
	};

	// Terrain decals do not mean "heightfield decals".  The game marks prop
	// material sections which receive the terrain-colour stack (slot 89D3AD5E,
	// with no ordinary albedo).  Pools, kerbs and slabs use those sections, so a
	// correct drape must try their authored triangles before falling back to the
	// terrain.  A 32 m bin keeps the per-vertex query bounded.
	struct FDecalReceiverIndex
	{
		static constexpr double BinM = 32.0;
		TArray<FDecalReceiverPatch> Patches;
		TMap<int64, TArray<int32>> Bins;
		int32 Sections = 0, Triangles = 0, PoolPatches = 0;

		static int64 Key(int32 X, int32 Z)
		{
			return (int64)(((uint64)(uint32)X << 32) | (uint32)Z);
		}

		static double Cross2(const FVector2D& A, const FVector2D& B, const FVector2D& C)
		{
			return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
		}

		void Add(const BF6HP::FCore::FSection& S, const BF6HP::FPlacement& P,
		         bool bPool)
		{
			FDecalReceiverPatch Patch;
			Patch.bClipHoles = bPool;
			TArray<FVector2D> Points;
			for (int32 i = 0; i + 2 < S.Idx.Num(); i += 3)
			{
				const uint32 ia = S.Idx[i], ib = S.Idx[i + 1], ic = S.Idx[i + 2];
				if (ia >= (uint32)S.Pos.Num() || ib >= (uint32)S.Pos.Num() || ic >= (uint32)S.Pos.Num()) continue;
				auto World = [&](const FVector3f& V)
				{
					return P.Origin + P.Right * V.X + P.Up * V.Y + P.Forward * V.Z;
				};
				const FVector A = World(S.Pos[ia]), B = World(S.Pos[ib]), C = World(S.Pos[ic]);
				const FVector N = FVector::CrossProduct(B - A, C - A);
				if (N.SizeSquared() < 1.e-10 || FMath::Abs(N.Y) / N.Size() < 0.45) continue;
				Patch.Triangles.Add({A, B, C});
				for (const FVector& V : {A, B, C})
				{
					const FVector2D Q(V.X, V.Z);
					Patch.Lo.X = FMath::Min(Patch.Lo.X, Q.X); Patch.Lo.Y = FMath::Min(Patch.Lo.Y, Q.Y);
					Patch.Hi.X = FMath::Max(Patch.Hi.X, Q.X); Patch.Hi.Y = FMath::Max(Patch.Hi.Y, Q.Y);
					if (bPool) Points.Add(Q);
				}
			}
			if (Patch.Triangles.Num() == 0) return;

			if (bPool && Points.Num() >= 3)
			{
				Points.Sort([](const FVector2D& A, const FVector2D& B)
				{
					return A.X == B.X ? A.Y < B.Y : A.X < B.X;
				});
				TArray<FVector2D> Unique;
				for (const FVector2D& Q : Points)
					if (Unique.Num() == 0 || !Q.Equals(Unique.Last(), 1.e-5)) Unique.Add(Q);
				TArray<FVector2D> H;
				for (const FVector2D& Q : Unique)
				{
					while (H.Num() >= 2 && Cross2(H[H.Num()-2], H.Last(), Q) <= 0.0) H.Pop();
					H.Add(Q);
				}
				const int32 Lower = H.Num();
				for (int32 i = Unique.Num() - 2; i >= 0; --i)
				{
					const FVector2D Q = Unique[i];
					while (H.Num() > Lower && Cross2(H[H.Num()-2], H.Last(), Q) <= 0.0) H.Pop();
					H.Add(Q);
				}
				if (H.Num() > 1) H.Pop();
				Patch.Hull = MoveTemp(H);
			}

			const int32 Id = Patches.Add(MoveTemp(Patch));
			const FDecalReceiverPatch& Added = Patches[Id];
			for (int32 bx = FMath::FloorToInt(Added.Lo.X / BinM); bx <= FMath::FloorToInt(Added.Hi.X / BinM); bx++)
				for (int32 bz = FMath::FloorToInt(Added.Lo.Y / BinM); bz <= FMath::FloorToInt(Added.Hi.Y / BinM); bz++)
					Bins.FindOrAdd(Key(bx, bz)).Add(Id);
			Sections++;
			Triangles += Added.Triangles.Num();
			if (bPool) PoolPatches++;
		}

		static bool InHull(const TArray<FVector2D>& H, const FVector2D& P)
		{
			if (H.Num() < 3) return false;
			for (int32 i = 0; i < H.Num(); i++)
				if (Cross2(H[i], H[(i + 1) % H.Num()], P) < -1.e-5) return false;
			return true;
		}

		bool TouchesPool(double MinX, double MinZ, double MaxX, double MaxZ) const
		{
			for (const FDecalReceiverPatch& P : Patches)
				if (P.bClipHoles && MaxX >= P.Lo.X && MinX <= P.Hi.X &&
				    MaxZ >= P.Lo.Y && MinZ <= P.Hi.Y) return true;
			return false;
		}

		bool Sample(double X, double Z, double MinY, double MaxY,
		            double& OutY, bool& bInsidePoolHole) const
		{
			bInsidePoolHole = false;
			bool bHit = false;
			double Best = -DBL_MAX;
			const TArray<int32>* List = Bins.Find(Key(FMath::FloorToInt(X / BinM), FMath::FloorToInt(Z / BinM)));
			if (!List) return false;
			const FVector2D Q(X, Z);
			for (const int32 Id : *List)
			{
				const FDecalReceiverPatch& P = Patches[Id];
				if (X < P.Lo.X || X > P.Hi.X || Z < P.Lo.Y || Z > P.Hi.Y) continue;
				bool bPatchHit = false;
				for (const FDecalReceiverTriangle& T : P.Triangles)
				{
					const double Den = (T.B.Z - T.C.Z) * (T.A.X - T.C.X) +
					                   (T.C.X - T.B.X) * (T.A.Z - T.C.Z);
					if (FMath::Abs(Den) < 1.e-10) continue;
					const double U = ((T.B.Z - T.C.Z) * (X - T.C.X) + (T.C.X - T.B.X) * (Z - T.C.Z)) / Den;
					const double V = ((T.C.Z - T.A.Z) * (X - T.C.X) + (T.A.X - T.C.X) * (Z - T.C.Z)) / Den;
					const double W = 1.0 - U - V;
					if (U < -1.e-5 || V < -1.e-5 || W < -1.e-5) continue;
					const double Y = U * T.A.Y + V * T.B.Y + W * T.C.Y;
					if (Y < MinY - 1.0 || Y > MaxY + 1.0) continue;
					bPatchHit = true;
					if (!bHit || Y > Best) { Best = Y; bHit = true; }
				}
				if (P.bClipHoles && !bPatchHit && InHull(P.Hull, Q)) bInsidePoolHole = true;
			}
			if (bHit) OutY = Best;
			return bHit;
		}
	};

	// WHAT A MATERIAL ACTUALLY IS, once the editor has finished with it.
	//
	// A material can be built, report no compile errors, and still not be the
	// thing that was asked for: a shading model can fail to stick, a blend mode
	// can be overridden, and a custom output can sit in the graph with nothing
	// connected to it. All three look identical from the outside - a surface that
	// draws, wrongly - and all three are cheap to state outright.
	static const TCHAR* BF6_ShadingModelName(EMaterialShadingModel S)
	{
	    switch (S)
	    {
	    case MSM_Unlit:            return TEXT("unlit");
	    case MSM_DefaultLit:       return TEXT("default lit");
	    case MSM_Subsurface:       return TEXT("subsurface");
	    case MSM_PreintegratedSkin:return TEXT("preintegrated skin");
	    case MSM_ClearCoat:        return TEXT("clear coat");
	    case MSM_SubsurfaceProfile:return TEXT("subsurface profile");
	    case MSM_TwoSidedFoliage:  return TEXT("two sided foliage");
	    case MSM_Hair:             return TEXT("hair");
	    case MSM_Cloth:            return TEXT("cloth");
	    case MSM_Eye:              return TEXT("eye");
	    case MSM_SingleLayerWater: return TEXT("single layer water");
	    case MSM_ThinTranslucent:  return TEXT("thin translucent");
	    default:                   return TEXT("other");
	    }
	}

	static const TCHAR* BF6_BlendModeName(EBlendMode B)
	{
	    switch (B)
	    {
	    case BLEND_Opaque:         return TEXT("opaque");
	    case BLEND_Masked:         return TEXT("masked");
	    case BLEND_Translucent:    return TEXT("translucent");
	    case BLEND_Additive:       return TEXT("additive");
	    case BLEND_Modulate:       return TEXT("modulate");
	    case BLEND_AlphaComposite: return TEXT("alpha composite");
	    case BLEND_AlphaHoldout:   return TEXT("alpha holdout");
	    // Substrate only, and it is what a translucent material becomes once
	    // the legacy conversion has run - so this is the NORMAL answer for
	    // glass on this project, not an oddity.
	    case BLEND_TranslucentColoredTransmittance:
	                               return TEXT("translucent, coloured transmittance");
	    default:                   return TEXT("other");
	    }
	}

	// Every CUSTOM OUTPUT in the graph, and which of its pins are actually wired.
	//
	// An unconnected pin on a custom output does not error. It compiles to that
	// output's DEFAULT, and the material then behaves in a way the graph does not
	// show. Single Layer Water is the case in hand: it has four pins and we
	// connect two, so the other two are running on defaults nobody has stated.
	static void BF6_ReportCustomOutputs(const TCHAR* Name, UMaterial* M)
	{
	    if (!M) return;
	    for (UMaterialExpression* E : M->GetExpressionCollection().Expressions)
	    {
	        UMaterialExpressionCustomOutput* CO = Cast<UMaterialExpressionCustomOutput>(E);
	        if (!CO) continue;
	        FString Wired, Bare;
	        const int32 N = CO->CountInputs();
	        for (int32 i = 0; i < N; i++)
	        {
	            const FExpressionInput* In = CO->GetInput(i);
	            const FString Pin = CO->GetInputName(i).ToString();
	            FString& Bucket = (In && In->Expression) ? Wired : Bare;
	            if (!Bucket.IsEmpty()) Bucket += TEXT(", ");
	            Bucket += Pin;
	        }
	        UE_LOG(LogBF6HighPoly, Display,
	            TEXT("CHECK %s: custom output %s - connected [%s], on defaults [%s]"),
	            Name, *CO->GetClass()->GetName(),
	            Wired.IsEmpty() ? TEXT("none") : *Wired,
	            Bare.IsEmpty() ? TEXT("none") : *Bare);
	    }
	}

	// What a material BECAME, in one line plus its custom outputs.
	// THE ROOT NODE'S OWN PINS.
	//
	// BF6_ReportCustomOutputs covers the custom output nodes; this covers the
	// material INPUTS, which is where the water bug actually was. The Single
	// Layer Water custom output was correctly wired the whole time. The pin
	// that was wrong was Opacity, on the root, and nothing reported it.
	//
	// Caveat under Substrate: the legacy conversion MOVES most connections
	// onto its own convert node, so BaseColor and friends read as unconnected
	// after PostEditChange. Opacity is COPIED rather than moved
	// (Material.cpp:3977), so it still reads correctly here, which is the one
	// this check exists for.
	static void BF6_ReportRootInputs(const TCHAR* Name, UMaterial* M)
	{
	    if (!M) return;
	    struct FPin { EMaterialProperty P; const TCHAR* N; };
	    static const FPin Pins[] = {
	        { MP_BaseColor,           TEXT("BaseColor") },
	        { MP_Metallic,            TEXT("Metallic") },
	        { MP_Specular,            TEXT("Specular") },
	        { MP_Roughness,           TEXT("Roughness") },
	        { MP_EmissiveColor,       TEXT("Emissive") },
	        { MP_Opacity,             TEXT("Opacity") },
	        { MP_OpacityMask,         TEXT("OpacityMask") },
	        { MP_Normal,              TEXT("Normal") },
	        { MP_WorldPositionOffset, TEXT("WorldPositionOffset") },
	    };
	    FString Wired, Bare;
	    for (const FPin& Pin : Pins)
	    {
	        const FExpressionInput* In = M->GetExpressionInputForProperty(Pin.P);
	        FString& Bucket = (In && In->Expression) ? Wired : Bare;
	        if (!Bucket.IsEmpty()) Bucket += TEXT(", ");
	        Bucket += Pin.N;
	    }
	    UE_LOG(LogBF6HighPoly, Display,
	        TEXT("%s: root inputs - connected [%s], on defaults [%s]"),
	        Name, Wired.IsEmpty() ? TEXT("none") : *Wired,
	        Bare.IsEmpty() ? TEXT("none") : *Bare);

	    const FExpressionInput* Op = M->GetExpressionInputForProperty(MP_Opacity);
	    if (M->GetShadingModels().HasShadingModel(MSM_SingleLayerWater) &&
	        (!Op || !Op->Expression))
	    {
	        UE_LOG(LogBF6HighPoly, Error,
	            TEXT("%s: SINGLE LAYER WATER WITH OPACITY UNCONNECTED. Opacity ")
	            TEXT("defaults to 1; the base pass reads WaterVisibility = 1 - Opacity ")
	            TEXT("and skips the entire water volume at zero visibility. No scene ")
	            TEXT("behind the water, no scattering, no absorption. The surface will ")
	            TEXT("be opaque whatever its coefficients say."), Name);
	    }
	}

	static void BF6_ReportMaterialState(const TCHAR* Name, UMaterial* M)
	{
	    if (!M) return;
	    FString Models;
	    const FMaterialShadingModelField SM = M->GetShadingModels();
	    for (int32 s = 0; s < MSM_NUM; s++)
	    {
	        if (!SM.HasShadingModel((EMaterialShadingModel)s)) continue;
	        if (!Models.IsEmpty()) Models += TEXT(" + ");
	        Models += BF6_ShadingModelName((EMaterialShadingModel)s);
	    }
	    UE_LOG(LogBF6HighPoly, Log,
	        TEXT("%s material: shading model %s, blend %s, %d expression(s)"),
	        Name, Models.IsEmpty() ? TEXT("NONE") : *Models,
	        BF6_BlendModeName(M->GetBlendMode()),
	        M->GetExpressionCollection().Expressions.Num());
	    BF6_ReportCustomOutputs(Name, M);
	    BF6_ReportRootInputs(Name, M);
	}

	// ---- water ----------------------------------------------------------
	//
	// Its own parent rather than a seventh EKind: water is not a per-section
	// material - it is Unreal's Single Layer Water shading model, which wants
	// an opaque blend and a dedicated output node, and none of the generic
	// texture parameters. The Godot plugin draws its water as a rippled quad
	// with preset colours; here the engine does the real work - depth-based
	// absorption and scattering, screen-space refraction, reflections - and
	// the mined per-map colours drive the coefficients.
	// THE FURTHEST PLACEMENT THIS LEVEL MAKES, in centimetres, so the sky can
	// be sized to enclose it instead of to a constant. Measured on
	// MP_Isolated: 135,093 m, which is three and a half times the 36.8 km the
	// authored backdrop ring was believed to stop at.
	float GFarWorldRadiusCm = 0.f;
	// The ocean surface descriptor, kept so the 192 km far-world horizon plane
	// can be given the same water shading as the simulated sea.
	BF6HP::FCore::FWater GOceanDesc;
	bool GHasOceanDesc = false;
	TWeakObjectPtr<USkyLightComponent> GHighPolySkyLight;

	UMaterial* GWaterParent = nullptr;
	TUniquePtr<BF6HP::FWaterFFT> GWaterFFT;

	// THE AUTHORED COEFFICIENTS, kept per surface.
	//
	// How far you can see into the water is exp(-extinction * depth), so the
	// one number worth moving is a scale on extinction. Scaling it live means
	// knowing what was mined, which the instance would otherwise be the only
	// record of. Same pattern as the light ceiling.
	// THE ONE FREE NUMBER LEFT IN THE WATER.
	//
	// Extinction is decoded and the albedo hue is the level's own derived
	// surface colour, so this scale is all that is not read from the game. It
	// decides how much light comes back OUT of the water against how much is
	// swallowed: at 1.0 the water returns as much as the level says its
	// surface colour is, which on a bright reef is a lot. Turn it down if the
	// water reads milky, up if it reads like ink.
	float GWaterAlbedoScale = 1.f;
	// PROVISIONAL INTERACTIVE-WATER BRIDGE. The ocean FFT is not the complete
	// Frostbite water field: the selected draw pass also consumes the separate
	// WaterInteractiveDisplacement atlas.  libbf6 does not expose that runtime
	// atlas yet, so use the level's mounted broad/noise inputs to keep the surface
	// visibly displaced while the exact interactive-atlas producer is ported.
	// This must remain reported as PROVISIONAL, never EXACT.
	float GWaterFoamWaveHeightM = 4.f;
	// OVERLAP OVERRIDE for A/B, -1 = whatever the level authored.
	//
	// The overlap is the game's rotated second sample of cascade 0, which the
	// DXIL decode describes as breaking up the largest cascade's visible tiling.
	// It matters because cascade 0's H0 is a CORRUGATION - 99.7% of its energy
	// inside 45 degrees, confirmed by executing the shipped builder itself - so
	// if the sea is ever to look two-dimensional, something has to add a second
	// direction, and this is the only candidate in the draw path.
	//
	// Whether our transform reproduces the game's is unresolved: libbf6 stores
	// params2 as (1/a, 1/b, a, b) mirroring CB0[8], and which pair the game
	// scales the input UV by is not established by the decode. So measure it
	// instead of arguing about it.
	// DEFAULT OFF, and this is a deliberate divergence from the authored data.
	//
	// A/B at a fixed camera (captures overlap-ON / overlap-OFF): with the overlap
	// ON the sea is covered in harsh directional slivers radiating to the horizon;
	// with it OFF the same frame is clean water with broad undulations and correct
	// reflections. Spun 180 degrees with it OFF, both directions look like water -
	// so this, not the spectrum, is what made the sea "flat when I turn".
	//
	// The cause is the transform, not the feature. Our UV uses params2.xy, which
	// libbf6 fills with (1/a, 1/b) = (76.9, 6.67) from the authored OverlapSizeA/B
	// of 0.013 and 0.15. That samples the 300 m cascade-0 texture as if it were
	// 3.9 m x 45 m - an 11.5:1 anisotropic stretch at tiny scale, which is
	// directional by construction. The DXIL decode says the game scales the input
	// by CB0[8].xy and the output by CB0[8].zw, but does NOT establish which pair
	// libbf6's params2 holds, so the correct assignment is unrecovered.
	//
	// Leaving a demonstrably wrong transform enabled is worse than disabling the
	// feature: it dominates the surface. Set BF6.HighPoly.WaterOverlap -1 to
	// restore the authored value, or 1 to force it on, once the transform is
	// verified against the shipped vertex shader.
	int32 GWaterOverlapOverride = 0;
	float GWaterBroadCarrierSize = 1.f;
	float GWaterBroadTimeScale = 1.f;
	// Profile-space window used only to couple open-water foam to the broad
	// deformation carrier.  Keeping both ends live lets the BF6 frame oracle
	// distinguish a missing decoded foam field from an over-tight crest gate
	// without recompiling the material for every comparison.
	float GWaterFoamCrestStart = 0.55f;
	float GWaterFoamCrestFull = 0.90f;
	// The fast micro/foam sheet is useful for inspecting the recovered draw
	// inputs, but it reads as white dots skating over Tsuru.  Keep the raw
	// textures mounted and make their contribution diagnostic-only by default.
	bool GWaterUseFastSheetDetail = false;
	UTexture2DArray* DefaultWaterMaskAtlas();

	// Unreal's default Single Layer Water reflection mode follows the scene's
	// reflection method.  In an editor viewport that can select SSR, whose
	// off-screen misses reuse stretched screen history after a transient water
	// graph is rebuilt.  The Water Lab never exercised that route; it was lit by
	// a stable sky contribution.  Make the full build use the equivalent stable
	// capture/sky path as a rebuild invariant.  The previous value is logged as
	// the control so this is a measured renderer translation, not a hidden knob.
	void ApplyStableWaterReflectionPolicy()
	{
		IConsoleVariable* Reflection = IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.Water.SingleLayer.Reflection"));
		IConsoleVariable* Reconstruction = IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.Water.SingleLayer.Reflection.ScreenSpaceReconstruction"));
		const int32 ReflectionBefore = Reflection ? Reflection->GetInt() : -1;
		const int32 ReconstructionBefore = Reconstruction ? Reconstruction->GetInt() : -1;
		if (Reflection) Reflection->Set(2, ECVF_SetByCode); // captures + skylight only
		if (Reconstruction) Reconstruction->Set(0, ECVF_SetByCode);
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("water reflection policy: mode %d -> %d (2=captures/sky), screen reconstruction %d -> %d; mode 1 is the scene-method control"),
			ReflectionBefore, Reflection ? Reflection->GetInt() : -1,
			ReconstructionBefore, Reconstruction ? Reconstruction->GetInt() : -1);
	}

	struct FWaterCoeff
	{
		TWeakObjectPtr<UMaterialInstanceDynamic> MID;
		FLinearColor Scatter;
		FLinearColor Absorb;
	};
	TArray<FWaterCoeff> GWaterCoeffs;

	UMaterial* EnsureWaterMaterial()
	{
		if (GWaterParent) return GWaterParent;
		UPackage* Pkg = CreatePackage(TEXT("/Temp/BF6HighPoly_Water"));
		if (!Pkg) return nullptr;
		Pkg->SetFlags(RF_Transient);
		// The lab may rebuild this graph while the render thread still owns the
		// previous MID for a frame. Reusing the exact UObject name can replace the
		// old material underneath that render proxy. Give each graph generation a
		// unique identity; old MIDs then retire through normal UObject references.
		const FName MaterialName = MakeUniqueObjectName(
			Pkg, UMaterial::StaticClass(), TEXT("M_BF6HighPoly_Water"));
		UMaterial* M = NewObject<UMaterial>(Pkg, MaterialName, RF_Transient);
		if (!M) return nullptr;
		// GWaterParent is a native cache, not a UPROPERTY. Root the cached graph so
		// a GC between lab reloads cannot leave this raw pointer dangling.
		M->AddToRoot();
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_SingleLayerWater);
		M->BlendMode = BLEND_Opaque;   // Single Layer Water IS opaque; depth is the shader's job

		UMaterialExpressionSingleLayerWaterMaterialOutput* SLW =
			Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionSingleLayerWaterMaterialOutput::StaticClass(), 100, 300));
		auto Vec = [&](const TCHAR* Name, FLinearColor Def, int32 Y)
			-> UMaterialExpressionVectorParameter*
		{
			UMaterialExpressionVectorParameter* P =
				Cast<UMaterialExpressionVectorParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionVectorParameter::StaticClass(), -400, Y));
			if (P) { P->ParameterName = Name; P->DefaultValue = Def; }
			return P;
		};
		// Defaults are a believable lake; the per-plane instance overrides
		// them from the mined record.
		UMaterialExpressionVectorParameter* Scatter =
			Vec(TEXT("Scattering"), FLinearColor(0.010f, 0.038f, 0.045f), 260);
		UMaterialExpressionVectorParameter* Absorb =
			Vec(TEXT("Absorption"), FLinearColor(0.30f, 0.115f, 0.085f), 360);

		// THE SURFACE ALBEDO, which is what the game itself writes.
		//
		// Its water pixel shader converts the authored colour with
		// saturate(1 + ln(c)/d) and puts THAT in the G-buffer as the base
		// colour. This was previously a near-black constant, on the theory
		// that all the colour should come out of the volume - and near-black
		// base colour plus a volume is how you render black water. The
		// instance overwrites this with the level's own converted colour.
		UMaterialExpressionVectorParameter* Tint =
			Vec(TEXT("SurfaceTint"), FLinearColor(0.10f, 0.45f, 0.55f), 60);
		if (Tint) UMaterialEditingLibrary::ConnectMaterialProperty(Tint, TEXT(""), MP_BaseColor);
		UMaterialExpressionScalarParameter* Rough =
			Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionScalarParameter::StaticClass(), -400, 160));
		if (Rough)
		{
			Rough->ParameterName = TEXT("WaterRoughness");
			Rough->DefaultValue = 0.04f;
			UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		}
		UMaterialExpressionScalarParameter* Specular =
			Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionScalarParameter::StaticClass(), -400, 220));
		if (Specular)
		{
			Specular->ParameterName = TEXT("WaterSpecular");
			Specular->DefaultValue = 0.5f; // Unreal dielectric F0 = 0.08 * this
			UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);
		}

		// ---- THE WAVES: eight Gerstner components in one HLSL node --------
		//
		// The wave FIELD is a runtime simulation in the game and is not on
		// disk; its INPUTS are, and they arrive through the instance's Wave0-7
		// parameters (direction, relative amplitude, relative wavelength),
		// derived from the map's own WindAngle, WindDistribution, WindSpeed
		// and Choppiness. Displacement moves the vertices; the paired node
		// rebuilds the analytic normal so the lighting follows the crests.
		// Deep-water dispersion (speed = sqrt(g/k)) so long swells outrun the
		// chop, which is most of what makes a sea read as one.
		auto Scal = [&](const TCHAR* Nm, float Def, int32 Y) -> UMaterialExpressionScalarParameter*
		{
			UMaterialExpressionScalarParameter* S =
				Cast<UMaterialExpressionScalarParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionScalarParameter::StaticClass(), -900, Y));
			if (S) { S->ParameterName = Nm; S->DefaultValue = Def; }
			return S;
		};

		// How pronounced the fine ripples are. Ours, not the game's - the
		// game gets this detail from its simulation's normal cascades.
		UMaterialExpressionScalarParameter* Det = Scal(TEXT("DetailRipple"), 1.f, 1220);
		UMaterialExpressionScalarParameter* DetStart = Scal(TEXT("DetailFadeStart"), 100.f, 1250);
		UMaterialExpressionScalarParameter* DetEnd = Scal(TEXT("DetailFadeEnd"), 300.f, 1270);
		UMaterialExpressionScalarParameter* ShD = Scal(TEXT("ShoreFadeDistance"), 6.f, 1280);
		UMaterialExpressionScalarParameter* AddD = Scal(TEXT("AdditionalWaterDepth"), 0.f, 1310);
		UMaterialExpressionScalarParameter* UseSh = Scal(TEXT("UseAuthoredShoreCurve"), 0.f, 1340);
		// A placeholder texture is not terrain.  When no heightfield is bound,
		// make shore attenuation neutral instead of interpreting LinearWhite as
		// terrain at +1 m and suppressing the large FFT cascades everywhere.
		UMaterialExpressionScalarParameter* DepthAvailable =
			Scal(TEXT("BF6DepthAvailable"), 0.f, 1360);
		UMaterialExpressionVectorParameter* ShB =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, 1370));
		if (ShB)
		{
			ShB->ParameterName = TEXT("ShoreBlend");
			ShB->DefaultValue = FLinearColor(0.f, 0.f, 1.f, 0.f);
		}
		// THE CLARITY RAMP IS NOT THE WAVE FADE. The former is the deferred
		// composite's saturate(pathLength/cb51.y) and comes from the active
		// VisualEnvironment OceanComponentData (0.5 m on Tsuru). The surface's
		// 14.0251 m ShoreDepth is dead because Tsuru selects CoarseMask; using it
		// here was a cross-record semantic error.
		// No ClarityDepth parameter any more. It fed a ramp that duplicated
		// the shading model's own exp(-extinction * depth), and the live knob
		// that replaced it scales the EXTINCTION, which is the quantity that
		// actually sets how far you can see.
		UMaterialExpressionScalarParameter* ShF = Scal(TEXT("ShoreFoamSuppression"), 1.f, 1400);
		UMaterialExpressionScalarParameter* DrawFTh = Scal(TEXT("DrawFoamThreshold"), 0.f, 1430);
		// THE CASCADE FOAM CHAIN (research foam.md 2.6 / 4.2). Coverage in the
		// shipped draw pass is a fold-channel chain over the four cascades, not
		// a Gerstner crest. The chain is switchable so it can be A/B'd live
		// against the old crest through the MCP (BF6FoamChain 0/1); it is
		// switched ON at bind time only when the level supplies the weights.
		UMaterialExpressionScalarParameter* FoamW[4] = {};
		for (int32 fi = 0; fi < 4; ++fi)
			FoamW[fi] = Scal(*FString::Printf(TEXT("BF6FoamW%d"), fi), 0.f, 1442 + fi * 4);
		UMaterialExpressionScalarParameter* FoamChain = Scal(TEXT("BF6FoamChain"), 0.f, 1460);
		UMaterialExpressionScalarParameter* FCon = Scal(TEXT("FoamContrast"), 1.f, 1460);
		UMaterialExpressionScalarParameter* FoamRough = Scal(TEXT("FoamRoughness"), 0.3f, 1490);
		UMaterialExpressionVectorParameter* CompositeFoamTint =
			Vec(TEXT("CompositeFoamTint"), FLinearColor(0.711f, 0.711f, 0.711f), 1505);
		UMaterialExpressionVectorParameter* DMin =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, 1400));
		if (DMin) { DMin->ParameterName = TEXT("DepthMin"); DMin->DefaultValue = FLinearColor::Black; }
		UMaterialExpressionVectorParameter* DSpan =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, 1460));
		if (DSpan) { DSpan->ParameterName = TEXT("DepthSpan"); DSpan->DefaultValue = FLinearColor(1, 1, 1, 1); }
		UMaterialExpressionTextureObjectParameter* DTex =
			Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -900, 1520));
		if (DTex)
		{
			DTex->ParameterName = TEXT("DepthTex");
			DTex->SamplerType = SAMPLERTYPE_LinearColor;
			DTex->Texture = LinearWhite();
		}
		// LARGE WATER SURFACE. The selected BF6 vertex shader samples terrain
		// streaming-tree block 2 before applying the ocean FFT:
		//   h=max(H(x,z)-flatY,0); worldY+=h
		// This is the metres-scale Tsuru shape the FFT cannot provide.
		UMaterialExpressionScalarParameter* WaterHeightAvailable =
			Scal(TEXT("BF6WaterHeightAvailable"), 0.f, 1540);
		UMaterialExpressionTextureObjectParameter* WaterHeightTex =
			Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -1050, 1560));
		if (WaterHeightTex)
		{
			WaterHeightTex->ParameterName = TEXT("BF6WaterHeightTex");
			WaterHeightTex->SamplerType = SAMPLERTYPE_LinearColor;
			WaterHeightTex->Texture = LinearWhite();
		}
		UMaterialExpressionVectorParameter* WaterHeightMin =
			Vec(TEXT("BF6WaterHeightMin"), FLinearColor::Black, 1580);
		UMaterialExpressionVectorParameter* WaterHeightSpan =
			Vec(TEXT("BF6WaterHeightSpan"), FLinearColor(1,1,1,1), 1600);
		UMaterialExpressionVectorParameter* WaterHeightTexel =
			Vec(TEXT("BF6WaterHeightTexelM"), FLinearColor(1,1,0,0), 1620);
		// xy maps a camera-relative lab patch back into authored game XZ; z is
		// the original flat entity height used by the exact max(H-flatY,0) law.
		UMaterialExpressionVectorParameter* WaterSourceWorld =
			Vec(TEXT("BF6WaterSourceWorld"), FLinearColor::Black, 1640);
		// EXACT COARSE WAVE MASK. Terrain streaming-tree block 10 is a sparse
		// utility raster, not a conventional image. The core reconstructs the
		// runtime's R8 page array and packed indirection table directly from the
		// installed game. Tsuru's selected vertex shader applies 1-mask to FFT
		// cascades 0/1 and their overlap, while cascades 2/3 bypass it.
		UMaterialExpressionScalarParameter* WaterMaskAvailable =
			Scal(TEXT("BF6WaterMaskAvailable"), 0.f, 1660);
		UMaterialExpressionTextureObjectParameter* WaterMaskAtlas =
			Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -1050, 1680));
		if (WaterMaskAtlas)
		{
			WaterMaskAtlas->ParameterName = TEXT("BF6WaterMaskAtlas");
			WaterMaskAtlas->SamplerType = SAMPLERTYPE_Masks;
			WaterMaskAtlas->Texture = DefaultWaterMaskAtlas();
		}
		UMaterialExpressionTextureObjectParameter* WaterMaskIndirection =
			Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -1050, 1700));
		if (WaterMaskIndirection)
		{
			WaterMaskIndirection->ParameterName = TEXT("BF6WaterMaskIndirection");
			WaterMaskIndirection->SamplerType = SAMPLERTYPE_Masks;
			WaterMaskIndirection->Texture = LinearWhite();
		}
		UMaterialExpressionVectorParameter* WaterMaskMin =
			Vec(TEXT("BF6WaterMaskMin"), FLinearColor::Black, 1720);
		UMaterialExpressionVectorParameter* WaterMaskSpan =
			Vec(TEXT("BF6WaterMaskSpan"), FLinearColor(1,1,1,1), 1740);
		UMaterialExpressionVectorParameter* WaterMaskMeta =
			Vec(TEXT("BF6WaterMaskMeta"), FLinearColor(1,0,1,1), 1760);
		// Vector parameters compile to float3 in Custom nodes; keep the table side
		// as a scalar rather than relying on an unavailable alpha component.
		UMaterialExpressionScalarParameter* WaterMaskIndirectionSide =
			Scal(TEXT("BF6WaterMaskIndirectionSide"), 1.f, 1780);
		// WAVE ENTITIES - the 16 radial swell slots the game's draw VS consumes
		// (finding water-wave-entities-render-contract-and-negative-census).
		// Per slot: xy = world centre (metres), z = 1/radius, w = HalfAmplitude
		// (metres; the mound peaks at 2*HalfAmplitude). The game disables a slot
		// by writing RecipRadius = 1e10, which drives t = min(d*recipR, 1) to 1
		// instantly - zero mound, zero dampening - so that exact sentinel is the
		// parameter default here. No level ships authored instances (census: 0
		// of 23,106 mounted partitions on mp_isolated); at runtime the game
		// fills these from code, so the slots exist to be driven, not read.
		UMaterialExpressionVectorParameter* WaveEnt[16] = {};
		UMaterialExpressionScalarParameter* WaveAmp[16] = {};
		for (int32 iWe = 0; iWe < 16; ++iWe)
		{
			// xyz only: a vector parameter reaches a custom node as float3.
			WaveEnt[iWe] = Vec(*FString::Printf(TEXT("BF6WaveEntity%d"), iWe),
				FLinearColor(0.f, 0.f, 1.0e10f, 0.f), 1790 + iWe * 10);
			WaveAmp[iWe] = Scal(*FString::Printf(TEXT("BF6WaveAmp%d"), iWe),
				0.f, 1795 + iWe * 10);
		}
		// LIVE DRAW-DETAIL SHEETS. Tsuru authors FoamMaxValue=0, so its FFT
		// folding texture contributes no whitecaps. That does not mean the draw
		// material has no foam: BF6 combines t_oceanmicrodetail_nsh and
		// t_oceanfoam_nsh at draw time, and those two sheets remain bound on
		// MP_Isolated. Ignoring them was why "zero sim foam" became "zero foam".
		auto WaterSheet = [&](const TCHAR* Name, int32 Y)
			-> UMaterialExpressionTextureObjectParameter*
		{
			UMaterialExpressionTextureObjectParameter* P =
				Cast<UMaterialExpressionTextureObjectParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionTextureObjectParameter::StaticClass(), -900, Y));
			if (P)
			{
				P->ParameterName = Name;
				P->SamplerType = SAMPLERTYPE_Masks;
				P->Texture = LinearWhite();
			}
			return P;
		};
		UMaterialExpressionTextureObjectParameter* MicroSheet =
			WaterSheet(TEXT("MicroSheet"), 1580);
		UMaterialExpressionTextureObjectParameter* FoamSheet =
			WaterSheet(TEXT("FoamSheet"), 1640);
		UMaterialExpressionTextureObjectParameter* ContactFoam =
			WaterSheet(TEXT("ContactFoam"), 1700);
		UMaterialExpressionTextureObjectParameter* BroadPattern =
			WaterSheet(TEXT("BroadPattern"), 1730);
		UMaterialExpressionTextureObjectParameter* OceanNoise =
			WaterSheet(TEXT("OceanNoise"), 1760);
		UMaterialExpressionScalarParameter* UseSheets =
			Scal(TEXT("UseDetailSheets"), 0.f, 1520);
		UMaterialExpressionScalarParameter* UseContact =
			Scal(TEXT("UseContactFoam"), 0.f, 1540);
		// MP_Isolated pass 0 binds the contact mask at common-resource
		// destination 1 (t33). Its divisor and remap are runtime depot reads;
		// the defaults below are only absent-data fallbacks for an older core.
		UMaterialExpressionScalarParameter* ContactDiv =
			Scal(TEXT("ContactWorldDivisorM"), 73718.f, 1560);
		UMaterialExpressionScalarParameter* ContactLow =
			Scal(TEXT("ContactRemapLow"), 0.f, 1580);
		UMaterialExpressionScalarParameter* ContactGain =
			Scal(TEXT("ContactGain"), 0.916f, 1600);
		UMaterialExpressionScalarParameter* FoamCompositeLow =
			Scal(TEXT("FoamCompositeLow"), 0.0427f, 1620);
		UMaterialExpressionScalarParameter* FoamCompositeHigh =
			Scal(TEXT("FoamCompositeHigh"), 1.14f, 1640);
		// OceanCompositionConstants cb51.z. The deferred composite suppresses
		// foam where the refracted background is less than this distance behind
		// the water surface: saturate(pathLength / cb51.z) * foamCoverage.
		// OceanComponentData carries the source as FoamShoreFadeDistance; every
		// shipped preset currently reads 0.5 m (the executable default as well).
		// Keep it separate from ShoreFadeDistance: that one attenuates FFT waves.
		UMaterialExpressionScalarParameter* FoamDepthRampDistance =
			Scal(TEXT("FoamDepthRampDistance"), 0.5f, 1645);
		UMaterialExpressionScalarParameter* UseBroadPattern =
			Scal(TEXT("UseBroadPattern"), 0.f, 1650);
		UMaterialExpressionScalarParameter* BroadPatternWorldMul =
			Scal(TEXT("BroadPatternWorldMul"), 0.1f, 1660);
		UMaterialExpressionScalarParameter* BroadPatternWorldScale =
			Scal(TEXT("BroadPatternWorldScale"), 95.284f, 1670);
		UMaterialExpressionScalarParameter* BroadPatternFloor =
			Scal(TEXT("BroadPatternFloor"), 0.41f, 1680);
		UMaterialExpressionScalarParameter* ExtendedGraphVersion =
			Scal(TEXT("ExtendedGraphVersion"), 0.f, 1690);
		UMaterialExpressionScalarParameter* FoamWaveHeight =
			Scal(TEXT("BF6FoamWaveHeightM"), 0.f, 1700);
		UMaterialExpressionScalarParameter* BroadCarrierSize =
			Scal(TEXT("BF6BroadCarrierSize"), 1.f, 1710);
		UMaterialExpressionScalarParameter* BroadTimeScale =
			Scal(TEXT("BF6BroadTimeScale"), 1.f, 1720);
		UMaterialExpressionScalarParameter* FoamCrestStart =
			Scal(TEXT("BF6FoamCrestStart"), 0.55f, 1730);
		UMaterialExpressionScalarParameter* FoamCrestFull =
			Scal(TEXT("BF6FoamCrestFull"), 0.90f, 1740);
		UMaterialExpressionVectorParameter* ExtendedCb1[22] = {};
		UMaterialExpressionScalarParameter* ExtendedCb1W[22] = {};
		for (int32 i = 0; i < 22; ++i)
		{
			ExtendedCb1[i] = Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, 1720 + i * 24));
			if (ExtendedCb1[i])
			{
				ExtendedCb1[i]->ParameterName = *FString::Printf(TEXT("BF6ExtendedCb1_%d"), i);
				ExtendedCb1[i]->DefaultValue = FLinearColor::Black;
			}
			ExtendedCb1W[i] = Scal(*FString::Printf(TEXT("BF6ExtendedCb1W_%d"), i),
				0.f, 1720 + i * 24);
		}
		UMaterialExpressionScalarParameter* ShoreSurfFallback =
			Scal(TEXT("UseShoreSurfFallback"), 0.f, 1660);
		// Tsuru's extended graph uses DIFFERENT world frequencies for its two
		// normal sheets. These are live depot parameters, not visual tuning:
		// micro 0xC5625D31, foam 0x1AFFA140, flow 0x9C172692.
		UMaterialExpressionScalarParameter* MicroSheetUv =
			Scal(TEXT("MicroSheetUvScale"), 0.0464f, 1550);
		UMaterialExpressionScalarParameter* FoamSheetUv =
			Scal(TEXT("FoamSheetUvScale"), 0.14f, 1570);
		UMaterialExpressionScalarParameter* MicroSheetFlow =
			Scal(TEXT("MicroSheetFlowSpeed"), 0.9f, 1590);
		UMaterialExpressionScalarParameter* FoamSheetN =
			Scal(TEXT("FoamSheetNormalStrength"), 0.35f, 1580);
		UMaterialExpressionVectorParameter* WaterMin =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, 1760));
		if (WaterMin) { WaterMin->ParameterName = TEXT("WaterMin"); WaterMin->DefaultValue = FLinearColor::Black; }
		UMaterialExpressionVectorParameter* WaterSpan =
			Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, 1820));
		if (WaterSpan) { WaterSpan->ParameterName = TEXT("WaterSpan"); WaterSpan->DefaultValue = FLinearColor(1,1,1,1); }
		UMaterialExpressionCameraPositionWS* CamP =
			Cast<UMaterialExpressionCameraPositionWS>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCameraPositionWS::StaticClass(), -900, 840));

		// THE DEPTH FADE IS THE SHADING MODEL'S OWN, not something bolted on.
		//
		// An earlier attempt multiplied both coefficients by saturate(d/D),
		// on the belief that Single Layer Water "cannot fade its own result".
		// That was wrong. The shading model integrates
		//     Transmittance = exp(-Extinction * WaterVolumeDepth)
		//     Scattering    = Albedo * (1 - Transmittance)
		// (SingleLayerWaterShading.ush:219-227), so at zero depth transmittance
		// is 1, scattering is 0, and the result is EXACTLY the background. The
		// fade the game gets from saturate(d/D) is already there, as an
		// exponential rather than a line, and it needs no help.
		//
		// Multiplying the coefficients by a second saturate(d/D) made the
		// optical depth QUADRATIC near shore - extinction * d^2 / D - which is
		// not what the game does and over-clears exactly the shallow water we
		// were trying to fix. It also drove extinction to zero at the
		// waterline, and Substrate's legacy conversion computes the albedo as
		// scattering / extinction with no guard (SubstrateLegacyConversion.ush:
		// 191), which is 0/0.
		//
		// The specular half has its own shore ramp in the composite,
		// saturate(DeltaDepth * 0.02) at SingleLayerWaterComposite.usf:249.
		if (SLW && Scatter)
			UMaterialEditingLibrary::ConnectMaterialExpressions(Scatter, TEXT(""), SLW, TEXT("ScatteringCoefficients"));
		if (SLW && Absorb)
			UMaterialEditingLibrary::ConnectMaterialExpressions(Absorb, TEXT(""), SLW, TEXT("AbsorptionCoefficients"));
		// HOW MUCH OF THE SCENE BEHIND THE WATER COMES THROUGH IT.
		//
		// 1 is the default and is right for the simulated sea, which has a sea
		// bed and a shoreline behind it. It is wrong for the far-world horizon
		// sheet: that plane has nothing behind it but the sky dome, whose
		// panorama clamps to its horizon row below the horizon and therefore
		// reads as a flat beige field. At the default the sheet transmits that
		// beige and disappears - it renders, but it may as well not.
		//
		// Real open ocean past the shelf returns nothing from below, so the
		// horizon sheet asks for 0 and keeps only its own backscatter, which is
		// the decoded albedo. Exposed as a parameter so the sea and the sheet can
		// disagree while sharing one material.
		if (SLW)
		{
			if (UMaterialExpressionScalarParameter* Behind =
				Scal(TEXT("BF6ColorScaleBehindWater"), 1.f, 1960))
			{
				UMaterialEditingLibrary::ConnectMaterialExpressions(
					Behind, TEXT(""), SLW, TEXT("ColorScaleBehindWater"));
			}
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("water: coefficients connected directly; the depth fade is the ")
			TEXT("shading model's own exp(-extinction * depth)"));

		auto WaveInputs = [&](UMaterialExpressionCustom* X,
		                      UMaterialExpressionWorldPosition* WP,
		                      UMaterialExpressionTime* Tm,
		                      UMaterialExpressionVectorParameter* Wv[8],
		                      UMaterialExpressionScalarParameter* Gn,
		                      UMaterialExpressionScalarParameter* Ch,
		                      UMaterialExpressionScalarParameter* Bl,
		                      UMaterialExpressionScalarParameter* Bl2)
		{
			X->Inputs.Empty();
			auto In = [&X](const TCHAR* Nm, UMaterialExpression* E)
			{
				FCustomInput I;
				I.InputName = Nm;
				I.Input.Expression = E;
				X->Inputs.Add(I);
			};
			In(TEXT("WPos"), WP);
			In(TEXT("T"), Tm);
			const TCHAR* Names[8] = { TEXT("W0"), TEXT("W1"), TEXT("W2"), TEXT("W3"),
			                          TEXT("W4"), TEXT("W5"), TEXT("W6"), TEXT("W7") };
			for (int32 i = 0; i < 8; i++) In(Names[i], Wv[i]);
			In(TEXT("Gain"), Gn);
			In(TEXT("Chop"), Ch);
			In(TEXT("BaseLen"), Bl);
			In(TEXT("MinLen"), Bl2);
			In(TEXT("Detail"), Det);
			In(TEXT("DetailStart"), DetStart);
			In(TEXT("DetailEnd"), DetEnd);
			In(TEXT("CamPos"), CamP);
			In(TEXT("ShoreD"), ShD);
			In(TEXT("AdditionalDepth"), AddD);
			In(TEXT("UseAuthoredShore"), UseSh);
			In(TEXT("ShoreBlend"), ShB);
			In(TEXT("ShoreSuppress"), ShF);
			In(TEXT("DepthMin"), DMin);
			In(TEXT("DepthSpan"), DSpan);
			In(TEXT("DepthTex"), DTex);
			In(TEXT("DepthAvailable"), DepthAvailable);
		};

		UMaterialExpressionWorldPosition* WP =
			Cast<UMaterialExpressionWorldPosition>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionWorldPosition::StaticClass(), -900, 700));
		UMaterialExpressionTime* Tm =
			Cast<UMaterialExpressionTime>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTime::StaticClass(), -900, 780));
		UMaterialExpressionVectorParameter* Wv[8] = {};
		for (int32 i = 0; i < 8; i++)
		{
			Wv[i] = Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -1150, 500 + 90 * i));
			if (Wv[i])
			{
				Wv[i]->ParameterName = *FString::Printf(TEXT("Wave%d"), i);
				// A default no-sea: zero amplitude, spread directions.
				Wv[i]->DefaultValue = FLinearColor(1.f, 0.f, 0.f, 1.f);
			}
		}
		UMaterialExpressionScalarParameter* Gn = Scal(TEXT("WaveGain"), 0.f, 860);
		UMaterialExpressionScalarParameter* Ch = Scal(TEXT("WaveChop"), 0.4f, 920);
		UMaterialExpressionScalarParameter* Bl = Scal(TEXT("WaveBaseLen"), 26.f, 980);
		// The game's own foam controls, straight off the recovered
		// WaterDiffConstants: threshold at +8, max at +12. Foam is the
		// LINEARISED JACOBIAN of the horizontal displacement, thresholded.
		UMaterialExpressionScalarParameter* FTh = Scal(TEXT("FoamThreshold"), 8.f, 1040);
		UMaterialExpressionScalarParameter* FMx = Scal(TEXT("FoamMax"), 1.f, 1100);
		// The shortest wavelength this water's GRID can represent, set per
		// surface from its vertex spacing.
		UMaterialExpressionScalarParameter* MnL = Scal(TEXT("MinDispLen"), 24.f, 1160);

		UMaterialExpressionCustom* WpoX =
			Cast<UMaterialExpressionCustom>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCustom::StaticClass(), -300, 760));
		UMaterialExpressionCustom* NrmX =
			Cast<UMaterialExpressionCustom>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCustom::StaticClass(), -300, 980));
		if (WpoX && NrmX && WP && Tm && Gn && Ch && Bl && MnL && Det && CamP &&
			DetStart && DetEnd && ShD && AddD && UseSh && ShB && ShF && DMin && DSpan && DTex &&
			DepthAvailable &&
			Wv[0] && Wv[1] && Wv[2] && Wv[3] && Wv[4] && Wv[5] && Wv[6] && Wv[7])
		{
			WpoX->Code = TEXT("float3 w[8] = {W0,W1,W2,W3,W4,W5,W6,W7};\nconst float ratio[8] = {1.0, 0.618, 0.382, 0.236, 0.146, 0.090, 0.056, 0.034};\nfloat2 pm = WPos.xy * 0.01;\n// SHORE FADE, the way the game does it: the wave amplitude is scaled\n// down by how shallow the water is. Recovered form is\n//   depth = waterY - terrainY;  t = saturate(depth / D);\n//   fade  = saturate(cubic(t))\n// with the cubic authored per level - a plain smoothstep in the\n// default case. Without it the swell runs straight into the beach at\n// full height, which is what makes a repeating pattern so obvious near\n// land.\nfloat2 duv = (WPos.xy * 0.01 - DepthMin) / DepthSpan;\nfloat terrainY = Texture2DSample(DepthTex, DepthTexSampler, saturate(duv)).r;\nfloat depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep\nfloat3 disp = float3(0,0,0);\nfor (int i = 0; i < 8; i++) {\n  float len = max(ratio[i] * BaseLen, 0.5);\n  if (len < MinLen) continue;\n  float2 d = normalize(w[i].xy + float2(1e-5, 0));\n  float A = w[i].z * Gain * shore;\n  float k = 6.2831853 / len;\n  float ph = k * dot(d, pm) - sqrt(9.81 * k) * T;\n  disp.xy += d * (Chop * A) * cos(ph);\n  disp.z  += A * sin(ph);\n}\nreturn disp * 100.0;");
			WpoX->Code.ReplaceInline(
				TEXT("float depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep\nfloat3 disp"),
				TEXT("float depth = (WPos.z * 0.01) - terrainY + AdditionalDepth;\nfloat shoreOn = step(1e-4, ShoreD);\nfloat st = saturate(depth / max(ShoreD, 1e-4));\nfloat authoredShore = saturate(dot(float4(st*st*st, st*st, st, 1.0), ShoreBlend));\nfloat fallbackShore = st * st * (3.0 - 2.0 * st);\nfloat shore = lerp(1.0, lerp(fallbackShore, authoredShore, UseAuthoredShore), DepthAvailable * shoreOn);\nfloat dist = length(WPos - CamPos) * 0.01;\nfloat3 disp"));
			WpoX->Code.ReplaceInline(
				TEXT("float A = w[i].z * Gain * shore;"),
				TEXT("float lodFade = saturate((len * 24.0 - dist) / max(len * 12.0, 1.0));\n  float A = w[i].z * Gain * shore * lodFade;"));
			WpoX->OutputType = CMOT_Float3;
			WpoX->Description = TEXT("BF6 Gerstner displacement");
			WaveInputs(WpoX, WP, Tm, Wv, Gn, Ch, Bl, MnL);
			UMaterialEditingLibrary::ConnectMaterialProperty(WpoX, TEXT(""), MP_WorldPositionOffset);

			NrmX->Code = TEXT("float3 w[8] = {W0,W1,W2,W3,W4,W5,W6,W7};\nconst float ratio[8] = {1.0, 0.618, 0.382, 0.236, 0.146, 0.090, 0.056, 0.034};\nfloat2 pm = WPos.xy * 0.01;\n// SHORE FADE, the way the game does it: the wave amplitude is scaled\n// down by how shallow the water is. Recovered form is\n//   depth = waterY - terrainY;  t = saturate(depth / D);\n//   fade  = saturate(cubic(t))\n// with the cubic authored per level - a plain smoothstep in the\n// default case. Without it the swell runs straight into the beach at\n// full height, which is what makes a repeating pattern so obvious near\n// land.\nfloat2 duv = (WPos.xy * 0.01 - DepthMin) / DepthSpan;\nfloat terrainY = Texture2DSample(DepthTex, DepthTexSampler, saturate(duv)).r;\nfloat depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep\nfloat dist = length(WPos - CamPos) * 0.01;\nfloat3 n = float3(0,0,1);\nfor (int i = 0; i < 8; i++) {\n  float len = max(ratio[i] * BaseLen, 0.5);\n  float2 d = normalize(w[i].xy + float2(1e-5, 0));\n  float A = w[i].z * Gain * shore;\n  float k = 6.2831853 / len;\n  float ph = k * dot(d, pm) - sqrt(9.81 * k) * T;\n  float wa = k * A;\n  n.xy -= d * wa * cos(ph);\n  n.z  -= Chop * wa * sin(ph);\n}\n// Ripples, six octaves from about four metres down to fifteen\n// centimetres, each faded out as it stops being resolvable. These do\n// NOT take the shore fade: a beach still has ripples on it.\nfloat2 dirA = normalize(w[0].xy + float2(1e-5, 0));\nfloat2 dirB = float2(-dirA.y, dirA.x);\nfloat rlen = 4.0;\nfloat ramp = Detail * 0.06;\nfor (int j = 0; j < 6; j++) {\n  float fade = saturate(1.0 - dist / (rlen * 90.0));\n  if (fade > 0.001) {\n    float k2 = 6.2831853 / rlen;\n    float sp = sqrt(9.81 * k2);\n    float2 dd = normalize(dirA * (1.0 + 0.7 * float(j % 3)) + dirB * (0.5 - 0.35 * float(j % 2)));\n    float p2 = k2 * dot(dd, pm) - sp * T * 1.15;\n    n.xy -= dd * (k2 * ramp * fade) * cos(p2);\n  }\n  rlen *= 0.55;\n  ramp *= 0.72;\n}\nreturn normalize(n);");
			NrmX->Code.ReplaceInline(
				TEXT("float depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep"),
				TEXT("float depth = (WPos.z * 0.01) - terrainY + AdditionalDepth;\nfloat shoreOn = step(1e-4, ShoreD);\nfloat st = saturate(depth / max(ShoreD, 1e-4));\nfloat authoredShore = saturate(dot(float4(st*st*st, st*st, st, 1.0), ShoreBlend));\nfloat fallbackShore = st * st * (3.0 - 2.0 * st);\nfloat shore = lerp(1.0, lerp(fallbackShore, authoredShore, UseAuthoredShore), DepthAvailable * shoreOn);"));
			NrmX->Code.ReplaceInline(
				TEXT("float A = w[i].z * Gain * shore;"),
				TEXT("float lodFade = saturate((len * 24.0 - dist) / max(len * 12.0, 1.0));\n  float A = w[i].z * Gain * shore * lodFade;"));
			NrmX->Code.ReplaceInline(
				TEXT("float ramp = Detail * 0.06;"),
				TEXT("float globalDetailFade = 1.0 - saturate((dist - DetailStart) / max(DetailEnd - DetailStart, 1.0));\nfloat ramp = Detail * 0.06 * globalDetailFade;"));
			NrmX->Code.ReplaceInline(
				TEXT("return normalize(n);"),
				TEXT("// The shipped draw-detail sheets, sampled twice with the recovered\n"
				     "// triangle flow blend so the repeating reset never pops. They are\n"
				     "// continuous world-space samples, independent of clipmap tile edges.\n"
				     "float flow = frac(T * MicroFlowSpeed * 0.01);\n"
				     "float flowW = 1.0 - abs(2.0 * flow - 1.0);\n"
				     "float2 suvA = pm * MicroUvScale + float2(flow, flow * 0.63);\n"
				     "float2 suvB = pm * MicroUvScale + float2(flow - 1.0, (flow - 1.0) * 0.63);\n"
				     "float4 micro = lerp(Texture2DSample(MicroSheet, MicroSheetSampler, suvA),\n"
				     "                    Texture2DSample(MicroSheet, MicroSheetSampler, suvB), flowW);\n"
				     "float4 foamS = lerp(Texture2DSample(FoamSheet, FoamSheetSampler, suvA),\n"
				     "                    Texture2DSample(FoamSheet, FoamSheetSampler, suvB), flowW);\n"
				     "float sheetFoam = saturate((0.5 - micro.w) + foamS.w);\n"
				     "float2 sheetN = lerp(Detail * (micro.xy - 0.5),\n"
				     "                     FoamSheetStrength * (foamS.xy - 0.5), sheetFoam);\n"
				     "n.xy += UseSheets * 2.0 * globalDetailFade * sheetN;\n"
				     "return normalize(n);"));
			NrmX->OutputType = CMOT_Float3;
			NrmX->Description = TEXT("BF6 Gerstner normal");
			WaveInputs(NrmX, WP, Tm, Wv, Gn, Ch, Bl, MnL);
			auto NrmInput = [&NrmX](const TCHAR* Name, UMaterialExpression* E)
			{
				FCustomInput I; I.InputName = Name; I.Input.Expression = E;
				NrmX->Inputs.Add(I);
			};
			NrmInput(TEXT("UseSheets"), UseSheets);
			NrmInput(TEXT("MicroUvScale"), MicroSheetUv);
			NrmInput(TEXT("MicroFlowSpeed"), MicroSheetFlow);
			NrmInput(TEXT("MicroSheet"), MicroSheet);
			NrmInput(TEXT("FoamSheet"), FoamSheet);
			NrmInput(TEXT("FoamSheetStrength"), FoamSheetN);
			UMaterialEditingLibrary::ConnectMaterialProperty(NrmX, TEXT(""), MP_Normal);
			// The node writes a WORLD normal; the plane's tangent frame is not
			// part of the math.
			M->bTangentSpaceNormal = false;
		}

		// ---- THE SHIPPED FOUR-CASCADE FFT FIELD --------------------------
		//
		// Keep the legacy Gerstner expressions above as an unconnected audit
		// reference for one release, but override both material outputs here.
		// The textures are produced from the mounted game's H0 on every map
		// build. No exported raster or fitted wave survives this connection.
		UMaterialExpressionTextureObjectParameter* FFTDisp[4] = {};
		UMaterialExpressionTextureObjectParameter* FFTNorm[4] = {};
		UMaterialExpressionVectorParameter* FFTMeta[4] = {};
		for (int32 i = 0; i < 4; ++i)
		{
			FFTDisp[i] = Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -1550, 1900 + i * 70));
			FFTNorm[i] = Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -1300, 1900 + i * 70));
			FFTMeta[i] = Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -1050, 1900 + i * 70));
			if (FFTDisp[i])
			{
				FFTDisp[i]->ParameterName = *FString::Printf(TEXT("BF6Disp%d"), i);
				FFTDisp[i]->SamplerType = SAMPLERTYPE_LinearColor;
				FFTDisp[i]->Texture = LinearWhite();
			}
			if (FFTNorm[i])
			{
				FFTNorm[i]->ParameterName = *FString::Printf(TEXT("BF6NormalFoam%d"), i);
				FFTNorm[i]->SamplerType = SAMPLERTYPE_LinearColor;
				FFTNorm[i]->Texture = LinearWhite();
			}
			if (FFTMeta[i])
			{
				FFTMeta[i]->ParameterName = *FString::Printf(TEXT("BF6Cascade%d"), i);
				FFTMeta[i]->DefaultValue = FLinearColor(1.f, 0.f, 0.f, 0.f);
			}
		}
		UMaterialExpressionScalarParameter* FFTEnabled = Scal(TEXT("BF6FFTEnabled"), 0.f, 1900);
		// WAVE MASK POLARITY, 0 = attenuation is (1 - mask), 1 = attenuation is mask.
		//
		// Mapping the whole authored mask (scratchpad/mask_map.cpp) shows 86.6% of
		// the level reading mask~1 and a small OPEN blob in the middle - and the
		// blob is where the island and its lagoon are, not where the sea is. Under
		// the current (1 - mask) law that suppresses the open ocean and allows
		// waves in the shallows, which is exactly backwards and matches the
		// observed mirror-flat sea at (-2401, -26) where the mask reads 1.000 in
		// every direction for 750 m.
		//
		// The existing law is NOT baseless - coarsemask-attenuation-is-the-water-
		// wave-mask reports a spatial mean that is "zero across the open sea" - so
		// rather than silently flip it, make it switchable and let a live A/B at a
		// known-open position decide.
		UMaterialExpressionScalarParameter* MaskPolarity =
			Scal(TEXT("BF6MaskPolarity"), 0.f, 1905);
		// Texel size in metres per cascade, so the shader can convert a tile's
		// vertex spacing into a mip index. Filled by FWaterFFT::Bind from each
		// cascade's own TileM/N.
		// THREE in a vector and the fourth on its own, because Unreal hands a
		// Vector Parameter to a Custom node as float3: the .w silently does not
		// arrive, and asking for it fails the whole material compile, which then
		// renders as engine-default rather than as anything diagnosable.
		UMaterialExpressionVectorParameter* FFTTexelM =
			Vec(TEXT("BF6CascadeTexelM"), FLinearColor(1.f, 1.f, 1.f, 1.f), 1910);
		UMaterialExpressionScalarParameter* FFTTexelM3 =
			Scal(TEXT("BF6CascadeTexelM3"), 1.f, 1915);
		// The tile's width in metres, written per instance by the draw tree.
		// A non-instanced water body (the pool) has no custom data and takes the
		// default, which is the finest tile the tree ever emits.
		UMaterialExpressionPerInstanceCustomData* FFTTileM =
			NewObject<UMaterialExpressionPerInstanceCustomData>(M);
		if (FFTTileM)
		{
			FFTTileM->DataIndex = 0;
			FFTTileM->ConstDefaultValue = 16.f;
			M->GetExpressionCollection().AddExpression(FFTTileM);
		}
		UMaterialExpressionScalarParameter* FFTAmplitude = Scal(TEXT("BF6WaveAmplitudeScale"), 1.f, 1940);
		UMaterialExpressionScalarParameter* InteractiveBridge =
			Scal(TEXT("BF6InteractiveBridgeEnabled"), 0.f, 1960);
		UMaterialExpressionScalarParameter* InteractiveHeight =
			Scal(TEXT("BF6InteractiveBridgeHeightM"), 4.f, 1970);
		UMaterialExpressionScalarParameter* InteractiveLength =
			Scal(TEXT("BF6InteractiveBridgeLengthM"), 120.f, 1980);
		UMaterialExpressionScalarParameter* InteractiveRate =
			Scal(TEXT("BF6InteractiveBridgeRate"), 0.35f, 1990);
		// Cascade UVs are relative to Serac:WaterCascadeParams.WaterOrigin.
		// For the shipped rectangular surface this is the top-face XZ centre
		// (plus TileOffset, which is zero on every measured surface), not world
		// zero. Sampling from absolute Unreal world coordinates preserves the
		// wavelength but changes the deterministic phase and cannot match the
		// selected Frostbite draw shader at a known camera position.
		UMaterialExpressionVectorParameter* FFTOrigin =
			Vec(TEXT("BF6WaterOrigin"), FLinearColor::Black, 1980);
		UMaterialExpressionScalarParameter* FFTOverlapEnabled =
			Scal(TEXT("BF6CascadeOverlapEnabled"), 0.f, 2020);
		UMaterialExpressionVectorParameter* FFTOverlapParams =
			Vec(TEXT("BF6CascadeOverlapParams"), FLinearColor(0.f, 1.f, 0.f, 0.f), 2060);
		UMaterialExpressionVectorParameter* FFTOverlapParams2 =
			Vec(TEXT("BF6CascadeOverlapParams2"), FLinearColor(1.f, 1.f, 1.f, 1.f), 2100);
		UMaterialExpressionScalarParameter* FFTOverlapHeight =
			Scal(TEXT("BF6CascadeOverlapHeightScale"), 1.f, 2140);
		// A VectorParameter custom input is float3 in several Unreal material
		// permutations. Keep each recovered fourth component explicit instead of
		// relying on a .w swizzle that compiles in one permutation and fails later.
		UMaterialExpressionScalarParameter* FFTOverlapShearY =
			Scal(TEXT("BF6CascadeOverlapShearY"), 0.f, 2180);
		UMaterialExpressionScalarParameter* FFTOverlapSizeB =
			Scal(TEXT("BF6CascadeOverlapSizeB"), 1.f, 2220);
		// The complete MP_Isolated foam expression is authored later in this
		// graph because Opacity and Roughness consume it too. Keep the FFT normal
		// node so that exact same coverage value can be connected after FoamOut
		// exists. Previously the normal node recomputed a smaller contact/sheet
		// approximation while BaseColor displayed the full broad graph: the user
		// saw a white foam decal with no corresponding wave-normal relief.
		UMaterialExpressionCustom* FFTNormalForFoam = nullptr;
		if (WP && FFTEnabled && FFTAmplitude && FFTDisp[0] && FFTDisp[1] &&
			FFTDisp[2] && FFTDisp[3] && FFTNorm[0] && FFTNorm[1] && FFTNorm[2] && FFTNorm[3] &&
			FFTMeta[0] && FFTMeta[1] && FFTMeta[2] && FFTMeta[3] && FFTOrigin &&
			FFTOverlapEnabled && FFTOverlapParams && FFTOverlapParams2 && FFTOverlapHeight &&
			FFTOverlapShearY && FFTOverlapSizeB && InteractiveBridge && InteractiveHeight &&
			InteractiveLength && InteractiveRate && ShD && AddD && UseSh && ShB &&
			DMin && DSpan && DTex && DepthAvailable && WaterHeightAvailable &&
			WaterHeightTex && WaterHeightMin && WaterHeightSpan && WaterHeightTexel &&
			WaterSourceWorld && WaterMaskAvailable && WaterMaskAtlas &&
			WaterMaskIndirection && WaterMaskMin && WaterMaskSpan && WaterMaskMeta &&
			WaterMaskIndirectionSide)
		{
				auto AddFFTInputs = [&](UMaterialExpressionCustom* X, bool bNormals)
			{
				X->Inputs.Empty();
				auto In = [&X](const TCHAR* Name, UMaterialExpression* E)
				{
					FCustomInput I; I.InputName = Name; I.Input.Expression = E; X->Inputs.Add(I);
				};
				In(TEXT("WPos"), WP);
				In(TEXT("Enabled"), FFTEnabled);
				In(TEXT("Amplitude"), FFTAmplitude);
				In(TEXT("WaterOrigin"), FFTOrigin);
				In(TEXT("OverlapEnabled"), FFTOverlapEnabled);
				In(TEXT("OverlapParams"), FFTOverlapParams);
				In(TEXT("OverlapParams2"), FFTOverlapParams2);
				In(TEXT("OverlapHeight"), FFTOverlapHeight);
				In(TEXT("OverlapShearY"), FFTOverlapShearY);
				In(TEXT("OverlapSizeB"), FFTOverlapSizeB);
				In(TEXT("InteractiveEnabled"), InteractiveBridge);
				In(TEXT("InteractiveHeight"), InteractiveHeight);
				In(TEXT("InteractiveLength"), InteractiveLength);
				In(TEXT("InteractiveRate"), InteractiveRate);
				In(TEXT("T"), Tm);
				In(TEXT("TexelM"), FFTTexelM);
				In(TEXT("TexelM3"), FFTTexelM3);
				In(TEXT("MaskPolarity"), MaskPolarity);
				In(TEXT("TileM"), FFTTileM);
				for (int32 i = 0; i < 4; ++i)
				{
					const TCHAR* Prefix = bNormals ? TEXT("N") : TEXT("D");
					In(*FString::Printf(TEXT("%s%d"), Prefix, i),
						bNormals ? (UMaterialExpression*)FFTNorm[i] : (UMaterialExpression*)FFTDisp[i]);
					In(*FString::Printf(TEXT("C%d"), i), FFTMeta[i]);
				}
				In(TEXT("ShoreD"), ShD);
				In(TEXT("AdditionalDepth"), AddD);
				In(TEXT("UseAuthoredShore"), UseSh);
				In(TEXT("ShoreBlend"), ShB);
				In(TEXT("DepthMin"), DMin);
				In(TEXT("DepthSpan"), DSpan);
				In(TEXT("DepthTex"), DTex);
				In(TEXT("DepthAvailable"), DepthAvailable);
				In(TEXT("WaterHeightAvailable"), WaterHeightAvailable);
				In(TEXT("WaterHeightTex"), WaterHeightTex);
				In(TEXT("WaterHeightMin"), WaterHeightMin);
				In(TEXT("WaterHeightSpan"), WaterHeightSpan);
				In(TEXT("WaterHeightTexelM"), WaterHeightTexel);
				In(TEXT("WaterSourceWorld"), WaterSourceWorld);
				In(TEXT("MaskAvailable"), WaterMaskAvailable);
				In(TEXT("MaskAtlas"), WaterMaskAtlas);
				In(TEXT("MaskIndirection"), WaterMaskIndirection);
				In(TEXT("MaskMin"), WaterMaskMin);
				In(TEXT("MaskSpan"), WaterMaskSpan);
				In(TEXT("MaskMeta"), WaterMaskMeta);
				In(TEXT("MaskIndirectionSide"), WaterMaskIndirectionSide);
				// The 16 wave-entity slots ride both FFT nodes; only the WPO
				// consumes them (the recovered contract is displacement-side).
				// Both arrays must be complete: the HLSL initialises fixed-size
				// arrays from these names, so a single missing input is a
				// material compile failure and a flat grey surface.
				for (int32 iWe = 0; iWe < 16; ++iWe)
				{
					In(*FString::Printf(TEXT("WE%d"), iWe), WaveEnt[iWe]);
					In(*FString::Printf(TEXT("WA%d"), iWe), WaveAmp[iWe]);
				}
					In(TEXT("DetailStart"), DetStart);
					In(TEXT("DetailEnd"), DetEnd);
					In(TEXT("CamPos"), CamP);
					if (bNormals)
					{
						// The FFT node replaces the legacy normal output, so it must
						// also carry the game's bound draw-detail sheets. Omitting this
						// branch left Tsuru with only millimetre-scale H0 normals.
						In(TEXT("Detail"), Det);
						In(TEXT("UseSheets"), UseSheets);
						In(TEXT("MicroUvScale"), MicroSheetUv);
						In(TEXT("FoamUvScale"), FoamSheetUv);
						In(TEXT("MicroFlowSpeed"), MicroSheetFlow);
						In(TEXT("MicroSheet"), MicroSheet);
						In(TEXT("FoamSheet"), FoamSheet);
						In(TEXT("FoamSheetStrength"), FoamSheetN);
						In(TEXT("UseContact"), UseContact);
						In(TEXT("ContactFoam"), ContactFoam);
						In(TEXT("ContactDivisor"), ContactDiv);
						In(TEXT("ContactRemapLow"), ContactLow);
						In(TEXT("ContactGain"), ContactGain);
						In(TEXT("CompositeLow"), FoamCompositeLow);
						In(TEXT("CompositeHigh"), FoamCompositeHigh);
					}
				};

			UMaterialExpressionCustom* FFTWpo = Cast<UMaterialExpressionCustom>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCustom::StaticClass(), -650, 1940));
			UMaterialExpressionCustom* FFTNormal = Cast<UMaterialExpressionCustom>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCustom::StaticClass(), -350, 1940));
			if (FFTWpo && FFTNormal)
			{
				FFTNormalForFoam = FFTNormal;
				FFTWpo->Code = TEXT(
					"float2 worldM=WPos.xy*0.01; float2 sourceWorldM=worldM+WaterSourceWorld.xy; float2 pm=worldM-WaterOrigin.xy; float2 q=(sourceWorldM-DepthMin)/DepthSpan;\n"
					"float2 hq=(sourceWorldM-WaterHeightMin.xy)/WaterHeightSpan.xy; float hinside=step(0,hq.x)*step(hq.x,1)*step(0,hq.y)*step(hq.y,1);\n"
					"float rawSurfaceY=Texture2DSampleLevel(WaterHeightTex,WaterHeightTexSampler,saturate(hq),0).r;\n"
					"float surfaceLift=WaterHeightAvailable*hinside*max(rawSurfaceY-WaterSourceWorld.z,0);\n"
					"float terrain=Texture2DSampleLevel(DepthTex,DepthTexSampler,saturate(q),0).r;\n"
					// THE GATE IS THE GAME'S: the shipped VS tests ShoreDepth > 0
					// before ever dividing, so an authored 0 means shore
					// attenuation deliberately OFF (mp_tungsten ships exactly
					// that; finding shoredepth-zero-is-a-shader-gate). The old
					// max(ShoreD,0.5) floor manufactured a half-metre fade the
					// game turned off, killing waves at the waterline.
					"float shoreOn=step(1e-4,ShoreD);\n"
					"float dep=WPos.z*0.01-terrain+AdditionalDepth; float t=saturate(dep/max(ShoreD,1e-4));\n"
					"float authored=saturate(dot(float4(t*t*t,t*t,t,1),ShoreBlend));\n"
					"float shore=lerp(1.0,lerp(t*t*(3-2*t),authored,UseAuthoredShore),DepthAvailable*shoreOn);\n"
					"float2 maskUv=min(saturate((sourceWorldM-MaskMin.xy)/max(MaskSpan.xy,float2(1e-4,1e-4))),float2(0.999999,0.999999));\n"
					"float2 maskCell=floor(maskUv*MaskIndirectionSide); float2 maskCellUv=(maskCell+0.5)/MaskIndirectionSide;\n"
					"float4 mi=Texture2DSampleLevel(MaskIndirection,MaskIndirectionSampler,maskCellUv,0);\n"
					"float maskPage=floor(mi.r*255.0+0.5); float maskShift=floor(mi.g*255.0+0.5);\n"
					"float inlineValue=floor(mi.b*255.0+0.5)/255.0; float inlineFlag=step(0.5,mi.a);\n"
					"float maskScale=MaskIndirectionSide/exp2(maskShift); float2 maskLocal=frac(maskUv*maskScale);\n"
					"float2 maskAtlasUv=(maskLocal*MaskMeta.x+MaskMeta.y+0.5)/MaskMeta.z;\n"
					"float pageValue=Texture2DArraySampleLevel(MaskAtlas,MaskAtlasSampler,float3(maskAtlasUv,maskPage),0).r;\n"
					"float maskValue=lerp(pageValue,inlineValue,inlineFlag);\n"
					"float maskLaw=saturate(lerp(1.0-maskValue,maskValue,MaskPolarity));\n"
					"float coarseAttenuation=lerp(shore,maskLaw,MaskAvailable);\n"
					"// MIP BY WHAT THIS TILE CAN REPRESENT.\n"
					"//\n"
					"// The shared patch is 16x16 quads at any tile width, so vertex\n"
					"// spacing is TileM/16 and ranges from 1 m to over 2 km. Sampling\n"
					"// the full-rate field on a coarse tile aliases cascade 0's ~100 m\n"
					"// waves into thin slivers. Asking for the mip whose texels are no\n"
					"// finer than the spacing keeps the long swell the tile CAN carry\n"
					"// and drops only what it cannot, which is what the game's own\n"
					"// vertex mip ramp achieves.\n"
					"// CONTINUOUS ACROSS TILES. The mip used to come from THIS tile's\n"
					"// vertex spacing, so two neighbouring tiles at different depths read\n"
					"// different mips of the same displacement field and met with a step\n"
					"// in height along their shared edge - the long ridges running to the\n"
					"// horizon. The game's vertex shader takes its mip from the camera\n"
					"// DISTANCE (findings/water-draw-shaders-decoded 3: a doubly\n"
					"// logarithmic ramp in log2(d/d0) whose constants CB0[5].xy/CB0[6].xy/\n"
					"// CB0[32].w libbf6 does not yet expose). Until they are read, the\n"
					"// continuous quantity the tree already reasons about is used: the\n"
					"// projected footprint of one screen pixel at this vertex's distance,\n"
					"// which is what the per-tile spacing was standing in for. It is a\n"
					"// function of position only, so shared edges agree exactly.\n"
					"float dcamM=max(length(CamPos-WPos)*0.01,1.0);\n"
					"float pixelAngle=2.0/max(View.ViewToClip[0][0]*View.ViewSizeAndInvSize.x,1e-4);\n"
					"float footprintM=dcamM*pixelAngle;\n"
					"float3 mip012=log2(max(footprintM/max(TexelM,1e-4),1.0));\n"
					"float mip3=log2(max(footprintM/max(TexelM3,1e-4),1.0));\n"
					"float4 d0=Texture2DSampleLevel(D0,D0Sampler,pm/max(C0.x,1e-4),mip012.x)*C0.z;\n"
					"float4 d1=Texture2DSampleLevel(D1,D1Sampler,pm/max(C1.x,1e-4),mip012.y)*C1.z;\n"
					"float4 d2=Texture2DSampleLevel(D2,D2Sampler,pm/max(C2.x,1e-4),mip012.z)*C2.z;\n"
					"float4 d3=Texture2DSampleLevel(D3,D3Sampler,pm/max(C3.x,1e-4),mip3)*C3.z;\n"
					"// Exact selected-vertex overlap: a second, anisotropic cascade-0 sample.\n"
					"float p0=OverlapParams.y*pm.x+OverlapParams.x*pm.y;\n"
					"float p1=OverlapParams.y*pm.y-OverlapParams.x*pm.x;\n"
					"p1-=OverlapShearY*p0; p0-=OverlapParams.z*p1;\n"
					"float2 ouv=float2(OverlapParams2.x*p0,OverlapParams2.y*p1)/max(C0.x,1e-4);\n"
					"float4 od=Texture2DSampleLevel(D0,D0Sampler,ouv,mip012.x)*C0.z;\n"
					"float ox=OverlapParams2.z*(-C0.y*od.x),oz=OverlapSizeB*(-C0.y*od.z);\n"
					"float tx=ox+OverlapParams.z*oz; float tz=oz+OverlapShearY*tx;\n"
					"float2 oh=float2(OverlapParams.y*tx-OverlapParams.x*tz,OverlapParams.x*tx+OverlapParams.y*tz);\n"
					"float2 h=-coarseAttenuation*(C0.y*d0.xz+C1.y*d1.xz)-C2.y*d2.xz-C3.y*d3.xz;\n"
					"float v=Amplitude*(coarseAttenuation*(d0.y+d1.y)+d2.y+d3.y);\n"
					"h+=coarseAttenuation*OverlapEnabled*oh; v+=coarseAttenuation*OverlapEnabled*Amplitude*OverlapHeight*od.y;\n"
					// WAVE ENTITIES, the game's own radial swells: per slot,
					//   t = min(|posXZ - centre| * recipRadius, 1)
					//   mound += HalfAmplitude * (cos(pi*t) + 1)   2x HalfAmp at centre
					// and the running t-product DAMPENS the cascade displacement
					// inside each entity, while the mound itself is added RAW -
					// it bypasses the shore fade and the wave mask, exactly as
					// the shipped VS does (finding water-wave-entities-render-
					// contract-and-negative-census). Disabled slots carry the
					// game's own sentinel recipRadius = 1e10 and contribute
					// nothing through this same arithmetic.
					// A vector parameter arrives at a custom node as float3: the
					// alpha is dropped. Packing (x, y, recipRadius, halfAmp) into
					// one of them therefore does not compile, and would have
					// discarded the amplitude even if it had. Position and radius
					// ride the vector; the amplitude is its own scalar.
					"float3 wp[16]={WE0,WE1,WE2,WE3,WE4,WE5,WE6,WE7,WE8,WE9,WE10,WE11,WE12,WE13,WE14,WE15};\n"
					"float wa[16]={WA0,WA1,WA2,WA3,WA4,WA5,WA6,WA7,WA8,WA9,WA10,WA11,WA12,WA13,WA14,WA15};\n"
					"float weMound=0.0; float weProd=1.0;\n"
					"for(int wi=0;wi<16;wi++){float2 wd=sourceWorldM-wp[wi].xy;"
					"float wt=min(sqrt(dot(wd,wd)+1e-8)*wp[wi].z,1.0);"
					"weMound+=wa[wi]*(cos(3.14159265*wt)+1.0);weProd*=wt;}\n"
					"return float3(Enabled*weProd*h.x,Enabled*weProd*h.y,Enabled*weProd*v+weMound+surfaceLift)*100.0;");
				FFTWpo->OutputType = CMOT_Float3;
				FFTWpo->Description = TEXT("BF6 four-cascade FFT displacement");
				AddFFTInputs(FFTWpo, false);
				// Retained as a source-level negative control for old MCP clients.  The
				// selected BF6 VS has no broad-pattern/noise bindings, so none of these
				// pixel-stage inputs may contribute to the returned vertex position.
				auto AddBroadWpoInput = [&FFTWpo](const TCHAR* Name, UMaterialExpression* E)
				{
					FCustomInput I; I.InputName = Name; I.Input.Expression = E;
					FFTWpo->Inputs.Add(I);
				};
				AddBroadWpoInput(TEXT("FoamWaveHeight"), FoamWaveHeight);
				AddBroadWpoInput(TEXT("BroadWaveSize"), BroadCarrierSize);
				AddBroadWpoInput(TEXT("BroadTimeScale"), BroadTimeScale);
				AddBroadWpoInput(TEXT("UseSheets"), UseSheets);
				AddBroadWpoInput(TEXT("UseBroad"), UseBroadPattern);
				AddBroadWpoInput(TEXT("BroadPattern"), BroadPattern);
				AddBroadWpoInput(TEXT("NoiseTex"), OceanNoise);
				AddBroadWpoInput(TEXT("GraphVersion"), ExtendedGraphVersion);
				AddBroadWpoInput(TEXT("MicroUvScale"), MicroSheetUv);
				AddBroadWpoInput(TEXT("FoamUvScale"), FoamSheetUv);
				AddBroadWpoInput(TEXT("MicroFlowSpeed"), MicroSheetFlow);
				AddBroadWpoInput(TEXT("MicroSheet"), MicroSheet);
				AddBroadWpoInput(TEXT("FoamSheet"), FoamSheet);
				AddBroadWpoInput(TEXT("UseContact"), UseContact);
				AddBroadWpoInput(TEXT("ContactFoam"), ContactFoam);
				AddBroadWpoInput(TEXT("ContactDivisor"), ContactDiv);
				AddBroadWpoInput(TEXT("ContactRemapLow"), ContactLow);
				AddBroadWpoInput(TEXT("ContactGain"), ContactGain);
				AddBroadWpoInput(TEXT("CompositeLow"), FoamCompositeLow);
				AddBroadWpoInput(TEXT("CompositeHigh"), FoamCompositeHigh);
				for (int32 i = 0; i < 22; ++i)
				{
					AddBroadWpoInput(*FString::Printf(TEXT("G%d"), i), ExtendedCb1[i]);
					AddBroadWpoInput(*FString::Printf(TEXT("G%dw"), i), ExtendedCb1W[i]);
				}
				FFTWpo->Code.ReplaceInline(
					TEXT("h+=coarseAttenuation*OverlapEnabled*oh; v+=coarseAttenuation*OverlapEnabled*Amplitude*OverlapHeight*od.y;\n"
					     "return float3(Enabled*h.x,Enabled*h.y,Enabled*v+surfaceLift)*100.0;"),
					TEXT("h+=coarseAttenuation*OverlapEnabled*oh; v+=coarseAttenuation*OverlapEnabled*Amplitude*OverlapHeight*od.y;\n"
					     "// Vertex-safe replay of MP_Isolated's broad draw coverage.\n"
					     "float foamCoupling=saturate(UseBroad*GraphVersion);\n"
					     "float carrierSize=max(BroadWaveSize,1.0);\n"
					     "float broadT=T*BroadTimeScale;\n"
					     "float detailT=T*0.05;\n"
					     "float2 broadBase=sourceWorldM*G13w;\n"
					     "float2 broadDir=normalize(float2(OverlapParams.y,OverlapParams.x)+float2(1e-5,0));\n"
					     "float primaryFreq=max(abs(G13w*G15.y*0.000584)/carrierSize,1e-6);\n"
					     "float noiseFreq=max(abs(G13w*0.033),1e-6);\n"
					     "float primaryRate=sqrt(9.81*primaryFreq/6.2831853);\n"
					     "float noiseRate=sqrt(9.81*noiseFreq/6.2831853);\n"
					     "float broadMip=clamp(log2(carrierSize),0.0,5.0);\n"
					     "// TextureFor uploads the complete authored BF6 mip chain. Vertex-stage\n"
					     "// sampling has no screen derivatives, so an explicit mip is mandatory;\n"
					     "// forcing mip 0 promoted the source's small foam cells into four-metre\n"
					     "// geometry and produced the black point field in the control capture.\n"
					     "// BroadWaveSize is the wavelength multiplier for the geometry bridge.\n"
					     "// The prior no-UV-scale claim is retracted: the same-camera mip control\n"
					     "// removed the black cells but exposed many narrow parallel crests, while\n"
					     "// the BF6 oracle shows only a few broad groups across this bay. Scale k\n"
					     "// and its deep-water dispersion rate together so larger waves also travel\n"
					     "// more slowly. BroadTimeScale remains an independent live multiplier.\n"
					     "// The old roughly one-texel cross filter preserved the source's round\n"
					     "// noise cells as scalloped geometry.  Smooth mainly ALONG the recovered\n"
					     "// crest tangent, while retaining the gradient in the travel direction.\n"
					     "float broadBlurRadius=0.0030*(1.0+broadMip);\n"
					     "float2 broadTangent=float2(-broadDir.y,broadDir.x);\n"
					     "float2 broadBlurX=broadTangent*(broadBlurRadius*2.0);\n"
					     "float2 broadBlurY=broadDir*broadBlurRadius;\n"
					     "float2 buv0=broadBase*(G15.y*0.000584/carrierSize)-broadDir*(broadT*primaryRate);\n"
					     "float4 bp0=0.5*Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv0,broadMip)\n"
					     "+0.125*(Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv0+broadBlurX,broadMip)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv0-broadBlurX,broadMip)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv0+broadBlurY,broadMip)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv0-broadBlurY,broadMip));\n"
					     "float bm0=saturate((dot(bp0.rgb,float3(0,0.8,0.2))+0.05)/0.85);\n"
					     "float threshold=lerp(G6.x,G6.y,bm0);\n"
					     "float shoreAmount=1.0-shore;\n"
					     "float shoreGate=saturate((shoreAmount-threshold)/min(G15.z-threshold,-1e-4));\n"
					     "float vertexDist=length(WPos-CamPos)*0.01;\n"
					     "float distanceGate=saturate((G8.z-vertexDist)/max(G8.z,1e-4));\n"
					     "float ba=lerp(G5.z,G5w,distanceGate)*shoreGate;\n"
					     "float2 contactUv=sourceWorldM/max(ContactDivisor,1.0)-0.5;\n"
					     "float contactRaw=Texture2DSampleLevel(ContactFoam,ContactFoamSampler,contactUv,0).r;\n"
					     "float contact=saturate((contactRaw-ContactRemapLow)/max(1.0-ContactRemapLow,1e-4));\n"
					     "ba+=contact*ContactGain*UseContact;\n"
					     "float2 buv1=sourceWorldM*G12.y+detailT*float2(G11.y*0.1,G11.y*0.0001);\n"
					     "float4 bp1=0.5*Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv1,0)\n"
					     "+0.125*(Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv1+broadBlurX,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv1-broadBlurX,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv1+broadBlurY,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv1-broadBlurY,0));\n"
					     "float bm1=1.0+G11.x*(dot(bp1,float4(G1,G1w))-1.0);\n"
					     "float2 buv2=sourceWorldM*G11.z-detailT*float2(G11.y,G11.y*0.001);\n"
					     "float4 bp2=0.5*Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv2,0)\n"
					     "+0.125*(Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv2+broadBlurX,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv2-broadBlurX,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv2+broadBlurY,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv2-broadBlurY,0));\n"
					     "float2 buv3=sourceWorldM*G18w;\n"
					     "float4 bp3=0.5*Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv3,0)\n"
					     "+0.125*(Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv3+broadBlurX,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv3-broadBlurX,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv3+broadBlurY,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv3-broadBlurY,0));\n"
					     "float bq3=G10.z+G10w*G10.z*(2.0*bp3.w-1.0);\n"
					     "float shaped=saturate((bm1-G12w+bq3*bm1*(dot(bp2,float4(G0,G0w))-1.0))/max(G12.x-G12w,1e-4));\n"
					     "ba+=G13.y*shaped;\n"
					     "float2 buv4=sourceWorldM*(G13w*G15.y*0.000584*G14.y)-detailT*float2(0.011,0.008);\n"
					     "float4 bp4=0.5*Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv4,0)\n"
					     "+0.125*(Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv4+broadBlurX,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv4-broadBlurX,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv4+broadBlurY,0)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,buv4-broadBlurY,0));\n"
					     "float bm4=saturate((dot(bp4.yz,float2(0.3,0.7))-G15.x)/max(0.8-G15.x,1e-4));\n"
					     "float bv=bm0+(2.0*bm0*bm4-bm0)*bp4.w;\n"
					     "float mixed=ba*(1.0+G14w*(bv-1.0));\n"
					     "float2 noiseUv=broadBase*0.033-broadDir*(broadT*noiseRate);\n"
					     "float noiseR=0.5*Texture2DSampleLevel(NoiseTex,NoiseTexSampler,noiseUv,broadMip).r\n"
					     "+0.125*(Texture2DSampleLevel(NoiseTex,NoiseTexSampler,noiseUv+broadBlurX,broadMip).r+Texture2DSampleLevel(NoiseTex,NoiseTexSampler,noiseUv-broadBlurX,broadMip).r+Texture2DSampleLevel(NoiseTex,NoiseTexSampler,noiseUv+broadBlurY,broadMip).r+Texture2DSampleLevel(NoiseTex,NoiseTexSampler,noiseUv-broadBlurY,broadMip).r);\n"
					     "float rr=saturate((mixed-0.1)*(4.0/3.0));\n"
					     "float drawAcc=mixed-(1.0-noiseR)*0.75*G14.z*(0.35-0.23*rr);\n"
					     "float2 foamUv=sourceWorldM*FoamUvScale;\n"
					     "float2 microBase=sourceWorldM*MicroUvScale;\n"
					     "float microPhase=T*MicroFlowSpeed;\n"
					     "float4 microA=Texture2DSampleLevel(MicroSheet,MicroSheetSampler,microBase+float2(microPhase,microPhase)*0.01,0);\n"
					     "float4 microB=Texture2DSampleLevel(MicroSheet,MicroSheetSampler,microBase-microPhase*float2(0.008,0.0075),0);\n"
					     "float4 foamA=Texture2DSampleLevel(FoamSheet,FoamSheetSampler,foamUv,0);\n"
					     "float fastSheet=UseSheets*(foamA.w-pow(max(microA.w,microB.w),2.0));\n"
					     "float authoredExtended=saturate(2.0*drawAcc-0.5+fastSheet);\n"
					     "float broadCoverage=saturate((authoredExtended-CompositeLow)/max(CompositeHigh-CompositeLow,1e-4));\n"
					     "// Use the SAME raw BF6 low-frequency inputs that feed the broad foam graph.\n"
					     "// bm0 is the wavelength-scaled primary field; noiseR keeps its native\n"
					     "// low-frequency scale and breaks up otherwise uniform parallel crests.\n"
					     "// Blend those shipped fields for unfoamed trough/body motion, then use the\n"
					     "// continuous pre-threshold drawAcc as a wide shoulder under the exact foam.\n"
					     "// Never feed the full high-frequency foam graph into the clipmap vertices.\n"
					     "// Its progressively coarser rings undersample that graph into isolated\n"
					     "// point spikes.  The two shipped low-frequency fields are continuous and\n"
					     "// already 173--291 m wide, so they form the actual water body while the\n"
					     "// exact draw graph remains a pixel-stage foam mask.  The bridge height is\n"
					     "// calibrated so 4 m means a +4 m crest and a -0.4 m trough.\n"
					     "// Directional broad-pattern energy must carry the wave train.  OceanNoise\n"
					     "// only breaks repetition; making it dominant creates travelling bubbles.\n"
					     "float rawBroadBody=saturate(0.10*noiseR+0.90*bm0);\n"
					     "rawBroadBody=rawBroadBody*rawBroadBody*(3.0-2.0*rawBroadBody);\n"
					     "// The approved four-metre value is crest rise, not a symmetric\n"
					     "// seven-metre peak-to-trough wall. Keep a shallow -0.4 m floor and\n"
					     "// continuously ramp the complete water body up to the +4 m crest.\n"
					     "float waveProfile=lerp(-0.10,1.0,rawBroadBody);\n"
					     "float crestM=foamCoupling*FoamWaveHeight*waveProfile;\n"
					     "// This broad field is a draw-graph deformation bridge, not an FFT\n"
					     "// cascade. Keep it outside BF6FFTEnabled so an unavailable/disabled\n"
					     "// cascade control cannot leave moving foam on a flat surface.\n"
					     "float broadZ=coarseAttenuation*crestM;\n"
					     "// The separate BF6 interactive simulation is an eWave field. Until its\n"
					     "// runtime atlas producer is exposed, replay a stable low-frequency modal\n"
					     "// seed with the decoded deep-water dispersion law. Unlike broadZ this\n"
					     "// remains available on Aftermath, whose broad draw sheets are absent.\n"
					     "float2 idir=normalize(float2(0.84,0.54));\n"
					     "float ilen=max(InteractiveLength,4.0); float iz=0.0;\n"
					     "const float iw[4]={0.52,0.25,0.15,0.08};\n"
					     "const float ir[4]={1.0,0.58,0.34,0.21};\n"
					     "for(int ii=0;ii<4;++ii){ float L=ilen*ir[ii]; float k=6.2831853/L;\n"
					     "  float a=(ii&1)?-0.22:0.17; float2 d=normalize(float2(idir.x-a*idir.y,idir.y+a*idir.x));\n"
					     "  float ph=k*dot(d,sourceWorldM)-sqrt(9.81*k)*T*InteractiveRate+ii*1.713;\n"
					     "  iz+=iw[ii]*sin(ph); }\n"
					     "float interactiveZ=InteractiveEnabled*coarseAttenuation*InteractiveHeight*iz;\n"
					     "// Provisional stand-in for the missing WaterInteractiveDisplacement\n"
					     "// atlas. It uses only mounted level resources and is deliberately\n"
					     "// excluded from the exactness claim below.\n"
					     "return float3(Enabled*h.x,Enabled*h.y,Enabled*v+surfaceLift+broadZ+interactiveZ)*100.0;"));
				UMaterialEditingLibrary::ConnectMaterialProperty(FFTWpo, TEXT(""), MP_WorldPositionOffset);

				FFTNormal->Code = TEXT(
					"float2 worldM=WPos.xy*0.01; float2 sourceWorldM=worldM+WaterSourceWorld.xy; float2 cascadePm=worldM-WaterOrigin.xy; float2 q=(sourceWorldM-DepthMin)/DepthSpan;\n"
					"float2 hq=(sourceWorldM-WaterHeightMin.xy)/WaterHeightSpan.xy; float hinside=step(0,hq.x)*step(hq.x,1)*step(0,hq.y)*step(hq.y,1);\n"
					"float2 he=WaterHeightTexelM.xy/max(WaterHeightSpan.xy,float2(1e-4,1e-4));\n"
					"float hc=Texture2DSample(WaterHeightTex,WaterHeightTexSampler,saturate(hq)).r;\n"
					"float hl=Texture2DSample(WaterHeightTex,WaterHeightTexSampler,saturate(hq-float2(he.x,0))).r;\n"
					"float hr=Texture2DSample(WaterHeightTex,WaterHeightTexSampler,saturate(hq+float2(he.x,0))).r;\n"
					"float hb=Texture2DSample(WaterHeightTex,WaterHeightTexSampler,saturate(hq-float2(0,he.y))).r;\n"
					"float ht=Texture2DSample(WaterHeightTex,WaterHeightTexSampler,saturate(hq+float2(0,he.y))).r;\n"
					"float hactive=WaterHeightAvailable*hinside*step(WaterSourceWorld.z+1e-4,hc);\n"
					"float2 heightSlope=-hactive*float2((hr-hl)/max(2*WaterHeightTexelM.x,1e-4),(ht-hb)/max(2*WaterHeightTexelM.y,1e-4));\n"
					"float terrain=Texture2DSample(DepthTex,DepthTexSampler,saturate(q)).r;\n"
					// THE GATE IS THE GAME'S: the shipped VS tests ShoreDepth > 0
					// before ever dividing, so an authored 0 means shore
					// attenuation deliberately OFF (mp_tungsten ships exactly
					// that; finding shoredepth-zero-is-a-shader-gate). The old
					// max(ShoreD,0.5) floor manufactured a half-metre fade the
					// game turned off, killing waves at the waterline.
					"float shoreOn=step(1e-4,ShoreD);\n"
					"float dep=WPos.z*0.01-terrain+AdditionalDepth; float t=saturate(dep/max(ShoreD,1e-4));\n"
					"float authored=saturate(dot(float4(t*t*t,t*t,t,1),ShoreBlend));\n"
					"float shore=lerp(1.0,lerp(t*t*(3-2*t),authored,UseAuthoredShore),DepthAvailable*shoreOn);\n"
					"float2 maskUv=min(saturate((sourceWorldM-MaskMin.xy)/max(MaskSpan.xy,float2(1e-4,1e-4))),float2(0.999999,0.999999));\n"
					"float2 maskCell=floor(maskUv*MaskIndirectionSide); float2 maskCellUv=(maskCell+0.5)/MaskIndirectionSide;\n"
					"float4 mi=Texture2DSampleLevel(MaskIndirection,MaskIndirectionSampler,maskCellUv,0);\n"
					"float maskPage=floor(mi.r*255.0+0.5); float maskShift=floor(mi.g*255.0+0.5);\n"
					"float inlineValue=floor(mi.b*255.0+0.5)/255.0; float inlineFlag=step(0.5,mi.a);\n"
					"float maskScale=MaskIndirectionSide/exp2(maskShift); float2 maskLocal=frac(maskUv*maskScale);\n"
					"float2 maskAtlasUv=(maskLocal*MaskMeta.x+MaskMeta.y+0.5)/MaskMeta.z;\n"
					"float pageValue=Texture2DArraySampleLevel(MaskAtlas,MaskAtlasSampler,float3(maskAtlasUv,maskPage),0).r;\n"
					"float maskValue=lerp(pageValue,inlineValue,inlineFlag);\n"
					"float maskLaw=saturate(lerp(1.0-maskValue,maskValue,MaskPolarity));\n"
					"float coarseAttenuation=lerp(shore,maskLaw,MaskAvailable);\n"
					"float dist=length(WPos-CamPos)*0.01;\n"
					"float df=saturate((dist-DetailEnd)/min(DetailStart-DetailEnd,-1e-4));\n"
					// NEVER frac() A UV THAT FEEDS A HARDWARE-DERIVATIVE SAMPLE.
					//
					// frac() is discontinuous, so ddx/ddy of the wrapped coordinate spike
					// to ~1 at every wrap seam. The sampler reads that as 'this pixel
					// covers the whole texture' and picks the top mip, which is a flat
					// normal. On a sea that wraps every few metres those seams cover most
					// of the surface, and which pixels land on one depends on view angle -
					// so the water flattens as the camera turns and looks correct only
					// along one axis. The cascade textures are TA_Wrap (see
					// BF6HighPolyWaterFFT.cpp), so wrapping is the sampler's job and the
					// frac() was never needed.
					//
					// The vertex/displacement node above is unaffected: it samples with an
					// explicit mip, where derivatives are never consulted.
					//
					// Cascades 0 and 1 keep hardware derivatives; cascades 2 and 3 take an
					// explicit mip 0. That split is the GAME'S, read out of the MP_Tungsten
					// pixel DXIL (water-draw-shaders-decoded, section 3): the PS 'calls
					// sampleBias ... for cascades 0 and 1, and sampleLevel(..., 0) for the
					// flow-blended cascades 2 and 3'. The doubly-logarithmic mip ramp in
					// that finding belongs to the VERTEX shader and is not used here.
					"float4 n0=Texture2DSample(N0,N0Sampler,cascadePm/max(C0.x,1e-4))*C0.z;\n"
					"float4 n1=Texture2DSample(N1,N1Sampler,cascadePm/max(C1.x,1e-4))*C1.z;\n"
					"float4 n2=Texture2DSampleLevel(N2,N2Sampler,cascadePm/max(C2.x,1e-4),0)*C2.z;\n"
					"float4 n3=Texture2DSampleLevel(N3,N3Sampler,cascadePm/max(C3.x,1e-4),0)*C3.z;\n"
					"float p0=OverlapParams.y*cascadePm.x+OverlapParams.x*cascadePm.y;\n"
					"float p1=OverlapParams.y*cascadePm.y-OverlapParams.x*cascadePm.x;\n"
					"p1-=OverlapShearY*p0; p0-=OverlapParams.z*p1;\n"
					"float2 ouv=float2(OverlapParams2.x*p0,OverlapParams2.y*p1)/max(C0.x,1e-4);\n"
					// Same rule: the rotated second tap of cascade 0 is a derivative
					// sample too, so it must not frac() either.
					"float4 on=Texture2DSample(N0,N0Sampler,ouv)*C0.z;\n"
					"float2 x0=n0.yz*2-1,x1=n1.yz*2-1,x2=n2.yz*2-1,x3=n3.yz*2-1;\n"
					"float2 s0=x0*Amplitude*rsqrt(max(1-dot(x0,x0),1e-5));\n"
					"float2 s1=x1*Amplitude*rsqrt(max(1-dot(x1,x1),1e-5));\n"
					"float2 s2=x2*Amplitude*rsqrt(max(1-dot(x2,x2),1e-5));\n"
					"float2 s3=x3*Amplitude*rsqrt(max(1-dot(x3,x3),1e-5));\n"
					"float2 oxn=on.yz*2-1; float2 os=oxn*(Amplitude*OverlapHeight)*rsqrt(max(1-dot(oxn,oxn),1e-5));\n"
					"// Exact PS SSA 303..314: undo the two shears, then rotate back.\n"
					"float oy=os.y-os.x*OverlapParams.z; float ox=os.x-oy*OverlapShearY;\n"
					"float2 overlapSlope=float2(OverlapParams.y*ox-OverlapParams.x*oy,OverlapParams.x*ox+OverlapParams.y*oy);\n"
					"float2 slope=heightSlope+coarseAttenuation*(s0+s1+OverlapEnabled*overlapSlope)+df*(s2+s3);\n"
					"float3 n=normalize(lerp(float3(0,0,1),float3(slope.x,slope.y,1),Enabled));\n"
					"// MP_Isolated extended graph, recovered from its selected pixel DXIL.\n"
					"// The foam sheet is stationary at its own scale; the micro sheet uses\n"
					"// two asymmetric authored-flow taps. They are not a triangle blend.\n"
					"float2 foamUv=sourceWorldM*FoamUvScale;\n"
					"float2 microBase=sourceWorldM*MicroUvScale;\n"
					"float microPhase=T*MicroFlowSpeed;\n"
					"float2 microUvA=microBase+float2(microPhase,microPhase)*0.01;\n"
					"float2 microUvB=microBase-microPhase*float2(0.008,0.0075);\n"
					"float4 microA=Texture2DSample(MicroSheet,MicroSheetSampler,microUvA);\n"
					"float4 microB=Texture2DSample(MicroSheet,MicroSheetSampler,microUvB);\n"
					"float4 foamS=Texture2DSample(FoamSheet,FoamSheetSampler,foamUv);\n"
					"float2 contactUv=sourceWorldM/max(ContactDivisor,1.0)-0.5;\n"
					"float contactRaw=Texture2DSample(ContactFoam,ContactFoamSampler,contactUv).r;\n"
					"float contact=saturate((contactRaw-ContactRemapLow)/max(1.0-ContactRemapLow,1e-4));\n"
					"contact*=ContactGain*UseContact;\n"
					"float rawFoam=saturate(2.0*contact-0.5+foamS.w-pow(max(microA.w,microB.w),2.0));\n"
					"float coverage=saturate((rawFoam-CompositeLow)/max(CompositeHigh-CompositeLow,1e-4));\n"
					"float2 microN=0.5*(microA.xy+microB.xy)-0.5;\n"
					"float2 sheetN=lerp(Detail*microN,FoamSheetStrength*(foamS.xy-0.5),coverage);\n"
					"float2 dnxy=UseSheets*2.0*df*sheetN;\n"
					"float dnz=sqrt(saturate(1.0-dot(dnxy,dnxy)));\n"
					"float3 dpx=ddx(WPos),dpy=ddy(WPos); float2 dux=ddx(microBase),duy=ddy(microBase);\n"
					"float3 tangent=cross(dpy,n)*dux.x+cross(n,dpx)*dux.y;\n"
					"float3 bitangent=cross(dpy,n)*duy.x+cross(n,dpx)*duy.y;\n"
					"float invLen=rsqrt(max(max(dot(tangent,tangent),dot(bitangent,bitangent)),1e-8));\n"
					"float3 nd=dnz*n-dnxy.x*tangent*invLen+dnxy.y*bitangent*invLen;\n"
					"// `nd` already contains dn.z times the base FFT normal. The selected\n"
					"// MP_Isolated PS normalizes that perturbed result directly; adding `n`\n"
					"// here a second time is the rejected double-add control.\n"
					"return normalize(float3(nd.x,nd.y,1.0));");
				FFTNormal->OutputType = CMOT_Float3;
				FFTNormal->Description = TEXT("BF6 four-cascade FFT normal");
				AddFFTInputs(FFTNormal, true);
				auto AddBroadNormalInput = [&FFTNormal](const TCHAR* Name, UMaterialExpression* E)
				{
					FCustomInput I; I.InputName = Name; I.Input.Expression = E;
					FFTNormal->Inputs.Add(I);
				};
				AddBroadNormalInput(TEXT("UseBroad"), UseBroadPattern);
				AddBroadNormalInput(TEXT("GraphVersion"), ExtendedGraphVersion);
				AddBroadNormalInput(TEXT("FoamWaveHeight"), FoamWaveHeight);
				AddBroadNormalInput(TEXT("BroadWaveSize"), BroadCarrierSize);
				AddBroadNormalInput(TEXT("BroadTimeScale"), BroadTimeScale);
				AddBroadNormalInput(TEXT("BroadPattern"), BroadPattern);
				AddBroadNormalInput(TEXT("NoiseTex"), OceanNoise);
				AddBroadNormalInput(TEXT("G13w"), ExtendedCb1W[13]);
				AddBroadNormalInput(TEXT("G15"), ExtendedCb1[15]);
				FFTNormal->Code.ReplaceInline(
					TEXT("return normalize(float3(nd.x,nd.y,1.0));"),
					TEXT("// Evaluate the low-frequency displacement field in the PIXEL stage.\n"
					     "// The rejected control differentiated BroadHeightCm exported by the\n"
					     "// vertex WPO node. That value was affine within each mesh triangle,\n"
					     "// producing a hard normal/reflection change across every quad diagonal.\n"
					     "// This is the same broad body law and source textures as WPO, evaluated\n"
					     "// continuously per pixel so screen derivatives measure the field rather\n"
					     "// than the water grid's triangulation.\n"
					     "float bgCoupling=saturate(UseBroad*GraphVersion);\n"
					     "float bgCarrier=max(BroadWaveSize,1.0);\n"
					     "float bgTime=T*BroadTimeScale;\n"
					     "float2 bgBase=sourceWorldM*G13w;\n"
					     "float2 bgDir=normalize(float2(OverlapParams.y,OverlapParams.x)+float2(1e-5,0));\n"
					     "float bgPrimaryFreq=max(abs(G13w*G15.y*0.000584)/bgCarrier,1e-6);\n"
					     "float bgNoiseFreq=max(abs(G13w*0.033),1e-6);\n"
					     "float bgPrimaryRate=sqrt(9.81*bgPrimaryFreq/6.2831853);\n"
					     "float bgNoiseRate=sqrt(9.81*bgNoiseFreq/6.2831853);\n"
					     "float bgMip=clamp(log2(bgCarrier),0.0,5.0);\n"
					     "float bgRadius=0.0030*(1.0+bgMip);\n"
					     "float2 bgTangent=float2(-bgDir.y,bgDir.x);\n"
					     "float2 bgAlong=bgTangent*(bgRadius*2.0);\n"
					     "float2 bgAcross=bgDir*bgRadius;\n"
					     "float2 bgUv=bgBase*(G15.y*0.000584/bgCarrier)-bgDir*(bgTime*bgPrimaryRate);\n"
					     "float4 bgP=0.5*Texture2DSampleLevel(BroadPattern,BroadPatternSampler,bgUv,bgMip)\n"
					     "+0.125*(Texture2DSampleLevel(BroadPattern,BroadPatternSampler,bgUv+bgAlong,bgMip)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,bgUv-bgAlong,bgMip)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,bgUv+bgAcross,bgMip)+Texture2DSampleLevel(BroadPattern,BroadPatternSampler,bgUv-bgAcross,bgMip));\n"
					     "float bgM=saturate((dot(bgP.rgb,float3(0,0.8,0.2))+0.05)/0.85);\n"
					     "float2 bgNoiseUv=bgBase*0.033-bgDir*(bgTime*bgNoiseRate);\n"
					     "float bgNoise=0.5*Texture2DSampleLevel(NoiseTex,NoiseTexSampler,bgNoiseUv,bgMip).r\n"
					     "+0.125*(Texture2DSampleLevel(NoiseTex,NoiseTexSampler,bgNoiseUv+bgAlong,bgMip).r+Texture2DSampleLevel(NoiseTex,NoiseTexSampler,bgNoiseUv-bgAlong,bgMip).r+Texture2DSampleLevel(NoiseTex,NoiseTexSampler,bgNoiseUv+bgAcross,bgMip).r+Texture2DSampleLevel(NoiseTex,NoiseTexSampler,bgNoiseUv-bgAcross,bgMip).r);\n"
					     "float bgBody=saturate(0.10*bgNoise+0.90*bgM);\n"
					     "bgBody=bgBody*bgBody*(3.0-2.0*bgBody);\n"
					     "float broadHeightM=coarseAttenuation*bgCoupling*FoamWaveHeight*lerp(-0.10,1.0,bgBody);\n"
					     "float2 px=ddx(sourceWorldM), py=ddy(sourceWorldM);\n"
					     "float hx=ddx(broadHeightM), hy=ddy(broadHeightM);\n"
					     "float det=px.x*py.y-px.y*py.x;\n"
					     "float invDet=(abs(det)>1e-6)?rcp(det):0.0;\n"
					     "float2 broadGrad=float2(hx*py.y-hy*px.y,px.x*hy-py.x*hx)*invDet;\n"
					     "// broadGrad belongs to the rejected pixel-texture-as-WPO control.\n"
					     "// Analytic normal for the provisional eWave modal seed used by WPO.\n"
					     "float2 idir=normalize(float2(0.84,0.54));\n"
					     "float ilen=max(InteractiveLength,4.0); float2 igrad=float2(0,0);\n"
					     "const float iw[4]={0.52,0.25,0.15,0.08};\n"
					     "const float ir[4]={1.0,0.58,0.34,0.21};\n"
					     "for(int ii=0;ii<4;++ii){ float L=ilen*ir[ii]; float k=6.2831853/L;\n"
					     "  float a=(ii&1)?-0.22:0.17; float2 d=normalize(float2(idir.x-a*idir.y,idir.y+a*idir.x));\n"
					     "  float ph=k*dot(d,sourceWorldM)-sqrt(9.81*k)*T*InteractiveRate+ii*1.713;\n"
					     "  igrad+=InteractiveHeight*iw[ii]*k*d*cos(ph); }\n"
					     "float2 iNormal=-InteractiveEnabled*coarseAttenuation*igrad;\n"
					     "return normalize(float3(nd.x+iNormal.x,nd.y+iNormal.y,1.0));"));
				const bool bPixelBroadNormal = false;
				const bool bRejectedTriangleNormal = FFTNormal->Code.Contains(TEXT("ddx(BroadHeightCm)"));
				UE_LOG(LogBF6HighPoly, Display,
					TEXT("water broad-normal route: pixel-field real=%d, interpolated-triangle control=%d"),
					bPixelBroadNormal ? 1 : 0, bRejectedTriangleNormal ? 1 : 0);
				UMaterialEditingLibrary::ConnectMaterialProperty(FFTNormal, TEXT(""), MP_Normal);
				M->bTangentSpaceNormal = false;
			}
		}

		// ---- FOAM, from the folding of the displacement field -------------
		//
		// Hoisted out of the block because OPACITY needs it. On a Single Layer
		// Water material Opacity is not "how much water": it is the coverage of
		// whatever opaque material sits ON TOP of the water, and foam is
		// exactly that. See the Opacity block below.
		UMaterialExpressionCustom* FoamOut = nullptr;
		if (WP && Tm && Gn && Ch && Bl && MnL && FTh && FMx && DrawFTh && FCon &&
			ShD && AddD && UseSh && ShB && ShF && DepthAvailable && Wv[0] && FFTOverlapParams)
		{
			UMaterialExpressionCustom* FoamX =
				Cast<UMaterialExpressionCustom>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionCustom::StaticClass(), -300, 1160));
			if (FoamX)
			{
				FoamX->Code = TEXT("float3 w[8] = {W0,W1,W2,W3,W4,W5,W6,W7};\nconst float ratio[8] = {1.0, 0.618, 0.382, 0.236, 0.146, 0.090, 0.056, 0.034};\nfloat2 worldM = WPos.xy * 0.01;\n// The lab recentres only its preview geometry. Every authored draw texture is\n// world anchored, so restore the source MP_Isolated coordinate before sampling.\nfloat2 sourceWorldM = worldM + WaterSourceWorld.xy;\nfloat2 pm = sourceWorldM;\n// SHORE FADE, the way the game does it: the wave amplitude is scaled\n// down by how shallow the water is. Recovered form is\n//   depth = waterY - terrainY;  t = saturate(depth / D);\n//   fade  = saturate(cubic(t))\n// with the cubic authored per level - a plain smoothstep in the\n// default case. Without it the swell runs straight into the beach at\n// full height, which is what makes a repeating pattern so obvious near\n// land.\nfloat2 duv = (sourceWorldM - DepthMin) / DepthSpan;\nfloat terrainY = Texture2DSample(DepthTex, DepthTexSampler, saturate(duv)).r;\nfloat depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep\nfloat div = 0.0;\nfloat scale = 0.0;\nfor (int i = 0; i < 8; i++) {\n  float len = max(ratio[i] * BaseLen, 0.5);\n  if (len < MinLen) continue;\n  float2 d = normalize(w[i].xy + float2(1e-5, 0));\n  float A = w[i].z * Gain * shore;\n  float k = 6.2831853 / len;\n  float ph = k * dot(d, pm) - sqrt(9.81 * k) * T;\n  float term = Chop * A * k;\n  div   += term * sin(ph);\n  scale += term;\n}\nfloat f = div / max(scale, 1e-4);\nfloat bias = clamp(Thr / 60.0, 0.02, 0.85);\nfloat crest = saturate((f - bias) / max(1.0 - bias, 0.05)) * Mx;\n// AND FOAM WHERE IT MEETS LAND. The game exports 1 - fade to the pixel\n// shader for exactly this: surf collects where the water is shallow.\nfloat surf = saturate(1.0 - shore) * ShoreFoam;\nreturn saturate(max(crest, surf));");
				FoamX->Code.ReplaceInline(
					TEXT("float depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep\nfloat div"),
					TEXT("float depth = (WPos.z * 0.01) - terrainY + AdditionalDepth;\nfloat shoreOn = step(1e-4, ShoreD);\nfloat st = saturate(depth / max(ShoreD, 1e-4));\nfloat authoredShore = saturate(dot(float4(st*st*st, st*st, st, 1.0), ShoreBlend));\nfloat fallbackShore = st * st * (3.0 - 2.0 * st);\nfloat shore = lerp(1.0, lerp(fallbackShore, authoredShore, UseAuthoredShore), DepthAvailable * shoreOn);\nfloat dist = length(WPos - CamPos) * 0.01;\nfloat div"));
				FoamX->Code.ReplaceInline(
					TEXT("float A = w[i].z * Gain * shore;"),
					TEXT("float lodFade = saturate((len * 24.0 - dist) / max(len * 12.0, 1.0));\n  float A = w[i].z * Gain * shore * lodFade;"));
				FoamX->Code.ReplaceInline(
					TEXT("float crest = saturate((f - bias) / max(1.0 - bias, 0.05)) * Mx;\n// AND FOAM WHERE IT MEETS LAND. The game exports 1 - fade to the pixel\n// shader for exactly this: surf collects where the water is shallow.\nfloat surf = saturate(1.0 - shore) * ShoreFoam;\nreturn saturate(max(crest, surf));"),
					TEXT("float crest = saturate((f - bias) / max(1.0 - bias, 0.05));\ncrest = saturate((crest - DrawThr) / max(1.0 - DrawThr, 0.05));\ncrest = saturate(crest / max(FoamContrast, 0.01)) * Mx;\n// BF6's shore term suppresses simulated foam; it does not manufacture surf.\ncrest *= saturate(1.0 - ShoreSuppress * (1.0 - shore));\nreturn saturate(crest);"));
				FoamX->Code.ReplaceInline(
					TEXT("// BF6's shore term suppresses simulated foam; it does not manufacture surf.\ncrest *= saturate(1.0 - ShoreSuppress * (1.0 - shore));\nreturn saturate(crest);"),
					TEXT("// Tsuru's FFT FoamMaxValue is zero, but its draw shader still makes\n"
					     "// whitecaps from the shipped micro/foam sheets. MP_Isolated uses a\n"
					     "// max-of-two-taps squared family rather than Tungsten's simple lerp.\n"
					     "float2 foamUv = pm * FoamUvScale;\n"
					     "float2 microBase = pm * MicroUvScale;\n"
					     "float microPhase = T * MicroFlowSpeed;\n"
					     "float2 microUvA = microBase + float2(microPhase, microPhase) * 0.01;\n"
					     "float2 microUvB = microBase - microPhase * float2(0.008, 0.0075);\n"
					     "float4 microA = Texture2DSample(MicroSheet, MicroSheetSampler, microUvA);\n"
					     "float4 microB = Texture2DSample(MicroSheet, MicroSheetSampler, microUvB);\n"
					     "float4 foamA = Texture2DSample(FoamSheet, FoamSheetSampler, foamUv);\n"
					     "float2 contactUv = pm / max(ContactDivisor, 1.0) - 0.5;\n"
					     "float contactRaw = Texture2DSample(ContactFoam, ContactFoamSampler, contactUv).r;\n"
					     "float contact = saturate((contactRaw - ContactRemapLow) / max(1.0 - ContactRemapLow, 1e-4));\n"
					     "contact *= ContactGain * UseContact;\n"
					     "crest *= saturate(1.0 - ShoreSuppress * (1.0 - shore));\n"
					     "// Full open-ocean t32 accumulation from MP_Isolated pass 0. G0..G20\n"
					     "// are the current material cbuffer rebuilt from the mounted depot.\n"
					     "// Numeric coefficients below are literals in the selected DXIL.\n"
					     "float carrierSize = max(BroadWaveSize, 1.0);\n"
					     "float broadT = T * BroadTimeScale;\n"
					     "float detailT = T * 0.05;\n"
					     "float2 broadBase = pm * G13w;\n"
					     "float2 broadDir = normalize(float2(BroadDirection.y,BroadDirection.x)+float2(1e-5,0));\n"
					     "float primaryFreq = max(abs(G13w*G15.y*0.000584),1e-6);\n"
					     "float carrierFreq = max(primaryFreq/carrierSize,1e-6);\n"
					     "float noiseFreq = max(abs(G13w*0.033),1e-6);\n"
					     "float primaryRate = sqrt(9.81*primaryFreq/6.2831853);\n"
					     "float carrierRate = sqrt(9.81*carrierFreq/6.2831853);\n"
					     "float noiseRate = sqrt(9.81*noiseFreq/6.2831853);\n"
					     "float broadMip = clamp(log2(carrierSize),0.0,5.0);\n"
					     "float carrierRadius = 0.0030*(1.0+broadMip);\n"
					     "float2 carrierTangent = float2(-broadDir.y,broadDir.x);\n"
					     "float2 carrierAlong = carrierTangent*(carrierRadius*2.0);\n"
					     "float2 carrierAcross = broadDir*carrierRadius;\n"
					     "float2 uv0 = broadBase * G15.y * 0.000584 - broadDir*(broadT*primaryRate);\n"
					     "float4 p0 = Texture2DSample(BroadPattern,BroadPatternSampler,uv0);\n"
					     "float m0 = saturate((dot(p0.rgb,float3(0,0.8,0.2))+0.05)/0.85);\n"
					     "float2 carrierUv0 = broadBase*(G15.y*0.000584/carrierSize)-broadDir*(broadT*carrierRate);\n"
					     "float4 carrierP0 = 0.5*Texture2DSample(BroadPattern,BroadPatternSampler,carrierUv0) + 0.125*(Texture2DSample(BroadPattern,BroadPatternSampler,carrierUv0+carrierAlong)+Texture2DSample(BroadPattern,BroadPatternSampler,carrierUv0-carrierAlong)+Texture2DSample(BroadPattern,BroadPatternSampler,carrierUv0+carrierAcross)+Texture2DSample(BroadPattern,BroadPatternSampler,carrierUv0-carrierAcross));\n"
					     "float carrierM0 = saturate((dot(carrierP0.rgb,float3(0,0.8,0.2))+0.05)/0.85);\n"
					     "float threshold = lerp(G6.x,G6.y,m0);\n"
					     "float shoreAmount = 1.0-shore;\n"
					     "float shoreGate = saturate((shoreAmount-threshold)/min(G15.z-threshold,-1e-4));\n"
					     "float distanceGate = saturate((G8.z-dist)/max(G8.z,1e-4));\n"
					     "// The selected shader multiplies the following authored term by the\n"
					     "// region-atlas value t31.z*2. Outside a painted region its branch default\n"
					     "// is exactly 1, so the neutral runtime fallback is the unscaled term below.\n"
					     "// THE GAME'S COVERAGE SEED: the per-cascade fold channel, chained the\n"
					     "// way the Tungsten pass-0 pixel shader chains it (foam.md 2.6).\n"
					     "// Cascades 2 and 3 take NO shore or distance factor - the vertex\n"
					     "// shader stores TEXCOORD2.w = 0.0 literally.\n"
					     "float2 cpm = worldM - WaterOrigin.xy;\n"
					     "float cf0 = saturate(Texture2DSample(N0,N0Sampler,cpm/max(C0.x,1e-4)).x*C0.z);\n"
					     "float cf1 = saturate(Texture2DSample(N1,N1Sampler,cpm/max(C1.x,1e-4)).x*C1.z);\n"
					     "float cf2 = saturate(Texture2DSampleLevel(N2,N2Sampler,cpm/max(C2.x,1e-4),0).x*C2.z);\n"
					     "float cf3 = saturate(Texture2DSampleLevel(N3,N3Sampler,cpm/max(C3.x,1e-4),0).x*C3.z);\n"
					     "float chainFade = shore;\n"
					     "float fch = cf0*chainFade*FoamW0;\n"
					     "fch = lerp(fch, FoamW1, cf1*chainFade);\n"
					     "fch = lerp(fch, FoamW2, cf2);\n"
					     "fch = lerp(fch, FoamW3, cf3);\n"
					     "float chainDamp = 1.0 - ShoreSuppress*shoreAmount;\n"
					     "float acc = (DrawThr - 0.5) + chainDamp*(1.0 - DrawThr)*fch;\n"
					     "float a = lerp(G5.z,G5w,distanceGate)*shoreGate + contact + FoamChain*acc;\n"
					     "float2 uv1 = pm*G12.y + detailT*float2(G11.y*0.1,G11.y*0.0001);\n"
					     "float4 p1 = Texture2DSample(BroadPattern,BroadPatternSampler,uv1);\n"
					     "float m1 = 1.0 + G11.x*(dot(p1,float4(G1,G1w))-1.0);\n"
					     "float2 uv2 = pm*G11.z - detailT*float2(G11.y,G11.y*0.001);\n"
					     "float4 p2 = Texture2DSample(BroadPattern,BroadPatternSampler,uv2);\n"
					     "float2 uv3 = pm*G18w;\n"
					     "float4 p3 = Texture2DSample(BroadPattern,BroadPatternSampler,uv3);\n"
					     "float q3 = G10.z + G10w*G10.z*(2.0*p3.w-1.0);\n"
					     "float shaped = saturate((m1-G12w + q3*m1*(dot(p2,float4(G0,G0w))-1.0))\n"
					     "                       / max(G12.x-G12w,1e-4));\n"
					     "a += G13.y*shaped;\n"
					     "float2 uv4 = pm*(G13w*G15.y*0.000584*G14.y)-detailT*float2(0.011,0.008);\n"
					     "float4 p4 = Texture2DSample(BroadPattern,BroadPatternSampler,uv4);\n"
					     "float m4 = saturate((dot(p4.yz,float2(0.3,0.7))-G15.x)/max(0.8-G15.x,1e-4));\n"
					     "float v = m0 + (2.0*m0*m4-m0)*p4.w;\n"
					     "float mixed = a*(1.0+G14w*(v-1.0));\n"
					     "float2 carrierNoiseUv = broadBase*0.033-broadDir*(broadT*noiseRate);\n"
					     "float noiseR = Texture2DSample(NoiseTex,NoiseTexSampler,carrierNoiseUv).r;\n"
					     "float carrierNoise = 0.5*noiseR + 0.125*(Texture2DSample(NoiseTex,NoiseTexSampler,carrierNoiseUv+carrierAlong).r+Texture2DSample(NoiseTex,NoiseTexSampler,carrierNoiseUv-carrierAlong).r+Texture2DSample(NoiseTex,NoiseTexSampler,carrierNoiseUv+carrierAcross).r+Texture2DSample(NoiseTex,NoiseTexSampler,carrierNoiseUv-carrierAcross).r);\n"
					     "float rr = saturate((mixed-0.1)*(4.0/3.0));\n"
					     "float drawAcc = mixed-(1.0-noiseR)*0.75*G14.z*(0.35-0.23*rr);\n"
					     "float fastSheet = UseSheets*(foamA.w-pow(max(microA.w,microB.w),2.0));\n"
					     "float authoredExtended = saturate(2.0*drawAcc-0.5+fastSheet);\n"
					     "// Ocean family: saturate(saturate((0.5-microW)+foamW+2*acc)/contrast).\n"
					     "float chainMicroW = UseSheets*pow(max(microA.w,microB.w),2.0);\n"
					     "float chainFoamW = UseSheets*foamA.w;\n"
					     "float coverageOcean = saturate(saturate((0.5-chainMicroW)+chainFoamW+2.0*acc)/max(FoamContrast,1e-4));\n"
					     "float authoredSimple = lerp(saturate(2.0*(crest+contact)-0.5+fastSheet), coverageOcean, FoamChain);\n"
					     "float authored = lerp(authoredSimple,authoredExtended,UseBroad*GraphVersion);\n"
					     "float combined = saturate((authored - CompositeLow) / max(CompositeHigh - CompositeLow, 1e-4));\n"
					     "// BF6 composites foam with saturate(d/cb51.z), where d is the ray\n"
					     "// path from water surface to the opaque background. The terrain raster\n"
					     "// gives vertical depth here; divide by view cosine to recover that path.\n"
					     "float3 toEye = normalize(CamPos - WPos);\n"
					     "float pathDepth = max(depth, 0.0) / max(abs(toEye.z), 0.05);\n"
					     "float foamDepthGate = lerp(1.0, saturate(pathDepth / max(FoamDepthRamp, 0.01)), DepthAvailable);\n"
					     "// The selected ocean draw pass lacks the separate shoreline producer seen\n"
					     "// in the final game frame. Reconstruct its all-sides band from the same\n"
					     "// terrain depth that attenuates waves: zero on land/deep water, maximal\n"
					     "// through the shallow transition, with native noise breaking uniformity.\n"
					     "float shoreCore = saturate(4.0*shore*(1.0-shore));\n"
					     "float shoreBand = DepthAvailable*0.40*shoreCore*shoreCore;\n"
					     "shoreBand *= lerp(0.55,1.0,noiseR);\n"
					     "// Open-ocean foam is coverage on the upper part of the SAME broad\n"
					     "// carrier that deforms the vertices. Shore foam remains an independent\n"
					     "// shallow-water band, so neither term can paint white through troughs.\n"
					     "float broadBody = saturate(0.10*carrierNoise+0.90*carrierM0);\n"
					     "broadBody = broadBody*broadBody*(3.0-2.0*broadBody);\n"
					     "float waveProfile = lerp(-0.10,1.0,broadBody);\n"
					     "float foamShoulder = saturate((waveProfile-FoamCrestStart)/max(FoamCrestFull-FoamCrestStart,1e-4));\n"
					     "foamShoulder = foamShoulder*foamShoulder*(3.0-2.0*foamShoulder);\n"
					     "// The selected draw's combined term is a breakup/modulation field, not\n"
					     "// the crest body. Multiplying by it erased nearly every broad crest at\n"
					     "// Tsuru. Preserve the decoded modulation while guaranteeing that the\n"
					     "// displaced carrier itself produces a continuous foam shoulder.\n"
					     "float crestBreakup = lerp(0.35,1.0,combined);\n"
					     "float crestFoam = foamShoulder * crestBreakup * foamDepthGate;\n"
					     "// The shipped draw graph already outputs coverage. Shore depth only\n"
					     "// suppresses that coverage; it never synthesizes a surf band.\n"
					     "return saturate(combined * foamDepthGate);"));
				FoamX->OutputType = CMOT_Float1;
				FoamX->Description = TEXT("BF6 Jacobian foam");
				FoamX->Inputs.Empty();
				auto In = [&FoamX](const TCHAR* Nm, UMaterialExpression* E)
				{
					FCustomInput I; I.InputName = Nm; I.Input.Expression = E;
					FoamX->Inputs.Add(I);
				};
				In(TEXT("WPos"), WP);
				In(TEXT("T"), Tm);
				const TCHAR* WN[8] = { TEXT("W0"), TEXT("W1"), TEXT("W2"), TEXT("W3"),
				                       TEXT("W4"), TEXT("W5"), TEXT("W6"), TEXT("W7") };
				for (int32 i = 0; i < 8; i++) In(WN[i], Wv[i]);
				In(TEXT("Gain"), Gn);
				In(TEXT("Chop"), Ch);
				In(TEXT("BaseLen"), Bl);
				In(TEXT("MinLen"), MnL);
				In(TEXT("Detail"), Det);
				In(TEXT("DetailStart"), DetStart);
				In(TEXT("DetailEnd"), DetEnd);
				In(TEXT("CamPos"), CamP);
				In(TEXT("ShoreD"), ShD);
				In(TEXT("AdditionalDepth"), AddD);
				In(TEXT("UseAuthoredShore"), UseSh);
				In(TEXT("ShoreBlend"), ShB);
				In(TEXT("ShoreSuppress"), ShF);
				In(TEXT("DepthMin"), DMin);
				In(TEXT("DepthSpan"), DSpan);
				In(TEXT("DepthTex"), DTex);
				In(TEXT("DepthAvailable"), DepthAvailable);
				In(TEXT("Thr"), FTh);
				In(TEXT("Mx"), FMx);
				In(TEXT("DrawThr"), DrawFTh);
				In(TEXT("FoamContrast"), FCon);
				In(TEXT("UseSheets"), UseSheets);
				In(TEXT("MicroUvScale"), MicroSheetUv);
				In(TEXT("FoamUvScale"), FoamSheetUv);
				In(TEXT("MicroFlowSpeed"), MicroSheetFlow);
				In(TEXT("MicroSheet"), MicroSheet);
				In(TEXT("FoamSheet"), FoamSheet);
				In(TEXT("UseContact"), UseContact);
				In(TEXT("ContactFoam"), ContactFoam);
				// The cascade fold channels and their tile/scale meta, the same
				// objects the normal node samples, plus the chain's own constants.
				In(TEXT("WaterOrigin"), FFTOrigin);
				for (int32 fi = 0; fi < 4; ++fi)
				{
					In(*FString::Printf(TEXT("N%d"), fi), FFTNorm[fi]);
					In(*FString::Printf(TEXT("C%d"), fi), FFTMeta[fi]);
					In(*FString::Printf(TEXT("FoamW%d"), fi), FoamW[fi]);
				}
				In(TEXT("FoamChain"), FoamChain);
				In(TEXT("ContactDivisor"), ContactDiv);
				In(TEXT("ContactRemapLow"), ContactLow);
				In(TEXT("ContactGain"), ContactGain);
				In(TEXT("CompositeLow"), FoamCompositeLow);
				In(TEXT("CompositeHigh"), FoamCompositeHigh);
				In(TEXT("FoamDepthRamp"), FoamDepthRampDistance);
				In(TEXT("UseBroad"), UseBroadPattern);
				In(TEXT("BroadPattern"), BroadPattern);
				In(TEXT("NoiseTex"), OceanNoise);
				In(TEXT("GraphVersion"), ExtendedGraphVersion);
				In(TEXT("BroadWaveSize"), BroadCarrierSize);
				In(TEXT("BroadTimeScale"), BroadTimeScale);
				In(TEXT("FoamCrestStart"), FoamCrestStart);
				In(TEXT("FoamCrestFull"), FoamCrestFull);
				In(TEXT("BroadDirection"), FFTOverlapParams);
				for (int32 i = 0; i < 22; ++i)
				{
					In(*FString::Printf(TEXT("G%d"), i), ExtendedCb1[i]);
					In(*FString::Printf(TEXT("G%dw"), i), ExtendedCb1W[i]);
				}
				In(TEXT("ShoreFallback"), ShoreSurfFallback);
				In(TEXT("WaterMin"), WaterMin);
				In(TEXT("WaterSpan"), WaterSpan);
				In(TEXT("WaterSourceWorld"), WaterSourceWorld);

				// Frostbite writes foam coverage to a second target and its deferred
				// ocean composite applies cb54.rgb LAST. Unreal has no equivalent
				// private water target in this material, so reproduce that final
				// operation here with the same coverage signal and live VE tint.
				UMaterialExpressionLinearInterpolate* FoamTintMix =
					Cast<UMaterialExpressionLinearInterpolate>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionLinearInterpolate::StaticClass(), -100, 80));
				if (FoamTintMix && Tint && CompositeFoamTint)
				{
					UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), FoamTintMix, TEXT("A"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(CompositeFoamTint, TEXT(""), FoamTintMix, TEXT("B"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(FoamX, TEXT(""), FoamTintMix, TEXT("Alpha"));
					UMaterialEditingLibrary::ConnectMaterialProperty(FoamTintMix, TEXT(""), MP_BaseColor);
				}
				// foam kills the mirror finish where it sits
				UMaterialExpressionLinearInterpolate* RoughMix =
					Cast<UMaterialExpressionLinearInterpolate>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionLinearInterpolate::StaticClass(), -100, 200));
				if (RoughMix && Rough && FoamRough)
				{
					UMaterialEditingLibrary::ConnectMaterialExpressions(Rough, TEXT(""), RoughMix, TEXT("A"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(FoamRough, TEXT(""), RoughMix, TEXT("B"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(FoamX, TEXT(""), RoughMix, TEXT("Alpha"));
					UMaterialEditingLibrary::ConnectMaterialProperty(RoughMix, TEXT(""), MP_Roughness);
				}
				FoamOut = FoamX;
			}
		}

		// One coverage signal, as in the selected pixel shader. The old normal
		// branch used only contact + the two detail sheets, while FoamOut also
		// included MP_Isolated's broad authored graph. That disconnected the
		// visible foam bands from the normals that are supposed to make them read
		// as wave structure. Mutating the custom node here is safe: the material
		// has not been compiled yet and FoamOut has no dependency on FFTNormal.
		if (FFTNormalForFoam && FoamOut)
		{
			FCustomInput ExternalFoam;
			ExternalFoam.InputName = TEXT("ExternalFoam");
			ExternalFoam.Input.Expression = FoamOut;
			FFTNormalForFoam->Inputs.Add(ExternalFoam);
			FFTNormalForFoam->Code.ReplaceInline(
				TEXT("float coverage=saturate((rawFoam-CompositeLow)/max(CompositeHigh-CompositeLow,1e-4));"),
				TEXT("float coverage=saturate(ExternalFoam);"));
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water: exact authored foam coverage now drives the detail-normal blend; BaseColor remains authored"));
		}

		// ---- OPACITY: THE PIN THAT DECIDES WHETHER THIS IS WATER AT ALL ----
		//
		// This is what made every previous attempt fail, and not one of the
		// coefficients was ever involved.
		//
		// On a Single Layer Water material Opacity means the COVERAGE OF THE
		// MATERIAL LAYERED ON TOP OF THE WATER, not the water's own opacity.
		// The base pass reads it as
		//     BaseMaterialCoverageOverWater = Opacity      BasePassPixelShader.usf:1140
		//     WaterVisibility = 1.0 - BaseMaterialCoverageOverWater
		// and the whole of EvaluateWaterVolumeLighting sits inside
		//     if (WaterVisibility > 0.0f)                  SingleLayerWaterShading.ush:74
		// so at Opacity 1 the scene behind the water is never sampled, the
		// transmittance is never applied and the scattering is never added.
		// What is left is a lit sheet of BaseColor plus a reflection, whatever
		// the extinction says. That is why WaterClarity 5000 changed nothing:
		// extinction was never consulted.
		//
		// Unconnected, Opacity IS 1 - FVector4(1,0,0,0) in
		// MaterialAttributeDefinitionMap.cpp:401 - and MP_Opacity stays an
		// active property for this shading model at any blend mode
		// (Material.cpp:7976). Substrate lands in the same place: the legacy
		// conversion passes it straight into TopMaterialOpacity
		// (SubstrateLegacyConversion.ush:201) and the base pass reads it back
		// at line 1090. The trap is that Unreal's own Substrate water node
		// defaults this same quantity to ZERO. Only the legacy root pin
		// defaults to one.
		//
		// So: zero for open water, and the foam mask where an opaque layer
		// really does sit on the surface.
		{
			UMaterialExpressionScalarParameter* FoamCov =
				Cast<UMaterialExpressionScalarParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionScalarParameter::StaticClass(), -400, -60));
			if (FoamCov)
			{
				FoamCov->ParameterName = TEXT("FoamCoverage");
				FoamCov->DefaultValue = 1.f;
			}
			UMaterialExpressionMultiply* OpMul =
				(FoamOut && FoamCov)
				? Cast<UMaterialExpressionMultiply>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionMultiply::StaticClass(), -100, -60))
				: nullptr;
			if (OpMul)
			{
				UMaterialEditingLibrary::ConnectMaterialExpressions(FoamOut, TEXT(""), OpMul, TEXT("A"));
				UMaterialEditingLibrary::ConnectMaterialExpressions(FoamCov, TEXT(""), OpMul, TEXT("B"));
				UMaterialEditingLibrary::ConnectMaterialProperty(OpMul, TEXT(""), MP_Opacity);
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("water: opacity wired to the foam mask (0 = clear water, ")
					TEXT("1 = opaque foam on top)"));
			}
			else
			{
				// NO FOAM NODE, SO A HARD ZERO. Falling through here and
				// leaving Opacity unconnected is the exact bug this block
				// exists to fix, so the fallback must still connect something.
				UMaterialExpressionConstant* Zero =
					Cast<UMaterialExpressionConstant>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionConstant::StaticClass(), -100, -60));
				if (Zero)
				{
					Zero->R = 0.f;
					UMaterialEditingLibrary::ConnectMaterialProperty(Zero, TEXT(""), MP_Opacity);
					UE_LOG(LogBF6HighPoly, Log,
						TEXT("water: opacity wired to constant 0 (no foam node built)"));
				}
				else
				{
					UE_LOG(LogBF6HighPoly, Error,
						TEXT("water: OPACITY IS NOT CONNECTED. It defaults to 1, which ")
						TEXT("makes WaterVisibility 0 and skips the entire water volume - ")
						TEXT("the surface will be opaque whatever its colours say."));
				}
			}
		}

		// The normal full-map ocean is a HISM. If this permutation is requested
		// only when the component is registered, Unreal begins a second material
		// compile *after* the level reports ready; opening Water Lab later then
		// appears to "load the water shaders" even though it merely gives that
		// late compile time to finish. Compile the actual full-build vertex factory
		// with the parent graph from the start.
		M->PreEditChange(nullptr);
		M->PostEditChange();
		// SetMaterialUsage compiles immediately; the graph and its texture
		// references must be finalized before that permutation is requested.
		M->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		// A material that fails to compile renders as the ENGINE DEFAULT, which
		// is a flat grey - indistinguishable from "the water data is wrong"
		// unless somebody reads the log. Say it loudly, at Warning, with the
		// consequence spelled out.
		// SHADERS COMPILE ASYNCHRONOUSLY, so asking IsComplete the instant
		// after PostEditChange always says no and always warned - which
		// trains everyone to ignore the warning that matters. The engine
		// raises its own 'Failed to compile Material' with the HLSL error
		// attached; this just points at it.
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("water material built (shaders compile async - if the surface draws flat ")
			TEXT("grey, search the log for 'Failed to compile Material')"));
		// SAID ON EVERY BUILD, not only when someone runs the check.
		//
		// This material has been wrong for days and the self-check did not cover
		// it, so no line in the log has ever said what it actually is. A shading
		// model that failed to stick, and a custom output pin quietly running on
		// its default, both draw a surface that looks merely wrong rather than
		// broken.
		BF6_ReportMaterialState(TEXT("water"), M);
		GWaterParent = M;
		return M;
	}

	// The map's wave set, from its own simulation entity. Every rule here was
	// learned in the Godot plugin the hard way and is ported, not re-derived:
	// lobes are picked from WindDistribution by energy with a minimum angular
	// separation; each lobe gets a FAN of components rather than one ray (two
	// opposed rays of one wavelength are a STANDING wave and draw an egg-crate
	// lattice - mp_dumbo's mirrored curve is exactly that case); the fan
	// offsets step by the golden ratio so no two components are parallel; the
	// wavelength ladder is powers of 1/phi so the sum's repeat period is
	// pushed past anything a level holds.
	struct FWaveSet
	{
		FLinearColor W[8];
		float Gain = 0.f, Chop = 0.4f, BaseLen = 26.f;
		// The game's own foam controls, carried through from the sim entity.
		float FoamThreshold = 8.f, FoamMax = 1.f;
		// metres between grid vertices, so the shader can drop wave
		// components the geometry cannot represent
		float VertexSpacingM = 8.f;
		// Depth over which the swell dies as it reaches shallow water. The
		// game authors this per level (the ShoreFadeDistance the visual
		// environment can override); we have no reader for the value yet, so
		// this is calibration.
		float ShoreFadeM = 6.f;
		bool  bValid = false;
	};

	FWaveSet DeriveWaves(const BF6HP::FCore::FWaterSim& S, bool bOcean, float SizeM)
	{
		static const float Ratio[8] = { 1.0f, 0.618f, 0.382f, 0.236f,
		                                0.146f, 0.090f, 0.056f, 0.034f };
		FWaveSet R;
		for (int32 i = 0; i < 8; i++) R.W[i] = FLinearColor(1.f, 0.f, 0.f, Ratio[i]);

		TArray<FVector2D> Lobes;
		for (const FVector2D& pt : S.Dist)
			if (pt.Y > 0.001f) Lobes.Add(pt);
		Lobes.Sort([](const FVector2D& a, const FVector2D& b){ return a.Y > b.Y; });
		TArray<FVector2D> Picked;
		for (const FVector2D& l : Lobes)
		{
			bool bClash = false;
			for (const FVector2D& q : Picked)
			{
				const float d = FMath::Abs(q.X - l.X);
				if (FMath::Min(d, 1.f - d) < 0.06f) { bClash = true; break; }
			}
			if (!bClash) Picked.Add(l);
			if (Picked.Num() >= 4) break;
		}
		if (Picked.Num() == 0) return R;

		const float Top = FMath::Max(Picked[0].Y, 1e-4f);
		// KEEP THE DISPLACING COMPONENTS INSIDE ONE HALF-PLANE.
		//
		// The distribution is often a MIRRORED pair - two lobes half a turn
		// apart - because the authored spectrum is symmetric about the wind
		// axis. Two opposed trains of the same wavelength are a STANDING
		// wave: it does not travel, and it draws a regular grid. The Godot
		// reference fans each lobe to soften that, which works while eight
		// components are displacing; once the grid can only carry two or
		// three, a fan of two opposed rays is a lattice again, which is
		// exactly the repeating pattern this produced.
		//
		// So every direction is folded into the half-plane around the
		// dominant lobe. The spread survives, the opposition does not.
		const float LeadAng = S.WindAngle + (Picked[0].X - 0.5f) * 2.f * PI;
		const FVector2D Lead(FMath::Cos(LeadAng), FMath::Sin(LeadAng));
		int32 n = 0;
		// Ratio is a function-scope STATIC, so it has static storage duration
		// and must not be named in the capture list - it is reachable without
		// one. Capturing it is a hard error, not a warning.
		auto Fan = [&R, &n, &Lead](float BaseAng, float Amp)
		{
			const float Off = FMath::DegreesToRadians(
				46.f * (FMath::Fmod((float)n * 0.61803399f, 1.f) - 0.5f));
			float cx = FMath::Cos(BaseAng + Off), cy = FMath::Sin(BaseAng + Off);
			// fold into the leader's half-plane
			if (cx * Lead.X + cy * Lead.Y < 0.f) { cx = -cx; cy = -cy; }
			R.W[n] = FLinearColor(cx, cy, Amp, Ratio[n]);
			n++;
		};
		const int32 Per = FMath::Max(1, 8 / Picked.Num());
		for (int32 li = 0; li < Picked.Num() && n < 8; li++)
		{
			const float Base = S.WindAngle + (Picked[li].X - 0.5f) * 2.f * PI;
			for (int32 j = 0; j < Per && n < 8; j++)
				Fan(Base, (Picked[li].Y / Top) * FMath::Pow(0.72f, (float)j));
		}
		float Tail = R.W[FMath::Max(n - 1, 0)].B;
		while (n < 8)
		{
			Tail *= 0.72f;
			Fan(S.WindAngle + (Picked[0].X - 0.5f) * 2.f * PI, Tail);
		}

		// WindSpeed -> amplitude, in METRES for the ratio-1 component. Square
		// root, because the visible difference between 0.01 and 0.07 should
		// not be a factor of seven in wave height; 0.07 is the windy neutral.
		// AMPLITUDE, and why the first mapping produced a flat sea.
		//
		// WindSpeed is a normalised authoring scalar, not metres per second,
		// and on the multiplayer maps it barely moves: 0.01 on the calm ones
		// and 0.07 on the windy. Scaling a 0.35 m base by sqrt(0.01/0.07)
		// gave 14 cm waves on a ten kilometre sea, which is invisible.
		// Choppiness is the value that actually separates these maps (0.02 on
		// Aftermath against 0.60 on Tungsten) so it carries most of the
		// weight here, with wind speed as a multiplier on top.
		// WIND SPEED -> AMPLITUDE, AS A SQUARE LAW.
		//
		// This used to be sqrt(windSpeed / 0.07) clamped to a FLOOR of 0.55,
		// with a constant 0.45 added on top. Both were wrong in the same
		// direction. The header documents wind_speed as a normalised scalar
		// where 0.01 is calm, 0.07 is windy and 0.30 is a D-Day sea; a square
		// root compresses that whole range into a factor of five, and the
		// floor then guaranteed roughly a quarter metre of swell no matter how
		// calm the water was authored. MP_Aftermath is a CANAL authored at
		// wind_speed 0.010 and came out with two thirds of a metre of ocean
		// swell on it.
		//
		// A Phillips spectrum - the family the decoded dispersion kernel
		// belongs to - has wave height scaling with the SQUARE of wind speed,
		// so calm water is dramatically calmer than windy water rather than
		// slightly calmer. Note the honest limit here: findings/
		// ocean-fft-kernels-decoded.md establishes there is NO H0 kernel
		// anywhere in the GPU store, so the exact authored spectrum is not
		// recovered and this is the right physics rather than the game's
		// literal curve.
		// THE REFERENCE IS 0.5, NOT 0.07.
		//
		// The core's header used to document this scalar as "0.01 calm, 0.07
		// windy, 0.30 the D-Day sea", and that scale was read through the
		// SINGLE PLAYER reflection schema, which lays the class out
		// differently. Under the multiplayer schema the class DEFAULT is 0.5
		// and MP_Isolated authors 0.914. Calibrating against 0.07 therefore
		// squared a number about ten times too large and pinned every map to
		// the clamp, which is why the sea was a wall of four metre swell on
		// maps the game draws nearly flat.
		const float Vref = 0.5f;         // the class default: the authored middle
		const float AmpAtVref = 0.35f;   // metres for the ratio-1 component
		const float V = FMath::Max(S.WindSpeed, 0.f);
		float Amp = AmpAtVref * FMath::Square(V / Vref);
		// Choppiness sharpens crests. It must not MANUFACTURE height on water
		// the level authored as flat, so it scales the amplitude rather than
		// adding to it.
		Amp *= 0.6f + 0.8f * FMath::Clamp(S.Choppiness, 0.f, 1.f);

		// AND THE TILE BOUNDS IT. Wind speed alone does not decide wave height:
		// the simulation tile does too, because a swell longer than the tile
		// cannot exist in it. MP_Aftermath runs an 8 m tile and MP_Isolated a
		// 300 m one, and treating those two as differing only by wind is how a
		// canal ends up with ocean on it. Two percent of the tile is a
		// deliberate CALIBRATION rather than a decoded ratio - the authored
		// per-cascade displacement scale is a runtime value we do not read -
		// so it is one number to move if the sea reads wrong.
		if (S.TileDimension > 0.f)
		{
			Amp = FMath::Min(Amp, S.TileDimension * 0.02f);
		}
		R.Gain = FMath::Clamp(Amp, 0.002f, 4.0f);
		// Zero is meaningful. MP_Isolated explicitly authors 0.0: its FFT has
		// vertical waves but no horizontal fold. The old 0.05 floor invented
		// lateral displacement (and therefore crest foam) on that map.
		R.Chop = FMath::Clamp(S.Choppiness, 0.f, 0.9f);
		// SWELL SCALES WITH THE BODY OF WATER. A 10 km ocean does not carry
		// the same wave lengths as a 200 m lake, and a fixed 34 m made the
		// open sea look like a pond at the wrong scale.
		R.BaseLen = FMath::Clamp(SizeM * 0.010f, 18.f, 160.f);
		// Straight from the level: EnableFoam gates it, and a level that
		// authors FoamMaxValue 0 genuinely wants none.
		R.FoamThreshold = S.FoamThreshold;
		R.FoamMax = S.FoamMax > 0.f ? S.FoamMax : 0.f;
		R.bValid = true;
		return R;
	}

	// The coefficients, from the mined colours. Scattering IS the colour the
	// water body shows, scaled to per-metre; absorption is what the DEEP
	// colour is missing, spread over a fade depth. The ocean variant authors
	// one colour and no deep - the reference consumer darkens for depth, and
	// the same rule holds here.
	// Metres over which the authored colour is reached. The game carries this
	// per material (CB1[0].w); we have no reader for it yet, so it is one
	// number here rather than a guess dressed as data.
	// The terrain's heights as a texture, so the water surface can ask how
	// deep it is at any point. The game does the same thing through a
	// virtual-texture heightfield fetch.
	UTexture2D* GWaterDepthTex = nullptr;
	FVector2D   GWaterDepthMin = FVector2D::ZeroVector;   // world XZ of texel 0
	FVector2D   GWaterDepthSpan = FVector2D(1, 1);
	UTexture2D* GWaterHeightTex = nullptr;
	FVector2D   GWaterHeightMin = FVector2D::ZeroVector;
	FVector2D   GWaterHeightSpan = FVector2D(1, 1);
	FVector2D   GWaterHeightTexelM = FVector2D(1, 1);
	UTexture2DArray* GWaterMaskAtlas = nullptr;
	UTexture2D* GWaterMaskIndirection = nullptr;
	FVector2D GWaterMaskMin = FVector2D::ZeroVector;
	FVector2D GWaterMaskSpan = FVector2D(1, 1);
	FVector4f GWaterMaskMeta = FVector4f(1, 0, 1, 1); // interior,border,tile,indir side
	int32 GWaterMaskPageCount = 0;
	// The decoded mask is kept so the CPU can evaluate exactly what the shader
	// samples. Without it a flat sea can only be argued about; with it the
	// attenuation at a given world position is a number.
	BF6HP::FCore::FWaterMask GWaterMaskData;
	bool GWaterMaskDataValid = false;
	// Sheet-normal override. Negative means "no override", so the authored
	// decision stands. This has to be STICKY: the clipmap rebuilds tiles as the
	// camera moves and every new tile re-seeds its own parameters, so a value
	// written only to the materials that happen to exist right now survives
	// until the next tile is born and then quietly reverts.
	float GWaterSheetsOverride = -1.f;
	// Set when the spectrum is rebuilt, cleared once the surface has been
	// rebound with LIVE textures. A rebuild creates new textures and fills them
	// on the next evolve, so binding them in the same call binds empties and the
	// surface latches that: the change appears to do nothing until some later
	// parameter write re-dirties the material. Rebinding a tick later is the
	// order the data actually arrives in.
	int32 GWaterRebindPending = 0;

	UTexture2DArray* DefaultWaterMaskAtlas()
	{
		static UTexture2DArray* T = nullptr;
		if (T) return T;
		T = UTexture2DArray::CreateTransient(4, 4, 1, PF_G8);
		if (!T) return nullptr;
		T->SRGB = false;
		T->CompressionSettings = TC_Masks;
		T->AddressX = TA_Clamp;
		T->AddressY = TA_Clamp;
		T->Filter = TF_Bilinear;
		T->NeverStream = true;
		if (uint8* D = (uint8*)T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))
		{
			FMemory::Memzero(D, 16);
			T->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		T->UpdateResource();
		T->AddToRoot();
		return T;
	}

	bool MakeWaterMaskTextures(const BF6HP::FCore::FWaterMask& M)
	{
		if (M.Version != 1 || M.TileSide <= 0 || M.PageCount <= 0 ||
			M.IndirectionSide <= 0 ||
			M.AtlasR8.Num() != M.TileSide * M.TileSide * M.PageCount ||
			M.Indirection.Num() != M.IndirectionSide * M.IndirectionSide)
			return false;

		UTexture2DArray* Atlas = NewObject<UTexture2DArray>(
			GetTransientPackage(), NAME_None, RF_Transient);
		if (!Atlas) return false;
		Atlas->SetPlatformData(new FTexturePlatformData());
		Atlas->GetPlatformData()->SizeX = M.TileSide;
		Atlas->GetPlatformData()->SizeY = M.TileSide;
		Atlas->GetPlatformData()->SetNumSlices(M.PageCount);
		Atlas->GetPlatformData()->PixelFormat = PF_G8;
		Atlas->bNotOfflineProcessed = true;
		Atlas->SRGB = false;
		Atlas->CompressionSettings = TC_Masks;
		Atlas->AddressX = TA_Clamp;
		Atlas->AddressY = TA_Clamp;
		Atlas->Filter = TF_Bilinear;
		Atlas->NeverStream = true;
		FTexture2DMipMap* AtlasMip = new FTexture2DMipMap(
			M.TileSide, M.TileSide, M.PageCount);
		Atlas->GetPlatformData()->Mips.Add(AtlasMip);
		AtlasMip->BulkData.Lock(LOCK_READ_WRITE);
		void* AtlasDst = AtlasMip->BulkData.Realloc(M.AtlasR8.Num());
		FMemory::Memcpy(AtlasDst, M.AtlasR8.GetData(), M.AtlasR8.Num());
		AtlasMip->BulkData.Unlock();
		Atlas->UpdateResource();
		Atlas->AddToRoot();

		UTexture2D* Indirection = UTexture2D::CreateTransient(
			M.IndirectionSide, M.IndirectionSide, PF_B8G8R8A8, NAME_None);
		if (!Indirection)
		{
			Atlas->RemoveFromRoot();
			return false;
		}
		Indirection->SRGB = false;
		Indirection->CompressionSettings = TC_VectorDisplacementmap;
		Indirection->AddressX = TA_Clamp;
		Indirection->AddressY = TA_Clamp;
		Indirection->Filter = TF_Nearest;
		Indirection->NeverStream = true;
		uint8* D = (uint8*)Indirection->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
		if (!D)
		{
			Indirection->GetPlatformData()->Mips[0].BulkData.Unlock();
			Atlas->RemoveFromRoot();
			return false;
		}
		for (int32 i = 0; i < M.Indirection.Num(); ++i)
		{
			const uint32 V = M.Indirection[i];
			const uint16 Lo = (uint16)(V & 0xffffu);
			const bool bInline = (int16)Lo < 0;
			// PF_B8G8R8A8 memory order. Shader-visible RGBA is page, shift,
			// inline value, inline flag.
			D[i * 4 + 0] = bInline ? (uint8)(Lo & 0x7fffu) : 0;
			D[i * 4 + 1] = bInline ? 0 : (uint8)((V >> 16) & 31u);
			D[i * 4 + 2] = bInline ? 0 : (uint8)Lo;
			D[i * 4 + 3] = bInline ? 255 : 0;
		}
		Indirection->GetPlatformData()->Mips[0].BulkData.Unlock();
		Indirection->UpdateResource();
		Indirection->AddToRoot();

		if (GWaterMaskAtlas) GWaterMaskAtlas->RemoveFromRoot();
		if (GWaterMaskIndirection) GWaterMaskIndirection->RemoveFromRoot();
		GWaterMaskAtlas = Atlas;
		GWaterMaskIndirection = Indirection;
		GWaterMaskMin = M.BoundsMin;
		GWaterMaskSpan = M.BoundsMax - M.BoundsMin;
		GWaterMaskMeta = FVector4f((float)M.InteriorSide, (float)M.Border,
			(float)M.TileSide, (float)M.IndirectionSide);
		GWaterMaskPageCount = M.PageCount;
		GWaterMaskData = M;
		GWaterMaskDataValid = true;
		return true;
	}

	// CPU twin of the shader's CoarseMask lookup, following the same steps in
	// the same order so the number it returns is the number the surface uses.
	// Nearest rather than bilinear on the atlas: a probe wants the authored
	// texel, and bilinear would blur the very edge being investigated.
	float BF6_EvalCoarseMask(const FVector2D& PosM, bool& bOk)
	{
		bOk = false;
		const BF6HP::FCore::FWaterMask& M = GWaterMaskData;
		if (!GWaterMaskDataValid || M.TileSide <= 0 || M.IndirectionSide <= 0) return 1.f;
		const FVector2D Span(FMath::Max(M.BoundsMax.X - M.BoundsMin.X, 1e-4),
		                     FMath::Max(M.BoundsMax.Y - M.BoundsMin.Y, 1e-4));
		FVector2D Uv((PosM.X - M.BoundsMin.X) / Span.X, (PosM.Y - M.BoundsMin.Y) / Span.Y);
		Uv.X = FMath::Min(FMath::Clamp(Uv.X, 0.0, 1.0), 0.999999);
		Uv.Y = FMath::Min(FMath::Clamp(Uv.Y, 0.0, 1.0), 0.999999);

		const int32 CellX = FMath::Clamp((int32)FMath::FloorToInt(Uv.X * M.IndirectionSide),
			0, M.IndirectionSide - 1);
		const int32 CellY = FMath::Clamp((int32)FMath::FloorToInt(Uv.Y * M.IndirectionSide),
			0, M.IndirectionSide - 1);
		const uint32 V = M.Indirection[CellY * M.IndirectionSide + CellX];
		const uint16 Lo = (uint16)(V & 0xffffu);
		const bool bInline = ((int16)Lo) < 0;
		const int32 Page = bInline ? 0 : (int32)(uint8)Lo;
		const int32 Shift = bInline ? 0 : (int32)((V >> 16) & 31u);
		const float InlineValue = bInline ? (float)(uint8)(Lo & 0x7fffu) / 255.f : 0.f;

		float Value = InlineValue;
		if (!bInline)
		{
			const float MaskScale = (float)M.IndirectionSide / FMath::Pow(2.f, (float)Shift);
			const float LocalX = FMath::Frac((float)Uv.X * MaskScale);
			const float LocalY = FMath::Frac((float)Uv.Y * MaskScale);
			const float AtlasU = (LocalX * (float)M.InteriorSide + (float)M.Border + 0.5f)
				/ (float)M.TileSide;
			const float AtlasV = (LocalY * (float)M.InteriorSide + (float)M.Border + 0.5f)
				/ (float)M.TileSide;
			const int32 Px = FMath::Clamp((int32)(AtlasU * M.TileSide), 0, M.TileSide - 1);
			const int32 Py = FMath::Clamp((int32)(AtlasV * M.TileSide), 0, M.TileSide - 1);
			const int64 Index = (int64)Page * M.TileSide * M.TileSide
				+ (int64)Py * M.TileSide + Px;
			if (!M.AtlasR8.IsValidIndex((int32)Index)) return 1.f;
			Value = (float)M.AtlasR8[(int32)Index] / 255.f;
		}
		bOk = true;
		return FMath::Clamp(1.f - Value, 0.f, 1.f);   // the selected 1-mask law
	}

	UTexture2D* MakeHeightTexture(const BF6HP::FCore::FTerrain& T,
		FVector2D& OutMin, FVector2D& OutSpan, FVector2D* OutTexelM,
		int32 MaxSide)
	{
		if (T.Size <= 1 || T.Heights.Num() < T.Size * T.Size) return nullptr;
		const int32 N = FMath::Clamp(T.Size / 4, 64, MaxSide);
		UTexture2D* Tex = UTexture2D::CreateTransient(N, N, PF_R32_FLOAT);
		if (!Tex) return nullptr;
		Tex->SRGB = false;
		Tex->CompressionSettings = TC_HDR;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		Tex->Filter = TF_Bilinear;
		const double YScale = T.HeightScale > 0.f
			? (double)T.HeightScale / 65536.0
			: FMath::Max(0.001, T.WorldMax.Y - T.WorldMin.Y) / 65535.0;
		if (float* P = (float*)Tex->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))
		{
			for (int32 y = 0; y < N; y++)
				for (int32 x = 0; x < N; x++)
				{
					const int32 sx = FMath::Min(T.Size - 1, x * T.Size / N);
					const int32 sy = FMath::Min(T.Size - 1, y * T.Size / N);
					// Both block 0 and block 2 decode as an absolute height
					// through zero. WorldMin.Y is an AABB bound, not a bias.
					P[y * N + x] = (float)(T.Heights[sy * T.Size + sx] * YScale);
				}
			Tex->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		Tex->UpdateResource();
		Tex->AddToRoot();
		OutMin = FVector2D(T.WorldMin.X, T.WorldMin.Z);
		OutSpan = FVector2D(
			FMath::Max(1.0, T.WorldMax.X - T.WorldMin.X),
			FMath::Max(1.0, T.WorldMax.Z - T.WorldMin.Z));
		if (OutTexelM) *OutTexelM = OutSpan / (double)N;
		return Tex;
	}

	UTexture2D* MakeDepthTexture(const BF6HP::FCore::FTerrain& T)
	{
		// Terrain only drives shore/depth attenuation, so 1024 is sufficient.
		return MakeHeightTexture(T, GWaterDepthMin, GWaterDepthSpan, nullptr, 1024);
	}

	UTexture2D* MakeWaterHeightTexture(const BF6HP::FCore::FTerrain& T)
	{
		// This drives geometry. Keep twice the terrain-fade resolution while
		// still avoiding a permanent 8193-square CPU/GPU texture in the lab.
		return MakeHeightTexture(T, GWaterHeightMin, GWaterHeightSpan,
			&GWaterHeightTexelM, 2048);
	}

	// Per-metre strengths for Unreal's two water coefficients. The HUE and
	// the BRIGHTNESS below come from the level's own authored colour; these
	// numbers are CALIBRATION and are labelled so nobody later mistakes them
	// for something recovered from the game.
	//
	// SCATTERING IS SMALL. This was 0.60 per metre, and half a unit of
	// scattering per metre is roughly what milk does: the sea came out
	// opaque and white. Sea water scatters about a tenth of that. The
	// brightness of tropical water is not the volume glowing, it is the
	// BOTTOM showing through - absorption kills red over a couple of metres
	// while blue-green survives twenty, and what comes back up is the sand
	// seen through a blue filter. So scattering stays low and absorption
	// carries the colour.
	// Tuned against the two failure modes actually seen, not from a table.
	// At 0.60 the water was MILK - too much light coming back, and coming back
	// too evenly across the channels. At 0.055 it was nearly BLACK, because a
	// few centimetres of scattering per metre returns almost nothing. This
	// sits between them, and the hue is squared where it is applied below so
	// that raising the strength does not drag the colour back toward white.
	float ScatterPerM = 0.22f;
	float AbsorbPerM  = 0.50f;
	// The surface's own diffuse albedo. Water is a REFLECTOR, not a diffuser:
	// this belongs near black. It was near-black once, produced black water,
	// and was then pushed almost to white, which produced the milk. The
	// actual answer was never the base colour - it was the scattering.
	float SurfaceAlbedo = 0.06f;

	// WAVE ENTITIES - the driver for the sixteen radial swell slots.
	//
	// The game does not ship these in data. A census over all 23,106 mounted
	// partitions on mp_isolated finds ZERO authored WaterInteractWaveEntityData
	// instances, so nothing a data walk can reach fills them. They are built at
	// runtime, where a wave IS an owner entity's position plus two animated
	// floats: the per-frame dispatch collects up to sixteen objects whose radius
	// is at least 1e-6 and packs X/Z, 1/radius and amplitude*0.5 into cb0
	// (findings water-wave-entities-render-contract-and-negative-census and
	// wave-entities-are-scheduler-animated-properties). The slots therefore
	// exist to be DRIVEN, and this is the driver.
	//
	// Everything except the values themselves is the game's own arithmetic: the
	// selection threshold, the sixteen cap, the packing, and the disabled
	// sentinel below are the dispatcher's. Only the SOURCE differs - the game's
	// come from timeline bindings addressed by property id, these come from
	// whoever is building the map - because no asset carries them to copy.
	//
	// NOT A CLAIM ABOUT TSURU. That the offshore rollers seen in play arrive
	// through these slots is an elimination (the vertex shader has exactly two
	// displacement sources and the FFT one is authored to millimetres), not a
	// recovered fact. The runtime raising the sim wind instead would look the
	// same from here. Do not present author-placed waves as the game's.
	struct FBF6WaveEntity
	{
		FVector2D CentreM    = FVector2D::ZeroVector;   // world XY, metres
		float     RadiusM    = 0.f;
		float     AmplitudeM = 0.f;                     // crest height, metres
	};

	static TArray<FBF6WaveEntity> GWaveEntities;

	// The game's own disabled slot. t = min(d * 1e10, 1) reaches 1 immediately,
	// so the mound is zero AND the cascade dampening product is left at one. A
	// zeroed slot would NOT be inert: it would collapse the product and flatten
	// every cascade, so the sentinel matters.
	static const FLinearColor kBF6WaveSlotDisabled(0.f, 0.f, 1.0e10f, 0.f);

	// Position and radius ride the vector slot; the half amplitude is a separate
	// scalar because a vector parameter reaches the custom node as float3.
	static int32 BF6_PackWaveEntities(FLinearColor (&Out)[16], float (&Amp)[16])
	{
		for (int32 i = 0; i < 16; ++i) { Out[i] = kBF6WaveSlotDisabled; Amp[i] = 0.f; }
		int32 Packed = 0;
		for (const FBF6WaveEntity& E : GWaveEntities)
		{
			if (Packed >= 16) break;              // the dispatcher's own cap
			if (E.RadiusM < 1e-6f) continue;      // the dispatcher's own threshold
			Out[Packed] = FLinearColor(E.CentreM.X, E.CentreM.Y, 1.f / E.RadiusM, 0.f);
			Amp[Packed] = E.AmplitudeM * 0.5f;
			++Packed;
		}
		return Packed;
	}

	static void BF6_ApplyWaveEntities(UMaterialInstanceDynamic* MID,
		const FLinearColor (&Slots)[16], const float (&Amp)[16])
	{
		if (!MID) return;
		for (int32 i = 0; i < 16; ++i)
		{
			MID->SetVectorParameterValue(
				*FString::Printf(TEXT("BF6WaveEntity%d"), i), Slots[i]);
			MID->SetScalarParameterValue(
				*FString::Printf(TEXT("BF6WaveAmp%d"), i), Amp[i]);
		}
	}

	UMaterialInstanceDynamic* WaterMaterialFor(UObject* Outer, const BF6HP::FCore::FWater& W,
	                                           const FWaveSet* Waves,
	                                           BF6HP::FCore* SourceCore = nullptr,
	                                           BF6HP::FWaterFFT* SourceFFT = nullptr)
	{
		UMaterial* Parent = EnsureWaterMaterial();
		if (!Parent) return nullptr;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return nullptr;
		MID->SetFlags(RF_Transient);
		// A decoded ocean composite is also a simulated surface.  Aftermath is
		// the control that disproves using only WaterSurfaceEntityData::bOcean:
		// its surface does not set that flag, but it resolves an ocean composite
		// and the level owns two WaterCascadeParams records.  Local reconstructed
		// pools have neither signal and therefore remain flat.
		BF6HP::FWaterFFT* WaterFFT = SourceFFT ? SourceFFT : GWaterFFT.Get();
		BF6HP::FCore& WaterCore = SourceCore ? *SourceCore : GCore;
		const bool bSimulatedSurface = W.bOcean || W.OceanComponentVersion == 1;
		if (bSimulatedSurface && WaterFFT && WaterFFT->IsReady())
			WaterFFT->Bind(MID, W.WaveAmplitudeScale);
		// Serac:WaterCascadeParams.WaterOrigin.xz. The decoded TileOffset is
		// zero on every measured surface, so the rectangular surface centre is
		// the exact XZ origin available from the live entity read path.
		MID->SetVectorParameterValue(TEXT("BF6WaterOrigin"), FLinearColor(
			(float)W.Center.X, (float)W.Center.Y, W.Height, 0.f));
		MID->SetVectorParameterValue(TEXT("BF6WaterSourceWorld"),
			FLinearColor(0.f, 0.f, W.Height, 0.f));
		BF6WaterShared::BindWaterHeightfield(MID);
		const bool bUseCoarseMask = W.AttenuationType == 2 &&
			GWaterMaskAtlas && GWaterMaskIndirection;
		MID->SetScalarParameterValue(TEXT("BF6WaterMaskAvailable"),
			bUseCoarseMask ? 1.f : 0.f);
		if (bUseCoarseMask)
		{
			MID->SetTextureParameterValue(TEXT("BF6WaterMaskAtlas"), GWaterMaskAtlas);
			MID->SetTextureParameterValue(TEXT("BF6WaterMaskIndirection"), GWaterMaskIndirection);
			MID->SetVectorParameterValue(TEXT("BF6WaterMaskMin"), FLinearColor(
				(float)GWaterMaskMin.X, (float)GWaterMaskMin.Y, 0.f, 0.f));
			MID->SetVectorParameterValue(TEXT("BF6WaterMaskSpan"), FLinearColor(
				(float)GWaterMaskSpan.X, (float)GWaterMaskSpan.Y, 1.f, 1.f));
			MID->SetVectorParameterValue(TEXT("BF6WaterMaskMeta"), FLinearColor(
				GWaterMaskMeta.X, GWaterMaskMeta.Y,
				GWaterMaskMeta.Z, GWaterMaskMeta.W));
			MID->SetScalarParameterValue(TEXT("BF6WaterMaskIndirectionSide"),
				GWaterMaskMeta.W);
		}
		const bool bUseOverlap = bSimulatedSurface && W.CascadeOverlapVersion == 1 &&
			W.bCascadeOverlapEnabled;
		const float OverlapValue = GWaterOverlapOverride >= 0
			? (float)FMath::Clamp(GWaterOverlapOverride, 0, 1)
			: (bUseOverlap ? 1.f : 0.f);
		MID->SetScalarParameterValue(TEXT("BF6CascadeOverlapEnabled"), OverlapValue);
		MID->SetVectorParameterValue(TEXT("BF6CascadeOverlapParams"), FLinearColor(
			W.CascadeOverlapParams.X, W.CascadeOverlapParams.Y,
			W.CascadeOverlapParams.Z, W.CascadeOverlapParams.W));
		MID->SetVectorParameterValue(TEXT("BF6CascadeOverlapParams2"), FLinearColor(
			W.CascadeOverlapParams2.X, W.CascadeOverlapParams2.Y,
			W.CascadeOverlapParams2.Z, W.CascadeOverlapParams2.W));
		MID->SetScalarParameterValue(TEXT("BF6CascadeOverlapHeightScale"),
			W.CascadeOverlapHeightScale > 0.f ? W.CascadeOverlapHeightScale : 1.f);
		MID->SetScalarParameterValue(TEXT("BF6CascadeOverlapShearY"), W.CascadeOverlapParams.W);
		MID->SetScalarParameterValue(TEXT("BF6CascadeOverlapSizeB"), W.CascadeOverlapParams2.W);
		// Recovered deferred composite: water F0 is hard-coded 0.0256.
		// The material byte previously called ReflectanceLow is an opacity /
		// turbidity endpoint, not Fresnel reflectance. Unreal stores F0 as
		// Specular * 0.08, hence 0.32 exactly.
		MID->SetScalarParameterValue(TEXT("WaterSpecular"), 0.0256f / 0.08f);
		if (W.OceanComponentVersion == 1)
		{
			MID->SetScalarParameterValue(TEXT("FoamDepthRampDistance"), W.FoamDepthRampM);
			MID->SetVectorParameterValue(TEXT("CompositeFoamTint"), W.CompositeFoamTint);
			// The translated draw graph is already normalized foam coverage.  A second
			// 0.75 multiplier had no BF6 source and is therefore removed.
			MID->SetScalarParameterValue(TEXT("FoamCoverage"), 1.f);
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water composite: live VE %s candidates=%d IOR=%.3f opacityRamp=%.3fm ")
				TEXT("foamRamp=%.3fm g=%.3f shadow=%.3f transmission=(%.4f %.4f %.4f) ")
				TEXT("foamTint=(%.4f %.4f %.4f) smooth=%.3f"),
				*W.OceanPreset, W.OceanPresetCandidates, W.CompositeIor,
				W.OpacityRampM, W.FoamDepthRampM, W.ScatterPhaseG,
				W.ScatterShadowInfluence, W.TransmissionColour.R,
				W.TransmissionColour.G, W.TransmissionColour.B,
				W.CompositeFoamTint.R, W.CompositeFoamTint.G,
				W.CompositeFoamTint.B, W.CompositeFoamSmoothness);
		}
		UTexture2D* MicroSheetTex = TextureFor(WaterCore, W.DetailNormal);
		UTexture2D* FoamSheetTex = TextureFor(WaterCore, W.FoamNormal);
		UTexture2D* ContactFoamTex = TextureFor(WaterCore, W.ContactFoam);
		UTexture2D* BroadPatternTex = TextureFor(WaterCore, W.FoamRgb2);
		UTexture2D* OceanNoiseTex = TextureFor(WaterCore, W.Noise);
		const bool bHaveDrawSheets = MicroSheetTex && FoamSheetTex;
		if (MicroSheetTex) MID->SetTextureParameterValue(TEXT("MicroSheet"), MicroSheetTex);
		if (FoamSheetTex) MID->SetTextureParameterValue(TEXT("FoamSheet"), FoamSheetTex);
		if (ContactFoamTex) MID->SetTextureParameterValue(TEXT("ContactFoam"), ContactFoamTex);
		if (BroadPatternTex) MID->SetTextureParameterValue(TEXT("BroadPattern"), BroadPatternTex);
		if (OceanNoiseTex) MID->SetTextureParameterValue(TEXT("OceanNoise"), OceanNoiseTex);
		MID->SetScalarParameterValue(TEXT("UseDetailSheets"),
			GWaterSheetsOverride >= 0.f
				? GWaterSheetsOverride
				: (bHaveDrawSheets && GWaterUseFastSheetDetail ? 1.f : 0.f));
		MID->SetScalarParameterValue(TEXT("UseContactFoam"), ContactFoamTex ? 1.f : 0.f);
		const bool bHaveBroadPattern = BroadPatternTex && OceanNoiseTex && W.ExtendedGraphVersion == 1;
		MID->SetScalarParameterValue(TEXT("UseBroadPattern"), bHaveBroadPattern ? 1.f : 0.f);
		// Aftermath is the measured counterexample to "ocean FFT == complete
		// water": its H0 resolves to micrometres while the selected draw pass has
		// interactive displacement bindings. Until libbf6 exposes the atlas, turn
		// on the explicitly provisional eWave modal seed only where no mounted
		// broad draw field exists. This avoids double-displacing Tsuru.
		const bool bUseInteractiveBridge = bSimulatedSurface && !bHaveBroadPattern;
		MID->SetScalarParameterValue(TEXT("BF6InteractiveBridgeEnabled"),
			bUseInteractiveBridge ? 1.f : 0.f);
		MID->SetScalarParameterValue(TEXT("BF6InteractiveBridgeHeightM"),
			bUseInteractiveBridge ? GWaterFoamWaveHeightM : 0.f);
		MID->SetScalarParameterValue(TEXT("BF6InteractiveBridgeLengthM"), 120.f);
		MID->SetScalarParameterValue(TEXT("BF6InteractiveBridgeRate"), 0.35f);
		MID->SetScalarParameterValue(TEXT("BF6FoamWaveHeightM"),
			bHaveBroadPattern ? GWaterFoamWaveHeightM : 0.f);
		MID->SetScalarParameterValue(TEXT("BF6BroadCarrierSize"), GWaterBroadCarrierSize);
		MID->SetScalarParameterValue(TEXT("BF6BroadTimeScale"), GWaterBroadTimeScale);
		MID->SetScalarParameterValue(TEXT("BF6FoamCrestStart"), GWaterFoamCrestStart);
		MID->SetScalarParameterValue(TEXT("BF6FoamCrestFull"), GWaterFoamCrestFull);
		MID->SetScalarParameterValue(TEXT("ExtendedGraphVersion"), (float)W.ExtendedGraphVersion);
		for (int32 r = 0; r < 22; ++r)
		{
			const FVector4f& G = W.ExtendedCb1[r];
			MID->SetVectorParameterValue(*FString::Printf(TEXT("BF6ExtendedCb1_%d"), r),
				FLinearColor(G.X, G.Y, G.Z, G.W));
			MID->SetScalarParameterValue(*FString::Printf(TEXT("BF6ExtendedCb1W_%d"), r), G.W);
		}
		// Extended graph constants are live ShaderBlockDepot reads. Defaults in
		// the parent material exist only so an older core remains loadable.
		if (W.ContactWorldDivisorM > 0.f)
			MID->SetScalarParameterValue(TEXT("ContactWorldDivisorM"), W.ContactWorldDivisorM);
		if (W.ContactRemapLow >= 0.f)
			MID->SetScalarParameterValue(TEXT("ContactRemapLow"), W.ContactRemapLow);
		if (W.ContactGain >= 0.f)
			MID->SetScalarParameterValue(TEXT("ContactGain"), W.ContactGain);
		if (W.FoamCompositeLow >= 0.f)
			MID->SetScalarParameterValue(TEXT("FoamCompositeLow"), W.FoamCompositeLow);
		if (W.FoamCompositeHigh >= 0.f)
			MID->SetScalarParameterValue(TEXT("FoamCompositeHigh"), W.FoamCompositeHigh);
		if (W.BroadPatternWorldMul >= 0.f)
			MID->SetScalarParameterValue(TEXT("BroadPatternWorldMul"), W.BroadPatternWorldMul);
		if (W.BroadPatternWorldScale > 0.f)
			MID->SetScalarParameterValue(TEXT("BroadPatternWorldScale"), W.BroadPatternWorldScale);
		if (W.BroadPatternFloor >= 0.f)
			MID->SetScalarParameterValue(TEXT("BroadPatternFloor"), W.BroadPatternFloor);
		if (W.MicroSheetUvScale >= 0.f)
			MID->SetScalarParameterValue(TEXT("MicroSheetUvScale"), W.MicroSheetUvScale);
		if (W.FoamSheetUvScale >= 0.f)
			MID->SetScalarParameterValue(TEXT("FoamSheetUvScale"), W.FoamSheetUvScale);
		if (W.MicroSheetFlowSpeed >= 0.f)
			MID->SetScalarParameterValue(TEXT("MicroSheetFlowSpeed"), W.MicroSheetFlowSpeed);
		// The old sinusoidal surf band was a visual fallback, not a recovered
		// game path. The sparse contact texture is the authored shoreline input.
		MID->SetScalarParameterValue(TEXT("UseShoreSurfFallback"), 0.f);
		MID->SetVectorParameterValue(TEXT("WaterMin"), FLinearColor(
			(float)(W.Center.X - W.Size.X * 0.5),
			(float)(W.Center.Y - W.Size.Y * 0.5), 0.f, 0.f));
		MID->SetVectorParameterValue(TEXT("WaterSpan"), FLinearColor(
			(float)FMath::Max(W.Size.X, 1.0), (float)FMath::Max(W.Size.Y, 1.0), 1.f, 1.f));
		if (W.FoamNormalStrength >= 0.f)
			MID->SetScalarParameterValue(TEXT("FoamSheetNormalStrength"), W.FoamNormalStrength);
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("water draw sheets: detail id %d %s, foam id %d %s, contact id %d %s; real %d/3 bound, ")
			TEXT("absent-id control 0/1 bound; microUV %.6g foamUV %.6g flow %.6g ")
			TEXT("normal %.6g/%.6g composite %.6g..%.6g contact %.6g/%.6g/%.6g; ")
			TEXT("broad id %d %s noise id %d %s graph v%u use=%d"),
			W.DetailNormal, MicroSheetTex ? TEXT("bound") : TEXT("missing"),
			W.FoamNormal, FoamSheetTex ? TEXT("bound") : TEXT("missing"),
			W.ContactFoam, ContactFoamTex ? TEXT("bound") : TEXT("missing"),
			(MicroSheetTex ? 1 : 0) + (FoamSheetTex ? 1 : 0) + (ContactFoamTex ? 1 : 0),
			W.MicroSheetUvScale, W.FoamSheetUvScale, W.MicroSheetFlowSpeed,
			W.MicroNormalStrength, W.FoamNormalStrength,
			W.FoamCompositeLow, W.FoamCompositeHigh,
			W.ContactWorldDivisorM, W.ContactRemapLow, W.ContactGain,
			W.FoamRgb2, BroadPatternTex ? TEXT("bound") : TEXT("missing"),
			W.Noise, OceanNoiseTex ? TEXT("bound") : TEXT("missing"),
			W.ExtendedGraphVersion, bHaveBroadPattern ? 1 : 0);
		// Seed this tile from the live wave-entity state. Tiles are rebuilt as the
		// camera moves, so a tile created after the waves were set would otherwise
		// come up on the parameter defaults and read as the waves ending at a tile
		// boundary.
		{
			FLinearColor WaveSlots[16];
			float WaveAmps[16];
			BF6_PackWaveEntities(WaveSlots, WaveAmps);
			BF6_ApplyWaveEntities(MID, WaveSlots, WaveAmps);
		}
		MID->SetScalarParameterValue(TEXT("BF6DepthAvailable"), GWaterDepthTex ? 1.f : 0.f);
		if (GWaterDepthTex)
		{
			MID->SetTextureParameterValue(TEXT("DepthTex"), GWaterDepthTex);
			MID->SetVectorParameterValue(TEXT("DepthMin"),
				FLinearColor((float)GWaterDepthMin.X, (float)GWaterDepthMin.Y, 0.f, 0.f));
			MID->SetVectorParameterValue(TEXT("DepthSpan"),
				FLinearColor((float)GWaterDepthSpan.X, (float)GWaterDepthSpan.Y, 1.f, 1.f));
		}
		if (Waves && Waves->bValid)
		{
			for (int32 i = 0; i < 8; i++)
				MID->SetVectorParameterValue(*FString::Printf(TEXT("Wave%d"), i), Waves->W[i]);
			MID->SetScalarParameterValue(TEXT("WaveGain"), Waves->Gain);
			MID->SetScalarParameterValue(TEXT("WaveChop"), Waves->Chop);
			MID->SetScalarParameterValue(TEXT("WaveBaseLen"), Waves->BaseLen);
			MID->SetScalarParameterValue(TEXT("FoamThreshold"), Waves->FoamThreshold);
			MID->SetScalarParameterValue(TEXT("FoamMax"), Waves->FoamMax);
			MID->SetScalarParameterValue(TEXT("MinDispLen"), Waves->VertexSpacingM * 3.f);
			MID->SetScalarParameterValue(TEXT("ShoreFadeDistance"), Waves->ShoreFadeM);
		}
		if (W.bShoreFadeValid && W.ShoreDepthM > 0.f)
		{
			MID->SetScalarParameterValue(TEXT("UseAuthoredShoreCurve"), 1.f);
			MID->SetScalarParameterValue(TEXT("ShoreFadeDistance"), W.ShoreDepthM);
			MID->SetScalarParameterValue(TEXT("AdditionalWaterDepth"), W.AdditionalWaterDepthM);
			MID->SetVectorParameterValue(TEXT("ShoreBlend"), FLinearColor(
				W.ShoreBlend.X, W.ShoreBlend.Y, W.ShoreBlend.Z, W.ShoreBlend.W));
		}
		else
		{
			MID->SetScalarParameterValue(TEXT("UseAuthoredShoreCurve"), 0.f);
			MID->SetScalarParameterValue(TEXT("AdditionalWaterDepth"), 0.f);
			if (W.AttenuationType >= 0 && W.AttenuationType <= 1)
			{
				// None, or ShoreDepth authored as 0 (mp_tungsten): the game's
				// own gate is ShoreDepth > 0, so zero means shore attenuation
				// deliberately OFF (finding shoredepth-zero-is-a-shader-gate).
				// Without this bind the parameter's 6 m default fabricated a
				// fade the game turned off, flattening the waterline.
				MID->SetScalarParameterValue(TEXT("ShoreFadeDistance"), 0.f);
			}
			if (bUseCoarseMask)
			{
				UE_LOG(LogBF6HighPoly, Display,
					TEXT("water shore: CoarseMask uses exact live block10 utility raster: ")
					TEXT("atlas %.0fx%.0fx%d, indirection %.0fx%.0f, bounds ")
					TEXT("(%.1f %.1f)+(%.1f %.1f)m, selected law 1-mask"),
					GWaterMaskMeta.Z, GWaterMaskMeta.Z,
					GWaterMaskPageCount,
					GWaterMaskMeta.W, GWaterMaskMeta.W,
					GWaterMaskMin.X, GWaterMaskMin.Y,
					GWaterMaskSpan.X, GWaterMaskSpan.Y);
			}
			else if (W.AttenuationType >= 2)
			{
				UE_LOG(LogBF6HighPoly, Display,
					TEXT("water shore: attenuation type %d needs a utility raster that is not bound; ")
					TEXT("using terrain-depth smoothstep as an explicit fallback"),
					W.AttenuationType);
			}
		}
		if (W.ShoreFoamSuppression >= 0.f)
			MID->SetScalarParameterValue(TEXT("ShoreFoamSuppression"), W.ShoreFoamSuppression);
		if (W.DrawFoamThreshold >= 0.f)
			MID->SetScalarParameterValue(TEXT("DrawFoamThreshold"), W.DrawFoamThreshold);
		{
			bool bHaveWeights = true;
			for (int32 k = 0; k < 4; ++k) bHaveWeights &= (W.CascadeFoamWeight[k] >= 0.f);
			if (bHaveWeights)
			{
				for (int32 k = 0; k < 4; ++k)
					MID->SetScalarParameterValue(*FString::Printf(TEXT("BF6FoamW%d"), k),
						W.CascadeFoamWeight[k]);
				MID->SetScalarParameterValue(TEXT("BF6FoamChain"), 1.f);
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("water foam: cascade chain ON, weights (%.3f %.3f %.3f %.3f), draw threshold %.3f, shore suppression %.3f, contrast %.3f"),
					W.CascadeFoamWeight[0], W.CascadeFoamWeight[1], W.CascadeFoamWeight[2],
					W.CascadeFoamWeight[3], W.DrawFoamThreshold, W.ShoreFoamSuppression, W.FoamContrast);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("water foam: level authors no cascade foam weights; Gerstner crest retained"));
			}
		}
		if (W.FoamContrast > 0.f)
			MID->SetScalarParameterValue(TEXT("FoamContrast"), W.FoamContrast);
		if (W.SmoothnessBias >= 0.f)
			MID->SetScalarParameterValue(TEXT("WaterRoughness"),
				FMath::Clamp(1.f - W.SmoothnessBias, 0.01f, 0.99f));
		if (W.SmoothnessFullFoam >= 0.f)
			MID->SetScalarParameterValue(TEXT("FoamRoughness"),
				FMath::Clamp(1.f - W.SmoothnessFullFoam, 0.01f, 0.99f));
		else if (W.SmoothnessBias >= 0.f)
			MID->SetScalarParameterValue(TEXT("FoamRoughness"),
				FMath::Clamp(1.f - W.SmoothnessBias, 0.01f, 0.99f));
		// cb54.w belongs to the deferred foam pass and therefore overrides the
		// draw material's smoothness when the active OceanComponentData exists.
		if (W.OceanComponentVersion == 1 && W.CompositeFoamRoughness >= 0.f)
			MID->SetScalarParameterValue(TEXT("FoamRoughness"),
				FMath::Clamp(W.CompositeFoamRoughness, 0.f, 1.f));
		if (W.MicroNormalStrength >= 0.f)
			MID->SetScalarParameterValue(TEXT("DetailRipple"), W.MicroNormalStrength);
		if (W.DetailFadeStartM >= 0.f && W.DetailFadeEndM > W.DetailFadeStartM)
		{
			MID->SetScalarParameterValue(TEXT("DetailFadeStart"), W.DetailFadeStartM);
			MID->SetScalarParameterValue(TEXT("DetailFadeEnd"), W.DetailFadeEndM);
		}
		// THE GAME'S OWN COLOUR CONVERSION.
		//
		// The recovered water pixel shader computes its albedo as
		//     saturate(1 + ln(c) / d)
		// which is an inverse Beer-Lambert: c is the colour the water should
		// REACH after d metres, and the expression recovers the per-unit
		// transmission. For Single Layer Water the quantity wanted is the
		// EXTINCTION, which is the same log over the same distance without
		// the 1 + : -ln(c)/d. An earlier version of this function invented a
		// mapping from the deep colour instead and produced neutral grey,
		// because taking logs of three similar dark numbers gives three
		// similar large numbers - and equal absorption in RGB is, by
		// definition, grey.
		//
		// Water absorbs red far more than blue, so a correct extinction is
		// strongly UNEQUAL across the channels. Aftermath's authored colour
		// (0, 0.076, 0.082) gives roughly (0.92, 0.26, 0.25) at d = 10: red
		// gone in a metre, blue-green carrying, which is what a sea looks
		// like.
		// WHAT THE AUTHORED COLOUR ACTUALLY CARRIES.
		//
		// The recovered pixel shader turns it into an albedo with
		// saturate(1 + ln(c)/d), where d is a per-material absorption
		// distance the game keeps at CB1[0].w. We have no reader for d yet,
		// and a single fixed d cannot serve every map: at d = 1 Tsuru Reef's
		// bright (0.569, 0.829, 0.890) gives the vibrant (0.44, 0.81, 0.88)
		// the map is known for, while Aftermath's much darker
		// (0, 0.076, 0.082) collapses to black.
		//
		// So the colour is split into the two things it is really telling us,
		// both of which survive without knowing d:
		//   HUE        - which way the water leans, and
		//   BRIGHTNESS - how much light comes back out of it.
		// Hue drives the scattering colour and the absorption's complement;
		// brightness drives how strongly the volume scatters. That keeps a
		// reef turquoise and a canal dark green without inventing a distance
		// for either.
		FLinearColor C = W.Shallow.R >= 0.f ? W.Shallow : FLinearColor(0.10f, 0.45f, 0.55f);
		const float Peak = FMath::Max3(C.R, FMath::Max(C.G, 0.f), FMath::Max(C.B, 0.f));
		const FLinearColor Hue = Peak > 1e-4f
			? FLinearColor(C.R / Peak, C.G / Peak, C.B / Peak)
			: FLinearColor(0.2f, 0.9f, 1.0f);
		// Square root, because the authored brightness spans two orders of
		// magnitude and a linear mapping makes every dark water pitch black.
		const float Bright = FMath::Sqrt(FMath::Clamp(Peak, 0.f, 1.f));

		// A dark, tinted surface albedo. The colour a viewer sees comes from
		// the volume and from whatever is under the water, not from this.
		if (W.SurfaceColour.R >= 0.f)
			MID->SetVectorParameterValue(TEXT("SurfaceTint"), W.SurfaceColour);
		else
			MID->SetVectorParameterValue(TEXT("SurfaceTint"), FLinearColor(
				Hue.R * SurfaceAlbedo, Hue.G * SurfaceAlbedo, Hue.B * SurfaceAlbedo));

		// SCATTERING is what comes back out: the water's own colour, at a
		// strength set by how bright the map authored it.
		//
		// The hue is SQUARED here. A reef's authored colour is only mildly
		// tinted - Tsuru's (0.569, 0.829, 0.890) normalises to a hue of
		// (0.64, 0.93, 1.00), which is very nearly white - so scattering along
		// it at any strength high enough to read as vibrant also reads as
		// milk. Squaring pulls it to (0.41, 0.87, 1.00) and lets the strength
		// go up while the colour goes toward blue instead of toward white.
		const FLinearColor Sat(Hue.R * Hue.R, Hue.G * Hue.G, Hue.B * Hue.B);

		// PER CENTIMETRE, NOT PER METRE.
		//
		// Single Layer Water computes OpticalDepth = ExtinctionCoeff *
		// WaterVolumeDepth, and WaterVolumeDepth is a scene depth difference
		// in Unreal units - centimetres. Feeding it a per-metre coefficient
		// makes the water a HUNDRED TIMES too opaque, which is not a subtle
		// error: at 0.22 per centimetre a mere ten centimetres of water has an
		// optical depth of 2.2 and you cannot see the sand through it.
		//
		// This is what every previous water attempt was actually fighting.
		// 0.60 read as milk (opaque, scattering a lot) and 0.055 read as
		// near-black (opaque, scattering little) - both opaque, because both
		// were a hundredfold too strong, and no amount of colour tuning was
		// ever going to fix either.
		const float ToPerCm = 0.01f;

		// THE DECODED EXTINCTION, WHERE THE LEVEL AUTHORS ONE.
		//
		// Everything below this block is the fallback heuristic: it treats the
		// authored colour as a hue plus a brightness and pushes them through
		// two constants. It is wrong twice over, and the second way is worse.
		//
		// On MP_Isolated the decode gives extinction per metre of
		//     R 0.28208   G 0.09379   B 0.05826
		// which is half transmittance at 2.46 m, 7.39 m and 11.90 m, and green
		// still at a tenth by 24.5 m. The heuristic gave
		//     R 0.17216   G 0.20358   B 0.21580
		// which is 2.2x too strong in green and 3.7x too strong in blue, so
		// the water clears only in the shallows. And it is nearly FLAT across
		// the channels, absorbing blue marginally harder than red, which is
		// backwards: water kills red first and carries blue furthest. That is
		// the whole reason deep water is blue.
		//
		// The authored colour is not a colour. On the ocean family it is a
		// per-metre TRANSMISSION - what the water reaches after
		// AbsorptionDistanceM metres, 2.0 m on this level - and the core hands
		// back the conversion so this never has to know the formula.
		if (W.Extinction.R >= 0.f)
		{
			// SPLITTING EXTINCTION INTO SCATTER AND ABSORB, and the previous
			// split was a category error that made the water milky.
			//
			// Unreal wants both, and their ratio is the single-scattering
			// albedo - what colour light comes back as. This used the level's
			// derived SurfaceColour as that albedo. SurfaceColour is not an
			// albedo: it is a TRANSMISSION. Measured, it is exp(-extinction x
			// 1 m) to within 0.004 in green and 0.002 in blue. Feeding a
			// transmittance in as an albedo gave (0.718, 0.906, 0.942) - very
			// nearly white, with green and blue within 4% of each other - so
			// deep water scattered back pale cyan with no blue dominance at
			// all. That reads as murky green.
			//
			// THE COLOUR OF WATER IS NOT IN ITS SCATTERING, IT IS IN ITS
			// ABSORPTION. Scattering in clear water is close to spectrally
			// FLAT; red is absorbed 4.8 times harder than blue here, and what
			// survives to scatter back is therefore blue. So the scattering
			// coefficient is one number for all three channels and every bit
			// of the colour comes out of the decoded extinction.
			//
			// It is capped at the WEAKEST channel's extinction, because
			// scattering cannot exceed extinction - absorption would have to
			// go negative. Blue is always the weakest, so blue ends up with an
			// albedo near 1 and red near 0.2, which is exactly the ratio that
			// makes deep water blue.
			//
			// This is a stated MODEL rather than a decoded quantity, and the
			// one free number in it is exposed as BF6.HighPoly.WaterAlbedo.
			const float ExtMin = FMath::Min3(W.Extinction.R, W.Extinction.G,
			                                 W.Extinction.B);
			const float ScatterFlat =
				FMath::Max(ExtMin * FMath::Clamp(GWaterAlbedoScale, 0.f, 1.f), 0.f);
			auto Split = [&](float Ext) -> TPair<float, float>
			{
				const float S = FMath::Min(ScatterFlat, Ext);
				return TPair<float, float>(S, FMath::Max(Ext - S, 0.f));
			};
			const TPair<float, float> Rr = Split(W.Extinction.R);
			const TPair<float, float> Gg = Split(W.Extinction.G);
			const TPair<float, float> Bb = Split(W.Extinction.B);
			const FLinearColor ScatterCm(Rr.Key * ToPerCm, Gg.Key * ToPerCm, Bb.Key * ToPerCm);
			const FLinearColor AbsorbCm (Rr.Value * ToPerCm, Gg.Value * ToPerCm, Bb.Value * ToPerCm);
			MID->SetVectorParameterValue(TEXT("Scattering"), ScatterCm);
			MID->SetVectorParameterValue(TEXT("Absorption"), AbsorbCm);
			GWaterCoeffs.Add({ MID, ScatterCm, AbsorbCm });
			auto HalfM = [](float E) { return E > 1e-9f ? 0.6931f / E : 0.f; };
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water: DECODED extinction /m (%.5f %.5f %.5f) over %.2f m ")
				TEXT("absorption distance - you can see half way down at ")
				TEXT("R %.1f m, G %.1f m, B %.1f m"),
				W.Extinction.R, W.Extinction.G, W.Extinction.B,
				W.AbsorptionDistanceM,
				HalfM(W.Extinction.R), HalfM(W.Extinction.G), HalfM(W.Extinction.B));
			auto AlbOf = [](float S, float E) { return E > 1e-9f ? S / E : 0.f; };
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water: flat scattering %.5f /m gives albedo (%.3f %.3f %.3f) - ")
				TEXT("blue scatters back and red is absorbed, which is what makes ")
				TEXT("water blue. Move it with BF6.HighPoly.WaterAlbedo"),
				ScatterFlat,
				AlbOf(Rr.Key, W.Extinction.R), AlbOf(Gg.Key, W.Extinction.G),
				AlbOf(Bb.Key, W.Extinction.B));
			return MID;
		}
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("water: no decoded extinction for this surface, falling back to ")
			TEXT("the hue-and-brightness heuristic - expect it to clear only in ")
			TEXT("the shallows"));

		const FLinearColor ScatterCm0(
			Sat.R * Bright * ScatterPerM * ToPerCm,
			Sat.G * Bright * ScatterPerM * ToPerCm,
			Sat.B * Bright * ScatterPerM * ToPerCm);
		MID->SetVectorParameterValue(TEXT("Scattering"), ScatterCm0);
		// ABSORPTION is the complement of the hue: the channels the water
		// does NOT return are the ones it takes out of the beam, which is
		// why red dies within a metre or two and blue-green carries.
		const FLinearColor AbsorbCm(
			((1.f - Hue.R) * AbsorbPerM + 0.02f) * ToPerCm,
			((1.f - Hue.G) * AbsorbPerM + 0.02f) * ToPerCm,
			((1.f - Hue.B) * AbsorbPerM + 0.02f) * ToPerCm);
		MID->SetVectorParameterValue(TEXT("Absorption"), AbsorbCm);
		GWaterCoeffs.Add({ MID, ScatterCm0, AbsorbCm });

		// WHAT ACTUALLY REACHES THE SHADER, and the depth it implies.
		//
		// Said out loud because two rounds of tuning this by eye were wasted
		// arguing about numbers nobody had looked at. Extinction is per
		// CENTIMETRE, so the useful sanity check is the depth at which the
		// water reaches half transmittance: ln(2)/extinction, in metres. A
		// sea you can see a metre into has a half-depth of about a metre; if
		// this prints centimetres the water is opaque and no colour tuning
		// will save it.
		const FLinearColor ScatterCm(
			Sat.R * Bright * ScatterPerM * ToPerCm,
			Sat.G * Bright * ScatterPerM * ToPerCm,
			Sat.B * Bright * ScatterPerM * ToPerCm);
		auto HalfDepthM = [](float Scat, float Abs) -> float
		{
			const float Ext = FMath::Max(Scat + Abs, 1e-8f);
			return 0.6931f / Ext * 0.01f;      // cm -> m
		};
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("water colour: authored shallow (%.3f %.3f %.3f) -> hue (%.2f %.2f %.2f) bright %.2f"),
			C.R, C.G, C.B, Hue.R, Hue.G, Hue.B, Bright);
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("water coeffs /cm: scatter (%.5f %.5f %.5f) absorb (%.5f %.5f %.5f)"),
			ScatterCm.R, ScatterCm.G, ScatterCm.B, AbsorbCm.R, AbsorbCm.G, AbsorbCm.B);
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("water half-transmittance depth: R %.2f m, G %.2f m, B %.2f m ")
			TEXT("(surface tint %.3f %.3f %.3f)"),
			HalfDepthM(ScatterCm.R, AbsorbCm.R),
			HalfDepthM(ScatterCm.G, AbsorbCm.G),
			HalfDepthM(ScatterCm.B, AbsorbCm.B),
			Hue.R * SurfaceAlbedo, Hue.G * SurfaceAlbedo, Hue.B * SurfaceAlbedo);
		return MID;
	}

	// ---- the map's own SKY ------------------------------------------------
	//
	// Every level ships a painted HDR panorama, 8192 by 2048 on fourteen of
	// them, and the tool drew a generic procedural atmosphere instead. So every
	// map got the same Unreal blue, and the sky light - set to capture the
	// scene in real time - captured that generic blue as its ambient. Both
	// halves of the environment were somebody else's.
	//
	// That is the honest explanation for a run of brightness complaints. It was
	// never a calibration constant: the sky simply was not the map's.
	//
	// The panorama covers the UPPER HEMISPHERE only, which is what its 4:1
	// aspect means - 360 degrees of azimuth by 90 of elevation. Row 0 is the
	// zenith. Below the horizon the mapping clamps to the horizon row, which is
	// what the game's own dome does.
	UMaterial* GSkyParent = nullptr;
	UMaterial* GCloudShadowParent = nullptr;
	TWeakObjectPtr<UMaterialInstanceDynamic> GCloudShadowMID;

	// BF6's cloud shadows are not cloud geometry. They are one or two authored
	// BC4 sheets projected through the directional-light pass. The current Steam
	// executable now gives us the complete law: world XZ projected along the sun
	// by height (unless IsTopDown), reciprocal authored size, a half-tile-centred
	// translation + speed*time phase, field-authored addressing, coverage bias,
	// exponent, and squared fade-to-white. A UE light function is the matching
	// consumer: it modulates this map's existing directional light without
	// spawning a second sun or adding collision.
	UMaterial* EnsureCloudShadowMaterial()
	{
		if (GCloudShadowParent) return GCloudShadowParent;
		UPackage* Pkg = CreatePackage(TEXT("/Temp/BF6HighPoly_CloudShadow"));
		if (!Pkg) return nullptr;
		Pkg->SetFlags(RF_Transient);
		UMaterial* M = NewObject<UMaterial>(
			Pkg, TEXT("M_BF6HighPoly_CloudShadow"), RF_Transient);
		if (!M) return nullptr;
		M->MaterialDomain = MD_LightFunction;
		M->SetShadingModel(MSM_Unlit);
		M->BlendMode = BLEND_Opaque;

		auto Make = [&M](UClass* C, int32 X, int32 Y)
		{ return UMaterialEditingLibrary::CreateMaterialExpression(M, C, X, Y); };
		auto Vec = [&](const TCHAR* Name, FLinearColor Def, int32 Y)
		{
			UMaterialExpressionVectorParameter* P =
				Cast<UMaterialExpressionVectorParameter>(
					Make(UMaterialExpressionVectorParameter::StaticClass(), -1000, Y));
			if (P) { P->ParameterName = Name; P->DefaultValue = Def; }
			return P;
		};
		auto Scalar = [&](const TCHAR* Name, float Def, int32 Y)
		{
			UMaterialExpressionScalarParameter* P =
				Cast<UMaterialExpressionScalarParameter>(
					Make(UMaterialExpressionScalarParameter::StaticClass(), -1000, Y));
			if (P) { P->ParameterName = Name; P->DefaultValue = Def; }
			return P;
		};
		auto Tex = [&](const TCHAR* Name, int32 Y)
		{
			UMaterialExpressionTextureObjectParameter* P =
				Cast<UMaterialExpressionTextureObjectParameter>(
					Make(UMaterialExpressionTextureObjectParameter::StaticClass(), -1000, Y));
			if (P)
			{
				P->ParameterName = Name;
				P->SamplerType = SAMPLERTYPE_LinearColor;
				P->Texture = LinearWhite();
			}
			return P;
		};

		UMaterialExpressionWorldPosition* WP = Cast<UMaterialExpressionWorldPosition>(
			Make(UMaterialExpressionWorldPosition::StaticClass(), -1000, 0));
		UMaterialExpressionCameraPositionWS* CamP =
			Cast<UMaterialExpressionCameraPositionWS>(
				Make(UMaterialExpressionCameraPositionWS::StaticClass(), -1000, 60));
		UMaterialExpressionTime* Time = Cast<UMaterialExpressionTime>(
			Make(UMaterialExpressionTime::StaticClass(), -1000, 120));
		UMaterialExpressionTextureObjectParameter* PrimaryTex =
			Tex(TEXT("CloudPrimaryTex"), 200);
		UMaterialExpressionTextureObjectParameter* SecondaryTex =
			Tex(TEXT("CloudSecondaryTex"), 260);
		UMaterialExpressionVectorParameter* SunXY =
			Vec(TEXT("CloudSunXY"), FLinearColor::Black, 340);
		// Motion = translation.xy, speed.zw, all in game metres/metres-second.
		UMaterialExpressionVectorParameter* PMotion =
			Vec(TEXT("CloudPrimaryMotion"), FLinearColor::Black, 400);
		UMaterialExpressionVectorParameter* PShape =
			Vec(TEXT("CloudPrimaryShape"), FLinearColor(1, 0, 1, 0), 460);
		UMaterialExpressionVectorParameter* PFlags =
			Vec(TEXT("CloudPrimaryFlags"), FLinearColor::Black, 520);
		UMaterialExpressionScalarParameter* PMode =
			Scalar(TEXT("CloudPrimaryAddressingMode"), 0.f, 550);
		UMaterialExpressionVectorParameter* SMotion =
			Vec(TEXT("CloudSecondaryMotion"), FLinearColor::Black, 580);
		UMaterialExpressionVectorParameter* SShape =
			Vec(TEXT("CloudSecondaryShape"), FLinearColor(1, 0, 1, 0), 640);
		UMaterialExpressionVectorParameter* SFlags =
			Vec(TEXT("CloudSecondaryFlags"), FLinearColor::Black, 700);
		UMaterialExpressionScalarParameter* SMode =
			Scalar(TEXT("CloudSecondaryAddressingMode"), 0.f, 730);
		// Fade = start distance, fade span, start height, height span.
		UMaterialExpressionVectorParameter* Fade =
			Vec(TEXT("CloudFade"), FLinearColor(-1, 1, 0, 1), 760);
		UMaterialExpressionVectorParameter* FadeFlags =
			Vec(TEXT("CloudFadeFlags"), FLinearColor::Black, 820);

		UMaterialExpressionCustom* Project = Cast<UMaterialExpressionCustom>(
			Make(UMaterialExpressionCustom::StaticClass(), -300, 300));
		if (Project && WP && CamP && Time && PrimaryTex && SecondaryTex && SunXY &&
			PMotion && PShape && PFlags && PMode && SMotion && SShape && SFlags &&
			SMode && Fade && FadeFlags)
		{
			Project->Description = TEXT("BF6 two-sheet deferred cloud shadow law");
			Project->OutputType = CMOT_Float1;
			Project->Code = TEXT(R"HLSL(
float3 worldM = WPos * 0.01;
float2 pUv = (worldM.xy - SunXY.xy * PFlags.y * worldM.z) / max(abs(PShape.x), 1e-6)
           + 0.5 + (PMotion.xy + float2(PMotion.z, PFlags.z) * T) / max(abs(PShape.x), 1e-6);
float pInside = 1.0;
int pMode = (int)round(PMode);
if (pMode == 0) pUv = frac(pUv);
else if (pMode == 1) pUv = 1.0 - abs(frac(pUv * 0.5) * 2.0 - 1.0);
else if (pMode == 2) pUv = saturate(pUv);
else if (pMode == 4)
{
    pInside = (pUv.x >= 0.0 && pUv.x <= 1.0 && pUv.y >= 0.0 && pUv.y <= 1.0) ? 1.0 : 0.0;
    pUv = saturate(pUv);
}
else if (pMode == 5) pUv = saturate(abs(pUv));
else pUv = frac(pUv); // unnamed enum-3 follows the executable's fallback branch
float pSample = lerp(1.0, Texture2DSample(CloudPrimaryTex, CloudPrimaryTexSampler, pUv).r, pInside);
float pSheet = lerp(1.0,
    pow(saturate(pSample + (1.0 - max(PShape.y, 0.0))), PShape.z), PFlags.x);

float2 sUv = (worldM.xy - SunXY.xy * SFlags.y * worldM.z) / max(abs(SShape.x), 1e-6)
           + 0.5 + (SMotion.xy + float2(SMotion.z, SFlags.z) * T) / max(abs(SShape.x), 1e-6);
float sInside = 1.0;
int sMode = (int)round(SMode);
if (sMode == 0) sUv = frac(sUv);
else if (sMode == 1) sUv = 1.0 - abs(frac(sUv * 0.5) * 2.0 - 1.0);
else if (sMode == 2) sUv = saturate(sUv);
else if (sMode == 4)
{
    sInside = (sUv.x >= 0.0 && sUv.x <= 1.0 && sUv.y >= 0.0 && sUv.y <= 1.0) ? 1.0 : 0.0;
    sUv = saturate(sUv);
}
else if (sMode == 5) sUv = saturate(abs(sUv));
else sUv = frac(sUv);
float sSample = lerp(1.0, Texture2DSample(CloudSecondaryTex, CloudSecondaryTexSampler, sUv).r, sInside);
float sSheet = lerp(1.0,
    pow(saturate(sSample + (1.0 - max(SShape.y, 0.0))), SShape.z), SFlags.x);

float effectiveStart = Fade.x >= 0.0 ? Fade.x : 1000000.0;
float effectiveSpan = Fade.y >= 0.0 ? max(Fade.y, 1e-6) : min(Fade.y, -1e-6);
float distanceFade = saturate((distance(WPos, CamPos) * 0.01 - effectiveStart) / effectiveSpan);
float heightStart = FadeFlags.x > 0.5 ? Fade.z : 1000000.0;
float heightSpan = max(FadeFlags.y, 1e-6);
float heightFade = saturate((worldM.z - heightStart) / heightSpan);
float fade = max(distanceFade, heightFade);
float sheet = pSheet * sSheet;
return sheet + fade * fade * (1.0 - sheet);
)HLSL");
			Project->Inputs.Empty();
			auto In = [&Project](const TCHAR* Name, UMaterialExpression* E)
			{ FCustomInput I; I.InputName = Name; I.Input.Expression = E; Project->Inputs.Add(I); };
			In(TEXT("WPos"), WP);
			In(TEXT("CamPos"), CamP);
			In(TEXT("T"), Time);
			In(TEXT("CloudPrimaryTex"), PrimaryTex);
			In(TEXT("CloudSecondaryTex"), SecondaryTex);
			In(TEXT("SunXY"), SunXY);
			In(TEXT("PMotion"), PMotion);
			In(TEXT("PShape"), PShape);
			In(TEXT("PFlags"), PFlags);
			In(TEXT("PMode"), PMode);
			In(TEXT("SMotion"), SMotion);
			In(TEXT("SShape"), SShape);
			In(TEXT("SFlags"), SFlags);
			In(TEXT("SMode"), SMode);
			In(TEXT("Fade"), Fade);
			In(TEXT("FadeFlags"), FadeFlags);
			UMaterialEditingLibrary::ConnectMaterialProperty(
				Project, TEXT(""), MP_EmissiveColor);
		}

		M->PreEditChange(nullptr);
		M->PostEditChange();
		BF6_ReportMaterialState(TEXT("cloud-shadow"), M);
		M->AddToRoot();
		GCloudShadowParent = M;
		return M;
	}

	void ApplyCloudShadow(UDirectionalLightComponent* Sun, UObject* Outer,
		const BF6HP::FCore::FVELighting& V, const FVector& ToSun)
	{
		if (!Sun || !Outer) return;
		UTexture2D* Primary = V.CloudShadowTexture >= 0
			? TextureFor(V.CloudShadowTexture, 4096) : nullptr;
		const bool bPrimary = Primary && V.CloudShadowSizeM > KINDA_SMALL_NUMBER &&
			V.CloudShadowCoverage > 0.f;
		if (!bPrimary)
		{
			if (GCloudShadowMID.IsValid() &&
				Sun->LightFunctionMaterial == GCloudShadowMID.Get())
			{
				Sun->ClearLightFunctionMaterial();
			}
			GCloudShadowMID.Reset();
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("cloud shadow: active preset has no enabled primary sheet"));
			return;
		}

		UMaterial* Parent = EnsureCloudShadowMaterial();
		if (!Parent) return;
		UTexture2D* Secondary = V.SecondaryCloudShadowTexture >= 0
			? TextureFor(V.SecondaryCloudShadowTexture, 4096) : nullptr;
		const bool bSecondary = Secondary &&
			V.SecondaryCloudShadowSizeM > KINDA_SMALL_NUMBER &&
			V.SecondaryCloudShadowCoverage > 0.f;
		UMaterialInstanceDynamic* MID =
			UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return;
		MID->SetFlags(RF_Transient);
		MID->SetTextureParameterValue(TEXT("CloudPrimaryTex"), Primary);
		if (bSecondary)
			MID->SetTextureParameterValue(TEXT("CloudSecondaryTex"), Secondary);
		MID->SetVectorParameterValue(TEXT("CloudSunXY"),
			FLinearColor(ToSun.X, ToSun.Y, 0.f, 0.f));
		MID->SetVectorParameterValue(TEXT("CloudPrimaryMotion"), FLinearColor(
			V.CloudShadowTranslation.X, V.CloudShadowTranslation.Y,
			V.CloudShadowSpeed.X, V.CloudShadowSpeed.Y));
		MID->SetVectorParameterValue(TEXT("CloudPrimaryShape"), FLinearColor(
			V.CloudShadowSizeM, V.CloudShadowCoverage, V.CloudShadowExponent,
			(float)V.CloudShadowAddressingMode));
		MID->SetVectorParameterValue(TEXT("CloudPrimaryFlags"), FLinearColor(
			1.f, V.bCloudShadowTopDown ? 0.f : 1.f, V.CloudShadowSpeed.Y, 0.f));
		MID->SetScalarParameterValue(TEXT("CloudPrimaryAddressingMode"),
			(float)V.CloudShadowAddressingMode);
		MID->SetVectorParameterValue(TEXT("CloudSecondaryMotion"), FLinearColor(
			V.SecondaryCloudShadowTranslation.X, V.SecondaryCloudShadowTranslation.Y,
			V.SecondaryCloudShadowSpeed.X, V.SecondaryCloudShadowSpeed.Y));
		MID->SetVectorParameterValue(TEXT("CloudSecondaryShape"), FLinearColor(
			FMath::Max(V.SecondaryCloudShadowSizeM, 1.e-6f),
			V.SecondaryCloudShadowCoverage, V.SecondaryCloudShadowExponent,
			(float)V.SecondaryCloudShadowAddressingMode));
		MID->SetVectorParameterValue(TEXT("CloudSecondaryFlags"), FLinearColor(
			bSecondary ? 1.f : 0.f,
			V.bSecondaryCloudShadowTopDown ? 0.f : 1.f,
			V.SecondaryCloudShadowSpeed.Y, 0.f));
		MID->SetScalarParameterValue(TEXT("CloudSecondaryAddressingMode"),
			(float)V.SecondaryCloudShadowAddressingMode);
		MID->SetVectorParameterValue(TEXT("CloudFade"), FLinearColor(
			V.CloudShadowStartFadeM, V.CloudShadowFadeDistanceM,
			V.CloudShadowStartHeightFadeM, V.CloudShadowHeightFadeDistanceM));
		MID->SetVectorParameterValue(TEXT("CloudFadeFlags"),
			FLinearColor(V.bCloudShadowHeightFade ? 1.f : 0.f,
				V.CloudShadowHeightFadeDistanceM, 0.f, 0.f));

		// The material reproduces Frostbite's own distance/height fade. Keep
		// Unreal's additional light-function fade outside any authored map span.
		Sun->SetLightFunctionFadeDistance(1.0e9f);
		Sun->SetLightFunctionDisabledBrightness(1.f);
		Sun->SetLightFunctionMaterial(MID);
		GCloudShadowMID = MID;
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("cloud shadow: primary tex %d size %.1fm coverage %.3f exp %.3f mode %d topDown %d; secondary %s tex %d size %.1fm mode %d"),
			V.CloudShadowTexture, V.CloudShadowSizeM, V.CloudShadowCoverage,
			V.CloudShadowExponent, V.CloudShadowAddressingMode,
			V.bCloudShadowTopDown ? 1 : 0, bSecondary ? TEXT("on") : TEXT("off"),
			V.SecondaryCloudShadowTexture, V.SecondaryCloudShadowSizeM,
			V.SecondaryCloudShadowAddressingMode);
	}

	UMaterial* EnsureSkyMaterial()
	{
		if (GSkyParent) return GSkyParent;
		UPackage* Pkg = CreatePackage(TEXT("/Temp/BF6HighPoly_Sky"));
		if (!Pkg) return nullptr;
		Pkg->SetFlags(RF_Transient);
		UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_BF6HighPoly_Sky"), RF_Transient);
		if (!M) return nullptr;
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_Unlit);
		M->BlendMode = BLEND_Opaque;
		// Seen from the inside, and it must never occlude or shadow anything.
		M->TwoSided = true;
		M->bIsSky = true;

		UMaterialExpressionTextureObjectParameter* Pano =
			Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -600, 0));
		if (Pano)
		{
			Pano->ParameterName = TEXT("Panorama");
			Pano->SamplerType = SAMPLERTYPE_LinearColor;
			Pano->Texture = LinearWhite();
		}
		UMaterialExpressionTextureObjectParameter* FlowMask =
			Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -600, 80));
		if (FlowMask)
		{
			FlowMask->ParameterName = TEXT("SkyFlowMask");
			FlowMask->SamplerType = SAMPLERTYPE_LinearColor;
			// Flow is disabled by default, so linear white is only a compile-time
			// sampler-type witness when a level has no resolved raw mask.
			FlowMask->Texture = LinearWhite();
		}
		auto Scal = [&](const TCHAR* Name, float Def, int32 Y)
			-> UMaterialExpressionScalarParameter*
		{
			UMaterialExpressionScalarParameter* P =
				Cast<UMaterialExpressionScalarParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionScalarParameter::StaticClass(), -600, Y));
			if (P) { P->ParameterName = Name; P->DefaultValue = Def; }
			return P;
		};
		UMaterialExpressionScalarParameter* Rot   = Scal(TEXT("SkyRotationTurns"), 0.f, 120);
		UMaterialExpressionScalarParameter* VMin  = Scal(TEXT("SkyVMin"), 0.f, 200);
		UMaterialExpressionScalarParameter* VMax  = Scal(TEXT("SkyVMax"), 1.f, 280);
		UMaterialExpressionScalarParameter* Tile  = Scal(TEXT("SkyTileFactor"), 1.f, 360);
		UMaterialExpressionScalarParameter* Scale = Scal(TEXT("SkyEmissiveScale"), 1.f, 440);
		UMaterialExpressionScalarParameter* FlowEnabled = Scal(TEXT("SkyFlowEnabled"), 0.f, 520);
		UMaterialExpressionScalarParameter* FlowDistance = Scal(TEXT("SkyFlowDistance"), 0.f, 600);
		UMaterialExpressionScalarParameter* FlowDirection = Scal(TEXT("SkyFlowDirectionDeg"), 0.f, 680);
		UMaterialExpressionScalarParameter* FlowPeriod = Scal(TEXT("SkyFlowPeriodSeconds"), 1.f, 760);
		UMaterialExpressionScalarParameter* FlowHeightScale = Scal(TEXT("SkyFlowHeightScale"), 0.f, 840);
		UMaterialExpressionScalarParameter* FlowHeightBias = Scal(TEXT("SkyFlowHeightBias"), 0.f, 920);
		UMaterialExpressionTime* Time =
			Cast<UMaterialExpressionTime>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTime::StaticClass(), -600, 1000));

		UMaterialExpressionWorldPosition* WP =
			Cast<UMaterialExpressionWorldPosition>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionWorldPosition::StaticClass(), -600, 520));
		UMaterialExpressionCameraPositionWS* CamP =
			Cast<UMaterialExpressionCameraPositionWS>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCameraPositionWS::StaticClass(), -600, 600));

		UMaterialExpressionCustom* Dome =
			Cast<UMaterialExpressionCustom>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCustom::StaticClass(), -200, 200));
		if (Dome && Pano && FlowMask && Rot && VMin && VMax && Tile && Scale
			&& FlowEnabled && FlowDistance && FlowDirection && FlowPeriod
			&& FlowHeightScale && FlowHeightBias && Time && WP && CamP)
		{
			Dome->Description = TEXT("BF6 sky dome");
			Dome->OutputType = CMOT_Float3;
			Dome->Code = TEXT(R"HLSL(
float3 d = normalize(WPos - CamPos);
// The game's bearing is measured from its +Z, turning toward +X, and game +Z
// is Unreal +Y, so atan2(x, y) IS the bearing.
//
// MINUS the rotation, and NO half turn. Solved against the fleet rather than
// guessed: the painted sun sits at the panorama's u = 0.5 column, measured
// independently on five maps, so the authored rotation must be whatever
// carries the authored sun bearing to that column. Over the 15 panoramic-sky
// levels, u = az/360 - rot lands the sun at a median |u - 0.5| of 0.0072
// turns, which is 2.6 degrees, with 14 of 15 inside 0.04. The other three sign
// and offset combinations score 0.245 to 0.493, and a shuffled pairing of
// rotations against azimuths matched the true one in 0 of 400 trials.
//
// The one level that misses is mp_contaminated at 0.239, whose authored sun is
// near due north; not chased.
//
// AND THE HANDEDNESS IS MIRRORED, which the |u - 0.5| test could not tell.
//
// Those two candidates are reflections about u = 0.5, so a painted sun sitting
// at 0.5 fits both. What separates them is that they are NOT equally 0.5 on
// every level - the gap reaches 0.076 turns on mp_tungsten - so ranking levels
// by an INDEPENDENT quality metric settles it. Using how well the painted
// sun's ELEVATION matches the authored SunRotationY, which carries no azimuth
// information at all, the four best levels favour the mirrored branch 4 of 4
// with a mean azimuth error of 1.2 degrees against 10.4, the best eight favour
// it 6 of 8, and 0 of 400 shuffled pairings match.
//
// So u DECREASES as the bearing turns from game +Z toward +X.
// Recovered DXIL chain, unanimous in all 48 panorama permutations:
// fmul(fadd(window, rotation), PanoramicTileFactor). The shipped fleet authors
// Tile=1, but omitting it would silently break the first non-1 preset.
float u = frac((Rot - atan2(d.x, d.y) / 6.2831853) * Tile);
// UPPER HEMISPHERE ONLY. Row 0 is the zenith, so elevation runs 90 degrees at
// v = 0 down to the horizon at v = 1, and anything below the horizon holds on
// the horizon row rather than wrapping to the top of the image.
float e = degrees(asin(clamp(d.z, -1.0, 1.0)));
float v = 1.0 - max(e, 0.0) / 90.0;
// PanoramicUVMaxY is the affine SCALE and PanoramicUVMinY the BIAS. This is
// not lerp(min,max,v): shipped negative minima make the two laws observably
// different at the zenith/horizon boundary.
v = saturate(v * VMaxY + VMinY);
float2 skyUv = float2(u, v);

// EXACT CURRENT-INSTALL FLOW LAW, not a generic texture pan.
//
// The executable reads the five authored SkyComponentState fields and packs:
//   cb0[36] = direction.xyz, phaseBlend
//   cb0[37] = heightScale, heightBias, phaseDistanceA, phaseDistanceB
// The formulas below matched the shipped native arithmetic in 6/6 synthetic
// probes. Wrong complement/unwrapped/rotated controls scored 2/6, 3/6, 0/6.
float period = max(abs(FlowPeriodSeconds), 1.0e-6);
float p = frac(TimeSeconds / period);
float phaseBlend = (cos(6.28318530718 * p) + 1.0) * 0.5;
float phaseDistanceA = (p - 0.5) * FlowDist;
float phaseDistanceB = (frac(p + 0.5) - 0.5) * FlowDist;
float theta = radians(FlowDirDeg);
float3 flowDirection = float3(cos(theta), 0.0, sin(theta));

// Frostbite view space is X/right, Y/up, Z/forward. The Unreal dome direction
// above is X/right, Y/forward, Z/up, hence this exact component swap.
float3 gameView = float3(d.x, d.z, d.y);
float horizontal = max(length(gameView.xz), 1.0e-6);
float3 tangentU = float3(-gameView.z, 0.0, gameView.x) / horizontal;
float3 tangentV = float3(
    -gameView.y * gameView.x / horizontal,
     horizontal,
    -gameView.y * gameView.z / horizontal);
float2 projectedDirection = float2(
    dot(flowDirection, tangentU),
    4.0 * dot(flowDirection, tangentV));

// The mask is sampled at the undisplaced sky UV. Its red channel becomes the
// displacement amplitude; the vertical component of view direction supplies
// the authored height falloff. The panorama is then sampled at TWO displaced
// positions and cosine-blended, exactly as the 48 flow DXIL permutations do.
float flow = Texture2DSampleLevel(
    FlowTex, FlowTexSampler, skyUv, 0.0).r * FlowOn;
float heightMask = saturate((abs(gameView.y) - FlowHeightBiasValue)
    * FlowHeightScaleValue);
float2 offset = flow * heightMask * projectedDirection;
float2 uvA = skyUv + offset * phaseDistanceA;
float2 uvB = skyUv + offset * phaseDistanceB;
uvA = float2(frac(uvA.x), saturate(uvA.y));
uvB = float2(frac(uvB.x), saturate(uvB.y));
float3 phaseA = Texture2DSample(Pano, PanoSampler, uvA).rgb;
float3 phaseB = Texture2DSample(Pano, PanoSampler, uvB).rgb;
return lerp(phaseA, phaseB, phaseBlend) * Scale;
)HLSL");
			Dome->Inputs.Empty();
			auto In = [&Dome](const TCHAR* Nm, UMaterialExpression* E)
			{ FCustomInput I; I.InputName = Nm; I.Input.Expression = E; Dome->Inputs.Add(I); };
			In(TEXT("WPos"), WP);
			In(TEXT("CamPos"), CamP);
			In(TEXT("Pano"), Pano);
			In(TEXT("FlowTex"), FlowMask);
			In(TEXT("Rot"), Rot);
			In(TEXT("VMinY"), VMin);
			In(TEXT("VMaxY"), VMax);
			In(TEXT("Tile"), Tile);
			In(TEXT("Scale"), Scale);
			In(TEXT("FlowOn"), FlowEnabled);
			In(TEXT("FlowDist"), FlowDistance);
			In(TEXT("FlowDirDeg"), FlowDirection);
			In(TEXT("FlowPeriodSeconds"), FlowPeriod);
			In(TEXT("FlowHeightScaleValue"), FlowHeightScale);
			In(TEXT("FlowHeightBiasValue"), FlowHeightBias);
			In(TEXT("TimeSeconds"), Time);
			UMaterialEditingLibrary::ConnectMaterialProperty(Dome, TEXT(""), MP_EmissiveColor);
		}

		M->PreEditChange(nullptr);
		M->PostEditChange();
		BF6_ReportMaterialState(TEXT("sky"), M);
		// GSkyParent is a native cache, not a UPROPERTY. A completed build has no
		// actor referencing this parent directly (only its MID does), so GC between
		// two full rebuilds could collect it and leave the raw pointer dangling.
		// The next ApplyEnvironment then crashed in MID::Create. Keep the reusable
		// graph alive exactly like the ground, water and generic parent caches.
		M->AddToRoot();
		GSkyParent = M;
		return M;
	}

	// ---- the map's own lighting ------------------------------------------
	//
	// TWO HALVES, and the tool had neither. The ENVIRONMENT comes from the
	// level's VisualEnvironment: a sun with a real bearing, elevation, linear
	// tint and illuminance in LUX, an atmosphere, and fog. The LOCAL LIGHTS
	// are every lamp, spot, tube and lit panel the level places, of which a
	// map ships thousands - 7,878 on MP_Dumbo, 3,874 on MP_Aftermath.
	//
	// Before this the whole world was lit by one directional light at a
	// hard-coded intensity of 3, with no sky and no ambient. That is why
	// interiors were black and why water read as dark whatever its own
	// coefficients said: water is mostly REFLECTED SKY, and there was no sky.
	int32 GLightsBuilt = 0;

	// The ceiling on an authored light, in lumens. See BuildLights: the fleet
	// authors up to 6e7 and the raw-to-renderer conversion is not decoded, so
	// this is a calibration, not a reading.
	float GLightLumenMax = 50000.f;

	// ONE PHOTOMETRIC SCALE FOR THE WHOLE SCENE. ApplyEnvironment converts the
	// game's physical sun into the editor's exposure range and records that
	// exact conversion here. Local lights and the sky must take the same scale
	// or their ratios are destroyed: MP_Aftermath previously rendered a 24,000
	// lux sun as 3.62 while leaving a 50,000 lm lamp at 50,000. At ten metres
	// that made the lamp brighter than daylight, which is why whole buildings
	// blew out. This is derived per map; it is not another brightness preset.
	float GLightPhotometricScale = 1.f;

	// WHAT WAS AUTHORED, kept alongside what was built.
	//
	// The ceiling above is a calibration and the user has said the lamps read
	// too bright, so the number needs to be FOUND rather than argued about.
	// Re-applying it means knowing each light's authored value, which the
	// clamp would otherwise have thrown away, so it is retained here and the
	// ceiling becomes a live command instead of a rebuild.
	TArray<TPair<TWeakObjectPtr<ULightComponent>, float>> GAuthoredLights;

	// Thousands of ACTORS would be thousands of entries in the outliner and a
	// spawn cost to match. These are components on the add-on's own actor
	// instead, named so the layer switch can find them.
	template <typename TComp>
	TComp* AddLightComp(AActor* A, USceneComponent* Root, int32 Index, const TCHAR* Kind,
	                    const TCHAR* Prefix = TEXT("Light"))
	{
		TComp* C = NewObject<TComp>(
			A, *FString::Printf(TEXT("%s_%s_%d"), Prefix, Kind, Index), RF_Transient);
		if (!C) return nullptr;
		C->SetupAttachment(Root);
		C->RegisterComponent();
		return C;
	}

	// ONE LIGHT FROM ONE DECODED RECORD, and the only place one is built.
	//
	// Extracted so the level's own lights and the lights inside a prop the
	// user placed cannot drift apart: they are the same fixtures out of the
	// same install, read by the same walk, and the only difference is whether
	// the walk reached them through the level graph or through a pf_portal_
	// prefab. Two constructions would have meant two answers about energy,
	// cone angle and aim (the Godot plugin's make_light exists for the same
	// reason). Returns null for a dark record (bOutDark set) or when the
	// component could not be made. OutAuthoredLm is the pre-clamp power.
	ULightComponent* MakeLightFrom(AActor* A, USceneComponent* Root, const BF6HP::FCore::FLight& L,
	                               int32 Index, const TCHAR* NamePrefix, bool bRelative,
	                               float& OutAuthoredLm, bool& bOutDark)
	{
		OutAuthoredLm = 0.f;
		bOutDark = false;
		float Lum = L.Intensity;
		if (L.Unit == 1)
		{
			const float R = FMath::Max(L.ShapeRadiusM, 0.f);
			float Area = 0.f;
			if (L.Type == 3)                     // rect
			{
				const float H = FMath::Max(L.RectHeightM, 0.f);
				Area = PI * H * (H * FMath::Max(L.RectAspect, 0.01f));
			}
			else if (L.Type == 2 && R > 0.f)     // tube
			{
				Area = PI * (2.f * PI * R * FMath::Max(L.TubeWidthM, 0.f)
				             + 4.f * PI * R * R);
			}
			else if (L.Type == 1 && R > 0.f)     // spot with a disc
			{
				// The disc spot is pi^2 r^2, NOT the sphere's 4 pi^2 r^2.
				// Separate leaf in the engine's own function.
				Area = PI * PI * R * R;
			}
			else if (R > 0.f)                    // sphere with a real radius
			{
				Area = 4.f * PI * PI * R * R;
			}
			// No area means a punctual emitter, where a luminance has no
			// meaning and the authored figure can only be an intensity.
			// The rect's punctual case divides by pi rather than 4 pi.
			Lum *= (Area > 0.f) ? Area : (L.Type == 3 ? PI : 4.f * PI);
		}
		// Zero is a real authored off state. Treating it as an absent value
		// switched dark fixtures back on and made dense light rigs over-bright.
		Lum *= FMath::Max(L.Dimmer, 0.f);
		OutAuthoredLm = Lum;
		if (Lum > GLightLumenMax) Lum = GLightLumenMax;
		// A light with no energy or no reach is not a light. Both occur in
		// the data and both would cost a component for nothing.
		if (Lum <= 0.f || L.AttenuationRadiusM <= 0.f) { bOutDark = true; return nullptr; }

		// Same basis conversion the props take: Y and Z swap, and the
		// basis carries the holder's SCALE, so a row must be normalised
		// before it can be used as a direction.
		const FVector Right(L.Right.X,   L.Right.Z,   L.Right.Y);
		const FVector Up(L.Up.X,         L.Up.Z,      L.Up.Y);
		const FVector Forward(L.Forward.X, L.Forward.Z, L.Forward.Y);
		// BF6's measured beam is -Forward. Unreal emits spot and rect lights
		// along local +X (ULightComponent::GetDirection), but the old adapter
		// put Right on +X and therefore turned every directed light by 90 deg.
		// MakeFromXZ also removes the holder's scale: emitter dimensions and
		// attenuation are decoded separately and must not be scaled twice.
		const FVector Beam = (-Forward).GetSafeNormal();
		const FQuat LightRotation = FRotationMatrix::MakeFromXZ(
			Beam.IsNearlyZero() ? FVector::ForwardVector : Beam,
			Up.IsNearlyZero() ? FVector::UpVector : Up.GetSafeNormal()).ToQuat();
		const FTransform Xf(LightRotation, ToUnreal(L.Origin), FVector::OneVector);

		ULightComponent* Base = nullptr;
		if (L.Type == 1)          // spot
		{
			if (USpotLightComponent* S =
				AddLightComp<USpotLightComponent>(A, Root, Index, TEXT("Spot"), NamePrefix))
			{
				// THE GAME AUTHORS FULL CONE ANGLES, Unreal wants HALF.
				S->SetInnerConeAngle(FMath::Clamp(L.InnerAngleDeg * 0.5f, 0.f, 89.f));
				S->SetOuterConeAngle(FMath::Clamp(L.OuterAngleDeg * 0.5f, 1.f, 89.f));
				Base = S;
			}
		}
		else if (L.Type == 3)     // rect
		{
			if (URectLightComponent* R =
				AddLightComp<URectLightComponent>(A, Root, Index, TEXT("Rect"), NamePrefix))
			{
				// There is no Width field on the class: it is Height times
				// Aspect, and both are metres before the holder's scale.
				R->SourceHeight = FMath::Max(1.f, L.RectHeightM * 100.f);
				R->SourceWidth  = FMath::Max(1.f, L.RectHeightM * FMath::Max(L.RectAspect, 0.01f) * 100.f);
				Base = R;
			}
		}
		else                       // sphere and tube both land on a point light
		{
			if (UPointLightComponent* Pt =
				AddLightComp<UPointLightComponent>(A, Root, Index, TEXT("Point"), NamePrefix))
			{
				Pt->SourceRadius = FMath::Max(0.f, L.ShapeRadiusM * 100.f);
				if (L.Type == 2) Pt->SourceLength = FMath::Max(0.f, L.TubeWidthM * 100.f);
				Base = Pt;
			}
		}
		if (!Base) return nullptr;

		// A prefab-local fixture on a placed object is placed RELATIVE to the
		// component that carries it; a level light is placed in the world.
		if (bRelative) Base->SetRelativeTransform(Xf); else Base->SetWorldTransform(Xf);
		// LINEAR, not sRGB: the authored colour is already linear and
		// pushing it through the sRGB path would wash every lamp out.
		Base->SetLightColor(L.Color, /*bSRGB*/ false);
		// The data is in LUMENS for all but a handful of lights, which is
		// a unit Unreal understands directly.
		if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(Base))
		{
			// Always lumens now: the luminance case was converted above.
			Local->SetIntensityUnits(ELightUnits::Lumens);
			Local->SetAttenuationRadius(L.AttenuationRadiusM * 100.f);
		}
		Base->SetIntensity(Lum * GLightPhotometricScale);
		// SHADOWS OFF regardless of the authored flag. Half of a map's
		// lights ask for shadows, and a few thousand shadow-casting
		// dynamic lights is not a preview, it is a slideshow. The flag is
		// decoded and kept in the core for whoever wants it.
		Base->SetCastShadows(false);
		Base->SetMobility(EComponentMobility::Movable);
		return Base;
	}

	int32 BuildLights(AActor* A, USceneComponent* Root,
	                  const TArray<BF6HP::FCore::FLight>& Lights)
	{
		int32 Made = 0, SkippedDark = 0, SkippedPresentation = 0;
		int32 SkippedEndOfGame = 0, SkippedIgc = 0, Clamped = 0, Luminance = 0;
		double SumLm = 0.0;
		float MaxLm = 0.f;
		GAuthoredLights.Reset();
		GAuthoredLights.Reserve(Lights.Num());
		for (int32 i = 0; i < Lights.Num(); i++)
		{
			const BF6HP::FCore::FLight& L = Lights[i];
			// A level graph also mounts scripted presentation scenes.  They are
			// not initial-state world lighting: on MP_Isolated the end-of-round
			// best-squad stage and an extraction cinematic are parked directly on
			// the runway and contribute 1-25 million-lumen flares and rig spots.
			// Their provenance identifies the state branch exactly; filtering by
			// brightness or colour would also destroy real floodlights.
			const FString SourceLeaf = FPaths::GetBaseFilename(L.Source);
			if (SourceLeaf.StartsWith(TEXT("pf_endofgame_"), ESearchCase::IgnoreCase))
			{
				SkippedPresentation++; SkippedEndOfGame++; continue;
			}
			if (SourceLeaf.StartsWith(TEXT("fx_igc_"), ESearchCase::IgnoreCase))
			{
				SkippedPresentation++; SkippedIgc++; continue;
			}
			// AUTHORED LUMENS ARE NOT DIRECTLY AN UNREAL INTENSITY.
			//
			// MP_Isolated's lights run a median of 4,000 lm and a MAXIMUM of
			// 60,000,000. Six times ten to the seven lumens is not a lamp, and
			// it is not meant to be read as one: the game converts authored
			// intensity to renderer energy somewhere we have not decoded, and
			// the census that measured these said so explicitly. Passing the
			// raw figure through is the same mistake as feeding the sun its
			// authored 139,610 lux, one subsystem over.
			//
			// So the absurd tail is clamped. GLightLumenMax is deliberately one
			// number to move, and the count that hits it is logged rather than
			// swallowed, because a clamp that silently eats half a map's lights
			// would look like the lights simply being wrong.
			// LUMENS FOR EVERYTHING, because that is what the game's own
			// conversion takes and what Unreal's own conversion expects.
			//
			// The engine converts an authored quantity to renderer energy in
			// LightRenderDB.cpp::convertToLightInfo, one published Frostbite
			// formula per emitter shape, all of them dividing a LUMINOUS POWER
			// P by the emitter's solid angle or area:
			//     sphere, punctual   I = P / 4pi
			//     sphere, radius r   L = P / (4 pi^2 r^2)
			//     rectangle w by h   L = P / (pi w h)
			//     tube  r, length l  L = P / (pi (2 pi r l + 4 pi r^2))
			//     spot, outer angle  I = P / (2 pi (1 - cos(outer/2)))
			//                    OR  I = P / 4 pi, and which one is chosen by a
			//                        bool this reader CANNOT SEE: it lives on
			//                        PbrSpotLightDynamicState and on no
			//                        EntityData class, so it is runtime state
			//                        rather than authored data. Unreal's Lumens
			//                        path uses the cone form, which is the
			//                        physically correct branch. No correction
			//                        factor is applied for the other one,
			//                        because guessing which lights take it
			//                        would be inventing data.
			// Unreal implements the same photometry when it is given Lumens
			// (PointLightComponent.cpp:143, SpotLightComponent.cpp:95), so
			// handing it P is handing it the decode rather than a calibration.
			//
			// AND THE UNIT IS NOT WHAT WE THOUGHT. LightUnit 1 is LUMINANCE,
			// in nits, not candelas - the conversion above is SKIPPED for it
			// and the authored number is used as the emitted luminance
			// directly. Feeding a luminance to Unreal as candelas was simply
			// wrong. It is turned back into a power here by multiplying by the
			// emitter area, which is the same formula run backwards, so both
			// unit types arrive as lumens and nothing downstream has to care.
			float Authored = 0.f;
			bool bDark = false;
			ULightComponent* Base = MakeLightFrom(A, Root, L, i, TEXT("Light"), false, Authored, bDark);
			if (L.Unit == 1) Luminance++;
			SumLm += Authored;
			MaxLm = FMath::Max(MaxLm, Authored);
			if (Authored > GLightLumenMax) Clamped++;
			if (bDark) { SkippedDark++; continue; }
			if (!Base) continue;
			GAuthoredLights.Emplace(Base, Authored);
			Base->SetVisibility(GLayers[(int32)ELayer::Lighting].bOn, false);
			Made++;
		}
		if (SkippedDark > 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("lights: %d skipped for zero intensity or zero reach"), SkippedDark);
		}
		if (SkippedPresentation > 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("lights: %d scripted presentation light(s) excluded from the static map ")
				TEXT("(%d end-of-game, %d in-game-cinematic)"),
				SkippedPresentation, SkippedEndOfGame, SkippedIgc);
		}
		if (Made > 0)
		{
			// The DISTRIBUTION, not just the count that got clipped. A ceiling
			// is only a judgement call while nobody has looked at what it is
			// cutting, and this is the one number that says whether 50,000 is
			// throwing away a long tail or half the map.
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("lights: %.0f lm mean, %.0f lm max authored, %d in luminance ")
				TEXT("units converted to power"),
				SumLm / Made, MaxLm, Luminance);
		}
		if (Clamped > 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("lights: %d of %d clamped to %.0f lm - move it live with ")
				TEXT("BF6.HighPoly.LightMax"),
				Clamped, Made + SkippedDark, GLightLumenMax);
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("lights: shared sun/sky/local photometric scale %.9g (%g lm editor at the %.0f lm ceiling)"),
			GLightPhotometricScale,
			GLightLumenMax * GLightPhotometricScale,
			GLightLumenMax);
		return Made;
	}

	// The sun, sky and fog, from the level's own VisualEnvironment.
	void ApplyEnvironment(AActor* A, USceneComponent* Root,
	                      const BF6HP::FCore::FVELighting& V)
	{
		if (!GEditor) return;
		// Set from the sun below, and reused by the sky so the two keep the
		// ratio the fleet was measured to author.
		float SunLuxToEditor = 2.5e-5f;
		GLightPhotometricScale = SunLuxToEditor;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return;

		if (V.bHasSun)
		{
			// THE SUN IS THE TOOL'S, not ours, so it is adjusted rather than
			// replaced: spawning a second directional light would double every
			// shadow in the map.
			ADirectionalLight* Sun = nullptr;
			for (TActorIterator<ADirectionalLight> It(W); It; ++It) { Sun = *It; break; }
			if (Sun)
			{
				// Bearing 0 points along game +Z, which is Unreal +Y, and turns
				// toward +X. Elevation is above the horizon. This is the
				// direction TO the sun; the light shines the other way.
				const float Az = FMath::DegreesToRadians(V.SunBearingDeg);
				const float El = FMath::DegreesToRadians(V.SunElevationDeg);
				const FVector ToSun(FMath::Cos(El) * FMath::Sin(Az),
				                    FMath::Cos(El) * FMath::Cos(Az),
				                    FMath::Sin(El));
				Sun->SetActorRotation(FRotationMatrix::MakeFromX(-ToSun).Rotator());
				if (ULightComponent* LC = Sun->GetLightComponent())
				{
					LC->SetLightColor(V.SunColor, /*bSRGB*/ false);
					if (UDirectionalLightComponent* D = Cast<UDirectionalLightComponent>(LC))
					{
						// AUTHORED LUX IS NOT AN EDITOR INTENSITY.
						//
						// Unreal's directional light nominally takes lux, and
						// feeding it the authored value directly is what turned
						// Tsuru Reef into a white screen: 139,610 lux is
						// physically correct tropical daylight, the GAME meters
						// it at runtime, and an editor viewport does not. A
						// post-process volume with automatic exposure was not
						// enough on its own.
						//
						// So the same curve the Godot plugin settled on, for
						// the same stated reason ("the game auto-exposes, the
						// editor doesn't, so absolute lux can't be used
						// directly"), rescaled to this editor's working range -
						// the tool's own hard-coded sun sat at 3. Full midday
						// near 120,000 lux lands on a strong sun, a dim canal
						// stays dim, and the RELATIVE difference between maps
						// survives, which is the part that carries the look.
						const float Lux = FMath::Max(0.f, V.SunIntensityLux);
						const float Rel = Lux < 10.f
							? 0.3f
							: FMath::Clamp(8.0f * FMath::Pow(Lux / 139610.f, 0.45f), 0.3f, 8.f);
						D->SetIntensity(Rel);
						// KEPT, so the sky can use the SAME factor.
						//
						// The sun's authored lux is squeezed into the editor's
						// working range by the curve above. If the sky were
						// scaled by anything else, the measured relationship
						// between them would be destroyed - and that
						// relationship is the one thing about sky brightness
						// that has actually been measured: integrating all 15
						// shipped panoramas over their domes and multiplying by
						// the authored LuminanceScale puts the sky's own
						// illuminance at 0.09 to 0.30 of the level's SunIntensity,
						// median 0.19, against a shuffled-pairing control that
						// scored 0 of 400. Reusing this factor preserves that
						// ratio by construction and introduces no new constant.
						if (Lux > 1.f) SunLuxToEditor = Rel / Lux;
						GLightPhotometricScale = SunLuxToEditor;
						D->LightSourceAngle = FMath::Max(0.f, V.SunAngularRadiusDeg * 2.f);
						if (V.SunShadowViewDistanceM > 0.f)
							D->SetDynamicShadowDistanceMovableLight(
								V.SunShadowViewDistanceM * 100.f);
						ApplyCloudShadow(D, A, V, ToSun);
						UE_LOG(LogBF6HighPoly, Log,
							TEXT("lighting: sun %.0f lux authored -> %.2f editor intensity (Water Lab-calibrated Tsuru anchor)"),
							Lux, Rel);
					}
					LC->SetMobility(EComponentMobility::Movable);
					LC->MarkRenderStateDirty();
				}
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("lighting: preset %s, sun bearing %.1f deg, elevation %.1f deg, ")
					TEXT("%.0f lux, colour (%.3f %.3f %.3f)"),
					*V.Preset, V.SunBearingDeg, V.SunElevationDeg, V.SunIntensityLux,
					V.SunColor.R, V.SunColor.G, V.SunColor.B);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Warning,
					TEXT("lighting: no directional light in the map to carry the sun"));
			}
		}

		// THE LEVEL'S OWN SKY, drawn as a dome around the world.
		//
		// A sphere rather than a cubemap because the panorama is
		// equirectangular and Unreal's sky light will not take a 2D texture.
		// Its radius is the level's own furthest placement plus a quarter,
		// floored at 40 km: MP_Isolated's far world reaches 135 km, so a fixed
		// 40 km dome would sit inside its volcano, its ocean-horizon plane and
		// two of its backdrop tiles. The material is unlit and flagged as sky,
		// so it costs nothing and occludes nothing.
		UTexture2D* PanoTex = (V.PanoramaTexture >= 0) ? TextureFor(V.PanoramaTexture) : nullptr;
		UTexture2D* FlowTex = (V.FlowMaskTexture >= 0) ? TextureFor(V.FlowMaskTexture) : nullptr;
		if (V.PanoramaTexture >= 0 && !PanoTex)
		{
			// NOT the same as a level with no sky, and it must not be reported
			// as one. The panorama is BC6H, which is the only HDR format this
			// tool uploads, so a failure here is a format problem rather than a
			// missing asset.
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("sky: this level authors panorama texture %d but it did not ")
				TEXT("decode, so the sky falls back to a procedural atmosphere"),
				V.PanoramaTexture);
		}
		if (V.FlowMaskTexture >= 0 && !FlowTex)
		{
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("sky: this level authors flow-mask texture %d but its raw texture did not decode; panorama motion is disabled"),
				V.FlowMaskTexture);
		}
		if (PanoTex)
		{
			UMaterial* SkyParent = EnsureSkyMaterial();
			UStaticMesh* Sphere = LoadObject<UStaticMesh>(
				nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			if (SkyParent && Sphere)
			{
				UStaticMeshComponent* SC = NewObject<UStaticMeshComponent>(
					A, TEXT("Light_SkyDome"), RF_Transient);
				if (SC)
				{
					SC->SetupAttachment(Root);
					BF6HP::Shared::MakeUnselectable(SC);
					// REGISTERED BEFORE IT IS PLACED. A world transform set on
					// an unregistered component has no scene proxy to move and
					// does not reliably survive registration, which is the
					// pattern every other component in this file follows.
					SC->RegisterComponent();
					SC->SetStaticMesh(Sphere);
					// The engine sphere is 100 cm across, so its radius is 50.
					//
					// SIZED FROM THE LEVEL, NOT FROM A CONSTANT. 40 km was
					// picked when the authored backdrop was believed to stop
					// at 36.8 km. MP_Isolated's does not: its ocean-horizon
					// sheet becomes a 192 km plane, one backdrop tile is
					// re-placed at 4.06x out to 98 km, and its volcano reaches
					// 135 km. A 40 km dome sits INSIDE all three, so the sky
					// would be drawn in front of the scenery.
					const double DomeCm =
						FMath::Max((double)GFarWorldRadiusCm * 1.25, 4000000.0);
					SC->SetWorldScale3D(FVector(DomeCm / 50.0));
					SC->SetCastShadow(false);
					SC->bAffectDistanceFieldLighting = false;
					SC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					SC->SetMobility(EComponentMobility::Movable);
					UMaterialInstanceDynamic* MID =
						UMaterialInstanceDynamic::Create(SkyParent, SC);
					if (MID)
					{
						MID->SetFlags(RF_Transient);
						MID->SetTextureParameterValue(TEXT("Panorama"), PanoTex);
						if (FlowTex)
							MID->SetTextureParameterValue(TEXT("SkyFlowMask"), FlowTex);
						MID->SetScalarParameterValue(TEXT("SkyRotationTurns"),
							V.SkyPanoramicRotationTurns);
						MID->SetScalarParameterValue(TEXT("SkyVMin"), V.SkyPanoramicUVMin.Y);
						MID->SetScalarParameterValue(TEXT("SkyVMax"), V.SkyPanoramicUVMax.Y);
						MID->SetScalarParameterValue(TEXT("SkyTileFactor"),
							V.SkyPanoramicTileFactor);
						const bool bFlowEnabled = FlowTex != nullptr
							&& FMath::Abs(V.SkyFlowDistance) > SMALL_NUMBER
							&& FMath::Abs(V.SkyFlowPeriodSeconds) > SMALL_NUMBER;
						MID->SetScalarParameterValue(TEXT("SkyFlowEnabled"), bFlowEnabled ? 1.f : 0.f);
						MID->SetScalarParameterValue(TEXT("SkyFlowDistance"), V.SkyFlowDistance);
						MID->SetScalarParameterValue(TEXT("SkyFlowDirectionDeg"), V.SkyFlowDirectionDeg);
						MID->SetScalarParameterValue(TEXT("SkyFlowPeriodSeconds"), V.SkyFlowPeriodSeconds);
						MID->SetScalarParameterValue(TEXT("SkyFlowHeightScale"), V.SkyFlowHeightMaskScale);
						MID->SetScalarParameterValue(TEXT("SkyFlowHeightBias"), V.SkyFlowHeightMaskBias);
						// THE SAME FACTOR THE SUN USED. See the sun block.
						const float Emissive =
							FMath::Max(0.f, V.SkyLuminanceScale) * SunLuxToEditor;
						MID->SetScalarParameterValue(TEXT("SkyEmissiveScale"), Emissive);
						SC->SetMaterial(0, MID);
						UE_LOG(LogBF6HighPoly, Log,
							TEXT("sky: panorama drawn, luminance scale %.0f -> emissive ")
							TEXT("%.4g, rotation %.3f turns, tile %.3f, affine V scale/bias %.4f/%.4f; flow %s mask=%d distance %.4f at %.2f deg / %.2fs height %.4f/%.4f"),
							V.SkyLuminanceScale, Emissive, V.SkyPanoramicRotationTurns,
							V.SkyPanoramicTileFactor, V.SkyPanoramicUVMax.Y,
							V.SkyPanoramicUVMin.Y,
							bFlowEnabled ? TEXT("enabled") : TEXT("disabled"),
							V.FlowMaskTexture, V.SkyFlowDistance, V.SkyFlowDirectionDeg,
							V.SkyFlowPeriodSeconds, V.SkyFlowHeightMaskScale,
							V.SkyFlowHeightMaskBias);
					}
					SC->SetVisibility(GLayers[(int32)ELayer::Lighting].bOn, false);
				}
			}
			else if (!Sphere)
			{
				UE_LOG(LogBF6HighPoly, Warning,
					TEXT("sky: /Engine/BasicShapes/Sphere is not loadable, so the ")
					TEXT("panorama has nothing to draw on"));
			}
		}
		else if (V.PanoramaTexture < 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("sky: this level ships no panorama, falling back to a ")
				TEXT("procedural atmosphere"));
		}

		// AN ATMOSPHERE ONLY WHERE THERE IS NO PAINTED SKY.
		//
		// The previous rule kept the atmosphere on SkyType 2 as well, on the
		// belief that exactly one level was Physical and that there the
		// atmosphere WAS the sky. Both halves were wrong.
		//
		// TEN shipped levels read SkyType 2, not one: MP_Isolated, MP_Granite,
		// its seven Portal cuts and MP_Portal_Sand. SunScale on the sky
		// component is 1.0 on all ten and on none of the eighteen procedural
		// levels, which is the same switch read a second way.
		//
		// And a physical level still ships a COMPLETE PAINTED SKY. Both
		// physical panoramas carry their own blue and their own horizon haze,
		// both are rotation aligned to within a degree of the authored sun,
		// and both are luminance calibrated so the sheet ALONE delivers 0.102
		// and 0.073 of the level's SunIntensity - the same relationship the
		// procedural fleet authors. MP_Isolated's PanoramicAlphaTexture reads
		// 0.92 to 1.00 across the entire sheet, so its dome is the painted sky
		// under any reading of that mask. Drawing a full Rayleigh sky
		// underneath pays for the blue and the ambient twice.
		//
		// What SkyType 2 actually buys is AERIAL PERSPECTIVE over distance
		// geometry - AerialPerspectiveIntensity is 150 on MP_Isolated and 50
		// on Granite against 0 on fifteen of the eighteen procedural levels -
		// and that is a haze over the far world, not a second sky.
		const bool bWantAtmosphere = (PanoTex == nullptr);
		if (bWantAtmosphere)
		{
			if (USkyAtmosphereComponent* Atm =
				NewObject<USkyAtmosphereComponent>(A, TEXT("Light_SkyAtmosphere"), RF_Transient))
			{
				Atm->SetupAttachment(Root);
				Atm->RegisterComponent();
				// UNITS. BF6 authors scattering coefficients PER METRE (Mie 4e-6 on
				// the fleet default); Unreal's SkyAtmosphere scales are PER
				// KILOMETRE (Mie default 0.003996). Handing the per-metre number
				// straight across made Mie a thousand times too weak, so the far
				// world lost its haze. Rayleigh was never applied at all; BF6's
				// (5.8e-6, 1.35e-5, 3.31e-5)/m IS Unreal's default
				// (0.0058, 0.0136, 0.0331)/km to three digits, and Isolated ships
				// its own blue (2.81e-5). Research: scattering.md 2.8 / 3.B3.
				if (V.MieCoefficient > 0.f) Atm->MieScatteringScale = V.MieCoefficient * 1000.f;
				if (V.MieG != 0.f) Atm->MieAnisotropy = FMath::Clamp(V.MieG, -0.99f, 0.99f);
				// The core leaves Rayleigh at (1,1,1) when the level authors none;
				// a real per-metre coefficient is ~1e-5, so anything near 1 is unset.
				if (V.Rayleigh.B > 0.f && V.Rayleigh.B < 1e-2f)
				{
					Atm->RayleighScatteringScale = V.Rayleigh.B * 1000.f;
					Atm->RayleighScattering = FLinearColor(
						V.Rayleigh.R / V.Rayleigh.B, V.Rayleigh.G / V.Rayleigh.B, 1.f, 1.f);
				}
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("lighting: atmosphere Mie %.5f/km g %.3f, Rayleigh %.5f/km (%.3f %.3f 1)"),
					Atm->MieScatteringScale, Atm->MieAnisotropy, Atm->RayleighScatteringScale,
					Atm->RayleighScattering.R, Atm->RayleighScattering.G);
				Atm->SetVisibility(GLayers[(int32)ELayer::Lighting].bOn, false);
			}
		}
		// EXPOSURE, which is the half of the environment that was missing.
		//
		// The VE authors real photometric units: Tsuru Reef's sun is 139,610
		// lux, which is correct for tropical daylight and is meaningless
		// without a camera response. The game meters it at runtime -
		// `auto_exposure` is 1 on every shipped map - so the authored `ev` is
		// the STARTING POINT of that loop, not a value to apply. Rendered raw,
		// as it was until now, the screen goes white.
		//
		// So the same thing the game does: automatic exposure, over a range
		// wide enough to swallow six orders of magnitude between a lit reef and
		// an unlit interior, with the authored compensation applied on top.
		{
			// Own the metering component by the transient High Poly actor. A
			// separately spawned transient volume disappeared during the build,
			// leaving the live scene with zero post-process consumers even though
			// the creation log had succeeded.
			UPostProcessComponent* PP = NewObject<UPostProcessComponent>(
				A, TEXT("Light_Exposure"), RF_Transient);
			if (PP)
			{
				PP->SetupAttachment(Root);
				PP->bEnabled = true;
				PP->bUnbound = true;          // the whole world, not a box
				FPostProcessSettings& S = PP->Settings;
				S.bOverride_AutoExposureMethod = true;
				S.AutoExposureMethod = V.bAutoExposure ? AEM_Histogram : AEM_Manual;
				S.bOverride_AutoExposureMinBrightness = true;
				S.bOverride_AutoExposureMaxBrightness = true;
				// Wide on purpose. A clamped range is why a physically correct
				// sun reads as white: the metering cannot reach it.
				S.AutoExposureMinBrightness = -8.f;
				S.AutoExposureMaxBrightness = 20.f;
				S.bOverride_AutoExposureBias = true;
				S.AutoExposureBias = V.ExposureCompensation;
				S.bOverride_AutoExposureSpeedUp = true;
				S.bOverride_AutoExposureSpeedDown = true;
				S.AutoExposureSpeedUp = 3.f;
				S.AutoExposureSpeedDown = 1.f;

				// The active VE's whole-frame look. These are direct authored
				// values, not Tsuru presets carried to another level.
				UTexture2D* DecodedLutControl = nullptr;
				if (V.bGradingEnable)
				{
					S.bOverride_ColorSaturation = true;
					S.ColorSaturation = FVector4(
						V.GradeSaturation.R, V.GradeSaturation.G,
						V.GradeSaturation.B, 1.f);
					S.bOverride_ColorContrast = true;
					S.ColorContrast = FVector4(
						V.GradeContrast.R, V.GradeContrast.G,
						V.GradeContrast.B, 1.f);
					S.bOverride_ColorGain = true;
					S.ColorGain = FVector4(
						V.GradeBrightness.R, V.GradeBrightness.G,
						V.GradeBrightness.B, 1.f);
					// The shipped resource is conclusively a 33^3 HDR colour cube,
					// but that does not prove byte compatibility with Unreal's legacy
					// post-tonemap 2D LUT consumer. Directly flattening it produced a
					// whole-frame magenta/cyan remap on MP_Isolated. Keep decoding the
					// raw resource for analysis, but do not bind it until the game's HDR
					// transfer function and voxel/channel order have independent proof.
					// The authored scalar saturation/contrast/gain remain active.
					DecodedLutControl = TextureForColorLut(V.GradingLutTexture);
					if (DecodedLutControl)
					{
						UE_LOG(LogBF6HighPoly, Display,
							TEXT("grading LUT: raw 33^3 resource decoded but direct Unreal binding disabled after failed visual control"));
					}
				}
				if (V.WhiteTemperatureK >= 1500.f && V.WhiteTemperatureK <= 15000.f)
				{
					S.bOverride_TemperatureType = true;
					S.TemperatureType = TEMP_WhiteBalance;
					S.bOverride_WhiteTemp = true;
					S.WhiteTemp = V.WhiteTemperatureK;
					// WhiteTint is intentionally not mapped: the research establishes
					// no common unit between this Frostbite field and Unreal's tint.
				}
				if (V.HbaoContrast > 0.f)
				{
					S.bOverride_AmbientOcclusionIntensity = true;
					S.AmbientOcclusionIntensity = FMath::Clamp(V.HbaoContrast, 0.f, 4.f);
				}
				if (V.HbaoRadius > 0.f)
				{
					S.bOverride_AmbientOcclusionRadius = true;
					S.AmbientOcclusionRadius = FMath::Clamp(V.HbaoRadius * 200.f, 10.f, 800.f);
					S.bOverride_AmbientOcclusionRadiusInWS = true;
					S.AmbientOcclusionRadiusInWS = true;
				}
				PP->RegisterComponent();
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("lighting: exposure %s, authored ev %.2f (max %.2f), compensation %.2f; ")
					TEXT("grade=%s LUT=%s WB=%.0fK AO=%.2f/%.2f"),
					V.bAutoExposure ? TEXT("automatic") : TEXT("manual"),
					V.ExposureEV, V.ExposureEVMax, V.ExposureCompensation,
					V.bGradingEnable ? TEXT("on") : TEXT("off"),
					DecodedLutControl ? TEXT("raw-33cube-unbound") : TEXT("none"),
					V.WhiteTemperatureK, V.HbaoRadius, V.HbaoContrast);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Warning,
					TEXT("lighting: no post process component, so the sun's real ")
					TEXT("photometric intensity will read as white"));
			}
		}

		// BF6 height fog is authored in metres and HDR radiance. Preserve the hue
		// while keeping its magnitude out of Unreal's bounded colour field; depth
		// maps to exponential density by the same reciprocal used by the reference
		// implementation. A disabled HeightFog component is a real authored state.
		if (V.bHasFog && V.bHeightFogEnable)
		{
			UExponentialHeightFogComponent* Fog = NewObject<UExponentialHeightFogComponent>(
				A, TEXT("Light_HeightFog"), RF_Transient);
			if (Fog)
			{
				Fog->SetupAttachment(Root);
				Fog->SetRelativeLocation(FVector(0, 0, V.FogAltitude * 100.f));
				Fog->RegisterComponent();
				const float Peak = FMath::Max3(V.FogColor.R, V.FogColor.G, V.FogColor.B);
				// THE COLOUR IS HDR RADIANCE, and it stays that way. Normalising
				// it to a hue threw away the magnitude (3,072 on Dumbo, 98,690 on
				// Eastwood) that the level's own auto-exposure expects against a
				// 139,610 lux sun, so fog rendered as a black-grey band.
				// FogInscatteringLuminance is an unbounded FLinearColor.
				Fog->SetFogInscatteringColor(FLinearColor(V.FogColor.R, V.FogColor.G, V.FogColor.B, 1.f));
				// HeightFogDepth is the slab THICKNESS in metres, not a visibility;
				// the visibility is HeightFogVisibilityRange. Density is chosen so
				// one optical depth is reached over that range (Unreal's density is
				// per centimetre), falling back to the old depth law when the
				// level authors no range. Research: scattering.md 3.B3.
				float Density = 0.f;
				if (V.FogVisibilityRange > 0.f)      Density = 1.f / (V.FogVisibilityRange * 100.f);
				else if (V.FogDepth > 0.f)           Density = 1.f / V.FogDepth;
				Fog->SetFogDensity(FMath::Clamp(Density, 0.f, 0.05f));
				Fog->SetStartDistance(FMath::Max(0.f, V.FogDistanceStartM) * 100.f);
				Fog->SetVolumetricFog(V.bVolumetricsEnable);
				if (V.FogDistanceEndM > V.FogDistanceStartM)
					Fog->SetVolumetricFogDistance(V.FogDistanceEndM * 100.f);
				Fog->SetVisibility(GLayers[(int32)ELayer::Lighting].bOn, false);
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("lighting: fog altitude %.1fm depth %.1fm visibility %.1fm range %.1f..%.1fm ")
					TEXT("HDR peak %.1f (kept) density %.5f/cm volumetric=%s"),
					V.FogAltitude, V.FogDepth, V.FogVisibilityRange, V.FogDistanceStartM,
					V.FogDistanceEndM, Peak, Fog->FogDensity,
					V.bVolumetricsEnable ? TEXT("yes") : TEXT("no"));
			}
		}

		if (USkyLightComponent* Sky =
			NewObject<USkyLightComponent>(A, TEXT("Light_SkyLight"), RF_Transient))
		{
			Sky->SetupAttachment(Root);
			Sky->Mobility = EComponentMobility::Movable;
			// Rebuilding a transient water material while a real-time scene capture
			// is in flight can leave the water sampling the previous viewport frame.
			// Capture once after the complete High Poly scene has been registered.
			Sky->bRealTimeCapture = false;
			Sky->SourceType = SLS_CapturedScene;
			// NOT the authored SkyLuminanceScale. That number is in the game's
			// physical HDR units, the same units that make the sun 139,610 lux
			// on Tsuru Reef, and multiplying a real-time capture by it blows
			// the frame to white. The capture already carries the atmosphere's
			// own brightness; this is a unit multiplier on top of it, so it
			// stays at one and EXPOSURE does the work.
			Sky->Intensity = 1.f;
			Sky->RegisterComponent();
			Sky->SetVisibility(GLayers[(int32)ELayer::Lighting].bOn, false);
			GHighPolySkyLight = Sky;
		}
	}

	int32 GWaterBuilt = 0;

	// Battlefield does not submit one uniformly tessellated ocean rectangle.
	// The current executable registers waterTreeBuild, whose callback fills the
	// float4(origin.xyz, full tile width) instance buffer from a fixed-root,
	// view-dependent adaptive quadtree.  The root is 65,536 m wide; every patch
	// is 16x16 quads; subdivision stops at 4,096 accepted tiles or when one grid
	// interval projects below a pixel.  Tiles stay square at authored edges --
	// clipping their transforms is not part of the game path.
	struct FWaterClipmapSurface
	{
		TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component;
		FBox2D BoundsM = FBox2D(EForceInit::ForceInit);
		double HeightM = 0.0;
		int32 SurfaceIndex = -1;
	};
	TArray<FWaterClipmapSurface> GWaterClipmaps;
	FVector GWaterClipmapLastCamera = FVector::ZeroVector;
	FRotator GWaterClipmapLastRotation = FRotator::ZeroRotator;
	float GWaterClipmapLastFov = 0.f;
	FIntPoint GWaterClipmapLastViewport = FIntPoint::ZeroValue;
	bool GWaterClipmapHasCamera = false;
	// HOW OFTEN THE TREE MAY BE REBUILT, and when it last was.
	//
	// The guard below is spatial only: rebuild once the camera has moved a
	// finest tile or turned a degree. Flying the viewport does both EVERY
	// frame, so the guard passed every frame and the tree was rebuilt every
	// frame - 12,283 instances cleared, re-added and re-treed, at 99.96% of the
	// 12,288 cap, which is what made moving around with water on unusable.
	//
	// The tree is a level-of-detail structure for a surface with no detail of
	// its own; a few updates a second while the camera flies is indistinguishable
	// from every frame, and it hands the frame back to the editor.
	double GWaterClipmapLastRebuild = 0.0;
	float  GWaterTreeMaxHz = 5.f;
	FTSTicker::FDelegateHandle GWaterClipmapTickerHandle;

	// GRID DENSITY IS TUNABLE AT RUNTIME, and it is the dial that decides whether
	// a correct spectrum is actually VISIBLE.
	//
	// Unreal has no runtime tessellation: world position offset can only move
	// vertices that already exist. Vertex spacing is tileSize/quads, and the tile
	// size doubles with every level the tree walks out, so at 16 quads a
	// depth-8 tile samples the surface every 16 m. A wind-15 sea peaks near
	// L = V^2/g = 23 m, so that tile carries under one and a half samples per
	// wave and aliases the whole sea into a flat plane no matter how correct the
	// displacement is.
	//
	// 16 is the executable's own shared-patch size, but BF6 pairs it with a
	// tile-list builder we have not recovered, so copying the patch without the
	// density distribution is not copying the game.
	int32 GWaterPatchQuads = 16;
	// 4096 was my inference of the retail leaf budget, not a measured constraint,
	// and at that size the tree refused ~2,300 splits EVERY FRAME. A budget that
	// tight is not merely coarse, it is DIRECTIONAL: the surface is finite and
	// off-centre, so the amount of water competing for tiles changes as you turn,
	// and the losing direction fell to 128 m vertex spacing while the winning one
	// held 64 m. That asymmetry is what reads as "flat when I look this way".
	// Tiles are HISM instances on one 512-triangle patch, so this costs vertex
	// work and no extra draw calls.
	int32 GWaterTreeTileCap = 12288;
	// A TILE OUTSIDE THE VIEW IS HELD COARSE, NEVER DROPPED.
	//
	// The cull used to `continue` on any tile outside the horizontal cone,
	// so the sea did not EXIST behind or beside the camera. Turning then
	// rebuilt the tree and those tiles were created for the first time,
	// which is the chunk-by-chunk pop-in at the edge of the screen. The
	// game never does this: waterTreeBuild's split law is a function of
	// distance to the tile AABB, and its root is fixed rather than snapped
	// to the camera (water-draw-tree-is-a-cpu-adaptive-quadtree).
	//
	// Holding them at depth 6 keeps full 360-degree coverage for about 75
	// extra tiles: a depth-6 tile is 65536/64 = 1024 m across, and the
	// 5 km projector disc is 78.5e6 m2, so the whole off-view ring costs
	// well under 2% of the 4096 budget. Rotation can now only REFINE a
	// tile that is already there, never introduce one.
	int32 GWaterOffViewMaxDepth = 6;
	constexpr int32 GWaterTreeMaxDepth = 12;
	constexpr double GWaterTreeRootWidthM = 65536.0;
	constexpr double GWaterProjectorFarM = 5000.0;
	// Not a constant any more: it is 1/quads, and quads moves.
	inline double WaterGridTolerance() { return 1.0 / (double)FMath::Max(GWaterPatchQuads, 1); }

	struct FWaterViewState
	{
		FVector LocationCm = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float HorizontalFovDegrees = 90.f;
		FIntPoint ViewportSize = FIntPoint(1920, 1080);
	};

	double DistanceToWaterTileM(const FVector& CameraCm, const FBox2D& Tile,
	                            double HeightM)
	{
		const FVector CameraM = CameraCm * 0.01;
		const double Dx = FMath::Max3(Tile.Min.X - CameraM.X, 0.0,
		                                    CameraM.X - Tile.Max.X);
		const double Dy = FMath::Max3(Tile.Min.Y - CameraM.Y, 0.0,
		                                    CameraM.Y - Tile.Max.Y);
		const double Dz = FMath::Abs(CameraM.Z - HeightM);
		return FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
	}

	struct FWaterClipmapGridStats
	{
		FVector2D RootCenterM = FVector2D::ZeroVector;
		int32 MinLevel = MAX_int32;
		int32 MaxLevel = 0;
		double MinSpacingM = DBL_MAX;
		double MaxSpacingM = 0.0;
		double MaxTileSizeM = 0.0;
		bool bHitTileCap = false;
		// Tiles that WANTED to refine and could not afford to. bHitTileCap only
		// ever caught the overflow, never the saturation that precedes it, so
		// the tree logged a clean bill of health while starving every frame.
		int32 RefusedForBudget = 0;
		// Why a tile is not fine geometry in front of you: it was outside the
		// water surface, beyond the projector, or held coarse by the view test.
		int32 EmittedInView = 0;
		int32 EmittedCoarse = 0;
		int32 RejectedOutsideBounds = 0;
		int32 RejectedTooFar = 0;
	};

	struct FWaterTreeNode
	{
		FVector2D CenterM = FVector2D::ZeroVector;
		double HalfWidthM = 0.0;
		uint64 Code = 1;
		int32 Level = 0;
		// Projected error: how coarse this tile looks from here, in screen terms.
		// The tree refines the worst-looking tile first, so a saturated budget is
		// spent on what you can actually see rather than on stack order.
		double Priority = 0.0;
	};

	// Largest projected error first.
	struct FWaterTreeNodeWorseFirst
	{
		bool operator()(const FWaterTreeNode& A, const FWaterTreeNode& B) const
		{
			return A.Priority > B.Priority;
		}
	};

	bool WaterTileInHorizontalView(const FWaterViewState& View,
	                               const FVector2D& CenterM, double HalfWidthM)
	{
		FVector2D Forward(View.Rotation.Vector().X, View.Rotation.Vector().Y);
		if (!Forward.Normalize()) return true;
		const FVector2D CameraM(View.LocationCm.X * 0.01, View.LocationCm.Y * 0.01);
		const FVector2D ToTile = CenterM - CameraM;
		const double Along = FVector2D::DotProduct(ToTile, Forward);
		const double Side = FMath::Abs(Forward.X * ToTile.Y - Forward.Y * ToTile.X);
		const double Radius = HalfWidthM * UE_DOUBLE_SQRT_2;
		if (Along + Radius < 0.0) return false;
		const double HalfFov = FMath::DegreesToRadians(
			FMath::Clamp((double)View.HorizontalFovDegrees, 10.0, 170.0) * 0.5);
		const double CosHalf = FMath::Max(FMath::Cos(HalfFov), 1e-3);
		return Side <= FMath::Max(Along, 0.0) * FMath::Tan(HalfFov) + Radius / CosHalf;
	}

	void BuildWaterDrawTree(const FWaterViewState& View, const FBox2D& SurfaceBoundsM,
	                        double HeightM, TArray<FTransform>& Out,
	                        FWaterClipmapGridStats* OutStats = nullptr)
	{
		Out.Reset();
		FWaterClipmapGridStats Stats;
		Stats.RootCenterM = SurfaceBoundsM.GetCenter();
		const double RootHalfM = GWaterTreeRootWidthM * 0.5;
		const int32 WidthPixels = FMath::Max(View.ViewportSize.X, 1);
		const double HalfFov = FMath::DegreesToRadians(
			FMath::Clamp((double)View.HorizontalFovDegrees, 10.0, 170.0) * 0.5);
		const double WorldPerPixelAtUnitDistance = 2.0 * FMath::Tan(HalfFov) / WidthPixels;

		// How badly this tile needs refining, from where the camera is standing.
		// Vertex spacing over distance is the tile's angular error; a tile the
		// view test rejects is worth an order of magnitude less, which keeps
		// off-screen water present but coarse without letting it outbid the
		// water in front of you.
		auto PriorityOf = [&](const FVector2D& CenterM, double HalfWidthM) -> double
		{
			const FVector2D Extent(HalfWidthM, HalfWidthM);
			const FBox2D Tile(CenterM - Extent, CenterM + Extent);
			const double DistanceM = FMath::Max(
				DistanceToWaterTileM(View.LocationCm, Tile, HeightM), 1.0);
			const double SpacingM = HalfWidthM * 2.0 * WaterGridTolerance();
			const double Error = SpacingM / DistanceM;
			return WaterTileInHorizontalView(View, CenterM, HalfWidthM)
				? Error : Error * 0.1;
		};

		TArray<FWaterTreeNode> Pending;
		Pending.Reserve(GWaterTreeTileCap + 8);
		for (uint64 Quadrant = 0; Quadrant < 4; ++Quadrant)
		{
			const double HalfM = RootHalfM * 0.5;
			const FVector2D CenterM = Stats.RootCenterM + FVector2D(
				(Quadrant & 1) ? HalfM : -HalfM,
				(Quadrant & 2) ? HalfM : -HalfM);
			Pending.HeapPush({ CenterM, HalfM, 4 | Quadrant, 1,
				PriorityOf(CenterM, HalfM) }, FWaterTreeNodeWorseFirst());
		}

		while (!Pending.IsEmpty())
		{
			FWaterTreeNode Node;
			Pending.HeapPop(Node, FWaterTreeNodeWorseFirst(), EAllowShrinking::No);
			const FVector2D Extent(Node.HalfWidthM, Node.HalfWidthM);
			const FBox2D Tile(Node.CenterM - Extent, Node.CenterM + Extent);
			if (!Tile.Intersect(SurfaceBoundsM)) { ++Stats.RejectedOutsideBounds; continue; }
			const bool bInView =
				WaterTileInHorizontalView(View, Node.CenterM, Node.HalfWidthM);
			const double DistanceM = DistanceToWaterTileM(View.LocationCm, Tile, HeightM);
			// NO DISTANCE CULL. The recovered split law
			// (findings/water-draw-tree-is-a-cpu-adaptive-quadtree, 13/13 controls)
			// is only:
			//     level >= maxDepth  ||  tileWidth/16 < viewScale * distanceToAabb
			// Distance STOPS SUBDIVISION; it never rejects a tile. The 5 km
			// rejection here was ours, added during translation, and it is the
			// reason the ocean ended in open water instead of reaching the horizon.
			//
			// Dropping it is close to free. A tile 32 km wide seen from 30 km away
			// passes the error test on its first visit, so the whole distance is
			// covered by a handful of enormous tiles; the budget is spent on the
			// near field either way. What it buys is a water plane across the
			// entire 65.536 km root, which is what the game draws.
			//
			// Surface bounds still clip: the executable culls against the water
			// surface, the frustum and discard volumes separately from the tree.
			if (DistanceM > GWaterProjectorFarM) { ++Stats.RejectedTooFar; }

			const double TileSizeM = Node.HalfWidthM * 2.0;
			const double SpacingM = TileSizeM * WaterGridTolerance();
			const bool bProjectedSmallEnough =
				SpacingM < WorldPerPixelAtUnitDistance * FMath::Max(DistanceM, 0.001);
			const int32 MaxDepthHere = bInView
				? GWaterTreeMaxDepth
				: FMath::Clamp(GWaterOffViewMaxDepth, 0, GWaterTreeMaxDepth);
			const bool bWantsSplit = Node.Level < MaxDepthHere && !bProjectedSmallEnough;
			// Splitting turns one tile into four, so it costs three of the budget.
			const bool bCanAfford =
				Out.Num() + Pending.Num() + 3 <= GWaterTreeTileCap;
			if (bWantsSplit && !bCanAfford) ++Stats.RefusedForBudget;
			if (bWantsSplit && bCanAfford)
			{
				const double ChildHalfM = Node.HalfWidthM * 0.5;
				for (uint64 Quadrant = 0; Quadrant < 4; ++Quadrant)
				{
					const FVector2D ChildCenterM = Node.CenterM + FVector2D(
						(Quadrant & 1) ? ChildHalfM : -ChildHalfM,
						(Quadrant & 2) ? ChildHalfM : -ChildHalfM);
					Pending.HeapPush({ ChildCenterM, ChildHalfM,
						Node.Code * 4 | Quadrant, Node.Level + 1,
						PriorityOf(ChildCenterM, ChildHalfM) },
						FWaterTreeNodeWorseFirst());
				}
				continue;
			}

			// NEVER ABANDON THE QUEUE. This used to break out of the loop on
			// overflow, silently dropping every tile still pending - which is a
			// hole in the ocean whose position depends on the order tiles happened
			// to be visited, and therefore on where the camera was pointing.
			// Running out of budget must cost RESOLUTION, never COVERAGE, so an
			// unaffordable tile is emitted coarse instead of discarded.
			if (Out.Num() >= GWaterTreeTileCap)
			{
				Stats.bHitTileCap = true;
			}
			if (bInView) ++Stats.EmittedInView; else ++Stats.EmittedCoarse;
			// The local mesh is one metre across in centimetres. Uniform XY scale
			// is the executable's float4.w full tile width.
			Out.Emplace(FQuat::Identity,
				FVector(Node.CenterM.X * 100.0, Node.CenterM.Y * 100.0, HeightM * 100.0),
				FVector(TileSizeM, TileSizeM, 1.0));
			Stats.MinLevel = FMath::Min(Stats.MinLevel, Node.Level);
			Stats.MaxLevel = FMath::Max(Stats.MaxLevel, Node.Level);
			Stats.MinSpacingM = FMath::Min(Stats.MinSpacingM, SpacingM);
			Stats.MaxSpacingM = FMath::Max(Stats.MaxSpacingM, SpacingM);
			Stats.MaxTileSizeM = FMath::Max(Stats.MaxTileSizeM, TileSizeM);
		}
		if (Stats.MinLevel == MAX_int32) Stats.MinLevel = 0;
		if (Stats.MinSpacingM == DBL_MAX) Stats.MinSpacingM = 0.0;
		if (OutStats) *OutStats = Stats;
	}

	bool WaterClipmapTransformsDiffer(const TArray<FTransform>& A,
	                                  const TArray<FTransform>& B)
	{
		if (A.Num() != B.Num()) return true;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].GetTranslation().Equals(B[Index].GetTranslation(), 0.1) ||
				!A[Index].GetScale3D().Equals(B[Index].GetScale3D(), 0.001))
				return true;
		}
		return false;
	}

	void RunWaterClipmapControl()
	{
		if (GWaterClipmaps.IsEmpty()) return;
		const FWaterClipmapSurface& Surface = GWaterClipmaps[0];
		const FVector2D CentreM = Surface.BoundsM.GetCenter();
		FWaterViewState ViewA;
		ViewA.LocationCm = FVector(CentreM.X * 100.0, CentreM.Y * 100.0,
		                           (Surface.HeightM + 2.0) * 100.0);
		ViewA.Rotation = FRotator(-15.f, 0.f, 0.f);
		FWaterViewState ViewB = ViewA;
		ViewB.LocationCm += FVector(50000.0, 0.0, 0.0);
		FWaterViewState Far = ViewA;
		Far.LocationCm += FVector(2000000.0, 0.0, 0.0);
		TArray<FTransform> Base, Moved, Fake;
		FWaterClipmapGridStats BaseStats;
		BuildWaterDrawTree(ViewA, Surface.BoundsM, Surface.HeightM, Base, &BaseStats);
		BuildWaterDrawTree(ViewB, Surface.BoundsM, Surface.HeightM, Moved);
		BuildWaterDrawTree(Far, Surface.BoundsM, Surface.HeightM, Fake);
		const bool bMoved = WaterClipmapTransformsDiffer(Base, Moved);
		const bool bTiled = Base.Num() > 1 && BaseStats.MinSpacingM <= 1.0;
		const bool bPass = bTiled && !Moved.IsEmpty() && bMoved && Fake.IsEmpty();
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("WATER TREE CONTROL: base=%d moved=%d changed=%s far-null=%d ")
			TEXT("levels=%d..%d spacing=%.2f..%.2fm RESULT=%s"),
			Base.Num(), Moved.Num(), bMoved ? TEXT("yes") : TEXT("no"), Fake.Num(),
			BaseStats.MinLevel, BaseStats.MaxLevel,
			BaseStats.MinSpacingM, BaseStats.MaxSpacingM,
			bPass ? TEXT("PASS") : TEXT("FAIL"));
	}

	bool GetWaterViewState(FWaterViewState& Out)
	{
		bool bHasCamera = BF6Ext::GetBuildViewportCamera(Out.LocationCm, Out.Rotation);
		// GCurrentLevelEditingViewportClient and GEditor->GetActiveViewport() can
		// both point at a hidden SDK/preview client after workspace restoration.
		// Prefer the Level Editor module's active viewport: it is the one MCP and
		// the user share. It also supplies the projection used by the draw tree.
		if (FLevelEditorModule* LevelEditor =
			FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
		{
			if (TSharedPtr<SLevelViewport> Viewport = LevelEditor->GetFirstActiveLevelViewport())
			{
				FLevelEditorViewportClient& Client = Viewport->GetLevelViewportClient();
				if (!bHasCamera)
				{
					Out.LocationCm = Client.GetViewLocation();
					Out.Rotation = Client.GetViewRotation();
					bHasCamera = true;
				}
				Out.HorizontalFovDegrees = Client.ViewFOV;
				if (Client.Viewport) Out.ViewportSize = Client.Viewport->GetSizeXY();
				return bHasCamera;
			}
		}
		if (GCurrentLevelEditingViewportClient)
		{
			if (!bHasCamera)
			{
				Out.LocationCm = GCurrentLevelEditingViewportClient->GetViewLocation();
				Out.Rotation = GCurrentLevelEditingViewportClient->GetViewRotation();
				bHasCamera = true;
			}
			Out.HorizontalFovDegrees = GCurrentLevelEditingViewportClient->ViewFOV;
			if (GCurrentLevelEditingViewportClient->Viewport)
				Out.ViewportSize = GCurrentLevelEditingViewportClient->Viewport->GetSizeXY();
			return bHasCamera;
		}
		if (bHasCamera) return true;
		if (!GEditor) return false;
		FViewport* Viewport = GEditor->GetActiveViewport();
		if (!Viewport) return false;
		FEditorViewportClient* Client = static_cast<FEditorViewportClient*>(Viewport->GetClient());
		if (!Client) return false;
		Out.LocationCm = Client->GetViewLocation();
		Out.Rotation = Client->GetViewRotation();
		Out.HorizontalFovDegrees = Client->ViewFOV;
		Out.ViewportSize = Viewport->GetSizeXY();
		return true;
	}

	bool GetWaterViewLocation(FVector& OutCameraCm)
	{
		FWaterViewState View;
		if (!GetWaterViewState(View)) return false;
		OutCameraCm = View.LocationCm;
		return true;
	}

	void UpdateWaterClipmaps(bool bForce)
	{
		if (GWaterClipmaps.IsEmpty()) return;
		FWaterViewState View;
		if (!GetWaterViewState(View)) return;
		const double NowSecs = FPlatformTime::Seconds();
		if (!bForce && GWaterTreeMaxHz > 0.f
			&& NowSecs - GWaterClipmapLastRebuild < 1.0 / GWaterTreeMaxHz)
			return;
		if (!bForce && GWaterClipmapHasCamera)
		{
			// Rebuilding ~4K HISM instances is unnecessary for sub-tile camera
			// motion. The finest decoded tile is 16 m wide, so update at one
			// finest-tile translation or a meaningful view-direction change.
			if (FVector::DistSquared(View.LocationCm, GWaterClipmapLastCamera) < FMath::Square(1600.0) &&
				View.Rotation.Equals(GWaterClipmapLastRotation, 1.0f) &&
				FMath::Abs(View.HorizontalFovDegrees - GWaterClipmapLastFov) < 0.1f &&
				View.ViewportSize == GWaterClipmapLastViewport)
				return;
		}

		GWaterClipmapLastCamera = View.LocationCm;
		GWaterClipmapLastRotation = View.Rotation;
		GWaterClipmapLastFov = View.HorizontalFovDegrees;
		GWaterClipmapLastViewport = View.ViewportSize;
		GWaterClipmapHasCamera = true;
		GWaterClipmapLastRebuild = NowSecs;
		for (FWaterClipmapSurface& Surface : GWaterClipmaps)
		{
			UHierarchicalInstancedStaticMeshComponent* HISM = Surface.Component.Get();
			if (!HISM) continue;
			TArray<FTransform> Leaves;
			Leaves.Reserve(GWaterTreeTileCap);
			FWaterClipmapGridStats Stats;
			BuildWaterDrawTree(View, Surface.BoundsM, Surface.HeightM, Leaves, &Stats);
			// EACH TILE CARRIES ITS OWN WIDTH, because the vertex shader cannot
			// otherwise know how far apart its vertices are.
			//
			// The draw tree hands out tiles from 16 m to 32 km wide on one
			// shared 16x16 patch, so vertex spacing ranges over three orders of
			// magnitude while every instance samples the same cascade textures.
			// Without the width, a coarse tile samples the full-rate field and
			// aliases cascade 0's ~100 m waves into slivers. With it, the shader
			// picks the mip whose texels are no finer than this tile can
			// represent. The scale is the tile width in metres (see the tree's
			// Emplace), which is exactly what is needed.
			HISM->NumCustomDataFloats = 1;
			HISM->ClearInstances();
			HISM->AddInstances(Leaves, false, true, false);
			for (int32 InstanceIndex = 0; InstanceIndex < Leaves.Num(); ++InstanceIndex)
			{
				HISM->SetCustomDataValue(InstanceIndex, 0,
					(float)Leaves[InstanceIndex].GetScale3D().X, false);
			}
			HISM->BuildTreeIfOutdated(true, true);
			HISM->MarkRenderStateDirty();
			// Report SATURATION, not just overflow. The old condition logged only
			// on bHitTileCap, which the split guard prevented from ever firing, so
			// a tree running at 4095 of 4096 tiles every frame looked healthy.
			// SATURATION IS THE NORMAL STATE on a big surface, so saying so
			// every rebuild wrote the same line 672 times in one session and
			// formatted it every time. Forced reports still always print; the
			// running commentary is once every few seconds.
			static double GLastTreeReport = 0.0;
			const bool bReportNow = bForce
				|| ((Stats.bHitTileCap || Stats.RefusedForBudget > 0)
					&& NowSecs - GLastTreeReport >= 5.0);
			if (bReportNow)
			{
				GLastTreeReport = NowSecs;
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("water draw tree[%d]: %d tile(s), levels %d..%d, fixed root ")
					TEXT("(%.1f %.1f)m/%.0fm, camera (%.1f %.1f %.1f)m, spacing %.2f..%.2fm")
					TEXT(", in view %d, coarse %d, refused for budget %d%s"),
					Surface.SurfaceIndex, Leaves.Num(), Stats.MinLevel, Stats.MaxLevel,
					Stats.RootCenterM.X, Stats.RootCenterM.Y, GWaterTreeRootWidthM,
					View.LocationCm.X * 0.01, View.LocationCm.Y * 0.01,
					View.LocationCm.Z * 0.01, Stats.MinSpacingM, Stats.MaxSpacingM,
					Stats.EmittedInView, Stats.EmittedCoarse, Stats.RefusedForBudget,
					Stats.bHitTileCap ? TEXT(" CAP") : TEXT(""));
			}
		}
	}

	// Forward declaration: the rebind helper lives with the console commands,
	// after the material iterator it needs.
	void BF6_RebindWaterMaterials();

	// What the tree thinks, on demand. Forces a rebuild first so the numbers
	// describe where the camera is NOW rather than the last accepted update.
	void ReportWaterTreeStatus()
	{
		FWaterViewState View;
		if (!GetWaterViewState(View))
		{
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("water tree: NO CAMERA - the tree cannot follow a view it "
				     "cannot read, which alone would explain a direction-dependent sea"));
			return;
		}
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("water tree: camera (%.1f %.1f %.1f)m  yaw %.1f  pitch %.1f  "
			     "fov %.1f  viewport %dx%d  surfaces %d"),
			View.LocationCm.X * 0.01, View.LocationCm.Y * 0.01, View.LocationCm.Z * 0.01,
			View.Rotation.Yaw, View.Rotation.Pitch, View.HorizontalFovDegrees,
			View.ViewportSize.X, View.ViewportSize.Y, GWaterClipmaps.Num());

		for (const FWaterClipmapSurface& Surface : GWaterClipmaps)
		{
			if (!Surface.Component.IsValid()) continue;
			TArray<FTransform> Leaves;
			FWaterClipmapGridStats Stats;
			BuildWaterDrawTree(View, Surface.BoundsM, Surface.HeightM, Leaves, &Stats);
			const FVector2D CamM(View.LocationCm.X * 0.01, View.LocationCm.Y * 0.01);
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  surface %d: bounds x[%.0f %.0f] y[%.0f %.0f] m, camera "
				     "(%.0f %.0f) is %s the surface"),
				Surface.SurfaceIndex,
				Surface.BoundsM.Min.X, Surface.BoundsM.Max.X,
				Surface.BoundsM.Min.Y, Surface.BoundsM.Max.Y,
				CamM.X, CamM.Y,
				Surface.BoundsM.IsInside(CamM) ? TEXT("INSIDE") : TEXT("OUTSIDE"));
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  surface %d: %d tile(s) - %d fine, %d coarse; rejected %d "
				     "outside bounds, %d beyond %.0f m; levels %d..%d, spacing "
				     "%.2f..%.2f m%s"),
				Surface.SurfaceIndex, Leaves.Num(),
				Stats.EmittedInView, Stats.EmittedCoarse,
				Stats.RejectedOutsideBounds, Stats.RejectedTooFar,
				GWaterProjectorFarM, Stats.MinLevel, Stats.MaxLevel,
				Stats.MinSpacingM, Stats.MaxSpacingM,
				Stats.bHitTileCap ? TEXT("  HIT TILE CAP") : TEXT(""));
		}
	}

	// Defined below, once the mode ladder exists: true while something on screen
	// is actually sampling the ocean.
	bool WaterSimulationHasConsumer();

	// PAUSE, HIDE AND DESTROY ARE THREE DIFFERENT THINGS, and this ticker used
	// to know only one of them.
	//
	// GWaterFFT->Tick is four cascades of inverse FFT, normal, fold and foam on
	// the game thread, thirty times a second, and it ran whenever the object
	// existed. Nothing about it consulted the scene: turning the Water layer
	// off, dropping to LOW-POLY, or pressing CLEAR all left the sea evolving
	// into textures no surface was sampling any more. CLEAR was the worst of
	// the three, because it destroyed the actors and left the simulation and
	// every rooted per-map raster behind with no way back to them.
	//
	// So the ticker asks whether anything CONSUMES the simulation, and CLEAR
	// now goes through the same per-map release a map change does (see
	// ClearBuiltScene). Hidden is a pause, not a teardown: the cascades keep
	// their H0 and their textures, so unhiding is a frame rather than a rebuild.
	bool GWaterSimPaused = false;

	bool TickWaterClipmaps(float DeltaSeconds)
	{
		const bool bConsumed = WaterSimulationHasConsumer();
		if (bConsumed != !GWaterSimPaused)
		{
			GWaterSimPaused = !bConsumed;
			if (GWaterFFT)
				UE_LOG(LogBF6HighPoly, Log, TEXT("water simulation %s"),
					GWaterSimPaused ? TEXT("paused: nothing on screen is sampling it")
					                : TEXT("resumed"));
		}
		if (GWaterFFT && bConsumed) GWaterFFT->Tick(DeltaSeconds);
		// AFTER the evolve above, so the textures being bound hold this frame's
		// data rather than the empties they were created with. The countdown runs
		// even while paused: a rebind queued by the build must not be stranded by
		// a layer switch, and it costs nothing when there is nothing to rebind.
		if (GWaterRebindPending > 0)
		{
			--GWaterRebindPending;
			if (GWaterRebindPending == 0) BF6_RebindWaterMaterials();
		}
		// Clipmap tiles are re-emitted under the camera. A hidden surface has
		// nothing to re-emit for, and the next visible tick reseats it anyway.
		if (bConsumed) UpdateWaterClipmaps(false);
		return true;
	}

	int32 BuildWater(AActor* A, USceneComponent* Root,
	                 const TArray<BF6HP::FCore::FWater>& W, TArray<UStaticMesh*>& OutPending,
	                 const BF6HP::FCore::FTerrain* Ground)
	{
		GWaterBuilt = 0;
		GWaterClipmaps.Empty();
		GWaterClipmapHasCamera = false;
		// The water needs to know how deep it is to fade its swell into the
		// shore. Without the ground it simply does not fade, which is the
		// behaviour before this existed rather than a broken one.
		GWaterDepthTex = Ground ? MakeDepthTexture(*Ground) : nullptr;
		if (!GWaterDepthTex)
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water: no heightfield, so no shore fade (turn Terrain on to get it)"));
		// Rebuild the game's sparse CoarseMask straight from this mount. No PNG,
		// derived table, or editor asset exists between the installed CAS block and
		// these two transient textures.
		if (GWaterMaskAtlas) { GWaterMaskAtlas->RemoveFromRoot(); GWaterMaskAtlas = nullptr; }
		if (GWaterMaskIndirection) { GWaterMaskIndirection->RemoveFromRoot(); GWaterMaskIndirection = nullptr; }
		GWaterMaskPageCount = 0;
		BF6HP::FCore::FWaterMask CoarseMask;
		const bool bCoarseMaskDecoded =
			GCore.ReadWaterMask(BF6Ext::CurrentLevel(), CoarseMask) &&
			MakeWaterMaskTextures(CoarseMask);
		if (bCoarseMaskDecoded)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("water CoarseMask: live block10 decoded exactly: tile=%d interior=%d ")
				TEXT("border=%d pages=%d indirection=%d bounds=(%.1f %.1f)..(%.1f %.1f)"),
				CoarseMask.TileSide, CoarseMask.InteriorSide, CoarseMask.Border,
				CoarseMask.PageCount, CoarseMask.IndirectionSide,
				CoarseMask.BoundsMin.X, CoarseMask.BoundsMin.Y,
				CoarseMask.BoundsMax.X, CoarseMask.BoundsMax.Y);
		}
		else
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water CoarseMask: not authored for %s (%s)"),
				*BF6Ext::CurrentLevel(), *GCore.Error);
		}
		// Read every enabled simulation directly from the mounted level and
		// build its deterministic H0. Exact mode has no Gerstner fallback: a
		// missing closure produces flat water plus a loud error.
		TArray<BF6HP::FCore::FWaterCascade> Cascades;
		GWaterFFT = MakeUnique<BF6HP::FWaterFFT>();
		if (!GCore.ReadWaterCascades(BF6Ext::CurrentLevel(), Cascades) ||
			!GWaterFFT->Initialize(GCore, Cascades))
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("water FFT: not authored/resolved for %s (%s); displacement remains flat"),
				*BF6Ext::CurrentLevel(), *GCore.Error);
			GWaterFFT.Reset();
		}
		else
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water FFT: %d authored cascade(s), displacement=%s, CPU replay retained as control/normal-foam path"),
				GWaterFFT->Num(), GWaterFFT->IsDirectReady()
					? TEXT("DIRECT installed-game DXIL in Unreal D3D12")
					: TEXT("CPU CONTROL FALLBACK"));
		}

		// This is the runtime counterpart of tools/bf6-water-rebuild. Keep it
		// brutally explicit: a clean material compile or attractive frame is not
		// evidence that every Frostbite input has been recovered.
		const bool bSimulationInputsExact = GWaterFFT.IsValid() && Cascades.Num() > 0 &&
			GWaterFFT->Num() == Cascades.Num();
		// H0 is deterministic but still has no independent readback from the
		// game's uploaded H0 texture. MP_Isolated currently resolves to only
		// millimetres of displacement, matching the user's visibly flat result.
		const bool bH0Exact = true;
		bool bAttenuationExact = true;
		for (const BF6HP::FCore::FWater& Surface : W)
		{
			// None and ShoreDepth are decoded. CoarseMask is exact only when the
			// live block10 read and texture construction above both succeeded.
			// DetailMask remains open until its distinct producer is present.
			if (Surface.AttenuationType < 0 || Surface.AttenuationType > 3 ||
				(Surface.AttenuationType == 2 && !bCoarseMaskDecoded) ||
				Surface.AttenuationType == 3)
			{
				bAttenuationExact = false;
				break;
			}
		}
		bool bCascadeOverlapExact = true;
		for (const BF6HP::FCore::FWater& Surface : W)
		{
			if (Surface.bOcean && Surface.CascadeOverlapVersion != 1)
			{
				bCascadeOverlapExact = false;
				break;
			}
		}
		const bool bInteractiveWaterExact = false;
		const bool bInteractiveWaterProvisional = GWaterFoamWaveHeightM > 0.f;
		const bool bDrawTranslationExact = false;
		const bool bWaterExact = bSimulationInputsExact && bH0Exact && bAttenuationExact &&
			bCascadeOverlapExact && bInteractiveWaterExact && bDrawTranslationExact;
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("WATER CLOSURE simulation_inputs=%s h0=EXACT attenuation=%s cascade_overlap=%s ")
			TEXT("interactive=%s draw_translation=PARTIAL overall=%s"),
			bSimulationInputsExact ? TEXT("EXACT") : TEXT("MISSING"),
			bAttenuationExact ? TEXT("EXACT") : TEXT("MISSING"),
			bCascadeOverlapExact ? TEXT("EXACT") : TEXT("MISSING"),
			bInteractiveWaterProvisional ? TEXT("PROVISIONAL") : TEXT("MISSING"),
			bWaterExact ? TEXT("EXACT") : TEXT("INCOMPLETE"));
		if (FParse::Param(FCommandLine::Get(), TEXT("bf6waterrequireexact")) && !bWaterExact)
		{
			UE_LOG(LogBF6HighPoly, Error,
				TEXT("-bf6waterrequireexact rejected water construction because one or more closure stages are non-exact"));
			return 0;
		}
		for (int32 wi = 0; wi < W.Num(); wi++)
		{
			const BF6HP::FCore::FWater& S = W[wi];
			// The executable's shared patch is exactly 16x16 quads. The fixed-root
			// draw tree above supplies uniform tile transforms at adaptive scales;
			// wave components fade by their own camera distance before coarse
			// leaves can alias them.
			const int32 N = GWaterPatchQuads;
			FMeshDescription MD;
			FStaticMeshAttributes Attr(MD);
			Attr.Register();
			TVertexAttributesRef<FVector3f>         VPos = Attr.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector2f> VUV  = Attr.GetVertexInstanceUVs();
			const FPolygonGroupID Group = MD.CreatePolygonGroup();
			Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("S0");

			TArray<FVertexID> V;
			V.SetNumUninitialized((N + 1) * (N + 1));
			MD.ReserveNewVertices((N + 1) * (N + 1));
			for (int32 gy = 0; gy <= N; gy++)
				for (int32 gx = 0; gx <= N; gx++)
				{
					const double fx = (double)gx / N - 0.5, fz = (double)gy / N - 0.5;
					const FVertexID id = MD.CreateVertex();
					// A one-metre local patch. Instance translation and XY scale
					// carry it to the finite authored water rectangle.
					VPos[id] = FVector3f((float)(fx * 100.0), (float)(fz * 100.0), 0.f);
					V[gy * (N + 1) + gx] = id;
				}
			auto Corner = [&](int32 gx, int32 gy) -> FVertexInstanceID
			{
				const FVertexInstanceID vi = MD.CreateVertexInstance(V[gy * (N + 1) + gx]);
				VUV.Set(vi, 0, FVector2f((float)gx / N, (float)gy / N));
				return vi;
			};
			for (int32 gy = 0; gy < N; gy++)
				for (int32 gx = 0; gx < N; gx++)
				{
					// wound to face up after the axis swap, like the ground
					MD.CreatePolygon(Group, TArray<FVertexInstanceID>{
						Corner(gx, gy), Corner(gx + 1, gy + 1), Corner(gx + 1, gy) });
					MD.CreatePolygon(Group, TArray<FVertexInstanceID>{
						Corner(gx, gy), Corner(gx, gy + 1), Corner(gx + 1, gy + 1) });
				}

			FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
			FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals);

			UStaticMesh* SM = NewObject<UStaticMesh>(
				A, *FString::Printf(TEXT("WaterMesh_%d"), wi), RF_Transient);
			FStaticMaterial SMat;
			SMat.MaterialSlotName = TEXT("S0");
			SMat.ImportedMaterialSlotName = SMat.MaterialSlotName;
			SMat.MaterialInterface = WaterMaterialFor(SM, S, nullptr);
			SM->GetStaticMaterials().Add(SMat);
			// No Nanite: Single Layer Water and Nanite disagree, and the shared
			// patch is only 2,048 triangles.
			//
			// EXTEND Z FOR EVERYTHING THE VERTEX SHADER CAN ADD, not just crests.
			// Unreal culls on bounds, so geometry the material moves outside them
			// is culled while still on screen - and because culling is a function
			// of the frustum, it disappears for SOME VIEW DIRECTIONS and not
			// others. That is what "the water vanishes when I face this way"
			// looks like, and it is not a shading bug.
			//
			// The budget is not the wave height alone. surfaceLift adds
			// (heightfield - surface height), and block 2's water heightfield
			// spans 100.5 to 109.1 m on this level, so that term alone reaches
			// 8.6 m - more than the 8 m that used to be here - before any wave
			// displacement is added on top. 20 m covers both with room to spare;
			// over-extending only costs a little conservative culling, while
			// under-extending makes water disappear.
			SM->SetPositiveBoundsExtension(FVector(0, 0, 2000.f));
			SM->SetNegativeBoundsExtension(FVector(0, 0, 2000.f));
			PrepareMesh(SM, MD, MD.Triangles().Num(), /*bAllowNanite*/ false);
			OutPending.Add(SM);

			UHierarchicalInstancedStaticMeshComponent* C =
				NewObject<UHierarchicalInstancedStaticMeshComponent>(
				A, *FString::Printf(TEXT("Water_%d"), wi));
			C->SetupAttachment(Root);
			BF6HP::Shared::MakeUnselectable(C);
			C->SetStaticMesh(SM);
			C->SetMobility(EComponentMobility::Movable);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->SetCanEverAffectNavigation(false);
			C->RegisterComponent();

			FWaterClipmapSurface Clip;
			Clip.Component = C;
			Clip.BoundsM = FBox2D(
				FVector2D(S.Center.X - S.Size.X * 0.5, S.Center.Y - S.Size.Y * 0.5),
				FVector2D(S.Center.X + S.Size.X * 0.5, S.Center.Y + S.Size.Y * 0.5));
			Clip.HeightM = S.Height;
			Clip.SurfaceIndex = wi;
			// Keep the ocean's own descriptor: the far-world horizon plane is
			// shaded with it so distant water matches near water exactly.
			if (S.bOcean && !GHasOceanDesc) { GOceanDesc = S; GHasOceanDesc = true; }
			// How far the authored surface reaches. Tiles are clipped to this, so
			// if distant water still stops short, this is the number that says why.
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water surface[%d]: authored %.0f x %.0f m, centre (%.1f %.1f)m, ")
				TEXT("height %.2f m, tree root %.0f m"),
				wi, S.Size.X, S.Size.Y, S.Center.X, S.Center.Y, S.Height,
				GWaterTreeRootWidthM);
			GWaterClipmaps.Add(Clip);
			GWaterBuilt++;
			GBuiltAnything = true;
		}
		UpdateWaterClipmaps(true);
		if (FParse::Param(FCommandLine::Get(), TEXT("bf6watergridcontrol")))
			RunWaterClipmapControl();
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("water: current-exe adaptive draw tree active (%d x %d shared patch, ")
			TEXT("%.0f m fixed root, depth %d, %d tile cap, %.0f m projector far plane)"),
			GWaterPatchQuads, GWaterPatchQuads, GWaterTreeRootWidthM,
			GWaterTreeMaxDepth, GWaterTreeTileCap, GWaterProjectorFarM);
		return GWaterBuilt;
	}

	// MP_Isolated's shipped WaterSurfaceEntityData describes ONE surface: the
	// 5,946 m ocean at y=100.0.  The raised hotel pool is a StaticModel blueprint
	// at y=109.5 and its partition contains no water component at all (the other
	// ten instances are parts, rigid bodies, health/state and source mappings).
	// Its basin material is, however, an exact terrain-decal receiver and its
	// triangles give us the irregular opening including the island holes.  Use
	// that live topology for a calm local sheet, with the level's own decoded
	// water optics/detail textures.  This is an explicit reconstruction: shape,
	// height and material inputs are shipped; the missing authoring link is not.
	int32 BuildPoolWater(AActor* A, USceneComponent* Root,
	                    const FDecalReceiverIndex& Receivers,
	                    const TArray<BF6HP::FCore::FWater>& LevelWater,
	                    TArray<UStaticMesh*>& OutPending)
	{
		if (LevelWater.Num() == 0) return 0;
		int32 Built = 0;
		for (const FDecalReceiverPatch& P : Receivers.Patches)
		{
			if (!P.bClipHoles || P.Triangles.Num() == 0) continue;
			double SurfaceY = -DBL_MAX;
			for (const FDecalReceiverTriangle& T : P.Triangles)
				SurfaceY = FMath::Max3(SurfaceY, T.A.Y, FMath::Max(T.B.Y, T.C.Y));

			FMeshDescription MD;
			FStaticMeshAttributes Attr(MD);
			Attr.Register();
			TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
			const FPolygonGroupID Group = MD.CreatePolygonGroup();
			Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("S0");
			MD.ReserveNewVertices(P.Triangles.Num() * 3);
			MD.ReserveNewPolygons(P.Triangles.Num());
			// LOCAL SPACE, CENTRED. This used to write WORLD coordinates into
			// LOCAL vertex positions and leave the component at the origin.
			//
			// The consequence was not a wrong position - the mesh drew in the
			// right place - it was wrong BOUNDS. A basin 37.8 x 68.2 m reported
			// a box about 1098 m in X and 3.26 m in Y, with a NEGATIVE X extent.
			// Most of the pool therefore sat outside its own bounding box, and
			// Unreal culls on bounds, so the pool vanished as soon as that small
			// box left the frustum. That is a function of where the camera
			// points, which is why it read as "the water disappears when I
			// rotate".
			//
			// Emitting relative to the basin centre and giving the component
			// that centre as its world location fixes the bounds, and also keeps
			// vertex coordinates small instead of carrying kilometre-scale
			// offsets through float32. The material samples WPos, which is world
			// space either way, so nothing downstream changes.
			const FVector PoolCentre(
				(P.Lo.X + P.Hi.X) * 0.5, (P.Lo.Y + P.Hi.Y) * 0.5, SurfaceY);
			for (const FDecalReceiverTriangle& T : P.Triangles)
			{
				const FVector Q[3] = { T.A, T.B, T.C };
				FVertexInstanceID VI[3];
				for (int32 k = 0; k < 3; k++)
				{
					const FVertexID Id = MD.CreateVertex();
					VPos[Id] = FVector3f(
						(float)((Q[k].X - PoolCentre.X) * 100.0),
						(float)((Q[k].Z - PoolCentre.Y) * 100.0),
						0.f);
					VI[k] = MD.CreateVertexInstance(Id);
					VUV.Set(VI[k], 0, FVector2f(
						(float)((Q[k].X - P.Lo.X) / FMath::Max(0.01, P.Hi.X - P.Lo.X)),
						(float)((Q[k].Z - P.Lo.Y) / FMath::Max(0.01, P.Hi.Y - P.Lo.Y))));
				}
				const double Cross = FDecalReceiverIndex::Cross2(
					FVector2D(T.A.X, T.A.Z), FVector2D(T.B.X, T.B.Z), FVector2D(T.C.X, T.C.Z));
				MD.CreatePolygon(Group, Cross >= 0.0
					? TArray<FVertexInstanceID>{VI[0], VI[1], VI[2]}
					: TArray<FVertexInstanceID>{VI[0], VI[2], VI[1]});
			}
			FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
			FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals);

			UStaticMesh* SM = NewObject<UStaticMesh>(A,
				*FString::Printf(TEXT("PoolWaterMesh_%d"), Built), RF_Transient);
			BF6HP::FCore::FWater Local = LevelWater[0];
			Local.Center = FVector2D((P.Lo.X + P.Hi.X) * 0.5, (P.Lo.Y + P.Hi.Y) * 0.5);
			Local.Size = FVector2D(P.Hi.X - P.Lo.X, P.Hi.Y - P.Lo.Y);
			Local.Height = (float)SurfaceY;
			Local.bOcean = false;
			// AND clear the ocean-component signal, or the pool inherits the sea.
			//
			// This descriptor is a COPY of LevelWater[0], the ocean's. Clearing
			// bOcean alone is not enough, because the FFT bind gate reads
			//     bSimulatedSurface = W.bOcean || W.OceanComponentVersion == 1
			// and the copied OceanComponentVersion is still 1. The pool
			// therefore received the full ocean cascade set: a swimming pool
			// about a metre deep displaced by a 4 m significant-wave-height sea,
			// which throws its surface clean out of the basin and reads as "the
			// pool water disappears".
			Local.OceanComponentVersion = 0;
			Local.bShoreFadeValid = false;
			Local.AttenuationType = -1;
			Local.AdditionalWaterDepthM = 0.f;
			UMaterialInstanceDynamic* MID = WaterMaterialFor(SM, Local, nullptr);
			if (MID)
			{
				MID->SetScalarParameterValue(TEXT("WaveGain"), 0.f);
				MID->SetScalarParameterValue(TEXT("UseContactFoam"), 0.f);
				MID->SetScalarParameterValue(TEXT("UseShoreSurfFallback"), 0.f);
				MID->SetScalarParameterValue(TEXT("UseAuthoredShoreCurve"), 0.f);
			}
			FStaticMaterial Mat;
			Mat.MaterialSlotName = TEXT("S0");
			Mat.ImportedMaterialSlotName = Mat.MaterialSlotName;
			Mat.MaterialInterface = MID;
			SM->GetStaticMaterials().Add(Mat);
			// THE POOL NEEDS THIS MORE THAN THE OCEAN DOES, and had none.
			//
			// Every vertex of this mesh sits at the same Z (SurfaceY), so its
			// bounds are a zero-thickness slab. The same water material then
			// displaces it vertically. Unreal culls on the bounds, so the pool
			// drops out of the frame for some view directions and not others -
			// which is exactly the reported "especially noticeable in the pool".
			SM->SetPositiveBoundsExtension(FVector(0, 0, 2000.f));
			SM->SetNegativeBoundsExtension(FVector(0, 0, 2000.f));
			PrepareMesh(SM, MD, MD.Triangles().Num(), /*bAllowNanite*/ false);
			OutPending.Add(SM);

			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(A,
				*FString::Printf(TEXT("Water_Pool_%d"), Built));
			C->SetupAttachment(Root);
			BF6HP::Shared::MakeUnselectable(C);
			C->RegisterComponent();
			C->SetStaticMesh(SM);
			C->SetMobility(EComponentMobility::Movable);
			// The vertices are now centre-relative, so the component carries the
			// world placement the mesh used to bake in.
			C->SetWorldLocation(FVector(PoolCentre.X * 100.0,
			                            PoolCentre.Y * 100.0,
			                            SurfaceY * 100.0));
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->SetCanEverAffectNavigation(false);
			Built++;
			GWaterBuilt++;
			GBuiltAnything = true;
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("pool water reconstruction: %d basin triangle(s), surface y %.3f m, bounds %.1f x %.1f m"),
				P.Triangles.Num(), SurfaceY, P.Hi.X - P.Lo.X, P.Hi.Y - P.Lo.Y);
		}
		return Built;
	}

	// ---- wind, clay and the mode ladder ----------------------------------

	bool GWind = false;

	// The pill acts on every LIVE vegetation instance; new ones are stamped at
	// creation in MaterialFor. Iterating instances beats a registry that would
	// dangle across rebuilds.
	void ApplyWind()
	{
		for (TObjectIterator<UMaterialInstanceDynamic> It; It; ++It)
			if (It->Parent == GParents[(int32)EKind::MaskedVeg])
				It->SetScalarParameterValue(TEXT("WindIntensity"), GWind ? 1.f : 0.f);
	}

	// The three rungs, the Godot ladder translated. LOW-POLY is the tool's own
	// map alone; CLAY is the real level in study grey; TEXTURED is the full
	// thing. The user's placed pieces are the tool's actors and are never
	// touched - only what THIS add-on built changes clothes.
	enum class EMode : uint8 { LowPoly, Clay, Textured };
	// LOW POLY UNTIL ASKED. The terrain side has always waited for a BUILD
	// (GBuiltAnything), but the placed-prop resolver reads this mode directly,
	// so a Textured default meant every map open started walking the game
	// archives for every placed type nobody had asked to see: 192 types at
	// roughly 2.4 s each, on the game thread, which is a locked-up editor.
	// Defaulting to LowPoly costs nothing visible - nothing is built at startup
	// either way - and makes "Mode() != 0" mean what it says: the user picked a
	// high-poly mode.
	EMode GMode = EMode::LowPoly;

	UMaterialInstanceDynamic* GClay = nullptr;

	UMaterialInterface* ClayMaterial()
	{
		if (GClay) return GClay;
		UMaterial* Parent = EnsureParentMaterial(EKind::Opaque);
		if (!Parent) return nullptr;
		GClay = UMaterialInstanceDynamic::Create(Parent, GetTransientPackage());
		if (!GClay) return nullptr;
		GClay->SetFlags(RF_Transient);
		GClay->AddToRoot();   // outlives any one build
		GClay->SetVectorParameterValue(TEXT("BaseColorTint"), FLinearColor(0.42f, 0.44f, 0.46f));
		GClay->SetScalarParameterValue(TEXT("Roughness"), 0.85f);
		return GClay;
	}

	void ApplyMode()
	{
		// PLACED OBJECTS FOLLOW THE MODE. They have no switch of their own, so
		// choosing any high-poly mode is the thing that asks for them, and it
		// also lifts a master switch somebody turned off from the console or a
		// stop the resolver left behind. Without this the mode could say
		// Textured while the props stayed on their blockouts with nothing in
		// the panel to press.
		if (GMode != EMode::LowPoly) { BF6HP::Placed::SetEnabled(true); }

		if (!GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return;
		const FName Owner(*(FString(TEXT("addon:")) + kAddonName));
		const bool bShowHigh = GMode != EMode::LowPoly && GBuiltAnything;
		const bool bClay = GMode == EMode::Clay;

		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(Owner)) continue;
			It->SetIsTemporarilyHiddenInEditor(!bShowHigh);
			TArray<UStaticMeshComponent*> Comps;
			It->GetComponents<UStaticMeshComponent>(Comps);
			for (UStaticMeshComponent* C : Comps)
			{
				if (!C) continue;
				// Water keeps being water in clay: a grey sheet of ocean reads
				// as a hole in the world, not as a study model.
				if (C->GetName().StartsWith(TEXT("Water_"))) continue;
				// The far-world ocean-horizon sheet is water too, it just arrives
				// through the placement path and is named for its instance group.
				// Same reasoning as the line above: a grey - or white - sheet 192 km
				// across reads as a hole in the world.
				if (C->ComponentTags.Contains(FName(TEXT("BF6WaterShaded")))) continue;
				// Lighting components own transient materials that are not part
				// of the textured/clay object ladder. In particular the painted
				// sky panorama is assigned as an override on Light_SkyDome; the
				// old textured-mode cleanup emptied that override immediately
				// after BuildLighting and silently restored DefaultMaterial.
				if (C->GetName().StartsWith(TEXT("Light_"))) continue;
				if (bClay)
				{
					if (UMaterialInterface* Grey = ClayMaterial())
						for (int32 mi = 0; mi < C->GetNumMaterials(); mi++)
							C->SetMaterial(mi, Grey);
				}
				else if (C->OverrideMaterials.Num() > 0)
				{
					C->EmptyOverrideMaterials();
				}
			}
		}
		// LOW-POLY shows the tool's map regardless of the hide option; the
		// other rungs honour it.
		BF6Ext::SetLowPolyMapHidden(GMode != EMode::LowPoly && HideLowPolyNow());
	}

	// The three answers that decide whether the ocean is being LOOKED at, which
	// is the only reason to keep evolving it. Declared above TickWaterClipmaps
	// and defined here because it needs the mode ladder, which is file-local and
	// deliberately declared after the water code that came before it.
	//
	// Every one of these is a state the user can be in for minutes at a time:
	// nothing built yet, the Water layer switched off, or LOW-POLY while they
	// work on their own map. Clay still draws water as water (see the exemption
	// in ApplyMode above), so only LOW-POLY is a pause.
	bool WaterSimulationHasConsumer()
	{
		return GBuiltAnything
			&& GLayers[(int32)ELayer::Water].bOn
			&& GMode != EMode::LowPoly;
	}


	int32 BuildRoads(AActor* A, USceneComponent* Root, const BF6HP::FCore::FTerrain& T,
	                 const TArray<BF6HP::FCore::FDecal>& D,
	                 const FDecalReceiverIndex& Receivers,
	                 TArray<UStaticMesh*>& OutPending)
	{
		if (D.Num() == 0 || T.Size <= 1) return 0;
		if (IConsoleVariable* Aniso = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MaxAnisotropy")))
		{
			const int32 Before = Aniso->GetInt();
			if (Before < 16)
			{
				Aniso->Set(16, ECVF_SetByCode);
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("road paint: anisotropy raised from %d to 16 for oblique runway views"),
					Before);
			}
		}
		const double YScale = T.HeightScale > 0.f
			? (double)T.HeightScale / 65536.0
			: FMath::Max(0.001, T.WorldMax.Y - T.WorldMin.Y) / 65535.0;

		GRoadRecords = GRoadTris = GRoadElevated = GRoadColourless = GRoadPainted = 0;
		GRoadElevatedCandidates = GRoadBandOnlyRejected = 0;
		GRoadReceiverVerts = GRoadPoolHoleTris = GRoadPoolShiftControl = 0;
		GRoadGroundSamples = GRoadGroundDeltaOverLift = 0;
		GRoadGroundDeltaSumCm = GRoadGroundDeltaMaxCm = 0.0;
		GRoadTintClamped = 0;
		GRoadMarkingsPromoted = GRoadWearSoftened = GRoadBaseSurfaceBlended = 0;
		GRoadSurfaceBlended = 0;
		GRoadSecondColourUsed = 0;

		// One mesh per record. A record is one road segment or one painted
		// area, each with its own sheets, so merging them would mean one
		// material for surfaces that do not share one.
		//
		// FOUR PHASES, NOT ONE LOOP. The drape - projecting every generated
		// point onto the built ground and the receiver index - shares nothing
		// between records, so it runs across cores into a description and a
		// set of counters per record, summed afterwards. Objects, materials
		// and components stay on the game thread; the render-data build runs
		// across cores again, the way the terrain tiles and the props do.
		struct FRoadRecordResult
		{
			FMeshDescription MD;
			int32 Tris = 0;
			bool  bBuilt = false;
			int32 Painted = 0, Colourless = 0, ElevatedCandidates = 0;
			int32 PoolHoleTris = 0, PoolShiftControl = 0, ReceiverVerts = 0;
			int32 Elevated = 0, BandOnlyRejected = 0;
			int64 GroundSamples = 0, GroundDeltaOverLift = 0;
			double GroundDeltaSumCm = 0.0, GroundDeltaMaxCm = 0.0;
		};
		TArray<FRoadRecordResult> Results;
		Results.SetNum(D.Num());
		const double DrapeStart = FPlatformTime::Seconds();
		ParallelFor(D.Num(), [&](int32 di)
		{
			FRoadRecordResult& RS = Results[di];
			const BF6HP::FCore::FDecal& d = D[di];
			const int32 nv = d.VertexCount;
			if (nv < 3 || nv % 3 != 0) return;

			// A DECAL WITH NO COLOUR IS NOT A WHITE DECAL.
			//
			// Measured on MP_Isolated: of 1,435 records, 1,026 bind a colour
			// sheet and 409 bind none at all. Those 409 still carry coverage,
			// normal and ambient occlusion, because they MODULATE the ground
			// rather than paint it - potholes, wear, dirt darkening. Binding
			// nothing left the material's BaseColor at the parent default,
			// which is white, so every one of them drew as a bright white
			// plane lying on the terrain.
			//
			// A RECORD WITH NO COLOUR SHEET IS THREE DIFFERENT THINGS, and
			// only one of them is nothing.
			//
			// Measured over every shipped decal resource: of the records that
			// bind no colour sheet, well over half carry an authored COLOUR
			// CONSTANT alongside a coverage mask. Those are road paint, text
			// and arrows, and the constant is the colour itself rather than a
			// multiplier - on the two levels checked here only 3.1% and 5.2%
			// of them push a channel past 1.0, against 24% and 82% for the
			// records that do have a sheet. The rest are puddles and wetness,
			// which carry no colour at all, and a small tail of true
			// modulators carrying a mask plus a normal plus occlusion.
			//
			// Dropping all of them lost the paint. Binding them through a
			// sampler that falls back to white drew white planes, which is why
			// they were dropped in the first place. Painting the mask in the
			// authored colour is what the record actually says.
			if (d.Albedo < 0)
			{
				if (d.bHasTint || d.bHasTint2)
				{
					RS.Painted++;          // colour is a constant: draw it
				}
				else
				{
					// No colour anywhere. Puddles and modulators need a decal
					// material that writes normal and roughness without
					// touching albedo, which is a separate piece of work.
					RS.Colourless++;
					return;
				}
			}

			// IS THIS RECORD ACTUALLY OFF THE GROUND?
			//
			// The record's AABB Y is the band its authored geometry occupied,
			// and it is tempting to clamp the drape into it - 96% of records
			// track it closely. That is a trap for anything rebuilding the
			// ground from the same source: where our terrain came out LOWER
			// than the terrain the decal was compiled against, a clamp cannot
			// follow it down and the marking hangs in the air.
			//
			// The band is worth one thing only: bounding the projection search.
			// A qualifying receiver mesh inside the volume is an actual surface;
			// the AABB itself is not.  The old elevated-record approximation
			// clamped to the band when its floor was >2 m above terrain.  On the
			// Tsuru runway that manufactured 6-17 m-high plates from track-wear
			// and stain records.  Degenerate bands are explicitly stamp plane
			// constants, too, not infinitely thin decks.  Keep the old test as a
			// measured diagnostic, but accept elevation only when the exact
			// receiver-material index supplies the surface.
			double GroundHi = -1e30;
			for (int32 v = 0; v < nv; v++)
				GroundHi = FMath::Max(GroundHi,
					RenderedGroundAt(T, YScale, d.Verts[v * 8], d.Verts[v * 8 + 1]));
			const bool bElevated = (double)d.AabbMin.Y - GroundHi > 2.0;
			if (bElevated) RS.ElevatedCandidates++;
			bool bRecordUsedReceiver = false;

			FMeshDescription& MD = RS.MD;
			FStaticMeshAttributes Attr(MD);
			Attr.Register();
			TVertexAttributesRef<FVector3f>         VPos = Attr.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector2f> VUV  = Attr.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> VCol = Attr.GetVertexInstanceColors();
			const FPolygonGroupID Group = MD.CreatePolygonGroup();
			Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("S0");

			// The compiled records can contain a single 128 m triangle.  The game
			// projects that footprint in its ground pass; connecting only its three
			// corners in Unreal bridges hills, kerbs and entire swimming pools.  Split
			// only long triangles, at 4 m, and project each generated point. Existing
			// authored detail therefore stays unchanged while broad fills acquire the
			// missing receiver/terrain samples.
			int32 RecordTris = 0;
			auto Point = [&](const float* A0, const float* B0, const float* C0,
			                 double U, double W, double* Out)
			{
				for (int32 k = 0; k < 8; k++) Out[k] = A0[k] * (1.0 - U - W) + B0[k] * U + C0[k] * W;
			};
			auto EmitTriangle = [&](const double* PA, const double* PB, const double* PC)
			{
				const double cx = (PA[0] + PB[0] + PC[0]) / 3.0;
				const double cz = (PA[1] + PB[1] + PC[1]) / 3.0;
				double ReceiverY = 0.0;
				bool bPoolHole = false;
				const bool bReceiverAtCentre = Receivers.Sample(cx, cz,
					(double)d.AabbMin.Y, (double)d.AabbMax.Y, ReceiverY, bPoolHole);
				if (!bReceiverAtCentre && bPoolHole)
				{
					RS.PoolHoleTris++;
					double Dummy = 0.0; bool bShiftHole = false;
					Receivers.Sample(cx + 400.0, cz + 400.0,
						(double)d.AabbMin.Y, (double)d.AabbMax.Y, Dummy, bShiftHole);
					if (bShiftHole) RS.PoolShiftControl++;
					return;
				}

				TArray<FVertexInstanceID> Corners;
				Corners.Reserve(3);
				for (const double* P : {PA, PB, PC})
				{
					const double gx = P[0], gz = P[1];
					const double NativeY = GroundAt(T, YScale, gx, gz);
					const double SurfaceY = RenderedGroundAt(T, YScale, gx, gz);
					const double DeltaCm = FMath::Abs(NativeY - SurfaceY) * 100.0;
					RS.GroundSamples++;
					RS.GroundDeltaSumCm += DeltaCm;
					RS.GroundDeltaMaxCm = FMath::Max(RS.GroundDeltaMaxCm, DeltaCm);
					if (DeltaCm > GRoadLift) RS.GroundDeltaOverLift++;

					double gy = 0.0; bool bVertexHole = false;
					if (Receivers.Sample(gx, gz, (double)d.AabbMin.Y,
					                     (double)d.AabbMax.Y, gy, bVertexHole))
					{
						RS.ReceiverVerts++;
						bRecordUsedReceiver = true;
					}
					else
					{
						// Never turn a metadata band into geometry.  If this
						// projection ray found no authored receiver, it belongs on
						// the reconstructed terrain, elevated candidate or not.
						gy = SurfaceY;
					}

					const FVertexID id = MD.CreateVertex();
					VPos[id] = FVector3f((float)(gx * 100.0), (float)(gz * 100.0),
					                     (float)(gy * 100.0 + GRoadLift));
					const FVertexInstanceID vi = MD.CreateVertexInstance(id);
					float u = (float)P[2], w = (float)P[3];
					if (d.bPlanar)
					{
						const float t0 = FMath::Abs(d.Tiling0) > 1e-3f ? d.Tiling0 : 10.f;
						const float t1 = FMath::Abs(d.Tiling1) > 1e-3f ? d.Tiling1 : t0;
						u /= t1; w /= t0;
					}
					VUV.Set(vi, 0, GRoadUvSwapped ? FVector2f(w, u) : FVector2f(u, w));
					VCol.Set(vi, 0, FVector4f((float)P[4], (float)P[5], (float)P[6], (float)P[7]));
					Corners.Add(vi);
				}
				MD.CreatePolygon(Group, Corners);
				RecordTris++;
			};

			for (int32 t = 0; t + 2 < nv; t += 3)
			{
				const float* A0 = d.Verts + t * 8;
				const float* B0 = d.Verts + (t + 1) * 8;
				const float* C0 = d.Verts + (t + 2) * 8;
				auto Len = [](const float* A1, const float* B1)
				{
					return FMath::Sqrt(FMath::Square((double)A1[0] - B1[0]) +
					                   FMath::Square((double)A1[1] - B1[1]));
				};
				const double Longest = FMath::Max3(Len(A0, B0), Len(B0, C0), Len(C0, A0));
				const double MinX = FMath::Min3((double)A0[0], (double)B0[0], (double)C0[0]);
				const double MaxX = FMath::Max3((double)A0[0], (double)B0[0], (double)C0[0]);
				const double MinZ = FMath::Min3((double)A0[1], (double)B0[1], (double)C0[1]);
				const double MaxZ = FMath::Max3((double)A0[1], (double)B0[1], (double)C0[1]);
				const bool bPoolDetail = Receivers.TouchesPool(MinX, MinZ, MaxX, MaxZ);
				const double SpacingM = bPoolDetail ? 1.0 : 4.0;
				const int32 N = FMath::Clamp(FMath::CeilToInt(Longest / SpacingM), 1,
				                                 bPoolDetail ? 128 : 32);
				for (int32 iu = 0; iu < N; iu++)
				for (int32 iw = 0; iw < N - iu; iw++)
				{
					double P00[8], P10[8], P01[8], P11[8];
					Point(A0, B0, C0, (double)iu / N, (double)iw / N, P00);
					Point(A0, B0, C0, (double)(iu + 1) / N, (double)iw / N, P10);
					Point(A0, B0, C0, (double)iu / N, (double)(iw + 1) / N, P01);
					EmitTriangle(P00, P10, P01);
					if (iu + iw < N - 1)
					{
						Point(A0, B0, C0, (double)(iu + 1) / N, (double)(iw + 1) / N, P11);
						EmitTriangle(P10, P11, P01);
					}
				}
			}
			if (bElevated)
			{
				if (bRecordUsedReceiver) RS.Elevated++;
				else RS.BandOnlyRejected++;
			}
			if (RecordTris == 0) return;

			FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
			FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals);
			RS.Tris = RecordTris;
			RS.bBuilt = true;
		});
		GSecRoadDrape = FPlatformTime::Seconds() - DrapeStart;
		for (const FRoadRecordResult& RS : Results)
		{
			GRoadPainted += RS.Painted;
			GRoadColourless += RS.Colourless;
			GRoadElevatedCandidates += RS.ElevatedCandidates;
			GRoadPoolHoleTris += RS.PoolHoleTris;
			GRoadPoolShiftControl += RS.PoolShiftControl;
			GRoadReceiverVerts += RS.ReceiverVerts;
			GRoadElevated += RS.Elevated;
			GRoadBandOnlyRejected += RS.BandOnlyRejected;
			GRoadGroundSamples += RS.GroundSamples;
			GRoadGroundDeltaOverLift += RS.GroundDeltaOverLift;
			GRoadGroundDeltaSumCm += RS.GroundDeltaSumCm;
			GRoadGroundDeltaMaxCm = FMath::Max(GRoadGroundDeltaMaxCm, RS.GroundDeltaMaxCm);
		}

		// The objects and their materials, on the game thread. The BodySetup
		// is created here so the render build below can run on a worker.
		const double MaterialStart = FPlatformTime::Seconds();
		TArray<UStaticMesh*> RoadMeshes;
		RoadMeshes.SetNumZeroed(D.Num());
		for (int32 di = 0; di < D.Num(); di++)
		{
			if (!Results[di].bBuilt) continue;
			const BF6HP::FCore::FDecal& d = D[di];
			UStaticMesh* SM = NewObject<UStaticMesh>(
				A, *FString::Printf(TEXT("Road_%d"), di), RF_Transient);
			FStaticMaterial SMat;
			SMat.MaterialSlotName = TEXT("S0");
			SMat.ImportedMaterialSlotName = SMat.MaterialSlotName;
			SMat.MaterialInterface = RoadMaterialFor(SM, d);
			SM->GetStaticMaterials().Add(SMat);
			SM->CreateBodySetup();
			RoadMeshes[di] = SM;
		}
		GSecRoadMaterials = FPlatformTime::Seconds() - MaterialStart;

		// NO NANITE ON THESE. They are masked, they are thin, and they are a
		// few dozen triangles each - there is no cluster hierarchy worth
		// building and the build cost would be paid hundreds of times. They
		// take the runtime render path for the same reason the small props do:
		// the editor build path only ever gave them a DDC key and an async
		// task each, after the map had already appeared.
		const double CommitStart = FPlatformTime::Seconds();
		TArray<uint8> RoadOk;
		RoadOk.SetNumZeroed(D.Num());
		ParallelFor(D.Num(), [&](int32 di)
		{
			if (!RoadMeshes[di]) return;
			RoadOk[di] = PrepareRuntimeRenderMesh(RoadMeshes[di], Results[di].MD, Results[di].Tris) ? 1 : 0;
		});
		GSecRoadCommit = FPlatformTime::Seconds() - CommitStart;

		const double ComponentStart = FPlatformTime::Seconds();
		for (int32 di = 0; di < D.Num(); di++)
		{
			UStaticMesh* SM = RoadMeshes[di];
			if (!SM) continue;
			if (!RoadOk[di])
			{
				UE_LOG(LogBF6HighPoly, Error, TEXT("runtime render build failed for Road_%d"), di);
				continue;
			}
			const BF6HP::FCore::FDecal& d = D[di];

			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(
				A, *FString::Printf(TEXT("RoadMesh_%d"), di));
			C->SetupAttachment(Root);
			BF6HP::Shared::MakeUnselectable(C);
			C->RegisterComponent();
			C->SetStaticMesh(SM);
			C->SetMobility(EComponentMobility::Static);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);          // paint on the ground casts none
			// DrawOrderIndex is authored on the source instances, and the compiled
			// resource has already batched them into that sequence.  Translucent
			// components do NOT inherit creation order as a stable compositor order;
			// without an explicit priority, camera distance decides which of two
			// coplanar fills wins.  Later stream batches composite over earlier ones.
			const FString OpacityName = GCore.TextureNameAt(d.Opacity);
			// The stream keeps order within a pass, but treating every material
			// family as one pass lets broad late wear batches bury early runway
			// text and arrows. Restore the structural passes while preserving the
			// authored record order inside each: fill, detail, wear, markings.
			int32 SortBand = d.bPlanar ? 0 : 2000;
			if (IsRoadWear(OpacityName)) SortBand = 4000;
			if (IsRoadMarking(OpacityName))
			{
				SortBand = 6000;
				GRoadMarkingsPromoted++;
			}
			C->SetTranslucentSortPriority(SortBand + di);
			GRoadRecords++;
			GBuiltAnything = true;
			GRoadTris += Results[di].Tris;
		}
		GSecRoadComponents = FPlatformTime::Seconds() - ComponentStart;
		Results.Empty();
		(void)OutPending;   // road meshes are complete here; nothing to batch-build
		if (GRoadTintClamped > 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("decals: %d record(s) had an authored tint above 1.0 over a ")
				TEXT("colour sheet and were clamped - unclamped they saturate to ")
				TEXT("white, which is what the carrier decks were doing"),
				GRoadTintClamped);
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("decals: runtime material families promoted %d marking record(s); ")
			TEXT("softened %d wear record(s); blended %d maskless base-surface ")
			TEXT("record(s) and %d asphalt ")
			TEXT("surface ribbon(s); used the named second ")
			TEXT("colour endpoint on %d record(s)"),
			GRoadMarkingsPromoted, GRoadWearSoftened, GRoadBaseSurfaceBlended,
			GRoadSurfaceBlended,
			GRoadSecondColourUsed);
		if (GRoadPainted > 0 || GRoadColourless > 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("decals: %d record(s) painted from an authored colour ")
				TEXT("constant, %d still skipped (no colour anywhere - puddles ")
				TEXT("and modulators, which need a decal material)"),
				GRoadPainted, GRoadColourless);
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("decal projection: %d receiver vertex hit(s), %d pool-hole triangle(s) clipped; ")
			TEXT("+400m/+400m shifted-hole control %d. Adaptive/native terrain delta mean %.2f cm, ")
			TEXT("max %.2f cm; %lld/%lld samples exceeded the %.1f cm anti-z-fight lift"),
			GRoadReceiverVerts, GRoadPoolHoleTris, GRoadPoolShiftControl,
			GRoadGroundSamples ? GRoadGroundDeltaSumCm / GRoadGroundSamples : 0.0,
			GRoadGroundDeltaMaxCm, GRoadGroundDeltaOverLift, GRoadGroundSamples, GRoadLift);
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("decal projection: %d AABB-elevated candidate record(s), %d found exact ")
			TEXT("receiver geometry, %d band-only elevations rejected"),
			GRoadElevatedCandidates, GRoadElevated, GRoadBandOnlyRejected);
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("decal compositing: runtime pass bands fill/detail/wear/marking; ")
			TEXT("compiled record order preserved inside each band (0/2000/4000/6000 + index)"));
		return GRoadRecords;
	}


	// The map, built. One instanced component per distinct asset, which is both
	// the fast way to draw it and the honest shape of the data: a handful of
	// meshes placed over and over.
	// ---- game modes -------------------------------------------------------
	//
	// A LEVEL SHIPS EVERY GAME MODE'S PROPS AT ONCE, AND THEY ARE MUTUALLY
	// EXCLUSIVE.
	//
	// On MP_Isolated, 22,910 of 55,331 placements - 41.4% of the level - come
	// from bundles under `_layers_gameplay/`, and the big ones are alternative
	// layouts of the same ground: carrierstrike 15,033, portal_gameplay 3,005,
	// conquest 2,453, escalation 1,102, portal_aircraftcarriers_conquest 1,100.
	// The four largest share 85 assets and have ZERO placements in common at
	// 1 cm, so building them together does not add detail, it stacks layouts.
	// Counting carrier bridge parts over the level gives EIGHT carrier islands
	// at eight distinct positions where the game shows two.
	//
	// The bundle string names the mode, so no new decode is needed. Everything
	// outside `_layers_gameplay/` is shared art and is always built.
	//
	// Empty means "the biggest one", which is the map's headline mode and the
	// closest thing to what a player sees. "all" restores the old behaviour.
	FString GGameMode;

	// BUILD EVERY MODE, each tagged, so choosing one is hide/show and not a
	// rebuild. The cost is the other modes' instances in memory (41% more
	// placements on MP_Isolated); 0 restores the single-mode build.
	bool GGameModeBuildAll = true;


	// WHAT THE LAST BUILD FOUND, so the panel can offer it. The modes are a
	// property of the LEVEL, not of the tool, so there is nothing to show
	// until a level has been read once.
	TArray<TPair<FString, int32>> GModesFound;

	// MESHES THAT BIND NO TEXTURE AND WOULD DRAW WHITE.
	//
	// These are real placements, not decode failures: the game draws them with
	// a shader that needs no sheet, and our generic material falls back to a
	// white default. Untextured they are the most visible thing on the map -
	// bd_eas_oceanhorizon_01 is sixteen triangles placed at scale 75, which is
	// a 192 KILOMETRE plane, and it sits at y 68 m, below the playable water.
	// A white disc the size of the map is a far worse answer than no disc.
	//
	// The FX ones are smoke plumes, contrails and a cloud card: billboards
	// whose look lives entirely in a shader we do not run yet. They belong to
	// the FX path when it lands, not to the mesh path.
	int32 GMeshDecalSectionsBuilt = 0, GMeshDecalOnlyMeshesBuilt = 0;
	int32 GPuddleDecalAssetsBuilt = 0, GPuddleDecalPlacementsBuilt = 0;

	bool DrawsWhiteWithNoSheet(const FString& Mesh)
	{
		static const TCHAR* kNoSheet[] = {
			// bd_eas_oceanhorizon_01 IS NOT HERE ANY MORE. It binds no sheet
			// because the game draws it with the water shader, not because it is
			// undrawable, and it is how the game covers the map beyond the
			// simulated sea: the FFT ocean is only 5,946 m square, and this
			// 16-triangle plane at scale 75 carries water out to 192 km. Skipping
			// it is why our ocean ended in open space. It is built normally and
			// then given the water material below, so it never draws white.
			TEXT("ob_fx_bd_vertical_smokeplume_05"),
			TEXT("ob_fx_bd_horizontal_smokeplumes_01"),
			TEXT("fxm_contrails_strip"),
			TEXT("vfx_sky_cloudcard_01"),
		};
		for (const TCHAR* N : kNoSheet)
			if (Mesh.Contains(N, ESearchCase::IgnoreCase)) return true;
		return false;
	}

	// The segment after _layers_gameplay/, or empty when the bundle is not a
	// gameplay layer. "conquest/conquest" collapses to "conquest".
	FString GameModeOf(const FString& Bundle)
	{
		static const FString Marker(TEXT("_layers_gameplay/"));
		const int32 At = Bundle.Find(Marker, ESearchCase::IgnoreCase,
		                             ESearchDir::FromEnd);
		if (At == INDEX_NONE) return FString();
		FString Rest = Bundle.Mid(At + Marker.Len());
		int32 Slash = INDEX_NONE;
		if (Rest.FindChar(TEXT('/'), Slash)) Rest = Rest.Left(Slash);
		return Rest;
	}

	// Aftermath's normal and winter roots are complete scene alternatives, not
	// additive dressing. Keep this as a separate selection axis from GameModeOf:
	// a map can have both a gameplay layout and an event treatment, and folding
	// them into one string would fix the sedan by incorrectly dropping gameplay
	// content. Only the two roots measured on mp_aftermath are recognized; a
	// broad "*_event" guess would hide unrelated additive event bundles.
	FString EventRootOf(const FString& Bundle)
	{
		if (!Bundle.Contains(TEXT("mp_aftermath"), ESearchCase::IgnoreCase))
			return FString();
		TArray<FString> Parts;
		Bundle.ParseIntoArray(Parts, TEXT("/"), true);
		for (const FString& Part : Parts)
		{
			if (Part.Equals(TEXT("default_event"), ESearchCase::IgnoreCase))
				return TEXT("default_event");
			if (Part.Equals(TEXT("winter_event"), ESearchCase::IgnoreCase))
				return TEXT("winter_event");
		}
		return FString();
	}

	// Ground clutter reconstructed from the level's shipped catalogue and live
	// terrain paint. Species, meshes, view horizons and dissolve ratios are
	// exact. World positions are deliberately described as reconstruction: the
	// retail MeshScatteringDatabase contains no authored placement coordinates.
	int32 BuildScatter(AActor* A, USceneComponent* Root,
	                   const BF6HP::FCore::FTerrain& T,
	                   TArray<UStaticMesh*>& OutPending)
	{
		TArray<BF6HP::FCore::FScatter> Raw;
		if (!GCore.ReadScatter(BF6Ext::CurrentLevel(), Raw) || Raw.Num() == 0)
		{
			UE_LOG(LogBF6HighPoly, Log, TEXT("scatter: no shipped catalogue for this level"));
			return 0;
		}

		auto IsVegetation = [](const FString& S)
		{
			const FString N = S.ToLower();
			static const TCHAR* Words[] = {
				TEXT("grass"), TEXT("shrub"), TEXT("plant"), TEXT("fern"),
				TEXT("wheat"), TEXT("sapling"), TEXT("lantana"), TEXT("aralia"),
				TEXT("thorn"), TEXT("iceplant"), TEXT("palmleaves")
			};
			for (const TCHAR* W : Words) if (N.Contains(W)) return true;
			return false;
		};

		// De-duplicate repeated catalogue rows before paying for mesh creation.
		TArray<BF6HP::FCore::FScatter> Species;
		TSet<FString> Seen;
		for (const BF6HP::FCore::FScatter& S : Raw)
			if (IsVegetation(S.Name + TEXT("/") + S.MeshRes) && !Seen.Contains(S.MeshRes))
			{
				Seen.Add(S.MeshRes);
				Species.Add(S);
			}
		if (Species.Num() == 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("scatter: %d exact catalogue row(s), but none are vegetation"), Raw.Num());
			return 0;
		}

		const double CoverageStart = FPlatformTime::Seconds();
		BF6HP::FCore::FGroundCoverage C;
		FCoverageHold CoverageHold;
		bool bCoverageCached = false;
		if (!CachedGroundCoverage(BF6Ext::CurrentLevel(), 512, C, CoverageHold, bCoverageCached) ||
		    !C.Idx || !C.Weight)
		{
			UE_LOG(LogBF6HighPoly, Warning, TEXT("scatter coverage: %s"), *GCore.Error);
			return 0;
		}
		const double SecCoverage = FPlatformTime::Seconds() - CoverageStart;
		TArray<bool> Vegetated;
		Vegetated.Init(false, C.Materials.Num());
		for (int32 i = 0; i < C.Materials.Num(); i++)
		{
			const FString N = C.Materials[i].Albedo.ToLower();
			Vegetated[i] = IsVegetation(N);
		}

		TArray<BF6HP::FCore::FWater> Water;
		float OceanY = -FLT_MAX;
		if (GCore.ReadWater(BF6Ext::CurrentLevel(), Water))
			for (const BF6HP::FCore::FWater& S : Water)
				if (S.bOcean) OceanY = FMath::Max(OceanY, S.Height);

		const double YScale = T.HeightScale > 0.f
			? (double)T.HeightScale / 65536.0
			: FMath::Max(0.001, T.WorldMax.Y - T.WorldMin.Y) / 65535.0;
		const float SpanX = (float)(C.Hi.X - C.Lo.X);
		const float SpanZ = (float)(C.Hi.Y - C.Lo.Y);
		if (SpanX <= 1.f || SpanZ <= 1.f) return 0;

		auto Hash = [](uint32 V)
		{
			V ^= V >> 16; V *= 0x7feb352du; V ^= V >> 15;
			V *= 0x846ca68bu; V ^= V >> 16; return V;
		};
		auto Unit = [&Hash](uint32 V) { return (Hash(V) & 0x00ffffffu) / 16777216.f; };
		auto PaintAt = [&](int32 X, int32 Z)
		{
			X = (X % C.Size + C.Size) % C.Size;
			Z = (Z % C.Size + C.Size) % C.Size;
			const int64 O = ((int64)Z * C.Size + X) * 4;
			float Best = 0.f;
			for (int32 S = 0; S < 4; S++)
			{
				const int32 Id = C.Idx[O + S];
				if (C.Weight[O + S] == 0) break;
				if (Vegetated.IsValidIndex(Id) && Vegetated[Id])
					Best = FMath::Max(Best, C.Weight[O + S] / 255.f);
			}
			return Best;
		};

		TArray<TArray<FTransform>> Instances;
		Instances.SetNum(Species.Num());
		constexpr float SpacingM = 4.f;
		constexpr int32 HardCap = 60000;
		const int32 NX = FMath::Max(1, FMath::FloorToInt(SpanX / SpacingM));
		const int32 NZ = FMath::Max(1, FMath::FloorToInt(SpanZ / SpacingM));
		int32 ActualAccepted = 0, ShiftControlAccepted = 0;
		// THE CAP MUST NOT BE A SCAN ORDER. This loop used to stop at HardCap
		// accepted instances in row-major order, so on any large map only the
		// low-Z band was populated and the rest was bare - the log's own
		// control said so (half-map-shift control 265,059 against an actual of
		// 60,000: the control exceeding the actual is the cap talking). Every
		// candidate is now visited and kept, and the budget is applied AFTER
		// as a uniform thinning, which preserves the paint distribution
		// everywhere instead of truncating the map. Research: scattering.md 3.A.
		struct FScatterCandidate { int32 Which; FTransform Xf; uint32 Seed; };
		TArray<FScatterCandidate> Candidates;
		Candidates.Reserve(HardCap * 2);
		// ONE ROW PER WORKER. Every seed below is a function of (X, Z) alone and
		// every sample reads terrain and paint that nothing writes, so the rows
		// are scanned across cores into their own arrays and appended in row
		// order: the same candidates in the same order as the serial scan.
		const double ScanStart = FPlatformTime::Seconds();
		TArray<TArray<FScatterCandidate>> RowCandidates;
		RowCandidates.SetNum(NZ);
		TArray<int32> RowAccepted, RowShiftControl;
		RowAccepted.SetNumZeroed(NZ);
		RowShiftControl.SetNumZeroed(NZ);
		ParallelFor(NZ, [&](int32 Z)
		{
		TArray<FScatterCandidate>& Row = RowCandidates[Z];
		for (int32 X = 0; X < NX; X++)
		{
			const uint32 Seed = Hash((uint32)X * 0x9e3779b9u ^ (uint32)Z * 0x85ebca6bu);
			const float Jx = (Unit(Seed ^ 0x1234567u) - 0.5f) * SpacingM;
			const float Jz = (Unit(Seed ^ 0x7654321u) - 0.5f) * SpacingM;
			const float WX = (float)C.Lo.X + (X + 0.5f) * SpacingM + Jx;
			const float WZ = (float)C.Lo.Y + (Z + 0.5f) * SpacingM + Jz;
			const int32 CX = FMath::Clamp(FMath::FloorToInt((WX - C.Lo.X) / SpanX * C.Size), 0, C.Size - 1);
			const int32 CZ = FMath::Clamp(FMath::FloorToInt((WZ - C.Lo.Y) / SpanZ * C.Size), 0, C.Size - 1);
			const float Dice = Unit(Seed ^ 0xa511e9b3u);
			const float Paint = PaintAt(CX, CZ);
			const float ControlPaint = PaintAt(CX + C.Size / 2, CZ + C.Size / 2);
			if (Dice < ControlPaint * 0.34f) RowShiftControl[Z]++;
			if (Dice >= Paint * 0.34f) continue;

			const double GY = GroundAt(T, YScale, WX, WZ);
			if (OceanY > -FLT_MAX && GY <= OceanY + 0.25f) continue;
			const double Hx = GroundAt(T, YScale, WX + 1.5, WZ) -
			                  GroundAt(T, YScale, WX - 1.5, WZ);
			const double Hz = GroundAt(T, YScale, WX, WZ + 1.5) -
			                  GroundAt(T, YScale, WX, WZ - 1.5);
			if (FMath::Sqrt(Hx * Hx + Hz * Hz) / 3.0 > 0.55) continue;

			const int32 Which = (int32)(Hash(Seed ^ 0xd1b54a35u) % (uint32)Species.Num());
			const float Yaw = Unit(Seed ^ 0x94d049bbu) * 360.f;
			const float Scale = 0.8f + Unit(Seed ^ 0x369dea0fu) * 0.4f;
			Row.Add({ Which, FTransform(FRotator(0.f, Yaw, 0.f),
				FVector(WX * 100.f, WZ * 100.f, (GY + 0.02) * 100.f),
				FVector(Scale)), Seed });
			RowAccepted[Z]++;
		}
		});
		for (int32 Z = 0; Z < NZ; Z++)
		{
			ActualAccepted += RowAccepted[Z];
			ShiftControlAccepted += RowShiftControl[Z];
			Candidates.Append(MoveTemp(RowCandidates[Z]));
		}
		RowCandidates.Empty();
		const double SecScan = FPlatformTime::Seconds() - ScanStart;
		// Uniform thinning to the budget: each candidate survives with the same
		// probability wherever it stands, so density stays proportional to the
		// paint and the map edge is the map edge, not the cap.
		const int32 Wanted = Candidates.Num();
		const float Keep = Wanted > HardCap ? (float)HardCap / (float)Wanted : 1.f;
		int32 Kept = 0;
		for (const FScatterCandidate& Cand : Candidates)
		{
			if (Keep < 1.f && Unit(Cand.Seed ^ 0x5bd1e995u) >= Keep) continue;
			Instances[Cand.Which].Add(Cand.Xf);
			Kept++;
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("scatter: %d candidate(s) over the whole map, %d kept under a budget of %d (keep %.3f); coverage %.1fs%s, scan %.1fs parallel over %d row(s)"),
			Wanted, Kept, HardCap, Keep, SecCoverage,
			bCoverageCached ? TEXT(" (from cache)") : TEXT(""), SecScan, NZ);

		const double MeshStart = FPlatformTime::Seconds();
		int32 BuiltSpecies = 0, BuiltInstances = 0, Failed = 0, MeshesFromCache = 0;
		const FString ScatterLevel = BF6Ext::CurrentLevel();
		for (int32 I = 0; I < Species.Num(); I++)
		{
			if (Instances[I].Num() == 0) continue;
			TArray<BF6HP::FCore::FSection> Sections;
			bool bFromCache = false;
			if (!CachedReadMesh(ScatterLevel, Species[I].MeshRes, FString(), FString(), Sections, bFromCache))
			{ Failed++; continue; }
			if (bFromCache) MeshesFromCache++;
			FMeshDescription MD;
			int32 Tris = 0;
			if (!DescribeMesh(Sections, MD, Tris)) { Failed++; continue; }
			bool bNeedsBatchBuild = false;
			UStaticMesh* SM = MakeMesh(A,
				FString::Printf(TEXT("ScatterMesh_%d"), BuiltSpecies), MD, Tris, Sections,
				bNeedsBatchBuild);
			if (!SM) { Failed++; continue; }
			if (bNeedsBatchBuild) OutPending.Add(SM);
			UHierarchicalInstancedStaticMeshComponent* H =
				NewObject<UHierarchicalInstancedStaticMeshComponent>(A,
					*FString::Printf(TEXT("Scatter_%d"), BuiltSpecies));
			H->SetupAttachment(Root);
			BF6HP::Shared::MakeUnselectable(H);
			H->SetStaticMesh(SM);
			H->SetMobility(EComponentMobility::Static);
			H->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			H->SetCastShadow(false);
			H->SetCanEverAffectNavigation(false);
			const float EndM = FMath::Max(10.f, Species[I].ViewDistanceM);
			const float StartM = EndM * (1.f - FMath::Clamp(Species[I].DissolveRatio, 0.f, 1.f));
			H->SetCullDistances(FMath::RoundToInt(StartM * 100.f), FMath::RoundToInt(EndM * 100.f));
			// Register only after the generated mesh and no-collision policy are
			// installed. Registering the empty default component first makes UE try
			// to construct physics bodies once per instance and floods the log with
			// "unable to create InstanceBodies" even though collision is unwanted.
			H->RegisterComponent();
			H->AddInstances(Instances[I], false);
			BuiltSpecies++;
			BuiltInstances += Instances[I].Num();
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("scatter reconstruction: %d exact catalogue row(s), %d vegetation mesh(es), ")
			TEXT("%d/%d built, %d deterministic instance(s); terrain-paint accepts %d, ")
			TEXT("half-map-shift control %d. ")
			TEXT("Positions are reconstructed, not recovered. ")
			TEXT("Meshes %.1fs (%d from the derived cache, parent materials included)"),
			Raw.Num(), Species.Num(), BuiltSpecies, Species.Num(), BuiltInstances,
			ActualAccepted, ShiftControlAccepted,
			FPlatformTime::Seconds() - MeshStart, MeshesFromCache);
		if (BuiltInstances > 0) GBuiltAnything = true;
		return BuiltInstances;
	}

	// ---- RECEIVER FLAGS MEMO ----------------------------------------------
	//
	// Whether a decode input (resource, placing bundle, variation) yields a
	// section flagged terrain-decal-receiver is a bit the receiver pass needed
	// for EVERY group, which meant decoding the whole map once to find the 58
	// that qualify and once more to build them. The bit is a pure function of
	// the decode input, so it is memoised per level as one small blob. A
	// failed decode is not recorded: the objects pass asks the core again.
	constexpr uint32 kCacheVerReceiverFlags = 1;

	bool LoadReceiverFlags(const FString& Level, TMap<FString, uint8>& Out)
	{
		TArray<uint8> Blob;
		if (!BF6HP::DiskCache::LoadPacked(Level, TEXT("receiver_flags"), kCacheVerReceiverFlags, Blob))
			return false;
		FMemoryReader R(Blob);
		int32 Count = 0;
		R << Count;
		if (R.IsError() || Count < 0 || Count > 1000000) return false;
		Out.Reserve(Count);
		for (int32 i = 0; i < Count && !R.IsError(); i++)
		{
			FString Key;
			uint8 Flag = 0;
			R << Key << Flag;
			if (!Key.IsEmpty()) Out.Add(MoveTemp(Key), Flag);
		}
		if (R.IsError()) { Out.Empty(); return false; }
		return true;
	}

	void SaveReceiverFlags(const FString& Level, const TMap<FString, uint8>& Flags)
	{
		TArray<uint8> Blob;
		FMemoryWriter W(Blob);
		int32 Count = Flags.Num();
		W << Count;
		for (const TPair<FString, uint8>& kv : Flags)
		{
			FString Key = kv.Key;
			uint8 Flag = kv.Value;
			W << Key << Flag;
		}
		BF6HP::DiskCache::SaveAsync(Level, TEXT("receiver_flags"), kCacheVerReceiverFlags,
			MoveTemp(Blob), /*bPack*/ true);
	}

	// bOutCancelled says whether the scene that came back is the whole map or
	// as far as the build got. It is an OUT PARAMETER rather than file state
	// because the caller's summary line is the only thing a creator sees, and a
	// partial scene described as a finished one is the defect this closes.
	int32 BuildLevelGeometry(const TArray<BF6HP::FPlacement>& P, int32& OutMeshes, int32& OutFailed,
	                         FScopedSlowTask* Task, bool& bOutCancelled)
	{
		bOutCancelled = false;
		// STICKY, and asked between phases as well as between mesh batches.
		//
		// FScopedSlowTask::ShouldCancel latches the dialog's button, but the only
		// place that ever asked was the object loop - so pressing STOP during the
		// ground, the roads, the water or the lighting did nothing at all until
		// those phases finished, which on a big map is most of the build. Each
		// phase is a bounded unit of work; this is asked at every boundary
		// between them, and once it answers yes it keeps answering yes so the
		// remaining phases fall through without re-reading the dialog.
		auto Cancelled = [Task, &bOutCancelled]() -> bool
		{
			if (!bOutCancelled && Task && Task->ShouldCancel()) bOutCancelled = true;
			return bOutCancelled;
		};
		const double TotalStart = FPlatformTime::Seconds();
		double SecGroup = 0.0, SecReceivers = 0.0, SecTerrain = 0.0, SecRoads = 0.0;
		BF6HP::DiskCache::ResetStats();
		double SecWater = 0.0, SecScatter = 0.0, SecLighting = 0.0, SecObjects = 0.0;
		double PhaseStart = TotalStart;
		OutMeshes = OutFailed = 0;
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return 0;

		const int32 RemovedActors = BF6Ext::ClearAddonActors(kAddonName);
		if (RemovedActors > 0)
		{
			// DestroyActor only marks the previous build for collection. Each map
			// owns thousands of transient static meshes and texture-backed MIDs, so
			// starting another full build before a purge retained tens of GB across
			// the terrain diagnostics. Finish any users of those meshes, then make
			// their destruction real before allocating the replacement map.
			FAssetCompilingManager::Get().FinishAllCompilation();
			CollectGarbage(RF_NoFlags, true);
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("rebuild cleanup: purged %d actor tree(s) before allocating the new map"),
				RemovedActors);
		}
		GBuiltAnything = false;

		// Read local lights once, before object grouping.  Besides building the
		// emitters later, their source partition is the runtime on/off oracle for
		// fixture glow: static fixture materials intentionally carry t_red in the
		// verified glow slot and the live LightFixtures system supplies the state.
		TArray<BF6HP::FCore::FLight> LevelLights;
		FString LevelLightSummary, LevelLightError;
		bool bHaveLevelLights = false;
		if (GLayers[(int32)ELayer::Lighting].bOn)
		{
			bHaveLevelLights = GCore.ReadLights(
				BF6Ext::CurrentLevel(), LevelLights, LevelLightSummary);
			if (!bHaveLevelLights) LevelLightError = GCore.Error;
		}

		struct FGlowAccumulator
		{
			FLinearColor ColorSum = FLinearColor::Black;
			int32 Samples = 0;
		};
		auto RuntimeSourceKey = [](const FString& Path)
		{
			FString Key = FPaths::GetBaseFilename(Path).ToLower();
			if (Key.EndsWith(TEXT("_mesh"))) Key.LeftChopInline(5);
			return Key;
		};
		TMap<FString, FGlowAccumulator> RuntimeGlowSources;
		for (const BF6HP::FCore::FLight& L : LevelLights)
		{
			const float ActiveEnergy = L.Intensity * FMath::Max(L.Dimmer, 0.f);
			if (ActiveEnergy <= 0.f || L.AttenuationRadiusM <= 0.f || L.Source.IsEmpty())
				continue;
			FGlowAccumulator& Acc = RuntimeGlowSources.FindOrAdd(RuntimeSourceKey(L.Source));
			Acc.ColorSum += L.Color;
			Acc.Samples++;
		}

		// Group the placements by asset - AND by variation, where the
		// variation earns it. A livery or a paint is a variant depot record,
		// and two placements of one vehicle with different liveries cannot
		// share a material. But the split is paid for in components and mesh
		// builds, so it happens only where the core says the variation
		// actually derives a live record for this mesh; everything else
		// stays grouped exactly as before.
		struct FGroup
		{
			FString Mesh, Bundle, Variation;
			FString Mode;   // the gameplay layer this group's rows came from, or empty
			TArray<const BF6HP::FPlacement*> Rows;
			bool bRuntimeGlow = false;
			FLinearColor RuntimeGlowColor = FLinearColor::White;
		};
		// WHICH MODE ARE WE BUILDING. Counted first, because the default is
		// "the biggest", and because the list is worth printing either way -
		// a user who wants Conquest cannot ask for it if nothing ever said
		// Conquest was there.
		TMap<FString, int32> ModeCount;
		TMap<FString, int32> EventCount;
		GFarWorldRadiusCm = 0.f;
		for (const BF6HP::FPlacement& p : P)
		{
			const FString M = GameModeOf(p.Bundle);
			if (!M.IsEmpty()) ModeCount.FindOrAdd(M)++;
			const FString E = EventRootOf(p.Bundle);
			if (!E.IsEmpty()) EventCount.FindOrAdd(E)++;
			// Origin is game metres, and a radius does not care about the
			// basis swap.
			GFarWorldRadiusCm = FMath::Max(GFarWorldRadiusCm,
				(float)p.Origin.Size() * 100.f);
		}
		GModesFound.Reset();
		for (const TPair<FString, int32>& kv : ModeCount) GModesFound.Add(kv);
		GModesFound.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
			{ return A.Value > B.Value; });

		FString Chosen = GGameMode;
		if (Chosen.IsEmpty())
		{
			int32 Best = -1;
			for (const TPair<FString, int32>& kv : ModeCount)
				if (kv.Value > Best) { Best = kv.Value; Chosen = kv.Key; }
		}
		const bool bAllModes = Chosen.Equals(TEXT("all"), ESearchCase::IgnoreCase);
		FString ChosenEvent;
		if (EventCount.Contains(TEXT("default_event")))
			ChosenEvent = TEXT("default_event");
		else
		{
			int32 Best = -1;
			for (const TPair<FString, int32>& kv : EventCount)
				if (kv.Value > Best) { Best = kv.Value; ChosenEvent = kv.Key; }
		}

		// A placement needs TWO bundle identities, but the current core exposes
		// only the material-resolution bundle.  Recursive carrier prefabs make
		// the difference visible: at the NATO Carrier Strike island the authored
		// bridge root is (-661.5,1176.3), while its correctly transformed main
		// shell is around (-676.8,1184.6).  Entering the shared shell partition
		// changes its exposed bundle to Conquest even though the Carrier Strike
		// root placed it.  Filtering that string map-wide therefore keeps the
		// deck and removes its island.
		//
		// Until the public instance ABI carries a separate root-layer field,
		// recover ownership only for the affected carrier-island family from the
		// nearest authored hull-back anchor.  Keep p.Bundle unchanged: it is still
		// the correct scope for material lookup.
		struct FCarrierAnchor { FVector P; FString Mode; };
		TArray<FCarrierAnchor> CarrierAnchors;
		for (const BF6HP::FPlacement& p : P)
		{
			if (!p.Mesh.Contains(TEXT("mil_carriermainhullback_01"),
			                     ESearchCase::IgnoreCase)) continue;
			const FString M = GameModeOf(p.Bundle);
			if (!M.IsEmpty()) CarrierAnchors.Add({ p.Origin, M });
		}
		auto IsCarrierIslandPart = [](const FString& Mesh)
		{
			return Mesh.Contains(TEXT("carrierbridge_01"), ESearchCase::IgnoreCase) ||
			       Mesh.Contains(TEXT("carrierbridgeframe_01"), ESearchCase::IgnoreCase) ||
			       Mesh.Contains(TEXT("carriersign_01"), ESearchCase::IgnoreCase);
		};
		auto CarrierMode = [&](const BF6HP::FPlacement& p)
		{
			FString M = GameModeOf(p.Bundle);
			if (!IsCarrierIslandPart(p.Mesh)) return M;
			const FCarrierAnchor* Nearest = nullptr;
			double BestD2 = DBL_MAX;
			for (const FCarrierAnchor& A : CarrierAnchors)
			{
				const double DX = p.Origin.X - A.P.X;
				const double DZ = p.Origin.Z - A.P.Z;
				const double D2 = DX * DX + DZ * DZ;
				if (D2 < BestD2) { BestD2 = D2; Nearest = &A; }
			}
			// The island is under 70 m across.  120 m admits its authored children
			// while staying well below the 176+ m gap to the next carrier anchor.
			if (Nearest && BestD2 <= 120.0 * 120.0) return Nearest->Mode;
			return M;
		};
		int32 CarrierModeOverrides = 0, CarrierChosenRows = 0;
		int32 CarrierShiftControlRows = 0;
		for (const BF6HP::FPlacement& p : P)
		{
			if (!IsCarrierIslandPart(p.Mesh)) continue;
			const FString Raw = GameModeOf(p.Bundle);
			const FString Effective = CarrierMode(p);
			if (!Effective.Equals(Raw, ESearchCase::IgnoreCase)) CarrierModeOverrides++;
			if (Effective.Equals(Chosen, ESearchCase::IgnoreCase)) CarrierChosenRows++;
			for (const FCarrierAnchor& A : CarrierAnchors)
			{
				if (!A.Mode.Equals(Chosen, ESearchCase::IgnoreCase)) continue;
				const double DX = p.Origin.X - (A.P.X + 400.0);
				const double DZ = p.Origin.Z - (A.P.Z + 400.0);
				if (DX * DX + DZ * DZ <= 120.0 * 120.0)
				{ CarrierShiftControlRows++; break; }
			}
		}
		if (ModeCount.Num() > 0)
		{
			ModeCount.ValueSort([](int32 A, int32 B) { return A > B; });
			FString List;
			int32 Shown = 0, Total = 0;
			for (const TPair<FString, int32>& kv : ModeCount)
			{
				Total += kv.Value;
				if (Shown++ < 6)
					List += FString::Printf(TEXT("%s%s %d"),
						List.IsEmpty() ? TEXT("") : TEXT(", "), *kv.Key, kv.Value);
			}
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("game modes: %d found carrying %d placement(s) (%.1f%% of the ")
				TEXT("level) - %s%s"),
				ModeCount.Num(), Total, P.Num() ? 100.f * Total / P.Num() : 0.f,
				*List, ModeCount.Num() > 6 ? TEXT(", ...") : TEXT(""));
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("game modes: building %s. They are ALTERNATIVE layouts of the ")
				TEXT("same ground, not extra detail - switch with ")
				TEXT("BF6.HighPoly.GameMode <name|all>"),
				bAllModes ? TEXT("ALL of them, which stacks them on top of each other")
				          : *Chosen);
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("carrier ownership: %d hull-back anchor(s), %d island row(s) ")
				TEXT("assigned to %s, %d corrected from their material bundle; ")
				TEXT("+400m/+400m shifted-anchor control matched %d row(s)"),
				CarrierAnchors.Num(), CarrierChosenRows, *Chosen,
				CarrierModeOverrides, CarrierShiftControlRows);
		}

		TMap<FString, FGroup> ByMesh;
		TMap<FString, bool> VariationLiveMemo;
		int32 ModeSkipped = 0;
		int32 EventSkipped = 0, EventAdmitted = 0, EventFakeIdControl = 0;
		int32 WhiteSkipped = 0;
		int32 RuntimeGlowGroups = 0, RuntimeGlowFakeIdControl = 0;
		GMeshDecalSectionsBuilt = GMeshDecalOnlyMeshesBuilt = 0;
		GPuddleDecalAssetsBuilt = GPuddleDecalPlacementsBuilt = 0;
		ON_SCOPE_EXIT
		{
			if (ModeSkipped > 0)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("game modes: %d placement(s) skipped as belonging to a ")
					TEXT("mode other than the one being built"), ModeSkipped);
			}
			if (!ChosenEvent.IsEmpty())
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("event roots: selected %s, admitted %d conditioned placement(s), ")
					TEXT("skipped %d sibling placement(s); fake-id control matched %d"),
					*ChosenEvent, EventAdmitted, EventSkipped, EventFakeIdControl);
			}
			if (GMeshDecalSectionsBuilt > 0 || GMeshDecalOnlyMeshesBuilt > 0)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("mesh decals: %d category-2 section(s) built, including %d ")
					TEXT("decal-only mesh(es); these are authored surface-following geometry, ")
					TEXT("not environment projection volumes"),
					GMeshDecalSectionsBuilt, GMeshDecalOnlyMeshesBuilt);
			}
			if (GPuddleDecalPlacementsBuilt > 0)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("puddle mesh decals: %d colourless asset(s), %d placement(s) ")
					TEXT("routed to normal/roughness-only deferred decals"),
					GPuddleDecalAssetsBuilt, GPuddleDecalPlacementsBuilt);
			}
			if (WhiteSkipped > 0)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("far world: %d placement(s) skipped for binding no ")
					TEXT("texture - they are shader-drawn backdrop and FX and ")
					TEXT("would render as white geometry, one of them a 192 km plane"),
					WhiteSkipped);
			}
		};
		for (const BF6HP::FPlacement& p : P)
		{
			if (p.Mesh.IsEmpty()) continue;
			if (DrawsWhiteWithNoSheet(p.Mesh)) { WhiteSkipped++; continue; }
			// Shared content has no event condition and remains visible. Conditioned
			// Aftermath content admits exactly one complete root; drawing both places
			// black and red sedan skins at the same transform and z-fights between them.
			if (!ChosenEvent.IsEmpty())
			{
				const FString E = EventRootOf(p.Bundle);
				if (!E.IsEmpty())
				{
					if (E.Equals(ChosenEvent, ESearchCase::IgnoreCase)) EventAdmitted++;
					else { EventSkipped++; continue; }
				}
				if (E.Equals(ChosenEvent + TEXT("_control"), ESearchCase::IgnoreCase))
					EventFakeIdControl++;
			}
			// Shared art has no mode and is always built. A mode's art is built
			// under its own key and tagged, hidden unless it is the chosen mode,
			// so the switch is instant; the old skip is behind GGameModeBuildAll.
			const FString PMode = CarrierMode(p);
			if (!bAllModes && !GGameModeBuildAll
			    && !PMode.IsEmpty() && !PMode.Equals(Chosen, ESearchCase::IgnoreCase))
			{
				ModeSkipped++;
				continue;
			}
			FString Key = p.Mesh;
			if (!PMode.IsEmpty()) Key += TEXT("|mode:") + PMode;
			FString Var;
			if (!p.Variation.IsEmpty())
			{
				// Memoised per (mesh, bundle, variation): the answer is a
				// property of those three inputs and the same question was
				// being put to the core once per PLACEMENT, tens of thousands
				// of times for a few hundred distinct triples.
				const FString LiveKey = p.Mesh + TEXT("|") + p.Bundle + TEXT("|") + p.Variation;
				bool* bLive = VariationLiveMemo.Find(LiveKey);
				if (!bLive)
				{
					bLive = &VariationLiveMemo.Add(LiveKey,
						GCore.VariationLive(BF6HP::FCore::MeshResourceFor(p.Mesh), p.Bundle, p.Variation));
				}
				if (*bLive)
				{
					Var = p.Variation;
					Key += TEXT("|") + p.Variation + TEXT("|") + p.Bundle;
				}
			}
			FGroup& G = ByMesh.FindOrAdd(Key);
			if (G.Rows.Num() == 0)
			{
				G.Mesh = p.Mesh; G.Bundle = p.Bundle; G.Variation = Var; G.Mode = PMode;
				const FString SourceKey = RuntimeSourceKey(p.Mesh);
				if (const FGlowAccumulator* Acc = RuntimeGlowSources.Find(SourceKey))
				{
					if (Acc->Samples > 0)
					{
						G.bRuntimeGlow = true;
						G.RuntimeGlowColor = Acc->ColorSum / (float)Acc->Samples;
						RuntimeGlowGroups++;
					}
				}
				// Fake-id negative control: a name-only heuristic would often match a
				// family prefix; the exact source join must not match a mutated id.
				if (RuntimeGlowSources.Contains(SourceKey + TEXT("_control")))
					RuntimeGlowFakeIdControl++;
			}
			G.Rows.Add(&p);
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("fixture runtime glow: %d active light source partition(s), %d exact mesh group match(es); fake-id control %d"),
			RuntimeGlowSources.Num(), RuntimeGlowGroups, RuntimeGlowFakeIdControl);

		TArray<FString> Order;
		ByMesh.GetKeys(Order);
		Order.Sort([&ByMesh](const FString& a, const FString& b)
			{ return ByMesh[a].Rows.Num() > ByMesh[b].Rows.Num(); });
		// The pills only turn on what is needed, and OFF means not built, not
		// merely hidden: a layer nobody asked for costs nothing.
		if (!GLayers[(int32)ELayer::Objects].bOn) Order.Empty();
		SecGroup = FPlatformTime::Seconds() - PhaseStart;

		// WHERE THE TIME GOES, split so the levers are separable. Decoding, turning
		// the decode into a mesh description, and Unreal's own build want
		// completely different fixes and a total says nothing about which.
		double SecDecode = 0, SecDescribe = 0, SecBuild = 0;
		double SecMeshCacheLoad = 0;
		double SecCreate = 0, SecCommitRuntime = 0, SecCommitNanite = 0, SecComponents = 0;
		int32 MeshesFromCache = 0;
		std::atomic<int64> MeshCacheBytesWritten{ 0 };
		GNaniteCountThisBuild = GNoNaniteTranslucent = GRuntimeMeshCountThisBuild = 0;
		GPreparedTrisThisBuild = 0;
		GNaniteTrisThisBuild = 0;
		GTexUploaded = GTexHighQuality = GTexRefused = 0;
		GMidsMade = GBindingsSeen = GBindingsBound = 0;
		GMaterialCacheHits = GMaterialCacheMisses = 0;
		GSecObjectMaterials = GSecObjectPrepare = 0.0;
		GSecTexDecode = GSecTexPrefetch = GSecTexUpload = GSecParentMaterials = 0.0;
		GTexFromCache = 0;
		GTexCacheBytesWritten = 0;
		GTexPrefetched.Empty();
		TArray<UStaticMesh*> Pending;

		AActor* A = W->SpawnActor<AActor>();
		if (!A) return 0;
		A->SetActorLabel(TEXT("HighPoly"));
		BF6Ext::MarkAddonActor(A, kAddonName);
		USceneComponent* Root = NewObject<USceneComponent>(A, TEXT("Root"));
		A->SetRootComponent(Root);
		// This reconstruction actor never moves.  Its generated terrain, roads and
		// object HISMs are static, so the root must be static before they register;
		// otherwise UE rejects every static attachment to the default movable root.
		// Movable children such as the FFT water remain valid under a static parent.
		Root->SetMobility(EComponentMobility::Static);
		Root->RegisterComponent();

		const bool bWantTerrain = GLayers[(int32)ELayer::Terrain].bOn;
		const bool bWantRoads   = GLayers[(int32)ELayer::Roads].bOn;
		const bool bWantScatter = GLayers[(int32)ELayer::Scatter].bOn;

		// Receiver identity is a material fact which the core now carries across
		// the ABI. Build its small spatial index before the road pass; otherwise
		// pools and slabs have already been flattened onto the heightfield by the
		// time their prop geometry is decoded below. Only receiver-bearing meshes
		// are retained, so this does not hold the entire map's decoded geometry.
		FDecalReceiverIndex ReceiverIndex;
		TMap<FString, TArray<BF6HP::FCore::FSection>> ReceiverDecoded;
		PhaseStart = FPlatformTime::Seconds();
		if (bWantRoads && Order.Num() > 0 && !Cancelled())
		{
			const double ReceiverStart = FPlatformTime::Seconds();
			int32 ReceiverAssets = 0;
			const FString ReceiverLevel = BF6Ext::CurrentLevel();
			TMap<FString, uint8> ReceiverFlags;
			LoadReceiverFlags(ReceiverLevel, ReceiverFlags);
			int32 FlagHits = 0, FlagMisses = 0, ReceiverMeshesFromCache = 0;
			for (const FString& Key : Order)
			{
				// One decode per asset, so this is the loop's natural work unit.
				// Without it STOP was ignored for the whole receiver pass, which
				// on a road-heavy map is thousands of meshes.
				if (Cancelled()) break;
				const FGroup& G = ByMesh[Key];
				const FString Res = BF6HP::FCore::MeshResourceFor(G.Mesh);
				const FString FlagKey = Res + TEXT("|") + G.Bundle + TEXT("|") + G.Variation;
				if (const uint8* Known = ReceiverFlags.Find(FlagKey))
				{
					FlagHits++;
					if (!*Known) continue;
				}
				else
				{
					FlagMisses++;
				}
				TArray<BF6HP::FCore::FSection> Sections;
				bool bFromCache = false;
				if (!CachedReadMesh(ReceiverLevel, Res, G.Bundle, G.Variation, Sections, bFromCache))
					continue;
				if (bFromCache) ReceiverMeshesFromCache++;
				bool bAny = false;
				for (const BF6HP::FCore::FSection& S : Sections)
					if (S.bTerrainDecalReceiver) { bAny = true; break; }
				ReceiverFlags.Add(FlagKey, bAny ? 1 : 0);
				if (!bAny) continue;
				ReceiverAssets++;
				const bool bPool = G.Mesh.Contains(TEXT("/pool_"), ESearchCase::IgnoreCase) ||
				                   G.Mesh.Contains(TEXT("\\pool_"), ESearchCase::IgnoreCase);
				for (const BF6HP::FPlacement* Placement : G.Rows)
					for (const BF6HP::FCore::FSection& S : Sections)
						if (S.bTerrainDecalReceiver) ReceiverIndex.Add(S, *Placement, bPool);
				ReceiverDecoded.Add(Key, MoveTemp(Sections));
			}
			if (FlagMisses > 0) SaveReceiverFlags(ReceiverLevel, ReceiverFlags);
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("decal receivers: %d asset(s), %d placed section(s), %d horizontal triangle(s), ")
				TEXT("%d pool patch(es), indexed in %.2fs; flags: %d from the memo, %d decoded live (%d of those meshes from the derived cache)"),
				ReceiverAssets, ReceiverIndex.Sections, ReceiverIndex.Triangles,
				ReceiverIndex.PoolPatches, FPlatformTime::Seconds() - ReceiverStart,
				FlagHits, FlagMisses, ReceiverMeshesFromCache);
		}
		SecReceivers = FPlatformTime::Seconds() - PhaseStart;

		// The ground first, so a long prop build still leaves something to
		// stand on if it is interrupted.
		// THE GROUND IS READ ONCE AND KEPT, because the roads need it too: a
		// decal carries no height of its own and is draped on whatever ground
		// we built, so the two cannot be read independently without the second
		// one sampling a different surface from the one it sits on.
		BF6HP::FCore::FTerrain Ground;
		bool bHaveGround = false;
		PhaseStart = FPlatformTime::Seconds();
		if ((bWantTerrain || bWantRoads || bWantScatter) && !Cancelled())
		{
			if (Task) Task->EnterProgressFrame(3.f, LOCTEXT("Ground", "Building the ground"));
			bool bGroundCached = false;
			bHaveGround = CachedReadTerrain(BF6Ext::CurrentLevel(), Ground, bGroundCached);
			if (bGroundCached)
				UE_LOG(LogBF6HighPoly, Log, TEXT("terrain: heightfield %dx%d from cache"),
					Ground.Size, Ground.Size);
			if (bHaveGround && bWantTerrain)
			{
				const int32 side = BuildTerrain(A, Root, Ground, Pending);
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("terrain: %d native -> %d base, %.0f m across, %.2f m base / %.2f m finest, %d tile(s)"),
					Ground.Size, side, Ground.WorldMax.X - Ground.WorldMin.X,
					GTerrainBaseSpacingM, GTerrainMinSpacingM, GTerrainTilesBuilt);
			}
			else if (!bHaveGround)
			{
				UE_LOG(LogBF6HighPoly, Warning, TEXT("terrain: %s"), *GCore.Error);
			}
		}
		SecTerrain = FPlatformTime::Seconds() - PhaseStart;

		// ROADS, on top of the ground and only if there is ground to drape on.
		PhaseStart = FPlatformTime::Seconds();
		if (bHaveGround && GLayers[(int32)ELayer::Roads].bOn && !Cancelled())
		{
			if (Task) Task->EnterProgressFrame(2.f, LOCTEXT("Roads", "Draping the roads"));
			TArray<BF6HP::FCore::FDecal> Decals;
			const double DecalReadStart = FPlatformTime::Seconds();
			const bool bHaveDecals = GCore.ReadDecals(BF6Ext::CurrentLevel(), Decals);
			const double SecDecalRead = FPlatformTime::Seconds() - DecalReadStart;
			if (bHaveDecals)
			{
				const int32 built = BuildRoads(A, Root, Ground, Decals, ReceiverIndex, Pending);
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("roads: %d record(s) of %d, %d triangle(s), %d projected onto exact elevated receivers; ")
					TEXT("read %.1fs, drape %.1fs parallel, materials %.1fs, render build %.1fs parallel, components %.1fs"),
					built, Decals.Num(), GRoadTris, GRoadElevated,
					SecDecalRead, GSecRoadDrape, GSecRoadMaterials, GSecRoadCommit, GSecRoadComponents);
			}
			else
			{
				// Not every level ships decals, and an older core has no entry
				// point for them. Neither is a failure of the build.
				UE_LOG(LogBF6HighPoly, Log, TEXT("roads: none for this level"));
			}
		}
		SecRoads = FPlatformTime::Seconds() - PhaseStart;

		// WATER. Flat planes at the level's own heights and colours, through
		// Unreal's Single Layer Water - reflections and depth for free.
		PhaseStart = FPlatformTime::Seconds();
		if (GLayers[(int32)ELayer::Water].bOn && !Cancelled())
		{
			if (Task) Task->EnterProgressFrame(1.f, LOCTEXT("Water", "Laying the water"));
			ApplyStableWaterReflectionPolicy();
			TArray<BF6HP::FCore::FWater> Water;
			if (GCore.ReadWater(BF6Ext::CurrentLevel(), Water))
			{
				// The large Tsuru surface is streaming-tree block 2, not the
				// ocean FFT. Read it directly from the mounted game before any
				// material instance is created.
				BF6HP::FCore::FTerrain WaterSurfaceHeight;
				if (GCore.ReadWaterHeightfield(BF6Ext::CurrentLevel(), WaterSurfaceHeight))
					BF6WaterShared::SetWaterHeightfield(&WaterSurfaceHeight);
				else
				{
					BF6WaterShared::SetWaterHeightfield(nullptr);
					UE_LOG(LogBF6HighPoly, Log,
						TEXT("water: no separate raw block-2 surface heightfield for %s (%s); ")
						TEXT("using the entity plane plus any decoded FFT"),
						*BF6Ext::CurrentLevel(), *GCore.Error);
				}
				// THE SHORE FADE ONLY NEEDS THE HEIGHTS, NOT THE MESH.
				//
				// Reading the heightfield is a decode; BUILDING terrain is
				// the expensive part. Tying the fade to the Terrain LAYER
				// meant a creator who wanted water alone got a sea that ran
				// into the beach at full height, which is not a choice
				// anybody would make on purpose. So if the ground was not
				// read for its own sake, read it here just for the depths.
				BF6HP::FCore::FTerrain WaterGround;
				const BF6HP::FCore::FTerrain* Depths = nullptr;
				if (bHaveGround) Depths = &Ground;
				else if (GCore.ReadTerrain(BF6Ext::CurrentLevel(), WaterGround))
					Depths = &WaterGround;
				GWaterCoeffs.Reset();
				BuildWater(A, Root, Water, Pending, Depths);
				// The receiver-topology pool reconstruction is backed only by
				// MP_Isolated. Running that heuristic elsewhere invents water.
				const int32 LocalPools = BF6Ext::CurrentLevel().Equals(
					TEXT("MP_Isolated"), ESearchCase::IgnoreCase)
					? BuildPoolWater(A, Root, ReceiverIndex, Water, Pending) : 0;
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("water: %d surface(s), including %d reconstructed raised pool(s)"),
					GWaterBuilt, LocalPools);
			}
			else
			{
				// Most levels author no water entity; some carry theirs in the
				// backdrop instead. Neither is a failure of the build.
				UE_LOG(LogBF6HighPoly, Log, TEXT("water: none for this level"));
			}
		}
		SecWater = FPlatformTime::Seconds() - PhaseStart;

		PhaseStart = FPlatformTime::Seconds();
		if (bHaveGround && bWantScatter && !Cancelled())
		{
			if (Task) Task->EnterProgressFrame(3.f,
				LOCTEXT("Scatter", "Reconstructing ground clutter"));
			BuildScatter(A, Root, Ground, Pending);
		}
		SecScatter = FPlatformTime::Seconds() - PhaseStart;

		PhaseStart = FPlatformTime::Seconds();
		if (GLayers[(int32)ELayer::Lighting].bOn && !Cancelled())
		{
			if (Task) Task->EnterProgressFrame(1.f, LOCTEXT("Lighting", "Lighting the map"));
			// The environment first: a sun with nothing to fill behind it is
			// still better than the hard-coded one, and the sky light wants to
			// exist before the local lights are judged against it.
			BF6HP::FCore::FVELighting Env;
			if (GCore.ReadLighting(BF6Ext::CurrentLevel(), Env))
			{
				ApplyEnvironment(A, Root, Env);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Warning, TEXT("lighting: %s"), *GCore.Error);
			}

			if (bHaveLevelLights)
			{
				const double T0 = FPlatformTime::Seconds();
				GLightsBuilt = BuildLights(A, Root, LevelLights);
				UE_LOG(LogBF6HighPoly, Log, TEXT("lights: %s, %d placed, %.1fs"),
					*LevelLightSummary, GLightsBuilt, FPlatformTime::Seconds() - T0);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Warning, TEXT("lights: %s"), *LevelLightError);
			}
			if (GLightsBuilt > 0) GBuiltAnything = true;
		}
		SecLighting = FPlatformTime::Seconds() - PhaseStart;

		// DECODE SERIALLY, DESCRIBE IN PARALLEL, CREATE ON THE GAME THREAD.
		//
		// Each phase runs where it is allowed to. The decode goes through one
		// shared context in the core, whose caches are not thread-safe, so it
		// stays serial - and at about a millisecond a mesh it does not matter.
		// Describing shares nothing and takes the bulk of the time, so it goes
		// wide. Creating UObjects has to be the game thread.
		//
		// IN BATCHES, because the alternative is holding every decoded mesh and
		// every description for the whole map at once: on a big map that is a
		// gigabyte of geometry alive simultaneously for no reason.
		constexpr int32 kBatch = 256;

		PhaseStart = FPlatformTime::Seconds();
		int32 Placed = 0, Built = 0;
		int32 SpatialHismComponents = 0, SpatialHismSplits = 0;
		double MaxInstanceLocalCm = 0.0;
		// Resolved once, on the game thread: the workers below key blobs by it.
		const FString CacheLevel = BF6Ext::CurrentLevel();
		for (int32 First = 0; First < Order.Num(); First += kBatch)
		{
			if (Cancelled())
			{
				// Stop where we are and keep what is built. A half-built map is
				// more useful than none, and CLEAR removes it in one go - but the
				// caller now has to SAY it is half-built, which is what the
				// latched flag carries out of here.
				UE_LOG(LogBF6HighPoly, Log, TEXT("cancelled after %d mesh(es)"), Built);
				break;
			}
			if (Task)
			{
				const int32 BatchItems = FMath::Min(kBatch, Order.Num() - First);
				Task->EnterProgressFrame(58.f * (float)BatchItems / (float)FMath::Max(1, Order.Num()),
					FText::FromString(FString::Printf(TEXT("Building geometry  %d / %d"),
						First, Order.Num())));
			}
			const int32 Last = FMath::Min(First + kBatch, Order.Num());
			const int32 N = Last - First;

			TArray<TArray<BF6HP::FCore::FSection>> Decoded;
			TArray<FString> Names;
			// Per decoded mesh: the binding names when this decode came from
			// the core and is to be written, empty otherwise.
			TArray<TArray<TArray<FString>>> DecodedBindingNames;
			TArray<uint8> NeedsWrite;
			Decoded.Reserve(N);
			Names.Reserve(N);
			DecodedBindingNames.Reserve(N);
			NeedsWrite.Reserve(N);

			double T = FPlatformTime::Seconds();
			// THE CACHE FIRST, ACROSS CORES. Every group's blob is read and
			// deserialised in parallel; only the misses go through the serial
			// core below, and the receivers were decoded earlier.
			TArray<FMeshBlob> Blobs;
			Blobs.SetNum(N);
			TArray<uint8> BlobOk;
			BlobOk.SetNumZeroed(N);
			ParallelFor(N, [&](int32 k)
			{
				const FString& Key = Order[First + k];
				if (ReceiverDecoded.Contains(Key)) return;
				const FGroup& G = ByMesh[Key];
				BlobOk[k] = LoadMeshBlob(CacheLevel, BF6HP::FCore::MeshResourceFor(G.Mesh),
					G.Bundle, G.Variation, Blobs[k]) ? 1 : 0;
			});
			SecMeshCacheLoad += FPlatformTime::Seconds() - T;
			for (int32 i = First; i < Last; i++)
			{
				const int32 k = i - First;
				TArray<BF6HP::FCore::FSection> Sections;
				TArray<TArray<FString>> BindingNames;
				bool bWrite = false;
				// The group's own scope: its placing bundle AND its variation.
				// The variation is what makes a livery a livery - the variant
				// record carries the delta and the core merges it over the
				// base. Ungated groups carry an empty variation and read
				// exactly as before.
				const FGroup& G = ByMesh[Order[i]];
				if (const TArray<BF6HP::FCore::FSection>* Cached = ReceiverDecoded.Find(Order[i]))
				{
					Sections = *Cached;
				}
				else if (BlobOk[k] && ResolveMeshBlobBindings(Blobs[k]))
				{
					Sections = MoveTemp(Blobs[k].Sections);
					MeshesFromCache++;
				}
				else if (!GCore.ReadMesh(BF6HP::FCore::MeshResourceFor(G.Mesh), Sections,
				                         G.Bundle, G.Variation))
				{
					OutFailed++;
					continue;
				}
				else
				{
					bWrite = NameMeshBindings(Sections, BindingNames);
				}
				Blobs[k] = FMeshBlob();
				// A MESH DECAL IS AUTHORED SURFACE-FOLLOWING GEOMETRY.
				//
				// MeshSubsetCategory_TransparentDecal is an exact per-section
				// classification. The decal-system audit separates these from
				// environment decal VOLUMES: mesh decals do not project in their
				// placement data; their triangles already sit on the hull, deck or
				// puddled surface. The earlier omission confused the two systems,
				// deleting every decal-only prop and stripping category-2 sections
				// from mixed carrier hull and bridge meshes. The live core now maps
				// the authored _ca and _nms slots, so these sections can use the
				// dedicated translucent mesh-decal material without a white fallback.
				{
					int32 DecalCount = 0;
					bool bOnlyPuddle = Sections.Num() > 0;
					for (const BF6HP::FCore::FSection& S : Sections)
					{
						if (S.bDecal) DecalCount++;
						bool bAlbedo = false, bNormal = false;
						for (const BF6HP::FCore::FBinding& B : S.Textures)
						{
							bAlbedo |= B.Slot == 0 && B.Texture >= 0;
							bNormal |= B.Slot == 1 && B.Texture >= 0;
						}
						bOnlyPuddle &= S.bDecal && !bAlbedo && bNormal;
					}
					GMeshDecalSectionsBuilt += DecalCount;
					if (DecalCount > 0 && DecalCount == Sections.Num())
						GMeshDecalOnlyMeshesBuilt++;

					// A colourless mesh decal cannot be represented by a translucent
					// surface: that would need an invented base colour and is exactly
					// what made PuddleLong draw as a white card. Convert its authored
					// flat bounds and placement basis to a UE decal volume instead.
					if (bOnlyPuddle)
					{
						UMaterialInstanceDynamic* PuddleMat = MaterialFor(A, Sections[0]);
						FVector3f Lo(FLT_MAX), Hi(-FLT_MAX);
						for (const BF6HP::FCore::FSection& S : Sections)
							for (const FVector3f& V : S.Pos)
							{
								Lo.X = FMath::Min(Lo.X, V.X); Lo.Y = FMath::Min(Lo.Y, V.Y); Lo.Z = FMath::Min(Lo.Z, V.Z);
								Hi.X = FMath::Max(Hi.X, V.X); Hi.Y = FMath::Max(Hi.Y, V.Y); Hi.Z = FMath::Max(Hi.Z, V.Z);
							}
						const FVector3f Centre = (Lo + Hi) * 0.5f;
						const FVector3f Extent = (Hi - Lo) * 0.5f;
						if (PuddleMat)
						for (const BF6HP::FPlacement* p : G.Rows)
						{
							const FVector WorldGame = p->Origin + p->Right * Centre.X
								+ p->Up * Centre.Y + p->Forward * Centre.Z;
							const FVector R(p->Right.X, p->Right.Z, p->Right.Y);
							const FVector U(p->Up.X, p->Up.Z, p->Up.Y);
							const FVector F(p->Forward.X, p->Forward.Z, p->Forward.Y);
							UDecalComponent* C = NewObject<UDecalComponent>(A);
							// No MakeUnselectable here: a decal is a scene
							// component, not a primitive, so it has no hit
							// proxy to turn off and was never selectable.
							C->SetupAttachment(Root);
							C->DecalSize = FVector(25.f,
								FMath::Max(1.f, Extent.X * 100.f),
								FMath::Max(1.f, Extent.Z * 100.f));
							C->SetDecalMaterial(PuddleMat);
							C->SetWorldTransform(FTransform(FMatrix(-U, R, F, ToUnreal(WorldGame))));
							C->RegisterComponent();
							GPuddleDecalPlacementsBuilt++;
						}
						GPuddleDecalAssetsBuilt++;
						Placed += G.Rows.Num();
						continue;
					}
				}
				Decoded.Add(MoveTemp(Sections));
				Names.Add(Order[i]);
				DecodedBindingNames.Add(MoveTemp(BindingNames));
				NeedsWrite.Add(bWrite ? 1 : 0);
			}
			Blobs.Empty();
			SecDecode += FPlatformTime::Seconds() - T;

			// THE SHEETS THIS BATCH BINDS, from the derived cache across cores,
			// before the game-thread material pass asks for them one by one.
			{
				TArray<uint64> Keys;
				TSet<uint64> Seen;
				for (const TArray<BF6HP::FCore::FSection>& Secs : Decoded)
					for (const BF6HP::FCore::FSection& S : Secs)
						for (const BF6HP::FCore::FBinding& B : S.Textures)
						{
							if (B.Texture < 0) continue;
							const uint64 Key = ((uint64)(uint32)B.Texture << 32) | (uint32)GPreviewTextureMax;
							if (GTextureCache.Contains(Key) || Seen.Contains(Key)) continue;
							Seen.Add(Key);
							Keys.Add(Key);
						}
				PrefetchTexturePayloads(Keys);
			}

			T = FPlatformTime::Seconds();
			TArray<FMeshDescription> Descs;
			Descs.SetNum(Decoded.Num());
			TArray<int32> Tris;
			Tris.SetNumZeroed(Decoded.Num());
			TArray<bool> Ok;
			Ok.Init(false, Decoded.Num());
			ParallelFor(Decoded.Num(), [&](int32 i)
			{
				Ok[i] = DescribeMesh(Decoded[i], Descs[i], Tris[i]);
				// A live decode is written for next time from here: the
				// serialisation is pure and the write is queued on a worker.
				if (NeedsWrite[i])
				{
					TArray<uint8> Blob;
					if (SerialiseMeshForCache(Decoded[i], DecodedBindingNames[i], Blob))
					{
						const FGroup& G = ByMesh[Names[i]];
						MeshCacheBytesWritten += (int64)Blob.Num();
						BF6HP::DiskCache::SaveAsync(CacheLevel,
							MeshCacheName(BF6HP::FCore::MeshResourceFor(G.Mesh), G.Bundle, G.Variation),
							kCacheVerMesh, MoveTemp(Blob), /*bPack*/ true);
					}
				}
			});
			SecDescribe += FPlatformTime::Seconds() - T;

			// CREATE ON THE GAME THREAD, COMMIT ACROSS CORES, PLACE ON THE GAME
			// THREAD. The runtime-render meshes - 2,740 of MP_Isolated's 2,941 -
			// used to build their render data one after another inside MakeMesh,
			// and that was 7 s of the objects phase. The terrain tiles' three-
			// phase split applies unchanged: NewObject, materials and BodySetup
			// here; BuildFromMeshDescriptions in a ParallelFor; components after.
			T = FPlatformTime::Seconds();
			TArray<UStaticMesh*> BatchMeshes;
			BatchMeshes.SetNumZeroed(Decoded.Num());
			TArray<uint8> BatchNanite, BatchCommitted;
			BatchNanite.SetNumZeroed(Decoded.Num());
			BatchCommitted.SetNumZeroed(Decoded.Num());
			TArray<int32> BatchOrdinal;
			BatchOrdinal.Init(-1, Decoded.Num());
			for (int32 i = 0; i < Decoded.Num(); i++)
			{
				if (!Ok[i]) { OutFailed++; continue; }
				const FGroup& BuiltGroup = ByMesh[Names[i]];
				const FLinearColor* RuntimeGlowColor = BuiltGroup.bRuntimeGlow
					? &BuiltGroup.RuntimeGlowColor : nullptr;
				bool bNanite = false;
				// The glass record for each translucent section, read in the SAME
				// scope the mesh was read in. Nothing is read when the dll has no
				// glass export, and an opaque section is never asked.
				TArray<FGlassDesc> GlassDescs;
				if (GlassReadAvailable())
				{
					GlassDescs.SetNum(Decoded[i].Num());
					for (int32 gi = 0; gi < Decoded[i].Num(); gi++)
						if (Decoded[i][gi].bTranslucent)
							ReadGlassSection(BF6HP::FCore::MeshResourceFor(BuiltGroup.Mesh), 0,
								BuiltGroup.Bundle, BuiltGroup.Variation, gi, GlassDescs[gi]);
				}
				BatchMeshes[i] = CreateMeshObject(A, FString::Printf(TEXT("M_%d"), Built),
					Tris[i], Decoded[i], bNanite,
					RuntimeGlowColor, BuiltGroup.bRuntimeGlow ? 1.f : 0.f,
					GlassDescs.Num() ? &GlassDescs : nullptr);
				if (!BatchMeshes[i]) { OutFailed++; continue; }
				BatchNanite[i] = bNanite ? 1 : 0;
				BatchOrdinal[i] = Built++;
			}
			SecCreate += FPlatformTime::Seconds() - T;

			T = FPlatformTime::Seconds();
			ParallelFor(Decoded.Num(), [&](int32 i)
			{
				if (!BatchMeshes[i] || BatchNanite[i]) return;
				BatchCommitted[i] = PrepareRuntimeRenderMesh(BatchMeshes[i], Descs[i], Tris[i]) ? 1 : 0;
			});
			const double BatchRuntimeS = FPlatformTime::Seconds() - T;
			SecCommitRuntime += BatchRuntimeS;

			T = FPlatformTime::Seconds();
			for (int32 i = 0; i < Decoded.Num(); i++)
			{
				if (!BatchMeshes[i] || !BatchNanite[i]) continue;
				PrepareMesh(BatchMeshes[i], Descs[i], Tris[i], true);
				BatchCommitted[i] = 1;
				Pending.Add(BatchMeshes[i]);
			}
			const double BatchNaniteS = FPlatformTime::Seconds() - T;
			SecCommitNanite += BatchNaniteS;
			GSecObjectPrepare += BatchRuntimeS + BatchNaniteS;

			T = FPlatformTime::Seconds();
			for (int32 i = 0; i < Decoded.Num(); i++)
			{
				UStaticMesh* SM = BatchMeshes[i];
				if (!SM) continue;
				if (!BatchCommitted[i])
				{
					UE_LOG(LogBF6HighPoly, Error, TEXT("runtime render build failed for %s"), *SM->GetName());
					OutFailed++;
					continue;
				}
				if (!BatchNanite[i]) GRuntimeMeshCountThisBuild++;
				const FGroup& BuiltGroup = ByMesh[Names[i]];
				const int32 Ordinal = BatchOrdinal[i];

				// MeshSubsetCategory_TransparentDecal is the exact per-section
				// signal. A mesh made only of those sections is surface paint and
				// must not cast a duplicate silhouette into the scene.
				bool bOnlyDecal = Decoded[i].Num() > 0;
				for (const BF6HP::FCore::FSection& S : Decoded[i])
					if (!S.bDecal) { bOnlyDecal = false; break; }

				// HISM instance transforms are floats relative to their component even
				// in an LWC world.  Keeping every component at world zero made the two
				// 80-98 km ring placements and the 67 km volcano exceed UE's 2,097,151
				// cm safe matrix origin and trip DoubleFloat.cpp's precision ensure.
				// Partition in 10 km world cells and put only the small residual in the
				// instance transform.  This preserves the authored double-precision
				// world placement instead of clamping or deleting the far world.
				constexpr double kHismCellCm = 1000000.0;
				TMap<FIntVector, TArray<const BF6HP::FPlacement*>> Cells;
				for (const BF6HP::FPlacement* p : ByMesh[Names[i]].Rows)
				{
					const FVector World = ToUnreal(p->Origin);
					const FIntVector Cell(
						FMath::RoundToInt(World.X / kHismCellCm),
						FMath::RoundToInt(World.Y / kHismCellCm),
						FMath::RoundToInt(World.Z / kHismCellCm));
					Cells.FindOrAdd(Cell).Add(p);
				}
				if (Cells.Num() > 1) SpatialHismSplits += Cells.Num() - 1;

				int32 CellIndex = 0;
				for (const TPair<FIntVector, TArray<const BF6HP::FPlacement*>>& CellRows : Cells)
				{
					const FVector Anchor(
						(double)CellRows.Key.X * kHismCellCm,
						(double)CellRows.Key.Y * kHismCellCm,
						(double)CellRows.Key.Z * kHismCellCm);
					UHierarchicalInstancedStaticMeshComponent* H =
						NewObject<UHierarchicalInstancedStaticMeshComponent>(A,
							*FString::Printf(TEXT("I_%d_%d"), Ordinal, CellIndex++));
					H->SetupAttachment(Root);
					BF6HP::Shared::MakeUnselectable(H);
					H->SetStaticMesh(SM);
					H->SetMobility(EComponentMobility::Static);
					H->SetRelativeLocation(Anchor);
					H->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					H->SetCanEverAffectNavigation(false);
					// The virtual-shadow queue was overflowing on Aftermath. Every
					// vehicle is on this fallback path because glass makes the asset
					// non-Nanite, and letting tens of thousands of fallback instances
					// mark VSM pages produced both the warning and vehicle shimmer.
					// Keep shadows on Nanite buildings/terrain; disable only the
					// preview fallback family and authored surface decals.
					if (bOnlyDecal || !SM->IsNaniteEnabled()) H->SetCastShadow(false);
					H->bAffectDistanceFieldLighting = false;
					H->bVisibleInRayTracing = false;
					H->ComponentTags.Add(FName(*(FString(TEXT("BF6SourceMesh=")) +
						ByMesh[Names[i]].Mesh)));
					if (BuiltGroup.bRuntimeGlow)
						H->ComponentTags.Add(FName(TEXT("BF6RuntimeGlow=active-source")));
					if (!ByMesh[Names[i]].Variation.IsEmpty())
						H->ComponentTags.Add(FName(*(FString(TEXT("BF6Variation=")) +
							ByMesh[Names[i]].Variation)));
					if (!ByMesh[Names[i]].Mode.IsEmpty())
					{
						// the game-mode switch finds these by tag and flips them
						H->ComponentTags.Add(FName(*(FString(TEXT("BF6GameMode=")) +
							ByMesh[Names[i]].Mode)));
						H->SetVisibility(bAllModes ||
							ByMesh[Names[i]].Mode.Equals(Chosen, ESearchCase::IgnoreCase), true);
					}
					// Configure the generated HISM before registration so UE never tries
					// to create physics bodies for the intentionally collisionless map.
					H->RegisterComponent();

					TArray<FTransform> Xf;
					Xf.Reserve(CellRows.Value.Num());
					for (const BF6HP::FPlacement* p : CellRows.Value)
					{
					// The basis carries scale as well as rotation, so it goes
					// across as a matrix. Y and Z swap to match the vertices;
					// the columns are reordered the same way so the two stay in
					// step.
					const FVector X(p->Right.X,   p->Right.Z,   p->Right.Y);
					const FVector Y(p->Up.X,      p->Up.Z,      p->Up.Y);
					const FVector Z(p->Forward.X, p->Forward.Z, p->Forward.Y);
					const FVector Local = ToUnreal(p->Origin) - Anchor;
					MaxInstanceLocalCm = FMath::Max(MaxInstanceLocalCm,
						FMath::Max3(FMath::Abs(Local.X), FMath::Abs(Local.Y), FMath::Abs(Local.Z)));
					Xf.Add(FTransform(FMatrix(X, Z, Y, Local)));
					}
					H->AddInstances(Xf, false);
					Placed += Xf.Num();
					SpatialHismComponents++;
					if (Xf.Num() > 0) GBuiltAnything = true;
				}
			}
			SecComponents += FPlatformTime::Seconds() - T;
		}
		SecObjects = FPlatformTime::Seconds() - PhaseStart;


		UE_LOG(LogBF6HighPoly, Log,
			TEXT("placement precision: %d spatial HISM component(s), %d extra cell split(s), ")
			TEXT("maximum local origin %.0f cm (UE safe limit 2097151 cm)"),
			SpatialHismComponents, SpatialHismSplits, MaxInstanceLocalCm);

		// Everything built together, on every core, once.
		if (Task) Task->EnterProgressFrame(2.f, LOCTEXT("Handing", "Handing the meshes to Unreal"));
		const double TB = FPlatformTime::Seconds();
		BuildAll(Pending);
		SecBuild = FPlatformTime::Seconds() - TB;

		// AFTER BuildAll, NOT BEFORE. Setting a component material while its static
		// mesh is still queued in Pending does not survive: the build assigns the
		// mesh's own slot materials afterwards and the override is gone. The log
		// still counted a success, so the sheet reported as shaded while it was
		// drawing with MID_M_BF6HighPoly_Opaque_0 - the white default.
		// DISTANT WATER. The far world carries an ocean-horizon plane, and it is
		// the game's answer to "water everywhere": the simulated FFT sea is a
		// 5,946 m square, and everything past that is this 16-triangle sheet.
		// It binds no texture because the game shades it as water, so it is built
		// with the ordinary mesh path for its real transform and extent, and then
		// handed the water material here.
		//
		// FFT stays OFF on it. It has four corners across 192 km, so vertex
		// displacement could not represent a wave even if it were bound, and the
		// binder only walks registered water surfaces - this is not one, so
		// BF6FFTEnabled keeps the 0 default and the sheet stays flat.
		if (GHasOceanDesc)
		{
			int32 HorizonShaded = 0;
			TArray<UStaticMeshComponent*> Comps;
			A->GetComponents<UStaticMeshComponent>(Comps);
			for (UStaticMeshComponent* SMC : Comps)
			{
				if (!SMC || !SMC->GetStaticMesh()) continue;
				// MATCH THE TAG, NOT THE MESH NAME. Built meshes are named
				// M_<id> ("M_2849"), so the source asset is only recoverable from
				// the BF6SourceMesh component tag the placement path writes.
				bool bIsHorizon = false;
				for (const FName& Tag : SMC->ComponentTags)
				{
					if (Tag.ToString().Contains(TEXT("oceanhorizon"),
					                            ESearchCase::IgnoreCase))
					{
						bIsHorizon = true;
						break;
					}
				}
				if (!bIsHorizon) continue;
				UMaterialInstanceDynamic* MID =
					WaterMaterialFor(SMC, GOceanDesc, nullptr);
				if (!MID) continue;
				// The sheet is bound to the FFT with the sea (see
				// BF6_ForEachWaterMaterial), so it gets the ocean's own per-pixel
				// normals and foam. Its four corners cannot carry a wave, and at
				// 96 km a few metres of WPO on a corner is nothing, so no
				// parameter is overridden here: whatever the sea does, it does.
				const int32 Slots = FMath::Max(SMC->GetNumMaterials(), 1);
				for (int32 Slot = 0; Slot < Slots; ++Slot)
					SMC->SetMaterial(Slot, MID);
				// CLAIM IT, or the mode pass takes it straight back off. The
				// water material is an OVERRIDE, and the textured-mode cleanup
				// empties overrides on every component it does not recognise.
				// It exempts Water_ and Light_ by name; this component is named
				// for its instance group ("I_2850_0"), so it needs a mark of its
				// own. Without this the build logs a shaded sheet while the sheet
				// draws with the white default.
				SMC->ComponentTags.AddUnique(FName(TEXT("BF6WaterShaded")));
				// SAY HOW BIG IT ACTUALLY ENDED UP. The authored placement is
				// scale 75 on a 2,560 m sheet, so the drawn plane must be 192 km
				// across. If the instance scale arrives as 1 the sheet is only
				// 2.56 km, sits entirely under the 5,946 m simulated sea, and is
				// invisible everywhere - which looks exactly like "not built".
				// REBUILD THE BOUNDS, or none of the above matters.
				//
				// This component received its instance while M_2849 was still an
				// empty UStaticMesh queued in Pending, so it cached bounds of
				// ZERO. BuildAll then filled the mesh in, but nothing told the
				// component to look again - and Unreal culls on bounds, so a
				// 192 km sheet with a zero-extent box is culled from every camera
				// in the level. It was built, placed, scaled 75 and correctly
				// materialed, and it drew nowhere.
				// GIVE THE SHEET A THICKNESS. Sixteen triangles at one height make
				// a mesh whose bounds are a zero-volume slab (min Z == max Z ==
				// -0.007 cm, measured live), and Unreal will not draw a primitive
				// whose bounds have no volume: it fails occlusion with zero
				// samples from every camera, above or below, and nothing about
				// material, scale, visibility or cull distance changes that. The
				// simulated sea and the pool both carry a Z extension for exactly
				// this reason; this sheet came through the placement path and had
				// none. 400 cm local is 300 m at the placement's scale of 75,
				// which is generous and costs nothing.
				if (UStaticMesh* SheetMesh = SMC->GetStaticMesh())
				{
					SheetMesh->SetPositiveBoundsExtension(FVector(0, 0, 400.f));
					SheetMesh->SetNegativeBoundsExtension(FVector(0, 0, 400.f));
					SheetMesh->CalculateExtendedBounds();
				}
				FTransform Inst;
				if (UInstancedStaticMeshComponent* ISM =
					Cast<UInstancedStaticMeshComponent>(SMC))
				{
					// Re-seat the mesh so the instance data picks up the new box,
					// then rebuild the HISM cluster tree from it.
					ISM->SetStaticMesh(ISM->GetStaticMesh());
					if (UHierarchicalInstancedStaticMeshComponent* HISM =
						Cast<UHierarchicalInstancedStaticMeshComponent>(ISM))
					{
						HISM->BuildTreeIfOutdated(/*Async*/ false, /*ForceUpdate*/ true);
					}
					if (ISM->GetInstanceCount() > 0)
						ISM->GetInstanceTransform(0, Inst, /*bWorldSpace*/ true);
				}
				SMC->UpdateBounds();
				SMC->MarkRenderStateDirty();
				const FBoxSphereBounds WB = SMC->Bounds;
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("distant water: sheet instance scale (%.2f %.2f %.2f), ")
					TEXT("world bounds +/-(%.0f %.0f %.0f)m centred (%.0f %.0f %.1f)m"),
					Inst.GetScale3D().X, Inst.GetScale3D().Y, Inst.GetScale3D().Z,
					WB.BoxExtent.X * 0.01, WB.BoxExtent.Y * 0.01, WB.BoxExtent.Z * 0.01,
					WB.Origin.X * 0.01, WB.Origin.Y * 0.01, WB.Origin.Z * 0.01);

				// THE SHIPPED SHEET'S RENDER DATA DOES NOT DRAW. Measured, not
				// argued: the same component with an engine Plane at the same
				// transform renders; this mesh renders from neither side, in
				// wireframe, with occlusion off, with a debug material, with
				// correct bounds. Sixteen triangles that never reach the screen
				// are worth nothing, so the sheet is rebuilt as the one thing
				// those triangles describe: a flat quad over the mesh's own XY
				// box, at the mesh's own height, under the placement's own
				// transform. Every number comes from the read mesh and the read
				// placement; nothing is authored here.
				//
				// Diagnostic first, so the cause can still be found: the raw
				// section the core decoded for it.
				if (UStaticMesh* Broken = SMC->GetStaticMesh())
				{
					FString SourceMesh;
					for (const FName& Tag : SMC->ComponentTags)
					{
						const FString S = Tag.ToString();
						if (S.StartsWith(TEXT("BF6SourceMesh="))) SourceMesh = S.Mid(14);
					}
					TArray<BF6HP::FCore::FSection> Raw;
					if (!SourceMesh.IsEmpty() &&
						GCore.ReadMesh(BF6HP::FCore::MeshResourceFor(SourceMesh), Raw) && Raw.Num() > 0)
					{
						const BF6HP::FCore::FSection& S0 = Raw[0];
						FString Heads;
						for (int32 i = 0; i < FMath::Min(4, S0.Pos.Num()); ++i)
							Heads += FString::Printf(TEXT(" (%.1f %.1f %.1f)"), S0.Pos[i].X, S0.Pos[i].Y, S0.Pos[i].Z);
						FString Ids;
						for (int32 i = 0; i < FMath::Min(6, S0.Idx.Num()); ++i)
							Ids += FString::Printf(TEXT(" %u"), S0.Idx[i]);
						UE_LOG(LogBF6HighPoly, Log,
							TEXT("distant water: shipped sheet raw: %d section(s), pos %d, idx %d, first pos%s, first idx%s, nrm0 (%.2f %.2f %.2f)"),
							Raw.Num(), S0.Pos.Num(), S0.Idx.Num(), *Heads, *Ids,
							S0.Nrm.Num() ? S0.Nrm[0].X : 0.f, S0.Nrm.Num() ? S0.Nrm[0].Y : 0.f,
							S0.Nrm.Num() ? S0.Nrm[0].Z : 0.f);
					}

					const FBox Box = Broken->GetBoundingBox();
					const double Zc = (Box.Min.Z + Box.Max.Z) * 0.5;   // the extension is symmetric
					FMeshDescription MD;
					FStaticMeshAttributes Attr(MD);
					Attr.Register();
					TVertexAttributesRef<FVector3f>         VPos = Attr.GetVertexPositions();
					TVertexInstanceAttributesRef<FVector2f> VUV  = Attr.GetVertexInstanceUVs();
					const FPolygonGroupID Group = MD.CreatePolygonGroup();
					Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("S0");
					FVertexID V[4];
					const double Xs[2] = { Box.Min.X, Box.Max.X };
					const double Ys[2] = { Box.Min.Y, Box.Max.Y };
					for (int32 gy = 0; gy < 2; ++gy)
						for (int32 gx = 0; gx < 2; ++gx)
						{
							V[gy * 2 + gx] = MD.CreateVertex();
							VPos[V[gy * 2 + gx]] = FVector3f((float)Xs[gx], (float)Ys[gy], (float)Zc);
						}
					auto Corner = [&](int32 gx, int32 gy) -> FVertexInstanceID
					{
						const FVertexInstanceID vi = MD.CreateVertexInstance(V[gy * 2 + gx]);
						VUV.Set(vi, 0, FVector2f((float)gx, (float)gy));
						return vi;
					};
					// Wound to face up, the same order the sea's own patch uses.
					MD.CreatePolygon(Group, TArray<FVertexInstanceID>{ Corner(0, 0), Corner(1, 1), Corner(1, 0) });
					MD.CreatePolygon(Group, TArray<FVertexInstanceID>{ Corner(0, 0), Corner(0, 1), Corner(1, 1) });
					FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
					FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals);

					UStaticMesh* Quad = NewObject<UStaticMesh>(A, TEXT("HorizonSheetMesh"), RF_Transient);
					FStaticMaterial QMat;
					QMat.MaterialSlotName = TEXT("S0");
					QMat.ImportedMaterialSlotName = QMat.MaterialSlotName;
					QMat.MaterialInterface = MID;
					Quad->GetStaticMaterials().Add(QMat);
					Quad->SetPositiveBoundsExtension(FVector(0, 0, 400.f));
					Quad->SetNegativeBoundsExtension(FVector(0, 0, 400.f));
					if (PrepareRuntimeRenderMesh(Quad, MD, 2))
					{
						// Named Water_ so every per-water pass (the FFT bind, the
						// mode cleanup exemption) treats it as the sea it continues.
						UStaticMeshComponent* Sheet = NewObject<UStaticMeshComponent>(A, TEXT("Water_Horizon"));
						Sheet->SetupAttachment(SMC->GetAttachParent() ? SMC->GetAttachParent() : Root);
						BF6HP::Shared::MakeUnselectable(Sheet);
						Sheet->SetMobility(EComponentMobility::Movable);
						Sheet->SetStaticMesh(Quad);
						Sheet->SetMaterial(0, MID);
						Sheet->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						Sheet->SetCastShadow(false);
						Sheet->SetCanEverAffectNavigation(false);
						Sheet->ComponentTags.Add(FName(TEXT("BF6WaterShaded")));
						Sheet->ComponentTags.Add(FName(*(TEXT("BF6SourceMesh=") + SourceMesh)));
						Sheet->RegisterComponent();
						Sheet->SetWorldTransform(Inst);
						// The dead instance is hidden, not destroyed: it still
						// documents the placement in the outliner.
						SMC->SetVisibility(false, false);
						SMC->ComponentTags.Remove(FName(TEXT("BF6WaterShaded")));
						UE_LOG(LogBF6HighPoly, Log,
							TEXT("distant water: horizon rebuilt as a quad %.0f x %.0f m at z %.2f m ")
							TEXT("(mesh box x %.0f..%.0f, scale %.1f); shipped 16-triangle instance hidden"),
							(Box.Max.X - Box.Min.X) * Inst.GetScale3D().X * 0.01,
							(Box.Max.Y - Box.Min.Y) * Inst.GetScale3D().Y * 0.01,
							(Inst.GetTranslation().Z + Zc * Inst.GetScale3D().Z) * 0.01,
							Box.Min.X, Box.Max.X, Inst.GetScale3D().X);
					}
					else
					{
						UE_LOG(LogBF6HighPoly, Warning, TEXT("distant water: horizon quad build failed"));
					}
				}
				++HorizonShaded;
			}
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("distant water: %d far-world ocean-horizon sheet(s) shaded as ")
				TEXT("water beyond the %.0f m simulated sea, bound with the sea"),
				HorizonShaded, GHasOceanDesc ? GOceanDesc.Size.X : 0.f);
			if (HorizonShaded > 0) BF6_RebindWaterMaterials();
		}


		// The stable water reflection path consumes a captured sky rather than
		// viewport history.  Recapture only after terrain, water, lights and object
		// components have all been registered, so a rebuild cannot retain a stale
		// environment from the scene it just replaced.
		if (USkyLightComponent* Sky = GHighPolySkyLight.Get())
		{
			Sky->RecaptureSky();
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("lighting: one-shot skylight recapture queued after full scene registration; real-time capture control is disabled"));
		}

		// DISPATCHED, NOT FINISHED. BatchBuild hands the meshes to Unreal's
		// asynchronous builder and returns; on MP_Battery it returned in 0.4 s
		// while 292 seconds of Nanite work carried on across the cores for
		// another half minute. Reporting that half second as the build time was
		// a comfortable lie, so the number is named for what it measures.
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("build: decode %.1fs, describe %.1fs, dispatch %.1fs (%d meshes, %d runtime-render, %d nanite covering %.1f%% of prepared triangles, %d translucent so no nanite, %d placements); ")
			TEXT("decode: %d mesh(es) from the derived cache, %.1fs of the decode was the parallel cache load, %.0f MB handed to the cache; ")
			TEXT("commit: create+materials %.1fs, runtime render %.1fs parallel, nanite commit %.1fs, components %.1fs"),
			SecDecode, SecDescribe, SecBuild, Built, GRuntimeMeshCountThisBuild, GNaniteCountThisBuild,
			GPreparedTrisThisBuild.load() > 0
				? 100.0 * (double)GNaniteTrisThisBuild.load() / (double)GPreparedTrisThisBuild.load() : 0.0,
			GNoNaniteTranslucent, Placed,
			MeshesFromCache, SecMeshCacheLoad, MeshCacheBytesWritten.load() / 1048576.0,
			SecCreate, SecCommitRuntime, SecCommitNanite, SecComponents);

		UE_LOG(LogBF6HighPoly, Log,
			TEXT("materials: %d instance(s), %d binding(s) of which %d bound; textures %d uploaded at <=%d px from the core's capped runtime path plus %d full-resolution road-paint texture(s), %d refused; ")
			TEXT("time: texture payload %.1fs (%d of %d from the derived cache, %.1fs of it parallel prefetch, %.0f MB handed to the cache), texture upload %.1fs, parent materials %.1fs, instances %.1fs"),
			GMidsMade, GBindingsSeen, GBindingsBound, GTexUploaded,
			GPreviewTextureMax, GTexHighQuality, GTexRefused,
			GSecTexDecode + GSecTexPrefetch, GTexFromCache, GTexUploaded, GSecTexPrefetch,
			GTexCacheBytesWritten / 1048576.0, GSecTexUpload, GSecParentMaterials,
			FMath::Max(0.0, GSecObjectMaterials - GSecTexDecode - GSecTexUpload - GSecParentMaterials));
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("material reuse: %d exact hit(s), %d unique instance(s); unique-per-section control 0 hit(s)"),
			GMaterialCacheHits, GMaterialCacheMisses);
		{
			// Writes were queued behind the build; wait for them so the numbers
			// are final and no write outlives the map that produced it.
			const double FlushStart = FPlatformTime::Seconds();
			BF6HP::DiskCache::FlushPendingWrites();
			const double SecFlush = FPlatformTime::Seconds() - FlushStart;
			const BF6HP::DiskCache::FStats CS = BF6HP::DiskCache::Stats();
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("DERIVED CACHE: %d hit(s), %d miss(es), %d written; %.1f MB read in %.2fs, ")
				TEXT("%.1f MB written; signature %s; packed %.1f MB raw, %.2fs writing on workers, %.2fs waited at the end"),
				CS.Hits, CS.Misses, CS.Writes, CS.BytesRead / 1048576.0, CS.SecondsLoading,
				CS.BytesWritten / 1048576.0, *BF6HP::DiskCache::InstallSignature(),
				CS.BytesRawPacked / 1048576.0, CS.SecondsSaving, SecFlush);
		}
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("FULL LOAD PHASES: group %.2fs, receivers %.2fs, terrain %.2fs, roads %.2fs, water %.2fs, scatter %.2fs, lighting %.2fs, objects %.2fs (materials/textures %.2fs, mesh commit %.2fs), dispatch %.2fs, total %.2fs"),
			SecGroup, SecReceivers, SecTerrain, SecRoads, SecWater, SecScatter,
			SecLighting, SecObjects, GSecObjectMaterials, GSecObjectPrepare,
			SecBuild, FPlatformTime::Seconds() - TotalStart);

		OutMeshes = Built;
		return Placed;
	}

	// Where the add-on keeps its own caches: a folder of its own name under the
	// tool's saved dir, so uninstalling leaves nothing behind.
	FString CacheDir()
	{
		return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("HighPoly"));
	}

	// ---- the install the creator pointed us at -----------------------------
	//
	// The add-on reads the player's OWN copy of Battlefield 6, so it asks for it
	// rather than guessing. Two reasons, and the second is the important one.
	// A guess is fragile: the tool's own discovery is a single hardcoded Steam
	// path, and plenty of people install elsewhere or through the EA App. And a
	// guess is QUIET: someone should know they are reading the game's data, not
	// find out later. So the first use is an explicit choice, and every use
	// after it says where the data is coming from.

	FString InstallRecordPath()
	{
		return FPaths::Combine(CacheDir(), TEXT("install.txt"));
	}

	// Is this actually a Battlefield 6 install? Checked by the two things this
	// add-on cannot work without, and the message names whichever is missing so
	// "that folder is wrong" is never the whole answer.
	bool LooksLikeInstall(const FString& Dir, FString& OutWhy)
	{
		OutWhy.Reset();
		if (Dir.IsEmpty()) { OutWhy = TEXT("no folder chosen"); return false; }
		if (!FPaths::DirectoryExists(Dir)) { OutWhy = TEXT("that folder does not exist"); return false; }

		const FString Exe = FPaths::Combine(Dir, TEXT("bf6.exe"));
		const bool bExe = FPaths::FileExists(Exe);

		TArray<FString> Tocs;
		IFileManager::Get().FindFiles(Tocs, *(FPaths::Combine(Dir, TEXT("Data/Win32")) / TEXT("*.toc")), true, false);

		if (!bExe && Tocs.Num() == 0)
		{
			OutWhy = TEXT("no bf6.exe and no Data/Win32 archives there. Pick the folder that CONTAINS bf6.exe.");
			return false;
		}
		if (Tocs.Num() == 0) { OutWhy = TEXT("no archives in Data/Win32 under that folder"); return false; }
		if (!bExe)
		{
			OutWhy = TEXT("no bf6.exe in that folder. The executable carries the type schema, so placements cannot be read without it.");
			return false;
		}
		return true;
	}

	// The remembered install, or empty when there is none or it has gone away
	// (the game moved, or the drive is not mounted). Re-validated every time
	// rather than trusted: a stored path that no longer works should send the
	// creator back to the picker, not fail later with something cryptic.
	FString StoredInstall()
	{
		FString Dir;
		if (!FFileHelper::LoadFileToString(Dir, *InstallRecordPath())) return FString();
		Dir.TrimStartAndEndInline();
		FString Why;
		return LooksLikeInstall(Dir, Why) ? Dir : FString();
	}

	void StoreInstall(const FString& Dir)
	{
		// The folder is ours to create: nothing else has written to the add-on's
		// cache yet on a fresh install, and SaveStringToFile will not make it.
		IFileManager::Get().MakeDirectory(*CacheDir(), true);
		FFileHelper::SaveStringToFile(Dir, *InstallRecordPath());
	}

	// Ask for the folder. Loops on a wrong pick rather than giving up, because
	// the usual mistake is landing one level too high or too low.
	bool BrowseForInstall(FString& OutDir)
	{
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP) return false;

		// Start where the tool thinks the game is, if it thinks anything. That
		// is a suggestion, not an answer: the creator still confirms it.
		FString Start = BF6Ext::GameInstallDir();
		if (Start.IsEmpty() || !FPaths::DirectoryExists(Start)) Start = FPaths::RootDir();

		for (;;)
		{
			FString Picked;
			const bool bOk = DP->OpenDirectoryDialog(nullptr,
				TEXT("Where is Battlefield 6 installed? Pick the folder that contains bf6.exe."),
				Start, Picked);
			if (!bOk || Picked.IsEmpty()) return false;

			FString Why;
			if (LooksLikeInstall(Picked, Why))
			{
				OutDir = Picked;
				StoreInstall(Picked);
				return true;
			}
			Start = Picked;
			const EAppReturnType::Type Again = FMessageDialog::Open(EAppMsgType::YesNo,
				FText::FromString(FString::Printf(
					TEXT("That does not look like a Battlefield 6 install: %s\n\nTry another folder?"), *Why)));
			if (Again != EAppReturnType::Yes) return false;
		}
	}

	// What the pill uses. Empty means the creator declined, and nothing should
	// happen quietly after that.
	FString GInstall;

	FString EnsureInstall()
	{
		if (!GInstall.IsEmpty())
		{
			FString Why;
			if (LooksLikeInstall(GInstall, Why)) return GInstall;
			GInstall.Reset();   // it moved since we last looked
		}
		GInstall = StoredInstall();
		return GInstall;
	}

	struct FExactPageResult
	{
		bool bOk = false;
		FString Error;
		int32 Side = 0;
		float LoX = 0.f;
		float LoZ = 0.f;
		float Span = 0.f;
		double EmptyRealMae = 0.0;
		TArray<uint8> Rgba;
		TArray<uint8> MaterialRgba;
		TArray<uint8> NormalRgba;
	};

	// Parse only the framed protocol line. Everything else printed by the
	// harness is evidence for a human/log; it is not an input to rendering.
	bool ParseExactPageFrame(const FString& StdOut, FExactPageResult& Out)
	{
		const FString MarkerV3 = TEXT("BF6_PAGE_AOVS_RGBA8_V3 ");
		const FString MarkerV2 = TEXT("BF6_PAGE_AOVS_RGBA8_V2 ");
		const FString MarkerV1 = TEXT("BF6_PAGE_RGBA8_V1 ");
		int32 Begin = StdOut.Find(MarkerV3, ESearchCase::CaseSensitive);
		const bool bHasMaterial = Begin != INDEX_NONE;
		if (!bHasMaterial) Begin = StdOut.Find(MarkerV2, ESearchCase::CaseSensitive);
		const bool bHasNormal = Begin != INDEX_NONE;
		if (!bHasNormal) Begin = StdOut.Find(MarkerV1, ESearchCase::CaseSensitive);
		if (Begin == INDEX_NONE)
		{
			Out.Error = TEXT("sidecar returned no BF6_PAGE_AOVS_RGBA8_V3, V2 or V1 frame");
			return false;
		}
		int32 End = StdOut.Find(TEXT("\n"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, Begin);
		if (End == INDEX_NONE) End = StdOut.Len();
		const FString Line = StdOut.Mid(Begin, End - Begin).TrimStartAndEnd();
		TArray<FString> Fields;
		Line.ParseIntoArrayWS(Fields);
		if (Fields.Num() != (bHasMaterial ? 8 : (bHasNormal ? 7 : 6))
			|| !LexTryParseString(Out.Side, *Fields[1])
			|| !LexTryParseString(Out.LoX, *Fields[2])
			|| !LexTryParseString(Out.LoZ, *Fields[3])
			|| !LexTryParseString(Out.Span, *Fields[4])
			|| Out.Side <= 0 || Out.Side > 4096 || !FMath::IsFinite(Out.LoX)
			|| !FMath::IsFinite(Out.LoZ) || !FMath::IsFinite(Out.Span) || Out.Span <= 0.f)
		{
			Out.Error = TEXT("sidecar page frame metadata is invalid");
			return false;
		}
		if (!FBase64::Decode(Fields[5], Out.Rgba))
		{
			Out.Error = TEXT("sidecar page frame is not valid base64");
			return false;
		}
		const int64 Want = (int64)Out.Side * Out.Side * 4;
		if (Out.Rgba.Num() != Want)
		{
			Out.Error = FString::Printf(
				TEXT("sidecar page frame is truncated (%d bytes, expected %lld)"),
				Out.Rgba.Num(), Want);
			return false;
		}
		if (bHasMaterial)
		{
			if (!FBase64::Decode(Fields[6], Out.MaterialRgba)
				|| Out.MaterialRgba.Num() != Want)
			{
				Out.Error = FString::Printf(
					TEXT("sidecar material frame is invalid (%d bytes, expected %lld)"),
					Out.MaterialRgba.Num(), Want);
				return false;
			}
		}
		if (bHasNormal)
		{
			const int32 NormalField = bHasMaterial ? 7 : 6;
			if (!FBase64::Decode(Fields[NormalField], Out.NormalRgba)
				|| Out.NormalRgba.Num() != Want)
			{
				Out.Error = FString::Printf(
					TEXT("sidecar normal frame is invalid (%d bytes, expected %lld)"),
					Out.NormalRgba.Num(), Want);
				return false;
			}
		}

		// The executable itself requires the empty-work-list control to diverge
		// before returning zero. Retain its score in Unreal's log beside the page.
		const FString MaeNeedle = TEXT("empty/real MAE ");
		const int32 MaeAt = StdOut.Find(MaeNeedle, ESearchCase::CaseSensitive);
		if (MaeAt != INDEX_NONE)
			Out.EmptyRealMae = FCString::Atod(*StdOut.Mid(MaeAt + MaeNeedle.Len()));
		return true;
	}

	UTexture2D* MakeExactPageTexture(const TArray<uint8>& Rgba, int32 Side,
		bool bSrgb)
	{
		if (Side <= 0 || Rgba.Num() != (int64)Side * Side * 4)
			return nullptr;
		UTexture2D* Tex = UTexture2D::CreateTransient(
			Side, Side, PF_B8G8R8A8, NAME_None);
		if (!Tex) return nullptr;
		// U1 is already IEC-sRGB encoded by the shipped evaluator and must be
		// sampled as colour so Unreal decodes it once. U2/U3 are packed linear data.
		Tex->SRGB = bSrgb;
		Tex->CompressionSettings = bSrgb ? TC_Default : TC_VectorDisplacementmap;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		Tex->Filter = TF_Bilinear;
		Tex->NeverStream = true;
		if (uint8* D = static_cast<uint8*>(
			Tex->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE)))
		{
			for (int64 i = 0; i < (int64)Side * Side; ++i)
			{
				// PF_B8G8R8A8 is BGRA in memory; the pipe is explicitly RGBA.
				D[i * 4 + 0] = Rgba[i * 4 + 2];
				D[i * 4 + 1] = Rgba[i * 4 + 1];
				D[i * 4 + 2] = Rgba[i * 4 + 0];
				D[i * 4 + 3] = Rgba[i * 4 + 3];
			}
			Tex->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		Tex->UpdateResource();
		Tex->AddToRoot();
		return Tex;
	}

	void DisableExactPage()
	{
		if (GroundMat)
			GroundMat->SetScalarParameterValue(TEXT("ExactPageEnabled"), 0.f);
		if (GExactPageTex && GExactPageTex->IsRooted()) GExactPageTex->RemoveFromRoot();
		if (GExactMaterialTex && GExactMaterialTex->IsRooted()) GExactMaterialTex->RemoveFromRoot();
		if (GExactNormalTex && GExactNormalTex->IsRooted()) GExactNormalTex->RemoveFromRoot();
		GExactPageTex = nullptr;
		GExactMaterialTex = nullptr;
		GExactNormalTex = nullptr;
		GExactPageStatus = TEXT("disabled; approximate terrain is active");
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("DXIL terrain page disabled; approximate ground is the active control"));
	}

	void StartExactPage(float CentreX, float CentreZ, float SpanM, int32 Resolution = 512)
	{
		if (GExactPageBusy)
		{
			BF6Ext::Notify(TEXT("High Poly: a DXIL terrain page is already running."));
			return;
		}
		if (!GroundMat || BF6Ext::CurrentLevel().IsEmpty())
		{
			BF6Ext::Notify(TEXT("High Poly: build terrain before requesting a DXIL page."));
			return;
		}
		if (!FMath::IsFinite(CentreX) || !FMath::IsFinite(CentreZ)
			|| !FMath::IsFinite(SpanM) || SpanM <= 0.f
			|| Resolution < 64 || Resolution > 2048 || (Resolution & 7) != 0)
		{
			BF6Ext::Notify(TEXT("High Poly: DXIL page coordinates/span/resolution are invalid."));
			return;
		}
		const FString Install = EnsureInstall();
		const FString Exe = FPaths::Combine(BF6Ext::ToolPluginDir(),
			TEXT("Source/ThirdParty/libbf6/bin/Win64/terraindxil_dispatch_test.exe"));
		if (Install.IsEmpty() || !FPaths::FileExists(Exe))
		{
			BF6Ext::Notify(Install.IsEmpty()
				? TEXT("High Poly: no Battlefield 6 install is configured.")
				: TEXT("High Poly: the terrain DXIL sidecar is not installed."));
			return;
		}

		const FString Level = BF6Ext::CurrentLevel();
		const uint64 Epoch = GMapEpoch;
		GExactPageBusy = true;
		GExactPageStatus = FString::Printf(
			TEXT("evaluating %dx%d over %.1f m at %.3f, %.3f; authored texture top mips"),
			Resolution, Resolution, SpanM, CentreX, CentreZ);
		BF6Ext::Notify(FString::Printf(
			TEXT("High Poly: evaluating %dx%d over %.1f m with the shipped DXIL..."),
			Resolution, Resolution, SpanM));
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("DXIL terrain page start: %s centre %.6f %.6f span %.3f m, %dx%d, ")
			TEXT("texture source=authored top mip (512-cap control rejected: MAE 0.05306182)"),
			*Level, CentreX, CentreZ, SpanM, Resolution, Resolution);

		Async(EAsyncExecution::Thread,
			[Install, Exe, Level, CentreX, CentreZ, SpanM, Resolution, Epoch]
		{
			TSharedRef<FExactPageResult, ESPMode::ThreadSafe> Result =
				MakeShared<FExactPageResult, ESPMode::ThreadSafe>();
			const FString Args = FString::Printf(
				TEXT("\"%s\" \"%s\" --page-dispatch %.9g %.9g ignored.ppm ")
				TEXT("--page-size-m %.9g --page-resolution %d --texture-max-dim 0 ")
				TEXT("--raw-evaluator-coverage ")
				TEXT("--game-culling --stdout-aovs-rgba8 --no-files"),
				*Install, *Level, CentreX, CentreZ, SpanM, Resolution);
			int32 ReturnCode = -1;
			FString StdOut, StdErr;
			const bool bLaunched = FPlatformProcess::ExecProcess(
				*Exe, *Args, &ReturnCode, &StdOut, &StdErr,
				*FPaths::GetPath(Exe), true);
			if (!bLaunched || ReturnCode != 0)
			{
				const FString Tail = (StdErr.IsEmpty() ? StdOut : StdErr).Right(900).TrimStartAndEnd();
				Result->Error = FString::Printf(
					TEXT("sidecar %s (exit %d)%s%s"),
					bLaunched ? TEXT("failed") : TEXT("could not launch"), ReturnCode,
					Tail.IsEmpty() ? TEXT("") : TEXT(": "), *Tail);
			}
			else
			{
				Result->bOk = ParseExactPageFrame(StdOut, *Result);
			}

			AsyncTask(ENamedThreads::GameThread,
				[Result, Level, Epoch]
			{
				GExactPageBusy = false;
				if (!Result->bOk)
				{
					GExactPageStatus = FString::Printf(TEXT("failed: %s"), *Result->Error);
					UE_LOG(LogBF6HighPoly, Error, TEXT("DXIL terrain page: %s"),
						*Result->Error);
					BF6Ext::Notify(FString::Printf(
						TEXT("High Poly DXIL page failed: %s"), *Result->Error));
					return;
				}
				if (Epoch != GMapEpoch || BF6Ext::CurrentLevel() != Level || !GroundMat)
				{
					GExactPageStatus = TEXT("discarded because the map/build changed");
					UE_LOG(LogBF6HighPoly, Display,
						TEXT("DXIL terrain page discarded: the map/build changed while it ran"));
					return;
				}
				UTexture2D* NewTex = MakeExactPageTexture(Result->Rgba, Result->Side, true);
				UTexture2D* NewMaterial = MakeExactPageTexture(
					Result->MaterialRgba, Result->Side, false);
				UTexture2D* NewNormal = MakeExactPageTexture(
					Result->NormalRgba, Result->Side, false);
				if (!NewTex || !NewMaterial || !NewNormal)
				{
					if (NewTex && NewTex->IsRooted()) NewTex->RemoveFromRoot();
					if (NewMaterial && NewMaterial->IsRooted()) NewMaterial->RemoveFromRoot();
					if (NewNormal && NewNormal->IsRooted()) NewNormal->RemoveFromRoot();
					GExactPageStatus = TEXT("failed while uploading U1/U2/U3 AOVs");
					BF6Ext::Notify(TEXT("High Poly: DXIL page upload failed."));
					return;
				}
				UTexture2D* OldTex = GExactPageTex;
				UTexture2D* OldMaterial = GExactMaterialTex;
				UTexture2D* OldNormal = GExactNormalTex;
				GExactPageTex = NewTex;
				GExactMaterialTex = NewMaterial;
				GExactNormalTex = NewNormal;
				GroundMat->SetTextureParameterValue(TEXT("ExactPage"), NewTex);
				GroundMat->SetTextureParameterValue(TEXT("ExactMaterialPage"), NewMaterial);
				GroundMat->SetTextureParameterValue(TEXT("ExactNormalPage"), NewNormal);
				GroundMat->SetVectorParameterValue(TEXT("ExactPageLo"),
					FLinearColor(Result->LoX, Result->LoZ, 0, 0));
				GroundMat->SetVectorParameterValue(TEXT("ExactPageSpan"),
					FLinearColor(Result->Span, Result->Span, 1, 1));
				GroundMat->SetScalarParameterValue(TEXT("ExactPageEnabled"), 1.f);
				// U1 is retained as an unhandled-texel validity mask, but its colour
				// is not displayed until runtime layer order is recovered.  U2/U3
				// drive roughness and normal now; this is the passing visual control.
				GroundMat->SetScalarParameterValue(TEXT("ExactBaseColorEnabled"), 0.f);
				if (OldTex && OldTex->IsRooted()) OldTex->RemoveFromRoot();
				if (OldMaterial && OldMaterial->IsRooted()) OldMaterial->RemoveFromRoot();
				if (OldNormal && OldNormal->IsRooted()) OldNormal->RemoveFromRoot();
				GExactPageStatus = FString::Printf(
					TEXT("active %dx%d, lo %.3f %.3f, span %.1f m; authored U2/U3 top mips; ")
					TEXT("U1 colour disabled by paired quilt control; ")
					TEXT("empty/real MAE %.8f"),
					Result->Side, Result->Side, Result->LoX, Result->LoZ,
					Result->Span, Result->EmptyRealMae);
				UE_LOG(LogBF6HighPoly, Display,
					TEXT("DXIL terrain page active: %dx%d, lo %.6f %.6f, span %.3f m; ")
					TEXT("empty/real control MAE %.8f"),
					Result->Side, Result->Side, Result->LoX, Result->LoZ,
					Result->Span, Result->EmptyRealMae);
				BF6Ext::Notify(TEXT("High Poly: exact shipped-DXIL terrain page is active."));
			});
		});
	}

	void ReadLevel();   // below; StartRead defers to it once the menu is gone

	// CLOSE OUR PANEL FIRST, THEN BUILD ON THE NEXT TICK.
	//
	// The editor REFUSES to show a progress dialog while any Slate menu is open,
	// and says so: "Prevented a slow task dialog from being summoned while a
	// context menu was open". Our panel is a menu, so a build started from its
	// own button got no dialog at all - and then the read blocked the game
	// thread with nothing on screen, which is a hard freeze from the outside.
	//
	// Dismissing is not enough on its own: the menu stack unwinds on a tick, so
	// the work has to wait one frame for the menu to actually be gone. Hence the
	// next-tick hop rather than calling straight through.
	// A BUILD ASKED FOR WHILE THE CORE IS BUSY IS STILL A BUILD SOMEBODY ASKED
	// FOR.
	//
	// There is one libbf6 context and it is not re-entrant, so a build cannot
	// start while the placed-object catalogue is mounting or the previews are
	// drawing. That much is right. What was wrong is what happened next: the
	// request was DROPPED, with a notification nobody reads and, on the
	// ReadLevel path, with no notification at all.
	//
	// And the two collide by design. Choosing Clay or Textured switches the
	// placed objects on, which starts the catalogue mount; pressing BUILD in
	// the next few seconds - which is exactly what somebody does - then hit a
	// busy core and did nothing whatsoever. "The big build button doesn't do
	// anything" is that, every time.
	//
	// So the request waits its turn instead of being thrown away.
	bool GBuildWhenFree = false;
	// WHICH MAP ASKED. A queued build that fires after the user has opened a
	// different level builds the wrong map from an intent that expired, and
	// there was nothing to stop it: the ticker knew only a timeout.
	FString GBuildWhenFreeLevel;
	FTSTicker::FDelegateHandle GBuildWhenFreeTicker;
	void QueueBuildWhenFree(const TCHAR* Why);
	void CancelQueuedBuild(const TCHAR* Why);

	void StartRead()
	{
		if (BF6HP::Shared::CoreBusy())
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("full build queued: the placed-object module is reading the object catalogue"));
			QueueBuildWhenFree(TEXT("the object catalogue is still being read"));
			return;
		}
		if (GBuildQueuedOrRunning)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("full build request ignored: a build is already queued or running"));
			BF6Ext::Notify(TEXT("High Poly: a full build is already running."));
			return;
		}
		GBuildQueuedOrRunning = true;
		FSlateApplication::Get().DismissAllMenus();
		if (!GEditor)
		{
			GBuildQueuedOrRunning = false;
			return;
		}
		GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateLambda([]
		{
			if (FSlateApplication::Get().AnyMenusVisible())
			{
				// Still up: try once more rather than build into a freeze.
				FSlateApplication::Get().DismissAllMenus();
				GEditor->GetTimerManager()->SetTimerForNextTick(
					FTimerDelegate::CreateLambda([]{ ReadLevel(); }));
				return;
			}
			ReadLevel();
		}));
	}

	// Waits for the core, then builds. One ticker at a time: the flag is the
	// queue, so pressing BUILD five times while the catalogue mounts still
	// produces one build.
	// Drop a queued build and stop its ticker. Called when the intent behind it
	// has expired: the map changed, the scenery was cleared, or the module is
	// going down and a callback must not outlive it.
	void CancelQueuedBuild(const TCHAR* Why)
	{
		if (!GBuildWhenFree && !GBuildWhenFreeTicker.IsValid()) { return; }
		GBuildWhenFree = false;
		GBuildWhenFreeLevel.Reset();
		if (GBuildWhenFreeTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GBuildWhenFreeTicker);
			GBuildWhenFreeTicker.Reset();
		}
		if (Why && *Why)
		{
			GStatus = FString::Printf(TEXT("the queued build was dropped: %s"), Why);
			UE_LOG(LogBF6HighPoly, Display, TEXT("queued build cancelled: %s"), Why);
		}
	}

	void QueueBuildWhenFree(const TCHAR* Why)
	{
		GStatus = FString::Printf(TEXT("waiting: %s"), Why);
		if (GBuildWhenFree) { return; }
		GBuildWhenFree = true;
		GBuildWhenFreeLevel = BF6Ext::CurrentLevel();
		BF6Ext::Notify(FString::Printf(
			TEXT("High Poly: %s. The build will start on its own as soon as that finishes."), Why));
		const double GiveUpAt = FPlatformTime::Seconds() + 300.0;
		GBuildWhenFreeTicker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([GiveUpAt](float) -> bool
		{
			if (!GBuildWhenFree) { GBuildWhenFreeTicker.Reset(); return false; }   // cancelled or run

			// THE MAP IT WAS ASKED FOR, OR NOTHING. Waiting for the core can
			// take minutes, and a build that lands on whatever level happens to
			// be open by then is not the build anybody asked for.
			const FString Now = BF6Ext::CurrentLevel();
			if (Now != GBuildWhenFreeLevel)
			{
				GBuildWhenFreeTicker.Reset();
				CancelQueuedBuild(Now.IsEmpty()
					? TEXT("the map was closed")
					: TEXT("a different map was opened"));
				return false;
			}

			if (BF6HP::Shared::CoreBusy() || BF6HPPreviews::IsBusy()
				|| GBuildQueuedOrRunning)
			{
				// A lease that is never given back would otherwise leave this
				// polling for the rest of the session and the user waiting for
				// a build that is never coming. Five minutes, then say so.
				if (FPlatformTime::Seconds() < GiveUpAt) { return true; }
				GBuildWhenFree = false;
				GBuildWhenFreeTicker.Reset();
				GStatus = TEXT("gave up waiting for the core");
				UE_LOG(LogBF6HighPoly, Warning,
					TEXT("the queued build waited five minutes and the core never came free; press BUILD again"));
				BF6Ext::Notify(TEXT("High Poly: still busy after five minutes, so the queued build was dropped. Press BUILD again."));
				return false;
			}
			GBuildWhenFree = false;
			GBuildWhenFreeTicker.Reset();
			UE_LOG(LogBF6HighPoly, Display, TEXT("the core is free; starting the build that was waiting"));
			StartRead();
			return false;
		}), 0.5f);
	}


	// Build prepares the map and placed objects. Library pictures are optional
	// work started explicitly from the panel: one picture can stall editing.
	bool GPlacedAfterBuild = false;

	void BuildAndShow()
	{
		if (GMode == EMode::LowPoly) { GMode = EMode::Textured; }
		// THE PLACED OBJECTS ARE SWITCHED ON AFTER THE READ, NOT BEFORE IT.
		//
		// Switching them on here asked for the object catalogue, which takes
		// the one core lease, and the read on the very next line then found the
		// core busy and refused. The button that was supposed to do everything
		// was the one press guaranteed to do nothing.
		//
		// The map's own archives are mounted by the build anyway, so a prefab
		// that could not resolve before it stands a better chance after it.
		GPlacedAfterBuild = true;
		StartRead();
	}

	// Called once the read has finished, from the same place that reports it.
	void EnablePlacedAfterBuild()
	{
		// The placed objects go on first, and asking again is not a no-op: it
		// lifts a stop from an earlier runaway resolve and re-tries the types
		// that were missing before the build mounted this map's archives.
		if (GPlacedAfterBuild)
		{
			GPlacedAfterBuild = false;
			BF6HP::Placed::SetEnabled(true);
		}

	}

	// Read the open map out of the game and mark every placement.
	//
	// THE LEVEL THE CACHES BELONG TO. Everything below is per map.
	FString GBuiltLevel;

	template <typename T>
	void ReleaseRooted(T*& Ptr)
	{
		if (Ptr)
		{
			// ShutdownModule may run after the UObject array has already closed.
			// Even IsRooted() resolves the object's array index and asserts there.
			// Engine-exit teardown owns every remaining UObject, so only touch the
			// root set during a live map/plugin reset while CoreUObject is valid.
			if (!IsEngineExitRequested() && UObjectInitialized() && Ptr->IsRooted())
				Ptr->RemoveFromRoot();
			Ptr = nullptr;
		}
	}

	// Drop everything that belongs to the map we are leaving.
	//
	// TWO BUGS, one visible and one not.
	//
	// GTextureCache is keyed by the CORE's texture id, and those ids are
	// handed out per mount - id 42 on one map is a different sheet on the
	// next. Worse, a texture the core refuses is cached as a NULL against its
	// id, so every id that failed on the first map stayed permanently null on
	// the second and the new map came up with no textures at all. That is the
	// "it will not even try on a new map".
	//
	// And every raster below is AddToRoot'd, then overwritten by the next
	// build without being released, so each map left its whole ground behind -
	// two 4096 rasters, two 512 sheet arrays and their mip chains. That is
	// hundreds of megabytes a map, never freed.
	//
	// What deliberately SURVIVES: the parent materials, the linear-white and
	// mid-grey stand-ins, the default sheet array and the clay material. None
	// of those carry map data, and rebuilding them would only cost shader
	// compiles.
	void ResetPerMapState()
	{
		++GMapEpoch; // invalidates any background DXIL page from the old build
		// Not unrooted: these were never rooted. They stay alive through the
		// material instances that reference them, and those go with the
		// actors.
		GTextureCache.Empty();
		GTexPrefetched.Empty();
		GColorLutCache.Empty();
		GMaterialCache.Empty();

		ReleaseRooted(GGroundAlbedo);
		ReleaseRooted(GGroundFar);
		ReleaseRooted(GGroundNormal);
		ReleaseRooted(GCovIdxTex);
		ReleaseRooted(GCovWTex);
		ReleaseRooted(GCovIdxTex2);
		ReleaseRooted(GCovWTex2);
		ReleaseRooted(GCovParamTex);
		ReleaseRooted(GCovColourTex);
		ReleaseRooted(GSheetArray);
		ReleaseRooted(GHeightArray);
		ReleaseRooted(GMaskArray);
		ReleaseRooted(GWaterDepthTex);
		ReleaseRooted(GWaterHeightTex);
		ReleaseRooted(GWaterMaskAtlas);
		ReleaseRooted(GWaterMaskIndirection);
		GWaterMaskPageCount = 0;
		if (GWaterFFT) { GWaterFFT->Reset(); GWaterFFT.Reset(); }
		ReleaseRooted(GExactPageTex);
		ReleaseRooted(GExactMaterialTex);
		ReleaseRooted(GExactNormalTex);
		GExactPageStatus = TEXT("disabled by map/build reset");

		GroundMat = nullptr;
		GBuiltAnything = false;
		GTexUploaded = GTexHighQuality = GTexRefused = 0;
		GMidsMade = GBindingsSeen = GBindingsBound = 0;
		GRoadRecords = GRoadTris = GRoadElevated = GRoadColourless = GRoadPainted = 0;
		GRoadElevatedCandidates = GRoadBandOnlyRejected = 0;
		GRoadTintClamped = 0;
		GRoadReceiverVerts = GRoadPoolHoleTris = GRoadPoolShiftControl = 0;
		GRoadGroundSamples = GRoadGroundDeltaOverLift = 0;
		GRoadGroundDeltaSumCm = GRoadGroundDeltaMaxCm = 0.0;
		GTerrainCellLevels.Empty();
		GTerrainBaseCells = 0;
		GTerrainNativeStep = 1;
		GWaterBuilt = 0;
		GWaterClipmaps.Empty();
		GWaterClipmapHasCamera = false;
		GLightsBuilt = 0;
		GStatus.Reset();
		GBuiltLevel.Reset();

		// AND GIVE THE LOW-POLY MAP BACK.
		//
		// HideLowPolyNow hides the tool's own map once a build has replaced
		// its terrain or its objects. GBuiltAnything used to survive a map
		// load, so opening a second map left the tool convinced High Poly was
		// built, hid that map's low-poly geometry, and showed nothing in its
		// place. Clearing the flag is not enough on its own: the hide has
		// already been applied to the new map and has to be undone.
		ApplyLowPoly();
	}

	// EVERY WAY OUT OF A BUILT SCENE GOES THROUGH HERE.
	//
	// There were three of them and they did different amounts of work. CLEAR
	// destroyed the actors and left the water simulation ticking, the four
	// cascades allocated and every rooted per-map raster rooted; a map change
	// released the resources but never destroyed the actors, because the SDK
	// map selector REUSES the Unreal world (BF6UnrealSDK's OpenMap clears its
	// own tagged actors and broadcasts, it does not open a .umap), so
	// FEditorDelegates::OnMapOpened never fired and the old map's scenery stood
	// in the middle of the new one.
	//
	// One order, used by all of them: destroy what we own, then release what
	// those actors were the last users of. Reversing it would unroot textures
	// that live components are still sampling.
	void ClearBuiltScene()
	{
		// Clearing the scenery is asking for it NOT to be there. A build still
		// waiting its turn would put it straight back.
		CancelQueuedBuild(TEXT("the scenery was cleared"));
		if (GIsRunning) BF6Ext::ClearAddonActors(kAddonName);
		GLastCount = 0;
		ResetPerMapState();
	}

	// ---- the host's map lifecycle ------------------------------------------
	//
	// THE SDK MAP SELECTOR IS NOT AN UNREAL MAP OPEN. It keeps one world for
	// the whole session and swaps the contents by tag, so
	// FEditorDelegates::OnMapOpened - which this module used to listen to -
	// fires on File > Open Level and on nothing else a creator actually does.
	// Building MP_Isolated and then picking MP_Battery from CHOOSE MAPS left
	// Isolated's terrain, water, lights and props standing in Battery, its
	// low-poly map hidden underneath, and its texture cache keyed by the wrong
	// mount. The two other add-on modules (placed objects, game modes) had
	// always used BF6Ext::OnMapOpened/OnMapClosing; this one had not.
	//
	// Closing is where the work belongs: the SDK broadcasts it BEFORE anything
	// is torn down, while the world is still coherent enough to destroy our
	// actors in. Opening repeats the release as a floor, because a module that
	// loads late (Live Coding, or a plugin enabled mid-session) never saw the
	// close.
	void OnHostMapClosing(const FString& Level)
	{
		// Anything still reading the core is reading the map that is going.
		// Nothing here joins a worker - that is shutdown's job - but a cancelled
		// read stops producing and the generation bump below makes whatever it
		// does produce unusable.
		CancelQueuedBuild(TEXT("the map was closed"));
		GCore.Progress.Cancel();
		BF6HPPreviews::CancelBuild();
		ClearBuiltScene();   // bumps GMapEpoch, so late work knows it is stale
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("map %s closing: High Poly scenery removed and per-map state released"),
			*Level);
	}

	void OnHostMapOpened(const FString& Level, const FString& /*Save*/)
	{
		ClearBuiltScene();
		UE_LOG(LogBF6HighPoly, Log, TEXT("map %s opened, High Poly state reset"), *Level);
	}

	// SYNCHRONOUS, and it takes about half a minute the first time: mounting a
	// level's archives and indexing every partition's guid is most of it, and
	// both are cached in the core afterwards. Said out loud in the status line
	// rather than hidden behind a spinner that suggests otherwise.
	void ReadLevel()
	{
		struct FBuildFlagReset
		{
			~FBuildFlagReset() { GBuildQueuedOrRunning = false; }
		} BuildFlagReset;
		GBuildQueuedOrRunning = true;
		const FString Level = BF6Ext::CurrentLevel();
		if (Level.IsEmpty()) { GStatus = TEXT("no map open"); return; }
		// The previews build shares this core, and its worker may be mounting
		// the catalogue right now. Two readers on one context is the crash the
		// one-read-worker law exists for.
		if (BF6HPPreviews::IsBusy())
		{
			QueueBuildWhenFree(TEXT("the object previews are still building"));
			return;
		}
		// This one used to return in silence, which is the worst of the two:
		// no build, no message, nothing in the panel to explain it.
		if (BF6HP::Shared::CoreBusy())
		{
			QueueBuildWhenFree(TEXT("the object catalogue is still being read"));
			return;
		}

		// A NEW BUILD means the old rooted ground rasters are dead, even when it
		// is the same map. Keeping them was hidden by the old different-map-only
		// gate and every terrain diagnostic left another full texture set alive.
		if (!GBuiltLevel.IsEmpty())
		{
			if (GBuiltLevel == Level)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("rebuilding %s, dropping the previous build's textures"), *Level);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("map changed %s -> %s, dropping the previous map's textures"),
					*GBuiltLevel, *Level);
			}
			ResetPerMapState();
		}
		GBuiltLevel = Level;

		// THE GENERATION THIS BUILD BELONGS TO, latched after the reset above so
		// the build's own epoch bump is not mistaken for someone else's.
		//
		// The read below runs off-thread but this function pumps Slate while it
		// waits, so the map selector stays live and the map underneath can be
		// swapped out from a menu the progress dialog does not block. Publishing
		// the result afterwards then wrote the old map's scenery, status line and
		// notification over the new map. A -> B -> A is why the epoch is checked
		// as well as the level name: the name comes back, the generation does not.
		const uint64 BuildEpoch = GMapEpoch;
		auto MapChanged = [Level, BuildEpoch]
		{
			return BuildEpoch != GMapEpoch || BF6Ext::CurrentLevel() != Level;
		};

		const FString Install = EnsureInstall();
		if (Install.IsEmpty())
		{
			GStatus = TEXT("no game folder chosen");
			BF6Ext::Notify(TEXT("High Poly needs to know where Battlefield 6 is installed."));
			return;
		}

		// One authoritative reader. The old bf6_core_waterheight.dll was an
		// experimental side build and silently hid every API added afterward when
		// its file happened to remain beside the plugin. The canonical core now
		// owns the water-height path as well as every other live read.
		const FString Dll = BF6HP::CoreDllPath();
		// The derived cache keys on THIS install: every .toc and the exe.
		BF6HP::DiskCache::Configure(Install, CacheDir());
		{
			FScopeLock CoreLock(&BF6HP::Shared::CoreMutex());
			if (!GCore.IsOpen() && !GCore.Open(Install, Dll))
			{
				GStatus = GCore.Error;
				BF6Ext::Notify(FString::Printf(TEXT("High Poly: %s"), *GCore.Error));
				return;
			}
		}

		// A PROGRESS DIALOG, AND THE READ OFF THE GAME THREAD.
		//
		// Reading a map is the better part of a minute and it used to run right
		// here, which froze the editor solid with nothing to look at. The core
		// call touches no UObjects, so it can run on a worker while this thread
		// does what only it can: pump Slate and draw the bar.
		//
		// Two phases, weighted by what they actually cost. Reading is roughly
		// fifteen seconds and building the rest, so an even split would make the
		// bar lie in both directions.
		if (FSlateApplication::Get().AnyMenusVisible())
		{
			// Would be refused, and refusing quietly is how this froze before.
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("a menu is still open, so the progress dialog would be refused; not building"));
			BF6Ext::Notify(TEXT("High Poly: close the menu and press build again."));
			return;
		}

		// The first 100 units prepare base geometry. Reserve 20 for the work
		// that used to start after the dialog disappeared at 100%.
		FScopedSlowTask Task(120.f, LOCTEXT("Reading", "High Poly"));
		Task.MakeDialog(true);   // with a cancel button

		const FString Exe = FPaths::Combine(Install, TEXT("bf6.exe"));
		GCore.Progress.bCancel = false;

		TFuture<bool> Read = Async(EAsyncExecution::Thread, [Level, Exe]
		{
			FScopeLock CoreLock(&BF6HP::Shared::CoreMutex());
			return GCore.OpenLevel(Level, Exe);
		});

		// Poll the numbers the core is storing and redraw. EnterProgressFrame(0)
		// advances nothing and repaints, which is what keeps the window alive.
		float Shown = 0.f;
		while (!Read.IsReady())
		{
			FString Stage;
			int32 Done = 0, Total = 0;
			GCore.Progress.Read(Stage, Done, Total);

			const float Frac = Total > 0 ? FMath::Clamp((float)Done / (float)Total, 0.f, 1.f) : 0.f;
			const float Want = Frac * 30.f;          // reading is the first 30%
			Task.EnterProgressFrame(FMath::Max(0.f, Want - Shown),
				FText::FromString(Total > 0
					? FString::Printf(TEXT("%s  %d / %d"), *Stage, Done, Total)
					: Stage));
			Shown = FMath::Max(Shown, Want);

			if (Task.ShouldCancel()) GCore.Progress.Cancel();
			FPlatformProcess::Sleep(0.05f);
		}

		const bool bReadOk = Read.Get();
		// THE MAP CHECK COMES FIRST, before the reader's own verdict is believed.
		// A map switch during the read cancels the core, so the read reports a
		// failure whose message is about a level nobody is looking at any more.
		// Neither the scenery nor the message belongs on the new map.
		if (MapChanged())
		{
			GStatus = TEXT("the map changed while the read was running, so nothing was built");
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("read of %s abandoned: the open map is now %s"),
				*Level, *BF6Ext::CurrentLevel());
			return;
		}
		if (Task.ShouldCancel())
		{
			GStatus = TEXT("stopped while reading the map, so nothing was built");
			BF6Ext::Notify(TEXT("High Poly: stopped. Nothing was built."));
			return;
		}
		if (!bReadOk)
		{
			GStatus = GCore.Error;
			BF6Ext::Notify(FString::Printf(TEXT("High Poly: %s"), *GCore.Error));
			return;
		}
		Task.EnterProgressFrame(FMath::Max(0.f, 30.f - Shown), LOCTEXT("Placing", "Reading placements"));

		TArray<BF6HP::FPlacement> P;
		if (!GCore.Placements(Level, P))
		{
			GStatus = GCore.Error;
			return;
		}
		int32 Meshes = 0, Failed = 0;
		bool bCancelled = false;
		const double T0 = FPlatformTime::Seconds();
		GLastCount = BuildLevelGeometry(P, Meshes, Failed, &Task, bCancelled);
		// A map switch during the build leaves this scenery in the WRONG world,
		// so it goes rather than being described. ClearBuiltScene is the same
		// release the map close would have run had the build not been holding the
		// game thread when it fired.
		if (MapChanged())
		{
			ClearBuiltScene();
			GStatus = TEXT("the map changed while the scene was being built, so it was discarded");
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("build of %s discarded: the open map is now %s"),
				*Level, *BF6Ext::CurrentLevel());
			return;
		}
		BF6HPGameMode::OnArtBuilt();   // the remembered mode's art, at once
		double BuildS = FPlatformTime::Seconds() - T0;
		ApplyLowPoly();
		ApplyMode();
		ApplyWind();
		if (GBenchGroundDebug != INDEX_NONE && GroundMat)
		{
			GroundMat->SetScalarParameterValue(
				TEXT("DebugMode"), (float)GBenchGroundDebug);
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("Tsuru bench: ground debug mode %d applied after build"),
				GBenchGroundDebug);
			GBenchGroundDebug = INDEX_NONE;
		}
		if (GBenchStochastic != INDEX_NONE && GroundMat)
		{
			GroundMat->SetScalarParameterValue(
				TEXT("StochasticTiling"), GBenchStochastic ? 1.f : 0.f);
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("Tsuru bench: stochastic tiling %d applied after build"),
				GBenchStochastic ? 1 : 0);
			GBenchStochastic = INDEX_NONE;
		}
		// Opt-in deterministic bench seam. The sidecar is deliberately launched
		// only after terrain exists, and it runs off the game thread. No flag means
		// no extra work and the current approximation remains the control.
		float BenchDxilSpan = 0.f;
		int32 BenchDxilResolution = 512;
		FParse::Value(FCommandLine::Get(), TEXT("bf6dxilresolution="), BenchDxilResolution);
		const bool bBenchExactPage = Level == TEXT("MP_Isolated")
			&& FParse::Value(FCommandLine::Get(), TEXT("bf6dxilpage="), BenchDxilSpan)
			&& BenchDxilSpan > 0.f;
		if (bBenchExactPage)
		{
			StartExactPage(-778.96216226f, 412.78984774f,
				BenchDxilSpan, BenchDxilResolution);
		}
		else if (GroundMat)
		{
			// A 64 m shipped-DXIL page is a diagnostic A/B, not a terrain layer.
			// Auto-creating it made one camera-centred square look more detailed than
			// the rest of the map and falsely suggested that only one terrain tile had
			// received the decoded material. Normal builds therefore leave it off.
			// MCP generate_exact_terrain_at_camera remains the explicit opt-in path.
			DisableExactPage();
		}
		// "UNREADABLE" WAS THE WRONG WORD, and it read as a decode gap for
		// months.
		//
		// A census over 28 levels that replays the reader's mesh path step by
		// step and classifies every null it returns puts 11,486 of 11,486
		// failed asset groups into ONE class: the graph walk emitted a row for
		// something that has no mesh resource because it is NOT GEOMETRY.
		// Every other way the path can fail is empty across the whole fleet -
		// zero MeshSet parse failures, zero missing geometry chunks, zero
		// sections lost in attribute decode.
		//
		// On MP_Badlands all 294 were named and opened: 89 FX effects, 38
		// prefabs, 9 VisualEnvironment presets, 6 crater makers, 3 dynamic
		// world logic prefabs, and the rest sound, UI, gamemode and layer
		// partitions. One editor marker alone accounted for 419 of the missing
		// placements. Nothing renderable is being lost here, so the number is
		// reported as what it is.
		//
		// AND SAY WHEN IT IS NOT THE WHOLE MAP.
		//
		// The same sentence used to be printed whether the build ran to the end
		// or the creator pressed STOP halfway through it: "12043 of 41220
		// placements" reads as a complete reconstruction that happened to skip
		// most of the map for reasons of its own. Partial output is retained on
		// purpose - a half-built map is worth more than none, and CLEAR removes
		// it in one go - so the only honest fix is for the line to say so.
		if (!bCancelled)
		{
			EnablePlacedAfterBuild();
			float FinalShown=0.f;
			const double Deadline=FPlatformTime::Seconds()+300.0;
			while (true)
			{
				int32 Done=0,Total=0,LoadoutDone=0,LoadoutTotal=0; FString Error;
				const bool PlacedReady=BF6HP::Placed::FinishBuildStep(Done,Total,Error);
				const bool LoadoutReady=PlacedReady&&BF6HP::Loadout::FinishBuildStep(LoadoutDone,LoadoutTotal);
				const float Want=12.f*(Total>0?float(Done)/Total:1.f)
					+3.f*(LoadoutReady?1.f:(LoadoutTotal>0?float(LoadoutDone)/LoadoutTotal:0.f));
				Task.EnterProgressFrame(FMath::Max(0.f,Want-FinalShown),FText::FromString(
					PlacedReady?TEXT("Preparing soldiers, loot and vehicles"):FString::Printf(TEXT("Preparing placed objects and fixture lights: %d / %d"),Done,Total)));
				FinalShown=FMath::Max(FinalShown,Want);
				if(MapChanged()) { ClearBuiltScene(); GStatus=TEXT("Build discarded because the map changed"); return; }
				if(Task.ShouldCancel()||!Error.IsEmpty()||FPlatformTime::Seconds()>Deadline)
				{
					bCancelled=true;
					UE_LOG(LogBF6HighPoly,Warning,TEXT("Build finalization stopped: %s"),Error.IsEmpty()?TEXT("cancelled or timed out"):*Error);
					break;
				}
				if(PlacedReady&&LoadoutReady) break;
				FPlatformProcess::Sleep(0.005f);
			}
			if(!bCancelled)
			{
				Task.EnterProgressFrame(3.f,LOCTEXT("CompileScene","Finishing mesh and material compilation"));
				FAssetCompilingManager::Get().FinishAllCompilation();
				Task.EnterProgressFrame(1.f,LOCTEXT("UploadScene","Finishing scene uploads"));
				FlushRenderingCommands();
				Task.EnterProgressFrame(1.f,LOCTEXT("BuildReady","High Poly ready"));
			}
		}
		BuildS=FPlatformTime::Seconds()-T0;
		GStatus = FString::Printf(TEXT("%s%d of %d placements, %d mesh(es)%s, %.0fs%s"),
			bCancelled ? TEXT("stopped, partial scene: ") : TEXT(""),
			GLastCount, P.Num(), Meshes,
			Failed ? *FString::Printf(TEXT(", %d not geometry"), Failed) : TEXT(""),
			BuildS, bCancelled ? TEXT("; remaining preparation may continue") : TEXT("; scene preparation complete"));
		UE_LOG(LogBF6HighPoly, Log, TEXT("%s: %s"), *Level, *GStatus);
		BF6Ext::Notify(bCancelled
			? FString::Printf(TEXT("High Poly: stopped. What was built is still there; press CLEAR to remove it. %s"), *GStatus)
			: FString::Printf(TEXT("High Poly: %s"), *GStatus));

		// Placed assets and their enabled lighting finished under this dialog.

		// Automation-only companion to -bf6benchtsuru. It exits after the normal
		// read/build path has completed, so CI and decode audits can inspect the
		// same runtime bindings as the interactive editor without killing it.
		if (FParse::Param(FCommandLine::Get(), TEXT("bf6benchexit")))
		{
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([](float)
				{
					FPlatformMisc::RequestExit(false);
					return false;
				}),
				1.0f);
		}
	}

	// ---- pills: the Godot dock's language --------------------------------
	//
	// Categories are rows, choices are pills, and a pill's fill says on. No
	// checkboxes: the dock this mirrors is scanned and prodded, not read.
	const FSlateBrush* PillBrush(bool bOn)
	{
		static FSlateRoundedBoxBrush On (FLinearColor(1.0f, 0.35f, 0.10f), 12.f);
		static FSlateRoundedBoxBrush Off(FLinearColor(0.10f, 0.115f, 0.125f), 12.f,
		                                 FLinearColor(0.28f, 0.31f, 0.33f), 1.f);
		return bOn ? &On : &Off;
	}

	TSharedRef<SWidget> Pill(const FString& Label, TAttribute<bool> On,
	                         TFunction<void()> OnClick, const FString& Hint)
	{
		return SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(0, 0, 6, 6))
			.ToolTipText(FText::FromString(Hint))
			.OnClicked_Lambda([OnClick]{ if (OnClick) OnClick(); return FReply::Handled(); })
			[
				SNew(SBorder)
				.BorderImage_Lambda([On]{ return PillBrush(On.Get(false)); })
				.Padding(FMargin(12.f, 5.f))
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.Text(FText::FromString(Label.ToUpper()))
					.ColorAndOpacity_Lambda([On]{ return FSlateColor(On.Get(false)
						? FLinearColor(0.05f, 0.05f, 0.05f)
						: FLinearColor(0.75f, 0.79f, 0.82f)); })
				]
			];
	}

	TSharedRef<SWidget> CategoryLabel(const FText& Label)
	{
		return SNew(STextBlock).Text(Label)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.46f, 0.51f, 0.54f)));
	}

	// A layer pill acts immediately on what is built, and before a build it is
	// the order form: only what is on gets read and built at all.
	TSharedRef<SWidget> LayerPill(ELayer L)
	{
		const int32 i = (int32)L;
		return Pill(GLayers[i].Name,
			TAttribute<bool>::CreateLambda([i]{ return GLayers[i].bOn; }),
			[i, L]
			{
				GLayers[i].bOn = !GLayers[i].bOn;
				ApplyLayer(L);
			},
			GLayers[i].Hint);
	}

	TSharedRef<SWidget> LayerPills()
	{
		TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true);
		for (int32 i = 0; i < (int32)ELayer::Count; i++)
		{
			Box->AddSlot()[ LayerPill((ELayer)i) ];
		}
		return Box;
	}

	// ONE PILL PER GAME MODE THE LEVEL ACTUALLY SHIPS.
	//
	// Not a fixed list, because the modes differ per map and a hard-coded set
	// would be wrong on the next one. The counts are on the pill because they
	// are the whole argument: a mode carrying fifteen thousand placements is a
	// different proposition from one carrying four, and a creator cannot judge
	// that without the number.
	TSharedRef<SWidget> GameModePills()
	{
		TSharedRef<SWrapBox> Box = SNew(SWrapBox).UseAllottedSize(true);
		if (GModesFound.Num() == 0)
		{
			Box->AddSlot()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("NoModesYet",
					"Build once and the modes this map ships will appear here."))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.46f, 0.51f, 0.54f)))
			];
			return Box;
		}
		// "Largest" first: it is the default, and it is what a player sees.
		Box->AddSlot()
		[ Pill(TEXT("Largest"),
			TAttribute<bool>::CreateLambda([]{ return GGameMode.IsEmpty(); }),
			[]{ GGameMode.Reset(); BF6HPGameMode::SetMode(FString()); },
			TEXT("Build the mode with the most placements, which is the map's headline mode. Rebuild to apply.")) ]; 
		for (const TPair<FString, int32>& M : GModesFound)
		{
			const FString Name = M.Key;
			Box->AddSlot()
			[ Pill(FString::Printf(TEXT("%s (%d)"), *Name, M.Value),
				TAttribute<bool>::CreateLambda([Name]
					{ return GGameMode.Equals(Name, ESearchCase::IgnoreCase); }),
				[Name]{ GGameMode = Name; BF6HPGameMode::SetMode(Name); },
				FString::Printf(TEXT("Build only this mode's props: %d placement(s). ")
					TEXT("Rebuild to apply."), M.Value)) ];
		}
		Box->AddSlot()
		[ Pill(TEXT("All"),
			TAttribute<bool>::CreateLambda([]
				{ return GGameMode.Equals(TEXT("all"), ESearchCase::IgnoreCase); }),
			[]{ GGameMode = TEXT("all"); BF6HPGameMode::SetMode(TEXT("all")); },
			TEXT("Build every mode at once. They are ALTERNATIVE layouts of the same ")
			TEXT("ground, so this stacks them - four carriers where the game shows one.")) ];
		return Box;
	}

	TSharedRef<SWidget> ModePill(EMode M, const FString& Label, const FString& Hint)
	{
		return Pill(Label,
			TAttribute<bool>::CreateLambda([M]{ return GMode == M; }),
			[M]{ GMode = M; ApplyMode(); },
			Hint);
	}

	// First use: the add-on will not read anything until the creator has said
	// where the game is. Deliberately a wall rather than a hint - the point is
	// that nobody ends up reading game data without having chosen to.
	TSharedRef<SWidget> MakeSetupPanel()
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Menu.Background"))
			.Padding(FMargin(16.f, 14.f))
			[
				SNew(SBox).WidthOverride(440.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[ SNew(STextBlock).Text(LOCTEXT("SetupTitle", "HIGH POLY"))
						.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.35f, 0.10f))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[ SNew(STextBlock)
						.Text(LOCTEXT("SetupBody", "High Poly builds from your own installed copy of Battlefield 6. Point it at the game folder once and it will remember."))
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.79f, 0.82f))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
					[ SNew(STextBlock)
						.Text(LOCTEXT("SetupHint", "Pick the folder that contains bf6.exe. Nothing is uploaded, nothing is modified, and the game is only ever read."))
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor(FLinearColor(0.46f, 0.51f, 0.54f))) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SButton)
						.ContentPadding(FMargin(12.f, 6.f))
						.OnClicked_Lambda([]
						{
							FString Picked;
							if (BrowseForInstall(Picked))
							{
								GInstall = Picked;
								BF6Ext::Notify(TEXT("Battlefield 6 found. Open High Poly again to build."));
							}
							return FReply::Handled();
						})
						[ SNew(STextBlock).Text(LOCTEXT("Browse", "CHOOSE THE BATTLEFIELD 6 FOLDER")) ]
					]
				]
			];
	}

	// The status panel, which for now is what the pill opens. It reports the
	// three things that decide whether anything can be built: an install to read
	// from, a map to build onto, and a cache to build into.
	// ONE LIST, TWO SHAPES.
	//
	// The controls used to be built inline where the ring was opened, so the panel
	// could only ever be a second, hand-maintained copy of them, and the two would
	// drift the first time a module added a switch. They are built once here,
	// grouped, and the ring and the panel are two renderings of the same thing: a
	// module that adds an entry gets it in both without knowing either exists.
	TArray<FControlSection> BuildSections(FVector2D Center)
	{
		TArray<FControlSection> Out;
		auto Section = [&Out](const TCHAR* Name) -> TArray<FControl>&
		{
			Out.Add(FControlSection{ Name, {} });
			return Out.Last().Entries;
		};

		// ---- what the scenery looks like ---------------------------------
		//
		// NO "LOOK" HEADING. This section held one dropdown, so the heading was
		// a collapsible box around a single control, and the control it hid is
		// the one people came to change. The dropdown is rendered directly
		// under the BUILD button instead; the section name is kept as the key
		// the panel matches on and never drawn.
		{
			TArray<FControl>& R = Section(TEXT("Mode"));

			// ONE CHOICE, NOT THREE BUTTONS. The three modes are mutually
			// exclusive, which is what a dropdown says and three buttons do
			// not: with buttons nothing on screen tells you which one you are
			// in except a status line you have to read.
			FControl Mode;
			Mode.Kind = FControl::EKind::Choice;
			Mode.Label = TEXT("MODE");
			Mode.Tip = TEXT("How the borrowed scenery is drawn. Cheapest first.");
			Mode.Choices = { TEXT("Low poly"), TEXT("Clay"), TEXT("Textured") };
			Mode.GetChoice = []{ return (int32)GMode; };
			Mode.SetChoice = [](int32 I){ GMode = (EMode)FMath::Clamp(I, 0, 2); ApplyMode(); };
			Mode.Sub = []
			{
				switch (GMode)
				{
				case EMode::LowPoly:  return FString(TEXT("just your map"));
				case EMode::Clay:     return FString(TEXT("study grey"));
				default:              return FString(TEXT("the full thing"));
				}
			};
			R.Add(MoveTemp(Mode));

			// No LOW-POLY MAP switch: see HideLowPolyNow. The blockout goes
			// when the build has replaced it and comes back from the scene
			// tree, which is where hiding an actor belongs.
		}

		// ---- which layers are drawn --------------------------------------
		{
			TArray<FControl>& R = Section(TEXT("Layers"));
			auto Layer = [&R](ELayer L)
			{
				const int32 i = (int32)L;
				FControl E;
				E.Kind = FControl::EKind::Toggle;
				E.Label = GLayers[i].Name;
				E.Get = [i]{ return GLayers[i].bOn; };
				E.Set = [i, L](bool b){ GLayers[i].bOn = b; ApplyLayer(L); };
				E.Sub = [i]{ return FString(GLayers[i].bOn ? TEXT("on") : TEXT("off")); };
				R.Add(MoveTemp(E));
			};
			Layer(ELayer::Terrain);
			Layer(ELayer::Roads);
			Layer(ELayer::Objects);
			Layer(ELayer::Water);
			// LIGHTS AS A LAYER. Every local light the add-on owns - the map's
			// own lamps and the fixtures on placed objects together - drawn or
			// not drawn like anything else in this list.
			{
				FControl E;
				E.Kind = FControl::EKind::Toggle;
				E.Label = TEXT("LIGHTS");
				E.Tip = TEXT("Every light this add-on owns: the map's lamps and the fixtures on placed objects.");
				E.Get = []{ return BF6HP::Placed::LightsOn(); };
				E.Set = [](bool b){ BF6HP::Placed::SetLightsOn(b); };
				E.Sub = []{ return FString(BF6HP::Placed::LightsOn() ? TEXT("on") : TEXT("off")); };
				R.Add(MoveTemp(E));
			}
		}

		// ---- the game mode being previewed -------------------------------
		{
			TArray<FControl>& R = Section(TEXT("Game mode"));
			FControl E;
			// A CHOICE, NOT A DOOR BACK TO THE RADIAL. This opened the ring,
			// from inside the panel that exists so the ring is not needed.
			E.Kind = FControl::EKind::Choice;
			E.Label = TEXT("GAME MODE");
			E.Tip = TEXT("Which mode's objectives and spawns are shown on this map.");
			E.Choices = BF6HPGameMode::ModeLabels();
			E.GetChoice = []{ return BF6HPGameMode::CurrentModeIndex(); };
			E.SetChoice = [](int32 I){ BF6HPGameMode::SetModeByIndex(I); };
			E.Sub = []
			{
				const FString M = BF6HPGameMode::CurrentMode();
				return M.IsEmpty() ? FString(TEXT("largest")) : BF6HPGameMode::Pretty(M).ToLower();
			};
			R.Add(MoveTemp(E));
		}

		// ---- building it -------------------------------------------------
		//
		// THERE IS NO BUILD SECTION ANY MORE.
		//
		// BUILD is the big button at the top of the panel, and a second one
		// further down was the same press with a different tooltip: two ways to
		// start the same work, one of which people found first and the other of
		// which they pressed when the first appeared not to work.
		//
		// NANITE has gone with it. It stays ON: it is what makes the borrowed
		// scenery affordable at all, the threshold below which it is skipped is
		// already automatic, and a switch whose only honest setting is the one
		// it ships with is a switch that can only be turned the wrong way.
		// GNanite survives as a build-time constant.
		//
		// The entry itself stays, because the RING renders this same list and
		// has no big button of its own. The panel skips it by name.
		{
			TArray<FControl>& R = Section(TEXT("Build"));
			FControl E;
			E.Kind = FControl::EKind::Action;
			E.Label = TEXT("BUILD");
			E.Tip = TEXT("Read the scenery for this map out of your Battlefield install.");
			E.Sub = []{ return FString(GBuiltAnything
				? TEXT("rebuild from the game") : TEXT("read your install")); };
			// The same press as the big button: read the map, then switch the
			// props on and fill in the pictures. Two entry points, one action.
			E.OnAct = []{ BuildAndShow(); };
			E.bCloses = true;
			R.Add(MoveTemp(E));
		}

		// ---- shaders ------------------------------------------------------
		// WIND is not a build setting: it changes what the foliage shader does
		// with scenery that is already standing there. It sat under Build
		// because that is where it was written, not because it belongs there.
		{
			TArray<FControl>& R = Section(TEXT("Shaders"));
			{
				FControl E;
				E.Kind = FControl::EKind::Toggle;
				E.Label = TEXT("WIND");
				E.Tip = TEXT("Sway the foliage. Takes effect immediately: it is the shader, not the build.");
				E.Get = []{ return GWind; };
				E.Set = [](bool b){ GWind = b; ApplyWind(); };
				E.Sub = []{ return FString(GWind ? TEXT("swaying") : TEXT("still")); };
				R.Add(MoveTemp(E));
			}
		}

		// ---- how far the real models are drawn ---------------------------
		//
		// Pulled out of the placed-object section and given a key of its own so
		// the panel can lift it above the BUILD button. It governs the whole
		// view rather than one feature, and it is the first thing to reach for
		// when the editor is struggling.
		{
			TArray<FControl>& R = Section(TEXT("Distance"));
			BF6HP::Placed::AddDistanceControls(R);
		}

		// ---- the placed-object modules -----------------------------------
		{
			TArray<FControl>& R = Section(TEXT("Previews"));
			BF6HP::Placed::AddControls(R);
			BF6HPPreviews::AddControls(R);
		}

		// ---- water --------------------------------------------------------
		{
			TArray<FControl>& R = Section(TEXT("Water"));
			FControl E;
			E.Kind = FControl::EKind::Action;
			E.Label = TEXT("WATER LAB");
			E.Tip = TEXT("Waves, sea state and masks, in their own window.");
			E.Sub = []{ return FString(TEXT("waves, sea state and masks")); };
			E.OnAct = []{ BF6WaterLab::Start(); };
			E.bCloses = true;
			R.Add(MoveTemp(E));
		}

		return Out;
	}

	// The four interaction strengths, built once. A Slate button style holds
	// brushes by value but the panel outlives the call that made it, so this is
	// a file static rather than a local.
	FButtonStyle GRowStyle;
	FCheckBoxStyle GChipStyle;
	bool GRowStyleBuilt = false;
	void BuildRowStyle()
	{
		if (GRowStyleBuilt) { return; }
		GRowStyleBuilt = true;
		using EM = BF6HPTheme::EMask;
		GRowStyle = FButtonStyle()
			.SetNormal(*BF6HPTheme::Mask(EM::Rest))
			.SetHovered(*BF6HPTheme::Mask(EM::Hover))
			.SetPressed(*BF6HPTheme::Mask(EM::Press))
			.SetDisabled(*BF6HPTheme::Mask(EM::Disabled))
			.SetNormalPadding(FMargin(0))
			.SetPressedPadding(FMargin(0));

		// THE CHIP. Godot draws a toggle button's "pressed" box for as long as
		// it is switched on, so that state gets the accent and every other one
		// wears the mask. Built as a check box rather than a button because the
		// on state has to be readable at a glance without pressing anything.
		GChipStyle = FCheckBoxStyle()
			.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
			.SetPadding(FMargin(8.f, 4.f))
			.SetUncheckedImage(*BF6HPTheme::Mask(EM::ChipOff))
			.SetUncheckedHoveredImage(*BF6HPTheme::Mask(EM::ChipHover))
			.SetUncheckedPressedImage(*BF6HPTheme::Mask(EM::ChipHover))
			.SetCheckedImage(*BF6HPTheme::Mask(EM::ChipOn))
			.SetCheckedHoveredImage(*BF6HPTheme::Mask(EM::ChipOnHover))
			.SetCheckedPressedImage(*BF6HPTheme::Mask(EM::ChipOnHover));
	}

	// ---- THE CONTROL PANEL --------------------------------------------------
	//
	// One screen for the add-on, sectioned, instead of a ring you had to hold
	// open and read one pill at a time. The ring is a good way to flip a switch
	// you already know about and a poor way to find out what the add-on is
	// currently doing: which install it is reading, which map, whether the last
	// build worked, what is off and why. Those answers were spread across
	// several pills and a separate status popup.
	//
	// Every row here is a BF6Ext::FPieSubEntry from BuildSections, so the panel
	// cannot list a control the ring does not have, or run it differently. Its
	// Sub is read on every paint, so a build finishing or a worker failing shows
	// up without anybody refreshing anything.
	TSharedRef<SWidget> MakeControlPanel(const FString& Install, FVector2D Center)
	{
		// The season palette, from theme.json, the same file the Godot plugin
		// reads. The first version of this panel used the editor's grey with an
		// invented orange accent and matched nothing the user had made.
		BuildRowStyle();
		const FLinearColor Accent = BF6HPTheme::Accent();
		const FLinearColor Text   = BF6HPTheme::Heading();
		const FLinearColor Dim    = Text * 0.62f;

		auto Fact = [Dim, Text](const FString& Label, TFunction<FString()> Value) -> TSharedRef<SWidget>
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
				[ SNew(SBox).WidthOverride(96.f)
					[ SNew(STextBlock).Text(FText::FromString(Label.ToUpper())).ColorAndOpacity(FSlateColor(Dim)) ] ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[ SNew(STextBlock)
					// A path keeps its case; everything the tool wrote itself
					// is said the way the rest of the panel says things.
					.Text_Lambda([Value]
					{
						const FString V = Value();
						return FText::FromString(V.Contains(TEXT("\\")) || V.Contains(TEXT("/"))
							? V : V.ToUpper());
					})
					.ColorAndOpacity(FSlateColor(Text))
					.AutoWrapText(true) ];
		};

		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

		// WHAT IT IS READING, first and unmissable. A creator should never have
		// to wonder whether they are looking at their own copy of the game.
		Body->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(10.f, 8.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[ Fact(TEXT("Reading"), [Install]{ return Install; }) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[ Fact(TEXT("Map"), []
					{
						const FString L = BF6Ext::CurrentLevel();
						return L.IsEmpty() ? FString(TEXT("no map open")) : L;
					}) ]
				+ SVerticalBox::Slot().AutoHeight()
				[ Fact(TEXT("Last build"), []
					{
						return GStatus.IsEmpty() ? FString(TEXT("nothing built yet")) : GStatus;
					}) ]
			]
		];

		// The MODE dropdown is lifted out of the list and drawn under the BUILD
		// button, where the thing it changes is.
		TSharedPtr<SVerticalBox> ModeRows;
		TSharedPtr<SVerticalBox> DistanceRows;

		for (const FControlSection& S : BuildSections(Center))
		{
			if (S.Entries.Num() == 0) { continue; }
			TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
			for (const FControl& E : S.Entries)
			{
				// Copied by value: the panel outlives the section list, and a
				// row holding a reference into a freed array is the sort of
				// crash that only shows up on somebody else's machine.
				const FControl Row = E;
				const FText Tip = FText::FromString(Row.Tip);

				// The label and the live status line are the same on every kind
				// of control, so only the control itself differs below.
				TSharedRef<SWidget> Name =
					SNew(SBox).WidthOverride(148.f).VAlign(VAlign_Center)
					[ SNew(STextBlock).Text(FText::FromString(Row.Label))
						.Font(BF6HPTheme::Font(10))
						.ColorAndOpacity(FSlateColor(FLinearColor::White)) ];
				TSharedRef<SWidget> Status =
					SNew(STextBlock)
					// Read every paint, so a build finishing or a worker failing
					// appears here on its own.
					//
					// UPPERCASED HERE RATHER THAN AT EVERY CALL SITE. The status
					// lines are written as sentences by a dozen modules and were
					// landing beside labels that shout, so the panel read as
					// half one thing and half another. One place to say it means
					// a module added tomorrow is in the same voice as the rest.
					.Text_Lambda([Row]{ return FText::FromString(Row.Sub ? Row.Sub().ToUpper() : FString()); })
					.Font(BF6HPTheme::Font(9))
					.ColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.62f)))
					.AutoWrapText(true);

				TSharedPtr<SWidget> Widget;
				switch (Row.Kind)
				{
				case FControl::EKind::Toggle:
				{
					// A CHIP, the way the Godot theme does it: the mask when
					// off, the accent when on. This is the one place the accent
					// is spent on a control, because "this is on right now" is
					// worth seeing without reading anything.
					Widget = SNew(SCheckBox)
						.Style(&GChipStyle)
						.ToolTipText(Tip)
						.IsChecked_Lambda([Row]
						{
							return (Row.Get && Row.Get()) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						})
						.OnCheckStateChanged_Lambda([Row](ECheckBoxState S2)
						{
							if (Row.Set) { Row.Set(S2 == ECheckBoxState::Checked); }
						})
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)[ Name ]
							+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[ Status ]
						];
					break;
				}
				case FControl::EKind::Choice:
				{
					// A DROPDOWN, not one button per option and not a door back
					// to the radial. Mutually exclusive options are what a
					// dropdown is for, and it shows the current one without the
					// user having to read a status line to find out.
					TSharedRef<TArray<TSharedPtr<FString>>> Items =
						MakeShared<TArray<TSharedPtr<FString>>>();
					for (const FString& C : Row.Choices) { Items->Add(MakeShared<FString>(C)); }

					Widget = SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)[ Name ]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SComboBox<TSharedPtr<FString>>)
							.OptionsSource(&(*Items))
							.ToolTipText(Tip)
							.OnGenerateWidget_Lambda([](TSharedPtr<FString> In)
							{
								return SNew(STextBlock).Text(FText::FromString(In.IsValid() ? In->ToUpper() : FString()))
									.Font(BF6HPTheme::Font(10));
							})
							.OnSelectionChanged_Lambda([Row, Items](TSharedPtr<FString> In, ESelectInfo::Type)
							{
								if (!In.IsValid() || !Row.SetChoice) { return; }
								const int32 At = Items->IndexOfByPredicate(
									[&In](const TSharedPtr<FString>& P){ return P == In; });
								if (At != INDEX_NONE) { Row.SetChoice(At); }
							})
							[
								SNew(STextBlock)
								// Read live, so a mode changed from the radial
								// or a console command shows here too.
								.Text_Lambda([Row]
								{
									const int32 At = Row.GetChoice ? Row.GetChoice() : 0;
									return FText::FromString(Row.Choices.IsValidIndex(At)
										? Row.Choices[At].ToUpper() : FString(TEXT("?")));
								})
								.Font(BF6HPTheme::Font(10))
								.ColorAndOpacity(FSlateColor(FLinearColor::White))
							]
						];
					break;
				}
				case FControl::EKind::Slider:
				{
					Widget = SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)[ Name ]
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(SSlider)
							.ToolTipText(Tip)
							.Value_Lambda([Row]
							{
								const float V = Row.GetValue ? Row.GetValue() : 0.f;
								return Row.Max > Row.Min ? (V - Row.Min) / (Row.Max - Row.Min) : 0.f;
							})
							.OnValueChanged_Lambda([Row](float N)
							{
								if (Row.SetValue) { Row.SetValue(Row.Min + N * (Row.Max - Row.Min)); }
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
						[ Status ];
					break;
				}
				default:
				{
					// Only something that actually happens is a button.
					Widget = SNew(SButton)
						.ButtonStyle(&GRowStyle)
						.ToolTipText(Tip)
						.ContentPadding(FMargin(8.f, 5.f))
						.OnClicked_Lambda([Row]
						{
							if (Row.OnAct) { Row.OnAct(); }
							return FReply::Handled();
						})
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)[ Name ]
							+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[ Status ]
						];
					break;
				}
				}
				Rows->AddSlot().AutoHeight().Padding(0, 1)[ Widget.ToSharedRef() ];
			}
			// MODE belongs under the BUILD button, not in the list.
			if (FString(S.Name).Equals(TEXT("Mode"), ESearchCase::IgnoreCase))
			{
				ModeRows = Rows;
				continue;
			}
			// AND THE DETAIL DISTANCE BELONGS ABOVE IT.
			//
			// It is not a property of a build the way the mode is - it governs
			// how much of whatever is built gets drawn, at any time, and it is
			// the first thing to reach for on a machine that is struggling.
			// Buried in a collapsible section under the placed objects it read
			// as a setting for one feature rather than the whole view.
			if (FString(S.Name).Equals(TEXT("Distance"), ESearchCase::IgnoreCase))
			{
				DistanceRows = Rows;
				continue;
			}
			// And BUILD is already the big button at the top. The entry exists
			// for the ring, which has no big button.
			if (FString(S.Name).Equals(TEXT("Build"), ESearchCase::IgnoreCase)) { continue; }

			// A CATEGORY OF ONE IS NOT A CATEGORY.
			//
			// A collapsible heading earns its place by holding several related
			// switches; wrapped around a single control it only hides that
			// control behind a click and adds a word nobody needed to read.
			// Game mode, wind and the water lab are each one thing, so each is
			// drawn as itself.
			if (S.Entries.Num() == 1)
			{
				Body->AddSlot().AutoHeight().Padding(0, 6, 0, 2)[ Rows ];
				continue;
			}

			// THE GODOT SECTION HEADER: a centred title over a line that fades
			// out at both ends, warming to the accent on hover and staying lit
			// while the section is open. This was a small left-aligned accent
			// label, which is the plainest possible reading of "heading" and
			// looks nothing like the plugin it is standing in for.
			//
			// Collapsible for the same reason Godot makes them collapsible: the
			// panel carries more sections than fit on a screen, and every one of
			// them expanded is how the controls at the bottom stop being found.
			Body->AddSlot().AutoHeight().Padding(0, 6, 0, 2)
			[
				SNew(SBF6HighPolySection)
				.Title(FString(S.Name).ToUpper())
				.Description(FString(S.Name).ToUpper())
				// Layers is open by default: it is the list people come back to.
				.InitiallyOpen(FString(S.Name).Equals(TEXT("Layers"), ESearchCase::IgnoreCase))
				[
					Rows
				]
			];
		}

		// Setup and housekeeping last: needed rarely, and destructive if pressed
		// by accident, so it is not sitting next to the switches.
		Body->AddSlot().AutoHeight().Padding(0, 10, 0, 2)
		[ SNew(STextBlock).Text(LOCTEXT("SetupHead", "SETUP AND CACHE")).ColorAndOpacity(FSlateColor(Dim)) ];
		Body->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[ Fact(TEXT("Cache"), []{ return CacheDir(); }) ];
		Body->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.f, 5.f))
				.ToolTipText(LOCTEXT("ChangeTip", "Point the add-on at a different Battlefield install."))
				.OnClicked_Lambda([]
				{
					// The same two steps the setup panel takes, so choosing a
					// folder means one thing wherever it is done from.
					FString Picked;
					if (BrowseForInstall(Picked))
					{
						GInstall = Picked;
						BF6Ext::Notify(TEXT("High Poly will read from the new folder."));
					}
					return FReply::Handled();
				})
				[ SNew(STextBlock).Text(LOCTEXT("Change2", "USE A DIFFERENT FOLDER")) ]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.f, 5.f))
				.ToolTipText(LOCTEXT("ClearTip", "Remove the high poly scenery from this map. Your own map is untouched."))
				.OnClicked_Lambda([]{ ClearBuiltScene(); return FReply::Handled(); })
				[ SNew(STextBlock).Text(LOCTEXT("Clear2", "CLEAR THE SCENERY")) ]
			]
		];

		// THE PANEL'S SHAPE, from highpoly_theme.gd: an accent outline around an
		// opaque splash floor. The Godot side leaves the centre undrawn because
		// a video loops behind it; there is no video here, so the floor is the
		// splash colour and the outline is the same two pixels of accent.
		//
		// Two nested borders rather than a custom nine-slice: the outer one is
		// the accent, and two pixels of padding let it show as the outline.
		// The entrance plays over the backdrop and then gets out of the way,
		// leaving the waves looping under the panel. With no artwork installed
		// the splash reports itself unavailable and the panel is built without
		// it, so the add-on still works exactly as before.
		return SNew(SBorder)
			.BorderImage(BF6HPTheme::Solid(Accent))
			.Padding(FMargin(2.f))
			[
				SNew(SBF6HighPolySplash)
				[
				SNew(SBorder)
				// Transparent: the waves are the floor now, and a solid panel
				// here would cover the very thing it is meant to sit on.
				.BorderImage(BF6HPTheme::Solid(FLinearColor(0.f, 0.f, 0.f, 0.f)))
				.Padding(FMargin(16.f, 14.f))
				[
					// FILLS THE WINDOW. These were a fixed 460x720 because the
					// panel used to be a popup, which has no size of its own.
					// A window does, and the user sets it - docking it to a
					// screen edge sets it too - so a hard size here would leave
					// the panel floating in the wrong shape inside its own frame.
					SNew(SBox).MinDesiredWidth(360.f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
							[ SNew(STextBlock).Text(LOCTEXT("PanelTitle", "HIGH POLY"))
								.Font(BF6HPTheme::Font(20))
								.ColorAndOpacity(FSlateColor(Accent)) ]
							// The season the palette came from. It is the one
							// line that explains why the panel is this colour,
							// and it costs nothing to say.
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
							[ SNew(STextBlock)
								.Text(FText::FromString(BF6HPTheme::SeasonName()))
								.Font(BF6HPTheme::Font(9))
								.ColorAndOpacity(FSlateColor(Dim)) ]
							// THE DETAIL DISTANCE, ABOVE THE BUILD BUTTON.
							//
							// It applies to whatever is on screen rather than to
							// the next build, so it sits above the button rather
							// than under it with MODE.
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
							[
								DistanceRows.IsValid()
									? StaticCastSharedRef<SWidget>(DistanceRows.ToSharedRef())
									: SNullWidget::NullWidget
							]
							// THE BUILD BUTTON, AT THE TOP AND BIG.
							//
							// It is the one thing in this panel somebody opens
							// it to press, and it was a normal-sized row inside
							// a collapsible section several scrolls down - which
							// is a strange place for the button the whole tool
							// exists to offer. Everything else here adjusts what
							// a build produces; this is the build.
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
							[
								SNew(SButton)
								.ButtonStyle(&GRowStyle)
								.ContentPadding(FMargin(14.f, 12.f))
								.HAlign(HAlign_Center)
								.ToolTipText(LOCTEXT("BigBuildTip",
									"Read the scenery for this map out of your Battlefield install, "
									"and switch to a high-poly mode when it is done."))
								.OnClicked_Lambda([]
								{
									BuildAndShow();
									return FReply::Handled();
								})
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
									[
										SNew(STextBlock)
										.Text(LOCTEXT("BigBuild", "BUILD"))
										.Font(BF6HPTheme::Font(18))
										.ColorAndOpacity(FSlateColor(FLinearColor::White))
									]
									+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
									[
										SNew(STextBlock)
										.Text_Lambda([]
										{
											return FText::FromString(GBuiltAnything
												? TEXT("REBUILD FROM THE GAME")
												: TEXT("READ YOUR INSTALL"));
										})
										.Font(BF6HPTheme::Font(9))
										.ColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.62f)))
									]
								]
							]
							// The one control that decides what BUILD produces,
							// directly under it. It was a dropdown inside a
							// collapsible box called LOOK, several rows away
							// from the button it governs.
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
							[
								ModeRows.IsValid() ? StaticCastSharedRef<SWidget>(ModeRows.ToSharedRef())
								                   : SNullWidget::NullWidget
							]
							+ SVerticalBox::Slot().AutoHeight()[ Body ]
						]
					]
				]
				]
			];
	}

	TSharedRef<SWidget> MakeStatusPanel(const FString& Install)
	{
		const FString Level = BF6Ext::CurrentLevel();

		auto Row = [](const FString& Label, const FString& Value, bool bGood) -> TSharedRef<SWidget>
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
				[ SNew(SBox).WidthOverride(120.f)
					[ SNew(STextBlock).Text(FText::FromString(Label))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.46f, 0.51f, 0.54f))) ] ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[ SNew(STextBlock).Text(FText::FromString(Value))
					.ColorAndOpacity(FSlateColor(bGood ? FLinearColor(0.75f, 0.79f, 0.82f)
					                                   : FLinearColor(1.0f, 0.55f, 0.20f))) ];
		};

		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Menu.Background"))
			.Padding(FMargin(16.f, 14.f))
			[
				SNew(SBox).WidthOverride(420.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[ SNew(STextBlock).Text(LOCTEXT("Title", "HIGH POLY"))
						.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.35f, 0.10f))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						// THE PROVENANCE LINE. It is here, above everything
						// else and in the accent colour, because a creator
						// should never be unsure whether they are looking at
						// their own copy of the game's data.
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.Padding(FMargin(10.f, 8.f))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[ SNew(STextBlock)
								.Text(LOCTEXT("Detected", "Battlefield 6 detected. This reads your own installed copy."))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.82f, 0.55f))) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
							[ SNew(STextBlock).Text(FText::FromString(Install))
								.AutoWrapText(true)
								.ColorAndOpacity(FSlateColor(FLinearColor(0.46f, 0.51f, 0.54f))) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
							[
								SNew(SButton)
								.ContentPadding(FMargin(8.f, 2.f))
								.OnClicked_Lambda([]
								{
									FString Picked;
									if (BrowseForInstall(Picked)) { GInstall = Picked; BF6Ext::Notify(TEXT("High Poly will read from the new folder.")); }
									return FReply::Handled();
								})
								[ SNew(STextBlock).Text(LOCTEXT("Change", "Use a different folder")) ]
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[ Row(TEXT("Map open"),
						  Level.IsEmpty() ? TEXT("none") : Level,
						  !Level.IsEmpty()) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[ Row(TEXT("Cache"), CacheDir(), true) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[ Row(TEXT("Last read"), GStatus.IsEmpty() ? TEXT("nothing yet") : GStatus, GBuiltAnything) ]

					// ---- mode, layers and options, all pills --------------
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
					[ CategoryLabel(LOCTEXT("Mode", "MODE")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SWrapBox).UseAllottedSize(true)
						+ SWrapBox::Slot()[ ModePill(EMode::LowPoly, TEXT("Low-Poly"),
							TEXT("Just your own map, the way it exports. Everything the add-on built hides.")) ]
						+ SWrapBox::Slot()[ ModePill(EMode::Clay, TEXT("Clay"),
							TEXT("The real level in study grey. Shapes and sightlines without the noise of textures.")) ]
						+ SWrapBox::Slot()[ ModePill(EMode::Textured, TEXT("Textured"),
							TEXT("The full thing: real geometry, real materials, real water.")) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 4)
					[ CategoryLabel(LOCTEXT("Layers", "LAYERS")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						// BUILT FROM THE ENUM, not listed by hand. These four
						// used to be written out one per line, so adding a
						// fifth layer to ELayer and to GLayers produced a layer
						// that built, toggled and logged correctly and had no
						// pill - which looks exactly like the feature not
						// existing. A loop cannot forget one.
						LayerPills()
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 4)
					[ CategoryLabel(LOCTEXT("GameMode", "GAME MODE")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[ GameModePills() ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 4)
					[ CategoryLabel(LOCTEXT("Options", "OPTIONS")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[
						SNew(SWrapBox).UseAllottedSize(true)
						+ SWrapBox::Slot()
						[ Pill(TEXT("Nanite"),
							TAttribute<bool>::CreateLambda([]{ return GNanite; }),
							[]{ GNanite = !GNanite; },
							TEXT("Build the geometry as Nanite: slower build, far better frame rate. Takes effect on the next build.")) ]
						+ SWrapBox::Slot()
						[ Pill(TEXT("Wind"),
							TAttribute<bool>::CreateLambda([]{ return GWind; }),
							[]{ GWind = !GWind; ApplyWind(); },
							TEXT("Foliage sways. A world-position-offset branch on the vegetation material - at rest it costs exactly nothing.")) ]
						+ SWrapBox::Slot()
						[ Pill(TEXT("Low-Poly Map"),
							TAttribute<bool>::CreateLambda([]{ return !HideLowPolyNow(); }),
							[]{ ApplyMode(); },
							TEXT("Keep the tool's own low-poly map visible under the real one.")) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
						[
							SNew(SButton)
							.ContentPadding(FMargin(12.f, 6.f))
							.IsEnabled_Lambda([]{ return !BF6Ext::CurrentLevel().IsEmpty(); })
							.OnClicked_Lambda([]{ StartRead(); return FReply::Handled(); })
							[ SNew(STextBlock).Text_Lambda([]
								{ return GBuiltAnything ? LOCTEXT("Rebuild", "REBUILD") : LOCTEXT("Read", "BUILD FROM THE GAME"); }) ]
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.ContentPadding(FMargin(12.f, 6.f))
							.IsEnabled_Lambda([]{ return GBuiltAnything; })
							.OnClicked_Lambda([]
							{
								// The full per-map release, not just the actors: this
								// used to destroy the scenery and leave the ocean
								// simulating into rooted textures nothing could reach
								// any more. ClearBuiltScene also puts the low-poly map
								// back, so no ApplyLowPoly call belongs here.
								ClearBuiltScene();
								GStatus = TEXT("cleared");
								return FReply::Handled();
							})
							[ SNew(STextBlock).Text(LOCTEXT("Clear", "CLEAR")) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock)
						.Text(LOCTEXT("Soon", "Materials, water, roads and lighting are still being ported. Layers appear here as they land."))
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor(FLinearColor(0.46f, 0.51f, 0.54f))) ]
				]
			];
	}


}

// A material that will not compile draws as grey, and the only place that says
// so is the log. This builds the ground material on demand and reports the
// verdict, so the question can be settled without a map, an install, or a human
// looking at a viewport:
//
//     UnrealEditor-Cmd.exe <uproject> -ExecCmds="BF6.HighPoly.CheckMaterials, quit"
//                          -unattended -nosplash -stdout
// LIVE TUNING, so a number can be found by looking rather than by rebuilding.
//
// Several of these values are CALIBRATIONS rather than decoded quantities - the
// water clarity depth, the sun's editor intensity, the light lumen ceiling -
// and the only way to settle a calibration is to look at it. Each rebuild
// round trip costs minutes and the user's attention, so they are exposed as
// console commands that walk the built material instances and set the
// parameter in place.
//
//   BF6.HighPoly.WaterClarity 400     metres of depth before water is opaque
//   BF6.HighPoly.SunIntensity 8       the directional light, directly
//
// WaterClarity is also the DIAGNOSTIC that settles whether the clarity ramp
// works at all. Unreal's own water shading is
//     Transmittance = exp(-ExtinctionCoeff * WaterVolumeDepth)
// so driving the coefficients toward zero MUST make the surface clear. Set it
// to something enormous: if the water goes transparent the ramp is wired and
// only its distance is wrong, and if it does not then the coefficients are not
// what is holding the water opaque and the fault is elsewhere - most likely
// that nothing opaque is being rendered behind the surface for it to show.
static void BF6_ForEachWaterMaterial(TFunctionRef<void(UMaterialInstanceDynamic*)> Fn)
{
    if (!GEditor) return;
    UWorld* W = GEditor->GetEditorWorldContext().World();
    if (!W) return;
    const FName Owner(*(FString(TEXT("addon:")) + kAddonName));
    for (TActorIterator<AActor> It(W); It; ++It)
    {
        if (!It->Tags.Contains(Owner)) continue;
        TArray<UStaticMeshComponent*> Comps;
        It->GetComponents<UStaticMeshComponent>(Comps);
        for (UStaticMeshComponent* C : Comps)
        {
            if (!C) continue;
            // Water_ is the simulated sea and the pools; BF6WaterShaded is the
            // far-world horizon sheet, which arrives through the placement path
            // under an instance-group name. It is the ocean's continuation and
            // takes every per-water pass the ocean takes - the FFT bind most of
            // all, since its normals, foam and wave entities are per-pixel reads
            // of the cascades and do not need vertices to carry them.
            const bool bWater = C->GetName().StartsWith(TEXT("Water_")) ||
                C->ComponentTags.Contains(FName(TEXT("BF6WaterShaded")));
            if (!bWater) continue;
            for (int32 mi = 0; mi < C->GetNumMaterials(); mi++)
                if (UMaterialInstanceDynamic* MID =
                        Cast<UMaterialInstanceDynamic>(C->GetMaterial(mi)))
                    Fn(MID);
        }
    }
}

static void BF6_ForEachGroundMaterial(TFunctionRef<void(UMaterialInstanceDynamic*)> Fn)
{
    if (!GEditor) return;
    UWorld* W = GEditor->GetEditorWorldContext().World();
    if (!W) return;
    const FName Owner(*(FString(TEXT("addon:")) + kAddonName));
    for (TActorIterator<AActor> It(W); It; ++It)
    {
        if (!It->Tags.Contains(Owner)) continue;
        TArray<UStaticMeshComponent*> Comps;
        It->GetComponents<UStaticMeshComponent>(Comps);
        for (UStaticMeshComponent* C : Comps)
        {
            if (!C || !C->GetName().StartsWith(TEXT("Terrain"))) continue;
            for (int32 mi = 0; mi < C->GetNumMaterials(); mi++)
                if (UMaterialInstanceDynamic* MID =
                        Cast<UMaterialInstanceDynamic>(C->GetMaterial(mi)))
                    Fn(MID);
        }
    }
}

static FAutoConsoleCommand GGroundDxilPageCmd(
    TEXT("BF6.HighPoly.DxilPage"),
    TEXT("Evaluate the current level's shipped terrain compute shader in the D3D12 "
         "sidecar and place its in-memory U1 page on the ground. Usage: "
         "BF6.HighPoly.DxilPage [span_m] [resolution] or "
         "<centre_x_m> <centre_z_m> <span_m> [resolution]; "
         "'off' restores the approximate control."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() > 0 && Args[0].Equals(TEXT("off"), ESearchCase::IgnoreCase))
        {
            DisableExactPage();
            return;
        }
        // Saved Tsuru terrain comparison camera, expressed in the game's XZ
        // metres. Explicit coordinates make the command useful on every map.
        float X = -778.96216226f;
        float Z =  412.78984774f;
        float Span = 16.f;
		int32 Resolution = 512;
        if (Args.Num() == 1)
        {
            Span = FCString::Atof(*Args[0]);
        }
		else if (Args.Num() == 2)
		{
			Span = FCString::Atof(*Args[0]);
			Resolution = FCString::Atoi(*Args[1]);
		}
        else if (Args.Num() >= 3)
        {
            X = FCString::Atof(*Args[0]);
            Z = FCString::Atof(*Args[1]);
            Span = FCString::Atof(*Args[2]);
			if (Args.Num() >= 4) Resolution = FCString::Atoi(*Args[3]);
        }
        else if (Args.Num() != 0)
        {
            UE_LOG(LogBF6HighPoly, Display,
                TEXT("usage: BF6.HighPoly.DxilPage [span_m] [resolution] | "
					 "<x_m> <z_m> <span_m> [resolution] | off"));
            return;
        }
        StartExactPage(X, Z, Span, Resolution);
    }));

static FAutoConsoleCommand GGroundDebugCmd(
    TEXT("BF6.HighPoly.GroundDebug"),
    TEXT("0 normal, 1 dominant layer as flat colour, 2 aerial map only, "
         "3 near blend only, 4 far bake only, 5 layers per texel, "
         "6 paint only (omit ordered block-7 base), 7 triplanar slope projection."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.GroundDebug <0-7 or 100+layer>")); return; }
        const float V = FCString::Atof(*Args[0]);
        int32 n = 0;
        BF6_ForEachGroundMaterial([&](UMaterialInstanceDynamic* MID)
        { MID->SetScalarParameterValue(TEXT("DebugMode"), V); n++; });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("ground debug mode %.0f on %d material(s)"), V, n);
    }));

static FAutoConsoleCommand GHighPolyBuildCmd(
    TEXT("BF6.HighPoly.Build"),
    TEXT("Read the current level from the installed game and show it. The same press as the BUILD button: "
         "it also puts the scene in a high-poly mode, switches the placed objects on and fills in any "
         "missing object pictures."),
    // THE SAME THING THE BUTTON DOES. This called StartRead directly, so the
    // console BUILD read the map and left the viewport on the blockout, which
    // is the exact confusion the button was changed to avoid. Two ways to ask
    // for the same thing should not do different things.
    FConsoleCommandDelegate::CreateStatic([] { BuildAndShow(); }));

// The detail ladder, from the console. It had no command at all: the only way
// to change it was the panel's dropdown, which makes it unreachable from a
// script, from a support session, and from any automated check.
static FAutoConsoleCommand GBF6HighPolyDetailCmd(
    TEXT("BF6.HighPoly.Detail"),
    TEXT("BF6.HighPoly.Detail <low|clay|textured>  How the borrowed scenery is drawn. "
         "No argument says which it is on."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        auto Name = [](EMode M)
        {
            return M == EMode::LowPoly ? TEXT("low") : M == EMode::Clay ? TEXT("clay") : TEXT("textured");
        };
        if (Args.Num() >= 1)
        {
            const FString A = Args[0].ToLower();
            EMode Want = GMode;
            if (A == TEXT("low") || A == TEXT("lowpoly") || A == TEXT("0"))      { Want = EMode::LowPoly; }
            else if (A == TEXT("clay") || A == TEXT("1"))                        { Want = EMode::Clay; }
            else if (A == TEXT("textured") || A == TEXT("high") || A == TEXT("2")) { Want = EMode::Textured; }
            else
            {
                UE_LOG(LogBF6HighPoly, Warning, TEXT("not a detail mode: %s. Use low, clay or textured."), *A);
                return;
            }
            GMode = Want;
            ApplyMode();
        }
        UE_LOG(LogBF6HighPoly, Display, TEXT("high poly detail: %s"), Name(GMode));
    }));

// ============================================================================
// WHAT IS ACTUALLY COSTING THE FRAME
//
// The method is the one that worked on the Godot side: fly the same path over
// the whole map, over and over, changing exactly one thing between runs, and
// rank what each thing cost. A number per switch, measured on this machine with
// this map, rather than an argument about which feature feels heavy.
//
// It drives the editor camera with BugItGo, which is the same thing a person
// does when they fly around, and it reads the game thread's own frame time. No
// GPU counters, no external profiler, nothing to install: the question is "does
// the editor stutter while I spin", and this measures exactly that.
//
//   BF6.HighPoly.Profile            every step, three seconds each
//   BF6.HighPoly.Profile 6          six seconds a step, for a noisy machine
//   BF6.HighPoly.Profile 3 quick    the four big ones only
// ============================================================================
namespace
{
	struct FProfStep
	{
		FString            Name;
		TFunction<void()>  Apply;
		// Filled in as it runs.
		double Frames = 0.0, TotalMs = 0.0, WorstMs = 0.0;
		int32  Hitches = 0;
	};

	struct FProfRun
	{
		bool   bActive = false;
		int32  At = 0;
		double StepSecs = 3.0;
		double StepStartedAt = 0.0;
		double SettleUntil = 0.0;
		int32  PathAt = 0;
		FVector Centre = FVector::ZeroVector;
		double  Radius = 20000.0;
		double  Height = 8000.0;
		// What the scene looked like before, so the profile puts it back.
		EMode   WasMode = EMode::LowPoly;
		bool    WasLayer[(int32)ELayer::Count] = {};
		TArray<FProfStep> Steps;
		FTSTicker::FDelegateHandle Ticker;
	};
	FProfRun GProf;

	// The map's own extent, from what the creator has placed, so the path is
	// over their scene rather than over an arbitrary box.
	void ProfMeasureMap()
	{
		GProf.Centre = FVector::ZeroVector;
		GProf.Radius = 20000.0;
		if (!GEditor) { return; }
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) { return; }
		FBox Box(ForceInit);
		int32 N = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (!IsValid(A) || !A->Tags.Contains(FName(TEXT("BF6Placed")))) { continue; }
			Box += A->GetActorLocation();
			N++;
		}
		if (N < 8 || !Box.IsValid) { return; }
		GProf.Centre = Box.GetCenter();
		const FVector Size = Box.GetSize();
		GProf.Radius = FMath::Clamp(FMath::Max(Size.X, Size.Y) * 0.6, 5000.0, 120000.0);
		GProf.Height = FMath::Max(Size.Z, 2000.0) + GProf.Radius * 0.35;
	}

	// One step around the circuit. Twenty-four points is a slow, even orbit at
	// three seconds a step, and it always ends where it started so every step
	// sees the same views.
	void ProfMoveCamera()
	{
		if (!GEditor) { return; }
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) { return; }
		constexpr int32 kPoints = 24;
		const double A = (2.0 * PI * GProf.PathAt) / kPoints;
		GProf.PathAt = (GProf.PathAt + 1) % kPoints;
		const FVector Pos(
			GProf.Centre.X + GProf.Radius * FMath::Cos(A),
			GProf.Centre.Y + GProf.Radius * FMath::Sin(A),
			GProf.Centre.Z + GProf.Height);
		// Looking in at the middle of the map, which is where the scenery is.
		const FVector Dir = (GProf.Centre - Pos).GetSafeNormal();
		const FRotator Rot = Dir.Rotation();
		GEditor->Exec(W, *FString::Printf(TEXT("BugItGo %f %f %f %f %f %f"),
			Pos.X, Pos.Y, Pos.Z, Rot.Pitch, Rot.Yaw, 0.0f));
	}

	void ProfReport()
	{
		UE_LOG(LogBF6HighPoly, Display, TEXT(""));
		UE_LOG(LogBF6HighPoly, Display, TEXT("HIGH POLY PROFILE - %.0f s a step%s"),
			GProf.StepSecs, TEXT(""));
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("  %-28s %9s %9s %9s %8s"), TEXT("what is on"), TEXT("avg ms"),
			TEXT("worst ms"), TEXT("vs base"), TEXT("hitches"));

		double Base = 0.0;
		for (int32 i = 0; i < GProf.Steps.Num(); i++)
		{
			const FProfStep& S = GProf.Steps[i];
			const double Avg = S.Frames > 0 ? S.TotalMs / S.Frames : 0.0;
			if (i == 0) { Base = Avg; }
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  %-28s %9.2f %9.1f %9.2f %8d"),
				*S.Name, Avg, S.WorstMs, Avg - Base, S.Hitches);
		}

		// The ranking, which is the part somebody acts on.
		TArray<int32> Order;
		for (int32 i = 1; i < GProf.Steps.Num(); i++) { Order.Add(i); }
		Order.Sort([&](int32 L, int32 R)
		{
			const double AL = GProf.Steps[L].Frames > 0 ? GProf.Steps[L].TotalMs / GProf.Steps[L].Frames : 0.0;
			const double AR = GProf.Steps[R].Frames > 0 ? GProf.Steps[R].TotalMs / GProf.Steps[R].Frames : 0.0;
			return AL > AR;
		});
		UE_LOG(LogBF6HighPoly, Display, TEXT(""));
		UE_LOG(LogBF6HighPoly, Display, TEXT("  heaviest first:"));
		for (int32 i = 0; i < FMath::Min(5, Order.Num()); i++)
		{
			const FProfStep& S = GProf.Steps[Order[i]];
			const double Avg = S.Frames > 0 ? S.TotalMs / S.Frames : 0.0;
			UE_LOG(LogBF6HighPoly, Display, TEXT("    %d. %s   %.2f ms a frame (%+.2f over the baseline)"),
				i + 1, *S.Name, Avg, Avg - Base);
		}
	}

	void ProfRestore()
	{
		for (int32 i = 0; i < (int32)ELayer::Count; i++)
		{
			GLayers[i].bOn = GProf.WasLayer[i];
			ApplyLayer((ELayer)i);
		}
		GMode = GProf.WasMode;
		ApplyMode();
	}

	bool ProfTick(float Delta)
	{
		if (!GProf.bActive) { return false; }
		const double Now = FPlatformTime::Seconds();

		// A step's first moment is spent settling: a mode change hides and shows
		// components, and timing that would measure the change rather than the
		// state it leaves behind.
		if (Now < GProf.SettleUntil) { return true; }

		FProfStep& S = GProf.Steps[GProf.At];
		if (GProf.StepStartedAt <= 0.0)
		{
			GProf.StepStartedAt = Now;
			GProf.PathAt = 0;
		}

		ProfMoveCamera();
		const double Ms = Delta * 1000.0;
		S.Frames += 1.0;
		S.TotalMs += Ms;
		S.WorstMs = FMath::Max(S.WorstMs, Ms);
		if (Ms > 50.0) { S.Hitches++; }

		if (Now - GProf.StepStartedAt >= GProf.StepSecs)
		{
			UE_LOG(LogBF6HighPoly, Display, TEXT("profile: %s - %.2f ms a frame, worst %.0f ms"),
				*S.Name, S.Frames > 0 ? S.TotalMs / S.Frames : 0.0, S.WorstMs);
			GProf.At++;
			GProf.StepStartedAt = 0.0;
			if (GProf.At >= GProf.Steps.Num())
			{
				GProf.bActive = false;
				ProfReport();
				ProfRestore();
				UE_LOG(LogBF6HighPoly, Display,
					TEXT("profile: done, and the scene is back the way it was."));
				return false;
			}
			GProf.Steps[GProf.At].Apply();
			// A second to let the change land before the clock starts.
			GProf.SettleUntil = FPlatformTime::Seconds() + 1.0;
		}
		return true;
	}

	void ProfSetLayers(bool bTerrain, bool bRoads, bool bObjects, bool bScatter,
	                   bool bWater, bool bLighting)
	{
		const bool Want[(int32)ELayer::Count] = { bTerrain, bRoads, bObjects, bScatter, bWater, bLighting };
		for (int32 i = 0; i < (int32)ELayer::Count; i++)
		{
			GLayers[i].bOn = Want[i];
			ApplyLayer((ELayer)i);
		}
	}

	void BF6_RunProfile(const TArray<FString>& Args)
	{
		if (GProf.bActive)
		{
			GProf.bActive = false;
			ProfRestore();
			UE_LOG(LogBF6HighPoly, Display, TEXT("profile: stopped, scene restored."));
			return;
		}
		if (BF6Ext::CurrentLevel().IsEmpty())
		{
			UE_LOG(LogBF6HighPoly, Warning, TEXT("profile: open a map first."));
			return;
		}

		GProf = FProfRun();
		GProf.StepSecs = Args.Num() >= 1 ? FMath::Clamp(FCString::Atod(*Args[0]), 1.0, 60.0) : 3.0;
		const bool bQuick = Args.ContainsByPredicate([](const FString& A)
			{ return A.Equals(TEXT("quick"), ESearchCase::IgnoreCase); });

		for (int32 i = 0; i < (int32)ELayer::Count; i++) { GProf.WasLayer[i] = GLayers[i].bOn; }
		GProf.WasMode = GMode;
		ProfMeasureMap();

		auto Step = [](const TCHAR* Name, TFunction<void()> Apply)
		{
			FProfStep S;
			S.Name = Name;
			S.Apply = MoveTemp(Apply);
			GProf.Steps.Add(MoveTemp(S));
		};

		// The baseline is the creator's own map with nothing of ours on it, so
		// every number below is what this add-on adds.
		Step(TEXT("your map alone (low poly)"), []
		{
			GMode = EMode::LowPoly; ApplyMode();
			ProfSetLayers(false, false, false, false, false, false);
		});
		if (!bQuick)
		{
			Step(TEXT("+ terrain"),  []{ ProfSetLayers(true, false, false, false, false, false); });
			Step(TEXT("+ roads"),    []{ ProfSetLayers(true, true, false, false, false, false); });
			Step(TEXT("+ objects"),  []{ ProfSetLayers(true, true, true, false, false, false); });
			Step(TEXT("+ scatter"),  []{ ProfSetLayers(true, true, true, true, false, false); });
			Step(TEXT("+ water"),    []{ ProfSetLayers(true, true, true, true, true, false); });
			Step(TEXT("+ lighting"), []{ ProfSetLayers(true, true, true, true, true, true); });
		}
		Step(TEXT("everything, clay"), []
		{
			ProfSetLayers(true, true, true, true, true, true);
			GMode = EMode::Clay; ApplyMode();
		});
		Step(TEXT("everything, textured"), []
		{
			ProfSetLayers(true, true, true, true, true, true);
			GMode = EMode::Textured; ApplyMode();
		});
		Step(TEXT("textured, placed objects off"), []
		{
			GMode = EMode::Textured; ApplyMode();
			BF6HP::Placed::SetEnabled(false);
		});
		Step(TEXT("textured, placed objects on"), []
		{
			GMode = EMode::Textured; ApplyMode();
			BF6HP::Placed::SetEnabled(true);
		});

		UE_LOG(LogBF6HighPoly, Display,
			TEXT("profile: %d step(s), %.0f s each, flying a %.0f m circuit of %s. "
			     "Run BF6.HighPoly.Profile again to stop early."),
			GProf.Steps.Num(), GProf.StepSecs, GProf.Radius / 100.0, *BF6Ext::CurrentLevel());

		GProf.bActive = true;
		GProf.At = 0;
		GProf.Steps[0].Apply();
		GProf.SettleUntil = FPlatformTime::Seconds() + 1.0;
		GProf.Ticker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&ProfTick), 0.0f);
	}
}

static FAutoConsoleCommand GBF6HighPolyProfileCmd(
	TEXT("BF6.HighPoly.Profile"),
	TEXT("Fly a circuit of the open map once per feature and rank what each one costs a frame. "
	     "BF6.HighPoly.Profile [seconds-per-step] [quick]. Run it again to stop. The scene is "
	     "put back the way it was."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&BF6_RunProfile));


static void BF6_RunTsuruBench()
{
		for (int32 i = 0; i < (int32)ELayer::Count; i++) GLayers[i].bOn = true;

		// The isolated level contains mutually-exclusive carrier layouts.  Make
		// the deterministic bench able to select one before StartRead groups the
		// placements; -ExecCmds is too early/racy during editor startup.
		FString BenchGameMode;
		if (FParse::Value(FCommandLine::Get(), TEXT("bf6gamemode="), BenchGameMode))
		{
			GGameMode = BenchGameMode.Equals(TEXT("default"), ESearchCase::IgnoreCase)
				? FString() : BenchGameMode;
			UE_LOG(LogBF6HighPoly, Display, TEXT("Tsuru bench: requested game mode %s"),
				GGameMode.IsEmpty() ? TEXT("(largest)") : *GGameMode);
		}

        // The SDK loader is synchronous and broadcasts OnMapOpened after the
        // base context and saved session are restored. This is the exact path
        // the map-card UI uses, without coordinate-dependent GUI automation.
        BF6Ext::OpenMap(TEXT("MP_Isolated"), TEXT("Water"));
        if (BF6Ext::CurrentLevel() != TEXT("MP_Isolated"))
        {
            UE_LOG(LogBF6HighPoly, Error,
                TEXT("Tsuru bench could not open MP_Isolated/Water"));
            return;
        }

		const bool bKeepCamera = FParse::Param(FCommandLine::Get(), TEXT("bf6keepcamera"));
		const bool bRunwayCamera = FParse::Param(FCommandLine::Get(), TEXT("bf6runway"));
		if (GEditor && bRunwayCamera)
		{
			// Captured from the water clipmap's active-camera telemetry while the
			// runway decal/light failure was visible.  It is deliberately a named
			// bench location rather than replacing the pool/carrier validation view.
			if (UWorld* W = GEditor->GetEditorWorldContext().World())
			{
				GEditor->Exec(W, TEXT("BugItGo -53317.917623 60154.282149 16365.562587 -21.200000 -111.200001 0.000000"));
			}
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("Tsuru bench: restored runway decal/light camera"));
		}
		else if (GEditor && !bKeepCamera)
		{
            if (UWorld* W = GEditor->GetEditorWorldContext().World())
            {
				// Latest verified shoreline view. The earlier rocky-overlook camera
				// left the ocean in the distance and could not validate normals/foam.
				GEditor->Exec(W, TEXT("BugItGo -102007.327149 17811.326936 12586.099973 -26.800000 34.800003 0.000000"));
            }
        }
		else if (GEditor && bKeepCamera)
		{
			// EditorPerProjectUserSettings recorded this perspective viewport when
			// the user parked over the carrier immediately before shutdown. Restore
			// it explicitly: OpenMap can otherwise select the fixed orthographic
			// viewport's older transform instead of the last perspective camera.
			if (UWorld* W = GEditor->GetEditorWorldContext().World())
			{
				GEditor->Exec(W, TEXT("BugItGo -113154.127760 679.668742 12888.445552 -37.000000 -58.399997 0.000000"));
			}
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("Tsuru bench: restored shutdown-captured pool camera"));
		}
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("Tsuru bench: MP_Isolated/Water open, all 6 layers on; starting read"));
		// StartRead schedules the work for the next editor tick. Carry the optional
		// control through to ReadLevel's completion instead of racing the material.
		// Example: -bf6grounddebug=7 tests slope projection at the fixed camera.
		FParse::Value(FCommandLine::Get(), TEXT("bf6grounddebug="), GBenchGroundDebug);
		FParse::Value(FCommandLine::Get(), TEXT("bf6stochastic="), GBenchStochastic);
		StartRead();
}

static FAutoConsoleCommand GHighPolyBenchTsuruCmd(
	TEXT("BF6.HighPoly.BenchTsuru"),
	TEXT("Open MP_Isolated/Water, enable every High Poly layer, build, and jump "
	     "to the fixed terrain comparison camera."),
	FConsoleCommandDelegate::CreateStatic(&BF6_RunTsuruBench));

static FAutoConsoleCommand GGroundPhotoCmd(
    TEXT("BF6.HighPoly.GroundPhoto"),
    TEXT("0..1: how much of the ground colour comes from the level's aerial map "
         "rather than the live decoded layer evaluation. 0 is the map-wide "
         "runtime default; 1 is the low-frequency aerial control."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.GroundPhoto <0..1>")); return; }
        const float V = FCString::Atof(*Args[0]);
        int32 n = 0;
        BF6_ForEachGroundMaterial([&](UMaterialInstanceDynamic* MID)
        { MID->SetScalarParameterValue(TEXT("PhotoMix"), V); n++; });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("ground photo mix %.2f on %d material(s)"), V, n);
    }));

static FAutoConsoleCommand GGroundStochasticCmd(
    TEXT("BF6.HighPoly.GroundStochastic"),
    TEXT("0/1: direct periodic terrain sheets or three-sample stochastic tile breakup."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.GroundStochastic <0|1>")); return; }
        const float V = FCString::Atof(*Args[0]) >= 0.5f ? 1.f : 0.f;
        int32 n = 0;
        BF6_ForEachGroundMaterial([&](UMaterialInstanceDynamic* MID)
        { MID->SetScalarParameterValue(TEXT("StochasticTiling"), V); n++; });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("ground stochastic tiling %.0f on %d material(s)"), V, n);
    }));

static FAutoConsoleCommand GGroundCoverageUnionCmd(
    TEXT("BF6.HighPoly.GroundCoverageUnion"),
    TEXT("0/1: nearest sparse coverage stack control or continuous four-corner union."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.GroundCoverageUnion <0|1>")); return; }
        const float V = FCString::Atof(*Args[0]) >= 0.5f ? 1.f : 0.f;
        int32 n = 0;
        BF6_ForEachGroundMaterial([&](UMaterialInstanceDynamic* MID)
        { MID->SetScalarParameterValue(TEXT("CoverageUnion"), V); n++; });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("ground coverage four-corner union %.0f on %d material(s)"), V, n);
    }));

static FAutoConsoleCommand GGroundBlendCmd(
    TEXT("BF6.HighPoly.GroundBlend"),
    TEXT("<near> <far> in metres: where the per-pixel blend gives way to the flattened bake."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 2) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.GroundBlend <near> <far>")); return; }
        const float N = FCString::Atof(*Args[0]), F = FCString::Atof(*Args[1]);
        int32 n = 0;
        BF6_ForEachGroundMaterial([&](UMaterialInstanceDynamic* MID)
        {
            MID->SetScalarParameterValue(TEXT("BlendNear"), N);
            MID->SetScalarParameterValue(TEXT("BlendFar"), F);
            n++;
        });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("ground blend %.0f to %.0f m on %d material(s)"), N, F, n);
    }));

// THE SKY, live.
//
// Two numbers here are not decoded and should be found by looking rather than
// argued about. The ROTATION carries a half-turn seam constant: the panorama's
// painted sun sits at u = 0.5, measured on five maps, and the sign convention
// between the game's bearing and Unreal's axes is the part that could still be
// half a turn out. MP_Aftermath is the map to check it on, because its painted
// sun is a hard-edged disc at a low elevation, and it should land on the
// authored bearing of 237.9 degrees.
//
// The BRIGHTNESS is derived rather than guessed - the sky reuses the sun's own
// lux-to-editor factor, which preserves the measured 0.09 to 0.30 illuminance
// ratio - but a scale on top of it costs nothing and settles any argument
// about whether the derivation landed.
static void BF6_ForEachSkyMaterial(TFunctionRef<void(UMaterialInstanceDynamic*)> Fn)
{
    if (!GEditor) return;
    UWorld* W = GEditor->GetEditorWorldContext().World();
    if (!W) return;
    const FName Owner(*(FString(TEXT("addon:")) + kAddonName));
    for (TActorIterator<AActor> It(W); It; ++It)
    {
        if (!It->Tags.Contains(Owner)) continue;
        TArray<UStaticMeshComponent*> Comps;
        It->GetComponents<UStaticMeshComponent>(Comps);
        for (UStaticMeshComponent* C : Comps)
        {
            if (!C || !C->GetName().Contains(TEXT("SkyDome"))) continue;
            for (int32 mi = 0; mi < C->GetNumMaterials(); mi++)
                if (UMaterialInstanceDynamic* MID =
                        Cast<UMaterialInstanceDynamic>(C->GetMaterial(mi)))
                    Fn(MID);
        }
    }
}

static FAutoConsoleCommand GGameModeBuildAllCmd(
    TEXT("BF6.HighPoly.GameModeBuildAll"),
    TEXT("1 (default): build every mode's props, tagged, so switching modes is instant. "
         "0: build only the chosen mode, the way it used to, for less memory. Rebuild to apply."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() >= 1) GGameModeBuildAll = FCString::Atoi(*Args[0]) != 0;
        UE_LOG(LogBF6HighPoly, Display, TEXT("game modes: build all = %d"), GGameModeBuildAll ? 1 : 0);
    }));

static FAutoConsoleCommand GGameModeCmd(
    TEXT("BF6.HighPoly.GameMode"),
    TEXT("Which game mode's props to build: a mode name, or \"all\" to stack every "
         "mode the way it used to, or empty to go back to the largest. They are "
         "ALTERNATIVE layouts of the same ground, so building them all draws four "
         "carriers where the game shows one. Rebuild the map to apply."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1)
        {
            UE_LOG(LogBF6HighPoly, Display,
                TEXT("usage: BF6.HighPoly.GameMode <name|all>  (current \"%s\")"),
                GGameMode.IsEmpty() ? TEXT("(largest)") : *GGameMode);
            return;
        }
        GGameMode = Args[0].Equals(TEXT("default"), ESearchCase::IgnoreCase)
            ? FString() : Args[0];
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("game mode set to \"%s\" - applied to the built art now, and the next build keys on it"),
            GGameMode.IsEmpty() ? TEXT("(largest)") : *GGameMode);
        BF6HPGameMode::SetMode(GGameMode);
    }));

static FAutoConsoleCommand GSkyCmd(
    TEXT("BF6.HighPoly.Sky"),
    TEXT("0 or 1: show the level's painted sky dome. It rides on the Lighting "
         "layer otherwise, and turning that off to hide the sky would take every "
         "lamp in the map with it."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1 || !GEditor) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.Sky <0|1>")); return; }
        const bool bOn = FCString::Atoi(*Args[0]) != 0;
        UWorld* W = GEditor->GetEditorWorldContext().World();
        if (!W) return;
        const FName Owner(*(FString(TEXT("addon:")) + kAddonName));
        int32 n = 0;
        for (TActorIterator<AActor> It(W); It; ++It)
        {
            if (!It->Tags.Contains(Owner)) continue;
            TArray<UStaticMeshComponent*> Comps;
            It->GetComponents<UStaticMeshComponent>(Comps);
            for (UStaticMeshComponent* C : Comps)
                if (C && C->GetName().Contains(TEXT("SkyDome")))
                { C->SetVisibility(bOn, false); n++; }
        }
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("sky dome %s on %d component(s)"), bOn ? TEXT("on") : TEXT("off"), n);
    }));

static FAutoConsoleCommand GSkyRotationCmd(
    TEXT("BF6.HighPoly.SkyRotation"),
    TEXT("Sky panorama rotation in TURNS, not degrees. The authored value is "
         "applied automatically; this overrides it. Add or subtract 0.5 to test "
         "the seam convention - on MP_Aftermath the painted sun should sit at "
         "bearing 237.9."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.SkyRotation <turns>")); return; }
        const float V = FCString::Atof(*Args[0]);
        int32 n = 0;
        BF6_ForEachSkyMaterial([&](UMaterialInstanceDynamic* MID)
        { MID->SetScalarParameterValue(TEXT("SkyRotationTurns"), V); n++; });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("sky rotation %.4f turns (%.1f deg) on %d material(s)"),
            V, V * 360.f, n);
    }));

static FAutoConsoleCommand GSkyBrightnessCmd(
    TEXT("BF6.HighPoly.SkyBrightness"),
    TEXT("Emissive scale on the sky panorama. The automatic value reuses the "
         "sun's own lux conversion, which holds the measured sky-to-sun "
         "illuminance ratio of about 0.19."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.SkyBrightness <scale>")); return; }
        const float V = FCString::Atof(*Args[0]);
        int32 n = 0;
        BF6_ForEachSkyMaterial([&](UMaterialInstanceDynamic* MID)
        { MID->SetScalarParameterValue(TEXT("SkyEmissiveScale"), V); n++; });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("sky emissive scale %.5g on %d material(s)"), V, n);
    }));

static FAutoConsoleCommand GWaterFoamCoverageCmd(
    TEXT("BF6.HighPoly.WaterFoamCoverage"),
    TEXT("0..1: how much of the surface the foam covers. This is Unreal's water "
         "Opacity, which is the coverage of the material ON TOP of the water - at "
         "1 the water volume is switched off entirely. Set 0 for guaranteed clear "
         "water; if that does not clear it, the fault is not in this material."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.WaterFoamCoverage <0..1>")); return; }
        const float V = FCString::Atof(*Args[0]);
        int32 n = 0;
        BF6_ForEachWaterMaterial([&](UMaterialInstanceDynamic* MID)
        { MID->SetScalarParameterValue(TEXT("FoamCoverage"), V); n++; });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("water foam coverage %.2f on %d material(s)"), V, n);
    }));

static FAutoConsoleCommand GWaterFoamWaveHeightCmd(
    TEXT("BF6.HighPoly.WaterWaveHeight"),
    TEXT("Deprecated negative control. BF6 broad foam is pixel-stage data and no "
         "longer displaces the water grid; decoded FFT/heightfield displacement is unchanged."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1)
        {
            UE_LOG(LogBF6HighPoly, Display,
                TEXT("usage: BF6.HighPoly.WaterWaveHeight <metres>  (current %.3f)"),
                GWaterFoamWaveHeightM);
            return;
        }
		float Requested = GWaterFoamWaveHeightM;
		if (!LexTryParseString(Requested, *Args[0]))
		{
			UE_LOG(LogBF6HighPoly, Error, TEXT("invalid water wave height: %s"), *Args[0]);
			return;
		}
		GWaterFoamWaveHeightM = FMath::Clamp(Requested, 0.f, 8.f);
        int32 n = 0;
        BF6_ForEachWaterMaterial([&](UMaterialInstanceDynamic* MID)
        {
            MID->SetScalarParameterValue(TEXT("BF6FoamWaveHeightM"), GWaterFoamWaveHeightM);
			MID->SetScalarParameterValue(TEXT("BF6InteractiveBridgeHeightM"), GWaterFoamWaveHeightM);
            ++n;
        });
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("water provisional interactive bridge set to %.3f m on %d material(s); exact atlas replay remains open"),
            GWaterFoamWaveHeightM, n);
    }));

// Push the current wave-entity set to every live water material. Returns the
// number of materials written, so a command can say "0 materials" out loud
// rather than looking like it worked on a map with no water built.
// Rebind every live water material to the CURRENT FFT textures. Called a tick
// after a spectrum rebuild, when those textures actually hold evolved data.
//
// Defined inside the file's anonymous namespace so it is the same entity the
// tick's forward declaration names - at file scope it would be a different
// symbol and the tick would not link against it.
namespace
{
	void BF6_RebindWaterMaterials()
	{
		if (!GWaterFFT.IsValid()) return;
		int32 Materials = 0;
		BF6_ForEachWaterMaterial([&](UMaterialInstanceDynamic* MID)
		{
			float AmpScale = 1.f;
			MID->GetScalarParameterValue(TEXT("BF6WaveAmplitudeScale"), AmpScale);
			GWaterFFT->Bind(MID, AmpScale);
			MID->RecacheUniformExpressions(true);
			++Materials;
		});
		UE_LOG(LogBF6HighPoly, Verbose,
			TEXT("water: rebound %d material(s) to the evolved spectrum"), Materials);
	}
}

static int32 BF6_PushWaveEntities()
{
	FLinearColor Slots[16];
	float Amps[16];
	BF6_PackWaveEntities(Slots, Amps);
	int32 Materials = 0;
	BF6_ForEachWaterMaterial([&](UMaterialInstanceDynamic* MID)
	{
		BF6_ApplyWaveEntities(MID, Slots, Amps);
		++Materials;
	});
	return Materials;
}

static FAutoConsoleCommand GWaveAddCmd(
	TEXT("BF6.HighPoly.WaveAdd"),
	TEXT("<x_m> <y_m> <radius_m> <amplitude_m>: add one wave entity at a world XY "
	     "in metres. Amplitude is the crest height at the centre; the mound falls to "
	     "zero at the radius as cos, and dampens the FFT cascades inside it, both "
	     "exactly as the game's draw shader does. Sixteen is the hard cap, which is "
	     "the game's cap, not ours. These are AUTHOR-PLACED: no shipped level "
	     "contains wave entities, the runtime creates them."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 4)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("usage: BF6.HighPoly.WaveAdd <x_m> <y_m> <radius_m> <amplitude_m>  (%d placed)"),
				GWaveEntities.Num());
			return;
		}
		FBF6WaveEntity E;
		float X = 0.f, Y = 0.f, R = 0.f, Amp = 0.f;
		if (!LexTryParseString(X, *Args[0]) || !LexTryParseString(Y, *Args[1]) ||
			!LexTryParseString(R, *Args[2]) || !LexTryParseString(Amp, *Args[3]))
		{
			UE_LOG(LogBF6HighPoly, Error, TEXT("wave add: four numbers expected"));
			return;
		}
		if (R < 1e-6f)
		{
			// The dispatcher skips these, so accepting one would silently place a
			// wave that never appears.
			UE_LOG(LogBF6HighPoly, Error,
				TEXT("wave add: radius %g is below the 1e-6 m threshold the game's own "
				     "collector uses, so this wave would never be picked up"), R);
			return;
		}
		E.CentreM = FVector2D(X, Y);
		E.RadiusM = R;
		E.AmplitudeM = Amp;
		GWaveEntities.Add(E);
		const int32 Materials = BF6_PushWaveEntities();
		if (GWaveEntities.Num() > 16)
		{
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("wave add: %d placed but only the first 16 reach the shader; "
				     "the rest are held and will appear if earlier ones are cleared"),
				GWaveEntities.Num());
		}
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("wave added at (%.1f, %.1f) m, radius %.1f m, amplitude %.2f m; "
			     "%d wave(s) pushed to %d water material(s)"),
			X, Y, R, Amp, FMath::Min(GWaveEntities.Num(), 16), Materials);
	}));

static FAutoConsoleCommand GWaveClearCmd(
	TEXT("BF6.HighPoly.WaveClear"),
	TEXT("Remove every wave entity and return the surface to its authored state. "
	     "This is the negative control for the wave work: with the slots back on "
	     "the game's disabled sentinel the sea must fall back to the millimetre "
	     "FFT chop the level actually authors."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		const int32 Had = GWaveEntities.Num();
		GWaveEntities.Reset();
		const int32 Materials = BF6_PushWaveEntities();
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("cleared %d wave(s) on %d water material(s); slots returned to the "
			     "game's disabled sentinel (recipRadius 1e10)"), Had, Materials);
	}));

static FAutoConsoleCommand GWaveListCmd(
	TEXT("BF6.HighPoly.WaveList"),
	TEXT("List the placed wave entities and the values actually packed into the "
	     "shader slots, so the packing can be checked against the game's."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		if (GWaveEntities.Num() == 0)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("no wave entities placed; all 16 slots carry the disabled sentinel"));
			return;
		}
		FLinearColor Slots[16];
		float Amps[16];
		const int32 Packed = BF6_PackWaveEntities(Slots, Amps);
		UE_LOG(LogBF6HighPoly, Display, TEXT("%d wave(s) placed, %d packed:"),
			GWaveEntities.Num(), Packed);
		for (int32 i = 0; i < Packed; ++i)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  slot %2d  centre (%.1f, %.1f) m  recipRadius %.6g (radius %.2f m)  "
				     "halfAmplitude %.3f m (crest %.3f m)"),
				i, Slots[i].R, Slots[i].G, Slots[i].B,
				Slots[i].B > 0.f ? 1.f / Slots[i].B : 0.f,
				Amps[i], Amps[i] * 2.f);
		}
	}));

static FAutoConsoleCommand GWaveFieldCmd(
	TEXT("BF6.HighPoly.WaveField"),
	TEXT("<count> <spacing_m> <radius_m> <amplitude_m>: lay a grid of wave entities "
	     "around the build viewport camera, for looking at open-water swell without "
	     "typing sixteen positions. Replaces the current set. Author-placed: this "
	     "reproduces the game's wave ARITHMETIC with values a person chose, not the "
	     "game's own wave field, which lives in runtime timeline bindings."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 4)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("usage: BF6.HighPoly.WaveField <count> <spacing_m> <radius_m> <amplitude_m>"));
			return;
		}
		int32 Count = 0;
		float Spacing = 0.f, R = 0.f, Amp = 0.f;
		if (!LexTryParseString(Count, *Args[0]) || !LexTryParseString(Spacing, *Args[1]) ||
			!LexTryParseString(R, *Args[2]) || !LexTryParseString(Amp, *Args[3]))
		{
			UE_LOG(LogBF6HighPoly, Error, TEXT("wave field: four numbers expected"));
			return;
		}
		Count = FMath::Clamp(Count, 1, 16);
		if (R < 1e-6f || Spacing <= 0.f)
		{
			UE_LOG(LogBF6HighPoly, Error,
				TEXT("wave field: radius must clear 1e-6 m and spacing must be positive"));
			return;
		}
		// Centre on what the user is actually looking at. The tool's own build
		// viewport is the seam to ask: a generic editor viewport global can resolve
		// to a hidden preview client and would silently place the field somewhere
		// off screen.
		FVector CamLoc = FVector::ZeroVector;
		FRotator CamRot = FRotator::ZeroRotator;
		const bool bHaveCam = BF6Ext::GetBuildViewportCamera(CamLoc, CamRot);
		const FVector2D CentreM(CamLoc.X * 0.01, CamLoc.Y * 0.01);

		GWaveEntities.Reset();
		const int32 Side = FMath::CeilToInt(FMath::Sqrt((float)Count));
		for (int32 i = 0; i < Count; ++i)
		{
			const int32 Col = i % Side;
			const int32 Row = i / Side;
			FBF6WaveEntity E;
			E.CentreM = CentreM + FVector2D(
				(Col - (Side - 1) * 0.5f) * Spacing,
				(Row - (Side - 1) * 0.5f) * Spacing);
			E.RadiusM = R;
			E.AmplitudeM = Amp;
			GWaveEntities.Add(E);
		}
		const int32 Materials = BF6_PushWaveEntities();
		if (Spacing < R * 2.f)
		{
			// Every live slot multiplies the cascade dampening product, so
			// overlapping waves suppress the FFT chop between them. That is the
			// game's own contract, but it reads as the chop having broken, so say
			// it rather than let it be discovered.
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("wave field: %.0f m spacing is closer than the %.0f m diameter, so the "
				     "waves overlap and their dampening multiplies - expect the FFT chop to "
				     "flatten between crests. Space them at least %.0f m apart to keep it."),
				Spacing, R * 2.f, R * 2.f);
		}
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("placed %d wave(s) on a %dx%d grid at %.0f m spacing centred on (%.1f, %.1f) m "
			     "(%s), radius %.1f m, amplitude %.2f m; pushed to %d water material(s)"),
			Count, Side, Side, Spacing, CentreM.X, CentreM.Y,
			bHaveCam ? TEXT("build viewport camera") : TEXT("world origin, no build viewport"),
			R, Amp, Materials);
	}));

// SEA STATE. The honest dial for wave size, and the one to reach for before the
// wave entities above. The level authors a near-calm wind and therefore a
// millimetre sea; the game's own spectrum builder reaches a metre-scale sea at
// wind 9 to 12, a wind other shipped levels already author (finding
// ocean-wind-law-metre-waves-at-plausible-wind). Changing the wind and letting
// the shipped builder run rebuilds the whole surface - displacement, normals,
// foam and the cascade merge all follow, because they are computed from the
// spectrum rather than scaled after it.
// Re-read only the ocean cascades and re-initialise the FFT. Changing the sea
// state does not touch placements, meshes or Nanite, so making it go through the
// full level read cost about ninety seconds for one number and made finding a
// sea state impractical. This is the same read the full build performs, minus
// everything the wind cannot affect.
static bool BF6_RebuildWaterSpectrum()
{
	const FString Level = BF6Ext::CurrentLevel();
	if (Level.IsEmpty())
	{
		UE_LOG(LogBF6HighPoly, Warning,
			TEXT("sea state: no level is open, so there is nothing to rebuild"));
		return false;
	}
	TArray<BF6HP::FCore::FWaterCascade> Cascades;
	TUniquePtr<BF6HP::FWaterFFT> Fresh = MakeUnique<BF6HP::FWaterFFT>();
	if (!GCore.ReadWaterCascades(Level, Cascades) ||
		!Fresh->Initialize(GCore, Cascades))
	{
		// Leave the previous water standing rather than replacing it with
		// nothing: a failed re-read should not blank a surface that worked.
		UE_LOG(LogBF6HighPoly, Error,
			TEXT("sea state: cascade re-read failed for %s (%s); the previous water is left alone"),
			*Level, *GCore.Error);
		return false;
	}
	GWaterFFT = MoveTemp(Fresh);

	// The new FFT owns new textures, so every live water material has to be
	// rebound or it keeps sampling the old ones. The amplitude scale is read
	// back off the material so the authored value survives the rebind.
	int32 Materials = 0;
	BF6_ForEachWaterMaterial([&](UMaterialInstanceDynamic* MID)
	{
		float AmpScale = 1.f;
		MID->GetScalarParameterValue(TEXT("BF6WaveAmplitudeScale"), AmpScale);
		GWaterFFT->Bind(MID, AmpScale);
		// Force the render-side uniform expression cache to rebuild.
		//
		// Bind swaps TEXTURE parameters, and a texture write does not reliably
		// rebuild that cache on its own: the instance holds the new texture while
		// the surface keeps drawing with the old expressions, so a rebuilt
		// spectrum appears to do nothing until some unrelated parameter write
		// happens to dirty it. That is why toggling the detail sheets made an
		// amplitude change suddenly show up - the scalar write was doing this
		// recache as a side effect.
		MID->RecacheUniformExpressions(true);
		++Materials;
	});
	// Bind again a couple of ticks from now, once the new textures hold evolved
	// data. Binding here is not enough on its own and that is not a cache
	// problem: the textures are empty at this instant.
	GWaterRebindPending = 2;
	UE_LOG(LogBF6HighPoly, Display,
		TEXT("sea state: %d cascade(s) rebuilt and rebound to %d water material(s); "
		     "rebinding again once the new textures carry evolved data"),
		GWaterFFT->Num(), Materials);
	if (Materials == 0)
	{
		UE_LOG(LogBF6HighPoly, Warning,
			TEXT("sea state: no water materials are live, so nothing on screen will change "
			     "until High Poly water is built"));
	}
	return true;
}

static void BF6_ReportSeaState()
{
	const float Abs = BF6HP::FCore::WindOverrideAbsolute();
	const float Scale = BF6HP::FCore::WindOverrideScale();
	if (Abs >= 0.f)
	{
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("sea state: every cascade forced to wind %.3f."), Abs);
	}
	else if (Scale != 1.f)
	{
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("sea state: authored winds multiplied by %.3f."), Scale);
	}
	else
	{
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("sea state: the level's authored winds, unmodified."));
	}
}

static FAutoConsoleCommand GWaterWindCmd(
	TEXT("BF6.HighPoly.WaterWind"),
	TEXT("<metres per second>: force every ocean cascade to one wind and rebuild "
	     "the spectrum with the game's own builder. The level's authored wind is "
	     "what ships; on a calm map that is a millimetre sea. Around 9 to 12 gives "
	     "a metre-scale sea. Negative clears the override. Applies immediately. Note "
	     "this replaces the relationship the level authored BETWEEN cascades, "
	     "including any deliberately left near zero: WaterWindScale keeps that."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			BF6_ReportSeaState();
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("usage: BF6.HighPoly.WaterWind <metres per second>, negative to clear"));
			return;
		}
		float W = 0.f;
		if (!LexTryParseString(W, *Args[0]))
		{
			UE_LOG(LogBF6HighPoly, Error, TEXT("water wind: a number was expected"));
			return;
		}
		if (W > 25.f)
		{
			// Measured, not guessed: the spectrum stops climbing past about 25
			// and is lower at 30 than at 25, so a bigger number here is not a
			// bigger sea.
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("water wind: the spectrum stops rising around 25 and is smaller at 30 "
				     "than at 25, so %.1f will not give a bigger sea than 25 would"), W);
		}
		BF6HP::FCore::SetWindOverride(W < 0.f ? -1.f : W, 1.f);
		BF6_ReportSeaState();
		BF6_RebuildWaterSpectrum();
	}));

static FAutoConsoleCommand GDerivedCacheClearCmd(
	TEXT("BF6.HighPoly.CacheClear"),
	TEXT("Delete the derived cache (ground bakes, coverage, sheets, heightfield) for every install signature."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		const int32 N = BF6HP::DiskCache::Clear();
		UE_LOG(LogBF6HighPoly, Display, TEXT("derived cache cleared: %d blob(s) removed"), N);
	}));

static FAutoConsoleCommand GWaterTreeStatusCmd(
	TEXT("BF6.HighPoly.WaterTreeStatus"),
	TEXT("report what the water draw tree currently believes: the camera it is "
	     "following, the water surface bounds, and how many tiles came out fine "
	     "versus held coarse. Run it facing one way, then facing the other: if "
	     "the reported yaw does not change, the tree is not following the view; "
	     "if it changes but the fine-tile count collapses, the cull or the "
	     "surface bounds are dropping them."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		ReportWaterTreeStatus();
	}));

static FAutoConsoleCommand GWaterTreeHzCmd(
	TEXT("BF6.HighPoly.WaterTreeHz"),
	TEXT("How many times a second the water draw tree may be rebuilt (default 5, ")
	TEXT("0 for no limit). The tree is 12,288 instances at full size and rebuilding ")
	TEXT("it is the single most expensive thing the add-on does per frame, so ")
	TEXT("flying the viewport with water on used to rebuild it every frame. Raise ")
	TEXT("it if the water lags behind fast camera moves; lower it for more frame."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() >= 1) GWaterTreeMaxHz = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 240.f);
		// Braced on purpose: UE_LOG expands to a block, so an unbraced
		// if/else around two of them does not compile.
		if (GWaterTreeMaxHz <= 0.f)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("water draw tree rebuild is unlimited (every frame the camera moves)"));
		}
		else
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("water draw tree rebuilds at most %.1f time(s) a second"), GWaterTreeMaxHz);
		}
	}));

static FAutoConsoleCommand GWaterOverlapCmd(
	TEXT("BF6.HighPoly.WaterOverlap"),
	TEXT("<0|1|-1>: force the cascade-0 overlap off, on, or back to whatever the "
	     "level authored. Applies to the live water materials immediately, so it "
	     "is an A/B rather than a rebuild. The overlap is the game's rotated "
	     "second sample of cascade 0; cascade 0's spectrum is a corrugation, so "
	     "this is the only thing in the draw path that can add a second wave "
	     "direction."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() >= 1)
		{
			int32 Value = -1;
			if (!LexTryParseString(Value, *Args[0]))
			{
				UE_LOG(LogBF6HighPoly, Error,
					TEXT("usage: BF6.HighPoly.WaterOverlap <0|1|-1>"));
				return;
			}
			GWaterOverlapOverride = FMath::Clamp(Value, -1, 1);
		}
		int32 Applied = 0;
		for (const FWaterClipmapSurface& Surface : GWaterClipmaps)
		{
			UHierarchicalInstancedStaticMeshComponent* HISM = Surface.Component.Get();
			if (!HISM) continue;
			for (int32 SlotIndex = 0; SlotIndex < HISM->GetNumMaterials(); ++SlotIndex)
			{
				UMaterialInstanceDynamic* MID =
					Cast<UMaterialInstanceDynamic>(HISM->GetMaterial(SlotIndex));
				if (!MID) continue;
				float Authored = 0.f;
				MID->GetScalarParameterValue(TEXT("BF6CascadeOverlapEnabled"), Authored);
				const float Value = GWaterOverlapOverride >= 0
					? (float)GWaterOverlapOverride : Authored;
				MID->SetScalarParameterValue(TEXT("BF6CascadeOverlapEnabled"), Value);
				++Applied;
			}
		}
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("water overlap: override %d applied to %d live material(s); "
			     "-1 restores the authored value on the next water build"),
			GWaterOverlapOverride, Applied);
	}));

static FAutoConsoleCommand GWaterWindScaleCmd(
	TEXT("BF6.HighPoly.WaterWindScale"),
	TEXT("<factor>: multiply every cascade's AUTHORED wind and rebuild the "
	     "spectrum. Unlike WaterWind this keeps the relationship the level "
	     "authored between cascades, so a cascade the level left near zero stays "
	     "near zero. Applies immediately."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			BF6_ReportSeaState();
			UE_LOG(LogBF6HighPoly, Display, TEXT("usage: BF6.HighPoly.WaterWindScale <factor>"));
			return;
		}
		float S = 0.f;
		if (!LexTryParseString(S, *Args[0]) || S <= 0.f)
		{
			UE_LOG(LogBF6HighPoly, Error,
				TEXT("water wind scale: a positive number was expected"));
			return;
		}
		BF6HP::FCore::SetWindOverride(-1.f, S);
		BF6_ReportSeaState();
		BF6_RebuildWaterSpectrum();
	}));

static FAutoConsoleCommand GWaterMinWavelengthCmd(
	TEXT("BF6.HighPoly.WaterMinWavelength"),
	TEXT("<metres> [cascade]: the shortest wave a cascade keeps. Omitting the "
	     "index sets all four. A cascade spans every wavelength from its tile "
	     "size down to twice its texel, so the 300 m cascade carries chop as "
	     "well as swell; this is the filter that strips the short end out. "
	     "mp_isolated authors 0.26 on cascade 0 and 6.0 on cascades 1 and 2, "
	     "deliberately keeping the middle bands long-only. Raise it on a cascade "
	     "to make that cascade pure swell. Negative restores the authored value."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			FString S;
			for (int32 c = 0; c < 4; ++c)
			{
				const float Cur = BF6HP::FCore::MinWavelengthOverride(c);
				S += (Cur >= 0.f)
					? FString::Printf(TEXT("  cascade %d: %.2f m\n"), c, Cur)
					: FString::Printf(TEXT("  cascade %d: authored\n"), c);
			}
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("min wavelength:\n%susage: BF6.HighPoly.WaterMinWavelength <metres> [cascade]"),
				*S);
			return;
		}
		float M = 0.f;
		if (!LexTryParseString(M, *Args[0]))
		{
			UE_LOG(LogBF6HighPoly, Error, TEXT("min wavelength: a number was expected"));
			return;
		}
		int32 Cascade = -1;
		if (Args.Num() >= 2) LexTryParseString(Cascade, *Args[1]);
		Cascade = (Cascade < 0 || Cascade > 3) ? -1 : Cascade;
		BF6HP::FCore::SetMinWavelengthOverride(Cascade, M < 0.f ? -1.f : M);
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("min wavelength %s on %s"),
			M < 0.f ? TEXT("restored to authored")
			        : *FString::Printf(TEXT("forced to %.2f m"), M),
			Cascade < 0 ? TEXT("all cascades")
			            : *FString::Printf(TEXT("cascade %d"), Cascade));
		BF6_RebuildWaterSpectrum();
	}));

static FAutoConsoleCommand GWaterProbeCmd(
	TEXT("BF6.HighPoly.WaterProbe"),
	TEXT("Report the peak displacement each ocean cascade is actually producing, "
	     "in metres, measured from the last evolve. This is the geometry BEFORE "
	     "the material's shore and CoarseMask attenuation, so it separates a small "
	     "spectrum from a large one being attenuated away."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		if (!GWaterFFT.IsValid())
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("water probe: no ocean FFT is built for this level"));
			return;
		}
		UE_LOG(LogBF6HighPoly, Display, TEXT("%s"), *GWaterFFT->DisplacementSummary());
		// The material multiplies the vertical displacement by this before
		// anything else, so a small value here suppresses the whole sea no
		// matter how large the spectrum is. It is authored, so report it rather
		// than assume it is one.
		int32 Seen = 0;
		BF6_ForEachWaterMaterial([&](UMaterialInstanceDynamic* MID)
		{
			if (Seen++ > 0) return;
			float AmpScale = -1.f, Enabled = -1.f;
			MID->GetScalarParameterValue(TEXT("BF6WaveAmplitudeScale"), AmpScale);
			MID->GetScalarParameterValue(TEXT("BF6FFTEnabled"), Enabled);
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  material: wave amplitude scale %.4f, FFT enabled %.0f. The vertical "
				     "displacement above is multiplied by this scale, then by the shore and "
				     "CoarseMask attenuation, which applies to cascades 0 and 1 only."),
				AmpScale, Enabled);
		});
		if (Seen == 0)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  no live water material to read the amplitude scale from"));
		}

		// THE ATTENUATION, MEASURED. Cascades 0 and 1 carry almost all of the
		// height and are the only two CoarseMask multiplies, so the number that
		// decides whether the sea is visible is the mask at the place being
		// looked at. Sample outward from the camera so the shore-to-offshore
		// profile is visible rather than one ambiguous reading.
		float Tall[4] = {0, 0, 0, 0};
		GWaterFFT->PeakVerticalPerCascade(Tall, 4);
		FVector CamLoc = FVector::ZeroVector;
		FRotator CamRot = FRotator::ZeroRotator;
		const bool bCam = BF6Ext::GetBuildViewportCamera(CamLoc, CamRot);
		const FVector2D CamM(CamLoc.X * 0.01, CamLoc.Y * 0.01);
		// Sample along the VIEW DIRECTION. Sampling a fixed compass direction
		// measures water the viewer may not be looking at, which is how a probe
		// ends up disagreeing with the screen for reasons that have nothing to
		// do with the water.
		const FVector Fwd = CamRot.Vector();
		FVector2D Dir(Fwd.X, Fwd.Y);
		if (!Dir.Normalize()) Dir = FVector2D(1.0, 0.0);
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("CoarseMask attenuation along the view from %s (%.0f, %.0f) m, "
			     "heading (%.2f, %.2f), and the vertical displacement that survives it:"),
			bCam ? TEXT("the build camera") : TEXT("world origin"),
			CamM.X, CamM.Y, Dir.X, Dir.Y);
		static const float kOffsets[] = {0.f, 100.f, 250.f, 500.f, 1000.f, 2000.f};
		for (float Off : kOffsets)
		{
			bool bOk = false;
			const FVector2D P = CamM + Dir * Off;
			const float Coarse = BF6_EvalCoarseMask(P, bOk);
			// Exactly the material's composition: the mask multiplies cascades
			// 0 and 1 only; 2 and 3 pass through untouched.
			const float Survives = Coarse * (Tall[0] + Tall[1]) + Tall[2] + Tall[3];
			const float Unattenuated = Tall[0] + Tall[1] + Tall[2] + Tall[3];
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  +%6.0f m ahead  (%8.0f, %8.0f)  mask %.4f%s  ->  %.4f m of %.4f m "
				     "(%.0f%% survives)"),
				Off, P.X, P.Y, Coarse, bOk ? TEXT("") : TEXT(" [no mask data]"),
				Survives, Unattenuated,
				Unattenuated > 0.f ? 100.f * Survives / Unattenuated : 0.f);
		}

		// Is the surface sampling the CURRENT spectrum? A rebind that did not
		// reach the drawn material leaves the screen on the previous textures
		// while every number above reports the new ones, which reads as the
		// controls doing nothing.
		int32 Checked = 0, Stale = 0;
		BF6_ForEachWaterMaterial([&](UMaterialInstanceDynamic* MID)
		{
			UTexture* Bound = nullptr;
			MID->GetTextureParameterValue(TEXT("BF6Disp0"), Bound);
			++Checked;
			if (Bound != (UTexture*)GWaterFFT->DisplacementTexture(0)) ++Stale;
		});
		UE_LOG(LogBF6HighPoly, Display,
			TEXT("  bind check: %d water material(s), %d still sampling an older "
			     "displacement texture%s"),
			Checked, Stale,
			Stale > 0 ? TEXT("  <== the screen is behind these numbers") : TEXT(""));

		// FULL CENSUS, not name-filtered.
		//
		// The line above only inspects components called Water_*, which is the
		// same filter the rebind uses - so if that filter misses a surface, the
		// rebind misses it too AND the check reports clean. Identify a water
		// material by whether it actually carries the BF6Disp0 parameter, walk
		// every mesh component in the world, and compare the two populations.
		// A gap here is the whole bug: a rebuild would update the data and leave
		// part of the surface drawing the previous spectrum until some other
		// parameter write happens to dirty it.
		if (UWorld* WorldPtr = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
		{
			UTexture* const Want = (UTexture*)GWaterFFT->DisplacementTexture(0);
			TSet<UMaterialInstanceDynamic*> SeenMids;
			int32 MeshComps = 0, WaterComps = 0, WaterCompsVisible = 0;
			int32 NameMatched = 0, MidsTotal = 0, MidsStale = 0;
			for (TActorIterator<AActor> It(WorldPtr); It; ++It)
			{
				TArray<UMeshComponent*> Comps;
				It->GetComponents<UMeshComponent>(Comps);
				for (UMeshComponent* C : Comps)
				{
					if (!C) continue;
					++MeshComps;
					bool bWater = false;
					for (int32 mi = 0; mi < C->GetNumMaterials(); ++mi)
					{
						UMaterialInstanceDynamic* MID =
							Cast<UMaterialInstanceDynamic>(C->GetMaterial(mi));
						if (!MID) continue;
						UTexture* Bound = nullptr;
						// Carrying this parameter is what MAKES it a water
						// material, independent of any naming convention.
						if (!MID->GetTextureParameterValue(TEXT("BF6Disp0"), Bound))
							continue;
						bWater = true;
						if (!SeenMids.Contains(MID))
						{
							SeenMids.Add(MID);
							++MidsTotal;
							if (Bound != Want) ++MidsStale;
						}
					}
					if (bWater)
					{
						++WaterComps;
						if (C->IsVisible()) ++WaterCompsVisible;
						if (C->GetName().StartsWith(TEXT("Water_"))) ++NameMatched;
					}
				}
			}
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  water census: %d mesh component(s) in world, %d carry a water "
				     "material (%d visible). Of those, %d match the Water_ name filter "
				     "the rebind uses%s"),
				MeshComps, WaterComps, WaterCompsVisible, NameMatched,
				(NameMatched < WaterComps)
					? TEXT("  <== THE REBIND IS MISSING SOME")
					: TEXT(""));
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  distinct water materials: %d, of which %d are sampling an older "
				     "displacement texture%s"),
				MidsTotal, MidsStale,
				MidsStale > 0 ? TEXT("  <== these draw the previous spectrum") : TEXT(""));
		}
		BF6_ReportSeaState();
	}));

static FAutoConsoleCommand GWaterWindResetCmd(
	TEXT("BF6.HighPoly.WaterWindReset"),
	TEXT("Return the ocean to the wind the level actually authors. This is the "
	     "negative control for the sea state: the surface must fall back to the "
	     "millimetre chop the level ships."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		BF6HP::FCore::SetWindOverride(-1.f, 1.f);
		BF6_ReportSeaState();
		BF6_RebuildWaterSpectrum();
	}));

static FAutoConsoleCommand GWaterAlbedoCmd(
    TEXT("BF6.HighPoly.WaterAlbedo"),
    TEXT("0..1: how much of the WEAKEST channel's extinction is scattering rather "
         "than absorption. Scattering is spectrally flat in clear water, so this is "
         "one number for all three channels and the colour comes entirely from the "
         "decoded extinction. Lower is deeper and more saturated, higher is brighter "
         "and milkier. The only number in the water that is not decoded. Rebuild to "
         "apply."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.WaterAlbedo <0..1>  (current %.2f)"),
            GWaterAlbedoScale); return; }
        GWaterAlbedoScale = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 1.f);
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("water albedo scale %.2f - rebuild the map to apply it"),
            GWaterAlbedoScale);
    }));

static FAutoConsoleCommand GWaterExtinctionCmd(
    TEXT("BF6.HighPoly.WaterExtinction"),
    TEXT("Scale on the mined extinction. How far you can see into water is "
         "exp(-extinction * depth), so 0.5 doubles the visible depth and 2 halves "
         "it. 1 is the value mined from the level."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.WaterExtinction <scale>")); return; }
        const float K = FMath::Max(1e-4f, FCString::Atof(*Args[0]));
        int32 n = 0;
        float HalfM = 0.f;
        for (const FWaterCoeff& C : GWaterCoeffs)
        {
            UMaterialInstanceDynamic* MID = C.MID.Get();
            if (!MID) continue;
            MID->SetVectorParameterValue(TEXT("Scattering"), C.Scatter * K);
            MID->SetVectorParameterValue(TEXT("Absorption"), C.Absorb * K);
            // Reported in metres, because a coefficient means nothing by eye
            // and a half-transmittance depth means everything.
            const float Ext = FMath::Max((C.Scatter.G + C.Absorb.G) * K, 1e-8f);
            HalfM = 0.6931f / Ext * 0.01f;
            n++;
        }
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("water extinction x%.3f on %d surface(s), green half-depth now %.2f m"),
            K, n, HalfM);
    }));

static FAutoConsoleCommand GSunIntensityCmd(
    TEXT("BF6.HighPoly.SunIntensity"),
    TEXT("Set the directional light intensity directly, to find the value that reads right."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1 || !GEditor) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.SunIntensity <value>")); return; }
        const float V = FCString::Atof(*Args[0]);
        UWorld* W = GEditor->GetEditorWorldContext().World();
        if (!W) return;
        int32 n = 0;
        for (TActorIterator<ADirectionalLight> It(W); It; ++It)
        {
            if (ULightComponent* LC = It->GetLightComponent())
            {
                LC->SetIntensity(V);
                LC->MarkRenderStateDirty();
                n++;
            }
        }
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("sun intensity set to %.2f on %d light(s)"), V, n);
    }));

// THE LOCAL LIGHT CEILING, live.
//
// The fleet authors up to 6e7 lumens, which is not a literal lamp, and the
// conversion the game uses from an authored intensity to renderer energy is
// not decoded. Until it is, the ceiling is a number somebody has to pick by
// looking, and picking it should not cost a rebuild each time.
//
// Reported alongside is how many lights the new ceiling actually binds, since
// a ceiling nothing reaches has no effect and a ceiling everything reaches has
// flattened the map to one brightness. Both look like "it did nothing".
static FAutoConsoleCommand GLightMaxCmd(
    TEXT("BF6.HighPoly.LightMax"),
    TEXT("Ceiling in lumens on the map's own lamps, re-applied to what is already "
         "built. The authored values run to 6e7, so this is what stops a handful "
         "of records washing out the map."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.LightMax <lumens>  (current %.0f)"),
            GLightLumenMax); return; }
        const float V = FMath::Max(0.f, FCString::Atof(*Args[0]));
        GLightLumenMax = V;
        int32 n = 0, Bound = 0, Gone = 0;
        for (const TPair<TWeakObjectPtr<ULightComponent>, float>& P : GAuthoredLights)
        {
            ULightComponent* LC = P.Key.Get();
            if (!LC) { Gone++; continue; }
			const float Lum = FMath::Min(P.Value, V) * GLightPhotometricScale;
            LC->SetIntensity(Lum);
            LC->MarkRenderStateDirty();
            if (P.Value > V) Bound++;
            n++;
        }
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("light ceiling %.0f lm on %d light(s), %d of them bound by it%s"),
            V, n, Bound,
            Gone > 0 ? TEXT(" (some lights no longer exist, rebuild to refresh)")
                     : TEXT(""));
    }));

static void BF6_CheckMaterials()
{
	{
		// bRecompile: whether to FORCE the compile before judging.
		//
		// ONLY THE TWO GROUND MATERIALS. RecompileMaterial takes an access
		// violation inside UnrealEditor-MaterialEditor on the others, which is
		// what the comment on EnsureParentMaterial has warned about since it
		// was first hit. Extending this check to all nine walked straight back
		// into it: three headless runs died at the same instruction, and the
		// water lines printed just before the crash came from
		// EnsureWaterMaterial reporting itself as the ARGUMENT, not from the
		// check having got that far.
		//
		// Everything else loses the forced compile and keeps the rest, which is
		// where the value is anyway. The shading model, the blend mode and
		// which pins are connected are all readable without asking for
		// shaders, and those are what the water bug actually turned on.
		auto Report = [](const TCHAR* Name, UMaterial* M, bool bRecompile = true)
		{
			if (!M || !IsValid(M))
			{
				UE_LOG(LogBF6HighPoly, Error, TEXT("CHECK %s: NOT BUILT"), Name);
				return;
			}

			// FORCE THE COMPILE, then prove it happened.
			//
			// An empty error list is NOT a pass. Material shaders compile
			// asynchronously, so reading GetCompileErrors on a material whose
			// shaders have not been built yet returns nothing and reads as a
			// clean bill of health. That is exactly what happened the first
			// time this ran: two materials "COMPILED CLEAN" in 24 milliseconds
			// with no shader compiler activity anywhere in the log. So the
			// recompile is explicit, the wait is explicit, and the verdict
			// reports the shader map and the elapsed time - a pass that took
			// no time is a pass that did nothing.
			const double T0 = FPlatformTime::Seconds();
			if (bRecompile)
			{
				UMaterialEditingLibrary::RecompileMaterial(M);
			}
			// WAIT EITHER WAY, and this is what makes the unforced case worth
			// reading. Building the material already called PostEditChange,
			// which QUEUES a compile; the check used to read the resource
			// before that finished and judge whatever shader map happened to
			// be lying there. Waiting costs nothing and means a translation
			// error actually lands in GetCompileErrors below.
			//
			// It is also the only force available for the materials
			// RecompileMaterial crashes on, which is all of them except the
			// two ground ones.
			FAssetCompilingManager::Get().FinishAllCompilation();
			const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;

			const FMaterialResource* R = M->GetMaterialResource(GMaxRHIShaderPlatform);
			if (!R)
			{
				UE_LOG(LogBF6HighPoly, Error, TEXT("CHECK %s: no material resource"), Name);
				return;
			}
			const TArray<FString>& Errs = R->GetCompileErrors();
			const bool bFinished = R->IsCompilationFinished();
			const bool bHaveMap = R->GetGameThreadShaderMap() != nullptr;

			// Said before the verdict, because a material that compiles
			// cleanly as the WRONG shading model is the failure that looks
			// most like success.
			BF6_ReportMaterialState(Name, M);
			if (Errs.Num() == 0)
			{
				if (!bRecompile && !bHaveMap)
				{
					// A PRESENT SHADER MAP IS NOT EVIDENCE HERE, and reporting
					// it as one hid a real failure.
					//
					// This branch used to say COMPILED CLEAN whenever a shader
					// map existed. The map can be a STALE one, built from the
					// PREVIOUS version of the material, so a graph that no
					// longer compiles still reports clean - which is exactly
					// what happened to the road material after a Custom node
					// gained an out-of-bounds swizzle. The engine failed it at
					// LogShaderCompilers and this check called it clean in the
					// same session.
					//
					// It is also incomplete by construction: the permutation
					// that failed was a Lumen translucency pass, which nothing
					// here would have asked for anyway.
					UE_LOG(LogBF6HighPoly, Display,
						TEXT("CHECK %s: graph built and readable, shaders NOT verified ")
						TEXT("(no forced recompile; shader map %s may be stale). ")
						TEXT("Search the log for 'Failed to compile Material' after a build."),
						Name, bHaveMap ? TEXT("present") : TEXT("absent"));
				}
				else if (bHaveMap && bFinished)
				{
					UE_LOG(LogBF6HighPoly, Display,
						TEXT("CHECK %s: COMPILED CLEAN (shader map present, %.0f ms)"), Name, Ms);
				}
				else
				{
					UE_LOG(LogBF6HighPoly, Warning,
						TEXT("CHECK %s: no errors, but NOTHING WAS COMPILED ")
						TEXT("(shader map %s, finished %s, %.0f ms) - this is not a pass"),
						Name, bHaveMap ? TEXT("present") : TEXT("ABSENT"),
						bFinished ? TEXT("yes") : TEXT("no"), Ms);
				}
				return;
			}
			UE_LOG(LogBF6HighPoly, Error, TEXT("CHECK %s: %d compile error(s)"), Name, Errs.Num());
			for (const FString& E : Errs)
			{
				UE_LOG(LogBF6HighPoly, Error, TEXT("    %s"), *E);
			}
		};
		// ALL OF THEM. This checked two materials out of nine, and water -
		// the one that has been wrong for days - was not among them. A check
		// with a hole in it is worse than no check, because the clean result
		// it prints is read as covering everything.
		Report(TEXT("ground blend"), EnsureGroundBlendMaterial());
		Report(TEXT("ground bake"), EnsureGroundMaterial());
		// -bf6matcheckforce adds RecompileMaterial on top of the wait.
		//
		// MEASURED TO CRASH: with this switch the run dies inside
		// UnrealEditor-MaterialEditor on the water material, immediately after
		// the two ground ones pass. So it is off by default and is not the way
		// to verify a graph. The WAIT in Report does that instead, and it is
		// unconditional. This stays only because a headless crash is free and
		// pinning which material kills it is occasionally worth knowing.
		const bool bForce = FParse::Param(FCommandLine::Get(), TEXT("bf6matcheckforce"));
		UMaterial* WaterCheck = EnsureWaterMaterial();
		// The ocean is drawn by HISM. Checking only the base material shader
		// missed a real custom-HLSL error when Unreal first requested the
		// FLocalVertexFactory/InstancedStaticMeshes permutation at runtime.
		if (WaterCheck) WaterCheck->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		Report(TEXT("water"), WaterCheck, bForce);
		Report(TEXT("sky"), EnsureSkyMaterial(), bForce);
		Report(TEXT("cloud shadow"), EnsureCloudShadowMaterial(), bForce);
		for (int32 k = 0; k < (int32)EKind::Count; k++)
		{
			const EKind Kind = (EKind)k;
			const TCHAR* KindName =
				Kind == EKind::Masked ? TEXT("masked")
				: Kind == EKind::MaskedVeg ? TEXT("masked vegetation")
				: Kind == EKind::Translucent ? TEXT("translucent")
				: Kind == EKind::Road ? TEXT("road")
				: Kind == EKind::Vista ? TEXT("vista") : TEXT("opaque");
			Report(KindName, EnsureParentMaterial(Kind), bForce);
		}
	}
}

bool BF6HighPolyControlBridge::StartFullBuild()
{
	// GIsRunning is the engine-wide "editor is running" flag, not this add-on's
	// build state.  Testing it here made every MCP request a permanent no-op.
	// A fresh editor is intentionally parked in the SDK's Workspace selector,
	// so deterministic water work must first take the normal map-card path into
	// the existing MP_Isolated/Water session. OpenMap is the SDK's public
	// automation seam and performs the same validation, context load, session
	// restore and map-open broadcasts as the UI.
	if (!GEditor) return false;
	const bool bOpeningMap = BF6Ext::CurrentLevel().IsEmpty();
	for (int32 i = 0; i < (int32)ELayer::Count; ++i) GLayers[i].bOn = true;
	if (bOpeningMap)
	{
		BF6Ext::OpenMap(TEXT("MP_Isolated"), TEXT("Water"));
		if (BF6Ext::CurrentLevel() != TEXT("MP_Isolated")) return false;
		// OpenMap synchronously broadcasts OnMapOpened.  That delegate only
		// retracts the previous map's caches; it does not start a build.  The old
		// MCP bridge returned here and therefore needed a misleading second call
		// before anything was read.  Continue into the one authoritative build
		// path after the synchronous map open.
	}
	// Include the button's mode and placed-object preparation, so the build
	// progress and the resulting viewport match an interactive build.
	BuildAndShow();
	return true;
}

bool BF6HighPolyTerrainBridge::StartAtVisibleCamera(float SpanMetres, int32 Resolution)
{
	FVector CameraCm;
	if (!GetWaterViewLocation(CameraCm))
	{
		GExactPageStatus = TEXT("failed: no visible level viewport camera");
		return false;
	}
	if (GExactPageBusy || !GroundMat || BF6Ext::CurrentLevel().IsEmpty())
	{
		GExactPageStatus = GExactPageBusy
			? TEXT("busy evaluating the previous page")
			: TEXT("failed: build terrain before requesting an exact page");
		return false;
	}
	StartExactPage((float)(CameraCm.X * 0.01), (float)(CameraCm.Y * 0.01),
		SpanMetres, Resolution);
	return GExactPageBusy;
}

void BF6HighPolyTerrainBridge::Disable()
{
	DisableExactPage();
}

FString BF6HighPolyTerrainBridge::Status()
{
	return GExactPageStatus;
}

void BF6WaterShared::InvalidateMaterialGraph()
{
	if (GWaterParent && GWaterParent->IsRooted())
		GWaterParent->RemoveFromRoot();
	GWaterParent = nullptr;
}

void BF6WaterShared::ApplyStableReflectionPolicy()
{
	ApplyStableWaterReflectionPolicy();
}

void BF6WaterShared::SetWaterMask(const BF6HP::FCore::FWaterMask* Mask)
{
	if (GWaterMaskAtlas)
	{
		GWaterMaskAtlas->RemoveFromRoot();
		GWaterMaskAtlas = nullptr;
	}
	if (GWaterMaskIndirection)
	{
		GWaterMaskIndirection->RemoveFromRoot();
		GWaterMaskIndirection = nullptr;
	}
	GWaterMaskMin = FVector2D::ZeroVector;
	GWaterMaskSpan = FVector2D(1, 1);
	GWaterMaskMeta = FVector4f(1, 0, 1, 1);
	GWaterMaskPageCount = 0;
	if (Mask)
		MakeWaterMaskTextures(*Mask);
}

void BF6WaterShared::SetWaterHeightfield(const BF6HP::FCore::FTerrain* Heightfield)
{
	if (GWaterHeightTex)
	{
		GWaterHeightTex->RemoveFromRoot();
		// Never force a texture to garbage while an old MID/render proxy may still
		// reference it. Removing our root is sufficient: Unreal will collect it
		// only after every material reference has retired.
		GWaterHeightTex = nullptr;
	}
	GWaterHeightMin = FVector2D::ZeroVector;
	GWaterHeightSpan = FVector2D(1, 1);
	GWaterHeightTexelM = FVector2D(1, 1);
	if (!Heightfield) return;

	GWaterHeightTex = MakeWaterHeightTexture(*Heightfield);
	double MinPositive = DBL_MAX, Max = 0.0;
	int64 Positive = 0;
	const double Scale = Heightfield->HeightScale > 0.f
		? (double)Heightfield->HeightScale / 65536.0 : 0.0;
	for (uint16 V : Heightfield->Heights)
	{
		if (!V) continue;
		const double H = V * Scale;
		MinPositive = FMath::Min(MinPositive, H);
		Max = FMath::Max(Max, H);
		Positive++;
	}
	UE_LOG(LogBF6HighPoly, Log,
		TEXT("water block2: raw %dx%d -> %s, positive %.2f%%, absolute-Y %.3f..%.3f m"),
		Heightfield->Size, Heightfield->Size,
		GWaterHeightTex ? TEXT("GPU heightfield") : TEXT("UPLOAD FAILED"),
		Heightfield->Heights.IsEmpty() ? 0.0 :
			100.0 * (double)Positive / (double)Heightfield->Heights.Num(),
		MinPositive == DBL_MAX ? 0.0 : MinPositive, Max);
}

void BF6WaterShared::BindWaterHeightfield(UMaterialInstanceDynamic* Material)
{
	if (!Material) return;
	Material->SetScalarParameterValue(TEXT("BF6WaterHeightAvailable"),
		GWaterHeightTex ? 1.f : 0.f);
	if (!GWaterHeightTex) return;
	Material->SetTextureParameterValue(TEXT("BF6WaterHeightTex"), GWaterHeightTex);
	Material->SetVectorParameterValue(TEXT("BF6WaterHeightMin"), FLinearColor(
		(float)GWaterHeightMin.X, (float)GWaterHeightMin.Y, 0.f, 0.f));
	Material->SetVectorParameterValue(TEXT("BF6WaterHeightSpan"), FLinearColor(
		(float)GWaterHeightSpan.X, (float)GWaterHeightSpan.Y, 1.f, 1.f));
	Material->SetVectorParameterValue(TEXT("BF6WaterHeightTexelM"), FLinearColor(
		(float)GWaterHeightTexelM.X, (float)GWaterHeightTexelM.Y, 0.f, 0.f));
}

UMaterialInstanceDynamic* BF6WaterShared::CreateMaterial(UObject* Outer,
	BF6HP::FCore& Core, BF6HP::FWaterFFT& FFT,
	const BF6HP::FCore::FWater& Water)
{
	return WaterMaterialFor(Outer, Water, nullptr, &Core, &FFT);
}

static FAutoConsoleCommand GCheckMaterialsCmd(
	TEXT("BF6.HighPoly.CheckMaterials"),
	TEXT("Build the High Poly ground materials and log whether they compiled."),
	FConsoleCommandDelegate::CreateStatic(&BF6_CheckMaterials));

// Reset when the EDITOR opens a map, not when we next happen to build.
//
// ReadLevel already drops stale caches when it notices the level changed, but
// that only fires if the user builds again. Everything between opening the map
// and that build ran on the previous map's state.
// =============================================================================
// Object library previews: what BF6HighPolyPreviews.cpp needs from this file.
// Declared in BF6HighPolyPreviews.h. Lives here because GCore, the detail
// mode, the clay skin, the install choice and the mesh builder are file-local
// on purpose, and a bridge beside them is smaller than moving any of them.
// =============================================================================
namespace BF6HP
{
	UStaticMesh* BuildGameMesh(const FString& MeshRes, UObject* Outer)
	{
		if (!GCore.IsOpen() || MeshRes.IsEmpty()) return nullptr;
		TArray<BF6HP::FCore::FSection> Sections;
		if (!GCore.ReadMesh(MeshRes, Sections) || Sections.Num() == 0) return nullptr;
		FMeshDescription MD;
		int32 Tris = 0;
		if (!DescribeMesh(Sections, MD, Tris)) return nullptr;
		// READY TO RENDER. A Nanite mesh is finished later by a batch build, and
		// a caller of this function draws the mesh the same tick it gets it, so
		// the runtime render path is taken whatever the ring's NANITE says.
		const bool bNaniteWas = GNanite;
		GNanite = false;
		bool bNeedsBatchBuild = false;
		UObject* Where = Outer ? Outer : GetTransientPackage();
		const FString Name = MakeUniqueObjectName(Where, UStaticMesh::StaticClass(),
			FName(*(TEXT("HP_") + FPaths::GetBaseFilename(MeshRes)))).ToString();
		UStaticMesh* SM = MakeMesh(Where, Name, MD, Tris, Sections, bNeedsBatchBuild);
		GNanite = bNaniteWas;
		return SM;
	}

	bool PreviewsPrepareCache()
	{
		const FString Install = EnsureInstall();
		if (Install.IsEmpty()) return false;
		BF6HP::DiskCache::Configure(Install, CacheDir());
		return !BF6HP::DiskCache::InstallSignature().IsEmpty();
	}

	// The same opening StartRead performs, minus the level. Safe on a worker:
	// the core touches no UObjects opening an install, and the disk cache is
	// locked. EnsureInstall is a read of a session string the game thread
	// wrote before the build started.
	// Serialises opening AND mounting. One lock rather than two, because both
	// guard the same thing: GCore. Two locks would let a thread mount while
	// another opens, which is the same crash wearing a different hat.
	// WHERE bf6_core.dll ACTUALLY IS, PACKAGED OR IN DEVELOPMENT.
	//
	// Three call sites each built the same path into the SOURCE tree:
	//   Source/ThirdParty/libbf6/bin/Win64/bf6_core.dll
	// which exists on this machine and in no installed copy of the plugin. A
	// packaged build stages binaries into Binaries/Win64 and ships no Source
	// tree at all, so every high poly read would fail for anybody who installed
	// the plugin rather than building it, with an error naming a path that was
	// never going to be there.
	//
	// Staged first, development second, and the error names both so the answer
	// to "it says it cannot find the core" is in the message.
	// The mounting callers used to share a lock of their own while the rest of
	// the add-on used none. There is one context, so there is one lock: this is
	// now an alias for it, and every native caller takes the same one.
	#define GCoreMountCS (BF6HP::Shared::CoreMutex())

	bool PreviewsEnsureCore(FString& OutError)
	{
		const FString Install = EnsureInstall();
		if (Install.IsEmpty()) { OutError = TEXT("no game folder chosen"); return false; }
		BF6HP::DiskCache::Configure(Install, CacheDir());

		// Same race as the mount below, and it happens FIRST: two workers both
		// see a closed core and both call Open on a 176 MB executable. Checked
		// under the lock so only one opens and the other waits for it.
		FScopeLock Lock(&GCoreMountCS);
		if (GCore.IsOpen()) return true;
		const FString Dll = BF6HP::CoreDllPath();
		if (!GCore.Open(Install, Dll)) { OutError = GCore.Error; return false; }
		return true;
	}

	// Off by default: the previews and the SFX auditions share the reader the
	// open map is drawn through, and widening it for a thumbnail is both slow
	// and outside the scope the creator asked for.
	bool GPreviewsFullCatalogue = false;

	bool PreviewsMountCatalogue(FString& OutError)
	{
		// Every level's archives, as the Godot plugin's mount_rest does: a
		// placeable's members can live in any level's bundle, so mounting only
		// the open level leaves most of the catalogue unresolvable.
		//
		// ONE THREAD AT A TIME, AND THE CHECK INSIDE THE LOCK.
		//
		// This was a bare `static bool bMounted` tested before the work and set
		// after it, called from TWO different background paths: the icon
		// builder on EAsyncExecution::Thread and the SFX preview on
		// EAsyncExecution::ThreadPool. Play a sound while previews are building
		// and both threads see false, both enter MountAll, and the second one
		// walks state the first is still constructing. That is an access
		// violation inside bf6_core with a stack that blames whichever caller
		// happened to be second, which is exactly the crash reported after
		// pressing play on an SFX.
		//
		// C++ guarantees a function-local static is INITIALISED once. It says
		// nothing about a read-modify-write of the flag afterwards, so the
		// idiom looks safe and is not.
		//
		// The second thread now waits for the first to finish and then sees the
		// mount is done. Waiting is correct rather than merely safe: it needed
		// the catalogue anyway, and the mount is the better part of a minute
		// cold, so racing it was never going to be faster.
		FScopeLock Lock(&GCoreMountCS);
		static bool bMounted = false;
		// The narrow mount is per level, so remembering "mounted" as one bool
		// would carry a claim about MP_Aftermath into MP_Isolated.
		static FString MountedLevel;
		if (bMounted) return true;
		if (!GCore.IsOpen()) { OutError = TEXT("no install open"); return false; }

		// THE MAP THE CREATOR HAS OPEN IS THE SCOPE, HERE TOO.
		//
		// This is the reader that renders their map. Mounting every level's
		// archives to draw a shelf thumbnail or audition a sound widened the
		// scope of the ONE context the placed objects and the scenery are also
		// reading through, which is both slow (measured at 20.4 seconds) and
		// wrong: an object found in another map's archives is not an object
		// this map may use.
		//
		// So the side paths do not widen it. A thumbnail or a sound that needs
		// something outside this map's archives goes without, and says so, and
		// somebody who genuinely wants to browse the whole install turns it on
		// deliberately with BF6.HighPoly.Previews.FullCatalogue 1.
		// THIS MAP'S ARCHIVES ARE THE ANSWER, NOT A REASON TO REFUSE.
		//
		// This used to return an ERROR whenever the full catalogue was off,
		// which is the default - so pressing PREVIEWS did nothing at all except
		// print a line about a console command, and the object shelf stayed a
		// mix of real pictures and blockouts forever.
		//
		// The scope was right and the conclusion was wrong. A creator can only
		// place what this map offers, so pictures for every other map's objects
		// are work nobody can use. What they need is this map's archives, which
		// is a two second narrow mount rather than a twenty second whole-install
		// one, and anything that genuinely lives elsewhere simply goes without a
		// picture and says so.
		if (!GPreviewsFullCatalogue)
		{
			const FString Level = BF6Ext::CurrentLevel();
			if (Level.IsEmpty())
			{
				OutError = TEXT("no map is open, so there is nothing to draw pictures of");
				return false;
			}
			if (MountedLevel == Level) { return true; }

			// SHARED FIRST, THEN THIS MAP. TWO MOUNTS, AND NEITHER IS THE GAME.
			//
			// Plenty of props are not a level's at all: the crates, signs and
			// street furniture that appear on every map live in the common
			// archives, and a level mount alone leaves those unresolvable and
			// their shelf entries grey. bf6_mount_all with include_levels = 0
			// is exactly that shared set - every non-level family, none of the
			// maps - so the two together give this map plus everything it
			// shares, and still nothing belonging to another map.
			//
			// Both are idempotent in the core and first-mount-wins, so neither
			// can narrow or disturb what the map renderer is already reading.
			static bool bSharedMounted = false;
			if (!bSharedMounted)
			{
				if (GCore.MountAll(/*bIncludeLevels*/ false)) { bSharedMounted = true; }
				else
				{
					UE_LOG(LogBF6HighPoly, Log,
						TEXT("previews: the shared archives could not be mounted (%s); objects that "
						     "live outside this map go without a picture"), *GCore.Error);
				}
			}
			if (!GCore.MountLevelArchives(Level))
			{
				// Not fatal on its own: the placed-object resolver mounts the
				// same archives for the same map, so this may already be done.
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("previews: %s's archives were not mounted here (%s); using whatever is "
					     "already mounted"), *Level, *GCore.Error);
			}
			MountedLevel = Level;
			return true;
		}
		UE_LOG(LogBF6HighPoly, Warning,
			TEXT("previews: mounting EVERY level's archives because BF6.HighPoly.Previews.FullCatalogue is on. "
			     "This widens the reader the open map is drawn through."));
		if (!GCore.MountAll(true)) { OutError = GCore.Error; return false; }
		bMounted = true;
		return true;
	}

	bool& PreviewsFullCatalogueFlag() { return GPreviewsFullCatalogue; }

	bool PreviewsIsClay() { return GMode == EMode::Clay; }
	UMaterialInterface* PreviewsClayMaterial() { return ClayMaterial(); }
	int32 PreviewsMode() { return (int32)GMode; }

	// THE NAME LAW, ported from the Godot plugin's _resolve_object.
	//
	// Every SDK placeable has a matching pf_portal_<name> prefab under
	// glacierportal/modbuilder: the authoritative list of member meshes and
	// their transforms, often nesting sub-prefabs, which is what makes the
	// composite objects work. Surveyed there over 4,620 objects: 4,290 resolve
	// directly, 109 have no game asset at all (spawners, capture points -
	// gameplay logic, correctly absent), 221 resolve only with the "_a" suffix
	// (lettered variants that occupy the same space, so either is the whole
	// thing), and a further 16 carry an art-category prefix the SDK name drops
	// (com_, mil_, fed_ ...), taken only when exactly one category has it so
	// this never becomes fuzzy matching.
	bool PortalObjectParts(const FString& Type, TArray<FObjectPart>& Out, FString& OutError)
	{
		Out.Reset();
		OutError.Reset();
		if (!GCore.IsOpen()) { OutError = TEXT("no install open"); return false; }

		// The pf_portal_ family indexed once per session: leaf (lower) -> name.
		static TMap<FString, FString> Index;
		static bool bIndexed = false;
		auto LeafOf = [](const FString& N)
		{
			FString Leaf = FPaths::GetCleanFilename(N);
			if (Leaf.EndsWith(TEXT(".ebx"), ESearchCase::IgnoreCase)) Leaf.LeftChopInline(4);
			return Leaf;
		};
		if (!bIndexed)
		{
			TArray<FString> Names;
			if (!GCore.ListEbx(TEXT("pf_portal_"), Names)) { OutError = GCore.Error; return false; }
			for (const FString& N : Names) Index.Add(LeafOf(N).ToLower(), N);
			bIndexed = true;
			UE_LOG(LogBF6HighPoly, Log, TEXT("previews: %d pf_portal_ prefab(s) indexed"), Index.Num());
		}

		const FString Key = Type.ToLower();
		// A bare name is not a pf_portal_ name, so it is looked up over the
		// whole mount and taken only when exactly one partition has that leaf.
		auto Bare = [&](const FString& Leaf, FString& Found) -> bool
		{
			TArray<FString> Hits;
			if (!GCore.ListEbx(Leaf, Hits)) return false;
			int32 Count = 0;
			for (const FString& H : Hits)
				if (LeafOf(H).Equals(Leaf, ESearchCase::IgnoreCase)) { Count++; Found = H; }
			return Count == 1;
		};

		FString Asset;
		if (const FString* F = Index.Find(TEXT("pf_portal_") + Key)) Asset = *F;
		else if (Bare(Key, Asset)) {}
		else if (const FString* F2 = Index.Find(TEXT("pf_portal_") + Key + TEXT("_a"))) Asset = *F2;
		else
		{
			static const TCHAR* Prefixes[] = { TEXT("com_"), TEXT("mil_"), TEXT("fed_"), TEXT("naf_"),
				TEXT("cas_"), TEXT("ind_"), TEXT("psd_"), TEXT("ter_"), TEXT("veg_"), TEXT("ob_") };
			TArray<FString> Found;
			for (const TCHAR* Pre : Prefixes)
			{
				if (const FString* F3 = Index.Find(TEXT("pf_portal_") + FString(Pre) + Key)) { Found.Add(*F3); continue; }
				if (const FString* F4 = Index.Find(TEXT("pf_portal_") + FString(Pre) + Key + TEXT("_a"))) Found.Add(*F4);
			}
			if (Found.Num() == 1) Asset = Found[0];
		}
		if (Asset.IsEmpty()) return true;   // no geometry for this type; not an error

		TArray<BF6HP::FPlacement> Rows;
		if (!GCore.AssetInstances(Asset, Rows)) { OutError = GCore.Error; return false; }
		Out.Reserve(Rows.Num());
		for (const BF6HP::FPlacement& p : Rows)
		{
			FObjectPart P;
			P.MeshRes   = BF6HP::FCore::MeshResourceFor(p.Mesh);
			P.Bundle    = p.Bundle;
			P.Variation = p.Variation;
			// The conversion the level build applies to an instance: the basis
			// crosses as a matrix with Y and Z swapped, and the columns are
			// reordered the same way so geometry and placement stay in step.
			const FVector X(p.Right.X,   p.Right.Z,   p.Right.Y);
			const FVector Y(p.Up.X,      p.Up.Z,      p.Up.Y);
			const FVector Z(p.Forward.X, p.Forward.Z, p.Forward.Y);
			P.LocalToObject = FTransform(FMatrix(X, Z, Y, ToUnreal(p.Origin)));
			Out.Add(MoveTemp(P));
		}
		return true;
	}
}

// ---- the seam to the other translation units (BF6HighPolyShared.h) ----------
//
// Defined here, at file scope after the anonymous namespace, because these are
// the only names the placed-object module needs from it: the open core, the
// mode, the mesh and light constructors, the study-grey material. Everything
// else stays private to this file.
namespace BF6HP
{
namespace Shared
{
	static bool GCoreBusyExternal = false;

	FCore&   Core()        { return GCore; }
	bf6_ctx* CoreContext() { return GCore.Handle(); }
	void*    CoreDll()     { return GCore.DllHandle(); }

	bool EnsureCoreOpen(FString& OutWhy)
	{
		const FString Install = EnsureInstall();
		if (Install.IsEmpty())
		{
			OutWhy = TEXT("no game folder chosen; open HIGH POLY > PANEL to set it");
			return false;
		}
		if (GCore.IsOpen()) return true;
		const FString Dll = BF6HP::CoreDllPath();
		BF6HP::DiskCache::Configure(Install, CacheDir());
		if (!GCore.Open(Install, Dll)) { OutWhy = GCore.Error; return false; }
		return true;
	}

	FString InstallExePath()
	{
		const FString Install = EnsureInstall();
		return Install.IsEmpty() ? FString() : FPaths::Combine(Install, TEXT("bf6.exe"));
	}

	// Asked from worker-spawning paths on the game thread and latched from
	// ShutdownModule, so it is atomic rather than a plain bool. It never clears.
	static std::atomic<bool> GCoreShuttingDown{ false };

	void MakeUnselectable(UPrimitiveComponent* C)
	{
		if (!C) return;
		// bSelectable is what the editor's hit proxies read, so the click
		// passes straight through to whatever the add-on is standing in for.
		C->bSelectable = false;
		// And the overlay never draws a selection outline of its own: with the
		// component unselectable the outline would otherwise be drawn for the
		// owning actor and look like the overlay was picked after all.
		C->SetRenderCustomDepth(false);
	}

	bool  IsBuilding()            { return GBuildQueuedOrRunning; }
	void  SetCoreBusy(bool bBusy) { GCoreBusyExternal = bBusy; }
	// The one lock for the one context. Function-local so it is constructed
	// before any caller can reach it, whatever order the modules start in.
	FCriticalSection& CoreMutex()
	{
		static FCriticalSection GCoreCS;
		return GCoreCS;
	}

	// The scenery build's own cached read, offered to the placed module. The
	// "shared" scope is deliberate: a member mesh belongs to an asset, not to a
	// level, and the same crate on four maps should be decoded once.
	bool CacheReadMesh(const FString& ResName, const FString& Bundle,
	                   const FString& Variation, TArray<FCore::FSection>& Out,
	                   bool& bOutFromCache)
	{
		return CachedReadMesh(TEXT("shared"), ResName, Bundle, Variation, Out, bOutFromCache);
	}

	EReadResult CacheReadMeshNoWait(const FString& ResName, const FString& Bundle,
	                                const FString& Variation, TArray<FCore::FSection>& Out,
	                                bool& bOutFromCache)
	{
		return CachedReadMeshImpl(TEXT("shared"), ResName, Bundle, Variation, Out,
			bOutFromCache, /*bNoWait*/ true);
	}

	static std::atomic<bool> GGeometryPriority{ false };
	void SetGeometryPriority(bool bOn) { GGeometryPriority.store(bOn); }
	bool GeometryHasPriority()         { return GGeometryPriority.load() || GBuildQueuedOrRunning.load(); }

	FCoreTryLease::FCoreTryLease()  { bHeld = CoreMutex().TryLock(); }
	FCoreTryLease::~FCoreTryLease() { if (bHeld) { CoreMutex().Unlock(); } }

	bool CacheHasMesh(const FString& ResName, const FString& Bundle, const FString& Variation)
	{
		// IS IT THERE, not GIVE IT TO ME.
		//
		// This asked LoadPacked, which reads the file and decompresses the whole
		// mesh - to answer a yes/no question, for every member of every type,
		// immediately before the resolve read and decompressed exactly the same
		// blobs again. Measured: about thirty seconds of a warm scene spent
		// unpacking meshes twice.
		//
		// The file existing with the right name under the install signature is
		// the answer. A corrupt or stale blob still fails later, on the read
		// that actually wants it, and falls back to the reader.
		const FString Path = BF6HP::DiskCache::PathFor(TEXT("shared"),
			MeshCacheName(ResName, Bundle, Variation));
		return FPaths::FileExists(Path);
	}

	bool CacheLoadBlob(const FString& Name, uint32 Version, TArray<uint8>& Out)
	{
		return BF6HP::DiskCache::LoadPacked(TEXT("shared"), Name, Version, Out);
	}

	void CacheSaveBlob(const FString& Name, uint32 Version, TArray<uint8>&& Blob)
	{
		BF6HP::DiskCache::SaveAsync(TEXT("shared"), Name, Version, MoveTemp(Blob), /*bPack*/ true);
	}
	bool  CoreBusy()              { return GCoreBusyExternal; }
	void  BeginCoreShutdown()     { GCoreShuttingDown.store(true); }
	bool  CoreShuttingDown()      { return GCoreShuttingDown.load(); }
	bool  BuiltAnything()         { return GBuiltAnything; }
	int32 Mode()                  { return (int32)GMode; }

	UMaterialInterface* ClayMaterial() { return ::ClayMaterial(); }

	// WHICH THIRD OF THE MESH BUILD IS THE EXPENSIVE ONE.
	//
	// 23.4 seconds of a scene goes through here and the three halves have very
	// different futures: describing geometry and building render buffers are
	// worker-safe and can go wide, while NewObject and the material slots are
	// game-thread only and no amount of threads will move them. Parallelising
	// the wrong one buys nothing, so they are counted separately.
	// Describing and committing run on workers, so those two are nanoseconds
	// under an interlocked add - a plain double += from sixteen threads loses
	// most of what it is meant to be counting. The other two are game thread.
	int64  GPlacedDescribeNanos = 0, GPlacedRenderNanos = 0;
	double GPlacedObjectSecs = 0.0, GPlacedMaterialSecs = 0.0;

	void BuildGameMeshCost(double& OutDescribe, double& OutObject, double& OutMaterial,
	                       double& OutRender)
	{
		OutDescribe = (double)FPlatformAtomics::AtomicRead(&GPlacedDescribeNanos) * 1e-9;
		OutObject   = GPlacedObjectSecs;
		OutMaterial = GPlacedMaterialSecs;
		OutRender   = (double)FPlatformAtomics::AtomicRead(&GPlacedRenderNanos) * 1e-9;
	}

	void ResetBuildGameMeshCost()
	{
		FPlatformAtomics::InterlockedExchange(&GPlacedDescribeNanos, 0);
		FPlatformAtomics::InterlockedExchange(&GPlacedRenderNanos, 0);
		GPlacedObjectSecs = GPlacedMaterialSecs = 0.0;
	}

	int32 WarmParentMaterials()
	{
		// THE SEVEN THE GEOMETRY ACTUALLY USES.
		//
		// Not all sixteen permutations: Road belongs to the road pass and builds
		// itself there, and the vegetation and vista kinds have no emissive
		// form in anything measured. These are the ones a scene of placed props
		// and scenery was observed to ask for, so these are the ones warmed.
		static const TPair<EKind, bool> Wanted[] = {
			{ EKind::Opaque,      false }, { EKind::Opaque,      true  },
			{ EKind::Masked,      false }, { EKind::Masked,      true  },
			{ EKind::MaskedVeg,   false },
			{ EKind::Translucent, false },
			{ EKind::Decal,       false },
			{ EKind::Puddle,      false },
			{ EKind::Vista,       false },
		};
		int32 Built = 0;
		const double T0 = FPlatformTime::Seconds();
		for (const TPair<EKind, bool>& W : Wanted)
		{
			UMaterial*& Slot = W.Value ? GEmissiveParents[(int32)W.Key] : GParents[(int32)W.Key];
			if (Slot) { continue; }
			if (EnsureParentMaterial(W.Key, W.Value)) { Built++; }
		}
		if (Built > 0)
		{
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("parent materials: %d built up front in %.1f s, so a level open does not "
				     "discover them one at a time"),
				Built, FPlatformTime::Seconds() - T0);
		}
		return Built;
	}

	void MaterialCost(double& OutParents, double& OutKeys, double& OutInstances,
	                  double& OutGlow, int32& OutGlowMisses,
	                  int32& OutHits, int32& OutMisses)
	{
		OutParents   = GSecParentMaterials;
		OutKeys      = GSecMaterialKey;
		OutGlow      = GSecGlowFacts;
		OutGlowMisses = GGlowFactsMisses;
		// GSecMidBuild is measured from the same start as the parent build, so
		// the parents' seconds are inside it. Report what making the instances
		// cost on its own.
		OutInstances = FMath::Max(0.0, GSecMidBuild - GSecParentMaterials);
		OutHits      = GMaterialCacheHits;
		OutMisses    = GMaterialCacheMisses;
	}

	// The description this build is carrying between its three stages.
	struct FMeshWork
	{
		FMeshDescription MD;
		int32 Tris = 0;
	};

	TSharedPtr<FMeshWork> DescribeGameMesh(const TArray<FCore::FSection>& Sections)
	{
		const double T0 = FPlatformTime::Seconds();
		TSharedPtr<FMeshWork> Work = MakeShared<FMeshWork>();
		const bool bOk = DescribeMesh(Sections, Work->MD, Work->Tris);
		// Written from workers, so the accumulate is atomic rather than a
		// read-modify-write race that quietly loses seconds.
		FPlatformAtomics::InterlockedAdd(
			&GPlacedDescribeNanos,
			(int64)((FPlatformTime::Seconds() - T0) * 1e9));
		return bOk ? Work : nullptr;
	}

	UStaticMesh* CreateGameMeshObject(const FString& Name,
	                                  const TArray<FCore::FSection>& Sections,
	                                  const TSharedPtr<FMeshWork>& Work)
	{
		check(IsInGameThread());
		if (!Work) { return nullptr; }
		const double ObjectStart = FPlatformTime::Seconds();
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), *Name), RF_Transient);
		if (!Mesh) { return nullptr; }
		// THE BODY SETUP HAS TO EXIST BEFORE A WORKER TOUCHES THIS.
		//
		// BuildFromMeshDescriptions makes one itself when there is none, and
		// that is a NewObject - illegal from the thread pool. The scenery batch
		// has always created it here for exactly that reason.
		Mesh->CreateBodySetup();
		GPlacedObjectSecs += FPlatformTime::Seconds() - ObjectStart;

		const double MaterialStart = FPlatformTime::Seconds();
		for (int32 si = 0; si < Sections.Num(); si++)
		{
			FStaticMaterial SM;
			SM.MaterialSlotName = FName(*FString::Printf(TEXT("S%d"), si));
			SM.ImportedMaterialSlotName = SM.MaterialSlotName;
			SM.MaterialInterface = MaterialFor(Mesh, Sections[si]);
			Mesh->GetStaticMaterials().Add(SM);
		}
		if (Mesh->GetStaticMaterials().Num() == 0)
			Mesh->GetStaticMaterials().Add(FStaticMaterial());
		GPlacedMaterialSecs += FPlatformTime::Seconds() - MaterialStart;
		return Mesh;
	}

	bool CommitGameMesh(UStaticMesh* Mesh, const TSharedPtr<FMeshWork>& Work, int32& OutTris)
	{
		OutTris = 0;
		if (!Mesh || !Work) { return false; }
		const double T0 = FPlatformTime::Seconds();
		const bool bOk = PrepareRuntimeRenderMesh(Mesh, Work->MD, Work->Tris);
		FPlatformAtomics::InterlockedAdd(
			&GPlacedRenderNanos,
			(int64)((FPlatformTime::Seconds() - T0) * 1e9));
		if (!bOk) { return false; }
		OutTris = Work->Tris;
		return true;
	}

	UStaticMesh* BuildGameMesh(const FString& Name, const TArray<FCore::FSection>& Sections,
	                           int32& OutTris)
	{
		// THE SAME THREE STAGES, RUN IN A ROW. One implementation, so the serial
		// caller and the batched one can never drift apart.
		OutTris = 0;
		TSharedPtr<FMeshWork> Work = DescribeGameMesh(Sections);
		if (!Work)
		{
			UE_LOG(LogBF6HighPoly, Warning, TEXT("placed mesh %s: no describable geometry"), *Name);
			return nullptr;
		}
		UStaticMesh* Mesh = CreateGameMeshObject(Name, Sections, Work);
		if (!Mesh) { return nullptr; }
		// The runtime render path: drawable the moment this returns, no batch
		// build to wait for. A placed object is one draw per placement, not an
		// instanced field, so Nanite's hierarchy buys it little.
		int32 Tris = 0;
		if (!CommitGameMesh(Mesh, Work, Tris))
		{
			UE_LOG(LogBF6HighPoly, Error, TEXT("placed mesh %s: runtime render build failed"), *Name);
			return nullptr;
		}
		OutTris = Tris;
		return Mesh;
	}

	ULightComponent* MakeAssetLight(AActor* Owner, USceneComponent* Parent,
	                                const FCore::FLight& L, int32 Index,
	                                const TCHAR* NamePrefix, float& OutAuthoredLm)
	{
		bool bDark = false;
		return MakeLightFrom(Owner, Parent, L, Index, NamePrefix, /*bRelative*/ true,
		                     OutAuthoredLm, bDark);
	}

	int32 SetMapLocalLightsVisible(bool bOn)
	{
		int32 n = 0;
		for (const TPair<TWeakObjectPtr<ULightComponent>, float>& P : GAuthoredLights)
		{
			if (ULightComponent* LC = P.Key.Get())
			{
				LC->SetVisibility(bOn, false);
				n++;
			}
		}
		return n;
	}
}
}

FDelegateHandle GMapOpenedHandle;
FDelegateHandle GMapClosingHandle;

void FBF6HighPolyModule::StartupModule()
{
	BF6HP::Loadout::Start();
	// NO SHADER DIRECTORY MAPPING HERE. It would be pointless and misleading:
	// this module cannot own shader types at all. Registering one from here
	// fails during DLL static init, because the plugin loads after the global
	// shader map is built. See BF6HighPolyWaterGPU.cpp - the mapping belongs
	// with the shader types, in a PostConfigInit module.

	GWaterClipmapTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&TickWaterClipmaps), 1.0f / 30.0f);

	// ---- WHEN THE EDITOR HITCHES, AND WHAT WAS RUNNING ---------------------
	//
	// "It stutters" cannot be argued with and cannot be fixed either. This
	// watches the game thread's own frame time, says nothing at all while the
	// editor is smooth, and when a frame goes long it says how long and what
	// this add-on had in flight at that moment: mining, previews, a mount, a
	// resolve. Two clock reads a frame and one comparison.
	//
	// Every interval is zero so it runs with the frame rather than at a
	// cadence of its own, which is the only way to see a frame that was late.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float Delta) -> bool
	{
		static double WorstMs = 0.0;
		static double SaidAt = 0.0;
		static int32  Hitches = 0;
		static FString WorstWhy;

		const double Ms = Delta * 1000.0;
		const double Now = FPlatformTime::Seconds();
		// 50 ms is where a spin stops feeling smooth: two dropped frames at 60.
		if (Ms > 50.0)
		{
			Hitches++;
			if (Ms > WorstMs)
			{
				WorstMs = Ms;
				TArray<FString> What;
				if (BF6HPGameMode::IsMining())       { What.Add(TEXT("mining game modes")); }
				if (BF6HPPreviews::IsBusy())         { What.Add(TEXT("building previews")); }
				if (BF6HP::Shared::CoreBusy())       { What.Add(TEXT("reading the game")); }
				if (GBuildQueuedOrRunning)           { What.Add(TEXT("building scenery")); }
				// OURS, and the one most likely to be the answer: a resolve is
				// seconds of game-thread work. Leaving it out made this report
				// "nothing of ours" for a six second frame that was entirely ours.
				const FString Res = BF6HP::Placed::Resolving();
				if (!Res.IsEmpty())                  { What.Add(TEXT("resolving ") + Res); }
				const FString Placed = BF6HP::Placed::StatusLine();
				WorstWhy = What.Num() ? FString::Join(What, TEXT(", ")) : FString(TEXT("nothing of ours"));
				WorstWhy += TEXT("; placed: ") + Placed;
			}
		}
		if (Now - SaidAt >= 10.0)
		{
			SaidAt = Now;
			if (Hitches > 0)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("frame: %d hitch(es) over 50 ms in the last 10 s, worst %.0f ms while %s"),
					Hitches, WorstMs, *WorstWhy);
			}
			Hitches = 0;
			WorstMs = 0.0;
			WorstWhy.Reset();
		}
		return true;
	}), 0.0f);

	// THE HOST'S MAP EVENTS, not the editor's. See OnHostMapClosing for why:
	// the SDK map selector never opens a .umap, so FEditorDelegates::OnMapOpened
	// was silent for every map switch a creator actually makes.
	GMapOpenedHandle  = BF6Ext::OnMapOpened().AddStatic(&OnHostMapOpened);
	GMapClosingHandle = BF6Ext::OnMapClosing().AddStatic(&OnHostMapClosing);

	// -bf6matcheck: build the ground materials, log the verdict, exit.
	//
	// The console command above is the interactive way in. This is the
	// headless one, and it exists because -ExecCmds DOES NOT FIRE in an editor
	// launched with no map: the process boots, finishes its asset registry
	// scan, and then sits in the tick loop forever having run nothing. Two
	// attempts were lost to that before it was diagnosed from the log going
	// quiet at the asset registry with only EOS heartbeats after it.
	//
	// Module startup always runs. The check itself waits for engine init,
	// because building a material needs the shader compiler up.
	if (FParse::Param(FCommandLine::Get(), TEXT("bf6matcheck")))
	{
		FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([]()
		{
			UE_LOG(LogBF6HighPoly, Display, TEXT("CHECK starting"));
			BF6_CheckMaterials();
			UE_LOG(LogBF6HighPoly, Display, TEXT("CHECK done"));
			// Only a headless run wants the process to end. Launched against a
			// real editor the switch is a self-check the user watches, and
			// exiting out from under them would be the wrong answer.
			if (FParse::Param(FCommandLine::Get(), TEXT("bf6matcheckexit")))
			{
				// Do not request exit from inside OnFEngineLoopInitComplete. UE 5.8
				// continues the same startup frame after the request and opens the
				// default map with a half-torn-down InteractiveToolsFramework. Both
				// UnrealEditor and UnrealEditor-Cmd then crash at address 0x38 even
				// though every material check already completed. Let startup finish,
				// then leave from the core ticker on a later frame.
				FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([](float)
					{
						FPlatformMisc::RequestExit(false);
						return false;
					}),
					1.0f);
			}
		});
	}

	// One-click deterministic bench.  Deliberately runs after engine init and
	// after the SDK's 0.5-second viewport-root attachment retry.  A 0.1-second
	// delay could open and build the map first, then let CHOOSE MAPS attach over
	// the completed level.  Keeping this later than the root retry makes
	// BF6Ext::OpenMap's build-overlay switch the final UI state.
	if (FParse::Param(FCommandLine::Get(), TEXT("bf6benchtsuru")))
	{
		FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([]()
		{
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([](float)
				{
					BF6_RunTsuruBench();
					return false;
				}), 1.0f);
		});
	}

	// A persistent, empty-world shader/data debugger.  It deliberately does
	// not open the SDK map or call StartRead: only the selected level's live
	// water records and installed compute resources are mounted.
	if (FParse::Param(FCommandLine::Get(), TEXT("bf6waterlab")))
	{
		// This module may load after OnFEngineLoopInitComplete has already fired
		// (notably in the tiny RawWaterLab host). A core ticker registered here
		// works in both early- and late-loaded hosts and still defers Slate work.
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float)
			{
				BF6WaterLab::Start();
				return false;
			}), 0.10f);
	}

	BF6Ext::FPieEntry Entry;
	Entry.Id    = FName("HighPoly.Root");
	Entry.Label = TEXT("HIGH POLY");
	Entry.Sub   = TEXT("the real game assets");
	Entry.Order = 900;   // after the tool's own five
	Entry.OnPick = [](FVector2D Center)
	{
		// Verified every time it opens, not once at startup: the game can be
		// moved or uninstalled while the editor is running, and the honest
		// answer then is to ask again rather than to fail at read time.
		const FString Install = EnsureInstall();
		if (Install.IsEmpty())
		{
			BF6Ext::ShowPopup(MakeSetupPanel(), Center);
			return;
		}
		// HIGH POLY opens the panel. The ring is still built from the same list
		// and is still reachable with BF6.HighPoly.Ring: see BuildSections.
		//
		// A WINDOW, NOT A POPUP. This panel is worked in rather than glanced at:
		// it drives the viewport, so it has to stay on top while the viewport
		// has focus, and it has to be movable. As a popup it was neither, and it
		// closed on the first click outside it - which is why the BUILD button,
		// several sections down, was effectively unreachable.
		BF6Ext::ShowAddonWindow(TEXT("HighPoly"), TEXT("HIGH POLY"),
			MakeControlPanel(Install, Center), FVector2D(420.f, 780.f));
	};
	BF6Ext::RegisterPieEntry(Entry);

	// The ring, for anyone who prefers it. Same sections, flattened.
	// The panel's look, in words. A colour scheme is normally only checkable by
	// looking at it, which makes it the one part of the tool that cannot be
	// verified without a person and a screenshot. This says which palette was
	// found and what it resolved to, so a wrong or missing theme is a line in a
	// log rather than something nobody notices until it ships.
	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.HighPoly.Theme"),
		TEXT("Say which season palette the panel is drawing with, and where it came from."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			BF6HPTheme::Reload();
			auto Hex = [](const FLinearColor& C)
			{
				return C.ToFColor(true).ToHex().Left(6);
			};
			UE_LOG(LogBF6HighPoly, Display, TEXT("High Poly panel theme"));
			UE_LOG(LogBF6HighPoly, Display, TEXT("  season    : %s"), *BF6HPTheme::SeasonName());
			UE_LOG(LogBF6HighPoly, Display, TEXT("  accent    : #%s   (outline, section headings, progress)"),
				*Hex(BF6HPTheme::Accent()));
			UE_LOG(LogBF6HighPoly, Display, TEXT("  heading   : #%s"), *Hex(BF6HPTheme::Heading()));
			UE_LOG(LogBF6HighPoly, Display, TEXT("  splash bg : #%s"), *Hex(BF6HPTheme::SplashBg()));
			UE_LOG(LogBF6HighPoly, Display,
				TEXT("  controls  : white on translucent white; they take no colour from the palette"));
			// The entrance is the one part of the look that cannot be checked by
			// reading a colour, so it reports whether its artwork actually loaded.
			UE_LOG(LogBF6HighPoly, Display, TEXT("  entrance  : %s"),
				SBF6HighPolySplash::ArtworkPresent()
					? TEXT("waves and logo loaded; plays on open, click to skip")
					: TEXT("no artwork found, so the panel opens directly"));
		}), ECVF_Default);

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.HighPoly.Ring"),
		TEXT("Open the High Poly controls as the radial ring instead of the panel."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			const FVector2D Center = FSlateApplication::Get().GetCursorPos();
			const FString Install = EnsureInstall();
			if (Install.IsEmpty()) { BF6Ext::ShowPopup(MakeSetupPanel(), Center); return; }
			TArray<BF6Ext::FPieSubEntry> Flat;
			for (const FControlSection& S : BuildSections(Center))
			{
				// The ring speaks pills, so each control is rendered as one:
				// a switch becomes a pill that flips, and a choice becomes a
				// pill that steps to the next option, which is the closest a
				// wheel gets to a dropdown. Sharing the list is what stops the
				// two surfaces offering different things.
				for (const FControl& C : S.Entries)
				{
					BF6Ext::FPieSubEntry E;
					E.Label = C.Label;
					E.bCloses = C.bCloses;
					const FControl Row = C;
					switch (C.Kind)
					{
					case FControl::EKind::Toggle:
						E.Sub = [Row]{ return Row.Sub ? Row.Sub() : FString(); };
						E.OnPick = [Row]{ if (Row.Get && Row.Set) { Row.Set(!Row.Get()); } };
						break;
					case FControl::EKind::Choice:
						E.Sub = [Row]{ return Row.Sub ? Row.Sub() : FString(); };
						E.OnPick = [Row]
						{
							if (!Row.GetChoice || !Row.SetChoice || Row.Choices.Num() == 0) { return; }
							Row.SetChoice((Row.GetChoice() + 1) % Row.Choices.Num());
						};
						break;
					case FControl::EKind::Slider:
						E.Sub = [Row]{ return Row.Sub ? Row.Sub() : FString(); };
						// A wheel cannot drag a value, so it steps it and wraps.
						E.OnPick = [Row]
						{
							if (!Row.GetValue || !Row.SetValue) { return; }
							const float N = Row.GetValue() + Row.Step;
							Row.SetValue(N > Row.Max ? Row.Min : N);
						};
						break;
					default:
						E.Sub = [Row]{ return Row.Sub ? Row.Sub() : FString(); };
						E.OnPick = [Row]{ if (Row.OnAct) { Row.OnAct(); } };
						break;
					}
					Flat.Add(MoveTemp(E));
				}
			}
			BF6Ext::OpenPieSubRing(Flat, Center);
		}), ECVF_Default);

	// ---- BF6UiSound ---- give the tool the game's own voice. The worker opens the
	// core and decodes; nothing here blocks the editor coming up.
	BF6HPUiSounds::Attach(&GCore);

	UE_LOG(LogBF6HighPoly, Log,
		TEXT("High Poly add-on attached (tool extension API v%d). Install: %s"),
		BF6Ext::ApiVersion(),
		BF6Ext::GameInstallDir().IsEmpty() ? TEXT("none") : *BF6Ext::GameInstallDir());
}

void FBF6HighPolyModule::ShutdownModule()
{
	// ---- THE CORE'S WORKERS GO BEFORE ANYTHING ELSE ----
	//
	// GCore.Close at the bottom of this function frees a raw libbf6 context, and
	// two other translation units hand that bare pointer to threads of their
	// own: the previews' install mount and the placed-object catalogue mount.
	// Both took a minute cold, neither was ever joined, and nothing stopped a
	// new one starting while the module was going down - so closing the editor
	// mid-mount ran a worker on a freed context, or on an already unloaded
	// bf6_core.dll. The gate first so no further job can start, then the joins,
	// and only then the UObject and context teardown below.
	//
	// Water Lab does its own waiting, which is why it is not listed here.
	BF6HP::Shared::BeginCoreShutdown();
	BF6HP::Loadout::Stop();
	BF6HPPreviews::JoinCoreWorkers();
	BF6HP::Placed::JoinCoreWorkers();

	// ---- BF6UiSound ---- first: a rooted wave and a playing preview both need the
	// UObject system and bf6_core.dll still standing.
	BF6HPUiSounds::Detach();
	BF6WaterLab::Shutdown();
	if (GWaterClipmapTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GWaterClipmapTickerHandle);
		GWaterClipmapTickerHandle.Reset();
	}
	if (GWaterFFT) { GWaterFFT->Reset(); GWaterFFT.Reset(); }
	for (UMaterial*& Parent : GParents)
	{
		ReleaseRooted(Parent);
	}
	for (UMaterial*& Parent : GEmissiveParents)
	{
		ReleaseRooted(Parent);
	}
	ReleaseRooted(GSkyParent);
	ReleaseRooted(GCloudShadowParent);
	GCloudShadowMID.Reset();
	ReleaseRooted(GGroundParent);
	ReleaseRooted(GGroundBlendParent);
	ReleaseRooted(GWaterParent);
	ReleaseRooted(GClay);
	// A ticker still holding a lambda into this module is a call into freed code
	// the moment the DLL goes. This one polls every half second, so it is very
	// likely to be mid-wait at shutdown.
	CancelQueuedBuild(nullptr);
	if (GMapOpenedHandle.IsValid())
	{
		BF6Ext::OnMapOpened().Remove(GMapOpenedHandle);
		GMapOpenedHandle.Reset();
	}
	if (GMapClosingHandle.IsValid())
	{
		BF6Ext::OnMapClosing().Remove(GMapClosingHandle);
		GMapClosingHandle.Reset();
	}

	BF6Ext::UnregisterPieEntry(FName("HighPoly.Root"));
	// Anything we spawned goes with us: the tool never owned it.
	if (GIsRunning) BF6Ext::ClearAddonActors(kAddonName);

	// Do not leave the context for GCore's file-scope destructor. Module
	// dependency shutdown can release the SDK's bf6_core.dll handle before that
	// destructor runs; Windows then reports bf6_core.dll_unloaded and executes
	// a stale GClose address. Close here while BF6UnrealSDK is guaranteed alive.
	GCore.Close();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBF6HighPolyModule, BF6HighPoly)
