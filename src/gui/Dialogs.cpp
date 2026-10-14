#include "gui/Dialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>

#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/Midi.h"
#include "engine/Mixer.h"
#include "engine/TapeDeck.h"
#include "engine/RoadLink.h"
#include "gui/AppContext.h"
#include "gui/MainWindow.h"

namespace road {

namespace {

// ------------------------------------------------------------------ binding helpers
// Every bound control refreshes from the parameter (MIDI / network may change it)
// and writes back when the user edits it.
class Binder : public QObject {
public:
    explicit Binder(QWidget* owner) : QObject(owner) {
        connect(&t_, &QTimer::timeout, this, [this] { for (auto& f : refresh_) f(); });
        t_.start(200);
    }
    void check(QCheckBox* c, Param* p) {
        c->setChecked(p->on());
        QObject::connect(c, &QCheckBox::toggled, c, [p](bool on) { p->set(on ? 1 : 0); });
        refresh_.push_back([c, p] { if (c->isChecked() != p->on()) { QSignalBlocker b(c); c->setChecked(p->on()); } });
    }
    void combo(QComboBox* c, Param* p, const QList<QPair<QString, int>>& items) {
        for (auto& it : items) c->addItem(it.first, it.second);
        auto sel = [c, p] {
            const int idx = c->findData(p->i());
            if (idx >= 0 && idx != c->currentIndex()) { QSignalBlocker b(c); c->setCurrentIndex(idx); }
        };
        sel();
        QObject::connect(c, &QComboBox::currentIndexChanged, c, [c, p](int) { p->set(float(c->currentData().toInt())); });
        refresh_.push_back(sel);
    }
    void spin(QSpinBox* s, Param* p) {
        s->setRange(int(p->min), int(p->max));
        s->setValue(p->i());
        QObject::connect(s, &QSpinBox::valueChanged, s, [p](int v) { p->set(float(v)); });
        refresh_.push_back([s, p] { if (!s->hasFocus() && s->value() != p->i()) { QSignalBlocker b(s); s->setValue(p->i()); } });
    }
    void dspin(QDoubleSpinBox* s, Param* p, int decimals) {
        s->setDecimals(decimals);
        s->setRange(double(p->min), double(p->max));
        s->setValue(double(p->get()));
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [p](double v) { p->set(float(v)); });
        refresh_.push_back([s, p] { if (!s->hasFocus() && std::fabs(s->value() - p->get()) > 1e-4) { QSignalBlocker b(s); s->setValue(double(p->get())); } });
    }
    void line(QLineEdit* e, const std::string& key) {
        e->setText(QString::fromStdString(params().str(key)));
        QObject::connect(e, &QLineEdit::editingFinished, e, [e, key] { params().setStr(key, e->text().toStdString()); });
        refresh_.push_back([e, key] {
            const QString v = QString::fromStdString(params().str(key));
            if (!e->hasFocus() && e->text() != v) e->setText(v);
        });
    }
    void label(QLabel* l, std::function<QString()> f) {
        l->setText(f());
        refresh_.push_back([l, f] { l->setText(f()); });
    }
    void every(std::function<void()> f) { refresh_.push_back(std::move(f)); }
private:
    QTimer t_;
    std::vector<std::function<void()>> refresh_;
};

template <typename T>
T* singleton(QWidget* parent, T*& instance) {
    if (!instance) {
        instance = new T(parent);
        instance->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(instance, &QObject::destroyed, [&instance] { instance = nullptr; });
    }
    instance->show();
    instance->raise();
    instance->activateWindow();
    return instance;
}

QList<QPair<QString, int>> inputChannelItems() {
    QList<QPair<QString, int>> items{{"Default BUS channel", 0}};
    static const char* pos[8] = {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};
    for (int s = 0; s < kStrips; ++s) {
        for (int c = 0; c < stripChannels(s); ++c) {
            QString n = isHwStrip(s) ? QStringLiteral("IN %1 %2").arg(s + 1).arg(c == 0 ? "Left" : "Right")
                                     : QStringLiteral("Virtual %1 %2").arg(s - kHwStrips + 1).arg(pos[c]);
            items.push_back({n, stripFirstInCh(s) + c + 1});
        }
    }
    return items;
}

} // namespace

// ====================================================================== system settings
class SystemSettingsDialog : public QDialog {
public:
    explicit SystemSettingsDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle("System Settings / Options");
        ParamStore& P = params();
        auto* b = new Binder(this);
        auto* v = new QVBoxLayout(this);

        // ---- devices status
        auto* devBox = new QGroupBox("Audio devices (PipeWire)");
        auto* dv = new QVBoxLayout(devBox);
        auto* status = new QLabel;
        b->label(status, [] { return "Engine: " + QString::fromStdString(ctx().engine->statusText()); });
        dv->addWidget(status);
        table_ = new QTableWidget(kHwStrips * 2 + kHwBuses * 2 + kVirtStrips + kVirtBuses, 3);
        table_->setHorizontalHeaderLabels({"Point", "Device", "Status"});
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table_->verticalHeader()->hide();
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setMinimumHeight(220);
        dv->addWidget(table_);
        b->every([this] { fillTable(); });
        fillTable();
        v->addWidget(devBox);

