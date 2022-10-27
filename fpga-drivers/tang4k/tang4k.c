/*
 *  Name: tang4k.c
 *
 *  Description: Driver for the buttons and drivlist on the FPGA card
 *
 *  Hardware Registers:
 *    0: buttons   - 8-bit read only
 *   64: Drivlist  - table of 16 16-bit peripheral driver ID values
 * 
 *  Resources:
 *    buttons      - broadcast ASCII auto-data from buttons
 *
 * Copyright:   Copyright (C) 2014-2022 Demand Peripherals, Inc.
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
 * 
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "daemon.h"
#include "readme.h"

/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // TNG4K register definitions
#define TNG4K_REG_BUTTONS   0x00
#define TNG4K_REG_DRIVLIST  0x40
        // Resource names
#define FN_BUTTONS          "buttons"
#define FN_DRIVLIST         "drivlist"
        // Resource index numbers
#define RSC_BUTTONS         0
#define RSC_DRIVLIST        1


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an tang4k
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    int      drivlist[NUM_CORE];  // list of peripheral IDs
} TNG4KDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, PC_PKT *, int);
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void getdriverlist(TNG4KDEV *);
static void noAck(void *, TNG4KDEV *);
extern int  pc_tx_pkt(CORE *pcore, PC_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    TNG4KDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (TNG4KDEV *) malloc(sizeof(TNG4KDEV));
    if (pctx == (TNG4KDEV *) 0) {
        // Malloc failure this early?
        pclog("memory allocation failure in tang4k initialization");
        return (-1);
    }

    // Init our TNG4KDEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_BUTTONS].name = FN_BUTTONS;
    pslot->rsc[RSC_BUTTONS].flags = IS_READABLE | CAN_BROADCAST;
    pslot->rsc[RSC_BUTTONS].bkey = 0;
    pslot->rsc[RSC_BUTTONS].pgscb = usercmd;
    pslot->rsc[RSC_BUTTONS].uilock = -1;
    pslot->rsc[RSC_BUTTONS].slot = pslot;
    pslot->rsc[RSC_DRIVLIST].name = FN_DRIVLIST;
    pslot->rsc[RSC_DRIVLIST].flags = IS_READABLE;
    pslot->rsc[RSC_DRIVLIST].bkey = 0;
    pslot->rsc[RSC_DRIVLIST].pgscb = usercmd;
    pslot->rsc[RSC_DRIVLIST].uilock = -1;
    pslot->rsc[RSC_DRIVLIST].slot = pslot;
    pslot->name = "tang4k";
    pslot->desc = "The buttons and peripheral list on the Tang Nano 4K";
    pslot->help = README;

    (void) getdriverlist(pctx);

    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,      // handle for our slot's internal info
    PC_PKT  *pkt,        // the received packet
    int      len)        // number of bytes in the received packet
{
    TNG4KDEV *pctx;      // our local info
    RSC     *prsc;       // pointer to this packet's resource
    char     buttons[9]; // ASCII value of buttons "xx\n"
    int      buttonlen;  // #chars in buttons, should be 3
    int      i;          // loop counter

    pctx = (TNG4KDEV *)(pslot->priv);  // Our "private" data is a TNG4KDEV

    // Clear the timer on write response packets
    if ((pkt->cmd & PC_CMD_OP_MASK) == PC_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // If a read response from a user pcget command, send value to UI
    if (((pkt->cmd & PC_CMD_AUTO_MASK) != PC_CMD_AUTO_DATA) &&
        (pkt->reg == TNG4K_REG_DRIVLIST) && (pkt->count == (2 * NUM_CORE))) {
        // copy drivlist read results to drivlist table
        for (i = 0; i < NUM_CORE; i++) {
            pctx->drivlist[i] = (pkt->data[2*i] << 8) + pkt->data[2*i +1];
        }
        del_timer(pctx->ptimer);  //Got the response
        pctx->ptimer = 0;
    }

    // if not the peripheral list, must be the buttons.
    prsc = &(pslot->rsc[RSC_BUTTONS]);

    // If a read response from a user pcget command, send value to UI
    if (((pkt->cmd & PC_CMD_AUTO_MASK) != PC_CMD_AUTO_DATA) &&
        (pkt->reg == TNG4K_REG_BUTTONS) && (pkt->count == 1)) {
        buttonlen = sprintf(buttons, "%02x\n", pkt->data[0]);
        send_ui(buttons, buttonlen, prsc->uilock);
        prompt(prsc->uilock);
        // Response sent so clear the lock
        prsc->uilock = -1;
        del_timer(pctx->ptimer);  //Got the response
        pctx->ptimer = 0;
    }

    // Process of elimination makes this an autosend button update.
    // Broadcast it if any UI are monitoring it.
    else if (prsc->bkey != 0) {
        buttonlen = sprintf(buttons, "%02x\n", pkt->data[0]);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(buttons, buttonlen, &(prsc->bkey));
    }

    return;
}


/**************************************************************
 * usercmd():  - The user is reading the buttons or the drivlist.
 * Get the value from the baseboard if needed and write into the
 * the supplied buffer.
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
    TNG4KDEV *pctx;    // our local info
    PC_PKT    pkt;     // send write and read cmds to the tang4k
    int       ret;     // return count
    int       txret;   // ==0 if the packet went out OK
    CORE     *pmycore; // FPGA peripheral info
    int       i;       // loop counter


    pctx = (TNG4KDEV *) pslot->priv;
    pmycore = pslot->pcore;


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
            ret += snprintf(&(buf[ret]), (*plen-ret), "%04x ", pctx->drivlist[i]);
        }
        // replace last space with a newline
        buf[ret-1] = '\n';
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == PCGET) && (rscid == RSC_BUTTONS)) {
        // create a read packet to get the current value of the pins
        pkt.cmd = PC_CMD_OP_READ | PC_CMD_NOAUTOINC;
        pkt.core = (pslot->pcore)->core_id;
        pkt.reg = TNG4K_REG_BUTTONS;
        pkt.count = 1;

        // send the packet.  Report any errors
        txret = pc_tx_pkt(pmycore, &pkt, 4);
        if (txret != 0) {
            ret = snprintf(buf, *plen, E_WRFPGA);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }

        // Start timer to look for a read response.
        if (pctx->ptimer == 0)
            pctx->ptimer = add_timer(PC_ONESHOT, 100, noAck, (void *) pctx);

        // lock this resource to the UI session cn
        pslot->rsc[RSC_BUTTONS].uilock = (char) cn;

        // Nothing to send back to the user
        *plen = 0;
    }

    return;
}


/**************************************************************
 * getdriverlist():  - Read the list of peripheral IDs in the tang4k
 **************************************************************/
static void getdriverlist(
    TNG4KDEV *pctx)    // This peripheral's context
{
    PC_PKT   pkt;      // send write and read cmds to the tang4k
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info

    pmyslot = pctx->pslot;
    pmycore = (CORE *)pmyslot->pcore;

    // Get the list of peripherals
    pkt.cmd = PC_CMD_OP_READ | PC_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = TNG4K_REG_DRIVLIST;
    pkt.count = 32;
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
    TNG4KDEV *pctx)    // 
{
    // Log the missing ack
    pclog(E_NOACK);

    return;
}
// end of tang4k.c
