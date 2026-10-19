#include "analysis/EngineConfigConverter.h"
#include <algorithm>
#include <charconv>
#include <sstream>

namespace angel_lsp::analysis
{
namespace
{
struct EnumEntry
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> values;
};

struct ClassEntry
{
    std::string name;
    std::vector<std::string> constructors;
    std::vector<std::string> destructors;
    std::vector<std::string> properties;
    std::vector<std::string> methods;
};

struct InterfaceEntry
{
    std::string name;
    std::vector<std::string> methods;
};

struct NamespaceData
{
    std::string name;
    std::vector<EnumEntry> enums;
    std::vector<ClassEntry> classes;
    std::vector<InterfaceEntry> interfaces;
    std::vector<std::string> typedefs;
    std::vector<std::string> funcdefs;
    std::vector<std::string> functions;
    std::vector<std::string> properties;
};

std::string ConsumeQuotedToken(std::string_view line, size_t& i)
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

std::string_view ConsumeWordToken(std::string_view line, size_t& i)
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

NamespaceData& FindOrCreateScope(std::vector<NamespaceData>& scopes, const std::string& name)
{
    for (auto& s : scopes)
    {
        if (s.name == name)
            return s;
    }
    scopes.push_back(NamespaceData{.name = name});
    return scopes.back();
}

ClassEntry& FindOrCreateClass(NamespaceData& scope, const std::string& name)
{
    for (auto& c : scope.classes)
    {
        if (c.name == name)
            return c;
    }
    scope.classes.push_back(ClassEntry{.name = name});
    return scope.classes.back();
}

EnumEntry& FindOrCreateEnum(NamespaceData& scope, const std::string& name)
{
    for (auto& e : scope.enums)
    {
        if (e.name == name)
            return e;
    }
    scope.enums.push_back(EnumEntry{.name = name});
    return scope.enums.back();
}

InterfaceEntry& FindOrCreateInterface(NamespaceData& scope, const std::string& name)
{
    for (auto& in : scope.interfaces)
    {
        if (in.name == name)
            return in;
    }
    scope.interfaces.push_back(InterfaceEntry{.name = name});
    return scope.interfaces.back();
}

void ProcessBehaviour(ClassEntry& cls, int behId, std::string decl)
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

void EnsureSemicolon(std::string& s)
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

bool ProcessTypeDirective(const std::vector<std::string>& tok, NamespaceData& scope)
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

bool ProcessEnumDirective(const std::vector<std::string>& tok, NamespaceData& scope)
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

void ProcessEntityDirective(const std::vector<std::string>& tok, NamespaceData& scope)
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

void FormatEnums(const std::vector<EnumEntry>& enums, const std::string& indent, std::string& out)
{
    for (const auto& en : enums)
    {
        out += indent + "enum " + en.name + "\n" + indent + "{\n";
        for (const auto& [vName, vVal] : en.values)
            out += indent + "    " + vName + " = " + vVal + ",\n";
        out += indent + "}\n\n";
    }
}

void FormatClasses(const std::vector<ClassEntry>& classes, const std::string& indent, std::string& out)
{
    for (const auto& cls : classes)
    {
        out += indent + "class " + cls.name + "\n" + indent + "{\n";
        for (const auto& d : cls.destructors)
            out += indent + "    " + d + "\n";
        for (const auto& c : cls.constructors)
            out += indent + "    " + c + "\n";
        for (const auto& p : cls.properties)
            out += indent + "    " + p + "\n";
        for (const auto& m : cls.methods)
            out += indent + "    " + m + "\n";
        out += indent + "}\n\n";
    }
}

void FormatScopeEntities(const NamespaceData& scope, const std::string& indent, std::string& out)
{
    FormatEnums(scope.enums, indent, out);
    for (const auto& td : scope.typedefs)
        out += indent + td + "\n";
    for (const auto& fd : scope.funcdefs)
        out += indent + fd + "\n";
    for (const auto& in : scope.interfaces)
    {
        out += indent + "interface " + in.name + "\n" + indent + "{\n";
        for (const auto& m : in.methods)
            out += indent + "    " + m + "\n";
        out += indent + "}\n\n";
    }
    FormatClasses(scope.classes, indent, out);
    for (const auto& p : scope.properties)
        out += indent + p + "\n";
    for (const auto& f : scope.functions)
        out += indent + f + "\n";
}

void FormatNamespace(const NamespaceData& scope, std::string& out)
{
    if (scope.name.empty())
    {
        FormatScopeEntities(scope, "", out);
    }
    else
    {
        out += "namespace " + scope.name + "\n{\n";
        FormatScopeEntities(scope, "    ", out);
        out += "}\n\n";
    }
}
} // namespace

bool IsOfficialEngineConfig(std::string_view content)
{
    if (content.empty())
        return false;
    const std::string_view sample = content.substr(0, std::min<size_t>(content.size(), 2048));
    if (sample.find("// AngelScript") != std::string_view::npos)
        return true;
    if (sample.starts_with("ep 0 ") || sample.find("\nep 0 ") != std::string_view::npos ||
        sample.starts_with("ep 1 ") || sample.find("\nep 1 ") != std::string_view::npos)
        return true;
    return (sample.starts_with("objtype \"") || sample.find("\nobjtype \"") != std::string_view::npos);
}

EngineConfigResult ConvertEngineConfigToPredefined(std::string_view configContent)
{
    EngineConfigResult result;
    std::vector<NamespaceData> scopes;
    std::string currNs;

    std::stringstream ss((std::string(configContent)));
    std::string line;
    while (std::getline(ss, line))
    {
        const auto tok = TokenizeLine(line);
        if (tok.empty())
            continue;

        if (tok[0] == "ep" && tok.size() >= 3)
        {
            int propId = 0;
            long long val = 0;
            std::from_chars(tok[1].data(), tok[1].data() + tok[1].size(), propId);
            std::from_chars(tok[2].data(), tok[2].data() + tok[2].size(), val);
            ApplyEngineProperty(propId, val, result.engineProperties);
        }
        else if (tok[0] == "namespace" && tok.size() >= 2)
        {
            currNs = tok[1];
        }
        else
        {
            auto& scope = FindOrCreateScope(scopes, currNs);
            ProcessMemberDirective(tok, scope);
            ProcessDeclarationDirective(tok, scope);
        }
    }

    result.predefinedContent = "// Predefined stub generated from AngelScript engine configuration dump\n\n";
    for (const auto& scope : scopes)
        FormatNamespace(scope, result.predefinedContent);

    return result;
}
} // namespace angel_lsp::analysis
