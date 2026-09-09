#include "BF6HighPolyWaterGPU.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogBF6WaterGPU, Log, All);

namespace
{
	class FBF6DispersionCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FBF6DispersionCS);
		SHADER_USE_PARAMETER_STRUCT(FBF6DispersionCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float2>, H0)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutHeight)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutDispX)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutDispY)
			SHADER_PARAMETER(uint32, N)
			SHADER_PARAMETER(float, TileM)
			SHADER_PARAMETER(float, Time)
			SHADER_PARAMETER(float, Pad0)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{
			return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FBF6FftCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FBF6FftCS);
		SHADER_USE_PARAMETER_STRUCT(FBF6FftCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, FftIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, FftOut)
			SHADER_PARAMETER(uint32, FftN)
			SHADER_PARAMETER(uint32, FftAxis)
			SHADER_PARAMETER(uint32, FftLogN)
			SHADER_PARAMETER(uint32, FftPad)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{
			return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
		}
	};
}

// The mapping is /BF6HighPoly -> <plugin>/Shaders, and the file lives in
// Shaders/Private, so the virtual path carries the Private segment. Getting
// this wrong is a FATAL error at shader compile time ("Couldn't find source
// file of virtual shader path"), not a warning.
IMPLEMENT_GLOBAL_SHADER(FBF6DispersionCS,
	"/BF6HighPoly/Private/BF6WaterOcean.usf", "MainDispersionCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBF6FftCS,
	"/BF6HighPoly/Private/BF6WaterOcean.usf", "MainFftCS", SF_Compute);

