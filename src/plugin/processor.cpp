#include "plugin/processor.hpp"
#include <cstring>
#include "host/slot_name.hpp"
#include <pluginterfaces/base/funknown.h>
using namespace Steinberg;
#include <reaper_vst3_interfaces.h>
#include "host/coordinator.hpp"
#include "host/diagnostics.hpp"
#include <cmath>
#include <filesystem>
#include "engine/probe.hpp"
#include "engine/state_machine.hpp"
#include "plugin/fallback.hpp"
#include "plugin/persistence.hpp"
#include "plugin/settings_apply.hpp"
#include "plugin/latency_source.hpp"
#include "gui/editor.hpp"
#include "engine/level_cal.hpp"

DEF_CLASS_IID (IReaperHostApplication)

using ni::engine::Reason;
using ni::engine::State;
using ni::plugin::Param;
namespace P = ni::plugin;

namespace
{
std::atomic<int> g_instanceCounter { 0 };
juce::String id (Param p) { return juce::String (P::paramId (p).data(), P::paramId (p).size()); }

struct ReadOnlyInt final : juce::AudioParameterInt
{
    using AudioParameterInt::AudioParameterInt;
    bool isAutomatable() const override { return false; }   // must agree with P::kParamAutomatable (checked in the constructor)
};
const ni::engine::ProbeGenerator& sharedProbe()   // deterministic template, one per process (message thread)
{
    static const ni::engine::ProbeGenerator g { ni::engine::ProbeSpec::click() };   // the latency ping (2026-11-05)
    return g;
}
} // namespace

// CAL port over the instance's slot loop: the reference tone is the looping probe scaled by the send trims, the return is
// captured before the return trim, and the plugin output is muted while the tone runs (message thread).
struct NovaInsertProcessor::EngineCalPort final : ni::plugin::CalPort
{
    explicit EngineCalPort (NovaInsertProcessor& o) : p (o) {}
    void startTone (const float* tone, int n) override
    {
        auto& e = p.inst_->engine();
        p.calRt_.store (true, std::memory_order_release);
        e.setMuted (true);
        e.setProbe (tone, n, true, true);
    }
    void stopTone() override
    {
        auto& e = p.inst_->engine();
        e.setProbe (nullptr, 0);
        p.calRt_.store (false, std::memory_order_release);
        e.setMuted (p.pdcMuted_);
    }
    int captured() const override { return p.inst_->engine().capturedCount(); }
    bool capture (float* l, float* r, int n) override { return p.inst_->engine().captureReturn (l, r, n); }
    int physLoop() const override { return p.inst_->physLoop(); }
    NovaInsertProcessor& p;
};

NovaInsertProcessor::NovaInsertProcessor()
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    instNo_ = g_instanceCounter.fetch_add (1);
    const int v = 1;   // parameter version hint
    addParameter (pDevice      = new juce::AudioParameterInt    ({ id (Param::Device), v }, "Device", 0, P::kDeviceMax, 0));
    addParameter (pMode        = new juce::AudioParameterChoice ({ id (Param::Mode), v }, "Mode", { "Insert", "Effect" }, 0));
    addParameter (pSendSel     = new juce::AudioParameterChoice ({ id (Param::SendSel), v }, "Send", { "Stereo", "L", "R" }, 0));
    addParameter (pReturnSel   = new juce::AudioParameterChoice ({ id (Param::ReturnSel), v }, "Return", { "Stereo", "L", "R" }, 0));
    addParameter (pStereo      = new juce::AudioParameterBool   ({ id (Param::Stereo), v }, "Stereo", false));
    addParameter (pSendTrim    = new juce::AudioParameterFloat  ({ id (Param::SendTrim), v }, "Send trim", P::kTrimMinDb, P::kTrimMaxDb, 0.0f));
    addParameter (pReturnTrim  = new juce::AudioParameterFloat  ({ id (Param::ReturnTrim), v }, "Return trim", P::kTrimMinDb, P::kTrimMaxDb, 0.0f));
    addParameter (pPreRoll     = new juce::AudioParameterBool   ({ id (Param::PreRollLeadIn), v }, "Pre-roll lead-in", false));
    addParameter (pReapplyAnt  = new juce::AudioParameterBool   ({ id (Param::ReapplyAnticipative), v }, "Re-apply anticipative FX fix", false));
    addParameter (pState       = new ReadOnlyInt ({ id (Param::State), v }, "State", 0, P::kStateMax, static_cast<int> (State::UnsupportedHost)));
    addParameter (pPhysLoop    = new ReadOnlyInt ({ id (Param::PhysLoop), v }, "Physical loop", 0, P::kPhysLoopMax, 0));
    addParameter (pReportedPdc = new ReadOnlyInt ({ id (Param::ReportedPdc), v }, "Reported PDC", 0, P::kPhysLoopMax, 0));
    addParameter (pInherentGap = new ReadOnlyInt ({ id (Param::InherentGap), v }, "Inherent gap", 0, P::kPhysLoopMax, 0));
    addParameter (pLink        = new juce::AudioParameterBool   ({ id (Param::Link), v }, "Link", false));
    addParameter (pCalAction   = new ReadOnlyInt ({ id (Param::CalAction), v }, "CAL action", 0, P::kCalActionMax, 0));
    addParameter (pCalStatus   = new ReadOnlyInt ({ id (Param::CalStatus), v }, "CAL status", 0, P::kCalStatusMax, 0));
    // The parameter table (plugin/parameters.hpp) is the contract ReaScript/tests rely on: same order, same ids, same automatable flags.
    jassert (getParameters().size() == P::kParamCount);
    for (int i = 0; i < P::kParamCount; ++i)
    {
        [[maybe_unused]] auto* q = dynamic_cast<juce::RangedAudioParameter*> (getParameters()[i]);
        jassert (q != nullptr && q->getParameterID() == id (static_cast<Param> (i)));
        jassert (q->isAutomatable() == P::kParamAutomatable[static_cast<std::size_t> (i)]);
    }
    // meters are fed by the hook (device thread) from the first configure on; only 48 kHz runs (F1), so prepared once here
    sendMeter_.prepare (ni::engine::kSampleRate);
    returnMeter_.prepare (ni::engine::kSampleRate);
    pDevice->addListener (this);
    startTimer (20);   // Measurer tick (F1 §7 timeouts); host polling runs every 25th tick (500 ms)
}

NovaInsertProcessor::~NovaInsertProcessor()
{
    stopTimer();
    pDevice->removeListener (this);
    releaseOwnership();
    if (inst_) logLoop (true);
    ni::host::DiagLog::get().text ("DESTROY", instNo_, "state=%d|sendFrames=%llu", static_cast<int> (state()),
                                   static_cast<unsigned long long> (inst_ ? inst_->sendFrames() : 0));
    instRt_.store (nullptr, std::memory_order_release);
    measuringRt_.store (false, std::memory_order_release);
    if (measurer_) measurer_->cancel (Reason::None);
    corrClient_.reset();   // drops our queued jobs, waits for a running one (it reads the Measurer's buffers)
    measurer_.reset();
    probeIo_.reset();
    worker_.reset();       // last instance: joins the correlator thread
    pdc_.reset();     // holds the Instance's observer
    cal_.reset(); calPort_.reset();
    inst_.reset();    // unregisters from the hook before the coordinator can go
    coord_.reset();   // last instance: hook uninstalled, diagnostics flushed
    if (reaperHost_ != nullptr) { reaperHost_->release(); reaperHost_ = nullptr; }
}

void NovaInsertProcessor::releaseOwnership()
{
    if (! coord_) return;
    cancelMeasure (Reason::DeviceLost);
    requeueAfterRecord_ = false;
    if (inst_ && inst_->active()) { inst_->release(); loopCfg_ = {}; loopRouting_ = -1; }
    coord_->removeListener (instNo_);
    coord_->clearRenderView (instNo_);
    coord_->scheduler().cancel (instNo_);
    coord_->hostSettings().release (instNo_);
    if (claimedDevice_ > 0) { coord_->pairs().release (instNo_); ni::host::DiagLog::get().text ("PAIR", instNo_, "release|device=%d", claimedDevice_); }
    claimedDevice_ = -1; pairOwned_ = false; conflicted_ = false;
}

