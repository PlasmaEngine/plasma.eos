#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>
#include <Foundation/System/EnvironmentVariableUtils.h>

// Sign-in is a chain: EOS Auth (Epic account) -> copy the auth token -> EOS Connect (product user id), creating
// the product user on first contact -> the local user's display name. One plEosCall travels through the whole
// chain and completes the operation at the end or at the first failure.

namespace
{
  // Values of plEosCall::m_iNumber while it travels through the sign-in chain.
  constexpr plInt64 s_iAuthStepDefault = 0;
  constexpr plInt64 s_iAuthStepPersistent = 1; ///< Persistent-token attempt; the account portal is the fallback.
  constexpr plInt64 s_iAuthStepRefresh = 2;    ///< Connect token renewal for a user who is already signed in.
} // namespace

plPlatformOperationPtr plEosPlatformBackend::SignIn()
{
  if (m_bSignedIn)
  {
    plPlatformOperationPtr pOp = plPlatformOperation::Create("Identity.SignIn");
    pOp->MutableResult().m_User = m_User;
    pOp->Succeed();
    return pOp;
  }

  if (m_pSignInOp != nullptr && !m_pSignInOp->IsFinished())
    return m_pSignInOp;

  if (m_hAuth == nullptr || m_hConnect == nullptr)
    return plPlatformOperation::CreateFinished("Identity.SignIn", plPlatformError::BackendFailure);

  plEosCall* pCall = plEosNewCall(this, "Identity.SignIn");
  m_pSignInOp = pCall->m_pOp;
  SetOnlineState(plPlatformOnlineState::Connecting);

  // Developer auth tool: PLASMA_EOS_DEV_AUTH = "localhost:8081|CredentialName".
  const plString sDevAuth = plEnvironmentVariableUtils::GetValueString("PLASMA_EOS_DEV_AUTH");
  if (!sDevAuth.IsEmpty())
  {
    const char* szSeparator = sDevAuth.FindSubString("|");
    if (szSeparator == nullptr)
    {
      plLog::Error("EOS: PLASMA_EOS_DEV_AUTH must be 'host:port|credential'.");
      pCall->m_pOp->Complete(plPlatformError::InvalidArgument);
      SetOnlineState(plPlatformOnlineState::Offline);
      PL_DEFAULT_DELETE(pCall);
      return m_pSignInOp;
    }

    // Both halves are held by the call so they outlive the request.
    pCall->m_sArg = sDevAuth;
    const plStringBuilder sHost(plStringView(sDevAuth.GetData(), szSeparator));
    const plStringBuilder sCredential(plStringView(szSeparator + 1));

    BeginAuthLogin(pCall, EOS_ELoginCredentialType::EOS_LCT_Developer, sHost.GetData(), sCredential.GetData());
    return m_pSignInOp;
  }

  // Launched by the Epic launcher: exchange code.
  const plString sExchangeCode = plEosFindLaunchArgument("-AUTH_PASSWORD=");
  if (!sExchangeCode.IsEmpty())
  {
    BeginAuthLogin(pCall, EOS_ELoginCredentialType::EOS_LCT_ExchangeCode, nullptr, sExchangeCode.GetData());
    return m_pSignInOp;
  }

  // Otherwise the token remembered from a previous run; OnAuthLogin falls back to the account portal.
  pCall->m_iNumber = s_iAuthStepPersistent;
  BeginAuthLogin(pCall, EOS_ELoginCredentialType::EOS_LCT_PersistentAuth, nullptr, nullptr);

  return m_pSignInOp;
}

void plEosPlatformBackend::BeginAuthLogin(plEosCall* pCall, EOS_ELoginCredentialType type, const char* szId, const char* szToken)
{
  EOS_Auth_Credentials credentials = {};
  credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
  credentials.Type = type;
  credentials.Id = szId;
  credentials.Token = szToken;

  EOS_Auth_LoginOptions options = {};
  options.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
  options.Credentials = &credentials;
  options.ScopeFlags = EOS_EAuthScopeFlags::EOS_AS_BasicProfile | EOS_EAuthScopeFlags::EOS_AS_FriendsList | EOS_EAuthScopeFlags::EOS_AS_Presence;

  EOS_Auth_Login(m_hAuth, &options, pCall, &OnAuthLogin);
}

