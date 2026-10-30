#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>

// EOS has no server-side matchmaker, so a ticket is a loop over the lobby service: search for a public lobby in
// the bucket tagged "plasma_mm" that satisfies the filters and has a free slot, join the first one, otherwise
// create one with the requested attributes and wait for others to arrive. The ticket is Found once the lobby holds
// the requested number of players. At the timeout a lobby with at least two players still counts as Found; an
// empty one is left and the ticket fails.

static constexpr const char* s_szMatchmakingAttribute = "plasma_mm";
static constexpr const char* s_szBucketDefault = "plasma";
static const plTime s_SearchInterval = plTime::MakeFromSeconds(5.0);

plPlatformOperationPtr plEosPlatformBackend::Start(const plPlatformMatchmakingRequest& request)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Matchmaking.Start"))
    return pGate;

  if (m_pTicketOp != nullptr && !m_pTicketOp->IsFinished())
    return plPlatformOperation::CreateFinished("Matchmaking.Start", plPlatformError::Busy);

  if (m_bInLobby)
    return plPlatformOperation::CreateFinished("Matchmaking.Start", plPlatformError::AlreadyInLobby);

  ++m_uiTicketCounter;

  plStringBuilder sTicket;
  sTicket.SetFormat("eos-mm-{}", m_uiTicketCounter);

  m_Ticket = plPlatformMatchmakingTicket();
  m_Ticket.m_sId = sTicket;
  m_TicketRequest = request;
  m_TicketRequest.m_uiPlayersWanted = plMath::Max<plUInt32>(2, request.m_uiPlayersWanted);
  m_TicketStart = plTime::Now();
  m_TicketNextSearch = m_TicketStart;
  m_bTicketBusy = false;
  m_pTicketOp = plPlatformOperation::Create("Matchmaking.Start");

  SetMatchmakingState(plPlatformMatchmakingState::Queued);

  return m_pTicketOp;
}

plPlatformOperationPtr plEosPlatformBackend::Cancel(plStringView sTicket)
{
  if (m_pTicketOp == nullptr || m_pTicketOp->IsFinished() || (!sTicket.IsEmpty() && m_Ticket.m_sId != sTicket))
    return plPlatformOperation::CreateFinished("Matchmaking.Cancel", plPlatformError::NotFound);

  if (m_bInLobby && m_bMatchmakingLobby)
  {
    plEosCall* pCall = plEosNewCall(this, "Matchmaking.Leave");
    pCall->m_sArg = m_CurrentLobby.m_Id.m_sId;

    EOS_Lobby_LeaveLobbyOptions options = {};
    options.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
    options.LocalUserId = m_LocalProductUserId;
    options.LobbyId = pCall->m_sArg.GetData();

    EOS_Lobby_LeaveLobby(m_hLobby, &options, pCall, &OnMatchmakingLeave);
  }

  SetMatchmakingState(plPlatformMatchmakingState::Cancelled);

  return plPlatformOperation::CreateFinished("Matchmaking.Cancel", plPlatformError::Success);
}

void plEosPlatformBackend::SetMatchmakingState(plPlatformMatchmakingState state)
{
  m_Ticket.m_State = state;
  m_Ticket.m_Elapsed = plTime::Now() - m_TicketStart;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::MatchmakingStateChanged;
  e.m_sId = m_Ticket.m_sId;
  e.m_Value = static_cast<plInt32>(state);
  e.m_Lobby = m_Ticket.m_Lobby;
  Emit(e);

  if (m_pTicketOp == nullptr)
    return;

  switch (state)
  {
    case plPlatformMatchmakingState::Found:
      m_pTicketOp->MutableResult().m_Ticket = m_Ticket;
      m_pTicketOp->MutableResult().m_Lobby = m_CurrentLobby;
      m_pTicketOp->Succeed();
      m_pTicketOp = nullptr;
      break;

    case plPlatformMatchmakingState::Cancelled:
      m_pTicketOp->MutableResult().m_Ticket = m_Ticket;
      m_pTicketOp->Complete(plPlatformError::Cancelled);
      m_pTicketOp = nullptr;
      break;

    case plPlatformMatchmakingState::Failed:
      m_pTicketOp->MutableResult().m_Ticket = m_Ticket;
      m_pTicketOp->Complete(plPlatformError::Timeout);
      m_pTicketOp = nullptr;
      break;

    default:
      break;
  }
}

