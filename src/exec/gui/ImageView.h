/**
 * @file ImageView.h
 * @brief Zoomable image display with ROI overlays.
 */

#ifndef AndorEMCCDcontroller__gui_ImageView_h
#define AndorEMCCDcontroller__gui_ImageView_h

#include <vector>

#include <QImage>
#include <QPointF>
#include <QTransform>
#include <QWidget>

#include <AndorEMCCDcontroller/Analysis.h>

namespace AndorEMCCDcontroller {

/**
 * @brief Shows a false-color frame.
 *
 * Mouse: wheel zooms around the cursor, left drag pans (or moves the ROI
 * under the cursor), double click fits the image. In ROI drawing mode a left
 * drag creates a ROI. Coordinates in signals are frame pixels (0-based).
 */
class ImageView : public QWidget {
  Q_OBJECT

 public:
  explicit ImageView(QWidget* parent = nullptr);

  /** @p image has one pixel per frame pixel (row 0 = frame row 0). */
  void SetImage(const QImage& image);
  const QImage& Image() const { return image_; }

  void SetRois(const std::vector<Roi>& rois, int selected);
  void SetFlip(bool horizontal, bool vertical);
  /** Clockwise rotation in degrees (0, 90, 180, 270), applied after flips. */
  void SetRotation(int degrees_cw);
  void SetSmoothDownsampling(bool enabled);
  /** Frame pixels -> displayed image (flips then rotation), origin at 0. */
  QTransform Orientation() const;
  void SetDrawRoiMode(bool on);

 public slots:
  void FitToWindow();
  void ZoomOneToOne();

 signals:
  /** Frame pixel under the cursor; valid == false outside the image. */
  void CursorMoved(int x, int y, bool valid);
  void RoiDrawn(QRect rect);
  void RoiMoved(int index, int x, int y);
  void RoiSelected(int index);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  /** Frame pixel coordinates -> widget coordinates. */
  QTransform ImageToWidget() const;
  QPointF WidgetToImage(const QPointF& p) const;
  int RoiAt(const QPointF& image_point) const;

  QImage image_;
  std::vector<Roi> rois_;
  int selected_ = -1;
  bool flip_h_ = false;
  bool flip_v_ = false;
  int rotation_ = 0;
  bool draw_roi_mode_ = false;
  bool fit_pending_ = true;
  bool auto_fit_ = true;
  bool smooth_downsampling_ = true;

  double scale_ = 1.0;
  QPointF offset_;  ///< Widget position of the (oriented) image origin.

  enum class Drag { kNone, kPan, kDraw, kMoveRoi };
  Drag drag_ = Drag::kNone;
  QPointF drag_start_widget_;
  QPointF drag_start_image_;
  QPointF drag_current_image_;
  QPointF drag_start_offset_;
  QPoint drag_roi_origin_;
};

/** Vertical color scale with min/max labels. */
class ColorBar : public QWidget {
  Q_OBJECT

 public:
  explicit ColorBar(QWidget* parent = nullptr);
  void SetColormap(const std::vector<unsigned int>& table);
  void SetRange(double lo, double hi, bool log);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  std::vector<unsigned int> table_;
  double lo_ = 0.0;
  double hi_ = 1.0;
  bool log_ = false;
};

}  // namespace AndorEMCCDcontroller

#endif
