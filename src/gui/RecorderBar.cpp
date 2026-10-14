#include "gui/RecorderBar.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

#include "core/Params.h"
#include "engine/TapeDeck.h"
#include "gui/AppContext.h"
#include "gui/Dialogs.h"
#include "gui/Theme.h"
#include "gui/Widgets.h"

namespace road {

using namespace theme;

namespace {

QString hms(double s, bool hours) {
    const int t = int(std::max(0.0, s));
    return hours ? QString::asprintf("%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60)
                 : QString::asprintf("%02d:%02d", t / 60, t % 60);
}

// ---------------------------------------------------------------- vector icons
void iconPlay(QPainter& g, QPointF c, double s) {
    QPainterPath p;
    p.moveTo(c.x() - s * 0.35, c.y() - s * 0.45);
    p.lineTo(c.x() + s * 0.5, c.y());
    p.lineTo(c.x() - s * 0.35, c.y() + s * 0.45);
    p.closeSubpath();
    g.fillPath(p, g.pen().color());
}
void iconPause(QPainter& g, QPointF c, double s) {
    g.fillRect(QRectF(c.x() - s * 0.4, c.y() - s * 0.45, s * 0.28, s * 0.9), g.pen().color());
    g.fillRect(QRectF(c.x() + s * 0.12, c.y() - s * 0.45, s * 0.28, s * 0.9), g.pen().color());
}
void iconStop(QPainter& g, QPointF c, double s) {
    QPainterPath p;
    p.addRoundedRect(QRectF(c.x() - s * 0.38, c.y() - s * 0.38, s * 0.76, s * 0.76), 2, 2);
    g.fillPath(p, g.pen().color());
}
void iconSkip(QPainter& g, QPointF c, double s, bool fwd) {
    const double d = fwd ? 1 : -1;
    for (int k = 0; k < 2; ++k) {
        QPainterPath p;
        const double x0 = c.x() + d * (k * s * 0.38 - s * 0.4);
        p.moveTo(x0, c.y() - s * 0.38);
        p.lineTo(x0 + d * s * 0.42, c.y());
        p.lineTo(x0, c.y() + s * 0.38);
        p.closeSubpath();
        g.fillPath(p, g.pen().color());
    }
}
void iconFolder(QPainter& g, QPointF c, double s) {
    QPainterPath p;
    p.moveTo(c.x() - s * 0.5, c.y() - s * 0.3);
    p.lineTo(c.x() - s * 0.15, c.y() - s * 0.3);
    p.lineTo(c.x() - s * 0.05, c.y() - s * 0.18);
    p.lineTo(c.x() + s * 0.5, c.y() - s * 0.18);
    p.lineTo(c.x() + s * 0.5, c.y() + s * 0.38);
    p.lineTo(c.x() - s * 0.5, c.y() + s * 0.38);
    p.closeSubpath();
    g.fillPath(p, g.pen().color());
}
void iconLoop(QPainter& g, QPointF c, double s) {
    QPen pen = g.pen();
    pen.setWidthF(2);
    g.setPen(pen);
    g.setBrush(Qt::NoBrush);
    const QRectF r(c.x() - s * 0.45, c.y() - s * 0.3, s * 0.9, s * 0.6);
    g.drawArc(r, 30 * 16, 300 * 16);
    QPainterPath a;
    a.moveTo(r.right() + 1, r.center().y() - s * 0.22);
    a.lineTo(r.right() - s * 0.16, r.center().y() - s * 0.05);
    a.lineTo(r.right() + s * 0.12, r.center().y() + s * 0.05);
    a.closeSubpath();
    g.fillPath(a, pen.color());
}
void iconGear(QPainter& g, QPointF c, double s) {
    QPainterPath p;
    const int teeth = 8;
    for (int i = 0; i < teeth * 2; ++i) {
        const double ang = i * M_PI / teeth;
        const double rr = (i % 2 ? 0.36 : 0.48) * s;
        const QPointF pt(c.x() + std::cos(ang) * rr, c.y() + std::sin(ang) * rr);
        if (i == 0) p.moveTo(pt); else p.lineTo(pt);
    }
    p.closeSubpath();
    p.addEllipse(c, s * 0.15, s * 0.15);
    p.setFillRule(Qt::OddEvenFill);
    g.fillPath(p, g.pen().color());
}

void drawCassette(QPainter& g, const QRectF& r, float reel, bool active, bool rec) {
    g.setPen(QPen(active ? (rec ? meterRed : title) : border, 1.2));
    QLinearGradient lg(r.topLeft(), r.bottomLeft());
    lg.setColorAt(0, QColor(0x3a, 0x33, 0x22));
    lg.setColorAt(1, QColor(0x1a, 0x17, 0x10));
    g.setBrush(lg);
    g.drawRoundedRect(r, 4, 4);
    const QRectF label = r.adjusted(4, 3, -4, -r.height() * 0.62);
    g.setPen(Qt::NoPen);
    g.setBrush(rec ? meterRed : QColor(0xf3, 0xe7, 0xb0));
    g.drawRoundedRect(label, 2, 2);
    const QRectF win(r.left() + r.width() * 0.18, r.top() + r.height() * 0.45, r.width() * 0.64, r.height() * 0.4);
    g.setBrush(QColor(0x0e, 0x0e, 0x0e));
    g.drawRoundedRect(win, 4, 4);
    for (int i = 0; i < 2; ++i) {
        const QPointF c(win.left() + win.height() / 2 + 2 + i * (win.width() - win.height() - 4), win.center().y());
        const double rr = win.height() * 0.42;
        g.setBrush(QColor(0xdd, 0xdd, 0xdd));
        g.drawEllipse(c, rr, rr);
        g.setPen(QPen(QColor(0x22, 0x22, 0x22), 1.4));
        for (int k = 0; k < 3; ++k) {
            const double a = reel + k * 2.0944;
            g.drawLine(c + QPointF(std::cos(a) * rr * 0.3, std::sin(a) * rr * 0.3), c + QPointF(std::cos(a) * rr * 0.85, std::sin(a) * rr * 0.85));
        }
        g.setPen(Qt::NoPen);
    }
}

} // namespace

RecorderBar::RecorderBar(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(84);
    setMaximumHeight(84);
    setMouseTracking(true);
    gain_ = new Knob(params().rec.gain, "Gain", accent, this);
    gain_->setCompact(true);
    gain_->setToolTip("Playback gain of the file (dB)");
    setToolTip("Recorder / player. Right click: recorder options (source, format, folder...)");
}

void RecorderBar::resizeEvent(QResizeEvent*) { layoutZones(); }

void RecorderBar::layoutZones() {
    zones_.clear();
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    const double cy = r.center().y();
    double x = r.left() + 10;
    deck_ = QRectF(x, cy - 22, 66, 44);
    x += 66 + 10;
    x += 92;                                   // title / state text
    // transport
    auto btn = [&](const QString& id, double size) {
        zones_[id] = QRectF(x, cy - size / 2, size, size);
        x += size + 8;
    };
    btn("rew", 34); btn("play", 46); btn("stop", 34); btn("ff", 34); btn("rec", 42);
    x += 8;
    // right side: tools then chips
    double rx = r.right() - 8;
    auto rbtn = [&](const QString& id, double size) {
        rx -= size;
        zones_[id] = QRectF(rx, cy - size / 2, size, size);
        rx -= 6;
    };
    rbtn("options", 32); rbtn("open", 32); rbtn("loop", 32);
    rx -= 4;
    gain_->setGeometry(int(rx - 40), int(cy - 21), 40, 42);
    rx -= 50;
    const double chipW = 30, chipGap = 3;
    const double chipsW = kBuses * chipW + (kBuses - 1) * chipGap + 8;
    chipsArea_ = QRectF(rx - chipsW, r.top() + 8, chipsW, r.height() - 16);
    double cx = chipsArea_.left();
    for (int b = 0; b < kBuses; ++b) {
        if (b == kHwBuses) cx += 8;
        zones_[QStringLiteral("chip%1").arg(b)] = QRectF(cx, chipsArea_.top() + 22, chipW, 22);
        cx += chipW + chipGap;
    }
    rx = chipsArea_.left() - 14;
    track_ = QRectF(x + 6, r.top() + 10, std::max(120.0, rx - x - 6), r.height() - 20);
    progress_ = QRectF(track_.left(), track_.center().y() - 3, track_.width(), 8);
    zones_["track"] = QRectF(track_.left(), track_.top(), track_.width(), progress_.top() - track_.top() - 2);
    zones_["progress"] = progress_.adjusted(0, -6, 0, 6);
}

QString RecorderBar::hit(QPointF p) const {
    for (auto& [id, z] : zones_) if (z.contains(p)) return id;
    if (deck_.contains(p)) return "track";
    return {};
}

void RecorderBar::tick() {
    TapeDeck* t = ctx().tape;
    if (!t) return;
    const bool playing = t->playing() && !t->paused();
    if (playing || t->recording()) reel_ += 0.22f;
    ++pulse_;
    update();
}

void RecorderBar::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    TapeDeck* t = ctx().tape;
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient bgG(r.topLeft(), r.bottomLeft());
    bgG.setColorAt(0, QColor(0x26, 0x2c, 0x35));
    bgG.setColorAt(1, QColor(0x1a, 0x1f, 0x26));
    g.setPen(QPen(border, 1));
    g.setBrush(bgG);
    g.drawRoundedRect(r, 8, 8);
    if (zones_.empty()) layoutZones();