namespace BF6HP
{

// One implementation, two entry points. The FFT stage is optional so the
// dispersion kernel can still be proved on its own: if the full chain ever
// disagrees, being able to re-run just the first stage is what tells you which
// half moved.
static FWaterGPU::FResult RunChain(const FWaterGPU::FCascadeInput& In,
	float TimeSeconds, bool bRunFft)
{
	using FResult = FWaterGPU::FResult;
	FResult Out;
	if (In.N < 8 || !FMath::IsPowerOfTwo(In.N) || !In.H0 ||
		In.H0->Num() != In.N * In.N || In.TileM <= 0.f)
	{
		Out.Error = TEXT("invalid cascade input");
		return Out;
	}

	const int32 N = In.N;
	const int32 Count = N * N;
	TArray<FVector2f> H0Copy = *In.H0;
	const float TileM = In.TileM;

	TArray<FVector2f> Height, DispX, DispY;
	Height.SetNumZeroed(Count);
	DispX.SetNumZeroed(Count);
	DispY.SetNumZeroed(Count);
	bool bRan = false;
	FString Error;

	ENQUEUE_RENDER_COMMAND(BF6WaterGPUDispersion)(
		[N, Count, TileM, TimeSeconds, bRunFft, H0Copy, &Height, &DispX, &DispY, &bRan, &Error]
		(FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		FRDGBufferRef H0Buffer = CreateUploadBuffer(GraphBuilder,
			TEXT("BF6Water.H0"), sizeof(FVector2f), Count,
			H0Copy.GetData(), Count * sizeof(FVector2f));
		FRDGBufferSRVRef H0SRV = GraphBuilder.CreateSRV(H0Buffer, PF_G32R32F);

		const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
			FIntPoint(N, N), PF_G32R32F,
			FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);

		FRDGTextureRef TexH = GraphBuilder.CreateTexture(Desc, TEXT("BF6Water.Height"));
		FRDGTextureRef TexX = GraphBuilder.CreateTexture(Desc, TEXT("BF6Water.DispX"));
		FRDGTextureRef TexY = GraphBuilder.CreateTexture(Desc, TEXT("BF6Water.DispY"));

		FBF6DispersionCS::FParameters* Params =
			GraphBuilder.AllocParameters<FBF6DispersionCS::FParameters>();
		Params->H0 = H0SRV;
		Params->OutHeight = GraphBuilder.CreateUAV(TexH);
		Params->OutDispX = GraphBuilder.CreateUAV(TexX);
		Params->OutDispY = GraphBuilder.CreateUAV(TexY);
		Params->N = (uint32)N;
		Params->TileM = TileM;
		Params->Time = TimeSeconds;
		Params->Pad0 = 0.f;

		TShaderMapRef<FBF6DispersionCS> Shader(
			GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("BF6WaterDispersion %dx%d", N, N),
			Shader, Params,
			FComputeShaderUtils::GetGroupCount(FIntPoint(N, N), FIntPoint(8, 8)));

		if (bRunFft)
		{
			// Rows, then columns, ping-ponging through a scratch texture and
			// back, so the readback below still reads the same three textures.
			// RDG owns the transitions between these passes; that ownership is
			// the entire reason this route exists.
			const uint32 LogN = (uint32)FMath::FloorLog2((uint32)N);
			FRDGTextureRef Scratch[3] = {
				GraphBuilder.CreateTexture(Desc, TEXT("BF6Water.FftTmpH")),
				GraphBuilder.CreateTexture(Desc, TEXT("BF6Water.FftTmpX")),
				GraphBuilder.CreateTexture(Desc, TEXT("BF6Water.FftTmpY")) };
			FRDGTextureRef Field[3] = { TexH, TexX, TexY };
			TShaderMapRef<FBF6FftCS> Fft(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			for (int32 f = 0; f < 3; ++f)
			{
				for (uint32 Axis = 0; Axis < 2; ++Axis)
				{
					FBF6FftCS::FParameters* P =
						GraphBuilder.AllocParameters<FBF6FftCS::FParameters>();
					P->FftIn  = (Axis == 0) ? Field[f] : Scratch[f];
					P->FftOut = GraphBuilder.CreateUAV((Axis == 0) ? Scratch[f] : Field[f]);
					P->FftN = (uint32)N;
					P->FftAxis = Axis;
					P->FftLogN = LogN;
					P->FftPad = 0;
					// One group per line, not a 2D grid: the whole line lives in
					// groupshared memory for the duration of the transform.
					FComputeShaderUtils::AddPass(GraphBuilder,
						RDG_EVENT_NAME("BF6WaterFFT field%d axis%u", f, Axis),
						Fft, P, FIntVector(N, 1, 1));
				}
			}
		}

		// Read all three back. Synchronous by design: this is the proof path,
		// not the render path.
		FRHIGPUTextureReadback* RbH = new FRHIGPUTextureReadback(TEXT("BF6WaterRbH"));
		FRHIGPUTextureReadback* RbX = new FRHIGPUTextureReadback(TEXT("BF6WaterRbX"));
		FRHIGPUTextureReadback* RbY = new FRHIGPUTextureReadback(TEXT("BF6WaterRbY"));
		AddEnqueueCopyPass(GraphBuilder, RbH, TexH);
		AddEnqueueCopyPass(GraphBuilder, RbX, TexX);
		AddEnqueueCopyPass(GraphBuilder, RbY, TexY);

		GraphBuilder.Execute();
		// SUBMIT and block. Blocking alone is not enough: without the submit the
		// commands may still be queued, the readback is never ready, and the
		// proof reports a failure that is really a missing flush.
		RHICmdList.SubmitAndBlockUntilGPUIdle();

		auto Fetch = [N, Count](FRHIGPUTextureReadback* Rb, TArray<FVector2f>& Dst) -> bool
		{
			if (!Rb->IsReady()) return false;
			int32 RowPitchPixels = 0;
			const void* Src = Rb->Lock(RowPitchPixels);
			if (!Src) { Rb->Unlock(); return false; }
			// The readback row pitch is in PIXELS and is not necessarily N:
			// copying N*N contiguously would shear the image on any aligned
			// pitch, which is exactly the kind of error that looks like a wrong
			// kernel.
			const FVector2f* S = static_cast<const FVector2f*>(Src);
			for (int32 y = 0; y < N; ++y)
				FMemory::Memcpy(&Dst[y * N], S + (int64)y * RowPitchPixels,
					N * sizeof(FVector2f));
			Rb->Unlock();
			return true;
		};

		const bool bH = Fetch(RbH, Height);
		const bool bX = Fetch(RbX, DispX);
		const bool bY = Fetch(RbY, DispY);
		bRan = bH && bX && bY;
		if (!bRan) Error = TEXT("GPU readback did not complete");

		delete RbH; delete RbX; delete RbY;
	});

	FlushRenderingCommands();

	Out.bRan = bRan;
	Out.Error = Error;
	if (bRan)
	{
		Out.Height = MoveTemp(Height);
		Out.DispX = MoveTemp(DispX);
		Out.DispY = MoveTemp(DispY);
	}
	return Out;
}

FWaterGPU::FResult FWaterGPU::DispersionReadback(const FCascadeInput& In, float TimeSeconds)
{
	return RunChain(In, TimeSeconds, /*bRunFft*/ false);
}

FWaterGPU::FResult FWaterGPU::DispersionFftReadback(const FCascadeInput& In, float TimeSeconds)
{
	return RunChain(In, TimeSeconds, /*bRunFft*/ true);
}

namespace
{
	// Relative L2 between two complex fields. Reported rather than thresholded
	// here; the caller decides what passes.
	double RelL2(const TArray<FVector2f>& A, const TArray<FVector2f>& B)
	{
		if (A.Num() != B.Num() || A.Num() == 0) return -1.0;
		double Num = 0.0, Den = 0.0;
		for (int32 i = 0; i < A.Num(); ++i)
		{
			const double dx = (double)A[i].X - (double)B[i].X;
			const double dy = (double)A[i].Y - (double)B[i].Y;
			Num += dx * dx + dy * dy;
			Den += (double)B[i].X * B[i].X + (double)B[i].Y * B[i].Y;
		}
		if (Den <= 0.0) return Num > 0.0 ? 1.0 : 0.0;
		return FMath::Sqrt(Num / Den);
	}

