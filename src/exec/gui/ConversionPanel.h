#ifndef AndorEMCCDcontroller__gui_ConversionPanel_h
#define AndorEMCCDcontroller__gui_ConversionPanel_h

#include <AndorEMCCDcontroller/Protocol.h>

#include <QWidget>
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace AndorEMCCDcontroller {
class ConversionPanel : public QWidget {
  Q_OBJECT
 public:
  explicit ConversionPanel(QWidget* parent = nullptr);
  Json Config() const;
  void SetConfig(const Json& config);
  void SetCalibration(const Json& calibration);
  void ShowResult(const Json& result);
  void ApplyQeTable(const Json& reference);
  void SetQeTableError(const QString& error);
 signals:
  void Changed();
  void LoadQeTableRequested();

 private:
  void UpdateSource();
  void UpdateQeReference();
  QComboBox* units_;
  QComboBox* source_;
  QDoubleSpinBox* wavelength_;
  QDoubleSpinBox* sensitivity_;
  QDoubleSpinBox* gain_;
  QDoubleSpinBox* qe_;
  QDoubleSpinBox* baseline_;
  QLabel* status_;
  QLabel* camera_values_;
  QLabel* qe_reference_label_;
  QPushButton* load_qe_;
  Json calibration_ = Json::object();
  Json qe_reference_ = Json::object();
  bool qe_from_table_ = false;
};
}  // namespace AndorEMCCDcontroller
#endif
