class HealthMonitor
{
    private float m_threshold = 25.0f;

    void Sample(CBasePlayer@ target)
    {
        float healthReading = target.GetHealth() * 0.5f;
    }
}
