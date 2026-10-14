#include "gui/Widgets.h"

#include <QInputDialog>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <set>

#include "core/Settings.h"
#include "gui/Theme.h"

namespace road {

using namespace theme;

// ------------------------------------------------------------------ Syncable registry
static std::set<Syncable*>& registry() {
    static std::set<Syncable*> r;
    return r;
}
Syncable::Syncable() { registry().insert(this); }
Syncable::~Syncable() { registry().erase(this); }
void Syncable::syncAll() { for (Syncable* s : registry()) s->sync(); }
bool Syncable::guiLocked() { return Settings::options().lockGui; }

QString formatDb(float v) {
    if (v <= -59.95f) return QStringLiteral("-60.0");
    return QString::asprintf("%+.1f", double(v)).replace("+0.0", "0.0");
}

// largest font (<= px) that makes `text` fit in `width`
static QFont fitFont(const QString& text, int px, bool bold, double width) {
    QFont f = mkFont(px, bold);
    while (px > 7 && QFontMetrics(f).horizontalAdvance(text) > width) f = mkFont(--px, bold);
    return f;
}

static bool editValue(QWidget* w, Param* p, const QString& title, int decimals) {
    bool ok = false;
    const double v = QInputDialog::getDouble(w, title, QStringLiteral("Value (%1 .. %2)").arg(p->min).arg(p->max),
                                             double(p->get()), double(p->min), double(p->max), decimals, &ok);
    if (ok) p->set(float(v));
    return ok;
}

// ------------------------------------------------------------------ Knob
Knob::Knob(Param* p, const QString& label, QColor color, QWidget* parent)
    : QWidget(parent), p_(p), label_(label), color_(color) {
    setMinimumSize(36, 64);
    setCursor(Qt::SizeVerCursor);
    setToolTip(label + "\nDrag / wheel to change, double click to reset, right click to type a value");
}

void Knob::sync() {
    const float pv = post_ ? post_->get() : 0;
    if (p_->get() != last_ || pv != lastPost_ || badge_) { last_ = p_->get(); lastPost_ = pv; update(); }
}

void Knob::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    const int textH = compact_ ? 10 : 12;
    const float side = std::min(float(width() - 2), float(height() - textH * (compact_ ? 1 : 2) - 2));
    const QPointF c(width() / 2.0, textH + 2 + side / 2.0);
    const QRectF r(c.x() - side / 2, c.y() - side / 2, side, side);
    const float norm = (p_->get() - p_->min) / (p_->max - p_->min);
    const float zeroNorm = p_->min < 0 && p_->max > 0 ? (0 - p_->min) / (p_->max - p_->min) : 0;

    g.setFont(fitFont(label_, compact_ ? 8 : 9, true, width() - 2));
    if (post_ && post_->on()) {
        g.setPen(Qt::NoPen);
        g.setBrush(orange);
        g.drawRoundedRect(QRectF(1, 0, width() - 2, textH), 2, 2);
        g.setPen(bgDeep);
    } else {
        g.setPen(text);
    }
    g.drawText(QRectF(0, 0, width(), textH), Qt::AlignCenter, label_);

    // track
    QPen track(border, 4, Qt::SolidLine, Qt::RoundCap);
    g.setPen(track);
    g.drawArc(r.adjusted(2, 2, -2, -2), int(-45 * 16), int(270 * 16));
    // value arc
    QPen val(color_, 4, Qt::SolidLine, Qt::RoundCap);
    g.setPen(val);
    const float a0 = 225 - zeroNorm * 270, a1 = 225 - norm * 270;
    g.drawArc(r.adjusted(2, 2, -2, -2), int(a0 * 16), int((a1 - a0) * 16));
    // body
    QRadialGradient grad(c - QPointF(side * 0.15, side * 0.2), side * 0.6);
    grad.setColorAt(0, panelHi.lighter(130));
    grad.setColorAt(1, bgDeep);
    g.setPen(QPen(border, 1));
    g.setBrush(grad);
    g.drawEllipse(r.adjusted(6, 6, -6, -6));
    // pointer
    const double ang = (225 - norm * 270) * M_PI / 180.0;
    const float rad = side / 2 - 8;
    g.setPen(QPen(text, 2, Qt::SolidLine, Qt::RoundCap));
    g.drawLine(c + QPointF(std::cos(ang) * rad * 0.35, -std::sin(ang) * rad * 0.35),
               c + QPointF(std::cos(ang) * rad, -std::sin(ang) * rad));

