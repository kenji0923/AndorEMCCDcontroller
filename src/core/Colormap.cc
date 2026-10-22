/**
 * @file Colormap.cc
 * @brief Implementation of AndorEMCCDcontroller::Colormap.
 */

#include <AndorEMCCDcontroller/Colormap.h>

#include <algorithm>
#include <utility>

namespace AndorEMCCDcontroller {

namespace {

struct NamedTable {
  const char* name;
  ColormapTable table;
};

const std::vector<NamedTable>& Tables() {
  static const std::vector<NamedTable> tables{
#include "ColormapTables.inc"
  };
  return tables;
}

}  // namespace

const std::vector<std::string>& ColormapNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> n;
    for (const auto& t : Tables()) n.emplace_back(t.name);
    return n;
  }();
  return names;
}

ColormapTable GetColormap(const std::string& name, bool reversed) {
  ColormapTable table = Tables().front().table;
  for (const auto& t : Tables()) {
    if (name == t.name) table = t.table;
  }
  if (reversed) std::reverse(table.begin(), table.end());
  return table;
}

}  // namespace AndorEMCCDcontroller
