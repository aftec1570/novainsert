#include "plugin/library.hpp"
#include "host/platform.hpp"
#include <algorithm>
#include "plugin/library_json.hpp"

namespace ni::plugin
{
namespace
{
namespace fs = std::filesystem;
using engine::CalibKey;
using engine::CalibResult;

// Exclusive lock on <dir>/.lock for the duration of one read-merge-write (shared by every process using the library).
struct DirLock
{
    host::FileLock lock;
    explicit DirLock(const fs::path& dir) : lock((dir / ".lock").string()) {}
};

enum class Read { Ok, Missing, Corrupt };
Read readJson(const fs::path& p, juce::var& out, std::string& err)
{
    const juce::File f(juce::String(p.string()));
    if (!f.existsAsFile()) return Read::Missing;
    const auto r = juce::JSON::parse(f.loadFileAsString(), out);
    if (r.failed() || !out.isObject()) { err = r.getErrorMessage().toStdString(); return Read::Corrupt; }
    return Read::Ok;
}

// Atomic replace: unique temp file in the same directory, then rename (no partial file is ever visible).
bool writeJson(const fs::path& p, const juce::var& v)
{
    static std::atomic<unsigned> counter { 0 };
    const fs::path tmp = p.parent_path() / (p.filename().string() + ".tmp-" + std::to_string(host::processId()) + "-"
                                            + std::to_string(counter.fetch_add(1)));
    const juce::File t(juce::String(tmp.string()));
    if (!t.replaceWithText(juce::JSON::toString(v))) { std::error_code ec; fs::remove(tmp, ec); return false; }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

bool fileWritable(const fs::path& p) { return !fs::exists(p) || juce::File(juce::String(p.string())).hasWriteAccess(); }

juce::var pair(const int* v, bool stereo)
{
    juce::Array<juce::var> a; a.add(v[0]); if (stereo) a.add(v[1]); return a;
}
void readPair(const juce::var& o, const char* k, int* dst, bool stereo)
{
    dst[0] = dst[1] = -1;
    if (const auto* arr = o.getProperty(k, juce::var()).getArray())
        for (int i = 0; i < 2 && i < arr->size(); ++i) dst[i] = static_cast<int>((*arr)[i]);
    if (!stereo) dst[1] = -1;
}
std::string str(const juce::var& o, const char* k) { return o.getProperty(k, "").toString().toStdString(); }
int skinOf(const juce::var& o) { const int v = static_cast<int>(o.getProperty("skin", 0)); return v >= 0 && v <= 2 ? v : 0; }
int rangedInt(const juce::var& o, const char* key, int hi) { const int v = static_cast<int>(o.getProperty(key, 0)); return v >= 0 && v <= hi ? v : 0; }
float flt(const juce::var& o, const char* k, float d) { return static_cast<float>(static_cast<double>(o.getProperty(k, d))); }
void readFloatPair(const juce::var& o, const char* k, float dst[2])
{
    dst[0] = dst[1] = 0.0f;
    if (const auto* arr = o.getProperty(k, juce::var()).getArray())
        for (int i = 0; i < 2 && i < arr->size(); ++i) dst[i] = sanitizeOffsetDb(static_cast<float>(static_cast<double>((*arr)[i])));
}
juce::var floatPair(const float v[2]) { return juce::Array<juce::var> { v[0], v[1] }; }

DeviceRecord toDevice(const juce::var& v)
{
    DeviceRecord r;
    r.id = str(v, "id"); r.name = str(v, "name");
    const bool legacy = static_cast<bool>(v.getProperty("stereo", true));   // records written before the per-direction widths
    r.sendStereo = static_cast<bool>(v.getProperty("sendStereo", legacy));
    r.returnStereo = static_cast<bool>(v.getProperty("returnStereo", legacy));
    readPair(v, "out", r.out, r.sendStereo); readPair(v, "in", r.in, r.returnStereo);
    r.identIn = str(v, "identIn"); r.identOut = str(v, "identOut");
    r.mode = static_cast<int>(v.getProperty("mode", 0)) == 1 ? engine::Mode::Effect : engine::Mode::Insert;
    r.sendTrimDb = flt(v, "sendTrimDb", 0.0f); r.returnTrimDb = flt(v, "returnTrimDb", 0.0f); r.vuRefDbfs = flt(v, "vuRefDbfs", -20.0f);
    r.version = static_cast<int>(v.getProperty("version", 0));
    r.s3LoopExtra = std::max(0, static_cast<int>(v.getProperty("s3LoopExtra", 0)));
    r.sendDbuAt0dBFS = flt(v, "sendDbuAt0dBFS", 24.0f); r.returnDbuAt0dBFS = flt(v, "returnDbuAt0dBFS", 24.0f);
    if (v.hasProperty("outboardMaxInDbu")) r.outboardMaxInDbu = flt(v, "outboardMaxInDbu", 0.0f);
    r.link = static_cast<bool>(v.getProperty("link", false));
    readFloatPair(v, "sendOffsetDb", r.sendOffsetDb); readFloatPair(v, "returnOffsetDb", r.returnOffsetDb);
    return r;
}
juce::var fromDevice(const DeviceRecord& r)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("id", juce::String(r.id)); o->setProperty("name", juce::String(r.name));
    o->setProperty("sendStereo", r.sendStereo); o->setProperty("returnStereo", r.returnStereo);
    o->setProperty("out", pair(r.out, r.sendStereo)); o->setProperty("in", pair(r.in, r.returnStereo));
    o->setProperty("identIn", juce::String(r.identIn)); o->setProperty("identOut", juce::String(r.identOut));
    o->setProperty("mode", static_cast<int>(r.mode));
    o->setProperty("sendTrimDb", r.sendTrimDb); o->setProperty("returnTrimDb", r.returnTrimDb); o->setProperty("vuRefDbfs", r.vuRefDbfs);
    o->setProperty("version", r.version);
    if (r.s3LoopExtra != 0) o->setProperty("s3LoopExtra", r.s3LoopExtra);
    o->setProperty("sendDbuAt0dBFS", r.sendDbuAt0dBFS); o->setProperty("returnDbuAt0dBFS", r.returnDbuAt0dBFS);
    if (r.outboardMaxInDbu) o->setProperty("outboardMaxInDbu", *r.outboardMaxInDbu);
    o->setProperty("link", r.link);
    o->setProperty("sendOffsetDb", floatPair(r.sendOffsetDb)); o->setProperty("returnOffsetDb", floatPair(r.returnOffsetDb));
    return juce::var(o);
}

juce::var fromKey(const CalibKey& k)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("identIn", juce::String(k.identIn)); o->setProperty("identOut", juce::String(k.identOut));
    o->setProperty("sendIdx", pair(k.sendIdx, true)); o->setProperty("returnIdx", pair(k.returnIdx, true));
    juce::Array<juce::var> sn { juce::String(k.sendName[0]), juce::String(k.sendName[1]) };
    juce::Array<juce::var> rn { juce::String(k.returnName[0]), juce::String(k.returnName[1]) };
    o->setProperty("sendName", sn); o->setProperty("returnName", rn);
    o->setProperty("sampleRate", k.sampleRate); o->setProperty("bufferSize", k.bufferSize);
    o->setProperty("sendStereo", k.sendStereo); o->setProperty("returnStereo", k.returnStereo);
    o->setProperty("send", static_cast<int>(k.send)); o->setProperty("ret", static_cast<int>(k.ret));
    o->setProperty("mode", static_cast<int>(k.mode)); o->setProperty("algorithmVersion", k.algorithmVersion);
    return juce::var(o);
}
CalibKey toKey(const juce::var& v)
{
    CalibKey k;
    k.identIn = str(v, "identIn"); k.identOut = str(v, "identOut");
    readPair(v, "sendIdx", k.sendIdx, true); readPair(v, "returnIdx", k.returnIdx, true);
    for (int i = 0; i < 2; ++i)
    {
        if (const auto* a = v["sendName"].getArray(); a && i < a->size()) k.sendName[i] = (*a)[i].toString().toStdString();
        if (const auto* a = v["returnName"].getArray(); a && i < a->size()) k.returnName[i] = (*a)[i].toString().toStdString();
    }
    k.sampleRate = static_cast<int>(v["sampleRate"]); k.bufferSize = static_cast<int>(v["bufferSize"]);
    k.sendStereo = static_cast<bool>(v["sendStereo"]); k.returnStereo = static_cast<bool>(v["returnStereo"]);
    k.send = static_cast<engine::SideSel>(static_cast<int>(v["send"]));
    k.ret = static_cast<engine::SideSel>(static_cast<int>(v["ret"]));
    k.mode = static_cast<engine::Mode>(static_cast<int>(v["mode"]));
    k.algorithmVersion = static_cast<int>(v["algorithmVersion"]);
    return k;
}
juce::var fromResult(const CalibResult& r)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("physLoop", pair(r.physLoop, true)); o->setProperty("reportedPdc", r.reportedPdc);
    juce::Array<juce::var> fr { r.fractional[0], r.fractional[1] }, neg { r.negativePolarity[0], r.negativePolarity[1] },
                           pbr { r.pbrDb[0], r.pbrDb[1] }, snr { r.snrDb[0], r.snrDb[1] };
    o->setProperty("fractional", fr); o->setProperty("negativePolarity", neg); o->setProperty("pbrDb", pbr); o->setProperty("snrDb", snr);
    o->setProperty("sendPeakDb", r.sendPeakDb); o->setProperty("returnPeakDb", r.returnPeakDb); o->setProperty("sendRmsDb", r.sendRmsDb);
    o->setProperty("returnRmsDb", r.returnRmsDb); o->setProperty("gainPeakDb", r.gainPeakDb);
    o->setProperty("timestampUnix", static_cast<juce::int64>(r.timestampUnix)); o->setProperty("generation", static_cast<int>(r.generation));
    o->setProperty("failReason", static_cast<int>(r.failReason));
    return juce::var(o);
}
CalibResult toResult(const juce::var& v)
{
    CalibResult r;
    readPair(v, "physLoop", r.physLoop, true);
    r.reportedPdc = static_cast<int>(v["reportedPdc"]);
    for (int i = 0; i < 2; ++i)
    {
        auto at = [&](const char* k) { const auto* a = v[k].getArray(); return a && i < a->size() ? (*a)[i] : juce::var(); };
        r.fractional[i] = static_cast<float>(static_cast<double>(at("fractional")));
        r.negativePolarity[i] = static_cast<bool>(at("negativePolarity"));
        r.pbrDb[i] = static_cast<float>(static_cast<double>(at("pbrDb")));
        r.snrDb[i] = static_cast<float>(static_cast<double>(at("snrDb")));
    }
    r.sendPeakDb = flt(v, "sendPeakDb", 0); r.returnPeakDb = flt(v, "returnPeakDb", 0); r.sendRmsDb = flt(v, "sendRmsDb", 0);
    r.returnRmsDb = flt(v, "returnRmsDb", 0); r.gainPeakDb = flt(v, "gainPeakDb", 0);
    r.timestampUnix = static_cast<juce::int64>(v["timestampUnix"]);
    r.generation = static_cast<uint32_t>(static_cast<int>(v["generation"]));
    r.failReason = static_cast<engine::Reason>(static_cast<int>(v["failReason"]));
    return r;
}

