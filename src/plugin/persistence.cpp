#include "plugin/persistence.hpp"
#include "plugin/library_json.hpp"

namespace ni::plugin
{
juce::MemoryBlock serialize(const InstanceState& s)
{
    auto* o = new juce::DynamicObject();
    auto* params = new juce::DynamicObject();
    for (const auto& [id, v] : s.params) params->setProperty(juce::Identifier(id.c_str()), v);
    o->setProperty("schema", s.schema);
    o->setProperty("params", juce::var(params));
    if (s.deviceSnapshot) o->setProperty("device", json::fromDevice(*s.deviceSnapshot));
    if (s.calibSnapshot) o->setProperty("calibration", json::fromCalibration(*s.calibSnapshot));
    if (s.cal)
    {
        auto* c = new juce::DynamicObject();
        c->setProperty("link", s.cal->link);
        c->setProperty("sendOffsetDb", juce::Array<juce::var> { s.cal->sendOffsetDb[0], s.cal->sendOffsetDb[1] });
        c->setProperty("returnOffsetDb", juce::Array<juce::var> { s.cal->returnOffsetDb[0], s.cal->returnOffsetDb[1] });
        o->setProperty("cal", juce::var(c));
    }
    const auto text = juce::JSON::toString(juce::var(o), true);
    return juce::MemoryBlock(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

std::optional<InstanceState> deserialize(const void* data, std::size_t size, std::string* whyOut)
{
    auto fail = [&](std::string why) -> std::optional<InstanceState> { if (whyOut) *whyOut = std::move(why); return std::nullopt; };
    if (data == nullptr || size == 0) return fail("empty state");
    juce::var root;
    const auto r = juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(data), static_cast<int>(size)), root);
    if (r.failed() || !root.isObject()) return fail("state unreadable: " + r.getErrorMessage().toStdString());
    InstanceState s;
    s.schema = static_cast<int>(root.getProperty("schema", 0));
    if (s.schema > 1) return fail("state schema " + std::to_string(s.schema) + " is newer than this NovaInsert");
    s.schema = 1;
    if (const auto* p = root["params"].getDynamicObject())
        for (const auto& kv : p->getProperties()) s.params[kv.name.toString().toStdString()] = static_cast<double>(kv.value);
    if (root.hasProperty("device")) s.deviceSnapshot = json::toDevice(root["device"]);
    if (root.hasProperty("calibration")) s.calibSnapshot = json::toCalibration(root["calibration"]);
    if (root.hasProperty("cal"))
    {
        const auto& c = root["cal"];
        InstanceCal ic;
        ic.link = static_cast<bool>(c.getProperty("link", false));
        auto rd = [&](const char* k, float dst[2]) {
            if (const auto* a = c.getProperty(k, juce::var()).getArray())
                for (int i = 0; i < 2 && i < a->size(); ++i) dst[i] = sanitizeOffsetDb(static_cast<float>(static_cast<double>((*a)[i])));
        };
        rd("sendOffsetDb", ic.sendOffsetDb); rd("returnOffsetDb", ic.returnOffsetDb);
        s.cal = ic;
    }
    return s;
}

SnapshotDiff compare(const InstanceState& s, const DeviceLibrary& lib)
{
    if (!s.deviceSnapshot) return SnapshotDiff::Same;
    const auto d = lib.device(s.deviceSnapshot->id);
    if (!d) return SnapshotDiff::NotInLibrary;
    if (!(*d == *s.deviceSnapshot)) return sameExceptDefaults(*d, *s.deviceSnapshot) ? SnapshotDiff::DefaultsChanged : SnapshotDiff::DeviceChanged;
    if (s.calibSnapshot)
        if (const auto c = lib.calibration(s.calibSnapshot->key); c && c->version != s.calibSnapshot->version)
            return SnapshotDiff::CalibChanged;
    return SnapshotDiff::Same;
}

RestoreAction restoreAction(const InstanceState& s, const engine::CalibKey& current, bool measureOnOpen)
{
    if (s.calibSnapshot && s.calibSnapshot->result.failReason == engine::Reason::None && s.calibSnapshot->key == current)
        return measureOnOpen ? RestoreAction::MeasureKeepSnapshot : RestoreAction::ApplySnapshot;
    return RestoreAction::Remeasure;
}

std::optional<CalibRecord> openFallback(const std::optional<CalibRecord>& kept, bool finalFailure,
                                        const std::optional<engine::CalibKey>& current)
{
    if (kept && finalFailure && current && kept->key == *current) return kept;
    return std::nullopt;
}

std::optional<DeviceRecord> activeDevice(const std::optional<DeviceRecord>& fromLibrary, const std::optional<DeviceRecord>& fromProject)
{
    return fromProject ? fromProject : fromLibrary;
}

void ProjectRestore::loaded(const InstanceState& st, int deviceParam)
{
    clear();
    device_ = st.deviceSnapshot;
    param_ = deviceParam;
    armed_ = true;
    if (st.calibSnapshot) pending_ = st;
}

ProjectRestore::Claim ProjectRestore::claim(int deviceParam)
{
    if (armed_ && deviceParam == param_) { armed_ = false; return Claim::Restore; }
    if (deviceParam != param_) clear();
    else { armed_ = false; pending_.reset(); }   // the same value claimed again without a load: a selection, nothing to restore
    return Claim::Selection;
}

std::optional<DeviceRecord> ProjectRestore::record(int claimedParam, const std::optional<DeviceRecord>& fromLibrary) const
{
    return activeDevice(fromLibrary, usesProject(claimedParam) ? device_ : std::nullopt);
}

bool ProjectRestore::snapshotEquals(const std::optional<DeviceRecord>& running) const
{
    return device_ && running && *device_ == *running;
}

void ProjectRestore::adopt(const DeviceRecord& lib)
{
    if (!device_ || device_->id != lib.id) return;
    DeviceRecord& d = *device_;
    d.name = lib.name; d.mode = lib.mode;   // DEVICES page rename / mode edit reaches the restored instance (final review M1)
    d.sendDbuAt0dBFS = lib.sendDbuAt0dBFS; d.returnDbuAt0dBFS = lib.returnDbuAt0dBFS; d.vuRefDbfs = lib.vuRefDbfs;
    d.outboardMaxInDbu = lib.outboardMaxInDbu;
    d.sendTrimDb = lib.sendTrimDb; d.returnTrimDb = lib.returnTrimDb; d.link = lib.link;
    for (int ch = 0; ch < 2; ++ch) { d.sendOffsetDb[ch] = lib.sendOffsetDb[ch]; d.returnOffsetDb[ch] = lib.returnOffsetDb[ch]; }
    DeviceRecord v = d; v.version = lib.version;
    if (v == lib) d.version = lib.version;
}

std::optional<InstanceState> ProjectRestore::takePending()
{
    auto p = std::move(pending_);
    pending_.reset();
    return p;
}

std::optional<CalibRecord> ProjectRestore::calibForSave(const std::string& deviceId) const
{
    if (pending_ && pending_->calibSnapshot && pending_->calibSnapshot->deviceId == deviceId) return pending_->calibSnapshot;
    return std::nullopt;
}
} // namespace ni::plugin
