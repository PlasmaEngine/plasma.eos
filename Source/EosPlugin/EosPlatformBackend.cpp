#include <EosPlugin/EosPluginPCH.h>

#include <AudioSystem/Core/AudioMiddleware.h>
#include <EosPlugin/EosPlatformBackend.h>
#include <Foundation/Application/Application.h>
#include <Foundation/Application/SdkVersion.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Utilities/CommandLineUtils.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameEngine/PlatformServices/PlatformServicesConfig.h>

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
#  include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#  include <Windows/eos_Windows.h>
#endif

PL_IMPLEMENT_SINGLETON(plEosPlatformBackend);

// ---- helpers shared by the capability files ----------------------------------------------------------------

const char* plEosResultName(EOS_EResult result)
{
  return EOS_EResult_ToString(result);
}

plPlatformError plEosResultToError(EOS_EResult result)
{
  switch (result)
  {
    case EOS_EResult::EOS_Success:
      return plPlatformError::Success;
    case EOS_EResult::EOS_NotFound:
      return plPlatformError::NotFound;
    case EOS_EResult::EOS_TimedOut:
      return plPlatformError::Timeout;
    case EOS_EResult::EOS_TooManyRequests:
      return plPlatformError::RateLimited;
    case EOS_EResult::EOS_NoConnection:
      return plPlatformError::Offline;
    case EOS_EResult::EOS_Canceled:
      return plPlatformError::Cancelled;
    case EOS_EResult::EOS_AccessDenied:
    case EOS_EResult::EOS_MissingPermissions:
      return plPlatformError::PermissionDenied;
    case EOS_EResult::EOS_InvalidParameters:
    case EOS_EResult::EOS_InvalidRequest:
      return plPlatformError::InvalidArgument;
    case EOS_EResult::EOS_InvalidAuth:
    case EOS_EResult::EOS_InvalidCredentials:
    case EOS_EResult::EOS_InvalidUser:
      return plPlatformError::NotSignedIn;
    case EOS_EResult::EOS_AlreadyPending:
      return plPlatformError::Busy;
    case EOS_EResult::EOS_NotImplemented:
      return plPlatformError::Unsupported;
    default:
      break;
  }

  // The lobby and permission families are matched by name: they are many, and what the caller needs to know is the
  // class of failure, not the member.
  const plStringView sName = EOS_EResult_ToString(result);

  if (sName.StartsWith("EOS_Permission_"))
    return plPlatformError::PermissionDenied;

  if (sName.FindSubString("TooManyPlayers") != nullptr || sName.FindSubString("Full") != nullptr || sName.FindSubString("AtCapacity") != nullptr)
    return plPlatformError::LobbyFull;

  if (sName.FindSubString("AlreadyInLobby") != nullptr || sName.FindSubString("PresenceLobbyExists") != nullptr || sName.FindSubString("AlreadyInSession") != nullptr)
    return plPlatformError::AlreadyInLobby;

  if (sName.FindSubString("Duplicate") != nullptr || sName.FindSubString("AlreadyExists") != nullptr)
    return plPlatformError::Conflict;

  return plPlatformError::BackendFailure;
}

plString plEosProductUserIdToString(EOS_ProductUserId id)
{
  if (!EOS_ProductUserId_IsValid(id))
    return plString();

  char szBuffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
  int32_t iLength = EOS_PRODUCTUSERID_MAX_LENGTH + 1;

  if (EOS_ProductUserId_ToString(id, szBuffer, &iLength) != EOS_EResult::EOS_Success)
    return plString();

  return szBuffer;
}

plString plEosEpicAccountIdToString(EOS_EpicAccountId id)
{
  if (!EOS_EpicAccountId_IsValid(id))
    return plString();

  char szBuffer[EOS_EPICACCOUNTID_MAX_LENGTH + 1] = {};
  int32_t iLength = EOS_EPICACCOUNTID_MAX_LENGTH + 1;

  if (EOS_EpicAccountId_ToString(id, szBuffer, &iLength) != EOS_EResult::EOS_Success)
    return plString();

  return szBuffer;
}

EOS_ProductUserId plEosProductUserIdFromUser(const plPlatformUserId& user)
{
  if (!user.IsValid() || user.GetBackend() != "eos")
    return nullptr;

  const plStringBuilder sNative(user.GetNativeId());
  return EOS_ProductUserId_FromString(sNative.GetData());
}

