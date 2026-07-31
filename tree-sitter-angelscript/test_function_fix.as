funcdef void function();

void test_function(function@ f) {
    f();
}

function@ myFunc = function() {};
