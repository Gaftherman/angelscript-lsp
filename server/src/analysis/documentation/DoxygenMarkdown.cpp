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
 * @brief Assembles brief and general body paragraphs/code blocks into output sections.
 * @param[in] briefText Summary brief text.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleBriefAndBody(std::string briefText, const std::vector<DocBlock>& blocks,
                                 std::vector<std::string>& sections)
{
    size_t firstBriefBlockIdx = std::string::npos;
    if (!briefText.empty() && HasAlphanumeric(briefText))
    {
        sections.push_back(std::move(briefText));
    }
    else
    {
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            if (blocks[i].kind == DocBlockKind::Brief && !blocks[i].text.empty() && HasAlphanumeric(blocks[i].text))
            {
                sections.push_back(blocks[i].text);
                firstBriefBlockIdx = i;
                break;
            }
        }
    }

    for (size_t i = 0; i < blocks.size(); ++i)
    {
        if (i == firstBriefBlockIdx)
        {
            continue;
        }
        const auto& b = blocks[i];
        if ((b.kind == DocBlockKind::Body || b.kind == DocBlockKind::Brief) && !b.text.empty() &&
            HasAlphanumeric(b.text))
        {
            sections.push_back(b.text);
        }
        else if (b.kind == DocBlockKind::CodeBlock && !b.text.empty())
        {
            sections.push_back(b.text);
        }
    }
}

/**
 * @brief Assembles template parameter bullets into output sections.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleTParamBullets(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
{
    std::vector<std::string> tparamBullets;
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::TParam)
        {
            std::string bullet = "* `" + b.tparam.name + "`";
            if (!b.tparam.description.empty())
            {
                bullet += ": " + b.tparam.description;
            }
            tparamBullets.push_back(std::move(bullet));
        }
    }
    if (!tparamBullets.empty())
    {
        std::string joined;
        for (size_t i = 0; i < tparamBullets.size(); ++i)
        {
            if (i > 0)
            {
                joined += "\n";
            }
            joined += tparamBullets[i];
        }
        sections.push_back(std::move(joined));
    }
}

/**
 * @brief Assembles parameter bullets into output sections.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleParamBullets(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
{
    std::vector<std::string> paramBullets;
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::Param)
        {
            for (const auto& name : b.param.names)
            {
                std::string bullet = "* `" + name + "`";
                if (!b.param.direction.empty())
                {
                    bullet += " *(" + b.param.direction + ")*";
                }
                if (!b.param.description.empty())
                {
                    bullet += ": " + b.param.description;
                }
                paramBullets.push_back(std::move(bullet));
            }
        }
    }
    if (!paramBullets.empty())
    {
        std::string joined;
        for (size_t i = 0; i < paramBullets.size(); ++i)
        {
            if (i > 0)
            {
                joined += "\n";
            }
            joined += paramBullets[i];
        }
        sections.push_back(std::move(joined));
    }
}

/**
 * @brief Assembles return and retval sections into output sections.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleReturnSection(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
{
    std::string returnSection;
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::Return)
        {
            returnSection = "**Returns:**";
            if (!b.text.empty())
            {
                returnSection += " " + b.text;
            }
            break;
        }
    }

    std::vector<std::string> retvalBullets;
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::RetVal)
        {
            std::string bullet = "* `" + b.retval.value + "`";
            if (!b.retval.description.empty())
            {
                bullet += ": " + b.retval.description;
            }
            retvalBullets.push_back(std::move(bullet));
        }
    }
    if (!retvalBullets.empty())
    {
        if (returnSection.empty())
        {
            returnSection = "**Returns:**";
        }
        for (const auto& bullet : retvalBullets)
        {
            returnSection += "\n" + bullet;
        }
    }
    if (!returnSection.empty())
    {
        sections.push_back(std::move(returnSection));
    }
}

/**
 * @brief Assembles admonitions in source order into output sections.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleAdmonitions(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
{
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::Admonition)
        {
            std::string adm = "> **" + b.label + ":**";
            if (!b.text.empty())
            {
                adm += " " + b.text;
            }
            sections.push_back(std::move(adm));
        }
    }
}

/**
 * @brief Assembles extracted blocks and brief into canonical clangd Markdown format.
 */
