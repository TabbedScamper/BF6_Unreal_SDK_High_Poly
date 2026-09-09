#include "BF6HighPolyTheme.h"
#include "BF6SDKExtension.h"      // BF6Ext::ToolPluginDir, for the SDK's own copy

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"

namespace BF6HPTheme
{
namespace
{
	bool GLoaded = false;
	FLinearColor GAccent, GAccentDim, GHeading, GSplash;
	FString GSeason;

	// highpoly_theme.gd _mask(): set_corner_radius_all(3),
	// set_border_width_all(1).
	constexpr float kCornerRadius = 3.f;
	constexpr float kBorderWidth  = 1.f;
	// highpoly_theme.gd FONT_SCALE: the panel is deliberately bigger
	// than the surrounding editor chrome.
	constexpr float kFontScale = 1.5f;

	// The Godot plugin's own fallback, kept identical on purpose: an install
	// with no palette file should look the same in both tools rather than
	// diverging into two different "defaults".
	const TCHAR* kFallbackAccent    = TEXT("#ff3d00");
	const TCHAR* kFallbackAccentDim = TEXT("#a82800");
	const TCHAR* kFallbackHeading   = TEXT("#ffb59e");
	const TCHAR* kFallbackSplash    = TEXT("#140a06");

	FLinearColor FromHex(const FString& Hex, const TCHAR* Fallback)
	{
		FString S = Hex.TrimStartAndEnd();
		if (S.IsEmpty()) { S = Fallback; }
		S.RemoveFromStart(TEXT("#"));
		if (S.Len() != 6 && S.Len() != 8) { S = FString(Fallback).RightChop(1); }
		const FColor C = FColor::FromHex(S.Len() == 6 ? (TEXT("#") + S) : (TEXT("#") + S));
		// sRGB: these are colours a person picked by eye in a colour picker, so
		// they are gamma encoded. Reading them as linear washes the palette out.
		return FLinearColor::FromSRGBColor(C);
	}

	// The add-on's own copy ships beside it. The SDK's copy wins when it is
	// there, because that is the file the user edits to change season and the
	// two tools should not drift apart.
	FString PalettePath()
	{
		TArray<FString> Tries;
		if (TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("BF6HighPoly")))
		{
			Tries.Add(FPaths::Combine(P->GetBaseDir(), TEXT("Resources"), TEXT("theme"), TEXT("theme.json")));
		}
		// The SDK checkout, when this editor is pointed at one.
		const FString Sdk = BF6Ext::GameInstallDir();   // not the palette, but the SDK sits beside the project
		(void)Sdk;
		FString Best;
		for (const FString& T : Tries)
		{
			if (FPaths::FileExists(T)) { Best = T; break; }
		}
		return Best;
	}

	void Load()
	{
		if (GLoaded) { return; }
		GLoaded = true;
		GAccent    = FromHex(TEXT(""), kFallbackAccent);
		GAccentDim = FromHex(TEXT(""), kFallbackAccentDim);
		GHeading   = FromHex(TEXT(""), kFallbackHeading);
		GSplash    = FromHex(TEXT(""), kFallbackSplash);
		GSeason    = TEXT("no palette installed");

		FString Text;
		const FString Path = PalettePath();
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path)) { return; }
		TSharedPtr<FJsonObject> J;
		const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, J) || !J.IsValid()) { return; }

		FString S;
		if (J->TryGetStringField(TEXT("accent"), S))     { GAccent    = FromHex(S, kFallbackAccent); }
		if (J->TryGetStringField(TEXT("accent_dim"), S)) { GAccentDim = FromHex(S, kFallbackAccentDim); }
		if (J->TryGetStringField(TEXT("heading"), S))    { GHeading   = FromHex(S, kFallbackHeading); }
		if (J->TryGetStringField(TEXT("splash_bg"), S))  { GSplash    = FromHex(S, kFallbackSplash); }
		J->TryGetStringField(TEXT("name"), GSeason);
	}

	// Slate holds a raw pointer to a brush for as long as the widget lives, so
	// a brush built on the stack and handed over is a use-after-free waiting to
	// happen. These are owned here, keyed by colour, and never freed.
	TMap<uint32, TSharedPtr<FSlateBrush>> GBrushes;
}

void Reload() { GLoaded = false; GBrushes.Reset(); Load(); }

FLinearColor Accent()    { Load(); return GAccent; }
FLinearColor AccentDim() { Load(); return GAccentDim; }
FLinearColor Heading()   { Load(); return GHeading; }
FLinearColor SplashBg()  { Load(); return GSplash; }
FString      SeasonName(){ Load(); return GSeason; }

const FSlateBrush* Solid(const FLinearColor& C)
{
	const uint32 Key = C.ToFColor(false).ToPackedARGB();
	if (TSharedPtr<FSlateBrush>* Found = GBrushes.Find(Key)) { return Found->Get(); }
	TSharedPtr<FSlateBrush> B = MakeShared<FSlateBrush>();
	B->TintColor = FSlateColor(C);
	B->DrawAs = ESlateBrushDrawType::Image;
	// A 1x1 white image tinted by the colour above. FCoreStyle's "WhiteBrush"
	// is the one image every style is guaranteed to have.
	B->SetResourceObject(nullptr);
	B->ImageSize = FVector2D(8.f, 8.f);
	GBrushes.Add(Key, B);
	return B.Get();
}

