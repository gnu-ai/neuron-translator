# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

bin_PROGRAMS = sigmoid-neuron-translator

# --- Le cœur de calcul ------------------------------------------------
# neuron.c est du C23/POSIX pur (réseau sigmoïde feedforward,
# mémoire contiguë, aucune allocation dans les chemins chauds) ;
# trivfs-hooks.c implémente les fonctions trivfs_S_* appelées par
# le demuxer de libtrivfs.  Ces deux fichiers sont liés dans tous
# les cas ; seule la boucle principale change (voir configure.ac).
CORE_SOURCES = \
    neuron.c \
    trivfs-hooks.c

AM_CPPFLAGS = -I$(top_srcdir)/include
AM_CFLAGS = $(C23_CFLAGS) -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
            -DDEBUG -Wall -Wextra

# --- La boucle principale, selon le système ---------------------------
# Sur GNU/Hurd, main-hurd.c suit la structure canonique d'un
# translator trivfs (trivfs_startup puis boucle serveur) ; ailleurs,
# main.c construit le même binaire en mode vérification.
if ON_HURD
sigmoid_neuron_translator_SOURCES = $(CORE_SOURCES) main-hurd.c
sigmoid_neuron_translator_CPPFLAGS = $(AM_CPPFLAGS) -DON_HURD
sigmoid_neuron_translator_LDADD = -ltrivfs -lfshelp -lports \
                                 -lshouldbeinlibc -lm -lpthread
else
sigmoid_neuron_translator_SOURCES = $(CORE_SOURCES) main.c
sigmoid_neuron_translator_CPPFLAGS = $(AM_CPPFLAGS)
sigmoid_neuron_translator_LDADD = -lm -lpthread
endif
