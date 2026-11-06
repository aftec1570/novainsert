// Persistence (plan Task 22, R-LIB-1 project side): the VST3 state blob = JSON {"schema":1, "params":{id: value},
// "device":{…}, "calibration":{…}}. The snapshot travels with the project so a reopened project locks without probing when
// the calibration key still matches; a library record newer than the snapshot is reported, never auto-applied.
// Message thread only.
#pragma once
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <juce_core/juce_core.h>
#include "plugin/library.hpp"

namespace ni::plugin
{
using ParamSnapshot = std::map<std::string, double>;   // automatable parameter id → plain value

// The instance's own CAL block (VU GUI spec §7): LINK and per-channel offsets; absent → the device defaults apply.
struct InstanceCal
{
    bool link = false;
    float sendOffsetDb[2] { 0.0f, 0.0f }, returnOffsetDb[2] { 0.0f, 0.0f };
};

struct InstanceState
{
    int schema = 1;
    ParamSnapshot params;
    std::optional<DeviceRecord> deviceSnapshot;
    std::optional<CalibRecord> calibSnapshot;
    std::optional<InstanceCal> cal;
};

juce::MemoryBlock serialize(const InstanceState& s);
std::optional<InstanceState> deserialize(const void* data, std::size_t size, std::string* whyOut = nullptr);   // nullopt: unreadable or newer schema

// DefaultsChanged: the library record differs from the snapshot only in the CAL defaults (trims, LINK, offsets).
enum class SnapshotDiff { Same, DeviceChanged, CalibChanged, NotInLibrary, DefaultsChanged };
SnapshotDiff compare(const InstanceState& s, const DeviceLibrary& lib);

enum class RestoreAction { ApplySnapshot, Remeasure, MeasureKeepSnapshot };
// ApplySnapshot: a successful calibration snapshot whose key equals the current one → ApplyingLatency(StateRestore), 0 probes.
// MeasureKeepSnapshot (measure on open, user 2026-11-06): the same snapshot, but the instance measures and keeps it as the
// fallback for a final failure. Remeasure: no usable snapshot for this key.
RestoreAction restoreAction(const InstanceState& s, const engine::CalibKey& current, bool measureOnOpen = false);
// The snapshot a failed measure-on-open falls back to: only on a final failure (no retry queued) and only while the key is still
// the one the snapshot was taken under.
std::optional<CalibRecord> openFallback(const std::optional<CalibRecord>& kept, bool finalFailure,
                                        const std::optional<engine::CalibKey>& current);

// The record an instance runs with (F1 §16/§21): a restored project's device snapshot wins over the library record, so a later
// library edit never changes an old project's sound and a missing/corrupt library still runs; otherwise the library record.
std::optional<DeviceRecord> activeDevice(const std::optional<DeviceRecord>& fromLibrary, const std::optional<DeviceRecord>& fromProject);

// The project snapshot's life in one instance (message thread only). A state load arms a token for the device parameter value
// the state carried; the next claim of that value is the restore (the slot gets the calibration snapshot, the instance runs the
// snapshot record); a claim of any other value is a user selection and drops the snapshot (S7/S8 review C1/I1).
class ProjectRestore
{
public:
    enum class Claim { Restore, Selection };
    void loaded(const InstanceState& st, int deviceParam);
    Claim claim(int deviceParam);
    bool armed() const { return armed_; }
    int deviceParam() const { return param_; }
    bool pending() const { return pending_.has_value(); }
    // the record for the claimed parameter value: the kept project snapshot for its own value, else the library record
    std::optional<DeviceRecord> record(int claimedParam, const std::optional<DeviceRecord>& fromLibrary) const;
    bool usesProject(int claimedParam) const { return device_ && claimedParam == param_; }
    void reindex(int deviceParam) { param_ = deviceParam; }   // the running record's library index moved (a preset deleted elsewhere): the snapshot follows it
    const std::optional<DeviceRecord>& device() const { return device_; }
    bool snapshotEquals(const std::optional<DeviceRecord>& running) const;   // the instance already runs the snapshot record
    // An edit this instance wrote to the library (SETUP converter, CAL result, Apply defaults) reaches the kept snapshot: the
    // converter, default, name and mode fields follow the library record of the same id; channels/identity stay as kept, and the version
    // follows only when nothing else differs (so compare() then reports Same, not a hidden device change). Final review C1/I1.
    void adopt(const DeviceRecord& fromLibrary);
    std::optional<InstanceState> takePending();
    void dropPending() { pending_.reset(); armed_ = false; }   // no claim follows this load (review I2)
    std::optional<CalibRecord> calibForSave(const std::string& deviceId) const;   // review I5
    void setStale(bool s) { stale_ = s; }
    bool stale() const { return stale_ && device_.has_value(); }
    void clear() { *this = ProjectRestore {}; }

private:
    std::optional<DeviceRecord> device_;
    std::optional<InstanceState> pending_;
    int param_ = -1;
    bool armed_ = false, stale_ = false;
};
} // namespace ni::plugin
