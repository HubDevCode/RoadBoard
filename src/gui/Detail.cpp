#include "gui/Detail.h"

#include <QDialog>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

#include "core/Settings.h"
#include "engine/Mixer.h"
#include "gui/AppContext.h"
#include "gui/Theme.h"
#include "gui/Widgets.h"

namespace road {

using namespace theme;

namespace {

QString stripTitle(int s) {
    const QString label = QString::fromStdString(params().str("strip[" + std::to_string(s) + "].label"));
    return label.isEmpty() ? QStringLiteral("Input %1").arg(s + 1) : label;
}

// compressor transfer curve + live level
class TransferGraph : public QWidget {
public:
    explicit TransferGraph(int strip, QWidget* parent) : QWidget(parent), s_(strip) { setMinimumSize(260, 200); }
    void tick() {
        const float in = Meters::take(ctx().mixer->meters().stripCompIn[s_]);
        level_ = std::max(in > 1e-6f ? 20 * std::log10(in) : -80.0f, level_ - 1.5f);
        gr_ = ctx().mixer->meters().stripComp[s_].load();
        gate_ = ctx().mixer->meters().stripGate[s_].load();
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        g.fillRect(rect(), bgDeep);
        const QRectF p = QRectF(rect()).adjusted(30, 8, -40, -22);
        auto X = [&](double db) { return p.left() + (db + 60) / 60 * p.width(); };
        auto Y = [&](double db) { return p.bottom() - (db + 60) / 60 * p.height(); };
        g.setFont(mkFont(9));
        for (int db = -60; db <= 0; db += 12) {
            g.setPen(QPen(border, 1));
            g.drawLine(QPointF(X(db), p.top()), QPointF(X(db), p.bottom()));
            g.drawLine(QPointF(p.left(), Y(db)), QPointF(p.right(), Y(db)));
            g.setPen(textDim);
            g.drawText(QRectF(X(db) - 14, p.bottom() + 3, 28, 14), Qt::AlignCenter, QString::number(db));
            g.drawText(QRectF(0, Y(db) - 7, 26, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(db));
        }
        const auto& c = params().strip[s_].comp_;
        const auto& gt = params().strip[s_].gate_;
        const float ratio = c.ratio->get(), thr = c.threshold->get(), knee = c.knee->get() * 20, gi = c.gainIn->get();
        const float slope = 1 - 1 / ratio;
        const float makeup = c.makeUp->on() ? -thr * slope * 0.6f : 0;
        const float go = c.gainOut->get() + makeup;
        QPainterPath path;
        for (int i = 0; i <= 120; ++i) {
            const float in = -60 + i * 0.5f;
            const float x = in + gi;
            const float over = x - thr;
            float gr = 0;
            if (knee > 0.01f && std::fabs(over) <= knee / 2) { const float t = over + knee / 2; gr = slope * t * t / (2 * knee); }
            else if (over > 0) gr = slope * over;
            float out = x - gr + go;
            if (in < gt.threshold->get() && gt.threshold->get() > -59.9f)
                out = gt.damping->get() <= -59.9f ? -60 : out + gt.damping->get();
            const QPointF pt(X(in), Y(std::clamp(out, -60.0f, 0.0f)));
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        g.setPen(QPen(textDim, 1, Qt::DashLine));
        g.drawLine(QPointF(X(-60), Y(-60)), QPointF(X(0), Y(0)));
        g.setPen(QPen(orange, 2));
        g.drawPath(path);
        if (gt.threshold->get() > -59.9f) {
            g.setPen(QPen(accent, 1, Qt::DotLine));
            g.drawLine(QPointF(X(gt.threshold->get()), p.top()), QPointF(X(gt.threshold->get()), p.bottom()));
        }
        // live input level
        g.setPen(Qt::NoPen);
        g.setBrush(meterGreen);
        g.drawRect(QRectF(p.left(), p.bottom() + 16, std::max(0.0, X(std::clamp(level_, -60.0f, 0.0f)) - p.left()), 4));
        // gain reduction bar (red, right side) + gate state
        const double grH = std::min(1.0, gr_ / 24.0) * p.height();
        g.setBrush(meterRed);
        g.drawRect(QRectF(p.right() + 10, p.top(), 8, grH));
        g.setBrush(gate_ > 0.5f ? meterGreen : meterRed);
        g.drawEllipse(QPointF(p.right() + 28, p.top() + 6), 5, 5);
        g.setPen(textDim);
        g.drawText(QRectF(p.right() + 4, p.bottom() - 12, 36, 12), Qt::AlignCenter, "GR");
    }
private:
    int s_;
    float level_ = -80, gr_ = 0, gate_ = 1;
};

QWidget* knobGrid(QWidget* parent, const std::vector<std::tuple<Param*, QString, QString, int>>& knobs, QColor color, int cols) {
    auto* w = new QWidget(parent);
    auto* g = new QGridLayout(w);
    g->setSpacing(2);
    g->setContentsMargins(0, 0, 0, 0);
    int i = 0;
    for (auto& [p, label, unit, dec] : knobs) {
        auto* k = new Knob(p, label, color, w);
        k->setUnit(unit);
        k->setDecimals(dec);
        k->setMinimumSize(54, 70);
        g->addWidget(k, i / cols, i % cols);
        ++i;
    }
    return w;
}

class CompGateDialog : public QDialog {
public:
    CompGateDialog(QWidget* parent, int s) : QDialog(parent), s_(s) {
        setWindowTitle("Compressor / Gate — " + stripTitle(s));
        auto& sp = params().strip[s];
        auto* v = new QVBoxLayout(this);
        graph_ = new TransferGraph(s, this);
        v->addWidget(graph_, 1);
        auto* compHead = new QHBoxLayout;
        compHead->addWidget(new Caption("COMPRESSOR", orange, 11, Qt::AlignLeft | Qt::AlignVCenter, this), 1);
        auto* makeup = new ToggleButton(sp.comp_.makeUp, "Auto make-up", orange, this);
        makeup->setFixedSize(100, 20);
        compHead->addWidget(makeup);
        v->addLayout(compHead);
        v->addWidget(knobGrid(this, {{sp.comp_.gainIn, "Input gain", " dB", 1}, {sp.comp_.ratio, "Ratio", ":1", 1},
                                     {sp.comp_.threshold, "Threshold", " dB", 1}, {sp.comp_.attack, "Attack", " ms", 0},
                                     {sp.comp_.release, "Release", " ms", 0}, {sp.comp_.knee, "Knee", "", 2},
                                     {sp.comp_.gainOut, "Output gain", " dB", 1}}, orange, 7));
        v->addWidget(new Caption("GATE", accent, 11, Qt::AlignLeft | Qt::AlignVCenter, this));
        v->addWidget(knobGrid(this, {{sp.gate_.threshold, "Threshold", " dB", 1}, {sp.gate_.damping, "Damping max", " dB", 1},
                                     {sp.gate_.bpSidechain, "BP sidechain", " Hz", 0}, {sp.gate_.attack, "Attack", " ms", 0},
                                     {sp.gate_.hold, "Hold", " ms", 0}, {sp.gate_.release, "Release", " ms", 0}}, accent, 7));
        v->addWidget(new QLabel("Gate threshold at -60 dB = gate off · damping at -60 dB = full closing (-inf) · BP sidechain below 100 Hz = off.\n"
                                "Moving the Comp / Gate knob on the strip resets these values (Ctrl+right click on the knob = undo)."));
        connect(&t_, &QTimer::timeout, this, [this] { graph_->tick(); });
        t_.start(40);
        resize(620, 560);
    }
private:
    int s_;
    TransferGraph* graph_;
    QTimer t_;
};

// ------------------------------------------------------------------ voice modeler presets
struct VoicePreset { QString name; float dw, pitch, lo, med, hi; };

QString presetFile() { return Settings::configDir() + "/voice_presets.json"; }

std::vector<VoicePreset> loadPresets() {
    std::vector<VoicePreset> p = {{"Deep voice", 100, -4, -3, -2, -2}, {"Monster", 100, -9, -6, -4, -3},
                                  {"Chipmunk", 100, 8, 5, 5, 4}, {"Child", 100, 5, 3, 4, 3},
                                  {"Robot (detune)", 0, 0.3f, 0, 3, -4}, {"Radio", 100, 0, -12, 6, -12},
                                  {"Octave down", 0, -12, 0, 0, 0}, {"Octave up", 0, 12, 0, 0, 0}};
    QFile f(presetFile());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonArray a = QJsonDocument::fromJson(f.readAll()).array();
        for (int i = 0; i < std::min<int>(8, int(a.size())); ++i) {
            const QJsonObject o = a[i].toObject();
            p[size_t(i)] = {o["name"].toString(), float(o["dw"].toDouble()), float(o["pitch"].toDouble()),
                            float(o["lo"].toDouble()), float(o["med"].toDouble()), float(o["hi"].toDouble())};
        }
    }
    return p;
}

void savePresets(const std::vector<VoicePreset>& p) {
    QJsonArray a;
    for (auto& x : p) a.append(QJsonObject{{"name", x.name}, {"dw", x.dw}, {"pitch", x.pitch}, {"lo", x.lo}, {"med", x.med}, {"hi", x.hi}});
    QFile f(presetFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(a).toJson());
}

class DenoiserDialog : public QDialog {
public:
    DenoiserDialog(QWidget* parent, int s) : QDialog(parent), s_(s) {
        setWindowTitle("Denoiser / Voice Modeler — " + stripTitle(s));
        auto& sp = params().strip[s];
        auto* v = new QVBoxLayout(this);
        auto* dh = new QHBoxLayout;
        dh->addWidget(new Caption("NOISE FLOOR REMOVER", meterGreen, 11, Qt::AlignLeft | Qt::AlignVCenter, this), 1);
        auto* byp = new ToggleButton(sp.denoiseBypass, "Bypass", theme::mute, this);
        byp->setFixedSize(70, 20);
        dh->addWidget(byp);
        v->addLayout(dh);
        v->addWidget(knobGrid(this, {{sp.denoiseThreshold, "Amount", "", 1}}, meterGreen, 6));

        auto* vh = new QHBoxLayout;
        vh->addWidget(new Caption("VOICE MODELER", orange, 11, Qt::AlignLeft | Qt::AlignVCenter, this), 1);
        auto* en = new ToggleButton(sp.pitch.on, "Enable Fx", orange, this);
        en->setFixedSize(80, 20);
        vh->addWidget(en);
        v->addLayout(vh);
        group_.key = "local.formantgroup";
        group_.min = -12; group_.max = 12; group_.def = 0;
        group_.v.store((sp.pitch.lo->get() + sp.pitch.med->get() + sp.pitch.hi->get()) / 3);
        lastGroup_ = group_.get();
        v->addWidget(knobGrid(this, {{sp.pitch.dryWet, "Dry / Wet", " %", 0}, {sp.pitch.value, "Pitch", " st", 1},
                                     {sp.pitch.lo, "Formant Lo", " st", 1}, {sp.pitch.med, "Formant Med", " st", 1},
                                     {sp.pitch.hi, "Formant Hi", " st", 1}, {&group_, "Formant group", " st", 1}}, orange, 6));
        v->addWidget(new QLabel("Presets: left click to recall, right click to store / rename."));
        auto* pr = new QHBoxLayout;
        for (int i = 0; i < 8; ++i) {
            auto* b = new ToggleButton(nullptr, "", textDim, this);
            b->setTextFn([i] { return QStringLiteral("%1. %2").arg(i + 1).arg(loadPresetsCached()[size_t(i)].name); });
            b->setMomentary([this, i] { recallVoicePreset(s_, i); });
            b->setFixedHeight(24);
            connect(b, &ToggleButton::rightClicked, this, [this, i](QPoint p) { presetMenu(i, p); });
            pr->addWidget(b);
        }
        v->addLayout(pr);
        connect(&t_, &QTimer::timeout, this, [this] {
            auto& pt = params().strip[s_].pitch;
            const float g = group_.get();
            if (g != lastGroup_) {
                const float d = g - lastGroup_;
                pt.lo->set(pt.lo->get() + d); pt.med->set(pt.med->get() + d); pt.hi->set(pt.hi->get() + d);
            } else {
                group_.v.store((pt.lo->get() + pt.med->get() + pt.hi->get()) / 3);
            }
            lastGroup_ = group_.get();
        });
        t_.start(50);
        resize(760, 380);
    }
    static std::vector<VoicePreset>& loadPresetsCached() {
        static std::vector<VoicePreset> p = loadPresets();
        return p;
    }
private:
    void presetMenu(int i, QPoint pos) {
        QMenu m(this);
        connect(m.addAction("Store current settings here..."), &QAction::triggered, this, [this, i] {
            bool ok = false;
            auto& all = loadPresetsCached();
            const QString name = QInputDialog::getText(this, "Voice preset", "Name:", QLineEdit::Normal, all[size_t(i)].name, &ok);
            if (!ok) return;
            auto& pt = params().strip[s_].pitch;
            all[size_t(i)] = {name, pt.dryWet->get(), pt.value->get(), pt.lo->get(), pt.med->get(), pt.hi->get()};
            savePresets(all);
        });
        m.exec(pos);
    }
    int s_;
    Param group_;
    float lastGroup_ = 0;
    QTimer t_;
};

template <typename T>
void openFor(QWidget* parent, int s, T** table) {
    if (!table[s]) {
        table[s] = new T(parent, s);
        table[s]->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(table[s], &QObject::destroyed, [table, s] { table[s] = nullptr; });
    }
    table[s]->show();
    table[s]->raise();
    table[s]->activateWindow();
}

CompGateDialog* gComp[kStrips] = {};
DenoiserDialog* gDen[kStrips] = {};

} // namespace

void openCompGateDetail(QWidget* parent, int strip) { openFor(parent, strip, gComp); }
void openDenoiserDetail(QWidget* parent, int strip) { openFor(parent, strip, gDen); }

bool recallVoicePreset(int strip, int preset) {
    if (strip < 0 || strip >= kHwStrips || preset < 0 || preset >= 8) return false;
    const auto& p = DenoiserDialog::loadPresetsCached()[size_t(preset)];
    auto& pt = params().strip[strip].pitch;
    pt.dryWet->set(p.dw); pt.value->set(p.pitch); pt.lo->set(p.lo); pt.med->set(p.med); pt.hi->set(p.hi);
    pt.on->set(1);
    return true;
}

} // namespace road
