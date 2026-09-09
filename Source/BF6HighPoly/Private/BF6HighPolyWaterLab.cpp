#include "BF6HighPolyWaterLab.h"

#include "BF6HighPolyCore.h"
#include "BF6HighPolyWaterFFT.h"
#include "BF6HighPolyWaterMaterial.h"
#include "BF6SDKExtension.h"

#include "Async/Async.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "MaterialDomain.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "RenderingThread.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"

DEFINE_LOG_CATEGORY_STATIC(LogBF6WaterLab, Log, All);

namespace
{
	constexpr int32 LayerCount = 5;
	const TCHAR* LayerNames[LayerCount] = {
		TEXT("RAW CASCADE 0"), TEXT("RAW CASCADE 1"), TEXT("RAW CASCADE 2"),
		TEXT("RAW CASCADE 3"), TEXT("STACKED SUM") };
	const FLinearColor LayerColors[LayerCount] = {
		FLinearColor(0.95f, 0.12f, 0.08f), FLinearColor(0.08f, 0.85f, 0.24f),
		FLinearColor(0.10f, 0.32f, 1.0f), FLinearColor(1.0f, 0.55f, 0.06f),
		FLinearColor(0.10f, 0.85f, 1.0f) };

	BF6HP::FCore Core;
	TUniquePtr<BF6HP::FWaterFFT> FFT;
	TArray<BF6HP::FCore::FWaterCascade> LiveCascades;
	TArray<BF6HP::FCore::FWater> LiveWater;
	TWeakObjectPtr<AActor> LabActor;
	TWeakObjectPtr<UStaticMesh> LabMesh;
	TWeakObjectPtr<UStaticMeshComponent> GameComponent;
	TWeakObjectPtr<UStaticMeshComponent> SeabedComponent;
	TWeakObjectPtr<UMaterialInstanceDynamic> GameMaterial;
	TArray<TWeakObjectPtr<UStaticMeshComponent>> LayerComponents;
	TArray<TWeakObjectPtr<UTextRenderComponent>> Labels;
	TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> LayerMaterials;
	TSharedPtr<SWindow> Window;
	TSharedPtr<SEditableTextBox> LevelBox;
	FTSTicker::FDelegateHandle TickHandle;
	FString LabLevel = TEXT("MP_Isolated");
	FString Status = TEXT("waiting to read the current Steam install");
	bool bBusy = false;
	double LoadStartedAt = 0.0;
	bool bPaused = false;
	bool bZeroControl = false;
	bool bDiagnosticStack = false;
	bool bBaseFFT = true;
	bool bDrawSheets = false;
	bool bExtendedDraw = true;
	bool bVisible[LayerCount] = { true, true, true, true, true };
	float ExplodeCm = 350.f;
	float Gain = 1000.f;
	// Explicit comparisons against decoded values. One is always the installed
	// game's value; RESET returns every control to that unmodified route.
	float HeightMultiplier = 1.f;
	float WavelengthMultiplier = 1.f;
	float HorizontalMultiplier = 1.f;
	float SimulationRate = 1.f;
	float MicroNormalMultiplier = 1.f;
	float FoamNormalMultiplier = 1.f;
	// Unreal translation bridge, not a decoded FFT multiplier.  The ramp width
	// expands the continuous water shoulder below the native broad draw fields;
	// it does not rescale the already-hundreds-of-metres source UVs.
	float BroadCarrierSize = 5.f;
	float BroadTimeScale = 0.05f;
	float BroadWaveHeightM = 4.f;
	float FoamCrestStart = 0.55f;
	float FoamCrestFull = 0.90f;
	FVector2D PreviewSourceCenter = FVector2D::ZeroVector;
	float PreviewBlock2RangeM = 0.f;

	struct FPendingWaterLoad
	{
		FString RequestedLevel;
		FString ReadError;
		TArray<BF6HP::FCore::FWaterCascade> Inputs;
		TArray<BF6HP::FCore::FWater> Water;
		BF6HP::FCore::FWaterMask WaterMask;
		BF6HP::FCore::FTerrain WaterHeightfield;
		bool bControl = false;
		bool bRead = false;
		bool bWaterMaskRead = false;
		bool bHeightfieldOnly = false;
		double CascadeSeconds = 0.0;
		double SurfaceSeconds = 0.0;
	};

	FCriticalSection PendingLoadLock;
	TUniquePtr<FPendingWaterLoad> PendingLoad;
	TFuture<void> ActiveRead;
	TFuture<void> ActiveHeightfieldRead;
	bool bHeightfieldBusy = false;

	void SetStatus(const FString& In)
	{
		Status = In;
		UE_LOG(LogBF6WaterLab, Display, TEXT("%s"), *In);
	}

	double SampleHeight(const BF6HP::FCore::FTerrain& T, double X, double Z)
	{
		if (T.Size < 2 || T.Heights.Num() < T.Size * T.Size) return 0.0;
		const double U = (X - T.WorldMin.X) / FMath::Max(1.0, T.WorldMax.X - T.WorldMin.X);
		const double V = (Z - T.WorldMin.Z) / FMath::Max(1.0, T.WorldMax.Z - T.WorldMin.Z);
		if (U < 0.0 || U > 1.0 || V < 0.0 || V > 1.0) return 0.0;
		const int32 Ix = FMath::Clamp(FMath::RoundToInt(U * (T.Size - 1)), 0, T.Size - 1);
		const int32 Iz = FMath::Clamp(FMath::RoundToInt(V * (T.Size - 1)), 0, T.Size - 1);
		return T.Heights[Iz * T.Size + Ix] * ((double)T.HeightScale / 65536.0);
	}

	FVector2D FindMostVaryingPatch(const BF6HP::FCore::FTerrain& T,
		const BF6HP::FCore::FWater& W, float& OutRange)
	{
		FVector2D Best = W.Center;
		double BestRange = -1.0;
		const double X0 = W.Center.X - W.Size.X * .5 + 64.0;
		const double X1 = W.Center.X + W.Size.X * .5 - 64.0;
		const double Z0 = W.Center.Y - W.Size.Y * .5 + 64.0;
		const double Z1 = W.Center.Y + W.Size.Y * .5 - 64.0;
		for (double Z = Z0; Z <= Z1; Z += 32.0)
			for (double X = X0; X <= X1; X += 32.0)
			{
				double Lo = DBL_MAX, Hi = -DBL_MAX;
				for (int32 Dz = -64; Dz <= 64; Dz += 32)
					for (int32 Dx = -64; Dx <= 64; Dx += 32)
					{
						const double H = SampleHeight(T, X + Dx, Z + Dz);
						if (H <= 0.0) continue;
						Lo = FMath::Min(Lo, H); Hi = FMath::Max(Hi, H);
					}
				if (Lo == DBL_MAX || Hi - Lo <= BestRange) continue;
				BestRange = Hi - Lo;
				Best = FVector2D(X, Z);
			}
		OutRange = (float)FMath::Max(0.0, BestRange);
		return Best;
	}

	float MeasurePatchRange(const BF6HP::FCore::FTerrain& T, const FVector2D& Center)
	{
		double Lo = DBL_MAX, Hi = -DBL_MAX;
		for (int32 Dz = -64; Dz <= 64; Dz += 16)
			for (int32 Dx = -64; Dx <= 64; Dx += 16)
			{
				const double H = SampleHeight(T, Center.X + Dx, Center.Y + Dz);
				if (H <= 0.0) continue;
				Lo = FMath::Min(Lo, H);
				Hi = FMath::Max(Hi, H);
			}
		return Lo == DBL_MAX ? 0.f : (float)FMath::Max(0.0, Hi - Lo);
	}

