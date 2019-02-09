﻿using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using JetBrains.Annotations;
using Mono.Cecil;
using NuGet.Configuration;
using NuGet.Versioning;
using Snap.Core;
using Snap.Core.IO;
using Snap.Core.Models;
using Snap.Core.Resources;
using Snap.NuGet;
using Snap.Reflection;
using Snap.Shared.Tests.Extensions;

namespace Snap.Shared.Tests
{
    [SuppressMessage("ReSharper", "UnusedMember.Global")]
    [SuppressMessage("ReSharper", "MemberCanBePrivate.Global")]
    [SuppressMessage("ReSharper", "MemberCanBeMadeStatic.Global")]
    [UsedImplicitly]
    public class BaseFixture
    {
        static readonly Random RandomSource = new Random();
        
        public string WorkingDirectory => Directory.GetCurrentDirectory();
        public string NugetTempDirectory => Path.Combine(WorkingDirectory, "nuget");

        public SnapApp BuildSnapApp(string appId = "demoapp")
        {
            var pushFeed = new SnapNugetFeed
            {
                Name = "nuget.org",
                Source = new Uri(NuGetConstants.V3FeedUrl),
                ProtocolVersion = NuGetProtocolVersion.V3,
                ApiKey = "myapikey"
            };

            var updateFeedNuget = new SnapNugetFeed
            {
                Name = "nuget.org",
                Source = new Uri(NuGetConstants.V3FeedUrl),
                ProtocolVersion = NuGetProtocolVersion.V3,
                Username = "myusername",
                Password = "mypassword"
            };

            var updateFeedHttp = new SnapHttpFeed
            {
                Source = new Uri("https://mydynamicupdatefeed.com")
            };

            var testChannel = new SnapChannel
            {
                Name = "test",
                PushFeed = pushFeed,
                UpdateFeed = updateFeedNuget,
                Current = true
            };

            var stagingChannel = new SnapChannel
            {
                Name = "staging",
                PushFeed = pushFeed,
                UpdateFeed = updateFeedHttp
            };

            var productionChannel = new SnapChannel
            {
                Name = "production",
                PushFeed = pushFeed,
                UpdateFeed = updateFeedNuget
            };

            OSPlatform osPlatform;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                osPlatform = OSPlatform.Windows;
            } else if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            {
                osPlatform = OSPlatform.Linux;
            }
            else
            {
                throw new NotSupportedException();
            }
            
            return new SnapApp
            {
                Id = appId,
                Version = new SemanticVersion(1, 0, 0),
                Channels = new List<SnapChannel>
                {
                    testChannel,
                    stagingChannel,
                    productionChannel
                },
                Target = new SnapTarget
                {
                    Os = osPlatform,
                    Framework = "netcoreapp2.1",
                    Rid = "unknown-x64",
                    Nuspec = "test.nuspec"
                },
                PersistentAssets = new List<string>
                {
                    "application.json"
                }
            };
        }

        public SnapApps BuildSnapApps()
        {
            var snapApp = BuildSnapApp();

            return new SnapApps
            {
                Channels = snapApp.Channels.Select(x => new SnapsChannel(x)).ToList(),
                Apps = new List<SnapsApp> { new SnapsApp(snapApp) },
                Generic = new SnapAppsGeneric
                {
                    Nuspecs = "snap/nuspecs",
                    Packages = "snap/packages"
                }
            };
        }

