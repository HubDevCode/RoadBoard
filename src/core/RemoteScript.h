// Request scripting:
//   Strip(0).Mute = 1;  Bus[1].Gain -= 3;  Strip(2).FadeTo = (-10.0, 500);
//   Bus(0).mode.Amix = 1;  Command.Restart = 1;  Recorder.Load = "/path/file.wav";
// Used by the CLI socket, RoadLink text, MIDI mapping and macro buttons.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace road {

class RemoteScript {
public:
    static RemoteScript& instance();

    // Execute a script (several requests separated by ';' or new lines).
    // Returns the number of requests that were understood; errors appended to *errors.
    int execute(const std::string& script, std::string* errors = nullptr);

    // Read a value ("Strip(0).Gain" -> "-10.0"). Returns false when unknown.
    bool get(const std::string& name, std::string* out);

    // Commands (actions) with optional string/number argument. Indexed actions are
    // registered with "[]" (e.g. "command.savebuseq[]") and receive the index.
    using Action = std::function<void(const std::string& arg, float value, int index)>;
    void registerAction(const std::string& key, Action a);

    // timed fades, advanced by tick() (called ~100 Hz from a timer)
    void tick();

private:
    RemoteScript() = default;
    bool executeOne(const std::string& stmt, std::string* errors);
    std::string resolveAlias(const std::string& key) const;

    struct Fade { std::string key; float from, to; double start, duration; };
    std::mutex mutex_;
    std::vector<Fade> fades_;
    std::map<std::string, Action> actions_;
};

inline RemoteScript& remote() { return RemoteScript::instance(); }

} // namespace road
