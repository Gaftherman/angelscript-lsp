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
 *        Checks for signature lines produced by WriteConfigToFile / WriteConfigToStream (scripthelper.cpp),
 * @param content Text content of the file.
 * @return True if content matches the official engine configuration format.
 */
bool IsOfficialEngineConfig(std::string_view content);

/**
 * @brief Converts an official AngelScript engine configuration dump into a standard .as.predefined stub.
 *        Parses engine properties (ep), enums, object types, object methods, behaviours, interfaces,
 * @param configContent Source content of the engine configuration file.
 * @return Transpiled predefined stub and extracted engine properties.
 */
EngineConfigResult ConvertEngineConfigToPredefined(std::string_view configContent);

/**
 * @brief Parsed enumeration metadata mapping symbol names to their backing integer or string representations.
 */
struct EnumEntry
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> values;
};

/**
 * @brief Parsed class declaration metadata storing base inheritance hierarchy, interfaces, and member lists.
 */
struct ClassEntry
{
    std::string name;
    std::vector<std::string> constructors;
    std::vector<std::string> destructors;
    std::vector<std::string> properties;
    std::vector<std::string> methods;
};

/**
 * @brief Parsed interface declaration metadata specifying required abstract method signatures.
 */
struct InterfaceEntry
{
    std::string name;
    std::vector<std::string> methods;
};

/**
 * @brief Hierarchical namespace metadata storing enclosed symbols, nested declarations, and scope descriptors.
 */
struct NamespaceData
{
    std::string name;
    std::vector<EnumEntry> enums;
    std::vector<ClassEntry> classes;
    std::vector<InterfaceEntry> interfaces;
    std::vector<std::string> typedefs;
    std::vector<std::string> funcdefs;
    std::vector<std::string> functions;
    std::vector<std::string> properties;
};
} // namespace angel_lsp::analysis
