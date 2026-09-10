#include "BF6HighPolyLoadout.h"
#include "BF6HighPolyIconReader.h"
#include "BF6HighPolyShared.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace BF6HP::Loadout
{
namespace
{
    const FName ProviderId(TEXT("HighPoly.EquipmentCards"));
    struct FPending { FString Json; BF6Ext::FEquipmentPreviewReply Reply; };
    TArray<FPending> Pending;
    TFuture<FString> CardWorker;
    BF6Ext::FEquipmentPreviewReply ActiveReply;
    FTSTicker::FDelegateHandle CardTicker;
    IImageWrapperModule* Images=nullptr;
    bool OwnCardBusy=false;

    FString Json(const TSharedRef<FJsonObject>& O)
    {
        FString Text;
        auto W=TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(O,W);
        return Text;
    }
    FString Failure(const FString& Error)
    {
        auto O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("error"),Error); return Json(O);
    }
    FString Fold(const FString& S)
    { FString R; for (TCHAR C:S) if (FChar::IsAlnum(C)) R.AppendChar(FChar::ToLower(C)); return R; }
    FString Slot(const bf6_attachment_catalogue_row& R)
    { FString S=UTF8_TO_TCHAR(R.slot); return S==TEXT("opt") ? TEXT("sca") : S; }
    FString Prefix(const FString& S)
    {
        static const TMap<FString,FString> Names{{TEXT("scp"),TEXT("Scope")},{TEXT("sca"),TEXT("Scope")},
            {TEXT("brl"),TEXT("Barrel")},{TEXT("mzl"),TEXT("Muzzle")},{TEXT("mag"),TEXT("Magazine")},
            {TEXT("amo"),TEXT("Ammo")},{TEXT("erg"),TEXT("Ergonomic")},{TEXT("btm"),TEXT("Bottom")},
            {TEXT("top"),TEXT("Top")},{TEXT("lft"),TEXT("Left")},{TEXT("rgt"),TEXT("Right")}};
        return Names.FindRef(S);
    }
    struct FLayer
    {
        FString Slot;
        FHardwareIconReader::FSprite Sprite;
        bf6_card_layout_entry Layout{};
        FVector2f Position;
        int32 Order=0;
    };
    bool EncodeCard(const TArray<FColor>& Pixels,int32 Width,int32 Height,FString& Out)
    {
        auto Image=Images->CreateImageWrapper(EImageFormat::PNG);
        if (!Image || !Image->SetRaw(Pixels.GetData(),int64(Pixels.Num())*4,Width,Height,ERGBFormat::BGRA,8)) return false;
        const TArray64<uint8>& Png=Image->GetCompressed();
        if (Png.IsEmpty() || Png.Num()>300000) return false;
        Out=FBase64::Encode(Png.GetData(),Png.Num()); return true;
    }
    FString DrawGadget(FCore& Core,FHardwareIconReader& Reader,const FString& Item,int32 Width,int32 Height)
    {
        const auto Read=Reader.Export<decltype(&bf6_gadget_ui_metadata_rows)>(TEXT("bf6_gadget_ui_metadata_rows"));
        const int32 N=Read ? Read(Core.Handle(),nullptr,0) : -1;
        if (N<1 || N>4096) return Failure(TEXT("The installed gadget catalogue is unavailable."));
        TArray<bf6_gadget_ui_metadata> Rows; Rows.SetNumZeroed(N);
        if (Read(Core.Handle(),Rows.GetData(),N)!=N) return Failure(TEXT("The gadget catalogue is incomplete."));
        FString Atlas; int32 Index=-1;
        for (const auto& R:Rows)
        {
            if (Fold(UTF8_TO_TCHAR(R.debug_name))!=Fold(Item) && Fold(UTF8_TO_TCHAR(R.name))!=Fold(Item)) continue;
            const FString Source=UTF8_TO_TCHAR(R.icon_atlas);
            if (!Atlas.IsEmpty() && (Atlas!=Source || Index!=R.icon_index)) return Failure(TEXT("Gadget artwork is ambiguous: ")+Item);
            Atlas=Source; Index=R.icon_index;
        }
        Atlas.RemoveFromEnd(TEXT(".ebx"));
        FHardwareIconReader::FSprite Sprite;
        if (!Reader.Read(Atlas,Index,Sprite)) return Failure(TEXT("No installed icon binding for gadget ")+Item);
        const float Scale=FMath::Min(Width/Sprite.Data.size[0],Height/Sprite.Data.size[1]);
        const FVector2f Size(Sprite.Data.size[0]*Scale,Sprite.Data.size[1]*Scale),Origin((Width-Size.X)*.5f,(Height-Size.Y)*.5f);
        const float Edge=FHardwareIconReader::Width(Sprite,Size.Y);
        TArray<FColor> Pixels; Pixels.Init(FColor(0,0,0,0),Width*Height);
        for (int32 Y=0;Y<Height;++Y) for (int32 X=0;X<Width;++X)
        {
            const float U=(X+.5f-Origin.X)/Size.X,V=(Y+.5f-Origin.Y)/Size.Y;
            if (U<0 || V<0 || U>1 || V>1) continue;
            const auto RG=FHardwareIconReader::Sample(Sprite,U,V);
            HardwareIcon::State State;
            State.Layer(RG.X,RG.Y,Edge,{.15f,.18f,.18f,1},{1,1,1,1},1);
            Pixels[Y*Width+X]=FHardwareIconReader::Pixel(State.Finish());
        }
        FString Png;
        if (!EncodeCard(Pixels,Width,Height,Png)) return Failure(TEXT("Gadget image encoding failed."));
        auto Result=MakeShared<FJsonObject>(); Result->SetStringField(TEXT("png"),Png);
        Result->SetStringField(TEXT("item"),Item);
        Result->SetStringField(TEXT("detail"),TEXT("Installed gadget artwork. Styling and framing still require comparison with Portal."));
        return Json(Result);
    }
    FString DrawCard(const FString& Request)
    {
        TSharedPtr<FJsonObject> Input;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Request),Input) || !Input) return Failure(TEXT("Invalid equipment request."));
        FString Kind,Item; Input->TryGetStringField(TEXT("kind"),Kind); Input->TryGetStringField(TEXT("item"),Item);
        if (Kind!=TEXT("WeaponImage") && Kind!=TEXT("GadgetImage")) return Failure(TEXT("Unsupported equipment widget."));
        FString Category,Weapon;
        if (Item.IsEmpty() || Item.Len()>128 || (Kind==TEXT("WeaponImage") && !Item.Split(TEXT("_"),&Category,&Weapon))) return Failure(TEXT("Choose a Portal equipment item."));
        Weapon=Weapon.ToLower();
        double W=512,H=256; Input->TryGetNumberField(TEXT("width"),W); Input->TryGetNumberField(TEXT("height"),H);
        if (!FMath::IsFinite(W) || !FMath::IsFinite(H)) return Failure(TEXT("Invalid image dimensions."));
        const int32 Width=FMath::Clamp(FMath::RoundToInt(W),1,1024),Height=FMath::Clamp(FMath::RoundToInt(H),1,512);
        TArray<FString> Attachments;
        const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
        if (Input->TryGetArrayField(TEXT("attachments"),Values))
        {
            if (Values->Num()>16) return Failure(TEXT("Too many attachment selections."));
            for (const auto& V:*Values) { FString S; if (!V->TryGetString(S) || S.Len()>128) return Failure(TEXT("Invalid attachment.")); Attachments.Add(S); }
        }
        FScopeLock Lock(&Shared::CoreMutex());
        if (Shared::CoreShuttingDown()) return Failure(TEXT("The game reader is shutting down."));
        FCore& Core=Shared::Core(); FHardwareIconReader Reader(Core);
        const auto Mount=Reader.Export<decltype(&bf6_mount_frontend)>(TEXT("bf6_mount_frontend"));
        const auto Receiver=Reader.Export<decltype(&bf6_weapon_card_receiver)>(TEXT("bf6_weapon_card_receiver"));
        const auto Layout=Reader.Export<decltype(&bf6_card_layout)>(TEXT("bf6_card_layout"));
        const auto Catalogue=Reader.Export<decltype(&bf6_weapon_attachment_catalogue)>(TEXT("bf6_weapon_attachment_catalogue"));
        const auto Factory=Reader.Export<decltype(&bf6_weapon_factory_fits)>(TEXT("bf6_weapon_factory_fits"));
        const auto List=Reader.Export<decltype(&bf6_list_ebx)>(TEXT("bf6_list_ebx"));
        if (!Mount || !Receiver || !Layout || !Catalogue || !Factory || !List) return Failure(TEXT("Update the SDK game reader to enable weapon cards."));
        char Why[512]{};
        if (!Mount(Core.Handle(),Why,512)) return Failure(UTF8_TO_TCHAR(Why));
        if (Kind==TEXT("GadgetImage")) return DrawGadget(Core,Reader,Item,Width,Height);
        bf6_card_receiver Base{};
        if (Receiver(Core.Handle(),TCHAR_TO_UTF8(*Weapon),&Base)!=1) return Failure(TEXT("No unambiguous receiver artwork for ")+Item+TEXT(" in this game install."));
        const int32 Count=Catalogue(Core.Handle(),TCHAR_TO_UTF8(*Weapon),nullptr,0);
        if (Count<0 || Count>8192) return Failure(TEXT("Cannot read this weapon's attachment catalogue."));
        TArray<bf6_attachment_catalogue_row> Rows; Rows.SetNumZeroed(Count);
        if (Catalogue(Core.Handle(),TCHAR_TO_UTF8(*Weapon),Rows.GetData(),Count)!=Count) return Failure(TEXT("Attachment catalogue changed during the request."));
        // Preserve every factory token. Several tokens can name the same art;
        // resolve that equivalence only after collecting the request's matches.
        const auto UniqueArtwork = [&](const TArray<int32>& Matches) -> int32
        {
            if (Matches.IsEmpty()) return INDEX_NONE;
            const auto& A=Rows[Matches[0]];
            for (int32 I : Matches)
            {
                const auto& B=Rows[I];
                if (Slot(A)!=Slot(B) || FCStringAnsi::Strcmp(A.ad_stem,B.ad_stem)!=0 ||
                    FCStringAnsi::Strcmp(A.icon_atlas,B.icon_atlas)!=0 || A.icon_index!=B.icon_index ||
                    FCStringAnsi::Strcmp(A.layered_atlas,B.layered_atlas)!=0 || A.layered_index!=B.layered_index)
                    return INDEX_NONE;
            }
            return Matches[0];
        };
        TMap<FString,int32> Selected;
        TSet<FString> CustomSlots;
        for (const FString& A:Attachments)
        {
            FString PublicSlot,Name;
            if (!A.Split(TEXT("_"),&PublicSlot,&Name)) return Failure(TEXT("Invalid attachment: ")+A);
            TArray<int32> Matches;
            for (int32 I=0;I<Rows.Num();++I)
                if (Prefix(Slot(Rows[I]))==PublicSlot && Fold(UTF8_TO_TCHAR(Rows[I].name))==Fold(Name)) Matches.Add(I);
            const int32 Match=UniqueArtwork(Matches);
            if (Match==INDEX_NONE) return Failure(TEXT("No unique installed artwork binding for ")+A+TEXT(" on ")+Item+TEXT(". No factory artwork was substituted."));
            const FString S=Slot(Rows[Match]);
            if (CustomSlots.Contains(S)) return Failure(TEXT("Two attachments occupy the same slot: ")+S);
            CustomSlots.Add(S); Selected.Add(S,Match);
        }
        const FString EquipmentLeaf=TEXT("equipment_")+Weapon;
        const int32 N=List(Core.Handle(),TCHAR_TO_UTF8(*EquipmentLeaf),nullptr,0);
        if (N<1 || N>1024) return Failure(TEXT("Factory equipment record is unavailable."));
        TArray<bf6_asset> Assets; Assets.SetNumZeroed(N); List(Core.Handle(),TCHAR_TO_UTF8(*EquipmentLeaf),Assets.GetData(),N);
        FString Equipment;
        for (const auto& A:Assets) if (A.name)
        {
            FString Path=UTF8_TO_TCHAR(A.name); Path.RemoveFromEnd(TEXT(".ebx"));
            if (FPaths::GetCleanFilename(Path)!=EquipmentLeaf) continue;
            if (!Equipment.IsEmpty() && Equipment!=Path) return Failure(TEXT("Factory equipment record is ambiguous."));
            Equipment=Path;
        }
        bf6_weapon_fit Fits[32]{};
        const int32 FitCount=Factory(Core.Handle(),TCHAR_TO_UTF8(*Equipment),Fits,32);
        if (FitCount<1 || FitCount>32) return Failure(TEXT("Factory attachment membership could not be read."));
        for (int32 F=0;F<FitCount;++F)
        {
            const FString S=UTF8_TO_TCHAR(Fits[F].slot),Token=UTF8_TO_TCHAR(Fits[F].attachment);
            if (Selected.Contains(S)) continue;
            TArray<int32> Matches;
            for (int32 I=0;I<Rows.Num();++I) if (Slot(Rows[I])==S &&
                (Fold(Token)==UTF8_TO_TCHAR(Rows[I].name_key) || Fold(Weapon+S+Token)==UTF8_TO_TCHAR(Rows[I].ad_stem))) Matches.Add(I);
            const int32 Match=UniqueArtwork(Matches);
            if (Match==INDEX_NONE) return Failure(TEXT("Factory artwork binding is unresolved: ")+S+TEXT("/")+Token);
            Selected.Add(S,Match);
        }
        const int32 LC=Layout(Core.Handle(),Base.layout,nullptr,0);
        if (LC<1 || LC>8192) return Failure(TEXT("The weapon card layout is missing."));
        TArray<bf6_card_layout_entry> Layouts; Layouts.SetNumZeroed(LC);
        if (Layout(Core.Handle(),Base.layout,Layouts.GetData(),LC)!=LC) return Failure(TEXT("The weapon card layout is incomplete."));
        TArray<FLayer> Layers;
        FString Error;
        auto Add=[&](const FString& S,const char* Atlas,int32 Index)
        {
            FLayer L; L.Slot=S;
            if (!Reader.Read(UTF8_TO_TCHAR(Atlas),Index,L.Sprite)) { Error=TEXT("Missing card sprite for ")+S; return false; }
            TArray<int32> Matches;
            for (int32 I=0;I<LC;++I) if (Layouts[I].sprite_hash==L.Sprite.Data.name_hash) Matches.Add(I);
            if (Matches.Num()!=1 || Layouts[Matches[0]].kind<0)
            { Error=TEXT("Missing or ambiguous authored placement for ")+S; return false; }
            L.Order=Matches[0]; L.Layout=Layouts[L.Order];
            L.Position=FVector2f(L.Sprite.Data.placement[0]+L.Layout.offset[0],L.Sprite.Data.placement[1]+L.Layout.offset[1]);
            Layers.Add(MoveTemp(L)); return true;
        };
        if (!Add(TEXT("receiver"),Base.atlas,Base.index)) return Failure(Error);
        for (const auto& Pair:Selected)
        {
            const auto& R=Rows[Pair.Value];
            // Ammunition can author an empty image. Its gameplay selection is
            // preserved; an empty reference contributes no silhouette layer.
            if (!R.layered_atlas[0] || R.layered_index<0) continue;
            if (!Add(Pair.Key,R.layered_atlas,R.layered_index)) return Failure(Error);
        }
        const FLayer* Barrel=Layers.FindByPredicate([](const FLayer& L){return L.Layout.kind==2;});
        const FLayer* Scope=Layers.FindByPredicate([](const FLayer& L){return L.Layout.kind==4;});
        for (FLayer& L:Layers)
        {
            if (L.Layout.kind==1 && Barrel && (Barrel->Layout.flags&1))
                L.Position=FVector2f(L.Sprite.Data.placement[0]+Barrel->Layout.secondary_anchor[0],L.Sprite.Data.placement[1]+Barrel->Layout.secondary_anchor[1]);
            if (L.Layout.kind==3 && (L.Layout.flags&2))
            {
                if (!Scope || !(Scope->Layout.flags&1)) return Failure(TEXT("This canted sight needs an authored scope mounting anchor."));
                L.Position=FVector2f(L.Sprite.Data.placement[0]+L.Layout.offset[0]+Scope->Layout.secondary_anchor[0],
                    L.Sprite.Data.placement[1]+L.Layout.offset[1]+Scope->Layout.secondary_anchor[1]);
            }
        }
        Layers.Sort([](const FLayer& A,const FLayer& B){return A.Order<B.Order;});
        // Frame the assembled artwork as a whole. HIAO offsets can legitimately
        // place long barrels/muzzles outside the nominal 512x256 source canvas.
        // Preserve every relative attachment position and fractional coordinate.
        FVector2f Minimum(FLT_MAX,FLT_MAX),Maximum(-FLT_MAX,-FLT_MAX);
        for (const FLayer& L:Layers)
        {
            if (!FMath::IsFinite(L.Position.X) || !FMath::IsFinite(L.Position.Y)) return Failure(TEXT("Invalid authored card position."));
            Minimum.X=FMath::Min(Minimum.X,L.Position.X); Minimum.Y=FMath::Min(Minimum.Y,L.Position.Y);
            Maximum.X=FMath::Max(Maximum.X,L.Position.X+L.Sprite.Data.size[0]);
            Maximum.Y=FMath::Max(Maximum.Y,L.Position.Y+L.Sprite.Data.size[1]);
        }
        const FVector2f Extent=Maximum-Minimum;
        if (Extent.X<=0 || Extent.Y<=0) return Failure(TEXT("Invalid assembled card bounds."));
        const float Scale=FMath::Min(Width/Extent.X,Height/Extent.Y);
        const FVector2f Origin((Width-Extent.X*Scale)*.5f-Minimum.X*Scale,(Height-Extent.Y*Scale)*.5f-Minimum.Y*Scale);
        TArray<HardwareIcon::State> Pixels; Pixels.SetNum(Width*Height);
        for (const FLayer& L:Layers)
        {
            const FVector2f P=Origin+L.Position*Scale,Size(L.Sprite.Data.size[0]*Scale,L.Sprite.Data.size[1]*Scale);
            const float Edge=FHardwareIconReader::Width(L.Sprite,Size.Y);
            const int32 X0=FMath::Clamp(FMath::FloorToInt(P.X),0,Width),Y0=FMath::Clamp(FMath::FloorToInt(P.Y),0,Height);
            const int32 X1=FMath::Clamp(FMath::CeilToInt(P.X+Size.X),0,Width),Y1=FMath::Clamp(FMath::CeilToInt(P.Y+Size.Y),0,Height);
            for (int32 Y=Y0;Y<Y1;++Y) for (int32 X=X0;X<X1;++X)
            {
                const float U=(X+.5f-P.X)/Size.X,V=(Y+.5f-P.Y)/Size.Y;
                if (U<0 || V<0 || U>1 || V>1) continue;
                const auto RG=FHardwareIconReader::Sample(L.Sprite,U,V);
                Pixels[Y*Width+X].Layer(RG.X,RG.Y,Edge,{.15f,.18f,.18f,1},{1,1,1,1},1);
            }
        }
        TArray<FColor> Rgba; Rgba.Reserve(Pixels.Num());
        for (const auto& P:Pixels) Rgba.Add(FHardwareIconReader::Pixel(P.Finish()));
        FString Png;
        if (!EncodeCard(Rgba,Width,Height,Png)) return Failure(TEXT("Card image encoding failed or exceeded the browser transfer limit."));
        auto Result=MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("item"),Item);
        Result->SetStringField(TEXT("png"),Png);
        Result->SetNumberField(TEXT("layers"),Layers.Num());
        TArray<TSharedPtr<FJsonValue>> Choices;
        TSet<FString> Offered;
        for (const auto& R:Rows)
        {
            if (Prefix(Slot(R)).IsEmpty()) continue;
            const FString Name=Fold(UTF8_TO_TCHAR(R.name));
            const FString Key=Prefix(Slot(R))+TEXT("_")+Name;
            if (Offered.Contains(Key)) continue;
            TArray<int32> Matches;
            for (int32 I=0;I<Rows.Num();++I)
                if (Prefix(Slot(Rows[I]))==Prefix(Slot(R)) && Fold(UTF8_TO_TCHAR(Rows[I].name))==Name) Matches.Add(I);
            if (UniqueArtwork(Matches)==INDEX_NONE) continue;
            Offered.Add(Key);
            auto Choice=MakeShared<FJsonObject>();
            Choice->SetStringField(TEXT("slot"),Slot(R)); Choice->SetStringField(TEXT("prefix"),Prefix(Slot(R)));
            Choice->SetStringField(TEXT("label"),UTF8_TO_TCHAR(R.name));
            Choices.Add(MakeShared<FJsonValueObject>(Choice));
        }
        Result->SetArrayField(TEXT("attachmentChoices"),Choices);
        Result->SetStringField(TEXT("detail"),TEXT("Installed game artwork and authored attachment positions. Styling and framing still require comparison with Portal."));
        return Json(Result);
    }
    bool CardsTick(float)
    {
        if (CardWorker.IsValid())
        {
            if (!CardWorker.IsReady()) return true;
            FString Result=CardWorker.Get(); CardWorker=TFuture<FString>();
            if (OwnCardBusy) { Shared::SetCoreBusy(false); OwnCardBusy=false; }
            auto Reply=MoveTemp(ActiveReply); if (Reply) Reply(MoveTemp(Result));
        }
        if (Pending.IsEmpty() || Shared::IsBuilding() || Shared::CoreBusy() || Shared::GeometryHasPriority()) return true;
        FPending Next=MoveTemp(Pending[0]); Pending.RemoveAt(0);
        FString Error;
        if (!Shared::EnsureCoreOpen(Error)) { Next.Reply(Failure(Error)); return true; }
        ActiveReply=MoveTemp(Next.Reply);
        Shared::SetCoreBusy(true); OwnCardBusy=true;
        CardWorker=Async(EAsyncExecution::ThreadPool,[Request=MoveTemp(Next.Json)] { return DrawCard(Request); });
        return true;
    }
}
void StartEquipmentCards()
{
    Images=&FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    BF6Ext::RegisterEquipmentPreviewProvider(ProviderId,[](const FString& Request,BF6Ext::FEquipmentPreviewReply Reply)
    {
        if (Pending.Num()>=32 || Request.Len()>16384) { Reply(Failure(TEXT("Equipment preview queue is full. Retry after the current previews finish."))); return; }
        Pending.Add({Request,MoveTemp(Reply)});
    });
    CardTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&CardsTick));
}
void StopEquipmentCards()
{
    BF6Ext::UnregisterEquipmentPreviewProvider(ProviderId);
    if (CardTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(CardTicker); CardTicker.Reset(); }
    if (CardWorker.IsValid()) { CardWorker.Wait(); CardWorker=TFuture<FString>(); }
    ActiveReply=nullptr; Pending.Reset();
    if (OwnCardBusy) { Shared::SetCoreBusy(false); OwnCardBusy=false; }
    Images=nullptr;
}
static FAutoConsoleCommand ExportCardCommand(TEXT("BF6.HighPoly.Card.Export"),
    TEXT("Export a local equipment preview: <request.json> <output.png>. Reads the same request as the UI designer."),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
    {
        FString Request;
        if (Args.Num()!=2 || !Args[1].EndsWith(TEXT(".png")) || !FFileHelper::LoadFileToString(Request,*Args[0]))
        { UE_LOG(LogTemp,Warning,TEXT("BF6.HighPoly.Card.Export requires a readable request.json and output.png.")); return; }
        BF6Ext::RequestEquipmentPreview(Request,[Path=Args[1]](FString Reply)
        {
            TSharedPtr<FJsonObject> O;
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Reply),O);
            FString Png,Error; TArray<uint8> Bytes;
            if (!O || !O->TryGetStringField(TEXT("png"),Png) || !FBase64::Decode(Png,Bytes) || !FFileHelper::SaveArrayToFile(Bytes,*Path))
            {
                if (O) O->TryGetStringField(TEXT("error"),Error);
                UE_LOG(LogTemp,Warning,TEXT("Equipment card export failed: %s"),*Error); return;
            }
            UE_LOG(LogTemp,Display,TEXT("Equipment card exported: %s"),*Path);
        });
    }));
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBF6HardwareIconCoverageTest,"BF6.HighPoly.EquipmentCards.Coverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBF6HardwareIconCoverageTest::RunTest(const FString&)
{
    using namespace HardwareIcon;
    TestEqual(TEXT("Empty field has no fill"),FillCoverage(0,.1f),0.f);
    TestEqual(TEXT("Half-distance has half coverage"),FillCoverage(.5f,.125f),.5f);
    TestEqual(TEXT("Zero footprint is finite and empty"),FillCoverage(.5f,0),0.f);
    TestEqual(TEXT("Interior is opaque"),FillCoverage(1,.1f),1.f);
    State S;
    S.Layer(1,1,.125f,{1,0,0,1},{1,0,0,1},1);
    TestEqual(TEXT("First part contributes"),S.Finish().A,1.f);
    S.Layer(0,1,.125f,{0,0,0,0},{0,1,0,1},1,false);
    TestEqual(TEXT("Transparent foreground erases covered rear lines"),S.Finish().A,0.f);
    S.Layer(0,1,.125f,{0,0,1,.5f},{1,1,1,1},1,false);
    TestEqual(TEXT("Fill alpha retained"),S.Finish().A,.5f);
    const FColor Pixel=FHardwareIconReader::Pixel(S.Finish());
    TestEqual(TEXT("Straight-alpha blue is not darkened twice"),Pixel.B,uint8(255));
    TestEqual(TEXT("Straight-alpha opacity"),Pixel.A,uint8(128));
    return true;
}
#endif
}
