﻿using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using JetBrains.Annotations;
using Mono.Cecil;
using NuGet.Configuration;
using NuGet.Versioning;
using Snap.Core;
using Snap.Core.IO;
using Snap.NuGet;
using Snap.Shared.Tests.Extensions;
using TypeAttributes = Mono.Cecil.TypeAttributes;

namespace Snap.Shared.Tests
{
    [SuppressMessage("ReSharper", "UnusedMember.Global")]
    public class BaseFixture
    {
        public string WorkingDirectory => Directory.GetCurrentDirectory();

        public SnapAppSpec BuildSnapAppSpec([NotNull] string channelName = "test")
        {
            if (channelName == null) throw new ArgumentNullException(nameof(channelName));

            var feed = new SnapFeed
            {
                Name = "nuget.org",
                SourceUri = new Uri(NuGetConstants.V3FeedUrl),
                ProtocolVersion = NuGetProtocolVersion.NugetV3
            };

            var channel = new SnapChannel
            {
                Name = channelName,
                Feed = feed.Name
            };

            return new SnapAppSpec
            {
                Id = "demoapp",
                Version = new SemanticVersion(1, 0, 0),
                Feed = feed,
                Channel = channel,
                TargetFramework = new SnapTargetFramework
                {
                    Framework = "netcoreapp2.1",
                    RuntimeIdentifier = "win7-x64",
                    OsPlatform = OSPlatform.Windows.ToString()                    
                },
                AvailableChannels = new List<SnapChannel> { channel }
            };
        }

        public void WriteAssemblies(string workingDirectory, List<AssemblyDefinition> assemblyDefinitions, bool disposeAssemblyDefinitions = false)
        {
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));
            if (assemblyDefinitions == null) throw new ArgumentNullException(nameof(assemblyDefinitions));

            foreach (var assemblyDefinition in assemblyDefinitions)
            {
                assemblyDefinition.Write(Path.Combine(workingDirectory, assemblyDefinition.GetRelativeFilename()));

                if (disposeAssemblyDefinitions)
                {
                    assemblyDefinition.Dispose();
                }
            }
        }

        public void WriteAssemblies(string workingDirectory, bool disposeAssemblyDefinitions = false, params AssemblyDefinition[]  assemblyDefinitions)
        {
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));
            if (assemblyDefinitions == null) throw new ArgumentNullException(nameof(assemblyDefinitions));

            WriteAssemblies(workingDirectory, assemblyDefinitions.ToList(), disposeAssemblyDefinitions);
        }

        public void WriteAndDisposeAssemblies(string workingDirectory, params AssemblyDefinition[]  assemblyDefinitions)
        {
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));
            if (assemblyDefinitions == null) throw new ArgumentNullException(nameof(assemblyDefinitions));

            WriteAssemblies(workingDirectory, assemblyDefinitions.ToList(), true);
        }

        internal IDisposable WithDisposableAssemblies(string workingDirectory, ISnapFilesystem filesystem, params AssemblyDefinition[] assemblyDefinitions)
        {
            if (workingDirectory == null) throw new ArgumentNullException(nameof(workingDirectory));
            if (assemblyDefinitions == null) throw new ArgumentNullException(nameof(assemblyDefinitions));

            WriteAndDisposeAssemblies(workingDirectory, assemblyDefinitions);

            return new DisposableFiles(filesystem, assemblyDefinitions.Select(x => x.GetFullPath(workingDirectory)).ToArray());
        }

        public AssemblyDefinition BuildEmptyLibrary(string libraryName, IReadOnlyCollection<AssemblyDefinition> references = null)
        {
            if (libraryName == null) throw new ArgumentNullException(nameof(libraryName));

            var assembly = AssemblyDefinition.CreateAssembly(
                new AssemblyNameDefinition(libraryName, new Version(1, 0, 0, 0)), libraryName, ModuleKind.Dll);
            
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

        public AssemblyDefinition BuildEmptyExecutable(string applicationName, IReadOnlyCollection<AssemblyDefinition> references = null)
        {
            if (applicationName == null) throw new ArgumentNullException(nameof(applicationName));

            var assembly = AssemblyDefinition.CreateAssembly(
                new AssemblyNameDefinition(applicationName, new Version(1, 0, 0, 0)), applicationName, ModuleKind.Console);
            
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
    }
}