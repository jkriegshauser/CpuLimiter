# CpuLimiter

While trying to play [Assassin's Creed: Unity](https://store.steampowered.com/agecheck/app/289650/) on an AMD Threadripper 
5975W, the game would crash immediately on start. After the splash screen I would hear a second of audio, see the first few
frames of an intro video, and then crash to desktop.

A bit of searching led me to a [Reddit user that had the same issue](https://www.reddit.com/r/ubisoft/comments/16pgvwl/ac_unity_crash_on_startup_on_highend_pc/)
and a comment that had a [YouTube video describing a fix](https://youtu.be/5abOt0V59d0?t=570).

However, their fix was to disable CPUs in Windows and reboot. That wasn't a great solution for me, so I figured that I would make
a utility that would *lie* to the game about the system's CPU topology, essentially claiming that there are fewer cores than there are.

Hence, CpuLimiter.

Enjoy!

## Building Instructions

1. Clone this repo (or your fork of it)
2. Make sure to get submodules: `git submodule update --init`
3. In your Visual Studio Developer Command Prompt from in the Detours directory, build Detours with `nmake`.
4. In Visual Studio, build CpuLimiter (*Release* configuration recommended).

## Installing Instructions

Installation can be tricky since games often have DRM that look for modifications and balk. *Assassin's Creed: Unity*
(which was the impetus for this project) did exactly that.

If you can launch a game directly, you might be able to launch it with Detours' `withdll.exe`:

```bat
withdll.exe /d:CpuLimiter.dll <Your game launch instructions>
```

You can also patch a game (or maybe a dependent DLL) by using Detours' `setdll.exe`:

```bat
setdll.exe /d:CpuLimiter.dll <exe or dll binary>
```

For Assassin's Creed: Unity, I patched *NvGsa.x64.dll* since the game executable detected the modification.
