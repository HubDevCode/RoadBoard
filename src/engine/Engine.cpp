#include "engine/Engine.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "engine/Mixer.h"

namespace road {

namespace {

const char* const kEngineNode = "roadboard_engine";
const char* const kVinNames[kVirtStrips] = {"roadboard_input", "roadboard_aux_input", "roadboard_raio3_input"};
const char* const kVoutNames[kVirtBuses] = {"roadboard_out_b1", "roadboard_out_b2", "roadboard_out_b3"};
const char* const kPos8[kBusCh] = {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};

std::string inPortName(int idx) {
    if (idx >= kFxReturnBase) { const int k = idx - kFxReturnBase; return "fxret_" + std::to_string(k / 2 + 1) + (k % 2 ? "_R" : "_L"); }
    if (idx >= kRaioInBase) { const int k = idx - kRaioInBase; return "raio_" + std::to_string(k / 2 + 1) + (k % 2 ? "_R" : "_L"); }
    for (int s = 0; s < kStrips; ++s) {
        const int f = stripFirstInCh(s);
        if (idx >= f && idx < f + stripChannels(s)) {
            const int c = idx - f;
            if (isHwStrip(s)) return "in_" + std::to_string(s + 1) + (c == 0 ? "_L" : "_R");
            return "in_" + std::to_string(s + 1) + "_" + kPos8[c];
        }
    }
    return "in_" + std::to_string(idx);
}
std::string outPortName(int idx) {
    if (idx >= kFxSendBase) { const int k = idx - kFxSendBase; return "fxsend_" + std::to_string(k / 2 + 1) + (k % 2 ? "_R" : "_L"); }
    return std::string("out_") + kBusNames[idx / kBusCh] + "_" + kPos8[idx % kBusCh];
}

int posIndex(const std::string& ch) {
    for (int i = 0; i < kBusCh; ++i) if (ch == kPos8[i]) return i;
    if (ch == "BL") return 4;
    if (ch == "BR") return 5;
    if (ch.rfind("AUX", 0) == 0) { try { return std::stoi(ch.substr(3)); } catch (...) {} }
    return -1;
}

std::string positionList(int nch) {
    if (nch == 1) return "MONO";
    std::string s;
    for (int i = 0; i < std::min(nch, kBusCh); ++i) { if (i) s += ","; s += kPos8[i]; }
    return s;
}

const char* lookup(const spa_dict* d, const char* key) {
    const char* v = d ? spa_dict_lookup(d, key) : nullptr;
    return v ? v : "";
}

struct LinkCtx {
    Engine* engine;
    std::pair<uint32_t, uint32_t> key;
    pw_proxy* proxy;
    spa_hook hook;
};

struct NodeCtl {
    Engine* engine;
    uint32_t id;
    pw_proxy* proxy;
    spa_hook hook;
    std::vector<float> vols;
    bool mute = false, have = false;
};

bool isStreamClass(const std::string& c) { return c == "Stream/Output/Audio" || c == "Stream/Input/Audio"; }
bool isDeviceClass(const std::string& c) { return c.rfind("Audio/Sink", 0) == 0 || c.rfind("Audio/Source", 0) == 0 || c == "Audio/Duplex"; }

} // namespace

// Static trampolines with access to Engine internals
struct EngineCallbacks {
    static void onGlobal(void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
        auto* e = static_cast<Engine*>(data);
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            Engine::NodeInfo n;
            n.id = id;
            n.serial = std::strtoull(lookup(props, PW_KEY_OBJECT_SERIAL), nullptr, 10);
            n.name = lookup(props, PW_KEY_NODE_NAME);
            n.description = lookup(props, PW_KEY_NODE_DESCRIPTION);
            if (n.description.empty()) n.description = lookup(props, PW_KEY_NODE_NICK);
            if (n.description.empty()) n.description = n.name;
            n.mediaClass = lookup(props, PW_KEY_MEDIA_CLASS);
            n.api = lookup(props, PW_KEY_OBJECT_PATH);
            n.api = n.api.substr(0, n.api.find(':'));
            n.appName = lookup(props, PW_KEY_APP_NAME);
            n.mediaName = lookup(props, PW_KEY_MEDIA_NAME);
            n.binary = lookup(props, PW_KEY_APP_PROCESS_BINARY);
            n.icon = lookup(props, PW_KEY_APP_ICON_NAME);
            if (n.appName.empty()) n.appName = n.binary.empty() ? n.description : n.binary;
            const bool stream = isStreamClass(n.mediaClass);
            const bool device = isDeviceClass(n.mediaClass);
            {
                std::lock_guard<std::mutex> lk(e->infoMutex_);
                auto& slot = e->nodes_[id];
                slot = n;
                if ((stream || device) && n.appName != "RoadBoard") e->bindNode(slot);
            }
            if (device) e->devicesDirty_ = true;
            if (stream) { e->streamsDirty_ = true; e->rulesPending_ = true; }
            e->scheduleReconcile();
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            Engine::PortInfo p;
            p.id = id;
            const char* nid = lookup(props, PW_KEY_NODE_ID);
            p.node = *nid ? uint32_t(std::strtoul(nid, nullptr, 10)) : 0;
            p.out = std::strcmp(lookup(props, PW_KEY_PORT_DIRECTION), "out") == 0;
            p.monitor = std::strcmp(lookup(props, PW_KEY_PORT_MONITOR), "true") == 0;
            p.name = lookup(props, PW_KEY_PORT_NAME);
            p.channel = lookup(props, PW_KEY_AUDIO_CHANNEL);
            const char* fmt = lookup(props, PW_KEY_FORMAT_DSP);
            if (*fmt && std::strstr(fmt, "audio") == nullptr) return; // MIDI etc.
            {
                std::lock_guard<std::mutex> lk(e->infoMutex_);
                e->ports_[id] = p;
            }
            e->devicesDirty_ = true;
            e->scheduleReconcile();
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
            const uint32_t o = uint32_t(std::strtoul(lookup(props, PW_KEY_LINK_OUTPUT_NODE), nullptr, 10));
            const uint32_t i = uint32_t(std::strtoul(lookup(props, PW_KEY_LINK_INPUT_NODE), nullptr, 10));
            {
                std::lock_guard<std::mutex> lk(e->infoMutex_);
                e->graphLinks_[id] = {o, i};
            }
            e->streamsDirty_ = true;
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            if (std::strcmp(lookup(props, PW_KEY_METADATA_NAME), "default") == 0 && !e->metadata_) {
                e->metadata_ = static_cast<pw_proxy*>(pw_registry_bind(e->registry_, id, PW_TYPE_INTERFACE_Metadata,
                                                                       PW_VERSION_METADATA, 0));
                e->metadataId_ = id;
            }
        }
    }

