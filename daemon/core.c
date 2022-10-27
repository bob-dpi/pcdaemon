/*
 * Name: core.c
 * 
 * Description: This code manages the shared object drivers for the peripherals
 * 
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
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>              // for PATH_MAX
#include "main.h"


/***************************************************************************
 *  - Limits and defines
 ***************************************************************************/
        // SLIP decoder states
#define SKIP_FIRST_ZEROES (0)
#define AWAITING_PKT  (1)
#define IN_PACKET     (2)
#define INESCAPE      (3)



/***************************************************************************
 *  - Function prototypes
 ***************************************************************************/
void         initslot(SLOT *);  // Load and init this slot
int          add_so(char *);
int          pc_tx_pkt(CORE *pcore, PC_PKT *inpkt, int len);
void         receivePkt(int fd, void *priv, int rw);
static void  dispatch_packet(unsigned char *inbuf, int len);
static int   pctoslip(unsigned char *, int, unsigned char *);
static uint16_t crc16(unsigned char *, int);


/***************************************************************************
 *  - System-wide global variable allocation
 ***************************************************************************/
extern SLOT     Slots[MX_SLOT];       // Allocate the driver table
extern PC_FD    Pc_Fd[MX_FD];         // Table of open FDs and callbacks
extern PC_TIMER Timers[MX_TIMER];     // Table of timers
extern UI       UiCons[MX_UI];        // Table of UI connections
extern CORE     Core[NUM_CORE];       // Table of FPGA based peripherals
extern int      UseStderr;            // use stderr
extern int      Verbosity;            // verbosity level
extern int      DebugMode;            // run in debug mode
extern int      UiaddrAny;            // Use any IP address if set
extern int      UiPort;               // TCP port for ui connections
extern char     PeriList[];
extern char    *SerialPort;
extern int      fpgaFD;               // -1 or fd to SerialPort


/***************************************************************************
 *  - core.c specific globals
 ***************************************************************************/
unsigned char   Slrx[RXBUF_SZ];  // slip received packet from USB port
int             Slix;             // where in slrx the next byte goes



/***************************************************************************
 *  pc_tx_pkt():  Send a packet to the board
 *     Return number of bytes sent or -1 on error
 ***************************************************************************/
int pc_tx_pkt(
    CORE    *pcore,    // The fpga core sending the packet
    PC_PKT  *inpkt,    // The packet to send
    int      len)      // Number of bytes in the packet
{
    unsigned char sltx[PC_PKTLEN]; // SLIP encoded packet
    int      txcount;  // Length of SLIP encoded packet
    int      sntcount; // Number of bytes actually sent
    int      i;

    // sanity check
    if (len < 4) {
        pclog("Invalid packet of length %d from core %d\n", pcore->core_id, len);
        return (-1);
    }

    // Fill in the destination core # and add 'e' in high nibble
    // to help sanity checking down on the board.  Make high
    // nibble of the cmd byte an 'f' to help sanity checking on
    // board too.
    // Note that core is 0 indexed to the peripherals in the FPGA
    // and slot is 0 indexed to the loaded drivers.  They might or
    // might not be equal.
    inpkt->cmd  = inpkt->cmd | 0xf0;  // helps error checking
    inpkt->core = inpkt->core | 0xe0;

    // Get and check the USB port's FD from the enumerator core info
    if (fpgaFD == -1) {
        //  not connected to the board.
        return(-1);
    }

    // Convert PC pkt to a SLIP encoded packet
    txcount = pctoslip((unsigned char *) inpkt, len, sltx);

    // print pkts to stdout if debug enabled
    if (DebugMode && (Verbosity == PC_VERB_TRACE)) {
        printf(">>");
        for (i = 0; i < txcount; i++)
            printf(" %02x", sltx[i]);
        printf("\n");
    }

    // write SLIP packet to the USB FD
    sntcount = write(fpgaFD, sltx, txcount);

    // Check how many bytes were sent.  We get EAGAIN if the USB port
    // buffer is full.  Return an error in this case to let the sender
    // set a timer and try again later (or deal with however).  All
    // other possibilities indicate something more serious -- log it.
    if (sntcount != txcount) {
        if ((sntcount == -1) && (errno != EAGAIN)) {
            pclog("Error sending to FPGA, errno=%d\n", errno);
        }
        return (sntcount);  // return error on partial writes
    }
    return (0);
}


/***************************************************************************
 *  pctoslip():  Convert a PC packet to a SLIP encoded PC packet
 *  Return the number of bytes in the new packet
 ***************************************************************************/
