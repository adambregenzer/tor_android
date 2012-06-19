LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE:= libtor
LOCAL_MODULE_TAGS:= optional

LOCAL_SRC_FILES :=                      \
    buffers.c				\
    circuitbuild.c			\
    circuitlist.c			\
    circuituse.c			\
    command.c				\
    config.c				\
    connection.c			\
    connection_edge.c			\
    connection_or.c			\
    control.c				\
    cpuworker.c				\
    directory.c				\
    dirserv.c				\
    dirvote.c				\
    dns.c				\
    dnsserv.c				\
    geoip.c				\
    hibernate.c				\
    main.c				\
    microdesc.c				\
    networkstatus.c			\
    nodelist.c				\
    onion.c				\
    transports.c			\
    policies.c				\
    reasons.c				\
    relay.c				\
    rendclient.c			\
    rendcommon.c			\
    rendmid.c				\
    rendservice.c			\
    rephist.c				\
    router.c				\
    routerlist.c			\
    routerparse.c			\
    status.c				\
    config_codedigest.c

config_codedigest.o: or_sha1.i

LOCAL_C_INCLUDES += $(TOR_LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/../common/
LOCAL_C_INCLUDES += external/libevent/include/
LOCAL_C_INCLUDES += external/openssl/include/

LOCAL_CFLAGS += -UNDEBUG
LOCAL_CFLAGS += -DHAVE_CONFIG_H
LOCAL_CFLAGS += -DSHARE_DATADIR="\"/usr/share\""
LOCAL_CFLAGS += -DLOCALSTATEDIR="\"/usr/var\""

include $(BUILD_STATIC_LIBRARY)




include $(CLEAR_VARS)

LOCAL_MODULE := tor
LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := tor_main.c

tor_main.o: micro-revision.i

LOCAL_STATIC_LIBRARIES := libtor libor libor-crypto libor-event libevent_full libssl_static libcrypto_static libz libm

LOCAL_C_INCLUDES += $(TOR_LOCAL_PATH)
LOCAL_C_INCLUDES += $(TOR_LOCAL_PATH)/src/common/
LOCAL_C_INCLUDES += external/libevent/include/

LOCAL_CFLAGS += -DHAVE_CONFIG_H

include $(BUILD_EXECUTABLE)




#micro-revision.i: FORCE
#	@rm -f micro-revision.tmp;					\
#	if test -d "$(top_srcdir)/.git" &&				\
#	  test -x "`which git 2>&1;true`"; then				\
#	  HASH="`cd "$(top_srcdir)" && git rev-parse --short=16 HEAD`"; 	\
#	  echo \"$$HASH\" > micro-revision.tmp; 			\
#        fi;								\
#	if test ! -f micro-revision.tmp ; then				\
#	  if test ! -f micro-revision.i ; then				\
#	    echo '""' > micro-revision.i;				\
#	  fi;								\
#	elif test ! -f micro-revision.i ||				\
#	  test x"`cat micro-revision.tmp`" != x"`cat micro-revision.i`"; then \
#	  mv micro-revision.tmp micro-revision.i;			\
#	fi; true
#
#or_sha1.i: $(tor_SOURCES) $(libtor_a_SOURCES)
#	if test "@SHA1SUM@" != none; then \
#	  (cd "$(srcdir)" && "@SHA1SUM@" $(tor_SOURCES) $(libtor_a_SOURCES)) | \
#	  "@SED@" -n 's/^\(.*\)$$/"\1\\n"/p' > or_sha1.i; \
#	elif test "@OPENSSL@" != none; then \
#	  (cd "$(srcdir)" && "@OPENSSL@" sha1 $(tor_SOURCES) $(libtor_a_SOURCES)) | \
#	  "@SED@" -n 's/SHA1(\(.*\))= \(.*\)/"\2  \1\\n"/p' > or_sha1.i; \
#	else \
#	  rm or_sha1.i; \
#	  touch or_sha1.i; \
#	fi

