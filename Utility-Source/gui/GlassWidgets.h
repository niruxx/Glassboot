#pragma once
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QGuiApplication>
#include <QPropertyAnimation>
#include <QTimer>
#include <QEnterEvent>
#include <QFrame>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVariantAnimation>
#include <cmath>
#include <functional>

namespace glassboot {

inline int motionDuration(int milliseconds) {
    return qEnvironmentVariableIntValue("GLASSBOOT_REDUCED_MOTION") ? 0 : milliseconds;
}

// An internal, painted backdrop: glass remains consistent without depending on
// compositor blur APIs or sampling the user's desktop.
class GlassCanvas final : public QWidget {
public:
    explicit GlassCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        // Ambient movement stays at 20 fps; short control animations remain smooth.
        flow_.setInterval(50);
        connect(&flow_, &QTimer::timeout, this, [this] {
            phase_ = std::fmod(phase_ + 6.283185307 * 50 / 24000, 6.283185307);
            update();
        });
    }
protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        if (!qEnvironmentVariableIntValue("GLASSBOOT_REDUCED_MOTION")) flow_.start();
    }
    void hideEvent(QHideEvent* event) override { flow_.stop(); QWidget::hideEvent(event); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient base(0, 0, width(), height());
        base.setColorAt(0, QColor("#e8effb"));
        base.setColorAt(0.52, QColor("#f3effb"));
        base.setColorAt(1, QColor("#e2f2f5"));
        p.fillRect(rect(), base);
        auto light = [&](QPointF center, qreal radius, QColor tint) {
            QRadialGradient glow(center, radius);
            glow.setColorAt(0, tint);
            tint.setAlpha(0);
            glow.setColorAt(1, tint);
            p.fillRect(rect(), glow);
        };
        const qreal span = qMax(width(), height());
        light(QPointF(width() * (0.16 + 0.05 * std::sin(phase_)), height() * 0.20), span * .67, QColor(133, 170, 255, 135));
        light(QPointF(width() * .93, height() * (.44 + .05 * std::cos(phase_))), span * .63, QColor(185, 151, 246, 122));
        light(QPointF(width() * (.43 + .08 * std::cos(phase_)), height() * .97), span * .55, QColor(122, 221, 224, 90));
        light(QPointF(width() * .72, height() * .11), span * .38, QColor(255, 255, 255, 195));
        // A restrained light arc gives the flat window a sense of depth.
        p.setPen(QPen(QColor(255, 255, 255, 90), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(-width() * .4, height() * .48, width() * 1.7, height() * 1.3));
    }
private:
    QTimer flow_;
    qreal phase_ = 0;
};

class GlassPanel final : public QFrame {
public:
    explicit GlassPanel(QWidget* parent = nullptr) : QFrame(parent) {
        setAttribute(Qt::WA_TranslucentBackground);
        sheen_.setStartValue(0.0);
        sheen_.setEndValue(1.0);
        sheen_.setDuration(motionDuration(650));
        sheen_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&sheen_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            sheenPosition_ = value.toReal(); update();
        });
    }
    void animateChange() { sheen_.stop(); sheen_.start(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF surface = QRectF(rect()).adjusted(5, 5, -5, -7);
        for (int spread = 5; spread > 0; --spread) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(73, 81, 130, 3));
            p.drawRoundedRect(surface.adjusted(-spread, -spread + 3, spread, spread + 3), 27, 27);
        }
        QPainterPath shape;
        shape.addRoundedRect(surface, 26, 26);
        QLinearGradient glass(surface.topLeft(), surface.bottomRight());
        glass.setColorAt(0, QColor(255, 255, 255, 205));
        glass.setColorAt(.5, QColor(255, 255, 255, 132));
        glass.setColorAt(1, QColor(238, 245, 255, 180));
        p.fillPath(shape, glass);
        p.setPen(QPen(QColor(255, 255, 255, 230), 1.4));
        p.drawPath(shape);
        p.setPen(QPen(QColor(119, 139, 196, 35), 1));
        p.drawRoundedRect(surface.adjusted(1.5, 1.5, -1.5, -1.5), 24, 24);
        if (sheen_.state() == QAbstractAnimation::Running) {
            p.save(); p.setClipPath(shape);
            const qreal x = -width() + width() * 3 * sheenPosition_;
            QLinearGradient ray(x - width() * .3, 0, x + width() * .3, height());
            ray.setColorAt(0, QColor(255, 255, 255, 0));
            ray.setColorAt(.5, QColor(255, 255, 255, 90));
            ray.setColorAt(1, QColor(255, 255, 255, 0));
            p.fillRect(rect(), ray); p.restore();
        }
    }
