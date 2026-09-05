#pragma once

#include <string_view>

namespace angel_lsp::diagnostics::codes
{
    // Type and Declaration Errors
    inline constexpr std::string_view UnknownType = "as-err-unresolved-type";
    inline constexpr std::string_view DuplicateDeclaration = "as-err-duplicate-symbol";
    inline constexpr std::string_view VoidVariable = "as-err-void-variable";
    inline constexpr std::string_view HandleOnPrimitive = "as-err-handle-on-primitive";
    inline constexpr std::string_view ReservedKeywordName = "as-err-reserved-keyword-name";
    inline constexpr std::string_view MixinNotAType = "as-err-mixin-not-a-type";
    inline constexpr std::string_view MixinFinal = "as-err-mixin-final";
    inline constexpr std::string_view UndefinedNamespace = "as-err-undefined-namespace";
    inline constexpr std::string_view UndefinedIdentifier = "as-err-undefined-identifier";
    inline constexpr std::string_view TypedefNonPrimitive = "as-err-typedef-non-primitive";

    // Access and Visibility Errors
    inline constexpr std::string_view PrivateMemberAccess = "as-err-private-member-access";
    inline constexpr std::string_view ProtectedMemberAccess = "as-err-protected-member-access";
    inline constexpr std::string_view MemberNotFound = "as-err-member-not-found";
    inline constexpr std::string_view LambdaClosureDisallowed = "as-err-lambda-closure-disallowed";

    // Call and Overload Errors
    inline constexpr std::string_view CallArgumentCount = "as-err-call-argument-count";
    inline constexpr std::string_view CallAmbiguous = "as-err-call-ambiguous";
    inline constexpr std::string_view CallNoMatchingSignature = "as-err-call-no-matching-signature";
    inline constexpr std::string_view NoMatchingConstructor = "as-err-no-matching-constructor";
    inline constexpr std::string_view NoImplicitConversion = "as-err-no-implicit-conversion";
    inline constexpr std::string_view PositionalAfterNamedArg = "as-err-positional-after-named-arg";
    inline constexpr std::string_view LValueRequiredForOutParam = "as-err-lvalue-required-for-out-param";

    // Const and Assignment Errors
    inline constexpr std::string_view ConstAssignment = "as-err-const-assignment";
    inline constexpr std::string_view ConstMethodRequired = "as-err-const-method-required";
    inline constexpr std::string_view NotLValue = "as-err-not-lvalue";
    inline constexpr std::string_view AssignVoid = "as-err-assign-void";
    inline constexpr std::string_view AssignNonRefCall = "as-err-assign-non-ref-call";

    // Control Flow Errors
    inline constexpr std::string_view BreakOutsideLoop = "as-err-break-outside-loop";
    inline constexpr std::string_view ContinueOutsideLoop = "as-err-continue-outside-loop";
    inline constexpr std::string_view NotAllPathsReturn = "as-err-not-all-paths-return";
    inline constexpr std::string_view ReturnValueRequired = "as-err-return-value-required";
    inline constexpr std::string_view ConditionNotBoolean = "as-err-condition-not-boolean";
    inline constexpr std::string_view InvalidCaseType = "as-err-invalid-case-type";
    inline constexpr std::string_view CaseNotConstant = "as-err-case-not-constant";
    inline constexpr std::string_view DuplicateCaseValue = "as-err-duplicate-case-value";
    inline constexpr std::string_view DefaultMustBeLast = "as-err-default-must-be-last";
    inline constexpr std::string_view IfEmptyStatement = "as-err-if-empty-statement";
    inline constexpr std::string_view ElseEmptyStatement = "as-err-else-empty-statement";

    // Shared and Isolation Errors
    inline constexpr std::string_view SharedNotAllowedOnEntity = "as-err-shared-not-allowed-on-entity";
    inline constexpr std::string_view SharedCannotAccessNonShared = "as-err-shared-cannot-access-non-shared";
    inline constexpr std::string_view ExternalNotFound = "as-err-external-not-found";
    inline constexpr std::string_view ExternalNotShared = "as-err-external-not-shared";

    // Engine Property Disallowed Errors
    inline constexpr std::string_view PrimitiveInoutRefDisallowed = "as-err-inout-on-primitive";
    inline constexpr std::string_view GlobalVarsDisallowed = "as-err-global-vars-disallowed";

    // Numeric Conversion Warnings
    //
    // The compiler emits five of these; these are the two it decides from types alone. The other
    // three - "Implicit conversion changed sign of value", "Value is too large for data type" and
    // "Implicit conversion of value is not exact" - fire only on constant expressions and need a
    // constant folder to answer. See TypeConversionChecker.cpp's numeric-warning section.
    inline constexpr std::string_view AccessorDisabled = "as-hint-accessor-disabled";
    inline constexpr std::string_view UnsupportedDirective = "as-warn-unsupported-directive";
    inline constexpr std::string_view SignedUnsignedMismatch = "as-warn-signed-unsigned-mismatch";
    inline constexpr std::string_view FloatTruncation = "as-warn-float-truncation";

    // Definite Assignment Warnings
    //
    // A warning, not an error, which is what the compiler answers: reading a local before
    // writing it is accepted with `WARNING: 'n' is not initialized.` in every shape measured.
    inline constexpr std::string_view UninitializedVariableRead = "as-warn-uninitialized-variable-read";
}
