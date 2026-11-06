#include "gui/vu_meter.hpp"
#include <cmath>
#include "gui/controls.hpp"

namespace ni::gui
{
using juce::Colour;
using juce::ColourGradient;
using juce::Point;

namespace
{
constexpr float kCx = VuMeter::kFaceW * 0.5f, kCy = VuMeter::kFaceH + 95.0f, kR = 225.0f, kRS = kR - 12.0f, kRP = kRS - 10.0f;
Colour vuInk() { return Colour (sk().vuInk); }
Colour vuRed() { return Colour (sk().vuRed); }
Colour vuNeedleL() { return Colour (sk().needleL); }
Colour vuNeedleR() { return Colour (sk().needleR); }

Point<float> pt (float deg, float r)
{
    const float a = juce::degreesToRadians (deg);
    return { kCx + r * std::sin (a), kCy - r * std::cos (a) };
}
float ang (float vu) { return VuMeter::angleDeg (vuToPosition (vu)); }

void arc (juce::Graphics& g, float a0, float a1, float r, float w, Colour c)
{
    juce::Path p;
    p.addCentredArc (kCx, kCy, r, r, 0.0f, juce::degreesToRadians (a0), juce::degreesToRadians (a1), true);
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
}

void tick (juce::Graphics& g, float vu, float r0, float r1, float w, Colour c)
{
    g.setColour (c);
    g.drawLine ({ pt (ang (vu), r0), pt (ang (vu), r1) }, w);
}

// text centred on a point (mockup SVG text uses baseline y; callers pass the visual centre)
void centred (juce::Graphics& g, const juce::String& s, Point<float> c, float w = 60.0f, float h = 24.0f)
{
    g.drawText (s, juce::Rectangle<float> (c.x - w * 0.5f, c.y - h * 0.5f, w, h), juce::Justification::centred, false);
}

juce::String subLabel (float v)
{
    if (std::fabs (v) < 0.05f) return "0";
    const bool whole = std::fabs (v - std::round (v)) < 0.05f;
    const juce::String digits = juce::String (std::fabs (v), whole ? 0 : 1);
    return (v < 0 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) : juce::String()) + digits;
}
} // namespace

float VuMeter::angleDeg (float position) noexcept
{
    return -33.0f + 66.0f * std::clamp (position, kPinLow, kPinHigh);
}

VuMeter::VuMeter()
{
    face_.m = this; needleLayer_.m = this; glass_.m = this;
    for (juce::Component* c : { (juce::Component*) &face_, (juce::Component*) &needleLayer_, (juce::Component*) &glass_ })
    {
        c->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (c);
    }
    face_.setBufferedToImage (true);
    glass_.setBufferedToImage (true);
    setOpaque (false);
}

void VuMeter::setConfig (bool stereo, float subScaleOffsetDb, juce::String label, std::optional<float> markerVu)
{
    const bool same = stereo == stereo_ && std::fabs (subScaleOffsetDb - offset_) < 1e-3f && label == label_
                      && marker_.has_value() == markerVu.has_value() && (! marker_ || std::fabs (*marker_ - *markerVu) < 1e-3f);
    if (same) return;
    stereo_ = stereo; offset_ = subScaleOffsetDb; label_ = std::move (label); marker_ = markerVu;
    face_.repaint();
    needleLayer_.repaint();
}

void VuMeter::setDrive (float l, float r, bool peak)
{
    drive_[0] = std::isfinite (l) ? l : 0.0f;
    drive_[1] = std::isfinite (r) ? r : 0.0f;
    if (peak != peak_) { peak_ = peak; glass_.repaint(); }
}

void VuMeter::advance (double dt)
{
    const double step = std::min (dt, 0.25);   // a stalled message thread must not fling the needle
    needles_[0].advance (step, drive_[0]);
    needles_[1].advance (step, stereo_ ? drive_[1] : 0.0f);
    bool moved = false;
    for (int c = 0; c < 2; ++c)
        if (std::fabs (needles_[c].position() - drawn_[c]) > 2e-4f) moved = true;
    if (moved) needleLayer_.repaint();
}

