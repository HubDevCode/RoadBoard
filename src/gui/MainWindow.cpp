#include "gui/MainWindow.h"

#include <QApplication>
#include <cmath>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "core/RemoteScript.h"
#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/Midi.h"
#include "engine/Mixer.h"
#include "engine/TapeDeck.h"
#include "engine/RoadLink.h"
#include "gui/AppContext.h"
#include "gui/Dialogs.h"
#include "gui/AppMixer.h"
#include "gui/Detail.h"
#include "gui/EqDialog.h"
#include "gui/FxWindow.h"
#include "gui/StreamerView.h"
#include "gui/MacroButtons.h"
#include "gui/RecorderBar.h"
#include "gui/StripWidgets.h"
#include "gui/Theme.h"

namespace road {

using namespace theme;

AppContext& ctx() {
    static AppContext c;
    return c;
}

MainWindow::MainWindow() {
    ctx().window = this;
    setWindowTitle("RoadBoard — Virtual Audio Mixer");
    setWindowIcon(QIcon(":/icons/roadboard.svg"));
    buildUi();
    registerActions();
    setupTray();
    applyWindowOptions();

    connect(&timer_, &QTimer::timeout, this, &MainWindow::tick);
    timer_.start(33);
    connect(&fastTimer_, &QTimer::timeout, this, [] { remote().tick(); });
    fastTimer_.start(10);
    connect(&saveTimer_, &QTimer::timeout, this, &MainWindow::autosave);
    saveTimer_.start(3000);
    savedRev_ = params().revision();

    if (ctx().engine) {
        ctx().engine->onDevicesChanged = [this] {
            QMetaObject::invokeMethod(this, [this] { devicesChanged(); }, Qt::QueuedConnection);
        };
        ctx().engine->onStreamsChanged = [this] {
            QMetaObject::invokeMethod(this, [this] { streamsChanged(); }, Qt::QueuedConnection);
        };
    }
    params().addStrListener([this](const std::string& key, const std::string&) {
        if (key.find(".device") != std::string::npos)
            QMetaObject::invokeMethod(this, [this] { devicesChanged(); }, Qt::QueuedConnection);
    });
}

MainWindow::~MainWindow() { autosave(); }

void MainWindow::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.fillRect(rect(), bg);
}

