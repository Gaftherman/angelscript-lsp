#include "utils/Utils.h"
#include <algorithm>

namespace angel_lsp::utils
{
    std::string UriToPath(const std::string &uri)
    {
#if defined(_WIN32)
        if (!uri.empty() && uri[0] == '/')
            return uri.substr(1);
#endif
        return uri;
    }

    size_t PositionToOffset(const std::string &text, uint32_t line, uint32_t character)
    {
        size_t currentLine = 0;
        size_t offset = 0;

        while (offset < text.size() && currentLine < line)
        {
            if (text[offset] == '\n')
                currentLine++;
            offset++;
        }
        return std::min(offset + character, text.size());
    }

    void ApplyIncrementalChange(std::string &buffer,
                               uint32_t startLine, uint32_t startCharacter,
                               uint32_t endLine,   uint32_t endCharacter,
                               const std::string &newText)
    {
        size_t startOffset = PositionToOffset(buffer, startLine, startCharacter);
        size_t endOffset   = PositionToOffset(buffer, endLine,   endCharacter);

        if (startOffset <= endOffset)
        {
            buffer.replace(startOffset, endOffset - startOffset, newText);
        }
    }
}