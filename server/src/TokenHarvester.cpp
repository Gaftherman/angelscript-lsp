/**
 * @file TokenHarvester.cpp
 * @brief Implements fault-tolerant scanning for AngelScript LSP.
 */

#include "TokenHarvester.h"
#include <sstream>
#include <algorithm>
#include <vector>
#include <map>
#include <fmt/core.h>

namespace TokenHarvester {

// =========================================================================
// INTERNAL LEXICAL HELPERS
// =========================================================================

/**
 * @brief Identifies if a token represents a scope or type declaration block.
 * @param token The keyword string to evaluate.
 * @return True if it is a class, interface, namespace, or enum.
 */
static inline bool IsTypeDeclaration(std::string_view token) {
    return token == "class" || token == "interface" || token == "namespace" || token == "enum";
}

/**
 * @brief Identifies if a token is an access or sharing modifier.
 * @param token The keyword string to evaluate.
 * @return True if it modifies scope visibility or shared status.
 */
static inline bool IsAccessModifier(std::string_view token) {
    return token == "private" || token == "protected" || token == "public" || token == "shared";
}

/**
 * @brief Identifies if a token is a global structural modifier.
 * @param token The keyword string to evaluate.
 * @return True if it is an access modifier, external, or mixin declaration.
 */
static inline bool IsGlobalModifier(std::string_view token) {
    return IsAccessModifier(token) || token == "external" || token == "mixin";
}

/**
 * @brief Identifies if a token acts as an end-of-statement or property separator.
 * @param token The keyword string to evaluate.
 * @return True if it is a semicolon, comma, or assignment operator.
 */
static inline bool IsPropertySeparator(std::string_view token) {
    return token == ";" || token == "=" || token == ",";
}

/**
 * @brief Identifies if a token represents a core built-in data type or native template.
 * @param token The keyword string to evaluate.
 * @return True if it is a primitive type (int, float, etc.) or a native template (array, dictionary).
 */
static inline bool IsBuiltInType(std::string_view token) {
    return token == "void" || token == "int" || token == "int8" || token == "int16" || token == "int32" || token == "int64" || 
           token == "uint" || token == "uint8" || token == "uint16" || token == "uint32" || token == "uint64" || 
           token == "float" || token == "double" || token == "bool" || token == "auto" || token == "string" ||
           token == "array" || token == "dictionary" || token == "grid";
}

/**
 * @brief Identifies if a token is a method declaration modifier.
 * @param token The keyword string to evaluate.
 * @return True if the token is a standard method modifier (const, override, final).
 */
static inline bool IsMethodModifier(std::string_view token) {
    return token == "const" || token == "override" || token == "final";
}

// =========================================================================
// CORE STREAM AND TYPE UTILITIES
// =========================================================================

/**
 * @class TokenStream
 * @brief Wrapper for the native AngelScript lexer to advance and peek tokens safely.
 */
class TokenStream {
private:
    asIScriptEngine* engine;
    std::string_view code;
    size_t pos;

public:
    TokenStream(asIScriptEngine* e, std::string_view c, size_t start = 0) 
        : engine(e), code(c), pos(start) {}

    asETokenClass Peek(std::string_view& outToken) {
        size_t tempPos = pos;
        asUINT len = 0;
        asETokenClass tc;
        
        do {
            if (tempPos >= code.length()) return asTC_UNKNOWN;
            tc = engine->ParseToken(code.data() + tempPos, static_cast<asUINT>(code.length() - tempPos), &len);
            if (len == 0) return asTC_UNKNOWN; 
            outToken = code.substr(tempPos, len);
            tempPos += len;
        } while (tc == asTC_WHITESPACE || tc == asTC_COMMENT);
        
        return tc;
    }

    asETokenClass Advance(std::string_view& outToken) {
        asUINT len = 0;
        asETokenClass tc;
        
        do {
            if (pos >= code.length()) return asTC_UNKNOWN;
            tc = engine->ParseToken(code.data() + pos, static_cast<asUINT>(code.length() - pos), &len);
            if (len == 0) return asTC_UNKNOWN; 
            outToken = code.substr(pos, len);
            pos += len;
        } while (tc == asTC_WHITESPACE || tc == asTC_COMMENT);
        
        return tc;
    }

