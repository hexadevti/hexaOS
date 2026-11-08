# hexaOS

A lightweight operating system based on the Linux kernel, built with [Buildroot](https://buildroot.org).
It boots via UEFI on x86-64 PCs and in QEMU. Main target: Samsung Galaxy Book4 Pro
(Intel Core Ultra / Meteor Lake).

- Quiet boot with a splash screen and a Wayland desktop (Weston) with a terminal (foot)
- Laptop drivers: Intel AX211 Wi-Fi, Intel Arc graphics, I2C touchpad, Intel SOF audio
- Intel graphics acceleration: OpenGL (Mesa iris), Vulkan (ANV) and hardware video decoding (VA-API)
- 64-bit Wine (WoW64) with OpenGL, Vulkan and DXVK (DirectX 9/10/11 -> Vulkan) for games
- Firefox (YouTube, claude.ai) and VS Code; Python 3, git, Docker and Claude Code
- Sound through PipeWire (PulseAudio compatible)

## Layout

```
configs/hexaos_x86_64_defconfig   system recipe (packages, kernel, boot)
board/hexaos/linux.fragment       kernel configuration changes (laptop drivers, Docker...)
board/hexaos/grub-efi.cfg         boot menu (hidden: hold Esc to show it)
board/hexaos/genimage-efi.cfg     disk layout (GPT: EFI + ext4 root)
board/hexaos/users.txt            system users
board/hexaos/device_table.txt     file ownership fixes (/opt/firefox)
board/hexaos/art/                 logo and wallpaper (PostScript -> PNG with Ghostscript)
board/hexaos/rootfs_overlay/      files copied into the system
  usr/sbin/hexa-session           starts the desktop on tty1
  usr/bin/hexa-dev                Ubuntu development container (Docker)
  usr/bin/hexa-settings           Settings app (gear icon on the panel)
  etc/init.d/S02growfs            grows the root partition on first boot
  etc/xdg/weston/weston.ini       panel, wallpaper, keyboard layout
package/hexa-wine/                64-bit Wine with WoW64 (Wayland driver)
package/hexa-dxvk/                DXVK: DirectX 9/10/11 on Vulkan for Wine
package/hexa-firefox/             Firefox ESR (official Mozilla binary)
package/hexa-vscode/              Visual Studio Code (official Microsoft binary)
package/hexa-sof-bin/             Meteor Lake audio firmware
patches/                          tweaks to Buildroot packages (splash colors)
scripts/build.sh                  builds the system
scripts/run-qemu.sh               runs it in QEMU (window or --serial)
scripts/smoke-test.sh             automated test with screenshots
scripts/install-ssd.ps1           installs to a secondary SSD (dual boot)
```

## Requirements (Ubuntu / WSL)

```bash
sudo apt install -y build-essential cpio unzip rsync bc file libncurses-dev \
    qemu-system-x86 ovmf gcc-mingw-w64
sudo usermod -aG kvm $USER   # much faster QEMU (then: wsl --shutdown)
```

The build needs about 60 GB of free disk space. If WSL has little RAM, give it more
memory and swap in `%UserProfile%\.wslconfig` (e.g. `memory=12GB`, `swap=16GB`).

## Usage (inside WSL, in the project folder)

```bash
bash scripts/build.sh              # build -> ~/hexaos-build/images/disk.img
bash scripts/run-qemu.sh           # open hexaOS in a window
bash scripts/smoke-test.sh         # automated test with screenshots
bash scripts/build.sh menuconfig   # choose packages in the menu
bash scripts/build.sh linux-menuconfig  # configure the kernel
```

The first build takes a few hours (LLVM/Mesa and Wine are the largest packages);
later builds only rebuild what changed.

To regenerate the logo and wallpaper (on Windows, with Ghostscript installed):
`powershell -ExecutionPolicy Bypass -File board\hexaos\art\render.ps1`

## Using hexaOS

| What                       | How                                                     |
|----------------------------|---------------------------------------------------------|
| Desktop                    | starts automatically (user `hexa`, password `hexa`)     |
| Text login (root)          | Ctrl+Alt+F2 (root, no password) · back with Ctrl+Alt+F1 |
| Boot menu                  | hold Esc during boot                                    |
| Settings                   | gear icon on the panel (or `hexa-settings`): Wi-Fi, wired network, sound, brightness, scale, keyboard layout, timezone, password, services, restart/shut down |
| Wi-Fi (command line)       | `iwctl station wlan0 scan`, `iwctl station wlan0 connect "NetworkName"` |
| Volume (command line)      | `wpctl set-volume @DEFAULT_AUDIO_SINK@ 70%` (or `alsamixer`) |
| Web browser / YouTube      | Firefox icon on the panel                               |
| Programming                | VS Code icon, or `code folder/`                         |
| Full Ubuntu (apt)          | `hexa-dev` (Docker container sharing your home folder)  |
| Windows program            | `wine program.exe`                                      |
| Windows games (DirectX)    | `hexa-dxvk install` once, then `wine game.exe`          |
| Claude Code (first time)   | `curl -fsSL https://claude.ai/install.sh \| bash`, then `claude` |
| Administrator              | `sudo command` (password of the hexa user)              |
| Change password            | Settings > Password (or `sudo passwd hexa`)             |

The "text mode" and "safe mode" boot menu entries help diagnose video problems.

## Writing to a USB drive (real PC)

Use a USB drive of 16 GB or more (USB 3). Write `disk.img` with [Rufus](https://rufus.ie)
("DD" mode) or balenaEtcher.

To configure it first (Wi-Fi, password, files): run `bash scripts/run-qemu.sh --keep`,
make your changes, shut down with `poweroff` and write `disk-config.img` instead of `disk.img`.
To start over, delete `disk-config.img`.
On the laptop: disable Secure Boot and choose the USB drive in the boot menu (F2/F10 at power-on).
Before disabling Secure Boot, make sure your BitLocker recovery key is saved.
hexaOS runs entirely from the USB drive: it does not touch the installed Windows.
On first boot the system partition grows automatically to fill the whole drive.

## Known limitations

- QEMU has no Intel GPU: the desktop renders on the CPU and OpenGL is software (llvmpipe).
- Audio: without UCM profiles, the volume may need manual tuning in `alsamixer`.
- Games with kernel anti-cheat (Valorant, Fortnite...) do not run under Wine.
- On first run Wine creates `~/.wine` (slow) and offers to install Wine Mono (.NET);
  this needs internet. "Cancel" works too.
- Docker does not start at boot: `hexa-dev` starts it (asks for the password), or use
  Settings > Services (which can also turn on "start at boot"). `hexa-dev` downloads Ubuntu the first time.
- Without KVM, the QEMU scripts use `-cpu max,-smap`: with SMAP, QEMU 8.x TCG
  kills Wine. This does not happen on real hardware.
