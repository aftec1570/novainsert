#include "gui/controls.hpp"
#include "gui/view_model.hpp"
#include <cmath>
#include <numeric>

namespace ni::gui
{
using juce::Colour;
using juce::ColourGradient;
using juce::Rectangle;

namespace
{
Skin gSkin = Skin::Dark;
}

const Palette& sk() noexcept { return palette (gSkin); }
Skin currentSkin() noexcept { return gSkin; }

void setSkin (Skin s)
{
    gSkin = s;
    const Palette& p = palette (s);
    ink::cream = Colour (p.cream); ink::label = Colour (p.label); ink::dim = Colour (p.dim); ink::amber = Colour (p.amber);
    ink::lcdText = Colour (p.lcdText); ink::lcdDim = Colour (p.lcdDim); ink::red = Colour (p.red); ink::warn = Colour (p.warn);
}

juce::Font uiFont (float size, bool bold, float tracking)
{
    // Not cached on purpose: a static juce::Font outlives JUCE's typeface cache and aborts the host at exit ("mutex lock failed",
    // pluginval 2026-11-04); JUCE's own TypefaceCache already makes this a lookup, not a typeface load.
    return juce::Font (juce::FontOptions ("Helvetica Neue", size, bold ? juce::Font::bold : juce::Font::plain).withKerningFactor (tracking));
}
juce::Font monoFont (float size) { return juce::Font (juce::FontOptions ("Menlo", size, juce::Font::bold)); }
juce::Font scaleFont (float size, bool bold) { return juce::Font (juce::FontOptions ("Futura", size, bold ? juce::Font::bold : juce::Font::plain)); }

namespace
{
void screw (juce::Graphics& g, juce::Point<float> c)
{
    const float r = 6.0f;
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.fillEllipse (c.x - r, c.y - r + 1.0f, 2 * r, 2 * r);
    ColourGradient head (Colour (sk().screwHi), c.x - 2.0f, c.y - 2.5f, Colour (sk().screwLo), c.x + r, c.y + r, true);
    head.addColour (0.45, Colour (sk().screwMid));
    g.setGradientFill (head);
    g.fillEllipse (c.x - r, c.y - r, 2 * r, 2 * r);
    juce::Path slot;
    slot.addRoundedRectangle (-4.0f, -1.0f, 8.0f, 2.0f, 1.0f);
    g.setColour (Colour (sk().screwSlot));
    g.fillPath (slot, juce::AffineTransform::rotation (juce::degreesToRadians (35.0f)).translated (c));
}

void insetShadow (juce::Graphics& g, Rectangle<float> r, float corner, float alpha)
{
    g.setColour (juce::Colours::black.withAlpha (alpha));
    g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    ColourGradient top (juce::Colours::black.withAlpha (alpha * 0.8f), 0.0f, r.getY(), juce::Colours::transparentBlack, 0.0f, r.getY() + 5.0f, false);
    g.setGradientFill (top);
    g.fillRoundedRectangle (r.withHeight (5.0f), corner);
}
} // namespace

void drawHousing (juce::Graphics& g, Rectangle<float> r)
{
    juce::DropShadow (juce::Colours::black.withAlpha (0.8f), 30, { 0, 16 }).drawForRectangle (g, r.toNearestInt());
    ColourGradient body (Colour (sk().housingTop), 0.0f, r.getY(), Colour (sk().housingBot), 0.0f, r.getBottom(), false);
    body.addColour (0.45, Colour (sk().housingMid));
    g.setGradientFill (body);
    g.fillRoundedRectangle (r, 10.0f);
    {   // brushed texture: two hairline hatches, clipped to the housing
        juce::Graphics::ScopedSaveState s (g);
        juce::Path clip;
        clip.addRoundedRectangle (r, 10.0f);
        g.reduceClipRegion (clip);
        g.setColour (juce::Colours::white.withAlpha (0.025f));
        for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += 3.0f)
            g.drawLine (x, r.getBottom(), x + r.getHeight(), r.getY(), 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.07f));
        for (float x = r.getX(); x < r.getRight() + r.getHeight(); x += 3.0f)
            g.drawLine (x, r.getBottom(), x - r.getHeight(), r.getY(), 1.0f);
    }
    g.setColour (Colour (sk().housingEdgeHi));
    g.drawLine (r.getX() + 8.0f, r.getY() + 1.0f, r.getRight() - 8.0f, r.getY() + 1.0f, 2.0f);
    g.setColour (Colour (sk().housingEdgeLo));
    g.drawLine (r.getX() + 8.0f, r.getBottom() - 1.5f, r.getRight() - 8.0f, r.getBottom() - 1.5f, 3.0f);
    for (auto p : { juce::Point<float> { r.getX() + 14.0f, r.getY() + 14.0f }, { r.getRight() - 14.0f, r.getY() + 14.0f },
                    { r.getX() + 14.0f, r.getBottom() - 14.0f }, { r.getRight() - 14.0f, r.getBottom() - 14.0f } })
        screw (g, p);
}

