#include "analysis/documentation/DoxygenMarkdown.h"
#include "analysis/documentation/DoxygenMarkdownInternal.h"
#include "parser/DoxygenSyntax.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::doxygen
{
/**
 * @brief Renders a code_word inline node to Markdown.
 */
static std::string RenderCodeWordNode(parser::DoxygenNodeView node)
{
    parser::DoxygenNodeView codeChild = node.FindChild(parser::DoxygenNodeKind::Code);
    if (!codeChild.IsNull())
    {
        return "`" + std::string(codeChild.Text()) + "`";
    }
    std::string_view full = node.Text();
    if (full.starts_with("\\c"))
    {
        full.remove_prefix(2);
    }
    while (!full.empty() && (full.front() == ' ' || full.front() == '\t'))
    {
        full.remove_prefix(1);
    }
    return "`" + std::string(full) + "`";
}

/**
 * @brief Renders an emphasis inline node to Markdown.
 */
static std::string RenderEmphasisNode(parser::DoxygenNodeView node)
{
    parser::DoxygenNodeView textChild = node.FindChild(parser::DoxygenNodeKind::Text);
    if (!textChild.IsNull())
    {
        return "*" + std::string(textChild.Text()) + "*";
    }
    std::string_view full = node.Text();
    if (full.starts_with("\\a"))
    {
        full.remove_prefix(2);
    }
    while (!full.empty() && (full.front() == ' ' || full.front() == '\t'))
    {
        full.remove_prefix(1);
    }
    return "*" + std::string(full) + "*";
}

/**
 * @brief Renders a link inline node to Markdown.
 * @param[in] node First-pass Doxygen syntax node view.
 * @return Rendered Markdown link string.
 */
static std::string RenderLinkNode(parser::DoxygenNodeView node)
{
    std::string nodeText = std::string(node.Text());
    std::string linkText;
    parser::DoxygenNodeView textChild = node.FindChild(parser::DoxygenNodeKind::Text);
    if (!textChild.IsNull())
    {
        linkText = std::string(textChild.Text());
    }

    std::string url;
    size_t hrefPos = nodeText.find("href=");
    if (hrefPos != std::string::npos)
    {
        size_t quoteStart = hrefPos + 5;
        if (quoteStart < nodeText.size() && (nodeText[quoteStart] == '"' || nodeText[quoteStart] == '\''))
        {
            char q = nodeText[quoteStart];
            size_t quoteEnd = nodeText.find(q, quoteStart + 1);
            if (quoteEnd != std::string::npos)
            {
                url = nodeText.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            }
        }
    }
    if (!url.empty())
    {
        return !linkText.empty() ? "[" + linkText + "](" + url + ")" : "[" + url + "](" + url + ")";
    }
    return linkText.empty() ? nodeText : linkText;
}

/**
 * @brief Renders an inline syntax node (code_word, emphasis, link, function_link) to Markdown.
 */
static std::string RenderInlineNode(parser::DoxygenNodeView node)
{
    const parser::DoxygenNodeKind kind = node.Kind();
    if (kind == parser::DoxygenNodeKind::CodeWord)
    {
        return RenderCodeWordNode(node);
    }
    if (kind == parser::DoxygenNodeKind::Emphasis)
    {
        return RenderEmphasisNode(node);
    }
    if (kind == parser::DoxygenNodeKind::FunctionLink)
    {
        return "`" + std::string(node.Text()) + "`";
    }
    if (kind == parser::DoxygenNodeKind::Link)
    {
        return RenderLinkNode(node);
    }

    return std::string(node.Text());
}

/**
 * @brief Renders a description node by walking its byte range and substituting named children.
 */
static std::string RenderDescription(parser::DoxygenNodeView descNode)
{
    const std::string_view sourceCode = descNode.Source();
    uint32_t startByte = descNode.StartByte();
    uint32_t endByte = descNode.EndByte();
    if (startByte >= endByte || endByte > sourceCode.size())
    {
        return "";
    }

    std::string raw;
    uint32_t cursor = startByte;
    uint32_t childCount = descNode.NamedChildCount();

    for (uint32_t i = 0; i < childCount; ++i)
    {
        parser::DoxygenNodeView child = descNode.NamedChild(i);
        uint32_t childStart = child.StartByte();
        uint32_t childEnd = child.EndByte();

        if (childStart > cursor)
        {
            std::string verbatim = std::string(sourceCode.substr(cursor, childStart - cursor));
            raw.append(RewriteAtInlineCommands(verbatim));
        }

        raw.append(RenderInlineNode(child));
        cursor = std::max(cursor, childEnd);
    }

    if (cursor < endByte)
    {
        std::string verbatim = std::string(sourceCode.substr(cursor, endByte - cursor));
        raw.append(RewriteAtInlineCommands(verbatim));
    }

    return CleanDescriptionLines(raw);
}

/**
 * @brief Extracts the brief text from a brief_header node.
 */
static std::string ProcessBriefHeader(parser::DoxygenNodeView briefNode)
{
    const std::string_view sourceCode = briefNode.Source();
    parser::DoxygenNodeView descChild = briefNode.FindChild(parser::DoxygenNodeKind::BriefDescription);
    if (!descChild.IsNull())
    {
        return RenderDescription(descChild);
    }

    parser::DoxygenNodeView tagChild = briefNode.FindChild(parser::DoxygenNodeKind::TagName);
    uint32_t start = !tagChild.IsNull() ? tagChild.EndByte() : briefNode.StartByte();
    uint32_t end = briefNode.EndByte();
    if (end > start && end <= sourceCode.size())
    {
        return CleanDescriptionLines(RewriteAtInlineCommands(std::string(sourceCode.substr(start, end - start))));
    }

    return "";
}

/**
 * @brief Formats a code_block node into a Markdown fenced block.
 */
static std::string ProcessCodeBlock(parser::DoxygenNodeView blockNode)
{
    parser::DoxygenNodeView langNode = blockNode.FindChild(parser::DoxygenNodeKind::CodeBlockLanguage);
    std::string lang;
    if (!langNode.IsNull())
    {
        lang = ParseCodeLanguage(langNode.Text());
    }

    parser::DoxygenNodeView contentNode = blockNode.FindChild(parser::DoxygenNodeKind::CodeBlockContent);
    std::string content;
    if (!contentNode.IsNull())
    {
        content = std::string(contentNode.Text());
    }

    auto lines = SplitLines(content);
    for (auto& line : lines)
    {
        line = StripLineLeadingStar(line);
    }

    while (!lines.empty() && Trim(lines.front()).empty())
    {
        lines.erase(lines.begin());
    }
    while (!lines.empty() && Trim(lines.back()).empty())
    {
        lines.pop_back();
    }

    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i > 0)
        {
            joined += "\n";
        }
        joined += lines[i];
    }

    return "```" + lang + "\n" + joined + "\n```";
}