static int pctoslip(
    unsigned char *pcpkt,  // The unencode PC packet (input)
    int      len,      // Number of bytes in pcpkt
    unsigned char *slppkt) // The SLIP encoded packet (output)
{
    int      dpix = 0; // Index into the input PC packet
    int      slix = 0; // Indes into the output SLIP packet
    uint16_t crc;      // CRC16/XMODEM

    // Sanity check on input length
    if (len > PC_PKTLEN)
        return (0);

    // Computer the CRC and add it to the end of the packet
    crc = crc16(pcpkt, len);
    pcpkt[len] = (crc >> 8);
    pcpkt[len +1] = crc & 0x00ff;

    // Van Jacobson encoding.  Send opening SLIP_END character
    slppkt[slix++] = SLIP_END;

    // Copy the input packet to the output packet but replace any
    // ESC or END characters with their two character equivalent
    for (dpix = 0; dpix < len+2; dpix++) {   // +2 for crc bytes
        if (pcpkt[dpix] == SLIP_END) {
            slppkt[slix++] = SLIP_ESC;
            slppkt[slix++] = INPKT_END;
        }
        else if (pcpkt[dpix] == SLIP_ESC) {
            slppkt[slix++] = SLIP_ESC;
            slppkt[slix++] = INPKT_ESC;
        }
        else {
            slppkt[slix++] = pcpkt[dpix];
        }
    }

    // Send closing SLIP_END character
    slppkt[slix++] = SLIP_END;

    return (slix);
}


/***************************************************************************
 *  receivePkt()  - Handle packets from the FPGA board.
 * 
 ***************************************************************************/
void receivePkt(
    int      fd,       // FD of USB port with data to read
    void    *priv,     // transparent callback data
    int      rw)       // ==0 on read ready, ==1 on write ready
{
    unsigned char pcpkt[RXBUF_SZ]; // the SLIP decoded packet
    int      dpix;     // index into pcpkt
    unsigned char c;   // current char to decode
    static int    s_slstate = SKIP_FIRST_ZEROES;  // STATIC current state of the decoder at startup
    int      rdret;    // read return value
    int      i;        // buffer loop counter


    rdret = read(fpgaFD, &(Slrx[Slix]), (RXBUF_SZ - Slix));

    // Was there an error or has the port closed on us?
    if (rdret <= 0) {
        if ((errno != EAGAIN) || (rdret == 0)) {
            pclog(M_NOREAD, SerialPort);
            exit(-1);
        }
        // EAGAIN means it's recoverable and we just try again later
        return;
    }
    Slix += rdret;


    // At this point we have read some bytes from the host port.  We
    // now scan those bytes looking for SLIP packets.  We put any
    // packets we find into the pcpkt buffer and then dispatch the
    // completed packet to the packet handler which routes the packet
    // to the callback registered for that core.
    // Packets with a protocol violation are dropped with a log
    // message.
    // It sometimes happens that a read() returns a full packet and
    // a partial packet.  In this case we process the full packet and
    // move the bytes of the partial packet to the start of the buffer.
    // This way we can always start the SLIP processing at the start 
    // of the buffer.  
    // Drop into a loop to process all the packets in the buffer
    dpix = 0;               // at start of a new decoded packet

    for (i = 0; i < Slix; i++) {
        c = Slrx[i];
      //  printf(": %x",c);

        if (c == SLIP_END) {
            if (s_slstate == IN_PACKET) {
                // Process completed packet and set up for next one
                if (dpix > 0) {
                    // non empty frames get handled
                    dispatch_packet(pcpkt, dpix);
                }
                // return if no more bytes in buffer
                if (i == Slix) {
                    Slix = 0;
                    return;
                }
                // else move remaining bytes in buffer down and scan again
                (void) memmove(Slrx, Slrx  + i , Slix - i );
                Slix = Slix - i; //- 1;
                s_slstate = IN_PACKET;
                dpix = 0;
                i = 0;      // scan again from start of buffer
            }
        }
        else if (c == SLIP_ESC) {
            // this should only occur while IN_PACKET
            if (s_slstate == IN_PACKET)
                s_slstate = INESCAPE;
            else {
                // A protocol error.  Report it. Move remaining bytes down
                pclog(M_BADSLIP, SerialPort);
                (void) memmove(Slrx, Slrx + i , Slix - i);
                Slix = Slix - i - 1;
                s_slstate = IN_PACKET;
                dpix = 0;
                i = 0;
            }
        }
        else if ((c == INPKT_END) && (s_slstate == INESCAPE)) {
            pcpkt[dpix] = SLIP_END;
            dpix++;
            s_slstate = IN_PACKET;
        }
        else if ((c == INPKT_ESC) && (s_slstate == INESCAPE)) {
            pcpkt[dpix] = SLIP_ESC;
            dpix++;
            s_slstate = IN_PACKET;
        }
        else if ((c == 0x00) && (s_slstate == SKIP_FIRST_ZEROES)) {
            /* ignore zero byte outside of packet */
            pclog("skipping zero byte\n");
            s_slstate = SKIP_FIRST_ZEROES; // keep skipping zeroes
            continue; // ignore byte
        }
        else if ((c != 0x00) && (s_slstate == SKIP_FIRST_ZEROES)) {
            /* first char not zero in stream we need it */
            /* if empty start frame ignore it */
            if (c == SLIP_END) {
                pclog("skipping empty frame\n");
                s_slstate = IN_PACKET;
                continue;
            } else {
                // printf("first byte %x\n",c);
                // valid data, first found in the stream, add to buffer
                pcpkt[dpix] = c;
                dpix++;
                s_slstate = IN_PACKET; // we are getting packets from now on
            }
        }
        else {
            // a normal character
            pcpkt[dpix] = c;
            dpix++;
        }
    } // for
}