juce::var wrap(const char* listKey, const juce::Array<juce::var>& items)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("schema", DeviceLibrary::kSchema);
    o->setProperty(listKey, items);
    return juce::var(o);
}
std::vector<DeviceRecord> devicesOf(const juce::var& root)
{
    std::vector<DeviceRecord> out;
    if (const auto* a = root["devices"].getArray()) for (const auto& v : *a) out.push_back(toDevice(v));
    return out;
}
std::vector<CalibRecord> calibrationsOf(const juce::var& root)
{
    std::vector<CalibRecord> out;
    if (const auto* a = root["calibrations"].getArray())
        for (const auto& v : *a)
            out.push_back({ toKey(v["key"]), str(v, "deviceId"), toResult(v["result"]), static_cast<int>(v.getProperty("version", 0)) });
    return out;
}
juce::var devicesVar(const std::vector<DeviceRecord>& ds)
{
    juce::Array<juce::var> a; for (const auto& d : ds) a.add(fromDevice(d)); return wrap("devices", a);
}
juce::var calibrationsVar(const std::vector<CalibRecord>& cs)
{
    juce::Array<juce::var> a;
    for (const auto& c : cs)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("key", fromKey(c.key)); o->setProperty("deviceId", juce::String(c.deviceId));
        o->setProperty("result", fromResult(c.result)); o->setProperty("version", c.version);
        a.add(juce::var(o));
    }
    return wrap("calibrations", a);
}
} // namespace

