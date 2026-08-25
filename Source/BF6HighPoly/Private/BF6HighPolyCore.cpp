#include "BF6HighPolyCore.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include "bf6_core.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogBF6HPCore, Log, All);

namespace BF6HP
{
namespace
{
	typedef bf6_ctx* (*FnOpen)(const char*, char*, int);
	typedef void     (*FnClose)(bf6_ctx*);
	typedef int      (*FnOpenLevel)(bf6_ctx*, const char*, const char*, int, char*, int);
	typedef int      (*FnInstances)(bf6_ctx*, const char*, bf6_instance*, int);
	typedef bf6_mesh* (*FnReadMesh)(bf6_ctx*, const char*, int);
	typedef void      (*FnFree)(bf6_ctx*, void*);
	typedef bf6_terrain* (*FnTerrain)(bf6_ctx*, const char*);
	typedef void (*FnSetProgress)(bf6_ctx*, bf6_progress_fn, void*);
	typedef bf6_mesh* (*FnReadMeshScoped)(bf6_ctx*, const char*, int, const char*, const char*);
	typedef const bf6_texture* (*FnTextureAt)(bf6_ctx*, int);
	typedef int (*FnWater)(bf6_ctx*, const char*, bf6_water*, int);
	typedef int (*FnVarLive)(bf6_ctx*, const char*, const char*, const char*);
	typedef int (*FnWaterSim)(bf6_ctx*, const char*, bf6_water_sim*);
	typedef int (*FnWaterRender)(bf6_ctx*, const char*, bf6_water_render*, int);
	typedef int (*FnFx)(bf6_ctx*, const char*, bf6_fx_layer*, int, bf6_fx_stats*, char*, int);
	typedef int (*FnFxPlacements)(bf6_ctx*, const char*, const char*, float*, int);
	typedef int (*FnFxAtlas)(bf6_ctx*, const char*, int, const uint8_t**, int32_t*, char*, int);
	typedef int (*FnFxFrameUv)(const bf6_fx_layer*, int, float*);
	typedef int (*FnBakeGround)(bf6_ctx*, const char*, const bf6_terrain_bake_opts*,
	                            bf6_terrain_bake*, char*, int);
	typedef int (*FnDecals)(bf6_ctx*, const char*, bf6_decal*, int);
	typedef int (*FnGroundCov)(bf6_ctx*, const char*, int, bf6_ground_coverage*, char*, int);
	typedef int (*FnLayerSheet)(bf6_ctx*, const char*, int, uint8_t*, char*, int);
	typedef int (*FnVeLighting)(bf6_ctx*, const char*, bf6_ve_lighting*, char*, int);
	typedef int (*FnLevelLights)(bf6_ctx*, const char*, bf6_light*, int,
	                             bf6_light_stats*, char*, int);

	FnOpen      GOpen      = nullptr;
	FnClose     GClose     = nullptr;
	FnOpenLevel GOpenLevel = nullptr;
	FnInstances GInstances = nullptr;
	FnReadMesh  GReadMesh  = nullptr;
	FnFree      GFree      = nullptr;
	FnTerrain   GTerrain   = nullptr;
	FnDecals    GDecals    = nullptr;
	FnSetProgress GSetProgress = nullptr;
	FnReadMeshScoped GReadScoped = nullptr;
	FnTextureAt GTextureAt = nullptr;
	FnWater     GWater     = nullptr;
	FnVarLive   GVarLive   = nullptr;
	FnWaterSim  GWaterSim  = nullptr;
	FnWaterRender GWaterRender = nullptr;
	FnFx           GFx = nullptr;
	FnFxPlacements GFxPlacements = nullptr;
	FnFxAtlas      GFxAtlas = nullptr;
	FnFxFrameUv    GFxFrameUv = nullptr;
	FnBakeGround GBakeGround = nullptr;
	FnGroundCov  GGroundCov  = nullptr;
	FnLayerSheet GLayerSheet = nullptr;
	FnVeLighting GVeLighting = nullptr;
	FnLevelLights GLevelLights = nullptr;

