// @expect: reject
namespace VaultDungeon {
    class SecretChest {
        private int GoldCoins;
    }
    void stealTreasure() {
        SecretChest chest;
        int x = chest.GoldCoins;
    }
}
