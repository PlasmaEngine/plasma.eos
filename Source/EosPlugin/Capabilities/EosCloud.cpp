#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Utilities/ConversionUtils.h>

// Cloud saves on EOS Player Data Storage.
//
// Threading: the save system calls the provider from an I/O task and may block, while every EOS call has to be
// issued on the main thread. A call therefore builds a plEosCloudRequest, queues it and waits on the request's
// signal; Tick() starts one request at a time and the SDK callbacks finish it. When the caller IS the main thread
// the request is pumped inline (EOS_Platform_Tick in a loop) so the wait cannot deadlock. A wait that runs out
// abandons the request: the backend frees it when the SDK eventually answers.
//
// Files: "<project>/<profile>/<slot>.plsave" with the two uuids in base64url (22 characters each) so the name fits
// the 64-byte limit; the project prefix is the project name reduced to allowed characters, at most 11 of them.
// Every file starts with a 40-byte header: magic "PLSV", version, flags (bit 0 = tombstone), padding, the revision
// and the parent revision as raw uuid bytes. The envelope follows.
//
// Revision checks read the current file before writing; EOS has no compare-and-swap, so two clients publishing
// the same slot at the same instant can still race. The window is one round trip.

namespace
{
  constexpr plUInt32 s_uiHeaderSize = 40;
  constexpr plUInt8 s_uiHeaderVersion = 1;
  constexpr plUInt8 s_uiFlagDeleted = 1;
  constexpr plUInt32 s_uiChunkSize = 64 * 1024;
  constexpr plUInt32 s_uiMaxProjectChars = 11;
  const plTime s_WaitTimeout = plTime::MakeFromSeconds(120.0);
  const plTime s_InlineTimeout = plTime::MakeFromSeconds(60.0);

  const char* s_szBase64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  plInt32 Base64UrlValue(char c)
  {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '-')
      return 62;
    if (c == '_')
      return 63;