    size_t GetPos() const { return pos; }
    void SetPos(size_t p) { pos = p; }
};

std::string GetBaseType(const std::string& type) {
    std::string clean = type;
    if (clean.find("const ") == 0) clean = clean.substr(6);
    
    clean.erase(std::remove(clean.begin(), clean.end(), '@'), clean.end());
    clean.erase(std::remove(clean.begin(), clean.end(), '&'), clean.end());

    size_t templatePos = clean.find('<');
    if (templatePos != std::string::npos) clean = clean.substr(0, templatePos);

    if (clean.find("[]") != std::string::npos) return "array";

    return clean;
}

std::string ExtractInnerType(const std::string& type) {
    size_t start = type.find('<');
    size_t end = type.rfind('>');
    if (start != std::string::npos && end != std::string::npos && end > start) {
        return type.substr(start + 1, end - start - 1);
    }
    size_t bracket = type.rfind("[]");
    if (bracket != std::string::npos) {
        return type.substr(0, bracket);
    }
    return type;
}

static bool IsValidDataType(asIScriptEngine* engine, TokenStream& stream, std::string_view token, const std::vector<std::string>& localClasses) {
    if (IsBuiltInType(token)) return true;

    std::string tokenStr(token);
    if (engine->GetTypeInfoByName(tokenStr.c_str()) != nullptr) return true;
    if (std::find(localClasses.begin(), localClasses.end(), tokenStr) != localClasses.end()) return true;

    size_t savedPos = stream.GetPos();
    std::string_view dummy;
    stream.Advance(dummy); 
    if (stream.Peek(dummy) == asTC_KEYWORD && dummy == "::") {
        stream.SetPos(savedPos); 
        return true;
    }
    stream.SetPos(savedPos);

    return false;
}

static std::string ParseDataType(asIScriptEngine* engine, TokenStream& stream, const std::vector<std::string>& localClasses) {
    std::string typeName = "";
    std::string_view token;
    asETokenClass tc = stream.Peek(token);

    if (tc == asTC_KEYWORD && token == "const") {
        stream.Advance(token);
        typeName += "const ";
        tc = stream.Peek(token);
    }

    if (IsValidDataType(engine, stream, token, localClasses)) {
        while (tc == asTC_IDENTIFIER || tc == asTC_KEYWORD) { 
            stream.Advance(token);
            typeName += std::string(token);
            tc = stream.Peek(token);
            
            if (tc == asTC_KEYWORD && token == "::") {
                stream.Advance(token);
                typeName += "::";
                tc = stream.Peek(token);
            } else break;
        }
    } else return "";

    if (stream.Peek(token) == asTC_KEYWORD && token == "<") {
        stream.Advance(token);
        typeName += "<";
        int templateDepth = 1;
        while (templateDepth > 0 && stream.Peek(token) != asTC_UNKNOWN) {
            stream.Advance(token);
            typeName += std::string(token);
            
            if (token == "<") templateDepth++;
            else if (token == ">") templateDepth--;
            else if (token == ">>") templateDepth -= 2;
            else if (token == ">>>") templateDepth -= 3;
        }
    }

    while (stream.Peek(token) == asTC_KEYWORD && token == "[") {
        stream.Advance(token);
        typeName += "[";
        if (stream.Peek(token) == asTC_KEYWORD && token == "]") {
            stream.Advance(token);
            typeName += "]";
        }
    }

    if (stream.Peek(token) == asTC_KEYWORD && token == "@") {
        stream.Advance(token);
        typeName += "@";
    }

    return typeName;
}

size_t GetAbsolutePosition(std::string_view text, int line, int character) {
    size_t pos = 0; int currentLine = 0;
    while (currentLine < line && pos < text.length()) {
        if (text[pos] == '\n') currentLine++;
        pos++;
    }
    return pos + character;
}

CompletionContext GetCompletionContext(asIScriptEngine* engine, std::string_view code, size_t cursorAbsolutePos) {
    CompletionContext ctx;
    ctx.isMemberAccess = false;
    ctx.partialMember = "";

    TokenStream stream(engine, code);
    std::string_view token;
    std::vector<std::string> tokens;
    
    while (stream.GetPos() < cursorAbsolutePos && stream.Peek(token) != asTC_UNKNOWN) {
        stream.Advance(token);
        tokens.push_back(std::string(token));
    }

    if (tokens.empty()) return ctx;

    int i = (int)tokens.size() - 1;
    
    // FIX: Tolerate a single colon ":" to trigger scope resolution autocompletion instantly
    if (tokens[i] == "." || tokens[i] == "::" || tokens[i] == ":") {
        ctx.isMemberAccess = true;
        i--; 
    } else if (i >= 1 && (tokens[i - 1] == "." || tokens[i - 1] == "::" || tokens[i - 1] == ":")) {
        ctx.isMemberAccess = true;
        ctx.partialMember = tokens[i];
        i -= 2; 
    }

    if (ctx.isMemberAccess) {
        std::vector<std::string> chain;
        while (i >= 0) {
            std::string fullIdent = "";
            
            while (i >= 0 && (tokens[i] == "]" || tokens[i] == ")")) {
                std::string closingToken = tokens[i];
                std::string openingToken = (closingToken == "]") ? "[" : "(";
                int depth = 0;
                std::string segment = "";
                do {
                    if (tokens[i] == closingToken) depth++;
                    else if (tokens[i] == openingToken) depth--;
                    segment = tokens[i] + segment;
                    i--;
                } while (i >= 0 && depth > 0);
                fullIdent = segment + fullIdent;
            }
            
            if (i >= 0 && tokens[i] != "." && tokens[i] != "::" && tokens[i] != ":") {
                fullIdent = tokens[i] + fullIdent; 
                chain.insert(chain.begin(), fullIdent);
                i--;
            } else if (!fullIdent.empty()) {
                chain.insert(chain.begin(), fullIdent);
            }
            
            if (i >= 0 && (tokens[i] == "." || tokens[i] == "::" || tokens[i] == ":")) i--;
            else break;
        }
        ctx.objectChain = chain;
    }

    return ctx;
}

// =========================================================================
// STATE MACHINE SCANNERS
// =========================================================================

/**
 * @class ASTScanner
 * @brief Internal base state machine for parsing classes and scopes from tokens.
 */
class ASTScanner {
protected:
    struct ScopeCtx { 
        std::string name; 
        std::string type; 
        int depth; 
    };
    
