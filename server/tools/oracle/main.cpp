// A real AngelScript compiler, used as an oracle for the parity audit.
//
// The analyzer in this repository is hand-written and never links the AngelScript SDK, so nothing
// in the test suite could say whether what it does matches the language - only whether it matches
// what the tests expect. This binary closes that: it builds a real asIScriptEngine, registers the
// SDK's standard add-ons, compiles the script it is given, and exits 0 when the compiler accepts it
// and 1 when it does not.
//
// It exists because the parity work previously depended on AS-Harness, a binary from a repository
// that is not published, so the audit could only ever run on one developer's machine. Everything
// here is fetched from https://github.com/anjo76/angelscript, which means CI can build it too.
//
// The script surface this registers is described by tests/fixtures/sdk-addons.as.predefined, and
// the two have to be kept in step: an oracle and a stub that disagree produce findings about the
// disagreement rather than about the analyzer. Every registration below has a matching declaration
// there, and nothing is registered that the stub does not declare.
//
// Usage:
//     angelscript_oracle <script.as> [--json] [--property-accessor-mode=<2|3>]
//
// The accessor mode is the one engine property whose value changes what the language accepts in a
// way this analyzer models as a setting, so both halves of that answer have to be recordable. Left
// unset, the engine's own default (3) applies and every existing invocation is unchanged.
//
// Exit codes: 0 compiled, 1 rejected, 2 could not read the file or start the engine.

#include <angelscript.h>

#include <scriptarray/scriptarray.h>
#include <scriptany/scriptany.h>
#include <scriptbuilder/scriptbuilder.h>
#include <scriptdictionary/scriptdictionary.h>
#include <scriptfile/scriptfile.h>
#include <scriptfile/scriptfilesystem.h>
#include <scriptgrid/scriptgrid.h>
#include <scripthandle/scripthandle.h>
#include <scripthelper/scripthelper.h>
#include <scriptmath/scriptmath.h>
#include <scriptmath/scriptmathcomplex.h>
#include <scriptstdstring/scriptstdstring.h>
#include <datetime/datetime.h>
#include <weakref/weakref.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct Message
    {
        std::string section;
        int row = 0;
        int col = 0;
        std::string type;
        std::string text;
    };

    std::vector<Message> g_messages;
    bool g_sawError = false;

    void MessageCallback(const asSMessageInfo *info, void *)
    {
        const char *type = "INFO";
        if (info->type == asMSGTYPE_ERROR)
        {
            type = "ERROR";
            g_sawError = true;
        }
        else if (info->type == asMSGTYPE_WARNING)
        {
            type = "WARNING";
        }

        g_messages.push_back(Message{
            info->section ? info->section : "", info->row, info->col, type,
            info->message ? info->message : ""});
    }

    // `print` is not an SDK add-on - the SDK leaves output to the host - but scripts in the corpora
    // call it, so the oracle provides it and the stub declares it. Registering nothing would make
    // every such script fail for a reason that has nothing to do with the analyzer.
    void ScriptPrint(const std::string &line) { std::fputs(line.c_str(), stdout); }
    void ScriptPrintLine(const std::string &line) { std::printf("%s\n", line.c_str()); }

    /**
     * @brief Names of the `mixin class` declarations in a script, at global scope.
     *
     * Skips comments and string literals so a `mixin` written inside either is not mistaken for a
     * declaration, and stops descending at the first `{` of a body so nested types are not scanned.
     */
    std::vector<std::string> FindGlobalMixins(const std::string &source)
    {
        std::vector<std::string> names;

        auto isIdentChar = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        };

        std::vector<std::string> tokens;
        std::vector<size_t> depthAt;
        size_t depth = 0;

        for (size_t i = 0; i < source.size();)
        {
            const char c = source[i];

            if (c == '/' && i + 1 < source.size() && source[i + 1] == '/')
            {
                while (i < source.size() && source[i] != '\n') { ++i; }
                continue;
            }
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) { ++i; }
                i = (i + 2 < source.size()) ? i + 2 : source.size();
                continue;
            }
            if (c == '"' || c == '\'')
            {
                const char quote = c;
                ++i;
                while (i < source.size() && source[i] != quote)
                {
                    if (source[i] == '\\') { ++i; }
                    ++i;
                }
                ++i;
                continue;
            }
            if (c == '{') { ++depth; ++i; continue; }
            if (c == '}') { if (depth > 0) { --depth; } ++i; continue; }

            if (isIdentChar(c))
            {
                const size_t start = i;
                while (i < source.size() && isIdentChar(source[i])) { ++i; }
                tokens.push_back(source.substr(start, i - start));
                depthAt.push_back(depth);
                continue;
            }
            ++i;
        }

        for (size_t i = 0; i + 1 < tokens.size(); ++i)
        {
            if (tokens[i] != "mixin" || depthAt[i] != 0)
            {
                continue;
            }
            // `mixin` may be followed by other declaration modifiers before `class`.
            size_t j = i + 1;
            while (j < tokens.size() && tokens[j] != "class")
            {
                if (tokens[j] != "shared" && tokens[j] != "abstract" && tokens[j] != "final" &&
                    tokens[j] != "external")
                {
                    break;
                }
                ++j;
            }
            if (j < tokens.size() && tokens[j] == "class" && j + 1 < tokens.size())
            {
                names.push_back(tokens[j + 1]);
            }
        }

        return names;
    }

    /**
     * @brief Appends a class that inherits each mixin, so the compiler validates it.
     *
     * AngelScript only checks a mixin when something inherits it: `mixin class M : SomeClass {}` is
     * an error the compiler never reaches if nothing uses `M`. That leaves every mixin rule
     * unanswerable by an oracle, and worse, makes a *correct* diagnostic from the analyzer look like
     * a false positive - which is exactly what happened to `as-err-mixin-inherit-class`.
     *
     * AS-Harness solves it the same way, with the same kind of synthetic subclass, so the answers
     * this oracle gives on mixins line up with the ones the corpora were recorded against.
     *
     * The cost is honest and worth naming: the probe can surface an error the original script would
     * not have had on its own - an unimplemented interface method, say - because instantiating a
     * mixin is what makes those checkable at all.
     */
    std::string WithMixinProbes(const std::string &source)
    {
        const std::vector<std::string> mixins = FindGlobalMixins(source);
        if (mixins.empty())
        {
            return source;
        }

        std::string augmented = source;
        augmented += "\n// Appended by angelscript_oracle so the compiler validates these mixins.\n";
        for (size_t i = 0; i < mixins.size(); ++i)
        {
            augmented += "class _OracleMixinProbe_" + std::to_string(i) + " : " + mixins[i] + " {}\n";
        }
        return augmented;
    }

    std::string ReadWholeFile(const std::string &path, bool &ok)
    {
        ok = false;
        std::FILE *handle = std::fopen(path.c_str(), "rb");
        if (handle == nullptr)
        {
            return {};
        }
        std::string contents;
        char buffer[4096];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), handle)) > 0)
        {
            contents.append(buffer, read);
        }
        std::fclose(handle);
        ok = true;
        return contents;
    }

    std::string JsonEscape(const std::string &text)
    {
        std::string out;
        out.reserve(text.size() + 8);
        for (const char c : text)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else
                {
                    out += c;
                }
            }
        }
        return out;
    }

    void PrintJson()
    {
        std::printf("{\n  \"hasError\": %s,\n  \"messages\": [\n", g_sawError ? "true" : "false");
        for (size_t i = 0; i < g_messages.size(); ++i)
        {
            const Message &m = g_messages[i];
            std::printf("    { \"type\": \"%s\", \"row\": %d, \"col\": %d, \"message\": \"%s\" }%s\n",
                        m.type.c_str(), m.row, m.col, JsonEscape(m.text).c_str(),
                        i + 1 < g_messages.size() ? "," : "");
        }
        std::printf("  ]\n}\n");
    }
}