    static void onGlobalRemove(void* data, uint32_t id) {
        auto* e = static_cast<Engine*>(data);
        bool devChanged = false, streamChanged = false;
        {
            std::lock_guard<std::mutex> lk(e->infoMutex_);
            auto it = e->nodes_.find(id);
            if (it != e->nodes_.end()) {
                if (auto* ctl = static_cast<NodeCtl*>(it->second.ctl)) {
                    spa_hook_remove(&ctl->hook);
                    pw_proxy_destroy(ctl->proxy);
                    delete ctl;
                }
                streamChanged = isStreamClass(it->second.mediaClass);
                devChanged = !streamChanged;
                e->nodes_.erase(it);
                e->ruleApplied_.erase(id);
            }
            devChanged |= e->ports_.erase(id) > 0;
            streamChanged |= e->graphLinks_.erase(id) > 0;
        }
        if (id == e->metadataId_ && e->metadata_) {
            pw_proxy_destroy(e->metadata_);
            e->metadata_ = nullptr;
            e->metadataId_ = 0;
        }
        if (devChanged) { e->devicesDirty_ = true; e->scheduleReconcile(); }
        if (streamChanged) e->streamsDirty_ = true;
    }

    static void onNodeParam(void* data, int, uint32_t id, uint32_t, uint32_t, const spa_pod* param) {
        auto* ctl = static_cast<NodeCtl*>(data);
        if (id != SPA_PARAM_Props || !param || !spa_pod_is_object(param)) return;
        const spa_pod_prop* prop;
        const spa_pod_object* obj = reinterpret_cast<const spa_pod_object*>(param);
        bool changed = false;
        SPA_POD_OBJECT_FOREACH(obj, prop) {
            if (prop->key == SPA_PROP_channelVolumes) {
                float v[64];
                const uint32_t nv = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, v, 64);
                if (nv > 0) {
                    std::lock_guard<std::mutex> lk(ctl->engine->infoMutex_);
                    ctl->vols.assign(v, v + nv);
                    ctl->have = true;
                    changed = true;
                }
            } else if (prop->key == SPA_PROP_mute) {
                bool m = false;
                if (spa_pod_get_bool(&prop->value, &m) == 0) {
                    std::lock_guard<std::mutex> lk(ctl->engine->infoMutex_);
                    ctl->mute = m;
                    changed = true;
                }
            }
        }
        if (changed) { ctl->engine->streamsDirty_ = true; ctl->engine->devicesDirty_ = true; }
    }

    static void onProcess(void* data, spa_io_position* position) {
        static_cast<Engine*>(data)->onProcess(position);
    }

    static void onFilterState(void* data, pw_filter_state, pw_filter_state state, const char* error) {
        auto* e = static_cast<Engine*>(data);
        if (state == PW_FILTER_STATE_ERROR) {
            e->lastError_ = error ? error : "filter error";
            std::fprintf(stderr, "roadboard: engine filter error: %s\n", e->lastError_.c_str());
        }
        if (state == PW_FILTER_STATE_STREAMING || state == PW_FILTER_STATE_PAUSED) e->scheduleReconcile();
    }

    static void onTimer(void* data, uint64_t) { static_cast<Engine*>(data)->onTimer(); }

    static void onLinkRemoved(void* data) { pw_proxy_destroy(static_cast<LinkCtx*>(data)->proxy); }
    static void onLinkDestroy(void* data) {
        auto* ctx = static_cast<LinkCtx*>(data);
        spa_hook_remove(&ctx->hook);
        auto& links = ctx->engine->links_;
        auto it = links.find(ctx->key);
        if (it != links.end() && it->second == ctx) links.erase(it);
        delete ctx;
    }
    static void onLinkError(void*, int, int res, const char* message) {
        std::fprintf(stderr, "roadboard: link error %d: %s\n", res, message ? message : "");
    }

    static void onCoreError(void* data, uint32_t id, int, int res, const char* message) {
        auto* e = static_cast<Engine*>(data);
        if (id == PW_ID_CORE) {
            e->lastError_ = message ? message : "core error";
            std::fprintf(stderr, "roadboard: pipewire core error: %s (%d)\n", e->lastError_.c_str(), res);
        }
    }
};

