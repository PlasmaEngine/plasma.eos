#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>

// Datagrams over EOS P2P on one named socket ("P2PSocket" setting, "plasma" by default). Connections are
// implicit: Connect() requests one through AcceptConnection, Send() queues until it is established, and incoming
// requests are accepted while listening or when the peer was asked for by the local side. Packets are drained
// once per Tick into the event queue the networking package polls.

void plEosPlatformBackend::FillSocketId(EOS_P2P_SocketId& out_socket) const
{
  out_socket = {};
  out_socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
  plStringUtils::Copy(out_socket.SocketName, EOS_P2P_SOCKETID_SOCKETNAME_SIZE, m_sSocketName.GetData());
}

plEosPlatformBackend::Peer* plEosPlatformBackend::FindPeer(EOS_ProductUserId id)
{
  for (Peer& peer : m_Peers)
  {
    if (peer.m_Id == id)
      return &peer;
  }

  return nullptr;
}

plUInt32 plEosPlatformBackend::GetMaxPayloadBytes() const
{
  return EOS_P2P_MAX_PACKET_SIZE;
}

plResult plEosPlatformBackend::Listen()
{
  if (m_hP2P == nullptr || !m_bSignedIn)
    return PL_FAILURE;

  m_bListening = true;
  RegisterTransportNotifications();

  return PL_SUCCESS;
}

void plEosPlatformBackend::StopListening()
{
  m_bListening = false;
}

