/*
 *  Name: tonegen.c
 *
 *  Description: Driver for the tonegen peripheral
 *
 *  The is a simple square wave generator.  The user can specify the
 *  frequency to four decimal places or as note and octave, for example
 *  "b4".  The volume is specified  in a range of 0 to 100.  Volume
 *  follows a log taper so a setting of 50 gives an output of about
 *  0.1 of the full scale.  Tone duration is given in milliseconds with
 *  a range of 1 to 4095 milliseconds.
 *  The user can specify that a file of notes be played where the file
 *  has one line per note.  Lines which do not parse as frequency, 
 *  volume, and duration are treated as comments and quietly ignored.
 *  For example
 *      pcset tonegen note c3 40 1000
 *      pcset tonegen note 324.5 30 100
 *      pcset tonegen melody mymelody.txt
 *
 *  Note that all note sequencing is done by the host.  There is no 
 *  queue or FIFO of notes to be played.
 *
 *  Hardware Registers:
 *      0:  duration in milliseconds
 *      1:  low byte of 24 bit phase offset
 *      2:  mid byte of 24 bit phase offset0 MHz
 *      3:  high byte of 24 bit phase offset MHz
 *      4:  low 4 bits are PWM for LSB output, upper 4 bits control pin1
 *      5:  high 4 bits are PWM for MSB output, lower 4 bits control pin2
 *
 */

/*
 *
 * Copyright:   Copyright (C) 2022 Demand Peripherals, Inc.
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
#include <ctype.h>
#include "daemon.h"
#include "readme.h"

/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // tonegen register definitions
#define TG_REG_DURAT        0     // Duration in milliseconds
#define TG_REG_PHASE0       1     // Phase offset, low byte
#define TG_REG_PHASE1       2     // Phase offset, mid byte
#define TG_REG_PHASE2       3     // Phase offset, high byte
#define TG_REG_PWM10        4     // Volume PWM control for pins 1,0
#define TG_REG_PWM32        5     // Volume PWM control for pins 3,2
        // misc constants
#define MXLNLEN             100   // Maximum line length
#define MAX_DURATION        4095
#define MAX_FREQ            10000.0
#define MIN_FREQ            10.0
#define MAX_VOLUME          100
#define N_NOTES             108   // 12 notes in 9 octaves
        // Resources
#define RSC_NOTE            0
#define RSC_MELODY          1

/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of a tonegen
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    int      melodyfd; // FD of melody file
    void    *pnextnote;  // timer to read next note from file
    void    *ptimer;   // timer to watch for dropped ACK packets
} TONEGENDEV;

    // A musical note to frequency table
typedef struct
{
    char    *music;    // Frequency in music notation, eg "b5"
    float    freq;     // Frequency in Hertz
} TGNOTE;              // A ToneGen NOTE

    // A linear volume to audio taper table
typedef struct
{
    int      pwm3;     // PWM value for pin3
    int      pwm2;     // PWM value for pin2
    int      pwm1;     // PWM value for pin1
    int      pwm0;     // PWM value for pin0
} TGVOLUME;

/**************************************************************
 *  - Function prototypes and allocated storage
 **************************************************************/
