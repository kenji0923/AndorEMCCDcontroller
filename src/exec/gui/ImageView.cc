/**
 * @file ImageView.cc
 * @brief Implementation of AndorEMCCDcontroller::ImageView and ColorBar.
 */

#include "ImageView.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

namespace AndorEMCCDcontroller {

namespace {

constexpr double kMinScale = 0.05;
constexpr double kMaxScale = 64.0;

QColor RoiColor(const Roi& roi) {
  return QColor((roi.color >> 16) & 0xff, (roi.color >> 8) & 0xff,
                roi.color & 0xff);
}

}  // namespace

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  setMinimumSize(200, 200);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_OpaquePaintEvent);
}

void ImageView::SetImage(const QImage& image) {
  if (image.size() != image_.size()) fit_pending_ = true;
  image_ = image;
  if (fit_pending_ && auto_fit_) FitToWindow();
  update();
}

void ImageView::SetRois(const std::vector<Roi>& rois, int selected) {
  rois_ = rois;
  selected_ = selected;
  update();
}

void ImageView::SetFlip(bool horizontal, bool vertical) {
  flip_h_ = horizontal;
  flip_v_ = vertical;
  update();
}

void ImageView::SetRotation(int degrees_cw) {
  rotation_ = ((degrees_cw / 90) % 4 + 4) % 4 * 90;
  fit_pending_ = true;
  if (auto_fit_) FitToWindow();
  update();
}

QTransform ImageView::Orientation() const {
  const double w = image_.width();
  const double h = image_.height();
  // QTransform(m11, m12, m21, m22, dx, dy): x' = m11 x + m21 y + dx,
  // y' = m12 x + m22 y + dy. Flips act on [0, w] x [0, h] first.
  const QTransform flip(flip_h_ ? -1.0 : 1.0, 0.0, 0.0, flip_v_ ? -1.0 : 1.0,
                        flip_h_ ? w : 0.0, flip_v_ ? h : 0.0);
  QTransform rotate;
  switch (rotation_) {
    case 90:
      rotate = QTransform(0.0, 1.0, -1.0, 0.0, h, 0.0);
      break;
    case 180:
      rotate = QTransform(-1.0, 0.0, 0.0, -1.0, w, h);
      break;
    case 270:
      rotate = QTransform(0.0, -1.0, 1.0, 0.0, 0.0, w);
      break;
    default:
      break;
  }
  return flip * rotate;
}

void ImageView::SetDrawRoiMode(bool on) {
  draw_roi_mode_ = on;
  setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
}

void ImageView::FitToWindow() {
  if (image_.isNull()) return;
  const bool turned = rotation_ == 90 || rotation_ == 270;
  const double w = turned ? image_.height() : image_.width();
  const double h = turned ? image_.width() : image_.height();
  scale_ = std::min(width() / w, height() / h);
  scale_ = std::clamp(scale_, kMinScale, kMaxScale);
  offset_ = QPointF((width() - w * scale_) / 2.0, (height() - h * scale_) / 2.0);
  fit_pending_ = false;
  auto_fit_ = true;
  update();
}

void ImageView::ZoomOneToOne() {
  if (image_.isNull()) return;
  const QPointF center = WidgetToImage(QPointF(width() / 2.0, height() / 2.0));
  scale_ = 1.0;
  auto_fit_ = false;
  // Keep the image point at the center of the widget.
  const QPointF now = ImageToWidget().map(center);
  offset_ += QPointF(width() / 2.0, height() / 2.0) - now;
  update();
}

QTransform ImageView::ImageToWidget() const {
  // Points go through the orientation first, then zoom and pan.
  return Orientation() * QTransform::fromScale(scale_, scale_) *
         QTransform::fromTranslate(offset_.x(), offset_.y());
}

QPointF ImageView::WidgetToImage(const QPointF& p) const {
  return ImageToWidget().inverted().map(p);
}

int ImageView::RoiAt(const QPointF& ip) const {
  // Prefer the selected ROI, then the topmost (last drawn).
  auto contains = [&](const Roi& r) {
    return ip.x() >= r.x && ip.x() < r.x + r.width && ip.y() >= r.y &&
           ip.y() < r.y + r.height;
  };
  if (selected_ >= 0 && selected_ < static_cast<int>(rois_.size()) &&
      contains(rois_[selected_])) {
    return selected_;
  }
  for (int i = static_cast<int>(rois_.size()) - 1; i >= 0; --i) {
    if (contains(rois_[i])) return i;
  }
  return -1;
}

void ImageView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(32, 32, 32));
  if (image_.isNull()) {
    p.setPen(Qt::lightGray);
    p.drawText(rect(), Qt::AlignCenter, tr("No image"));
    return;
  }
  const QTransform t = ImageToWidget();
  p.save();
  p.setTransform(t);
  p.setRenderHint(QPainter::SmoothPixmapTransform, scale_ < 1.0);
  p.drawImage(QPointF(0, 0), image_);
  p.restore();

  // Overlays are drawn in widget coordinates so lines stay 1 px wide.
  for (int i = 0; i < static_cast<int>(rois_.size()); ++i) {
    const Roi& roi = rois_[i];
    const QRectF r = t.mapRect(QRectF(roi.x, roi.y, roi.width, roi.height));
    QPen pen(RoiColor(roi), i == selected_ ? 2 : 1);
    if (!roi.enabled) pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    p.drawText(r.topLeft() + QPointF(3, -3), QString::fromStdString(roi.name));
  }
  if (drag_ == Drag::kDraw) {
    p.setPen(QPen(Qt::yellow, 1, Qt::DashLine));
    p.drawRect(t.mapRect(QRectF(drag_start_image_, drag_current_image_)));
  }
}

