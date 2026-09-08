#include "analysis/DocComment.h"
#include "analysis/DoxygenMarkdown.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
    namespace
    {
        /** @brief Splits text into lines, dropping a trailing carriage return from each. */
        std::vector<std::string> SplitLines(const std::string &str)
        {
            std::vector<std::string> lines;
            std::string line;
            std::istringstream stream(str);
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.push_back(line);
            }
            return lines;
        }

        /** @brief Strips leading and trailing whitespace. */
        std::string Trim(const std::string &str)
        {
            size_t first = str.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return "";
            }
            size_t last = str.find_last_not_of(" \t\r\n");
            return str.substr(first, (last - first + 1));
        }
    }

    std::string ExtractDocComment(const std::string &sourceCode, uint32_t declStartLine)
    {
        if (declStartLine == 0 || sourceCode.empty())
        {
            return "";
        }

        auto lines = SplitLines(sourceCode);
        if (declStartLine > lines.size())
        {
            return "";
        }

        std::vector<std::string> commentLines;
        int currentLine = static_cast<int>(declStartLine) - 1;

        while (currentLine >= 0 && Trim(lines[currentLine]).empty())
        {
            currentLine--;
        }

        if (currentLine < 0)
        {
            return "";
        }

        std::string trimmed = Trim(lines[currentLine]);
        if (trimmed.ends_with("*/"))
        {
            // Block comment: scan upwards to find /* or /**
            while (currentLine >= 0)
            {
                std::string l = lines[currentLine];
                commentLines.push_back(l);
                if (l.find("/*") != std::string::npos)
                {
                    break;
                }
                currentLine--;
            }
            std::reverse(commentLines.begin(), commentLines.end());
        }
        else if (trimmed.starts_with("//"))
        {
            // Line comments: scan upwards as long as lines start with //
            while (currentLine >= 0)
            {
                std::string l = Trim(lines[currentLine]);
                if (l.starts_with("//"))
                {
                    commentLines.push_back(lines[currentLine]);
                    currentLine--;
                }
                else
                {
                    break;
                }
            }
            std::reverse(commentLines.begin(), commentLines.end());
        }
        else
        {
            return "";
        }

        if (commentLines.empty())
        {
            return "";
        }

        // Return the RAW comment text: do not strip comment delimiters or leading stars here,
        // because tree-sitter-doxygen grammar matches the openers and handles leading stars.
        std::string rawComment;
        for (size_t i = 0; i < commentLines.size(); ++i)
        {
            if (i > 0)
            {
                rawComment += "\n";
            }
            rawComment += commentLines[i];
        }

        return RenderDoxygenMarkdown(rawComment);
    }
}
