/*-
 * Copyright (c) 1998 Doug Rabson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * Based very closely on NetBSD version-
 *
 * Copyright (c) 1997 by Matthew Jacob
 * NASA AMES Research Center.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice immediately at the beginning of the file, without modification,
 *    this list of conditions, and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "opt_simos.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <machine/bus.h>
#include <sys/rman.h>

#include <machine/swiz.h>
#include <machine/intr.h>
#include <machine/intrcnt.h>
#include <machine/resource.h>
#include <machine/sgmap.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
 

#include <alpha/tlsb/tlsbreg.h>
#include <alpha/tlsb/tlsbvar.h>

#include <alpha/tlsb/kftxxreg.h>
#include <alpha/tlsb/kftxxvar.h>

#include <alpha/tlsb/dwlpxreg.h>
#include <alpha/tlsb/dwlpxvar.h>
#include <alpha/pci/pcibus.h>
#include <pci/pcivar.h>

static devclass_t	dwlpx_devclass;
static device_t		dwlpxs[DWLPX_NIONODE][DWLPX_NHOSE];


#define KV(pa)		((void *)ALPHA_PHYS_TO_K0SEG(pa))

struct dwlpx_softc {
	struct dwlpx_softc *next;
	device_t	dev;		/* backpointer */
	u_int64_t	sysbase;	/* shorthand */
	vm_offset_t	dmem_base;	/* dense memory */
	vm_offset_t	smem_base;	/* sparse memory */
	vm_offset_t	io_base;	/* sparse i/o */
	int		bushose;	/* our bus && hose */
	u_int		:	26,
		nhpc 	:	2,	/* how many HPCs */
		dwlpb	:	1,	/* this is a DWLPB */
		sgmapsz	:	3;	/* Scatter Gather map size */
};

static int dwlpx_probe(device_t dev);
static int dwlpx_attach(device_t dev);

static int dwlpx_setup_intr(device_t, device_t, struct resource *, int,
    driver_intr_t *, void *, void **);
static int
dwlpx_teardown_intr(device_t, device_t, struct resource *, void *);
static driver_intr_t dwlpx_intr;

static device_method_t dwlpx_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		dwlpx_probe),
	DEVMETHOD(device_attach,	dwlpx_attach),

	/* Bus interface */
	DEVMETHOD(bus_setup_intr,	dwlpx_setup_intr),
	DEVMETHOD(bus_teardown_intr,	dwlpx_teardown_intr),
	DEVMETHOD(bus_alloc_resource,	pci_alloc_resource),
	DEVMETHOD(bus_release_resource,	pci_release_resource),
	DEVMETHOD(bus_activate_resource,	pci_activate_resource),
	DEVMETHOD(bus_deactivate_resource,	pci_deactivate_resource),

	{ 0, 0 }
};
static driver_t dwlpx_driver = {
	"dwlpx", dwlpx_methods, sizeof (struct dwlpx_softc)
};

static u_int32_t imaskcache[DWLPX_NIONODE][DWLPX_NHOSE][NHPC];
static void dwlpx_eintr(unsigned long);

/*
 * Direct-mapped window: 2G at 2G
 */
#define	DWLPx_DIRECT_MAPPED_BASE	(2UL*1024UL*1024UL*1024UL)
#define	DWLPx_DIRECT_MAPPED_SIZE	(2UL*1024UL*1024UL*1024UL)
#define	DWLPx_DIRECT_MAPPED_WMASK	PCIA_WMASK_2G

/*
 * SGMAP window A: 256M at 1.75G or 1G at 1G
 */
#define	DWLPx_SG_MAPPED_SIZE(x)		((x) * PAGE_SIZE)
static void dwlpx_dma_init(struct dwlpx_softc *);



#define DWLPX_SOFTC(dev)	(struct dwlpx_softc *) device_get_softc(dev)
static struct dwlpx_softc *dwlpx_root;