    const bool loaded = t && t->loaded();
    const bool playing = t && t->playing() && !t->paused();
    const bool paused = t && t->paused();
    const bool rec = t && t->recording();
    const bool armed = params().rec.record->on();

    // deck & state
    drawCassette(g, deck_, reel_, playing || rec, rec);
    const QRectF titleR(deck_.right() + 10, r.top() + 14, 92, 20);
    g.setFont(mkFont(12, true));
    g.setPen(title);
    g.drawText(titleR, Qt::AlignLeft | Qt::AlignVCenter, "RECORDER");
    QString state = "STOPPED";
    QColor stateC = textDim;
    if (rec) { state = "● RECORDING"; stateC = (pulse_ / 15) % 2 ? meterRed : meterRed.darker(150); }
    else if (armed) { state = "● STARTING…"; stateC = meterRed; }
    else if (playing) { state = "▶ PLAYING"; stateC = meterGreen; }
    else if (paused) { state = "❚❚ PAUSED"; stateC = meterYellow; }
    else if (!loaded) state = "NO FILE";
    g.setFont(mkFont(10, true));
    g.setPen(stateC);
    g.drawText(QRectF(titleR.left(), titleR.bottom(), 92, 18), Qt::AlignLeft | Qt::AlignVCenter, state);
    const QString src = params().rec.recBus->on() ? [&] {
        for (int b = 0; b < kBuses; ++b) if (params().rec.armBus[b]->on()) return QStringLiteral("rec: BUS %1").arg(kBusNames[b]);
        return QStringLiteral("rec: BUS");
    }() : QStringLiteral("rec: inputs");
    g.setFont(mkFont(9));
    g.setPen(textDim);
    g.drawText(QRectF(titleR.left(), titleR.bottom() + 16, 92, 16), Qt::AlignLeft | Qt::AlignVCenter, src);

