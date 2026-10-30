// #pl-version 1
//
// Builds a plugin package against an installed engine SDK rather than from inside the engine
// checkout. PLASMA_SDK_ROOT names that SDK and everything else is derived from it, so a package
// repository needs no knowledge of where the engine lives.
//
// The difference from an in-tree plugin is what it binds to. In the engine workspace a plugin
// declares dependencies on engine *targets* by name and the build system resolves them; there is no
// such workspace here, so this links the engine's import libraries directly out of the SDK's
// Binaries directory and adds its include paths by hand.

using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageSdk
{
    private static string s_Root;

    /// \brief Absolute path of the engine SDK this package is built against.
    public static string Root
    {
        get
        {
            if (s_Root != null)
            {
                return s_Root;
            }

            var root = Environment.GetEnvironmentVariable("PLASMA_SDK_ROOT");

            if (string.IsNullOrWhiteSpace(root))
            {
                throw new InvalidOperationException(
                    "PLASMA_SDK_ROOT is not set. A package is built against an installed engine SDK; " +
                    "point that variable at the engine root (the directory containing Code/ and Binaries/).");
            }

            root = Path.GetFullPath(root);

            if (!Directory.Exists(Path.Combine(root, "Code", "Engine")))
            {
                throw new DirectoryNotFoundException(
                    $"PLASMA_SDK_ROOT is '{root}', which does not look like a Plasma SDK - no Code/Engine below it.");
            }

            s_Root = root;
            return s_Root;
        }
    }

    /// \brief Where the SDK keeps the engine binaries and their import libraries.
    public static string SdkBinaryDirectory(BuildContext context)
    {
        return Path.Combine(Root, "Binaries", $"{context.Platform}_{context.Configuration}");
    }

    /// \brief Where this package's own binaries go.
    ///
    /// This layout is not arbitrary: it is what Package.sha256 records and what the publisher packs
    /// into the platform components, so a local build produces exactly the tree that gets published.
    public static string PackageBinaryDirectory(BuildContext context)
    {
        return Path.Combine(context.ProjectRoot, "Bin", $"{context.Platform}_x64_{context.Configuration}");
    }

    /// \brief Include paths, defines and engine import libraries. Every package module needs this.
    public static void ConfigureSdkConsumer(ModuleRules rules, BuildContext context)
    {
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Code", "Engine"));
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Code", "EnginePlugins"));
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Code", "EditorPlugins"));
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Code", "ThirdParty"));

        // EditorFramework and EditorEngineProcessFramework live here, and the tools libraries
        // (ToolsFoundation, GuiFoundation) here. Include paths only - linking is opt-in, so a
        // runtime-only package pays nothing for these being visible.
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Code", "Editor"));
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Code", "Tools", "Libs"));
        AddSdkThirdPartyIncludePaths(rules);

        rules.PublicDefinitions.Add("BUILDSYSTEM_COMPILE_ENGINE_AS_DLL");
        rules.PublicDefinitions.Add("BUILDSYSTEM_SDKVERSION_MAJOR=0");
        rules.PublicDefinitions.Add("BUILDSYSTEM_SDKVERSION_MINOR=11");
        rules.PublicDefinitions.Add("BUILDSYSTEM_SDKVERSION_PATCH=2");
        rules.PublicDefinitions.Add("UNICODE");
        rules.PublicDefinitions.Add("_UNICODE");

        // The configuration is part of the ABI, so it has to be declared the same way the engine
        // declared it when it was built - see plSdkVersion::GetAbiTag().
        if (context.Configuration == BuildConfiguration.Debug)
        {
            rules.PublicDefinitions.Add("BUILDSYSTEM_BUILDTYPE_Debug");
            rules.PublicDefinitions.Add("BUILDSYSTEM_BUILDTYPE=\"Debug\"");
        }
        else if (context.Configuration == BuildConfiguration.Development)
        {
            rules.PublicDefinitions.Add("BUILDSYSTEM_BUILDTYPE_Dev");
            rules.PublicDefinitions.Add("BUILDSYSTEM_BUILDTYPE=\"Dev\"");
        }
        else
        {
            rules.PublicDefinitions.Add("BUILDSYSTEM_BUILDTYPE_Shipping");
            rules.PublicDefinitions.Add("BUILDSYSTEM_BUILDTYPE=\"Shipping\"");
        }

        AddFeatureDefines(rules);

        AddImportLibrary(rules, context, "plFoundation");
        AddImportLibrary(rules, context, "plCore");
        AddImportLibrary(rules, context, "plTexture");
        AddImportLibrary(rules, context, "plRendererFoundation");
        AddImportLibrary(rules, context, "plRendererCore");
        AddImportLibrary(rules, context, "plAudioSystem");
        AddImportLibrary(rules, context, "plUtilities");
        AddImportLibrary(rules, context, "plGameEngine");

        if (context.Platform == TargetPlatform.Windows)
        {
            rules.PrivateLibraries.Add("Shell32.lib");
        }
    }

    /// \brief The additional libraries an editor-side plugin binds to.
    ///
    /// Kept separate because a runtime-only package must not drag the editor framework in.
    public static void ConfigureEditorSdkConsumer(ModuleRules rules, BuildContext context)
    {
        AddImportLibrary(rules, context, "plToolsFoundation");
        AddImportLibrary(rules, context, "plGuiFoundation");
        AddImportLibrary(rules, context, "EditorFramework");
        AddImportLibrary(rules, context, "EditorEngineProcessFramework");

        // The engine gives every editor plugin these two as well. An editor plugin that imports a
        // mesh - a collision mesh, say - reaches plModelImporter2 through them, and without them
        // fails to link on symbols that look nothing like the plugin's own code.
        AddImportLibrary(rules, context, "ModelImporter2");
        AddImportLibrary(rules, context, "HairImporter");

        // uic output for the engine's own dialogs. A plugin that embeds one - the asset browser,
        // the curve editor - includes <EditorFramework/ui_Foo.h>, which is generated rather than
        // checked in, so it only exists in an SDK that has actually been built.
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "Editor"));
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "Tools", "Libs"));
    }

    /// \brief Include root and import library for the plasma.assets built-in.
    ///
    /// A package whose editor plugin defines its own asset document type builds on
    /// EditorPluginAssets, and does so by including that plugin's PCH directly - which is what the
    /// engine's own plugins do, because the engine puts each plugin group's parent directory on the
    /// include path. Out here that root has to be named explicitly.
    public static void ConfigureAssetsBuiltIn(ModuleRules rules, BuildContext context)
    {
        rules.PublicIncludePaths.Add(Path.Combine(Root, "Code", "EditorPlugins", "Assets"));
        AddImportLibrary(rules, context, "plEditorPluginAssets");

        // SharedPluginAssets is its own DLL, and carries the editor/engine messages an
        // engine-process plugin exchanges with the editor - plEditorEngineRestartSimulationMsg and
        // friends. Including its header without linking it fails at the RTTI accessors.
        AddImportLibrary(rules, context, "plSharedPluginAssets");
    }

    /// \brief The module shape every package plugin shares: PCH, language level, export define.
    public static void ConfigurePackageModule(ModuleRules rules, BuildContext context, string sourceDirectory,
        string pchHeaderFile, string buildDefinition)
    {
        rules.Type = ModuleType.Runtime;
        rules.SourceDirectory = sourceDirectory;
        rules.PCHUsage = PCHUsage.UseExplicitOrShared;
        rules.SharedPCHHeaderFile = pchHeaderFile;
        rules.AdditionalCompilerFlags.Add($"/FI\"{Path.Combine(context.ProjectRoot, sourceDirectory, pchHeaderFile)}\"");

        rules.PublicIncludePaths.Add("..");
        rules.PrivateDefinitions.Add(buildDefinition);

        rules.CStandard = CStandard.C17;
        rules.CppStandard = CppStandard.Cpp20;
        rules.TreatWarningsAsErrors = false;

        ConfigureSdkConsumer(rules, context);
    }

    /// \brief The optional-feature defines the SDK's own binaries were compiled with.
    ///
    /// These are not cosmetic. Engine public headers branch on them, so a package that compiles
    /// without one sees a different definition of a shared type than the engine's DLL does - which
    /// is an ABI mismatch that links cleanly and crashes later, not a compile error.
    ///
    /// In the engine workspace they arrive through the module graph, from the third-party module
    /// that provides each feature. There is no graph out here, so they have to be restated. This
    /// list is the set plGameEngine was built with, which is the widest a package binds against.
    ///
    /// TODO: the engine build should emit this next to its binaries and this method should read it.
    /// A hand-maintained copy drifts silently, and the failure mode is a crash in unrelated code.
    private static void AddFeatureDefines(ModuleRules rules)
    {
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_ZSTD_SUPPORT");
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_ZLIB_SUPPORT");
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_TRACY_SUPPORT");
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_ENET_SUPPORT");
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_LUA_SUPPORT");
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_DUKTAPE_SUPPORT");
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_OZZ_SUPPORT");
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_NRD_SUPPORT");
        rules.PublicDefinitions.Add("BUILDSYSTEM_ENABLE_IMGUI_SUPPORT");
    }

    /// \brief Binds to another plugin shipped by this same package.
    ///
    /// The in-tree equivalent links out of the engine's Binaries directory. Here the sibling is
    /// built by this workspace, so it comes from the package's own output instead.
    public static void AddPackagePluginImport(ModuleRules rules, BuildContext context, string pluginSourceDirectory,
        string outputName)
    {
        rules.PublicIncludePaths.Add(Path.Combine(context.ProjectRoot, pluginSourceDirectory));

        var sourceParent = Directory.GetParent(Path.Combine(context.ProjectRoot, pluginSourceDirectory));
        if (sourceParent != null)
        {
            rules.PublicIncludePaths.Add(sourceParent.FullName);
        }

        if (context.Platform == TargetPlatform.Windows)
        {
            rules.PrivateLibraries.Add(Path.Combine(PackageBinaryDirectory(context), outputName + ".lib"));
        }
    }

    public static void AddImportLibrary(ModuleRules rules, BuildContext context, string outputName)
    {
        var library = Path.Combine(SdkBinaryDirectory(context),
            outputName + (context.Platform == TargetPlatform.Windows ? ".lib" : ".so"));

        if (File.Exists(library))
        {
            rules.PublicLibraries.Add(library);
        }
    }

    private static void AddSdkThirdPartyIncludePaths(ModuleRules rules)
    {
        var thirdPartyRoot = Path.Combine(Root, "Code", "ThirdParty");

        if (!Directory.Exists(thirdPartyRoot))
        {
            return;
        }

        foreach (var directory in Directory.EnumerateDirectories(thirdPartyRoot))
        {
            rules.PublicIncludePaths.Add(directory);
        }

        foreach (var directory in Directory.EnumerateDirectories(thirdPartyRoot, "*", SearchOption.AllDirectories))
        {
            var name = Path.GetFileName(directory);
            if (string.Equals(name, "include", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "inc", StringComparison.OrdinalIgnoreCase))
            {
                rules.PublicIncludePaths.Add(directory);
            }
        }
    }
}
