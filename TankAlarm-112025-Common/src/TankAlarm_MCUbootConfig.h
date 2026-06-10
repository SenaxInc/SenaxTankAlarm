/**
 * TankAlarm_MCUbootConfig.h
 * 
 * Shared constants and configuration for MCUboot slot geometry and partitions.
 * Shared by KeyProvisioning, Client, Server, and Viewer.
 * 
 * Copyright (c) 2026 Senax Inc. All rights reserved.
 */

#ifndef TANKALARM_MCUBOOT_CONFIG_H
#define TANKALARM_MCUBOOT_CONFIG_H

#define TANKALARM_MCUBOOT_HEADER_SIZE  0x20000UL
#define TANKALARM_MCUBOOT_APP_SIZE     0x1C0000UL
#define TANKALARM_MCUBOOT_SLOT_SIZE    0x1E0000UL
#define TANKALARM_MCUBOOT_SCRATCH_SIZE 0x20000UL

#endif // TANKALARM_MCUBOOT_CONFIG_H
