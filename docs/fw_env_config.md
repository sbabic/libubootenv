fw_env.config configuration file
================================

This is the configuration file for fw_(printenv/setenv) utility.
Up to two entries are valid, in this case the redundant
environment sector is assumed present.
Notice, that the "Number of sectors" is not required on NOR and SPI-dataflash.
Futhermore, if the Flash sector size is omitted, this value is assumed to
be the same as the Environment size, which is valid for NOR and SPI-dataflash
Device offset must be prefixed with 0x to be parsed as a hexadecimal value.

Structure of the file
---------------------

Entries must be separated by spaces or tab


| device name     | Device offset|Env. size|Flash sector size|Number of sectors|Disable lock mechanism|
|-----------------|--------------|---------|-----------------|-----------------|----------------------|
|                 |              |         |                 |                 |                      | 

- device name: device or file where env is stored (mandatory)
- offset : offset from start of file or device (mandatory)
- env size : size of environment
- Flash sector size : (optional) if not set, it is read from kernel
- Number of sectors: (optional) number of sectors for environment (mainly used with raw NAND)
- Disable lock mechanism : (optional), 0|1, default=0 (LOCK enable) 

NOR example
-----------
| device name     | Device offset|Env. size|Flash sector size|Number of sectors|Disable lock mechanism|
|-----------------|--------------|---------|-----------------|-----------------|----------------------|
|/dev/mtd1        |     0x0000   | 0x4000  |	0x4000       |                 |                      | 
|/dev/mtd2        |     0x0000   | 0x4000  |	0x4000       |                 |                      | 

MTD SPI-dataflash example
---------------------------
| device name     | Device offset|Env. size|Flash sector size|Number of sectors|Disable lock mechanism|
|-----------------|--------------|---------|-----------------|-----------------|----------------------|
|/dev/mtd5        |     0x4200   | 0x4000  |	             |                 |                      | 
|/dev/mtd6        |     0x4200   | 0x4000  |	             |                 |                      | 

NAND example
-----------
| device name     | Device offset|Env. size|Flash sector size|Number of sectors|Disable lock mechanism|
|-----------------|--------------|---------|-----------------|-----------------|----------------------|
|/dev/mtd0        |     0x4000   | 0x4000  |    0x20000	     |       2         |                      | 

Block device example
--------------------
On a block device a negative offset is treated as a backwards offset from the
end of the device/partition, rather than a forwards offset from the start.


| device name     | Device offset|Env. size|Flash sector size|Number of sectors|Disable lock mechanism|
|-----------------|--------------|---------|-----------------|-----------------|----------------------|
|/dev/mmcblk0     |     0xc0000  | 0x20000 |                 |                 |                      | 
|/dev/mmcblk0     |    -0x20000  | 0x20000 |                 |                 |                      | 

VFAT example
------------

| device name     | Device offset|Env. size|Flash sector size|Number of sectors|Disable lock mechanism|
|-----------------|--------------|---------|-----------------|-----------------|----------------------|
|/boot/uboot.env  |     0x00000  | 0x4000  |                 |                 |                      | 

UBI volume
------------

| device name     | Device offset|Env. size|Flash sector size|Number of sectors|Disable lock mechanism|
|-----------------|--------------|---------|-----------------|-----------------|----------------------|
|/dev/ubi0_0      |     0x0      | 0x1f000 |  0x1f000        |                 |                      | 
|/dev/ubi0_1      |     0x0      | 0x1f000 |  0x1f000        |                 |                      | 

UBI volume by name
------------------

| device name     | Device offset|Env. size|Flash sector size|Number of sectors|Disable lock mechanism|
|-----------------|--------------|---------|-----------------|-----------------|----------------------|
|/dev/ubi0:env    |     0x0      | 0x1f000 |  0x1f000        |                 |                      | 
|/dev/ubi0:redund |     0x0      | 0x1f000 |  0x1f000        |                 |                      | 
