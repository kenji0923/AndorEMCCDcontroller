/**
 * @file SettingsPanel.h
 * @brief Editor for the camera settings, built from the camera information.
 */

#ifndef AndorEMCCDcontroller__gui_SettingsPanel_h
#define AndorEMCCDcontroller__gui_SettingsPanel_h

#include <map>
#include <string>

#include <QWidget>

#include <AndorEMCCDcontroller/Protocol.h>

class QFormLayout;
class QCheckBox;
class QPushButton;
class QVBoxLayout;

namespace AndorEMCCDcontroller {

/**
 * @brief Shows every setting; edited fields are highlighted until applied.
 *
 * Times are shown in milliseconds and sent in seconds.
 */
class SettingsPanel : public QWidget {
  Q_OBJECT

 public:
  explicit SettingsPanel(QWidget* parent = nullptr);

  /** Camera capabilities from "get_info" (fills the choice lists). */
  void SetInfo(const Json& info);
  /** Current values from the backend; clears the edit marks. */
  void SetSettings(const Json& settings);
  /** Keys whose widget differs from the last SetSettings(). */
  Json ChangedSettings() const;
  /** Enables Apply only while this client holds the control right. */
  void SetEditable(bool editable, const QString& reason);

 signals:
  void ApplyRequested(const Json& patch);
  void ReloadRequested();

 private:
  enum class Kind { kBool, kInt, kDouble, kChoice };

  struct Field {
    std::string key;
    Kind kind = Kind::kInt;
    double scale = 1.0;  ///< Displayed = sent * scale (s -> ms).
    QWidget* widget = nullptr;
  };

  void Build();
  void AddField(QFormLayout* form, const std::string& key, const QString& label,
                Kind kind, double lo = 0, double hi = 0, double scale = 1.0,
                int decimals = 3, const QString& tooltip = {});
  Json WidgetValue(const Field& f) const;
  void SetWidgetValue(const Field& f, const Json& v);
  void SetChoices(const std::string& key,
                  const std::vector<std::pair<QString, Json>>& choices);
  void UpdateHsChoices();
  void UpdateEmRange();
  void MarkChanged();

  QPushButton* apply_ = nullptr;
  QCheckBox* wheel_changes_ = nullptr;
  Json info_ = Json::object();
  Json settings_ = Json::object();
  std::map<std::string, Field> fields_;
  bool updating_ = false;
};

}  // namespace AndorEMCCDcontroller

#endif
