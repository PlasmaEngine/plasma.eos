#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>

plPlatformOperationPtr plEosPlatformBackend::Unlock(plStringView sBackendId)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Achievements.Unlock"))
    return pGate;

  if (sBackendId.IsEmpty())
    return plPlatformOperation::CreateFinished("Achievements.Unlock", plPlatformError::InvalidArgument);

  plEosCall* pCall = plEosNewCall(this, "Achievements.Unlock");
  pCall->m_sArg = sBackendId;

  const char* szId = pCall->m_sArg.GetData();

  EOS_Achievements_UnlockAchievementsOptions options = {};
  options.ApiVersion = EOS_ACHIEVEMENTS_UNLOCKACHIEVEMENTS_API_LATEST;
  options.UserId = m_LocalProductUserId;
  options.AchievementIds = &szId;
  options.AchievementsCount = 1;

  EOS_Achievements_UnlockAchievements(m_hAchievements, &options, pCall, &OnUnlockAchievements);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnUnlockAchievements(const EOS_Achievements_OnUnlockAchievementsCompleteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
    plLog::Warning("EOS: unlocking achievement '{}' failed: {}", pCall->m_sArg, plEosResultName(pData->ResultCode));

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

plPlatformOperationPtr plEosPlatformBackend::QueryAchievements()
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Achievements.Query"))
    return pGate;

  plEosCall* pCall = plEosNewCall(this, "Achievements.Query");

  EOS_Achievements_QueryPlayerAchievementsOptions options = {};
  options.ApiVersion = EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST;
  options.TargetUserId = m_LocalProductUserId;
  options.LocalUserId = m_LocalProductUserId;

  EOS_Achievements_QueryPlayerAchievements(m_hAchievements, &options, pCall, &OnQueryPlayerAchievements);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnQueryPlayerAchievements(const EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  EOS_Achievements_GetPlayerAchievementCountOptions countOptions = {};
  countOptions.ApiVersion = EOS_ACHIEVEMENTS_GETPLAYERACHIEVEMENTCOUNT_API_LATEST;
  countOptions.UserId = pThis->m_LocalProductUserId;

  const uint32_t uiCount = EOS_Achievements_GetPlayerAchievementCount(pThis->m_hAchievements, &countOptions);

  plDynamicArray<plPlatformAchievementState>& states = pCall->m_pOp->MutableResult().m_Achievements;
  states.Reserve(uiCount);

  for (uint32_t i = 0; i < uiCount; ++i)
  {
    EOS_Achievements_CopyPlayerAchievementByIndexOptions copyOptions = {};
    copyOptions.ApiVersion = EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYINDEX_API_LATEST;
    copyOptions.TargetUserId = pThis->m_LocalProductUserId;
    copyOptions.LocalUserId = pThis->m_LocalProductUserId;
    copyOptions.AchievementIndex = i;

    EOS_Achievements_PlayerAchievement* pAchievement = nullptr;
    if (EOS_Achievements_CopyPlayerAchievementByIndex(pThis->m_hAchievements, &copyOptions, &pAchievement) != EOS_EResult::EOS_Success || pAchievement == nullptr)
      continue;

    plPlatformAchievementState& state = states.ExpandAndGetRef();
    state.m_sId = pAchievement->AchievementId;
    state.m_bUnlocked = pAchievement->UnlockTime != EOS_ACHIEVEMENTS_ACHIEVEMENT_UNLOCKTIME_UNDEFINED;
    if (state.m_bUnlocked)
      state.m_iUnlockTimeUtcMicroseconds = static_cast<plInt64>(pAchievement->UnlockTime) * 1000000;

    // Progress is the first stat bound to the achievement in the developer portal.
    if (pAchievement->StatInfoCount > 0 && pAchievement->StatInfo != nullptr)
    {
      const EOS_Achievements_PlayerStatInfo& stat = pAchievement->StatInfo[0];
      state.m_uiProgress = static_cast<plUInt32>(plMath::Max<int32_t>(0, stat.CurrentValue));
      state.m_uiProgressTarget = static_cast<plUInt32>(plMath::Max<int32_t>(0, stat.ThresholdValue));
    }

    EOS_Achievements_PlayerAchievement_Release(pAchievement);
  }

  pCall->m_pOp->Succeed();
  PL_DEFAULT_DELETE(pCall);
}
