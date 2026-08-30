#include "utils/Utils.h"

#include <algorithm>
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
        if (normalized.empty())
            return "file:///";
        if (normalized[0] == '/')
            return "file://" + normalized;
        return "file:///" + normalized;
    }

    size_t PositionToOffset(const std::string &text, uint32_t line, uint32_t character, PositionEncoding enc)
    {
        // character is in the negotiated encoding (UTF-16 code units unless the client agreed to
        // UTF-8), while every offset the rest of the server deals in is a byte offset. Resolving it
        // against the line content instead of adding it to the line start is what keeps documents
        // containing non-ASCII text from drifting: the byte offset of a column only equals the
        // column itself on a pure-ASCII line.
        const size_t lineStart = LineStartOffset(text, line);
        if (lineStart >= text.size())
            return text.size();

        const std::string_view lineText = GetLine(text, line);
        return std::min(lineStart + LspCharToByteColumn(lineText, character, enc), text.size());
    }

    void ApplyIncrementalChange(std::string &buffer,
                                uint32_t startLine, uint32_t startCharacter,
                                uint32_t endLine, uint32_t endCharacter,
                                const std::string &newText,
                                PositionEncoding enc)
    {
        size_t startOffset = PositionToOffset(buffer, startLine, startCharacter, enc);
        size_t endOffset = PositionToOffset(buffer, endLine, endCharacter, enc);

        if (startOffset <= endOffset)
        {
            buffer.replace(startOffset, endOffset - startOffset, newText);
        }
    }

    bool IsPredefinedFile(const std::string_view &fileUri, const std::string_view extension)
    {
        // AngelScript's own convention is a file named exactly `as.predefined`, and that is what
        // the community's stubs are called. It is not matched by the configured suffix - the
        // default `.as.predefined` wants a dot where such a path has a separator - so it is
        // recognised on its own. Without this the Sven Coop stub, 646 KB describing the entire
        // engine API, is never picked up by a workspace scan and every host type stays invisible.
        constexpr std::string_view k_conventionalName = "as.predefined";
        if (fileUri.ends_with(k_conventionalName))
        {
            const size_t nameStart = fileUri.size() - k_conventionalName.size();
            if (nameStart == 0 || fileUri[nameStart - 1] == '/' || fileUri[nameStart - 1] == '\\')
                return true;
        }

        if (extension.empty())
            return false;
        return fileUri.ends_with(extension) || fileUri.ends_with(fmt::format("/{}", extension));
    }

    bool IsPrimitiveType(const std::string &typeName)
    {
        static const std::unordered_set<std::string> primitiveTypes = {
            "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64",
            "float", "double", "bool", "void"
        };
        return primitiveTypes.find(typeName) != primitiveTypes.end();
    }
}