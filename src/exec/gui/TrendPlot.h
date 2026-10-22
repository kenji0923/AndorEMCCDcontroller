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
  QSize sizeHint() const override { return QSize(400, 180); }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  std::vector<double> times_;
  std::vector<Series> series_;
  bool normalize_ = false;
};

}  // namespace AndorEMCCDcontroller

#endif