    // transport buttons
    auto round = [&](const QString& id, QColor fill, QColor fg, bool active) {
        const QRectF z = zones_[id];
        const bool hv = hover_ == id, pr = pressed_ == id;
        QRadialGradient rg(z.center() - QPointF(z.width() * 0.15, z.height() * 0.2), z.width() * 0.7);
        QColor base = active ? fill : btnOff;
        if (hv) base = base.lighter(125);
        if (pr) base = base.darker(120);
        rg.setColorAt(0, base.lighter(130));
        rg.setColorAt(1, base.darker(115));
        g.setBrush(rg);
        g.setPen(QPen(active ? fill.lighter(140) : border, 1));
        g.drawEllipse(z.adjusted(1, 1, -1, -1));
        g.setPen(active ? fg : (hv ? text : btnText));
        return z;
    };
    { const QRectF z = round("rew", accent, bgDeep, false); iconSkip(g, z.center(), z.width() * 0.45, false); }
    { const QRectF z = round("play", meterGreen, bgDeep, playing); if (playing) iconPause(g, z.center(), z.width() * 0.4); else iconPlay(g, z.center() + QPointF(2, 0), z.width() * 0.42); }
    { const QRectF z = round("stop", accent, bgDeep, false); iconStop(g, z.center(), z.width() * 0.42); }
    { const QRectF z = round("ff", accent, bgDeep, false); iconSkip(g, z.center(), z.width() * 0.45, true); }
    {
        const bool on = rec || armed;
        QColor recC = meterRed;
        if (rec && (pulse_ / 15) % 2) recC = meterRed.darker(130);
        const QRectF z = round("rec", recC, Qt::white, on);
        g.setBrush(on ? QColor(Qt::white) : meterRed);
        g.setPen(Qt::NoPen);
        g.drawEllipse(z.center(), z.width() * 0.2, z.width() * 0.2);
    }

