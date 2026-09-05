// Case labels written with the enum's own scope.
//
//     angelscript_oracle doc_p39_switch_qualified_enum_labels.as   accepted
//
// `case ELabels::A:` and the bare `case A:` are both legal, and the qualified spelling is the one a
// case-label rule is most likely to fail to resolve and report as a non-constant.
enum ELabels { A = 1, B = 2 }

int test(ELabels e)
{
    switch (e)
    {
        case ELabels::A: return 1;
        case ELabels::B: return 2;
    }
    return 0;
}
