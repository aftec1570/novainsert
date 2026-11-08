#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "plugin/library.hpp"

using namespace ni::plugin;
namespace fs = std::filesystem;

namespace
{
fs::path tempDir(const char* tag)
{
    const fs::path d = fs::temp_directory_path() / (std::string("novainsert-library-") + tag);
    fs::remove_all(d); fs::create_directories(d); return d;
}
void writeFile(const fs::path& p, const char* s) { std::ofstream o(p); o << s; }
}

TEST_CASE("library: missing devices.json -> Missing, no devices, no throw")
{
    DeviceLibrary lib(tempDir("missing"));
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Missing);
    REQUIRE(lib.devices().empty());
    REQUIRE_FALSE(lib.byParameter(1).has_value());
}

TEST_CASE("library: devices.json records are read in file order and addressed 1-based by the device parameter")
{
    const auto d = tempDir("two");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[
      {"id":"bh-0","name":"BH pair 0","stereo":true,"out":[8,9],"in":[8,9]},
      {"id":"bh-1","name":"BH pair 1","stereo":false,"out":[10],"in":[10]}]})");
    DeviceLibrary lib(d);
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE(lib.devices().size() == 2);
    const auto a = lib.byParameter(1); REQUIRE(a.has_value());
    REQUIRE(a->id == "bh-0"); REQUIRE(a->name == "BH pair 0"); REQUIRE(a->sendStereo); REQUIRE(a->returnStereo);   // legacy "stereo" loads both widths
    REQUIRE(a->out[0] == 8); REQUIRE(a->out[1] == 9); REQUIRE(a->in[0] == 8); REQUIRE(a->in[1] == 9);
    const auto b = lib.byParameter(2); REQUIRE(b.has_value());
    REQUIRE_FALSE(b->sendStereo); REQUIRE_FALSE(b->returnStereo); REQUIRE(b->out[0] == 10); REQUIRE(b->out[1] == -1); REQUIRE(b->in[1] == -1);
    REQUIRE_FALSE(lib.byParameter(0).has_value());   // 0 = none
    REQUIRE_FALSE(lib.byParameter(3).has_value());
}

TEST_CASE("library: corrupt devices.json -> Corrupt, empty, warning text, no throw")
{
    const auto d = tempDir("corrupt");
    writeFile(d / "devices.json", "{this is not json");
    DeviceLibrary lib(d);
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Corrupt);
    REQUIRE(lib.devices().empty());
    REQUIRE_FALSE(lib.warnings().empty());
}

TEST_CASE("library: s3LoopExtra (temporary until the Task 16 Measurer) is read per record, default 0, negative clamped to 0")
{
    const auto d = tempDir("extra");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[
      {"id":"pultec","name":"Pultec","stereo":false,"out":[11],"in":[9],"s3LoopExtra":346},
      {"id":"plain","name":"Plain","stereo":true,"out":[8,9],"in":[8,9]},
      {"id":"neg","name":"Neg","stereo":true,"out":[8,9],"in":[8,9],"s3LoopExtra":-5}]})");
    DeviceLibrary lib(d);
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE(lib.byParameter(1)->s3LoopExtra == 346);
    REQUIRE(lib.byParameter(2)->s3LoopExtra == 0);
    REQUIRE(lib.byParameter(3)->s3LoopExtra == 0);
}

// ---- plan Task 23: schema migration, atomic + locked writes, calibration store, settings ----
#include <sys/stat.h>
#include <juce_core/juce_core.h>

namespace
{
std::string readFile(const fs::path& p) { std::ifstream i(p); return std::string((std::istreambuf_iterator<char>(i)), {}); }
DeviceRecord rec(const char* id, const char* name)
{
    DeviceRecord r; r.id = id; r.name = name; r.sendStereo = r.returnStereo = true; r.out[0] = 2; r.out[1] = 3; r.in[0] = 2; r.in[1] = 3;
    r.identIn = r.identOut = "Apollo x8p"; return r;
}
ni::engine::CalibKey key(int32_t buffer)
{
    ni::engine::CalibKey k; k.identIn = k.identOut = "Apollo x8p"; k.sendIdx[0] = 2; k.sendIdx[1] = 3; k.returnIdx[0] = 2;
    k.returnIdx[1] = 3; k.sendName[0] = "Line 3"; k.returnName[0] = "Line 3"; k.sampleRate = 48000; k.bufferSize = buffer;
    k.sendStereo = k.returnStereo = true; k.algorithmVersion = 1; return k;
}
}

