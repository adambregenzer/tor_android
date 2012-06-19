LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE:= libor
LOCAL_MODULE_TAGS:= optional

LOCAL_SRC_FILES :=                   \
    address.c                        \
    compat.c                         \
    container.c                      \
    di_ops.c                         \
    log.c                            \
    memarea.c                        \
    mempool.c                        \
    procmon.c                        \
    util.c                           \
    util_codedigest.c

util_codedigest.o: common_sha1.i

LOCAL_C_INCLUDES += $(TOR_LOCAL_PATH)
LOCAL_C_INCLUDES += external/libevent/include/

LOCAL_CFLAGS += -UNDEBUG
LOCAL_CFLAGS += -DHAVE_CONFIG_H

include $(BUILD_STATIC_LIBRARY)




include $(CLEAR_VARS)

LOCAL_MODULE:= libor-crypto
LOCAL_MODULE_TAGS:= optional

LOCAL_SRC_FILES :=                   \
    aes.c                            \
    crypto.c                         \
    torgzip.c                        \
    tortls.c

crypto.o: sha256.c

LOCAL_C_INCLUDES += external/zlib
LOCAL_C_INCLUDES += external/libevent/include
LOCAL_C_INCLUDES += external/openssl/include
LOCAL_C_INCLUDES += $(TOR_LOCAL_PATH)

LOCAL_CFLAGS += -UNDEBUG
LOCAL_CFLAGS += -DHAVE_CONFIG_H

include $(BUILD_STATIC_LIBRARY)




include $(CLEAR_VARS)

LOCAL_MODULE:= libor-event
LOCAL_MODULE_TAGS:= optional

LOCAL_SRC_FILES := compat_libevent.c

LOCAL_C_INCLUDES += $(TOR_LOCAL_PATH)
LOCAL_C_INCLUDES += external/libevent/include/

LOCAL_CFLAGS += -UNDEBUG
LOCAL_CFLAGS += -DHAVE_CONFIG_H

include $(BUILD_STATIC_LIBRARY)




#common_sha1.i: $(libor_SOURCES) $(libor_crypto_a_SOURCES) $(noinst_HEADERS)
#	if test "@SHA1SUM@" != none; then \
#	  (cd "$(srcdir)" && "@SHA1SUM@" $(libor_SOURCES) $(libor_crypto_a_SOURCES) $(noinst_HEADERS)) | "@SED@" -n 's/^\(.*\)$$/"\1\\n"/p' > common_sha1.i; \
#	elif test "@OPENSSL@" != none; then \
#	  (cd "$(srcdir)" && "@OPENSSL@" sha1 $(libor_SOURCES) $(libor_crypto_a_SOURCES) $(noinst_HEADERS)) | "@SED@" -n 's/SHA1(\(.*\))= \(.*\)/"\2  \1\\n"/p' > common_sha1.i; \
#	else \
#	  rm common_sha1.i; \
#	  touch common_sha1.i; \
#	fi

