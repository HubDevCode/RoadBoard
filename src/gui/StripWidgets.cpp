#include "gui/StripWidgets.h"

#include <QFileDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/Mixer.h"
#include "engine/TapeDeck.h"
#include "gui/AppContext.h"
#include "gui/AppMixer.h"
#include "gui/Detail.h"
#include "gui/Dialogs.h"
#include "gui/EqDialog.h"
#include "gui/Theme.h"

namespace road {

using namespace theme;

static std::string sk(int s, const char* leaf) { return "strip[" + std::to_string(s) + "]." + leaf; }

void StripBase::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    g.setPen(QPen(border, 1));
    g.setBrush(panel);
    g.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
}

QString deviceDisplayName(const std::string& nodeName, bool* connected) {
    if (nodeName.empty()) { if (connected) *connected = false; return {}; }
    const std::string d = ctx().engine ? ctx().engine->describe(nodeName) : std::string();
    if (connected) *connected = !d.empty();
    return QString::fromStdString(d.empty() ? nodeName : d);
}

void showInputDeviceMenu(QWidget* parent, int strip, QPoint pos) {
    QMenu menu(parent);
    const std::string key = sk(strip, "device");
    const std::string cur = params().str(key);
    QAction* none = menu.addAction("- remove selection -");
    none->setCheckable(true);
    none->setChecked(cur.empty());
    QObject::connect(none, &QAction::triggered, [key] { params().setStr(key, ""); });
    menu.addSeparator();
    const auto devs = ctx().engine ? ctx().engine->inputDevices() : std::vector<AudioDevice>{};
    QMenu* monitors = nullptr;
    for (const AudioDevice& d : devs) {
        QMenu* target = &menu;
        if (d.monitor) {
            if (!monitors) monitors = menu.addMenu("Monitor of playback devices");
            target = monitors;
        }
        QString text = QString::fromStdString(d.description);
        if (!d.api.empty()) text += QStringLiteral("   [%1, %2 ch]").arg(QString::fromStdString(d.api)).arg(d.channels);
        QAction* a = target->addAction(text);
        a->setCheckable(true);
        a->setChecked(d.name == cur);
        const std::string name = d.name;
        QObject::connect(a, &QAction::triggered, [key, name] { params().setStr(key, name); });
    }
    if (devs.empty()) menu.addAction("(no capture device found)")->setEnabled(false);
    menu.exec(pos);
}

void showOutputDeviceMenu(QWidget* parent, int bus, QPoint pos) {
    QMenu menu(parent);
    const std::string key = "bus[" + std::to_string(bus) + "].device";
    const std::string cur = params().str(key);
    QAction* none = menu.addAction("- remove selection -");
    none->setCheckable(true);
    none->setChecked(cur.empty());
    QObject::connect(none, &QAction::triggered, [key] { params().setStr(key, ""); });
    menu.addSeparator();
    const auto devs = ctx().engine ? ctx().engine->outputDevices() : std::vector<AudioDevice>{};
    for (const AudioDevice& d : devs) {
        QString text = QString::fromStdString(d.description);
        if (!d.api.empty()) text += QStringLiteral("   [%1, %2 ch]").arg(QString::fromStdString(d.api)).arg(d.channels);
        QAction* a = menu.addAction(text);
        a->setCheckable(true);
        a->setChecked(d.name == cur);
        const std::string name = d.name;
        QObject::connect(a, &QAction::triggered, [key, name] { params().setStr(key, name); });
    }
    if (devs.empty()) menu.addAction("(no playback device found)")->setEnabled(false);
    menu.exec(pos);
}

// ------------------------------------------------------------------ strip menu
void showStripMenu(QWidget* parent, int strip, QPoint pos) {
    static Settings::KV clipboard;
    static bool clipHw = true;
    QMenu menu(parent);
    const QStringList sections = Settings::stripSections(strip);
    QMenu* reset = menu.addMenu("Reset");
    QMenu* copy = menu.addMenu("Copy");
    QMenu* paste = menu.addMenu("Paste");
    menu.addSeparator();
    QMenu* load = menu.addMenu("Load from file");
    QMenu* save = menu.addMenu("Save to file");
    for (const QString& s : sections) {
        QObject::connect(reset->addAction(s), &QAction::triggered, [strip, s] { Settings::resetStrip(strip, s); });
        QObject::connect(copy->addAction(s), &QAction::triggered, [strip, s] {
            clipboard = Settings::exportStrip(strip, s);
            clipHw = isHwStrip(strip);
        });
        QObject::connect(save->addAction(s), &QAction::triggered, [parent, strip, s] {
            const QString f = QFileDialog::getSaveFileName(parent, "Save strip settings", QString(), "Strip settings (*.xml)");
            if (!f.isEmpty()) Settings::saveKV(f, isHwStrip(strip) ? "RoadBoardHwStrip" : "RoadBoardVirtualStrip", Settings::exportStrip(strip, s));
        });
    }
    QAction* pasteAll = paste->addAction("Paste");
    pasteAll->setEnabled(!clipboard.isEmpty() && clipHw == isHwStrip(strip));
    QObject::connect(pasteAll, &QAction::triggered, [strip] { Settings::importStrip(strip, clipboard); });
    QAction* loadAll = load->addAction("Load...");
    QObject::connect(loadAll, &QAction::triggered, [parent, strip] {
        const QString f = QFileDialog::getOpenFileName(parent, "Load strip settings", QString(), "Strip settings (*.xml)");
        Settings::KV kv;
        if (!f.isEmpty() && Settings::loadKV(f, isHwStrip(strip) ? "RoadBoardHwStrip" : "RoadBoardVirtualStrip", &kv))
            Settings::importStrip(strip, kv);
        else if (!f.isEmpty()) QMessageBox::warning(parent, "RoadBoard", "This file does not contain settings for this kind of strip.");
    });
    menu.exec(pos);
}