    return -1;
  }

  void UuidToBytes(const plUuid& uuid, plUInt8* pOut16)
  {
    plUInt64 uiLow = 0;
    plUInt64 uiHigh = 0;
    uuid.GetValues(uiLow, uiHigh);

    for (plUInt32 i = 0; i < 8; ++i)
    {
      pOut16[i] = static_cast<plUInt8>(uiLow >> (i * 8));
      pOut16[8 + i] = static_cast<plUInt8>(uiHigh >> (i * 8));
    }
  }

  plUuid UuidFromBytes(const plUInt8* pIn16)
  {
    plUInt64 uiLow = 0;
    plUInt64 uiHigh = 0;

    for (plUInt32 i = 0; i < 8; ++i)
    {
      uiLow |= static_cast<plUInt64>(pIn16[i]) << (i * 8);
      uiHigh |= static_cast<plUInt64>(pIn16[8 + i]) << (i * 8);
    }

    return plUuid(uiLow, uiHigh);
  }

  // 16 bytes -> 22 characters, no padding.
  void EncodeUuid(const plUuid& uuid, plStringBuilder& ref_sOut)
  {
    // Two spare zero bytes round the input up to whole 3-byte groups; their characters are dropped.
    plUInt8 bytes[18] = {};
    UuidToBytes(uuid, bytes);

    char szText[23] = {};
    plUInt32 uiOut = 0;

    for (plUInt32 i = 0; i < 18; i += 3)
    {
      const plUInt32 uiTriple = (static_cast<plUInt32>(bytes[i]) << 16) | (static_cast<plUInt32>(bytes[i + 1]) << 8) | bytes[i + 2];

      szText[uiOut++] = s_szBase64Url[(uiTriple >> 18) & 63];
      szText[uiOut++] = s_szBase64Url[(uiTriple >> 12) & 63];
      szText[uiOut++] = s_szBase64Url[(uiTriple >> 6) & 63];
      szText[uiOut++] = s_szBase64Url[uiTriple & 63];
    }

    szText[22] = '\0';
    ref_sOut.Append(szText);
  }

  bool DecodeUuid(plStringView sText, plUuid& out_uuid)
  {
    if (sText.GetElementCount() != 22)
      return false;

    // Pad back to whole 4-character groups with zero digits.
    char szText[24] = {};
    plMemoryUtils::Copy(szText, sText.GetStartPointer(), 22);
    szText[22] = 'A';
    szText[23] = 'A';

    plUInt8 bytes[18] = {};
    plUInt32 uiOut = 0;

    for (plUInt32 i = 0; i < 24; i += 4)
    {
      const plInt32 a = Base64UrlValue(szText[i]);
      const plInt32 b = Base64UrlValue(szText[i + 1]);
      const plInt32 c = Base64UrlValue(szText[i + 2]);
      const plInt32 d = Base64UrlValue(szText[i + 3]);

      if (a < 0 || b < 0 || c < 0 || d < 0)
        return false;

      const plUInt32 uiTriple = (a << 18) | (b << 12) | (c << 6) | d;
      bytes[uiOut++] = static_cast<plUInt8>(uiTriple >> 16);
      bytes[uiOut++] = static_cast<plUInt8>(uiTriple >> 8);
      bytes[uiOut++] = static_cast<plUInt8>(uiTriple);
    }

    out_uuid = UuidFromBytes(bytes);
    return true;
  }

  void ProjectPrefix(plStringView sProject, plStringBuilder& ref_sOut)
  {
    plUInt32 uiCount = 0;

    for (auto it = sProject.GetIteratorFront(); it.IsValid() && uiCount < s_uiMaxProjectChars; ++it)
    {
      const plUInt32 uiChar = it.GetCharacter();
      const bool bAllowed = (uiChar >= 'a' && uiChar <= 'z') || (uiChar >= 'A' && uiChar <= 'Z') || (uiChar >= '0' && uiChar <= '9') || uiChar == '-' || uiChar == '_';

      if (!bAllowed)
        continue;

      ref_sOut.Append(uiChar);
      ++uiCount;
    }

    if (uiCount == 0)
      ref_sOut.Append("default");
  }

  void MakeFileName(const plSaveGameCloudKey& key, plStringBuilder& ref_sOut)
  {
    ref_sOut.Clear();
    ProjectPrefix(key.m_sProject, ref_sOut);
    ref_sOut.Append("/");
    EncodeUuid(key.m_Profile, ref_sOut);
    ref_sOut.Append("/");
    EncodeUuid(key.m_Slot, ref_sOut);
    ref_sOut.Append(".plsave");
  }

  bool ParseFileName(plStringView sName, plStringView sProject, const plUuid& profile, plSaveGameCloudKey& out_key)
  {
    plStringBuilder sPrefix;
    ProjectPrefix(sProject, sPrefix);
    sPrefix.Append("/");
    EncodeUuid(profile, sPrefix);
    sPrefix.Append("/");

    if (!sName.StartsWith(sPrefix))
      return false;

    plStringView sRest(sName.GetStartPointer() + sPrefix.GetElementCount(), sName.GetEndPointer());
    if (!sRest.EndsWith(".plsave"))
      return false;

    // Strip the ".plsave" extension.
    sRest = plStringView(sRest.GetStartPointer(), sRest.GetEndPointer() - 7);

    plUuid slot;
    if (!DecodeUuid(sRest, slot))
      return false;

    out_key.m_sProject = sProject;
    out_key.m_Profile = profile;
    out_key.m_Slot = slot;

    return true;
  }

  void SerializeRecord(const plSaveGameCloudRecord& record, plDynamicArray<plUInt8>& out_bytes)
  {
    out_bytes.SetCountUninitialized(s_uiHeaderSize + record.m_Envelope.GetCount());

    plUInt8* pHeader = out_bytes.GetData();
    pHeader[0] = 'P';
    pHeader[1] = 'L';
    pHeader[2] = 'S';
    pHeader[3] = 'V';
    pHeader[4] = s_uiHeaderVersion;
    pHeader[5] = record.m_bDeleted ? s_uiFlagDeleted : 0;
    pHeader[6] = 0;
    pHeader[7] = 0;
    UuidToBytes(record.m_Revision, pHeader + 8);
    UuidToBytes(record.m_ParentRevision, pHeader + 24);

    if (!record.m_Envelope.IsEmpty())
      plMemoryUtils::Copy(pHeader + s_uiHeaderSize, record.m_Envelope.GetData(), record.m_Envelope.GetCount());
  }

  bool ParseRecord(const plDynamicArray<plUInt8>& bytes, plSaveGameCloudRecord& out_record)
  {
    if (bytes.GetCount() < s_uiHeaderSize)
      return false;

    const plUInt8* pHeader = bytes.GetData();
    if (pHeader[0] != 'P' || pHeader[1] != 'L' || pHeader[2] != 'S' || pHeader[3] != 'V' || pHeader[4] != s_uiHeaderVersion)
      return false;

    out_record.m_bDeleted = (pHeader[5] & s_uiFlagDeleted) != 0;
    out_record.m_Revision = UuidFromBytes(pHeader + 8);
    out_record.m_ParentRevision = UuidFromBytes(pHeader + 24);
    out_record.m_Envelope.SetCountUninitialized(bytes.GetCount() - s_uiHeaderSize);

    if (!out_record.m_Envelope.IsEmpty())
      plMemoryUtils::Copy(out_record.m_Envelope.GetData(), pHeader + s_uiHeaderSize, out_record.m_Envelope.GetCount());

    return true;
  }
} // namespace