        auto* grid = new QHBoxLayout;
        // ---- engine
        auto* eng = new QGroupBox("Audio engine");
        auto* ef = new QFormLayout(eng);
        auto* sr = new QComboBox;
        b->combo(sr, P.opt.sr, {{"Follow PipeWire graph", 0}, {"44.1 kHz", 44100}, {"48 kHz", 48000}, {"88.2 kHz", 88200},
                                {"96 kHz", 96000}, {"176.4 kHz", 176400}, {"192 kHz", 192000}});
        ef->addRow("Preferred main samplerate", sr);
        auto* buf = new QComboBox;
        b->combo(buf, P.opt.buffer, {{"PipeWire default", 0}, {"64", 64}, {"128", 128}, {"256", 256}, {"512", 512},
                                     {"1024", 1024}, {"2048", 2048}});
        ef->addRow("Buffering (samples)", buf);
        auto* apply = new QPushButton("Apply (restart audio engine)");
        connect(apply, &QPushButton::clicked, this, [] { ctx().window->restartEngine(); });
        ef->addRow(apply);
        auto* outLim = new QCheckBox("OUT limiter (0 dBfs brick-wall on every BUS)");
        b->check(outLim, P.opt.outLimiter);
        ef->addRow(outLim);
        auto* meterPost = new QCheckBox("Strip meters show post-fader level");
        b->check(meterPost, P.opt.meterPost);
        ef->addRow(meterPost);
        auto* monSel = new QCheckBox("Monitor on SEL");
        b->check(monSel, P.opt.monitorOnSel);
        ef->addRow(monSel);
        auto* monBus = new QComboBox;
        QList<QPair<QString, int>> busItems{{"(none)", -1}};
        for (int k = 0; k < kBuses; ++k) busItems.push_back({kBusNames[k], k});
        for (auto& it : busItems) monBus->addItem(it.first, it.second);
        auto selMon = [monBus] {
            int cur = -1;
            for (int k = 0; k < kBuses; ++k) if (params().bus[k].monitor->on()) cur = k;
            const int idx = monBus->findData(cur);
            if (idx != monBus->currentIndex()) { QSignalBlocker x(monBus); monBus->setCurrentIndex(idx); }
        };
        selMon();
        connect(monBus, &QComboBox::currentIndexChanged, this, [monBus](int) {
            const int b2 = monBus->currentData().toInt();
            for (int k = 0; k < kBuses; ++k) params().bus[k].monitor->set(k == b2 ? 1 : 0);
        });
        b->every(selMon);
        ef->addRow("Monitoring BUS", monBus);
        auto* slider = new QComboBox;
        b->combo(slider, P.opt.sliderMode, {{"ABSOLUTE", 0}, {"RELATIVE", 1}});
        ef->addRow("Strip slider mode", slider);
        auto* fxType = new QComboBox;
        b->combo(fxType, P.opt.fxType, {{"MultiTap Delay", 0}, {"5-band compressor", 1}});
        ef->addRow("Internal FX 2", fxType);
        auto* extFx = new QCheckBox("External FX ports (RoadBoard FX1/FX2 Send & Return devices)");
        b->check(extFx, P.opt.externalFx);
        ef->addRow(extFx);
        auto* raio = new QCheckBox("RAIO extension (RoadBoard In 1-5 and Out A1-A5 devices)");
        b->check(raio, P.opt.raioExt);
        ef->addRow(raio);
        grid->addWidget(eng);

        // ---- delays & limiter types
        auto* sync = new QGroupBox("Output delay / limiter type");
        auto* sg = new QGridLayout(sync);
        sg->addWidget(new QLabel("BUS"), 0, 0);
        sg->addWidget(new QLabel("Delay (ms)"), 0, 1);
        sg->addWidget(new QLabel("Limiter"), 0, 2);
        for (int i = 0; i < kBuses; ++i) {
            sg->addWidget(new QLabel(kBusNames[i]), i + 1, 0);
            auto* d = new QDoubleSpinBox;
            b->dspin(d, P.bus[i].delay, 1);
            sg->addWidget(d, i + 1, 1);
            auto* lt = new QComboBox;
            b->combo(lt, P.bus[i].limType, {{"Yellow", 0}, {"Green (5 ms)", 1}});
            sg->addWidget(lt, i + 1, 2);
        }
        for (int s = 0; s < kStrips; ++s) {
            sg->addWidget(new QLabel(QStringLiteral("Strip %1 limiter").arg(s + 1)), kBuses + 1 + s, 0, 1, 2);
            auto* lt = new QComboBox;
            b->combo(lt, P.strip[s].limType, {{"Yellow", 0}, {"Green (5 ms)", 1}});
            sg->addWidget(lt, kBuses + 1 + s, 2);
        }
        grid->addWidget(sync);

