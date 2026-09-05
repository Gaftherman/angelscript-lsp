// Probes an else block placed directly inside a loop body without an if statement.
// An else block cannot stand alone inside a block of statements.
// EXPECT: reject
void test() {
    for (int i = 0; i < 5; i++) {
        else {
            break;
        }
    }
}
