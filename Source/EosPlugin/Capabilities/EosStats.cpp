#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>
#include <GameEngine/PlatformServices/PlatformDefinitions.h>

PL_DEFINE_AS_POD_TYPE(EOS_Stats_IngestData);

// EOS stats are ingest-only integers: the server applies the amount with the aggregation configured in the
// portal. The hub hands over absolute values, so a Sum stat is sent as the difference to the last value the
// server reported; Min, Max and Latest stats take the value itself.

plPlatformOperationPtr plEosPlatformBackend::Store(plArrayPtr<const plPlatformStatValue> values)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Stats.Store"))
    return pGate;

  if (values.IsEmpty())
    return plPlatformOperation::CreateFinished("Stats.Store", plPlatformError::Success);

  const plPlatformDefinitions& definitions = m_pHost->GetDefinitions();

  // Names are reserved up front, so the StatName pointers stored in ingest are never invalidated by a reallocation.
  plHybridArray<plString, 16> names;
  plHybridArray<EOS_Stats_IngestData, 16> ingest;
  names.Reserve(values.GetCount());

  for (const plPlatformStatValue& value : values)
  {
    if (value.m_sId.IsEmpty())
      continue;

    const plInt32 iValue = static_cast<plInt32>(plMath::Round(value.m_Value.ConvertTo<double>()));

    plPlatformStatAggregate aggregate = plPlatformStatAggregate::Sum;
    if (const plPlatformStatDef* pDef = definitions.FindStat(definitions.StatIdFromBackend("eos", value.m_sId)))
      aggregate = pDef->m_Aggregate;

    plInt32 iAmount = iValue;
    if (aggregate == plPlatformStatAggregate::Sum)
    {
      const plInt32* pKnown = m_ServerStats.GetValue(value.m_sId);
      iAmount = iValue - (pKnown ? *pKnown : 0);

      if (iAmount == 0)
        continue;

      m_ServerStats[value.m_sId] = iValue;
    }
    else
    {
      m_ServerStats[value.m_sId] = iValue;
    }

    names.PushBack(value.m_sId);

    EOS_Stats_IngestData& data = ingest.ExpandAndGetRef();
    data.ApiVersion = EOS_STATS_INGESTDATA_API_LATEST;
    data.StatName = names.PeekBack().GetData();
    data.IngestAmount = iAmount;
  }

  if (ingest.IsEmpty())
    return plPlatformOperation::CreateFinished("Stats.Store", plPlatformError::Success);

  plEosCall* pCall = plEosNewCall(this, "Stats.Store");

  EOS_Stats_IngestStatOptions options = {};
  options.ApiVersion = EOS_STATS_INGESTSTAT_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.TargetUserId = m_LocalProductUserId;
  options.Stats = ingest.GetData();
  options.StatsCount = ingest.GetCount();

  EOS_Stats_IngestStat(m_hStats, &options, pCall, &OnIngestStat);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnIngestStat(const EOS_Stats_IngestStatCompleteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
    plLog::Warning("EOS: ingesting stats failed: {}", plEosResultName(pData->ResultCode));

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

plPlatformOperationPtr plEosPlatformBackend::QueryStats()
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Stats.Query"))
    return pGate;

  plEosCall* pCall = plEosNewCall(this, "Stats.Query");

  EOS_Stats_QueryStatsOptions options = {};
  options.ApiVersion = EOS_STATS_QUERYSTATS_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.TargetUserId = m_LocalProductUserId;
  options.StartTime = EOS_STATS_TIME_UNDEFINED;
  options.EndTime = EOS_STATS_TIME_UNDEFINED;
  options.StatNames = nullptr;
  options.StatNamesCount = 0;

  EOS_Stats_QueryStats(m_hStats, &options, pCall, &OnQueryStats);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnQueryStats(const EOS_Stats_OnQueryStatsCompleteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  pThis->ReadStatsIntoCache();

  plDynamicArray<plPlatformStatValue>& stats = pCall->m_pOp->MutableResult().m_Stats;

  for (auto it = pThis->m_ServerStats.GetIterator(); it.IsValid(); ++it)
  {
    plPlatformStatValue& value = stats.ExpandAndGetRef();
    value.m_sId = it.Key();
    value.m_Value = it.Value();
  }

  pCall->m_pOp->Succeed();
  PL_DEFAULT_DELETE(pCall);
}

void plEosPlatformBackend::ReadStatsIntoCache()
{
  EOS_Stats_GetStatCountOptions countOptions = {};
  countOptions.ApiVersion = EOS_STATS_GETSTATCOUNT_API_LATEST;
  countOptions.TargetUserId = m_LocalProductUserId;

  const uint32_t uiCount = EOS_Stats_GetStatsCount(m_hStats, &countOptions);

  for (uint32_t i = 0; i < uiCount; ++i)
  {
    EOS_Stats_CopyStatByIndexOptions copyOptions = {};
    copyOptions.ApiVersion = EOS_STATS_COPYSTATBYINDEX_API_LATEST;
    copyOptions.TargetUserId = m_LocalProductUserId;
    copyOptions.StatIndex = i;

    EOS_Stats_Stat* pStat = nullptr;
    if (EOS_Stats_CopyStatByIndex(m_hStats, &copyOptions, &pStat) != EOS_EResult::EOS_Success || pStat == nullptr)
      continue;

    m_ServerStats[plString(pStat->Name)] = pStat->Value;
    EOS_Stats_Stat_Release(pStat);
  }
}