// top bar container with a subtle gradient
class TopBar : public QWidget {
public:
    explicit TopBar(QWidget* parent) : QWidget(parent) { setFixedHeight(44); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QLinearGradient lg(r.topLeft(), r.bottomLeft());
        lg.setColorAt(0, QColor(0x2a, 0x30, 0x3a));
        lg.setColorAt(1, QColor(0x1d, 0x22, 0x29));
        g.setPen(QPen(border, 1));
        g.setBrush(lg);
        g.drawRoundedRect(r, 8, 8);
    }
};

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(6);

    // ------------------------------------------------ top bar
    auto* bar = new TopBar(central);
    auto* top = new QHBoxLayout(bar);
    top->setContentsMargins(12, 4, 8, 4);
    auto* logo = new Caption("ROADBOARD", title, 22, Qt::AlignLeft | Qt::AlignVCenter, bar);
    logo->setMinimumWidth(160);
    top->addWidget(logo);
    auto* sub = new Caption("virtual audio mixer", textDim, 11, Qt::AlignLeft | Qt::AlignVCenter, bar);
    top->addWidget(sub);
    top->addStretch(1);
    status_ = new Caption("", textDim, 11, Qt::AlignRight | Qt::AlignVCenter, bar);
    status_->setMinimumWidth(320);
    top->addWidget(status_);
    top->addSpacing(10);
    auto addTop = [&](ToggleButton* b, int w, const QString& tip) { b->setFixedSize(w, 28); b->setToolTip(tip); top->addWidget(b); return b; };
    auto* apps = addTop(new ToggleButton(nullptr, "APPS", accent, bar), 64, "Application Mixer: choose where every application plays");
    apps->setMomentary([this] { openAppMixer(this); });
    auto* fx = addTop(new ToggleButton(nullptr, "FX", accent, bar), 50, "Special FX: Reverb, MultiTap Delay / 5-band compressor");
    fx->setMomentary([this] { openFxWindow(this); });
    auto* streamer = addTop(new ToggleButton(nullptr, "STREAMER", accent, bar), 90, "Streamer View: every sub-mix on one page");
    streamer->setMomentary([this] { openStreamerView(this); });
    auto* link = addTop(new ToggleButton(params().link.enable, "RoadLink", accent, bar), 84,
                        "RoadLink network audio on/off (right click: configuration)");
    connect(link, &ToggleButton::rightClicked, this, [this] { openRoadLinkDialog(this); });
    auto* menuBtn = addTop(new ToggleButton(nullptr, "MENU", title, bar), 70, "Main menu");
    menuBtn->setMomentary([this] { showMenu(QCursor::pos()); });
    root->addWidget(bar);

    // ------------------------------------------------ four clearly separated groups
    auto* body = new QHBoxLayout;
    body->setSpacing(8);
    auto* hwIn = new GroupPanel("HARDWARE INPUTS", "1 – 5 · microphones, instruments, sound cards", hwInColor, central);
    auto* vIn = new GroupPanel("VIRTUAL INPUTS", "V1 – V3 · applications play here", virtInColor, central);
    auto* hwOut = new GroupPanel("HARDWARE OUTPUTS", "A1 – A5 · speakers, headphones", busA, central);
    auto* vOut = new GroupPanel("VIRTUAL OUTPUTS", "B1 – B3 · virtual mics", busB, central);
    for (int s = 0; s < kStrips; ++s) {
        StripBase* w = isHwStrip(s) ? static_cast<StripBase*>(new HwStripWidget(s, central))
                                    : static_cast<StripBase*>(new VirtStripWidget(s, central));
        w->setMinimumWidth(106);
        w->setMaximumWidth(150);
        strips_.push_back(w);
        (isHwStrip(s) ? hwIn : vIn)->row()->addWidget(w, 1);
    }
    for (int b = 0; b < kBuses; ++b) {
        auto* w = new BusStripWidget(b, central);
        w->setMinimumWidth(80);
        w->setMaximumWidth(120);
        buses_.push_back(w);
        (b < kHwBuses ? hwOut : vOut)->row()->addWidget(w, 1);
    }
    body->addWidget(hwIn, kHwStrips * 106);
    body->addWidget(vIn, kVirtStrips * 106);
    body->addWidget(hwOut, kHwBuses * 80);
    body->addWidget(vOut, kVirtBuses * 80);
    root->addLayout(body, 1);

    // ------------------------------------------------ recorder / player
    recorder_ = new RecorderBar(central);
    root->addWidget(recorder_);

    resize(1680, 940);
    setMinimumSize(1500, 840);
}

void MainWindow::streamsChanged() {
    for (auto* s : strips_)
        if (auto* v = qobject_cast<VirtStripWidget*>(s)) v->refreshApps();
}

void MainWindow::tick() {
    params().syncLinked();
    if (macro_) macro_->tick();    // before the meters are read & reset by the strips
    Syncable::syncAll();
    for (auto* s : strips_) s->tick();
    for (auto* b : buses_) b->tick();
    if (ctx().tape) { ctx().tape->poll(); recorder_->tick(); }
    if (ctx().midi) ctx().midi->sendFeedback();

    static int n = 0;
    if (++n % 15 == 0) streamsChanged();      // app volumes changed elsewhere
    if (n % 15 == 0 && ctx().engine) {
        QString s = QString::fromStdString(ctx().engine->statusText());
        if (ctx().mixer) s += QStringLiteral("   DSP %1%").arg(int(ctx().mixer->meters().dspLoad.load() * 100));
        if (Settings::options().lockGui) s += "   [LOCKED]";
        status_->setText(s);
        if (tray_) tray_->setToolTip("RoadBoard\n" + s);
    }
}

void MainWindow::devicesChanged() {
    for (auto* s : strips_) s->refreshDevices();
    for (auto* b : buses_) b->refreshDevices();
}

void MainWindow::autosave() {
    const uint32_t rev = params().revision();
    if (rev == savedRev_) return;
    savedRev_ = rev;
    Settings::save(Settings::defaultConfigFile());
}

