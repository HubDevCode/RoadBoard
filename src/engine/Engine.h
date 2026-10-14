// PipeWire audio engine.
//
// Creates one DSP filter node ("roadboard_engine") holding every input/output
// port of the console plus the RoadBoard virtual devices (null-audio-sink
// nodes): virtual inputs (playback devices for applications), virtual outputs
// (recording devices), the optional RAIO extension (one virtual input per
// hardware strip, one virtual output per A bus) and external FX send/return.
// It keeps PipeWire links between those nodes and the hardware devices chosen
// in the GUI, and lets the GUI route / control application streams.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/Params.h"

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_filter;
struct pw_proxy;
struct spa_source;
struct spa_io_position;

namespace road {

class Mixer;

struct AudioDevice {
    uint32_t id = 0;
    std::string name;          // node.name (persistent identifier)
    std::string description;   // human readable
    std::string mediaClass;
    std::string api;           // alsa, bluez5, jack, ...
    int channels = 0;
    bool monitor = false;      // "monitor of" pseudo-source
    bool virtualDevice = false;
    float volume = 1;          // linear (cubic UI scale applied by the GUI)
    bool mute = false;
    bool hasVolume = false;
};

// an application playing (output) or recording (input) audio
struct AppStream {
    uint32_t id = 0;
    uint64_t serial = 0;
    std::string appName, mediaName, binary, icon;
    bool output = true;
    std::string target;        // node.name of the device it is connected to (may be empty)
    std::string targetDescription;
    int channels = 2;
    float volume = 1;
    bool mute = false;
    bool hasVolume = false;
};

class Engine {
public:
    explicit Engine(Mixer& mixer);
    ~Engine();

    bool start(std::string* error = nullptr);
    void stop();
    void restart();
    bool running() const { return running_.load(); }

    // hardware (and other non RoadBoard) devices
    std::vector<AudioDevice> inputDevices() const;
    std::vector<AudioDevice> outputDevices() const;
    // every playback / capture device including the RoadBoard virtual devices
    std::vector<AudioDevice> allSinks() const;
    std::vector<AudioDevice> allSources() const;
    std::string describe(const std::string& nodeName) const;   // description or empty
    bool deviceConnected(const std::string& nodeName) const;

    // application streams
    std::vector<AppStream> streams() const;
    void moveStream(uint32_t streamId, const std::string& targetNodeName);  // "" = system default
    void setNodeVolume(uint32_t nodeId, float linear);
    void setNodeMute(uint32_t nodeId, bool mute);
    // remembered routing: application name -> device node name (applied to new streams)
    void setAppRule(const std::string& app, bool output, const std::string& target);
    std::map<std::string, std::string> appRules() const;
    void setAppRules(const std::map<std::string, std::string>& rules);
    static std::string ruleKey(const std::string& app, bool output) { return (output ? "out:" : "in:") + app; }

    // make one of our virtual devices the system default
    void setSystemDefault(int vin);
    void setSystemDefaultSource(int vout);
    void setDefaultNode(const std::string& nodeName, bool sink);

    float sampleRate() const { return rate_.load(); }
    int quantum() const { return quantum_.load(); }
    std::string statusText() const;

    // notified from the PipeWire thread when devices or streams change
    std::function<void()> onDevicesChanged;
    std::function<void()> onStreamsChanged;

    // RoadBoard device names
    static const char* virtualInputName(int v);          // strips 6..8
    static const char* virtualOutputName(int v);         // B1..B3
    static std::string virtualInputDescription(int v);
    static std::string virtualOutputDescription(int v);
    static std::string raioInputName(int strip);         // hw strip RAIO
    static std::string raioOutputName(int bus);          // A bus RAIO
    static std::string raioInputDescription(int strip);
    static std::string raioOutputDescription(int bus);
    static std::string fxSendName(int fx);
    static std::string fxReturnName(int fx);
    static bool isOwnNode(const std::string& name) { return name.rfind("roadboard_", 0) == 0; }

    struct PortInfo { uint32_t id = 0, node = 0; bool out = false, monitor = false; std::string name, channel; };
    struct NodeInfo {
        uint32_t id = 0; uint64_t serial = 0;
        std::string name, description, mediaClass, api, appName, mediaName, binary, icon;
        void* ctl = nullptr;      // NodeCtl (bound proxy for volume / mute)
    };

private:
    friend struct EngineCallbacks;
    void createFilter();
    void createVirtualDevices();
    void destroyVirtualDevices();
    void scheduleReconcile();
    void reconcile();
    void applyRules();
    void onProcess(spa_io_position* position);
    void onTimer();
    uint32_t nodeIdByName(const std::string& name) const;
    std::vector<const PortInfo*> nodePorts(uint32_t node, bool out, int monitor /* -1 any */) const;
    void bindNode(NodeInfo& n);
    std::string streamTarget(uint32_t node, bool output) const;

    Mixer& mixer_;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_filter* filter_ = nullptr;
    spa_source* timer_ = nullptr;
    void* coreListener_ = nullptr;
    void* registryListener_ = nullptr;
    void* filterListener_ = nullptr;
    pw_proxy* metadata_ = nullptr;
    uint32_t metadataId_ = 0;

    std::vector<void*> inPorts_;   // kEngineIn
    std::vector<void*> outPorts_;  // kEngineOut
    std::vector<pw_proxy*> virtualNodes_;
    std::map<std::pair<uint32_t, uint32_t>, void*> links_;   // -> LinkCtx (links we own)

    mutable std::mutex infoMutex_;
    std::map<uint32_t, NodeInfo> nodes_;
    std::map<uint32_t, PortInfo> ports_;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> graphLinks_;   // every link: id -> (out node, in node)
    std::map<std::string, std::string> rules_;
    std::set<uint32_t> ruleApplied_;

    std::atomic<bool> running_{false};
    std::atomic<float> rate_{0};
    std::atomic<int> quantum_{0};
    bool reconcilePending_ = false;
    bool devicesDirty_ = false;
    bool streamsDirty_ = false;
    bool rulesPending_ = false;
    uint32_t lastRoutingRev_ = 0;
    int lastRaioExt_ = -1, lastExtFx_ = -1;
    std::string lastError_;
};

} // namespace road
