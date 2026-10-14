#pragma once

class QWidget;

namespace road {

// detailed compressor / gate parameters with transfer graph
void openCompGateDetail(QWidget* parent, int strip);
// denoiser + Voice Modeler (pitch / formants, 8 presets)
void openDenoiserDetail(QWidget* parent, int strip);
// Voice Modeler presets (shared by all strips)
bool recallVoicePreset(int strip, int preset);

} // namespace road
