int doubleValue(int value)
{
    int total = value * 2;
    println("debug total: " + total); // BP
    return total;
}

int main()
{
    int input = 6;
    println("debug input: " + input);
    int total = doubleValue(input);
    return total == 12 ? 0 : 1;
}
