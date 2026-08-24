#pragma once

#include "CoreMinimal.h"

// The add-on's own handle on libbf6.
//
// It loads its OWN copy of bf6_core.dll rather than borrowing the tool's
// context. That costs a second mount, but it is what keeps the add-on additive:
// the tool exposes no decode state through the seam, and it should not have to.
// If the two ever need to share, that is a deliberate change to the seam rather
// than a reach-around.
struct bf6_ctx;

namespace BF6HP
{
	// One placement the walk found: an asset name and its transform, still in
	// the GAME's space (Y-up, metres). Converting is the binding's job.
	struct FPlacement
	{
		FString  Mesh;
		FVector  Right, Up, Forward, Origin;
		// The scope a material resolves in. Both belong to the INSTANCE, not to
		// the mesh: two placements of one prop can carry different variations
		// and come from different bundles.
		FString  Bundle;
		FString  Variation;
	};

	class FCore
	{
	public:
		~FCore();

		// Load the dll and open the install. False with Error set.
		bool Open(const FString& GameDir, const FString& DllPath);
		bool IsOpen() const { return Ctx != nullptr; }

		// Mount a level and walk it. Seconds, not milliseconds: the mount and
		// the partition index dominate. Cached in the core per level.
		bool OpenLevel(const FString& Level, const FString& ExePath);

		// Live progress out of the long calls. Set once; the core calls it from
		// whatever thread is doing the work, INCLUDING SEVERAL AT ONCE while it
		// indexes partitions. So this stores numbers and nothing else: touching
		// Slate from here would be a crash waiting for a busy machine.
		struct FProgress
		{
			FCriticalSection Lock;
			FString          Stage;
			int32            Done = 0, Total = 0;
			bool             bCancel = false;

			void Set(const FString& S, int32 D, int32 T)
			{
				FScopeLock G(&Lock);
				Stage = S; Done = D; Total = T;
			}
			void Read(FString& S, int32& D, int32& T)
			{
				FScopeLock G(&Lock);
				S = Stage; D = Done; T = Total;
			}
			bool Cancelled() { FScopeLock G(&Lock); return bCancel; }
			void Cancel()    { FScopeLock G(&Lock); bCancel = true; }
		};
		FProgress Progress;

		// The placements from the last OpenLevel.
		bool Placements(const FString& Level, TArray<FPlacement>& Out);

		// One asset's geometry, decoded from the install. lod 0 is full detail.
		//
		// The resource name is the placement's asset path with ".ebx" swapped
		// for "_mesh": checked against the catalogue, that is the convention and
		// not a guess. Constructing it rather than searching also avoids the
		// "dc_" twin, which is the DESTROYED copy of the same prop and sits
		// right beside it under a name a fuzzy search would happily return.
		// What one section binds. Slots follow the core's enum: 0 albedo,
		// 1 normal, 2 metallic/roughness/occlusion, 3 emissive, 4 mask.
		struct FBinding { int32 Slot = 0; int32 Texture = -1; };

		struct FSection
		{
			TArray<FVector3f> Pos, Nrm;
			TArray<FVector2f> UV;
			TArray<uint32>    Idx;
			TArray<FBinding>  Textures;
			// From the shader's own record, not guessed from the textures.
			bool bAlphaTest = false;
			bool bTranslucent = false;
			// Vegetation cuts out from its own base colour's alpha; everything
			// else uses a separate single-channel sheet.
			bool bAlphaFromAlbedo = false;
			// THE COLOUR THE RECORD ITSELF CARRIES, as a multiplier over the
			// albedo sheet. Whole families have no sheet at all - car paint is the
			// obvious one, where the body colour is a depot constant and nothing
			// else - so a reader that only looks for textures draws them white.
			// One field covers both cases: with no sheet the consumer multiplies
			// white and gets the colour, with one it gets the tint.
			FLinearColor     BaseColor = FLinearColor::White;
			float            Roughness = 0.5f;
			// The far-LOD vista "_nsm": RG normal / B wetness / A smoothness,
			// straight from the recovered M_Vista pixel shader. The material
			// has to unpack it, not sample it as a normal map.
			bool bNsm = false;
		};

		// PlacingBundle is the bundle whose placement pulled this mesh in, from
		// the walk. It is the exact scope for a material: a shader state key is
		// unique only within a bundle, so resolving anywhere else can bind a
		// material that merely collides, which looks right and is not. Empty is
		// allowed and falls back to the bundle the resource lives in, which is
		// the only option for a mesh nothing has placed.
		// Does this variation derive a live record for this mesh? The gate
		// that makes splitting instance groups by variation EARNED rather
		// than automatic - the reference plugin measured the difference at
		// four milliseconds of frame on one map.
		bool VariationLive(const FString& ResName, const FString& Bundle,
		                   const FString& Variation);

		bool ReadMesh(const FString& ResName, TArray<FSection>& Out,
		              const FString& PlacingBundle = FString(),
		              const FString& Variation = FString());

