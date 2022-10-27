/*
 *  Name: bootflash.c
 *
 *  Description: Driver for the SPI boot flash
 *
 *  Hardware Registers: (spi)
 *    Addr=0    Clock select, chip select control, interrupt control and
 *              SPI mode register
 *    Addr=1    Data fifo.  First byte is packet size
 *
 *  NOTES:
 *   - Extend the number of bytes in a packet by forcing CS low and sending
 *     several packets.  The electronics will see just one packet.  We use
 *     this to read and write flash pages.
 *
 *  Resources:
 *    info      - Manufacturer ID, device ID, and capacity in bytes
 *    file      - read/write file name to send to flash or copy from flash
 *
 *
 * Copyright:   Copyright (C) 2015-2021 Demand Peripherals, Inc.
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

/* TODO
 *     The bootflash peripheral is woefully incomplete.  It needs
 *  1) A verification phase for flash writing.  IMPORTANT!!!
 *  2) A erase/write offset so different files could be written at
 *     different locations.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "daemon.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // register definitions
#define ESPI_REG_CONFIG  0x00
#define ESPI_REG_FIFO    0x01
#define ESPI_NBYT          32   // num data byte to send in write pkt
        // BTFL (spi) definitions.  Must match Verilog file
#define CS_MODE_AL       0x00   // Active low chip select
#define CS_MODE_AH       0x04   // Active high chip select
#define CS_MODE_FL       0x08   // Forced low chip select
#define CS_MODE_FH       0x0c   // Forced high chip select
#define CLK_2M           0x00   // 2 MHz
#define CLK_1M           0x40   // 1 MHz
#define CLK_500K         0x80   // 500 KHz
#define CLK_100K         0xc0   // 100 KHz
        // misc constants
#define MAX_LINE_LEN        120
#define FN_INFO             "info"
#define FN_FILE             "file"
        // Resource index numbers
#define RSC_INFO            0
#define RSC_FILE            1
        // State of the peripheral
#define BT_IDLE          0x00   // No activity, ready for command
#define BT_INFO          0x10   // Send command to ready JEDEC info
#define BT_READ_1        0x21   // Reading, send config 1000000 fl
#define BT_READ_2        0x22   // Reading, send 0B cmd
#define BT_READ_3        0x23   // Reading, write data to file
#define BT_READ_4        0x24   // Reading, send config 1000000 al
#define BT_ERASE_1       0x31   // Erasing, send 06, write enable cmd
#define BT_ERASE_2       0x32   // Erasing, send D4, 64K block erase cmd
#define BT_ERASE_3       0x33   // Erasing, send 05, read status register
#define BT_WRITE_1       0x41   // Writing, send 06, write enable command
#define BT_WRITE_2       0x42   // Writing, send 02 page program cmd and 32 data bytes
#define BT_WRITE_3       0x43   // Writing, send 05 to get status register
#define BT_CHECK         0x50   // Verifying a write


// bootflash local context
typedef struct
{
    SLOT    *pSlot;         // handle to peripheral's slot info
    int      flowCtrl;      // ==1 if we are applying flow control
    int      state;         // idle, info, read, erase, write, check
    int      j_manid;       // JEDEC manufacturer ID
    int      j_devid;       // JEDEC device ID
    int      j_size;        // JEDEC size as log2(size in bytes)
    int      flfd;          // ==-1 if not open or FD if open
    int      filesz;        // write file size in bytes.
    int      rwidx;         // read/erase/write byte index 
    void    *ptimer;        // Watchdog timer to abort a failed transfer
} BTFLDEV;


/**************************************************************
 *  - Function prototypes and external references
 **************************************************************/
