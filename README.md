# ca8210-posix
Glue code for linking Cascoda's API code to the ca8210 Linux driver or a ca821x usb dongle

## Building
This project must be configured with cmake before building. It is recommended to use the cmake gui, ccmake or cmake interactive mode, as there are user-configurable options. This will generate the required config files and setup whatever build system/projects you need.

For more information, consult https://cmake.org/runningcmake/

An example is included. Make sure that at least one of the Hid library or ca821x kernel driver is installed as detailed below. The example programs will be built alongside the library.

The example takes a series of device IDs as arguments. These should be unique, and are used as the short addresses.
```bash
cd example
make
#higher privileges may be needed if your user does not have permission to access devices
./app 1 2 3
```

## Kernel Driver
The kernel driver must be installed in order to use the kernel exchange. The driver can be found at https://github.com/Cascoda/ca8210-linux

In order to expose the ca8210 device node to this library the Linux debug file system must be mounted. debugfs can be mounted with the command:
```
mount -t debugfs none /sys/kernel/debug
```

## USB
In order to be able to connect to a dongle, hid-api must be installed. Follow the documentation in usb-exchange/hidapi to install as a shared library