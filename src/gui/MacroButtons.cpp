#include "gui/MacroButtons.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <chrono>
#include <cmath>

#include "core/RemoteScript.h"
#include "core/Settings.h"
#include "engine/Midi.h"
#include "engine/Mixer.h"
#include "gui/AppContext.h"
#include "gui/MainWindow.h"
#include "gui/Theme.h"

namespace road {

using namespace theme;

static const QColor kMacroColors[9] = {QColor(0x3a, 0x42, 0x4d), QColor(0x2f, 0xbf, 0x71), QColor(0x3d, 0x8b, 0xfd), QColor(0xe5, 0x48, 0x4d),
                                       QColor(0xf5, 0xc5, 0x18), QColor(0x9b, 0x7b, 0xff), QColor(0xf0, 0x8c, 0x00), QColor(0x20, 0xc9, 0xc9),
                                       QColor(0xe0, 0x5c, 0xb0)};

static double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

QJsonObject MacroButton::toJson() const {
    QJsonObject o;
    o["name"] = name; o["subName"] = subName; o["toggle"] = toggle; o["color"] = color;
    o["init"] = initScript; o["push"] = pushScript; o["release"] = releaseScript;
    o["key"] = key.toString(); o["midiStatus"] = midiStatus; o["midiD1"] = midiD1;
    o["triggerOn"] = triggerOn; o["triggerStrip"] = triggerStrip; o["triggerIn"] = triggerIn;
    o["triggerOut"] = triggerOut; o["triggerHoldMs"] = triggerHoldMs; o["event"] = event;
    o["state"] = state;
    return o;
}

MacroButton MacroButton::fromJson(const QJsonObject& o) {
    MacroButton b;
    b.name = o["name"].toString(); b.subName = o["subName"].toString(); b.toggle = o["toggle"].toBool();
    b.color = o["color"].toInt(); b.initScript = o["init"].toString(); b.pushScript = o["push"].toString();
    b.releaseScript = o["release"].toString(); b.key = QKeySequence(o["key"].toString());
    b.midiStatus = o["midiStatus"].toInt(-1); b.midiD1 = o["midiD1"].toInt(-1);
    b.triggerOn = o["triggerOn"].toBool(); b.triggerStrip = o["triggerStrip"].toInt();
    b.triggerIn = float(o["triggerIn"].toDouble(-30)); b.triggerOut = float(o["triggerOut"].toDouble(-40));
    b.triggerHoldMs = o["triggerHoldMs"].toInt(500); b.event = o["event"].toString();
    b.state = o["state"].toBool();
    return b;
}

// ------------------------------------------------------------------ button widget
class MacroButtonWidget : public QWidget {
public:
    MacroButtonWidget(MacroButtonsWindow* w, std::vector<MacroButton>* list, int idx)
        : QWidget(w), win_(w), list_(list), idx_(idx) {
        setMinimumSize(96, 64);
        setCursor(Qt::PointingHandCursor);
    }
    std::function<void()> onPress, onRelease, onConfigure;
protected:
    void paintEvent(QPaintEvent*) override {
        const MacroButton& b = (*list_)[size_t(idx_)];
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        QColor c = kMacroColors[std::clamp(b.color, 0, 8)];
        if (b.state) c = c.lighter(150);
        const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
        QLinearGradient lg(r.topLeft(), r.bottomLeft());
        lg.setColorAt(0, c.lighter(115));
        lg.setColorAt(1, c.darker(125));
        g.setBrush(lg);
        g.setPen(QPen(b.state ? title : border, b.state ? 2 : 1));
        g.drawRoundedRect(r, 6, 6);
        g.setPen(b.state ? bgDeep : text);
        g.setFont(mkFont(13, true));
        g.drawText(r.adjusted(4, 4, -4, -r.height() / 2), Qt::AlignCenter | Qt::TextWordWrap,
                   b.name.isEmpty() ? QStringLiteral("Button %1").arg(idx_ + 1) : b.name);
        g.setFont(mkFont(10));
        QString sub = b.subName;
        if (!b.key.isEmpty()) sub += (sub.isEmpty() ? "" : "  ") + QStringLiteral("[%1]").arg(b.key.toString());
        if (b.triggerOn) sub += " ⚡";
        g.drawText(r.adjusted(4, r.height() / 2, -4, -4), Qt::AlignCenter, sub);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::RightButton) { if (onConfigure) onConfigure(); return; }
        if (onPress) onPress();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && onRelease) onRelease();
    }
