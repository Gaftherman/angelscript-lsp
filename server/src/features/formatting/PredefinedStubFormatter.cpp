#include "features/formatting/PredefinedStubFormatter.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features::formatting
{
    namespace
    {
        /** @brief One namespace declaration found at the top level, in source order. */
        struct Occurrence
        {
            /// Text between the previous top-level declaration and this one: its comments, and any
            /// blank lines. Carried across so a comment never parts company with what it describes.
            std::string lead;

            /// Everything between the body's braces, exactly as written.
            std::string body;
        };

        struct Group
        {
            std::string name;
            std::vector<Occurrence> occurrences;

            /// Index into the output plan, so a merged namespace lands where it was first written
            /// rather than being sorted somewhere new.
            size_t firstSlot = 0;
        };

        std::string_view Slice(const std::string &source, uint32_t from, uint32_t to)
        {
            if (from > to || to > source.size())
            {
                return {};
            }
            return std::string_view(source).substr(from, to - from);
        }

        /** @brief Drops trailing spaces and tabs from every line, leaving the line breaks alone. */
        std::string TrimTrailingSpaceOnEachLine(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());

            size_t lineStart = 0;
            while (lineStart <= text.size())
            {
                size_t lineEnd = text.find('\n', lineStart);
                const bool last = lineEnd == std::string_view::npos;
                if (last)
                {
                    lineEnd = text.size();
                }

                std::string_view line = text.substr(lineStart, lineEnd - lineStart);
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                {
                    line.remove_suffix(1);
                }

                out.append(line);
                if (last)
                {
                    break;
                }
                out.push_back('\n');
                lineStart = lineEnd + 1;
            }

            return out;
        }

        /**
         * @brief Re-indents a block of text to one level, keeping its internal shape.
         *
         * Each non-empty line loses whatever leading whitespace it had and gains `indent`. A blank
         * line stays blank rather than becoming a line of whitespace, which is what a formatter
         * that only prepended would produce.
         */
        std::string Reindent(std::string_view text, const std::string &indent)
        {
            std::string out;
            out.reserve(text.size() + text.size() / 8);

            size_t lineStart = 0;
            while (lineStart <= text.size())
            {
                size_t lineEnd = text.find('\n', lineStart);
                const bool last = lineEnd == std::string_view::npos;
                if (last)
                {
                    lineEnd = text.size();
                }

                std::string_view line = text.substr(lineStart, lineEnd - lineStart);
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

                if (last)
                {
                    break;
                }
                lineStart = lineEnd + 1;
            }

            // The loop above ends every line, including one the source did not.
            if (!out.empty() && out.back() == '\n')
            {
                out.pop_back();
            }
            return out;
        }

        /** @brief Whether a string is entirely whitespace. */
        bool IsBlank(std::string_view text)
        {
            return std::all_of(text.begin(), text.end(),
                               [](unsigned char c) { return std::isspace(c) != 0; });
        }

        /**
         * @brief Drops whitespace from both ends, keeping any blank line in the middle.
         *
         * The text between two declarations begins and ends with the line breaks that separated
         * them. Carried into a block unchanged those become blank lines around every member, so a
         * merged namespace would grow a gap for each occurrence it absorbed.
         */
        std::string_view TrimBlankEdges(std::string_view text)
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
    }

    std::string FormatPredefinedStub(const std::string &source,
                                     angel_lsp::parser::AngelScriptParser &parser,
                                     const std::string &indent)
    {
        TSTree *tree = parser.Parse(source);
        if (tree == nullptr)
        {
            return source;
        }

        const TSNode root = ts_tree_root_node(tree);

        // A file that did not parse is a file whose declaration boundaries are guesses, and moving
        // text on a guess is how a formatter eats someone's work. Left alone instead.
        if (ts_node_has_error(root))
        {
            ts_tree_delete(tree);
            return source;
        }

        // The plan is the output in order: either a literal run of source, or a namespace group to
        // be emitted at that point.
        struct Slot
        {
            bool isGroup = false;
            std::string literal;   ///< used when isGroup is false
            std::string groupName; ///< used when isGroup is true
        };

        std::vector<Slot> plan;
        std::map<std::string, Group> groups;

        uint32_t cursor = 0;
        const uint32_t childCount = ts_node_named_child_count(root);

        for (uint32_t i = 0; i < childCount; ++i)
        {
            const TSNode child = ts_node_named_child(root, i);

            // A comment is a named child of the root in this grammar, so left to itself it becomes
            // a declaration of its own and stops belonging to anything. Skipped WITHOUT moving the
            // cursor, which leaves it inside the next declaration's lead - that is the whole
            // mechanism by which a doc comment travels with what it describes.
            //
            // A comment after the last declaration is picked up by the tail for the same reason.
            if (std::string_view(ts_node_type(child)) == "comment")
            {
                continue;
            }

            const uint32_t start = ts_node_start_byte(child);
            const uint32_t end = ts_node_end_byte(child);

            const std::string_view lead = Slice(source, cursor, start);
            cursor = end;

            if (std::string_view(ts_node_type(child)) != "namespace_declaration")
            {
                plan.push_back(Slot{ false, std::string(lead) + std::string(Slice(source, start, end)), {} });
                continue;
            }

            const TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
            const TSNode bodyNode = ts_node_child_by_field_name(child, "body", 4);
            if (ts_node_is_null(nameNode) || ts_node_is_null(bodyNode))
            {
                plan.push_back(Slot{ false, std::string(lead) + std::string(Slice(source, start, end)), {} });
                continue;
            }

            const std::string name(Slice(source, ts_node_start_byte(nameNode), ts_node_end_byte(nameNode)));

            // Inside the braces, which are the body's first and last bytes.
            const uint32_t bodyStart = ts_node_start_byte(bodyNode) + 1;
            const uint32_t bodyEnd = ts_node_end_byte(bodyNode) - 1;

            auto [entry, inserted] = groups.try_emplace(name, Group{ name, {}, plan.size() });
            if (inserted)
            {
                plan.push_back(Slot{ true, {}, name });
            }

            entry->second.occurrences.push_back(
                Occurrence{ std::string(lead), std::string(Slice(source, bodyStart, bodyEnd)) });
        }

        const std::string tail(Slice(source, cursor, static_cast<uint32_t>(source.size())));
        ts_tree_delete(tree);

        std::string out;
        out.reserve(source.size());

        for (const Slot &slot : plan)
        {
            if (!slot.isGroup)
            {
                out.append(slot.literal);
                continue;
            }

            const Group &group = groups.at(slot.groupName);

            // The lead of the FIRST occurrence is the only one that can be about the namespace
            // rather than about a member, because it is the only one that was not preceded by a
            // declaration of this same namespace. It stays outside the block; every later one goes
            // inside with the members it introduced, which is the case the user meets - a comment
            // sitting above `namespace String { const string EMPTY_STRING; }`.
            const std::string &firstLead = group.occurrences.front().lead;
            out.append(TrimTrailingSpaceOnEachLine(firstLead));

            out.append("namespace ");
            out.append(group.name);
            out.push_back('\n');
            out.append("{\n");

            for (size_t i = 0; i < group.occurrences.size(); ++i)
            {
                const Occurrence &occurrence = group.occurrences[i];

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

        out.append(tail);

        // Returned as the same object when it came out identical, so that formatting a formatted
        // file is guaranteed to produce no edit at all rather than an empty-looking one.
        return out == source ? source : out;
    }
}