TEST_CASE("library_missing_files_create_empty_and_warn")
{
    const auto d = tempDir("create");
    DeviceLibrary lib(d);
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Missing);
    REQUIRE_FALSE(lib.warnings().empty());
    REQUIRE(fs::exists(d / "devices.json"));
    juce::var v;
    REQUIRE(juce::JSON::parse(juce::String(readFile(d / "devices.json")), v).wasOk());
    REQUIRE(static_cast<int>(v["schema"]) == 1);
    DeviceLibrary again(d);
    REQUIRE(again.load() == DeviceLibrary::LoadStatus::Ok);
}

TEST_CASE("library_old_schema_0_migrated_and_rewritten")
{
    const auto d = tempDir("old");
    writeFile(d / "devices.json", R"({"devices":[{"id":"a","name":"A","stereo":false,"out":[4],"in":[4]}]})");
    DeviceLibrary lib(d);
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::OldSchemaMigrated);
    REQUIRE(lib.devices().size() == 1);
    juce::var v;
    REQUIRE(juce::JSON::parse(juce::String(readFile(d / "devices.json")), v).wasOk());
    REQUIRE(static_cast<int>(v["schema"]) == 1);
    REQUIRE(v["devices"].getArray()->size() == 1);
}

TEST_CASE("library_newer_schema_2_read_only_never_rewritten")
{
    const auto d = tempDir("newer");
    const char* text = R"({"schema":2,"devices":[{"id":"a","name":"A","stereo":false,"out":[4],"in":[4]}]})";
    writeFile(d / "devices.json", text);
    DeviceLibrary lib(d);
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::NewerSchema);
    REQUIRE(lib.devices().size() == 1);
    REQUIRE_FALSE(lib.upsertDevice(rec("b", "B")));
    REQUIRE(readFile(d / "devices.json") == text);
    REQUIRE_FALSE(lib.warnings().empty());
}

TEST_CASE("library_readonly_file_warns_and_runs")
{
    const auto d = tempDir("ro");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[{"id":"a","name":"A","stereo":false,"out":[4],"in":[4]}]})");
    ::chmod((d / "devices.json").c_str(), 0444);
    DeviceLibrary lib(d);
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::ReadOnly);
    REQUIRE_FALSE(lib.upsertDevice(rec("b", "B")));
    REQUIRE_FALSE(lib.warnings().empty());
    REQUIRE(lib.devices().size() == 1);
    ::chmod((d / "devices.json").c_str(), 0644);
}

TEST_CASE("library_concurrent_writers_keep_both_records")
{
    const auto d = tempDir("concurrent");
    DeviceLibrary a(d), b(d);
    a.load(); b.load();
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(a.upsertDevice(rec(("a" + std::to_string(i)).c_str(), "A")));
        REQUIRE(b.upsertDevice(rec(("b" + std::to_string(i)).c_str(), "B")));
    }
    DeviceLibrary c(d);
    REQUIRE(c.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE(c.devices().size() == 20);
}

TEST_CASE("library_stale_version_upsert_rejected")
{
    const auto d = tempDir("version");
    DeviceLibrary a(d), b(d);
    a.load();
    REQUIRE(a.upsertDevice(rec("x", "First")));
    b.load();
    auto fromB = *b.device("x");
    auto fromA = *a.device("x");
    fromA.name = "A edit";
    REQUIRE(a.upsertDevice(fromA));
    fromB.name = "B edit (stale)";
    REQUIRE_FALSE(b.upsertDevice(fromB));
    DeviceLibrary c(d);
    c.load();
    REQUIRE(c.device("x")->name == "A edit");
}

TEST_CASE("library_atomic_write_no_partial_file")
{
    const auto d = tempDir("atomic");
    DeviceLibrary lib(d);
    lib.load();
    for (int i = 0; i < 100; ++i)
    {
        const std::string id = "d" + std::to_string(i % 7);
        DeviceRecord r = rec(id.c_str(), "N");
        if (const auto cur = lib.device(id)) r.version = cur->version;   // edit of the current record
        REQUIRE(lib.upsertDevice(r));
    }
    for (const auto& e : fs::directory_iterator(d)) REQUIRE(e.path().string().find(".tmp") == std::string::npos);
    juce::var v;
    REQUIRE(juce::JSON::parse(juce::String(readFile(d / "devices.json")), v).wasOk());
    REQUIRE(v["devices"].getArray()->size() == 7);
}

