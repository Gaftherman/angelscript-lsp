/** Coins a player carries. */
class Wallet
{
    int coins = 0;

    void Add(int amount)
    {
        coins += amount;
    }

    void Spend(int amount)
    {
        coins -= amount;
        return true;
    }
}