    asIScriptEngine* engine;
    std::string_view code;
    TokenStream stream;
    std::vector<std::string> knownClassNames;
    std::vector<ScopeCtx> scopeStack;

    std::string_view token;
    std::string pendingScopeType;
    std::string pendingScopeName;
    int currentDepth;

public:
    ASTScanner(asIScriptEngine* e, std::string_view c) 
        : engine(e), code(c), stream(e, c), currentDepth(0) {}

protected:
    std::string GetFullScope() const {
        std::string res;
        for (const auto& s : scopeStack) { 
            if (!res.empty()) res += "::"; 
            res += s.name; 
        }
        return res;
    }

    void SkipBlock() {
        int methodDepth = 1;
        while (methodDepth > 0 && stream.Peek(token) != asTC_UNKNOWN) {
            stream.Advance(token);
            if (token == "{") methodDepth++;
            else if (token == "}") methodDepth--;
        }
    }

    void CollectKnownTypes() {
        TokenStream quickStream(engine, code);
        std::vector<std::string> quickScope;
        int qDepth = 0;
        std::string qPendingName, qPendingType;
        
        while (quickStream.Peek(token) != asTC_UNKNOWN) {
            asETokenClass tc = quickStream.Advance(token);
            if (tc == asTC_KEYWORD && IsTypeDeclaration(token)) {
                qPendingType = std::string(token);
                if (quickStream.Peek(token) == asTC_IDENTIFIER) {
                    qPendingName = std::string(token);
                    quickStream.Advance(token);
                }
            } else if (tc == asTC_KEYWORD && token == "{") {
                qDepth++;
                if (!qPendingName.empty()) {
                    quickScope.push_back(qPendingName);
                    std::string fScope = "";
                    for(auto& s : quickScope) fScope += (fScope.empty() ? "" : "::") + s;
                    knownClassNames.push_back(fScope);
                    qPendingName = "";
                }
            } else if (tc == asTC_KEYWORD && token == "}") {
                if (!quickScope.empty()) quickScope.pop_back();
                qDepth--;
            }
        }
    }
};

/**
 * @class CustomClassScanner
 * @brief Extends ASTScanner to extract full definitions of user-defined types.
 */
class CustomClassScanner : public ASTScanner {
private:
    std::map<std::string, ScriptClass> classMap;
    std::string currentAccess;

public:
    CustomClassScanner(asIScriptEngine* e, std::string_view c) : ASTScanner(e, c), currentAccess("public") {
        CollectKnownTypes();
    }