private:
    MacroButtonsWindow* win_;
    std::vector<MacroButton>* list_;
    int idx_;
};

// ------------------------------------------------------------------ window
MacroButtonsWindow::MacroButtonsWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle("RoadBoard — Macro Buttons");
    setWindowIcon(QIcon(":/icons/roadboard.svg"));
    setFocusPolicy(Qt::StrongFocus);
    auto* v = new QVBoxLayout(this);
    auto* bar = new QHBoxLayout;
    auto* menuBtn = new QPushButton("Menu");
    connect(menuBtn, &QPushButton::clicked, this, [this, menuBtn] {
        QMenu m(this);
        QMenu* layout = m.addMenu("Layout");
        for (auto [r, c] : std::vector<std::pair<int, int>>{{1, 4}, {2, 4}, {3, 4}, {4, 4}, {4, 5}, {5, 6}, {6, 8}, {8, 10}}) {
            QAction* a = layout->addAction(QStringLiteral("%1 x %2").arg(r).arg(c));
            a->setCheckable(true);
            a->setChecked(r == rows_ && c == cols_);
            connect(a, &QAction::triggered, this, [this, r, c] { rows_ = r; cols_ = c; buttons_.resize(size_t(r * c)); rebuild(); save(); });
        }
        connect(m.addAction("Save button map as..."), &QAction::triggered, this, [this] {
            QFile::copy(Settings::configDir() + "/macrobuttons.json", Settings::configDir() + "/macrobuttons-backup.json");
            save();
        });
        connect(m.addAction("Run all init scripts"), &QAction::triggered, this, [this] { runInitScripts(); });
        m.exec(menuBtn->mapToGlobal(QPoint(0, menuBtn->height())));
    });
    bar->addWidget(menuBtn);
    bar->addWidget(new QLabel("Left click: push · right click: configure · keyboard shortcuts work while this window has focus"));
    bar->addStretch(1);
    v->addLayout(bar);
    grid_ = new QGridLayout;
    grid_->setSpacing(4);
    v->addLayout(grid_, 1);
    load();
    rebuild();
    if (ctx().midi) {
        auto prev = ctx().midi->onMessage;
        ctx().midi->onMessage = [this, prev](int s, int a, int b) {
            if (prev) prev(s, a, b);
            QMetaObject::invokeMethod(this, [this, s, a, b] { onMidi(s, a, b); }, Qt::QueuedConnection);
        };
    }
    resize(560, 360);
}

void MacroButtonsWindow::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.fillRect(rect(), bg);
}

void MacroButtonsWindow::rebuild() {
    for (QWidget* w : widgets_) w->deleteLater();
    widgets_.clear();
    buttons_.resize(size_t(rows_ * cols_));
    for (int i = 0; i < rows_ * cols_; ++i) {
        auto* w = new MacroButtonWidget(this, &buttons_, i);
        w->onPress = [this, i] { press(i); };
        w->onRelease = [this, i] { release(i); };
        w->onConfigure = [this, i] { configure(i); };
        grid_->addWidget(w, i / cols_, i % cols_);
        widgets_.push_back(w);
    }
}