void drawStrip (juce::Graphics& g, Rectangle<float> r)
{
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 6, { 0, 3 }).drawForRectangle (g, r.toNearestInt());
    g.setGradientFill (ColourGradient (Colour (sk().stripTop), 0.0f, r.getY(), Colour (sk().stripBot), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (Colour (sk().stripEdge));
    g.drawLine (r.getX() + 6.0f, r.getY() + 0.5f, r.getRight() - 6.0f, r.getY() + 0.5f, 1.0f);
}

void drawDivider (juce::Graphics& g, Rectangle<float> r)
{
    ColourGradient d (Colour (sk().dividerEdge), r.getX(), 0.0f, Colour (sk().dividerEdge), r.getRight(), 0.0f, false);
    d.addColour (0.5, Colour (sk().dividerMid));
    g.setGradientFill (d);
    g.fillRoundedRectangle (r, 4.0f);
    insetShadow (g, r, 4.0f, 0.9f);
}

NovaLookAndFeel::NovaLookAndFeel()
{
    applySkin();
}

void NovaLookAndFeel::applySkin()
{
    setColour (juce::ComboBox::textColourId, ink::lcdText);
    setColour (juce::PopupMenu::backgroundColourId, Colour (sk().popupBg));
    setColour (juce::PopupMenu::textColourId, ink::cream);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colour (sk().popupHi));
    setColour (juce::PopupMenu::highlightedTextColourId, Colour (sk().popupHiText));
    setColour (juce::Slider::textBoxTextColourId, ink::cream);
    setColour (juce::Slider::textBoxBackgroundColourId, Colour (sk().textBoxBg));
    setColour (juce::Slider::textBoxOutlineColourId, Colour (sk().textBoxOutline));
    setColour (juce::TextButton::buttonColourId, Colour (sk().buttonBase));
    setColour (juce::TextButton::textColourOffId, ink::cream);
    setColour (juce::ToggleButton::textColourId, ink::cream);
    setColour (juce::ToggleButton::tickColourId, ink::amber);
    setColour (juce::ToggleButton::tickDisabledColourId, ink::dim);
}

void NovaLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s)
{
    const auto area = Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const float size = std::min (area.getWidth(), area.getHeight());
    const auto c = area.getCentre();
    const float rTicks = size * 0.5f - 1.0f, rKnob = rTicks - size * 0.11f, rCap = rKnob * 0.70f;

    // scale ticks around the knob (every 27°, −135° … +135°)
    g.setColour (ink::label.withAlpha (s.isEnabled() ? 1.0f : 0.4f));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = a0 + (a1 - a0) * (float) i / 10.0f;
        const auto p1 = c.getPointOnCircumference (rTicks - size * 0.04f, a), p2 = c.getPointOnCircumference (rTicks, a);
        g.drawLine ({ p1, p2 }, i == 0 || i == 10 || i == 5 ? 1.6f : 1.0f);
    }

    // drop shadow + knurled skirt (alternating light/dark wedges read as machined metal)
    juce::Path disc;
    disc.addEllipse (c.x - rKnob, c.y - rKnob, 2 * rKnob, 2 * rKnob);
    juce::DropShadow (juce::Colours::black.withAlpha (0.85f), (int) (size * 0.14f), { 0, (int) (size * 0.07f) }).drawForPath (g, disc);
    constexpr int kWedges = 96;
    for (int i = 0; i < kWedges; ++i)
    {
        const float t0 = juce::MathConstants<float>::twoPi * (float) i / kWedges, t1 = t0 + juce::MathConstants<float>::twoPi / kWedges + 0.01f;
        const float k = 0.5f + 0.5f * std::cos (4.0f * t0 + 0.6f);
        juce::Path wedge;
        wedge.addPieSegment (c.x - rKnob, c.y - rKnob, 2 * rKnob, 2 * rKnob, t0, t1, 0.0f);
        g.setColour (Colour (sk().knobSkirtA).interpolatedWith (Colour (sk().knobSkirtB), k));
        g.fillPath (wedge);
    }
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawEllipse (c.x - rKnob + 0.5f, c.y - rKnob + 0.5f, 2 * rKnob - 1.0f, 2 * rKnob - 1.0f, 1.0f);

    // domed cap
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillEllipse (c.x - rCap - 1.0f, c.y - rCap + 1.0f, 2 * rCap + 2.0f, 2 * rCap + 2.0f);
    ColourGradient cap (Colour (sk().knobCapHi), c.x - rCap * 0.25f, c.y - rCap * 0.4f, Colour (sk().knobCapLo), c.x + rCap, c.y + rCap, true);
    cap.addColour (0.55, Colour (sk().knobCapMid));
    g.setGradientFill (cap);
    g.fillEllipse (c.x - rCap, c.y - rCap, 2 * rCap, 2 * rCap);
    g.setColour (juce::Colours::white.withAlpha (0.3f));
    g.drawEllipse (c.x - rCap + 0.5f, c.y - rCap + 0.5f, 2 * rCap - 1.0f, 2 * rCap - 1.0f, 0.8f);

    // pointer
    const float a = a0 + pos * (a1 - a0);
    juce::Path ptr;
    ptr.addRoundedRectangle (-1.5f, -rKnob + size * 0.03f, 3.0f, rKnob * 0.45f, 1.5f);
    const auto tf = juce::AffineTransform::rotation (a).translated (c);
    g.setColour (Colour (sk().knobPointer).withAlpha ((juce::uint8) 0x88));
    g.strokePath (ptr, juce::PathStrokeType (2.0f), tf);
    g.setColour (Colour (sk().knobPointer).withAlpha (s.isEnabled() ? 1.0f : 0.5f));
    g.fillPath (ptr, tf);
}

void NovaLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const Colour&, bool over, bool down)
{
    const auto style = b.getProperties()["ni"].toString();
    auto r = b.getLocalBounds().toFloat();
    const bool on = b.getToggleState();
    const float alpha = b.isEnabled() ? 1.0f : 0.45f;

    if (style == "link")
    {
        const float d = std::min (r.getWidth(), r.getHeight()) - 10.0f;
        auto c = r.withSizeKeepingCentre (d, d).translated (0.0f, -2.0f);
        juce::Path p;
        p.addEllipse (c);
        juce::DropShadow (juce::Colours::black.withAlpha (0.8f), 8, { 0, 5 }).drawForPath (g, p);
        ColourGradient body (Colour (sk().linkHi), c.getX() + d * 0.4f, c.getY() + d * 0.3f, Colour (sk().linkLo), c.getRight(), c.getBottom(), true);
        g.setGradientFill (body);
        g.fillPath (p);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawEllipse (c.reduced (0.5f), 1.0f);
        if (on)
        {
            g.setColour (Colour (0x66FFB300));
            g.drawEllipse (c.expanded (1.5f), 2.0f);
        }
        // chain: two interlocked rounded links at 45°
        const Colour chain = on ? ink::amber : Colour (sk().linkOff);
        const float lw = d * 0.30f, lh = d * 0.16f;
        juce::Path link;
        link.addRoundedRectangle (-lw * 0.95f, -lh * 0.5f, lw, lh, lh * 0.5f);
        link.addRoundedRectangle (-lw * 0.05f, -lh * 0.5f, lw, lh, lh * 0.5f);
        const auto tf = juce::AffineTransform::rotation (-juce::MathConstants<float>::pi / 4.0f).translated (c.getCentre());
        if (on)
        {
            g.setColour (Colour (0x55FFB300));
            g.strokePath (link, juce::PathStrokeType (5.0f), tf);
        }
        g.setColour (chain.withMultipliedAlpha (alpha));
        g.strokePath (link, juce::PathStrokeType (2.4f), tf);
        return;
    }

    const bool red = style == "cal";
    const bool pressed = down || on;
    const float drop = pressed ? 1.0f : 3.0f;
    auto face = r.withTrimmedBottom (4.0f).translated (0.0f, pressed ? 2.0f : 0.0f);
    juce::Path fp;
    fp.addRoundedRectangle (face, 5.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 8, { 0, (int) drop + 2 }).drawForPath (g, fp);
    g.setColour ((red ? Colour (sk().calBase) : Colour (sk().hwBase)).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (face.translated (0.0f, drop), 5.0f);
    const Colour top = red ? Colour (sk().calTop) : Colour (sk().hwTop), bot = red ? Colour (sk().calBot) : Colour (sk().hwBot);
    g.setGradientFill (ColourGradient ((over ? top.brighter (0.08f) : top).withMultipliedAlpha (alpha), 0.0f, face.getY(),
                                       bot.withMultipliedAlpha (alpha), 0.0f, face.getBottom(), false));
    g.fillPath (fp);
    g.setColour ((red ? Colour (sk().calHighlight) : Colour (sk().hwHighlight)).withMultipliedAlpha (alpha));
    g.drawLine (face.getX() + 4.0f, face.getY() + 0.8f, face.getRight() - 4.0f, face.getY() + 0.8f, 1.0f);
    if (red && on)
    {
        g.setColour (Colour (0x66FF5A4A));
        g.drawRoundedRectangle (face.expanded (2.0f), 6.0f, 3.0f);
    }
}

void NovaLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool down)
{
    const auto style = b.getProperties()["ni"].toString();
    if (style == "link") return;
    const bool pressed = down || b.getToggleState();
    auto face = b.getLocalBounds().toFloat().withTrimmedBottom (4.0f).translated (0.0f, pressed ? 2.0f : 0.0f);
    g.setFont (getTextButtonFont (b, b.getHeight()));
    const Colour c = style == "cal" ? Colour (sk().calText) : Colour (sk().hwText);
    g.setColour (c.withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
    g.drawText (b.getButtonText(), face, juce::Justification::centred, false);
}

juce::Font NovaLookAndFeel::getTextButtonFont (juce::TextButton& b, int)
{
    return b.getProperties()["ni"].toString().isEmpty() ? uiFont (13.0f) : uiFont (11.5f, true, 0.08f);
}

void NovaLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);
    g.setGradientFill (ColourGradient (Colour (sk().lcdTop), 0.0f, 0.0f, Colour (sk().lcdBot), 0.0f, (float) h, false));
    g.fillRoundedRectangle (r, 5.0f);
    insetShadow (g, r, 5.0f, 1.0f);
    g.setColour (Colour (sk().lcdOutline));
    g.drawLine (4.0f, (float) h - 0.5f, (float) w - 4.0f, (float) h - 0.5f, 1.0f);
    // ▾ arrow on the left
    juce::Path tri;
    const float ax = 12.0f, ay = (float) h * 0.5f;
    tri.addTriangle (ax - 5.0f, ay - 3.0f, ax + 5.0f, ay - 3.0f, ax, ay + 4.0f);
    g.setColour (ink::lcdText.withAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.fillPath (tri);
    const auto detail = box.getProperties()["detail"].toString();
    if (detail.isNotEmpty())
    {
        g.setFont (monoFont (11.0f));
        g.setColour (ink::lcdDim);
        g.drawText (detail, r.reduced (14.0f, 0.0f), juce::Justification::centredRight, true);
    }
}

