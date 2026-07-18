<div align="center">
  <img src="res/logo.png" alt="Logo" width="640">

  <p align="center">
    <a href="https://twilitrealm.dev">Official Website</a>
    •
    <a href="https://discord.gg/6NpMhefCK9">Discord</a>
  </p>
</div>

# Overview

This project is a fork of the original **Dusklight** project, specifically ported to Android and featuring a second-screen mod designed for dual-screen devices (such as the AYN Thor). 

*The original repository can be found here: [https://github.com/TwilitRealm/dusklight](https://github.com/TwilitRealm/dusklight)*

Dusklight is a reverse-engineered reimplementation of Twilight Princess. It aims to be as accurate as possible to the original while also providing new options, enhancements, and tools to customize your experience.

### Dual-Screen Features

This fork heavily utilizes the bottom screen to provide a more immersive and streamlined experience. Key features include:

* **Decluttered Main Screen:** Most HUD elements—including the controller layout, minimap, health (hearts), and rupees—have been moved to the bottom screen, leaving the top screen clean and cinematic.
* **Interactive Maps:** Features a dedicated minimap and dungeon map with icon overlays. The map is fully touch-responsive, allowing you to drag to pan and pinch to zoom.
* **Quick Inventory Management:** View item information and equip items entirely from the bottom screen.
* **Comprehensive Collection Screen:** Instantly check your progress on heart containers, caught fish, golden bugs, hidden skills, and mail. You can also swap your sword, shield, and other gear directly without ever opening the pause menu.
* **Quest & Achievement Tracking:** Easily monitor your active quests and Dusklight achievements from the second screen.
* **Instant Form Toggle:** Transform between human and wolf forms instantly using a dedicated button on the bottom screen.

# Setup

> [!IMPORTANT]
> Dusklight does *not* provide any copyrighted assets. You must provide your own copy of the original game.

> [!IMPORTANT]
> At a minimum, Dusklight requires a GPU with support for D3D12, Vulkan 1.1+, or Metal. For older devices, best-effort support is provided for D3D11 and OpenGL ES (Android), but will not achieve full accuracy or performance. Your experience with specific hardware, operating systems, and drivers may vary.

### 1. Dump your game

You must dump your own copy of the game. Please see [this article](https://wiki.dolphin-emu.org/index.php?title=Ripping_Games) for instructions. After dumping, you can use a program like [Dolphin](https://dolphin-emu.org/) or [nodtool](https://github.com/encounter/nod/releases) to convert the `.iso` to `.rvz` to save space.

Currently, only the GameCube USA and EUR releases are supported. Support for other versions of the game is planned in the future.

### 2. Install Dusklight

Visit the [official installation guide](https://twilitrealm.dev/install/) for full instructions.

> [!IMPORTANT]
> **Android Installation Note:** Because this fork uses the exact same package name as the official Dusklight Android app, they cannot be installed side-by-side. If you already have the official version of Dusklight installed, please use the following workaround:
> 1. **Export** your current save file from the official app.
> 2. **Uninstall** the official Dusklight app.
> 3. **Install** this dual-screen mod version.
> 4. **Restore** your save data into the new installation.

# Building

If you'd like to build Dusklight from source, please read the [build instructions](docs/building.md).

Pull requests are welcomed! Note that we do not accept contributions that are primarily AI-generated and will close your PR if we suspect as much. Please also see the [code conventions](docs/code-conventions.md).

# Credits

Special thanks to the [TP decompilation](https://github.com/zeldaret/tp) team, the GC/Wii decompilation community, the [Aurora](https://github.com/encounter/aurora) developers, the [TP speedrunning community](https://zsrtp.link), and all [contributors](https://github.com/TwilitRealm/dusklight/graphs/contributors).

<br/>
<div align="center">
    <a href="https://github.com/encounter/aurora">
        <img src="assets/aurora-powered.png" alt="Powered by Aurora" width="800">
    </a>
</div>