void MainWindow::restartEngine() {
    if (!ctx().engine) return;
    ctx().engine->restart();
    devicesChanged();
}

void MainWindow::shutdown() {
    quitting_ = true;
    autosave();
    qApp->quit();
}

MacroButtonsWindow* MainWindow::macroButtons() {
    if (!macro_) macro_ = new MacroButtonsWindow(nullptr);
    return macro_;
}

void MainWindow::showWindow(int mode) {
    if (mode == 0) { hide(); return; }
    show();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();
}

void MainWindow::applyWindowOptions() {
    const auto& o = Settings::options();
    const bool visible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, o.alwaysOnTop);
    if (visible) show();
    if (tray_) tray_->setVisible(o.systemTray);
    for (QWidget* w : findChildren<QWidget*>()) w->setToolTipDuration(o.contextHelp ? -1 : 1);
}

void MainWindow::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    tray_ = new QSystemTrayIcon(QIcon(":/icons/roadboard.svg"), this);
    auto* menu = new QMenu(this);
    connect(menu->addAction("Show / Hide"), &QAction::triggered, this, [this] { showWindow(isVisible() ? 0 : 1); });
    connect(menu->addAction("Macro Buttons"), &QAction::triggered, this, [this] { macroButtons()->show(); macroButtons()->raise(); });
    connect(menu->addAction("Restart Audio Engine"), &QAction::triggered, this, &MainWindow::restartEngine);
    menu->addSeparator();
    connect(menu->addAction("Quit"), &QAction::triggered, this, &MainWindow::shutdown);
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger) showWindow(isVisible() ? 0 : 1);
    });
    tray_->setVisible(Settings::options().systemTray);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!quitting_ && tray_ && tray_->isVisible() && Settings::options().systemTray) {
        hide();
        e->ignore();
        return;
    }
    autosave();
    e->accept();
    qApp->quit();
}

