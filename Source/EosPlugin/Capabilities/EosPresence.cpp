#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>

PL_DEFINE_AS_POD_TYPE(EOS_Presence_DataRecord);
PL_DEFINE_AS_POD_TYPE(EOS_PresenceModification_DataRecordId);

// Presence is cached locally and published as one modification per change burst: Set/SetJoinString/Clear mark it
// dirty, Tick publishes when nothing is in flight. The key "status" is the rich text line the overlay shows;
// every other key is a data record. EOS allows 32 records, keys up to 64 and values up to 255 bytes.

plResult plEosPlatformBackend::Set(plStringView sKey, plStringView sValue)
{
  if (m_hPresence == nullptr || sKey.IsEmpty())
    return PL_FAILURE;

  if (sKey.GetElementCount() > EOS_PRESENCE_DATA_MAX_KEY_LENGTH || sValue.GetElementCount() > EOS_PRESENCE_DATA_MAX_VALUE_LENGTH)
  {
    plLog::Warning("EOS: presence key '{}' or its value is too long.", sKey);
    return PL_FAILURE;
  }

  const plString sKeyStr(sKey);

  // An empty value removes the key; the deletion goes out with the next publish.
  if (sValue.IsEmpty())
  {
    if (m_PresenceData.Remove(sKeyStr))
    {
      if (!m_PresenceRemovedKeys.Contains(sKeyStr))
        m_PresenceRemovedKeys.PushBack(sKeyStr);

      m_bPresenceDirty = true;
    }

    return PL_SUCCESS;
  }

  if (m_PresenceData.GetCount() >= EOS_PRESENCE_DATA_MAX_KEYS && !m_PresenceData.Contains(sKeyStr))
  {
    plLog::Warning("EOS: presence already holds {} keys, '{}' is dropped.", EOS_PRESENCE_DATA_MAX_KEYS, sKey);
    return PL_FAILURE;
  }

  m_PresenceData[sKeyStr] = sValue;
  m_PresenceRemovedKeys.RemoveAndCopy(sKeyStr);
  m_bPresenceDirty = true;

  return PL_SUCCESS;
}

plResult plEosPlatformBackend::SetJoinString(plStringView sConnect)
{
  if (m_hPresence == nullptr)
    return PL_FAILURE;

  if (sConnect.GetElementCount() > EOS_PRESENCEMODIFICATION_JOININFO_MAX_LENGTH)
    return PL_FAILURE;

  m_sJoinString = sConnect;
  m_bPresenceDirty = true;

  return PL_SUCCESS;
}

void plEosPlatformBackend::Clear()
{
  for (auto it = m_PresenceData.GetIterator(); it.IsValid(); ++it)
  {
    if (!m_PresenceRemovedKeys.Contains(it.Key()))
      m_PresenceRemovedKeys.PushBack(it.Key());
  }

  m_PresenceData.Clear();
  m_sJoinString.Clear();
  m_bPresenceDirty = true;
}