plPlatformUserId plEosMakeUserId(EOS_ProductUserId id)
{
  return plPlatformUserId::Make("eos", plEosProductUserIdToString(id));
}

plEosCall* plEosNewCall(plEosPlatformBackend* pBackend, plStringView sKind)
{
  plEosCall* pCall = PL_DEFAULT_NEW(plEosCall);
  pCall->m_pBackend = pBackend;
  pCall->m_pOp = plPlatformOperation::Create(sKind);

  return pCall;
}

plString plEosFindLaunchArgument(plStringView sPrefix)
{
  const plCommandLineUtils* pCmd = plCommandLineUtils::GetGlobalInstance();

  for (plUInt32 i = 0; i < pCmd->GetParameterCount(); ++i)
  {
    const plString& sParam = pCmd->GetParameter(i);
    if (sParam.StartsWith_NoCase(sPrefix))
      return plStringView(sParam.GetData() + sPrefix.GetElementCount());
  }

  return plString();
}

void plEosFillLobbyAttribute(EOS_Lobby_AttributeData& out_data, const char* szKey, const char* szValue, bool bNumeric)
{
  out_data = {};
  out_data.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
  out_data.Key = szKey;

  plInt64 iValue = 0;
  if (bNumeric && plConversionUtils::StringToInt64(szValue, iValue).Succeeded())
  {
    out_data.ValueType = EOS_ELobbyAttributeType::EOS_AT_INT64;
    out_data.Value.AsInt64 = iValue;
    return;
  }

  out_data.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
  out_data.Value.AsUtf8 = szValue;
}

// ---- logging -----------------------------------------------------------------------------------------------