        // ---- virtual devices
        auto* raioBox = new QGroupBox("Virtual devices (RAIO)");
        auto* vf = new QFormLayout(raioBox);
        const QList<QPair<QString, int>> chItems{{"Mono (1)", 1}, {"Stereo (2)", 2}, {"5.1 (6)", 6}, {"7.1 (8)", 8}};
        for (int i = 0; i < kVirtStrips; ++i) {
            auto* c = new QComboBox;
            b->combo(c, P.opt.vinCh[i], chItems);
            connect(c, &QComboBox::activated, this, [] { params().routingRevision.fetch_add(1); });
            vf->addRow(QString::fromStdString(Engine::virtualInputDescription(i)), c);
        }
        for (int i = 0; i < kVirtBuses; ++i) {
            auto* c = new QComboBox;
            b->combo(c, P.opt.voutCh[i], chItems);
            connect(c, &QComboBox::activated, this, [] { params().routingRevision.fetch_add(1); });
            vf->addRow(QString::fromStdString(Engine::virtualOutputDescription(i)), c);
        }
        grid->addWidget(raioBox);
        v->addLayout(grid);

        // ---- patch composite
        auto* comp = new QGroupBox("Patch composite (BUS mode = Composite)");
        auto* cg = new QGridLayout(comp);
        static const char* chn[8] = {"1 FL", "2 FR", "3 FC", "4 LFE", "5 RL", "6 RR", "7 SL", "8 SR"};
        for (int c = 0; c < kBusCh; ++c) {
            cg->addWidget(new QLabel(chn[c]), c / 4 * 2, c % 4);
            auto* cb = new QComboBox;
            b->combo(cb, P.opt.composite[c], inputChannelItems());
            cg->addWidget(cb, c / 4 * 2 + 1, c % 4);
        }
        auto* post = new QComboBox;
        b->combo(post, P.opt.postFaderComposite, {{"Select PRE-fader inputs", 0}, {"Select POST-fader inputs", 1}});
        cg->addWidget(post, 4, 0, 1, 4);
        v->addWidget(comp);

        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
        v->addWidget(bb);
        resize(900, 760);
    }

private:
    void fillTable() {
        int row = 0;
        auto set = [&](const QString& point, const std::string& node) {
            bool ok = false;
            QString name;
            if (!node.empty()) { const std::string d = ctx().engine->describe(node); ok = !d.empty(); name = QString::fromStdString(d.empty() ? node : d); }
            table_->setItem(row, 0, new QTableWidgetItem(point));
            table_->setItem(row, 1, new QTableWidgetItem(name.isEmpty() ? QString("(none)") : name));
            table_->setItem(row, 2, new QTableWidgetItem(node.empty() ? QString("-") : ok ? QString("connected") : QString("NOT FOUND")));
            ++row;
        };
        for (int s = 0; s < kHwStrips; ++s) set(QStringLiteral("IN %1").arg(s + 1), params().str("strip[" + std::to_string(s) + "].device"));
        for (int s = 0; s < kHwStrips; ++s) set(QStringLiteral("IN %1 RAIO").arg(s + 1), params().opt.raioExt->on() ? Engine::raioInputName(s) : std::string());
        for (int i = 0; i < kVirtStrips; ++i) set(QStringLiteral("Virtual IN %1").arg(i + 1), Engine::virtualInputName(i));
        for (int b = 0; b < kHwBuses; ++b) set(QStringLiteral("OUT %1").arg(kBusNames[b]), params().str("bus[" + std::to_string(b) + "].device"));
        for (int b = 0; b < kHwBuses; ++b) set(QStringLiteral("OUT %1 RAIO").arg(kBusNames[b]), params().opt.raioExt->on() ? Engine::raioOutputName(b) : std::string());
        for (int i = 0; i < kVirtBuses; ++i) set(QStringLiteral("OUT %1").arg(kBusNames[kHwBuses + i]), Engine::virtualOutputName(i));
    }
    QTableWidget* table_;
};

static SystemSettingsDialog* gSettings = nullptr;
void openSystemSettings(QWidget* parent) { singleton(parent, gSettings); }

// ====================================================================== RoadLink
class RoadLinkDialog : public QDialog {
public:
    explicit RoadLinkDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle("RoadLink — network audio");
        ParamStore& P = params();
        auto* b = new Binder(this);
        auto* v = new QVBoxLayout(this);
        auto* top = new QHBoxLayout;
        auto* en = new QCheckBox("RoadLink enabled");
        b->check(en, P.link.enable);
        top->addWidget(en);
        top->addWidget(new QLabel("This unit name:"));
        auto* nm = new QLineEdit;
        b->line(nm, "roadlink.name");
        top->addWidget(nm);
        auto* compat = new QCheckBox("Compatible packet format (talk to existing network-audio apps)");
        b->check(compat, P.link.compat);
        top->addWidget(compat);
        top->addStretch(1);
        v->addLayout(top);

