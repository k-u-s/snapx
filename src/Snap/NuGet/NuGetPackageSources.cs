using System;
using System.Collections.Generic;
using System.Linq;
using Fasterflect;
using JetBrains.Annotations;
using NuGet.Common;
using NuGet.Configuration;

#if NET45
using Snap.Extensions;
#endif

namespace Snap.NuGet
{
    #if NET45
    class NuGetMachineWideSettings : IMachineWideSettings
    {
        readonly Lazy<IEnumerable<Settings>> _settings;

        public IEnumerable<Settings> Settings => _settings.Value;

        public NuGetMachineWideSettings()
        {
            var baseDirectory = NuGetEnvironment.GetFolderPath(NuGetFolderPath.MachineWideConfigDirectory);
            _settings = new Lazy<IEnumerable<Settings>>(() => global::NuGet.Configuration.Settings.LoadMachineWideSettings(baseDirectory));
        }
    }
    #else
    class NuGetMachineWideSettings : IMachineWideSettings
    {
        readonly Lazy<ISettings> _settings;

        public ISettings Settings => _settings.Value;

        public NuGetMachineWideSettings()
        {
            var baseDirectory = NuGetEnvironment.GetFolderPath(NuGetFolderPath.MachineWideConfigDirectory);
            _settings = new Lazy<ISettings>(() => global::NuGet.Configuration.Settings.LoadMachineWideSettings(baseDirectory));
        }
    }
    #endif
    
    internal sealed class NugetOrgOfficialV2PackageSource : NuGetPackageSources
    {
        static readonly PackageSource PackageSourceV2 = new PackageSource(NuGetConstants.V2FeedUrl, "nuget.org", true, true, false)
        {
            ProtocolVersion = (int)NuGetProtocolVersion.V2,
            IsMachineWide = true
        };

        public NugetOrgOfficialV2PackageSource() : base(new NullSettings(), new List<PackageSource> { PackageSourceV2 })
        {

        }
    }

    internal sealed class NugetOrgOfficialV3PackageSource : NuGetPackageSources
    {
        static readonly PackageSource PackageSourceV3 = new PackageSource(NuGetConstants.V3FeedUrl, "nuget.org", true, true, false)
        {
            ProtocolVersion = (int)NuGetProtocolVersion.V3,
            IsMachineWide = true
        };

        public NugetOrgOfficialV3PackageSource() : base(new NullSettings(), new List<PackageSource> { PackageSourceV3 })
        {

        }
    }

    internal class NuGetMachineWidePackageSources : NuGetPackageSources
    {
        public NuGetMachineWidePackageSources() 
        {
            var nuGetConfigFileReader = new NuGetConfigFileReader();
            var nugetMachineWideSettings = new NuGetMachineWideSettings();

            var configFilePaths = nugetMachineWideSettings.Settings.GetConfigFilePaths();
            var packageSources = configFilePaths.Select(configFilePath => nuGetConfigFileReader.ReadNugetSources(configFilePath)).ToList();

            Items = packageSources.SelectMany(x => x.Items).ToList();
            Settings = nugetMachineWideSettings.Settings;
        }
    }

    internal interface INuGetPackageSources
    {
        ISettings Settings { get; }
        IReadOnlyCollection<PackageSource> Items { get; }
    }

    internal class NuGetPackageSources : INuGetPackageSources
    {
        public ISettings Settings { get; protected set; }
        public IReadOnlyCollection<PackageSource> Items { get; protected set; }

        protected NuGetPackageSources()
        {
            Items = new List<PackageSource>();
        }

        public NuGetPackageSources([NotNull] ISettings settings) : this(settings, settings.GetConfigFilePaths().Select(x => new PackageSource(x)))
        {
            if (settings == null) throw new ArgumentNullException(nameof(settings));
        }

        public NuGetPackageSources([NotNull] ISettings settings, [NotNull] IEnumerable<PackageSource> sources) 
        {
            if (settings == null) throw new ArgumentNullException(nameof(settings));
            if (sources == null) throw new ArgumentNullException(nameof(sources));

            var items = sources.ToList();

            if (!items.Any())
            {
                throw new ArgumentException(nameof(items));
            }
            
            Items = items;
            Settings = settings;
        }

        public override string ToString()
        {
            return string.Join(",", Items.Select(s => s.SourceUri.ToString()));
        }
    }
}
