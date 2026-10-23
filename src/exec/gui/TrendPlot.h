/**
 * @file TrendPlot.h
 * @brief Strip chart of analysis values versus time.
 */

#ifndef AndorEMCCDcontroller__gui_TrendPlot_h
#define AndorEMCCDcontroller__gui_TrendPlot_h

#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

namespace AndorEMCCDcontroller {

class TrendPlot : public QWidget {
  Q_OBJECT

 public:
  struct Series {
    QString name;
    QColor color;
    std::vector<double> values;
  };

  explicit TrendPlot(QWidget* parent = nullptr);

  /** @p times in seconds (any origin); every series has the same length. */
  void SetData(std::vector<double> times, std::vector<Series> series);
  /** Scales every series to 0..1 so different magnitudes share the axis. */
  void SetNormalize(bool on);
  /**
   * Fixes the value axis to [@p lo, @p hi] (after normalization) when @p on;
   * otherwise it follows the data.
   */
  void SetFixedRange(bool on, double lo, double hi);
  /** Axis range the automatic scale would use now; false without data. */
  bool AutoRange(double* lo, double* hi) const;
  QSize sizeHint() const override { return QSize(400, 180); }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  /** Copy of the series, normalized if requested. */
  std::vector<Series> Prepared() const;
  static bool DataRange(const std::vector<Series>& series, double* lo,
                        double* hi);

  std::vector<double> times_;
  std::vector<Series> series_;
  bool normalize_ = false;
  bool fixed_ = false;
  double fixed_lo_ = 0.0;
  double fixed_hi_ = 1.0;
};

}  // namespace AndorEMCCDcontroller

#endif
