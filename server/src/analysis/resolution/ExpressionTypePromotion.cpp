#include "analysis/resolution/ExpressionTypeBinary.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

bool IsBooleanBinaryOp(std::string_view op) noexcept
{
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" || op == "is" ||
           op == "!is" || op == "&&" || op == "||" || op == "and" || op == "or" || op == "^^" || op == "xor";
}

static bool IsFloatOrIntPrimitive(std::string_view type)
{
    return type == "float" || type == "double" || type == "int" || type == "uint";
}

static std::string ResolveBinaryShift(std::string_view cleanLeft)
{
    if (cleanLeft == "int64" || cleanLeft == "uint64")
    {
        return std::string(cleanLeft);
    }
    return IsUnsignedInteger(cleanLeft) ? "uint" : "int";
}

static std::string ResolveBinaryBitwiseLogic(std::string_view cleanLeft, std::string_view cleanRight,
                                             const SymbolTable* symbolTable)
{
    if (symbolTable && (ResolvesToEnum(cleanLeft, *symbolTable) || ResolvesToEnum(cleanRight, *symbolTable)))
    {
        return "int";
    }
    if (cleanLeft == "int64" || cleanRight == "int64")
    {
        return "int64";
    }
    if (cleanLeft == "uint64" || cleanRight == "uint64")
    {
        return "uint64";
    }
    if (cleanLeft == "uint8" || cleanLeft == "uint16")
    {
        return "uint";
    }
    if (cleanLeft == "int8" || cleanLeft == "int16")
    {
        return "int";
    }
    if (cleanLeft == cleanRight)
    {
        return std::string(cleanLeft);
    }
    if (IsUnsignedInteger(cleanLeft) || IsUnsignedInteger(cleanRight))
    {
        return "uint";
    }
    return "int";
}

static std::string ResolveBinaryShiftBitwise(std::string_view op, std::string_view cleanLeft,
                                             std::string_view cleanRight, const SymbolTable* symbolTable = nullptr)
{
    if (op == "<<" || op == ">>" || op == ">>>")
    {
        return ResolveBinaryShift(cleanLeft);
    }
    if (op == "|" || op == "&" || op == "^")
    {
        return ResolveBinaryBitwiseLogic(cleanLeft, cleanRight, symbolTable);
    }
    return "";
}

static std::string ResolveBinary64OrFloat(std::string_view cleanLeft, std::string_view cleanRight)
{
    if (cleanLeft == "double" || cleanRight == "double")
    {
        return "double";
    }
    if (cleanLeft == "float" || cleanRight == "float")
    {
        return "float";
    }
    if (cleanLeft == "uint64" || cleanRight == "uint64")
    {
        return "uint64";
    }
    if (cleanLeft == "int64" || cleanRight == "int64")
    {
        return "int64";
    }
    return "";
}

static std::string ResolveBinary32OrLess(std::string_view cleanLeft, std::string_view cleanRight)
{
    if (cleanLeft == "void" || cleanRight == "void")
    {
        return "";
    }
    if (cleanLeft == "uint" || cleanRight == "uint" || cleanLeft == "uint32" || cleanRight == "uint32")
    {
        return "uint";
    }
    if (IsPrimitiveTypeName(cleanLeft) && IsPrimitiveTypeName(cleanRight))
    {
        return "int";
    }
    return "";
}

static std::string ResolveBinaryDivisionPromotion(std::string_view cleanLeft, std::string_view cleanRight,
                                                  bool disableIntegerDivision)
{
    if (cleanLeft == "double" || cleanRight == "double")
    {
        return "double";
    }
    if (IsFloatingPointPrimitive(cleanLeft) || IsFloatingPointPrimitive(cleanRight))
    {
        return "float";
    }
    if (!disableIntegerDivision && IsIntegerPrimitive(cleanLeft) && IsIntegerPrimitive(cleanRight))
    {
        return ResolveBinary32OrLess(cleanLeft, cleanRight);
    }
    return "float";
}

std::string ResolveBinaryPrimitivePromotion(std::string_view op, std::string_view cleanLeft,
                                            std::string_view cleanRight, bool disableIntegerDivision,
                                            const SymbolTable* symbolTable)
{
    if (cleanLeft.empty() || cleanRight.empty() || cleanLeft == "void" || cleanRight == "void")
    {
        return "";
    }
    if (op == "/" && IsFloatOrIntPrimitive(cleanLeft) && IsFloatOrIntPrimitive(cleanRight))
    {
        return ResolveBinaryDivisionPromotion(cleanLeft, cleanRight, disableIntegerDivision);
    }
    if (auto wide = ResolveBinary64OrFloat(cleanLeft, cleanRight); !wide.empty())
    {
        return wide;
    }
    if (auto shiftRes = ResolveBinaryShiftBitwise(op, cleanLeft, cleanRight, symbolTable); !shiftRes.empty())
    {
        return shiftRes;
    }
    return ResolveBinary32OrLess(cleanLeft, cleanRight);
}

} // namespace angel_lsp::analysis