    std::vector<ScriptClass> Scan() {
        while (stream.Peek(token) != asTC_UNKNOWN) {
            size_t savedPos = stream.GetPos();
            asETokenClass tc = stream.Advance(token);

            if (tc == asTC_KEYWORD && IsTypeDeclaration(token)) {
                pendingScopeType = std::string(token);
                if (stream.Peek(token) == asTC_IDENTIFIER) {
                    pendingScopeName = std::string(token);
                    stream.Advance(token);
                }
                continue;
            }

            if (tc == asTC_KEYWORD && token == "{") {
                currentDepth++;
                if (!pendingScopeName.empty()) {
                    scopeStack.push_back({pendingScopeName, pendingScopeType, currentDepth});
                    std::string fullScope = GetFullScope();
                    if (classMap.find(fullScope) == classMap.end()) {
                        classMap[fullScope] = ScriptClass{fullScope, {}, {}};
                    }
                    pendingScopeName = "";
                    pendingScopeType = "";
                    currentAccess = "public";
                }
                continue;
            }

            if (tc == asTC_KEYWORD && token == "}") {
                if (!scopeStack.empty() && scopeStack.back().depth == currentDepth) {
                    scopeStack.pop_back();
                    currentAccess = "public";
                }
                currentDepth--;
                continue;
            }

            if (tc == asTC_KEYWORD && IsAccessModifier(token)) {
                if (token != "shared") currentAccess = std::string(token);
                continue;
            }

            if (scopeStack.empty()) continue; 

            if (ProcessEnum(tc)) continue;

            bool validDeclFound = ProcessConstructorOrDestructor(tc);
            if (!validDeclFound) {
                validDeclFound = ProcessMember(savedPos);
            }

            if (validDeclFound) currentAccess = "public";
            else if (stream.GetPos() == savedPos) stream.Advance(token);
        }

        std::vector<ScriptClass> results;
        for (auto& pair : classMap) results.push_back(pair.second);
        return results;
    }

private:
    bool ProcessEnum(asETokenClass tc) {
        if (scopeStack.back().type != "enum") return false;
        if (tc == asTC_IDENTIFIER) {
            classMap[GetFullScope()].properties.push_back({std::string(token), "int", "public"});
        }
        return true;
    }

    bool ProcessConstructorOrDestructor(asETokenClass tc) {
        ScopeCtx& currentScope = scopeStack.back();
        std::string fullScope = GetFullScope();

        if (tc == asTC_IDENTIFIER && token == currentScope.name) {
            if (stream.Peek(token) == asTC_KEYWORD && token == "(") {
                classMap[fullScope].methods.push_back({currentScope.name, "void", fullScope + "()", currentAccess, true});
                SkipToBody();
                return true;
            }
        } 
        else if (tc == asTC_KEYWORD && token == "~") {
            if (stream.Peek(token) == asTC_IDENTIFIER && token == currentScope.name) {
                stream.Advance(token);
                if (stream.Peek(token) == asTC_KEYWORD && token == "(") {
                    classMap[fullScope].methods.push_back({"~" + currentScope.name, "void", "~" + fullScope + "()", currentAccess, true});
                    SkipToBody();
                    return true;
                }
            }
        }
        return false;
    }

    void SkipToBody() {
        while (stream.Peek(token) != asTC_UNKNOWN && token != "{" && token != ";") stream.Advance(token);
        if (stream.Peek(token) == asTC_KEYWORD && token == "{") {
            stream.Advance(token);
            SkipBlock();
        } else if (stream.Peek(token) == asTC_KEYWORD && token == ";") {
            stream.Advance(token);
        }
    }

    bool ProcessMember(size_t savedPos) {
        stream.SetPos(savedPos);
        std::string parsedType = ParseDataType(engine, stream, knownClassNames);
        if (parsedType.empty() || stream.Peek(token) != asTC_IDENTIFIER) {
            stream.SetPos(savedPos);
            return false;
        }

        std::string memberName = std::string(token);
        stream.Advance(token);
        asETokenClass lookTc = stream.Peek(token);

        if (lookTc == asTC_KEYWORD && token == "(") {
            ParseMethod(parsedType, memberName);
            return true;
        } else if (lookTc == asTC_KEYWORD && IsPropertySeparator(token)) {
            ParseProperty(parsedType, memberName);
            return true;
        }
        stream.SetPos(savedPos);
        return false;
    }

    void ParseMethod(const std::string& parsedType, const std::string& memberName) {    
        std::string fullScope = GetFullScope();
        size_t sigStart = stream.GetPos(); 
        int parenDepth = 0;
        
        do {
            stream.Advance(token);
            if (token == "(") parenDepth++;
            else if (token == ")") parenDepth--;
        } while (parenDepth > 0 && stream.Peek(token) != asTC_UNKNOWN);
        
        size_t sigEnd = stream.GetPos();
        std::string exactParams(code.data() + sigStart, sigEnd - sigStart);
        std::string signature = parsedType + " " + fullScope + "::" + memberName + exactParams;

        classMap[fullScope].methods.push_back({memberName, parsedType, signature, currentAccess, false});

        while (stream.Peek(token) == asTC_IDENTIFIER || (stream.Peek(token) == asTC_KEYWORD && IsMethodModifier(token))) {
            stream.Advance(token);
        }

        if (stream.Peek(token) == asTC_KEYWORD && token == "{") {
            stream.Advance(token);
            SkipBlock();
        } else if (stream.Peek(token) == asTC_KEYWORD && token == ";") {
            stream.Advance(token);
        }
    }