        // incoming
        auto* inBox = new QGroupBox("Incoming streams (right click on a name to pick a detected stream)");
        auto* ig = new QGridLayout(inBox);
        const QStringList ih{"On", "Stream name", "IP address from", "Port", "To strip", "Net quality", "Format", "Errors"};
        for (int c = 0; c < ih.size(); ++c) ig->addWidget(new QLabel("<b>" + ih[c] + "</b>"), 0, c);
        const QList<QPair<QString, int>> quality{{"Optimal", 0}, {"Fast", 1}, {"Medium", 2}, {"Slow", 3}, {"Very slow", 4}};
        QList<QPair<QString, int>> strips;
        for (int s = 0; s < kStrips; ++s)
            strips.push_back({isHwStrip(s) ? QStringLiteral("IN %1").arg(s + 1) : QStringLiteral("Virtual %1").arg(s - kHwStrips + 1), s});
        for (int i = 0; i < kLinkStreams; ++i) {
            const std::string pre = "roadlink.instream[" + std::to_string(i) + "].";
            auto* on = new QCheckBox; b->check(on, P.link.in[i].on); ig->addWidget(on, i + 1, 0);
            auto* name = new QLineEdit; b->line(name, pre + "name"); name->setMaxLength(16); ig->addWidget(name, i + 1, 1);
            name->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(name, &QWidget::customContextMenuRequested, this, [this, name, i, pre](QPoint p) {
                QMenu m(this);
                for (const RoadLinkDetected& d : ctx().link->detected()) {
                    connect(m.addAction(QStringLiteral("%1  from %2  (%3 Hz, %4 ch)").arg(QString::fromStdString(d.name), QString::fromStdString(d.ip))
                                            .arg(d.sampleRate).arg(d.channels)),
                            &QAction::triggered, this, [pre, d, i] {
                                params().setStr(pre + "name", d.name);
                                params().setStr(pre + "ip", d.ip);
                                params().link.in[i].on->set(1);
                            });
                }
                if (m.isEmpty()) m.addAction("(no stream detected on the network — is RoadLink enabled?)")->setEnabled(false);
                m.exec(name->mapToGlobal(p));
            });
            auto* ip = new QLineEdit; b->line(ip, pre + "ip"); ig->addWidget(ip, i + 1, 2);
            auto* port = new QSpinBox; b->spin(port, P.link.in[i].port); ig->addWidget(port, i + 1, 3);
            auto* route = new QComboBox; b->combo(route, P.link.in[i].route, strips); ig->addWidget(route, i + 1, 4);
            auto* q = new QComboBox; b->combo(q, P.link.in[i].quality, quality); ig->addWidget(q, i + 1, 5);
            auto* fmt = new QLabel;
            b->label(fmt, [i] {
                auto& s = params().link.in[i];
                if (s.sr->i() == 0) return QString("-");
                static const char* bits[] = {"8b", "16b", "24b", "32b", "f32", "f64", "12b", "10b"};
                return QStringLiteral("%1 Hz %2ch %3").arg(s.sr->i()).arg(s.ch->i()).arg(bits[std::clamp(s.bit->i(), 0, 7)]);
            });
            ig->addWidget(fmt, i + 1, 6);
            auto* err = new QLabel;
            b->label(err, [i] {
                RoadLinkStats& st = ctx().link->inStats(i);
                return QStringLiteral("ovl %1 cor %2 dis %3 mis %4 und %5").arg(st.overload.load()).arg(st.corrupt.load())
                    .arg(st.disorder.load()).arg(st.missing.load()).arg(st.underrun.load());
            });
            ig->addWidget(err, i + 1, 7);
        }
        auto* txt = new QHBoxLayout;
        auto* ton = new QCheckBox("Incoming RoadLink text commands (request scripts)");
        b->check(ton, P.link.textOn);
        txt->addWidget(ton);
        txt->addWidget(new QLabel("Name:"));
        auto* tname = new QLineEdit; b->line(tname, "roadlink.instream.text.name"); txt->addWidget(tname);
        txt->addWidget(new QLabel("Port:"));
        auto* tport = new QSpinBox; b->spin(tport, P.link.textPort); txt->addWidget(tport);
        ig->addLayout(txt, kLinkStreams + 1, 0, 1, ih.size());
        v->addWidget(inBox);

        // outgoing
        auto* outBox = new QGroupBox("Outgoing streams");
        auto* og = new QGridLayout(outBox);
        const QStringList oh{"On", "Stream name", "IP address to", "Port", "From BUS", "Samplerate", "Channels", "Format", "Level"};
        for (int c = 0; c < oh.size(); ++c) og->addWidget(new QLabel("<b>" + oh[c] + "</b>"), 0, c);
        QList<QPair<QString, int>> buses;
        for (int bb = 0; bb < kBuses; ++bb) buses.push_back({kBusNames[bb], bb});
        const QList<QPair<QString, int>> rates{{"Engine rate", 0}, {"11025", 11025}, {"16000", 16000}, {"22050", 22050}, {"24000", 24000},
                                               {"32000", 32000}, {"44100", 44100}, {"48000", 48000}, {"64000", 64000}, {"88200", 88200}, {"96000", 96000}};
        for (int i = 0; i < kLinkStreams; ++i) {
            const std::string pre = "roadlink.outstream[" + std::to_string(i) + "].";
            auto* on = new QCheckBox; b->check(on, P.link.out[i].on); og->addWidget(on, i + 1, 0);
            auto* name = new QLineEdit; b->line(name, pre + "name"); name->setMaxLength(16); og->addWidget(name, i + 1, 1);
            auto* ip = new QLineEdit; b->line(ip, pre + "ip"); og->addWidget(ip, i + 1, 2);
            auto* port = new QSpinBox; b->spin(port, P.link.out[i].port); og->addWidget(port, i + 1, 3);
            auto* route = new QComboBox; b->combo(route, P.link.out[i].route, buses); og->addWidget(route, i + 1, 4);
            auto* sr = new QComboBox; b->combo(sr, P.link.out[i].sr, rates); og->addWidget(sr, i + 1, 5);
            auto* ch = new QSpinBox; b->spin(ch, P.link.out[i].ch); og->addWidget(ch, i + 1, 6);
            auto* bit = new QComboBox; b->combo(bit, P.link.out[i].bit, {{"16 bits PCM", 1}, {"24 bits PCM", 2}, {"32 bits float", 4}}); og->addWidget(bit, i + 1, 7);
            auto* lvl = new QLabel;
            b->label(lvl, [i] {
                const float l = ctx().link->outStats(i).level.load();
                return l > 1e-5f ? QString::asprintf("%.1f dB", 20 * std::log10(l)) : QString("-");
            });
            og->addWidget(lvl, i + 1, 8);
        }
        v->addWidget(outBox);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
        v->addWidget(bb);
        resize(1100, 720);
    }
};

