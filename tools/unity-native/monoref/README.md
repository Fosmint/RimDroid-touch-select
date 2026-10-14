# Mono reference assemblies

`4.5/` holds the 4.5 profile of the Mono class libraries that Unity's player runs on (mscorlib, System,
System.Core and the Facades), copied from Unity 6000.0.75f1's Editor
(`Data/MonoBleedingEdge/lib/mono/4.5`). They are only compiled against, by `build_bridge.sh`, to build
RimDroidBridge.dll; they are not shipped in the APK.

The Mono class libraries, including Unity's fork (github.com/Unity-Technologies/mono), are under the
MIT License, Copyright (c) .NET Foundation and Contributors. Ubuntu's Mono 6.8 cannot replace them:
its 4.5-api has no netstandard 2.1 facade, which UnityEngine.CoreModule references (CS0012).