    void ParseProperty(const std::string& parsedType, const std::string& memberName) {
        std::string fullScope = GetFullScope();
        classMap[fullScope].properties.push_back({memberName, parsedType, currentAccess});

        while (true) {
            asETokenClass tcc = stream.Peek(token);
            if (tcc == asTC_UNKNOWN || (tcc == asTC_KEYWORD && (token == ";" || token == ")"))) {
                stream.Advance(token);
                break;
            }
            if (tcc == asTC_KEYWORD && token == ",") {
                stream.Advance(token);
                if (stream.Peek(token) == asTC_IDENTIFIER) {
                    std::string nextVar = std::string(token);
                    stream.Advance(token);
                    classMap[fullScope].properties.push_back({nextVar, parsedType, currentAccess});
                    continue;
                }
            } else stream.Advance(token);
        }
    }
};

std::vector<ScriptClass> ScanCustomClasses(asIScriptEngine* engine, std::string_view code) {
    CustomClassScanner scanner(engine, code);
    return scanner.Scan();
}

/**
 * @class LocalVariableScanner
 * @brief Extends ASTScanner to extract variables mapped to functional block scopes.
 */
/**
 * @class LocalVariableScanner
 * @brief Extends ASTScanner to extract variables mapped to functional block scopes, featuring type inference for the 'auto' keyword.
 */
/**
 * @class LocalVariableScanner
 * @brief Extends ASTScanner to extract variables mapped to functional block scopes, featuring type inference for the 'auto' keyword.
 */
class LocalVariableScanner : public ASTScanner {
private:
    std::vector<LocalVariable> locals;
    int parenDepth;
    std::string lastTokenStr;
    size_t cursorAbsolutePos;
    
    const std::vector<ScriptClass>& customClasses;
    const std::vector<GlobalVariable>& globalVars;
    const std::vector<GlobalFunction>& globalFuncs;

public:
    LocalVariableScanner(asIScriptEngine* e, std::string_view c, size_t pos, 
                         const std::vector<ScriptClass>& cClasses,
                         const std::vector<GlobalVariable>& gVars,
                         const std::vector<GlobalFunction>& gFuncs) 
        : ASTScanner(e, c), parenDepth(0), cursorAbsolutePos(pos), 
          customClasses(cClasses), globalVars(gVars), globalFuncs(gFuncs) {
        CollectKnownTypes();
    }

    std::vector<LocalVariable> Scan(std::string& outEnclosingClass) {
        while (stream.GetPos() < cursorAbsolutePos && stream.Peek(token) != asTC_UNKNOWN) {
            size_t savedPos = stream.GetPos();
            asETokenClass tc = stream.Peek(token);

            if (tc == asTC_KEYWORD && token == "(") parenDepth++;

            if (tc == asTC_KEYWORD && IsTypeDeclaration(token)) {
                pendingScopeType = std::string(token);
                stream.Advance(token); 
                lastTokenStr = pendingScopeType;
                if (stream.Peek(token) == asTC_IDENTIFIER) {
                    pendingScopeName = std::string(token);
                    stream.Advance(token);
                    lastTokenStr = pendingScopeName;
                }
                continue;
            }

            if (tc == asTC_KEYWORD && token == "{") {
                currentDepth++;
                if (!pendingScopeName.empty()) {
                    scopeStack.push_back({pendingScopeName, pendingScopeType, currentDepth});
                    pendingScopeName = "";
                    pendingScopeType = "";
                }
                stream.Advance(token);
                lastTokenStr = "{";
                continue;
            }
            
            if (tc == asTC_KEYWORD && token == "}") {
                if (!scopeStack.empty() && scopeStack.back().depth == currentDepth) {
                    scopeStack.pop_back();
                }
                currentDepth--;
                
                int captureDepth = currentDepth;
                locals.erase(std::remove_if(locals.begin(), locals.end(),
                    [captureDepth](const LocalVariable& v) { return v.declarationDepth > captureDepth; }), locals.end());
                    
                stream.Advance(token);
                lastTokenStr = "}";
                continue;
            }

            while (tc == asTC_KEYWORD && IsAccessModifier(token)) {
                stream.Advance(token);
                lastTokenStr = std::string(token);
                tc = stream.Peek(token);
            }

            bool validDeclFound = false;

            if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::") {
                validDeclFound = ProcessLocalDeclaration(savedPos);
            }

            if (!validDeclFound && stream.GetPos() == savedPos) {
                stream.Advance(token);
                if (token == "(") parenDepth++;
                else if (token == ")") { if (parenDepth > 0) parenDepth--; }
                lastTokenStr = std::string(token);
            }
        }
        
        if (!scopeStack.empty() && scopeStack.back().type == "class") {
            outEnclosingClass = GetFullScope();
            locals.push_back({"this", outEnclosingClass + "@", 1});
        }