FLinearColor MaskBorder(EMask M)
{
	switch (M)
	{
	case EMask::Hover:       return FLinearColor(1.f, 1.f, 1.f, 0.85f);
	case EMask::Press:       return FLinearColor(1.f, 1.f, 1.f, 1.00f);
	case EMask::Disabled:    return FLinearColor(1.f, 1.f, 1.f, 0.16f);
	case EMask::ChipOff:     return FLinearColor(1.f, 1.f, 1.f, 0.45f);
	case EMask::ChipHover:   return FLinearColor(1.f, 1.f, 1.f, 0.80f);
	case EMask::ChipOn:      return FLinearColor::White;
	case EMask::ChipOnHover: return FLinearColor::White;
	default:                 return FLinearColor(1.f, 1.f, 1.f, 0.55f);
	}
}

// Godot's Color.lightened(a): each channel moves that fraction of the way to
// white. Used for the hovered state of a switched-on chip.
FLinearColor Lightened(const FLinearColor& C, float A)
{
	return FLinearColor(C.R + (1.f - C.R) * A,
	                    C.G + (1.f - C.G) * A,
	                    C.B + (1.f - C.B) * A, C.A);
}

// ONE MASK, ROUNDED AND OUTLINED, exactly as highpoly_theme.gd's _mask builds
// it: a translucent white fill, a 1 px white border at its own strength, and
// corners rounded by 3. The fill and the border are ONE brush here because
// Slate can draw both, where Godot needs a StyleBoxFlat carrying the pair.
//
// This was a flat rectangle with no corner and no outline, which is the single
// biggest reason the panel did not look like the Godot one: every control in
// that plugin is a soft-edged outlined mask, and a hard filled rectangle reads
// as a different toolkit no matter how well the alphas match.
const FSlateBrush* Box(const FLinearColor& Fill, const FLinearColor& Border)
{
	// Key on both halves: the same fill appears at more than one border
	// strength (the focus box is fill 0), so keying on the fill alone hands
	// back a brush with the wrong outline.
	const uint32 Key = Fill.ToFColor(false).ToPackedARGB()
		^ (Border.ToFColor(false).ToPackedARGB() * 2654435761u);
	if (TSharedPtr<FSlateBrush>* Found = GBrushes.Find(Key)) { return Found->Get(); }
	TSharedPtr<FSlateBrush> B = MakeShared<FSlateRoundedBoxBrush>(
		Fill, kCornerRadius, Border, kBorderWidth);
	GBrushes.Add(Key, B);
	return B.Get();
}

const FSlateBrush* Mask(EMask M)
{
	// White on translucent white, at the strengths in highpoly_theme.gd.
	// Deliberately NOT tinted with the accent, with one exception: a chip that
	// is switched ON. Godot draws a toggle button's "pressed" box for as long
	// as it is on, and that is the state worth colouring, because it is the one
	// thing in a row that says "this is active right now".
	const FLinearColor B = MaskBorder(M);
	switch (M)
	{
	case EMask::Hover:       return Box(FLinearColor(1.f, 1.f, 1.f, 0.20f), B);
	case EMask::Press:       return Box(FLinearColor(1.f, 1.f, 1.f, 0.30f), B);
	case EMask::Disabled:    return Box(FLinearColor(1.f, 1.f, 1.f, 0.04f), B);
	case EMask::ChipOff:     return Box(FLinearColor(1.f, 1.f, 1.f, 0.08f), B);
	case EMask::ChipHover:   return Box(FLinearColor(1.f, 1.f, 1.f, 0.18f), B);
	case EMask::ChipOn:      return Box(Accent(), B);
	case EMask::ChipOnHover: return Box(Lightened(Accent(), 0.15f), B);
	default:                 return Box(FLinearColor(1.f, 1.f, 1.f, 0.10f), B);
	}
}

FSlateFontInfo Font(int32 Size)
{
	// The season face, loaded straight off disk the way the Godot side does, so
	// swapping it needs no reimport. Falls back to the editor's own font, which
	// is what the Godot plugin does too when the face is missing.
	static FString Face;
	static bool bTried = false;
	if (!bTried)
	{
		bTried = true;
		if (TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("BF6HighPoly")))
		{
			const FString T = FPaths::Combine(P->GetBaseDir(), TEXT("Resources"), TEXT("theme"), TEXT("ui_font.ttf"));
			if (FPaths::FileExists(T)) { Face = T; }
		}
	}
	// THE PANEL RUNS LARGER THAN THE EDITOR, by exactly the factor the Godot
	// plugin uses (highpoly_theme.gd FONT_SCALE). Every size passed in here was
	// chosen against the Godot sizes, so scaling in one place keeps the whole
	// panel in proportion instead of nine call sites drifting apart.
	const int32 Scaled = FMath::Max(1, FMath::RoundToInt(float(Size) * kFontScale));
	if (Face.IsEmpty()) { return FCoreStyle::GetDefaultFontStyle("Regular", Scaled); }
	return FSlateFontInfo(Face, Scaled);
}

} // namespace BF6HPTheme