struct plEosCloudRequest
{
  enum class Kind : plUInt8
  {
    Read,
    Publish,
    Enumerate,
  };

  enum class Phase : plUInt8
  {
    Idle,
    ReadingExisting, ///< Publish: the revision check before the write.
    Reading,
    Writing,
    Listing,
  };

  plEosPlatformBackend* m_pBackend = nullptr;
  Kind m_Kind = Kind::Read;
  Phase m_Phase = Phase::Idle;
  plSaveGameCloudKey m_Key;
  plString m_sFileName;

  // Inputs.
  plSaveGameCloudRecord m_Record; ///< Publish: what to write.
  plUuid m_ExpectedRevision;
  plString m_sProject;
  plUuid m_Profile;

  // Outputs, valid until the caller abandons the request.
  plSaveGameCloudRecord* m_pOutRecord = nullptr;
  plDynamicArray<plSaveGameCloudKey>* m_pOutKeys = nullptr;

  // Transfer state.
  plDynamicArray<plUInt8> m_Buffer;
  plUInt32 m_uiWriteOffset = 0;
  EOS_HPlayerDataStorageFileTransferRequest m_hTransfer = nullptr;

  plSaveGameError m_Result = plSaveGameError::IoError;
  bool m_bFinished = false;
  bool m_bAbandoned = false; ///< The waiting side gave up; whoever finishes the request frees it.
  plMutex m_Mutex;
  plThreadSignal m_Done{plThreadSignal::Mode::ManualReset};
};

//////////////////////////////////////////////////////////////////////////
// Provider entry points

plSaveGameError plEosPlatformBackend::Read(const plSaveGameCloudKey& key, plSaveGameCloudRecord& out_record)
{
  plEosCloudRequest* pRequest = PL_DEFAULT_NEW(plEosCloudRequest);
  pRequest->m_Kind = plEosCloudRequest::Kind::Read;
  pRequest->m_Key = key;
  pRequest->m_pOutRecord = &out_record;

  return RunCloudRequest(pRequest);
}

plSaveGameError plEosPlatformBackend::Publish(const plSaveGameCloudKey& key, const plSaveGameCloudRecord& record, const plUuid& expectedRemoteRevision)
{
  plEosCloudRequest* pRequest = PL_DEFAULT_NEW(plEosCloudRequest);
  pRequest->m_Kind = plEosCloudRequest::Kind::Publish;
  pRequest->m_Key = key;
  pRequest->m_Record = record;
  pRequest->m_ExpectedRevision = expectedRemoteRevision;

  return RunCloudRequest(pRequest);
}

plSaveGameError plEosPlatformBackend::Enumerate(plStringView project, const plUuid& profile, plDynamicArray<plSaveGameCloudKey>& out_keys)
{
  plEosCloudRequest* pRequest = PL_DEFAULT_NEW(plEosCloudRequest);
  pRequest->m_Kind = plEosCloudRequest::Kind::Enumerate;
  pRequest->m_sProject = project;
  pRequest->m_Profile = profile;
  pRequest->m_pOutKeys = &out_keys;

  return RunCloudRequest(pRequest);
}

