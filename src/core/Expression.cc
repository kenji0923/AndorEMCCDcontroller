/**
 * @file Expression.cc
 * @brief Implementation of AndorEMCCDcontroller::ExpressionSet.
 */

#include <AndorEMCCDcontroller/Expression.h>

#include <cmath>
#include <limits>

#include <tinyexpr.h>

#include <AndorEMCCDcontroller/Analysis.h>

namespace AndorEMCCDcontroller {

struct ExpressionSet::Impl {
  std::vector<std::string> names;  ///< Inputs then results.
  std::vector<double> values;      ///< Bound by address; never reallocated.
  std::vector<te_expr*> exprs;
  std::size_t n_inputs = 0;

  ~Impl() { Clear(); }

  void Clear() {
    for (te_expr* e : exprs) te_free(e);
    exprs.clear();
  }
};

ExpressionSet::ExpressionSet() : impl_(std::make_unique<Impl>()) {}
ExpressionSet::~ExpressionSet() = default;
ExpressionSet::ExpressionSet(ExpressionSet&&) noexcept = default;
ExpressionSet& ExpressionSet::operator=(ExpressionSet&&) noexcept = default;

bool ExpressionSet::Compile(const std::vector<std::string>& inputs,
                            const std::vector<ExpressionDef>& defs,
                            std::vector<std::string>* errors) {
  Impl& d = *impl_;
  d.Clear();
  d.names = inputs;
  d.n_inputs = inputs.size();
  for (const auto& def : defs) d.names.push_back(def.name);
  d.values.assign(d.names.size(), std::numeric_limits<double>::quiet_NaN());

  std::vector<te_variable> vars;
  vars.reserve(d.names.size());
  for (std::size_t i = 0; i < d.n_inputs; ++i) {
    vars.push_back({d.names[i].c_str(), &d.values[i], TE_VARIABLE, nullptr});
  }

  errors->assign(defs.size(), "");
  bool ok = true;
  for (std::size_t k = 0; k < defs.size(); ++k) {
    te_expr* expr = nullptr;
    if (!IsValidIdentifier(defs[k].name)) {
      (*errors)[k] = "invalid name '" + defs[k].name + "'";
    } else {
      int error_pos = 0;
      expr = te_compile(defs[k].formula.c_str(), vars.data(),
                        static_cast<int>(vars.size()), &error_pos);
      if (!expr) {
        (*errors)[k] = "syntax error at character " + std::to_string(error_pos);
      }
    }
    if (!expr) ok = false;
    d.exprs.push_back(expr);
    // Later formulas may use this result.
    const std::size_t slot = d.n_inputs + k;
    vars.push_back({d.names[slot].c_str(), &d.values[slot], TE_VARIABLE,
                    nullptr});
  }
  return ok;
}

std::vector<double> ExpressionSet::Evaluate(
    const std::vector<double>& input_values) {
  Impl& d = *impl_;
  const std::size_t n = std::min(input_values.size(), d.n_inputs);
  for (std::size_t i = 0; i < n; ++i) d.values[i] = input_values[i];
  std::vector<double> results(d.exprs.size());
  for (std::size_t k = 0; k < d.exprs.size(); ++k) {
    const double v = d.exprs[k] ? te_eval(d.exprs[k])
                                : std::numeric_limits<double>::quiet_NaN();
    d.values[d.n_inputs + k] = v;
    results[k] = v;
  }
  return results;
}

std::size_t ExpressionSet::Size() const { return impl_->exprs.size(); }

}  // namespace AndorEMCCDcontroller