void EOS_CALL plEosPlatformBackend::OnAuthLogin(const EOS_Auth_LoginCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pThis->StopCancelledSignIn(pCall))
    return;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    pThis->m_LocalEpicAccountId = pData->LocalUserId;
    pThis->BeginConnectLogin(pCall);
    return;
  }

  if (pCall->m_iNumber == s_iAuthStepPersistent)
  {
    // No stored token, or it expired: show the account portal.
    pCall->m_iNumber = s_iAuthStepDefault;
    pThis->BeginAuthLogin(pCall, EOS_ELoginCredentialType::EOS_LCT_AccountPortal, nullptr, nullptr);
    return;
  }

  plLog::Error("EOS: Auth login failed: {}", plEosResultName(pData->ResultCode));
  pThis->SetOnlineState(plPlatformOnlineState::Offline);
  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

void plEosPlatformBackend::BeginConnectLogin(plEosCall* pCall)
{
  EOS_Auth_CopyUserAuthTokenOptions copyOptions = {};
  copyOptions.ApiVersion = EOS_AUTH_COPYUSERAUTHTOKEN_API_LATEST;

  EOS_Auth_Token* pToken = nullptr;
  const EOS_EResult copyResult = EOS_Auth_CopyUserAuthToken(m_hAuth, &copyOptions, m_LocalEpicAccountId, &pToken);
  if (copyResult != EOS_EResult::EOS_Success || pToken == nullptr)
  {
    plLog::Error("EOS: could not copy the auth token: {}", plEosResultName(copyResult));
    SetOnlineState(plPlatformOnlineState::Offline);
    pCall->m_pOp->Complete(plEosResultToError(copyResult));
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  EOS_Connect_Credentials credentials = {};
  credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
  credentials.Token = pToken->AccessToken;
  credentials.Type = EOS_EExternalCredentialType::EOS_ECT_EPIC;

  EOS_Connect_LoginOptions options = {};
  options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
  options.Credentials = &credentials;

  const EOS_Connect_OnLoginCallback onLogin = pCall->m_iNumber == s_iAuthStepRefresh ? &OnConnectRefresh : &OnConnectLogin;

  EOS_Connect_Login(m_hConnect, &options, pCall, onLogin);
  EOS_Auth_Token_Release(pToken);
}

void EOS_CALL plEosPlatformBackend::OnConnectLogin(const EOS_Connect_LoginCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pThis->StopCancelledSignIn(pCall))
    return;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    pThis->m_LocalProductUserId = pData->LocalUserId;
    pThis->FinishSignIn(pCall);
    return;
  }

  if (pData->ResultCode == EOS_EResult::EOS_InvalidUser && pData->ContinuanceToken != nullptr)
  {
    EOS_Connect_CreateUserOptions options = {};
    options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
    options.ContinuanceToken = pData->ContinuanceToken;

    EOS_Connect_CreateUser(pThis->m_hConnect, &options, pCall, &OnConnectCreateUser);
    return;
  }

  plLog::Error("EOS: Connect login failed: {}", plEosResultName(pData->ResultCode));
  pThis->SetOnlineState(plPlatformOnlineState::Offline);
  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

void EOS_CALL plEosPlatformBackend::OnConnectCreateUser(const EOS_Connect_CreateUserCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pThis->StopCancelledSignIn(pCall))
    return;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    plLog::Error("EOS: creating the product user failed: {}", plEosResultName(pData->ResultCode));
    pThis->SetOnlineState(plPlatformOnlineState::Offline);
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  pThis->m_LocalProductUserId = pData->LocalUserId;
  pThis->FinishSignIn(pCall);
}

