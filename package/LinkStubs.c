// SPDX-License-Identifier: BSD-2-Clause-Patent
// Definitions of hooks that the patched XNU and the linked drivers reference from components the qemu defconfigs
// leave out. The kernel link needs each symbol even where its caller never runs or treats it as absent. They are weak:
// a linked component that defines a hook (qemu_root_defconfig's display archive defines the first one) replaces it.

#include <stdint.h>

#define STUB __attribute__((weak))

// offline-component-load-probes.patch, bsd_init: called on offline-legacy-register=1 whether or not
// NativeLegacyDisplay is linked. Without it the argument does nothing.
void native_legacy_display_probe(void);
STUB void native_legacy_display_probe(void)
{
}

// NativeVirtioBlock in verified mode (qemu_root_defconfig) asks NativeUfs whether a provider is one of its windows.
// It declares the hook weak, but a static kernel link still requires a definition; -1 means "not a UFS window", the
// same as the hook being absent.
int native_ufs_block_authenticates(void *provider, const char *uuid, uint8_t *digest);
STUB int native_ufs_block_authenticates(void *provider, const char *uuid, uint8_t *digest)
{
  (void)provider;
  (void)uuid;
  (void)digest;
  return -1;
}

// NativeRamBlock keeps the Data overlay in a UFS store when NativeUfs reports a store mode (sheng-data-persist). Mode 0
// is no store, so the Data ramdisk stays in memory and the other hooks are never called; they refuse if they are.
unsigned native_ufs_store_mode(void);
STUB unsigned native_ufs_store_mode(void)
{
  return 0;
}

int native_ufs_store_open(uint64_t blocks, uint64_t *first, const char **why);
STUB int native_ufs_store_open(uint64_t blocks, uint64_t *first, const char **why)
{
  (void)blocks;
  (void)first;
  if (why)
    *why = "no UFS store on this board";
  return 0;
}

int native_ufs_store_read(uint64_t block, uint32_t count, void *buffer);
STUB int native_ufs_store_read(uint64_t block, uint32_t count, void *buffer)
{
  (void)block;
  (void)count;
  (void)buffer;
  return 0;
}

int native_ufs_store_write(uint64_t block, uint32_t count, const void *buffer);
STUB int native_ufs_store_write(uint64_t block, uint32_t count, const void *buffer)
{
  (void)block;
  (void)count;
  (void)buffer;
  return 0;
}

int native_ufs_store_sync(void);
STUB int native_ufs_store_sync(void)
{
  return 0;
}

unsigned native_ufs_store_gpt_check(void);
STUB unsigned native_ufs_store_gpt_check(void)
{
  return 0;
}

// The display archive (qemu_root_defconfig) shares the frame buffer with sheng's boot progress screen, which this
// board does not have. Each definition behaves as the absent hook: the display may always draw, keeps no diagnostic
// mapping and reports no stages.
int boot_progress_prepare_scanout(void);
STUB int boot_progress_prepare_scanout(void)
{
  return 1;
}

void boot_progress_cancel_scanout(void);
STUB void boot_progress_cancel_scanout(void)
{
}

void boot_progress_stage(const char *message);
STUB void boot_progress_stage(const char *message)
{
  (void)message;
}

// The extension rows of sheng's diagnostic page, where KeyStoreProbe.cpp writes its AppleKeyStore counters.
STUB char boot_progress_ext[8][48];

int boot_progress_pulse_anchor(uintptr_t address, uint64_t bytes, unsigned width, unsigned height, unsigned row);
STUB int boot_progress_pulse_anchor(uintptr_t address, uint64_t bytes, unsigned width, unsigned height, unsigned row)
{
  (void)address;
  (void)bytes;
  (void)width;
  (void)height;
  (void)row;
  return 0;
}