	FVector2D SelectPreviewPatch(const FString& Level,
		const BF6HP::FCore::FTerrain& T, const BF6HP::FCore::FWater& W,
		float& OutRange)
	{
		// This is a camera/reference fixture, never a simulation input. The point
		// is the recorded Tsuru overlook used by the retained BF6 frame. Sampling
		// another part of the 5.9 km surface changed every world-anchored draw
		// texture and made a visual comparison impossible even though the raw
		// water values were identical.
		if (Level.Equals(TEXT("MP_Isolated"), ESearchCase::IgnoreCase))
		{
			const FVector2D TsuruOverlook(-1020.07327149, 178.11326936);
			const bool bInside =
				FMath::Abs(TsuruOverlook.X - W.Center.X) <= W.Size.X * 0.5 &&
				FMath::Abs(TsuruOverlook.Y - W.Center.Y) <= W.Size.Y * 0.5;
			if (bInside)
			{
				OutRange = MeasurePatchRange(T, TsuruOverlook);
				return TsuruOverlook;
			}
		}
		return FindMostVaryingPatch(T, W, OutRange);
	}

	void ClearScene()
	{
		// ShutdownModule can run after GUObjectArray has already been torn down.
		// TWeakObjectPtr::Get() consults that array and asserts even though the
		// process is already destroying every UObject. Only dereference/destroy
		// the lab actor during a live reload; engine-exit cleanup is automatic.
		const bool bEngineExit = IsEngineExitRequested();
		if (!bEngineExit)
		{
			if (AActor* A = LabActor.Get()) A->Destroy();
		}
		LabActor.Reset();
		LabMesh.Reset();
		GameComponent.Reset();
		SeabedComponent.Reset();
		GameMaterial.Reset();
		LayerComponents.Empty();
		Labels.Empty();
		LayerMaterials.Empty();
		// Destroy/unregister queues render-thread work. Complete that work before
		// releasing the heightfield and rebuilding a material graph that uses the
		// same parameter names. Without this fence Reload could leave the old proxy
		// evaluating BF6WaterHeightTex after its UObject had been retired.
		if (!bEngineExit) FlushRenderingCommands();
		if (FFT) { FFT->Reset(); FFT.Reset(); }
		// SetWaterHeightfield removes the transient texture from the UObject root
		// set. That is required for a live reload, but illegal after GUObjectArray
		// teardown and unnecessary during process exit.
		if (!bEngineExit) BF6WaterShared::SetWaterHeightfield(nullptr);
	}

