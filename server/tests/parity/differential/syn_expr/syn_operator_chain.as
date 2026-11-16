// ==== case: opchain_class_ok ====
// @expect: accept
class N {
    N opAdd(const N&in) const { return N(); }
    N opMul(const N&in) const { return N(); }
    N opMul_r(int) const { return N(); }
    N& opAddAssign(const N&in) { return this; }
    N& opPreInc() { return this; }
    N opPostInc() { return N(); }
}
void main() {
    N i;
    i += i + i * i * i;
    i++;
    ++i;
    i = 2 * i;
    i += (i * i) * i;
}


// ==== case: opchain_int_ok ====
// @expect: accept
void main() {
    int j = 1;
    j += j + j * j * j;
    j++;
    ++j;
}


// ==== case: opchain_missing_predec_bad ====
// @expect: reject
class N {
    N& opPreInc() { return this; }
}
void main() {
    N i;
    --i;
}


// ==== case: opchain_missing_subassign_bad ====
// @expect: reject
class N {
    N& opAddAssign(const N&in) { return this; }
}
void main() {
    N i;
    i -= i;
}
