// Every one of AngelScript's 52 operator overloads, declared and used.
//
//     angelscript_oracle doc_p123_operator_overloads_declared.as
//         (accepted, no output - all 52)
//
// One class per operator, so a verdict names the operator that produced it. Generated, and the
// generator is worth keeping in mind when reading this: each case is the declaration from
// sdk/docs/doxygen/source/doc_script_class_ops.h paired with the expression that calls it.
//
// Written after measuring the whole set at once. Twelve of the 52 were reported on code the
// compiler accepts, all of them compound assignments: `value += 1` on a class declaring
// opAddAssign(int) was read as a conversion from int to that class, because the assignment check
// compared the two sides and never looked at the operator between them. This file is what keeps
// that from coming back one operator at a time.
//
// The counterpart is doc_g01, which uses each operator on a class that does NOT declare it.

class Other01
{
    int filler;
}
class Op01opAssign
{
    int stored;
    Op01opAssign& opAssign(const Op01opAssign &in o) { return this; }
}
void Use01opAssign()
{
    Op01opAssign value;
    Op01opAssign other;
    Op01opAssign@ handle;
    value = other;
}

class Other02
{
    int filler;
}
class Op02opHndlAssign
{
    int stored;
    Op02opHndlAssign@ opHndlAssign(const Op02opHndlAssign @h) { return this; }
}
void Use02opHndlAssign()
{
    Op02opHndlAssign value;
    Op02opHndlAssign other;
    Op02opHndlAssign@ handle;
    @handle = @other;
}

class Other03
{
    int filler;
}
class Op03opAddAssign
{
    int stored;
    Op03opAddAssign& opAddAssign(int x) { return this; }
}
void Use03opAddAssign()
{
    Op03opAddAssign value;
    Op03opAddAssign other;
    Op03opAddAssign@ handle;
    value += 1;
}

class Other04
{
    int filler;
}
class Op04opSubAssign
{
    int stored;
    Op04opSubAssign& opSubAssign(int x) { return this; }
}
void Use04opSubAssign()
{
    Op04opSubAssign value;
    Op04opSubAssign other;
    Op04opSubAssign@ handle;
    value -= 1;
}

class Other05
{
    int filler;
}
class Op05opMulAssign
{
    int stored;
    Op05opMulAssign& opMulAssign(int x) { return this; }
}
void Use05opMulAssign()
{
    Op05opMulAssign value;
    Op05opMulAssign other;
    Op05opMulAssign@ handle;
    value *= 2;
}

class Other06
{
    int filler;
}
class Op06opDivAssign
{
    int stored;
    Op06opDivAssign& opDivAssign(int x) { return this; }
}
void Use06opDivAssign()
{
    Op06opDivAssign value;
    Op06opDivAssign other;
    Op06opDivAssign@ handle;
    value /= 2;
}

class Other07
{
    int filler;
}
class Op07opModAssign
{
    int stored;
    Op07opModAssign& opModAssign(int x) { return this; }
}
void Use07opModAssign()
{
    Op07opModAssign value;
    Op07opModAssign other;
    Op07opModAssign@ handle;
    value %= 2;
}

class Other08
{
    int filler;
}
class Op08opPowAssign
{
    int stored;
    Op08opPowAssign& opPowAssign(int x) { return this; }
}
void Use08opPowAssign()
{
    Op08opPowAssign value;
    Op08opPowAssign other;
    Op08opPowAssign@ handle;
    value **= 2;
}

class Other09
{
    int filler;
}
class Op09opAndAssign
{
    int stored;
    Op09opAndAssign& opAndAssign(uint x) { return this; }
}
void Use09opAndAssign()
{
    Op09opAndAssign value;
    Op09opAndAssign other;
    Op09opAndAssign@ handle;
    value &= 1;
}

class Other10
{
    int filler;
}
class Op10opOrAssign
{
    int stored;
    Op10opOrAssign& opOrAssign(uint x) { return this; }
}
void Use10opOrAssign()
{
    Op10opOrAssign value;
    Op10opOrAssign other;
    Op10opOrAssign@ handle;
    value |= 1;
}

class Other11
{
    int filler;
}
class Op11opXorAssign
{
    int stored;
    Op11opXorAssign& opXorAssign(uint x) { return this; }
}
void Use11opXorAssign()
{
    Op11opXorAssign value;
    Op11opXorAssign other;
    Op11opXorAssign@ handle;
    value ^= 1;
}

