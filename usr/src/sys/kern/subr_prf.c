/*-
 * Copyright (c) 1986, 1988, 1991 The Regents of the University of California.
 * All rights reserved.
 *
 * %sccs.include.redist.c%
 *
 *	@(#)subr_prf.c	7.29 (Berkeley) %G%
 */

#include "param.h"
#include "systm.h"
#include "buf.h"
#include "conf.h"
#include "reboot.h"
#include "msgbuf.h"
#include "proc.h"
#include "ioctl.h"
#include "vnode.h"
#include "file.h"
#include "tty.h"
#include "tprintf.h"
#include "syslog.h"
#include "malloc.h"

/*
 * Note that stdarg.h and the ANSI style va_start macro is used for both
 * ANSI and traditional C compilers.
 */
#include <machine/stdarg.h>

#ifdef KADB
#include "machine/kdbparam.h"
#endif

#define TOCONS	0x01
#define TOTTY	0x02
#define TOLOG	0x04

struct	tty *constty;			/* pointer to console "window" tty */

#ifdef KADB
extern	cngetc();			/* standard console getc */
int	(*v_getc)() = cngetc;		/* "" getc from virtual console */
extern	cnpoll();
int	(*v_poll)() = cnpoll;		/* kdb hook to enable input polling */
#endif
extern	cnputc();			/* standard console putc */
int	(*v_putc)() = cnputc;		/* routine to putc on virtual console */

static void  logpri __P((int level));
static void  putchar __P((int ch, int flags, struct tty *tp));
static char *ksprintn __P((u_long num, int base, int *len));
void  kprintf __P((const char *fmt, int flags, struct tty *tp, va_list));

extern	cnputc();			/* standard console putc */
extern	struct tty cons;		/* standard console tty */
struct	tty *constty;			/* pointer to console "window" tty */
int	(*v_console)() = cnputc;	/* routine to putc on virtual console */

/*
 * Variable panicstr contains argument to first call to panic; used
 * as flag to indicate that the kernel has already called panic.
 */
char	*panicstr;

/*
 * Panic is called on unresolvable fatal errors.  It prints "panic: mesg",
 * and then reboots.  If we are called twice, then we avoid trying to sync
 * the disks as this often leads to recursive panics.
 */
void
panic(msg)
	char *msg;
{
	int bootopt = RB_AUTOBOOT | RB_DUMP;

	if (panicstr)
		bootopt |= RB_NOSYNC;
	else
		panicstr = msg;
	printf("panic: %s\n", msg);
#ifdef KGDB
	kgdb_panic();
#endif
#ifdef KADB
	if (boothowto & RB_KDB) {
		int s = splnet();	/* below kdb pri */
		setsoftkdb();
		splx(s);
	}
#endif
	boot(bootopt);
}

/*
 * Warn that a system table is full.
 */
void
tablefull(tab)
	char *tab;
{

	log(LOG_ERR, "%s: table is full\n", tab);
}

/*
 * Uprintf prints to the controlling terminal for the current process.
 * It may block if the tty queue is overfull.  No message is printed if
 * the queue does not clear in a reasonable time.
 */
void
#ifdef __STDC__
uprintf(const char *fmt, ...)
#else
uprintf(fmt /*, va_alist */)
	char *fmt;
#endif
{
	register struct proc *p = curproc;
	va_list ap;

	if (p->p_flag & SCTTY && p->p_session->s_ttyvp) {
		va_start(ap, fmt);
		kprintf(fmt, TOTTY, p->p_session->s_ttyp, ap);
		va_end(ap);
	}
}

tpr_t
tprintf_open(p)
	register struct proc *p;
{

	if (p->p_flag & SCTTY && p->p_session->s_ttyvp) {
		SESSHOLD(p->p_session);
		return ((tpr_t) p->p_session);
	}
	return ((tpr_t) NULL);
}

void
tprintf_close(sess)
	tpr_t sess;
{

	if (sess)
		SESSRELE((struct session *) sess);
}

/*
 * tprintf prints on the controlling terminal associated
 * with the given session.
 */
void
#ifdef __STDC__
tprintf(tpr_t tpr, const char *fmt, ...)
#else
tprintf(tpr, fmt /*, va_alist */)
	tpr_t tpr;
	char *fmt;