void NovaLookAndFeel::drawComboBoxTextWhenNothingSelected (juce::Graphics& g, juce::ComboBox& box, juce::Label& label)
{
    g.setColour (ink::lcdDim.withAlpha (box.isEnabled() ? 0.9f : 0.4f));
    g.setFont (getComboBoxFont (box));
    g.drawFittedText (box.getTextWhenNothingSelected(), label.getBounds().reduced (2, 1), label.getJustificationType(), 1);
}

juce::Font NovaLookAndFeel::getComboBoxFont (juce::ComboBox& box) { return monoFont (box.getProperties()["small"] ? 13.0f : 16.0f); }

void NovaLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    const auto detail = box.getProperties()["detail"].toString();
    const int detailW = detail.isEmpty() ? 0 : juce::GlyphArrangement::getStringWidthInt (monoFont (11.0f), detail) + 20;
    label.setBounds (24, 0, std::max (40, box.getWidth() - 24 - detailW), box.getHeight());
    label.setFont (getComboBoxFont (box));
    label.setColour (juce::Label::textColourId, ink::lcdText);
}

Segment::Segment (juce::StringArray items, int cellPad) : items_ (std::move (items))
{
    for (const auto& s : items_) cellW_.push_back (juce::GlyphArrangement::getStringWidthInt (uiFont (10.0f, true, 0.1f), s) + cellPad);
    totalW_ = std::max (1, std::accumulate (cellW_.begin(), cellW_.end(), 0));
}

int Segment::idealWidth() const { return std::accumulate (cellW_.begin(), cellW_.end(), 0); }