class Other12
{
    int filler;
}
class Op12opShlAssign
{
    int stored;
    Op12opShlAssign& opShlAssign(int s) { return this; }
}
void Use12opShlAssign()
{
    Op12opShlAssign value;
    Op12opShlAssign other;
    Op12opShlAssign@ handle;
    value <<= 1;
}

class Other13
{
    int filler;
}
class Op13opShrAssign
{
    int stored;
    Op13opShrAssign& opShrAssign(int s) { return this; }
}
void Use13opShrAssign()
{
    Op13opShrAssign value;
    Op13opShrAssign other;
    Op13opShrAssign@ handle;
    value >>= 1;
}

class Other14
{
    int filler;
}
class Op14opUShrAssign
{
    int stored;
    Op14opUShrAssign& opUShrAssign(int s) { return this; }
}
void Use14opUShrAssign()
{
    Op14opUShrAssign value;
    Op14opUShrAssign other;
    Op14opUShrAssign@ handle;
    value >>>= 1;
}

class Other15
{
    int filler;
}
class Op15opAdd
{
    int stored;
    Op15opAdd opAdd(const Op15opAdd &in o) const { return this; }
}
void Use15opAdd()
{
    Op15opAdd value;
    Op15opAdd other;
    Op15opAdd@ handle;
    Op15opAdd result = value + other;
}

class Other16
{
    int filler;
}
class Op16opSub
{
    int stored;
    Op16opSub opSub(const Op16opSub &in o) const { return this; }
}
void Use16opSub()
{
    Op16opSub value;
    Op16opSub other;
    Op16opSub@ handle;
    Op16opSub result = value - other;
}

class Other17
{
    int filler;
}
class Op17opMul
{
    int stored;
    Op17opMul opMul(int s) const { return this; }
}
void Use17opMul()
{
    Op17opMul value;
    Op17opMul other;
    Op17opMul@ handle;
    Op17opMul result = value * 2;
}

class Other18
{
    int filler;
}
class Op18opDiv
{
    int stored;
    Op18opDiv opDiv(int s) const { return this; }
}
void Use18opDiv()
{
    Op18opDiv value;
    Op18opDiv other;
    Op18opDiv@ handle;
    Op18opDiv result = value / 2;
}

class Other19
{
    int filler;
}
class Op19opMod
{
    int stored;
    Op19opMod opMod(int m) const { return this; }
}
void Use19opMod()
{
    Op19opMod value;
    Op19opMod other;
    Op19opMod@ handle;
    Op19opMod result = value % 2;
}

class Other20
{
    int filler;
}
class Op20opPow
{
    int stored;
    Op20opPow opPow(int p) const { return this; }
}
void Use20opPow()
{
    Op20opPow value;
    Op20opPow other;
    Op20opPow@ handle;
    Op20opPow result = value ** 2;
}

class Other21
{
    int filler;
}
class Op21opAnd
{
    int stored;
    Op21opAnd opAnd(uint m) const { return this; }
}
void Use21opAnd()
{
    Op21opAnd value;
    Op21opAnd other;
    Op21opAnd@ handle;
    Op21opAnd result = value & 1;
}

class Other22
{
    int filler;
}
class Op22opOr
{
    int stored;
    Op22opOr opOr(uint m) const { return this; }
}
void Use22opOr()
{
    Op22opOr value;
    Op22opOr other;
    Op22opOr@ handle;
    Op22opOr result = value | 1;
}

class Other23
{
    int filler;
}
class Op23opXor
{
    int stored;
    Op23opXor opXor(uint m) const { return this; }
}
void Use23opXor()
{
    Op23opXor value;
    Op23opXor other;
    Op23opXor@ handle;
    Op23opXor result = value ^ 1;
}

class Other24
{
    int filler;
}
class Op24opShl
{
    int stored;
    Op24opShl opShl(int s) const { return this; }
}
void Use24opShl()
{
    Op24opShl value;
    Op24opShl other;
    Op24opShl@ handle;
    Op24opShl result = value << 1;
}

