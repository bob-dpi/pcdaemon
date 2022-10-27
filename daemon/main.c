/*
 * Name: main.c
 * 
 * Description: This is the entry point for pcdaemon
 * 
 * Copyright:   Copyright (C) 2021 by Demand Peripherals, Inc.
 *              All rights reserved.
 *
 * License:     This program is free software; you can redistribute it and/or
 *              modify it under the terms of the Version 2 of the GNU General
 *              Public License as published by the Free Software Foundation.
 *              GPL2.txt in the top level directory is a copy of this license.
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *              GNU General Public License for more details.
 *
 *
 */

/*
 * 
 * pcdaemon Program Flow:
 *   -- Process command line options
 *   -- Init
 *   -- Load command line .so files (-s option)
 *   -- Load enumerator.so file and get driver IDs from the FPGA
 *   -- Become a daemon
 *   -- Main loop
 * 
 * Synopsis: pcdaemon [options]
 *   
 * options:
 *  -e, --stderr           Route messages to stderr instead of log even if running in
 *                         background (i.e. no stderr redirection).
 *  -v, --verbosity        Set the verbosity level of messages: 0 (errors), 1 (+commands),
 *                         2 (+ responses), 3 (+ internal trace), default = 0.
 *  -d, --debug            Enable debug mode.
 *  -f, --foreground       Stay in foreground.
 *  -a, --listen_any       Listen for incoming UI connections on any IP address
 *  -p, --listen_port      Listen for incoming UI connections on this port
 *  -r, --realtime         Try to run with real-time extensions.
 *  -V, --version          Print version number and exit.
 *  -o, --overload         Overload peripheral in slot with specified .so file (as slotID:file.1)
 *  -h, --help             Print usage message
 *  -s, --serial           Use serial port specified, not the default
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <limits.h>              // for PATH_MAX
#include <termios.h>
#include <sys/ioctl.h> 
#include <linux/serial.h>
#include <locale.h>
#include "main.h"

/***************************************************************************
 *  - Limits and defines
 ***************************************************************************/
        // give up after trying to reset the FPGA this many times
#define MAXFPGARESET    100

/***************************************************************************
 *  - Function prototypes
 ***************************************************************************/
static void globalinit();
static void daemonize();
static void invokerealtimeextensions();
static void processcmdline(int, char *[]);
static void openfpgaserial();
extern void open_ui_port();
extern void muxmain();
extern void initslot(SLOT *);  // Load and init this slot
extern void add_so_slot(char *);
extern void receivePkt(int, void *, int);


/***************************************************************************
 *  - System-wide global variable allocation
 ***************************************************************************/
SLOT     Slots[MX_SLOT];       // Allocate the driver table
PC_FD    Pc_Fd[MX_FD];         // Table of open FDs and callbacks
PC_TIMER Timers[MX_TIMER];     // Table of timers
UI       UiCons[MX_UI];        // Table of UI connections
CORE     Core[NUM_CORE];       // Table of FPGA based peripherals
int      UseStderr = 0;        // use stderr
int      Verbosity = 0;        // verbosity level
int      DebugMode = 0;        // run in debug mode
int      UiaddrAny = 0;        // Use any IP address if set
int      UiPort = DEF_UIPORT;  // TCP port for ui connections
int      ForegroundMode = 0;   // run in foreground
int      RealtimeMode = 0;     // use realtime extension
char    *SerialPort = DEFFPGAPORT;
int      fpgaFD = -1;          // -1 or fd to SerialPort


/***************************************************************************
 *  - Main.c specific globals
 ***************************************************************************/
char       *CmdName;     // How this program was invoked
const char *versionStr = "pcdaemon Version 0.9.0, Copyright 2019 by Demand Peripherals, Inc.";
const char *usageStr = "usage: pcdaemon [-ev[level]dfrVmol[fpgabinfile]s[serialport]h]\n";
const char *helpText = "\
pcdaemon [options] \n\
 options:\n\
 -e, --stderr            Route messages to stderr instead of log even if running in\n\
                         background (i.e. no stderr redirection).\n\
 -v, --verbosity         Set the verbosity level of messages: 0 (errors), 1 (+debug),\n\
                         2 (+ warnings), or 3 (+ info), default = 0.\n\
 -d, --debug             Enable debug mode.\n\
 -f, --foreground        Stay in foreground.\n\
 -a, --listen_any        Use any/all IP addresses for UI TCP connections\n\
 -p, --listen_port       Listen for incoming UI connections on this TCP port\n\
 -r, --realtime          Try to run with real-time extensions.\n\
 -V, --version           Print version number and exit.\n\
 -o, --overload          Load .so.X file for slot specified, as slotID:file.so\n\
 -h, --help              Print usage message.\n\
 -s, --serialport        Use serial port specified not default port.\n\
";


/***************************************************************************
 *  - main():  It all begins here
 ***************************************************************************/
