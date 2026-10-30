#include <EosPlugin/EosPluginPCH.h>

#include <AudioSystem/Core/AudioMiddleware.h>
#include <EosPlugin/EosPlatformBackend.h>
#include <Foundation/Threading/Lock.h>

// Voice is the RTC room of the lobby the user is in: lobbies are created and joined with an RTC room when the
// "LobbyVoice" setting is on, and the SDK connects the room itself. JoinRoom(<lobby id>) therefore resolves the
// room name, hooks the audio notifications and completes once the room reports connected. Rooms outside a lobby
// would need a participant token from a trusted server (EOS_RTCAdmin), which this backend has no source for.
//
// Output: when an plAudioPcmStreamProvider is registered the lobby is joined with manual audio output and the mixed
// frames from AddNotifyAudioBeforeRender are written into a PCM stream on the master bus. Without a provider the
// SDK renders to the default device itself.

static constexpr plUInt32 s_uiRtcRoomNameSize = 256;

void plEosPlatformBackend::SetVoiceState(plPlatformVoiceState state)
{
  if (m_VoiceState == state)
    return;

  m_VoiceState = state;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::VoiceStateChanged;
  e.m_Value = static_cast<plInt32>(state);
  Emit(e);

  if (state == plPlatformVoiceState::Connected && m_pVoiceJoinOp != nullptr)
  {
    m_pVoiceJoinOp->Succeed();
    m_pVoiceJoinOp = nullptr;
  }
  else if (state == plPlatformVoiceState::Disconnected)
  {
    m_SpeakingUsers.Clear();

    if (m_pVoiceJoinOp != nullptr)
    {
      m_pVoiceJoinOp->Complete(plPlatformError::BackendFailure);
      m_pVoiceJoinOp = nullptr;
    }
  }
}

plPlatformOperationPtr plEosPlatformBackend::JoinRoom(plStringView sRoom)
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Voice.Join"))
    return pGate;

  if (m_hRTCAudio == nullptr || m_hLobby == nullptr)
    return plPlatformOperation::CreateFinished("Voice.Join", plPlatformError::Unsupported);

  const plString sLobby = sRoom.IsEmpty() && m_bInLobby ? m_CurrentLobby.m_Id.m_sId : plString(sRoom);
  if (!m_bInLobby || m_CurrentLobby.m_Id.m_sId != sLobby)
    return plPlatformOperation::CreateFinished("Voice.Join", plPlatformError::NotFound);

  if (m_VoiceState != plPlatformVoiceState::Disconnected && m_sVoiceRoom == sLobby)
  {
    if (m_VoiceState == plPlatformVoiceState::Connected)
      return plPlatformOperation::CreateFinished("Voice.Join", plPlatformError::Success);

    if (m_pVoiceJoinOp != nullptr)
      return m_pVoiceJoinOp;
  }

  EOS_Lobby_GetRTCRoomNameOptions nameOptions = {};
  nameOptions.ApiVersion = EOS_LOBBY_GETRTCROOMNAME_API_LATEST;
  nameOptions.LobbyId = sLobby.GetData();
  nameOptions.LocalUserId = m_LocalProductUserId;

  char szRoomName[s_uiRtcRoomNameSize] = {};
  uint32_t uiLength = s_uiRtcRoomNameSize;
  const EOS_EResult nameResult = EOS_Lobby_GetRTCRoomName(m_hLobby, &nameOptions, szRoomName, &uiLength);
  if (nameResult != EOS_EResult::EOS_Success)
  {
    plLog::Warning("EOS: lobby '{}' has no RTC room ({}); enable LobbyVoice before creating lobbies.", sLobby, plEosResultName(nameResult));
    return plPlatformOperation::CreateFinished("Voice.Join", plPlatformError::Unsupported);
  }

  UnregisterVoiceNotifications();
  m_sVoiceRoom = sLobby;
  m_sVoiceRtcRoom = szRoomName;
  m_pPcmProvider = plSingletonRegistry::GetSingletonInstance<plAudioPcmStreamProvider>();
  RegisterVoiceNotifications();

  // Sending and receiving follow the cached flags; a previous LeaveRoom disabled both.
  SetCaptureEnabled(m_bCaptureEnabled);

  {
    EOS_RTCAudio_UpdateReceivingOptions options = {};
    options.ApiVersion = EOS_RTCAUDIO_UPDATERECEIVING_API_LATEST;
    options.LocalUserId = m_LocalProductUserId;
    options.RoomName = m_sVoiceRtcRoom.GetData();
    options.ParticipantId = nullptr;
    options.bAudioEnabled = EOS_TRUE;

    plEosCall* pCall = plEosNewCall(this, "Voice.Receive");
    EOS_RTCAudio_UpdateReceiving(m_hRTCAudio, &options, pCall, &OnUpdateReceiving);
  }

  for (const plString& sMuted : m_MutedUsers)
    SetUserMuted(plPlatformUserId::Make("eos", sMuted), true);

  EOS_Lobby_IsRTCRoomConnectedOptions connectedOptions = {};
  connectedOptions.ApiVersion = EOS_LOBBY_ISRTCROOMCONNECTED_API_LATEST;
  connectedOptions.LobbyId = sLobby.GetData();
  connectedOptions.LocalUserId = m_LocalProductUserId;

  EOS_Bool bConnected = EOS_FALSE;
  EOS_Lobby_IsRTCRoomConnected(m_hLobby, &connectedOptions, &bConnected);

  if (bConnected == EOS_TRUE)
  {
    SetVoiceState(plPlatformVoiceState::Connected);
    return plPlatformOperation::CreateFinished("Voice.Join", plPlatformError::Success);
  }

  m_pVoiceJoinOp = plPlatformOperation::Create("Voice.Join");
  SetVoiceState(plPlatformVoiceState::Connecting);

  return m_pVoiceJoinOp;
}

