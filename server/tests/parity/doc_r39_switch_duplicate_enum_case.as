// Two enum names holding the same number are one case label twice.
//
//     angelscript_oracle doc_r39_switch_duplicate_enum_case.as
//         ERROR (15, 14): Duplicate switch case
//
// The duplicate is in the VALUES, not the spelling: `A` and `B` are different identifiers and both
// are 1. A switch dispatches on the number, so the second label can never be reached.
enum EDuplicate { A = 1, B = 1 }

int test(EDuplicate e)
{
    switch (e)
    {
        case A: return 1;
        case B: return 2;
    }
    return 0;
}
