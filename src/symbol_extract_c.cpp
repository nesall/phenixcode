#include "symbol_service.h"

namespace symbol_service::detail {
  std::vector<SymbolInfo> extractC(TSNode root, std::string_view content) {
    return extractCpp(root, content);
  }
} // namespace symbol_service::detail