// ------------------------------------------------------------------ bus assignment buttons
bool BusAssignButton::layerView = false;

BusAssignButton::BusAssignButton(int strip, int bus, QWidget* parent) : QWidget(parent), strip_(strip), bus_(bus) {
    setMinimumSize(30, 17);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Send to bus %1\nRight click: show / hide the sub-mix (gain per bus)\n"
                              "Ctrl+wheel: change the sub-mix level, Ctrl+click: type it").arg(kBusNames[bus]));
}

void BusAssignButton::sync() {
    const float on = params().strip[strip_].bus[bus_]->get();
    const float l = params().strip[strip_].gainLayer[bus_]->get();
    if (on != lastOn_ || l != lastLayer_ || layerView != lastView_) { lastOn_ = on; lastLayer_ = l; lastView_ = layerView; update(); }
}

void BusAssignButton::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    const bool on = params().strip[strip_].bus[bus_]->on();
    const QColor c = bus_ < kHwBuses ? busA : busB;
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    g.setBrush(on ? c : btnOff);
    g.setPen(QPen(on ? c.lighter(140) : border, 1));
    g.drawRoundedRect(r, 3, 3);
    g.setPen(on ? bgDeep : btnText);
    if (layerView) {
        const float l = params().strip[strip_].gainLayer[bus_]->get();
        // small bar showing the layer level
        const float pos = (l + 60.0f) / 72.0f;
        g.fillRect(QRectF(r.left() + 2, r.bottom() - 3, (r.width() - 4) * pos, 2), on ? bgDeep : accent);
        g.setFont(mkFont(9, true));
        g.drawText(r.adjusted(0, -2, 0, 0), Qt::AlignCenter, QString::number(double(l), 'f', l <= -10 || l >= 10 ? 0 : 1));
    } else {
        g.setFont(mkFont(10, true));
        g.drawText(r, Qt::AlignCenter, kBusNames[bus_]);
    }
}

void BusAssignButton::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {
        layerView = !layerView;
        Syncable::syncAll();
        return;
    }
    if (Syncable::guiLocked()) return;
    Param* layer = params().strip[strip_].gainLayer[bus_];
    if (e->modifiers() & Qt::ControlModifier) {
        bool ok = false;
        const double v = QInputDialog::getDouble(this, QStringLiteral("Sub-mix %1").arg(kBusNames[bus_]), "Gain (dB)",
                                                 double(layer->get()), -60, 12, 1, &ok);
        if (ok) layer->set(float(v));
        return;
    }
    Param* p = params().strip[strip_].bus[bus_];
    p->set(p->on() ? 0 : 1);
    sync();
}

void BusAssignButton::wheelEvent(QWheelEvent* e) {
    if (Syncable::guiLocked() || !(e->modifiers() & Qt::ControlModifier)) { e->ignore(); return; }
    Param* layer = params().strip[strip_].gainLayer[bus_];
    layer->set(layer->get() + (e->angleDelta().y() > 0 ? 0.5f : -0.5f));
    sync();
}

// ------------------------------------------------------------------ strip fader (follows SEL)
StripFader::StripFader(int strip, QWidget* parent) : Fader(params().strip[strip].gain, parent), strip_(strip) {}

void StripFader::sync() {
    ParamStore& P = params();
    int sel = -1;
    for (int b = 0; b < kBuses; ++b) if (P.bus[b].sel->on()) { sel = b; break; }
    if (sel >= 0) setParam(P.strip[strip_].gainLayer[sel], QStringLiteral("%1 mix").arg(kBusNames[sel]), sel < kHwBuses ? busA : busB);
    else setParam(P.strip[strip_].gain);
    Fader::sync();
}

// ------------------------------------------------------------------ applications on a virtual input
AppStripList::AppStripList(int strip, QWidget* parent) : QWidget(parent), strip_(strip) {
    setMinimumHeight(4 * 17 + 2);
    setToolTip("Applications playing on this virtual input.\nDrag: volume, M: mute, right click: Application Mixer");
}

