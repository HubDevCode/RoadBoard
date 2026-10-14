#include "core/Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "core/Params.h"

namespace road {

int Settings::currentPreset = -1;

namespace {

bool isVolatile(const std::string& k) {
    static const char* const skip[] = {"recorder.play", "recorder.stop", "recorder.record", "recorder.pause",
                                       "recorder.ff", "recorder.rew"};
    for (const char* s : skip) if (k == s) return true;
    if (k.rfind("roadlink.instream[", 0) == 0) {
        const auto dot = k.rfind('.');
        const std::string leaf = k.substr(dot + 1);
        if (leaf == "sr" || leaf == "channel" || leaf == "bit") return true;
    }
    return false;
}

QString stripPrefix(int s) { return QStringLiteral("strip[%1].").arg(s); }

// leaf prefixes of each strip section ("comp" matches comp, comp.ratio, ...)
QStringList sectionPrefixes(int strip, const QString& section) {
    const bool hw = isHwStrip(strip);
    QMap<QString, QStringList> m;
    if (hw) {
        m["Color Panel"] = {"color_x", "color_y"};
        m["Modulation"] = {"fx_x", "fx_y"};
        m["3D Panel"] = {"pan_x", "pan_y"};
        m["Equalizer"] = {"eq"};
        m["Compressor"] = {"comp"};
        m["Gate"] = {"gate"};
        m["Denoiser"] = {"denoiser"};
        m["Voice Modeler"] = {"pitch"};
        m["FX Sends"] = {"reverb", "delay", "fx1", "fx2", "postreverb", "postdelay", "postfx1", "postfx2"};
        m["Mono"] = {"mono"};
    } else {
        m["Equalizer"] = {"eqgain1", "eqgain2", "eqgain3"};
        m["Pan Pot"] = {"pan_x", "pan_y", "mode"};
        m["M.C"] = {"mc"};
        m["Karaoke"] = {"karaoke"};
    }
    m["Limiter"] = {"limit"};
    if (section.isEmpty() || section == "All") {
        QStringList all;
        for (auto& v : m) all += v;
        return all;
    }
    return m.value(section);
}

// every parameter key (relative to "strip[i].") of a section
QStringList sectionKeys(int strip, const QString& section) {
    const QStringList prefixes = sectionPrefixes(strip, section);
    const std::string base = "strip[" + std::to_string(strip) + "].";
    QStringList keys;
    for (auto& p : params().all()) {
        if (p->key.rfind(base, 0) != 0) continue;
        const QString leaf = QString::fromStdString(p->key.substr(base.size()));
        for (const QString& pre : prefixes)
            if (leaf == pre || leaf.startsWith(pre + ".")) { keys << leaf; break; }
    }
    return keys;
}

} // namespace

QString Settings::configDir() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/roadboard";
    QDir().mkpath(d);
    return d;
}
QString Settings::defaultConfigFile() { return configDir() + "/current.xml"; }
QString Settings::presetDir() {
    QString d = configDir() + "/presets";
    QDir().mkpath(d);
    return d;
}

