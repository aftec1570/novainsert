// Persistence (plan Task 22, F1 §9/§21, R-LIB-1 project side): the VST3 state blob carries the parameters plus a device and
// calibration snapshot; a matching snapshot is applied on load without re-measuring; a newer library record is reported,
// never auto-applied.
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include "engine/state_machine.hpp"
#include "plugin/persistence.hpp"

using namespace ni::plugin;
using namespace ni::engine;
namespace fs = std::filesystem;

namespace
{
CalibKey key(int32_t buffer)
{
    CalibKey k; k.identIn = k.identOut = "Apollo x8p"; k.sendIdx[0] = 2; k.returnIdx[0] = 2; k.sampleRate = 48000;
    k.bufferSize = buffer; k.algorithmVersion = 1; return k;
}
InstanceState sample()
{
    InstanceState s;
    s.params = { { "device", 3.0 }, { "sendTrim", -2.5 }, { "mode", 0.0 } };
    DeviceRecord d; d.id = "pultec"; d.name = "Pultec"; d.sendStereo = d.returnStereo = false; d.out[0] = 11; d.in[0] = 9; d.version = 4;
    d.identIn = d.identOut = "Apollo x8p";
    s.deviceSnapshot = d;
    CalibRecord c; c.key = key(128); c.deviceId = "pultec"; c.result.physLoop[0] = 602; c.result.reportedPdc = 896; c.version = 2;
    s.calibSnapshot = c;
    return s;
}
fs::path tempDir(const char* tag)
{
    const fs::path d = fs::temp_directory_path() / (std::string("novainsert-persist-") + tag);
    fs::remove_all(d); fs::create_directories(d); return d;
}
} // namespace

TEST_CASE("persistence_round_trip_equality")
{
    const InstanceState s = sample();
    const auto blob = serialize(s);
    const auto back = deserialize(blob.getData(), blob.getSize());
    REQUIRE(back.has_value());
    REQUIRE(back->schema == 1);
    REQUIRE(back->params == s.params);
    REQUIRE(back->deviceSnapshot == s.deviceSnapshot);
    REQUIRE(back->calibSnapshot->key == s.calibSnapshot->key);
    REQUIRE(back->calibSnapshot->deviceId == "pultec");
    REQUIRE(back->calibSnapshot->result.physLoop[0] == 602);
    REQUIRE(back->calibSnapshot->result.reportedPdc == 896);
    REQUIRE(back->calibSnapshot->version == 2);
}

TEST_CASE("persistence_without_snapshots_round_trips")
{
    InstanceState s;
    s.params = { { "device", 0.0 } };
    const auto blob = serialize(s);
    const auto back = deserialize(blob.getData(), blob.getSize());
    REQUIRE(back.has_value());
    REQUIRE_FALSE(back->deviceSnapshot.has_value());
    REQUIRE_FALSE(back->calibSnapshot.has_value());
}

TEST_CASE("persistence_truncated_blob_is_nullopt")
{
    const auto blob = serialize(sample());
    std::string why;
    REQUIRE_FALSE(deserialize(blob.getData(), blob.getSize() / 2, &why).has_value());
    REQUIRE_FALSE(why.empty());
    REQUIRE_FALSE(deserialize(nullptr, 0).has_value());
}

TEST_CASE("persistence_schema_99_is_nullopt_with_reason")
{
    const char* text = R"({"schema":99,"params":{}})";
    std::string why;
    REQUIRE_FALSE(deserialize(text, std::strlen(text), &why).has_value());
    REQUIRE(why.find("99") != std::string::npos);
}

TEST_CASE("persistence_s1_blob_without_snapshots_still_loads")
{
    const char* text = R"({"schema":1,"params":{"device":2.0}})";   // written by S1–S6 builds
    const auto back = deserialize(text, std::strlen(text));
    REQUIRE(back.has_value());
    REQUIRE(back->params.at("device") == 2.0);
}

