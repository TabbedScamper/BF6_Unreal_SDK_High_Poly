#include "BF6HighPolySection.h"
#include "BF6HighPolyTheme.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Rendering/DrawElements.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"

namespace
{
	// highpoly_section.gd, verbatim.
	constexpr float kLineGap    = 7.f;
	constexpr float kLineH      = 2.f;
	constexpr float kEdge       = 22.f;
	constexpr float kFade       = 64.f;
	constexpr float kTitlePad   = 4.f;
	constexpr float kContentTop = 8.f;
	constexpr float kOpenSecs   = 0.40f;
	constexpr float kHlSecs     = 0.28f;
	constexpr float kRise       = 24.f;
	// Pal.fs(16) on the Godot side; BF6HPTheme::Font already applies the scale.
	constexpr int32 kTitleSize  = 16;

	// Godot's Tween.TRANS_CUBIC with Tween.EASE_OUT.
	float EaseOutCubic(float T)
	{
		const float U = 1.f - FMath::Clamp(T, 0.f, 1.f);
		return 1.f - U * U * U;
	}
}

float SBF6HighPolySection::FTween::Value() const
{
	if (Duration <= 0.f) { return To; }
	return FMath::Lerp(From, To, EaseOutCubic(Elapsed / Duration));
}

void SBF6HighPolySection::FTween::To_(float NewTo, float Secs)
{
	if (FMath::IsNearlyEqual(NewTo, To) && IsDone()) { return; }
	From = Value();          // from wherever it actually is now
	To = NewTo;
	Elapsed = 0.f;
	Duration = FMath::Max(Secs, KINDA_SMALL_NUMBER);
}

// ---------------------------------------------------------------------------
// The header: a centred title with a line under it that dissolves at both ends.
// ---------------------------------------------------------------------------
void SBF6HighPolySectionHead::Construct(const FArguments& InArgs)
{
	Title = InArgs._Title;
	Highlight = InArgs._Highlight;
}

FVector2D SBF6HighPolySectionHead::ComputeDesiredSize(float) const
{
	const TSharedRef<FSlateFontMeasure> Measure =
		FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FVector2D T = Measure->Measure(Title, BF6HPTheme::Font(kTitleSize));
	// head.custom_minimum_size.y = title height + LINE_GAP + LINE_H + TITLE_PAD
	return FVector2D(T.X + kEdge * 2.f, T.Y + kLineGap + kLineH + kTitlePad);
}

int32 SBF6HighPolySectionHead::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const float H = Highlight.IsSet() ? Highlight.Get() : 0.f;
	const FLinearColor C = FMath::Lerp(FLinearColor::White, BF6HPTheme::Accent(), H);

	const FSlateFontInfo TitleFont = BF6HPTheme::Font(kTitleSize);
	const TSharedRef<FSlateFontMeasure> Measure =
		FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FVector2D T = Measure->Measure(Title, TitleFont);

	const FVector2D Size = AllottedGeometry.GetLocalSize();

	// The title, centred, exactly as _title_x does it.
	const float TitleX = FMath::Max(kEdge, (Size.X - T.X) * 0.5f);
	FSlateDrawElement::MakeText(
		OutDrawElements, LayerId,
		AllottedGeometry.ToPaintGeometry(FVector2f(T.X, T.Y), FSlateLayoutTransform(FVector2f(TitleX, 0.f))),
		Title, TitleFont, ESlateDrawEffect::None, C);

	// The line, at LINE_GAP below the title, in three runs so the ends dissolve.
	const float Y  = T.Y + kLineGap;
	const float X0 = kEdge;
	const float X3 = FMath::Max(X0 + 4.f, Size.X - kEdge);
	const float F  = FMath::Min(kFade, (X3 - X0) * 0.45f);
	const FLinearColor Clear(C.R, C.G, C.B, 0.f);

	auto Run = [&](float XA, float XB, const FLinearColor& CA, const FLinearColor& CB)
	{
		if (XB <= XA) { return; }
		TArray<FSlateGradientStop> Stops;
		Stops.Add(FSlateGradientStop(FVector2D(0.f, 0.f), CA));
		Stops.Add(FSlateGradientStop(FVector2D(XB - XA, 0.f), CB));
		FSlateDrawElement::MakeGradient(
			OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(FVector2f(XB - XA, kLineH),
				FSlateLayoutTransform(FVector2f(XA, Y))),
			MoveTemp(Stops), Orient_Vertical);
	};
	Run(X0, X0 + F, Clear, C);
	Run(X0 + F, X3 - F, C, C);
	Run(X3 - F, X3, C, Clear);

	return LayerId + 2;
}

// ---------------------------------------------------------------------------
// The section.
// ---------------------------------------------------------------------------
float SBF6HighPolySection::ContentHeight() const
{
	return Contents.IsValid() ? float(Contents->GetDesiredSize().Y) : 0.f;
}

void SBF6HighPolySection::Kick()
{
	if (!Anim.IsValid())
	{
		Anim = RegisterActiveTimer(0.f,
			FWidgetActiveTimerDelegate::CreateSP(this, &SBF6HighPolySection::OnAnimate));
	}
}

