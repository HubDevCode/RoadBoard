// Custom painted controls bound to ParamStore parameters.
#pragma once

#include <QColor>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QHBoxLayout>
#include <QWidget>
#include <functional>
#include <vector>

#include "core/Params.h"

namespace road {

// Widgets that mirror a parameter. syncAll() is called by the GUI timer so that
// any change (mouse, MIDI, network, script) is reflected immediately.
class Syncable {
public:
    Syncable();
    virtual ~Syncable();
    virtual void sync() = 0;
    static void syncAll();
    static bool guiLocked();
};

class Knob : public QWidget, public Syncable {
    Q_OBJECT
public:
    Knob(Param* p, const QString& label, QColor color, QWidget* parent = nullptr);
    void sync() override;
    void setDecimals(int d) { decimals_ = d; }
    void setUnit(const QString& u) { unit_ = u; }
    // compact knobs (FX sends / returns): no value line, small label
    void setCompact(bool c) { compact_ = c; setMinimumSize(c ? 24 : 36, c ? 34 : 64); updateGeometry(); }
    // optional POST toggle: clicking the label switches it
    void setPostParam(Param* p) { post_ = p; }
    // replaces the default right click (type a value)
    void setRightClick(std::function<void(QMouseEvent*)> f) { rightClick_ = std::move(f); }
    void setBadge(std::function<QString()> f) { badge_ = std::move(f); }
    QSize sizeHint() const override { return compact_ ? QSize(30, 38) : QSize(48, 68); }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void enterEvent(QEnterEvent*) override { hover_ = true; update(); }
    void leaveEvent(QEvent*) override { hover_ = false; update(); }
private:
    Param* p_;
    Param* post_ = nullptr;
    QString label_, unit_;
    QColor color_;
    float last_ = -1e9f, lastPost_ = -1, dragStartV_ = 0;
    int dragStartY_ = 0, decimals_ = 1;
    bool dragging_ = false, hover_ = false, compact_ = false;
    std::function<void(QMouseEvent*)> rightClick_;
    std::function<QString()> badge_;
};

class Fader : public QWidget, public Syncable {
    Q_OBJECT
public:
    explicit Fader(Param* p, QWidget* parent = nullptr);
    void sync() override;
    void setParam(Param* p, const QString& caption = QString(), QColor captionColor = QColor()) {
        if (p != p_ || caption != caption_) { p_ = p; caption_ = caption; captionColor_ = captionColor; last_ = -1e9f; update(); }
    }
    Param* param() const { return p_; }
    QSize sizeHint() const override { return {34, 200}; }
    QSize minimumSizeHint() const override { return {30, 120}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    float valueToY(float v) const;
    float yToValue(float y) const;
    QRectF trackRect() const;
    Param* p_;
    QString caption_;
    QColor captionColor_;
    float last_ = -1e9f, dragStartV_ = 0;
    int dragStartY_ = 0;
    bool dragging_ = false;
};

class LevelMeter : public QWidget {
    Q_OBJECT
public:
    using Source = std::function<float(int ch)>;    // returns linear peak since last call
    LevelMeter(int channels, Source src, QWidget* parent = nullptr);
    void setChannels(int n) { if (n != ch_) { ch_ = n; level_.assign(size_t(n), -120); hold_.assign(size_t(n), -120); holdT_.assign(size_t(n), 0); update(); } }
    int channels() const { return ch_; }
    // strip / bus limiter threshold drawn on the meter (click to set)
    void setLimit(Param* limit, Param* type, float maxValue) { limit_ = limit; limType_ = type; limitMax_ = maxValue; }
    void setGainReduction(std::function<float()> gr) { gr_ = std::move(gr); }
    void tick();
    QSize sizeHint() const override { return {14, 200}; }
    static float dbToPos(float db);   // 0 (bottom, -60) .. 1 (top, +12)
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    void setLimitFromY(int y);
    int ch_;
    Source src_;
    std::vector<float> level_, hold_;
    std::vector<int> holdT_;
    Param* limit_ = nullptr;
    Param* limType_ = nullptr;
    float limitMax_ = 12, lastLimit_ = 1e9f;
    std::function<float()> gr_;
    float grShown_ = 1;
};

class ToggleButton : public QWidget, public Syncable {
    Q_OBJECT
public:
    // states: labels per value (size 0/1 = simple on/off with `text`)
    ToggleButton(Param* p, const QString& text, QColor onColor, QWidget* parent = nullptr);
    void setStates(const QStringList& labels, const std::vector<QColor>& colors);
    void setIndicator(std::function<std::vector<QColor>()> f) { indicator_ = std::move(f); }
    void setMomentary(std::function<void()> action) { action_ = std::move(action); }
    void setTextFn(std::function<QString()> f) { textFn_ = std::move(f); }
    void sync() override;
    QSize sizeHint() const override { return {40, 20}; }
signals:
    void rightClicked(QPoint globalPos);
    void clicked();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    Param* p_;
    QString text_;
    QColor on_;
    QStringList states_;
    std::vector<QColor> stateColors_;
    std::function<std::vector<QColor>()> indicator_;
    std::function<void()> action_;
    std::function<QString()> textFn_;
    float last_ = -1e9f;
    bool down_ = false;
};

// 2D panel (RoadPan / 5.1 pan pot) with several pages
class Pad2D : public QWidget, public Syncable {
    Q_OBJECT
public:
    struct Page {
        Param* x; Param* y;
        QString name;
        QColor top, bottom;
        QString left, right, up, down;
        bool showSpeakers = false;
    };
    explicit Pad2D(QWidget* parent = nullptr);
    void addPage(const Page& p) { pages_.push_back(p); }
    void setPage(int i) { page_ = std::clamp(i, 0, int(pages_.size()) - 1); update(); }
    int page() const { return page_; }
    int pageCount() const { return int(pages_.size()); }
    const Page& currentPage() const { return pages_[size_t(page_)]; }
    void setIndicators(bool on) { indicators_ = on; }
    void setOverlayText(std::function<QString()> f) { overlay_ = std::move(f); }
    void sync() override;
    QSize sizeHint() const override { return {96, 82}; }
signals:
    void contextMenuRequested(QPoint globalPos);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    void setFromPos(QPointF p);
    QRectF area() const;
    std::vector<Page> pages_;
    int page_ = 0;
    bool indicators_ = false;
    std::vector<float> last_;
    std::function<QString()> overlay_;
};

// clickable area showing a device name (top of strips, A1..A3 selectors)
class DeviceButton : public QWidget {
    Q_OBJECT
public:
    DeviceButton(const QString& caption, QWidget* parent = nullptr);
    void setDevice(const QString& name, bool connected);
    void setCaption(const QString& c) { caption_ = c; update(); }
    void setAccent(QColor c) { accent_ = c; update(); }
    // coloured badge on the left ("1", "V2", "A3"...) that identifies the channel
    void setBadge(const QString& text, QColor color) { badge_ = text; badgeColor_ = color; update(); }
    void setPlaceholder(const QString& p) { placeholder_ = p; update(); }
    QSize sizeHint() const override { return {100, 36}; }
signals:
    void clicked(QPoint globalPos);
    void rightClicked(QPoint globalPos);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override { hover_ = true; update(); }
    void leaveEvent(QEvent*) override { hover_ = false; update(); }
private:
    QString caption_, device_, badge_, placeholder_ = QStringLiteral("select device…");
    bool connected_ = false, hover_ = false;
    QColor accent_, badgeColor_;
};

// Rounded group with a coloured title bar (HARDWARE INPUTS, VIRTUAL OUTPUTS...)
class GroupPanel : public QWidget {
public:
    GroupPanel(const QString& title, const QString& subtitle, QColor accent, QWidget* parent = nullptr);
    QHBoxLayout* row() const { return row_; }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QString title_, subtitle_;
    QColor accent_;
    QHBoxLayout* row_;
};

// label bound to a string parameter, double click to edit
class EditableLabel : public QWidget, public Syncable {
    Q_OBJECT
public:
    EditableLabel(const std::string& key, const QString& placeholder, QWidget* parent = nullptr);
    void sync() override;
    void setPlaceholder(const QString& p) { placeholder_ = p; update(); }
    QSize sizeHint() const override { return {100, 18}; }
signals:
    void rightClicked(QPoint globalPos);
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    std::string key_;
    QString placeholder_, text_;
    uint32_t rev_ = ~0u;
};

// small caption text
class Caption : public QWidget {
public:
    Caption(const QString& text, QColor color, int px, Qt::Alignment align = Qt::AlignCenter, QWidget* parent = nullptr);
    void setText(const QString& t) { text_ = t; update(); }
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QString text_;
    QColor color_;
    int px_;
    Qt::Alignment align_;
};

QString formatDb(float v);

} // namespace road