int main(int argc, char *argv[])
{
    int     i;      // loop counter

    // Ignore the SIGPIPE signal since that can occur if a
    // UI socket closes just before we try to write to it.
    (void) signal(SIGPIPE, SIG_IGN);

    // Set Locale so sscanf() is consistent.
    setlocale(LC_NUMERIC, "C");

    // Initialize globals for slots, timers, ui connections, and select fds
    globalinit();

    // Parse the command line and set global flags 
    processcmdline(argc, argv);
    (void) umask((mode_t) 000);

    // Become a daemon
    if (!ForegroundMode)
        daemonize();

    // Open serial port to the FPGA
    openfpgaserial();

    // The enumerator is a peripheral that queries the FPGA to get the
    // list of driver IDs in the FPGA.  The enumerator then looks up
    // the name of the .so file corresponding to each driver ID and
    // loads that driver
    add_so_slot("0:enumerator.so");

    // You can overload the FPGA driver list or add non-FPGa peripherals
    // here.  For example ...
    //add_so_slot("9:tts.so");

    // Start pcdaemon and the drivers loaded from the command line 
    for (i = 0; i < MX_SLOT; i++) {
        initslot(&(Slots[i]));
    }

    // invoke real-time extensions if specified
    if (RealtimeMode)
        invokerealtimeextensions();

    // Open the TCP listen port for UI connections
    open_ui_port();

    // Drop into the select loop and wait for events
    muxmain();

    return (0);
}


/***************************************************************************
 *  globalinit()   Initialize the global arrays
 *      Explicitly initializing every field is more part of the documentation
 *  than part of the code.
 ***************************************************************************/
void globalinit()
{
    int    i,j;    // loop counters

    // Initialize the driver table
    for (i = 0; i < MX_SLOT; i++) {
        Slots[i].slot_id = i;
        Slots[i].name    = (char *) NULL; // functional name of driver
        Slots[i].soname[0] = (char) 0;    // so file name
        Slots[i].handle  = (void *) NULL; // non-null when in use
        Slots[i].priv    = (void *) NULL;
        Slots[i].desc    = (void *) NULL;
        Slots[i].help    = (void *) NULL;
        for (j = 0; j < MX_RSC; j++) {
            Slots[i].rsc[j].name   = (char *) NULL;
            Slots[i].rsc[j].pgscb  = NULL;
            Slots[i].rsc[j].slot   = (void *) NULL;
            Slots[i].rsc[j].bkey   = 0;
            Slots[i].rsc[j].uilock = 0;
            Slots[i].rsc[j].flags  = 0;
        }
        if (i < NUM_CORE)
            Slots[i].pcore = &(Core[i]);  // Each core gets a slot, even if unused
    }

    // Init the Core structures
    for (i = 0; i < NUM_CORE; i++) {
        Core[i].slot_id   = -1;
        Core[i].core_id   = i;
        Core[i].driv_id   = 0;            // the null driver
        Core[i].pcb       = (void *) 0;   // non-null when in use
    }

    // Init table of file descriptors to use in select()
    for (i = 0; i < MX_FD; i++) {
        Pc_Fd[i].fd       = -1;
        Pc_Fd[i].stype    = 0;    // read, write, or except
        Pc_Fd[i].scb      = NULL; // callback on select() activity
        Pc_Fd[i].pcb_data = (void *) NULL; // data included in call of callback
    }

    // Init table of utility timers
    for (i = 0; i < MX_TIMER; i++) {
        Timers[i].type     = PC_UNUSED;
        Timers[i].type     = 0;           // one-shot, periodic, or unused
        Timers[i].to       = (long long) 0; // ms since Jan 1, 1970 to timeout
        Timers[i].us       = 0;           // period or timeout interval in uS
        Timers[i].cb       = NULL;        // Callback on timeout
        Timers[i].pcb_data = (void *) NULL; // data included in call of callbacks
    }

    // Init table of UI TCP connections
    for (i = 0; i <MX_UI; i++) {
        UiCons[i].cn = i;                 // Record index in struct
        UiCons[i].fd = -1;                // fd=-1 says ui is not in use
        UiCons[i].bkey = 0;               // if set, brdcst data from this slot/rsc
        UiCons[i].o_port = 0;             // Other-end TCP port number
        UiCons[i].o_ip = 0;               // Other-end IP address
        UiCons[i].cmdindx = 0;            // Index of next location in cmd buffer
        UiCons[i].cmd[0] = (char) 0;      // command from UI program
    }
}


/***************************************************************************
 *  processcmdline()   Process the command line
 ***************************************************************************/
