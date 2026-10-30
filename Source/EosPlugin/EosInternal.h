#pragma once

#include <EosPlugin/EosIncludes.h>
#include <GameEngine/PlatformServices/PlatformOperation.h>

class plEosPlatformBackend;

/// Heap context handed to every EOS request through ClientData. It retains the operation until the SDK answered,
/// so dropping the handle on the game side never leaves a callback with a dead pointer. The completing callback
/// frees it.
struct plEosCall
{
  plEosPlatformBackend* m_pBackend = nullptr;
  plPlatformOperationPtr m_pOp;
  plString m_sArg;          ///< Whatever the completing callback needs: an id, a file name, a room.
  plUInt32 m_uiPending = 0; ///< Sub-requests still outstanding in a chained call.
  plInt64 m_iNumber = 0;
  void* m_pUser = nullptr;
};

plPlatformError plEosResultToError(EOS_EResult result);
const char* plEosResultName(EOS_EResult result);

plString plEosProductUserIdToString(EOS_ProductUserId id);
plString plEosEpicAccountIdToString(EOS_EpicAccountId id);

/// The backend id of a canonical user: "eos:<product user id>". Returns null for ids of another backend.
EOS_ProductUserId plEosProductUserIdFromUser(const plPlatformUserId& user);
plPlatformUserId plEosMakeUserId(EOS_ProductUserId id);

/// Allocates the context for an operation of the given kind.
plEosCall* plEosNewCall(plEosPlatformBackend* pBackend, plStringView sKind);

/// Completes the operation from the result code and frees the context. Fits every callback info that carries
/// ResultCode and ClientData and needs nothing else.
template <typename CallbackInfo>
void EOS_CALL plEosCompleteSimple(const CallbackInfo* pData)
{
  plEosCall* pCall = static_cast<plEosCall*>(pData->ClientData);
  pCall->m_pOp->Complete(plEosResultToError(pData->ResultCode));
  PL_DEFAULT_DELETE(pCall);
}

/// The value after a "-Name=" launch token. The Epic launcher passes "-AUTH_PASSWORD=<code>" as one token, which
/// the option parser does not split.
plString plEosFindLaunchArgument(plStringView sPrefix);

/// Fills an EOS lobby attribute: integer literals become INT64 so numeric filters compare as numbers, everything
/// else is a string. The value pointer must outlive the call that consumes the attribute.
void plEosFillLobbyAttribute(EOS_Lobby_AttributeData& out_data, const char* szKey, const char* szValue, bool bNumeric);