void Segment::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    juce::Path outline;
    outline.addRoundedRectangle (r, 5.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 4, { 0, 2 }).drawForPath (g, outline);
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (outline);
    const int n = items_.size();
    float x = 0.0f;
    const float total = (float) totalW_;
    const auto font = uiFont (10.0f, true, 0.1f);
    for (int i = 0; i < n; ++i)
    {
        const float wi = r.getWidth() * (float) cellW_[(size_t) i] / total;
        auto cell = Rectangle<float> (x, 0.0f, wi, r.getHeight());
        const bool on = i == sel_;
        g.setGradientFill (on ? ColourGradient (Colour (sk().segOnTop), 0.0f, 0.0f, Colour (sk().segOnBot), 0.0f, r.getHeight(), false)
                              : ColourGradient (Colour (sk().segOffTop), 0.0f, 0.0f, Colour (sk().segOffBot), 0.0f, r.getHeight(), false));
        g.fillRect (cell);
        g.setColour (juce::Colours::black);
        if (i > 0) g.drawVerticalLine ((int) x, 0.0f, r.getHeight());
        g.setColour ((on ? Colour (sk().segOnText) : Colour (sk().segOffText)).withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));
        g.setFont (font);
        g.drawText (items_[i], cell, juce::Justification::centred, false);
        x += wi;
    }
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
}

void Segment::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled()) return;
    const float total = (float) totalW_;
    float x = 0.0f;
    for (int i = 0; i < items_.size(); ++i)
    {
        x += (float) getWidth() * (float) cellW_[(size_t) i] / total;
        if ((float) e.x < x)
        {
            if (i != sel_)
            {
                setSelected (i);
                if (onChange) onChange (i);
            }
            return;
        }
    }
}

void Led::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float d = std::min (r.getWidth(), r.getHeight());
    auto c = r.withSizeKeepingCentre (d, d);
    if (on_)
    {
        g.setColour (Colour (0x66FF3020));
        g.fillEllipse (c.expanded (1.0f));
    }
    g.setColour (juce::Colours::black);
    g.fillEllipse (c.translated (0.0f, 0.8f));
    const auto inner = c.reduced (1.0f);
    ColourGradient led (on_ ? Colour (0xFFFFB3A8) : Colour (0xFF6A2A2A), inner.getX() + inner.getWidth() * 0.4f, inner.getY() + inner.getHeight() * 0.35f,
                        on_ ? Colour (0xFF7A0C06) : Colour (0xFF220606), inner.getRight(), inner.getBottom(), true);
    if (on_) led.addColour (0.45, Colour (0xFFE0261B));
    g.setGradientFill (led);
    g.fillEllipse (inner);
}

void StatusLamp::set (Colour c, juce::String line1, juce::String line2)
{
    if (c == colour_ && line1 == l1_ && line2 == l2_) return;
    colour_ = c; l1_ = std::move (line1); l2_ = std::move (line2);
    repaint();
}

void StatusLamp::paint (juce::Graphics& g)
{
    const float h = (float) getHeight();
    auto lamp = Rectangle<float> (4.0f, h * 0.5f - 6.0f, 12.0f, 12.0f);
    g.setColour (colour_.withAlpha (0.35f));
    g.fillEllipse (lamp.expanded (4.0f));
    g.setColour (juce::Colours::black);
    g.fillEllipse (lamp.translated (0.0f, 1.0f));
    ColourGradient lg (colour_.brighter (1.2f), lamp.getX() + 4.8f, lamp.getY() + 4.2f, colour_.darker (1.2f), lamp.getRight(), lamp.getBottom(), true);
    lg.addColour (0.55, colour_);
    g.setGradientFill (lg);
    g.fillEllipse (lamp);
    auto text = getLocalBounds().toFloat().withTrimmedLeft (24.0f);
    g.setFont (uiFont (11.0f, true, 0.09f));
    g.setColour (Colour (sk().lampText));
    g.drawText (l1_, text.withTrimmedBottom (h * 0.5f), juce::Justification::bottomLeft, true);
    g.setColour (ink::dim);
    g.drawText (l2_, text.withTrimmedTop (h * 0.5f), juce::Justification::topLeft, true);
}

void setupNumberSlider (juce::Slider& s, double lo, double hi, double step, const juce::String& unit)
{
    s.setSliderStyle (juce::Slider::IncDecButtons);
    s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 74, 24);
    s.setRange (lo, hi, step);
    s.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    s.textFromValueFunction = [unit] (double v) {
        return juce::String (ViewModel::signedText ((float) v, 1)) + (unit.isEmpty() ? "" : " " + unit);
    };
    s.valueFromTextFunction = [] (const juce::String& t) {
        return t.replace (juce::CharPointer_UTF8 ("\xe2\x88\x92"), "-").retainCharacters ("+-.0123456789").getDoubleValue();
    };
}
} // namespace ni::gui
