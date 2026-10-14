#include "gui/StreamerView.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

#include "core/Params.h"
#include "gui/Theme.h"
#include "gui/Widgets.h"

namespace road {

using namespace theme;

namespace {

QString stripName(int s) {
    const QString l = QString::fromStdString(params().str("strip[" + std::to_string(s) + "].label"));
    return l.isEmpty() ? (isHwStrip(s) ? QStringLiteral("Input %1").arg(s + 1) : QStringLiteral("Virtual %1").arg(s - kHwStrips + 1)) : l;
}
QString busName(int b) {
    const QString l = QString::fromStdString(params().str("bus[" + std::to_string(b) + "].label"));
    return l.isEmpty() ? QString(kBusNames[b]) : QStringLiteral("%1 %2").arg(kBusNames[b], l);
}

class StreamerView : public QWidget {
public:
    explicit StreamerView(QWidget* parent) : QWidget(parent, Qt::Window) {
        setWindowTitle("RoadBoard — Streamer View");
        auto* v = new QVBoxLayout(this);
        labeledOnly_ = new QCheckBox("Show labelled strips / buses only");
        v->addWidget(labeledOnly_);
        grid_ = new QGridLayout;
        grid_->setHorizontalSpacing(10);
        v->addLayout(grid_, 1);
        v->addWidget(new QLabel("Each slider is the level of a strip in the mix of a BUS. MON: listen to that BUS on the monitoring BUS "
                                "(set it with right click on SEL in the master section)."));
        connect(labeledOnly_, &QCheckBox::toggled, this, [this] { rebuild(); });
        rebuild();
        connect(&t_, &QTimer::timeout, this, [this] { refresh(); });
        t_.start(150);
        resize(1000, 520);
    }
protected:
    void paintEvent(QPaintEvent*) override { QPainter g(this); g.fillRect(rect(), bg); }
private:
    void rebuild() {
        while (QLayoutItem* it = grid_->takeAt(0)) { delete it->widget(); delete it; }
        sliders_.clear();
        const bool only = labeledOnly_->isChecked();
        std::vector<int> strips, buses;
        for (int s = 0; s < kStrips; ++s) if (!only || !params().str("strip[" + std::to_string(s) + "].label").empty()) strips.push_back(s);
        for (int b = 0; b < kBuses; ++b) if (!only || !params().str("bus[" + std::to_string(b) + "].label").empty()) buses.push_back(b);
        int col = 1;
        for (int b : buses) {
            auto* head = new QVBoxLayout;
            head->addWidget(new Caption(busName(b), b < kHwBuses ? busA : busB, 12, Qt::AlignCenter, this));
            auto* mon = new ToggleButton(params().bus[b].sel, "MON", title, this);
            mon->setFixedHeight(20);
            mon->setMomentary([b] {
                auto& P = params();
                const bool was = P.bus[b].sel->on();
                for (int k = 0; k < kBuses; ++k) P.bus[k].sel->set(0);
                P.bus[b].sel->set(was ? 0 : 1);
                P.opt.monitorOnSel->set(1);
            });
            head->addWidget(mon);
            grid_->addLayout(head, 0, col++);
        }
        int row = 1;
        for (int s : strips) {
            auto* name = new Caption(stripName(s), text, 12, Qt::AlignLeft | Qt::AlignVCenter, this);
            name->setMinimumWidth(130);
            grid_->addWidget(name, row, 0);
            col = 1;
            for (int b : buses) {
                auto* sl = new QSlider(Qt::Horizontal);
                sl->setRange(-600, 120);
                sl->setMinimumWidth(90);
                Param* p = params().strip[s].gainLayer[b];
                sl->setValue(int(std::lround(p->get() * 10)));
                connect(sl, &QSlider::valueChanged, this, [p](int v) { p->set(v / 10.0f); });
                sl->setToolTip(QStringLiteral("%1 in %2").arg(stripName(s), busName(b)));
                grid_->addWidget(sl, row, col++);
                sliders_.push_back({sl, p, params().strip[s].bus[b]});
            }
            auto* mute = new ToggleButton(params().strip[s].mute, "Mute", theme::mute, this);
            mute->setFixedSize(54, 22);
            grid_->addWidget(mute, row, col);
            ++row;
        }
        grid_->setRowStretch(row, 1);
    }
    void refresh() {
        for (auto& [sl, p, assign] : sliders_) {
            const int v = int(std::lround(p->get() * 10));
            if (!sl->isSliderDown() && sl->value() != v) { QSignalBlocker b(sl); sl->setValue(v); }
            sl->setEnabled(assign->on());
        }
    }
    QGridLayout* grid_;
    QCheckBox* labeledOnly_;
    std::vector<std::tuple<QSlider*, Param*, Param*>> sliders_;
    QTimer t_;
};

StreamerView* gView = nullptr;

} // namespace

void openStreamerView(QWidget* parent) {
    if (!gView) {
        gView = new StreamerView(parent);
        gView->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(gView, &QObject::destroyed, [] { gView = nullptr; });
    }
    gView->show();
    gView->raise();
    gView->activateWindow();
}

} // namespace road
