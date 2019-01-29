﻿using System;
using System.Runtime.InteropServices;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using YamlDotNet.Serialization;

namespace Snap.Core.Yaml
{
    internal sealed class OsPlatformYamlTypeConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type)
        {
            return type == typeof(OSPlatform);
        }

        public object ReadYaml(IParser parser, Type type)
        {
            var osPlatform = ((Scalar)parser.Current).Value;
            parser.MoveNext();
            return TryCreateOsPlatform(osPlatform);
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var osPlatformStr = ((OSPlatform)value).ToString();
            emitter.Emit(new Scalar(osPlatformStr));
        }

        static OSPlatform TryCreateOsPlatform(string osPlatform)
        {
            if (string.IsNullOrWhiteSpace(osPlatform))
            {
                osPlatform = "unknown";
            }
            else if (osPlatform.Equals("anyos", StringComparison.InvariantCultureIgnoreCase))
            {
                osPlatform = "anyos";            
            }

            return OSPlatform.Create(osPlatform);
        }
    }
}