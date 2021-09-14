#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([overwriter], [0.0.1], [katsumata-m@t-axis.co.jp])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_SRCDIR([src/overwriter.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h inttypes.h limits.h stdlib.h string.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_CHECK_MEMBERS([struct stat.st_blksize])

# Checks for library functions.
AC_FUNC_FORK
AC_CHECK_FUNCS([dup2 memmove select])

AC_CONFIG_FILES([Makefile src/Makefile])

AC_OUTPUT
