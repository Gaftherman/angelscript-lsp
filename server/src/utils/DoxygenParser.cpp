#include "DoxygenParser.h"
#include <tree_sitter/api.h>
#include <vector>
#include <sstream>

extern "C" TSLanguage *tree_sitter_doxygen();

namespace angel_lsp {
namespace utils {

    // Helper to get text from a node
    static std::string GetNodeText(TSNode node, const std::string& source) {
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (start < end && end <= source.length()) {
            return source.substr(start, end - start);
        }
        return "";
    }

    // Helper to remove leading/trailing whitespace and asterisks from extracted text
    static std::string CleanText(std::string text) {
        // Strip leading whitespace and asterisks
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r' || text.front() == '*')) {
            text.erase(text.begin());
        }
        // Strip trailing whitespace
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        
        // Remove ' * ' at the beginning of each line
        size_t pos = 0;
        while ((pos = text.find("\n", pos)) != std::string::npos) {
            pos++; // move past \n
            while (pos < text.length() && (text[pos] == ' ' || text[pos] == '\t')) {
                text.erase(pos, 1);
            }
            if (pos < text.length() && text[pos] == '*') {
                text.erase(pos, 1);
            }
            if (pos < text.length() && text[pos] == ' ') {
                text.erase(pos, 1);
            }
        }
        
        return text;
    }

    std::string FormatDoxygenToMarkdown(const std::string& rawDoxygen, i18n::Locale locale, const std::string& targetParam) {
        if (rawDoxygen.empty()) return "";

        std::string wrappedDoxygen = rawDoxygen;
        if (wrappedDoxygen.find("/**") == std::string::npos && wrappedDoxygen.find("/*!") == std::string::npos) {
            wrappedDoxygen = "/**\n" + wrappedDoxygen + "\n*/";
        }

        TSParser *parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_doxygen());
        TSTree *tree = ts_parser_parse_string(parser, NULL, wrappedDoxygen.c_str(), wrappedDoxygen.length());
        TSNode root = ts_tree_root_node(tree);

        std::string briefText = "";
        std::vector<std::string> tparams;
        std::vector<std::string> params;
        std::string returns = "";
        std::string extraDescription = "";
        std::vector<std::string> notes;
        std::vector<std::string> warnings;
        std::string deprecated = "";