bool sameExceptDefaults(const DeviceRecord& a, const DeviceRecord& b)
{
    DeviceRecord x = a, y = b;
    for (auto* r : { &x, &y })
    {
        r->sendTrimDb = r->returnTrimDb = 0.0f; r->link = false; r->version = 0;
        r->sendOffsetDb[0] = r->sendOffsetDb[1] = r->returnOffsetDb[0] = r->returnOffsetDb[1] = 0.0f;
    }
    return x == y;
}

namespace json
{
juce::var fromDevice(const DeviceRecord& r) { return ::ni::plugin::fromDevice(r); }
DeviceRecord toDevice(const juce::var& v) { return ::ni::plugin::toDevice(v); }
juce::var fromCalibration(const CalibRecord& c)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("key", fromKey(c.key)); o->setProperty("deviceId", juce::String(c.deviceId));
    o->setProperty("result", fromResult(c.result)); o->setProperty("version", c.version);
    return juce::var(o);
}
CalibRecord toCalibration(const juce::var& v)
{
    return { toKey(v["key"]), str(v, "deviceId"), toResult(v["result"]), static_cast<int>(v.getProperty("version", 0)) };
}
} // namespace json

DeviceLibrary::LoadStatus DeviceLibrary::load()
{
    devices_.clear(); calibrations_.clear(); warnings_.clear(); settings_ = {}; writable_ = true;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const fs::path devPath = dir_ / "devices.json";

    juce::var s; std::string serr;
    if (readJson(dir_ / "settings.json", s, serr) == Read::Ok)
    {
        settings_.maxConcurrentProbes = std::max(0, static_cast<int>(s.getProperty("maxConcurrentProbes", 0)));
        settings_.diagnostics = static_cast<bool>(s.getProperty("diagnostics", false));
        settings_.keepOnlineFallback = static_cast<bool>(s.getProperty("keepOnlineFallback", false));
        settings_.diagnosticsDir = str(s, "diagnosticsDir");
        settings_.measureOnOpen = static_cast<bool>(s.getProperty("measureOnOpen", true));
        settings_.skin = skinOf(s);
        settings_.reverseWheel = static_cast<bool>(s.getProperty("reverseWheel", false));
        settings_.meterView = rangedInt(s, "meterView", 3);
        settings_.meterUnit = rangedInt(s, "meterUnit", 1);
        settings_.allowAnyDriver = static_cast<bool>(s.getProperty("allowAnyDriver", false));
    }
    juce::var c; std::string cerr;
    const Read cr = readJson(dir_ / "calibrations.json", c, cerr);
    if (cr == Read::Ok) calibrations_ = calibrationsOf(c);
    else if (cr == Read::Corrupt) warnings_.push_back("calibrations.json unreadable: " + cerr + " (stored calibrations ignored)");

    juce::var root; std::string err;
    const Read r = readJson(devPath, root, err);
    if (r == Read::Missing)
    {
        warnings_.push_back("devices.json missing: created an empty library");
        DirLock lock(dir_);
        if (!fs::exists(devPath)) writeJson(devPath, devicesVar({}));
        noteDevicesFile();
        return LoadStatus::Missing;
    }
    if (r == Read::Corrupt || (root.hasProperty("devices") && root["devices"].getArray() == nullptr))
    {
        warnings_.push_back("devices.json unreadable: " + err + " (library ignored)");
        writable_ = false;   // never overwrite a file the user may want to repair
        return LoadStatus::Corrupt;
    }
    devices_ = devicesOf(root);
    noteDevicesFile();
    const int schema = static_cast<int>(root.getProperty("schema", 0));
    if (schema > kSchema)
    {
        writable_ = false;
        warnings_.push_back("devices.json schema " + std::to_string(schema) + " is newer than this NovaInsert: read-only");
        return LoadStatus::NewerSchema;
    }
    if (!fileWritable(devPath))
    {
        writable_ = false;
        warnings_.push_back("devices.json is read-only: changes are kept in memory only");
        return LoadStatus::ReadOnly;
    }
    if (schema < kSchema)
    {
        DirLock lock(dir_);
        if (!writeJson(devPath, devicesVar(devices_))) warnings_.push_back("devices.json migration could not be written");
        noteDevicesFile();
        return LoadStatus::OldSchemaMigrated;
    }
    return LoadStatus::Ok;
}

