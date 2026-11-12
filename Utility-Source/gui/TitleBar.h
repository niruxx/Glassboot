#pragma once
#include <QAbstractButton>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <QVariantAnimation>
#include <QFocusEvent>
#include <QLinearGradient>

namespace glassboot {

// Vector glyphs and painted glass highlights remain crisp at every display scale.
class WindowButton final : public QAbstractButton {
public:
    enum Kind { Minimize, Maximize, Close };
    WindowButton(Kind kind, QWidget* owner, QWidget* parent)
        : QAbstractButton(parent), kind_(kind), owner_(owner) {
        setFixedSize(36, 32);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::ArrowCursor);
        hover_ = new QVariantAnimation(this);
        hover_->setDuration(170);
        hover_->setEasingCurve(QEasingCurve::OutCubic);
        connect(hover_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            highlight_ = value.toReal();
            update();
        });
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF surface = QRectF(rect()).adjusted(2.5, 2.5, -2.5, -2.5);
        if (highlight_ > 0.001 || isDown()) {
            const qreal strength = isDown() ? 1.0 : highlight_;
            QLinearGradient glass(surface.topLeft(), surface.bottomLeft());
            if (kind_ == Close) {
                glass.setColorAt(0, QColor(255, 231, 235, qRound(220 * strength)));
                glass.setColorAt(1, QColor(250, 204, 214, qRound(165 * strength)));
            } else {
                glass.setColorAt(0, QColor(255, 255, 255, qRound(235 * strength)));
                glass.setColorAt(1, QColor(202, 213, 250, qRound(150 * strength)));
            }
            painter.setBrush(glass);
            painter.setPen(QPen(QColor(255, 255, 255, qRound(210 * strength)), 1));
            painter.drawRoundedRect(surface, 10, 10);
        }
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor("#7184dc"), 1.2));
            painter.drawRoundedRect(surface, 10, 10);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(kind_ == Close && highlight_ > 0.1 ? QColor("#a73b50") : QColor("#43517c"), 1.5));
        const QPointF c = QRectF(rect()).center() + QPointF(0, isDown() ? 0.5 : 0);
        if (kind_ == Minimize) painter.drawLine(c + QPointF(-5, 3), c + QPointF(5, 3));
        else if (kind_ == Close) {
            painter.drawLine(c + QPointF(-4, -4), c + QPointF(4, 4));
            painter.drawLine(c + QPointF(4, -4), c + QPointF(-4, 4));
        } else if (owner_->isMaximized()) {
            // Draw the back window as an open corner to avoid an opaque patch.
            QPainterPath back;
            back.moveTo(c + QPointF(-2, -5));
            back.lineTo(c + QPointF(5, -5));
            back.lineTo(c + QPointF(5, 2));
            painter.drawPath(back);
            painter.drawRoundedRect(QRectF(c + QPointF(-5, -2), QSizeF(7, 7)), 0.7, 0.7);
        } else painter.drawRoundedRect(QRectF(c + QPointF(-4, -4), QSizeF(8, 8)), 0.7, 0.7);
    }
    void enterEvent(QEnterEvent* event) override { QAbstractButton::enterEvent(event); animateHighlight(); }
    void leaveEvent(QEvent* event) override { QAbstractButton::leaveEvent(event); animateHighlight(); }
    void focusInEvent(QFocusEvent* event) override { QAbstractButton::focusInEvent(event); animateHighlight(); }
    void focusOutEvent(QFocusEvent* event) override { QAbstractButton::focusOutEvent(event); animateHighlight(); }
private:
    void animateHighlight() {
        hover_->stop();
        hover_->setStartValue(highlight_);
        hover_->setEndValue(underMouse() || hasFocus() ? 1.0 : 0.0);
        hover_->start();
    }
    Kind kind_;
    QWidget* owner_;
    QVariantAnimation* hover_ = nullptr;
    qreal highlight_ = 0;
};

