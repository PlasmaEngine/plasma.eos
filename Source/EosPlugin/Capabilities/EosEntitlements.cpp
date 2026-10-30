#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>
#include <GameEngine/PlatformServices/PlatformDefinitions.h>

// Ownership is asked from the Epic catalogue: the "AppItemId" setting names the base game's catalogue item, and
// every DLC definition's "eos" backend id is a catalogue item too. Without an AppItemId the base game counts as
// owned once the user is signed in, which is what a launcher-distributed build can rely on.

bool plEosPlatformBackend::OwnsApp() const
{
  if (!m_bOwnershipKnown)
    return m_bSignedIn;

  return m_bOwnsApp;
}

bool plEosPlatformBackend::OwnsDlc(plStringView sBackendId) const
{
  if (sBackendId.IsEmpty())
    return false;

  for (const plString& sItem : m_OwnedItems)
  {
    if (sItem == sBackendId)
      return true;
  }

  return false;
}

plPlatformOperationPtr plEosPlatformBackend::Refresh()
{
  if (plPlatformOperationPtr pGate = GateSignedIn("Entitlements.Refresh"))
    return pGate;

  if (!EOS_EpicAccountId_IsValid(m_LocalEpicAccountId))
    return plPlatformOperation::CreateFinished("Entitlements.Refresh", plPlatformError::Unsupported);

  plDynamicArray<plString>* pItems = PL_DEFAULT_NEW(plDynamicArray<plString>);

  const plString sAppItem = GetSetting("AppItemId");
  if (!sAppItem.IsEmpty())
    pItems->PushBack(sAppItem);

  for (const plPlatformDlcDef& dlc : m_pHost->GetDefinitions().m_Dlcs)
  {
    const plStringView sItem = dlc.m_Backend.Resolve("eos", dlc.m_sId);
    if (!sItem.IsEmpty() && !pItems->Contains(plString(sItem)))
      pItems->PushBack(sItem);
  }

  // No catalogue items configured: the base game counts as owned.
  if (pItems->IsEmpty())
  {
    PL_DEFAULT_DELETE(pItems);
    m_bOwnershipKnown = true;
    m_bOwnsApp = true;

    plPlatformOperationPtr pOp = plPlatformOperation::Create("Entitlements.Refresh");
    pOp->MutableResult().m_bFlag = true;
    pOp->Succeed();
    return pOp;
  }

  plHybridArray<const char*, 32> itemPointers;
  const plUInt32 uiMax = plMath::Min<plUInt32>(pItems->GetCount(), EOS_ECOM_QUERYOWNERSHIP_MAX_CATALOG_IDS);

  for (plUInt32 i = 0; i < uiMax; ++i)
    itemPointers.PushBack((*pItems)[i].GetData());

  plEosCall* pCall = plEosNewCall(this, "Entitlements.Refresh");
  pCall->m_pUser = pItems;
  pCall->m_sArg = sAppItem;

  const plString sNamespace = GetSetting("CatalogNamespace");

  EOS_Ecom_QueryOwnershipOptions options = {};
  options.ApiVersion = EOS_ECOM_QUERYOWNERSHIP_API_LATEST;
  options.LocalUserId = m_LocalEpicAccountId;
  options.CatalogItemIds = itemPointers.GetData();
  options.CatalogItemIdCount = itemPointers.GetCount();
  options.CatalogNamespace = sNamespace.IsEmpty() ? nullptr : sNamespace.GetData();

  EOS_Ecom_QueryOwnership(m_hEcom, &options, pCall, &OnQueryOwnership);

  return pCall->m_pOp;
}

void EOS_CALL plEosPlatformBackend::OnQueryOwnership(const EOS_Ecom_QueryOwnershipCallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  plEosPlatformBackend* pThis = pCall->m_pBackend;
  plDynamicArray<plString>* pItems = static_cast<plDynamicArray<plString>*>(pCall->m_pUser);

  if (pData->ResultCode == EOS_EResult::EOS_Success)
  {
    pThis->m_OwnedItems.Clear();

    for (uint32_t i = 0; i < pData->ItemOwnershipCount; ++i)
    {
      const EOS_Ecom_ItemOwnership& ownership = pData->ItemOwnership[i];
      if (ownership.OwnershipStatus == EOS_EOwnershipStatus::EOS_OS_Owned && ownership.Id != nullptr)
        pThis->m_OwnedItems.PushBack(ownership.Id);
    }

    pThis->m_bOwnershipKnown = true;
    pThis->m_bOwnsApp = pCall->m_sArg.IsEmpty() || pThis->m_OwnedItems.Contains(pCall->m_sArg);

    plPlatformEvent e;
    e.m_Type = plPlatformEvent::Type::EntitlementsChanged;
    pThis->Emit(e);
  }
  else
  {
    plLog::Warning("EOS: ownership query failed: {}", plEosResultName(pData->ResultCode));
  }

  pCall->m_pOp->MutableResult().m_bFlag = pThis->OwnsApp();
  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));

  PL_DEFAULT_DELETE(pItems);
  PL_DEFAULT_DELETE(pCall);
}