static alpha_chipset_inb_t	dwlpx_inb;
static alpha_chipset_inw_t	dwlpx_inw;
static alpha_chipset_inl_t	dwlpx_inl;
static alpha_chipset_outb_t	dwlpx_outb;
static alpha_chipset_outw_t	dwlpx_outw;
static alpha_chipset_outl_t	dwlpx_outl;
static alpha_chipset_readb_t	dwlpx_readb;
static alpha_chipset_readw_t	dwlpx_readw;
static alpha_chipset_readl_t	dwlpx_readl;
static alpha_chipset_writeb_t	dwlpx_writeb;
static alpha_chipset_writew_t	dwlpx_writew;
static alpha_chipset_writel_t	dwlpx_writel;
static alpha_chipset_maxdevs_t	dwlpx_maxdevs;
static alpha_chipset_cfgreadb_t	dwlpx_cfgreadb;
static alpha_chipset_cfgreadw_t	dwlpx_cfgreadw;
static alpha_chipset_cfgreadl_t	dwlpx_cfgreadl;
static alpha_chipset_cfgwriteb_t dwlpx_cfgwriteb;
static alpha_chipset_cfgwritew_t dwlpx_cfgwritew;
static alpha_chipset_cfgwritel_t dwlpx_cfgwritel;

static alpha_chipset_t dwlpx_chipset = {
	dwlpx_inb,
	dwlpx_inw,
	dwlpx_inl,
	dwlpx_outb,
	dwlpx_outw,
	dwlpx_outl,
	dwlpx_readb,
	dwlpx_readw,
	dwlpx_readl,
	dwlpx_writeb,
	dwlpx_writew,
	dwlpx_writel,
	dwlpx_maxdevs,
	dwlpx_cfgreadb,
	dwlpx_cfgreadw,
	dwlpx_cfgreadl,
	dwlpx_cfgwriteb,
	dwlpx_cfgwritew,
	dwlpx_cfgwritel,
};

#define	DWLPX_IONODE(port)	((port >> 29) & 0x7)
#define	DWLPX_HOSE(port)	((port >> 27) & 0x3)
#define	DWLPX_INST(port)	dwlpxs[DWLPX_IONODE(port)][DWLPX_HOSE(port)]
#define	DWLPX_ADDR(port)	(port & 0x07ffffff)

static u_int8_t
dwlpx_inb(u_int32_t port)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(port));
	return SPARSE_READ_BYTE(sc->io_base, DWLPX_ADDR(port));
}

static u_int16_t
dwlpx_inw(u_int32_t port)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(port));
	return SPARSE_READ_WORD(sc->io_base, DWLPX_ADDR(port));
}

static u_int32_t
dwlpx_inl(u_int32_t port)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(port));
	return SPARSE_READ_LONG(sc->io_base, DWLPX_ADDR(port));
}

static void
dwlpx_outb(u_int32_t port, u_int8_t data)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(port));
	SPARSE_WRITE_BYTE(sc->io_base, DWLPX_ADDR(port), data);
	alpha_mb();
}

static void
dwlpx_outw(u_int32_t port, u_int16_t data)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(port));
	SPARSE_WRITE_WORD(sc->io_base, DWLPX_ADDR(port), data);
	alpha_mb();
}

static void
dwlpx_outl(u_int32_t port, u_int32_t data)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(port));
	SPARSE_WRITE_LONG(sc->io_base, DWLPX_ADDR(port), data);
	alpha_mb();
}

static u_int8_t
dwlpx_readb(u_int32_t pa)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(pa));
	return SPARSE_READ_BYTE(sc->smem_base, DWLPX_ADDR(pa));
}

static u_int16_t
dwlpx_readw(u_int32_t pa)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(pa));
	return SPARSE_READ_WORD(sc->smem_base, DWLPX_ADDR(pa));
}

static u_int32_t
dwlpx_readl(u_int32_t pa)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(pa));
	return SPARSE_READ_LONG(sc->smem_base, DWLPX_ADDR(pa));
}

static void
dwlpx_writeb(u_int32_t pa, u_int8_t data)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(pa));
	SPARSE_WRITE_BYTE(sc->smem_base, DWLPX_ADDR(pa), data);
	alpha_mb();
}

static void
dwlpx_writew(u_int32_t pa, u_int16_t data)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(pa));
	SPARSE_WRITE_WORD(sc->smem_base, DWLPX_ADDR(pa), data);
	alpha_mb();
}

static void
dwlpx_writel(u_int32_t pa, u_int32_t data)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(DWLPX_INST(pa));
	SPARSE_WRITE_LONG(sc->smem_base, DWLPX_ADDR(pa), data);
	alpha_mb();
}