class TitleBar final : public QWidget {
public:
    explicit TitleBar(QWidget* owner) : QWidget(owner), owner_(owner) {
        setObjectName("titleBar");
        setAttribute(Qt::WA_StyledBackground);
        setFixedHeight(48);
        setStyleSheet("QWidget#titleBar { background: rgba(255, 255, 255, 70); border-bottom: 1px solid rgba(255, 255, 255, 150); }");
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(16, 7, 8, 7);
        row->setSpacing(4);
        auto* brand = new QLabel("Glassboot");
        brand->setStyleSheet("color: #303a63; font-weight: 600; font-size: 14px; background: transparent;");
        brand->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(brand);
        row->addStretch();
        auto* minimize = new WindowButton(WindowButton::Minimize, owner, this);
        maximize_ = new WindowButton(WindowButton::Maximize, owner, this);
        auto* close = new WindowButton(WindowButton::Close, owner, this);
        minimize->setObjectName("windowMinimize");
        maximize_->setObjectName("windowMaximize");
        close->setObjectName("windowClose");
        minimize->setAccessibleName(QCoreApplication::translate("glassboot::TitleBar", "Minimize"));
        close->setAccessibleName(QCoreApplication::translate("glassboot::TitleBar", "Close"));
        minimize->setToolTip(QCoreApplication::translate("glassboot::TitleBar", "Minimize"));
        close->setToolTip(QCoreApplication::translate("glassboot::TitleBar", "Close"));
        row->addWidget(minimize);
        row->addWidget(maximize_);
        row->addWidget(close);
        connect(minimize, &QAbstractButton::clicked, owner, &QWidget::showMinimized);
        connect(maximize_, &QAbstractButton::clicked, this, [this] { toggleMaximized(); });
        // Keep MainWindow's installation-in-progress close confirmation intact.
        connect(close, &QAbstractButton::clicked, owner, &QWidget::close);
        qApp->installEventFilter(this);
        QTimer::singleShot(0, this, [this] {
            owner_->setMouseTracking(true);
            for (auto* child : owner_->findChildren<QWidget*>()) child->setMouseTracking(true);
        });
        updateMaximize();
    }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        offset_ = event->globalPosition().toPoint() - owner_->frameGeometry().topLeft();
        dragging_ = !owner_->windowHandle()->startSystemMove();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ && (event->buttons() & Qt::LeftButton) && !owner_->isMaximized())
            owner_->move(event->globalPosition().toPoint() - offset_);
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = false; }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) { dragging_ = false; toggleMaximized(); }
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == owner_ && event->type() == QEvent::WindowStateChange) updateMaximize();
        auto* widget = qobject_cast<QWidget*>(object);
        if (!widget || widget->window() != owner_ || owner_->isMaximized() || owner_->isFullScreen())
            return false;
        if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress) return false;
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint position = owner_->mapFromGlobal(mouse->globalPosition().toPoint());
        Qt::Edges edges;
        if (position.x() < 5) edges |= Qt::LeftEdge;
        else if (position.x() >= owner_->width() - 5) edges |= Qt::RightEdge;
        if (position.y() < 5) edges |= Qt::TopEdge;
        else if (position.y() >= owner_->height() - 5) edges |= Qt::BottomEdge;
        if (event->type() == QEvent::MouseMove && !mouse->buttons()) {
            if (cursorWidget_ && cursorWidget_ != widget) cursorWidget_->unsetCursor();
            cursorWidget_ = edges ? widget : nullptr;
            if (edges == (Qt::TopEdge | Qt::LeftEdge) || edges == (Qt::BottomEdge | Qt::RightEdge))
                widget->setCursor(Qt::SizeFDiagCursor);
            else if (edges == (Qt::TopEdge | Qt::RightEdge) || edges == (Qt::BottomEdge | Qt::LeftEdge))
                widget->setCursor(Qt::SizeBDiagCursor);
            else if (edges & (Qt::LeftEdge | Qt::RightEdge)) widget->setCursor(Qt::SizeHorCursor);
            else if (edges & (Qt::TopEdge | Qt::BottomEdge)) widget->setCursor(Qt::SizeVerCursor);
            else widget->unsetCursor();
        }
        if (edges && event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton)
            return owner_->windowHandle()->startSystemResize(edges);
        return false;
    }
private:
    void toggleMaximized() { owner_->isMaximized() ? owner_->showNormal() : owner_->showMaximized(); }
    void updateMaximize() {
        if (cursorWidget_) cursorWidget_->unsetCursor();
        cursorWidget_.clear();
        const auto label = owner_->isMaximized() ? QCoreApplication::translate("glassboot::TitleBar", "Restore") : QCoreApplication::translate("glassboot::TitleBar", "Maximize");
        maximize_->setToolTip(label);
        maximize_->setAccessibleName(label);
        maximize_->update();
    }
    QWidget* owner_;
    QPointer<QWidget> cursorWidget_;
    WindowButton* maximize_ = nullptr;
    QPoint offset_;
    bool dragging_ = false;
};
} // namespace glassboot
