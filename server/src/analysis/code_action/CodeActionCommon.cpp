#include "analysis/code_action/CodeActionInternal.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "i18n/i18n.h"

#include <algorithm>

namespace angel_lsp::analysis::code_action
{

std::string GetLineIndentation(std::string_view sourceCode, uint32_t line)
{
    uint32_t currLine = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < sourceCode.size(); ++i)
    {
        if (currLine == line)
        {
            lineStart = i;
            break;
        }
        if (sourceCode[i] == '\n')
        {
            currLine++;
        }
    }
    if (currLine != line)
    {
        return "";
    }
    size_t i = lineStart;
    while (i < sourceCode.size() && (sourceCode[i] == ' ' || sourceCode[i] == '\t'))
    {
        i++;
    }
    return std::string(sourceCode.substr(lineStart, i - lineStart));
}

const analysis::Scope* FindScopeByLine(const analysis::Scope* root, uint32_t line)
{
    if (!root)
    {
        return nullptr;
    }
    const analysis::Scope* curr = root;
    bool movedDeeper = true;
    while (movedDeeper)
    {
        movedDeeper = false;
        for (const auto& child : curr->children)
        {
            if (child->startLine <= line && child->endLine >= line)
            {
                curr = child.get();
                movedDeeper = true;
                break;
            }
        }
    }
    return curr;
}

const analysis::Scope* FindScopeByLineOrRoot(const analysis::Scope* root, uint32_t line)
{
    const analysis::Scope* found = FindScopeByLine(root, line);
    return found ? found : root;
}

/**
 * @brief Compares diagnostic code strings directly and canonically.
 * @param[in] code Diagnostic code string to check.
 * @param[in] expectedCode Target error or warning code.
 * @return True if code matches expectedCode directly or via canonical aliases.
 */
static bool CodeMatches(std::string_view code, std::string_view expectedCode)
{
    if (code.empty())
    {
        return false;
    }
    if (code == expectedCode)
    {
        return true;
    }
    std::string_view canonical = analysis::GetCanonicalDiagnosticCode(code);
    if (!canonical.empty() && canonical == expectedCode)
    {
        return true;
    }
    std::string_view expectedCanonical = analysis::GetCanonicalDiagnosticCode(expectedCode);
    return !expectedCanonical.empty() && expectedCanonical == canonical;
}

bool MatchDiagnosticCode(const CodeActionDiagnosticView& diag, std::string_view expectedCode)
{
    return CodeMatches(diag.code, expectedCode);
}

size_t BoundedEditDistance(std::string_view a, std::string_view b, size_t limit)
{
    if (a.size() > b.size())
    {
        std::swap(a, b);
    }
    if (b.size() - a.size() > limit)
    {
        return limit + 1;
    }

    std::vector<size_t> previous(a.size() + 1);
    std::vector<size_t> current(a.size() + 1);
    for (size_t i = 0; i <= a.size(); ++i)
    {
        previous[i] = i;
    }

    for (size_t j = 1; j <= b.size(); ++j)
    {
        current[0] = j;
        size_t rowBest = current[0];
        for (size_t i = 1; i <= a.size(); ++i)
        {
            const size_t substitution = previous[i - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            current[i] = std::min({substitution, previous[i] + 1, current[i - 1] + 1});
            rowBest = std::min(rowBest, current[i]);
        }

        if (rowBest > limit)
        {
            return limit + 1;
        }
        previous.swap(current);
    }

    return previous[a.size()];
}

size_t SuggestionLimit(size_t nameLength)
{
    if (nameLength < 3)
    {
        return 0;
    }
    if (nameLength < 5)
    {
        return 1;
    }
    if (nameLength < 8)
    {
        return 2;
    }
    return 3;
}

std::string FoldCase(std::string_view text)
{
    std::string folded(text);
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return folded;
}

void CollectAllReferences(const analysis::Scope* rootScope, ankerl::unordered_dense::set<std::string>& refs)
{
    if (!rootScope)
    {
        return;
    }
    std::vector<const analysis::Scope*> worklist = {rootScope};
    while (!worklist.empty())
    {
        const analysis::Scope* sc = worklist.back();
        worklist.pop_back();
        for (const auto& ref : sc->references)
        {
            refs.insert(ref.name);
        }
        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
}

CodeActionEntry MakeDisableDiagnosticEntry(const DisableDiagnosticEntryOptions& options)
{
    CodeActionEntry entry;
    entry.title = !options.title.empty()
                      ? options.title
                      : i18n::FormatMessage(options.i18n, "action-disable-in-workspace-settings",
                                            "Disable in workspace settings (angelscript.{})", options.settingKey);
    entry.kind = CodeActionEntryKind::QuickFix;
    entry.diagnosticIndices = std::vector<uint32_t>{options.diagnosticIndex};

    CodeActionCommandEntry cmd;
    cmd.kind = CodeActionCommandKind::DisableDiagnostic;
    cmd.title = i18n::FormatMessage(options.i18n, "action-disable-in-settings", "Disable in settings");
    cmd.setting = options.settingKey;
    cmd.code = options.code;
    entry.command = std::move(cmd);

    return entry;
}

CodeActionEntry MakeQuickFixEntry(QuickFixEntryOptions options)
{
    CodeActionEntry entry;
    entry.title = std::move(options.title);
    entry.kind = CodeActionEntryKind::QuickFix;
    entry.isPreferred = options.isPreferred;
    entry.diagnosticIndices = std::vector<uint32_t>{options.diagnosticIndex};
    entry.edits = std::move(options.edits);
    return entry;
}

} // namespace angel_lsp::analysis::code_action
