#include "BF6HighPolyLoadout.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
THIRD_PARTY_INCLUDES_START
#include "bf6_core.h"
THIRD_PARTY_INCLUDES_END

namespace BF6HP::Loadout
{
void ReadWeaponAttachments(FCore& Core, FCatalogue& AttachmentCatalogue, const FString& ItemId)
{
	if (AttachmentCatalogue.Attachments.Contains(ItemId)) return;
	TArray<FChoice>& Choices = AttachmentCatalogue.Attachments.Add(ItemId);
	const FChoice* Item = AttachmentCatalogue.Items.FindByPredicate([&](const FChoice& I){ return I.Id == ItemId; });
	if (!Item || Item->Group != TEXT("Weapon")) return;
	const auto Read = reinterpret_cast<decltype(&bf6_weapon_attachment_catalogue)>(FPlatformProcess::GetDllExport(Core.DllHandle(), TEXT("bf6_weapon_attachment_catalogue")));
	if (!Read) return;
	const FString Weapon = FPaths::GetCleanFilename(ItemId);
	const FTCHARToUTF8 W(*Weapon);
	const int32 Count = Read(Core.Handle(), W.Get(), nullptr, 0);
	if (Count < 1 || Count > 8192) return;
	TArray<bf6_attachment_catalogue_row> Rows; Rows.SetNumZeroed(Count);
	if (Read(Core.Handle(), W.Get(), Rows.GetData(), Count) != Count) return;
	const auto Fold = [](const FString& S) { FString R; for (TCHAR C : S) if (FChar::IsAlnum(C)) R.AppendChar(FChar::ToLower(C)); return R; };
	const TMap<FString,FString> Prefixes{{TEXT("scp"),TEXT("Scope")},{TEXT("sca"),TEXT("Scope")},
		{TEXT("brl"),TEXT("Barrel")},{TEXT("mzl"),TEXT("Muzzle")},{TEXT("mag"),TEXT("Magazine")},
		{TEXT("amo"),TEXT("Ammo")},{TEXT("erg"),TEXT("Ergonomic")},{TEXT("btm"),TEXT("Bottom")},
		{TEXT("top"),TEXT("Top")},{TEXT("lft"),TEXT("Left")},{TEXT("rgt"),TEXT("Right")}};
	const TArray<FString> Enums = BF6Ext::WeaponAttachmentItems();
	TMap<FString,TSet<FString>> NativeTokens;
	const FString Stem = TEXT("attachment_") + Weapon + TEXT("_");
	for (const FString& Path : AttachmentCatalogue.Ebx)
	{
		if (!Path.StartsWith(Item->Asset + TEXT("/"))) continue;
		const FString Leaf = FPaths::GetCleanFilename(Path);
		if (!Leaf.StartsWith(Stem)) continue;
		FString Slot, Token; if (!Leaf.Mid(Stem.Len()).Split(TEXT("_"), &Slot, &Token) || !Prefixes.Contains(Slot)) continue;
		NativeTokens.FindOrAdd(Slot).Add(Token);
	}
	// Both joins require unique exact identifiers after punctuation folding.
	// A similar-looking display name must never select a different game part.
	TMap<FString,FChoice> Candidates;
	TSet<FString> Ambiguous;
	for (const auto& Row : Rows)
	{
		FString Slot = UTF8_TO_TCHAR(Row.slot); if (Slot == TEXT("opt")) Slot = TEXT("sca");
		const FString* Prefix = Prefixes.Find(Slot); const TSet<FString>* Tokens = NativeTokens.Find(Slot);
		if (!Prefix || !Tokens) continue;
		TArray<FString> Matches;
		for (const FString& Token : *Tokens)
			if (Fold(Token) == UTF8_TO_TCHAR(Row.name_key) || Fold(Token) == UTF8_TO_TCHAR(Row.ad_stem)) Matches.Add(Token);
		if (Matches.Num() != 1) continue;
		TArray<FString> PublicMatches;
		for (const FString& Enum : Enums)
			if (Enum.StartsWith(*Prefix + TEXT("_")) && Fold(Enum.Mid(Prefix->Len()+1)) == Fold(UTF8_TO_TCHAR(Row.name))) PublicMatches.Add(Enum);
		if (PublicMatches.Num() != 1) continue;
		const FString Key = Slot + TEXT(":") + PublicMatches[0];
		FChoice Choice{PublicMatches[0],UTF8_TO_TCHAR(Row.name),Slot,Item->Asset,Matches[0]};
		if (const FChoice* Previous = Candidates.Find(Key)) { if (Previous->Bundle != Choice.Bundle) Ambiguous.Add(Key); }
		else Candidates.Add(Key, MoveTemp(Choice));
	}
	for (const auto& Pair : Candidates) if (!Ambiguous.Contains(Pair.Key)) Choices.Add(Pair.Value);
	Choices.Sort([](const FChoice& A,const FChoice& B){return A.Label < B.Label;});
}
}