static const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = EngineCallbacks::onGlobal,
    .global_remove = EngineCallbacks::onGlobalRemove,
};

static const pw_filter_events kFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .state_changed = EngineCallbacks::onFilterState,
    .process = EngineCallbacks::onProcess,
};

static const pw_proxy_events kLinkEvents = {
    .version = PW_VERSION_PROXY_EVENTS,
    .destroy = EngineCallbacks::onLinkDestroy,
    .removed = EngineCallbacks::onLinkRemoved,
    .error = EngineCallbacks::onLinkError,
};

static const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = EngineCallbacks::onCoreError,
};

static const pw_node_events kNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .param = EngineCallbacks::onNodeParam,
};

// ------------------------------------------------------------------------------------------

Engine::Engine(Mixer& mixer) : mixer_(mixer) {
    pw_init(nullptr, nullptr);
    params().addStrListener([this](const std::string& key, const std::string&) {
        if (key.find(".device") == std::string::npos || !loop_) return;
        pw_thread_loop_lock(loop_);
        scheduleReconcile();
        pw_thread_loop_unlock(loop_);
    });
}

Engine::~Engine() { stop(); }

const char* Engine::virtualInputName(int v) { return kVinNames[v]; }
const char* Engine::virtualOutputName(int v) { return kVoutNames[v]; }
std::string Engine::virtualInputDescription(int v) {
    static const char* const d[kVirtStrips] = {"RoadBoard Input", "RoadBoard AUX Input", "RoadBoard RAIO3 Input"};
    return d[v];
}
std::string Engine::virtualOutputDescription(int v) { return "RoadBoard Out " + std::string(kBusNames[kHwBuses + v]); }
std::string Engine::raioInputName(int s) { return "roadboard_in_" + std::to_string(s + 1); }
std::string Engine::raioOutputName(int b) { return "roadboard_out_a" + std::to_string(b + 1); }
std::string Engine::raioInputDescription(int s) { return "RoadBoard In " + std::to_string(s + 1); }
std::string Engine::raioOutputDescription(int b) { return "RoadBoard Out " + std::string(kBusNames[b]); }
std::string Engine::fxSendName(int f) { return "roadboard_fx" + std::to_string(f + 1) + "_send"; }
std::string Engine::fxReturnName(int f) { return "roadboard_fx" + std::to_string(f + 1) + "_return"; }

bool Engine::start(std::string* error) {
    if (running_) return true;
    lastError_.clear();
    loop_ = pw_thread_loop_new("roadboard-pw", nullptr);
    if (!loop_) { if (error) *error = "cannot create PipeWire loop"; return false; }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) { if (error) *error = "cannot create PipeWire context"; pw_thread_loop_destroy(loop_); loop_ = nullptr; return false; }
    if (pw_thread_loop_start(loop_) < 0) { if (error) *error = "cannot start PipeWire thread"; return false; }

    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, pw_properties_new(PW_KEY_APP_NAME, "RoadBoard", nullptr), 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        if (error) *error = "cannot connect to PipeWire (is the PipeWire daemon running?)";
        stop();
        return false;
    }
    coreListener_ = new spa_hook{};
    pw_core_add_listener(core_, static_cast<spa_hook*>(coreListener_), &kCoreEvents, this);

    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    registryListener_ = new spa_hook{};
    pw_registry_add_listener(registry_, static_cast<spa_hook*>(registryListener_), &kRegistryEvents, this);

    createVirtualDevices();
    createFilter();

    timer_ = pw_loop_add_timer(pw_thread_loop_get_loop(loop_), EngineCallbacks::onTimer, this);
    timespec value{0, 100 * 1000 * 1000}, interval{0, 100 * 1000 * 1000};
    pw_loop_update_timer(pw_thread_loop_get_loop(loop_), timer_, &value, &interval, false);
    lastRoutingRev_ = params().routingRevision.load();
    running_ = true;
    pw_thread_loop_unlock(loop_);
    return true;
}