TEST_CASE("persistence_snapshot_applied_without_remeasure_when_key_matches")
{
    const InstanceState s = sample();
    REQUIRE(restoreAction(s, key(128)) == RestoreAction::ApplySnapshot);
    REQUIRE(restoreAction(s, key(256)) == RestoreAction::Remeasure);   // buffer changed since save
    InstanceState none = s; none.calibSnapshot.reset();
    REQUIRE(restoreAction(none, key(128)) == RestoreAction::Remeasure);
    InstanceState failed = s; failed.calibSnapshot->result.failReason = Reason::NoReturn;
    REQUIRE(restoreAction(failed, key(128)) == RestoreAction::Remeasure);

    StateMachine sm;   // the restore path: Idle → ApplyingLatency with StateRestore, then Locked via the observer
    sm.setKey(9);
    REQUIRE(sm.apply(State::ApplyingLatency, Reason::StateRestore, 1, 0));
    REQUIRE(sm.apply(State::Locked, Reason::None, 1, 1));
}

TEST_CASE("persistence_compare_against_library")
{
    const auto d = tempDir("compare");
    DeviceLibrary lib(d);
    lib.load();
    InstanceState s = sample();
    REQUIRE(compare(s, lib) == SnapshotDiff::NotInLibrary);

    DeviceRecord rec = *s.deviceSnapshot;
    rec.version = 0;
    REQUIRE(lib.upsertDevice(rec));                 // library now holds version 1
    s.deviceSnapshot = *lib.device("pultec");
    CalibRecord c = *s.calibSnapshot;
    REQUIRE(lib.storeCalibration(c));               // version 1
    s.calibSnapshot = *lib.calibration(c.key);
    REQUIRE(compare(s, lib) == SnapshotDiff::Same);

    c.result.physLoop[0] = 640;
    REQUIRE(lib.storeCalibration(c));               // newer record in the library: reported, never auto-applied
    REQUIRE(compare(s, lib) == SnapshotDiff::CalibChanged);
    REQUIRE(s.calibSnapshot->result.physLoop[0] == 602);

    auto edited = *lib.device("pultec");
    edited.name = "Pultec EQP-1A";
    REQUIRE(lib.upsertDevice(edited));
    REQUIRE(compare(s, lib) == SnapshotDiff::DeviceChanged);
}

TEST_CASE("active_device_keeps_the_project_snapshot_over_a_changed_or_missing_library_record")
{
    const InstanceState s = sample();
    DeviceRecord edited = *s.deviceSnapshot;
    edited.vuRefDbfs = -14.0f; edited.version = 5;   // the library was edited after the project was saved
    const auto a = activeDevice (edited, s.deviceSnapshot);
    REQUIRE(a.has_value());
    REQUIRE(a->vuRefDbfs == s.deviceSnapshot->vuRefDbfs);   // never auto-applied (F1 §16)
    REQUIRE(a->version == 4);
    const auto missing = activeDevice (std::nullopt, s.deviceSnapshot);   // library missing/corrupt: run with the snapshot (F1 §21)
    REQUIRE(missing.has_value());
    REQUIRE(missing->id == "pultec");
    const auto fresh = activeDevice (edited, std::nullopt);   // no project snapshot: the library record
    REQUIRE(fresh.has_value());
    REQUIRE(fresh->version == 5);
    REQUIRE_FALSE(activeDevice (std::nullopt, std::nullopt).has_value());
}

// Review C1/I1/I2/I5 (S7/S8 gate): the restore is tied to the device parameter value the state carried, never to "first claim"
TEST_CASE("project_restore_claim_of_the_saved_device_is_the_restore")
{
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    REQUIRE(pr.armed());
    REQUIRE(pr.claim(3) == ProjectRestore::Claim::Restore);
    REQUIRE_FALSE(pr.armed());
    REQUIRE(pr.record(3, std::nullopt)->id == "pultec");       // runs the project's record
    REQUIRE(pr.takePending().has_value());                     // the slot gets the calibration snapshot once
    REQUIRE_FALSE(pr.takePending().has_value());
}

