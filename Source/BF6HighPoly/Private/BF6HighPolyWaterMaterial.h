#pragma once

#include "BF6HighPolyCore.h"

class UMaterialInstanceDynamic;
class UObject;

namespace BF6HP
{
	class FWaterFFT;
}

namespace BF6WaterShared
{
	// Select the stable Single Layer Water capture/sky reflection path used by
	// both the isolated lab and normal full-map rebuilds.
	void ApplyStableReflectionPolicy();
	// The lab regenerates its transient graph after a code/data reload so an
	// earlier graph cannot survive and make a successful Live Coding patch look
	// ineffective. Existing full-level MIDs retain their own material reference.
	void InvalidateMaterialGraph();
	// Uploads the validated raw block-2 grid. Passing null explicitly disables
	// the large surface offset; this is independent of the terrain-depth input.
	void SetWaterHeightfield(const BF6HP::FCore::FTerrain* Heightfield);
	// Rebinds the current engine-owned heightfield to an already-live water MID.
	// This lets the lab become interactive before the 8193-square raw grid has
	// finished reading, then add the shoreline data without rebuilding the view.
	void BindWaterHeightfield(UMaterialInstanceDynamic* Material);
	// Upload and bind the exact runtime block-10 CoarseMask. The isolated lab
	// calls this before material construction so it exercises the same shoreline
	// attenuation path as a full high-poly level build.
	void SetWaterMask(const BF6HP::FCore::FWaterMask* Mask);

	// The same accumulated material used by the full high-poly build, but with
	// an explicitly supplied runtime context and FFT for the isolated lab.
	UMaterialInstanceDynamic* CreateMaterial(UObject* Outer, BF6HP::FCore& Core,
		BF6HP::FWaterFFT& FFT, const BF6HP::FCore::FWater& Water);
}