static RoadLinkDialog* gRoadLink = nullptr;
void openRoadLinkDialog(QWidget* parent) { singleton(parent, gRoadLink); }

// ====================================================================== MIDI
class MidiDialog : public QDialog {
public:
    explicit MidiDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle("M.I.D.I. Mapping");
        MidiEngine* m = ctx().midi;
        auto* v = new QVBoxLayout(this);
        if (!m || !m->available()) {
            v->addWidget(new QLabel("The ALSA MIDI sequencer is not available on this system."));
            return;
        }
        auto* devs = new QFormLayout;
        in0_ = new QComboBox; in1_ = new QComboBox; out_ = new QComboBox;
        devs->addRow("MIDI input device", in0_);
        devs->addRow("MIDI extra input device", in1_);
        devs->addRow("MIDI output (feedback)", out_);
        mapName_ = new QLineEdit(QString::fromStdString(m->mapName));
        connect(mapName_, &QLineEdit::editingFinished, this, [this, m] { m->mapName = mapName_->text().toStdString(); });
        devs->addRow("Map name", mapName_);
        last_ = new QLabel("-");
        devs->addRow("Last message", last_);
        v->addLayout(devs);
        fillDevices();
        connect(in0_, &QComboBox::activated, this, [this, m] { m->connectInput(0, in0_->currentData().toString().toStdString()); });
        connect(in1_, &QComboBox::activated, this, [this, m] { m->connectInput(1, in1_->currentData().toString().toStdString()); });
        connect(out_, &QComboBox::activated, this, [this, m] { m->connectOutput(out_->currentData().toString().toStdString()); });

        table_ = new QTableWidget;
        table_->setColumnCount(5);
        table_->setHorizontalHeaderLabels({"Control", "MIDI code", "Learn", "PTT", "Feedback"});
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->verticalHeader()->hide();
        const auto targets = MidiEngine::targets();
        table_->setRowCount(int(targets.size()));
        for (int r = 0; r < int(targets.size()); ++r) {
            const std::string key = targets[size_t(r)].first;
            table_->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(targets[size_t(r)].second)));
            auto* code = new QTableWidgetItem("-");
            code->setFlags(code->flags() & ~Qt::ItemIsEditable);
            table_->setItem(r, 1, code);
            auto* learn = new QPushButton("Learn");
            learn->setCheckable(true);
            connect(learn, &QPushButton::toggled, this, [m, key](bool on) { m->learn(on ? key : std::string()); });
            table_->setCellWidget(r, 2, learn);
            auto* ptt = new QCheckBox;
            ptt->setEnabled(key.find("mute") != std::string::npos);
            connect(ptt, &QCheckBox::toggled, this, [this, m, key](bool on) { modify(key, [on](MidiMapping& x) { x.ptt = on; }); (void)m; });
            table_->setCellWidget(r, 3, ptt);
            auto* fb = new QComboBox;
            fb->addItems({"-", "F", "FF"});
            connect(fb, &QComboBox::activated, this, [this, key](int i) { modify(key, [i](MidiMapping& x) { x.feedback = i; }); });
            table_->setCellWidget(r, 4, fb);
            keys_.push_back(key);
        }
        table_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table_, &QWidget::customContextMenuRequested, this, [this, m](QPoint p) {
            const int r = table_->rowAt(p.y());
            if (r < 0) return;
            QMenu menu(this);
            connect(menu.addAction("Reset this MIDI code"), &QAction::triggered, this, [this, m, r] {
                auto map = m->mappings();
                map.erase(std::remove_if(map.begin(), map.end(), [&](const MidiMapping& x) { return x.target == keys_[size_t(r)]; }), map.end());
                m->setMappings(map);
            });
            menu.exec(table_->viewport()->mapToGlobal(p));
        });
        v->addWidget(table_, 1);

        auto* btns = new QHBoxLayout;
        auto* reset = new QPushButton("Reset map");
        connect(reset, &QPushButton::clicked, this, [m] { m->resetMap(); });
        auto* load = new QPushButton("Load...");
        connect(load, &QPushButton::clicked, this, [this, m] {
            const QString f = QFileDialog::getOpenFileName(this, "Load MIDI map", Settings::configDir(), "MIDI map (*.map)");
            if (!f.isEmpty()) { m->load(f.toStdString()); mapName_->setText(QString::fromStdString(m->mapName)); fillDevices(); }
        });
        auto* save = new QPushButton("Save...");
        connect(save, &QPushButton::clicked, this, [this, m] {
            const QString f = QFileDialog::getSaveFileName(this, "Save MIDI map", Settings::configDir() + "/controller.map", "MIDI map (*.map)");
            if (!f.isEmpty()) m->save(f.toStdString());
        });
        auto* refreshBtn = new QPushButton("Refresh controller");
        connect(refreshBtn, &QPushButton::clicked, this, [m] { m->sendFeedback(true); });
        auto* rescan = new QPushButton("Rescan devices");
        connect(rescan, &QPushButton::clicked, this, [this] { fillDevices(); });
        for (auto* w : {reset, load, save, refreshBtn, rescan}) btns->addWidget(w);
        btns->addStretch(1);
        v->addLayout(btns);

        connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
        timer_.start(150);
        resize(760, 700);
    }
    ~MidiDialog() override {
        if (ctx().midi) { ctx().midi->learn({}); ctx().midi->save((Settings::configDir() + "/midi.map").toStdString()); }
    }