void MacroButtonsWindow::save() {
    QJsonArray arr;
    for (auto& b : buttons_) arr.append(b.toJson());
    QJsonObject root;
    root["rows"] = rows_; root["cols"] = cols_; root["buttons"] = arr;
    QFile f(Settings::configDir() + "/macrobuttons.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(root).toJson());
}

void MacroButtonsWindow::load() {
    QFile f(Settings::configDir() + "/macrobuttons.json");
    if (!f.open(QIODevice::ReadOnly)) { buttons_.resize(size_t(rows_ * cols_)); return; }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    rows_ = std::clamp(root["rows"].toInt(4), 1, 8);
    cols_ = std::clamp(root["cols"].toInt(4), 1, 10);
    buttons_.clear();
    for (const QJsonValue& v : root["buttons"].toArray()) buttons_.push_back(MacroButton::fromJson(v.toObject()));
    buttons_.resize(size_t(rows_ * cols_));
}

void MacroButtonsWindow::runInitScripts() {
    for (int i = 0; i < int(buttons_.size()); ++i)
        if (!buttons_[size_t(i)].initScript.trimmed().isEmpty()) runScript(buttons_[size_t(i)].initScript, i);
}

// Script runner with macro-button extensions (Wait, Button(i).State, Show, System.Execute)
void MacroButtonsWindow::runScript(const QString& script, int self) {
    QStringList stmts;
    for (QString line : script.split(QRegularExpression("[;\\n]"), Qt::SkipEmptyParts)) {
        const int c = line.indexOf("//");
        if (c >= 0) line = line.left(c);
        line = line.trimmed();
        if (!line.isEmpty()) stmts << line;
    }
    auto run = std::make_shared<std::function<void(int)>>();
    *run = [this, stmts, self, run](int from) {
        static const QRegularExpression waitRe(R"(^wait\s*\(\s*(\d+)\s*\)$)", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression btnRe(R"(^button\s*[\(\[]\s*(\d*)\s*[\)\]]\s*\.\s*(state|stateonly|trigger|color)\s*=\s*(\d+)$)", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression btnSelf(R"(^button\s*\.\s*state\s*=\s*(\d+)$)", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression showRe(R"(^show\s*\(\s*(\d)\s*\)$)", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression execRe(R"re(^system\.execute\s*\(\s*"([^"]*)"\s*(?:,\s*"([^"]*)")?\s*\)$)re", QRegularExpression::CaseInsensitiveOption);
        for (int i = from; i < stmts.size(); ++i) {
            const QString& s = stmts[i];
            QRegularExpressionMatch m;
            if ((m = waitRe.match(s)).hasMatch()) {
                QTimer::singleShot(m.captured(1).toInt(), this, [run, i] { (*run)(i + 1); });
                return;
            }
            if ((m = btnRe.match(s)).hasMatch()) {
                const int idx = m.captured(1).isEmpty() ? self : m.captured(1).toInt();
                const QString what = m.captured(2).toLower();
                const int v = m.captured(3).toInt();
                if (what == "state") setButtonState(idx, v != 0, true);
                else if (what == "stateonly") setButtonState(idx, v != 0, false);
                else if (what == "trigger") setTriggerEnabled(idx, v != 0);
                else setButtonColor(idx, v);
                continue;
            }
            if ((m = btnSelf.match(s)).hasMatch()) { setButtonState(self, m.captured(1).toInt() != 0, false); continue; }
            if ((m = showRe.match(s)).hasMatch()) { m.captured(1) == "0" ? hide() : show(); continue; }
            if ((m = execRe.match(s)).hasMatch()) {
                QStringList args = QProcess::splitCommand(m.captured(2));
                QProcess::startDetached(m.captured(1), args);
                continue;
            }
            remote().execute(s.toStdString());
        }
    };
    (*run)(0);
}

void MacroButtonsWindow::press(int i) {
    if (i < 0 || i >= int(buttons_.size())) return;
    MacroButton& b = buttons_[size_t(i)];
    if (b.toggle) {
        b.state = !b.state;
        runScript(b.state ? b.pushScript : b.releaseScript, i);
    } else {
        b.state = true;
        runScript(b.pushScript, i);
    }
    if (i < int(widgets_.size())) widgets_[size_t(i)]->update();
}

void MacroButtonsWindow::release(int i) {
    if (i < 0 || i >= int(buttons_.size())) return;
    MacroButton& b = buttons_[size_t(i)];
    if (b.toggle) return;
    b.state = false;
    runScript(b.releaseScript, i);
    if (i < int(widgets_.size())) widgets_[size_t(i)]->update();
}

void MacroButtonsWindow::setButtonState(int i, bool on, bool runScripts) {
    if (i < 0 || i >= int(buttons_.size())) return;
    MacroButton& b = buttons_[size_t(i)];
    if (runScripts) {
        if (on && !b.state) { if (b.toggle) press(i); else { b.state = true; runScript(b.pushScript, i); } }
        else if (!on && b.state) { if (b.toggle) press(i); else release(i); }
    }
    b.state = on;
    if (i < int(widgets_.size())) widgets_[size_t(i)]->update();
}

void MacroButtonsWindow::setTriggerEnabled(int i, bool on) {
    if (i >= 0 && i < int(buttons_.size())) { buttons_[size_t(i)].triggerOn = on; if (i < int(widgets_.size())) widgets_[size_t(i)]->update(); }
}
void MacroButtonsWindow::setButtonColor(int i, int color) {
    if (i >= 0 && i < int(buttons_.size())) { buttons_[size_t(i)].color = std::clamp(color, 0, 8); if (i < int(widgets_.size())) widgets_[size_t(i)]->update(); }
}

void MacroButtonsWindow::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    const QKeySequence seq(e->modifiers() | e->key());
    for (int i = 0; i < int(buttons_.size()); ++i)
        if (!buttons_[size_t(i)].key.isEmpty() && buttons_[size_t(i)].key == seq) press(i);
}
void MacroButtonsWindow::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    const QKeySequence seq(e->modifiers() | e->key());
    for (int i = 0; i < int(buttons_.size()); ++i)
        if (!buttons_[size_t(i)].key.isEmpty() && buttons_[size_t(i)].key == seq) release(i);
}

void MacroButtonsWindow::onMidi(int status, int d1, int d2) {
    const int kind = status & 0xF0;
    if (learnMidi_ >= 0 && (kind == 0x90 || kind == 0xB0)) {
        buttons_[size_t(learnMidi_)].midiStatus = (status & 0x0F) | 0x90 * (kind == 0x90) | 0xB0 * (kind == 0xB0);
        buttons_[size_t(learnMidi_)].midiD1 = d1;
        learnMidi_ = -1;
        save();
        return;
    }
    for (int i = 0; i < int(buttons_.size()); ++i) {
        const MacroButton& b = buttons_[size_t(i)];
        if (b.midiStatus < 0 || b.midiD1 != d1) continue;
        const int bk = b.midiStatus & 0xF0, bc = b.midiStatus & 0x0F;
        if ((status & 0x0F) != bc) continue;
        const bool isNote = kind == 0x90 || kind == 0x80;
        if ((bk == 0x90) != isNote) continue;
        const bool down = (kind == 0x90 && d2 > 0) || (kind == 0xB0 && d2 >= 64);
        if (down) press(i); else release(i);
    }
}

void MacroButtonsWindow::onTapeEvent(const QString& e) {
    for (int i = 0; i < int(buttons_.size()); ++i)
        if (!buttons_[size_t(i)].event.isEmpty() && buttons_[size_t(i)].event == e) press(i), release(i);
}

void MacroButtonsWindow::tick() {
    // audio trigger (auto ducking): IN threshold pushes, OUT threshold releases after HOLD
    if (!ctx().mixer) return;
    const double t = nowSec();
    for (int i = 0; i < int(buttons_.size()); ++i) {
        MacroButton& b = buttons_[size_t(i)];
        if (!b.triggerOn) continue;
        const int s = std::clamp(b.triggerStrip, 0, kStrips - 1);
        float pk = 0;
        for (int c = 0; c < stripChannels(s); ++c) pk = std::max(pk, ctx().mixer->meters().stripPre[s][c].load());
        const float db = pk > 1e-6f ? 20 * std::log10(pk) : -120;
        if (db >= b.triggerIn) {
            b.triggerHoldUntil = t + b.triggerHoldMs / 1000.0;
            if (!b.state) setButtonState(i, true, true);
        } else if (db < b.triggerOut && b.state && t > b.triggerHoldUntil) {
            setButtonState(i, false, true);
        }
    }
}

void MacroButtonsWindow::configure(int i) {
    MacroButton& b = buttons_[size_t(i)];
    QDialog d(this);
    d.setWindowTitle(QStringLiteral("Macro Button #%1").arg(i));
    auto* v = new QVBoxLayout(&d);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(b.name); form->addRow("Name", name);
    auto* sub = new QLineEdit(b.subName); form->addRow("Sub title", sub);
    auto* type = new QComboBox; type->addItems({"Push button", "2 position button"}); type->setCurrentIndex(b.toggle); form->addRow("Type", type);
    auto* color = new QComboBox;
    for (int c = 0; c < 9; ++c) { color->addItem(QStringLiteral("Color %1").arg(c)); color->setItemData(c, kMacroColors[c], Qt::DecorationRole); }
    color->setCurrentIndex(b.color); form->addRow("Button color", color);
    auto* key = new QKeySequenceEdit(b.key); form->addRow("Keyboard shortcut", key);
    auto* midiRow = new QHBoxLayout;
    auto* midiLbl = new QLabel(b.midiStatus < 0 ? QString("-") : QString::asprintf("%02X %02X", b.midiStatus, b.midiD1));
    auto* learn = new QPushButton("Learn");
    connect(learn, &QPushButton::clicked, &d, [this, i, midiLbl] { learnMidi_ = i; midiLbl->setText("move a MIDI control..."); });
    auto* reset = new QPushButton("Reset");
    connect(reset, &QPushButton::clicked, &d, [&b, midiLbl] { b.midiStatus = -1; b.midiD1 = -1; midiLbl->setText("-"); });
    midiRow->addWidget(midiLbl, 1); midiRow->addWidget(learn); midiRow->addWidget(reset);
    form->addRow("M.I.D.I.", midiRow);
    auto* trig = new QCheckBox("Audio trigger (auto ducking)"); trig->setChecked(b.triggerOn);
    auto* tStrip = new QComboBox;
    for (int s = 0; s < kStrips; ++s) tStrip->addItem(QStringLiteral("Strip %1").arg(s + 1));
    tStrip->setCurrentIndex(b.triggerStrip);
    auto* tIn = new QDoubleSpinBox; tIn->setRange(-80, 0); tIn->setValue(double(b.triggerIn)); tIn->setSuffix(" dB IN");
    auto* tOut = new QDoubleSpinBox; tOut->setRange(-80, 0); tOut->setValue(double(b.triggerOut)); tOut->setSuffix(" dB OUT");
    auto* tHold = new QSpinBox; tHold->setRange(0, 10000); tHold->setValue(b.triggerHoldMs); tHold->setSuffix(" ms hold");
    auto* tr = new QHBoxLayout; tr->addWidget(trig); tr->addWidget(tStrip); tr->addWidget(tIn); tr->addWidget(tOut); tr->addWidget(tHold);
    form->addRow("Trigger", tr);
    auto* ev = new QComboBox; ev->addItems({"", "play", "stop", "record", "eof"}); ev->setCurrentText(b.event);
    form->addRow("RoadBoard event (tape)", ev);
    v->addLayout(form);
    auto* tabs = new QTabWidget;
    auto* init = new QPlainTextEdit(b.initScript);
    auto* push = new QPlainTextEdit(b.pushScript);
    auto* rel = new QPlainTextEdit(b.releaseScript);
    tabs->addTab(push, "Request for button ON / trigger IN");
    tabs->addTab(rel, "Request for button OFF / trigger OUT");
    tabs->addTab(init, "Initial state request");
    v->addWidget(tabs, 1);
    v->addWidget(new QLabel("Examples: Strip(0).Mute=1;  Bus(0).Gain-=3;  Strip(1).FadeTo=(-20,500);  Wait(1000);\n"
                            "Button(3).State=1;  Recorder.Load=\"/path/jingle.wav\";  System.Execute(\"notify-send\",\"hello\");"));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    d.resize(720, 620);
    if (d.exec() != QDialog::Accepted) { learnMidi_ = -1; return; }
    b.name = name->text(); b.subName = sub->text(); b.toggle = type->currentIndex() == 1; b.color = color->currentIndex();
    b.key = key->keySequence(); b.triggerOn = trig->isChecked(); b.triggerStrip = tStrip->currentIndex();
    b.triggerIn = float(tIn->value()); b.triggerOut = float(tOut->value()); b.triggerHoldMs = tHold->value();
    b.event = ev->currentText();
    b.initScript = init->toPlainText(); b.pushScript = push->toPlainText(); b.releaseScript = rel->toPlainText();
    save();
    if (i < int(widgets_.size())) widgets_[size_t(i)]->update();
}

} // namespace road
