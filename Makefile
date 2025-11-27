# NOTE: Must make cleandepend when changing SRCTOP between builds.
SRCTOP?= /usr/src
.export SRCTOP

SHLIB_NAME=	ck.so
SHLIBDIR=	${LIBDIR}/flua

SRCS+=		lua_ck.c \
		ec.c \
		fifo.c \
		pr.c \
		ring.c \
		sequence.c \
		serde.c \
		serdebuf.c \
		shared.c \

CFLAGS+= \
	-I${SRCTOP}/contrib/lua/src \
	-I${SRCTOP}/lib/liblua \
	-I/usr/local/include \

LDADD+=	-L/usr/local/lib -lck

MAN=	ck.3lua \
	ck.ec.3lua \
	ck.fifo.3lua \
	ck.pr.3lua \
	ck.ring.3lua \
	ck.sequence.3lua \
	ck.shared.3lua \
	ck.shared.pr.3lua \
	ck.shared.pr.md128.3lua \

.include <bsd.lib.mk>