plPlatformOperationPtr plEosPlatformBackend::LeaveRoom()
{
  if (m_VoiceState == plPlatformVoiceState::Disconnected)
    return plPlatformOperation::CreateFinished("Voice.Leave", plPlatformError::Success);

  // The lobby owns the RTC room; leaving voice mutes both directions and drops the audio hooks.
  if (m_hRTCAudio != nullptr && !m_sVoiceRtcRoom.IsEmpty() && EOS_ProductUserId_IsValid(m_LocalProductUserId))
  {
    {
      EOS_RTCAudio_UpdateSendingOptions options = {};
      options.ApiVersion = EOS_RTCAUDIO_UPDATESENDING_API_LATEST;
      options.LocalUserId = m_LocalProductUserId;
      options.RoomName = m_sVoiceRtcRoom.GetData();
      options.AudioStatus = EOS_ERTCAudioStatus::EOS_RTCAS_Disabled;

      plEosCall* pCall = plEosNewCall(this, "Voice.Send");
      EOS_RTCAudio_UpdateSending(m_hRTCAudio, &options, pCall, &OnUpdateSending);
    }

    {
      EOS_RTCAudio_UpdateReceivingOptions options = {};
      options.ApiVersion = EOS_RTCAUDIO_UPDATERECEIVING_API_LATEST;
      options.LocalUserId = m_LocalProductUserId;
      options.RoomName = m_sVoiceRtcRoom.GetData();
      options.ParticipantId = nullptr;
      options.bAudioEnabled = EOS_FALSE;

      plEosCall* pCall = plEosNewCall(this, "Voice.Receive");
      EOS_RTCAudio_UpdateReceiving(m_hRTCAudio, &options, pCall, &OnUpdateReceiving);
    }
  }

  UnregisterVoiceNotifications();
  DestroyVoiceStream();
  SetVoiceState(plPlatformVoiceState::Disconnected);
  m_sVoiceRoom.Clear();
  m_sVoiceRtcRoom.Clear();

  return plPlatformOperation::CreateFinished("Voice.Leave", plPlatformError::Success);
}

