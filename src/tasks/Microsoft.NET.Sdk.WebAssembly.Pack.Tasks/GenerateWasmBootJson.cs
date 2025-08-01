// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Xml.Linq;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;
using ResourceHashesByNameDictionary = System.Collections.Generic.Dictionary<string, string>;

namespace Microsoft.NET.Sdk.WebAssembly;

public class GenerateWasmBootJson : Task
{
    private static readonly string[] jiterpreterOptions = [
        "jiterpreter-traces-enabled",
        "jiterpreter-interp-entry-enabled",
        "jiterpreter-jit-call-enabled"
    ];

    [Required]
    public string AssemblyPath { get; set; }

    [Required]
    public ITaskItem[] Resources { get; set; }

    [Required]
    public ITaskItem[] Endpoints { get; set; }

    [Required]
    public bool DebugBuild { get; set; }

    public string DebugLevel { get; set; }

    [Required]
    public bool LinkerEnabled { get; set; }

    [Required]
    public bool CacheBootResources { get; set; }

    public bool LoadFullICUData { get; set; }

    public bool LoadCustomIcuData { get; set; }

    public string InvariantGlobalization { get; set; }

    public ITaskItem[] ConfigurationFiles { get; set; }

    public ITaskItem[] EnvVariables { get; set; }

    public ITaskItem[] Extensions { get; set; }

    public string[]? Profilers { get; set; }

    public string? RuntimeConfigJsonPath { get; set; }

    public string Jiterpreter { get; set; }

    public string RuntimeOptions { get; set; }

    [Required]
    public string TargetFrameworkVersion { get; set; }

    public ITaskItem[] ModuleAfterConfigLoaded { get; set; }

    public ITaskItem[] ModuleAfterRuntimeReady { get; set; }

    [Required]
    public string OutputPath { get; set; }

    public ITaskItem[] LazyLoadedAssemblies { get; set; }

    public bool IsPublish { get; set; }

    public bool IsAot { get; set; }

    public bool IsMultiThreaded { get; set; }

    public bool FingerprintAssets { get; set; }

    public string ApplicationEnvironment { get; set; }

    public string MergeWith { get; set; }

    public bool BundlerFriendly { get; set; }

    public override bool Execute()
    {
        var entryAssemblyName = AssemblyName.GetAssemblyName(AssemblyPath).Name;

        try
        {
            WriteBootConfig(entryAssemblyName);
        }
        catch (Exception ex)
        {
            Log.LogError(ex.ToString());
        }

        return !Log.HasLoggedErrors;
    }