static void EOS_CALL EosLogCallback(const EOS_LogMessage* pMessage)
{
  if (pMessage == nullptr || pMessage->Message == nullptr)
    return;

  switch (pMessage->Level)
  {
    case EOS_ELogLevel::EOS_LOG_Fatal:
    case EOS_ELogLevel::EOS_LOG_Error:
      plLog::Error("EOS [{}]: {}", pMessage->Category, pMessage->Message);
      break;
    case EOS_ELogLevel::EOS_LOG_Warning:
      plLog::Warning("EOS [{}]: {}", pMessage->Category, pMessage->Message);
      break;
    case EOS_ELogLevel::EOS_LOG_Info:
      plLog::Info("EOS [{}]: {}", pMessage->Category, pMessage->Message);
      break;
    case EOS_ELogLevel::EOS_LOG_Verbose:
    case EOS_ELogLevel::EOS_LOG_VeryVerbose:
      plLog::Debug("EOS [{}]: {}", pMessage->Category, pMessage->Message);
      break;
    default:
      break;
  }
}

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
/// The XAudio 2.9 redistributable EOS voice needs on Windows: beside the game, or beside the EOS SDK DLL (which is where
/// the SDK ships it). Empty when neither has it.
static plString FindXAudioRedist()
{
  plHybridArray<plString, 2> dirs;
  dirs.PushBack(plOSFile::GetApplicationDirectory());

  if (HMODULE hEos = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll"))
  {
    wchar_t szPath[MAX_PATH] = {};
    if (GetModuleFileNameW(hEos, szPath, MAX_PATH) > 0)
    {
      plStringBuilder sDir = plStringUtf8(szPath).GetData();
      sDir.PathParentDirectory();
      dirs.PushBack(sDir);
    }
  }

  plStringBuilder sFile;
  for (const plString& sDir : dirs)
  {
    sFile = sDir;
    sFile.AppendPath("xaudio2_9redist.dll");
    sFile.MakeCleanPath();
    if (plOSFile::ExistsFile(sFile))
      return sFile;
  }

  return {};
}
#endif

// ---- lifetime ----------------------------------------------------------------------------------------------

plEosPlatformBackend::plEosPlatformBackend()
  : m_SingletonRegistrar(this)
{
}

plEosPlatformBackend::~plEosPlatformBackend()
{
  Shutdown();
}

plResult plEosPlatformBackend::Initialize(plPlatformBackendHost& host)
{
  if (m_bInitialized)
    return PL_SUCCESS;

  m_pHost = &host;

  m_sProductId = GetSetting("ProductId");
  const plString sSandboxId = GetSetting("SandboxId");
  const plString sDeploymentId = GetSetting("DeploymentId");
  const plString sClientId = GetSetting("ClientId");
  const plString sEncryptionKey = GetSetting("EncryptionKey");

  plString sSecretEnvironment = GetSetting("ClientSecretEnvironment");
  if (sSecretEnvironment.IsEmpty())
    sSecretEnvironment = "PLASMA_EOS_CLIENT_SECRET";

  const plString sClientSecret = plEnvironmentVariableUtils::GetValueString(sSecretEnvironment);

  if (m_sProductId.IsEmpty() || sSandboxId.IsEmpty() || sDeploymentId.IsEmpty() || sClientId.IsEmpty())
  {
    plLog::Error("EOS: ProductId, SandboxId, DeploymentId and ClientId must all be set in the 'eos' block of PlatformServices.ddl.");
    m_pHost = nullptr;
    return PL_FAILURE;
  }

  if (sClientSecret.IsEmpty())
  {
    plLog::Error("EOS: configured client-secret environment variable is not set; the secret is never read from a config file.");
    m_pHost = nullptr;
    return PL_FAILURE;
  }

  plString sProductName = "Plasma";
  if (plApplication::GetApplicationInstance() != nullptr && !plApplication::GetApplicationInstance()->GetApplicationName().IsEmpty())
    sProductName = plApplication::GetApplicationInstance()->GetApplicationName();

  EOS_InitializeOptions initOptions = {};
  initOptions.ApiVersion = EOS_INITIALIZE_API_LATEST;
  initOptions.ProductName = sProductName.GetData();
  initOptions.ProductVersion = plSdkVersion::GetString();

  const EOS_EResult initResult = EOS_Initialize(&initOptions);
  if (initResult != EOS_EResult::EOS_Success && initResult != EOS_EResult::EOS_AlreadyConfigured)
  {
    plLog::Error("EOS: EOS_Initialize failed: {}", plEosResultName(initResult));
    m_pHost = nullptr;
    return PL_FAILURE;
  }

  EOS_Logging_SetCallback(&EosLogCallback);
  EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);

  const plString sCacheDirectory = plOSFile::GetTempDataFolder("PlasmaEos");

  EOS_Platform_RTCOptions rtcOptions = {};
  rtcOptions.ApiVersion = EOS_PLATFORM_RTCOPTIONS_API_LATEST;

  EOS_Platform_Options platformOptions = {};
  platformOptions.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
  platformOptions.bIsServer = EOS_FALSE;
  platformOptions.ProductId = m_sProductId.GetData();
  platformOptions.SandboxId = sSandboxId.GetData();
  platformOptions.DeploymentId = sDeploymentId.GetData();
  platformOptions.ClientCredentials.ClientId = sClientId.GetData();
  platformOptions.ClientCredentials.ClientSecret = sClientSecret.GetData();
  platformOptions.EncryptionKey = sEncryptionKey.IsEmpty() ? nullptr : sEncryptionKey.GetData();
  platformOptions.CacheDirectory = sCacheDirectory.GetData();
  platformOptions.Flags = 0;
  platformOptions.TickBudgetInMilliseconds = 0;
  // Do not initialize RTC for projects that disable lobby voice.
  bool bEnableRtc = GetSettingBool("LobbyVoice", false);

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
  // EOS_Platform_Create rejects Windows RTC options without the XAudio 2.9 redistributable, so without the DLL the
  // platform is created without RTC: lobby voice is then off, everything else works.
  const plString sXAudioPath = bEnableRtc ? FindXAudioRedist() : plString();
  EOS_Windows_RTCOptions windowsRtcOptions = {};
  windowsRtcOptions.ApiVersion = EOS_WINDOWS_RTCOPTIONS_API_LATEST;
  windowsRtcOptions.XAudio29DllPath = sXAudioPath.GetData();
  rtcOptions.PlatformSpecificOptions = &windowsRtcOptions;

  if (bEnableRtc && sXAudioPath.IsEmpty())
  {
    plLog::Warning("EOS: xaudio2_9redist.dll was found neither beside the game nor beside the EOS SDK DLL; lobby voice is disabled.");
    bEnableRtc = false;
  }
#endif

  platformOptions.RTCOptions = bEnableRtc ? &rtcOptions : nullptr;

  m_hPlatform = EOS_Platform_Create(&platformOptions);
  if (m_hPlatform == nullptr)
  {
    plLog::Error("EOS: EOS_Platform_Create failed; check the ids in PlatformServices.ddl and the client secret.");
    EOS_Shutdown();
    m_pHost = nullptr;
    return PL_FAILURE;
  }

  AcquireInterfaces();

  if (sEncryptionKey.IsEmpty())
  {
    plLog::Warning("EOS: no EncryptionKey configured, player data storage (cloud saves) is unavailable.");
    m_hPlayerDataStorage = nullptr;
  }

  m_sSocketName = GetSetting("P2PSocket", "plasma");

  // Decides whether lobbies are joined with manual audio output; absent means EOS renders voice itself.
  m_pPcmProvider = plSingletonRegistry::GetSingletonInstance<plAudioPcmStreamProvider>();

  m_bInitialized = true;

  RegisterLobbyNotifications();
  ReadLaunchArguments();

  if (m_hUI != nullptr)
  {
    EOS_UI_AddNotifyDisplaySettingsUpdatedOptions options = {};
    options.ApiVersion = EOS_UI_ADDNOTIFYDISPLAYSETTINGSUPDATED_API_LATEST;

    m_DisplaySettingsNotification = EOS_UI_AddNotifyDisplaySettingsUpdated(m_hUI, &options, this, &OnDisplaySettingsUpdated);
  }

  plPlatformEvent ready;
  ready.m_Type = plPlatformEvent::Type::BackendReady;
  ready.m_sId = "eos";
  Emit(ready);

  plLog::Info("EOS: platform created for product '{}'.", m_sProductId);

  return PL_SUCCESS;
}