void NovaInsertProcessor::setIHostApplication (Steinberg::FUnknown* host)
{
    if (host == nullptr) return;
    if (coord_) return;   // JUCE may call this again on reconnect/setAudioProcessor: one acquire per instance (S1 review I1)
    IReaperHostApplication* reaper = nullptr;
    if (host->queryInterface (IReaperHostApplication::iid, reinterpret_cast<void**> (&reaper)) != Steinberg::kResultOk || reaper == nullptr)
        return;   // not REAPER: state stays UnsupportedHost

    ni::host::ReaperApi api;
    std::vector<std::string> missing;
    const auto status = ni::host::loadReaperApi ([reaper] (const char* n) { return reaper->getReaperApi (n); }, api, missing);
    reaperHost_ = reaper;   // retained (released in the destructor) for getReaperParent()

    // Settings (product feature, F1 §21): <ResourcePath>/NovaInsert/settings.json
    // {"diagnostics": true, "diagnosticsDir": "...", "maxConcurrentProbes": 0}. Diagnostics are enabled before the
    // status check so an api=missing row reaches the file too (S1 review M3).
    ni::host::DiagLog::Settings settings;
    std::string base;
    if (api.GetResourcePath != nullptr)
    {
        base = std::string (api.GetResourcePath()) + "/NovaInsert";
        settings = ni::host::DiagLog::probeSettings (base + "/settings.json");
        if (settings.diagnostics && ! ni::host::DiagLog::get().enabled()) ni::host::DiagLog::get().enable (settings.dir.empty() ? base : settings.dir);
    }

    if (status != ni::host::ApiStatus::Ok)
    {
        std::string names;
        for (auto& m : missing) { names += m; names += ' '; }
        ni::host::DiagLog::get().text ("INIT", instNo_, "api=missing|%s", names.c_str());
        return;
    }

    apiOk_ = true;
    coord_ = ni::host::Coordinator::acquire (api);
    inst_ = std::make_unique<ni::plugin::Instance> (coord_->hooks());
    inst_->engine().setMeters (&sendMeter_, &returnMeter_);   // hook-side taps (VU GUI §7), set before the hook registers
    instRt_.store (inst_.get(), std::memory_order_release);
    pdc_ = std::make_unique<ni::plugin::PdcReporter> (
        [this] (int32_t R)   // the reporter's host call: engine and host latency change together (F1 §9)
        {
            if (pendingPhys_ >= 0 && inst_ && inst_->applyMeasured (pendingPhys_)) physOrigin_ = pendingOrigin_;
            else if (pendingPhys_ >= 0) ni::host::DiagLog::get().text ("PDC", instNo_, "apply_failed|phys=%d|gen=%u", pendingPhys_, pdcGen_);
            pendingPhys_ = -1;
            setLatencySamples (R);
            setReadOnly (Param::PhysLoop, inst_ ? inst_->physLoop() : 0);
            setReadOnly (Param::ReportedPdc, R);
            ni::host::DiagLog::get().text ("PDC", instNo_, "set|R=%d|phys=%d|B=%d|gen=%u", R, inst_ ? inst_->physLoop() : 0, loopCfg_.B, pdcGen_);
            logLoop (true);
        },
        inst_->pdcObserver(),
        [this] (State st, Reason r, uint32_t g, int64_t, const char* d) { return applyPdcState (st, r, g, d); },
        [this] { return lkgValid_ && lkgCfg_ == loopCfg_; });
    worker_ = ni::plugin::CorrelatorWorker::acquire ([] (std::function<void()> f) { juce::MessageManager::callAsync (std::move (f)); });
    probeIo_ = std::make_unique<ni::plugin::SlotLoopProbeIo> (inst_->engine());
    corrClient_ = std::make_unique<ni::plugin::CorrelatorWorker::Client> (worker_,
        [this] (uint64_t t, const ni::engine::CorrResult& l, const ni::engine::CorrResult& r) { if (measurer_) measurer_->onCorrelation (t, l, r); });
    measurer_ = std::make_unique<ni::plugin::Measurer> (*probeIo_, *corrClient_, sharedProbe(),
        [] { return static_cast<int64_t> (juce::Time::getMillisecondCounterHiRes()); }, 1);
    measurer_->onResult = [this] (uint32_t g, const ni::engine::CalibResult& r) { onMeasured (g, r); };
    calPort_ = std::make_unique<EngineCalPort> (*this);
    cal_ = std::make_unique<ni::plugin::CalController> (*calPort_);
    const bool hook = coord_->hooks().installed();
    ni::host::DiagLog::get().text ("INIT", instNo_, "api=ok|hook=%d|instances=%d|reaper=%s", hook ? 1 : 0,
                                   coord_->instanceCount(), api.GetAppVersion ? api.GetAppVersion() : "?");
    coord_->scheduler().setMaxConcurrent (settings.maxConcurrentProbes);
    coord_->addListener (instNo_, [this] (const ni::host::Coordinator::Event& e) { onCoordinatorEvent (e); });
    coord_->setRenderView (instNo_, [this] { return renderView(); });   // render preflight (F1 §10, Task 21)
    if (! coord_->render().renderWindowOpen)
    {
        coord_->render().renderWindowOpen = &ni::host::platform::renderWindowOpen;
        coord_->render().dialogOpening = &ni::host::platform::renderWindowBeforeDialog;
        coord_->render().dialogOpened = &ni::host::platform::renderWindowAfterDialog;
    }
    library_ = std::make_unique<ni::plugin::DeviceLibrary> (std::filesystem::path (base));
    const auto ls = library_->load();
    ni::plugin::applySettings (library_->settings(), coord_->render());   // keep-online fallback (F1 §10; final review I1)
    ni::host::DiagLog::get().text ("LIB", instNo_, "status=%d|devices=%d|warn=%s", static_cast<int> (ls), static_cast<int> (library_->devices().size()),
                                   library_->warnings().empty() ? "" : library_->warnings().front().c_str());
    evaluateHostState();
    pollHost();
}

void NovaInsertProcessor::onCoordinatorEvent (const ni::host::Coordinator::Event& e)
{
    using Kind = ni::host::Coordinator::Event::Kind;
    ni::host::DiagLog::get().text ("EVENT", instNo_, "kind=%d|reason=%d|gen=%u", static_cast<int> (e.kind), static_cast<int> (e.reason), e.generation);
    switch (e.kind)
    {
        case Kind::Host:
            if (e.reason == Reason::DeviceLost) setState (State::Unavailable, Reason::DeviceLost);
            else if (e.reason == Reason::DeviceReturned) evaluateHostState (Reason::DeviceReturned);
            else if (e.reason == Reason::BufferChange)
            {
                // read the new device key now (lastBufferSize_, then loopCfg_.B via syncInstance and the driver latency in
                // serviceMeasurer, all in this tick) instead of re-evaluating at the old B and again at the next 500 ms poll
                pollHost();
                if (state() != State::Queued) requestMeasure (Reason::BufferChange);
            }
            break;
        case Kind::Promoted:
            pairOwned_ = true; conflicted_ = false;
            ni::host::DiagLog::get().text ("PAIR", instNo_, "promoted|device=%d", claimedDevice_);
            setState (State::Idle, Reason::ConflictResolved);
            requestMeasure (Reason::ConflictResolved);
            break;
        case Kind::Violation:
            ni::host::DiagLog::get().text ("VIOLATION", instNo_, "track=%s", e.text.c_str());
            antViolation_ = true;
            setState (State::UnsupportedHost, Reason::AnticipativeOn);
            break;
        case Kind::ViolationCleared:   // the user set the bit again, or any instance's reapply did
            ni::host::DiagLog::get().text ("VIOLATION", instNo_, "cleared");
            if (antViolation_) { antViolation_ = false; evaluateHostState (Reason::AnticipativeOn); }
            break;
        case Kind::MeasureStart:
            if (e.generation != generation_) break;   // a request this instance has since withdrawn or superseded
            ni::host::DiagLog::get().text ("MEAS_START", instNo_, "gen=%u|reason=%d", e.generation, static_cast<int> (e.reason));
            measureGen_ = e.generation;
            startPending_ = true;   // serviceMeasurer starts it once the loop is active and the transport is not recording
            if (! restore_.pending()) setState (State::Measuring, e.reason);   // a project snapshot may lock without a probe (Task 22)
            break;
    }
}

void NovaInsertProcessor::requestMeasure (Reason r)
{
    if (! apiOk_ || ! pairOwned_) return;
    const auto s = state();
    if (! ni::engine::measureRequestAllowed (s, r)) return;   // Failed only for the user's RE-CALIBRATION
    if (s == State::Measuring || startPending_) cancelMeasure (r);   // a re-evaluation trigger voids the running or granted measurement (F1 §13, S7 review I3)
    if (pdc_) { pdc_->cancel(); pendingPhys_ = -1; syncPdcMute(); }   // a newer request supersedes a pending/awaited PDC transition
    openFallbackOn_ = false;   // a new latency check is under way: the warning returns only if it fails again with a kept snapshot
    generation_ = coord_->nextGeneration();
    coord_->scheduler().request ({ instNo_, generation_, r, false });
    setState (State::Queued, r);
    ni::host::DiagLog::get().text ("SCHED", instNo_, "gen=%u|reason=%d|pos=%d", generation_, static_cast<int> (r), coord_->scheduler().queuePosition (instNo_));
}

void NovaInsertProcessor::applyDeviceParameter()
{
    const int dev = pDevice->get();
    if (dev == claimedDevice_)
    {
        if (! restore_.armed() || restore_.deviceParam() != dev) return;
        // a state load for the device already claimed (undo, preset, a second setState): nothing to redo when the instance
        // already runs the snapshot record; a different record is re-claimed as the restore (S7/S8 review I1/I2)
        if (! restore_.device() || restore_.snapshotEquals (runningRec_)) { restore_.dropPending(); return; }
        claimedDevice_ = -2;   // forces the re-claim below
    }
    const bool first = generation_ == 0;   // first measurement this instance ever requested = load/restore, later claims = user selection
    // Stop this instance's hook I/O before the registry hands the pair to a waiting instance (release() promotes it
    // synchronously): never two writers on one pair, not even for one timer tick (S3 review I2).
    if (inst_ && inst_->active()) releaseLoop ("pair");
    if (claimedDevice_ > 0) { coord_->pairs().release (instNo_); ni::host::DiagLog::get().text ("PAIR", instNo_, "release|device=%d", claimedDevice_); }
    coord_->scheduler().cancel (instNo_);   // a request for the old pair must never start
    claimedDevice_ = dev; pairOwned_ = false; conflicted_ = false; lastWarn_.clear();
    slotDirty_ = true; measureAfterDefaults_.reset();
    lastCalib_.reset(); runningRec_.reset(); openKept_.reset(); openFallbackOn_ = false;
    const bool restore = restore_.claim (dev) == ni::plugin::ProjectRestore::Claim::Restore;   // any other claim is the user's selection
    if (dev == 0)
    {
        ni::host::DiagLog::get().text ("PAIR", instNo_, "none");
        restore_.dropPending();
        if (const auto s = ni::engine::deviceClearedState (state()); s != state()) setState (s, Reason::DeviceSelection);   // no loop: no lock
        if (state() == State::Unavailable) evaluateHostState();
        return;
    }
    const auto rec = activeRecord();
    if (! rec) { restore_.dropPending(); ni::host::DiagLog::get().text ("PAIR", instNo_, "missing|device=%d|library=%d", dev, library_ ? static_cast<int> (library_->devices().size()) : -1); return; }
    pair_ = ni::plugin::recordPair (*rec);
    runningRec_ = rec;
    vuRef_ = rec->vuRefDbfs;
    defaultsPending_ = ! restore;   // the user's selection takes the device defaults (applied outside REAPER's set call)
    defaultsChanged_ = false;
    pairLoopExtra_ = rec->s3LoopExtra;
    char name[256] = "";
    if (track_ != nullptr && coord_->api().GetTrackName) coord_->api().GetTrackName (track_, name, sizeof (name));
    const auto claim = coord_->pairs().claim (instNo_, pair_, name, rec->id);
    if (claim == ni::host::Claim::Owner)
    {
        pairOwned_ = true;
        ni::host::DiagLog::get().text ("PAIR", instNo_, "owner|device=%d|id=%s|out=%d/%d|in=%d/%d|sendStereo=%d|retStereo=%d|ver=%d|vu=%.1f|from=%s", dev, rec->id.c_str(), pair_.out[0], pair_.out[1],
                                       pair_.in[0], pair_.in[1], rec->sendStereo ? 1 : 0, rec->returnStereo ? 1 : 0, rec->version, static_cast<double> (rec->vuRefDbfs), restore_.usesProject (claimedDevice_) ? "project" : "library");
        if (state() == State::Unavailable) evaluateHostState();
        // a restore measures now; a user selection measures on the tick that applies the preset's defaults (its mode is part of
        // the key: requesting first and switching mode after would cancel and requeue, and an Effect preset chosen from Insert
        // would briefly hold an Insert probe request — final review M2)
        if (restore) requestMeasure (Reason::ProjectLoad);
        else measureAfterDefaults_ = first ? Reason::ProjectLoad : Reason::DeviceSelection;
    }
    else
    {
        conflicted_ = true;
        const auto owner = coord_->pairs().ownerTrackOf (pair_);
        ni::host::DiagLog::get().text ("PAIR", instNo_, "conflict|device=%d|owner=%s", dev, owner ? owner->c_str() : "?");
        setState (State::Unavailable, Reason::PairConflict);
    }
}

