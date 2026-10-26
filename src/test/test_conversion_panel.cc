#include <AndorEMCCDcontroller/DisplayConversion.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>

#include "../exec/gui/ConversionPanel.h"

namespace aec = AndorEMCCDcontroller;

namespace {
aec::Json Reference() {
  return {{"qe", .61},
          {"wavelength_nm", 422},
          {"table_file", "BV.txt"},
          {"source", "sdk_qe_table"},
          {"head_model", "DU897_BV"},
          {"calibration_kind", "sensor-model reference"}};
}
aec::Json ManualConfig() {
  return {{"source", "manual"},   {"units", "photons"},
          {"wavelength_nm", 422}, {"sensitivity_e_per_adu", 2.5},
          {"em_gain", 20},        {"qe", .9},
          {"baseline_adu", 101}};
}
}  // namespace

TEST(ConversionPanel, LoadsEditableQeAndPersistsProvenance) {
  aec::ConversionPanel panel;
  panel.SetConfig(ManualConfig());
  bool requested = false;
  QObject::connect(&panel, &aec::ConversionPanel::LoadQeTableRequested,
                   [&] { requested = true; });
  panel.findChild<QPushButton*>("load_qe_table")->click();
  EXPECT_TRUE(requested);
  panel.ApplyQeTable(Reference());
  auto config = panel.Config();
  EXPECT_DOUBLE_EQ(config["qe"].get<double>(), .61);
  EXPECT_EQ(config["qe_source"], "sdk_qe_table");
  EXPECT_EQ(config["qe_reference"], Reference());
  EXPECT_EQ(config["em_gain"], 20);
  EXPECT_EQ(config["baseline_adu"], 101);
  EXPECT_EQ(config["sensitivity_e_per_adu"], 2.5);
  aec::ConversionPanel restored;
  restored.SetConfig(config);
  EXPECT_EQ(restored.Config(), config);
  restored.findChild<QDoubleSpinBox*>("display_qe")->setValue(62.25);
  EXPECT_EQ(restored.Config()["qe_source"], "manual");
  EXPECT_NEAR(restored.Config()["qe"].get<double>(), .6225, 1e-12);
  EXPECT_EQ(restored.Config()["qe_reference"], Reference());
}

TEST(ConversionPanel, RejectsStaleWavelengthUntilReloadOrManualEdit) {
  aec::ConversionPanel panel;
  panel.SetConfig(ManualConfig());
  panel.ApplyQeTable(Reference());
  EXPECT_TRUE(aec::ResolveDisplayConversion(panel.Config(), {}, {})["valid"]);
  panel.findChild<QDoubleSpinBox*>("display_wavelength_nm")->setValue(500);
  EXPECT_FALSE(aec::ResolveDisplayConversion(panel.Config(), {}, {})["valid"]);
  EXPECT_TRUE(
      panel.findChild<QLabel*>("qe_table_status")->text().contains("Reload"));
  const auto before = panel.Config();
  EXPECT_THROW(panel.ApplyQeTable(Reference()), std::runtime_error);
  EXPECT_EQ(panel.Config(), before);
  panel.findChild<QDoubleSpinBox*>("display_qe")->setValue(75);
  EXPECT_TRUE(aec::ResolveDisplayConversion(panel.Config(), {}, {})["valid"]);
}

TEST(ConversionPanel, DoesNotInventMissingCameraCoefficients) {
  aec::ConversionPanel panel;
  panel.SetConfig({{"wavelength_nm", 422}});
  const auto before = panel.Config();
  EXPECT_THROW(panel.ApplyQeTable(Reference()), std::runtime_error);
  EXPECT_EQ(panel.Config(), before);
  panel.SetCalibration({{"sensitivity_e_per_adu", 4.2},
                        {"em_gain", 1.0},
                        {"baseline_adu", 100.0},
                        {"qe", .8}});
  panel.ApplyQeTable(Reference());
  EXPECT_EQ(panel.Config()["source"], "manual");
  EXPECT_EQ(panel.Config()["sensitivity_e_per_adu"], 4.2);
  EXPECT_EQ(panel.Config()["em_gain"], 1.0);
  // A later switch to camera calibration must not label its QE as table data.
  panel.findChild<QComboBox*>("display_calibration_source")->setCurrentIndex(0);
  panel.findChild<QComboBox*>("display_calibration_source")->setCurrentIndex(1);
  EXPECT_EQ(panel.Config()["qe_source"], "manual");
  EXPECT_EQ(panel.Config()["qe"], .8);
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