	// The C callback the core drives. Stores and returns; no UI, no allocation
	// beyond the string, because this runs on the core's worker threads.
	int ProgressThunk(void* user, const char* stage, int done, int total)
	{
		FCore::FProgress* P = (FCore::FProgress*)user;
		if (!P) return 1;
		P->Set(UTF8_TO_TCHAR(stage), done, total);
		return P->Cancelled() ? 0 : 1;
	}
}

FCore::~FCore()
{
	if (Ctx && GClose) GClose(Ctx);
	Ctx = nullptr;
	// The handle is deliberately NOT freed: the tool's own module may have the
	// same dll loaded, and unloading it out from under that would take the
	// editor with it.
	Dll = nullptr;
}

bool FCore::Open(const FString& GameDir, const FString& DllPath)
{
	Error.Reset();
	if (Ctx) return true;

	Dll = FPlatformProcess::GetDllHandle(*DllPath);
	if (!Dll) { Error = FString::Printf(TEXT("could not load %s"), *DllPath); return false; }

	GOpen      = (FnOpen)      FPlatformProcess::GetDllExport(Dll, TEXT("bf6_open"));
	GClose     = (FnClose)     FPlatformProcess::GetDllExport(Dll, TEXT("bf6_close"));
	GOpenLevel = (FnOpenLevel) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_open_level"));
	GInstances = (FnInstances) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_instances"));
	GReadMesh  = (FnReadMesh)  FPlatformProcess::GetDllExport(Dll, TEXT("bf6_read_mesh"));
	GFree      = (FnFree)      FPlatformProcess::GetDllExport(Dll, TEXT("bf6_free"));
	GTerrain   = (FnTerrain)   FPlatformProcess::GetDllExport(Dll, TEXT("bf6_read_terrain"));
	GDecals    = (FnDecals)    FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_decals"));
	GSetProgress = (FnSetProgress) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_set_progress"));
	GReadScoped  = (FnReadMeshScoped) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_read_mesh_scoped"));
	GTextureAt   = (FnTextureAt) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_texture_at"));
	GWater       = (FnWater)     FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_water"));
	GVarLive     = (FnVarLive)   FPlatformProcess::GetDllExport(Dll, TEXT("bf6_variation_live"));
	GWaterSim    = (FnWaterSim)  FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_water_sim"));
	GWaterRender = (FnWaterRender)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_water_render"));
	GFx           = (FnFx)          FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_fx"));
	GFxPlacements = (FnFxPlacements)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_fx_placements"));
	GFxAtlas      = (FnFxAtlas)     FPlatformProcess::GetDllExport(Dll, TEXT("bf6_fx_atlas_mip0"));
	GFxFrameUv    = (FnFxFrameUv)   FPlatformProcess::GetDllExport(Dll, TEXT("bf6_fx_frame_uv"));
	GBakeGround  = (FnBakeGround)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_bake_terrain"));
	GGroundCov   = (FnGroundCov) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_ground_coverage_get"));
	GLayerSheet  = (FnLayerSheet)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_layer_sheet"));
	GVeLighting  = (FnVeLighting)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_lighting"));
	GLevelLights = (FnLevelLights)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_lights"));
	if (!GOpen || !GOpenLevel || !GInstances)
	{
		// The tool ships a core too, and an older one has no bf6_open_level.
		// Saying so beats a null call.
		Error = TEXT("this bf6_core.dll is older than the add-on: it has no bf6_open_level");
		return false;
	}

	char err[512] = {0};
	Ctx = GOpen(TCHAR_TO_UTF8(*GameDir), err, sizeof(err));
	if (!Ctx) { Error = UTF8_TO_TCHAR(err); return false; }
	if (GSetProgress) GSetProgress(Ctx, &ProgressThunk, &Progress);
	return true;
}

bool FCore::OpenLevel(const FString& Level, const FString& ExePath)
{
	Error.Reset();
	if (!Ctx) { Error = TEXT("no install open"); return false; }
	char err[512] = {0};
	const int rc = GOpenLevel(Ctx, TCHAR_TO_UTF8(*Level),
		ExePath.IsEmpty() ? "" : TCHAR_TO_UTF8(*ExePath), 0, err, sizeof(err));
	if (rc != 0) { Error = UTF8_TO_TCHAR(err); return false; }
	return true;
}

bool FCore::Placements(const FString& Level, TArray<FPlacement>& Out)
{
	Out.Reset();
	if (!Ctx) { Error = TEXT("no install open"); return false; }

	// Asked for the count first, then filled. The core returns the true total
	// even when the buffer is short, which is the whole point of the two-step.
	const int n = GInstances(Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0);
	if (n <= 0) { Error = TEXT("no placements (was the level opened?)"); return false; }

	TArray<bf6_instance> Raw;
	Raw.SetNumZeroed(n);
	const int got = GInstances(Ctx, TCHAR_TO_UTF8(*Level), Raw.GetData(), n);
	const int take = FMath::Min(got, n);

	Out.Reserve(take);
	for (int i = 0; i < take; i++)
	{
		const bf6_instance& r = Raw[i];
		FPlacement p;
		p.Mesh    = r.res_name ? UTF8_TO_TCHAR(r.res_name) : TEXT("");
		p.Right   = FVector(r.xform[0], r.xform[1],  r.xform[2]);
		p.Up      = FVector(r.xform[3], r.xform[4],  r.xform[5]);
		p.Forward = FVector(r.xform[6], r.xform[7],  r.xform[8]);
		p.Origin  = FVector(r.xform[9], r.xform[10], r.xform[11]);
		p.Bundle    = r.placing_bundle ? UTF8_TO_TCHAR(r.placing_bundle) : TEXT("");
		p.Variation = r.variation      ? UTF8_TO_TCHAR(r.variation)      : TEXT("");
		Out.Add(MoveTemp(p));
	}
	return true;
}

FString FCore::MeshResourceFor(const FString& PlacementPath)
{
	FString s = PlacementPath;
	if (s.EndsWith(TEXT(".ebx"), ESearchCase::IgnoreCase)) s.LeftChopInline(4);
	return s + TEXT("_mesh");
}

bool FCore::ReadMesh(const FString& ResName, TArray<FSection>& Out,
                     const FString& PlacingBundle, const FString& Variation)
{
	Out.Reset();
	if (!Ctx || !GReadMesh) { Error = TEXT("no install open"); return false; }

	bf6_mesh* m = GReadScoped
		? GReadScoped(Ctx, TCHAR_TO_UTF8(*ResName), 0,
			PlacingBundle.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*PlacingBundle),
			Variation.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*Variation))
		: GReadMesh(Ctx, TCHAR_TO_UTF8(*ResName), 0);
	if (!m) { Error = FString::Printf(TEXT("no mesh at %s"), *ResName); return false; }

	for (int32 si = 0; si < m->section_count; si++)
	{
		const bf6_section& s = m->sections[si];
		if (s.vertex_count <= 0 || s.index_count <= 0 || !s.positions) continue;

		FSection out;
		out.Pos.Reserve(s.vertex_count);
		for (int32 v = 0; v < s.vertex_count; v++)
			out.Pos.Add(FVector3f(s.positions[v * 3], s.positions[v * 3 + 1], s.positions[v * 3 + 2]));

		if (s.normals)
		{
			out.Nrm.Reserve(s.vertex_count);
			for (int32 v = 0; v < s.vertex_count; v++)
				out.Nrm.Add(FVector3f(s.normals[v * 3], s.normals[v * 3 + 1], s.normals[v * 3 + 2]));
		}
		if (s.uv0)
		{
			out.UV.Reserve(s.vertex_count);
			for (int32 v = 0; v < s.vertex_count; v++)
				out.UV.Add(FVector2f(s.uv0[v * 2], s.uv0[v * 2 + 1]));
		}
		out.Idx.Append(s.indices, s.index_count);

		// The material sits beside the section, one per section.
		if (m->materials && si < m->material_count)
		{
			const bf6_material_desc& md = m->materials[si];
			out.bAlphaTest   = md.alpha_test != 0;
			out.bTranslucent = md.translucent != 0;
			out.bAlphaFromAlbedo = md.alpha_from_albedo != 0;
			out.bNsm         = md.normal_is_nsm != 0;
			out.BaseColor = FLinearColor(md.base_color[0], md.base_color[1],
			                             md.base_color[2], 1.f);
			out.Roughness = md.roughness;
			for (int32 b = 0; b < md.texture_count; b++)
			{
				FBinding bind;
				bind.Slot    = (int32)md.textures[b].slot;
				bind.Texture = md.textures[b].texture;
				out.Textures.Add(bind);
			}
		}
		Out.Add(MoveTemp(out));
	}

	if (GFree) GFree(Ctx, m);
	if (Out.Num() == 0) { Error = FString::Printf(TEXT("%s has no renderable section"), *ResName); return false; }
	return true;
}

