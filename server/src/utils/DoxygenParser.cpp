/**
 * @file DoxygenParser.cpp
 * @brief Implementation of C++20 Doxygen parser and Markdown formatter.
 * @ingroup Utils
 */

#include "utils/DoxygenParser.h"
#include "document/Document.h"
#include <tree_sitter/api.h>
#include <vector>
#include <sstream>
#include <cstring>
#include <memory>
#include <regex>
#include <algorithm>
#include <cctype>

extern "C" TSLanguage *tree_sitter_doxygen();

namespace angel_lsp::utils
{
    using document::UniqueTSParser;
    using document::UniqueTSTree;

    static std::string CleanRawCommentText(std::string_view rawComment)
    {
        std::string text(rawComment);
        if (text.empty())
        {
            return "";
        }

        if (text.starts_with("/**"))
        {
            text = text.substr(3);
        }
        else if (text.starts_with("/*!"))
        {
            text = text.substr(3);
        }
        else if (text.starts_with("/*"))
        {
            text = text.substr(2);
        }

        if (text.ends_with("*/"))
        {
            text = text.substr(0, text.length() - 2);
        }

        std::istringstream stream(text);
        std::string line;
        std::string result;

        while (std::getline(stream, line))
        {
            size_t start = 0;
            while (start < line.length() && (line[start] == ' ' || line[start] == '\t'))
            {
                start++;
            }

            if (line.compare(start, 3, "///") == 0)
            {
                start += 3;
            }
            else if (line.compare(start, 2, "//") == 0)
            {
                start += 2;
            }
            else if (start < line.length() && line[start] == '*')
            {
                start++;
            }

            if (start < line.length() && line[start] == ' ')
            {
                start++;
            }

            std::string cleanedLine = line.substr(start);
            if (!result.empty())
            {
                result += "\n";
            }
            result += cleanedLine;
        }

        size_t first = result.find_first_not_of(" \t\n\r");
        if (first == std::string::npos)
        {
            return "";
        }
        size_t last = result.find_last_not_of(" \t\n\r");
        return result.substr(first, last - first + 1);
    }

    static std::string ApplyInlineFormatting(std::string text)
    {
        if (text.empty())
        {
            return "";
        }

        size_t codePos = 0;
        while ((codePos = text.find("@code")) != std::string::npos || (codePos = text.find("\\code")) != std::string::npos)
        {
            size_t endCodePos = text.find("@endcode", codePos);
            size_t endLen = 8;
            if (endCodePos == std::string::npos)
            {
                endCodePos = text.find("\\endcode", codePos);
            }
            if (endCodePos != std::string::npos)
            {
                std::string codeBody = text.substr(codePos + 5, endCodePos - (codePos + 5));
                while (!codeBody.empty() && (codeBody.front() == '\n' || codeBody.front() == '\r'))
                {
                    codeBody.erase(codeBody.begin());
                }
                std::string formattedBlock = "```angelscript\n" + codeBody + "\n```";
                text.replace(codePos, (endCodePos + endLen) - codePos, formattedBlock);
            }
            else
            {
                break;
            }
        }

        std::regex codeWordRegex(R"([@\\][cp]\s+([A-Za-z0-9_.]+))");
        text = std::regex_replace(text, codeWordRegex, "`$1`");

        std::regex boldWordRegex(R"([@\\]b\s+([A-Za-z0-9_.]+))");
        text = std::regex_replace(text, boldWordRegex, "**$1**");

        std::regex italicWordRegex(R"([@\\][ia]\s+([A-Za-z0-9_.]+))");
        text = std::regex_replace(text, italicWordRegex, "*$1*");

        return text;
    }