void Engine::stop() {
    if (!loop_) return;
    pw_thread_loop_lock(loop_);
    running_ = false;
    if (timer_) { pw_loop_destroy_source(pw_thread_loop_get_loop(loop_), timer_); timer_ = nullptr; }
    while (!links_.empty()) pw_proxy_destroy(static_cast<LinkCtx*>(links_.begin()->second)->proxy);
    if (filter_) { pw_filter_destroy(filter_); filter_ = nullptr; }
    delete static_cast<spa_hook*>(filterListener_); filterListener_ = nullptr;
    inPorts_.clear(); outPorts_.clear();
    destroyVirtualDevices();
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        for (auto& [id, n] : nodes_)
            if (auto* ctl = static_cast<NodeCtl*>(n.ctl)) { spa_hook_remove(&ctl->hook); pw_proxy_destroy(ctl->proxy); delete ctl; n.ctl = nullptr; }
    }
    if (metadata_) { pw_proxy_destroy(metadata_); metadata_ = nullptr; }
    if (registry_) {
        spa_hook_remove(static_cast<spa_hook*>(registryListener_));
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        registry_ = nullptr;
    }
    delete static_cast<spa_hook*>(registryListener_); registryListener_ = nullptr;
    if (core_) {
        spa_hook_remove(static_cast<spa_hook*>(coreListener_));
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    delete static_cast<spa_hook*>(coreListener_); coreListener_ = nullptr;
    pw_thread_loop_unlock(loop_);
    pw_thread_loop_stop(loop_);
    if (context_) { pw_context_destroy(context_); context_ = nullptr; }
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        nodes_.clear(); ports_.clear(); graphLinks_.clear(); ruleApplied_.clear();
    }
    rate_ = 0; quantum_ = 0;
}

void Engine::restart() {
    stop();
    start();
    if (onDevicesChanged) onDevicesChanged();
}

void Engine::destroyVirtualDevices() {
    while (!links_.empty()) pw_proxy_destroy(static_cast<LinkCtx*>(links_.begin()->second)->proxy);
    for (pw_proxy* p : virtualNodes_) pw_proxy_destroy(p);
    virtualNodes_.clear();
}

void Engine::createVirtualDevices() {
    ParamStore& P = params();
    auto make = [&](const std::string& name, const std::string& desc, const char* cls, int nch, const char* icon) {
        const std::string pos = positionList(nch);
        pw_properties* props = pw_properties_new(
            "factory.name", "support.null-audio-sink",
            PW_KEY_NODE_NAME, name.c_str(),
            PW_KEY_NODE_DESCRIPTION, desc.c_str(),
            PW_KEY_MEDIA_CLASS, cls,
            "audio.position", pos.c_str(),
            "monitor.channel-volumes", "true",
            PW_KEY_PRIORITY_DRIVER, "1",
            PW_KEY_OBJECT_LINGER, "false",
            PW_KEY_DEVICE_ICON_NAME, icon,
            nullptr);
        auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(core_, "adapter", PW_TYPE_INTERFACE_Node,
                                                                  PW_VERSION_NODE, &props->dict, 0));
        pw_properties_free(props);
        if (proxy) virtualNodes_.push_back(proxy);
    };
    for (int v = 0; v < kVirtStrips; ++v)
        make(kVinNames[v], virtualInputDescription(v), "Audio/Sink", P.opt.vinCh[v]->i(), "audio-card");
    for (int v = 0; v < kVirtBuses; ++v)
        make(kVoutNames[v], virtualOutputDescription(v), "Audio/Source/Virtual", P.opt.voutCh[v]->i(), "audio-input-microphone");
    lastRaioExt_ = P.opt.raioExt->i();
    if (lastRaioExt_) {
        for (int s = 0; s < kHwStrips; ++s) make(raioInputName(s), raioInputDescription(s), "Audio/Sink", 2, "audio-card");
        for (int b = 0; b < kHwBuses; ++b) make(raioOutputName(b), raioOutputDescription(b), "Audio/Source/Virtual", 2, "audio-input-microphone");
    }
    lastExtFx_ = P.opt.externalFx->i();
    if (lastExtFx_) {
        for (int f = 0; f < kExtFx; ++f) {
            make(fxSendName(f), "RoadBoard FX" + std::to_string(f + 1) + " Send", "Audio/Source/Virtual", 2, "audio-input-microphone");
            make(fxReturnName(f), "RoadBoard FX" + std::to_string(f + 1) + " Return", "Audio/Sink", 2, "audio-card");
        }
    }
}

void Engine::createFilter() {
    ParamStore& P = params();
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_NAME, kEngineNode,
        PW_KEY_NODE_DESCRIPTION, "RoadBoard Mixing Engine",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        PW_KEY_NODE_AUTOCONNECT, "false",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        nullptr);
    const int buffer = P.opt.buffer->i();
    const int sr = P.opt.sr->i();
    const int refRate = sr > 0 ? sr : 48000;
    if (buffer > 0) pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", buffer, refRate);
    if (sr > 0) pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", sr);

    filter_ = pw_filter_new(core_, "RoadBoard Engine", props);
    filterListener_ = new spa_hook{};
    pw_filter_add_listener(filter_, static_cast<spa_hook*>(filterListener_), &kFilterEvents, this);

    auto addPort = [&](pw_direction dir, const std::string& name) {
        std::string ch = name.substr(name.rfind('_') + 1);
        if (ch == "L") ch = "FL";
        if (ch == "R") ch = "FR";
        return pw_filter_add_port(filter_, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, name.c_str(),
                              PW_KEY_AUDIO_CHANNEL, ch.c_str(), nullptr), nullptr, 0);
    };
    inPorts_.assign(kEngineIn, nullptr);
    for (int i = 0; i < kEngineIn; ++i) inPorts_[size_t(i)] = addPort(PW_DIRECTION_INPUT, inPortName(i));
    outPorts_.assign(kEngineOut, nullptr);
    for (int i = 0; i < kEngineOut; ++i) outPorts_[size_t(i)] = addPort(PW_DIRECTION_OUTPUT, outPortName(i));

    if (pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        lastError_ = "cannot connect engine filter";
        std::fprintf(stderr, "roadboard: %s\n", lastError_.c_str());
    }
}

