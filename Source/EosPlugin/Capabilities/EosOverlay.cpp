#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>

// The Epic overlay exposes one page to games: the friends list, which also carries invites when the user is in a
// presence-enabled lobby. Profile, store and settings pages have no EOS entry point and answer failure.

plResult plEosPlatformBackend::Show(plPlatformOverlayPage page)
{
  if (m_hUI == nullptr || !EOS_EpicAccountId_IsValid(m_LocalEpicAccountId))
    return PL_FAILURE;

  switch (page)
  {
    case plPlatformOverlayPage::Default:
    case plPlatformOverlayPage::Friends:
    case plPlatformOverlayPage::Players:
    case plPlatformOverlayPage::InviteDialog:
      break;
    default:
      return PL_FAILURE;
  }

  plEosCall* pCall = plEosNewCall(this, "Overlay.Show");

  EOS_UI_ShowFriendsOptions options = {};
  options.ApiVersion = EOS_UI_SHOWFRIENDS_API_LATEST;
  options.LocalUserId = m_LocalEpicAccountId;

  EOS_UI_ShowFriends(m_hUI, &options, pCall, &OnShowFriends);

  return PL_SUCCESS;
}

plResult plEosPlatformBackend::ShowUser(const plPlatformUserId& user)
{
  PL_IGNORE_UNUSED(user);
  return PL_FAILURE;
}

plResult plEosPlatformBackend::ShowInviteDialog(const plPlatformLobbyId& lobby)
{
  if (!m_bInLobby || !lobby.IsValid() || m_CurrentLobby.m_Id != lobby)
    return PL_FAILURE;

  return Show(plPlatformOverlayPage::Friends);
}

plResult plEosPlatformBackend::ShowStore(plStringView sDlcBackendId)
{
  PL_IGNORE_UNUSED(sDlcBackendId);
  return PL_FAILURE;
}

void EOS_CALL plEosPlatformBackend::OnShowFriends(const EOS_UI_ShowFriendsCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);

  if (pData->ResultCode != EOS_EResult::EOS_Success)
    plLog::Warning("EOS: showing the friends overlay failed: {}", plEosResultName(pData->ResultCode));

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

void EOS_CALL plEosPlatformBackend::OnDisplaySettingsUpdated(const EOS_UI_OnDisplaySettingsUpdatedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);
  const bool bVisible = pData->bIsVisible == EOS_TRUE;

  if (pThis->m_bOverlayVisible == bVisible)
    return;

  pThis->m_bOverlayVisible = bVisible;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::OverlayToggled;
  e.m_bFlag = bVisible;
  pThis->Emit(e);
}