static void packet_hdlr(SLOT *, PC_PKT *, int);
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, TONEGENDEV *);
extern int  pc_tx_pkt(CORE *pcore, PC_PKT *inpkt, int len);
static void read_melody_note(void *timer, TONEGENDEV *pctx);
static void notetofpga(TONEGENDEV *pctx, float freq, int vol, int dur);
static void load_Tgnote();
static void load_Tgvolume();
static float lookup_note(char *);
TGNOTE Tgnote[N_NOTES];
TGVOLUME Tgvolume[MAX_VOLUME + 1];  // 0 to 100 is 101 possible values


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    TONEGENDEV *pctx;  // our local device context

    // Allocate memory for this peripheral
    pctx = (TONEGENDEV *) malloc(sizeof(TONEGENDEV));
    if (pctx == (TONEGENDEV *) 0) {
        // Malloc failure this early?
        pclog("memory allocation failure in tonegen initialization");
        return (-1);
    }

    // Init our TONEGENDEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->melodyfd = -1;       // FD of note file
    pctx->pnextnote = 0;       // timer to read next note from file
    pctx->ptimer = 0;          // set while waiting for a response

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_NOTE].name = "note";
    pslot->rsc[RSC_NOTE].flags = IS_WRITABLE;
    pslot->rsc[RSC_NOTE].bkey = 0;
    pslot->rsc[RSC_NOTE].pgscb = usercmd;
    pslot->rsc[RSC_NOTE].uilock = -1;
    pslot->rsc[RSC_NOTE].slot = pslot;
    pslot->rsc[RSC_MELODY].name = "melody";
    pslot->rsc[RSC_MELODY].flags = IS_WRITABLE;
    pslot->rsc[RSC_MELODY].bkey = 0;
    pslot->rsc[RSC_MELODY].pgscb = usercmd;
    pslot->rsc[RSC_MELODY].uilock = -1;
    pslot->rsc[RSC_MELODY].slot = pslot;
    pslot->name = "tonegen";
    pslot->desc = "Tone generator";
    pslot->help = README;

    load_Tgnote();      // Load the notes table
    load_Tgvolume();    // Load the volume to PWM table

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
    TONEGENDEV *pctx;     // our local info

    pctx = (TONEGENDEV *)(pslot->priv);  // Our "private" data is a TONEGENDEV

    // Clear the timer on write response packets
    if ((pkt->cmd & PC_CMD_OP_MASK) == PC_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // There are no other packets from the tonegen FPGA code
    // so if we get here there is a problem.  Log the error.
    pclog("invalid tonegen packet from board to host");

    return;
}


/**************************************************************
 * usercmd():  - The user is starting a note or playing a file
 * Get the note and send it to the board or open the file and
 * set a timer to read and send the note.
 **************************************************************/
static void usercmd(
    int      cmd,        //==PCGET if a read, ==PCSET on write
    int      rscid,      // ID of resource being accessed
    char    *val,        // new value for the resource
    SLOT    *pslot,      // pointer to slot info.
    int      cn,         // Index into UI table for requesting conn
    int     *plen,       // size of buf on input, #char in buf on output
    char    *buf)
{
    TONEGENDEV *pctx;     // our local info
    char        note[MXLNLEN];     // the frequency in decimal or in music notation
    float       freq;     // frequency in Hertz
    int         vol;      // volume in range of 0 to 100
    int         dur;      // duration in milliseconds in range of 1 to 4095
    int         sret;     // scanf return value
    int         pret;     // sprintf return of #char printed

    pctx = (TONEGENDEV *) pslot->priv;

    // Play note or read notes from a file
    if (rscid == RSC_NOTE) {
        sret = sscanf(val, "%s %d %d", note, &vol, &dur);
        if ((sret != 3) || (vol < 0) || (vol > 100) ||
            (dur < 1) || (dur > MAX_DURATION)) {
            pret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = pret;
            return;
        }
        // Get the frequency
        sret = sscanf(note, "%f", &freq);
        if (sret != 1) {                // not a number, a musical note maybe?
            freq = lookup_note(note);
        }
        if ((freq > 10000.0) || (freq < 10.0)) {
            pret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = pret;
            return;
        }
        // The note looks valid.  Send it to the FPGA
        notetofpga(pctx, freq, vol, dur);
        return;
    }
    else if (rscid == RSC_MELODY) {
        // Close existing melody if open
        if (pctx->melodyfd >= 0) {
            close(pctx->melodyfd);
            pctx->melodyfd = -1;
        }
        if (pctx->pnextnote != 0) {   // stop timer if running
            del_timer(pctx->pnextnote);
            pctx->pnextnote = (void *) 0;
        }
        // Get file name and open for reading
        pctx->melodyfd = open(val, O_RDONLY);
        if (pctx->melodyfd == -1) {
            pret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = pret;
            return;
        }
        read_melody_note((void *)0, pctx);    // play first note in file and start timer
        return;
    }
}


