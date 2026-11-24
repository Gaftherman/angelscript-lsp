class HealthBeacon
{
    // Violation of engine rules: raw entity handle in member
    private CBasePlayer@ m_targetPlayer;

    void Activate(CBasePlayer@ target)
    {
        // Type mismatch diagnostic: string assigned from float
        string healthName = target.GetHealth();
    }
}
