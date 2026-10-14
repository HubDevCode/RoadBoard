#include "engine/Midi.h"

#include <alsa/asoundlib.h>
#include <poll.h>

#include <cmath>
#include <fstream>
#include <sstream>

#include "core/Params.h"
#include "core/RemoteScript.h"
#include "core/Settings.h"

namespace road {

namespace {
snd_seq_t* S(void* p) { return static_cast<snd_seq_t*>(p); }

float toNorm(const Param* p, float v) { return p->max > p->min ? (v - p->min) / (p->max - p->min) : 0; }
bool isBool(const Param* p) { return p->integer && p->min == 0 && p->max == 1; }
} // namespace

MidiEngine::MidiEngine() = default;
MidiEngine::~MidiEngine() { stop(); }

bool MidiEngine::start() {
    if (seq_) return true;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) return false;
    snd_seq_set_client_name(seq, "RoadBoard");
    clientId_ = snd_seq_client_id(seq);
    inPort_ = snd_seq_create_simple_port(seq, "RoadBoard MIDI In",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    outPort_ = snd_seq_create_simple_port(seq, "RoadBoard MIDI Feedback",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    seq_ = seq;
    run_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void MidiEngine::stop() {
    run_ = false;
    if (thread_.joinable()) thread_.join();
    if (seq_) { snd_seq_close(S(seq_)); seq_ = nullptr; }
}

static std::vector<MidiPort> listPorts(snd_seq_t* seq, int self, unsigned capsNeeded) {
    std::vector<MidiPort> out;
    if (!seq) return out;
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        const int client = snd_seq_client_info_get_client(cinfo);
        if (client == self || client == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            const unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & capsNeeded) != capsNeeded) continue;
            if (caps & SND_SEQ_PORT_CAP_NO_EXPORT) continue;
            MidiPort p;
            p.client = client;
            p.port = snd_seq_port_info_get_port(pinfo);
            p.name = std::string(snd_seq_client_info_get_name(cinfo)) + ": " + snd_seq_port_info_get_name(pinfo);
            out.push_back(p);
        }
    }
    return out;
}

std::vector<MidiPort> MidiEngine::inputPorts() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return listPorts(S(seq_), clientId_, SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
}

std::vector<MidiPort> MidiEngine::outputPorts() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return listPorts(S(seq_), clientId_, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
}

void MidiEngine::connectInput(int slot, const std::string& name) {
    if (!seq_) return;
    auto ports = inputPorts();
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& p : ports) if (p.name == inName_[slot]) snd_seq_disconnect_from(S(seq_), inPort_, p.client, p.port);
    inName_[slot] = name;
    for (auto& p : ports) if (p.name == name) snd_seq_connect_from(S(seq_), inPort_, p.client, p.port);
}

void MidiEngine::connectOutput(const std::string& name) {
    if (!seq_) return;
    auto ports = outputPorts();
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& p : ports) if (p.name == outName_) snd_seq_disconnect_to(S(seq_), outPort_, p.client, p.port);
    outName_ = name;
    for (auto& p : ports) if (p.name == name) snd_seq_connect_to(S(seq_), outPort_, p.client, p.port);
}

std::vector<MidiMapping> MidiEngine::mappings() const { std::lock_guard<std::mutex> lk(mutex_); return map_; }
void MidiEngine::setMappings(const std::vector<MidiMapping>& m) { std::lock_guard<std::mutex> lk(mutex_); map_ = m; }
void MidiEngine::learn(const std::string& target) { std::lock_guard<std::mutex> lk(mutex_); learn_ = target; }
std::string MidiEngine::learning() const { std::lock_guard<std::mutex> lk(mutex_); return learn_; }
void MidiEngine::resetMap() { std::lock_guard<std::mutex> lk(mutex_); map_.clear(); }