	float AbsMax(const TArray<FVector2f>& A)
	{
		float M = 0.f;
		for (const FVector2f& V : A)
			M = FMath::Max(M, FMath::Max(FMath::Abs(V.X), FMath::Abs(V.Y)));
		return M;
	}
}

FString FWaterGPU::Verify(const FCascadeInput& In, float TimeSeconds,
	const TArray<FVector2f>& CpuHeight,
	const TArray<FVector2f>& CpuDispX,
	const TArray<FVector2f>& CpuDispY)
{
	FResult Real = DispersionReadback(In, TimeSeconds);
	if (!Real.bRan)
		return FString::Printf(TEXT("GPU dispersion FAILED to run: %s"), *Real.Error);

	const double L2H = RelL2(Real.Height, CpuHeight);
	const double L2X = RelL2(Real.DispX, CpuDispX);
	const double L2Y = RelL2(Real.DispY, CpuDispY);

	// ZERO CONTROL. An all-zero spectrum must produce exactly zero. A kernel
	// that returns energy from no input is not computing the dispersion, and
	// would otherwise look like a pass on the real comparison alone.
	TArray<FVector2f> Zero;
	Zero.SetNumZeroed(In.N * In.N);
	FCascadeInput ZeroIn = In;
	ZeroIn.H0 = &Zero;
	FResult Ctl = DispersionReadback(ZeroIn, TimeSeconds);
	const float ZeroMax = Ctl.bRan
		? FMath::Max3(AbsMax(Ctl.Height), AbsMax(Ctl.DispX), AbsMax(Ctl.DispY))
		: -1.f;

	const bool bAgrees = L2H >= 0.0 && L2H < 1e-3 && L2X < 1e-3 && L2Y < 1e-3;
	const bool bZeroOk = Ctl.bRan && ZeroMax == 0.f;

	return FString::Printf(
		TEXT("GPU dispersion %dx%d tile %.1f m at t=%.3f:\n")
		TEXT("  vs CPU replay  height rel-L2 %.9g  dispX %.9g  dispY %.9g  -> %s\n")
		TEXT("  zero-H0 control absmax %.9g -> %s\n")
		TEXT("  overall: %s"),
		In.N, In.N, In.TileM, TimeSeconds, L2H, L2X, L2Y,
		bAgrees ? TEXT("AGREES") : TEXT("DISAGREES"),
		ZeroMax, bZeroOk ? TEXT("PASS") : TEXT("FAIL"),
		(bAgrees && bZeroOk)
			? TEXT("both controls pass; this kernel is safe to build on")
			: TEXT("NOT PASSING - do not bind this to the material"));
}

}   // namespace BF6HP

