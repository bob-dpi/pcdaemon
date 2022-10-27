```
HARDWARE
   The io8 card has eight dedicated inputs and eight dedicated
outputs.  The peripheral can be configured so that a change on
an input line automatically sends the input status up to the
host.


RESOURCES
output : The value on the output pins as a 2 digit hex value. 
    You can read and write this resource using pcget and pcset.

input : The value on the input pins.  This resource works with
    pcget and pccat.  A read (pcget) requires a round trip to
    the FPGA and may take a few milliseconds.  You can monitor
    the input pins using a pccat command.  Using pccat makes
    sense only if one or more of the inputs are configured as 
    an interrupt-on-change pin.


interrupt : An 'interrupt-on-change' enable as a 2 digit hex
    value.  When an input pin has its corresponding interrupt
    bit set any change in value on that pin causes an update
    to be sent to the host.  This feature makes reading the
    input pins something that can be done using select().
    This resource works with pcget and pcset.


EXAMPLES
    To output 11000011 to the outputs, make all inputs
interrupt-on-change, and start a data stream of new input
values :
        pcset io8 output c3
        pcset io8 interrupt ff
        pccat io8 inputs

```