/***************************************************************************
 *  dispatch_packet()  - Verify and route packets to peripheral modules
 ***************************************************************************/
static void dispatch_packet(
    unsigned char *inbuf, // Points to input packet
    int      len)         // Length of input packet
{
    int      bogus = 0;   // assume it is OK
    PC_PKT  *ppkt;        // maps char pointer to a PC_PKT
    int      pktcore;     // source core for the packet
    int      requested_bytes; // request len and remaining
    int      returned_bytes;
    int      remaining_bytes;
    int      i;

    ppkt = (PC_PKT *) inbuf;
    pktcore = ppkt->core & 0x0f;   // mask high four bits of address

    // Perform as many sanity check as we can before routing this to
    // the driver.

    // Verify minimum packet length (4 header, 2 crc)
    if (len < 6) {
        bogus = 1;
    }

    // Verify crc
    else if (crc16(inbuf, len) != (uint16_t) 0) {
        // failed crc check
        bogus = 2;
    }

    // Cmd has to be either a read or a write response
    else if ((ppkt->cmd & PC_CMD_OP_MASK) == 0) {
        bogus = 3;
    }

    // Verify core is in a valid range
    else if (pktcore >= NUM_CORE) {
        bogus = 4;
    }

    // Verify word size, request count, remaining count and len are OK
    // for reads
    else if (ppkt->cmd & PC_CMD_OP_READ) {
        requested_bytes = ppkt->count;
        returned_bytes = len - 7; // four header bytes & remaining
                                  // count & 2 crc bytes
        // Difference between requested and returned should be in
        // remaining.  (Remaining is third byte from the end.)
        remaining_bytes = (int) inbuf[len - 3];
        if (remaining_bytes != requested_bytes - returned_bytes) {
            bogus = 5;
        }
    }

    if (bogus != 0)  {
        pclog(M_BADPKT, SerialPort);
        if (DebugMode && (Verbosity == PC_VERB_TRACE)) {
            printf("<X");
            for (i = 0; i < len; i++)
                printf(" %02x", (unsigned char) (inbuf[i]));
            printf("\n");
        }
        return;
    }

    // print pkts to stdout if debug enabled
    if (DebugMode && (Verbosity == PC_VERB_TRACE)) {
        printf("<<");
        for (i = 0; i < len; i++)
            printf(" %02x", (unsigned char) (inbuf[i]));
        printf("\n");
    }

    // Packet looks OK, dispatch it to the driver if core
    // has registered a received packet callback
    if (Core[pktcore].pcb) {
        (Core[pktcore].pcb) (
          &(Slots[Core[pktcore].slot_id]),  // slot pointer
            ppkt,               // the received packet
            len-2);             // num bytes in packet (-2 crc bytes)
    }
    else {
        // There is no driver for this core and this is an error.
        // However, this is common during start-up since packets can
        // arrive from the FPGA before we've had a chance to register
        // all the peripherals. 
        pclog(M_NOSO, Core[pktcore].core_id, SerialPort);
    }
}



// crc16/xmodem.
uint16_t crc16(unsigned char *pkt, int length)
{
    uint8_t   c;
    uint8_t   x;
    uint16_t  crc = 0x0000;

    while (length --) {
        c = *pkt;
        //c = *pkt++;
        x = (crc >> 8) ^ c;
        x ^= x >> 4;
        crc = (crc << 8) ^ ((uint16_t)x << 12) ^ ((uint16_t)x << 5) ^ ((uint16_t)x);
        pkt++;
    }
    return (crc);
}

/* end of core.c */
