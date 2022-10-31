/*
 *  Name: axo2.c
 *
 *  Description: Custom interface to the Axelsys MachXO2 FPGA card.
 *              The board has no buttons and the LEDs are tied internally
 *              to the first two peripherals.
 *              This peripheral is loaded in slot 0 to replace the enumerator.
 *              
 *  Hardware Registers:
 *              None
 * 
 *  Resources:
 *              drivlist  - list of driver identification numbers in the FPGA
 *                          image
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
        // axo2 register definitions
#define AXO2_REG_DRIVLIST   0x40
        // Resource names
#define FN_DRIVLIST         "drivlist"
        // Resource index numbers
#define RSC_DRIVLIST        0
        // What we are is a ...
#define PLUGIN_NAME        "axo2"
#define MX_MSGLEN 1000
        // Axo2 is always in core #0
#define COREZERO            0



/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an bb4io
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
} AXO2DEV;


/**************************************************************
 *  - Function prototypes and externs
 **************************************************************/
static void  usercmd(int, int, char*, SLOT*, int, int*, char*);
extern CORE  Core[];



/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    AXO2DEV *pctx;     // our local device context

    // Allocate memory for this peripheral
    pctx = (AXO2DEV *) malloc(sizeof(AXO2DEV));
    if (pctx == (AXO2DEV *) 0) {
        // Malloc failure this early?
        pclog("memory allocation failure in axo2 initialization");
        return (-1);
    }

    // Init our AXO2DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_DRIVLIST].name = FN_DRIVLIST;
    pslot->rsc[RSC_DRIVLIST].flags = IS_READABLE;
    pslot->rsc[RSC_DRIVLIST].bkey = 0;
    pslot->rsc[RSC_DRIVLIST].pgscb = usercmd;
    pslot->rsc[RSC_DRIVLIST].uilock = -1;
    pslot->rsc[RSC_DRIVLIST].slot = pslot;
    pslot->name = "axo2";
    pslot->desc = "Axelsys MachXO2 board peripherals";
    pslot->help = README;
    pslot->priv = pctx;

    return (0);
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
    int       ret;     // return count
    int       i;       // loop counter


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

// end of axo2.c

