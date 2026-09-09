// The post-transform stage on the GPU: assemble, foam, diff.
//
// Split from BF6HighPolyWaterGPU.cpp only for size. The shader types here
// register the same way and rely on the same PostConfigInit module.

#include "BF6HighPolyWaterGPU.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogBF6WaterSurface, Log, All);

namespace
{
	BEGIN_SHADER_PARAMETER_STRUCT(FBF6SurfaceCommon, )
		SHADER_PARAMETER(uint32, SurfN)
		SHADER_PARAMETER(float, SurfDerivScale)
		SHADER_PARAMETER(float, SurfFoamThreshold)
		SHADER_PARAMETER(float, SurfFoamMax)
		SHADER_PARAMETER(float, SurfLag)
		SHADER_PARAMETER(float, SurfFoamEnabled)
		SHADER_PARAMETER(float, SurfPad0)
		SHADER_PARAMETER(float, SurfPad1)
	END_SHADER_PARAMETER_STRUCT()

	class FBF6AssembleCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FBF6AssembleCS);
		SHADER_USE_PARAMETER_STRUCT(FBF6AssembleCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float2>, AsmHeight)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float2>, AsmDispX)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float2>, AsmDispY)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, AsmOut)
			SHADER_PARAMETER_STRUCT_INCLUDE(FBF6SurfaceCommon, Common)
		END_SHADER_PARAMETER_STRUCT()
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{ return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	};

	class FBF6FoamCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FBF6FoamCS);
		SHADER_USE_PARAMETER_STRUCT(FBF6FoamCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FoamDisp)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, FoamPrev)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, FoamOut)
			SHADER_PARAMETER_STRUCT_INCLUDE(FBF6SurfaceCommon, Common)
		END_SHADER_PARAMETER_STRUCT()
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{ return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	};

	class FBF6DiffCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FBF6DiffCS);
		SHADER_USE_PARAMETER_STRUCT(FBF6DiffCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DiffDisp)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DiffFoam)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DiffOut)
			SHADER_PARAMETER_STRUCT_INCLUDE(FBF6SurfaceCommon, Common)
		END_SHADER_PARAMETER_STRUCT()
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{ return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	};
}

IMPLEMENT_GLOBAL_SHADER(FBF6AssembleCS,
	"/BF6HighPoly/Private/BF6WaterOcean.usf", "MainAssembleCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBF6FoamCS,
	"/BF6HighPoly/Private/BF6WaterOcean.usf", "MainFoamCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBF6DiffCS,
	"/BF6HighPoly/Private/BF6WaterOcean.usf", "MainDiffCS", SF_Compute);

namespace BF6HP
{

FWaterGPU::FSurfaceResult FWaterGPU::SurfaceReadback(const FCascadeInput& In,
	float TimeSeconds, const FSurfaceParams& Params)
{
	FSurfaceResult Out;

	// The spectra come from the existing chain, so this stage is verified on
	// exactly the data the earlier stages already proved rather than on a
	// separate run of them.
	FResult Spectra = DispersionFftReadback(In, TimeSeconds);
	if (!Spectra.bRan)
	{
		Out.Error = Spectra.Error;
		return Out;
	}

	const int32 N = In.N;
	const int32 Count = N * N;
	const float DerivScale = (float)N / (2.f * In.TileM);

	TArray<FVector2f> SpecH = Spectra.Height, SpecX = Spectra.DispX, SpecY = Spectra.DispY;
	TArray<float> Prev;
	Prev.SetNumZeroed(Count);
	if (Params.FoamPrevious && Params.FoamPrevious->Num() == Count)
		Prev = *Params.FoamPrevious;

	TArray<FVector4f> Disp, NormalFoam;
	TArray<float> FoamWork;
	Disp.SetNumZeroed(Count);
	NormalFoam.SetNumZeroed(Count);
	FoamWork.SetNumZeroed(Count);
	bool bRan = false;
	FString Error;

	const FBF6SurfaceCommon CommonIn = [&]
	{
		FBF6SurfaceCommon C;
		C.SurfN = (uint32)N;
		C.SurfDerivScale = DerivScale;
		C.SurfFoamThreshold = Params.FoamThreshold;
		C.SurfFoamMax = Params.FoamMax;
		C.SurfLag = Params.Lag;
		C.SurfFoamEnabled = Params.bFoamEnabled ? 1.f : 0.f;
		C.SurfPad0 = C.SurfPad1 = 0.f;
		return C;
	}();

	ENQUEUE_RENDER_COMMAND(BF6WaterSurface)(
		[N, Count, CommonIn, SpecH, SpecX, SpecY, Prev,
		 &Disp, &NormalFoam, &FoamWork, &bRan, &Error]
		(FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		auto Upload2 = [&](const TArray<FVector2f>& Src, const TCHAR* Name)
		{
			FRDGBufferRef B = CreateUploadBuffer(GraphBuilder, Name,
				sizeof(FVector2f), Count, Src.GetData(), Count * sizeof(FVector2f));
			return GraphBuilder.CreateSRV(B, PF_G32R32F);
		};

		FRDGBufferSRVRef TexH = Upload2(SpecH, TEXT("BF6Water.SpecH"));
		FRDGBufferSRVRef TexX = Upload2(SpecX, TEXT("BF6Water.SpecX"));
		FRDGBufferSRVRef TexY = Upload2(SpecY, TEXT("BF6Water.SpecY"));

		FRDGTextureRef TexDisp = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(FIntPoint(N, N), PF_A32B32G32R32F,
				FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource),
			TEXT("BF6Water.Disp"));
		FRDGBufferSRVRef TexPrev = GraphBuilder.CreateSRV(
			CreateUploadBuffer(GraphBuilder, TEXT("BF6Water.FoamPrev"),
				sizeof(float), Count, Prev.GetData(), Count * sizeof(float)),
			PF_R32_FLOAT);
		FRDGTextureRef TexFoam = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(FIntPoint(N, N), PF_R32_FLOAT,
				FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource),
			TEXT("BF6Water.FoamWork"));
		FRDGTextureRef TexNF = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(FIntPoint(N, N), PF_A32B32G32R32F,
				FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource),
			TEXT("BF6Water.NormalFoam"));

