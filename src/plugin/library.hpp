// DeviceLibrary (plan Task 23, F1 §9 / R-LIB-1): device records, calibrations and settings under
// <ResourcePath>/NovaInsert/{devices,calibrations,settings}.json, "schema": 1. Writes are atomic (temp file + rename) under
// an flock on <dir>/.lock and re-read the file first, merging by id + version, so two REAPER instances never lose a record.
// Schema 0 (no field) is migrated and rewritten; a newer schema is used read-only and never rewritten; a read-only or corrupt
// file warns and the plugin runs on in-memory data (audio is never blocked). Message thread / worker only — never RT.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "engine/level_policy.hpp"
#include "engine/types.hpp"
#include "host/pair_registry.hpp"

namespace ni::plugin
{

// Per-channel trim offsets come from files a user or another tool may edit: every one that reaches the engine is finite and
// inside the trim range (non-finite → 0).
constexpr float kOffsetMinDb = -24.0f, kOffsetMaxDb = 12.0f;
inline float sanitizeOffsetDb(float v) noexcept
{
    if (!(v >= -1e30f && v <= 1e30f)) return 0.0f;   // NaN / ±inf: every comparison with NaN is false (no <cmath> here)
    return v < kOffsetMinDb ? kOffsetMinDb : v > kOffsetMaxDb ? kOffsetMaxDb : v;
}

struct DeviceRecord
{
    std::string id;
    std::string name;
    bool sendStereo = true, returnStereo = true;   // per-direction widths (mono/stereo routing design 2026-11-05)
    int out[2] = { -1, -1 };   // 0-based hardware output channels; [1] = -1 for a mono send
    int in[2] = { -1, -1 };    // 0-based hardware input channels; [1] = -1 for a mono return
    std::string identIn, identOut;   // device identity the channels belong to (F1 §4)
    engine::Mode mode = engine::Mode::Insert;
    float sendTrimDb = 0.0f, returnTrimDb = 0.0f, vuRefDbfs = -20.0f;
    int version = 0;                       // bumped by every successful upsert
    int s3LoopExtra = 0;       // provisional loop beyond 2B until the Measurer locks (S3 bench records)
    // Level CAL (VU GUI spec §2/§7): converter dBu at 0 dBFS, the outboard's max input, LINK and per-channel trim offsets.
    float sendDbuAt0dBFS = 24.0f, returnDbuAt0dBFS = 24.0f;
    std::optional<float> outboardMaxInDbu;
    bool link = false;
    float sendOffsetDb[2] = { 0.0f, 0.0f }, returnOffsetDb[2] = { 0.0f, 0.0f };

