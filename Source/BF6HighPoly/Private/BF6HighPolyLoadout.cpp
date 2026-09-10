#include "BF6HighPolyLoadout.h"
#include "BF6HighPolyShared.h"
#include "BF6HighPolyPlaced.h"
#include "Async/Async.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY_STATIC(LogBF6Loadout, Log, All);

namespace BF6HP::Loadout
{
namespace
{
	const FName ComponentTag(TEXT("BF6HP.Loadout"));
	bool FinishingBuild = false;
	struct FRecord
	{
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<UStaticMeshComponent> Component;
		FString Wanted, Installed, Error;
		FRequest Request;
		bool HiddenProxy = false, Near = false;
	};
	struct FAsset { UStaticMesh* Mesh = nullptr; FString Error, Detail; TMap<FString, FVector3f> SlotAnchors; };
	struct FJob
	{
		FString Key, Error;
		FDecoded Decoded;
		TSharedPtr<FCatalogue, ESPMode::ThreadSafe> Catalogue;
		TSharedPtr<Shared::FMeshWork> Work;
	};
	TArray<FRecord> Records;
	TMap<FString, FAsset> Assets;
	TSharedPtr<FCatalogue, ESPMode::ThreadSafe> Catalogue;
	TFuture<TSharedPtr<FJob, ESPMode::ThreadSafe>> Worker;
	FTSTicker::FDelegateHandle Ticker;
	FDelegateHandle MapClose;
	FDelegateHandle PreExit;
	bool NeedCatalogue = false, Closing = false, OwnBusy = false;
	double LastPoll = 0;
	FString LastError;
	FString PreviewDemandId;
	double LastPreviewDemand = 0;
	FString TypeOf(AActor* A)
	{
		for (const TCHAR* Prefix : {TEXT("type:"), TEXT("label:")})
			for (const FName& Tag : A->Tags)
				if (Tag.ToString().StartsWith(Prefix)) return Tag.ToString().Mid(FCString::Strlen(Prefix));
		return FString();
	}
	TMap<FString, FString> ValuesOf(AActor* A)
	{
		TMap<FString, FString> Out;
		for (const FName& T : A->Tags)
		{
			const FString S = T.ToString(); FString K, V;
			if (S.StartsWith(TEXT("preview:highpoly.")) && S.Mid(17).Split(TEXT("="), &K, &V)) Out.Add(K, V);
		}
		return Out;
	}
	UPrimitiveComponent* Proxy(AActor* A) { return Cast<UPrimitiveComponent>(A->GetRootComponent()); }
	void Show(FRecord& R, bool High)
	{
		AActor* A = R.Actor.Get(); if (!A) return;
		if (UStaticMeshComponent* C = R.Component.Get())
		{
			if (C->IsVisible() != High) C->SetVisibility(High, false);
			if (UPrimitiveComponent* P = Proxy(A))
				if (C->bSelectable != P->bSelectable) { C->bSelectable = P->bSelectable; C->MarkRenderStateDirty(); }
		}
		if (UPrimitiveComponent* P = Proxy(A))
		{
			if (High && !R.HiddenProxy && P->IsVisible()) { P->SetVisibility(false, false); R.HiddenProxy = true; }
			else if (!High && R.HiddenProxy) { P->SetVisibility(true, false); R.HiddenProxy = false; }
		}
		R.Near = High;
	}
	void Release(FRecord& R)
	{
		Show(R, false);
		if (UStaticMeshComponent* C = R.Component.Get()) C->DestroyComponent();
		R.Component.Reset();
	}
	void ClearRecords()
	{
		PreviewDemandId.Reset(); LastPreviewDemand = 0;
		for (FRecord& R : Records) Release(R);
		Records.Reset(); LastPoll = 0;
		for (auto& Pair : Assets) if (Pair.Value.Mesh) Pair.Value.Mesh->RemoveFromRoot();
		Assets.Reset();
	}
	FString VehicleForType(const FString& Type)
	{
		if (!Catalogue) return FString();
		FString Want = Type.Mid(4).ToLower().Replace(TEXT("_"), TEXT(""));
		// These exact aliases retain the SDK's naming rather than borrowing a neighbouring vehicle.
		if (Type == TEXT("VEH_DirtBike_M1030")) return TEXT("motorcycle/dirtbike02");
		if (Type == TEXT("VEH_DirtBike_TMO450")) return TEXT("motorcycle/dirtbike01");
		FString Result;
		for (const FChoice& V : Catalogue->Vehicles)
		{
			FString Kind, Token; V.Id.Split(TEXT("/"), &Kind, &Token);
			if (Token.Replace(TEXT("_"), TEXT("")) != Want
				&& (Kind + Token).Replace(TEXT("_"), TEXT("")) != Want) continue;
			if (!Result.IsEmpty()) return FString();
			Result = V.Id;
		}
		return Result;
	}
	void Poll()
	{
		if (!GEditor || GIsSavingPackage || IsGarbageCollecting()) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		TSet<AActor*> Seen;
		TArray<FString> Allowed; BF6Ext::PlaceableTypes(Allowed);
		TSet<FString> AllowedVehicles;
		for (const FString& T : Allowed) if (T.StartsWith(TEXT("VEH_")))
		{
			const FString V = VehicleForType(T); if (!V.IsEmpty()) AllowedVehicles.Add(V);
		}
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (!IsValid(A) || !A->Tags.Contains(FName(TEXT("BF6Placed")))) continue;
			const FString Type = TypeOf(A); if (!Handles(Type)) continue;
			Seen.Add(A);
			FRecord* R = Records.FindByPredicate([&](const FRecord& I){ return I.Actor.Get() == A; });
			if (!R)
			{
				R = &Records.AddDefaulted_GetRef(); R->Actor = A;
				TArray<UStaticMeshComponent*> Existing; A->GetComponents(Existing);
				for (UStaticMeshComponent* C : Existing) if (C->ComponentTags.Contains(ComponentTag)) C->DestroyComponent();
			}
			R->Request = RequestFor(Type, ValuesOf(A));
			if (Type.StartsWith(TEXT("VEH_"))) R->Request.Vehicle = VehicleForType(Type);
			if (Type.StartsWith(TEXT("VEH_")) && !Allowed.Contains(Type))
			{
				R->Wanted.Reset(); R->Error = TEXT("This vehicle is not available in this level's SDK catalogue."); Show(*R, false); continue;
			}
			if (Type == TEXT("VehicleSpawner") && !R->Request.Vehicle.IsEmpty() && !AllowedVehicles.Contains(R->Request.Vehicle))
			{
				R->Wanted.Reset(); R->Error = TEXT("Choose a vehicle available on this map."); Show(*R, false); continue;
			}
			R->Wanted = BF6Ext::CurrentLevel() + TEXT("\n") + R->Request.Key();
			if (R->Wanted != R->Installed) R->Error.Reset();
		}
		for (int32 I = Records.Num() - 1; I >= 0; --I)
			if (!Records[I].Actor.IsValid() || !Seen.Contains(Records[I].Actor.Get())) { Release(Records[I]); Records.RemoveAtSwap(I); }
	}
	void StartJob(const FString& Key, const FRequest& Request)
	{
		Shared::SetCoreBusy(true); OwnBusy = true;
		const auto CurrentCatalogue = Catalogue;
		Worker = Async(EAsyncExecution::ThreadPool, [Key, Request, CurrentCatalogue]() -> TSharedPtr<FJob, ESPMode::ThreadSafe>
		{
			auto Result = MakeShared<FJob, ESPMode::ThreadSafe>(); Result->Key = Key;
			FScopeLock Lock(&Shared::CoreMutex());
			if (Shared::CoreShuttingDown()) { Result->Error = TEXT("Reader is shutting down."); return Result; }
			Result->Catalogue = CurrentCatalogue;
			if (!Result->Catalogue) Result->Catalogue = ReadCatalogue(Shared::Core(), Result->Error);
			if (Result->Catalogue && !Result->Catalogue->Attachments.Contains(Request.Item))
			{
				Result->Catalogue = MakeShared<FCatalogue,ESPMode::ThreadSafe>(*Result->Catalogue);
				ReadWeaponAttachments(Shared::Core(),*Result->Catalogue,Request.Item);
			}
			if (Result->Catalogue && !Key.IsEmpty())
			{
				Result->Decoded = Decode(Shared::Core(), *Result->Catalogue, Request);
				if (Result->Decoded.Error.IsEmpty()) Result->Work = Shared::DescribeGameMesh(Result->Decoded.Sections);
			}
			return Result;
		});
	}
	void FinishJob()
	{
		if (!Worker.IsValid() || !Worker.IsReady()) return;
		const auto Result = Worker.Get();
		Worker = TFuture<TSharedPtr<FJob, ESPMode::ThreadSafe>>();
		if (Result->Catalogue) Catalogue = Result->Catalogue;
		LastError = Result->Error;
		if (!Result->Key.IsEmpty() && Result->Key.StartsWith(BF6Ext::CurrentLevel() + TEXT("\n")))
		{
			FAsset& Asset = Assets.FindOrAdd(Result->Key);
			Asset.Error = Result->Error.IsEmpty() ? Result->Decoded.Error : Result->Error;
			Asset.Detail = Result->Decoded.Detail;
			Asset.SlotAnchors = MoveTemp(Result->Decoded.SlotAnchors);
			if (Asset.Error.IsEmpty() && Result->Work.IsValid())
			{
				// All native reads have finished. Hold the same lock for material
				// texture reads so the UI sound worker cannot mutate this context.
				FScopeLock Lock(&Shared::CoreMutex());
				Asset.Mesh = Shared::CreateGameMeshObject(TEXT("Loadout"), Result->Decoded.Sections, Result->Work);
				int32 Triangles = 0;
				if (Asset.Mesh && Shared::CommitGameMesh(Asset.Mesh, Result->Work, Triangles)) Asset.Mesh->AddToRoot();
				else { Asset.Mesh = nullptr; Asset.Error = TEXT("The preview mesh could not be built."); }
			}
			if (!Asset.Mesh && Asset.Error.IsEmpty()) Asset.Error = TEXT("The selected item has no usable preview geometry.");
			UE_LOG(LogBF6Loadout, Display, TEXT("loadout preview: %s"), Asset.Error.IsEmpty() ? *Asset.Detail : *Asset.Error);
		}
		Shared::SetCoreBusy(false); OwnBusy = false; LastPoll = 0;
	}
	void Install(FRecord& R, UStaticMesh* Mesh)
	{
		AActor* A = R.Actor.Get(); if (!A || !A->GetRootComponent()) return;
		UStaticMeshComponent* C = R.Component.Get();
		if (!C)
		{
			C = NewObject<UStaticMeshComponent>(A, NAME_None, RF_Transient | RF_DuplicateTransient);
			C->ComponentTags.Add(ComponentTag);
			C->SetMobility(EComponentMobility::Movable); C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCanEverAffectNavigation(false); C->SetupAttachment(A->GetRootComponent());
			C->SetVisibility(false); C->bSelectable = Proxy(A) ? Proxy(A)->bSelectable : true;
			C->SetStaticMesh(Mesh); C->RegisterComponent(); R.Component = C;
		}
		else C->SetStaticMesh(Mesh);
		if (R.Request.Type == TEXT("LootSpawner"))
		{
			// The SDK marker and game weapon both point along source Z. Keep
			// that orientation and real weapon scale; align in the marker's local
			// space so rotated/scaled parents cannot introduce a world-space offset.
			if (UPrimitiveComponent* P = Proxy(A))
				C->SetRelativeLocation(P->CalcBounds(FTransform::Identity).Origin - Mesh->GetBounds().Origin);
		}
		R.Installed = R.Wanted; R.Error.Reset();
	}
	bool Tick(float)
	{
		if (Closing || Shared::CoreShuttingDown()) return true;
		if (GIsSavingPackage || IsGarbageCollecting()) return true;
		FinishJob();
		const double Now = FPlatformTime::Seconds();
		if (Now - LastPoll > .25) { Poll(); LastPoll = Now; }
		TArray<FVector> Cameras; BF6Ext::GetBuildViewportLocations(Cameras);
		const bool Enabled = Placed::Enabled() && Shared::Mode() > 0;
		for (FRecord& R : Records)
		{
			if (const FAsset* Asset = Assets.Find(R.Wanted))
			{
				if (Enabled && Asset->Mesh && R.Wanted != R.Installed) Install(R, Asset->Mesh);
				R.Error = Asset->Error;
			}
			bool High = false;
			if (UStaticMeshComponent* C = R.Component.Get())
			{
				const float Distance = Placed::CullMetres() * 100.f * (R.Near ? 1.03f : .97f) + C->Bounds.SphereRadius;
				const bool Ready = Enabled && !R.Wanted.IsEmpty() && R.Installed == R.Wanted && R.Error.IsEmpty();
				High = Ready && (Placed::NoCull() || (Cameras.IsEmpty() && R.Near));
				if (Ready) for (const FVector& Camera : Cameras)
					if (FVector::DistSquared(Camera, C->Bounds.Origin) <= double(Distance) * Distance) High = true;
				for (int32 I = 0; I < C->GetNumMaterials(); ++I)
				{
					UMaterialInterface* Material = Shared::Mode() == 1 ? Shared::ClayMaterial() : C->GetStaticMesh()->GetMaterial(I);
					if (C->GetMaterial(I) != Material) C->SetMaterial(I, Material);
				}
			}
			Show(R, High);
		}
		if (Worker.IsValid() || (Shared::IsBuilding() && !FinishingBuild) || Shared::CoreBusy()) return true;
		// A visible loadout menu may request its one selected item while the map
		// stays in Low Poly. Opening that menu must not dress every scene spawner.
		FRecord* Next = Records.FindByPredicate([&](const FRecord& R)
		{
			const bool Demanded = Now - LastPreviewDemand < 2 && R.Actor.IsValid() && R.Actor->GetPathName() == PreviewDemandId;
			return (Enabled || Demanded) && !R.Wanted.IsEmpty() && !Assets.Contains(R.Wanted);
		});
		if (!Next && (!NeedCatalogue || Catalogue)) return true;
		FString Error;
		if (!Shared::EnsureCoreOpen(Error)) { LastError = Error; NeedCatalogue = false; return true; }
		NeedCatalogue = false;
		StartJob(Next && Catalogue ? Next->Wanted : FString(), Next ? Next->Request : FRequest());
		return true;
	}
}