		const FIntVector Groups =
			FComputeShaderUtils::GetGroupCount(FIntPoint(N, N), FIntPoint(8, 8));

		{
			FBF6AssembleCS::FParameters* P =
				GraphBuilder.AllocParameters<FBF6AssembleCS::FParameters>();
			P->AsmHeight = TexH; P->AsmDispX = TexX; P->AsmDispY = TexY;
			P->AsmOut = GraphBuilder.CreateUAV(TexDisp);
			P->Common = CommonIn;
			TShaderMapRef<FBF6AssembleCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder,
				RDG_EVENT_NAME("BF6WaterAssemble"), S, P, Groups);
		}
		{
			FBF6FoamCS::FParameters* P =
				GraphBuilder.AllocParameters<FBF6FoamCS::FParameters>();
			P->FoamDisp = TexDisp; P->FoamPrev = TexPrev;
			P->FoamOut = GraphBuilder.CreateUAV(TexFoam);
			P->Common = CommonIn;
			TShaderMapRef<FBF6FoamCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder,
				RDG_EVENT_NAME("BF6WaterFoam"), S, P, Groups);
		}
		{
			FBF6DiffCS::FParameters* P =
				GraphBuilder.AllocParameters<FBF6DiffCS::FParameters>();
			P->DiffDisp = TexDisp; P->DiffFoam = TexFoam;
			P->DiffOut = GraphBuilder.CreateUAV(TexNF);
			P->Common = CommonIn;
			TShaderMapRef<FBF6DiffCS> S(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder,
				RDG_EVENT_NAME("BF6WaterDiff"), S, P, Groups);
		}

		FRHIGPUTextureReadback* RbD = new FRHIGPUTextureReadback(TEXT("BF6WaterRbDisp"));
		FRHIGPUTextureReadback* RbN = new FRHIGPUTextureReadback(TEXT("BF6WaterRbNF"));
		FRHIGPUTextureReadback* RbF = new FRHIGPUTextureReadback(TEXT("BF6WaterRbFoam"));
		AddEnqueueCopyPass(GraphBuilder, RbD, TexDisp);
		AddEnqueueCopyPass(GraphBuilder, RbN, TexNF);
		AddEnqueueCopyPass(GraphBuilder, RbF, TexFoam);

		GraphBuilder.Execute();
		RHICmdList.SubmitAndBlockUntilGPUIdle();

		auto Fetch4 = [N](FRHIGPUTextureReadback* Rb, TArray<FVector4f>& Dst) -> bool
		{
			if (!Rb->IsReady()) return false;
			int32 Pitch = 0;
			const void* Src = Rb->Lock(Pitch);
			if (!Src) { Rb->Unlock(); return false; }
			const FVector4f* S = static_cast<const FVector4f*>(Src);
			for (int32 y = 0; y < N; ++y)
				FMemory::Memcpy(&Dst[y * N], S + (int64)y * Pitch, N * sizeof(FVector4f));
			Rb->Unlock();
			return true;
		};
		auto Fetch1 = [N](FRHIGPUTextureReadback* Rb, TArray<float>& Dst) -> bool
		{
			if (!Rb->IsReady()) return false;
			int32 Pitch = 0;
			const void* Src = Rb->Lock(Pitch);
			if (!Src) { Rb->Unlock(); return false; }
			const float* S = static_cast<const float*>(Src);
			for (int32 y = 0; y < N; ++y)
				FMemory::Memcpy(&Dst[y * N], S + (int64)y * Pitch, N * sizeof(float));
			Rb->Unlock();
			return true;
		};

		bRan = Fetch4(RbD, Disp) && Fetch4(RbN, NormalFoam) && Fetch1(RbF, FoamWork);
		if (!bRan) Error = TEXT("surface readback did not complete");
		delete RbD; delete RbN; delete RbF;
	});

	FlushRenderingCommands();

	Out.bRan = bRan;
	Out.Error = Error;
	if (bRan)
	{
		Out.Displacement = MoveTemp(Disp);
		Out.NormalFoam = MoveTemp(NormalFoam);
		Out.FoamWork = MoveTemp(FoamWork);
	}
	return Out;
}

