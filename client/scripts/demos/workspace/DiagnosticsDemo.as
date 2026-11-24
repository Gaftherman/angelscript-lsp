class HealthBeacon
{
    // Violation of engine rules: raw entity handle in member
    private CBasePlayer@ m_targetPlayer;

    void Activate(CBasePlayer@ target)
    {
        // Unused local variable diagnostic
        string healthName = target.GetHealth();

    }
}
