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

    // =====================================================================================
    // The rest, and why they arrived in one block.
    //
    // This header held 48 constants while 148 codes were being emitted, 100 of them as raw string
    // literals at 333 sites. CLAUDE.md has always said every new diagnostic must be registered
    // here, and nothing checked it: check-diagnostic-codes.py verified that a code and its message
    // agree, which is a different question and passed the whole time.
    //
    // Registering them is what lets the compiler catch a typo. A misspelled literal at an emit site
    // produces a diagnostic the message table has never heard of, and the user sees a raw code.
    // Converting the 333 emit sites to name these constants is mechanical and comes separately;
    // this block is what makes that conversion possible, and what the guard now enforces for
    // anything new.
    // =====================================================================================
    inline constexpr std::string_view AbstractInstantiated                 = "as-err-abstract-instantiated";
    inline constexpr std::string_view ArrayInvalidTemplate                 = "as-err-array-invalid-template";
    inline constexpr std::string_view AttributeRepeated                    = "as-err-attribute-repeated";
    inline constexpr std::string_view AutoRequiresInitializer              = "as-err-auto-requires-initializer";
    inline constexpr std::string_view BinaryOperatorArity                  = "as-err-binary-operator-arity";
    inline constexpr std::string_view CannotInferNull                      = "as-err-cannot-infer-null";
    inline constexpr std::string_view CannotInferVoid                      = "as-err-cannot-infer-void";
    inline constexpr std::string_view CannotReturnLocalRef                 = "as-err-cannot-return-local-ref";
    inline constexpr std::string_view CannotReturnParamRef                 = "as-err-cannot-return-param-ref";
    inline constexpr std::string_view CharacterLiteralIsString             = "as-err-character-literal-is-string";
    inline constexpr std::string_view CircularInherit                      = "as-err-circular-inherit";
    inline constexpr std::string_view ClassMemberConst                     = "as-err-class-member-const";
    inline constexpr std::string_view CompoundAssignOnIndexedProp          = "as-err-compound-assign-on-indexed-prop";
    inline constexpr std::string_view CompoundAssignOnValueProp            = "as-err-compound-assign-on-value-prop";
    inline constexpr std::string_view ConstOutParam                        = "as-err-const-out-param";
    inline constexpr std::string_view ConstVoidReturn                      = "as-err-const-void-return";
    inline constexpr std::string_view ConstructorDelegationDisallowed      = "as-err-constructor-delegation-disallowed";
    inline constexpr std::string_view ConstructorNotCallable               = "as-err-constructor-not-callable";
    inline constexpr std::string_view CyclicAutoDependency                 = "as-err-cyclic-auto-dependency";
    inline constexpr std::string_view DeclarationMissingBody               = "as-err-declaration-missing-body";
    inline constexpr std::string_view DefaultParamOrder                    = "as-err-default-param-order";
    inline constexpr std::string_view DeleteNotAutoGenerated               = "as-err-delete-not-auto-generated";
    inline constexpr std::string_view DeleteWithBody                       = "as-err-delete-with-body";
    inline constexpr std::string_view DeleteWithOtherQualifier             = "as-err-delete-with-other-qualifier";
    inline constexpr std::string_view DeletedMethodCalled                  = "as-err-deleted-method-called";
    inline constexpr std::string_view DestructorDelete                     = "as-err-destructor-delete";
    inline constexpr std::string_view DestructorParam                      = "as-err-destructor-param";
    inline constexpr std::string_view DestructorReturnType                 = "as-err-destructor-return-type";
    inline constexpr std::string_view DoubleReference                      = "as-err-double-reference";
    inline constexpr std::string_view DuplicateParam                       = "as-err-duplicate-param";
    inline constexpr std::string_view EmptyListElement                     = "as-err-empty-list-element";
    inline constexpr std::string_view EnumInvalidInitializer               = "as-err-enum-invalid-initializer";
    inline constexpr std::string_view ExplicitNotMember                    = "as-err-explicit-not-member";
    inline constexpr std::string_view ForeachUnsupported                   = "as-err-foreach-unsupported";
    inline constexpr std::string_view FuncdefAttribute                     = "as-err-funcdef-attribute";
    inline constexpr std::string_view FuncdefNotHandle                     = "as-err-funcdef-not-handle";
    inline constexpr std::string_view GlobalFunctionQualifiers             = "as-err-global-function-qualifiers";
    inline constexpr std::string_view GlobalVariableAccessModifier         = "as-err-global-variable-access-modifier";
    inline constexpr std::string_view ImportHasBody                        = "as-err-import-has-body";
    inline constexpr std::string_view IncDecOnVirtualProp                  = "as-err-inc-dec-on-virtual-prop";
    inline constexpr std::string_view InheritFinal                         = "as-err-inherit-final";
    inline constexpr std::string_view InitializerListExpected              = "as-err-initializer-list-expected";
    inline constexpr std::string_view InitializerListNotSupported          = "as-err-initializer-list-not-supported";
    inline constexpr std::string_view InitializerListTooFew                = "as-err-initializer-list-too-few";
    inline constexpr std::string_view InitializerListTooMany               = "as-err-initializer-list-too-many";
    inline constexpr std::string_view InterfaceConstructor                 = "as-err-interface-constructor";
    inline constexpr std::string_view InterfaceImplMissing                 = "as-err-interface-impl-missing";
    inline constexpr std::string_view InterfaceInstantiated                = "as-err-interface-instantiated";
    inline constexpr std::string_view InterfaceMethodAttribute             = "as-err-interface-method-attribute";
    inline constexpr std::string_view InvalidCast                          = "as-err-invalid-cast";
    inline constexpr std::string_view InvalidForeachContainer              = "as-err-invalid-foreach-container";
    inline constexpr std::string_view MissingBody                          = "as-err-missing-body";
    inline constexpr std::string_view MixinAbstract                        = "as-err-mixin-abstract";
    inline constexpr std::string_view MixinChildType                       = "as-err-mixin-child-type";
    inline constexpr std::string_view MixinConstructor                     = "as-err-mixin-constructor";
    inline constexpr std::string_view MixinDestructor                      = "as-err-mixin-destructor";
    inline constexpr std::string_view MixinInheritClass                    = "as-err-mixin-inherit-class";
    inline constexpr std::string_view MixinVirtualProperty                 = "as-err-mixin-virtual-property";
    inline constexpr std::string_view MultiClassInherit                    = "as-err-multi-class-inherit";
    inline constexpr std::string_view MultilineString                      = "as-err-multiline-string";
    inline constexpr std::string_view NameConflict                         = "as-err-name-conflict";
    inline constexpr std::string_view NamedArgumentSyntax                  = "as-err-named-argument-syntax";
    inline constexpr std::string_view NoDefaultConstructor                 = "as-err-no-default-constructor";
    inline constexpr std::string_view NoExplicitConversion                 = "as-err-no-explicit-conversion";
    inline constexpr std::string_view NullNonHandle                        = "as-err-null-non-handle";
    inline constexpr std::string_view OpOverloadGlobal                     = "as-err-op-overload-global";
    inline constexpr std::string_view OpcmpReturnInt                       = "as-err-opcmp-return-int";
    inline constexpr std::string_view OpequalsReturnBool                   = "as-err-opequals-return-bool";
    inline constexpr std::string_view OpindexNoParams                      = "as-err-opindex-no-params";
    inline constexpr std::string_view OutParamDefault                      = "as-err-out-param-default";
    inline constexpr std::string_view OverrideFinalMethod                  = "as-err-override-final-method";
    inline constexpr std::string_view OverrideNoBase                       = "as-err-override-no-base";
    inline constexpr std::string_view ParameterNotInstantiable             = "as-err-parameter-not-instantiable";
    inline constexpr std::string_view PropertyAccessorMissingBody          = "as-err-property-accessor-missing-body";
    inline constexpr std::string_view PropertyDuplicateAccessor            = "as-err-property-duplicate-accessor";
    inline constexpr std::string_view PropertyTypeMismatch                 = "as-err-property-type-mismatch";
    inline constexpr std::string_view ReadOnlyProperty                     = "as-err-read-only-property";
    inline constexpr std::string_view RefTypeBoolConvDisallowed            = "as-err-ref-type-bool-conv-disallowed";
    inline constexpr std::string_view ReturnNotInstantiable                = "as-err-return-not-instantiable";
    inline constexpr std::string_view SignatureMismatchFuncHandle          = "as-err-signature-mismatch-func-handle";
    inline constexpr std::string_view TemplateClassNotSupported            = "as-err-template-class-not-supported";
    inline constexpr std::string_view ValueAssignForRef                    = "as-err-value-assign-for-ref";
    inline constexpr std::string_view VirtualPropertySignature             = "as-err-virtual-property-signature";
    inline constexpr std::string_view VoidParameter                        = "as-err-void-parameter";
    inline constexpr std::string_view VoidReference                        = "as-err-void-reference";
    inline constexpr std::string_view VoidReturnValue                      = "as-err-void-return-value";
    inline constexpr std::string_view WriteOnlyProperty                    = "as-err-write-only-property";
    inline constexpr std::string_view AccessorPortability                  = "as-hint-accessor-portability";
    inline constexpr std::string_view BoolConversion                       = "as-hint-bool-conversion";
    inline constexpr std::string_view FuncdefMissing                       = "as-hint-funcdef-missing";
    inline constexpr std::string_view IntegerDivision                      = "as-hint-integer-division";
    inline constexpr std::string_view ListPatternUnknown                   = "as-hint-list-pattern-unknown";
    inline constexpr std::string_view SyntaxError                                = "as-syntax-error";
    inline constexpr std::string_view SyntaxErrorGeneric                         = "as-syntax-error-generic";
    inline constexpr std::string_view SyntaxErrorMissing                         = "as-syntax-error-missing";
    inline constexpr std::string_view GlobalFunctionAttribute              = "as-warn-global-function-attribute";
    inline constexpr std::string_view IncludeNotFound                      = "as-warn-include-not-found";
    inline constexpr std::string_view UndeclaredIdentifier                 = "as-warn-undeclared-identifier";
    inline constexpr std::string_view UnreachableCode                      = "as-warn-unreachable-code";
    inline constexpr std::string_view UnusedVariable                       = "as-warn-unused-variable";
}
