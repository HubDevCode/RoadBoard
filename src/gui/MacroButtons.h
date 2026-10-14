// Macro Buttons: programmable push / 2-position buttons running request
// scripts, triggered by mouse, keyboard, MIDI, audio level (auto-ducking)
// or tape deck events.
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QKeySequence>
#include <QWidget>
#include <vector>

class QGridLayout;

namespace road {

struct MacroButton {
    QString name, subName;
    bool toggle = false;           // 2 positions
    int color = 0;                 // 0..8
    QString initScript, pushScript, releaseScript;
    QKeySequence key;
    int midiStatus = -1, midiD1 = -1;
    bool triggerOn = false;
    int triggerStrip = 0;
    float triggerIn = -30, triggerOut = -40;
    int triggerHoldMs = 500;
    QString event;                 // "", "play", "stop", "record", "eof"
    // runtime
    bool state = false;
    double triggerHoldUntil = 0;
    QJsonObject toJson() const;
    static MacroButton fromJson(const QJsonObject& o);
};

class MacroButtonsWindow : public QWidget {
    Q_OBJECT
public:
    explicit MacroButtonsWindow(QWidget* parent = nullptr);
    void setButtonState(int i, bool on, bool runScripts);
    void setTriggerEnabled(int i, bool on);
    void setButtonColor(int i, int color);
    void tick();
    void onTapeEvent(const QString& e);
    void onMidi(int status, int d1, int d2);
    void runInitScripts();

protected:
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void rebuild();
    void save();
    void load();
    void press(int i);
    void release(int i);
    void configure(int i);
    void runScript(const QString& script, int self);

    std::vector<MacroButton> buttons_;
    int rows_ = 4, cols_ = 4;
    QGridLayout* grid_ = nullptr;
    std::vector<QWidget*> widgets_;
    int learnMidi_ = -1;
};

} // namespace road