std::optional<DeviceRecord> DeviceLibrary::device(const std::string& id) const
{
    for (const auto& d : devices_) if (d.id == id) return d;
    return std::nullopt;
}

std::optional<DeviceRecord> DeviceLibrary::byParameter(int deviceParam) const
{
    if (deviceParam < 1 || deviceParam > static_cast<int>(devices_.size())) return std::nullopt;
    return devices_[static_cast<std::size_t>(deviceParam - 1)];
}

int DeviceLibrary::indexOf(const std::string& id) const
{
    for (std::size_t i = 0; i < devices_.size(); ++i)
        if (devices_[i].id == id) return static_cast<int>(i) + 1;
    return 0;
}

void DeviceLibrary::reloadSettings()
{
    juce::var s; std::string err;
    if (readJson(dir_ / "settings.json", s, err) != Read::Ok) return;
    settings_.measureOnOpen = static_cast<bool>(s.getProperty("measureOnOpen", true));
    settings_.skin = skinOf(s);
    settings_.reverseWheel = static_cast<bool>(s.getProperty("reverseWheel", false));
    settings_.meterView = rangedInt(s, "meterView", 3);
    settings_.meterUnit = rangedInt(s, "meterUnit", 1);
}

bool DeviceLibrary::writeSetting(const char* key, int value, bool asBool, const std::string& what)
{
    const fs::path p = dir_ / "settings.json";
    juce::var s; std::string err;
    const Read r = readJson(p, s, err);
    if (r == Read::Corrupt) { warnings_.push_back("settings.json unreadable: " + err + " (" + what + " not saved)"); return false; }
    if (r == Read::Missing) s = juce::var(new juce::DynamicObject());
    if (!fileWritable(p)) { warnings_.push_back("settings.json is read-only (" + what + " not saved)"); return false; }
    s.getDynamicObject()->setProperty(key, asBool ? juce::var(value != 0) : juce::var(value));
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (!writeJson(p, s)) { warnings_.push_back("settings.json not written (" + what + " not saved)"); return false; }
    return true;
}