		// One decoded texture, still block-compressed.
		struct FTexture
		{
			int32 Width = 0, Height = 0;
			int32 Format = 0;        // the core's bf6_fmt
			int32 MipCount = 1;      // the chain, largest first, tightly packed
			bool  bSrgb = false;
			const uint8* Data = nullptr;
			int32 DataLen = 0;
		};
		bool TextureAt(int32 Id, FTexture& Out);

		// The level's ground: a square grid of u16 samples plus the world box
		// it spans, both in the GAME's space. The grid is whatever resolution
		// the streaming tree actually holds, which on a Portal map is 16385 a
		// side, so a caller almost always wants to step over it rather than
		// take every sample.
		struct FTerrain
		{
			int32            Size = 0;
			TArray<uint16>   Heights;
			// THE HEIGHT SCALE THE TREE ITSELF CARRIES. A sample decodes as
			// y = u16 / 65536 * HeightScale, a pure scale through zero. Fitting
			// a line from the AABB onto the raw range instead happens to give
			// the same answer while the AABB is tight to the data, which it is
			// on every map measured - but that is a coincidence of the data, not
			// a rule, and it is one nobody would notice breaking.
			float            HeightScale = 0.f;
			FVector          WorldMin = FVector::ZeroVector, WorldMax = FVector::ZeroVector;
		};
		bool ReadTerrain(const FString& Level, FTerrain& Out);

		// ---- roads and street markings ----------------------------------
		//
		// The street SURFACE is the terrain: the heightfield IS the asphalt, and
		// a reader without this still draws ground where a road is. What it does
		// not draw is anything that makes a road read as one - lane markings,
		// crossings, mud, wear, tyre tracks, kerb blending.
		//
		// A decal vertex carries world X and Z and NO Y. The consumer drapes it
		// on the ground it built.
		struct FDecal
		{
			// x, z, u, v, r, g, b, a per vertex, stride 8. NON-INDEXED: the count
			// is exactly three per triangle, in triangle order.
			const float*     Verts = nullptr;
			int32            VertexCount = 0;
			FVector          AabbMin = FVector::ZeroVector;
			FVector          AabbMax = FVector::ZeroVector;
			float            Tiling0 = 0.f, Tiling1 = 0.f;
			bool             bPlanar = false;
			// THE MARKINGS LIVE IN Opacity. A lane stripe is coverage, not
			// colour: bind only the base colour and the asphalt draws with none
			// of the paint on it, which still reads as an empty road.
			int32            Albedo = -1, Opacity = -1, Normal = -1;
		};
		bool ReadDecals(const FString& Level, TArray<FDecal>& Out);

		// One water surface: a flat plane at an absolute height, with the
		// colours the level's own record authors. Shallow.R < 0 = no mined
		// colour, use a preset. Ocean authors ONE colour; Deep stays absent.
		struct FWater
		{
			FVector2D    Center = FVector2D::ZeroVector;   // game X, Z
			FVector2D    Size   = FVector2D::ZeroVector;   // metres
			float        Height = 0.f;                     // game Y
			FLinearColor Shallow = FLinearColor(-1.f, 0.f, 0.f);
			FLinearColor Deep    = FLinearColor(-1.f, 0.f, 0.f);
			bool         bOcean = false;
		};
		bool ReadWater(const FString& Level, TArray<FWater>& Out);

		// The authored sea state: the ocean simulation's inputs. The wave
		// FIELD is a runtime GPU sim and is not on disk; these numbers drive
		// it. WindAngle is radians (axis convention unresolved), WindSpeed a
		// normalised scalar, the distribution is wave energy by direction.
		struct FWaterSim
		{
			float WindAngle = 0.f, WindSpeed = 0.f, Choppiness = 0.f;
			float TileDimension = 0.f, MinWavelength = 0.f;
			float LargeWaveReduction = 0.f, FoamThreshold = 0.f, FoamMax = 0.f;
			bool  bFlagged = false;
			TArray<FVector2D> Dist;   // x 0..1 = a turn around WindAngle, y = energy
		};
		bool ReadWaterSim(const FString& Level, FWaterSim& Out);

		// The ground, composited from the game's own layer materials. A bake
		// rather than a live shader: the renderer drapes it over the
		// heightfield. Pixels are RGBA8 and sRGB-encoded; the buffers belong
		// to the core and stay valid until the next bake.
		struct FGroundBake
		{
			int32   Size = 0;
			FVector2D Lo = FVector2D::ZeroVector;   // world XZ, metres
			FVector2D Hi = FVector2D::ZeroVector;
			float   MetresPerTexel = 0.f;
			const uint8* Albedo = nullptr;
			const uint8* Normal = nullptr;
			int32   LayersUsed = 0, LayersTextured = 0;
			float   FallbackFraction = 0.f;
		};
		// RectSize <= 0 bakes the whole map footprint.
		bool BakeGround(const FString& Level, const FVector2D& RectMin, float RectSize,
		                int32 Size, FGroundBake& Out);

		static FString MeshResourceFor(const FString& PlacementPath);

		FString Error;

	private:
		void*    Dll = nullptr;
		bf6_ctx* Ctx = nullptr;
	};
}
