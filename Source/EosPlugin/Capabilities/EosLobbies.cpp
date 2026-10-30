#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>
#include <Foundation/Utilities/ConversionUtils.h>

// Lobbies map straight onto EOS lobbies. The local user is in at most one (the presence lobby), and its cached
// info is refreshed from the SDK's lobby details on every notification. Attribute values that are integer literals
// are stored as INT64 so numeric search filters compare as numbers; everything else is a string.
//
// EOS lobbies have no chat. SendChat writes the member attribute "plasma_chat" = "<sequence>|<text>"; other members
// see it through the member-update notification and raise LobbyChatMessage once per sequence number.

static constexpr const char* s_szChatAttribute = "plasma_chat";
static constexpr const char* s_szBucketDefault = "plasma";

namespace
{
  // Travels through Create: the attributes still to apply after the lobby exists.
  struct CreateContext
  {
    plDynamicArray<plPlatformLobbyAttribute> m_Attributes;
  };
} // namespace

static EOS_ELobbyPermissionLevel PermissionFromVisibility(plPlatformLobbyVisibility visibility)
{
  switch (visibility)
  {
    case plPlatformLobbyVisibility::Public:
      return EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
    case plPlatformLobbyVisibility::FriendsOnly:
      return EOS_ELobbyPermissionLevel::EOS_LPL_JOINVIAPRESENCE;
    case plPlatformLobbyVisibility::Private:
    case plPlatformLobbyVisibility::Invisible:
    default:
      return EOS_ELobbyPermissionLevel::EOS_LPL_INVITEONLY;
  }
}

static plPlatformLobbyVisibility VisibilityFromPermission(EOS_ELobbyPermissionLevel level, bool bJoinById)
{
  switch (level)
  {
    case EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED:
      return plPlatformLobbyVisibility::Public;
    case EOS_ELobbyPermissionLevel::EOS_LPL_JOINVIAPRESENCE:
      return plPlatformLobbyVisibility::FriendsOnly;
    case EOS_ELobbyPermissionLevel::EOS_LPL_INVITEONLY:
    default:
      return bJoinById ? plPlatformLobbyVisibility::Private : plPlatformLobbyVisibility::Invisible;
  }
}

static EOS_EComparisonOp ComparisonFromFilter(plPlatformFilterComparison comparison)
{
  switch (comparison)
  {
    case plPlatformFilterComparison::NotEqual:
      return EOS_EComparisonOp::EOS_CO_NOTEQUAL;
    case plPlatformFilterComparison::Less:
      return EOS_EComparisonOp::EOS_CO_LESSTHAN;
    case plPlatformFilterComparison::Greater:
      return EOS_EComparisonOp::EOS_CO_GREATERTHAN;
    case plPlatformFilterComparison::Equal:
    default:
      return EOS_EComparisonOp::EOS_CO_EQUAL;
  }
}

static void AttributeToText(const EOS_Lobby_AttributeData& data, plString& out_sValue)
{
  plStringBuilder sTemp;

  switch (data.ValueType)
  {
    case EOS_ELobbyAttributeType::EOS_AT_BOOLEAN:
      out_sValue = data.Value.AsBool == EOS_TRUE ? "true" : "false";
      break;
    case EOS_ELobbyAttributeType::EOS_AT_INT64:
      sTemp.SetFormat("{}", data.Value.AsInt64);
      out_sValue = sTemp;
      break;
    case EOS_ELobbyAttributeType::EOS_AT_DOUBLE:
      sTemp.SetFormat("{}", data.Value.AsDouble);
      out_sValue = sTemp;
      break;
    case EOS_ELobbyAttributeType::EOS_AT_STRING:
    default:
      out_sValue = data.Value.AsUtf8 != nullptr ? data.Value.AsUtf8 : "";
      break;
  }
}

//////////////////////////////////////////////////////////////////////////
// Shared

void plEosPlatformBackend::FillLocalRtcOptions(EOS_Lobby_LocalRTCOptions& out_options) const
{
  out_options = {};
  out_options.ApiVersion = EOS_LOBBY_LOCALRTCOPTIONS_API_LATEST;
  out_options.Flags = 0;
  out_options.bUseManualAudioInput = EOS_FALSE;
  // With a PCM stream provider the mixed voice goes through the engine's audio system; without one EOS renders
  // to the default device itself.
  out_options.bUseManualAudioOutput = m_pPcmProvider != nullptr ? EOS_TRUE : EOS_FALSE;
  out_options.bLocalAudioDeviceInputStartsMuted = m_bCaptureEnabled ? EOS_FALSE : EOS_TRUE;
}

void plEosPlatformBackend::EmitLobbyEvent(plPlatformEvent::Type type, const plPlatformUserId& user)
{
  plPlatformEvent e;
  e.m_Type = type;
  e.m_Lobby = m_CurrentLobby.m_Id;
  e.m_User = user;
  Emit(e);
}

plUInt32 plEosPlatformBackend::CountLobbyMembers(EOS_HLobbyDetails hDetails) const
{
  EOS_LobbyDetails_GetMemberCountOptions options = {};
  options.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;

  return EOS_LobbyDetails_GetMemberCount(hDetails, &options);
}

