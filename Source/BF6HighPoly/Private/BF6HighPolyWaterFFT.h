#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Math/Float16Color.h"
#include "BF6HighPolyCore.h"

class UMaterialInstanceDynamic;
class UTexture2D;

namespace BF6HP
{
	// CPU replay of Frostbite's four-cascade ocean pipeline. H0 is produced by
	// libbf6 from the mounted game at build time; this class performs only the
	// decoded time evolution, inverse FFT, merge, normal, fold and foam passes.
	class FWaterFFT
	{
	public:
		struct FComplex { float R = 0.f, I = 0.f; };
		~FWaterFFT();
		bool Initialize(FCore& Core, const TArray<FCore::FWaterCascade>& Inputs);
		void Reset();
		void Tick(float DeltaSeconds);
		void Bind(UMaterialInstanceDynamic* MID, float WaveAmplitudeScale) const;
		bool IsReady() const { return Cascades.Num() > 0; }
		bool IsDirectReady() const;
		FString ProofSummary() const;
		// Peak displacement per cascade, in metres, from the last evolve. This
		// is the geometry BEFORE the material's shore and CoarseMask
		// attenuation, so comparing it against what is on screen says whether a
		// flat sea is a small spectrum or a mask eating a large one.
		FString DisplacementSummary() const;
		// Peak |vertical| per cascade in metres, so a caller can compose the
		// material's own attenuation and say what actually reaches the surface.
		void PeakVerticalPerCascade(float* Out, int32 OutMax) const;
		// The texture a material should currently be sampling for this cascade.
		// Comparing it against what is actually bound catches a rebind that
		// silently did not reach the surface being drawn.
		class UTexture2D* DisplacementTexture(int32 Index) const;
		// One cascade's H0 and geometry, for the GPU proof. H0 is the CPU
		// replay's own input, so a GPU kernel fed from here is being compared
		// against this class on identical data rather than on a re-read.
		bool GetCascadeForProof(int32 Index, int32& OutN, float& OutTileM,
			float& OutTime, TArray<FVector2f>& OutH0) const;
		// The replay's OWN transform, exposed so a GPU port is compared against
		// the exact FFT the CPU path runs rather than against a second
		// implementation written for the test.
		static void ReferenceFFT2D(TArray<FVector2f>& InOut, int32 N);
		// The replay's own post-transform geometry, for the surface proof.
		bool GetCascadeDisplacementForProof(int32 Index, TArray<FVector3f>& OutDisp,
			bool& bOutFoam, float& OutFoamThreshold, float& OutFoamMax) const;
		int32 Num() const { return Cascades.Num(); }

	private:
		friend class FWaterAsyncTest;
		struct FCascade
		{
			int32 N = 0;
			float TileM = 0.f, Choppiness = 0.f;
			bool bFoam = false;
			float FoamThreshold = 0.f, FoamMax = 0.f, FoamHalfLife = 0.f;
			TArray<FComplex> H0, Height, DispX, DispY;
			// Last CPU replay at the exact time submitted to the installed DXIL.
			// It is copied only for the one-shot proof readback, never used to
			// drive the direct rendering path.
			TArray<FVector3f> LastDisplacement;
			TArray<float> FoamPrevious, FoamWork;
			TArray<FFloat16Color> DisplacementPixels, NormalPixels;
			UTexture2D* Displacement = nullptr;
			UTexture2D* NormalFoam = nullptr;
		};

		TArray<FCascade> Cascades;
		struct FDirectState;
		TSharedPtr<FDirectState, ESPMode::ThreadSafe> Direct;
		float TimeSeconds = 0.f;
		float PendingSeconds = 0.f;
		struct FAsyncFrame;
		TFuture<TSharedPtr<FAsyncFrame, ESPMode::ThreadSafe>> PendingFrame;
		static void FFT1D(FComplex* Values, int32 Count);
		static void FFT2D(TArray<FComplex>& Values, int32 N);
		static UTexture2D* MakeTexture(int32 N, const TCHAR* Name);
		static void Upload(UTexture2D* Texture, const TArray<FFloat16Color>& Pixels, int32 N);
		static void Evolve(FCascade& C, float DeltaSeconds, float SimulationTime);
	};
}