/**
 * @brief Normalizes storageclass bracketed parameter direction text.
 * @param[in] rawSc Raw storage class string view containing bracketed direction.
 * @return Normalized direction string ("in", "out", "in,out"), or empty string if invalid.
 */
static std::string NormalizeParamDirection(std::string_view rawSc)
{
    size_t open = rawSc.find('[');
    size_t close = rawSc.rfind(']');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
    {
        return "";
    }

    std::string inner = Trim(std::string(rawSc.substr(open + 1, close - open - 1)));
    std::string lowerInner = ToLower(inner);
    std::string compact;
    for (char c : lowerInner)
    {
        if (c != ' ' && c != '\t')
        {
            compact += c;
        }
    }
    if (compact == "inout" || compact == "in,out")
    {
        return "in,out";
    }
    if (compact == "in" || compact == "out")
    {
        return compact;
    }
    return compact;
}

/**
 * @brief Extracts parameter direction, names, and description from tag children.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @param[in,out] item Target ParamItem receiving extracted data.
 */
static void ProcessParamTagChildren(parser::DoxygenNodeView tagNode, ParamItem& item)
{
    const std::string_view sourceCode = tagNode.Source();
    uint32_t childCount = tagNode.ChildCount();
    parser::DoxygenNodeView lastIdNode;
    for (uint32_t i = 0; i < childCount; ++i)
    {
        parser::DoxygenNodeView child = tagNode.Child(i);
        const parser::DoxygenNodeKind kind = child.Kind();
        if (kind == parser::DoxygenNodeKind::StorageClass)
        {
            item.direction = NormalizeParamDirection(child.Text());
        }
        else if (kind == parser::DoxygenNodeKind::Identifier || kind == parser::DoxygenNodeKind::QualifiedIdentifier)
        {
            std::string name = Trim(std::string(child.Text()));
            if (!name.empty())
            {
                item.names.push_back(std::move(name));
                lastIdNode = child;
            }
        }
        else if (kind == parser::DoxygenNodeKind::Description)
        {
            item.description = RenderDescription(child);
        }
    }

    if (!lastIdNode.IsNull())
    {
        const uint32_t start = lastIdNode.EndByte();
        size_t end = sourceCode.rfind("*/");
        if (end == std::string_view::npos || end < start)
        {
            end = tagNode.EndByte();
        }
        if (end > start && end <= sourceCode.size())
        {
            std::string rawSlice =
                CleanDescriptionLines(RewriteAtInlineCommands(std::string(sourceCode.substr(start, end - start))));
            rawSlice = Trim(rawSlice);
            if (!rawSlice.empty())
            {
                item.description = std::move(rawSlice);
            }
        }
    }
}

