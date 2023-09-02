<!--
SPDX-FileCopyrightText: 2019-2021 Stefano Babic <sbabic@denx.de>

SPDX-License-Identifier:     LGPL-2.1-or-later
-->
libubootenv - Library to access U-Boot environment
==================================================

[![pipeline status](https://source.denx.de/swupdate/libubootenv/badges/master/pipeline.svg?ignore_skipped=true)](https://source.denx.de/swupdate/libubootenv/-/commits/master)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/21387/badge.svg)](https://scan.coverity.com/projects/21387)
[![REUSE status](https://api.reuse.software/badge/github.com/sbabic/libubootenv)](https://api.reuse.software/info/github.com/sbabic/libubootenv)

libubootenv is a library that provides a hardware independent way to access
to U-Boot environment. U-Boot has its default environment compiled board-dependently
and this means that tools to access the environment are also board specific, too.

Source Code: https://github.com/sbabic/libubootenv

Documentation (doxygen): https://sbabic.github.io/libubootenv

Replacement old tools
---------------------

Part of the library are the replacement of the "fw_printenv / fw_setenv" tools that
can be used with any board - they accept as parameter a file as initial environment if none is found
on the persistent storage. The syntax for the data configuration file is the same as the one
described in the U-Boot project whilst the syntax of the script file is a subset of the original one.

::

        Usage fw_printenv [OPTION]
         -h,                              : print this help
         -c, --config <filename>          : configuration file (by default: /etc/fw_env.config)
         -f, --defenv <filename>          : default environment if no one found (by default: /etc/u-boot-initial-env)
         -V,                              : print version and exit
         -n, --no-header                  : do not print variable name

        Usage fw_setenv [OPTION]
         -h,                              : print this help
         -c, --config <filename>          : configuration file (by default: /etc/fw_env.config)
         -f, --defenv <filename>          : default environment if no one found (by default: /etc/u-boot-initial-env)
         -V,                              : print version and exit
         -s, --script <filename>          : read variables to be set from a script

        Script Syntax:
         key=value
         lines starting with '#' are treated as comment
         lines without '=' are ignored

        Script Example:
         netdev=eth0
         kernel_addr=400000
         foo=empty empty empty    empty empty empty
         bar

License
-------

libubootenv is licensed under LGPL-2.1

OE / Yocto support
------------------

Recipe is provided in openembedded-core layer https://git.openembedded.org/openembedded-core/tree/meta/recipes-bsp/u-boot/

Buildroot support
-----------------

Package is provided in https://git.buildroot.net/buildroot/tree/package/libubootenv

Contributing to the project
---------------------------

Contributions are welcome !  You can submit your patches (or post questions
regarding the project) to the swupdate Mailing List:

	swupdate@googlegroups.com

Please read the [contributing](http://sbabic.github.io/swupdate/contributing.html)
chapter in the documentation how to contribute to the project.