bool FCore::ReadTerrain(const FString& Level, FTerrain& Out)
{
	Out.Heights.Reset();
	Out.Size = 0;
	if (!Ctx || !GTerrain) { Error = TEXT("no install open"); return false; }

	bf6_terrain* t = GTerrain(Ctx, TCHAR_TO_UTF8(*Level));
	if (!t || t->width <= 0 || !t->heights)
	{
		Error = TEXT("no heightfield for this level");
		return false;
	}
	Out.Size = t->width;
	Out.Heights.SetNumUninitialized(t->width * t->height);
	FMemory::Memcpy(Out.Heights.GetData(), t->heights, sizeof(uint16) * Out.Heights.Num());
	Out.WorldMin = FVector(t->world_min[0], t->world_min[1], t->world_min[2]);
	Out.WorldMax = FVector(t->world_max[0], t->world_max[1], t->world_max[2]);
	Out.HeightScale = t->height_scale;
	if (GFree) GFree(Ctx, t);
	return true;
}

bool FCore::ReadDecals(const FString& Level, TArray<FDecal>& Out)
{
	Out.Reset();
	Error.Reset();
	// A CORE WITHOUT THIS EXPORT IS NOT AN ERROR. The tool ships its own copy of
	// bf6_core.dll and an older one has no decals entry point; roads are then
	// simply absent, which is what the add-on did until now anyway. Refusing the
	// whole build over it would be worse than the missing layer.
	if (!Ctx || !GDecals) return false;

	const int32 n = GDecals(Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0);
	if (n <= 0) return false;

	TArray<bf6_decal> Raw;
	Raw.SetNumUninitialized(n);
	const int32 got = GDecals(Ctx, TCHAR_TO_UTF8(*Level), Raw.GetData(), n);

	Out.Reserve(got);
	for (int32 i = 0; i < got; i++)
	{
		const bf6_decal& r = Raw[i];
		if (!r.verts || r.vertex_count < 3) continue;
		FDecal d;
		// The pointer is the core's and stays valid until the next level is
		// opened, which is the same lifetime the placements already have.
		d.Verts       = r.verts;
		d.VertexCount = r.vertex_count;
		d.AabbMin = FVector(r.aabb_min[0], r.aabb_min[1], r.aabb_min[2]);
		d.AabbMax = FVector(r.aabb_max[0], r.aabb_max[1], r.aabb_max[2]);
		d.Tiling0 = r.tiling0;
		d.Tiling1 = r.tiling1;
		d.bPlanar = r.planar != 0;
		d.Albedo  = r.albedo;
		d.Opacity = r.opacity;
		d.Normal  = r.normal;
		d.Ao      = r.ao;
		d.Tint  = FLinearColor(r.tint[0],  r.tint[1],  r.tint[2]);
		d.Tint2 = FLinearColor(r.tint2[0], r.tint2[1], r.tint2[2]);
		d.bHasTint  = r.has_tint  != 0;
		d.bHasTint2 = r.has_tint2 != 0;
		d.MaskChannel = r.mask_channel;
		d.AssetSlot   = r.asset_slot;
		Out.Add(d);
	}
	return Out.Num() > 0;
}