std::vector<std::pair<std::string, std::string>> MidiEngine::targets() {
    std::vector<std::pair<std::string, std::string>> t;
    for (int s = 0; s < kStrips; ++s) {
        const std::string k = "strip[" + std::to_string(s) + "].";
        const std::string l = "Strip " + std::to_string(s + 1) + " ";
        t.push_back({k + "gain", l + "Fader"});
        t.push_back({k + "mute", l + "Mute"});
        t.push_back({k + "solo", l + "Solo"});
        if (isHwStrip(s)) {
            t.push_back({k + "mono", l + "Mono"});
            t.push_back({k + "comp", l + "Comp"});
            t.push_back({k + "gate", l + "Gate"});
            t.push_back({k + "denoiser", l + "Denoiser"});
            t.push_back({k + "reverb", l + "Reverb send"});
            t.push_back({k + "delay", l + "Delay send"});
            t.push_back({k + "eq.on", l + "EQ on"});
            t.push_back({k + "pitch.on", l + "Voice Modeler on"});
            t.push_back({k + "color_x", l + "Color X"});
            t.push_back({k + "color_y", l + "Color Y"});
            t.push_back({k + "fx_x", l + "Modulation X"});
            t.push_back({k + "fx_y", l + "Modulation Y"});
            t.push_back({k + "pan_x", l + "3D Pan X"});
            t.push_back({k + "pan_y", l + "3D Pan Y"});
        } else {
            t.push_back({k + "mc", l + "M.C"});
            t.push_back({k + "eqgain3", l + "Treble"});
            t.push_back({k + "eqgain2", l + "Medium"});
            t.push_back({k + "eqgain1", l + "Bass"});
            t.push_back({k + "pan_x", l + "Pan X"});
            t.push_back({k + "pan_y", l + "Pan Y"});
        }
        for (int b = 0; b < kBuses; ++b) t.push_back({k + kBusNames[b], l + "-> " + kBusNames[b]});
        for (int b = 0; b < kBuses; ++b) t.push_back({k + "gainlayer[" + std::to_string(b) + "]", l + "level in " + kBusNames[b]});
    }
    for (int b = 0; b < kBuses; ++b) {
        const std::string k = "bus[" + std::to_string(b) + "].";
        const std::string l = std::string("Bus ") + kBusNames[b] + " ";
        t.push_back({k + "gain", l + "Fader"});
        t.push_back({k + "mute", l + "Mute"});
        t.push_back({k + "mono", l + "Mono"});
        t.push_back({k + "eq.on", l + "EQ"});
        t.push_back({k + "sel", l + "SEL"});
        t.push_back({k + "returnreverb", l + "Reverb return"});
        t.push_back({k + "returndelay", l + "Delay return"});
    }
    t.push_back({"fx.reverb.on", "Reverb on"});
    t.push_back({"fx.delay.on", "Delay / multiband on"});
    for (const char* a : {"play", "stop", "record", "pause", "rew", "ff", "replay"})
        t.push_back({std::string("recorder.") + a, std::string("Tape ") + a});
    t.push_back({"recorder.gain", "Tape playback gain"});
    t.push_back({"command.restart", "Restart audio engine"});
    t.push_back({"command.refreshmidi", "Refresh MIDI controller"});
    return t;
}

void MidiEngine::loop() {
    snd_seq_t* seq = S(seq_);
    const int nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    std::vector<pollfd> fds(size_t(std::max(nfds, 1)));
    snd_seq_poll_descriptors(seq, fds.data(), unsigned(nfds), POLLIN);
    while (run_) {
        if (poll(fds.data(), nfds_t(nfds), 100) <= 0) continue;
        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
            switch (ev->type) {
            case SND_SEQ_EVENT_CONTROLLER:
                handle(0xB0 | (ev->data.control.channel & 0x0F), int(ev->data.control.param) & 0x7F, int(ev->data.control.value) & 0x7F);
                break;
            case SND_SEQ_EVENT_NOTEON:
                handle(0x90 | (ev->data.note.channel & 0x0F), ev->data.note.note, ev->data.note.velocity);
                break;
            case SND_SEQ_EVENT_NOTEOFF:
                handle(0x80 | (ev->data.note.channel & 0x0F), ev->data.note.note, 0);
                break;
            case SND_SEQ_EVENT_PITCHBEND: {
                const int v = ev->data.control.value + 8192;
                handle(0xE0 | (ev->data.control.channel & 0x0F), v & 0x7F, (v >> 7) & 0x7F);
                break;
            }
            case SND_SEQ_EVENT_PGMCHANGE:
                handle(0xC0 | (ev->data.control.channel & 0x0F), int(ev->data.control.value) & 0x7F, 0);
                break;
            default: break;
            }
        }
    }
}

void MidiEngine::handle(int status, int d1, int d2) {
    lastStatus = status; lastD1 = d1; lastD2 = d2;
    if (onMessage) onMessage(status, d1, d2);
    const int kind = status & 0xF0, ch = status & 0x0F;
    int type = -1;
    if (kind == 0xB0) type = 0;
    else if (kind == 0x90 || kind == 0x80) type = 1;
    else if (kind == 0xE0) type = 2;
    if (type < 0) return;
    const bool pressed = (kind == 0x90 && d2 > 0) || (kind == 0xB0 && d2 >= 64);
    int value = type == 2 ? (d2 << 7 | d1) : d2;

    std::unique_lock<std::mutex> lk(mutex_);
    if (!learn_.empty()) {
        // a learnt message replaces any previous use of the same target
        MidiMapping* m = nullptr;
        for (auto& x : map_) if (x.target == learn_) m = &x;
        if (!m) { map_.push_back({}); m = &map_.back(); m->target = learn_; }
        m->type = type; m->channel = ch; m->number = type == 2 ? 0 : d1;
        learn_.clear();
        return;
    }
    std::vector<std::string> actions;
    for (auto& m : map_) {
        if (m.type != type || m.channel != ch) continue;
        if (type != 2 && m.number != d1) continue;
        if (kind == 0x80 && !m.ptt) continue; // note off only matters for PTT
        if (!params().find(m.target)) { if (pressed) actions.push_back(m.target + "=1;"); continue; }
        apply(m, value, pressed);
    }
    lk.unlock();
    for (auto& a : actions) remote().execute(a);
}

