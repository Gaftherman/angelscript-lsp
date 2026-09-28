// A duplicate named argument.
//
//     angelscript_oracle doc_r96_duplicate_named_argument.as
//         ERROR (10, 19): Duplicate named argument 'id'
void R96Take(int id, bool f, int other) {}

void main()
{
    R96Take(f: true, id: 1, id: 2);
}