TEST_CASE("library_device_record_round_trips_all_fields")
{
    const auto d = tempDir("fields");
    DeviceLibrary lib(d);
    lib.load();
    DeviceRecord r = rec("p", "Pultec");
    r.sendStereo = r.returnStereo = false; r.out[1] = r.in[1] = -1; r.mode = ni::engine::Mode::Effect; r.sendTrimDb = -3.5f; r.returnTrimDb = 2.0f;
    r.vuRefDbfs = -18.0f;
    REQUIRE(lib.upsertDevice(r));
    DeviceLibrary back(d);
    back.load();
    const auto g = *back.device("p");
    REQUIRE(g.name == "Pultec"); REQUIRE_FALSE(g.sendStereo); REQUIRE_FALSE(g.returnStereo); REQUIRE(g.out[0] == 2); REQUIRE(g.out[1] == -1);
    REQUIRE(g.identIn == "Apollo x8p"); REQUIRE(g.mode == ni::engine::Mode::Effect);
    REQUIRE(g.sendTrimDb == -3.5f); REQUIRE(g.returnTrimDb == 2.0f); REQUIRE(g.vuRefDbfs == -18.0f);
    REQUIRE(g.version == 1);
}

TEST_CASE("calibration_keyed_exactly_and_rename_does_not_change_key")
{
    const auto d = tempDir("calib");
    DeviceLibrary lib(d);
    lib.load();
    REQUIRE(lib.upsertDevice(rec("x", "Old name")));
    CalibRecord c; c.key = key(128); c.deviceId = "x"; c.result.physLoop[0] = 300; c.result.physLoop[1] = 300;
    c.result.reportedPdc = 556; c.result.gainPeakDb = 1.5f; c.result.timestampUnix = 1761800000;
    REQUIRE(lib.storeCalibration(c));
    REQUIRE_FALSE(lib.calibration(key(256)).has_value());   // buffer size is part of the key
    auto renamed = *lib.device("x");
    renamed.name = "New name";
    REQUIRE(lib.upsertDevice(renamed));
    DeviceLibrary back(d);
    back.load();
    const auto got = back.calibration(key(128));
    REQUIRE(got.has_value());
    REQUIRE(got->deviceId == "x");
    REQUIRE(got->result.physLoop[0] == 300);
    REQUIRE(got->result.reportedPdc == 556);
    REQUIRE(got->result.gainPeakDb == 1.5f);
    REQUIRE(got->result.timestampUnix == 1761800000);
    c.result.physLoop[0] = 301;   // re-store under the same key replaces
    REQUIRE(back.storeCalibration(c));
    REQUIRE(back.calibration(key(128))->result.physLoop[0] == 301);
}

TEST_CASE("settings_defaults_and_parse")
{
    const auto d = tempDir("settings");
    DeviceLibrary lib(d);
    lib.load();
    Settings s = lib.settings();
    REQUIRE(s.maxConcurrentProbes == 0); REQUIRE_FALSE(s.diagnostics); REQUIRE_FALSE(s.keepOnlineFallback); REQUIRE(s.diagnosticsDir.empty());
    writeFile(d / "settings.json", R"({"maxConcurrentProbes":1,"diagnostics":true,"keepOnlineFallback":true,"diagnosticsDir":"/x"})");
    lib.load();
    s = lib.settings();
    REQUIRE(s.maxConcurrentProbes == 1); REQUIRE(s.diagnostics); REQUIRE(s.keepOnlineFallback); REQUIRE(s.diagnosticsDir == "/x");
}

TEST_CASE("settings_measure_on_open_defaults_on_and_persists")
{
    const auto d = tempDir("settings_open");
    DeviceLibrary lib(d);
    lib.load();
    REQUIRE(lib.settings().measureOnOpen);   // absent file → ON (user 2026-11-06)
    writeFile(d / "settings.json", R"({"maxConcurrentProbes":2,"diagnostics":true,"measureOnOpen":false})");
    lib.load();
    REQUIRE_FALSE(lib.settings().measureOnOpen);
    REQUIRE(lib.setMeasureOnOpen(true));     // the SETUP toggle: written atomically, other keys kept
    REQUIRE(lib.settings().measureOnOpen);
    DeviceLibrary back(d);
    back.load();
    REQUIRE(back.settings().measureOnOpen);
    REQUIRE(back.settings().maxConcurrentProbes == 2);
    REQUIRE(back.settings().diagnostics);
    REQUIRE(back.setMeasureOnOpen(false));
    DeviceLibrary again(d);
    again.load();
    REQUIRE_FALSE(again.settings().measureOnOpen);
}

