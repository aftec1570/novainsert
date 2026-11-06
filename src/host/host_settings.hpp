// HostSettings — ownership-based anticipative-FX handling (F1 §5, plan Task 9; R-HOOK-3/R-HOOK-4).
// Message thread only. Every instance requires I_PERFFLAGS bit 2 ("no anticipative FX") on its own track and all
// folder ancestors. Records per track: the value before our write, the value we wrote, and the requiring instances.
// release() restores a track only when nobody else requires it, the current value is still ours, and the previous
// value had bit 2 clear. recheck() (500 ms) follows reorders and reports a user-cleared bit once per our write
// (onViolation); the bit is never set again automatically — reapply() is the explicit GUI/ReaScript offer.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "host/reaper_api.hpp"

namespace ni::host
{

class HostSettings
{
public:
    static constexpr int kNoAnticipativeBit = 2;

    explicit HostSettings(const ReaperApi& api) : api_(api) {}
    void require(int instanceId, MediaTrack* own);
    void release(int instanceId);
    void recheck();
    bool reapply(int instanceId);                       // sets the bit again on every track the instance requires
    std::function<void(int instanceId, std::string trackName)> onViolation;
    std::function<void(int instanceId)> onViolationCleared;   // the bit is set again (user, or any instance's reapply)

private:
    struct Record { MediaTrack* track; int prevValue; int ourValue; std::vector<int> requirers; bool violationReported; };
    const ReaperApi& api_;
    std::map<std::string, Record> records_;            // keyed by track GUID bytes
    std::map<int, MediaTrack*> ownTrack_;              // instance → own track
    std::map<int, std::vector<std::string>> required_; // instance → GUID keys (own + ancestors)

    std::string keyOf(MediaTrack* t) const;
    bool valid(MediaTrack* t) const;                   // ValidatePtr2: REAPER may have deleted the track since we saw it
    std::vector<MediaTrack*> chain(MediaTrack* own) const;
    int get(MediaTrack* t) const;
    void set(MediaTrack* t, int v) const;
    void requireTrack(int instanceId, MediaTrack* t);
    void releaseTrack(int instanceId, const std::string& key);
};

} // namespace ni::host