TEST_CASE("project_restore_another_device_is_a_user_selection_and_drops_the_snapshot")
{
    // C1: the load claim never became Owner (conflict / device closed); the user then picks device 5
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    REQUIRE(pr.claim(5) == ProjectRestore::Claim::Selection);
    DeviceRecord lib; lib.id = "ssl"; lib.version = 1;
    REQUIRE(pr.record(5, lib)->id == "ssl");
    REQUIRE_FALSE(pr.takePending().has_value());
    REQUIRE_FALSE(pr.stale());
    REQUIRE(pr.claim(3) == ProjectRestore::Claim::Selection);   // back to 3: the library record, not the dropped snapshot
    REQUIRE_FALSE(pr.record(3, std::nullopt).has_value());
}

TEST_CASE("project_restore_snapshot_record_only_for_its_own_device_value")
{
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    DeviceRecord lib; lib.id = "other";
    REQUIRE(pr.record(4, lib)->id == "other");   // M3: another parameter value never borrows this snapshot
}

TEST_CASE("project_restore_a_second_load_rearms_on_a_measured_instance")
{
    // I1: undo / preset recall on an instance that already measured is a restore, not a user selection
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    REQUIRE(pr.claim(3) == ProjectRestore::Claim::Restore);
    pr.takePending();
    pr.loaded(sample(), 6);
    REQUIRE(pr.claim(6) == ProjectRestore::Claim::Restore);
    REQUIRE(pr.takePending().has_value());
}

TEST_CASE("project_restore_drop_pending_leaves_no_snapshot_for_a_later_remeasure")
{
    // I2: a load whose claim does not follow (same device, equal record) must not hijack a later DeviceReopen slot
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    pr.dropPending();
    REQUIRE_FALSE(pr.armed());
    REQUIRE_FALSE(pr.takePending().has_value());
}

TEST_CASE("project_restore_calibration_survives_a_save_before_the_slot")
{
    // I5: saved while the device is closed / in conflict: the blob keeps the calibration snapshot
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    REQUIRE(pr.claim(3) == ProjectRestore::Claim::Restore);
    REQUIRE(pr.calibForSave("pultec").has_value());
    REQUIRE(pr.calibForSave("pultec")->result.physLoop[0] == 602);
    REQUIRE_FALSE(pr.calibForSave("ssl").has_value());
    pr.takePending();
    REQUIRE_FALSE(pr.calibForSave("pultec").has_value());
}

TEST_CASE("project_restore_snapshot_equals_the_running_record")
{
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    REQUIRE(pr.snapshotEquals(sample().deviceSnapshot));
    DeviceRecord other = *sample().deviceSnapshot; other.vuRefDbfs = -12.0f;
    REQUIRE_FALSE(pr.snapshotEquals(other));
    REQUIRE_FALSE(pr.snapshotEquals(std::nullopt));
}

// ---- VU GUI plan Task 4: device defaults vs snapshot, instance CAL block ------------------------------------------------------
TEST_CASE("persistence_compare_trim_only_change_is_defaults_changed")
{
    const auto d = tempDir("defaults");
    DeviceLibrary lib(d);
    lib.load();
    InstanceState s = sample();
    s.calibSnapshot.reset();
    DeviceRecord rec = *s.deviceSnapshot; rec.version = 0;
    REQUIRE(lib.upsertDevice(rec));
    s.deviceSnapshot = *lib.device("pultec");
    REQUIRE(compare(s, lib) == SnapshotDiff::Same);

    auto cal = *lib.device("pultec");
    cal.sendTrimDb = 3.8f; cal.returnTrimDb = -3.5f; cal.returnOffsetDb[0] = 0.2f;
    REQUIRE(lib.upsertDevice(cal));
    REQUIRE(compare(s, lib) == SnapshotDiff::DefaultsChanged);

    auto ref = *lib.device("pultec");
    ref.vuRefDbfs = -18.0f;
    REQUIRE(lib.upsertDevice(ref));
    REQUIRE(compare(s, lib) == SnapshotDiff::DeviceChanged);
}

