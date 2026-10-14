#include "gui/FxWindow.h"

#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cmath>

#include "core/Params.h"
#include "engine/Mixer.h"
#include "gui/AppContext.h"
#include "gui/Theme.h"
#include "gui/Widgets.h"

namespace road {

using namespace theme;

namespace {

const QColor kRevColor(0xb1, 0x97, 0xfc);
const QColor kDlyColor(0x4d, 0xab, 0xf7);
const QColor kC5Color(0xf0, 0x8c, 0x00);

Knob* mk(QWidget* parent, Param* p, const QString& label, QColor c, const QString& unit, int dec) {
    auto* k = new Knob(p, label, c, parent);
    k->setUnit(unit);
    k->setDecimals(dec);
    k->setMinimumSize(52, 70);
    return k;
}

// ---------------------------------------------------------------- reverb memory page
QWidget* reverbPage(QWidget* parent, int m) {
    auto& M = params().reverb.m[m];
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel("Preset:"));
    auto* preset = new QComboBox;
    for (int i = 0; i < kReverbPresetCount; ++i) preset->addItem(kReverbPresets[i].name);
    preset->setCurrentIndex(M.preset->i());
    QObject::connect(preset, &QComboBox::currentIndexChanged, w, [m](int i) { params().reverb.m[m].preset->set(float(i)); });
    top->addWidget(preset, 1);
    top->addWidget(new QLabel("Bypass:"));
    auto* byp = new QComboBox;
    byp->addItems({"Off", "All", "EQ only"});
    byp->setCurrentIndex(M.bypass->i());
    QObject::connect(byp, &QComboBox::currentIndexChanged, w, [m](int i) { params().reverb.m[m].bypass->set(float(i)); });
    top->addWidget(byp);
    v->addLayout(top);
    auto* g = new QGridLayout;
    g->setSpacing(2);
    g->addWidget(mk(w, M.dry, "Dry", kRevColor, " dB", 1), 0, 0);
    g->addWidget(mk(w, M.wet, "Wet", kRevColor, " dB", 1), 0, 1);
    g->addWidget(mk(w, M.preDelay, "Pre-delay", kRevColor, " ms", 0), 0, 2);
    g->addWidget(mk(w, M.er, "E.Ref", kRevColor, " %", 0), 0, 3);
    g->addWidget(mk(w, M.decay, "Decay", kRevColor, " %", 0), 0, 4);
    static const char* const bn[3] = {"Low", "Mid", "High"};
    for (int k = 0; k < 3; ++k) {
        g->addWidget(mk(w, M.eqGain[k], QStringLiteral("%1 gain").arg(bn[k]), accent, " dB", 1), 1, k * 2);
        g->addWidget(mk(w, M.eqFreq[k], QStringLiteral("%1 freq").arg(bn[k]), accent, " Hz", 0), 1, k * 2 + 1);
    }
    v->addLayout(g);
    // keep combos in sync with remote changes
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, w, [preset, byp, m] {
        auto& MM = params().reverb.m[m];
        if (preset->currentIndex() != MM.preset->i()) { QSignalBlocker b(preset); preset->setCurrentIndex(MM.preset->i()); }
        if (byp->currentIndex() != MM.bypass->i()) { QSignalBlocker b(byp); byp->setCurrentIndex(MM.bypass->i()); }
    });
    t->start(300);
    return w;
}

