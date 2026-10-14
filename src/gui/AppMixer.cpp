#include "gui/AppMixer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <set>

#include "core/Settings.h"
#include "engine/Engine.h"
#include "gui/AppContext.h"
#include "gui/Theme.h"
#include "gui/Widgets.h"

namespace road {

using namespace theme;

namespace {

QString rulesFile() { return Settings::configDir() + "/apps.ini"; }

void saveRules() {
    QSettings s(rulesFile(), QSettings::IniFormat);
    s.remove("rules");
    s.beginGroup("rules");
    int i = 0;
    for (auto& [k, v] : ctx().engine->appRules()) {
        s.setValue(QStringLiteral("r%1/key").arg(i), QString::fromStdString(k));
        s.setValue(QStringLiteral("r%1/target").arg(i), QString::fromStdString(v));
        ++i;
    }
    s.endGroup();
}

QIcon appIcon(const AppStream& a) {
    for (const std::string& n : {a.icon, a.binary, a.appName}) {
        if (n.empty()) continue;
        QIcon i = QIcon::fromTheme(QString::fromStdString(n));
        if (!i.isNull()) return i;
        i = QIcon::fromTheme(QString::fromStdString(n).toLower());
        if (!i.isNull()) return i;
    }
    return QIcon::fromTheme(a.output ? "audio-speakers" : "audio-input-microphone");
}

QString shortName(const AudioDevice& d) {
    if (!d.virtualDevice) return QString::fromStdString(d.description);
    for (int v = 0; v < kVirtStrips; ++v)
        if (d.name == Engine::virtualInputName(v))
            return QStringLiteral("Virtual Input %1 — %2").arg(v + 1).arg(QString::fromStdString(d.description));
    return QString::fromStdString(d.description);
}

// one application row
class AppRow : public QWidget {
public:
    AppRow(const AppStream& s, QWidget* parent) : QWidget(parent), id_(s.id), app_(s.appName), output_(s.output) {
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(6, 3, 6, 3);
        auto* icon = new QLabel;
        icon->setPixmap(appIcon(s).pixmap(28, 28));
        icon->setFixedSize(30, 30);
        h->addWidget(icon);
        auto* names = new QVBoxLayout;
        names->setSpacing(0);
        auto* n = new QLabel("<b>" + QString::fromStdString(s.appName).toHtmlEscaped() + "</b>");
        auto* m = new QLabel(QString::fromStdString(s.mediaName).toHtmlEscaped());
        m->setStyleSheet("color:#7d8896; font-size:11px;");
        n->setMinimumWidth(170); n->setMaximumWidth(220);
        m->setMaximumWidth(220);
        names->addWidget(n);
        names->addWidget(m);
        h->addLayout(names);

        if (output_) {
            // one click buttons: Virtual Input 1 / 2 / 3
            for (int v = 0; v < kVirtStrips; ++v) {
                auto* b = new QPushButton(QString::number(v + 1));
                b->setCheckable(true);
                b->setFixedSize(34, 28);
                b->setToolTip(QString::fromStdString("Play on Virtual Input " + std::to_string(v + 1) + " (" + Engine::virtualInputDescription(v) + ")"));
                const std::string target = Engine::virtualInputName(v);
                connect(b, &QPushButton::clicked, this, [this, target] { route(target); });
                quick_.push_back({b, target});
                h->addWidget(b);
            }
        }
        combo_ = new QComboBox;
        combo_->setMinimumWidth(240);
        h->addWidget(combo_, 1);
        connect(combo_, &QComboBox::activated, this, [this](int i) { route(combo_->itemData(i).toString().toStdString()); });

        slider_ = new QSlider(Qt::Horizontal);
        slider_->setRange(0, 150);
        slider_->setFixedWidth(150);
        h->addWidget(slider_);
        pct_ = new QLabel;
        pct_->setFixedWidth(42);
        h->addWidget(pct_);
        connect(slider_, &QSlider::valueChanged, this, [this](int v) {
            pct_->setText(QStringLiteral("%1%").arg(v));
            if (updating_) return;
            const double x = v / 100.0;
            ctx().engine->setNodeVolume(id_, float(x * x * x));
        });
        mute_ = new QPushButton("M");
        mute_->setCheckable(true);
        mute_->setFixedSize(30, 28);
        connect(mute_, &QPushButton::toggled, this, [this](bool on) { if (!updating_) ctx().engine->setNodeMute(id_, on); });
        h->addWidget(mute_);
        remember_ = new QCheckBox("Remember");
        remember_->setToolTip("Always send this application to the chosen device when it starts");
        connect(remember_, &QCheckBox::toggled, this, [this](bool on) {
            if (updating_) return;
            ctx().engine->setAppRule(app_, output_, on ? current_ : std::string());
            saveRules();
        });
        h->addWidget(remember_);
        update(s);
    }

    uint32_t id() const { return id_; }