TEST_CASE("persistence_instance_cal_round_trips")
{
    InstanceState s = sample();
    InstanceCal c; c.link = true; c.sendOffsetDb[1] = -0.5f; c.returnOffsetDb[0] = 1.25f;
    s.cal = c;
    const auto blob = serialize(s);
    const auto back = deserialize(blob.getData(), blob.getSize());
    REQUIRE(back);
    REQUIRE(back->cal);
    REQUIRE(back->cal->link);
    REQUIRE(back->cal->sendOffsetDb[1] == -0.5f);
    REQUIRE(back->cal->returnOffsetDb[0] == 1.25f);
    InstanceState plain = sample();
    const auto b2 = serialize(plain);
    REQUIRE_FALSE(deserialize(b2.getData(), b2.getSize())->cal.has_value());
}

TEST_CASE("persistence_instance_cal_offsets_are_clamped")
{
    const char* text = R"({"schema":1,"params":{},"cal":{"link":true,"sendOffsetDb":[99,0],"returnOffsetDb":[-99,1.5]}})";
    const auto st = deserialize(text, std::strlen(text));
    REQUIRE(st);
    REQUIRE(st->cal);
    REQUIRE(st->cal->sendOffsetDb[0] == kOffsetMaxDb);
    REQUIRE(st->cal->returnOffsetDb[0] == kOffsetMinDb);
    REQUIRE(st->cal->returnOffsetDb[1] == 1.5f);
}

// ---- VU GUI final review C1/I1: an edit this instance writes to the library also updates the kept project snapshot ---------
TEST_CASE("project_restore_adopt_takes_converter_and_defaults_from_the_library_record")
{
    const auto d = tempDir("adopt");
    DeviceLibrary lib(d);
    lib.load();
    InstanceState s = sample();
    s.calibSnapshot.reset();
    DeviceRecord rec = *s.deviceSnapshot; rec.version = 0;
    REQUIRE(lib.upsertDevice(rec));
    s.deviceSnapshot = *lib.device("pultec");
    ProjectRestore pr;
    pr.loaded(s, 3);
    REQUIRE(pr.claim(3) == ProjectRestore::Claim::Restore);

    // SETUP: converter calibration written to the library (C1) — the restored instance must run it, not the old snapshot
    auto conv = *lib.device("pultec");
    conv.sendDbuAt0dBFS = 20.2f; conv.returnDbuAt0dBFS = 20.2f; conv.vuRefDbfs = -18.0f; conv.outboardMaxInDbu = 22.0f;
    REQUIRE(lib.upsertDevice(conv));
    pr.adopt(*lib.device("pultec"));
    const auto running = pr.record(3, lib.device("pultec"));
    REQUIRE(running);
    REQUIRE(running->sendDbuAt0dBFS == 20.2f);
    REQUIRE(running->vuRefDbfs == -18.0f);
    REQUIRE(running->outboardMaxInDbu == 22.0f);

    // CAL Done / Apply (I1): trims, LINK, offsets written to the library — the saved snapshot then equals the library
    auto cal = *lib.device("pultec");
    cal.sendTrimDb = 3.8f; cal.returnTrimDb = -3.5f; cal.link = true; cal.returnOffsetDb[1] = 0.2f;
    REQUIRE(lib.upsertDevice(cal));
    pr.adopt(*lib.device("pultec"));
    InstanceState saved = s;
    saved.deviceSnapshot = pr.device();
    REQUIRE(compare(saved, lib) == SnapshotDiff::Same);
}

