#include <u.h>
#include <libc.h>
#include <bio.h>
#include "snap.h"

static Proc*
findpid(Proc *plist, long pid)
{
	while(plist) {
		if(plist->pid == pid)
			break;
		plist = plist->link;
	}
	return plist;
}

Page*
findpage(Proc *plist, long pid, int type, uvlong off)
{
	Seg *s;
	int i;

	plist = findpid(plist, pid);
	if(plist == nil)
		sysfatal("can't find referenced pid");

	if(type == 't') {
		if(off%Pagesize)
			sysfatal("bad text offset alignment");
		s = plist->text;
		if(off >= s->len)
			return nil;
		return s->pg[off/Pagesize];
	}

	s = nil;
	for(i=0; i<plist->nseg; i++) {
		s = plist->seg[i];
		if(s && s->offset <= off && off < s->offset+s->len)
			break;
		s = nil;
	}
	if(s == nil)
		return nil;

	off -= s->offset;
	if(off%Pagesize)
		sysfatal("bad mem offset alignment");

	return s->pg[off/Pagesize];
}

static int
Breadnumber(Biobuf *b, char *buf)
{
	int i;
	int c;
	int havedigits;
	
	havedigits = 0;
	for(i=0; i<22; i++){
		if((c = Bgetc(b)) == Beof)
			return -1;
		if('0' <= c && c <= '9'){
			*buf++ = c;
			havedigits = 1;
		}else if(c == ' '){
			if(havedigits){
				while((c = Bgetc(b)) == ' ')
					;
				if(c != Beof)
					Bungetc(b);
				break;
			}
		}else{
			werrstr("bad character %.2ux", c);
			return -1;
		}
	}
	*buf = 0;
	return 0;
}

static int
Breadulong(Biobuf *b, ulong *x)
{
	char buf[32];
	
	if(Breadnumber(b, buf) < 0)
		return -1;
	*x = strtoul(buf, 0, 0);
	return 0;
}

static int
Breaduvlong(Biobuf *b, uvlong *x)
{
	char buf[32];
	
	if(Breadnumber(b, buf) < 0)
		return -1;
	*x = strtoull(buf, 0, 0);
	return 0;
}

static Data*
readdata(Biobuf *b)
{
	Data *d;
	char str[32];
	ulong len;

	if(Bread(b, str, 12) != 12)
		sysfatal("can't read data hdr: %r");
	str[12] = 0;
	len = strtoul(str, 0, 0);
	if(len + sizeof(*d) < sizeof(*d))
		sysfatal("data len too large");
	d = emalloc(sizeof(*d) + len);
	if(len && Bread(b, d->data, len) != len)
		sysfatal("can't read data body");
	d->len = len;
	return d;
}

static Seg*
readseg(Seg **ps, Biobuf *b, Proc *plist, char *name)
{
	Seg *s;
	Page **pp;
	int t;
	int n, len;
	ulong i, npg;
	ulong pid;
	uvlong off;
	char buf[Pagesize];
	extern char zeros[];

	s = emalloc(sizeof *s);
	if(Breaduvlong(b, &s->offset) < 0
	|| Breaduvlong(b, &s->len) < 0)
		sysfatal("error reading segment: %r");

	if(debug)
		fprint(2, "readseg %.8llux - %.8llux %s\n", s->offset, s->offset + s->len, name);

	npg = (s->len + Pagesize-1)/Pagesize;
	s->npg = npg;

	if(s->npg == 0)
		return s;

	pp = emalloc(sizeof(*pp)*npg);
	s->pg = pp;
	*ps = s;

	len = Pagesize;
	for(i=0; i<npg; i++) {
		if(i == npg-1)
			len = s->len - (uvlong)i*Pagesize;

		switch(t = Bgetc(b)) {
		case 'z':
			pp[i] = datapage(zeros, len);
			if(debug > 1)
				fprint(2, "0x%.8llux all zeros\n", s->offset+(uvlong)i*Pagesize);
			break;
		case 'm':
		case 't':
			if(Breadulong(b, &pid) < 0 
			|| Breaduvlong(b, &off) < 0)
				sysfatal("error reading segment x: %r");
			pp[i] = findpage(plist, pid, t, off);
			if(pp[i] == nil)
				sysfatal("bad page reference in snapshot");
			if(debug > 1)
				fprint(2, "0x%.8llux same as %s pid %lud 0x%.8llux\n",
					s->offset+(uvlong)i*Pagesize, t=='m'?"mem":"text", pid, off);
			break;
		case 'r':
			if((n=Bread(b, buf, len)) != len)
				sysfatal("short read of segment %d/%d at %llx: %r", n, len, Boffset(b));
			pp[i] = datapage(buf, len);
			if(debug > 1)
				fprint(2, "0x%.8llux is raw data\n", s->offset+(uvlong)i*Pagesize);
			break;
		default:
			fprint(2, "bad type char %#.2ux\n", t);
			sysfatal("error reading segment");
		}
	}
	return s;
}

Proc*
readsnap(Biobuf *b)
{
	char *q;
	char buf[12];
	long pid;
	Proc *p, *plist;
	int i, n;

	if((q = Brdline(b, '\n')) == nil)
		sysfatal("error reading snapshot file");
	if(strncmp(q, "process snapshot", strlen("process snapshot")) != 0)
		sysfatal("bad snapshot file format");

	plist = nil;
	while(q = Brdline(b, '\n')) {
		q[Blinelen(b)-1] = 0;
		pid = atol(q);
		q += 12;
		p = findpid(plist, pid);
		if(p == nil) {
			p = emalloc(sizeof(*p));
			p->link = plist;
			p->pid = pid;
			plist = p;
		}

		for(i=0; i<Npfile; i++) {
			if(strcmp(pfile[i], q) == 0) {
				p->d[i] = readdata(b);
				break;
			}
		}
		if(i != Npfile)
			continue;
		if(strcmp(q, "mem") == 0) {
			if(Bread(b, buf, 12) != 12) 
				sysfatal("can't read memory section: %r");
			buf[12] = 0;
			n = atoi(buf);
			if(n <= 0 || n > 16)
				sysfatal("bad segment count: %d", n);
			p->nseg = n;
			p->seg = emalloc(n*sizeof(*p->seg));
			for(i=0; i<n; i++){
				snprint(buf, sizeof(buf), "[%d]", i);
				readseg(&p->seg[i], b, plist, buf);
			}
		} else if(strcmp(q, "text") == 0) {
			readseg(&p->text, b, plist, q);
		} else
			sysfatal("unknown section");
	}
	return plist;
}
