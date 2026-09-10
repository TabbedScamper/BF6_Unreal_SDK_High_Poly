#include "BF6HighPolyWaterFFT.h"

#include "Engine/Texture2D.h"
#include "UObject/Package.h"
#include "Engine/Texture2DDynamic.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Math/Float16Color.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#endif

#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d12.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBF6WaterFFT, Log, All);

namespace BF6HP
{
struct FWaterFFT::FAsyncFrame
{
	TArray<FCascade> Cascades;
	float Seconds = 0.f;
};
namespace
{
	TAutoConsoleVariable<int32> CVarWaterAsync(TEXT("BF6.HighPoly.WaterAsync"), 1,
		TEXT("Compute ocean FFTs on one bounded worker job. 0 provides the synchronous comparison."));
	FORCEINLINE int32 Wrap(int32 V, int32 N) { return V & (N - 1); }
	FORCEINLINE FWaterFFT::FComplex Mul(const FWaterFFT::FComplex& A, float C, float S)
	{
		return { A.R * C - A.I * S, A.R * S + A.I * C };
	}

#if PLATFORM_WINDOWS
	using Microsoft::WRL::ComPtr;

	constexpr const TCHAR* GDispersionResource =
		TEXT("shaders/bytecode/9e132eef-0f4f-5264-0348-9423c38cc6ce");
	constexpr const TCHAR* GColumnResource =
		TEXT("shaders/bytecode/ec29cb5a-a06d-f031-91bf-63983b82e10d");
	constexpr const TCHAR* GRowResource =
		TEXT("shaders/bytecode/4e3cfd7c-646c-c2d8-c538-fe1b8a8de8fb");
	constexpr const TCHAR* GMergeResource =
		TEXT("shaders/bytecode/bcf63891-56a1-2011-be38-defc092d7074");

	FString HResultText(HRESULT Hr)
	{
		TCHAR Buffer[512] = {};
		FPlatformMisc::GetSystemErrorMessage(Buffer, UE_ARRAY_COUNT(Buffer), Hr);
		return FString::Printf(TEXT("0x%08x %s"), static_cast<uint32>(Hr), Buffer);
	}

	D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE Type)
	{
		D3D12_HEAP_PROPERTIES H = {};
		H.Type = Type;
		H.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		H.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		H.CreationNodeMask = H.VisibleNodeMask = 1;
		return H;
	}

	D3D12_RESOURCE_DESC BufferDesc(UINT64 Bytes)
	{
		D3D12_RESOURCE_DESC D = {};
		D.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		D.Width = Bytes;
		D.Height = D.DepthOrArraySize = D.MipLevels = 1;
		D.SampleDesc.Count = 1;
		D.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		return D;
	}

	D3D12_RESOURCE_DESC TextureDesc(UINT N, DXGI_FORMAT Format,
		D3D12_RESOURCE_FLAGS Flags)
	{
		D3D12_RESOURCE_DESC D = {};
		D.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		D.Width = N;
		D.Height = N;
		D.DepthOrArraySize = D.MipLevels = 1;
		D.Format = Format;
		D.SampleDesc.Count = 1;
		D.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		D.Flags = Flags;
		return D;
	}

	void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
		D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
	{
		D3D12_RESOURCE_BARRIER B = {};
		B.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		B.Transition.pResource = Resource;
		B.Transition.StateBefore = Before;
		B.Transition.StateAfter = After;
		B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		CL->ResourceBarrier(1, &B);
	}

	void UavBarrier(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource)
	{
		D3D12_RESOURCE_BARRIER B = {};
		B.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		B.UAV.pResource = Resource;
		CL->ResourceBarrier(1, &B);
	}

	bool ExtractDxContainer(const TArray<uint8>& Wrapped, TArray<uint8>& Out,
		FString& Error)
	{
		Out.Reset();
		const int32 Limit = FMath::Min(Wrapped.Num() - 3, 8192);
		int32 At = INDEX_NONE;
		for (int32 I = 0; I < Limit; ++I)
		{
			if (Wrapped[I] == 'D' && Wrapped[I + 1] == 'X' &&
				Wrapped[I + 2] == 'B' && Wrapped[I + 3] == 'C')
			{
				At = I;
				break;
			}
		}
		if (At == INDEX_NONE || At + 0x20 > Wrapped.Num())
		{
			Error = TEXT("installed resource has no DXBC/DXIL container");
			return false;
		}
		uint32 Bytes = 0;
		FMemory::Memcpy(&Bytes, Wrapped.GetData() + At + 0x18, sizeof(Bytes));
		if (Bytes < 0x20 || static_cast<int64>(At) + Bytes > Wrapped.Num())
		{
			Error = TEXT("installed DXBC/DXIL container length is invalid");
			return false;
		}
		Out.Append(Wrapped.GetData() + At, static_cast<int32>(Bytes));
		return true;
	}

	class FBF6NativeTextureResource final : public FTextureResource
	{
	public:
		FBF6NativeTextureResource(UTexture2DDynamic* InOwner,
			ComPtr<ID3D12Resource> InNative, int32 InN)
			: Owner(InOwner), Native(MoveTemp(InNative)), N(InN) {}

		virtual uint32 GetSizeX() const override { return N; }
		virtual uint32 GetSizeY() const override { return N; }
		virtual FString GetFriendlyName() const override
		{
			return Owner ? Owner->GetPathName() : TEXT("BF6DirectDXILTexture");
		}

		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			ID3D12DynamicRHI* D3D12RHI = GetID3D12DynamicRHI();
			// Unreal only samples this alias.  The native resource is still used as
			// a UAV by the BF6 command list, but advertising UAV ownership to the
			// RHI makes D3D12RHI demand an internal ED3D12Access state that the
			// public external-resource API cannot supply.  SRV-only is the accurate
			// contract at this boundary and avoids D3D12Resources.h's Unknown-access
			// assertion.
			TextureRHI = D3D12RHI->RHICreateTexture2DFromResource(
				PF_FloatRGBA, TexCreate_ShaderResource,
				FClearValueBinding::None, Native.Get());
			SamplerStateRHI = RHICreateSamplerState(FSamplerStateInitializerRHI(
				SF_Bilinear, AM_Wrap, AM_Wrap, AM_Wrap));
			if (TextureReferenceRHI.IsValid())
				RHICmdList.UpdateTextureReference(TextureReferenceRHI, TextureRHI);
		}

		virtual void ReleaseRHI() override
		{
			if (TextureReferenceRHI.IsValid()) RHIClearTextureReference(TextureReferenceRHI);
			TextureRHI.SafeRelease();
			SamplerStateRHI.SafeRelease();
			Native.Reset();
		}

	private:
		UTexture2DDynamic* Owner = nullptr;
		ComPtr<ID3D12Resource> Native;
		int32 N = 0;
	};
#endif
}

struct FWaterFFT::FDirectState
{
#if PLATFORM_WINDOWS
	struct FCascade
	{
		int32 N = 0;
		float TileM = 0.f;
		bool bRan = false;
		bool bUploaded = false;
		ComPtr<ID3D12Resource> Input, Spec[3], Tmp[3], Spatial[3], Merged, Display,
			Upload, ProofReadback;
		ComPtr<ID3D12DescriptorHeap> Heap;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT InputFootprint = {};
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT ProofFootprint = {};
		UINT64 ProofBytes = 0;
		UTexture2DDynamic* Texture = nullptr;
	};

	struct FProofRequest
	{
		float Seconds = 0.f;
		bool bZeroInput = false;
		TArray<TArray<FVector3f>> Expected;
	};

	ComPtr<ID3D12RootSignature> Root;
	ComPtr<ID3D12PipelineState> Dispersion, Column, Row, Merge;
	TArray<FCascade> Cascades;
	bool bFakeResourceRejected = false;
	ComPtr<ID3D12Fence> ProofFence;
	uint64 ProofFenceValue = 0;
	TSharedPtr<FProofRequest, ESPMode::ThreadSafe> PendingProof;
	mutable FCriticalSection ProofLock;
	FString ProofText = TEXT("GPU proof: waiting for the first direct dispatch");
	bool bProofQueued = false;

	~FDirectState() { ResetTextures(); }

