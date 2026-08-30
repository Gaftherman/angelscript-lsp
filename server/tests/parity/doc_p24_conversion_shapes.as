// Four shapes the conversion pass reported over the corpus, each one legal. Verified against the
// oracle together, as this file, and separately one at a time while each fix was written.
//
//   1. An enum widens out to an integer, as an argument as much as in an assignment. Only the
//      inward direction is closed - `Color c = 1;` is the error, doc_r09. Eleven findings.
//   2. A class declared inside a namespace can be constructed by its unqualified name from inside
//      that namespace. Its constructor is keyed `Hooks::Hook::Hook`, and the two keys built from
//      the written spelling reached neither, so every such construction read as having none. Ten
//      findings.
//   3. `array<T> a(33);` passes the count to the container's initial-size constructor. The
//      conversion pass was comparing it against the *element* type - "no conversion from 'int' to
//      'PlayerSlide'". Three findings.
//   4. `auto` deduced from a call, then passed on. There is no type to judge until the compiler
//      deduces one. Four findings.
enum Mode
{
    ModeOne = 1,
    ModeTwo = 2
}

void TakeFlags(int flags) {}
void TakeCount(uint count) {}

class Element
{
    int value;
}

namespace Hooks
{
    class Hook
    {
        string name;

        Hook(const string &in hookName)
        {
            this.name = hookName;
        }
    }

    Hook@ Make()
    {
        // Unqualified, from inside the namespace it is declared in.
        return @Hook("OnMapActivate");
    }
}

Element@ MakeElement()
{
    return Element();
}

void main()
{
    TakeFlags(ModeOne);
    TakeCount(ModeTwo);
    int widened = ModeOne;

    Hooks::Hook@ hook = Hooks::Make();

    array<Element> elements(33);
    array<int> counts(8);

    auto@ deduced = MakeElement();
    deduced.value = 1;
}
