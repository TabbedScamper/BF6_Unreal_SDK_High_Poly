#include "BF6HighPoly.h"

#include "BF6SDKExtension.h"
#include "BF6HighPolyCore.h"

#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Misc/ScopedSlowTask.h"
#include "Framework/Application/SlateApplication.h"
#include "TimerManager.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
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
#include "Engine/PostProcessVolume.h"
#include "Engine/DirectionalLight.h"
#include "TextureResource.h"   // FTexture2DMipMap, for the mip BulkData walk
#include "PixelFormat.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
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
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionThinTranslucentMaterialOutput.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialFunction.h"
#include "UObject/UObjectIterator.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/Layout/SWrapBox.h"
#include "MaterialDomain.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "AssetCompilingManager.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"

#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"

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

	TMap<int32, UTexture2D*> GTextureCache;
	int32 GTexUploaded = 0, GTexRefused = 0, GMidsMade = 0, GBindingsSeen = 0, GBindingsBound = 0;

	// One UTexture2D per core texture id. Cached because the same sheet is
	// bound by many materials, and a 4K BC7 upload per binding is minutes of
	// nothing.
	UTexture2D* TextureFor(int32 Id)
	{
		if (Id < 0) return nullptr;
		if (UTexture2D** hit = GTextureCache.Find(Id)) return *hit;

		BF6HP::FCore::FTexture T;
		if (!GCore.TextureAt(Id, T)) { GTexRefused++; GTextureCache.Add(Id, nullptr); return nullptr; }
		const EPixelFormat PF = PixelFormatOf(T.Format);
		if (PF == PF_Unknown) { GTexRefused++; GTextureCache.Add(Id, nullptr); return nullptr; }

		// THE WHOLE CHAIN, not just the top level.
		//
		// Foliage drawn with a hard alpha test against a mask with NO mip chain
		// speckles every frame and crawls with the camera - the "lacy,
		// moth-eaten" look. It is invariant to mask resolution, so shrinking the
		// texture never fixes it and the mask gets blamed instead.
		UTexture2D* Tex = UTexture2D::CreateTransient(T.Width, T.Height, PF, NAME_None);
		if (!Tex) { GTextureCache.Add(Id, nullptr); return nullptr; }
		GTexUploaded++;

		// SRGB IS THE TEXTURE'S, NOT THE FORMAT'S. The same BC7 blocks carry a
		// colour sheet and a normal map, and the header bit says which. Reading
		// a linear normal as sRGB is the classic "lighting looks wrong and
		// nothing is obviously broken" bug.
		Tex->SRGB = T.bSrgb;
		// BC6H IS HDR, and it is neither a colour sheet nor a mask.
		//
		// The sky panorama arrives here as BC6H and the two-way choice below
		// would file it as TC_Masks, which is the setting for a packed
		// non-colour sheet. TC_HDR is what a float format wants, and SRGB must
		// be off or the values are decoded twice.
		const EPixelFormat Pf = PixelFormatOf(T.Format);
		if (Pf == PF_BC6H || Pf == PF_FloatRGBA)
		{
			Tex->SRGB = false;
		}
		Tex->NeverStream = true;
		Tex->CompressionSettings =
			(Pf == PF_BC6H || Pf == PF_FloatRGBA) ? TC_HDR
			: (T.bSrgb ? TC_Default : TC_Masks);

		FTexturePlatformData* PD = Tex->GetPlatformData();

		// CreateTransient makes one mip; the rest are added here and filled from
		// the chain the core handed over, which is contiguous and largest-first.
		int32 Off = 0, W = T.Width, H = T.Height;
		for (int32 m = 0; m < T.MipCount; m++)
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
			const int32 Left = T.DataLen - Off;
			if (Left <= 0)
			{
				// The chain ran out early: keep the mips we filled and drop the
				// rest, rather than uploading a level of uninitialised memory.
				Mip.BulkData.Unlock();
				while (PD->Mips.Num() > m) PD->Mips.RemoveAt(PD->Mips.Num() - 1);
				break;
			}
			FMemory::Memcpy(Dst, T.Data + Off, FMath::Min(Have, Left));
			Mip.BulkData.Unlock();
			Off += Have;
			W = FMath::Max(1, W / 2);
			H = FMath::Max(1, H / 2);
		}
		Tex->UpdateResource();

		GTextureCache.Add(Id, Tex);
		return Tex;
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
	enum class EKind : uint8 { Opaque, Masked, MaskedVeg, Translucent, Road, Vista, Count };
	UMaterial* GParents[(int32)EKind::Count] = {};

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

	UMaterial* EnsureParentMaterial(EKind Kind)
	{
		if (GParents[(int32)Kind]) return GParents[(int32)Kind];

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
			: Kind == EKind::Road ? TEXT("Road")
			: Kind == EKind::Vista ? TEXT("Vista") : TEXT("Opaque");

		UPackage* Pkg = CreatePackage(*FString::Printf(TEXT("/Temp/BF6HighPoly_%s"), KindName));
		if (!Pkg) return nullptr;
		Pkg->SetFlags(RF_Transient);

		UMaterial* M = NewObject<UMaterial>(Pkg,
			*FString::Printf(TEXT("M_BF6HighPoly_%s"), KindName), RF_Transient);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_DefaultLit);
		if (Kind == EKind::Masked || Kind == EKind::MaskedVeg)
		{
			M->BlendMode = BLEND_Masked;
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
			M->BlendMode = BLEND_Translucent;
			M->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
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
		else if (Kind == EKind::Road)
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

		UMaterialExpressionTextureSampleParameter2D* Base =
			AddParam(TEXT("BaseColor"), SAMPLERTYPE_Color, 0);
		UMaterialExpressionTextureSampleParameter2D* Norm =
			AddParam(TEXT("Normal"),
				// ROADS TAKE THE VISTA TREATMENT TOO. A road decal binds an
				// "_nhs" sheet - normal, height, smoothness - which is the
				// same packing as the vista's "_nsm" for the two channels
				// either of them reads: RG is the tangent normal's XY and A is
				// smoothness. Only the unread middle channel differs, height
				// against wetness. Sampled as a NORMAL map the whole RGB goes
				// into the normal pin, so the height channel is read as Z and
				// the smoothness is thrown away.
				(Kind == EKind::Vista || Kind == EKind::Road)
					? SAMPLERTYPE_LinearColor : SAMPLERTYPE_Normal, 300);

		// A parameter with no default texture compiles to nothing useful, so
		// each gets a neutral one: white for colour, flat for the normal.
		if (Base) Base->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		// The vista NSM is sampled linear, and a linear sampler with the
		// normal-compressed default fails the same compile-time check the
		// sRGB white does - so it defaults to our linear white instead. It is
		// never sampled once a real sheet is bound.
		if (Norm) Norm->Texture = (Kind == EKind::Vista || Kind == EKind::Road)
			? LinearWhite()
			: LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal"));

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
		if (Base && Mul && Tint)
		{
			UMaterialEditingLibrary::ConnectMaterialExpressions(Base, TEXT(""), Mul, TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Mul, TEXT("B"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Mul, TEXT(""), MP_BaseColor);
		}
		else if (Base)
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);
		}
		if (Norm && (Kind == EKind::Vista || Kind == EKind::Road))
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
				UMaterialEditingLibrary::ConnectMaterialExpressions(Norm, TEXT("A"), OM, TEXT(""));
				UMaterialEditingLibrary::ConnectMaterialProperty(OM, TEXT(""), MP_Roughness);
			}
		}
		else if (Norm) UMaterialEditingLibrary::ConnectMaterialProperty(Norm, TEXT(""), MP_Normal);

		// Roughness, likewise from the record. Car paint carries a smoothness and
		// a car that is not glossy does not read as a car. NOT on vista: there
		// the roughness is per-pixel from the NSM alpha, wired above, and a
		// second connection here would overwrite it.
		if (Kind != EKind::Vista && Kind != EKind::Road)
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
					UMaterialEditingLibrary::ConnectMaterialProperty(OpMul, TEXT(""), MP_Opacity);
				}
			}
			else if (VC)
			{
				UMaterialEditingLibrary::ConnectMaterialProperty(VC, TEXT("A"), MP_Opacity);
			}
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
				// 0.75 grey is the old flat 0.25 opacity expressed as what gets
				// through, so nothing changes brightness on day one and the
				// per-record tint replaces it as the decode lands.
				Trans->DefaultValue = FLinearColor(0.75f, 0.75f, 0.75f);
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
				// Thin Translucent still reads MP_Opacity as the surface's
				// own coverage, and it defaults to 1, which is what glass
				// wants. Connected anyway so the root-input check can see it
				// rather than reporting a pin on defaults.
				UMaterialEditingLibrary::ConnectMaterialProperty(Cov, TEXT(""), MP_Opacity);
			}
			if (!TT)
			{
				UE_LOG(LogBF6HighPoly, Error,
					TEXT("glass: no ThinTranslucentMaterialOutput node. The engine ")
					TEXT("refuses to compile this shading model without one and the ")
					TEXT("material will fall back to the default grey."));
			}
		}

		// PostEditChange is what a properly-packaged material needs to cache its
		// shaders. RecompileMaterial is deliberately NOT called: it crashed
		// here, and it is the heavyweight path meant for an asset being edited.
		M->PreEditChange(nullptr);
		M->PostEditChange();

		UE_LOG(LogBF6HighPoly, Log,
			TEXT("parent material %s: %d expression(s), complete=%d (compiling=%d)"), KindName,
			M->GetExpressionCollection().Expressions.Num(),
			M->IsComplete() ? 1 : 0, M->IsCompiling() ? 1 : 0);

		GParents[(int32)Kind] = M;
		return M;
	}

	// One material instance per section, bound to what the depot said.
	UMaterialInstanceDynamic* MaterialFor(UObject* Outer, const BF6HP::FCore::FSection& S)
	{
		const EKind Kind =
			S.bNsm ? EKind::Vista
			: S.bTranslucent ? EKind::Translucent
			: (S.bAlphaTest ? (S.bAlphaFromAlbedo ? EKind::MaskedVeg : EKind::Masked)
			                : EKind::Opaque);
		UMaterial* Parent = EnsureParentMaterial(Kind);
		if (!Parent) return nullptr;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return nullptr;
		// The mesh outlives the call that made it, so the instance has to be
		// rooted to the mesh rather than left for the next collection.
		MID->SetFlags(RF_Transient);

		GMidsMade++;
		// White and 0.5 are the parent's own defaults, so setting them anyway
		// costs a parameter write and keeps the two sides in step.
		MID->SetVectorParameterValue(TEXT("BaseColorTint"), S.BaseColor);
		MID->SetScalarParameterValue(TEXT("Roughness"), S.Roughness);
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
			case 4: MID->SetTextureParameterValue(TEXT("Opacity"), T); break;   // the cutout sheet
			default: break;   // mro / emissive: parameters follow
			}
		}
		return MID;
	}

	// The material for one road record. Its sheets come straight off the record
	// rather than through a shader state key, which is why this does not go
	// through MaterialFor.
	// Decal tints above 1.0 over a colour sheet, clamped rather than passed
	// through. See RoadMaterialFor.
	int32 GRoadTintClamped = 0;

	UMaterialInstanceDynamic* RoadMaterialFor(UObject* Outer, const BF6HP::FCore::FDecal& D)
	{
		UMaterial* Parent = EnsureParentMaterial(EKind::Road);
		if (!Parent) return nullptr;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return nullptr;
		MID->SetFlags(RF_Transient);
		if (UTexture2D* T = TextureFor(D.Albedo))  MID->SetTextureParameterValue(TEXT("BaseColor"), T);
		if (UTexture2D* T = TextureFor(D.Normal))  MID->SetTextureParameterValue(TEXT("Normal"), T);
		if (UTexture2D* T = TextureFor(D.Opacity)) MID->SetTextureParameterValue(TEXT("Opacity"), T);

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
				if (C.R < D.Tint.R || C.G < D.Tint.G || C.B < D.Tint.B) GRoadTintClamped++;
			}
			MID->SetVectorParameterValue(TEXT("BaseColorTint"), C);
		}

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
	int32 GNaniteMinTris = 2000;
	int32 GNaniteCountThisBuild = 0;
	int32 GNoNaniteTranslucent = 0;
	int32 GTerrainTilesBuilt = 0;

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
		if (GNanite && bAllowNanite && Tris >= GNaniteMinTris)
		{
			GNaniteCountThisBuild++;
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
		Mesh->CreateMeshDescription(0, MoveTemp(MD));

		UStaticMesh::FCommitMeshDescriptionParams C;
		C.bMarkPackageDirty = false;        // these meshes are transient
		C.bUseHashAsGuid    = true;         // see below
		Mesh->CommitMeshDescription(0, C);
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
		OutTris = MD.Triangles().Num();
		return MD.Vertices().Num() > 0;
	}

	// The UObject half, which has to be on the game thread.
	UStaticMesh* MakeMesh(UObject* Outer, const FString& Name, FMeshDescription& MD, int32 Tris,
	                      const TArray<BF6HP::FCore::FSection>& Sections)
	{
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, *Name, RF_Transient);

		// ONE SLOT PER SECTION, in the same order DescribeMesh created the
		// polygon groups, so slot i is section i. The two are kept in step by
		// the slot NAME ("S<i>"), which both sides build the same way - a
		// mismatch here does not error, it just renders the wrong material on
		// the wrong triangles.
		for (int32 si = 0; si < Sections.Num(); si++)
		{
			FStaticMaterial SM;
			SM.MaterialSlotName  = FName(*FString::Printf(TEXT("S%d"), si));
			SM.ImportedMaterialSlotName = SM.MaterialSlotName;
			SM.MaterialInterface = MaterialFor(Mesh, Sections[si]);
			Mesh->GetStaticMaterials().Add(SM);
		}
		if (Mesh->GetStaticMaterials().Num() == 0)
			Mesh->GetStaticMaterials().Add(FStaticMaterial());

		// Glass is what costs a vehicle its Nanite, and it is worth knowing how
		// often: reported once per build rather than per mesh.
		bool bTranslucent = false;
		for (const BF6HP::FCore::FSection& S : Sections)
			if (S.bTranslucent) { bTranslucent = true; break; }
		if (bTranslucent) GNoNaniteTranslucent++;

		PrepareMesh(Mesh, MD, Tris, !bTranslucent);
		return Mesh;
	}

	// THE LAYERS, and what they mean.
	//
	// The Godot plugin's dock is a list of these with a switch on each, and the
	// point of it is that a creator can take the real world apart while looking
	// at it. Only layers that actually exist are listed: an inert switch for
	// something unported would be a lie told in the interface.
	enum class ELayer : uint8 { Terrain, Roads, Objects, Water, Lighting, Count };

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
		{ TEXT("Water"), TEXT("The level's water surfaces, at the game's own heights and colours, drawn with Unreal's real water shading - depth absorption, reflections, refraction.") },
		{ TEXT("Lighting"), TEXT("The map's own lighting: the authored sun angle, colour and illuminance, its sky and fog, and every lamp, spot and lit panel the level places. Preview only.") },
	};

	bool GHideLowPoly = true;

	// Components are named by layer so a switch can find them again without
	// holding pointers across a rebuild, which is what would dangle when a map
	// closes underneath us.
	const TCHAR* LayerPrefix(ELayer L)
	{
		return L == ELayer::Terrain  ? TEXT("Terrain")
			 : L == ELayer::Roads    ? TEXT("RoadMesh_")
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
		BF6Ext::SetLowPolyMapHidden(GHideLowPoly && GBuiltAnything);
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
	// 2049 doubles the resolution for four times the triangles, and Nanite is
	// what makes that affordable - 8 million triangles of ground is a lot to
	// draw and very little to cluster.
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
	// reason other than the number being hard-coded. Both are sized to hold
	// about two metres a texel and capped, because the cost is quadratic.
	int32 GGroundTexelM = 2;         // metres per texel to aim for
	int32 GGroundSizeMax = 4096;     // 4096^2 RGBA is 67 MB before mips
	int32 GSheetSize    = 512;       // one array slice

	// The near field runs much further than it did. It was 45 to 130 m, which
	// is fine standing on the ground and useless in the air: fly at any
	// height and every pixel on screen is past 130 m, so the whole world is
	// the flattened bake and the detail never follows the camera. The sheet
	// arrays are mipped, so running them out to several hundred metres costs
	// bandwidth rather than correctness.
	float GBlendNear    = 150.f;     // metres: all sheets
	float GBlendFar     = 450.f;     // metres: all bake

	int32 SizeForExtent(double ExtentM)
	{
		const int32 Want = (int32)FMath::RoundUpToPowerOfTwo(
			(uint32)FMath::Max(1.0, ExtentM / FMath::Max(1, GGroundTexelM)));
		return FMath::Clamp(Want, 2048, GGroundSizeMax);
	}

	UMaterial*       GGroundBlendParent = nullptr;
	UTexture2D*      GCovIdxTex = nullptr;
	UTexture2D*      GCovWTex   = nullptr;
	UTexture2D*      GCovParamTex = nullptr;
	UTexture2D*      GCovColourTex = nullptr;
	UTexture2DArray* GSheetArray  = nullptr;
	UTexture2DArray* GHeightArray = nullptr;

	// An RGBA8 raster that must be read EXACTLY as authored: no sRGB curve, no
	// filtering, no mips. A bilinear read of a layer index is a different
	// layer, so this is the one texture in the material that must be point
	// sampled.
	UTexture2D* MakeRawTexture(const uint8* Px, int32 N, bool bPoint)
	{
		if (!Px || N <= 0) return nullptr;
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
				D[i * 4 + 0] = Px[i * 4 + 2];
				D[i * 4 + 1] = Px[i * 4 + 1];
				D[i * 4 + 2] = Px[i * 4 + 0];
				D[i * 4 + 3] = Px[i * 4 + 3];
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
	//   row 3  uv offset xy
	//
	// A texture rather than a set of shader parameters because the material
	// must serve any map, and maps carry anywhere from a dozen to forty ground
	// layers.
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
				D[i * 4 + 2] = FMath::Clamp(Mats[i].Overlay, 0.f, 1.f);
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
				R3[i * 4 + 2] = 0.f;
				R3[i * 4 + 3] = 0.f;
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
	UTexture2DArray* MakeSheetArray(const TArray<TArray<uint8>>& Sheets, int32 SheetSize)
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
		Tex->SRGB = true;
		Tex->CompressionSettings = TC_Default;
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
					Cur[s][i * 4 + 0] = 128; Cur[s][i * 4 + 1] = 128;
					Cur[s][i * 4 + 2] = 128; Cur[s][i * 4 + 3] = 255;
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

		auto Make = [&M](UClass* C, int32 X, int32 Y)
		{ return UMaterialEditingLibrary::CreateMaterialExpression(M, C, X, Y); };

		UMaterialExpressionWorldPosition* WP = Cast<UMaterialExpressionWorldPosition>(
			Make(UMaterialExpressionWorldPosition::StaticClass(), -1100, 0));
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
		UMaterialExpressionScalarParameter* NearP = Scal(TEXT("BlendNear"), 45.f, 400);
		UMaterialExpressionScalarParameter* FarP  = Scal(TEXT("BlendFar"), 130.f, 460);
		// A DEBUG CHANNEL, so the ground can be interrogated by looking at it.
		// Every mode below answers one question that is otherwise a guess.
		UMaterialExpressionScalarParameter* Dbg = Scal(TEXT("DebugMode"), 0.f, 520);
		// PHOTO BASE. How much of the ground's COLOUR comes from the level's own
		// aerial map rather than from the layer sheets. See the shader.
		UMaterialExpressionScalarParameter* Photo = Scal(TEXT("PhotoMix"), 0.f, 580);
		// DEFAULT OFF. The aerial map is a correct photograph and a tempting
		// crutch, but the goal is ground good enough to REPLACE it, and a
		// crutch left in place is how that never happens. Kept as a live
		// comparison via BF6.HighPoly.GroundPhoto, not as the answer.

		UTexture2D* White = LoadObject<UTexture2D>(nullptr,
			TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		UMaterialExpressionTextureObjectParameter* CovIdx =
			TexObj(TEXT("CovIdx"), White, SAMPLERTYPE_LinearColor, 520);
		UMaterialExpressionTextureObjectParameter* CovW =
			TexObj(TEXT("CovW"), White, SAMPLERTYPE_LinearColor, 580);
		UMaterialExpressionTextureObjectParameter* ParamT =
			TexObj(TEXT("CovParams"), White, SAMPLERTYPE_LinearColor, 640);
		UMaterialExpressionTextureObjectParameter* Baked =
			TexObj(TEXT("GroundAlbedo"), White, SAMPLERTYPE_Color, 700);
		// The far bake: the whole footprint at low density, for the ground
		// beyond the playable box. sRGB like the near bake it stands in for.
		UMaterialExpressionTextureObjectParameter* FarBaked =
			TexObj(TEXT("FarBake"), MidGreyTexture(), SAMPLERTYPE_Color, 1340);
		UMaterialExpressionTextureObjectParameter* ColourM =
			TexObj(TEXT("GroundColour"), White, SAMPLERTYPE_Color, 820);
		UMaterialExpressionTextureObjectParameter* Sheets =
			TexObj(TEXT("Sheets"), DefaultSheetArray(), SAMPLERTYPE_Color, 760);
		// The normal/height sheets. Raw, not sRGB: the blue channel is a
		// HEIGHT and putting a display curve through it changes the blend.
		UMaterialExpressionTextureObjectParameter* Heights =
			TexObj(TEXT("Heights"), DefaultSheetArray(), SAMPLERTYPE_LinearColor, 880);

		UMaterialExpressionCustom* Blend = Cast<UMaterialExpressionCustom>(
			Make(UMaterialExpressionCustom::StaticClass(), -500, 300));
		if (!Blend || !WP || !CamP || !Lo || !Span || !CovN || !MatN || !NearP || !FarP
			|| !CovIdx || !CovW || !ParamT || !Baked || !Sheets || !ColourM || !Heights
			|| !FarBaked || !FarLoP || !FarSpanP || !BoxCentreP || !BoxHalfP
			|| !Dbg || !Photo)
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
// Four coverage taps, bilinear over the WEIGHTS while the indices stay point
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

float4 IDS[4], WS[4];
IDS[0] = Texture2DSampleLevel(CovIdx, CovIdxSampler, b0, 0) * 255.0;
WS[0]  = Texture2DSampleLevel(CovW,   CovWSampler,   b0, 0);
IDS[1] = Texture2DSampleLevel(CovIdx, CovIdxSampler, b0 + float2(inv, 0), 0) * 255.0;
WS[1]  = Texture2DSampleLevel(CovW,   CovWSampler,   b0 + float2(inv, 0), 0);
IDS[2] = Texture2DSampleLevel(CovIdx, CovIdxSampler, b0 + float2(0, inv), 0) * 255.0;
WS[2]  = Texture2DSampleLevel(CovW,   CovWSampler,   b0 + float2(0, inv), 0);
IDS[3] = Texture2DSampleLevel(CovIdx, CovIdxSampler, b0 + float2(inv, inv), 0) * 255.0;
WS[3]  = Texture2DSampleLevel(CovW,   CovWSampler,   b0 + float2(inv, inv), 0);

float BW[4];
BW[0] = (1.0 - fr.x) * (1.0 - fr.y);
BW[1] = fr.x * (1.0 - fr.y);
BW[2] = (1.0 - fr.x) * fr.y;
BW[3] = fr.x * fr.y;

// The nearest corner supplies the four layers to blend. Anything a neighbour
// carries that this one does not is genuinely a different material and gets
// no weight here; it will own the pixel a texel further on.
int   refc = (fr.x < 0.5 ? 0 : 1) + (fr.y < 0.5 ? 0 : 2);
float rid[4] = { IDS[refc].x, IDS[refc].y, IDS[refc].z, IDS[refc].w };
float acc[4] = { 0.0, 0.0, 0.0, 0.0 };

for (int c = 0; c < 4; c++)
{
	float ic[4] = { IDS[c].x, IDS[c].y, IDS[c].z, IDS[c].w };
	float wc[4] = { WS[c].x,  WS[c].y,  WS[c].z,  WS[c].w  };
	for (int s = 0; s < 4; s++)
	{
		for (int k = 0; k < 4; k++)
		{
			if (wc[k] > 0.0 && abs(ic[k] - rid[s]) < 0.5)
			{
				acc[s] += BW[c] * wc[k];
			}
		}
	}
}

// ASCENDING BY LAYER, because the evaluator's accumulators are order
// dependent: each layer's coverage is measured against the composite of
// everything below it, and the raster hands its slots back weight-sorted.
for (int p = 1; p < 4; p++)
{
	for (int q = p; q > 0; q--)
	{
		if (rid[q] < rid[q - 1])
		{
			float tid = rid[q]; rid[q] = rid[q - 1]; rid[q - 1] = tid;
			float tw2 = acc[q]; acc[q] = acc[q - 1]; acc[q - 1] = tw2;
		}
	}
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

for (int s2 = 0; s2 < 4; s2++)
{
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
	float4 nh4 = Texture2DArraySampleGrad(Heights, HeightsSampler,
	                                      float3(uv, slice), ddu, ddv);

	// The layer's own height, and the range it can move within.
	float ht      = saturate(nh4.b);
	float baseH   = P2.r;
	float disp    = P2.g;
	float hLayer  = baseH + (ht - 0.5) * disp;
	float hMax    = baseH + 0.5 * disp;
	float hMin    = baseH - 0.5 * disp;

	float m       = saturate(acc[s2]);
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

	if (c > 0.0)
	{
		accH     = clamp(lerp(loRef, hiRef, rawc), -1.0, 10.0);
		accLoMax = lerp(accLoMax, hMin, rawc);
		accHiMax = lerp(accHiMax, hMax, rawc);
		touched  = true;
	}
}

// Nothing resolved here: the aerial photograph of this spot is by far the
// closest answer available, and it is real shipped data rather than a guess.
// LINEARISED HERE, because this is the one place the map is used as a
// COLOUR rather than as the Overlay's operand. The texture is raw now, so
// anything consuming it as light has to de-gamma explicitly.
if (!touched) col = pow(max(aerial, 0.0), 2.2);

// COLOUR FROM THE PHOTOGRAPH, DETAIL FROM THE MATERIALS.
//
// Assembling ground colour out of layer sheets depends on picking the right
// sheet for every layer, and where that join is wrong the ground is wrong in a
// way no amount of blending fixes. The level's own aerial colour map does not
// have that problem: it is a photograph of this exact ground, correct by
// construction, and merely low frequency - now 0.62 m a texel over the
// playable box rather than the 2 m it was over the whole footprint.
//
// So the two are used for what each is good at. The photograph carries the
// COLOUR, which is what the eye judges a map by, and the sheets carry the
// high-frequency STRUCTURE as a brightness modulation around their own mean.
// A wrong sheet then costs detail rather than hue, which is a far smaller
// error than the one it replaces.
//
// This is not a departure from the game either: the shipped evaluator already
// Overlay-blends this same map over the layer colours. It is the same two
// inputs with the emphasis where the data is trustworthy.
{
    float sheetLuma = dot(col, float3(0.2126, 0.7152, 0.0722));
    // Around a mid reference rather than a measured mean: the sheets are
    // authored near mid grey and a per-pixel mean is not available here.
    float detail = clamp(sheetLuma / 0.35, 0.55, 1.7);
    // Same reason as the untouched case above: as a colour, not an operand.
    float3 photo = pow(max(aerial, 0.0), 2.2) * detail;
    col = lerp(col, photo, saturate(PhotoMix));
}

float dist = length(WPos - CamPos) * 0.01;
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
int dbg = (int)(DebugMode + 0.5);
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
if (dbg == 5)
{
    float used = 0.0;
    for (int d = 0; d < 4; d++) if (acc[d] > 0.0) used += 1.0;
    return float3(used * 0.25, used * 0.25, used * 0.25);
}
return lerp(col, baked, k);
)HLSL");

		Blend->Inputs.Empty();
		auto In = [&Blend](const TCHAR* Nm, UMaterialExpression* E)
		{ FCustomInput I; I.InputName = Nm; I.Input.Expression = E; Blend->Inputs.Add(I); };
		In(TEXT("WPos"), WP);
		In(TEXT("CamPos"), CamP);
		In(TEXT("Lo"), Lo);
		In(TEXT("Span"), Span);
		In(TEXT("CovSize"), CovN);
		In(TEXT("MatCount"), MatN);
		In(TEXT("NearM"), NearP);
		In(TEXT("FarM"), FarP);
		In(TEXT("CovIdx"), CovIdx);
		In(TEXT("CovW"), CovW);
		In(TEXT("Params"), ParamT);
		In(TEXT("Sheets"), Sheets);
		In(TEXT("Bake"), Baked);
		In(TEXT("FarBake"), FarBaked);
		In(TEXT("FarLo"), FarLoP);
		In(TEXT("FarSpan"), FarSpanP);
		In(TEXT("BoxCentre"), BoxCentreP);
		In(TEXT("BoxHalf"), BoxHalfP);
		In(TEXT("Aerial"), ColourM);
		In(TEXT("DebugMode"), Dbg);
		In(TEXT("PhotoMix"), Photo);
		In(TEXT("Heights"), Heights);

		UMaterialEditingLibrary::ConnectMaterialProperty(Blend, TEXT(""), MP_BaseColor);

		UMaterialExpressionScalarParameter* Rg = Scal(TEXT("GroundRoughness"), 0.9f, 900);
		if (Rg) UMaterialEditingLibrary::ConnectMaterialProperty(Rg, TEXT(""), MP_Roughness);

		M->PreEditChange(nullptr);
		M->PostEditChange();
		M->AddToRoot();   // see EnsureGroundMaterial: a raw global must be rooted
		GGroundBlendParent = M;
		return M;
	}

	// The per-pixel ground, or null to fall back on the flattened bake.
	UMaterialInstanceDynamic* MakeGroundBlendMaterial(UObject* Outer, const FString& Level,
	                                                  double ExtentM)
	{
		BF6HP::FCore::FGroundCoverage C;
		const double T0 = FPlatformTime::Seconds();
		if (!GCore.GroundCoverage(Level, SizeForExtent(ExtentM), C))
		{
			UE_LOG(LogBF6HighPoly, Warning, TEXT("ground coverage: %s"), *GCore.Error);
			return nullptr;
		}

		// Decode every bound sheet to one size so they can share an array.
		// The height sheets go into a second array of the same shape: the
		// evaluator needs a height per layer at the same point, and without it
		// the blend has nothing to sharpen with.
		TArray<TArray<uint8>> Sheets, Heights;
		Sheets.SetNum(C.Materials.Num());
		Heights.SetNum(C.Materials.Num());
		int32 Decoded = 0, HeightsDecoded = 0;
		for (int32 i = 0; i < C.Materials.Num(); i++)
		{
			if (!C.Materials[i].Albedo.IsEmpty())
			{
				if (GCore.LayerSheet(C.Materials[i].Albedo, GSheetSize, Sheets[i])) Decoded++;
				else Sheets[i].Reset();
			}
			if (!C.Materials[i].Normal.IsEmpty())
			{
				if (GCore.LayerSheet(C.Materials[i].Normal, GSheetSize, Heights[i])) HeightsDecoded++;
				else Heights[i].Reset();
			}
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("ground coverage: %d px over %.0f m (%.2f m/texel), %d material(s), ")
			TEXT("%d albedo + %d height sheet(s) decoded, %.1f%% empty, %.1fs"),
			C.Size, C.Hi.X - C.Lo.X, (float)(C.Hi.X - C.Lo.X) / (float)FMath::Max(1, C.Size),
			C.Materials.Num(), Decoded, HeightsDecoded, C.EmptyFraction * 100.f,
			FPlatformTime::Seconds() - T0);
		if (Decoded == 0)
		{
			UE_LOG(LogBF6HighPoly, Warning,
				TEXT("no ground sheet decoded, so the per-pixel blend would draw flat; ")
				TEXT("falling back to the bake"));
			return nullptr;
		}

		GCovIdxTex = MakeRawTexture(C.Idx, C.Size, /*point*/ true);
		GCovWTex = MakeRawTexture(C.Weight, C.Size, /*point*/ true);
		GCovParamTex = MakeParamTexture(C.Materials);
		GCovColourTex = C.Colour ? MakeColourTexture(C.Colour, C.Size) : nullptr;
		GSheetArray = MakeSheetArray(Sheets, GSheetSize);
		GHeightArray = MakeSheetArray(Heights, GSheetSize);
		if (!GCovIdxTex || !GCovWTex || !GCovParamTex || !GSheetArray || !GHeightArray)
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
		MID->SetTextureParameterValue(TEXT("Sheets"), GSheetArray);
		MID->SetTextureParameterValue(TEXT("Heights"), GHeightArray);
		if (GGroundAlbedo) MID->SetTextureParameterValue(TEXT("GroundAlbedo"), GGroundAlbedo);
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
		return MID;
	}

	// Bake the ground and hand back a material instance for the terrain tiles.
	// Null when there is nothing to bake, which leaves the terrain as it was.
	UMaterialInstanceDynamic* MakeGroundMaterial(UObject* Outer, const FString& Level,
	                                             double ExtentM,
	                                             const FVector2D& FarLo, float FarSize)
	{
		BF6HP::FCore::FGroundBake B;
		const double T0 = FPlatformTime::Seconds();
		GGroundBakeSize = SizeForExtent(ExtentM);
		if (!GCore.BakeGround(Level, FVector2D::ZeroVector, 0.f, GGroundBakeSize, B))
		{
			UE_LOG(LogBF6HighPoly, Warning, TEXT("ground bake: %s"), *GCore.Error);
			return nullptr;
		}
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("ground bake: %d px over %.0f m (%.2f m/texel), %d layer(s), %d textured, ")
			TEXT("%.1f%% untextured, %.1fs"),
			B.Size, B.Hi.X - B.Lo.X, B.MetresPerTexel, B.LayersUsed, B.LayersTextured,
			B.FallbackFraction * 100.f, FPlatformTime::Seconds() - T0);

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
			const double TF = FPlatformTime::Seconds();
			if (GCore.BakeGround(Level, FarLo, FarSize, 1024, F))
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
				UE_LOG(LogBF6HighPoly, Warning,
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

		const int32 Per   = FMath::Clamp(GTerrainTile, 16, N - 1);
		const int32 Tiles = FMath::DivideAndRoundUp(N - 1, Per);

		// One vertex of the whole grid, by grid index. Shared by the tiles that
		// meet on it, which is what keeps the seams closed.
		auto VertexAt = [&](int32 x, int32 z)
		{
			const int32 sx = FMath::Min(x * Step, T.Size - 1);
			const int32 sz = FMath::Min(z * Step, T.Size - 1);
			// Game space is X across, Z along, Y up. Unreal wants Z up, so the
			// same swap the props get, and the same unit change.
			const double gx = XLo + XSpan * ((double)x / (N - 1));
			const double gz = ZLo + ZSpan * ((double)z / (N - 1));
			const double gy = (double)T.Heights[sz * T.Size + sx] * YScale;
			return FVector3f((float)(gx * 100.0), (float)(gz * 100.0), (float)(gy * 100.0));
		};

		int32 Built = 0;
		for (int32 tz = 0; tz < Tiles; tz++)
		for (int32 tx = 0; tx < Tiles; tx++)
		{
			const int32 x0 = tx * Per, x1 = FMath::Min(x0 + Per, N - 1);
			const int32 z0 = tz * Per, z1 = FMath::Min(z0 + Per, N - 1);
			const int32 nx = x1 - x0 + 1, nz = z1 - z0 + 1;
			if (nx < 2 || nz < 2) continue;

			FMeshDescription MD;
			FStaticMeshAttributes Attr(MD);
			Attr.Register();
			TVertexAttributesRef<FVector3f>         VPos = Attr.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector2f> VUV  = Attr.GetVertexInstanceUVs();
			const FPolygonGroupID Group = MD.CreatePolygonGroup();
			Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("BF6Terrain");

			TArray<FVertexID> V;
			V.SetNumUninitialized(nx * nz);
			MD.ReserveNewVertices(nx * nz);
			for (int32 z = 0; z < nz; z++)
				for (int32 x = 0; x < nx; x++)
				{
					const FVertexID id = MD.CreateVertex();
					VPos[id] = VertexAt(x0 + x, z0 + z);
					V[z * nx + x] = id;
				}

			MD.ReserveNewPolygons((nx - 1) * (nz - 1) * 2);
			for (int32 z = 0; z + 1 < nz; z++)
				for (int32 x = 0; x + 1 < nx; x++)
				{
					const FVertexID a = V[z * nx + x],           b = V[z * nx + x + 1];
					const FVertexID c = V[(z + 1) * nx + x + 1], e = V[(z + 1) * nx + x];
					// UV stays in WHOLE-MAP space, not tile space, so a texture
					// laid over the ground crosses tiles without repeating.
					const float u0 = (float)(x0 + x)     / (N - 1);
					const float u1 = (float)(x0 + x + 1) / (N - 1);
					const float v0 = (float)(z0 + z)     / (N - 1);
					const float v1 = (float)(z0 + z + 1) / (N - 1);

					auto Corner = [&](const FVertexID id, float u, float v)
					{
						const FVertexInstanceID vi = MD.CreateVertexInstance(id);
						VUV.Set(vi, 0, FVector2f(u, v));
						return vi;
					};
					// Wound so the ground faces up after the axis swap, same
					// reason the props are reversed.
					MD.CreatePolygon(Group, TArray<FVertexInstanceID>{
						Corner(a, u0, v0), Corner(e, u0, v1), Corner(b, u1, v0) });
					MD.CreatePolygon(Group, TArray<FVertexInstanceID>{
						Corner(b, u1, v0), Corner(e, u0, v1), Corner(c, u1, v1) });
				}

			FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
			FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals);

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
			PrepareMesh(SM, MD, MD.Triangles().Num());
			OutPending.Add(SM);

			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(
				A, *FString::Printf(TEXT("TerrainMesh_%d_%d"), tx, tz));
			C->SetupAttachment(Root);
			C->RegisterComponent();
			C->SetStaticMesh(SM);
			C->SetMobility(EComponentMobility::Static);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Built++;
		}
		GTerrainTilesBuilt = Built;
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
	int32 GRoadColourless = 0;   // records that bind no colour sheet at all
	int32 GRoadPainted = 0;      // ...of which carry an authored colour instead

	// THE UV ORDER IS THE ONE OPEN QUESTION HERE, and it cannot be settled by
	// arithmetic. The edge metric that validates the SCALE is symmetric in its
	// two terms, so it pins the tiling and cannot see a 90 degree rotation;
	// correlating v against a record's long axis has the same blind spot. Only
	// looking at it settles it: under the wrong order, tyre tracks run ACROSS
	// the direction of travel instead of along it. The research hub's corrected
	// reading is (u, v), so that is what this uses, and this switch exists so
	// the other order is one line away rather than a rebuild of the reasoning.
	bool GRoadUvSwapped = false;

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

	UMaterial* GWaterParent = nullptr;

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
		UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_BF6HighPoly_Water"), RF_Transient);
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
		UMaterialExpressionScalarParameter* ShD = Scal(TEXT("ShoreFadeDistance"), 6.f, 1280);
		// THE CLARITY RAMP IS NOT THE WAVE FADE, and reusing one number for
		// both was wrong. The wave fade decides where swell stops running into
		// the beach, and six metres is right for that. The clarity ramp is the
		// composite's `saturate(d / cb51.y)`, and the authored ShoreDepth it
		// corresponds to is 14.03 m on MP_Isolated and 100 on MP_Dumbo - far
		// deeper. Sharing the wave's six metres made the water opaque a few
		// steps out from the sand, when in game the shallows stay clear for a
		// long stretch.
		// No ClarityDepth parameter any more. It fed a ramp that duplicated
		// the shading model's own exp(-extinction * depth), and the live knob
		// that replaced it scales the EXTINCTION, which is the quantity that
		// actually sets how far you can see.
		UMaterialExpressionScalarParameter* ShF = Scal(TEXT("ShoreFoam"), 0.55f, 1340);
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
			In(TEXT("CamPos"), CamP);
			In(TEXT("ShoreD"), ShD);
			In(TEXT("ShoreFoam"), ShF);
			In(TEXT("DepthMin"), DMin);
			In(TEXT("DepthSpan"), DSpan);
			In(TEXT("DepthTex"), DTex);
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
			ShD && ShF && DMin && DSpan && DTex &&
			Wv[0] && Wv[1] && Wv[2] && Wv[3] && Wv[4] && Wv[5] && Wv[6] && Wv[7])
		{
			WpoX->Code = TEXT("float3 w[8] = {W0,W1,W2,W3,W4,W5,W6,W7};\nconst float ratio[8] = {1.0, 0.618, 0.382, 0.236, 0.146, 0.090, 0.056, 0.034};\nfloat2 pm = WPos.xy * 0.01;\n// SHORE FADE, the way the game does it: the wave amplitude is scaled\n// down by how shallow the water is. Recovered form is\n//   depth = waterY - terrainY;  t = saturate(depth / D);\n//   fade  = saturate(cubic(t))\n// with the cubic authored per level - a plain smoothstep in the\n// default case. Without it the swell runs straight into the beach at\n// full height, which is what makes a repeating pattern so obvious near\n// land.\nfloat2 duv = (WPos.xy * 0.01 - DepthMin) / DepthSpan;\nfloat terrainY = Texture2DSample(DepthTex, DepthTexSampler, saturate(duv)).r;\nfloat depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep\nfloat3 disp = float3(0,0,0);\nfor (int i = 0; i < 8; i++) {\n  float len = max(ratio[i] * BaseLen, 0.5);\n  if (len < MinLen) continue;\n  float2 d = normalize(w[i].xy + float2(1e-5, 0));\n  float A = w[i].z * Gain * shore;\n  float k = 6.2831853 / len;\n  float ph = k * dot(d, pm) - sqrt(9.81 * k) * T;\n  disp.xy += d * (Chop * A) * cos(ph);\n  disp.z  += A * sin(ph);\n}\nreturn disp * 100.0;");
			WpoX->OutputType = CMOT_Float3;
			WpoX->Description = TEXT("BF6 Gerstner displacement");
			WaveInputs(WpoX, WP, Tm, Wv, Gn, Ch, Bl, MnL);
			UMaterialEditingLibrary::ConnectMaterialProperty(WpoX, TEXT(""), MP_WorldPositionOffset);

			NrmX->Code = TEXT("float3 w[8] = {W0,W1,W2,W3,W4,W5,W6,W7};\nconst float ratio[8] = {1.0, 0.618, 0.382, 0.236, 0.146, 0.090, 0.056, 0.034};\nfloat2 pm = WPos.xy * 0.01;\n// SHORE FADE, the way the game does it: the wave amplitude is scaled\n// down by how shallow the water is. Recovered form is\n//   depth = waterY - terrainY;  t = saturate(depth / D);\n//   fade  = saturate(cubic(t))\n// with the cubic authored per level - a plain smoothstep in the\n// default case. Without it the swell runs straight into the beach at\n// full height, which is what makes a repeating pattern so obvious near\n// land.\nfloat2 duv = (WPos.xy * 0.01 - DepthMin) / DepthSpan;\nfloat terrainY = Texture2DSample(DepthTex, DepthTexSampler, saturate(duv)).r;\nfloat depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep\nfloat dist = length(WPos - CamPos) * 0.01;\nfloat3 n = float3(0,0,1);\nfor (int i = 0; i < 8; i++) {\n  float len = max(ratio[i] * BaseLen, 0.5);\n  float2 d = normalize(w[i].xy + float2(1e-5, 0));\n  float A = w[i].z * Gain * shore;\n  float k = 6.2831853 / len;\n  float ph = k * dot(d, pm) - sqrt(9.81 * k) * T;\n  float wa = k * A;\n  n.xy -= d * wa * cos(ph);\n  n.z  -= Chop * wa * sin(ph);\n}\n// Ripples, six octaves from about four metres down to fifteen\n// centimetres, each faded out as it stops being resolvable. These do\n// NOT take the shore fade: a beach still has ripples on it.\nfloat2 dirA = normalize(w[0].xy + float2(1e-5, 0));\nfloat2 dirB = float2(-dirA.y, dirA.x);\nfloat rlen = 4.0;\nfloat ramp = Detail * 0.06;\nfor (int j = 0; j < 6; j++) {\n  float fade = saturate(1.0 - dist / (rlen * 90.0));\n  if (fade > 0.001) {\n    float k2 = 6.2831853 / rlen;\n    float sp = sqrt(9.81 * k2);\n    float2 dd = normalize(dirA * (1.0 + 0.7 * float(j % 3)) + dirB * (0.5 - 0.35 * float(j % 2)));\n    float p2 = k2 * dot(dd, pm) - sp * T * 1.15;\n    n.xy -= dd * (k2 * ramp * fade) * cos(p2);\n  }\n  rlen *= 0.55;\n  ramp *= 0.72;\n}\nreturn normalize(n);");
			NrmX->OutputType = CMOT_Float3;
			NrmX->Description = TEXT("BF6 Gerstner normal");
			WaveInputs(NrmX, WP, Tm, Wv, Gn, Ch, Bl, MnL);
			UMaterialEditingLibrary::ConnectMaterialProperty(NrmX, TEXT(""), MP_Normal);
			// The node writes a WORLD normal; the plane's tangent frame is not
			// part of the math.
			M->bTangentSpaceNormal = false;
		}

		// ---- FOAM, from the folding of the displacement field -------------
		//
		// Hoisted out of the block because OPACITY needs it. On a Single Layer
		// Water material Opacity is not "how much water": it is the coverage of
		// whatever opaque material sits ON TOP of the water, and foam is
		// exactly that. See the Opacity block below.
		UMaterialExpressionCustom* FoamOut = nullptr;
		if (WP && Tm && Gn && Ch && Bl && MnL && FTh && FMx && Wv[0])
		{
			UMaterialExpressionCustom* FoamX =
				Cast<UMaterialExpressionCustom>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionCustom::StaticClass(), -300, 1160));
			if (FoamX)
			{
				FoamX->Code = TEXT("float3 w[8] = {W0,W1,W2,W3,W4,W5,W6,W7};\nconst float ratio[8] = {1.0, 0.618, 0.382, 0.236, 0.146, 0.090, 0.056, 0.034};\nfloat2 pm = WPos.xy * 0.01;\n// SHORE FADE, the way the game does it: the wave amplitude is scaled\n// down by how shallow the water is. Recovered form is\n//   depth = waterY - terrainY;  t = saturate(depth / D);\n//   fade  = saturate(cubic(t))\n// with the cubic authored per level - a plain smoothstep in the\n// default case. Without it the swell runs straight into the beach at\n// full height, which is what makes a repeating pattern so obvious near\n// land.\nfloat2 duv = (WPos.xy * 0.01 - DepthMin) / DepthSpan;\nfloat terrainY = Texture2DSample(DepthTex, DepthTexSampler, saturate(duv)).r;\nfloat depth = (WPos.z * 0.01) - terrainY;\nfloat st = saturate(depth / max(ShoreD, 0.5));\nfloat shore = st * st * (3.0 - 2.0 * st);   // smoothstep\nfloat div = 0.0;\nfloat scale = 0.0;\nfor (int i = 0; i < 8; i++) {\n  float len = max(ratio[i] * BaseLen, 0.5);\n  if (len < MinLen) continue;\n  float2 d = normalize(w[i].xy + float2(1e-5, 0));\n  float A = w[i].z * Gain * shore;\n  float k = 6.2831853 / len;\n  float ph = k * dot(d, pm) - sqrt(9.81 * k) * T;\n  float term = Chop * A * k;\n  div   += term * sin(ph);\n  scale += term;\n}\nfloat f = div / max(scale, 1e-4);\nfloat bias = clamp(Thr / 60.0, 0.02, 0.85);\nfloat crest = saturate((f - bias) / max(1.0 - bias, 0.05)) * Mx;\n// AND FOAM WHERE IT MEETS LAND. The game exports 1 - fade to the pixel\n// shader for exactly this: surf collects where the water is shallow.\nfloat surf = saturate(1.0 - shore) * ShoreFoam;\nreturn saturate(max(crest, surf));");
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
				In(TEXT("CamPos"), CamP);
				In(TEXT("ShoreD"), ShD);
				In(TEXT("ShoreFoam"), ShF);
				In(TEXT("DepthMin"), DMin);
				In(TEXT("DepthSpan"), DSpan);
				In(TEXT("DepthTex"), DTex);
				In(TEXT("Thr"), FTh);
				In(TEXT("Mx"), FMx);

				// Foam is white, rough and unlit-ish: it rides over the water
				// colour rather than tinting it, so it goes through BaseColor
				// and Roughness rather than through the water volume.
				UMaterialExpressionLinearInterpolate* FoamMix =
					Cast<UMaterialExpressionLinearInterpolate>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionLinearInterpolate::StaticClass(), -100, 60));
				UMaterialExpressionConstant3Vector* FoamCol =
					Cast<UMaterialExpressionConstant3Vector>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionConstant3Vector::StaticClass(), -300, 20));
				if (FoamMix && FoamCol && Tint)
				{
					FoamCol->Constant = FLinearColor(0.78f, 0.82f, 0.84f);
					UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), FoamMix, TEXT("A"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(FoamCol, TEXT(""), FoamMix, TEXT("B"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(FoamX, TEXT(""), FoamMix, TEXT("Alpha"));
					UMaterialEditingLibrary::ConnectMaterialProperty(FoamMix, TEXT(""), MP_BaseColor);
				}
				// foam kills the mirror finish where it sits
				UMaterialExpressionLinearInterpolate* RoughMix =
					Cast<UMaterialExpressionLinearInterpolate>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							M, UMaterialExpressionLinearInterpolate::StaticClass(), -100, 200));
				if (RoughMix && Rough)
				{
					UMaterialEditingLibrary::ConnectMaterialExpressions(Rough, TEXT(""), RoughMix, TEXT("A"));
					UMaterialEditingLibrary::ConnectMaterialExpressions(FoamX, TEXT(""), RoughMix, TEXT("Alpha"));
					// B defaults to 1 = fully rough foam
					UMaterialEditingLibrary::ConnectMaterialProperty(RoughMix, TEXT(""), MP_Roughness);
				}
				FoamOut = FoamX;
			}
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

		M->PreEditChange(nullptr);
		M->PostEditChange();
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
		R.Chop = FMath::Clamp(S.Choppiness, 0.05f, 0.9f);
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

	UTexture2D* MakeDepthTexture(const BF6HP::FCore::FTerrain& T)
	{
		if (T.Size <= 1 || T.Heights.Num() < T.Size * T.Size) return nullptr;
		// A quarter of the native side is plenty: this drives an amplitude
		// fade, not geometry.
		const int32 N = FMath::Clamp(T.Size / 4, 64, 1024);
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
					P[y * N + x] = (float)(T.Heights[sy * T.Size + sx] * YScale + T.WorldMin.Y);
				}
			Tex->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		Tex->UpdateResource();
		Tex->AddToRoot();
		GWaterDepthMin = FVector2D(T.WorldMin.X, T.WorldMin.Z);
		GWaterDepthSpan = FVector2D(
			FMath::Max(1.0, T.WorldMax.X - T.WorldMin.X),
			FMath::Max(1.0, T.WorldMax.Z - T.WorldMin.Z));
		return Tex;
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

	UMaterialInstanceDynamic* WaterMaterialFor(UObject* Outer, const BF6HP::FCore::FWater& W,
	                                           const FWaveSet* Waves)
	{
		UMaterial* Parent = EnsureWaterMaterial();
		if (!Parent) return nullptr;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (!MID) return nullptr;
		MID->SetFlags(RF_Transient);
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
		UE_LOG(LogBF6HighPoly, Warning,
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
		UMaterialExpressionScalarParameter* Scale = Scal(TEXT("SkyEmissiveScale"), 1.f, 360);

		UMaterialExpressionWorldPosition* WP =
			Cast<UMaterialExpressionWorldPosition>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionWorldPosition::StaticClass(), -600, 440));
		UMaterialExpressionCameraPositionWS* CamP =
			Cast<UMaterialExpressionCameraPositionWS>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCameraPositionWS::StaticClass(), -600, 520));

		UMaterialExpressionCustom* Dome =
			Cast<UMaterialExpressionCustom>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionCustom::StaticClass(), -200, 200));
		if (Dome && Pano && Rot && VMin && VMax && Scale && WP && CamP)
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
float u = frac(Rot - atan2(d.x, d.y) / 6.2831853);
// UPPER HEMISPHERE ONLY. Row 0 is the zenith, so elevation runs 90 degrees at
// v = 0 down to the horizon at v = 1, and anything below the horizon holds on
// the horizon row rather than wrapping to the top of the image.
float e = degrees(asin(clamp(d.z, -1.0, 1.0)));
float v = saturate(1.0 - max(e, 0.0) / 90.0);
v = lerp(VMinY, VMaxY, v);
return Texture2DSample(Pano, PanoSampler, float2(u, v)).rgb * Scale;
)HLSL");
			Dome->Inputs.Empty();
			auto In = [&Dome](const TCHAR* Nm, UMaterialExpression* E)
			{ FCustomInput I; I.InputName = Nm; I.Input.Expression = E; Dome->Inputs.Add(I); };
			In(TEXT("WPos"), WP);
			In(TEXT("CamPos"), CamP);
			In(TEXT("Pano"), Pano);
			In(TEXT("Rot"), Rot);
			In(TEXT("VMinY"), VMin);
			In(TEXT("VMaxY"), VMax);
			In(TEXT("Scale"), Scale);
			UMaterialEditingLibrary::ConnectMaterialProperty(Dome, TEXT(""), MP_EmissiveColor);
		}

		M->PreEditChange(nullptr);
		M->PostEditChange();
		BF6_ReportMaterialState(TEXT("sky"), M);
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
	TComp* AddLightComp(AActor* A, USceneComponent* Root, int32 Index, const TCHAR* Kind)
	{
		TComp* C = NewObject<TComp>(
			A, *FString::Printf(TEXT("Light_%s_%d"), Kind, Index), RF_Transient);
		if (!C) return nullptr;
		C->SetupAttachment(Root);
		C->RegisterComponent();
		return C;
	}

	int32 BuildLights(AActor* A, USceneComponent* Root,
	                  const TArray<BF6HP::FCore::FLight>& Lights)
	{
		int32 Made = 0, SkippedDark = 0, Clamped = 0, Luminance = 0;
		double SumLm = 0.0;
		float MaxLm = 0.f;
		GAuthoredLights.Reset();
		GAuthoredLights.Reserve(Lights.Num());
		for (int32 i = 0; i < Lights.Num(); i++)
		{
			const BF6HP::FCore::FLight& L = Lights[i];
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
			Lum *= (L.Dimmer > 0.f ? L.Dimmer : 1.f);
			const float Authored = Lum;
			if (L.Unit == 1) Luminance++;
			SumLm += Authored;
			MaxLm = FMath::Max(MaxLm, Authored);
			if (Lum > GLightLumenMax) { Lum = GLightLumenMax; Clamped++; }
			// A light with no energy or no reach is not a light. Both occur in
			// the data and both would cost a component for nothing.
			if (Lum <= 0.f || L.AttenuationRadiusM <= 0.f) { SkippedDark++; continue; }

			// Same basis conversion the props take: Y and Z swap, and the
			// basis carries the holder's SCALE, so a row must be normalised
			// before it can be used as a direction.
			const FVector X(L.Right.X,   L.Right.Z,   L.Right.Y);
			const FVector Y(L.Up.X,      L.Up.Z,      L.Up.Y);
			const FVector Z(L.Forward.X, L.Forward.Z, L.Forward.Y);
			const FTransform Xf(FMatrix(X, Z, Y, ToUnreal(L.Origin)));

			ULightComponent* Base = nullptr;
			if (L.Type == 1)          // spot
			{
				if (USpotLightComponent* S =
					AddLightComp<USpotLightComponent>(A, Root, i, TEXT("Spot")))
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
					AddLightComp<URectLightComponent>(A, Root, i, TEXT("Rect")))
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
					AddLightComp<UPointLightComponent>(A, Root, i, TEXT("Point")))
				{
					Pt->SourceRadius = FMath::Max(0.f, L.ShapeRadiusM * 100.f);
					if (L.Type == 2) Pt->SourceLength = FMath::Max(0.f, L.TubeWidthM * 100.f);
					Base = Pt;
				}
			}
			if (!Base) continue;

			Base->SetWorldTransform(Xf);
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
			Base->SetIntensity(Lum);
			GAuthoredLights.Emplace(Base, Authored);
			// SHADOWS OFF regardless of the authored flag. Half of a map's
			// lights ask for shadows, and a few thousand shadow-casting
			// dynamic lights is not a preview, it is a slideshow. The flag is
			// decoded and kept in the core for whoever wants it.
			Base->SetCastShadows(false);
			Base->SetMobility(EComponentMobility::Movable);
			Base->SetVisibility(GLayers[(int32)ELayer::Lighting].bOn, false);
			Made++;
		}
		if (SkippedDark > 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("lights: %d skipped for zero intensity or zero reach"), SkippedDark);
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
							: FMath::Clamp(3.0f * FMath::Pow(Lux / 120000.f, 0.45f), 0.3f, 8.f);
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
						D->LightSourceAngle = FMath::Max(0.f, V.SunAngularRadiusDeg * 2.f);
						UE_LOG(LogBF6HighPoly, Log,
							TEXT("lighting: sun %.0f lux authored -> %.2f editor intensity"),
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
						MID->SetScalarParameterValue(TEXT("SkyRotationTurns"),
							V.SkyPanoramicRotationTurns);
						// THE SAME FACTOR THE SUN USED. See the sun block.
						const float Emissive =
							FMath::Max(0.f, V.SkyLuminanceScale) * SunLuxToEditor;
						MID->SetScalarParameterValue(TEXT("SkyEmissiveScale"), Emissive);
						SC->SetMaterial(0, MID);
						UE_LOG(LogBF6HighPoly, Log,
							TEXT("sky: panorama drawn, luminance scale %.0f -> emissive ")
							TEXT("%.4g, rotation %.3f turns"),
							V.SkyLuminanceScale, Emissive, V.SkyPanoramicRotationTurns);
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
				if (V.MieCoefficient > 0.f) Atm->MieScatteringScale = V.MieCoefficient;
				if (V.MieG != 0.f) Atm->MieAnisotropy = FMath::Clamp(V.MieG, -0.99f, 0.99f);
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
			UWorld* PW = GEditor->GetEditorWorldContext().World();
			APostProcessVolume* PP = PW ? PW->SpawnActor<APostProcessVolume>() : nullptr;
			if (PP)
			{
				PP->SetActorLabel(TEXT("Light_Exposure"));
				PP->Tags.Add(FName(*(FString(TEXT("addon:")) + kAddonName)));
				PP->SetFlags(RF_Transient);
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
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("lighting: exposure %s, authored ev %.2f (max %.2f), compensation %.2f"),
					V.bAutoExposure ? TEXT("automatic") : TEXT("manual"),
					V.ExposureEV, V.ExposureEVMax, V.ExposureCompensation);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Warning,
					TEXT("lighting: no post process volume, so the sun's real ")
					TEXT("photometric intensity will read as white"));
			}
		}

		if (USkyLightComponent* Sky =
			NewObject<USkyLightComponent>(A, TEXT("Light_SkyLight"), RF_Transient))
		{
			Sky->SetupAttachment(Root);
			Sky->Mobility = EComponentMobility::Movable;
			Sky->bRealTimeCapture = true;
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
		}
	}

	int32 GWaterBuilt = 0;

	int32 BuildWater(AActor* A, USceneComponent* Root,
	                 const TArray<BF6HP::FCore::FWater>& W, TArray<UStaticMesh*>& OutPending,
	                 const BF6HP::FCore::FTerrain* Ground)
	{
		GWaterBuilt = 0;
		// The water needs to know how deep it is to fade its swell into the
		// shore. Without the ground it simply does not fade, which is the
		// behaviour before this existed rather than a broken one.
		GWaterDepthTex = Ground ? MakeDepthTexture(*Ground) : nullptr;
		if (!GWaterDepthTex)
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("water: no heightfield, so no shore fade (turn Terrain on to get it)"));
		// One sim per level; every surface shares the sea state. bOcean below
		// tunes only the base wavelength.
		BF6HP::FCore::FWaterSim Sim;
		const bool bHaveSim = GCore.ReadWaterSim(BF6Ext::CurrentLevel(), Sim);
		if (bHaveSim)
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("ocean sim: %s, angle %.2f, speed %.3f, chop %.2f, %d lobe point(s)"),
				Sim.bFlagged ? TEXT("flagged") : TEXT("first"),
				Sim.WindAngle, Sim.WindSpeed, Sim.Choppiness, Sim.Dist.Num());
		for (int32 wi = 0; wi < W.Num(); wi++)
		{
			const BF6HP::FCore::FWater& S = W[wi];
			const float SizeM = (float)FMath::Max(S.Size.X, S.Size.Y);
			FWaveSet Waves = bHaveSim ? DeriveWaves(Sim, S.bOcean, SizeM) : FWaveSet();
			// The sea state, said out loud. Wave height is the one water number
			// a person can judge by eye against the real game, so the inputs
			// and the result belong in the log rather than in a guess.
			if (bHaveSim)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("water waves: wind speed %.4f, choppiness %.3f, tile %.2f m ")
					TEXT("-> amplitude %.3f m, chop %.2f, base wavelength %.1f m"),
					Sim.WindSpeed, Sim.Choppiness, Sim.TileDimension,
					Waves.Gain, Waves.Chop, Waves.BaseLen);
			}
			// A GRID DENSE ENOUGH TO DISPLACE. The Gerstner offset moves
			// vertices, so vertex spacing is the wave resolution: 12 m steps
			// give the ratio-1 swell (26-34 m) three-plus vertices per length,
			// and the shorter components ride in the per-pixel normal instead.
			// Capped at 512 a side - half a million triangles for a 10 km sea,
			// which draws fine without Nanite and never fights WPO.
			const int32 N = FMath::Clamp(
				(int32)(FMath::Max(S.Size.X, S.Size.Y) / 12.0), 64, 1024);
			// WHAT THE GRID CAN ACTUALLY CARRY. A wave shorter than a few
			// vertex spacings cannot be represented as geometry: sampled at
			// under two vertices per wavelength it does not become a small
			// wave, it becomes NOISE. On a 10 km ocean at 1024 a side that
			// is a vertex every 9.8 m, so anything under about 30 m has to
			// live in the per-pixel normal instead. This number is what the
			// material gates displacement on.
			Waves.VertexSpacingM = (float)(FMath::Max(S.Size.X, S.Size.Y) / (double)N);
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
					const double gxw = S.Center.X + fx * S.Size.X;
					const double gzw = S.Center.Y + fz * S.Size.Y;
					const FVertexID id = MD.CreateVertex();
					// game (x, y up, z) -> unreal (x, z, y up), metres -> cm
					VPos[id] = FVector3f((float)(gxw * 100.0), (float)(gzw * 100.0),
					                     (float)(S.Height * 100.0));
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
			SMat.MaterialInterface = WaterMaterialFor(SM, S, &Waves);
			SM->GetStaticMaterials().Add(SMat);
			// No Nanite: Single Layer Water and Nanite disagree, and 2,048
			// triangles need no cluster hierarchy.
			PrepareMesh(SM, MD, MD.Triangles().Num(), /*bAllowNanite*/ false);
			OutPending.Add(SM);

			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(
				A, *FString::Printf(TEXT("Water_%d"), wi));
			C->SetupAttachment(Root);
			C->RegisterComponent();
			C->SetStaticMesh(SM);
			C->SetMobility(EComponentMobility::Static);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			GWaterBuilt++;
			GBuiltAnything = true;
		}
		return GWaterBuilt;
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
	EMode GMode = EMode::Textured;

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
		BF6Ext::SetLowPolyMapHidden(GMode != EMode::LowPoly && GHideLowPoly && GBuiltAnything);
	}


	int32 BuildRoads(AActor* A, USceneComponent* Root, const BF6HP::FCore::FTerrain& T,
	                 const TArray<BF6HP::FCore::FDecal>& D, TArray<UStaticMesh*>& OutPending)
	{
		if (D.Num() == 0 || T.Size <= 1) return 0;
		const double YScale = T.HeightScale > 0.f
			? (double)T.HeightScale / 65536.0
			: FMath::Max(0.001, T.WorldMax.Y - T.WorldMin.Y) / 65535.0;

		GRoadRecords = GRoadTris = GRoadElevated = GRoadColourless = GRoadPainted = 0;
		GRoadTintClamped = 0;

		// One mesh per record. A record is one road segment or one painted
		// area, each with its own sheets, so merging them would mean one
		// material for surfaces that do not share one.
		for (int32 di = 0; di < D.Num(); di++)
		{
			const BF6HP::FCore::FDecal& d = D[di];
			const int32 nv = d.VertexCount;
			if (nv < 3 || nv % 3 != 0) continue;

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
					GRoadPainted++;          // colour is a constant: draw it
				}
				else
				{
					// No colour anywhere. Puddles and modulators need a decal
					// material that writes normal and roughness without
					// touching albedo, which is a separate piece of work.
					GRoadColourless++;
					continue;
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
			// The band is worth one thing only: telling a rooftop court or a
			// loading deck from a street. So it is applied only where the
			// authored floor stands well above the ground under this record's
			// own vertices.
			double GroundHi = -1e30;
			for (int32 v = 0; v < nv; v++)
				GroundHi = FMath::Max(GroundHi,
					GroundAt(T, YScale, d.Verts[v * 8], d.Verts[v * 8 + 1]));
			const bool bElevated = (double)d.AabbMin.Y - GroundHi > 2.0;
			if (bElevated) GRoadElevated++;

			FMeshDescription MD;
			FStaticMeshAttributes Attr(MD);
			Attr.Register();
			TVertexAttributesRef<FVector3f>         VPos = Attr.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector2f> VUV  = Attr.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> VCol = Attr.GetVertexInstanceColors();
			const FPolygonGroupID Group = MD.CreatePolygonGroup();
			Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("S0");

			TArray<FVertexID> V;
			V.SetNumUninitialized(nv);
			MD.ReserveNewVertices(nv);
			for (int32 v = 0; v < nv; v++)
			{
				const float* p = d.Verts + v * 8;
				const double gx = p[0], gz = p[1];
				// Draped, then lifted clear. Without the lift the decal shares
				// a plane with the ground and z-fights across the whole map.
				double gy = bElevated
					? FMath::Clamp(GroundAt(T, YScale, gx, gz), (double)d.AabbMin.Y, (double)d.AabbMax.Y)
					: GroundAt(T, YScale, gx, gz);
				const FVertexID id = MD.CreateVertex();
				VPos[id] = FVector3f((float)(gx * 100.0), (float)(gz * 100.0),
				                     (float)(gy * 100.0 + GRoadLift));
				V[v] = id;
			}

			MD.ReserveNewPolygons(nv / 3);
			for (int32 t = 0; t + 2 < nv; t += 3)
			{
				auto Corner = [&](int32 v)
				{
					const float* p = d.Verts + v * 8;
					const FVertexInstanceID vi = MD.CreateVertexInstance(V[v]);
					float u = p[2], w = p[3];
					// A PLANAR FILL STORES WORLD X AND Z VERBATIM rather than
					// authoring u across and v along, and tiles by the record's
					// own tilings. Left alone it would sample one texel for a
					// whole car park.
					if (d.bPlanar)
					{
						const float t0 = FMath::Abs(d.Tiling0) > 1e-3f ? d.Tiling0 : 10.f;
						const float t1 = FMath::Abs(d.Tiling1) > 1e-3f ? d.Tiling1 : t0;
						u = p[2] / t1;
						w = p[3] / t0;
					}
					VUV.Set(vi, 0, GRoadUvSwapped ? FVector2f(w, u) : FVector2f(u, w));
					// VERTEX ALPHA IS AUTHORED BLENDING, NOT A FLAG. Roughly a
					// tenth of vertices sit strictly between 0 and 1 - those are
					// the mud and dirt edge fades. Dropped, every decal ends on
					// a hard boundary instead of blending into the terrain.
					VCol.Set(vi, 0, FVector4f(p[4], p[5], p[6], p[7]));
					return vi;
				};
				// FACE UP. Observed upside down: visible from underneath and
				// invisible from above, which is a face pointing at the
				// ground.
				//
				// The reasoning that put a reversal here is sound in general -
				// mapping game (x, y, z) to Unreal (x, z, y) swaps two axes,
				// flips handedness and so flips winding - but the decal
				// vertices do not arrive in the same order a mesh's do, so
				// applying it here reversed something that was already
				// correct. Two reversals is none.
				//
				// Winding, not a two-sided material: a two-sided fix would
				// make them visible again while leaving every normal pointing
				// down, so they would light as though the sun were under the
				// map.
				MD.CreatePolygon(Group, TArray<FVertexInstanceID>{
					Corner(t), Corner(t + 1), Corner(t + 2) });
			}

			FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
			FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals);

			UStaticMesh* SM = NewObject<UStaticMesh>(
				A, *FString::Printf(TEXT("Road_%d"), di), RF_Transient);
			FStaticMaterial SMat;
			SMat.MaterialSlotName = TEXT("S0");
			SMat.ImportedMaterialSlotName = SMat.MaterialSlotName;
			SMat.MaterialInterface = RoadMaterialFor(SM, d);
			SM->GetStaticMaterials().Add(SMat);

			// NO NANITE ON THESE. They are masked, they are thin, and they are
			// a few dozen triangles each - there is no cluster hierarchy worth
			// building and the build cost would be paid hundreds of times.
			PrepareMesh(SM, MD, MD.Triangles().Num(), /*bAllowNanite*/ false);
			OutPending.Add(SM);

			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(
				A, *FString::Printf(TEXT("RoadMesh_%d"), di));
			C->SetupAttachment(Root);
			C->RegisterComponent();
			C->SetStaticMesh(SM);
			C->SetMobility(EComponentMobility::Static);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);          // paint on the ground casts none
			GRoadRecords++;
			GBuiltAnything = true;
			GRoadTris += nv / 3;
		}
		if (GRoadTintClamped > 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("decals: %d record(s) had an authored tint above 1.0 over a ")
				TEXT("colour sheet and were clamped - unclamped they saturate to ")
				TEXT("white, which is what the carrier decks were doing"),
				GRoadTintClamped);
		}
		if (GRoadPainted > 0 || GRoadColourless > 0)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("decals: %d record(s) painted from an authored colour ")
				TEXT("constant, %d still skipped (no colour anywhere - puddles ")
				TEXT("and modulators, which need a decal material)"),
				GRoadPainted, GRoadColourless);
		}
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
	// Decal meshes: a quad whose whole job is the sheet stuck on it.
	bool IsDecalPath(const FString& Mesh)
	{
		return Mesh.Contains(TEXT("/decals/"), ESearchCase::IgnoreCase)
		    || Mesh.Contains(TEXT("/decal/"), ESearchCase::IgnoreCase);
	}
	int32 GDecalSectionsDropped = 0, GDecalMeshesDropped = 0;

	bool DrawsWhiteWithNoSheet(const FString& Mesh)
	{
		static const TCHAR* kNoSheet[] = {
			TEXT("bd_eas_oceanhorizon_01"),
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

	int32 BuildLevelGeometry(const TArray<BF6HP::FPlacement>& P, int32& OutMeshes, int32& OutFailed,
	                         FScopedSlowTask* Task)
	{
		OutMeshes = OutFailed = 0;
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return 0;

		BF6Ext::ClearAddonActors(kAddonName);
		GBuiltAnything = false;

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
			TArray<const BF6HP::FPlacement*> Rows;
		};
		// WHICH MODE ARE WE BUILDING. Counted first, because the default is
		// "the biggest", and because the list is worth printing either way -
		// a user who wants Conquest cannot ask for it if nothing ever said
		// Conquest was there.
		TMap<FString, int32> ModeCount;
		GFarWorldRadiusCm = 0.f;
		for (const BF6HP::FPlacement& p : P)
		{
			const FString M = GameModeOf(p.Bundle);
			if (!M.IsEmpty()) ModeCount.FindOrAdd(M)++;
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
		}

		TMap<FString, FGroup> ByMesh;
		int32 ModeSkipped = 0;
		int32 WhiteSkipped = 0;
		GDecalSectionsDropped = GDecalMeshesDropped = 0;
		ON_SCOPE_EXIT
		{
			if (ModeSkipped > 0)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("game modes: %d placement(s) skipped as belonging to a ")
					TEXT("mode other than the one being built"), ModeSkipped);
			}
			if (GDecalSectionsDropped > 0 || GDecalMeshesDropped > 0)
			{
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("decal meshes: %d section(s) dropped for binding no ")
					TEXT("colour sheet, taking %d whole mesh(es) with them - a ")
					TEXT("decal with no colour draws white, and in game it is ")
					TEXT("simply not there"),
					GDecalSectionsDropped, GDecalMeshesDropped);
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
			// Shared art has no mode and is always built.
			if (!bAllModes)
			{
				const FString M = GameModeOf(p.Bundle);
				if (!M.IsEmpty() && !M.Equals(Chosen, ESearchCase::IgnoreCase))
				{
					ModeSkipped++;
					continue;
				}
			}
			FString Key = p.Mesh;
			FString Var;
			if (!p.Variation.IsEmpty() &&
			    GCore.VariationLive(BF6HP::FCore::MeshResourceFor(p.Mesh), p.Bundle, p.Variation))
			{
				Var = p.Variation;
				Key += TEXT("|") + p.Variation + TEXT("|") + p.Bundle;
			}
			FGroup& G = ByMesh.FindOrAdd(Key);
			if (G.Rows.Num() == 0) { G.Mesh = p.Mesh; G.Bundle = p.Bundle; G.Variation = Var; }
			G.Rows.Add(&p);
		}

		TArray<FString> Order;
		ByMesh.GetKeys(Order);
		Order.Sort([&ByMesh](const FString& a, const FString& b)
			{ return ByMesh[a].Rows.Num() > ByMesh[b].Rows.Num(); });
		// The pills only turn on what is needed, and OFF means not built, not
		// merely hidden: a layer nobody asked for costs nothing.
		if (!GLayers[(int32)ELayer::Objects].bOn) Order.Empty();

		// WHERE THE TIME GOES, split so the levers are separable. Decoding, turning
		// the decode into a mesh description, and Unreal's own build want
		// completely different fixes and a total says nothing about which.
		double SecDecode = 0, SecDescribe = 0, SecBuild = 0;
		GNaniteCountThisBuild = GNoNaniteTranslucent = 0;
		GTexUploaded = GTexRefused = GMidsMade = GBindingsSeen = GBindingsBound = 0;
		TArray<UStaticMesh*> Pending;

		AActor* A = W->SpawnActor<AActor>();
		if (!A) return 0;
		A->SetActorLabel(TEXT("HighPoly"));
		BF6Ext::MarkAddonActor(A, kAddonName);
		USceneComponent* Root = NewObject<USceneComponent>(A, TEXT("Root"));
		A->SetRootComponent(Root);
		Root->RegisterComponent();

		// The ground first, so a long prop build still leaves something to
		// stand on if it is interrupted.
		// THE GROUND IS READ ONCE AND KEPT, because the roads need it too: a
		// decal carries no height of its own and is draped on whatever ground
		// we built, so the two cannot be read independently without the second
		// one sampling a different surface from the one it sits on.
		BF6HP::FCore::FTerrain Ground;
		bool bHaveGround = false;
		const bool bWantTerrain = GLayers[(int32)ELayer::Terrain].bOn;
		const bool bWantRoads   = GLayers[(int32)ELayer::Roads].bOn;
		if (bWantTerrain || bWantRoads)
		{
			if (Task) Task->EnterProgressFrame(3.f, LOCTEXT("Ground", "Building the ground"));
			bHaveGround = GCore.ReadTerrain(BF6Ext::CurrentLevel(), Ground);
			if (bHaveGround && bWantTerrain)
			{
				const int32 side = BuildTerrain(A, Root, Ground, Pending);
				// Metres per vertex, not just the vertex count: the count alone says
				// nothing without the size of the map it is spread over.
				const double MPerVert = side > 1
					? (Ground.WorldMax.X - Ground.WorldMin.X) / (side - 1) : 0.0;
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("terrain: %d native -> %d built, %.0f m across at %.2f m per vertex, %d tile(s)"),
					Ground.Size, side, Ground.WorldMax.X - Ground.WorldMin.X, MPerVert, GTerrainTilesBuilt);
			}
			else if (!bHaveGround)
			{
				UE_LOG(LogBF6HighPoly, Warning, TEXT("terrain: %s"), *GCore.Error);
			}
		}

		// ROADS, on top of the ground and only if there is ground to drape on.
		if (bHaveGround && GLayers[(int32)ELayer::Roads].bOn)
		{
			if (Task) Task->EnterProgressFrame(2.f, LOCTEXT("Roads", "Draping the roads"));
			TArray<BF6HP::FCore::FDecal> Decals;
			if (GCore.ReadDecals(BF6Ext::CurrentLevel(), Decals))
			{
				const int32 built = BuildRoads(A, Root, Ground, Decals, Pending);
				UE_LOG(LogBF6HighPoly, Log,
					TEXT("roads: %d record(s) of %d, %d triangle(s), %d elevated onto a deck"),
					built, Decals.Num(), GRoadTris, GRoadElevated);
			}
			else
			{
				// Not every level ships decals, and an older core has no entry
				// point for them. Neither is a failure of the build.
				UE_LOG(LogBF6HighPoly, Log, TEXT("roads: none for this level"));
			}
		}

		// WATER. Flat planes at the level's own heights and colours, through
		// Unreal's Single Layer Water - reflections and depth for free.
		if (GLayers[(int32)ELayer::Water].bOn)
		{
			if (Task) Task->EnterProgressFrame(1.f, LOCTEXT("Water", "Laying the water"));
			TArray<BF6HP::FCore::FWater> Water;
			if (GCore.ReadWater(BF6Ext::CurrentLevel(), Water))
			{
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
				UE_LOG(LogBF6HighPoly, Log, TEXT("water: %d surface(s)"), GWaterBuilt);
			}
			else
			{
				// Most levels author no water entity; some carry theirs in the
				// backdrop instead. Neither is a failure of the build.
				UE_LOG(LogBF6HighPoly, Log, TEXT("water: none for this level"));
			}
		}

		if (GLayers[(int32)ELayer::Lighting].bOn)
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

			TArray<BF6HP::FCore::FLight> Lights;
			FString Summary;
			if (GCore.ReadLights(BF6Ext::CurrentLevel(), Lights, Summary))
			{
				const double T0 = FPlatformTime::Seconds();
				GLightsBuilt = BuildLights(A, Root, Lights);
				UE_LOG(LogBF6HighPoly, Log, TEXT("lights: %s, %d placed, %.1fs"),
					*Summary, GLightsBuilt, FPlatformTime::Seconds() - T0);
			}
			else
			{
				UE_LOG(LogBF6HighPoly, Warning, TEXT("lights: %s"), *GCore.Error);
			}
			if (GLightsBuilt > 0) GBuiltAnything = true;
		}

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

		int32 Placed = 0, Built = 0;
		for (int32 First = 0; First < Order.Num(); First += kBatch)
		{
			if (Task)
			{
				if (Task->ShouldCancel())
				{
					// Stop where we are and keep what is built. A half-built map
					// is more useful than none, and CLEAR removes it in one go.
					UE_LOG(LogBF6HighPoly, Log, TEXT("cancelled after %d mesh(es)"), Built);
					break;
				}
				Task->EnterProgressFrame(65.f * (float)kBatch / (float)FMath::Max(1, Order.Num()),
					FText::FromString(FString::Printf(TEXT("Building geometry  %d / %d"),
						First, Order.Num())));
			}
			const int32 Last = FMath::Min(First + kBatch, Order.Num());
			const int32 N = Last - First;

			TArray<TArray<BF6HP::FCore::FSection>> Decoded;
			TArray<FString> Names;
			Decoded.Reserve(N);
			Names.Reserve(N);

			double T = FPlatformTime::Seconds();
			for (int32 i = First; i < Last; i++)
			{
				TArray<BF6HP::FCore::FSection> Sections;
				// The group's own scope: its placing bundle AND its variation.
				// The variation is what makes a livery a livery - the variant
				// record carries the delta and the core merges it over the
				// base. Ungated groups carry an empty variation and read
				// exactly as before.
				const FGroup& G = ByMesh[Order[i]];
				if (!GCore.ReadMesh(BF6HP::FCore::MeshResourceFor(G.Mesh), Sections,
				                    G.Bundle, G.Variation))
				{
					OutFailed++;
					continue;
				}
				// A DECAL WITH NO COLOUR SHEET IS INVISIBLE IN GAME, NOT WHITE.
				//
				// Decal meshes carry their colour through a slot this chain
				// does not resolve - the materials are named M_Decal, M_Decals
				// and M_CarrierFlightDeckDecal_01 - so their base colour falls
				// back to the parent's white default and they draw as solid
				// white quads. On MP_Isolated that is 770 instances over four
				// meshes, and on a carrier deck it is a field of white squares
				// at head height.
				//
				// Scoped to the decal PATH on purpose. The same census counts
				// 4,349 unbound instances over 39 meshes that are NOT decals -
				// floor plates, windows, signage - and those are real surfaces
				// that should still draw, white or not, because absent
				// geometry there would be a hole rather than a missing
				// sticker.
				if (IsDecalPath(G.Mesh))
				{
					const int32 Before = Sections.Num();
					Sections.RemoveAll([](const BF6HP::FCore::FSection& S)
					{
						for (const BF6HP::FCore::FBinding& B : S.Textures)
							if (B.Slot == 0 && B.Texture >= 0) return false;
						return true;
					});
					GDecalSectionsDropped += Before - Sections.Num();
					if (Sections.Num() == 0) { GDecalMeshesDropped++; continue; }
				}
				Decoded.Add(MoveTemp(Sections));
				Names.Add(Order[i]);
			}
			SecDecode += FPlatformTime::Seconds() - T;

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
			});
			SecDescribe += FPlatformTime::Seconds() - T;

			for (int32 i = 0; i < Decoded.Num(); i++)
			{
				if (!Ok[i]) { OutFailed++; continue; }

				UStaticMesh* SM = MakeMesh(A, FString::Printf(TEXT("M_%d"), Built), Descs[i], Tris[i], Decoded[i]);
				if (!SM) { OutFailed++; continue; }
				Pending.Add(SM);
				Built++;

				UHierarchicalInstancedStaticMeshComponent* H =
					NewObject<UHierarchicalInstancedStaticMeshComponent>(A, *FString::Printf(TEXT("I_%d"), Built));
				H->SetupAttachment(Root);
				H->RegisterComponent();
				H->SetStaticMesh(SM);
				H->SetMobility(EComponentMobility::Static);
				H->SetCollisionEnabled(ECollisionEnabled::NoCollision);

				TArray<FTransform> Xf;
				for (const BF6HP::FPlacement* p : ByMesh[Names[i]].Rows)
				{
					// The basis carries scale as well as rotation, so it goes
					// across as a matrix. Y and Z swap to match the vertices;
					// the columns are reordered the same way so the two stay in
					// step.
					const FVector X(p->Right.X,   p->Right.Z,   p->Right.Y);
					const FVector Y(p->Up.X,      p->Up.Z,      p->Up.Y);
					const FVector Z(p->Forward.X, p->Forward.Z, p->Forward.Y);
					Xf.Add(FTransform(FMatrix(X, Z, Y, ToUnreal(p->Origin))));
				}
				H->AddInstances(Xf, false);
				Placed += Xf.Num();
				if (Xf.Num() > 0) GBuiltAnything = true;
			}
		}

		// Everything built together, on every core, once.
		if (Task) Task->EnterProgressFrame(2.f, LOCTEXT("Handing", "Handing the meshes to Unreal"));
		const double TB = FPlatformTime::Seconds();
		BuildAll(Pending);
		SecBuild = FPlatformTime::Seconds() - TB;

		// DISPATCHED, NOT FINISHED. BatchBuild hands the meshes to Unreal's
		// asynchronous builder and returns; on MP_Battery it returned in 0.4 s
		// while 292 seconds of Nanite work carried on across the cores for
		// another half minute. Reporting that half second as the build time was
		// a comfortable lie, so the number is named for what it measures.
		UE_LOG(LogBF6HighPoly, Log,
			TEXT("build: decode %.1fs, describe %.1fs, dispatch %.1fs (%d meshes, %d nanite, %d translucent so no nanite, %d placements)"),
			SecDecode, SecDescribe, SecBuild, Built, GNaniteCountThisBuild,
			GNoNaniteTranslucent, Placed);

		UE_LOG(LogBF6HighPoly, Log,
			TEXT("materials: %d instance(s), %d binding(s) of which %d bound; textures %d uploaded, %d refused"),
			GMidsMade, GBindingsSeen, GBindingsBound, GTexUploaded, GTexRefused);

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
	void StartRead()
	{
		FSlateApplication::Get().DismissAllMenus();
		if (!GEditor) return;
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

	// Read the open map out of the game and mark every placement.
	//
	// THE LEVEL THE CACHES BELONG TO. Everything below is per map.
	FString GBuiltLevel;

	template <typename T>
	void ReleaseRooted(T*& Ptr)
	{
		if (Ptr)
		{
			if (Ptr->IsRooted()) Ptr->RemoveFromRoot();
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
		// Not unrooted: these were never rooted. They stay alive through the
		// material instances that reference them, and those go with the
		// actors.
		GTextureCache.Empty();

		ReleaseRooted(GGroundAlbedo);
		ReleaseRooted(GGroundFar);
		ReleaseRooted(GGroundNormal);
		ReleaseRooted(GCovIdxTex);
		ReleaseRooted(GCovWTex);
		ReleaseRooted(GCovParamTex);
		ReleaseRooted(GCovColourTex);
		ReleaseRooted(GSheetArray);
		ReleaseRooted(GHeightArray);
		ReleaseRooted(GWaterDepthTex);

		GroundMat = nullptr;
		GBuiltAnything = false;
		GTexUploaded = GTexRefused = GMidsMade = GBindingsSeen = GBindingsBound = 0;
		GRoadRecords = GRoadTris = GRoadElevated = GRoadColourless = GRoadPainted = 0;
		GRoadTintClamped = 0;
		GWaterBuilt = 0;
		GLightsBuilt = 0;
		GStatus.Reset();
		GBuiltLevel.Reset();

		// AND GIVE THE LOW-POLY MAP BACK.
		//
		// ApplyLowPoly hides the tool's own map whenever GHideLowPoly and
		// GBuiltAnything are both set. GBuiltAnything used to survive a map
		// load, so opening a second map left the tool convinced High Poly was
		// built, hid that map's low-poly geometry, and showed nothing in its
		// place. Clearing the flag is not enough on its own: the hide has
		// already been applied to the new map and has to be undone.
		ApplyLowPoly();
	}

	// SYNCHRONOUS, and it takes about half a minute the first time: mounting a
	// level's archives and indexing every partition's guid is most of it, and
	// both are cached in the core afterwards. Said out loud in the status line
	// rather than hidden behind a spinner that suggests otherwise.
	void ReadLevel()
	{
		const FString Level = BF6Ext::CurrentLevel();
		if (Level.IsEmpty()) { GStatus = TEXT("no map open"); return; }

		// A DIFFERENT MAP MEANS THE CACHES ARE LIES. See ResetPerMapState.
		if (!GBuiltLevel.IsEmpty() && GBuiltLevel != Level)
		{
			UE_LOG(LogBF6HighPoly, Log,
				TEXT("map changed %s -> %s, dropping the previous map's textures"),
				*GBuiltLevel, *Level);
			ResetPerMapState();
		}
		GBuiltLevel = Level;

		const FString Install = EnsureInstall();
		if (Install.IsEmpty())
		{
			GStatus = TEXT("no game folder chosen");
			BF6Ext::Notify(TEXT("High Poly needs to know where Battlefield 6 is installed."));
			return;
		}

		const FString Dll = FPaths::Combine(BF6Ext::ToolPluginDir(),
			TEXT("Source/ThirdParty/libbf6/bin/Win64/bf6_core.dll"));
		if (!GCore.IsOpen() && !GCore.Open(Install, Dll))
		{
			GStatus = GCore.Error;
			BF6Ext::Notify(FString::Printf(TEXT("High Poly: %s"), *GCore.Error));
			return;
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

		FScopedSlowTask Task(100.f, LOCTEXT("Reading", "High Poly"));
		Task.MakeDialog(true);   // with a cancel button

		const FString Exe = FPaths::Combine(Install, TEXT("bf6.exe"));
		GCore.Progress.bCancel = false;

		TFuture<bool> Read = Async(EAsyncExecution::Thread, [Level, Exe]
		{
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

		if (!Read.Get())
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
		const double T0 = FPlatformTime::Seconds();
		GLastCount = BuildLevelGeometry(P, Meshes, Failed, &Task);
		const double BuildS = FPlatformTime::Seconds() - T0;
		ApplyLowPoly();
		ApplyMode();
		ApplyWind();
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
		GStatus = FString::Printf(TEXT("%d of %d placements, %d mesh(es)%s, %.0fs%s"),
			GLastCount, P.Num(), Meshes,
			Failed ? *FString::Printf(TEXT(", %d not geometry"), Failed) : TEXT(""),
			BuildS, GNanite ? TEXT(" + nanite still building") : TEXT(""));
		UE_LOG(LogBF6HighPoly, Log, TEXT("%s: %s"), *Level, *GStatus);
		BF6Ext::Notify(FString::Printf(TEXT("High Poly: %s"), *GStatus));
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
			[]{ GGameMode.Reset(); },
			TEXT("Build the mode with the most placements, which is the map's headline mode. Rebuild to apply.")) ]; 
		for (const TPair<FString, int32>& M : GModesFound)
		{
			const FString Name = M.Key;
			Box->AddSlot()
			[ Pill(FString::Printf(TEXT("%s (%d)"), *Name, M.Value),
				TAttribute<bool>::CreateLambda([Name]
					{ return GGameMode.Equals(Name, ESearchCase::IgnoreCase); }),
				[Name]{ GGameMode = Name; },
				FString::Printf(TEXT("Build only this mode's props: %d placement(s). ")
					TEXT("Rebuild to apply."), M.Value)) ];
		}
		Box->AddSlot()
		[ Pill(TEXT("All"),
			TAttribute<bool>::CreateLambda([]
				{ return GGameMode.Equals(TEXT("all"), ESearchCase::IgnoreCase); }),
			[]{ GGameMode = TEXT("all"); },
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
							TAttribute<bool>::CreateLambda([]{ return !GHideLowPoly; }),
							[]{ GHideLowPoly = !GHideLowPoly; ApplyMode(); },
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
								BF6Ext::ClearAddonActors(kAddonName);
								GLastCount = 0;
								GBuiltAnything = false;
								GStatus = TEXT("cleared");
								ApplyLowPoly();     // and the low-poly map comes back
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
            if (!C || !C->GetName().StartsWith(TEXT("Water_"))) continue;
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

static FAutoConsoleCommand GGroundDebugCmd(
    TEXT("BF6.HighPoly.GroundDebug"),
    TEXT("0 normal, 1 dominant layer as flat colour, 2 aerial map only, "
         "3 near blend only, 4 far bake only, 5 layers per texel."),
    FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
    {
        if (Args.Num() < 1) { UE_LOG(LogBF6HighPoly, Display,
            TEXT("usage: BF6.HighPoly.GroundDebug <0-5>")); return; }
        const float V = FCString::Atof(*Args[0]);
        int32 n = 0;
        BF6_ForEachGroundMaterial([&](UMaterialInstanceDynamic* MID)
        { MID->SetScalarParameterValue(TEXT("DebugMode"), V); n++; });
        UE_LOG(LogBF6HighPoly, Display,
            TEXT("ground debug mode %.0f on %d material(s)"), V, n);
    }));

static FAutoConsoleCommand GGroundPhotoCmd(
    TEXT("BF6.HighPoly.GroundPhoto"),
    TEXT("0..1: how much of the ground colour comes from the level's aerial map "
         "rather than the layer sheets. 0 is sheets only, 1 is the photograph "
         "modulated by sheet detail."),
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
            TEXT("game mode set to \"%s\" - rebuild the map to apply it"),
            GGameMode.IsEmpty() ? TEXT("(largest)") : *GGameMode);
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
            const float Lum = FMath::Min(P.Value, V);
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
		Report(TEXT("water"), EnsureWaterMaterial(), bForce);
		Report(TEXT("sky"), EnsureSkyMaterial(), bForce);
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

static FAutoConsoleCommand GCheckMaterialsCmd(
	TEXT("BF6.HighPoly.CheckMaterials"),
	TEXT("Build the High Poly ground materials and log whether they compiled."),
	FConsoleCommandDelegate::CreateStatic(&BF6_CheckMaterials));

// Reset when the EDITOR opens a map, not when we next happen to build.
//
// ReadLevel already drops stale caches when it notices the level changed, but
// that only fires if the user builds again. Everything between opening the map
// and that build ran on the previous map's state.
FDelegateHandle GMapOpenedHandle;

void FBF6HighPolyModule::StartupModule()
{
	GMapOpenedHandle = FEditorDelegates::OnMapOpened.AddLambda(
		[](const FString& /*Filename*/, bool /*bAsTemplate*/)
		{
			UE_LOG(LogBF6HighPoly, Log, TEXT("map opened, resetting High Poly state"));
			ResetPerMapState();
		});

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
				FPlatformMisc::RequestExit(false);
			}
		});
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

		// THE RING, not a panel: the same wheel the rest of the tool speaks,
		// with each pill's state answering on its own sub line. Toggles hold
		// the wheel open; BUILD and PANEL close it like any other pick.
		TArray<BF6Ext::FPieSubEntry> R;

		auto Mode = [&R](EMode M, const TCHAR* Label, const TCHAR* Means)
		{
			BF6Ext::FPieSubEntry E;
			E.Label = Label;
			E.Sub = [M, Means]{ return GMode == M ? FString(TEXT("current")) : FString(Means); };
			E.OnPick = [M]{ GMode = M; ApplyMode(); };
			R.Add(E);
		};
		Mode(EMode::LowPoly,  TEXT("LOW-POLY"), TEXT("just your map"));
		Mode(EMode::Clay,     TEXT("CLAY"),     TEXT("study grey"));
		Mode(EMode::Textured, TEXT("TEXTURED"), TEXT("the full thing"));

		auto Layer = [&R](ELayer L)
		{
			const int32 i = (int32)L;
			BF6Ext::FPieSubEntry E;
			E.Label = GLayers[i].Name;
			E.Sub = [i]{ return FString(GLayers[i].bOn ? TEXT("on") : TEXT("off")); };
			E.OnPick = [i, L]{ GLayers[i].bOn = !GLayers[i].bOn; ApplyLayer(L); };
			R.Add(E);
		};
		Layer(ELayer::Terrain);
		Layer(ELayer::Roads);
		Layer(ELayer::Objects);
		Layer(ELayer::Water);

		{
			BF6Ext::FPieSubEntry E;
			E.Label = TEXT("NANITE");
			E.Sub = []{ return FString(GNanite ? TEXT("on - next build") : TEXT("off")); };
			E.OnPick = []{ GNanite = !GNanite; };
			R.Add(E);
		}
		{
			BF6Ext::FPieSubEntry E;
			E.Label = TEXT("WIND");
			E.Sub = []{ return FString(GWind ? TEXT("swaying") : TEXT("still")); };
			E.OnPick = []{ GWind = !GWind; ApplyWind(); };
			R.Add(E);
		}
		{
			BF6Ext::FPieSubEntry E;
			E.Label = TEXT("LOW-POLY MAP");
			E.Sub = []{ return FString(!GHideLowPoly ? TEXT("visible") : TEXT("hidden")); };
			E.OnPick = []{ GHideLowPoly = !GHideLowPoly; ApplyMode(); };
			R.Add(E);
		}
		{
			BF6Ext::FPieSubEntry E;
			E.Label = TEXT("BUILD");
			E.Sub = []{ return FString(GBuiltAnything
				? TEXT("rebuild from the game") : TEXT("read your install")); };
			E.OnPick = []{ StartRead(); };
			E.bCloses = true;
			R.Add(E);
		}
		{
			BF6Ext::FPieSubEntry E;
			E.Label = TEXT("PANEL");
			E.Sub = []{ return FString(TEXT("status and setup")); };
			E.OnPick = [Center]
			{
				const FString Now = EnsureInstall();
				BF6Ext::ShowPopup(Now.IsEmpty() ? MakeSetupPanel() : MakeStatusPanel(Now), Center);
			};
			E.bCloses = true;
			R.Add(E);
		}

		BF6Ext::OpenPieSubRing(R, Center);
	};
	BF6Ext::RegisterPieEntry(Entry);

	UE_LOG(LogBF6HighPoly, Log,
		TEXT("High Poly add-on attached (tool extension API v%d). Install: %s"),
		BF6Ext::ApiVersion(),
		BF6Ext::GameInstallDir().IsEmpty() ? TEXT("none") : *BF6Ext::GameInstallDir());
}

void FBF6HighPolyModule::ShutdownModule()
{
	if (GMapOpenedHandle.IsValid())
	{
		FEditorDelegates::OnMapOpened.Remove(GMapOpenedHandle);
		GMapOpenedHandle.Reset();
	}

	BF6Ext::UnregisterPieEntry(FName("HighPoly.Root"));
	// Anything we spawned goes with us: the tool never owned it.
	if (GIsRunning) BF6Ext::ClearAddonActors(kAddonName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBF6HighPolyModule, BF6HighPoly)
