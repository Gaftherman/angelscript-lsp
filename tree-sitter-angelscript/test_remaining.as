// Test virtual_property AST structure
int prop
{
    get { return 0; }
    set { }
}

// Test funcdef with handle return (potential missing field)
funcdef int@ HandleCallback(int x);

// Test class template parameter capture
class Array<T> {}