void AppStripList::refresh() {
    std::vector<Row> rows;
    if (ctx().engine) {
        const std::string target = Engine::virtualInputName(strip_ - kHwStrips);
        for (const AppStream& s : ctx().engine->streams())
            if (s.output && s.target == target)
                rows.push_back({s.id, QString::fromStdString(s.appName), std::cbrt(s.volume), s.mute});
    }
    bool same = rows.size() == rows_.size();
    for (size_t i = 0; same && i < rows.size(); ++i)
        same = rows[i].id == rows_[i].id && std::fabs(rows[i].vol - rows_[i].vol) < 0.002f && rows[i].mute == rows_[i].mute;
    if (same) return;
    if (drag_ >= 0) return;   // do not fight with the user
    rows_ = rows;
    update();
}

int AppStripList::rowAt(QPointF p) const {
    const int r = int(p.y() / 17);
    return r >= 0 && r < int(std::min<size_t>(rows_.size(), 4)) ? r : -1;
}

void AppStripList::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    g.fillRect(rect(), bgDeep);
    g.setFont(mkFont(9, true));
    if (rows_.empty()) {
        g.setPen(textDim);
        g.drawText(rect(), Qt::AlignCenter, "no application");
        return;
    }
    for (size_t i = 0; i < std::min<size_t>(rows_.size(), 4); ++i) {
        const Row& r = rows_[i];
        const QRectF row(1, 1 + 17 * double(i), width() - 2, 15);
        const QRectF bar(row.left(), row.top() + 9, row.width() - 18, 5);
        g.setPen(r.mute ? textDim : text);
        g.drawText(row.adjusted(2, -1, -18, -5), Qt::AlignLeft | Qt::AlignTop, g.fontMetrics().elidedText(r.name, Qt::ElideRight, int(row.width() - 22)));
        g.fillRect(bar, panelHi);
        g.fillRect(QRectF(bar.left(), bar.top(), bar.width() * std::min(r.vol, 1.0f), bar.height()), r.mute ? textDim : accent);
        const QRectF m(row.right() - 15, row.top(), 15, 15);
        g.setBrush(r.mute ? theme::mute : btnOff);
        g.setPen(QPen(border, 1));
        g.drawRoundedRect(m, 2, 2);
        g.setPen(r.mute ? bgDeep : btnText);
        g.drawText(m, Qt::AlignCenter, "M");
    }
    if (rows_.size() > 4) {
        g.setPen(title);
        g.drawText(rect().adjusted(0, 0, -2, 0), Qt::AlignRight | Qt::AlignBottom, QStringLiteral("+%1").arg(rows_.size() - 4));
    }
}

void AppStripList::setVolFromX(int row, double x) {
    const double w = width() - 20;
    const float pos = float(std::clamp((x - 1) / w, 0.0, 1.0));
    rows_[size_t(row)].vol = pos;
    ctx().engine->setNodeVolume(rows_[size_t(row)].id, pos * pos * pos);
    update();
}

void AppStripList::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) { openAppMixer(this); return; }
    if (Syncable::guiLocked() || !ctx().engine) return;
    const int r = rowAt(e->position());
    if (r < 0) return;
    if (e->position().x() > width() - 17) {
        rows_[size_t(r)].mute = !rows_[size_t(r)].mute;
        ctx().engine->setNodeMute(rows_[size_t(r)].id, rows_[size_t(r)].mute);
        update();
        return;
    }
    drag_ = r;
    setVolFromX(r, e->position().x());
}

void AppStripList::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ < 0 || !(e->buttons() & Qt::LeftButton)) { drag_ = -1; return; }
    if (drag_ < int(rows_.size())) setVolFromX(drag_, e->position().x());
}

void AppStripList::wheelEvent(QWheelEvent* e) {
    const int r = rowAt(e->position());
    if (r < 0 || Syncable::guiLocked() || !ctx().engine) return;
    const float pos = std::clamp(rows_[size_t(r)].vol + (e->angleDelta().y() > 0 ? 0.02f : -0.02f), 0.0f, 1.0f);
    rows_[size_t(r)].vol = pos;
    ctx().engine->setNodeVolume(rows_[size_t(r)].id, pos * pos * pos);
    update();
}

// small helpers ----------------------------------------------------------------
// "send to" buttons, grouped: hardware outputs A1..A5 (green) then virtual outputs B1..B3 (blue)
static void addBusButtons(QWidget* parent, int strip, QVBoxLayout* col) {
    auto group = [&](const QString& t, QColor c, int from, int to) {
        auto* cap = new Caption(t, c, 8, Qt::AlignCenter, parent);
        cap->setFixedHeight(10);
        col->addWidget(cap);
        for (int b = from; b < to; ++b) {
            auto* btn = new BusAssignButton(strip, b, parent);
            btn->setFixedHeight(18);
            col->addWidget(btn);
        }
    };
    group("OUT A", busA, 0, kHwBuses);
    col->addSpacing(3);
    group("OUT B", busB, kHwBuses, kBuses);
}