    private void WriteBootConfig(string entryAssemblyName)
    {
        var helper = new BootJsonBuilderHelper(Log, DebugLevel, IsMultiThreaded, IsPublish);

        var result = new BootJsonData
        {
            resources = new ResourcesData(),
        };

        if (IsTargeting100OrLater())
        {
            result.applicationEnvironment = ApplicationEnvironment;
        }

        if (IsTargeting80OrLater())
        {
            result.mainAssemblyName = entryAssemblyName;
            result.globalizationMode = GetGlobalizationMode().ToString().ToLowerInvariant();

            if (!IsTargeting100OrLater())
            {
                if (CacheBootResources)
                    result.cacheBootResources = CacheBootResources;
            }

            if (LinkerEnabled)
                result.linkerEnabled = LinkerEnabled;
        }
        else
        {
            result.cacheBootResources = CacheBootResources;
            result.linkerEnabled = LinkerEnabled;
            result.config = new();
            result.debugBuild = DebugBuild;
            result.entryAssembly = entryAssemblyName;
            result.icuDataMode = GetGlobalizationMode();
        }

        if (!string.IsNullOrEmpty(RuntimeOptions))
        {
            string[] runtimeOptions = RuntimeOptions.Split(' ');
            result.runtimeOptions = runtimeOptions;
        }

        bool? jiterpreter = helper.ParseOptionalBool(Jiterpreter);
        if (jiterpreter != null)
        {
            var runtimeOptions = result.runtimeOptions?.ToHashSet() ?? new HashSet<string>(3);
            foreach (var jiterpreterOption in jiterpreterOptions)
            {
                if (jiterpreter == true)
                {
                    if (!runtimeOptions.Contains($"--no-{jiterpreterOption}"))
                        runtimeOptions.Add($"--{jiterpreterOption}");
                }
                else
                {
                    if (!runtimeOptions.Contains($"--{jiterpreterOption}"))
                        runtimeOptions.Add($"--no-{jiterpreterOption}");
                }
            }

            result.runtimeOptions = runtimeOptions.ToArray();
        }

        string[] moduleAfterConfigLoadedFullPaths = ModuleAfterConfigLoaded?.Select(s => s.GetMetadata("FullPath")).ToArray() ?? Array.Empty<string>();
        string[] moduleAfterRuntimeReadyFullPaths = ModuleAfterRuntimeReady?.Select(s => s.GetMetadata("FullPath")).ToArray() ?? Array.Empty<string>();

        // Build a two-level dictionary of the form:
        // - assembly:
        //   - UriPath (e.g., "System.Text.Json.dll")
        //     - ContentHash (e.g., "4548fa2e9cf52986")
        // - runtime:
        //   - UriPath (e.g., "dotnet.js")
        //     - ContentHash (e.g., "3448f339acf512448")
        ResourcesData resourceData = (ResourcesData)result.resources;
        if (Resources != null)
        {
            var endpointByAsset = Endpoints.ToDictionary(e => e.GetMetadata("AssetFile"));

            var lazyLoadAssembliesWithoutExtension = (LazyLoadedAssemblies ?? Array.Empty<ITaskItem>()).ToDictionary(l =>
            {
                var extension = Path.GetExtension(l.ItemSpec);
                if (extension == ".dll" || extension == Utils.WebcilInWasmExtension)
                    return Path.GetFileNameWithoutExtension(l.ItemSpec);

                return l.ItemSpec;
            });

            var remainingLazyLoadAssemblies = new List<ITaskItem>(LazyLoadedAssemblies ?? Array.Empty<ITaskItem>());

            if (FingerprintAssets)
                resourceData.fingerprinting = new();

            foreach (var resource in Resources)
            {
                ResourceHashesByNameDictionary resourceList = null;

                string behavior = null;
                var fileName = resource.GetMetadata("FileName");
                var fileExtension = resource.GetMetadata("Extension");
                var assetTraitName = resource.GetMetadata("AssetTraitName");
                var assetTraitValue = resource.GetMetadata("AssetTraitValue");
                var resourceName = Path.GetFileName(resource.GetMetadata("OriginalItemSpec"));
                var resourceRoute = Path.GetFileName(endpointByAsset[resource.ItemSpec].ItemSpec);

                if (TryGetLazyLoadedAssembly(lazyLoadAssembliesWithoutExtension, resourceName, out var lazyLoad))
                {
                    MapFingerprintedAsset(resourceData, resourceRoute, resourceName);
                    Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as a lazy loaded assembly.", resource.ItemSpec);
                    remainingLazyLoadAssemblies.Remove(lazyLoad);
                    resourceData.lazyAssembly ??= new ResourceHashesByNameDictionary();
                    resourceList = resourceData.lazyAssembly;
                }
                else if (string.Equals("Culture", assetTraitName, StringComparison.OrdinalIgnoreCase))
                {
                    MapFingerprintedAsset(resourceData, resourceRoute, resourceName);
                    Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as satellite assembly with culture '{1}'.", resource.ItemSpec, assetTraitValue);
                    resourceData.satelliteResources ??= new Dictionary<string, ResourceHashesByNameDictionary>(StringComparer.OrdinalIgnoreCase);

                    if (!IsTargeting80OrLater())
                        resourceRoute = assetTraitValue + "/" + resourceRoute;

                    if (!resourceData.satelliteResources.TryGetValue(assetTraitValue, out resourceList))
                    {
                        resourceList = new ResourceHashesByNameDictionary();
                        resourceData.satelliteResources.Add(assetTraitValue, resourceList);
                    }
                }
                else if (string.Equals("symbol", assetTraitValue, StringComparison.OrdinalIgnoreCase))
                {
                    MapFingerprintedAsset(resourceData, resourceRoute, resourceName);
                    if (TryGetLazyLoadedAssembly(lazyLoadAssembliesWithoutExtension, fileName, out _))
                    {
                        Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as a lazy loaded symbols file.", resource.ItemSpec);
                        resourceData.lazyAssembly ??= new ResourceHashesByNameDictionary();
                        resourceList = resourceData.lazyAssembly;
                    }
                    else
                    {
                        if (IsTargeting90OrLater() && (IsAot || helper.IsCoreAssembly(resourceName)))
                        {
                            Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as core symbols file.", resource.ItemSpec);
                            resourceData.corePdb ??= new ResourceHashesByNameDictionary();
                            resourceList = resourceData.corePdb;
                        }
                        else
                        {
                            Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as symbols file.", resource.ItemSpec);
                            resourceData.pdb ??= new ResourceHashesByNameDictionary();
                            resourceList = resourceData.pdb;
                        }
                    }
                }
                else if (string.Equals("runtime", assetTraitValue, StringComparison.OrdinalIgnoreCase))
                {
                    MapFingerprintedAsset(resourceData, resourceRoute, resourceName);
                    if (IsTargeting90OrLater() && (IsAot || helper.IsCoreAssembly(resourceName)))
                    {
                        Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as core assembly.", resource.ItemSpec);
                        resourceList = resourceData.coreAssembly;
                    }
                    else
                    {
                        Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as an app assembly.", resource.ItemSpec);
                        resourceList = resourceData.assembly;
                    }
                }
                else if (string.Equals(assetTraitName, "WasmResource", StringComparison.OrdinalIgnoreCase) &&
                        string.Equals(assetTraitValue, "native", StringComparison.OrdinalIgnoreCase))
                {
                    MapFingerprintedAsset(resourceData, resourceRoute, resourceName);
                    Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as a native application resource.", resource.ItemSpec);

                    if (IsTargeting80OrLater())
                    {
                        resourceList = helper.GetNativeResourceTargetInBootConfig(result, resourceName);
                    }
                    else
                    {
                        if (fileName.StartsWith("dotnet", StringComparison.OrdinalIgnoreCase) && string.Equals(fileExtension, ".wasm", StringComparison.OrdinalIgnoreCase))
                        {
                            behavior = "dotnetwasm";
                        }

                        resourceList = resourceData.runtime ??= new();
                    }
                }
                else if (string.Equals("JSModule", assetTraitName, StringComparison.OrdinalIgnoreCase) &&
                    string.Equals(assetTraitValue, "JSLibraryModule", StringComparison.OrdinalIgnoreCase))
                {
                    Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as a library initializer resource.", resource.ItemSpec);

                    var targetPath = endpointByAsset[resource.ItemSpec].ItemSpec;
                    Debug.Assert(!string.IsNullOrEmpty(targetPath), "Target path for '{0}' must exist.", resource.ItemSpec);

                    resourceList = resourceData.libraryInitializers ??= new ResourceHashesByNameDictionary();
                    AddResourceToList(resource, resourceList, targetPath);

                    if (IsTargeting80OrLater())
                    {
                        if (moduleAfterConfigLoadedFullPaths.Contains(resource.ItemSpec))
                        {
                            resourceList = resourceData.modulesAfterConfigLoaded ??= new();
                        }
                        else if (moduleAfterRuntimeReadyFullPaths.Contains(resource.ItemSpec))
                        {
                            resourceList = resourceData.modulesAfterRuntimeReady ??= new();
                        }
                        else if (File.Exists(resource.ItemSpec))
                        {
                            string fileContent = File.ReadAllText(resource.ItemSpec);
                            if (fileContent.Contains("onRuntimeConfigLoaded") || fileContent.Contains("beforeStart") || fileContent.Contains("afterStarted"))
                                resourceList = resourceData.modulesAfterConfigLoaded ??= new();
                            else
                                resourceList = resourceData.modulesAfterRuntimeReady ??= new();
                        }
                        else
                        {
                            resourceList = resourceData.modulesAfterConfigLoaded ??= new();
                        }

                        string newTargetPath = "../" + targetPath; // This needs condition once WasmRuntimeAssetsLocation is supported in Wasm SDK
                        AddResourceToList(resource, resourceList, newTargetPath);
                    }

                    continue;
                }
                else if (string.Equals("WasmResource", assetTraitName, StringComparison.OrdinalIgnoreCase) &&
                            assetTraitValue.StartsWith("extension:", StringComparison.OrdinalIgnoreCase))
                {
                    Log.LogMessage(MessageImportance.Low, "Candidate '{0}' is defined as an extension resource '{1}'.", resource.ItemSpec, assetTraitValue);
                    var extensionName = assetTraitValue.Substring("extension:".Length);
                    resourceData.extensions ??= new();
                    if (!resourceData.extensions.TryGetValue(extensionName, out resourceList))
                    {
                        resourceList = new();
                        resourceData.extensions[extensionName] = resourceList;
                    }
                    var targetPath = endpointByAsset[resource.ItemSpec].ItemSpec;
                    Debug.Assert(!string.IsNullOrEmpty(targetPath), "Target path for '{0}' must exist.", resource.ItemSpec);
                    AddResourceToList(resource, resourceList, targetPath);
                    continue;
                }
                else
                {
                    Log.LogMessage(MessageImportance.Low, "Skipping resource '{0}' since it doesn't belong to a defined category.", resource.ItemSpec);
                    // This should include items such as XML doc files, which do not need to be recorded in the manifest.
                    continue;
                }

                if (resourceList != null)
                {
                    AddResourceToList(resource, resourceList, resourceRoute);
                }

                if (!string.IsNullOrEmpty(behavior))
                {
                    resourceData.runtimeAssets ??= new Dictionary<string, AdditionalAsset>();
                    AddToAdditionalResources(resource, resourceData.runtimeAssets, resourceRoute, behavior);
                }
            }

            if (remainingLazyLoadAssemblies.Count > 0)
            {
                const string message = "Unable to find '{0}' to be lazy loaded later. Confirm that project or " +
                    "package references are included and the reference is used in the project.";

                Log.LogError(
                    subcategory: null,
                    errorCode: "BLAZORSDK1001",
                    helpKeyword: null,
                    file: null,
                    lineNumber: 0,
                    columnNumber: 0,
                    endLineNumber: 0,
                    endColumnNumber: 0,
                    message: message,
                    string.Join(";", remainingLazyLoadAssemblies.Select(a => a.ItemSpec)));

                return;
            }
        }

        if (IsTargeting80OrLater())
        {
            result.debugLevel = helper.GetDebugLevel(resourceData.pdb?.Count > 0);
        }

        if (ConfigurationFiles != null)
        {
            foreach (var configFile in ConfigurationFiles)
            {
                string configUrl = Path.GetFileName(configFile.ItemSpec);
                if (IsTargeting80OrLater())
                {
                    result.appsettings ??= new();

                    configUrl = "../" + configUrl; // This needs condition once WasmRuntimeAssetsLocation is supported in Wasm SDK
                    result.appsettings.Add(configUrl);
                }
                else
                {
                    result.config.Add(configUrl);
                }
            }
        }


        if (EnvVariables != null && EnvVariables.Length > 0)
        {
            result.environmentVariables = new Dictionary<string, string>();
            foreach (var env in EnvVariables)
            {
                string name = env.ItemSpec;
                result.environmentVariables[name] = env.GetMetadata("Value");
            }
        }
        if (Extensions != null && Extensions.Length > 0)
        {
            result.extensions = new Dictionary<string, Dictionary<string, object>>();
            foreach (var configExtension in Extensions)
            {
                var key = configExtension.GetMetadata("key");
                using var fs = File.OpenRead(configExtension.ItemSpec);
                var config = JsonSerializer.Deserialize<Dictionary<string, object>>(fs, BootJsonBuilderHelper.JsonOptions);
                result.extensions[key] = config;
            }
        }

        if (RuntimeConfigJsonPath != null && File.Exists(RuntimeConfigJsonPath))
        {
            using var fs = File.OpenRead(RuntimeConfigJsonPath);
            var runtimeConfig = JsonSerializer.Deserialize<RuntimeConfigData>(fs, BootJsonBuilderHelper.JsonOptions);
            result.runtimeConfig = runtimeConfig;
        }

        Profilers ??= Array.Empty<string>();
        var browserProfiler = Profilers.FirstOrDefault(p => p.StartsWith("browser:"));
        if (browserProfiler != null)
        {
            result.environmentVariables ??= new();
            result.environmentVariables["DOTNET_WasmPerformanceInstrumentation"] = browserProfiler.Substring("browser:".Length);
        }

        helper.ComputeResourcesHash(result);

        string? imports = null;
        if (IsTargeting100OrLater())
            imports = helper.TransformResourcesToAssets(result, BundlerFriendly);

        helper.WriteConfigToFile(result, OutputPath, mergeWith: MergeWith, imports: imports);

        void AddResourceToList(ITaskItem resource, ResourceHashesByNameDictionary resourceList, string resourceKey)
        {
            if (!resourceList.ContainsKey(resourceKey))
            {
                Log.LogMessage(MessageImportance.Low, "Added resource '{0}' with key '{1}' to the manifest.", resource.ItemSpec, resourceKey);
                resourceList.Add(resourceKey, $"sha256-{resource.GetMetadata("Integrity")}");
            }
        }
    }

