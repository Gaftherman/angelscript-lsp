#pragma once
#include <string>
#include <cstdint>
#include <filesystem>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::utils
{
    std::string UriToPath(const std::string &uri);
    std::string PathToUri(const std::string &filePath);
    size_t PositionToOffset(const std::string &text, uint32_t line, uint32_t character);
    void ApplyIncrementalChange(std::string &buffer,
                                uint32_t startLine, uint32_t startCharacter,
                                uint32_t endLine, uint32_t endCharacter,
                                const std::string &newText);
    bool IsPredefinedFile(const std::string_view &fileUri, const std::string_view extension);

    /**
     * @brief Checks whether the given type name is a standard AngelScript primitive type.
     * @param typeName The name of the type to check.
     * @return True if the type is a standard AngelScript primitive type; otherwise false.
     */
    bool IsPrimitiveType(const std::string &typeName);
}