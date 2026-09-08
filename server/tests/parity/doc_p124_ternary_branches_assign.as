// A conditional whose branches are assignments, used as a statement.
//
//     angelscript_oracle doc_p124_ternary_branches_assign.as
//         (accepted, no output)
//
// Found on a real Cry of Fear weapon script, which writes it exactly this way:
//
//     (g_iMode == MODE_SLASH) ? pev.punchangle.y = RandomLong(-3, -2)
//                             : pev.punchangle.x = RandomLong(2, 3);
//
// Three errors on one file. The second half of the file covers the shape it was being confused
// with - assigning to the conditional itself - because that is legal too, which is what makes
// staying silent here cost nothing.
class P124Angles
{
    float x;
    float y;
}

int P124Pick(int low, int high) { return low; }

void main()
{
    P124Angles angles;
    bool p124Slash = true;

    // The branches are the assignments.
    p124Slash ? angles.y = 1.0f : angles.x = 2.0f;

    // And the conditional itself is an assignable l-value.
    (p124Slash ? angles.y : angles.x) = 3.0f;

    // With a call on the right, as the real script has.
    p124Slash ? angles.y = P124Pick(-3, -2) : angles.x = P124Pick(2, 3);
}
