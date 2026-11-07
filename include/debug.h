/*
 * debug.h - Debug Logging Helper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 GNU AI Project
 * Author: Claire Ivanenka <claire@gnu-ai.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

/**
 * log_debug_message - Write diagnostics to the process's stderr.
 *
 * @param message The debug message to log
 */
static inline void log_debug_message(const char *message)
{
    fprintf(stderr, "%s\n", message);
    fflush(stderr);
}

#endif /* DEBUG_H */
