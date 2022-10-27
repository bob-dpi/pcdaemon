/*
 *  Name: basys3.c
 *
 *  Description: Driver for the Digilent Basys3 FPGA card
 *
 *  Hardware Registers:
 *    0-2: switches   - Switch and button values
 *    4-7: segments   - Segment values.  Reg 4 is right-most display
 *   64: Drivlist     - table of 16 16-bit peripheral driver ID values
 * 
 *  Resources:
 *    switches     - 6 digit hex value of the 16 switches and 5 buttons
 *    display      - 4 digit display as characters
 *    segments     - 4 digit display as individual segments
 *    drivlist     - List of requested drivers for this FPGA build
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
#include <stdint.h>
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
        // BASYS register definitions
#define BASYS_REG_SWITCHES  0x00
#define BASYS_REG_DISPLAY   0x04
#define BASYS_REG_DRIVLIST  0x40
        // Resource names
#define FN_DRIVLIST         "drivlist"
#define FN_SWITCHES         "switches"
#define FN_SEGMENTS         "segments"
#define FN_DISPLAY          "display"
        // Resource index numbers
#define RSC_DRIVLIST        0
#define RSC_SWITCHES        1
#define RSC_SEGMENTS        2
#define RSC_DISPLAY         3
        // The display has 4 digits
#define NDIGITS             4


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an basys3
typedef struct
{
    void    *pslot;                 // handle to peripheral's slot info
    uint32_t lastswitch;            // last reported value of the switches
    char     text[(NDIGITS*2) +1];  // Display characters
    int      segs[NDIGITS];         // Array of segment values
    void    *ptimer;                // timer to watch for dropped ACK packets
    int      drivlist[NUM_CORE];    // list of peripheral IDs
} BASYSDEV;

    // character to 7-segment mapping
typedef struct
{
    char sym;               // character to map
    int  segval;            // 7 segment equivalent
} SYMBOL;

SYMBOL symbols[] = {   // segments MSB -> pgfedcba <- LSB
    {'0', 0x3f }, {'1', 0x06 }, {'2', 0x5b }, {'3', 0x4f },
    {'4', 0x66 }, {'5', 0x6d }, {'6', 0x7d }, {'7', 0x07 },
    {'8', 0x7f }, {'9', 0x67 }, {'a', 0x77 }, {'b', 0x7c },
    {'c', 0x39 }, {'d', 0x5e }, {'e', 0x79 }, {'f', 0x71 },
    {'A', 0x77 }, {'B', 0x7c }, {'C', 0x39 }, {'D', 0x5e },
    {'E', 0x79 }, {'F', 0x71 }, {'o', 0x5c }, {'L', 0x38 },
    {'r', 0x50 }, {'h', 0x74 }, {'H', 0x76 }, {'-', 0x40 },
    {' ', 0x00 }, {'_', 0x08 }, {'u', 0x1c }, {'.', 0x00 }
};
#define NSYM (sizeof(symbols) / sizeof(SYMBOL))


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, PC_PKT *, int);
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void text_to_segs(char *, int *);
static void getdriverlist(BASYSDEV *);
static int  board2tofpga(BASYSDEV *);
static void noAck(void *, BASYSDEV *);
extern int  pc_tx_pkt(CORE *pcore, PC_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    BASYSDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (BASYSDEV *) malloc(sizeof(BASYSDEV));
    if (pctx == (BASYSDEV *) 0) {
        // Malloc failure this early?
        pclog("memory allocation failure in basys3 initialization");
        return (-1);
    }

    // Init our BASYSDEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_SWITCHES].name = FN_SWITCHES;
    pslot->rsc[RSC_SWITCHES].flags = IS_READABLE | CAN_BROADCAST;
    pslot->rsc[RSC_SWITCHES].bkey = 0;
    pslot->rsc[RSC_SWITCHES].pgscb = usercmd;
    pslot->rsc[RSC_SWITCHES].uilock = -1;
    pslot->rsc[RSC_SWITCHES].slot = pslot;
    pslot->rsc[RSC_DISPLAY].name = FN_DISPLAY;
    pslot->rsc[RSC_DISPLAY].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_DISPLAY].bkey = 0;
    pslot->rsc[RSC_DISPLAY].pgscb = usercmd;
    pslot->rsc[RSC_DISPLAY].uilock = -1;
    pslot->rsc[RSC_DISPLAY].slot = pslot;
    pslot->rsc[RSC_SEGMENTS].name = FN_SEGMENTS;
    pslot->rsc[RSC_SEGMENTS].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_SEGMENTS].bkey = 0;
    pslot->rsc[RSC_SEGMENTS].pgscb = usercmd;
    pslot->rsc[RSC_SEGMENTS].uilock = -1;
    pslot->rsc[RSC_SEGMENTS].slot = pslot;
    pslot->rsc[RSC_DRIVLIST].name = FN_DRIVLIST;
    pslot->rsc[RSC_DRIVLIST].flags = IS_READABLE;
    pslot->rsc[RSC_DRIVLIST].bkey = 0;
    pslot->rsc[RSC_DRIVLIST].pgscb = usercmd;
    pslot->rsc[RSC_DRIVLIST].uilock = -1;
    pslot->rsc[RSC_DRIVLIST].slot = pslot;
    pslot->name = "basys3";
    pslot->desc = "The switches, buttons, and displays on the Basys3";
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
    BASYSDEV *pctx;      // our local info
    RSC     *prsc;       // pointer to this packet's resource
    char     swval[9];   // ASCII value of switches "xxxxxx\n"
    int      swvallen;   // #chars in switches, should be 7
    int      i;          // loop counter
    uint32_t newswitch;  // reported value of the switches/buttons

    pctx = (BASYSDEV *)(pslot->priv);  // Our "private" data is a BASYSDEV

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
        (pkt->reg == BASYS_REG_DRIVLIST) && (pkt->count == (2 * NUM_CORE))) {
        // copy drivlist read results to drivlist table
        for (i = 0; i < NUM_CORE; i++) {
            pctx->drivlist[i] = (pkt->data[2*i] << 8) + pkt->data[2*i +1];
        }
        del_timer(pctx->ptimer);  //Got the response
        pctx->ptimer = 0;
    }

    // if not the peripheral list, must be the switches.
    prsc = &(pslot->rsc[RSC_SWITCHES]);

    // If a read response from a user pcget command, send value to UI
    if (((pkt->cmd & PC_CMD_AUTO_MASK) != PC_CMD_AUTO_DATA) &&
        (pkt->reg == BASYS_REG_SWITCHES) && (pkt->count == 3)) {
        swvallen = sprintf(swval, "%02x%02x%02x\n", pkt->data[2], pkt->data[1], pkt->data[0]);
        send_ui(swval, swvallen, prsc->uilock);
        prompt(prsc->uilock);
        // Response sent so clear the lock
        prsc->uilock = -1;
        del_timer(pctx->ptimer);  //Got the response
        pctx->ptimer = 0;
    }

    // Process of elimination makes this an autosend switch update.
    // Broadcast it if any UI are monitoring it.
    else if (prsc->bkey != 0) {
        // Pressing the buttons simultaneously can cause duplicate packet
        // to be sent up from the hardware.  We filter that out here.
        // This only needs to be done on autosend packets.
        newswitch = (pkt->data[2] << 16) + (pkt->data[1] << 8) + pkt->data[0];
        if (newswitch != pctx->lastswitch) {
            swvallen = sprintf(swval, "%06x\n", newswitch);
            // bkey will return cleared if UIs are no longer monitoring us
            bcst_ui(swval, swvallen, &(prsc->bkey));
        }
        pctx->lastswitch = newswitch;
    }

    return;
}


/**************************************************************
 * usercmd():  - The user is reading the switches, setting the
 * display, or getting the drivlist.
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
    BASYSDEV *pctx;    // our local info
    PC_PKT    pkt;     // send write and read cmds to the basys3
    int       ret;     // return count
    int       txret;   // ==0 if the packet went out OK
    CORE     *pmycore; // FPGA peripheral info
    int       i;       // loop counter
    int       s0,s1,s2,s3;  // display as segemnt values


    pctx = (BASYSDEV *) pslot->priv;
    pmycore = pslot->pcore;


    // Is this a display update?
    if ((cmd == PCSET) && (rscid == RSC_DISPLAY )) {
        strncpy(pctx->text, val, (2 * NDIGITS));
        pctx->text[(2 * NDIGITS)] = (char) 0;
        text_to_segs(pctx->text, pctx->segs);

        txret =  board2tofpga(pctx);   // Send segments to device
        if (txret != 0) {
            // the send of the new outval did not succeed.  This probably
            // means the input buffer to the USB port is full.  Tell the
            // user of the problem.
            ret = snprintf(buf, *plen, E_WRFPGA);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
    }
    else if ((cmd == PCGET) && (rscid == RSC_DISPLAY)) {
        ret = snprintf(buf, *plen, "%s\n", pctx->text);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == PCSET) && (rscid == RSC_SEGMENTS)) {
        ret = sscanf(val, "%x %x %x %x", &s0, &s1, &s2, &s3);
        if ((ret != 4) || (s0 < 0) || (s0 > 0xff) ||
                          (s1 < 0) || (s1 > 0xff) ||
                          (s2 < 0) || (s2 > 0xff) ||
                          (s3 < 0) || (s3 > 0xff)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->segs[0] = s0;
        pctx->segs[1] = s1;
        pctx->segs[2] = s2;
        pctx->segs[3] = s3;

        txret =  board2tofpga(pctx);   // This peripheral's context
        if (txret != 0) {
            // the send of the new outval did not succeed.  This probably
            // means the input buffer to the USB port is full.  Tell the
            // user of the problem.
            ret = snprintf(buf, *plen, E_WRFPGA);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
    }
    else if ((cmd == PCGET) && (rscid == RSC_SEGMENTS)) {
        ret = snprintf(buf, *plen, "%02x %02x %02x %02x\n",
                  pctx->segs[0], pctx->segs[1], pctx->segs[2], pctx->segs[2]);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == PCGET) && (rscid == RSC_SWITCHES)) {
        // create a read packet to get the current value of the pins
        pkt.cmd = PC_CMD_OP_READ | PC_CMD_AUTOINC;
        pkt.core = (pslot->pcore)->core_id;
        pkt.reg = BASYS_REG_SWITCHES;
        pkt.count = 3;

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
        pslot->rsc[RSC_SWITCHES].uilock = (char) cn;

        // Nothing to send back to the user
        *plen = 0;
    }
    else if ((cmd == PCGET) && (rscid == RSC_DRIVLIST)) {
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

    return;
}


/**************************************************************
 * getdriverlist():  - Read the list of peripheral IDs in the basys3
 **************************************************************/