void VuMeter::settle (float l, float r)
{
    needles_[0].reset (l * vuToPosition (0.0f));
    needles_[1].reset (r * vuToPosition (0.0f));
    drive_[0] = l; drive_[1] = r;
    needleLayer_.repaint();
}

void VuMeter::resized()
{
    const float s = (float) getWidth() / (kFaceW + 2 * kBezel);
    const auto face = juce::Rectangle<float> (kBezel * s, kBezel * s, kFaceW * s, kFaceH * s).toNearestInt();
    for (juce::Component* c : { (juce::Component*) &face_, (juce::Component*) &needleLayer_, (juce::Component*) &glass_ })
        c->setBounds (face);
}

void VuMeter::paint (juce::Graphics& g)
{
    // metallic bezel
    auto r = getLocalBounds().toFloat();
    const float s = r.getWidth() / (kFaceW + 2 * kBezel);
    juce::Path p;
    p.addRoundedRectangle (r, 7.0f * s);
    juce::DropShadow (juce::Colours::black.withAlpha (0.8f), (int) (6 * s), { 0, (int) (3 * s) }).drawForPath (g, p);
    ColourGradient bz (Colour (sk().bezel[0]), r.getX(), r.getY(), Colour (sk().bezel[4]), r.getRight(), r.getBottom(), false);
    bz.addColour (0.18, Colour (sk().bezel[1]));
    bz.addColour (0.50, Colour (sk().bezel[2]));
    bz.addColour (0.85, Colour (sk().bezel[3]));
    g.setGradientFill (bz);
    g.fillPath (p);
    g.setColour (Colour (sk().bezelHi));
    g.drawLine (r.getX() + 6 * s, r.getY() + 0.6f, r.getRight() - 6 * s, r.getY() + 0.6f, 1.0f);
    g.setColour (Colour (sk().bezelLo));
    g.drawLine (r.getX() + 6 * s, r.getBottom() - 0.6f, r.getRight() - 6 * s, r.getBottom() - 0.6f, 1.0f);
}