void plEosPlatformBackend::ReadLobbyDetails(EOS_HLobbyDetails hDetails, plPlatformLobbyInfo& out_info, bool bMembers)
{
  out_info = plPlatformLobbyInfo();

  EOS_LobbyDetails_CopyInfoOptions infoOptions = {};
  infoOptions.ApiVersion = EOS_LOBBYDETAILS_COPYINFO_API_LATEST;

  EOS_LobbyDetails_Info* pInfo = nullptr;
  const EOS_EResult infoResult = EOS_LobbyDetails_CopyInfo(hDetails, &infoOptions, &pInfo);

  if (infoResult == EOS_EResult::EOS_Success && pInfo != nullptr)
  {
    out_info.m_Id.m_sId = pInfo->LobbyId != nullptr ? pInfo->LobbyId : "";
    out_info.m_Owner = plEosMakeUserId(pInfo->LobbyOwnerUserId);
    out_info.m_uiMaxMembers = pInfo->MaxMembers;
    out_info.m_uiMemberCount = pInfo->MaxMembers >= pInfo->AvailableSlots ? pInfo->MaxMembers - pInfo->AvailableSlots : 0;
    out_info.m_Visibility = VisibilityFromPermission(pInfo->PermissionLevel, pInfo->bAllowJoinById == EOS_TRUE);

    EOS_LobbyDetails_Info_Release(pInfo);
  }

  EOS_LobbyDetails_GetAttributeCountOptions countOptions = {};
  countOptions.ApiVersion = EOS_LOBBYDETAILS_GETATTRIBUTECOUNT_API_LATEST;

  const uint32_t uiAttributes = EOS_LobbyDetails_GetAttributeCount(hDetails, &countOptions);

  for (uint32_t i = 0; i < uiAttributes; ++i)
  {
    EOS_LobbyDetails_CopyAttributeByIndexOptions copyOptions = {};
    copyOptions.ApiVersion = EOS_LOBBYDETAILS_COPYATTRIBUTEBYINDEX_API_LATEST;
    copyOptions.AttrIndex = i;

    EOS_Lobby_Attribute* pAttribute = nullptr;
    const EOS_EResult copyResult = EOS_LobbyDetails_CopyAttributeByIndex(hDetails, &copyOptions, &pAttribute);
    if (copyResult != EOS_EResult::EOS_Success || pAttribute == nullptr)
      continue;

    if (pAttribute->Data != nullptr && pAttribute->Data->Key != nullptr)
    {
      plPlatformLobbyAttribute& attribute = out_info.m_Attributes.ExpandAndGetRef();
      attribute.m_sKey = pAttribute->Data->Key;
      AttributeToText(*pAttribute->Data, attribute.m_sValue);
    }

    EOS_Lobby_Attribute_Release(pAttribute);
  }

  if (!bMembers)
    return;

  const uint32_t uiMembers = CountLobbyMembers(hDetails);
  out_info.m_uiMemberCount = uiMembers;

  for (uint32_t i = 0; i < uiMembers; ++i)
  {
    EOS_LobbyDetails_GetMemberByIndexOptions memberOptions = {};
    memberOptions.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST;
    memberOptions.MemberIndex = i;

    const EOS_ProductUserId memberId = EOS_LobbyDetails_GetMemberByIndex(hDetails, &memberOptions);
    if (!EOS_ProductUserId_IsValid(memberId))
      continue;

    plPlatformLobbyMember& member = out_info.m_Members.ExpandAndGetRef();
    member.m_User.m_Id = plEosMakeUserId(memberId);
    member.m_User.m_sDisplayName = memberId == m_LocalProductUserId ? m_User.m_sDisplayName : GetCachedDisplayName(memberId);
    member.m_User.m_Status = plPlatformPresenceStatus::InGame;
    member.m_User.m_bInSameGame = true;

    EOS_LobbyDetails_GetMemberAttributeCountOptions attrCountOptions = {};
    attrCountOptions.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERATTRIBUTECOUNT_API_LATEST;
    attrCountOptions.TargetUserId = memberId;

    const uint32_t uiMemberAttributes = EOS_LobbyDetails_GetMemberAttributeCount(hDetails, &attrCountOptions);

    for (uint32_t a = 0; a < uiMemberAttributes; ++a)
    {
      EOS_LobbyDetails_CopyMemberAttributeByIndexOptions copyOptions = {};
      copyOptions.ApiVersion = EOS_LOBBYDETAILS_COPYMEMBERATTRIBUTEBYINDEX_API_LATEST;
      copyOptions.TargetUserId = memberId;
      copyOptions.AttrIndex = a;

      EOS_Lobby_Attribute* pAttribute = nullptr;
      const EOS_EResult copyResult = EOS_LobbyDetails_CopyMemberAttributeByIndex(hDetails, &copyOptions, &pAttribute);
      if (copyResult != EOS_EResult::EOS_Success || pAttribute == nullptr)
        continue;

      if (pAttribute->Data != nullptr && pAttribute->Data->Key != nullptr)
      {
        plPlatformLobbyAttribute& attribute = member.m_Attributes.ExpandAndGetRef();
        attribute.m_sKey = pAttribute->Data->Key;
        AttributeToText(*pAttribute->Data, attribute.m_sValue);
      }

      EOS_Lobby_Attribute_Release(pAttribute);
    }
  }
}

bool plEosPlatformBackend::RefreshLobbyInfo(const char* szLobbyId)
{
  if (m_hLobby == nullptr || szLobbyId == nullptr)
    return false;

  EOS_Lobby_CopyLobbyDetailsHandleOptions options = {};
  options.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
  options.LobbyId = szLobbyId;
  options.LocalUserId = m_LocalProductUserId;

  EOS_HLobbyDetails hDetails = nullptr;
  if (EOS_Lobby_CopyLobbyDetailsHandle(m_hLobby, &options, &hDetails) != EOS_EResult::EOS_Success || hDetails == nullptr)
    return false;

  ReadLobbyDetails(hDetails, m_CurrentLobby, true);
  EOS_LobbyDetails_Release(hDetails);
  m_bInLobby = true;

  plHybridArray<EOS_ProductUserId, 16> members;

  for (const plPlatformLobbyMember& member : m_CurrentLobby.m_Members)
    members.PushBack(plEosProductUserIdFromUser(member.m_User.m_Id));

  ResolveDisplayNames(members);

  return true;
}

void plEosPlatformBackend::OnLeftLobby(const char* szLobbyId)
{
  if (!m_bInLobby || szLobbyId == nullptr || m_CurrentLobby.m_Id.m_sId != szLobbyId)
    return;

  if (m_VoiceState != plPlatformVoiceState::Disconnected && m_sVoiceRoom == szLobbyId)
  {
    UnregisterVoiceNotifications();
    DestroyVoiceStream();
    SetVoiceState(plPlatformVoiceState::Disconnected);
  }

  EmitLobbyEvent(plPlatformEvent::Type::LobbyLeft, m_User.m_Id);

  m_bInLobby = false;
  m_bMatchmakingLobby = false;
  m_CurrentLobby = plPlatformLobbyInfo();
  m_SeenChatSequence.Clear();
}