static int
dwlpx_maxdevs(u_int b)
{
	return (DWLPX_MAXDEV);
}

static u_int32_t dwlpx_cfgread(u_int, u_int, u_int, u_int, u_int, int);
static void dwlpx_cfgwrite(u_int, u_int, u_int, u_int, u_int, int, u_int32_t);

#if	0
#define	RCFGP	printf
#else
#define	RCFGP	if (0) printf
#endif


static u_int32_t
dwlpx_cfgread(u_int bh, u_int bus, u_int slot, u_int func, u_int off, int sz)
{
	struct dwlpx_softc *sc;
	device_t dev;
	u_int32_t *dp, data, rvp, pci_idsel, hpcdev;
	unsigned long paddr;
	int hose, ionode;
	int s = 0, i;

	RCFGP("CFGREAD %u.%u.%u.%u.%u.%d", bh, bus, slot, func, off, sz);

	rvp = data = ~0;
	if (bh == (u_int8_t)-1)
		bh = bus >> 4;
	ionode = ((bh >> 2) & 0x7);
	hose = (bh & 0x3);
	dev = dwlpxs[ionode][hose];
	if (dev == (device_t) 0) {
		RCFGP(" (no dev)\n");
		return (data);
	}
	sc = DWLPX_SOFTC(dev);
	bus &= 0xf;

	if (sc->nhpc < 1) {
		RCFGP(" (no hpcs)\n");
		return (data);
	} else if (sc->nhpc < 2 && slot >= 4) {
		RCFGP(" (bad hpcs (%d) <> bad slot (%d))\n", sc->nhpc, slot);
		return (data);
	} else if (sc->nhpc < 3 && slot >= 8) {
		RCFGP(" (bad hpcs (%d) <> bad slot (%d))\n", sc->nhpc, slot);
		return (data);
	} else if (slot >= DWLPX_MAXDEV) {
		RCFGP(" (bad slot (%d))\n", slot);
		return (data);
	}
	hpcdev = slot >> 2;
	pci_idsel = (1 << ((slot & 0x3) + 2));
	paddr = (hpcdev << 22) | (pci_idsel << 16) | (func << 13);

	if (bus) {
		paddr &= 0x1fffff;
		paddr |= (bus << 21);
		alpha_pal_draina();
		s = splhigh();
		/*
		 * Set up HPCs for type 1 cycles.
		 */
		for (i = 0; i < sc->nhpc; i++) {
			rvp = REGVAL(PCIA_CTL(i)+sc->sysbase) | PCIA_CTL_T1CYC;
			alpha_mb();
			REGVAL(PCIA_CTL(i) + sc->sysbase) = rvp;
			alpha_mb();
		}
	}

	paddr |= ((unsigned long) ((off >> 2) << 7));
	paddr |= ((sz - 1) << 3);
	paddr |= DWLPX_PCI_CONF;
	paddr |= ((unsigned long) hose) << 34;
	paddr |= ((unsigned long) ionode) << 36;
	paddr |= 1L << 39;

	dp = (u_int32_t *)KV(paddr);
	RCFGP(" hose %d node%d paddr 0x%lx", bh, ionode+4, paddr);
	if (badaddr(dp, sizeof (*dp)) == 0) {
		data = *dp;
	}
	if (bus) {
		alpha_pal_draina();
		for (i = 0; i < sc->nhpc; i++) {
			rvp = REGVAL(PCIA_CTL(i)+sc->sysbase) & ~PCIA_CTL_T1CYC;
			alpha_mb();
			REGVAL(PCIA_CTL(i) + sc->sysbase) = rvp;
			alpha_mb();
		}
		(void) splx(s);
	}

	if (data != ~0) {
		if (sz == 1) {
			rvp = SPARSE_BYTE_EXTRACT(off, data);
		} else if (sz == 2) {
			rvp = SPARSE_WORD_EXTRACT(off, data);
		} else {
			rvp = data;
		}
	} else {
		rvp = data;
	}
	RCFGP(" data %x->0x%x\n", data, rvp);
	return (rvp);
}

#if	0
#define	WCFGP	printf
#else
#define	WCFGP	if (0) printf
#endif

