#include "analysis/DocComment.h"

#include <algorithm>
#include <sstream>
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
                    commentLines.push_back(l);
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

        // Clean comment lines
        std::vector<std::string> cleanLines;
        for (const auto &raw : commentLines)
        {
            std::string l = Trim(raw);
            if (l.starts_with("/**"))
            {
                l = l.substr(3);
            }
            else if (l.starts_with("/*"))
            {
                l = l.substr(2);
            }
            else if (l.starts_with("///"))
            {
                l = l.substr(3);
            }
            else if (l.starts_with("//"))
            {
                l = l.substr(2);
            }

            if (l.ends_with("*/"))
            {
                l = l.substr(0, l.size() - 2);
            }

            l = Trim(l);
            if (l.starts_with("*"))
            {
                l = Trim(l.substr(1));
            }

            if (!l.empty() || !cleanLines.empty())
            {
                cleanLines.push_back(l);
            }
        }

        while (!cleanLines.empty() && cleanLines.back().empty())
        {
            cleanLines.pop_back();
        }

        if (cleanLines.empty())
        {
            return "";
        }

        // Parse Doxygen tags
        std::ostringstream out;
        std::vector<std::string> mainText;
        std::vector<std::pair<std::string, std::string>> params;
        std::vector<std::string> returns;
        std::vector<std::string> notes;
        std::vector<std::string> warnings;
        std::vector<std::string> sees;

        for (const auto &line : cleanLines)
        {
            if (line.starts_with("@brief ") || line.starts_with("\\brief "))
            {
                mainText.push_back(line.substr(7));
            }
            else if (line.starts_with("@param ") || line.starts_with("\\param "))
            {
                std::string rem = Trim(line.substr(7));
                size_t spacePos = rem.find_first_of(" \t");
                if (spacePos != std::string::npos)
                {
                    std::string paramName = rem.substr(0, spacePos);
                    std::string paramDesc = Trim(rem.substr(spacePos + 1));
                    params.emplace_back(paramName, paramDesc);
                }
                else
                {
                    params.emplace_back(rem, "");
                }
            }
            else if (line.starts_with("@tparam ") || line.starts_with("\\tparam "))
            {
                std::string rem = Trim(line.substr(8));
                size_t spacePos = rem.find_first_of(" \t");
                if (spacePos != std::string::npos)
                {
                    std::string paramName = rem.substr(0, spacePos);
                    std::string paramDesc = Trim(rem.substr(spacePos + 1));
                    params.emplace_back("<" + paramName + ">", paramDesc);
                }
                else
                {
                    params.emplace_back("<" + rem + ">", "");
                }
            }
            else if (line.starts_with("@return ") || line.starts_with("\\return ") ||
                     line.starts_with("@returns ") || line.starts_with("\\returns "))
            {
                size_t tagLen = (line.starts_with("@returns ") || line.starts_with("\\returns ")) ? 9 : 8;
                returns.push_back(Trim(line.substr(tagLen)));
            }
            else if (line.starts_with("@note ") || line.starts_with("\\note "))
            {
                notes.push_back(Trim(line.substr(6)));
            }
            else if (line.starts_with("@warning ") || line.starts_with("\\warning "))
            {
                warnings.push_back(Trim(line.substr(9)));
            }
            else if (line.starts_with("@see ") || line.starts_with("\\see "))
            {
                sees.push_back(Trim(line.substr(5)));
            }
            else
            {
                mainText.push_back(line);
            }
        }

        bool hasContent = false;
        for (const auto &m : mainText)
        {
            if (hasContent)
            {
                out << "\n";
            }
            out << m;
            hasContent = true;
        }

        if (!params.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            out << "**Parameters:**\n";
            for (const auto &p : params)
            {
                out << "- `" << p.first << "`" << (p.second.empty() ? "" : ": " + p.second) << "\n";
            }
            hasContent = true;
        }

        if (!returns.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            out << "**Returns:**\n";
            for (const auto &r : returns)
            {
                out << "- " << r << "\n";
            }
            hasContent = true;
        }

        if (!notes.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            for (const auto &n : notes)
            {
                out << "> **Note:** " << n << "\n";
            }
            hasContent = true;
        }

        if (!warnings.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            for (const auto &w : warnings)
            {
                out << "> **Warning:** " << w << "\n";
            }
            hasContent = true;
        }

        if (!sees.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            out << "**See also:** ";
            for (size_t i = 0; i < sees.size(); ++i)
            {
                if (i > 0)
                {
                    out << ", ";
                }
                out << sees[i];
            }
            out << "\n";
        }

        return Trim(out.str());
    }
}