EActiveTimerReturnType SBF6HighPolySection::OnAnimate(double InTime, float InDelta)
{
	Reveal.Advance(InDelta);
	Fade.Advance(InDelta);
	Warm.Advance(InDelta);

	if (Contents.IsValid())
	{
		// The rise: the contents sit RISE above their resting place when shut
		// and slide down to it, clipped by the box so they emerge from behind
		// the line. A RENDER transform, not padding, so the layout underneath
		// does not move while it plays.
		const float R = Reveal.Value();
		const float Y = FMath::Lerp(kContentTop - kRise, kContentTop, R);
		Contents->SetRenderTransform(FSlateRenderTransform(FVector2f(0.f, Y)));
		Contents->SetRenderOpacity(Fade.Value());
	}

	const bool bDone = Reveal.IsDone() && Fade.IsDone() && Warm.IsDone();
	if (bDone)
	{
		// Fully shut: stop the contents taking clicks they cannot be seen to
		// offer. Fully open: hand interaction back.
		if (Contents.IsValid())
		{
			Contents->SetVisibility(bOpen ? EVisibility::SelfHitTestInvisible
			                              : EVisibility::Hidden);
		}
		Anim.Reset();
		return EActiveTimerReturnType::Stop;
	}
	return EActiveTimerReturnType::Continue;
}

FReply SBF6HighPolySection::OnHeaderClicked()
{
	SetOpen(!bOpen);
	return FReply::Handled();
}

void SBF6HighPolySection::OnHoverChanged(bool bIn)
{
	bHovered = bIn;
	// An open section stays lit: this is _on_hover's early return.
	if (bOpen) { return; }
	Warm.To_(bIn ? 1.f : 0.f, kHlSecs);
	Kick();
}

void SBF6HighPolySection::SetOpen(bool bInOpen, bool bAnimate)
{
	bOpen = bInOpen;

	if (Contents.IsValid())
	{
		// Visible before the reveal starts, or there is nothing to animate and
		// nothing to measure. Hidden (not Collapsed) while shut so the contents
		// still report a desired size for the box to grow into.
		Contents->SetVisibility(EVisibility::SelfHitTestInvisible);
	}

	if (!bAnimate)
	{
		Reveal.Snap(bOpen ? 1.f : 0.f);
		Fade.Snap(bOpen ? 1.f : 0.f);
		Warm.Snap(bOpen ? 1.f : 0.f);
		if (Contents.IsValid())
		{
			Contents->SetRenderTransform(FSlateRenderTransform(
				FVector2f(0.f, bOpen ? kContentTop : kContentTop - kRise)));
			Contents->SetRenderOpacity(bOpen ? 1.f : 0.f);
			Contents->SetVisibility(bOpen ? EVisibility::SelfHitTestInvisible : EVisibility::Hidden);
		}
		return;
	}

	Reveal.To_(bOpen ? 1.f : 0.f, kOpenSecs);
	// The fade is deliberately not the same length as the movement: opening
	// spends most of the travel fading in, closing goes faster than it moves.
	Fade.To_(bOpen ? 1.f : 0.f, kOpenSecs * (bOpen ? 0.9f : 0.55f));
	Warm.To_(bOpen ? 1.f : (bHovered ? 1.f : 0.f), kHlSecs);
	Kick();
}

void SBF6HighPolySection::Construct(const FArguments& InArgs)
{
	Title = InArgs._Title;
	Description = InArgs._Description;
	bOpen = InArgs._InitiallyOpen;
	Contents = InArgs._Content.Widget;

	// The header is a button so it takes the click and the hover without this
	// widget having to track mouse capture itself. It wears no style at all:
	// the look is entirely what the head paints.
	TSharedRef<SWidget> Head = StaticCastSharedRef<SWidget>(
		SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), "NoBorder")
		.ContentPadding(FMargin(0.f))
		.ToolTipText(FText::FromString(Description))
		.OnClicked(this, &SBF6HighPolySection::OnHeaderClicked)
		.OnHovered_Lambda([this] { OnHoverChanged(true); })
		.OnUnhovered_Lambda([this] { OnHoverChanged(false); })
		[
			SNew(SBF6HighPolySectionHead)
			.Title(Title)
			.Highlight(TAttribute<float>::CreateSP(this, &SBF6HighPolySection::Highlight))
		]);

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ Head ]
		+ SVerticalBox::Slot().AutoHeight()
		[
			// THE BOX WHOSE HEIGHT IS THE ANIMATION. Clipped, so the contents
			// are hidden behind the line until they have risen into view, and
			// so the sections below reflow as this one grows rather than being
			// drawn over.
			SAssignNew(Body, SBox)
			.Clipping(EWidgetClipping::ClipToBounds)
			.HeightOverride_Lambda([this]
			{
				return FOptionalSize(Reveal.Value() * (ContentHeight() + kContentTop));
			})
			[
				Contents.ToSharedRef()
			]
		]
	];

	SetOpen(bOpen, /*bAnimate*/ false);
}
