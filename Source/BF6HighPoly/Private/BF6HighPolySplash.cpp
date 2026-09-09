#include "BF6HighPolySplash.h"
#include "BF6HighPolyTheme.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "ImageUtils.h"
#include "Engine/Texture2D.h"
#include "Rendering/DrawElements.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	// The timeline, from highpoly_splash.gd. Same names, same numbers.
	const float kWavesSecs   = 1.30f;
	const float kLogoIn      = 0.30f;
	const float kLogoHold    = 1.50f;
	const float kLogoOut     = 0.70f;
	const float kSettleSecs  = 1.20f;
	const float kUiDelay     = 0.30f;
	const float kUiSecs      = 0.85f;
	const float kLogoMaxFill = 0.62f;   // the logo never touches the panel edges
	const float kTintMax     = 0.72f;   // theme.json's tint

	FString ThemeDir()
	{
		if (TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("BF6HighPoly")))
		{
			return FPaths::Combine(P->GetBaseDir(), TEXT("Resources"), TEXT("theme"));
		}
		return FString();
	}

	// The artwork, loaded once and kept for the editor's life. Rooted because
	// Slate holds the brush by raw pointer and a collected texture would be a
	// crash rather than a missing picture.
	struct FArt
	{
		TStrongObjectPtr<UTexture2D> Sheet;
		TStrongObjectPtr<UTexture2D> Logo;
		FSlateBrush SheetBrush;
		FSlateBrush LogoBrush;
		int32 Cells = 0, Cols = 12, Rows = 8, CellW = 160, CellH = 267, Fps = 12;
		bool  bTried = false;
	};

	FArt& Art()
	{
		static FArt A;
		if (A.bTried) { return A; }
		A.bTried = true;
		const FString Dir = ThemeDir();
		if (Dir.IsEmpty()) { return A; }

		// The sheet's own description travels with it, so the cell grid is read
		// rather than assumed: a re-extracted loop with different numbers works
		// without touching this code.
		FString Meta;
		if (FFileHelper::LoadFileToString(Meta, *FPaths::Combine(Dir, TEXT("waves_sheet.json"))))
		{
			TSharedPtr<FJsonObject> J;
			const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Meta);
			if (FJsonSerializer::Deserialize(R, J) && J.IsValid())
			{
				J->TryGetNumberField(TEXT("cells"),  A.Cells);
				J->TryGetNumberField(TEXT("cols"),   A.Cols);
				J->TryGetNumberField(TEXT("rows"),   A.Rows);
				J->TryGetNumberField(TEXT("cell_w"), A.CellW);
				J->TryGetNumberField(TEXT("cell_h"), A.CellH);
				J->TryGetNumberField(TEXT("fps"),    A.Fps);
			}
		}

		auto Load = [](const FString& Path) -> UTexture2D*
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Path)) { return nullptr; }
			// Straight off disk rather than through the asset system, the way
			// the Godot side loads its artwork, so a replacement drops in with
			// no reimport.
			return FImageUtils::ImportBufferAsTexture2D(Bytes);
		};

		if (A.Cells > 0)
		{
			if (UTexture2D* T = Load(FPaths::Combine(Dir, TEXT("waves_sheet.jpg"))))
			{
				A.Sheet.Reset(T);
				A.SheetBrush.SetResourceObject(T);
				A.SheetBrush.ImageSize = FVector2D(A.CellW, A.CellH);
				A.SheetBrush.DrawAs = ESlateBrushDrawType::Image;
			}
		}
		if (UTexture2D* T = Load(FPaths::Combine(Dir, TEXT("logo.png"))))
		{
			A.Logo.Reset(T);
			A.LogoBrush.SetResourceObject(T);
			A.LogoBrush.ImageSize = FVector2D(T->GetSizeX(), T->GetSizeY());
			A.LogoBrush.DrawAs = ESlateBrushDrawType::Image;
		}
		return A;
	}
}

bool SBF6HighPolySplash::ArtworkPresent()
{
	const FArt& A = Art();
	return A.Sheet.IsValid() || A.Logo.IsValid();
}

void SBF6HighPolySplash::Construct(const FArguments& InArgs)
{
	Art();
	SkipEmpty();
	LastTick = FPlatformTime::Seconds();
	ChildSlot[ InArgs._Content.Widget ];
}

void SBF6HighPolySplash::SkipEmpty()
{
	// Every asset is optional, as on the Godot side: no sheet skips stage 1, no
	// logo skips stage 2, and with neither the panel simply appears.
	const FArt& A = Art();
	if (Stage == EStage::Waves && !A.Sheet.IsValid()) { Stage = EStage::Logo; T = 0.f; }
	if (Stage == EStage::Logo  && !A.Logo.IsValid())  { Stage = EStage::Settle; T = 0.f; }
}

