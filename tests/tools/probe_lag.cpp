// F3 S4 f3_4 fixture oracle (F1 §18 (b): "the independent fixture (F05Loop on the Apollo, same probe)"). Renders the
// product's measurement probe to a raw float32 file for `F05Loop calibrate --stimulus`, and analyses F05Loop's `--dump`
// capture with the product's matched correlator. The I/O path (CoreAudio HAL IOProc, no REAPER, no hook) is the independent
// part; probe and estimator are the product's by definition of the analog timing reference (F1 §18 Q13).
//   ni_probe_lag gen <out.f32> <peakDbfs>
//   ni_probe_lag analyse <capture.f32> <channels>      (capture = channel-major raw float32, as written by --dump)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "engine/correlator.hpp"
#include "engine/probe.hpp"

int main(int argc, char** argv)
{
    using namespace ni::engine;
    if (argc < 4) { std::fprintf(stderr, "usage: ni_probe_lag gen <out.f32> <peakDbfs> | analyse <capture.f32> <channels>\n"); return 1; }
    const ProbeGenerator probe { ProbeSpec {} };
    if (std::strcmp(argv[1], "gen") == 0)
    {
        std::vector<float> x(static_cast<size_t>(probe.length()));
        probe.render(x.data(), static_cast<float>(std::strtod(argv[3], nullptr)));
        FILE* f = std::fopen(argv[2], "wb");
        if (f == nullptr || std::fwrite(x.data(), sizeof(float), x.size(), f) != x.size()) { std::fprintf(stderr, "ERROR write\n"); return 2; }
        std::fclose(f);
        std::printf("PROBE {\"frames\":%d,\"sweep_start\":%d,\"sweep\":%d}\n", probe.length(), probe.sweepStart(), probe.sweepLength());
        return 0;
    }
    if (std::strcmp(argv[1], "analyse") != 0) { std::fprintf(stderr, "ERROR unknown mode\n"); return 1; }
    FILE* f = std::fopen(argv[2], "rb");
    if (f == nullptr) { std::fprintf(stderr, "ERROR open\n"); return 2; }
    std::vector<float> all;
    float buf[4096];
    for (size_t n; (n = std::fread(buf, sizeof(float), 4096, f)) > 0;) all.insert(all.end(), buf, buf + n);
    std::fclose(f);
    const int nch = std::atoi(argv[3]);
    if (nch <= 0 || all.size() % static_cast<size_t>(nch) != 0) { std::fprintf(stderr, "ERROR channel count\n"); return 2; }
    const size_t per = all.size() / static_cast<size_t>(nch);
    const Correlator corr { probe };
    std::printf("PROBE_LAG [");
    for (int ch = 0; ch < nch; ++ch)
    {
        const CorrResult r = corr.analyse(all.data() + static_cast<size_t>(ch) * per, static_cast<int32_t>(per), 0);
        std::printf("%s{\"ch\":%d,\"lag\":%d,\"frac\":%.3f,\"amp\":%.4f,\"neg\":%d,\"pbr\":%.1f,\"alt\":%.1f,\"alt_lag\":%d,\"ret_peak\":%.2f,\"snr\":%.1f,\"clip\":%d}",
                    ch ? "," : "", ch, r.lag, r.fractional, r.mainAmp, r.negative ? 1 : 0, r.pbrDb, r.altPeakRelDb, r.altLag,
                    r.returnPeakDbfs, r.snrDb, r.clipped ? 1 : 0);
    }
    std::printf("]\n");
    return 0;
}
