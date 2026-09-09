#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BF6HighPolyViewportLibrary.generated.h"

// Small editor-only bridge used by the built-in Unreal MCP toolset.  The
// stock EditorLevelLibrary camera calls can address a hidden preview viewport;
// these functions deliberately address the visible level-editing client.
UCLASS()
class BF6HIGHPOLY_API UBF6HighPolyViewportLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool SetVisibleLevelViewportCamera(FVector Location, FRotator Rotation);

	UFUNCTION(BlueprintPure, Category="BF6 High Poly|MCP")
	static bool GetVisibleLevelViewportCamera(FVector& Location, FRotator& Rotation);

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool RedrawVisibleLevelViewport();

	// Synchronous native-size capture of the visible level viewport. Unlike the
	// automation screenshot API this has no latent task whose Python wrapper can
	// be collected or replaced by a second request.
	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool CaptureVisibleLevelViewport(const FString& OutputPath,
		int32& Width, int32& Height);

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool ShowBuildViewport();

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool StartFullHighPolyBuild();

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool OpenBaseMap(const FString& Level);

	UFUNCTION(BlueprintPure, Category="BF6 High Poly|MCP")
	static FString GetCurrentLevel();

	UFUNCTION(BlueprintPure, Category="BF6 High Poly|MCP")
	static int32 GetMapImageState();

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static int32 SetMapImageVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool ShowWaterLab();

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool ReloadWaterLab();

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static bool StartExactTerrainAtVisibleCamera(float SpanMetres = 64.f,
		int32 Resolution = 1024);

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static void DisableExactTerrain();

	UFUNCTION(BlueprintPure, Category="BF6 High Poly|MCP")
	static FString GetExactTerrainStatus();

	UFUNCTION(BlueprintCallable, Category="BF6 High Poly|MCP")
	static int32 FixSafeValidationIssues(bool bSave = true);
};