FString FWaterGPU::VerifySurface(const FCascadeInput& In, float TimeSeconds,
	const FSurfaceParams& Params, const TArray<FVector3f>& CpuDisplacement)
{
	FSurfaceResult R = SurfaceReadback(In, TimeSeconds, Params);
	if (!R.bRan)
		return FString::Printf(TEXT("GPU surface FAILED to run: %s"), *R.Error);
	if (R.Displacement.Num() != CpuDisplacement.Num())
		return TEXT("GPU surface: displacement size disagrees with the replay");

	double Num = 0.0, Den = 0.0;
	float PeakGpu = 0.f, PeakCpu = 0.f;
	for (int32 i = 0; i < CpuDisplacement.Num(); ++i)
	{
		const FVector3f G(R.Displacement[i].X, R.Displacement[i].Y, R.Displacement[i].Z);
		const FVector3f C = CpuDisplacement[i];
		Num += FVector3f::DistSquared(G, C);
		Den += (double)C.SizeSquared();
		PeakGpu = FMath::Max(PeakGpu, FMath::Abs(G.Y));
		PeakCpu = FMath::Max(PeakCpu, FMath::Abs(C.Y));
	}
	const double RelL2 = (Den > 0.0) ? FMath::Sqrt(Num / Den) : (Num > 0.0 ? 1.0 : 0.0);

	TArray<FVector2f> Zero;
	Zero.SetNumZeroed(In.N * In.N);
	FCascadeInput ZeroIn = In;
	ZeroIn.H0 = &Zero;
	FSurfaceParams ZeroParams = Params;
	ZeroParams.FoamPrevious = nullptr;      // no history, so nothing can leak in
	FSurfaceResult Ctl = SurfaceReadback(ZeroIn, TimeSeconds, ZeroParams);
	float ZeroMax = -1.f;
	if (Ctl.bRan)
	{
		ZeroMax = 0.f;
		for (const FVector4f& V : Ctl.Displacement)
			ZeroMax = FMath::Max(ZeroMax,
				FMath::Max3(FMath::Abs(V.X), FMath::Abs(V.Y), FMath::Abs(V.Z)));
	}

	const bool bAgrees = RelL2 < 1e-3;
	const bool bZeroOk = Ctl.bRan && ZeroMax == 0.f;
	return FString::Printf(
		TEXT("GPU surface (assemble+foam+diff) %dx%d tile %.1f m:\n")
		TEXT("  displacement vs replay rel-L2 %.9g -> %s\n")
		TEXT("  peak vertical  GPU %.6f m   CPU %.6f m\n")
		TEXT("  zero-H0 control displacement absmax %.9g -> %s\n")
		TEXT("  overall: %s"),
		In.N, In.N, In.TileM, RelL2, bAgrees ? TEXT("AGREES") : TEXT("DISAGREES"),
		PeakGpu, PeakCpu, ZeroMax, bZeroOk ? TEXT("PASS") : TEXT("FAIL"),
		(bAgrees && bZeroOk)
			? TEXT("both controls pass; the GPU produces the replay's geometry")
			: TEXT("NOT PASSING - do not bind this to the material"));
}

}   // namespace BF6HP