void Engine::onProcess(spa_io_position* position) {
    if (!position) return;
    const uint32_t n = uint32_t(position->clock.duration);
    const float rate = float(position->clock.rate.denom);
    const float* in[kEngineIn];
    float* out[kEngineOut];
    for (int i = 0; i < kEngineIn; ++i) in[i] = static_cast<const float*>(pw_filter_get_dsp_buffer(inPorts_[size_t(i)], n));
    for (int i = 0; i < kEngineOut; ++i) out[i] = static_cast<float*>(pw_filter_get_dsp_buffer(outPorts_[size_t(i)], n));
    rate_.store(rate, std::memory_order_relaxed);
    quantum_.store(int(n), std::memory_order_relaxed);
    mixer_.process(in, out, int(n), rate);
}

void Engine::scheduleReconcile() { reconcilePending_ = true; }

void Engine::onTimer() {
    mixer_.maintenance();
    ParamStore& P = params();
    if (P.routingRevision.load() != lastRoutingRev_ || P.opt.raioExt->i() != lastRaioExt_ || P.opt.externalFx->i() != lastExtFx_) {
        lastRoutingRev_ = P.routingRevision.load();
        destroyVirtualDevices();
        createVirtualDevices();
        reconcilePending_ = true;
    }
    if (reconcilePending_) { reconcilePending_ = false; reconcile(); }
    if (rulesPending_) { rulesPending_ = false; applyRules(); }
    if (devicesDirty_) { devicesDirty_ = false; if (onDevicesChanged) onDevicesChanged(); }
    if (streamsDirty_) { streamsDirty_ = false; if (onStreamsChanged) onStreamsChanged(); }
}

uint32_t Engine::nodeIdByName(const std::string& name) const {
    for (auto& [id, n] : nodes_) if (n.name == name) return id;
    return 0;
}

std::vector<const Engine::PortInfo*> Engine::nodePorts(uint32_t node, bool out, int monitor) const {
    std::vector<const PortInfo*> v;
    for (auto& [id, p] : ports_) {
        if (p.node != node || p.out != out) continue;
        if (monitor >= 0 && p.monitor != bool(monitor)) continue;
        v.push_back(&p);
    }
    std::sort(v.begin(), v.end(), [](const PortInfo* a, const PortInfo* b) { return a->id < b->id; });
    return v;
}