void RequestCatalogue() { NeedCatalogue = true; }
bool FinishBuildStep(int32& Done, int32& Total)
{
	TGuardValue<bool> Guard(FinishingBuild,true);
	LastPoll = 0; Tick(0.f);
	Done=Total=0;
	if(!Placed::Enabled()||Shared::Mode()==0) return true;
	for(const FRecord& R:Records)
	{
		if(!R.Actor.IsValid()||R.Wanted.IsEmpty()) continue;
		++Total;
		if(R.Wanted==R.Installed||!R.Error.IsEmpty()) ++Done;
	}
	return Done==Total&&!Worker.IsValid();
}
FString Status()
{
	if (!LastError.IsEmpty()) return LastError;
	if (!Catalogue) return TEXT("Reading available loadouts...");
	return FString::Printf(TEXT("%d characters, %d outfits, %d equipment items; %s"), Catalogue->Characters.Num(), Catalogue->Outfits.Num(), Catalogue->Items.Num(), Worker.IsValid() ? TEXT("building preview") : TEXT("ready"));
}
FString SelectionStatus(const FString& Id)
{
	for (const FRecord& R : Records) if (R.Actor.IsValid() && R.Actor->GetPathName() == Id)
	{
		if (!R.Error.IsEmpty()) return R.Error;
		if (const FAsset* Asset = Assets.Find(R.Wanted)) if (Asset->Mesh) return TEXT("Preview ready");
		return R.Wanted == R.Installed ? TEXT("Preview ready") : TEXT("Preview queued");
	}
	return TEXT("Select a soldier, loot or vehicle spawner.");
}
UStaticMesh* PreviewMesh(const FString& Id, TMap<FString, FVector3f>& OutAnchors)
{
	OutAnchors.Reset();
	PreviewDemandId = Id; LastPreviewDemand = FPlatformTime::Seconds();
	for (const FRecord& R : Records) if (R.Actor.IsValid() && R.Actor->GetPathName() == Id)
		if (const FAsset* Asset = Assets.Find(R.Wanted)) { OutAnchors = Asset->SlotAnchors; return Asset->Mesh; }
	return nullptr;
}
TArray<FChoice> Choices(const FString& Field, const BF6Ext::FObjectPreview& Object)
{
	if (Field == TEXT("faction")) return {{TEXT("alliance"), TEXT("NATO")}, {TEXT("pax"), TEXT("PAX")}};
	if (Field == TEXT("role")) return {{TEXT("assault"), TEXT("Assault")}, {TEXT("engineer"), TEXT("Engineer")}, {TEXT("support"), TEXT("Support")}, {TEXT("recon"), TEXT("Recon")}};
	if (!Catalogue) return {};
	const FRequest R = RequestFor(Object.Type, Object.Values);
	if (Field.StartsWith(TEXT("attachment_")))
	{
		TArray<FChoice> AttachmentChoices{{TEXT(""),TEXT("Factory")}};
		if (const TArray<FChoice>* All = Catalogue->Attachments.Find(R.Item))
			for (const FChoice& C : *All) if (C.Group == Field.Mid(11)) AttachmentChoices.Add(C);
		return AttachmentChoices;
	}
	if (Field == TEXT("character")) return Catalogue->Characters;
	TArray<FChoice> Out;
	if (Field == TEXT("outfit")) for (const FChoice& I : Catalogue->Outfits) { if (I.Group == R.Character) Out.Add(I); }
	if (Field == TEXT("item") || Field == TEXT("gadget") || Field == TEXT("throwable"))
		for (const FChoice& I : Catalogue->Items)
		{
			const FString Kind = Field == TEXT("item") ? TEXT("Weapon") : Field == TEXT("gadget") ? TEXT("Gadget") : TEXT("Throwable");
			if (I.Group == Kind) Out.Add(I);
		}
	if (Field == TEXT("vehicle"))
	{
		TArray<FString> Allowed; BF6Ext::PlaceableTypes(Allowed);
		TSet<FString> Ids; for (const FString& T : Allowed) if (T.StartsWith(TEXT("VEH_"))) Ids.Add(VehicleForType(T));
		for (const FChoice& I : Catalogue->Vehicles) if (Ids.Contains(I.Id)) Out.Add(I);
	}
	if (Field == TEXT("skin"))
	{
		Out.Add({TEXT(""), TEXT("Factory")});
		const FString Vehicle = Object.Type.StartsWith(TEXT("VEH_")) ? VehicleForType(Object.Type) : R.Vehicle;
		for (const FChoice& I : Catalogue->VehicleSkins) if (I.Group == Vehicle) Out.Add(I);
	}
	return Out;
}
void Start()
{
	Closing = false;
	StartEquipmentCards();
	// ShutdownModule runs after the UObject system has closed during editor
	// exit. Release rooted previews while those objects are still alive.
	PreExit = FCoreDelegates::OnEnginePreExit.AddStatic(&Stop);
	BF6Ext::FPieEntry Pill; Pill.Id = TEXT("HighPoly.Loadout"); Pill.Label = TEXT("LOADOUT");
	Pill.Sub = TEXT("Soldiers, loot and vehicles"); Pill.Order = 1050;
	Pill.IsAvailable = []{ return BF6Ext::IsEditing(); };
	Pill.OnPick = [](FVector2D){ OpenPanel(); };
	BF6Ext::RegisterPieEntry(Pill);
	BF6Ext::RegisterObjectEditor(TEXT("HighPoly.Loadout"), []
	{
		const auto Object = BF6Ext::SelectedObjectPreview();
		if (Object.Id.IsEmpty() || !Handles(Object.Type)) return false;
		OpenPanel(); return true;
	});
	Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
	MapClose = BF6Ext::OnMapClosing().AddLambda([](const FString&){ ClearRecords(); });
}
void Stop()
{
	if (Closing) return;
	FCoreDelegates::OnEnginePreExit.Remove(PreExit); PreExit.Reset();
	Closing = true;
	StopEquipmentCards();
	if (Ticker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
	if (MapClose.IsValid()) BF6Ext::OnMapClosing().Remove(MapClose);
	BF6Ext::CloseAddonWindow(TEXT("HighPoly.Loadout"));
	BF6Ext::UnregisterPieEntry(TEXT("HighPoly.Loadout"));
	BF6Ext::UnregisterObjectEditor(TEXT("HighPoly.Loadout"));
	if (Worker.IsValid()) Worker.Wait();
	Worker = TFuture<TSharedPtr<FJob, ESPMode::ThreadSafe>>();
	if (OwnBusy) { Shared::SetCoreBusy(false); OwnBusy = false; }
	ClearRecords();
	Catalogue.Reset();
}
static FAutoConsoleCommand OpenCmd(TEXT("BF6.HighPoly.Loadout.Open"), TEXT("Open the selected spawner's loadout menu."), FConsoleCommandDelegate::CreateStatic(&OpenPanel));
static FAutoConsoleCommand StatusCmd(TEXT("BF6.HighPoly.Loadout.Status"), TEXT("Report loadout preview work."), FConsoleCommandDelegate::CreateLambda([]
{
	UE_LOG(LogBF6Loadout, Display, TEXT("%s"), *Status());
	for (const FRecord& R : Records) if (R.Actor.IsValid()) UE_LOG(LogBF6Loadout, Display, TEXT("%s: %s"), *R.Actor->GetActorLabel(), *SelectionStatus(R.Actor->GetPathName()));
}));
}
