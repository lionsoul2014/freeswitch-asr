include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_yytts

if HAVE_YYTTS

mod_LTLIBRARIES = mod_yytts.la
mod_yytts_la_SOURCES  = mod_yytts.c
mod_yytts_la_CFLAGS   = $(AM_CFLAGS)
mod_yytts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_yytts_la_LDFLAGS  = -avoid-version -module -no-undefined -shared

else
install: error
all: error
error:
	$(error You must install libcurl to build mod_yytts)
endif
