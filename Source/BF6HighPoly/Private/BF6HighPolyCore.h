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
			int32            Ao = -1;
			// THE AUTHORED COLOUR, and it means two different things. With a
			// colour sheet bound it MULTIPLIES that sheet: measured over two
			// levels, 24% and 82% of such records push a channel past 1.0 and
			// the largest is 61.1. With no sheet it IS the colour, absolute:
			// only 3.1% and 5.2% exceed 1.0 there, and the max is about 2.
			// Switch on whether Albedo is bound, never on the value.
			FLinearColor     Tint = FLinearColor::White;
			FLinearColor     Tint2 = FLinearColor::White;
			bool             bHasTint = false, bHasTint2 = false;
			// Which channel of a PACKED mask is this record's coverage, 0..3,
			// or -1. The sheets are atlases holding three or four painted road
			// words and each record picks one, so a reader that always takes
			// red gets about one record in three right.
			int32            MaskChannel = -1;
			int32            AssetSlot = -1;
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
			// THE DECODED OPTICS, or absent.
			//
			// Shallow/Deep above are the AUTHORED colours, and on the ocean
			// family the authored colour is not a colour at all: it is a
			// per-metre TRANSMISSION, what the water reaches after
			// AbsorptionDistanceM metres. Extinction is that converted, so a
			// renderer never has to know the formula.
			//
			// This matters because guessing it gets the CHANNEL ORDER wrong.
			// On MP_Isolated the decode gives R 0.282, G 0.094, B 0.058 per
			// metre - red dies in 2.5 m and blue carries to 12 - while the
			// heuristic it replaces gave a nearly flat 0.172 / 0.204 / 0.216,
			// absorbing blue slightly harder than red, which is backwards and
			// which is why the water read as clear only in the shallows.
			FLinearColor Extinction = FLinearColor(-1.f, 0.f, 0.f);   // per metre
			FLinearColor SurfaceColour = FLinearColor(-1.f, 0.f, 0.f);
			float        AbsorptionDistanceM = -1.f;
		};
		bool ReadWater(const FString& Level, TArray<FWater>& Out);

		// ---- FX ----------------------------------------------------------
		//
		// ONE ROW PER EMITTER LAYER, not per effect and not per particle. An
		// effect is a bundle of layers, each layer is one emitter with its own
		// sheet, and the level places the EFFECT many times. So a renderer
		// wants: the layer's look, and separately the list of world transforms
		// the effect sits at.
		//
		// The sheet is a FLIPBOOK ATLAS. Cols and Frames are AUTHORED and the
		// filename lies about them on three sheets in thirty, so never parse
		// the name. Six sheets in thirty have a fractional pixel cell, so
		// frame rects must be built in UV and never in pixels - which is what
		// FrameUV is for.
		struct FFxLayer
		{
			FString Effect, EffectPath, Graph, Family;
			int32   Placements = 0;
			int32   LayerIndex = 0;
			FVector Right = FVector(1,0,0), Up = FVector(0,1,0),
			        Forward = FVector(0,0,1), Origin = FVector::ZeroVector;
			// The sheet, or empty when this family draws none - 169 of 474
			// layers on one map bind no sheet and are not failures.
			FString Atlas;
			int32   AtlasCols = 0, AtlasFrames = 0, AtlasLeftRight = 0;
			int32   AtlasWidth = 0, AtlasHeight = 0;
			// 0 Emissive, 1 VertexLit, 2 GnomonLit, or -1 unset.
			int32   LightingModel = -1;
			int32   Alignment = -1;
			float   ParticleLife = 0.f, EmitterLife = 0.f;
			float   CullDistance = 0.f;
			int32   ParticleMax = 0;
			// Resolved from the parameter table by name hash, or absent.
			FVector2D Size = FVector2D(-1.f, -1.f);
			FLinearColor Colour = FLinearColor(-1.f, 0.f, 0.f);
			float   Alpha = -1.f, AlphaCull = -1.f;
		};
		bool ReadFx(const FString& Level, TArray<FFxLayer>& Out);
		// The world transforms an effect is placed at, 3x4 row-major each.
		bool FxPlacements(const FString& Level, const FString& Effect,
		                  TArray<FPlacement>& Out);
		// Mip 0 of a layer's sheet, as it ships. Every atlas measured is BC3.
		bool FxAtlas(const FString& Level, int32 LayerIndex, TArray<uint8>& Out);
		// The UV rect of one frame: umin, vmin, umax, vmax. Built by the core
		// so a fractional cell cannot be rounded to pixels on the way out.
		bool FxFrameUV(const FFxLayer& L, int32 Frame, FVector4f& Out);

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

		// ---- the ground as COVERAGE, which is the one a viewport wants ----
		//
		// The bake above flattens the ground into a single raster, and over a
		// whole map that lands at two to four metres a texel while the ground
		// materials themselves repeat every one to seven. Every material is
		// therefore averaged away before the renderer sees it, and the result
		// reads as a low-resolution photograph of ground rather than ground.
		//
		// This keeps the two apart: the MIXING WEIGHTS bake (they vary slowly
		// and rasterise happily at a couple of metres) and the materials get
		// sampled per pixel at their own tiling. All the detail then comes from
		// the sheets at full resolution.
		struct FGroundMaterial
		{
			int32   Layer = -1;
			FString Albedo, Normal;      // resource names, empty when unbound
			float   MetresPerRepeat = 4.f;
			float   RotationDeg = 0.f;
			FLinearColor Tint = FLinearColor::White;
			// How strongly this layer takes the aerial colour map, as a
			// Photoshop Overlay. One when the level does not author it.
			float   Overlay = 1.f;
			// The evaluator constants. The raster's weights are the game's raw
			// MASK; coverage comes from running these per pixel against each
			// layer's height, and normalising the masks instead averages every
			// material together.
			float   BaseHeight = 0.f;
			float   DisplaceRange = 0.f;
			float   MaskRampExp = 1.f;
			float   HeightBlend = 0.f;
			FVector2f CoordScale = FVector2f(1.f, 1.f);
			FVector2f UvOffset = FVector2f(0.f, 0.f);
		};
		struct FGroundCoverage
		{
			int32     Size = 0;
			FVector2D Lo = FVector2D::ZeroVector, Hi = FVector2D::ZeroVector;
			// Size*Size*4 each and owned by the CORE, valid until the next
			// coverage call. idx indexes Materials and 255 means "nothing
			// here"; weight is that slot's share, weight-sorted.
			const uint8* Idx = nullptr;
			const uint8* Weight = nullptr;
			// The aerial colour map over the same rectangle, Size*Size*3 sRGB
			// bytes, or null when the level ships none. This is what carries a
			// map's real palette; the sheets on their own are studio colour.
			const uint8* Colour = nullptr;
			TArray<FGroundMaterial> Materials;
			float EmptyFraction = 0.f;
		};
		bool GroundCoverage(const FString& Level, int32 Size, FGroundCoverage& Out);

		// One material sheet decoded and resampled to Size x Size RGBA8, so
		// every ground layer can go into one texture array. Sheets ship at
		// different sizes and formats; an array needs one of each.
		bool LayerSheet(const FString& ResName, int32 Size, TArray<uint8>& Out);

		// ---- the level's own lighting ------------------------------------
		//
		// Two halves, and both were missing entirely. The ENVIRONMENT is the
		// authored VisualEnvironment: sun angle, colour and real illuminance,
		// the sky, fog and exposure. The LOCAL LIGHTS are every lamp, spot and
		// window panel the level places, of which a map ships thousands.
		//
		// Without either, a map is lit by one hard-coded directional light and
		// nothing else - no sky to reflect, no ambient to scatter, and every
		// interior black. That is why the water read as dark no matter what
		// its own coefficients said.
		struct FVELighting
		{
			FString Preset;
			// SunRotationX is a COMPASS BEARING in degrees (0 along game +Z,
			// turning toward +X) and SunRotationY is elevation above the
			// horizon. Intensity is real illuminance in LUX, which is also
			// what Unreal's directional light wants.
			float SunBearingDeg = 0.f, SunElevationDeg = 45.f;
			FLinearColor SunColor = FLinearColor::White;
			float SunIntensityLux = 100000.f;
			float SunAngularRadiusDeg = 0.29f;
			// The sky is a panorama plus an atmosphere, not three gradient
			// colours: the VE authors no top/horizon/ground fields at all.
			float SkyLuminanceScale = 1.f;
			float SkyPanoramicRotationTurns = 0.f;
			// THE LEVEL'S OWN SKY, which the tool never drew.
			//
			// Every map shipped a painted 8192x2048 HDR panorama and we
			// rendered a generic procedural atmosphere over the top of it, so
			// every level got the same blue and the sky light captured that
			// instead of the authored sky. That is where both "too bright" and
			// "too dark" came from: nothing about the sky was the map's.
			//
			// SkyType 0 is panoramic and 2 is physical. On a physical-sky level
			// the atmosphere IS the sky and should be kept.
			int32 PanoramaTexture = -1;
			int32 SkyTypeValue = 0;
			FLinearColor Rayleigh = FLinearColor(1.f, 1.f, 1.f);
			float MieCoefficient = 0.f, MieG = 0.f;
			bool  bHasSun = false, bHasSky = false, bHasFog = false;
			FLinearColor FogColor = FLinearColor::Black;
			float FogAltitude = 0.f, FogDensity = 0.f, FogVisibilityRange = 0.f;
			// EXPOSURE IS AUTOMATIC on every shipped map, so `ExposureEV` is
			// the STARTING POINT of a runtime metering loop, not a fixed value
			// to apply. The game never ships a fixed exposure; a renderer that
			// wants one has to choose it.
			bool  bAutoExposure = true;
			float ExposureEV = 0.f;
			float ExposureEVMax = 0.f;
			float ExposureCompensation = 0.f;
		};
		bool ReadLighting(const FString& Level, FVELighting& Out);

		// One placed light. The transform is a 3x4 basis in GAME space and
		// carries the holder's SCALE as well as its rotation, so a consumer
		// must normalise before using a row as a direction.
		struct FLight
		{
			int32   Type = 0;               // bf6_light_type
			FVector Right, Up, Forward, Origin;
			FLinearColor Color = FLinearColor::White;
			float   Intensity = 0.f;        // in Unit
			int32   Unit = 0;               // 0 lumens, 1 cd/m2
			float   Dimmer = 1.f;
			float   AttenuationRadiusM = 0.f;
			// FULL cone angles in degrees. Unreal wants HALF angles.
			float   InnerAngleDeg = 0.f, OuterAngleDeg = 0.f;
			float   ShapeRadiusM = 0.f, TubeWidthM = 0.f;
			float   RectHeightM = 0.f, RectAspect = 1.f;
			bool    bCastShadows = false;
		};
		bool ReadLights(const FString& Level, TArray<FLight>& Out, FString& OutSummary);

		static FString MeshResourceFor(const FString& PlacementPath);

		FString Error;

	private:
		void*    Dll = nullptr;
		bf6_ctx* Ctx = nullptr;
	};
}
