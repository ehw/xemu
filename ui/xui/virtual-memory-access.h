/*
 * Virtual Memory Access Functions for xemu
 *
 * Copyright (C) 2025 MiniMax Agent
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Virtual memory address space size for Xbox (4GB total)
// Xbox uses 32-bit virtual addresses, but actual usable space varies by game
#define XBOX_VIRTUAL_ADDRESS_SPACE_SIZE (1024ULL * 1024 * 1024 * 4) // 4GB

// Common Xbox virtual address ranges
#define XBOX_VIRTUAL_LOW_MEMORY_START  0x00000000
#define XBOX_VIRTUAL_LOW_MEMORY_END    0x7FFFFFFF  // 2GB - Low memory (user space)
#define XBOX_VIRTUAL_HIGH_MEMORY_START 0x80000000  
#define XBOX_VIRTUAL_HIGH_MEMORY_END   0xFFFFFFFF  // 2GB - High memory (system space)

// Standard XBE virtual base address
#define XBOX_VIRTUAL_XBE_BASE_DEFAULT  0x00010000

// XBE-specific virtual memory ranges
#define XBOX_VIRTUAL_XBE_CODE_START    0x00010000
#define XBOX_VIRTUAL_XBE_CODE_END      0x00100000  // ~1MB for code
#define XBOX_VIRTUAL_XBE_DATA_START    0x00100000
#define XBOX_VIRTUAL_XBE_DATA_END      0x02000000  // ~32MB for data
#define XBOX_VIRTUAL_XBE_HEAP_START    0x02000000
#define XBOX_VIRTUAL_XBE_HEAP_END      0x04000000  // ~32MB for heap
#define XBOX_VIRTUAL_XBE_STACK_START   0x04000000
#define XBOX_VIRTUAL_XBE_STACK_END     0x08000000  // ~64MB for stack

// Xbox system virtual memory ranges  
#define XBOX_VIRTUAL_SYSTEM_START      0x80000000
#define XBOX_VIRTUAL_KERNEL_START      0x80000000
#define XBOX_VIRTUAL_KERNEL_END        0xA0000000  // ~512MB for kernel
#define XBOX_VIRTUAL_HAL_START         0xA0000000
#define XBOX_VIRTUAL_HAL_END           0xC0000000  // ~512MB for HAL
#define XBOX_VIRTUAL_DRIVER_START      0xC0000000
#define XBOX_VIRTUAL_DRIVER_END        0xE0000000  // ~512MB for drivers

// Read memory from virtual address space
// Returns 1 on success, 0 on failure
int xemu_virtual_memory_read(uint64_t virtual_addr, void *buffer, size_t size, 
                            char *error_msg, size_t error_msg_size);

// Write memory to virtual address space (for patching)
// Returns 1 on success, 0 on failure  
int xemu_virtual_memory_write(uint64_t virtual_addr, const void *data, size_t size,
                             char *error_msg, size_t error_msg_size);

// Virtual address validation (internal helper function)
bool xemu_virtual_memory_is_valid_address(uint64_t addr);

#ifdef __cplusplus
}
#endif