        return locals;
    }

private:
    /**
     * @brief Acts as a mini-evaluator, executing assignment expressions contextually to deduce the true type of 'auto' variables.
     */
    std::string InferAutoType(const std::vector<std::string>& tokens) {
        if (tokens.empty()) return "auto";
        if (tokens[0][0] == '"' || tokens[0][0] == '\'') return "string";
        if (tokens[0] == "true" || tokens[0] == "false") return "bool";
        if (tokens[0].find_first_of(".f") != std::string::npos && isdigit(tokens[0][0])) return "float";
        if (isdigit(tokens[0][0])) return "int";

        std::string inferredType = "";
        std::string currentEnclosingClass = GetFullScope();
        int tempBracketDepth = 0;
        int tempParenDepth = 0;

        for (size_t i = 0; i < tokens.size(); i++) {
            std::string tok = tokens[i];
            
            // Manage internal evaluation depths
            if (tok == "[") {
                if (tempBracketDepth == 0) inferredType = ExtractInnerType(inferredType);
                tempBracketDepth++;
                continue;
            }
            if (tok == "]") { tempBracketDepth--; continue; }
            if (tok == "(") { tempParenDepth++; continue; }
            if (tok == ")") { tempParenDepth--; continue; }

            // Ignore tokens inside indexers or method parameters (e.g., 'i' inside [i])
            if (tempBracketDepth > 0 || tempParenDepth > 0) continue; 
            
            // Ignore syntax operators
            if (tok == "." || tok == "::" || tok == "new" || tok == "@") continue;
            if (!isalpha(tok[0]) && tok[0] != '_') continue; 

            if (inferredType.empty()) {
                if (tok == "this") {
                    inferredType = currentEnclosingClass;
                } else {
                    // Check implicit 'this' properties first
                    if (!currentEnclosingClass.empty()) {
                        for (const auto& c : customClasses) {
                            if (c.name == currentEnclosingClass) {
                                for (const auto& p : c.properties) if (p.name == tok) { inferredType = p.typeName; break; }
                                if (inferredType.empty()) for (const auto& m : c.methods) if (m.name == tok) { inferredType = m.typeName; break; }
                                break;
                            }
                        }
                    }
                    if (inferredType.empty()) for (const auto& v : locals) if (v.name == tok) { inferredType = v.typeName; break; }
                    if (inferredType.empty()) for (const auto& v : globalVars) if (v.name == tok) { inferredType = v.typeName; break; }
                    if (inferredType.empty()) for (const auto& c : customClasses) if (c.name == tok) { inferredType = tok; break; }
                    if (inferredType.empty()) for (const auto& f : globalFuncs) if (f.name == tok) { inferredType = f.typeName; break; }
                    
                    if (inferredType.empty() && engine) {
                        for (asUINT g = 0; g < engine->GetGlobalPropertyCount(); g++) {
                            const char* vName = nullptr; int tId = 0;
                            engine->GetGlobalPropertyByIndex(g, &vName, nullptr, &tId, nullptr);
                            if (vName && std::string(vName) == tok) {
                                const char* decl = engine->GetTypeDeclaration(tId, true);
                                if (decl) inferredType = decl;
                            }
                        }
                    }
                }
            } else {
                std::string baseType = GetBaseType(inferredType);
                bool found = false;
                
                for (const auto& c : customClasses) {
                    if (c.name == baseType) {
                        for (const auto& p : c.properties) if (p.name == tok) { inferredType = p.typeName; found = true; break; }
                        if (!found) for (const auto& m : c.methods) if (m.name == tok) { inferredType = m.typeName; found = true; break; }
                        break;
                    }
                }
                
                if (!found && engine) {
                    asITypeInfo* t = engine->GetTypeInfoByName(baseType.c_str());
                    if (t) {
                        for (asUINT p = 0; p < t->GetPropertyCount(); p++) {
                            const char* pName = nullptr; int pTypeId = 0;
                            t->GetProperty(p, &pName, &pTypeId);
                            if (pName && std::string(pName) == tok) {
                                const char* decl = engine->GetTypeDeclaration(pTypeId, true);
                                if (decl) { inferredType = decl; found = true; break; }
                            }
                        }
                        if (!found) {
                            for (asUINT m = 0; m < t->GetMethodCount(); m++) {
                                asIScriptFunction* func = t->GetMethodByIndex(m);
                                if (func && std::string(func->GetName()) == tok) {
                                    int rTypeId = func->GetReturnTypeId();
                                    const char* decl = engine->GetTypeDeclaration(rTypeId, true);
                                    if (decl) { inferredType = decl; found = true; break; }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        if (!inferredType.empty()) {
            inferredType.erase(std::remove(inferredType.begin(), inferredType.end(), '@'), inferredType.end());
            inferredType.erase(std::remove(inferredType.begin(), inferredType.end(), '&'), inferredType.end());
            return inferredType;
        }
        return "auto";
    }

    bool ProcessLocalDeclaration(size_t savedPos) {
        stream.SetPos(savedPos);
        std::string parsedType = ParseDataType(engine, stream, knownClassNames);
        
        if (parsedType.empty() || stream.Peek(token) != asTC_IDENTIFIER) {
            stream.SetPos(savedPos);
            return false;
        }

        std::string varName = std::string(token);
        stream.Advance(token); 
        asETokenClass tc = stream.Peek(token);

        if (tc == asTC_KEYWORD && (token == ";" || token == "=" || token == "," || token == ")")) {
            bool isLocal = false;
            int effectiveDepth = currentDepth;
            
            if (parenDepth > 0) effectiveDepth++; 
            
            if (scopeStack.empty()) { 
                if (effectiveDepth >= 1) isLocal = true; 
            } else {
                if (effectiveDepth > scopeStack.back().depth) isLocal = true;
            }

            if (isLocal) {
                auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable& v){ return v.name == varName; });
                if (it == locals.end()) locals.push_back({varName, parsedType, effectiveDepth});

                ParseAssignments(parsedType, effectiveDepth, varName);
                return true;
            }
        }
        stream.SetPos(savedPos);
        return false;
    }

    void ParseAssignments(std::string parsedType, int effectiveDepth, std::string currentVarName) {
        bool isAssigning = false;
        std::vector<std::string> expressionTokens;

        while (true) {
            asETokenClass lookTc = stream.Peek(token);

            if (lookTc == asTC_UNKNOWN || (lookTc == asTC_KEYWORD && (token == ";" || token == ")"))) {
                // Execute accumulated expression to resolve 'auto' type
                if (isAssigning && parsedType == "auto" && !expressionTokens.empty()) {
                    std::string inferred = InferAutoType(expressionTokens);
                    if (inferred != "auto") {
                        parsedType = inferred;
                        auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable& v){ return v.name == currentVarName; });
                        if (it != locals.end()) it->typeName = parsedType;
                    }
                }

                stream.Advance(token);
                if (token == ")") {
                    if (parenDepth > 0) parenDepth--;
                    lastTokenStr = ")";
                } else lastTokenStr = ";";
                break;
            }

            if (lookTc == asTC_KEYWORD && token == "=") {
                stream.Advance(token);
                lastTokenStr = "=";
                isAssigning = true;
                expressionTokens.clear();
                continue;
            }

            if (lookTc == asTC_KEYWORD && token == ",") {
                if (isAssigning && parsedType == "auto" && !expressionTokens.empty()) {
                    std::string inferred = InferAutoType(expressionTokens);
                    if (inferred != "auto") {
                        parsedType = inferred;
                        auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable& v){ return v.name == currentVarName; });
                        if (it != locals.end()) it->typeName = parsedType;
                    }
                }

                stream.Advance(token); 
                lastTokenStr = ",";
                isAssigning = false;
                expressionTokens.clear();
                
                stream.Peek(token);
                if (IsValidDataType(engine, stream, token, knownClassNames)) break; 
                
                if (stream.Peek(token) == asTC_IDENTIFIER) {
                    std::string nextVar = std::string(token);
                    stream.Advance(token);
                    lastTokenStr = nextVar;
                    currentVarName = nextVar;
                    
                    auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable& v){ return v.name == nextVar; });
                    if (it == locals.end()) locals.push_back({nextVar, parsedType, effectiveDepth});
                    continue;
                }
            } else {
                stream.Advance(token);
                if (isAssigning && lookTc != asTC_WHITESPACE && lookTc != asTC_COMMENT) {
                    expressionTokens.push_back(std::string(token));
                }
                
                if (token == "(") parenDepth++;
                else if (token == ")") { if (parenDepth > 0) parenDepth--; }
                lastTokenStr = std::string(token);
            }
        }
    }
};