    if (badge_) {
        const QString b = badge_();
        if (!b.isEmpty()) {
            g.setFont(mkFont(8, true));
            g.setPen(orange);
            g.drawText(r, Qt::AlignCenter, b);
        }
    }
    if (compact_) {
        if (dragging_ || hover_) {
            g.setFont(mkFont(8, true));
            g.setPen(title);
            g.drawText(r, Qt::AlignCenter, QString::number(double(p_->get()), 'f', 1));
        }
        return;
    }
    g.setFont(mkFont(10));
    g.setPen(dragging_ || hover_ ? title : textDim);
    g.drawText(QRectF(0, height() - textH, width(), textH), Qt::AlignCenter,
               QString::number(double(p_->get()), 'f', decimals_) + unit_);
}

void Knob::mousePressEvent(QMouseEvent* e) {
    if (guiLocked()) return;
    if (e->button() == Qt::RightButton) {
        if (rightClick_) rightClick_(e);
        else editValue(this, p_, label_, decimals_);
        return;
    }
    if (post_ && e->position().y() < (compact_ ? 10 : 12)) { post_->set(post_->on() ? 0 : 1); sync(); return; }
    dragging_ = true;
    dragStartY_ = int(e->position().y());
    dragStartV_ = p_->get();
    update();
}
void Knob::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    float sens = (p_->max - p_->min) / 180.0f;
    if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) sens *= 0.1f;
    p_->set(dragStartV_ + float(dragStartY_ - e->position().y()) * sens);
    sync();
}
void Knob::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; update(); }
void Knob::mouseDoubleClickEvent(QMouseEvent*) { if (!guiLocked()) { p_->reset(); sync(); } }
void Knob::wheelEvent(QWheelEvent* e) {
    if (guiLocked()) return;
    float step = (p_->max - p_->min) / 100.0f;
    if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) step *= 0.1f;
    p_->set(p_->get() + (e->angleDelta().y() > 0 ? step : -step));
    sync();
}

// ------------------------------------------------------------------ Fader
Fader::Fader(Param* p, QWidget* parent) : QWidget(parent), p_(p) {
    setMinimumSize(30, 120);
    setToolTip("Gain (dB)\nDrag / wheel (Ctrl = fine), double click = 0 dB, right click to type a value");
}

void Fader::sync() { if (p_->get() != last_) { last_ = p_->get(); update(); } }

QRectF Fader::trackRect() const { return QRectF(width() / 2.0 - 3, 12, 6, height() - 36); }
float Fader::valueToY(float v) const {
    const QRectF t = trackRect();
    return float(t.bottom() - (v - p_->min) / (p_->max - p_->min) * t.height());
}
float Fader::yToValue(float y) const {
    const QRectF t = trackRect();
    return p_->min + float((t.bottom() - y) / t.height()) * (p_->max - p_->min);
}

void Fader::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    const QRectF t = trackRect();
    // scale ticks
    g.setFont(mkFont(8));
    for (int db = 12; db >= -60; db -= 12) {
        const float y = valueToY(float(db));
        g.setPen(QPen(border, 1));
        g.drawLine(QPointF(t.left() - 6, y), QPointF(t.left() - 2, y));
        g.drawLine(QPointF(t.right() + 2, y), QPointF(t.right() + 6, y));
    }
    g.setPen(Qt::NoPen);
    g.setBrush(bgDeep);
    g.drawRoundedRect(t, 3, 3);
    // 0 dB marker
    g.setPen(QPen(title.darker(150), 1));
    const float y0 = valueToY(0);
    g.drawLine(QPointF(t.left() - 8, y0), QPointF(t.right() + 8, y0));
    // filled part
    const float yv = valueToY(p_->get());
    g.setPen(Qt::NoPen);
    g.setBrush(accent.darker(170));
    g.drawRoundedRect(QRectF(t.left() + 1, yv, t.width() - 2, t.bottom() - yv), 2, 2);
    // cap
    const QRectF cap(2, yv - 10, width() - 4, 20);
    QLinearGradient lg(cap.topLeft(), cap.bottomLeft());
    lg.setColorAt(0, QColor(0xe8, 0xee, 0xf4));
    lg.setColorAt(0.5, QColor(0xb4, 0xbe, 0xc9));
    lg.setColorAt(1, QColor(0x8a, 0x95, 0xa1));
    g.setBrush(lg);
    g.setPen(QPen(bgDeep, 1));
    g.drawRoundedRect(cap, 3, 3);
    g.setPen(QPen(dragging_ ? title : QColor(0x20, 0x26, 0x2e), 2));
    g.drawLine(QPointF(cap.left() + 3, yv), QPointF(cap.right() - 3, yv));
    // value
    g.setFont(mkFont(10, true));
    g.setPen(p_->get() > 0.05f ? title : text);
    g.drawText(QRectF(0, height() - 18, width(), 18), Qt::AlignCenter, formatDb(p_->get()));
    if (!caption_.isEmpty()) {
        g.setFont(mkFont(9, true));
        g.setPen(captionColor_.isValid() ? captionColor_ : title);
        g.drawText(QRectF(0, 0, width(), 12), Qt::AlignCenter, caption_);
    }
}

