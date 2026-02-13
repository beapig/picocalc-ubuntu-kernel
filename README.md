# Picocalc Luckfox Lyra

Based on Luckfox_Lyra_SDK_250429

Image and SDK [Download](https://mega.nz/folder/hn8iUDzY#Db-FwlFiGOg4CetCUm3r9Q)

## Special Disclaimer
The sole purpose of this repository is to build a development environment for the Linux kernel. It is specifically designed for Picocalc (configured with Luckfox Lyra) running a specific Ubuntu rootfs, and to provide the necessary environment for compiling drivers such as Wi-Fi. If you intend to use this for other purposes, you will need to handle any compatibility issues yourself.

## 1. Prepare SDK

### 1.1. SDK Environment Deployment

[Luckfox-Lyra](https://wiki.luckfox.com/Luckfox-Lyra/SDK)

### 1.2. Clone Picocalc-Luckfox-Lyra

1. Change to the SDK directory

```shell
cd $SDK_DIR
```

2. Clone

```shell
git clone https://github.com/beapig/picocalc-ubuntu-kernel.git
```

The SDK directory structure will be like

```
├── build.sh -> device/rockchip/common/scripts/build.sh ---- SDK compilation script
├── app ----------------------------- Contains upper-layer applications (mainly demo apps)
├── buildroot ----------------------- Root filesystem developed based on Buildroot (2024)
├── device ----------------- Contains chip board-level configurations and scripts/files for SDK compilation and firmware packaging
├── docs ---------------------------- Contains general development guidance documents, chip platform-related documents, Linux system development-related documents, and other reference materials
├── external --------------------- Contains third-party related repositories, including display, audio and video, cameras,networking, security, etc.
├── kernel -------------------------- Contains code for Kernel development
├── output -------------------------- Stores information on generated firmware, compilation information, XML files, host environment, etc.
├── picocalc-ubuntu-kernel------------ This repository
├── prebuilts ----------------------- Contains cross-compilation toolchains
├── rkbin --------------------------- Contains Rockchip-related binaries and tools
├── rockdev ------------------------- Stores compiled output firmware, actually a symlink to output/firmware 
├── tools --------------------------- Contains commonly used tools for Linux and Windows operating systems 
├── u-boot -------------------------- Contains U-Boot code developed based on version v2017.09
└── yocto --------------------------- Contains root filesystem developed based on Yocto 5.0
```

3. Prepare

```shell
cd picocalc-ubuntu-kernel
./prepare.sh
```

## 2. Build

```shell
./build.sh chip

############### Rockchip Linux SDK ###############

Manifest: luckfox_linux6.1_rk3506_release_v1.2_20250311.xml

Log colors: message notice warning error fatal

Log saved at /home/lyra/sdk-test/output/sessions/2026-02-14_00-32-43
Switching to chip: rk3506
Pick a defconfig:

1. luckfox_lyra_buildroot_sdmmc_defconfig
2. luckfox_lyra_buildroot_spinand_defconfig
3. luckfox_lyra_plus_buildroot_sdmmc_defconfig
4. luckfox_lyra_plus_buildroot_spinand_defconfig
5. luckfox_lyra_ultra-w_buildroot_emmc_defconfig
6. luckfox_lyra_ultra_buildroot_emmc_defconfig
7. picocalc_luckfox_lyra_buildroot_sdmmc_defconfig
Which would you like? [1]: 7


./build.sh kernel
```
# Important Note
Based on my testing, if you are only compiling the kernel, the process may trigger an error during the final step. However, the compilation is actually successful. You can find the required output, "zboot.img", located in the %sdk-path/kernel-6.1/ directory.