std::vector<LocalVariable> ScanLocalVariables(
    asIScriptEngine* engine, std::string_view code, size_t cursorAbsolutePos, std::string& outEnclosingClass,
    const std::vector<ScriptClass>& customClasses, const std::vector<GlobalVariable>& globalVars, const std::vector<GlobalFunction>& globalFuncs) {
    
    LocalVariableScanner scanner(engine, code, cursorAbsolutePos, customClasses, globalVars, globalFuncs);
    return scanner.Scan(outEnclosingClass);
}

std::vector<GlobalFunction> ScanGlobalFunctions(asIScriptEngine* engine, std::string_view code) {
    std::vector<GlobalFunction> funcs;
    std::vector<ScriptClass> customClasses = ScanCustomClasses(engine, code);
    std::vector<std::string> localClasses;
    for (const auto& c : customClasses) localClasses.push_back(c.name);

    TokenStream stream(engine, code);
    int currentDepth = 0;
    std::string_view token;
    std::string lastTokenStr = "";

    while (stream.Peek(token) != asTC_UNKNOWN) {
        size_t savedPos = stream.GetPos();
        asETokenClass tc = stream.Peek(token);

        if (tc == asTC_KEYWORD) {
            if (token == "{") { currentDepth++; stream.Advance(token); lastTokenStr = "{"; continue; }
            if (token == "}") { currentDepth--; stream.Advance(token); lastTokenStr = "}"; continue; }
        }

        while (tc == asTC_KEYWORD && IsGlobalModifier(token)) {
            stream.Advance(token);
            lastTokenStr = std::string(token);
            tc = stream.Peek(token);
        }

        bool validDeclFound = false;

        if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::") {
            size_t beforeTypePos = stream.GetPos();
            std::string parsedType = ParseDataType(engine, stream, localClasses);
            
            if (!parsedType.empty() && stream.Peek(token) == asTC_IDENTIFIER) {
                std::string funcName = std::string(token);
                stream.Advance(token);

                if (stream.Peek(token) == asTC_KEYWORD && token == "(") {
                    validDeclFound = true;
                    if (currentDepth == 0) { 
                        std::string decl = fmt::format("{} {}()", parsedType, funcName);
                        auto it = std::find_if(funcs.begin(), funcs.end(), [&](const GlobalFunction& f){ return f.name == funcName; });
                        if (it == funcs.end()) funcs.push_back({funcName, parsedType, decl});
                    }
                }
            }
            if (!validDeclFound) stream.SetPos(beforeTypePos);
        }

        if (!validDeclFound && stream.GetPos() == savedPos) {
            stream.Advance(token);
            lastTokenStr = std::string(token);
        }
    }
    return funcs;
}