/**************************************************************
 * read_melody_note():  - Read a note from an open file and send
 * to the FPGA.  Set a timer to read the next note at the completion
 * of the one just sent to the FPGA.  Close file on read error.
 **************************************************************/
static void read_melody_note(
    void       *timer, // timer that invoked us, ignored
    TONEGENDEV *pctx)  // context for this peripheral
{
    char        mline[MXLNLEN];    // melody line
    char        note[MXLNLEN];     // the frequency in decimal or in music notation
    float       freq;     // frequency in Hertz
    int         vol;      // volume in range of 0 to 100
    int         dur;      // duration in milliseconds in range of 1 to 4095
    int         sret;     // scanf return value
    int         rret;     // read() return value
    char        c;        // read line character-by-character
    int         i;        // index into note buffer

    // read a line from the file
    i = 0;
    do {
        rret = (int)read(pctx->melodyfd, &c, 1);
        if (rret != 1) {
            close(pctx->melodyfd);   // close if we did not get a char
            pctx->melodyfd = -1;
            pctx->pnextnote = (void *) 0;
            return;
        }
        mline[i++] = c;
    } while ((c != '\n') && (i < MXLNLEN));

    // mline[] should now have the frequency, volume, and duration
    sret = sscanf(mline, "%s %d %d", note, &vol, &dur);
    if ((sret != 3) || (vol < 0) || (vol > 100) || (dur < 1) || (dur > MAX_DURATION)) {
        // We quietly ignore invalid lines as comments
        // Schedule an immediate call again
        pctx->pnextnote = add_timer(PC_ONESHOT, 0, read_melody_note, (void *) pctx);
        return;
    }
    // Get the frequency
    sret = sscanf(note, "%f", &freq);
    if (sret != 1) {                // not a number, a musical note maybe?
        freq = lookup_note(note);
    }
    if ((freq > 10000.0) || (freq < 10.0)) {
        // We quietly ignore invalid lines as comments
        pctx->pnextnote = add_timer(PC_ONESHOT, 0, read_melody_note, (void *) pctx);
        return;
    }
    // The note looks valid.  Send it to the FPGA
    notetofpga(pctx, freq, vol, dur);
    pctx->pnextnote = add_timer(PC_ONESHOT, dur, read_melody_note, (void *) pctx);

    return;
}


/**************************************************************
 * notetofpga():  - Send a note to the FPGA.  Convert the freq
 * to a phase offset at 100 KHz.
 **************************************************************/
static void notetofpga(
    TONEGENDEV *pctx,  // context for this peripheral
    float    freq,     // Frequency in Hz between 10 and 10000
    int      vol,      // Volume in range of 0 to 100
    int      dur)      // Duration in range of 1 to 4095
{
    PC_PKT   pkt;      // send write and read cmds to the tif
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      pktlen;   // size of outgoing packet
    int      phase;    // Phase offset per 100KHz cycle

    pslot = pctx->pslot;
    pmycore = pslot->pcore;
    pkt.cmd = PC_CMD_OP_WRITE | PC_CMD_AUTOINC;
    pkt.core = (pslot->pcore)->core_id;

    // Phase offset is measured in fractions of a full cycle at 100KHz
    // So the output will advance by freq/100000ths of a cycle in 10us.
    phase = (int)((freq/100000.0) * (1 << 24));

    pkt.reg = TG_REG_DURAT;   // first reg is duration
    pkt.data[0] = dur & 0xff;                 // duration
    pkt.data[1] = (dur >> 8) & 0xff;          // duration high bits
    pkt.data[2] = phase & 0x0000ff;           // low phase offset
    pkt.data[3] = (phase >> 8) & 0x0000ff;    // mid phase offset
    pkt.data[4] = (phase >> 16) & 0x0000ff;   // high phase offset
    pkt.data[5] = (Tgvolume[vol].pwm1 << 4) + Tgvolume[vol].pwm0;
    pkt.data[6] = (Tgvolume[vol].pwm3 << 4) + Tgvolume[vol].pwm1;
    pkt.count = 7;
    pktlen = 4 + pkt.count;    // 4 header + 7 data

    // Packet is built.  Send it and start an ACK timer 
    txret = pc_tx_pkt(pmycore, &pkt, pktlen);
    if (txret != 0) {
        // the send of the new values did not succeed.  This probably
        // means the input buffer to the FPGS serial port is full.
        pclog("Tonegen failed to send packet. Link overloaded?");
        return;
    }
    if (pctx->ptimer == 0) {
        pctx->ptimer = add_timer(PC_ONESHOT, 100, noAck, (void *) pctx);
    }

    return;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void      *timer,   // handle of the timer that expired
    TONEGENDEV *pctx)      // points to instance of this peripheral
{
    // Log the missing ack
    pclog(E_NOACK);

    return;
}