static void
dwlpx_cfgwrite(u_int bh, u_int bus, u_int slot, u_int func, u_int off,
	int sz, u_int32_t data)
{
	int hose, ionode;
	int s = 0, i;
	u_int32_t *dp, rvp, pci_idsel, hpcdev;
	unsigned long paddr;
	struct dwlpx_softc *sc;
	device_t dev;

	WCFGP("CFGWRITE %u.%u.%u.%u.%u.%d", bh, bus, slot, func, off, sz);
	if (bh == (u_int8_t)-1)
		bh = bus >> 4;
	ionode = ((bh >> 2) & 0x7);
	hose = (bh & 0x3);
	dev = dwlpxs[ionode][hose];
	if (dev == (device_t) 0) {
		WCFGP(" (no dev)\n");
		return;
	}
	sc = DWLPX_SOFTC(dev);
	bus &= 0xf;

	if (sc->nhpc < 1) {
		WCFGP(" (no hpcs)\n");
		return;
	} else if (sc->nhpc < 2 && slot >= 4) {
		WCFGP(" (bad hpcs (%d) <> bad slot (%d))\n", sc->nhpc, slot);
		return;
	} else if (sc->nhpc < 3 && slot >= 8) {
		WCFGP(" (bad hpcs (%d) <> bad slot (%d))\n", sc->nhpc, slot);
		return;
	} else if (slot >= DWLPX_MAXDEV) {
		WCFGP(" (bad slot (%d))\n", slot);
		return;
	}
	hpcdev = slot >> 2;
	pci_idsel = (1 << ((slot & 0x3) + 2));
	paddr = (hpcdev << 22) | (pci_idsel << 16) | (func << 13);
	bus = 0;

	if (bus) {
		paddr &= 0x1fffff;
		paddr |= (bus << 21);
		alpha_pal_draina();
		s = splhigh();
		/*
		 * Set up HPCs for type 1 cycles.
		 */
		for (i = 0; i < sc->nhpc; i++) {
			rvp = REGVAL(PCIA_CTL(i)+sc->sysbase) | PCIA_CTL_T1CYC;
			alpha_mb();
			REGVAL(PCIA_CTL(i) + sc->sysbase) = rvp;
			alpha_mb();
		}
	}

	paddr |= ((unsigned long) ((off >> 2) << 7));
	paddr |= ((sz - 1) << 3);
	paddr |= DWLPX_PCI_CONF;
	paddr |= ((unsigned long) hose) << 34;
	paddr |= ((unsigned long) ionode) << 36;
	paddr |= 1L << 39;
	dp = (u_int32_t *)KV(paddr);
	WCFGP(" hose %d node%d paddr 0x%lx\n", bh, ionode+4, paddr);
	if (badaddr(dp, sizeof (*dp)) == 0) {
		u_int32_t new_data;
		if (sz == 1) {
			new_data = SPARSE_BYTE_INSERT(off, data);
		} else if (sz == 2) {
			new_data = SPARSE_WORD_INSERT(off, data);
		} else  {
			new_data = data;
		}
		*dp = new_data;
	}
	if (bus) {
		alpha_pal_draina();
		for (i = 0; i < sc->nhpc; i++) {
			rvp = REGVAL(PCIA_CTL(i)+sc->sysbase) & ~PCIA_CTL_T1CYC;
			alpha_mb();
			REGVAL(PCIA_CTL(i) + sc->sysbase) = rvp;
			alpha_mb();
		}
		(void) splx(s);
	}
}

static u_int8_t
dwlpx_cfgreadb(u_int h, u_int b, u_int s, u_int f, u_int r)
{
	return (u_int8_t) dwlpx_cfgread(h, b, s, f, r, 1);
}

static u_int16_t
dwlpx_cfgreadw(u_int h, u_int b, u_int s, u_int f, u_int r)
{
	return (u_int16_t) dwlpx_cfgread(h, b, s, f, r, 2);
}

static u_int32_t
dwlpx_cfgreadl(u_int h, u_int b, u_int s, u_int f, u_int r)
{
	return dwlpx_cfgread(h, b, s, f, r, 4);
}

static void
dwlpx_cfgwriteb(u_int h, u_int b, u_int s, u_int f, u_int r, u_int8_t data)
{
	dwlpx_cfgwrite(h, b, s, f, r, 1, (u_int32_t) data);
}

