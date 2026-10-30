#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPlatformBackend.h>
#include <EosPlugin/EosPluginDLL.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/Configuration/Startup.h>
#include <GameEngine/PlatformServices/PlatformServices.h>

// The backend registers itself under plPlatformBackend from its constructor; the hub picks it up by id when
// PlatformServices.ddl lists "eos". Tool processes (editor, engine process, command-line tools) never get a
// store backend, so nothing is created there.
static plEosPlatformBackend* s_pEosBackend = nullptr;

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(Eos, EosPlugin)

  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation",
    "Core"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    if (plPlatformServices::IsToolProcess())
    {
      plLog::Dev("EOS: tool process, backend not registered.");
      return;
    }

    s_pEosBackend = PL_DEFAULT_NEW(plEosPlatformBackend);
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    if (s_pEosBackend != nullptr)
    {
      PL_DEFAULT_DELETE(s_pEosBackend);
      s_pEosBackend = nullptr;
    }
  }

  ON_HIGHLEVELSYSTEMS_STARTUP
  {
  }

  ON_HIGHLEVELSYSTEMS_SHUTDOWN
  {
  }

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on

PL_PLUGIN_ON_LOADED()
{
}

PL_PLUGIN_ON_UNLOADED()
{
}

PL_STATICLINK_FILE(EosPlugin, EosPlugin_EosPlugin);