    // Exact field equality (DEVICES page dirty tracking, snapshot diff): a stored float equals itself bit for bit.
#if defined(__GNUC__)   // clang and gcc
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
    bool operator==(const DeviceRecord&) const = default;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
};

// True when a and b differ only in the CAL defaults (trims, LINK, per-channel offsets) and the version they bump: a project
// snapshot that differs from the library record only there is "DefaultsChanged", not a different device.
bool sameExceptDefaults(const DeviceRecord& a, const DeviceRecord& b);

// The record's whole pair (ownership: a preset is in use as a unit) and the pair the loop actually runs on: a STEREO direction
// narrowed to one side by its selector (L = [0], R = [1]) becomes a mono direction on that channel; a MONO direction ignores
// its selector (mono/stereo routing design 2026-11-05).
inline host::Pair recordPair(const DeviceRecord& d) noexcept { return host::Pair { { d.out[0], d.out[1] }, { d.in[0], d.in[1] }, d.sendStereo, d.returnStereo }; }
inline host::Pair effectivePair(host::Pair p, engine::SideSel send, engine::SideSel ret) noexcept
{
    if (p.sendStereo && send != engine::SideSel::Stereo) { p.out[0] = p.out[send == engine::SideSel::R ? 1 : 0]; p.out[1] = -1; p.sendStereo = false; }
    if (p.retStereo && ret != engine::SideSel::Stereo) { p.in[0] = p.in[ret == engine::SideSel::R ? 1 : 0]; p.in[1] = -1; p.retStereo = false; }
    return p;
}
inline host::Pair effectivePair(const DeviceRecord& d, engine::SideSel send, engine::SideSel ret) noexcept { return effectivePair(recordPair(d), send, ret); }

struct CalibRecord
{
    engine::CalibKey key;
    std::string deviceId;
    engine::CalibResult result;
    int version = 0;
};

struct Settings
{
    int maxConcurrentProbes = 0;   // 0 = unlimited (F1 §11)
    bool diagnostics = false;
    bool keepOnlineFallback = false;
    std::string diagnosticsDir;
    bool measureOnOpen = true;   // every instance measures its loop when a project opens; the saved snapshot is the fallback (user 2026-11-06)
    int skin = 0;                // GUI skin, global: 0 Dark / 1 Ivory / 2 Silver; anything else reads as Dark (user 2026-11-06)
    bool allowAnyDriver = false; // test setups only (Dummy Audio, FlexASIO …): an unsupported REAPER driver is let through, labelled
    bool reverseWheel = false;   // SETUP: turn the trim knobs the other way on the mouse wheel (GitHub #1: a scroll utility that
                                 // reverses the mouse leaves macOS's "natural" flag set, so the knob follows the wrong direction)
};

// Channel names the host reports for a record's channels ([0]/[1] = L/R; empty when unknown or mono R) — part of the key, so a
// renamed/remapped interface channel never reuses a calibration (F1 §4).
struct ChannelNames { std::string send[2]; std::string ret[2]; };

// The calibration key of a device record on the current host device (F1 §4; Task 22/23 processor wiring).
engine::CalibKey calibKeyFor(const DeviceRecord& d, const std::string& identIn, const std::string& identOut, int sampleRate,
                             int bufferSize, engine::Mode mode, engine::SideSel send, engine::SideSel ret, const ChannelNames& names);
// LevelPolicy start level: a stored successful calibration is the level history (F1 §8 known-level start).
engine::LevelHistory levelHistoryFrom(const std::optional<CalibRecord>& c);

class DeviceLibrary
{
public:
    static constexpr int kSchema = 1;
    enum class LoadStatus { Ok, Missing, Corrupt, OldSchemaMigrated, NewerSchema, ReadOnly };

    explicit DeviceLibrary(std::filesystem::path dir) : dir_(std::move(dir)) {}
    LoadStatus load();
    const std::vector<DeviceRecord>& devices() const noexcept { return devices_; }
    std::optional<DeviceRecord> device(const std::string& id) const;
    std::optional<DeviceRecord> byParameter(int deviceParam) const;   // 1-based; 0 or out of range -> nullopt
    int indexOf(const std::string& id) const;                          // 1-based device parameter value of a record, 0 = not in the library
    bool upsertDevice(DeviceRecord r);         // false: read-only/newer schema/stale version/IO error (warning added)
    // Device presets edited in the GUI (spec 2026-11-05): validation text ("" = ok), a unique stable id for a new preset,
    // removal, and a cheap re-read when devices.json changed on disk (another instance's save or a hand edit).
    std::string validate(const DeviceRecord& r, int numOuts, int numIns) const;
    std::string newId(const std::string& name) const;
    bool removeDevice(const std::string& id);   // false: unknown id / read-only / newer schema / IO error
    bool reloadIfChanged();                     // true when devices.json was re-read (size or mtime differ from the last read/write)
    std::optional<CalibRecord> calibration(const engine::CalibKey& key) const;
    bool storeCalibration(CalibRecord c);
    Settings settings() const noexcept { return settings_; }
    bool setMeasureOnOpen(bool on);
    bool setSkin(int skin);           // SETUP skin selector: same atomic, keep-other-keys write as setMeasureOnOpen
    bool setReverseWheel(bool on);    // SETUP mouse-wheel direction: same write as setMeasureOnOpen
    void reloadSettings();            // ≤ 1 Hz from the editor poll: picks up measureOnOpen / skin set by another instance; a broken file keeps the last value   // SETUP toggle: settings.json rewritten atomically, other keys kept; false = not written (warning added)
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    const std::filesystem::path& dir() const noexcept { return dir_; }

private:
    bool writeSetting(const char* key, int value, bool asBool, const std::string& what);   // settings.json: atomic, other keys kept
    std::filesystem::path dir_;
    std::vector<DeviceRecord> devices_;
    std::vector<CalibRecord> calibrations_;
    Settings settings_;
    std::vector<std::string> warnings_;
    bool writable_ = true;   // false: newer schema or read-only file
    std::uintmax_t seenSize_ = 0; long long seenMtime_ = 0;   // devices.json as last read or written (reloadIfChanged)
    void noteDevicesFile();
};

} // namespace ni::plugin
