
LIB_VERSION = 0.0.5
LD_VERSION = 0:5:0

AM_CFLAGS = -Wall -Wsign-compare -I$(top_builddir)/include \
	-I$(top_srcdir)/include \
	-DIPMI_CHECK_LOCKS $(GLIB_CFLAGS) \
	$(TCL_CFLAGS) -DBUILDING_IPMI_CMDLANG_DLL

lib_LTLIBRARIES = libOpenIPMIcmdlang.la

libOpenIPMIcmdlang_la_SOURCES = cmdlang.c cmd_domain.c cmd_entity.c cmd_mc.c \
	cmd_pet.c cmd_sensor.c cmd_control.c cmd_sel.c cmd_lanparm.c \
	cmd_pef.c cmd_conn.c cmd_fru.c out_fru.c cmd_solparm.c
libOpenIPMIcmdlang_la_LIBADD = -lm \
	$(top_builddir)/utils/libOpenIPMIutils.la \
	$(top_builddir)/lib/libOpenIPMI.la
libOpenIPMIcmdlang_la_LDFLAGS = -rdynamic -version-info $(LD_VERSION) \
	-L$(libdir)

bin_PROGRAMS = openipmish

openipmish_SOURCES = ipmish.c os_debug.c
openipmish_LDADD =  libOpenIPMIcmdlang.la \
		$(top_builddir)/utils/libOpenIPMIutils.la \
		$(top_builddir)/lib/libOpenIPMI.la \
		$(top_builddir)/unix/libOpenIPMIposix.la \
		-lreadline \
		$(TERM_LIBS) \
		$(SNMPLIBS) $(GLIB_LIB) $(GLIB_LIBS) \
		$(TCL_LIB) $(TCL_LIBS) \
		$(OPENSSLLIBS) $(GDBM_LIB) $(RT_LIB)

# We need to make a link from ipmish to openipmish for backwards
# compatability.
install-data-local:
	rm -f $(DESTDIR)$(bindir)/ipmish
	$(LN_S) openipmish $(DESTDIR)$(bindir)/ipmish

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/ipmish