bool NovaInsertProcessor::resolveTrack()
{
    if (track_ != nullptr) return true;
    if (! apiOk_ || reaperHost_ == nullptr) return false;
    track_ = static_cast<MediaTrack*> (reaperHost_->getReaperParent (1));
    proj_ = static_cast<ReaProject*> (reaperHost_->getReaperParent (3));
    if (track_ == nullptr) return false;
    char name[256] = "";
    if (coord_->api().GetTrackName) coord_->api().GetTrackName (track_, name, sizeof (name));
    ni::host::DiagLog::get().text ("TRACK", instNo_, "name=%s|proj=%d", name, proj_ != nullptr ? 1 : 0);
    coord_->hostSettings().require (instNo_, track_);
    ni::host::DiagLog::get().text ("ANT", instNo_, "require|track=%s", name);
    return true;
}

void NovaInsertProcessor::syncSlot()
{
    if (! apiOk_ || reaperHost_ == nullptr || track_ == nullptr || ! coord_) return;
    // index of this instance in its track's FX chain (REAPER context selector 6, 1-based; it follows moves in the chain)
    const int idx = ni::host::chainIndexFromContext (reinterpret_cast<std::intptr_t> (reaperHost_->getReaperParent (6)));
    const std::string want = claimedDevice_ > 0 && runningRec_ ? runningRec_->name : std::string();
    const auto r = ni::host::syncSlotName (coord_->api(), track_, idx, want);
    if (r == ni::host::SlotSync::Written || idx != lastSlotIdx_)
        ni::host::DiagLog::get().text ("SLOT", instNo_, "idx=%d|res=%d|name=%s", idx, static_cast<int> (r), want.c_str());
    lastSlotIdx_ = idx;
}

void NovaInsertProcessor::parameterValueChanged (int parameterIndex, float)
{
    if (parameterIndex != static_cast<int> (Param::Device)) return;
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();   // never create from the audio thread
    if (apiOk_ && mm != nullptr && mm->isThisTheMessageThread())
    {
        inParamCallback_ = true;
        resolveTrack(); applyDeviceParameter();
        inParamCallback_ = false;
    }
    else deviceDirty_.store (true, std::memory_order_release);
}

void NovaInsertProcessor::pollHost()
{
    if (! apiOk_) return;
    resolveTrack();
    evaluateHostState();
    syncLibrary();   // presets deleted/edited by another instance realign this one even with its editor closed (final review H1)
    applyDeviceParameter();
    syncSlot();   // re-asserted: the slot is not renamable while NovaInsert runs (user 2026-11-02)
    if (pairOwned_)
    {
        std::string w;
        for (const auto& t : coord_->pairs().hardwareSendWarnings (coord_->api(), proj_, pair_)) { if (! w.empty()) w += "; "; w += t; }
        if (w != lastWarn_) { lastWarn_ = w; ni::host::DiagLog::get().text ("WARN", instNo_, "%s", w.empty() ? "none" : w.c_str()); }
    }
    const int pos = coord_->scheduler().queuePosition (instNo_);
    if (pos != lastQueuePos_) { lastQueuePos_ = pos; ni::host::DiagLog::get().text ("QPOS", instNo_, "pos=%d|gen=%u", pos, generation_); }
    if (instNo_ == 0)   // one instance reports the shared hook's view of the device (on change only)
    {
        const auto st = coord_->hooks().stats();
        char line[192];
        std::snprintf (line, sizeof (line), "len=%d|in=%d|out=%d|nullstreak=%d|lastnullrun=%d|firstnull=%d|dropouts=%llu",
                       st.lastLen, st.inputChannels, st.outputChannels, st.nullBufferStreak, st.lastNullRun, st.firstNullChannel,
                       static_cast<unsigned long long> (st.dropouts));
        if (lastHookStat_ != line)
        {
            lastHookStat_ = line;
            ni::host::DiagLog::get().text ("HOOKSTAT", instNo_, "cycles=%llu|%s", static_cast<unsigned long long> (st.cycles), line);
        }
    }
}

void NovaInsertProcessor::evaluateHostState (Reason trigger)
{
    if (! apiOk_) { setState (State::UnsupportedHost, Reason::None); return; }
    const auto& api = coord_->api();
    const auto key = ni::host::readDeviceKey (api);
    char keyLine[512];
    std::snprintf (keyLine, sizeof (keyLine), "in=%s|out=%s|sr=%d|bs=%d|default=%d|open=%d",
                   key.identIn.c_str(), key.identOut.c_str(), key.sampleRate, key.bufferSize, key.isDefault ? 1 : 0, key.open ? 1 : 0);
    if (lastKeyLine_ != keyLine) { lastKeyLine_ = keyLine; ni::host::DiagLog::get().text ("DEVKEY", instNo_, "%s", keyLine); }   // on change only
    if (key.open && key.bufferSize > 0) coord_->hooks().setExpectedBlock (key.bufferSize);   // device BSIZE, not the plugin block
    const bool wasOpen = lastOpen_; lastOpen_ = key.open;
    if (! key.open) { setState (State::Unavailable, Reason::DeviceLost); return; }        // device closed / not opened yet
    Reason pending = Reason::None;   // device key transition seen by polling (Audio_Quit/Audio_Init, BSIZE or device change)
    if (! wasOpen && everOpen_) pending = Reason::DeviceReopen;
    else if (wasOpen && lastBufferSize_ > 0 && key.bufferSize != lastBufferSize_) pending = Reason::BufferChange;
    else if (wasOpen && ! lastIdent_.empty() && lastIdent_ != key.identIn + "|" + key.identOut) pending = Reason::IoChange;
    everOpen_ = true; lastBufferSize_ = key.bufferSize; lastIdent_ = key.identIn + "|" + key.identOut;
    identIn_ = key.identIn; identOut_ = key.identOut;
    if (key.isDefault) { setState (State::Unavailable, Reason::DefaultDevice); return; }
    driverMode_ = key.mode;
    driverOverride_ = false;
    switch (ni::host::driverVerdict (key.mode))
    {
        case ni::host::DriverVerdict::Supported: lastUnsupportedMode_.clear(); break;
        case ni::host::DriverVerdict::Unknown:
            lastUnsupportedMode_.clear();
            if (! driverUnknownLogged_) { driverUnknownLogged_ = true; ni::host::DiagLog::get().text ("DRIVER_UNKNOWN", instNo_, "mode="); }
            break;
        case ni::host::DriverVerdict::Unsupported:
            if (library_ && library_->settings().allowAnyDriver)
            {
                driverOverride_ = true;
                if (! driverOverrideLogged_) { driverOverrideLogged_ = true; ni::host::DiagLog::get().text ("DRIVER_OVERRIDE", instNo_, "%s", key.mode.c_str()); }
                break;
            }
            if (lastUnsupportedMode_ != key.mode)
            {
                lastUnsupportedMode_ = key.mode;
                ni::host::DiagLog::get().text ("DRIVER_UNSUPPORTED", instNo_, "%s", key.mode.c_str());
            }
            setState (State::Unavailable, Reason::DriverUnsupported);
            return;
    }
    const double prepared = preparedSampleRate_.load (std::memory_order_acquire);
    const int sr = key.sampleRate > 0 ? key.sampleRate : static_cast<int> (prepared);
    if (sr != ni::engine::kSampleRate || (prepared > 0.0 && static_cast<int> (prepared) != ni::engine::kSampleRate))
    { setState (State::UnsupportedSampleRate, Reason::SampleRate); return; }
    if (! coord_->hooks().installed()) { setState (State::Unavailable, Reason::DeviceLost); return; }
    // The hook is the authority on buffers: a device the key reports open but that delivers no buffers stays
    // Unavailable until DeviceReturned (F1 §5/§15); the poll must not override the hook's DeviceLost.
    // the hook's debounced verdict, or a null run that stopped calling back (S6 review I2) — not a raw streak (Task 21)
    if (coord_->hooks().deviceLostNow()) { setState (State::Unavailable, Reason::DeviceLost); return; }
    if (state() == State::Unavailable && lastReason_ == Reason::DeviceLost && trigger == Reason::None) trigger = Reason::DeviceReturned;
    if (antViolation_) { setState (State::UnsupportedHost, Reason::AnticipativeOn); return; }
    if (conflicted_) { setState (State::Unavailable, Reason::PairConflict); return; }
    if (state() == State::UnsupportedHost || state() == State::Unavailable || state() == State::UnsupportedSampleRate)
        setState (State::Idle, Reason::None);
    // host usable again: labelled by what made it so; a recording abort waits for the record path (reason Recording)
    if (const Reason r = ni::engine::hostPollRequeue (state(), pending, trigger, requeueAfterRecord_, measureAfterDefaults_.has_value()); r != Reason::None) requestMeasure (r);
}

void NovaInsertProcessor::setState (State s, Reason r)
{
    if (s == state() && r == lastReason_) return;
    state_.store (s, std::memory_order_release);
    lastReason_ = r;
    setReadOnly (Param::State, static_cast<int> (s));
    ni::host::DiagLog::get().text ("STATE", instNo_, "state=%d|reason=%d", static_cast<int> (s), static_cast<int> (r));
}

void NovaInsertProcessor::setReadOnly (Param p, int value)
{
    // REAPER drops an edit made inside its own parameter set call, yet JUCE would keep the new value and the timer's re-push
    // would then see no change: inside the callback, leave the value for the timer (f3_5 ap_pdc_reset, 2026-11-01)
    if (inParamCallback_) return;
    juce::AudioParameterInt* param = p == Param::State ? pState : p == Param::PhysLoop ? pPhysLoop
                                   : p == Param::ReportedPdc ? pReportedPdc : p == Param::CalStatus ? pCalStatus : pInherentGap;
    if (param != nullptr && param->get() != value) *param = value;
}

void NovaInsertProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedSampleRate_.store (sampleRate, std::memory_order_release);
    ni::host::DiagLog::get().text ("PREPARE", instNo_, "sr=%.0f|block=%d|nonrt=%d", sampleRate, samplesPerBlock, isNonRealtime() ? 1 : 0);
    hostDirty_.store (true, std::memory_order_release);   // evaluated on the timer: the host may call this off the message thread
}

void NovaInsertProcessor::timerCallback()
{
    if (pReapplyAnt->get())
    {
        *pReapplyAnt = false;   // momentary
        if (coord_)
        {
            const bool did = coord_->hostSettings().reapply (instNo_);
            ni::host::DiagLog::get().text ("ANT", instNo_, "reapply|set=%d", did ? 1 : 0);
            if (antViolation_) { antViolation_ = false; evaluateHostState (Reason::AnticipativeOn); }
        }
    }
    if (! apiOk_) return;
    // re-push every read-only value: REAPER drops edits made inside its own parameter set call (a device change runs there and
    // releases the loop; f3_5 ap_pdc_reset showed PhysLoop/ReportedPdc stuck at the released loop's values)
    setReadOnly (Param::State, static_cast<int> (state()));
    {
        const bool act = inst_ && inst_->active() && loopCfg_.B > 0;
        setReadOnly (Param::PhysLoop, act ? inst_->physLoop() : 0);
        setReadOnly (Param::ReportedPdc, getLatencySamples());
        if (! act) setReadOnly (Param::InherentGap, 0);
    }
    coord_->tick (static_cast<int64_t> (juce::Time::getMillisecondCounterHiRes()));
    if (hostDirty_.exchange (false, std::memory_order_acq_rel)) evaluateHostState();
    if (deviceDirty_.exchange (false, std::memory_order_acq_rel)) { resolveTrack(); applyDeviceParameter(); }
    if (slotDirty_ && ! inParamCallback_) { slotDirty_ = false; syncSlot(); }
    // Every 500 ms: own track (available only after the host connected us), device key transitions
    // (Audio_Quit/Audio_Init, BSIZE), device parameter, hardware-send warnings, queue position.
    if (++ticks_ >= 25) { ticks_ = 0; pollHost(); }
    // the mode is part of the calibration key (F1 §4): a change re-evaluates the latency source (Insert probe / Effect driver)
    if (const int m = pMode->getIndex(); m != lastModeIdx_)
    {
        const bool first = lastModeIdx_ < 0;
        lastModeIdx_ = m;
        if (! first) { ni::host::DiagLog::get().text ("MODE", instNo_, "mode=%d", m); requestMeasure (Reason::KeyChange); }
    }
    if (defaultsPending_ && ! inParamCallback_)
    {
        defaultsPending_ = false;
        if (runningRec_) applyRecordDefaults (*runningRec_);
        if (const int m = pMode->getIndex(); m != lastModeIdx_) { lastModeIdx_ = m; ni::host::DiagLog::get().text ("MODE", instNo_, "mode=%d|from=preset", m); }
        if (measureAfterDefaults_) { const auto r = *measureAfterDefaults_; measureAfterDefaults_.reset(); requestMeasure (r); }
    }
    serviceLink();
    serviceCal();
    syncInstance();
    serviceMeasurer();
    if (pdc_)
    {
        const auto now = static_cast<int64_t> (juce::Time::getMillisecondCounterHiRes());
        const bool playing = playingNow();
        if (playing != lastPlaying_) { lastPlaying_ = playing; ni::host::DiagLog::get().text ("PDC", instNo_, "transport|playing=%d", playing ? 1 : 0); }
        pdc_->onTransport (playing, now);   // deferred R applied at stop; the stopped-change window starts at play
        pdc_->tick (now);
        syncPdcMute();   // follows the reporter (lifted at stop after the last retry); re-asserted over the Measurer's delayed unmute
    }
    logLoop (false);
    logMeters();
}

void NovaInsertProcessor::logMeters()
{
    if (! ni::host::DiagLog::get().enabled() || ++meterTicks_ < 50) return;   // 1 s
    meterTicks_ = 0;
    const auto s = sendMeter_.read (vuRef_), r = returnMeter_.read (vuRef_);
    constexpr float kSilent = -100.0f;
    if (s.rmsDb[0] < kSilent && s.rmsDb[1] < kSilent && r.rmsDb[0] < kSilent && r.rmsDb[1] < kSilent) return;
    auto d = [] (float x) { return static_cast<double> (x); };
    ni::host::DiagLog::get().text ("METER", instNo_, "send=%.2f/%.2f|sendVu=%.2f/%.2f|sendPk=%.2f/%.2f|ret=%.2f/%.2f|retVu=%.2f/%.2f|retPk=%.2f/%.2f|vuRef=%.1f|trim=%.2f/%.2f",
                                   d (s.rmsDb[0]), d (s.rmsDb[1]), d (s.vu[0]), d (s.vu[1]), d (s.peakDb[0]), d (s.peakDb[1]),
                                   d (r.rmsDb[0]), d (r.rmsDb[1]), d (r.vu[0]), d (r.vu[1]), d (r.peakDb[0]), d (r.peakDb[1]), d (vuRef_),
                                   d (pSendTrim->get()), d (pReturnTrim->get()));
}

void NovaInsertProcessor::syncPdcMute()
{
    const bool m = pdc_ != nullptr && pdc_->muted();
    if (m == pdcMuted_ && ! m) return;
    pdcMuted_ = m;
    if (inst_) inst_->engine().setMuted (m);
}

ni::host::RenderArbiter::InstanceView NovaInsertProcessor::renderView() const
{
    char name[256] = "";
    if (track_ != nullptr && coord_ && coord_->api().GetTrackName) coord_->api().GetTrackName (track_, name, sizeof (name));
    return { instNo_, state(), pMode->getIndex() == 1 ? ni::engine::Mode::Effect : ni::engine::Mode::Insert,
             inst_ ? inst_->lastProcessedMs() : -1, false, name[0] ? name : "track" };
}

bool NovaInsertProcessor::playingNow() const
{
    const auto& api = coord_->api();
    const int ps = api.GetPlayStateEx != nullptr ? api.GetPlayStateEx (proj_) : api.GetPlayState != nullptr ? api.GetPlayState() : 0;
    return (ps & 1) != 0 && (ps & 2) == 0;   // paused counts as stopped (F1 §9 defers to stop/pause)
}

bool NovaInsertProcessor::applyPdcState (State st, Reason r, uint32_t gen, const char* diag)
{
    if (gen != pdcGen_ || gen != generation_) return false;   // superseded: a newer request owns the state
    ni::host::DiagLog::get().text ("PDC", instNo_, "state=%d|reason=%d|gen=%u|%s", static_cast<int> (st), static_cast<int> (r), gen, diag);
    if (st == State::Locked) { lkgValid_ = true; lkgCfg_ = loopCfg_; }
    syncPdcMute();   // the reporter sets/clears its mute before it reports the state (F1 §9 Q10)
    setState (st, r);
    return true;
}

void NovaInsertProcessor::releaseLoop (const char* why)
{
    cancelMeasure (Reason::DeviceLost);
    if (cal_ && cal_->active()) { cal_->cancel(); ni::host::DiagLog::get().text ("CAL", instNo_, "cancelled|why=%s", why); }
    if (inst_) inst_->release();
    loopCfg_ = {}; loopRouting_ = -1;
    if (pdc_) { pdc_->cancel(); pdc_->noteReported (0); }
    syncPdcMute();
    pendingPhys_ = -1;
    setLatencySamples (0);
    setReadOnly (Param::PhysLoop, 0); setReadOnly (Param::ReportedPdc, 0); setReadOnly (Param::InherentGap, 0);
    ni::host::DiagLog::get().text ("LOOPCFG", instNo_, "released|why=%s|state=%d", why, static_cast<int> (state()));
}

void NovaInsertProcessor::syncInstance()
{
    if (! inst_) return;
    const auto st = state();
    const bool usable = st != State::Unavailable && st != State::UnsupportedHost && st != State::UnsupportedSampleRate;
    const auto eff = effectivePair();   // a STEREO direction narrowed to one side by its selector runs as a mono direction on that channel
    const LoopCfg want { eff, (apiOk_ && pairOwned_ && usable) ? lastBufferSize_ : 0 };
    if (want.B <= 0)
    {
        if (loopCfg_.B != 0) releaseLoop ("state");
        return;
    }
    ni::engine::Routing routing { eff.sendStereo, eff.retStereo, pSendTrim->get(), pReturnTrim->get() };
    for (int ch = 0; ch < 2; ++ch) { routing.sendOffsetDb[ch] = sendOff_[ch]; routing.returnOffsetDb[ch] = retOff_[ch]; }
    const int packed = (pSendSel->getIndex() << 2) | pReturnSel->getIndex();
    if (! (want == loopCfg_))
    {
        // a side change on a running loop moves the physical path: the loop is rebuilt and measured again (RoutingChange)
        const bool sideChange = loopCfg_.B > 0 && want.B == loopCfg_.B && packed != loopRouting_;
        if (pdc_) { pdc_->cancel(); syncPdcMute(); }   // a transition measured for the old pair/B must never be applied to the new one
        pendingPhys_ = -1;
        if (! inst_->configure (eff, routing, want.B, pairLoopExtra_))
        {
            ni::host::DiagLog::get().text ("LOOPCFG", instNo_, "rejected|B=%d|out=%d/%d|in=%d/%d", want.B, eff.out[0], eff.out[1], eff.in[0], eff.in[1]);
            releaseLoop ("rejected");                 // configure already stopped the hook I/O; report no latency either
            loopCfg_ = want; loopRouting_ = packed;   // do not retry every tick; a pair/B change retries
            return;
        }
        loopCfg_ = want; loopRouting_ = packed;
        physOrigin_ = "PROVISIONAL";
        setLatencySamples (inst_->reportedLatency());
        if (pdc_) pdc_->noteReported (inst_->reportedLatency());   // base of the next measured transition
        setReadOnly (Param::PhysLoop, inst_->physLoop());
        setReadOnly (Param::ReportedPdc, inst_->reportedLatency());
        ni::host::DiagLog::get().text ("LOOPCFG", instNo_, "physLoop_origin=%s|phys=%d|R=%d|B=%d|out=%d/%d|in=%d/%d|sendStereo=%d|retStereo=%d|send=%d|ret=%d",
                                       physOrigin_, inst_->physLoop(), inst_->reportedLatency(), want.B, eff.out[0], eff.out[1], eff.in[0], eff.in[1],
                                       eff.sendStereo ? 1 : 0, eff.retStereo ? 1 : 0, pSendSel->getIndex(), pReturnSel->getIndex());
        logLoop (true);
        if (sideChange) requestMeasure (Reason::RoutingChange);
        return;
    }
    if (packed != loopRouting_)
    {
        loopRouting_ = packed;
        ni::host::DiagLog::get().text ("LOOPCFG", instNo_, "routing|send=%d|ret=%d", pSendSel->getIndex(), pReturnSel->getIndex());
    }
    inst_->engine().setRouting (routing);   // RT-safe atomics; trims and selectors take effect at the next block
}

