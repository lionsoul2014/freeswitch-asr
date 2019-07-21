include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_yyasr

if HAVE_YYASR

mod_LTLIBRARIES = mod_yyasr.la
mod_yyasr_la_SOURCES  = mod_yyasr.c
mod_yyasr_la_CFLAGS   = $(AM_CFLAGS)
mod_yyasr_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_yyasr_la_LDFLAGS  = -avoid-version -module -no-undefined -shared

else
install: error
all: error
error:
	$(error You must install libcurl to build mod_yyasr)
endif
