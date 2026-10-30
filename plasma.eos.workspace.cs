// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaEosWorkspace : WorkspaceRules
{
    public PlasmaEosWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("EosPlugin");
    }
}