bool FCore::ReadLighting(const FString& Level, FVELighting& Out)
{
	Out = FVELighting();
	Error.Reset();
	if (!Ctx || !GVeLighting)
	{
		Error = TEXT("this bf6_core.dll has no VisualEnvironment lighting decode");
		return false;
	}
	bf6_ve_lighting v{};
	char err[512] = {0};
	if (!GVeLighting(Ctx, TCHAR_TO_UTF8(*Level), &v, err, sizeof(err)))
	{
		Error = UTF8_TO_TCHAR(err);
		return false;
	}
	Out.Preset = UTF8_TO_TCHAR(v.preset);
	Out.SunBearingDeg = v.sun_rotation_x;
	Out.SunElevationDeg = v.sun_rotation_y;
	Out.SunColor = FLinearColor(v.sun_color[0], v.sun_color[1], v.sun_color[2]);
	Out.SunIntensityLux = v.sun_intensity;
	Out.SunAngularRadiusDeg = v.sun_angular_radius;
	Out.SkyLuminanceScale = v.sky_luminance_scale;
	Out.bAutoExposure = v.auto_exposure != 0;
	Out.ExposureEV = v.ev;
	Out.ExposureEVMax = v.ev_max;
	Out.ExposureCompensation = v.exposure_compensation;
	Out.SkyPanoramicRotationTurns = v.sky_panoramic_rotation;
	Out.PanoramaTexture = v.has_panorama ? v.panorama_texture : -1;
	Out.SkyTypeValue = v.sky_type;
	Out.Rayleigh = FLinearColor(v.rayleigh[0], v.rayleigh[1], v.rayleigh[2]);
	Out.MieCoefficient = v.mie_coefficient;
	Out.MieG = v.mie_g;
	Out.bHasSun = (v.components & BF6_VE_SUN) != 0;
	Out.bHasSky = (v.components & BF6_VE_SKY) != 0;
	Out.bHasFog = (v.components & BF6_VE_FOG) != 0;
	return true;
}

