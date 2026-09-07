"""The 52 operator overloads, each measured against the compiler and against this analyzer.

Two files, so one oracle run and one server run cover the lot:

  positive.as  every operator, on a class that declares it     -> the compiler should accept
  negative.as  the same expression, on a class that does not   -> the compiler should reject

Each case gets its own class and its own function, so a verdict maps back to an operator by the
class name in the message or by the line.

SELF is the class under test and OTHER the unrelated class the cast operators target. Placeholders
rather than a bare `T`, because `T` is also a perfectly good substring of nothing here and a plain
replace of a single letter is how a generator quietly writes something else than it says.
"""
import io
import os

# name, declaration inside the class, statement exercising it
OPERATORS = [
    # 1. Assignment and compound assignment
    ("opAssign",      "SELF& opAssign(const SELF &in o)",  "value = other;"),
    ("opHndlAssign",  "SELF@ opHndlAssign(const SELF @h)", "@handle = @other;"),
    ("opAddAssign",   "SELF& opAddAssign(int x)",          "value += 1;"),
    ("opSubAssign",   "SELF& opSubAssign(int x)",          "value -= 1;"),
    ("opMulAssign",   "SELF& opMulAssign(int x)",          "value *= 2;"),
    ("opDivAssign",   "SELF& opDivAssign(int x)",          "value /= 2;"),
    ("opModAssign",   "SELF& opModAssign(int x)",          "value %= 2;"),
    ("opPowAssign",   "SELF& opPowAssign(int x)",          "value **= 2;"),
    ("opAndAssign",   "SELF& opAndAssign(uint x)",         "value &= 1;"),
    ("opOrAssign",    "SELF& opOrAssign(uint x)",          "value |= 1;"),
    ("opXorAssign",   "SELF& opXorAssign(uint x)",         "value ^= 1;"),
    ("opShlAssign",   "SELF& opShlAssign(int s)",          "value <<= 1;"),
    ("opShrAssign",   "SELF& opShrAssign(int s)",          "value >>= 1;"),
    ("opUShrAssign",  "SELF& opUShrAssign(int s)",         "value >>>= 1;"),

    # 2. Binary, object on the left
    ("opAdd",   "SELF opAdd(const SELF &in o) const", "SELF result = value + other;"),
    ("opSub",   "SELF opSub(const SELF &in o) const", "SELF result = value - other;"),
    ("opMul",   "SELF opMul(int s) const",            "SELF result = value * 2;"),
    ("opDiv",   "SELF opDiv(int s) const",            "SELF result = value / 2;"),
    ("opMod",   "SELF opMod(int m) const",            "SELF result = value % 2;"),
    ("opPow",   "SELF opPow(int p) const",            "SELF result = value ** 2;"),
    ("opAnd",   "SELF opAnd(uint m) const",           "SELF result = value & 1;"),
    ("opOr",    "SELF opOr(uint m) const",            "SELF result = value | 1;"),
    ("opXor",   "SELF opXor(uint m) const",           "SELF result = value ^ 1;"),
    ("opShl",   "SELF opShl(int s) const",            "SELF result = value << 1;"),
    ("opShr",   "SELF opShr(int s) const",            "SELF result = value >> 1;"),
    ("opUShr",  "SELF opUShr(int s) const",           "SELF result = value >>> 1;"),

    # 3. Binary, object on the right
    ("opAdd_r",   "SELF opAdd_r(int b) const",   "SELF result = 1 + value;"),
    ("opSub_r",   "SELF opSub_r(int b) const",   "SELF result = 1 - value;"),
    ("opMul_r",   "SELF opMul_r(int b) const",   "SELF result = 2 * value;"),
    ("opDiv_r",   "SELF opDiv_r(int b) const",   "SELF result = 2 / value;"),
    ("opMod_r",   "SELF opMod_r(int b) const",   "SELF result = 2 % value;"),
    ("opPow_r",   "SELF opPow_r(int b) const",   "SELF result = 2 ** value;"),
    ("opAnd_r",   "SELF opAnd_r(uint b) const",  "SELF result = 1 & value;"),
    ("opOr_r",    "SELF opOr_r(uint b) const",   "SELF result = 1 | value;"),
    ("opXor_r",   "SELF opXor_r(uint b) const",  "SELF result = 1 ^ value;"),
    ("opShl_r",   "SELF opShl_r(int b) const",   "SELF result = 1 << value;"),
    ("opShr_r",   "SELF opShr_r(int b) const",   "SELF result = 1 >> value;"),
    ("opUShr_r",  "SELF opUShr_r(uint b) const", "SELF result = 1 >>> value;"),

    # 4. Unary, increment and decrement
    ("opNeg",     "SELF opNeg() const", "SELF result = -value;"),
    ("opCom",     "SELF opCom() const", "SELF result = ~value;"),
    ("opPreInc",  "SELF& opPreInc()",   "++value;"),
    ("opPostInc", "SELF opPostInc()",   "value++;"),
    ("opPreDec",  "SELF& opPreDec()",   "--value;"),
    ("opPostDec", "SELF opPostDec()",   "value--;"),

    # 5. Comparison
    ("opEquals", "bool opEquals(const SELF &in o) const", "bool result = (value == other);"),
    ("opCmp",    "int opCmp(const SELF &in o) const",     "bool result = (value < other);"),

    # 6. Access and indexing
    ("opIndex", "int& opIndex(int idx)", "int result = value[0];"),
    ("opCall",  "void opCall(int x)",    "value(1);"),

    # 7. Casting and conversion
    ("opConv",     "int opConv() const",     "int result = int(value);"),
    ("opImplConv", "int opImplConv() const", "int result = value;"),
    ("opCast",     "OTHER@ opCast()",        "OTHER@ result = cast<OTHER@>(value);"),
    ("opImplCast", "OTHER@ opImplCast()",    "OTHER@ result = value;"),
]