	void ResetTextures()
	{
		if (!IsEngineExitRequested())
		{
			for (FCascade& C : Cascades)
			{
				if (C.Texture)
				{
					C.Texture->ReleaseResource();
					C.Texture->RemoveFromRoot();
					C.Texture = nullptr;
				}
			}
		}
	}

	bool TryQueueProof()
	{
		FScopeLock Lock(&ProofLock);
		if (bProofQueued) return false;
		bProofQueued = true;
		ProofText = TEXT("GPU proof: readback queued");
		return true;
	}

	FString GetProofText() const
	{
		FScopeLock Lock(&ProofLock);
		return ProofText;
	}

	void SetProofText(const FString& Text)
	{
		{
			FScopeLock Lock(&ProofLock);
			ProofText = Text;
		}
		UE_LOG(LogBF6WaterFFT, Display, TEXT("%s"), *Text);
	}

	static bool MakeRoot(ID3D12Device* Device, ComPtr<ID3D12RootSignature>& Out,
		FString& Error)
	{
		D3D12_DESCRIPTOR_RANGE Ranges[2] = {};
		Ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0,
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
		Ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0,
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
		D3D12_ROOT_PARAMETER Params[3] = {};
		Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		Params[0].Constants.ShaderRegister = 0;
		Params[0].Constants.RegisterSpace = 0;
		Params[0].Constants.Num32BitValues = 4;
		Params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		for (UINT I = 0; I < 2; ++I)
		{
			Params[I + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			Params[I + 1].DescriptorTable.NumDescriptorRanges = 1;
			Params[I + 1].DescriptorTable.pDescriptorRanges = &Ranges[I];
			Params[I + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		}
		D3D12_ROOT_SIGNATURE_DESC Desc = {};
		Desc.NumParameters = UE_ARRAY_COUNT(Params);
		Desc.pParameters = Params;
		ComPtr<ID3DBlob> Blob, Errors;
		HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1,
			&Blob, &Errors);
		if (FAILED(Hr))
		{
			Error = Errors
				? FString(UTF8_TO_TCHAR(static_cast<const char*>(Errors->GetBufferPointer())))
				: HResultText(Hr);
			return false;
		}
		Hr = Device->CreateRootSignature(0, Blob->GetBufferPointer(),
			Blob->GetBufferSize(), IID_PPV_ARGS(&Out));
		if (FAILED(Hr)) { Error = HResultText(Hr); return false; }
		return true;
	}

	static bool MakePso(ID3D12Device* Device, ID3D12RootSignature* InRoot,
		const TArray<uint8>& Shader, ComPtr<ID3D12PipelineState>& Out, FString& Error)
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC Desc = {};
		Desc.pRootSignature = InRoot;
		Desc.CS = { Shader.GetData(), static_cast<SIZE_T>(Shader.Num()) };
		const HRESULT Hr = Device->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&Out));
		if (FAILED(Hr)) { Error = HResultText(Hr); return false; }
		return true;
	}

	static UTexture2DDynamic* MakeNativeTexture(ComPtr<ID3D12Resource> Native,
		int32 N, int32 Index)
	{
		UTexture2DDynamic* Texture = NewObject<UTexture2DDynamic>(GetTransientPackageAsObject(),
			*FString::Printf(TEXT("BF6_DirectDXIL_Disp_%d"), Index));
		if (!Texture) return nullptr;
		Texture->SizeX = N;
		Texture->SizeY = N;
		Texture->Format = PF_FloatRGBA;
		Texture->NumMips = 1;
		Texture->SRGB = false;
		Texture->NeverStream = true;
		Texture->Filter = TF_Bilinear;
		Texture->SamplerAddressMode = AM_Wrap;
		Texture->AddToRoot();
		FBF6NativeTextureResource* Resource =
			new FBF6NativeTextureResource(Texture, MoveTemp(Native), N);
		Texture->SetResource(Resource);
		ENQUEUE_RENDER_COMMAND(BF6InitNativeWaterTexture)(
			[Texture, Resource](FRHICommandListImmediate& RHICmdList)
			{
				Resource->SetTextureReference(Texture->TextureReference.TextureReferenceRHI);
				Resource->InitResource(RHICmdList);
			});
		return Texture;
	}

	bool InitCascade(ID3D12Device* Device, const FCore::FWaterCascade& InputData,
		int32 Index, FString& Error)
	{
		FCascade C;
		C.N = InputData.Resolution;
		C.TileM = InputData.TileDimension;
		const auto DefaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
		const auto UploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
		D3D12_RESOURCE_DESC RG = TextureDesc(C.N, DXGI_FORMAT_R32G32_FLOAT,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		D3D12_RESOURCE_DESC InputDesc = RG;
		InputDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		D3D12_RESOURCE_DESC Half4 = TextureDesc(C.N,
			DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		HRESULT Hr = Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE,
			&InputDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&C.Input));
		if (FAILED(Hr)) { Error = TEXT("create H0 texture: ") + HResultText(Hr); return false; }
		for (int32 I = 0; I < 3; ++I)
		{
			Hr = Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE,
				&RG, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&C.Spec[I]));
			if (SUCCEEDED(Hr)) Hr = Device->CreateCommittedResource(&DefaultHeap,
				D3D12_HEAP_FLAG_NONE, &RG, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				nullptr, IID_PPV_ARGS(&C.Tmp[I]));
			if (SUCCEEDED(Hr)) Hr = Device->CreateCommittedResource(&DefaultHeap,
				D3D12_HEAP_FLAG_NONE, &RG, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				nullptr, IID_PPV_ARGS(&C.Spatial[I]));
			if (FAILED(Hr)) { Error = TEXT("create FFT textures: ") + HResultText(Hr); return false; }
		}
		constexpr D3D12_RESOURCE_STATES ShaderRead =
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		Hr = Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE,
			&Half4, ShaderRead, nullptr, IID_PPV_ARGS(&C.Merged));
		if (FAILED(Hr)) { Error = TEXT("create merged texture: ") + HResultText(Hr); return false; }
		D3D12_RESOURCE_DESC DisplayDesc = Half4;
		DisplayDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		Hr = Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE,
			&DisplayDesc, ShaderRead, nullptr, IID_PPV_ARGS(&C.Display));
		if (FAILED(Hr)) { Error = TEXT("create display texture: ") + HResultText(Hr); return false; }

		UINT ProofRows = 0;
		UINT64 ProofRowBytes = 0;
		Device->GetCopyableFootprints(&Half4, 0, 1, 0, &C.ProofFootprint,
			&ProofRows, &ProofRowBytes, &C.ProofBytes);
		const D3D12_RESOURCE_DESC ProofDesc = BufferDesc(C.ProofBytes);
		const auto ReadbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
		Hr = Device->CreateCommittedResource(&ReadbackHeap, D3D12_HEAP_FLAG_NONE,
			&ProofDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(&C.ProofReadback));
		if (FAILED(Hr)) { Error = TEXT("create proof readback: ") + HResultText(Hr); return false; }

		UINT Rows = 0;
		UINT64 RowBytes = 0, UploadBytes = 0;
		Device->GetCopyableFootprints(&InputDesc, 0, 1, 0, &C.InputFootprint,
			&Rows, &RowBytes, &UploadBytes);
		const D3D12_RESOURCE_DESC UploadDesc = BufferDesc(UploadBytes);
		Hr = Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE,
			&UploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&C.Upload));
		if (FAILED(Hr)) { Error = TEXT("create H0 upload: ") + HResultText(Hr); return false; }
		uint8* Mapped = nullptr;
		D3D12_RANGE NoRead = { 0, 0 };
		Hr = C.Upload->Map(0, &NoRead, reinterpret_cast<void**>(&Mapped));
		if (FAILED(Hr)) { Error = TEXT("map H0 upload: ") + HResultText(Hr); return false; }
		for (int32 Y = 0; Y < C.N; ++Y)
		{
			FMemory::Memcpy(Mapped + C.InputFootprint.Offset +
				static_cast<SIZE_T>(Y) * C.InputFootprint.Footprint.RowPitch,
				InputData.H0.GetData() + static_cast<SIZE_T>(Y) * C.N,
				static_cast<SIZE_T>(C.N) * sizeof(FVector2f));
		}
		C.Upload->Unmap(0, nullptr);

		constexpr UINT Passes = 8, Block = 6;
		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		HeapDesc.NumDescriptors = Passes * Block;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Hr = Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&C.Heap));
		if (FAILED(Hr)) { Error = TEXT("create descriptor heap: ") + HResultText(Hr); return false; }
		const UINT Increment = Device->GetDescriptorHandleIncrementSize(HeapDesc.Type);
		auto Cpu = [&](UINT I)
		{
			auto H = C.Heap->GetCPUDescriptorHandleForHeapStart();
			H.ptr += static_cast<SIZE_T>(I) * Increment;
			return H;
		};
		auto MakeBlock = [&](UINT Pass, ID3D12Resource* S0, ID3D12Resource* S1,
			ID3D12Resource* S2, ID3D12Resource* U0, ID3D12Resource* U1,
			ID3D12Resource* U2, DXGI_FORMAT U0Format)
		{
			const UINT Base = Pass * Block;
			ID3D12Resource* SR[3] = { S0, S1, S2 };
			for (UINT I = 0; I < 3; ++I)
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC V = {};
				V.Format = DXGI_FORMAT_R32G32_FLOAT;
				V.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				V.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				V.Texture2D.MipLevels = 1;
				Device->CreateShaderResourceView(SR[I], &V, Cpu(Base + I));
			}
			ID3D12Resource* UR[3] = { U0, U1, U2 };
			for (UINT I = 0; I < 3; ++I)
			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC V = {};
				V.Format = I == 0 ? U0Format : DXGI_FORMAT_R32G32_FLOAT;
				V.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				Device->CreateUnorderedAccessView(UR[I], nullptr, &V, Cpu(Base + 3 + I));
			}
		};
		MakeBlock(0, C.Input.Get(), nullptr, nullptr, C.Spec[0].Get(),
			C.Spec[1].Get(), C.Spec[2].Get(), DXGI_FORMAT_R32G32_FLOAT);
		for (UINT I = 0; I < 3; ++I)
			MakeBlock(1 + I, C.Spec[I].Get(), nullptr, nullptr, C.Tmp[I].Get(),
				nullptr, nullptr, DXGI_FORMAT_R32G32_FLOAT);
		for (UINT I = 0; I < 3; ++I)
			MakeBlock(4 + I, C.Tmp[I].Get(), nullptr, nullptr, C.Spatial[I].Get(),
				nullptr, nullptr, DXGI_FORMAT_R32G32_FLOAT);
		MakeBlock(7, C.Spatial[0].Get(), C.Spatial[1].Get(), C.Spatial[2].Get(),
			C.Merged.Get(), nullptr, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);

		// D3D12RHI cannot import an ALLOW_UNORDERED_ACCESS resource without an
		// internal initial-access value, which its public external-resource API
		// does not expose.  Keep the BF6 output private and UAV-capable, then give
		// Unreal a read-only twin populated on the same command list.
		C.Texture = MakeNativeTexture(C.Display, C.N, Index);
		if (!C.Texture) { Error = TEXT("create Unreal native texture wrapper"); return false; }
		Cascades.Add(MoveTemp(C));
		return true;
	}

	bool Initialize(FCore& Core, const TArray<FCore::FWaterCascade>& Inputs,
		FString& Error)
	{
		if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12)
		{
			Error = TEXT("Unreal is not running the D3D12 RHI");
			return false;
		}
		TArray<uint8> Fake;
		bFakeResourceRejected = !Core.ReadRawResource(
			TEXT("shaders/bytecode/00000000-0000-0000-0000-000000000000"), Fake);
		const TCHAR* Names[4] = { GDispersionResource, GColumnResource,
			GRowResource, GMergeResource };
		TArray<uint8> Shaders[4];
		for (int32 I = 0; I < 4; ++I)
		{
			TArray<uint8> Wrapped;
			if (!Core.ReadRawResource(Names[I], Wrapped) ||
				!ExtractDxContainer(Wrapped, Shaders[I], Error))
			{
				if (Error.IsEmpty()) Error = Core.Error;
				return false;
			}
		}
		ID3D12Device* Device = GetID3D12DynamicRHI()->RHIGetDevice(0);
		if (!Device) { Error = TEXT("Unreal D3D12 device is unavailable"); return false; }
		HRESULT Hr = Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
			IID_PPV_ARGS(&ProofFence));
		if (FAILED(Hr)) { Error = TEXT("create proof fence: ") + HResultText(Hr); return false; }
		if (!MakeRoot(Device, Root, Error) ||
			!MakePso(Device, Root.Get(), Shaders[0], Dispersion, Error) ||
			!MakePso(Device, Root.Get(), Shaders[1], Column, Error) ||
			!MakePso(Device, Root.Get(), Shaders[2], Row, Error) ||
			!MakePso(Device, Root.Get(), Shaders[3], Merge, Error))
			return false;
		for (int32 I = 0; I < Inputs.Num() && I < 4; ++I)
		{
			if (!InitCascade(Device, Inputs[I], I, Error)) return false;
		}
		UE_LOG(LogBF6WaterFFT, Display,
			TEXT("DIRECT DXIL: installed resources=4/4 PSOs=4/4 cascades=%d fake-id-control=%d/1"),
			Cascades.Num(), bFakeResourceRejected ? 1 : 0);
		return Cascades.Num() == FMath::Min(Inputs.Num(), 4) && bFakeResourceRejected;
	}

	void PollProof()
	{
		if (!PendingProof || !ProofFence ||
			ProofFence->GetCompletedValue() < ProofFenceValue) return;

		double Error2 = 0.0;
		double Reference2 = 0.0;
		float MaxAbsError = 0.f;
		int64 Compared = 0;
		int64 NonFinite = 0;
		int64 NonZero = 0;
		bool bMapFailed = false;
		for (int32 CascadeIndex = 0; CascadeIndex < Cascades.Num(); ++CascadeIndex)
		{
			FCascade& C = Cascades[CascadeIndex];
			if (!PendingProof->Expected.IsValidIndex(CascadeIndex) ||
				PendingProof->Expected[CascadeIndex].Num() != C.N * C.N)
			{
				bMapFailed = true;
				break;
			}
			uint8* Mapped = nullptr;
			D3D12_RANGE ReadRange = { 0, static_cast<SIZE_T>(C.ProofBytes) };
			if (FAILED(C.ProofReadback->Map(0, &ReadRange,
				reinterpret_cast<void**>(&Mapped))))
			{
				bMapFailed = true;
				break;
			}
			for (int32 Y = 0; Y < C.N; ++Y)
			{
				const FFloat16Color* RowPixels = reinterpret_cast<const FFloat16Color*>(
					Mapped + C.ProofFootprint.Offset +
					static_cast<SIZE_T>(Y) * C.ProofFootprint.Footprint.RowPitch);
				for (int32 X = 0; X < C.N; ++X)
				{
					const FVector3f& E = PendingProof->Expected[CascadeIndex][Y * C.N + X];
					const float Expected[3] = { E.X, E.Y, E.Z };
					const float Actual[3] = { RowPixels[X].R.GetFloat(), RowPixels[X].G.GetFloat(),
						RowPixels[X].B.GetFloat() };
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						if (!FMath::IsFinite(Actual[Channel])) { ++NonFinite; continue; }
						if (Actual[Channel] != 0.f) ++NonZero;
						const double Difference = static_cast<double>(Actual[Channel]) - Expected[Channel];
						Error2 += Difference * Difference;
						Reference2 += static_cast<double>(Expected[Channel]) * Expected[Channel];
						MaxAbsError = FMath::Max(MaxAbsError, FMath::Abs(static_cast<float>(Difference)));
						++Compared;
					}
				}
			}
			D3D12_RANGE NoWrite = { 0, 0 };
			C.ProofReadback->Unmap(0, &NoWrite);
		}

		if (bMapFailed || Compared == 0)
		{
			SetProofText(TEXT("GPU proof: FAIL (readback or expected-shape mismatch)"));
		}
		else
		{
			const double RelativeL2 = Reference2 > 0.0
				? FMath::Sqrt(Error2 / Reference2) : FMath::Sqrt(Error2);
			const bool bPass = NonFinite == 0 && (PendingProof->bZeroInput
				? NonZero == 0
				: RelativeL2 <= 0.01 && MaxAbsError <= 1.0e-4f);
			SetProofText(FString::Printf(
				TEXT("GPU proof: %s | installed DXIL vs independent CPU replay at t=%.6fs | rel-L2 %.9g | max |error| %.9gm | finite %lld/%lld | %s nonzero %lld"),
				bPass ? TEXT("PASS") : TEXT("FAIL"), PendingProof->Seconds,
				RelativeL2, MaxAbsError, Compared, Compared + NonFinite,
				PendingProof->bZeroInput ? TEXT("ZERO-H0 CONTROL") : TEXT("live-input"),
				NonZero));
		}
		PendingProof.Reset();
	}

	void Dispatch(FRHICommandListImmediate& RHICmdList, float Seconds,
		TSharedPtr<FProofRequest, ESPMode::ThreadSafe> Proof)
	{
		PollProof();
		ID3D12DynamicRHI* D3D12RHI = GetID3D12DynamicRHI();
		D3D12RHI->RHIFlushResourceBarriers(RHICmdList, 0);
		ID3D12GraphicsCommandList* CL = D3D12RHI->RHIGetGraphicsCommandList(RHICmdList, 0);
		constexpr D3D12_RESOURCE_STATES NonPixel =
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		constexpr D3D12_RESOURCE_STATES ShaderRead = NonPixel |
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		for (int32 CascadeIndex = 0; CascadeIndex < Cascades.Num(); ++CascadeIndex)
		{
			FCascade& C = Cascades[CascadeIndex];
			if (!C.bUploaded)
			{
				D3D12_TEXTURE_COPY_LOCATION Dest = {};
				Dest.pResource = C.Input.Get();
				Dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				D3D12_TEXTURE_COPY_LOCATION Source = {};
				Source.pResource = C.Upload.Get();
				Source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				Source.PlacedFootprint = C.InputFootprint;
				CL->CopyTextureRegion(&Dest, 0, 0, 0, &Source, nullptr);
				Transition(CL, C.Input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NonPixel);
				C.bUploaded = true;
			}
			if (C.bRan)
			{
				for (int32 I = 0; I < 3; ++I)
				{
					Transition(CL, C.Spec[I].Get(), NonPixel,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					Transition(CL, C.Tmp[I].Get(), NonPixel,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					Transition(CL, C.Spatial[I].Get(), NonPixel,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				}
			}
			Transition(CL, C.Merged.Get(), ShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			ID3D12DescriptorHeap* Heaps[] = { C.Heap.Get() };
			CL->SetDescriptorHeaps(1, Heaps);
			CL->SetComputeRootSignature(Root.Get());
			const UINT Increment = GetID3D12DynamicRHI()->RHIGetDevice(0)
				->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto Gpu = [&](UINT I)
			{
				auto H = C.Heap->GetGPUDescriptorHandleForHeapStart();
				H.ptr += static_cast<UINT64>(I) * Increment;
				return H;
			};
			auto Bind = [&](UINT Pass)
			{
				const UINT Base = Pass * 6;
				CL->SetComputeRootDescriptorTable(1, Gpu(Base));
				CL->SetComputeRootDescriptorTable(2, Gpu(Base + 3));
			};
			uint32 DispersionConstants[4] = {};
			DispersionConstants[0] = static_cast<uint32>(C.N);
			const float InvTile = 1.f / C.TileM;
			FMemory::Memcpy(&DispersionConstants[1], &InvTile, sizeof(float));
			FMemory::Memcpy(&DispersionConstants[2], &Seconds, sizeof(float));
			CL->SetComputeRoot32BitConstants(0, 4, DispersionConstants, 0);
			CL->SetPipelineState(Dispersion.Get());
			Bind(0);
			CL->Dispatch((C.N + 15) / 16, (C.N / 2 + 16) / 16, 1);
			const uint32 FftConstants[4] = { static_cast<uint32>(C.N / 2), 0, 0, 0 };
			CL->SetComputeRoot32BitConstants(0, 4, FftConstants, 0);
			for (int32 I = 0; I < 3; ++I)
			{
				UavBarrier(CL, C.Spec[I].Get());
				Transition(CL, C.Spec[I].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NonPixel);
				CL->SetPipelineState(Column.Get());
				Bind(1 + I);
				CL->Dispatch(C.N, 1, 1);
				UavBarrier(CL, C.Tmp[I].Get());
				Transition(CL, C.Tmp[I].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NonPixel);
				CL->SetPipelineState(Row.Get());
				Bind(4 + I);
				CL->Dispatch(C.N, 1, 1);
				UavBarrier(CL, C.Spatial[I].Get());
				Transition(CL, C.Spatial[I].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NonPixel);
			}
			CL->SetPipelineState(Merge.Get());
			Bind(7);
			CL->Dispatch(C.N / 16, C.N / 16, 1);
			UavBarrier(CL, C.Merged.Get());
			Transition(CL, C.Merged.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(CL, C.Display.Get(), ShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
			CL->CopyResource(C.Display.Get(), C.Merged.Get());
			if (Proof)
			{
				D3D12_TEXTURE_COPY_LOCATION ProofDest = {};
				ProofDest.pResource = C.ProofReadback.Get();
				ProofDest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				ProofDest.PlacedFootprint = C.ProofFootprint;
				D3D12_TEXTURE_COPY_LOCATION ProofSource = {};
				ProofSource.pResource = C.Merged.Get();
				ProofSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				CL->CopyTextureRegion(&ProofDest, 0, 0, 0, &ProofSource, nullptr);
			}
			Transition(CL, C.Display.Get(), D3D12_RESOURCE_STATE_COPY_DEST, ShaderRead);
			Transition(CL, C.Merged.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ShaderRead);
			C.bRan = true;
		}
		D3D12RHI->RHIFinishExternalComputeWork(RHICmdList, 0, CL);
		if (Proof)
		{
			PendingProof = MoveTemp(Proof);
			++ProofFenceValue;
			D3D12RHI->RHISignalManualFence(RHICmdList, ProofFence.Get(), ProofFenceValue);
		}
	}
#else
	bool Initialize(FCore&, const TArray<FCore::FWaterCascade>&, FString& Error)
	{
		Error = TEXT("direct BF6 DXIL execution requires Windows D3D12");
		return false;
	}
	void ResetTextures() {}
	bool TryQueueProof() { return false; }
	FString GetProofText() const { return TEXT("GPU proof: unavailable without Windows D3D12"); }
#endif
};

FWaterFFT::~FWaterFFT() { Reset(); }

UTexture2D* FWaterFFT::MakeTexture(int32 N, const TCHAR* Name)
{
	// A FULL MIP CHAIN, and the reason it has to exist.
	//
	// UTexture2D::CreateTransient makes ONE level. With one level every sample
	// is the full-resolution field no matter how coarse the geometry under it
	// is, and the draw tree hands us tiles whose vertex spacing runs from 1 m to
	// 2048 m. Cascade 0's waves are about 100 m long, so on the coarse tiles
	// they are sampled far below Nyquist and alias into thin radiating slivers
	// rather than resolving into distant swell. That is the artefact.
	//
	// The game solves it in its VERTEX shader with an explicit, camera-distance
	// driven mip: log2(k * log2(d/d0)) (water-draw-shaders-decoded section 3).
	// That instruction is meaningless against a single-level texture, which is
	// why the ramp could not simply be ported. Build the chain first.
	//
	// Mipping is also the RIGHT filter here rather than fading a cascade out: a
	// cascade spans a band of wavelengths, and a mip discards only the part
	// below the mesh's Nyquist while keeping the long swell that the coarse tile
	// CAN represent. Fading the whole cascade would flatten the horizon instead.
	const int32 NumMips = FMath::Max(1, (int32)FMath::FloorLog2((uint32)N) + 1);
	UTexture2D* T = NewObject<UTexture2D>(GetTransientPackage(), FName(Name), RF_Transient);
	if (!T) return nullptr;

	FTexturePlatformData* PD = new FTexturePlatformData();
	PD->SizeX = N;
	PD->SizeY = N;
	PD->PixelFormat = PF_FloatRGBA;
	T->SetPlatformData(PD);
	for (int32 m = 0; m < NumMips; ++m)
	{
		const int32 S = FMath::Max(1, N >> m);
		FTexture2DMipMap* Mip = new FTexture2DMipMap();
		Mip->SizeX = S;
		Mip->SizeY = S;
		Mip->SizeZ = 1;
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		void* Dst = Mip->BulkData.Realloc((int64)S * (int64)S * sizeof(FFloat16Color));
		FMemory::Memzero(Dst, (SIZE_T)S * (SIZE_T)S * sizeof(FFloat16Color));
		Mip->BulkData.Unlock();
		PD->Mips.Add(Mip);
	}

	T->SRGB = false;
	T->NeverStream = true;
	T->CompressionSettings = TC_HDR;
	T->AddressX = TA_Wrap;
	T->AddressY = TA_Wrap;
	T->Filter = TF_Bilinear;
	T->AddToRoot();
	T->UpdateResource();
	return T;
}

bool FWaterFFT::Initialize(FCore& Core, const TArray<FCore::FWaterCascade>& Inputs)
{
	Reset();
	(void)Core;
	// RETRACTED/QUARANTINED: the native D3D12 escape-hatch path is not a valid
	// Unreal resource path.  On UE 5.8 it failed its own independent numerical
	// replay (rel-L2 1.21519917), then reload produced an RHI-thread assertion
	// and a DMA page fault / DXGI_ERROR_DEVICE_HUNG in compute_02.  Never create
	// or bind those native resources from the user-facing lab.  The installed
	// DXIL decoder remains research code above, but only the engine-owned CPU
	// replay textures are allowed into the material until an RDG/RHI-native
	// implementation passes both the real and zero-H0 controls.
	Direct.Reset();
	UE_LOG(LogBF6WaterFFT, Display,
		TEXT("installed-DXIL dispatch quarantined after failed numeric proof and GPU page fault; using engine-owned CPU replay textures"));
	for (int32 Index = 0; Index < Inputs.Num() && Index < 4; ++Index)
	{
		const FCore::FWaterCascade& In = Inputs[Index];
		if (In.Resolution < 16 || !FMath::IsPowerOfTwo(In.Resolution) ||
			In.H0.Num() != In.Resolution * In.Resolution || In.TileDimension <= 0.f)
		{
			UE_LOG(LogBF6WaterFFT, Error,
				TEXT("cascade %d rejected: N=%d H0=%d tile=%g"),
				Index, In.Resolution, In.H0.Num(), In.TileDimension);
			continue;
		}
		FCascade C;
		C.N = In.Resolution;
		C.TileM = In.TileDimension;
		C.Choppiness = In.Choppiness;
		C.bFoam = In.bFoamEnabled;
		C.FoamThreshold = In.FoamThreshold;
		C.FoamMax = In.FoamMax;
		C.FoamHalfLife = In.FoamHalfLife;
		const int32 Count = C.N * C.N;
		C.H0.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i) C.H0[i] = { In.H0[i].X, In.H0[i].Y };
		C.Height.SetNumUninitialized(Count);
		C.DispX.SetNumUninitialized(Count);
		C.DispY.SetNumUninitialized(Count);
		C.LastDisplacement.SetNumUninitialized(Count);
		C.FoamPrevious.Init(0.f, Count);
		C.FoamWork.Init(0.f, Count);
		C.Displacement = MakeTexture(C.N,
			*FString::Printf(TEXT("BF6_WaterDisp_%d"), Index));
		C.NormalFoam = MakeTexture(C.N,
			*FString::Printf(TEXT("BF6_WaterNormalFoam_%d"), Index));
		if (!C.Displacement || !C.NormalFoam)
		{
			if (C.Displacement) { C.Displacement->RemoveFromRoot(); C.Displacement = nullptr; }
			if (C.NormalFoam) { C.NormalFoam->RemoveFromRoot(); C.NormalFoam = nullptr; }
			continue;
		}
		UE_LOG(LogBF6WaterFFT, Log,
			TEXT("cascade %d live: source %d, %dx%d, tile %.3fm, chop %.3f, foam %s max %.3f"),
			Index, In.SourceIndex, C.N, C.N, C.TileM, C.Choppiness,
			C.bFoam ? TEXT("on") : TEXT("off"), C.FoamMax);
		Cascades.Add(MoveTemp(C));
	}
	if (!Cascades.IsEmpty())
	{
		// t=0 is deterministic and provides valid textures before the first
		// editor ticker. The repeated run is the control used by the core test.
		for (FCascade& C : Cascades)
		{
			Evolve(C, 0.f, TimeSeconds);
			Upload(C.Displacement, C.DisplacementPixels, C.N);
			Upload(C.NormalFoam, C.NormalPixels, C.N);
		}
	}
	return !Cascades.IsEmpty();
}

void FWaterFFT::Reset()
{
	// A job owns only copied numeric inputs, never UObjects or this instance.
	// Join this one bounded calculation before module code can be unloaded.
	if (PendingFrame.IsValid()) { PendingFrame.Wait(); PendingFrame = {}; }
	PendingSeconds = 0.f;
	if (Direct)
	{
		// Native D3D12 resources cannot be released merely because their last
		// recording lambda has executed: the GPU may still be consuming that
		// command list.  Reload used to drop the ComPtrs here and could page-fault
		// in compute_02.  Retire all previously queued work on the render thread,
		// wait for the GPU once (reload/shutdown only), and only then detach the
		// UObject aliases and release the native state.
		if (!IsEngineExitRequested())
		{
			check(IsInGameThread());
			const TSharedPtr<FDirectState, ESPMode::ThreadSafe> Retiring = Direct;
			ENQUEUE_RENDER_COMMAND(BF6RetireInstalledWaterDXIL)(
				[Retiring](FRHICommandListImmediate& RHICmdList)
				{
					RHICmdList.SubmitAndBlockUntilGPUIdle();
				});
			FlushRenderingCommands();
			Direct->ResetTextures();
			// ReleaseResource is itself enqueued.  Complete it while the rooted
			// UTexture objects and their FTextureResource wrappers are still alive.
			FlushRenderingCommands();
		}
		Direct.Reset();
	}
	// Module shutdown can occur after GUObjectArray has begun tearing down. At
	// that point even dereferencing a formerly valid rooted texture asserts in
	// IndexToObject; the process is already releasing every UObject, so only do
	// explicit root removal during a live engine reset/rebuild.
	if (!IsEngineExitRequested())
	{
		for (FCascade& C : Cascades)
		{
			if (C.Displacement) C.Displacement->RemoveFromRoot();
			if (C.NormalFoam) C.NormalFoam->RemoveFromRoot();
		}
	}
	Cascades.Reset();
	TimeSeconds = 0.f;
}

void FWaterFFT::FFT1D(FComplex* V, int32 N)
{
	for (int32 i = 1, j = 0; i < N; ++i)
	{
		int32 Bit = N >> 1;
		for (; j & Bit; Bit >>= 1) j ^= Bit;
		j ^= Bit;
		if (i < j) Swap(V[i], V[j]);
	}
	for (int32 Len = 2; Len <= N; Len <<= 1)
	{
		const float Angle = 2.f * PI / Len; // inverse transform, unnormalised
		const float WC = FMath::Cos(Angle), WS = FMath::Sin(Angle);
		for (int32 i = 0; i < N; i += Len)
		{
			float C = 1.f, S = 0.f;
			for (int32 j = 0; j < Len / 2; ++j)
			{
				const FComplex A = V[i + j];
				const FComplex B = Mul(V[i + j + Len / 2], C, S);
				V[i + j] = { A.R + B.R, A.I + B.I };
				V[i + j + Len / 2] = { A.R - B.R, A.I - B.I };
				const float NC = C * WC - S * WS;
				S = C * WS + S * WC;
				C = NC;
			}
		}
	}
}

void FWaterFFT::FFT2D(TArray<FComplex>& V, int32 N)
{
	for (int32 y = 0; y < N; ++y) FFT1D(V.GetData() + y * N, N);
	TArray<FComplex> Column;
	Column.SetNumUninitialized(N);
	for (int32 x = 0; x < N; ++x)
	{
		for (int32 y = 0; y < N; ++y) Column[y] = V[y * N + x];
		FFT1D(Column.GetData(), N);
		for (int32 y = 0; y < N; ++y) V[y * N + x] = Column[y];
	}
}

void FWaterFFT::Upload(UTexture2D* Texture, const TArray<FFloat16Color>& Pixels, int32 N)
{
	if (!Texture || Pixels.Num() != N * N) return;

	// Push mip 0, then each halved level, so a coarse tile can ask for a
	// pre-filtered version of the field instead of aliasing the full-rate one.
	//
	// A plain 2x2 box average is the correct filter for these payloads: the
	// displacement channels are a linear field, the encoded normal is linear in
	// its components, and foam coverage is a linear coverage fraction. The whole
	// chain costs about a third again over mip 0, on fields no larger than
	// 128x128, so it stays on the CPU beside the evolve that produced them.
	auto Push = [Texture](const FFloat16Color* Src, int32 Side, int32 MipIndex)
	{
		const SIZE_T Bytes = (SIZE_T)Side * (SIZE_T)Side * sizeof(FFloat16Color);
		uint8* Copy = new uint8[Bytes];
		FMemory::Memcpy(Copy, Src, Bytes);
		FUpdateTextureRegion2D* Region =
			new FUpdateTextureRegion2D(0, 0, 0, 0, Side, Side);
		Texture->UpdateTextureRegions(MipIndex, 1, Region,
			Side * sizeof(FFloat16Color), sizeof(FFloat16Color), Copy,
			[](uint8* Data, const FUpdateTextureRegion2D* R)
			{
				delete[] Data;
				delete R;
			});
	};

	Push(Pixels.GetData(), N, 0);

	const int32 NumMips = FMath::Max(1, (int32)FMath::FloorLog2((uint32)N) + 1);
	TArray<FFloat16Color> Cur(Pixels);
	TArray<FFloat16Color> Next;
	int32 Side = N;
	for (int32 m = 1; m < NumMips; ++m)
	{
		const int32 Half = FMath::Max(1, Side >> 1);
		Next.SetNumUninitialized(Half * Half);
		for (int32 y = 0; y < Half; ++y)
			for (int32 x = 0; x < Half; ++x)
			{
				const int32 sx = x * 2, sy = y * 2;
				const int32 sx1 = FMath::Min(sx + 1, Side - 1);
				const int32 sy1 = FMath::Min(sy + 1, Side - 1);
				const FLinearColor P00 = Cur[sy * Side + sx].GetFloats();
				const FLinearColor P10 = Cur[sy * Side + sx1].GetFloats();
				const FLinearColor P01 = Cur[sy1 * Side + sx].GetFloats();
				const FLinearColor P11 = Cur[sy1 * Side + sx1].GetFloats();
				Next[y * Half + x] = FFloat16Color((P00 + P10 + P01 + P11) * 0.25f);
			}
		Push(Next.GetData(), Half, m);
		Cur = Next;
		Side = Half;
	}
}

void FWaterFFT::Evolve(FCascade& C, float DeltaSeconds, float SimulationTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(BF6WaterCompute);
	const int32 N = C.N;
	const int32 Count = N * N;
	const float Pi = 3.1415927410125732421875f;
	for (int32 y = 0; y < N; ++y)
	{
		for (int32 x = 0; x < N; ++x)
		{
			const int32 I = y * N + x;
			const int32 Mirror = Wrap(N - y, N) * N + Wrap(N - x, N);
			const float Kx = ((float)(x + x) - N) * -Pi / C.TileM;
			const float Ky = ((float)(y + y) - N) *  Pi / C.TileM;
			const float K = FMath::Sqrt(Kx * Kx + Ky * Ky);
			const float Phase = FMath::Sqrt(9.8f * K) * SimulationTime;
			const float Co = FMath::Cos(Phase), Si = FMath::Sin(Phase);
			const FComplex A = Mul(C.H0[I], Co, Si);
			const FComplex HM = { C.H0[Mirror].R, -C.H0[Mirror].I };
			const FComplex B = Mul(HM, Co, -Si);
			const FComplex H = { A.R + B.R, A.I + B.I };
			C.Height[I] = H;
			const float InvK = 1.f / (K + 1.0e-5f);
			const float FX = Kx * InvK, FY = Ky * InvK;
			C.DispX[I] = { H.I * FX, -H.R * FX }; // -i*kx/k*h
			C.DispY[I] = { H.I * FY, -H.R * FY };
		}
	}
	FFT2D(C.Height, N);
	FFT2D(C.DispX, N);
	FFT2D(C.DispY, N);

	TArray<FVector3f>& D = C.LastDisplacement;
	for (int32 y = 0; y < N; ++y)
		for (int32 x = 0; x < N; ++x)
		{
			const int32 I = y * N + x;
			const float Sign = ((x + y) & 1) ? -1.f : 1.f;
			D[I] = FVector3f(Sign * C.DispX[I].R,
				Sign * C.Height[I].R, Sign * C.DispY[I].R);
		}

	const float DerivativeScale = (float)N / (2.f * C.TileM);
	const float Lag = (C.bFoam && C.FoamHalfLife > 0.f)
		? FMath::Pow(0.5f, DeltaSeconds / C.FoamHalfLife) : 0.f;
	// THE FOAM GAIN IS NOT FoamMaxValue. Read from the live executable's
	// WaterDiffConstants fill (research report foam.md 2.2, machine-code fact):
	//     cb0.w = 1 / ((1 - 0.5^(1/FoamHalfLife)) * FoamMaxValue)
	// with no guard, so the 21 MP sims that author FoamMaxValue = 0 produce
	// +inf that the draw shader's saturate turns into zero foam. The old code
	// multiplied by FoamMaxValue instead, which on Granite (T such that the
	// normaliser is ~0.019) is wrong by a factor of 53. The guard reproduces the
	// OUTCOME of the unguarded division, which is "no foam".
	const float FoamNorm = (C.FoamHalfLife > 0.f)
		? (1.f - FMath::Pow(0.5f, 1.f / C.FoamHalfLife)) : 1.f;
	const float FoamGain = (C.bFoam && C.FoamMax > 0.f && FoamNorm > 1e-6f)
		? 1.f / (FoamNorm * C.FoamMax) : 0.f;
	for (int32 y = 0; y < N; ++y)
		for (int32 x = 0; x < N; ++x)
		{
			const int32 I = y * N + x;
			const FVector3f& L = D[y * N + Wrap(x - 1, N)];
			const FVector3f& R = D[y * N + Wrap(x + 1, N)];
			const FVector3f& B = D[Wrap(y - 1, N) * N + x];
			const FVector3f& T = D[Wrap(y + 1, N) * N + x];
			const float Fold = DerivativeScale * ((R.X - L.X) + (T.Z - B.Z));
			const float Instant = FMath::Max(0.f, Fold - C.FoamThreshold) * FoamGain;
			C.FoamWork[I] = FMath::Lerp(Instant, C.FoamPrevious[I], Lag);
		}

	TArray<FFloat16Color>& DisplacementPixels = C.DisplacementPixels;
	TArray<FFloat16Color>& NormalPixels = C.NormalPixels;
	DisplacementPixels.SetNumUninitialized(Count);
	NormalPixels.SetNumUninitialized(Count);
	for (int32 y = 0; y < N; ++y)
		for (int32 x = 0; x < N; ++x)
		{
			const int32 I = y * N + x;
			const FVector3f& L = D[y * N + Wrap(x - 1, N)];
			const FVector3f& R = D[y * N + Wrap(x + 1, N)];
			const FVector3f& B = D[Wrap(y - 1, N) * N + x];
			const FVector3f& T = D[Wrap(y + 1, N) * N + x];
			const FVector3f Normal = FVector3f(
				-DerivativeScale * (R.Y - L.Y),
				-DerivativeScale * (T.Y - B.Y), 1.f).GetSafeNormal();
			float Foam = 0.f;
			for (int32 oy = -1; oy <= 1; ++oy)
				for (int32 ox = -1; ox <= 1; ++ox)
				{
					const float W = (ox == 0 ? 2.f : 1.f) * (oy == 0 ? 2.f : 1.f);
					Foam += W * C.FoamWork[Wrap(y + oy, N) * N + Wrap(x + ox, N)];
				}
			Foam *= 1.f / 16.f;
			DisplacementPixels[I] = FFloat16Color(FLinearColor(D[I].X, D[I].Y, D[I].Z, 0.f));
			NormalPixels[I] = FFloat16Color(FLinearColor(Foam,
				Normal.X * 0.5f + 0.5f, Normal.Y * 0.5f + 0.5f, 1.f));
		}
	C.FoamPrevious = C.FoamWork;
}

void FWaterFFT::Tick(float DeltaSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(BF6WaterTick);
	if (Cascades.IsEmpty()) return;
	PendingSeconds += FMath::Clamp(DeltaSeconds, 0.f, 0.1f);
	if (PendingFrame.IsValid())
	{
		if (!PendingFrame.IsReady()) return; // Keep displaying the last complete frame; never queue a backlog.
		auto Frame = PendingFrame.Get(); PendingFrame = {};
		check(Frame->Cascades.Num() == Cascades.Num());
		for (int32 I = 0; I < Cascades.Num(); ++I)
		{
			UTexture2D* Displacement = Cascades[I].Displacement;
			UTexture2D* Normal = Cascades[I].NormalFoam;
			Cascades[I] = MoveTemp(Frame->Cascades[I]);
			Cascades[I].Displacement = Displacement; Cascades[I].NormalFoam = Normal;
			Upload(Displacement, Cascades[I].DisplacementPixels, Cascades[I].N);
			Upload(Normal, Cascades[I].NormalPixels, Cascades[I].N);
		}
		TimeSeconds = Frame->Seconds; // Proofs and diagnostics describe the published textures.
	}
	if (PendingSeconds > 0.f)
	{
		const float Step = PendingSeconds; PendingSeconds = 0.f;
		if (!Direct && CVarWaterAsync.GetValueOnGameThread() != 0)
		{
			TSharedPtr<FAsyncFrame, ESPMode::ThreadSafe> Frame = MakeShared<FAsyncFrame, ESPMode::ThreadSafe>();
			Frame->Seconds = TimeSeconds + Step;
			Frame->Cascades = Cascades;
			for (FCascade& C : Frame->Cascades) { C.Displacement = nullptr; C.NormalFoam = nullptr; }
			PendingFrame = Async(EAsyncExecution::ThreadPool, [Frame, Step]
			{
				for (FCascade& C : Frame->Cascades) Evolve(C, Step, Frame->Seconds);
				return Frame;
			});
		}
		else
		{
			TimeSeconds += Step;
			for (FCascade& C : Cascades)
			{
				Evolve(C, Step, TimeSeconds);
				Upload(C.Displacement, C.DisplacementPixels, C.N);
				Upload(C.NormalFoam, C.NormalPixels, C.N);
			}
		}
	}

	// HEADLESS DIAGNOSTIC. The one number that separates "the sea is wrong" from
	// "the sea is not moving": the height field's RMS straight out of the CPU
	// replay, in metres, before the material ever sees it.
	//
	// The transform is an UNNORMALISED inverse (see FFT1D), so this is directly
	// comparable to the offline Parseval figure of 4*sqrt(sum|H0|^2), which was
	// itself validated at 0.318 m against 0.314 m measured here at wind 12. If
	// this line reports metres, the displacement exists and anything flat on
	// screen is lost downstream in the material or the gate. If it reports
	// millimetres, the loss is upstream and the replay is the problem.
	static float GDiagAccum = 0.f;
	GDiagAccum += DeltaSeconds;
	if (GDiagAccum >= 60.f)
	{
		GDiagAccum = 0.f;
		FString Line;
		for (int32 i = 0; i < Cascades.Num(); ++i)
		{
			const FCascade& C = Cascades[i];
			const int32 N = C.LastDisplacement.Num();
			if (N <= 0) { Line += FString::Printf(TEXT(" [%d NEVER EVOLVED]"), i); continue; }
			double SumSq = 0.0;
			float PeakY = 0.f;
			for (const FVector3f& D : C.LastDisplacement)
			{
				SumSq += (double)D.Y * (double)D.Y;
				PeakY = FMath::Max(PeakY, FMath::Abs(D.Y));
			}
			const float Rms = (float)FMath::Sqrt(SumSq / (double)N);
			Line += FString::Printf(TEXT("  c%d(%.0fm) Hs %.2f peak %.2f"),
				i, C.TileM, 4.f * Rms, PeakY);
		}
		UE_LOG(LogBF6WaterFFT, Display, TEXT("DIAG metres:%s"), *Line);
	}
	if (Direct)
	{
		const TSharedPtr<FDirectState, ESPMode::ThreadSafe> State = Direct;
		const float Seconds = TimeSeconds;
		TSharedPtr<FDirectState::FProofRequest, ESPMode::ThreadSafe> Proof;
		if (State->TryQueueProof())
		{
			Proof = MakeShared<FDirectState::FProofRequest, ESPMode::ThreadSafe>();
			Proof->Seconds = Seconds;
			Proof->Expected.Reserve(Cascades.Num());
			Proof->bZeroInput = true;
			for (const FCascade& C : Cascades)
			{
				Proof->Expected.Add(C.LastDisplacement);
				for (const FComplex& H : C.H0)
					if (H.R != 0.f || H.I != 0.f) { Proof->bZeroInput = false; break; }
			}
		}
		ENQUEUE_RENDER_COMMAND(BF6DispatchInstalledWaterDXIL)(
			[State, Seconds, Proof](FRHICommandListImmediate& RHICmdList)
			{
				// The D3D12 native command-list escape hatch is a bottom-of-pipe API.
				// Record an explicit graphics-pipeline scope first so the executing
				// command list has the compute-capable graphics context it requires.
				FRHICommandListScopedPipeline PipelineScope(
					RHICmdList, ERHIPipeline::Graphics);
				RHICmdList.EnqueueLambda(TEXT("BF6DirectWaterDXIL"),
					[State, Seconds, Proof](FRHICommandListImmediate& ExecutingCmdList)
					{
						State->Dispatch(ExecutingCmdList, Seconds, Proof);
					});
				if (Proof)
				{
					// The native lambda above executes on the RHI thread. Submit and
					// wait from this enclosing render-thread command, then read back.
					// This is a one-shot proof path; normal animation never blocks.
					RHICmdList.SubmitAndBlockUntilGPUIdle();
					State->PollProof();
				}
			});
	}
}

bool FWaterFFT::IsDirectReady() const
{
	return Direct.IsValid() && Direct->Cascades.Num() == Cascades.Num();
}

FString FWaterFFT::ProofSummary() const
{
	return Direct ? Direct->GetProofText()
		: TEXT("GPU proof: DIRECT DXIL QUARANTINED (failed numeric proof + device-hung control); CPU replay active");
}

UTexture2D* FWaterFFT::DisplacementTexture(int32 Index) const
{
	return Cascades.IsValidIndex(Index) ? Cascades[Index].Displacement : nullptr;
}

bool FWaterFFT::GetCascadeDisplacementForProof(int32 Index, TArray<FVector3f>& OutDisp,
	bool& bOutFoam, float& OutFoamThreshold, float& OutFoamMax) const
{
	if (!Cascades.IsValidIndex(Index)) return false;
	const FCascade& C = Cascades[Index];
	if (C.LastDisplacement.Num() != C.N * C.N) return false;
	OutDisp = C.LastDisplacement;
	bOutFoam = C.bFoam;
	OutFoamThreshold = C.FoamThreshold;
	OutFoamMax = C.FoamMax;
	return true;
}

void FWaterFFT::ReferenceFFT2D(TArray<FVector2f>& InOut, int32 N)
{
	if (N <= 0 || InOut.Num() != N * N) return;
	TArray<FComplex> Work;
	Work.SetNumUninitialized(InOut.Num());
	for (int32 i = 0; i < InOut.Num(); ++i)
		Work[i] = FComplex{ InOut[i].X, InOut[i].Y };
	FFT2D(Work, N);
	for (int32 i = 0; i < InOut.Num(); ++i)
		InOut[i] = FVector2f(Work[i].R, Work[i].I);
}

bool FWaterFFT::GetCascadeForProof(int32 Index, int32& OutN, float& OutTileM,
	float& OutTime, TArray<FVector2f>& OutH0) const
{
	if (!Cascades.IsValidIndex(Index)) return false;
	const FCascade& C = Cascades[Index];
	if (C.N <= 0 || C.H0.Num() != C.N * C.N) return false;
	OutN = C.N;
	OutTileM = C.TileM;
	OutTime = TimeSeconds;
	OutH0.SetNumUninitialized(C.H0.Num());
	for (int32 i = 0; i < C.H0.Num(); ++i)
		OutH0[i] = FVector2f(C.H0[i].R, C.H0[i].I);
	return true;
}

void FWaterFFT::PeakVerticalPerCascade(float* Out, int32 OutMax) const
{
	if (!Out || OutMax <= 0) return;
	for (int32 i = 0; i < OutMax; ++i) Out[i] = 0.f;
	for (int32 i = 0; i < Cascades.Num() && i < OutMax; ++i)
	{
		float Peak = 0.f;
		for (const FVector3f& D : Cascades[i].LastDisplacement)
			Peak = FMath::Max(Peak, FMath::Abs(D.Y));
		Out[i] = Peak;
	}
}

FString FWaterFFT::DisplacementSummary() const
{
	if (Cascades.IsEmpty()) return TEXT("water displacement: no cascades");
	FString Out = TEXT("water displacement, peak per cascade from the last evolve "
	                   "(BEFORE shore and CoarseMask attenuation):");
	float Worst = 0.f;
	for (int32 i = 0; i < Cascades.Num(); ++i)
	{
		const FCascade& C = Cascades[i];
		float PeakV = 0.f, PeakH = 0.f;
		for (const FVector3f& D : C.LastDisplacement)
		{
			PeakV = FMath::Max(PeakV, FMath::Abs(D.Y));
			PeakH = FMath::Max(PeakH, FMath::Max(FMath::Abs(D.X), FMath::Abs(D.Z)));
		}
		Worst = FMath::Max(Worst, PeakV);
		Out += FString::Printf(
			TEXT("\n  cascade %d  tile %7.1f m  res %3d  vertical %8.4f m (%7.1f mm)  horizontal %8.4f m%s"),
			i, C.TileM, C.N, PeakV, PeakV * 1000.f, PeakH,
			C.LastDisplacement.IsEmpty() ? TEXT("  [never evolved]") : TEXT(""));
	}
	Out += FString::Printf(
		TEXT("\n  tallest cascade %.4f m (%.1f mm). The sea on screen is this times the "
		     "wave amplitude scale times the shore/mask attenuation, so a large number "
		     "here with a flat surface means the attenuation, not the spectrum."),
		Worst, Worst * 1000.f);
	return Out;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterAsyncTest, "BF6.HighPoly.Water.AsyncReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWaterAsyncTest::RunTest(const FString&)
{
	const int32 Previous = CVarWaterAsync.GetValueOnGameThread();
	CVarWaterAsync->Set(1, ECVF_SetByCode);
	ON_SCOPE_EXIT { CVarWaterAsync->Set(Previous, ECVF_SetByCode); };
	for (bool Zero : { true, false })
	{
		FWaterFFT::FCascade Seed;
		Seed.N = 16; Seed.TileM = 64.f; Seed.Choppiness = 1.f;
		Seed.bFoam = true; Seed.FoamThreshold = -0.1f; Seed.FoamMax = 0.8f; Seed.FoamHalfLife = 2.f;
		const int32 Count = Seed.N * Seed.N;
		Seed.H0.SetNumZeroed(Count); Seed.Height.SetNumZeroed(Count);
		Seed.DispX.SetNumZeroed(Count); Seed.DispY.SetNumZeroed(Count);
		Seed.LastDisplacement.SetNumZeroed(Count);
		Seed.FoamPrevious.SetNumZeroed(Count); Seed.FoamWork.SetNumZeroed(Count);
		if (!Zero) for (int32 I = 0; I < Count; ++I)
			Seed.H0[I] = { float(I % 7 - 3) * 0.0001f, float(I % 11 - 5) * 0.0001f };
		FWaterFFT Worker; Worker.Cascades.Add(Seed);
		auto Expected = Seed;
		float Seconds = 0.f;
		for (int32 Step = 0; Step < 12; ++Step)
		{
			const float Delta = 1.f / 30.f; Seconds += Delta;
			FWaterFFT::Evolve(Expected, Delta, Seconds);
			Worker.Tick(Delta);
			TestTrue(TEXT("One pending compute job"), Worker.PendingFrame.IsValid());
			Worker.PendingFrame.Wait(); Worker.Tick(0.f);
			TestFalse(TEXT("Publishing without elapsed time does not queue work"), Worker.PendingFrame.IsValid());
			TestEqual(TEXT("Published proof time matches simulation"), Worker.TimeSeconds, Seconds);
			const auto& Actual = Worker.Cascades[0];
			TestEqual(TEXT("Worker displacement matches synchronous replay exactly"),
				FMemory::Memcmp(Actual.LastDisplacement.GetData(), Expected.LastDisplacement.GetData(), Count * sizeof(FVector3f)), 0);
			TestEqual(TEXT("Foam history survives frame publication"),
				FMemory::Memcmp(Actual.FoamPrevious.GetData(), Expected.FoamPrevious.GetData(), Count * sizeof(float)), 0);
			TestEqual(TEXT("Displacement texture pixels match"),
				FMemory::Memcmp(Actual.DisplacementPixels.GetData(), Expected.DisplacementPixels.GetData(), Count * sizeof(FFloat16Color)), 0);
			TestEqual(TEXT("Normal and foam texture pixels match"),
				FMemory::Memcmp(Actual.NormalPixels.GetData(), Expected.NormalPixels.GetData(), Count * sizeof(FFloat16Color)), 0);
		}
		Worker.Tick(0.03f); Worker.Reset();
		TestFalse(TEXT("Reset drains outstanding worker ownership"), Worker.PendingFrame.IsValid());
		TestFalse(TEXT("Reset discards the old simulation"), Worker.IsReady());
	}
	return true;
}
#endif

void FWaterFFT::Bind(UMaterialInstanceDynamic* MID, float WaveAmplitudeScale) const
{
	if (!MID) return;
	MID->SetScalarParameterValue(TEXT("BF6FFTEnabled"), Cascades.IsEmpty() ? 0.f : 1.f);
	MID->SetScalarParameterValue(TEXT("BF6WaveAmplitudeScale"), WaveAmplitudeScale);
	// Texel size in metres per cascade, so the vertex shader can turn a tile's
	// spacing into a mip index. A cascade that is absent leaves a large value,
	// which asks for the coarsest mip rather than the sharpest.
	FLinearColor TexelM(1.f, 1.f, 1.f, 1.f);
	for (int32 i = 0; i < 4; ++i)
	{
		if (!Cascades.IsValidIndex(i) || Cascades[i].N <= 0) continue;
		TexelM.Component(i) = Cascades[i].TileM / (float)Cascades[i].N;
	}
	MID->SetVectorParameterValue(TEXT("BF6CascadeTexelM"), TexelM);
	MID->SetScalarParameterValue(TEXT("BF6CascadeTexelM3"), TexelM.A);
	UE_LOG(LogBF6WaterFFT, Display,
		TEXT("BIND %s: BF6FFTEnabled=%.1f BF6WaveAmplitudeScale=%.4f cascades=%d "
		     "displacement_source=%s"),
		*MID->GetName(), Cascades.IsEmpty() ? 0.f : 1.f, WaveAmplitudeScale,
		Cascades.Num(), IsDirectReady() ? TEXT("DIRECT-DXIL") : TEXT("CPU-REPLAY"));
	for (int32 i = 0; i < 4; ++i)
	{
		const FString DispName = FString::Printf(TEXT("BF6Disp%d"), i);
		const FString NormName = FString::Printf(TEXT("BF6NormalFoam%d"), i);
		const FString MetaName = FString::Printf(TEXT("BF6Cascade%d"), i);
		if (Cascades.IsValidIndex(i))
		{
			const FCascade& C = Cascades[i];
			UTexture* DisplacementTexture = C.Displacement;
			if (Direct && Direct->Cascades.IsValidIndex(i) && Direct->Cascades[i].Texture)
				DisplacementTexture = Direct->Cascades[i].Texture;
			MID->SetTextureParameterValue(*DispName, DisplacementTexture);
			MID->SetTextureParameterValue(*NormName, C.NormalFoam);
			MID->SetVectorParameterValue(*MetaName,
				FLinearColor(C.TileM, C.Choppiness, 1.f, C.bFoam ? 1.f : 0.f));
		}
		else MID->SetVectorParameterValue(*MetaName, FLinearColor(1.f, 0.f, 0.f, 0.f));
	}
}
}
