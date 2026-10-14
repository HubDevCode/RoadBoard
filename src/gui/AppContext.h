#pragma once

namespace road {

class Engine;
class Mixer;
class TapeDeck;
class RoadLinkEngine;
class MidiEngine;
class MainWindow;

// services shared by all GUI parts
struct AppContext {
    Engine* engine = nullptr;
    Mixer* mixer = nullptr;
    TapeDeck* tape = nullptr;
    RoadLinkEngine* link = nullptr;
    MidiEngine* midi = nullptr;
    MainWindow* window = nullptr;
};

AppContext& ctx();

} // namespace road
