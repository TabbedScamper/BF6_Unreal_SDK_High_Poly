#include "BF6HighPolyLoadout.h"
#include "HAL/PlatformProcess.h"
#include "Math/QuatRotationTranslationMatrix.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/AutomationTest.h"
THIRD_PARTY_INCLUDES_START
#include "bf6_core.h"
THIRD_PARTY_INCLUDES_END

namespace BF6HP::Loadout
{
namespace
{
	template<typename T> T Export(FCore& C, const TCHAR* Name)
	{
		return reinterpret_cast<T>(FPlatformProcess::GetDllExport(C.DllHandle(), Name));
	}
	FString Leaf(FString S) { S.RemoveFromEnd(TEXT(".ebx")); return FPaths::GetCleanFilename(S); }
	TArray<FString> Names(FCore& C, const FString& Query, bool Resources = false)
	{
		using Fn = decltype(&bf6_list_ebx);
		Fn List = Export<Fn>(C, Resources ? TEXT("bf6_list_res") : TEXT("bf6_list_ebx"));
		TArray<FString> Out;
		if (!List) return Out;
		const int32 N = List(C.Handle(), TCHAR_TO_UTF8(*Query), nullptr, 0);
		if (N < 1 || N > 1000000) return Out;
		TArray<bf6_asset> Rows; Rows.SetNumZeroed(N);
		const int32 Got = FMath::Clamp(List(C.Handle(), TCHAR_TO_UTF8(*Query), Rows.GetData(), N), 0, N);
		for (int32 I = 0; I < Got; ++I) if (Rows[I].name) Out.Add(UTF8_TO_TCHAR(Rows[I].name));
		Out.Sort(); return Out;
	}
	FString ExactLeaf(const TArray<FString>& Rows, const FString& Wanted, const FString& Within = FString())
	{
		FString Found;
		for (const FString& Path : Rows)
		{
			if ((!Within.IsEmpty() && !Path.StartsWith(Within + TEXT("/"))) || Leaf(Path) != Wanted) continue;
			if (!Found.IsEmpty() && Found != Path) return FString();
			Found = Path;
		}
		return Found;
	}
	FMatrix44f Matrix(const float* P)
	{
		FMatrix44f Out = FMatrix44f::Identity;
		for (int32 R = 0; R < 4; ++R) for (int32 Col = 0; Col < 3; ++Col) Out.M[R][Col] = P[R * 3 + Col];
		return Out;
	}
	void Transform(TArray<FCore::FSection>& Sections, const FMatrix44f& M)
	{
		const FMatrix44f N = M.Inverse().GetTransposed();
		for (FCore::FSection& S : Sections)
		{
			for (FVector3f& P : S.Pos) P = FVector3f(M.TransformPosition(P));
			for (FVector3f& V : S.Nrm) V = FVector3f(N.TransformVector(V)).GetSafeNormal();
		}
	}
	struct FPose
	{
		TArray<bf6_anim_binding> Bindings;
		TArray<float> Channels;
		bf6_anim_binding_stats Stats{};
	};
	constexpr const TCHAR* Rig = TEXT("animations/glacier/global/rigging/soldier_3p.rig");
	constexpr const TCHAR* Skeleton = TEXT("common/characters/_soldier/ske_soldier_3p");
	bool PoseSamples(FCore& C, const FString& Role, FPose& Out, FString& Error)
	{
		auto Bind = Export<decltype(&bf6_anim_bindings)>(C, TEXT("bf6_anim_bindings"));
		auto Open = Export<decltype(&bf6_anim_clip_open)>(C, TEXT("bf6_anim_clip_open"));
		auto Sample = Export<decltype(&bf6_anim_clip_sample)>(C, TEXT("bf6_anim_clip_sample"));
		auto Free = Export<decltype(&bf6_free)>(C, TEXT("bf6_free"));
		if (!Bind || !Open || !Sample || !Free) { Error = TEXT("Update the game reader to enable posed soldiers."); return false; }
		const FString ClipPath = TEXT("animations/glacier/assets/frontend/mainmenu/loadout/ui_frontend_standing_idle_") + Role + TEXT("_01");
		const int32 Count = Bind(C.Handle(), TCHAR_TO_UTF8(*ClipPath), TCHAR_TO_UTF8(Rig), TCHAR_TO_UTF8(Skeleton), nullptr, 0, &Out.Stats);
		if (Count < 1 || Count > 4096) { Error = TEXT("The selected soldier pose has no readable animation binding."); return false; }
		Out.Bindings.SetNumZeroed(Count);
		if (Bind(C.Handle(), TCHAR_TO_UTF8(*ClipPath), TCHAR_TO_UTF8(Rig), TCHAR_TO_UTF8(Skeleton), Out.Bindings.GetData(), Count, &Out.Stats) != Count) return false;
		bf6_anim_clip* Clip = Open(C.Handle(), TCHAR_TO_UTF8(*ClipPath));
		if (!Clip) { Error = TEXT("The selected soldier pose is unavailable."); return false; }
		ON_SCOPE_EXIT { Free(C.Handle(), Clip); };
		if (Clip->channel_count != Count || Clip->key_time_count < 1) return false;
		Out.Channels.SetNumZeroed(Count * 4);
		// A static editor representation needs one authored sample, not all 1139 frames.
		return Sample(C.Handle(), Clip, 0, Out.Channels.GetData(), nullptr) != 0;
	}
	bool SkinFor(FCore& C, const FCatalogue& MeshCatalogue, const FString& Mesh, const FPose& Pose,
		TArray<FMatrix44f>& Skin, int32& RigBones, FMatrix44f& Weapon, FVector3f& FootAnchor, FString& Error)
	{
		auto Compose = Export<decltype(&bf6_skeleton_compose)>(C, TEXT("bf6_skeleton_compose"));
		auto Free = Export<decltype(&bf6_free)>(C, TEXT("bf6_free"));
		auto Imports = Export<decltype(&bf6_armory_ebx_instance_imports)>(C, TEXT("bf6_armory_ebx_instance_imports"));
		if (!Compose || !Free) return false;
		FString Renderbones;
		// Prefer the mesh asset's actual Renderbones import; some outfits reuse another rig.
		FString MeshAsset = Mesh; MeshAsset.RemoveFromEnd(TEXT("_mesh"));
		if (Imports)
		{
			bf6_ebx_instance_import Rows[32]{};
			const int32 Count = Imports(C.Handle(), TCHAR_TO_UTF8(*MeshAsset), 0, Rows, 32);
			for (int32 I = 0; I < FMath::Min(Count, 32); ++I)
				if (Rows[I].field_hash == 0xA38BC860 && Rows[I].path[0]) Renderbones = UTF8_TO_TCHAR(Rows[I].path);
		}
		Renderbones.RemoveFromEnd(TEXT(".ebx"));
		if (Renderbones.IsEmpty() && MeshCatalogue.Ebx.Contains(MeshAsset + TEXT("_renderbonesdata")))
			Renderbones = MeshAsset + TEXT("_renderbonesdata");
		bf6_skeleton* S = Compose(C.Handle(), TCHAR_TO_UTF8(Skeleton), Renderbones.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*Renderbones));
		if (!S) { Error = TEXT("The character render skeleton is unavailable."); return false; }
		ON_SCOPE_EXIT { Free(C.Handle(), S); };
		if (S->bone_count < 1 || S->bone_count > 8192) return false;
		RigBones = S->rig_bone_count;
		TArray<FMatrix44f> Local, Model; Local.SetNum(S->bone_count); Model.SetNum(S->bone_count); Skin.SetNum(S->bone_count);
		for (int32 I = 0; I < S->bone_count; ++I) Local[I] = Matrix(S->bones[I].local);
		int32 Applied = 0;
		for (const bf6_anim_binding& B : Pose.Bindings)
		{
			if (B.bone < 0 || B.bone >= S->bone_count) continue;
			if (B.channel < 0 || B.channel * 4 + 3 >= Pose.Channels.Num()) return false;
			const float* V = Pose.Channels.GetData() + B.channel * 4;
			if (B.component == BF6_ANIM_DOF_QUATERNION)
			{
				const FQuat4f Q(V[0], V[1], V[2], V[3]);
				const FMatrix44f Rotation = FQuatRotationTranslationMatrix44f(Q.GetNormalized(), FVector3f::ZeroVector);
				for (int32 R = 0; R < 3; ++R) for (int32 Col = 0; Col < 3; ++Col) Local[B.bone].M[R][Col] = Rotation.M[R][Col];
				++Applied;
			}
			else if (B.component == BF6_ANIM_DOF_VECTOR3)
			{
				for (int32 Col = 0; Col < 3; ++Col) Local[B.bone].M[3][Col] = V[Col];
				++Applied;
			}
		}
		if (Applied != Pose.Stats.quaternion_bones + Pose.Stats.vector_bones) { Error = TEXT("The soldier pose did not bind completely."); return false; }
		bool HaveWeapon = false;
		int32 RightHand = INDEX_NONE, RightGrip = INDEX_NONE, FootCount = 0;
		FootAnchor = FVector3f::ZeroVector;
		for (int32 I = 0; I < S->bone_count; ++I)
		{
			const int32 Parent = S->bones[I].parent;
			if (Parent >= I) return false;
			Model[I] = Parent < 0 ? Local[I] : Local[I] * Model[Parent];
			Skin[I] = Matrix(S->bones[I].inverse) * Model[I];
			if (S->bones[I].name && FCStringAnsi::Strcmp(S->bones[I].name, "Wep_Align") == 0)
			{ Weapon = Model[I]; HaveWeapon = true; }
			if (S->bones[I].name && FCStringAnsi::Strcmp(S->bones[I].name, "RightHand") == 0) RightHand = I;
			if (S->bones[I].name && FCStringAnsi::Strcmp(S->bones[I].name, "Wep_IK_RightHand") == 0) RightGrip = I;
			if (S->bones[I].name && (FCStringAnsi::Strcmp(S->bones[I].name, "LeftFoot") == 0 || FCStringAnsi::Strcmp(S->bones[I].name, "RightFoot") == 0))
			{ FootAnchor += Model[I].GetOrigin(); ++FootCount; }
		}
		if (FootCount) FootAnchor /= float(FootCount);
		if (HaveWeapon && RightHand != INDEX_NONE && RightGrip != INDEX_NONE)
		{
			// Front-end clips carry a separate weapon IK target. Its pose-space
			// offset is not already applied to the rendered arm. For this static
			// preview, join the authored grip frame to the actual wrist frame.
			// Directly using Wep_Align left every rifle floating above the hands.
			Weapon = Weapon * Model[RightGrip].Inverse() * Model[RightHand];
			return true;
		}
		Error = TEXT("The pose has no complete weapon-to-hand binding."); return false;
	}
	FString ModelDefinition(FCore& C, const FCatalogue& Cat, const FChoice& Item)
	{
		auto Imports = Export<decltype(&bf6_armory_ebx_instance_imports)>(C, TEXT("bf6_armory_ebx_instance_imports"));
		TSet<FString> Targets;
		for (const FString& Path : Cat.Ebx)
		{
			if (FPaths::GetPath(Path) != Item.Asset || !Leaf(Path).StartsWith(TEXT("cust_")) || !Imports) continue;
			bf6_ebx_instance_import Rows[64]{};
			const int32 N = Imports(C.Handle(), TCHAR_TO_UTF8(*Path), 0, Rows, 64);
			for (int32 I = 0; I < FMath::Min(N, 64); ++I)
				if (Rows[I].field_hash == 0xff2bbdd1 && Rows[I].path[0])
				{ FString P = UTF8_TO_TCHAR(Rows[I].path); P.RemoveFromEnd(TEXT(".ebx")); Targets.Add(P); }
		}
		if (Targets.Num() == 1) return *Targets.CreateConstIterator();
		if (Targets.Num() > 1) return FString();
		return ExactLeaf(Cat.Ebx, TEXT("md_") + Leaf(Item.Id), Item.Asset);
	}
	bool ItemMesh(FCore& C, const FCatalogue& Cat, const FString& Id, const TMap<FString,FString>& Attachments, TArray<FCore::FSection>& Out, FString& Error)
	{
		const FChoice* Item = Cat.Items.FindByPredicate([&](const FChoice& I){ return I.Id == Id; });
		if (!Item) { Error = TEXT("This item is absent from the installed equipment catalogue."); return false; }
		const FString Md = ModelDefinition(C, Cat, *Item);
		if (Md.IsEmpty()) { Error = TEXT("This item has no unambiguous model definition."); return false; }
		auto Factory = Export<decltype(&bf6_weapon_factory_fits)>(C, TEXT("bf6_weapon_factory_fits"));
		auto Assembly = Export<decltype(&bf6_weapon_configured_assembly)>(C, TEXT("bf6_weapon_configured_assembly"));
		if (!Assembly) { Error = TEXT("The reader cannot assemble configured equipment."); return false; }
		const FString Equipment = ExactLeaf(Cat.Ebx, TEXT("equipment_") + Leaf(Id), Item->Asset);
		bf6_weapon_fit Fits[64]{}; int32 NFits = 0;
		if (Factory && !Equipment.IsEmpty()) NFits = Factory(C.Handle(), TCHAR_TO_UTF8(*Equipment), Fits, 64);
		if (NFits < 0 || NFits > 64) { Error = TEXT("The item's factory configuration is unreadable."); return false; }
		TArray<TUniquePtr<FTCHARToUTF8>> FitStrings;
		const auto Utf8 = [&](const FString& S) { FitStrings.Add(MakeUnique<FTCHARToUTF8>(*S)); return FitStrings.Last()->Get(); };
		const TArray<FChoice>* Available = Cat.Attachments.Find(Id);
		for (const auto& Pair : Attachments)
		{
			const FChoice* Choice = Available ? Available->FindByPredicate([&](const FChoice& V){ return V.Group == Pair.Key && V.Id == Pair.Value; }) : nullptr;
			if (!Choice) { Error = TEXT("An attachment is unavailable for this weapon. Choose it again in Loadout."); return false; }
			int32 Index = INDEX_NONE;
			for (int32 I=0;I<NFits;++I) if (Fits[I].slot && Pair.Key == UTF8_TO_TCHAR(Fits[I].slot)) { Index=I; break; }
			if (Index == INDEX_NONE) { if (NFits == 64) { Error=TEXT("Too many configured attachments."); return false; } Index=NFits++; }
			Fits[Index] = {Utf8(Pair.Key),Utf8(Choice->Bundle)};
		}
		bf6_weapon_part_pose Parts[256]{}; bf6_bone_xform Bones[1024]{}; int BoneCount = 0;
		const int32 Count = Assembly(C.Handle(), TCHAR_TO_UTF8(*Md), Fits, NFits, Parts, 256, Bones, 1024, &BoneCount);
		if (Count < 1 || Count > 256 || BoneCount < 0 || BoneCount > 1024) { Error = TEXT("No complete configured item assembly was returned."); return false; }
		TArray<FMatrix44f> Skin; for (int32 I = 0; I < BoneCount; ++I) Skin.Add(Matrix(Bones[I].m));
		// Copy context-owned names before mesh reads can mutate native scratch storage.
		struct FPart { FString Mesh, Bundle; FMatrix44f Attach; bool HasAttach; };
		TArray<FPart> Copy;
		for (int32 I = 0; I < Count; ++I) if (Parts[I].mesh)
			Copy.Add({ UTF8_TO_TCHAR(Parts[I].mesh), Parts[I].bundle ? UTF8_TO_TCHAR(Parts[I].bundle) : FString(), Matrix(Parts[I].attach_transform), Parts[I].has_attach_transform != 0 });
		for (const FPart& P : Copy)
		{
			TArray<FCore::FSection> S;
			if (!C.ReadMesh(P.Mesh, S, P.Bundle, FString(), Skin.IsEmpty() ? nullptr : &Skin)) { Error = C.Error; return false; }
			if (P.HasAttach) Transform(S, P.Attach);
			Out.Append(MoveTemp(S));
		}
		return !Out.IsEmpty();
	}
}