void plEosPlatformBackend::FinishSignIn(plEosCall* pCall)
{
  m_User = plPlatformUser();
  m_User.m_Id = plEosMakeUserId(m_LocalProductUserId);
  m_User.m_sSecondaryId = plEosEpicAccountIdToString(m_LocalEpicAccountId);
  m_User.m_sDisplayName = "Epic user";
  m_User.m_Status = plPlatformPresenceStatus::Online;
  m_User.m_bInSameGame = true;

  if (m_hUserInfo != nullptr && EOS_EpicAccountId_IsValid(m_LocalEpicAccountId))
  {
    EOS_UserInfo_QueryUserInfoOptions options = {};
    options.ApiVersion = EOS_USERINFO_QUERYUSERINFO_API_LATEST;
    options.LocalUserId = m_LocalEpicAccountId;
    options.TargetUserId = m_LocalEpicAccountId;

    EOS_UserInfo_QueryUserInfo(m_hUserInfo, &options, pCall, &OnLocalUserInfo);
    return;
  }

  CompleteSignIn(pCall);
}

void EOS_CALL plEosPlatformBackend::OnLocalUserInfo(const EOS_UserInfo_QueryUserInfoCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    EOS_UserInfo_CopyUserInfoOptions copyOptions = {};
    copyOptions.ApiVersion = EOS_USERINFO_COPYUSERINFO_API_LATEST;
    copyOptions.LocalUserId = pThis->m_LocalEpicAccountId;
    copyOptions.TargetUserId = pThis->m_LocalEpicAccountId;

    EOS_UserInfo* pInfo = nullptr;
    const EOS_EResult copyResult = EOS_UserInfo_CopyUserInfo(pThis->m_hUserInfo, &copyOptions, &pInfo);

    if (copyResult == EOS_EResult::EOS_Success && pInfo != nullptr)
    {
      if (pInfo->DisplayName != nullptr && pInfo->DisplayName[0] != '\0')
        pThis->m_User.m_sDisplayName = pInfo->DisplayName;

      EOS_UserInfo_Release(pInfo);
    }
  }

  pThis->CompleteSignIn(pCall);
}

void plEosPlatformBackend::CompleteSignIn(plEosCall* pCall)
{
  if (StopCancelledSignIn(pCall))
    return;

  m_DisplayNames[plString(m_User.m_Id.GetNativeId())] = m_User.m_sDisplayName;
  m_bSignedIn = true;
  SetOnlineState(plPlatformOnlineState::Online);
  RegisterUserNotifications();
  RegisterTransportNotifications();
  m_bPresenceDirty = !m_PresenceData.IsEmpty() || !m_sJoinString.IsEmpty();

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::SignedIn;
  e.m_User = m_User.m_Id;
  Emit(e);

  pCall->m_pOp->MutableResult().m_User = m_User;
  pCall->m_pOp->Succeed();
  m_pSignInOp = nullptr;
  PL_DEFAULT_DELETE(pCall);
}

bool plEosPlatformBackend::StopCancelledSignIn(plEosCall* pCall)
{
  if (!pCall->m_pOp->IsCancelRequested())
    return false;

  // Continuing offline must not publish a late SignedIn event or open another
  // portal as the outstanding native callback chain finishes.
  SetOnlineState(plPlatformOnlineState::Offline);
  m_User = plPlatformUser();
  pCall->m_pOp->Complete(plPlatformError::Cancelled);
  m_pSignInOp = nullptr;
  PL_DEFAULT_DELETE(pCall);

  return true;
}

//////////////////////////////////////////////////////////////////////////
// Sign out

