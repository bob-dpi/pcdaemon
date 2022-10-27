```
============================================================

stpxo2

HARDWARE
   The STEP-MachXO2 board has three buttons, four switches, two RGB
LEDs, and two seven-segment displays.  This is a board driver and
so includes the list of driver IDs in the FPGA image, and is always
in slot #0.


RESOURCES
drivlist : list of driver IDs
   Each of the peripherals in the FPGA image has a recommended 
driver identification number.  Use pcget on this resource to see
the list of sixteen driver IDs in this build.  For example:
        pcget stpxo2 drivlist

switches : state of buttons and switches on the board
   Use pcget or pccat to get the values of the switches and the
buttons on the board.  The return value is a seven bit hex number
with the switches in the low four bits and the buttons in bits 
3 to 6.  For example:
        pcget stpxo2 switches
        pccat 0 switches

rgb: state of the two RGB LEDs
   Use pcset to control the on/off state of the six LEDs (two each
of red, green, and blue).  Specify the state using two hex digits
in the range of 0 to 7 where the LSB controls the red, bit 1 controls
the green, and bit 2 controls the blue LEDs.  For example:
        pcset stpxo2 rgb 7 7        # All six LEDs on
        pcset stpxo2 rgb 4 4        # Both blue LEDs on
        pcset stpxo2 rgb 0 0        # All six LEDs off
 

display : state of the two 7-segment displays
   Use pcset to display two character on the 7-segment display.
Characters for the text message must be taken from the following set:
        0 1 2 3 4 5 6 7 8 9
        A b C d E F  (may be given as upper or lower case)
        o L r h H - u
        (space) (underscore) (decimal point)
The characters you enter are converted to eight segment values
which are visable in the segments resource described below.  Examples
of use include:
        pcset 0 display 8.8.         # All segments on
        pcset 0 display '  '         # Two spaces = all segments off
        pcset 0 FD                   # a two digit hex value


segments : individual segment control
   You can directly control which segments are displayed by writing
two space-separated hexadecimal values to the segments resource.
The LSB of each value controls the 'a' segment and the next-MSB value
controls the 'b' segment.  The MSB controls the decimal point.  For
example:
        pcset 0 segments 1 1         # Both 'a' segments on
        pcset 0 segments 81 81       # 'a' and the decimal point



```
