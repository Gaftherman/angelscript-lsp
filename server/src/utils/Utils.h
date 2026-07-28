#pragma once
#include <string>
#include <cstdint>

namespace angel_lsp::utils
{
    std::string UriToPath(const std::string &uri);
    size_t PositionToOffset(const std::string &text, uint32_t line, uint32_t character);
    void ApplyIncrementalChange(std::string &buffer,
                               uint32_t startLine, uint32_t startCharacter,
                               uint32_t endLine,   uint32_t endCharacter,
                               const std::string &newText);
}