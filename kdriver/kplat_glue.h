// SPDX-License-Identifier: GPL-2.0-only
/* What the WDF glue tells the platform layer (kplat.cpp, kproxy.cpp). */
#pragma once

void NekoGlueSetCard(volatile UCHAR *bar, ULONG barLength, USHORT vendor, USHORT device, USHORT subsystem,
                     WDFDMAENABLER dma);
void NekoGlueRunCloseOps();
