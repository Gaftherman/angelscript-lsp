// The other direction, and the guard that keeps doc_p23 from becoming a blanket exemption. A
// `string` reaches nothing implicitly:
//
//     int i = s;    Can't implicitly convert from 'string' to 'int'.
//     bool b = s;   Can't implicitly convert from 'string' to 'bool'.
//     float f = s;  Can't implicitly convert from 'string' to 'float'.
//     Flag f = s;   Can't implicitly convert from 'string' to 'Flag'.
//
// `string` is a sink: everything primitive flows in, nothing flows out. Widening it into "string
// and primitives are interchangeable" would have silenced all four of these.
enum Flag
{
    FlagOne = 1
}

void main()
{
    string s = "5";
    int fromString = s;
}