bool FCore::ReadLights(const FString& Level, TArray<FLight>& Out, FString& OutSummary)
{
	Out.Reset();
	OutSummary.Reset();
	Error.Reset();
	if (!Ctx || !GLevelLights)
	{
		Error = TEXT("this bf6_core.dll has no level light decode");
		return false;
	}

	// Asked for the count first, then filled. A map ships thousands, and
	// guessing a cap would silently drop whichever lights sorted last.
	bf6_light_stats st{};
	char err[512] = {0};
	if (!GLevelLights(Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0, &st, err, sizeof(err)))
	{
		Error = UTF8_TO_TCHAR(err);
		return false;
	}
	if (st.total <= 0) { OutSummary = TEXT("no lights"); return true; }

	TArray<bf6_light> Raw;
	Raw.SetNumZeroed(st.total);
	const int32 Got = GLevelLights(Ctx, TCHAR_TO_UTF8(*Level), Raw.GetData(),
	                               Raw.Num(), &st, err, sizeof(err));
	if (Got <= 0) { Error = UTF8_TO_TCHAR(err); return false; }

	Out.Reserve(Got);
	for (int32 i = 0; i < Got && i < Raw.Num(); i++)
	{
		const bf6_light& L = Raw[i];
		FLight F;
		F.Type = L.type;
		// 3x4 row-major: rows 0..2 are the basis, row 3 the origin.
		F.Right   = FVector(L.xform[0], L.xform[1], L.xform[2]);
		F.Up      = FVector(L.xform[3], L.xform[4], L.xform[5]);
		F.Forward = FVector(L.xform[6], L.xform[7], L.xform[8]);
		F.Origin  = FVector(L.xform[9], L.xform[10], L.xform[11]);
		F.Color = FLinearColor(L.color[0], L.color[1], L.color[2]);
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
		Out.Add(MoveTemp(F));
	}
	OutSummary = FString::Printf(
		TEXT("%d light(s): %d sphere, %d spot, %d tube, %d rect"),
		st.total, st.sphere, st.spot, st.tube, st.rect);
	return true;
}

bool FCore::GroundCoverage(const FString& Level, int32 Size, FGroundCoverage& Out)
{
	Out = FGroundCoverage();
	Error.Reset();
	if (!Ctx || !GGroundCov)
	{
		Error = TEXT("this bf6_core.dll has no per-pixel ground coverage");
		return false;
	}

	bf6_ground_coverage g{};
	char err[512] = {0};
	if (!GGroundCov(Ctx, TCHAR_TO_UTF8(*Level), Size, &g, err, sizeof(err)))
	{
		Error = UTF8_TO_TCHAR(err);
		return false;
	}
	Out.Size = g.size;
	Out.Lo = FVector2D(g.lo[0], g.lo[1]);
	Out.Hi = FVector2D(g.hi[0], g.hi[1]);
	Out.Idx = g.idx;
	Out.Weight = g.weight;
	Out.Colour = g.colour;
	Out.EmptyFraction = g.empty_fraction;
	Out.Materials.Reserve(g.material_count);
	for (int32 i = 0; i < g.material_count; i++)
	{
		const bf6_ground_material& m = g.materials[i];
		FGroundMaterial M;
		M.Layer = m.layer;
		M.Albedo = m.albedo_res ? UTF8_TO_TCHAR(m.albedo_res) : TEXT("");
		M.Normal = m.normal_res ? UTF8_TO_TCHAR(m.normal_res) : TEXT("");
		// A repeat of zero would divide the world by nothing in the shader.
		M.MetresPerRepeat = m.metres_per_repeat > 0.01f ? m.metres_per_repeat : 4.f;
		M.RotationDeg = m.uv_rotation_deg;
		M.Tint = FLinearColor(m.tint[0], m.tint[1], m.tint[2], 1.f);
		M.Overlay = m.overlay;
		M.BaseHeight = m.base_height;
		M.DisplaceRange = m.displace_range;
		M.MaskRampExp = m.mask_ramp_exp > 0.f ? m.mask_ramp_exp : 1.f;
		M.HeightBlend = m.height_blend;
		M.CoordScale = FVector2f(m.coord_scale[0] != 0.f ? m.coord_scale[0] : 1.f,
		                         m.coord_scale[1] != 0.f ? m.coord_scale[1] : 1.f);
		M.UvOffset = FVector2f(m.uv_offset[0], m.uv_offset[1]);
		Out.Materials.Add(MoveTemp(M));
	}
	return Out.Size > 0 && Out.Idx != nullptr && Out.Weight != nullptr;
}

