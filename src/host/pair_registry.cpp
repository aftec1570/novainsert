#include "host/pair_registry.hpp"
#include <algorithm>
#include <cstring>

namespace ni::host
{
namespace
{
int chans(const int (&c)[2], bool stereo, int out[2])
{
    int n = 0;
    if (c[0] >= 0) out[n++] = c[0];
    if (stereo && c[1] >= 0) out[n++] = c[1];
    return n;
}
}

bool PairRegistry::overlaps(const Pair& a, const Pair& b)
{
    int ao[2], bo[2], ai[2], bi[2];
    const int nao = chans(a.out, a.sendStereo, ao), nbo = chans(b.out, b.sendStereo, bo);
    const int nai = chans(a.in, a.retStereo, ai), nbi = chans(b.in, b.retStereo, bi);
    for (int i = 0; i < nao; ++i) for (int j = 0; j < nbo; ++j) if (ao[i] == bo[j]) return true;
    for (int i = 0; i < nai; ++i) for (int j = 0; j < nbi; ++j) if (ai[i] == bi[j]) return true;
    return false;
}

const PairRegistry::Entry* PairRegistry::ownerOf(const Pair& pair, int except) const
{
    for (const auto& e : entries_)
        if (e.owner && e.id != except && overlaps(e.pair, pair)) return &e;
    return nullptr;
}

Claim PairRegistry::claim(int instanceId, const Pair& pair, std::string trackName, std::string deviceId)
{
    release(instanceId);   // a re-claim replaces the previous registration
    const bool conflict = ownerOf(pair) != nullptr;
    entries_.push_back(Entry { instanceId, pair, std::move(trackName), std::move(deviceId), !conflict });
    return conflict ? Claim::Conflict : Claim::Owner;
}

void PairRegistry::release(int instanceId)
{
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == instanceId; });
    if (it == entries_.end()) return;
    const bool wasOwner = it->owner;
    entries_.erase(it);
    if (!wasOwner) return;
    // Promote the oldest waiter whose pair is now free of owners.
    for (auto& e : entries_)
    {
        if (e.owner || ownerOf(e.pair, e.id) != nullptr) continue;
        e.owner = true;
        const int id = e.id;
        if (onPromoted) onPromoted(id);
        return;
    }
}

std::optional<std::string> PairRegistry::ownerTrackOf(const Pair& pair) const
{
    if (const Entry* e = ownerOf(pair)) return e->track;
    return std::nullopt;
}

std::optional<std::string> PairRegistry::ownerTrackOf(const Pair& pair, int exceptInstance) const
{
    if (const Entry* e = ownerOf(pair, exceptInstance)) return e->track;
    return std::nullopt;
}

bool PairRegistry::sameLibraryDeviceElsewhere(int instanceId, const std::string& deviceId) const
{
    if (deviceId.empty()) return false;
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id != instanceId && e.device == deviceId; });
}

std::vector<std::string> PairRegistry::hardwareSendWarnings(const ReaperApi& api, ReaProject* proj, const Pair& pair) const
{
    std::vector<std::string> out;
    if (!api.CountTracks || !api.GetTrack || !api.GetTrackNumSends || !api.GetTrackSendInfo_Value) return out;
    int po[2]; const int npo = chans(pair.out, pair.sendStereo, po);
    const int n = api.CountTracks(proj);
    for (int t = 0; t < n; ++t)
    {
        MediaTrack* tr = api.GetTrack(proj, t);
        if (!tr) continue;
        const int sends = api.GetTrackNumSends(tr, 1);   // category 1 = hardware output
        for (int s = 0; s < sends; ++s)
        {
            const int dst = static_cast<int>(api.GetTrackSendInfo_Value(tr, 1, s, "I_DSTCHAN"));
            if (dst < 0) continue;
            const bool mono = (dst & 1024) != 0;
            const int first = dst & 1023, count = mono ? 1 : 2;
            bool hit = false;
            for (int i = 0; i < npo; ++i) if (po[i] >= first && po[i] < first + count) hit = true;
            if (!hit) continue;
            char name[256] = "";
            if (api.GetTrackName) api.GetTrackName(tr, name, sizeof(name));
            std::string w = name;
            w += " hardware send on out " + std::to_string(first + 1);
            if (!mono) w += "/" + std::to_string(first + 2);
            out.push_back(std::move(w));
        }
    }
    return out;
}

} // namespace ni::host