bool DeviceLibrary::setMeasureOnOpen(bool on)
{
    if (!writeSetting("measureOnOpen", on ? 1 : 0, true, "measure on open")) return false;
    settings_.measureOnOpen = on;
    return true;
}

bool DeviceLibrary::setReverseWheel(bool on)
{
    if (!writeSetting("reverseWheel", on ? 1 : 0, true, "mouse wheel direction")) return false;
    settings_.reverseWheel = on;
    return true;
}

bool DeviceLibrary::setMeterView(int view)
{
    if (view < 0 || view > 3) view = 0;
    if (!writeSetting("meterView", view, false, "meter view")) return false;
    settings_.meterView = view;
    return true;
}

bool DeviceLibrary::setMeterUnit(int unit)
{
    if (unit < 0 || unit > 1) unit = 0;
    if (!writeSetting("meterUnit", unit, false, "meter unit")) return false;
    settings_.meterUnit = unit;
    return true;
}

bool DeviceLibrary::setSkin(int skin)
{
    if (skin < 0 || skin > 2) skin = 0;
    if (!writeSetting("skin", skin, false, "skin")) return false;
    settings_.skin = skin;
    return true;
}

bool DeviceLibrary::upsertDevice(DeviceRecord r)
{
    const fs::path p = dir_ / "devices.json";
    if (!writable_ || !fileWritable(p)) { warnings_.push_back("device \"" + r.name + "\" not saved: library is read-only"); return false; }
    DirLock lock(dir_);
    juce::var root; std::string err;
    std::vector<DeviceRecord> disk;
    const Read rd = readJson(p, root, err);
    if (rd == Read::Corrupt) { warnings_.push_back("device not saved: devices.json became unreadable"); return false; }
    if (rd == Read::Ok)
    {
        if (static_cast<int>(root.getProperty("schema", 0)) > kSchema) { warnings_.push_back("device not saved: newer schema on disk"); return false; }
        disk = devicesOf(root);
    }
    auto it = std::find_if(disk.begin(), disk.end(), [&](const DeviceRecord& d) { return d.id == r.id; });
    if (it != disk.end() && it->version > r.version)
    {
        warnings_.push_back("device \"" + r.name + "\" not saved: it was changed elsewhere (version " + std::to_string(it->version) + ")");
        devices_ = std::move(disk);
        return false;
    }
    r.version = (it != disk.end() ? it->version : r.version) + 1;
    if (it != disk.end()) *it = r; else disk.push_back(r);
    if (!writeJson(p, devicesVar(disk))) { warnings_.push_back("device not saved: write failed"); return false; }
    devices_ = std::move(disk);
    noteDevicesFile();
    return true;
}

