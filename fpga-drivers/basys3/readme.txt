============================================================

HARDWARE
    Each Digilent Basys-3 board has sixteen LEDs, sixteen
switches, five push buttons, and four digit seven-segment
display.  (The VGA and PS2 interfaces are handled by the
ba4term peripheral.)

    The LEDs are tied internally to Pmod port C and D.  
This provides a convenient way to monitor those ports.
LED #0 corresponds to the LSB of port C and LED #15 is
connected to the MSB of port D.

The buttons and switches are all treated as switches and
are reported as one.  There is a 50 millisecond debounce
time associated with all twenty-on switches.

You can write directly to the segments of the display or
write a four character text message to it.  Characters for
the text message must be taken from the following set:
        0 1 2 3 4 5 6 7 8 9
        A b C d E F  (may be given as upper or lower case)
        o L r h H - u
        (space) (underscore) (decimal point)

    The bb4io peripheral is a 'board peripheral' and so has
the list of driver IDs that the daemon loads at daemon
start time.  



RESOURCES

switches : Value of the switches as a six digit hex number.
This resource works with pcget and pccat.  Slide switch #0
is the LSB.  The high byte has the buttons as follows:
    01 : Center
    02 : Up
    04 : Left
    08 : Right
    10 : Down

display : Four 7-segment digits
============================================================
   A pcset on this resource writes characters to the 7-segment
displays.  The characters must be taken from the set shown
above and only the first four characters  are displayed.
The exception to this are decimal points which are displayed
between the characters and which do not count toward the four
character limit.  Messages with less than four characters are
left justified.  For example:
        # display 1234
        pcset basys3 display 1234
        # display 8.8.8.8
        pcset basys3 display 8.8.8.8
        # Display 3.1415926 -- discarding the extra digits
        pcset basys3 display 3.1415926
        # Display 12 left justified
        pcset basys3 display 12

segments : Individual segment control
   You can directly control which segments are displayed by
writing four space-separated hexadecimal values to the segments
resource.  The MSB of each value controls the 'a' segment and
the next-MSB value controls the 'b' segment.  The LSB controls
the decimal point.  For example:
        # display the middle bar (segment g)  on the first two
        # digits and vertical bars (segments 'e' and 'f') on
        # the last two digits
        pcset 6 segments 80  80  60  60

driverlist : This is a read-only resource that returns the
identification numbers of the drivers requested for the
peripherals in the FPGA build.  It works only with pcget and
returns sixteen space separated hex values.


EXAMPLES
   pcset basys3 display 0000
   pccat basys3 switches
   pcget basys3 driverlist