class Other25
{
    int filler;
}
class Op25opShr
{
    int stored;
    Op25opShr opShr(int s) const { return this; }
}
void Use25opShr()
{
    Op25opShr value;
    Op25opShr other;
    Op25opShr@ handle;
    Op25opShr result = value >> 1;
}

class Other26
{
    int filler;
}
class Op26opUShr
{
    int stored;
    Op26opUShr opUShr(int s) const { return this; }
}
void Use26opUShr()
{
    Op26opUShr value;
    Op26opUShr other;
    Op26opUShr@ handle;
    Op26opUShr result = value >>> 1;
}

class Other27
{
    int filler;
}
class Op27opAddRr
{
    int stored;
    Op27opAddRr opAdd_r(int b) const { return this; }
}
void Use27opAddRr()
{
    Op27opAddRr value;
    Op27opAddRr other;
    Op27opAddRr@ handle;
    Op27opAddRr result = 1 + value;
}

class Other28
{
    int filler;
}
class Op28opSubRr
{
    int stored;
    Op28opSubRr opSub_r(int b) const { return this; }
}
void Use28opSubRr()
{
    Op28opSubRr value;
    Op28opSubRr other;
    Op28opSubRr@ handle;
    Op28opSubRr result = 1 - value;
}

class Other29
{
    int filler;
}
class Op29opMulRr
{
    int stored;
    Op29opMulRr opMul_r(int b) const { return this; }
}
void Use29opMulRr()
{
    Op29opMulRr value;
    Op29opMulRr other;
    Op29opMulRr@ handle;
    Op29opMulRr result = 2 * value;
}

class Other30
{
    int filler;
}
class Op30opDivRr
{
    int stored;
    Op30opDivRr opDiv_r(int b) const { return this; }
}
void Use30opDivRr()
{
    Op30opDivRr value;
    Op30opDivRr other;
    Op30opDivRr@ handle;
    Op30opDivRr result = 2 / value;
}

class Other31
{
    int filler;
}
class Op31opModRr
{
    int stored;
    Op31opModRr opMod_r(int b) const { return this; }
}
void Use31opModRr()
{
    Op31opModRr value;
    Op31opModRr other;
    Op31opModRr@ handle;
    Op31opModRr result = 2 % value;
}

class Other32
{
    int filler;
}
class Op32opPowRr
{
    int stored;
    Op32opPowRr opPow_r(int b) const { return this; }
}
void Use32opPowRr()
{
    Op32opPowRr value;
    Op32opPowRr other;
    Op32opPowRr@ handle;
    Op32opPowRr result = 2 ** value;
}

class Other33
{
    int filler;
}
class Op33opAndRr
{
    int stored;
    Op33opAndRr opAnd_r(uint b) const { return this; }
}
void Use33opAndRr()
{
    Op33opAndRr value;
    Op33opAndRr other;
    Op33opAndRr@ handle;
    Op33opAndRr result = 1 & value;
}

class Other34
{
    int filler;
}
class Op34opOrRr
{
    int stored;
    Op34opOrRr opOr_r(uint b) const { return this; }
}
void Use34opOrRr()
{
    Op34opOrRr value;
    Op34opOrRr other;
    Op34opOrRr@ handle;
    Op34opOrRr result = 1 | value;
}

class Other35
{
    int filler;
}
class Op35opXorRr
{
    int stored;
    Op35opXorRr opXor_r(uint b) const { return this; }
}
void Use35opXorRr()
{
    Op35opXorRr value;
    Op35opXorRr other;
    Op35opXorRr@ handle;
    Op35opXorRr result = 1 ^ value;
}

class Other36
{
    int filler;
}
class Op36opShlRr
{
    int stored;
    Op36opShlRr opShl_r(int b) const { return this; }
}
void Use36opShlRr()
{
    Op36opShlRr value;
    Op36opShlRr other;
    Op36opShlRr@ handle;
    Op36opShlRr result = 1 << value;
}

class Other37
{
    int filler;
}
class Op37opShrRr
{
    int stored;
    Op37opShrRr opShr_r(int b) const { return this; }
}
void Use37opShrRr()
{
    Op37opShrRr value;
    Op37opShrRr other;
    Op37opShrRr@ handle;
    Op37opShrRr result = 1 >> value;
}