void plEosPlatformBackend::AcquireInterfaces()
{
  m_hAuth = EOS_Platform_GetAuthInterface(m_hPlatform);
  m_hConnect = EOS_Platform_GetConnectInterface(m_hPlatform);
  m_hUserInfo = EOS_Platform_GetUserInfoInterface(m_hPlatform);
  m_hAchievements = EOS_Platform_GetAchievementsInterface(m_hPlatform);
  m_hStats = EOS_Platform_GetStatsInterface(m_hPlatform);
  m_hLeaderboards = EOS_Platform_GetLeaderboardsInterface(m_hPlatform);
  m_hFriends = EOS_Platform_GetFriendsInterface(m_hPlatform);
  m_hPresence = EOS_Platform_GetPresenceInterface(m_hPlatform);
  m_hLobby = EOS_Platform_GetLobbyInterface(m_hPlatform);
  m_hEcom = EOS_Platform_GetEcomInterface(m_hPlatform);
  m_hUI = EOS_Platform_GetUIInterface(m_hPlatform);
  m_hP2P = EOS_Platform_GetP2PInterface(m_hPlatform);
  m_hPlayerDataStorage = EOS_Platform_GetPlayerDataStorageInterface(m_hPlatform);
  m_hRTC = EOS_Platform_GetRTCInterface(m_hPlatform);
  m_hRTCAudio = m_hRTC != nullptr ? EOS_RTC_GetAudioInterface(m_hRTC) : nullptr;
}