#endif
{
	register struct session *sess = (struct session *)tpr;
	struct tty *tp = NULL;
	int flags = TOLOG;
	va_list ap;

	logpri(LOG_INFO);
	if (sess && sess->s_ttyvp && ttycheckoutq(sess->s_ttyp, 0)) {
		flags |= TOTTY;
		tp = sess->s_ttyp;
	}
	va_start(ap, fmt);
	kprintf(fmt, flags, tp, ap);
	va_end(ap);
	logwakeup();
}

/*
 * Ttyprintf displays a message on a tty; it should be used only by
 * the tty driver, or anything that knows the underlying tty will not
 * be revoke(2)'d away.  Other callers should use tprintf.
 */
void
#ifdef __STDC__
ttyprintf(struct tty *tp, const char *fmt, ...)
#else
ttyprintf(tp, fmt /*, va_alist */)
	struct tty *tp;
	char *fmt;
#endif
{
	va_list ap;

	va_start(ap, fmt);
	kprintf(fmt, TOTTY, tp, ap);
	va_end(ap);
}

extern	int log_open;

/*
 * Log writes to the log buffer, and guarantees not to sleep (so can be
 * called by interrupt routines).  If there is no process reading the
 * log yet, it writes to the console also.
 */
void
#ifdef __STDC__
log(int level, const char *fmt, ...)
#else
log(level, fmt /*, va_alist */)
	int level;
	char *fmt;
#endif
{
	register int s = splhigh();
	va_list ap;

	logpri(level);
	va_start(ap, fmt);
	kprintf(fmt, TOLOG, NULL, ap);
	splx(s);
	va_end(ap);
	if (!log_open) {
		va_start(ap, fmt);
		kprintf(fmt, TOCONS, NULL, ap);
		va_end(ap);
	}
	logwakeup();
}

static void
logpri(level)
	int level;
{
	register int ch;
	register char *p;

	putchar('<', TOLOG, NULL);
	for (p = ksprintn((u_long)level, 10, NULL); ch = *p--;)
		putchar(ch, TOLOG, NULL);
	putchar('>', TOLOG, NULL);
}

void
#ifdef __STDC__
addlog(const char *fmt, ...)
#else
addlog(fmt /*, va_alist */)
	char *fmt;
#endif
{
	register int s = splhigh();
	va_list ap;

	va_start(ap, fmt);
	kprintf(fmt, TOLOG, NULL, ap);
	splx(s);
	va_end(ap);
	if (!log_open) {
		va_start(ap, fmt);
		kprintf(fmt, TOCONS, NULL, ap);
		va_end(ap);
	}
	logwakeup();
}

#if defined(tahoe)
int	consintr = 1;			/* ok to handle console interrupts? */
#endif

void
#ifdef __STDC__
printf(const char *fmt, ...)
#else
printf(fmt /*, va_alist */)
	char *fmt;
#endif
{
	va_list ap;
#ifdef tahoe
	register int savintr;

	savintr = consintr;		/* disable interrupts */
	consintr = 0;
#endif
	va_start(ap, fmt);
	kprintf(fmt, TOCONS | TOLOG, NULL, ap);
	va_end(ap);
	if (!panicstr)
		logwakeup();
#ifdef tahoe
	consintr = savintr;		/* reenable interrupts */
#endif
}

/*
 * Scaled down version of printf(3).
 *
 * Two additional formats:
 *
 * The format %b is supported to decode error registers.
 * Its usage is:
 *
 *	printf("reg=%b\n", regval, "<base><arg>*");
 *
 * where <base> is the output base expressed as a control character, e.g.
 * \10 gives octal; \20 gives hex.  Each arg is a sequence of characters,
 * the first of which gives the bit number to be inspected (origin 1), and
 * the next characters (up to a control character, i.e. a character <= 32),
 * give the name of the register.  Thus:
 *
 *	printf("reg=%b\n", 3, "\10\2BITTWO\1BITONE\n");
 *
 * would produce output:
 *
 *	reg=3<BITTWO,BITONE>
 *
 * The format %r is supposed to pass an additional format string and argument
 * list recursively.
 * Its usage is:
 *
 * fn(otherstuff, char *fmt, ...)
 * {
 *	va_list ap;
 *	va_start(ap, fmt);
 *	printf("prefix: %r, other stuff\n", fmt, ap);
 *	va_end(ap);
 *
 * Space or zero padding and a field width are supported for the numeric
 * formats only.
 */
