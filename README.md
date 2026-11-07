# hexaOS

Sistema operacional leve baseado no kernel Linux, montado com o [Buildroot](https://buildroot.org).
Dá boot via UEFI em PCs x86-64 e no QEMU.

## Estrutura

```
configs/hexaos_x86_64_defconfig   receita do sistema (pacotes, kernel, boot)
board/hexaos/linux.fragment       ajustes de configuracao do kernel
board/hexaos/grub-efi.cfg         menu de boot
board/hexaos/genimage-efi.cfg     layout do disco (GPT: EFI + raiz ext4)
board/hexaos/rootfs_overlay/      arquivos copiados para o sistema (logo, prompt...)
scripts/build.sh                  compila
scripts/run-qemu.sh               roda no QEMU (janela ou --serial)
scripts/smoke-test.sh             teste automatico com capturas de tela
```

## Requisitos (Ubuntu / WSL)

```bash
sudo apt install -y build-essential cpio unzip rsync bc file libncurses-dev qemu-system-x86 ovmf
```

## Uso (dentro do WSL, na pasta do projeto)

```bash
bash scripts/build.sh              # compila -> ~/hexaos-build/images/disk.img
bash scripts/run-qemu.sh           # abre o hexaOS numa janela
bash scripts/build.sh menuconfig   # escolher pacotes no menu
bash scripts/build.sh linux-menuconfig  # configurar o kernel
```

A primeira compilacao leva de 20 a 40 minutos; as seguintes so recompilam o que mudou.
Login: `root` (sem senha, por enquanto).

## Gravar num pendrive (PC real)

Grave `disk.img` com o [Rufus](https://rufus.ie) (modo "DD") ou o balenaEtcher.
No notebook: desative o Secure Boot e escolha o pendrive no menu de boot.
Antes de desligar o Secure Boot, confira se a chave de recuperacao do BitLocker esta salva.
