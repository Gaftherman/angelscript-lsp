enum MyEnum { MY_TEST_ID_1, MY_TEST_ID_2 };
namespace TEST {
    void main_func() { int id = 1; my_test_func(id); }
    void my_test_func(MyEnum test_id) { if (test_id == MyEnum::MY_TEST_ID_1) {} }
}