/**************************************************************
 * load_Tgnote():  Load the table that translates music notation
 * to frequency.  Edit this if you don't want middle A at 440 Hz.
 **************************************************************/
static void load_Tgnote()
{
    int    i;
    TGNOTE default_notes[N_NOTES] = {
        { "C0", 16.35 },
        { "C#0", 17.32 },
        { "D0", 18.35 },
        { "D#0", 19.45 },
        { "E0", 20.60 },
        { "F0", 21.83 },
        { "F#0", 23.12 },
        { "G0", 24.50 },
        { "G#0", 25.96 },
        { "A0", 27.50 },
        { "A#0", 29.14 },
        { "B0", 30.87 },
        { "C1", 32.70 },
        { "C#1", 34.65 },
        { "D1", 36.71 },
        { "D#1", 38.89 },
        { "E1", 41.20 },
        { "F1", 43.65 },
        { "F#1", 46.25 },
        { "G1", 49.00 },
        { "G#1", 51.91 },
        { "A1", 55.00 },
        { "A#1", 58.27 },
        { "B1", 61.74 },
        { "C2", 65.41 },
        { "C#2", 69.30 },
        { "D2", 73.42 },
        { "D#2", 77.78 },
        { "E2", 82.41 },
        { "F2", 87.31 },
        { "F#2", 92.50 },
        { "G2", 98.00 },
        { "G#2", 103.83 },
        { "A2", 110.00 },
        { "A#2", 116.54 },
        { "B2", 123.47 },
        { "C3", 130.81 },
        { "C#3", 138.59 },
        { "D3", 146.83 },
        { "D#3", 155.56 },
        { "E3", 164.81 },
        { "F3", 174.61 },
        { "F#3", 185.00 },
        { "G3", 196.00 },
        { "G#3", 207.65 },
        { "A3", 220.00 },
        { "A#3", 233.08 },
        { "B3", 246.94 },
        { "C4", 261.63 },
        { "C#4", 277.18 },
        { "D4", 293.66 },
        { "D#4", 311.13 },
        { "E4", 329.63 },
        { "F4", 349.23 },
        { "F#4", 369.99 },
        { "G4", 392.00 },
        { "G#4", 415.30 },
        { "A4", 440.00 },
        { "A#4", 466.16 },
        { "B4", 493.88 },
        { "C5", 523.25 },
        { "C#5", 554.37 },
        { "D5", 587.33 },
        { "D#5", 622.25 },
        { "E5", 659.25 },
        { "F5", 698.46 },
        { "F#5", 739.99 },
        { "G5", 783.99 },
        { "G#5", 830.61 },
        { "A5", 880.00 },
        { "A#5", 932.33 },
        { "B5", 987.77 },
        { "C6", 1046.50 },
        { "C#6", 1108.73 },
        { "D6", 1174.66 },
        { "D#6", 1244.51 },
        { "E6", 1318.51 },
        { "F6", 1396.91 },
        { "F#6", 1479.98 },
        { "G6", 1567.98 },
        { "G#6", 1661.22 },
        { "A6", 1760.00 },
        { "A#6", 1864.66 },
        { "B6", 1975.53 },
        { "C7", 2093.00 },
        { "C#7", 2217.46 },
        { "D7", 2349.32 },
        { "D#7", 2489.02 },
        { "E7", 2637.02 },
        { "F7", 2793.83 },
        { "F#7", 2959.96 },
        { "G7", 3135.96 },
        { "G#7", 3322.44 },
        { "A7", 3520.00 },
        { "A#7", 3729.31 },
        { "B7", 3951.07 },
        { "C8", 4186.01 },
        { "C#8", 4434.92 },
        { "D8", 4698.63 },
        { "D#8", 4978.03 },
        { "E8", 5274.04 },
        { "F8", 5587.65 },
        { "F#8", 5919.91 },
        { "G8", 6271.93 },
        { "G#8", 6644.88 },
        { "A8", 7040.00 },
        { "A#8", 7458.62 },
        { "B8", 7902.13 },
    };

    for (i = 0; i < N_NOTES; i++) {
        Tgnote[i].music = default_notes[i].music;
        Tgnote[i].freq  = default_notes[i].freq;
    }

    return;
}


