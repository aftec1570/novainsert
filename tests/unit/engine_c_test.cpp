// C surface over the correlator and lock evaluator (plan Task 13): same lag, fields and verdict as the C++ classes.
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "engine/correlator.hpp"
#include "engine/engine_c.h"

using namespace ni::engine;

TEST_CASE("c_surface_matches_cpp_correlator_and_lock")
{
    const ProbeGenerator g { ProbeSpec {} };
    const Correlator c { g };
    std::vector<float> clean(static_cast<size_t>(g.length())), x(clean.size(), 0.0f);
    g.render(clean.data(), -12.0f);
    const size_t D = 2222;
    for (size_t n = 0; n + D < x.size(); ++n) x[n + D] = -0.5f * clean[n];

    const auto ref = c.analyse(x.data(), static_cast<int32_t>(x.size()), 128);
    ni_corr* h = ni_corr_create(48000);
    REQUIRE(h != nullptr);
    ni_corr_result r[3] {};
    for (auto& ri : r) REQUIRE(ni_corr_analyse(h, x.data(), static_cast<int>(x.size()), 128, &ri) == 0);
    REQUIRE(r[0].lag == ref.lag);
    REQUIRE(r[0].lag == static_cast<int>(D));
    REQUIRE(r[0].negative == 1);
    REQUIRE(r[0].mainAmp == ref.mainAmp);
    REQUIRE(r[0].pbrDb == ref.pbrDb);
    REQUIRE(r[0].snrDb == ref.snrDb);
    REQUIRE(r[0].clipped == 0);

    int lag = -1, reason = -1;
    REQUIRE(ni_lock_evaluate(r, 9, 9, 128, &lag, &reason) == 1);
    REQUIRE(lag == static_cast<int>(D));
    REQUIRE(reason == static_cast<int>(Reason::None));
    REQUIRE(ni_lock_evaluate(r, 9, 10, 128, &lag, &reason) == 0);
    REQUIRE(reason == static_cast<int>(Reason::KeyChange));

    REQUIRE(ni_corr_analyse(nullptr, x.data(), 10, 128, &r[0]) != 0);
    REQUIRE(ni_corr_analyse(h, x.data(), 10, 128, nullptr) != 0);
    ni_corr_destroy(h);
    ni_corr_destroy(nullptr);
}
