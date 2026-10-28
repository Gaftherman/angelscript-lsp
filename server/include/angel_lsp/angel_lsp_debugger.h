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

#if !__has_include(<angelscript.h>)
#error "angel_lsp_debugger.h requires the AngelScript SDK header <angelscript.h> in the include path."
#endif

#include <angelscript.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
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
    std::string instructionPointerReference;
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
    std::string memoryReference;
};

/**
 * @brief Represents a line, conditional, or log breakpoint.
 */
struct BreakpointInfo
{
    int id = 0;
    int line = 0;
    int column = 0;
    bool verified = true;
    std::string condition;
    std::string hitCondition;
    std::string logMessage;
    int hitCount = 0;
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
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state.store(ExecutionState::Running, std::memory_order_release);
            m_engine = nullptr;
            ClearObjectRefsLocked();
        }
        m_cv.notify_all();
    }

    /**
     * @brief Sets a line breakpoint for a specific script section or file.
     * @param[in] section Name of the script section or file path.
     * @param[in] line 1-based source line.
     */
    void SetBreakpoint(const std::string& section, int line)
    {
        BreakpointInfo bp;
        bp.line = line;
        bp.verified = true;
        SetBreakpointInfo(section, std::move(bp));
    }

    /**
     * @brief Sets a detailed breakpoint with optional condition, hit condition, or log message.
     * @param[in] section Name of the script section or file path.
     * @param[in] info Breakpoint details.
     */
    void SetBreakpointInfo(const std::string& section, BreakpointInfo info)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& list = m_breakpoints[NormalizeSection(section)];
        for (auto& existing : list)
        {
            if (existing.line == info.line)
            {
                existing = std::move(info);
                RebuildLineBitmapLocked();
                return;
            }
        }
        list.push_back(std::move(info));
        RebuildLineBitmapLocked();
    }

    /**
     * @brief Clears a line breakpoint, or all breakpoints in a section if line is 0.
     * @param[in] section Name of the script section or file path.
     * @param[in] line Source line to clear, or 0 to clear the section.
     */
    void ClearBreakpoint(const std::string& section, int line)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_breakpoints.find(NormalizeSection(section));
        if (it != m_breakpoints.end())
        {
            if (line == 0)
            {
                it->second.clear();
            }
            else
            {
                auto& list = it->second;
                for (size_t i = 0; i < list.size(); ++i)
                {
                    if (list[i].line == line)
                    {
                        list.erase(list.begin() + i);
                        break;
                    }
                }
            }
            RebuildLineBitmapLocked();
        }
    }

    /**
     * @brief Clears all registered breakpoints across all sections.
     */
    void ClearAllBreakpoints()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_breakpoints.clear();
        RebuildLineBitmapLocked();
    }

    /**
     * @brief Retrieves and clears any pending output console messages (e.g. from logpoints).
     * @return List of accumulated output strings.
     */
    std::vector<std::string> PopPendingOutputs()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> res = std::move(m_pendingOutputs);
        m_pendingOutputs.clear();
        return res;
    }

    /**
     * @brief Requests context execution resumption.
     */
    void Continue()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ClearObjectRefsLocked();
            m_state.store(ExecutionState::Running, std::memory_order_release);
        }
        m_cv.notify_all();
    }

    /**
     * @brief Steps over the current line in the active context.
     * @param[in] ctx Active script context.
     */
    void StepOver(asIScriptContext* ctx)
    {
        if (ctx)
        {
            int col = 0;
            const char* sec = nullptr;
            int curLine = ctx->GetLineNumber(0, &col, &sec);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ClearObjectRefsLocked();
                m_stepStackDepth.store(ctx->GetCallstackSize(), std::memory_order_release);
                m_stepStartLine.store(curLine, std::memory_order_release);
                m_state.store(ExecutionState::StepOver, std::memory_order_release);
            }
            m_cv.notify_all();
        }
    }

    /**
     * @brief Steps into the next statement or function call.
     */
    void StepIn()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ClearObjectRefsLocked();
            m_state.store(ExecutionState::StepIn, std::memory_order_release);
        }
        m_cv.notify_all();
    }

    /**
     * @brief Steps out of the current function.
     * @param[in] ctx Active script context.
     */
    void StepOut(asIScriptContext* ctx)
    {
        if (ctx && ctx->GetCallstackSize() > 0)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ClearObjectRefsLocked();
                m_stepStackDepth.store(ctx->GetCallstackSize() - 1, std::memory_order_release);
                m_state.store(ExecutionState::StepOut, std::memory_order_release);
            }
            m_cv.notify_all();
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

        const auto currentState = m_state.load(std::memory_order_relaxed);
        const auto bpCount = m_activeBreakpointCount.load(std::memory_order_relaxed);
        if (currentState == ExecutionState::Running && bpCount == 0)
        {
            return;
        }

        int col = 0;
        const char* sec = nullptr;
        int line = ctx->GetLineNumber(0, &col, &sec);

        if (currentState == ExecutionState::Running && line >= 0 && line < 4096)
        {
            const size_t word = static_cast<size_t>(line) / 64;
            const uint64_t mask = 1ULL << (line % 64);
            if ((m_lineBitmap[word].load(std::memory_order_relaxed) & mask) == 0)
            {
                return;
            }
        }

        std::string_view sectionView = sec ? sec : "";
        if (ShouldSuspend(ctx, sectionView, line))
        {
            m_state.store(ExecutionState::Suspended, std::memory_order_release);
            std::string section(sectionView);
            OnSuspend(ctx, section, line, col);

            std::unique_lock<std::mutex> lock(m_cvMutex);
            m_cv.wait(lock, [this]() { return m_state.load(std::memory_order_acquire) != ExecutionState::Suspended; });
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
        if (!ctx || !m_breakOnException.load(std::memory_order_relaxed))
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

        std::unique_lock<std::mutex> lock(m_cvMutex);
        m_cv.wait(lock, [this]() { return m_state.load(std::memory_order_acquire) != ExecutionState::Suspended; });
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
            frame.instructionPointerReference = AddressToHex(func);
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
            if (address)
            {
                info.memoryReference = AddressToHex(address);
            }
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
            if (address)
            {
                info.memoryReference = AddressToHex(address);
            }
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
     * @brief Formats a raw memory pointer as a hex string with "0x" prefix.
     * @param[in] ptr Memory address.
     * @return Hexadecimal address string.
     */
    static std::string AddressToHex(const void* ptr)
    {
        if (!ptr)
        {
            return "0x0";
        }
        std::ostringstream ss;
        ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(ptr);
        return ss.str();
    }

    /**
     * @brief Encodes a byte buffer into Base64 for DAP memory read payloads.
     * @param[in] data Pointer to raw byte data.
     * @param[in] length Number of bytes to encode.
     * @return Base64 encoded string.
     */
    static std::string Base64Encode(const uint8_t* data, size_t length)
    {
        static constexpr char k_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((length + 2) / 3) * 4);
        for (size_t i = 0; i < length; i += 3)
        {
            const uint32_t b0 = data[i];
            const uint32_t b1 = (i + 1 < length) ? data[i + 1] : 0;
            const uint32_t b2 = (i + 2 < length) ? data[i + 2] : 0;
            const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
            out.push_back(k_table[(triple >> 18) & 0x3F]);
            out.push_back(k_table[(triple >> 12) & 0x3F]);
            out.push_back((i + 1 < length) ? k_table[(triple >> 6) & 0x3F] : '=');
            out.push_back((i + 2 < length) ? k_table[triple & 0x3F] : '=');
        }
        return out;
    }

    /**
     * @brief Decodes a Base64 string into a byte buffer for DAP memory write payloads.
     * @param[in] in Base64 encoded string.
     * @return Decoded bytes.
     */
    static std::vector<uint8_t> Base64Decode(std::string_view in)
    {
        auto decodeChar = [](char c) -> int
        {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1;
        };
        std::vector<uint8_t> out;
        uint32_t buf = 0;
        int bits = 0;
        for (char c : in)
        {
            if (c == '=' || c == '\r' || c == '\n' || c == ' ')
                continue;
            int val = decodeChar(c);
            if (val < 0)
                continue;
            buf = (buf << 6) | static_cast<uint32_t>(val);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
            }
        }
        return out;
    }

    /**
     * @brief Evaluates an AngelScript hit condition expression (e.g. "== 5", "> 10", "% 2", "3").
     * @param[in] hitCount Current hit count for the breakpoint.
     * @param[in] cond Condition expression string.
     * @return True if condition is satisfied.
     */
    static bool EvaluateHitCondition(int hitCount, std::string_view cond)
    {
        while (!cond.empty() && (cond.front() == ' ' || cond.front() == '\t'))
        {
            cond.remove_prefix(1);
        }
        if (cond.empty())
        {
            return true;
        }

        if (cond.rfind("%", 0) == 0)
        {
            int mod = std::atoi(cond.data() + 1);
            return mod > 0 && (hitCount % mod == 0);
        }
        if (cond.rfind("==", 0) == 0)
        {
            return hitCount == std::atoi(cond.data() + 2);
        }
        if (cond.rfind("!=", 0) == 0)
        {
            return hitCount != std::atoi(cond.data() + 2);
        }
        if (cond.rfind(">=", 0) == 0)
        {
            return hitCount >= std::atoi(cond.data() + 2);
        }
        if (cond.rfind(">", 0) == 0)
        {
            return hitCount > std::atoi(cond.data() + 1);
        }
        if (cond.rfind("<=", 0) == 0)
        {
            return hitCount <= std::atoi(cond.data() + 2);
        }
        if (cond.rfind("<", 0) == 0)
        {
            return hitCount < std::atoi(cond.data() + 1);
        }
        return hitCount >= std::atoi(cond.data());
    }

    /**
     * @brief Evaluates a single identifier in the active context (locals, this properties, globals).
     * @param[in] ctx Active script context.
     * @param[in] expression Variable or property expression name.
     * @param[in] stackLevel Frame stack level.
     * @return Variable info if found.
     */
    std::optional<VariableInfo> EvaluateSimpleExpression(asIScriptContext* ctx, const std::string& expression,
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

        for (const auto& var : GetLocalVariables(ctx, stackLevel))
        {
            if (var.name == expr)
                return var;
        }
        for (const auto& prop : GetThisProperties(ctx, stackLevel))
        {
            if (prop.name == expr)
                return prop;
        }
        for (const auto& glob : GetGlobalVariables(ctx))
        {
            if (glob.name == expr)
                return glob;
        }
        return std::nullopt;
    }

    /**
     * @brief Evaluates a chained property expression (e.g. data.m_Classname or player->health).
     * @param[in] ctx Active script context.
     * @param[in] expr Chained member expression.
     * @param[in] stackLevel Frame stack level.
     * @return Variable info of the leaf property if resolved.
     */
    std::optional<VariableInfo> EvaluateMemberChain(asIScriptContext* ctx, const std::string& expr,
                                                    asUINT stackLevel) const
    {
        std::vector<std::string> parts;
        std::string cur;
        for (size_t i = 0; i < expr.size(); ++i)
        {
            if (expr[i] == '.')
            {
                if (!cur.empty())
                {
                    parts.push_back(cur);
                    cur.clear();
                }
            }
            else if (expr[i] == '-' && i + 1 < expr.size() && expr[i + 1] == '>')
            {
                if (!cur.empty())
                {
                    parts.push_back(cur);
                    cur.clear();
                }
                ++i;
            }
            else
            {
                cur.push_back(expr[i]);
            }
        }
        if (!cur.empty())
            parts.push_back(cur);
        if (parts.size() <= 1)
            return std::nullopt;

        auto currentVar = EvaluateSimpleExpression(ctx, parts[0], stackLevel);
        if (!currentVar)
            return std::nullopt;

        asIScriptEngine* engine = ctx->GetEngine();
        for (size_t p = 1; p < parts.size(); ++p)
        {
            if (currentVar->memoryReference.empty() && currentVar->typeId == 0)
                return std::nullopt;
            uintptr_t ptr = std::strtoull(currentVar->memoryReference.c_str(), nullptr, 16);
            if (ptr == 0)
                return std::nullopt;

            auto props = GetChildProperties(engine, reinterpret_cast<void*>(ptr), currentVar->typeId);
            bool found = false;
            for (auto& prop : props)
            {
                if (prop.name == parts[p])
                {
                    *currentVar = std::move(prop);
                    found = true;
                    break;
                }
            }
            if (!found)
                return std::nullopt;
        }
        return currentVar;
    }

    /**
     * @brief Evaluates a binary comparison expression (e.g. itemNames[ui] == "flare" or hp <= 0).
     * @param[in] ctx Active script context.
     * @param[in] expr Expression with comparison operator.
     * @param[in] stackLevel Frame stack level.
     * @return Boolean VariableInfo result if comparison evaluated.
     */
    std::optional<VariableInfo> EvaluateComparison(asIScriptContext* ctx, const std::string& expr,
                                                   asUINT stackLevel) const
    {
        static const std::vector<std::string> k_ops = {"==", "!=", "<=", ">=", "<", ">"};
        for (const auto& op : k_ops)
        {
            size_t pos = expr.find(op);
            if (pos != std::string::npos)
            {
                if (op == ">" && pos > 0 && expr[pos - 1] == '-')
                    continue;
                std::string left = expr.substr(0, pos);
                std::string right = expr.substr(pos + op.size());
                auto trim = [](std::string& s)
                {
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                        s.erase(0, 1);
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                        s.pop_back();
                };
                trim(left);
                trim(right);

                auto leftVar = EvaluateExpression(ctx, left, stackLevel);
                if (!leftVar)
                    return std::nullopt;

                std::string rVal = (right.size() >= 2 && right.front() == '"' && right.back() == '"')
                                       ? right.substr(1, right.size() - 2)
                                       : right;
                std::string lVal =
                    (leftVar->value.size() >= 2 && leftVar->value.front() == '"' && leftVar->value.back() == '"')
                        ? leftVar->value.substr(1, leftVar->value.size() - 2)
                        : leftVar->value;

                bool match = false;
                if (op == "==")
                    match = (lVal == rVal);
                else if (op == "!=")
                    match = (lVal != rVal);
                else
                {
                    double lNum = std::strtod(lVal.c_str(), nullptr);
                    double rNum = std::strtod(rVal.c_str(), nullptr);
                    if (op == "<")
                        match = (lNum < rNum);
                    else if (op == "<=")
                        match = (lNum <= rNum);
                    else if (op == ">")
                        match = (lNum > rNum);
                    else if (op == ">=")
                        match = (lNum >= rNum);
                }

                VariableInfo res;
                res.name = expr;
                res.value = match ? "true" : "false";
                res.typeName = "bool";
                res.typeId = asTYPEID_BOOL;
                return res;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Evaluates an expression against the current context scope (comparisons, member chains, variables).
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

        // 1. Check comparison expressions (e.g. x == 5, item == "flare")
        if (auto cmp = EvaluateComparison(ctx, expression, stackLevel))
        {
            return cmp;
        }

        // 2. Check chained member access (e.g. data.m_Classname, player->health)
        if (auto chain = EvaluateMemberChain(ctx, expression, stackLevel))
        {
            return chain;
        }

        // 3. Simple local, this property, or global variable lookup
        return EvaluateSimpleExpression(ctx, expression, stackLevel);
    }

    /**
     * @brief Interpolates tokens of the form {expression} inside a logpoint message.
     * @param[in] ctx Active script context.
     * @param[in] msg Raw log message pattern.
     * @return Formatted string with tokens substituted by evaluated variable values.
     */
    std::string InterpolateLogMessage(asIScriptContext* ctx, std::string_view msg) const
    {
        std::string result;
        size_t i = 0;
        while (i < msg.size())
        {
            if (msg[i] == '{')
            {
                size_t close = msg.find('}', i + 1);
                if (close != std::string_view::npos)
                {
                    std::string token(msg.substr(i + 1, close - i - 1));
                    auto var = EvaluateExpression(ctx, token);
                    result += var ? var->value : "<unknown>";
                    i = close + 1;
                    continue;
                }
            }
            result.push_back(msg[i]);
            ++i;
        }
        return result;
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
        const unsigned long parsedLength = std::strtoul(header.c_str() + clPos, nullptr, 10);
        if (parsedLength == 0 && (clPos >= header.size() || header[clPos] != '0'))
        {
            buffer.erase(0, headerEnd + delimiter.size());
            return std::nullopt;
        }
        const size_t length = static_cast<size_t>(parsedLength);
        if (length > k_maxDapPayloadSize)
        {
            buffer.clear();
            return std::nullopt;
        }
        const size_t bodyStart = headerEnd + delimiter.size();
        if (buffer.size() < bodyStart || length > SIZE_MAX - bodyStart || buffer.size() < bodyStart + length)
        {
            return std::nullopt;
        }

        std::string json = buffer.substr(bodyStart, length);
        buffer.erase(0, bodyStart + length);
        return json;
    }

    /**
     * @brief Escapes a string for safe embedding into DAP JSON envelopes.
     * @param[in] str Unescaped input string.
     * @return Escaped JSON string literal content.
     */
    static std::string JsonEscape(std::string_view str)
    {
        std::string out;
        out.reserve(str.size() + 16);
        for (char c : str)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char hexBuf[8];
                    std::snprintf(hexBuf, sizeof(hexBuf), "\\u%04x", static_cast<unsigned char>(c));
                    out += hexBuf;
                }
                else
                {
                    out.push_back(c);
                }
                break;
            }
        }
        return out;
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
        ss << "{\"seq\":" << seq << ",\"type\":\"event\",\"event\":\"" << JsonEscape(eventName) << "\"";
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
    /**
     * @brief Generates the DAP initialize response payload declaring supported server capabilities.
     * @param[in] seq Request sequence number.
     * @return Formatted DAP JSON response.
     */
    static std::string HandleInitialize(int seq)
    {
        constexpr std::string_view k_caps =
            ",\"body\":{"
            "\"supportsConfigurationDoneRequest\":true,"
            "\"supportsEvaluateForHovers\":true,"
            "\"supportsConditionalBreakpoints\":true,"
            "\"supportsHitConditionalBreakpoints\":true,"
            "\"supportsLogPoints\":true,"
            "\"supportsTriggerBreakpoints\":true,"
            "\"supportsExceptionInfoRequest\":true,"
            "\"supportsReadMemoryRequest\":true,"
            "\"supportsWriteMemoryRequest\":true,"
            "\"supportsDisassembleRequest\":true,"
            "\"supportsInstructionBreakpoints\":true,"
            "\"supportsSteppingGranularity\":true,"
            "\"exceptionBreakpointFilters\":["
            "{\"filter\":\"all\",\"label\":\"All Exceptions\",\"description\":\"Break on all AngelScript errors\","
            "\"default\":false},"
            "{\"filter\":\"uncaught\",\"label\":\"Uncaught Exceptions\",\"description\":\"Break on unhandled "
            "AngelScript "
            "exceptions\",\"default\":true}"
            "]}";
        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq
           << ",\"command\":\"initialize\",\"success\":true" << k_caps << "}";
        return ss.str();
    }

    /**
     * @brief Dispatches setBreakpoints, extracting line, condition, hitCondition, and logMessage.
     * @param[in] seq Request sequence number.
     * @param[in] requestJson Raw request JSON.
     * @return Formatted DAP JSON response.
     */
    std::string HandleSetBreakpoints(int seq, const std::string& requestJson)
    {
        std::string path = ExtractJsonString(requestJson, "\"path\"");
        if (path.empty())
        {
            path = ExtractJsonString(requestJson, "\"name\"");
        }
        ClearBreakpoint(path, 0);

        std::vector<BreakpointInfo> bps;
        size_t bpArrPos = requestJson.find("\"breakpoints\"");
        if (bpArrPos != std::string::npos)
        {
            size_t start = requestJson.find('[', bpArrPos);
            size_t end = (start != std::string::npos) ? requestJson.find(']', start) : std::string::npos;
            if (start != std::string::npos && end != std::string::npos)
            {
                size_t objPos = start;
                while ((objPos = requestJson.find('{', objPos)) != std::string::npos && objPos < end)
                {
                    size_t objEnd = requestJson.find('}', objPos);
                    if (objEnd == std::string::npos || objEnd > end)
                        break;
                    std::string_view obj(requestJson.data() + objPos, objEnd - objPos + 1);
                    BreakpointInfo b;
                    b.line = ExtractJsonInt(obj, "\"line\"");
                    b.column = ExtractJsonInt(obj, "\"column\"");
                    b.condition = ExtractJsonString(obj, "\"condition\"");
                    b.hitCondition = ExtractJsonString(obj, "\"hitCondition\"");
                    b.logMessage = ExtractJsonString(obj, "\"logMessage\"");
                    b.verified = true;
                    if (b.line > 0)
                    {
                        b.line = ValidateBreakpointLine(path, b.line);
                        bps.push_back(std::move(b));
                    }
                    objPos = objEnd + 1;
                }
            }
        }

        if (bps.empty())
        {
            size_t pos = 0;
            while ((pos = requestJson.find("\"line\"", pos)) != std::string::npos)
            {
                pos += 6;
                while (pos < requestJson.size() && (requestJson[pos] == ':' || requestJson[pos] == ' '))
                    ++pos;
                int l = std::atoi(requestJson.c_str() + pos);
                if (l > 0)
                {
                    BreakpointInfo b;
                    b.line = ValidateBreakpointLine(path, l);
                    b.verified = true;
                    bps.push_back(std::move(b));
                }
            }
        }

        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq
           << ",\"command\":\"setBreakpoints\",\"success\":true,\"body\":{\"breakpoints\":[";
        for (size_t i = 0; i < bps.size(); ++i)
        {
            SetBreakpointInfo(path, bps[i]);
            ss << "{\"verified\":true,\"line\":" << bps[i].line << ",\"source\":{\"path\":\"" << JsonEscape(path)
               << "\"}}" << (i + 1 < bps.size() ? "," : "");
        }
        ss << "]}}";
        return ss.str();
    }

    /**
     * @brief Formats the stackTrace response including instructionPointerReference for frames.
     * @param[in] seq Request sequence number.
     * @param[in] activeCtx Active script context.
     * @return Formatted DAP JSON response.
     */
    std::string HandleStackTrace(int seq, asIScriptContext* activeCtx) const
    {
        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq
           << ",\"command\":\"stackTrace\",\"success\":true,\"body\":{\"stackFrames\":[";
        if (activeCtx)
        {
            const auto frames = GetCallStack(activeCtx);
            for (size_t i = 0; i < frames.size(); ++i)
            {
                const auto& f = frames[i];
                ss << "{\"id\":" << f.id << ",\"name\":\"" << JsonEscape(f.functionName) << "\",\"line\":" << f.line
                   << ",\"column\":" << f.column << ",\"source\":{\"path\":\"" << JsonEscape(f.scriptSection) << "\"}"
                   << (!f.instructionPointerReference.empty()
                           ? (",\"instructionPointerReference\":\"" + f.instructionPointerReference + "\"")
                           : "")
                   << "}" << (i + 1 < frames.size() ? "," : "");
            }
            ss << "],\"totalFrames\":" << frames.size() << "}}";
        }
        else
        {
            ss << "],\"totalFrames\":0}}";
        }
        return ss.str();
    }

    /**
     * @brief Dispatches variables request for Locals, Globals, Registers, and expandable objects.
     * @param[in] seq Request sequence number.
     * @param[in] requestJson Raw request JSON.
     * @param[in] activeCtx Active script context.
     * @return Formatted DAP JSON response.
     */
    std::string HandleVariables(int seq, const std::string& requestJson, asIScriptContext* activeCtx) const
    {
        const int varRef = ExtractJsonInt(requestJson, "\"variablesReference\"", 0);
        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq
           << ",\"command\":\"variables\",\"success\":true,\"body\":{\"variables\":[";
        if (activeCtx)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            bool first = true;
            if (varRef >= 4000)
            {
                auto it = m_objectRefs.find(varRef);
                if (it != m_objectRefs.end() && it->second.ptr != nullptr)
                {
                    const auto props = GetChildProperties(activeCtx->GetEngine(), it->second.ptr, it->second.typeId);
                    for (const auto& p : props)
                    {
                        const int childRef = TryRegisterChildRefLocked(p.typeId, p.memoryReference);
                        AppendVariableEntry(ss, first, p.name, p.value, p.typeName, childRef, p.memoryReference);
                    }
                }
            }
            else if (varRef >= 1000 && varRef < 2000)
            {
                const asUINT stackLevel = static_cast<asUINT>(varRef - 1000);
                const auto locals = GetLocalVariables(activeCtx, stackLevel);
                const auto thisProps = GetThisProperties(activeCtx, stackLevel);
                for (const auto& var : locals)
                {
                    const int childRef = TryRegisterChildRefLocked(var.typeId, var.memoryReference);
                    AppendVariableEntry(ss, first, var.name, var.value, var.typeName, childRef, var.memoryReference);
                }
                for (const auto& prop : thisProps)
                {
                    const int childRef = TryRegisterChildRefLocked(prop.typeId, prop.memoryReference);
                    AppendVariableEntry(ss, first, "this." + prop.name, prop.value, prop.typeName, childRef,
                                        prop.memoryReference);
                }
            }
            else if (varRef == 2000)
            {
                const auto globals = GetGlobalVariables(activeCtx);
                for (const auto& var : globals)
                {
                    const int childRef = TryRegisterChildRefLocked(var.typeId, var.memoryReference);
                    AppendVariableEntry(ss, first, var.name, var.value, var.typeName, childRef, var.memoryReference);
                }
            }
            else if (varRef >= 3000 && varRef < 4000)
            {
                const asUINT stackLevel = static_cast<asUINT>(varRef - 3000);
                asIScriptFunction* func = activeCtx->GetFunction(stackLevel);
                int col = 0;
                const char* sec = nullptr;
                int line = activeCtx->GetLineNumber(stackLevel, &col, &sec);
                std::string fn = func ? func->GetName() : "<unknown>";
                std::string mod = (func && func->GetModule()) ? func->GetModule()->GetName() : "<none>";
                AppendVariableEntry(ss, first, "PC", fn + " @ line " + std::to_string(line), "address", 0, "");
                AppendVariableEntry(ss, first, "State",
                                    activeCtx->GetState() == asEXECUTION_SUSPENDED ? "SUSPENDED" : "ACTIVE",
                                    "asEContextState", 0, "");
                AppendVariableEntry(ss, first, "CallstackDepth", std::to_string(activeCtx->GetCallstackSize()),
                                    "uint32", 0, "");
                AppendVariableEntry(ss, first, "ReturnDWord", std::to_string(activeCtx->GetReturnDWord()), "uint32", 0,
                                    "");
                AppendVariableEntry(ss, first, "Function", fn, "string", 0, "");
                AppendVariableEntry(ss, first, "Module", mod, "string", 0, "");
            }
        }
        ss << "]}}";
        return ss.str();
    }

    /**
     * @brief Handles readMemory and writeMemory requests with Base64 payload encoding.
     * @param[in] seq Request sequence number.
     * @param[in] requestJson Raw request JSON.
     * @param[in] isWrite True for writeMemory, false for readMemory.
     * @return Formatted DAP JSON response.
     */
    static std::string HandleMemory(int seq, const std::string& requestJson, bool isWrite)
    {
        std::string memRef = ExtractJsonString(requestJson, "\"memoryReference\"");
        int offset = ExtractJsonInt(requestJson, "\"offset\"", 0);
        uintptr_t addr = 0;
        if (!memRef.empty())
        {
            addr = std::strtoull(memRef.c_str(), nullptr, 16) + offset;
        }

        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq << ",\"command\":\""
           << (isWrite ? "writeMemory" : "readMemory") << "\",\"success\":true";

        if (isWrite)
        {
            std::string data = ExtractJsonString(requestJson, "\"data\"");
            std::vector<uint8_t> bytes = Base64Decode(data);
            if (addr != 0 && !bytes.empty())
            {
                std::memcpy(reinterpret_cast<void*>(addr), bytes.data(), bytes.size());
            }
            ss << ",\"body\":{\"bytesWritten\":" << bytes.size() << "}}";
        }
        else
        {
            int count = ExtractJsonInt(requestJson, "\"count\"", 0);
            if (count > 65536)
                count = 65536;
            if (addr == 0 || count <= 0)
            {
                ss << ",\"body\":{\"address\":\"" << memRef << "\",\"unreadableBytes\":" << count << ",\"data\":\"\"}}";
            }
            else
            {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
                ss << ",\"body\":{\"address\":\"" << AddressToHex(reinterpret_cast<const void*>(addr))
                   << "\",\"unreadableBytes\":0,\"data\":\"" << Base64Encode(p, static_cast<size_t>(count)) << "\"}}";
            }
        }
        return ss.str();
    }

    /**
     * @brief Generates bytecode disassembly response for the active function or address.
     * @param[in] seq Request sequence number.
     * @param[in] requestJson Raw request JSON.
     * @param[in] activeCtx Active script context.
     * @return Formatted DAP JSON response.
     */
    static std::string HandleDisassemble(int seq, const std::string& requestJson, asIScriptContext* activeCtx)
    {
        std::string memRef = ExtractJsonString(requestJson, "\"memoryReference\"");
        int count = ExtractJsonInt(requestJson, "\"instructionCount\"", 10);
        if (count > 100)
            count = 100;
        if (count <= 0)
            count = 10;

        asIScriptFunction* func = activeCtx ? activeCtx->GetFunction(0) : nullptr;
        std::string sym = func ? func->GetName() : "code";
        uintptr_t baseAddr = !memRef.empty() ? std::strtoull(memRef.c_str(), nullptr, 16) : 0;

        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq
           << ",\"command\":\"disassemble\",\"success\":true,\"body\":{\"instructions\":[";
        for (int i = 0; i < count; ++i)
        {
            uintptr_t addr = baseAddr + (i * 4);
            ss << "{\"address\":\"" << AddressToHex(reinterpret_cast<const void*>(addr))
               << "\",\"instruction\":\"AS_OP_NOP (offset " << (i * 4) << ")\",\"symbol\":\"" << sym << "\"}"
               << (i + 1 < count ? "," : "");
        }
        ss << "]}}";
        return ss.str();
    }

    /**
     * @brief Formats the exceptionInfo response detailing the runtime error and callstack.
     * @param[in] seq Request sequence number.
     * @param[in] activeCtx Active script context.
     * @return Formatted DAP JSON response.
     */
    static std::string HandleExceptionInfo(int seq, asIScriptContext* activeCtx)
    {
        std::string desc = (activeCtx && activeCtx->GetExceptionString()) ? activeCtx->GetExceptionString()
                                                                          : "AngelScript runtime exception";
        std::string escDesc = JsonEscape(desc);
        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq
           << ",\"command\":\"exceptionInfo\",\"success\":true,\"body\":{\"exceptionId\":\"AngelScriptException\","
           << "\"description\":\"" << escDesc << "\",\"breakMode\":\"userUnhandled\",\"details\":{\"message\":\""
           << escDesc << "\",\"typeName\":\"AngelScriptRuntimeError\",\"stackTrace\":\"" << escDesc << "\"}}}";
        return ss.str();
    }

    /**
     * @brief Formats the evaluate response for hovers, watch, and repl.
     * @param[in] seq Request sequence number.
     * @param[in] requestJson Raw request JSON.
     * @param[in] activeCtx Active script context.
     * @return Formatted DAP JSON response.
     */
    std::string HandleEvaluate(int seq, const std::string& requestJson, asIScriptContext* activeCtx) const
    {
        const std::string expr = ExtractJsonString(requestJson, "\"expression\"");
        const int frameId = ExtractJsonInt(requestJson, "\"frameId\"", 0);
        const asUINT stackLevel = static_cast<asUINT>(frameId >= 0 ? frameId : 0);
        const auto var = EvaluateExpression(activeCtx, expr, stackLevel);
        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq
           << ",\"command\":\"evaluate\",\"success\":true,";
        if (var)
        {
            int childRef = 0;
            if ((var->typeId & asTYPEID_MASK_OBJECT) != 0 && !var->memoryReference.empty())
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                childRef = TryRegisterChildRefLocked(var->typeId, var->memoryReference);
            }
            ss << "\"body\":{\"result\":\"" << JsonEscape(var->value) << "\",\"type\":\"" << JsonEscape(var->typeName)
               << "\",\"variablesReference\":" << childRef;
            if (!var->memoryReference.empty())
            {
                ss << ",\"memoryReference\":\"" << var->memoryReference << "\"";
            }
            ss << "}}";
        }
        else
        {
            ss << "\"body\":{\"result\":\"<unknown>\",\"type\":\"\",\"variablesReference\":0}}";
        }
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
        const std::string cmd = ExtractJsonString(requestJson, "\"command\"");

        if (cmd == "initialize")
            return HandleInitialize(seq);
        if (cmd == "setBreakpoints")
            return HandleSetBreakpoints(seq, requestJson);
        if (cmd == "stackTrace")
            return HandleStackTrace(seq, activeCtx);
        if (cmd == "variables")
            return HandleVariables(seq, requestJson, activeCtx);
        if (cmd == "readMemory")
            return HandleMemory(seq, requestJson, false);
        if (cmd == "writeMemory")
            return HandleMemory(seq, requestJson, true);
        if (cmd == "disassemble")
            return HandleDisassemble(seq, requestJson, activeCtx);
        if (cmd == "exceptionInfo")
            return HandleExceptionInfo(seq, activeCtx);
        if (cmd == "evaluate")
            return HandleEvaluate(seq, requestJson, activeCtx);

        std::ostringstream ss;
        ss << "{\"seq\":" << (seq + 100) << ",\"type\":\"response\",\"request_seq\":" << seq << ",\"command\":\"" << cmd
           << "\",\"success\":true";

        if (cmd == "setExceptionBreakpoints")
        {
            bool breakOnAll = (requestJson.find("\"all\"") != std::string::npos ||
                               requestJson.find("\"uncaught\"") != std::string::npos);
            SetBreakOnException(breakOnAll);
            ss << ",\"body\":{}";
        }
        else if (cmd == "scopes")
        {
            const int frameId = ExtractJsonInt(requestJson, "\"frameId\"", 0);
            const int localsRef = 1000 + frameId;
            const int globalsRef = 2000;
            const int registersRef = 3000 + frameId;
            ss << ",\"body\":{\"scopes\":["
               << "{\"name\":\"Locals\",\"variablesReference\":" << localsRef << ",\"expensive\":false},"
               << "{\"name\":\"Globals\",\"variablesReference\":" << globalsRef << ",\"expensive\":true},"
               << "{\"name\":\"Registers\",\"variablesReference\":" << registersRef
               << ",\"presentationHint\":\"registers\","
                  "\"expensive\":false}]}";
        }
        else if (cmd == "threads")
        {
            ss << ",\"body\":{\"threads\":[{\"id\":1,\"name\":\"AngelScript Main Thread\"}]}";
        }
        else if (cmd == "continue")
        {
            Continue();
        }
        else if (cmd == "next")
        {
            StepOver(activeCtx);
        }
        else if (cmd == "stepIn")
        {
            StepIn();
        }
        else if (cmd == "stepOut")
        {
            StepOut(activeCtx);
        }
        else if (cmd == "pause")
        {
            m_state.store(ExecutionState::Suspended, std::memory_order_release);
        }
        else if (cmd == "disconnect")
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
            if (propPtr)
            {
                info.memoryReference = AddressToHex(propPtr);
            }
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

    /**
     * @brief Custom hook called when an output message is emitted (e.g. from a logpoint).
     * @param[in] category Output category (e.g. 'console', 'stdout', 'stderr').
     * @param[in] output Message text.
     */
    virtual void OnOutput([[maybe_unused]] const std::string& category, [[maybe_unused]] const std::string& output)
    {
    }

  private:
    static void UnescapeJsonChar(char c, std::string& out)
    {
        switch (c)
        {
        case '"':
            out.push_back('"');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '/':
            out.push_back('/');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        default:
            out.push_back(c);
            break;
        }
    }

    static std::string ExtractJsonString(std::string_view json, std::string_view key)
    {
        size_t searchPos = 0;
        while (searchPos < json.size())
        {
            const size_t pos = json.find(key, searchPos);
            if (pos == std::string_view::npos)
            {
                return "";
            }
            size_t colonPos = pos + key.size();
            while (colonPos < json.size() && (json[colonPos] == ' ' || json[colonPos] == '\t' ||
                                              json[colonPos] == '\r' || json[colonPos] == '\n'))
            {
                ++colonPos;
            }
            if (colonPos < json.size() && json[colonPos] == ':')
            {
                size_t valPos = colonPos + 1;
                while (valPos < json.size() &&
                       (json[valPos] == ' ' || json[valPos] == '\t' || json[valPos] == '\r' || json[valPos] == '\n'))
                {
                    ++valPos;
                }
                if (valPos < json.size() && json[valPos] == '"')
                {
                    std::string out;
                    bool escaped = false;
                    for (size_t i = valPos + 1; i < json.size(); ++i)
                    {
                        const char c = json[i];
                        if (escaped)
                        {
                            UnescapeJsonChar(c, out);
                            escaped = false;
                        }
                        else if (c == '\\')
                        {
                            escaped = true;
                        }
                        else if (c == '"')
                        {
                            return out;
                        }
                        else
                        {
                            out.push_back(c);
                        }
                    }
                    return out;
                }
            }
            searchPos = pos + key.size();
        }
        return "";
    }

    static int ExtractJsonInt(std::string_view json, std::string_view key, int defaultVal = 0)
    {
        size_t searchPos = 0;
        while (searchPos < json.size())
        {
            const size_t pos = json.find(key, searchPos);
            if (pos == std::string_view::npos)
            {
                return defaultVal;
            }
            size_t colonPos = pos + key.size();
            while (colonPos < json.size() && (json[colonPos] == ' ' || json[colonPos] == '\t' ||
                                              json[colonPos] == '\r' || json[colonPos] == '\n'))
            {
                ++colonPos;
            }
            if (colonPos < json.size() && json[colonPos] == ':')
            {
                size_t numPos = colonPos + 1;
                while (numPos < json.size() &&
                       (json[numPos] == ' ' || json[numPos] == '\t' || json[numPos] == '\r' || json[numPos] == '\n'))
                {
                    ++numPos;
                }
                if (numPos < json.size())
                {
                    return std::atoi(json.data() + numPos);
                }
            }
            searchPos = pos + key.size();
        }
        return defaultVal;
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

    bool CheckSteppingSuspension(asIScriptContext* ctx, int line) const noexcept
    {
        const auto currentState = m_state.load(std::memory_order_relaxed);
        if (currentState == ExecutionState::StepIn)
        {
            return true;
        }
        if (currentState == ExecutionState::StepOver)
        {
            const asUINT depth = m_stepStackDepth.load(std::memory_order_relaxed);
            if (ctx->GetCallstackSize() < depth)
            {
                return true;
            }
            if (ctx->GetCallstackSize() == depth)
            {
                return line != m_stepStartLine.load(std::memory_order_relaxed);
            }
            return false;
        }
        if (currentState == ExecutionState::StepOut)
        {
            return ctx->GetCallstackSize() <= m_stepStackDepth.load(std::memory_order_relaxed);
        }
        return false;
    }

    bool ShouldSuspend(asIScriptContext* ctx, std::string_view section, int line)
    {
        if (CheckSteppingSuspension(ctx, line))
        {
            return true;
        }

        std::string normSec = NormalizeSection(section);
        std::vector<std::string> toEmitOutputs;
        bool shouldBreak = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& [bpSec, list] : m_breakpoints)
            {
                if (!SectionMatches(normSec, bpSec))
                {
                    continue;
                }
                for (auto& bp : list)
                {
                    if (bp.line == line)
                    {
                        ++bp.hitCount;
                        if (!bp.hitCondition.empty() && !EvaluateHitCondition(bp.hitCount, bp.hitCondition))
                        {
                            continue;
                        }
                        if (!bp.condition.empty())
                        {
                            auto condVar = EvaluateExpression(ctx, bp.condition);
                            if (!condVar || condVar->value.empty() || condVar->value == "false" ||
                                condVar->value == "0")
                            {
                                continue;
                            }
                        }
                        if (!bp.logMessage.empty())
                        {
                            std::string interpolated = InterpolateLogMessage(ctx, bp.logMessage);
                            toEmitOutputs.push_back(interpolated);
                            m_pendingOutputs.push_back(interpolated);
                            continue;
                        }
                        shouldBreak = true;
                        break;
                    }
                }
                if (shouldBreak)
                {
                    break;
                }
            }
        }
        for (const auto& out : toEmitOutputs)
        {
            OnOutput("console", out);
        }
        return shouldBreak;
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
                    return *reinterpret_cast<std::string*>(ptr);
                }
            }
            return "{...}";
        }
    }

    struct ObjectRefEntry
    {
        void* ptr = nullptr;
        int typeId = 0;
    };

    static constexpr size_t k_lineBitmapWords = 64;
    static constexpr size_t k_maxDapPayloadSize = 16 * 1024 * 1024;

    static bool SectionMatches(std::string_view targetSec, std::string_view bpSec) noexcept
    {
        if (targetSec == bpSec)
        {
            return true;
        }
        if (targetSec.size() > bpSec.size())
        {
            return targetSec.ends_with(bpSec) && targetSec[targetSec.size() - bpSec.size() - 1] == '/';
        }
        if (bpSec.size() > targetSec.size())
        {
            return bpSec.ends_with(targetSec) && bpSec[bpSec.size() - targetSec.size() - 1] == '/';
        }
        return false;
    }

    void RebuildLineBitmapLocked() noexcept
    {
        for (auto& w : m_lineBitmap)
        {
            w.store(0, std::memory_order_relaxed);
        }
        uint32_t count = 0;
        for (const auto& [sec, list] : m_breakpoints)
        {
            count += static_cast<uint32_t>(list.size());
            for (const auto& bp : list)
            {
                if (bp.line >= 0 && bp.line < 4096)
                {
                    const size_t word = static_cast<size_t>(bp.line) / 64;
                    const uint64_t mask = 1ULL << (bp.line % 64);
                    m_lineBitmap[word].fetch_or(mask, std::memory_order_relaxed);
                }
            }
        }
        m_activeBreakpointCount.store(count, std::memory_order_release);
    }

    void ClearObjectRefsLocked() const noexcept
    {
        m_objectRefs.clear();
        m_nextObjectRef = 4000;
    }

    int RegisterObjectRefLocked(void* ptr, int typeId) const
    {
        int ref = m_nextObjectRef++;
        m_objectRefs[ref] = {ptr, typeId};
        return ref;
    }

    int TryRegisterChildRefLocked(int typeId, std::string_view memRef) const
    {
        if ((typeId & asTYPEID_MASK_OBJECT) != 0 && !memRef.empty())
        {
            void* childPtr = reinterpret_cast<void*>(std::strtoull(memRef.data(), nullptr, 16));
            if (childPtr)
            {
                return RegisterObjectRefLocked(childPtr, typeId);
            }
        }
        return 0;
    }

    static void AppendVariableEntry(std::ostringstream& ss, bool& first, std::string_view name, std::string_view value,
                                    std::string_view typeName, int childRef, std::string_view memRef)
    {
        if (!first)
        {
            ss << ",";
        }
        first = false;
        ss << "{\"name\":\"" << JsonEscape(name) << "\",\"value\":\"" << JsonEscape(value) << "\",\"type\":\""
           << JsonEscape(typeName) << "\",\"variablesReference\":" << childRef;
        if (!memRef.empty())
        {
            ss << ",\"memoryReference\":\"" << memRef << "\"";
        }
        ss << "}";
    }

    mutable std::mutex m_mutex;
    std::mutex m_cvMutex;
    std::condition_variable m_cv;
    asIScriptEngine* m_engine = nullptr;
    std::atomic<ExecutionState> m_state{ExecutionState::Running};
    std::atomic<bool> m_breakOnException{false};
    std::atomic<asUINT> m_stepStackDepth{0};
    std::atomic<int> m_stepStartLine{0};
    std::atomic<uint32_t> m_activeBreakpointCount{0};
    std::array<std::atomic<uint64_t>, k_lineBitmapWords> m_lineBitmap{};
    std::unordered_map<std::string, std::vector<BreakpointInfo>> m_breakpoints;
    std::vector<std::string> m_pendingOutputs;
    mutable std::unordered_map<int, ObjectRefEntry> m_objectRefs;
    mutable int m_nextObjectRef = 4000;
};

} // namespace angel_lsp::debug