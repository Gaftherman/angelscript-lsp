// Everything the compiler accepts around an enum, next to doc_r09 which is what it rejects. An
// enum is a sink: nothing reaches it implicitly but itself, while widening out of it is fine.
enum Color { Red = 1, Green = 2 }
enum Other { OtherOne }
class W { Color opImplConv() const { return Red; } }

void main()
{
    Color a = Red;
    Color b = Color::Green;
    Color c = a;
    int widened = Color::Red;
    int arithmetic = a + 1;
    W w;
    Color viaOperator = w;
    Color viaCast = Color(1);

    switch (a) { case Red: break; default: break; }
    print("" + widened + arithmetic + b + c + viaOperator + viaCast + OtherOne);
}
