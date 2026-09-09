#include "BF6HighPolyCore.h"
#include "BF6SDKExtension.h"   // BF6Ext::ToolPluginDir, for the core dll path

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
	typedef const bf6_texture* (*FnTextureAtMaxDim)(bf6_ctx*, int, int);
	typedef const char* (*FnTextureNameAt)(bf6_ctx*, int);
	typedef int (*FnTextureIdByName)(bf6_ctx*, const char*);
	typedef int (*FnWater)(bf6_ctx*, const char*, bf6_water*, int);
	typedef int (*FnVarLive)(bf6_ctx*, const char*, const char*, const char*);
	typedef int (*FnWaterSim)(bf6_ctx*, const char*, bf6_water_sim*);
	typedef int (*FnWaterSims)(bf6_ctx*, const char*, bf6_water_sim_v2*, int);
	typedef int (*FnWaterH0)(const bf6_water_sim_v2*, float*, int);
	typedef int (*FnSeaState)(bf6_ctx*, const char*, bf6_ocean_sea_state*);
	typedef int (*FnWaterRender)(bf6_ctx*, const char*, bf6_water_render*, int);
	typedef int (*FnWaterMask)(bf6_ctx*, const char*, bf6_water_mask*, char*, int);
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
	typedef int (*FnScatter)(bf6_ctx*, const char*, bf6_scatter_entry*, int,
	                         char*, int);
	typedef int64_t (*FnReadRaw)(bf6_ctx*, int, const char*, const uint8_t**);
	typedef int (*FnMountAll)(bf6_ctx*, int, char*, int);
	typedef int (*FnMountLevelArchives)(bf6_ctx*, const char*, char*, int);
	typedef int (*FnMaterialScopeExists)(bf6_ctx*, const char*);
	typedef int (*FnListEbx)(bf6_ctx*, const char*, bf6_asset*, int);
	typedef int (*FnAssetInstances)(bf6_ctx*, const char*, bf6_instance*, int, char*, int);

	FnOpen      GOpen      = nullptr;
	FnClose     GClose     = nullptr;
	FnOpenLevel GOpenLevel = nullptr;
	FnInstances GInstances = nullptr;
	FnReadMesh  GReadMesh  = nullptr;
	FnFree      GFree      = nullptr;
	FnTerrain   GTerrain   = nullptr;
	FnTerrain   GWaterHeightfield = nullptr;
	FnDecals    GDecals    = nullptr;
	FnSetProgress GSetProgress = nullptr;
	FnReadMeshScoped GReadScoped = nullptr;
	FnReadMeshScoped GReadArmoryScoped = nullptr;
	FnTextureAt GTextureAt = nullptr;
	FnTextureAtMaxDim GTextureAtMaxDim = nullptr;
	FnTextureNameAt GTextureNameAt = nullptr;
	FnTextureIdByName GTextureIdByName = nullptr;
	FnWater     GWater     = nullptr;
	FnVarLive   GVarLive   = nullptr;
	FnWaterSim  GWaterSim  = nullptr;
	FnWaterSims GWaterSims = nullptr;
	FnWaterSims GWaterSimsIsolated = nullptr;
	FnWaterH0   GWaterH0   = nullptr;
	FnSeaState  GSeaState  = nullptr;
	FnWaterRender GWaterRender = nullptr;
	FnWaterMask   GWaterMask = nullptr;
	FnFx           GFx = nullptr;
	FnFxPlacements GFxPlacements = nullptr;
	FnFxAtlas      GFxAtlas = nullptr;
	FnFxFrameUv    GFxFrameUv = nullptr;
	FnBakeGround GBakeGround = nullptr;
	FnGroundCov  GGroundCov  = nullptr;
	FnLayerSheet GLayerSheet = nullptr;
	FnVeLighting GVeLighting = nullptr;
	FnLevelLights GLevelLights = nullptr;
	FnScatter    GScatter    = nullptr;
	FnReadRaw    GReadRaw    = nullptr;
	FnMountAll   GMountAll   = nullptr;
	FnMountLevelArchives GMountLevelArchives = nullptr;
	FnMaterialScopeExists GMaterialScopeExists = nullptr;
	FnListEbx    GListEbx    = nullptr;
	FnAssetInstances GAssetInstances = nullptr;

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
	Close();
}

void FCore::Close()
{
	if (Ctx && GSetProgress) GSetProgress(Ctx, nullptr, nullptr);
	if (Ctx && GClose) GClose(Ctx);
	Ctx = nullptr;
	OpenedFor.Reset();
	// The handle is deliberately NOT freed: the tool's own module may have the
	// same dll loaded, and unloading it out from under that would take the
	// editor with it.
	Dll = nullptr;
}

FString CoreDllPath()
{
	const FString Root = BF6Ext::ToolPluginDir();
	const FString Staged = FPaths::Combine(Root, TEXT("Binaries/Win64/bf6_core.dll"));
	const FString Dev    = FPaths::Combine(Root, TEXT("Source/ThirdParty/libbf6/bin/Win64/bf6_core.dll"));
	if (FPaths::FileExists(Staged)) { return Staged; }
	if (FPaths::FileExists(Dev))    { return Dev; }
	// Neither is there. Hand back the staged path so the failure names the
	// one an installed plugin is supposed to have.
	return Staged;
}

