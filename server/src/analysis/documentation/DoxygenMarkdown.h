#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Parses a raw Doxygen comment and renders clangd-canonical Markdown for hover and completion.
 *        Converts Doxygen markup to structured Markdown following clangd's compact presentation:
 * @param rawComment The raw comment string, including delimiters (`/\*\* ... *\/` or a `///` run).
 * @return Formatted Markdown string, or empty if the comment carries no documentation content.
 */
std::string RenderDoxygenMarkdown(const std::string& rawComment);

/**
 * @brief Extracted inline Doxygen command representation with formatting attributes.
 */
struct MatchedInlineCommand
{
    size_t cmdLen = 0;
    std::string_view cmd;
};

/**
 * @brief Classification of documentation comment text segments into plain text, tags, or verbatim code.
 */
enum class CommentSegmentKind
{
    Tag,
    Text,
    VerbatimCode
};

/**
 * @brief Segment of parsed Doxygen comment text with markup formatting style and line ranges.
 */
struct CommentSegment
{
    CommentSegmentKind kind = CommentSegmentKind::Text;
    std::vector<std::string> lines;
    std::string language;
};

/**
 * @brief Tracking state during Doxygen comment markdown segmentation and section extraction.
 */
struct SegmenterState
{
    CommentSegment currentSeg;
    bool hasActiveSeg = false;
    bool inCode = false;
    CommentSegment currentCode;
};

/**
 * @brief Documents a function parameter tag and its descriptive markdown payload extracted from Javadoc comments.
 */
struct ParamItem
{
    std::vector<std::string> names;
    std::string direction; // "in", "out", "in,out", or empty
    std::string description;
};

/**
 * @brief Documents a template parameter tag and its descriptive markdown payload extracted from Javadoc comments.
 */
struct TParamItem
{
    std::string name;
    std::string description;
};

/**
 * @brief Stores a return value documentation tag, qualifying value, and description extracted from Javadoc comments.
 */
struct RetValItem
{
    std::string value;
    std::string description;
};

/**
 * @brief Classification of documentation block elements extracted from Javadoc docstrings.
 */
enum class DocBlockKind
{
    Brief,
    Body,
    CodeBlock,
    TParam,
    Param,
    Return,
    RetVal,
    Admonition
};

/**
 * @brief Aggregates parsed Javadoc Doxygen sections, parameter descriptions, template tags, and return documentation.
 */
struct DocBlock
{
    DocBlockKind kind;
    std::string text;
    std::string label;
    TParamItem tparam;
    ParamItem param;
    RetValItem retval;

    /**
     * @brief Creates a documentation block with a specific kind and text content.
     * @param[in] kind Categorical kind of the documentation block.
     * @param[in] t Body text for the block.
     * @return Constructed DocBlock instance.
     */
    static DocBlock MakeWithKind(DocBlockKind kind, std::string t)
    {
        DocBlock b;
        b.kind = kind;
        b.text = std::move(t);
        return b;
    }

    static DocBlock MakeBrief(std::string t)
    {
        return MakeWithKind(DocBlockKind::Brief, std::move(t));
    }

    static DocBlock MakeBody(std::string t)
    {
        return MakeWithKind(DocBlockKind::Body, std::move(t));
    }

    static DocBlock MakeCodeBlock(std::string t)
    {
        return MakeWithKind(DocBlockKind::CodeBlock, std::move(t));
    }

    static DocBlock MakeTParam(TParamItem tp)
    {
        DocBlock b;
        b.kind = DocBlockKind::TParam;
        b.tparam = std::move(tp);
        return b;
    }

    static DocBlock MakeParam(ParamItem p)
    {
        DocBlock b;
        b.kind = DocBlockKind::Param;
        b.param = std::move(p);
        return b;
    }

    static DocBlock MakeReturn(std::string t)
    {
        return MakeWithKind(DocBlockKind::Return, std::move(t));
    }

    static DocBlock MakeRetVal(RetValItem r)
    {
        DocBlock b;
        b.kind = DocBlockKind::RetVal;
        b.retval = std::move(r);
        return b;
    }

    /**
     * @brief Creates an admonition documentation block with a label and text.
     * @param[in] l Admonition label (e.g. Note, Warning).
     * @param[in] t Content body of the admonition.
     * @return Constructed DocBlock configured as an admonition.
     */
    static DocBlock MakeAdmonition(std::string l, std::string t)
    {
        DocBlock b;
        b.kind = DocBlockKind::Admonition;
        b.label = std::move(l);
        b.text = std::move(t);
        return b;
    }
};
} // namespace angel_lsp::analysis
