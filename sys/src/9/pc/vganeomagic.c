#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/pci.h"
#include "../port/error.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

typedef struct CursorNM CursorNM;
struct CursorNM {
	int	enable;
	int	x;
	int	y;
	int	colour1;
	int	colour2;
	int	addr;
};

static void
neomagicenable(VGAscr* scr)
{
	Pcidev *p;
	int bar, curoff, vmsize;
	uvlong ioaddr;
	vlong iosize;

	/*
	 * scr->mmio holds the virtual address of the cursor registers
	 * in the MMIO space. This may need to change for older chips
	 * which have the MMIO space offset in the framebuffer region.
	 *
	 * scr->io holds the offset into mmio of the CursorNM struct.
	 */
	if(scr->mmio)
		return;
	p = scr->pci;
	if(p == nil || p->vid != 0x10C8)
		return;
	bar = 1;
	switch(p->did){
	case 0x0003:		/* MagicGraph 128ZV */
		bar = 0;
		if(p->mem[bar].bar & 1)
			return;
		ioaddr = (p->mem[bar].bar & ~0x0F) + 0x200000;
		iosize = 0x200000;
		curoff = 0x100;
		vmsize = 1152*1024;
		goto Map;
	case 0x0083:		/* MagicGraph 128ZV+ */
		curoff = 0x100;
		vmsize = 1152*1024;
		break;
	case 0x0004:		/* MagicGraph 128XD */
		curoff = 0x100;
		vmsize = 2048*1024;
		break;
	case 0x0005:		/* MagicMedia 256AV */
		curoff = 0x1000;
		vmsize = 2560*1024;
		break;
	case 0x0006:		/* MagicMedia 256ZX */
		curoff = 0x1000;
		vmsize = 4096*1024;
		break;
	case 0x0016:		/* MagicMedia 256XL+ */
		curoff = 0x1000;
		/* Vaio VESA BIOS says 6080, but then hwgc doesn't work */
		vmsize = 4096*1024;
		break;
	default:
		return;
	}
	if(p->mem[bar].bar & 1 || p->mem[bar].size == 0)
		return;
	ioaddr = p->mem[bar].bar & ~0x0F;
	iosize = p->mem[bar].size;
Map:
	scr->mmio = vmap(ioaddr, iosize);
	if(scr->mmio == nil)
		return;
	addvgaseg("neomagicmmio", ioaddr, iosize);

	/*
	 * Find a place for the cursor data in display memory.
	 * 2 cursor images might be needed, 1KB each so use the
	 * last 2KB of the framebuffer.
	 */
	scr->storage = vmsize-2*1024;
	scr->io = curoff;
	vgalinearpci(scr);
	if(scr->paddr)
		addvgaseg("neomagicscreen", scr->paddr, scr->apsize);
}

static void
neomagiccurdisable(VGAscr* scr)
{
	CursorNM *cursornm;

	if(scr->mmio == 0)
		return;
	cursornm = (void*)((char*)scr->mmio + scr->io);
	cursornm->enable = 0;
}

static void
neomagicinitcursor(VGAscr* scr, int xo, int yo, int index)
{
	uchar *p;
	uint p0, p1;
	int x, y;

	p = (uchar*)scr->vaddr;
	p += scr->storage + index*1024;

	for(y = yo; y < 16; y++){
		p0 = scr->set[2*y];
		p1 = scr->set[2*y+1];
		if(xo){
			p0 = (p0<<xo)|(p1>>(8-xo));
			p1 <<= xo;
		}
		*p++ = p0;
		*p++ = p1;

		for(x = 16; x < 64; x += 8)
			*p++ = 0x00;

		p0 = scr->clr[2*y]|scr->set[2*y];
		p1 = scr->clr[2*y+1]|scr->set[2*y+1];
		if(xo){
			p0 = (p0<<xo)|(p1>>(8-xo));
			p1 <<= xo;
		}
		*p++ = p0;
		*p++ = p1;

		for(x = 16; x < 64; x += 8)
			*p++ = 0x00;
	}
	while(y < 64+yo){
		for(x = 0; x < 64; x += 8){
			*p++ = 0x00;
			*p++ = 0x00;
		}
		y++;
	}
}