static void
dwlpx_cfgwritew(u_int h, u_int b, u_int s, u_int f, u_int r, u_int16_t data)
{
	dwlpx_cfgwrite(h, b, s, f, r, 2, (u_int32_t) data);
}

static void
dwlpx_cfgwritel(u_int h, u_int b, u_int s, u_int f, u_int r, u_int32_t data)
{
	dwlpx_cfgwrite(h, b, s, f, r, 4, (u_int32_t) data);
}

static int
dwlpx_probe(device_t dev)
{
	device_t child;
	u_int32_t ctl;
	struct dwlpx_softc *xc, *sc = DWLPX_SOFTC(dev);
	unsigned long ls;
	int io, hose;

	io = kft_get_node(dev) - 4;
	hose = kft_get_hosenum(dev);

	sc->bushose = (io << 2) | hose; 

	if (dwlpxs[io][hose]) {
		printf("%s: already attached\n", device_get_nameunit(dev));
		return EEXIST;
	}
	if ((xc = dwlpx_root) == NULL) {
		dwlpx_root = sc;
	} else {
		while (xc->next)
			xc = xc->next;
		xc->next = sc;
	}
	sc->dev = dwlpxs[io][hose] = dev;

	ls = DWLPX_BASE(io + 4, hose);
	for (sc->nhpc = 1; sc->nhpc < NHPC; sc->nhpc++) {
		if (badaddr(KV(PCIA_CTL(sc->nhpc) + ls), sizeof (ctl))) {
			break;
		}
	}
	if (sc->nhpc != NHPC) {
		REGVAL(PCIA_ERR(0) + ls) = PCIA_ERR_ALLERR;
	}
	ctl = REGVAL(PCIA_PRESENT + ls);
	if ((ctl >> PCIA_PRESENT_REVSHIFT) & PCIA_PRESENT_REVMASK) {
		sc->dwlpb = 1;
		device_set_desc(dev, "DWLPB PCI adapter");
	} else {
		device_set_desc(dev, "DWLPA PCI adapter");
	}
	sc->sgmapsz = DWLPX_SG32K;

	if (device_get_unit(dev) == 0) {
		pci_init_resources();
	}

	child = device_add_child(dev, "pcib", device_get_unit(dev));
	device_set_ivars(child, &sc->bushose);
	return (0);
}

