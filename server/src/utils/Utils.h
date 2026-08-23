#pragma once
#include <string>
#include <cstdint>
#include <filesystem>
#include <spdlog/fmt/fmt.h>

#include "utils/PositionEncoding.h"

namespace angel_lsp::utils
{
    std::string UriToPath(const std::string &uri);
    std::string PathToUri(const std::string &filePath);
    /**
     * @brief Converts an LSP position into a byte offset into the document text.
     * @param text Full document text (UTF-8).
     * @param line 0-indexed line number.
     * @param character Character offset within the line, in the negotiated encoding.
     * @param enc The position encoding negotiated with the client.
     * @return Byte offset, clamped to the end of the line and then to the end of the text.
     */
    size_t PositionToOffset(const std::string &text, uint32_t line, uint32_t character, PositionEncoding enc);

    /**
     * @brief Applies one LSP incremental content change to the document buffer in place.
     * @param enc The position encoding negotiated with the client - the range is expressed in it.
     */
    void ApplyIncrementalChange(std::string &buffer,
                                uint32_t startLine, uint32_t startCharacter,
                                uint32_t endLine, uint32_t endCharacter,
                                const std::string &newText,
                                PositionEncoding enc);
    bool IsPredefinedFile(const std::string_view &fileUri, const std::string_view extension);

    /**
     * @brief Checks whether the given type name is a standard AngelScript primitive type.
     * @param typeName The name of the type to check.
     * @return True if the type is a standard AngelScript primitive type; otherwise false.
     */
    bool IsPrimitiveType(const std::string &typeName);
}