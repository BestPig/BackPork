# PS5 BackPork 🐷

A background payload for PlayStation 5 that automatically allows system library sideloading.

## Overview

Backpork monitors game launches on the PS5. When a game (PPSA/CUSA title) is launched, it automatically mounts a `fakelib` folder from the game's app directory over the game's library path using unionfs. This allows for library replacement/injection without modifying originals files.

Original concept and some code by idlesauce

## How It Works

1. **Game Launch Detection**: When a game is launched, it retrieves the app info to get the title ID (PPSA/CUSA format)
2. **Sandbox Discovery**: Locates the game's sandbox directory at `/mnt/sandbox/<title_id>_XXX/`
3. **Fakelib Check**: Checks if a `fakelib` folder exists in the game's `app0` directory
4. **Union Mount**: If fakelibs exist, mounts them over the game's `common/lib` directory using unionfs
5. **Cleanup**: When the game exits, unmounts the fakelibs and cleans up the sandbox directory

## Requirements

- [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk)
- A jailbroken PS5 with payload execution capability

## Building

   ```bash
   export PS5_PAYLOAD_SDK=/path/to/sdk
   make
   ```

3. This produces `ps5-backpork.elf`

## Usage

> Note: To boot a game, you must downgrade its `eboot.bin` and every `prx`/`sprx` using this tool [ps5_elf_sdk_downgrade.py](https://gist.github.com/idlesauce/2ded24b7b5ff296f21792a8202542aaa); without this, the system will refuse to launch the title.

1. Place your replacement libraries in a `fakelib` folder inside the game's installation directory (`PPSSAXXXXX/fakelib/`)
2. Run the `backpork.elf` payload on your PS5
3. Launch your game - the fakelibs will be automatically mounted
4. When the game closes, cleanup is performed automatically

## Sideloading Libraries

The libraries you want to sideload must come from a firmware version compatible with the game you want to run. However, you cannot use them directly - they must be modified to remove dependencies that are not available on your current firmware, otherwise the loader will crash.

### Best Practices

It is recommended to sideload as few libraries as possible, as there is no guarantee that there won't be side effects. Most games seem to only need the two Agc libraries, but some games like Minecraft required additional libraries.

### Patching Libraries

I provide patches in the `patches/` folder (BPS format) to patch libraries from firmware 10.01 and make them functional on firmware 7.61. This is probably achievable for lower firmwares as well, but it has not been tested and would require additional patches.

> Warning: Patches are split by firmware. I recommend using the highest patch set that matches your firmware instead of using lower versions because I had to alter the normal behavior of some functions to make it work.

To apply a patch:
1. The library to patch must be decrypted (ELF format, not SELF)
2. Use an online patcher like [RomPatcher.js](https://www.marcrobledo.com/RomPatcher.js/)
3. Select the decrypted library as the ROM file
4. Select the corresponding `.bps` patch file
5. Apply the patch
6. Fake sign the patched library ([make_fself.py](https://github.com/ps5-payload-dev/sdk/blob/master/samples/install_app/make_fself.py))
7. Use the resulting library in your `fakelib` folder

## Disclaimer

This software is provided "as is", without warranty of any kind. I do not guarantee anything and I am not responsible for any damage or issues that may occur. Use at your own risk.

## License

This project is provided for educational and research purposes.

## Credits

- [idlesauce](https://github.com/idlesauce/) for the idea, code and support
- [john-tornblom](https://github.com/john-tornblom) for the SDK
- [EchoStretch](https://github.com/EchoStretch/) for endlessly testing my stupid, broken patches!
- kira for helping me figure out some patches (also known as my useful notepad)
- SlimGary for being there every day, for years.
- [Hakim](https://x.com/Hakim9919271/status/2013605108848718219) for the love of **pork** 🐷😍
