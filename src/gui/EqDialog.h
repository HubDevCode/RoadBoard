#pragma once

class QWidget;

namespace road {

// Parametric EQ dialog: 6 cells x 8 channels (BUS) or stereo (hardware strip),
// trim / delay per channel, A/B memories, flat, channel copy, copy all, load / save.
void openBusEqDialog(QWidget* parent, int bus);
void openStripEqDialog(QWidget* parent, int strip);

} // namespace road
