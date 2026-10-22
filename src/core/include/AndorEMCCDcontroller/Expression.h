/**
 * @file Expression.h
 * @brief User formulas over ROI statistics (tinyexpr).
 */

#ifndef AndorEMCCDcontroller__Expression_h
#define AndorEMCCDcontroller__Expression_h

#include <memory>
#include <string>
#include <vector>

namespace AndorEMCCDcontroller {

/** One named formula, e.g. {"signal", "sig_sum - bg_mean * sig_count"}. */
struct ExpressionDef {
  std::string name;
  std::string formula;
};

/**
 * @brief Compiled set of formulas evaluated in order.
 *
 * Formulas can use every input variable (e.g. "sig_sum") and the results of
 * the formulas listed before them. Functions: abs, sqrt, exp, ln, log10, pow,
 * sin, cos, ... (tinyexpr built-ins).
 */
class ExpressionSet {
 public:
  ExpressionSet();
  ~ExpressionSet();
  ExpressionSet(ExpressionSet&&) noexcept;
  ExpressionSet& operator=(ExpressionSet&&) noexcept;

  /**
   * @brief Compiles @p defs against @p inputs.
   *
   * @param errors One entry per definition ("" when it compiled).
   * @return true if all compiled.
   */
  bool Compile(const std::vector<std::string>& inputs,
               const std::vector<ExpressionDef>& defs,
               std::vector<std::string>* errors);

  /**
   * @brief Evaluates every formula.
   *
   * @param input_values Same order as the inputs given to Compile().
   * @return One value per definition (NaN where it failed to compile).
   */
  std::vector<double> Evaluate(const std::vector<double>& input_values);

  std::size_t Size() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace AndorEMCCDcontroller

#endif
