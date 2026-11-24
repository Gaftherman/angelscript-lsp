class HealthMonitor
{
    private float m_threshold = 25.0f;

    void Sample(CBasePlayer@ target) const
    {
        float multiplier = 0.5f;
        float healthReading = target.GetHealth() * multiplier;
    }
}
