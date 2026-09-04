// A constructor returns void, and nothing was checking it.
//
//     angelscript_oracle doc_r12_constructor_returns_value.as
//         ERROR (9, 9): Can't return value when return type is 'void'
//
// as-err-void-return-value covers the same mistake in a function that spells `void` out, and it
// reads the return type node - which a constructor does not have. So the one function in the
// language that can only return void was the one nothing checked.
class Widget
{
    Widget()
    {
        return 42;
    }
}