bool NovaInsertProcessor::recordingNow() const
{
    const auto& api = coord_->api();
    return api.GetPlayStateEx != nullptr ? (api.GetPlayStateEx (proj_) & 4) != 0 : api.GetPlayState != nullptr && (api.GetPlayState() & 4) != 0;
}

void NovaInsertProcessor::serviceMeasurer()
{
    if (! measurer_ || ! coord_) return;
    const bool rec = recordingNow();
    if (recordCut_.exchange (false, std::memory_order_acq_rel) || (rec && measurer_->busy()))
    {
        if (measurer_->busy())
        {
            measurer_->abortForRecording();
            measuringRt_.store (false, std::memory_order_release);
            coord_->scheduler().cancel (instNo_);   // no retry now: requested again once recording stops
            requeueAfterRecord_ = true;
            setState (State::Idle, Reason::Recording);
            ni::host::DiagLog::get().text ("MEAS", instNo_, "gen=%u|aborted|reason=%d", measureGen_, static_cast<int> (Reason::Recording));
        }
    }
    if (requeueAfterRecord_ && ! rec && state() == State::Idle)
    {
        requeueAfterRecord_ = false;
        requestMeasure (Reason::Recording);
    }
    if (startPending_ && ! rec && ! (cal_ && cal_->active()) && inst_ && inst_->active() && loopCfg_.B > 0)   // never under the CAL tone
    {
        startPending_ = false;
        const auto key = currentKey();
        if (const auto pending = restore_.takePending())   // the restore claim's slot: a matching snapshot locks without a probe (F1 §16)
        {
            const auto snap = pending->calibSnapshot;
            const int32_t sp = snap ? snap->result.physLoop[0] : -1;
            const bool inRange = sp >= 0 && sp <= ni::engine::kLMax - 2 * loopCfg_.B;   // same bound as a measured result (review M1)
            const bool onOpen = library_ ? library_->settings().measureOnOpen : true;
            const auto action = key && inRange ? ni::plugin::restoreAction (*pending, *key, onOpen) : ni::plugin::RestoreAction::Remeasure;
            const bool apply = action == ni::plugin::RestoreAction::ApplySnapshot;
            if (action == ni::plugin::RestoreAction::MeasureKeepSnapshot) openKept_ = snap;   // measure now; the snapshot is the fallback
            ni::host::DiagLog::get().text ("STATE", instNo_, "restore|gen=%u|apply=%d|keep=%d|phys=%d|B=%d", measureGen_, apply ? 1 : 0,
                                           openKept_ ? 1 : 0, snap ? snap->result.physLoop[0] : -1, loopCfg_.B);
            if (apply)
            {
                lastCalib_ = snap;
                acceptResult (measureGen_, snap->result, Reason::StateRestore, "SNAPSHOT");
                measurer_->tick();
                return;
            }
        }
        // Effect mode never probes (Effect PDC spec 2026-11-04 §2): the driver's own round-trip (GetInputOutputLatency in + out
        // at the current buffer size, message thread) is the loop latency, or the instance fails without a probe
        int driverIn = -1, driverOut = -1;
        if (pMode->getIndex() == 1 && coord_->api().GetInputOutputLatency) coord_->api().GetInputOutputLatency (&driverIn, &driverOut);
        if (const auto src = ni::plugin::latencySource (pMode->getIndex() == 1 ? ni::engine::Mode::Effect : ni::engine::Mode::Insert,
                                                        driverIn, driverOut, loopCfg_.B);
            ! src.probe)
        {
            ni::host::DiagLog::get().text ("MEAS", instNo_, "gen=%u|driver|in=%d|out=%d|phys=%d|fail=%d|B=%d", measureGen_, driverIn, driverOut,
                                           src.phys, static_cast<int> (src.fail), loopCfg_.B);
            if (src.fail == Reason::None)
            {
                ni::engine::CalibResult lib {};
                lib.physLoop[0] = lib.physLoop[1] = src.phys;
                lib.timestampUnix = static_cast<int64_t> (juce::Time::currentTimeMillis() / 1000);
                acceptResult (measureGen_, lib, Reason::None, "DRIVER");
            }
            else if (! useOpenFallback (measureGen_, src.fail))   // the kept snapshot replaces the failure (no retry is queued then)
            {
                coord_->scheduler().finished (instNo_, measureGen_, false, src.fail);
                setState (State::Failed, src.fail);
            }
            measurer_->tick();
            return;
        }
        if (state() != State::Measuring) setState (State::Measuring, lastReason_);   // the snapshot did not match: probe after all
        const auto history = ni::plugin::levelHistoryFrom (key && library_ ? library_->calibration (*key) : std::nullopt);
        measuringRt_.store (true, std::memory_order_release);
        measurer_->start (measureGen_, history, loopCfg_.pair.retStereo, loopCfg_.B);   // the correlator reads the effective return width
        ni::host::DiagLog::get().text ("MEAS", instNo_, "gen=%u|start|B=%d|sendStereo=%d|retStereo=%d", measureGen_, loopCfg_.B, loopCfg_.pair.sendStereo ? 1 : 0, loopCfg_.pair.retStereo ? 1 : 0);
    }
    measurer_->tick();
}

void NovaInsertProcessor::cancelMeasure (Reason why)
{
    startPending_ = false;
    measuringRt_.store (false, std::memory_order_release);
    if (measurer_ && measurer_->busy())
    {
        measurer_->cancel (why);
        ni::host::DiagLog::get().text ("MEAS", instNo_, "gen=%u|cancelled|reason=%d", measureGen_, static_cast<int> (why));
    }
}

void NovaInsertProcessor::onMeasured (uint32_t gen, const ni::engine::CalibResult& r)
{
    measuringRt_.store (false, std::memory_order_release);
    ni::host::DiagLog::get().text ("MEAS", instNo_,
        "gen=%u|result|reason=%d|phys=%d/%d|frac=%.3f/%.3f|neg=%d/%d|send=%.2f|ret=%.2f|gain=%.2f|pbr=%.1f/%.1f|snr=%.1f/%.1f",
        gen, static_cast<int> (r.failReason), r.physLoop[0], r.physLoop[1], static_cast<double> (r.fractional[0]), static_cast<double> (r.fractional[1]),
        r.negativePolarity[0] ? 1 : 0, r.negativePolarity[1] ? 1 : 0, static_cast<double> (r.sendPeakDb), static_cast<double> (r.returnPeakDb),
        static_cast<double> (r.gainPeakDb), static_cast<double> (r.pbrDb[0]), static_cast<double> (r.pbrDb[1]),
        static_cast<double> (r.snrDb[0]), static_cast<double> (r.snrDb[1]));
    if (gen != generation_ || ! coord_) return;   // superseded while it ran
    const int32_t phys = r.physLoop[0];
    const int B = loopCfg_.B;
    if (r.failReason == Reason::None && inst_ && inst_->active() && B > 0 && phys >= 0 && phys <= ni::engine::kLMax - 2 * B)
    {
        // the calibration store (F1 §8/§16): keyed exactly, written on the message thread, never on the audio thread
        const auto key = currentKey();
        const auto rec = activeRecord();
        if (key && rec)
        {
            const bool stored = library_->storeCalibration ({ *key, rec->id, r, 0 });
            lastCalib_ = stored ? library_->calibration (*key) : std::optional<ni::plugin::CalibRecord> (ni::plugin::CalibRecord { *key, rec->id, r, 0 });
            ni::host::DiagLog::get().text ("CALIB", instNo_, "stored=%d|device=%s|phys=%d|B=%d|version=%d", stored ? 1 : 0, rec->id.c_str(), phys, B,
                                           lastCalib_ ? lastCalib_->version : 0);
        }
        acceptResult (gen, r, Reason::None, "MEASURED");
        return;
    }
    const Reason why = r.failReason != Reason::None ? r.failReason : Reason::OutOfRange;
    coord_->scheduler().finished (instNo_, gen, false, why);
    if (coord_->scheduler().queuePosition (instNo_) >= 0) { setState (State::Queued, why); return; }   // the retry
    if (! useOpenFallback (gen, why)) setState (State::Failed, why);
}

bool NovaInsertProcessor::useOpenFallback (uint32_t gen, Reason why)
{
    const auto fb = ni::plugin::openFallback (openKept_, true, currentKey());
    openKept_.reset();   // used once: a later failure is a plain failure
    if (! fb) return false;
    lastCalib_ = fb;
    openFallbackOn_ = true;
    openFallbackReason_ = why;
    ni::host::DiagLog::get().text ("STATE", instNo_, "open_fallback|gen=%u|reason=%d|phys=%d|B=%d", gen, static_cast<int> (why), fb->result.physLoop[0], loopCfg_.B);
    acceptResult (gen, fb->result, Reason::StateRestore, "SNAPSHOT_FALLBACK");
    return true;
}

void NovaInsertProcessor::acceptResult (uint32_t gen, const ni::engine::CalibResult& r, Reason why, const char* origin)
{
    const int32_t phys = r.physLoop[0];
    const int B = loopCfg_.B;
    lastMeasuredUnix_ = r.timestampUnix;
    coord_->scheduler().finished (instNo_, gen, true, Reason::None);
    if (antViolation_) { setState (State::UnsupportedHost, Reason::AnticipativeOn); return; }   // F1 §9: not even attempted
    if (std::strcmp (origin, "SNAPSHOT_FALLBACK") != 0) { openKept_.reset(); openFallbackOn_ = false; }   // a real lock ends the fallback
    const int32_t R = ni::plugin::Instance::reportedFor (phys, B);
    pendingPhys_ = phys;
    pendingOrigin_ = origin;
    pdcGen_ = gen;
    ni::host::DiagLog::get().text ("LOOPCFG", instNo_, "physLoop_origin=%s|phys=%d|R=%d|B=%d|gen=%u", origin, phys, R, B, gen);
    // the transport read now, not the timer's last edge: a play started since must still defer the change (S5 I1)
    pdc_->request (R, true, playingNow(), gen, B, static_cast<int64_t> (juce::Time::getMillisecondCounterHiRes()), why);   // ApplyingLatency
}