private:
    void fillDevices() {
        MidiEngine* m = ctx().midi;
        auto fill = [](QComboBox* c, const std::vector<MidiPort>& ports, const std::string& cur) {
            c->clear();
            c->addItem("- none -", QString());
            for (auto& p : ports) c->addItem(QString::fromStdString(p.name), QString::fromStdString(p.name));
            const int i = c->findData(QString::fromStdString(cur));
            c->setCurrentIndex(i >= 0 ? i : 0);
        };
        fill(in0_, m->inputPorts(), m->inputName(0));
        fill(in1_, m->inputPorts(), m->inputName(1));
        fill(out_, m->outputPorts(), m->outputName());
    }
    void modify(const std::string& key, std::function<void(MidiMapping&)> f) {
        auto map = ctx().midi->mappings();
        for (auto& x : map) if (x.target == key) { f(x); ctx().midi->setMappings(map); return; }
    }
    void refresh() {
        MidiEngine* m = ctx().midi;
        const int st = m->lastStatus.load();
        if (st >= 0) last_->setText(QString::asprintf("%02X %02X %02X  (channel %d)", st, m->lastD1.load(), m->lastD2.load(), (st & 0x0F) + 1));
        const auto map = m->mappings();
        const std::string learning = m->learning();
        for (int r = 0; r < int(keys_.size()); ++r) {
            const MidiMapping* found = nullptr;
            for (auto& x : map) if (x.target == keys_[size_t(r)]) found = &x;
            QString code = "-";
            if (found && found->type >= 0) {
                static const char* t[] = {"CC", "Note", "PitchBend"};
                code = QStringLiteral("%1 ch%2 #%3").arg(t[found->type]).arg(found->channel + 1).arg(found->number);
            }
            if (table_->item(r, 1)->text() != code) table_->item(r, 1)->setText(code);
            auto* learn = static_cast<QPushButton*>(table_->cellWidget(r, 2));
            const bool on = learning == keys_[size_t(r)];
            if (learn->isChecked() != on) { QSignalBlocker b(learn); learn->setChecked(on); }
            auto* ptt = static_cast<QCheckBox*>(table_->cellWidget(r, 3));
            if (found && ptt->isChecked() != found->ptt) { QSignalBlocker b(ptt); ptt->setChecked(found->ptt); }
            auto* fb = static_cast<QComboBox*>(table_->cellWidget(r, 4));
            const int f = found ? found->feedback : 0;
            if (fb->currentIndex() != f) { QSignalBlocker b(fb); fb->setCurrentIndex(f); }
        }
    }
    QComboBox *in0_ = nullptr, *in1_ = nullptr, *out_ = nullptr;
    QLineEdit* mapName_ = nullptr;
    QLabel* last_ = nullptr;
    QTableWidget* table_ = nullptr;
    std::vector<std::string> keys_;
    QTimer timer_;
};

static MidiDialog* gMidi = nullptr;
void openMidiDialog(QWidget* parent) { singleton(parent, gMidi); }

// ====================================================================== recorder options
class TapeOptionsDialog : public QDialog {
public:
    explicit TapeOptionsDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle("Recorder Options");
        ParamStore& P = params();
        auto* b = new Binder(this);
        auto* v = new QVBoxLayout(this);

