// Two levels of array around a dictionary.
//
//     angelscript_oracle doc_p41_array_of_array_of_dictionary.as   accepted
//
// The nesting is the point: `array<array<dictionary>>` needs the '>>' read as two closers rather
// than a shift operator, and needs the inner `array<dictionary>()` constructor call to resolve.
void test()
{
    array<array<dictionary>> board;
    board.insertLast(array<dictionary>());

    dictionary cell;
    cell.set("v", 1);
    board[0].insertLast(cell);
}
