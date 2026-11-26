#pragma once
#include <QApplication>
#include <QDialog>
#include <QGuiApplication>
#include <QPropertyAnimation>
#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>
#include <cmath>
#include <functional>

namespace glassboot {

inline int motionDuration(int milliseconds) {
    return qEnvironmentVariableIntValue("GLASSBOOT_REDUCED_MOTION") ? 0 : milliseconds;
}

// The dot grid and contour lines move behind the opaque controls.
class GlassCanvas final : public QWidget {
public:
    explicit GlassCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        flow_.setInterval(50);
        connect(&flow_, &QTimer::timeout, this, [this] {
            phase_ = (phase_ + 1) % 480;
            update();
        });
    }
protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        if (motionDuration(50)) flow_.start();
    }
    void hideEvent(QHideEvent* event) override {
        flow_.stop();
        QWidget::hideEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette().window());
        painter.setRenderHint(QPainter::Antialiasing);

        constexpr int spacing = 28;
        const qreal drift = spacing * phase_ / 480.0;
        QColor accent = palette().highlight().color();
        accent.setAlpha(40);
        painter.setPen(QPen(accent, 2, Qt::SolidLine, Qt::RoundCap));
        for (int y = -spacing; y < height(); y += spacing) {
            for (int x = -spacing; x < width(); x += spacing)
                painter.drawPoint(QPointF(x + drift, y + drift));
        }

        constexpr qreal cycle = 6.283185307179586;
        const qreal angle = cycle * phase_ / 480.0;
        accent.setAlpha(48);
        painter.setPen(QPen(accent, 1));
        for (int line = 0; line < 4; ++line) {
            QPainterPath contour;
            const qreal offset = height() * .66 + line * 18;
            contour.moveTo(0, offset + 26 * std::sin(angle + line * .17));
            for (int x = 8; x < width() + 8; x += 8)
                contour.lineTo(x, offset + 26 * std::sin(x / 145.0 + angle + line * .17));
            painter.drawPath(contour);
        }
    }
private:
    QTimer flow_;
    int phase_ = 0;
};

// Keep transient windows in the same motion language without touching child paint effects.
class DialogMotion final : public QObject {
public:
    explicit DialogMotion(QObject* parent) : QObject(parent) { qApp->installEventFilter(this); }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() != QEvent::Show || !motionDuration(180)) return false;
        auto* widget = qobject_cast<QWidget*>(object);
        if (!widget || !widget->isWindow() || (!qobject_cast<QDialog*>(widget) && widget->windowType() != Qt::Popup)) return false;
        const QString platform = QGuiApplication::platformName();
        if (platform == "offscreen" || platform == "minimal" || platform.startsWith("wayland")) return false;
        auto* reveal = new QPropertyAnimation(widget, "windowOpacity", widget);
        reveal->setStartValue(.35); reveal->setEndValue(1.0);
        reveal->setDuration(motionDuration(180));
        reveal->setEasingCurve(QEasingCurve::OutCubic);
        reveal->start(QAbstractAnimation::DeleteWhenStopped);
        return false;
    }
};

// Blend snapshots rather than nesting graphics effects, which can corrupt Qt's
// repaint cache when controls are enabled/disabled after an installation.
class SnapshotFade final : public QWidget {
public:
    SnapshotFade(QWidget* area, QPixmap snapshot) : QWidget(area), snapshot_(std::move(snapshot)) {
        setObjectName("glassTransition");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setGeometry(area->rect());
        animation_.setStartValue(1.0); animation_.setEndValue(0.0);
        animation_.setDuration(motionDuration(240));
        connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            opacity_ = value.toReal(); update();
        });
        connect(&animation_, &QVariantAnimation::finished, this, &QObject::deleteLater);
        show(); raise(); animation_.start();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setOpacity(opacity_); p.drawPixmap(rect(), snapshot_);
    }
private:
    QPixmap snapshot_;
    QVariantAnimation animation_;
    qreal opacity_ = 1;
};
inline void crossfade(QWidget* area, const std::function<void()>& change) {
    for (auto* old : area->findChildren<QWidget*>("glassTransition", Qt::FindDirectChildrenOnly)) delete old;
    if (!area->isVisible() || !motionDuration(240)) { change(); return; }
    const QPixmap snapshot = area->grab();
    change();
    new SnapshotFade(area, snapshot);
}
} // namespace glassboot
