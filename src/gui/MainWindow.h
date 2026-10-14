#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QTimer>
#include <vector>

namespace road {

class StripBase;
class RecorderBar;
class Caption;
class MacroButtonsWindow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    void showMenu(QPoint pos);
    void devicesChanged();          // thread safe (queued)
    void streamsChanged();
    void applyWindowOptions();
    void showWindow(int mode);      // 0 hide, 1 show, 2 show on top
    MacroButtonsWindow* macroButtons();
    void restartEngine();
    void shutdown();

protected:
    void closeEvent(QCloseEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void buildUi();
    void tick();
    void setupTray();
    void registerActions();
    void autosave();
    void checkLostDevices();

    std::vector<StripBase*> strips_;
    std::vector<StripBase*> buses_;
    RecorderBar* recorder_ = nullptr;
    Caption* status_ = nullptr;
    QTimer timer_, fastTimer_, saveTimer_;
    QSystemTrayIcon* tray_ = nullptr;
    MacroButtonsWindow* macro_ = nullptr;
    uint32_t savedRev_ = 0;
    bool quitting_ = false;
    bool a1WasConnected_ = false;
};

} // namespace road