void ImageView::wheelEvent(QWheelEvent* event) {
  if (image_.isNull()) return;
  const QPointF pos = event->position();
  const QPointF anchor = WidgetToImage(pos);
  const double factor = std::pow(1.0015, event->angleDelta().y());
  scale_ = std::clamp(scale_ * factor, kMinScale, kMaxScale);
  const QPointF now = ImageToWidget().map(anchor);
  offset_ += pos - now;
  auto_fit_ = false;
  update();
}

void ImageView::mousePressEvent(QMouseEvent* event) {
  if (image_.isNull()) return;
  drag_start_widget_ = event->position();
  drag_start_image_ = WidgetToImage(event->position());
  drag_current_image_ = drag_start_image_;
  if (event->button() == Qt::MiddleButton) {
    drag_ = Drag::kPan;
  } else if (event->button() == Qt::LeftButton) {
    const int roi = RoiAt(drag_start_image_);
    if (draw_roi_mode_) {
      drag_ = Drag::kDraw;
    } else if (roi >= 0) {
      drag_ = Drag::kMoveRoi;
      selected_ = roi;
      drag_roi_origin_ = QPoint(rois_[roi].x, rois_[roi].y);
      emit RoiSelected(roi);
    } else {
      drag_ = Drag::kPan;
    }
  }
  drag_start_offset_ = offset_;
}

void ImageView::mouseMoveEvent(QMouseEvent* event) {
  const QPointF ip = WidgetToImage(event->position());
  const int x = static_cast<int>(std::floor(ip.x()));
  const int y = static_cast<int>(std::floor(ip.y()));
  emit CursorMoved(x, y,
                   !image_.isNull() && x >= 0 && y >= 0 &&
                       x < image_.width() && y < image_.height());
  switch (drag_) {
    case Drag::kPan:
      offset_ = drag_start_offset_ + (event->position() - drag_start_widget_);
      auto_fit_ = false;
      update();
      break;
    case Drag::kDraw:
      drag_current_image_ = ip;
      update();
      break;
    case Drag::kMoveRoi: {
      const QPointF d = ip - drag_start_image_;
      Roi& roi = rois_[selected_];
      roi.x = drag_roi_origin_.x() + static_cast<int>(std::lround(d.x()));
      roi.y = drag_roi_origin_.y() + static_cast<int>(std::lround(d.y()));
      update();
      break;
    }
    case Drag::kNone:
      break;
  }
}

void ImageView::mouseReleaseEvent(QMouseEvent*) {
  if (drag_ == Drag::kDraw) {
    const QRectF r = QRectF(drag_start_image_, drag_current_image_).normalized();
    const int x0 = std::clamp(static_cast<int>(std::floor(r.left())), 0,
                              image_.width() - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(r.top())), 0,
                              image_.height() - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(r.right())), x0 + 1,
                              image_.width());
    const int y1 = std::clamp(static_cast<int>(std::ceil(r.bottom())), y0 + 1,
                              image_.height());
    if (x1 - x0 >= 1 && y1 - y0 >= 1) emit RoiDrawn(QRect(x0, y0, x1 - x0, y1 - y0));
  } else if (drag_ == Drag::kMoveRoi && selected_ >= 0) {
    const Roi& roi = rois_[selected_];
    if (roi.x != drag_roi_origin_.x() || roi.y != drag_roi_origin_.y()) {
      emit RoiMoved(selected_, roi.x, roi.y);
    }
  }
  drag_ = Drag::kNone;
  update();
}

void ImageView::mouseDoubleClickEvent(QMouseEvent*) { FitToWindow(); }

void ImageView::resizeEvent(QResizeEvent*) {
  if (auto_fit_) FitToWindow();
}

ColorBar::ColorBar(QWidget* parent) : QWidget(parent) {
  setMinimumWidth(70);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void ColorBar::SetColormap(const std::vector<unsigned int>& table) {
  table_ = table;
  update();
}

void ColorBar::SetRange(double lo, double hi, bool log) {
  lo_ = lo;
  hi_ = hi;
  log_ = log;
  update();
}

QSize ColorBar::sizeHint() const { return QSize(80, 300); }

void ColorBar::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), palette().window());
  if (table_.empty()) return;
  const int bar_w = 18;
  const int top = 10;
  const int bottom = height() - 10;
  const int span = std::max(1, bottom - top);
  for (int y = top; y < bottom; ++y) {
    const int idx = std::clamp(255 * (bottom - 1 - y) / span, 0, 255);
    const unsigned int c = table_[idx];
    p.setPen(QColor((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff));
    p.drawLine(4, y, 4 + bar_w, y);
  }
  p.setPen(palette().color(QPalette::WindowText));
  p.drawRect(4, top, bar_w, span);
  // Five tick labels, linear or logarithmic.
  for (int k = 0; k <= 4; ++k) {
    const double t = k / 4.0;
    const double v = log_ ? lo_ + std::expm1(t * std::log1p(hi_ - lo_))
                          : lo_ + t * (hi_ - lo_);
    const int y = bottom - static_cast<int>(t * span);
    p.drawLine(4 + bar_w, y, 8 + bar_w, y);
    p.drawText(QRect(10 + bar_w, y - 8, width() - bar_w - 10, 16),
               Qt::AlignLeft | Qt::AlignVCenter, QString::number(v, 'g', 5));
  }
}

}  // namespace AndorEMCCDcontroller
