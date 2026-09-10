#include "BF6HighPolyTextureStreaming.h"

#include "Engine/Texture2D.h"
#include "ContentStreaming.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Streaming/TextureMipDataProvider.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBF6MipStreaming, Log, All);

struct FBF6MipSource
{
	struct FMip { int64 Offset = 0, Bytes = 0; uint32 RowBytes = 0, Rows = 0; };
	FString Path;
	TArray<FMip> Mips;
	~FBF6MipSource()
	{
		// Only our unique, individually owned file, after the last worker releases it.
		if (!Path.IsEmpty()) IFileManager::Get().Delete(*Path, false, true);
	}

	bool Read(IFileHandle& File, int32 Index, void* Dest, uint64 Capacity, uint32 Pitch = 0) const
	{
		if (!Mips.IsValidIndex(Index) || !Dest) return false;
		const FMip& Mip = Mips[Index];
		if (!Pitch) Pitch = Mip.RowBytes;
		const uint64 Required = uint64(Pitch) * (Mip.Rows - 1) + Mip.RowBytes;
		if (Pitch < Mip.RowBytes || (Capacity && Capacity < Required) || !File.Seek(Mip.Offset)) return false;
		if (Pitch == Mip.RowBytes) return File.Read(static_cast<uint8*>(Dest), Mip.Bytes);
		for (uint32 Row = 0; Row < Mip.Rows; ++Row)
			if (!File.Read(static_cast<uint8*>(Dest) + uint64(Row) * Pitch, Mip.RowBytes)) return false;
		return true;
	}
};

namespace
{
	class FBF6MipRequest final : public FTextureMipDataProvider
	{
		TSharedPtr<FBF6MipSource, ESPMode::ThreadSafe> Source;
		bool bOk = true;
	public:
		FBF6MipRequest(UTexture* Texture, TSharedPtr<FBF6MipSource, ESPMode::ThreadSafe> InSource)
			: FTextureMipDataProvider(Texture, ETickState::Init, ETickThread::Async), Source(MoveTemp(InSource)) {}
		virtual void Init(const FTextureUpdateContext&, const FTextureUpdateSyncOptions&) override
		{ AdvanceTo(ETickState::GetMips, ETickThread::Async); }
		virtual int32 GetMips(const FTextureUpdateContext&, int32 First,
			const FTextureMipInfoArray& Infos, const FTextureUpdateSyncOptions&) override
		{
			TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Source->Path));
			bOk = File.IsValid();
			for (int32 I = First; bOk && I < CurrentFirstLODIdx; ++I)
			{
				bOk = Infos.IsValidIndex(I) && Source->Read(*File, I + ResourceState.AssetLODBias,
					Infos[I].DestData, Infos[I].DataSize, Infos[I].RowPitch);
			}
			AdvanceTo(ETickState::PollMips, ETickThread::Async);
			// A failed read aborts this update. Never let DDC supply unrelated bytes.
			return CurrentFirstLODIdx;
		}
		virtual bool PollMips(const FTextureUpdateSyncOptions&) override
		{
			if (!bOk) UE_LOG(LogBF6MipStreaming, Warning, TEXT("Texture mip read failed; retaining resident detail"));
			Source.Reset();
			AdvanceTo(ETickState::Done, ETickThread::None);
			return bOk;
		}
		virtual void CleanUp(const FTextureUpdateSyncOptions&) override
		{ AdvanceTo(ETickState::Done, ETickThread::None); }
		virtual void Cancel(const FTextureUpdateSyncOptions&) override { Source.Reset(); }
		virtual ETickThread GetCancelThread() const override
		{ return Source ? ETickThread::Async : ETickThread::None; }
	};
}

