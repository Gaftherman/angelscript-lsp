// Every position the grammar lets an initializer list appear in, all of them accepted by the
// compiler. The regression guard for doc_r18 through doc_r22: the checks that reach an argument, an
// assignment and a return must not report the same code written correctly.
//
// A `repeat` is satisfied by no elements at all, which is why the empty list compiles.
array<int> Make()
{
    return {1, 2};
}

void Take(array<int> values)
{
}

void main()
{
    array<int> a = {1, 2};
    a = {3, 4};
    Take({5, 6});
    Take(array<int> = {7, 8});

    array<int> empty = {};
    array<array<int>> cells = {{1, 2}, {3, 4}};
    dictionary d = {{'k', 1}, {'j', 2}};
}
