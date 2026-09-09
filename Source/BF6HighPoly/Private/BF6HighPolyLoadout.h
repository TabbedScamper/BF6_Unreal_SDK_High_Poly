#pragma once
#include "CoreMinimal.h"
#include "BF6HighPolyCore.h"
#include "BF6SDKExtension.h"

namespace BF6HP::Loadout
{
	struct FChoice
	{
		FString Id, Label, Group, Asset, Bundle, Variation;
	};
	struct FCatalogue
	{
		TArray<FChoice> Characters, Outfits, Items, Vehicles, VehicleSkins;
		TSet<FString> Resources;
		TArray<FString> Ebx;
		TMap<FString, TArray<FChoice>> Attachments;
	};
	struct FRequest
	{
		FString Type, Character = TEXT("cha0001wisp"), Outfit = TEXT("001");
		FString Faction = TEXT("alliance"), Role = TEXT("assault");
		FString Item = TEXT("carbine/m4a1"), Vehicle, Skin;
		TMap<FString, FString> Attachments;
		FString Key() const;
	};
	struct FDecoded
	{
		TArray<FCore::FSection> Sections;
		FString Error, Detail;
	};
	// Reader calls are made on one owned worker while holding CoreMutex.
	TSharedPtr<FCatalogue, ESPMode::ThreadSafe> ReadCatalogue(FCore& Core, FString& Error);
	void ReadWeaponAttachments(FCore& Core, FCatalogue& Catalogue, const FString& Item);
	FDecoded Decode(FCore& Core, const FCatalogue& Catalogue, const FRequest& Request);
	bool Handles(const FString& Type);
	FRequest RequestFor(const FString& Type, const TMap<FString, FString>& Values);
	void Start();
	void Stop();
	void OpenPanel();
	TArray<FChoice> Choices(const FString& Field, const BF6Ext::FObjectPreview& Object);
	FString Status();
	FString SelectionStatus(const FString& Id);
	void RequestCatalogue();
	bool FinishBuildStep(int32& Done, int32& Total);
}