void Fader::mousePressEvent(QMouseEvent* e) {
    if (guiLocked()) return;
    if (e->button() == Qt::RightButton) { editValue(this, p_, "Gain", 1); return; }
    dragging_ = true;
    dragStartY_ = int(e->position().y());
    // click outside the cap jumps there
    const float yv = valueToY(p_->get());
    if (std::fabs(e->position().y() - yv) > 12) p_->set(yToValue(float(e->position().y())));
    dragStartV_ = p_->get();
    sync();
}
void Fader::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    float dy = float(dragStartY_ - e->position().y());
    const float perPx = (p_->max - p_->min) / float(trackRect().height());
    float v = dragStartV_ + dy * perPx * ((e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) ? 0.1f : 1.0f);
    p_->set(v);
    sync();
}
void Fader::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; update(); }
void Fader::mouseDoubleClickEvent(QMouseEvent*) { if (!guiLocked()) { p_->reset(); sync(); } }
void Fader::wheelEvent(QWheelEvent* e) {
    if (guiLocked()) return;
    const float step = (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) ? 0.1f : 0.5f;
    p_->set(p_->get() + (e->angleDelta().y() > 0 ? step : -step));
    sync();
}

// ------------------------------------------------------------------ LevelMeter
LevelMeter::LevelMeter(int channels, Source src, QWidget* parent)
    : QWidget(parent), ch_(channels), src_(std::move(src)) {
    level_.assign(size_t(channels), -120);
    hold_.assign(size_t(channels), -120);
    holdT_.assign(size_t(channels), 0);
    setMinimumWidth(8);
}

float LevelMeter::dbToPos(float db) { return std::clamp((db + 60.0f) / 72.0f, 0.0f, 1.0f); }

void LevelMeter::tick() {
    bool changed = false;
    for (int c = 0; c < ch_; ++c) {
        const float v = src_ ? src_(c) : 0;
        const float db = v > 1e-6f ? 20 * std::log10(v) : -120;
        float& l = level_[size_t(c)];
        const float nl = std::max(db, l - 1.6f); // ~50 dB/s decay at 30 fps
        if (std::fabs(nl - l) > 0.05f) changed = true;
        l = nl;
        if (db >= hold_[size_t(c)] || --holdT_[size_t(c)] <= 0) {
            if (hold_[size_t(c)] != std::max(db, -120.0f)) changed = true;
            hold_[size_t(c)] = db >= hold_[size_t(c)] ? db : std::max(-120.0f, hold_[size_t(c)] - 1.0f);
            if (db >= hold_[size_t(c)]) holdT_[size_t(c)] = 45;
        }
    }
    if (limit_ && limit_->get() != lastLimit_) { lastLimit_ = limit_->get(); changed = true; }
    if (gr_) { float g = gr_(); if (std::fabs(g - grShown_) > 0.005f) { grShown_ = g; changed = true; } }
    if (changed && isVisible()) update();
}