plSaveGameError plEosPlatformBackend::RunCloudRequest(plEosCloudRequest* pRequest)
{
  pRequest->m_pBackend = this;

  if (!m_bInitialized || m_hPlayerDataStorage == nullptr || !m_bSignedIn || !EOS_ProductUserId_IsValid(m_LocalProductUserId))
  {
    plLog::Warning("EOS: cloud save request while the backend is not signed in.");
    PL_DEFAULT_DELETE(pRequest);
    return plSaveGameError::IoError;
  }

  {
    PL_LOCK(m_CloudMutex);
    m_CloudQueue.PushBack(pRequest);
  }

  if (plThreadUtils::IsMainThread())
  {
    // Nobody else ticks the platform while this thread waits, so pump it here.
    const plTime deadline = plTime::Now() + s_InlineTimeout;

    while (!pRequest->m_bFinished)
    {
      ProcessCloudQueue();
      EOS_Platform_Tick(m_hPlatform);

      if (plTime::Now() > deadline)
        break;

      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
    }
  }
  else
  {
    pRequest->m_Done.WaitForSignal(s_WaitTimeout);
  }

  plSaveGameError result = plSaveGameError::IoError;
  bool bDeleteNow = false;

  {
    PL_LOCK(pRequest->m_Mutex);

    if (pRequest->m_bFinished)
    {
      result = pRequest->m_Result;
      bDeleteNow = true;
    }
    else
    {
      // Still queued or in flight: detach the outputs and let the main thread free it later.
      pRequest->m_bAbandoned = true;
      pRequest->m_pOutRecord = nullptr;
      pRequest->m_pOutKeys = nullptr;
      plLog::Warning("EOS: cloud save request timed out.");
    }
  }

  if (bDeleteNow)
    PL_DEFAULT_DELETE(pRequest);

  return result;
}

void plEosPlatformBackend::ProcessCloudQueue()
{
  if (m_pActiveCloud != nullptr)
    return;

  plEosCloudRequest* pNext = nullptr;

  {
    PL_LOCK(m_CloudMutex);

    while (!m_CloudQueue.IsEmpty())
    {
      pNext = m_CloudQueue.PeekFront();
      m_CloudQueue.PopFront();

      PL_LOCK(pNext->m_Mutex);
      if (!pNext->m_bAbandoned)
        break;

      // Abandoned before it started: nobody waits, nothing to do.
      PL_DEFAULT_DELETE(pNext);
      pNext = nullptr;
    }
  }

  if (pNext == nullptr)
    return;

  m_pActiveCloud = pNext;
  StartCloudRequest(*pNext);
}

void plEosPlatformBackend::FinishCloudRequest(plEosCloudRequest& ref_request, plSaveGameError error)
{
  if (ref_request.m_hTransfer != nullptr)
  {
    EOS_PlayerDataStorageFileTransferRequest_Release(ref_request.m_hTransfer);
    ref_request.m_hTransfer = nullptr;
  }

  if (m_pActiveCloud == &ref_request)
    m_pActiveCloud = nullptr;

  bool bDelete = false;

  {
    PL_LOCK(ref_request.m_Mutex);
    ref_request.m_Result = error;
    ref_request.m_bFinished = true;
    bDelete = ref_request.m_bAbandoned;
  }

  if (bDelete)
  {
    plEosCloudRequest* pRequest = &ref_request;
    PL_DEFAULT_DELETE(pRequest);
    return;
  }

  ref_request.m_Done.RaiseSignal();
}

//////////////////////////////////////////////////////////////////////////
// Request state machine

