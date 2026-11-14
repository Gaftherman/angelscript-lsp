// The accepting side, and the reason the rule above is about keywords and nothing else.
//
//     angelscript_oracle doc_p15_shadowing_a_type_name.as   accepted
//
// A local may carry the name of a class, an enum or a function declared in the same script: the
// compiler accepts all three, measured. Only a GLOBAL clashing with a class is a name conflict, and
// as-err-name-conflict already reports that. A local named after a type the *application* registers
// is rejected too, but that is a fact about the host rather than about the language, and silence is
// the right answer where this analyzer cannot see the registration.
//
// `matrix` rather than `grid` on the last line for exactly that reason: `grid` is a registered type
// in some engine builds and the declaration is refused for the name, not for the nesting.
class myClass {}
enum Color { Red }
void helper() {}

void test()
{
    int myClass = 1;
    int Color = 2;
    int helper = 3;
    array<array<int>> matrix;

    matrix.insertLast(array<int>());
    myClass += Color + helper;
}