std::vector<GlobalVariable> ScanGlobalVariables(asIScriptEngine* engine, std::string_view code) {
    std::vector<GlobalVariable> vars;
    std::vector<ScriptClass> customClasses = ScanCustomClasses(engine, code);
    std::vector<std::string> localClasses;
    for (const auto& c : customClasses) localClasses.push_back(c.name);

    TokenStream stream(engine, code);
    int currentDepth = 0;
    std::string_view token;
    std::string lastTokenStr = "";

    while (stream.Peek(token) != asTC_UNKNOWN) {
        size_t savedPos = stream.GetPos();
        asETokenClass tc = stream.Peek(token);

        if (tc == asTC_KEYWORD) {
            if (token == "{") { currentDepth++; stream.Advance(token); lastTokenStr = "{"; continue; }
            if (token == "}") { currentDepth--; stream.Advance(token); lastTokenStr = "}"; continue; }
        }

        while (tc == asTC_KEYWORD && IsGlobalModifier(token)) {
            stream.Advance(token);
            lastTokenStr = std::string(token);
            tc = stream.Peek(token);
        }

        bool validDeclFound = false;

        if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::") {
            size_t beforeTypePos = stream.GetPos();
            std::string parsedType = ParseDataType(engine, stream, localClasses);
            
            if (!parsedType.empty() && stream.Peek(token) == asTC_IDENTIFIER) {
                std::string varName = std::string(token);
                stream.Advance(token);

                tc = stream.Peek(token);
                if (tc == asTC_KEYWORD && IsPropertySeparator(token)) {
                    validDeclFound = true;
                    if (currentDepth == 0) { 
                        auto it = std::find_if(vars.begin(), vars.end(), [&](const GlobalVariable& v){ return v.name == varName; });
                        if (it == vars.end()) vars.push_back({varName, parsedType});

                        while (true) {
                            asETokenClass lookTc = stream.Peek(token);
                            if (lookTc == asTC_UNKNOWN || (lookTc == asTC_KEYWORD && token == ";")) break;
                            
                            if (lookTc == asTC_KEYWORD && token == ",") {
                                stream.Advance(token);
                                lastTokenStr = ",";
                                if (stream.Peek(token) == asTC_IDENTIFIER) {
                                    std::string nextVar = std::string(token);
                                    stream.Advance(token);
                                    lastTokenStr = nextVar;
                                    
                                    it = std::find_if(vars.begin(), vars.end(), [&](const GlobalVariable& v){ return v.name == nextVar; });
                                    if (it == vars.end()) vars.push_back({nextVar, parsedType});
                                    continue;
                                }
                            } else {
                                stream.Advance(token);
                                lastTokenStr = std::string(token);
                            }
                        }
                    }
                }
            }
            if (!validDeclFound) stream.SetPos(beforeTypePos);
        }

        if (!validDeclFound && stream.GetPos() == savedPos) {
            stream.Advance(token);
            lastTokenStr = std::string(token);
        }
    }
    return vars;
}

} // namespace TokenHarvester