/**
 * @brief Extracts parameter names, optional direction, and description from a @param tag.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @return Structured ParamItem populated from the tag node.
 */
static ParamItem ProcessParamTag(parser::DoxygenNodeView tagNode)
{
    ParamItem item;
    ProcessParamTagChildren(tagNode, item);

    if (item.names.empty() && !item.description.empty())
    {
        size_t sp = item.description.find_first_of(" \t");
        if (sp != std::string::npos)
        {
            item.names.push_back(item.description.substr(0, sp));
            item.description = Trim(item.description.substr(sp + 1));
        }
        else
        {
            item.names.push_back(item.description);
            item.description = "";
        }
    }

    return item;
}

/**
 * @brief Parses a two-part Doxygen tag (such as @tparam or @retval) into a key and description pair.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @return Pair of {key token, trailing description}.
 */
static std::pair<std::string, std::string> ExtractTwoPartTag(parser::DoxygenNodeView tagNode)
{
    const std::string_view sourceCode = tagNode.Source();
    parser::DoxygenNodeView nameChild = tagNode.FindChild(parser::DoxygenNodeKind::TagName);
    uint32_t start = !nameChild.IsNull() ? nameChild.EndByte() : tagNode.StartByte();
    size_t end = sourceCode.rfind("*/");
    if (end == std::string_view::npos || end < start)
    {
        end = tagNode.EndByte();
    }
    std::string desc;
    if (end > start && end <= sourceCode.size())
    {
        desc = CleanDescriptionLines(RewriteAtInlineCommands(std::string(sourceCode.substr(start, end - start))));
    }
    else
    {
        parser::DoxygenNodeView descChild = tagNode.FindChild(parser::DoxygenNodeKind::Description);
        if (!descChild.IsNull())
        {
            desc = RenderDescription(descChild);
        }
    }

    desc = Trim(desc);
    size_t sp = desc.find_first_of(" \t");
    if (sp != std::string::npos)
    {
        return {desc.substr(0, sp), Trim(desc.substr(sp + 1))};
    }
    return {desc, std::string()};
}

/**
 * @brief Splits the template parameter name from the description for a @tparam tag.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @return Structured TParamItem populated from the tag node.
 */
