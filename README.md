# Xenia Classics Crossplay

A fork of [Xenia Canary Netplay](https://github.com/AdrianCassar/xenia-canary) that lets PC players join
**Call of Duty 4** (TU4) and **Modern Warfare 2** (TU9) matches hosted on a **real Xbox 360**, alongside
Xbox 360 and PS3 players. It powers PC play in [Classics Ranked](https://classics.gg/ranked).

**You need your own copy of the game.** This repository and its releases contain no game files.

## Download and set up

* **Download:** the latest build is on the [Releases](../../releases) page.
* **Setup guide:** [classics.gg/ranked/how-it-works](https://classics.gg/ranked/how-it-works) (PC · Xenia).
* **Ranked play** needs the Subscriber role in the [Classics Discord](https://classics.gg/discord). On first
  start Xenia shows a code; type `/link` with it in the Discord. The message closes once you're linked.

## What this fork adds

All of it is off unless `bridge_enabled` is set in the config (`[Live]` section).

* **Crossplay bridge** (`src/xenia/kernel/xbridge*`): system-link traffic for the 360 host goes through the
  host's crossplay relay, with a small header in front; replies are presented to the game as the host's.
  A control channel carries the player's build, input mode and the Discord link.
* **Discord link** (crossplay build 8): ranked identity is the player's Discord account. The relay assigns
  the account a fixed XUID, which the profile plays under while `bridge_fake_live` is on.
* **MW2 Xbox Live private-match party join** through the relay (`bridge_fake_live`), automatic rejoin after
  a match ends, and an optional offline-profile unlock so custom classes can be built.
* **Keyboard and mouse** for crossplay (`bridge_input_mode = "kbm"`), which removes the game's aim assist.
* **Host player settings** (flipped bumpers/triggers, sensitivity compensation, CoD4 Bumper Jumper).

The relay, the host console plugin and the ranked services are not part of this repository.

## Build

Windows, Visual Studio 2022 and the Vulkan SDK; CMake/Ninja:

```
python xenia-build.py build --config=release
```

The exe is `build/bin/Windows/Release/xenia_canary_netplay.exe`.

## License

BSD, as upstream Xenia (see [LICENSE](LICENSE)). Everything below is the upstream Xenia Canary Netplay readme.

---

# Xenia Canary Netplay

This is a fork of [Xenia Canary](https://github.com/xenia-canary/xenia-canary) which implements online multiplayer features. The REST API powering this fork can be found [here](https://github.com/AdrianCassar/Xenia-WebServices#xenia-web-services).

Current online sessions are displayed at [https://xenia-netplay-2a0298c0e3f4.herokuapp.com/](https://xenia-netplay-2a0298c0e3f4.herokuapp.com/).

---

## Netplay Wiki

The wiki contains a compatibility list and general information about the current state netplay.

To get started with netplay read [config setup](https://github.com/AdrianCassar/xenia-canary/wiki/Config-Setup).

### Wiki Pages:
* [Home](https://github.com/AdrianCassar/xenia-canary/wiki)
* [Netplay Compatibility](https://github.com/AdrianCassar/xenia-canary/wiki/Netplay-Compatibility)
* [Testing Procedure](https://github.com/AdrianCassar/xenia-canary/wiki/Testing-Procedure)
* [Config Setup](https://github.com/AdrianCassar/xenia-canary/wiki/Config-Setup)
* [Systemlink](https://github.com/AdrianCassar/xenia-canary/wiki/Systemlink)
* [FAQ](https://github.com/AdrianCassar/xenia-canary/wiki/FAQ)

### Netplay Builds:
* [Latest stable release](https://github.com/AdrianCassar/xenia-canary/releases/latest)
* [Github Actions](https://github.com/AdrianCassar/xenia-canary/actions?query=actor%3AAdrianCassar+branch%3Anetplay_canary_experimental)
    * These builds are latest work in progress and require a github account to download.
* [Mousehook](https://github.com/marinesciencedude/xenia-canary-mousehook/releases?q=Netplay)
    * A limited number of games have netplay and mousehook support.

---

<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary/tree/canary_experimental/assets/icon">
        <img height="256px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Xenia Canary - Xbox 360 Emulator</h1>

Xenia Canary is an experimental fork of the Xenia emulator. For more information, see the
[Xenia Canary wiki](https://github.com/xenia-canary/xenia-canary/wiki).

Come chat with us about **emulator-related topics** on [Discord](https://discord.gg/Q9mxZf9).
For developer chat join `#dev` but stay on topic. Lurking is not only fine, but encouraged!
Please check the [FAQ](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) page before asking questions.
We've got jobs/lives/etc, so don't expect instant answers.

Discussing illegal activities will get you banned.

## Status

Buildbot | Status | Releases
-------- | ------ | --------
Canary (🪟, 🐧) | [![CI](https://github.com/xenia-canary/xenia-canary/actions/workflows/Orchestrator.yml/badge.svg?branch=canary_experimental)](https://github.com/xenia-canary/xenia-canary/actions/workflows/Orchestrator.yml/badge.svg?branch=canary_experimental) [![Codacy Badge](https://app.codacy.com/project/badge/Grade/cd506034fd8148309a45034925648499)](https://app.codacy.com/gh/xenia-canary/xenia-canary/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade) | [Latest](https://github.com/xenia-canary/xenia-canary/releases/latest) ◦ [All](https://github.com/xenia-canary/xenia-canary/releases) ◦ [Old](https://github.com/xenia-canary/xenia-canary-releases/releases)

### Experimental Netplay

Buildbot | Status | Releases
-------- | ------ | --------
Windows | [![Codacy Badge](https://app.codacy.com/project/badge/Grade/d814c4b6aa444dcc9c1631e0224b2739)](https://app.codacy.com/gh/AdrianCassar/xenia-canary/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade) | [Latest](https://github.com/AdrianCassar/xenia-canary/releases/latest)

## Quickstart

See the [Quickstart](https://github.com/xenia-canary/xenia-canary/wiki/Quickstart) page.

## FAQ

See the [frequently asked questions](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) page.

## Game Compatibility

See the [Game compatibility list](https://github.com/xenia-canary/game-compatibility/issues)
for currently tracked games, and feel free to contribute your own updates,
screenshots, and information there following the [existing conventions](https://github.com/xenia-canary/game-compatibility/blob/canary/README.md).

## Building

See [building.md](docs/building.md) for setup and information about the
`xb` script. When writing code, check the [style guide](docs/style_guide.md)
and be sure to run clang-format!

## Contributors Wanted!

Have some spare time, know advanced C++, and want to write an emulator?
Contribute! There's a ton of work that needs to be done, a lot of which
is wide open greenfield fun.

**For general rules and guidelines please see [CONTRIBUTING.md](.github/CONTRIBUTING.md).**

Fixes and optimizations are always welcome (please!), but in addition to
that there are some major work areas still untouched:

* Help work through [missing functionality/bugs in games](https://github.com/xenia-canary/xenia-canary/labels/compat)
* Reduce the size of Xenia's [huge log files](https://github.com/xenia-canary/xenia-canary/issues/1526)
* Skilled with Linux? A strong contributor is needed to [help with porting](https://github.com/xenia-canary/xenia-canary/labels/platform-linux)

See more projects [good for contributors](https://github.com/xenia-canary/xenia-canary/labels/good%20first%20issue). It's a good idea to ask on Discord and check the issues page before beginning work on
something.

## Disclaimer

The goal of this project is to experiment, research, and educate on the topic
of emulation of modern devices and operating systems. **It is not for enabling
illegal activity**. All information is obtained via reverse engineering of
legally purchased devices and games and information made public on the internet
(you'd be surprised what's indexed on Google...).
