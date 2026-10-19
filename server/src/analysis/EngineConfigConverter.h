#pragma once

#include "config/ServerConfig.h"
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Result of converting an official AngelScript engine configuration dump to predefined stub format.
 */
struct EngineConfigResult
{
    std::string predefinedContent;
    config::EngineProperties engineProperties;
    std::vector<std::string> warnings;
};

/**
 * @brief Detects whether content is in the official AngelScript engine configuration dump format.
 *
 * Checks for signature lines produced by WriteConfigToFile / WriteConfigToStream (scripthelper.cpp),
 * such as "// AngelScript", "ep <id> <val>", "objtype", "objmthd", "objbeh", "enumval".
 *
 * @param content Text content of the file.
 * @return True if content matches the official engine configuration format.
 */
bool IsOfficialEngineConfig(std::string_view content);

/**
 * @brief Converts an official AngelScript engine configuration dump into a standard .as.predefined stub.
 *
 * Parses engine properties (ep), enums, object types, object methods, behaviours, interfaces,
 * typedefs, funcdefs, global functions, and global properties, producing standard AngelScript syntax
 * and extracting compiler-relevant EngineProperties.
 *
 * @param configContent Source content of the engine configuration file.
 * @return Transpiled predefined stub and extracted engine properties.
 */
EngineConfigResult ConvertEngineConfigToPredefined(std::string_view configContent);

} // namespace angel_lsp::analysis
