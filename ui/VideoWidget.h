#pragma once
// Phase 5 — client video surface.
//
// Displays decoded BGRA frames, aspect-preserving and letterboxed, with the
// F10 diagnostics overlay (ping/loss/jitter/bitrate/fps/decode latency) and
// fullscreen support (F11 / double-click). Forwards keyboard/mouse events as
// signals for the input pipeline (Phase 11).

#include <utility>
#include <QImage>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <vector>

namespace rp::ui {

class VideoWidget : public QWidget {
    Q_OBJECT
public:
    using StatsProvider = std::function<std::vector<QString>()>;

    explicit VideoWidget(QWidget* parent = nullptr);

    // Presents one BGRA frame (copies into the widget's backing store).
    void presentFrame(const uint8_t* bgra, int width, int height, int stride);

    void setStatsProvider(StatsProvider provider) { statsProvider_ = std::move(provider); }
    void showOverlay(bool on) { overlayVisible_ = on; update(); }
    bool overlayVisible() const { return overlayVisible_; }
    // Set by the owning window: only a fullscreen Esc is consumed as "exit
    // fullscreen"; otherwise Esc forwards to the game like any other key.
    void setFullscreenActive(bool on) { fullscreenActive_ = on; }

    // Connection quality banner (shown when non-empty).
    void setQualityBanner(const QString& text);

    [[nodiscard]] QSize sizeHint() const override;

signals:
    void keyPressed(int qtKey, bool autorepeat);
    void keyReleased(int qtKey);
    void mouseMoved(int x, int y);            // widget-relative
    void mouseButtonPressed(int button, int x, int y);
    void mouseButtonReleased(int button, int x, int y);
    void mouseWheel(int delta);
    void toggleFullscreenRequested(bool fullscreen);
    void escapePressed();                     // exits fullscreen (only consumed then)

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void drawOverlay(class QPainter& p);

public:
    [[nodiscard]] QRect videoDestRect() const;

    QImage frame_;
    uint64_t framesPresented_ = 0;
    double lastPresentMs_ = 0.0;
    bool overlayVisible_ = true;
    QString qualityBanner_;
    StatsProvider statsProvider_;
    double fpsEstimate_ = 0.0;
    uint64_t lastFpsUpdateNs_ = 0;
    bool fullscreenActive_ = false;
};

} // namespace rp::ui
