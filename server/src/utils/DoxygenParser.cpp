#include "DoxygenParser.h"
#include <tree_sitter/api.h>
#include <vector>
#include <sstream>
#include "features/hover/HoverInfo.h"

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

    static std::string TrimString(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\n\r");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\n\r");
        return str.substr(first, (last - first + 1));
    }


    void FillHoverInfoFromDoxygen(const std::string& rawDoxygen, features::HoverInfo& info, const std::string& targetParam) {
        if (rawDoxygen.empty()) return;

        std::string wrappedDoxygen = rawDoxygen;
        if (wrappedDoxygen.find("/**") == std::string::npos && wrappedDoxygen.find("/*!") == std::string::npos) {
            wrappedDoxygen = "/**\n" + wrappedDoxygen + "\n*/";
        }

        TSParser *parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_doxygen());
        TSTree *tree = ts_parser_parse_string(parser, NULL, wrappedDoxygen.c_str(), wrappedDoxygen.length());
        TSNode root = ts_tree_root_node(tree);

        // Iterate over top-level nodes in the document
        uint32_t childCount = ts_node_child_count(root);
        for (uint32_t i = 0; i < childCount; ++i) {
            TSNode child = ts_node_child(root, i);
            const char* nodeType = ts_node_type(child);

            if (strcmp(nodeType, "text") == 0 || strcmp(nodeType, "text_block") == 0) {
                if (targetParam.empty()) {
                    std::string txt = CleanText(GetNodeText(child, wrappedDoxygen));
                    if (!txt.empty() && txt != "/" && txt != "/**" && txt != "*/") {
                        if (!info.detailsText.empty()) info.detailsText += " ";
                        info.detailsText += txt;
                    }
                }
            }
            else if (strcmp(nodeType, "brief_header") == 0) {
                if (targetParam.empty()) {
                    TSNode textNode = ts_node_child_by_field_name(child, "text", 4);
                    if (ts_node_is_null(textNode)) {
                        // Try to find brief_description node manually
                        uint32_t bc = ts_node_child_count(child);
                        for (uint32_t j = 0; j < bc; ++j) {
                            TSNode sub = ts_node_child(child, j);
                            if (strcmp(ts_node_type(sub), "brief_description") == 0) {
                                info.briefText = CleanText(GetNodeText(sub, wrappedDoxygen));
                                break;
                            }
                        }
                    } else {
                        info.briefText = CleanText(GetNodeText(textNode, wrappedDoxygen));
                    }
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
                    if (targetParam.empty() && info.templateParameters) {
                        for (auto& p : *info.templateParameters) {
                            if (p.name == identifier) {
                                p.docDescription = description;
                                break;
                            }
                        }
                    }
                } else if (tagName == "@param" || tagName == "\\param") {
                    if (!targetParam.empty()) {
                        if (identifier == targetParam) {
                            info.briefText = description;
                        }
                    } else {
                        if (info.parameters) {
                            for (auto& p : *info.parameters) {
                                if (p.name == identifier) {
                                    p.docDescription = description;
                                    break;
                                }
                            }
                        }
                    }
                } else if (targetParam.empty()) {
                    if (tagName == "@return" || tagName == "\\return" || tagName == "@returns" || tagName == "\\returns") {
                        info.returnDoc = description;
                    } else if (tagName == "@brief" || tagName == "\\brief") {
                        info.briefText = description;
                    } else if (tagName == "@note" || tagName == "\\note") {
                        info.notes.push_back(description);
                    } else if (tagName == "@warning" || tagName == "\\warning") {
                        info.warnings.push_back(description);
                    } else if (tagName == "@deprecated" || tagName == "\\deprecated") {
                        info.deprecated = description;
                    } else if (tagName == "@details" || tagName == "\\details") {
                        info.detailsText = description;
                    } else {
                        // Other tags
                        if (tagName.length() > 1) {
                            std::string tagLabel = tagName.substr(1);
                            tagLabel[0] = toupper(tagLabel[0]);
                            if (!info.detailsText.empty()) info.detailsText += "\n";
                            info.detailsText += "> **" + tagLabel + ":** " + description;
                        }
                    }
                }
            }
        }

        std::string trimmedBrief = TrimString(info.briefText);
        std::string trimmedDetails = TrimString(info.detailsText);

        info.briefText = trimmedBrief;
        
        if (!trimmedDetails.empty() && trimmedDetails == trimmedBrief) {
            info.detailsText = "";
        } else {
            info.detailsText = trimmedDetails;
        }

        ts_parser_delete(parser);
        ts_tree_delete(tree);
    }

    std::string FormatDoxygenToMarkdown(const std::string& rawDoxygen, i18n::Locale locale, const std::string& targetParam) {
        if (rawDoxygen.empty()) return "";

        features::HoverInfo info;
        
        // Emulate the parameters structure for the legacy parser to fill
        info.parameters.emplace();
        info.templateParameters.emplace();
        
        // We need to parse first to know what params exist, or we can just extract from raw text
        // For the legacy wrapper to work without an actual Symbol, we'll let FillHoverInfoFromDoxygen
        // just parse text, but wait, FillHoverInfoFromDoxygen expects parameters to be pre-populated
        // with their names from the signature. If they aren't pre-populated, it won't add them.
        
        // Wait, for the legacy wrapper, it's used in tests.
        // Actually, if it's used in tests like `FormatDoxygenToMarkdown(source_code, ...)`,
        // it expects the output to have the markdown.
        // Let's modify FillHoverInfoFromDoxygen to append to parameters if they aren't found?
        // No, clangd doesn't do that.
        // Let's re-implement the legacy wrapper to just parse and build the Markdown manually
        // for tests, OR we can pre-populate parameters by doing a simple pass.
        
        // Since this is just for HoverTemplateTest.cpp, which tests FormatDoxygenToMarkdown,
        // let's do a simple pass over the AST to populate parameters before filling docs.
        std::string wrappedDoxygen = rawDoxygen;
        if (wrappedDoxygen.find("/**") == std::string::npos && wrappedDoxygen.find("/*!") == std::string::npos) {
            wrappedDoxygen = "/**\n" + wrappedDoxygen + "\n*/";
        }
        TSParser *parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_doxygen());
        TSTree *tree = ts_parser_parse_string(parser, NULL, wrappedDoxygen.c_str(), wrappedDoxygen.length());
        TSNode root = ts_tree_root_node(tree);
        
        uint32_t childCount = ts_node_child_count(root);
        for (uint32_t i = 0; i < childCount; ++i) {
            TSNode child = ts_node_child(root, i);
            if (strcmp(ts_node_type(child), "tag") == 0) {
                std::string tagName = "";
                std::string identifier = "";
                for (uint32_t j = 0; j < ts_node_child_count(child); ++j) {
                    TSNode sub = ts_node_child(child, j);
                    if (strcmp(ts_node_type(sub), "tag_name") == 0) tagName = CleanText(GetNodeText(sub, wrappedDoxygen));
                    else if (strcmp(ts_node_type(sub), "identifier") == 0) identifier = CleanText(GetNodeText(sub, wrappedDoxygen));
                }
                if (tagName == "@param" || tagName == "\\param") {
                    features::HoverParam p; p.name = identifier;
                    info.parameters->push_back(p);
                } else if (tagName == "@tparam" || tagName == "\\tparam") {
                    features::HoverParam p; p.name = identifier;
                    info.templateParameters->push_back(p);
                }
            }
        }
        ts_parser_delete(parser);
        ts_tree_delete(tree);

        FillHoverInfoFromDoxygen(rawDoxygen, info, targetParam);
        
        // If a specific targetParam is requested, only output that parameter's docs
        if (!targetParam.empty()) {
            for (const auto& p : *info.parameters) {
                if (p.name == targetParam) {
                    return "```angelscript\n" + targetParam + "\n```\n" + p.docDescription;
                }
            }
            return "";
        }
        
        // The legacy test expects just the doc output, not the code block.
        // We'll strip the heading and code block from ToMarkdown.
        info.kind = analysis::SymbolKind::Unknown; // Don't print heading
        auto sections = info.ToHoverSections(locale);
        std::string md = "";
        for (const auto& sec : sections) {
            if (!sec.isCodeBlock) {
                if (!md.empty()) md += "\n\n---\n\n";
                md += sec.content;
            }
        }
        
        return md;
    }

}
}
