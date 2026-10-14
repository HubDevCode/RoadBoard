#include "gui/EqDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <map>

#include "core/Settings.h"
#include "dsp/Dsp.h"
#include "gui/Theme.h"

namespace road {

using namespace theme;

namespace {

// EQ target id: 0..7 = BUS, 100 + i = hardware strip i
constexpr int kStripEq = 100;
bool isStripEq(int t) { return t >= kStripEq; }
Param* eqOnP(int t) { return isStripEq(t) ? params().strip[t - kStripEq].eqOn : params().bus[t].eqOn; }
Param* eqABP(int t) { return isStripEq(t) ? params().strip[t - kStripEq].eqAB : params().bus[t].eqAB; }
int eqNch(int t) { return isStripEq(t) ? kHwStripCh : kBusCh; }
std::atomic<uint32_t>& eqRev(int t) { return isStripEq(t) ? params().stripEqRevision[t - kStripEq] : params().eqRevision[t]; }

ParamStore::EqChan& eqChan(int t, int ch) {
    ParamStore& P = params();
    if (isStripEq(t)) return P.strip[t - kStripEq].eq[P.strip[t - kStripEq].eqAB->i()][std::min(ch, kHwStripCh - 1)];
    return P.bus[t].eq[P.bus[t].eqAB->i()][ch];
}

Settings::KV exportEq(int t) {
    Settings::KV kv;
    for (int c = 0; c < eqNch(t); ++c) {
        const auto& ch = eqChan(t, c);
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

void importEq(int t, const Settings::KV& kv) {
    for (int c = 0; c < eqNch(t); ++c) {
        auto& ch = eqChan(t, c);
        const QString pre = QStringLiteral("channel[%1].").arg(c);
        auto val = [&](const QString& k, Param* p) { auto it = kv.find(pre + k); if (it != kv.end()) p->set(it.value().toFloat()); };
        for (int k = 0; k < kEqCells; ++k) {
            const QString cp = QStringLiteral("cell[%1].").arg(k);
            val(cp + "on", ch.cell[k].on); val(cp + "type", ch.cell[k].type); val(cp + "f", ch.cell[k].f);
            val(cp + "gain", ch.cell[k].gain); val(cp + "q", ch.cell[k].q);
        }
        val("trim", ch.trim); val("delay", ch.delay);
    }
}

void flatEq(int t, int channel) {
    for (int c = 0; c < eqNch(t); ++c) {
        if (channel >= 0 && c != channel) continue;
        auto& ch = eqChan(t, c);
        for (auto& cell : ch.cell) { cell.on->reset(); cell.type->reset(); cell.f->reset(); cell.gain->reset(); cell.q->reset(); }
        ch.trim->reset(); ch.delay->reset();
    }
}

const QColor kCellColors[kEqCells] = {QColor(0xff, 0x6b, 0x6b), QColor(0xff, 0xb3, 0x47), QColor(0xf5, 0xe1, 0x4b),
                                      QColor(0x5d, 0xd3, 0x9e), QColor(0x4d, 0xab, 0xf7), QColor(0xb1, 0x97, 0xfc)};

class EqGraph : public QWidget {
public:
    EqGraph(int bus, QWidget* parent) : QWidget(parent), bus_(bus) {
        setMinimumSize(640, 240);
        setMouseTracking(true);
    }
    void setChannel(int c) { ch_ = c; update(); }
    int channel() const { return std::max(ch_, 0); }
    std::function<void(int cell, std::function<void(ParamStore::EqCell&)>)> editCell;

protected:
    QRectF plot() const { return QRectF(40, 10, width() - 50, height() - 30); }
    double fToX(double f) const { const QRectF p = plot(); return p.left() + std::log10(f / 20.0) / 3.0 * p.width(); }
    double xToF(double x) const { const QRectF p = plot(); return 20.0 * std::pow(10.0, (x - p.left()) / p.width() * 3.0); }
    double dbToY(double db) const { const QRectF p = plot(); return p.center().y() - db / range_ * p.height() / 2; }
    double yToDb(double y) const { const QRectF p = plot(); return (p.center().y() - y) / (p.height() / 2) * range_; }

    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        g.fillRect(rect(), bgDeep);
        const QRectF p = plot();
        g.setFont(mkFont(9));
        for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
            const double x = fToX(f);
            g.setPen(QPen(border, 1));
            g.drawLine(QPointF(x, p.top()), QPointF(x, p.bottom()));
            g.setPen(textDim);
            g.drawText(QRectF(x - 20, p.bottom() + 2, 40, 14), Qt::AlignCenter, f >= 1000 ? QString::number(f / 1000) + "k" : QString::number(f));
        }
        for (double db = -range_; db <= range_ + 0.01; db += range_ / 3) {
            const double y = dbToY(db);
            g.setPen(QPen(db == 0 ? textDim : border, 1));
            g.drawLine(QPointF(p.left(), y), QPointF(p.right(), y));
            g.setPen(textDim);
            g.drawText(QRectF(0, y - 7, 36, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(db, 'f', 0));
        }
        const double fs = 48000;
        auto& ec = eqChan(bus_, channel());
        dsp::BiquadCoeffs coeffs[kEqCells];
        bool on[kEqCells];
        for (int k = 0; k < kEqCells; ++k) {
            on[k] = ec.cell[k].on->on();
            coeffs[k] = dsp::makeBiquad(EqType(ec.cell[k].type->i()), fs, ec.cell[k].f->get(), ec.cell[k].q->get(), ec.cell[k].gain->get());
        }
        // individual cells (thin), then the sum (thick)
        for (int k = 0; k < kEqCells; ++k) {
            if (!on[k]) continue;
            QPainterPath path;
            for (int x = int(p.left()); x <= int(p.right()); x += 2) {
                const double db = dsp::biquadMagDb(coeffs[k], fs, xToF(x));
                const QPointF pt(x, dbToY(std::clamp(db, -range_ * 1.2, range_ * 1.2)));
                if (x == int(p.left())) path.moveTo(pt); else path.lineTo(pt);
            }
            QColor c = kCellColors[k];
            c.setAlpha(110);
            g.setPen(QPen(c, 1));
            g.drawPath(path);
        }
        QPainterPath sum;
        for (int x = int(p.left()); x <= int(p.right()); x += 2) {
            double db = ec.trim->get();
            for (int k = 0; k < kEqCells; ++k) if (on[k]) db += dsp::biquadMagDb(coeffs[k], fs, xToF(x));
            const QPointF pt(x, dbToY(std::clamp(db, -range_ * 1.2, range_ * 1.2)));
            if (x == int(p.left())) sum.moveTo(pt); else sum.lineTo(pt);
        }
        g.setPen(QPen(title, 2.2));
        g.drawPath(sum);
        // handles
        for (int k = 0; k < kEqCells; ++k) {
            const auto& c = ec.cell[k];
            const EqType t = EqType(c.type->i());
            const bool usesGain = t == EqType::Peak || t == EqType::LowShelf || t == EqType::HighShelf;
            const QPointF pt(fToX(c.f->get()), dbToY(usesGain ? c.gain->get() : 0));
            g.setBrush(on[k] ? kCellColors[k] : btnOff);
            g.setPen(QPen(k == hover_ ? text : bgDeep, 1.5));
            g.drawEllipse(pt, 7, 7);
            g.setPen(bgDeep);
            g.setFont(mkFont(9, true));
            g.drawText(QRectF(pt.x() - 7, pt.y() - 7, 14, 14), Qt::AlignCenter, QString::number(k + 1));
        }
        g.setPen(textDim);
        g.drawText(p.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignRight,
                   ch_ < 0 ? QStringLiteral("editing ALL channels (curve of ch 1)") : QStringLiteral("channel %1").arg(ch_ + 1));
    }

    int cellAt(QPointF pos) const {
        auto& ec = eqChan(bus_, channel());
        for (int k = kEqCells - 1; k >= 0; --k) {
            const auto& c = ec.cell[k];
            const EqType t = EqType(c.type->i());
            const bool usesGain = t == EqType::Peak || t == EqType::LowShelf || t == EqType::HighShelf;
            const QPointF pt(fToX(c.f->get()), dbToY(usesGain ? c.gain->get() : 0));
            if (QLineF(pt, pos).length() < 9) return k;
        }
        return -1;
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::RightButton) {
            QMenu m(this);
            for (double r : {6.0, 12.0, 18.0, 24.0, 36.0}) {
                QAction* a = m.addAction(QStringLiteral("±%1 dB scale").arg(r));
                a->setCheckable(true);
                a->setChecked(range_ == r);
                connect(a, &QAction::triggered, this, [this, r] { range_ = r; update(); });
            }
            m.exec(e->globalPosition().toPoint());
            return;
        }
        drag_ = cellAt(e->position());
        if (drag_ >= 0 && editCell) editCell(drag_, [](ParamStore::EqCell& c) { c.on->set(1); });
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (drag_ < 0) {
            const int h = cellAt(e->position());
            if (h != hover_) { hover_ = h; update(); }
            return;
        }
        const double f = std::clamp(xToF(e->position().x()), 20.0, 20000.0);
        const double db = std::clamp(yToDb(e->position().y()), -36.0, 18.0);
        if (editCell) editCell(drag_, [f, db](ParamStore::EqCell& c) {
            c.f->set(float(f));
            const EqType t = EqType(c.type->i());
            if (t == EqType::Peak || t == EqType::LowShelf || t == EqType::HighShelf) c.gain->set(float(db));
        });
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { drag_ = -1; }
    void wheelEvent(QWheelEvent* e) override {
        const int k = cellAt(e->position());
        if (k < 0 || !editCell) return;
        const float mul = e->angleDelta().y() > 0 ? 1.15f : 1 / 1.15f;
        editCell(k, [mul](ParamStore::EqCell& c) { c.q->set(c.q->get() * mul); });
        update();
    }

private:
    int bus_, ch_ = -1, drag_ = -1, hover_ = -1;
    double range_ = 18;
};

class BusEqDialog : public QDialog {
public:
    BusEqDialog(QWidget* parent, int bus) : QDialog(parent), bus_(bus) {
        setWindowTitle(isStripEq(bus) ? QStringLiteral("Input %1 — Parametric EQ (6 cells, stereo)").arg(bus - kStripEq + 1)
                                      : QStringLiteral("BUS %1 — Parametric EQ (6 cells × 8 channels)").arg(kBusNames[bus]));
        auto* v = new QVBoxLayout(this);
        auto* top = new QHBoxLayout;
        auto* on = new QCheckBox("EQ ON");
        on->setChecked(eqOnP(bus)->on());
        connect(on, &QCheckBox::toggled, this, [bus](bool b) { eqOnP(bus)->set(b ? 1 : 0); });
        top->addWidget(on);
        top->addWidget(new QLabel("Channel:"));
        chSel_ = new QComboBox;
        chSel_->addItem("All channels", -1);
        static const char* pos[8] = {"1 FL", "2 FR", "3 FC", "4 LFE", "5 RL", "6 RR", "7 SL", "8 SR"};
        static const char* lr[2] = {"Left", "Right"};
        for (int c = 0; c < eqNch(bus); ++c) chSel_->addItem(isStripEq(bus) ? lr[c] : pos[c], c);
        top->addWidget(chSel_);
        abBtn_ = new QPushButton;
        abBtn_->setCheckable(true);
        connect(abBtn_, &QPushButton::clicked, this, [this, bus] {
            eqABP(bus)->set(eqABP(bus)->on() ? 0 : 1);
            load();
        });
        top->addWidget(abBtn_);
        auto addBtn = [&](const QString& t, std::function<void()> f) {
            auto* b = new QPushButton(t);
            connect(b, &QPushButton::clicked, this, std::move(f));
            top->addWidget(b);
        };
        addBtn("Flat", [this, bus] { flatEq(bus, chSel_->currentData().toInt()); load(); });
        addBtn("Ch copy", [this] { chClip_ = exportEq(bus_); chClipCh_ = graph_->channel(); });
        addBtn("Ch paste", [this] { pasteChannel(); });
        addBtn("Copy all", [this] { allClip() = exportEq(bus_); });
        addBtn("Paste all", [this] { if (!allClip().isEmpty()) { importEq(bus_, allClip()); load(); } });
        addBtn("Load...", [this] {
            const QString f = QFileDialog::getOpenFileName(this, "Load BUS EQ", Settings::configDir(), "BUS EQ (*.xml)");
            Settings::KV kv;
            if (!f.isEmpty() && Settings::loadKV(f, "RoadBoardEq", &kv)) { importEq(bus_, kv); load(); }
        });
        addBtn("Save...", [this] {
            const QString f = QFileDialog::getSaveFileName(this, "Save BUS EQ", Settings::configDir() + "/buseq.xml", "BUS EQ (*.xml)");
            if (!f.isEmpty()) Settings::saveKV(f, "RoadBoardEq", exportEq(bus_));
        });
        top->addStretch(1);
        v->addLayout(top);

        graph_ = new EqGraph(bus, this);
        graph_->editCell = [this](int k, std::function<void(ParamStore::EqCell&)> f) {
            forChannels([&](int c) { f(eqChan(bus_, c).cell[k]); });
            load();
        };
        v->addWidget(graph_, 1);

        auto* grid = new QGridLayout;
        const QStringList rows{"On", "Type", "Freq (Hz)", "Gain (dB)", "Q"};
        for (int r = 0; r < rows.size(); ++r) grid->addWidget(new QLabel(rows[r]), r + 1, 0);
        for (int k = 0; k < kEqCells; ++k) {
            auto* head = new QLabel(QStringLiteral("<b style='color:%1'>Cell %2</b>").arg(kCellColors[k].name()).arg(k + 1));
            grid->addWidget(head, 0, k + 1, Qt::AlignCenter);
            Cell& c = cells_[k];
            c.on = new QCheckBox;
            c.type = new QComboBox;
            for (int t = 0; t < int(EqType::Count); ++t) c.type->addItem(kEqTypeNames[t], t);
            c.f = new QDoubleSpinBox; c.f->setRange(20, 20000); c.f->setDecimals(0);
            c.gain = new QDoubleSpinBox; c.gain->setRange(-36, 18); c.gain->setDecimals(1); c.gain->setSingleStep(0.5);
            c.q = new QDoubleSpinBox; c.q->setRange(0.3, 100); c.q->setDecimals(2); c.q->setSingleStep(0.1);
            grid->addWidget(c.on, 1, k + 1, Qt::AlignCenter);
            grid->addWidget(c.type, 2, k + 1);
            grid->addWidget(c.f, 3, k + 1);
            grid->addWidget(c.gain, 4, k + 1);
            grid->addWidget(c.q, 5, k + 1);
            connect(c.on, &QCheckBox::toggled, this, [this, k](bool b) { if (!loading_) forChannels([&](int ch) { eqChan(bus_, ch).cell[k].on->set(b); }), graph_->update(); });
            connect(c.type, &QComboBox::currentIndexChanged, this, [this, k](int i) { if (!loading_) forChannels([&](int ch) { eqChan(bus_, ch).cell[k].type->set(float(i)); }), graph_->update(); });
            connect(c.f, &QDoubleSpinBox::valueChanged, this, [this, k](double x) { if (!loading_) forChannels([&](int ch) { eqChan(bus_, ch).cell[k].f->set(float(x)); }), graph_->update(); });
            connect(c.gain, &QDoubleSpinBox::valueChanged, this, [this, k](double x) { if (!loading_) forChannels([&](int ch) { eqChan(bus_, ch).cell[k].gain->set(float(x)); }), graph_->update(); });
            connect(c.q, &QDoubleSpinBox::valueChanged, this, [this, k](double x) { if (!loading_) forChannels([&](int ch) { eqChan(bus_, ch).cell[k].q->set(float(x)); }), graph_->update(); });
        }
        trim_ = new QDoubleSpinBox; trim_->setRange(-24, 24); trim_->setDecimals(1); trim_->setSuffix(" dB");
        delay_ = new QDoubleSpinBox; delay_->setRange(0, 500); delay_->setDecimals(1); delay_->setSuffix(" ms");
        grid->addWidget(new QLabel("<b>Channel trim</b>"), 0, kEqCells + 1);
        grid->addWidget(trim_, 1, kEqCells + 1);
        grid->addWidget(new QLabel("<b>Channel delay</b>"), 2, kEqCells + 1);
        grid->addWidget(delay_, 3, kEqCells + 1);
        connect(trim_, &QDoubleSpinBox::valueChanged, this, [this](double x) { if (!loading_) forChannels([&](int ch) { eqChan(bus_, ch).trim->set(float(x)); }), graph_->update(); });
        connect(delay_, &QDoubleSpinBox::valueChanged, this, [this](double x) { if (!loading_) forChannels([&](int ch) { eqChan(bus_, ch).delay->set(float(x)); }); });
        v->addLayout(grid);
        v->addWidget(new QLabel("Drag the numbered points in the graph (wheel = Q). Right click on the graph to change the dB scale.\n"
                                "With STEREO REPEAT bus mode, the 8 channels can be used as a 2/3/4-way crossover (LP/HP cells + trim + delay)."));

        connect(chSel_, &QComboBox::currentIndexChanged, this, [this] { graph_->setChannel(chSel_->currentData().toInt()); load(); });
        connect(&timer_, &QTimer::timeout, this, [this, on] {
            if (on->isChecked() != eqOnP(bus_)->on()) { QSignalBlocker b(on); on->setChecked(eqOnP(bus_)->on()); }
            const uint32_t r = eqRev(bus_).load();
            if (r != seenRev_) { seenRev_ = r; if (!anyFocus()) load(); graph_->update(); }
        });
        timer_.start(250);
        load();
        resize(900, 600);
    }

private:
    struct Cell { QCheckBox* on; QComboBox* type; QDoubleSpinBox *f, *gain, *q; };
    static Settings::KV& allClip() { static Settings::KV kv; return kv; }
    bool anyFocus() const {
        for (auto& c : cells_) if (c.f->hasFocus() || c.gain->hasFocus() || c.q->hasFocus()) return true;
        return trim_->hasFocus() || delay_->hasFocus();
    }
    void forChannels(const std::function<void(int)>& f) {
        const int sel = chSel_->currentData().toInt();
        if (sel < 0) for (int c = 0; c < eqNch(bus_); ++c) f(c);
        else f(sel);
    }
    void pasteChannel() {
        if (chClip_.isEmpty()) return;
        const QString src = QStringLiteral("channel[%1].").arg(chClipCh_);
        forChannels([&](int c) {
            Settings::KV kv;
            for (auto it = chClip_.begin(); it != chClip_.end(); ++it)
                if (it.key().startsWith(src)) kv[QStringLiteral("channel[%1].").arg(c) + it.key().mid(src.size())] = it.value();
            importEq(bus_, kv);
        });
        load();
    }
    void load() {
        loading_ = true;
        abBtn_->setText(eqABP(bus_)->on() ? "Memory B" : "Memory A");
        auto& ec = eqChan(bus_, graph_->channel());
        for (int k = 0; k < kEqCells; ++k) {
            Cell& c = cells_[k];
            c.on->setChecked(ec.cell[k].on->on());
            c.type->setCurrentIndex(ec.cell[k].type->i());
            c.f->setValue(double(ec.cell[k].f->get()));
            c.gain->setValue(double(ec.cell[k].gain->get()));
            c.q->setValue(double(ec.cell[k].q->get()));
        }
        trim_->setValue(double(ec.trim->get()));
        delay_->setValue(double(ec.delay->get()));
        loading_ = false;
        graph_->update();
    }
    int bus_;
    EqGraph* graph_;
    QComboBox* chSel_;
    QPushButton* abBtn_;
    Cell cells_[kEqCells];
    QDoubleSpinBox *trim_, *delay_;
    Settings::KV chClip_;
    int chClipCh_ = 0;
    bool loading_ = false;
    uint32_t seenRev_ = 0;
    QTimer timer_;
};

std::map<int, BusEqDialog*> gEq;

void openEq(QWidget* parent, int t) {
    if (!gEq[t]) {
        gEq[t] = new BusEqDialog(parent, t);
        gEq[t]->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(gEq[t], &QObject::destroyed, [t] { gEq[t] = nullptr; });
    }
    gEq[t]->show();
    gEq[t]->raise();
    gEq[t]->activateWindow();
}

} // namespace

void openBusEqDialog(QWidget* parent, int bus) { openEq(parent, bus); }
void openStripEqDialog(QWidget* parent, int strip) { if (isHwStrip(strip)) openEq(parent, kStripEq + strip); }

} // namespace road