void processcmdline(int argc, char *argv[])
{
    int      c;
    int      optidx = 0;
    static struct option longoptions[] = {
        {"stderr", 0, 0, 'e'},
        {"verbosity", 1, 0, 'v'},
        {"debug", 0, 0, 'd'},
        {"foreground", 0, 0, 'f'},
        {"realtime", 0, 0, 'r'},
        {"version", 0, 0, 'V'},
        {"listen_any", 0, 0, 'a'},
        {"listen_port", 1, 0, 'p'},
        {"overload", 1, 0, 'o'},
        {"help", 0, 0, 'h'},
        {"serialport", 1, 0, 's'},
        {0, 0, 0, 0}
    };
    static char optStr[] = "ev:dfrVs:p:ao:hs:";

    while (1) {
        c = getopt_long(argc, argv, optStr, longoptions, &optidx);
        if (c == -1)
            break;

        switch (c) {
            case 'e':
                UseStderr = 1;
                break;

            case 'v':
                Verbosity = atoi(optarg);
                Verbosity = (Verbosity < PC_VERB_OFF)  ? PC_VERB_OFF : Verbosity;
                Verbosity = (Verbosity > PC_VERB_TRACE) ? PC_VERB_TRACE : Verbosity;
                break;

            case 'd':
                DebugMode = 1;
                ForegroundMode = 1;
                break;

            case 'f':
                ForegroundMode = 1;
                break;

            case 'a':
                UiaddrAny = 1;
                break;

            case 'p':
                UiPort = atoi(optarg);
                break;

            case 'r':
                RealtimeMode = 1;
                break;

            case 's':
                SerialPort = optarg;
                break;

            case 'V':
                printf("%s\n", versionStr);
                exit(-1);
                break;

            case 'o':
                add_so_slot(optarg);
                break;

            default:
                printf("%s", helpText);
                exit(-1);
        }
    }

    // At this point we've gotten all of the command line options.
    // Save the command line name for this program
    CmdName = argv[0];
}

/***************************************************************************
 *  Become a daemon
 ***************************************************************************/
void daemonize()
{
    pid_t    dpid;
    int      maxFd;
    int      fd;
    int      i;

    // go into the background
    if ((dpid = fork()) < 0) {
        pclog(M_NOFORK, strerror(errno));
        exit(-1);
    }
    if (dpid > 0) {
        // exit if parent
        exit(0);
    }

    // become process and session leader
    if ((dpid = setsid()) < 0) {
        pclog(M_NOSID, strerror(errno));
        exit(-1);
    }

    // set the current directory
    if (chdir("/") < 0) {
        pclog(M_NOCD, strerror(errno));
        exit(-1);
    }

    // redirect stio to /dev/null
    close(STDIN_FILENO);
    fd = open("/dev/null", O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        pclog(M_NONULL, strerror(errno));
        exit(-1);
    }
    else if (fd != 0) {
        // no stdio
        pclog(M_NOREDIR, "stdin");
        exit(-1);
    }
    close(STDOUT_FILENO);
    fd = open("/dev/null", O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        pclog(M_NONULL, strerror(errno));
        exit(-1);
    }
    else if (fd != 1) {
        // no stdio
        pclog(M_NOREDIR, "stdout");
        exit(-1);
    }
    if (!UseStderr) {
        // do not redirect if forced to use stderr
        close(STDERR_FILENO);
        fd = open("/dev/null", O_RDONLY | O_NOCTTY);
        if (fd < 0) {
            pclog(M_NONULL, strerror(errno));
            exit(-1);
        }
        else if (fd != 2) {
            // no stdio
            pclog(M_NOREDIR, "stderr");
            exit(-1);
        }
    }

    // close all non-stdio
    maxFd = getdtablesize();
    for (i = 3; i < maxFd; i++) {
        close(i);
    }

    // reset the file modes
    (void) umask((mode_t) 000);
}


/***************************************************************************
 *  Give the daemon the highest scheduling priority possible
 ***************************************************************************/
void invokerealtimeextensions()
{
    struct sched_param sp;
    int      policy;

    // change the static priority to the highest possible and set FIFO
    // scheduling
    if ((pthread_getschedparam(pthread_self(), &policy, & sp) != 0)) {
        pclog(M_BADSCHED, strerror(errno));
    }
    if (policy == SCHED_OTHER) {
        sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            pclog(M_BADSCHED, strerror(errno));
        }
    }

    // lock all current and future memory pages
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        pclog(M_BADMLOCK, strerror(errno));
    }
}


/***************************************************************************
 *  Open serial port to the FPGA
 ***************************************************************************/
void openfpgaserial()
{
    struct termios tbuf;       // set baud rate
    int      actions;

    fpgaFD = open(SerialPort, (O_RDWR), 0);
    if (fpgaFD < 0) {
        pclog(M_BADPORT, SerialPort, strerror(errno));
        exit(-1);
    }

    // port is open and can be configured
    tbuf.c_cflag = CS8 | CREAD | CLOCAL | DEFFPGABAUD;
    tbuf.c_iflag = IGNBRK;
    tbuf.c_oflag = 0;
    tbuf.c_lflag = 0;
    tbuf.c_cc[VMIN] = 1;        /* character-by-character input */
    tbuf.c_cc[VTIME] = 0;       /* no delay waiting for characters */
    actions = TCSANOW;
    if (tcsetattr(fpgaFD, actions, &tbuf) < 0) {
        pclog(M_BADPORT, SerialPort, strerror(errno));
        exit(-1);
    }

    // add callback for received characters
    add_fd(fpgaFD, PC_READ, receivePkt, (void *) 0);

    return;
}

// end of main.c