static int
dwlpx_attach(device_t dev)
{
	struct dwlpx_softc *sc = DWLPX_SOFTC(dev);
	device_t parent = device_get_parent(dev);
	vm_offset_t regs;
	u_int32_t ctl;
	int i, io, hose;
	void *intr;

	io = kft_get_node(dev) - 4;
	hose = kft_get_hosenum(dev);

	chipset = dwlpx_chipset;
	/* chipset.intrdev = dev; */

	sc->sysbase	= DWLPX_BASE(io + 4, hose);
	regs		= (vm_offset_t) KV(sc->sysbase);
	sc->dmem_base	= regs + DWLPX_PCI_DENSE;
	sc->smem_base	= regs + DWLPX_PCI_SPARSE;
	sc->io_base	= regs + DWLPX_PCI_IOSPACE;

	/*
	 * Set up interrupt stuff for this DWLPX.
	 *
	 * Note that all PCI interrupt pins are disabled at this time.
	 *
	 * Do this even for all HPCs- even for the
	 * nonexistent one on hose zero of a KFTIA.
	 */
	for (i = 0; i < NHPC; i++) {
		REGVAL(PCIA_IMASK(i) + sc->sysbase) = DWLPX_IMASK_DFLT;
		REGVAL(PCIA_ERRVEC(i) + sc->sysbase) =
		    DWLPX_ERRVEC(io, hose);
	}

	for (i = 0; i < DWLPX_MAXDEV; i++) {
		u_int16_t vec;
		int ss, hpc;

		vec = DWLPX_MVEC(io, hose, i);
		ss = i;
		if (i < 4) {
			hpc = 0;
		} else if (i < 8) {
			ss -= 4;
			hpc = 1;
		} else {
			ss -= 8;
			hpc = 2;
		}
		REGVAL(PCIA_DEVVEC(hpc, ss, 1) + sc->sysbase) = vec;
		REGVAL(PCIA_DEVVEC(hpc, ss, 2) + sc->sysbase) = vec;
		REGVAL(PCIA_DEVVEC(hpc, ss, 3) + sc->sysbase) = vec;
		REGVAL(PCIA_DEVVEC(hpc, ss, 4) + sc->sysbase) = vec;
	}

	/*
	 * Establish HAE values, as well as make sure of sanity elsewhere.
	 */
	for (i = 0; i < sc->nhpc; i++) {
		ctl = REGVAL(PCIA_CTL(i) + sc->sysbase);
		ctl &= 0x0fffffff;
		ctl &= ~(PCIA_CTL_MHAE(0x1f) | PCIA_CTL_IHAE(0x1f));
		/*
		 * I originally also had it or'ing in 3, which makes no sense.
		 */

		ctl |= PCIA_CTL_RMMENA | PCIA_CTL_RMMARB;

		/*
		 * Only valid if we're attached to a KFTIA or a KTHA.
		 */
		ctl |= PCIA_CTL_3UP;

		ctl |= PCIA_CTL_CUTENA;

		/*
		 * Fit in appropriate S/G Map Ram size.
		 */
		if (sc->sgmapsz == DWLPX_SG32K)
			ctl |= PCIA_CTL_SG32K;
		else if (sc->sgmapsz == DWLPX_SG128K)
			ctl |= PCIA_CTL_SG128K;
		else
			ctl |= PCIA_CTL_SG32K;

		REGVAL(PCIA_CTL(i) + sc->sysbase) = ctl;
	}

	/*
	 * Enable TBIT if required
	 */
	if (sc->sgmapsz == DWLPX_SG128K)
		REGVAL(PCIA_TBIT + sc->sysbase) = 1;

	alpha_mb();

	for (io = 0; io < DWLPX_NIONODE; io++) {
		for (hose = 0; hose < DWLPX_NHOSE; hose++) {
			for (i = 0; i < NHPC; i++) {
				imaskcache[io][hose][i] = DWLPX_IMASK_DFLT;
			}
		}
	}

	/*
	 * Set up DMA stuff here.
	 */

	dwlpx_dma_init(sc);


	/*
	 * Register our interrupt service requirements with out parent.
	 */
	i = BUS_SETUP_INTR(parent, dev, NULL,
		INTR_TYPE_MISC, dwlpx_intr, 0, &intr);
	if (i == 0) {
		bus_generic_attach(dev);
	}
	return (i);
}

static void dwlpx_enadis_intr(int, int, int);

static void
dwlpx_enadis_intr(int vector, int intpin, int onoff)
{
	unsigned long paddr;
	u_int32_t val;
	int device, ionode, hose, hpc, s;

	ionode = DWLPX_MVEC_IONODE(vector);
	hose = DWLPX_MVEC_HOSE(vector);
	device = DWLPX_MVEC_PCISLOT(vector);

	paddr = (1LL << 39);
	paddr |= (unsigned long) ionode << 36;
	paddr |= (unsigned long) hose << 34;
	if (device < 4) {
		hpc = 0;
	} else if (device < 8) {
		hpc = 1;
		device -= 4;
	} else {
		hpc = 2;
		device -= 8;
	}
	intpin <<= (device << 2);
	val = imaskcache[ionode][hose][hpc];
	if (onoff)
		val |= intpin;
	else
		val &= ~intpin;
	imaskcache[ionode][hose][hpc] = val;
	s = splhigh();
	REGVAL(PCIA_IMASK(hpc) + paddr) = val;
	alpha_mb();
	splx(s);
}

static int
dwlpx_setup_intr(device_t dev, device_t child, struct resource *irq, int flags,
       driver_intr_t *intr, void *arg, void **cookiep)
{
	int slot, ionode, hose, error, vector, intpin;
	
	error = rman_activate_resource(irq);
	if (error)
		return error;

	intpin = pci_get_intpin(child);
	slot = pci_get_slot(child);
	hose = pci_get_hose(child);
	ionode = hose >> 2;
	hose &= 0x3;

	vector = DWLPX_MVEC(ionode, hose, slot);
	error = alpha_setup_intr(vector, intr, arg, cookiep,
	    &intrcnt[INTRCNT_KN8AE_IRQ]);
	if (error)
		return error;
	dwlpx_enadis_intr(vector, intpin, 1);
	device_printf(child, "Node %d Hose %d Slot %d interrupting at TLSB "
	    "vector 0x%x intpin %d\n", ionode+4, hose, slot, vector, intpin);
	return (0);
}

