// NovaInsertProcessor — the VST3 instance (F1 §1 Instance): host bridge (REAPER API via IReaperHostApplication), parameter
// model, state machine, device library + routing, Measurer / CAL / LINK services, PDC reporting, render preflight view,
// persistence (state blob), diagnostics, non-realtime/offline behaviour. Message-thread timer order: see timerCallback().
#pragma once
#include <memory>
#include <string>
#include "host/coordinator.hpp"
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include "engine/types.hpp"
#include "host/reaper_api.hpp"
#include "plugin/parameters.hpp"
#include "plugin/library.hpp"
#include "plugin/persistence.hpp"
#include "plugin/instance.hpp"
#include "plugin/measurer.hpp"
#include "plugin/pdc_reporter.hpp"
#include "plugin/render_window.hpp"
#include "host/render_arbiter.hpp"
#include "plugin/correlator_worker.hpp"
#include "engine/meters.hpp"
#include "gui/view_model.hpp"
#include "plugin/cal_controller.hpp"
#include <array>

class IReaperHostApplication;

class NovaInsertProcessor final : public juce::AudioProcessor,
                                  public juce::VST3ClientExtensions,
                                  private juce::Timer,
                                  private juce::AudioProcessorParameter::Listener
{
public:
    NovaInsertProcessor();
    ~NovaInsertProcessor() override;

    // juce::VST3ClientExtensions
    void setIHostApplication (Steinberg::FUnknown* host) override;
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "NovaInsert"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    // Minimal parameter persistence (schema 1 JSON; Task 22 adds the device/calibration snapshots to the same blob).
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    ni::engine::State state() const noexcept { return state_.load (std::memory_order_acquire); }

    // Editor surface (message thread; plan Task 25). The editor only reads these and changes parameters.
    ni::gui::ViewInputs viewInputs() const;
    std::vector<std::string> deviceNames() const;
    int selectedDevice() const { return pDevice->get(); }
    void selectDeviceFromGui (int device);
    void reapplyAnticipativeFromGui();
    void recalibrateFromGui();   // RE-CALIBRATION (message thread): re-queue the latency check after a failed calibration
    ni::engine::MeterEngine& sendMeter() noexcept { return sendMeter_; }
    ni::engine::MeterEngine& returnMeter() noexcept { return returnMeter_; }
    float vuReference() const noexcept { return vuRef_; }
    // Level CAL / LINK / device defaults (VU GUI spec 2026-11-02 §2/§7; message thread)
    std::vector<ni::gui::DeviceListEntry> deviceList() const;   // library order; channels owned by another instance are "in use"
    void pressCal (int action);                                 // 1 = SEND CAL toggle, 2 = RETURN CAL, 3 = cancel
    void applyDeviceDefaults();                                 // the library record's trims, LINK and offsets → this instance
    void setConverterCal (float sendDbuAt0dBFS, float returnDbuAt0dBFS, float vuRefDbfs, std::optional<float> outboardMaxInDbu);
    void setOffsets (std::array<float, 2> send, std::array<float, 2> ret);
    ni::plugin::CalStatus calStatus() const { return cal_ ? cal_->status() : ni::plugin::CalStatus::Idle; }
    // DEVICES page (spec 2026-11-05; message thread): presets edited in the editor. Nothing about latency is stored with a
    // preset — saving or selecting one claims its channels and measures at once.
    std::string saveDevice (ni::plugin::DeviceRecord r);      // validate → write → select (re-claim when the running one changed); "" = ok
    std::string deleteDevice (const std::string& id);         // refused while another instance uses it; "" = ok
    void pollLibrary();                                       // ≤ 1 Hz from the editor: devices.json re-read, host channel names
    void setMeasureOnOpen (bool on);                          // SETUP: global (settings.json), read by every instance on project open
    void setSkin (int skin);                                  // SETUP: global GUI skin (settings.json), every open editor follows

private:
    void timerCallback() override;
    // Device parameter changes are applied synchronously when they arrive on the message thread (ReaScript, GUI,
    // state restore) so first-come pair ownership follows the order the user made the changes in; changes from
    // other threads (automation) are deferred to the next timer tick.
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int, bool) override {}
    bool resolveTrack();               // own MediaTrack via getReaperParent; true once known
    void syncSlot();                   // message thread: own FX slot shows the device name (host/slot_name.hpp)
    void setState (ni::engine::State s, ni::engine::Reason r);
    void evaluateHostState (ni::engine::Reason trigger = ni::engine::Reason::None);   // message thread: device key / sample rate / hook -> state; trigger labels the re-request
    void setReadOnly (ni::plugin::Param p, int value);
    void pollHost();                   // message thread, 500 ms: own track, device key transitions, device parameter, warnings, queue position
    void applyDeviceParameter();       // device parameter -> library record -> pair claim
    void requestMeasure (ni::engine::Reason r);
    void onCoordinatorEvent (const ni::host::Coordinator::Event& e);
    void releaseOwnership();
    void syncInstance();              // message thread (timer): pair owned + host usable -> Instance configured (Task 11 fixed latency)
    void releaseLoop (const char* why); // message thread: stop hook I/O now, latency 0, clear the read-only loop parameters
    void logLoop (bool force);
    void logMeters();   // diag: what the SEND/RETURN meters read (AES17 dBFS), 1 s while a signal is present
    void serviceMeasurer();           // message thread (timer): deferred start, record cut/requeue, Measurer tick
    void onMeasured (uint32_t gen, const ni::engine::CalibResult& r);   // Measurer result -> latency, state, Scheduler
    // a valid physLoop (measured, or a matching project snapshot) -> LOOPCFG row, ApplyingLatency(why) through the reporter
    void acceptResult (uint32_t gen, const ni::engine::CalibResult& r, ni::engine::Reason why, const char* origin);
    std::optional<ni::plugin::DeviceRecord> activeRecord() const;   // the claimed device: project snapshot over library (Task 23)
    std::optional<ni::engine::CalibKey> currentKey() const;   // claimed record on the current host device; nullopt until known
    void cancelMeasure (ni::engine::Reason why);   // running or pending measurement abandoned (no result)
    bool recordingNow() const;        // REAPER transport is recording (GetPlayStateEx bit 4)        // LOOP diagnostics row: on change of the engine status (and every second while active)

    // parameters (order = ni::plugin::kParamIds)
    juce::AudioParameterInt*    pDevice = nullptr;
    juce::AudioParameterChoice* pMode = nullptr;
    juce::AudioParameterChoice* pSendSel = nullptr;
    juce::AudioParameterChoice* pReturnSel = nullptr;
    juce::AudioParameterBool*   pStereo = nullptr;
    juce::AudioParameterFloat*  pSendTrim = nullptr;
    juce::AudioParameterFloat*  pReturnTrim = nullptr;
    juce::AudioParameterBool*   pPreRoll = nullptr;
    juce::AudioParameterBool*   pReapplyAnt = nullptr;
    juce::AudioParameterInt*    pState = nullptr;
    juce::AudioParameterInt*    pPhysLoop = nullptr;
    juce::AudioParameterInt*    pReportedPdc = nullptr;
    juce::AudioParameterInt*    pInherentGap = nullptr;
    juce::AudioParameterBool*   pLink = nullptr;
    juce::AudioParameterInt*    pCalAction = nullptr;   // momentary, not automatable
    juce::AudioParameterInt*    pCalStatus = nullptr;   // read-only

    std::atomic<ni::engine::State> state_ { ni::engine::State::UnsupportedHost };
    ni::engine::Reason lastReason_ = ni::engine::Reason::None;
    std::shared_ptr<ni::host::Coordinator> coord_;   // one shared_ptr per instance; reset in the destructor
    bool apiOk_ = false;
    std::atomic<double> preparedSampleRate_ { 0.0 };
    std::atomic<bool> hostDirty_ { false };   // prepareToPlay (any thread) -> evaluateHostState on the timer
    int instNo_ = -1;
    std::string lastKeyLine_;          // last DEVKEY diagnostics line (logged on change only)
    IReaperHostApplication* reaperHost_ = nullptr;   // retained for getReaperParent(); released in the destructor
    MediaTrack* track_ = nullptr;
    int lastSlotIdx_ = -2;             // getReaperParent(6) last seen (diagnostics on change)
    bool slotDirty_ = false;           // device changed: sync the slot name on the next timer tick (outside REAPER's set call)
    ReaProject* proj_ = nullptr;
    std::unique_ptr<ni::plugin::DeviceLibrary> library_;
    int claimedDevice_ = -1;           // device parameter value last applied (-1 = never)
    bool pairOwned_ = false;
    bool conflicted_ = false;
    bool antViolation_ = false;        // user cleared the no-anticipative-FX bit on a required track (F1 §5)
    bool everOpen_ = false;
    bool lastOpen_ = false;
    int lastBufferSize_ = 0;
    std::string lastIdent_;
    std::string identIn_, identOut_;   // current host device identity (CalibKey, Task 22/23)
    std::vector<std::string> outNames_, inNames_;   // host channel names for the DEVICES page (pollLibrary)
    int libraryIndexOf (const std::string& id) const;   // 1-based library index, 0 = not in the library
    void realignClaim();                                // after a library reload/delete: the claimed index follows the running record's id
    void syncLibrary();                                 // devices.json re-read when another instance wrote it, then realignClaim()
    std::optional<ni::plugin::CalibRecord> lastCalib_;   // last accepted calibration (measured or restored) → the blob's snapshot
    // measure on project open (user 2026-11-06): the project's matching snapshot, kept while the open measurement runs; a final
    // failure falls back to it (openFallbackOn_ shows the warning while that latency is the running one)
    std::optional<ni::plugin::CalibRecord> openKept_;
    bool openFallbackOn_ = false;
    ni::engine::Reason openFallbackReason_ = ni::engine::Reason::None;
    bool useOpenFallback (uint32_t gen, ni::engine::Reason why);   // true: the kept snapshot was accepted instead of failing
    ni::plugin::ProjectRestore restore_;   // project snapshot + restore token (Tasks 22/23, S7/S8 review C1/I1/I2/I5)
    std::optional<ni::plugin::DeviceRecord> runningRec_;   // the record the owned pair was claimed with
    const char* pendingOrigin_ = "MEASURED";             // physLoop origin the reporter's host call applies
    ni::host::Pair pair_ {};           // the record's whole pair (claimed as a unit); effectivePair() is what the loop runs on
    ni::host::Pair effectivePair() const noexcept;
    ni::engine::SideSel sendSide() const noexcept; ni::engine::SideSel returnSide() const noexcept;
    int pairLoopExtra_ = 0;   // device record s3LoopExtra: provisional loop until the Measurer locks (diagnostics PROVISIONAL)
    uint32_t generation_ = 0;
    int lastQueuePos_ = -2;
    std::string lastWarn_;
    std::string lastHookStat_;
    int ticks_ = 0;
    int lastModeIdx_ = -1;             // mode parameter edge (Insert/Effect): re-evaluates the latency source
    std::atomic<bool> deviceDirty_ { false };
    std::unique_ptr<ni::plugin::Instance> inst_;      // created with the coordinator (message thread), destroyed before it
    std::atomic<ni::plugin::Instance*> instRt_ { nullptr };   // processBlock's view of inst_
    struct LoopCfg { ni::host::Pair pair {}; int B = 0; bool operator== (const LoopCfg& o) const noexcept
        { return B == o.B && pair.sendStereo == o.pair.sendStereo && pair.retStereo == o.pair.retStereo && pair.out[0] == o.pair.out[0] && pair.out[1] == o.pair.out[1] && pair.in[0] == o.pair.in[0] && pair.in[1] == o.pair.in[1]; } };
    LoopCfg loopCfg_ {};              // configuration last applied to inst_ (B == 0: released)
    int loopRouting_ = -1;            // packed routing last applied
    std::string lastLoopKey_;
    int loopTicks_ = 0;
    int meterTicks_ = 0;
    // Measurement (Task 16): process-wide correlator thread, one Measurer per instance (single attempt; the Scheduler retries).
    std::shared_ptr<ni::plugin::CorrelatorWorker> worker_;
    std::unique_ptr<ni::plugin::SlotLoopProbeIo> probeIo_;
    std::unique_ptr<ni::plugin::CorrelatorWorker::Client> corrClient_;   // destroyed before measurer_ (waits for its job)
    std::unique_ptr<ni::plugin::Measurer> measurer_;
    uint32_t measureGen_ = 0;
    bool startPending_ = false;        // MeasureStart received, waiting for the loop to be active and recording to stop
    bool requeueAfterRecord_ = false;  // a measurement was cut by recording: request again once recording stops
    const char* physOrigin_ = "PROVISIONAL";
    // PDC transition (Task 19, F1 §9): a measured R is reported through the PdcReporter (deferred to stop while playing);
    // Locked only after the HostTransitionObserver accepted the new latency. Destroyed before inst_ (holds its observer).
    std::unique_ptr<ni::plugin::PdcReporter> pdc_;
    int32_t pendingPhys_ = -1;         // measured physLoop waiting for the reporter to apply it
    uint32_t pdcGen_ = 0;              // generation of the request the reporter is driving
    bool lkgValid_ = false;            // a previous Locked for lkgCfg_ (Stale vs Failed on a PDC timeout)
    LoopCfg lkgCfg_ {};
    bool lastPlaying_ = false;         // transport edge log (PDC rows are ordered against it in f3_5)
    bool pdcMuted_ = false;            // mirror of PdcReporter::muted() applied to the engine (F1 §9 Q10)
    void syncPdcMute();                // message thread: follow the reporter's mute; re-assert while it holds
    bool playingNow() const;
    ni::host::RenderArbiter::InstanceView renderView() const;   // message thread: this instance for the render preflight           // message thread: transport playing (not paused, not stopped)
    bool applyPdcState (ni::engine::State s, ni::engine::Reason r, uint32_t gen, const char* diag);
    std::atomic<bool> measuringRt_ { false };   // processBlock: a probe may be armed (cut it on a record start)
    std::atomic<bool> recordCut_ { false };     // processBlock -> timer: the probe was cut by recording
    ni::engine::MeterEngine sendMeter_, returnMeter_;   // fed in processBlock (RT), read by the editor
    float vuRef_ = -20.0f;                              // device record vuRefDbfs (0 VU)
    int64_t lastMeasuredUnix_ = 0;
    bool inParamCallback_ = false;
    // Level CAL (Task 7): the controller drives the looping, send-trimmed probe through EngineCalPort
    struct EngineCalPort;
    std::unique_ptr<EngineCalPort> calPort_;
    std::unique_ptr<ni::plugin::CalController> cal_;
    std::atomic<bool> calRt_ { false };           // processBlock: the CAL tone may be armed (cut it on a record start)
    float sendOff_[2] { 0.0f, 0.0f }, retOff_[2] { 0.0f, 0.0f };   // per-channel trim offsets of this instance
    float linkPrevSend_ = 0.0f, linkPrevRet_ = 0.0f;               // LINK: trims last seen (P7)
    bool defaultsPending_ = false;                // a user device selection: apply the record's defaults on the next tick
    std::optional<ni::engine::Reason> measureAfterDefaults_;   // that selection's first measurement, requested on the same tick (after the preset's mode)
    bool defaultsChanged_ = false;                // restored snapshot differs from the library only in the CAL defaults
    int lastCalStatus_ = 0;
    ni::plugin::CalContext calContext() const;
    void serviceCal();
    void serviceLink();
    void linkResync();
    void applyCalResult();
    void applyRecordDefaults (const ni::plugin::DeviceRecord& rec);
    void adoptLibraryRecord (const std::string& id);   // final review C1/I1
    static void setPlain (juce::RangedAudioParameter* p, float v);     // REAPER drops a read-only parameter edit made from inside its own set call; the timer re-pushes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NovaInsertProcessor)
};