void plEosPlatformBackend::ReleaseJoinDetails()
{
  for (auto it = m_JoinDetails.GetIterator(); it.IsValid(); ++it)
  {
    if (it.Value() != nullptr)
      EOS_LobbyDetails_Release(it.Value());
  }

  m_JoinDetails.Clear();
}

//////////////////////////////////////////////////////////////////////////
// Create

plPlatformOperationPtr plEosPlatformBackend::Create(const plPlatformLobbyDesc& desc)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Lobbies.Create"))
    return pGate;

  if (m_bInLobby)
    return plPlatformOperation::CreateFinished("Lobbies.Create", plPlatformError::AlreadyInLobby);

  plEosCall* pCall = plEosNewCall(this, "Lobbies.Create");

  CreateContext* pContext = PL_DEFAULT_NEW(CreateContext);
  pContext->m_Attributes = desc.m_Attributes;
  pCall->m_pUser = pContext;

  const plString sBucket = GetSetting("LobbyBucket", s_szBucketDefault);
  const bool bVoice = GetSettingBool("LobbyVoice", true) && m_hRTC != nullptr;

  EOS_Lobby_LocalRTCOptions rtcOptions;
  FillLocalRtcOptions(rtcOptions);

  EOS_Lobby_CreateLobbyOptions options = {};
  options.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.MaxLobbyMembers = plMath::Max<plUInt32>(1, desc.m_uiMaxMembers);
  options.PermissionLevel = PermissionFromVisibility(desc.m_Visibility);
  options.bPresenceEnabled = EOS_TRUE;
  options.bAllowInvites = EOS_TRUE;
  options.BucketId = sBucket.GetData();
  options.bDisableHostMigration = EOS_FALSE;
  options.bEnableRTCRoom = bVoice ? EOS_TRUE : EOS_FALSE;
  options.LocalRTCOptions = bVoice ? &rtcOptions : nullptr;
  options.LobbyId = nullptr;
  options.bEnableJoinById = desc.m_Visibility != plPlatformLobbyVisibility::Invisible ? EOS_TRUE : EOS_FALSE;
  options.bRejoinAfterKickRequiresInvite = EOS_FALSE;

  EOS_Lobby_CreateLobby(m_hLobby, &options, pCall, &OnCreateLobby);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnCreateLobby(const EOS_Lobby_CreateLobbyCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;
  CreateContext* pContext = static_cast<CreateContext*>(pCall->m_pUser);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    plLog::Warning("EOS: creating a lobby failed: {}", plEosResultName(pData->ResultCode));
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pContext);
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  pCall->m_sArg = pData->LobbyId;
  pThis->m_bMatchmakingLobby = false;

  if (pContext->m_Attributes.IsEmpty())
  {
    pThis->RefreshLobbyInfo(pCall->m_sArg.GetData());
    pCall->m_pOp->MutableResult().m_Lobby = pThis->m_CurrentLobby;
    pCall->m_pOp->Succeed();
    PL_DEFAULT_DELETE(pContext);
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  EOS_Lobby_UpdateLobbyModificationOptions modOptions = {};
  modOptions.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
  modOptions.LocalUserId = pThis->m_LocalProductUserId;
  modOptions.LobbyId = pCall->m_sArg.GetData();

  EOS_HLobbyModification hModification = nullptr;
  const EOS_EResult modResult = EOS_Lobby_UpdateLobbyModification(pThis->m_hLobby, &modOptions, &hModification);
  if (modResult != EOS_EResult::EOS_Success || hModification == nullptr)
  {
    pThis->RefreshLobbyInfo(pCall->m_sArg.GetData());
    pCall->m_pOp->MutableResult().m_Lobby = pThis->m_CurrentLobby;
    pCall->m_pOp->Complete(plPlatformError::BackendFailure);
    PL_DEFAULT_DELETE(pContext);
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  for (const plPlatformLobbyAttribute& attribute : pContext->m_Attributes)
  {
    EOS_Lobby_AttributeData data;
    plEosFillLobbyAttribute(data, attribute.m_sKey.GetData(), attribute.m_sValue.GetData(), true);

    EOS_LobbyModification_AddAttributeOptions addOptions = {};
    addOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
    addOptions.Attribute = &data;
    addOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;

    EOS_LobbyModification_AddAttribute(hModification, &addOptions);
  }

  EOS_Lobby_UpdateLobbyOptions updateOptions = {};
  updateOptions.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
  updateOptions.LobbyModificationHandle = hModification;

  EOS_Lobby_UpdateLobby(pThis->m_hLobby, &updateOptions, pCall, &OnUpdateLobby);
  EOS_LobbyModification_Release(hModification);

  // OnUpdateLobby only needs the lobby id in m_sArg; the context is done.
  PL_DEFAULT_DELETE(pContext);
  pCall->m_pUser = nullptr;
}

void EOS_CALL plEosPlatformBackend::OnUpdateLobby(const EOS_Lobby_UpdateLobbyCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
    plLog::Warning("EOS: updating lobby '{}' failed: {}", pCall->m_sArg, plEosResultName(pData->ResultCode));

  if (pThis->RefreshLobbyInfo(pCall->m_sArg.GetData()))
    pCall->m_pOp->MutableResult().m_Lobby = pThis->m_CurrentLobby;

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

//////////////////////////////////////////////////////////////////////////
// Find

plPlatformOperationPtr plEosPlatformBackend::Find(const plPlatformLobbySearch& search)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Lobbies.Find"))
    return pGate;

  EOS_Lobby_CreateLobbySearchOptions createOptions = {};
  createOptions.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
  createOptions.MaxResults = plMath::Clamp<plUInt32>(search.m_uiMaxResults, 1, EOS_LOBBY_MAX_SEARCH_RESULTS);

  EOS_HLobbySearch hSearch = nullptr;
  const EOS_EResult createResult = EOS_Lobby_CreateLobbySearch(m_hLobby, &createOptions, &hSearch);
  if (createResult != EOS_EResult::EOS_Success || hSearch == nullptr)
    return plPlatformOperation::CreateFinished("Lobbies.Find", plEosResultToError(createResult));

  const plString sBucket = GetSetting("LobbyBucket", s_szBucketDefault);

  {
    EOS_Lobby_AttributeData data;
    plEosFillLobbyAttribute(data, EOS_LOBBY_SEARCH_BUCKET_ID, sBucket.GetData(), false);

    EOS_LobbySearch_SetParameterOptions paramOptions = {};
    paramOptions.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
    paramOptions.Parameter = &data;
    paramOptions.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;

    EOS_LobbySearch_SetParameter(hSearch, &paramOptions);
  }

  for (const plPlatformLobbyFilter& filter : search.m_Filters)
  {
    EOS_Lobby_AttributeData data;
    plEosFillLobbyAttribute(data, filter.m_sKey.GetData(), filter.m_sValue.GetData(), filter.m_bNumeric);

    EOS_LobbySearch_SetParameterOptions paramOptions = {};
    paramOptions.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
    paramOptions.Parameter = &data;
    paramOptions.ComparisonOp = ComparisonFromFilter(filter.m_Comparison);

    EOS_LobbySearch_SetParameter(hSearch, &paramOptions);
  }

  if (search.m_bAvailableSlotsOnly)
  {
    EOS_Lobby_AttributeData data = {};
    data.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
    data.Key = EOS_LOBBY_SEARCH_MINSLOTSAVAILABLE;
    data.ValueType = EOS_ELobbyAttributeType::EOS_AT_INT64;
    data.Value.AsInt64 = 1;

    EOS_LobbySearch_SetParameterOptions paramOptions = {};
    paramOptions.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
    paramOptions.Parameter = &data;
    paramOptions.ComparisonOp = EOS_EComparisonOp::EOS_CO_GREATERTHANOREQUAL;

    EOS_LobbySearch_SetParameter(hSearch, &paramOptions);
  }

  plEosCall* pCall = plEosNewCall(this, "Lobbies.Find");
  pCall->m_pUser = hSearch;

  EOS_LobbySearch_FindOptions findOptions = {};
  findOptions.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
  findOptions.LocalUserId = m_LocalProductUserId;

  EOS_LobbySearch_Find(hSearch, &findOptions, pCall, &OnLobbySearchFind);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnLobbySearchFind(const EOS_LobbySearch_FindCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;
  EOS_HLobbySearch hSearch = static_cast<EOS_HLobbySearch>(pCall->m_pUser);

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    EOS_LobbySearch_GetSearchResultCountOptions countOptions = {};
    countOptions.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;

    const uint32_t uiCount = EOS_LobbySearch_GetSearchResultCount(hSearch, &countOptions);

    plDynamicArray<plPlatformLobbyInfo>& lobbies = pCall->m_pOp->MutableResult().m_Lobbies;
    lobbies.Reserve(uiCount);

    for (uint32_t i = 0; i < uiCount; ++i)
    {
      EOS_LobbySearch_CopySearchResultByIndexOptions copyOptions = {};
      copyOptions.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
      copyOptions.LobbyIndex = i;

      EOS_HLobbyDetails hDetails = nullptr;
      const EOS_EResult copyResult = EOS_LobbySearch_CopySearchResultByIndex(hSearch, &copyOptions, &hDetails);
      if (copyResult != EOS_EResult::EOS_Success || hDetails == nullptr)
        continue;

      pThis->ReadLobbyDetails(hDetails, lobbies.ExpandAndGetRef(), false);
      EOS_LobbyDetails_Release(hDetails);
    }
  }

  EOS_LobbySearch_Release(hSearch);

  // An empty result set is a NotFound from EOS; to the caller it is a successful search with no lobbies.
  const plPlatformError result = pData->ResultCode == EOS_EResult::EOS_NotFound ? plPlatformError::Success : plEosResultToError(pData->ResultCode);

  pCall->m_pOp->Complete(result);
  PL_DEFAULT_DELETE(pCall);
}

//////////////////////////////////////////////////////////////////////////
// Join / leave

plPlatformOperationPtr plEosPlatformBackend::Join(const plPlatformLobbyId& lobby)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Lobbies.Join"))
    return pGate;

  if (!lobby.IsValid())
    return plPlatformOperation::CreateFinished("Lobbies.Join", plPlatformError::InvalidArgument);

  if (m_bInLobby)
  {
    // Joining the lobby we are already in is a no-op success.
    const plPlatformError result = m_CurrentLobby.m_Id == lobby ? plPlatformError::Success : plPlatformError::AlreadyInLobby;
    return plPlatformOperation::CreateFinished("Lobbies.Join", result);
  }

  plEosCall* pCall = plEosNewCall(this, "Lobbies.Join");
  pCall->m_sArg = lobby.m_sId;

  EOS_Lobby_LocalRTCOptions rtcOptions;
  FillLocalRtcOptions(rtcOptions);

  // A details handle from an invite or the overlay joins lobbies that are not joinable by id.
  EOS_HLobbyDetails hDetails = nullptr;
  if (m_JoinDetails.TryGetValue(lobby.m_sId, hDetails) && hDetails != nullptr)
  {
    m_JoinDetails.Remove(lobby.m_sId);

    EOS_Lobby_JoinLobbyOptions options = {};
    options.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
    options.LobbyDetailsHandle = hDetails;
    options.LocalUserId = m_LocalProductUserId;
    options.bPresenceEnabled = EOS_TRUE;
    options.LocalRTCOptions = m_hRTC != nullptr ? &rtcOptions : nullptr;

    EOS_Lobby_JoinLobby(m_hLobby, &options, pCall, &OnJoinLobbyDetails);
    EOS_LobbyDetails_Release(hDetails);

    return pCall->m_pOp;
  }

  EOS_Lobby_JoinLobbyByIdOptions options = {};
  options.ApiVersion = EOS_LOBBY_JOINLOBBYBYID_API_LATEST;
  options.LobbyId = pCall->m_sArg.GetData();
  options.LocalUserId = m_LocalProductUserId;
  options.bPresenceEnabled = EOS_TRUE;
  options.LocalRTCOptions = m_hRTC != nullptr ? &rtcOptions : nullptr;

  EOS_Lobby_JoinLobbyById(m_hLobby, &options, pCall, &OnJoinLobby);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnJoinLobby(const EOS_Lobby_JoinLobbyByIdCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    pThis->m_bMatchmakingLobby = false;

    if (pThis->RefreshLobbyInfo(pData->LobbyId))
      pCall->m_pOp->MutableResult().m_Lobby = pThis->m_CurrentLobby;
  }
  else
  {
    plLog::Warning("EOS: joining lobby '{}' failed: {}", pCall->m_sArg, plEosResultName(pData->ResultCode));
  }

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

void EOS_CALL plEosPlatformBackend::OnJoinLobbyDetails(const EOS_Lobby_JoinLobbyCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    pThis->m_bMatchmakingLobby = false;

    if (pThis->RefreshLobbyInfo(pData->LobbyId))
      pCall->m_pOp->MutableResult().m_Lobby = pThis->m_CurrentLobby;
  }
  else
  {
    plLog::Warning("EOS: joining lobby '{}' failed: {}", pCall->m_sArg, plEosResultName(pData->ResultCode));
  }

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

plPlatformOperationPtr plEosPlatformBackend::Leave(const plPlatformLobbyId& lobby)
{
  if (!m_bInLobby || !lobby.IsValid() || m_CurrentLobby.m_Id != lobby)
    return plPlatformOperation::CreateFinished("Lobbies.Leave", plPlatformError::NotFound);

  if (plPlatformOperationPtr pGate = GateSignedIn("Lobbies.Leave"))
    return pGate;

  plEosCall* pCall = plEosNewCall(this, "Lobbies.Leave");
  pCall->m_sArg = lobby.m_sId;

  EOS_Lobby_LeaveLobbyOptions options = {};
  options.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.LobbyId = pCall->m_sArg.GetData();

  EOS_Lobby_LeaveLobby(m_hLobby, &options, pCall, &OnLeaveLobby);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnLeaveLobby(const EOS_Lobby_LeaveLobbyCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  // Whatever the server said, the local side is out.
  pThis->OnLeftLobby(pCall->m_sArg.GetData());

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

//////////////////////////////////////////////////////////////////////////
// Cached state and modifications

const plPlatformLobbyInfo* plEosPlatformBackend::GetInfo(const plPlatformLobbyId& lobby) const
{
  if (m_bInLobby && m_CurrentLobby.m_Id == lobby)
    return &m_CurrentLobby;

  return nullptr;
}

plResult plEosPlatformBackend::ModifyLobby(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue, bool bMember)
{
  if (!m_bInLobby || m_CurrentLobby.m_Id != lobby || !m_bSignedIn || sKey.IsEmpty())
    return PL_FAILURE;

  EOS_Lobby_UpdateLobbyModificationOptions modOptions = {};
  modOptions.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
  modOptions.LocalUserId = m_LocalProductUserId;
  modOptions.LobbyId = m_CurrentLobby.m_Id.m_sId.GetData();

  EOS_HLobbyModification hModification = nullptr;
  const EOS_EResult modResult = EOS_Lobby_UpdateLobbyModification(m_hLobby, &modOptions, &hModification);
  if (modResult != EOS_EResult::EOS_Success || hModification == nullptr)
  {
    plLog::Warning("EOS: lobby modification failed: {}", plEosResultName(modResult));
    return PL_FAILURE;
  }

  // The SDK wants zero-terminated strings.
  const plStringBuilder sKeyZ(sKey);
  const plStringBuilder sValueZ(sValue);

  EOS_Lobby_AttributeData data;
  plEosFillLobbyAttribute(data, sKeyZ.GetData(), sValueZ.GetData(), true);

  EOS_EResult addResult;

  if (bMember)
  {
    EOS_LobbyModification_AddMemberAttributeOptions addOptions = {};
    addOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDMEMBERATTRIBUTE_API_LATEST;
    addOptions.Attribute = &data;
    addOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;

    addResult = EOS_LobbyModification_AddMemberAttribute(hModification, &addOptions);
  }
  else
  {
    EOS_LobbyModification_AddAttributeOptions addOptions = {};
    addOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
    addOptions.Attribute = &data;
    addOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;

    addResult = EOS_LobbyModification_AddAttribute(hModification, &addOptions);
  }

  if (addResult != EOS_EResult::EOS_Success)
  {
    EOS_LobbyModification_Release(hModification);
    return PL_FAILURE;
  }

  const char* szKind = bMember ? "Lobbies.SetMemberAttribute" : "Lobbies.SetAttribute";

  plEosCall* pCall = plEosNewCall(this, szKind);
  pCall->m_sArg = m_CurrentLobby.m_Id.m_sId;

  EOS_Lobby_UpdateLobbyOptions updateOptions = {};
  updateOptions.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
  updateOptions.LobbyModificationHandle = hModification;

  EOS_Lobby_UpdateLobby(m_hLobby, &updateOptions, pCall, &OnUpdateLobby);
  EOS_LobbyModification_Release(hModification);

  return PL_SUCCESS;
}

plResult plEosPlatformBackend::SetAttribute(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue)
{
  // Lobby attributes belong to the owner; members can only set their own member attributes.
  if (m_bInLobby && m_CurrentLobby.m_Owner != m_User.m_Id)
    return PL_FAILURE;

  return ModifyLobby(lobby, sKey, sValue, false);
}

plResult plEosPlatformBackend::SetMemberAttribute(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue)
{
  return ModifyLobby(lobby, sKey, sValue, true);
}

plResult plEosPlatformBackend::SetVisibility(const plPlatformLobbyId& lobby, plPlatformLobbyVisibility visibility)
{
  if (!m_bInLobby || m_CurrentLobby.m_Id != lobby || !m_bSignedIn || m_CurrentLobby.m_Owner != m_User.m_Id)
    return PL_FAILURE;

  EOS_Lobby_UpdateLobbyModificationOptions modOptions = {};
  modOptions.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
  modOptions.LocalUserId = m_LocalProductUserId;
  modOptions.LobbyId = m_CurrentLobby.m_Id.m_sId.GetData();

  EOS_HLobbyModification hModification = nullptr;
  const EOS_EResult modResult = EOS_Lobby_UpdateLobbyModification(m_hLobby, &modOptions, &hModification);
  if (modResult != EOS_EResult::EOS_Success || hModification == nullptr)
  {
    plLog::Warning("EOS: lobby modification failed: {}", plEosResultName(modResult));
    return PL_FAILURE;
  }

  EOS_LobbyModification_SetPermissionLevelOptions permissionOptions = {};
  permissionOptions.ApiVersion = EOS_LOBBYMODIFICATION_SETPERMISSIONLEVEL_API_LATEST;
  permissionOptions.PermissionLevel = PermissionFromVisibility(visibility);

  if (EOS_LobbyModification_SetPermissionLevel(hModification, &permissionOptions) != EOS_EResult::EOS_Success)
  {
    EOS_LobbyModification_Release(hModification);
    return PL_FAILURE;
  }

  plEosCall* pCall = plEosNewCall(this, "Lobbies.SetVisibility");
  pCall->m_sArg = m_CurrentLobby.m_Id.m_sId;

  EOS_Lobby_UpdateLobbyOptions updateOptions = {};
  updateOptions.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
  updateOptions.LobbyModificationHandle = hModification;

  EOS_Lobby_UpdateLobby(m_hLobby, &updateOptions, pCall, &OnUpdateLobby);
  EOS_LobbyModification_Release(hModification);

  m_CurrentLobby.m_Visibility = visibility;
  return PL_SUCCESS;
}

plResult plEosPlatformBackend::SetOwner(const plPlatformLobbyId& lobby, const plPlatformUserId& user)
{
  if (!m_bInLobby || m_CurrentLobby.m_Id != lobby || !m_bSignedIn)
    return PL_FAILURE;

  const EOS_ProductUserId targetId = plEosProductUserIdFromUser(user);
  if (!EOS_ProductUserId_IsValid(targetId))
    return PL_FAILURE;

  plEosCall* pCall = plEosNewCall(this, "Lobbies.SetOwner");
  pCall->m_sArg = m_CurrentLobby.m_Id.m_sId;

  EOS_Lobby_PromoteMemberOptions options = {};
  options.ApiVersion = EOS_LOBBY_PROMOTEMEMBER_API_LATEST;
  options.LobbyId = pCall->m_sArg.GetData();
  options.LocalUserId = m_LocalProductUserId;
  options.TargetUserId = targetId;

  EOS_Lobby_PromoteMember(m_hLobby, &options, pCall, &OnPromoteMember);

  return PL_SUCCESS;
}

void EOS_CALL plEosPlatformBackend::OnPromoteMember(const EOS_Lobby_PromoteMemberCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
    plLog::Warning("EOS: promoting a lobby member failed: {}", plEosResultName(pData->ResultCode));

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

plResult plEosPlatformBackend::SendChat(const plPlatformLobbyId& lobby, plStringView sText)
{
  if (!m_bInLobby || m_CurrentLobby.m_Id != lobby)
    return PL_FAILURE;

  ++m_uiChatSequence;

  plStringBuilder sValue;
  sValue.SetFormat("{}|{}", m_uiChatSequence, sText);

  if (ModifyLobby(lobby, s_szChatAttribute, sValue, true).Failed())
    return PL_FAILURE;

  // The local message is raised here; the member-update notification skips it via the seen sequence.
  m_SeenChatSequence[plString(m_User.m_Id.GetNativeId())] = m_uiChatSequence;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::LobbyChatMessage;
  e.m_Lobby = m_CurrentLobby.m_Id;
  e.m_User = m_User.m_Id;
  e.m_sText = sText;
  Emit(e);

  return PL_SUCCESS;
}

void plEosPlatformBackend::CheckLobbyChat(EOS_ProductUserId member)
{
  const plPlatformUserId memberId = plEosMakeUserId(member);

  for (const plPlatformLobbyMember& lobbyMember : m_CurrentLobby.m_Members)
  {
    if (lobbyMember.m_User.m_Id != memberId)
      continue;

    for (const plPlatformLobbyAttribute& attribute : lobbyMember.m_Attributes)
    {
      if (attribute.m_sKey != s_szChatAttribute)
        continue;

      const char* szSeparator = attribute.m_sValue.FindSubString("|");
      if (szSeparator == nullptr)
        return;

      const plStringView sSequence(attribute.m_sValue.GetData(), szSeparator);

      plUInt32 uiSequence = 0;
      if (plConversionUtils::StringToUInt(sSequence, uiSequence).Failed())
        return;

      const plString sKey(memberId.GetNativeId());

      plUInt32 uiSeen = 0;
      m_SeenChatSequence.TryGetValue(sKey, uiSeen);

      if (uiSequence <= uiSeen)
        return;

      m_SeenChatSequence[sKey] = uiSequence;

      plPlatformEvent e;
      e.m_Type = plPlatformEvent::Type::LobbyChatMessage;
      e.m_Lobby = m_CurrentLobby.m_Id;
      e.m_User = memberId;
      e.m_sText = szSeparator + 1;
      Emit(e);

      return;
    }

    return;
  }
}

//////////////////////////////////////////////////////////////////////////
// Notifications

void plEosPlatformBackend::RegisterLobbyNotifications()
{
  if (m_hLobby == nullptr)
    return;

  {
    EOS_Lobby_AddNotifyLobbyUpdateReceivedOptions options = {};
    options.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYUPDATERECEIVED_API_LATEST;

    m_LobbyUpdateNotification = EOS_Lobby_AddNotifyLobbyUpdateReceived(m_hLobby, &options, this, &OnLobbyUpdateReceived);
  }

  {
    EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions options = {};
    options.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERUPDATERECEIVED_API_LATEST;

    m_LobbyMemberUpdateNotification = EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(m_hLobby, &options, this, &OnLobbyMemberUpdateReceived);
  }

  {
    EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions options = {};
    options.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;

    m_LobbyMemberStatusNotification = EOS_Lobby_AddNotifyLobbyMemberStatusReceived(m_hLobby, &options, this, &OnLobbyMemberStatusReceived);
  }

  {
    EOS_Lobby_AddNotifyLobbyInviteReceivedOptions options = {};
    options.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYINVITERECEIVED_API_LATEST;

    m_LobbyInviteReceivedNotification = EOS_Lobby_AddNotifyLobbyInviteReceived(m_hLobby, &options, this, &OnLobbyInviteReceived);
  }

  {
    EOS_Lobby_AddNotifyLobbyInviteAcceptedOptions options = {};
    options.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYINVITEACCEPTED_API_LATEST;

    m_LobbyInviteAcceptedNotification = EOS_Lobby_AddNotifyLobbyInviteAccepted(m_hLobby, &options, this, &OnLobbyInviteAccepted);
  }

  {
    EOS_Lobby_AddNotifyJoinLobbyAcceptedOptions options = {};
    options.ApiVersion = EOS_LOBBY_ADDNOTIFYJOINLOBBYACCEPTED_API_LATEST;

    m_JoinLobbyAcceptedNotification = EOS_Lobby_AddNotifyJoinLobbyAccepted(m_hLobby, &options, this, &OnJoinLobbyAccepted);
  }

  {
    EOS_Lobby_AddNotifyRTCRoomConnectionChangedOptions options = {};
    options.ApiVersion = EOS_LOBBY_ADDNOTIFYRTCROOMCONNECTIONCHANGED_API_LATEST;

    m_RtcRoomConnectionNotification = EOS_Lobby_AddNotifyRTCRoomConnectionChanged(m_hLobby, &options, this, &OnRtcRoomConnectionChanged);
  }
}

void plEosPlatformBackend::UnregisterLobbyNotifications()
{
  ReleaseJoinDetails();

  if (m_hLobby == nullptr)
    return;

  if (m_LobbyUpdateNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Lobby_RemoveNotifyLobbyUpdateReceived(m_hLobby, m_LobbyUpdateNotification);

  if (m_LobbyMemberUpdateNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Lobby_RemoveNotifyLobbyMemberUpdateReceived(m_hLobby, m_LobbyMemberUpdateNotification);

  if (m_LobbyMemberStatusNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(m_hLobby, m_LobbyMemberStatusNotification);

  if (m_LobbyInviteReceivedNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Lobby_RemoveNotifyLobbyInviteReceived(m_hLobby, m_LobbyInviteReceivedNotification);

  if (m_LobbyInviteAcceptedNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Lobby_RemoveNotifyLobbyInviteAccepted(m_hLobby, m_LobbyInviteAcceptedNotification);

  if (m_JoinLobbyAcceptedNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Lobby_RemoveNotifyJoinLobbyAccepted(m_hLobby, m_JoinLobbyAcceptedNotification);

  if (m_RtcRoomConnectionNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Lobby_RemoveNotifyRTCRoomConnectionChanged(m_hLobby, m_RtcRoomConnectionNotification);

  m_LobbyUpdateNotification = EOS_INVALID_NOTIFICATIONID;
  m_LobbyMemberUpdateNotification = EOS_INVALID_NOTIFICATIONID;
  m_LobbyMemberStatusNotification = EOS_INVALID_NOTIFICATIONID;
  m_LobbyInviteReceivedNotification = EOS_INVALID_NOTIFICATIONID;
  m_LobbyInviteAcceptedNotification = EOS_INVALID_NOTIFICATIONID;
  m_JoinLobbyAcceptedNotification = EOS_INVALID_NOTIFICATIONID;
  m_RtcRoomConnectionNotification = EOS_INVALID_NOTIFICATIONID;
}

void EOS_CALL plEosPlatformBackend::OnLobbyUpdateReceived(const EOS_Lobby_LobbyUpdateReceivedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (!pThis->m_bInLobby || pThis->m_CurrentLobby.m_Id.m_sId != pData->LobbyId)
    return;

  pThis->RefreshLobbyInfo(pData->LobbyId);
  pThis->EmitLobbyEvent(plPlatformEvent::Type::LobbyUpdated);
}

void EOS_CALL plEosPlatformBackend::OnLobbyMemberUpdateReceived(const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (!pThis->m_bInLobby || pThis->m_CurrentLobby.m_Id.m_sId != pData->LobbyId)
    return;

  pThis->RefreshLobbyInfo(pData->LobbyId);

  if (pData->TargetUserId != pThis->m_LocalProductUserId)
    pThis->CheckLobbyChat(pData->TargetUserId);

  pThis->EmitLobbyEvent(plPlatformEvent::Type::LobbyUpdated, plEosMakeUserId(pData->TargetUserId));
}

void EOS_CALL plEosPlatformBackend::OnLobbyMemberStatusReceived(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (!pThis->m_bInLobby || pThis->m_CurrentLobby.m_Id.m_sId != pData->LobbyId)
    return;

  const bool bLocal = pData->TargetUserId == pThis->m_LocalProductUserId;
  const plPlatformUserId user = plEosMakeUserId(pData->TargetUserId);

  switch (pData->CurrentStatus)
  {
    case EOS_ELobbyMemberStatus::EOS_LMS_JOINED:
      pThis->RefreshLobbyInfo(pData->LobbyId);
      pThis->EmitLobbyEvent(plPlatformEvent::Type::LobbyMemberJoined, user);
      break;

    case EOS_ELobbyMemberStatus::EOS_LMS_LEFT:
    case EOS_ELobbyMemberStatus::EOS_LMS_DISCONNECTED:
    case EOS_ELobbyMemberStatus::EOS_LMS_KICKED:
      if (bLocal)
      {
        pThis->OnLeftLobby(pData->LobbyId);
      }
      else
      {
        pThis->RefreshLobbyInfo(pData->LobbyId);
        pThis->m_SeenChatSequence.Remove(plString(user.GetNativeId()));
        pThis->EmitLobbyEvent(plPlatformEvent::Type::LobbyMemberLeft, user);
      }
      break;

    case EOS_ELobbyMemberStatus::EOS_LMS_PROMOTED:
      pThis->RefreshLobbyInfo(pData->LobbyId);
      pThis->EmitLobbyEvent(plPlatformEvent::Type::LobbyOwnerChanged, user);
      break;

    case EOS_ELobbyMemberStatus::EOS_LMS_CLOSED:
      pThis->OnLeftLobby(pData->LobbyId);
      break;

    default:
      break;
  }
}

void EOS_CALL plEosPlatformBackend::OnLobbyInviteReceived(const EOS_Lobby_LobbyInviteReceivedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (pData->InviteId == nullptr)
    return;

  EOS_Lobby_CopyLobbyDetailsHandleByInviteIdOptions options = {};
  options.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLEBYINVITEID_API_LATEST;
  options.InviteId = pData->InviteId;

  EOS_HLobbyDetails hDetails = nullptr;
  const EOS_EResult copyResult = EOS_Lobby_CopyLobbyDetailsHandleByInviteId(pThis->m_hLobby, &options, &hDetails);
  if (copyResult != EOS_EResult::EOS_Success || hDetails == nullptr)
    return;

  plPlatformLobbyInfo info;
  pThis->ReadLobbyDetails(hDetails, info, false);

  // Keep the handle so Join can use it; a previous handle for the same lobby is replaced.
  EOS_HLobbyDetails hPrevious = nullptr;
  if (pThis->m_JoinDetails.TryGetValue(info.m_Id.m_sId, hPrevious) && hPrevious != nullptr)
    EOS_LobbyDetails_Release(hPrevious);

  pThis->m_JoinDetails[info.m_Id.m_sId] = hDetails;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::InviteReceived;
  e.m_User = plEosMakeUserId(pData->TargetUserId);
  e.m_Lobby = info.m_Id;
  pThis->Emit(e);

  pThis->ResolveDisplayNames(plArrayPtr<const EOS_ProductUserId>(&pData->TargetUserId, 1));
}

void EOS_CALL plEosPlatformBackend::OnLobbyInviteAccepted(const EOS_Lobby_LobbyInviteAcceptedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (pData->LobbyId == nullptr)
    return;

  if (pData->InviteId != nullptr)
  {
    EOS_Lobby_CopyLobbyDetailsHandleByInviteIdOptions options = {};
    options.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLEBYINVITEID_API_LATEST;
    options.InviteId = pData->InviteId;

    EOS_HLobbyDetails hDetails = nullptr;
    const EOS_EResult copyResult = EOS_Lobby_CopyLobbyDetailsHandleByInviteId(pThis->m_hLobby, &options, &hDetails);

    if (copyResult == EOS_EResult::EOS_Success && hDetails != nullptr)
    {
      EOS_HLobbyDetails hPrevious = nullptr;
      if (pThis->m_JoinDetails.TryGetValue(plString(pData->LobbyId), hPrevious) && hPrevious != nullptr)
        EOS_LobbyDetails_Release(hPrevious);

      pThis->m_JoinDetails[plString(pData->LobbyId)] = hDetails;
    }
  }

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::JoinRequested;
  e.m_User = plEosMakeUserId(pData->TargetUserId);
  e.m_Lobby.m_sId = pData->LobbyId;
  pThis->Emit(e);

  pThis->m_LaunchJoin = plPlatformJoinRequest();
  pThis->m_LaunchJoin.m_From = e.m_User;
  pThis->m_LaunchJoin.m_Lobby = e.m_Lobby;
  pThis->m_bLaunchJoin = true;
}

void EOS_CALL plEosPlatformBackend::OnJoinLobbyAccepted(const EOS_Lobby_JoinLobbyAcceptedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  EOS_Lobby_CopyLobbyDetailsHandleByUiEventIdOptions options = {};
  options.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLEBYUIEVENTID_API_LATEST;
  options.UiEventId = pData->UiEventId;

  EOS_HLobbyDetails hDetails = nullptr;
  const EOS_EResult copyResult = EOS_Lobby_CopyLobbyDetailsHandleByUiEventId(pThis->m_hLobby, &options, &hDetails);

  // The overlay waits for the acknowledgement whether or not the copy worked.
  if (pThis->m_hUI != nullptr)
  {
    EOS_UI_AcknowledgeEventIdOptions ackOptions = {};
    ackOptions.ApiVersion = EOS_UI_ACKNOWLEDGEEVENTID_API_LATEST;
    ackOptions.UiEventId = pData->UiEventId;
    ackOptions.Result = copyResult;

    EOS_UI_AcknowledgeEventId(pThis->m_hUI, &ackOptions);
  }

  if (copyResult != EOS_EResult::EOS_Success || hDetails == nullptr)
    return;

  plPlatformLobbyInfo info;
  pThis->ReadLobbyDetails(hDetails, info, false);

  EOS_HLobbyDetails hPrevious = nullptr;
  if (pThis->m_JoinDetails.TryGetValue(info.m_Id.m_sId, hPrevious) && hPrevious != nullptr)
    EOS_LobbyDetails_Release(hPrevious);

  pThis->m_JoinDetails[info.m_Id.m_sId] = hDetails;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::JoinRequested;
  e.m_User = info.m_Owner;
  e.m_Lobby = info.m_Id;
  pThis->Emit(e);

  pThis->m_LaunchJoin = plPlatformJoinRequest();
  pThis->m_LaunchJoin.m_From = info.m_Owner;
  pThis->m_LaunchJoin.m_Lobby = info.m_Id;
  pThis->m_bLaunchJoin = true;
}

void EOS_CALL plEosPlatformBackend::OnRtcRoomConnectionChanged(const EOS_Lobby_RTCRoomConnectionChangedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (pData->LobbyId == nullptr || pThis->m_sVoiceRoom.IsEmpty())
    return;

  // m_sVoiceRoom holds the lobby id the voice room belongs to.
  if (pThis->m_sVoiceRoom != pData->LobbyId)
    return;

  const plPlatformVoiceState state = pData->bIsConnected == EOS_TRUE ? plPlatformVoiceState::Connected : plPlatformVoiceState::Connecting;
  pThis->SetVoiceState(state);
}