std::optional<ni::plugin::DeviceRecord> NovaInsertProcessor::activeRecord() const
{
    if (claimedDevice_ <= 0) return std::nullopt;
    return restore_.record (claimedDevice_, library_ ? library_->byParameter (claimedDevice_) : std::nullopt);
}

std::optional<ni::engine::CalibKey> NovaInsertProcessor::currentKey() const
{
    const auto rec = activeRecord();
    if (! rec || ! coord_ || loopCfg_.B <= 0 || identIn_.empty()) return std::nullopt;
    const auto& api = coord_->api();
    ni::plugin::ChannelNames names;
    for (int c = 0; c < 2; ++c)
    {
        if (rec->out[c] >= 0 && api.GetOutputChannelName) if (const char* n = api.GetOutputChannelName (rec->out[c])) names.send[c] = n;
        if (rec->in[c] >= 0 && api.GetInputChannelName) if (const char* n = api.GetInputChannelName (rec->in[c])) names.ret[c] = n;
    }
    return ni::plugin::calibKeyFor (*rec, identIn_, identOut_, ni::engine::kSampleRate, loopCfg_.B,
                                    pMode->getIndex() == 1 ? ni::engine::Mode::Effect : ni::engine::Mode::Insert,
                                    sendSide(), returnSide(), names);
}

ni::engine::SideSel NovaInsertProcessor::sendSide() const noexcept { return static_cast<ni::engine::SideSel> (juce::jlimit (0, 2, pSendSel->getIndex())); }
ni::engine::SideSel NovaInsertProcessor::returnSide() const noexcept { return static_cast<ni::engine::SideSel> (juce::jlimit (0, 2, pReturnSel->getIndex())); }
ni::host::Pair NovaInsertProcessor::effectivePair() const noexcept { return ni::plugin::effectivePair (pair_, sendSide(), returnSide()); }

void NovaInsertProcessor::logLoop (bool force)
{
    if (! inst_) return;
    const auto s = inst_->engine().status();
    if (loopCfg_.B > 0) setReadOnly (Param::InherentGap, static_cast<int> (juce::jlimit<int64_t> (0, P::kPhysLoopMax, s.inherentGap)));
    char key[256];
    std::snprintf (key, sizeof (key), "seg=%llu|gap=%lld|under=%llu|late=%llu|nan=%llu|cont=%d|lag=%d|ovf=%llu|active=%d",
                   static_cast<unsigned long long> (s.segments), static_cast<long long> (s.inherentGap), static_cast<unsigned long long> (s.holdUnderruns),
                   static_cast<unsigned long long> (s.lateDrops), static_cast<unsigned long long> (s.nanFlushed), s.continuityOk ? 1 : 0, s.lagObserved,
                   static_cast<unsigned long long> (s.sendOverflows), inst_->active() ? 1 : 0);
    const bool periodic = inst_->active() && ++loopTicks_ >= 50;   // 1 s
    if (! force && ! periodic && lastLoopKey_ == key) return;
    if (periodic) loopTicks_ = 0;
    lastLoopKey_ = key;
    const auto hs = coord_ ? coord_->hooks().stats() : ni::host::HookDispatcher::Stats {};
    ni::host::DiagLog::get().text ("LOOP", instNo_, "physLoop_origin=%s|phys=%d|R=%d|B=%d|%s|stable=%d|send=%llu|dropouts=%llu",
                                   physOrigin_, inst_->physLoop(), inst_->reportedLatency(), loopCfg_.B, key, s.stableCycles,
                                   static_cast<unsigned long long> (inst_->sendFrames()), static_cast<unsigned long long> (hs.dropouts));
}

bool NovaInsertProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void NovaInsertProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // F1 §5: offline/non-realtime calls never touch the rings (Insert outputs silence, Effect passes through).
    const int nch = juce::jmin (2, buffer.getNumChannels());
    auto* inst = instRt_.load (std::memory_order_acquire);
    if (inst != nullptr && nch > 0 && ! isNonRealtime())
    {
        bool playing = false, recording = false; int64_t pos = 0;
        if (auto* ph = getPlayHead())
            if (auto info = ph->getPosition())
            {
                recording = info->getIsRecording();
                if (auto t = info->getTimeInSamples()) { playing = info->getIsPlaying(); pos = *t; }
            }
        const bool meas = measuringRt_.load (std::memory_order_acquire);
        if (recording && (meas || calRt_.load (std::memory_order_acquire)))
        {
            inst->engine().cutProbe();   // RT: the probe (or CAL tone) stops at the next hook block (plan Review Focus 3)
            if (meas) recordCut_.store (true, std::memory_order_release);   // the CAL controller sees the recording on its tick
        }
        float* io[2] { buffer.getWritePointer (0), nch > 1 ? buffer.getWritePointer (1) : nullptr };
        const int n = buffer.getNumSamples();
        inst->engine().setTrims (pSendTrim->get(), pReturnTrim->get());   // block-accurate trims (automation, render); ramps across the block
        if (inst->process (io, nch, n, pos, playing)) return;   // meters are tapped in the hook (send and return both after the trim)
    }
    float* io[2] { buffer.getNumChannels() > 0 ? buffer.getWritePointer (0) : nullptr, nch > 1 ? buffer.getWritePointer (1) : nullptr };
    ni::plugin::applyFallback (pMode->getIndex() == 1 ? ni::engine::Mode::Effect : ni::engine::Mode::Insert, io, nch, buffer.getNumSamples());
}

juce::AudioProcessorEditor* NovaInsertProcessor::createEditor() { return new ni::gui::NovaInsertEditor (*this); }

ni::gui::ViewInputs NovaInsertProcessor::viewInputs() const
{
    ni::gui::ViewInputs in;
    const auto rec = activeRecord();
    if (rec) in.deviceName = rec->name;
    in.interfaceName = lastIdent_.substr (0, lastIdent_.find ('|'));
    if (const auto bar = lastIdent_.find ('|'); bar != std::string::npos) in.interfaceOutName = lastIdent_.substr (bar + 1);
    const auto eff = effectivePair();
    auto chans = [&] (const char* what, const int* c, bool stereo) {
        if (c[0] < 0) return std::string ("\u2014");
        return std::string (what) + " " + std::to_string (c[0] + 1) + (stereo && c[1] >= 0 ? "/" + std::to_string (c[1] + 1) : "");
    };
    in.outName = chans ("Out", eff.out, eff.sendStereo);
    in.inName = chans ("In", eff.in, eff.retStereo);
    in.mode = pMode->getIndex() == 1 ? ni::engine::Mode::Effect : ni::engine::Mode::Insert;
    if (apiOk_ && coord_ && coord_->api().GetAppVersion) in.hostVersion = coord_->api().GetAppVersion();   // F1 §15 label
    in.state = state();
    in.reason = lastReason_;
    in.driverMode = driverMode_;
    in.driverOverride = driverOverride_;
    const bool active = inst_ && inst_->active();
    in.physLoop = active ? inst_->physLoop() : -1;
    in.reportedPdc = active ? getLatencySamples() : -1;
    if (coord_ && coord_->api().GetInputOutputLatency) coord_->api().GetInputOutputLatency (&in.driverIn, &in.driverOut);
    in.gapSamples = pInherentGap->get();
    in.timestampUnix = lastMeasuredUnix_;
    in.preRollLeadIn = pPreRoll->get();
    if (antViolation_)
    {
        char name[256] = "this track";
        if (track_ != nullptr && coord_ && coord_->api().GetTrackName) coord_->api().GetTrackName (track_, name, sizeof (name));
        in.anticipativeViolationTracks.push_back (name);
    }
    if (conflicted_) in.conflictWith = "another instance";
    in.staleSnapshot = restore_.stale() && restore_.usesProject (claimedDevice_);
    in.openFallback = openFallbackOn_;
    in.openFallbackReason = openFallbackReason_;
    in.measureOnOpen = library_ ? library_->settings().measureOnOpen : true;
    in.skin = library_ ? library_->settings().skin : 0;
    in.reverseWheel = library_ ? library_->settings().reverseWheel : false;
    if (library_ && ! library_->warnings().empty()) in.libraryWarning = library_->warnings().front();
    in.sendStereo = eff.sendStereo; in.retStereo = eff.retStereo;
    in.recSendStereo = pair_.sendStereo; in.recReturnStereo = pair_.retStereo;
    in.link = pLink->get();
    in.defaultsChanged = defaultsChanged_;
    if (rec) { in.sendDbuAt0dBFS = rec->sendDbuAt0dBFS; in.returnDbuAt0dBFS = rec->returnDbuAt0dBFS; in.outboardMaxInDbu = rec->outboardMaxInDbu; }
    in.vuRefDbfs = vuRef_;
    in.calStatus = static_cast<int> (calStatus());
    in.sendTrimDb = pSendTrim->get(); in.returnTrimDb = pReturnTrim->get();
    for (int ch = 0; ch < 2; ++ch) { in.sendOffsetDb[ch] = sendOff_[ch]; in.returnOffsetDb[ch] = retOff_[ch]; }
    in.devices = deviceList();
    in.outChannelNames = outNames_; in.inChannelNames = inNames_;
    if (library_) in.records = library_->devices();
    return in;
}

// --- DEVICES page (spec 2026-11-05) -----------------------------------------------------------------------------------------

int NovaInsertProcessor::libraryIndexOf (const std::string& id) const
{
    return library_ ? library_->indexOf (id) : 0;
}

void NovaInsertProcessor::realignClaim()
{
    if (claimedDevice_ <= 0 || ! runningRec_) return;
    const int idx = libraryIndexOf (runningRec_->id);
    if (idx == claimedDevice_) return;
    ni::host::DiagLog::get().text ("LIB", instNo_, "realign|id=%s|from=%d|to=%d", runningRec_->id.c_str(), claimedDevice_, idx);
    if (idx == 0) { selectDeviceFromGui (0); return; }   // the running preset vanished from the library (hand edit): release
    restore_.reindex (idx);   // a kept project snapshot stays the running record under its new value (final review H2)
    claimedDevice_ = idx;   // same record, new index: no release, no re-measure (applyDeviceParameter sees dev == claimedDevice_)
    selectDeviceFromGui (idx);
}

