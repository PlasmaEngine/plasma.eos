using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EosPluginTarget : TargetRules
{
    public EosPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEosPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        ExtraModules.Add("EosPluginModule");
    }
}