        auto* src = new QGroupBox("Recording source");
        auto* sg = new QGridLayout(src);
        auto* pre = new QRadioButton("PRE-FADER INPUTS (mixed in stereo, original level)");
        auto* post = new QRadioButton("POST-FADER OUTPUT of a BUS (2 to 8 channels)");
        sg->addWidget(pre, 0, 0, 1, 5);
        for (int s = 0; s < kStrips; ++s) {
            auto* c = new QCheckBox(isHwStrip(s) ? QStringLiteral("IN %1").arg(s + 1) : QStringLiteral("Virtual %1").arg(s - kHwStrips + 1));
            b->check(c, P.rec.armStrip[s]);
            sg->addWidget(c, 1, s);
        }
        sg->addWidget(post, 2, 0, 1, 5);
        auto* busSel = new QComboBox;
        for (int bb = 0; bb < kBuses; ++bb) busSel->addItem(kBusNames[bb], bb);
        auto selBus = [busSel] {
            for (int bb = 0; bb < kBuses; ++bb) if (params().rec.armBus[bb]->on()) { if (busSel->currentIndex() != bb) { QSignalBlocker x(busSel); busSel->setCurrentIndex(bb); } return; }
        };
        selBus();
        connect(busSel, &QComboBox::currentIndexChanged, this, [](int i) {
            for (int bb = 0; bb < kBuses; ++bb) params().rec.armBus[bb]->set(bb == i ? 1 : 0);
        });
        b->every(selBus);
        sg->addWidget(busSel, 3, 0);
        auto* chans = new QSpinBox;
        b->spin(chans, P.rec.channels);
        sg->addWidget(new QLabel("Channels:"), 3, 1);
        sg->addWidget(chans, 3, 2);
        (P.rec.recBus->on() ? post : pre)->setChecked(true);
        connect(post, &QRadioButton::toggled, this, [](bool on) { params().rec.recBus->set(on ? 1 : 0); });
        v->addWidget(src);

        auto* file = new QGroupBox("File");
        auto* ff = new QFormLayout(file);
        auto* folderRow = new QHBoxLayout;
        auto* folder = new QLineEdit;
        b->line(folder, "recorder.folder");
        folder->setPlaceholderText(QString::fromStdString(TapeDeck::defaultFolder()));
        auto* browse = new QPushButton("...");
        connect(browse, &QPushButton::clicked, this, [this, folder] {
            const QString d = QFileDialog::getExistingDirectory(this, "Recording folder", folder->text());
            if (!d.isEmpty()) { folder->setText(d); params().setStr("recorder.folder", d.toStdString()); }
        });
        folderRow->addWidget(folder);
        folderRow->addWidget(browse);
        ff->addRow("Target directory", folderRow);
        auto* prefix = new QLineEdit;
        b->line(prefix, "recorder.prefix");
        ff->addRow("Filename prefix", prefix);
        auto* type = new QComboBox;
        b->combo(type, P.rec.fileType, {{"WAV", 1}, {"BWF (broadcast wave)", 3}, {"AIFF", 2}, {"FLAC", 4}, {"OGG Vorbis", 5}, {"MP3 (stereo)", 100}});
        ff->addRow("File type", type);
        auto* bits = new QComboBox;
        b->combo(bits, P.rec.bitResolution, {{"8 bits", 8}, {"16 bits", 16}, {"24 bits", 24}, {"32 bits float", 32}});
        ff->addRow("Resolution", bits);
        auto* kbps = new QComboBox;
        b->combo(kbps, P.rec.kbps, {{"32 kbps", 32}, {"64 kbps", 64}, {"96 kbps", 96}, {"128 kbps", 128}, {"160 kbps", 160},
                                    {"192 kbps", 192}, {"256 kbps", 256}, {"320 kbps", 320}});
        ff->addRow("MP3 bitrate", kbps);
        auto* sr = new QComboBox;
        b->combo(sr, P.rec.sampleRate, {{"Engine rate", 0}, {"22050", 22050}, {"32000", 32000}, {"44100", 44100}, {"48000", 48000},
                                        {"88200", 88200}, {"96000", 96000}, {"192000", 192000}});
        ff->addRow("File samplerate", sr);
        auto* multi = new QCheckBox("Also write one file per channel (_Track1, _Track2...)");
        b->check(multi, P.rec.multiTrack);
        ff->addRow("Multitrack", multi);
        auto* preRec = new QSpinBox;
        b->spin(preRec, P.rec.preRec);
        preRec->setSuffix(" s");
        ff->addRow("Pre-recording buffer", preRec);
        auto* stopAfter = new QTimeEdit;
        stopAfter->setDisplayFormat("HH:mm:ss");
        stopAfter->setTime(QTime(0, 0).addSecs(P.rec.stopAfter->i()));
        connect(stopAfter, &QTimeEdit::timeChanged, this, [](QTime t) { params().rec.stopAfter->set(float(QTime(0, 0).secsTo(t))); });
        ff->addRow("Stop record after (0 = off)", stopAfter);
        v->addWidget(file);

