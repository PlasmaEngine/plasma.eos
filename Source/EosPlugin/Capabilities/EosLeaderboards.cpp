#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>

// EOS leaderboards are views over a stat: the portal binds a leaderboard id to a stat name and an aggregation.
// UploadScore therefore ingests the bound stat and the server ranks it; KeepBest versus ForceUpdate is decided by
// the aggregation configured in the portal, not here, and the detail values have no EOS counterpart. Reading
// resolves the binding once through the definitions query and caches it.

namespace
{
  struct ScoreDescending
  {
    PL_ALWAYS_INLINE bool Less(const plPlatformLeaderboardEntry& a, const plPlatformLeaderboardEntry& b) const { return a.m_iScore > b.m_iScore; }
  };

  struct ScoreAscending
  {
    PL_ALWAYS_INLINE bool Less(const plPlatformLeaderboardEntry& a, const plPlatformLeaderboardEntry& b) const { return a.m_iScore < b.m_iScore; }
  };

  // Lives in plEosCall::m_pUser for the duration of a read.
  struct ReadContext
  {
    plPlatformLeaderboardQuery m_Query;
    plString m_sLeaderboardId;
  };
} // namespace

bool plEosPlatformBackend::CacheLeaderboardDefinition(plStringView sId)
{
  const plStringBuilder sIdZ(sId);

  EOS_Leaderboards_CopyLeaderboardDefinitionByLeaderboardIdOptions options = {};
  options.ApiVersion = EOS_LEADERBOARDS_COPYLEADERBOARDDEFINITIONBYLEADERBOARDID_API_LATEST;
  options.LeaderboardId = sIdZ.GetData();

  EOS_Leaderboards_Definition* pDefinition = nullptr;
  if (EOS_Leaderboards_CopyLeaderboardDefinitionByLeaderboardId(m_hLeaderboards, &options, &pDefinition) != EOS_EResult::EOS_Success || pDefinition == nullptr)
    return false;

  LeaderboardDef& def = m_LeaderboardDefs[plString(sId)];
  def.m_sStat = pDefinition->StatName;
  def.m_Aggregation = pDefinition->Aggregation;
  EOS_Leaderboards_Definition_Release(pDefinition);

  return true;
}

