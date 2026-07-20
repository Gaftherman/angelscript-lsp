#pragma once

#include <string>

#include "i18n/LspStrings.h"

// Forward declaration
namespace angel_lsp { namespace features { struct HoverInfo; } }

namespace angel_lsp {
namespace utils {

    // Fills the documentation fields of a HoverInfo from a raw Doxygen comment.
    // Uses tree-sitter-doxygen for proper AST parsing.
    // Cross-links @param names to HoverInfo.parameters[i].docDescription.
    void FillHoverInfoFromDoxygen(const std::string& rawDoxygen, features::HoverInfo& info, const std::string& targetParam = "");

    // Legacy wrapper — kept for backward compatibility with existing tests.
    // Internally calls FillHoverInfoFromDoxygen and renders to markdown.
    std::string FormatDoxygenToMarkdown(const std::string& rawDoxygen, i18n::Locale locale, const std::string& targetParam = "");

}
}