// The module exists to be EARLY, not to do work. Registering the virtual shader
// path here is the whole job: it has to happen before the global shader map is
// built, and the shader types below register during this DLL's static init for
// the same reason.
class FBF6HighPolyShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Self =
			IPluginManager::Get().FindPlugin(TEXT("BF6HighPoly"));
		if (!Self.IsValid())
		{
			UE_LOG(LogBF6WaterGPU, Warning,
				TEXT("BF6HighPoly plugin not found; ocean shaders cannot resolve their source"));
			return;
		}
		const FString ShaderDir = FPaths::Combine(Self->GetBaseDir(), TEXT("Shaders"));
		if (!FPaths::DirectoryExists(ShaderDir))
		{
			UE_LOG(LogBF6WaterGPU, Warning,
				TEXT("ocean shader directory missing: %s"), *ShaderDir);
			return;
		}
		if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/BF6HighPoly")))
		{
			AddShaderSourceDirectoryMapping(TEXT("/BF6HighPoly"), ShaderDir);
		}

		// Say out loud whether the file the shader type names actually resolves.
		// An unresolvable virtual path is a FATAL error later, during shader
		// compilation, with a message that points at the path rather than at
		// this mapping - so check it here where the cause is obvious.
		const FString Expected = FPaths::Combine(ShaderDir, TEXT("Private"),
			TEXT("BF6WaterOcean.usf"));
		UE_LOG(LogBF6WaterGPU, Display,
			TEXT("ocean shaders: /BF6HighPoly -> %s ; BF6WaterOcean.usf %s"),
			*ShaderDir,
			FPaths::FileExists(Expected) ? TEXT("found") : TEXT("MISSING - shader compilation will be fatal"));
	}
};

IMPLEMENT_MODULE(FBF6HighPolyShadersModule, BF6HighPolyShaders)

namespace BF6HP
{
FString FWaterGPU::VerifyFft(const FCascadeInput& In, float TimeSeconds,
	const TArray<FVector2f>& CpuHeight,
	const TArray<FVector2f>& CpuDispX,
	const TArray<FVector2f>& CpuDispY)
{
	FResult Real = DispersionFftReadback(In, TimeSeconds);
	if (!Real.bRan)
		return FString::Printf(TEXT("GPU dispersion+FFT FAILED to run: %s"), *Real.Error);

	const double L2H = RelL2(Real.Height, CpuHeight);
	const double L2X = RelL2(Real.DispX, CpuDispX);
	const double L2Y = RelL2(Real.DispY, CpuDispY);

	TArray<FVector2f> Zero;
	Zero.SetNumZeroed(In.N * In.N);
	FCascadeInput ZeroIn = In;
	ZeroIn.H0 = &Zero;
	FResult Ctl = DispersionFftReadback(ZeroIn, TimeSeconds);
	const float ZeroMax = Ctl.bRan
		? FMath::Max3(AbsMax(Ctl.Height), AbsMax(Ctl.DispX), AbsMax(Ctl.DispY))
		: -1.f;

	// A looser bound than the dispersion stage on purpose. The transform sums
	// N terms per output, and the GPU computes each twiddle from its angle
	// while the replay carries an incremental rotation, so the two drift apart
	// at the last bits by construction. This is still four orders below any
	// error that would be visible.
	const bool bAgrees = L2H >= 0.0 && L2H < 1e-3 && L2X < 1e-3 && L2Y < 1e-3;
	const bool bZeroOk = Ctl.bRan && ZeroMax == 0.f;

	return FString::Printf(
		TEXT("GPU dispersion+FFT %dx%d tile %.1f m at t=%.3f:\n")
		TEXT("  vs CPU replay  height rel-L2 %.9g  dispX %.9g  dispY %.9g  -> %s\n")
		TEXT("  zero-H0 control absmax %.9g -> %s\n")
		TEXT("  overall: %s"),
		In.N, In.N, In.TileM, TimeSeconds, L2H, L2X, L2Y,
		bAgrees ? TEXT("AGREES") : TEXT("DISAGREES"),
		ZeroMax, bZeroOk ? TEXT("PASS") : TEXT("FAIL"),
		(bAgrees && bZeroOk)
			? TEXT("both controls pass; the transform matches the replay")
			: TEXT("NOT PASSING - do not bind this to the material"));
}
}