static int
dwlpx_teardown_intr(device_t dev, device_t child, struct resource *irq, void *c)
{
	int slot, ionode, hose, vector, intpin;

	intpin = pci_get_intpin(child);
	slot = pci_get_slot(child);
	hose = pci_get_hose(child);
	ionode = hose >> 2;
	hose &= 0x3;
	vector = DWLPX_MVEC(ionode, hose, slot);
	dwlpx_enadis_intr(vector, intpin, 0);
	alpha_teardown_intr(c);
	return rman_deactivate_resource(irq);
}

static void
dwlpx_dma_init(struct dwlpx_softc *sc)
{
	u_int32_t *tbl, sgwmask, sgwbase, sgwend;
	int i, lim;

	/*
	 * Determine size of Window C based on the amount of SGMAP
	 * page table SRAM available.
	 */
	if (sc->sgmapsz == DWLPX_SG128K) {
		lim = 128 * 1024;
		sgwmask = PCIA_WMASK_1G;
		sgwbase = 1UL*1024UL*1024UL*1024UL;
	} else {
		lim = 32 * 1024;
		sgwmask = PCIA_WMASK_256M;
		sgwbase = 1UL*1024UL*1024UL*1024UL+3UL*256UL*1024UL*1024UL;
	}
	sgwend = sgwbase + (lim * 8192) - 1;

	/*
	 * A few notes about SGMAP-mapped DMA on the DWLPx:
	 *
	 * The DWLPx has PCIA-resident SRAM that is used for
	 * the SGMAP page table; there is no TLB.  The DWLPA
	 * has room for 32K entries, yielding a total of 256M
	 * of sgva space.  The DWLPB has 32K entries or 128K
	 * entries, depending on TBIT, yielding either 256M or
	 * 1G of sgva space.
	 */

	/*
	 * Initialize the page table.
	 */
	tbl = (u_int32_t *) ALPHA_PHYS_TO_K0SEG(PCIA_SGMAP_PT + sc->sysbase);
	for (i = 0; i < lim; i++)
		tbl[i] = 0;

#if	0
	/* XXX NOT DONE YET XXX */
	/*
	 * Initialize the SGMAP for window C:
	 *
	 *	Size: 256M or 1GB
	 *	Window base: 1GB
	 *	SGVA base: 0
	 */
	chipset.sgmap = sgmap_map_create(sgwbase, sgwend, dwlpx_sgmap_map, tbl);
#endif

	/*
	 * Set up DMA windows for this DWLPx.
	 */
	for (i = 0; i < sc->nhpc; i++) {
		REGVAL(PCIA_WMASK_A(i) + sc->sysbase) =
		    DWLPx_DIRECT_MAPPED_WMASK;
		REGVAL(PCIA_TBASE_A(i) + sc->sysbase) = 0;
		REGVAL(PCIA_WBASE_A(i) + sc->sysbase) =
		    DWLPx_DIRECT_MAPPED_BASE | PCIA_WBASE_W_EN;

		REGVAL(PCIA_WMASK_B(i) + sc->sysbase) = 0;
		REGVAL(PCIA_TBASE_B(i) + sc->sysbase) = 0;
		REGVAL(PCIA_WBASE_B(i) + sc->sysbase) = 0;

		REGVAL(PCIA_WMASK_C(i) + sc->sysbase) = sgwmask;
		REGVAL(PCIA_TBASE_C(i) + sc->sysbase) = 0;
		REGVAL(PCIA_WBASE_C(i) + sc->sysbase) =
		    sgwbase | PCIA_WBASE_W_EN | PCIA_WBASE_SG_EN;
	}
	alpha_mb();

	/* XXX XXX BEGIN XXX XXX */
	{							/* XXX */
		alpha_XXX_dmamap_or = DWLPx_DIRECT_MAPPED_BASE;	/* XXX */
	}							/* XXX */
	/* XXX XXX END XXX XXX */
}

/*
 */

