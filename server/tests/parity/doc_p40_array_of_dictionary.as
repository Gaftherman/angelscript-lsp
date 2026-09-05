// An array whose element type is a dictionary.
//
//     angelscript_oracle doc_p40_array_of_dictionary.as   accepted
//
// Both halves matter: putting a dictionary into the array, and reading an element back out into a
// dictionary variable, which is where a template-argument resolver tends to lose the element type.
void test()
{
    array<dictionary> rows;

    dictionary row;
    row.set("count", 1);
    rows.insertLast(row);

    dictionary first = rows[0];
    first.set("count", 2);
}