    void update(const AppStream& s) {
        updating_ = true;
        current_ = s.target;
        // destination list
        combo_->clear();
        const auto devs = output_ ? ctx().engine->allSinks() : ctx().engine->allSources();
        combo_->addItem("System default", QString());
        bool found = s.target.empty();
        for (const AudioDevice& d : devs) {
            combo_->addItem(shortName(d), QString::fromStdString(d.name));
            if (d.name == s.target) { combo_->setCurrentIndex(combo_->count() - 1); found = true; }
        }
        if (!found) { combo_->addItem(QString::fromStdString(s.targetDescription.empty() ? s.target : s.targetDescription), QString::fromStdString(s.target)); combo_->setCurrentIndex(combo_->count() - 1); }
        for (auto& [b, t] : quick_) b->setChecked(t == s.target);
        if (!slider_->isSliderDown()) slider_->setValue(int(std::lround(std::cbrt(s.volume) * 100)));
        pct_->setText(QStringLiteral("%1%").arg(slider_->value()));
        slider_->setEnabled(s.hasVolume);
        mute_->setChecked(s.mute);
        const auto rules = ctx().engine->appRules();
        remember_->setChecked(rules.count(Engine::ruleKey(app_, output_)) > 0);
        updating_ = false;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        g.setPen(QPen(border, 1));
        g.setBrush(panel);
        g.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    }

private:
    void route(const std::string& target) {
        current_ = target;
        ctx().engine->moveStream(id_, target);
        for (auto& [b, t] : quick_) b->setChecked(t == target);
        // choosing a destination remembers it for the next start of the application
        ctx().engine->setAppRule(app_, output_, target);
        saveRules();
        QSignalBlocker blk(remember_);
        remember_->setChecked(!target.empty());
    }
    uint32_t id_;
    std::string app_, current_;
    bool output_;
    bool updating_ = false;
    QComboBox* combo_;
    QSlider* slider_;
    QLabel* pct_;
    QPushButton* mute_;
    QCheckBox* remember_;
    std::vector<std::pair<QPushButton*, std::string>> quick_;
};

// one device row (Devices page)
class DeviceRow : public QWidget {
public:
    DeviceRow(const AudioDevice& d, bool sink, QWidget* parent) : QWidget(parent), id_(d.id), name_(d.name), sink_(sink) {
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(6, 2, 6, 2);
        auto* icon = new QLabel;
        icon->setPixmap(QIcon::fromTheme(d.virtualDevice ? "audio-card" : sink ? "audio-speakers" : "audio-input-microphone").pixmap(22, 22));
        h->addWidget(icon);
        auto* n = new QLabel((d.virtualDevice ? "<span style='color:#f5c518'>" : "<span>") +
                             QString::fromStdString(d.description).toHtmlEscaped() + "</span>");
        n->setMinimumWidth(280);
        h->addWidget(n, 1);
        auto* info = new QLabel(QStringLiteral("%1 ch  %2").arg(d.channels).arg(QString::fromStdString(d.api)));
        info->setStyleSheet("color:#7d8896;");
        h->addWidget(info);
        slider_ = new QSlider(Qt::Horizontal);
        slider_->setRange(0, 150);
        slider_->setFixedWidth(180);
        h->addWidget(slider_);
        pct_ = new QLabel;
        pct_->setFixedWidth(42);
        h->addWidget(pct_);
        connect(slider_, &QSlider::valueChanged, this, [this](int v) {
            pct_->setText(QStringLiteral("%1%").arg(v));
            if (updating_) return;
            const double x = v / 100.0;
            ctx().engine->setNodeVolume(id_, float(x * x * x));
        });
        mute_ = new QPushButton("M");
        mute_->setCheckable(true);
        mute_->setFixedSize(30, 26);
        connect(mute_, &QPushButton::toggled, this, [this](bool on) { if (!updating_) ctx().engine->setNodeMute(id_, on); });
        h->addWidget(mute_);
        auto* def = new QPushButton("Default");
        def->setToolTip(sink ? "Make this the system playback device" : "Make this the system recording device");
        connect(def, &QPushButton::clicked, this, [this] { ctx().engine->setDefaultNode(name_, sink_); });
        h->addWidget(def);
        update(d);
    }
    void update(const AudioDevice& d) {
        updating_ = true;
        if (!slider_->isSliderDown()) slider_->setValue(int(std::lround(std::cbrt(d.volume) * 100)));
        pct_->setText(QStringLiteral("%1%").arg(slider_->value()));
        slider_->setEnabled(d.hasVolume);
        mute_->setChecked(d.mute);
        updating_ = false;
    }
    uint32_t id() const { return id_; }
private:
    uint32_t id_;
    std::string name_;
    bool sink_, updating_ = false;
    QSlider* slider_;
    QLabel* pct_;
    QPushButton* mute_;
};

class AppMixerWindow : public QWidget {
public:
    explicit AppMixerWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
        setWindowTitle("RoadBoard — Application Mixer");
        setWindowIcon(QIcon(":/icons/roadboard.svg"));
        auto* v = new QVBoxLayout(this);
        auto* tabs = new QTabWidget;
        v->addWidget(tabs);
        auto page = [&](const QString& name, QVBoxLayout** a, QVBoxLayout** b, const QString& ta, const QString& tb) {
            auto* scroll = new QScrollArea;
            scroll->setWidgetResizable(true);
            auto* w = new QWidget;
            auto* l = new QVBoxLayout(w);
            l->addWidget(new Caption(ta, title, 12, Qt::AlignLeft | Qt::AlignVCenter, w));
            *a = new QVBoxLayout; (*a)->setSpacing(3);
            l->addLayout(*a);
            l->addSpacing(10);
            l->addWidget(new Caption(tb, title, 12, Qt::AlignLeft | Qt::AlignVCenter, w));
            *b = new QVBoxLayout; (*b)->setSpacing(3);
            l->addLayout(*b);
            l->addStretch(1);
            scroll->setWidget(w);
            tabs->addTab(scroll, name);
        };
        page("Applications", &playLay_, &recLay_, "PLAYBACK — where each application plays (1 / 2 / 3 = Virtual Input 1 / 2 / 3)",
             "RECORDING — what each application records");
        page("Devices", &outLay_, &inLay_, "OUTPUTS (playback devices)", "INPUTS (recording devices)");
        auto* hint = new QLabel("Applications sent to a RoadBoard virtual input are mixed by the console (strip 6, 7, 8). "
                                "The choice is remembered for the next time the application starts.");
        hint->setWordWrap(true);
        hint->setStyleSheet("color:#7d8896;");
        v->addWidget(hint);
        connect(&t_, &QTimer::timeout, this, [this] { refresh(); });
        t_.start(400);
        refresh();
        resize(1100, 640);
    }