static void
dwlpx_intr(void *arg)
{
#ifdef SIMOS
	extern void simos_intr(int);
	simos_intr(0);
#else
	unsigned long vec = (unsigned long) arg;
	if ((vec & DWLPX_VEC_EMARK) != 0) {
		dwlpx_eintr(vec);
		return;
	}
	if ((vec & DWLPX_VEC_MARK) == 0) {
		panic("dwlpx_intr: bad vector %p", arg);
		/* NOTREACHED */
	}
	alpha_dispatch_intr(NULL, vec);
#endif
}

static void
dwlpx_eintr(unsigned long vec)
{
	device_t dev;
	struct dwlpx_softc *sc;
	int ionode, hosenum, i;
	struct {
		u_int32_t err;
		u_int32_t addr;
	} hpcs[NHPC];

	ionode = (vec >> 8) & 0xf;
	hosenum = (vec >> 4) & 0x7;
	if (ionode >= DWLPX_NIONODE || hosenum >= DWLPX_NHOSE) {
		panic("dwlpx_iointr: mangled vector 0x%lx", vec);
		/* NOTREACHED */
	}
	dev = dwlpxs[ionode][hosenum];
	sc = DWLPX_SOFTC(dev);
	for (i = 0; i < sc->nhpc; i++) {
		hpcs[i].err = REGVAL(PCIA_ERR(i) + sc->sysbase);
		hpcs[i].addr = REGVAL(PCIA_FADR(i) + sc->sysbase);
	}
	printf("%s: node %d hose %d error interrupt\n",
	   device_get_nameunit(dev), ionode + 4, hosenum);
	
	for (i = 0; i < sc->nhpc; i++) {
		if ((hpcs[i].err & PCIA_ERR_ERROR) == 0)
			continue;
		printf("\tHPC %d: ERR=0x%08x; DMA %s Memory, "
			"Failing Address 0x%x\n",
			i, hpcs[i].err, hpcs[i].addr & 0x1? "write to" :
			"read from", hpcs[i].addr & ~3);
		if (hpcs[i].err & PCIA_ERR_SERR_L)
			printf("\t       PCI device asserted SERR_L\n");
		if (hpcs[i].err & PCIA_ERR_ILAT)
			printf("\t       Incremental Latency Exceeded\n");
		if (hpcs[i].err & PCIA_ERR_SGPRTY)
			printf("\t       CPU access of SG RAM Parity Error\n");
		if (hpcs[i].err & PCIA_ERR_ILLCSR)
			printf("\t       Illegal CSR Address Error\n");
		if (hpcs[i].err & PCIA_ERR_PCINXM)
			printf("\t       Nonexistent PCI Address Error\n");
		if (hpcs[i].err & PCIA_ERR_DSCERR)
			printf("\t       PCI Target Disconnect Error\n");
		if (hpcs[i].err & PCIA_ERR_ABRT)
			printf("\t       PCI Target Abort Error\n");
		if (hpcs[i].err & PCIA_ERR_WPRTY)
			printf("\t       PCI Write Parity Error\n");
		if (hpcs[i].err & PCIA_ERR_DPERR)
			printf("\t       PCI Data Parity Error\n");
		if (hpcs[i].err & PCIA_ERR_APERR)
			printf("\t       PCI Address Parity Error\n");
		if (hpcs[i].err & PCIA_ERR_DFLT)
			printf("\t       SG Map RAM Invalid Entry Error\n");
		if (hpcs[i].err & PCIA_ERR_DPRTY)
			printf("\t       DMA access of SG RAM Parity Error\n");
		if (hpcs[i].err & PCIA_ERR_DRPERR)
			printf("\t       DMA Read Return Parity Error\n");
		if (hpcs[i].err & PCIA_ERR_MABRT)
			printf("\t       PCI Master Abort Error\n");
		if (hpcs[i].err & PCIA_ERR_CPRTY)
			printf("\t       CSR Parity Error\n");
		if (hpcs[i].err & PCIA_ERR_COVR)
			printf("\t       CSR Overrun Error\n");
		if (hpcs[i].err & PCIA_ERR_MBPERR)
			printf("\t       Mailbox Parity Error\n");
		if (hpcs[i].err & PCIA_ERR_MBILI)
			printf("\t       Mailbox Illegal Length Error\n");
		REGVAL(PCIA_ERR(i) + sc->sysbase) = hpcs[i].err;
	}
}

DRIVER_MODULE(dwlpx, kft, dwlpx_driver, dwlpx_devclass, 0, 0);