static void
neomagiccurload(VGAscr* scr, Cursor* curs)
{
	CursorNM *cursornm;

	if(scr->mmio == 0)
		return;
	cursornm = (void*)((char*)scr->mmio + scr->io);

	cursornm->enable = 0;
	memmove(&scr->Cursor, curs, sizeof(Cursor));
	neomagicinitcursor(scr, 0, 0, 0);
	cursornm->enable = 1;
}

static int
neomagiccurmove(VGAscr* scr, Point p)
{
	CursorNM *cursornm;
	int addr, index, x, xo, y, yo;

	if(scr->mmio == 0)
		return 1;
	cursornm = (void*)((char*)scr->mmio + scr->io);

	index = 0;
	if((x = p.x+scr->offset.x) < 0){
		xo = -x;
		x = 0;
	}
	else
		xo = 0;
	if((y = p.y+scr->offset.y) < 0){
		yo = -y;
		y = 0;
	}
	else
		yo = 0;

	if(xo || yo){
		index = 1;
		neomagicinitcursor(scr, xo, yo, index);
	}
	addr = ((scr->storage+(1024*index))>>10) & 0xFFF;
	addr = ((addr & 0x00F)<<8)|((addr>>4) & 0xFF);
	if(cursornm->addr != addr)
		cursornm->addr = addr;

	cursornm->x = x;
	cursornm->y = y;

	return 0;
}

static void
neomagiccurenable(VGAscr* scr)
{
	CursorNM *cursornm;

	neomagicenable(scr);
	if(scr->mmio == 0)
		return;
	cursornm = (void*)((char*)scr->mmio + scr->io);
	cursornm->enable = 0;

	/*
	 * Cursor colours.
	 */
	cursornm->colour1 = (Pblack<<16)|(Pblack<<8)|Pblack;
	cursornm->colour2 = (Pwhite<<16)|(Pwhite<<8)|Pwhite;

	/*
	 * Load, locate and enable the 64x64 cursor.
	 */
	neomagiccurload(scr, &cursor);
	neomagiccurmove(scr, ZP);
	cursornm->enable = 1;
}

static int neomagicbltflags;

/* registers */
enum {
	BltStat = 0,
	BltCntl = 1,
	XPColor = 2,
	FGColor = 3,
	BGColor = 4,
	Pitch = 5,
	ClipLT = 6,
	ClipRB = 7,
	SrcBitOff = 8,
	SrcStartOff = 9,

	DstStartOff = 11,
	XYExt = 12,

	PageCntl = 20,
	PageBase,
	PostBase,
	PostPtr,
	DataPtr,
};

/* flags */
enum {
	NEO_BS0_BLT_BUSY =	0x00000001,
	NEO_BS0_FIFO_AVAIL =	0x00000002,
	NEO_BS0_FIFO_PEND =	0x00000004,

	NEO_BC0_DST_Y_DEC =	0x00000001,
	NEO_BC0_X_DEC =		0x00000002,
	NEO_BC0_SRC_TRANS =	0x00000004,
	NEO_BC0_SRC_IS_FG =	0x00000008,
	NEO_BC0_SRC_Y_DEC =	0x00000010,
	NEO_BC0_FILL_PAT =	0x00000020,
	NEO_BC0_SRC_MONO =	0x00000040,
	NEO_BC0_SYS_TO_VID =	0x00000080,

	NEO_BC1_DEPTH8 =	0x00000100,
	NEO_BC1_DEPTH16 =	0x00000200,
	NEO_BC1_DEPTH24 =	0x00000300,
	NEO_BC1_X_320 =		0x00000400,
	NEO_BC1_X_640 =		0x00000800,
	NEO_BC1_X_800 =		0x00000c00,
	NEO_BC1_X_1024 =	0x00001000,
	NEO_BC1_X_1152 =	0x00001400,
	NEO_BC1_X_1280 =	0x00001800,
	NEO_BC1_X_1600 =	0x00001c00,
	NEO_BC1_DST_TRANS =	0x00002000,
	NEO_BC1_MSTR_BLT =	0x00004000,
	NEO_BC1_FILTER_Z =	0x00008000,