void plEosPlatformBackend::SetCaptureEnabled(bool bEnabled)
{
  m_bCaptureEnabled = bEnabled;

  if (m_hRTCAudio == nullptr || m_sVoiceRtcRoom.IsEmpty() || !EOS_ProductUserId_IsValid(m_LocalProductUserId))
    return;

  EOS_RTCAudio_UpdateSendingOptions options = {};
  options.ApiVersion = EOS_RTCAUDIO_UPDATESENDING_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.RoomName = m_sVoiceRtcRoom.GetData();
  options.AudioStatus = bEnabled ? EOS_ERTCAudioStatus::EOS_RTCAS_Enabled : EOS_ERTCAudioStatus::EOS_RTCAS_Disabled;

  plEosCall* pCall = plEosNewCall(this, "Voice.Send");
  EOS_RTCAudio_UpdateSending(m_hRTCAudio, &options, pCall, &OnUpdateSending);
}

void plEosPlatformBackend::SetUserMuted(const plPlatformUserId& user, bool bMuted)
{
  const plString sNative(user.GetNativeId());

  if (bMuted)
  {
    if (!m_MutedUsers.Contains(sNative))
      m_MutedUsers.PushBack(sNative);
  }
  else
  {
    m_MutedUsers.RemoveAndCopy(sNative);
  }

  const EOS_ProductUserId participant = plEosProductUserIdFromUser(user);
  if (m_hRTCAudio == nullptr || m_sVoiceRtcRoom.IsEmpty() || !EOS_ProductUserId_IsValid(participant) || !EOS_ProductUserId_IsValid(m_LocalProductUserId))
    return;

  EOS_RTCAudio_UpdateReceivingOptions options = {};
  options.ApiVersion = EOS_RTCAUDIO_UPDATERECEIVING_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.RoomName = m_sVoiceRtcRoom.GetData();
  options.ParticipantId = participant;
  options.bAudioEnabled = bMuted ? EOS_FALSE : EOS_TRUE;

  plEosCall* pCall = plEosNewCall(this, "Voice.Receive");
  EOS_RTCAudio_UpdateReceiving(m_hRTCAudio, &options, pCall, &OnUpdateReceiving);
}

bool plEosPlatformBackend::IsUserMuted(const plPlatformUserId& user) const
{
  return m_MutedUsers.Contains(plString(user.GetNativeId()));
}

bool plEosPlatformBackend::IsUserSpeaking(const plPlatformUserId& user) const
{
  return m_SpeakingUsers.Contains(plString(user.GetNativeId()));
}

void plEosPlatformBackend::RegisterVoiceNotifications()
{
  if (m_hRTCAudio == nullptr || m_sVoiceRtcRoom.IsEmpty())
    return;

  {
    EOS_RTCAudio_AddNotifyParticipantUpdatedOptions options = {};
    options.ApiVersion = EOS_RTCAUDIO_ADDNOTIFYPARTICIPANTUPDATED_API_LATEST;
    options.LocalUserId = m_LocalProductUserId;
    options.RoomName = m_sVoiceRtcRoom.GetData();

    m_ParticipantUpdatedNotification = EOS_RTCAudio_AddNotifyParticipantUpdated(m_hRTCAudio, &options, this, &OnParticipantUpdated);
  }

  if (m_pPcmProvider != nullptr)
  {
    EOS_RTCAudio_AddNotifyAudioBeforeRenderOptions options = {};
    options.ApiVersion = EOS_RTCAUDIO_ADDNOTIFYAUDIOBEFORERENDER_API_LATEST;
    options.LocalUserId = m_LocalProductUserId;
    options.RoomName = m_sVoiceRtcRoom.GetData();
    options.bUnmixedAudio = EOS_FALSE;

    m_AudioBeforeRenderNotification = EOS_RTCAudio_AddNotifyAudioBeforeRender(m_hRTCAudio, &options, this, &OnAudioBeforeRender);
  }
}