void plEosPlatformBackend::Shutdown()
{
  if (!m_bInitialized)
    return;

  // Requests still waiting on the I/O side get an answer before the platform goes away.
  {
    plDynamicArray<plEosCloudRequest*> pending;

    {
      PL_LOCK(m_CloudMutex);
      while (!m_CloudQueue.IsEmpty())
      {
        pending.PushBack(m_CloudQueue.PeekFront());
        m_CloudQueue.PopFront();
      }
    }

    for (plEosCloudRequest* pRequest : pending)
      FinishCloudRequest(*pRequest, plSaveGameError::Cancelled);
  }

  if (m_pActiveCloud != nullptr)
  {
    plEosCloudRequest* pActive = m_pActiveCloud;
    m_pActiveCloud = nullptr;
    FinishCloudRequest(*pActive, plSaveGameError::Cancelled);
  }

  if (m_pTicketOp != nullptr)
    SetMatchmakingState(plPlatformMatchmakingState::Cancelled);

  UnregisterVoiceNotifications();
  DestroyVoiceStream();
  UnregisterTransportNotifications();
  UnregisterUserNotifications();
  UnregisterLobbyNotifications();

  if (m_hUI != nullptr && m_DisplaySettingsNotification != EOS_INVALID_NOTIFICATIONID)
  {
    EOS_UI_RemoveNotifyDisplaySettingsUpdated(m_hUI, m_DisplaySettingsNotification);
    m_DisplaySettingsNotification = EOS_INVALID_NOTIFICATIONID;
  }

  if (m_hPlatform != nullptr)
  {
    EOS_Platform_Release(m_hPlatform);
    m_hPlatform = nullptr;
  }

  EOS_Shutdown();

  m_hAuth = nullptr;
  m_hConnect = nullptr;
  m_hUserInfo = nullptr;
  m_hAchievements = nullptr;
  m_hStats = nullptr;
  m_hLeaderboards = nullptr;
  m_hFriends = nullptr;
  m_hPresence = nullptr;
  m_hLobby = nullptr;
  m_hEcom = nullptr;
  m_hUI = nullptr;
  m_hP2P = nullptr;
  m_hPlayerDataStorage = nullptr;
  m_hRTC = nullptr;
  m_hRTCAudio = nullptr;

  m_bSignedIn = false;
  m_OnlineState = plPlatformOnlineState::Offline;
  m_LocalEpicAccountId = nullptr;
  m_LocalProductUserId = nullptr;

  m_pSignInOp = nullptr;
  m_pFriendsOp = nullptr;
  m_pTicketOp = nullptr;
  m_pVoiceJoinOp = nullptr;

  m_Friends.Clear();
  m_Peers.Clear();
  m_TransportEvents.Clear();
  m_bInLobby = false;
  m_CurrentLobby = plPlatformLobbyInfo();

  m_bInitialized = false;
  m_pHost = nullptr;
}

void plEosPlatformBackend::Tick()
{
  if (!m_bInitialized)
    return;

  ProcessCloudQueue();
  EOS_Platform_Tick(m_hPlatform);

  if (m_bListening || !m_Peers.IsEmpty())
    ReceivePackets();

  if (m_bPresenceDirty && !m_bPresenceInFlight && m_bSignedIn)
    PublishPresence().IgnoreResult();

  if (m_pTicketOp != nullptr)
    UpdateMatchmaking();
}

plBitflags<plPlatformCapability> plEosPlatformBackend::GetCapabilities() const
{
  plBitflags<plPlatformCapability> caps;

  if (m_hAchievements)
    caps.Add(plPlatformCapability::Achievements);
  if (m_hStats)
    caps.Add(plPlatformCapability::Stats);
  if (m_hLeaderboards)
    caps.Add(plPlatformCapability::Leaderboards);
  if (m_hFriends)
    caps.Add(plPlatformCapability::Friends);
  if (m_hPresence)
    caps.Add(plPlatformCapability::Presence);
  if (m_hLobby)
    caps.Add(plPlatformCapability::Lobbies | plPlatformCapability::Matchmaking | plPlatformCapability::Sessions);
  if (m_hPlayerDataStorage)
    caps.Add(plPlatformCapability::CloudStorage);
  if (m_hEcom)
    caps.Add(plPlatformCapability::Entitlements);
  if (m_hUI)
    caps.Add(plPlatformCapability::Overlay);
  if (m_hP2P)
    caps.Add(plPlatformCapability::Transport);
  if (m_hRTC)
    caps.Add(plPlatformCapability::Voice);

  return caps;
}

// ---- shared helpers ----------------------------------------------------------------------------------------

void plEosPlatformBackend::Emit(const plPlatformEvent& event)
{
  if (m_pHost != nullptr)
    m_pHost->Emit(event);
}

plString plEosPlatformBackend::GetSetting(plStringView sKey, plStringView sFallback) const
{
  if (m_pHost == nullptr)
    return sFallback;

  const plVariantDictionary& settings = m_pHost->GetBackendSettings();
  const plVariant* pValue = settings.GetValue(plString(sKey));
  if (pValue == nullptr || !pValue->IsValid())
    return sFallback;

  return pValue->ConvertTo<plString>();
}

bool plEosPlatformBackend::GetSettingBool(plStringView sKey, bool bFallback) const
{
  if (m_pHost == nullptr)
    return bFallback;

  const plVariantDictionary& settings = m_pHost->GetBackendSettings();
  const plVariant* pValue = settings.GetValue(plString(sKey));
  if (pValue == nullptr || !pValue->IsValid())
    return bFallback;

  return pValue->ConvertTo<bool>();
}