void
kprintf(fmt, flags, tp, ap)
	register const char *fmt;
	int flags;
	struct tty *tp;
	va_list ap;
{
	register char *p;
	register int ch, n;
	u_long ul;
	int base, lflag, tmp, width;
	char padc;

	for (;;) {
		padc = ' ';
		width = 0;
		while ((ch = *(u_char *)fmt++) != '%') {
			if (ch == '\0')
				return;
			putchar(ch, flags, tp);
		}
		lflag = 0;
reswitch:	switch (ch = *(u_char *)fmt++) {
		case '0':
			padc = '0';
			goto reswitch;
		case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			for (width = 0;; ++fmt) {
				width = width * 10 + ch - '0';
				ch = *fmt;
				if (ch < '0' || ch > '9')
					break;
			}
			goto reswitch;
		case 'l':
			lflag = 1;
			goto reswitch;
		case 'b':
			ul = va_arg(ap, int);
			p = va_arg(ap, char *);
			for (p = ksprintn(ul, *p++, NULL); ch = *p--;)
				putchar(ch, flags, tp);

			if (!ul)
				break;

			for (tmp = 0; n = *p++;) {
				if (ul & (1 << (n - 1))) {
					putchar(tmp ? ',' : '<', flags, tp);
					for (; (n = *p) > ' '; ++p)
						putchar(n, flags, tp);
					tmp = 1;
				} else
					for (; *p > ' '; ++p);
			}
			if (tmp)
				putchar('>', flags, tp);
			break;
		case 'c':
			putchar(va_arg(ap, int), flags, tp);
			break;
		case 'r':
			p = va_arg(ap, char *);
			kprintf(p, flags, tp, va_arg(ap, va_list));
			break;
		case 's':
			p = va_arg(ap, char *);
			while (ch = *p++)
				putchar(ch, flags, tp);
			break;
		case 'd':
			ul = lflag ? va_arg(ap, long) : va_arg(ap, int);
			if ((long)ul < 0) {
				putchar('-', flags, tp);
				ul = -(long)ul;
			}
			base = 10;
			goto number;
		case 'o':
			ul = lflag ? va_arg(ap, u_long) : va_arg(ap, u_int);
			base = 8;
			goto number;
		case 'u':
			ul = lflag ? va_arg(ap, u_long) : va_arg(ap, u_int);
			base = 10;
			goto number;
		case 'x':
			ul = lflag ? va_arg(ap, u_long) : va_arg(ap, u_int);
			base = 16;
number:			p = ksprintn(ul, base, &tmp);
			if (width && (width -= tmp) > 0)
				while (width--)
					putchar(padc, flags, tp);
			while (ch = *p--)
				putchar(ch, flags, tp);
			break;
		default:
			putchar('%', flags, tp);
			if (lflag)
				putchar('l', flags, tp);
			/* FALLTHROUGH */
		case '%':
			putchar(ch, flags, tp);
		}
	}
}

/*
 * Print a character on console or users terminal.  If destination is
 * the console then the last MSGBUFS characters are saved in msgbuf for
 * inspection later.
 */
static void
putchar(c, flags, tp)
	register int c;
	int flags;
	struct tty *tp;
{
	extern int msgbufmapped;
	register struct msgbuf *mbp = msgbufp;

	if (panicstr)
		constty = NULL;
	if ((flags & TOCONS) && tp == NULL && constty) {
		tp = constty;
		flags |= TOTTY;
	}
	if ((flags & TOCONS) && panicstr == 0 && tp == 0 && constty) {
		tp = constty;
		flags |= TOTTY;
	}
	if ((flags & TOTTY) && tp && tputchar(c, tp) < 0 &&
	    (flags & TOCONS) && tp == constty)
		constty = NULL;
	if ((flags & TOLOG) &&
	    c != '\0' && c != '\r' && c != 0177 && msgbufmapped) {
		if (mbp->msg_magic != MSG_MAGIC) {
			bzero((caddr_t)mbp, sizeof(*mbp));
			mbp->msg_magic = MSG_MAGIC;
		}
		mbp->msg_bufc[mbp->msg_bufx++] = c;
		if (mbp->msg_bufx < 0 || mbp->msg_bufx >= MSG_BSIZE)
			mbp->msg_bufx = 0;
	}
		(*v_console)(c);
}
