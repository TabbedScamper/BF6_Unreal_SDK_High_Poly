#pragma once

#include <algorithm>
#include <cmath>

// HardwareDfIcon's distance-field coverage and separate fill/line accumulators.
// Atlas alpha is unused. Inputs and accumulators are linear, premultiplied.
// Width is textureHeight * fwidth(atlasUV.y) / 24, evaluated at output size.
namespace BF6HP::HardwareIcon
{
inline float Clamp(float X, float Lo = 0.f, float Hi = 1.f) { return std::max(Lo, std::min(Hi, X)); }
struct Color { float R = 0, G = 0, B = 0, A = 0; };
inline Color Scale(Color C, float S) { return {C.R*S, C.G*S, C.B*S, C.A*S}; }
inline Color Over(Color Top, Color Bottom)
{
    const float S = 1-Top.A;
    return {Top.R+Bottom.R*S, Top.G+Bottom.G*S, Top.B+Bottom.B*S, Top.A+Bottom.A*S};
}
inline float Ramp(float Value, float Low, float Width)
{
    return std::abs(Width) < .000001f ? 0.f : Clamp((Value-Low)/Width);
}
inline float FillCoverage(float G, float Width) { return Ramp(G, .5f-Width*.5f, Width); }
inline float LineCoverage(float R, float Width, float Thickness, float ScaleFactor = 1.f)
{
    const float W = Clamp(Width*2), H = W*.5f;
    return Ramp(R, Clamp(1-Width*.5f*Thickness*ScaleFactor, H, 1)-H, W);
}
struct State
{
    Color Fill, Line;
    float MaximumG = 0, MaximumWidth = 0, MaximumFillAlpha = 0;
    void Layer(float R, float G, float Width, Color FillColor, Color LineColor,
               float Thickness, bool Lines = true, float ScaleFactor = 1.f)
    {
        Width = Clamp(Width);
        const float Coverage = FillCoverage(G, Width);
        // Covered artwork erases previous outlines even for transparent fill.
        Fill = Scale(Fill, 1-Coverage);
        Line = Scale(Line, 1-Coverage);
        Fill = Over(Scale({FillColor.R, FillColor.G, FillColor.B, 1}, Coverage*FillColor.A), Fill);
        if (Lines) Line = Over(Scale({LineColor.R, LineColor.G, LineColor.B, 1}, LineCoverage(R, Width, Thickness, ScaleFactor)), Line);
        if (G > MaximumG) { MaximumG=G; MaximumWidth=Width; MaximumFillAlpha=FillColor.A; }
    }
    Color Finish(bool Outline = false, Color OutlineColor = {}, float Thickness = 0, float ScaleFactor = 1.f) const
    {
        Color Result = Over(Line, Fill);
        if (Outline)
        {
            const float Low=.5f-MaximumWidth*.5f;
            const float Outside=Ramp(MaximumG+Clamp(Thickness*MaximumWidth*ScaleFactor, 0, Low), Low, MaximumWidth);
            const float Inside=Ramp(MaximumG-MaximumFillAlpha*MaximumWidth, Low, MaximumWidth);
            const float A=(Outside-Inside)*(1-Result.A);
            Result.R+=A*OutlineColor.R; Result.G+=A*OutlineColor.G; Result.B+=A*OutlineColor.B; Result.A+=A;
        }
        return Result;
    }
};
}
