/**
 * @file DoxygenParser.cpp
 * @brief Implementation of Tree-Sitter based Doxygen docstring parser and Markdown formatter.
 * @ingroup Utils
 */

#include "utils/DoxygenParser.h"
#include "document/Document.h"
#include <tree_sitter/api.h>
#include <vector>
#include <sstream>
#include <cstring>
#include <memory>

extern "C" TSLanguage *tree_sitter_doxygen();

namespace angel_lsp::utils
{
    using document::UniqueTSParser;
    using document::UniqueTSTree;

    // Helper to get text from a node
    static std::string GetNodeText(TSNode node, const std::string &source)
    {
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (start < end && end <= source.length())
        {
            return source.substr(start, end - start);
        }
        return "";
    }

    // Helper to remove leading/trailing whitespace and asterisks from extracted text
    static std::string CleanText(std::string text)
    {
        // Strip leading whitespace and asterisks
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r' || text.front() == '*'))
        {
            text.erase(text.begin());
        }
        // Strip trailing whitespace
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
        {
            text.pop_back();
        }

        // Remove ' * ' at the beginning of each line
        size_t pos = 0;
        while ((pos = text.find("\n", pos)) != std::string::npos)
        {
            pos++; // move past \n
            while (pos < text.length() && (text[pos] == ' ' || text[pos] == '\t'))
            {
                text.erase(pos, 1);
            }
            if (pos < text.length() && text[pos] == '*')
            {
                text.erase(pos, 1);
            }
            if (pos < text.length() && text[pos] == ' ')
            {
                text.erase(pos, 1);
            }
        }

        return text;
    }

    static std::string StripDoxygenTags(std::string text)
    {
        static const std::vector<std::string> tags = {
            "@brief", "\\brief", "@details", "\\details",
            "@note", "\\note", "@warning", "\\warning",
            "@deprecated", "\\deprecated", "@return", "\\return", "@returns", "\\returns"};
        for (const auto &tag : tags)
        {
            if (text.starts_with(tag))
            {
                text = text.substr(tag.length());
                break;
            }
        }
        size_t first = text.find_first_not_of(" \t\n\r");
        if (first == std::string::npos)
        {
            return "";
        }
        size_t last = text.find_last_not_of(" \t\n\r");
        return text.substr(first, (last - first + 1));
    }

    static std::string TrimString(const std::string &str)
    {
        return StripDoxygenTags(str);
    }