static void  packet_hdlr(SLOT *, PC_PKT *, int);
static void  cb_user(int, int, char *, SLOT *, int, int *, char *);
static void  get_info(BTFLDEV *);
static void  read_sector(BTFLDEV *);
static void  erase_sector(BTFLDEV *);
static void  write_sector(BTFLDEV *);
static void  no_ack(void *, BTFLDEV *);
extern int   pc_tx_pkt(CORE *pcore, PC_PKT *inpkt, int len);
extern int   DebugMode;  // set to 1 for debug data


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    BTFLDEV *pctx;     // our local device context

    // Allocate memory for this peripheral
    pctx = (BTFLDEV *) malloc(sizeof(BTFLDEV));
    if (pctx == (BTFLDEV *) 0) {
        // Malloc failure this early?
        pclog("memory allocation failure in bootflash initialization");
        return (-1);
    }

    pctx->pSlot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->state = BT_IDLE;     // idle, info, read, erase, write, check
    pctx->j_manid = -1;        // JEDEC manufacturer ID
    pctx->j_devid = -1;        // JEDEC device ID
    pctx->j_size = -1;         // JEDEC size as log2(size in bytes)
    pctx->flfd = -1;           // ==-1 if not open or local disk FD if open
    pctx->filesz = 0;          // write file size in bytes.
    pctx->rwidx = 0;           // read/erase/write byte index 


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_INFO].name = FN_INFO;
    pslot->rsc[RSC_INFO].flags = IS_READABLE;
    pslot->rsc[RSC_INFO].bkey = 0;
    pslot->rsc[RSC_INFO].pgscb = cb_user;
    pslot->rsc[RSC_INFO].uilock = -1;
    pslot->rsc[RSC_INFO].slot = pslot;
    pslot->rsc[RSC_FILE].name = FN_FILE;
    pslot->rsc[RSC_FILE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_FILE].bkey = 0;
    pslot->rsc[RSC_FILE].pgscb = cb_user;
    pslot->rsc[RSC_FILE].uilock = -1;
    pslot->rsc[RSC_FILE].slot = pslot;
    pslot->name = "bootflash";
    pslot->desc = "SPI boot flash memory";
    pslot->help = README;

    get_info(pctx);            // get JEDEC info 

    return (0);
}


/**************************************************************
 * Handle incoming packets from the peripheral.
 * Check for unexpected packets, discard write response packet,
 * send read response packet data to UI.
 **************************************************************/
