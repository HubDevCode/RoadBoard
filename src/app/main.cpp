// RoadBoard — virtual audio mixer for Linux (PipeWire).
#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QMessageBox>
#include <QPalette>
#include <QRegularExpression>
#include <cmath>
#include <QStyleFactory>
#include <QTimer>
#include <clocale>
#include <cstdio>

#include "core/RemoteScript.h"
#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/Midi.h"
#include "engine/Mixer.h"
#include "engine/RemoteServer.h"
#include "engine/TapeDeck.h"
#include "engine/RoadLink.h"
#include "gui/AppContext.h"
#include "gui/AppMixer.h"
#include "gui/Dialogs.h"
#include "gui/Detail.h"
#include "gui/EqDialog.h"
#include "gui/FxWindow.h"
#include "gui/StreamerView.h"
#include "gui/MacroButtons.h"
#include "gui/MainWindow.h"
#include "gui/Theme.h"

using namespace road;

static void applyDarkTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, theme::bg);
    p.setColor(QPalette::WindowText, theme::text);
    p.setColor(QPalette::Base, theme::bgDeep);
    p.setColor(QPalette::AlternateBase, theme::panel);
    p.setColor(QPalette::ToolTipBase, theme::panelHi);
    p.setColor(QPalette::ToolTipText, theme::text);
    p.setColor(QPalette::Text, theme::text);
    p.setColor(QPalette::Button, theme::panel);
    p.setColor(QPalette::ButtonText, theme::text);
    p.setColor(QPalette::BrightText, theme::title);
    p.setColor(QPalette::Highlight, theme::accent.darker(140));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, theme::accent);
    p.setColor(QPalette::PlaceholderText, theme::textDim);
    p.setColor(QPalette::Disabled, QPalette::Text, theme::textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, theme::textDim);
    app.setPalette(p);
    app.setStyleSheet(
        "QToolTip { color: #c9d2dc; background: #313843; border: 1px solid #3c4552; padding: 4px; }"
        "QGroupBox { border: 1px solid #3c4552; border-radius: 5px; margin-top: 14px; padding-top: 6px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #f5c518; font-weight: bold; }"
        "QMenu { background: #272d36; border: 1px solid #3c4552; }"
        "QMenu::item:selected { background: #2a5a7a; }"
        "QMenu::section { color: #f5c518; font-weight: bold; padding: 4px; }"
        "QPushButton:checked { background: #3fc1ff; color: #12151a; font-weight: bold; }");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // numbers in scripts / files always use '.' whatever the user's locale is
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setApplicationName("roadboard");
    QApplication::setApplicationDisplayName("RoadBoard");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setDesktopFileName("io.github.roadboard.RoadBoard");
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser cli;
    cli.setApplicationDescription("RoadBoard — virtual audio mixer for Linux (8 inputs, 8 buses)");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption cmd({"c", "cmd"}, "Send a request script to the running instance, e.g. \"Strip(0).Mute=1;\"", "script");
    QCommandLineOption get({"g", "get"}, "Read a parameter from the running instance, e.g. \"Strip(0).Gain\"", "param");
    QCommandLineOption minimized({"m", "minimized"}, "Start hidden in the system tray.");
    QCommandLineOption config("config", "Load this settings file on startup.", "file");
    QCommandLineOption shot("screenshot", "Render the main window to a PNG file and quit (testing).", "png");
    shot.setFlags(QCommandLineOption::HiddenFromHelp);
    cli.addOption(shot);
    cli.addOption(cmd);
    cli.addOption(get);
    cli.addOption(minimized);
    cli.addOption(config);
    cli.process(app);

    // ---------------------------------------------------- client mode
    if (cli.isSet(cmd) || cli.isSet(get)) {
        bool connected = false;
        const QString reply = cli.isSet(get) ? RemoteServer::request("?" + cli.value(get), 2000, &connected)
                                             : RemoteServer::request(cli.value(cmd), 2000, &connected);
        if (!connected) { std::fprintf(stderr, "roadboard is not running\n"); return 2; }
        std::printf("%s\n", reply.toUtf8().constData());
        return reply.startsWith("ERR") ? 1 : 0;
    }
    // ---------------------------------------------------- single instance
    {
        bool connected = false;
        RemoteServer::request("Command.Show=1", 500, &connected);
        if (connected) return 0;
    }

    applyDarkTheme(app);
    Settings::loadOptions();
    QString startFile = cli.isSet(config) ? cli.value(config) : Settings::options().startupFile;
    if (startFile.isEmpty() || !QFileInfo::exists(startFile)) startFile = Settings::defaultConfigFile();
    if (QFileInfo::exists(startFile)) Settings::load(startFile);

    // ---------------------------------------------------- engine
    Mixer mixer;
    TapeDeck tape;
    RoadLinkEngine link;
    MidiEngine midi;
    mixer.setTapeDeck(&tape);
    mixer.setLink(&link);
    Engine engine(mixer);
    ctx().mixer = &mixer;
    ctx().tape = &tape;
    ctx().link = &link;
    ctx().midi = &midi;
    ctx().engine = &engine;

    std::string err;
    // ROADBOARD_NO_ENGINE=1: GUI only (tests, screenshots) without touching the audio graph
    const bool noEngine = qEnvironmentVariableIsSet("ROADBOARD_NO_ENGINE");
    const bool engineOk = noEngine || engine.start(&err);
    link.start();
    if (midi.start()) midi.load((Settings::configDir() + "/midi.map").toStdString());
    link.onText = [](const std::string& text, const std::string&) { remote().execute(text); };

    MainWindow window;
    loadAppRules();
    RemoteServer server;
    // "?level.strip[i]" / "?level.strip[i].post" / "?level.bus[i]": current peak per channel in dBFS
    RemoteServer::extraGetter = [&mixer](const QString& key, QString* out) {
        static const QRegularExpression re(R"(^level\.(strip|bus)[\[\(](\d+)[\]\)](\.post)?$)", QRegularExpression::CaseInsensitiveOption);
        const auto m = re.match(key.trimmed());
        if (!m.hasMatch()) return false;
        const bool strip = m.captured(1).toLower() == "strip";
        const int i = m.captured(2).toInt();
        if (i < 0 || i >= (strip ? kStrips : kBuses)) return false;
        Meters& mt = mixer.meters();
        QStringList v;
        const int nch = strip ? stripChannels(i) : kBusCh;
        for (int c = 0; c < nch; ++c) {
            const float l = strip ? (m.captured(3).isEmpty() ? mt.stripPre[i][c] : mt.stripPost[i][c]).load() : mt.bus[i][c].load();
            v << (l > 1e-6f ? QString::number(20 * std::log10(l), 'f', 1) : QStringLiteral("-inf"));
        }
        *out = v.join(' ');
        return true;
    };
    server.listen();

    tape.onEvent = [&window](const std::string& e) {
        QMetaObject::invokeMethod(&window, [&window, e] {
            if (auto* m = window.macroButtons()) m->onTapeEvent(QString::fromStdString(e));
        }, Qt::QueuedConnection);
    };

    const auto& opt = Settings::options();
    const bool hidden = cli.isSet(minimized) || (!opt.showOnLaunch && opt.systemTray);
    if (!hidden) window.show();
    if (opt.startMacroButtons) { window.macroButtons()->show(); }
    window.macroButtons()->runInitScripts();
    if (opt.showPresetsOnStartup) openPresets(&window);
    if (!engineOk) {
        QMessageBox::critical(&window, "RoadBoard",
            "Cannot start the audio engine:\n" + QString::fromStdString(err) +
            "\n\nRoadBoard needs PipeWire (pipewire + pipewire-pulse or wireplumber). "
            "Install/start it and use Menu → Restart Audio Engine.");
    }

    if (cli.isSet(shot)) {
        window.show();
        QTimer::singleShot(1200, &window, [&window, path = cli.value(shot)] {
            window.grab().save(path);
            if (qEnvironmentVariableIsSet("ROADBOARD_SHOT_DIALOGS")) {
                const QString base = path.left(path.size() - 4);
                openSystemSettings(&window);
                openRoadLinkDialog(&window);
                openMidiDialog(&window);
                openTapeOptions(&window);
                openPresets(&window);
                openBusEqDialog(&window, 0);
                openFxWindow(&window);
                openAppMixer(&window);
                openCompGateDetail(&window, 0);
                openDenoiserDetail(&window, 0);
                openStreamerView(&window);
                window.macroButtons()->show();
                QTimer::singleShot(600, &window, [&window, base] {
                    int i = 0;
                    for (QWidget* w : QApplication::topLevelWidgets())
                        if (w->isVisible() && w != &window)
                            w->grab().save(base + "-" + QString::number(i++) + "-" + w->windowTitle().section(QChar(0x2014), -1).trimmed().replace(' ', '_').replace('/', '_') + ".png");
                    window.shutdown();
                });
                return;
            }
            window.shutdown();
        });
    }

    const int rc = app.exec();
    Settings::save(Settings::defaultConfigFile());
    midi.save((Settings::configDir() + "/midi.map").toStdString());
    engine.stop();
    link.stop();
    midi.stop();
    return rc;
}
