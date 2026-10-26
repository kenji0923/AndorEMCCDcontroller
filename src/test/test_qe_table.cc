#include <AndorEMCCDcontroller/QeTable.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>

namespace aec = AndorEMCCDcontroller;

namespace {
class QeTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("aec-qe-test-" + std::to_string(std::random_device{}()));
    ASSERT_TRUE(std::filesystem::create_directory(directory_));
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  void Write(const std::string& text) {
    std::ofstream(directory_ / "BV.txt") << text;
  }
  aec::Json Read(double wavelength, const std::string& model = "DU897_BV") {
    return aec::ReadSdkQeTable(directory_.string(), model, wavelength);
  }
  std::filesystem::path directory_;
};
}  // namespace

TEST_F(QeTableTest, InterpolatesAndRecordsReference) {
  // Synthetic points, not a redistributed manufacturer table.
  Write("# wavelength_nm QE_fraction\r\n400 0.4\r\n\r\n500 0.8\r\n600 0.6\r\n");
  const auto value = Read(450);
  EXPECT_NEAR(value["qe"].get<double>(), .6, 1e-12);
  EXPECT_EQ(value["head_model"], "DU897_BV");
  EXPECT_EQ(value["table_file"], "BV.txt");
  EXPECT_EQ(value["range_nm"], aec::Json::array({400.0, 600.0}));
  EXPECT_EQ(value["bracketing_points"][0], aec::Json::array({400.0, .4}));
  EXPECT_DOUBLE_EQ(Read(400)["qe"].get<double>(), .4);
  EXPECT_DOUBLE_EQ(Read(600)["qe"].get<double>(), .6);
  std::filesystem::rename(directory_ / "BV.txt", directory_ / "BV.TXT");
  EXPECT_EQ(Read(500)["table_file"], "BV.TXT");
}

TEST_F(QeTableTest, RejectsMissingTablesAndExtrapolation) {
  EXPECT_THROW(Read(450), std::runtime_error);
  Write("400 0.4\n500 0.8\n");
  EXPECT_THROW(Read(399), std::runtime_error);
  EXPECT_THROW(Read(501), std::runtime_error);
  EXPECT_THROW(Read(std::numeric_limits<double>::quiet_NaN()),
               std::runtime_error);
  EXPECT_THROW(Read(std::numeric_limits<double>::infinity()),
               std::runtime_error);
  EXPECT_THROW(Read(450, "unknown"), std::runtime_error);
  EXPECT_THROW(Read(450, "DU897_../../BV"), std::runtime_error);
  EXPECT_THROW(Read(450, "DU897_EX"), std::runtime_error);
}

TEST_F(QeTableTest, RejectsMalformedOrAmbiguousCoefficients) {
  for (const char* text :
       {"", "400 0.4\n", "400 40\n500 80\n", "400 0.4\n400 0.8\n",
        "500 0.8\n400 0.4\n", "400 -0.1\n500 0.8\n",
        "400 0.4 garbage\n500 0.8\n", "400 nan\n500 0.8\n"}) {
    Write(text);
    EXPECT_THROW(Read(450), std::runtime_error) << text;
  }
  Write("400 0\n500 0.8\n");
  EXPECT_THROW(Read(400), std::runtime_error);
}