	NEO_BC2_WR_TR_DST =	0x00800000,

	NEO_BC3_SRC_XY_ADDR =	0x01000000,
	NEO_BC3_DST_XY_ADDR =	0x02000000,
	NEO_BC3_CLIP_ON =	0x04000000,
	NEO_BC3_FIFO_EN =	0x08000000,
	NEO_BC3_BLT_ON_ADDR =	0x10000000,
	NEO_BC3_SKIP_MAPPING =	0x80000000,

	NEO_MODE1_DEPTH8 =	0x0100,
	NEO_MODE1_DEPTH16 =	0x0200,
	NEO_MODE1_DEPTH24 =	0x0300,
	NEO_MODE1_X_320 =	0x0400,
	NEO_MODE1_X_640 =	0x0800,
	NEO_MODE1_X_800 =	0x0c00,
	NEO_MODE1_X_1024 =	0x1000,
	NEO_MODE1_X_1152 =	0x1400,
	NEO_MODE1_X_1280 =	0x1800,
	NEO_MODE1_X_1600 =	0x1c00,
	NEO_MODE1_BLT_ON_ADDR =	0x2000,
};

/* Raster Operations */
enum {
	GXclear =		0x000000,	/* 0x0000 */
	GXand =			0x080000,	/* 0x1000 */
	GXandReverse =		0x040000,	/* 0x0100 */
	GXcopy =		0x0c0000,	/* 0x1100 */
	GXandInvert =		0x020000,	/* 0x0010 */
	GXnoop =		0x0a0000,	/* 0x1010 */
	GXxor =			0x060000,	/* 0x0110 */
	GXor =			0x0e0000,	/* 0x1110 */
	GXnor =			0x010000,	/* 0x0001 */
	GXequiv =		0x090000,	/* 0x1001 */
	GXinvert =		0x050000,	/* 0x0101 */
	GXorReverse =		0x0d0000,	/* 0x1101 */
	GXcopyInvert =		0x030000,	/* 0x0011 */
	GXorInverted =		0x0b0000,	/* 0x1011 */
	GXnand =		0x070000,	/* 0x0111 */
	GXset =			0x0f0000,	/* 0x1111 */
};

static void
waitforidle(VGAscr *scr)
{
	ulong *mmio;
	long x;

	mmio = scr->mmio;
	x = 0;
	while((mmio[BltStat] & NEO_BS0_BLT_BUSY) && x++ < 1000000)
		;
	//if(x >= 1000000)
	//	iprint("idle stat %lud scrmmio %#.8lux scr %#p pc %#p\n", mmio[BltStat], scr->mmio, scr, getcallerpc(&scr));
}

static void
waitforfifo(VGAscr *scr, int entries)
{
	ulong *mmio;
	long x;

	mmio = scr->mmio;
	x = 0;
	while(((mmio[BltStat]>>8) < entries) && x++ < 1000000)
		;
	//if(x >= 1000000)
	//	iprint("fifo stat %d scrmmio %#.8lux scr %#p pc %#p\n", mmio[BltStat]>>8, scr->mmio, scr, getcallerpc(&scr));
	/* DirectFB says the above doesn't work.  if so... */
	/* waitforidle(scr); */
}

static int
neomagichwfill(VGAscr *scr, Rectangle r, ulong sval)
{
	ulong *mmio;

	mmio = scr->mmio;

	waitforfifo(scr, 1);
	mmio[FGColor] = sval;
	waitforfifo(scr, 3);
	mmio[BltCntl] = neomagicbltflags
		| NEO_BC3_FIFO_EN
		| NEO_BC0_SRC_IS_FG
		| NEO_BC3_SKIP_MAPPING
		| GXcopy;
	mmio[DstStartOff] = scr->paddr
		+ r.min.y*scr->pitch
		+ r.min.x*scr->bpp;
	mmio[XYExt] = (Dy(r) << 16) | (Dx(r) & 0xffff);
	waitforidle(scr);
	return 1;
}