void plEosPlatformBackend::StartCloudRequest(plEosCloudRequest& ref_request)
{
  switch (ref_request.m_Kind)
  {
    case plEosCloudRequest::Kind::Read:
    {
      plStringBuilder sName;
      MakeFileName(ref_request.m_Key, sName);

      ref_request.m_sFileName = sName;
      ref_request.m_Phase = plEosCloudRequest::Phase::Reading;
      BeginCloudRead(ref_request);
      return;
    }

    case plEosCloudRequest::Kind::Publish:
    {
      plStringBuilder sName;
      MakeFileName(ref_request.m_Key, sName);

      ref_request.m_sFileName = sName;
      ref_request.m_Phase = plEosCloudRequest::Phase::ReadingExisting;
      BeginCloudRead(ref_request);
      return;
    }

    case plEosCloudRequest::Kind::Enumerate:
    {
      ref_request.m_Phase = plEosCloudRequest::Phase::Listing;

      EOS_PlayerDataStorage_QueryFileListOptions options = {};
      options.ApiVersion = EOS_PLAYERDATASTORAGE_QUERYFILELIST_API_LATEST;
      options.LocalUserId = m_LocalProductUserId;

      EOS_PlayerDataStorage_QueryFileList(m_hPlayerDataStorage, &options, &ref_request, &OnCloudFileList);
      return;
    }
  }
}

void plEosPlatformBackend::BeginCloudRead(plEosCloudRequest& ref_request)
{
  ref_request.m_Buffer.Clear();

  EOS_PlayerDataStorage_ReadFileOptions options = {};
  options.ApiVersion = EOS_PLAYERDATASTORAGE_READFILE_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.Filename = ref_request.m_sFileName.GetData();
  options.ReadChunkLengthBytes = s_uiChunkSize;
  options.ReadFileDataCallback = &OnCloudReadData;
  options.FileTransferProgressCallback = nullptr;

  ref_request.m_hTransfer = EOS_PlayerDataStorage_ReadFile(m_hPlayerDataStorage, &options, &ref_request, &OnCloudReadComplete);

  if (ref_request.m_hTransfer == nullptr)
    FinishCloudRequest(ref_request, plSaveGameError::IoError);
}

void plEosPlatformBackend::BeginCloudWrite(plEosCloudRequest& ref_request)
{
  ref_request.m_Phase = plEosCloudRequest::Phase::Writing;
  SerializeRecord(ref_request.m_Record, ref_request.m_Buffer);
  ref_request.m_uiWriteOffset = 0;

  // The revision-check read of a Publish still holds its transfer handle.
  if (ref_request.m_hTransfer != nullptr)
  {
    EOS_PlayerDataStorageFileTransferRequest_Release(ref_request.m_hTransfer);
    ref_request.m_hTransfer = nullptr;
  }

  EOS_PlayerDataStorage_WriteFileOptions options = {};
  options.ApiVersion = EOS_PLAYERDATASTORAGE_WRITEFILE_API_LATEST;
  options.LocalUserId = m_LocalProductUserId;
  options.Filename = ref_request.m_sFileName.GetData();
  options.ChunkLengthBytes = s_uiChunkSize;
  options.WriteFileDataCallback = &OnCloudWriteData;
  options.FileTransferProgressCallback = nullptr;

  ref_request.m_hTransfer = EOS_PlayerDataStorage_WriteFile(m_hPlayerDataStorage, &options, &ref_request, &OnCloudWriteComplete);

  if (ref_request.m_hTransfer == nullptr)
    FinishCloudRequest(ref_request, plSaveGameError::IoError);
}

EOS_PlayerDataStorage_EReadResult EOS_CALL plEosPlatformBackend::OnCloudReadData(const EOS_PlayerDataStorage_ReadFileDataCallbackInfo* pData)
{
  plEosCloudRequest* pRequest = static_cast<plEosCloudRequest*>(pData->ClientData);

  if (pData->DataChunkLengthBytes > 0 && pData->DataChunk != nullptr)
  {
    const plUInt32 uiOldCount = pRequest->m_Buffer.GetCount();
    pRequest->m_Buffer.SetCountUninitialized(uiOldCount + pData->DataChunkLengthBytes);

    plMemoryUtils::Copy(pRequest->m_Buffer.GetData() + uiOldCount, static_cast<const plUInt8*>(pData->DataChunk), pData->DataChunkLengthBytes);
  }

  return EOS_PlayerDataStorage_EReadResult::EOS_RR_ContinueReading;
}