TEST_CASE("settings_measure_on_open_seen_by_another_open_instance")
{
    // one SETUP toggle is global: an instance already open picks it up on its next library poll
    const auto d = tempDir("settings_shared");
    DeviceLibrary a(d), b(d);
    a.load(); b.load();
    REQUIRE(a.setMeasureOnOpen(false));
    REQUIRE(b.settings().measureOnOpen);
    b.reloadSettings();
    REQUIRE_FALSE(b.settings().measureOnOpen);
    writeFile(d / "settings.json", "{ broken");
    b.reloadSettings();                       // a half-written or broken file keeps the last value
    REQUIRE_FALSE(b.settings().measureOnOpen);
}

TEST_CASE("settings_skin_defaults_dark_persists_and_is_shared")
{
    // GUI skin (user 2026-11-06 "Put Ivory and Silver in the plugin too"): global, settings.json "skin" 0 Dark / 1 Ivory / 2 Silver
    const auto d = tempDir("settings_skin");
    DeviceLibrary a(d), b(d);
    a.load(); b.load();
    REQUIRE(a.settings().skin == 0);           // absent file → Dark
    REQUIRE(a.setSkin(2));
    REQUIRE(a.settings().skin == 2);
    REQUIRE(a.setMeasureOnOpen(false));        // other keys kept both ways
    b.reloadSettings();
    REQUIRE(b.settings().skin == 2);
    REQUIRE_FALSE(b.settings().measureOnOpen);
    DeviceLibrary c(d);
    c.load();
    REQUIRE(c.settings().skin == 2);
    writeFile(d / "settings.json", R"({"skin":7})");
    c.load();
    REQUIRE(c.settings().skin == 0);           // unknown value → Dark
    writeFile(d / "settings.json", R"({"skin":-1})");
    c.reloadSettings();
    REQUIRE(c.settings().skin == 0);
    writeFile(d / "settings.json", "{ broken");
    REQUIRE_FALSE(c.setSkin(1));               // a corrupt file is never overwritten
    REQUIRE(c.settings().skin == 0);
}

TEST_CASE("calib_key_for_assembles_device_host_and_channel_fields")
{
    DeviceRecord d; d.id = "dev-1"; d.sendStereo = d.returnStereo = true; d.out[0] = 85; d.out[1] = 86; d.in[0] = 83; d.in[1] = 84;
    const ChannelNames names { { "Out 86", "Out 87" }, { "In 84", "In 85" } };
    const auto k = calibKeyFor (d, "UA Thunderbolt in", "UA Thunderbolt out", 48000, 64, ni::engine::Mode::Insert, ni::engine::SideSel::Stereo, ni::engine::SideSel::Stereo, names);
    REQUIRE (k.identIn == "UA Thunderbolt in");
    REQUIRE (k.identOut == "UA Thunderbolt out");
    REQUIRE (k.sendIdx[0] == 85); REQUIRE (k.sendIdx[1] == 86);
    REQUIRE (k.returnIdx[0] == 83); REQUIRE (k.returnIdx[1] == 84);
    REQUIRE (k.sendName[1] == "Out 87"); REQUIRE (k.returnName[0] == "In 84");
    REQUIRE (k.sampleRate == 48000); REQUIRE (k.bufferSize == 64);
    REQUIRE (k.sendStereo); REQUIRE (k.returnStereo);
    REQUIRE (k.mode == ni::engine::Mode::Insert);
    REQUIRE (k.algorithmVersion == ni::engine::kAlgorithmVersion);
    REQUIRE (ni::engine::kAlgorithmVersion >= 1);
    // a different buffer size is a different calibration (F1 §4)
    REQUIRE_FALSE (calibKeyFor (d, "UA Thunderbolt in", "UA Thunderbolt out", 48000, 128, ni::engine::Mode::Insert, ni::engine::SideSel::Stereo, ni::engine::SideSel::Stereo, names) == k);
    DeviceRecord mono = d; mono.sendStereo = mono.returnStereo = false; mono.out[1] = -1; mono.in[1] = -1;
    const auto km = calibKeyFor (mono, "a", "b", 48000, 64, ni::engine::Mode::Insert, ni::engine::SideSel::Stereo, ni::engine::SideSel::Stereo, {});
    REQUIRE_FALSE (km.sendStereo); REQUIRE_FALSE (km.returnStereo); REQUIRE (km.sendIdx[1] == -1); REQUIRE (km.sendName[0].empty());
}

