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
		Tex->NeverStream = true;
		Tex->CompressionSettings = T.bSrgb ? TC_Default : TC_Masks;

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
				Kind == EKind::Vista ? SAMPLERTYPE_LinearColor : SAMPLERTYPE_Normal, 300);

		// A parameter with no default texture compiles to nothing useful, so
		// each gets a neutral one: white for colour, flat for the normal.
		if (Base) Base->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		// The vista NSM is sampled linear, and a linear sampler with the
		// normal-compressed default fails the same compile-time check the
		// sRGB white does - so it defaults to our linear white instead. It is
		// never sampled once a real sheet is bound.
		if (Norm) Norm->Texture = Kind == EKind::Vista
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
		if (Norm && Kind == EKind::Vista)
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
		if (Kind != EKind::Vista)
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
					UMaterialEditingLibrary::ConnectMaterialExpressions(Cov, TEXT("R"), OpMul, TEXT("A"));
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
			// Glass has no cutout sheet; its transparency is a constant the
			// depot carries. A flat value is the honest first pass rather than
			// inventing a per-pixel one.
			UMaterialExpressionScalarParameter* Op =
				Cast<UMaterialExpressionScalarParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						M, UMaterialExpressionScalarParameter::StaticClass(), -400, 600));
			if (Op)
			{
				Op->ParameterName = TEXT("Opacity");
				Op->DefaultValue = 0.25f;
				UMaterialEditingLibrary::ConnectMaterialProperty(Op, TEXT(""), MP_Opacity);
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
	enum class ELayer : uint8 { Terrain, Roads, Objects, Water, Count };

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
	};

	bool GHideLowPoly = true;

	// Components are named by layer so a switch can find them again without
	// holding pointers across a rebuild, which is what would dangle when a map
	// closes underneath us.
	const TCHAR* LayerPrefix(ELayer L)
	{
		return L == ELayer::Terrain ? TEXT("Terrain")
			 : L == ELayer::Roads   ? TEXT("RoadMesh_")
			 : L == ELayer::Water   ? TEXT("Water_") : TEXT("I_");
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

	int32 BuildTerrain(AActor* A, USceneComponent* Root, const BF6HP::FCore::FTerrain& T,
	                   TArray<UStaticMesh*>& OutPending)
	{
		if (T.Size <= 1) return 0;

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
			SM->GetStaticMaterials().Add(FStaticMaterial());
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

	// ---- water ----------------------------------------------------------
	//
	// Its own parent rather than a seventh EKind: water is not a per-section
	// material - it is Unreal's Single Layer Water shading model, which wants
	// an opaque blend and a dedicated output node, and none of the generic
	// texture parameters. The Godot plugin draws its water as a rippled quad
	// with preset colours; here the engine does the real work - depth-based
	// absorption and scattering, screen-space refraction, reflections - and
	// the mined per-map colours drive the coefficients.
	UMaterial* GWaterParent = nullptr;

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
		if (SLW && Scatter)
			UMaterialEditingLibrary::ConnectMaterialExpressions(Scatter, TEXT(""), SLW, TEXT("ScatteringCoefficients"));
		if (SLW && Absorb)
			UMaterialEditingLibrary::ConnectMaterialExpressions(Absorb, TEXT(""), SLW, TEXT("AbsorptionCoefficients"));

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
		R.Gain = FMath::Clamp(
			(0.45f + 1.60f * FMath::Sqrt(FMath::Clamp(S.Choppiness, 0.f, 3.f))) *
			FMath::Clamp(FMath::Sqrt(FMath::Max(S.WindSpeed, 0.f) / 0.07f), 0.55f, 2.2f),
			0.30f, 4.0f);
		// Choppiness -> crest sharpening, clamped below 1 or the phase warp
		// folds crests into themselves.
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
	// two numbers are CALIBRATION and are labelled as such so nobody later
	// mistakes them for something recovered from the game.
	float ScatterPerM = 0.60f;
	float AbsorbPerM  = 0.55f;

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

		// The surface albedo the deferred pass would receive in the game.
		// Hue keeps the map's character; the brightness floor stops a dark
		// authored colour from rendering as a black mirror, which is what
		// happened when this was left at its near-black default.
		MID->SetVectorParameterValue(TEXT("SurfaceTint"), FLinearColor(
			Hue.R * FMath::Max(Bright, 0.35f),
			Hue.G * FMath::Max(Bright, 0.35f),
			Hue.B * FMath::Max(Bright, 0.35f)));

		// SCATTERING is what comes back out: the water's own colour, at a
		// strength set by how bright the map authored it.
		MID->SetVectorParameterValue(TEXT("Scattering"), FLinearColor(
			Hue.R * Bright * ScatterPerM,
			Hue.G * Bright * ScatterPerM,
			Hue.B * Bright * ScatterPerM));
		// ABSORPTION is the complement of the hue: the channels the water
		// does NOT return are the ones it takes out of the beam, which is
		// why red dies within a metre or two and blue-green carries.
		MID->SetVectorParameterValue(TEXT("Absorption"), FLinearColor(
			(1.f - Hue.R) * AbsorbPerM + 0.02f,
			(1.f - Hue.G) * AbsorbPerM + 0.02f,
			(1.f - Hue.B) * AbsorbPerM + 0.02f));
		return MID;
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

		GRoadRecords = GRoadTris = GRoadElevated = 0;

		// One mesh per record. A record is one road segment or one painted
		// area, each with its own sheets, so merging them would mean one
		// material for surfaces that do not share one.
		for (int32 di = 0; di < D.Num(); di++)
		{
			const BF6HP::FCore::FDecal& d = D[di];
			const int32 nv = d.VertexCount;
			if (nv < 3 || nv % 3 != 0) continue;

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
				// Wound to face up after the axis swap, the same reversal the
				// ground and the props take.
				MD.CreatePolygon(Group, TArray<FVertexInstanceID>{
					Corner(t), Corner(t + 2), Corner(t + 1) });
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
		return GRoadRecords;
	}


	// The map, built. One instanced component per distinct asset, which is both
	// the fast way to draw it and the honest shape of the data: a handful of
	// meshes placed over and over.
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
		TMap<FString, FGroup> ByMesh;
		for (const BF6HP::FPlacement& p : P)
		{
			if (p.Mesh.IsEmpty()) continue;
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
	// SYNCHRONOUS, and it takes about half a minute the first time: mounting a
	// level's archives and indexing every partition's guid is most of it, and
	// both are cached in the core afterwards. Said out loud in the status line
	// rather than hidden behind a spinner that suggests otherwise.
	void ReadLevel()
	{
		const FString Level = BF6Ext::CurrentLevel();
		if (Level.IsEmpty()) { GStatus = TEXT("no map open"); return; }

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
		GStatus = FString::Printf(TEXT("%d of %d placements, %d mesh(es)%s, %.0fs%s"),
			GLastCount, P.Num(), Meshes,
			Failed ? *FString::Printf(TEXT(", %d unreadable"), Failed) : TEXT(""),
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
						SNew(SWrapBox).UseAllottedSize(true)
						+ SWrapBox::Slot()[ LayerPill(ELayer::Terrain) ]
						+ SWrapBox::Slot()[ LayerPill(ELayer::Roads) ]
						+ SWrapBox::Slot()[ LayerPill(ELayer::Objects) ]
						+ SWrapBox::Slot()[ LayerPill(ELayer::Water) ]
					]
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

void FBF6HighPolyModule::StartupModule()
{
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
	BF6Ext::UnregisterPieEntry(FName("HighPoly.Root"));
	// Anything we spawned goes with us: the tool never owned it.
	if (GIsRunning) BF6Ext::ClearAddonActors(kAddonName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBF6HighPolyModule, BF6HighPoly)
