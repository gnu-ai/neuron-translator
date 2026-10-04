# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

# The CLI conformance test needs nothing but the built binary; the
# model tests exercise the compute core (neuron.c) directly — pure
# POSIX C23, no Hurd libraries, so both suites run on any system.

AM_CPPFLAGS = -I$(top_srcdir)/include
AM_CFLAGS = $(C23_CFLAGS) -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
            -Wall -Wextra

TESTS = test_cli.sh $(check_PROGRAMS)

EXTRA_DIST = test_cli.sh

check_PROGRAMS = test_model

# neuron.c is the whole compute core; the test drives it directly,
# the way an external trainer would drive the .nn contract.
test_model_SOURCES = test_model.c $(top_srcdir)/src/neuron.c
test_model_LDADD = -lm