plPlatformOperationPtr plEosPlatformBackend::SignOut()
{
  if (!m_bSignedIn && !EOS_EpicAccountId_IsValid(m_LocalEpicAccountId))
    return plPlatformOperation::CreateFinished("Identity.SignOut", plPlatformError::Success);

  plEosCall* pCall = plEosNewCall(this, "Identity.SignOut");

  if (m_hAuth == nullptr || !EOS_EpicAccountId_IsValid(m_LocalEpicAccountId))
  {
    OnAuthLoggedOut();
    pCall->m_pOp->Succeed();

    plPlatformOperationPtr pOp = pCall->m_pOp;
    PL_DEFAULT_DELETE(pCall);
    return pOp;
  }

  // An explicit sign-out also forgets the persistent token, so the next start asks again.
  EOS_Auth_DeletePersistentAuthOptions deleteOptions = {};
  deleteOptions.ApiVersion = EOS_AUTH_DELETEPERSISTENTAUTH_API_LATEST;

  EOS_Auth_DeletePersistentAuth(m_hAuth, &deleteOptions, nullptr, &OnDeletePersistentAuth);

  EOS_Auth_LogoutOptions options = {};
  options.ApiVersion = EOS_AUTH_LOGOUT_API_LATEST;
  options.LocalUserId = m_LocalEpicAccountId;

  EOS_Auth_Logout(m_hAuth, &options, pCall, &OnAuthLogout);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnDeletePersistentAuth(const EOS_Auth_DeletePersistentAuthCallbackInfo* pData)
{
  PL_IGNORE_UNUSED(pData);
}

void EOS_CALL plEosPlatformBackend::OnAuthLogout(const EOS_Auth_LogoutCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);

  pCall->m_pBackend->OnAuthLoggedOut();
  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

void plEosPlatformBackend::OnAuthLoggedOut()
{
  const bool bWasSignedIn = m_bSignedIn;

  if (m_bInLobby)
    OnLeftLobby(m_CurrentLobby.m_Id.m_sId.GetData());

  UnregisterVoiceNotifications();
  DestroyVoiceStream();
  UnregisterTransportNotifications();
  UnregisterUserNotifications();

  m_bSignedIn = false;
  m_LocalEpicAccountId = nullptr;
  m_LocalProductUserId = nullptr;
  m_Friends.Clear();
  m_Peers.Clear();
  m_bOwnershipKnown = false;
  m_OwnedItems.Clear();
  m_ServerStats.Clear();
  SetVoiceState(plPlatformVoiceState::Disconnected);
  SetOnlineState(plPlatformOnlineState::Offline);

  if (bWasSignedIn)
  {
    plPlatformEvent e;
    e.m_Type = plPlatformEvent::Type::SignedOut;
    e.m_User = m_User.m_Id;
    Emit(e);
  }

  m_User = plPlatformUser();
}

//////////////////////////////////////////////////////////////////////////
// Status notifications

void plEosPlatformBackend::RegisterUserNotifications()
{
  if (m_hAuth != nullptr && m_AuthStatusNotification == EOS_INVALID_NOTIFICATIONID)
  {
    EOS_Auth_AddNotifyLoginStatusChangedOptions options = {};
    options.ApiVersion = EOS_AUTH_ADDNOTIFYLOGINSTATUSCHANGED_API_LATEST;

    m_AuthStatusNotification = EOS_Auth_AddNotifyLoginStatusChanged(m_hAuth, &options, this, &OnAuthLoginStatusChanged);
  }

  if (m_hConnect != nullptr && m_ConnectStatusNotification == EOS_INVALID_NOTIFICATIONID)
  {
    EOS_Connect_AddNotifyLoginStatusChangedOptions options = {};
    options.ApiVersion = EOS_CONNECT_ADDNOTIFYLOGINSTATUSCHANGED_API_LATEST;

    m_ConnectStatusNotification = EOS_Connect_AddNotifyLoginStatusChanged(m_hConnect, &options, this, &OnConnectLoginStatusChanged);
  }

  if (m_hConnect != nullptr && m_AuthExpirationNotification == EOS_INVALID_NOTIFICATIONID)
  {
    EOS_Connect_AddNotifyAuthExpirationOptions options = {};
    options.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;

    m_AuthExpirationNotification = EOS_Connect_AddNotifyAuthExpiration(m_hConnect, &options, this, &OnConnectAuthExpiration);
  }

  if (m_hFriends != nullptr && m_FriendsUpdateNotification == EOS_INVALID_NOTIFICATIONID)
  {
    EOS_Friends_AddNotifyFriendsUpdateOptions options = {};
    options.ApiVersion = EOS_FRIENDS_ADDNOTIFYFRIENDSUPDATE_API_LATEST;

    m_FriendsUpdateNotification = EOS_Friends_AddNotifyFriendsUpdate(m_hFriends, &options, this, &OnFriendsUpdate);
  }

  if (m_hPresence != nullptr && m_PresenceChangedNotification == EOS_INVALID_NOTIFICATIONID)
  {
    EOS_Presence_AddNotifyOnPresenceChangedOptions options = {};
    options.ApiVersion = EOS_PRESENCE_ADDNOTIFYONPRESENCECHANGED_API_LATEST;

    m_PresenceChangedNotification = EOS_Presence_AddNotifyOnPresenceChanged(m_hPresence, &options, this, &OnPresenceChanged);
  }

  if (m_hPresence != nullptr && m_JoinGameAcceptedNotification == EOS_INVALID_NOTIFICATIONID)
  {
    EOS_Presence_AddNotifyJoinGameAcceptedOptions options = {};
    options.ApiVersion = EOS_PRESENCE_ADDNOTIFYJOINGAMEACCEPTED_API_LATEST;

    m_JoinGameAcceptedNotification = EOS_Presence_AddNotifyJoinGameAccepted(m_hPresence, &options, this, &OnJoinGameAccepted);
  }
}

void plEosPlatformBackend::UnregisterUserNotifications()
{
  if (m_hAuth != nullptr && m_AuthStatusNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Auth_RemoveNotifyLoginStatusChanged(m_hAuth, m_AuthStatusNotification);

  if (m_hConnect != nullptr && m_ConnectStatusNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Connect_RemoveNotifyLoginStatusChanged(m_hConnect, m_ConnectStatusNotification);

  if (m_hConnect != nullptr && m_AuthExpirationNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Connect_RemoveNotifyAuthExpiration(m_hConnect, m_AuthExpirationNotification);

  if (m_hFriends != nullptr && m_FriendsUpdateNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Friends_RemoveNotifyFriendsUpdate(m_hFriends, m_FriendsUpdateNotification);

  if (m_hPresence != nullptr && m_PresenceChangedNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Presence_RemoveNotifyOnPresenceChanged(m_hPresence, m_PresenceChangedNotification);

  if (m_hPresence != nullptr && m_JoinGameAcceptedNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_Presence_RemoveNotifyJoinGameAccepted(m_hPresence, m_JoinGameAcceptedNotification);

  m_AuthStatusNotification = EOS_INVALID_NOTIFICATIONID;
  m_ConnectStatusNotification = EOS_INVALID_NOTIFICATIONID;
  m_AuthExpirationNotification = EOS_INVALID_NOTIFICATIONID;
  m_FriendsUpdateNotification = EOS_INVALID_NOTIFICATIONID;
  m_PresenceChangedNotification = EOS_INVALID_NOTIFICATIONID;
  m_JoinGameAcceptedNotification = EOS_INVALID_NOTIFICATIONID;
}

void EOS_CALL plEosPlatformBackend::OnAuthLoginStatusChanged(const EOS_Auth_LoginStatusChangedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (pData->CurrentStatus == EOS_ELoginStatus::EOS_LS_NotLoggedIn && pThis->m_bSignedIn)
  {
    plLog::Info("EOS: the Epic account signed out.");
    pThis->OnAuthLoggedOut();
  }
}

void EOS_CALL plEosPlatformBackend::OnConnectLoginStatusChanged(const EOS_Connect_LoginStatusChangedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (!pThis->m_bSignedIn)
    return;

  switch (pData->CurrentStatus)
  {
    case EOS_ELoginStatus::EOS_LS_LoggedIn:
      pThis->SetOnlineState(plPlatformOnlineState::Online);
      break;
    case EOS_ELoginStatus::EOS_LS_UsingLocalProfile:
      pThis->SetOnlineState(plPlatformOnlineState::Connecting);
      break;
    case EOS_ELoginStatus::EOS_LS_NotLoggedIn:
    default:
      pThis->SetOnlineState(plPlatformOnlineState::Offline);
      break;
  }
}

void EOS_CALL plEosPlatformBackend::OnConnectAuthExpiration(const EOS_Connect_AuthExpirationCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (!pThis->m_bSignedIn || !EOS_EpicAccountId_IsValid(pThis->m_LocalEpicAccountId))
    return;

  // The Connect token is about to lapse; a fresh Auth token renews it without a new sign-in.
  plEosCall* pCall = plEosNewCall(pThis, "Identity.Refresh");
  pCall->m_iNumber = s_iAuthStepRefresh;
  pThis->BeginConnectLogin(pCall);
}

void EOS_CALL plEosPlatformBackend::OnConnectRefresh(const EOS_Connect_LoginCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
    plLog::Warning("EOS: renewing the Connect session failed: {}", plEosResultName(pData->ResultCode));

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

//////////////////////////////////////////////////////////////////////////
// Other users

plPlatformOperationPtr plEosPlatformBackend::QueryUser(const plPlatformUserId& user)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Identity.QueryUser"))
    return pGate;

  if (user == m_User.m_Id)
  {
    plPlatformOperationPtr pOp = plPlatformOperation::Create("Identity.QueryUser");
    pOp->MutableResult().m_User = m_User;
    pOp->Succeed();
    return pOp;
  }

  EOS_ProductUserId targetId = plEosProductUserIdFromUser(user);
  if (!EOS_ProductUserId_IsValid(targetId))
    return plPlatformOperation::CreateFinished("Identity.QueryUser", plPlatformError::InvalidArgument);

  for (const Friend& friendEntry : m_Friends)
  {
    if (friendEntry.m_User.m_Id == user)
    {
      plPlatformOperationPtr pOp = plPlatformOperation::Create("Identity.QueryUser");
      pOp->MutableResult().m_User = friendEntry.m_User;
      pOp->Succeed();
      return pOp;
    }
  }

  plEosCall* pCall = plEosNewCall(this, "Identity.QueryUser");
  pCall->m_sArg = user.m_sCanonical;

  EOS_Connect_QueryProductUserIdMappingsOptions options = {};
  options.ApiVersion = EOS_CONNECT_QUERYPRODUCTUSERIDMAPPINGS_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.ProductUserIds = &targetId;
  options.ProductUserIdCount = 1;

  EOS_Connect_QueryProductUserIdMappings(m_hConnect, &options, pCall, &OnQueryUserMappings);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnQueryUserMappings(const EOS_Connect_QueryProductUserIdMappingsCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
    PL_DEFAULT_DELETE(pCall);
    return;
  }

  const plPlatformUserId userId = plPlatformUserId::FromCanonical(pCall->m_sArg);
  const EOS_ProductUserId targetId = plEosProductUserIdFromUser(userId);

  plPlatformUser& user = pCall->m_pOp->MutableResult().m_User;
  user.m_Id = userId;
  user.m_sDisplayName = pThis->GetCachedDisplayName(targetId);

  EOS_Connect_CopyProductUserExternalAccountByAccountTypeOptions copyOptions = {};
  copyOptions.ApiVersion = EOS_CONNECT_COPYPRODUCTUSEREXTERNALACCOUNTBYACCOUNTTYPE_API_LATEST;
  copyOptions.TargetUserId = targetId;
  copyOptions.AccountIdType = EOS_EExternalAccountType::EOS_EAT_EPIC;

  EOS_Connect_ExternalAccountInfo* pInfo = nullptr;
  const EOS_EResult copyResult = EOS_Connect_CopyProductUserExternalAccountByAccountType(pThis->m_hConnect, &copyOptions, &pInfo);

  if (copyResult == EOS_EResult::EOS_Success && pInfo != nullptr)
  {
    if (pInfo->DisplayName != nullptr && pInfo->DisplayName[0] != '\0')
    {
      user.m_sDisplayName = pInfo->DisplayName;
      pThis->m_DisplayNames[userId.GetNativeId()] = user.m_sDisplayName;
    }

    if (pInfo->AccountId != nullptr)
      user.m_sSecondaryId = pInfo->AccountId;

    EOS_Connect_ExternalAccountInfo_Release(pInfo);
  }

  pCall->m_pOp->Succeed();
  PL_DEFAULT_DELETE(pCall);
}