    static void ProcessNode(TSNode node, const std::string &wrappedDoxygen, ParsedDoxygenDoc &doc)
    {
        uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            TSNode child = ts_node_child(node, i);
            const char *nodeType = ts_node_type(child);

            if (strcmp(nodeType, "brief_header") == 0)
            {
                std::string txt = CleanText(GetNodeText(child, wrappedDoxygen));
                txt = StripDoxygenTags(txt);
                if (!txt.empty())
                {
                    doc.brief = txt;
                }
            }
            else if (strcmp(nodeType, "tag") == 0)
            {
                std::string tagName = "";
                std::string identifier = "";
                std::string description = "";

                uint32_t tagChildCount = ts_node_child_count(child);
                for (uint32_t j = 0; j < tagChildCount; ++j)
                {
                    TSNode sub = ts_node_child(child, j);
                    const char *subType = ts_node_type(sub);

                    if (strcmp(subType, "tag_name") == 0)
                    {
                        tagName = CleanText(GetNodeText(sub, wrappedDoxygen));
                    }
                    else if (strcmp(subType, "identifier") == 0)
                    {
                        identifier = CleanText(GetNodeText(sub, wrappedDoxygen));
                    }
                    else if (strcmp(subType, "description") == 0)
                    {
                        description = CleanText(GetNodeText(sub, wrappedDoxygen));
                    }
                }
                if (description.empty())
                {
                    for (uint32_t j = 0; j < tagChildCount; ++j)
                    {
                        TSNode sub = ts_node_child(child, j);
                        const char *subType = ts_node_type(sub);
                        if (strcmp(subType, "tag_name") != 0 && strcmp(subType, "identifier") != 0)
                        {
                            std::string subTxt = CleanText(GetNodeText(sub, wrappedDoxygen));
                            if (!subTxt.empty())
                            {
                                if (!description.empty())
                                {
                                    description += " ";
                                }
                                description += subTxt;
                            }
                        }
                    }
                }
                description = StripDoxygenTags(description);

                if (tagName == "@brief" || tagName == "\\brief")
                {
                    doc.brief = description;
                }
                else if (tagName == "@details" || tagName == "\\details")
                {
                    doc.details = description;
                }
                else if (tagName == "@return" || tagName == "\\return" || tagName == "@returns" || tagName == "\\returns")
                {
                    doc.returns = description;
                }
                else if (tagName == "@note" || tagName == "\\note")
                {
                    if (!doc.note.empty())
                    {
                        doc.note += "\n";
                    }
                    doc.note += description;
                }
                else if (tagName == "@warning" || tagName == "\\warning")
                {
                    if (!doc.warning.empty())
                    {
                        doc.warning += "\n";
                    }
                    doc.warning += description;
                }
                else if (tagName == "@deprecated" || tagName == "\\deprecated")
                {
                    doc.deprecated = description;
                }
                else if (tagName == "@param" || tagName == "\\param" || tagName == "@tparam" || tagName == "\\tparam")
                {
                    DoxygenParam p;
                    p.name = identifier;
                    p.description = description;
                    doc.parameters.push_back(p);
                }
                else if (tagName.length() > 1)
                {
                    std::string tagLabel = tagName.substr(1);
                    tagLabel[0] = toupper(tagLabel[0]);
                    if (!doc.details.empty())
                    {
                        doc.details += "\n";
                    }
                    doc.details += "> **" + tagLabel + ":** " + description;
                }
            }
            else if (strcmp(nodeType, "text") == 0 || strcmp(nodeType, "text_block") == 0)
            {
                std::string txt = CleanText(GetNodeText(child, wrappedDoxygen));
                if (!txt.empty() && txt != "/" && txt != "/**" && txt != "*/")
                {
                    if (!doc.details.empty())
                    {
                        doc.details += " ";
                    }
                    doc.details += txt;
                }
            }
            else
            {
                ProcessNode(child, wrappedDoxygen, doc);
            }
        }
    }

    ParsedDoxygenDoc ParseDoxygenComment(const std::string &rawDoxygen)
    {
        ParsedDoxygenDoc doc;
        if (rawDoxygen.empty())
        {
            return doc;
        }

        std::string wrappedDoxygen = rawDoxygen;
        if (wrappedDoxygen.find("/**") == std::string::npos && wrappedDoxygen.find("/*!") == std::string::npos)
        {
            wrappedDoxygen = "/**\n" + wrappedDoxygen + "\n*/";
        }

        UniqueTSParser parser(ts_parser_new());
        if (!parser)
        {
            return doc;
        }
        ts_parser_set_language(parser.get(), tree_sitter_doxygen());

        UniqueTSTree tree(ts_parser_parse_string(parser.get(), nullptr, wrappedDoxygen.c_str(), wrappedDoxygen.length()));
        if (!tree)
        {
            return doc;
        }
        TSNode root = ts_tree_root_node(tree.get());

        ProcessNode(root, wrappedDoxygen, doc);

        doc.brief = TrimString(doc.brief);
        doc.details = TrimString(doc.details);

        if (doc.brief.empty() && !doc.details.empty())
        {
            doc.brief = doc.details;
            doc.details.clear();
        }
        else if (!doc.details.empty() && doc.details == doc.brief)
        {
            doc.details.clear();
        }

        return doc;
    }

    std::string FormatDoxygenToMarkdown(const std::string &rawDoxygen, i18n::Locale locale, const std::string &targetParam)
    {
        if (rawDoxygen.empty())
        {
            return "";
        }

        ParsedDoxygenDoc doc = ParseDoxygenComment(rawDoxygen);

        if (!targetParam.empty())
        {
            for (const auto &p : doc.parameters)
            {
                if (p.name == targetParam)
                {
                    return "```angelscript\n" + targetParam + "\n```\n" + p.description;
                }
            }
            return "";
        }

        std::string md;
        if (!doc.brief.empty())
        {
            md += doc.brief;
        }
        if (!doc.details.empty())
        {
            if (!md.empty())
            {
                md += "\n\n";
            }
            md += doc.details;
        }
        if (!doc.returns.empty())
        {
            if (!md.empty())
            {
                md += "\n\n";
            }
            md += "**Returns:** " + doc.returns;
        }
        return md;
    }
}
