// ==== case: break_in_while ====
// @expect: accept
int f() {
    int i = 0;
    while (true) {
        if (i == 5) break;
        i++;
    }
    return i;
}

// ==== case: break_outside_loop ====
// @expect: reject
void f() {
    break;
}

// ==== case: catch_with_param ====
// @expect: reject
void f() {
    try {
        int x = 1;
    } catch (int e) {
        int y = e;
    }
}

// ==== case: continue_in_for ====
// @expect: accept
int f() {
    int s = 0;
    for (int i = 0; i < 5; i++) {
        if (i == 2) continue;
        s += i;
    }
    return s;
}

// ==== case: continue_in_switch ====
// @expect: reject
void f(int x) {
    switch (x) {
        case 1:
            continue;
        default:
            break;
    }
}

// ==== case: continue_outside_loop ====
// @expect: reject
void f() {
    continue;
}

// ==== case: do_while_basic ====
// @expect: accept
int f(int n) {
    int i = 0;
    do {
        i++;
    } while (i < n);
    return i;
}

// ==== case: do_while_missing_semicolon ====
// @expect: reject
// @range: loose
void f(int n) {
    int i = 0;
    do {
        i++;
    } while (i < n)
}

// ==== case: empty_statement_semicolon ====
// @expect: reject
void f(bool b) {
    ;
    if (b) ;
}

// ==== case: for_basic ====
// @expect: accept
int f(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += i;
    }
    return s;
}

// ==== case: for_empty_clauses ====
// @expect: accept
int f() {
    int i = 0;
    for (;;) {
        if (i >= 3) break;
        i++;
    }
    return i;
}

// ==== case: for_init_scope_after_loop ====
// @expect: reject
int f() {
    for (int i = 0; i < 3; i++) {
    }
    return i;
}

// ==== case: for_missing_semicolon ====
// @expect: reject
void f() {
    for (int i = 0; i < 5) {
    }
}

// ==== case: for_multi_init_step ====
// @expect: accept
int f() {
    int sum = 0;
    for (int i = 0, j = 10; i < j; i++, j--) {
        sum += i + j;
    }
    return sum;
}

// ==== case: for_non_bool_cond ====
// @expect: reject
void f() {
    for (int i = 0; i; i++) {
    }
}

// ==== case: foreach_array_basic ====
// @expect: accept
int f(array<int>@ arr) {
    int s = 0;
    foreach (int v : arr) {
        s += v;
    }
    return s;
}

// ==== case: foreach_missing_in ====
// @expect: reject
void f(array<int>@ arr) {
    foreach (int v) {
    }
}

// ==== case: if_assign_in_cond ====
// @expect: reject
void f(int x) {
    if (x = 1) {
        return;
    }
}

// ==== case: if_dangling_else ====
// @expect: accept
int f(bool a, bool b) {
    int r = 0;
    if (a)
        if (b)
            r = 1;
        else
            r = 2;
    return r;
}

// ==== case: if_else_basic ====
// @expect: accept
int f(int x) {
    if (x > 0) {
        return 1;
    } else {
        return -1;
    }
}

// ==== case: if_else_if_chain ====
// @expect: accept
int f(int x) {
    if (x < 0) {
        return -1;
    } else if (x == 0) {
        return 0;
    } else {
        return 1;
    }
}

// ==== case: if_missing_parens ====
// @expect: reject
void f(bool b) {
    if b {
        return;
    }
}

// ==== case: if_non_bool_cond_int ====
// @expect: reject
void f(int x) {
    if (x) {
        x = 0;
    }
}

// ==== case: nested_scope_block ====
// @expect: accept
int f() {
    {
        int x = 1;
    }
    {
        int x = 2;
        return x;
    }
}

// ==== case: nested_scope_redeclare_same ====
// @expect: reject
void f() {
    {
        int x = 1;
        int x = 2;
    }
}

// ==== case: nested_scope_shadow_inner ====
// @expect: accept
int f() {
    int x = 1;
    {
        int x = 2;
        if (x > 1) return x;
    }
    return x;
}

// ==== case: nested_scope_use_after_block ====
// @expect: reject
int f() {
    {
        int x = 1;
    }
    return x;
}

// ==== case: return_all_paths_if_else ====
// @expect: accept
int f(bool a, bool b) {
    if (a) {
        if (b) return 1;
        else return 2;
    } else {
        return 3;
    }
}

// ==== case: return_all_paths_switch ====
// @expect: accept
int f(int x) {
    switch (x) {
        case 1: return 10;
        case 2: return 20;
        default: return 30;
    }
}

// ==== case: return_in_infinite_loop ====
// @expect: reject
int f() {
    while (true) {
        return 1;
    }
}

// ==== case: return_missing_if_no_else ====
// @expect: reject
int f(bool a) {
    if (a) {
        return 1;
    }
}

// ==== case: return_missing_switch_no_default ====
// @expect: reject
int f(int x) {
    switch (x) {
        case 1: return 10;
        case 2: return 20;
    }
}

// ==== case: return_nonvoid_empty ====
// @expect: reject
int f() {
    return;
}

// ==== case: return_unreachable_after_return ====
// @expect: accept
int f() {
    return 1;
    int x = 2;
    return x;
}

// ==== case: return_void_with_value ====
// @expect: reject
void f() {
    return 1;
}

// ==== case: switch_duplicate_case ====
// @expect: reject
int f(int x) {
    switch (x) {
        case 1: return 10;
        case 1: return 20;
        default: return 0;
    }
}

// ==== case: switch_enum ====
// @expect: accept
enum Color { Red, Green, Blue }
int f(Color c) {
    switch (c) {
        case Red: return 1;
        case Green: return 2;
        default: return 3;
    }
}

// ==== case: switch_fallthrough_cases ====
// @expect: accept
int f(int x) {
    switch (x) {
        case 1:
        case 2:
            return 12;
        default:
            return 0;
    }
}

// ==== case: switch_int_basic ====
// @expect: accept
int f(int x) {
    int r = 0;
    switch (x) {
        case 1:
            r = 10;
            break;
        case 2:
            r = 20;
            break;
        default:
            r = -1;
            break;
    }
    return r;
}

// ==== case: try_catch_basic ====
// @expect: accept
int f() {
    int x = 0;
    try {
        x = 1;
    } catch {
        x = -1;
    }
    return x;
}

// ==== case: while_basic ====
// @expect: accept
int f(int n) {
    int i = 0;
    while (i < n) {
        i++;
    }
    return i;
}

// ==== case: while_empty_cond ====
// @expect: reject
void f() {
    while () {
        break;
    }
}

// ==== case: while_non_bool_cond ====
// @expect: reject
void f(int x) {
    while (x) {
        x--;
    }
}

// ==== case: switch_default_only ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(3, 5, 'Empty switch statement')]
// @expect: reject
int f(int x) {
    switch (x) {
        default:
            return 42;
    }
}

// ==== case: switch_float_cond ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(3, 13, 'Switch expressions must be integral numbers')]
// @expect: reject
int f(float x) {
    switch (x) {
        case 1: return 1;
        default: return 0;
    }
}

// ==== case: switch_non_const_case ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 14, 'Case expressions must be literal constants')]
// @expect: reject
int f(int x, int y) {
    switch (x) {
        case y: return 1;
        default: return 0;
    }
}

// ==== case: do_while_non_bool_cond ====
// @expect: reject
void f(int x) {
    do {
        x--;
    } while (x);
}

// ==== case: try_without_catch ====
// @expect: reject
void f() {
    try {
        int x = 1;
    }
}