static TParamItem ProcessTParamTag(parser::DoxygenNodeView tagNode)
{
    auto [name, desc] = ExtractTwoPartTag(tagNode);
    return TParamItem{std::move(name), std::move(desc)};
}

/**
 * @brief Extracts the description string for return tags.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @return Extracted return description text.
 */
static std::string ProcessReturnTag(parser::DoxygenNodeView tagNode)
{
    parser::DoxygenNodeView descChild = tagNode.FindChild(parser::DoxygenNodeKind::Description);
    if (!descChild.IsNull())
    {
        return RenderDescription(descChild);
    }
    return "";
}

/**
 * @brief Extracts description text from an admonition tag node.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @return Extracted admonition description text.
 */
static std::string ExtractAdmonitionDescription(parser::DoxygenNodeView tagNode)
{
    const std::string_view sourceCode = tagNode.Source();
    parser::DoxygenNodeView descChild = tagNode.FindChild(parser::DoxygenNodeKind::Description);
    if (!descChild.IsNull())
    {
        return RenderDescription(descChild);
    }

    std::vector<std::string> parts;
    uint32_t count = tagNode.ChildCount();
    for (uint32_t i = 0; i < count; ++i)
    {
        parser::DoxygenNodeView c = tagNode.Child(i);
        const parser::DoxygenNodeKind kind = c.Kind();
        if (kind == parser::DoxygenNodeKind::Identifier || kind == parser::DoxygenNodeKind::FunctionLink ||
            kind == parser::DoxygenNodeKind::Function)
        {
            parts.push_back(std::string(c.Text()));
        }
    }
    if (!parts.empty())
    {
        std::string desc;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
            {
                desc += ", ";
            }
            desc += parts[i];
        }
        return desc;
    }

    parser::DoxygenNodeView nameChild = tagNode.FindChild(parser::DoxygenNodeKind::TagName);
    uint32_t start = !nameChild.IsNull() ? nameChild.EndByte() : tagNode.StartByte();
    uint32_t end = tagNode.EndByte();
    if (end > start && end <= sourceCode.size())
    {
        return CleanDescriptionLines(RewriteAtInlineCommands(std::string(sourceCode.substr(start, end - start))));
    }
    return "";
}

/**
 * @brief Maps a cleaned Doxygen admonition tag name to its canonical Markdown label.
 * @param[in] cleanedTag Cleaned lowercase tag name without prefix.
 * @return Canonical title label for the admonition block.
 */
static std::string MapAdmonitionLabel(std::string_view cleanedTag)
{
    static constexpr std::pair<std::string_view, std::string_view> kKnownLabels[] = {
        {"note", "Note"},
        {"warning", "Warning"},
        {"attention", "Attention"},
        {"caution", "Caution"},
        {"deprecated", "Deprecated"},
        {"see", "See also"},
        {"sa", "See also"},
        {"remark", "Remark"},
        {"remarks", "Remark"},
        {"since", "Since"},
        {"todo", "Todo"},
        {"bug", "Bug"},
        {"pre", "Pre"},
        {"post", "Post"},
        {"throw", "Throws"},
        {"throws", "Throws"},
        {"exception", "Throws"},
    };

    std::string lower = ToLower(std::string(cleanedTag));
    for (const auto& [tag, label] : kKnownLabels)
    {
        if (lower == tag)
        {
            return std::string(label);
        }
    }

    char first = static_cast<char>(std::toupper(static_cast<unsigned char>(cleanedTag[0])));
    return first + std::string(cleanedTag.substr(1));
}

/**
 * @brief Maps tag name and description to an Admonition block.
 * @param[in] cleanedTag Cleaned tag name string.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @return DocBlock configured as an admonition.
 */
static DocBlock ProcessAdmonitionTag(std::string_view cleanedTag, parser::DoxygenNodeView tagNode)
{
    std::string desc = Trim(ExtractAdmonitionDescription(tagNode));
    std::string label = MapAdmonitionLabel(cleanedTag);
    return DocBlock::MakeAdmonition(std::move(label), std::move(desc));
}