        public void WriteAssemblies(string workingDirectory, List<AssemblyDefinition> assemblyDefinitions, bool disposeAssemblyDefinitions = false)
        {
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));
            if (assemblyDefinitions == null) throw new ArgumentNullException(nameof(assemblyDefinitions));

            foreach (var assemblyDefinition in assemblyDefinitions)
            {
                assemblyDefinition.Write(Path.Combine(workingDirectory, assemblyDefinition.BuildRelativeFilename()));

                if (disposeAssemblyDefinitions)
                {
                    assemblyDefinition.Dispose();
                }
            }
        }

        public void WriteAssemblies(string workingDirectory, bool disposeAssemblyDefinitions = false, params AssemblyDefinition[] assemblyDefinitions)
        {
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));
            if (assemblyDefinitions == null) throw new ArgumentNullException(nameof(assemblyDefinitions));

            WriteAssemblies(workingDirectory, assemblyDefinitions.ToList(), disposeAssemblyDefinitions);
        }

        public void WriteAndDisposeAssemblies(string workingDirectory, params AssemblyDefinition[] assemblyDefinitions)
        {
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));
            if (assemblyDefinitions == null) throw new ArgumentNullException(nameof(assemblyDefinitions));

            WriteAssemblies(workingDirectory, assemblyDefinitions.ToList(), true);
        }

        internal IDisposable WithDisposableAssemblies(string workingDirectory, [NotNull] ISnapFilesystem filesystem, params AssemblyDefinition[] assemblyDefinitions)
        {
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));
            if (filesystem == null) throw new ArgumentNullException(nameof(filesystem));
            if (assemblyDefinitions == null) throw new ArgumentNullException(nameof(assemblyDefinitions));

            WriteAndDisposeAssemblies(workingDirectory, assemblyDefinitions);

            return new DisposableFiles(filesystem, assemblyDefinitions.Select(x => x.GetFullPath(filesystem, workingDirectory)).ToArray());
        }

        public AssemblyDefinition BuildEmptyLibrary(string libraryName, bool randomVersion = false, IReadOnlyCollection<AssemblyDefinition> references = null)
        {
            if (libraryName == null) throw new ArgumentNullException(nameof(libraryName));
            
            var version = randomVersion ? new Version(
                RandomSource.Next(0, 1000), 
                RandomSource.Next(0, 1000), 
                RandomSource.Next(0, 1000), 
                RandomSource.Next(0, 1000)) : 
                new Version(1, 0, 0, 0);
            
            var assembly = AssemblyDefinition.CreateAssembly(
                new AssemblyNameDefinition(libraryName, version), libraryName, ModuleKind.Dll);

            var mainModule = assembly.MainModule;

            if (references == null)
            {
                return assembly;
            }

            foreach (var assemblyDefinition in references)
            {
                mainModule.AssemblyReferences.Add(assemblyDefinition.Name);
            }

            return assembly;
        }

        public AssemblyDefinition BuildEmptyExecutable(string applicationName, bool randomVersion = false, IReadOnlyCollection<AssemblyDefinition> references = null)
        {
            if (applicationName == null) throw new ArgumentNullException(nameof(applicationName));

            var version = randomVersion ? new Version(
                    RandomSource.Next(0, 1000), 
                    RandomSource.Next(0, 1000), 
                    RandomSource.Next(0, 1000), 
                    RandomSource.Next(0, 1000)) : 
                new Version(1, 0, 0, 0);
            
            var assembly = AssemblyDefinition.CreateAssembly(
                new AssemblyNameDefinition(applicationName, version), applicationName, ModuleKind.Console);

            var mainModule = assembly.MainModule;

            if (references == null)
            {
                return assembly;
            }

            foreach (var assemblyDefinition in references)
            {
                mainModule.AssemblyReferences.Add(assemblyDefinition.Name);
            }

            return assembly;
        }

        public AssemblyDefinition BuildSnapAwareEmptyExecutable([NotNull] SnapApp snapApp, bool randomVersion = false, IReadOnlyCollection<AssemblyDefinition> references = null)
        {
            if (snapApp == null) throw new ArgumentNullException(nameof(snapApp));
            var testExeAssemblyDefinition = BuildEmptyExecutable(snapApp.Id, randomVersion, references);
            var testExeAssemblyDefinitionReflector = new CecilAssemblyReflector(testExeAssemblyDefinition);
            testExeAssemblyDefinitionReflector.SetSnapAware();
            return testExeAssemblyDefinition;
        }

        public AssemblyDefinition BuildLibrary(string libraryName, string className, IReadOnlyCollection<AssemblyDefinition> references = null)
        {
            if (libraryName == null) throw new ArgumentNullException(nameof(libraryName));
            if (className == null) throw new ArgumentNullException(nameof(className));

            var assembly = AssemblyDefinition.CreateAssembly(
                new AssemblyNameDefinition(libraryName, new Version(1, 0, 0, 0)), libraryName, ModuleKind.Dll);

            var mainModule = assembly.MainModule;

            var simpleClass = new TypeDefinition(libraryName, className,
                TypeAttributes.Class | TypeAttributes.Public, mainModule.TypeSystem.Object);

            mainModule.Types.Add(simpleClass);

            if (references == null)
            {
                return assembly;
            }

            foreach (var assemblyDefinition in references)
            {
                mainModule.AssemblyReferences.Add(assemblyDefinition.Name);
            }

            return assembly;
        }

        internal async Task<(MemoryStream memoryStream, SnapPackageDetails packageDetails)> BuildInMemoryPackageAsync(
            [NotNull] SnapApp snapApp, [NotNull] ISnapFilesystem filesystem, [NotNull] ISnapPack snapPack, [NotNull] ISnapEmbeddedResources snapEmbeddedResources, [NotNull] Dictionary<string, AssemblyDefinition> nuspecFilesLayout, 
            ISnapProgressSource progressSource = null, CancellationToken cancellationToken = default)
        {
            if (snapApp == null) throw new ArgumentNullException(nameof(snapApp));
            if (filesystem == null) throw new ArgumentNullException(nameof(filesystem));
            if (snapPack == null) throw new ArgumentNullException(nameof(snapPack));
            if (snapEmbeddedResources == null) throw new ArgumentNullException(nameof(snapEmbeddedResources));
            if (nuspecFilesLayout == null) throw new ArgumentNullException(nameof(nuspecFilesLayout));

            var (coreRunMemoryStream, _, _) = snapEmbeddedResources.GetCoreRunForSnapApp(snapApp);
            coreRunMemoryStream.Dispose();
            
            const string nuspecContent = @"<?xml version=""1.0""?>
<package xmlns=""http://schemas.microsoft.com/packaging/2010/07/nuspec.xsd"">
    <metadata>
        <title>Random Title</title>
        <authors>Peter Rekdal Sunde</authors>
    </metadata>
</package>";

            using (var tempDirectory = new DisposableTempDirectory(WorkingDirectory, filesystem))
            {
                var snapPackDetails = new SnapPackageDetails
                {
                    NuspecFilename = Path.Combine(tempDirectory.WorkingDirectory, "test.nuspec"),
                    NuspecBaseDirectory = tempDirectory.WorkingDirectory,
                    SnapProgressSource = progressSource,
                    App = snapApp
                };

                foreach (var pair in nuspecFilesLayout)
                {
                    var dstFilename = filesystem.PathCombine(snapPackDetails.NuspecBaseDirectory, pair.Key);
                    var directory = filesystem.PathGetDirectoryName(dstFilename);
                    filesystem.DirectoryCreateIfNotExists(directory);
                    pair.Value.Write(dstFilename);                
                }

                await filesystem.FileWriteUtf8StringAsync(nuspecContent, snapPackDetails.NuspecFilename, cancellationToken);

                var nupkgMemoryStream = await snapPack.BuildFullPackageAsync(snapPackDetails, cancellationToken: cancellationToken);
                return (nupkgMemoryStream, snapPackDetails);
            }
        }

    }
}