void MainWindow::showMenu(QPoint pos) {
    auto& o = Settings::options();
    QMenu menu(this);
    connect(menu.addAction("Restart Audio Engine"), &QAction::triggered, this, &MainWindow::restartEngine);
    connect(menu.addAction("Eject Cassette (release audio file)"), &QAction::triggered, this, [] { ctx().tape->eject(); });
    menu.addSeparator();
    connect(menu.addAction("Load Settings..."), &QAction::triggered, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, "Load settings", Settings::configDir(), "RoadBoard settings (*.xml)");
        QString err;
        if (!f.isEmpty() && !Settings::load(f, &err)) QMessageBox::warning(this, "RoadBoard", err);
    });
    connect(menu.addAction("Save Settings..."), &QAction::triggered, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, "Save settings", Settings::configDir() + "/settings.xml", "RoadBoard settings (*.xml)");
        QString err;
        if (!f.isEmpty() && !Settings::save(f, &err)) QMessageBox::warning(this, "RoadBoard", err);
    });
    QMenu* startup = menu.addMenu("Load Settings on Startup");
    QAction* noFile = startup->addAction("Last state (default)");
    noFile->setCheckable(true);
    noFile->setChecked(o.startupFile.isEmpty());
    connect(noFile, &QAction::triggered, this, [] { Settings::options().startupFile.clear(); Settings::saveOptions(); });
    QAction* pick = startup->addAction(o.startupFile.isEmpty() ? QString("Select file...") : o.startupFile);
    pick->setCheckable(true);
    pick->setChecked(!o.startupFile.isEmpty());
    connect(pick, &QAction::triggered, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, "Settings file loaded on startup", Settings::configDir(), "RoadBoard settings (*.xml)");
        if (!f.isEmpty()) { Settings::options().startupFile = f; Settings::saveOptions(); }
    });
    connect(menu.addAction("Reset to Default Settings"), &QAction::triggered, this, [this] {
        if (QMessageBox::question(this, "RoadBoard", "Reset every parameter to its default value?") == QMessageBox::Yes) Settings::resetAll();
    });
    menu.addSeparator();
    auto check = [&](const QString& text, bool& opt, std::function<void()> after = {}) {
        QAction* a = menu.addAction(text);
        a->setCheckable(true);
        a->setChecked(opt);
        connect(a, &QAction::toggled, this, [&opt, after](bool on) { opt = on; Settings::saveOptions(); if (after) after(); });
    };
    check("System Tray (run in background)", o.systemTray, [this] { applyWindowOptions(); });
    check("Run on System Startup", o.runAtStartup, [] { Settings::applyAutostart(Settings::options().runAtStartup); });
    check("Show Window on Launch", o.showOnLaunch);
    check("Always on Top", o.alwaysOnTop, [this] { applyWindowOptions(); });
    check("Lock GUI (prevent changes)", o.lockGui);
    menu.addSeparator();
    connect(menu.addAction("Macro Buttons..."), &QAction::triggered, this, [this] { macroButtons()->show(); macroButtons()->raise(); });
    connect(menu.addAction("Preset Scenes..."), &QAction::triggered, this, [this] { openPresets(this); });
    check("Run Macro Buttons on Launch", o.startMacroButtons);
    check("Show Preset Scenes on Startup", o.showPresetsOnStartup);
    check("Limit MIDI / Remote Gain to 0 dB", o.limitRemoteGain);
    check("Show Contextual Help (tooltips)", o.contextHelp, [this] { applyWindowOptions(); });
    menu.addSeparator();
    connect(menu.addAction("Application Mixer..."), &QAction::triggered, this, [this] { openAppMixer(this); });
    connect(menu.addAction("Special FX (Reverb / Delay / Multiband)..."), &QAction::triggered, this, [this] { openFxWindow(this); });
    connect(menu.addAction("Streamer View..."), &QAction::triggered, this, [this] { openStreamerView(this); });
    connect(menu.addAction("System Settings / Options..."), &QAction::triggered, this, [this] { openSystemSettings(this); });
    connect(menu.addAction("RoadLink (network audio)..."), &QAction::triggered, this, [this] { openRoadLinkDialog(this); });
    connect(menu.addAction("M.I.D.I. Mapping..."), &QAction::triggered, this, [this] { openMidiDialog(this); });
    connect(menu.addAction("Recorder Options..."), &QAction::triggered, this, [this] { openTapeOptions(this); });
    QMenu* defaults = menu.addMenu("System Default Devices");
    for (int v = 0; v < kVirtStrips; ++v)
        connect(defaults->addAction(QString::fromStdString("Playback: " + Engine::virtualInputDescription(v))), &QAction::triggered, this,
                [v] { ctx().engine->setSystemDefault(v); });
    for (int v = 0; v < kVirtBuses; ++v)
        connect(defaults->addAction(QString::fromStdString("Recording: " + Engine::virtualOutputDescription(v))), &QAction::triggered, this,
                [v] { ctx().engine->setSystemDefaultSource(v); });
    QMenu* tools = menu.addMenu("Audio Tools");
    for (const char* app : {"pavucontrol", "qpwgraph", "helvum", "coppwr", "easyeffects"}) {
        if (QStandardPaths::findExecutable(app).isEmpty()) continue;
        connect(tools->addAction(app), &QAction::triggered, this, [app] { QProcess::startDetached(app, {}); });
    }
    if (tools->isEmpty()) tools->addAction("(install pavucontrol / qpwgraph for more tools)")->setEnabled(false);
    menu.addSeparator();
    connect(menu.addAction("About / License..."), &QAction::triggered, this, [this] { openAbout(this); });
    connect(menu.addAction("Shut Down RoadBoard"), &QAction::triggered, this, &MainWindow::shutdown);
    menu.exec(pos);
}