void plEosPlatformBackend::UpdateMatchmaking()
{
  if (m_pTicketOp == nullptr || m_bTicketBusy)
    return;

  if (m_pTicketOp->IsCancelRequested())
  {
    Cancel(m_Ticket.m_sId);
    return;
  }

  const plTime now = plTime::Now();
  m_Ticket.m_Elapsed = now - m_TicketStart;
  const bool bTimedOut = m_Ticket.m_Elapsed >= m_TicketRequest.m_Timeout;

  if (m_bInLobby && m_bMatchmakingLobby)
  {
    if (m_CurrentLobby.m_uiMemberCount >= m_TicketRequest.m_uiPlayersWanted)
    {
      m_Ticket.m_Lobby = m_CurrentLobby.m_Id;
      SetMatchmakingState(plPlatformMatchmakingState::Found);
      return;
    }

    if (bTimedOut)
    {
      if (m_CurrentLobby.m_uiMemberCount >= 2)
      {
        m_Ticket.m_Lobby = m_CurrentLobby.m_Id;
        SetMatchmakingState(plPlatformMatchmakingState::Found);
        return;
      }

      plEosCall* pCall = plEosNewCall(this, "Matchmaking.Leave");
      pCall->m_sArg = m_CurrentLobby.m_Id.m_sId;

      EOS_Lobby_LeaveLobbyOptions options = {};
      options.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
      options.LocalUserId = m_LocalProductUserId;
      options.LobbyId = pCall->m_sArg.GetData();

      EOS_Lobby_LeaveLobby(m_hLobby, &options, pCall, &OnMatchmakingLeave);

      SetMatchmakingState(plPlatformMatchmakingState::Failed);
    }

    return;
  }

  if (bTimedOut)
  {
    SetMatchmakingState(plPlatformMatchmakingState::Failed);
    return;
  }

  if (now >= m_TicketNextSearch)
  {
    if (m_Ticket.m_State == plPlatformMatchmakingState::Queued)
      SetMatchmakingState(plPlatformMatchmakingState::Searching);

    MatchmakingSearch();
  }
}

void plEosPlatformBackend::MatchmakingSearch()
{
  EOS_Lobby_CreateLobbySearchOptions createOptions = {};
  createOptions.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
  createOptions.MaxResults = 10;

  EOS_HLobbySearch hSearch = nullptr;
  const EOS_EResult createResult = EOS_Lobby_CreateLobbySearch(m_hLobby, &createOptions, &hSearch);
  if (createResult != EOS_EResult::EOS_Success || hSearch == nullptr)
  {
    m_TicketNextSearch = plTime::Now() + s_SearchInterval;
    return;
  }

  const plString sBucket = GetSetting("LobbyBucket", s_szBucketDefault);

  auto SetParameter = [&](const EOS_Lobby_AttributeData& data, EOS_EComparisonOp op) {
    EOS_LobbySearch_SetParameterOptions paramOptions = {};
    paramOptions.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
    paramOptions.Parameter = &data;
    paramOptions.ComparisonOp = op;

    EOS_LobbySearch_SetParameter(hSearch, &paramOptions);
  };

  {
    EOS_Lobby_AttributeData data;
    plEosFillLobbyAttribute(data, EOS_LOBBY_SEARCH_BUCKET_ID, sBucket.GetData(), false);
    SetParameter(data, EOS_EComparisonOp::EOS_CO_EQUAL);
  }

  {
    EOS_Lobby_AttributeData data;
    plEosFillLobbyAttribute(data, s_szMatchmakingAttribute, "1", true);
    SetParameter(data, EOS_EComparisonOp::EOS_CO_EQUAL);
  }

  {
    EOS_Lobby_AttributeData data = {};
    data.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
    data.Key = EOS_LOBBY_SEARCH_MINSLOTSAVAILABLE;
    data.ValueType = EOS_ELobbyAttributeType::EOS_AT_INT64;
    data.Value.AsInt64 = 1;
    SetParameter(data, EOS_EComparisonOp::EOS_CO_GREATERTHANOREQUAL);
  }

  for (const plPlatformLobbyFilter& filter : m_TicketRequest.m_Filters)
  {
    EOS_Lobby_AttributeData data;
    plEosFillLobbyAttribute(data, filter.m_sKey.GetData(), filter.m_sValue.GetData(), filter.m_bNumeric);

    EOS_EComparisonOp op = EOS_EComparisonOp::EOS_CO_EQUAL;
    switch (filter.m_Comparison)
    {
      case plPlatformFilterComparison::NotEqual:
        op = EOS_EComparisonOp::EOS_CO_NOTEQUAL;
        break;
      case plPlatformFilterComparison::Less:
        op = EOS_EComparisonOp::EOS_CO_LESSTHAN;
        break;
      case plPlatformFilterComparison::Greater:
        op = EOS_EComparisonOp::EOS_CO_GREATERTHAN;
        break;
      default:
        break;
    }

    SetParameter(data, op);
  }

  plEosCall* pCall = plEosNewCall(this, "Matchmaking.Search");
  pCall->m_pUser = hSearch;
  m_bTicketBusy = true;

  EOS_LobbySearch_FindOptions findOptions = {};
  findOptions.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
  findOptions.LocalUserId = m_LocalProductUserId;

  EOS_LobbySearch_Find(hSearch, &findOptions, pCall, &OnMatchmakingSearch);
}