plPlatformOperationPtr plEosPlatformBackend::GateSignedIn(plStringView sKind) const
{
  if (!m_bSignedIn || !EOS_ProductUserId_IsValid(m_LocalProductUserId))
    return plPlatformOperation::CreateFinished(sKind, plPlatformError::NotSignedIn);

  return nullptr;
}

void plEosPlatformBackend::SetOnlineState(plPlatformOnlineState state)
{
  if (m_OnlineState == state)
    return;

  m_OnlineState = state;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::OnlineStateChanged;
  e.m_Value = static_cast<plInt32>(state);
  Emit(e);
}

plString plEosPlatformBackend::GetCachedDisplayName(EOS_ProductUserId id) const
{
  const plString sId = plEosProductUserIdToString(id);
  if (const plString* pName = m_DisplayNames.GetValue(sId))
    return *pName;

  return sId;
}

void plEosPlatformBackend::ResolveDisplayNames(plArrayPtr<const EOS_ProductUserId> ids)
{
  if (m_hConnect == nullptr || !m_bSignedIn || ids.IsEmpty())
    return;

  plHybridArray<EOS_ProductUserId, 16> unknown;

  for (EOS_ProductUserId id : ids)
  {
    if (!EOS_ProductUserId_IsValid(id))
      continue;

    if (m_DisplayNames.Contains(plEosProductUserIdToString(id)))
      continue;

    unknown.PushBack(id);
  }

  if (unknown.IsEmpty())
    return;

  plEosCall* pCall = plEosNewCall(this, "Identity.ResolveNames");
  pCall->m_pUser = PL_DEFAULT_NEW(plDynamicArray<EOS_ProductUserId>);
  static_cast<plDynamicArray<EOS_ProductUserId>*>(pCall->m_pUser)->PushBackRange(unknown);

  EOS_Connect_QueryProductUserIdMappingsOptions options = {};
  options.ApiVersion = EOS_CONNECT_QUERYPRODUCTUSERIDMAPPINGS_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.ProductUserIds = unknown.GetData();
  options.ProductUserIdCount = unknown.GetCount();

  EOS_Connect_QueryProductUserIdMappings(m_hConnect, &options, pCall, &OnResolveNames);
}

void EOS_CALL plEosPlatformBackend::OnResolveNames(const EOS_Connect_QueryProductUserIdMappingsCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;
  plDynamicArray<EOS_ProductUserId>* pIds = static_cast<plDynamicArray<EOS_ProductUserId>*>(pCall->m_pUser);

  if (pData->ResultCode == EOS_EResult::EOS_Success && pThis->m_hConnect != nullptr)
  {
    for (EOS_ProductUserId id : *pIds)
    {
      EOS_Connect_CopyProductUserExternalAccountByAccountTypeOptions copyOptions = {};
      copyOptions.ApiVersion = EOS_CONNECT_COPYPRODUCTUSEREXTERNALACCOUNTBYACCOUNTTYPE_API_LATEST;
      copyOptions.TargetUserId = id;
      copyOptions.AccountIdType = EOS_EExternalAccountType::EOS_EAT_EPIC;

      EOS_Connect_ExternalAccountInfo* pInfo = nullptr;
      if (EOS_Connect_CopyProductUserExternalAccountByAccountType(pThis->m_hConnect, &copyOptions, &pInfo) == EOS_EResult::EOS_Success && pInfo != nullptr)
      {
        if (pInfo->DisplayName != nullptr && pInfo->DisplayName[0] != '\0')
          pThis->m_DisplayNames[plEosProductUserIdToString(id)] = pInfo->DisplayName;

        EOS_Connect_ExternalAccountInfo_Release(pInfo);
      }
    }

    // Views that carry names re-read the cache on these.
    if (pThis->m_bInLobby)
    {
      for (plPlatformLobbyMember& member : pThis->m_CurrentLobby.m_Members)
        member.m_User.m_sDisplayName = pThis->GetCachedDisplayName(plEosProductUserIdFromUser(member.m_User.m_Id));

      plPlatformEvent e;
      e.m_Type = plPlatformEvent::Type::LobbyUpdated;
      e.m_Lobby = pThis->m_CurrentLobby.m_Id;
      pThis->Emit(e);
    }
  }

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pIds);
  PL_DEFAULT_DELETE(pCall);
}