    void refresh() {
        if (!ctx().engine) return;
        const auto streams = ctx().engine->streams();
        std::vector<AppStream> play, rec;
        for (auto& s : streams) (s.output ? play : rec).push_back(s);
        sync(playLay_, playRows_, play);
        sync(recLay_, recRows_, rec);
        syncDev(outLay_, outRows_, ctx().engine->allSinks(), true);
        syncDev(inLay_, inRows_, ctx().engine->allSources(), false);
    }

private:
    void sync(QVBoxLayout* lay, std::vector<AppRow*>& rows, const std::vector<AppStream>& list) {
        std::vector<uint32_t> ids;
        for (auto& s : list) ids.push_back(s.id);
        std::vector<uint32_t> have;
        for (auto* r : rows) have.push_back(r->id());
        if (ids != have) {
            for (auto* r : rows) r->deleteLater();
            rows.clear();
            for (auto& s : list) { auto* r = new AppRow(s, this); lay->addWidget(r); rows.push_back(r); }
            return;
        }
        // same streams: update only rows whose state changed (keeps combo boxes usable)
        for (size_t i = 0; i < rows.size(); ++i) {
            const AppStream& s = list[i];
            const std::string sig = s.target + "|" + std::to_string(s.volume) + "|" + std::to_string(s.mute);
            if (sigs_[s.id] != sig) { sigs_[s.id] = sig; rows[i]->update(s); }
        }
    }
    void syncDev(QVBoxLayout* lay, std::vector<DeviceRow*>& rows, const std::vector<AudioDevice>& list, bool sink) {
        std::vector<uint32_t> ids, have;
        for (auto& d : list) ids.push_back(d.id);
        for (auto* r : rows) have.push_back(r->id());
        if (ids != have) {
            for (auto* r : rows) r->deleteLater();
            rows.clear();
            for (auto& d : list) { auto* r = new DeviceRow(d, sink, this); lay->addWidget(r); rows.push_back(r); }
            return;
        }
        for (size_t i = 0; i < rows.size(); ++i) rows[i]->update(list[i]);
    }
    QVBoxLayout *playLay_, *recLay_, *outLay_, *inLay_;
    std::vector<AppRow*> playRows_, recRows_;
    std::vector<DeviceRow*> outRows_, inRows_;
    std::map<uint32_t, std::string> sigs_;
    QTimer t_;
};

AppMixerWindow* gApps = nullptr;

} // namespace

void openAppMixer(QWidget* parent) {
    if (!gApps) {
        gApps = new AppMixerWindow(parent);
        gApps->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(gApps, &QObject::destroyed, [] { gApps = nullptr; });
    }
    gApps->show();
    gApps->raise();
    gApps->activateWindow();
}

void loadAppRules() {
    if (!ctx().engine) return;
    QSettings s(rulesFile(), QSettings::IniFormat);
    s.beginGroup("rules");
    std::map<std::string, std::string> rules;
    for (const QString& g : s.childGroups()) {
        const QString k = s.value(g + "/key").toString(), t = s.value(g + "/target").toString();
        if (!k.isEmpty() && !t.isEmpty()) rules[k.toStdString()] = t.toStdString();
    }
    ctx().engine->setAppRules(rules);
}

} // namespace road
