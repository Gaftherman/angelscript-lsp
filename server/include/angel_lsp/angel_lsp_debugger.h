#pragma once

/**
 * @file angel_lsp_debugger.h
 * @brief Embeddable C++20 Debug Adapter Protocol (DAP) server and runtime debugger for AngelScript.
 *
 * AngelLSP Debugger Engine - Provides full runtime inspection, line-by-line stepping,
 * variable inspection, exception interception, and expression evaluation for AngelScript hosts and game engines.
 *
 * Compatible with AngelScript 2.20 through 2.38+.
 *
 * Zero external dependencies beyond standard C++20 and the AngelScript C++ API.
 */

#if __has_include(<angelscript.h>)
#include <angelscript.h>
#define ANGEL_LSP_HAS_ANGELSCRIPT 1
#else
#define ANGEL_LSP_HAS_ANGELSCRIPT 0
class asIScriptEngine;
class asIScriptContext;
class asIScriptFunction;
class asIScriptModule;
class asITypeInfo;
using asUINT = unsigned int;
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace angel_lsp::debug
{

/**
 * @brief Current execution state of the debugged script context.
 */
enum class ExecutionState : uint8_t
{
    Running,
    Suspended,
    StepOver,
    StepIn,
    StepOut
};

/**
 * @brief Represents a single stack frame in the active call hierarchy.
 */
struct StackFrameInfo
{
    uint32_t id = 0;
    std::string functionName;
    std::string scriptSection;
    int line = 0;
    int column = 0;
};

/**
 * @brief Represents an inspected variable or property in a frame or scope.
 */
struct VariableInfo
{
    std::string name;
    std::string value;
    std::string typeName;
    int typeId = 0;
    uint32_t variablesReference = 0;
};

/**
 * @brief Embeddable AngelScript debugger supporting DAP line callbacks and variable introspection.
 */
class Debugger
{
  public:
    /**
     * @brief Constructs an unattached debugger instance.
     */
    Debugger() = default;

    /**
     * @brief Destructs the debugger, ensuring clean socket and state teardown.
     */
    virtual ~Debugger()
    {
        Detach();
    }

    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;
    Debugger(Debugger&&) = delete;
    Debugger& operator=(Debugger&&) = delete;

    /**
     * @brief Attaches to an active AngelScript engine.
     * @param[in] engine Target AngelScript script engine.
     */
    void Attach(asIScriptEngine* engine)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_engine = engine;
    }

    /**
     * @brief Detaches the debugger and resumes any suspended contexts.
     */
    void Detach()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = ExecutionState::Running;
        m_engine = nullptr;
    }

    /**
     * @brief Sets a line breakpoint for a specific script section or file.
     * @param[in] section Name of the script section or file path.
     * @param[in] line 1-based or 0-based source line.
     */
    void SetBreakpoint(const std::string& section, int line)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_breakpoints[NormalizeSection(section)].insert(line);
    }

    /**
     * @brief Clears a line breakpoint.
     * @param[in] section Name of the script section or file path.
     * @param[in] line Source line to clear.
     */
    void ClearBreakpoint(const std::string& section, int line)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_breakpoints.find(NormalizeSection(section));
        if (it != m_breakpoints.end())
        {
            it->second.erase(line);
        }
    }

    /**
     * @brief Clears all registered breakpoints across all sections.
     */
    void ClearAllBreakpoints()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_breakpoints.clear();
    }

    /**
     * @brief Requests context execution resumption.
     */
    void Continue()
    {
        m_state.store(ExecutionState::Running, std::memory_order_release);
    }

    /**
     * @brief Steps over the current line in the active context.
     * @param[in] ctx Active script context.
     */
    void StepOver(asIScriptContext* ctx)
    {
        if (ctx)
        {
            m_stepStackDepth = ctx->GetCallstackSize();
            m_state.store(ExecutionState::StepOver, std::memory_order_release);
        }
    }

    /**
     * @brief Steps into the next statement or function call.
     */
    void StepIn()
    {
        m_state.store(ExecutionState::StepIn, std::memory_order_release);
    }

    /**
     * @brief Steps out of the current function.
     * @param[in] ctx Active script context.
     */
    void StepOut(asIScriptContext* ctx)
    {
        if (ctx && ctx->GetCallstackSize() > 0)
        {
            m_stepStackDepth = ctx->GetCallstackSize() - 1;
            m_state.store(ExecutionState::StepOut, std::memory_order_release);
        }
    }

    /**
     * @brief Line callback invoked by AngelScript during execution.
     *
     * Wire into your context via:
     * `ctx->SetLineCallback(asMETHOD(Debugger, LineCallback), debuggerInstance, asCALL_THISCALL);`
     *
     * @param[in] ctx Script context executing the current line.
     */
    void LineCallback(asIScriptContext* ctx)
    {
        if (!ctx)
        {
            return;
        }

        int col = 0;
        const char* sec = nullptr;
        int line = ctx->GetLineNumber(0, &col, &sec);
        std::string section = sec ? sec : "";

        if (ShouldSuspend(ctx, section, line))
        {
            m_state.store(ExecutionState::Suspended, std::memory_order_release);
            OnSuspend(ctx, section, line, col);

            while (m_state.load(std::memory_order_acquire) == ExecutionState::Suspended)
            {
#if defined(_WIN32)
                Sleep(10);
#else
                usleep(10000);
#endif
            }
        }
    }

    /**
     * @brief Exception callback invoked by AngelScript when a runtime script exception occurs.
     *
     * Wire into your context via:
     * `ctx->SetExceptionCallback(asMETHOD(Debugger, ExceptionCallback), debuggerInstance, asCALL_THISCALL);`
     *
     * @param[in] ctx Script context where the exception occurred.
     */
    /**
     * @brief Toggles breaking on runtime script exceptions.
     * @param[in] enable True to suspend when an exception is thrown.
     */
    void SetBreakOnException(bool enable) noexcept
    {
        m_breakOnException = enable;
    }

    /**
     * @brief Returns whether exception interception is active.
     */
    bool GetBreakOnException() const noexcept
    {
        return m_breakOnException;
    }

    void ExceptionCallback(asIScriptContext* ctx)
    {
        if (!ctx || !m_breakOnException)
        {
            return;
        }

        int col = 0;
        const char* sec = nullptr;
        int line = ctx->GetExceptionLineNumber(&col, &sec);
        std::string section = sec ? sec : "";
        const char* desc = ctx->GetExceptionString();
        std::string exceptionDesc = desc ? desc : "";

        m_state.store(ExecutionState::Suspended, std::memory_order_release);
        OnException(ctx, section, line, col, exceptionDesc);

        while (m_state.load(std::memory_order_acquire) == ExecutionState::Suspended)
        {
#if defined(_WIN32)
            Sleep(10);
#else
            usleep(10000);
#endif
        }
    }

    /**
     * @brief Validates or snaps a requested line to the nearest executable line in a section.
     * @param[in] section Name of the script section.
     * @param[in] requestedLine 1-based source line.
     * @return Validated or snapped line number.
     */
    int ValidateBreakpointLine([[maybe_unused]] const std::string& section, int requestedLine) const
    {
#if defined(ANGELSCRIPT_VERSION) && ANGELSCRIPT_VERSION >= 23800
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_engine)
        {
            return requestedLine;
        }
        const asUINT modCount = m_engine->GetModuleCount();
        for (asUINT m = 0; m < modCount; ++m)
        {
            asIScriptModule* mod = m_engine->GetModuleByIndex(m);
            if (!mod)
                continue;
            const asUINT funcCount = mod->GetFunctionCount();
            for (asUINT f = 0; f < funcCount; ++f)
            {
                asIScriptFunction* func = mod->GetFunctionByIndex(f);
                if (!func)
                    continue;
                const int entryCount = func->GetLineEntryCount();
                for (int e = 0; e < entryCount; ++e)
                {
                    const char* sec = nullptr;
                    int row = 0;
                    func->GetLineEntry(static_cast<asUINT>(e), &row, nullptr, &sec, nullptr);
                    if (sec && NormalizeSection(sec) == NormalizeSection(section) && row >= requestedLine)
                    {
                        return row;
                    }
                }
            }
        }
