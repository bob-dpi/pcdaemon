**QUICK INDEX:**<br>
    \- [**Application Architecture**](#arch)<br>
    \- [**Resources**](#resources)<br>
    \- [**API**](#api)<br>
    \- [**API Examples**](#examples)<br>
    \- [**Sample Programs**](#sample) <br>
      [**counter.py**](#counterpy)<br>
      [**counter.c**](#counter)<br>
      [**quad\_demo.c**](#quaddemo)<br>
    \- [**Security Notes**](#security)<br>

 

<span id="arch"></span>
### APPLICATION ARCHITECTURE

The diagram below shows a typical architecture of the application
programs running on a Linux host. The program ***pcdaemon*** provides
the interface between the low level register read/write protocol to the
FPGA and your high level applications that manage the peripherals.

<img src=arch3.svg width=640 />

The architecture supports more than one controlling application and
applications can use either the select() family of routines for
demultiplexing or have a separate thread for each TCP connection.
Typically when your application wants to start of stream of sensor data
it will open a TCP connection to port 8870 and issue a pccat command
(see below). This TCP connection is now dedicated to receiving a stream
of sensor data from one peripheral. Add the file descriptor to the
select list if you are using select() or just wait for data if you are
using threads. You will probably find that you have one TCP connection
for each monitored sensor and an additional connection that you use to
send configuration changes to the peripherals.

A ***driver*** is that part of a peripheral that runs in pcdaemon and
implements the API for that peripheral. The pcdaemon program receives
packets from the FPGA and routes those packets to the appropriate
driver. Pcdaemon also accepts TCP connections from applications and
routes ASCII commands and data to the appropriate driver. The driver
translates API commands into the appropriate low level register read and
write commands to send over the serial interface to the board. There is
always a one-to-one correspondence between FPGA peripherals and drivers.
Since they are loaded dynamically drivers are implemented as shared
libraries and are sometimes called *plug-ins*.

<br> 

<span id="resources"></span>
### RESOURCES

Peripherals are identified either by their name or, if there is more
than one instance of a given peripheral in the system, by their slot
number. Each peripheral has a set of one or more resources associated
with it. A resource may be compound in that it may take more than one
value. For example, a "config" resource may group common configuration
values into one line.

A ***resource*** is an application visible name for a given
configuration parameter or a sensor value. For example, the buttons on
the Demand Peripherals FPGA card are are in the "bb4io" peripheral and
have a resource name of "buttons". Resources can be read-write, write-only,
or read-only depending on the nature of the resource. Most configuration
resources are read-write, and sensor reading are usually read-only.

Some sensor resources can be configured to automatically send a reading
only when the input changes or when a timer expires. This can greatly
simplify your application since you do not need to continuously poll
sensors to detect changes.

 

<span id="api"></span>

**API:**

The API consists of space separated ASCII words sent over a TCP
connection. Each command is terminated by a newline. There are five
commands in the API:    - pcset : write a configuration parameter    -
pcget : read a configuration parameter    - pccat : start streaming
sensor data    - pclist : list available peripherals or give help on
specified peripheral    - pcloadso : load a non-FPGA based driver

The commands have the following syntax    pcset \<slot\#|peri\_name\>
\<resource\_name\> \<value\>    pcget \<slot\#|peri\_name\>
\<resource\_name\>    pccat \<slot\#|peri\_name\> \<resource\_name\>
   pclist \[peri\_name\]    pcloadso \<full path to .so driver file\>

<span id="examples"></span>

**EXAMPLES:**

Most FPGA cards have buttons and LEDs that are visible from the API. The
LEDs are usually internally tied to the lowest numbered peripherals.
This gives you a simple way to monitor a peripheral. For these examples
we assume that peripheral \#1 is an out4 peripheral.

       pcset out4 outval f   # turn LEDs on
       pccat bb4io buttons   # wait for a button press

The dual DC motor controller peripherals,
[dc2](http://demandperipherals.com/peripherals/dc2.html), has controls
for the PWM frequency, the mode (forward, reverse, brake, or coast), the
PWM duty cycle, and a watchdog timer that stops the motors if there are
no speed updates within a certain time.

       pcset dc2 pwmfrequency 20000    # set PWM freq to 20KHz
       pcset dc2 mode0 f     # motor 0 in forward mode
       pcset dc2 mode1 f     # motor 1 in forward mode
       pcset dc2 watchdog 15 # watchdog set to 1.5 seconds
       pcset dc2 power0 40   # motor 0 at 40% PWM
       pcset dc2 power1 70   # motor 0 at 70% PWM

The 6 digit LCD display peripheral,
[lcd6](http://demandperipherals.com/peripherals/lcd6.html), lets you
control individual segments or output fully formed digits.

       pcset lcd6 display 1234.56 # display a number 
       pcget lcd6 segments        # ask which segments are on

The octal 12-bit analog-to-digital peripheral,
[adc812](http://demandperipherals.com/peripherals/adc812.html), lets you
specify the sample rate and whether or not to combine two inputs into
one differential input ADC channel.

       pcset adc812 config 25, 0x00  # 25 ms updates, all singled ended
       pccat adc812 samples   # start sample stream

 

<span id="sample"></span>

**SAMPLE PROGRAMS:**

<span id="counterpy"></span> COUNTER.PY: The following is a simple but
complete Python program that shows how to use the pcdaemon API. The
source code is available here:
[counter.py](http://demandperipherals.com/downloads/counter.py).

``` 
#!/usr/bin/env python
import socket
import sys

# This program opens two sockets to the pcdaemon, one
# to listen for button press events and one to update
# the LED display.  This code uses a blocking read but
# a select() implementation would work too.
#
# Pressing button 1 increments the count, button 2
# clears the count and button 3 decrements the count.
# Buttons are represented as hex values 1, 2, and 4.

# Global state information
count = 0

try:
    sock_cmd = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock_cmd.connect(('localhost', 8870))
    sock_button = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock_button.connect(('localhost', 8870))
    sock_button.send('pccat bb4io buttons\n')
    # loop forever getting button presses and updating the count
    while True:
        display_count = count
        if display_count < 0:
            display_count = count + 16
        sock_cmd.send('pcset out4 outval ' "%x" '\n' % display_count)
        key = sock_button.recv(6)
        keyint = int(key[:3])
        if keyint == 1:
            count = (count + 1) % 256
        elif keyint == 2:
            count = 0;
        elif keyint == 4:
            count = (count - 1) % 256

except KeyboardInterrupt:
    # exit on Ctrl^C
    sock_cmd.close()
    sock_button.close()
    sys.exit()

except socket.error:
    print "Couldn't connect to pcdaemon"
    sys.exit()

```

 

<span id="counter"></span> COUNTER.C: The following is a simple but
complete C program that shows how to use the pcdaemon API. The source
code is available here:
[counter.c](http://demandperipherals.com/downloads/counter.c).

    /* Counter.c  :  This program demonstrates the use of the
     * Demand Peripherals API for the Baseboard4 FPGA cards.
     *
     * The idea is that we create a counter and watch the
     * buttons on the FPGA card.  If button 1 is press the
     * count goes down.  If button 2 is pressed the count
     * is zeroed, and if button 3 is pressed the count is
     * incremented.  The count is an 8 bit signed number
     * that is displayed on the LEDs of the FPGA card.
     *
     * Build with: gcc -o counter counter.c
     * Be sure pcdaemon is running and listening on port 8870
     */
    
    
    /* Overview:
     *  Open a TCP connection to the FPGA card.
     *  Send commands to flash the LEDs
     *  Open a second connection to the FPGA card
     *  pccat the button presses on the second connection
     *  loop forever
     *   -- wait for button press (ignore release events)
     *   -- decrement, clear, or increment counter
     *   
     * As a tutorial, this program is not flexible, uses
     * magic numbers,  ignores key bounce, and does a very
     * poor job of error checking.  It tries to be brief and 
     * readable instead.
     */
    
    #include <stdio.h>
    #include <stdlib.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <stddef.h>
    #include <string.h>    /* for memset */
    #include <arpa/inet.h> /* for inet_addr() */
    
    static int  cmdfd;          // FD for commands to the FPGA card
    static void sndcmd(char *cmd); // send a command to the board, get prompt
    
    
    int main()
    {
        int8_t counter;         // the 8-bit count to display
        int  tmp_int;           // a temporary integer
        int  evtfd;             // FD for button events from the FPGA card
        struct sockaddr_in skt; // network address for pcdaemon
        int  adrlen;
        char strled[99];        // command to set the leds
        char strevt[99];        // where to receive the button press string
        int  buttons;           // latest button event as an integer
    
        // Open connection to pcdaemon daemon
        adrlen = sizeof(struct sockaddr_in);
        (void) memset((void *) &skt, 0, (size_t) adrlen);
        skt.sin_family = AF_INET;
        skt.sin_port = htons(8870);
        if ((inet_aton("127.0.0.1", &(skt.sin_addr)) == 0) ||
            ((cmdfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) ||
            (connect(cmdfd, (struct sockaddr *) &skt, adrlen) < 0)) {
            printf("Error: unable to connect to pcdaemon.\n");
            exit(-1);
        }
    
        /* Blink the LEDs on the out4 peripheral in slot 1*/
        sndcmd("pcset out4 outval f\n");
        sleep(1);
        sndcmd("pcset out4 outval 0\n");
        sleep(1);
        sndcmd("pcset out4 outval f\n");
        sleep(1);
        sndcmd("pcset out4 outval 0\n");
    
        /* Open another connection to receive button events */
        (void) memset((void *) &skt, 0, (size_t) adrlen);
        skt.sin_family = AF_INET;
        skt.sin_port = htons(8870);
        if ((inet_aton("127.0.0.1", &(skt.sin_addr)) == 0) ||
            ((evtfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) ||
            (connect(evtfd, (struct sockaddr *) &skt, adrlen) < 0)) {
            printf("Error: unable to connect to pcdaemon.\n");
            exit(-1);
        }
    
        counter = 0;   // leds are already showing zero
    
        /* Start the stream of button events */
        write(evtfd, "pccat bb4io buttons\n", 20);
        /* the above command never returns so we do not use sndcmd() */
    
        while(1) {
            /* wait for a button press */
            read(evtfd, strevt, 3);    // two digits and a newline
            sscanf(strevt, "%d", &buttons);
    
            /* Examine pressed button and change counter */
            /* (We don't keep the old button value so do not do
               edge detection.  ie. One button at a time please) */
            if (buttons & 1) {
                counter--;
            }
            if (buttons & 2) {
                counter = 0;
            }
            if (buttons & 4) {
                counter++;
            }
    
            /* display new value of count */
            sprintf(strled, "pcset out4 outval 01x\n", (counter & 0x0f));
            sndcmd(strled);
        }
    }
    
    /* sndcmd():  Send a command to pcdaemon and wait for a response.  The
     *     response will be a prompt character, which we ignore and return,
     *     or an error message which we send to stderr. */
    static void sndcmd(char *cmd)
    {
        size_t count;          // number of chars in command to send
        char   c;              // prompt or error message character
        int    retval;         // return value of read()
    
        count = strlen(cmd);        // should sanity check count
        write(cmdfd, cmd, count);   // should look at write() return value
    
        /* loop getting characters.  Return on a prompt character '\' and
         * send any other character to stderr. */
        while (1) {
            retval = read(cmdfd, &c, 1);
            if (0 >= retval)
                exit(1);       // did TCP conn go down?
            else if ('\\' == c)
                return;        // got a prompt char.  Done with command
            else
                write(2, &c, 1);    // send to stderr
        }
    }

 

<span id="quaddemo"></span> QUAD\_DEMO.C: The following is a simple C
program that shows how use the select() system call to monitor several
sensors simultaneously. The source code is available here:
[quad\_demo.c](http://demandperipherals.com/downloads/quad_demo.c).

    /* A program to demonstrate how to use Baseboard peripherals  */
    /* in an event driven program using select().                 */
    /*   The program uses two peripherals, the buttons and LEDs   */
    /* on the Baseboard (bb4io), and the dual quadrature decoder, */
    /* quad2.  The program configures the quad2 to report every   */
    /* 50 milliseconds and prints to standard output the counts   */
    /* and frequency of the quadrature inputs.  Pressing button   */
    /* one on the Baseboard turns on or off quadratures output.   */
    /* The LEDs on the Baseboard are incremented on each reading  */
    /* from the quadrature decoder.                               */
    /*    gcc -o quad_demo quad_demo.c                            */
    /*    ./quad_demo                                             */
    
    /* This program is more complete than the first sample but is */
    /* still not production ready.  Please use or refactor this   */
    /* code as you see fit for your application.                  */
    
    #include <stdio.h$gt;
    #include <stdlib.h$gt;
    #include <unistd.h$gt;
    #include <netinet/in.h$gt;
    #include <sys/socket.h$gt;
    #include <sys/errno.h$gt;
    #include <sys/time.h$gt;
    #include <string.h$gt;
    
    
    /************************* Defines *****************************/
    #define PCSRVADDR    "127.0.0.1"
    #define PCSRVPORT    8870
    #define MXCMD        80   /* limits pcdaemon command size */
    #define MXBUF        99   /* limits size of response from pcdaemon */
    
    
    
    /********************** Global Variables ***********************/
    int     fd_cmd;           // fd to send commands to pcdaemon
    int     outenable;        // set ==1 to print quad2 readings on stdout
    void    sndcmd(int, char *, int);  // write a string down an FD
    void    do_quad(char *);  // process input line of quadrature data
    
    
    int main (int argc, char *argv[])
    {
        int    fd_quad;       // fd to stream of quadrature readings
        int    fd_button;     // fd to buttons on the FPGA card
        fd_set rfds;          // bit masks for select statement
        int    mxfd;          // Maximum FD for the select statement
        struct timeval tv;    // for the one second timer
        int    ret;           // return value for select() call
        char   cmd[MXCMD];    // print commands to pcdaemon here
        char   inbuf[MXBUF];  // read from pcdaemon goes here
        char   qbuf[MXBUF];   // data from the quadrature sensor goes here
        int    qinx;          // index into qbuf
        int    nread;         // return value for read()   
        int    gotline;       // ==1 if found a full line in the data stream
        int    cmdlen;        // length of command to send
        int    i;             // generic loop counter
        int    value;         // value read from bb4io button
        int    count;         // number of quad2 readings
        struct sockaddr_in skt; // network address for pcdaemon
        int    adrlen;
    
    
        /* Initialize the state */
        count = 0;
        outenable = 1;
        qinx = 0;
    
    
        // Open connections to pcdaemon
        adrlen = sizeof(struct sockaddr_in);
        (void) memset((void *) &skt, 0, (size_t) adrlen);
        skt.sin_family = AF_INET;
        skt.sin_port = htons(8870);
        if ((inet_aton("127.0.0.1", &(skt.sin_addr)) == 0) ||
            ((fd_cmd = socket(AF_INET, SOCK_STREAM, 0)) < 0) ||
            (connect(fd_cmd, (struct sockaddr *) &skt, adrlen) < 0) ||
            ((fd_quad = socket(AF_INET, SOCK_STREAM, 0)) < 0) ||
            (connect(fd_quad, (struct sockaddr *) &skt, adrlen) < 0) ||
            ((fd_button = socket(AF_INET, SOCK_STREAM, 0)) < 0) ||
            (connect(fd_button, (struct sockaddr *) &skt, adrlen) < 0)) {
            printf("Error: unable to connect to pcdaemon.\n");
            exit(-1);
        }
    
        /* Clear the LEDs */
        cmdlen = snprintf(cmd, MXCMD, "pcset out4 outval 0\n");
        sndcmd(fd_cmd, cmd, cmdlen);
    
        /* Configure quad2 for an update every 50 milliseconds */
        /* Then start the stream of quad2 readings             */
        // (note that we send the pccat command on fd_quad.  This is
        // means the quadrature readings will be available on fd_quad
        cmdlen = snprintf(cmd, MXCMD, "pcset quad2 update_period 50\n");
        sndcmd(fd_cmd, cmd, cmdlen);
        cmdlen = snprintf(cmd, MXCMD, "pccat quad2 counts\n");
        sndcmd(fd_quad, cmd, cmdlen);
    
        /* Start the data stream of button presses from the Baseboard */
        cmdlen = snprintf(cmd, MXCMD, "pccat bb4io buttons\n");
        sndcmd(fd_button, cmd, cmdlen);
    
    
    
        /* Watch for button press events, quadrature readings, and */
        /* timeout each second  */
        mxfd = (fd_quad $gt; fd_button) ? (fd_quad+1) : (fd_button+1);
        while (1) {
            FD_ZERO(&rfds);
            FD_SET(fd_quad, &rfds);
            FD_SET(fd_button, &rfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
    
            ret = select(mxfd, &rfds, (fd_set *)NULL, (fd_set *)NULL, &tv);
            /* if select error -- bail out on all but EINTR and EAGAIN */
            if ((ret < 0) && ((errno != EINTR) && (errno != EAGAIN))) { 
                perror("Failure in select() ");
                exit(-1);
            }
    
            if (ret == 0) {  // timeout
                // this timeout does not occur on one second boundaries
                // but one second after the last time we processed a 
                // read-ready file descriptor.  To make a _periodic_
                // timer you would use gettimeofday to intelligently
                // set tv_sec and tv_usec before each call to select().
            }
    
            if (FD_ISSET(fd_button, &rfds)) {
                ret = read(fd_button, inbuf, MXBUF);
                if (0 $gt; ret) {
                    if ((errno != EINTR) && (errno != EAGAIN)) 
                        perror("Error reading button press from bb4io");
                    continue;
                }
    
                // While tempting in its simplicity, the code below has a bug.
                // There is no guarantee that read() will return the newline
                // at the end of a button sensor reading.  The next read would
                // see the newline from the previous reading and not see the new
                // value.  This case is handled properly for quad2 readings.
                if (sscanf(inbuf, "%d", &value) == 1) {
                    if (1 == value)        // output quadrature readings on first button
                        outenable = 1;
                    else if (2 == value)
                        outenable = 0;     // suppress readings on second button
                }
            }
    
            if (FD_ISSET(fd_quad, &rfds)) {
                // increment count and send to LEDs
                count++;
                cmdlen = snprintf(cmd, MXCMD, "pcset out4 outval %x\n", count & 0x000f);
                sndcmd(fd_cmd, cmd, cmdlen);
    
                // read() is not guaranteed to return full lines of text.  Since we may
                // get part of a line, we need to store the partial line (qbuf) while
                // waiting for the next read.  Worse, if the processor is busy we may
                // find more than one line of input in the buffer.  We need to scan the
                // collected characters looking for a newline.  If found we process the
                // line and move any remaining characters to the beginning of the buffer.
                // The code below would normally go in a subroutine that is used for
                // reading sensor streams.
    
                /* Get data from quadrature sensor.  There may already be characters */
                /* in the buffer so add to end of qbuf */
                nread = read(fd_quad, &(qbuf[qinx]), (MXBUF - qinx));
                if (0 $gt;= nread) {
                    if ((errno != EINTR) && (errno != EAGAIN)) 
                        perror("Error reading the quadrature peripheral");
                    continue;
                }
                qinx += nread;
    
                /* Scan for a complete lines. */
                do {
                    gotline = 0;
                    // Scan for a newline.    If found, replace it with a null
                    for (i = 0; i < qinx; i++) {
                        if (qbuf[i] == '\n') {
                            qbuf[i] = (char) 0;
                            gotline = 1;
                            do_quad(qbuf);
    
                            // move any remaining characters to start of buffer
                            (void) memmove(qbuf, &(qbuf[i+1]), (qinx - (i+1)));
                            qinx -= i+1;
                            break;
                        }
                    }
                } while ((gotline == 1) && (qinx $gt; 0));
            }
        }
    }
    
    /* sndcmd() : send a command to the pcdaemon.  Report      */
    /* write errors to stdout but try to continue.             */
    void sndcmd(int fd, char *cmd, int length)
    {
        int     nwrt;   // number of bytes written
    
        if (0 $gt;= length) {
            printf("Error sending command of length %d\n", length);
            return;
        }
    
        nwrt = write(fd, cmd, length);
        if (nwrt != length) {
            printf("Error sending command to pcdaemon.  Wrote only %d of %d bytes\n", nwrt, length);
        }
    
        return;
    }
    
    /* do_quad() : get quadrature readings from a line of sensor data. */
    void do_quad(char *qline)
    {
        int    ret;           // return value for sscan() call
        int    tick0, tick1;  // quadrature ticks
        float  period0, period1; // seconds to get ticks
        float  freq0, freq1;  // tick frequency
    
        ret = sscanf(qline, "%d %f %d %f", &tick0, &period0, &tick1, &period1);
        if (4 != ret) {
            printf("error reading quadrature ticks and periods\n");
            return;
        }
    
        if ((0 == tick0) || (0 == period0))
            freq0 = 0.0;
        else
            freq0 = (float) tick0 / period0;
        if ((0 == tick1) || (0 == period1))
            freq1 = 0.0;
        else
            freq1 = (float) tick1 / period1;
    
        // print the counts and their frequency
        if (outenable)
            printf("%d %f %d %f\n", tick0, freq0, tick1, freq1);
    
        // In  a fully event driven system, this is where we
        // would do the PID loop to control motor speed.
        // do_pid(freq0, freq1);
        // and where we could do odometry
        // do_odometry(tick0, tick1);
    
        return;
    }

 
<span id="security"></span> <span></span>

### SECURITY NOTES

Security for the system is dependent on the security of the serial port
and the security of pcdaemon. The default security for the USB serial
port is probably sufficient for most applications. You may want to
change the ownership of the port to match the ownership of pcdaemon.

The security of pcdaemon relates to the security of the TCP port that
offers the ASCII interface. By default the port listens on the localhost
interface so the TCP connections must originate on the same machine that
is running pcdaemon. You can use the -a option when starting pcdaemon to
allow access from anywhere on the network. Clearly the -a option should
be avoided in a shipping system.

If you want to secure the system from local attack you may want to
switch from TCP sockets to UNIX sockets for the API. This will map
access permissions to the your filesystem. Setting ownership and
read/write access permissions in the filesystem is fairly easy and more
intuitive compared to trying to restrict permissions for TCP
connections.