bool Settings::save(const QString& path, QString* error) {
    QFile f(path + ".tmp");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    QXmlStreamWriter w(&f);
    w.setAutoFormatting(true);
    w.writeStartDocument();
    w.writeStartElement("RoadBoardSettings");
    w.writeAttribute("version", "1");
    w.writeAttribute("saved", QDateTime::currentDateTime().toString(Qt::ISODate));
    ParamStore& P = params();
    for (auto& p : P.all()) {
        if (isVolatile(p->key)) continue;
        w.writeStartElement("p");
        w.writeAttribute("k", QString::fromStdString(p->key));
        w.writeAttribute("v", QString::number(double(p->get()), 'g', 7));
        w.writeEndElement();
    }
    for (const std::string& k : P.strKeys()) {
        w.writeStartElement("s");
        w.writeAttribute("k", QString::fromStdString(k));
        w.writeAttribute("v", QString::fromStdString(P.str(k)));
        w.writeEndElement();
    }
    w.writeEndElement();
    w.writeEndDocument();
    f.close();
    QFile::remove(path);
    if (!QFile::rename(path + ".tmp", path)) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

bool Settings::load(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QXmlStreamReader r(&f);
    if (!r.readNextStartElement() || r.name() != QLatin1String("RoadBoardSettings")) {
        if (error) *error = "not a RoadBoard settings file";
        return false;
    }
    ParamStore& P = params();
    int raioBefore[kVirtStrips + kVirtBuses];
    for (int v = 0; v < kVirtStrips; ++v) raioBefore[v] = P.opt.vinCh[v]->i();
    for (int v = 0; v < kVirtBuses; ++v) raioBefore[kVirtStrips + v] = P.opt.voutCh[v]->i();
    // values not present in the file keep their defaults
    for (auto& p : P.all()) if (!isVolatile(p->key)) p->reset();
    while (r.readNextStartElement()) {
        const std::string k = r.attributes().value("k").toString().toStdString();
        const QString v = r.attributes().value("v").toString();
        if (r.name() == QLatin1String("p")) {
            if (Param* p = P.find(k)) if (!isVolatile(k)) p->set(v.toFloat());
        } else if (r.name() == QLatin1String("s")) {
            if (P.hasStr(k)) P.setStr(k, v.toStdString());
        }
        r.skipCurrentElement();
    }
    if (r.hasError()) {
        if (error) *error = r.errorString();
        return false;
    }
    bool raioChanged = false;
    for (int v = 0; v < kVirtStrips; ++v) raioChanged |= raioBefore[v] != P.opt.vinCh[v]->i();
    for (int v = 0; v < kVirtBuses; ++v) raioChanged |= raioBefore[kVirtStrips + v] != P.opt.voutCh[v]->i();
    if (raioChanged) P.routingRevision.fetch_add(1);
    return true;
}

void Settings::resetAll() {
    ParamStore& P = params();
    for (auto& p : P.all()) if (!isVolatile(p->key)) p->reset();
    for (int s = 0; s < kStrips; ++s) P.setStr("strip[" + std::to_string(s) + "].label", "");
    for (int b = 0; b < kBuses; ++b) P.setStr("bus[" + std::to_string(b) + "].label", "");
}

QStringList Settings::stripSections(int strip) {
    if (isHwStrip(strip)) return {"All", "Color Panel", "Modulation", "3D Panel", "Equalizer", "Compressor", "Gate", "Denoiser",
                                  "Voice Modeler", "FX Sends", "Limiter", "Mono"};
    return {"All", "Equalizer", "Pan Pot", "M.C", "Karaoke", "Limiter"};
}

Settings::KV Settings::exportStrip(int strip, const QString& section) {
    KV kv;
    for (const QString& k : sectionKeys(strip, section)) {
        if (Param* p = params().find((stripPrefix(strip) + k).toStdString())) kv[k] = QString::number(double(p->get()), 'g', 7);
    }
    if (section.isEmpty() || section == "All" || section == "Limiter") {
        kv["limtype"] = QString::number(params().strip[strip].limType->get());
    }
    return kv;
}

void Settings::importStrip(int strip, const KV& kv) {
    for (auto it = kv.begin(); it != kv.end(); ++it) {
        if (it.key() == "limtype") { params().strip[strip].limType->set(it.value().toFloat()); continue; }
        if (Param* p = params().find((stripPrefix(strip) + it.key()).toStdString())) p->set(it.value().toFloat());
    }
}

void Settings::resetStrip(int strip, const QString& section) {
    for (const QString& k : sectionKeys(strip, section))
        if (Param* p = params().find((stripPrefix(strip) + k).toStdString())) p->reset();
}

bool Settings::saveKV(const QString& path, const QString& root, const KV& kv) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QXmlStreamWriter w(&f);
    w.setAutoFormatting(true);
    w.writeStartDocument();
    w.writeStartElement(root);
    for (auto it = kv.begin(); it != kv.end(); ++it) {
        w.writeStartElement("p");
        w.writeAttribute("k", it.key());
        w.writeAttribute("v", it.value());
        w.writeEndElement();
    }
    w.writeEndElement();
    w.writeEndDocument();
    return true;
}