#endif
        return requestedLine;
    }

    /**
     * @brief Collects the full call stack from an active script context.
     * @param[in] ctx Active script context.
     * @return Ordered list of stack frames, innermost frame first.
     */
    std::vector<StackFrameInfo> GetCallStack(asIScriptContext* ctx) const
    {
        std::vector<StackFrameInfo> frames;
        if (!ctx)
        {
            return frames;
        }

        const asUINT stackSize = ctx->GetCallstackSize();
        frames.reserve(stackSize);

        for (asUINT i = 0; i < stackSize; ++i)
        {
            asIScriptFunction* func = ctx->GetFunction(i);
            int col = 0;
            const char* section = nullptr;
            int line = ctx->GetLineNumber(i, &col, &section);

            StackFrameInfo frame;
            frame.id = static_cast<uint32_t>(i);
            frame.functionName = func ? func->GetName() : "<unknown>";
            frame.scriptSection = section ? section : "";
            frame.line = line;
            frame.column = col;
            frames.push_back(std::move(frame));
        }

        return frames;
    }

    /**
     * @brief Collects local variables at a given stack frame level.
     * @param[in] ctx Active script context.
     * @param[in] stackLevel 0-based frame index (0 = topmost frame).
     * @return Formatted variable descriptions for locals in the frame.
     */
    std::vector<VariableInfo> GetLocalVariables(asIScriptContext* ctx, asUINT stackLevel = 0) const
    {
        std::vector<VariableInfo> locals;
        if (!ctx || stackLevel >= ctx->GetCallstackSize())
        {
            return locals;
        }

        asIScriptEngine* engine = ctx->GetEngine();
        const int varCount = ctx->GetVarCount(stackLevel);

        for (int i = 0; i < varCount; ++i)
        {
            const char* name = nullptr;
            int typeId = 0;
            ctx->GetVar(static_cast<asUINT>(i), stackLevel, &name, &typeId);
            void* address = ctx->GetAddressOfVar(static_cast<asUINT>(i), stackLevel);

            VariableInfo info;
            info.name = name ? name : "";
            info.typeId = typeId;
            info.typeName = engine ? FormatTypeName(engine, typeId) : "";
            info.value = FormatValue(engine, address, typeId);
            locals.push_back(std::move(info));
        }

        return locals;
    }

    /**
     * @brief Collects member properties of the `this` object at a given stack level.
     * @param[in] ctx Active script context.
     * @param[in] stackLevel 0-based frame index.
     * @return Member variables of the active `this` instance.
     */
    std::vector<VariableInfo> GetThisProperties(asIScriptContext* ctx, asUINT stackLevel = 0) const
    {
        std::vector<VariableInfo> props;
        if (!ctx || stackLevel >= ctx->GetCallstackSize())
        {
            return props;
        }

        void* thisPtr = ctx->GetThisPointer(stackLevel);
        const int thisTypeId = ctx->GetThisTypeId(stackLevel);
        if (!thisPtr || thisTypeId == 0)
        {
            return props;
        }

        return GetChildProperties(ctx->GetEngine(), thisPtr, thisTypeId);
    }

    /**
     * @brief Collects global variables from the active module.
     * @param[in] ctx Active script context.
     * @return Formatted variable descriptions for globals in the active module.
     */
    std::vector<VariableInfo> GetGlobalVariables(asIScriptContext* ctx) const
    {
        std::vector<VariableInfo> globals;
        if (!ctx || ctx->GetCallstackSize() == 0)
        {
            return globals;
        }

        asIScriptFunction* func = ctx->GetFunction(0);
        if (!func)
        {
            return globals;
        }

        asIScriptModule* module = func->GetModule();
        if (!module)
        {
            return globals;
        }

        asIScriptEngine* engine = ctx->GetEngine();
        const asUINT varCount = module->GetGlobalVarCount();
        globals.reserve(varCount);

        for (asUINT i = 0; i < varCount; ++i)
        {
            const char* name = nullptr;
            const char* ns = nullptr;
            int typeId = 0;
            module->GetGlobalVar(i, &name, &ns, &typeId, nullptr);
            void* address = module->GetAddressOfGlobalVar(i);

            VariableInfo info;
            std::string fullName;
            if (ns && ns[0] != '\0')
            {
                fullName = std::string(ns) + "::";
            }
            fullName += (name ? name : "");
            info.name = std::move(fullName);
            info.typeId = typeId;
            info.typeName = engine ? FormatTypeName(engine, typeId) : "";
            info.value = FormatValue(engine, address, typeId);
            globals.push_back(std::move(info));
        }

        return globals;
    }

    /**
     * @brief Collects child properties of an object instance or handle.
     * @param[in] engine Script engine instance.
     * @param[in] ptr Pointer to the object or handle.
     * @param[in] typeId Type ID of the object.
     * @return List of child properties.
     */
    /**
     * @brief Evaluates an expression against the current context scope (locals, member properties, and globals).
     * @param[in] ctx Active script context.
     * @param[in] expression Variable or property expression name.
     * @param[in] stackLevel Frame stack level (0 = topmost frame).
     * @return Formatted variable info if resolved, or std::nullopt.
     */
    std::optional<VariableInfo> EvaluateExpression(asIScriptContext* ctx, const std::string& expression,
                                                   asUINT stackLevel = 0) const
    {
        if (!ctx || expression.empty() || stackLevel >= ctx->GetCallstackSize())
        {
            return std::nullopt;
        }

        std::string expr = expression;
        if (expr.rfind("this.", 0) == 0)
        {
            expr = expr.substr(5);
        }

        // 1. Search local variables
        const auto locals = GetLocalVariables(ctx, stackLevel);
        for (const auto& var : locals)
        {
            if (var.name == expr)
            {
                return var;
            }
        }

        // 2. Search this properties
        const auto thisProps = GetThisProperties(ctx, stackLevel);
        for (const auto& prop : thisProps)
        {
            if (prop.name == expr)
            {
                return prop;
            }
        }

        // 3. Search global variables
        const auto globals = GetGlobalVariables(ctx);
        for (const auto& glob : globals)
        {
            if (glob.name == expr)
            {
                return glob;
            }
        }

        return std::nullopt;
    }

    /**
     * @brief Frames a JSON string as a DAP TCP packet (Content-Length header).
     * @param[in] json Message JSON content.
     * @return Formatted DAP packet.
     */
    static std::string FrameDapMessage(const std::string& json)
    {
        return "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    }

    /**
     * @brief Attempts to extract a complete DAP JSON message from a stream buffer.
     * @param[in,out] buffer Accumulator buffer containing received stream bytes.
     * @return Complete JSON payload if available, or std::nullopt.
     */
    static std::optional<std::string> UnframeDapMessage(std::string& buffer)
    {
        const std::string delimiter = "\r\n\r\n";
        const size_t headerEnd = buffer.find(delimiter);
        if (headerEnd == std::string::npos)
        {
            return std::nullopt;
        }

        const std::string header = buffer.substr(0, headerEnd);
        const std::string clKey = "Content-Length:";
        size_t clPos = header.find(clKey);
        if (clPos == std::string::npos)
        {
            clPos = header.find("content-length:");
        }
        if (clPos == std::string::npos)
        {
            buffer.erase(0, headerEnd + delimiter.size());
            return std::nullopt;
        }

        clPos += clKey.size();
        while (clPos < header.size() && (header[clPos] == ' ' || header[clPos] == '\t'))
        {
            ++clPos;
        }
        const size_t length = static_cast<size_t>(std::strtoul(header.c_str() + clPos, nullptr, 10));
        const size_t bodyStart = headerEnd + delimiter.size();

        if (buffer.size() < bodyStart + length)
        {
            return std::nullopt;
        }

        std::string json = buffer.substr(bodyStart, length);
        buffer.erase(0, bodyStart + length);
        return json;
    }

    /**
     * @brief Formats a DAP event JSON envelope.
     * @param[in] eventName Name of the event (e.g. 'stopped', 'initialized', 'terminated').
     * @param[in] bodyJson Formatted body payload JSON.
     * @param[in] seq Sequence number.
     * @return Formatted DAP event JSON string.
     */
    static std::string FormatDapEvent(const std::string& eventName, const std::string& bodyJson = "", int seq = 0)
    {
        std::ostringstream ss;
        ss << "{\"seq\":" << seq << ",\"type\":\"event\",\"event\":\"" << eventName << "\"";
        if (!bodyJson.empty())
        {
            ss << ",\"body\":" << bodyJson;
        }
        ss << "}";
        return ss.str();
    }

    /**
     * @brief Dispatches and handles a DAP JSON request, producing a conforming DAP JSON response.
     * @param[in] requestJson Raw JSON request string from the client.
     * @param[in] activeCtx Active script context if currently suspended.
     * @return Formatted DAP JSON response string.
     */
    std::string HandleDapRequest(const std::string& requestJson, asIScriptContext* activeCtx = nullptr)
    {
        const int seq = ExtractJsonInt(requestJson, "\"seq\"", 1);
        const std::string command = ExtractJsonString(requestJson, "\"command\"");

        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq << ",\"command\":\""
           << command << "\",\"success\":true";

        if (command == "initialize")
        {
            ss << ",\"body\":{\"supportsConfigurationDoneRequest\":true,"
               << "\"supportsEvaluateForHovers\":true,"
               << "\"supportsExceptionInfoRequest\":true,"
               << "\"supportsConditionalBreakpoints\":false,"
               << "\"supportsHitConditionalBreakpoints\":false}";
        }
        else if (command == "attach" || command == "launch")
        {
            // Successfully acknowledged
        }
        else if (command == "setBreakpoints")
        {
            std::string path = ExtractJsonString(requestJson, "\"path\"");
            if (path.empty())
            {
                path = ExtractJsonString(requestJson, "\"name\"");
            }
            ClearBreakpoint(path, 0); // clear existing breakpoints for section if re-sending
            m_breakpoints.erase(NormalizeSection(path));

            // Extract lines
            std::vector<int> lines;
            size_t pos = 0;
            while ((pos = requestJson.find("\"line\"", pos)) != std::string::npos)
            {
                pos += 6;
                while (pos < requestJson.size() && (requestJson[pos] == ':' || requestJson[pos] == ' '))
                {
                    ++pos;
                }
                int l = std::atoi(requestJson.c_str() + pos);
                if (l > 0)
                {
                    lines.push_back(l);
                }
            }

            ss << ",\"body\":{\"breakpoints\":[";
            for (size_t i = 0; i < lines.size(); ++i)
            {
                int validLine = ValidateBreakpointLine(path, lines[i]);
                SetBreakpoint(path, validLine);
                ss << "{\"verified\":true,\"line\":" << validLine << ",\"source\":{\"path\":\"" << path << "\"}}"
                   << (i + 1 < lines.size() ? "," : "");
            }
            ss << "]}";
        }
        else if (command == "setExceptionBreakpoints")
        {
            bool breakOnAll = (requestJson.find("\"all\"") != std::string::npos ||
                               requestJson.find("\"uncaught\"") != std::string::npos);
            SetBreakOnException(breakOnAll);
        }
        else if (command == "configurationDone")
        {
            // Ready for execution
        }
        else if (command == "threads")
        {
            ss << ",\"body\":{\"threads\":[{\"id\":1,\"name\":\"AngelScript Main Thread\"}]}";
        }
        else if (command == "stackTrace")
        {
            ss << ",\"body\":{\"stackFrames\":[";
            if (activeCtx)
            {
                const auto frames = GetCallStack(activeCtx);
                for (size_t i = 0; i < frames.size(); ++i)
                {
                    const auto& f = frames[i];
                    ss << "{\"id\":" << f.id << ",\"name\":\"" << f.functionName << "\",\"line\":" << f.line
                       << ",\"column\":" << f.column << ",\"source\":{\"path\":\"" << f.scriptSection << "\"}}"
                       << (i + 1 < frames.size() ? "," : "");
                }
                ss << "],\"totalFrames\":" << frames.size() << "}";
            }
            else
            {
                ss << "],\"totalFrames\":0}";
            }
        }
        else if (command == "scopes")
        {
            ss << ",\"body\":{\"scopes\":["
               << "{\"name\":\"Locals\",\"variablesReference\":1000,\"expensive\":false},"
               << "{\"name\":\"Globals\",\"variablesReference\":2000,\"expensive\":true}]}";
        }
        else if (command == "variables")
        {
            const int varRef = ExtractJsonInt(requestJson, "\"variablesReference\"", 0);
            ss << ",\"body\":{\"variables\":[";
            if (activeCtx)
            {
                if (varRef == 1000)
                {
                    const auto locals = GetLocalVariables(activeCtx, 0);
                    const auto thisProps = GetThisProperties(activeCtx, 0);
                    bool first = true;
                    for (const auto& var : locals)
                    {
                        if (!first)
                            ss << ",";
                        first = false;
                        ss << "{\"name\":\"" << var.name << "\",\"value\":\"" << var.value << "\",\"type\":\""
                           << var.typeName << "\",\"variablesReference\":0}";
                    }
                    for (const auto& prop : thisProps)
                    {
                        if (!first)
                            ss << ",";
                        first = false;
                        ss << "{\"name\":\"this." << prop.name << "\",\"value\":\"" << prop.value << "\",\"type\":\""
                           << prop.typeName << "\",\"variablesReference\":0}";
                    }
                }
                else if (varRef == 2000)
                {
                    const auto globals = GetGlobalVariables(activeCtx);
                    for (size_t i = 0; i < globals.size(); ++i)
                    {
                        const auto& var = globals[i];
                        ss << "{\"name\":\"" << var.name << "\",\"value\":\"" << var.value << "\",\"type\":\""
                           << var.typeName << "\",\"variablesReference\":0}" << (i + 1 < globals.size() ? "," : "");
                    }
                }
            }
            ss << "]}";
        }
        else if (command == "evaluate")
        {
            const std::string expr = ExtractJsonString(requestJson, "\"expression\"");
            const auto var = EvaluateExpression(activeCtx, expr);
            if (var)
            {
                ss << ",\"body\":{\"result\":\"" << var->value << "\",\"type\":\"" << var->typeName
                   << "\",\"variablesReference\":0}";
            }
            else
            {
                ss << ",\"body\":{\"result\":\"<unknown>\",\"type\":\"\",\"variablesReference\":0}";
            }
        }
        else if (command == "continue")
        {
            Continue();
        }
        else if (command == "next")
        {
            StepOver(activeCtx);
        }
        else if (command == "stepIn")
        {
            StepIn();
        }
        else if (command == "stepOut")
        {
            StepOut(activeCtx);
        }
        else if (command == "pause")
        {
            m_state.store(ExecutionState::Suspended, std::memory_order_release);
        }
        else if (command == "disconnect")
        {
            Detach();
            Continue();
        }

        ss << "}";
        return ss.str();
    }

    std::vector<VariableInfo> GetChildProperties(asIScriptEngine* engine, void* ptr, int typeId) const
    {
        std::vector<VariableInfo> props;
        if (!engine || !ptr || typeId == 0)
        {
            return props;
        }

        void* objPtr = ptr;
        if ((typeId & asTYPEID_MASK_OBJECT) != 0 && (typeId & asTYPEID_OBJHANDLE) != 0)
        {
            objPtr = *reinterpret_cast<void**>(ptr);
            if (!objPtr)
            {
                return props;
            }
        }

        asITypeInfo* typeInfo = engine->GetTypeInfoById(typeId);
        if (!typeInfo)
        {
            return props;
        }

        const asUINT propCount = typeInfo->GetPropertyCount();
        props.reserve(propCount);
        for (asUINT p = 0; p < propCount; ++p)
        {
            const char* propName = nullptr;
            int propTypeId = 0;
            int offset = 0;
            typeInfo->GetProperty(p, &propName, &propTypeId, nullptr, nullptr, &offset);

            void* propPtr = reinterpret_cast<uint8_t*>(objPtr) + offset;
            VariableInfo info;
            info.name = propName ? propName : "";
            info.typeId = propTypeId;
            info.typeName = FormatTypeName(engine, propTypeId);
            info.value = FormatValue(engine, propPtr, propTypeId);
            props.push_back(std::move(info));
        }

        return props;
    }

  protected:
    /**
     * @brief Custom hook called when execution pauses at a breakpoint or step.
     * @param[in] ctx Suspended script context.
     * @param[in] section Current script section name.
     * @param[in] line Current line number.
     * @param[in] col Current column number.
     */
    virtual void OnSuspend([[maybe_unused]] asIScriptContext* ctx, [[maybe_unused]] const std::string& section,
                           [[maybe_unused]] int line, [[maybe_unused]] int col)
    {
    }

    /**
     * @brief Custom hook called when execution pauses on a script exception.
     * @param[in] ctx Suspended script context.
     * @param[in] section Current script section name.
     * @param[in] line Current line number.
     * @param[in] col Current column number.
     * @param[in] exceptionDesc Human-readable description of the exception.
     */
    virtual void OnException([[maybe_unused]] asIScriptContext* ctx, [[maybe_unused]] const std::string& section,
                             [[maybe_unused]] int line, [[maybe_unused]] int col,
                             [[maybe_unused]] const std::string& exceptionDesc)
    {
    }

  private:
    static std::string ExtractJsonString(std::string_view json, std::string_view key)
    {
        const size_t pos = json.find(key);
        if (pos == std::string_view::npos)
        {
            return "";
        }
        size_t start = json.find('"', pos + key.size());
        if (start == std::string_view::npos)
        {
            return "";
        }
        ++start;
        size_t end = json.find('"', start);
        if (end == std::string_view::npos)
        {
            return "";
        }
        return std::string(json.substr(start, end - start));
    }

    static int ExtractJsonInt(std::string_view json, std::string_view key, int defaultVal = 0)
    {
        const size_t pos = json.find(key);
        if (pos == std::string_view::npos)
        {
            return defaultVal;
        }
        size_t numPos = pos + key.size();
        while (numPos < json.size() && (json[numPos] == ':' || json[numPos] == ' ' || json[numPos] == '\t'))
        {
            ++numPos;
        }
        if (numPos >= json.size())
        {
            return defaultVal;
        }
        return std::atoi(json.data() + numPos);
    }

    static std::string NormalizeSection(std::string_view sec)
    {
        std::string res;
        res.reserve(sec.size());
        for (char c : sec)
        {
            if (c == '\\')
                res.push_back('/');
            else
                res.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
        }
        return res;
    }

    bool ShouldSuspend(asIScriptContext* ctx, const std::string& section, int line)
    {
        const auto currentState = m_state.load(std::memory_order_acquire);
        if (currentState == ExecutionState::StepIn)
        {
            return true;
        }
        if (currentState == ExecutionState::StepOver)
        {
            return ctx->GetCallstackSize() <= m_stepStackDepth;
        }
        if (currentState == ExecutionState::StepOut)
        {
            return ctx->GetCallstackSize() <= m_stepStackDepth;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_breakpoints.find(NormalizeSection(section));
        if (it != m_breakpoints.end())
        {
            return it->second.find(line) != it->second.end();
        }
        return false;
    }

    static std::string FormatTypeName(asIScriptEngine* engine, int typeId)
    {
        if (!engine)
            return "";
        asITypeInfo* typeInfo = engine->GetTypeInfoById(typeId);
        if (typeInfo)
        {
            return typeInfo->GetName();
        }
        switch (typeId)
        {
        case asTYPEID_VOID:
            return "void";
        case asTYPEID_BOOL:
            return "bool";
        case asTYPEID_INT8:
            return "int8";
        case asTYPEID_INT16:
            return "int16";
        case asTYPEID_INT32:
            return "int";
        case asTYPEID_INT64:
            return "int64";
        case asTYPEID_UINT8:
            return "uint8";
        case asTYPEID_UINT16:
            return "uint16";
        case asTYPEID_UINT32:
            return "uint";
        case asTYPEID_UINT64:
            return "uint64";
        case asTYPEID_FLOAT:
            return "float";
        case asTYPEID_DOUBLE:
            return "double";
        default:
            return "var";
        }
    }

    static std::string FormatValue(asIScriptEngine* engine, void* ptr, int typeId)
    {
        if (!ptr)
            return "null";

        if ((typeId & asTYPEID_MASK_OBJECT) != 0 && (typeId & asTYPEID_OBJHANDLE) != 0)
        {
            void* handle = *reinterpret_cast<void**>(ptr);
            if (!handle)
                return "null";
            std::ostringstream ss;
            ss << "@" << handle;
            return ss.str();
        }

        switch (typeId)
        {
        case asTYPEID_BOOL:
            return *reinterpret_cast<bool*>(ptr) ? "true" : "false";
        case asTYPEID_INT8:
            return std::to_string(*reinterpret_cast<int8_t*>(ptr));
        case asTYPEID_INT16:
            return std::to_string(*reinterpret_cast<int16_t*>(ptr));
        case asTYPEID_INT32:
            return std::to_string(*reinterpret_cast<int32_t*>(ptr));
        case asTYPEID_INT64:
            return std::to_string(*reinterpret_cast<int64_t*>(ptr));
        case asTYPEID_UINT8:
            return std::to_string(*reinterpret_cast<uint8_t*>(ptr));
        case asTYPEID_UINT16:
            return std::to_string(*reinterpret_cast<uint16_t*>(ptr));
        case asTYPEID_UINT32:
            return std::to_string(*reinterpret_cast<uint32_t*>(ptr));
        case asTYPEID_UINT64:
            return std::to_string(*reinterpret_cast<uint64_t*>(ptr));
        case asTYPEID_FLOAT:
            return std::to_string(*reinterpret_cast<float*>(ptr));
        case asTYPEID_DOUBLE:
            return std::to_string(*reinterpret_cast<double*>(ptr));
        default:
            if (engine)
            {
                asITypeInfo* ti = engine->GetTypeInfoById(typeId);
                if (ti && std::string_view(ti->GetName()) == "string")
                {
                    return "\"" + *reinterpret_cast<std::string*>(ptr) + "\"";
                }
            }
            return "{...}";
        }
    }

    mutable std::mutex m_mutex;
    asIScriptEngine* m_engine = nullptr;
    std::atomic<ExecutionState> m_state{ExecutionState::Running};
    std::atomic<bool> m_breakOnException{false};
    asUINT m_stepStackDepth = 0;
    std::unordered_map<std::string, std::unordered_set<int>> m_breakpoints;
};

} // namespace angel_lsp::debug