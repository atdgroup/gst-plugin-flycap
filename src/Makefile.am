# Note: plugindir is set in configure

plugin_LTLIBRARIES = libflycapplugin.la

# Path to installation of the flycap SDK 
FLYCAP_CFLAGS = -I/usr/include/flycapture/C
FLYCAP_LIBS = -lflycapture-c -lflycapture -L/usr/lib

# sources used to compile this plug-in
libflycapplugin_la_SOURCES = gstflycapsrc.c gstflycapsrc.h gstplugin.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libflycapplugin_la_CFLAGS = $(GST_CFLAGS) $(FLYCAP_CFLAGS)
libflycapplugin_la_LIBADD = $(GST_LIBS) $(FLYCAP_LIBS) -lgstvideo-1.0
libflycapplugin_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libflycapplugin_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstflycapsrc.h
