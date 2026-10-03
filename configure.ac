# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

AC_INIT([data-base-translator], [0.1.0], [claire@gnu-ai.org])
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

AC_CANONICAL_HOST

# System extensions (POSIX/XSI): defines _GNU_SOURCE etc. in config.h.
# Required by the POSIX.1-2008 interfaces used from phase 1.  This
# macro must run before anything that compiles.
AC_USE_SYSTEM_EXTENSIONS

AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB

# Le projet est écrit en C23 ; certains compilateurs nomment encore ce
# mode « c2x ».  Exiger l'un des deux (même politique que
# httpfs-translator, pour une pile homogène).
AC_CACHE_CHECK([whether the C compiler accepts C23], [dbt_cv_c23], [
  dbt_cv_c23=no
  for dbt_flag in -std=c23 -std=c2x; do
    dbt_save_CFLAGS=$CFLAGS
    CFLAGS="$CFLAGS $dbt_flag"
    AC_COMPILE_IFELSE(
      [AC_LANG_PROGRAM([[
        constexpr int dbt_c23_test = 1;
        static void *dbt_c23_p = nullptr;]],
        [[return dbt_c23_p == nullptr ? dbt_c23_test : 0;]])],
      [dbt_cv_c23=$dbt_flag])
    CFLAGS=$dbt_save_CFLAGS
    test "x$dbt_cv_c23" != xno && break
  done])
AS_IF([test "x$dbt_cv_c23" = xno],
      [AC_MSG_ERROR([a C23 compiler is required (GCC 13 or newer)])])
AC_SUBST([C23_CFLAGS], [$dbt_cv_c23])

AC_CONFIG_HEADERS([config.h])

# --- GNU/Hurd ou non ---------------------------------------------------
# Le cœur du contrat (contract.c, ops.c, storage.c) est du C23/POSIX
# pur et se compile partout ; seules la boucle serveur trivfs et
# l'édition de liens des bibliothèques Hurd sont propres à
# GNU/Hurd.  Le système hôte décide quelle face est liée :
#   - gnu*/mach*  : main-hurd.c + trivfs-hooks.c (translator /db) ;
#   - sinon       : main.c (binaire de vérification sur stdio —
#                   utile pour `make check` et la compilation
#                   continue sur les autres POSIX).
case "$host_os" in
  gnu*|mach*) hurd_system=yes ;;
  *)          hurd_system=no ;;
esac
AM_CONDITIONAL([ON_HURD], [test "x$hurd_system" = xyes])

# libpq est le contrat de ce translator (PLAN.md section 3) : le
# driver est lié quand il est présent, et HAVE_LIBPQ l'annonce au
# code.  À l'image de libmicrohttpd dans httpfs-translator,
# l'absence est un avertissement, pas une erreur : sans driver le
# binaire reste compilable et testable (statut lisible, écritures
# en échec de transport), et la pile reste compilable partout.
PKG_CHECK_MODULES([LIBPQ], [libpq >= 12.0],
                  [have_libpq=yes], [have_libpq=no])
AM_CONDITIONAL([HAVE_LIBPQ], [test "x$have_libpq" = xyes])
AS_IF([test "x$have_libpq" = xyes],
      [AC_DEFINE([HAVE_LIBPQ], [1],
                 [Define to 1 when the libpq driver is linked in.])],
      [AC_MSG_WARN([libpq not found: the status stays readable, every write fails as EIO])])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    tests/Makefile
])
AC_OUTPUT
