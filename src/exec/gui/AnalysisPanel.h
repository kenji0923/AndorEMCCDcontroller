/**
 * @file AnalysisPanel.h
 * @brief ROI table, formulas, UDP output and the trend chart.
 */

#ifndef AndorEMCCDcontroller__gui_AnalysisPanel_h
#define AndorEMCCDcontroller__gui_AnalysisPanel_h

#include <string>
#include <vector>

#include <QWidget>

#include <AndorEMCCDcontroller/Analysis.h>
#include <AndorEMCCDcontroller/Expression.h>
#include <AndorEMCCDcontroller/Protocol.h>

#include "LiveProcessor.h"
#include "TrendPlot.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTableWidget;
class QDoubleSpinBox;

namespace AndorEMCCDcontroller {

class AnalysisPanel : public QWidget {
  Q_OBJECT

 public:
  explicit AnalysisPanel(QWidget* parent = nullptr);

  std::vector<Roi> Rois() const;
  std::vector<ExpressionDef> Formulas() const;
  UdpConfig Udp() const;
  int SelectedRoi() const;

  void AddRoi(const QRect& rect);
  void MoveRoi(int index, int x, int y);
  void SelectRoi(int index);
  /** Shows compile errors next to the formulas. */
  void SetFormulaErrors(const std::vector<std::string>& errors);
  /** Updates the value columns with the latest frame result. */
  void ShowValues(const std::vector<std::string>& names,
                  const std::vector<double>& values);
  void SetUdpStatus(const QString& text);
  /** Choices offered in the UDP variable column. */
  void SetVariableNames(const std::vector<std::string>& names);

  Json ToJson() const;
  void FromJson(const Json& j);

 signals:
  /** ROIs or formulas changed (analysis must be reconfigured). */
  void ConfigChanged();
  void UdpChanged();
  void DrawRoiToggled(bool on);

 private:
  void AddRoiRow(const Roi& roi);
  void AddFormulaRow(const ExpressionDef& def);
  void UpdateVariableHelp();
  void AddUdpRow(const UdpChannel& channel);

  QTableWidget* rois_;
  QTableWidget* formulas_;
  QLabel* variables_;
  QTableWidget* udp_;
  QLabel* udp_status_;
  QStringList variable_names_;
  bool updating_ = false;
  int next_color_ = 0;
};

/** Variable picker and strip chart. */
class TrendPanel : public QWidget {
  Q_OBJECT

 public:
  explicit TrendPanel(QWidget* parent = nullptr);

  void SetNames(const std::vector<std::string>& names);
  void Refresh(const LiveProcessor& processor);
  std::vector<std::string> Selected() const;
  void Select(const std::vector<std::string>& names);

 signals:
  void ClearRequested();

 private:
  QListWidget* list_;
  QDoubleSpinBox* window_s_;
  TrendPlot* plot_;
  std::vector<std::string> names_;
};

}  // namespace AndorEMCCDcontroller

#endif
