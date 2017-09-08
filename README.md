# ca8210-posix
Glue code for linking Cascoda's API code to the ca8210 Linux driver or a ca821x usb dongle

In order to build a useable library for the ca8210 after cloning the repository, run the commands:

```
git submodule update --init --recursive
make
```
Then include ca821x.h in your C source, and link with the libca8210.a library.

## Kernel Driver
The kernel driver must be installed in order to use the kernel exchange. The driver can be found at https://github.com/Cascoda/ca8210-linux

In order to expose the ca8210 device node to this library the Linux debug file system must be mounted. debugfs can be mounted with the command:
```
mount -t debugfs none /sys/kernel/debug
```

## USB
In order to be able to connect to a dongle, hid-api must be installed. Follow the documentation in usb-exchange/hidapi to install as a shared library