// ---------------------------------------------------------------- delay timeline
class TapTimeline : public QWidget {
public:
    TapTimeline(int mem, QWidget* parent) : QWidget(parent), m_(mem) { setMinimumHeight(170); setMouseTracking(true); }
protected:
    double maxMs() const {
        const auto& M = params().delay.m[m_];
        double mx = 2000;
        for (auto& t : M.tap) mx = std::max(mx, double(t.time->get()) * 1.1);
        return std::min(4000.0, std::ceil(mx / 500) * 500);
    }
    QRectF gainArea() const { return QRectF(30, 16, width() - 40, height() * 0.62 - 16); }
    QRectF panArea() const { const QRectF g = gainArea(); return QRectF(g.left(), g.bottom() + 8, g.width(), height() - g.bottom() - 14); }
    double xOf(double ms) const { const QRectF a = gainArea(); return a.left() + ms / maxMs() * a.width(); }
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        g.fillRect(rect(), bgDeep);
        const auto& M = params().delay.m[m_];
        const QRectF ga = gainArea(), pa = panArea();
        const double beat = 60000.0 / M.tempo->get();
        g.setFont(mkFont(9));
        for (int k = 0; xOf(k * beat / 4) <= ga.right() + 1 && k < 400; ++k) {   // 1/16 grid, 4/4 beats on top
            const double x = xOf(k * beat / 4);
            const bool bar = k % 16 == 0, b = k % 4 == 0;
            g.setPen(QPen(b ? (bar ? textDim : border.lighter(130)) : border, 1));
            g.drawLine(QPointF(x, ga.top()), QPointF(x, pa.bottom()));
            if (b) { g.setPen(textDim); g.drawText(QPointF(x + 2, 11), QString::number(k / 4 + 1)); }
        }
        for (int k = 0; xOf(k * beat / 3) <= ga.right() + 1 && k < 300; ++k) {     // 3/4 triplet marks at the bottom
            const double x = xOf(k * beat / 3);
            g.setPen(QPen(kC5Color.darker(160), 1));
            g.drawLine(QPointF(x, pa.bottom() - 4), QPointF(x, pa.bottom()));
        }
        g.setPen(QPen(border, 1));
        g.drawRect(ga);
        g.drawRect(pa);
        g.setPen(textDim);
        g.drawText(QRectF(0, ga.top(), 28, 12), Qt::AlignRight, "0dB");
        g.drawText(QRectF(0, pa.top(), 28, 12), Qt::AlignRight, "pan");
        for (int k = 0; k < kDelayTaps; ++k) {
            const auto& t = M.tap[k];
            const double x = xOf(t.time->get() * M.ratio->get());
            const double y = ga.bottom() - (t.gain->get() + 60) / 66 * ga.height();
            const double py = pa.center().y() + t.pan->get() * pa.height() / 2;
            const QColor c = t.on->on() ? kDlyColor : btnOff;
            g.setPen(QPen(c, 2));
            g.drawLine(QPointF(x, ga.bottom()), QPointF(x, y));
            g.setBrush(c);
            g.setPen(QPen(k == hover_ ? text : bgDeep, 1));
            g.drawEllipse(QPointF(x, y), 7, 7);
            g.drawEllipse(QPointF(x, py), 5, 5);
            g.setPen(bgDeep);
            g.setFont(mkFont(9, true));
            g.drawText(QRectF(x - 7, y - 7, 14, 14), Qt::AlignCenter, QString::number(k + 1));
        }
    }
    int tapAt(QPointF p, bool* pan) const {
        const auto& M = params().delay.m[m_];
        const QRectF ga = gainArea(), pa = panArea();
        for (int k = kDelayTaps - 1; k >= 0; --k) {
            const auto& t = M.tap[k];
            const double x = xOf(t.time->get() * M.ratio->get());
            const double y = ga.bottom() - (t.gain->get() + 60) / 66 * ga.height();
            const double py = pa.center().y() + t.pan->get() * pa.height() / 2;
            if (QLineF(p, QPointF(x, y)).length() < 9) { *pan = false; return k; }
            if (QLineF(p, QPointF(x, py)).length() < 8) { *pan = true; return k; }
        }
        return -1;
    }
    void mousePressEvent(QMouseEvent* e) override {
        bool pan = false;
        drag_ = tapAt(e->position(), &pan);
        dragPan_ = pan;
        if (drag_ >= 0 && e->button() == Qt::RightButton) {
            auto& t = params().delay.m[m_].tap[drag_];
            t.on->set(t.on->on() ? 0 : 1);
            drag_ = -1;
            update();
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (drag_ < 0) { bool p; const int h = tapAt(e->position(), &p); if (h != hover_) { hover_ = h; update(); } return; }
        auto& M = params().delay.m[m_];
        auto& t = M.tap[drag_];
        const QRectF ga = gainArea(), pa = panArea();
        if (dragPan_) t.pan->set(float((e->position().y() - pa.center().y()) / (pa.height() / 2)));
        else {
            double ms = (e->position().x() - ga.left()) / ga.width() * maxMs() / M.ratio->get();
            if (e->modifiers() & Qt::ShiftModifier) {          // snap to 1/16 notes
                const double step = 60000.0 / M.tempo->get() / 4;
                ms = std::round(ms / step) * step;
            }
            t.time->set(float(ms));
            t.gain->set(float((ga.bottom() - e->position().y()) / ga.height() * 66 - 60));
            t.on->set(1);
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { drag_ = -1; }
private:
    int m_, drag_ = -1, hover_ = -1;
    bool dragPan_ = false;
};

QWidget* delayPage(QWidget* parent, int m) {
    auto& M = params().delay.m[m];
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* tl = new TapTimeline(m, w);
    v->addWidget(tl, 1);
    v->addWidget(new QLabel("Drag the numbered points: time / gain (Shift = snap to 1/16), lower lane = pan. Right click a point: tap on/off."));
    auto* row = new QHBoxLayout;
    auto* g = new QGridLayout;
    g->setSpacing(1);
    g->addWidget(mk(w, M.tempo, "Tempo", kDlyColor, " bpm", 1), 0, 0);
    g->addWidget(mk(w, M.ratio, "Time ratio", kDlyColor, "x", 2), 0, 1);
    g->addWidget(mk(w, M.feedback, "Feedback", kDlyColor, " %", 0), 0, 2);
    g->addWidget(mk(w, M.dry, "Dry", kDlyColor, " dB", 1), 0, 3);
    g->addWidget(mk(w, M.wet, "Wet", kDlyColor, " dB", 1), 0, 4);
    g->addWidget(mk(w, M.preDelay, "Pre-delay", kDlyColor, " ms", 0), 0, 5);
    g->addWidget(mk(w, M.balL, "Bal. L in", kDlyColor, "", 2), 0, 6);
    g->addWidget(mk(w, M.balR, "Bal. R in", kDlyColor, "", 2), 0, 7);
    row->addLayout(g);
    auto* btns = new QVBoxLayout;
    auto* tap = new ToggleButton(nullptr, "TAP", title, w);
    auto taps = std::make_shared<std::vector<qint64>>();
    tap->setMomentary([taps, m] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!taps->empty() && now - taps->back() > 2500) taps->clear();
        taps->push_back(now);
        if (taps->size() > 8) taps->erase(taps->begin());
        if (taps->size() >= 4) {
            const double avg = double(taps->back() - taps->front()) / double(taps->size() - 1);
            params().delay.m[m].tempo->set(float(60000.0 / avg));
        }
    });
    tap->setToolTip("Tap 4 times or more in rhythm to set the tempo");
    tap->setFixedSize(84, 24);
    auto* fit = new ToggleButton(nullptr, "FIX DELAY", title, w);
    fit->setToolTip("Quantise every tap to the nearest 1/16 note of the current tempo (time ratio reset to 1)");
    fit->setMomentary([m] {
        auto& MM = params().delay.m[m];
        const double step = 60000.0 / MM.tempo->get() / 4;
        for (auto& t : MM.tap) t.time->set(float(std::max(1.0, std::round(t.time->get() * MM.ratio->get() / step) * step)));
        MM.ratio->set(1);
    });
    fit->setFixedSize(84, 24);
    auto* byp = new ToggleButton(M.bypass, "BYPASS", theme::mute, w);
    byp->setFixedSize(84, 24);
    btns->addWidget(tap);
    btns->addWidget(fit);
    btns->addWidget(byp);
    btns->addStretch(1);
    row->addLayout(btns);
    v->addLayout(row);
    // per tap filters
    auto* taps2 = new QGridLayout;
    taps2->setSpacing(0);
    for (int k = 0; k < kDelayTaps; ++k) {
        auto* on = new ToggleButton(M.tap[k].on, QStringLiteral("Tap %1").arg(k + 1), kDlyColor, w);
        on->setFixedHeight(18);
        taps2->addWidget(on, 0, k);
        auto* lp = new Knob(M.tap[k].lpf, "LPF", accent, w); lp->setCompact(true);
        auto* hp = new Knob(M.tap[k].hpf, "HPF", accent, w); hp->setCompact(true);
        auto* h = new QHBoxLayout; h->setSpacing(0); h->addWidget(hp); h->addWidget(lp);
        taps2->addLayout(h, 1, k);
    }
    v->addLayout(taps2);
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, tl, [tl] { tl->update(); });
    t->start(100);
    return w;
}