void Engine::reconcile() {
    if (!core_ || !filter_) return;
    std::set<std::pair<uint32_t, uint32_t>> want;
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        const uint32_t eng = nodeIdByName(kEngineNode);
        if (eng == 0) return;
        std::map<std::string, uint32_t> engIn, engOut;
        for (auto& [id, p] : ports_) {
            if (p.node != eng) continue;
            (p.out ? engOut : engIn)[p.name] = id;
        }
        ParamStore& P = params();

        // stereo source -> engine L/R ports (mono devices feed both sides)
        auto linkStereoSource = [&](uint32_t node, int monitor, const std::string& base) {
            auto src = nodePorts(node, true, monitor);
            if (src.empty()) return;
            const PortInfo *L = nullptr, *R = nullptr;
            for (auto* p : src) {
                if (p->channel == "FL") L = p;
                else if (p->channel == "FR") R = p;
                else if (p->channel == "MONO") { if (!L) L = p; if (!R) R = p; }
            }
            if (!L) L = src[0];
            if (!R) R = src.size() > 1 ? src[1] : src[0];
            if (engIn.count(base + "_L")) want.insert({L->id, engIn[base + "_L"]});
            if (engIn.count(base + "_R")) want.insert({R->id, engIn[base + "_R"]});
        };
        // engine output ports (by channel) -> device input ports
        auto linkToDevice = [&](uint32_t node, const std::function<std::string(int)>& portName, int maxCh) {
            auto dst = nodePorts(node, false, -1);
            for (size_t i = 0; i < dst.size(); ++i) {
                const PortInfo* p = dst[i];
                std::vector<int> chans;
                if (p->channel == "MONO") chans = {0, 1};
                else {
                    int k = posIndex(p->channel);
                    if (k < 0) k = int(i);
                    if (k < maxCh) chans = {k};
                }
                for (int k : chans) {
                    auto it = engOut.find(portName(k));
                    if (it != engOut.end()) want.insert({it->second, p->id});
                }
            }
        };

        for (int s = 0; s < kHwStrips; ++s) {
            std::string dev = P.str("strip[" + std::to_string(s) + "].device");
            if (!dev.empty()) {
                int monitor = 0;
                if (dev.size() > 8 && dev.compare(dev.size() - 8, 8, ".monitor") == 0) {
                    const std::string sink = dev.substr(0, dev.size() - 8);
                    if (nodeIdByName(dev) == 0 && nodeIdByName(sink) != 0) { dev = sink; monitor = 1; }
                }
                if (uint32_t node = nodeIdByName(dev)) linkStereoSource(node, monitor, "in_" + std::to_string(s + 1));
            }
            if (uint32_t node = nodeIdByName(raioInputName(s))) linkStereoSource(node, 1, "raio_" + std::to_string(s + 1));
        }
        for (int v = 0; v < kVirtStrips; ++v) {
            const uint32_t node = nodeIdByName(kVinNames[v]);
            if (!node) continue;
            const int s = kHwStrips + v;
            for (auto* p : nodePorts(node, true, 1)) {
                int k = p->channel == "MONO" ? 0 : posIndex(p->channel);
                if (k < 0 || k >= kBusCh) continue;
                auto it = engIn.find("in_" + std::to_string(s + 1) + "_" + kPos8[k]);
                if (it != engIn.end()) want.insert({p->id, it->second});
            }
        }
        for (int b = 0; b < kBuses; ++b) {
            auto busPort = [b](int k) { return outPortName(b * kBusCh + k); };
            if (b < kHwBuses) {
                const std::string dev = P.str("bus[" + std::to_string(b) + "].device");
                if (!dev.empty()) if (uint32_t node = nodeIdByName(dev)) linkToDevice(node, busPort, kBusCh);
                if (uint32_t node = nodeIdByName(raioOutputName(b))) linkToDevice(node, busPort, kBusCh);
            } else if (uint32_t node = nodeIdByName(kVoutNames[b - kHwBuses])) {
                linkToDevice(node, busPort, kBusCh);
            }
        }
        for (int f = 0; f < kExtFx; ++f) {
            if (uint32_t node = nodeIdByName(fxSendName(f)))
                linkToDevice(node, [f](int k) { return outPortName(kFxSendBase + f * 2 + k); }, 2);
            if (uint32_t node = nodeIdByName(fxReturnName(f))) linkStereoSource(node, 1, "fxret_" + std::to_string(f + 1));
        }
    }

    std::vector<LinkCtx*> drop;
    for (auto& [key, ctx] : links_) if (!want.count(key)) drop.push_back(static_cast<LinkCtx*>(ctx));
    for (LinkCtx* ctx : drop) pw_proxy_destroy(ctx->proxy);

    for (auto& key : want) {
        if (links_.count(key)) continue;
        uint32_t outNode = 0, inNode = 0;
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            auto a = ports_.find(key.first), b = ports_.find(key.second);
            if (a == ports_.end() || b == ports_.end()) continue;
            outNode = a->second.node; inNode = b->second.node;
        }
        pw_properties* props = pw_properties_new(nullptr, nullptr);
        pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", outNode);
        pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", key.first);
        pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", inNode);
        pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", key.second);
        pw_properties_set(props, PW_KEY_OBJECT_LINGER, "false");
        auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(core_, "link-factory", PW_TYPE_INTERFACE_Link,
                                                                  PW_VERSION_LINK, &props->dict, 0));
        pw_properties_free(props);
        if (!proxy) continue;
        auto* ctx = new LinkCtx{this, key, proxy, {}};
        pw_proxy_add_listener(proxy, &ctx->hook, &kLinkEvents, ctx);
        links_[key] = ctx;
    }
}

// ------------------------------------------------------------------ node control (volume / mute)

void Engine::bindNode(NodeInfo& n) {
    if (n.ctl || !registry_) return;
    auto* proxy = static_cast<pw_proxy*>(pw_registry_bind(registry_, n.id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!proxy) return;
    auto* ctl = new NodeCtl{this, n.id, proxy, {}, {}, false, false};
    pw_node_add_listener(reinterpret_cast<pw_node*>(proxy), &ctl->hook, &kNodeEvents, ctl);
    uint32_t ids[1] = {SPA_PARAM_Props};
    pw_node_subscribe_params(reinterpret_cast<pw_node*>(proxy), ids, 1);
    n.ctl = ctl;
}

void Engine::setNodeVolume(uint32_t nodeId, float linear) {
    if (!loop_) return;
    pw_thread_loop_lock(loop_);
    NodeCtl* ctl = nullptr;
    size_t nch = 2;
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        auto it = nodes_.find(nodeId);
        if (it != nodes_.end()) ctl = static_cast<NodeCtl*>(it->second.ctl);
        if (ctl && !ctl->vols.empty()) nch = ctl->vols.size();
    }
    if (ctl) {
        std::vector<float> v(nch, std::max(0.0f, linear));
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        spa_pod_frame f;
        spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, uint32_t(nch), v.data());
        auto* pod = static_cast<spa_pod*>(spa_pod_builder_pop(&b, &f));
        pw_node_set_param(reinterpret_cast<pw_node*>(ctl->proxy), SPA_PARAM_Props, 0, pod);
    }
    pw_thread_loop_unlock(loop_);
}

void Engine::setNodeMute(uint32_t nodeId, bool mute) {
    if (!loop_) return;
    pw_thread_loop_lock(loop_);
    NodeCtl* ctl = nullptr;
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        auto it = nodes_.find(nodeId);
        if (it != nodes_.end()) ctl = static_cast<NodeCtl*>(it->second.ctl);
    }
    if (ctl) {
        uint8_t buffer[256];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        spa_pod_frame f;
        spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&b, mute);
        auto* pod = static_cast<spa_pod*>(spa_pod_builder_pop(&b, &f));
        pw_node_set_param(reinterpret_cast<pw_node*>(ctl->proxy), SPA_PARAM_Props, 0, pod);
    }
    pw_thread_loop_unlock(loop_);
}

