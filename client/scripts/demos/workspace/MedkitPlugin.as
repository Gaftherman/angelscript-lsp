/**
 * @brief Medkit station utility.
 */
class MedkitStation
{
    private int m_charges;
    private float m_healPower;

    /**
     * @brief Medkit constructor.
     * @param initialCharges Uses count.
     * @param healPower Health restored.
     */
    MedkitStation(int initialCharges = 5, float healPower = 25.0f)
    {
        m_charges = initialCharges;
        m_healPower = healPower;
    }

    /**
     * @brief Dispenses medical aid.
     * @param player Target player entity.
     * @return True if healed.
     */
    bool Dispense(CBasePlayer@ player)
    {
        if (player is null || m_charges <= 0)
            return false;

        float currentHp = player.GetHealth();
        player.SetHealth(currentHp + m_healPower);
        m_charges--;

        g_EngineFuncs.ClientPrint(player, "Healed!\n");
        return true;
    }

    int GetRemainingCharges() const
    {
        return m_charges;
    }
}

// Global station instance and plugin entry points
MedkitStation g_MainStation(10, 30.0f);

void PluginInit()
{
    g_EngineFuncs.ServerPrint("Plugin loaded.\n");
}

void OnPlayerSpawn(CBasePlayer@ player)
{
    g_MainStation.Dispense(player);
}
