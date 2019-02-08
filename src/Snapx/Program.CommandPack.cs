using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Threading.Tasks;
using JetBrains.Annotations;
using NuGet.Packaging;
using NuGet.Versioning;
using snapx.Options;
using Snap.Core;
using Snap.Core.Models;
using Snap.Extensions;
using Snap.Logging;
using Snap.NuGet;

namespace snapx
{
    internal partial class Program
    {
        static int CommandPack([NotNull] PackOptions packOptions, [NotNull] ISnapFilesystem filesystem,
            [NotNull] ISnapAppReader appReader, ISnapAppWriter appWriter, [NotNull] INuGetPackageSources nuGetPackageSources, 
            [NotNull] ISnapPack snapPack, [NotNull] INugetService nugetService, [NotNull] ILog logger, [NotNull] string workingDirectory)
        {
            if (packOptions == null) throw new ArgumentNullException(nameof(packOptions));
            if (filesystem == null) throw new ArgumentNullException(nameof(filesystem));
            if (appReader == null) throw new ArgumentNullException(nameof(appReader));
            if (nuGetPackageSources == null) throw new ArgumentNullException(nameof(nuGetPackageSources));
            if (snapPack == null) throw new ArgumentNullException(nameof(snapPack));
            if (nugetService == null) throw new ArgumentNullException(nameof(nugetService));
            if (logger == null) throw new ArgumentNullException(nameof(logger));
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));

            var stopwatch = new Stopwatch();
            stopwatch.Restart();
            
            var (snapApps, snapApp, error, snapsManifestAbsoluteFilename) = BuildSnapAppFromDirectory(filesystem, appReader, 
                nuGetPackageSources, packOptions.AppId, packOptions.Rid, workingDirectory);
            if (snapApp == null)
            {
                if (!error)
                {
                    logger.Error($"Snap with id {packOptions.AppId} was not found in manifest: {snapsManifestAbsoluteFilename}");
                }

                return -1;
            }
                        
            snapApps.Generic.Packages = snapApps.Generic.Packages == null ?
                filesystem.PathCombine(workingDirectory, "packages") :
                filesystem.PathGetFullPath(snapApps.Generic.Packages);

            packOptions.ArtifactsDirectory =
                packOptions.ArtifactsDirectory == null ? string.Empty : 
                    filesystem.PathCombine(workingDirectory, packOptions.ArtifactsDirectory);

            filesystem.DirectoryCreateIfNotExists(snapApps.Generic.Packages);
            
            var (previousNupkgAbsolutePath, previousSnapApp) = filesystem
                .EnumerateFiles(snapApps.Generic.Packages)
                .Where(x => x.Name.EndsWith(".nupkg", StringComparison.Ordinal))
                .OrderByDescending(x => x.Name)
                .Select(x =>
                {
                    using (var coreReader = new PackageArchiveReader(x.FullName))
                    {
                        return (absolutePath: x.FullName, snapApp: snapPack.GetSnapAppAsync(coreReader).GetAwaiter().GetResult());
                    }
                })
                .Where(x => !x.snapApp.Delta)
                .OrderByDescending(x => x.snapApp.Version)
                .FirstOrDefault();

            var nuspecFilename = snapApp.Target.Nuspec == null
                ? string.Empty
                : filesystem.PathCombine(workingDirectory, snapApps.Generic.Nuspecs, snapApp.Target.Nuspec);

            if (!filesystem.FileExists(nuspecFilename))
            {
                logger.Error($"Nuspec does not exist: {nuspecFilename}");
                return -1;
            }

            if (!SemanticVersion.TryParse(packOptions.Version, out var semanticVersion))
            {
                logger.Error($"Unable to parse semantic version (v2): {packOptions.Version}");
                return -1;
            }

            if (packOptions.Force)
            {
                previousSnapApp = null;
                previousNupkgAbsolutePath = null;

                logger.Warn("Force enabled! Previous release will be overwritten.");
            }

            snapApp.Version = semanticVersion;

            var artifactsProperties = new Dictionary<string, string>
            {
                { "id", snapApp.Id },
                { "rid", snapApp.Target.Rid },
                { "version", snapApp.Version.ToNormalizedString() }
            };

            snapApps.Generic.Artifacts = snapApps.Generic.Artifacts == null ?
                null : filesystem.PathCombine(workingDirectory, 
                    snapApps.Generic.Artifacts.ExpandProperties(artifactsProperties));

            if (snapApps.Generic.Artifacts != null)
            {
                packOptions.ArtifactsDirectory = filesystem.PathCombine(workingDirectory, snapApps.Generic.Artifacts);
            }

            if (!filesystem.DirectoryExists(packOptions.ArtifactsDirectory))
            {
                logger.Error($"Artifacts directory does not exist: {packOptions.ArtifactsDirectory}");
                return -1;
            }

            logger.Info($"Packages directory: {snapApps.Generic.Packages}");
            logger.Info($"Artifacts directory {packOptions.ArtifactsDirectory}");
            logger.Info($"Pack strategy: {snapApps.Generic.PackStrategy}");
            logger.Info('-'.Repeat(TerminalWidth));
            if (previousSnapApp != null)
            {
                logger.Info($"Previous release detected: {previousSnapApp.Version}.");
                logger.Info('-'.Repeat(TerminalWidth));
            }
            logger.Info($"Id: {snapApp.Id}");
            logger.Info($"Version: {snapApp.Version}");
            logger.Info($"Channel: {snapApp.Channels.First().Name}");
            logger.Info($"Rid: {snapApp.Target.Rid}");
            logger.Info($"OS: {snapApp.Target.Os.ToString().ToLowerInvariant()}");
            logger.Info($"Nuspec: {nuspecFilename}");
                        
