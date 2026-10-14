#pragma once

class QWidget;

namespace road {

// Application Mixer: for every application choose where it plays
// (Virtual Input 1/2/3, RAIO inputs, hardware) or what it records, with
// volume / mute, remembered routing and a device page.
void openAppMixer(QWidget* parent);
void loadAppRules();     // apply remembered routing to the engine (startup)

} // namespace road