void EOS_CALL plEosPlatformBackend::OnMatchmakingSearch(const EOS_LobbySearch_FindCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;
  EOS_HLobbySearch hSearch = static_cast<EOS_HLobbySearch>(pCall->m_pUser);

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));

  // The ticket may have been cancelled while the search ran.
  if (pThis->m_pTicketOp == nullptr)
  {
    EOS_LobbySearch_Release(hSearch);
    pThis->m_bTicketBusy = false;
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  EOS_HLobbyDetails hChosen = nullptr;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    EOS_LobbySearch_GetSearchResultCountOptions countOptions = {};
    countOptions.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;

    const uint32_t uiCount = EOS_LobbySearch_GetSearchResultCount(hSearch, &countOptions);

    for (uint32_t i = 0; i < uiCount && hChosen == nullptr; ++i)
    {
      EOS_LobbySearch_CopySearchResultByIndexOptions copyOptions = {};
      copyOptions.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
      copyOptions.LobbyIndex = i;

      EOS_HLobbyDetails hDetails = nullptr;
      const EOS_EResult copyResult = EOS_LobbySearch_CopySearchResultByIndex(hSearch, &copyOptions, &hDetails);
      if (copyResult != EOS_EResult::EOS_Success || hDetails == nullptr)
        continue;

      plPlatformLobbyInfo info;
      pThis->ReadLobbyDetails(hDetails, info, false);

      const bool bSizeMatches = info.m_uiMaxMembers == pThis->m_TicketRequest.m_uiPlayersWanted;
      const bool bHasFreeSlot = info.m_uiMemberCount < info.m_uiMaxMembers;

      if (bSizeMatches && bHasFreeSlot)
        hChosen = hDetails;
      else
        EOS_LobbyDetails_Release(hDetails);
    }
  }

  EOS_LobbySearch_Release(hSearch);
  PL_DEFAULT_DELETE(pCall);

  if (hChosen == nullptr)
  {
    pThis->MatchmakingCreate();
    return;
  }

  plEosCall* pJoin = plEosNewCall(pThis, "Matchmaking.Join");

  EOS_Lobby_LocalRTCOptions rtcOptions;
  pThis->FillLocalRtcOptions(rtcOptions);

  EOS_Lobby_JoinLobbyOptions options = {};
  options.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
  options.LobbyDetailsHandle = hChosen;
  options.LocalUserId = pThis->m_LocalProductUserId;
  options.bPresenceEnabled = EOS_TRUE;
  options.LocalRTCOptions = pThis->m_hRTC != nullptr ? &rtcOptions : nullptr;

  EOS_Lobby_JoinLobby(pThis->m_hLobby, &options, pJoin, &OnMatchmakingJoin);
  EOS_LobbyDetails_Release(hChosen);
}

void EOS_CALL plEosPlatformBackend::OnMatchmakingJoin(const EOS_Lobby_JoinLobbyCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);

  pThis->m_bTicketBusy = false;
  pThis->m_TicketNextSearch = plTime::Now() + s_SearchInterval;

  // On failure the next Tick searches again.
  if (pData->ResultCode != EOS_EResult::EOS_Success)
    return;

  if (pThis->RefreshLobbyInfo(pData->LobbyId))
  {
    pThis->m_bMatchmakingLobby = true;
    pThis->m_Ticket.m_Lobby = pThis->m_CurrentLobby.m_Id;
    pThis->EmitLobbyEvent(plPlatformEvent::Type::LobbyMemberJoined, pThis->m_User.m_Id);
  }
}