plResult plEosPlatformBackend::PublishPresence()
{
  if (m_hPresence == nullptr || !EOS_EpicAccountId_IsValid(m_LocalEpicAccountId))
    return PL_FAILURE;

  EOS_Presence_CreatePresenceModificationOptions createOptions = {};
  createOptions.ApiVersion = EOS_PRESENCE_CREATEPRESENCEMODIFICATION_API_LATEST;
  createOptions.LocalUserId = m_LocalEpicAccountId;

  EOS_HPresenceModification hModification = nullptr;
  const EOS_EResult createResult = EOS_Presence_CreatePresenceModification(m_hPresence, &createOptions, &hModification);
  if (createResult != EOS_EResult::EOS_Success || hModification == nullptr)
  {
    plLog::Warning("EOS: creating the presence modification failed: {}", plEosResultName(createResult));
    m_bPresenceDirty = false;
    return PL_FAILURE;
  }

  EOS_PresenceModification_SetStatusOptions statusOptions = {};
  statusOptions.ApiVersion = EOS_PRESENCEMODIFICATION_SETSTATUS_API_LATEST;
  statusOptions.Status = EOS_Presence_EStatus::EOS_PS_Online;

  EOS_PresenceModification_SetStatus(hModification, &statusOptions);

  plHybridArray<EOS_Presence_DataRecord, 8> records;
  const plString* pRichText = nullptr;

  for (auto it = m_PresenceData.GetIterator(); it.IsValid(); ++it)
  {
    if (it.Key() == "status")
    {
      pRichText = &it.Value();
      continue;
    }

    EOS_Presence_DataRecord& record = records.ExpandAndGetRef();
    record.ApiVersion = EOS_PRESENCE_DATARECORD_API_LATEST;
    record.Key = it.Key().GetData();
    record.Value = it.Value().GetData();
  }

  if (pRichText != nullptr)
  {
    EOS_PresenceModification_SetRawRichTextOptions textOptions = {};
    textOptions.ApiVersion = EOS_PRESENCEMODIFICATION_SETRAWRICHTEXT_API_LATEST;
    textOptions.RichText = pRichText->GetData();

    EOS_PresenceModification_SetRawRichText(hModification, &textOptions);
  }

  if (!records.IsEmpty())
  {
    EOS_PresenceModification_SetDataOptions dataOptions = {};
    dataOptions.ApiVersion = EOS_PRESENCEMODIFICATION_SETDATA_API_LATEST;
    dataOptions.RecordsCount = records.GetCount();
    dataOptions.Records = records.GetData();

    EOS_PresenceModification_SetData(hModification, &dataOptions);
  }

  plHybridArray<EOS_PresenceModification_DataRecordId, 8> removed;

  for (const plString& sKey : m_PresenceRemovedKeys)
  {
    if (sKey == "status")
      continue;

    EOS_PresenceModification_DataRecordId& id = removed.ExpandAndGetRef();
    id.ApiVersion = EOS_PRESENCEMODIFICATION_DATARECORDID_API_LATEST;
    id.Key = sKey.GetData();
  }

  if (!removed.IsEmpty())
  {
    EOS_PresenceModification_DeleteDataOptions deleteOptions = {};
    deleteOptions.ApiVersion = EOS_PRESENCEMODIFICATION_DELETEDATA_API_LATEST;
    deleteOptions.RecordsCount = removed.GetCount();
    deleteOptions.Records = removed.GetData();

    EOS_PresenceModification_DeleteData(hModification, &deleteOptions);
  }

  // "status" is not a data record, so removing it clears the rich text instead.
  if (pRichText == nullptr && m_PresenceRemovedKeys.Contains(plString("status")))
  {
    EOS_PresenceModification_SetRawRichTextOptions textOptions = {};
    textOptions.ApiVersion = EOS_PRESENCEMODIFICATION_SETRAWRICHTEXT_API_LATEST;
    textOptions.RichText = "";

    EOS_PresenceModification_SetRawRichText(hModification, &textOptions);
  }

  EOS_PresenceModification_SetJoinInfoOptions joinOptions = {};
  joinOptions.ApiVersion = EOS_PRESENCEMODIFICATION_SETJOININFO_API_LATEST;
  joinOptions.JoinInfo = m_sJoinString.IsEmpty() ? nullptr : m_sJoinString.GetData();

  EOS_PresenceModification_SetJoinInfo(hModification, &joinOptions);

  plEosCall* pCall = plEosNewCall(this, "Presence.Set");

  EOS_Presence_SetPresenceOptions setOptions = {};
  setOptions.ApiVersion = EOS_PRESENCE_SETPRESENCE_API_LATEST;
  setOptions.LocalUserId = m_LocalEpicAccountId;
  setOptions.PresenceModificationHandle = hModification;

  EOS_Presence_SetPresence(m_hPresence, &setOptions, pCall, &OnSetPresence);
  EOS_PresenceModification_Release(hModification);

  m_PresenceRemovedKeys.Clear();
  m_bPresenceDirty = false;
  m_bPresenceInFlight = true;

  return PL_SUCCESS;
}

void EOS_CALL plEosPlatformBackend::OnSetPresence(const EOS_Presence_SetPresenceCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;

  pThis->m_bPresenceInFlight = false;

  if (pData->ResultCode != EOS_EResult::EOS_Success)
    plLog::Warning("EOS: publishing presence failed: {}", plEosResultName(pData->ResultCode));

  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

void EOS_CALL plEosPlatformBackend::OnJoinGameAccepted(const EOS_Presence_JoinGameAcceptedCallbackInfo* pData)
{
  plEosPlatformBackend* pThis = static_cast<plEosPlatformBackend*>(pData->ClientData);

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::JoinRequested;
  e.m_sText = pData->JoinInfo != nullptr ? pData->JoinInfo : "";

  // The friend whose presence was clicked, when they are in the list.
  for (const Friend& f : pThis->m_Friends)
  {
    if (f.m_EpicId == pData->TargetUserId)
    {
      e.m_User = f.m_User.m_Id;
      break;
    }
  }

  pThis->Emit(e);

  // Also kept as the launch request in case the game is still booting and nobody listened yet.
  pThis->m_LaunchJoin = plPlatformJoinRequest();
  pThis->m_LaunchJoin.m_From = e.m_User;
  pThis->m_LaunchJoin.m_sConnectString = e.m_sText;
  pThis->m_bLaunchJoin = true;

  if (pThis->m_hUI != nullptr && pData->UiEventId != EOS_UI_EVENTID_INVALID)
  {
    EOS_UI_AcknowledgeEventIdOptions ackOptions = {};
    ackOptions.ApiVersion = EOS_UI_ACKNOWLEDGEEVENTID_API_LATEST;
    ackOptions.UiEventId = pData->UiEventId;
    ackOptions.Result = EOS_EResult::EOS_Success;

    EOS_UI_AcknowledgeEventId(pThis->m_hUI, &ackOptions);
  }
}
