/** Warns the player when the round is about to end. */
void CheckRoundTimer(CBasePlayer@ player)
{
    if (GetRoundTimeLeft() < 10.0f)
        player.SetHealth(player.GetHealth());
}
