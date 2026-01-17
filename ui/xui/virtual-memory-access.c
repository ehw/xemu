/*
 * Virtual Memory Access Implementation for xemu
 *
 * Simplified implementation that compiles cleanly with xemu build system
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

#include "virtual-memory-access.h"
#include "qemu/osdep.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"
#include "sysemu/cpus.h"
#include <glib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

// Virtual memory read function using xemu's CPU virtual memory access
// This function reads from the VIRTUAL address space, not physical
int xemu_virtual_memory_read(uint64_t virtual_addr, void *buffer, size_t size, 
                            char *error_msg, size_t error_msg_size) {
    // Validate address range
    if (!xemu_virtual_memory_is_valid_address(virtual_addr)) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Invalid virtual address: 0x%016lX", (unsigned long)virtual_addr);
        }
        return 0;
    }
    
    // Get the first CPU (for single-core Xbox emulation)
    CPUState *cpu = qemu_get_cpu(0);
    if (cpu == NULL) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "No CPU available for virtual memory read");
        }
        return 0;
    }
    
    // Use cpu_memory_rw_debug to read from VIRTUAL address space
    // The false parameter indicates this is a READ operation, not write
    int result = cpu_memory_rw_debug(cpu, virtual_addr, buffer, size, false);
    
    if (result != 0) {
        // CPU virtual memory read failed
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Failed to read virtual memory at 0x%016lX (size: %zu)", 
                    (unsigned long)virtual_addr, size);
        }
        return 0;
    }
    
    if (error_msg && error_msg_size > 0) {
        error_msg[0] = '\0'; // Clear error message on success
    }
    
    return 1;
}

// Virtual memory write function using xemu's CPU virtual memory access
// This function writes to the VIRTUAL address space, not physical
int xemu_virtual_memory_write(uint64_t virtual_addr, const void *data, size_t size,
                             char *error_msg, size_t error_msg_size) {
    // Validate address range
    if (!xemu_virtual_memory_is_valid_address(virtual_addr)) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Invalid virtual address: 0x%016lX", (unsigned long)virtual_addr);
        }
        return 0;
    }
    
    // Get the first CPU (for single-core Xbox emulation)
    CPUState *cpu = qemu_get_cpu(0);
    if (cpu == NULL) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "No CPU available for virtual memory write");
        }
        return 0;
    }
    
    // Use cpu_memory_rw_debug to write to VIRTUAL address space
    // The true parameter indicates this is a WRITE operation
    int result = cpu_memory_rw_debug(cpu, virtual_addr, (void*)data, size, true);
    
    if (result != 0) {
        // CPU virtual memory write failed
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Failed to write virtual memory at 0x%016lX (size: %zu)", 
                    (unsigned long)virtual_addr, size);
        }
        return 0;
    }
    
    if (error_msg && error_msg_size > 0) {
        error_msg[0] = '\0'; // Clear error message on success
    }
    
    return 1;
}

// Virtual memory address validation
bool xemu_virtual_memory_is_valid_address(uint64_t addr) {
    return addr <= XBOX_VIRTUAL_HIGH_MEMORY_END;
}

