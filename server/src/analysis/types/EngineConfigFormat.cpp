#include "analysis/types/EngineConfigConverter.h"
#include "analysis/types/EngineConfigInternal.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace angel_lsp::analysis::engine_config
{
/**
 * @brief Appends one line per entry, each prefixed with the given indentation.
 * @param[in] indent Indentation prefix for every line.
 * @param[in] entries Declarations to write, one per line.
 * @param[in,out] out Destination string receiving the lines.
 */
static void AppendIndentedLines(std::string_view indent, const std::vector<std::string>& entries, std::string& out)
{
    for (const auto& entry : entries)
    {
        out.append(indent).append(entry).append("\n");
    }
}

/**
 * @brief Appends the "<keyword> <name>" header line and the opening brace of a block.
 * @param[in] indent Indentation prefix of the block.
 * @param[in] keyword Declaration keyword such as enum, class or interface.
 * @param[in] name Declared entity name.
 * @param[in,out] out Destination string receiving the header.
 */
static void AppendBlockOpen(std::string_view indent, std::string_view keyword, std::string_view name, std::string& out)
{
    out.append(indent).append(keyword).append(" ").append(name).append("\n");
    out.append(indent).append("{\n");
}

/**
 * @brief Formats a collection of enum definitions into indented AngelScript source text.
 * @param[in] enums List of EnumEntry declarations to format.
 * @param[in] indent Indentation whitespace prefix.
 * @param[in,out] out Destination string receiving formatted source output.
 */
static void FormatEnums(const std::vector<EnumEntry>& enums, std::string_view indent, std::string& out)
{
    for (const auto& en : enums)
    {
        AppendBlockOpen(indent, "enum", en.name, out);
        for (const auto& [vName, vVal] : en.values)
        {
            out.append(indent).append("    ").append(vName).append(" = ").append(vVal).append(",\n");
        }
        out.append(indent).append("}\n\n");
    }
}

/**
 * @brief Formats a collection of class definitions into indented AngelScript stub declarations.
 * @param[in] classes List of ClassEntry declarations to format.
 * @param[in] indent Indentation whitespace prefix.
 * @param[in,out] out Destination string receiving formatted source output.
 */
static void FormatClasses(const std::vector<ClassEntry>& classes, std::string_view indent, std::string& out)
{
    const std::string memberIndent = std::string(indent) + "    ";
    for (const auto& cls : classes)
    {
        AppendBlockOpen(indent, "class", cls.name, out);
        AppendIndentedLines(memberIndent, cls.destructors, out);
        AppendIndentedLines(memberIndent, cls.constructors, out);
        AppendIndentedLines(memberIndent, cls.properties, out);
        AppendIndentedLines(memberIndent, cls.methods, out);
        out.append(indent).append("}\n\n");
    }
}

/**
 * @brief Formats every entity of one scope in declaration-kind order.
 * @param[in] scope Namespace data whose entities are written.
 * @param[in] indent Indentation whitespace prefix.
 * @param[in,out] out Destination string receiving formatted source output.
 */
static void FormatScopeEntities(const NamespaceData& scope, std::string_view indent, std::string& out)
{
    const std::string memberIndent = std::string(indent) + "    ";
    FormatEnums(scope.enums, indent, out);
    AppendIndentedLines(indent, scope.typedefs, out);
    AppendIndentedLines(indent, scope.funcdefs, out);
    for (const auto& in : scope.interfaces)
    {
        AppendBlockOpen(indent, "interface", in.name, out);
        AppendIndentedLines(memberIndent, in.methods, out);
        out.append(indent).append("}\n\n");
    }
    FormatClasses(scope.classes, indent, out);
    AppendIndentedLines(indent, scope.properties, out);
    AppendIndentedLines(indent, scope.functions, out);
}

void FormatNamespace(const NamespaceData& scope, std::string& out)
{
    if (scope.name.empty())
    {
        FormatScopeEntities(scope, "", out);
    }
    else
    {
        out += "namespace " + scope.name + "\n{\n";
        FormatScopeEntities(scope, "    ", out);
        out += "}\n\n";
    }
}
} // namespace angel_lsp::analysis::engine_config
