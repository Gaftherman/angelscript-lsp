/** Gives a player armor when they get hurt. */
void OnPlayerHurt(CBasePlayer@ player)
{
    player.SetArmor(100);
}