bool Settings::loadKV(const QString& path, const QString& root, KV* kv) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QXmlStreamReader r(&f);
    if (!r.readNextStartElement() || r.name() != root) return false;
    while (r.readNextStartElement()) {
        (*kv)[r.attributes().value("k").toString()] = r.attributes().value("v").toString();
        r.skipCurrentElement();
    }
    return !r.hasError();
}

Settings::KV Settings::exportBusEq(int bus) {
    KV kv;
    ParamStore& P = params();
    const int ab = P.bus[bus].eqAB->i();
    for (int c = 0; c < kBusCh; ++c) {
        const auto& ch = P.bus[bus].eq[ab][c];
        const QString pre = QStringLiteral("channel[%1].").arg(c);
        for (int k = 0; k < kEqCells; ++k) {
            const QString cp = pre + QStringLiteral("cell[%1].").arg(k);
            kv[cp + "on"] = QString::number(ch.cell[k].on->get());
            kv[cp + "type"] = QString::number(ch.cell[k].type->get());
            kv[cp + "f"] = QString::number(double(ch.cell[k].f->get()), 'g', 7);
            kv[cp + "gain"] = QString::number(double(ch.cell[k].gain->get()), 'g', 7);
            kv[cp + "q"] = QString::number(double(ch.cell[k].q->get()), 'g', 7);
        }
        kv[pre + "trim"] = QString::number(double(ch.trim->get()), 'g', 7);
        kv[pre + "delay"] = QString::number(double(ch.delay->get()), 'g', 7);
    }
    return kv;
}

void Settings::importBusEq(int bus, const KV& kv) {
    ParamStore& P = params();
    const int ab = P.bus[bus].eqAB->i();
    const std::string pre = "bus[" + std::to_string(bus) + "].eq.ab[" + std::to_string(ab) + "].";
    for (auto it = kv.begin(); it != kv.end(); ++it)
        if (Param* p = P.find(pre + it.key().toStdString())) p->set(it.value().toFloat());
}

void Settings::flatBusEq(int bus, int channel) {
    ParamStore& P = params();
    const int ab = P.bus[bus].eqAB->i();
    for (int c = 0; c < kBusCh; ++c) {
        if (channel >= 0 && c != channel) continue;
        auto& ch = P.bus[bus].eq[ab][c];
        for (auto& cell : ch.cell) { cell.on->reset(); cell.type->reset(); cell.f->reset(); cell.gain->reset(); cell.q->reset(); }
        ch.trim->reset(); ch.delay->reset();
    }
}

// ------------------------------------------------------------------ presets

static QString presetPath(int i) { return Settings::presetDir() + QStringLiteral("/preset_%1.xml").arg(i, 2, 10, QChar('0')); }

QList<Settings::Preset> Settings::presets() {
    QList<Preset> list;
    QSettings meta(presetDir() + "/presets.ini", QSettings::IniFormat);
    for (int i = 0; i < 64; ++i) {
        Preset p;
        p.index = i;
        p.used = QFileInfo::exists(presetPath(i));
        p.name = meta.value(QStringLiteral("p%1/name").arg(i)).toString();
        p.comment = meta.value(QStringLiteral("p%1/comment").arg(i)).toString();
        list.push_back(p);
    }
    return list;
}

