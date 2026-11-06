// VU needle ballistics (VU GUI spec §5, IEC 60268-17). JUCE-free; GUI thread only.
// A 2nd-order mass-spring-damper (ζ 0.81, ωn 13.4 rad/s → 99 % of a step in 300 ms, 1.3 % overshoot) driven by the full-wave
// rectified average, integrated in fixed 1 ms steps independent of the paint rate (no stutter). Position is linear in voltage:
// 0 = rest, 1 = +3 VU; the stop pins sit at −0.02 and 1.05 and bounce the needle back with restitution 0.25.
#pragma once
#include <algorithm>
#include <cmath>

namespace ni::gui
{
constexpr float kZeta = 0.81f, kOmega = 13.4f, kRestitution = 0.25f;
constexpr double kStep = 0.001;
constexpr float kPinLow = -0.02f, kPinHigh = 1.05f;

inline float vuToPosition(float vu) noexcept { return std::pow(10.0f, (vu - 3.0f) / 20.0f); }
inline float positionToVu(float pos) noexcept { return pos > 1e-6f ? 20.0f * std::log10(pos) + 3.0f : -120.0f; }

// Rectified average (linear) → drive relative to 0 VU (1.0 = 0 VU), read as the AES17 level of a sine with that average
// (= its peak: a 0 VU = −20 dBFS reference sine peaks at −20 dBFS and drives 1.0).
inline float needleDrive(float rectAvgLin, float zeroVuDbfs) noexcept
{
    constexpr float kSinePeakPerAvg = 1.5707963f;   // π / 2
    return rectAvgLin * kSinePeakPerAvg / std::pow(10.0f, zeroVuDbfs / 20.0f);
}

class VuNeedle
{
public:
    void advance(double dtSeconds, float drive) noexcept
    {
        acc_ += std::max(0.0, dtSeconds);
        const float target = drive * vuToPosition(0.0f);   // drive 1.0 = 0 VU
        const float h = static_cast<float>(kStep);
        while (acc_ >= kStep)
        {
            acc_ -= kStep;
            const float a = kOmega * kOmega * (target - x_) - 2.0f * kZeta * kOmega * v_;
            v_ += a * h;
            x_ += v_ * h;
            if (x_ > kPinHigh) { x_ = kPinHigh; v_ = -v_ * kRestitution; }
            else if (x_ < kPinLow) { x_ = kPinLow; v_ = -v_ * kRestitution; }
        }
    }
    float position() const noexcept { return x_; }
    float velocity() const noexcept { return v_; }
    void reset(float pos = 0.0f) noexcept { x_ = pos; v_ = 0.0f; acc_ = 0.0; }

private:
    float x_ = 0.0f, v_ = 0.0f;
    double acc_ = 0.0;
};
} // namespace ni::gui
