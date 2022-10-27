```
============================================================

HARDWARE
    The Tang Nano 4K has I/O pins at 1.8, 2,5, and 3.3 volts.
Please examine the board data sheet before connecting to the
pins.
    The tang4k peripheral is a 'board peripheral' and so has
the list of driver IDs for the peripherals in the FPGA build.


RESOURCES
driverlist : This is a read-only resource that returns
the identification numbers of the drivers requested for
the peripherals in the FPGA build.  It works only with
pcget and returns sixteen space separated hex values.


EXAMPLES
   pcget tang4k driverlist


```