bool Settings::storePreset(int index, const QString& name, const QString& comment) {
    if (index < 0 || index >= 64) return false;
    if (!save(presetPath(index))) return false;
    QSettings meta(presetDir() + "/presets.ini", QSettings::IniFormat);
    meta.setValue(QStringLiteral("p%1/name").arg(index), name.isEmpty() ? QStringLiteral("Preset %1").arg(index + 1) : name);
    meta.setValue(QStringLiteral("p%1/comment").arg(index),
                  comment.isEmpty() ? QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm") : comment);
    currentPreset = index;
    return true;
}

bool Settings::recallPreset(int index) {
    if (index < 0 || index >= 64 || !QFileInfo::exists(presetPath(index))) return false;
    if (!load(presetPath(index))) return false;
    currentPreset = index;
    return true;
}

bool Settings::recallPresetByName(const QString& name) {
    if (name.isEmpty()) return recallPreset(currentPreset);
    for (const Preset& p : presets())
        if (p.used && p.name.compare(name, Qt::CaseInsensitive) == 0) return recallPreset(p.index);
    return false;
}

void Settings::deletePreset(int index) {
    QFile::remove(presetPath(index));
    QSettings meta(presetDir() + "/presets.ini", QSettings::IniFormat);
    meta.remove(QStringLiteral("p%1").arg(index));
}

// ------------------------------------------------------------------ options

Settings::AppOptions& Settings::options() {
    static AppOptions o;
    return o;
}

void Settings::loadOptions() {
    QSettings s(configDir() + "/options.ini", QSettings::IniFormat);
    AppOptions& o = options();
    o.systemTray = s.value("systemTray", o.systemTray).toBool();
    o.runAtStartup = s.value("runAtStartup", o.runAtStartup).toBool();
    o.showOnLaunch = s.value("showOnLaunch", o.showOnLaunch).toBool();
    o.alwaysOnTop = s.value("alwaysOnTop", o.alwaysOnTop).toBool();
    o.lockGui = s.value("lockGui", o.lockGui).toBool();
    o.contextHelp = s.value("contextHelp", o.contextHelp).toBool();
    o.autoRestartOnA1Lost = s.value("autoRestartOnA1Lost", o.autoRestartOnA1Lost).toBool();
    o.restartOnAnyLost = s.value("restartOnAnyLost", o.restartOnAnyLost).toBool();
    o.startMacroButtons = s.value("startMacroButtons", o.startMacroButtons).toBool();
    o.limitRemoteGain = s.value("limitRemoteGain", o.limitRemoteGain).toBool();
    o.showPresetsOnStartup = s.value("showPresetsOnStartup", o.showPresetsOnStartup).toBool();
    o.startupFile = s.value("startupFile").toString();
    o.zoom = s.value("zoom", o.zoom).toInt();
}

void Settings::saveOptions() {
    QSettings s(configDir() + "/options.ini", QSettings::IniFormat);
    const AppOptions& o = options();
    s.setValue("systemTray", o.systemTray);
    s.setValue("runAtStartup", o.runAtStartup);
    s.setValue("showOnLaunch", o.showOnLaunch);
    s.setValue("alwaysOnTop", o.alwaysOnTop);
    s.setValue("lockGui", o.lockGui);
    s.setValue("contextHelp", o.contextHelp);
    s.setValue("autoRestartOnA1Lost", o.autoRestartOnA1Lost);
    s.setValue("restartOnAnyLost", o.restartOnAnyLost);
    s.setValue("startMacroButtons", o.startMacroButtons);
    s.setValue("limitRemoteGain", o.limitRemoteGain);
    s.setValue("showPresetsOnStartup", o.showPresetsOnStartup);
    s.setValue("startupFile", o.startupFile);
    s.setValue("zoom", o.zoom);
}

void Settings::applyAutostart(bool enable) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
    const QString file = dir + "/io.github.roadboard.RoadBoard.desktop";
    if (!enable) { QFile::remove(file); return; }
    QDir().mkpath(dir);
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QString exe = QCoreApplication::applicationFilePath();
    if (const char* appimage = std::getenv("APPIMAGE")) exe = QString::fromLocal8Bit(appimage);
    const QString content = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=RoadBoard\nComment=Virtual audio mixer\n"
        "Exec=\"%1\" --minimized\nIcon=io.github.roadboard.RoadBoard\nX-GNOME-Autostart-enabled=true\n"
        "X-GNOME-Autostart-Delay=3\nTerminal=false\n").arg(exe);
    f.write(content.toUtf8());
}

} // namespace road