// ---------------------------------------------------------------- multiband compressor
QWidget* c5Page(QWidget* parent) {
    auto& C = params().c5;
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* xo = new QHBoxLayout;
    xo->addWidget(new QLabel("Crossovers:"));
    for (int k = 0; k < kC5Bands - 1; ++k) xo->addWidget(mk(w, C.xover[k], QStringLiteral("%1 | %2").arg(k + 1).arg(k + 2), kC5Color, " Hz", 0));
    xo->addStretch(1);
    xo->addWidget(mk(w, C.outGain, "Output", kC5Color, " dB", 1));
    auto* mu = new ToggleButton(C.makeUp, "AUTO MAKE UP", kC5Color, w); mu->setFixedSize(110, 24); xo->addWidget(mu);
    auto* by = new ToggleButton(C.bypass, "BYPASS", theme::mute, w); by->setFixedSize(80, 24); xo->addWidget(by);
    auto* link = new ToggleButton(C.link, "", textDim, w);
    link->setStates({"LINK: OFF", "LINK: ABS", "LINK: REL"}, {btnOff, accent, title});
    link->setFixedSize(90, 24);
    link->setToolTip("How a change on one band is applied to the others");
    xo->addWidget(link);
    v->addLayout(xo);
    auto* g = new QGridLayout;
    g->setSpacing(1);
    static const char* const rows[] = {"Gain", "Threshold", "Ratio", "Attack", "Release", "Knee", "Out gain", "Limit", "Distortion"};
    for (int r = 0; r < 9; ++r) g->addWidget(new QLabel(rows[r]), r + 2, 0);
    for (int b = 0; b < kC5Bands; ++b) {
        auto& B = C.band[b];
        g->addWidget(new Caption(QStringLiteral("BAND %1").arg(b + 1), kC5Color, 10, Qt::AlignCenter, w), 0, b + 1);
        auto* bh = new QHBoxLayout;
        bh->setSpacing(1);
        for (auto [p, t, c] : {std::tuple{B.on, "On", busA}, std::tuple{B.solo, "S", theme::solo}, std::tuple{B.mute, "M", theme::mute}}) {
            auto* tb = new ToggleButton(p, t, c, w);
            tb->setFixedHeight(18);
            bh->addWidget(tb);
        }
        g->addLayout(bh, 1, b + 1);
        Param* ps[9] = {B.gain, B.threshold, B.ratio, B.attack, B.release, B.knee, B.outGain, B.limit, B.dist};
        static const char* const units[9] = {" dB", " dB", ":1", " ms", " ms", "", " dB", " dB", ""};
        static const int dec[9] = {1, 1, 1, 1, 0, 2, 1, 1, 2};
        for (int r = 0; r < 9; ++r) {
            auto* k = new Knob(ps[r], "", kC5Color, w);
            k->setUnit(units[r]); k->setDecimals(dec[r]);
            k->setMinimumSize(48, 50);
            g->addWidget(k, r + 2, b + 1);
        }
    }
    v->addLayout(g);
    // band link
    auto snapshot = std::make_shared<std::vector<float>>();
    auto* t = new QTimer(w);
    QObject::connect(t, &QTimer::timeout, w, [snapshot] {
        auto& CC = params().c5;
        std::vector<float> now;
        for (auto& B : CC.band) for (Param* p : {B.gain, B.threshold, B.ratio, B.attack, B.release, B.knee, B.outGain, B.limit, B.dist}) now.push_back(p->get());
        const int mode = CC.link->i();
        if (mode != 0 && snapshot->size() == now.size()) {
            for (size_t i = 0; i < now.size(); ++i) {
                if (now[i] == (*snapshot)[i]) continue;
                const size_t param = i % 9;
                const float delta = now[i] - (*snapshot)[i];
                for (int b = 0; b < kC5Bands; ++b) {
                    auto& B = CC.band[b];
                    Param* ps[9] = {B.gain, B.threshold, B.ratio, B.attack, B.release, B.knee, B.outGain, B.limit, B.dist};
                    if (size_t(b) * 9 + param == i) continue;
                    ps[param]->set(mode == 1 ? now[i] : ps[param]->get() + delta);
                }
                break;
            }
            now.clear();
            for (auto& B : CC.band) for (Param* p : {B.gain, B.threshold, B.ratio, B.attack, B.release, B.knee, B.outGain, B.limit, B.dist}) now.push_back(p->get());
        }
        *snapshot = now;
    });
    t->start(60);
    return w;
}