void NovaInsertProcessor::syncLibrary()
{
    if (library_ && library_->reloadIfChanged())
    {
        ni::host::DiagLog::get().text ("LIB", instNo_, "reload|devices=%d", static_cast<int> (library_->devices().size()));
        realignClaim();
    }
}

void NovaInsertProcessor::setMeasureOnOpen (bool on)
{
    if (! library_) return;
    const bool ok = library_->setMeasureOnOpen (on);
    ni::host::DiagLog::get().text ("LIB", instNo_, "measureOnOpen=%d|saved=%d", on ? 1 : 0, ok ? 1 : 0);
}

void NovaInsertProcessor::setReverseWheel (bool on)
{
    if (! library_) return;
    const bool ok = library_->setReverseWheel (on);
    ni::host::DiagLog::get().text ("LIB", instNo_, "reverseWheel=%d|saved=%d", on ? 1 : 0, ok ? 1 : 0);
}

void NovaInsertProcessor::setSkin (int skin)
{
    if (! library_) return;
    const bool ok = library_->setSkin (skin);
    ni::host::DiagLog::get().text ("LIB", instNo_, "skin=%d|saved=%d", skin, ok ? 1 : 0);
}

void NovaInsertProcessor::pollLibrary()
{
    syncLibrary();
    if (library_) library_->reloadSettings();   // another instance's SETUP toggle (global)
    outNames_.clear(); inNames_.clear();
    if (! apiOk_ || ! coord_) return;
    const auto& api = coord_->api();
    const int no = api.GetNumAudioOutputs ? api.GetNumAudioOutputs() : 0, ni = api.GetNumAudioInputs ? api.GetNumAudioInputs() : 0;
    for (int i = 0; i < no; ++i) { const char* n = api.GetOutputChannelName ? api.GetOutputChannelName (i) : nullptr; outNames_.emplace_back (n ? n : ""); }
    for (int i = 0; i < ni; ++i) { const char* n = api.GetInputChannelName ? api.GetInputChannelName (i) : nullptr; inNames_.emplace_back (n ? n : ""); }
}

std::string NovaInsertProcessor::saveDevice (ni::plugin::DeviceRecord r)
{
    if (! library_) return "No device library";
    pollLibrary();
    r.identIn = identIn_; r.identOut = identOut_;
    if (! r.sendStereo) r.out[1] = -1;
    if (! r.returnStereo) r.in[1] = -1;
    if (const auto err = library_->validate (r, static_cast<int> (outNames_.size()), static_cast<int> (inNames_.size())); ! err.empty()) return err;
    const bool isNew = r.id.empty();
    if (isNew) { r.id = library_->newId (r.name); r.version = 0; }
    else if (const auto lib = library_->device (r.id))
    {   // the form never carries the CAL results: trims, LINK, offsets and the loop extra stay as stored
        r.version = lib->version; r.sendTrimDb = lib->sendTrimDb; r.returnTrimDb = lib->returnTrimDb; r.link = lib->link; r.s3LoopExtra = lib->s3LoopExtra;
        for (int ch = 0; ch < 2; ++ch) { r.sendOffsetDb[ch] = lib->sendOffsetDb[ch]; r.returnOffsetDb[ch] = lib->returnOffsetDb[ch]; }
    }
    else r.version = 0;
    const bool wasRunning = runningRec_ && runningRec_->id == r.id && claimedDevice_ > 0;
    const bool routingChanged = wasRunning && ! (runningRec_->sendStereo == r.sendStereo && runningRec_->returnStereo == r.returnStereo && runningRec_->out[0] == r.out[0] && runningRec_->out[1] == r.out[1]
                                                 && runningRec_->in[0] == r.in[0] && runningRec_->in[1] == r.in[1]);
    const bool modeChanged = wasRunning && runningRec_->mode != r.mode;
    if (! library_->upsertDevice (r)) { realignClaim(); return library_->warnings().empty() ? std::string ("Save failed") : library_->warnings().back(); }
    const int idx = libraryIndexOf (r.id);
    ni::host::DiagLog::get().text ("LIB", instNo_, "save|id=%s|new=%d|idx=%d|out=%d/%d|in=%d/%d|sendStereo=%d|retStereo=%d|running=%d|routing=%d", r.id.c_str(), isNew ? 1 : 0, idx,
                                   r.out[0], r.out[1], r.in[0], r.in[1], r.sendStereo ? 1 : 0, r.returnStereo ? 1 : 0, wasRunning ? 1 : 0, routingChanged ? 1 : 0);
    if (wasRunning && (routingChanged || modeChanged)) { selectDeviceFromGui (0); selectDeviceFromGui (idx); }   // release, re-claim, measure
    else if (wasRunning) { adoptLibraryRecord (r.id); vuRef_ = r.vuRefDbfs; slotDirty_ = true; }               // name/levels only: no re-measure
    else selectDeviceFromGui (idx);                                                                           // claims and measures now
    return {};
}

std::string NovaInsertProcessor::deleteDevice (const std::string& id)
{
    if (! library_) return "No device library";
    pollLibrary();
    const auto rec = library_->device (id);
    if (! rec) return "Preset not found";
    if (coord_)
        if (const auto owner = coord_->pairs().ownerTrackOf (ni::plugin::recordPair (*rec), instNo_))
            return "In use by " + (owner->empty() ? std::string ("another track") : *owner);
    const int idx = libraryIndexOf (id);
    if (claimedDevice_ == idx) selectDeviceFromGui (0);
    if (! library_->removeDevice (id)) return library_->warnings().empty() ? std::string ("Delete failed") : library_->warnings().back();
    ni::host::DiagLog::get().text ("LIB", instNo_, "delete|id=%s|idx=%d", id.c_str(), idx);
    realignClaim();
    return {};
}

std::vector<std::string> NovaInsertProcessor::deviceNames() const
{
    std::vector<std::string> out;
    if (library_) for (const auto& d : library_->devices()) out.push_back (d.name);
    return out;
}

void NovaInsertProcessor::selectDeviceFromGui (int device)
{
    pDevice->beginChangeGesture();
    pDevice->setValueNotifyingHost (pDevice->convertTo0to1 (static_cast<float> (juce::jmax (0, device))));
    pDevice->endChangeGesture();
}

void NovaInsertProcessor::reapplyAnticipativeFromGui() { pReapplyAnt->setValueNotifyingHost (1.0f); }

void NovaInsertProcessor::recalibrateFromGui()
{
    const auto s = state();
    const bool offered = ni::engine::recalibrationOffered (s, openFallbackOn_);
    ni::host::DiagLog::get().text ("USER", instNo_, "recalibrate|state=%d|fallback=%d|offered=%d", static_cast<int> (s), openFallbackOn_ ? 1 : 0, offered ? 1 : 0);
    if (offered) requestMeasure (Reason::UserRequest);
}

void NovaInsertProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    ni::plugin::InstanceState st;
    for (int i = 0; i < P::kParamCount; ++i)
        if (P::kParamAutomatable[static_cast<std::size_t> (i)])
            if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (getParameters()[i]))
                st.params[q->getParameterID().toStdString()] = q->convertFrom0to1 (q->getValue());
    st.cal = ni::plugin::InstanceCal { pLink->get(), { sendOff_[0], sendOff_[1] }, { retOff_[0], retOff_[1] } };
    st.deviceSnapshot = activeRecord();   // a kept project snapshot is saved again, never the edited library record
    if (st.deviceSnapshot && lastCalib_ && lastCalib_->deviceId == st.deviceSnapshot->id) st.calibSnapshot = lastCalib_;   // Task 22
    else if (st.deviceSnapshot) st.calibSnapshot = restore_.calibForSave (st.deviceSnapshot->id);   // saved before the restore slot ran (review I5)
    dest = ni::plugin::serialize (st);
}

void NovaInsertProcessor::setStateInformation (const void* data, int size)
{
    std::string why;
    const auto st = ni::plugin::deserialize (data, size > 0 ? static_cast<std::size_t> (size) : 0, &why);
    if (! st)   // unreadable or newer schema: keep defaults, say why
    {
        ni::host::DiagLog::get().text ("STATE", instNo_, "rejected|%s", why.c_str());
        return;
    }
    // before the parameters: setting the device parameter below claims the pair, and that claim's slot consults the snapshot
    // the token belongs to the device parameter value this state carries: only that claim is the restore (review C1/I1)
    const auto dp = st->params.find ("device");
    restore_.loaded (*st, dp != st->params.end() ? static_cast<int> (std::lround (dp->second)) : pDevice->get());
    for (int i = 0; i < P::kParamCount; ++i)
        if (P::kParamAutomatable[static_cast<std::size_t> (i)])
            if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (getParameters()[i]))
                if (const auto it = st->params.find (q->getParameterID().toStdString()); it != st->params.end())
                    q->setValueNotifyingHost (q->convertTo0to1 (static_cast<float> (it->second)));
    defaultsPending_ = false;   // a state load keeps its own trims; never the device defaults
    defaultsChanged_ = false;
    if (measureAfterDefaults_) { const auto r = *measureAfterDefaults_; measureAfterDefaults_.reset(); requestMeasure (r); }   // a non-restore claim from this load
    {
        const auto rec = st->deviceSnapshot ? st->deviceSnapshot : activeRecord();
        for (int ch = 0; ch < 2; ++ch)
        {
            sendOff_[ch] = P::sanitizeOffsetDb (st->cal ? st->cal->sendOffsetDb[ch] : rec ? rec->sendOffsetDb[ch] : 0.0f);
            retOff_[ch] = P::sanitizeOffsetDb (st->cal ? st->cal->returnOffsetDb[ch] : rec ? rec->returnOffsetDb[ch] : 0.0f);
        }
    }
    linkResync();   // restored trims are not a LINK move
    if (library_ && st->deviceSnapshot)
    {
        const auto diff = ni::plugin::compare (*st, *library_);
        defaultsChanged_ = diff == ni::plugin::SnapshotDiff::DefaultsChanged;   // a neutral note, not a stale snapshot
        // shown, never applied (F1 §16); NotInLibrary too (review M2)
        restore_.setStale (diff != ni::plugin::SnapshotDiff::Same && diff != ni::plugin::SnapshotDiff::DefaultsChanged);
        if (diff != ni::plugin::SnapshotDiff::Same)
            ni::host::DiagLog::get().text ("STATE", instNo_, "snapshot_vs_library=%d|device=%s", static_cast<int> (diff),
                                           st->deviceSnapshot->id.c_str());
    }
}