int main(int argc, char **argv)
{
    const char *scriptPath = nullptr;
    bool asJson = false;

    // Engine properties, so a script whose verdict depends on one can be asked about under each
    // setting rather than only under the SDK's default. asEP_PROPERTY_ACCESSOR_MODE is the first:
    // the whole of `get_X`/`set_X` handling turns on it - mode 3 requires the `property` keyword,
    // mode 2 does not - and a parity case recordable under only one of them records half the
    // answer.
    //
    // -1 leaves the engine's own default, so every existing invocation and every script already in
    // tests/parity answers exactly as it did.
    //
    // The other three are the rest of config::EngineProperties. The rule that produced this list
    // is: every property the analyzer models must be askable here, or its behaviour under that
    // setting is recorded on faith. That is exactly what had happened to the accessor mode - the
    // server had a setting, the document claimed an answer for both of its values, and only one
    // of them had ever been measured.
    int propertyAccessorMode = -1;
    int allowUnsafeReferences = -1;
    int privatePropAsProtected = -1;
    int disallowGlobalVars = -1;
    int boolConversionMode = -1;
    int useCharacterLiterals = -1;
    int allowMultilineStrings = -1;
    int disallowValueAssignForRef = -1;
    int alterSyntaxNamedArgs = -1;
    int disableIntegerDivision = -1;
    int disallowEmptyListElements = -1;
    int foreachSupport = -1;
    int heredocTrimMode = -1;

    for (int i = 1; i < argc; ++i)
    {
        // Unknown flags are ignored rather than rejected: the parity harness passes the same
        // arguments it passed AS-Harness, and failing on one of them would look like a rejected
        // script.
        if (std::strncmp(argv[i], "--", 2) == 0)
        {
            if (std::strcmp(argv[i], "--json") == 0)
            {
                asJson = true;
            }
            else if (std::strncmp(argv[i], "--property-accessor-mode=", 25) == 0)
            {
                propertyAccessorMode = std::atoi(argv[i] + 25);
            }
            else if (std::strncmp(argv[i], "--allow-unsafe-references=", 26) == 0)
            {
                allowUnsafeReferences = std::atoi(argv[i] + 26);
            }
            else if (std::strncmp(argv[i], "--private-prop-as-protected=", 28) == 0)
            {
                privatePropAsProtected = std::atoi(argv[i] + 28);
            }
            else if (std::strncmp(argv[i], "--disallow-global-vars=", 23) == 0)
            {
                disallowGlobalVars = std::atoi(argv[i] + 23);
            }
            else if (std::strncmp(argv[i], "--bool-conversion-mode=", 23) == 0)
            {
                boolConversionMode = std::atoi(argv[i] + 23);
            }
            else if (std::strncmp(argv[i], "--use-character-literals=", 25) == 0)
            {
                useCharacterLiterals = std::atoi(argv[i] + 25);
            }
            else if (std::strncmp(argv[i], "--allow-multiline-strings=", 26) == 0)
            {
                allowMultilineStrings = std::atoi(argv[i] + 26);
            }
            else if (std::strncmp(argv[i], "--disallow-value-assign-for-ref=", 32) == 0)
            {
                disallowValueAssignForRef = std::atoi(argv[i] + 32);
            }
            else if (std::strncmp(argv[i], "--alter-syntax-named-args=", 26) == 0)
            {
                alterSyntaxNamedArgs = std::atoi(argv[i] + 26);
            }
            else if (std::strncmp(argv[i], "--disable-integer-division=", 27) == 0)
            {
                disableIntegerDivision = std::atoi(argv[i] + 27);
            }
            else if (std::strncmp(argv[i], "--disallow-empty-list-elements=", 31) == 0)
            {
                disallowEmptyListElements = std::atoi(argv[i] + 31);
            }
            else if (std::strncmp(argv[i], "--foreach-support=", 18) == 0)
            {
                foreachSupport = std::atoi(argv[i] + 18);
            }
            else if (std::strncmp(argv[i], "--heredoc-trim-mode=", 20) == 0)
            {
                heredocTrimMode = std::atoi(argv[i] + 20);
            }
            continue;
        }
        if (scriptPath == nullptr)
        {
            scriptPath = argv[i];
        }
    }

    if (scriptPath == nullptr)
    {
        std::fprintf(stderr, "usage: angelscript_oracle <script.as> [--json]\n"
                             "       [--property-accessor-mode=<0|1|2|3>]\n"
                             "       [--allow-unsafe-references=<0|1>]\n"
                             "       [--private-prop-as-protected=<0|1>]\n"
                             "       [--disallow-global-vars=<0|1>]\n"
                             "       [--bool-conversion-mode=<0|1>]\n"
                             "       [--use-character-literals=<0|1>]\n"
                             "       [--allow-multiline-strings=<0|1>]\n"
                             "       [--disallow-value-assign-for-ref=<0|1>]\n"
                             "       [--alter-syntax-named-args=<0|1|2>]\n"
                             "       [--disable-integer-division=<0|1>]\n"
                             "       [--disallow-empty-list-elements=<0|1>]\n"
                             "       [--foreach-support=<0|1>]\n"
                             "       [--heredoc-trim-mode=<0|1|2>]\n"
                             "Any option left out keeps the engine's own default.\n");
        return 2;
    }

    asIScriptEngine *engine = asCreateScriptEngine();
    if (engine == nullptr)
    {
        std::fprintf(stderr, "angelscript_oracle: could not create the script engine\n");
        return 2;
    }

    engine->SetMessageCallback(asFUNCTION(MessageCallback), nullptr, asCALL_CDECL);

    // Set before anything is registered: these change how declarations are interpreted.
    if (propertyAccessorMode >= 0)
    {
        engine->SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE,
                                  static_cast<asPWORD>(propertyAccessorMode));
    }
    if (allowUnsafeReferences >= 0)
    {
        engine->SetEngineProperty(asEP_ALLOW_UNSAFE_REFERENCES,
                                  static_cast<asPWORD>(allowUnsafeReferences));
    }
    if (privatePropAsProtected >= 0)
    {
        engine->SetEngineProperty(asEP_PRIVATE_PROP_AS_PROTECTED,
                                  static_cast<asPWORD>(privatePropAsProtected));
    }
    if (disallowGlobalVars >= 0)
    {
        engine->SetEngineProperty(asEP_DISALLOW_GLOBAL_VARS,
                                  static_cast<asPWORD>(disallowGlobalVars));
    }
    if (boolConversionMode >= 0)
    {
        engine->SetEngineProperty(asEP_BOOL_CONVERSION_MODE,
                                  static_cast<asPWORD>(boolConversionMode));
    }
    if (useCharacterLiterals >= 0)
    {
        engine->SetEngineProperty(asEP_USE_CHARACTER_LITERALS,
                                  static_cast<asPWORD>(useCharacterLiterals));
    }
    if (allowMultilineStrings >= 0)
    {
        engine->SetEngineProperty(asEP_ALLOW_MULTILINE_STRINGS,
                                  static_cast<asPWORD>(allowMultilineStrings));
    }
    if (disallowValueAssignForRef >= 0)
    {
        engine->SetEngineProperty(asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE,
                                  static_cast<asPWORD>(disallowValueAssignForRef));
    }
    if (alterSyntaxNamedArgs >= 0)
    {
        engine->SetEngineProperty(asEP_ALTER_SYNTAX_NAMED_ARGS,
                                  static_cast<asPWORD>(alterSyntaxNamedArgs));
    }
    if (disableIntegerDivision >= 0)
    {
        engine->SetEngineProperty(asEP_DISABLE_INTEGER_DIVISION,
                                  static_cast<asPWORD>(disableIntegerDivision));
    }
    if (disallowEmptyListElements >= 0)
    {
        engine->SetEngineProperty(asEP_DISALLOW_EMPTY_LIST_ELEMENTS,
                                  static_cast<asPWORD>(disallowEmptyListElements));
    }
    if (foreachSupport >= 0)
    {
        engine->SetEngineProperty(asEP_FOREACH_SUPPORT,
                                  static_cast<asPWORD>(foreachSupport));
    }
    if (heredocTrimMode >= 0)
    {
        engine->SetEngineProperty(asEP_HEREDOC_TRIM_MODE,
                                  static_cast<asPWORD>(heredocTrimMode));
    }

    // Order matters: the dictionary needs `string` and `array<string>` to already exist, and the
    // grid and any add-ons assume the array is registered. This mirrors the order the SDK's own
    // samples use.
    RegisterStdString(engine);
    RegisterScriptArray(engine, true);
    RegisterStdStringUtils(engine);
    RegisterScriptDictionary(engine);
    RegisterScriptHandle(engine);
    RegisterScriptWeakRef(engine);
    RegisterScriptAny(engine);
    RegisterScriptGrid(engine);
    RegisterScriptMath(engine);
    RegisterScriptMathComplex(engine);
    RegisterScriptDateTime(engine);
    RegisterScriptFile(engine);
    RegisterScriptFileSystem(engine);
    RegisterExceptionRoutines(engine);

    engine->RegisterGlobalFunction("void print(const string &in line)",
                                   asFUNCTION(ScriptPrint), asCALL_CDECL);
    engine->RegisterGlobalFunction("void println(const string &in line)",
                                   asFUNCTION(ScriptPrintLine), asCALL_CDECL);

    // Built through CScriptBuilder rather than AddScriptSection, so `#include` and `#if` behave the
    // way they do for a real host - which is the same behaviour utils/PreprocessorRegions.h mirrors.
    CScriptBuilder builder;
    int result = builder.StartNewModule(engine, "oracle");
    if (result < 0)
    {
        std::fprintf(stderr, "angelscript_oracle: could not start a module\n");
        engine->ShutDownAndRelease();
        return 2;
    }

    bool readOk = false;
    const std::string source = ReadWholeFile(scriptPath, readOk);
    if (!readOk)
    {
        std::fprintf(stderr, "angelscript_oracle: could not read '%s'\n", scriptPath);
        engine->ShutDownAndRelease();
        return 2;
    }

    // The section keeps the script's own path as its name so diagnostics point at the real file.
    result = builder.AddSectionFromMemory(scriptPath, WithMixinProbes(source).c_str());
    if (result < 0)
    {
        // A file the builder cannot read is not a rejected script; say so distinctly so the harness
        // does not score it as a disagreement.
        if (asJson)
        {
            PrintJson();
        }
        std::fprintf(stderr, "angelscript_oracle: could not read '%s'\n", scriptPath);
        engine->ShutDownAndRelease();
        return 2;
    }

    result = builder.BuildModule();

    if (asJson)
    {
        PrintJson();
    }
    else
    {
        for (const Message &m : g_messages)
        {
            std::fprintf(stderr, "%s (%d, %d): %s\n", m.type.c_str(), m.row, m.col, m.text.c_str());
        }
    }

    engine->ShutDownAndRelease();
    return (result < 0 || g_sawError) ? 1 : 0;
}