            logger.Info('-'.Repeat(TerminalWidth));

            var snapPackageDetails = new SnapPackageDetails
            {
                App = snapApp,
                NuspecBaseDirectory = packOptions.ArtifactsDirectory,
                NuspecFilename = nuspecFilename,
                SnapProgressSource = new SnapProgressSource()
            };

            snapPackageDetails.SnapProgressSource.Progress += (sender, percentage) =>
            {
                logger.Info($"Progress: {percentage}%.");
            };

            logger.Info($"Building full package: {snapApp.Version}.");
            var currentNupkgAbsolutePath = filesystem.PathCombine(snapApps.Generic.Packages, snapApp.BuildNugetLocalFilename());
            using (var currentNupkgStream = snapPack.BuildFullPackageAsync(snapPackageDetails, logger).GetAwaiter().GetResult())
            {
                logger.Info($"Writing nupkg: {filesystem.PathGetFileName(currentNupkgAbsolutePath)}. Final size: {currentNupkgStream.Length.BytesAsHumanReadable()}.");
                filesystem.FileWriteAsync(currentNupkgStream, currentNupkgAbsolutePath, default).GetAwaiter().GetResult();
                if (previousSnapApp == null)
                {                    
                    if (snapApps.Generic.PackStrategy == SnapAppsPackStrategy.Push)
                    {
                        PushPackages(logger, filesystem, nugetService, snapApp, 
                            currentNupkgAbsolutePath);
                    }

                    goto success;
                }
            }

            logger.Info('-'.Repeat(TerminalWidth));        
            logger.Info($"Building delta package from previous release: {previousSnapApp.Version}.");

            var deltaProgressSource = new SnapProgressSource();
            deltaProgressSource.Progress += (sender, percentage) => { logger.Info($"Progress: {percentage}%."); };

            var (deltaNupkgStream, deltaSnapApp) = snapPack.BuildDeltaPackageAsync(previousNupkgAbsolutePath, 
                currentNupkgAbsolutePath, deltaProgressSource).GetAwaiter().GetResult();
            var deltaNupkgAbsolutePath = filesystem.PathCombine(snapApps.Generic.Packages, deltaSnapApp.BuildNugetLocalFilename());
            using (deltaNupkgStream)
            {
                logger.Info($"Writing nupkg: {filesystem.PathGetFileName(currentNupkgAbsolutePath)}. Final size: {deltaNupkgStream.Length.BytesAsHumanReadable()}.");
                filesystem.FileWriteAsync(deltaNupkgStream, deltaNupkgAbsolutePath, default).GetAwaiter().GetResult();
            }

            if (snapApps.Generic.PackStrategy == SnapAppsPackStrategy.Push)
            {
                PushPackages(logger, filesystem, nugetService, snapApp, 
                    currentNupkgAbsolutePath, deltaNupkgAbsolutePath);
            }

            success:
            logger.Info('-'.Repeat(TerminalWidth));
            logger.Info($"Releasify completed in {stopwatch.Elapsed.TotalSeconds:F1}s.");
            return 0;
        }

        static void PushPackages([NotNull] ILog logger, [NotNull] ISnapFilesystem filesystem, 
            [NotNull] INugetService nugetService, [NotNull] SnapApp snapApp, [NotNull] params string[] packages)
        {
            if (logger == null) throw new ArgumentNullException(nameof(logger));
            if (filesystem == null) throw new ArgumentNullException(nameof(filesystem));
            if (nugetService == null) throw new ArgumentNullException(nameof(nugetService));
            if (snapApp == null) throw new ArgumentNullException(nameof(snapApp));
            if (packages == null) throw new ArgumentNullException(nameof(packages));
            if (packages.Length == 0) throw new ArgumentException("Value cannot be an empty collection.", nameof(packages));
            
            logger.Info('-'.Repeat(TerminalWidth));

            var pushDegreeOfParallelism = Math.Min(Environment.ProcessorCount, packages.Length);

            var channel = snapApp.Channels.First();
            var nugetSources = snapApp.BuildNugetSources();
            var packageSource = nugetSources.Items.Single(x => x.Name == channel.PushFeed.Name);

            if (channel.UpdateFeed.HasCredentials())
            {
                if (!"y|yes".AskUser("Update feed contains credentials, do you still want to publish application? [y|n]"))
                {
                    logger.Error("Publish aborted.");
                    return;
                }
            }

            if (!"y|yes".AskUser($"Ready to publish application to {packageSource.Name}. Do you want to continue? [y|n]"))
            {
                logger.Error("Publish aborted.");
                return;
            }

            var nugetLogger = new NugetLogger(logger);
            var stopwatch = new Stopwatch();
            stopwatch.Restart();

            Task PushPackageAsync(string packageAbsolutePath, long bytes)
            {
                if (packageAbsolutePath == null) throw new ArgumentNullException(nameof(packageAbsolutePath));
                if (bytes <= 0) throw new ArgumentOutOfRangeException(nameof(bytes));

                return SnapUtility.Retry(async () =>
                {
                    await nugetService.PushAsync(packageAbsolutePath, nugetSources, packageSource, nugetLogger);
                });
            }

            logger.Info($"Pushing packages to default channel: {channel.Name}. Feed: {channel.PushFeed.Name}.");

            packages.ForEachAsync(x => PushPackageAsync(x, filesystem.FileStat(x).Length), pushDegreeOfParallelism).GetAwaiter().GetResult();

            logger.Info($"Successfully pushed {packages.Length} packages in {stopwatch.Elapsed.TotalSeconds:F1}s.");
        }

    }
}