TEST_CASE("calib_key_for_carries_the_routing_mode")
{
    // F1 §4: the side selectors (Stereo / L / R per direction) are key fields: a send=R calibration is never reused for send=L
    DeviceRecord d; d.id = "dev-1"; d.out[0] = 11; d.in[0] = 9;
    const auto k = calibKeyFor (d, "a", "b", 48000, 64, ni::engine::Mode::Insert, ni::engine::SideSel::R, ni::engine::SideSel::L, {});
    REQUIRE (k.send == ni::engine::SideSel::R);
    REQUIRE (k.ret == ni::engine::SideSel::L);
    REQUIRE_FALSE (calibKeyFor (d, "a", "b", 48000, 64, ni::engine::Mode::Insert, ni::engine::SideSel::L, ni::engine::SideSel::L, {}) == k);
    REQUIRE_FALSE (calibKeyFor (d, "a", "b", 48000, 64, ni::engine::Mode::Insert, ni::engine::SideSel::R, ni::engine::SideSel::Stereo, {}) == k);
}

TEST_CASE("level_history_from_a_successful_calibration_is_known")
{
    REQUIRE_FALSE (levelHistoryFrom (std::nullopt).known);
    CalibRecord c; c.result.returnPeakDb = -14.5f; c.result.gainPeakDb = 2.0f;
    const auto h = levelHistoryFrom (c);
    REQUIRE (h.known); REQUIRE (h.lastReturnPeakDb == -14.5f); REQUIRE (h.gainPeakDb == 2.0f);
    c.result.failReason = ni::engine::Reason::OutOfRange;   // a failed result says nothing about the level
    REQUIRE_FALSE (levelHistoryFrom (c).known);
}

// ---- VU GUI plan Task 4: converter calibration, LINK and per-channel offsets -------------------------------------------------
TEST_CASE("library_old_record_loads_with_level_cal_defaults")
{
    const auto d = tempDir("calold");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[{"id":"a","name":"A","stereo":true,"out":[2,3],"in":[2,3]}]})");
    DeviceLibrary lib(d);
    lib.load();
    const auto g = *lib.device("a");
    REQUIRE(g.sendDbuAt0dBFS == 24.0f); REQUIRE(g.returnDbuAt0dBFS == 24.0f);
    REQUIRE_FALSE(g.outboardMaxInDbu.has_value()); REQUIRE_FALSE(g.link);
    REQUIRE(g.sendOffsetDb[0] == 0.0f); REQUIRE(g.sendOffsetDb[1] == 0.0f);
    REQUIRE(g.returnOffsetDb[0] == 0.0f); REQUIRE(g.returnOffsetDb[1] == 0.0f);
}

TEST_CASE("library_level_cal_fields_round_trip_and_schema_stays_1")
{
    const auto d = tempDir("calrt");
    DeviceLibrary lib(d);
    lib.load();
    DeviceRecord r = rec("s", "SSL EQ stereo");
    r.sendDbuAt0dBFS = 20.2f; r.returnDbuAt0dBFS = 22.0f; r.outboardMaxInDbu = 18.0f; r.link = true;
    r.sendOffsetDb[0] = -0.4f; r.sendOffsetDb[1] = 0.3f; r.returnOffsetDb[0] = 1.1f; r.returnOffsetDb[1] = -1.1f;
    REQUIRE(lib.upsertDevice(r));
    DeviceLibrary back(d);
    back.load();
    auto g = *back.device("s");
    r.version = g.version;
    REQUIRE(g == r);
    juce::var v;
    REQUIRE(juce::JSON::parse(juce::String(readFile(d / "devices.json")), v).wasOk());
    REQUIRE(static_cast<int>(v["schema"]) == 1);
}

TEST_CASE("library_same_except_defaults")
{
    DeviceRecord a = rec("s", "S"), b = a;
    b.sendTrimDb = 3.8f; b.returnTrimDb = -3.0f; b.link = true; b.returnOffsetDb[1] = 0.5f; b.sendOffsetDb[0] = 0.2f; b.version = 9;
    REQUIRE(sameExceptDefaults(a, b));
    b.vuRefDbfs = -18.0f;
    REQUIRE_FALSE(sameExceptDefaults(a, b));
    b = a; b.sendDbuAt0dBFS = 20.2f;
    REQUIRE_FALSE(sameExceptDefaults(a, b));
}

