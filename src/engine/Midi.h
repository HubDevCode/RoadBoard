// MIDI remote control through the ALSA sequencer (works with any MIDI
// controller on every distribution, also through PipeWire's ALSA bridge).
// Learn mapping, push-to-talk, F / FF feedback for motorised faders & LEDs.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace road {

struct MidiPort {
    int client = 0, port = 0;
    std::string name;
};

struct MidiMapping {
    std::string target;      // parameter key (e.g. "strip[0].gain") or action ("recorder.play")
    int type = -1;           // 0 = CC, 1 = Note, 2 = Pitch bend, -1 = unassigned
    int channel = 0;         // 0..15
    int number = 0;          // CC / note number
    bool ptt = false;        // push-to-talk (mute targets only)
    int feedback = 0;        // 0 none, 1 'F', 2 'FF'
    int onValue = 127, offValue = 0;
    float lastSent = -1e9f;
};

class MidiEngine {
public:
    MidiEngine();
    ~MidiEngine();

    bool start();
    void stop();
    bool available() const { return seq_ != nullptr; }

    std::vector<MidiPort> inputPorts() const;   // devices that can send to us
    std::vector<MidiPort> outputPorts() const;  // devices that can receive feedback
    void connectInput(int slot /*0 main, 1 extra*/, const std::string& name);
    void connectOutput(const std::string& name);
    std::string inputName(int slot) const { return inName_[slot]; }
    std::string outputName() const { return outName_; }

    // mapping
    std::vector<MidiMapping> mappings() const;
    void setMappings(const std::vector<MidiMapping>& m);
    void learn(const std::string& target);      // empty = cancel
    std::string learning() const;
    void resetMap();
    std::string mapName;
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    static std::vector<std::pair<std::string, std::string>> targets(); // (key, label)

    // feedback: call from a timer (non realtime)
    void sendFeedback(bool forceAll = false);
    void sendRaw(int status, int d1, int d2);

    // every received message is forwarded (macro buttons, dialogs)
    std::function<void(int status, int d1, int d2)> onMessage;
    // last received message for display
    std::atomic<int> lastStatus{-1}, lastD1{0}, lastD2{0};

private:
    void loop();
    void handle(int status, int d1, int d2);
    void apply(MidiMapping& m, int value, bool pressed);

    void* seq_ = nullptr;        // snd_seq_t
    int inPort_ = -1, outPort_ = -1, clientId_ = -1;
    std::string inName_[2], outName_;
    std::thread thread_;
    std::atomic<bool> run_{false};
    mutable std::mutex mutex_;
    std::vector<MidiMapping> map_;
    std::string learn_;
};

} // namespace road