TEST_CASE("project_restore_adopt_keeps_identity_and_ignores_another_device")
{
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    DeviceRecord other = *sample().deviceSnapshot; other.id = "la2a"; other.sendDbuAt0dBFS = 20.2f;
    pr.adopt(other);
    REQUIRE(pr.device()->sendDbuAt0dBFS == 24.0f);

    DeviceRecord moved = *sample().deviceSnapshot; moved.out[0] = 5; moved.version = 9; moved.sendTrimDb = 1.0f;
    pr.adopt(moved);   // a library record with different channels: the defaults follow, the kept identity and version do not
    REQUIRE(pr.device()->out[0] == 11);
    REQUIRE(pr.device()->version == 4);
    REQUIRE(pr.device()->sendTrimDb == 1.0f);
}

TEST_CASE("project_restore_reindex_moves_the_snapshot_to_the_new_parameter")
{
    // final review H2 (2026-11-05): another instance deleted an earlier preset, so the running record's library index moved
    // from 3 to 2; the kept project snapshot must follow that index, or the restored instance would silently swap to the
    // library record (and save it over the project's snapshot) without the user re-selecting
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    REQUIRE(pr.claim(3) == ProjectRestore::Claim::Restore);
    REQUIRE(pr.usesProject(3));
    pr.reindex(2);
    REQUIRE(pr.deviceParam() == 2);
    REQUIRE(pr.usesProject(2));
    REQUIRE_FALSE(pr.usesProject(3));
    REQUIRE(pr.record(2, std::nullopt)->id == "pultec");   // still the snapshot record, now under its new value
}

TEST_CASE("project_restore_adopt_follows_name_and_mode_from_the_library_record")
{
    // final review M1 (2026-11-05): a rename on the DEVICES page must reach a restored instance's snapshot (the FX slot takes
    // the running record's name), and the version follows when the rename is the only difference
    ProjectRestore pr;
    pr.loaded(sample(), 3);
    DeviceRecord renamed = *sample().deviceSnapshot; renamed.name = "Pultec (rack 2)"; renamed.mode = ni::engine::Mode::Effect; renamed.version = 5;
    pr.adopt(renamed);
    REQUIRE(pr.device()->name == "Pultec (rack 2)");
    REQUIRE(pr.device()->mode == ni::engine::Mode::Effect);
    REQUIRE(pr.device()->version == 5);
}

TEST_CASE("persistence_measure_on_open_probes_and_keeps_the_snapshot_as_fallback")
{
    // measure on project open (user 2026-11-06): a matching snapshot no longer locks on its own — the instance measures and keeps
    // the snapshot for a final failure; with the setting off the old 0-probe restore stands
    const InstanceState s = sample();
    REQUIRE(restoreAction(s, key(128), true) == RestoreAction::MeasureKeepSnapshot);
    REQUIRE(restoreAction(s, key(128), false) == RestoreAction::ApplySnapshot);
    REQUIRE(restoreAction(s, key(256), true) == RestoreAction::Remeasure);   // another buffer: the snapshot is no fallback
    InstanceState none = s; none.calibSnapshot.reset();
    REQUIRE(restoreAction(none, key(128), true) == RestoreAction::Remeasure);
    InstanceState failed = s; failed.calibSnapshot->result.failReason = Reason::NoReturn;
    REQUIRE(restoreAction(failed, key(128), true) == RestoreAction::Remeasure);
}

TEST_CASE("persistence_open_fallback_only_on_a_final_failure_with_the_same_key")
{
    const auto kept = sample().calibSnapshot;
    REQUIRE(openFallback(kept, true, key(128)).has_value());
    REQUIRE(openFallback(kept, true, key(128))->result.physLoop[0] == 602);
    REQUIRE_FALSE(openFallback(kept, false, key(128)).has_value());          // a retry is still queued
    REQUIRE_FALSE(openFallback(kept, true, key(256)).has_value());           // buffer changed since open
    REQUIRE_FALSE(openFallback(kept, true, std::nullopt).has_value());       // no key (no device)
    REQUIRE_FALSE(openFallback(std::nullopt, true, key(128)).has_value());   // nothing kept
}