TEST_CASE("library_offsets_out_of_range_are_clamped_on_load")
{
    const auto d = tempDir("caloff");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[{"id":"a","name":"A","stereo":true,"out":[2,3],"in":[2,3],"sendOffsetDb":[500,-500],"returnOffsetDb":[1e30,-7]}]})");
    DeviceLibrary lib(d);
    lib.load();
    const auto g = *lib.device("a");
    REQUIRE(g.sendOffsetDb[0] == kOffsetMaxDb); REQUIRE(g.sendOffsetDb[1] == kOffsetMinDb);
    REQUIRE(g.returnOffsetDb[0] == kOffsetMaxDb); REQUIRE(g.returnOffsetDb[1] == -7.0f);
    REQUIRE(sanitizeOffsetDb(std::nanf("")) == 0.0f);
    REQUIRE(sanitizeOffsetDb(INFINITY) == 0.0f);
}

TEST_CASE("library: a legacy infraLatency property loads and is dropped on the next write (Effect PDC spec 2026-11-04 §3)")
{
    const auto d = tempDir("legacy-infra");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[
      {"id":"fx","name":"Lexicon","stereo":true,"out":[8,9],"in":[8,9],"mode":1,"infraLatency":123,"infraOrigin":"MEASURED"}]})");
    DeviceLibrary lib(d);
    REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Ok);
    auto r = *lib.device("fx");
    REQUIRE(r.mode == ni::engine::Mode::Effect);
    r.name = "Lexicon 224";
    REQUIRE(lib.upsertDevice(r));
    std::ifstream in(d / "devices.json"); std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(s.find("infraLatency") == std::string::npos);
    REQUIRE(s.find("infraOrigin") == std::string::npos);
    REQUIRE(s.find("Lexicon 224") != std::string::npos);
}

// --- device presets in the GUI (spec 2026-11-05) -------------------------------------------------------------------------

TEST_CASE("library_validate_rules")
{
    const auto d = tempDir("validate");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[{"id":"pultec","name":"Pultec","stereo":true,"out":[2,3],"in":[2,3]}]})");
    DeviceLibrary lib(d); REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Ok);
    DeviceRecord r; r.id = "new"; r.name = "SSL"; r.sendStereo = r.returnStereo = true; r.out[0] = 4; r.out[1] = 5; r.in[0] = 4; r.in[1] = 5;
    REQUIRE(lib.validate(r, 8, 8).empty());
    auto t = r; t.name = "   "; REQUIRE(lib.validate(t, 8, 8) == "Name is empty");
    t = r; t.name = " pultec "; REQUIRE(lib.validate(t, 8, 8) == "Name already used");   // case/space-insensitive, other id
    t = r; t.id = "pultec"; t.name = "Pultec"; t.out[0] = 2; t.out[1] = 3; t.in[0] = 2; t.in[1] = 3; REQUIRE(lib.validate(t, 8, 8).empty());   // its own name
    t = r; t.out[1] = 8; REQUIRE(lib.validate(t, 8, 8) == "Choose the output channels");
    t = r; t.in[0] = -1; REQUIRE(lib.validate(t, 8, 8) == "Choose the input channels");
    t = r; t.out[1] = 4; REQUIRE(lib.validate(t, 8, 8) == "L and R must differ");
    t = r; t.in[1] = 4; REQUIRE(lib.validate(t, 8, 8) == "L and R must differ");
    t = r; t.sendStereo = t.returnStereo = false; t.out[1] = -1; t.in[1] = 99; REQUIRE(lib.validate(t, 8, 8).empty());   // mono ignores [1]
    t = r; t.sendStereo = false; t.in[1] = 4; REQUIRE(lib.validate(t, 8, 8) == "L and R must differ");   // the stereo return still checks
    t = r; t.sendStereo = false; t.out[0] = 8; REQUIRE(lib.validate(t, 8, 8) == "Choose the output channels");
}