bool FCore::LayerSheet(const FString& ResName, int32 Size, TArray<uint8>& Out)
{
	Error.Reset();
	if (!Ctx || !GLayerSheet)
	{
		Error = TEXT("this bf6_core.dll has no layer sheet decode");
		return false;
	}
	if (Size <= 0 || ResName.IsEmpty()) { Error = TEXT("bad arguments"); return false; }

	Out.SetNumUninitialized(Size * Size * 4);
	char err[512] = {0};
	if (!GLayerSheet(Ctx, TCHAR_TO_UTF8(*ResName), Size, Out.GetData(), err, sizeof(err)))
	{
		Error = UTF8_TO_TCHAR(err);
		Out.Reset();
		return false;
	}
	return true;
}

bool FCore::BakeGround(const FString& Level, const FVector2D& RectMin, float RectSize,
                       int32 Size, FGroundBake& Out)
{
	Out = FGroundBake();
	Error.Reset();
	if (!Ctx || !GBakeGround) { Error = TEXT("this bf6_core.dll has no ground bake"); return false; }

	bf6_terrain_bake_opts o{};
	o.rect_min[0] = (float)RectMin.X;
	o.rect_min[1] = (float)RectMin.Y;
	o.rect_size   = RectSize;
	o.size        = Size;
	o.want_normal = 1;
	o.stochastic  = 1;
	o.colour_map  = 1;
	// Where no layer resolves, the aerial photograph of that spot beats the
	// game's magenta. On an urban map that is most of the ground.
	o.fallback_colour_map = 1;

	bf6_terrain_bake b{};
	char err[512] = {0};
	if (!GBakeGround(Ctx, TCHAR_TO_UTF8(*Level), &o, &b, err, sizeof(err)))
	{
		Error = UTF8_TO_TCHAR(err);
		return false;
	}
	Out.Size = b.size;
	Out.Lo = FVector2D(b.lo[0], b.lo[1]);
	Out.Hi = FVector2D(b.hi[0], b.hi[1]);
	Out.MetresPerTexel = b.metres_per_texel;
	Out.Albedo = b.albedo;
	Out.Normal = b.normal;
	Out.LayersUsed = b.layers_used;
	Out.LayersTextured = b.layers_textured;
	Out.FallbackFraction = b.fallback_fraction;
	return Out.Albedo != nullptr && Out.Size > 0;
}

bool FCore::ReadWaterSim(const FString& Level, FWaterSim& Out)
{
	Out = FWaterSim();
	if (!Ctx || !GWaterSim) return false;
	bf6_water_sim s{};
	if (!GWaterSim(Ctx, TCHAR_TO_UTF8(*Level), &s)) return false;
	Out.WindAngle = s.wind_angle;
	Out.WindSpeed = s.wind_speed;
	Out.Choppiness = s.choppiness;
	Out.TileDimension = s.tile_dimension;
	Out.MinWavelength = s.min_wavelength;
	Out.LargeWaveReduction = s.large_wave_reduction;
	Out.FoamThreshold = s.foam_threshold;
	Out.FoamMax = s.foam_max;
	Out.bFlagged = s.enabled != 0;
	for (int32 i = 0; i < s.dist_count && i < 13; i++)
		Out.Dist.Add(FVector2D(s.dist_x[i], s.dist_y[i]));
	return true;
}

bool FCore::VariationLive(const FString& ResName, const FString& Bundle,
                          const FString& Variation)
{
	// An older core cannot answer, and the safe answer is NO SPLIT: the base
	// look for everything, which is what the add-on always did.
	if (!Ctx || !GVarLive || Variation.IsEmpty()) return false;
	return GVarLive(Ctx, TCHAR_TO_UTF8(*ResName),
	                Bundle.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*Bundle),
	                TCHAR_TO_UTF8(*Variation)) != 0;
}