bool FCore::Open(const FString& GameDir, const FString& DllPath)
{
	Error.Reset();
	if (Ctx)
	{
		if (OpenedFor.IsEmpty() || OpenedFor == GameDir) { return true; }
		// Reopening in place is not safe here: catalogue workers may be holding
		// this context on other threads, and closing it under them is the crash
		// class this add-on already has. Refusing loudly is the honest answer
		// until core job ownership exists (audit HP-07).
		Error = FString::Printf(
			TEXT("the high poly reader is open on %s, but %s was asked for. ")
			TEXT("Restart the editor after changing the game folder."),
			*OpenedFor, *GameDir);
		return false;
	}

	Dll = FPlatformProcess::GetDllHandle(*DllPath);
	if (!Dll) { Error = FString::Printf(TEXT("could not load %s"), *DllPath); return false; }

	// The host checks its reader, but this add-on owns a separate load path.
	// Check before publishing function pointers or reading ABI-sized structs:
	// an exported function name alone does not establish layout compatibility.
	using FnAbiVersion = int (*)();
	const FnAbiVersion AbiVersion = (FnAbiVersion)FPlatformProcess::GetDllExport(
		Dll, TEXT("bf6_abi_version"));
	const int32 RuntimeAbi = AbiVersion ? AbiVersion() : -1;
	if (RuntimeAbi != BF6_ABI_VERSION)
	{
		Error = FString::Printf(
			TEXT("High Poly needs reader ABI %d, but %s reports %d. ")
			TEXT("Install the matching tool and add-on update, then restart Unreal."),
			BF6_ABI_VERSION, *DllPath, RuntimeAbi);
		// Release only the reference acquired above; another consumer may own
		// the same library. Existing shared decoder pointers remain untouched.
		FPlatformProcess::FreeDllHandle(Dll);
		Dll = nullptr;
		return false;
	}

	GOpen      = (FnOpen)      FPlatformProcess::GetDllExport(Dll, TEXT("bf6_open"));
	GClose     = (FnClose)     FPlatformProcess::GetDllExport(Dll, TEXT("bf6_close"));
	GOpenLevel = (FnOpenLevel) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_open_level"));
	GInstances = (FnInstances) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_instances"));
	GReadMesh  = (FnReadMesh)  FPlatformProcess::GetDllExport(Dll, TEXT("bf6_read_mesh"));
	GFree      = (FnFree)      FPlatformProcess::GetDllExport(Dll, TEXT("bf6_free"));
	GMountAll  = (FnMountAll)  FPlatformProcess::GetDllExport(Dll, TEXT("bf6_mount_all"));
	GMountLevelArchives = (FnMountLevelArchives) FPlatformProcess::GetDllExport(
		Dll, TEXT("bf6_mount_level_archives"));
	GMaterialScopeExists = (FnMaterialScopeExists) FPlatformProcess::GetDllExport(
		Dll, TEXT("bf6_material_scope_exists"));
	GListEbx   = (FnListEbx)   FPlatformProcess::GetDllExport(Dll, TEXT("bf6_list_ebx"));
	GAssetInstances = (FnAssetInstances) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_asset_instances"));
	GTerrain   = (FnTerrain)   FPlatformProcess::GetDllExport(Dll, TEXT("bf6_read_terrain"));
	GWaterHeightfield = (FnTerrain)FPlatformProcess::GetDllExport(
		Dll, TEXT("bf6_read_water_heightfield"));
	GDecals    = (FnDecals)    FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_decals"));
	GSetProgress = (FnSetProgress) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_set_progress"));
	GReadScoped  = (FnReadMeshScoped) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_read_mesh_scoped"));
	GReadArmoryScoped = (FnReadMeshScoped) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_read_armory_mesh_scoped"));
	GTextureAt   = (FnTextureAt) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_texture_at"));
	GTextureAtMaxDim = (FnTextureAtMaxDim)FPlatformProcess::GetDllExport(
		Dll, TEXT("bf6_texture_at_max_dim"));
	GTextureNameAt = (FnTextureNameAt)FPlatformProcess::GetDllExport(
		Dll, TEXT("bf6_texture_name_at"));
	GTextureIdByName = (FnTextureIdByName)FPlatformProcess::GetDllExport(
		Dll, TEXT("bf6_texture_id_by_name"));
	GWater       = (FnWater)     FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_water"));
	GVarLive     = (FnVarLive)   FPlatformProcess::GetDllExport(Dll, TEXT("bf6_variation_live"));
	GWaterSim    = (FnWaterSim)  FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_water_sim"));
	GWaterSims   = (FnWaterSims) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_water_sims"));
	GWaterSimsIsolated = (FnWaterSims)FPlatformProcess::GetDllExport(
		Dll, TEXT("bf6_level_water_sims_isolated"));
	GWaterH0     = (FnWaterH0)   FPlatformProcess::GetDllExport(Dll, TEXT("bf6_water_spectrum_h0"));
	// Optional: an older bf6_core.dll has no sea-state read. A null here
	// leaves the authored wind in place rather than a baked number.
	GSeaState    = (FnSeaState)  FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_ocean_sea_state"));
	GWaterRender = (FnWaterRender)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_water_render"));
	GWaterMask   = (FnWaterMask)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_water_mask"));
	GFx           = (FnFx)          FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_fx"));
	GFxPlacements = (FnFxPlacements)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_fx_placements"));
	GFxAtlas      = (FnFxAtlas)     FPlatformProcess::GetDllExport(Dll, TEXT("bf6_fx_atlas_mip0"));
	GFxFrameUv    = (FnFxFrameUv)   FPlatformProcess::GetDllExport(Dll, TEXT("bf6_fx_frame_uv"));
	GBakeGround  = (FnBakeGround)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_bake_terrain"));
	GGroundCov   = (FnGroundCov) FPlatformProcess::GetDllExport(Dll, TEXT("bf6_ground_coverage_get"));
	GLayerSheet  = (FnLayerSheet)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_layer_sheet"));
	GVeLighting  = (FnVeLighting)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_lighting"));
	GLevelLights = (FnLevelLights)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_lights"));
	GScatter     = (FnScatter)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_level_scatter"));
	GReadRaw     = (FnReadRaw)FPlatformProcess::GetDllExport(Dll, TEXT("bf6_read_raw"));
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
	OpenedFor = GameDir;   // so a later Open for a different install is caught
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
	// The sea state belongs to the level, so it is read here rather than left
	// to whoever happens to build the water first.
	ApplyLevelSeaState(Level);
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

bool FCore::MountAll(bool bIncludeLevels)
{
	Error.Reset();
	if (!Ctx) { Error = TEXT("no install open"); return false; }
	if (!GMountAll) { Error = TEXT("this bf6_core.dll has no bf6_mount_all"); return false; }
	char err[512] = {0};
	if (!GMountAll(Ctx, bIncludeLevels ? 1 : 0, err, sizeof(err)))
	{
		Error = err[0] ? UTF8_TO_TCHAR(err) : TEXT("bf6_mount_all failed");
		return false;
	}
	return true;
}

bool FCore::MountLevelArchives(const FString& Level)
{
	Error.Reset();
	if (!Ctx) { Error = TEXT("no install open"); return false; }
	if (!GMountLevelArchives)
	{
		Error = TEXT("this bf6_core.dll has no bf6_mount_level_archives");
		return false;
	}
	char err[512] = {0};
	if (!GMountLevelArchives(Ctx, TCHAR_TO_UTF8(*Level), err, sizeof(err)))
	{
		Error = err[0] ? UTF8_TO_TCHAR(err) : TEXT("bf6_mount_level_archives failed");
		return false;
	}
	return true;
}

bool FCore::ListEbx(const FString& Search, TArray<FString>& Out)
{
	Out.Reset();
	Error.Reset();
	if (!Ctx) { Error = TEXT("no install open"); return false; }
	if (!GListEbx) { Error = TEXT("this bf6_core.dll has no bf6_list_ebx"); return false; }
	// Count first, then fill: the core returns the true total on a short buffer.
	const int n = GListEbx(Ctx, TCHAR_TO_UTF8(*Search), nullptr, 0);
	if (n <= 0) return true;
	TArray<bf6_asset> Raw;
	Raw.SetNumZeroed(n);
	const int got = FMath::Min(GListEbx(Ctx, TCHAR_TO_UTF8(*Search), Raw.GetData(), n), n);
	Out.Reserve(got);
	for (int i = 0; i < got; i++)
		if (Raw[i].name) Out.Add(UTF8_TO_TCHAR(Raw[i].name));
	return true;
}

bool FCore::AssetInstances(const FString& Asset, TArray<FPlacement>& Out)
{
	Out.Reset();
	Error.Reset();
	if (!Ctx) { Error = TEXT("no install open"); return false; }
	if (!GAssetInstances) { Error = TEXT("this bf6_core.dll has no bf6_asset_instances"); return false; }
	char err[512] = {0};
	const int n = GAssetInstances(Ctx, TCHAR_TO_UTF8(*Asset), nullptr, 0, err, sizeof(err));
	if (n <= 0)
	{
		Error = err[0] ? UTF8_TO_TCHAR(err) : FString::Printf(TEXT("%s: no placements"), *Asset);
		return false;
	}
	TArray<bf6_instance> Raw;
	Raw.SetNumZeroed(n);
	const int got = GAssetInstances(Ctx, TCHAR_TO_UTF8(*Asset), Raw.GetData(), n, err, sizeof(err));
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
                     const FString& PlacingBundle, const FString& Variation,
                     const TArray<FMatrix44f>* Skin, int32 RigBoneCount)
{
	Out.Reset();
	if (!Ctx || !GReadMesh) { Error = TEXT("no install open"); return false; }

	// Vehicle materials use the bounded hardware partition index, like the
	// reference armory viewer. The generic index scans unrelated level files.
	const FnReadMeshScoped ReadScoped = GReadArmoryScoped
		&& (ResName.StartsWith(TEXT("common/hardware/"), ESearchCase::IgnoreCase)
			|| ResName.StartsWith(TEXT("common/characters/"), ESearchCase::IgnoreCase))
		? GReadArmoryScoped : GReadScoped;
	bf6_mesh* m = ReadScoped
		? ReadScoped(Ctx, TCHAR_TO_UTF8(*ResName), 0,
			PlacingBundle.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*PlacingBundle),
			Variation.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*Variation))
		: GReadMesh(Ctx, TCHAR_TO_UTF8(*ResName), 0);
	if (!m) { Error = FString::Printf(TEXT("no mesh at %s"), *ResName); return false; }

	// A SCOPED READ THAT BOUND NOTHING IS NOT AN ANSWER.
	//
	// The placing bundle is the exact scope for a material, and passing it is
	// right - a shader state key is only unique within a bundle, so resolving
	// anywhere else can bind a material that merely collides. But when that
	// bundle has no shader-state depot at all, the scoped read comes back with
	// geometry and no bindings whatsoever, and an untextured high-poly mesh
	// looks exactly like the white blockout it was supposed to replace. That is
	// the difference between "it did not upgrade" and "it upgraded and has no
	// textures", and from the viewport they are the same complaint.
	//
	// bf6viewer recovers the same situation for weapon parts through the armory
	// index. There is no armory here, so the recovery is the plain unscoped
	// read - the one the core documents as correct for a mesh with no placement
	// above it. It is only ever reached when the scoped read bound nothing, so
	// it cannot replace a good exact material with a colliding one.
	if (GReadScoped && !PlacingBundle.IsEmpty())
	{
		bool bBoundAnything = false;
		for (int32 mi = 0; mi < m->material_count && !bBoundAnything; mi++)
			bBoundAnything = m->materials[mi].texture_count > 0;
		if (!bBoundAnything)
		{
			// Ask the core whether that bundle could ever have bound: a miss
			// here is the cause, and a hit means the material is genuinely
			// untextured and must be left alone.
			const bool bScopeExists = GMaterialScopeExists
				&& GMaterialScopeExists(Ctx, TCHAR_TO_UTF8(*PlacingBundle)) == 1;
			if (!bScopeExists)
			{
				if (bf6_mesh* Plain = GReadMesh(Ctx, TCHAR_TO_UTF8(*ResName), 0))
				{
					bool bPlainBound = false;
					for (int32 mi = 0; mi < Plain->material_count && !bPlainBound; mi++)
						bPlainBound = Plain->materials[mi].texture_count > 0;
					if (bPlainBound)
					{
						if (GFree) GFree(Ctx, m);
						m = Plain;
						MaterialsRecovered++;
					}
					else if (GFree) { GFree(Ctx, Plain); }
				}
			}
		}
	}

	for (int32 si = 0; si < m->section_count; si++)
	{
		const bf6_section& s = m->sections[si];
		if (s.vertex_count <= 0 || s.index_count <= 0 || !s.positions) continue;

		FSection out;
		out.bDecal = s.is_decal != 0;
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
		if (Skin && !Skin->IsEmpty() && m->mesh_type == 1 && s.skin_bones && s.skin_weights)
		{
			for (int32 v = 0; v < s.vertex_count; ++v)
			{
				FVector3f P = FVector3f::ZeroVector, N = FVector3f::ZeroVector;
				float Accepted = 0.f;
				for (int32 Lane = 0; Lane < s.skin_influences; ++Lane)
				{
					const int32 I = v * s.skin_influences + Lane;
					const uint16 Raw = s.skin_bones[I];
					const int32 Bone = (Raw & 0x8000) ? RigBoneCount + ((Raw & 0x7fff) >> 1) : Raw;
					const float Weight = s.skin_weights[I];
					if (Weight <= 0.f) continue;
					if (!Skin->IsValidIndex(Bone))
					{
						if (GFree) GFree(Ctx, m);
						Out.Reset(); Error = TEXT("preview skin references an unavailable bone"); return false;
					}
					P += FVector3f((*Skin)[Bone].TransformPosition(out.Pos[v])) * Weight;
					if (out.Nrm.IsValidIndex(v)) N += FVector3f((*Skin)[Bone].TransformVector(out.Nrm[v])) * Weight;
					Accepted += Weight;
				}
				if (Accepted > SMALL_NUMBER)
				{
					out.Pos[v] = P / Accepted;
					if (out.Nrm.IsValidIndex(v)) out.Nrm[v] = N.GetSafeNormal();
				}
			}
		}
		out.Idx.Append(s.indices, s.index_count);

		// The material sits beside the section, one per section.
		if (m->materials && s.material >= 0 && s.material < m->material_count)
		{
			const bf6_material_desc& md = m->materials[s.material];
			out.bAlphaTest   = md.alpha_test != 0;
			out.bTranslucent = md.translucent != 0;
			out.bAlphaFromAlbedo = md.alpha_from_albedo != 0;
			out.bNsm         = md.normal_is_nsm != 0;
			out.bTerrainDecalReceiver = md.terrain_decal_receiver != 0;
			out.BaseColor = FLinearColor(md.base_color[0], md.base_color[1],
			                             md.base_color[2], 1.f);
			out.Roughness = md.roughness;
			bool bCarPaint = false;
			for (int32 b = 0; b < md.shader_texture_count; ++b)
				bCarPaint |= md.shader_textures[b].name32 == 0xA11011B8u;
			for (int32 b = 0; b < md.texture_count; ++b)
				if (md.textures[b].slot == BF6_TEX_ALBEDO) bCarPaint = false;
			// Eye sheets live outside the generic albedo/normal vocabulary.
			// Preserve their authored parameter identities in dedicated renderer slots.
			for (int32 b = 0; b < md.shader_texture_count; ++b)
			{
				const bf6_shader_tex_binding& T = md.shader_textures[b];
				int32 Slot = -1;
				switch (T.name32)
				{
				case 0x0370914Eu: Slot = 10; break; // iris colour
				case 0x1419F025u: Slot = 11; break; // sclera colour
				case 0x71828BD1u: Slot = 12; break; // iris normal
				case 0xEC2DE079u: Slot = 13; break; // sclera normal
				case 0xE15F84CEu: Slot = 14; break; // iris coverage distance field
				case 0x54BBCD22u:
				{
					const char* Name = GTextureNameAt ? GTextureNameAt(Ctx,T.texture) : nullptr;
					// The paint family uses a separate vinyl overlay. Default sheets
					// mean no wrap; the native reader already selects its authored UV.
					if (bCarPaint && Name && !FCStringAnsi::Strstr(Name,"/textures/default/")
						&& !FCStringAnsi::Strstr(Name,"/textures/debug/")) Slot = 17;
					break;
				}
				case 0x48B69B15u:
				{
					// Backdrop cards have their own colour/coverage sheet. The
					// verified rising-plume sheet packs three temporal samples.
					const char* Name = GTextureNameAt ? GTextureNameAt(Ctx,T.texture) : nullptr;
					Slot = Name && FCStringAnsi::Strstr(Name,"t_bd_smokeplume_05_rising_unpremult") ? 16 : 15;
					out.bTranslucent = true;
					break;
				}
				default: break;
				}
				if (Slot >= 0 && T.texture >= 0) out.Textures.Add({Slot,T.texture});
			}
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

bool FCore::ReadWaterHeightfield(const FString& Level, FTerrain& Out)
{
	Out.Heights.Reset();
	Out.Size = 0;
	if (!Ctx || !GWaterHeightfield)
	{
		Error = TEXT("bf6_core.dll has no block-2 water-height read path");
		return false;
	}

	bf6_terrain* t = GWaterHeightfield(Ctx, TCHAR_TO_UTF8(*Level));
	if (!t || t->width <= 0 || !t->heights)
	{
		Error = TEXT("no validated block-2 water heightfield for this level");
		return false;
	}
	Out.Size = t->width;
	Out.Heights.SetNumUninitialized(t->width * t->height);
	FMemory::Memcpy(Out.Heights.GetData(), t->heights,
		sizeof(uint16) * Out.Heights.Num());
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
	Out.PresetPath = UTF8_TO_TCHAR(v.preset_path);
	Out.PresetCandidates = v.preset_candidates;
	Out.FieldsFound = v.fields_found;
	Out.FieldsExpected = v.fields_expected;
	Out.SunBearingDeg = v.sun_rotation_x;
	Out.SunElevationDeg = v.sun_rotation_y;
	Out.SunColor = FLinearColor(v.sun_color[0], v.sun_color[1], v.sun_color[2]);
	Out.SunIntensityLux = v.sun_intensity;
	Out.SunAngularRadiusDeg = v.sun_angular_radius;
	Out.SunShadowViewDistanceM = v.sun_shadow_view_distance;
	Out.CloudShadowSizeM = v.cloud_shadow_size;
	Out.CloudShadowCoverage = v.cloud_shadow_coverage;
	Out.CloudShadowExponent = v.cloud_shadow_exponent;
	Out.CloudShadowSpeed = FVector2f(v.cloud_shadow_speed[0], v.cloud_shadow_speed[1]);
	Out.CloudShadowTranslation = FVector2f(v.cloud_shadow_translation[0], v.cloud_shadow_translation[1]);
	Out.SkyLuminanceScale = v.sky_luminance_scale;
	Out.SkyPanoramicTileFactor = v.sky_panoramic_tile_factor;
	if (v.sky_cloud_extension_version >= 1)
	{
		Out.SkyPanoramicUVMin = FVector2f(v.sky_panoramic_uv_min[0], v.sky_panoramic_uv_min[1]);
		Out.SkyPanoramicUVMax = FVector2f(v.sky_panoramic_uv_max[0], v.sky_panoramic_uv_max[1]);
		Out.SkyFlowDistance = v.sky_flow_distance;
		Out.SkyFlowDirectionDeg = v.sky_flow_direction;
		Out.SkyFlowPeriodSeconds = v.sky_flow_period;
		Out.SkyFlowHeightMaskScale = v.sky_flow_height_mask_scale;
		Out.SkyFlowHeightMaskBias = v.sky_flow_height_mask_bias;
		Out.SecondaryCloudShadowSizeM = v.secondary_cloud_shadow_size;
		Out.SecondaryCloudShadowCoverage = v.secondary_cloud_shadow_coverage;
		Out.SecondaryCloudShadowExponent = v.secondary_cloud_shadow_exponent;
		Out.SecondaryCloudShadowSpeed = FVector2f(v.secondary_cloud_shadow_speed[0], v.secondary_cloud_shadow_speed[1]);
		Out.SecondaryCloudShadowTranslation = FVector2f(v.secondary_cloud_shadow_translation[0], v.secondary_cloud_shadow_translation[1]);
		Out.CloudShadowAddressingMode = v.cloud_shadow_addressing_mode;
		Out.SecondaryCloudShadowAddressingMode = v.secondary_cloud_shadow_addressing_mode;
		Out.bCloudShadowTopDown = v.cloud_shadow_is_top_down != 0;
		Out.bSecondaryCloudShadowTopDown = v.secondary_cloud_shadow_is_top_down != 0;
		Out.CloudShadowStartFadeM = v.cloud_shadow_start_fade;
		Out.CloudShadowFadeDistanceM = v.cloud_shadows_fade_distance;
		Out.bCloudShadowHeightFade = v.cloud_shadow_height_fade_enable != 0;
		Out.CloudShadowStartHeightFadeM = v.cloud_shadow_start_height_fade;
		Out.CloudShadowHeightFadeDistanceM = v.cloud_shadows_height_fade_distance;
		Out.SecondaryCloudShadowTexture = v.secondary_cloud_shadow_texture;
		Out.PanoramaAlphaTexture = v.panorama_alpha_texture;
		Out.CloudLayer1Texture = v.cloud_layer1_texture;
	}
	Out.bAutoExposure = v.auto_exposure != 0;
	Out.ExposureEV = v.ev;
	Out.ExposureEVMax = v.ev_max;
	Out.ExposureCompensation = v.exposure_compensation;
	Out.SkyPanoramicRotationTurns = v.sky_panoramic_rotation;
	Out.PanoramaTexture = v.has_panorama ? v.panorama_texture : -1;
	Out.FlowMaskTexture = v.sky_cloud_extension_version >= 1 && v.flow_mask_texture >= 0
		? v.flow_mask_texture
		: TextureIdByName(UTF8_TO_TCHAR(v.flow_mask_res));
	Out.CloudShadowTexture = v.cloud_shadow_texture >= 0
		? v.cloud_shadow_texture
		: TextureIdByName(UTF8_TO_TCHAR(v.cloud_shadow_res));
	Out.GradingLutTexture = v.sky_cloud_extension_version >= 1 && v.grading_lut_texture >= 0
		? v.grading_lut_texture
		: TextureIdByName(UTF8_TO_TCHAR(v.grading_lut_res));
	Out.SkyTypeValue = v.sky_type;
	Out.Rayleigh = FLinearColor(v.rayleigh[0], v.rayleigh[1], v.rayleigh[2]);
	Out.MieCoefficient = v.mie_coefficient;
	Out.MieG = v.mie_g;
	Out.bUseAerialPerspective = v.use_aerial_perspective != 0;
	Out.AerialPerspectiveScale = v.aerial_perspective_scale;
	Out.AerialPerspectiveIntensity = v.aerial_perspective_intensity;
	Out.CloudLayerAltitudeM = v.cloud1_altitude;
	Out.CloudLayerTileFactor = v.cloud1_tile_factor;
	Out.CloudLayerRotationDeg = v.cloud1_rotation;
	Out.CloudLayerSpeed = v.cloud1_speed;
	Out.CloudLayerAlpha = v.cloud1_alpha_mul;
	Out.bHasSun = (v.components & BF6_VE_SUN) != 0;
	Out.bHasSky = (v.components & BF6_VE_SKY) != 0;
	Out.bHasFog = (v.components & BF6_VE_FOG) != 0;
	Out.bHeightFogEnable = v.fog_height_enable != 0;
	Out.bFogColorEnable = v.fog_color_enable != 0;
	Out.bFogGradientEnable = v.fog_gradient_enable != 0;
	Out.bVolumetricsEnable = v.volumetrics_enable != 0;
	Out.FogColor = FLinearColor(v.fog_color[0], v.fog_color[1], v.fog_color[2]);
	Out.FogDistanceStartM = v.fog_dist_start;
	Out.FogDistanceEndM = v.fog_dist_end;
	Out.FogHeightStartM = v.fog_height_start;
	Out.FogHeightEndM = v.fog_height_end;
	Out.FogAltitude = v.fog_altitude;
	Out.FogDepth = v.fog_depth;
	Out.FogVisibilityRange = v.fog_visibility_range;
	Out.SunScatterIntensity = v.sun_scatter_intensity;
	Out.LocalLightScatterIntensity = v.local_light_scatter_intensity;
	Out.bGradingEnable = v.grading_enable != 0;
	Out.GradeBrightness = FLinearColor(v.grade_brightness[0], v.grade_brightness[1], v.grade_brightness[2]);
	Out.GradeContrast = FLinearColor(v.grade_contrast[0], v.grade_contrast[1], v.grade_contrast[2]);
	Out.GradeSaturation = FLinearColor(v.grade_saturation[0], v.grade_saturation[1], v.grade_saturation[2]);
	Out.GradeHueDeg = v.grade_hue;
	Out.WhiteTemperatureK = v.white_temperature;
	Out.WhiteTint = v.white_tint;
	Out.bAoAffectsOutdoorLight = v.ao_affects_outdoor_light != 0;
	Out.bAoAffectsLocalLight = v.ao_affects_local_light != 0;
	Out.HbaoRadius = v.hbao_radius;
	Out.HbaoContrast = v.hbao_contrast;
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
		F.Source = L.source ? UTF8_TO_TCHAR(L.source) : TEXT("");
		F.Flags = L.flags;
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
	Out.SlotCount = g.slot_count > 0 ? g.slot_count : 4;
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
		M.Coverage = m.coverage_res ? UTF8_TO_TCHAR(m.coverage_res) : TEXT("");
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

// Sea-state override. See the comment on FCore::SetWindOverride.
//
// THE DEFAULT IS NOT THE LEVEL'S AUTHORED WIND, and that is deliberate.
//
// A level authors a BASE sea. The state actually rendered is chosen at runtime
// from a Beaufort number mapped through seven FloatCurves on a
// WaterOceanBeaufortMappingEntityData. No shipped level authors one of those,
// the executable carries no default table for it, and the script API that would
// read the number does not work in game - so the value the game runs cannot be
// recovered from the files. Using the authored base instead gives mp_isolated a
// millimetre sea, which is not what the map looks like in play.
//
// So the default is the one sea state the game DOES state outright: the
// constants its own physics ocean is built from (waterOceanCreatePhysicsSim,
// _DAT_1491663a0 and the immediate 0.1), which read as wind 15 and min
// wavelength 0. That is a number taken from the image, not chosen by eye.
//
// It is still a CALIBRATION, not the visual sim's authored value. Anyone who
// wants what the level literally ships runs BF6.HighPoly.WaterWindReset and
// BF6.HighPoly.WaterMinWavelength -1.
// The sea state is READ FROM THE GAME, never baked here.
//
// This was a hardcoded 13.9, which was the right number for mp_isolated and
// the wrong kind of thing to write down: it is one level's value, frozen into
// our source, obtained by interpolating the game's curve by hand.
//
// FCore::ApplyLevelSeaState now asks libbf6 for it at level load.
// -1 means "not set", which falls back to the level's authored per-cascade
// wind - the honest answer for a level that authors no Beaufort mapping, and
// most of them do not.
static float GWindAbsolute = -1.f;
static float GWindScale = 1.f;

void FCore::SetWindOverride(float AbsoluteMps, float Scale)
{
	GWindAbsolute = AbsoluteMps;
	GWindScale = Scale > 0.f ? Scale : 1.f;
}

// The sea state's curves, as read at level open. -1 means "not read".
//
// WHICH CURVE IS AMPLITUDE, and why slot 4. The collection has ten curves and
// only the wind one is named by its values. Slot 4 is identified by a
// CROSS-CHECK against a second, independent game source: the physics ocean
// (waterOceanCreatePhysicsSim) builds its parameter array from constants in
// .rdata, [8.0, 1.0, 0.5, 0.5, 10.0, 15.0, 0.0, 1.0, 0.1], whose index 5 is
// wind 15 and index 8 is amplitude 0.1. Wind 15 is Beaufort 7 - the wind curve
// reads 15.5 there - so evaluating the collection at force 7 should reproduce
// that array, and it does in four places: the constant-8 curve against the
// array's 8.0, the wind curve against 15.0, the two constant-0 curves against
// the array's 0.0 min wavelength, and slot 4's ~0.1125 against the array's 0.1
// amplitude.
//
// That is an identification by agreement between two game sources, NOT by
// field name. It can be wrong. What it replaces was worse: the authored 0.010
// measures a 0.44 m sea at this level's own wind, and the 0.25 that preceded
// it was a number picked by eye.
static float GSeaCurve[16];
static int32 GSeaCurveCount = 0;
// AMPLITUDE: slot 7, chosen on wave height rather than on the physics array.
//
// Slot 4 was the earlier choice, on a cross-check that still stands: the
// physics ocean's constant array is the curve collection evaluated at force 7,
// and its amplitude index 0.1 matches slot 4's ~0.1125 there. But the PHYSICS
// sim is a separate simulation with its own needs, so its amplitude is not
// evidence about the VISUAL one, and with the authored min wavelengths back in
// place slot 4 gives a 1.41 m sea at Beaufort 6.5.
//
// The independent constraint is the meteorological table - the same source
// that identified the wind curve, so this is the existing method rather than a
// new assumption. Beaufort 6 to 7 is a 3 to 4 m significant wave height. With
// the authored min wavelengths kept and wind 13.9 (scratchpad/amp_slot):
//
//   slot  value    combined Hs   worst crest steepness
//     6   0.0086      0.41 m       1.6%
//     4   0.1039      1.41 m       5.5%
//     9   0.2025      1.97 m       7.7%
//     8   0.2100      2.01 m       7.8%
//     7   0.4250      2.86 m      11.1%
//     5   1.7500      5.80 m      22.6%  breaks
//     2   8.0000     12.40 m      48.3%  breaks
//
// Slot 7 is the only candidate that reaches the meteorological height without
// pushing a cascade past breaking. Slightly under the table's ~3.5 m at force
// 6.5, which is the honest residual.
//
// HONESTY: this is chosen by matching a physical expectation, not by a field
// name or a second game source. It is weaker evidence than the wind curve,
// which reproduces the Beaufort table across all thirteen forces.
constexpr int32 kAmplitudeCurveSlot = 7;

// MIN WAVELENGTH IS AUTHORED, and driving it from the curve was WRONG.
//
// It was briefly forced to the curve's 0 on every cascade, which doubled the
// sea and revived the mid band. The executable's class default for
// MinWavelength is 1.0, and mp_isolated authors 0.26, 6.00, 6.00, 1.00 - so
// cascades 0, 1 and 2 are AUTHORED and only cascade 3 sits at the default.
// The 6.00 pair is a deliberate "hold this band back", not an oversight, and
// overriding it is not reading the game, it is disagreeing with it.
//
// What that override cost, measured (scratchpad/minwl_split), at wind 13.9
// with crest steepness against the 14.3% breaking limit:
//
//   min wavelength   combined Hs   worst crest
//   authored           1.41 m        5.5%
//   all forced to 0    3.05 m       28.6%  cascade 3 AND cascade 1 breaking
//
// The height came at the price of two cascades folding past breaking, which
// is what read as sharp pointy small waves inside the big ones.

float FCore::WindOverrideAbsolute() { return GWindAbsolute; }

// Read this level's sea state from the game and adopt it as the wind.
//
// THE CHAIN, all of it authored data: the level's schematic carries a
// WaterOceanBeaufortMappingEntityData whose scalar is the Beaufort force;
// common/artsetup/oceancurves/fcc_ocean holds ten curves keyed by that force;
// the wind curve is the one reaching 40 m/s where the others top out at 8, and
// its keys are the meteorological Beaufort table. libbf6 evaluates it with the
// authored Hermite tangents.
//
// TWO HONEST LIMITS, both inherited from the data rather than introduced here.
// The force field is identified by POSITION - the lone scalar on a Beaufort
// mapping entity, inside the curves' 0..12 domain - not by name, and the
// cross-level control could not run because only mp_isolated authors a
// mapping. And this applies ONE wind to every cascade, discarding the
// relationship the level authored between them (0.914, 0.010, 1.000, 1.000);
// whether the force sets a single wind or scales the authored ones is not
// established. WaterWindScale remains the way to keep that relationship.
bool FCore::ApplyLevelSeaState(const FString& Level)
{
	if (!Ctx || !GSeaState) return false;
	bf6_ocean_sea_state St;
	FMemory::Memzero(St);
	const FTCHARToUTF8 LevelUtf8(*Level);
	if (GSeaState(Ctx, (const char*)LevelUtf8.Get(), &St) == 0 || !St.found)
	{
		UE_LOG(LogBF6HPCore, Display,
			TEXT("water: %s authors no Beaufort mapping; keeping the authored wind"),
			*Level);
		return false;
	}
	if (St.wind_curve_index < 0 || !(St.wind_mps > 0.f))
	{
		UE_LOG(LogBF6HPCore, Warning,
			TEXT("water: %s has Beaufort force %.3f but no wind curve was identified; "
			     "keeping the authored wind"), *Level, St.force);
		return false;
	}
	GWindAbsolute = St.wind_mps;
	GSeaCurveCount = FMath::Clamp(St.curve_count, 0, 16);
	for (int32 i = 0; i < GSeaCurveCount; ++i) GSeaCurve[i] = St.curve_value[i];
	UE_LOG(LogBF6HPCore, Display,
		TEXT("water: %s sea state read from the game - Beaufort %.3f -> wind %.3f m/s "
		     "(curve %d of %d)"),
		*Level, St.force, St.wind_mps, St.wind_curve_index, St.curve_count)
	if (GSeaCurveCount > kAmplitudeCurveSlot)
	{
		UE_LOG(LogBF6HPCore, Display,
			TEXT("water: wave amplitude %.4f from Beaufort curve slot %d "
			     "(slot chosen because it is the only candidate reaching the "
			     "meteorological wave height for this force without a cascade "
			     "passing the breaking limit; weaker evidence than the wind curve)"),
			GSeaCurve[kAmplitudeCurveSlot], kAmplitudeCurveSlot);
	}
	return true;
}
float FCore::WindOverrideScale()    { return GWindScale; }

float FCore::EffectiveWind(float AuthoredWind)
{
	if (GWindAbsolute >= 0.f) return GWindAbsolute;
	return AuthoredWind * GWindScale;
}

// Default is the sea state's value; see kMinWavelengthCurveSlot.
static float GMinWavelength[4] = { -1.f, -1.f, -1.f, -1.f };

void FCore::SetMinWavelengthOverride(int32 Cascade, float Metres)
{
	if (Cascade < 0) { for (float& M : GMinWavelength) M = Metres; return; }
	if (Cascade < 4) GMinWavelength[Cascade] = Metres;
}

float FCore::MinWavelengthOverride(int32 Cascade)
{
	return (Cascade >= 0 && Cascade < 4) ? GMinWavelength[Cascade] : -1.f;
}

float FCore::EffectiveMinWavelength(int32 Cascade, float AuthoredMinWavelength)
{
	if (Cascade < 0 || Cascade >= 4) return AuthoredMinWavelength;
	// The console override still wins; otherwise the level's own value stands.
	return GMinWavelength[Cascade] >= 0.f ? GMinWavelength[Cascade] : AuthoredMinWavelength;
}

// Wave height, per cascade. NO OVERRIDE: height comes from the wind, as it
// does in the game.
//
// This used to be 0.25 on cascade 0 and authored on the rest, from when the
// wind was the authored 0.914 and the sea was millimetres. That workaround is
// now actively harmful, because cascade 0 is the most DIRECTIONAL cascade
// there is: binning |H0(k)|^2 by the direction of k, with the game's own H0
// builder on this level's data at wind 13.9, puts 98.2% of cascade 0's energy
// inside a single 45-degree wedge (isotropic control: 29.1%). A sea with all
// its height in one direction is a corrugation - long parallel ridges that
// look like waves across the ridges and FLAT along them, which is exactly the
// reported "correct along one axis, flat everywhere else".
//
// The four cascades peak in four different directions at wind 13.9 - 120, 30,
// 45 and 135 degrees - so the sea is only two-dimensional when all four
// contribute at their authored relative strengths. Forcing one cascade's
// amplitude discards the other three directions.
//
// Measured by scratchpad h0_aniso, whose binning passes both an isotropic
// control (flat) and a synthetic 1-D control (spike). H0 itself is verified
// elementwise against the shipped machine code by
// ocean-h0-machine-code-oracle-verifies-reconstruction, so the directional
// shape is the game's, not an artefact of our reconstruction.
static float GAmplitude[4] = { -1.f, -1.f, -1.f, -1.f };

void FCore::SetAmplitudeOverride(int32 Cascade, float Amplitude)
{
	if (Cascade < 0)
	{
		for (float& A : GAmplitude) A = Amplitude;
		return;
	}
	if (Cascade < 4) GAmplitude[Cascade] = Amplitude;
}

float FCore::AmplitudeOverride(int32 Cascade)
{
	return (Cascade >= 0 && Cascade < 4) ? GAmplitude[Cascade] : -1.f;
}

float FCore::EffectiveAmplitude(int32 Cascade, float AuthoredAmplitude)
{
	if (Cascade < 0 || Cascade >= 4) return AuthoredAmplitude;
	// An explicit override still wins, so the console commands keep working.
	if (GAmplitude[Cascade] >= 0.f) return GAmplitude[Cascade];
	// Then the sea state, which is the game's own answer for this level.
	if (GSeaCurveCount > kAmplitudeCurveSlot
		&& GSeaCurve[kAmplitudeCurveSlot] > 0.f)
		return GSeaCurve[kAmplitudeCurveSlot];
	return AuthoredAmplitude;
}

static bool ReadWaterCascadesThrough(bf6_ctx* Ctx, FnWaterSims Sims,
	FnWaterH0 WaterH0, const FString& Level,
	TArray<FCore::FWaterCascade>& Out, FString& Error)
{
	Out.Reset();
	Error.Reset();
	if (!Ctx || !Sims || !WaterH0)
	{
		Error = TEXT("bf6_core.dll has no complete ocean FFT read path");
		return false;
	}
	const int32 Count = Sims(Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0);
	if (Count <= 0)
	{
		Error = TEXT("level has no enabled ocean simulation cascades");
		return false;
	}
	TArray<bf6_water_sim_v2> Raw;
	Raw.SetNumZeroed(Count);
	const int32 Got = Sims(Ctx, TCHAR_TO_UTF8(*Level), Raw.GetData(), Count);
	if (Got <= 0) return false;
	Out.Reserve(Got);
	for (int32 i = 0; i < Got; ++i)
	{
		// Apply the sea state to a COPY of the authored row, then let the
		// game's own builder run on it. Changing the input and rebuilding is
		// what makes this faithful; scaling the resulting displacement would
		// be inventing amplitude and would leave the normals, foam and the
		// cascade merge describing a sea that is not the one being drawn.
		bf6_water_sim_v2 S = Raw[i];
		const float Authored = S.wind_speed;
		const float AuthoredMinWL = S.min_wavelength;
		const float AuthoredAmp = S.wave_amplitude;
		S.wind_speed = FCore::EffectiveWind(Authored);
		S.min_wavelength = FCore::EffectiveMinWavelength(i, AuthoredMinWL);
		S.wave_amplitude = FCore::EffectiveAmplitude(i, AuthoredAmp);
		if (S.wind_speed != Authored || S.min_wavelength != AuthoredMinWL ||
			S.wave_amplitude != AuthoredAmp)
		{
			UE_LOG(LogBF6HPCore, Display,
				TEXT("water cascade %d (tile %.1f m): wind %.3f -> %.3f, min wavelength "
				     "%.2f -> %.2f m, amplitude %.4f -> %.4f, spectrum rebuilt"),
				i, S.tile_dimension, Authored, S.wind_speed,
				AuthoredMinWL, S.min_wavelength, AuthoredAmp, S.wave_amplitude);
		}
		const int32 FloatCount = WaterH0(&S, nullptr, 0);
		if (FloatCount != S.resolution * S.resolution * 2)
		{
			UE_LOG(LogBF6HPCore, Error,
				TEXT("water cascade %d rejected: H0 wants %d floats for %dx%d"),
				i, FloatCount, S.resolution, S.resolution);
			continue;
		}
		FCore::FWaterCascade C;
		C.SourceIndex = S.source_index;
		C.Resolution = S.resolution;
		C.WindAngleDegrees = S.wind_angle_degrees;
		C.WindSpeed = S.wind_speed;
		C.Choppiness = S.choppiness;
		C.TileDimension = S.tile_dimension;
		C.MinWavelength = S.min_wavelength;
		C.LargeWaveReduction = S.large_wave_reduction;
		C.WaveAmplitude = S.wave_amplitude;
		C.WaveThickness = S.wave_thickness;
		C.bFoamEnabled = S.foam_enable != 0;
		C.FoamThreshold = S.foam_threshold;
		C.FoamMax = S.foam_max;
		C.FoamHalfLife = S.foam_half_life;
		C.bPhysicsSimulation = S.physics_simulation_enabled != 0;
		C.bForceSimplePlaneCollision = S.force_simple_plane_collision != 0;
		C.bVisualCpuSimulation = S.visual_cpu_simulation_enabled != 0;
		C.H0.SetNumUninitialized(S.resolution * S.resolution);
		if (WaterH0(&S, reinterpret_cast<float*>(C.H0.GetData()), FloatCount) != FloatCount)
		{
			UE_LOG(LogBF6HPCore, Error, TEXT("water cascade %d H0 build failed"), i);
			continue;
		}
		Out.Add(MoveTemp(C));
	}
	return !Out.IsEmpty();
}

bool FCore::ReadWaterCascades(const FString& Level, TArray<FWaterCascade>& Out)
{
	return ReadWaterCascadesThrough(Ctx, GWaterSims, GWaterH0, Level, Out, Error);
}

bool FCore::ReadWaterCascadesIsolated(const FString& Level, TArray<FWaterCascade>& Out)
{
	if (!GWaterSimsIsolated)
	{
		Out.Reset();
		Error = TEXT("bf6_core.dll has no isolated water-schematic read path");
		return false;
	}
	return ReadWaterCascadesThrough(
		Ctx, GWaterSimsIsolated, GWaterH0, Level, Out, Error);
}

bool FCore::ReadRawResource(const FString& ResourceName, TArray<uint8>& Out)
{
	Out.Reset();
	Error.Reset();
	if (!Ctx || !GReadRaw)
	{
		Error = TEXT("bf6_core.dll has no direct raw-resource read path");
		return false;
	}
	if (ResourceName.IsEmpty())
	{
		Error = TEXT("raw resource name is empty");
		return false;
	}
	const uint8_t* Data = nullptr;
	const int64_t Bytes = GReadRaw(Ctx, BF6_RAW_RES,
		TCHAR_TO_UTF8(*ResourceName), &Data);
	if (Bytes <= 0 || !Data || Bytes > MAX_int32)
	{
		Error = FString::Printf(TEXT("raw resource not found: %s"), *ResourceName);
		return false;
	}
	Out.Append(Data, static_cast<int32>(Bytes));
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
				Out[i].ReflectanceLow = s.reflectance_low;
				Out[i].OceanComponentVersion = s.ocean_component_version;
				Out[i].OceanPreset = UTF8_TO_TCHAR(s.ocean_preset);
				Out[i].OceanPresetCandidates = s.ocean_preset_candidates;
				Out[i].bOceanEnabled = s.ocean_enable > 0;
				Out[i].bSimplifiedDistortion = s.simplified_distortion > 0;
				Out[i].bFoamEnabled = s.foam_enable > 0;
				Out[i].CompositeIor = s.composite_ior;
				Out[i].OpacityRampM = s.opacity_ramp_m;
				Out[i].FoamDepthRampM = s.foam_depth_ramp_m;
				Out[i].ScatterPhaseG = s.scatter_phase_g;
				Out[i].TransmissionColour = FLinearColor(
					s.transmission_colour[0], s.transmission_colour[1],
					s.transmission_colour[2]);
				Out[i].ScatterShadowInfluence = s.scatter_shadow_influence;
				Out[i].CompositeFoamTint = FLinearColor(
					s.foam_tint[0], s.foam_tint[1], s.foam_tint[2]);
				Out[i].CompositeFoamSmoothness = s.foam_smoothness;
				Out[i].CompositeFoamRoughness = s.foam_roughness;
				Out[i].AuthoredOceanAlbedo = FLinearColor(
					s.authored_ocean_albedo[0], s.authored_ocean_albedo[1],
					s.authored_ocean_albedo[2]);
				Out[i].AuthoredAlbedoDistanceM = s.authored_albedo_distance_m;
				Out[i].AttenuationType = s.attenuation_type;
				Out[i].bRiver = s.is_river != 0;
				Out[i].bShoreFadeValid = s.shore_fade_valid != 0;
				Out[i].ShoreDepthM = s.shore_depth_m;
				Out[i].ShoreBlend = FVector4f(
					s.shore_blend[0], s.shore_blend[1],
					s.shore_blend[2], s.shore_blend[3]);
				Out[i].AdditionalWaterDepthM = s.additional_water_depth_m;
				Out[i].WaveAmplitudeScale = s.wave_amplitude_scale;
				Out[i].DetailFadeStartM = s.detail_fade_start_m;
				Out[i].DetailFadeEndM = s.detail_fade_end_m;
				Out[i].DrawFoamThreshold = s.foam_threshold;
				Out[i].ShoreFoamSuppression = s.shore_foam_suppression;
				Out[i].FoamContrast = s.foam_contrast_divisor;
				for (int32 k = 0; k < 4; ++k)
					Out[i].CascadeFoamWeight[k] = s.cascade_foam_weight[k];
				Out[i].SmoothnessZeroFoam = s.smoothness_zero_foam;
				Out[i].SmoothnessFullFoam = s.smoothness_full_foam;
				Out[i].SmoothnessBias = s.smoothness_bias;
				Out[i].SmoothnessNearMultiplier = s.smoothness_near_multiplier;
				Out[i].FoamNormalStrength = s.foam_sheet_normal_strength;
				Out[i].MicroNormalStrength = s.micro_sheet_normal_strength;
				Out[i].NoiseUvScale = s.noise_uv_scale;
				Out[i].MicroSheetUvScale = s.micro_sheet_uv_scale;
				Out[i].FoamSheetUvScale = s.foam_sheet_uv_scale;
				Out[i].MicroSheetFlowSpeed = s.micro_sheet_flow_speed;
				Out[i].FoamCompositeLow = s.foam_composite_low;
				Out[i].FoamCompositeHigh = s.foam_composite_high;
				Out[i].ContactWorldDivisorM = s.contact_world_divisor_m;
				Out[i].ContactRemapLow = s.contact_remap_low;
				Out[i].ContactGain = s.contact_gain;
				Out[i].BroadPatternWorldMul = s.broad_pattern_world_mul;
				Out[i].BroadPatternWorldScale = s.broad_pattern_world_scale;
				Out[i].BroadPatternFloor = s.broad_pattern_floor;
				Out[i].ExtendedGraphVersion = s.extended_graph_version;
				Out[i].CascadeOverlapVersion = s.cascade_overlap_version;
				Out[i].bCascadeOverlapEnabled = s.cascade_overlap_enabled > 0;
				Out[i].CascadeOverlapParams = FVector4f(
					s.cascade_overlap_params[0], s.cascade_overlap_params[1],
					s.cascade_overlap_params[2], s.cascade_overlap_params[3]);
				Out[i].CascadeOverlapParams2 = FVector4f(
					s.cascade_overlap_params2[0], s.cascade_overlap_params2[1],
					s.cascade_overlap_params2[2], s.cascade_overlap_params2[3]);
				Out[i].CascadeOverlapHeightScale = s.cascade_overlap_height_scale;
				for (int32 r = 0; r < 22; ++r)
					Out[i].ExtendedCb1[r] = FVector4f(
						s.extended_cb1[r][0], s.extended_cb1[r][1],
						s.extended_cb1[r][2], s.extended_cb1[r][3]);
				Out[i].DetailNormal = s.detail_normal;
				Out[i].FoamNormal = s.foam_normal;
				Out[i].FoamRgb = s.foam_rgb;
				Out[i].Noise = s.noise;
				Out[i].Perlin = s.perlin;
				Out[i].ContactFoam = s.contact_foam;
				Out[i].FoamRgb2 = s.foam_rgb2;
			}
		}
	}
	return Out.Num() > 0;
}

