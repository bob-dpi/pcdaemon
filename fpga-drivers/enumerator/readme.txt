```
============================================================

enumerator

At system startup the enumerator interrogates the FPGA to
get a list of the peripherals built into the FPGA image.
Specifically, it gets a list of the driver IDs that the
user wants loaded for the FPGA image.

The enumerator converts the list of ID numbers to file
names and does an ldopen on the drivers to load them
into the system.  

Typically, the enumerator becomes inaccessible after
system start since peripheral #0 is usually replaced
with the board IO peripheral specific to the FPGA board.


RESOURCES
drivlist : list of driver IDs


EXAMPLES
 pcget enumerator drivlist


```