static void getdriverlist(
    BASYSDEV *pctx)    // This peripheral's context
{
    PC_PKT   pkt;      // send write and read cmds to the basys3
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info

    pmyslot = pctx->pslot;
    pmycore = (CORE *)pmyslot->pcore;

    // Get the list of peripherals
    pkt.cmd = PC_CMD_OP_READ | PC_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = BASYS_REG_DRIVLIST;
    pkt.count = 32;
    (void) pc_tx_pkt(pmycore, &pkt, 4);

    // Start timer to look for a read response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(PC_ONESHOT, 100, noAck, (void *) pctx);

    return;
}


/**************************************************************
 * text_to_segs():  - Convert the given text to its 7-segment
 * equivalent.
 **************************************************************/
static void text_to_segs(char *text, int *segs)
{
    int   i;           // index into segs[]
    int   j;           // index into symbols[]
    int   k;           // index into text

    k = 0;
    for (i = 0; i < NDIGITS; i++) {
        segs[i] = 0;

        for (j = 0; j < NSYM; j++) {
            if (text[k] == symbols[j].sym) {
                segs[i] = symbols[j].segval;
                break;
            }
        }

        if ((text[k] != '.') && (text[k+1] == '.')) {
            segs[i] |= 0x80;     // decimal point is MSB of segments
            k++;
        }
        k++;
    }
}


/**************************************************************
 * board2tofpga():  - Send seven segment values
 * zero on success
 **************************************************************/
int board2tofpga(
    BASYSDEV *pctx)     // This peripheral's context
{
    PC_PKT    pkt;      // send write and read cmds to the out4
    SLOT     *pmyslot;  // This peripheral's slot info
    CORE     *pmycore;  // FPGA peripheral info
    int       txret;    // ==0 if the packet went out OK

    pmyslot = pctx->pslot;
    pmycore = pmyslot->pcore;

    // Got a new value for the LEDs and segments.  Send down to the card.
    pkt.cmd = PC_CMD_OP_WRITE | PC_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = BASYS_REG_DISPLAY;
    pkt.count = 4;
    pkt.data[0] = pctx->segs[0];             // left hand digit
    pkt.data[1] = pctx->segs[1];             // 
    pkt.data[2] = pctx->segs[2];             //
    pkt.data[3] = pctx->segs[3];             // right hand digit
    txret = pc_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    // Start timer to look for a write response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(PC_ONESHOT, 100, noAck, (void *) pctx);

    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void     *timer,   // handle of the timer that expired
    BASYSDEV *pctx)    // 
{
    // Log the missing ack
    pclog(E_NOACK);

    return;
}
// end of basys3.c
