#pragma once

#include <EosPlugin/EosIncludes.h>
#include <EosPlugin/EosInternal.h>
#include <EosPlugin/EosPluginDLL.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Threading/ThreadSignal.h>
#include <GameEngine/PlatformServices/PlatformBackend.h>
#include <GameEngine/SaveGame/SaveGameCloud.h>

class plAudioPcmStreamProvider;
struct plEosCloudRequest;

/// The "eos" backend: Epic account sign-in through EOS Auth + Connect, then every capability EOS offers on top of
/// the product user id. One instance lives for the plugin's lifetime and registers itself under plPlatformBackend;
/// the hub calls Initialize when PlatformServices.ddl names "eos".
///
/// Threading: every EOS call is issued on the main thread from inside Tick or from a provider method, which the
/// hub only calls on the main thread. The one exception is the cloud save provider, which the save system calls
/// from an I/O task; those calls post a request to the main thread and block until Tick completed it.
///
/// Backend settings (PlatformServices.ddl, block "eos"): ProductId, SandboxId, DeploymentId, ClientId,
/// EncryptionKey, LobbyBucket ("plasma"), P2PSocket ("plasma"), LobbyVoice (true), AppItemId, CatalogNamespace.
/// The client secret comes from the PLASMA_EOS_CLIENT_SECRET environment variable.
class PL_EOSPLUGIN_DLL plEosPlatformBackend final
  : public plPlatformBackend,
    public plPlatformIdentityProvider,
    public plPlatformAchievementsProvider,
    public plPlatformStatsProvider,
    public plPlatformLeaderboardsProvider,
    public plPlatformFriendsProvider,
    public plPlatformPresenceProvider,
    public plPlatformLobbiesProvider,
    public plPlatformMatchmakingProvider,
    public plPlatformSessionsProvider,
    public plPlatformEntitlementsProvider,
    public plPlatformOverlayProvider,
    public plPlatformTransport,
    public plPlatformVoice,
    public plSaveGameCloudProvider
{
  PL_DECLARE_SINGLETON_OF_INTERFACE(plEosPlatformBackend, plPlatformBackend);

public:
  plEosPlatformBackend();
  ~plEosPlatformBackend();

  /// \name plPlatformBackend
  ///@{
  plStringView GetId() const override { return "eos"; }
  plResult Initialize(plPlatformBackendHost& host) override;
  void Shutdown() override;
  void Tick() override;

  plBitflags<plPlatformCapability> GetCapabilities() const override;
  plPlatformOnlineState GetOnlineState() const override { return m_OnlineState; }

  plPlatformIdentityProvider& GetIdentity() override { return *this; }
  plPlatformAchievementsProvider* GetAchievements() override { return m_hAchievements ? this : nullptr; }
  plPlatformStatsProvider* GetStats() override { return m_hStats ? this : nullptr; }
  plPlatformLeaderboardsProvider* GetLeaderboards() override { return m_hLeaderboards ? this : nullptr; }
  plPlatformFriendsProvider* GetFriends() override { return m_hFriends ? this : nullptr; }
  plPlatformPresenceProvider* GetPresence() override { return m_hPresence ? this : nullptr; }
  plPlatformLobbiesProvider* GetLobbies() override { return m_hLobby ? this : nullptr; }
  plPlatformMatchmakingProvider* GetMatchmaking() override { return m_hLobby ? this : nullptr; }
  plPlatformSessionsProvider* GetSessions() override { return m_hLobby ? this : nullptr; }
  plPlatformEntitlementsProvider* GetEntitlements() override { return m_hEcom ? this : nullptr; }
  plPlatformOverlayProvider* GetOverlay() override { return m_hUI ? this : nullptr; }
  plPlatformTransport* GetTransport() override { return m_hP2P ? this : nullptr; }
  plPlatformVoice* GetVoice() override { return m_hRTC ? this : nullptr; }
  plSaveGameCloudProvider* GetCloudSaveProvider() override { return m_hPlayerDataStorage ? this : nullptr; }
  ///@}

  /// \name Identity (EosIdentity.cpp)
  ///@{
  bool IsSignedIn() const override { return m_bSignedIn; }
  const plPlatformUser& GetLocalUser() const override { return m_User; }
  plPlatformOperationPtr SignIn() override;
  plPlatformOperationPtr SignOut() override;
  plPlatformOperationPtr QueryUser(const plPlatformUserId& user) override;
  ///@}

  /// \name Achievements (EosAchievements.cpp)
  ///@{
  plPlatformOperationPtr Unlock(plStringView sBackendId) override;
  plPlatformOperationPtr QueryAchievements() override;
  ///@}

  /// \name Stats (EosStats.cpp)
  ///@{
  plPlatformOperationPtr Store(plArrayPtr<const plPlatformStatValue> values) override;
  plPlatformOperationPtr QueryStats() override;
  ///@}

  /// \name Leaderboards (EosLeaderboards.cpp)
  ///@{
  plPlatformOperationPtr UploadScore(plStringView sBackendId, plInt64 iScore, plPlatformLeaderboardUpload method, plArrayPtr<const plInt32> details) override;
  plPlatformOperationPtr Read(plStringView sBackendId, const plPlatformLeaderboardQuery& query) override;
  ///@}

  /// \name Friends (EosFriends.cpp)
  ///@{
  plPlatformOperationPtr QueryFriends() override;
  bool IsBlocked(const plPlatformUserId& user) const override;
  ///@}

  /// \name Presence (EosPresence.cpp)
  ///@{
  plResult Set(plStringView sKey, plStringView sValue) override;
  plResult SetJoinString(plStringView sConnect) override;
  void Clear() override;
  ///@}

  /// \name Lobbies (EosLobbies.cpp)
  ///@{
  plPlatformOperationPtr Create(const plPlatformLobbyDesc& desc) override;
  plPlatformOperationPtr Find(const plPlatformLobbySearch& search) override;
  plPlatformOperationPtr Join(const plPlatformLobbyId& lobby) override;
  plPlatformOperationPtr Leave(const plPlatformLobbyId& lobby) override;

  const plPlatformLobbyInfo* GetInfo(const plPlatformLobbyId& lobby) const override;

  plResult SetAttribute(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue) override;
  plResult SetMemberAttribute(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue) override;
  plResult SetOwner(const plPlatformLobbyId& lobby, const plPlatformUserId& user) override;
  plResult SetVisibility(const plPlatformLobbyId& lobby, plPlatformLobbyVisibility visibility) override;
  plResult SendChat(const plPlatformLobbyId& lobby, plStringView sText) override;
  ///@}

  /// \name Matchmaking (EosMatchmaking.cpp)
  ///@{
  plPlatformOperationPtr Start(const plPlatformMatchmakingRequest& request) override;
  plPlatformOperationPtr Cancel(plStringView sTicket) override;
  ///@}

  /// \name Sessions (EosSessions.cpp)
  ///@{
  plPlatformOperationPtr Invite(const plPlatformUserId& user, const plPlatformLobbyId& lobby) override;
  bool TakeLaunchJoinRequest(plPlatformJoinRequest& out_request) override;
  ///@}

  /// \name Entitlements (EosEntitlements.cpp)
  ///@{
  bool OwnsApp() const override;
  bool OwnsDlc(plStringView sBackendId) const override;
  plPlatformOperationPtr Refresh() override;
  ///@}

  /// \name Overlay (EosOverlay.cpp)
  ///@{
  bool IsVisible() const override { return m_bOverlayVisible; }
  plResult Show(plPlatformOverlayPage page) override;
  plResult ShowUser(const plPlatformUserId& user) override;
  plResult ShowInviteDialog(const plPlatformLobbyId& lobby) override;
  plResult ShowStore(plStringView sDlcBackendId) override;
  ///@}

  /// \name Transport (EosTransport.cpp)
  ///@{
  plResult Listen() override;
  void StopListening() override;
  plResult Connect(const plPlatformUserId& user) override;
  void Disconnect(const plPlatformUserId& user) override;
  void DisconnectAll() override;

  plResult Send(const plPlatformUserId& user, plArrayPtr<const plUInt8> data, bool bReliable, plUInt8 uiChannel) override;
  bool PollEvent(plPlatformTransportEvent& out_event) override;
  plUInt32 GetMaxPayloadBytes() const override;
  ///@}

  /// \name Voice (EosVoice.cpp)
  ///@{
  plPlatformOperationPtr JoinRoom(plStringView sRoom) override;
  plPlatformOperationPtr LeaveRoom() override;
  plPlatformVoiceState GetState() const override { return m_VoiceState; }

  void SetCaptureEnabled(bool bEnabled) override;
  bool IsCaptureEnabled() const override { return m_bCaptureEnabled; }

  void SetUserMuted(const plPlatformUserId& user, bool bMuted) override;
  bool IsUserMuted(const plPlatformUserId& user) const override;
  bool IsUserSpeaking(const plPlatformUserId& user) const override;
  ///@}

  /// \name Cloud saves (EosCloud.cpp). Callable from any thread; blocks until the main thread finished the request.
  ///@{
  plSaveGameError Read(const plSaveGameCloudKey& key, plSaveGameCloudRecord& out_record) override;
  plSaveGameError Publish(const plSaveGameCloudKey& key, const plSaveGameCloudRecord& record, const plUuid& expectedRemoteRevision) override;
  plSaveGameError Enumerate(plStringView project, const plUuid& profile, plDynamicArray<plSaveGameCloudKey>& out_keys) override;
  ///@}

  /// \name Shared by the capability files
  ///@{
  plPlatformBackendHost* GetHost() const { return m_pHost; }
  EOS_HPlatform GetPlatform() const { return m_hPlatform; }
  EOS_ProductUserId GetLocalProductUserId() const { return m_LocalProductUserId; }
  EOS_EpicAccountId GetLocalEpicAccountId() const { return m_LocalEpicAccountId; }

  void Emit(const plPlatformEvent& event);

  /// A string from the "eos" settings block, or the fallback.
  plString GetSetting(plStringView sKey, plStringView sFallback = {}) const;
  bool GetSettingBool(plStringView sKey, bool bFallback) const;

  /// A finished failure when nobody is signed in, otherwise nullptr.
  plPlatformOperationPtr GateSignedIn(plStringView sKind) const;

  /// Display name cached from mappings, lobby members and friends; the product user id text when unknown.
  plString GetCachedDisplayName(EOS_ProductUserId id) const;

  /// Learns display names for product user ids through the Connect mappings; emits FriendsChanged/LobbyUpdated
  /// afterwards so cached views refresh. Fire and forget.
  void ResolveDisplayNames(plArrayPtr<const EOS_ProductUserId> ids);
  ///@}

private:
  friend struct plEosCloudRequest;

  struct Friend
  {
    EOS_EpicAccountId m_EpicId = nullptr;
    EOS_ProductUserId m_ProductId = nullptr;
    plPlatformUser m_User;
  };

  struct Peer
  {
    EOS_ProductUserId m_Id = nullptr;
    bool m_bConnected = false;
  };

  struct LeaderboardDef
  {
    plString m_sStat;
    EOS_ELeaderboardAggregation m_Aggregation = EOS_ELeaderboardAggregation::EOS_LA_Max;
  };

  // ---- platform (EosPlatformBackend.cpp)
  void AcquireInterfaces();
  void SetOnlineState(plPlatformOnlineState state);
  void ProcessCloudQueue();

  // ---- identity (EosIdentity.cpp)
  void BeginAuthLogin(plEosCall* pCall, EOS_ELoginCredentialType type, const char* szId, const char* szToken);
  void BeginConnectLogin(plEosCall* pCall);
  void FinishSignIn(plEosCall* pCall);
  void CompleteSignIn(plEosCall* pCall);
  bool StopCancelledSignIn(plEosCall* pCall);
  void OnAuthLoggedOut();
  void RegisterUserNotifications();
  void UnregisterUserNotifications();

  static void EOS_CALL OnAuthLogin(const EOS_Auth_LoginCallbackInfo* pData);
  static void EOS_CALL OnConnectLogin(const EOS_Connect_LoginCallbackInfo* pData);
  static void EOS_CALL OnConnectCreateUser(const EOS_Connect_CreateUserCallbackInfo* pData);
  static void EOS_CALL OnLocalUserInfo(const EOS_UserInfo_QueryUserInfoCallbackInfo* pData);
  static void EOS_CALL OnAuthLogout(const EOS_Auth_LogoutCallbackInfo* pData);
  static void EOS_CALL OnDeletePersistentAuth(const EOS_Auth_DeletePersistentAuthCallbackInfo* pData);
  static void EOS_CALL OnAuthLoginStatusChanged(const EOS_Auth_LoginStatusChangedCallbackInfo* pData);
  static void EOS_CALL OnConnectLoginStatusChanged(const EOS_Connect_LoginStatusChangedCallbackInfo* pData);
  static void EOS_CALL OnConnectAuthExpiration(const EOS_Connect_AuthExpirationCallbackInfo* pData);
  static void EOS_CALL OnConnectRefresh(const EOS_Connect_LoginCallbackInfo* pData);
  static void EOS_CALL OnQueryUserMappings(const EOS_Connect_QueryProductUserIdMappingsCallbackInfo* pData);
  static void EOS_CALL OnResolveNames(const EOS_Connect_QueryProductUserIdMappingsCallbackInfo* pData);

  // ---- achievements (EosAchievements.cpp)
  static void EOS_CALL OnUnlockAchievements(const EOS_Achievements_OnUnlockAchievementsCompleteCallbackInfo* pData);
  static void EOS_CALL OnQueryPlayerAchievements(const EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo* pData);

  // ---- stats (EosStats.cpp)
  static void EOS_CALL OnIngestStat(const EOS_Stats_IngestStatCompleteCallbackInfo* pData);
  static void EOS_CALL OnQueryStats(const EOS_Stats_OnQueryStatsCompleteCallbackInfo* pData);

  void ReadStatsIntoCache();

  // ---- leaderboards (EosLeaderboards.cpp)
  void IngestLeaderboardScore(plEosCall* pCall, const LeaderboardDef& def);
  void BeginLeaderboardRead(plEosCall* pCall, const plPlatformLeaderboardQuery& query);

  static void EOS_CALL OnLeaderboardDefinitionsForUpload(const EOS_Leaderboards_OnQueryLeaderboardDefinitionsCompleteCallbackInfo* pData);
  static void EOS_CALL OnLeaderboardDefinitionsForRead(const EOS_Leaderboards_OnQueryLeaderboardDefinitionsCompleteCallbackInfo* pData);
  static void EOS_CALL OnLeaderboardRanks(const EOS_Leaderboards_OnQueryLeaderboardRanksCompleteCallbackInfo* pData);
  static void EOS_CALL OnLeaderboardUserScores(const EOS_Leaderboards_OnQueryLeaderboardUserScoresCompleteCallbackInfo* pData);

  bool CacheLeaderboardDefinition(plStringView sId);

  // ---- friends (EosFriends.cpp)
  static void EOS_CALL OnQueryFriends(const EOS_Friends_QueryFriendsCallbackInfo* pData);
  static void EOS_CALL OnFriendMappings(const EOS_Connect_QueryExternalAccountMappingsCallbackInfo* pData);
  static void EOS_CALL OnFriendUserInfo(const EOS_UserInfo_QueryUserInfoCallbackInfo* pData);
  static void EOS_CALL OnFriendPresence(const EOS_Presence_QueryPresenceCallbackInfo* pData);
  static void EOS_CALL OnFriendsUpdate(const EOS_Friends_OnFriendsUpdateInfo* pData);
  static void EOS_CALL OnPresenceChanged(const EOS_Presence_PresenceChangedCallbackInfo* pData);

  void FinishFriendsQuery(plEosCall* pCall);
  void ReadFriendPresence(Friend& ref_friend);
  Friend* FindFriendByEpicId(EOS_EpicAccountId id);

  // ---- presence (EosPresence.cpp)
  plResult PublishPresence();

  static void EOS_CALL OnSetPresence(const EOS_Presence_SetPresenceCallbackInfo* pData);
  static void EOS_CALL OnJoinGameAccepted(const EOS_Presence_JoinGameAcceptedCallbackInfo* pData);

  // ---- lobbies (EosLobbies.cpp)
  void RegisterLobbyNotifications();
  void UnregisterLobbyNotifications();
  bool RefreshLobbyInfo(const char* szLobbyId);
  void ReadLobbyDetails(EOS_HLobbyDetails hDetails, plPlatformLobbyInfo& out_info, bool bMembers);
  void OnLeftLobby(const char* szLobbyId);
  void ReleaseJoinDetails();
  void CheckLobbyChat(EOS_ProductUserId member);
  void FillLocalRtcOptions(EOS_Lobby_LocalRTCOptions& out_options) const;
  void EmitLobbyEvent(plPlatformEvent::Type type, const plPlatformUserId& user = {});
  plUInt32 CountLobbyMembers(EOS_HLobbyDetails hDetails) const;
  plResult ModifyLobby(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue, bool bMember);

  static void EOS_CALL OnCreateLobby(const EOS_Lobby_CreateLobbyCallbackInfo* pData);
  static void EOS_CALL OnUpdateLobby(const EOS_Lobby_UpdateLobbyCallbackInfo* pData);
  static void EOS_CALL OnLobbySearchFind(const EOS_LobbySearch_FindCallbackInfo* pData);
  static void EOS_CALL OnJoinLobby(const EOS_Lobby_JoinLobbyByIdCallbackInfo* pData);
  static void EOS_CALL OnJoinLobbyDetails(const EOS_Lobby_JoinLobbyCallbackInfo* pData);
  static void EOS_CALL OnLeaveLobby(const EOS_Lobby_LeaveLobbyCallbackInfo* pData);
  static void EOS_CALL OnPromoteMember(const EOS_Lobby_PromoteMemberCallbackInfo* pData);
  static void EOS_CALL OnLobbyUpdateReceived(const EOS_Lobby_LobbyUpdateReceivedCallbackInfo* pData);
  static void EOS_CALL OnLobbyMemberUpdateReceived(const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo* pData);
  static void EOS_CALL OnLobbyMemberStatusReceived(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* pData);
  static void EOS_CALL OnLobbyInviteReceived(const EOS_Lobby_LobbyInviteReceivedCallbackInfo* pData);
  static void EOS_CALL OnLobbyInviteAccepted(const EOS_Lobby_LobbyInviteAcceptedCallbackInfo* pData);
  static void EOS_CALL OnJoinLobbyAccepted(const EOS_Lobby_JoinLobbyAcceptedCallbackInfo* pData);
  static void EOS_CALL OnRtcRoomConnectionChanged(const EOS_Lobby_RTCRoomConnectionChangedCallbackInfo* pData);

  // ---- matchmaking (EosMatchmaking.cpp)
  void UpdateMatchmaking();
  void SetMatchmakingState(plPlatformMatchmakingState state);
  void MatchmakingSearch();
  void MatchmakingCreate();

  static void EOS_CALL OnMatchmakingSearch(const EOS_LobbySearch_FindCallbackInfo* pData);
  static void EOS_CALL OnMatchmakingJoin(const EOS_Lobby_JoinLobbyCallbackInfo* pData);
  static void EOS_CALL OnMatchmakingCreate(const EOS_Lobby_CreateLobbyCallbackInfo* pData);
  static void EOS_CALL OnMatchmakingUpdate(const EOS_Lobby_UpdateLobbyCallbackInfo* pData);
  static void EOS_CALL OnMatchmakingLeave(const EOS_Lobby_LeaveLobbyCallbackInfo* pData);

  // ---- sessions (EosSessions.cpp)
  void ReadLaunchArguments();

  static void EOS_CALL OnSendInvite(const EOS_Lobby_SendInviteCallbackInfo* pData);

  // ---- entitlements (EosEntitlements.cpp)
  static void EOS_CALL OnQueryOwnership(const EOS_Ecom_QueryOwnershipCallbackInfo* pData);

  // ---- overlay (EosOverlay.cpp)
  static void EOS_CALL OnShowFriends(const EOS_UI_ShowFriendsCallbackInfo* pData);
  static void EOS_CALL OnDisplaySettingsUpdated(const EOS_UI_OnDisplaySettingsUpdatedCallbackInfo* pData);

  // ---- transport (EosTransport.cpp)
  void RegisterTransportNotifications();
  void UnregisterTransportNotifications();
  void ReceivePackets();
  Peer* FindPeer(EOS_ProductUserId id);
  void FillSocketId(EOS_P2P_SocketId& out_socket) const;

  static void EOS_CALL OnPeerConnectionRequest(const EOS_P2P_OnIncomingConnectionRequestInfo* pData);
  static void EOS_CALL OnPeerConnectionEstablished(const EOS_P2P_OnPeerConnectionEstablishedInfo* pData);
  static void EOS_CALL OnPeerConnectionClosed(const EOS_P2P_OnRemoteConnectionClosedInfo* pData);

  // ---- voice (EosVoice.cpp)
  void RegisterVoiceNotifications();
  void UnregisterVoiceNotifications();
  void SetVoiceState(plPlatformVoiceState state);
  void DestroyVoiceStream();

  static void EOS_CALL OnParticipantUpdated(const EOS_RTCAudio_ParticipantUpdatedCallbackInfo* pData);
  static void EOS_CALL OnAudioBeforeRender(const EOS_RTCAudio_AudioBeforeRenderCallbackInfo* pData);
  static void EOS_CALL OnUpdateSending(const EOS_RTCAudio_UpdateSendingCallbackInfo* pData);
  static void EOS_CALL OnUpdateReceiving(const EOS_RTCAudio_UpdateReceivingCallbackInfo* pData);

  // ---- cloud (EosCloud.cpp)
  plSaveGameError RunCloudRequest(plEosCloudRequest* pRequest);
  void StartCloudRequest(plEosCloudRequest& ref_request);
  void FinishCloudRequest(plEosCloudRequest& ref_request, plSaveGameError error);
  void BeginCloudRead(plEosCloudRequest& ref_request);
  void BeginCloudWrite(plEosCloudRequest& ref_request);

  static EOS_PlayerDataStorage_EReadResult EOS_CALL OnCloudReadData(const EOS_PlayerDataStorage_ReadFileDataCallbackInfo* pData);
  static EOS_PlayerDataStorage_EWriteResult EOS_CALL OnCloudWriteData(const EOS_PlayerDataStorage_WriteFileDataCallbackInfo* pData, void* pOutDataBuffer, uint32_t* pOutDataWritten);
  static void EOS_CALL OnCloudReadComplete(const EOS_PlayerDataStorage_ReadFileCallbackInfo* pData);
  static void EOS_CALL OnCloudWriteComplete(const EOS_PlayerDataStorage_WriteFileCallbackInfo* pData);
  static void EOS_CALL OnCloudFileList(const EOS_PlayerDataStorage_QueryFileListCallbackInfo* pData);

  // platform
  plPlatformBackendHost* m_pHost = nullptr;
  EOS_HPlatform m_hPlatform = nullptr;
  EOS_HAuth m_hAuth = nullptr;
  EOS_HConnect m_hConnect = nullptr;
  EOS_HUserInfo m_hUserInfo = nullptr;
  EOS_HAchievements m_hAchievements = nullptr;
  EOS_HStats m_hStats = nullptr;
  EOS_HLeaderboards m_hLeaderboards = nullptr;
  EOS_HFriends m_hFriends = nullptr;
  EOS_HPresence m_hPresence = nullptr;
  EOS_HLobby m_hLobby = nullptr;
  EOS_HEcom m_hEcom = nullptr;
  EOS_HUI m_hUI = nullptr;
  EOS_HP2P m_hP2P = nullptr;
  EOS_HPlayerDataStorage m_hPlayerDataStorage = nullptr;
  EOS_HRTC m_hRTC = nullptr;
  EOS_HRTCAudio m_hRTCAudio = nullptr;
  bool m_bInitialized = false;

  // identity
  plPlatformUser m_User;
  bool m_bSignedIn = false;
  plPlatformOnlineState m_OnlineState = plPlatformOnlineState::Offline;
  EOS_EpicAccountId m_LocalEpicAccountId = nullptr;
  EOS_ProductUserId m_LocalProductUserId = nullptr;
  plPlatformOperationPtr m_pSignInOp;
  plString m_sProductId;
  EOS_NotificationId m_AuthStatusNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_ConnectStatusNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_AuthExpirationNotification = EOS_INVALID_NOTIFICATIONID;
  plMap<plString, plString> m_DisplayNames; ///< Product user id text -> display name.

  // stats: last value the server reported, so Sum stats ingest a delta and not the absolute value again.
  plMap<plString, plInt32> m_ServerStats;

  // leaderboards
  plMap<plString, LeaderboardDef> m_LeaderboardDefs;

  // friends
  plDynamicArray<Friend> m_Friends;
  plDynamicArray<plString> m_BlockedUsers; ///< Product user id text.
  EOS_NotificationId m_FriendsUpdateNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_PresenceChangedNotification = EOS_INVALID_NOTIFICATIONID;
  plPlatformOperationPtr m_pFriendsOp;

  // presence
  plMap<plString, plString> m_PresenceData;
  plDynamicArray<plString> m_PresenceRemovedKeys; ///< Keys to delete with the next publish.
  plString m_sJoinString;
  bool m_bPresenceDirty = false;
  bool m_bPresenceInFlight = false;
  EOS_NotificationId m_JoinGameAcceptedNotification = EOS_INVALID_NOTIFICATIONID;

  // lobbies
  plPlatformLobbyInfo m_CurrentLobby;
  bool m_bInLobby = false;
  /// Details handles from invites and overlay joins, keyed by lobby id; Join prefers them because invite-only
  /// lobbies cannot be joined by id alone.
  plMap<plString, EOS_HLobbyDetails> m_JoinDetails;
  bool m_bMatchmakingLobby = false; ///< The current lobby was joined or created by the matcher.
  plUInt32 m_uiChatSequence = 0;
  plMap<plString, plUInt32> m_SeenChatSequence; ///< Member id text -> last chat sequence read.
  EOS_NotificationId m_LobbyUpdateNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_LobbyMemberUpdateNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_LobbyMemberStatusNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_LobbyInviteReceivedNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_LobbyInviteAcceptedNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_JoinLobbyAcceptedNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_RtcRoomConnectionNotification = EOS_INVALID_NOTIFICATIONID;

  // matchmaking
  plPlatformMatchmakingTicket m_Ticket;
  plPlatformMatchmakingRequest m_TicketRequest;
  plPlatformOperationPtr m_pTicketOp;
  plTime m_TicketStart;
  plTime m_TicketNextSearch;
  bool m_bTicketBusy = false; ///< A search, join or create is in flight.
  plUInt32 m_uiTicketCounter = 0;

  // sessions
  plPlatformJoinRequest m_LaunchJoin;
  bool m_bLaunchJoin = false;

  // entitlements
  bool m_bOwnsApp = false;
  bool m_bOwnershipKnown = false;
  plDynamicArray<plString> m_OwnedItems;

  // overlay
  bool m_bOverlayVisible = false;
  EOS_NotificationId m_DisplaySettingsNotification = EOS_INVALID_NOTIFICATIONID;

  // transport
  plString m_sSocketName;
  bool m_bListening = false;
  plDynamicArray<Peer> m_Peers;
  plDeque<plPlatformTransportEvent> m_TransportEvents;
  EOS_NotificationId m_PeerRequestNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_PeerEstablishedNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_PeerClosedNotification = EOS_INVALID_NOTIFICATIONID;

  // voice
  plPlatformVoiceState m_VoiceState = plPlatformVoiceState::Disconnected;
  plString m_sVoiceRoom;    ///< The lobby id the voice room belongs to.
  plString m_sVoiceRtcRoom; ///< The RTC room name the SDK derived for that lobby.
  bool m_bCaptureEnabled = true;
  plDynamicArray<plString> m_MutedUsers;
  plDynamicArray<plString> m_SpeakingUsers;
  plPlatformOperationPtr m_pVoiceJoinOp;
  EOS_NotificationId m_ParticipantUpdatedNotification = EOS_INVALID_NOTIFICATIONID;
  EOS_NotificationId m_AudioBeforeRenderNotification = EOS_INVALID_NOTIFICATIONID;
  plAudioPcmStreamProvider* m_pPcmProvider = nullptr;
  plUInt32 m_uiVoiceStream = 0;
  bool m_bVoiceStreamCreated = false;
  plMutex m_VoiceStreamMutex;
  plDynamicArray<float> m_VoiceConvert; ///< Scratch for int16 -> float, audio thread only.

  // cloud
  plMutex m_CloudMutex;
  plDeque<plEosCloudRequest*> m_CloudQueue;
  plEosCloudRequest* m_pActiveCloud = nullptr;
};