void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter g(this);
    const QRectF area(0, 12, width(), height() - 36);
    g.fillRect(area, bgDeep);
    const float gap = 1;
    const float bw = (float(area.width()) - gap * float(ch_ - 1)) / float(ch_);
    const float yR = float(area.bottom() - dbToPos(0) * area.height());
    const float yY = float(area.bottom() - dbToPos(-12) * area.height());
    for (int c = 0; c < ch_; ++c) {
        const float x = float(area.left()) + float(c) * (bw + gap);
        const float pos = dbToPos(level_[size_t(c)]);
        const float top = float(area.bottom() - pos * area.height());
        // green / yellow / red segments
        auto seg = [&](float y0, float y1, QColor col) {
            if (top >= y1) return;
            const float a = std::max(top, y0);
            g.fillRect(QRectF(x, a, bw, y1 - a), col);
        };
        seg(yY, float(area.bottom()), meterGreen);
        seg(yR, yY, meterYellow);
        seg(float(area.top()), yR, meterRed);
        // peak hold
        const float hp = float(area.bottom() - dbToPos(hold_[size_t(c)]) * area.height());
        if (hold_[size_t(c)] > -60) g.fillRect(QRectF(x, hp - 1, bw, 2), hold_[size_t(c)] > 0 ? meterRed : text);
    }
    if (limit_) {
        const float lv = std::min(limit_->get(), limitMax_);
        if (lv < limitMax_ - 0.01f || (limType_ && limType_->on())) {
            const float y = float(area.bottom() - dbToPos(lv) * area.height());
            const QColor lc = limType_ && limType_->on() ? limitGreen : limitYellow;
            g.fillRect(QRectF(area.left(), y - 1, area.width(), 2), lc);
        }
        if (gr_ && grShown_ < 0.995f) { // limiter activity LED
            g.setBrush(meterRed);
            g.setPen(Qt::NoPen);
            g.drawEllipse(QPointF(width() / 2.0, 5), 3, 3);
        }
    }
}

void LevelMeter::setLimitFromY(int y) {
    const QRectF area(0, 12, width(), height() - 36);
    const float pos = float((area.bottom() - y) / area.height());
    float db = pos * 72.0f - 60.0f;
    limit_->set(std::min(db, limitMax_));
    update();
}

void LevelMeter::mousePressEvent(QMouseEvent* e) {
    if (!limit_ || Syncable::guiLocked()) return;
    if (e->button() == Qt::RightButton) { editValue(this, limit_, "Limiter threshold (dB)", 1); return; }
    if (e->modifiers() & Qt::ControlModifier) {
        if (limType_) limType_->set(limType_->on() ? 0 : 1);
        update();
        return;
    }
    setLimitFromY(int(e->position().y()));
}
void LevelMeter::mouseMoveEvent(QMouseEvent* e) {
    if (!limit_ || Syncable::guiLocked() || !(e->buttons() & Qt::LeftButton) || (e->modifiers() & Qt::ControlModifier)) return;
    setLimitFromY(int(e->position().y()));
}

// ------------------------------------------------------------------ ToggleButton
ToggleButton::ToggleButton(Param* p, const QString& text, QColor onColor, QWidget* parent)
    : QWidget(parent), p_(p), text_(text), on_(onColor) {
    setMinimumSize(30, 18);
    setCursor(Qt::PointingHandCursor);
}

void ToggleButton::setStates(const QStringList& labels, const std::vector<QColor>& colors) {
    states_ = labels;
    stateColors_ = colors;
    update();
}

void ToggleButton::sync() {
    if (p_ && p_->get() != last_) { last_ = p_->get(); update(); }
    else if (indicator_ || textFn_) update();
}

void ToggleButton::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    int state = p_ ? p_->i() : (down_ ? 1 : 0);
    QColor bgc = btnOff;
    QColor fg = btnText;
    QString label = textFn_ ? textFn_() : text_;
    if (!states_.isEmpty()) {
        state = std::clamp(state, 0, int(states_.size()) - 1);
        label = states_[state];
        if (state > 0) { bgc = state < int(stateColors_.size()) ? stateColors_[size_t(state)] : on_; fg = bgDeep; }
    } else if (state > 0) {
        bgc = on_; fg = bgDeep;
    }
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QLinearGradient lg(r.topLeft(), r.bottomLeft());
    lg.setColorAt(0, bgc.lighter(118));
    lg.setColorAt(1, bgc.darker(112));
    g.setBrush(lg);
    g.setPen(QPen(state > 0 ? bgc.lighter(140) : border, 1));
    g.drawRoundedRect(r, 3, 3);
    g.setFont(fitFont(label, std::min(11, std::max(9, height() - 8)), true, r.width() - 4));
    g.setPen(fg);
    QRectF tr = r;
    if (indicator_) tr.adjust(0, 0, 0, -4);
    g.drawText(tr, Qt::AlignCenter, label);
    if (indicator_) {
        const auto cols = indicator_();
        const float w = float(r.width() - 6) / float(std::max<size_t>(cols.size(), 1));
        for (size_t i = 0; i < cols.size(); ++i)
            g.fillRect(QRectF(r.left() + 3 + w * float(i), r.bottom() - 4, w - 1, 2), cols[i]);
    }
}

