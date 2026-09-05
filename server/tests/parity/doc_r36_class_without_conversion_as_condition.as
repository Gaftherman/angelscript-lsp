// A class instance is not a condition when nothing converts it.
//
//     angelscript_oracle doc_r36_class_without_conversion_as_condition.as
//         ERROR (15, 9): Expression must be of boolean type, instead found 'NoConversion&'
//
// The class is declared right here and declares neither opImplConv nor opConv, so there is no
// reading of any engine setting under which this compiles. A class that DOES declare one is a
// different case and is deliberately left to the opt-in hint: asEP_BOOL_CONVERSION_MODE is a host
// setting this analyzer only models, and mode 1 would make that code legal.
class NoConversion { int v; }

void test()
{
    NoConversion c;
    if (c)
    {
        c.v = 1;
    }
}
