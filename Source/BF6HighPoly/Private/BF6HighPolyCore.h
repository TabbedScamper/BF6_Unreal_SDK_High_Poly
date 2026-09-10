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
	// WHERE bf6_core.dll IS, PACKAGED OR IN DEVELOPMENT.
	//
	// One resolver for every consumer. Three call sites each built the same
	// path into the SOURCE tree, which exists on a developer machine and in no
	// installed copy of the plugin, so an installed add-on failed with an error
	// naming a path that was never going to be there. Staged first, development
	// second.
	FString CoreDllPath();

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
		// Close while the owning Unreal modules are still loaded. Relying on the
		// file-scope destructor is too late during editor shutdown: the SDK module
		// can release bf6_core.dll first, leaving GClose pointing into an unloaded
		// image.
		void Close();

		// Load the dll and open the install. False with Error set.
		bool Open(const FString& GameDir, const FString& DllPath);
		bool IsOpen() const { return Ctx != nullptr; }
		const FGuid& TextureNamespace() const { return TextureSession; }

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

		// ---- the placeable catalogue (object library previews) ----------
		//
		// Mount every level's archives, so any pf_portal_* prefab and every
		// member mesh it names resolves, not just the open level's. Slow cold
		// (the Godot plugin measured 85 s); run it on a worker. Idempotent in
		// the core: the first mount wins on a name collision.
		bool MountAll(bool bIncludeLevels);

		// ONE LEVEL'S ARCHIVES, WITHOUT WALKING THE LEVEL.
		//
		// OpenLevel does a full traversal, and that is what the placed-object
		// module was paying for: 26.0 seconds on MP_Aftermath to be able to read
		// prefabs by name, when it never looks at the level's own placements at
		// all - the SDK scene already says where things are. This is the core's
		// own narrow door for exactly that case, measured at 5.4 s standalone.
		// Existing names still win, so it cannot disturb an open level.
		bool MountLevelArchives(const FString& Level);
		// Every EBX partition whose name CONTAINS Search, as the mount spells it.
		bool ListEbx(const FString& Search, TArray<FString>& Out);
		// Walk one prefab/asset from an identity root: its members in the
		// ASSET's own space, same rows as Placements. THE CORE KEEPS ONE
		// WALKER, so this replaces the level walk; the next Placements() for a
		// level re-walks after OpenLevel is asked again. Copy the level's rows
		// out before asking for assets, which the build already does.
		bool AssetInstances(const FString& Asset, TArray<FPlacement>& Out);

		// One asset's geometry, decoded from the install. lod 0 is full detail.
		//
		// The resource name is the placement's asset path with ".ebx" swapped
		// for "_mesh": checked against the catalogue, that is the convention and
		// not a guess. Constructing it rather than searching also avoids the
		// "dc_" twin, which is the DESTROYED copy of the same prop and sits
		// right beside it under a name a fuzzy search would happily return.
		// What one section binds. Slots follow the core's enum: 0 albedo,
		// 1 normal, 2 metallic/roughness/occlusion, 3 emissive, 4 mask.
		// THE NAME, WHERE WE ALREADY HAD IT.
		//
		// Texture is this mount's id, and turning one back into a name costs
		// bf6_texture_name_at - measured on 9 September 2026 at about 0.8
		// SECONDS per call, so sixteen of them took 12.8 s of a 26 s level open.
		// But the cached mesh blob stores bindings BY NAME and resolves them to
		// ids on load, and the live decode names them to write that blob, so on
		// both paths the string was in hand a moment earlier. Keeping it costs
		// nothing and spares the reverse lookup entirely. Empty means unknown,
		// and the caller may fall back to asking.
		struct FBinding { int32 Slot = 0; int32 Texture = -1; FString Name; };

		struct FSection
		{
			TArray<FVector3f> Pos, Nrm;
			TArray<FVector2f> UV;
			// Linear surface palette multipliers. Empty means neutral white.
			TArray<FVector4f> Colors;
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
			bool bDecal = false;
			bool bTerrainDecalReceiver = false;
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
		              const FString& Variation = FString(),
		              const TArray<FMatrix44f>* Skin = nullptr, int32 RigBoneCount = 0, int32 Lod = 0);

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
		// MaxDim selects the core's capped decode path: it starts at the first
		// authored mip that fits instead of decoding a full sheet for the caller
		// to throw away. Zero keeps the original full-resolution path.
		bool TextureAt(int32 Id, FTexture& Out, int32 MaxDim = 0);
		// Call only after copying the borrowed capped TextureAt view.
		void ReleaseTexturePayload(int32 Id, int32 MaxDim);
		// Register a standalone texture RESOURCE by its live install name. This is
		// how VisualEnvironment textures that are not material bindings (flow
		// masks, cloud shadows and grading LUTs) enter the same direct decode path.
		int32 TextureIdByName(const FString& ResourceName) const;
		// Live resource name from the current install. Empty when the optional
		// export is unavailable, preserving compatibility with an older core.
		FString TextureNameAt(int32 Id) const;

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
		// Streaming-tree block 2: the absolute-Y water surface sampled by the
		// shipped water vertex shader, separate from the small ocean FFT.
		bool ReadWaterHeightfield(const FString& Level, FTerrain& Out);

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
			// Authored WaterSurfaceEntityData flag 0x8325B2E3. This is the
			// only verified readable water type: it agrees 14/14 with the
			// river-flow shader permutation. Do not infer it from dimensions.
			bool         bRiver = false;
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
			// Vertical displacement multiplier consumed by the shipped draw
			// shader. Horizontal displacement is scaled independently by each
			// cascade's Choppiness.
			float        WaveAmplitudeScale = 1.f;
			// Draw/composite inputs from the runtime material description. These
			// remain optional (negative sentinel) for an older core ABI.
			int32        AttenuationType = -1;
			bool         bShoreFadeValid = false;
			float        ShoreDepthM = -1.f;
			FVector4f    ShoreBlend = FVector4f(0.f, 0.f, 1.f, 0.f);
			float        AdditionalWaterDepthM = 0.f;
			float        DetailFadeStartM = -1.f, DetailFadeEndM = -1.f;
			float        DrawFoamThreshold = -1.f;
			float        ShoreFoamSuppression = -1.f;
			float        FoamContrast = -1.f;
			// CB1[1].wzyx in the draw pass: how much each cascade's fold channel
			// contributes to foam coverage. -1 = the level authors none.
			float        CascadeFoamWeight[4] = { -1.f, -1.f, -1.f, -1.f };
			float        SmoothnessZeroFoam = -1.f, SmoothnessFullFoam = -1.f;
			float        SmoothnessBias = -1.f, SmoothnessNearMultiplier = -1.f;
			float        FoamNormalStrength = -1.f, MicroNormalStrength = -1.f;
			float        NoiseUvScale = -1.f;
			// Extended/Isolated graph inputs, all runtime depot reads. Separate
			// sheet scales are essential: the shipping graph does not retile the
			// foam and micro normals at the same frequency.
			float        MicroSheetUvScale = -1.f, FoamSheetUvScale = -1.f;
			float        MicroSheetFlowSpeed = -1.f;
			float        FoamCompositeLow = -1.f, FoamCompositeHigh = -1.f;
			float        ContactWorldDivisorM = -1.f, ContactRemapLow = -1.f;
			float        ContactGain = -1.f;
			float        BroadPatternWorldMul = -1.f, BroadPatternWorldScale = -1.f;
			float        BroadPatternFloor = -1.f;
			uint32       ExtendedGraphVersion = 0;
			FVector4f    ExtendedCb1[22];
			// Exact selected pass-0 cascade overlap transform. Version zero means
			// the live vertex permutation no longer matches the decoded arithmetic.
			uint32       CascadeOverlapVersion = 0;
			bool         bCascadeOverlapEnabled = false;
			FVector4f    CascadeOverlapParams = FVector4f(0.f, 0.f, 0.f, 0.f);
			FVector4f    CascadeOverlapParams2 = FVector4f(0.f, 0.f, 0.f, 0.f);
			float        CascadeOverlapHeightScale = 0.f;
			// Live texture ids from bf6_level_water_render. These are resolved
			// from the mounted level's material depot on every read; they are not
			// exported/staged assets and therefore follow game updates.
			int32        DetailNormal = -1, FoamNormal = -1, FoamRgb = -1;
			int32        Noise = -1, Perlin = -1, ContactFoam = -1, FoamRgb2 = -1;
			// This byte was initially called reflectance. The recovered deferred
			// composite proves it is an opacity/turbidity input; water F0 is a
			// hard-coded 0.0256 and consumers must not substitute this value.
			float        ReflectanceLow = -1.f;
			// Active VisualEnvironment OceanComponentData. This is the other
			// half of Frostbite water's deferred composite and is joined live
			// through the mounted level root, never from an export.
			uint32       OceanComponentVersion = 0;
			FString      OceanPreset;
			int32        OceanPresetCandidates = 0;
			bool         bOceanEnabled = false;
			bool         bSimplifiedDistortion = false;
			bool         bFoamEnabled = false;
			float        CompositeIor = -1.f;
			float        OpacityRampM = -1.f;
			float        FoamDepthRampM = -1.f;
			float        ScatterPhaseG = -1.f;
			FLinearColor TransmissionColour = FLinearColor(-1.f, 0.f, 0.f);
			float        ScatterShadowInfluence = -1.f;
			FLinearColor CompositeFoamTint = FLinearColor(-1.f, 0.f, 0.f);
			float        CompositeFoamSmoothness = -1.f;
			float        CompositeFoamRoughness = -1.f;
			FLinearColor AuthoredOceanAlbedo = FLinearColor(-1.f, 0.f, 0.f);
			float        AuthoredAlbedoDistanceM = -1.f;
		};
		bool ReadWater(const FString& Level, TArray<FWater>& Out);

		// Exact terrain utility raster consumed by CoarseMask water attenuation.
		// Atlas is the installed game's R8 pages byte-for-byte; Indirection is the
		// packed table built by the same algorithm as the current game runtime.
		struct FWaterMask
		{
			uint32 Version = 0;
			int32 TileSide = 0, InteriorSide = 0, Border = 0;
			int32 PageCount = 0, IndirectionSide = 0;
			FVector2D BoundsMin = FVector2D::ZeroVector;
			FVector2D BoundsMax = FVector2D::ZeroVector;
			float CoverageSideRcp = 0.f, BorderFraction = 0.f;
			TArray<uint8> AtlasR8;
			TArray<uint32> Indirection;
		};
		bool ReadWaterMask(const FString& Level, FWaterMask& Out);

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

		// Legacy first-cascade view. New water code must use ReadWaterCascades:
		// Tsuru authors four enabled simulations and dropping three is not a
		// valid approximation.
		struct FWaterSim
		{
			float WindAngle = 0.f, WindSpeed = 0.f, Choppiness = 0.f;
			float TileDimension = 0.f, MinWavelength = 0.f;
			float LargeWaveReduction = 0.f, FoamThreshold = 0.f, FoamMax = 0.f;
			bool  bFlagged = false;
			TArray<FVector2D> Dist;   // x 0..1 = a turn around WindAngle, y = energy
		};
		bool ReadWaterSim(const FString& Level, FWaterSim& Out);

		// Complete live inputs for one shipped FFT cascade, followed by the
		// deterministic H0 complex spectrum produced by the game's CPU route.
		// Angles are authored degrees. H0 contains Resolution^2 FVector2f
		// entries in row-major order and is never an exported intermediate.
		struct FWaterCascade
		{
			int32 SourceIndex = -1;
			int32 Resolution = 0;
			float WindAngleDegrees = 0.f, WindSpeed = 0.f, Choppiness = 0.f;
			float TileDimension = 0.f, MinWavelength = 0.f;
			float LargeWaveReduction = 0.f, WaveAmplitude = 0.f;
			float WaveThickness = 0.f;
			bool bFoamEnabled = false;
			float FoamThreshold = 0.f, FoamMax = 0.f, FoamHalfLife = 0.f;
			bool bPhysicsSimulation = false, bForceSimplePlaneCollision = false;
			bool bVisualCpuSimulation = false;
			TArray<FVector2f> H0;
		};
		// SEA STATE. The authored wind is what the level ships, and on
		// mp_isolated that is a near-calm 0.914 producing a millimetre sea. The
		// same shipped H0 builder reaches a metre-scale sea at wind 9 to 12,
		// which is a wind other shipped levels already author, so the sea state
		// is a real dial rather than an invented multiplier (finding
		// ocean-wind-law-metre-waves-at-plausible-wind).
		//
		// This changes the INPUT and lets the game's own builder run. Nothing
		// downstream is scaled: displacement, normals, foam and the merge all
		// follow because they are computed from the rebuilt spectrum.
		//
		// Absolute sets every cascade to one wind. Scale multiplies each
		// cascade's authored wind and so preserves the relationship the level
		// authored between them, including cascades deliberately left near zero.
		// Absolute wins when both are set. Absolute < 0 means "not set".
		static void  SetWindOverride(float AbsoluteMps, float Scale);
		// Read this level's Beaufort force and the game's own wind curve, and
		// adopt the result as the wind. False when the level authors no
		// mapping, which leaves the authored per-cascade wind untouched.
		bool ApplyLevelSeaState(const FString& Level);
		static float WindOverrideAbsolute();
		static float WindOverrideScale();
		// The wind actually used for a cascade, given its authored value.
		static float EffectiveWind(float AuthoredWind);

		// SHORTEST WAVE. Each cascade authors a minimum wavelength that feeds an
		// extra exp(-l2k2 * minWavelength) attenuation, so it decides how much
		// SHORT-wave energy the cascade keeps. On mp_isolated the two middle
		// cascades author 6.0 against the finest cascade's 1.0, which flattens
		// exactly the 40 to 70 m band that reads as rollers: measured through the
		// shipped builder at wind 12, dropping it to 0.25 raises the 67.6 m
		// cascade 22x and the 43 m cascade 50x. Wind alone cannot fix that,
		// because wind puts its energy into the 300 m cascade where a third of a
		// metre spread over three hundred is invisible.
		//
		// Negative means "not set", and the authored value is used.
		// PER CASCADE, for the same reason amplitude is: a cascade spans every
		// wavelength from its tile size down to twice its texel, so the 300 m
		// cascade carries chop as well as swell. min_wavelength is what strips
		// the short end out, which is why the level authors 6.0 on the middle
		// cascades and keeps them long-only. Forcing 0 everywhere removes that
		// separation and makes raising a swell raise its chop with it.
		//
		// Cascade index is renderer order. -1 sets all. Negative metres restores
		// the authored value.
		static void  SetMinWavelengthOverride(int32 Cascade, float Metres);
		static float MinWavelengthOverride(int32 Cascade);
		static float EffectiveMinWavelength(int32 Cascade, float AuthoredMinWavelength);

		// WAVE HEIGHT. Every mp_isolated cascade authors wave_amplitude 0.010,
		// and the spectrum scales as its SQUARE ROOT: 0.010 -> 0.25 is five
		// times the wave height. Measured through the shipped builder, not
		// assumed. This is the dial for swell SIZE, where wind decides the
		// wavelength the energy sits at and min wavelength decides how much
		// short detail rides on top.
		//
		// Negative means "not set", and the authored value is used.
		// PER CASCADE, because one amplitude across all four does not work: the
		// cascades carry different wavelengths, so the same height is a gentle
		// swell on the 300 m tile and a broken, self-intersecting surface on the
		// 12 m one. Swell belongs in the long cascades; the short ones are
		// detail and should keep their authored height.
		//
		// Cascade index is renderer order (largest tile first). -1 sets all.
		// A negative amplitude restores the authored value.
		static void  SetAmplitudeOverride(int32 Cascade, float Amplitude);
		static float AmplitudeOverride(int32 Cascade);
		static float EffectiveAmplitude(int32 Cascade, float AuthoredAmplitude);

		bool ReadWaterCascades(const FString& Level, TArray<FWaterCascade>& Out);
		// Water-lab path: mounts the named level and reads only its water
		// schematics. It never runs the full placement/object-graph walk.
		bool ReadWaterCascadesIsolated(const FString& Level, TArray<FWaterCascade>& Out);

		// Copy one decompressed resource directly from the current mounted game.
		// The core owns a one-slot buffer, so this wrapper always copies before
		// returning. It never stages the bytes on disk.
		bool ReadRawResource(const FString& ResourceName, TArray<uint8>& Out);

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
			FString Albedo, Normal, Coverage; // resource names; Coverage is optional `_op`
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
			int32     SlotCount = 4;
			FVector2D Lo = FVector2D::ZeroVector, Hi = FVector2D::ZeroVector;
			// Size*Size*SlotCount each and owned by the CORE, valid until the next
			// coverage call. idx indexes Materials and 255 means "nothing
			// here"; weight is that slot's share. Slots are in Frostbite's
			// evaluator order: the block-7 base first, then block-1 detail.
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

		// Exact shipped clutter catalogue. The database does not contain world
		// positions, so placement built from these rows is necessarily a
		// deterministic reconstruction over the live terrain masks.
		struct FScatter
		{
			FString Name, MeshRes;
			float ViewDistanceM = 0.f, DissolveRatio = 0.f;
			int32 OpaquePointCount = 0;
		};
		bool ReadScatter(const FString& Level, TArray<FScatter>& Out);

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
			FString PresetPath;
			int32 PresetCandidates = 0;
			int32 FieldsFound = 0, FieldsExpected = 0;
			// SunRotationX is a COMPASS BEARING in degrees (0 along game +Z,
			// turning toward +X) and SunRotationY is elevation above the
			// horizon. Intensity is real illuminance in LUX, which is also
			// what Unreal's directional light wants.
			float SunBearingDeg = 0.f, SunElevationDeg = 45.f;
			FLinearColor SunColor = FLinearColor::White;
			float SunIntensityLux = 100000.f;
			float SunAngularRadiusDeg = 0.29f;
			float SunShadowViewDistanceM = 0.f;
			int32 CloudShadowTexture = -1;
			float CloudShadowSizeM = 0.f, CloudShadowCoverage = 0.f;
			float CloudShadowExponent = 0.f;
			FVector2f CloudShadowSpeed = FVector2f::ZeroVector;
			FVector2f CloudShadowTranslation = FVector2f::ZeroVector;
			float SecondaryCloudShadowSizeM = 0.f;
			float SecondaryCloudShadowCoverage = 0.f;
			float SecondaryCloudShadowExponent = 0.f;
			FVector2f SecondaryCloudShadowSpeed = FVector2f::ZeroVector;
			FVector2f SecondaryCloudShadowTranslation = FVector2f::ZeroVector;
			int32 CloudShadowAddressingMode = 0;
			int32 SecondaryCloudShadowAddressingMode = 0;
			bool bCloudShadowTopDown = false;
			bool bSecondaryCloudShadowTopDown = false;
			float CloudShadowStartFadeM = 0.f;
			float CloudShadowFadeDistanceM = 0.f;
			bool bCloudShadowHeightFade = false;
			float CloudShadowStartHeightFadeM = 0.f;
			float CloudShadowHeightFadeDistanceM = 0.f;
			// The sky is a panorama plus an atmosphere, not three gradient
			// colours: the VE authors no top/horizon/ground fields at all.
			float SkyLuminanceScale = 1.f;
			float SkyPanoramicRotationTurns = 0.f;
			float SkyPanoramicTileFactor = 1.f;
			FVector2f SkyPanoramicUVMin = FVector2f(0.f, 0.f);
			FVector2f SkyPanoramicUVMax = FVector2f(1.f, 1.f);
			float SkyFlowDistance = 0.f, SkyFlowDirectionDeg = 0.f;
			float SkyFlowPeriodSeconds = 0.f;
			float SkyFlowHeightMaskScale = 0.f;
			float SkyFlowHeightMaskBias = 0.f;
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
			// These IDs are already resolved by bf6_core from the active preset's
			// exact imports. Keep them in the Unreal bridge even where the renderer
			// deliberately waits for a proven consumer law; dropping them here made
			// a present raw binding indistinguishable from an absent one.
			int32 PanoramaAlphaTexture = -1;
			int32 FlowMaskTexture = -1;
			int32 CloudLayer1Texture = -1;
			int32 SecondaryCloudShadowTexture = -1;
			int32 GradingLutTexture = -1;
			int32 SkyTypeValue = 0;
			FLinearColor Rayleigh = FLinearColor(1.f, 1.f, 1.f);
			float MieCoefficient = 0.f, MieG = 0.f;
			bool bUseAerialPerspective = false;
			float AerialPerspectiveScale = 0.f, AerialPerspectiveIntensity = 0.f;
			float CloudLayerAltitudeM = 0.f, CloudLayerTileFactor = 0.f;
			float CloudLayerRotationDeg = 0.f, CloudLayerSpeed = 0.f;
			float CloudLayerAlpha = 0.f;
			bool  bHasSun = false, bHasSky = false, bHasFog = false;
			bool bHeightFogEnable = false, bFogColorEnable = false;
			bool bFogGradientEnable = false, bVolumetricsEnable = false;
			FLinearColor FogColor = FLinearColor::Black;
			float FogDistanceStartM = 0.f, FogDistanceEndM = 0.f;
			float FogHeightStartM = 0.f, FogHeightEndM = 0.f;
			float FogAltitude = 0.f, FogDepth = 0.f, FogVisibilityRange = 0.f;
			float SunScatterIntensity = 0.f, LocalLightScatterIntensity = 0.f;
			// EXPOSURE IS AUTOMATIC on every shipped map, so `ExposureEV` is
			// the STARTING POINT of a runtime metering loop, not a fixed value
			// to apply. The game never ships a fixed exposure; a renderer that
			// wants one has to choose it.
			bool  bAutoExposure = true;
			float ExposureEV = 0.f;
			float ExposureEVMax = 0.f;
			float ExposureCompensation = 0.f;
			bool bGradingEnable = false;
			FLinearColor GradeBrightness = FLinearColor::White;
			FLinearColor GradeContrast = FLinearColor::White;
			FLinearColor GradeSaturation = FLinearColor::White;
			float GradeHueDeg = 0.f;
			float WhiteTemperatureK = 6500.f, WhiteTint = 0.f;
			bool bAoAffectsOutdoorLight = false, bAoAffectsLocalLight = false;
			float HbaoRadius = 0.f, HbaoContrast = 0.f;
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
			FString Source;                  // partition that authored this light
			uint32  Flags = 0;               // retained for diagnostics; meaning open
		};
		bool ReadLights(const FString& Level, TArray<FLight>& Out, FString& OutSummary);

		static FString MeshResourceFor(const FString& PlacementPath);

		FString Error;

		// How many meshes came back from their placing bundle with no material
		// bound at all and were recovered through the unscoped read. Nonzero
		// means props were being drawn untextured, which from the viewport is
		// indistinguishable from not being upgraded at all. See ReadMesh.
		int32 MaterialsRecovered = 0;

		// The raw handles, for the few exports this class does not wrap.
		// Read-only: whoever calls them shares this context and must not be
		// inside it at the same time as a build (see BF6HighPolyShared.h).
		bf6_ctx* Handle() const { return Ctx; }
		void*    DllHandle() const { return Dll; }

	private:
		void*    Dll = nullptr;
		bf6_ctx* Ctx = nullptr;
		FGuid TextureSession = FGuid::NewGuid();
		// WHICH INSTALL THIS CONTEXT IS READING.
		//
		// Open returned early whenever a context existed, without comparing the
		// folder, so changing the game folder left the reader on the old install
		// while the disk cache was reconfigured for the new one. The two then
		// disagreed silently and produced meshes from one install keyed against
		// the other.
		FString OpenedFor;
	};
}