def return_expression(declaration):
    """A body the declared return type accepts, so the positive file compiles."""
    head = declaration.split('(')[0].strip()
    if head.startswith('void'):
        return ''
    if head.startswith('bool'):
        return 'true'
    if head.startswith('int&'):
        return 'stored'
    if head.startswith('int') or head.startswith('uint'):
        return '0'
    if head.startswith('OTHER'):
        return 'null'
    return 'this'


def render(index, name, declaration, usage, declare_operator):
    self_name = 'Op%02d%s' % (index, name.replace('_', 'R'))
    other_name = 'Other%02d' % index

    def fill(text):
        return text.replace('SELF', self_name).replace('OTHER', other_name)

    lines = []
    lines.append('class %s' % other_name)
    lines.append('{')
    lines.append('    int filler;')
    lines.append('}')
    lines.append('class %s' % self_name)
    lines.append('{')
    lines.append('    int stored;')
    if declare_operator:
        body = return_expression(declaration)
        lines.append('    %s { %s }' % (fill(declaration),
                                        ('return %s;' % body) if body else ''))
    lines.append('}')
    lines.append('void Use%02d%s()' % (index, name.replace('_', 'R')))
    lines.append('{')
    lines.append('    %s value;' % self_name)
    lines.append('    %s other;' % self_name)
    lines.append('    %s@ handle;' % self_name)
    lines.append('    %s' % fill(usage))
    lines.append('}')
    return '\n'.join(lines)


def build(declare_operator):
    out = ['// Generated by operators.py - one class per operator.']
    for i, (name, decl, use) in enumerate(OPERATORS, start=1):
        out.append(render(i, name, decl, use, declare_operator))
        out.append('')
    return '\n'.join(out)


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    target = os.path.join(here, 'ops')
    os.makedirs(target, exist_ok=True)

    for label, declare in (('positive', True), ('negative', False)):
        path = os.path.join(target, '%s.as' % label)
        io.open(path, 'w', encoding='utf-8', newline='\n').write(build(declare) + '\n')
        print('wrote %s (%d operators)' % (path, len(OPERATORS)))
