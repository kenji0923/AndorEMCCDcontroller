/**
 * @file TrendPlot.cc
 * @brief Implementation of AndorEMCCDcontroller::TrendPlot.
 */

#include "TrendPlot.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QPainter>
#include <QPainterPath>

namespace AndorEMCCDcontroller {

TrendPlot::TrendPlot(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(120);
}

void TrendPlot::SetData(std::vector<double> times, std::vector<Series> series) {
  times_ = std::move(times);
  series_ = std::move(series);
  update();
}

void TrendPlot::SetNormalize(bool on) {
  normalize_ = on;
  update();
}

void TrendPlot::SetFixedRange(bool on, double lo, double hi) {
  fixed_ = on;
  fixed_lo_ = lo;
  fixed_hi_ = hi;
  update();
}

std::vector<TrendPlot::Series> TrendPlot::Prepared() const {
  std::vector<Series> series = series_;
  if (!normalize_) return series;
  // Each series is scaled to 0..1 over the visible window.
  for (auto& s : series) {
    double a = std::numeric_limits<double>::infinity();
    double b = -a;
    for (double v : s.values) {
      if (std::isfinite(v)) {
        a = std::min(a, v);
        b = std::max(b, v);
      }
    }
    const double span = b > a ? b - a : 1.0;
    for (double& v : s.values) v = (v - a) / span;
    s.name += tr(" [%1, %2]").arg(a, 0, 'g', 4).arg(b, 0, 'g', 4);
  }
  return series;
}

bool TrendPlot::DataRange(const std::vector<Series>& series, double* lo,
                          double* hi) {
  double a = std::numeric_limits<double>::infinity();
  double b = -a;
  for (const auto& s : series) {
    for (double v : s.values) {
      if (std::isfinite(v)) {
        a = std::min(a, v);
        b = std::max(b, v);
      }
    }
  }
  if (!std::isfinite(a)) return false;
  if (b <= a) {
    b = a + 1.0;
    a -= 1.0;
  }
  const double pad = 0.05 * (b - a);
  *lo = a - pad;
  *hi = b + pad;
  return true;
}

bool TrendPlot::AutoRange(double* lo, double* hi) const {
  return times_.size() >= 2 && DataRange(Prepared(), lo, hi);
}

void TrendPlot::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), palette().base());
  const QColor text = palette().color(QPalette::Text);
  const QRectF plot(80, 8, width() - 90, height() - 34);
  p.setPen(text);
  p.drawRect(plot);
  if (times_.size() < 2 || series_.empty()) {
    p.drawText(plot, Qt::AlignCenter, tr("Select values to plot"));
    return;
  }
  // Work on a copy: normalization must not accumulate across repaints.
  const std::vector<Series> series = Prepared();
  double lo = 0.0;
  double hi = 1.0;
  if (fixed_ && fixed_hi_ > fixed_lo_) {
    lo = fixed_lo_;
    hi = fixed_hi_;
  } else if (!DataRange(series, &lo, &hi)) {
    return;
  }
  const double t0 = times_.front();
  const double t1 = std::max(times_.back(), t0 + 1e-9);
  auto map = [&](double t, double v) {
    return QPointF(plot.left() + (t - t0) / (t1 - t0) * plot.width(),
                   plot.bottom() - (v - lo) / (hi - lo) * plot.height());
  };

  // Axes labels: value ticks on the left, elapsed time below.
  for (int k = 0; k <= 4; ++k) {
    const double v = lo + k * (hi - lo) / 4.0;
    const QPointF a = map(t0, v);
    p.drawLine(QPointF(plot.left() - 4, a.y()), QPointF(plot.left(), a.y()));
    p.drawText(QRectF(0, a.y() - 8, plot.left() - 6, 16),
               Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'g', 4));
  }
  p.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), 18),
             Qt::AlignCenter,
             tr("Time (s), last %1 s").arg(t1 - t0, 0, 'f', 1));

  p.setRenderHint(QPainter::Antialiasing);
  p.setClipRect(plot);
  int legend_y = static_cast<int>(plot.top()) + 14;
  for (const auto& s : series) {
    QPainterPath path;
    bool started = false;
    for (std::size_t i = 0; i < times_.size() && i < s.values.size(); ++i) {
      if (!std::isfinite(s.values[i])) {
        started = false;
        continue;
      }
      const QPointF pt = map(times_[i], s.values[i]);
      if (started) {
        path.lineTo(pt);
      } else {
        path.moveTo(pt);
        started = true;
      }
    }
    p.setPen(QPen(s.color, 1));
    p.drawPath(path);
    p.drawText(QPointF(plot.left() + 6, legend_y), s.name);
    legend_y += 14;
  }
}

}  // namespace AndorEMCCDcontroller
