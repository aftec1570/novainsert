#include "gui/level_bars.hpp"
#include <cmath>
#include "gui/controls.hpp"
#include "gui/view_model.hpp"

namespace ni::gui
{
using juce::Colour;
using juce::ColourGradient;
using juce::Rectangle;

// One group's layout (SEND or RETURN) in its face's coordinates. TUBE and LED stack both groups on one 706 × 187 face;
// TWIN gives each group its own 325 × 187 face. Ported from the design canvas (2026-11-06, drawn at 2×).
struct LevelBars::Geo
{
    float sx, sw;                          // the −60 … 0 dBFS axis
    float top;                             // group top
    float dbuTextY, tickY0, tickY1, ruleY; // dBu labels, ticks, the red −9 … 0 rule
    float barY, barH, barStep;             // troughs: stereo rows at barY and barY + barStep, mono spans both
    float fsTickY, fsTextY;                // dBFS ticks and labels below
    float labelX;
    float xOf (float dbfs) const noexcept { return sx + sw * barPosition (dbfs); }
    Rectangle<float> trough (bool stereo, int ch) const noexcept
    {
        return stereo ? Rectangle<float> (sx - 2.0f, barY + (float) ch * barStep, sw + 4.0f, barH)
                      : Rectangle<float> (sx - 2.0f, barY, sw + 4.0f, barStep + barH);
    }
    float barsBottom (bool stereo) const noexcept { return trough (stereo, stereo ? 1 : 0).getBottom(); }
};

namespace
{
constexpr float kFaceW = (float) (LevelBars::kW - 2 * LevelBars::kBezel), kFaceH = (float) (LevelBars::kH - 2 * LevelBars::kBezel);
constexpr float kTwinFaceW = (float) (LevelBars::kTwinW - 2 * LevelBars::kBezel);
constexpr float kReadX = 596.0f, kReadW = 98.0f;   // TUBE / LED PEAK HOLD window

LevelBars::Geo stackGeo (bool send)
{
    const float t = send ? 3.0f : 92.0f;
    return { 80.0f, 500.0f, t, t + 4.0f, t + 16.0f, t + 21.5f, t + 19.5f, t + 25.0f, 17.0f, 19.0f, t + 62.0f, t + 66.0f, 11.0f };
}

LevelBars::Geo twinGeo()
{
    return { 35.0f, 267.0f, 0.0f, 45.0f, 56.0f, 62.5f, 60.5f, 67.0f, 20.0f, 23.0f, 112.0f, 117.0f, 12.0f };
}

float twinX (bool send) { return send ? 0.0f : (float) (LevelBars::kTwinW + LevelBars::kTwinGap); }

struct FacePalette { Colour ink, dim, fsLabel, ref, zero, outboard, chan; };
FacePalette creamPalette() { const auto& p = sk(); return { Colour (p.barInk), Colour (p.barDim), Colour (p.barFs), Colour (p.barRef), Colour (p.barZero), Colour (p.barOutboard), Colour (p.barChan) }; }
const FacePalette kSmoke { Colour (0xFFD9D2BC), Colour (0xFF8F8A7A), Colour (0xFFA9A28C), Colour (0xFFFFCF6A), Colour (0xFFFF5A4A), Colour (0xFFC8781E), Colour (0xFF8F8A7A) };
const Colour kLcdGreen { 0xFF7CF29A }, kLcdRed { 0xFFFF6A5E };

Colour zoneColour (int z)
{
    switch (z)
    {
        case 3: return Colour (226, 57, 46);
        case 2: return Colour (236, 160, 54);
        case 1: return Colour (232, 205, 80);
        default: return Colour (80, 214, 104);
    }
}

// radial gradient with an elliptical footprint (CSS "ellipse rx ry at cx cy"): drawn circular in a y-squashed space
void ellipseRadial (juce::Graphics& g, Rectangle<float> area, float cx, float cy, float rx, float ry, std::initializer_list<std::pair<double, Colour>> stops)
{
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (area.toNearestInt());
    const float k = ry / rx;
    g.addTransform (juce::AffineTransform::scale (1.0f, k, cx, cy));
    ColourGradient grad (stops.begin()->second, cx, cy, (stops.end() - 1)->second, cx + rx, cy, true);
    for (const auto& s : stops) grad.addColour (s.first, s.second);
    g.setGradientFill (grad);
    g.fillRect (area.getX(), cy + (area.getY() - cy) / k, area.getWidth(), area.getHeight() / k);
}

// CSS linear-gradient (angle) over a rectangle, white at the given alphas: the diagonal glass sheen
void sheen (juce::Graphics& g, Rectangle<float> r, float angleDeg, float a0, float a1, double p1, double p2)
{
    const float rad = juce::degreesToRadians (angleDeg), dx = std::sin (rad), dy = -std::cos (rad);
    const float len = std::abs (r.getWidth() * dx) + std::abs (r.getHeight() * dy);
    const auto c = r.getCentre();
    ColourGradient grad (juce::Colours::white.withAlpha (a0), c.x - dx * len * 0.5f, c.y - dy * len * 0.5f,
                         juce::Colours::white.withAlpha (0.0f), c.x + dx * len * 0.5f, c.y + dy * len * 0.5f, false);
    grad.addColour (p1, juce::Colours::white.withAlpha (a1));
    grad.addColour (p2, juce::Colours::white.withAlpha (0.0f));
    g.setGradientFill (grad);
    g.fillRect (r);
}

void bezel (juce::Graphics& g, Rectangle<float> r)
{
    juce::Path p;
    p.addRoundedRectangle (r, 7.0f);
    ColourGradient bz (Colour (sk().bezel[0]), r.getX(), r.getY(), Colour (sk().bezel[4]), r.getRight(), r.getBottom(), false);
    bz.addColour (0.18, Colour (sk().bezel[1]));
    bz.addColour (0.50, Colour (sk().bezel[2]));
    bz.addColour (0.85, Colour (sk().bezel[3]));
    g.setGradientFill (bz);
    g.fillPath (p);
    g.setColour (Colour (sk().bezelHi));
    g.drawLine (r.getX() + 6.0f, r.getY() + 0.6f, r.getRight() - 6.0f, r.getY() + 0.6f, 1.0f);
    g.setColour (Colour (sk().bezelLo));
    g.drawLine (r.getX() + 6.0f, r.getBottom() - 0.6f, r.getRight() - 6.0f, r.getBottom() - 0.6f, 1.0f);
}

void creamFace (juce::Graphics& g, Rectangle<float> f, float cyRel)
{
    ellipseRadial (g, f, f.getCentreX(), f.getY() + f.getHeight() * cyRel, f.getWidth() * 0.78f, f.getHeight() * 0.95f,
                   { { 0.0, Colour (sk().barFace[0]) }, { 0.55, Colour (sk().barFace[1]) }, { 1.0, Colour (sk().barFace[2]) } });
}

void smokeFace (juce::Graphics& g, Rectangle<float> f)
{
    ellipseRadial (g, f, f.getCentreX(), f.getY() + f.getHeight() * 0.3f, f.getWidth() * 0.8f, f.getHeight(),
                   { { 0.0, Colour (0xFF1D1F22) }, { 0.6, Colour (0xFF121315) }, { 1.0, Colour (0xFF08090A) } });
}

void innerShadow (juce::Graphics& g, Rectangle<float> f, float alpha, float depth)
{
    g.setGradientFill (ColourGradient (juce::Colours::black.withAlpha (alpha), 0.0f, f.getY(), juce::Colours::transparentBlack, 0.0f, f.getY() + depth, false));
    g.fillRect (f.withHeight (depth));
    g.setColour (Colour (0xFF0B0B0C));
    g.drawRect (f, 1.0f);
}

void lip (juce::Graphics& g, Rectangle<float> f, float h)
{
    const auto r = f.withTop (f.getBottom() - h);
    g.setGradientFill (ColourGradient (juce::Colours::transparentBlack, 0.0f, r.getY() - 3.0f, juce::Colours::black.withAlpha (0.35f), 0.0f, r.getY(), false));
    g.fillRect (r.withY (r.getY() - 3.0f).withHeight (3.0f));
    g.setGradientFill (ColourGradient (Colour (sk().barLip[0]), 0.0f, r.getY(), Colour (sk().barLip[1]), 0.0f, r.getBottom(), false));
    g.fillRect (r);
}

// an empty glass light-pipe sunk into the cream face
void tubeTrough (juce::Graphics& g, Rectangle<float> t)
{
    const float rad = t.getHeight() * 0.5f;
    g.setColour (juce::Colours::white.withAlpha (0.7f));
    g.fillRoundedRectangle (t.translated (0.0f, 1.0f), rad);
    ColourGradient tg (Colour (0xFF1A1712), 0.0f, t.getY(), Colour (0xFF3A3428), 0.0f, t.getBottom(), false);
    tg.addColour (0.45, Colour (0xFF2B261D));
    g.setGradientFill (tg);
    g.fillRoundedRectangle (t, rad);
    juce::Graphics::ScopedSaveState ss (g);
    juce::Path p;
    p.addRoundedRectangle (t, rad);
    g.reduceClipRegion (p);
    g.setGradientFill (ColourGradient (juce::Colours::black.withAlpha (0.85f), 0.0f, t.getY(), juce::Colours::transparentBlack, 0.0f, t.getY() + 4.0f, false));
    g.fillRect (t.withHeight (4.0f));
}

void ledTrough (juce::Graphics& g, Rectangle<float> t)
{
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRoundedRectangle (t.translated (0.0f, 0.5f), 2.5f);
    g.setColour (Colour (0xFF050505));
    g.fillRoundedRectangle (t, 2.5f);
}

void lcdBox (juce::Graphics& g, Rectangle<float> r, float highlight)
{
    g.setColour (juce::Colours::white.withAlpha (highlight));
    g.fillRoundedRectangle (r.translated (0.0f, 0.5f), 3.0f);
    g.setGradientFill (ColourGradient (Colour (0xFF050806), 0.0f, r.getY(), Colour (0xFF0D1410), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 3.0f);
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (r.toNearestInt());
    g.setGradientFill (ColourGradient (juce::Colours::black.withAlpha (0.9f), 0.0f, r.getY(), juce::Colours::transparentBlack, 0.0f, r.getY() + 3.0f, false));
    g.fillRect (r.withHeight (3.0f));
}

void glowText (juce::Graphics& g, const juce::String& s, Rectangle<float> r, Colour c, juce::Justification j)
{
    g.setColour (c.withAlpha (0.22f));
    for (auto d : { juce::Point<float> (-0.7f, 0.0f), juce::Point<float> (0.7f, 0.0f), juce::Point<float> (0.0f, -0.7f), juce::Point<float> (0.0f, 0.7f) })
        g.drawText (s, r.translated (d.x, d.y), j, false);
    g.setColour (c);
    g.drawText (s, r, j, false);
}

ColourGradient tubeGradient (const LevelBars::Geo& geo, float alpha)
{
    auto c = [alpha] (juce::uint32 argb) { return Colour (argb).withMultipliedAlpha (alpha); };
    ColourGradient g (c (0xFF2F9A4C), geo.sx, 0.0f, c (0xFFE2392E), geo.sx + geo.sw, 0.0f, false);
    g.addColour (0.66, c (0xFF4FCF62));
    g.addColour (0.82, c (0xFFE8C548));
    g.addColour (0.92, c (0xFFEC8A36));
    return g;
}

// one channel's light in its tube: the peak extent at 45 %, the RMS full with a green glow, the cylindrical shading, the hold bead
void tubeFill (juce::Graphics& g, const LevelBars::Geo& geo, Rectangle<float> trough, float peak, float rms, float hold)
{
    const auto inner = trough.reduced (2.0f);
    const float rad = inner.getHeight() * 0.5f;
    const float wPeak = peak > kBarMinDb ? geo.sw * barPosition (peak) : 0.0f, wRms = rms > kBarMinDb ? geo.sw * barPosition (rms) : 0.0f;
    if (wPeak > 0.0f)
    {
        g.setGradientFill (tubeGradient (geo, 0.45f));
        g.fillRoundedRectangle (inner.withWidth (wPeak), rad);
    }
    if (wRms > 0.0f)
    {
        g.setColour (Colour (110, 230, 120).withAlpha (0.12f));
        g.fillRoundedRectangle (inner.withWidth (wRms).expanded (2.5f), rad + 2.5f);
        g.setColour (Colour (110, 230, 120).withAlpha (0.16f));
        g.fillRoundedRectangle (inner.withWidth (wRms).expanded (1.2f), rad + 1.2f);
        g.setGradientFill (tubeGradient (geo, 1.0f));
        g.fillRoundedRectangle (inner.withWidth (wRms), rad);
    }
    if (const float w = std::max (wPeak, wRms); w > 0.0f)
    {
        ColourGradient cyl (juce::Colours::white.withAlpha (0.62f), 0.0f, inner.getY(), juce::Colours::black.withAlpha (0.28f), 0.0f, inner.getBottom(), false);
        cyl.addColour (0.28, juce::Colours::white.withAlpha (0.18f));
        cyl.addColour (0.50, juce::Colours::white.withAlpha (0.0f));
        g.setGradientFill (cyl);
        g.fillRoundedRectangle (inner.withWidth (w), rad);
    }
    // glass: the top specular strip along the whole tube
    const auto strip = Rectangle<float> (trough.getX() + 7.0f, trough.getY() + 1.5f, trough.getWidth() - 14.0f, std::min (4.5f, trough.getHeight() * 0.25f));
    g.setGradientFill (ColourGradient (juce::Colours::white.withAlpha (0.22f), 0.0f, strip.getY(), juce::Colours::white.withAlpha (0.0f), 0.0f, strip.getBottom(), false));
    g.fillRoundedRectangle (strip, strip.getHeight() * 0.5f);
    if (hold > kBarMinDb)
    {
        const float x = geo.xOf (hold);
        const auto bead = Rectangle<float> (x - 1.5f, trough.getY() + 1.0f, 3.0f, trough.getHeight() - 2.0f);
        const bool clip = hold >= -0.1f;
        g.setColour ((clip ? Colour (255, 90, 74) : Colour (255, 236, 170)).withAlpha (0.35f));
        g.fillRoundedRectangle (bead.expanded (1.5f), 2.5f);
        ColourGradient bg (clip ? Colour (0xFFFF8A7A) : Colour (0xFFFFF6DC), bead.getX(), 0.0f, clip ? Colour (0xFFA8261C) : Colour (0xFFB9A46E), bead.getRight(), 0.0f, false);
        bg.addColour (0.5, clip ? Colour (0xFFE2392E) : Colour (0xFFF3E2B0));
        g.setGradientFill (bg);
        g.fillRoundedRectangle (bead, 1.5f);
    }
}

Rectangle<float> ledSegment (const LevelBars::Geo& geo, Rectangle<float> trough, int k)
{
    const float pitch = geo.sw / 60.0f;
    return { geo.sx + (float) k * pitch + 0.9f, trough.getY() + 2.0f, pitch - 1.8f, trough.getHeight() - 4.0f };
}

void ledUnlit (juce::Graphics& g, Rectangle<float> s, int zone)
{
    const auto z = zoneColour (zone);
    g.setColour (Colour (0xFF0B0B0B));
    g.fillRoundedRectangle (s, 1.5f);
    g.setGradientFill (ColourGradient (z.withAlpha (0.17f), 0.0f, s.getY(), z.withAlpha (0.04f), 0.0f, s.getBottom(), false));
    g.fillRoundedRectangle (s, 1.5f);
}

void ledLit (juce::Graphics& g, Rectangle<float> s, int zone, float i)
{
    const auto z = zoneColour (zone);
    g.setColour (z.withAlpha (0.26f * i));
    g.fillRoundedRectangle (s.expanded (1.6f), 2.6f);
    const float cx = s.getCentreX(), cy = s.getY() + s.getHeight() * 0.28f;
    ColourGradient rg (juce::Colours::white.withAlpha (0.75f * i), cx, cy, Colour ((juce::uint8) (z.getRed() * 0.55f), (juce::uint8) (z.getGreen() * 0.55f), (juce::uint8) (z.getBlue() * 0.55f)).withAlpha (i),
                       cx, cy + s.getHeight() * 0.8f, true);
    rg.addColour (0.55, z.withAlpha (i));
    g.setGradientFill (rg);
    g.fillRoundedRectangle (s, 1.5f);
}
} // namespace

LevelBars::LevelBars()
{
    face_.m = this; bars_.m = this;
    for (juce::Component* c : { (juce::Component*) &face_, (juce::Component*) &bars_ })
    {
        c->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (c);
    }
    face_.setBufferedToImage (true);
    setOpaque (false);
}

void LevelBars::setConfig (bool sendStereo, bool retStereo, float sendDbuAt0dBFS, float returnDbuAt0dBFS, float vuRefDbfs, std::optional<float> outboardMaxDbu, bool dbu)
{
    const bool same = dbu == dbu_ && sendStereo == send_.stereo && retStereo == ret_.stereo && std::fabs (sendDbuAt0dBFS - send_.dbuAt0) < 1e-3f && std::fabs (returnDbuAt0dBFS - ret_.dbuAt0) < 1e-3f
                      && std::fabs (vuRefDbfs - vuRef_) < 1e-3f && outboardMaxDbu.has_value() == outboardMaxDbu_.has_value()
                      && (! outboardMaxDbu_ || std::fabs (*outboardMaxDbu_ - *outboardMaxDbu) < 1e-3f);
    if (same) return;
    send_.stereo = sendStereo; ret_.stereo = retStereo; send_.dbuAt0 = sendDbuAt0dBFS; ret_.dbuAt0 = returnDbuAt0dBFS; vuRef_ = vuRefDbfs; outboardMaxDbu_ = outboardMaxDbu; dbu_ = dbu;
    face_.repaint();
    bars_.repaint();
}

void LevelBars::setStyle (MeterView v)
{
    if (! isBarView (v) || v == style_) return;
    style_ = v;
    face_.repaint();
    bars_.repaint();
}

void LevelBars::setReadings (const Reading& send, const Reading& ret)
{
    send_.r = send;
    ret_.r = ret;
}

void LevelBars::advance (double dt)
{
    const double step = std::min (dt, 0.25);   // a stalled message thread must not drain the bars in one jump
    bool moved = false;
    int k = 0;
    for (Group* g : { &send_, &ret_ })
        for (int c = 0; c < 2; ++c, ++k)
        {
            const float v = g->fall[c].advance (step, g->stereo || c == 0 ? g->r.peakDb[c] : kBarFloorDb);
            if (std::fabs (v - drawn_[k]) > 0.05f) moved = true;
        }
    if (moved) bars_.repaint();
}

void LevelBars::settle()
{
    for (Group* g : { &send_, &ret_ })
        for (int c = 0; c < 2; ++c) g->fall[c].shownDb = std::max (kBarFloorDb, g->stereo || c == 0 ? g->r.peakDb[c] : kBarFloorDb);
    bars_.repaint();
}

void LevelBars::resized()
{
    face_.setBounds (getLocalBounds());
    bars_.setBounds (getLocalBounds());
}

juce::String LevelBars::holdText (const Group& grp, int ch) const
{
    const float hold = grp.r.peakHoldDb[ch];
    if (hold <= ViewModel::kFloorDb) return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
    return juce::String (ViewModel::signedText (dbu_ ? hold + grp.dbuAt0 : hold, 1));
}

void LevelBars::paintGroupFace (juce::Graphics& g, const Geo& geo, bool send) const
{
    const Group& grp = send ? send_ : ret_;
    const bool twin = style_ == MeterView::Twin, led = style_ == MeterView::Led;
    const FacePalette pal = led ? kSmoke : creamPalette();
    const float fs = twin ? 6.5f : 7.5f;   // scale label size

    // legends
    if (twin)
    {
        g.setColour (Colour (sk().barTitle));
        g.setFont (uiFont (17.0f, true, 0.06f));
        g.drawText (send ? "SEND" : "RETURN", Rectangle<float> (15.0f, 9.0f, 200.0f, 20.0f), juce::Justification::centredLeft, false);
    }
    else
    {
        g.setColour (led ? Colour (0xFFE6DCC0) : pal.ink);
        g.setFont (uiFont (9.5f, true, 0.22f));
        g.drawText (send ? "SEND" : "RETURN", Rectangle<float> (geo.labelX, geo.barY, 60.0f, geo.barStep + geo.barH), juce::Justification::centredLeft, false);
    }
    g.setColour (pal.dim);
    g.setFont (uiFont (fs, false, 0.12f));
    g.drawText ("dBu", Rectangle<float> (geo.labelX, geo.dbuTextY, 30.0f, 10.0f), juce::Justification::centredLeft, false);
    g.drawText ("dBFS", Rectangle<float> (geo.labelX, geo.fsTextY, 30.0f, 10.0f), juce::Justification::centredLeft, false);

    // the red rule from −9 to 0 dBFS and the dBu baseline (cream faces)
    if (! led)
    {
        g.setColour (pal.ref);
        g.fillRoundedRectangle (Rectangle<float> (geo.xOf (-9.0f), geo.ruleY, geo.xOf (0.0f) - geo.xOf (-9.0f), 2.5f), 1.0f);
        g.setColour (pal.ink.withAlpha (0.55f));
        g.fillRect (Rectangle<float> (geo.sx, geo.ruleY + 1.5f, geo.sw, 0.75f));
    }
    // dBu scale: labels above, ticks down to the bars
    for (const auto& t : dbuTicks (grp.dbuAt0))
    {
        const float x = geo.xOf (t.dbfs);
        g.setColour (t.reference ? pal.ref : pal.ink);
        g.fillRect (Rectangle<float> (x - (t.reference ? 0.75f : 0.5f), geo.tickY0, t.reference ? 1.5f : 1.0f, geo.tickY1 - geo.tickY0));
        g.setFont (uiFont (t.reference ? fs + 1.0f : fs, t.reference, 0.0f));
        g.drawText (juce::String (ViewModel::signedText (t.dbu, 0)), Rectangle<float> (x - 15.0f, geo.dbuTextY, 30.0f, 10.0f), juce::Justification::centred, false);
    }
    // outboard maximum input on the SEND dBu scale
    if (send && outboardMaxDbu_)
    {
        const float x = geo.xOf (std::clamp (*outboardMaxDbu_ - grp.dbuAt0, kBarMinDb, kBarMaxDb)), y = geo.tickY1;
        juce::Path tri;
        tri.addTriangle (x, y, x - 3.5f, y - 6.0f, x + 3.5f, y - 6.0f);
        g.setColour (pal.outboard);
        g.fillPath (tri);
    }

    // empty troughs / segments
    for (int c = 0; c < (grp.stereo ? 2 : 1); ++c)
    {
        const auto t = geo.trough (grp.stereo, c);
        if (led)
        {
            ledTrough (g, t);
            for (int k = 0; k < 60; ++k) ledUnlit (g, ledSegment (geo, t, k), barZone (kBarMinDb + (float) k + 0.5f));
        }
        else tubeTrough (g, t);
        if (grp.stereo)
        {
            g.setColour (twin && c == 1 ? pal.ref : pal.chan);
            g.setFont (uiFont (7.0f, true, 0.0f));
            g.drawText (c == 0 ? "L" : "R", Rectangle<float> (geo.sx - 16.0f, t.getY(), 10.0f, t.getHeight()), juce::Justification::centred, false);
        }
    }

    // dBFS scale below: ticks up to the bars, labels under them (TWIN: every 12 dB, 6 dB minor ticks)
    for (float v : dbfsTicks())
    {
        const float x = geo.xOf (v);
        const bool zero = v >= -0.01f, major = ! twin || std::fmod (-v, 12.0f) < 0.01f;
        g.setColour (zero ? pal.zero : major ? pal.fsLabel : pal.dim);
        g.fillRect (Rectangle<float> (x - 0.5f, geo.fsTickY, 1.0f, major ? 4.0f : 2.5f));
        if (! major) continue;
        g.setFont (uiFont (fs, zero, 0.0f));
        g.drawText (juce::String (ViewModel::signedText (v, 0)), Rectangle<float> (x - 15.0f, geo.fsTextY, 30.0f, 10.0f), juce::Justification::centred, false);
    }

    // PEAK HOLD window (TUBE / LED; TWIN carries it in its caption)
    if (! twin)
    {
        g.setColour (pal.dim);
        g.setFont (uiFont (6.5f, true, 0.24f));
        g.drawText ("PEAK HOLD", Rectangle<float> (kReadX + 2.0f, geo.top + 4.0f, kReadW, 10.0f), juce::Justification::centredLeft, false);
        lcdBox (g, Rectangle<float> (kReadX, geo.top + 16.0f, kReadW, 46.0f), led ? 0.1f : 0.75f);
    }
}

void LevelBars::paintGroupBars (juce::Graphics& g, const Geo& geo, bool send) const
{
    const Group& grp = send ? send_ : ret_;
    const bool twin = style_ == MeterView::Twin, led = style_ == MeterView::Led;
    // the 0 VU reference: +4 dBu at the group's converter in dBu, the SETUP reference in dBFS
    const float refX = geo.xOf (dbu_ ? 4.0f - grp.dbuAt0 : vuRef_);
    bool clip = false;
    for (int c = 0; c < (grp.stereo ? 2 : 1); ++c)
    {
        const auto t = geo.trough (grp.stereo, c);
        const float peak = grp.fall[c].shownDb, rms = grp.r.rmsDb[c], hold = grp.r.peakHoldDb[c];
        clip = clip || hold >= -0.1f;
        if (led)
            for (int k = 0; k < 60; ++k)
            {
                const float lo = kBarMinDb + (float) k;
                if (const float i = ledIntensity (lo, rms, peak, hold); i > 0.0f) ledLit (g, ledSegment (geo, t, k), barZone (lo + 0.5f), i);
            }
        else tubeFill (g, geo, t, peak, rms, hold);
    }
    g.setColour ((led ? Colour (0xFFFFCF6A) : Colour (sk().barRef)).withAlpha (led ? 0.45f : 0.5f));
    g.fillRect (Rectangle<float> (refX - 0.75f, geo.tickY1, 1.5f, geo.barsBottom (grp.stereo) - geo.tickY1 + 1.0f));

    const juce::String unit = dbu_ ? " dBu" : " dBFS";
    if (twin)
    {
        // clip lamp: lit while a channel's held peak is at full scale
        const auto lamp = Rectangle<float> (kTwinFaceW - 30.0f, 12.0f, 15.0f, 15.0f);
        if (clip)
        {
            g.setColour (Colour (255, 90, 74).withAlpha (0.35f));
            g.fillEllipse (lamp.expanded (3.0f));
        }
        ColourGradient lg (clip ? Colour (0xFFFFB0A0) : Colour (0xFF8A8A8A), lamp.getX() + lamp.getWidth() * 0.4f, lamp.getY() + lamp.getHeight() * 0.35f,
                           clip ? Colour (0xFF7A1410) : Colour (0xFF1A1A1A), lamp.getRight(), lamp.getBottom(), true);
        lg.addColour (0.55, clip ? Colour (0xFFE2392E) : Colour (0xFF3A3A3A));
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.fillEllipse (lamp.translated (0.0f, 0.6f));
        g.setGradientFill (lg);
        g.fillEllipse (lamp);
        // PEAK HOLD caption
        juce::String cap = "PEAK HOLD   ";
        cap += grp.stereo ? "L " + holdText (grp, 0) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 R ")) + holdText (grp, 1) : holdText (grp, 0);
        g.setColour (Colour (sk().barCaption));
        g.setFont (uiFont (7.0f, false, 0.16f));
        g.drawText (cap + unit, Rectangle<float> (0.0f, 146.0f, kTwinFaceW, 10.0f), juce::Justification::centred, false);
        return;
    }
    // LCD readouts: the held peak per channel
    const auto box = Rectangle<float> (kReadX, geo.top + 16.0f, kReadW, 46.0f);
    g.setFont (monoFont (10.5f));
    for (int c = 0; c < (grp.stereo ? 2 : 1); ++c)
    {
        const auto row = grp.stereo ? Rectangle<float> (box.getX() + 8.0f, box.getY() + 6.0f + (float) c * 17.0f, box.getWidth() - 10.0f, 16.0f)
                                    : Rectangle<float> (box.getX() + 8.0f, box.getCentreY() - 8.0f, box.getWidth() - 10.0f, 16.0f);
        const bool hot = grp.r.peakHoldDb[c] >= -0.1f;
        const juce::String s = grp.r.peakHoldDb[c] <= ViewModel::kFloorDb ? holdText (grp, c) : holdText (grp, c) + unit;
        glowText (g, (grp.stereo ? (c == 0 ? "L " : "R ") : "") + s, row, hot ? kLcdRed : kLcdGreen, juce::Justification::centredLeft);
    }
}

void LevelBars::paintFace (juce::Graphics& g) const
{
    if (style_ == MeterView::Twin)
    {
        for (bool send : { true, false })
        {
            const float x0 = twinX (send);
            bezel (g, Rectangle<float> (x0, 0.0f, (float) kTwinW, (float) kH));
            juce::Graphics::ScopedSaveState ss (g);
            g.addTransform (juce::AffineTransform::translation (x0 + (float) kBezel, (float) kBezel));
            const auto f = Rectangle<float> (0.0f, 0.0f, kTwinFaceW, kFaceH);
            g.reduceClipRegion (f.toNearestInt());
            creamFace (g, f, 0.36f);
            innerShadow (g, f, 0.55f, 5.0f);
            paintGroupFace (g, twinGeo(), send);
            g.setColour (creamPalette().ink);
            g.setFont (uiFont (7.5f, true, 0.3f));
            g.drawText ("NOVAINSERT", Rectangle<float> (0.0f, 133.0f, kTwinFaceW, 10.0f), juce::Justification::centred, false);
            lip (g, f, 19.0f);
        }
        return;
    }
    const bool led = style_ == MeterView::Led;
    bezel (g, Rectangle<float> (0.0f, 0.0f, (float) kW, (float) kH));
    juce::Graphics::ScopedSaveState ss (g);
    g.addTransform (juce::AffineTransform::translation ((float) kBezel, (float) kBezel));
    const auto f = Rectangle<float> (0.0f, 0.0f, kFaceW, kFaceH);
    g.reduceClipRegion (f.toNearestInt());
    if (led) smokeFace (g, f); else creamFace (g, f, 0.38f);
    innerShadow (g, f, led ? 0.9f : 0.55f, led ? 6.0f : 5.0f);
    paintGroupFace (g, stackGeo (true), true);
    paintGroupFace (g, stackGeo (false), false);
    if (! led) lip (g, f, 13.0f);
    g.setColour (led ? Colour (0xFF5A564C) : Colour (sk().barBrand));
    g.setFont (uiFont (6.0f, true, 0.34f));
    g.drawText ("NOVAINSERT", Rectangle<float> (kReadX, kFaceH - 11.0f, kReadW, 9.0f), juce::Justification::centredRight, false);
}

void LevelBars::paintBars (juce::Graphics& g) const
{
    if (style_ == MeterView::Twin)
    {
        for (bool send : { true, false })
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.addTransform (juce::AffineTransform::translation (twinX (send) + (float) kBezel, (float) kBezel));
            const auto f = Rectangle<float> (0.0f, 0.0f, kTwinFaceW, kFaceH);
            g.reduceClipRegion (f.toNearestInt());
            paintGroupBars (g, twinGeo(), send);
            sheen (g, f, 160.0f, 0.34f, 0.06f, 0.34, 0.48);
        }
        return;
    }
    juce::Graphics::ScopedSaveState ss (g);
    g.addTransform (juce::AffineTransform::translation ((float) kBezel, (float) kBezel));
    const auto f = Rectangle<float> (0.0f, 0.0f, kFaceW, kFaceH);
    g.reduceClipRegion (f.toNearestInt());
    paintGroupBars (g, stackGeo (true), true);
    paintGroupBars (g, stackGeo (false), false);
    if (style_ == MeterView::Led) sheen (g, f, 165.0f, 0.13f, 0.035f, 0.30, 0.44);
    else sheen (g, f, 160.0f, 0.32f, 0.06f, 0.32, 0.46);
}

void LevelBars::Face::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::scale ((float) getWidth() / (float) LevelBars::kW));
    m->paintFace (g);
}

void LevelBars::Bars::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::scale ((float) getWidth() / (float) LevelBars::kW));
    m->paintBars (g);
    int k = 0;
    for (const Group* grp : { &m->send_, &m->ret_ })
        for (int c = 0; c < 2; ++c, ++k) m->drawn_[k] = grp->fall[c].shownDb;
}
} // namespace ni::gui
