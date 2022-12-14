/*
 *  Name: out32.c
 *
 *  Description: Device driver for the out32 peripheral
 *
 *  Hardware Registers:
 *    0: outval    - output bits 16 and 0 in bit 1, bit 0
 *    1: outval    - output bits 17 and 1 in bit 1, bit 0
 *    :                           :     :
 *    15: outval   - output bits 31 and 15 in bit 1, bit 0
 * 
 *  Resources:
 *    outval       - 32 bit output value as a single hex number
 *
 * Copyright:   Copyright (C) 2014-2019 Demand Peripherals, Inc.
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
        // OUT32 register definitions
#define OUT32_REG_OUTVAL    0x00
        // line length from user to set OUT32 value ( e.g. "fedcba9876543210\n")
#define OUTVAL_LEN          24
#define OUT32PTKLEN         16   /* 16 data byte in packet to send */
#define FN_OUTVAL           "outval"
        // Resource index numbers
#define RSC_OUTVAL          0


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an out32
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    uint32_t outval;   // Current value of the outputs
    void    *ptimer;   // timer to watch for dropped ACK packets
} OUT32DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, PC_PKT *, int);
static void out32user(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, OUT32DEV *);
static int  out32tofpga(OUT32DEV *);
extern int  pc_tx_pkt(CORE *pcore, PC_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    OUT32DEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (OUT32DEV *) malloc(sizeof(OUT32DEV));
    if (pctx == (OUT32DEV *) 0) {
        // Malloc failure this early?
        pclog("memory allocation failure in out32 initialization");
        return (-1);
    }

    // Init our OUT32DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->outval = 0x0;        // Matches Verilog default value
    pctx->ptimer = 0;          // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_OUTVAL].name = "outval";
    pslot->rsc[RSC_OUTVAL].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_OUTVAL].bkey = 0;
    pslot->rsc[RSC_OUTVAL].pgscb = out32user;
    pslot->rsc[RSC_OUTVAL].uilock = -1;
    pslot->rsc[RSC_OUTVAL].slot = pslot;
    pslot->name = "out32";
    pslot->desc = "32 Channel Digital Output";
    pslot->help = README;

    (void) out32tofpga(pctx);

    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT   *pslot,     // handle for our slot's internal info
    PC_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    OUT32DEV *pctx;    // our local info

    pctx = (OUT32DEV *)(pslot->priv);  // Our "private" data is a OUT32DEV

    // Clear the timer on write response packets
    if ((pkt->cmd & PC_CMD_OP_MASK) == PC_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // Do a sanity check on the received packet.
    if ((pkt->reg != OUT32_REG_OUTVAL) || (pkt->count != OUT32PTKLEN)) {
        pclog("invalid out32 packet from board to host");
        return;
    }

    return;
}


/**************************************************************
 * out32user():  - The user is reading or writing to the output.
 * Get the value and update the out32 on the BaseBoard or read the
 * value and write it into the supplied buffer.
 **************************************************************/
static void out32user(
    int      cmd,      //==PCGET if a read, ==PCSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    OUT32DEV *pctx;    // our local info
    int      ret;      // return count
    uint32_t newout32; // new value to assign the out32
    int      txret;    // ==0 if the packet went out OK

    pctx = (OUT32DEV *) pslot->priv;

    if (cmd == PCGET) {
        ret = snprintf(buf, *plen, "%01x\n", pctx->outval);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    ret = sscanf(val, "%x", &newout32);
    if (ret != 1) {
        ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
        *plen = ret;
        return;
    }
    pctx->outval = newout32;

    txret =  out32tofpga(pctx);   // This peripheral's context
    if (txret != 0) {
        // the send of the new outval did not succeed.  This probably
        // means the input buffer to the USB port is full.  Tell the
        // user of the problem.
        ret = snprintf(buf, *plen, E_WRFPGA);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // Start timer to look for a write response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(PC_ONESHOT, 100, noAck, (void *) pctx);

    return;
}


/**************************************************************
 * out32tofpga():  - Send outval to the FPGA card.  Return
 * zero on success
 **************************************************************/
int out32tofpga(
    OUT32DEV *pctx)    // This peripheral's context
{
    PC_PKT   pkt;      // send write and read cmds to the out32
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      i;
    uint32_t tmp;
    int      shift[] = {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8};

    pmyslot = pctx->pslot;
    pmycore = pmyslot->pcore;

    // Got a new value for the outputs.  Send down to the card.
    // Build and send the write command to set the out32.
    pkt.cmd = PC_CMD_OP_WRITE | PC_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = OUT32_REG_OUTVAL;
    pkt.count = OUT32PTKLEN;
    for (i = 0; i < OUT32PTKLEN; i++) {
        tmp = pctx->outval >> shift[i];
        pkt.data[i]  = ((tmp & 0x00000001) != 0) ? 1 : 0;
        pkt.data[i] += ((tmp & 0x00010000) != 0) ? 2 : 0;
    }
    txret = pc_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void    *timer,   // handle of the timer that expired
    OUT32DEV *pctx)    // points to instance of this peripheral
{
    // Log the missing ack
    pclog(E_NOACK);

    return;
}
// end of out32.c