FString FRequest::Key() const
{
	FString Result = FString::Join(TArray<FString>{Type, Character, Outfit, Faction, Role, Item, Vehicle, Skin}, TEXT("\n"));
	TArray<FString> Keys; Attachments.GetKeys(Keys); Keys.Sort();
	for (const FString& K : Keys) Result += TEXT("\n") + K + TEXT("=") + Attachments[K];
	return Result;
}
bool Handles(const FString& Type)
{
	return Type == TEXT("PlayerSpawner") || Type == TEXT("HQ_PlayerSpawner") || Type == TEXT("AI_Spawner")
		|| Type == TEXT("SpawnPoint") || Type == TEXT("LootSpawner") || Type == TEXT("VehicleSpawner") || Type.StartsWith(TEXT("VEH_"));
}
FRequest RequestFor(const FString& Type, const TMap<FString, FString>& Values)
{
	FRequest R; R.Type = Type;
	if (Type == TEXT("AI_Spawner")) R.Faction = TEXT("pax");
	const auto Get = [&](const TCHAR* K, FString& V){ if (const FString* P = Values.Find(K)) V = *P; };
	Get(TEXT("character"), R.Character); Get(TEXT("outfit"), R.Outfit); Get(TEXT("faction"), R.Faction);
	Get(TEXT("role"), R.Role); Get(TEXT("item"), R.Item); Get(TEXT("vehicle"), R.Vehicle); Get(TEXT("skin"), R.Skin);
	for (const auto& Pair : Values) if (Pair.Key.StartsWith(TEXT("attachment_")) && !Pair.Value.IsEmpty()) R.Attachments.Add(Pair.Key.Mid(11),Pair.Value);
	return R;
}

