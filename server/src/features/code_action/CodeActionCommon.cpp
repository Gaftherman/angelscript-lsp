#include "analysis/DiagnosticSuppression.h"
#include "features/code_action/CodeActionInternal.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"

namespace angel_lsp::features
{

std::string GetNodeText(TSNode node, std::string_view sourceCode)
{
    return parser::GetNodeText(node, sourceCode);
}

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

bool MatchDiagnosticCode(const lsp::Diagnostic& diag, std::string_view expectedCode)
{
    if (!diag.code.has_value())
    {
        return false;
    }
    if (std::holds_alternative<lsp::String>(diag.code.value()))
    {
        const auto& codeStr = std::get<lsp::String>(diag.code.value());
        if (codeStr == expectedCode)
        {
            return true;
        }
        std::string_view canonical = analysis::GetCanonicalDiagnosticCode(codeStr);
        if (!canonical.empty() && canonical == expectedCode)
        {
            return true;
        }
        std::string_view expectedCanonical = analysis::GetCanonicalDiagnosticCode(expectedCode);
        return !expectedCanonical.empty() && expectedCanonical == canonical;
    }
    return false;
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

TSPoint FindClassClosingBracePoint(TSNode classBody)
{
    if (ts_node_is_null(classBody))
    {
        return TSPoint{0, 0};
    }
    TSPoint closingPt = ts_node_end_point(classBody);
    TSTreeCursor cursor = ts_tree_cursor_new(classBody);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            if (ts_node_end_byte(ch) == ts_node_end_byte(classBody))
            {
                closingPt = ts_node_start_point(ch);
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return closingPt;
}

lsp::CodeAction MakeDisableDiagnosticAction(const DisableDiagnosticActionOptions& options)
{
    std::string title = i18n::FormatMessage(options.i18n, "action-disable-in-workspace-settings",
                                            "Disable in workspace settings (angelscript.{})", options.settingKey);
    std::string cmdTitle = i18n::FormatMessage(options.i18n, "action-disable-in-settings", "Disable in settings");

    lsp::CodeAction action;
    action.title = std::move(title);
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.diagnostics = std::vector<lsp::Diagnostic>{options.diag};

    lsp::Command cmd;
    cmd.title = std::move(cmdTitle);
    cmd.command = "angelscript.disableDiagnostic";
    lsp::json::Array cmdArgs;
    lsp::json::Object argObj;
    argObj["settingKey"] = lsp::json::Value("angelscript." + options.settingKey);
    argObj["setting"] = lsp::json::Value(std::string(options.settingKey));
    argObj["value"] = lsp::json::Value(false);
    argObj["code"] = lsp::json::Value(std::string(options.code));
    cmdArgs.push_back(std::move(argObj));
    cmd.arguments = std::move(cmdArgs);

    action.command = std::move(cmd);
    return action;
}

lsp::CodeAction MakeDisableDiagnosticAction(const lsp::Diagnostic& diag, std::string title, std::string settingKey,
                                            std::string code)
{
    lsp::CodeAction action;
    action.title = std::move(title);
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.diagnostics = std::vector<lsp::Diagnostic>{diag};

    lsp::Command cmd;
    cmd.title = "Disable in settings";
    cmd.command = "angelscript.disableDiagnostic";
    lsp::json::Array cmdArgs;
    lsp::json::Object argObj;
    argObj["settingKey"] = lsp::json::Value("angelscript." + settingKey);
    argObj["setting"] = lsp::json::Value(std::move(settingKey));
    argObj["value"] = lsp::json::Value(false);
    argObj["code"] = lsp::json::Value(std::move(code));
    cmdArgs.push_back(std::move(argObj));
    cmd.arguments = std::move(cmdArgs);

    action.command = std::move(cmd);
    return action;
}

} // namespace angel_lsp::features
