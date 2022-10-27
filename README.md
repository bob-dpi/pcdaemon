**This README:**<br>
    - [**Introduction**](#intro)<br>
    - [**Architecture**](#arch)<br>
    - [**Invocation and Command Line Options**](#pcdaemon)<br>
    - [**API Quick Start**](#api)<br>
    - [**List of Peripherals**](#peri)<br>

**Related Documents:**<br>
    - [apispec.md](Docs/apispec.md)  - API Specification with Sample Programs <br> 
    - [newdriver](../pccore/README.md#drv)  - How to Create a New Peripheral <br> 
    - [design.txt](Docs/design.txt)  - pcdaemon Internal Design  <br> 

<br>

<span id="intro"></span>
### Introduction
**Pcdaemon** (this repository) gives your high level applications a
simple, intuitive interface to FPGA and non-FPGA based peripherals.
The daemon starts empty in the sense that the daemon core
provides only a command line interface, leaving the real
functionality to a set of *drivers* implemented as
loadable shared object libraries (plug-ins).  While intended
to support FPGA-based peripherals, pcdaemon has several features
you'll find useful for any Linux-to-hardware project:
  - Simple publish/subscribe mechanism for sensor data
  - All commands and data are printable ASCII over TCP (port 8870)
  - Only five API commands: *set*, *get*, *cat*, *loadso*, and *list*
  - Command line tools to view and set plug-in parameters
  - Modular plug-ins (drivers) for easy development
  - No dependencies (excluding libc)
  - Event-driven and C means low CPU/memory footprint
  - Supports both FPGA and non-FPGA peripherals
  - GPLv2 License.
<br>

<span id="arch"></span>
### System Architecture
The following diagram shows the major components in the Demand
Peripherals system. Major components include: daughter cards,
an FPGA card, pcdaemon, and your application. <br>
<img src='https://demandperipherals.com/images/arch_v2.svg'/>

**Pcdaemon** provides an API and acts as a
multiplexer for packets to and from the FPGA and for data to
and from other devices and services.  While FPGA based peripherals
are the focus, pcdaemon also includes drivers for a gamepad,
GPS receiver, voice output, and an IRC client.  Once you have
pcdaemon controlling FPGA based peripherals you may find you want
its API for all of your devices and services.

<br>

<span id="pcdaemon"></span>
### Invocation and Options
Build and install pcdaemon with the following commands:
``` 
        XXXXXXX
        git clone XXXXXX
        cd pcdaemon
        make
        sudo make install
```

The default installation directories are /usr/local/bin and
/usr/local/lib/pc. You can examine /usr/local/lib/pc to see the .so
files that are the individual peripheral drivers.

Pcdaemon has several options to let you customize its behaviour.
```
    pcdaemon [options] 
     options:
     -e, --stderr            Route messages to stderr instead of log even if running in
                             background (i.e. no stderr redirection).
     -v, --verbosity         Set the verbosity level of messages: 0 (errors), 1 (+debug),
                             2 (+ warnings), or 3 (+ info), default = 0.
     -d, --debug             Enable debug mode.
     -f, --foreground        Stay in foreground.
     -a, --listen_any        Use any/all IP addresses for UI TCP connections
     -p, --listen_port       Listen for incoming UI connections on this TCP port
     -r, --realtime          Try to run with real-time extensions.
     -V, --version           Print version number and exit.
     -o, --overload          Load .so.X file for slot specified, as slotID:file.so
     -h, --help              Print usage message.
     -s, --serialport        Use serial port specified not default port.
```

A typical debugging invocation of pcdaemon might turn on verbose debugging
and stay in the foreground.
``` 
    pcdaemon -efdv3 -s /dev/ttyUSB0
```

A typical init script invocation would usually let pcdaeom become
a real daemon and might turn on the real-time extensions.
``` 
    /usr/local/bin/pcdaemon -r
```

Peripheral number zero serves a dual purpose.  It has the *enumerator*,
a list of the peripherals in the FPGA image, and it has any FPGA board
specific I/O. The enumerator dictates which .so driver files are loaded
into pcdaemon when it starts. You can use the "-s" option to override
the enumerator list and load a new driver instead of the one specified
in the FPGA binary image.  For example, say you have a gpio4 in slot 2
and you want to overload it with a driver that you created called
bumper.so. You can replace the expected gpio4.so driver with yours using
the command:
``` 
    pcdaemon -ef -s2:bumper
```

<br>

<span id="api"></span>
### API Quick Start
The application programer's interface to pcdaemon consists of lines
of ASCII text sent over a TCP connection.  Shown below is a simple
example that configures the dual DC motor controller peripheral (dc2)
and sets its speeds to 50 and 75 percent.

    open : (TCP, localhost, port 8870)
    write: "pcset dc2 pwm_frequency 20000"
    write: "pcset dc2 watchdog 300"
    write: "pcset dc2 mode0 forward"
    write: "pcset dc2 mode1 forward"
    write: "pcset dc2 speed0 50"
    write: "pcset dc2 speed1 75"

Configure four GPIO pins as input without "send on change" and read
the pins:

    write: "pcset gpio4 interrupt 0"
    write: "pcset gpio4 direction 0"
    write: "pcget gpio4 pins"

Sensors can stream data without being polled using the *pccat*
command.  A stream of sensor data dedicates that TCP connection to
the stream of data.  Typically you'll have one TCP connection per
sensor and one more TCP connection for all other command.  Configure
the dual quadrature decoder and start its stream of data:

    open : (TCP, localhost, port 8870)
    write: "pcset quad2 update_rate 50"
    write: "pccat quad2 counts"
    read : "1   0.007472   0   0.000000"
    read : "0   0.000000   0   0.000000"
    read : "1   0.007476   0   0.000000"
    read : "1   0.007474   0   0.000000"
    read : "0   0.000000   0   0.000000"

By default pcdaemon will interogate an attached FPGA to find what 
drivers it needs.  Drivers (again, shared object plug-in modules)
for non-FPGA based peripherals need to be loaded explicitly.  Load
the text-to-speech driver, set the voice to awb, and say "hello
world".

    write: "pcloadso tts.so"
    write: "pcset tts voice awb"
    write: "pcset tts speak hello world"

Help text and self inspection are part of pcdaemon.  The *pclist*
command displays the drivers that are loaded in the system.  Giving
pclist the name of a driver as a command option displays help text
for that driver.  This command is normally run at the shell prompt
although the underlying program is just a wrapper around the identical
API command.  Display the list of loaded drivers and get a description
of the quadrature decoder peripheral.

    ~% pclist
    ~% pclist quad2

*Five commands*: If the above examples make sense you may consider
youself an expert on the pcdaemon API.  It really is that simple.

<br>
<br>

<span id="peri"></span>
###   List of Peripherals
Below is a list of available drivers with a link to the help text
for the peripheral.  A "hw" in parenthesis indicates a peripheral
for a specific board design.  See the *boards* repository for more
information on these peripherals.
|     |     |
| --- | --- |
***Motion Control***
|[dc2](fpga-drivers/dc2/readme.txt)| Dual DC motor controller |
|[quad2](fpga-drivers/quad2/readme.txt)| Dual quadrature decoder |
|[servo4](fpga-drivers/servo4/readme.txt)| Quad servo motor controller |
|[servo8](fpga-drivers/servo8/readme.txt)| Octal servo motor controller |
|[stepu](fpga-drivers/stepu/readme.txt)| Unipolar stepper motor controller |
|[stepb](fpga-drivers/stepb/readme.txt)| Bipolar stepper motor controller |
| | |
***User Interface***
|[aamp](fpga-drivers/aamp/readme.txt)| Audio amplifier with volume control and mute (hw) |
|[lcd6](fpga-drivers/lcd6/readme.txt)| Six digit LCD display (hw) |
|[tif](fpga-drivers/tif/readme.txt)| Text LCD and keypad interface (hw) |
|[ws28](fpga-drivers/ws28/readme.txt)| Quad WS2812 RBG(W) LED interface |
|[slide4](fpga-drivers/slide4/readme.txt)| Quad slide potentiometer (hw) |
|[irio](fpga-drivers/irio/readme.txt)| Consumer IR receiver/transmitter |
|[rcrx](fpga-drivers/rcrx/readme.txt)| 6/8 channel RC decoder |
|[roten](fpga-drivers/roten/readme.txt)| Rotary encoder with center button |
|[tonegen](fpga-drivers/tonegen/readme.txt)| Audio tone generator |
|[touch4](fpga-drivers/touch4/readme.txt)| Quad capacitive touch sensor (hw) |
| | |
***Input/ Output***
|[out4](fpga-drivers/out4/readme.txt)| Quad binary output |
|[in4](fpga-drivers/in4/readme.txt)| Quad binary input |
|[gpio4](fpga-drivers/gpio4/readme.txt)| Quad bidirectional I/O |
|[io8](fpga-drivers/io8/readme.txt)| Octal input / output (hw) |
|[out32](fpga-drivers/out32/readme.txt)| 32 Channel binary output (hw) |
|[in32](fpga-drivers/in32/readme.txt)| 32 Channel binary input (hw) |
|[serout4](fpga-drivers/serout4/readme.txt)| Quad Serial Output |
|[serout8](fpga-drivers/serout8/readme.txt)| Octal Serial Output |
| | |
***Sensors***
|[adc812](fpga-drivers/adc812/readme.txt)| Octal 12-bit ADC (hw) |
|[ping4](fpga-drivers/ping4/readme.txt)| Quad Parallax PING))) interface |
|[sr04](fpga-drivers/sr04/readme.txt)| Octal SRF04 interface (hw) |
|[count4](fpga-drivers/count4/readme.txt)| Quad event counter |
|[qtr4](fpga-drivers/qtr4/readme.txt)| Quad Pololu QTR interface |
|[qtr8](fpga-drivers/qtr8/readme.txt)| Octal Pololu QTR interface |
|[espi](fpga-drivers/espi/readme.txt)| SPI interface (hw) |
|[ei2c](fpga-drivers/ei2c/readme.txt)| I2C interface (hw) |
|[dac8](fpga-drivers/dac8/readme.txt)| Octal 8-bit DAC (hw) |
|[qpot](fpga-drivers/qpot/readme.txt)| Quad digital potentiometer (hw) |
|[pwmout4](fpga-drivers/pwmout4/readme.txt)| Quad PWM output |
|[pwmin4](fpga-drivers/pwmin4/readme.txt)| Quad PWM input |
|[rtc](fpga-drivers/rtc/readme.txt)| Real-time clock (hw) |
|[avr](fpga-drivers/avr/readme.txt)| AVR Microcontroller (hw) |
|[pulse2](fpga-drivers/pulse2/readme.txt)| Dual Pulse Generato |
| | |
***FPGA Board I/O***
|[axo2](fpga-drivers/axo2/readme.txt) | Axelsys Mach XO2 |
|[bb4io](fpga-drivers/bb4io/readme.txt) | Demand Peripherals Baseboard |
|[stpxo2](fpga-drivers/stpxo2/readme.txt) | Step Mach XO2 |
|[tang4k](fpga-drivers/tang4k/readme.txt) | Tang Nano 4K |
|[basys3](fpga-drivers/basys3/readme.txt) | Digilent Basys3 |
| | |
***Non-FPGA***
|[gamepad](drivers/gamepad/readme.txt) | Gamepad Interface |
|[gps](drivers/gps/readme.txt) | GPS Interface |
|[hello](drivers/hellodemo/readme.txt) | Hello World Sample |
|[irc](drivers/irccom/readme.txt) | IRC Peer-to-Peer Communications |