// ---- Level CAL, LINK and device defaults (VU GUI plan Task 7) -----------------------------------------------------------------

void NovaInsertProcessor::setPlain (juce::RangedAudioParameter* p, float v)
{
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (v));
    p->endChangeGesture();
}

ni::plugin::CalContext NovaInsertProcessor::calContext() const
{
    ni::plugin::CalContext c;
    c.effectMode = pMode->getIndex() == 1;
    c.recording = apiOk_ && coord_ && recordingNow();
    c.pairOwned = pairOwned_ && inst_ && inst_->active() && loopCfg_.B > 0;
    const auto s = state();
    c.measuring = startPending_ || (measurer_ && measurer_->busy()) || s == State::Queued || s == State::Measuring || s == State::ApplyingLatency;
    { const auto eff = effectivePair(); c.sendStereo = eff.sendStereo; c.retStereo = eff.retStereo; }
    const auto rec = activeRecord();
    c.vuRefDbfs = vuRef_;
    c.sendDbuAt0dBFS = rec ? rec->sendDbuAt0dBFS : 24.0f;
    c.sendTrimDb = pSendTrim->get();
    c.returnTrimDb = pReturnTrim->get();
    for (int ch = 0; ch < 2; ++ch) { c.sendOffsetDb[ch] = sendOff_[ch]; c.returnOffsetDb[ch] = retOff_[ch]; }
    const auto m = sendMeter_.read (vuRef_);   // SEND CAL reads the track's program as it leaves the converter
    c.sendLevelDbfs[0] = m.rmsDb[0]; c.sendLevelDbfs[1] = m.rmsDb[1];
    return c;
}

void NovaInsertProcessor::pressCal (int action)
{
    if (! cal_) return;
    const auto c = calContext();
    const auto s = action == 1 ? cal_->pressSend (c) : action == 2 ? cal_->pressReturn (c) : (cal_->cancel(), cal_->status());
    ni::host::DiagLog::get().text ("CAL", instNo_, "action=%d|status=%d|send=%.2f|ret=%.2f|loop=%d", action, static_cast<int> (s),
                                   static_cast<double> (c.sendTrimDb), static_cast<double> (c.returnTrimDb), inst_ ? inst_->physLoop() : -1);
    applyCalResult();
    setReadOnly (Param::CalStatus, static_cast<int> (cal_->status()));
}

void NovaInsertProcessor::serviceCal()
{
    if (! cal_) return;
    if (const int a = pCalAction->get(); a != 0) { *pCalAction = 0; pressCal (a); }   // momentary (ReaScript / host)
    cal_->tick (calContext(), juce::Time::getMillisecondCounterHiRes() / 1000.0);
    applyCalResult();
    const int s = static_cast<int> (cal_->status());
    if (s != lastCalStatus_)
    {
        lastCalStatus_ = s;
        ni::host::DiagLog::get().text ("CAL", instNo_, "status=%d|send=%.2f|ret=%.2f|retOff=%.2f/%.2f", s, static_cast<double> (pSendTrim->get()),
                                       static_cast<double> (pReturnTrim->get()), static_cast<double> (retOff_[0]), static_cast<double> (retOff_[1]));
    }
    setReadOnly (Param::CalStatus, s);
}

void NovaInsertProcessor::applyCalResult()
{
    const auto r = cal_ ? cal_->takeResult() : std::nullopt;
    if (! r) return;
    if (r->sendTrimDb)
    {
        setPlain (pSendTrim, *r->sendTrimDb);
        serviceLink();   // LINK on: the return moves by the opposite delta now (user 2026-11-06: SEND CAL moves the linked return)
        ni::host::DiagLog::get().text ("CAL", instNo_, "send_program|send=%.2f|ret=%.2f|link=%d", static_cast<double> (pSendTrim->get()),
                                       static_cast<double> (pReturnTrim->get()), pLink->get() ? 1 : 0);
    }
    if (r->returnTrimDb) setPlain (pReturnTrim, *r->returnTrimDb);
    if (r->returnOffsetDb) { retOff_[0] = P::sanitizeOffsetDb ((*r->returnOffsetDb)[0]); retOff_[1] = P::sanitizeOffsetDb ((*r->returnOffsetDb)[1]); }
    if (r->returnTrimDb) linkResync();   // RETURN CAL re-establishes unity itself: never a LINK move
    // SEND CAL sets this track's program level: a per-track trim, never the device default (ruling 2026-11-06)
    if (! r->returnTrimDb || cal_->status() != ni::plugin::CalStatus::Done || ! library_ || ! runningRec_) return;
    // the CAL result becomes the device default (spec §7): trims, LINK and offsets as this instance now runs them
    if (auto lib = library_->device (runningRec_->id))
    {
        lib->sendTrimDb = pSendTrim->get(); lib->returnTrimDb = pReturnTrim->get(); lib->link = pLink->get();
        for (int ch = 0; ch < 2; ++ch) { lib->sendOffsetDb[ch] = sendOff_[ch]; lib->returnOffsetDb[ch] = retOff_[ch]; }
        const bool ok = library_->upsertDevice (*lib);
        realignClaim();   // the write re-read the file: an index shift from another instance's delete is followed at once (final review H1)
        if (ok) adoptLibraryRecord (lib->id);
        ni::host::DiagLog::get().text ("CAL", instNo_, "defaults|id=%s|ok=%d|send=%.2f|ret=%.2f|retOff=%.2f/%.2f", lib->id.c_str(), ok ? 1 : 0,
                                       static_cast<double> (lib->sendTrimDb), static_cast<double> (lib->returnTrimDb),
                                       static_cast<double> (lib->returnOffsetDb[0]), static_cast<double> (lib->returnOffsetDb[1]));
    }
}

void NovaInsertProcessor::linkResync() { linkPrevSend_ = pSendTrim->get(); linkPrevRet_ = pReturnTrim->get(); }

void NovaInsertProcessor::serviceLink()
{
    const auto st = calStatus();
    if (st == ni::plugin::CalStatus::ReturnSettling || st == ni::plugin::CalStatus::ReturnMeasuring) { linkResync(); return; }   // never in a CAL window
    const float s = pSendTrim->get(), r = pReturnTrim->get();
    const auto o = ni::engine::cal::linkFollow (linkPrevSend_, linkPrevRet_, s, r, pLink->get(), P::kTrimMinDb, P::kTrimMaxDb);
    if (ni::engine::cal::moved (o.sendDb, s)) setPlain (pSendTrim, o.sendDb);
    if (ni::engine::cal::moved (o.returnDb, r)) setPlain (pReturnTrim, o.returnDb);
    linkResync();
}

void NovaInsertProcessor::applyRecordDefaults (const ni::plugin::DeviceRecord& rec)
{
    const int modeIdx = rec.mode == ni::engine::Mode::Effect ? 1 : 0;   // the preset's mode is one of its defaults (DEVICES page)
    if (pMode->getIndex() != modeIdx) setPlain (pMode, static_cast<float> (modeIdx));
    setPlain (pSendTrim, rec.sendTrimDb);
    setPlain (pReturnTrim, rec.returnTrimDb);
    setPlain (pLink, rec.link ? 1.0f : 0.0f);
    for (int ch = 0; ch < 2; ++ch) { sendOff_[ch] = P::sanitizeOffsetDb (rec.sendOffsetDb[ch]); retOff_[ch] = P::sanitizeOffsetDb (rec.returnOffsetDb[ch]); }
    linkResync();
    defaultsChanged_ = false;
    ni::host::DiagLog::get().text ("CAL", instNo_, "apply_defaults|id=%s|send=%.2f|ret=%.2f|link=%d", rec.id.c_str(),
                                   static_cast<double> (rec.sendTrimDb), static_cast<double> (rec.returnTrimDb), rec.link ? 1 : 0);
}

void NovaInsertProcessor::applyDeviceDefaults()
{
    const auto rec = activeRecord();
    if (! rec || ! library_) return;
    if (const auto lib = library_->device (rec->id)) { applyRecordDefaults (*lib); adoptLibraryRecord (lib->id); }
}

void NovaInsertProcessor::adoptLibraryRecord (const std::string& id)
{
    // a library write by this instance also updates the kept project snapshot, so a restored project runs the edit and saves
    // a snapshot equal to the library (final review C1/I1); the running record follows for the same id
    const auto lib = library_ ? library_->device (id) : std::nullopt;
    if (! lib) return;
    restore_.adopt (*lib);
    if (runningRec_ && runningRec_->id == id)
        if (const auto rec = activeRecord(); rec && rec->id == id) runningRec_ = rec;
}

void NovaInsertProcessor::setConverterCal (float sendDbu, float returnDbu, float vuRef, std::optional<float> outboardMax)
{
    const auto rec = activeRecord();
    if (! rec || ! library_) return;
    auto lib = library_->device (rec->id);
    if (! lib) return;
    lib->sendDbuAt0dBFS = sendDbu; lib->returnDbuAt0dBFS = returnDbu; lib->vuRefDbfs = vuRef; lib->outboardMaxInDbu = outboardMax;
    const bool ok = library_->upsertDevice (*lib);
    realignClaim();
    if (ok) { vuRef_ = vuRef; adoptLibraryRecord (lib->id); }
    ni::host::DiagLog::get().text ("CAL", instNo_, "converter|id=%s|ok=%d|send=%.1f|ret=%.1f|vu=%.1f", lib->id.c_str(), ok ? 1 : 0,
                                   static_cast<double> (sendDbu), static_cast<double> (returnDbu), static_cast<double> (vuRef));
}

void NovaInsertProcessor::setOffsets (std::array<float, 2> send, std::array<float, 2> ret)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        sendOff_[ch] = P::sanitizeOffsetDb (send[static_cast<size_t> (ch)]);
        retOff_[ch] = P::sanitizeOffsetDb (ret[static_cast<size_t> (ch)]);
    }
}

std::vector<ni::gui::DeviceListEntry> NovaInsertProcessor::deviceList() const
{
    std::vector<ni::gui::DeviceListEntry> out;
    if (! library_) return out;
    for (const auto& d : library_->devices())
    {
        ni::gui::DeviceListEntry e { d.name, {} };
        if (coord_)
            if (const auto owner = coord_->pairs().ownerTrackOf (ni::plugin::recordPair (d), instNo_))
                e.inUseBy = owner->empty() ? std::string ("another track") : *owner;
        out.push_back (std::move (e));
    }
    return out;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NovaInsertProcessor(); }