bool FCore::ReadWaterMask(const FString& Level, FWaterMask& Out)
{
	Out = FWaterMask();
	// Function pointers in this wrapper are historically process-global, while
	// the persistent lab deliberately owns a separately named runtime core.
	// Always resolve this optional API from THIS context's DLL: calling an export
	// from a sibling image with this context is an ABI-valid but wrong pairing.
	FnWaterMask WaterMask = Dll
		? (FnWaterMask)FPlatformProcess::GetDllExport(
			Dll, TEXT("bf6_level_water_mask"))
		: nullptr;
	if (!Ctx || !WaterMask)
	{
		Error = TEXT("this core has no exact water utility-raster API");
		return false;
	}
	char Err[512] = {};
	bf6_water_mask Raw{};
	if (!WaterMask(Ctx, TCHAR_TO_UTF8(*Level), &Raw, Err, sizeof(Err)) ||
		Raw.version != 1 || !Raw.atlas_r8 || !Raw.indirection_u32)
	{
		Error = UTF8_TO_TCHAR(Err);
		return false;
	}
	const int64 AtlasBytes = (int64)Raw.page_count * Raw.tile_side * Raw.tile_side;
	const int64 IndirectionCount = (int64)Raw.indirection_side * Raw.indirection_side;
	if (AtlasBytes <= 0 || AtlasBytes > MAX_int32 ||
		IndirectionCount <= 0 || IndirectionCount > MAX_int32)
	{
		Error = TEXT("water utility-raster dimensions are invalid");
		return false;
	}
	Out.Version = Raw.version;
	Out.TileSide = (int32)Raw.tile_side;
	Out.InteriorSide = (int32)Raw.interior_side;
	Out.Border = (int32)Raw.border;
	Out.PageCount = (int32)Raw.page_count;
	Out.IndirectionSide = (int32)Raw.indirection_side;
	Out.BoundsMin = FVector2D(Raw.bounds_min[0], Raw.bounds_min[1]);
	Out.BoundsMax = FVector2D(Raw.bounds_max[0], Raw.bounds_max[1]);
	Out.CoverageSideRcp = Raw.coverage_side_rcp;
	Out.BorderFraction = Raw.border_fraction;
	Out.AtlasR8.Append(Raw.atlas_r8, (int32)AtlasBytes);
	Out.Indirection.Append(Raw.indirection_u32, (int32)IndirectionCount);
	return true;
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

bool FCore::ReadScatter(const FString& Level, TArray<FScatter>& Out)
{
	Out.Reset();
	if (!Ctx || !GScatter) { Error = TEXT("this core has no scatter catalogue API"); return false; }
	char err[512] = {0};
	const int32 Count = GScatter(Ctx, TCHAR_TO_UTF8(*Level), nullptr, 0,
	                             err, sizeof(err));
	if (Count < 0) { Error = UTF8_TO_TCHAR(err); return false; }
	if (Count == 0) return true;
	TArray<bf6_scatter_entry> Rows;
	Rows.SetNumZeroed(Count);
	const int32 Got = GScatter(Ctx, TCHAR_TO_UTF8(*Level), Rows.GetData(), Count,
	                           err, sizeof(err));
	if (Got != Count) { Error = UTF8_TO_TCHAR(err); return false; }
	Out.Reserve(Count);
	for (const bf6_scatter_entry& R : Rows)
	{
		if (!R.mesh_res || !*R.mesh_res) continue;
		FScatter S;
		S.Name = R.name ? UTF8_TO_TCHAR(R.name) : TEXT("");
		S.MeshRes = UTF8_TO_TCHAR(R.mesh_res);
		S.ViewDistanceM = R.view_distance;
		S.DissolveRatio = R.dissolve_ratio;
		S.OpaquePointCount = R.point_count;
		Out.Add(MoveTemp(S));
	}
	return true;
}

bool FCore::TextureAt(int32 Id, FTexture& Out, int32 MaxDim)
{
	Out = FTexture();
	if (!Ctx || !GTextureAt || Id < 0) return false;
	const bf6_texture* t = MaxDim > 0 && GTextureAtMaxDim
		? GTextureAtMaxDim(Ctx, Id, MaxDim)
		: GTextureAt(Ctx, Id);
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

int32 FCore::TextureIdByName(const FString& ResourceName) const
{
	if (!Ctx || !GTextureIdByName || ResourceName.IsEmpty()) return -1;
	return GTextureIdByName(Ctx, TCHAR_TO_UTF8(*ResourceName));
}

FString FCore::TextureNameAt(int32 Id) const
{
	if (!Ctx || !GTextureNameAt || Id < 0) return FString();
	const char* Name = GTextureNameAt(Ctx, Id);
	return Name && *Name ? UTF8_TO_TCHAR(Name) : FString();
}

}  // namespace BF6HP
