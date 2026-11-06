// NovaInsert engine C surface (plan Task 13): see engine_c.h.
#include "engine/engine_c.h"

#include <new>
#include "engine/correlator.hpp"

using namespace ni::engine;

struct ni_corr
{
    explicit ni_corr(const ProbeSpec& s) : probe(s), corr(probe) {}
    ProbeGenerator probe;
    Correlator corr;
};

namespace
{
ni_corr_result toC(const CorrResult& r) noexcept
{
    return { r.lag, r.mainAmp, r.negative ? 1 : 0, r.fractional, r.pbrDb, r.altPeakRelDb, r.altLag,
             r.returnPeakDbfs, r.returnRmsDbfs, r.preSilenceRmsDbfs, r.snrDb, r.clipped ? 1 : 0 };
}

CorrResult fromC(const ni_corr_result& r) noexcept
{
    return { r.lag, r.mainAmp, r.negative != 0, r.fractional, r.pbrDb, r.altPeakRelDb, r.altLag,
             r.returnPeakDbfs, r.returnRmsDbfs, r.preSilenceRmsDbfs, r.snrDb, r.clipped != 0 };
}
} // namespace

extern "C" ni_corr* ni_corr_create(int fs)
{
    if (fs <= 0) return nullptr;
    ProbeSpec s;
    s.fs = fs;
    s.postSec = static_cast<double>(kLMax) / fs + 0.5;
    try { return new ni_corr(s); }
    catch (...) { return nullptr; }
}

extern "C" void ni_corr_destroy(ni_corr* c) { delete c; }

extern "C" int ni_corr_analyse(ni_corr* c, const float* x, int n, int hwBlock, ni_corr_result* out)
{
    if (c == nullptr || x == nullptr || out == nullptr || n < 0) return -1;
    *out = toC(c->corr.analyse(x, n, hwBlock));
    return 0;
}

extern "C" int ni_lock_evaluate(const ni_corr_result r[3], unsigned genStart, unsigned genNow, int hwBlock,
                                int* lagOut, int* reasonOut)
{
    if (r == nullptr) return -1;
    LockInputs in {};
    for (int i = 0; i < 3; ++i) in.probe[i] = fromC(r[i]);
    in.genStart = genStart; in.genNow = genNow; in.hwBlock = hwBlock;
    const auto v = evaluateLock(in);
    if (lagOut != nullptr) *lagOut = v.lag;
    if (reasonOut != nullptr) *reasonOut = static_cast<int>(v.reason);
    return v.locked ? 1 : 0;
}
