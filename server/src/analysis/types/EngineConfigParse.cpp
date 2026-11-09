#include "analysis/types/EngineConfigConverter.h"
#include "analysis/types/EngineConfigInternal.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace angel_lsp::analysis::engine_config
{
static std::string ConsumeQuotedToken(std::string_view line, size_t& i)
{
    const size_t len = line.size();
    ++i;
    std::string tok;
    while (i < len && line[i] != '"')
    {
        if (line[i] == '\\' && i + 1 < len && line[i + 1] == '"')
        {
            tok.push_back('"');
            i += 2;
        }
        else
        {
            tok.push_back(line[i++]);
        }
    }
    if (i < len && line[i] == '"')
        ++i;
    return tok;
}

static std::string_view ConsumeWordToken(std::string_view line, size_t& i)
{
    const size_t start = i;
    const size_t len = line.size();
    while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n')
        ++i;
    return line.substr(start, i - start);
}

std::vector<std::string> TokenizeLine(std::string_view line)
{
    std::vector<std::string> tokens;
    size_t i = 0;
    const size_t len = line.size();
    while (i < len)
    {
        while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
            ++i;
        if (i >= len || line[i] == '\n' || (line[i] == '/' && i + 1 < len && line[i + 1] == '/'))
            break;

        if (line[i] == '"')
            tokens.push_back(ConsumeQuotedToken(line, i));
        else
            tokens.emplace_back(ConsumeWordToken(line, i));
    }
    return tokens;
}

void ApplyEngineProperty(int id, long long val, config::EngineProperties& props)
{
    switch (id)
    {
    case 1:
        props.allowUnsafeReferences = (val != 0);
        break;
    case 5:
        props.useCharacterLiterals = static_cast<int>(val);
        break;
    case 6:
        props.allowMultilineStrings = (val != 0);
        break;
    case 10:
        props.requireEnumScope = (val != 0);
        break;
    case 14:
        props.propertyAccessorMode = static_cast<int>(val);
        break;
    case 17:
        props.disallowGlobalVars = (val != 0);
        break;
    case 18:
        props.alwaysImplDefaultConstruct = (val != 0);
        break;
    case 20:
        props.disallowValueAssignForRef = (val != 0);
        break;
    case 21:
        props.alterSyntaxNamedArgs = static_cast<int>(val);
        break;
    case 22:
        props.disableIntegerDivision = (val != 0);
        break;
    case 23:
        props.disallowEmptyListElements = (val != 0);
        break;
    case 24:
        props.privatePropAsProtected = (val != 0);
        break;
    case 39:
        props.boolConversionMode = static_cast<int>(val);
        break;
    case 40:
        props.foreachSupport = (val != 0);
        break;
    default:
        break;
    }
}

NamespaceData& FindOrCreateScope(std::vector<NamespaceData>& scopes, std::string_view name)
{
    for (auto& s : scopes)
    {
        if (s.name == name)
            return s;
    }
    scopes.push_back(NamespaceData{.name = std::string(name)});
    return scopes.back();
}

/**
 * @brief Retrieves an existing class entry by name within a namespace, or appends a new one.
 * @param[in,out] scope Namespace container data where class entries are stored.
 * @param[in] name Class identifier name to find or register.
 * @return Mutable reference to the located or newly created ClassEntry.
 */
static ClassEntry& FindOrCreateClass(NamespaceData& scope, std::string_view name)
{
    for (auto& c : scope.classes)
    {
        if (c.name == name)
            return c;
    }
    scope.classes.push_back(ClassEntry{.name = std::string(name)});
    return scope.classes.back();
}

/**
 * @brief Retrieves an existing enum entry by name within a namespace, or appends a new one.
 * @param[in,out] scope Namespace container data where enum entries are stored.
 * @param[in] name Enum identifier name to find or register.
 * @return Mutable reference to the located or newly created EnumEntry.
 */
static EnumEntry& FindOrCreateEnum(NamespaceData& scope, std::string_view name)
{
    for (auto& e : scope.enums)
    {
        if (e.name == name)
            return e;
    }
    scope.enums.push_back(EnumEntry{.name = std::string(name)});
    return scope.enums.back();
}

/**
 * @brief Retrieves an existing interface entry by name within a namespace, or appends a new one.
 * @param[in,out] scope Namespace container data where interface entries are stored.
 * @param[in] name Interface identifier name to find or register.
 * @return Mutable reference to the located or newly created InterfaceEntry.
 */
static InterfaceEntry& FindOrCreateInterface(NamespaceData& scope, std::string_view name)
{
    for (auto& in : scope.interfaces)
    {
        if (in.name == name)
            return in;
    }
    scope.interfaces.push_back(InterfaceEntry{.name = std::string(name)});
    return scope.interfaces.back();
}

static void ProcessBehaviour(ClassEntry& cls, int behId, std::string decl)
{
    if (decl.empty())
        return;
    if (decl.back() != ';')
        decl += ';';

    if (behId == 0) // asBEHAVE_CONSTRUCT
    {
        const size_t paren = decl.find('(');
        cls.constructors.push_back(cls.name + (paren != std::string::npos ? decl.substr(paren) : "();"));
    }
    else if (behId == 2) // asBEHAVE_DESTRUCT
    {
        cls.destructors.push_back("~" + cls.name + "();");
    }
    else if (behId == 3) // asBEHAVE_FACTORY
    {
        const size_t paren = decl.find('(');
        if (paren != std::string::npos)
        {
            std::string factoryCtor = cls.name + decl.substr(paren);
            while (factoryCtor.starts_with("@"))
                factoryCtor.erase(0, 1);
            if (!factoryCtor.empty())
                cls.constructors.push_back(std::move(factoryCtor));
        }
    }
    else
        cls.methods.push_back(std::move(decl));
}

static void EnsureSemicolon(std::string& s)
{
    if (!s.empty() && s.back() != ';')
        s += ';';
}

void ProcessMemberDirective(const std::vector<std::string>& tok, NamespaceData& scope)
{
    if (tok.size() < 3)
        return;
    if (tok[0] == "objbeh" && tok.size() >= 4)
    {
        auto& cls = FindOrCreateClass(scope, tok[1]);
        int behId = 0;
        std::from_chars(tok[2].data(), tok[2].data() + tok[2].size(), behId);
        ProcessBehaviour(cls, behId, tok[3]);
    }
    else if (tok[0] == "objmthd")
    {
        auto& cls = FindOrCreateClass(scope, tok[1]);
        std::string m = tok[2];
        EnsureSemicolon(m);
        cls.methods.push_back(std::move(m));
    }
    else if (tok[0] == "objprop")
    {
        auto& cls = FindOrCreateClass(scope, tok[1]);
        std::string p = tok[2];
        EnsureSemicolon(p);
        cls.properties.push_back(std::move(p));
    }
    else if (tok[0] == "intfmthd")
    {
        auto& in = FindOrCreateInterface(scope, tok[1]);
        std::string m = tok[2];
        EnsureSemicolon(m);
        in.methods.push_back(std::move(m));
    }
}

static bool ProcessTypeDirective(const std::vector<std::string>& tok, NamespaceData& scope)
{
    if (tok[0] == "objtype" && tok.size() >= 2)
    {
        FindOrCreateClass(scope, tok[1]);
        return true;
    }
    if (tok[0] == "intf" && tok.size() >= 2)
    {
        FindOrCreateInterface(scope, tok[1]);
        return true;
    }
    if (tok[0] == "typedef" && tok.size() >= 3)
    {
        scope.typedefs.push_back("typedef " + tok[2] + " " + tok[1] + ";");
        return true;
    }
    return false;
}

static bool ProcessEnumDirective(const std::vector<std::string>& tok, NamespaceData& scope)
{
    if (tok[0] == "enum" && tok.size() >= 2)
    {
        FindOrCreateEnum(scope, tok[1]);
        return true;
    }
    if (tok[0] == "enumval" && tok.size() >= 4)
    {
        auto& en = FindOrCreateEnum(scope, tok[1]);
        en.values.emplace_back(tok[2], tok[3]);
        return true;
    }
    return false;
}

static void ProcessEntityDirective(const std::vector<std::string>& tok, NamespaceData& scope)
{
    if (tok.size() < 2)
        return;
    if (tok[0] == "funcdef")
    {
        std::string f = tok[1];
        EnsureSemicolon(f);
        scope.funcdefs.push_back("funcdef " + f);
    }
    else if (tok[0] == "func")
    {
        std::string f = tok[1];
        EnsureSemicolon(f);
        scope.functions.push_back(std::move(f));
    }
    else if (tok[0] == "prop")
    {
        std::string p = tok[1];
        EnsureSemicolon(p);
        scope.properties.push_back(std::move(p));
    }
}

void ProcessDeclarationDirective(const std::vector<std::string>& tok, NamespaceData& scope)
{
    if (tok.empty())
        return;
    if (!ProcessTypeDirective(tok, scope) && !ProcessEnumDirective(tok, scope))
        ProcessEntityDirective(tok, scope);
}
} // namespace angel_lsp::analysis::engine_config