void ToggleButton::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) { emit rightClicked(e->globalPosition().toPoint()); return; }
    if (Syncable::guiLocked()) return;
    if (action_) { down_ = true; update(); return; }
    if (p_) {
        if (states_.size() > 2) p_->set(float((p_->i() + 1) % states_.size()));
        else p_->set(p_->on() ? 0 : 1);
        sync();
    }
    emit clicked();
}

void ToggleButton::mouseReleaseEvent(QMouseEvent* e) {
    if (action_ && down_) {
        down_ = false;
        update();
        if (rect().contains(e->position().toPoint())) { action_(); emit clicked(); }
    }
}

// ------------------------------------------------------------------ Pad2D
Pad2D::Pad2D(QWidget* parent) : QWidget(parent) {
    setMinimumSize(80, 64);
    setCursor(Qt::CrossCursor);
}

void Pad2D::sync() {
    std::vector<float> now;
    for (auto& p : pages_) { now.push_back(p.x->get()); now.push_back(p.y->get()); }
    if (now != last_) { last_ = now; update(); }
}

QRectF Pad2D::area() const { return QRectF(rect()).adjusted(1, indicators_ ? 10 : 1, -1, -1); }

void Pad2D::paintEvent(QPaintEvent*) {
    if (pages_.empty()) return;
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    const Page& pg = pages_[size_t(page_)];
    const QRectF a = area();
    QLinearGradient lg(a.topLeft(), a.bottomLeft());
    lg.setColorAt(0, pg.top);
    lg.setColorAt(1, pg.bottom);
    g.setPen(QPen(border, 1));
    g.setBrush(lg);
    g.drawRoundedRect(a, 4, 4);
    // grid
    g.setPen(QPen(QColor(255, 255, 255, 28), 1));
    for (int i = 1; i < 4; ++i) {
        g.drawLine(QPointF(a.left() + a.width() * i / 4, a.top() + 2), QPointF(a.left() + a.width() * i / 4, a.bottom() - 2));
        g.drawLine(QPointF(a.left() + 2, a.top() + a.height() * i / 4), QPointF(a.right() - 2, a.top() + a.height() * i / 4));
    }
    g.setFont(mkFont(8, true));
    g.setPen(QColor(255, 255, 255, 120));
    if (!pg.up.isEmpty()) g.drawText(a.adjusted(0, 2, 0, 0), Qt::AlignHCenter | Qt::AlignTop, pg.up);
    if (!pg.down.isEmpty()) g.drawText(a.adjusted(0, 0, 0, -2), Qt::AlignHCenter | Qt::AlignBottom, pg.down);
    if (!pg.left.isEmpty()) g.drawText(a.adjusted(3, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, pg.left);
    if (!pg.right.isEmpty()) g.drawText(a.adjusted(0, 0, -3, 0), Qt::AlignRight | Qt::AlignVCenter, pg.right);
    if (pg.showSpeakers) {
        g.setPen(Qt::NoPen);
        g.setBrush(QColor(255, 255, 255, 70));
        const QPointF sp[5] = {{0.08, 0.1}, {0.5, 0.06}, {0.92, 0.1}, {0.08, 0.92}, {0.92, 0.92}};
        for (auto s : sp) g.drawRect(QRectF(a.left() + s.x() * a.width() - 3, a.top() + s.y() * a.height() - 2, 6, 4));
    }
    // cursor
    const float nx = (pg.x->get() - pg.x->min) / (pg.x->max - pg.x->min);
    const float ny = (pg.y->get() - pg.y->min) / (pg.y->max - pg.y->min);
    const QPointF c(a.left() + nx * a.width(), a.bottom() - ny * a.height());
    QRadialGradient glow(c, 10);
    glow.setColorAt(0, QColor(255, 255, 255, 200));
    glow.setColorAt(1, QColor(255, 255, 255, 0));
    g.setBrush(glow);
    g.drawEllipse(c, 10, 10);
    g.setBrush(title);
    g.setPen(QPen(bgDeep, 1));
    g.drawEllipse(c, 4, 4);
    g.setFont(mkFont(9, true));
    g.setPen(QColor(255, 255, 255, 150));
    if (pages_.size() == 1) g.drawText(a.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignBottom, pg.name);
    if (overlay_) {
        g.drawText(a.adjusted(4, 2, -4, -2), Qt::AlignRight | Qt::AlignBottom, overlay_());
    }
    if (indicators_) {
        // one dot per page: orange when that page is not in its neutral position
        for (size_t i = 0; i < pages_.size(); ++i) {
            const Page& p = pages_[i];
            const bool active = std::fabs(p.x->get() - p.x->def) > 0.004f || std::fabs(p.y->get() - p.y->def) > 0.004f;
            g.setBrush(active ? orange : btnOff);
            g.setPen(Qt::NoPen);
            g.drawEllipse(QPointF(width() / 2.0 + (float(i) - (pages_.size() - 1) / 2.0f) * 12, 5), 3, 3);
        }
    }
}

void Pad2D::setFromPos(QPointF p) {
    const Page& pg = pages_[size_t(page_)];
    const QRectF a = area();
    const float nx = std::clamp(float((p.x() - a.left()) / a.width()), 0.0f, 1.0f);
    const float ny = std::clamp(float((a.bottom() - p.y()) / a.height()), 0.0f, 1.0f);
    pg.x->set(pg.x->min + nx * (pg.x->max - pg.x->min));
    pg.y->set(pg.y->min + ny * (pg.y->max - pg.y->min));
    sync();
}

void Pad2D::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) { emit contextMenuRequested(e->globalPosition().toPoint()); return; }
    if (guiLocked() || pages_.empty()) return;
    setFromPos(e->position());
}
void Pad2D::mouseMoveEvent(QMouseEvent* e) {
    if (guiLocked() || pages_.empty() || !(e->buttons() & Qt::LeftButton)) return;
    setFromPos(e->position());
}
void Pad2D::mouseDoubleClickEvent(QMouseEvent*) {
    if (guiLocked() || pages_.empty()) return;
    pages_[size_t(page_)].x->reset();
    pages_[size_t(page_)].y->reset();
    sync();
}