bool UBF6HighPolyMipProvider::Attach(UTexture2D* Texture, const TArray<uint8>& Bytes)
{
	FTexturePlatformData* PD = Texture ? Texture->GetPlatformData() : nullptr;
	if (!PD || PD->Mips.Num() < 2) return false;
	const FPixelFormatInfo& Format = GPixelFormats[PD->PixelFormat];
	if (!Format.BlockSizeX || !Format.BlockSizeY || !Format.BlockBytes) return false;
	auto NewSource = MakeShared<FBF6MipSource, ESPMode::ThreadSafe>();
	int64 Offset = 0;
	for (const FTexture2DMipMap& Mip : PD->Mips)
	{
		FBF6MipSource::FMip Info;
		Info.Offset = Offset;
		Info.RowBytes = FMath::DivideAndRoundUp(uint32(Mip.SizeX), uint32(Format.BlockSizeX)) * Format.BlockBytes;
		Info.Rows = FMath::DivideAndRoundUp(uint32(Mip.SizeY), uint32(Format.BlockSizeY));
		Info.Bytes = int64(Info.RowBytes) * Info.Rows;
		if (!Info.Rows || !Info.RowBytes || Info.Bytes > Bytes.Num() - Offset) return false;
		Offset += Info.Bytes;
		NewSource->Mips.Add(Info);
	}
	if (Offset != Bytes.Num()) return false;
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("BF6UnrealSDK/HighPoly/Streaming");
	if (!IFileManager::Get().MakeDirectory(*Directory, true)) return false;
	NewSource->Path = Directory / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".mips"));
	if (!FFileHelper::SaveArrayToFile(Bytes, *NewSource->Path)) return false;
	auto* Provider = NewObject<UBF6HighPolyMipProvider>(Texture);
	Provider->Source = MoveTemp(NewSource);
	Texture->AddAssetUserData(Provider);
	return true;
}

FTextureMipDataProvider* UBF6HighPolyMipProvider::AllocateMipDataProvider(UTexture* Texture)
{
	return Source ? new FBF6MipRequest(Texture, Source) : nullptr;
}

FStreamableRenderResourceState UBF6HighPolyMipProvider::GetResourcePostInitState(const UTexture* Owner, bool bAllowStreaming)
{
	const auto* Texture = CastChecked<UTexture2D>(Owner);
	const FTexturePlatformData* PD = Texture->GetPlatformData();
	FStreamableRenderResourceState State;
	const int32 Count = FMath::Min3(PD->Mips.Num(), GMaxTextureMipCount,
		int32(FStreamableRenderResourceState::MAX_LOD_COUNT));
	// Keep at most the 64-pixel tail resident. Higher authored mips are supplied
	// by this provider, without editor-only DDC/bulk-data streaming metadata.
	const int32 Tail = FMath::Min(7, Count);
	State.AssetLODBias = PD->Mips.Num() - Count;
	State.MaxNumLODs = Count;
	State.NumNonOptionalLODs = Count;
	State.NumNonStreamingLODs = Tail;
	State.bSupportsStreaming = bAllowStreaming && !Texture->NeverStream && Tail < Count;
	State.NumResidentLODs = State.NumRequestedLODs = State.bSupportsStreaming &&
		IStreamingManager::Get().IsRenderAssetStreamingEnabled(EStreamableRenderAssetType::Texture) ? Tail : Count;
	return State;
}

