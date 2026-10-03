# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

AC_INIT([sigmoid-neuron-translator], [0.1.0], [claire@gnu-ai.org])
AC_CONFIG_SRCDIR([src/neuron.c])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

AC_CANONICAL_HOST

# System extensions (POSIX/XSI): defines _GNU_SOURCE etc. in config.h.
# Required by the POSIX.1-2008 interfaces of the compute core.  This
# macro must run before anything that compiles.
AC_USE_SYSTEM_EXTENSIONS

AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB

# Le projet est écrit en C23 ; certains compilateurs nomment encore ce
# mode « c2x ».  Exiger l'un des deux (même politique que
# httpfs-translator, pour une pile homogène).
AC_CACHE_CHECK([whether the C compiler accepts C23], [snt_cv_c23], [
  snt_cv_c23=no
  for snt_flag in -std=c23 -std=c2x; do
    snt_save_CFLAGS=$CFLAGS
    CFLAGS="$CFLAGS $snt_flag"
    AC_COMPILE_IFELSE(
      [AC_LANG_PROGRAM([[
        constexpr int snt_c23_test = 1;
        static void *snt_c23_p = nullptr;]],
        [[return snt_c23_p == nullptr ? snt_c23_test : 0;]])],
      [snt_cv_c23=$snt_flag])
    CFLAGS=$snt_save_CFLAGS
    test "x$snt_cv_c23" != xno && break
  done])
AS_IF([test "x$snt_cv_c23" = xno],
      [AC_MSG_ERROR([a C23 compiler is required (GCC 13 or newer)])])
AC_SUBST([C23_CFLAGS], [$snt_cv_c23])

AC_CONFIG_HEADERS([config.h])

# --- GNU/Hurd ou non ---------------------------------------------------
# Le cœur de calcul (neuron.c) est du C23/POSIX pur et se compile
# partout ; seules la boucle serveur trivfs et l'édition de liens
# des bibliothèques Hurd sont propres à GNU/Hurd.  Le système
# hôte décide quelle boucle principale est liée :
#   - gnu*/mach*  : main-hurd.c (trivfs_startup + boucle serveur) ;
#   - sinon       : main.c (binaire de vérification, imprimant la
#                   bannière d'aide — utile pour `make check` et
#                   la compilation continue sur les autres POSIX).
case "$host_os" in
  gnu*|mach*) hurd_system=yes ;;
  *)          hurd_system=no ;;
esac
AM_CONDITIONAL([ON_HURD], [test "x$hurd_system" = xyes])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    tests/Makefile
])
AC_OUTPUT