bool FCore::ReadWater(const FString& Level, TArray<FWater>& Out)
{
	Out.Reset();
	Error.Reset();
	// Same contract as the decals: an older core without the entry point means
	// no water, which is what the add-on did until now anyway.
	if (!Ctx || !GWater) return false;

	const int32 n = GWater(Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0);
	if (n <= 0) return false;

	TArray<bf6_water> Raw;
	Raw.SetNumUninitialized(n);
	const int32 got = GWater(Ctx, TCHAR_TO_UTF8(*Level), Raw.GetData(), n);
	Out.Reserve(got);
	for (int32 i = 0; i < got; i++)
	{
		const bf6_water& r = Raw[i];
		FWater w;
		w.Center = FVector2D(r.center[0], r.center[1]);
		w.Size   = FVector2D(r.size[0], r.size[1]);
		w.Height = r.height;
		if (r.shallow[0] >= 0.f) w.Shallow = FLinearColor(r.shallow[0], r.shallow[1], r.shallow[2]);
		if (r.deep[0]    >= 0.f) w.Deep    = FLinearColor(r.deep[0], r.deep[1], r.deep[2]);
		w.bOcean = r.is_ocean != 0;
		Out.Add(w);
	}

	// THE DECODED OPTICS, merged in by index.
	//
	// A separate entry point because it came from a separate decode, and an
	// older core will not export it at all - in which case these stay absent
	// and the caller falls back to the heuristic, which is what it did before.
	if (GWaterRender)
	{
		const int32 rn = GWaterRender(Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0);
		if (rn > 0)
		{
			TArray<bf6_water_render> R;
			R.SetNumUninitialized(rn);
			const int32 rgot = GWaterRender(Ctx, TCHAR_TO_UTF8(*Level), R.GetData(), rn);
			for (int32 i = 0; i < rgot && i < Out.Num(); i++)
			{
				const bf6_water_render& s = R[i];
				// A NEGATIVE SENTINEL MEANS ABSENT, not zero. Zero is a legal
				// authored value for most of this, and two of the four ocean
				// levels carry none of the named slots at all.
				if (s.extinction[0] >= 0.f)
					Out[i].Extinction = FLinearColor(
						s.extinction[0], s.extinction[1], s.extinction[2]);
				if (s.surface_colour[0] >= 0.f)
					Out[i].SurfaceColour = FLinearColor(
						s.surface_colour[0], s.surface_colour[1], s.surface_colour[2]);
				Out[i].AbsorptionDistanceM = s.absorption_distance_m;
			}
		}
	}
	return Out.Num() > 0;
}

// Parameter ids, djb2-xor of the authored name. These four are the ones a
// billboard cannot be drawn without; everything else stays in the raw table.
static const uint32 kFxSize0     = 0x0DCF4930u;   // Vec2
static const uint32 kFxSizeX     = 0x0DCF4958u;   // Vec2
static const uint32 kFxSizeY     = 0x0DCF4959u;   // Vec2
static const uint32 kFxColor     = 0x0CA8C5F8u;   // Vec3
static const uint32 kFxAlpha     = 0x0C426471u;   // Float
static const uint32 kFxOpacity   = 0x01C5CADCu;   // Float
static const uint32 kFxAlphaCull = 0x1487F4B0u;   // Float

bool FCore::ReadFx(const FString& Level, TArray<FFxLayer>& Out)
{
	Out.Reset();
	Error.Reset();
	if (!Ctx || !GFx) return false;

	char err[512] = {0};
	const int32 n = GFx(Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0, nullptr, err, sizeof(err));
	if (n <= 0) { Error = UTF8_TO_TCHAR(err); return false; }

	TArray<bf6_fx_layer> Raw;
	Raw.SetNumUninitialized(n);
	bf6_fx_stats St{};
	const int32 got = GFx(Ctx, TCHAR_TO_UTF8(*Level), Raw.GetData(), n, &St, err, sizeof(err));
	Out.Reserve(got);
	for (int32 i = 0; i < got; i++)
	{
		const bf6_fx_layer& r = Raw[i];
		FFxLayer L;
		L.Effect      = r.effect      ? UTF8_TO_TCHAR(r.effect)      : TEXT("");
		L.EffectPath  = r.effect_path ? UTF8_TO_TCHAR(r.effect_path) : TEXT("");
		L.Graph       = r.graph       ? UTF8_TO_TCHAR(r.graph)       : TEXT("");
		L.Family      = r.family      ? UTF8_TO_TCHAR(r.family)      : TEXT("");
		L.Placements  = r.placements;
		L.LayerIndex  = i;
		L.Right   = FVector(r.local[0], r.local[1], r.local[2]);
		L.Up      = FVector(r.local[3], r.local[4], r.local[5]);
		L.Forward = FVector(r.local[6], r.local[7], r.local[8]);
		L.Origin  = FVector(r.local[9], r.local[10], r.local[11]);
		L.Atlas          = r.atlas ? UTF8_TO_TCHAR(r.atlas) : TEXT("");
		L.AtlasCols      = r.atlas_cols;
		L.AtlasFrames    = r.atlas_frames;
		L.AtlasLeftRight = r.atlas_left_right;
		L.AtlasWidth     = r.atlas_width;
		L.AtlasHeight    = r.atlas_height;
		L.LightingModel  = r.lighting_model;
		L.Alignment      = r.alignment;
		L.ParticleLife   = r.particle_life;
		L.EmitterLife    = r.emitter_life;
		L.CullDistance   = r.gpu_cull_distance;
		L.ParticleMax    = r.particle_max;
		// The parameter table is a flat list keyed by name hash. Absent stays
		// absent - a negative sentinel - because 0 is a legal authored size.
		for (int32 k = 0; k < r.param_count; k++)
		{
			const bf6_fx_param& p = r.params[k];
			switch (p.pid)
			{
			case kFxSize0: L.Size = FVector2D(p.v[0], p.v[1]); break;
			case kFxSizeX: if (L.Size.X < 0.f) L.Size.X = p.v[0]; break;
			case kFxSizeY: if (L.Size.Y < 0.f) L.Size.Y = p.v[0]; break;
			case kFxColor: L.Colour = FLinearColor(p.v[0], p.v[1], p.v[2]); break;
			case kFxAlpha: case kFxOpacity: L.Alpha = p.v[0]; break;
			case kFxAlphaCull: L.AlphaCull = p.v[0]; break;
			default: break;
			}
		}
		Out.Add(MoveTemp(L));
	}
	return Out.Num() > 0;
}

