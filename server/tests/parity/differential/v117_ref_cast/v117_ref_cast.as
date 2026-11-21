// @expect: accept
class CBaseEntity {}
class CBasePlayer : CBaseEntity {}
class EHandle {
  bool IsValid() const { return true; }
  CBaseEntity@ GetEntity() const { return null; }
}
namespace HZ::HUD::MONEY {
void Draw(CBasePlayer& pPlayer, int iValue) {}
}
void HZ_HUD_MONEY_DrawLate(const EHandle& in hPlayer, int iValue) {
  if (hPlayer.IsValid())
    HZ::HUD::MONEY::Draw(cast<CBasePlayer>(hPlayer.GetEntity()), iValue);
}
