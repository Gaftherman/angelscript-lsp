#pragma once
#include <string>
#include <vector>
#include <optional>
#include "analysis/Symbol.h"
#include "i18n/LspStrings.h"

namespace angel_lsp {
namespace features {

/// Structured data for one parameter (function param or template param).
/// Mirrors clangd's HoverInfo::Param exactly.
struct HoverParam {
    std::string typeName;       // "const float& in", "class T"
    std::string name;           // "value", "T"
    std::string defaultValue;   // "0.0f" or ""
    std::string docDescription; // from @param tag in doxygen
};

/// All structured data needed to render a hover panel.
/// Mirrors clangd's HoverInfo struct, adapted for AngelScript.
struct HoverInfo {
    // --- Identity ---
    std::string name;                    // "insertLast", "Vector3"
    analysis::SymbolKind kind = analysis::SymbolKind::Unknown;

    // --- Scope context (for code block header comment like clangd) ---
    std::string localScope;              // "array<T>", "Entity", "Engine::Math"
    // Note: AngelScript doesn't have C++ namespaces in the same sense,
    // so we collapse namespace+class into localScope.

    // --- Signature (for the code block) ---
    std::string rawSignature;            // full angelscript signature string

    // --- Structured function info (like clangd's Parameters/ReturnType) ---
    std::optional<std::string> returnType;              // "void", "float", nullopt for non-functions
    std::optional<std::vector<HoverParam>> parameters;  // structured params list, nullopt for non-functions
    std::optional<std::vector<HoverParam>> templateParameters; // <T>, <K,V>

    // --- Documentation (filled by FillHoverInfoFromDoxygen) ---
    std::string briefText;               // @brief content
    std::string detailsText;             // @details content
    std::vector<std::string> notes;      // @note entries
    std::vector<std::string> warnings;   // @warning entries
    std::string deprecated;              // @deprecated content
    std::string returnDoc;               // @return description (cross-linked with returnType)

    // --- Extras ---
    int overloadCount = 0;               // 0 = no extra overloads
    std::string templateSubstitution;    // "float" when hovering array<float>
    std::string enumValue;               // "= 42" for enum members
    std::string accessors;               // "{ get const; set; }"

    // --- Builtin info ---
    bool isBuiltin = false;
    std::string builtinLabel;            // "Built-in Function"

    // --- Diagnostics ---
    std::string diagnosticMessage;
    bool isDiagnosticError = false;

    /// Render to markdown using the given locale for section labels.
    std::string ToMarkdown(i18n::Locale locale) const;
};

} // namespace features
} // namespace angel_lsp