bool UBF6HighPolyMipProvider::GetInitialMipData(int32 First, TArrayView<void*> Data,
	TArrayView<int64> Sizes, FStringView)
{
	if (!Source || First < 0 || First + Data.Num() > Source->Mips.Num()
		|| (Sizes.Num() && Sizes.Num() != Data.Num())) return false;
	TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Source->Path));
	if (!File) return false;
	for (int32 I = 0; I < Data.Num(); ++I)
	{
		const int64 Size = Source->Mips[First + I].Bytes;
		Data[I] = FMemory::Malloc(Size);
		if (Sizes.Num()) Sizes[I] = Size;
		if (!Source->Read(*File, First + I, Data[I], Size))
		{
			for (int32 J = 0; J <= I; ++J) { FMemory::Free(Data[J]); Data[J] = nullptr; if (Sizes.Num()) Sizes[J] = 0; }
			return false;
		}
	}
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBF6MipBackingTest, "BF6.HighPoly.Streaming.BackingRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBF6MipBackingTest::RunTest(const FString&)
{
	TStrongObjectPtr<UTexture2D> Texture(UTexture2D::CreateTransient(128, 64, PF_DXT1));
	FTexturePlatformData* PD = Texture->GetPlatformData();
	TArray<uint8> Bytes;
	int32 W = 128, H = 64;
	for (int32 I = 0; I < 8; ++I)
	{
		if (I) PD->Mips.Add(new FTexture2DMipMap());
		PD->Mips[I].SizeX = W; PD->Mips[I].SizeY = H; PD->Mips[I].SizeZ = 1;
		const int32 Size = FMath::DivideAndRoundUp(W, 4) * FMath::DivideAndRoundUp(H, 4) * 8;
		for (int32 B = 0; B < Size; ++B) Bytes.Add(uint8(I * 23 + B));
		W = FMath::Max(1, W / 2); H = FMath::Max(1, H / 2);
	}
	TArray<uint8> Short = Bytes; Short.Pop();
	TestFalse(TEXT("Reject a truncated block-compressed mip chain"), UBF6HighPolyMipProvider::Attach(Texture.Get(), Short));
	if (!TestTrue(TEXT("Attach complete rectangular BC1 chain"), UBF6HighPolyMipProvider::Attach(Texture.Get(), Bytes))) return false;
	auto* Provider = Texture->GetAssetUserData<UBF6HighPolyMipProvider>();
	const FString Backing = Provider->Source->Path;
	{
		FBF6MipRequest Request(Texture.Get(), Provider->Source);
		TestTrue(TEXT("Outstanding request has cancellable ownership"), Request.GetCancelThread() == FTextureMipDataProvider::ETickThread::Async);
		Request.Cancel(FTextureUpdateSyncOptions());
		TestTrue(TEXT("Cancelled request cannot keep GC waiting for cleanup"), Request.GetCancelThread() == FTextureMipDataProvider::ETickThread::None);
		Request.Cancel(FTextureUpdateSyncOptions());
		TestTrue(TEXT("Cancellation is idempotent"), Request.GetCancelThread() == FTextureMipDataProvider::ETickThread::None);
	}
	for (FTexture2DMipMap& Mip : PD->Mips) Mip.BulkData.RemoveBulkData();
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		TArray<void*> Data; Data.SetNumZeroed(3);
		TArray<int64> Sizes; Sizes.SetNumZeroed(3);
		if (TestTrue(TEXT("Reload the mip tail without bulk data"), Provider->GetInitialMipData(5, Data, Sizes, FStringView())))
		{
			for (int32 I = 0; I < Data.Num(); ++I)
			{
				const auto& Mip = Provider->Source->Mips[I + 5];
				TestEqual(TEXT("Exact mip bytes"), FMemory::Memcmp(Data[I], Bytes.GetData() + Mip.Offset, Mip.Bytes), 0);
				FMemory::Free(Data[I]);
			}
		}
	}
	{
		TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Backing));
		const auto& Mip = Provider->Source->Mips[0];
		const uint32 Pitch = Mip.RowBytes + 64;
		TArray<uint8> Padded; Padded.Init(0xcd, Pitch * Mip.Rows);
		TestTrue(TEXT("Padded GPU row layout"), Provider->Source->Read(*File, 0, Padded.GetData(), Padded.Num(), Pitch));
		for (uint32 Row = 0; Row < Mip.Rows; ++Row)
		{
			TestEqual(TEXT("Exact row"), FMemory::Memcmp(Padded.GetData() + Row * Pitch,
				Bytes.GetData() + Row * Mip.RowBytes, Mip.RowBytes), 0);
			TestEqual(TEXT("Padding untouched"), Padded[Row * Pitch + Mip.RowBytes], uint8(0xcd));
		}
		TestFalse(TEXT("Reject undersized GPU buffer"), Provider->Source->Read(*File, 0, Padded.GetData(), 4, Pitch));
	}
	Texture.Reset();
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("Backing file released with texture"), IFileManager::Get().FileExists(*Backing));
	return true;
}
#endif