	UMaterial* MakeSeabedMaterial()
	{
		UPackage* Package = CreatePackage(TEXT("/Temp/BF6RawWaterLab_Seabed"));
		if (!Package) return nullptr;
		Package->SetFlags(RF_Transient);
		UMaterial* M = NewObject<UMaterial>(Package, TEXT("M_BF6RawWaterLab_Seabed"), RF_Transient);
		if (!M) return nullptr;
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_DefaultLit);
		auto* Colour = Cast<UMaterialExpressionVectorParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionVectorParameter::StaticClass(), -200, 0));
		if (Colour)
		{
			Colour->ParameterName = TEXT("SeabedColour");
			Colour->DefaultValue = FLinearColor(0.16f, 0.13f, 0.09f);
			UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_BaseColor);
		}
		auto* Roughness = Cast<UMaterialExpressionConstant>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionConstant::StaticClass(), -200, 80));
		if (Roughness)
		{
			Roughness->R = 0.85f;
			UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		}
		M->PreEditChange(nullptr);
		M->PostEditChange();
		return M;
	}

	UTexture2D* WhiteLinear()
	{
		static TWeakObjectPtr<UTexture2D> Cached;
		if (Cached.IsValid()) return Cached.Get();
		UTexture2D* T = UTexture2D::CreateTransient(1, 1, PF_FloatRGBA);
		if (!T) return nullptr;
		T->SRGB = false;
		T->NeverStream = true;
		if (FFloat16Color* P = static_cast<FFloat16Color*>(
			T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE)))
		{
			P[0] = FFloat16Color(FLinearColor::Black);
			T->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
		T->UpdateResource();
		T->AddToRoot();
		Cached = T;
		return T;
	}

	UMaterial* MakeLabMaterial()
	{
		UPackage* Package = CreatePackage(TEXT("/Temp/BF6RawWaterLab"));
		if (!Package) return nullptr;
		Package->SetFlags(RF_Transient);
		UMaterial* M = NewObject<UMaterial>(Package, TEXT("M_BF6RawWaterLab"), RF_Transient);
		if (!M) return nullptr;
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_DefaultLit);
		M->BlendMode = BLEND_Opaque;
		M->bTangentSpaceNormal = false;

		auto Scalar = [M](const TCHAR* Name, float Default, int32 Y)
		{
			auto* E = Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionScalarParameter::StaticClass(), -1100, Y));
			if (E) { E->ParameterName = Name; E->DefaultValue = Default; }
			return E;
		};
		auto Vector = [M](const TCHAR* Name, const FLinearColor& Default, int32 Y)
		{
			auto* E = Cast<UMaterialExpressionVectorParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionVectorParameter::StaticClass(), -900, Y));
			if (E) { E->ParameterName = Name; E->DefaultValue = Default; }
			return E;
		};

		auto* UV = Cast<UMaterialExpressionTextureCoordinate>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionTextureCoordinate::StaticClass(), -1300, -100));
		UMaterialExpressionTextureObjectParameter* Disp[4] = {};
		UMaterialExpressionVectorParameter* Meta[4] = {};
		UMaterialExpressionScalarParameter* Weight[4] = {};
		for (int32 I = 0; I < 4; ++I)
		{
			Disp[I] = Cast<UMaterialExpressionTextureObjectParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(
					M, UMaterialExpressionTextureObjectParameter::StaticClass(), -1300, 80 + I * 80));
			if (Disp[I])
			{
				Disp[I]->ParameterName = *FString::Printf(TEXT("BF6Disp%d"), I);
				Disp[I]->SamplerType = SAMPLERTYPE_LinearColor;
				Disp[I]->Texture = WhiteLinear();
			}
			Meta[I] = Vector(*FString::Printf(TEXT("BF6Cascade%d"), I),
				FLinearColor(1.f, 0.f, 0.f, 0.f), 80 + I * 80);
			Weight[I] = Scalar(*FString::Printf(TEXT("BF6LabWeight%d"), I), 1.f, 80 + I * 80);
		}
		auto* Span = Scalar(TEXT("BF6LabSpanM"), 128.f, 430);
		auto* VerticalGain = Scalar(TEXT("BF6LabGain"), 1000.f, 480);
		auto* Colour = Vector(TEXT("BF6LabColor"), LayerColors[4], 530);

		auto AddInputs = [&](UMaterialExpressionCustom* X)
		{
			auto In = [X](const TCHAR* Name, UMaterialExpression* Expression)
			{
				FCustomInput V;
				V.InputName = Name;
				V.Input.Expression = Expression;
				X->Inputs.Add(V);
			};
			In(TEXT("UV"), UV);
			In(TEXT("Span"), Span);
			In(TEXT("Gain"), VerticalGain);
			for (int32 I = 0; I < 4; ++I)
			{
				In(*FString::Printf(TEXT("D%d"), I), Disp[I]);
				In(*FString::Printf(TEXT("C%d"), I), Meta[I]);
				In(*FString::Printf(TEXT("W%d"), I), Weight[I]);
			}
		};

		auto* WPO = Cast<UMaterialExpressionCustom>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionCustom::StaticClass(), -500, 100));
		if (WPO)
		{
			WPO->Code = TEXT(
				"float2 p=(UV-0.5)*Span;\n"
				"float4 a=Texture2DSampleLevel(D0,D0Sampler,frac(p/max(C0.x,1e-4)),0)*W0;\n"
				"float4 b=Texture2DSampleLevel(D1,D1Sampler,frac(p/max(C1.x,1e-4)),0)*W1;\n"
				"float4 c=Texture2DSampleLevel(D2,D2Sampler,frac(p/max(C2.x,1e-4)),0)*W2;\n"
				"float4 d=Texture2DSampleLevel(D3,D3Sampler,frac(p/max(C3.x,1e-4)),0)*W3;\n"
				"float2 h=-(C0.y*a.xz+C1.y*b.xz+C2.y*c.xz+C3.y*d.xz);\n"
				"float v=a.y+b.y+c.y+d.y;\n"
				"return float3(h.x,h.y,v)*Gain*100.0;");
			WPO->OutputType = CMOT_Float3;
			WPO->Description = TEXT("Raw installed BF6 cascade stack; Gain is labelled diagnostic scale");
			AddInputs(WPO);
			UMaterialEditingLibrary::ConnectMaterialProperty(WPO, TEXT(""), MP_WorldPositionOffset);
		}

		auto* Normal = Cast<UMaterialExpressionCustom>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionCustom::StaticClass(), -250, 260));
		if (Normal)
		{
			Normal->Code = TEXT(
				"float2 p=(UV-0.5)*Span; float e=max(Span/256.0,1e-4);\n"
				"#define H(P) (Texture2DSample(D0,D0Sampler,frac((P)/max(C0.x,1e-4))).y*W0+Texture2DSample(D1,D1Sampler,frac((P)/max(C1.x,1e-4))).y*W1+Texture2DSample(D2,D2Sampler,frac((P)/max(C2.x,1e-4))).y*W2+Texture2DSample(D3,D3Sampler,frac((P)/max(C3.x,1e-4))).y*W3)\n"
				"float2 s=float2(H(p+float2(e,0))-H(p-float2(e,0)),H(p+float2(0,e))-H(p-float2(0,e)))/(2*e);\n"
				"#undef H\n"
				"return normalize(float3(-s.x*Gain,-s.y*Gain,1));");
			Normal->OutputType = CMOT_Float3;
			AddInputs(Normal);
			UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT(""), MP_Normal);
		}
		if (Colour)
			UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_BaseColor);
		auto* Roughness = Cast<UMaterialExpressionConstant>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionConstant::StaticClass(), -100, 520));
		if (Roughness)
		{
			Roughness->R = 0.28f;
			UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		}
		M->PreEditChange(nullptr);
		M->PostEditChange();
		return M;
	}

	UStaticMesh* MakeGrid(UObject* Outer)
	{
		constexpr int32 N = 256;
		FMeshDescription MD;
		FStaticMeshAttributes Attr(MD);
		Attr.Register();
		auto Positions = Attr.GetVertexPositions();
		auto UVs = Attr.GetVertexInstanceUVs();
		const FPolygonGroupID Group = MD.CreatePolygonGroup();
		Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("RawWater");
		TArray<FVertexID> Vertices;
		Vertices.SetNumUninitialized((N + 1) * (N + 1));
		for (int32 Y = 0; Y <= N; ++Y)
			for (int32 X = 0; X <= N; ++X)
			{
				const FVertexID V = MD.CreateVertex();
				Positions[V] = FVector3f(
					((float)X / N - 0.5f) * 12800.f,
					((float)Y / N - 0.5f) * 12800.f, 0.f);
				Vertices[Y * (N + 1) + X] = V;
			}
		auto Corner = [&](int32 X, int32 Y)
		{
			const FVertexInstanceID VI = MD.CreateVertexInstance(Vertices[Y * (N + 1) + X]);
			UVs.Set(VI, 0, FVector2f((float)X / N, (float)Y / N));
			return VI;
		};
		for (int32 Y = 0; Y < N; ++Y)
			for (int32 X = 0; X < N; ++X)
			{
				MD.CreatePolygon(Group, { Corner(X, Y), Corner(X + 1, Y + 1), Corner(X + 1, Y) });
				MD.CreatePolygon(Group, { Corner(X, Y), Corner(X, Y + 1), Corner(X + 1, Y + 1) });
			}
		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MD);
		FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals);
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, TEXT("BF6RawWaterGrid"), RF_Transient);
		if (!Mesh) return nullptr;
		// The transient component has collision disabled, but editor registration
		// still asks the mesh for collision data once. Retain this small grid's CPU
		// vertices so that query is internally consistent and warning-free.
		Mesh->bAllowCPUAccess = true;
		Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, TEXT("RawWater"), TEXT("RawWater")));
		Mesh->SetPositiveBoundsExtension(FVector(0, 0, 50000.f));
		Mesh->SetNegativeBoundsExtension(FVector(0, 0, 50000.f));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bFastBuild = true;
		return Mesh->BuildFromMeshDescriptions({ &MD }, Params) ? Mesh : nullptr;
	}

	void ApplyViewSettings()
	{
		if (UStaticMeshComponent* C = GameComponent.Get())
			C->SetVisibility(!bDiagnosticStack, true);
		if (UStaticMeshComponent* C = SeabedComponent.Get())
			C->SetVisibility(!bDiagnosticStack, true);
		if (UMaterialInstanceDynamic* MID = GameMaterial.Get())
		{
			MID->SetScalarParameterValue(TEXT("BF6FFTEnabled"), bBaseFFT ? 1.f : 0.f);
			const BF6HP::FCore::FWater* W = LiveWater.IsEmpty() ? nullptr : &LiveWater[0];
			if (W)
			{
				MID->SetScalarParameterValue(TEXT("BF6WaveAmplitudeScale"),
					W->WaveAmplitudeScale * HeightMultiplier);
				if (W->MicroNormalStrength >= 0.f)
					MID->SetScalarParameterValue(TEXT("DetailRipple"),
						W->MicroNormalStrength * MicroNormalMultiplier);
				if (W->FoamNormalStrength >= 0.f)
					MID->SetScalarParameterValue(TEXT("FoamSheetNormalStrength"),
						W->FoamNormalStrength * FoamNormalMultiplier);
			}
			for (int32 I = 0; I < 4; ++I)
			{
				const FString Name = FString::Printf(TEXT("BF6Cascade%d"), I);
				if (LiveCascades.IsValidIndex(I))
				{
					const BF6HP::FCore::FWaterCascade& C = LiveCascades[I];
					MID->SetVectorParameterValue(*Name, FLinearColor(
						C.TileDimension * WavelengthMultiplier,
						C.Choppiness * HorizontalMultiplier,
						1.f, C.bFoamEnabled ? 1.f : 0.f));
				}
			}
			MID->SetScalarParameterValue(TEXT("UseDetailSheets"),
				bDrawSheets && W && W->DetailNormal >= 0 && W->FoamNormal >= 0 ? 1.f : 0.f);
			MID->SetScalarParameterValue(TEXT("UseContactFoam"),
				bExtendedDraw && W && W->ContactFoam >= 0 ? 1.f : 0.f);
			MID->SetScalarParameterValue(TEXT("UseBroadPattern"),
				bExtendedDraw && W && W->ExtendedGraphVersion == 1 &&
				W->FoamRgb2 >= 0 && W->Noise >= 0 ? 1.f : 0.f);
			MID->SetScalarParameterValue(TEXT("BF6BroadCarrierSize"), BroadCarrierSize);
			MID->SetScalarParameterValue(TEXT("BF6BroadTimeScale"), BroadTimeScale);
			MID->SetScalarParameterValue(TEXT("BF6FoamWaveHeightM"), BroadWaveHeightM);
			MID->SetScalarParameterValue(TEXT("BF6FoamCrestStart"), FoamCrestStart);
			MID->SetScalarParameterValue(TEXT("BF6FoamCrestFull"), FoamCrestFull);
		}
		for (int32 I = 0; I < LayerCount; ++I)
		{
			if (UStaticMeshComponent* C = LayerComponents.IsValidIndex(I) ? LayerComponents[I].Get() : nullptr)
			{
				C->SetRelativeLocation(FVector(0, 0, I * ExplodeCm));
				C->SetVisibility(bDiagnosticStack && bVisible[I], true);
			}
			if (UTextRenderComponent* T = Labels.IsValidIndex(I) ? Labels[I].Get() : nullptr)
			{
				T->SetRelativeLocation(FVector(-6800.f, 0.f, I * ExplodeCm + 80.f));
				T->SetVisibility(bDiagnosticStack && bVisible[I], true);
			}
		}
		for (TWeakObjectPtr<UMaterialInstanceDynamic>& Weak : LayerMaterials)
			if (UMaterialInstanceDynamic* MID = Weak.Get())
				MID->SetScalarParameterValue(TEXT("BF6LabGain"), Gain);
	}

	float LogSliderToMultiplier(float V)
	{
		// Six orders of magnitude, with the exact game value centred at 1x.
		return FMath::Pow(10.f, FMath::Lerp(-3.f, 3.f, FMath::Clamp(V, 0.f, 1.f)));
	}

	float MultiplierToLogSlider(float V)
	{
		return FMath::Clamp((FMath::LogX(10.f, FMath::Max(V, 0.001f)) + 3.f) / 6.f, 0.f, 1.f);
	}

	void ResetGameValueControls()
	{
		HeightMultiplier = 1.f;
		WavelengthMultiplier = 1.f;
		HorizontalMultiplier = 1.f;
		SimulationRate = 1.f;
		MicroNormalMultiplier = 1.f;
		FoamNormalMultiplier = 1.f;
		BroadCarrierSize = 5.f;
		BroadTimeScale = 0.05f;
		BroadWaveHeightM = 4.f;
		FoamCrestStart = 0.55f;
		FoamCrestFull = 0.90f;
		ApplyViewSettings();
		UE_LOG(LogBF6WaterLab, Display,
			TEXT("game-value controls reset: decoded inputs at 1x; Unreal broad ramp at 5x, calibrated carrier rate at 0.05x"));
	}

	FString RawGameValueSummary()
	{
		if (LiveWater.IsEmpty() || LiveCascades.IsEmpty())
			return TEXT("Raw values appear after the live read completes.");
		const BF6HP::FCore::FWater& W = LiveWater[0];
		FString R = FString::Printf(
			TEXT("%s\nSurface: amplitude scale %.6g | micro normal %.6g | foam normal %.6g\n"),
			FFT ? *FFT->ProofSummary() : TEXT("GPU proof: not initialized"),
			W.WaveAmplitudeScale, W.MicroNormalStrength, W.FoamNormalStrength);
		for (int32 I = 0; I < LiveCascades.Num(); ++I)
		{
			const BF6HP::FCore::FWaterCascade& C = LiveCascades[I];
			R += FString::Printf(
				TEXT("C%d: wind %.6g | chop %.6g | tile %.6gm | min wave %.6gm | amplitude %.6g | large-wave reduction %.6g%s"),
				I, C.WindSpeed, C.Choppiness, C.TileDimension, C.MinWavelength,
				C.WaveAmplitude, C.LargeWaveReduction,
				I + 1 < LiveCascades.Num() ? TEXT("\n") : TEXT(""));
		}
		return R;
	}

	void FrameCamera()
	{
		if (!GEditor) return;
		if (FViewport* V = GEditor->GetActiveViewport())
			if (FEditorViewportClient* C = static_cast<FEditorViewportClient*>(V->GetClient()))
			{
				C->SetViewportType(LVT_Perspective);
				C->SetViewLocation(FVector(-15000.f, -15500.f, 9500.f));
				C->SetViewRotation(FRotator(-23.f, 43.f, 0.f));
				C->Invalidate();
			}
	}

	bool BuildScene()
	{
		if (!GEditor || !FFT || !FFT->IsReady() || LiveWater.IsEmpty()) return false;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World) return false;
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor) return false;
		Actor->SetActorLabel(TEXT("BF6 RAW WATER STACK (runtime game data)"));
		BF6Ext::MarkAddonActor(Actor, TEXT("RawWaterLab"));
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		UStaticMesh* Mesh = MakeGrid(Actor);
		UMaterial* DiagnosticParent = MakeLabMaterial();
		UMaterial* SeabedMaterial = MakeSeabedMaterial();
		const BF6HP::FCore::FWater SourceWater = LiveWater[0];
		BF6HP::FCore::FWater PreviewWater = SourceWater;
		// Preserve every authored render value while recentering the preview
		// patch around the lab camera. WaterOrigin follows the recentered patch,
		// so cascade phase remains internally consistent without large world coords.
		PreviewWater.Center = FVector2D::ZeroVector;
		PreviewWater.Size = FVector2D(128.f, 128.f);
		PreviewWater.Height = 0.f;
		BF6WaterShared::ApplyStableReflectionPolicy();
		BF6WaterShared::InvalidateMaterialGraph();
		UMaterialInstanceDynamic* Accumulated = Mesh && DiagnosticParent
			? BF6WaterShared::CreateMaterial(Actor, Core, *FFT, PreviewWater) : nullptr;
		if (!Mesh || !DiagnosticParent || !SeabedMaterial || !Accumulated)
		{
			Actor->Destroy();
			return false;
		}
		// The isolated lab intentionally has no terrain. Make that explicit so
		// the white placeholder depth texture cannot zero the coarse cascades.
		// Full-level import still binds its actual terrain-height fallback.
		Accumulated->SetScalarParameterValue(TEXT("BF6DepthAvailable"), 0.f);
		// Only the preview geometry is recentered. Heightfield sampling remains
		// in the authored MP_Isolated XZ frame and uses the original flat entity
		// Y in the exact max(H-flatY,0) vertex law.
		Accumulated->SetVectorParameterValue(TEXT("BF6WaterSourceWorld"), FLinearColor(
			(float)PreviewSourceCenter.X, (float)PreviewSourceCenter.Y,
			SourceWater.Height, 0.f));
		Accumulated->SetVectorParameterValue(TEXT("BF6WaterOrigin"), FLinearColor(
			(float)(SourceWater.Center.X - PreviewSourceCenter.X),
			(float)(SourceWater.Center.Y - PreviewSourceCenter.Y), 0.f, 0.f));
		LabActor = Actor;
		LabMesh = Mesh;
		GameMaterial = Accumulated;

		UStaticMeshComponent* GameSurface = NewObject<UStaticMeshComponent>(
			Actor, TEXT("AccumulatedGameWater"));
		GameSurface->SetupAttachment(Root);
		GameSurface->SetStaticMesh(Mesh);
		GameSurface->SetMaterial(0, Accumulated);
		GameSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		GameSurface->RegisterComponent();
		GameComponent = GameSurface;

		UStaticMeshComponent* Seabed = NewObject<UStaticMeshComponent>(
			Actor, TEXT("NeutralVerificationSeabed"));
		Seabed->SetupAttachment(Root);
		Seabed->SetStaticMesh(Mesh);
		Seabed->SetMaterial(0, SeabedMaterial);
		Seabed->SetRelativeLocation(FVector(0.f, 0.f, -500.f));
		Seabed->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Seabed->RegisterComponent();
		SeabedComponent = Seabed;

		for (int32 I = 0; I < LayerCount; ++I)
		{
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(DiagnosticParent, Actor);
			if (!MID) continue;
			FFT->Bind(MID, 1.f);
			for (int32 C = 0; C < 4; ++C)
				MID->SetScalarParameterValue(*FString::Printf(TEXT("BF6LabWeight%d"), C),
					(I == 4 || I == C) ? 1.f : 0.f);
			MID->SetScalarParameterValue(TEXT("BF6LabSpanM"), 128.f);
			MID->SetScalarParameterValue(TEXT("BF6LabGain"), Gain);
			MID->SetVectorParameterValue(TEXT("BF6LabColor"), LayerColors[I]);
			LayerMaterials.Add(MID);

			UStaticMeshComponent* Surface = NewObject<UStaticMeshComponent>(
				Actor, *FString::Printf(TEXT("RawLayer%d"), I));
			Surface->SetupAttachment(Root);
			Surface->SetStaticMesh(Mesh);
			Surface->SetMaterial(0, MID);
			Surface->SetMobility(EComponentMobility::Movable);
			Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Surface->SetCastShadow(true);
			Surface->RegisterComponent();
			LayerComponents.Add(Surface);

			UTextRenderComponent* Text = NewObject<UTextRenderComponent>(
				Actor, *FString::Printf(TEXT("RawLabel%d"), I));
			Text->SetupAttachment(Root);
			Text->SetText(FText::FromString(LayerNames[I]));
			Text->SetTextRenderColor(LayerColors[I].ToFColor(true));
			Text->SetWorldSize(85.f);
			Text->SetHorizontalAlignment(EHTA_Left);
			Text->SetVerticalAlignment(EVRTA_TextCenter);
			Text->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));
			Text->RegisterComponent();
			Labels.Add(Text);
		}

		// The full High Poly build now carries the same approved 8/1 water-view
		// lighting calibration. Do not add another global sun and skylight when the
		// Lab is opened over that build: the old duplicate pair changed the whole
		// level and made opening a diagnostic window look like it loaded shaders.
		bool bFullEnvironmentPresent = false;
		for (TObjectIterator<USkyLightComponent> It; It; ++It)
		{
			if (It->GetWorld() == World && It->GetName() == TEXT("Light_SkyLight"))
			{
				bFullEnvironmentPresent = true;
				break;
			}
		}
		if (!bFullEnvironmentPresent)
		{
			UDirectionalLightComponent* Sun = NewObject<UDirectionalLightComponent>(Actor, TEXT("LabSun"));
			Sun->SetupAttachment(Root);
			Sun->SetRelativeRotation(FRotator(-48.f, -35.f, 0.f));
			Sun->SetIntensity(8.f);
			Sun->RegisterComponent();
			USkyLightComponent* Sky = NewObject<USkyLightComponent>(Actor, TEXT("LabSky"));
			Sky->SetupAttachment(Root);
			Sky->Intensity = 1.0f;
			Sky->RegisterComponent();
		}
		else
		{
			UE_LOG(LogBF6WaterLab, Display,
				TEXT("full High Poly environment present; Lab duplicate global lights suppressed"));
		}
		ApplyViewSettings();
		// The lab is an overlay/diagnostic source, not a camera owner. Reframing
		// here destroyed the user's paired BF6/Unreal viewpoint on every MCP
		// reload, making visual controls incomparable.
		return true;
	}

	void FinishLoad(const FString& RequestedLevel, bool bControl, bool bRead,
		const FString& ReadError,
		TArray<BF6HP::FCore::FWaterCascade>&& Inputs,
		TArray<BF6HP::FCore::FWater>&& Water,
		BF6HP::FCore::FWaterMask&& WaterMask, bool bWaterMaskRead,
		BF6HP::FCore::FTerrain&& WaterHeightfield,
		double CascadeSeconds, double SurfaceSeconds)
	{
		const double ViewStartedAt = FPlatformTime::Seconds();
		bBusy = false;
		if (!bRead)
		{
			SetStatus(FString::Printf(TEXT("READ FAILED: %s"), *ReadError));
			return;
		}
		if (Inputs.IsEmpty() || Water.IsEmpty())
		{
			SetStatus(FString::Printf(TEXT("NO WATER DATA: %s"), *ReadError));
			return;
		}
		LiveCascades = Inputs;
		// Put the authored ocean first; local pools remain available to future
		// surface selection without accidentally becoming the main verification view.
		const int32 OceanIndex = Water.IndexOfByPredicate(
			[](const BF6HP::FCore::FWater& W) { return W.bOcean; });
		if (OceanIndex > 0) Water.Swap(0, OceanIndex);
		LiveWater = MoveTemp(Water);
		PreviewSourceCenter = SelectPreviewPatch(RequestedLevel,
			WaterHeightfield, LiveWater[0], PreviewBlock2RangeM);
		BF6WaterShared::SetWaterMask(bWaterMaskRead ? &WaterMask : nullptr);
		BF6WaterShared::SetWaterHeightfield(
			WaterHeightfield.Size > 1 ? &WaterHeightfield : nullptr);
		if (bControl)
			for (BF6HP::FCore::FWaterCascade& Cascade : Inputs)
				for (FVector2f& H0 : Cascade.H0) H0 = FVector2f::ZeroVector;
		FFT = MakeUnique<BF6HP::FWaterFFT>();
		if (!FFT->Initialize(Core, Inputs))
		{
			SetStatus(TEXT("FFT REPLAY INITIALIZATION FAILED; see LogBF6WaterFFT"));
			FFT.Reset();
			return;
		}
		if (!BuildScene())
		{
			SetStatus(TEXT("VIEW BUILD FAILED"));
			return;
		}
		FString Details;
		for (int32 I = 0; I < Inputs.Num(); ++I)
			Details += FString::Printf(TEXT(" C%d=N%d/%.1fm"), I,
				Inputs[I].Resolution, Inputs[I].TileDimension);
		const BF6HP::FCore::FWater& Surface = LiveWater[0];
		const double ReadySeconds = LoadStartedAt > 0.0
			? FPlatformTime::Seconds() - LoadStartedAt : 0.0;
		const double ViewSeconds = FPlatformTime::Seconds() - ViewStartedAt;
		SetStatus(FString::Printf(
			TEXT("GAME VIEW LIVE: %s | %s | stable CPU replay=%s | draw graph=v%u overlap=v%u | ")
			TEXT("block2=%dx%d RAW, preview (%.1f,%.1f), range %.3fm | interactive/player/boat=MISSING | attenuation=%s | ready %.3fs (cascades %.3fs, surfaces %.3fs, view %.3fs) |%s"),
			*RequestedLevel, bControl ? TEXT("ZERO-H0 CONTROL") : TEXT("CURRENT STEAM BYTES"),
			FFT->IsReady() ? TEXT("YES") : TEXT("NO"),
			Surface.ExtendedGraphVersion, Surface.CascadeOverlapVersion,
			WaterHeightfield.Size, WaterHeightfield.Size,
			PreviewSourceCenter.X, PreviewSourceCenter.Y, PreviewBlock2RangeM,
			bWaterMaskRead ? TEXT("EXACT COARSEMASK") :
				(Surface.bShoreFadeValid ? TEXT("AUTHORED") : TEXT("MISSING SOURCE")),
			ReadySeconds, CascadeSeconds, SurfaceSeconds, ViewSeconds, *Details));

		// The 8193-square raw water heightfield is useful for shoreline geometry,
		// but it is not required to display the decoded ocean/FFT stack. Read it
		// after first paint so the lab is interactive immediately, then bind the
		// engine-owned downsampled texture onto the already-live MID.
		bHeightfieldBusy = true;
		ActiveHeightfieldRead = Async(EAsyncExecution::Thread, [RequestedLevel]
		{
			TUniquePtr<FPendingWaterLoad> Result = MakeUnique<FPendingWaterLoad>();
			Result->RequestedLevel = RequestedLevel;
			Result->bHeightfieldOnly = true;
			Result->bRead = Core.ReadWaterHeightfield(
				RequestedLevel, Result->WaterHeightfield);
			if (!Result->bRead && Core.Error.IsEmpty())
				Core.Error = TEXT("raw water heightfield was not found");
			Result->ReadError = Core.Error;
			FScopeLock Lock(&PendingLoadLock);
			PendingLoad = MoveTemp(Result);
		});
	}

	void FinishHeightfield(FPendingWaterLoad& Result)
	{
		bHeightfieldBusy = false;
		if (!Result.bRead)
		{
			SetStatus(FString::Printf(
				TEXT("GAME VIEW LIVE; shoreline heightfield unavailable: %s"),
				*Result.ReadError));
			return;
		}
		PreviewBlock2RangeM = MeasurePatchRange(
			Result.WaterHeightfield, PreviewSourceCenter);
		BF6WaterShared::SetWaterHeightfield(&Result.WaterHeightfield);
		BF6WaterShared::BindWaterHeightfield(GameMaterial.Get());
		SetStatus(FString::Printf(
			TEXT("GAME VIEW LIVE + SHORELINE HEIGHTFIELD: %s | block2=%dx%d RAW | ")
			TEXT("preview range %.3fm | total %.3fs"),
			*Result.RequestedLevel, Result.WaterHeightfield.Size,
			Result.WaterHeightfield.Size, PreviewBlock2RangeM,
			FPlatformTime::Seconds() - LoadStartedAt));
	}

	void ReloadInternal(bool bControl)
	{
		if (bBusy || bHeightfieldBusy)
		{
			SetStatus(TEXT("a live read is already running"));
			return;
		}
		if (LevelBox.IsValid())
		{
			LabLevel = LevelBox->GetText().ToString().TrimStartAndEnd();
			if (LabLevel.IsEmpty()) LabLevel = TEXT("MP_Isolated");
		}
		ClearScene();
		bZeroControl = bControl;
		const FString Install = BF6Ext::GameInstallDir();
		// ONE AUTHORITATIVE CORE, the same rule the main reader already follows.
		//
		// This used to prefer a separately named bf6_core_waterheight.dll whenever
		// that file happened to exist, so the lab could take a new read path while
		// the editor held the standard image mapped. The cost outweighed it: the
		// side build is frozen at whatever day it was produced, it is NOT emitted
		// by the current libbf6 build at all, and preferring it silently hides
		// every API added afterwards while the plugin compiles against the current
		// header. That is an ABI mismatch by construction.
		//
		// Observed cost: the editor crashed on shutdown inside
		// bf6_core_waterheight.dll, reached through FCore::Close, after the lab had
		// loaded an image five days older than the deployed header.
		// BF6HighPoly.cpp records the same conclusion for the main reader.
		const FString Dll = BF6HP::CoreDllPath();   // staged first, then development
		if (Install.IsEmpty() || !FPaths::FileExists(FPaths::Combine(Install, TEXT("bf6.exe"))))
		{
			SetStatus(TEXT("Steam BF6 install was not found by the SDK"));
			return;
		}
		if (!Core.IsOpen() && !Core.Open(Install, Dll))
		{
			SetStatus(FString::Printf(TEXT("CORE OPEN FAILED: %s"), *Core.Error));
			return;
		}
		bBusy = true;
		LoadStartedAt = FPlatformTime::Seconds();
		const FString RequestedLevel = LabLevel;
		SetStatus(FString::Printf(TEXT("reading only %s water schematics directly from %s ..."),
			*RequestedLevel, *Install));
		ActiveRead = Async(EAsyncExecution::Thread, [RequestedLevel, bControl]
		{
			TUniquePtr<FPendingWaterLoad> Result = MakeUnique<FPendingWaterLoad>();
			Result->RequestedLevel = RequestedLevel;
			Result->bControl = bControl;
			double PhaseStart = FPlatformTime::Seconds();
			Result->bRead = Core.ReadWaterCascadesIsolated(RequestedLevel, Result->Inputs);
			Result->CascadeSeconds = FPlatformTime::Seconds() - PhaseStart;
			if (Result->bRead)
			{
				PhaseStart = FPlatformTime::Seconds();
				Result->bRead = Core.ReadWater(RequestedLevel, Result->Water);
				Result->SurfaceSeconds = FPlatformTime::Seconds() - PhaseStart;
			}
			if (Result->bRead)
				Result->bWaterMaskRead = Core.ReadWaterMask(
					RequestedLevel, Result->WaterMask);
			if (!Result->bRead && Core.Error.IsEmpty())
				Core.Error = TEXT("isolated surface/render record was not found");
			Result->ReadError = Core.Error;
			FScopeLock Lock(&PendingLoadLock);
			PendingLoad = MoveTemp(Result);
		});
	}

	TSharedRef<SWidget> MakeControls()
	{
		TSharedRef<SVerticalBox> Layers = SNew(SVerticalBox);
		for (int32 I = 0; I < LayerCount; ++I)
		{
			Layers->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([I] { return bVisible[I] ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([I](ECheckBoxState State)
				{
					bVisible[I] = State == ECheckBoxState::Checked;
					ApplyViewSettings();
				})
				[ SNew(STextBlock).Text(FText::FromString(LayerNames[I])) ]
			];
		}
		return SNew(SBox).WidthOverride(420.f).Padding(12.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[ SNew(STextBlock).Text(FText::FromString(TEXT("BF6 LIVE WATER LAB"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(
				TEXT("The viewport shows one accumulated game-water surface. Every available stage below is read from the current Steam install; missing runtime stages stay explicitly missing. No exported files are runtime inputs."))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[ SAssignNew(LevelBox, SEditableTextBox).Text(FText::FromString(LabLevel)) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("RELOAD LIVE RAW"))).OnClicked_Lambda([]
					{ ReloadInternal(false); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(4, 0, 0, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("ZERO-H0 CONTROL"))).OnClicked_Lambda([]
					{ ReloadInternal(true); return FReply::Handled(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[ SNew(SCheckBox)
				.IsChecked_Lambda([] { return bBaseFFT ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([](ECheckBoxState S) { bBaseFFT = S == ECheckBoxState::Checked; ApplyViewSettings(); })
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Base ocean FFT — stable replay + live H0"))) ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[ SNew(SCheckBox)
				.IsChecked_Lambda([] { return bDrawSheets ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([](ECheckBoxState S) { bDrawSheets = S == ECheckBoxState::Checked; ApplyViewSettings(); })
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Fast micro/foam sheet detail (diagnostic)"))) ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[ SNew(SCheckBox)
				.IsChecked_Lambda([] { return bExtendedDraw ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([](ECheckBoxState S) { bExtendedDraw = S == ECheckBoxState::Checked; ApplyViewSettings(); })
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Map draw graph — contact/broad foam where decoded"))) ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 8)
			[ SNew(STextBlock).Text(FText::FromString(
				TEXT("Interactive waves / players / boats: MISSING — not substituted"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
			[ SNew(STextBlock).Text(FText::FromString(TEXT("LIVE GAME-VALUE CONTROLS (RAW = 1x)"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(
				TEXT("Every control starts at 1x the value read from the current game. Multipliers are experiments; the raw values below remain unchanged. GPU proof compares Unreal's readback against the separate CPU replay; ZERO-H0 must return exact zero."))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 2)
			[ SNew(STextBlock).Text(FText::FromString(
				TEXT("BROAD WATER RAMP (Unreal bridge; native source wavelengths stay unchanged)"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 2, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("x1"))).OnClicked_Lambda([]
					{ BroadCarrierSize = 1.f; ApplyViewSettings(); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(2, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("x5"))).OnClicked_Lambda([]
					{ BroadCarrierSize = 5.f; ApplyViewSettings(); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(2, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("x10"))).OnClicked_Lambda([]
					{ BroadCarrierSize = 10.f; ApplyViewSettings(); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(2, 0, 0, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("x20"))).OnClicked_Lambda([]
					{ BroadCarrierSize = 20.f; ApplyViewSettings(); return FReply::Handled(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1, 0, 5)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Ramp width:"))) ]
				+ SHorizontalBox::Slot().FillWidth(1)
				[ SNew(SSpinBox<float>)
					.MinValue(1.f).MaxValue(64.f)
					.MinSliderValue(1.f).MaxSliderValue(32.f)
					.Delta(0.5f)
					.Value_Lambda([] { return BroadCarrierSize; })
					.OnValueChanged_Lambda([](float V)
						{ BroadCarrierSize = FMath::Clamp(V, 1.f, 64.f); ApplyViewSettings(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
			[ SNew(STextBlock).Text_Lambda([] { return FText::FromString(FString::Printf(
				TEXT("Ramp width x%.1f | native broad UV scale x1"), BroadCarrierSize)); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1, 0, 5)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Broad motion rate:"))) ]
				+ SHorizontalBox::Slot().FillWidth(1)
				[ SNew(SSpinBox<float>)
					.MinValue(0.f).MaxValue(4.f)
					.MinSliderValue(0.f).MaxSliderValue(2.f)
					.Delta(0.05f)
					.Value_Lambda([] { return BroadTimeScale; })
					.OnValueChanged_Lambda([](float V)
						{ BroadTimeScale = FMath::Clamp(V, 0.f, 4.f); ApplyViewSettings(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
			[ SNew(STextBlock).Text_Lambda([] { return FText::FromString(FString::Printf(
				TEXT("Broad deep-water rate x%.2f (0 freezes; height is unchanged)"), BroadTimeScale)); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1, 0, 5)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Foamed crest height (m):"))) ]
				+ SHorizontalBox::Slot().FillWidth(1)
				[ SNew(SSpinBox<float>)
					.MinValue(0.f).MaxValue(8.f)
					.MinSliderValue(0.f).MaxSliderValue(6.f)
					.Delta(0.1f)
					.Value_Lambda([] { return BroadWaveHeightM; })
					.OnValueChanged_Lambda([](float V)
						{ BroadWaveHeightM = FMath::Clamp(V, 0.f, 8.f); ApplyViewSettings(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 2)
			[ SNew(STextBlock).Text(FText::FromString(
				TEXT("OPEN-WATER FOAM CREST WINDOW (same broad height profile)"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Foam starts:"))) ]
				+ SHorizontalBox::Slot().FillWidth(1)
				[ SNew(SSpinBox<float>)
					.MinValue(-0.1f).MaxValue(0.99f)
					.MinSliderValue(-0.1f).MaxSliderValue(0.9f)
					.Delta(0.01f)
					.Value_Lambda([] { return FoamCrestStart; })
					.OnValueChanged_Lambda([](float V)
						{ FoamCrestStart = FMath::Clamp(V, -0.1f, FoamCrestFull - 0.01f); ApplyViewSettings(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1, 0, 5)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Foam full:"))) ]
				+ SHorizontalBox::Slot().FillWidth(1)
				[ SNew(SSpinBox<float>)
					.MinValue(-0.09f).MaxValue(1.f)
					.MinSliderValue(0.f).MaxSliderValue(1.f)
					.Delta(0.01f)
					.Value_Lambda([] { return FoamCrestFull; })
					.OnValueChanged_Lambda([](float V)
						{ FoamCrestFull = FMath::Clamp(V, FoamCrestStart + 0.01f, 1.f); ApplyViewSettings(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 2)
			[ SNew(STextBlock).Text(FText::FromString(
				TEXT("WAVE SIZE / WAVELENGTH (spreads the pattern; does not change height)"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 2, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("RAW x1"))).OnClicked_Lambda([]
					{ WavelengthMultiplier = 1.f; ApplyViewSettings(); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(2, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("SIZE x2"))).OnClicked_Lambda([]
					{ WavelengthMultiplier = 2.f; ApplyViewSettings(); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(2, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("SIZE x4"))).OnClicked_Lambda([]
					{ WavelengthMultiplier = 4.f; ApplyViewSettings(); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(2, 0, 0, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("SIZE x8"))).OnClicked_Lambda([]
					{ WavelengthMultiplier = 8.f; ApplyViewSettings(); return FReply::Handled(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Exact size multiplier:"))) ]
				+ SHorizontalBox::Slot().FillWidth(1)
				[ SNew(SSpinBox<float>)
					.MinValue(0.125f).MaxValue(64.f)
					.MinSliderValue(0.125f).MaxSliderValue(16.f)
					.Delta(0.125f)
					.Value_Lambda([] { return WavelengthMultiplier; })
					.OnValueChanged_Lambda([](float V)
						{ WavelengthMultiplier = FMath::Clamp(V, 0.125f, 64.f); ApplyViewSettings(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1, 0, 5)
			[ SNew(STextBlock).Text_Lambda([]
				{
					if (LiveCascades.IsEmpty())
						return FText::FromString(TEXT("Cascade tile sizes appear after the live read."));
					FString Sizes = TEXT("Effective tiles: ");
					for (int32 I = 0; I < LiveCascades.Num(); ++I)
						Sizes += FString::Printf(TEXT("C%d %.3gm%s"), I,
							LiveCascades[I].TileDimension * WavelengthMultiplier,
							I + 1 < LiveCascades.Num() ? TEXT(" | ") : TEXT(""));
					return FText::FromString(Sizes);
				}) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(STextBlock).Text(FText::FromString(
				TEXT("WAVE HEIGHT (vertical displacement only)"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(SSlider).Value_Lambda([] { return MultiplierToLogSlider(HeightMultiplier); })
				.OnValueChanged_Lambda([](float V) { HeightMultiplier = LogSliderToMultiplier(V); ApplyViewSettings(); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(STextBlock).Text_Lambda([]
				{
					const float Raw = LiveWater.IsEmpty() ? 1.f : LiveWater[0].WaveAmplitudeScale;
					return FText::FromString(FString::Printf(
						TEXT("FFT height: raw %.6g  x %.4g  = %.6g"), Raw, HeightMultiplier, Raw * HeightMultiplier));
				}) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(SSlider).MinValue(0.f).MaxValue(4.f).Value_Lambda([] { return HorizontalMultiplier; })
				.OnValueChanged_Lambda([](float V) { HorizontalMultiplier = V; ApplyViewSettings(); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(STextBlock).Text_Lambda([] { return FText::FromString(FString::Printf(
				TEXT("FFT horizontal/choppiness: x %.3f (raw per-cascade values below)"), HorizontalMultiplier)); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(SSlider).MinValue(0.f).MaxValue(4.f).Value_Lambda([] { return SimulationRate; })
				.OnValueChanged_Lambda([](float V) { SimulationRate = V; }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(STextBlock).Text_Lambda([] { return FText::FromString(FString::Printf(
				TEXT("Simulation time rate: x %.3f"), SimulationRate)); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(SSlider).MinValue(0.f).MaxValue(4.f).Value_Lambda([] { return MicroNormalMultiplier; })
				.OnValueChanged_Lambda([](float V) { MicroNormalMultiplier = V; ApplyViewSettings(); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(STextBlock).Text_Lambda([] { return FText::FromString(FString::Printf(
				TEXT("Shipped micro-normal strength: x %.3f"), MicroNormalMultiplier)); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(SSlider).MinValue(0.f).MaxValue(4.f).Value_Lambda([] { return FoamNormalMultiplier; })
				.OnValueChanged_Lambda([](float V) { FoamNormalMultiplier = V; ApplyViewSettings(); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1)
			[ SNew(STextBlock).Text_Lambda([] { return FText::FromString(FString::Printf(
				TEXT("Shipped foam-normal strength: x %.3f"), FoamNormalMultiplier)); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 5)
			[ SNew(SButton).Text(FText::FromString(TEXT("RESET ALL TO RAW GAME VALUES (1x)"))).OnClicked_Lambda([]
				{ ResetGameValueControls(); return FReply::Handled(); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 1, 0, 8)
			[ SNew(STextBlock).AutoWrapText(true).Text_Lambda([] { return FText::FromString(RawGameValueSummary()); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 8)
			[ SNew(SCheckBox)
				.IsChecked_Lambda([] { return bDiagnosticStack ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([](ECheckBoxState S) { bDiagnosticStack = S == ECheckBoxState::Checked; ApplyViewSettings(); })
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Show colored cascade diagnostic instead of game view"))) ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)
				[ SNew(SButton).Text_Lambda([] { return FText::FromString(bPaused ? TEXT("RESUME") : TEXT("PAUSE")); }).OnClicked_Lambda([]
					{ bPaused = !bPaused; return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(4, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("EXACT x1"))).OnClicked_Lambda([]
					{ Gain = 1.f; ApplyViewSettings(); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(4, 0, 0, 0)
				[ SNew(SButton).Text(FText::FromString(TEXT("DIAGNOSTIC x1000"))).OnClicked_Lambda([]
					{ Gain = 1000.f; ApplyViewSettings(); return FReply::Handled(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[ SNew(STextBlock).Text_Lambda([] { return FText::FromString(FString::Printf(
				TEXT("Cascade-debug displacement scale: x%.0f%s"), Gain,
				Gain == 1.f ? TEXT(" (exact metres)") : TEXT(" (diagnostic only)"))); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[ SNew(SSlider).MinValue(0.f).MaxValue(1200.f).Value_Lambda([] { return ExplodeCm; })
				.OnValueChanged_Lambda([](float V) { ExplodeCm = V; ApplyViewSettings(); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 6)
			[ SNew(STextBlock).Text_Lambda([] { return FText::FromString(FString::Printf(
				TEXT("Cascade-debug spacing: %.0f cm (0 overlays every band)"), ExplodeCm)); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 8)[ Layers ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[ SNew(STextBlock).AutoWrapText(true).Text_Lambda([] { return FText::FromString(Status); }) ]
		];
	}

	bool Tick(float DeltaSeconds)
	{
		TUniquePtr<FPendingWaterLoad> CompletedLoad;
		{
			FScopeLock Lock(&PendingLoadLock);
			CompletedLoad = MoveTemp(PendingLoad);
		}
		if (CompletedLoad && CompletedLoad->bHeightfieldOnly)
		{
			FinishHeightfield(*CompletedLoad);
		}
		else if (CompletedLoad)
		{
			FinishLoad(CompletedLoad->RequestedLevel, CompletedLoad->bControl,
				CompletedLoad->bRead, CompletedLoad->ReadError,
				MoveTemp(CompletedLoad->Inputs), MoveTemp(CompletedLoad->Water),
				MoveTemp(CompletedLoad->WaterMask), CompletedLoad->bWaterMaskRead,
				MoveTemp(CompletedLoad->WaterHeightfield),
				CompletedLoad->CascadeSeconds, CompletedLoad->SurfaceSeconds);
		}
		if (FFT && !bPaused) FFT->Tick(DeltaSeconds * SimulationRate);
		if (bBusy)
		{
			FString Stage;
			int32 Done = 0, Total = 0;
			Core.Progress.Read(Stage, Done, Total);
			if (!Stage.IsEmpty())
				Status = Total > 0
					? FString::Printf(TEXT("%s  %d / %d"), *Stage, Done, Total)
					: Stage;
		}
		return true;
	}
}

void BF6WaterLab::Show()
{
	if (Window.IsValid())
	{
		Window->BringToFront();
		return;
	}
	Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("BF6 Live Water Lab")))
		.ClientSize(FVector2D(480.f, 760.f))
		.SupportsMaximize(false)
		.SupportsMinimize(true)
		.IsTopmostWindow(false)
		[ MakeControls() ];
	Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([](const TSharedRef<SWindow>&)
	{
		Window.Reset();
		LevelBox.Reset();
	}));
	FSlateApplication::Get().AddWindow(Window.ToSharedRef());
}

void BF6WaterLab::Reload()
{
	ReloadInternal(false);
}

void BF6WaterLab::ZeroControl()
{
	ReloadInternal(true);
}

void BF6WaterLab::Start()
{
	if (!TickHandle.IsValid())
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&Tick), 1.f / 30.f);
	Show();
	Reload();
}

void BF6WaterLab::Shutdown()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (Window.IsValid()) Window->RequestDestroyWindow();
	Window.Reset();
	LevelBox.Reset();
	if (ActiveRead.IsValid()) ActiveRead.Wait();
	if (ActiveHeightfieldRead.IsValid()) ActiveHeightfieldRead.Wait();
	{
		FScopeLock Lock(&PendingLoadLock);
		PendingLoad.Reset();
	}
	ClearScene();
	Core.Close();
}

static FAutoConsoleCommand GWaterLabShowCmd(
	TEXT("BF6.WaterLab.Show"),
	TEXT("Start or show the persistent raw BF6 water-stack controls."),
	FConsoleCommandDelegate::CreateStatic(&BF6WaterLab::Start));

static FAutoConsoleCommand GWaterLabReloadCmd(
	TEXT("BF6.WaterLab.Reload"),
	TEXT("Re-read the selected level and installed shader bytes without restarting Unreal."),
	FConsoleCommandDelegate::CreateStatic(&BF6WaterLab::Reload));

static FAutoConsoleCommand GWaterLabZeroControlCmd(
	TEXT("BF6.WaterLab.ZeroControl"),
	TEXT("Re-read the selected level, replace H0 with zero, and require exact-zero GPU output."),
	FConsoleCommandDelegate::CreateStatic(&BF6WaterLab::ZeroControl));

// Versioned during lab development so an abandoned pre-reload module cannot
// retain the same console name and route a command back into stale code.
static FAutoConsoleCommand GWaterLabStartV9Cmd(
	TEXT("BF6.WaterLab.StartV9"),
	TEXT("Start the v9 raw-water lab with live decoded-value controls."),
	FConsoleCommandDelegate::CreateStatic(&BF6WaterLab::Start));
