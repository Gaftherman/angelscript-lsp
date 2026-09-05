// An enum value is not a condition.
//
//     angelscript_oracle doc_r35_enum_as_if_condition.as
//         ERROR (14, 9): Expression must be of boolean type, instead found 'ECondition'
//
// AngelScript has no conversion from an enum to bool, the same way it has none from an int. The
// enum is fully visible here - declared in this script - which is what makes it safe to report;
// a type this analyzer cannot find a declaration for is left alone.
enum ECondition { A = 1, B = 2 }

void test()
{
    ECondition e = A;
    if (e)
    {
        e = B;
    }
}