static void packet_hdlr(
    SLOT   *pslot,     // handle for our slot's internal info
    PC_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    BTFLDEV *pctx;
    int      ret;      // generic return value
    RSC     *prsc;


    pctx = (BTFLDEV *)(pslot->priv);

    // Packet ACK can change the state 
    if ((pkt->cmd & PC_CMD_AUTO_MASK) != PC_CMD_AUTO_DATA) {
        // clear ack timer
        del_timer(pctx->ptimer);              // Got the response
        pctx->ptimer = 0;

        // The code below is just handling the write ACKs.  Usually nothing to do on ACL
        if (pctx->state == BT_INFO)           // getting JEDEC info
            return;
        else if (pctx->state == BT_READ_1) {  // sent config FL, send OB cmd
            pctx->state = BT_READ_2;
            read_sector(pctx);
        }
        else if (pctx->state == BT_READ_2)    // sent OB cmd, start reading
            return;
        else if (pctx->state == BT_READ_3)    // reading. no action on ACK
            return;
        else if (pctx->state == BT_READ_4)    // sent config AL, all done!
            pctx->state = BT_IDLE;
        else if (pctx->state == BT_ERASE_1)   // sent write enable command
            return;
        else if (pctx->state == BT_ERASE_2)   // sent erase 64K sector
            return;
        else if (pctx->state == BT_ERASE_3)   // sent read status reg #1
            return;
        else if (pctx->state == BT_WRITE_1)   // sent 06, write enable command
            return;
        else if (pctx->state == BT_WRITE_2)   // sent block write cmd + 32 data bytes
            return;
        else if (pctx->state == BT_WRITE_3)   // sent read status reg #1
            return;
        else {
            pclog("Unknown espi ACK from board to host");
            return;
        }
        return;
    }

    // To get here the packet should be an auto send SPI reply.
    //    INFO response?
    if ((pctx->state == BT_INFO) && (pkt->count == 4)) {
        // got the JEDEC info response
        pctx->j_manid = pkt->data[1];        // manufacturer ID
        pctx->j_devid = pkt->data[2];        // device ID
        pctx->j_size  = pkt->data[3];        // log_2 flash size (bytes)
        pctx->state = BT_IDLE;
        return;
    }

    prsc = &(pslot->rsc[RSC_FILE]);
    //    FILE READ response?
        // got response to our config FL command?  Start reading bytes
    if ((pctx->state == BT_READ_2) && (pctx->flfd >= 0) &&
        (pkt->count == 5) && (pkt->reg == 0)) {
        pctx->state = BT_READ_3;             // start reading bytes
        read_sector(pctx);
        return;
    }
         // got flash data packet? Save to file
    else if ((pctx->state == BT_READ_3) && (pctx->flfd >= 0) &&
        (pkt->count == ESPI_NBYT) && (pkt->reg == 0)) {
        del_timer(pctx->ptimer);             // Got the response
        pctx->ptimer = 0;

        // read response packet.  Copy data to file
        // First four bytes are ff as response to four cmd bytes.
        ret = write(pctx->flfd, &(pkt->data[0]), ESPI_NBYT);
        if (ret != ESPI_NBYT) {
            while (errno == EAGAIN)
                ret = write(pctx->flfd, &(pkt->data[0]), ESPI_NBYT);
            if (ret != ESPI_NBYT) {
                // Unable to write to save file.  Error out
                close(pctx->flfd);
                pctx->flfd = -1;
                pclog("Unable to write to bootflash save file");
                pctx->state = BT_IDLE;
                prompt(prsc->uilock);
                prsc->uilock = -1;          // clear the UI lock
                return;
            }
        }
        // data written to file.  Increment index and test for done
        pctx->rwidx += ESPI_NBYT;
        if (pctx->rwidx >= pctx->filesz) {
            // Done!
            close(pctx->flfd);
            pctx->flfd = -1;
            prompt(prsc->uilock);
            prsc->uilock = -1;              // clear the UI lock
            pctx->state = BT_READ_4;        // send config AL
            read_sector(pctx);
            if (DebugMode)
                printf("\n");
            return;
        } else {
            // not done.  Send request for next set of bytes
            read_sector(pctx);
            return;
        } 
    }
    else if (pctx->state == BT_ERASE_1) {   // got byte from write enable
        pctx->state = BT_ERASE_2;
        erase_sector(pctx);                 // start erasing sectors
        return;
    }
    else if (pctx->state == BT_ERASE_2) {   // got erase block response. Ignore
        pctx->state = BT_ERASE_3;
        erase_sector(pctx);                 // Get status register
        return;
    }
    else if (pctx->state == BT_ERASE_3) {   // check status for erase complete
        // bit 0 is write status bit
        if (pkt->data[1] & 0x01) {
            erase_sector(pctx);             // still busy, go ask again
        } else {                            // erase complete. increment count
            pctx->rwidx += (1 << 16);       // and check for completion
            if (pctx->rwidx > pctx->filesz) {
                pctx->state = BT_WRITE_1;   // done erasing, start writing
                pctx->rwidx = 0;
                if (DebugMode)
                    printf("\n");
                write_sector(pctx);
            } else {
                // Not done, erase next sector
                pctx->state = BT_ERASE_1;
                erase_sector(pctx);
            }
        }
        return;
    }
    else if (pctx->state == BT_WRITE_1) {   // got write enable response
        pctx->state = BT_WRITE_2;           // write EPSI_NBYE bytes
        write_sector(pctx);
        return;
    }
    else if (pctx->state == BT_WRITE_2) {   // got write EPSI_NBYT response.  
        pctx->rwidx += pkt->count - 4;      // -4 to allow for the cmd+addr(3)
        pctx->state = BT_WRITE_3;           // wait for write complete
        write_sector(pctx);
        return;
    }
    else if (pctx->state == BT_WRITE_3) {   // check status for write complete
        // bit 0 is write status bit
        if (pkt->data[1] & 0x01)
            write_sector(pctx);             // still busy, go ask again
        else {
            // ESPI_NBYT write complete.  Free UIlock and go to idle if done
            if (pctx->rwidx >= pctx->filesz) {
                // Done!
                close(pctx->flfd);
                pctx->flfd = -1;
                prompt(prsc->uilock);
                prsc->uilock = -1;          // clear the UI lock
                pctx->state = BT_IDLE;
                if (DebugMode)
                    printf("\n");
            } else {
                // Not done, write next block
                pctx->state = BT_WRITE_1;
                write_sector(pctx);
            }
        }
        return;
    }

    return;
}


/**************************************************************
 * Callback used to handle requests from the user.  This can
 * put the device into the BT_READ or BT_ERASE states.
 **************************************************************/