void MainWindow::registerActions() {
    RemoteScript& r = remote();
    auto ui = [this](std::function<void()> f) { QMetaObject::invokeMethod(this, std::move(f), Qt::QueuedConnection); };
    r.registerAction("command.shutdown", [this, ui](const std::string&, float, int) { ui([this] { shutdown(); }); });
    r.registerAction("command.show", [this, ui](const std::string&, float v, int) { ui([this, v] { showWindow(int(v)); }); });
    r.registerAction("command.restart", [this, ui](const std::string&, float, int) { ui([this] { restartEngine(); }); });
    r.registerAction("command.eject", [ui](const std::string&, float, int) { ui([] { ctx().tape->eject(); }); });
    r.registerAction("command.reset", [ui](const std::string&, float, int) { ui([] { Settings::resetAll(); }); });
    r.registerAction("command.save", [ui](const std::string& a, float, int) { ui([a] { Settings::save(QString::fromStdString(a)); }); });
    r.registerAction("command.load", [ui](const std::string& a, float, int) { ui([a] { Settings::load(QString::fromStdString(a)); }); });
    r.registerAction("command.lock", [](const std::string&, float v, int) { Settings::options().lockGui = v >= 0.5f; });
    r.registerAction("command.savebuseq[]", [ui](const std::string& a, float, int i) {
        ui([a, i] { if (i >= 0 && i < kBuses) Settings::saveKV(QString::fromStdString(a), "RoadBoardBusEq", Settings::exportBusEq(i)); });
    });
    r.registerAction("command.loadbuseq[]", [ui](const std::string& a, float, int i) {
        ui([a, i] { Settings::KV kv; if (i >= 0 && i < kBuses && Settings::loadKV(QString::fromStdString(a), "RoadBoardBusEq", &kv)) Settings::importBusEq(i, kv); });
    });
    r.registerAction("command.preset[].recall", [ui](const std::string&, float, int i) { ui([i] { Settings::recallPreset(i); }); });
    r.registerAction("command.preset[].store", [ui](const std::string& a, float, int i) { ui([a, i] { Settings::storePreset(i, QString::fromStdString(a), {}); }); });
    r.registerAction("command.recallpreset", [ui](const std::string& a, float, int) { ui([a] { Settings::recallPresetByName(QString::fromStdString(a)); }); });
    r.registerAction("command.updatepreset", [ui](const std::string& a, float, int) {
        ui([a] {
            int idx = Settings::currentPreset;
            for (auto& p : Settings::presets()) if (p.used && p.name == QString::fromStdString(a)) idx = p.index;
            if (idx >= 0) Settings::storePreset(idx, a.empty() ? QString() : QString::fromStdString(a), {});
        });
    });
    r.registerAction("command.refreshmidi", [](const std::string&, float, int) { if (ctx().midi) ctx().midi->sendFeedback(true); });
    r.registerAction("command.button[].state", [this, ui](const std::string&, float v, int i) { ui([this, i, v] { macroButtons()->setButtonState(i, v >= 0.5f, true); }); });
    r.registerAction("command.button[].stateonly", [this, ui](const std::string&, float v, int i) { ui([this, i, v] { macroButtons()->setButtonState(i, v >= 0.5f, false); }); });
    r.registerAction("command.button[].trigger", [this, ui](const std::string&, float v, int i) { ui([this, i, v] { macroButtons()->setTriggerEnabled(i, v >= 0.5f); }); });
    r.registerAction("command.button[].color", [this, ui](const std::string&, float v, int i) { ui([this, i, v] { macroButtons()->setButtonColor(i, int(v)); }); });

    // applications on virtual input strips: Strip(i).App(k).Gain / .Mute, Strip(i).AppGain = ("name", g)
    auto appsOf = [](int strip) {
        std::vector<AppStream> v;
        if (!ctx().engine || strip < kHwStrips || strip >= kStrips) return v;
        const std::string target = Engine::virtualInputName(strip - kHwStrips);
        for (const AppStream& s : ctx().engine->streams()) if (s.output && s.target == target) v.push_back(s);
        return v;
    };
    r.registerAction("strip[].app[].gain", [appsOf](const std::string&, float v, int idx) {
        const auto apps = appsOf(idx / 1000);
        const size_t k = size_t(idx % 1000);
        if (k < apps.size()) ctx().engine->setNodeVolume(apps[k].id, std::pow(std::clamp(v, 0.0f, 1.0f), 3.0f));
    });
    r.registerAction("strip[].app[].mute", [appsOf](const std::string&, float v, int idx) {
        const auto apps = appsOf(idx / 1000);
        const size_t k = size_t(idx % 1000);
        if (k < apps.size()) ctx().engine->setNodeMute(apps[k].id, v >= 0.5f);
    });
    auto byName = [appsOf](const std::string& arg, int strip, bool mute) {
        // ("AppName", value)
        QString a = QString::fromStdString(arg).trimmed();
        if (a.startsWith('(')) a = a.mid(1);
        if (a.endsWith(')')) a.chop(1);
        const int comma = a.lastIndexOf(',');
        if (comma < 0) return;
        const QString name = a.left(comma).trimmed().remove('"');
        const float value = a.mid(comma + 1).trimmed().toFloat();
        for (const AppStream& s : appsOf(strip)) {
            if (!QString::fromStdString(s.appName).startsWith(name, Qt::CaseInsensitive)) continue;
            if (mute) ctx().engine->setNodeMute(s.id, value >= 0.5f);
            else ctx().engine->setNodeVolume(s.id, std::pow(std::clamp(value, 0.0f, 1.0f), 3.0f));
        }
    };
    r.registerAction("strip[].appgain", [byName](const std::string& a, float, int i) { byName(a, i, false); });
    r.registerAction("strip[].appmute", [byName](const std::string& a, float, int i) { byName(a, i, true); });
    // Command.RouteApp = ("app name", target): target = 1 / 2 / 3 (virtual inputs), a device
    // node name or description, or "" for the system default. Recording apps use "in:" prefix on the name.
    r.registerAction("command.routeapp", [ui](const std::string& arg, float, int) {
        ui([arg] {
            QString a = QString::fromStdString(arg).trimmed();
            if (a.startsWith('(')) a = a.mid(1);
            if (a.endsWith(')')) a.chop(1);
            const int comma = a.lastIndexOf(',');
            if (comma < 0 || !ctx().engine) return;
            QString name = a.left(comma).trimmed().remove('"');
            const QString target = a.mid(comma + 1).trimmed().remove('"');
            bool output = true;
            if (name.startsWith("in:")) { output = false; name = name.mid(3); }
            std::string node = target.toStdString();
            bool ok = false;
            const int v = target.toInt(&ok);
            if (ok && v >= 1 && v <= kVirtStrips) node = Engine::virtualInputName(v - 1);
            else if (!target.isEmpty()) {
                for (const AudioDevice& d : output ? ctx().engine->allSinks() : ctx().engine->allSources())
                    if (QString::fromStdString(d.description).compare(target, Qt::CaseInsensitive) == 0) node = d.name;
            }
            for (const AppStream& s : ctx().engine->streams())
                if (s.output == output && QString::fromStdString(s.appName).startsWith(name, Qt::CaseInsensitive))
                    ctx().engine->moveStream(s.id, node);
        });
    });
    r.registerAction("strip[].pitch.recallpreset", [ui](const std::string&, float v, int i) { ui([i, v] { recallVoicePreset(i, int(v)); }); });

    TapeDeck* t = ctx().tape;
    r.registerAction("recorder.play", [t](const std::string&, float v, int) { if (v >= 0.5f) t->play(); else t->stop(); });
    r.registerAction("recorder.stop", [t](const std::string&, float v, int) { if (v >= 0.5f) t->stop(); });
    r.registerAction("recorder.replay", [t](const std::string&, float, int) { t->replay(); });
    r.registerAction("recorder.record", [t](const std::string&, float v, int) { t->setRecord(v >= 0.5f); });
    r.registerAction("recorder.pause", [t](const std::string&, float v, int) { if ((v >= 0.5f) != t->paused()) t->pause(); });
    r.registerAction("recorder.ff", [t](const std::string&, float v, int) { if (v >= 0.5f) t->skip(10); });
    r.registerAction("recorder.rew", [t](const std::string&, float v, int) { if (v >= 0.5f) t->skip(-10); });
    r.registerAction("recorder.goto", [t](const std::string& a, float v, int) {
        const QStringList parts = QString::fromStdString(a).split(':');
        double sec = 0;
        if (parts.size() == 3) sec = parts[0].toDouble() * 3600 + parts[1].toDouble() * 60 + parts[2].toDouble();
        else sec = v;
        t->seek(sec);
    });
    r.registerAction("recorder.load", [t](const std::string& a, float, int) { t->load(a); });
    r.registerAction("recorder.eject", [t](const std::string&, float, int) { t->eject(); });
}

} // namespace road