void plEosPlatformBackend::MatchmakingCreate()
{
  const plString sBucket = GetSetting("LobbyBucket", s_szBucketDefault);
  const bool bVoice = GetSettingBool("LobbyVoice", true) && m_hRTC != nullptr;

  EOS_Lobby_LocalRTCOptions rtcOptions;
  FillLocalRtcOptions(rtcOptions);

  plEosCall* pCall = plEosNewCall(this, "Matchmaking.Create");
  m_bTicketBusy = true;

  EOS_Lobby_CreateLobbyOptions options = {};
  options.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.MaxLobbyMembers = m_TicketRequest.m_uiPlayersWanted;
  options.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
  options.bPresenceEnabled = EOS_TRUE;
  options.bAllowInvites = EOS_TRUE;
  options.BucketId = sBucket.GetData();
  options.bDisableHostMigration = EOS_FALSE;
  options.bEnableRTCRoom = bVoice ? EOS_TRUE : EOS_FALSE;
  options.LocalRTCOptions = bVoice ? &rtcOptions : nullptr;
  options.bEnableJoinById = EOS_TRUE;

  EOS_Lobby_CreateLobby(m_hLobby, &options, pCall, &OnMatchmakingCreate);
}

void EOS_CALL plEosPlatformBackend::OnMatchmakingCreate(const EOS_Lobby_CreateLobbyCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    plLog::Warning("EOS: the matchmaker could not create a lobby: {}", plEosResultName(pData->ResultCode));
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pCall);
    pThis->m_bTicketBusy = false;
    pThis->m_TicketNextSearch = plTime::Now() + s_SearchInterval;
    return;
  }

  pCall->m_sArg = pData->LobbyId;

  EOS_Lobby_UpdateLobbyModificationOptions modOptions = {};
  modOptions.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
  modOptions.LocalUserId = pThis->m_LocalProductUserId;
  modOptions.LobbyId = pData->LobbyId;

  EOS_HLobbyModification hModification = nullptr;
  const EOS_EResult modResult = EOS_Lobby_UpdateLobbyModification(pThis->m_hLobby, &modOptions, &hModification);
  if (modResult != EOS_EResult::EOS_Success || hModification == nullptr)
  {
    // The lobby exists but cannot be tagged; it still works for the local player and direct joins.
    pThis->RefreshLobbyInfo(pData->LobbyId);
    pThis->m_bMatchmakingLobby = true;
    pThis->m_Ticket.m_Lobby = pThis->m_CurrentLobby.m_Id;
    pCall->m_pOp->Complete(plPlatformError::BackendFailure);
    PL_DEFAULT_DELETE(pCall);
    pThis->m_bTicketBusy = false;
    return;
  }

  auto AddAttribute = [&](const char* szKey, const char* szValue) {
    EOS_Lobby_AttributeData data;
    plEosFillLobbyAttribute(data, szKey, szValue, true);

    EOS_LobbyModification_AddAttributeOptions addOptions = {};
    addOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
    addOptions.Attribute = &data;
    addOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;

    EOS_LobbyModification_AddAttribute(hModification, &addOptions);
  };

  AddAttribute(s_szMatchmakingAttribute, "1");

  for (const plPlatformLobbyAttribute& attribute : pThis->m_TicketRequest.m_Attributes)
    AddAttribute(attribute.m_sKey.GetData(), attribute.m_sValue.GetData());

  EOS_Lobby_UpdateLobbyOptions updateOptions = {};
  updateOptions.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
  updateOptions.LobbyModificationHandle = hModification;

  EOS_Lobby_UpdateLobby(pThis->m_hLobby, &updateOptions, pCall, &OnMatchmakingUpdate);
  EOS_LobbyModification_Release(hModification);
}

void EOS_CALL plEosPlatformBackend::OnMatchmakingUpdate(const EOS_Lobby_UpdateLobbyCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pThis->RefreshLobbyInfo(pCall->m_sArg.GetData()))
  {
    pThis->m_bMatchmakingLobby = true;
    pThis->m_Ticket.m_Lobby = pThis->m_CurrentLobby.m_Id;
    pThis->EmitLobbyEvent(plPlatformEvent::Type::LobbyMemberJoined, pThis->m_User.m_Id);
  }

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);

  pThis->m_bTicketBusy = false;
}

void EOS_CALL plEosPlatformBackend::OnMatchmakingLeave(const EOS_Lobby_LeaveLobbyCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);

  pCall->m_pBackend->OnLeftLobby(pCall->m_sArg.GetData());
  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}
