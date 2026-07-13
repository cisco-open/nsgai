/**
 * @file nsgai_cleanup.c
 * @brief Process and library teardown hooks.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
 * All rights reserved.
 */

#include "nsgai_internal.h"

void
nsgai_cleanup(void)
{
    nsgai_channel_cache_cleanup();
}

__attribute__((destructor)) static void
nsgai_library_destructor(void)
{
    nsgai_cleanup();
}
