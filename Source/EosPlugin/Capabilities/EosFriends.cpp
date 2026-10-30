#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>

// Friends are an Epic-account concept, users are product user ids. A refresh is a chain: the friend list (Epic
// ids) -> the Connect mapping to product user ids -> per friend the display name and presence. Sub-requests share
// the parent call through m_pUser and count down m_uiPending; the last one completes the operation.
//
// A friend who never played this product has no product user id; that friend is listed with the native id
// "epic:<account id>" so the entry is still addressable through the overlay and invites.

static plPlatformPresenceStatus PresenceStatusFromEos(EOS_Presence_EStatus status)
{
  switch (status)
  {
    case EOS_Presence_EStatus::EOS_PS_Online:
      return plPlatformPresenceStatus::Online;
    case EOS_Presence_EStatus::EOS_PS_Away:
    case EOS_Presence_EStatus::EOS_PS_ExtendedAway:
      return plPlatformPresenceStatus::Away;
    case EOS_Presence_EStatus::EOS_PS_DoNotDisturb:
      return plPlatformPresenceStatus::Busy;
    case EOS_Presence_EStatus::EOS_PS_Offline:
    default:
      return plPlatformPresenceStatus::Offline;
  }
}

plEosPlatformBackend::Friend* plEosPlatformBackend::FindFriendByEpicId(EOS_EpicAccountId id)
{
  for (Friend& f : m_Friends)
  {
    if (f.m_EpicId == id)
      return &f;
  }

  return nullptr;
}

