/*-
 * Copyright (c) 1991 The Regents of the University of California.
 * All rights reserved.
 *
 * %sccs.include.redist.c%
 *
 *	@(#)tp_trace.h	7.4 (Berkeley) %G%
 */

/***********************************************************
		Copyright IBM Corporation 1987

                      All Rights Reserved

Permission to use, copy, modify, and distribute this software and its 
documentation for any purpose and without fee is hereby granted, 
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in 
supporting documentation, and that the name of IBM not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.  

IBM DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
IBM BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

******************************************************************/

/*
 * ARGO Project, Computer Sciences Dept., University of Wisconsin - Madison
 */
/* 
 * ARGO TP
 *
 * $Header: tp_trace.h,v 5.1 88/10/12 12:21:51 root Exp $
 * $Source: /usr/argo/sys/netiso/RCS/tp_trace.h,v $
 *
 * 
 * Definitions needed for the protocol trace mechanism.
 */

#ifndef __TP_TRACE__
#define __TP_TRACE__

#ifdef TPPT

#define TPPTsendack	1
#define TPPTgotack	2
#define TPPTXack	3
#define TPPTgotXack	4
#define TPPTack		5
#define TPPTindicate	6
#define TPPTusrreq	7
#define TPPTmisc	8
#define TPPTpcb		9
#define TPPTref		10
#define TPPTtpduin	11
#define TPPTparam	12
#define TPPTertpdu	13
#define TPPTdriver	14
#define TPPTtpduout	15

#include "../netiso/tp_pcb.h"

/* this #if is to avoid lint */

#if  defined(TP_TRACEFILE)||!defined(KERNEL)

#include "../netiso/tp_tpdu.h"

#define TPTRACE_STRLEN 50


/* for packet tracing */
struct tp_timeval {
	SeqNum	tptv_seq;
	u_int tptv_kind;
	u_int tptv_window;
	u_int tptv_size;
};

struct	tp_Trace {
	u_int	tpt_event;
	u_int	tpt_arg;
	u_int 	tpt_arg2;
	int	tpt_tseq;
	struct timeval	tpt_time;
	union {
		struct inpcb	tpt_Inpcb; /* protocol control block */
		struct tp_ref 	tpt_Ref; /* ref part of pcb */
		struct tpdu 	tpt_Tpdu; /* header*/
		struct tp_param tpt_Param; /* ?? bytes, make sure < 128??*/
		struct tp_timeval tpt_Time;
		struct {
			u_int tptm_2;
			u_int tptm_3;
			u_int tptm_4;
			u_int tptm_5;
			char tpt_Str[TPTRACE_STRLEN];
			u_int tptm_1;
		} tptmisc;
		u_char 			tpt_Ertpdu; /* use rest of structure */
	} tpt_stuff;
};
#define tpt_inpcb tpt_stuff.tpt_Inpcb
#define tpt_pcb tpt_stuff.tpt_Pcb
#define tpt_ref tpt_stuff.tpt_Ref
#define tpt_tpdu tpt_stuff.tpt_Tpdu
#define tpt_param tpt_stuff.tpt_Param
#define tpt_ertpdu tpt_stuff.tpt_Ertpdu
#define tpt_str tpt_stuff.tptmisc.tpt_Str
#define tpt_m1 tpt_stuff.tptmisc.tptm_1
#define tpt_m2 tpt_stuff.tptmisc.tptm_2
#define tpt_m3 tpt_stuff.tptmisc.tptm_3
#define tpt_m4 tpt_stuff.tptmisc.tptm_4
#define tpt_m5 tpt_stuff.tptmisc.tptm_5

#define tpt_seq tpt_stuff.tpt_Time.tptv_seq
#define tpt_kind tpt_stuff.tpt_Time.tptv_kind
#define tpt_window tpt_stuff.tpt_Time.tptv_window
#define tpt_size tpt_stuff.tpt_Time.tptv_size

#define TPTRACEN 300
int tp_Tracen = 0;
struct tp_Trace tp_Trace[TPTRACEN];

#endif defined(TP_TRACEFILE)||!defined(KERNEL)

extern u_char	tp_traceflags[];

#define IFTRACE(ascii)\
	if(tp_traceflags[ascii]) {
/* 
 * for some reason lint complains about tp_param being undefined no
 * matter where or how many times I define it.
 */


#define ENDTRACE  }

#define tptrace(A,B,C,D,E,F) \
	tpTrace((struct tp_pcb *)0,\
	(u_int)(A),(u_int)(B),(u_int)(C),(u_int)(D),(u_int)(E),(u_int)(F))

#define tptraceTPCB(A,B,C,D,E,F) \
	tpTrace(tpcb,\
	(u_int)(A),(u_int)(B),(u_int)(C),(u_int)(D),(u_int)(E),(u_int)(F))

extern void tpTrace();

#else  TPPT

/***********************************************
 * NO TPPT TRACE STUFF
 **********************************************/

#ifndef STAR
#define STAR *
#endif	STAR
#define IFTRACE(ascii)	 //*beginning of comment*/STAR
#define ENDTRACE	 STAR/*end of comment*//

#endif TPPT

#endif __TP_TRACE__