// ------------------------------------------------------------------ hardware strip
HwStripWidget::HwStripWidget(int s, QWidget* parent) : StripBase(s, parent) {
    ParamStore& P = params();
    auto& sp = P.strip[s];
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(2);

    device_ = new DeviceButton("INPUT", this);
    device_->setBadge(QString::number(s + 1), hwInColor);
    device_->setAccent(hwInColor);
    device_->setToolTip("Click to choose the microphone / input device of this strip");
    device_->setFixedHeight(40);
    connect(device_, &DeviceButton::clicked, this, [this](QPoint p) { showInputDeviceMenu(this, index_, p); });
    v->addWidget(device_);

    auto* label = new EditableLabel(sk(s, "label"), QStringLiteral("Input %1").arg(s + 1), this);
    connect(label, &EditableLabel::rightClicked, this, [this](QPoint p) { showStripMenu(this, index_, p); });
    v->addWidget(label);

    auto* capRow = new QHBoxLayout;
    capRow->setSpacing(2);
    auto* cap = new ToggleButton(nullptr, "ROADPAN", textDim, this);
    cap->setTextFn([this] { return pad_ ? pad_->currentPage().name.toUpper() : QString("ROADPAN"); });
    cap->setFixedHeight(15);
    cap->setToolTip("Click: next panel (Color / Modulation / 3D)\nRight click: strip menu (reset, copy, paste, load, save)");
    connect(cap, &ToggleButton::rightClicked, this, [this](QPoint p) { showStripMenu(this, index_, p); });
    cap->setMomentary([this] { pad_->setPage((pad_->page() + 1) % pad_->pageCount()); });
    capRow->addWidget(cap, 1);
    auto* eq = new ToggleButton(sp.eqOn, "EQ", busA, this);
    eq->setFixedSize(26, 15);
    eq->setToolTip("6-band parametric EQ on/off. Right click to open it.");
    connect(eq, &ToggleButton::rightClicked, this, [this] { openStripEqDialog(this, index_); });
    capRow->addWidget(eq);
    v->addLayout(capRow);

    pad_ = new Pad2D(this);
    pad_->addPage({sp.colorX, sp.colorY, "Color Panel", QColor(0x6b, 0x4f, 0x8c), QColor(0x1f, 0x3e, 0x63), "dark", "bright", "room", "presence", false});
    pad_->addPage({sp.fxX, sp.fxY, "Modulation", QColor(0x2d, 0x6c, 0x6a), QColor(0x22, 0x3f, 0x5a), "fb", "", "mod", "chorus", false});
    pad_->addPage({sp.panX, sp.panY, "3D Panel", QColor(0x28, 0x4c, 0x7c), QColor(0x16, 0x2a, 0x45), "L", "R", "far", "near", false});
    pad_->setIndicators(true);
    pad_->setPage(P.opt.roadPanPage->i());
    pad_->setMinimumHeight(64);
    pad_->setToolTip("ROADPAN: drag the cursor, double click to reset.\nRight click to change panel.");
    connect(pad_, &Pad2D::contextMenuRequested, this, &HwStripWidget::showPadMenu);
    v->addWidget(pad_, 2);

    auto* knobs = new QHBoxLayout;
    knobs->setSpacing(0);
    auto* comp = new Knob(sp.comp, "Comp.", orange, this);
    comp->setToolTip("Compressor (auto make-up). Right click: detailed parameters, Ctrl+right click: undo");
    comp->setBadge([s] { return params().strip[s].comp_.prg->on() ? QStringLiteral("PRG") : QString(); });
    comp->setRightClick([this, s](QMouseEvent* e) {
        if (e->modifiers() & Qt::ControlModifier) params().undoComp(s);
        else openCompGateDetail(this, s);
    });
    auto* gate = new Knob(sp.gate, "Gate", accent, this);
    gate->setToolTip("Noise gate. Right click: detailed parameters, Ctrl+right click: undo");
    gate->setBadge([s] { return params().strip[s].gate_.prg->on() ? QStringLiteral("PRG") : QString(); });
    gate->setRightClick([this, s](QMouseEvent* e) {
        if (e->modifiers() & Qt::ControlModifier) params().undoGate(s);
        else openCompGateDetail(this, s);
    });
    auto* den = new Knob(sp.denoiser, "Denoiser", meterGreen, this);
    den->setToolTip("Noise floor remover (adds a little latency when not zero).\nRight click: denoiser & Voice Modeler");
    den->setBadge([s] { return params().strip[s].pitch.on->on() ? QStringLiteral("PRG") : QString(); });
    den->setRightClick([this, s](QMouseEvent*) { openDenoiserDetail(this, s); });
    knobs->addWidget(comp);
    knobs->addWidget(gate);
    knobs->addWidget(den);
    v->addLayout(knobs);

    auto* sends = new QHBoxLayout;
    sends->setSpacing(0);
    static const char* const sendNames[4] = {"Rev", "Dly", "Fx1", "Fx2"};
    static const QColor sendCols[4] = {QColor(0xb1, 0x97, 0xfc), QColor(0x4d, 0xab, 0xf7), QColor(0x9a, 0xa5, 0xb1), QColor(0x9a, 0xa5, 0xb1)};
    for (int k = 0; k < 4; ++k) {
        auto* kn = new Knob(sp.send[k], sendNames[k], sendCols[k], this);
        kn->setCompact(true);
        kn->setPostParam(sp.post[k]);
        kn->setToolTip(QStringLiteral("Send to %1 (0..10). Click the label to switch PRE / POST fader (orange = POST)")
                           .arg(k == 0 ? "Reverb" : k == 1 ? "Delay / multiband compressor" : QStringLiteral("external FX %1").arg(k - 1)));
        sends->addWidget(kn);
    }
    v->addLayout(sends);

    auto* lower = new QHBoxLayout;
    lower->setSpacing(2);
    meter_ = new LevelMeter(2, [s](int c) {
        Meters& m = ctx().mixer->meters();
        return Meters::take(params().opt.meterPost->on() ? m.stripPost[s][c] : m.stripPre[s][c]);
    }, this);
    meter_->setLimit(sp.limit, sp.limType, 12);
    meter_->setGainReduction([s] { return ctx().mixer->meters().stripLimGain[s].load(); });
    meter_->setFixedWidth(12);
    meter_->setToolTip("Input level. Click to set the strip limiter, right click to type it,\nCtrl+click: yellow / green (5 ms look-ahead) limiter.");
    lower->addWidget(meter_);
    lower->addWidget(new StripFader(s, this), 1);
    auto* col = new QVBoxLayout;
    col->setSpacing(1);
    addBusButtons(this, s, col);
    col->addStretch(1);
    auto* mono = new ToggleButton(sp.mono, "Mono", theme::mono, this);
    auto* solo = new ToggleButton(sp.solo, "Solo", theme::solo, this);
    auto* mute = new ToggleButton(sp.mute, "Mute", theme::mute, this);
    for (auto* b : {mono, solo, mute}) { b->setFixedHeight(19); col->addWidget(b); }
    lower->addLayout(col);
    v->addLayout(lower, 5);
    refreshDevices();
}