void EOS_CALL plEosPlatformBackend::OnCloudReadComplete(const EOS_PlayerDataStorage_ReadFileCallbackInfo* pData)
{
  plEosCloudRequest* pRequest = static_cast<plEosCloudRequest*>(pData->ClientData);
  plEosPlatformBackend* pThis = pRequest->m_pBackend;

  if (pRequest->m_Phase == plEosCloudRequest::Phase::Reading)
  {
    if (pData->ResultCode == EOS_EResult::EOS_NotFound)
    {
      pThis->FinishCloudRequest(*pRequest, plSaveGameError::NotFound);
      return;
    }

    if (pData->ResultCode != EOS_EResult::EOS_Success)
    {
      plLog::Warning("EOS: reading cloud file '{}' failed: {}", pRequest->m_sFileName, plEosResultName(pData->ResultCode));
      pThis->FinishCloudRequest(*pRequest, plSaveGameError::IoError);
      return;
    }

    plSaveGameCloudRecord record;
    if (!ParseRecord(pRequest->m_Buffer, record))
    {
      pThis->FinishCloudRequest(*pRequest, plSaveGameError::CorruptData);
      return;
    }

    {
      PL_LOCK(pRequest->m_Mutex);

      if (pRequest->m_pOutRecord != nullptr)
        *pRequest->m_pOutRecord = std::move(record);
    }

    pThis->FinishCloudRequest(*pRequest, plSaveGameError::Success);
    return;
  }

  // Publish: the revision check.
  if (pData->ResultCode == EOS_EResult::EOS_NotFound)
  {
    if (pRequest->m_ExpectedRevision.IsValid())
    {
      pThis->FinishCloudRequest(*pRequest, plSaveGameError::Conflict);
      return;
    }

    pThis->BeginCloudWrite(*pRequest);
    return;
  }

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    plLog::Warning("EOS: checking cloud file '{}' failed: {}", pRequest->m_sFileName, plEosResultName(pData->ResultCode));
    pThis->FinishCloudRequest(*pRequest, plSaveGameError::IoError);
    return;
  }

  plSaveGameCloudRecord existing;
  if (!ParseRecord(pRequest->m_Buffer, existing))
  {
    // An unreadable remote file is only replaced when the caller claims no prior knowledge of it.
    if (pRequest->m_ExpectedRevision.IsValid())
    {
      pThis->FinishCloudRequest(*pRequest, plSaveGameError::CorruptData);
      return;
    }

    pThis->BeginCloudWrite(*pRequest);
    return;
  }

  if (existing.m_Revision == pRequest->m_Record.m_Revision)
  {
    // Publishing the same revision again is idempotent.
    pThis->FinishCloudRequest(*pRequest, plSaveGameError::Success);
    return;
  }

  if (existing.m_Revision != pRequest->m_ExpectedRevision)
  {
    pThis->FinishCloudRequest(*pRequest, plSaveGameError::Conflict);
    return;
  }

  pThis->BeginCloudWrite(*pRequest);
}

EOS_PlayerDataStorage_EWriteResult EOS_CALL plEosPlatformBackend::OnCloudWriteData(const EOS_PlayerDataStorage_WriteFileDataCallbackInfo* pData, void* pOutDataBuffer, uint32_t* pOutDataWritten)
{
  plEosCloudRequest* pRequest = static_cast<plEosCloudRequest*>(pData->ClientData);

  const plUInt32 uiRemaining = pRequest->m_Buffer.GetCount() - pRequest->m_uiWriteOffset;
  const plUInt32 uiChunk = plMath::Min<plUInt32>(uiRemaining, pData->DataBufferLengthBytes);

  if (uiChunk > 0)
    plMemoryUtils::Copy(static_cast<plUInt8*>(pOutDataBuffer), pRequest->m_Buffer.GetData() + pRequest->m_uiWriteOffset, uiChunk);

  pRequest->m_uiWriteOffset += uiChunk;
  *pOutDataWritten = uiChunk;

  const bool bAllWritten = pRequest->m_uiWriteOffset >= pRequest->m_Buffer.GetCount();

  return bAllWritten ? EOS_PlayerDataStorage_EWriteResult::EOS_WR_CompleteRequest : EOS_PlayerDataStorage_EWriteResult::EOS_WR_ContinueWriting;
}