void VuMeter::Face::paint (juce::Graphics& g)
{
    const float s = (float) getWidth() / kFaceW;
    g.addTransform (juce::AffineTransform::scale (s));
    const float W = kFaceW, H = kFaceH;

    // cream face with a warm glow from below (elliptical: the mockup's bbox-relative radial gradient)
    ColourGradient fg (Colour (sk().vuFace[0]), kCx, H * 1.08f, Colour (sk().vuFace[3]), kCx + W * 0.95f, H * 1.08f, true);
    fg.addColour (0.35, Colour (sk().vuFace[1]));
    fg.addColour (0.70, Colour (sk().vuFace[2]));
    g.setFillType (juce::FillType (fg).transformed (juce::AffineTransform::scale (1.0f, H / W, kCx, H * 1.08f)));
    g.fillRect (0.0f, 0.0f, W, H);

    // main scale
    arc (g, ang (-20.0f), ang (0.0f), kRS, 1.6f, vuInk());
    arc (g, ang (0.0f), ang (3.0f), kRS + 3.0f, 6.0f, vuRed());
    for (float vu : { -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f })
    {
        const Colour c = vu > 0.0f ? vuRed() : vuInk();
        tick (g, vu, kRS, kRS + 12.0f, 1.6f, c);
        g.setColour (c);
        g.setFont (scaleFont (17.0f));
        centred (g, juce::String ((int) std::fabs (vu)), pt (ang (vu), kRS + 26.0f));
    }
    for (float vu : { -18.0f, -16.0f, -14.0f, -12.0f, -9.0f, -8.0f, -6.0f, -4.0f, -2.5f, -1.5f, -0.5f, 0.5f, 1.5f, 2.5f })
        tick (g, vu, kRS, kRS + 6.0f, 1.0f, vu > 0.0f ? vuRed() : vuInk());
    g.setColour (vuInk());
    g.setFont (uiFont (20.0f));
    centred (g, juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")), pt (ang (-21.5f), kRS + 4.0f).translated (-6.0f, 0.0f));
    g.setColour (vuRed());
    g.setFont (uiFont (18.0f, true));
    centred (g, "+", pt (ang (3.6f), kRS + 4.0f).translated (4.0f, 10.0f));

    // dBFS sub-scale: label = VU + offset
    arc (g, ang (-20.0f), ang (3.0f), kRP, 1.0f, vuInk());
    for (float vu : { -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -1.0f, 0.0f, 1.0f, 3.0f })
        tick (g, vu, kRP, kRP - 5.0f, 1.0f, vuInk());
    for (float vu : { -20.0f, -10.0f, -5.0f, 0.0f, 3.0f })
    {
        const float v = vu + m->offset_;
        const bool zero = std::fabs (v) < 0.05f;
        g.setColour (zero ? vuRed() : vuInk());
        g.setFont (uiFont (10.5f, zero));
        centred (g, subLabel (v), pt (ang (vu), kRP - 14.0f), 44.0f, 13.0f);
    }
    g.setColour (vuInk());
    g.setFont (uiFont (8.0f, false, 0.12f));
    centred (g, "dBFS", pt (ang (-20.0f), kRP - 14.0f).translated (-4.0f, 14.0f), 40.0f, 10.0f);

    // outboard limit marker
    if (m->marker_)
    {
        const float a = ang (std::clamp (*m->marker_, -20.0f, 3.0f));
        juce::Path tri;
        tri.addTriangle (pt (a, kRS - 1.0f), pt (a - 1.6f, kRS - 9.0f), pt (a + 1.6f, kRS - 9.0f));
        g.setColour (Colour (sk().vuMarker));
        g.fillPath (tri);
    }

    // legends
    g.setColour (vuInk());
    g.setFont (scaleFont (19.0f, true));
    g.drawText ("VU", juce::Rectangle<float> (26.0f, 16.0f, 60.0f, 24.0f), juce::Justification::left, false);
    g.setColour (Colour (sk().vuInk2));
    g.setFont (uiFont (10.0f, true, 0.3f));
    centred (g, "NOVAINSERT", { kCx, 118.0f }, 200.0f, 14.0f);
    if (m->stereo_)
    {
        g.setFont (uiFont (9.0f, true, 0.2f));
        g.setColour (vuNeedleL());
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f L")), juce::Rectangle<float> (kCx - 34.0f, 127.0f, 30.0f, 12.0f), juce::Justification::centredRight, false);
        g.setColour (vuNeedleR());
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f R")), juce::Rectangle<float> (kCx + 4.0f, 127.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
    }
    g.setColour (Colour (sk().vuInk3));
    g.setFont (uiFont (9.5f, false, 0.18f));
    centred (g, m->label_, { kCx, 147.0f }, 300.0f, 13.0f);
}

void VuMeter::Needles::paint (juce::Graphics& g)
{
    const float s = (float) getWidth() / kFaceW;
    g.addTransform (juce::AffineTransform::scale (s));
    const int n = m->stereo_ ? 2 : 1;
    for (int c = n - 1; c >= 0; --c)   // R below, L (black) on top
    {
        const float a = angleDeg (m->needles_[c].position());
        const Point<float> pivot { kCx, kCy }, tip = pt (a, kR - 4.0f);
        // soft shadow: the needle offset towards the lower right, three widening translucent strokes
        const Point<float> off { 4.0f, 3.0f };
        for (auto [w, al] : { std::pair { 6.0f, 0.06f }, std::pair { 4.0f, 0.09f }, std::pair { 2.2f, 0.14f } })
        {
            g.setColour (juce::Colours::black.withAlpha (al));
            g.drawLine ({ pivot + off, pt (a, kR - 8.0f) + off }, w);
        }
        g.setColour (c == 0 ? vuNeedleL() : vuNeedleR());
        g.drawLine ({ pivot, tip }, 1.8f);
        m->drawn_[c] = m->needles_[c].position();
    }
}

void VuMeter::Glass::paint (juce::Graphics& g)
{
    const float s = (float) getWidth() / kFaceW;
    g.addTransform (juce::AffineTransform::scale (s));
    const float W = kFaceW, H = kFaceH;

    // domed peak LED
    const Point<float> lc { W - 26.0f, 26.0f };
    if (m->peak_)
    {
        g.setColour (Colour (0x55FF3020));
        g.fillEllipse (lc.x - 12.0f, lc.y - 12.0f, 24.0f, 24.0f);
    }
    g.setColour (Colour (0xFF111111));
    g.fillEllipse (lc.x - 8.0f, lc.y - 8.0f, 16.0f, 16.0f);
    ColourGradient led (m->peak_ ? Colour (0xFFFFB3A8) : Colour (0xFF6A6A6A), lc.x - 1.3f, lc.y - 2.0f,
                        m->peak_ ? Colour (0xFF6A0A06) : Colour (0xFF0B0B0B), lc.x + 6.5f, lc.y + 6.5f, true);
    if (m->peak_) led.addColour (0.5, Colour (0xFFE0261B));
    g.setGradientFill (led);
    g.fillEllipse (lc.x - 6.5f, lc.y - 6.5f, 13.0f, 13.0f);

    // straight bottom lip: the needles vanish under it, no hub
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (0.0f, H - 27.0f, W, 3.0f);
    ColourGradient lip (Colour (sk().vuLip[0]), 0.0f, H - 24.0f, Colour (sk().vuLip[2]), 0.0f, H, false);
    lip.addColour (0.12, Colour (sk().vuLip[1]));
    g.setGradientFill (lip);
    g.fillRect (0.0f, H - 24.0f, W, 24.0f);

    // vignette
    ColourGradient vg (juce::Colours::transparentBlack, kCx, H * 0.6f, juce::Colours::black.withAlpha (0.35f), kCx + W * 0.75f, H * 0.6f, true);
    vg.addColour (0.6, juce::Colours::transparentBlack);
    g.setFillType (juce::FillType (vg).transformed (juce::AffineTransform::scale (1.0f, H / W, kCx, H * 0.6f)));
    g.fillRect (0.0f, 0.0f, W, H);

    // inner well shadows
    g.setGradientFill (ColourGradient (juce::Colours::black.withAlpha (0.75f), 0.0f, 0.0f, juce::Colours::transparentBlack, 0.0f, 16.0f, false));
    g.fillRect (0.0f, 0.0f, W, 16.0f);
    g.setGradientFill (ColourGradient (juce::Colours::black.withAlpha (0.55f), 0.0f, 0.0f, juce::Colours::transparentBlack, 12.0f, 0.0f, false));
    g.fillRect (0.0f, 0.0f, 12.0f, H);
    g.setGradientFill (ColourGradient (juce::Colours::black.withAlpha (0.55f), W, 0.0f, juce::Colours::transparentBlack, W - 12.0f, 0.0f, false));
    g.fillRect (W - 12.0f, 0.0f, 12.0f, H);

    // glass: diagonal sheen + a curved reflection across the top
    ColourGradient gl (juce::Colours::white.withAlpha (0.22f), 0.0f, 0.0f, juce::Colours::transparentWhite, W, H, false);
    gl.addColour (0.4, juce::Colours::white.withAlpha (0.04f));
    g.setGradientFill (gl);
    g.fillRect (0.0f, 0.0f, W, H);
    ColourGradient refl (juce::Colours::white.withAlpha (0.16f), 0.0f, 0.0f, juce::Colours::white.withAlpha (0.02f), 0.0f, 66.0f, false);
    g.setGradientFill (refl);
    g.fillEllipse (-0.2f * W, -0.6f * H, 1.4f * W, 0.95f * H);

    g.setColour (Colour (0xFF050505));
    g.drawRect (0.0f, 0.0f, W, H, 2.0f);
}
} // namespace ni::gui
