libubootenv - Library to access U-Boot environment
==================================================

[![Build Status](https://travis-ci.org/sbabic/libubootenv.svg?branch=master)](https://travis-ci.org/sbabic/libubootenv)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/21387/badge.svg)](https://scan.coverity.com/projects/21387)

libubootenv is a library that provides a hardware independent way to access
to U-Boot environment. U-Boot has its default environment compiled board-dependently
and this means that tools to access the environment are also board specific, too.

Source Code: https://github.com/sbabic/libubootenv

Documentation (doxygen): https://sbabic.github.io/libubootenv

Replacement old tools
---------------------

Part of the library are the replacement of the "fw_printenv / fw_setenv" tools that
can be used with any board - they accept as parameter a file as initial environment if none is found
on the persistent storage. The syntax for the file and for the data configuration file
is the same as the one described in the U-Boot project.

::

        Usage fw_printenv [OPTION]
         -h,                              : print this help
         -c, --config <filename>          : configuration file (old fw_env.config)
         -f, --defenv <filename>          : default environment if no one found
         -V,                              : print version and exit
         -n, --no-header                  : do not print variable name

        Usage fw_setenv [OPTION]
         -h,                              : print this help
         -c, --config <filename>          : configuration file (old fw_env.config)
         -f, --defenv <filename>          : default environment if no one found
         -V,                              : print version and exit
         -s, --script <filename>          : read variables to be set from a script

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