TEST_CASE("library_new_id_slug_unique_stable")
{
    const auto d = tempDir("newid");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[{"id":"pultec-eqp-1a","name":"Pultec EQP-1A","stereo":true,"out":[2,3],"in":[2,3]}]})");
    DeviceLibrary lib(d); REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE(lib.newId("SSL  G-Bus Comp ") == "ssl-g-bus-comp");
    REQUIRE(lib.newId("Pultec EQP-1A") == "pultec-eqp-1a-2");
    REQUIRE(lib.newId("Pultec EQP-1a") == "pultec-eqp-1a-2");
    REQUIRE(lib.newId("\xc3\x81rv\xc3\xadzt\xc5\xb1r\xc5\x91") == "rv-zt-r");   // non-ASCII bytes separate
    REQUIRE(lib.newId("***") == "device");
}

TEST_CASE("library_remove_device_rewrites_file")
{
    const auto d = tempDir("remove");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[
      {"id":"a","name":"A","stereo":true,"out":[0,1],"in":[0,1]},{"id":"b","name":"B","stereo":false,"out":[2],"in":[2]}]})");
    DeviceLibrary lib(d); REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE_FALSE(lib.removeDevice("zzz"));
    REQUIRE(lib.devices().size() == 2);
    REQUIRE(lib.removeDevice("a"));
    REQUIRE(lib.devices().size() == 1); REQUIRE(lib.devices()[0].id == "b"); REQUIRE(lib.byParameter(1)->id == "b");
    DeviceLibrary again(d); REQUIRE(again.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE(again.devices().size() == 1); REQUIRE(again.devices()[0].id == "b");
    fs::permissions(d / "devices.json", fs::perms::owner_read, fs::perm_options::replace);
    DeviceLibrary ro(d); REQUIRE(ro.load() == DeviceLibrary::LoadStatus::ReadOnly);
    REQUIRE_FALSE(ro.removeDevice("b"));
    REQUIRE(ro.devices().size() == 1);
    fs::permissions(d / "devices.json", fs::perms::owner_all, fs::perm_options::replace);
}

TEST_CASE("library_reload_if_changed_sees_external_write")
{
    const auto d = tempDir("reload");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[{"id":"a","name":"A","stereo":true,"out":[0,1],"in":[0,1]}]})");
    DeviceLibrary lib(d); REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE_FALSE(lib.reloadIfChanged());
    // another instance (or a hand edit) rewrites the file: a different size is enough even within the mtime resolution
    writeFile(d / "devices.json", R"({"schema":1,"devices":[{"id":"a","name":"A","stereo":true,"out":[0,1],"in":[0,1]},
      {"id":"b","name":"B renamed","stereo":false,"out":[2],"in":[2]}]})");
    REQUIRE(lib.reloadIfChanged());
    REQUIRE(lib.devices().size() == 2); REQUIRE(lib.devices()[1].name == "B renamed");
    REQUIRE_FALSE(lib.reloadIfChanged());
    // its own write is not a "change"
    auto b = *lib.device("b"); b.name = "B2"; REQUIRE(lib.upsertDevice(b));
    REQUIRE_FALSE(lib.reloadIfChanged());
    REQUIRE(lib.device("b")->name == "B2");
}

TEST_CASE("library_index_of_follows_an_external_delete_through_upsert")
{
    // final review H1 (2026-11-05): upsertDevice is read-merge-write, so after another instance deleted an earlier preset this
    // instance's own write already runs on the shorter list; the processor realigns its claimed index from indexOf() after
    // every write, not only after an editor-driven reload
    const auto d = tempDir("indexof");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[
      {"id":"a","name":"A","stereo":true,"out":[0,1],"in":[0,1]},{"id":"b","name":"B","stereo":true,"out":[2,3],"in":[2,3]},
      {"id":"c","name":"C","stereo":true,"out":[4,5],"in":[4,5]}]})");
    DeviceLibrary mine(d); REQUIRE(mine.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE(mine.indexOf("c") == 3);
    REQUIRE(mine.indexOf("zzz") == 0);
    DeviceLibrary other(d); REQUIRE(other.load() == DeviceLibrary::LoadStatus::Ok);
    REQUIRE(other.removeDevice("a"));
    auto c = *mine.device("c"); c.sendTrimDb = 1.5f;
    REQUIRE(mine.upsertDevice(c));
    REQUIRE(mine.indexOf("c") == 2);
    REQUIRE_FALSE(mine.reloadIfChanged());   // its own write noted the file: nothing later would realign it — the write must
}

