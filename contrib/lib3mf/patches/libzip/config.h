#ifndef HAD_CONFIG_H
#define HAD_CONFIG_H
#ifndef _HAD_ZIPCONF_H
#include "zipconf.h"
#endif

/* Pre-configured for lib3mf integration into assimp.
   Supports POSIX, macOS, and Emscripten platforms. */

/* POSIX/Emscripten features */
#define HAVE_FILENO
#define HAVE_FSEEKO
#define HAVE_FTELLO
#define HAVE_SNPRINTF
#define HAVE_STRCASECMP
#define HAVE_STRDUP
#define HAVE_STRTOLL
#define HAVE_STRTOULL
#define HAVE_STDBOOL_H
#define HAVE_STRINGS_H
#define HAVE_UNISTD_H
#define HAVE_LOCALTIME_R
#define HAVE_DIRENT_H

#ifndef __EMSCRIPTEN__
#define HAVE_MKSTEMP
#define HAVE_FCHMOD
#endif

#ifdef __APPLE__
#define HAVE_ARC4RANDOM
#define HAVE_CLONEFILE
#endif

#define SIZEOF_OFF_T 8
#ifdef __EMSCRIPTEN__
#define SIZEOF_SIZE_T 4
#else
#define SIZEOF_SIZE_T 8
#endif

#define PACKAGE "libzip"
#define VERSION "1.9.2"

#endif /* HAD_CONFIG_H */