    // track info + progress
    QString name, info;
    double pos = 0, dur = 0;
    if (rec) {
        name = QFileInfo(QString::fromStdString(t->lastRecordFile())).fileName();
        pos = t->recordSeconds();
        info = "recording";
    } else if (loaded) {
        name = QFileInfo(QString::fromStdString(t->fileName())).fileName();
        pos = t->position();
        dur = t->duration();
        const QString ext = QFileInfo(name).suffix().toUpper();
        info = ext;
        if (t->fileRate() > 0) info += QStringLiteral(" · %1 kHz").arg(t->fileRate() / 1000.0, 0, 'g', 3);
        info += QStringLiteral(" · %1").arg(t->fileChannels() == 1 ? "mono" : t->fileChannels() == 2 ? "stereo" : QStringLiteral("%1 ch").arg(t->fileChannels()));
        if (params().rec.loop->on()) info += " · loop";
    }
    g.setFont(mkFont(13, true));
    g.setPen(name.isEmpty() ? textDim : (hover_ == "track" ? title : text));
    const QRectF nameR(track_.left(), track_.top(), track_.width() * 0.68, progress_.top() - track_.top() - 4);
    g.drawText(nameR, Qt::AlignLeft | Qt::AlignBottom,
               g.fontMetrics().elidedText(name.isEmpty() ? QStringLiteral("No file loaded — click here to open one") : name, Qt::ElideMiddle, int(nameR.width())));
    g.setFont(mkFont(10));
    g.setPen(textDim);
    g.drawText(QRectF(nameR.right(), track_.top(), track_.width() - nameR.width(), nameR.height()), Qt::AlignRight | Qt::AlignBottom, info);
    // bar
    g.setPen(Qt::NoPen);
    g.setBrush(bgDeep);
    g.drawRoundedRect(progress_, 4, 4);
    if (rec) {
        const double w = progress_.width();
        const double off = std::fmod(pulse_ * 2.0, 40.0);
        g.save();
        g.setClipRect(progress_);
        for (double xx = progress_.left() - 40 + off; xx < progress_.right(); xx += 40)
            g.fillRect(QRectF(xx, progress_.top(), 20, progress_.height()), meterRed.darker(160));
        g.restore();
        (void)w;
    } else if (dur > 0) {
        const double f = std::clamp(pos / dur, 0.0, 1.0);
        QLinearGradient pg(progress_.topLeft(), progress_.topRight());
        pg.setColorAt(0, accent.darker(140));
        pg.setColorAt(1, accent);
        g.setBrush(pg);
        g.drawRoundedRect(QRectF(progress_.left(), progress_.top(), progress_.width() * f, progress_.height()), 4, 4);
        g.setBrush(Qt::white);
        g.drawEllipse(QPointF(progress_.left() + progress_.width() * f, progress_.center().y()), hover_ == "progress" ? 7 : 5, hover_ == "progress" ? 7 : 5);
    }
    g.setFont(mkFont(11, true));
    const bool hours = std::max(pos, dur) >= 3600;
    g.setPen(rec ? meterRed : text);
    g.drawText(QRectF(track_.left(), progress_.bottom() + 3, 120, track_.bottom() - progress_.bottom() - 3), Qt::AlignLeft | Qt::AlignTop, hms(pos, hours || rec));
    if (dur > 0 && !rec) {
        g.setPen(textDim);
        g.drawText(QRectF(track_.right() - 160, progress_.bottom() + 3, 160, track_.bottom() - progress_.bottom() - 3), Qt::AlignRight | Qt::AlignTop,
                   QStringLiteral("-%1 / %2").arg(hms(dur - pos, hours), hms(dur, hours)));
    }

