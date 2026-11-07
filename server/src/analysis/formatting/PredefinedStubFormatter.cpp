#include "analysis/formatting/PredefinedStubFormatter.h"

#include "analysis/index/FormattingFacts.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::formatting
{

static std::string_view Slice(const std::string& source, uint32_t from, uint32_t to)
{
    if (from > to || to > source.size())
    {
        return {};
    }
    return std::string_view(source).substr(from, to - from);
}

/**
 * @brief Iterates over each line in text view, invoking consumer with the line slice and last flag.
 */
template <typename LineConsumer> static void ForEachLine(std::string_view text, LineConsumer&& consumer)
{
    size_t lineStart = 0;
    while (lineStart <= text.size())
    {
        size_t lineEnd = text.find('\n', lineStart);
        const bool last = (lineEnd == std::string_view::npos);
        if (last)
        {
            lineEnd = text.size();
        }

        consumer(text.substr(lineStart, lineEnd - lineStart), last);

        if (last)
        {
            break;
        }
        lineStart = lineEnd + 1;
    }
}

/**
 * @brief Drops trailing spaces and tabs from every line, leaving the line breaks alone.
 */
static std::string TrimTrailingSpaceOnEachLine(std::string_view text)
{
    std::string out;
    out.reserve(text.size());

    ForEachLine(text,
                [&](std::string_view line, bool last)
                {
                    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                    {
                        line.remove_suffix(1);
                    }
                    out.append(line);
                    if (!last)
                    {
                        out.push_back('\n');
                    }
                });

    return out;
}

/**
 * @brief Re-indents a block of text to one level, keeping its internal shape.
 *        Each non-empty line loses whatever leading whitespace it had and gains `indent`. A blank
 */
static std::string Reindent(std::string_view text, const std::string& indent)
{
    std::string out;
    out.reserve(text.size() + text.size() / 8);

    ForEachLine(text,
                [&](std::string_view line, [[maybe_unused]] bool last)
                {
                    while (!line.empty() && (line.back() == '\r'))
                    {
                        line.remove_suffix(1);
                    }
                    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                    {
                        line.remove_prefix(1);
                    }
                    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                    {
                        line.remove_suffix(1);
                    }

                    if (!line.empty())
                    {
                        out.append(indent);
                        out.append(line);
                    }
                    out.push_back('\n');
                });

    // The loop above ends every line, including one the source did not.
    if (!out.empty() && out.back() == '\n')
    {
        out.pop_back();
    }
    return out;
}

/**
 * @brief Whether a string is entirely whitespace.
 */
static bool IsBlank(std::string_view text)
{
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

/**
 * @brief Drops whitespace from both ends, keeping any blank line in the middle.
 *        The text between two declarations begins and ends with the line breaks that separated
 */
static std::string_view TrimBlankEdges(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    return text;
}

/**
 * @brief Adds one namespace declaration to the plan, merging it into the group with the same name.
 * @param[in] decl Pass-1 layout of the namespace; one without name or body is kept verbatim.
 * @param[in] lead Text between the previous declaration and this one.
 * @param[in] source Stub text the byte offsets point into.
 * @param[in,out] plan Plan receiving the slot or the new occurrence.
 */
static void ProcessNamespaceChild(const IndexedStubDeclaration& decl, std::string_view lead, const std::string& source,
                                  FormattingPlan& plan)
{
    if (!decl.hasNameAndBody)
    {
        plan.slots.push_back(
            Slot{false, std::string(lead) + std::string(Slice(source, decl.startByte, decl.endByte)), {}});
        return;
    }

    const std::string name(Slice(source, decl.nameStartByte, decl.nameEndByte));
    const uint32_t bodyStart = decl.bodyStartByte + 1;
    const uint32_t bodyEnd = decl.bodyEndByte - 1;

    auto [entry, inserted] = plan.groups.try_emplace(name, Group{name, {}, plan.slots.size()});
    if (inserted)
    {
        plan.slots.push_back(Slot{true, {}, name});
    }

    entry->second.occurrences.push_back(Occurrence{std::string(lead), std::string(Slice(source, bodyStart, bodyEnd))});
}

/**
 * @brief Builds the merge plan: every top-level declaration in order, with same-named namespaces grouped.
 * @param[in] declarations Pass-1 top-level declarations, comments excluded.
 * @param[in] source Stub text the byte offsets point into.
 * @return Plan of literal slots and namespace groups plus the trailing text.
 */
static FormattingPlan BuildFormattingPlan(const std::vector<IndexedStubDeclaration>& declarations,
                                          const std::string& source)
{
    FormattingPlan plan;
    uint32_t cursor = 0;

    for (const IndexedStubDeclaration& decl : declarations)
    {
        const std::string_view lead = Slice(source, cursor, decl.startByte);
        cursor = decl.endByte;

        if (!decl.isNamespace)
        {
            plan.slots.push_back(
                Slot{false, std::string(lead) + std::string(Slice(source, decl.startByte, decl.endByte)), {}});
            continue;
        }

        ProcessNamespaceChild(decl, lead, source, plan);
    }

    plan.tail = std::string(Slice(source, cursor, static_cast<uint32_t>(source.size())));
    return plan;
}

static void EmitGroup(const Group& group, const std::string& indent, std::string& out)
{
    const std::string& firstLead = group.occurrences.front().lead;
    out.append(TrimTrailingSpaceOnEachLine(firstLead));

    out.append("namespace ");
    out.append(group.name);
    out.push_back('\n');
    out.append("{\n");

    for (size_t i = 0; i < group.occurrences.size(); ++i)
    {
        const Occurrence& occurrence = group.occurrences[i];

        if (i > 0 && !IsBlank(occurrence.lead))
        {
            out.append(Reindent(TrimBlankEdges(occurrence.lead), indent));
            out.push_back('\n');
        }

        if (!IsBlank(occurrence.body))
        {
            out.append(Reindent(TrimBlankEdges(occurrence.body), indent));
            out.push_back('\n');
        }
    }

    out.append("}");
}

static std::string EmitFormattedStub(const FormattingPlan& plan, const std::string& indent, size_t initialCapacity)
{
    std::string out;
    out.reserve(initialCapacity);

    for (const Slot& slot : plan.slots)
    {
        if (!slot.isGroup)
        {
            out.append(slot.literal);
            continue;
        }

        const Group& group = plan.groups.at(slot.groupName);
        EmitGroup(group, indent, out);
    }

    out.append(plan.tail);
    return out;
}

std::string FormatPredefinedStub(const std::string& source, angel_lsp::parser::AngelScriptParser& parser,
                                 const std::string& indent)
{
    const std::optional<std::vector<IndexedStubDeclaration>> declarations = ExtractStubDeclarations(source, parser);
    if (!declarations)
    {
        return source;
    }

    FormattingPlan plan = BuildFormattingPlan(*declarations, source);

    std::string out = EmitFormattedStub(plan, indent, source.size());
    return out == source ? source : out;
}
} // namespace angel_lsp::analysis::formatting