void HwStripWidget::showPadMenu(QPoint pos) {
    QMenu menu(this);
    const char* names[] = {"Color Panel", "Modulation", "3D Panel"};
    for (int i = 0; i < 3; ++i) {
        QAction* a = menu.addAction(names[i]);
        a->setCheckable(true);
        a->setChecked(pad_->page() == i);
        connect(a, &QAction::triggered, this, [this, i] { pad_->setPage(i); });
    }
    menu.addSeparator();
    connect(menu.addAction("Reset this panel"), &QAction::triggered, this, [this] {
        pad_->currentPage().x->reset(); pad_->currentPage().y->reset();
    });
    connect(menu.addAction("Strip menu..."), &QAction::triggered, this, [this, pos] { showStripMenu(this, index_, pos); });
    menu.exec(pos);
}

void HwStripWidget::tick() { meter_->tick(); }

void HwStripWidget::refreshDevices() {
    bool ok = false;
    const QString n = deviceDisplayName(params().str(sk(index_, "device")), &ok);
    device_->setDevice(n, ok);
}

// ------------------------------------------------------------------ virtual strip
VirtStripWidget::VirtStripWidget(int s, QWidget* parent) : StripBase(s, parent) {
    ParamStore& P = params();
    auto& sp = P.strip[s];
    const int v = s - kHwStrips;
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(2);

    header_ = new DeviceButton("VIRTUAL", this);
    header_->setBadge(QStringLiteral("V%1").arg(v + 1), virtInColor);
    header_->setAccent(virtInColor);
    header_->setToolTip("Applications that play on this device are mixed here. Click for options.");
    header_->setFixedHeight(40);
    connect(header_, &DeviceButton::clicked, this, &VirtStripWidget::showHeaderMenu);
    connect(header_, &DeviceButton::rightClicked, this, &VirtStripWidget::showHeaderMenu);
    lay->addWidget(header_);

    static const char* const ph[kVirtStrips] = {"RAIO", "AUX", "RAIO3"};
    auto* label = new EditableLabel(sk(s, "label"), ph[v], this);
    connect(label, &EditableLabel::rightClicked, this, [this](QPoint p) { showStripMenu(this, index_, p); });
    lay->addWidget(label);

    auto* cap = new ToggleButton(nullptr, "EQUALIZER", textDim, this);
    cap->setFixedHeight(15);
    cap->setToolTip("Click / right click: strip menu (reset, copy, paste, load, save)");
    cap->setMomentary([this] { showStripMenu(this, index_, QCursor::pos()); });
    connect(cap, &ToggleButton::rightClicked, this, [this](QPoint p) { showStripMenu(this, index_, p); });
    lay->addWidget(cap);

    auto* knobs = new QHBoxLayout;
    knobs->setSpacing(0);
    knobs->addWidget(new Knob(sp.eq3[2], "Treble", accent, this));
    knobs->addWidget(new Knob(sp.eq3[1], "Medium", accent, this));
    knobs->addWidget(new Knob(sp.eq3[0], "Bass", accent, this));
    lay->addLayout(knobs);

    pad_ = new Pad2D(this);
    pad_->addPage({sp.panX, sp.panY, "5.1", QColor(0x3f, 0x4a, 0x2a), QColor(0x22, 0x2c, 0x18), "", "", "", "", true});
    pad_->setOverlayText([s] {
        const int m = params().strip[s].mode->i();
        return m == 0 ? QString() : QString(kStripModeNames[m]);
    });
    pad_->setMinimumHeight(44);
    pad_->setToolTip("5.1 pan pot: drag, double click to reset.\nRight click: channel mode (mono, repeat, mix down, up mix...)");
    connect(pad_, &Pad2D::contextMenuRequested, this, &VirtStripWidget::showPanMenu);
    lay->addWidget(pad_, 1);

    apps_ = new AppStripList(s, this);
    lay->addWidget(apps_);

    auto* lower = new QHBoxLayout;
    lower->setSpacing(2);
    meter_ = new LevelMeter(P.opt.vinCh[v]->i() > 2 ? 8 : 2, [s](int c) {
        Meters& m = ctx().mixer->meters();
        return Meters::take(params().opt.meterPost->on() ? m.stripPost[s][c] : m.stripPre[s][c]);
    }, this);
    meter_->setLimit(sp.limit, sp.limType, 12);
    meter_->setGainReduction([s] { return ctx().mixer->meters().stripLimGain[s].load(); });
    meter_->setFixedWidth(12);
    lower->addWidget(meter_);
    lower->addWidget(new StripFader(s, this), 1);
    auto* col = new QVBoxLayout;
    col->setSpacing(1);
    addBusButtons(this, s, col);
    col->addStretch(1);
    auto* mc = new ToggleButton(sp.mc, "M.C", orange, this);
    mc->setToolTip("Mute centre channel (movie dialogs)");
    mc->setFixedHeight(19);
    col->addWidget(mc);
    if (v == 1) {
        auto* k = new ToggleButton(sp.karaoke, "K", orange, this);
        k->setStates({"K", "Km", "K1", "K2", "Kv"}, {btnOff, orange, orange, orange, orange});
        k->setToolTip("Karaoke voice removal:\nKm removes the common (centre) part\nK1 / K2 keep more bass and treble\nKv filters the 200-4000 Hz voice band");
        k->setFixedHeight(19);
        col->addWidget(k);
    }
    auto* solo = new ToggleButton(sp.solo, "Solo", theme::solo, this);
    auto* mute = new ToggleButton(sp.mute, "Mute", theme::mute, this);
    for (auto* b : {solo, mute}) { b->setFixedHeight(19); col->addWidget(b); }
    lower->addLayout(col);
    lay->addLayout(lower, 5);
    refreshDevices();
}

