#include <EosPlugin/EosPluginPCH.h>

#include <EosPlugin/EosPluginDLL.h>

PL_STATICLINK_LIBRARY(EosPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(EosPlugin_EosPlugin);
}