    DoxygenDoc ParseDoxygen(std::string_view rawComment)
    {
        DoxygenDoc doc;
        if (rawComment.empty())
        {
            return doc;
        }

        std::string cleanedText = CleanRawCommentText(rawComment);
        if (cleanedText.empty())
        {
            return doc;
        }

        // 1. IMPLICIT PLAIN TEXT FALLBACK: No '@' or '\' tags
        if (cleanedText.find_first_of("@\\") == std::string::npos)
        {
            doc.briefText = ApplyInlineFormatting(cleanedText);
            doc.brief = doc.briefText;
            return doc;
        }

        // 2. TAG PARSING: Parse block line-by-line and tag-by-tag
        std::istringstream stream(cleanedText);
        std::string line;

        std::string currentTag;
        std::string currentContent;

        auto flushCurrentTag = [&](const std::string &tag, const std::string &content)
        {
            if (tag.empty() && content.empty())
            {
                return;
            }

            std::string formattedContent = ApplyInlineFormatting(content);

            if (tag.empty())
            {
                if (doc.briefText.empty())
                {
                    doc.briefText = formattedContent;
                }
                else
                {
                    if (!doc.detailsText.empty())
                    {
                        doc.detailsText += "\n";
                    }
                    doc.detailsText += formattedContent;
                }
                return;
            }

            std::string lowerTag = tag;
            if (lowerTag.front() == '@' || lowerTag.front() == '\\')
            {
                lowerTag = lowerTag.substr(1);
            }

            std::transform(lowerTag.begin(), lowerTag.end(), lowerTag.begin(), [](unsigned char c) { return (char)std::tolower(c); });

            if (lowerTag == "brief")
            {
                doc.briefText = formattedContent;
            }
            else if (lowerTag == "details")
            {
                if (!doc.detailsText.empty())
                {
                    doc.detailsText += "\n";
                }
                doc.detailsText += formattedContent;
            }
            else if (lowerTag == "return" || lowerTag == "returns")
            {
                doc.returnDoc = formattedContent;
                doc.returns = formattedContent;
            }
            else if (lowerTag == "deprecated")
            {
                doc.deprecatedDoc = formattedContent;
                doc.deprecated = formattedContent;
            }
            else if (lowerTag == "note")
            {
                doc.notes.push_back(formattedContent);
                if (!doc.note.empty())
                {
                    doc.note += "\n";
                }
                doc.note += formattedContent;
            }
            else if (lowerTag == "warning")
            {
                doc.warnings.push_back(formattedContent);
                if (!doc.warning.empty())
                {
                    doc.warning += "\n";
                }
                doc.warning += formattedContent;
            }
            else if (lowerTag == "see" || lowerTag == "sa")
            {
                doc.seeAlso.push_back(formattedContent);
            }
            else if (lowerTag == "param" || lowerTag == "tparam")
            {
                DoxygenParam p;
                std::string rest = formattedContent;

                // Check for direction bracket [in], [out], [in,out], [out,in]
                if (rest.front() == '[')
                {
                    size_t bracketEnd = rest.find(']');
                    if (bracketEnd != std::string::npos)
                    {
                        p.direction = rest.substr(1, bracketEnd - 1);
                        rest = rest.substr(bracketEnd + 1);
                        size_t firstNonSpace = rest.find_first_not_of(" \t");
                        if (firstNonSpace != std::string::npos)
                        {
                            rest = rest.substr(firstNonSpace);
                        }
                    }
                }

                // Extract parameter name
                size_t spacePos = rest.find_first_of(" \t");
                if (spacePos != std::string::npos)
                {
                    p.name = rest.substr(0, spacePos);
                    std::string desc = rest.substr(spacePos + 1);
                    size_t firstNonSpace = desc.find_first_not_of(" \t");
                    if (firstNonSpace != std::string::npos)
                    {
                        desc = desc.substr(firstNonSpace);
                    }
                    p.description = desc;
                }
                else
                {
                    p.name = rest;
                }

                if (lowerTag == "tparam")
                {
                    doc.tparams.push_back(p);
                }
                else
                {
                    doc.params.push_back(p);
                    doc.parameters.push_back(p);
                }
            }
            else if (lowerTag == "throws" || lowerTag == "exception")
            {
                std::string exType;
                std::string exDesc;
                size_t spacePos = formattedContent.find_first_of(" \t");
                if (spacePos != std::string::npos)
                {
                    exType = formattedContent.substr(0, spacePos);
                    exDesc = formattedContent.substr(spacePos + 1);
                }
                else
                {
                    exType = formattedContent;
                }
                doc.throwsDocs.push_back({exType, exDesc});
            }
            else
            {
                // Generic tag fallback (e.g. @author, @todo, @bug)
                std::string tagLabel = tag.substr(1);
                if (!tagLabel.empty())
                {
                    tagLabel[0] = (char)std::toupper(tagLabel[0]);
                }
                doc.genericTags.push_back({tagLabel, formattedContent});
            }
        };

        while (std::getline(stream, line))
        {
            size_t firstChar = line.find_first_not_of(" \t");
            if (firstChar == std::string::npos)
            {
                continue;
            }

            if (line[firstChar] == '@' || line[firstChar] == '\\')
            {
                if (!currentTag.empty() || !currentContent.empty())
                {
                    flushCurrentTag(currentTag, currentContent);
                    currentContent.clear();
                }

                size_t tagEnd = line.find_first_of(" \t\n\r", firstChar);
                if (tagEnd == std::string::npos)
                {
                    currentTag = line.substr(firstChar);
                    currentContent = "";
                }
                else
                {
                    currentTag = line.substr(firstChar, tagEnd - firstChar);
                    size_t contentStart = line.find_first_not_of(" \t", tagEnd);
                    if (contentStart != std::string::npos)
                    {
                        currentContent = line.substr(contentStart);
                    }
                }
            }
            else
            {
                if (!currentContent.empty())
                {
                    currentContent += "\n";
                }
                currentContent += line.substr(firstChar);
            }
        }

        flushCurrentTag(currentTag, currentContent);

        doc.brief = doc.briefText;
        doc.details = doc.detailsText;

        return doc;
    }

