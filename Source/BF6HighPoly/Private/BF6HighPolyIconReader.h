#pragma once

#include "BF6HighPolyCore.h"
#include "BF6HardwareIcon.h"
#include "HAL/PlatformProcess.h"
THIRD_PARTY_INCLUDES_START
#include "bf6_core.h"
THIRD_PARTY_INCLUDES_END

namespace BF6HP
{
// One bounded reader request owns these copies. Native output pointers never
// survive a subsequent reader call or cross onto the game thread.
class FHardwareIconReader
{
public:
    struct FPage { int32 Width=0, Height=0; TArray<uint8> Pixels; };
    struct FSprite { bf6_icon_sprite Data{}; TSharedPtr<FPage> Page; };
    explicit FHardwareIconReader(FCore& InCore) : Core(InCore) {}
    template<typename T> T Export(const TCHAR* Name) const
    { return reinterpret_cast<T>(FPlatformProcess::GetDllExport(Core.DllHandle(), Name)); }
    bool Read(const FString& Atlas, int32 Index, FSprite& Out)
    {
        if (Atlas.IsEmpty() || Index < 0) return false;
        auto* Sprites=Atlases.Find(Atlas);
        if (!Sprites)
        {
            Sprites=&Atlases.Add(Atlas);
            const auto ReadAtlas=Export<decltype(&bf6_icon_atlas)>(TEXT("bf6_icon_atlas"));
            const int32 N=ReadAtlas ? ReadAtlas(Core.Handle(), TCHAR_TO_UTF8(*Atlas), nullptr, 0) : 0;
            if (N<1 || N>4096) return false;
            Sprites->SetNumZeroed(N);
            if (ReadAtlas(Core.Handle(), TCHAR_TO_UTF8(*Atlas), Sprites->GetData(), N)!=N) { Sprites->Reset(); return false; }
            for (auto& S:*Sprites) S.name=nullptr;
        }
        if (!Sprites->IsValidIndex(Index)) return false;
        Out.Data=(*Sprites)[Index];
        const auto& S=Out.Data;
        if (S.page<0 || S.size[0]<=0 || S.size[1]<=0) return false;
        for (float V:S.uv) if (!FMath::IsFinite(V) || V<0 || V>1) return false;
        if (S.uv[2]<=S.uv[0] || S.uv[3]<=S.uv[1]) return false;
        const FString Name=Atlas+FString::Printf(TEXT("_atlas%d"),S.page);
        if (const auto* Existing=Pages.Find(Name)) { Out.Page=*Existing; return Out.Page.IsValid(); }
        Pages.Add(Name, nullptr);
        const auto Id=Export<decltype(&bf6_texture_id_by_name)>(TEXT("bf6_texture_id_by_name"));
        const auto Texture=Export<decltype(&bf6_texture_at)>(TEXT("bf6_texture_at"));
        const auto Decode=Export<decltype(&bf6_texture_rgba)>(TEXT("bf6_texture_rgba"));
        if (!Id || !Texture || !Decode) return false;
        const int32 TextureId=Id(Core.Handle(),TCHAR_TO_UTF8(*Name));
        const bf6_texture* T=TextureId>=0 ? Texture(Core.Handle(),TextureId) : nullptr;
        if (!T || T->width<1 || T->height<1 || T->width>4096 || T->height>4096) return false;
        const int64 Bytes=int64(T->width)*T->height*4;
        if (Bytes+DecodedBytes>128*1024*1024) return false;
        auto P=MakeShared<FPage>(); P->Width=T->width; P->Height=T->height; P->Pixels.SetNumUninitialized(Bytes);
        if (!Decode(Core.Handle(),TextureId,P->Pixels.GetData(),Bytes)) return false;
        DecodedBytes+=Bytes; Pages[Name]=P; Out.Page=P;
        return true;
    }
    static FVector2f Sample(const FSprite& S, float X, float Y)
    {
        const auto& P=*S.Page;
        const float U=FMath::Lerp(S.Data.uv[0],S.Data.uv[2],X)*P.Width-.5f;
        const float V=FMath::Lerp(S.Data.uv[1],S.Data.uv[3],Y)*P.Height-.5f;
        const int32 X0=FMath::FloorToInt(U),Y0=FMath::FloorToInt(V);
        auto At=[&](int32 PX,int32 PY)
        {
            const int32 I=(FMath::Clamp(PY,0,P.Height-1)*P.Width+FMath::Clamp(PX,0,P.Width-1))*4;
            return FVector2f(P.Pixels[I]/255.f,P.Pixels[I+1]/255.f);
        };
        return FMath::Lerp(FMath::Lerp(At(X0,Y0),At(X0+1,Y0),U-X0),FMath::Lerp(At(X0,Y0+1),At(X0+1,Y0+1),U-X0),V-Y0);
    }
    static float Width(const FSprite& S, float OutputHeight)
    { return HardwareIcon::Clamp(S.Page->Height*(S.Data.uv[3]-S.Data.uv[1])/(24.f*OutputHeight)); }
    static FColor Pixel(HardwareIcon::Color C)
    {
        if (C.A<=.003f) return FColor(0,0,0,0);
        // PNG/Slate use straight alpha. Do not double-premultiply the image.
        return FColor(FMath::RoundToInt(HardwareIcon::Clamp(C.R/C.A)*255), FMath::RoundToInt(HardwareIcon::Clamp(C.G/C.A)*255),
            FMath::RoundToInt(HardwareIcon::Clamp(C.B/C.A)*255),FMath::RoundToInt(HardwareIcon::Clamp(C.A)*255));
    }
private:
    FCore& Core;
    int64 DecodedBytes=0;
    TMap<FString,TArray<bf6_icon_sprite>> Atlases;
    TMap<FString,TSharedPtr<FPage>> Pages;
};
}
