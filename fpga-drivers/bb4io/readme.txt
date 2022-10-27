```
============================================================

HARDWARE
    Each BaseBoard4 has three buttons (S1, S2 and S3) and
eight LEDs (LED0 to LED7).  The bb4io peripheral controls
these resources and is standard on all FPGA builds for the
BaseBoard4.

    The LEDs are tied internally to the first eight user
IO pins.  That is, they are tied to the FPGA pins available
on connector SV1 which corresponds to slots one and two.

    The bb4io peripheral is a 'board peripheral' and so has
the list of driver IDs that the daemon loads at daemon
start time.  



RESOURCES

buttons : The value of the buttons as a number between 0
and 7.  This resource works with pcget and pccat.  The
button 'S1' is the LSB.

driverlist : This is a read-only resource that returns
the identification numbers of the drivers requested for
the peripherals in the FPGA build.  It works only with
pcget and returns sixteen space separated hex values.


EXAMPLES

   pccat bb4io buttons
   pcget bb4io driverlist


```
