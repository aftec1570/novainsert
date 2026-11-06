#include "host/host_settings.hpp"
#include <algorithm>
#include <cstring>

namespace ni::host
{

std::string HostSettings::keyOf(MediaTrack* t) const
{
    if (api_.GetTrackGUID)
        if (GUID* g = api_.GetTrackGUID(t)) return std::string(reinterpret_cast<const char*>(g), sizeof(GUID));
    return std::string(reinterpret_cast<const char*>(&t), sizeof(t));   // fallback: pointer identity
}

bool HostSettings::valid(MediaTrack* t) const
{
    if (t == nullptr) return false;
    return api_.ValidatePtr2 == nullptr || api_.ValidatePtr2(nullptr, t, "MediaTrack*");
}

std::vector<MediaTrack*> HostSettings::chain(MediaTrack* own) const
{
    std::vector<MediaTrack*> out;
    for (MediaTrack* t = own; valid(t) && out.size() < 64; t = api_.GetParentTrack ? api_.GetParentTrack(t) : nullptr)
        out.push_back(t);
    return out;
}

int HostSettings::get(MediaTrack* t) const { return api_.GetMediaTrackInfo_Value ? static_cast<int>(api_.GetMediaTrackInfo_Value(t, "I_PERFFLAGS")) : 0; }
void HostSettings::set(MediaTrack* t, int v) const { if (api_.SetMediaTrackInfo_Value) api_.SetMediaTrackInfo_Value(t, "I_PERFFLAGS", v); }

void HostSettings::requireTrack(int instanceId, MediaTrack* t)
{
    const std::string key = keyOf(t);
    auto it = records_.find(key);
    if (it == records_.end())
    {
        const int prev = get(t);
        const int ours = prev | kNoAnticipativeBit;
        if (ours != prev) set(t, ours);
        it = records_.emplace(key, Record { t, prev, ours, {}, false }).first;
    }
    it->second.track = t;   // same GUID, possibly a new object (delete + undo): always address the current one
    auto& req = it->second.requirers;
    if (std::find(req.begin(), req.end(), instanceId) == req.end()) req.push_back(instanceId);
    auto& list = required_[instanceId];
    if (std::find(list.begin(), list.end(), key) == list.end()) list.push_back(key);
}

void HostSettings::releaseTrack(int instanceId, const std::string& key)
{
    const auto it = records_.find(key);
    if (it == records_.end()) return;
    auto& r = it->second;
    r.requirers.erase(std::remove(r.requirers.begin(), r.requirers.end(), instanceId), r.requirers.end());
    if (!r.requirers.empty()) return;
    // Restore only our own write: nobody else requires it, the value is still what we wrote, and it was off before.
    // A track REAPER has deleted is never touched (nothing to restore).
    if (valid(r.track) && get(r.track) == r.ourValue && (r.prevValue & kNoAnticipativeBit) == 0) set(r.track, r.prevValue);
    records_.erase(it);
}

void HostSettings::require(int instanceId, MediaTrack* own)
{
    if (own == nullptr) return;
    ownTrack_[instanceId] = own;
    for (MediaTrack* t : chain(own)) requireTrack(instanceId, t);
}

void HostSettings::release(int instanceId)
{
    const auto it = required_.find(instanceId);
    if (it != required_.end())
    {
        const std::vector<std::string> keys = it->second;
        required_.erase(it);
        for (const auto& k : keys) releaseTrack(instanceId, k);
    }
    ownTrack_.erase(instanceId);
}

void HostSettings::recheck()
{
    // Folder structure may have changed: require new ancestors, release tracks that are no longer ancestors.
    for (const auto& [id, own] : ownTrack_)
    {
        std::vector<std::string> now;
        for (MediaTrack* t : chain(own)) now.push_back(keyOf(t));
        const std::vector<std::string> before = required_[id];
        for (const auto& k : before)
            if (std::find(now.begin(), now.end(), k) == now.end())
            {
                auto& list = required_[id]; list.erase(std::remove(list.begin(), list.end(), k), list.end());
                releaseTrack(id, k);
            }
        for (MediaTrack* t : chain(own)) requireTrack(id, t);
    }
    // A user-cleared bit: report once per our write to every requirer; never set it again automatically.
    // A bit the user set again clears the violation for every requirer. Callbacks run after the scan: a handler
    // may call release()/reapply(), which mutate records_.
    std::vector<std::pair<int, std::string>> violated;
    std::vector<int> cleared;
    for (auto& [key, r] : records_)
    {
        if (!valid(r.track)) continue;   // deleted since the chain scan above (released at the next recheck)
        const int cur = get(r.track);
        if ((cur & kNoAnticipativeBit) != 0)
        {
            if (r.violationReported) { r.violationReported = false; for (const int id : r.requirers) cleared.push_back(id); }
            continue;
        }
        if (r.violationReported) continue;
        r.violationReported = true;
        char name[256] = "";
        if (api_.GetTrackName) api_.GetTrackName(r.track, name, sizeof(name));
        for (const int id : r.requirers) violated.emplace_back(id, name);
    }
    if (onViolation) for (auto& [id, name] : violated) onViolation(id, name);
    if (onViolationCleared) for (const int id : cleared) onViolationCleared(id);
}

bool HostSettings::reapply(int instanceId)
{
    const auto it = required_.find(instanceId);
    if (it == required_.end()) return false;
    bool any = false;
    std::vector<int> cleared;
    for (const auto& k : it->second)
    {
        const auto rec = records_.find(k);
        if (rec == records_.end()) continue;
        auto& r = rec->second;
        if (!valid(r.track)) continue;
        const int cur = get(r.track);
        if ((cur & kNoAnticipativeBit) == 0) { r.ourValue = cur | kNoAnticipativeBit; set(r.track, r.ourValue); any = true; }
        if (r.violationReported) { r.violationReported = false; for (const int id : r.requirers) cleared.push_back(id); }   // a new write of ours: the next user clear is reported again
    }
    if (onViolationCleared) for (const int id : cleared) onViolationCleared(id);
    return any;
}

} // namespace ni::host