private:
    QVariantAnimation sheen_;
    qreal sheenPosition_ = 1;
};

class GlassButton final : public QPushButton {
public:
    explicit GlassButton(const QString& text, QWidget* parent = nullptr) : QPushButton(text, parent) {
        setCursor(Qt::PointingHandCursor);
        hover_.setDuration(motionDuration(180));
        hover_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&hover_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            warmth_ = value.toReal(); update();
        });
        ripple_.setDuration(motionDuration(450));
        ripple_.setStartValue(0.0); ripple_.setEndValue(1.0);
        connect(&ripple_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            ripplePosition_ = value.toReal(); update();
        });
        connect(this, &QPushButton::pressed, this, [this] { ripple_.stop(); ripple_.start(); });
    }
protected:
    void enterEvent(QEnterEvent* event) override { QPushButton::enterEvent(event); warmTo(1); }
    void leaveEvent(QEvent* event) override { QPushButton::leaveEvent(event); warmTo(0); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool primary = property("primary").toBool() || isDefault();
        const QRectF face = QRectF(rect()).adjusted(2, 2, -2, -3);
        QPainterPath path; path.addRoundedRect(face, 19, 19);
        const qreal heat = isEnabled() ? warmth_ : 0;
        QLinearGradient fill(face.topLeft(), face.bottomRight());
        if (!isEnabled()) {
            fill.setColorAt(0, QColor(255, 255, 255, 125)); fill.setColorAt(1, QColor(212, 222, 242, 140));
        } else if (primary) {
            fill.setColorAt(0, QColor(90 - int(heat * 8), 116, 208));
            fill.setColorAt(.55, QColor(86, 98 - int(heat * 6), 194));
            fill.setColorAt(1, QColor(126, 88, 185));
        } else {
            fill.setColorAt(0, QColor(255, 255, 255, 205 + int(heat * 40)));
            fill.setColorAt(1, QColor(229, 238, 255, 140 + int(heat * 70)));
        }
        p.fillPath(path, fill);
        p.setPen(QPen(QColor(255, 255, 255, primary ? 140 : 240), 1.2)); p.drawPath(path);
        p.save(); p.setClipPath(path);
        QLinearGradient shine(0, face.top(), 0, face.bottom());
        shine.setColorAt(0, QColor(255, 255, 255, primary ? 65 : 90));
        shine.setColorAt(.46, QColor(255, 255, 255, 0));
        shine.setColorAt(1, QColor(255, 255, 255, 0));
        p.fillRect(face, shine);
        if (ripple_.state() == QAbstractAnimation::Running && isEnabled()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, int(65 * (1 - ripplePosition_))));
            const qreal radius = width() * ripplePosition_;
            p.drawEllipse(face.center(), radius, radius);
        }
        if (isDown()) p.fillRect(face, QColor(51, 60, 110, 25));
        p.restore();
        if (hasFocus()) {
            p.setPen(QPen(QColor("#5267c8"), 1.4)); p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(.8, .8, -.8, -.8), 21, 21);
        }
        p.setPen(!isEnabled() ? QColor("#71809b") : primary ? Qt::white : QColor("#34466e"));
        QFont labelFont = font(); labelFont.setWeight(QFont::DemiBold); p.setFont(labelFont);
        p.drawText(face.translated(0, isDown() ? 1 : 0), Qt::AlignCenter | Qt::TextShowMnemonic, text());
    }
private:
    void warmTo(qreal value) {
        hover_.stop(); hover_.setStartValue(warmth_); hover_.setEndValue(value); hover_.start();
    }
    QVariantAnimation hover_, ripple_;
    qreal warmth_ = 0, ripplePosition_ = 1;
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