/**************************************************************
 * lookup_note():  See if the note passed in as a string is in
 * Tgnote table.  If so, return its frequency, if not return -1.
 **************************************************************/
static float lookup_note(char *note)
{
    float    freq = -1.0;        // frequency to return
    int      i;

    if (strnlen(note, 4) < 2)
        return(freq);            // can not be valid if too short

    note[0] = toupper(note[0]);  // Table has upper case notes

    for (i = 0; i < N_NOTES; i++) {
        if (strncmp(Tgnote[i].music, note, 4) == 0) {
            freq = Tgnote[i].freq;
            break;
        }
    }

    return(freq);
}

/**************************************************************
 * load_Tgvolume():  Load the table that translates user visible
 * volume in the range of 0 to 100 into the PWM values to apply
 * to the DAC pins.
 **************************************************************/
static void load_Tgvolume()
{
    int      i;
    TGVOLUME default_volumes[MAX_VOLUME + 1] = {
        {0, 0, 0, 0},
        {0, 0, 0, 9},
        {0, 0, 0, 10},
        {0, 0, 0, 11},
        {0, 0, 0, 12},
        {0, 0, 0, 13},
        {0, 0, 0, 14},
        {0, 0, 0, 15},
        {0, 0, 1, 8},
        {0, 0, 1, 9},
        {0, 0, 1, 10},
        {0, 0, 1, 11},
        {0, 0, 1, 12},
        {0, 0, 1, 13},
        {0, 0, 1, 14},
        {0, 0, 1, 15},
        {0, 0, 2, 10},
        {0, 0, 2, 11},
        {0, 0, 2, 12},
        {0, 0, 2, 13},
        {0, 0, 2, 14},
        {0, 0, 2, 15},
        {0, 0, 3, 12},
        {0, 0, 3, 13},
        {0, 0, 3, 14},
        {0, 0, 3, 15},
        {0, 0, 4, 13},
        {0, 0, 4, 14},
        {0, 0, 5, 12},
        {0, 0, 5, 13},
        {0, 0, 5, 15},
        {0, 0, 6, 13},
        {0, 0, 6, 15},
        {0, 0, 7, 13},
        {0, 0, 7, 15},
        {0, 0, 8, 13},
        {0, 0, 8, 15},
        {0, 0, 9, 14},
        {0, 0, 10, 12},
        {0, 0, 10, 15},
        {0, 0, 11, 14},
        {0, 0, 12, 13},
        {0, 0, 13, 12},
        {0, 0, 13, 15},
        {0, 0, 14, 14},
        {0, 0, 15, 14},
        {0, 1, 12, 15},
        {0, 1, 13, 15},
        {0, 1, 14, 15},
        {0, 1, 15, 15},
        {0, 2, 13, 13},
        {0, 2, 14, 14},
        {0, 2, 15, 15},
        {0, 3, 13, 13},
        {0, 3, 14, 15},
        {0, 4, 12, 13},
        {0, 4, 14, 12},
        {0, 4, 15, 14},
        {0, 5, 13, 14},
        {0, 5, 15, 13},
        {0, 6, 13, 14},
        {0, 6, 15, 14},
        {0, 7, 13, 15},
        {0, 8, 12, 13},
        {0, 8, 14, 14},
        {0, 9, 13, 13},
        {0, 9, 15, 15},
        {0, 10, 14, 15},
        {0, 11, 13, 15},
        {0, 12, 13, 12},
        {0, 13, 12, 14},
        {0, 13, 15, 15},
        {0, 14, 15, 13},
        {0, 15, 15, 13},
        {1, 13, 12, 14},
        {1, 14, 12, 15},
        {1, 15, 13, 13},
        {2, 12, 15, 12},
        {2, 13, 15, 15},
        {2, 15, 13, 13},
        {3, 12, 15, 15},
        {3, 14, 13, 14},
        {3, 15, 15, 14},
        {4, 13, 15, 12},
        {4, 15, 13, 15},
        {5, 13, 14, 12},
        {5, 15, 13, 13},
        {6, 13, 14, 13},
        {6, 15, 14, 13},
        {7, 14, 12, 13},
        {8, 12, 14, 12},
        {8, 14, 15, 14},
        {9, 13, 14, 15},
        {10, 12, 14, 13},
        {10, 15, 13, 14},
        {11, 14, 14, 12},
        {12, 13, 15, 13},
        {13, 13, 13, 13},
        {14, 12, 15, 15},
        {15, 14, 12, 12},
        {15, 15, 15, 15},
    };
    for (i = 0; i < MAX_VOLUME + 1; i++) {
        Tgvolume[i].pwm3 = default_volumes[i].pwm3;     // PWM value for pin3
        Tgvolume[i].pwm2 = default_volumes[i].pwm2;     // PWM value for pin2
        Tgvolume[i].pwm1 = default_volumes[i].pwm1;     // PWM value for pin1
        Tgvolume[i].pwm0 = default_volumes[i].pwm0;     // PWM value for pin0
    }

    return;

    /* The above table is generated using the following Octave program.
     * Value at 0 and 100 are set manually.

      % Generate 100 points on a log curve and map the gain
      % to setting in pwm - nonlinear DAC scheme.  This gives
      % the actual table to use in the API driver modules.
      % Manually add {0,0,0,0} and {15,15,15,15}.

      % Get the target gains
      x = 1:1:100;
      out = exp((5 .* x) ./ 100);
      out = out ./ out(100);  % normalize to the maximum of the log table
      target_idx = 1;
      % loop past all possible gains recording the first to pass out(target_idx)
       for i3 = 0:15;
        for i2 = 0:15;
         for i1 = 0:15;
          for i0 = 0:15;
           % Use this for the non-linear 2R-R DAC
           gain =((i3 .* .73203) + (i2 .* .19608) + (i1 .* 0.05229) + (i0 .* 0.01307)) ./ 15;
           % Use this for a linear R-2R DAC 
           %gain =((i3 .* .5) + (i2 .* .25) + (i1 .* 0.125) + (i0 .* 0.0625)) ./ 15;
           if gain > out(target_idx),
               %printf("%d %f {%d, %d, %d, %d},\n", target_idx, gain, i3, i2, i1, i0);
               printf("{%d, %d, %d, %d},\n", i3, i2, i1, i0);
               target_idx = target_idx + 1;
           end
          end
         end
        end
       end

     */
}






