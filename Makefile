PROG=	slaballoc
SRCS=	alloc.c slabtest.c
NOMAN=	#

CFLAGS+=	-g -Wall

.include <bsd.prog.mk>