plPlatformOperationPtr plEosPlatformBackend::UploadScore(plStringView sBackendId, plInt64 iScore, plPlatformLeaderboardUpload method, plArrayPtr<const plInt32> details)
{
  PL_IGNORE_UNUSED(method);
  PL_IGNORE_UNUSED(details);

  if (plPlatformOperationPtr pGate = GateSignedIn("Leaderboards.Upload"))
    return pGate;

  if (sBackendId.IsEmpty() || m_hStats == nullptr)
    return plPlatformOperation::CreateFinished("Leaderboards.Upload", plPlatformError::InvalidArgument);

  plEosCall* pCall = plEosNewCall(this, "Leaderboards.Upload");
  pCall->m_sArg = sBackendId;
  pCall->m_iNumber = iScore;

  if (const LeaderboardDef* pDef = m_LeaderboardDefs.GetValue(pCall->m_sArg))
  {
    IngestLeaderboardScore(pCall, *pDef);
    return pCall->m_pOp;
  }

  // The stat binding is not known yet; the upload continues once the definitions arrived.
  EOS_Leaderboards_QueryLeaderboardDefinitionsOptions options = {};
  options.ApiVersion = EOS_LEADERBOARDS_QUERYLEADERBOARDDEFINITIONS_API_LATEST;
  options.StartTime = EOS_LEADERBOARDS_TIME_UNDEFINED;
  options.EndTime = EOS_LEADERBOARDS_TIME_UNDEFINED;
  options.LocalUserId = m_LocalProductUserId;

  EOS_Leaderboards_QueryLeaderboardDefinitions(m_hLeaderboards, &options, pCall, &OnLeaderboardDefinitionsForUpload);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnLeaderboardDefinitionsForUpload(const EOS_Leaderboards_OnQueryLeaderboardDefinitionsCompleteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode != EOS_EResult::EOS_Success || !pThis->CacheLeaderboardDefinition(pCall->m_sArg))
  {
    plLog::Warning("EOS: leaderboard '{}' has no definition ({}).", pCall->m_sArg, plEosResultName(pData->ResultCode));
    pCall->m_pOp->Complete(pData->ResultCode == EOS_EResult::EOS_Success ? plPlatformError::NotFound : plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  pThis->IngestLeaderboardScore(pCall, *pThis->m_LeaderboardDefs.GetValue(pCall->m_sArg));
}

void plEosPlatformBackend::IngestLeaderboardScore(plEosCall* pCall, const LeaderboardDef& def)
{
  const plInt32 iScore = static_cast<plInt32>(plMath::Clamp<plInt64>(pCall->m_iNumber, plMath::MinValue<plInt32>(), plMath::MaxValue<plInt32>()));

  // A Sum-aggregated stat accumulates: keep the server cache consistent with what Store() does.
  plInt32 iAmount = iScore;
  if (def.m_Aggregation == EOS_ELeaderboardAggregation::EOS_LA_Sum)
  {
    const plInt32* pKnown = m_ServerStats.GetValue(def.m_sStat);
    iAmount = iScore - (pKnown ? *pKnown : 0);
  }

  m_ServerStats[def.m_sStat] = iScore;

  EOS_Stats_IngestData data = {};
  data.ApiVersion = EOS_STATS_INGESTDATA_API_LATEST;
  data.StatName = def.m_sStat.GetData();
  data.IngestAmount = iAmount;

  EOS_Stats_IngestStatOptions options = {};
  options.ApiVersion = EOS_STATS_INGESTSTAT_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.TargetUserId = m_LocalProductUserId;
  options.Stats = &data;
  options.StatsCount = 1;

  EOS_Stats_IngestStat(m_hStats, &options, pCall, &OnIngestStat);
}

plPlatformOperationPtr plEosPlatformBackend::Read(plStringView sBackendId, const plPlatformLeaderboardQuery& query)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Leaderboards.Read"))
    return pGate;

  if (sBackendId.IsEmpty())
    return plPlatformOperation::CreateFinished("Leaderboards.Read", plPlatformError::InvalidArgument);

  plEosCall* pCall = plEosNewCall(this, "Leaderboards.Read");
  pCall->m_sArg = sBackendId;

  ReadContext* pContext = PL_DEFAULT_NEW(ReadContext);
  pContext->m_Query = query;
  pContext->m_sLeaderboardId = sBackendId;
  pCall->m_pUser = pContext;

  // The friends window queries user scores by stat name, which needs the definition first.
  if (query.m_Window == plPlatformLeaderboardWindow::Friends && !m_LeaderboardDefs.Contains(pCall->m_sArg))
  {
    EOS_Leaderboards_QueryLeaderboardDefinitionsOptions options = {};
    options.ApiVersion = EOS_LEADERBOARDS_QUERYLEADERBOARDDEFINITIONS_API_LATEST;
    options.StartTime = EOS_LEADERBOARDS_TIME_UNDEFINED;
    options.EndTime = EOS_LEADERBOARDS_TIME_UNDEFINED;
    options.LocalUserId = m_LocalProductUserId;

    EOS_Leaderboards_QueryLeaderboardDefinitions(m_hLeaderboards, &options, pCall, &OnLeaderboardDefinitionsForRead);

    return pCall->m_pOp;
  }

  BeginLeaderboardRead(pCall, query);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnLeaderboardDefinitionsForRead(const EOS_Leaderboards_OnQueryLeaderboardDefinitionsCompleteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;
  ReadContext* pContext = static_cast<ReadContext*>(pCall->m_pUser);

  if (pData->ResultCode != EOS_EResult::EOS_Success || !pThis->CacheLeaderboardDefinition(pCall->m_sArg))
  {
    pCall->m_pOp->Complete(pData->ResultCode == EOS_EResult::EOS_Success ? plPlatformError::NotFound : plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pContext);
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  pThis->BeginLeaderboardRead(pCall, pContext->m_Query);
}

void plEosPlatformBackend::BeginLeaderboardRead(plEosCall* pCall, const plPlatformLeaderboardQuery& query)
{
  if (query.m_Window == plPlatformLeaderboardWindow::Friends)
  {
    const LeaderboardDef& def = *m_LeaderboardDefs.GetValue(pCall->m_sArg);

    plHybridArray<EOS_ProductUserId, 32> users;
    users.PushBack(m_LocalProductUserId);

    for (const Friend& f : m_Friends)
    {
      if (EOS_ProductUserId_IsValid(f.m_ProductId))
        users.PushBack(f.m_ProductId);
    }

    EOS_Leaderboards_UserScoresQueryStatInfo statInfo = {};
    statInfo.ApiVersion = EOS_LEADERBOARDS_USERSCORESQUERYSTATINFO_API_LATEST;
    statInfo.StatName = def.m_sStat.GetData();
    statInfo.Aggregation = def.m_Aggregation;

    EOS_Leaderboards_QueryLeaderboardUserScoresOptions options = {};
    options.ApiVersion = EOS_LEADERBOARDS_QUERYLEADERBOARDUSERSCORES_API_LATEST;
    options.UserIds = users.GetData();
    options.UserIdsCount = users.GetCount();
    options.StatInfo = &statInfo;
    options.StatInfoCount = 1;
    options.StartTime = EOS_LEADERBOARDS_TIME_UNDEFINED;
    options.EndTime = EOS_LEADERBOARDS_TIME_UNDEFINED;
    options.LocalUserId = m_LocalProductUserId;

    EOS_Leaderboards_QueryLeaderboardUserScores(m_hLeaderboards, &options, pCall, &OnLeaderboardUserScores);

    return;
  }

  const ReadContext* pContext = static_cast<ReadContext*>(pCall->m_pUser);

  EOS_Leaderboards_QueryLeaderboardRanksOptions options = {};
  options.ApiVersion = EOS_LEADERBOARDS_QUERYLEADERBOARDRANKS_API_LATEST;
  options.LeaderboardId = pContext->m_sLeaderboardId.GetData();
  options.LocalUserId = m_LocalProductUserId;

  EOS_Leaderboards_QueryLeaderboardRanks(m_hLeaderboards, &options, pCall, &OnLeaderboardRanks);
}

void EOS_CALL plEosPlatformBackend::OnLeaderboardRanks(const EOS_Leaderboards_OnQueryLeaderboardRanksCompleteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;
  ReadContext* pContext = static_cast<ReadContext*>(pCall->m_pUser);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pContext);
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  EOS_Leaderboards_GetLeaderboardRecordCountOptions countOptions = {};
  countOptions.ApiVersion = EOS_LEADERBOARDS_GETLEADERBOARDRECORDCOUNT_API_LATEST;

  const uint32_t uiCount = EOS_Leaderboards_GetLeaderboardRecordCount(pThis->m_hLeaderboards, &countOptions);

  plDynamicArray<plPlatformLeaderboardEntry> allEntries;
  allEntries.Reserve(uiCount);
  plInt32 iLocalIndex = -1;

  for (uint32_t i = 0; i < uiCount; ++i)
  {
    EOS_Leaderboards_CopyLeaderboardRecordByIndexOptions copyOptions = {};
    copyOptions.ApiVersion = EOS_LEADERBOARDS_COPYLEADERBOARDRECORDBYINDEX_API_LATEST;
    copyOptions.LeaderboardRecordIndex = i;

    EOS_Leaderboards_LeaderboardRecord* pRecord = nullptr;
    if (EOS_Leaderboards_CopyLeaderboardRecordByIndex(pThis->m_hLeaderboards, &copyOptions, &pRecord) != EOS_EResult::EOS_Success || pRecord == nullptr)
      continue;

    plPlatformLeaderboardEntry& entry = allEntries.ExpandAndGetRef();
    entry.m_User = plEosMakeUserId(pRecord->UserId);
    entry.m_sDisplayName = pRecord->UserDisplayName != nullptr ? pRecord->UserDisplayName : "";
    entry.m_iRank = static_cast<plInt32>(pRecord->Rank);
    entry.m_iScore = pRecord->Score;

    if (pRecord->UserId == pThis->m_LocalProductUserId)
      iLocalIndex = static_cast<plInt32>(allEntries.GetCount() - 1);

    EOS_Leaderboards_LeaderboardRecord_Release(pRecord);
  }

  const plPlatformLeaderboardQuery& query = pContext->m_Query;
  plDynamicArray<plPlatformLeaderboardEntry>& entries = pCall->m_pOp->MutableResult().m_LeaderboardEntries;

  if (query.m_Window == plPlatformLeaderboardWindow::AroundUser)
  {
    if (iLocalIndex >= 0)
    {
      const plInt32 iFirst = plMath::Max(0, iLocalIndex - query.m_iRangeStart);
      const plInt32 iLast = plMath::Min<plInt32>(allEntries.GetCount() - 1, iLocalIndex + query.m_iRangeEnd);

      for (plInt32 i = iFirst; i <= iLast; ++i)
        entries.PushBack(allEntries[i]);
    }
  }
  else
  {
    for (const plPlatformLeaderboardEntry& entry : allEntries)
    {
      if (entry.m_iRank >= query.m_iRangeStart && entry.m_iRank <= query.m_iRangeEnd)
        entries.PushBack(entry);
    }
  }

  pCall->m_pOp->Succeed();
  PL_DEFAULT_DELETE(pContext);
  PL_DEFAULT_DELETE(pCall);
}

void EOS_CALL plEosPlatformBackend::OnLeaderboardUserScores(const EOS_Leaderboards_OnQueryLeaderboardUserScoresCompleteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;
  ReadContext* pContext = static_cast<ReadContext*>(pCall->m_pUser);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pContext);
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  const LeaderboardDef& def = *pThis->m_LeaderboardDefs.GetValue(pCall->m_sArg);

  EOS_Leaderboards_GetLeaderboardUserScoreCountOptions countOptions = {};
  countOptions.ApiVersion = EOS_LEADERBOARDS_GETLEADERBOARDUSERSCORECOUNT_API_LATEST;
  countOptions.StatName = def.m_sStat.GetData();

  const uint32_t uiCount = EOS_Leaderboards_GetLeaderboardUserScoreCount(pThis->m_hLeaderboards, &countOptions);

  plDynamicArray<plPlatformLeaderboardEntry>& entries = pCall->m_pOp->MutableResult().m_LeaderboardEntries;
  entries.Reserve(uiCount);

  for (uint32_t i = 0; i < uiCount; ++i)
  {
    EOS_Leaderboards_CopyLeaderboardUserScoreByIndexOptions copyOptions = {};
    copyOptions.ApiVersion = EOS_LEADERBOARDS_COPYLEADERBOARDUSERSCOREBYINDEX_API_LATEST;
    copyOptions.LeaderboardUserScoreIndex = i;
    copyOptions.StatName = def.m_sStat.GetData();

    EOS_Leaderboards_LeaderboardUserScore* pScore = nullptr;
    if (EOS_Leaderboards_CopyLeaderboardUserScoreByIndex(pThis->m_hLeaderboards, &copyOptions, &pScore) != EOS_EResult::EOS_Success || pScore == nullptr)
      continue;

    plPlatformLeaderboardEntry& entry = entries.ExpandAndGetRef();
    entry.m_User = plEosMakeUserId(pScore->UserId);
    entry.m_iScore = pScore->Score;

    if (pScore->UserId == pThis->m_LocalProductUserId)
      entry.m_sDisplayName = pThis->m_User.m_sDisplayName;
    else
      entry.m_sDisplayName = pThis->GetCachedDisplayName(pScore->UserId);

    EOS_Leaderboards_LeaderboardUserScore_Release(pScore);
  }

  // User score queries carry no rank; the friends window is ranked locally by the leaderboard's aggregation.
  if (def.m_Aggregation == EOS_ELeaderboardAggregation::EOS_LA_Min)
    entries.Sort(ScoreAscending());
  else
    entries.Sort(ScoreDescending());

  for (plUInt32 i = 0; i < entries.GetCount(); ++i)
    entries[i].m_iRank = static_cast<plInt32>(i + 1);

  pCall->m_pOp->Succeed();
  PL_DEFAULT_DELETE(pContext);
  PL_DEFAULT_DELETE(pCall);
}
