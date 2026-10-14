// Integrated recorder / player as a transport bar (bottom of the main window).
#pragma once

#include <QWidget>
#include <map>

namespace road {

class Knob;

class RecorderBar : public QWidget {
    Q_OBJECT
public:
    explicit RecorderBar(QWidget* parent = nullptr);
    void tick();
    QSize sizeHint() const override { return {1200, 84}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override { hover_.clear(); update(); }

private:
    void layoutZones();
    QString hit(QPointF p) const;
    void openFile();
    void seekAt(double x);

    std::map<QString, QRectF> zones_;     // hit areas: "play", "stop", "rec", "chipN", "progress", ...
    QString hover_, pressed_;
    QRectF deck_, track_, progress_, chipsArea_;
    Knob* gain_ = nullptr;
    float reel_ = 0;
    int pulse_ = 0;
    bool seeking_ = false;
};

} // namespace road
