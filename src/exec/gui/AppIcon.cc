/**
 * @file AppIcon.cc
 * @brief Implementation of the EMCCD application icon.
 *
 * A sensor package with gold pins, the image area as a pixel grid showing a
 * bright spot and a few single-photon events, and below it the serial
 * register whose electron-multiplying part brightens towards the output
 * amplifier. Drawn on a 256 x 256 canvas and scaled; small sizes use a
 * coarser grid so that the spot stays readable.
 */

#include "AppIcon.h"

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace AndorEMCCDcontroller {

namespace {

constexpr double kCanvas = 256.0;

/** Inferno-like ramp, t in [0, 1]. */
QColor Heat(double t) {
  static const QColor stops[] = {QColor(10, 12, 40),   QColor(60, 15, 110),
                                 QColor(150, 35, 105), QColor(225, 85, 45),
                                 QColor(250, 175, 30), QColor(252, 250, 180)};
  constexpr int kLast = 5;
  t = std::clamp(t, 0.0, 1.0) * kLast;
  const int i = std::min(static_cast<int>(t), kLast - 1);
  const double f = t - i;
  const QColor& a = stops[i];
  const QColor& b = stops[i + 1];
  return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f,
                          a.greenF() + (b.greenF() - a.greenF()) * f,
                          a.blueF() + (b.blueF() - a.blueF()) * f);
}

void DrawPins(QPainter* p, bool detailed) {
  const int n = detailed ? 7 : 4;
  const double pitch = 168.0 / n;
  p->setPen(Qt::NoPen);
  for (int i = 0; i < n; ++i) {
    const double y = 44 + pitch * (i + 0.5) - 6;
    p->setBrush(QColor(205, 160, 60));
    p->drawRoundedRect(QRectF(2, y, 30, 12), 3, 3);
    p->drawRoundedRect(QRectF(224, y, 30, 12), 3, 3);
  }
}

void DrawPackage(QPainter* p) {
  QLinearGradient body(0, 16, 0, 240);
  body.setColorAt(0, QColor(70, 76, 88));
  body.setColorAt(1, QColor(28, 31, 38));
  p->setPen(QPen(QColor(120, 128, 142), 3));
  p->setBrush(body);
  p->drawRoundedRect(QRectF(18, 18, 220, 220), 22, 22);
  // Gold bond ring around the die.
  p->setPen(QPen(QColor(205, 160, 60), 4));
  p->setBrush(Qt::NoBrush);
  p->drawRoundedRect(QRectF(34, 32, 188, 192), 8, 8);
}

void DrawImageArea(QPainter* p, bool detailed) {
  const QRectF area(44, 42, 168, 128);
  const int cols = detailed ? 8 : 4;
  const int rows = detailed ? 6 : 3;
  const double w = area.width() / cols;
  const double h = area.height() / rows;
  // Spot centre in grid units, and a few isolated photon events.
  const double cx = cols * 0.62;
  const double cy = rows * 0.42;
  const double sigma = detailed ? 1.15 : 0.75;
  struct Event {
    int c, r;
    double v;
  };
  const Event events[] = {{1, 4, 0.75}, {6, 5, 0.6}, {2, 1, 0.5}};
  p->setPen(Qt::NoPen);
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const double dx = c + 0.5 - cx;
      const double dy = r + 0.5 - cy;
      double v = 0.08 + 0.95 * std::exp(-(dx * dx + dy * dy) /
                                        (2 * sigma * sigma));
      if (detailed) {
        for (const auto& e : events) {
          if (e.c == c && e.r == r) v = std::max(v, e.v);
        }
      }
      p->setBrush(Heat(v));
      p->drawRect(QRectF(area.left() + c * w, area.top() + r * h, w, h));
    }
  }
  // Pixel boundaries.
  const double line = detailed ? 2.0 : 4.0;
  p->setPen(QPen(QColor(5, 8, 25, 200), line));
  for (int c = 1; c < cols; ++c) {
    const double x = area.left() + c * w;
    p->drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
  }
  for (int r = 1; r < rows; ++r) {
    const double y = area.top() + r * h;
    p->drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
  }
  p->setPen(QPen(QColor(150, 160, 190), 2));
  p->setBrush(Qt::NoBrush);
  p->drawRect(area);
}

void DrawGainRegister(QPainter* p, bool detailed) {
  // Serial register (grey) then the EM register brightening to the output.
  const QRectF strip(44, 182, 150, 28);
  const int n = detailed ? 10 : 5;
  const int serial = detailed ? 3 : 1;
  const double w = strip.width() / n;
  p->setPen(QPen(QColor(5, 8, 25), detailed ? 2.0 : 3.0));
  for (int i = 0; i < n; ++i) {
    QColor c;
    if (i < serial) {
      c = QColor(95, 104, 122);
    } else {
      const double t = static_cast<double>(i - serial + 1) / (n - serial);
      c = Heat(0.35 + 0.65 * t);
    }
    p->setBrush(c);
    p->drawRect(QRectF(strip.left() + i * w, strip.top(), w, strip.height()));
  }
  // Output amplifier.
  QPainterPath amp;
  amp.moveTo(198, 178);
  amp.lineTo(218, 196);
  amp.lineTo(198, 214);
  amp.closeSubpath();
  p->setPen(Qt::NoPen);
  p->setBrush(QColor(252, 240, 170));
  p->drawPath(amp);
}

}  // namespace

QImage RenderAppIcon(int size) {
  QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter p(&image);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.scale(size / kCanvas, size / kCanvas);
  const bool detailed = size >= 40;
  DrawPins(&p, detailed);
  DrawPackage(&p);
  DrawImageArea(&p, detailed);
  DrawGainRegister(&p, detailed);
  return image;
}

QIcon CreateAppIcon() {
  QIcon icon;
  for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
    icon.addPixmap(QPixmap::fromImage(RenderAppIcon(size)));
  }
  return icon;
}

}  // namespace AndorEMCCDcontroller
