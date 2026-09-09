#include "BF6HighPolyViewportLibrary.h"
#include "BF6SDKExtension.h"
#include "BF6HighPolyTerrainBridge.h"
#include "BF6HighPolyControlBridge.h"
#include "BF6HighPolyWaterLab.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "SLevelViewport.h"
#endif

namespace
{
#if WITH_EDITOR
	FLevelEditorViewportClient* VisibleLevelViewportClient()
	{
		// GCurrentLevelEditingViewportClient can point at the hidden SDK scene
		// viewport after a workspace restore.  The Level Editor module owns the
		// viewport the user can actually see, so it must win this selection.
		if (FLevelEditorModule* LevelEditor =
			FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
		{
			if (TSharedPtr<SLevelViewport> Viewport = LevelEditor->GetFirstActiveLevelViewport())
				return &Viewport->GetLevelViewportClient();
		}
		if (GCurrentLevelEditingViewportClient)
			return GCurrentLevelEditingViewportClient;
		return nullptr;
	}
#endif
}

bool UBF6HighPolyViewportLibrary::SetVisibleLevelViewportCamera(
	FVector Location, FRotator Rotation)
{
#if WITH_EDITOR
	// The SDK seam updates every same-world client, but after EnterBuild the
	// global "current" client can still be the hidden startup client.  Always
	// apply to the Level Editor module's visible viewport too; returning early
	// here produced a truthful camera report for a frame the user never saw.
	const bool bSdkUpdated = BF6Ext::SetBuildViewportCamera(Location, Rotation);
	bool bAnyUpdated = false;
	auto Apply = [&](FEditorViewportClient* AnyClient)
	{
		if (!AnyClient || !AnyClient->IsPerspective()) return;
		AnyClient->SetRealtime(true);
		AnyClient->SetViewLocation(Location);
		AnyClient->SetViewRotation(Rotation);
		AnyClient->Invalidate(true, true);
		if (AnyClient->Viewport) AnyClient->Viewport->InvalidateDisplay();
		bAnyUpdated = true;
	};
	FLevelEditorViewportClient* Client = VisibleLevelViewportClient();
	Apply(Client);
	// EnterBuild can leave the onscreen client out of the editor-world filter
	// until its next focus event.  Explicit MCP camera placement is intentional,
	// so update every perspective editor client rather than trusting that stale
	// world association. This also keeps split/restored layouts coherent.
	if (GEditor)
		for (FEditorViewportClient* AnyClient : GEditor->GetAllViewportClients())
			Apply(AnyClient);
	return bSdkUpdated || bAnyUpdated;
#else
	return false;
#endif
}

bool UBF6HighPolyViewportLibrary::GetVisibleLevelViewportCamera(
	FVector& Location, FRotator& Rotation)
{
#if WITH_EDITOR
	FLevelEditorViewportClient* Client = VisibleLevelViewportClient();
	if (Client)
	{
		Location = Client->GetViewLocation();
		Rotation = Client->GetViewRotation();
		return true;
	}
	return BF6Ext::GetBuildViewportCamera(Location, Rotation);
#else
	return false;
#endif
}

bool UBF6HighPolyViewportLibrary::RedrawVisibleLevelViewport()
{
#if WITH_EDITOR
	if (BF6Ext::RedrawBuildViewport()) return true;
	FLevelEditorViewportClient* Client = VisibleLevelViewportClient();
	if (!Client) return false;
	Client->Invalidate(true, true);
	if (Client->Viewport) Client->Viewport->InvalidateDisplay();
	if (GEditor) GEditor->RedrawLevelEditingViewports(true);
	return true;
#else
	return false;
#endif
}

bool UBF6HighPolyViewportLibrary::CaptureVisibleLevelViewport(
	const FString& OutputPath, int32& Width, int32& Height)
{
	Width = Height = 0;
#if WITH_EDITOR
	FLevelEditorViewportClient* Client = VisibleLevelViewportClient();
	if (!Client || !Client->Viewport || OutputPath.IsEmpty()) return false;
	Client->Invalidate(true, true);
	Client->Viewport->Draw();
	const FIntPoint Size = Client->Viewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0) return false;
	TArray<FColor> Pixels;
	if (!Client->Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
		return false;
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Size.X, Size.Y,
		TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
	if (Png.IsEmpty()) return false;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
	if (!FFileHelper::SaveArrayToFile(Png, *OutputPath)) return false;
	Width = Size.X;
	Height = Size.Y;
	return true;
#else
	return false;
#endif
}

bool UBF6HighPolyViewportLibrary::ShowBuildViewport()
{
#if WITH_EDITOR
	BF6Ext::ShowBuildOverlay();
	return RedrawVisibleLevelViewport();
#else
	return false;
#endif
}

bool UBF6HighPolyViewportLibrary::StartFullHighPolyBuild()
{
#if WITH_EDITOR
	BF6Ext::ShowBuildOverlay();
	return BF6HighPolyControlBridge::StartFullBuild();
#else
	return false;
#endif
}

bool UBF6HighPolyViewportLibrary::OpenBaseMap(const FString& Level)
{
#if WITH_EDITOR
	FString Clean = Level.TrimStartAndEnd();
	if (Clean.IsEmpty() || Clean.Contains(TEXT("/")) || Clean.Contains(TEXT("\\")) ||
		!Clean.StartsWith(TEXT("MP_"), ESearchCase::IgnoreCase))
		return false;
	BF6Ext::OpenMap(Clean, FString());
	return BF6Ext::CurrentLevel().Equals(Clean, ESearchCase::IgnoreCase);
#else
	return false;
#endif
}

FString UBF6HighPolyViewportLibrary::GetCurrentLevel()
{
#if WITH_EDITOR
	return BF6Ext::CurrentLevel();
#else
	return FString();
#endif
}

int32 UBF6HighPolyViewportLibrary::GetMapImageState()
{
#if WITH_EDITOR
	return BF6Ext::MapImageState();
#else
	return 0;
#endif
}

int32 UBF6HighPolyViewportLibrary::SetMapImageVisible(bool bVisible)
{
#if WITH_EDITOR
	BF6Ext::SetMapImageVisible(bVisible);
	RedrawVisibleLevelViewport();
	return BF6Ext::MapImageState();
#else
	return 0;
#endif
}

bool UBF6HighPolyViewportLibrary::ShowWaterLab()
{
#if WITH_EDITOR
	BF6WaterLab::Start();
	return true;
#else
	return false;
#endif
}

bool UBF6HighPolyViewportLibrary::ReloadWaterLab()
{
#if WITH_EDITOR
	BF6WaterLab::Reload();
	return true;
#else
	return false;
#endif
}

bool UBF6HighPolyViewportLibrary::StartExactTerrainAtVisibleCamera(
	float SpanMetres, int32 Resolution)
{
#if WITH_EDITOR
	return BF6HighPolyTerrainBridge::StartAtVisibleCamera(SpanMetres, Resolution);
#else
	return false;
#endif
}

void UBF6HighPolyViewportLibrary::DisableExactTerrain()
{
#if WITH_EDITOR
	BF6HighPolyTerrainBridge::Disable();
#endif
}

FString UBF6HighPolyViewportLibrary::GetExactTerrainStatus()
{
#if WITH_EDITOR
	return BF6HighPolyTerrainBridge::Status();
#else
	return TEXT("unavailable outside the editor");
#endif
}

int32 UBF6HighPolyViewportLibrary::FixSafeValidationIssues(bool bSave)
{
#if WITH_EDITOR
	return BF6Ext::FixSafeValidationIssues(bSave);
#else
	return 0;
#endif
}
