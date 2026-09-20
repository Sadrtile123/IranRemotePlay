// Phase 5 — client video surface implementation. See VideoWidget.h.

#include "VideoWidget.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <chrono>
#include <cstring>

namespace rp::ui {
namespace {
uint64_t steadyNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

VideoWidget::VideoWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(QSize(320, 180));
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFocusPolicy(Qt::StrongFocus);       // receive key events while streaming
    setMouseTracking(true);                // mouseMoved without buttons pressed
}

QSize VideoWidget::sizeHint() const {
    return frame_.isNull() ? QSize(1280, 720) : frame_.size();
}

void VideoWidget::presentFrame(const uint8_t* bgra, int width, int height, int stride) {
    // Copy into a tight QImage (stride == width*4) for simple reuse in paint.
    if (frame_.width() != width || frame_.height() != height) {
        frame_ = QImage(width, height, QImage::Format_ARGB32);
    }
    if (stride == width * 4) {
        std::memcpy(frame_.bits(), bgra, static_cast<size_t>(width) * height * 4);
    } else {
        for (int y = 0; y < height; ++y) {
            std::memcpy(frame_.scanLine(y), bgra + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
        }
    }
    ++framesPresented_;

    const uint64_t now = steadyNowNs();
    if (lastFpsUpdateNs_ == 0) lastFpsUpdateNs_ = now;
    else if (now - lastFpsUpdateNs_ >= 500'000'000ull) {
        fpsEstimate_ = static_cast<double>(framesPresented_) * 1e9 / static_cast<double>(now - lastFpsUpdateNs_);
        framesPresented_ = 0;
        lastFpsUpdateNs_ = now;
    }
    update();
}

void VideoWidget::setQualityBanner(const QString& text) {
    if (qualityBanner_ == text) return;
    qualityBanner_ = text;
    update();
}

QRect VideoWidget::videoDestRect() const {
    if (frame_.isNull()) return rect();
    const QSize s = frame_.size().scaled(rect().size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
}

void VideoWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(6, 7, 10));

    if (!frame_.isNull()) {
        const QRect dest = videoDestRect();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);   // crisp 1:1 or fast scaling
        p.drawImage(dest, frame_);
    } else {
        p.setPen(QColor(150, 160, 175));
        p.setFont(headerFont(12));
        p.drawText(rect().adjusted(0, -18, 0, -18), Qt::AlignCenter, tr("Waiting for the stream..."));
        p.setPen(QColor(90, 98, 112));
        QFont small;
        small.setPointSizeF(8.5);
        p.setFont(small);
        p.drawText(rect().adjusted(0, 14, 0, 14), Qt::AlignCenter,
                   tr("The video appears here as soon as the host starts the stream.\n"
                      "F11 fullscreen - F10 stats overlay"));
    }

    if (overlayVisible_) drawOverlay(p);
}

void VideoWidget::drawOverlay(QPainter& p) {
    QFont f = p.font();
    f.setPointSizeF(8.5);
    f.setFamily("Consolas");
    p.setFont(f);

    std::vector<QString> lines;
    if (statsProvider_) lines = statsProvider_();
    lines.push_back(QString("F10 overlay | F11 fullscreen"));

    // Semi-transparent backdrop for readability.
    const int lineH = 14;
    const int blockW = 260;
    const int blockH = static_cast<int>(lines.size()) * lineH + 10;
    QPainterPath path;
    path.addRoundedRect(QRect(8, 8, blockW, blockH), 6, 6);
    p.fillPath(path, QColor(0, 0, 0, 130));

    p.setPen(QColor(230, 232, 238));
    int y = 16;
    for (const QString& line : lines) {
        p.drawText(QRect(14, y, blockW - 12, lineH), Qt::AlignLeft | Qt::AlignVCenter, line);
        y += lineH;
    }

    if (!qualityBanner_.isEmpty()) {
        QPainterPath banner;
        banner.addRoundedRect(QRect(0, height() - 34, width(), 26), 0, 0);
        p.fillPath(banner, QColor(180, 95, 20, 200));
        p.setPen(Qt::white);
        p.drawText(QRect(10, height() - 34, width() - 20, 26), Qt::AlignVCenter, qualityBanner_);
    }
}

void VideoWidget::keyPressEvent(QKeyEvent* event) {
    // F10/F11 are consumed here AND at window level (QShortcut).
    if (event->key() == Qt::Key_F10) { overlayVisible_ = !overlayVisible_; update(); return; }
    if (event->key() == Qt::Key_F11) {
        emit toggleFullscreenRequested(true);   // window toggles; it knows the state
        return;
    }
    if (event->key() == Qt::Key_Escape && fullscreenActive_) {
        emit escapePressed();
        return;
    }
    emit keyPressed(event->key(), event->isAutoRepeat());
}

void VideoWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F10 || event->key() == Qt::Key_F11) return;
    emit keyReleased(event->key());
}

void VideoWidget::mouseMoveEvent(QMouseEvent* event) {
    emit mouseMoved(event->position().x(), event->position().y());
}

void VideoWidget::mousePressEvent(QMouseEvent* event) {
    emit mouseButtonPressed(event->button(), event->position().x(), event->position().y());
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* event) {
    emit mouseButtonReleased(event->button(), event->position().x(), event->position().y());
}

void VideoWidget::wheelEvent(QWheelEvent* event) {
    emit mouseWheel(event->angleDelta().y());
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    const bool entering = !isFullScreen();
    emit toggleFullscreenRequested(entering);
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace rp::ui