void VirtStripWidget::showHeaderMenu(QPoint pos) {
    const int v = index_ - kHwStrips;
    QMenu menu(this);
    menu.addSection(QString::fromStdString(Engine::virtualInputDescription(v)));
    connect(menu.addAction("Set as default system playback device"), &QAction::triggered, this,
            [v] { if (ctx().engine) ctx().engine->setSystemDefault(v); });
    connect(menu.addAction("Application Mixer..."), &QAction::triggered, this, [this] { openAppMixer(this); });
    QMenu* chans = menu.addMenu("Channels");
    for (int n : {2, 6, 8}) {
        QAction* a = chans->addAction(n == 2 ? "Stereo (2)" : n == 6 ? "5.1 (6)" : "7.1 (8)");
        a->setCheckable(true);
        a->setChecked(params().opt.vinCh[v]->i() == n);
        connect(a, &QAction::triggered, this, [this, v, n] {
            params().opt.vinCh[v]->set(float(n));
            params().routingRevision.fetch_add(1);
            meter_->setChannels(n > 2 ? 8 : 2);
        });
    }
    menu.addSeparator();
    connect(menu.addAction("Strip menu..."), &QAction::triggered, this, [this, pos] { showStripMenu(this, index_, pos); });
    menu.exec(pos);
}

void VirtStripWidget::showPanMenu(QPoint pos) {
    QMenu menu(this);
    Param* mode = params().strip[index_].mode;
    for (int i = 0; i < int(StripMode::Count); ++i) {
        QAction* a = menu.addAction(kStripModeNames[i]);
        a->setCheckable(true);
        a->setChecked(mode->i() == i);
        connect(a, &QAction::triggered, this, [mode, i] { mode->set(float(i)); });
        if (i == 0 || i == 1 || i == 5 || i == 9) menu.addSeparator();
    }
    menu.exec(pos);
}