static void cb_user(
    int      cmd,      //==PCGET if a read, ==PCSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    BTFLDEV   *pctx;               // our context
    int        ret;                // generic return value
    char       savename[PATH_MAX]; // name of save file
    int        sectorcount;        // # 64K sectors to read
    struct stat statbuf;           // status of file to download


    pctx = pslot->priv;

    if ((cmd == PCGET) && (rscid == RSC_INFO)) {
        // Display the JEDEC info for the user.  Display bytes.
        ret = snprintf(buf, *plen,
              "Manufacturer ID = 0x%02X, Device ID = 0x%02X, Size = %d\n", 
              pctx->j_manid, pctx->j_devid, (1 << pctx->j_size));
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // All other commands are allowed only if IDLE
    if (pctx->state != BT_IDLE) {
        ret = snprintf(buf, *plen, "Bootflash operation already in progress");
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    if ((cmd == PCGET) && (rscid == RSC_FILE)) {
        // Read from the flash to a file
        // Get the file name and open it for writing
        ret = sscanf(val, "%s %d", savename, &sectorcount);
        pctx->flfd = open(savename, (O_CREAT | O_TRUNC | O_WRONLY), 0666);
        if (pctx->flfd < 0) {
            ret = snprintf(buf, *plen, "Unable to open file %s for writing", savename);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        pctx->filesz = 1 << (pctx->j_size);      // assume to read the whole flash
        if (ret == 2) {                          // but limit if there's a 64K block limit
            if ((sectorcount * (1 << 16)) < pctx->filesz)
                pctx->filesz = sectorcount * (1 << 16);
        }
        // Init the counters and start the reading process
        pctx->rwidx = 0;
        pctx->state = BT_READ_1;
        read_sector(pctx);
        // lock UI waiting for read completion
        pslot->rsc[RSC_FILE].uilock = (char) cn;
    }
    else if ((cmd == PCSET) && (rscid == RSC_FILE)) {
        // Read from a file to flash
        // Get the file name and open it for reading
        pctx->flfd = open(val, O_RDONLY, 0666);
        if (pctx->flfd < 0) {
            ret = snprintf(buf, *plen, "Unable to open file %s for reading", val);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        ret = fstat(pctx->flfd, &statbuf);
        if (ret == -1) {
            ret = snprintf(buf, *plen, "Unable to get size of %s", val);
            *plen = ret;  // (errors are handled in calling routine)
            close(pctx->flfd);
            pctx->flfd = -1;
            return;
        }
        pctx->filesz = (int)(statbuf.st_size);
        // we expect the file to be larger than zero but let's check anyway
        if (pctx->filesz == 0) {
            ret = snprintf(buf, *plen, "Flash file %s has zero bytes.  Write aborted", val);
            *plen = ret;  // (errors are handled in calling routine)
            close(pctx->flfd);
            pctx->flfd = -1;
            return;
        }

        // Init the counters and start the writing process with an erase
        pctx->rwidx = 0;
        pctx->state = BT_ERASE_1;
        erase_sector(pctx);
        // lock UI waiting for write completion
        pslot->rsc[RSC_FILE].uilock = (char) cn;
    }

    return;
}


/**************************************************************
 * get_info()  Sends packet to request JEDEC info.
 * Set state to BT_INFO to await response
 **************************************************************/
static void get_info(
    BTFLDEV *pctx)    // This peripheral's context
{
    PC_PKT   pkt;
    SLOT    *pmyslot;  // Our per slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pmyslot = pctx->pSlot;
    pmycore = pmyslot->pcore;

    // create a write packet to set the mode reg
    pkt.cmd = PC_CMD_OP_WRITE;
    pkt.core = pmycore->core_id;

    pkt.reg = ESPI_REG_FIFO;
    pkt.count = 1 + 4;           // count byte plus four SPI packet bytes
    pkt.data[0] = 4;             // JEDEC packet size
    pkt.data[1] = 0x9f;          // JEDEC read info command
    pkt.data[2] = 0x00;          // dummy byte
    pkt.data[3] = 0x00;          // dummy byte
    pkt.data[4] = 0x00;          // dummy byte


    // Send the packet.  Error msg on failure, INFO state on success
    txret = pc_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data
    if (txret |= 0)
        pclog("Error reading flash JEDEC information");
    else
        pctx->state = BT_INFO;

    return;
}


/**************************************************************
 * read_sector()  Sends packets to read ESPI_NBTY byte blocks
 * of flash.
 **************************************************************/
static void read_sector(
    BTFLDEV *pctx)    // This peripheral's context
{
    PC_PKT   pkt;
    SLOT    *pmyslot;  // Our per slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pmyslot = pctx->pSlot;
    pmycore = pmyslot->pcore;

    // Base action on read state.  State is advanced by the packet handler
    // as responses come in
    if (pctx->state == BT_READ_1) {           // send config FL cmd
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_CONFIG;
        pkt.data[0] = CLK_1M | CS_MODE_FL;    // force CS low
        pkt.count = 1;                        // config is one byte
    }
    else if (pctx->state == BT_READ_2) {      // Send 'continuous read' (0B) cmd
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_FIFO;
        pkt.count = 1 + 5;                    // count byte + cmd + addr(3) + dummy
        pkt.data[0] = 5;                          // cmd,addr,dummy
        pkt.data[1] = 0x0B;                       // flash read command
        pkt.data[2] = (pctx->rwidx >> 16) & 0xff; // high address byte
        pkt.data[3] = (pctx->rwidx >> 8) & 0xff;  // mid address byte
        pkt.data[4] = pctx->rwidx & 0xff;         // low address byte
        pkt.data[5] = 0;                          // dummy
    }
    else if (pctx->state == BT_READ_3) {      // reading bytes ....
        if (DebugMode)
            printf("bootflash: reading block %d\r", pctx->rwidx);
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_FIFO;
        // response will have ESPI_NBYT
        pkt.count = 1 + ESPI_NBYT;            // count byte + NBTY data
        pkt.data[0] = ESPI_NBYT;              // room for the response
    }
    else if (pctx->state == BT_READ_4) {      // send config AL cmd
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_CONFIG;
        pkt.data[0] = CLK_100K | CS_MODE_AL;    // set CS active low
        pkt.count = 1;                        // config is one byte
    }

    // Send read request
    txret = pc_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    // Error msg on and IDLE state on failure,
    if (txret |= 0) {
        pclog("Error reading flash.  Read operation aborted.");
        close(pctx->flfd);
        pctx->flfd = -1;
        pctx->state = BT_IDLE;
    }

    // lots of packets so lots of chances for a lost one
    pctx->ptimer = add_timer(PC_ONESHOT, 100, no_ack, (void *) pctx);

    return;
}


/**************************************************************
 * erase_sector()  Erases flash up to the size of the file to be written.
 * The procedure for erasing flash is as follows:
 *     1) Send 06, write enable command
 *     2) Send D4, 64K block erase command
 *     3) Loop
 *             Send 05, read status register command
 *        Until status bit 0 is cleared
 *     4) Increment erasure count
 *     5) Repeat 1-4 until erase up to file size is complete
 **************************************************************/
static void erase_sector(
    BTFLDEV *pctx)     // This peripheral's context
{
    PC_PKT   pkt;
    SLOT    *pmyslot;  // Our per slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pmyslot = pctx->pSlot;
    pmycore = pmyslot->pcore;

    // Base action on erase state.  State is advanced by the packet handler
    // as responses come in
    if (pctx->state == BT_ERASE_1) {          // Send write enable command
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_FIFO;
        pkt.count = 1 + 1;                    // count byte + cmd
        pkt.data[0] = 1;                      // spi pkt size
        pkt.data[1] = 0x06;                   // write enable command
    }
    else if (pctx->state == BT_ERASE_2) {     // Send 64K block erase command
        if (DebugMode)
            printf("bootflash: erasing block %d\r", pctx->rwidx);
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_FIFO;
        pkt.count = 1 + 4;                    // count byte + cmd + addr(3)
        pkt.data[0] = 4;                          // pkt sz = cmd,addr
        pkt.data[1] = 0xD8;                       // flash erase command
        pkt.data[2] = (pctx->rwidx >> 16) & 0xff; // high address byte
        pkt.data[3] = (pctx->rwidx >> 8) & 0xff;  // mid address byte
        pkt.data[4] = pctx->rwidx & 0xff;         // low address byte
    }
    else if (pctx->state == BT_ERASE_3) {     // Read write status flag
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_FIFO;
        pkt.count = 1 + 2;                    // count byte + cmd + return status
        pkt.data[0] = 2;                      // spi pkt size
        pkt.data[1] = 0x05;                   // write enable command
        pkt.data[2] = 0x00;                   // dummy value on write
    }

    // Send read request
    txret = pc_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    // Error msg on and IDLE state on failure,
    if (txret |= 0) {
        pclog("Error erasing flash.  Erase operation aborted.");
        close(pctx->flfd);
        pctx->flfd = -1;
        pctx->state = BT_IDLE;
    }

    // lots of packets so lots of chances for a lost one
    pctx->ptimer = add_timer(PC_ONESHOT, 100, no_ack, (void *) pctx);

    return;
}


/**************************************************************
 * write_sector()  Sends packets to write file to flash.
 * Flash is written 256 bytes at a time.  The procedure is
 * as follows:
 * 1)   Send 06 write enable command
 * 2)   Send 02 page program command and ESPI_NBYT bytes of data
 * 3)   Get 05 status register to check for write complete
 * 4)   Loop 1-3 until all bytes are written
 **************************************************************/
static void write_sector(
    BTFLDEV *pctx)     // This peripheral's context
{
    PC_PKT   pkt;
    SLOT    *pmyslot;  // Our per slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      rdbyt;    // number of bytes returned in file read()
    RSC     *prsc;
    pmyslot = pctx->pSlot;
    pmycore = pmyslot->pcore;

    // Base action on write state.  State is advanced by the packet handler
    // as responses come in
    if (pctx->state == BT_WRITE_1) {          // Send write enable command
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_FIFO;
        pkt.count = 1 + 1;                    // count byte + cmd
        pkt.data[0] = 1;                      // spi pkt size
        pkt.data[1] = 0x06;                   // write enable command
    }
    else if (pctx->state == BT_WRITE_2) {     // Send write block cmd +32 bytes of data
        if (DebugMode)
            printf("bootflash: writing block %d\r", pctx->rwidx);
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_FIFO;
        pkt.data[1] = 0x02;                       // page program command
        pkt.data[2] = (pctx->rwidx >> 16) & 0xff; // high address byte
        pkt.data[3] = (pctx->rwidx >> 8) & 0xff;  // mid address byte
        pkt.data[4] = pctx->rwidx & 0xff;         // low address byte
        do {
            rdbyt = read(pctx->flfd, &(pkt.data[5]), ESPI_NBYT);
        } while ((rdbyt == -1) && (errno == EAGAIN));
        if (rdbyt <= 0) {
            pclog("Error reading file to flash.");
            close(pctx->flfd);
            pctx->flfd = -1;
            pctx->state = BT_IDLE;
            prsc = &(pmyslot->rsc[RSC_FILE]);
            prsc->uilock = -1;          // clear the UI lock
            return;
        }
        pkt.count =  1 + rdbyt + 4;           // spi pkt len + dbyt + cmd + addr(3)
        pkt.data[0] = 4 + rdbyt;              // cmd,addr(3),up to 56 data
    }
    else if (pctx->state == BT_WRITE_3) {     // Get write status flag
        pkt.cmd = PC_CMD_OP_WRITE;
        pkt.core = pmycore->core_id;
        pkt.reg = ESPI_REG_FIFO;
        pkt.count = 1 + 2;                    // count byte + cmd + return status
        pkt.data[0] = 2;                      // spi pkt size
        pkt.data[1] = 0x05;                   // write enable command
        pkt.data[2] = 0x00;                   // dummy value on write
    }

    // Send write request
    txret = pc_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    // Error msg on and IDLE state on failure,
    if (txret |= 0) {
        pclog("Error writing flash.  Write operation aborted.");
        close(pctx->flfd);
        pctx->flfd = -1;
        pctx->state = BT_IDLE;
    }

    // lots of packets so lots of chances for a lost one
    pctx->ptimer = add_timer(PC_ONESHOT, 100, no_ack, (void *) pctx);

    return;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void no_ack(
    void     *timer,   // handle of the timer that expired
    BTFLDEV *pctx)
{
    // Log the missing ack
    pclog(E_NOACK);

    return;
}

//end of espi.c