plPlatformOperationPtr plEosPlatformBackend::QueryFriends()
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Friends.Query"))
    return pGate;

  if (!EOS_EpicAccountId_IsValid(m_LocalEpicAccountId))
    return plPlatformOperation::CreateFinished("Friends.Query", plPlatformError::Unsupported);

  if (m_pFriendsOp != nullptr && !m_pFriendsOp->IsFinished())
    return m_pFriendsOp;

  plEosCall* pCall = plEosNewCall(this, "Friends.Query");
  m_pFriendsOp = pCall->m_pOp;

  EOS_Friends_QueryFriendsOptions options = {};
  options.ApiVersion = EOS_FRIENDS_QUERYFRIENDS_API_LATEST;
  options.LocalUserId = m_LocalEpicAccountId;

  EOS_Friends_QueryFriends(m_hFriends, &options, pCall, &OnQueryFriends);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnQueryFriends(const EOS_Friends_QueryFriendsCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    pThis->m_pFriendsOp = nullptr;
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  pThis->m_Friends.Clear();

  EOS_Friends_GetFriendsCountOptions countOptions = {};
  countOptions.ApiVersion = EOS_FRIENDS_GETFRIENDSCOUNT_API_LATEST;
  countOptions.LocalUserId = pThis->m_LocalEpicAccountId;

  const int32_t iCount = EOS_Friends_GetFriendsCount(pThis->m_hFriends, &countOptions);

  plDynamicArray<plString> epicIds;
  epicIds.Reserve(iCount);

  for (int32_t i = 0; i < iCount; ++i)
  {
    EOS_Friends_GetFriendAtIndexOptions atOptions = {};
    atOptions.ApiVersion = EOS_FRIENDS_GETFRIENDATINDEX_API_LATEST;
    atOptions.LocalUserId = pThis->m_LocalEpicAccountId;
    atOptions.Index = i;

    const EOS_EpicAccountId friendId = EOS_Friends_GetFriendAtIndex(pThis->m_hFriends, &atOptions);
    if (!EOS_EpicAccountId_IsValid(friendId))
      continue;

    EOS_Friends_GetStatusOptions statusOptions = {};
    statusOptions.ApiVersion = EOS_FRIENDS_GETSTATUS_API_LATEST;
    statusOptions.LocalUserId = pThis->m_LocalEpicAccountId;
    statusOptions.TargetUserId = friendId;

    if (EOS_Friends_GetStatus(pThis->m_hFriends, &statusOptions) != EOS_EFriendsStatus::EOS_FS_Friends)
      continue;

    Friend& f = pThis->m_Friends.ExpandAndGetRef();
    f.m_EpicId = friendId;
    f.m_User.m_sSecondaryId = plEosEpicAccountIdToString(friendId);
    f.m_User.m_sDisplayName = f.m_User.m_sSecondaryId;
    f.m_User.m_Id = plPlatformUserId::Make("eos", plStringBuilder("epic:", f.m_User.m_sSecondaryId));

    epicIds.PushBack(f.m_User.m_sSecondaryId);
  }

#ifdef EOS_FRIENDS_GETBLOCKEDUSERSCOUNT_API_LATEST
  {
    pThis->m_BlockedUsers.Clear();

    EOS_Friends_GetBlockedUsersCountOptions blockedCountOptions = {};
    blockedCountOptions.ApiVersion = EOS_FRIENDS_GETBLOCKEDUSERSCOUNT_API_LATEST;
    blockedCountOptions.LocalUserId = pThis->m_LocalEpicAccountId;

    const int32_t iBlocked = EOS_Friends_GetBlockedUsersCount(pThis->m_hFriends, &blockedCountOptions);

    for (int32_t i = 0; i < iBlocked; ++i)
    {
      EOS_Friends_GetBlockedUserAtIndexOptions atOptions = {};
      atOptions.ApiVersion = EOS_FRIENDS_GETBLOCKEDUSERATINDEX_API_LATEST;
      atOptions.LocalUserId = pThis->m_LocalEpicAccountId;
      atOptions.Index = i;

      const EOS_EpicAccountId blockedId = EOS_Friends_GetBlockedUserAtIndex(pThis->m_hFriends, &atOptions);
      if (EOS_EpicAccountId_IsValid(blockedId))
        pThis->m_BlockedUsers.PushBack(plEosEpicAccountIdToString(blockedId));
    }
  }
#endif

  if (epicIds.IsEmpty() || pThis->m_hConnect == nullptr)
  {
    pThis->FinishFriendsQuery(pCall);
    return;
  }

  // The mapping query takes at most EOS_CONNECT_QUERYEXTERNALACCOUNTMAPPINGS_MAX_ACCOUNT_IDS per call.
  plDynamicArray<const char*> epicIdPointers;
  const plUInt32 uiMax = plMath::Min<plUInt32>(epicIds.GetCount(), EOS_CONNECT_QUERYEXTERNALACCOUNTMAPPINGS_MAX_ACCOUNT_IDS);

  for (plUInt32 i = 0; i < uiMax; ++i)
    epicIdPointers.PushBack(epicIds[i].GetData());

  EOS_Connect_QueryExternalAccountMappingsOptions options = {};
  options.ApiVersion = EOS_CONNECT_QUERYEXTERNALACCOUNTMAPPINGS_API_LATEST;
  options.LocalUserId = pThis->m_LocalProductUserId;
  options.AccountIdType = EOS_EExternalAccountType::EOS_EAT_EPIC;
  options.ExternalAccountIds = epicIdPointers.GetData();
  options.ExternalAccountIdCount = epicIdPointers.GetCount();

  EOS_Connect_QueryExternalAccountMappings(pThis->m_hConnect, &options, pCall, &OnFriendMappings);
}