void VirtStripWidget::tick() { meter_->tick(); }

void VirtStripWidget::refreshDevices() {
    const int v = index_ - kHwStrips;
    // only flag a missing device while the engine is running
    const bool connected = !ctx().engine || !ctx().engine->running() || ctx().engine->deviceConnected(Engine::virtualInputName(v));
    header_->setDevice(QString::fromStdString(Engine::virtualInputDescription(v)).remove("RoadBoard "), connected);
    header_->setToolTip(QString::fromStdString(Engine::virtualInputDescription(v)) +
                        ": applications that play on this device are mixed here. Click for options.");
}

// ------------------------------------------------------------------ bus strip
BusStripWidget::BusStripWidget(int b, QWidget* parent) : StripBase(b, parent) {
    ParamStore& P = params();
    auto& bp = P.bus[b];
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(3, 3, 3, 3);
    lay->setSpacing(2);
    const bool hwBus = b < kHwBuses;
    header_ = new DeviceButton(hwBus ? "OUTPUT" : "VIRTUAL", this);
    header_->setBadge(kBusNames[b], hwBus ? busA : busB);
    header_->setAccent(hwBus ? busA : busB);
    header_->setFixedHeight(40);
    if (hwBus) {
        header_->setToolTip("Click to choose the speakers / headphones / sound card of this output");
        connect(header_, &DeviceButton::clicked, this, [this](QPoint p) { showOutputDeviceMenu(this, index_, p); });
    } else {
        header_->setToolTip("Virtual microphone: choose it as input in Discord, OBS, Zoom... Click for options.");
        auto menu = [this](QPoint p) {
            const int v = index_ - kHwBuses;
            QMenu m(this);
            connect(m.addAction("Set as default system recording device"), &QAction::triggered, this,
                    [v] { if (ctx().engine) ctx().engine->setSystemDefaultSource(v); });
            QMenu* chans = m.addMenu("Channels");
            for (int n : {1, 2, 6, 8}) {
                QAction* a = chans->addAction(QString::number(n));
                a->setCheckable(true);
                a->setChecked(params().opt.voutCh[v]->i() == n);
                connect(a, &QAction::triggered, this, [v, n] { params().opt.voutCh[v]->set(float(n)); params().routingRevision.fetch_add(1); });
            }
            m.exec(p);
        };
        connect(header_, &DeviceButton::clicked, this, menu);
        connect(header_, &DeviceButton::rightClicked, this, menu);
    }
    lay->addWidget(header_);
    auto* top = new QHBoxLayout;
    top->setSpacing(2);
    auto* label = new EditableLabel("bus[" + std::to_string(b) + "].label", "name", this);
    label->setFixedHeight(16);
    label->setToolTip("Double click to give this output a name (e.g. Headphones, Stream, Discord)");
    top->addWidget(label, 1);
    auto* sel = new ToggleButton(bp.sel, "SEL", title, this);
    sel->setFixedSize(28, 16);
    sel->setToolTip("SEL: strip faders show / edit the sub-mix of this BUS (Ctrl+click: several BUS).\n"
                    "With 'Monitor on SEL' the selected BUS is heard on the monitoring BUS.\nRight click: monitoring options");
    sel->setMomentary([b] {
        ParamStore& PP = params();
        const bool multi = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
        const bool was = PP.bus[b].sel->on();
        if (!multi) for (int k = 0; k < kBuses; ++k) PP.bus[k].sel->set(0);
        PP.bus[b].sel->set(was && !multi ? 0 : (multi ? !was : 1));
    });
    connect(sel, &ToggleButton::rightClicked, this, &BusStripWidget::showSelMenu);
    top->addWidget(sel);
    lay->addLayout(top);
    auto* mode = new ToggleButton(nullptr, "", textDim, this);
    mode->setTextFn([b] {
        QString t = kBusModeNames[params().bus[b].mode->i()];
        if (params().bus[b].monitor->on()) t = "MON · " + t;
        return t;
    });
    mode->setMomentary([this] { showModeMenu(QCursor::pos()); });
    connect(mode, &ToggleButton::rightClicked, this, &BusStripWidget::showModeMenu);
    mode->setFixedHeight(17);
    mode->setToolTip("BUS mode (click to change)");
    lay->addWidget(mode);

    auto* row = new QHBoxLayout;
    row->setSpacing(2);
    meter_ = new LevelMeter(b < kHwBuses ? 8 : 2, [b](int c) { return Meters::take(ctx().mixer->meters().bus[b][c]); }, this);
    meter_->setLimit(bp.limit, bp.limType, 0);
    meter_->setGainReduction([b] { return ctx().mixer->meters().busLimGain[b].load(); });
    meter_->setFixedWidth(b < kHwBuses ? 18 : 10);
    meter_->setToolTip("BUS level. Click to set the BUS limiter (-20..0 dB), Ctrl+click: yellow/green limiter");
    row->addWidget(meter_);
    row->addWidget(new Fader(bp.gain, this), 1);
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    auto* mono = new ToggleButton(bp.mono, "Mono", theme::mono, this);
    mono->setStates({"Mono", "Mono", "Rev."}, {btnOff, theme::mono, orange});
    mono->setToolTip("Mono, click again for stereo reverse (L/R swap)");
    auto* eq = new ToggleButton(bp.eqOn, "EQ", busA, this);
    eq->setToolTip("Parametric EQ on/off. Right click to open the EQ (6 cells x 8 channels)");
    eq->setIndicator([b] {
        std::vector<QColor> c;
        auto& PP = params();
        const int ab = PP.bus[b].eqAB->i();
        for (int ch = 0; ch < kBusCh; ++ch) {
            const auto& e = PP.bus[b].eq[ab][ch];
            bool eqd = e.trim->get() != 0;
            for (auto& cell : e.cell) eqd |= cell.on->on();
            const bool del = e.delay->get() > 0;
            c.push_back(eqd && del ? meterYellow : eqd ? meterGreen : del ? meterRed : bgDeep);
        }
        return c;
    });
    connect(eq, &ToggleButton::rightClicked, this, [this, b] { openBusEqDialog(this, b); });
    auto* mute = new ToggleButton(bp.mute, "Mute", theme::mute, this);
    for (auto* w : {static_cast<QWidget*>(mono), static_cast<QWidget*>(eq), static_cast<QWidget*>(mute)}) {
        w->setFixedHeight(20);
        col->addWidget(w);
    }
    col->addStretch(1);
    row->addLayout(col);
    lay->addLayout(row, 1);

    // FX returns
    auto* rets = new QGridLayout;
    rets->setSpacing(0);
    static const char* const rn[4] = {"Rev", "Dly", "Fx1", "Fx2"};
    for (int k = 0; k < 4; ++k) {
        auto* kn = new Knob(bp.ret[k], rn[k], k < 2 ? QColor(0xb1, 0x97, 0xfc) : QColor(0x9a, 0xa5, 0xb1), this);
        kn->setCompact(true);
        kn->setToolTip(QStringLiteral("%1 return level on this BUS (0..10)").arg(k == 0 ? "Reverb" : k == 1 ? "Delay" : QStringLiteral("External FX %1").arg(k - 1)));
        rets->addWidget(kn, k / 2, k % 2);
    }
    lay->addLayout(rets);
    refreshDevices();
}

