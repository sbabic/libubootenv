<!--
SPDX-FileCopyrightText: 2019-2021 Stefano Babic <sbabic@denx.de>

SPDX-License-Identifier:     LGPL-2.1-or-later
-->
fw_env.config Configuration File- Legacy format
================================================

This is the configuration file for fw_{printenv,setenv} utility. It was defined
in U-Boot project and it is defined here as legacy format.

Up to two entries are valid, in this case the redundant
environment sector is assumed present.
Notice, that the "Number of Sectors" is not required on NOR and SPI dataflash.
Futhermore, if the Flash Sector Size is omitted, this value is assumed to
be the same as the Environment Size, which is valid for NOR and SPI dataflash.
Device Offset must be prefixed with 0x to be parsed as a hexadecimal value.


Structure of the Configuration File
-----------------------------------

Entries must be separated by spaces or tabs.

| Device Name      | Device Offset | Environment Size | Flash Sector Size | Number of Sectors | Disable Lock Mechanism |
|------------------|---------------|------------------|-------------------|-------------------|------------------------|
|                  |               |                  |                   |                   |                        |

- Device Name: device or file where environment is stored (mandatory)
- Device Offset: offset from start of file or device (mandatory)
- Environment Size: size of environment (in bytes)
- Flash Sector Size: (optional) if not set, it is read from kernel
- Number of Sectors: (optional) number of sectors for environment (mainly used
  with raw NAND)
- Disable Lock Mechanism : (optional), 0|1, default=0 (LOCK enabled)


NOR Example
-----------

| Device Name      | Device Offset | Environment Size | Flash Sector Size | Number of Sectors | Disable Lock Mechanism |
|------------------|---------------|------------------|-------------------|-------------------|------------------------|
| /dev/mtd1        |     0x0       |      0x4000      |      0x4000       |                   |                        |
| /dev/mtd2        |     0x0       |      0x4000      |      0x4000       |                   |                        |


MTD SPI Dataflash Example
-------------------------

| Device Name      | Device Offset | Environment Size | Flash Sector Size | Number of Sectors | Disable Lock Mechanism |
|------------------|---------------|------------------|-------------------|-------------------|------------------------|
| /dev/mtd5        |     0x4200    |      0x4000      |                   |                   |                        |
| /dev/mtd6        |     0x4200    |      0x4000      |                   |                   |                        |


NAND Example
------------

| Device Name      | Device Offset | Environment Size | Flash Sector Size | Number of Sectors | Disable Lock Mechanism |
|------------------|---------------|------------------|-------------------|-------------------|------------------------|
| /dev/mtd0        |     0x4000    |      0x4000      |      0x20000      |         2         |                        |


Block Device Example
--------------------

On a block device a negative offset is treated as a backwards offset from the
end of the device/partition, rather than a forwards offset from the start.

| Device Name      | Device Offset | Environment Size | Flash Sector Size | Number of Sectors | Disable Lock Mechanism |
|------------------|---------------|------------------|-------------------|-------------------|------------------------|
| /dev/mmcblk0     |     0xC0000   |     0x20000      |                   |                   |                        |
| /dev/mmcblk0     |    -0x20000   |     0x20000      |                   |                   |                        |


VFAT Example
------------

| Device Name      | Device Offset | Environment Size | Flash Sector Size | Number of Sectors | Disable Lock Mechanism |
|------------------|---------------|------------------|-------------------|-------------------|------------------------|
| /boot/uboot.env  |     0x0       |      0x4000      |                   |                   |                        |


UBI Volume Example
------------------

| Device Name      | Device Offset | Environment Size | Flash Sector Size | Number of Sectors | Disable Lock Mechanism |
|------------------|---------------|------------------|-------------------|-------------------|------------------------|
| /dev/ubi0_0      |     0x0       |      0x1f000     |      0x1f000      |                   |                        |
| /dev/ubi0_1      |     0x0       |      0x1f000     |      0x1f000      |                   |                        |


UBI Volume by Name Example
--------------------------

| Device Name      | Device Offset | Environment Size | Flash Sector Size | Number of Sectors | Disable Lock Mechanism |
|------------------|---------------|------------------|-------------------|-------------------|------------------------|
| /dev/ubi0:env    |     0x0       |      0x1f000     |      0x1f000      |                   |                        |
| /dev/ubi0:redund |     0x0       |      0x1f000     |      0x1f000      |                   |                        |

Configuration File in YAML
==========================

A YAML format is defined to allow multiple sets of variable. This lets have same
features (redundancy, power-cut safe) for environment that are not bound to the
U-Boot bootloader.

A set is selected by using the `-m/--namespace` argument. In case the bootloader
tells us where the environment is located by setting the
`/chosen/u-boot,env-config` property in the devicetree, `fw_printenv/fw_setenv`
automatically uses the string from this property as a selector for the namespace
in the YAML config file.

```yaml
uboot:
  size : 0x4000
  lockfile : /var/lock/fw_printenv.lock
  devices:
    - path : /dev/mtd0
      offset : 0xA0000
      sectorsize : 0x10000
      unlock : yes
    - path : /dev/mtd0
      offset : 0xB0000
      sectorsize : 0x10000
      disable-lock : yes

appvar:
  size : 0x4000
  lockfile : /var/lock/appvar.lock
  devices:
    - path : /dev/mtd1
      offset : 0
      sectorsize : 0x10000
      unlock : yes
    - path : /dev/mtd1
      offset : 0x10000
      sectorsize : 0x10000
```