void EOS_CALL plEosPlatformBackend::OnFriendMappings(const EOS_Connect_QueryExternalAccountMappingsCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    for (Friend& f : pThis->m_Friends)
    {
      EOS_Connect_GetExternalAccountMappingsOptions options = {};
      options.ApiVersion = EOS_CONNECT_GETEXTERNALACCOUNTMAPPINGS_API_LATEST;
      options.LocalUserId = pThis->m_LocalProductUserId;
      options.AccountIdType = EOS_EExternalAccountType::EOS_EAT_EPIC;
      options.TargetExternalUserId = f.m_User.m_sSecondaryId.GetData();

      const EOS_ProductUserId mapped = EOS_Connect_GetExternalAccountMapping(pThis->m_hConnect, &options);
      if (EOS_ProductUserId_IsValid(mapped))
      {
        f.m_ProductId = mapped;
        f.m_User.m_Id = plEosMakeUserId(mapped);
      }
    }
  }
  else
  {
    plLog::Warning("EOS: mapping friends to product users failed: {}", plEosResultName(pData->ResultCode));
  }

  // Names and presence, two sub-requests per friend.
  pCall->m_uiPending = 0;

  for (Friend& f : pThis->m_Friends)
  {
    if (pThis->m_hUserInfo != nullptr)
    {
      plEosCall* pSub = PL_DEFAULT_NEW(plEosCall);
      pSub->m_pBackend = pThis;
      pSub->m_pOp = pCall->m_pOp;
      pSub->m_pUser = pCall;
      pSub->m_sArg = f.m_User.m_sSecondaryId;
      ++pCall->m_uiPending;

      EOS_UserInfo_QueryUserInfoOptions options = {};
      options.ApiVersion = EOS_USERINFO_QUERYUSERINFO_API_LATEST;
      options.LocalUserId = pThis->m_LocalEpicAccountId;
      options.TargetUserId = f.m_EpicId;

      EOS_UserInfo_QueryUserInfo(pThis->m_hUserInfo, &options, pSub, &OnFriendUserInfo);
    }

    if (pThis->m_hPresence != nullptr)
    {
      plEosCall* pSub = PL_DEFAULT_NEW(plEosCall);
      pSub->m_pBackend = pThis;
      pSub->m_pOp = pCall->m_pOp;
      pSub->m_pUser = pCall;
      pSub->m_sArg = f.m_User.m_sSecondaryId;
      ++pCall->m_uiPending;

      EOS_Presence_QueryPresenceOptions options = {};
      options.ApiVersion = EOS_PRESENCE_QUERYPRESENCE_API_LATEST;
      options.LocalUserId = pThis->m_LocalEpicAccountId;
      options.TargetUserId = f.m_EpicId;

      EOS_Presence_QueryPresence(pThis->m_hPresence, &options, pSub, &OnFriendPresence);
    }
  }

  if (pCall->m_uiPending == 0)
    pThis->FinishFriendsQuery(pCall);
}

void EOS_CALL plEosPlatformBackend::OnFriendUserInfo(const EOS_UserInfo_QueryUserInfoCallbackInfo* pData)
{
  plEosCall* pSub = static_cast<plEosCall*>(pData->ClientData);
  plEosCall* pParent = static_cast<plEosCall*>(pSub->m_pUser);
  plEosPlatformBackend* pThis = pSub->m_pBackend;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    if (Friend* pFriend = pThis->FindFriendByEpicId(pData->TargetUserId))
    {
      EOS_UserInfo_CopyUserInfoOptions copyOptions = {};
      copyOptions.ApiVersion = EOS_USERINFO_COPYUSERINFO_API_LATEST;
      copyOptions.LocalUserId = pThis->m_LocalEpicAccountId;
      copyOptions.TargetUserId = pData->TargetUserId;

      EOS_UserInfo* pInfo = nullptr;
      if (EOS_UserInfo_CopyUserInfo(pThis->m_hUserInfo, &copyOptions, &pInfo) == EOS_EResult::EOS_Success && pInfo != nullptr)
      {
        if (pInfo->DisplayName != nullptr && pInfo->DisplayName[0] != '\0')
        {
          pFriend->m_User.m_sDisplayName = pInfo->DisplayName;
          pThis->m_DisplayNames[plString(pFriend->m_User.m_Id.GetNativeId())] = pFriend->m_User.m_sDisplayName;
        }

        EOS_UserInfo_Release(pInfo);
      }
    }
  }

  PL_DEFAULT_DELETE(pSub);

  if (--pParent->m_uiPending == 0)
    pThis->FinishFriendsQuery(pParent);
}