void BusStripWidget::showSelMenu(QPoint pos) {
    QMenu menu(this);
    QAction* mon = menu.addAction("Use this BUS as monitoring BUS (Monitor on SEL)");
    mon->setCheckable(true);
    mon->setChecked(params().bus[index_].monitor->on());
    connect(mon, &QAction::toggled, this, [this](bool on) {
        for (int k = 0; k < kBuses; ++k) params().bus[k].monitor->set(0);
        params().bus[index_].monitor->set(on ? 1 : 0);
        if (on) params().opt.monitorOnSel->set(1);
    });
    QAction* opt = menu.addAction("Monitor on SEL enabled");
    opt->setCheckable(true);
    opt->setChecked(params().opt.monitorOnSel->on());
    connect(opt, &QAction::toggled, this, [](bool on) { params().opt.monitorOnSel->set(on ? 1 : 0); });
    QAction* rel = menu.addAction("Slider mode: RELATIVE (keep sub-mix differences)");
    rel->setCheckable(true);
    rel->setChecked(params().opt.sliderMode->on());
    connect(rel, &QAction::toggled, this, [](bool on) { params().opt.sliderMode->set(on ? 1 : 0); });
    menu.exec(pos);
}

void BusStripWidget::showModeMenu(QPoint pos) {
    QMenu menu(this);
    Param* mode = params().bus[index_].mode;
    for (int i = 0; i < int(BusMode::Count); ++i) {
        QAction* a = menu.addAction(kBusModeNames[i]);
        a->setCheckable(true);
        a->setChecked(mode->i() == i);
        connect(a, &QAction::triggered, this, [mode, i] { mode->set(float(i)); });
        if (i == 0 || i == 4) menu.addSeparator();
    }
    menu.addSeparator();
    connect(menu.addAction("Composite patch..."), &QAction::triggered, this, [this] { openSystemSettings(this); });
    menu.exec(pos);
}

void BusStripWidget::tick() { meter_->tick(); }

void BusStripWidget::refreshDevices() {
    if (index_ < kHwBuses) {
        bool ok = false;
        const QString n = deviceDisplayName(params().str("bus[" + std::to_string(index_) + "].device"), &ok);
        header_->setDevice(n, ok);
    } else {
        const int v = index_ - kHwBuses;
        header_->setDevice(QString::fromStdString(Engine::virtualOutputDescription(v)).remove("RoadBoard "),
                           !ctx().engine || !ctx().engine->running() || ctx().engine->deviceConnected(Engine::virtualOutputName(v)));
    }
}


} // namespace road
