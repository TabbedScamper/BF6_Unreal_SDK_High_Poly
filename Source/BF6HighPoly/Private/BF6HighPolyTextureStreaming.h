#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureAllMipDataProviderFactory.h"
#include "BF6HighPolyTextureStreaming.generated.h"

class UTexture2D;
struct FBF6MipSource;

// Session-owned backing storage, independent of the optional derived cache.
// No game reader or world UObject is accessed by a streaming worker.
UCLASS()
class UBF6HighPolyMipProvider : public UTextureAllMipDataProviderFactory
{
	GENERATED_BODY()
public:
	static bool Attach(UTexture2D* Texture, const TArray<uint8>& Bytes);
	virtual FTextureMipDataProvider* AllocateMipDataProvider(UTexture* Texture) override;
	virtual bool WillProvideMipDataWithoutDisk() const override { return Source.IsValid(); }
	virtual bool GetInitialMipData(int32 FirstMip, TArrayView<void*> Data,
		TArrayView<int64> Sizes, FStringView DebugContext) override;
	virtual FStreamableRenderResourceState GetResourcePostInitState(const UTexture* Owner,
		bool bAllowStreaming) override;

	TSharedPtr<FBF6MipSource, ESPMode::ThreadSafe> Source;
};