TSharedPtr<FCatalogue, ESPMode::ThreadSafe> ReadCatalogue(FCore& C, FString& Error)
{
	auto Mount = Export<decltype(&bf6_mount_frontend)>(C, TEXT("bf6_mount_frontend"));
	char Why[512]{};
	if (!Mount || !Mount(C.Handle(), Why, 512)) { Error = Why[0] ? UTF8_TO_TCHAR(Why) : TEXT("The reader cannot mount front-end equipment."); return nullptr; }
	auto Cat = MakeShared<FCatalogue, ESPMode::ThreadSafe>();
	Cat->Ebx = Names(C, TEXT("common/hardware/"));
	Cat->Ebx.Append(Names(C, TEXT("common/characters/")));
	const TArray<FString> Bundles = Names(C, TEXT("pf_cha"));
	Cat->Ebx.Append(Bundles);
	Cat->Ebx.Append(Names(C, TEXT("vse")));
	Cat->Ebx.Append(Names(C, TEXT("ov_veh")));
	Cat->Ebx.Append(Names(C, TEXT("dpf_veh")));
	Cat->Resources.Append(Names(C, TEXT("common/hardware/"), true));
	Cat->Resources.Append(Names(C, TEXT("common/characters/"), true));
	TSet<FString> Items, Characters, Outfits, Vehicles;
	for (const FString& P : Cat->Ebx)
	{
		TArray<FString> Seg; P.ParseIntoArray(Seg, TEXT("/"));
		if (Seg.Num() >= 6 && Seg[0] == TEXT("common") && Seg[1] == TEXT("hardware")
			&& (Seg[2] == TEXT("weapons") || Seg[2] == TEXT("gadgets")) && !Seg[3].StartsWith(TEXT("_")) && !Seg[4].StartsWith(TEXT("_")))
		{
			const FString Id = Seg[3] + TEXT("/") + Seg[4];
			if (!Items.Contains(Id))
			{
				Items.Add(Id); FChoice I; I.Id = Id; I.Label = Seg[4].ToUpper();
				I.Asset = FString::Join(TArray<FString>{Seg[0], Seg[1], Seg[2], Seg[3], Seg[4]}, TEXT("/"));
				I.Group = Seg[2] == TEXT("weapons") ? TEXT("Weapon")
					: (Seg[3].Contains(TEXT("grenade")) || Seg[3].Contains(TEXT("throw"))) ? TEXT("Throwable") : TEXT("Gadget");
				Cat->Items.Add(MoveTemp(I));
			}
		}
	}
	for (const FString& P : Bundles)
	{
		if (P.Contains(TEXT("/")) || !P.StartsWith(TEXT("pf_cha")) || !P.EndsWith(TEXT("_bundle_3p"))) continue;
		FString Char, Rest;
		if (!P.Mid(3).Split(TEXT("_set_"), &Char, &Rest) || Rest.Len() < 4) continue;
		const FString Set = Rest.Left(3), Key = Char + TEXT("/") + Set;
		const FString Root = TEXT("common/characters/mp/main/") + Char;
		if (!Cat->Resources.Contains(Root + TEXT("/set/set_001/") + Char + TEXT("_set_001_mesh"))) continue;
		if (!Characters.Contains(Char)) { Characters.Add(Char); Cat->Characters.Add({Char, Char.Mid(7).ToUpper(), TEXT("Character"), Root}); }
		if (!Outfits.Contains(Key)) { Outfits.Add(Key); Cat->Outfits.Add({Set, TEXT("Outfit ") + Set, Char, Root + TEXT("/set/set_") + Set, TEXT("win32/") + P}); }
	}
	for (const FString& P : Cat->Resources)
	{
		TArray<FString> S; P.ParseIntoArray(S, TEXT("/"));
		if (S.Num() != 7 || S[2] != TEXT("vehicles") || S[5] != TEXT("art")
			|| S[6] != TEXT("ob_veh_") + S[3] + TEXT("_") + S[4] + TEXT("_base_mesh")) continue;
		const FString Id = S[3] + TEXT("/") + S[4];
		if (!Vehicles.Contains(Id)) { Vehicles.Add(Id); Cat->Vehicles.Add({Id, S[4].ToUpper(), S[3], P, TEXT("md_veh_") + S[3] + TEXT("_") + S[4] + TEXT("_bundle_3p")}); }
	}
	auto WeaponNames = Export<decltype(&bf6_weapon_names)>(C, TEXT("bf6_weapon_names"));
	if (WeaponNames)
	{
		TArray<bf6_weapon_name_row> Rows; Rows.SetNumZeroed(512);
		const int32 Count = FMath::Clamp(WeaponNames(C.Handle(), Rows.GetData(), Rows.Num()), 0, Rows.Num());
		for (FChoice& I : Cat->Items) for (int32 N = 0; N < Count; ++N)
			if (Leaf(I.Id) == UTF8_TO_TCHAR(Rows[N].weapon) && Rows[N].name[0]) { I.Label = UTF8_TO_TCHAR(Rows[N].name); break; }
	}
	for (const FChoice& V : Cat->Vehicles)
	{
		const FString Base = FPaths::GetPath(V.Asset) + TEXT("/skins/");
		for (const FString& P : Cat->Ebx)
		{
			if (!P.StartsWith(Base) || !Leaf(P).StartsWith(TEXT("ov_")) || !Leaf(P).Contains(TEXT("base"))) continue;
			for (const FString& B : Cat->Ebx)
			{
				if (B.Contains(TEXT("/")) || !B.StartsWith(Leaf(P) + TEXT("_")) || !B.EndsWith(TEXT("_bundle_3p"))) continue;
				Cat->VehicleSkins.Add({P, FPaths::GetCleanFilename(FPaths::GetPath(P)).ToUpper(), V.Id, V.Asset, TEXT("win32/") + B, P});
				break;
			}
		}
	}
	Cat->Characters.Sort([](const FChoice& A, const FChoice& B){ return A.Label < B.Label; });
	Cat->Items.Sort([](const FChoice& A, const FChoice& B){ return A.Label < B.Label; });
	return Cat;
}

