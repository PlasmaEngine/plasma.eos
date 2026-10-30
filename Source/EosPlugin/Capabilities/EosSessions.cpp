#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>

// Invites ride on lobbies. A join request arrives through the overlay (invite accepted, presence join, both
// handled in EosLobbies.cpp and EosPresence.cpp) or through the launch arguments "-EosLobbyId=<id>" and
// "-EosConnect=<string>", which a launcher or a shortcut can pass. The Epic launcher itself passes no join
// target; the overlay delivers it after sign-in.

plPlatformOperationPtr plEosPlatformBackend::Invite(const plPlatformUserId& user, const plPlatformLobbyId& lobby)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Sessions.Invite"))
    return pGate;

  if (!m_bInLobby || !lobby.IsValid() || m_CurrentLobby.m_Id != lobby)
    return plPlatformOperation::CreateFinished("Sessions.Invite", plPlatformError::NotFound);

  // A friend who never played the product has no product user id and cannot receive a lobby invite.
  const EOS_ProductUserId target = plEosProductUserIdFromUser(user);
  if (!EOS_ProductUserId_IsValid(target))
    return plPlatformOperation::CreateFinished("Sessions.Invite", plPlatformError::NotFound);

  plEosCall* pCall = plEosNewCall(this, "Sessions.Invite");
  pCall->m_sArg = lobby.m_sId;

  EOS_Lobby_SendInviteOptions options = {};
  options.ApiVersion = EOS_LOBBY_SENDINVITE_API_LATEST;
  options.LobbyId = pCall->m_sArg.GetData();
  options.LocalUserId = m_LocalProductUserId;
  options.TargetUserId = target;

  EOS_Lobby_SendInvite(m_hLobby, &options, pCall, &OnSendInvite);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnSendInvite(const EOS_Lobby_SendInviteCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
    plLog::Warning("EOS: sending a lobby invite failed: {}", plEosResultName(pData->ResultCode));

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

bool plEosPlatformBackend::TakeLaunchJoinRequest(plPlatformJoinRequest& out_request)
{
  if (!m_bLaunchJoin)
    return false;

  out_request = m_LaunchJoin;
  m_LaunchJoin = plPlatformJoinRequest();
  m_bLaunchJoin = false;

  return true;
}

void plEosPlatformBackend::ReadLaunchArguments()
{
  const plString sLobby = plEosFindLaunchArgument("-EosLobbyId=");
  const plString sConnect = plEosFindLaunchArgument("-EosConnect=");

  if (sLobby.IsEmpty() && sConnect.IsEmpty())
    return;

  m_LaunchJoin = plPlatformJoinRequest();
  m_LaunchJoin.m_Lobby.m_sId = sLobby;
  m_LaunchJoin.m_sConnectString = sConnect;
  m_bLaunchJoin = true;
}
