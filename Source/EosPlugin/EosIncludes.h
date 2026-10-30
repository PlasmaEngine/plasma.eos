#pragma once

// The EOS SDK headers are C and lean on their own warning settings; keep them out of ours.
#if PL_ENABLED(PL_COMPILER_MSVC)
#  pragma warning(push)
#  pragma warning(disable : 4100 4201 4244 4365 4668 5039)
#endif

#include <eos_sdk.h>

#include <eos_achievements.h>
#include <eos_auth.h>
#include <eos_connect.h>
#include <eos_ecom.h>
#include <eos_friends.h>
#include <eos_init.h>
#include <eos_leaderboards.h>
#include <eos_lobby.h>
#include <eos_logging.h>
#include <eos_p2p.h>
#include <eos_playerdatastorage.h>
#include <eos_presence.h>
#include <eos_rtc.h>
#include <eos_rtc_audio.h>
#include <eos_stats.h>
#include <eos_ui.h>
#include <eos_userinfo.h>

#if PL_ENABLED(PL_COMPILER_MSVC)
#  pragma warning(pop)
#endif
