int main()
{
    int total = 3 * 4 + 2;
    println("total: " + total);

    array<string>@ args = getArgs();
    println("argument count: " + args.length());
    for (uint i = 0; i < args.length(); ++i)
    {
        println("argument " + i + ": " + args[i]);
    }
    return 0;
}