void plEosPlatformBackend::UnregisterVoiceNotifications()
{
  if (m_hRTCAudio == nullptr)
    return;

  if (m_ParticipantUpdatedNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_RTCAudio_RemoveNotifyParticipantUpdated(m_hRTCAudio, m_ParticipantUpdatedNotification);
  if (m_AudioBeforeRenderNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_RTCAudio_RemoveNotifyAudioBeforeRender(m_hRTCAudio, m_AudioBeforeRenderNotification);

  m_ParticipantUpdatedNotification = EOS_INVALID_NOTIFICATIONID;
  m_AudioBeforeRenderNotification = EOS_INVALID_NOTIFICATIONID;
}

void plEosPlatformBackend::DestroyVoiceStream()
{
  PL_LOCK(m_VoiceStreamMutex);

  if (m_bVoiceStreamCreated && m_pPcmProvider != nullptr)
    m_pPcmProvider->DestroyPcmStream(m_uiVoiceStream);

  m_bVoiceStreamCreated = false;
  m_uiVoiceStream = 0;
}

void EOS_CALL plEosPlatformBackend::OnParticipantUpdated(const EOS_RTCAudio_ParticipantUpdatedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (!EOS_ProductUserId_IsValid(pData->ParticipantId))
    return;

  const plPlatformUserId user = plEosMakeUserId(pData->ParticipantId);
  const plString sNative(user.GetNativeId());
  const bool bSpeaking = pData->bSpeaking == EOS_TRUE;
  const bool bWasSpeaking = pThis->m_SpeakingUsers.Contains(sNative);

  if (bSpeaking == bWasSpeaking)
    return;

  if (bSpeaking)
    pThis->m_SpeakingUsers.PushBack(sNative);
  else
    pThis->m_SpeakingUsers.RemoveAndCopy(sNative);

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::VoiceUserSpeaking;
  e.m_User = user;
  e.m_bFlag = bSpeaking;
  pThis->Emit(e);
}

// Runs on the SDK's audio thread. The PCM provider is thread-safe by contract; the mutex only guards the stream
// handle against DestroyVoiceStream on the main thread.
void EOS_CALL plEosPlatformBackend::OnAudioBeforeRender(const EOS_RTCAudio_AudioBeforeRenderCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);
  const EOS_RTCAudio_AudioBuffer* pBuffer = pData->Buffer;

  if (pBuffer == nullptr || pBuffer->Frames == nullptr || pBuffer->FramesCount == 0 || pBuffer->Channels == 0)
    return;

  PL_LOCK(pThis->m_VoiceStreamMutex);

  if (pThis->m_pPcmProvider == nullptr)
    return;

  if (!pThis->m_bVoiceStreamCreated)
  {
    plAudioPcmStreamDesc desc;
    desc.m_uiSampleRate = pBuffer->SampleRate;
    desc.m_uiChannels = static_cast<plUInt8>(plMath::Min<uint32_t>(pBuffer->Channels, 8));
    desc.m_uiRingFrames = pBuffer->SampleRate; // One second of lead.

    if (pThis->m_pPcmProvider->CreatePcmStream(desc, pThis->m_uiVoiceStream).Failed())
      return;

    pThis->m_bVoiceStreamCreated = true;
  }

  const plUInt32 uiSamples = pBuffer->FramesCount * pBuffer->Channels;
  pThis->m_VoiceConvert.SetCountUninitialized(uiSamples);

  for (plUInt32 i = 0; i < uiSamples; ++i)
    pThis->m_VoiceConvert[i] = static_cast<float>(pBuffer->Frames[i]) * (1.0f / 32768.0f);

  pThis->m_pPcmProvider->WritePcmFrames(pThis->m_uiVoiceStream, pThis->m_VoiceConvert.GetArrayPtr());
}

void EOS_CALL plEosPlatformBackend::OnUpdateSending(const EOS_RTCAudio_UpdateSendingCallbackInfo* pData)
{
  plEosCompleteSimple(pData);
}

void EOS_CALL plEosPlatformBackend::OnUpdateReceiving(const EOS_RTCAudio_UpdateReceivingCallbackInfo* pData)
{
  plEosCompleteSimple(pData);
}