class Other38
{
    int filler;
}
class Op38opUShrRr
{
    int stored;
    Op38opUShrRr opUShr_r(uint b) const { return this; }
}
void Use38opUShrRr()
{
    Op38opUShrRr value;
    Op38opUShrRr other;
    Op38opUShrRr@ handle;
    Op38opUShrRr result = 1 >>> value;
}

class Other39
{
    int filler;
}
class Op39opNeg
{
    int stored;
    Op39opNeg opNeg() const { return this; }
}
void Use39opNeg()
{
    Op39opNeg value;
    Op39opNeg other;
    Op39opNeg@ handle;
    Op39opNeg result = -value;
}

class Other40
{
    int filler;
}
class Op40opCom
{
    int stored;
    Op40opCom opCom() const { return this; }
}
void Use40opCom()
{
    Op40opCom value;
    Op40opCom other;
    Op40opCom@ handle;
    Op40opCom result = ~value;
}

class Other41
{
    int filler;
}
class Op41opPreInc
{
    int stored;
    Op41opPreInc& opPreInc() { return this; }
}
void Use41opPreInc()
{
    Op41opPreInc value;
    Op41opPreInc other;
    Op41opPreInc@ handle;
    ++value;
}

class Other42
{
    int filler;
}
class Op42opPostInc
{
    int stored;
    Op42opPostInc opPostInc() { return this; }
}
void Use42opPostInc()
{
    Op42opPostInc value;
    Op42opPostInc other;
    Op42opPostInc@ handle;
    value++;
}

class Other43
{
    int filler;
}
class Op43opPreDec
{
    int stored;
    Op43opPreDec& opPreDec() { return this; }
}
void Use43opPreDec()
{
    Op43opPreDec value;
    Op43opPreDec other;
    Op43opPreDec@ handle;
    --value;
}

class Other44
{
    int filler;
}
class Op44opPostDec
{
    int stored;
    Op44opPostDec opPostDec() { return this; }
}
void Use44opPostDec()
{
    Op44opPostDec value;
    Op44opPostDec other;
    Op44opPostDec@ handle;
    value--;
}

class Other45
{
    int filler;
}
class Op45opEquals
{
    int stored;
    bool opEquals(const Op45opEquals &in o) const { return true; }
}
void Use45opEquals()
{
    Op45opEquals value;
    Op45opEquals other;
    Op45opEquals@ handle;
    bool result = (value == other);
}

class Other46
{
    int filler;
}
class Op46opCmp
{
    int stored;
    int opCmp(const Op46opCmp &in o) const { return 0; }
}
void Use46opCmp()
{
    Op46opCmp value;
    Op46opCmp other;
    Op46opCmp@ handle;
    bool result = (value < other);
}

class Other47
{
    int filler;
}
class Op47opIndex
{
    int stored;
    int& opIndex(int idx) { return stored; }
}
void Use47opIndex()
{
    Op47opIndex value;
    Op47opIndex other;
    Op47opIndex@ handle;
    int result = value[0];
}

class Other48
{
    int filler;
}
class Op48opCall
{
    int stored;
    void opCall(int x) {  }
}
void Use48opCall()
{
    Op48opCall value;
    Op48opCall other;
    Op48opCall@ handle;
    value(1);
}

class Other49
{
    int filler;
}
class Op49opConv
{
    int stored;
    int opConv() const { return 0; }
}
void Use49opConv()
{
    Op49opConv value;
    Op49opConv other;
    Op49opConv@ handle;
    int result = int(value);
}

class Other50
{
    int filler;
}
class Op50opImplConv
{
    int stored;
    int opImplConv() const { return 0; }
}
void Use50opImplConv()
{
    Op50opImplConv value;
    Op50opImplConv other;
    Op50opImplConv@ handle;
    int result = value;
}

class Other51
{
    int filler;
}
class Op51opCast
{
    int stored;
    Other51@ opCast() { return null; }
}
void Use51opCast()
{
    Op51opCast value;
    Op51opCast other;
    Op51opCast@ handle;
    Other51@ result = cast<Other51@>(value);
}

class Other52
{
    int filler;
}
class Op52opImplCast
{
    int stored;
    Other52@ opImplCast() { return null; }
}
void Use52opImplCast()
{
    Op52opImplCast value;
    Op52opImplCast other;
    Op52opImplCast@ handle;
    Other52@ result = value;
}

