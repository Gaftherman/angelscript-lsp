#include "utils/Utils.h"

#include <unordered_set>

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

    std::string PathToUri(const std::string &filePath)
    {
        std::string normalized = filePath;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
#if defined(_WIN32)
        return "file:///" + normalized;
#else
        return "file://" + normalized;
#endif
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
                                uint32_t endLine, uint32_t endCharacter,
                                const std::string &newText)
    {
        size_t startOffset = PositionToOffset(buffer, startLine, startCharacter);
        size_t endOffset = PositionToOffset(buffer, endLine, endCharacter);

        if (startOffset <= endOffset)
        {
            buffer.replace(startOffset, endOffset - startOffset, newText);
        }
    }

    bool IsPredefinedFile(const std::string_view &fileUri, const std::string_view extension)
    {
        if (extension.empty())
            return false;
        return fileUri.ends_with(extension) || fileUri.ends_with(fmt::format("/{}", extension));
    }
}