    private void MapFingerprintedAsset(ResourcesData resources, string resourceRoute, string resourceName)
    {
        if (!FingerprintAssets || !IsTargeting90OrLater())
            return;

        resources.fingerprinting[resourceRoute] = resourceName;
    }

    private GlobalizationMode GetGlobalizationMode()
    {
        if (string.Equals(InvariantGlobalization, "true", StringComparison.OrdinalIgnoreCase))
            return GlobalizationMode.Invariant;
        else if (LoadFullICUData)
            return GlobalizationMode.All;
        else if (LoadCustomIcuData)
            return GlobalizationMode.Custom;

        return GlobalizationMode.Sharded;
    }

    private void AddToAdditionalResources(ITaskItem resource, Dictionary<string, AdditionalAsset> additionalResources, string resourceName, string behavior)
    {
        if (!additionalResources.ContainsKey(resourceName))
        {
            Log.LogMessage(MessageImportance.Low, "Added resource '{0}' to the list of additional assets in the manifest.", resource.ItemSpec);
            additionalResources.Add(resourceName, new AdditionalAsset
            {
                hash = $"sha256-{resource.GetMetadata("FileHash")}",
                behavior = behavior
            });
        }
    }

    private static bool TryGetLazyLoadedAssembly(Dictionary<string, ITaskItem> lazyLoadAssembliesNoExtension, string fileName, out ITaskItem lazyLoadedAssembly)
    {
        var extension = Path.GetExtension(fileName);
        if (extension == ".dll" || extension == Utils.WebcilInWasmExtension)
            fileName = Path.GetFileNameWithoutExtension(fileName);

        return lazyLoadAssembliesNoExtension.TryGetValue(fileName, out lazyLoadedAssembly);
    }

    private Version? parsedTargetFrameworkVersion;
    private static readonly Version version80 = new Version(8, 0);
    private static readonly Version version90 = new Version(9, 0);
    private static readonly Version version100 = new Version(10, 0);

    private bool IsTargeting80OrLater()
        => IsTargetingVersionOrLater(version80);

    private bool IsTargeting90OrLater()
        => IsTargetingVersionOrLater(version90);

    private bool IsTargeting100OrLater()
        => IsTargetingVersionOrLater(version100);

    private bool IsTargetingVersionOrLater(Version version)
    {
        if (parsedTargetFrameworkVersion == null)
        {
            string tfv = TargetFrameworkVersion;
            if (tfv.StartsWith("v"))
                tfv = tfv.Substring(1);

            parsedTargetFrameworkVersion = Version.Parse(tfv);
        }

        return parsedTargetFrameworkVersion >= version;
    }
}
