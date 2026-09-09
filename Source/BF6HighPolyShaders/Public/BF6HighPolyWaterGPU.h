#pragma once

#include "CoreMinimal.h"

// RDG-native ocean kernels.
//
// This replaces the quarantined native-D3D12 route. That route created its own
// ID3D12 resources, heaps and pipeline states, which is why it hung the device:
// resources outside Unreal's residency and state tracking get evicted or
// transitioned underneath an in-flight dispatch. Everything here is owned by
// RDG, so the graph builder does the barriers and the validation.
//
// The kernels are the game's algorithm, not the game's bytecode. Nothing is
// lost by that - dispersion, the FFT family, merge and diff are decoded exactly
// (finding ocean-fft-kernels-decoded) - and it is the only route the engine can
// own.
//
// WHY THIS IS ITS OWN MODULE: shader types register during static init, before
// the global shader map is built, so the module declaring them must load at
// PostConfigInit. The editor module cannot.
//
// NOTHING HERE IS ALLOWED INTO THE MATERIAL until it passes the two controls
// the quarantine names: agreement with the CPU replay on real H0, and an
// all-zero result on zero H0. Verify() runs both and reports numbers.
namespace BF6HP
{
	class BF6HIGHPOLYSHADERS_API FWaterGPU
	{
	public:
		struct FCascadeInput
		{
			int32 N = 0;
			float TileM = 0.f;
			const TArray<FVector2f>* H0 = nullptr;   // [y*N + x], (real, imag)
		};

		struct FResult
		{
			bool bRan = false;
			TArray<FVector2f> Height, DispX, DispY;
			FString Error;
		};

		// Runs the dispersion kernel through RDG and reads the result back.
		// Deliberately synchronous and slow: this is a VERIFICATION path, not
		// the render path.
		static FResult DispersionReadback(const FCascadeInput& In, float TimeSeconds);

		// Dispersion followed by the two FFT passes: the spatial field, which is
		// what the surface would actually sample.
		static FResult DispersionFftReadback(const FCascadeInput& In, float TimeSeconds);

		// Both controls, reported as text for the log:
		//  - real H0 against the caller's CPU reference (relative L2)
		//  - zero H0, which must produce exactly zero
		static FString Verify(const FCascadeInput& In, float TimeSeconds,
			const TArray<FVector2f>& CpuHeight,
			const TArray<FVector2f>& CpuDispX,
			const TArray<FVector2f>& CpuDispY);

		// The same two controls for the whole chain. The CPU arrays here are
		// POST-transform: the spatial field, not the spectra.
		static FString VerifyFft(const FCascadeInput& In, float TimeSeconds,
			const TArray<FVector2f>& CpuHeight,
			const TArray<FVector2f>& CpuDispX,
			const TArray<FVector2f>& CpuDispY);

		// The stage after the transform: displacement assembly, foam with its
		// temporal lag, and the normal/foam pack. Lag is passed in already
		// resolved rather than as a half-life, because the replay folds the
		// frame's delta time into it and this path must not re-derive that.
		struct FSurfaceParams
		{
			bool  bFoamEnabled = false;
			float FoamThreshold = 0.f;
			float FoamMax = 0.f;
			float Lag = 0.f;
			const TArray<float>* FoamPrevious = nullptr;   // N*N, may be null
		};

		struct FSurfaceResult
		{
			bool bRan = false;
			TArray<FVector4f> Displacement;   // xyz, the geometry
			TArray<FVector4f> NormalFoam;     // foam, nx, ny, 1
			TArray<float>     FoamWork;       // pre-blur, the history value
			FString Error;
		};

		static FSurfaceResult SurfaceReadback(const FCascadeInput& In,
			float TimeSeconds, const FSurfaceParams& Params);

		// Displacement against the replay's own LastDisplacement, plus the
		// zero-H0 control. This is the stage that actually drives geometry, so
		// it is the one that decides whether the GPU path can replace the CPU.
		static FString VerifySurface(const FCascadeInput& In, float TimeSeconds,
			const FSurfaceParams& Params,
			const TArray<FVector3f>& CpuDisplacement);
	};
}
