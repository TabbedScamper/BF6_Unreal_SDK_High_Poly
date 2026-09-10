#include "BF6HighPolyLoadout.h"
#include "Camera/CameraTypes.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "PreviewScene.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Text/STextBlock.h"

namespace BF6HP::Loadout
{
namespace
{
	class SWeaponPreview : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SWeaponPreview) {} SLATE_END_ARGS()
		void Construct(const FArguments&, const BF6Ext::FObjectPreview& InObject)
		{
			Object = InObject;
			Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
				.SetCreateDefaultLighting(true).SetLightRotation(FRotator(-35, -35, 0))
				.SetLightBrightness(UE_PI * 1.2f).SetSkyBrightness(1.f));
			Target.Reset(NewObject<UTextureRenderTarget2D>());
			Target->RenderTargetFormat = RTF_RGBA8;
			Target->ClearColor = FLinearColor(.025f, .035f, .04f);
			Target->InitAutoFormat(1120, 560);
			Capture = NewObject<USceneCaptureComponent2D>();
			Capture->CaptureSource = SCS_FinalColorLDR;
			Capture->TextureTarget = Target.Get();
			Capture->bCaptureEveryFrame = false; Capture->bCaptureOnMovement = false;
			Capture->ProjectionType = ECameraProjectionMode::Orthographic;
			Capture->bAutoCalculateOrthoPlanes = false; Capture->bUpdateOrthoPlanes = false;
			Scene->AddComponent(Capture, FTransform::Identity);
			Mesh = NewObject<UStaticMeshComponent>();
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Scene->AddComponent(Mesh, FTransform::Identity);
			Brush.SetResourceObject(Target.Get()); Brush.ImageSize = FVector2D(560, 280);
			ChildSlot[SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("Select an attachment point on the weapon")))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 5)
			[SNew(SBox).HeightOverride(280)[SNew(SScaleBox).Stretch(EStretch::ScaleToFit)
			[SNew(SBox).WidthOverride(560).HeightOverride(280)[SAssignNew(Canvas, SConstraintCanvas)]]]]
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock)
				.Text_Lambda([this]{ return FText::FromString(Mesh->GetStaticMesh() ? TEXT("More slots and factory reset are available below.") : SelectionStatus(Object.Id)); })
				.AutoWrapText(true)]];
			Canvas->AddSlot().Anchors(FAnchors(0, 0, 1, 1)).Offset(FMargin(0))[SNew(SImage).Image(&Brush)];
		}
		virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override
		{
			SCompoundWidget::Tick(Geometry, Time, Delta);
			if (Time - LastPoll < .25) return;
			LastPoll = Time;
			TMap<FString, FVector3f> Anchors;
			UStaticMesh* Ready = PreviewMesh(Object.Id, Anchors);
			if (Ready == Mesh->GetStaticMesh()) return;
			Mesh->SetStaticMesh(Ready);
			Canvas->ClearChildren();
			Canvas->AddSlot().Anchors(FAnchors(0, 0, 1, 1)).Offset(FMargin(0))[SNew(SImage).Image(&Brush)];
			if (!Ready) { Capture->CaptureScene(); return; }
			const FBox Box = Ready->GetBoundingBox();
			const FVector Center = Box.GetCenter();
			// Source +Z is the barrel, mapped to Unreal +Y by DescribeMesh.
			// Looking along +X gives a side view, with +Y to the right on screen.
			const double Width = FMath::Max3(Box.GetSize().Y * 1.22, Box.GetSize().Z * 2.44, 20.0);
			Capture->OrthoWidth = float(Width);
			Capture->SetWorldLocationAndRotation(Center - FVector(500 + Box.GetSize().X, 0, 0), FRotator::ZeroRotator);
			Capture->CaptureScene();
			const TMap<FString, FString> Labels{{TEXT("scp"),TEXT("Optic")},{TEXT("sca"),TEXT("Canted")},
				{TEXT("brl"),TEXT("Barrel")},{TEXT("mzl"),TEXT("Muzzle")},{TEXT("mag"),TEXT("Mag")},{TEXT("btm"),TEXT("Grip")}};
			TArray<FVector2D> Used;
			for (const TCHAR* Slot : {TEXT("scp"),TEXT("sca"),TEXT("brl"),TEXT("mzl"),TEXT("mag"),TEXT("btm")})
			{
				const FVector3f* P = Anchors.Find(Slot); if (!P) continue;
				const FString Field = FString(TEXT("attachment_")) + Slot;
				const TArray<FChoice> Options = Choices(Field, Object); if (Options.Num() <= 1) continue;
				const FVector UnrealPoint(P->X * 100., P->Z * 100., P->Y * 100.);
				FVector2D Point(280 + (UnrealPoint.Y - Center.Y) * 560 / Width,
					140 - (UnrealPoint.Z - Center.Z) * 560 / Width);
				Point.X = FMath::Clamp(Point.X, 30., 530.); Point.Y = FMath::Clamp(Point.Y, 15., 265.);
				for (const FVector2D& Other : Used)
					if (FMath::Abs(Point.X - Other.X) < 60 && FMath::Abs(Point.Y - Other.Y) < 25)
						Point.Y = FMath::Clamp(Other.Y + 27, 15., 265.);
				Used.Add(Point);
				const FString ActorId = Object.Id;
				Canvas->AddSlot().Offset(FMargin(Point.X - 29, Point.Y - 12, 58, 24))
				[SNew(SComboButton).HasDownArrow(false).ContentPadding(FMargin(2))
					.ToolTipText(FText::FromString(Labels[Slot] + TEXT(": choose an attachment")))
					.ButtonContent()[SNew(STextBlock).Text(FText::FromString(Labels[Slot])).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))]
					.OnGetMenuContent_Lambda([Options, ActorId, Field]
					{
						return ChoiceMenu(Options, [ActorId, Field](const FChoice& Choice)
						{
							if (!BF6Ext::SetObjectPreviews(ActorId, {{Field, Choice.Id}}))
								BF6Ext::Notify(TEXT("The selected spawner is no longer editable."));
						});
					})];
			}
		}
	private:
		BF6Ext::FObjectPreview Object;
		TUniquePtr<FPreviewScene> Scene;
		TStrongObjectPtr<UTextureRenderTarget2D> Target;
		USceneCaptureComponent2D* Capture = nullptr;
		UStaticMeshComponent* Mesh = nullptr;
		TSharedPtr<SConstraintCanvas> Canvas;
		FSlateBrush Brush;
		double LastPoll = 0;
	};
}
TSharedRef<SWidget> WeaponPreviewWidget(const BF6Ext::FObjectPreview& Object)
{
	return SNew(SWeaponPreview, Object);
}
}
