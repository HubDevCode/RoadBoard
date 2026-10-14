// Persistence: full configuration XML, strip / bus EQ partial files,
// 64 preset scenes, application options (tray, autostart...).
#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace road {

class Settings {
public:
    static QString configDir();
    static QString defaultConfigFile();          // auto-saved state
    static QString presetDir();

    static bool save(const QString& path, QString* error = nullptr);
    static bool load(const QString& path, QString* error = nullptr);
    static void resetAll();

    // strip settings (all except fader, solo/mute and bus assignment)
    using KV = QMap<QString, QString>;
    static KV exportStrip(int strip, const QString& section = QString());
    static void importStrip(int strip, const KV& kv);
    static void resetStrip(int strip, const QString& section = QString());
    static QStringList stripSections(int strip);       // "All", "Color Panel", ...
    static bool saveKV(const QString& path, const QString& root, const KV& kv);
    static bool loadKV(const QString& path, const QString& root, KV* kv);

    // bus EQ (current A/B memory, 8 channels)
    static KV exportBusEq(int bus);
    static void importBusEq(int bus, const KV& kv);
    static void flatBusEq(int bus, int channel /* -1 = all */);

    // preset scenes
    struct Preset { int index = 0; QString name, comment; bool used = false; };
    static QList<Preset> presets();
    static bool storePreset(int index, const QString& name, const QString& comment);
    static bool recallPreset(int index);
    static bool recallPresetByName(const QString& name);
    static void deletePreset(int index);
    static int currentPreset;

    // application options
    struct AppOptions {
        bool systemTray = true, runAtStartup = false, showOnLaunch = true, alwaysOnTop = false,
             lockGui = false, contextHelp = true, autoRestartOnA1Lost = true, restartOnAnyLost = false,
             startMacroButtons = false, limitRemoteGain = false, showPresetsOnStartup = false;
        QString startupFile;
        int zoom = 100;
    };
    static AppOptions& options();
    static void saveOptions();
    static void loadOptions();
    static void applyAutostart(bool enable);
};

} // namespace road