bool FCore::FxPlacements(const FString& Level, const FString& Effect,
                         TArray<FPlacement>& Out)
{
	Out.Reset();
	if (!Ctx || !GFxPlacements || Effect.IsEmpty()) return false;
	const int32 n = GFxPlacements(Ctx, TCHAR_TO_UTF8(*Level), TCHAR_TO_UTF8(*Effect), nullptr, 0);
	if (n <= 0) return false;
	TArray<float> Xf;
	Xf.SetNumUninitialized(n * 12);
	const int32 got = GFxPlacements(Ctx, TCHAR_TO_UTF8(*Level), TCHAR_TO_UTF8(*Effect),
	                                Xf.GetData(), n);
	Out.Reserve(got);
	for (int32 i = 0; i < got; i++)
	{
		const float* m = Xf.GetData() + (int64)i * 12;
		FPlacement p;
		p.Right   = FVector(m[0], m[1], m[2]);
		p.Up      = FVector(m[3], m[4], m[5]);
		p.Forward = FVector(m[6], m[7], m[8]);
		p.Origin  = FVector(m[9], m[10], m[11]);
		Out.Add(p);
	}
	return Out.Num() > 0;
}

bool FCore::FxAtlas(const FString& Level, int32 LayerIndex, TArray<uint8>& Out)
{
	Out.Reset();
	if (!Ctx || !GFxAtlas || LayerIndex < 0) return false;
	char err[512] = {0};
	const uint8_t* Data = nullptr;
	int32 Size = 0;
	if (!GFxAtlas(Ctx, TCHAR_TO_UTF8(*Level), LayerIndex, &Data, &Size, err, sizeof(err))
	    || !Data || Size <= 0)
	{ Error = UTF8_TO_TCHAR(err); return false; }
	Out.Append(Data, Size);
	return true;
}

bool FCore::FxFrameUV(const FFxLayer& L, int32 Frame, FVector4f& Out)
{
	Out = FVector4f(0.f, 0.f, 1.f, 1.f);
	if (!GFxFrameUv) return false;
	// The core owns this arithmetic on purpose: six sheets in thirty have a
	// fractional pixel cell, so a caller computing rects from cols and pixels
	// gets them wrong on exactly the map's fire.
	bf6_fx_layer R{};
	R.atlas_cols = L.AtlasCols;
	R.atlas_frames = L.AtlasFrames;
	R.atlas_left_right = L.AtlasLeftRight;
	R.atlas_width = L.AtlasWidth;
	R.atlas_height = L.AtlasHeight;
	float uv[4] = {0.f, 0.f, 1.f, 1.f};
	if (!GFxFrameUv(&R, Frame, uv)) return false;
	Out = FVector4f(uv[0], uv[1], uv[2], uv[3]);
	return true;
}

bool FCore::TextureAt(int32 Id, FTexture& Out)
{
	Out = FTexture();
	if (!Ctx || !GTextureAt || Id < 0) return false;
	const bf6_texture* t = GTextureAt(Ctx, Id);
	if (!t || !t->data || t->data_len <= 0) return false;
	Out.Width  = t->width;
	Out.Height = t->height;
	Out.Format = (int32)t->format;
	Out.MipCount = FMath::Max(1, t->mip_count);
	Out.bSrgb  = t->srgb != 0;
	// Borrowed, not copied: the core owns these blocks for the life of the
	// context, and copying a 4 MB sheet per binding would be pointless.
	Out.Data    = t->data;
	Out.DataLen = t->data_len;
	return true;
}

}  // namespace BF6HP