// ------------------------------------------------------------------ application streams

std::string Engine::streamTarget(uint32_t node, bool output) const {
    for (auto& [id, l] : graphLinks_) {
        const uint32_t other = output ? (l.first == node ? l.second : 0) : (l.second == node ? l.first : 0);
        if (!other) continue;
        auto it = nodes_.find(other);
        if (it != nodes_.end()) return it->second.name;
    }
    return {};
}

std::vector<AppStream> Engine::streams() const {
    std::lock_guard<std::mutex> lk(infoMutex_);
    std::vector<AppStream> v;
    for (auto& [id, n] : nodes_) {
        if (!isStreamClass(n.mediaClass) || n.appName == "RoadBoard") continue;
        AppStream s;
        s.id = id; s.serial = n.serial;
        s.appName = n.appName; s.mediaName = n.mediaName; s.binary = n.binary; s.icon = n.icon;
        s.output = n.mediaClass == "Stream/Output/Audio";
        s.target = streamTarget(id, s.output);
        for (auto& [nid, other] : nodes_) if (other.name == s.target) s.targetDescription = other.description;
        if (auto* ctl = static_cast<NodeCtl*>(n.ctl)) {
            if (ctl->have) {
                s.hasVolume = true;
                s.channels = int(ctl->vols.size());
                s.volume = ctl->vols.empty() ? 1.0f : *std::max_element(ctl->vols.begin(), ctl->vols.end());
            }
            s.mute = ctl->mute;
        }
        v.push_back(s);
    }
    std::sort(v.begin(), v.end(), [](const AppStream& a, const AppStream& b) {
        if (a.output != b.output) return a.output;
        return a.appName < b.appName;
    });
    return v;
}

void Engine::moveStream(uint32_t streamId, const std::string& target) {
    if (!loop_) return;
    pw_thread_loop_lock(loop_);
    if (metadata_) {
        auto* md = reinterpret_cast<pw_metadata*>(metadata_);
        uint64_t serial = 0;
        uint32_t nodeId = 0;
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            for (auto& [id, n] : nodes_) if (!target.empty() && n.name == target) { serial = n.serial; nodeId = id; }
        }
        if (target.empty() || !nodeId) {
            pw_metadata_set_property(md, streamId, "target.object", nullptr, nullptr);
            pw_metadata_set_property(md, streamId, "target.node", nullptr, nullptr);
        } else {
            const std::string s = std::to_string(serial), id = std::to_string(nodeId);
            pw_metadata_set_property(md, streamId, "target.object", "Spa:Id", s.c_str());
            pw_metadata_set_property(md, streamId, "target.node", "Spa:Id", id.c_str());
        }
    }
    pw_thread_loop_unlock(loop_);
}

void Engine::setAppRule(const std::string& app, bool output, const std::string& target) {
    std::lock_guard<std::mutex> lk(infoMutex_);
    if (target.empty()) rules_.erase(ruleKey(app, output));
    else rules_[ruleKey(app, output)] = target;
}
std::map<std::string, std::string> Engine::appRules() const { std::lock_guard<std::mutex> lk(infoMutex_); return rules_; }
void Engine::setAppRules(const std::map<std::string, std::string>& r) {
    { std::lock_guard<std::mutex> lk(infoMutex_); rules_ = r; ruleApplied_.clear(); }
    rulesPending_ = true;
}

void Engine::applyRules() {
    std::vector<std::pair<uint32_t, std::string>> moves;
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        for (auto& [id, n] : nodes_) {
            if (!isStreamClass(n.mediaClass) || ruleApplied_.count(id)) continue;
            const bool output = n.mediaClass == "Stream/Output/Audio";
            auto it = rules_.find(ruleKey(n.appName, output));
            if (it == rules_.end()) continue;
            if (nodeIdByName(it->second) == 0) continue;     // target not available yet
            ruleApplied_.insert(id);
            if (streamTarget(id, output) != it->second) moves.push_back({id, it->second});
        }
    }
    if (!metadata_) { if (!moves.empty()) rulesPending_ = true; return; }
    auto* md = reinterpret_cast<pw_metadata*>(metadata_);
    for (auto& [id, target] : moves) {
        uint64_t serial = 0; uint32_t nodeId = 0;
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            for (auto& [nid, n] : nodes_) if (n.name == target) { serial = n.serial; nodeId = nid; }
        }
        const std::string s = std::to_string(serial), i = std::to_string(nodeId);
        pw_metadata_set_property(md, id, "target.object", "Spa:Id", s.c_str());
        pw_metadata_set_property(md, id, "target.node", "Spa:Id", i.c_str());
    }
}

// ------------------------------------------------------------------ device lists

static void fillVolume(const Engine::NodeInfo& n, AudioDevice& d) {
    if (auto* ctl = static_cast<NodeCtl*>(n.ctl)) {
        if (ctl->have) {
            d.hasVolume = true;
            d.volume = ctl->vols.empty() ? 1.0f : *std::max_element(ctl->vols.begin(), ctl->vols.end());
        }
        d.mute = ctl->mute;
    }
}