plResult plEosPlatformBackend::Connect(const plPlatformUserId& user)
{
  if (m_hP2P == nullptr || !m_bSignedIn)
    return PL_FAILURE;

  const EOS_ProductUserId remoteId = plEosProductUserIdFromUser(user);
  if (!EOS_ProductUserId_IsValid(remoteId))
    return PL_FAILURE;

  if (FindPeer(remoteId) == nullptr)
  {
    Peer& peer = m_Peers.ExpandAndGetRef();
    peer.m_Id = remoteId;
  }

  RegisterTransportNotifications();

  EOS_P2P_SocketId socket;
  FillSocketId(socket);

  // AcceptConnection both accepts an incoming request and opens an outgoing one.
  EOS_P2P_AcceptConnectionOptions options = {};
  options.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.RemoteUserId = remoteId;
  options.SocketId = &socket;

  const EOS_EResult result = EOS_P2P_AcceptConnection(m_hP2P, &options);
  if (result != EOS_EResult::EOS_Success)
  {
    plLog::Warning("EOS: opening a P2P connection failed: {}", plEosResultName(result));
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

void plEosPlatformBackend::Disconnect(const plPlatformUserId& user)
{
  const EOS_ProductUserId remoteId = plEosProductUserIdFromUser(user);
  if (m_hP2P == nullptr || !EOS_ProductUserId_IsValid(remoteId))
    return;

  EOS_P2P_SocketId socket;
  FillSocketId(socket);

  EOS_P2P_CloseConnectionOptions options = {};
  options.ApiVersion = EOS_P2P_CLOSECONNECTION_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.RemoteUserId = remoteId;
  options.SocketId = &socket;

  EOS_P2P_CloseConnection(m_hP2P, &options);

  for (plUInt32 i = 0; i < m_Peers.GetCount(); ++i)
  {
    if (m_Peers[i].m_Id != remoteId)
      continue;

    if (m_Peers[i].m_bConnected)
    {
      plPlatformTransportEvent& e = m_TransportEvents.ExpandAndGetRef();
      e.m_Type = plPlatformTransportEvent::Type::Disconnected;
      e.m_User = user;
    }

    m_Peers.RemoveAtAndSwap(i);
    break;
  }
}

void plEosPlatformBackend::DisconnectAll()
{
  if (m_hP2P == nullptr || !EOS_ProductUserId_IsValid(m_LocalProductUserId))
  {
    m_Peers.Clear();
    return;
  }

  EOS_P2P_SocketId socket;
  FillSocketId(socket);

  EOS_P2P_CloseConnectionsOptions options = {};
  options.ApiVersion = EOS_P2P_CLOSECONNECTIONS_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.SocketId = &socket;

  EOS_P2P_CloseConnections(m_hP2P, &options);

  for (const Peer& peer : m_Peers)
  {
    if (!peer.m_bConnected)
      continue;

    plPlatformTransportEvent& e = m_TransportEvents.ExpandAndGetRef();
    e.m_Type = plPlatformTransportEvent::Type::Disconnected;
    e.m_User = plEosMakeUserId(peer.m_Id);
  }

  m_Peers.Clear();
}

plResult plEosPlatformBackend::Send(const plPlatformUserId& user, plArrayPtr<const plUInt8> data, bool bReliable, plUInt8 uiChannel)
{
  if (m_hP2P == nullptr || !m_bSignedIn)
    return PL_FAILURE;

  if (data.GetCount() > EOS_P2P_MAX_PACKET_SIZE)
    return PL_FAILURE;

  const EOS_ProductUserId remoteId = plEosProductUserIdFromUser(user);
  if (!EOS_ProductUserId_IsValid(remoteId))
    return PL_FAILURE;

  EOS_P2P_SocketId socket;
  FillSocketId(socket);

  const EOS_EPacketReliability reliability = bReliable ? EOS_EPacketReliability::EOS_PR_ReliableOrdered : EOS_EPacketReliability::EOS_PR_UnreliableUnordered;

  EOS_P2P_SendPacketOptions options = {};
  options.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.RemoteUserId = remoteId;
  options.SocketId = &socket;
  options.Channel = uiChannel;
  options.DataLengthBytes = data.GetCount();
  options.Data = data.GetPtr();
  options.bAllowDelayedDelivery = EOS_TRUE;
  options.Reliability = reliability;
  options.bDisableAutoAcceptConnection = EOS_FALSE;

  const EOS_EResult result = EOS_P2P_SendPacket(m_hP2P, &options);

  return result == EOS_EResult::EOS_Success ? PL_SUCCESS : PL_FAILURE;
}

bool plEosPlatformBackend::PollEvent(plPlatformTransportEvent& out_event)
{
  if (m_TransportEvents.IsEmpty())
    return false;

  out_event = std::move(m_TransportEvents.PeekFront());
  m_TransportEvents.PopFront();

  return true;
}

void plEosPlatformBackend::ReceivePackets()
{
  if (m_hP2P == nullptr || !EOS_ProductUserId_IsValid(m_LocalProductUserId))
    return;

  plUInt8 buffer[EOS_P2P_MAX_PACKET_SIZE];

  // Bounded so a peer flooding the socket cannot stall the frame.
  for (plUInt32 uiGuard = 0; uiGuard < 4096; ++uiGuard)
  {
    EOS_P2P_GetNextReceivedPacketSizeOptions sizeOptions = {};
    sizeOptions.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
    sizeOptions.LocalUserId = m_LocalProductUserId;
    sizeOptions.RequestedChannel = nullptr;

    uint32_t uiSize = 0;
    if (EOS_P2P_GetNextReceivedPacketSize(m_hP2P, &sizeOptions, &uiSize) != EOS_EResult::EOS_Success)
      break;

    EOS_P2P_ReceivePacketOptions options = {};
    options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
    options.LocalUserId = m_LocalProductUserId;
    options.MaxDataSizeBytes = EOS_P2P_MAX_PACKET_SIZE;
    options.RequestedChannel = nullptr;

    EOS_ProductUserId peerId = nullptr;
    EOS_P2P_SocketId socket = {};
    uint8_t uiChannel = 0;
    uint32_t uiWritten = 0;

    if (EOS_P2P_ReceivePacket(m_hP2P, &options, &peerId, &socket, &uiChannel, buffer, &uiWritten) != EOS_EResult::EOS_Success)
      break;

    if (m_sSocketName != socket.SocketName)
      continue;

    plPlatformTransportEvent& e = m_TransportEvents.ExpandAndGetRef();
    e.m_Type = plPlatformTransportEvent::Type::Data;
    e.m_User = plEosMakeUserId(peerId);
    e.m_uiChannel = uiChannel;
    e.m_Data.SetCountUninitialized(uiWritten);

    if (uiWritten > 0)
      plMemoryUtils::Copy(e.m_Data.GetData(), buffer, uiWritten);
  }
}

void plEosPlatformBackend::RegisterTransportNotifications()
{
  if (m_hP2P == nullptr || !EOS_ProductUserId_IsValid(m_LocalProductUserId) || m_PeerRequestNotification != EOS_INVALID_NOTIFICATIONID)
    return;

  EOS_P2P_SocketId socket;
  FillSocketId(socket);

  {
    EOS_P2P_AddNotifyPeerConnectionRequestOptions options = {};
    options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
    options.LocalUserId = m_LocalProductUserId;
    options.SocketId = &socket;

    m_PeerRequestNotification = EOS_P2P_AddNotifyPeerConnectionRequest(m_hP2P, &options, this, &OnPeerConnectionRequest);
  }

  {
    EOS_P2P_AddNotifyPeerConnectionEstablishedOptions options = {};
    options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
    options.LocalUserId = m_LocalProductUserId;
    options.SocketId = &socket;

    m_PeerEstablishedNotification = EOS_P2P_AddNotifyPeerConnectionEstablished(m_hP2P, &options, this, &OnPeerConnectionEstablished);
  }

  {
    EOS_P2P_AddNotifyPeerConnectionClosedOptions options = {};
    options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
    options.LocalUserId = m_LocalProductUserId;
    options.SocketId = &socket;

    m_PeerClosedNotification = EOS_P2P_AddNotifyPeerConnectionClosed(m_hP2P, &options, this, &OnPeerConnectionClosed);
  }
}

void plEosPlatformBackend::UnregisterTransportNotifications()
{
  if (m_hP2P == nullptr)
    return;

  if (m_PeerRequestNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_P2P_RemoveNotifyPeerConnectionRequest(m_hP2P, m_PeerRequestNotification);

  if (m_PeerEstablishedNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_P2P_RemoveNotifyPeerConnectionEstablished(m_hP2P, m_PeerEstablishedNotification);

  if (m_PeerClosedNotification != EOS_INVALID_NOTIFICATIONID)
    EOS_P2P_RemoveNotifyPeerConnectionClosed(m_hP2P, m_PeerClosedNotification);

  m_PeerRequestNotification = EOS_INVALID_NOTIFICATIONID;
  m_PeerEstablishedNotification = EOS_INVALID_NOTIFICATIONID;
  m_PeerClosedNotification = EOS_INVALID_NOTIFICATIONID;
  m_bListening = false;
}

void EOS_CALL plEosPlatformBackend::OnPeerConnectionRequest(const EOS_P2P_OnIncomingConnectionRequestInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (pData->SocketId == nullptr || pThis->m_sSocketName != pData->SocketId->SocketName)
    return;

  const bool bKnown = pThis->FindPeer(pData->RemoteUserId) != nullptr;
  if (!pThis->m_bListening && !bKnown)
    return;

  if (!bKnown)
  {
    Peer& peer = pThis->m_Peers.ExpandAndGetRef();
    peer.m_Id = pData->RemoteUserId;
  }

  EOS_P2P_AcceptConnectionOptions options = {};
  options.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
  options.LocalUserId = pThis->m_LocalProductUserId;
  options.RemoteUserId = pData->RemoteUserId;
  options.SocketId = pData->SocketId;

  EOS_P2P_AcceptConnection(pThis->m_hP2P, &options);
}

void EOS_CALL plEosPlatformBackend::OnPeerConnectionEstablished(const EOS_P2P_OnPeerConnectionEstablishedInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (pData->SocketId == nullptr || pThis->m_sSocketName != pData->SocketId->SocketName)
    return;

  Peer* pPeer = pThis->FindPeer(pData->RemoteUserId);
  if (pPeer == nullptr)
  {
    pPeer = &pThis->m_Peers.ExpandAndGetRef();
    pPeer->m_Id = pData->RemoteUserId;
  }

  if (pPeer->m_bConnected)
    return;

  pPeer->m_bConnected = true;

  plPlatformTransportEvent& transportEvent = pThis->m_TransportEvents.ExpandAndGetRef();
  transportEvent.m_Type = plPlatformTransportEvent::Type::Connected;
  transportEvent.m_User = plEosMakeUserId(pData->RemoteUserId);

  plPlatformEvent platformEvent;
  platformEvent.m_Type = plPlatformEvent::Type::TransportConnection;
  platformEvent.m_User = transportEvent.m_User;
  platformEvent.m_bFlag = true;
  pThis->Emit(platformEvent);
}

void EOS_CALL plEosPlatformBackend::OnPeerConnectionClosed(const EOS_P2P_OnRemoteConnectionClosedInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  if (pData->SocketId == nullptr || pThis->m_sSocketName != pData->SocketId->SocketName)
    return;

  const plPlatformUserId user = plEosMakeUserId(pData->RemoteUserId);
  bool bWasConnected = false;

  for (plUInt32 i = 0; i < pThis->m_Peers.GetCount(); ++i)
  {
    if (pThis->m_Peers[i].m_Id != pData->RemoteUserId)
      continue;

    bWasConnected = pThis->m_Peers[i].m_bConnected;
    pThis->m_Peers.RemoveAtAndSwap(i);
    break;
  }

  plPlatformTransportEvent& transportEvent = pThis->m_TransportEvents.ExpandAndGetRef();
  transportEvent.m_Type = bWasConnected ? plPlatformTransportEvent::Type::Disconnected : plPlatformTransportEvent::Type::ConnectFailed;
  transportEvent.m_User = user;

  if (bWasConnected)
  {
    plPlatformEvent platformEvent;
    platformEvent.m_Type = plPlatformEvent::Type::TransportConnection;
    platformEvent.m_User = user;
    platformEvent.m_bFlag = false;
    pThis->Emit(platformEvent);
  }
}