void SBF6HighPolySplash::Skip()
{
	// Land on the end state directly. However far the sequence got, the panel
	// must be left exactly as a finished run would leave it.
	Stage = EStage::Done;
	TintA = kTintMax;
	UiA = 1.f;
}

float SBF6HighPolySplash::LogoAlpha() const
{
	if (T < kLogoIn) { return T / kLogoIn; }
	if (T < kLogoIn + kLogoHold) { return 1.f; }
	return FMath::Clamp(1.f - (T - kLogoIn - kLogoHold) / kLogoOut, 0.f, 1.f);
}

void SBF6HighPolySplash::Tick(const FGeometry& Geo, const double Now, const float Delta)
{
	SCompoundWidget::Tick(Geo, Now, Delta);
	// The waves run on their own clock and never stop: the loop carries on long
	// after the entrance has finished, which is the whole point of it.
	WaveT += Delta;
	if (Stage == EStage::Done) { return; }

	T += Delta;
	switch (Stage)
	{
	case EStage::Waves:
		if (T >= kWavesSecs) { Stage = EStage::Logo; T = 0.f; SkipEmpty(); }
		break;
	case EStage::Logo:
		if (T >= kLogoIn + kLogoHold + kLogoOut) { Stage = EStage::Settle; T = 0.f; }
		break;
	case EStage::Settle:
		TintA = kTintMax * FMath::Clamp(T / kSettleSecs, 0.f, 1.f);
		UiA = FMath::Clamp((T - kUiDelay) / kUiSecs, 0.f, 1.f);
		if (T >= FMath::Max(kSettleSecs, kUiDelay + kUiSecs)) { Skip(); }
		break;
	default:
		break;
	}
}

FReply SBF6HighPolySplash::OnMouseButtonDown(const FGeometry&, const FPointerEvent&)
{
	if (Stage != EStage::Done)
	{
		Skip();
		// Swallowed: the click that skips the entrance should not also press
		// whatever control happens to be under the cursor.
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

int32 SBF6HighPolySplash::OnPaint(const FPaintArgs& Args, const FGeometry& Geo, const FSlateRect& Culled,
	FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const
{
	FArt& A = Art();
	const FVector2D Size = Geo.GetLocalSize();

	// ---- the waves, always, underneath everything ----------------------
	if (A.Sheet.IsValid() && A.Cells > 0)
	{
		const int32 Frame = A.Fps > 0
			? (int32)(WaveT * A.Fps) % A.Cells
			: 0;
		const int32 Col = Frame % A.Cols;
		const int32 Row = Frame / A.Cols;
		// A UV window over the one sheet, rather than one texture per frame.
		FSlateBrush B = A.SheetBrush;
		B.SetUVRegion(FBox2f(
			FVector2f((float)Col / A.Cols, (float)Row / A.Rows),
			FVector2f((float)(Col + 1) / A.Cols, (float)(Row + 1) / A.Rows)));
		FSlateDrawElement::MakeBox(Out, Layer, Geo.ToPaintGeometry(), &B,
			ESlateDrawEffect::None, FLinearColor::White);
	}
	else
	{
		// No sheet: the splash colour is the floor, which is what theme.json
		// says it is for.
		FSlateDrawElement::MakeBox(Out, Layer, Geo.ToPaintGeometry(),
			BF6HPTheme::Solid(BF6HPTheme::SplashBg()), ESlateDrawEffect::None, FLinearColor::White);
	}

	// ---- the dim in front of them --------------------------------------
	if (TintA > 0.f)
	{
		FLinearColor Tint = BF6HPTheme::SplashBg();
		Tint.A = TintA;
		FSlateDrawElement::MakeBox(Out, Layer + 1, Geo.ToPaintGeometry(),
			BF6HPTheme::Solid(FLinearColor::White), ESlateDrawEffect::None, Tint);
	}

	// ---- the panel, fading in during the settle ------------------------
	int32 Top = Layer + 2;
	if (UiA > 0.f)
	{
		FWidgetStyle Faded = FWidgetStyle(Style).BlendOpacity(UiA);
		Top = SCompoundWidget::OnPaint(Args, Geo, Culled, Out, Layer + 2, Faded, bParentEnabled);
	}

	// ---- the logo, centred, contained, never stretched ------------------
	if (Stage == EStage::Logo && A.Logo.IsValid())
	{
		const float W = A.LogoBrush.ImageSize.X;
		const float H = A.LogoBrush.ImageSize.Y;
		if (W > 0.f && H > 0.f)
		{
			const float S = FMath::Min(Size.X * kLogoMaxFill / W, Size.Y * kLogoMaxFill / H);
			const FVector2D D(W * S, H * S);
			FSlateDrawElement::MakeBox(Out, Top + 1,
				Geo.ToPaintGeometry(D, FSlateLayoutTransform((Size - D) * 0.5f)),
				&A.LogoBrush, ESlateDrawEffect::None,
				FLinearColor(1.f, 1.f, 1.f, LogoAlpha()));
		}
	}
	return Top + 2;
}