void EOS_CALL plEosPlatformBackend::OnCloudWriteComplete(const EOS_PlayerDataStorage_WriteFileCallbackInfo* pData)
{
  plEosCloudRequest* pRequest = static_cast<plEosCloudRequest*>(pData->ClientData);
  plEosPlatformBackend* pThis = pRequest->m_pBackend;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
  {
    plLog::Warning("EOS: writing cloud file '{}' failed: {}", pRequest->m_sFileName, plEosResultName(pData->ResultCode));

    const bool bRateLimited = plEosResultToError(pData->ResultCode) == plPlatformError::RateLimited;
    pThis->FinishCloudRequest(*pRequest, bRateLimited ? plSaveGameError::LimitExceeded : plSaveGameError::IoError);
    return;
  }

  plStringBuilder sSlot;
  plConversionUtils::ToString(pRequest->m_Key.m_Slot, sSlot);

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::CloudSyncFinished;
  e.m_sId = sSlot;
  e.m_bFlag = true;
  pThis->Emit(e);

  pThis->FinishCloudRequest(*pRequest, plSaveGameError::Success);
}

void EOS_CALL plEosPlatformBackend::OnCloudFileList(const EOS_PlayerDataStorage_QueryFileListCallbackInfo* pData)
{
  plEosCloudRequest* pRequest = static_cast<plEosCloudRequest*>(pData->ClientData);
  plEosPlatformBackend* pThis = pRequest->m_pBackend;

  // No files at all is an empty listing, not an error.
  if (pData->ResultCode != EOS_EResult::EOS_Success && pData->ResultCode != EOS_EResult::EOS_NotFound)
  {
    plLog::Warning("EOS: listing cloud files failed: {}", plEosResultName(pData->ResultCode));
    pThis->FinishCloudRequest(*pRequest, plSaveGameError::IoError);
    return;
  }

  plDynamicArray<plSaveGameCloudKey> keys;

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    EOS_PlayerDataStorage_GetFileMetadataCountOptions countOptions = {};
    countOptions.ApiVersion = EOS_PLAYERDATASTORAGE_GETFILEMETADATACOUNT_API_LATEST;
    countOptions.LocalUserId = pThis->m_LocalProductUserId;

    int32_t iCount = 0;
    const EOS_EResult countResult = EOS_PlayerDataStorage_GetFileMetadataCount(pThis->m_hPlayerDataStorage, &countOptions, &iCount);

    if (countResult == EOS_EResult::EOS_Success)
    {
      for (int32_t i = 0; i < iCount; ++i)
      {
        EOS_PlayerDataStorage_CopyFileMetadataAtIndexOptions copyOptions = {};
        copyOptions.ApiVersion = EOS_PLAYERDATASTORAGE_COPYFILEMETADATAATINDEX_API_LATEST;
        copyOptions.LocalUserId = pThis->m_LocalProductUserId;
        copyOptions.Index = static_cast<uint32_t>(i);

        EOS_PlayerDataStorage_FileMetadata* pMetadata = nullptr;
        const EOS_EResult copyResult = EOS_PlayerDataStorage_CopyFileMetadataAtIndex(pThis->m_hPlayerDataStorage, &copyOptions, &pMetadata);
        if (copyResult != EOS_EResult::EOS_Success || pMetadata == nullptr)
          continue;

        plSaveGameCloudKey key;
        if (pMetadata->Filename != nullptr && ParseFileName(pMetadata->Filename, pRequest->m_sProject, pRequest->m_Profile, key))
          keys.PushBack(key);

        EOS_PlayerDataStorage_FileMetadata_Release(pMetadata);
      }
    }
  }

  {
    PL_LOCK(pRequest->m_Mutex);

    if (pRequest->m_pOutKeys != nullptr)
      *pRequest->m_pOutKeys = std::move(keys);
  }

  pThis->FinishCloudRequest(*pRequest, plSaveGameError::Success);
}
