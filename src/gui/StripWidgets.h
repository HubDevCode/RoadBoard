// Input strips, master bus strips, hardware out header and the cassette deck.
#pragma once

#include <QWidget>
#include <vector>

#include "gui/Widgets.h"

namespace road {

class StripBase : public QWidget {
    Q_OBJECT
public:
    explicit StripBase(int index, QWidget* parent = nullptr) : QWidget(parent), index_(index) {}
    virtual void tick() = 0;           // meters (30 Hz)
    virtual void refreshDevices() {}
    int index() const { return index_; }
protected:
    void paintEvent(QPaintEvent*) override;
    int index_;
};

// A1..B3 assignment button. Right click swaps every strip to the "sub-mix"
// view where the button shows the strip gain layer for that bus
// (Ctrl+wheel adjusts it, Ctrl+click types it).
class BusAssignButton : public QWidget, public Syncable {
    Q_OBJECT
public:
    BusAssignButton(int strip, int bus, QWidget* parent = nullptr);
    void sync() override;
    static bool layerView;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    int strip_, bus_;
    float lastOn_ = -1, lastLayer_ = -999;
    bool lastView_ = false;
};

// strip fader that follows the BUS SEL buttons (shows / edits a gain layer)
class StripFader : public Fader {
    Q_OBJECT
public:
    StripFader(int strip, QWidget* parent = nullptr);
    void sync() override;
private:
    int strip_;
};

// applications connected to a virtual input (volume + mute), like the system volume mixer
class AppStripList : public QWidget {
    Q_OBJECT
public:
    AppStripList(int strip, QWidget* parent = nullptr);
    void refresh();
    QSize sizeHint() const override { return {100, 4 * 17 + 2}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    struct Row { uint32_t id; QString name; float vol; bool mute; };
    int rowAt(QPointF p) const;
    void setVolFromX(int row, double x);
    int strip_;
    std::vector<Row> rows_;
    int drag_ = -1;
};

class HwStripWidget : public StripBase {
    Q_OBJECT
public:
    explicit HwStripWidget(int strip, QWidget* parent = nullptr);
    void tick() override;
    void refreshDevices() override;
private:
    void showPadMenu(QPoint pos);
    DeviceButton* device_;
    Pad2D* pad_;
    LevelMeter* meter_;
};

class VirtStripWidget : public StripBase {
    Q_OBJECT
public:
    explicit VirtStripWidget(int strip, QWidget* parent = nullptr);
    void tick() override;
    void refreshDevices() override;
    void refreshApps() { apps_->refresh(); }
private:
    void showHeaderMenu(QPoint pos);
    void showPanMenu(QPoint pos);
    DeviceButton* header_;
    Pad2D* pad_;
    LevelMeter* meter_;
    AppStripList* apps_;
};

class BusStripWidget : public StripBase {
    Q_OBJECT
public:
    explicit BusStripWidget(int bus, QWidget* parent = nullptr);
    void tick() override;
    void refreshDevices() override;
private:
    void showModeMenu(QPoint pos);
    void showSelMenu(QPoint pos);
    DeviceButton* header_;
    LevelMeter* meter_;
};

// shared strip context menu: reset / copy / paste / load / save by section
void showStripMenu(QWidget* parent, int strip, QPoint pos);
// device selection popups (PipeWire devices)
void showInputDeviceMenu(QWidget* parent, int strip, QPoint pos);
void showOutputDeviceMenu(QWidget* parent, int bus, QPoint pos);
QString deviceDisplayName(const std::string& nodeName, bool* connected);

} // namespace road
