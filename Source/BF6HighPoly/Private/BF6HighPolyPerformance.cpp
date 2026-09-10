#include "BF6HighPolyPerformance.h"
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Scalability.h"
#include "DynamicRHI.h"
#include "RenderTimer.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace BF6HP::Performance
{
	namespace
	{
		int GProfile = 0;
		bool GInitialized = false, GHaveOriginal = false;
		Scalability::FQualityLevels GOriginal;
		struct FFrame { uint64 Number; double Time, Game, Render, RHI, GPU, GameWait, RenderWait; };
		TArray<FFrame> GFrames;
		FString GCapturePath;
		FDelegateHandle GCaptureHandle;
	}
	int GetProfile() { return GProfile; }
	void SetProfile(int Profile)
	{
		if (!GHaveOriginal) { GOriginal = Scalability::GetQualityLevels(); GHaveOriginal = true; }
		GInitialized = true;
		GProfile = FMath::Clamp(Profile, 0, 2);
		// Preserve unrelated groups even if the creator changed them after our
		// first preset. Restoring Current only restores the five groups we own.
		Scalability::FQualityLevels Quality = Scalability::GetQualityLevels();
		if (GProfile)
		{
			const int Level = GProfile == 1 ? 1 : 2;
			Quality.GlobalIlluminationQuality = Level;
			Quality.ReflectionQuality = Level;
			Quality.ShadowQuality = Level;
			Quality.PostProcessQuality = Level;
			Quality.EffectsQuality = Level;
		}
		else
		{
			Quality.GlobalIlluminationQuality = GOriginal.GlobalIlluminationQuality;
			Quality.ReflectionQuality = GOriginal.ReflectionQuality;
			Quality.ShadowQuality = GOriginal.ShadowQuality;
			Quality.PostProcessQuality = GOriginal.PostProcessQuality;
			Quality.EffectsQuality = GOriginal.EffectsQuality;
		}
		Scalability::SetQualityLevels(Quality);
		if (GConfig)
		{
			GConfig->SetInt(TEXT("BF6HighPoly"), TEXT("PerformanceProfile"), GProfile, GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
		}
	}
	void Initialize()
	{
		if (GInitialized) return;
		GInitialized = true;
		int Profile = 0;
		if (GConfig && GConfig->GetInt(TEXT("BF6HighPoly"), TEXT("PerformanceProfile"), Profile, GEditorPerProjectIni) && Profile)
			SetProfile(Profile);
	}
	void StopCapture()
	{
		if (!GCaptureHandle.IsValid()) return;
		FCoreDelegates::OnEndFrame.Remove(GCaptureHandle); GCaptureHandle.Reset();
		FString Csv(TEXT("frame,seconds,game_ms,render_ms,rhi_ms,gpu_ms,game_wait_ms,render_wait_ms\n"));
		for (const FFrame& F : GFrames)
			Csv += FString::Printf(TEXT("%llu,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n"),
				F.Number, F.Time, F.Game, F.Render, F.RHI, F.GPU, F.GameWait, F.RenderWait);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(GCapturePath), true);
		FFileHelper::SaveStringToFile(Csv, *GCapturePath);
		GFrames.Empty(); GCapturePath.Reset();
	}
	void StartCapture(const FString& Path)
	{
		StopCapture(); GCapturePath = Path; GFrames.Reserve(8192);
		GCaptureHandle = FCoreDelegates::OnEndFrame.AddLambda([]
		{
			if (GFrames.Num() >= 100000) { StopCapture(); return; }
			GFrames.Add({GFrameCounter, FPlatformTime::Seconds(), FPlatformTime::ToMilliseconds(GGameThreadTime),
				FPlatformTime::ToMilliseconds(GRenderThreadTime), FPlatformTime::ToMilliseconds(GRHIThreadTime),
				FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()), FPlatformTime::ToMilliseconds(GGameThreadWaitTime),
				FPlatformTime::ToMilliseconds(GRenderThreadWaitTime)});
		});
	}
}

static FAutoConsoleCommand GBF6PerfCapture(TEXT("BF6.HighPoly.PerfCapture"),
	TEXT("start <csv path> | stop: record engine CPU/GPU counters during a viewport test. A zero GPU time is unavailable."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 2 && Args[0] == TEXT("start")) BF6HP::Performance::StartCapture(Args[1]);
		else if (Args.Num() == 1 && Args[0] == TEXT("stop")) BF6HP::Performance::StopCapture();
	}));

static FAutoConsoleCommand GBF6PreviewQuality(TEXT("BF6.HighPoly.Quality"),
	TEXT("current | performance | balanced: viewport lighting, shadows and effects budget; preserves geometry and texture quality."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() != 1) return;
		const FString Name = Args[0].ToLower();
		if (Name == TEXT("current")) BF6HP::Performance::SetProfile(0);
		else if (Name == TEXT("performance")) BF6HP::Performance::SetProfile(1);
		else if (Name == TEXT("balanced")) BF6HP::Performance::SetProfile(2);
	}));