static int
neomagichwscroll(VGAscr *scr, Rectangle r, Rectangle sr)
{
	ulong *mmio;

	mmio = scr->mmio;

	waitforfifo(scr, 4);
	if (r.min.y < sr.min.y || (r.min.y == sr.min.y && r.min.x < sr.min.x)) {
		/* start from upper-left */
		mmio[BltCntl] = neomagicbltflags
			| NEO_BC3_FIFO_EN
			| NEO_BC3_SKIP_MAPPING
			| GXcopy;
		mmio[SrcStartOff] = scr->paddr
			+ sr.min.y*scr->pitch + sr.min.x*scr->bpp;
		mmio[DstStartOff] = scr->paddr
			+ r.min.y*scr->pitch + r.min.x*scr->bpp;
	} else {
		/* start from lower-right */
		mmio[BltCntl] = neomagicbltflags
			| NEO_BC0_X_DEC
			| NEO_BC0_DST_Y_DEC
			| NEO_BC0_SRC_Y_DEC
			| NEO_BC3_FIFO_EN
			| NEO_BC3_SKIP_MAPPING
			| GXcopy;
		mmio[SrcStartOff] = scr->paddr
			+ (sr.max.y-1)*scr->pitch + (sr.max.x-1)*scr->bpp;
		mmio[DstStartOff] = scr->paddr
			+ (r.max.y-1)*scr->pitch + (r.max.x-1)*scr->bpp;
	}
	mmio[XYExt] = (Dy(r) << 16) | (Dx(r) & 0xffff);
	waitforidle(scr);
	return 1;
}

static void
neomagicdrawinit(VGAscr *scr)
{
	ulong *mmio;
	uint bltmode;

	mmio = scr->mmio;

	neomagicbltflags = bltmode = 0;

	switch(scr->gscreen->depth) {
	case 8:
		bltmode |= NEO_MODE1_DEPTH8;
		neomagicbltflags |= NEO_BC1_DEPTH8;
		break;
	case 16:
		bltmode |= NEO_MODE1_DEPTH16;
		neomagicbltflags |= NEO_BC1_DEPTH16;
		break;
	case 24:	/* I can't get it to work, and XFree86 doesn't either. */
	default:	/* give up */
		return;
	}

	switch(scr->width) {
	case 320:
		bltmode |= NEO_MODE1_X_320;
		neomagicbltflags |= NEO_BC1_X_320;
		break;
	case 640:
		bltmode |= NEO_MODE1_X_640;
		neomagicbltflags |= NEO_BC1_X_640;
		break;
	case 800:
		bltmode |= NEO_MODE1_X_800;
		neomagicbltflags |= NEO_BC1_X_800;
		break;
	case 1024:
		bltmode |= NEO_MODE1_X_1024;
		neomagicbltflags |= NEO_BC1_X_1024;
		break;
	case 1152:
		bltmode |= NEO_MODE1_X_1152;
		neomagicbltflags |= NEO_BC1_X_1152;
		break;
	case 1280:
		bltmode |= NEO_MODE1_X_1280;
		neomagicbltflags |= NEO_BC1_X_1280;
		break;
	case 1600:
		bltmode |= NEO_MODE1_X_1600;
		neomagicbltflags |= NEO_BC1_X_1600;
		break;
	default:
		/* don't worry about it */
		break;
	}

	waitforidle(scr);
	mmio[BltStat] = bltmode << 16;
	mmio[Pitch] = (scr->pitch << 16) | (scr->pitch & 0xffff);

	scr->fill = neomagichwfill;
	scr->scroll = neomagichwscroll;
}

VGAdev vganeomagicdev = {
	"neomagic",

	neomagicenable,
	nil,
	nil,
	nil,
	neomagicdrawinit,
};

VGAcur vganeomagiccur = {
	"neomagichwgc",

	neomagiccurenable,
	neomagiccurdisable,
	neomagiccurload,
	neomagiccurmove,
};