FDecoded Decode(FCore& C, const FCatalogue& Cat, const FRequest& R)
{
	FDecoded Out;
	if (R.Type == TEXT("LootSpawner"))
	{
		if (!ItemMesh(C, Cat, R.Item, R.Attachments, Out.Sections, Out.Error)) Out.Sections.Reset();
		return Out;
	}
	if (R.Type.StartsWith(TEXT("VEH_")) || R.Type == TEXT("VehicleSpawner"))
	{
		FString Want = R.Type.Mid(4).ToLower().Replace(TEXT("_"), TEXT(""));
		const FChoice* V = Cat.Vehicles.FindByPredicate([&](const FChoice& I){ return R.Vehicle.IsEmpty() ? Leaf(I.Id).Replace(TEXT("_"), TEXT("")) == Want : I.Id == R.Vehicle; });
		if (!V) { Out.Error = TEXT("No exact model for this vehicle. The SDK marker is retained."); return Out; }
		FString Bundle = V->Bundle, Variation;
		if (!R.Skin.IsEmpty())
		{
			const FChoice* S = Cat.VehicleSkins.FindByPredicate([&](const FChoice& I){ return I.Id == R.Skin && I.Group == V->Id; });
			if (!S) { Out.Error = TEXT("The selected vehicle skin is unavailable."); return Out; }
			Bundle = S->Bundle; Variation = S->Variation;
		}
		auto Assembly = Export<decltype(&bf6_weapon_configured_assembly)>(C,TEXT("bf6_weapon_configured_assembly"));
		const FString Root = FPaths::GetPath(FPaths::GetPath(V->Asset));
		const FString Md = ExactLeaf(Cat.Ebx,TEXT("md_veh_") + V->Id.Replace(TEXT("/"),TEXT("_")),Root);
		bf6_weapon_part_pose Parts[512]{}; bf6_bone_xform Bones[2048]{}; int BoneCount=0;
		const int32 Count = Assembly && !Md.IsEmpty() ? Assembly(C.Handle(),TCHAR_TO_UTF8(*Md),nullptr,0,Parts,512,Bones,2048,&BoneCount) : -1;
		if (Count<1 || Count>512 || BoneCount<0 || BoneCount>2048) { Out.Error=TEXT("The vehicle assembly is unavailable. The SDK marker is retained."); return Out; }
		struct FVehiclePart { FString Mesh,Bundle; FMatrix44f Attach; bool HasAttach; };
		TArray<FVehiclePart> Copy; TSet<FString> MeshNames;
		int32 Unresolved=0;
		for(int32 I=0;I<Count;++I) if(Parts[I].mesh) {
			if(Parts[I].has_attach_transform<0) { ++Unresolved; continue; }
			const FString Mesh=UTF8_TO_TCHAR(Parts[I].mesh); MeshNames.Add(Mesh); Copy.Add({Mesh,Parts[I].bundle?UTF8_TO_TCHAR(Parts[I].bundle):FString(),Matrix(Parts[I].attach_transform),Parts[I].has_attach_transform>0});
		}
		TArray<FMatrix44f> Skin; for(int32 I=0;I<BoneCount;++I) Skin.Add(Matrix(Bones[I].m));
		int32 Drawn=0;
		for(const auto& P:Copy)
		{
			// Some 3p bundle manifests carry both views of the same cockpit/gun.
			if(P.Mesh.Contains(TEXT("_1p_mesh")) && MeshNames.Contains(P.Mesh.Replace(TEXT("_1p_mesh"),TEXT("_3p_mesh")))) continue;
			TArray<FCore::FSection> S;
			const bool IsBase=P.Mesh==V->Asset;
			if(!C.ReadMesh(P.Mesh,S,IsBase&&!Variation.IsEmpty()?Bundle:P.Bundle,IsBase?Variation:FString(),Skin.IsEmpty()?nullptr:&Skin)) { Out.Sections.Reset(); Out.Error=C.Error; return Out; }
			if(P.HasAttach) Transform(S,P.Attach);
			Out.Sections.Append(MoveTemp(S)); ++Drawn;
		}
		Out.Detail = FString::Printf(TEXT("Vehicle assembly: %d parts"),Drawn);
		if(Unresolved) Out.Detail += FString::Printf(TEXT("; incomplete preview: %d part(s) have unresolved mounts"),Unresolved);
		return Out;
	}
	const FChoice* Character = Cat.Characters.FindByPredicate([&](const FChoice& I){ return I.Id == R.Character; });
	const FChoice* Outfit = Cat.Outfits.FindByPredicate([&](const FChoice& I){ return I.Group == R.Character && I.Id == R.Outfit; });
	if (!Character || !Outfit) { Out.Error = TEXT("The selected character or outfit is unavailable."); return Out; }
	FPose Pose;
	if (!PoseSamples(C, R.Role, Pose, Out.Error)) return Out;
	FMatrix44f Weapon = FMatrix44f::Identity;
	FVector3f FootAnchor = FVector3f::ZeroVector;
	const FString Base = Character->Asset + TEXT("/set/set_001/") + R.Character + TEXT("_set_001");
	TArray<FString> Meshes;
	for (const FString& P : Cat.Resources) if (P.StartsWith(Base) && P.EndsWith(TEXT("_mesh")) && !P.Contains(TEXT("_1p"))) Meshes.Add(P);
	Meshes.Sort();
	const FString FaceBundle = [&]()
	{
		for (const FString& P : Cat.Ebx) if (P.StartsWith(TEXT("pf_") + R.Character + TEXT("_face_001_")) && !P.Contains(TEXT("/")) && P.EndsWith(TEXT("_bundle_3p"))) return P;
		return FString();
	}();
	for (const TCHAR* Suffix : {TEXT("base_standard"), TEXT("parts_standard")})
	{
		const FString Mesh = Character->Asset + TEXT("/face/_base/") + R.Character + TEXT("_face_") + Suffix + TEXT("_mesh");
		if (Cat.Resources.Contains(Mesh)) Meshes.Add(Mesh);
	}
	int32 Parts = 0;
	for (const FString& Mesh : Meshes)
	{
		const bool Face = Mesh.Contains(TEXT("/face/"));
		// Hair cards require their dedicated shader; solid white stand-ins are not acceptable.
		if (Mesh.Contains(TEXT("eyelash")) || Mesh.Contains(TEXT("eyebrow")) || Mesh.Contains(TEXT("velcro"))) continue;
		FString Variation = Mesh; Variation.RemoveFromEnd(TEXT("_mesh"));
		if (Face) Variation = Variation.Replace(TEXT("/face/_base/"), TEXT("/face/face_001/")).Replace(TEXT("_face_base_"), TEXT("_face_001_base_"));
		else Variation = Variation.Replace(TEXT("set_001"), *(TEXT("set_") + R.Outfit));
		if (!Cat.Ebx.Contains(Variation)) Variation.Reset();
		TArray<FMatrix44f> Skin; int32 RigBones = 0;
		if (!SkinFor(C, Cat, Mesh, Pose, Skin, RigBones, Weapon, FootAnchor, Out.Error)) { Out.Sections.Reset(); return Out; }
		TArray<FCore::FSection> Sections;
		if (!C.ReadMesh(Mesh, Sections, Face ? FaceBundle : Outfit->Bundle, Variation, &Skin, RigBones)) { Out.Error = C.Error; Out.Sections.Reset(); return Out; }
		if (Mesh.EndsWith(TEXT("_patch_mesh")))
		{
			const FString Badge = FString(TEXT("common/characters/_shared/patches/faction/t_patch_faction_")) + (R.Faction == TEXT("pax") ? TEXT("pax_01") : TEXT("alliance"));
			const int32 Texture = C.TextureIdByName(Badge + TEXT("_cs"));
			if (Texture < 0) { Out.Error = TEXT("The selected faction badge is unavailable."); Out.Sections.Reset(); return Out; }
			for (FCore::FSection& S : Sections)
			{
				S.Textures.RemoveAll([](const FCore::FBinding& B){ return B.Slot == 0; });
				S.Textures.Add({0, Texture, Badge + TEXT("_cs")}); S.bAlphaTest = true; S.bAlphaFromAlbedo = true;
			}
		}
		Out.Sections.Append(MoveTemp(Sections)); ++Parts;
	}
	TArray<FCore::FSection> Gun;
	FBox3f BodyBounds(ForceInit); for (const auto& Section : Out.Sections) for (const FVector3f& P : Section.Pos) BodyBounds += P;
	if (!ItemMesh(C, Cat, R.Item, R.Attachments, Gun, Out.Error)) { Out.Sections.Reset(); return Out; }
	Transform(Gun, Weapon); Out.Sections.Append(MoveTemp(Gun));
	if (BodyBounds.IsValid)
	{
		FMatrix44f Anchor = FMatrix44f::Identity;
		Anchor.SetOrigin(FVector3f(-FootAnchor.X, -BodyBounds.Min.Y, -FootAnchor.Z));
		Transform(Out.Sections, Anchor);
	}
	Out.Detail = FString::Printf(TEXT("Posed soldier, %d character parts and configured weapon"), Parts);
	return Out;
}
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBF6LoadoutInstalledTest, "BF6.HighPoly.Loadout.InstalledAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBF6LoadoutInstalledTest::RunTest(const FString&)
{
	BF6HP::FCore Core;
	if (!Core.Open(BF6Ext::GameInstallDir(), BF6HP::CoreDllPath())) { AddError(Core.Error); return false; }
	FString Error;
	const auto Cat = BF6HP::Loadout::ReadCatalogue(Core, Error);
	if (!Cat) { AddError(Error); return false; }
	AddInfo(FString::Printf(TEXT("Catalogue: %d characters, %d outfits, %d items, %d vehicles"), Cat->Characters.Num(), Cat->Outfits.Num(), Cat->Items.Num(), Cat->Vehicles.Num()));
	for (const TCHAR* Type : {TEXT("LootSpawner"), TEXT("PlayerSpawner"), TEXT("VEH_Flyer60"), TEXT("VEH_Abrams"), TEXT("VEH_UH60")})
	{
		BF6HP::Loadout::FRequest Request; Request.Type = Type;
		const double Start = FPlatformTime::Seconds();
		const auto Result = BF6HP::Loadout::Decode(Core, *Cat, Request);
		if (!Result.Error.IsEmpty()) AddError(FString(Type) + TEXT(": ") + Result.Error);
		TestTrue(FString(Type) + TEXT(" has geometry"), !Result.Sections.IsEmpty());
		FBox3f Bounds(ForceInit); int64 Triangles = 0;
		for (const auto& Section : Result.Sections)
		{
			Triangles += Section.Idx.Num() / 3;
			for (const FVector3f& P : Section.Pos) { if (P.ContainsNaN()) { AddError(TEXT("Non-finite preview vertex")); return false; } Bounds += P; }
		}
		AddInfo(FString::Printf(TEXT("%s: %.2fs, %lld triangles, size %s, %s"), Type, FPlatformTime::Seconds() - Start, Triangles, *Bounds.GetSize().ToString(), *Result.Detail));
		TestTrue(FString(Type)+TEXT(" has bounded geometry"),Bounds.IsValid && Bounds.GetSize().GetMax()<100.f);
		if(FString(Type)==TEXT("PlayerSpawner"))
			TestTrue(TEXT("Soldier preserves iris/sclera bindings"),Result.Sections.ContainsByPredicate([](const BF6HP::FCore::FSection& S){ return S.Textures.ContainsByPredicate([](const BF6HP::FCore::FBinding& B){return B.Slot==10;});}));
	}
	BF6HP::Loadout::FRequest Configured; Configured.Type=TEXT("LootSpawner");
	BF6HP::Loadout::ReadWeaponAttachments(Core,*Cat,Configured.Item);
	const auto* Choices=Cat->Attachments.Find(Configured.Item);
	TestTrue(TEXT("Default weapon has mapped public attachments"),Choices && !Choices->IsEmpty());
	if(Choices && !Choices->IsEmpty())
	{
		Configured.Attachments.Add((*Choices)[0].Group,(*Choices)[0].Id);
		const auto Result=BF6HP::Loadout::Decode(Core,*Cat,Configured);
		TestTrue(TEXT("Configured weapon decodes"),Result.Error.IsEmpty() && !Result.Sections.IsEmpty());
		if(!Result.Error.IsEmpty()) AddError(Result.Error);
	}
	return true;
}
#endif
