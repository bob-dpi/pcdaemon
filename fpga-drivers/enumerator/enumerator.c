/*
 *  Name: enumerator.c
 *
 *  Description: Interface to the driver list in a Demand Peripherals FPGA
 *              image.  The ID numbers a converted to file names which are
 *              added to the system using dlopen().
 *              
 *  Hardware Registers:
 *   64: Perilist  - table of 16 16-bit peripheral ID values
 * 
 *  Resources:
 *    drivlist  - list of driver identification numbersthe FPGA image
 */

/*
 * Copyright:   Copyright (C) 2022 by Demand Peripherals, Inc.
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
 *              Please contact Demand Peripherals if you wish to use this code
 *              in a non-GPLv2 compliant manner. 
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <limits.h>              // for PATH_MAX
#include <sys/fcntl.h>
#include <sys/types.h>
#include "daemon.h"
#include "readme.h"
#include "drivlist.h"            // Relates driver ID so .so file name




/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // enumerator register definitions
#define ENUM_REG_DRIVLIST   0x40
        // Resource names
#define FN_DRIVLIST         "drivlist"
        // Resource index numbers
#define RSC_DRIVLIST        0
        // What we are is a ...
#define PLUGIN_NAME        "enumerator"
#define MX_MSGLEN 1000
        // Enumerator is always in core #0
#define COREZERO            0



/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an bb4io
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
} ENUMDEV;


/**************************************************************
 *  - Function prototypes and externs
 **************************************************************/
static void  packet_hdlr(SLOT *, PC_PKT *, int);
static void  usercmd(int, int, char*, SLOT*, int, int*, char*);
static void  getdriverlist(ENUMDEV *);
static void  noAck(void *, ENUMDEV *);
extern int   pc_tx_pkt(CORE *pcore, PC_PKT *inpkt, int len);
static void  getSoName(char *, int);

extern int   add_so(char *);
extern void  initslot(SLOT *);
extern SLOT  Slots[];
extern CORE  Core[];
extern int   useStderr;
extern int   DebugMode;
extern int   ForegroundMode;
extern int   Verbosity;



/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    ENUMDEV *pctx;     // our local device context

    // Allocate memory for this peripheral
    pctx = (ENUMDEV *) malloc(sizeof(ENUMDEV));
    if (pctx == (ENUMDEV *) 0) {
        // Malloc failure this early?
        pclog("memory allocation failure in enumerator initialization");
        return (-1);
    }

    // Init our ENUMDEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_DRIVLIST].name = FN_DRIVLIST;
    pslot->rsc[RSC_DRIVLIST].flags = IS_READABLE;
    pslot->rsc[RSC_DRIVLIST].bkey = 0;
    pslot->rsc[RSC_DRIVLIST].pgscb = usercmd;
    pslot->rsc[RSC_DRIVLIST].uilock = -1;
    pslot->rsc[RSC_DRIVLIST].slot = pslot;
    pslot->name = "enumerator";
    pslot->desc = "The table of driver IDs for this FPGA image";
    pslot->help = README;
    pslot->priv = pctx;

    // Allocate and initialize CORE #0 for the enumerator.
    // This allocation will be overwritten by the driver list
    pslot->pcore = &(Core[COREZERO]);
    Core[COREZERO].pcb  = packet_hdlr;
    Core[COREZERO].slot_id  = pslot->slot_id;

    (void) getdriverlist(pctx);

    return (0);
}