// ------------------------------------------------------------------ DeviceButton
DeviceButton::DeviceButton(const QString& caption, QWidget* parent) : QWidget(parent), caption_(caption), accent_(device) {
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(30);
}

void DeviceButton::setDevice(const QString& name, bool connected) {
    if (name == device_ && connected == connected_) return;
    device_ = name;
    connected_ = connected;
    setToolTip(name.isEmpty() ? caption_ : name);
    update();
}

void DeviceButton::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    g.setBrush(hover_ ? bgDeep.lighter(150) : bgDeep);
    g.setPen(QPen(hover_ ? accent_ : border, 1));
    g.drawRoundedRect(r, 4, 4);
    QRectF textArea = r.adjusted(6, 2, -5, -2);
    if (!badge_.isEmpty()) {
        // badge + caption on the first line, device name on a full width second line
        g.setFont(mkFont(11, true));
        const double bw = g.fontMetrics().horizontalAdvance(badge_) + 10.0;
        const QRectF b(r.left() + 4, r.top() + 4, bw, 16);
        g.setPen(Qt::NoPen);
        g.setBrush(badgeColor_);
        g.drawRoundedRect(b, 3, 3);
        g.setPen(bgDeep);
        g.drawText(b, Qt::AlignCenter, badge_);
        const QRectF cap(b.right() + 5, b.top(), r.right() - b.right() - 14, b.height());
        g.setFont(fitFont(caption_, 9, true, cap.width()));
        g.setPen(textDim);
        g.drawText(cap, Qt::AlignLeft | Qt::AlignVCenter, caption_);
        textArea = QRectF(r.left() + 5, b.bottom() + 1, r.width() - 10, r.bottom() - b.bottom() - 2);
        QString t = device_.isEmpty() ? placeholder_ : device_;
        g.setFont(mkFont(11, !device_.isEmpty()));
        g.setPen(device_.isEmpty() ? textDim : (connected_ ? text : meterRed));
        g.drawText(textArea, Qt::AlignLeft | Qt::AlignVCenter, g.fontMetrics().elidedText(t, Qt::ElideRight, int(textArea.width())));
    } else {
        g.setFont(fitFont(caption_, 8, true, textArea.width()));
        g.setPen(textDim);
        g.drawText(textArea, Qt::AlignLeft | Qt::AlignTop, caption_);
        QString t = device_.isEmpty() ? placeholder_ : device_;
        g.setFont(mkFont(11, !device_.isEmpty()));
        g.setPen(device_.isEmpty() ? textDim : (connected_ ? text : meterRed));
        g.drawText(textArea, Qt::AlignLeft | Qt::AlignBottom, g.fontMetrics().elidedText(t, Qt::ElideRight, int(textArea.width())));
    }
    if (!device_.isEmpty()) {   // connection LED
        g.setPen(Qt::NoPen);
        g.setBrush(connected_ ? meterGreen : meterRed);
        g.drawEllipse(QPointF(r.right() - 6, r.top() + 7), 2.5, 2.5);
    }
}