static std::string AssembleMarkdown(std::string briefText, const std::vector<DocBlock>& blocks)
{
    std::vector<std::string> outputSections;
    AssembleBriefAndBody(std::move(briefText), blocks, outputSections);
    AssembleTParamBullets(blocks, outputSections);
    AssembleParamBullets(blocks, outputSections);
    AssembleReturnSection(blocks, outputSections);
    AssembleAdmonitions(blocks, outputSections);

    std::string finalMarkdown;
    for (size_t i = 0; i < outputSections.size(); ++i)
    {
        if (i > 0)
        {
            finalMarkdown += "\n\n";
        }
        finalMarkdown += outputSections[i];
    }

    return Trim(finalMarkdown);
}

/**
 * @brief Processes a verbatim code comment segment and appends a code block.
 * @param[in,out] seg Code comment segment.
 * @param[in,out] allBlocks Output documentation blocks.
 */
static void ProcessVerbatimCodeSegment(CommentSegment& seg, std::vector<DocBlock>& allBlocks)
{
    while (!seg.lines.empty() && Trim(seg.lines.front()).empty())
    {
        seg.lines.erase(seg.lines.begin());
    }
    while (!seg.lines.empty() && Trim(seg.lines.back()).empty())
    {
        seg.lines.pop_back();
    }

    std::string joined;
    for (size_t i = 0; i < seg.lines.size(); ++i)
    {
        if (i > 0)
        {
            joined += "\n";
        }
        joined += seg.lines[i];
    }

    std::string fenced = "```" + seg.language + "\n";
    if (!joined.empty())
    {
        fenced += joined + "\n";
    }
    fenced += "```";
    allBlocks.push_back(DocBlock::MakeCodeBlock(std::move(fenced)));
}

/**
 * @brief Processes a parsed doc segment and handles fallback text segments.
 * @param[in] seg Parsed comment segment.
 * @param[in,out] allBlocks Output documentation blocks.
 */
static void ProcessParsedDocSegment(const CommentSegment& seg, std::vector<DocBlock>& allBlocks)
{
    if (seg.lines.empty())
    {
        return;
    }
    size_t beforeCount = allBlocks.size();
    std::string synthetic = WrapInSyntheticComment(seg.lines);
    ParseDocSegment(synthetic, allBlocks);

    // Fallback for tree-sitter-doxygen grammar limitations (e.g. grammar rejects '!' in text):
    if (seg.kind == CommentSegmentKind::Text && allBlocks.size() == beforeCount)
    {
        std::string rawJoined;
        for (size_t i = 0; i < seg.lines.size(); ++i)
        {
            if (i > 0)
            {
                rawJoined += "\n";
            }
            rawJoined += seg.lines[i];
        }
        std::string cleaned = CleanDescriptionLines(RewriteAtInlineCommands(rawJoined));
        if (!cleaned.empty())
        {
            allBlocks.push_back(DocBlock::MakeBody(std::move(cleaned)));
        }
    }
}
} // namespace angel_lsp::analysis::doxygen

namespace angel_lsp::analysis
{
std::string RenderDoxygenMarkdown(std::string_view rawComment)
{
    auto contentLines = doxygen::ExtractCommentLines(rawComment);
    if (contentLines.empty() || std::all_of(contentLines.begin(), contentLines.end(),
                                            [](std::string_view l) { return doxygen::Trim(l).empty(); }))
    {
        return "";
    }

    std::string briefText = doxygen::ExtractBrief(contentLines);
    auto segments = doxygen::SegmentCommentLines(contentLines);
    std::vector<DocBlock> allBlocks;

    for (auto& seg : segments)
    {
        if (seg.kind == CommentSegmentKind::VerbatimCode)
        {
            doxygen::ProcessVerbatimCodeSegment(seg, allBlocks);
        }
        else
        {
            doxygen::ProcessParsedDocSegment(seg, allBlocks);
        }
    }

    return doxygen::AssembleMarkdown(std::move(briefText), allBlocks);
}
} // namespace angel_lsp::analysis
