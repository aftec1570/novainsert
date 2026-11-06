// PairRegistry — process-wide ownership of physical send/return channel pairs (F1 §1/§6, plan Task 7).
// Message thread only. First-come ownership; any shared physical channel (out or in) with an owner is a
// conflict; conflicting claimants wait in FIFO order and the oldest is promoted when the owner releases.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "host/reaper_api.hpp"

namespace ni::host
{

struct Pair
{
    int out[2];     // 0-based hardware output channels; [1] ignored when !sendStereo
    int in[2];      // 0-based hardware input channels;  [1] ignored when !retStereo
    bool sendStereo = true;   // per-direction widths (mono/stereo routing design 2026-11-05)
    bool retStereo = true;
};

enum class Claim { Owner, Conflict };

class PairRegistry
{
public:
    // deviceId: library device identity of the claimant (empty = none) for sameLibraryDeviceElsewhere().
    Claim claim(int instanceId, const Pair& pair, std::string trackName, std::string deviceId = {});
    void release(int instanceId);                                   // owner or waiter; promotes the oldest waiter
    std::optional<std::string> ownerTrackOf(const Pair& pair) const; // owner of any channel the pair touches
    std::optional<std::string> ownerTrackOf(const Pair& pair, int exceptInstance) const;   // same, never the asker itself
    bool sameLibraryDeviceElsewhere(int instanceId, const std::string& deviceId) const;
    // Tracks with a hardware output send overlapping the pair's outputs: "<track> hardware send on out 9/10"
    // (1-based channel numbers as REAPER shows them). Enumerates every track: message thread only.
    std::vector<std::string> hardwareSendWarnings(const ReaperApi& api, ReaProject* proj, const Pair& pair) const;
    std::function<void(int instanceId)> onPromoted;                 // fired once per promotion, after ownership moved

private:
    struct Entry { int id; Pair pair; std::string track; std::string device; bool owner; };
    std::vector<Entry> entries_;   // claim order = FIFO order
    static bool overlaps(const Pair& a, const Pair& b);
    const Entry* ownerOf(const Pair& pair, int except = -1) const;
};

} // namespace ni::host
