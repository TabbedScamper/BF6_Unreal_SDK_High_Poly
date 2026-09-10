#include "BF6HighPolyLoadout.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Images/SImage.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"
#include "Containers/Ticker.h"

namespace BF6HP::Loadout
{
namespace
{
	using FRow = TSharedPtr<FChoice>;
	class SChoiceImage : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SChoiceImage) {} SLATE_END_ARGS()
		void Construct(const FArguments&, TSharedPtr<const FChoiceIcon, ESPMode::ThreadSafe> Icon)
		{
			if (Icon && Icon->Width > 0 && Icon->Height > 0 && Icon->Pixels.Num() == Icon->Width * Icon->Height)
			{
				Texture.Reset(UTexture2D::CreateTransient(Icon->Width, Icon->Height, PF_B8G8R8A8));
				if (Texture.IsValid())
				{
					Texture->SRGB = true;
					void* Data = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
					FMemory::Memcpy(Data, Icon->Pixels.GetData(), Icon->Pixels.Num() * sizeof(FColor));
					Texture->GetPlatformData()->Mips[0].BulkData.Unlock(); Texture->UpdateResource();
					Brush.SetResourceObject(Texture.Get()); Brush.ImageSize = FVector2D(Icon->Width, Icon->Height);
				}
			}
			ChildSlot[SNew(SBox).WidthOverride(100).HeightOverride(60).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[SNew(SImage).Image(Texture.IsValid() ? &Brush : nullptr)]];
		}
	private:
		TStrongObjectPtr<UTexture2D> Texture;
		FSlateBrush Brush;
	};
	class SChoicePicker : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SChoicePicker) {} SLATE_END_ARGS()
		void Construct(const FArguments&, TArray<FChoice> Choices, TFunction<void(const FChoice&)> OnPick)
		{
			Pick = MoveTemp(OnPick);
			for (const FChoice& C : Choices) All.Add(MakeShared<FChoice>(C));
			Rows = All;
			ChildSlot[SNew(SBox).WidthOverride(410).HeightOverride(360)
			[SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[SNew(SSearchBox).HintText(FText::FromString(TEXT("Find an item...")))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					Rows.Reset(); for (const FRow& R : All)
						if (Text.IsEmpty() || R->Label.Contains(Text.ToString()) || R->Id.Contains(Text.ToString())) Rows.Add(R);
					List->RequestListRefresh();
				})]
			+ SVerticalBox::Slot().FillHeight(1)
			[SAssignNew(List, SListView<FRow>).ListItemsSource(&Rows).SelectionMode(ESelectionMode::Single)
				.OnGenerateRow_Lambda([](FRow R, const TSharedRef<STableViewBase>& Owner)
				{
					return SNew(STableRow<FRow>, Owner).Padding(5).ToolTipText(FText::FromString(R->Description.IsEmpty() ? R->Id : R->Description))
					[SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()[SNew(SChoiceImage, R->Icon)]
					+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8, 0)
					[SNew(STextBlock).Text(FText::FromString(R->Label)).AutoWrapText(true)]];
				})
				.OnSelectionChanged_Lambda([this](FRow R, ESelectInfo::Type How)
				{
					if (R && How != ESelectInfo::Direct) { const FChoice Chosen = *R; Pick(Chosen); FSlateApplication::Get().DismissAllMenus(); }
				})]]];
		}
	private:
		TArray<FRow> All, Rows;
		TSharedPtr<SListView<FRow>> List;
		TFunction<void(const FChoice&)> Pick;
	};
	class SLoadoutPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SLoadoutPanel) {} SLATE_END_ARGS()
		void Construct(const FArguments&)
		{
			RequestCatalogue();
			ChildSlot[SNew(SBorder).Padding(16).BorderBackgroundColor(FLinearColor(.025f, .045f, .045f))
			[SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[SNew(STextBlock).Text(FText::FromString(TEXT("Loadout"))).Font(FCoreStyle::GetDefaultFontStyle("Bold", 20))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[SNew(STextBlock).Text_Lambda([this]{ return FText::FromString(Object.Label.IsEmpty() ? TEXT("Select a spawner in the scene.") : Object.Label + TEXT("  /  ") + Object.Type); }).AutoWrapText(true)]
			+ SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(Fields, SVerticalBox)]]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)
			[SNew(STextBlock).Text_Lambda([this]{ return FText::FromString(SelectionStatus(Object.Id)); }).AutoWrapText(true)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
			[SNew(STextBlock).Text_Lambda([]{ return FText::FromString(Status()); }).AutoWrapText(true)]]];
			Refresh();
		}
		virtual void Tick(const FGeometry& Geometry, const double Time, const float Delta) override
		{
			SCompoundWidget::Tick(Geometry, Time, Delta);
			if (Time - LastRefresh > .25) { LastRefresh = Time; Refresh(); }
		}
	private:
		BF6Ext::FObjectPreview Object;
		TSharedPtr<SVerticalBox> Fields;
		FString Revision;
		FString ActionStatus;
		double LastRefresh = 0;
		void AddField(const FString& Key, const FString& Label, const FString& Value)
		{
			TArray<FChoice> Options;
			if (Key == TEXT("portalitem"))
			{
				for (const FString& Item : BF6Ext::LootItems()) Options.Add({Item, Item.Replace(TEXT("_"), TEXT(" "))});
			}
			else Options = Choices(Key, Object);
			TMap<FString, FString> PortalMatches;
			if (Object.Type == TEXT("LootSpawner") && (Key == TEXT("item") || Key == TEXT("gadget") || Key == TEXT("throwable")))
			{
				const auto Normalize = [](const FString& Text) { FString S; for (TCHAR C : Text) if (FChar::IsAlnum(C)) S.AppendChar(FChar::ToLower(C)); return S; };
				const TArray<FString> PortalItems = BF6Ext::LootItems();
				for (const FChoice& C : Options)
				{
					FString Category, Token; C.Id.Split(TEXT("/"), &Category, &Token);
					TArray<FString> Matches;
					for (const FString& P : PortalItems)
					{
						FString Kind, Member, Prefix, Name; P.Split(TEXT("."), &Kind, &Member); Member.Split(TEXT("_"), &Prefix, &Name);
						if (Normalize(Name) == Normalize(Token) || Normalize(Name) == Normalize(C.Label)) Matches.Add(P);
					}
					if (Matches.Num() == 1) PortalMatches.Add(C.Id, Matches[0]);
				}
			}
			FString Display = Value.IsEmpty() ? TEXT("Choose...") : Value;
			if (const FChoice* C = Options.FindByPredicate([&](const FChoice& I){ return I.Id == Value; })) Display = C->Label;
			const FString ActorId = Object.Id;
			Fields->AddSlot().AutoHeight().Padding(0, 5)
			[SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)[SNew(STextBlock).Text(FText::FromString(Label))]
			+ SVerticalBox::Slot().AutoHeight()
			[SNew(SComboButton).IsEnabled(!Options.IsEmpty()).ToolTipText(FText::FromString(TEXT("Choose the item shown at this spawner.")))
				.ButtonContent()[SNew(STextBlock).Text(FText::FromString(Display))]
				.OnGetMenuContent_Lambda([Options, PortalMatches, Key, ActorId]() -> TSharedRef<SWidget>
				{
					return SNew(SChoicePicker, Options, TFunction<void(const FChoice&)>([Key, ActorId, PortalMatches](const FChoice& C)
					{
						const FString StoreKey = Key == TEXT("gadget") || Key == TEXT("throwable") ? TEXT("item") : Key;
						TMap<FString, FString> Changes{{StoreKey, C.Id}};
						if (Key == TEXT("character")) Changes.Add(TEXT("outfit"), TEXT("001"));
						if (Key == TEXT("vehicle")) Changes.Add(TEXT("skin"), FString());
						if (StoreKey == TEXT("item"))
							for (const TCHAR* Slot : {TEXT("scp"),TEXT("sca"),TEXT("brl"),TEXT("mzl"),TEXT("mag"),TEXT("amo"),TEXT("erg"),TEXT("btm"),TEXT("top"),TEXT("lft"),TEXT("rgt")}) Changes.Add(FString(TEXT("attachment_"))+Slot,FString());
						if (Key == TEXT("item") || Key == TEXT("gadget") || Key == TEXT("throwable")) Changes.Add(TEXT("portalitem"), PortalMatches.FindRef(C.Id));
						if (!BF6Ext::SetObjectPreviews(ActorId, Changes)) BF6Ext::Notify(TEXT("The selected spawner is no longer editable."));
					}));
				})]];
		}
		void Refresh()
		{
			const auto Current = BF6Ext::SelectedObjectPreview();
			const FRequest R = RequestFor(Current.Type, Current.Values);
			const FString NewRevision = Current.Id + R.Key() + Current.Values.FindRef(TEXT("portalitem")) + Status();
			if (NewRevision == Revision) return;
			Revision = NewRevision; Object = Current; Fields->ClearChildren();
			if (!Handles(Object.Type)) return;
			if (Object.Type.StartsWith(TEXT("VEH_")) || Object.Type == TEXT("VehicleSpawner"))
			{
				if (Object.Type == TEXT("VehicleSpawner")) AddField(TEXT("vehicle"), TEXT("Vehicle on this map"), R.Vehicle);
				AddField(TEXT("skin"), TEXT("Vehicle skin"), R.Skin);
			}
			else
			{
				if (Object.Type != TEXT("LootSpawner"))
				{
					AddField(TEXT("character"), TEXT("Character"), R.Character);
					AddField(TEXT("faction"), TEXT("Side"), R.Faction);
					AddField(TEXT("outfit"), TEXT("Outfit"), R.Outfit);
					AddField(TEXT("role"), TEXT("Pose"), R.Role);
				}
				AddField(TEXT("item"), TEXT("Weapon"), R.Item);
				if (Object.Type == TEXT("LootSpawner"))
					Fields->AddSlot().AutoHeight().Padding(0, 8)[WeaponPreviewWidget(Object)];
				const TMap<FString,FString> AttachmentLabels{{TEXT("scp"),TEXT("Optic")},{TEXT("sca"),TEXT("Canted optic")},{TEXT("brl"),TEXT("Barrel")},{TEXT("mzl"),TEXT("Muzzle")},{TEXT("mag"),TEXT("Magazine")},{TEXT("amo"),TEXT("Ammunition")},{TEXT("erg"),TEXT("Ergonomics")},{TEXT("btm"),TEXT("Underbarrel")},{TEXT("top"),TEXT("Top rail")},{TEXT("lft"),TEXT("Left rail")},{TEXT("rgt"),TEXT("Right rail")}};
				for (const TCHAR* Slot : {TEXT("scp"),TEXT("sca"),TEXT("brl"),TEXT("mzl"),TEXT("mag"),TEXT("amo"),TEXT("erg"),TEXT("btm"),TEXT("top"),TEXT("lft"),TEXT("rgt")})
				{
					const FString Field = FString(TEXT("attachment_"))+Slot;
					if (Choices(Field,Object).Num()>1 || R.Attachments.Contains(Slot)) AddField(Field,AttachmentLabels[Slot],R.Attachments.FindRef(Slot));
				}
				if (Object.Type == TEXT("LootSpawner"))
				{
					AddField(TEXT("gadget"), TEXT("Gadget"), R.Item);
					AddField(TEXT("throwable"), TEXT("Throwable"), R.Item);
					FString PortalItem = Object.Values.FindRef(TEXT("portalitem"));
					if (PortalItem.IsEmpty() && R.Item == TEXT("carbine/m4a1")) PortalItem = TEXT("Weapons.Carbine_M4A1");
					AddField(TEXT("portalitem"), TEXT("Portal gameplay item"), PortalItem);
					Fields->AddSlot().AutoHeight().Padding(0, 10)
					[SNew(STextBlock).Text(FText::FromString(TEXT("Connect this spawner using its object ID. The block recipe spawns once at game start; change the event or add conditions in Blocks."))).AutoWrapText(true)];
					const FString ActorId = Object.Id;
					for (const FString& Action : {FString(TEXT("blocks")), FString(TEXT("script")), FString(TEXT("card"))})
					{
						const FString Label = Action == TEXT("blocks") ? TEXT("Create / update spawn rule") : Action == TEXT("script") ? TEXT("Open TypeScript helper") : TEXT("Open weapon card design");
						const FString Hint = Action == TEXT("blocks") ? TEXT("Create the native SpawnLoot rule, or update its item while keeping your event and conditions.") : Action == TEXT("script") ? TEXT("Open the item, spawner ID, card widget names and callable spawn helper. Use them in your chosen event handler.") : TEXT("Edit a reusable card with an optional action button. Connect its exported blocks or script to proximity, buy stations, Gunmaster or your own logic.");
						Fields->AddSlot().AutoHeight().Padding(0, 4)
						[SNew(SButton).Text(FText::FromString(Label)).ToolTipText(FText::FromString(Hint))
							.OnClicked_Lambda([this, ActorId, Action]
							{
								const bool Opened = BF6Ext::OpenLootBinding(ActorId, Action, ActionStatus);
								if (Opened)
									FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
									{ BF6Ext::CloseAddonWindow(TEXT("HighPoly.Loadout")); return false; }));
								return FReply::Handled();
							})];
					}
					Fields->AddSlot().AutoHeight().Padding(0, 8)
					[SNew(STextBlock).Text_Lambda([this]{ return FText::FromString(ActionStatus); }).AutoWrapText(true)];
				}
			}
		}
	};
}
TSharedRef<SWidget> ChoiceMenu(const TArray<FChoice>& Options, TFunction<void(const FChoice&)> OnPick)
{
	return SNew(SChoicePicker, Options, MoveTemp(OnPick));
}
void OpenPanel()
{
	BF6Ext::ShowAddonWindow(TEXT("HighPoly.Loadout"), TEXT("Loadout"), SNew(SLoadoutPanel), FVector2D(620, 900));
}
}