void MidiEngine::apply(MidiMapping& m, int value, bool pressed) {
    Param* p = params().find(m.target);
    if (!p) return;
    const float maxIn = m.type == 2 ? 16383.0f : 127.0f;
    if (isBool(p)) {
        if (m.ptt) p->set(m.target.find("mute") != std::string::npos ? (pressed ? 0 : 1) : (pressed ? 1 : 0));
        else if (m.type == 0 && !(value == 0 || value == 127 || value == 64)) p->set(value >= 64 ? 1 : 0); // switch CC
        else if (pressed) p->set(p->on() ? 0 : 1);
    } else if (m.type == 1) {
        if (pressed) p->set(p->def);
    } else {
        float maxV = p->max;
        if (m.target.find(".gain") != std::string::npos && Settings::options().limitRemoteGain) maxV = std::min(maxV, 0.0f);
        p->set(p->min + (maxV - p->min) * (float(value) / maxIn));
    }
    if (m.feedback == 1) m.lastSent = p->get(); // 'F': do not echo our own changes back
}

void MidiEngine::sendRaw(int status, int d1, int d2) {
    if (!seq_) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, outPort_);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    const int kind = status & 0xF0, ch = status & 0x0F;
    if (kind == 0xB0) snd_seq_ev_set_controller(&ev, ch, d1, d2);
    else if (kind == 0x90) snd_seq_ev_set_noteon(&ev, ch, d1, d2);
    else if (kind == 0x80) snd_seq_ev_set_noteoff(&ev, ch, d1, d2);
    else if (kind == 0xE0) snd_seq_ev_set_pitchbend(&ev, ch, (d2 << 7 | d1) - 8192);
    else return;
    snd_seq_event_output_direct(S(seq_), &ev);
}

void MidiEngine::sendFeedback(bool forceAll) {
    if (!seq_) return;
    std::vector<std::tuple<int, int, int>> msgs;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& m : map_) {
            if (m.type < 0 || (m.feedback == 0 && !forceAll)) continue;
            Param* p = params().find(m.target);
            if (!p) continue;
            const float v = p->get();
            if (!forceAll && v == m.lastSent) continue;
            m.lastSent = v;
            if (m.type == 0) {
                const int val = isBool(p) ? (p->on() ? m.onValue : m.offValue) : int(std::lround(toNorm(p, v) * 127));
                msgs.emplace_back(0xB0 | m.channel, m.number, std::clamp(val, 0, 127));
            } else if (m.type == 1) {
                const int val = isBool(p) ? (p->on() ? m.onValue : m.offValue) : (v != p->def ? 127 : 0);
                msgs.emplace_back(0x90 | m.channel, m.number, std::clamp(val, 0, 127));
            } else {
                const int val = int(std::lround(toNorm(p, v) * 16383));
                msgs.emplace_back(0xE0 | m.channel, val & 0x7F, (val >> 7) & 0x7F);
            }
        }
    }
    for (auto& [s, a, b] : msgs) sendRaw(s, a, b);
}

bool MidiEngine::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "# RoadBoard MIDI map\nname " << mapName << "\n";
    f << "input0 " << inName_[0] << "\ninput1 " << inName_[1] << "\noutput " << outName_ << "\n";
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& m : map_)
        f << "map " << m.target << ' ' << m.type << ' ' << m.channel << ' ' << m.number << ' ' << int(m.ptt) << ' '
          << m.feedback << ' ' << m.onValue << ' ' << m.offValue << "\n";
    return true;
}

bool MidiEngine::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::vector<MidiMapping> map;
    std::string line, in0, in1, out;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string tag;
        is >> tag;
        std::string rest;
        std::getline(is, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        if (tag == "name") mapName = rest;
        else if (tag == "input0") in0 = rest;
        else if (tag == "input1") in1 = rest;
        else if (tag == "output") out = rest;
        else if (tag == "map") {
            std::istringstream ms(rest);
            MidiMapping m;
            int ptt = 0;
            ms >> m.target >> m.type >> m.channel >> m.number >> ptt >> m.feedback >> m.onValue >> m.offValue;
            m.ptt = ptt != 0;
            map.push_back(m);
        }
    }
    setMappings(map);
    if (!in0.empty()) connectInput(0, in0);
    if (!in1.empty()) connectInput(1, in1);
    if (!out.empty()) connectOutput(out);
    return true;
}

} // namespace road