void EOS_CALL plEosPlatformBackend::OnFriendPresence(const EOS_Presence_QueryPresenceCallbackInfo* pData)
{
  plEosCall* pSub = static_cast<plEosCall*>(pData->ClientData);
  plEosCall* pParent = static_cast<plEosCall*>(pSub->m_pUser);
  plEosPlatformBackend* pThis = pSub->m_pBackend;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    if (Friend* pFriend = pThis->FindFriendByEpicId(pData->TargetUserId))
      pThis->ReadFriendPresence(*pFriend);
  }

  PL_DEFAULT_DELETE(pSub);

  if (--pParent->m_uiPending == 0)
    pThis->FinishFriendsQuery(pParent);
}

void plEosPlatformBackend::ReadFriendPresence(Friend& ref_friend)
{
  EOS_Presence_HasPresenceOptions hasOptions = {};
  hasOptions.ApiVersion = EOS_PRESENCE_HASPRESENCE_API_LATEST;
  hasOptions.LocalUserId = m_LocalEpicAccountId;
  hasOptions.TargetUserId = ref_friend.m_EpicId;

  if (EOS_Presence_HasPresence(m_hPresence, &hasOptions) != EOS_TRUE)
    return;

  EOS_Presence_CopyPresenceOptions copyOptions = {};
  copyOptions.ApiVersion = EOS_PRESENCE_COPYPRESENCE_API_LATEST;
  copyOptions.LocalUserId = m_LocalEpicAccountId;
  copyOptions.TargetUserId = ref_friend.m_EpicId;

  EOS_Presence_Info* pInfo = nullptr;
  if (EOS_Presence_CopyPresence(m_hPresence, &copyOptions, &pInfo) != EOS_EResult::EOS_Success || pInfo == nullptr)
    return;

  ref_friend.m_User.m_Status = PresenceStatusFromEos(pInfo->Status);
  ref_friend.m_User.m_bInSameGame = pInfo->ProductId != nullptr && m_sProductId == pInfo->ProductId;
  if (ref_friend.m_User.m_bInSameGame && ref_friend.m_User.m_Status != plPlatformPresenceStatus::Offline)
    ref_friend.m_User.m_Status = plPlatformPresenceStatus::InGame;

  EOS_Presence_Info_Release(pInfo);
}

void plEosPlatformBackend::FinishFriendsQuery(plEosCall* pCall)
{
  plDynamicArray<plPlatformUser>& users = pCall->m_pOp->MutableResult().m_Users;
  users.Reserve(m_Friends.GetCount());

  for (const Friend& f : m_Friends)
    users.PushBack(f.m_User);

  pCall->m_pOp->Succeed();
  m_pFriendsOp = nullptr;
  PL_DEFAULT_DELETE(pCall);
}

bool plEosPlatformBackend::IsBlocked(const plPlatformUserId& user) const
{
  if (m_BlockedUsers.IsEmpty())
    return false;

  // Blocked users are Epic account ids; a user without a product user id carries that id natively.
  plStringView sNative = user.GetNativeId();
  if (sNative.StartsWith("epic:"))
    sNative = plStringView(sNative.GetStartPointer() + 5, sNative.GetEndPointer());

  for (const plString& sBlocked : m_BlockedUsers)
  {
    if (sBlocked == sNative)
      return true;
  }

  for (const Friend& f : m_Friends)
  {
    if (f.m_User.m_Id == user)
      return m_BlockedUsers.Contains(f.m_User.m_sSecondaryId);
  }

  return false;
}

void EOS_CALL plEosPlatformBackend::OnFriendsUpdate(const EOS_Friends_OnFriendsUpdateInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::FriendsChanged;
  pThis->Emit(e);
}

void EOS_CALL plEosPlatformBackend::OnPresenceChanged(const EOS_Presence_PresenceChangedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  Friend* pFriend = pThis->FindFriendByEpicId(pData->PresenceUserId);
  if (pFriend == nullptr)
    return;

  pThis->ReadFriendPresence(*pFriend);

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::PresenceChanged;
  e.m_User = pFriend->m_User.m_Id;
  pThis->Emit(e);
}