class FxWindow : public QWidget {
public:
    explicit FxWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
        setWindowTitle("RoadBoard — Special FX");
        auto& P = params();
        auto* v = new QVBoxLayout(this);

        // reverb
        auto* rb = new QGroupBox("REVERB");
        auto* rv = new QVBoxLayout(rb);
        auto* rh = new QHBoxLayout;
        auto* ron = new ToggleButton(P.reverb.on, "ON", kRevColor, this); ron->setFixedSize(60, 24); rh->addWidget(ron);
        auto* rab = new ToggleButton(P.reverb.ab, "", textDim, this);
        rab->setStates({"Memory A", "Memory B"}, {btnOff, title});
        rab->setFixedSize(90, 24);
        rh->addWidget(rab);
        rh->addWidget(new Caption("Send with the Rev knobs on the hardware strips, return with the Rev knobs on each BUS.", textDim, 10,
                                  Qt::AlignLeft | Qt::AlignVCenter, this), 1);
        rv->addLayout(rh);
        revStack_ = new QStackedWidget;
        revStack_->addWidget(reverbPage(this, 0));
        revStack_->addWidget(reverbPage(this, 1));
        rv->addWidget(revStack_);
        v->addWidget(rb);

        // delay / multiband compressor
        auto* db = new QGroupBox("DELAY / MULTIBAND COMPRESSOR");
        auto* dv = new QVBoxLayout(db);
        auto* dh = new QHBoxLayout;
        auto* don = new ToggleButton(P.delay.on, "ON", kDlyColor, this); don->setFixedSize(60, 24); dh->addWidget(don);
        auto* type = new ToggleButton(P.opt.fxType, "", textDim, this);
        type->setStates({"MultiTap Delay", "5-band Compressor"}, {kDlyColor, kC5Color});
        type->setFixedSize(140, 24);
        dh->addWidget(type);
        auto* dab = new ToggleButton(P.delay.ab, "", textDim, this);
        dab->setStates({"Memory A", "Memory B"}, {btnOff, title});
        dab->setFixedSize(90, 24);
        dh->addWidget(dab);
        dh->addStretch(1);
        dv->addLayout(dh);
        dlyStack_ = new QStackedWidget;
        dlyStack_->addWidget(delayPage(this, 0));
        dlyStack_->addWidget(delayPage(this, 1));
        dlyStack_->addWidget(c5Page(this));
        dv->addWidget(dlyStack_, 1);
        v->addWidget(db, 1);

        connect(&t_, &QTimer::timeout, this, [this] {
            auto& PP = params();
            revStack_->setCurrentIndex(PP.reverb.ab->i());
            dlyStack_->setCurrentIndex(PP.opt.fxType->i() == 1 ? 2 : PP.delay.ab->i());
        });
        t_.start(100);
        resize(900, 900);
    }
private:
    QStackedWidget *revStack_, *dlyStack_;
    QTimer t_;
};

FxWindow* gFx = nullptr;

} // namespace

void openFxWindow(QWidget* parent) {
    if (!gFx) {
        gFx = new FxWindow(parent);
        gFx->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(gFx, &QObject::destroyed, [] { gFx = nullptr; });
    }
    gFx->show();
    gFx->raise();
    gFx->activateWindow();
}

} // namespace road