/**
 * @brief Extracts value and description from a @retval tag.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @return Structured RetValItem populated from the tag node.
 */
static RetValItem ProcessRetValTag(parser::DoxygenNodeView tagNode)
{
    auto [value, desc] = ExtractTwoPartTag(tagNode);
    return RetValItem{std::move(value), std::move(desc)};
}

/**
 * @brief Checks whether a Doxygen tag is a structural declaration command.
 * @param[in] tag Tag name string view to inspect.
 * @return True if the tag is a structural entity declaration tag; false otherwise.
 */
static bool IsStructuralTag(std::string_view tag)
{
    static const char* const kStructuralTags[] = {
        "class",     "struct",     "union",     "enum",     "typedef",  "typealias",  "interface",
        "fn",        "function",   "method",    "property", "var",      "variable",   "overload",
        "file",      "headerfile", "namespace", "package",  "defgroup", "addtogroup", "ingroup",
        "weakgroup", "endgroup",   "name",      "def",      "define"};
    for (const char* st : kStructuralTags)
    {
        if (tag == st)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Extracts documentation lines from multi-line structural tags.
 * @param[in] lines Vector of text lines within the structural tag.
 * @return Cleaned single-string description.
 */
static std::string ExtractMultiLineStructuralDescription(const std::vector<std::string>& lines)
{
    std::string subDesc;
    for (size_t i = 1; i < lines.size(); ++i)
    {
        std::string l = Trim(lines[i]);
        if (!l.empty())
        {
            if (!subDesc.empty())
            {
                subDesc += " ";
            }
            subDesc += l;
        }
    }
    return CleanDescriptionLines(subDesc);
}

/**
 * @brief Extracts descriptive text after single-line structural entity identifier.
 * @param[in] raw Raw line string view containing the structural command.
 * @return Description text following the entity identifier, or empty if none.
 */
static std::string ExtractSingleLineStructuralDescription(std::string_view raw)
{
    size_t sp = raw.find_first_of(" \t");
    if (sp != std::string_view::npos)
    {
        std::string remainder = Trim(std::string(raw.substr(sp + 1)));
        if (!remainder.empty() && !remainder.ends_with(')') && !remainder.ends_with(';'))
        {
            return remainder;
        }
    }
    return "";
}

/**
 * @brief Extracts attached body description following a structural tag, if any.
 * @param[in] tagNode First-pass Doxygen syntax node view.
 * @return Description text extracted from the tag node.
 */
static std::string ProcessStructuralTagDescription(parser::DoxygenNodeView tagNode)
{
    const std::string_view sourceCode = tagNode.Source();
    parser::DoxygenNodeView descChild = tagNode.FindChild(parser::DoxygenNodeKind::Description);
    std::string raw;
    if (!descChild.IsNull())
    {
        raw = RenderDescription(descChild);
    }
    else
    {
        parser::DoxygenNodeView nameChild = tagNode.FindChild(parser::DoxygenNodeKind::TagName);
        uint32_t start = !nameChild.IsNull() ? nameChild.EndByte() : tagNode.StartByte();
        uint32_t end = tagNode.EndByte();
        if (end > start && end <= sourceCode.size())
        {
            raw = CleanDescriptionLines(RewriteAtInlineCommands(std::string(sourceCode.substr(start, end - start))));
        }
    }

    raw = Trim(raw);
    if (raw.empty())
    {
        return "";
    }

    parser::DoxygenNodeView typeChild = tagNode.FindChild(parser::DoxygenNodeKind::Type);
    if (!typeChild.IsNull())
    {
        return !descChild.IsNull() ? raw : "";
    }

    auto lines = SplitLines(raw);
    if (lines.size() > 1)
    {
        return ExtractMultiLineStructuralDescription(lines);
    }

    return ExtractSingleLineStructuralDescription(raw);
}

/**
 * @brief Appends text to a string block with proper spacing.
 * @param[in,out] target String being appended to.
 * @param[in] addition Content to append.
 */
static void AppendToBlockText(std::string& target, std::string_view addition)
{
    if (addition.empty())
    {
        return;
    }
    if (target.empty())
    {
        target = addition;
        return;
    }
    if (!target.ends_with(' ') && !target.ends_with('\t') && !target.ends_with('(') && !target.ends_with('[') &&
        !target.ends_with('{'))
    {
        target += ' ';
    }
    target += addition;
}

/**
 * @brief Appends text to a DocBlock target.
 * @param[in,out] block Target documentation block.
 * @param[in] text Text chunk to append.
 */
static void AppendToDocBlock(DocBlock& block, std::string_view text)
{
    switch (block.kind)
    {
    case DocBlockKind::Brief:
    case DocBlockKind::Body:
    case DocBlockKind::Return:
    case DocBlockKind::Admonition:
        AppendToBlockText(block.text, text);
        break;
    case DocBlockKind::TParam:
        AppendToBlockText(block.tparam.description, text);
        break;
    case DocBlockKind::Param:
        AppendToBlockText(block.param.description, text);
        break;
    case DocBlockKind::RetVal:
        AppendToBlockText(block.retval.description, text);
        break;
    case DocBlockKind::CodeBlock:
        break;
    }
}

/**
 * @brief Splits an inline tag description into target word and trailing remainder.
 * @param[in] desc Tag description view.
 * @return Pair of target identifier and remaining descriptive text.
 */
static std::pair<std::string, std::string> SplitInlineTagWord(std::string_view desc)
{
    std::string trimmedDesc = TrimLeading(std::string(desc));
    std::string X;
    std::string remainder;
    size_t sp = trimmedDesc.find_first_of(" \t");
    if (sp != std::string::npos)
    {
        X = trimmedDesc.substr(0, sp);
        remainder = trimmedDesc.substr(sp);
    }
    else
    {
        X = trimmedDesc;
    }

    size_t punct = X.size();
    while (punct > 0 && IsTrailingPunctuation(X[punct - 1]))
    {
        --punct;
    }
    if (punct > 0 && punct < X.size())
    {
        remainder = X.substr(punct) + remainder;
        X.resize(punct);
    }
    return {std::move(X), std::move(remainder)};
}

/**
 * @brief Formats an inline word into Markdown depending on tag command.
 */
static std::string FormatInlineWord(std::string_view cmd, std::string_view word)
{
    if (cmd == "c" || cmd == "p" || cmd == "ref")
    {
        return "`" + std::string(word) + "`";
    }
    if (cmd == "b")
    {
        return "**" + std::string(word) + "**";
    }
    if (cmd == "a" || cmd == "e" || cmd == "em")
    {
        return "*" + std::string(word) + "*";
    }
    return std::string(word);
}

/**
 * @brief Merges an inline formatting tag (\b, \e, \em, \a, \c, \p, \ref) into the preceding block.
 */
static void MergeInlineTag(std::string_view cmd, parser::DoxygenNodeView tagNode, std::vector<DocBlock>& blocks)
{
    const std::string_view sourceCode = tagNode.Source();
    parser::DoxygenNodeView descChild = tagNode.FindChild(parser::DoxygenNodeKind::Description);
    std::string desc;
    if (!descChild.IsNull())
    {
        desc = RenderDescription(descChild);
    }
    else
    {
        parser::DoxygenNodeView nameChild = tagNode.FindChild(parser::DoxygenNodeKind::TagName);
        uint32_t start = !nameChild.IsNull() ? nameChild.EndByte() : tagNode.StartByte();
        uint32_t end = tagNode.EndByte();
        if (end > start && end <= sourceCode.size())
        {
            desc = CleanDescriptionLines(RewriteAtInlineCommands(std::string(sourceCode.substr(start, end - start))));
        }
    }

    auto [word, remainder] = SplitInlineTagWord(desc);
    std::string inlineRendered = FormatInlineWord(cmd, word) + remainder;

    if (blocks.empty() || blocks.back().kind == DocBlockKind::CodeBlock)
    {
        blocks.push_back(DocBlock::MakeBody(std::move(inlineRendered)));
    }
    else
    {
        AppendToDocBlock(blocks.back(), inlineRendered);
    }
}

/**
 * @brief Extracts raw tag name from a tag AST node.
 * @param[in] child First-pass Doxygen syntax node view.
 * @return Extracted raw tag identifier string.
 */
static std::string ExtractRawTagName(parser::DoxygenNodeView child)
{
    parser::DoxygenNodeView nameNode = child.FindChild(parser::DoxygenNodeKind::TagName);
    std::string rawTagName;
    if (!nameNode.IsNull())
    {
        rawTagName = std::string(nameNode.Text());
    }
    else
    {
        parser::DoxygenNodeView scNode = child.FindChild(parser::DoxygenNodeKind::StorageClass);
        if (!scNode.IsNull())
        {
            rawTagName = "param";
        }
    }
    if (rawTagName.starts_with('@') || rawTagName.starts_with('\\'))
    {
        rawTagName = rawTagName.substr(1);
    }
    return rawTagName;
}

/**
 * @brief Tries to dispatch common inline, parameter, return, or retval tags. Returns true if handled.
 * @param[in] lowerTag Normalized lowercase tag identifier.
 * @param[in] child First-pass Doxygen syntax node view.
 * @param[in,out] blocks Accumulated documentation blocks.
 * @return True if tag was recognized and dispatched; false otherwise.
 */
static bool TryDispatchCommonTag(std::string_view lowerTag, parser::DoxygenNodeView child,
                                 std::vector<DocBlock>& blocks)
{
    if (lowerTag == "b" || lowerTag == "e" || lowerTag == "em" || lowerTag == "a" || lowerTag == "c" ||
        lowerTag == "p" || lowerTag == "ref")
    {
        MergeInlineTag(lowerTag, child, blocks);
        return true;
    }
    if (lowerTag == "param")
    {
        blocks.push_back(DocBlock::MakeParam(ProcessParamTag(child)));
        return true;
    }
    if (lowerTag == "tparam")
    {
        blocks.push_back(DocBlock::MakeTParam(ProcessTParamTag(child)));
        return true;
    }
    if (lowerTag == "return" || lowerTag == "returns" || lowerTag == "result")
    {
        blocks.push_back(DocBlock::MakeReturn(ProcessReturnTag(child)));
        return true;
    }
    if (lowerTag == "retval")
    {
        blocks.push_back(DocBlock::MakeRetVal(ProcessRetValTag(child)));
        return true;
    }
    return false;
}

/**
 * @brief Dispatches tag AST nodes to corresponding block builders.
 * @param[in] child First-pass Doxygen syntax node view.
 * @param[in,out] blocks Destination documentation blocks.
 */
static void DispatchSegmentTagNode(parser::DoxygenNodeView child, std::vector<DocBlock>& blocks)
{
    std::string rawTagName = ExtractRawTagName(child);
    std::string lowerTag = ToLower(rawTagName);

    if (TryDispatchCommonTag(lowerTag, child, blocks))
    {
        return;
    }
    if (lowerTag == "brief" || lowerTag == "details")
    {
        std::string brief = ProcessReturnTag(child);
        if (!brief.empty())
        {
            blocks.push_back(DocBlock::MakeBody(std::move(brief)));
        }
    }
    else if (IsStructuralTag(lowerTag))
    {
        std::string desc = ProcessStructuralTagDescription(child);
        if (!desc.empty())
        {
            blocks.push_back(DocBlock::MakeBody(std::move(desc)));
        }
    }
    else if (lowerTag == "link")
    {
        std::string desc = Trim(ExtractAdmonitionDescription(child));
        if (desc.ends_with("@endlink"))
        {
            desc = Trim(desc.substr(0, desc.size() - 8));
        }
        else if (desc.ends_with("\\endlink"))
        {
            desc = Trim(desc.substr(0, desc.size() - 8));
        }
        blocks.push_back(DocBlock::MakeBody(desc.empty() ? "@link" : "@link " + desc));
    }
    else if (lowerTag == "endlink")
    {
        return;
    }
    else if (!rawTagName.empty())
    {
        blocks.push_back(ProcessAdmonitionTag(rawTagName, child));
    }
}

/**
 * @brief Appends or merges rendered description AST node into doc blocks.
 * @param[in] child First-pass Doxygen syntax node view.
 * @param[in,out] blocks Destination documentation blocks.
 */
static void DispatchDescriptionChildNode(parser::DoxygenNodeView child, std::vector<DocBlock>& blocks)
{
    std::string body = RenderDescription(child);
    if (body.empty() || !HasAlphanumeric(body))
    {
        return;
    }
    if (!blocks.empty() && blocks.back().kind == DocBlockKind::Body)
    {
        bool keepNewline = ShouldKeepDescriptionNewline(blocks.back().text, body);
        blocks.back().text += (keepNewline ? "\n" : " ") + body;
    }
    else
    {
        blocks.push_back(DocBlock::MakeBody(std::move(body)));
    }
}

/**
 * @brief Appends raw text line child node into doc blocks.
 * @param[in] child First-pass Doxygen syntax node view.
 * @param[in,out] blocks Destination documentation blocks.
 */
static void DispatchTextLineChildNode(parser::DoxygenNodeView child, std::vector<DocBlock>& blocks)
{
    std::string line = std::string(child.Text());
    line = CleanDescriptionLines(RewriteAtInlineCommands(line));
    if (line.empty() || !HasAlphanumeric(line))
    {
        return;
    }
    if (!blocks.empty() && blocks.back().kind == DocBlockKind::Body)
    {
        AppendToBlockText(blocks.back().text, line);
    }
    else
    {
        blocks.push_back(DocBlock::MakeBody(std::move(line)));
    }
}

/**
 * @brief Dispatches a child AST node of a parsed segment.
 * @param[in] child First-pass Doxygen syntax node view.
 * @param[in,out] blocks Destination documentation blocks.
 */
static void DispatchSegmentChildNode(parser::DoxygenNodeView child, std::vector<DocBlock>& blocks)
{
    const parser::DoxygenNodeKind kind = child.Kind();
    if (kind == parser::DoxygenNodeKind::BriefHeader)
    {
        std::string brief = ProcessBriefHeader(child);
        if (!brief.empty())
        {
            blocks.push_back(DocBlock::MakeBody(std::move(brief)));
        }
    }
    else if (kind == parser::DoxygenNodeKind::Description)
    {
        DispatchDescriptionChildNode(child, blocks);
    }
    else if (kind == parser::DoxygenNodeKind::CodeBlock)
    {
        std::string code = ProcessCodeBlock(child);
        if (!code.empty())
        {
            blocks.push_back(DocBlock::MakeCodeBlock(std::move(code)));
        }
    }
    else if (kind == parser::DoxygenNodeKind::Tag)
    {
        DispatchSegmentTagNode(child, blocks);
    }
    else if (kind == parser::DoxygenNodeKind::TextLine)
    {
        DispatchTextLineChildNode(child, blocks);
    }
}

void ParseDocSegment(std::string_view syntheticDoc, std::vector<DocBlock>& blocks)
{
    if (Trim(syntheticDoc).empty())
    {
        return;
    }

    const parser::DoxygenSyntaxFacts facts = parser::ExtractDoxygenSyntax(syntheticDoc);
    const parser::DoxygenNodeView root = parser::DoxygenNodeView::Root(facts, syntheticDoc);
    if (root.IsNull())
    {
        return;
    }

    uint32_t childCount = root.ChildCount();
    for (uint32_t i = 0; i < childCount; ++i)
    {
        DispatchSegmentChildNode(root.Child(i), blocks);
    }
}
} // namespace angel_lsp::analysis::doxygen