void DeviceLibrary::noteDevicesFile()
{
    std::error_code ec;
    const fs::path p = dir_ / "devices.json";
    seenSize_ = fs::file_size(p, ec); if (ec) seenSize_ = 0;
    const auto t = fs::last_write_time(p, ec);
    seenMtime_ = ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
}

bool DeviceLibrary::reloadIfChanged()
{
    std::error_code ec;
    const fs::path p = dir_ / "devices.json";
    const auto size = fs::file_size(p, ec); if (ec) return false;
    const auto t = fs::last_write_time(p, ec); if (ec) return false;
    if (size == seenSize_ && static_cast<long long>(t.time_since_epoch().count()) == seenMtime_) return false;
    juce::var root; std::string err;
    if (readJson(p, root, err) != Read::Ok || (root.hasProperty("devices") && root["devices"].getArray() == nullptr)) return false;   // half-written or corrupt: keep what we have
    if (static_cast<int>(root.getProperty("schema", 0)) > kSchema) return false;
    devices_ = devicesOf(root);
    noteDevicesFile();
    return true;
}

bool DeviceLibrary::removeDevice(const std::string& id)
{
    const fs::path p = dir_ / "devices.json";
    if (!writable_ || !fileWritable(p)) { warnings_.push_back("device not deleted: library is read-only"); return false; }
    DirLock lock(dir_);
    juce::var root; std::string err;
    if (readJson(p, root, err) != Read::Ok) { warnings_.push_back("device not deleted: devices.json unreadable"); return false; }
    if (static_cast<int>(root.getProperty("schema", 0)) > kSchema) { warnings_.push_back("device not deleted: newer schema on disk"); return false; }
    auto disk = devicesOf(root);
    const auto n = disk.size();
    disk.erase(std::remove_if(disk.begin(), disk.end(), [&](const DeviceRecord& d) { return d.id == id; }), disk.end());
    if (disk.size() == n) return false;
    if (!writeJson(p, devicesVar(disk))) { warnings_.push_back("device not deleted: write failed"); return false; }
    devices_ = std::move(disk);
    noteDevicesFile();
    return true;
}