        // Iterate over top-level nodes in the document
        uint32_t childCount = ts_node_child_count(root);
        for (uint32_t i = 0; i < childCount; ++i) {
            TSNode child = ts_node_child(root, i);
            const char* nodeType = ts_node_type(child);

            if (strcmp(nodeType, "text") == 0 || strcmp(nodeType, "text_block") == 0) {
                std::string txt = CleanText(GetNodeText(child, wrappedDoxygen));
                if (!txt.empty() && txt != "/" && txt != "/**" && txt != "*/") {
                    if (!extraDescription.empty()) extraDescription += " ";
                    extraDescription += txt;
                }
            }
            else if (strcmp(nodeType, "brief_header") == 0) {
                TSNode textNode = ts_node_child_by_field_name(child, "text", 4);
                if (ts_node_is_null(textNode)) {
                    // Try to find brief_description node manually
                    uint32_t bc = ts_node_child_count(child);
                    for (uint32_t j = 0; j < bc; ++j) {
                        TSNode sub = ts_node_child(child, j);
                        if (strcmp(ts_node_type(sub), "brief_description") == 0) {
                            briefText = CleanText(GetNodeText(sub, wrappedDoxygen));
                            break;
                        }
                    }
                } else {
                    briefText = CleanText(GetNodeText(textNode, wrappedDoxygen));
                }
            }
            else if (strcmp(nodeType, "tag") == 0) {
                // Find tag_name
                std::string tagName = "";
                std::string identifier = "";
                std::string description = "";

                uint32_t tc = ts_node_child_count(child);
                for (uint32_t j = 0; j < tc; ++j) {
                    TSNode sub = ts_node_child(child, j);
                    const char* subType = ts_node_type(sub);
                    if (strcmp(subType, "tag_name") == 0) {
                        tagName = CleanText(GetNodeText(sub, wrappedDoxygen));
                    } else if (strcmp(subType, "identifier") == 0) {
                        identifier = CleanText(GetNodeText(sub, wrappedDoxygen));
                    } else if (strcmp(subType, "description") == 0) {
                        description = CleanText(GetNodeText(sub, wrappedDoxygen));
                    }
                }

                if (tagName == "@tparam" || tagName == "\\tparam") {
                    tparams.push_back("- **" + identifier + "** " + description);
                } else if (tagName == "@param" || tagName == "\\param") {
                    params.push_back("- **" + identifier + "** " + description);
                } else if (tagName == "@return" || tagName == "\\return" || tagName == "@returns" || tagName == "\\returns") {
                    returns = description;
                } else if (tagName == "@brief" || tagName == "\\brief") {
                    briefText = description;
                } else if (tagName == "@note" || tagName == "\\note") {
                    notes.push_back(description);
                } else if (tagName == "@warning" || tagName == "\\warning") {
                    warnings.push_back(description);
                } else if (tagName == "@deprecated" || tagName == "\\deprecated") {
                    deprecated = description;
                } else {
                    // Other tags
                    if (tagName.length() > 1) {
                        std::string tagLabel = tagName.substr(1);
                        tagLabel[0] = toupper(tagLabel[0]);
                        extraDescription += "\n> **" + tagLabel + ":** " + description;
                    }
                }
            }
        }

        ts_parser_delete(parser);
        ts_tree_delete(tree);

        // Assemble markdown
        std::string md = "";

        // If a specific targetParam is requested, only output that parameter's docs
        if (!targetParam.empty()) {
            std::string prefix = "- **" + targetParam + "**";
            for (const std::string& p : params) {
                if (p.find(prefix) == 0) {
                    return "```angelscript\n" + targetParam + "\n```\n" + p.substr(prefix.length() + 1); // Extract description
                }
            }
            return "";
        }

        const auto& s = i18n::GetStrings(locale);

        if (!briefText.empty()) {
            md += briefText + "\n";
        }
        if (!extraDescription.empty()) {
            if (!md.empty()) md += "\n\n";
            md += extraDescription + "\n";
        }
        if (!deprecated.empty()) {
            if (!md.empty()) md += "\n\n";
            md += "> **" + std::string(s.hoverDeprecated) + ":** " + deprecated + "\n";
        }
        if (!tparams.empty()) {
            if (!md.empty()) md += "\n\n";
            md += "**" + std::string(s.hoverTemplateParams) + "**\n\n";
            for (const std::string& p : tparams) {
                md += p + "\n\n";
            }
            md.pop_back();
        }
        if (!params.empty()) {
            if (!md.empty()) md += "\n\n";
            md += "**" + std::string(s.hoverParams) + "**\n\n";
            for (const std::string& p : params) {
                md += p + "\n\n";
            }
            md.pop_back();
        }
        if (!returns.empty()) {
            if (!md.empty()) md += "\n\n";
            md += "**" + std::string(s.hoverReturns) + "**\n\n" + returns + "\n";
        }
        if (!notes.empty()) {
            for (const std::string& n : notes) {
                if (!md.empty()) md += "\n\n";
                md += "> **" + std::string(s.hoverNote) + ":** " + n;
            }
            md += "\n";
        }
        if (!warnings.empty()) {
            for (const std::string& w : warnings) {
                if (!md.empty()) md += "\n\n";
                md += "> **" + std::string(s.hoverWarning) + ":** " + w;
            }
            md += "\n";
        }

        // Clean trailing newlines
        while (!md.empty() && md.back() == '\n') md.pop_back();

        return md;
    }

}
}