    std::string FormatDoxygenToMarkdown(const DoxygenDoc &doc, i18n::Locale locale)
    {
        const auto headers = i18n::GetDoxygenHeaders(locale);
        std::vector<std::string> blocks;

        // 1. Brief
        if (!doc.briefText.empty())
        {
            blocks.push_back(doc.briefText);
        }

        // 2. Type Parameters
        if (!doc.tparams.empty())
        {
            std::string tpBlock = "### " + std::string(headers.typeParameters) + "\n";
            for (const auto &tp : doc.tparams)
            {
                tpBlock += "- `" + tp.name + "`";
                if (!tp.description.empty())
                {
                    tpBlock += " \xE2\x80\x94 " + tp.description;
                }
                tpBlock += "\n";
            }
            if (tpBlock.back() == '\n')
            {
                tpBlock.pop_back();
            }
            blocks.push_back(tpBlock);
        }

        // 3. Parameters
        if (!doc.params.empty())
        {
            std::string pBlock = "### " + std::string(headers.parameters) + "\n";
            for (const auto &p : doc.params)
            {
                pBlock += "- `" + p.name + "`";
                if (!p.direction.empty())
                {
                    pBlock += " `[" + p.direction + "]`";
                }
                if (!p.description.empty())
                {
                    pBlock += " \xE2\x80\x94 " + p.description;
                }
                pBlock += "\n";
            }
            if (pBlock.back() == '\n')
            {
                pBlock.pop_back();
            }
            blocks.push_back(pBlock);
        }

        // 4. Returns
        if (!doc.returnDoc.empty())
        {
            blocks.push_back("### " + std::string(headers.returns) + "\n" + doc.returnDoc);
        }

        // 5. Exceptions
        if (!doc.throwsDocs.empty())
        {
            std::string exBlock = "### " + std::string(headers.exceptions) + "\n";
            for (const auto &ex : doc.throwsDocs)
            {
                exBlock += "- `" + ex.first + "`";
                if (!ex.second.empty())
                {
                    exBlock += " \xE2\x80\x94 " + ex.second;
                }
                exBlock += "\n";
            }
            if (exBlock.back() == '\n')
            {
                exBlock.pop_back();
            }
            blocks.push_back(exBlock);
        }

        // 6. Warnings & Deprecated
        for (const auto &warn : doc.warnings)
        {
            blocks.push_back("**" + std::string(headers.warning) + "** " + warn);
        }
        if (!doc.deprecatedDoc.empty())
        {
            blocks.push_back("**" + std::string(headers.deprecated) + "** " + doc.deprecatedDoc);
        }

        // 7. Details
        if (!doc.detailsText.empty())
        {
            blocks.push_back(doc.detailsText);
        }

        // 8. Notes
        for (const auto &note : doc.notes)
        {
            blocks.push_back("**" + std::string(headers.note) + "** " + note);
        }

        // 9. See Also
        if (!doc.seeAlso.empty())
        {
            std::string saBlock = "### " + std::string(headers.seeAlso) + "\n";
            for (const auto &sa : doc.seeAlso)
            {
                saBlock += "- " + sa + "\n";
            }
            if (saBlock.back() == '\n')
            {
                saBlock.pop_back();
            }
            blocks.push_back(saBlock);
        }

        // 10. Generic Tags
        for (const auto &gt : doc.genericTags)
        {
            blocks.push_back("> **" + gt.first + ":** " + gt.second);
        }

        // Assembly
        std::string result;
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            result += blocks[i];
            if (i + 1 < blocks.size())
            {
                result += "\n\n";
            }
        }

        return result;
    }

    DoxygenDoc ParseDoxygenComment(const std::string &rawDoxygen)
    {
        return ParseDoxygen(rawDoxygen);
    }

    std::string FormatDoxygenToMarkdown(const std::string &rawDoxygen, i18n::Locale locale, const std::string &targetParam)
    {
        if (rawDoxygen.empty())
        {
            return "";
        }

        DoxygenDoc doc = ParseDoxygen(rawDoxygen);

        if (!targetParam.empty())
        {
            for (const auto &p : doc.params)
            {
                if (p.name == targetParam)
                {
                    return "```angelscript\n" + targetParam + "\n```\n" + p.description;
                }
            }
            return "";
        }

        return FormatDoxygenToMarkdown(doc, locale);
    }

} // namespace angel_lsp::utils