std::vector<AudioDevice> Engine::inputDevices() const {
    std::lock_guard<std::mutex> lk(infoMutex_);
    std::vector<AudioDevice> v;
    for (auto& [id, n] : nodes_) {
        const bool source = n.mediaClass.rfind("Audio/Source", 0) == 0;
        const bool sink = n.mediaClass == "Audio/Sink";
        if ((!source && !sink) || isOwnNode(n.name)) continue;
        AudioDevice d;
        d.id = id; d.api = n.api;
        if (source) {
            d.name = n.name; d.description = n.description; d.mediaClass = n.mediaClass;
            for (auto& [pid, p] : ports_) if (p.node == id && p.out) ++d.channels;
        } else {
            d.name = n.name + ".monitor"; d.description = "Monitor of " + n.description;
            d.mediaClass = "Audio/Source"; d.monitor = true;
            for (auto& [pid, p] : ports_) if (p.node == id && p.out && p.monitor) ++d.channels;
        }
        v.push_back(d);
    }
    std::sort(v.begin(), v.end(), [](const AudioDevice& a, const AudioDevice& b) {
        if (a.monitor != b.monitor) return !a.monitor;
        return a.description < b.description;
    });
    return v;
}

std::vector<AudioDevice> Engine::outputDevices() const {
    std::lock_guard<std::mutex> lk(infoMutex_);
    std::vector<AudioDevice> v;
    for (auto& [id, n] : nodes_) {
        if ((n.mediaClass != "Audio/Sink" && n.mediaClass != "Audio/Duplex") || isOwnNode(n.name)) continue;
        AudioDevice d;
        d.id = id; d.name = n.name; d.description = n.description; d.mediaClass = n.mediaClass; d.api = n.api;
        for (auto& [pid, p] : ports_) if (p.node == id && !p.out) ++d.channels;
        v.push_back(d);
    }
    std::sort(v.begin(), v.end(), [](const AudioDevice& a, const AudioDevice& b) { return a.description < b.description; });
    return v;
}

static std::vector<AudioDevice> listClass(const std::map<uint32_t, Engine::NodeInfo>& nodes,
                                          const std::map<uint32_t, Engine::PortInfo>& ports, bool sinks) {
    std::vector<AudioDevice> v;
    for (auto& [id, n] : nodes) {
        const bool match = sinks ? (n.mediaClass == "Audio/Sink" || n.mediaClass == "Audio/Duplex")
                                 : n.mediaClass.rfind("Audio/Source", 0) == 0;
        if (!match) continue;
        AudioDevice d;
        d.id = id; d.name = n.name; d.description = n.description; d.mediaClass = n.mediaClass; d.api = n.api;
        d.virtualDevice = Engine::isOwnNode(n.name);
        for (auto& [pid, p] : ports) if (p.node == id && p.out != sinks && !p.monitor) ++d.channels;
        fillVolume(n, d);
        v.push_back(d);
    }
    std::sort(v.begin(), v.end(), [](const AudioDevice& a, const AudioDevice& b) {
        if (a.virtualDevice != b.virtualDevice) return a.virtualDevice;
        return a.virtualDevice ? a.name < b.name : a.description < b.description;
    });
    return v;
}

std::vector<AudioDevice> Engine::allSinks() const { std::lock_guard<std::mutex> lk(infoMutex_); return listClass(nodes_, ports_, true); }
std::vector<AudioDevice> Engine::allSources() const { std::lock_guard<std::mutex> lk(infoMutex_); return listClass(nodes_, ports_, false); }

std::string Engine::describe(const std::string& nodeName) const {
    std::lock_guard<std::mutex> lk(infoMutex_);
    std::string name = nodeName;
    bool mon = false;
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ".monitor") == 0 && nodeIdByName(name) == 0) {
        name = name.substr(0, name.size() - 8); mon = true;
    }
    for (auto& [id, n] : nodes_) if (n.name == name) return mon ? "Monitor of " + n.description : n.description;
    return {};
}

bool Engine::deviceConnected(const std::string& nodeName) const { return !describe(nodeName).empty(); }

void Engine::setDefaultNode(const std::string& nodeName, bool sink) {
    if (!loop_) return;
    pw_thread_loop_lock(loop_);
    if (metadata_) {
        const std::string json = "{ \"name\": \"" + nodeName + "\" }";
        pw_metadata_set_property(reinterpret_cast<pw_metadata*>(metadata_), PW_ID_CORE,
                                 sink ? "default.configured.audio.sink" : "default.configured.audio.source",
                                 "Spa:String:JSON", json.c_str());
    }
    pw_thread_loop_unlock(loop_);
}

void Engine::setSystemDefault(int v) { setDefaultNode(kVinNames[v], true); }
void Engine::setSystemDefaultSource(int v) { setDefaultNode(kVoutNames[v], false); }

std::string Engine::statusText() const {
    std::ostringstream os;
    if (!running_) { os << "Engine stopped"; if (!lastError_.empty()) os << " (" << lastError_ << ")"; return os.str(); }
    const float r = rate_.load();
    const int q = quantum_.load();
    if (r <= 0) return "Engine idle";
    char ms[16];
    std::snprintf(ms, sizeof ms, "%.1f", 1000.0 * q / r);
    os << int(r) << " Hz   " << q << " smp   " << ms << " ms";
    return os.str();
}

} // namespace road
