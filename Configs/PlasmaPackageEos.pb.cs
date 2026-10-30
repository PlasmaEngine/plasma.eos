// #pl-version 1
//
// Locates an Epic Online Services SDK to build against.
//
// This package deliberately ships no EOS binaries. The EOS SDK is distributed under Epic's own
// licence through the Epic Developer Portal and the package registry is public, so the package
// carries Plasma's plugin code only; the SDK comes from the developer's own download.
//
// Consequence at runtime: plEosPlugin.dll imports from EOSSDK-Win64-Shipping.dll, so it will not
// load unless that DLL sits beside the executable. That is stated in the package description rather
// than left to be discovered as a load failure.

using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageEos
{
    /// \brief Finds the SDK root: the directory that contains the "SDK" folder of an EOS SDK download.
    public static bool TryGetSdk(out string sdkRoot, out string reason)
    {
        sdkRoot = string.Empty;
        reason = string.Empty;

        var fromEnv = Environment.GetEnvironmentVariable("EOS_SDK_PATH");
        if (string.IsNullOrWhiteSpace(fromEnv))
        {
            reason = "EOS_SDK_PATH is not set. Point it at the root of an EOS SDK download - the directory " +
                     "that contains SDK/Include/eos_sdk.h. This package ships no EOS binaries by design.";
            return false;
        }

        fromEnv = Path.GetFullPath(fromEnv.Trim());

        if (HasSdkFiles(fromEnv))
        {
            sdkRoot = fromEnv;
            return true;
        }

        // A path that points one level too deep (at the SDK folder itself) is a common slip.
        var parent = Directory.GetParent(fromEnv);
        if (parent != null && HasSdkFiles(parent.FullName))
        {
            sdkRoot = parent.FullName;
            return true;
        }

        reason = $"EOS_SDK_PATH is '{fromEnv}', but no EOS SDK was found there. It should name the directory " +
                 "containing SDK/Include/eos_sdk.h, SDK/Lib/EOSSDK-Win64-Shipping.lib and SDK/Bin/EOSSDK-Win64-Shipping.dll.";
        return false;
    }

    /// \brief Include path and import library. Deliberately copies no runtime DLL.
    public static void ConfigureConsumer(ModuleRules rules, BuildContext context)
    {
        if (context.Platform != TargetPlatform.Windows)
        {
            throw new PlatformNotSupportedException("The EOS plugin is only supported on Windows desktop.");
        }

        if (!TryGetSdk(out var sdkRoot, out var reason))
        {
            throw new DirectoryNotFoundException(reason);
        }

        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "SDK", "Include"));

        // Import library only. The matching DLL is the developer's to place beside the game.
        rules.PublicLibraries.Add(Path.Combine(sdkRoot, "SDK", "Lib", "EOSSDK-Win64-Shipping.lib"));
    }

    private static bool HasSdkFiles(string sdkRoot)
    {
        return File.Exists(Path.Combine(sdkRoot, "SDK", "Include", "eos_sdk.h")) &&
               File.Exists(Path.Combine(sdkRoot, "SDK", "Lib", "EOSSDK-Win64-Shipping.lib"));
    }
}