        auto* play = new QGroupBox("Playback");
        auto* pf = new QFormLayout(play);
        auto* pol = new QCheckBox; b->check(pol, P.rec.playOnLoad); pf->addRow("Play on load", pol);
        auto* loop = new QCheckBox; b->check(loop, P.rec.loop); pf->addRow("Loop", loop);
        auto* gain = new QSlider(Qt::Horizontal);
        gain->setRange(-600, 120);
        gain->setValue(int(P.rec.gain->get() * 10));
        auto* gl = new QLabel;
        connect(gain, &QSlider::valueChanged, this, [](int x) { params().rec.gain->set(x / 10.0f); });
        b->every([gain, gl] {
            const int x = int(std::lround(params().rec.gain->get() * 10));
            if (!gain->isSliderDown() && gain->value() != x) { QSignalBlocker k(gain); gain->setValue(x); }
            gl->setText(QString::asprintf("%+.1f dB", double(params().rec.gain->get())));
        });
        auto* gr = new QHBoxLayout; gr->addWidget(gain, 1); gr->addWidget(gl);
        pf->addRow("Playback gain", gr);
        v->addWidget(play);
        auto* last = new QLabel;
        b->label(last, [] {
            const std::string e = ctx().tape->lastError();
            const std::string f = ctx().tape->lastRecordFile();
            return QString::fromStdString(e.empty() ? (f.empty() ? "" : "Last record: " + f) : "Error: " + e);
        });
        last->setWordWrap(true);
        v->addWidget(last);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
        v->addWidget(bb);
        resize(560, 640);
    }
};

static TapeOptionsDialog* gTape = nullptr;
void openTapeOptions(QWidget* parent) { singleton(parent, gTape); }

// ====================================================================== presets
class PresetsDialog : public QDialog {
public:
    explicit PresetsDialog(QWidget* parent) : QDialog(parent) {
        setWindowTitle("Preset Scenes");
        auto* v = new QVBoxLayout(this);
        list_ = new QListWidget;
        v->addWidget(new QLabel("Enter / Ctrl+click to recall · F1–F12 recall presets 1–12 · right click to store"));
        v->addWidget(list_, 1);
        refill();
        connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { recall(list_->row(it)); });
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
            if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) recall(list_->row(it));
        });
        list_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(list_, &QWidget::customContextMenuRequested, this, [this](QPoint p) {
            const int r = list_->currentRow();
            if (r < 0) return;
            QMenu m(this);
            connect(m.addAction("Recall"), &QAction::triggered, this, [this, r] { recall(r); });
            connect(m.addAction("Store current settings here..."), &QAction::triggered, this, [this, r] {
                bool ok = false;
                const auto all = Settings::presets();
                const QString name = QInputDialog::getText(this, "Store preset", "Name:", QLineEdit::Normal, all[r].name, &ok);
                if (!ok) return;
                const QString comment = QInputDialog::getText(this, "Store preset", "Comment (empty = date):", QLineEdit::Normal, all[r].comment, &ok);
                if (!ok) return;
                Settings::storePreset(r, name, comment);
                refill();
            });
            connect(m.addAction("Delete"), &QAction::triggered, this, [this, r] { Settings::deletePreset(r); refill(); });
            m.exec(list_->viewport()->mapToGlobal(p));
        });
        for (int k = 0; k < 12; ++k) {
            auto* sc = new QShortcut(QKeySequence(Qt::Key_F1 + k), this);
            connect(sc, &QShortcut::activated, this, [this, k] { recall(k); });
        }
        resize(460, 620);
    }
private:
    void recall(int r) {
        if (!Settings::recallPreset(r)) QMessageBox::information(this, "RoadBoard", "This preset is empty (right click to store).");
        refill();
    }
    void refill() {
        const int cur = list_->currentRow();
        list_->clear();
        for (const auto& p : Settings::presets()) {
            QString t = QStringLiteral("%1. ").arg(p.index + 1, 2);
            t += p.used ? p.name + "   —   " + p.comment : QStringLiteral("(empty)");
            auto* it = new QListWidgetItem(t);
            if (p.index == Settings::currentPreset) { QFont f = it->font(); f.setBold(true); it->setFont(f); }
            if (!p.used) it->setForeground(Qt::gray);
            list_->addItem(it);
        }
        list_->setCurrentRow(cur);
    }
    QListWidget* list_;
};

static PresetsDialog* gPresets = nullptr;
void openPresets(QWidget* parent) { singleton(parent, gPresets); }

// ====================================================================== about
void openAbout(QWidget* parent) {
    QMessageBox::about(parent, "About RoadBoard",
        "<h2>RoadBoard " ROADBOARD_VERSION "</h2>"
        "<p>Advanced virtual audio mixing console for Linux.</p>"
        "<p>8 inputs (5 hardware + 3 virtual) · 8 buses (A1–A5 + B1–B3) with a full 8×8 sub-mix matrix · "
        "ROADPAN (Color / Modulation / 3D), compressor, gate, denoiser, Voice Modeler, 6-band EQ on inputs, "
        "limiters, karaoke, 6×8 parametric bus EQ, 14 bus modes, Reverb, MultiTap Delay, 5-band compressor, "
        "external FX ports, integrated recorder, RoadLink network audio, MIDI, macro buttons, "
        "Application Mixer and request scripting.</p>"
        "<p>Audio engine: PipeWire. Works on every distribution running PipeWire "
        "(PulseAudio / JACK applications included).</p>"
        "<p>Command line: <code>roadboard --cmd 'Strip(0).Mute=1'</code></p>"
        "<p>Free software released under the GNU General Public License v3.0 or later.<br>"
        "<a href='https://github.com/HubDevCode/RoadBoard'>github.com/HubDevCode/RoadBoard</a></p>");
}

} // namespace road