// ------------------------------------------------------------------ GroupPanel
GroupPanel::GroupPanel(const QString& title, const QString& subtitle, QColor accent, QWidget* parent)
    : QWidget(parent), title_(title), subtitle_(subtitle), accent_(accent) {
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(6, 30, 6, 6);
    row_->setSpacing(4);
}

void GroupPanel::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    g.setPen(QPen(border, 1));
    g.setBrush(QColor(0x1f, 0x24, 0x2c));
    g.drawRoundedRect(r, 8, 8);
    // title bar
    QPainterPath head;
    head.addRoundedRect(QRectF(r.left(), r.top(), r.width(), 24), 8, 8);
    g.setClipRect(QRectF(r.left(), r.top(), r.width(), 24));
    QLinearGradient lg(r.topLeft(), QPointF(r.right(), r.top()));
    QColor a1 = accent_; a1.setAlpha(70);
    QColor a2 = accent_; a2.setAlpha(10);
    lg.setColorAt(0, a1);
    lg.setColorAt(1, a2);
    g.fillPath(head, lg);
    g.setClipping(false);
    g.fillRect(QRectF(r.left() + 10, r.top() + 23, r.width() - 20, 1.5), accent_);
    g.setFont(mkFont(12, true));
    g.setPen(accent_);
    const int tw = g.fontMetrics().horizontalAdvance(title_);
    g.drawText(QRectF(r.left() + 10, r.top(), r.width(), 24), Qt::AlignLeft | Qt::AlignVCenter, title_);
    g.setFont(mkFont(10));
    g.setPen(textDim);
    g.drawText(QRectF(r.left() + 18 + tw, r.top(), r.width() - tw - 26, 24), Qt::AlignLeft | Qt::AlignVCenter,
               g.fontMetrics().elidedText(subtitle_, Qt::ElideRight, int(r.width() - tw - 30)));
}

void DeviceButton::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) emit rightClicked(e->globalPosition().toPoint());
    else if (!Syncable::guiLocked()) emit clicked(e->globalPosition().toPoint());
}

// ------------------------------------------------------------------ EditableLabel
EditableLabel::EditableLabel(const std::string& key, const QString& placeholder, QWidget* parent)
    : QWidget(parent), key_(key), placeholder_(placeholder) {
    setMinimumHeight(16);
    setToolTip("Double click to rename");
}

void EditableLabel::sync() {
    const uint32_t r = params().strRevision();
    if (r == rev_) return;
    rev_ = r;
    const QString t = QString::fromStdString(params().str(key_));
    if (t != text_) { text_ = t; update(); }
}

void EditableLabel::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setFont(mkFont(11, true));
    g.setPen(text_.isEmpty() ? textDim : text);
    g.drawText(rect(), Qt::AlignCenter, g.fontMetrics().elidedText(text_.isEmpty() ? placeholder_ : text_, Qt::ElideRight, width() - 2));
}

void EditableLabel::mouseDoubleClickEvent(QMouseEvent*) {
    if (guiLocked()) return;
    bool ok = false;
    const QString t = QInputDialog::getText(this, "Label", "Name:", QLineEdit::Normal, text_, &ok);
    if (ok) { params().setStr(key_, t.left(32).toStdString()); sync(); }
}

void EditableLabel::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) emit rightClicked(e->globalPosition().toPoint());
}

// ------------------------------------------------------------------ Caption
Caption::Caption(const QString& text, QColor color, int px, Qt::Alignment align, QWidget* parent)
    : QWidget(parent), text_(text), color_(color), px_(px), align_(align) {}
QSize Caption::sizeHint() const { return {QFontMetrics(mkFont(px_, true)).horizontalAdvance(text_) + 6, px_ + 6}; }
void Caption::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setFont(mkFont(px_, true));
    g.setPen(color_);
    g.drawText(rect(), int(align_), text_);
}

} // namespace road