    // play-to chips
    g.setFont(mkFont(9, true));
    g.setPen(textDim);
    g.drawText(QRectF(chipsArea_.left(), chipsArea_.top(), chipsArea_.width(), 18), Qt::AlignLeft | Qt::AlignVCenter, "PLAY FILE TO");
    for (int b = 0; b < kBuses; ++b) {
        const QRectF z = zones_[QStringLiteral("chip%1").arg(b)];
        const bool on = params().rec.bus[b]->on();
        const QColor c = b < kHwBuses ? busA : busB;
        g.setBrush(on ? c : (hover_ == QStringLiteral("chip%1").arg(b) ? btnOff.lighter(130) : btnOff));
        g.setPen(QPen(on ? c.lighter(130) : border, 1));
        g.drawRoundedRect(z, 4, 4);
        g.setPen(on ? bgDeep : btnText);
        g.drawText(z, Qt::AlignCenter, kBusNames[b]);
    }

    // tools
    auto tool = [&](const QString& id, bool active) {
        const QRectF z = zones_[id];
        const bool hv = hover_ == id;
        g.setBrush(active ? accent.darker(150) : (hv ? btnOff.lighter(125) : btnOff));
        g.setPen(QPen(active ? accent : border, 1));
        g.drawRoundedRect(z, 6, 6);
        g.setPen(active ? Qt::white : (hv ? text : btnText));
        return z;
    };
    { const QRectF z = tool("loop", params().rec.loop->on()); iconLoop(g, z.center(), z.width() * 0.6); }
    { const QRectF z = tool("open", false); iconFolder(g, z.center(), z.width() * 0.55); }
    { const QRectF z = tool("options", false); iconGear(g, z.center(), z.width() * 0.6); }
}

void RecorderBar::openFile() {
    const QString f = QFileDialog::getOpenFileName(this, "Open audio file", QString(),
        "Audio / video files (*.wav *.aif *.aiff *.flac *.ogg *.mp3 *.m4a *.mp4 *.mov *.avi *.wma *.wmv *.opus *.mkv *.webm);;All files (*)");
    if (f.isEmpty()) return;
    std::string err;
    if (!ctx().tape->load(f.toStdString(), &err))
        QMessageBox::warning(this, "RoadBoard", "Cannot open this file:\n" + QString::fromStdString(err));
}

void RecorderBar::seekAt(double x) {
    TapeDeck* t = ctx().tape;
    if (!t || t->duration() <= 0) return;
    t->seek(std::clamp((x - progress_.left()) / progress_.width(), 0.0, 1.0) * t->duration());
}

void RecorderBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) { openTapeOptions(this); return; }
    if (Syncable::guiLocked() || !ctx().tape) return;
    TapeDeck* t = ctx().tape;
    const QString id = hit(e->position());
    pressed_ = id;
    if (id == "progress") { seeking_ = true; seekAt(e->position().x()); }
    else if (id == "track") { if (!t->recording()) openFile(); }
    else if (id == "play") {
        if (!t->loaded()) openFile();
        else if (t->playing() || t->paused()) t->pause();
        else t->play();
    }
    else if (id == "stop") t->stop();
    else if (id == "rew") t->skip(-10);
    else if (id == "ff") t->skip(10);
    else if (id == "rec") t->setRecord(!(t->recording() || params().rec.record->on()));
    else if (id == "loop") params().rec.loop->set(params().rec.loop->on() ? 0 : 1);
    else if (id == "open") openFile();
    else if (id == "options") openTapeOptions(this);
    else if (id.startsWith("chip")) { Param* p = params().rec.bus[id.mid(4).toInt()]; p->set(p->on() ? 0 : 1); }
    update();
}

void RecorderBar::mouseMoveEvent(QMouseEvent* e) {
    if (seeking_) { seekAt(e->position().x()); return; }
    const QString h = hit(e->position());
    if (h != hover_) {
        hover_ = h;
        setCursor(h.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
        static const std::map<QString, QString> tips = {
            {"rew", "Back 10 s"}, {"ff", "Forward 10 s"}, {"play", "Play / pause"}, {"stop", "Stop"},
            {"rec", "Record (source and format: right click or ⚙)"}, {"loop", "Loop playback"}, {"open", "Open an audio file"},
            {"options", "Recorder options"}, {"progress", "Click / drag to move in the file"}, {"track", "Click to open a file"}};
        auto it = tips.find(h);
        setToolTip(it != tips.end() ? it->second : h.startsWith("chip") ? QStringLiteral("Play the file on this output") : QString());
        update();
    }
}

void RecorderBar::mouseReleaseEvent(QMouseEvent*) {
    seeking_ = false;
    pressed_.clear();
    update();
}

} // namespace road
