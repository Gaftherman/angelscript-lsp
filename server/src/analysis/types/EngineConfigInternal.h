#pragma once

#include "analysis/types/EngineConfigConverter.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::engine_config
{
// --- Defined in EngineConfigParse.cpp ---

/**
 * @brief Splits one engine-config dump line into words and quoted strings, stopping at a // comment.
 * @param[in] line One line of the dump; spaces, tabs and carriage returns separate tokens.
 * @return The tokens in order, with quotes removed from quoted strings; empty for blank or comment lines.
 */
std::vector<std::string> TokenizeLine(std::string_view line);

/**
 * @brief Maps one "ep <id> <value>" engine property from the dump onto the server engine properties.
 * @param[in] id AngelScript engine property identifier; unknown identifiers are ignored.
 * @param[in] val Property value as written in the dump.
 * @param[in,out] props Engine properties updated in place.
 */
void ApplyEngineProperty(int id, long long val, config::EngineProperties& props);

/**
 * @brief Finds the namespace with the given name, appending an empty one when it does not exist yet.
 * @param[in,out] scopes Namespaces collected so far, in first-seen order.
 * @param[in] name Namespace name; empty for the global namespace.
 * @return Mutable reference to the located or newly created NamespaceData.
 */
NamespaceData& FindOrCreateScope(std::vector<NamespaceData>& scopes, std::string_view name);

/**
 * @brief Records object behaviours, methods and properties (objbeh, objmthd, objprop) on their class.
 * @param[in] tok Tokens of one dump line.
 * @param[in,out] scope Namespace that owns the class.
 */
void ProcessMemberDirective(const std::vector<std::string>& tok, NamespaceData& scope);

/**
 * @brief Records type, enum or global entity declarations from one dump line.
 * @param[in] tok Tokens of one dump line.
 * @param[in,out] scope Namespace that receives the declaration.
 */
void ProcessDeclarationDirective(const std::vector<std::string>& tok, NamespaceData& scope);

// --- Defined in EngineConfigFormat.cpp ---

/**
 * @brief Writes one namespace as AngelScript stub text, wrapping it in a namespace block unless it is global.
 * @param[in] scope Namespace data to write.
 * @param[in,out] out Destination string receiving the stub text.
 */
void FormatNamespace(const NamespaceData& scope, std::string& out);
} // namespace angel_lsp::analysis::engine_config
