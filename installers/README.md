# Drum Foundry installers

These links always download the newest release:

- **macOS** (Apple Silicon and Intel, macOS 11 or later): [DrumFoundry-macOS.pkg](https://github.com/donjmilligan/drum-foundary/releases/latest/download/DrumFoundry-macOS.pkg)
- **Windows** (64-bit Windows 10 or 11): [DrumFoundry-Windows-Setup.exe](https://github.com/donjmilligan/drum-foundary/releases/latest/download/DrumFoundry-Windows-Setup.exe)
- **Windows, VST3 only** (to copy into your VST3 folder yourself): [DrumFoundry-Windows-VST3.zip](https://github.com/donjmilligan/drum-foundary/releases/latest/download/DrumFoundry-Windows-VST3.zip)

Older versions are on the [Releases page](https://github.com/donjmilligan/drum-foundary/releases).

The installers aren't signed with an Apple or Microsoft certificate, so macOS and Windows warn you the first time you open them. The steps below get past that.

## macOS

Download `DrumFoundry-macOS.pkg` (on the Releases page it's `DrumFoundry-<version>-macOS.pkg`). It's one installer for Apple Silicon and Intel Macs running macOS 11 (Big Sur) or later, and it installs:

- the VST3, `Drum Foundry.vst3`, into `/Library/Audio/Plug-Ins/VST3`
- the AU, `Drum Foundry.component`, into `/Library/Audio/Plug-Ins/Components`
- the standalone app, `Drum Foundry.app`, into `/Applications`
- the licence texts, into `/Library/Application Support/Drum Foundry/Licenses`

To leave out the VST3, the AU or the app, click **Customize** in the installer and untick it.

The first time you open the .pkg, macOS blocks it because it isn't signed. On macOS 15 (Sequoia) or later, click **Done**, open **System Settings ▸ Privacy & Security**, scroll down to the message about the installer, and click **Open Anyway**. On older macOS, right-click the .pkg, choose **Open**, then click **Open** again.

After installing, open your DAW and rescan plug-ins. Drum Foundry is an instrument, from Theres Tears Records. If a DAW doesn't show the AU, run `killall -9 AudioComponentRegistrar` in Terminal, then rescan or restart the DAW.

## Windows

Download `DrumFoundry-Windows-Setup.exe` (on the Releases page it's `DrumFoundry-<version>-Windows-Setup.exe`; 64-bit Windows 10 or 11) and run it. It installs:

- the VST3, `Drum Foundry.vst3`, into `C:\Program Files\Common Files\VST3`
- the standalone app into `C:\Program Files\Drum Foundry`, with a Start-menu shortcut
- the licence texts into the `Licenses` folder there

If Windows says **Windows protected your PC**, click **More info**, then **Run anyway**. After installing, open your DAW and rescan plug-ins. To uninstall, go to **Settings ▸ Apps**, find Drum Foundry and click **Uninstall**.

## Making installers and releases

*Making the installers yourself:* on a Mac, `packaging/macos/make-pkg.sh` builds a universal Release and writes the .pkg to `installers/Mac/`. It's unsigned unless you set the Developer ID variables described at the top of the script. The Windows installer is built on Windows with [Inno Setup](https://jrsoftware.org/isinfo.php) 6.3 or later, after a Release build into `build\` (see [Build](#build)): run `ISCC /DAppVersion=0.1.2 packaging\windows\DrumFoundry.iss`, using the version from `CMakeLists.txt` (without it the installer is labelled 0.0.0). The installer goes to `installers\Windows\`. The installer files themselves are never committed (only `installers/README.md` is): every CI run keeps the installers (and a zip of the bare Windows VST3) as downloadable artifacts, and each release attaches them on the Releases page.

*For developers:* a release is made by pushing a tag `vX.Y.Z` that matches the version in `CMakeLists.txt` (`project(DrumFoundry VERSION X.Y.Z ...)`) and the **Version X.Y.Z** line at the top of this README (CI fails if the README line doesn't match). The GitHub Actions workflow then builds both installers, plus the Windows VST3 zip, and attaches them to a new release, each under its versioned name and again under a name without the version (`DrumFoundry-macOS.pkg` and so on) so the download links above always fetch the newest release.

The installer files aren't stored in this folder. GitHub Actions builds them for each release and attaches them to the release, which keeps the repository small to clone. When you build the installers yourself (see the main README), they're written here, and git ignores them.