/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 *    Get the driver list and store it in the table of COREs.
 *    Allocate a SLOT for each non-zero driver ID.
 *    Look up the plug-in .so file name based on the driver ID.
 *    Use initslot() to do a dlopen() * on the .so file, save
 *    the handle, and call Initialize for the driver.
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,      // handle for our slot's internal info
    PC_PKT  *pkt,        // the received packet
    int      len)        // number of bytes in the received packet
{
    ENUMDEV *pctx;       // our local info
    int      slot;       // index into the SLOTs table
    int      i;          // loop counter

    pctx = (ENUMDEV *)(pslot->priv);  // Our "private" data is a ENUMDEV

    // Clear the timer on write response packets
    if ((pkt->cmd & PC_CMD_OP_MASK) == PC_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // Response to Driver ID table read ?
    if (((pkt->cmd & PC_CMD_AUTO_MASK) != PC_CMD_AUTO_DATA) &&
        (pkt->reg == ENUM_REG_DRIVLIST) && (pkt->count == (2 * NUM_CORE))) {

        del_timer(pctx->ptimer);  //Got the response
        pctx->ptimer = 0;

        // Process each driver ID in the response
        // Allocate slots starting from zero.  
        slot = 0;
        for (i = 0; i < NUM_CORE; i++) {
            // Get the driver ID and store it in the table of COREs. */
            Core[i].driv_id = (pkt->data[2*i] << 8) + pkt->data[2*i +1];

            // Allocate a SLOT for each non-zero driver ID.
            if (Core[i].driv_id == 0)
                continue;

            if (slot == MX_SLOT) {
                pclog("Unable to allocate a SLOT for core # %d", i);
                return;
            }
            Slots[slot].pcore = &(Core[i]);
            Core[i].slot_id = slot;

            // Get the plug-in .so file name based on the driver ID.
            getSoName(Slots[slot].soname, Core[i].driv_id);

            // Use initslot() to do the dlopen() and initialize the
            // the slot.
            initslot(&(Slots[slot]));

            // Set up for next core to process
            slot++;
        }
    }

    return;
}


/**************************************************************
 * usercmd():  - The user is reading the drivlist.
 **************************************************************/
static void usercmd(
    int      cmd,      //==PCGET if a read, ==PCSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    ENUMDEV *pctx;    // our local info
    int       ret;     // return count
    int       i;       // loop counter


    pctx = (ENUMDEV *) pslot->priv;

    if ((cmd == PCGET) && (rscid == RSC_DRIVLIST)) {
        // verify there is room in the buffer for the output.  Each
        // peripheral ID is four hex characters plus a space/newline + null.
        if (*plen < ((5 * NUM_CORE) +10)) {
            // no room for output.  Send nothing
            *plen = 0;
            return;
        }
        ret = 0;
        for (i = 0; i < 16; i++) {
            ret += snprintf(&(buf[ret]), (*plen-ret), "%04x ", Core[i].driv_id);
        }
        // replace last space with a newline
        buf[ret-1] = '\n';
        *plen = ret;  // (errors are handled in calling routine)
    }

    return;
}



/**************************************************************
 * getSoName():  - Get the peripheral driver name based on the
 * driver ID read from the FPGA.  Set to a null if the driver ID
 * is not in the table.  Copy at most (MX_SONAME-1) characters
 * including the library prefix passed from the Makefile.
 **************************************************************/
static void getSoName(
    char    *soname,   // where to save the plug-in driver file path
    int      driv_id)
{
    int      i;        // loop index

    // A user may have "overloaded" the FPGA specified driver with
    // with one given using the -o option on the command line.  If
    // so, the soname will not be null and we can just return so
    // the user specified driver is loaded.
    // The except is slot #0 -- we usually want the enumerator to
    // overloaded by the board IO driver specified in the FPGA.
    if ((*soname != (char) 0) && (strncmp(soname, "enumerator.so", MX_SONAME) != 0))
        return;

    for (i = 0; i < NPERI; i++) {
        if (pdesc[i].drivid != driv_id)
            continue;         // not the driver we seek

        // At this point the driver ID matches one in the table.
        // Copy the library path, a '/', and the file name, and
        // a ".so" to the input soname
        (void) snprintf(soname, (MX_SONAME-1),"%s.so", pdesc[i].periname);
        return;
    }

    // If we get here the name was not found in the table.  Log this error.
    pclog("Unable to find driver file name for driver ID: %d", driv_id);
    return;
}


/**************************************************************
 * getdriverlist():  - Read the list of peripheral IDs in the bb4io
 **************************************************************/
static void getdriverlist(
    ENUMDEV *pctx)    // This peripheral's context
{
    PC_PKT   pkt;      // send write and read cmds to the bb4io
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info

    pmyslot = pctx->pslot;
    pmycore = (CORE *)pmyslot->pcore;

    // Get the list of peripherals
    pkt.cmd = PC_CMD_OP_READ | PC_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = ENUM_REG_DRIVLIST;
    pkt.count = (2 * NUM_CORE);
    (void) pc_tx_pkt(pmycore, &pkt, 4);

    // Start timer to look for a read response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(PC_ONESHOT, 100, noAck, (void *) pctx);

    return;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void     *timer,   // handle of the timer that expired
    ENUMDEV *pctx)    // 
{
    // Log the missing ack
    pclog(E_NOACK);

    return;
}

// end of enumerator.c



