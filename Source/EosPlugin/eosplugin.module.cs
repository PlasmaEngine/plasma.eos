using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EosPluginModule : ModuleRules
{
    public EosPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EosPlugin", "EosPluginPCH.h",
            "BUILDSYSTEM_BUILDING_EOSPLUGIN_LIB");

        // Headers and import library from the developer's own EOS SDK download. No EOS binaries
        // are copied into this package - see Configs/PlasmaPackageEos.pb.cs.
        PlasmaPackageEos.ConfigureConsumer(this, context);
    }
}