namespace
{
std::string trimmed(const std::string& s)
{
    const auto a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string foldName(const std::string& s)   // comparison form: trimmed, ASCII lower-case
{
    std::string t = trimmed(s);
    for (auto& ch : t) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return t;
}
} // namespace

std::string DeviceLibrary::validate(const DeviceRecord& r, int numOuts, int numIns) const
{
    const std::string name = foldName(r.name);
    if (name.empty()) return "Name is empty";
    for (const auto& d : devices_) if (d.id != r.id && foldName(d.name) == name) return "Name already used";
    for (int c = 0; c < (r.sendStereo ? 2 : 1); ++c) if (r.out[c] < 0 || r.out[c] >= numOuts) return "Choose the output channels";
    for (int c = 0; c < (r.returnStereo ? 2 : 1); ++c) if (r.in[c] < 0 || r.in[c] >= numIns) return "Choose the input channels";
    if ((r.sendStereo && r.out[0] == r.out[1]) || (r.returnStereo && r.in[0] == r.in[1])) return "L and R must differ";
    return {};
}

std::string DeviceLibrary::newId(const std::string& name) const
{
    std::string slug; bool gap = false;
    for (const unsigned char ch : name)
    {
        const bool alnum = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (alnum) { if (gap && !slug.empty()) slug += '-'; gap = false; slug += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch); }
        else gap = true;
    }
    if (slug.empty()) slug = "device";
    auto taken = [&](const std::string& id) { for (const auto& d : devices_) if (d.id == id) return true; return false; };
    if (!taken(slug)) return slug;
    for (int i = 2;; ++i) { const std::string c = slug + "-" + std::to_string(i); if (!taken(c)) return c; }
}

engine::CalibKey calibKeyFor(const DeviceRecord& d, const std::string& identIn, const std::string& identOut, int sampleRate,
                             int bufferSize, engine::Mode mode, engine::SideSel send, engine::SideSel ret, const ChannelNames& names)
{
    CalibKey k;
    k.identIn = identIn; k.identOut = identOut;
    for (int c = 0; c < 2; ++c)
    {
        k.sendIdx[c] = d.out[c]; k.returnIdx[c] = d.in[c];
        k.sendName[c] = names.send[c]; k.returnName[c] = names.ret[c];
    }
    k.sampleRate = sampleRate; k.bufferSize = bufferSize; k.sendStereo = d.sendStereo; k.returnStereo = d.returnStereo; k.mode = mode;
    k.send = send; k.ret = ret;
    k.algorithmVersion = engine::kAlgorithmVersion;
    return k;
}

engine::LevelHistory levelHistoryFrom(const std::optional<CalibRecord>& c)
{
    if (!c || c->result.failReason != engine::Reason::None) return { false, 0.0f, 0.0f };
    return { true, c->result.returnPeakDb, c->result.gainPeakDb };
}

std::optional<CalibRecord> DeviceLibrary::calibration(const CalibKey& key) const
{
    for (const auto& c : calibrations_) if (c.key == key) return c;
    return std::nullopt;
}

bool DeviceLibrary::storeCalibration(CalibRecord c)
{
    const fs::path p = dir_ / "calibrations.json";
    if (!writable_ || !fileWritable(p)) { warnings_.push_back("calibration not saved: library is read-only"); return false; }
    DirLock lock(dir_);
    juce::var root; std::string err;
    std::vector<CalibRecord> disk;
    const Read rd = readJson(p, root, err);
    if (rd == Read::Corrupt) { warnings_.push_back("calibration not saved: calibrations.json unreadable"); return false; }
    if (rd == Read::Ok) disk = calibrationsOf(root);
    auto it = std::find_if(disk.begin(), disk.end(), [&](const CalibRecord& d) { return d.key == c.key; });
    c.version = (it != disk.end() ? it->version : 0) + 1;
    if (it != disk.end()) *it = c; else disk.push_back(c);
    if (!writeJson(p, calibrationsVar(disk))) { warnings_.push_back("calibration not saved: write failed"); return false; }
    calibrations_ = std::move(disk);
    return true;
}

} // namespace ni::plugin
