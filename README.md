# hexaOS

Sistema operacional leve baseado no kernel Linux, montado com o [Buildroot](https://buildroot.org).
Dá boot via UEFI em PCs x86-64 e no QEMU. Alvo principal: Samsung Galaxy Book4 Pro
(Intel Core Ultra / Meteor Lake).

- Boot silencioso com tela de splash e area de trabalho Wayland (Weston) com terminal (foot)
- Drivers do notebook: Wi-Fi Intel AX211, video Intel Arc, touchpad I2C, audio Intel SOF
- Wine 64-bit (WoW64) para programas Windows de 32 e 64 bits (pacote `hexa-wine`)

## Estrutura

```
configs/hexaos_x86_64_defconfig   receita do sistema (pacotes, kernel, boot)
board/hexaos/linux.fragment       ajustes de configuracao do kernel (drivers do notebook)
board/hexaos/grub-efi.cfg         menu de boot (oculto: segure Esc para ver)
board/hexaos/genimage-efi.cfg     layout do disco (GPT: EFI + raiz ext4)
board/hexaos/users.txt            usuarios do sistema
board/hexaos/art/                 logo e wallpaper (PostScript -> PNG com Ghostscript)
board/hexaos/rootfs_overlay/      arquivos copiados para o sistema
  usr/sbin/hexa-session           inicia a interface grafica no tty1
  etc/xdg/weston/weston.ini       painel, wallpaper, teclado ABNT2
package/hexa-wine/                Wine 64-bit com WoW64 (driver Wayland)
package/hexa-sof-bin/             firmware de audio do Meteor Lake
patches/                          ajustes em pacotes do Buildroot (cores do splash)
scripts/build.sh                  compila
scripts/run-qemu.sh               roda no QEMU (janela ou --serial)
scripts/smoke-test.sh             teste automatico com capturas de tela
```

## Requisitos (Ubuntu / WSL)

```bash
sudo apt install -y build-essential cpio unzip rsync bc file libncurses-dev \
    qemu-system-x86 ovmf gcc-mingw-w64
sudo usermod -aG kvm $USER   # QEMU bem mais rapido (depois: wsl --shutdown)
```

## Uso (dentro do WSL, na pasta do projeto)

```bash
bash scripts/build.sh              # compila -> ~/hexaos-build/images/disk.img
bash scripts/run-qemu.sh           # abre o hexaOS numa janela
bash scripts/smoke-test.sh         # teste automatico com capturas de tela
bash scripts/build.sh menuconfig   # escolher pacotes no menu
bash scripts/build.sh linux-menuconfig  # configurar o kernel
```

A primeira compilacao leva de 30 a 90 minutos (o Wine e o maior pacote);
as seguintes so recompilam o que mudou.

Para regerar o logo e o wallpaper (no Windows, com Ghostscript instalado):
`powershell -ExecutionPolicy Bypass -File board\hexaos\art\render.ps1`

## Usando o hexaOS

| O que                     | Como                                                  |
|---------------------------|-------------------------------------------------------|
| Area de trabalho          | abre sozinha (usuario `hexa`, senha `hexa`)           |
| Login em texto (root)     | Ctrl+Alt+F2 (root sem senha) · volta com Ctrl+Alt+F1  |
| Menu de boot              | segure Esc durante o boot                             |
| Wi-Fi                     | `iwctl station wlan0 scan`, `iwctl station wlan0 connect "NomeDaRede"` |
| Volume                    | `alsamixer`                                           |
| Programa Windows          | `wine programa.exe`                                   |
| Claude Code (1a vez)      | `curl -fsSL https://claude.ai/install.sh \| bash`, depois `claude` |
| Trocar senha              | `passwd`                                              |

O modo "texto" e o "seguro" (no menu de boot) ajudam a diagnosticar problemas de video.

## Gravar num pendrive (PC real)

Grave `disk.img` com o [Rufus](https://rufus.ie) (modo "DD") ou o balenaEtcher.

Para configurar antes (Wi-Fi, senha, arquivos): rode `bash scripts/run-qemu.sh --keep`,
faca os ajustes, desligue com `poweroff` e grave `disk-config.img` em vez de `disk.img`.
Para recomecar do zero, apague `disk-config.img`.
No notebook: desative o Secure Boot e escolha o pendrive no menu de boot (F2/F10 ao ligar).
Antes de desligar o Secure Boot, confira se a chave de recuperacao do BitLocker esta salva.
O hexaOS roda inteiro do pendrive: nao mexe no Windows instalado.

## Limitacoes conhecidas

- A interface usa renderizacao por CPU (sem Mesa/aceleracao 3D por enquanto).
- Audio: sem perfis UCM, o volume pode precisar de ajuste manual no `alsamixer`.
- Wine sem OpenGL/Vulkan: bom para programas comuns, nao para jogos.
- Na primeira execucao o Wine cria `~/.wine` (demora) e oferece instalar o
  Wine Mono (.NET); requer internet. "Cancel" tambem funciona.
- No QEMU sem KVM os scripts usam `-cpu max,-smap`: com SMAP o TCG do QEMU 8.x
  derruba o Wine. Isso nao acontece no hardware real.
