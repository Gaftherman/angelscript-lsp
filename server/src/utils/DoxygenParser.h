#pragma once

#include <string>

#include "i18n/LspStrings.h"

namespace angel_lsp {
namespace utils {

    // Converts a raw doxygen docstring (e.g., /** @brief ... */) to clean Markdown.
    // targetParam specifies an optional parameter name to highlight or extract specifically.
    std::string FormatDoxygenToMarkdown(const std::string& rawDoxygen, i18n::Locale locale, const std::string& targetParam = "");

}
}
