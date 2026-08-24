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
	typedef int (*FnDecals)(bf6_ctx*, const char*, bf6_decal*, int);

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
		Out.Add(d);
	}
	return Out.Num() > 0;
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
	return Out.Num() > 0;
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