TEST_CASE("library_per_direction_widths_round_trip_and_legacy_stereo_key")
{
    // mono/stereo routing design 2026-11-05: "sendStereo" / "returnStereo" replace "stereo"; a legacy record loads "stereo" into both
    const auto d = tempDir("widths");
    writeFile(d / "devices.json", R"({"schema":1,"devices":[
      {"id":"mixed","name":"Mixed","sendStereo":false,"returnStereo":true,"out":[11],"in":[9,10]},
      {"id":"old","name":"Old","stereo":false,"out":[4],"in":[4]}]})");
    DeviceLibrary lib(d); REQUIRE(lib.load() == DeviceLibrary::LoadStatus::Ok);
    const auto m = *lib.device("mixed");
    REQUIRE_FALSE(m.sendStereo); REQUIRE(m.returnStereo);
    REQUIRE(m.out[0] == 11); REQUIRE(m.out[1] == -1); REQUIRE(m.in[0] == 9); REQUIRE(m.in[1] == 10);
    const auto o = *lib.device("old");
    REQUIRE_FALSE(o.sendStereo); REQUIRE_FALSE(o.returnStereo);
    DeviceRecord r = m; r.name = "Mixed 2";
    REQUIRE(lib.upsertDevice(r));
    const std::string text = readFile(d / "devices.json");
    REQUIRE(text.find("\"sendStereo\"") != std::string::npos);
    REQUIRE(text.find("\"returnStereo\"") != std::string::npos);
    REQUIRE(text.find("\"stereo\"") == std::string::npos);   // the writer emits only the new keys
    DeviceLibrary back(d); back.load();
    const auto g = *back.device("mixed");
    REQUIRE_FALSE(g.sendStereo); REQUIRE(g.returnStereo); REQUIRE(g.in[1] == 10);
}

TEST_CASE("effective_pair_narrows_a_stereo_direction_to_one_side")
{
    using ni::engine::SideSel;
    DeviceRecord d; d.sendStereo = d.returnStereo = true; d.out[0] = 86; d.out[1] = 87; d.in[0] = 84; d.in[1] = 85;
    auto p = effectivePair(d, SideSel::Stereo, SideSel::Stereo);
    REQUIRE(p.sendStereo); REQUIRE(p.retStereo); REQUIRE(p.out[0] == 86); REQUIRE(p.out[1] == 87); REQUIRE(p.in[0] == 84); REQUIRE(p.in[1] == 85);
    p = effectivePair(d, SideSel::L, SideSel::R);
    REQUIRE_FALSE(p.sendStereo); REQUIRE_FALSE(p.retStereo);
    REQUIRE(p.out[0] == 86); REQUIRE(p.out[1] == -1); REQUIRE(p.in[0] == 85); REQUIRE(p.in[1] == -1);
    p = effectivePair(d, SideSel::R, SideSel::Stereo);
    REQUIRE_FALSE(p.sendStereo); REQUIRE(p.retStereo); REQUIRE(p.out[0] == 87); REQUIRE(p.out[1] == -1); REQUIRE(p.in[1] == 85);
    // a mono direction ignores its selector
    DeviceRecord m = d; m.sendStereo = false; m.out[1] = -1;
    p = effectivePair(m, SideSel::R, SideSel::L);
    REQUIRE_FALSE(p.sendStereo); REQUIRE(p.out[0] == 86); REQUIRE(p.out[1] == -1); REQUIRE_FALSE(p.retStereo); REQUIRE(p.in[0] == 84);
}

TEST_CASE("settings_reverse_wheel_defaults_off_persists_and_is_shared")
{
    // GitHub #1: global SETUP option for mice reversed by a scroll utility
    const auto d = tempDir("settings_wheel");
    DeviceLibrary a(d), b(d);
    a.load(); b.load();
    REQUIRE_FALSE(a.settings().reverseWheel);   // absent file → follow the system
    REQUIRE(a.setReverseWheel(true));
    REQUIRE(a.settings().reverseWheel);
    REQUIRE(a.setSkin(1));                      // other keys kept both ways
    b.reloadSettings();
    REQUIRE(b.settings().reverseWheel);
    REQUIRE(b.settings().skin == 1);
    DeviceLibrary c(d);
    c.load();
    REQUIRE(c.settings().reverseWheel);
    REQUIRE(c.setReverseWheel(false));
    c.reloadSettings();
    REQUIRE_FALSE(c.settings().reverseWheel);
    REQUIRE(c.settings().skin == 1);
    writeFile(d / "settings.json", "{ broken");
    REQUIRE_FALSE(c.setReverseWheel(true));    // a corrupt file is never overwritten
}
