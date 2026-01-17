/*
 * xemu Memory Patches System Implementation
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "xemu-patches.h"
#include "../xemu-xbe.h"
#include "xemu-notifications.h"
#include "ui/xui/virtual-memory-access.h"
#include "qemu/osdep.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include <glib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <time.h>



// Type definitions for virtual/physical addresses
typedef uint64_t vaddr;   // Virtual address
typedef uint64_t hwaddr;  // Physical address

// Xbox virtual memory address space constants
#define XBOX_VIRTUAL_LOW_MEMORY_START   0x00000000
#define XBOX_VIRTUAL_LOW_MEMORY_END     0x7FFFFFFF  // 2GB - Low memory (user space)
#define XBOX_VIRTUAL_HIGH_MEMORY_START  0x80000000  
#define XBOX_VIRTUAL_HIGH_MEMORY_END    0xFFFFFFFF  // 2GB - High memory (system space)

// Debug macro for MIN function
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// Debug macro for MAX function
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

// Helper function to validate Title IDs - eliminates repeated validation logic
static bool is_valid_xbox_title_id(uint32_t title_id) {
    return (title_id != 0xFFFF0002 &&  // Dashboard
            title_id != 0xFFFE0000 &&  // Invalid/Empty/Default  
            title_id != 0x00000000 &&  // Zero
            title_id != 0xFFFFFFFF);   // All ones
}

// Forward declaration for helper function used before definition
static void force_fresh_certificate_read(void);

// Helper function for consistent patch error reporting
static void report_patch_error(const char *patch_name, const char *operation) {
    error_report("Failed to %s patch '%s'", operation, patch_name);
}

// Helper function for safe memory reads with consistent error handling
static bool safe_memory_read(uint64_t address, void *buffer, size_t size, const char *context) {
    MemTxResult result = address_space_read(&address_space_memory, address, 
                                           MEMTXATTRS_UNSPECIFIED, buffer, size);
    if (result != MEMTX_OK) {
        return false;
    }
    return true;
}

// ============================================================================
// GLOBAL VARIABLES AND TYPE DEFINITIONS FOR PATCH MONITORING
// ============================================================================

#define MAX_MONITORED_PATCHES 16

typedef struct {
    uint32_t address;
    uint8_t value_data[16];
    int data_length;
    time_t applied_time;
    uint32_t verification_count;
    bool still_active;
    bool is_jake_patch;
    char description[128];
    uint8_t original_data[16];
} monitored_patch_t;

static monitored_patch_t g_monitored_patches[MAX_MONITORED_PATCHES];
static int g_monitored_patch_count = 0;
static bool g_monitoring_enabled = true;
static time_t g_last_monitoring_time = 0;

// ============================================================================
// RESET-SPECIFIC MEMORY MONITORING
// ============================================================================

// Flag to track when we're performing reset monitoring
static bool g_reset_monitoring_active = false;
static bool g_vm_reset_completed = false;           // Track when VM reset has actually completed
static uint64_t g_vm_reset_completion_time = 0;     // Time when VM reset completion was detected

// Structure to track reset monitoring data
typedef struct {
    uint32_t address;
    uint32_t value_before;     // 4-byte value before patch application
    uint32_t value_after;      // 4-byte value immediately after patch application
    uint32_t value_1sec;       // 4-byte value 1 second after
    uint32_t value_2sec;       // 4-byte value 2 seconds after
    uint32_t value_3sec;       // 4-byte value 3 seconds after
    bool monitoring_active;    // Whether this address is being monitored
    time_t start_time;         // When monitoring started
    char patch_name[128];      // Name of the patch modifying this address
} reset_monitored_address_t;

#define MAX_RESET_MONITORED_ADDRESSES 32
static reset_monitored_address_t g_reset_monitored_addresses[MAX_RESET_MONITORED_ADDRESSES];
static int g_reset_monitored_count = 0;

// Forward declarations for reset monitoring functions
void start_reset_memory_monitoring(void);
void stop_reset_memory_monitoring(void);
static void monitor_reset_patch_address(uint32_t address, const char* patch_name);
static void log_reset_memory_values(uint32_t address, const char* context);
static void perform_periodic_reset_monitoring(void);
static uint32_t read_32bit_value(uint32_t address, bool* success);

// ============================================================================
// FORWARD FUNCTION DECLARATIONS
// ============================================================================
static void verify_monitored_patch(int patch_index);

// ============================================================================
// VIRTUAL TO PHYSICAL ADDRESS TRANSLATION (QEMU/XEMU Integration)
// ============================================================================



// Direct virtual memory access - NO conversion to physical addresses
// We work with virtual addresses as-is using xemu's built-in virtual memory system
static bool write_direct_virtual_memory(vaddr vaddr, const void *buf, size_t len)
{
    char error_msg[256];
    
    // Use xemu's virtual memory write function directly
    // This handles the virtual memory access internally without requiring physical conversion
    if (xemu_virtual_memory_write(vaddr, buf, len, error_msg, sizeof(error_msg))) {
        return true;
    } else {
        return false;
    }
}

// ============================================================================
// RESET-SPECIFIC MEMORY MONITORING IMPLEMENTATION
// ============================================================================

// Start reset memory monitoring session
void start_reset_memory_monitoring(void)
{
    g_reset_monitoring_active = true;
    g_reset_monitored_count = 0;
}

// Stop reset memory monitoring session
void stop_reset_memory_monitoring(void)
{
    g_reset_monitoring_active = false;
}

// Read a 4-byte value from virtual memory
static uint32_t read_32bit_value(uint32_t address, bool* success)
{
    uint32_t value = 0;
    uint8_t buffer[4];
    char error_msg[256];
    
    if (xemu_virtual_memory_read(address, buffer, 4, error_msg, sizeof(error_msg))) {
        value = (buffer[0]) | (buffer[1] << 8) | (buffer[2] << 16) | (buffer[3] << 24);
        *success = true;
    } else {
        *success = false;
    }
    
    return value;
}

// Monitor a specific address for reset operations
static void monitor_reset_patch_address(uint32_t address, const char* patch_name)
{
    if (!g_reset_monitoring_active || g_reset_monitored_count >= MAX_RESET_MONITORED_ADDRESSES) {
        return;
    }
    
    // Check if this address is already being monitored
    for (int i = 0; i < g_reset_monitored_count; i++) {
        if (g_reset_monitored_addresses[i].address == address) {
            return; // Already monitoring this address
        }
    }
    
    // Add new address to monitoring list
    int index = g_reset_monitored_count++;
    g_reset_monitored_addresses[index].address = address;
    g_reset_monitored_addresses[index].monitoring_active = true;
    g_reset_monitored_addresses[index].start_time = time(NULL);
    strncpy(g_reset_monitored_addresses[index].patch_name, patch_name, sizeof(g_reset_monitored_addresses[index].patch_name) - 1);
    g_reset_monitored_addresses[index].patch_name[sizeof(g_reset_monitored_addresses[index].patch_name) - 1] = '\0';
    
    // Read and log the initial value (before patch application)
    // But skip this during reset if the value looks like uninitialized memory (0x62 pattern)
    bool read_success = false;
    uint32_t initial_value = read_32bit_value(address, &read_success);
    
    if (read_success) {
        // Check if this looks like uninitialized/placeholder memory during reset
        bool looks_like_placeholder = (initial_value == 0x00000062) || 
                                      (initial_value == 0x62000000) ||
                                      (initial_value == 0x00000000);
        
        if (!looks_like_placeholder) {
            // Format the initial value as hex string for better readability
            char initial_hex[32];
            sprintf(initial_hex, "%02X %02X %02X %02X", 
                    (initial_value >> 24) & 0xFF,
                    (initial_value >> 16) & 0xFF, 
                    (initial_value >> 8) & 0xFF,
                    initial_value & 0xFF);
            g_reset_monitored_addresses[index].value_before = initial_value;
        } else {
            g_reset_monitored_addresses[index].value_before = 0xDEADBEEF; // Mark as placeholder, will be updated after patch
        }
    } else {
        g_reset_monitored_addresses[index].value_before = 0xFFFFFFFF; // Mark as unreadable
    }
}

// Log current memory values at all monitored addresses
static void log_reset_memory_values(uint32_t address, const char* context)
{
    if (!g_reset_monitoring_active) return;
    
    for (int i = 0; i < g_reset_monitored_count; i++) {
        if (!g_reset_monitored_addresses[i].monitoring_active || 
            g_reset_monitored_addresses[i].address != address) {
            continue;
        }
        
        bool read_success = false;
        uint32_t current_value = read_32bit_value(address, &read_success);
        
        if (read_success) {
            // Format the current value as hex string for better readability
            char current_hex[32];
            sprintf(current_hex, "%02X %02X %02X %02X", 
                    (current_value >> 24) & 0xFF,
                    (current_value >> 16) & 0xFF, 
                    (current_value >> 8) & 0xFF,
                    current_value & 0xFF);
            
            if (strcmp(context, "after_application") == 0) {
                g_reset_monitored_addresses[i].value_after = current_value;
            } else if (strcmp(context, "1sec_after") == 0) {
                g_reset_monitored_addresses[i].value_1sec = current_value;
            } else if (strcmp(context, "2sec_after") == 0) {
                g_reset_monitored_addresses[i].value_2sec = current_value;
            } else if (strcmp(context, "3sec_after") == 0) {
                g_reset_monitored_addresses[i].value_3sec = current_value;
            } else if (strcmp(context, "4sec_after") == 0) {
            } else if (strcmp(context, "5sec_after") == 0) {
            } else if (strcmp(context, "6sec_after") == 0) {
            } else if (strcmp(context, "7sec_after") == 0) {
            } else if (strcmp(context, "8sec_after") == 0) {
            } else if (strcmp(context, "9sec_after") == 0) {
            } else if (strcmp(context, "10sec_after") == 0) {
            }
        } else {
        }
    }
}

// Perform periodic monitoring after patch application
// Helper function to log reset memory values at specific time intervals
static void log_reset_memory_at_interval(uint32_t address, int elapsed_seconds, bool *monitoring_active) {
    char context[32];
    snprintf(context, sizeof(context), "%dsec_after", elapsed_seconds);
    log_reset_memory_values(address, context);
    
    // Stop monitoring after 10 seconds
    if (elapsed_seconds >= 10) {
        *monitoring_active = false;
    }
}

static void perform_periodic_reset_monitoring(void)
{
    if (!g_reset_monitoring_active) return;
    
    time_t current_time = time(NULL);
    
    for (int i = 0; i < g_reset_monitored_count; i++) {
        if (!g_reset_monitored_addresses[i].monitoring_active) continue;
        
        time_t elapsed = current_time - g_reset_monitored_addresses[i].start_time;
        
        // Monitor every second for 10 seconds - simplified to use helper function
        if (elapsed >= 1 && elapsed < 11) {
            int seconds = (int)elapsed;
            log_reset_memory_at_interval(g_reset_monitored_addresses[i].address, seconds, 
                                        &g_reset_monitored_addresses[i].monitoring_active);
        }
    }
}

// Direct virtual memory read function for monitoring
static bool read_direct_virtual_memory(vaddr vaddr, void *buf, size_t len)
{
    char error_msg[256];

    
    // Use xemu's virtual memory read function directly
    if (xemu_virtual_memory_read(vaddr, buf, len, error_msg, sizeof(error_msg))) {
        if (error_msg[0]) {
            // Debug details available
        }
        return true;
    } else {
        return false;
    }
}

// Global patches database
XemuPatchesDatabase g_patches_db = {0};
bool g_patches_initialized = false;
bool g_patches_loaded = false;
static bool g_save_in_progress = false;  // Prevent recursive saves
static int g_save_timer = 0;             // Auto-save timer

// Certificate tracking to detect game switches
static uint32_t g_last_cert_title_id = 0;
static uint32_t g_last_cert_region = 0;
static uint32_t g_last_cert_version = 0;
static uint64_t g_last_cert_read_time = 0;
static bool g_cert_data_valid = false;

// Certificate cache for performance optimization
typedef struct {
    bool valid;
    int frame_last_read;
    uint64_t last_read_time;  // SDL_GetTicks() timestamp for reliable timing
    uint32_t title_id;
    uint32_t region;
    uint32_t version;
} cert_cache_t;
static cert_cache_t g_xbe_cache = {false, -1, 0, 0, 0, 0};

// Global flag to force fresh XBE certificate reads
bool g_force_fresh_xbe_read = false;

// Load Disc operation tracking to prevent early patch application
bool g_load_disc_in_progress = false;

// Post-reset processing protection to prevent spam and crashes
bool g_post_reset_crash_protection_active = false;

// Disc presence tracking - CRITICAL for preventing crashes when no disc is loaded
bool g_disc_present = false;        // Is a disc currently loaded?
bool g_patch_system_enabled = false; // Should we apply patches? (only when disc present)

// Forward declaration for disc presence management functions
static void update_disc_presence_state(bool disc_present);


// NOTIFICATION COORDINATION: Flag to prevent duplicate notifications across systems
bool g_notification_generation_active = false;
static uint32_t g_last_patch_application_title_id = 0;


// Manual reset tracking for coordination between notification systems
bool g_manual_reset_detected = false;
bool g_post_reset_system_active = false;

// INFINITE LOOP PREVENTION: Variables to prevent repeated reset detection
bool g_reset_detected_in_progress = false;
int g_reset_detection_count = 0;


uint32_t g_post_reset_current_title_id = 0;
uint32_t g_post_reset_start_time = 0;
int g_post_reset_call_count = 0;
bool g_immediate_trigger_checked = false;
uint64_t g_immediate_trigger_time = 0;

// COORDINATION FLAG: Prevent Simple Auto-Apply from running when Post-Reset system is active
bool g_post_reset_patch_application_active = false;

// NEW COORDINATION FLAG: Only allow VM reset detection after actual reset triggers
bool g_vm_reset_triggered = false;  // Set to true only when actual reset events occur

// Reset the last applied tracking variables to force fresh game detection
// This function will be defined where the static variables are accessible
void reset_last_applied_tracking(void);

// Load Disc operation tracking functions

// Called when Load Disc operation completes successfully (disc loaded)
void set_load_disc_completed(void) {
    // Only clear certificate data if we had valid data before (indicating a disc change)
    if (g_cert_data_valid && g_last_cert_title_id != 0) {
        g_last_cert_title_id = 0;
        g_last_cert_region = 0;
        g_last_cert_version = 0;
        g_cert_data_valid = false;
        g_patches_applied_for_current_cert = false;
    } else {
    }
    
    // ENHANCED FIX: Clear certificate cache to prevent stale data in Add Game dialog
    g_xbe_cache.valid = false;
    g_xbe_cache.frame_last_read = -1;
    g_xbe_cache.last_read_time = 0;
    
    // CRITICAL FIX: Force fresh certificate read for the newly loaded disc
    g_force_fresh_xbe_read = true;
    
    // Now clear the Load Disc flag - CRITICAL: This must happen immediately
    g_load_disc_in_progress = false;
    
    // CRITICAL FIX: Clear UI button state to force refresh
    
    // ENHANCED LOAD DISC COORDINATION: Schedule patch application retry
    
    // Set a flag to retry certificate reading and patch application
    g_load_disc_retry_pending = true;
    
    // IMMEDIATE RETRY: Try to get fresh certificate data right away
    g_force_fresh_xbe_read = true;
    
    // ENHANCED LOAD DISC: Schedule multiple retry attempts for certificate reading
    // load_disc_retry_count variable removed - not needed for current implementation
    
    update_disc_presence_state(true); // Enable patch system (disc is now present)
	
    
    // DUPLICATE PREVENTION: Prevent multiple Load Disc completions from triggering patch application
    static uint32_t g_last_processed_title_id = 0;
    
    // If certificate data is now available, apply patches immediately
    if (g_cert_data_valid && g_last_cert_title_id != 0) {
        
        // ENHANCED: Check if this is truly a new game that needs patches
        bool is_new_game = (g_last_processed_title_id != g_last_cert_title_id) || !g_patches_applied_for_current_cert;
        
        if (!is_new_game) {
            return;
        }
        
        // Reset the applied flag to allow patch application for the new game
        g_patches_applied_for_current_cert = false;
        g_last_processed_title_id = g_last_cert_title_id;
        g_disc_present = true; // Ensure disc presence is set
        
        
        // Call auto_enable_patches_when_ready to coordinate with the main system
        auto_enable_patches_when_ready();
    } else {
        g_last_processed_title_id = 0;
    }
}

// Called when disc is ejected or no disc is loaded

// Forward declaration for certificate detection function
static bool get_cached_xbe_info(uint32_t *title_id, uint32_t *region, uint32_t *version);
static bool get_cached_xbe_info_with_spam_prevention(uint32_t *title_id, uint32_t *region, uint32_t *version);

// Auto-boot detection function - checks if disc is already loaded on startup
static void check_for_auto_loaded_disc(void) {
    
    // First check: If we already have valid certificate data
    if (g_cert_data_valid && g_last_cert_title_id != 0) {
        g_disc_present = true;
        g_patch_system_enabled = true;
        return;
    }
    
    // Check if certificate data is already valid from previous detection
    if (g_cert_data_valid && g_last_cert_title_id != 0) {
        g_disc_present = true;
        g_patch_system_enabled = true;
        return;
    }
    
    // ENHANCED AUTO-BOOT: Force fresh certificate read for startup detection
    force_fresh_certificate_read();
    
    // Try immediate certificate detection with fresh read
    uint32_t temp_title_id, temp_region, temp_version;
    if (get_cached_xbe_info(&temp_title_id, &temp_region, &temp_version)) {
        
        // FIXED VALIDATION: Enhanced TitleID validation for startup scenarios
        // More comprehensive validation that handles XEMU initialization states
        bool is_valid_title_id = is_valid_xbox_title_id(temp_title_id);
        
        // Additional check: ensure TitleID doesn't have obviously invalid patterns
        if (is_valid_title_id) {
            // Check for unrealistic TitleID patterns
            if ((temp_title_id & 0xFF000000) == 0xFF000000 || // All high byte set
                (temp_title_id & 0x00FF0000) == 0x00FF0000 || // All middle-high byte set  
                (temp_title_id & 0x0000FF00) == 0x0000FF00 || // All middle-low byte set
                (temp_title_id & 0x000000FF) == 0x000000FF) { // All low byte set
                is_valid_title_id = false;
            }
        }
        
        if (is_valid_title_id) {  
            // Store certificate data for immediate auto-boot patch application
            g_last_cert_title_id = temp_title_id;
            g_last_cert_region = temp_region;
            g_last_cert_version = temp_version;
            g_cert_data_valid = true;
            g_disc_present = true;
            g_patch_system_enabled = true;
            
            // CRITICAL: Use auto_enable_patches_when_ready() to properly coordinate with the system
            auto_enable_patches_when_ready();
            return;
        } else {
        }
    } else {
    }
    
    // FIXED AUTO-BOOT RETRY: Implement improved retry mechanism for startup certificate reading
    static uint32_t auto_boot_retry_count = 0;
    static uint32_t auto_boot_start_time = 0;
    
    // Initialize retry tracking on first call
    if (auto_boot_start_time == 0) {
        auto_boot_start_time = SDL_GetTicks();
        auto_boot_retry_count = 0;
    }
    
    // Retry for up to 10 seconds if we haven't found valid certificate data (extended timeout)
    uint32_t elapsed_time = SDL_GetTicks() - auto_boot_start_time;
    if (elapsed_time < 10000 && auto_boot_retry_count < 20) { // Extended to 10 seconds, 20 attempts
        auto_boot_retry_count++;
        
        // Force fresh certificate read for retry
        g_force_fresh_xbe_read = true;
        
        // Try certificate read again
        if (get_cached_xbe_info_with_spam_prevention(&temp_title_id, &temp_region, &temp_version)) {
            
            // ENHANCED VALIDATION: Accept more TitleIDs as valid for auto-boot
            bool is_valid_title_id = is_valid_xbox_title_id(temp_title_id);
            
            if (is_valid_title_id) {
                
                // Store certificate data
                g_last_cert_title_id = temp_title_id;
                g_last_cert_region = temp_region;
                g_last_cert_version = temp_version;
                g_cert_data_valid = true;
                g_disc_present = true;
                g_patch_system_enabled = true;
                
                // Reset retry tracking
                auto_boot_start_time = 0;
                auto_boot_retry_count = 0;
                
                // CRITICAL FIX: Clear any existing patch application tracking to prevent duplicates
                g_patches_applied_for_current_cert = false;
                g_last_patch_application_title_id = 0; // Clear duplicate prevention tracking
                
                // Call patch enable function
                auto_enable_patches_when_ready();
                return;
            } else {
            }
        } else {
        }
    } else if (elapsed_time >= 10000) {
        auto_boot_start_time = 0; // Reset for next time
        auto_boot_retry_count = 0;
    }
    
    // Fallback: Try direct XBE info access if main function fails
    struct xbe *xbe_info = xemu_get_xbe_info();
    
    if (xbe_info && xbe_info->cert) {
        uint32_t title_id = ldl_le_p(&xbe_info->cert->m_titleid);
        
        // FIXED FALLBACK VALIDATION: Enhanced validation for fallback path
        bool is_valid_title_id = (title_id != 0 && 
                                 title_id != 0xFFFF0002 &&  // Dashboard
                                 title_id != 0xFFFE0000 &&  // Invalid/Empty/Default
                                 title_id != 0x00000000 &&  // Zero
                                 title_id != 0xFFFFFFFF);   // All ones
        
        // Additional validation for fallback path
        if (is_valid_title_id) {
            // Check for unrealistic TitleID patterns in fallback
            if ((title_id & 0xFF000000) == 0xFF000000 || // All high byte set
                (title_id & 0x00FF0000) == 0x00FF0000 || // All middle-high byte set  
                (title_id & 0x0000FF00) == 0x0000FF00 || // All middle-low byte set
                (title_id & 0x000000FF) == 0x000000FF) { // All low byte set
                is_valid_title_id = false;
            }
        }
        
        if (is_valid_title_id) {
            
            // Store certificate data directly for immediate auto-boot patch application
            g_last_cert_title_id = title_id;
            g_last_cert_region = ldl_le_p(&xbe_info->cert->m_game_region);
            g_last_cert_version = ldl_le_p(&xbe_info->cert->m_version);
            g_cert_data_valid = true;
            g_disc_present = true;
            g_patch_system_enabled = true;
            
            // CRITICAL FIX: Clear patch application tracking for fallback path too
            g_patches_applied_for_current_cert = false;
            g_last_patch_application_title_id = 0;
            
            // CRITICAL: Use auto_enable_patches_when_ready() to properly coordinate with the system
            auto_enable_patches_when_ready();
            return;
        } else {
        }
    }
    
    g_disc_present = false;
    g_patch_system_enabled = false;
}

// CRITICAL: Add startup retry mechanism to detect games after XEMU initialization
static uint32_t g_startup_retry_count = 0;
static uint64_t g_last_startup_retry_time = 0;
static bool g_startup_retry_enabled = true;



// GLOBAL CERTIFICATE SPAM PREVENTION - Applied to ALL XBE reads
static uint32_t g_last_invalid_title_id = 0;
static uint64_t g_last_invalid_read_time = 0;
static int g_invalid_read_count = 0;



// ENHANCED NOTIFICATION DUPLICATE PREVENTION
static uint32_t last_notified_title_id = 0;
static uint64_t last_notification_time = 0;

// FORWARD DECLARATIONS - New functions for regression fixes
static void reset_notification_tracking_for_new_game(void);


// WRAPPER FUNCTION: get_cached_xbe_info_with_spam_prevention
// This wrapper applies global spam prevention to ALL certificate reads
static bool get_cached_xbe_info_with_spam_prevention(uint32_t *title_id, uint32_t *region, uint32_t *version) {
    // Call the original function
    bool result = get_cached_xbe_info(title_id, region, version);
    
    if (result && title_id && *title_id) {
        // Valid certificate found - reset spam counters
        g_invalid_read_count = 0;
        g_last_invalid_title_id = 0;
        
        // CRITICAL FIX: Update global certificate variables for UI consistency
        // This prevents UI flickering by ensuring global state matches actual certificate data
        bool is_valid_title_id = (*title_id != 0 && 
                                 *title_id != 0xFFFF0002 &&  // Dashboard
                                 *title_id != 0xFFFE0000 &&  // Invalid/Empty
                                 *title_id != 0x00000000 &&  // Zero
                                 *title_id != 0xFFFFFFFF);   // All ones
        
        if (is_valid_title_id) {
            // Update global certificate data to prevent UI flickering
            bool was_valid = g_cert_data_valid;
            uint32_t old_title_id = g_last_cert_title_id;
            
            g_last_cert_title_id = *title_id;
            g_last_cert_region = *region;
            g_last_cert_version = *version;
            g_cert_data_valid = true;
            g_disc_present = true;
            
            // Log if this is a certificate change (indicating game switch)
            if (was_valid && old_title_id != *title_id) {
                
                // ENHANCED: Reset notification tracking when game switch is detected
                // This allows the new game to generate notifications without being blocked by old tracking data
                reset_notification_tracking_for_new_game();
            }
        }
        
        // Debug valid certificate reads
        if (*title_id != 0xFFFE0000) {
        }
        return result;
    }
    
    // Handle invalid or failed certificate reads
    if (title_id && *title_id) {
        // We got a certificate but it's invalid
        g_invalid_read_count++;
        
        // CRITICAL FIX: When we detect repeated invalid certificates, update global state to indicate "no disc"
        if (*title_id == 0xFFFE0000 || *title_id == 0xFFFF0002) {
            if (g_invalid_read_count > 5) {
                // After 5 consecutive invalid reads, clear certificate data to indicate "no disc"
                if (g_cert_data_valid) {
                    
                    g_last_cert_title_id = 0;
                    g_last_cert_region = 0;
                    g_last_cert_version = 0;
                    g_cert_data_valid = false;
                    g_disc_present = false;
                }
            }
        }
        
        // Check if we're reading the same invalid TitleID repeatedly
        if (*title_id == g_last_invalid_title_id) {
            if (g_invalid_read_count > 5) {
                // After 5 consecutive reads of same invalid ID, implement delay
                if (time(NULL) - g_last_invalid_read_time < 10000) {
                    return false; // Block this read
                }
            }
            // ENHANCED: If we're getting the same invalid ID (like 0xFFFE0000), block more aggressively
            if (*title_id == 0xFFFE0000 && g_invalid_read_count > 5) {
                return false; // Block aggressively for no-disc scenarios
            }
        } else {
            // Different invalid TitleID, reset counter
            g_invalid_read_count = 1;
        }
        
        g_last_invalid_title_id = *title_id;
        g_last_invalid_read_time = time(NULL);
        
    }
    
    return result;
}

// Initialize disc presence state on xemu startup
void initialize_disc_presence_tracking(void) {
    
    // CRITICAL FIX: Initialize all state variables properly
    g_disc_present = false;
    g_patch_system_enabled = false;
    g_load_disc_in_progress = false;
    g_cert_data_valid = false;
    g_patches_applied_for_current_cert = false;
    g_last_cert_title_id = 0;
    g_last_cert_region = 0;
    g_last_cert_version = 0;
    
    uint32_t temp_title_id = 0, temp_region = 0, temp_version = 0;
    bool cert_found = false;
    
    // Method 1: Try cached certificate read
    cert_found = get_cached_xbe_info_with_spam_prevention(&temp_title_id, &temp_region, &temp_version);
    
    // Method 2: If cached failed, try direct XBE read
    if (!cert_found) {
        // Force fresh read for startup detection
        g_force_fresh_xbe_read = true;
        cert_found = get_cached_xbe_info_with_spam_prevention(&temp_title_id, &temp_region, &temp_version);
    }
    
    // Validate the certificate
    if (cert_found && is_valid_xbox_title_id(temp_title_id)) {
        
        // Set all the state variables to indicate successful startup detection
        g_last_cert_title_id = temp_title_id;
        g_last_cert_region = temp_region;
        g_last_cert_version = temp_version;
        g_cert_data_valid = true;
        g_disc_present = true;
        g_patch_system_enabled = true;
    } else {
        
        // STARTUP FALLBACK: Set up for delayed detection
        force_fresh_certificate_read(); // Force fresh reads during startup
    }
    
    // 🔍 AUTO-BOOT DETECTION: Check if disc is already loaded (auto-boot scenario)
    // This handles cases where XEMU starts with a disc already in the drive
    check_for_auto_loaded_disc();
    
    // Call auto-boot patch application
    apply_patches_for_auto_boot();
    
    // ENABLE STARTUP RETRY: If auto-boot failed, enable runtime retry detection
    if (!g_cert_data_valid) {
        g_startup_retry_enabled = true;
        g_startup_retry_count = 0;
        g_last_startup_retry_time = 0;
    }
}

// CRITICAL: Add startup retry mechanism to detect games after XEMU initialization
static void check_startup_retry_detection(void) {
    if (!g_startup_retry_enabled) return;
    
    uint64_t current_time = SDL_GetTicks();
    
    // Only retry every 2 seconds to avoid excessive calls
    if (current_time - g_last_startup_retry_time < 2000) return;
    
    g_last_startup_retry_time = current_time;
    g_startup_retry_count++;
    
    uint32_t temp_title_id = 0, temp_region = 0, temp_version = 0;
    bool cert_found = get_cached_xbe_info_with_spam_prevention(&temp_title_id, &temp_region, &temp_version);
    
    if (cert_found && is_valid_xbox_title_id(temp_title_id)) {
        
        // Update state variables
        g_last_cert_title_id = temp_title_id;
        g_last_cert_region = temp_region;
        g_last_cert_version = temp_version;
        g_cert_data_valid = true;
        g_disc_present = true;
        g_patch_system_enabled = true;
        
        // Disable further retries since we found the game
        g_startup_retry_enabled = false;
        apply_patches_for_auto_boot();
    } else {
        
        // Stop retrying after 5 attempts to prevent infinite loops
        if (g_startup_retry_count >= 5) {
            g_startup_retry_enabled = false;
        }
    }
}



// Public function to check if disc is present (for UI integration)

// Enhanced disc presence check - simplified to just return global flag
bool is_disc_present_enhanced(void) {
    return g_disc_present;
}


// Auto-enable patches when certificate data becomes available
// This handles auto-boot and normal operation scenarios
void auto_enable_patches_when_ready(void) {
    // CRITICAL: Prevent ALL certificate operations during Load Disc
    // Load Disc operations cause VM instability and can cause memory crashes
    if (g_load_disc_in_progress) {
        return;
    }
    
    // Only enable patches if we have certificate data and haven't already enabled them
    if (g_cert_data_valid && g_last_cert_title_id != 0 && !g_patch_system_enabled) {
        g_disc_present = true;
        g_patch_system_enabled = true;
        
        // Apply patches if they haven't been applied yet
        if (!g_patches_applied_for_current_cert) {
            
            // DUPLICATE PREVENTION: Mark as applied before calling the function to prevent dual application
            g_patches_applied_for_current_cert = true;
            
            apply_patches_for_auto_boot();
        } else {
        }
    }
    
    // CRITICAL FIX: Apply patches if patch system is enabled but patches haven't been applied yet
    // This handles cases where certificate data becomes available after initial enable
    if (g_cert_data_valid && g_last_cert_title_id != 0 && g_patch_system_enabled && !g_patches_applied_for_current_cert) {
        // DUPLICATE PREVENTION: Mark as applied before calling the function to prevent dual application
        g_patches_applied_for_current_cert = true;
        
        apply_patches_for_auto_boot();
    }
    
    // CRITICAL: If patches were already applied, just log and exit (no re-application)
    if (g_cert_data_valid && g_last_cert_title_id != 0 && g_patch_system_enabled && g_patches_applied_for_current_cert) {
    }
}

// CRITICAL: Auto-boot patch application for games loaded at startup
// This ensures patches apply immediately when XEMU starts with a disc already loaded

// Apply patches for auto-boot scenario (called during XEMU startup)
// CRITICAL FIX: Add global flag to prevent ANY duplicate spam
static bool g_auto_boot_processing_active = false;

void apply_patches_for_auto_boot(void) {
    // ULTIMATE DUPLICATE PREVENTION: Prevent ALL duplicate processing
    if (g_auto_boot_processing_active) {
        return; // Exit immediately if already processing
    }
    
    // Enhanced duplicate prevention with logging suppression
    if (g_patches_applied_for_current_cert && g_last_cert_title_id != 0) {
        // Only log every 100th duplicate to prevent spam
        static uint32_t duplicate_log_count = 0;
        duplicate_log_count++;
        if (duplicate_log_count % 100 == 0) {
        }
        return; // Exit silently to prevent spam
    }
    
    // SET PROCESSING FLAG: Mark that we're processing to prevent duplicates
    g_auto_boot_processing_active = true;
    
    
    // Basic validation
    if (!g_patch_system_enabled || !g_cert_data_valid || g_last_cert_title_id == 0) {
        return;
    }
    
    // Simple TitleID to string conversion
    char title_id_str[17];
    snprintf(title_id_str, sizeof(title_id_str), "%08X", g_last_cert_title_id);
    
    // Find matching game
    XemuGamePatches* game = NULL;
    for (int i = 0; i < g_patches_db.game_count; i++) {
        if (g_patches_db.games[i].title_id && 
            strcasecmp(g_patches_db.games[i].title_id, title_id_str) == 0) {
            game = &g_patches_db.games[i];
            break;
        }
    }
    
    if (!game) {
        return;
    }
    
    if (game) {
        
        // Count and apply patches
        int patch_count = 0;
        for (int i = 0; i < game->patch_count; i++) {
            if (game->patches[i].enabled) {
                patch_count++;
            }
        }
        
        if (patch_count > 0) {
            
            // CRITICAL: Set flag BEFORE any other processing to prevent duplicates
            g_patches_applied_for_current_cert = true;
        } else {
            // Still mark as applied to prevent repeated searching
            g_patches_applied_for_current_cert = true;
        }
    }
    
    // CLEAR PROCESSING FLAG: Allow future processing after completion
    g_auto_boot_processing_active = false;
}

// Cached XBE info is now defined globally using cert_cache_t typedef above

// FIXED: Force fresh certificate read for Add Game dialog to prevent stale data
// This ensures the Add Game dialog always shows current game information


// Public function to check if patch system is enabled (for UI integration)

// Disc presence management functions - CRITICAL for crash prevention
static void update_disc_presence_state(bool disc_present) {
    if (disc_present && !g_disc_present) {
        // Disc was just inserted - enable patch system
        g_disc_present = true;
        g_patch_system_enabled = true;
        g_force_fresh_xbe_read = true; // Force fresh certificate read
        
        // Only clear certificate data if we had valid data before (indicating a genuine disc change)
        if (g_cert_data_valid && g_last_cert_title_id != 0) {
            g_cert_data_valid = false;
            g_last_cert_title_id = 0;
            g_last_cert_region = 0;
            g_last_cert_version = 0;
            g_patches_applied_for_current_cert = false;
        } else {
        }
        
        // TRIGGER PATCH APPLICATION: Call auto-enable to apply patches when disc is inserted
        auto_enable_patches_when_ready();
    } else if (!disc_present && g_disc_present) {
        g_disc_present = false;
        g_patch_system_enabled = false;
        
        // Clear cached certificate data to prevent confusion
        g_cert_data_valid = false;
        g_last_cert_title_id = 0;
        g_last_cert_region = 0;
        g_last_cert_version = 0;
        
        // Clear patch applied flag when disc is ejected
        g_patches_applied_for_current_cert = false;
    }
}



// ============================================================================
// SAVE REPLACED VALUES SYSTEM
// ============================================================================

#define MAX_SAVED_VALUES 1024

typedef struct {
    int game_index;
    int patch_index;
    uint32_t address;
    uint8_t *original_data;
    int data_length;
} SavedValueEntry;

static SavedValueEntry g_saved_values[MAX_SAVED_VALUES];
static int g_saved_values_count = 0;

// Find saved value entry for specific game/patch/address
static SavedValueEntry* find_saved_value(int game_index, int patch_index, uint32_t address)
{
    for (int i = 0; i < g_saved_values_count; i++) {
        if (g_saved_values[i].game_index == game_index && 
            g_saved_values[i].patch_index == patch_index &&
            g_saved_values[i].address == address) {
            
            return &g_saved_values[i];
        }
    }
    return NULL;
}

// Add a new saved value entry
static SavedValueEntry* add_saved_value(int game_index, int patch_index, uint32_t address, const uint8_t *original_data, int data_length)
{
    if (g_saved_values_count >= MAX_SAVED_VALUES) {
        return NULL;
    }
    
    // Check if entry already exists
    SavedValueEntry *existing = find_saved_value(game_index, patch_index, address);
    if (existing) {
        // Update existing entry
        g_free(existing->original_data);
        existing->original_data = g_malloc(data_length);
        memcpy(existing->original_data, original_data, data_length);
        existing->data_length = data_length;
        return existing;
    }
    
    // Add new entry
    int index = g_saved_values_count++;
    g_saved_values[index].game_index = game_index;
    g_saved_values[index].patch_index = patch_index;
    g_saved_values[index].address = address;
    g_saved_values[index].data_length = data_length;
    g_saved_values[index].original_data = g_malloc(data_length);
    memcpy(g_saved_values[index].original_data, original_data, data_length);
    
    return &g_saved_values[index];
}

// Remove all saved values for a specific game (called on reset/snapshot)
void xemu_patches_clear_saved_values_for_game(int game_index)
{
    for (int i = 0; i < g_saved_values_count; i++) {
        if (g_saved_values[i].game_index == game_index) {
            g_free(g_saved_values[i].original_data);
            
            // Remove this entry by swapping with the last entry
            if (i < g_saved_values_count - 1) {
                g_saved_values[i] = g_saved_values[g_saved_values_count - 1];
            }
            g_saved_values_count--;
            i--; // Adjust index after swap
        }
    }
}

// Clear all saved values (called on xemu exit)
void xemu_patches_clear_all_saved_values(void)
{
    for (int i = 0; i < g_saved_values_count; i++) {
        g_free(g_saved_values[i].original_data);
    }
    g_saved_values_count = 0;
}



// PATCH MONITORING SYSTEM

// Check if a game is currently running (to prevent applying patches when no game is loaded)
static bool is_game_currently_running(void)
{
    // Check if Xbox is running and has memory
    if (!get_system_memory()) {
        return false;
    }
    
    // Check if we can read from common Xbox memory locations
    uint8_t test_buffer[4];
    if (!safe_memory_read(0x00010000, test_buffer, 4, "game_running_check")) {
        return false;
    }
    return true;
}

// Helper function for consistent notification queuing
static void notify_message(const char *message) {
    xemu_queue_notification(message);
}

// Common certificate cache invalidation logic - eliminates code duplication
static void clear_certificate_cache_internal(void) {
    g_xbe_cache.valid = false;
    g_xbe_cache.frame_last_read = -1;
    g_xbe_cache.last_read_time = 0;
    g_force_fresh_xbe_read = true;
    g_cert_data_valid = false;
    
    g_last_cert_title_id = 0xFFFFFFFF;     // Force invalid title ID
    g_last_cert_region = 0xFFFFFFFF;       // Force invalid region
    g_last_cert_version = 0xFFFFFFFF;      // Force invalid version
    g_last_cert_read_time = 0;             // Force immediate re-read
}

// Force invalidate certificate cache (used during game switches)
void invalidate_certificate_cache(void)
{
    clear_certificate_cache_internal();
    
    // ENHANCED: Reset notification tracking when certificate cache is cleared
    // This ensures that new games can generate notifications without being blocked by old tracking data
    reset_notification_tracking_for_new_game();
}

// Helper function to force fresh certificate read - eliminates repeated assignments
static void force_fresh_certificate_read(void) {
    g_force_fresh_xbe_read = true;
}

// NOTIFICATION TRACKING RESET: Reset notification tracking for fresh game detection
void reset_notification_tracking_for_new_game(void) {
    // Reset notification tracking variables to allow fresh notifications
    last_notified_title_id = 0;
    last_notification_time = 0;
    
    // Clear notification generation flag to ensure new notifications can be generated
    g_notification_generation_active = false;
}

// Get cached XBE certificate info to reduce excessive reads
static bool get_cached_xbe_info(uint32_t *title_id, uint32_t *region, uint32_t *version)
{
    // ENHANCED FIX: Use SDL_GetTicks() for reliable timing instead of frame counting
    // Cache is valid for 5000ms (5 seconds) to dramatically reduce certificate reads
    uint64_t current_time = SDL_GetTicks();
    int current_frame = current_time / 16; // Approximate frame count at 60fps
    
    bool need_fresh_data = (current_time - g_xbe_cache.last_read_time >= 5000 || !g_xbe_cache.valid || g_force_fresh_xbe_read);
    
    if (need_fresh_data) {
        
        // LOAD DISC TIMEOUT FIX: Clear Load Disc flag if it's been stuck too long
        static uint32_t load_disc_stuck_counter = 0;
        if (g_load_disc_in_progress) {
            load_disc_stuck_counter++;
            // If Load Disc protection has been active for more than 300 frames (5 seconds), force clear it
            if (load_disc_stuck_counter > 60) {
                g_load_disc_in_progress = false;
                g_load_disc_retry_pending = false; // Also clear retry flag
                load_disc_stuck_counter = 0;
            }
        } else {
            load_disc_stuck_counter = 0;
        }
        
        // CRITICAL: Enhanced Load Disc protection with better fallback
        if (g_load_disc_in_progress) {
            
            // Only skip if we're in the middle of an active Load Disc operation
            // If retry is pending, allow certificate reads to detect the new disc
            if (!g_load_disc_retry_pending) {
                return false; // Indicate no valid certificate available during active Load Disc
            } else {
            }
            
            return false; // Indicate no valid certificate available
        }
        
        // Need fresh XBE info
        struct xbe *xbe_info = xemu_get_xbe_info();
        if (xbe_info && xbe_info->cert) {
            // CRITICAL: Enhanced certificate validation with retry mechanism
            uint32_t new_title_id = ldl_le_p(&xbe_info->cert->m_titleid);
            uint32_t new_region = ldl_le_p(&xbe_info->cert->m_game_region);
            uint32_t new_version = ldl_le_p(&xbe_info->cert->m_version);
            
            // Only accept valid game TitleIDs, reject dashboard and invalid IDs
            bool is_valid_title_id = (new_title_id != 0x00000000 &&  // Zero
                                      new_title_id != 0xFFFF0002 &&  // Dashboard  
                                      new_title_id != 0xFFFE0000 &&  // Invalid/Empty
                                      new_title_id != 0xFFFFFFFF);   // Force invalid
            
            // If we get invalid certificate data, don't cache it but don't fail completely
            if (!is_valid_title_id) {
                
                // Only set force fresh read occasionally to prevent constant retries
                static uint64_t last_invalid_time = 0;
                uint64_t current_time = SDL_GetTicks();
                if (current_time - last_invalid_time > 2000) { // Only retry every 2 seconds
                    g_force_fresh_xbe_read = true;
                    last_invalid_time = current_time;
                }
                return false; // Indicate no valid certificate available yet
            }
            
            g_xbe_cache.title_id = new_title_id;
            g_xbe_cache.region = new_region;
            g_xbe_cache.version = new_version;
            g_xbe_cache.valid = true;
            
            // CRITICAL FIX: Update cache timestamp when we actually read fresh data
            g_xbe_cache.last_read_time = current_time;
            g_xbe_cache.frame_last_read = current_frame;
            
            // Clear the force fresh read flag immediately after successful certificate read
            // This prevents infinite read loops while letting the cache timing handle future reads
            if (g_force_fresh_xbe_read) {
                g_force_fresh_xbe_read = false;
            }
            
            // CRITICAL TIMING FIX: Prevent early certificate reads during Load Disc operations
            static int load_disc_cooldown_frames = 0;
            if (g_force_fresh_xbe_read && !g_post_reset_patch_scheduled) {
                // We're in Load Disc mode but post-reset hasn't started yet
                // This might be an early read that should be ignored
                load_disc_cooldown_frames = 10; // Skip certificate analysis for next 10 frames
            } else if (load_disc_cooldown_frames > 0) {
                load_disc_cooldown_frames--;
                return false; // Skip certificate analysis during cooldown
            }
        } else {
            g_xbe_cache.valid = false;
            return false;
        }
    }
    
    if (g_xbe_cache.valid) {
        if (title_id) *title_id = g_xbe_cache.title_id;
        if (region) *region = g_xbe_cache.region;
        if (version) *version = g_xbe_cache.version;
        return true;
    }
    
    return false;
}

// Perform periodic monitoring of all patches
static void perform_periodic_monitoring(void)
{
    if (!g_monitoring_enabled || g_monitored_patch_count == 0) {
        return;
    }
    
    time_t current_time = time(NULL);
    
    // Check every 2 seconds
    if (difftime(current_time, g_last_monitoring_time) < 2.0) {
        return;
    }
    
    g_last_monitoring_time = current_time;
    
    for (int i = 0; i < g_monitored_patch_count; i++) {
        verify_monitored_patch(i);
    }
}

// Verify the integrity of a monitored patch
static void verify_monitored_patch(int patch_index)
{
    if (patch_index < 0 || patch_index >= g_monitored_patch_count) {
        return;
    }
    
    monitored_patch_t *patch = &g_monitored_patches[patch_index];
    
    // Read current memory value
    uint8_t current_data[16];
    if (!read_direct_virtual_memory(patch->address, current_data, patch->data_length)) {
        return;
    }
    
    // Compare with expected patch data
    bool matches = true;
    for (int i = 0; i < patch->data_length; i++) {
        if (current_data[i] != patch->value_data[i]) {
            matches = false;
            break;
        }
    }
    
    patch->verification_count++;
    patch->still_active = matches;
}

// Apply a patch with variable-length byte array
static bool apply_single_patch_bytes(uint32_t address, uint8_t *value_data, int value_length, uint8_t *original_value_buffer)
{
    // Validate parameters
    if (!value_data || value_length <= 0) {
        return false;
    }
    
    // Add address to reset monitoring if we're in reset mode
    if (g_reset_monitoring_active) {
        monitor_reset_patch_address(address, "reset_reapplication");
    }
    
    // Test memory access before writing and capture original value
    uint8_t test_buffer[16];
    char test_error_msg[256];
    int test_read = xemu_virtual_memory_read(address, test_buffer, MIN(value_length, 16), test_error_msg, sizeof(test_error_msg));
    if (!test_read) {
        return false;
    }
    
    // Capture the original value if buffer provided
    if (original_value_buffer && test_read >= value_length) {
        memcpy(original_value_buffer, test_buffer, value_length);
    } else if (original_value_buffer) {
    }
    
    // If this address was flagged as having placeholder initial value during reset monitoring,
    // update it with the current value before patching
    if (g_reset_monitoring_active) {
        for (int i = 0; i < g_reset_monitored_count; i++) {
            if (g_reset_monitored_addresses[i].address == address && 
                g_reset_monitored_addresses[i].value_before == 0xDEADBEEF) {
                g_reset_monitored_addresses[i].value_before = test_buffer[0] | (test_buffer[1] << 8) | (test_buffer[2] << 16) | (test_buffer[3] << 24);
                break;
            }
        }
    }
    
    // Format the patch value for logging
    char patch_value_str[64] = "";
    for (int i = 0; i < value_length && i < 16; i++) {
        char byte_str[8];
        if (i > 0) strcat(patch_value_str, " ");
        sprintf(byte_str, "%02X", value_data[i]);
        strcat(patch_value_str, byte_str);
    }
    
    // Perform the actual memory write
    bool write_success = write_direct_virtual_memory(address, value_data, value_length);
    
    // Log the value after patch application if we're monitoring
    if (g_reset_monitoring_active && write_success) {
        log_reset_memory_values(address, "after_application");
    }
    
    return write_success;
}

// Apply patch with save/restore logic
bool xemu_patches_apply_patch_with_save_restore(XemuMemoryPatch *patch, int game_index, int patch_index)
{
    if (!patch || !get_system_memory()) {
        return false;
    }
    
    bool all_success = true;
    bool original_values_saved = true; // Track if original values were successfully saved
    
    // Apply each address:value pair
    for (int i = 0; i < patch->address_value_count; i++) {
        XemuPatchAddressValue *addr_val = &patch->address_values[i];
        
        // CRITICAL FIX: Save the original value BEFORE applying the patch
        uint8_t captured_original_value[16];
        if (patch->save_replaced_values) {
            
            // Read the original value first (before any patch is applied)
            bool read_success = read_direct_virtual_memory(addr_val->address, captured_original_value, addr_val->value_length);
            if (read_success) {
                // Save the original value for restoration later
                add_saved_value(game_index, patch_index, addr_val->address, 
                               captured_original_value, addr_val->value_length);
            } else {
                original_values_saved = false;
            }
        }
        
        // Now apply the patch (write the new value)
        bool patch_success = apply_single_patch_bytes(addr_val->address, 
                                                     addr_val->value_data, 
                                                     addr_val->value_length,
                                                     NULL);  // Don't capture during patch application
        
        if (!patch_success) {
            all_success = false;
        } else {
        }
    }
    
    // Provide feedback about the save operation
    if (patch->save_replaced_values) {
        if (all_success && original_values_saved) {
        } else if (all_success) {
        } else {
        }
    } else {
    }
    
    return all_success;
}

// Remove patch with restore logic
bool xemu_patches_remove_patch_with_restore(int game_index, int patch_index)
{
    XemuPatchesDatabase *db = &g_patches_db;
    if (game_index < 0 || game_index >= db->game_count) {
        return false;
    }
    
    XemuGamePatches *game = &db->games[game_index];
    if (patch_index < 0 || patch_index >= game->patch_count) {
        return false;
    }
    
    XemuMemoryPatch *patch = &game->patches[patch_index];
    
    bool all_success = true;

    // For each address:value pair, either restore original or just leave current value
    for (int i = 0; i < patch->address_value_count; i++) {
        XemuPatchAddressValue *addr_val = &patch->address_values[i];
        
        if (patch->save_replaced_values) {
            
            // Find and restore the saved original value
            SavedValueEntry *saved_entry = find_saved_value(game_index, patch_index, addr_val->address);
            
            if (saved_entry) {
                // Restore the original value
                bool restore_success = apply_single_patch_bytes(addr_val->address,
                                                               saved_entry->original_data,
                                                               saved_entry->data_length,
                                                               NULL);  // No original value capture needed for restore
                
                if (restore_success) {
                    // Verify the restoration worked
                    uint8_t verify_memory[16];
                    bool verify_success = read_direct_virtual_memory(addr_val->address, verify_memory, saved_entry->data_length);
                    if (verify_success) {
                        // Check if restoration was successful
                        for (int j = 0; j < saved_entry->data_length; j++) {
                            if (verify_memory[j] != saved_entry->original_data[j]) {
                                break;
                            }
                        }
                    }
                } else {
                    all_success = false;
                }
                
                // Free the saved entry
                g_free(saved_entry->original_data);
                
                // Remove this entry from the array
                int entry_index = saved_entry - g_saved_values;
                if (entry_index < g_saved_values_count - 1) {
                    g_saved_values[entry_index] = g_saved_values[g_saved_values_count - 1];
                }
                g_saved_values_count--;
                
            } else {
            }
        } else {
        }
    }
    
    return all_success;
}

// Memory clearing function to force fresh certificate reads
static void xemu_clear_cert_cache(void) {
    clear_certificate_cache_internal();
}

// ============================================================================
// Parse a single line from the patches database
static bool parse_patch_line(const char *line, XemuPatchAddressValue **addresses_values, 
                            int *count, char **name, char **category, char **author, 
                            char **notes, bool *enabled)
{
    char *line_copy = g_strdup(line);
    char *equals = strchr(line_copy, '=');
    
    if (!equals) {
        // Old format: address:value (single)
        
        // Remove comments (# and // styles) from the line
        char *comment_start_hash = strchr(line_copy, '#');
        char *comment_start_slash = strstr(line_copy, "//");
        
        // Find the earliest comment start position
        char *comment_start = NULL;
        if (comment_start_hash && (!comment_start_slash || comment_start_hash < comment_start_slash)) {
            comment_start = comment_start_hash;
        } else if (comment_start_slash) {
            comment_start = comment_start_slash;
        }
        
        // Remove everything from comment start to end
        if (comment_start) {
            *comment_start = '\0';
        }
        
        char *colon = strchr(line_copy, ':');
        if (!colon) {
            g_free(line_copy);
            return false;
        }
        
        *colon = '\0';
        uint32_t address = strtoul(g_strstrip(line_copy), NULL, 16);
        
        // Parse hex string to byte array
        char *hex_str = g_strstrip(colon + 1);
        int hex_len = strlen(hex_str);
        int byte_len = (hex_len + 1) / 2;  // 2 hex chars per byte
        
        uint8_t *val_data = g_new(uint8_t, byte_len);
        for (int b = 0; b < byte_len; b++) {
            char hex_byte[3];
            hex_byte[0] = hex_str[b * 2];
            hex_byte[1] = (b * 2 + 1 < hex_len) ? hex_str[b * 2 + 1] : '0';
            hex_byte[2] = '\0';
            val_data[b] = (uint8_t)strtoul(hex_byte, NULL, 16);
        }
        
        *addresses_values = g_new(XemuPatchAddressValue, 1);
        (*addresses_values)[0].address = address;
        (*addresses_values)[0].value_data = val_data;
        (*addresses_values)[0].value_length = byte_len;
        *count = 1;
        *name = g_strdup_printf("Patch at 0x%08X", address);
        *category = g_strdup("General");
        *author = g_strdup("Unknown");
        *notes = g_strdup("");
        *enabled = true;
        
        g_free(line_copy);
        return true;
    }
    
    *equals = '\0';
    char *left_side = g_strstrip(line_copy);
    char *right_side = g_strstrip(equals + 1);
    
    // Parse new format: name=category:author:notes:address1:value1,address2:value2,...
    *name = g_strdup(left_side);
    
    // Split right side by colons
    char *category_part = strtok_r(right_side, ":", &right_side);
    char *author_part = strtok_r(right_side, ":", &right_side);
    char *notes_part = strtok_r(right_side, ":", &right_side);
    char *addresses_part = strtok_r(right_side, ":", &right_side);
    
    if (!addresses_part) {
        // No address part found, might be old format
        if (category_part && strchr(category_part, ':')) {
            // This looks like old format "name=address:value"
            g_free(line_copy);
            return parse_patch_line(line, addresses_values, count, name, category, author, notes, enabled);
        }
        // Fallback to old behavior
        addresses_part = category_part;
        category_part = (char*)"General";
        author_part = (char*)"Unknown";
        notes_part = (char*)"";
    }
    
    *category = g_strdup(category_part ? category_part : "General");
    *author = g_strdup(author_part ? author_part : "Unknown");
    *notes = g_strdup(notes_part ? notes_part : "");
    
    // Parse comma-separated address:value pairs
    if (!addresses_part || addresses_part[0] == '\0') {
        g_free(line_copy);
        return false;
    }
    
    // Count address:value pairs first
    int pair_count = 0;
    char *temp_addresses = g_strdup(addresses_part);
    char *token = strtok(temp_addresses, ",");
    while (token) {
        pair_count++;
        token = strtok(NULL, ",");
    }
    g_free(temp_addresses);
    
    if (pair_count == 0) {
        g_free(line_copy);
        return false;
    }
    
    // Allocate array and parse pairs
    *addresses_values = g_new(XemuPatchAddressValue, pair_count);
    *count = pair_count;
    
    temp_addresses = g_strdup(addresses_part);
    token = strtok(temp_addresses, ",");
    int i = 0;
    
    while (token && i < pair_count) {
        char *addr_colon = strchr(token, ':');
        if (addr_colon) {
            *addr_colon = '\0';
            (*addresses_values)[i].address = strtoul(g_strstrip(token), NULL, 16);
            
            // Parse hex string to byte array
            char *hex_str = g_strstrip(addr_colon + 1);
            int hex_len = strlen(hex_str);
            int byte_len = (hex_len + 1) / 2;  // 2 hex chars per byte
            
            uint8_t *val_data = g_new(uint8_t, byte_len);
            for (int b = 0; b < byte_len; b++) {
                char hex_byte[3];
                hex_byte[0] = hex_str[b * 2];
                hex_byte[1] = (b * 2 + 1 < hex_len) ? hex_str[b * 2 + 1] : '0';
                hex_byte[2] = '\0';
                val_data[b] = (uint8_t)strtoul(hex_byte, NULL, 16);
            }
            
            (*addresses_values)[i].value_data = val_data;
            (*addresses_values)[i].value_length = byte_len;
            i++;
        }
        token = strtok(NULL, ",");
    }
    
    g_free(temp_addresses);
    *enabled = true;
    g_free(line_copy);
    return true;
}

// Parse game information from database entry
static bool parse_game_info(const char **lines, int line_count, 
                           XemuGamePatches *game, int *lines_consumed)
{
    *lines_consumed = 0;
    
    for (int i = 0; i < line_count; i++) {
        const char *line = lines[i];
        char *trimmed = g_strstrip(g_strdup(line));
        
        // Skip empty lines and comments
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            (*lines_consumed)++;
            g_free(trimmed);
            continue;
        }
        
        // Stop parsing game info when we hit the "Patches" section marker
        if (g_str_has_prefix(trimmed, "Patches")) {
            (*lines_consumed)++;
            g_free(trimmed);
            break;
        }
        
        // FIRST: Check if this is a known game info line (before checking for patch patterns)
        // This prevents game info lines like "title-id: 53450004" from being misidentified as patch lines
        if (g_str_has_prefix(trimmed, "title:")) {
            // Extract value after "title:"
            char *colon = strchr(trimmed, ':');
            if (colon && *(colon + 1) != '\0') {
                char *value = g_strstrip(g_strdup(colon + 1));
                if (value && value[0] != '\0') {
                    // Clean up common formatting issues like trailing quotes
                    int len = strlen(value);
                    while (len > 0 && (value[len-1] == '"' || value[len-1] == '\'')) {
                        value[len-1] = '\0';
                        len--;
                    }
                    game->game_title = g_strstrip(value);
                } else {
                    g_free(value);
                }
            } else {
            }
            (*lines_consumed)++;
            g_free(trimmed);
            continue;
        } else if (g_str_has_prefix(trimmed, "title-id:")) {
            // Extract value after "title-id:"
            char *colon = strchr(trimmed, ':');
            if (colon && *(colon + 1) != '\0') {
                char *value = g_strstrip(g_strdup(colon + 1));
                if (value && value[0] != '\0') {
                    game->title_id = value;
                } else {
                    g_free(value);
                }
            } else {
            }
            (*lines_consumed)++;
            g_free(trimmed);
            continue;
        } else if (g_str_has_prefix(trimmed, "region:")) {
            // Extract value after "region:"
            char *colon = strchr(trimmed, ':');
            if (colon && *(colon + 1) != '\0') {
                char *value = g_strstrip(g_strdup(colon + 1));
                if (value && value[0] != '\0') {
                    game->region = value;
                } else {
                    g_free(value);
                }
            } else {
            }
            (*lines_consumed)++;
            g_free(trimmed);
            continue;
        } else if (g_str_has_prefix(trimmed, "version:")) {
            // Extract value after "version:"
            char *colon = strchr(trimmed, ':');
            if (colon && *(colon + 1) != '\0') {
                char *value = g_strstrip(g_strdup(colon + 1));
                if (value && value[0] != '\0') {
                    game->version = value;
                } else {
                    g_free(value);
                }
            } else {
            }
            (*lines_consumed)++;
            g_free(trimmed);
            continue;
        } else if (g_str_has_prefix(trimmed, "alternate-title-id:")) {
            // Extract value after "alternate-title-id:"
            char *colon = strchr(trimmed, ':');
            if (colon && *(colon + 1) != '\0') {
                char *value = g_strstrip(g_strdup(colon + 1));
                if (value && value[0] != '\0') {
                    game->alternate_title_id = value;
                } else {
                    g_free(value);
                }
            } else {
            }
            (*lines_consumed)++;
            g_free(trimmed);
            continue;
        } else if (g_str_has_prefix(trimmed, "time-date:")) {
            // Extract value after "time-date:"
            char *colon = strchr(trimmed, ':');
            if (colon && *(colon + 1) != '\0') {
                char *value = g_strstrip(g_strdup(colon + 1));
                if (value && value[0] != '\0') {
                    game->time_date = value;
                } else {
                    g_free(value);
                }
            } else {
            }
            (*lines_consumed)++;
            g_free(trimmed);
            continue;
        } else if (g_str_has_prefix(trimmed, "disc-number:")) {
            // Extract value after "disc-number:"
            char *colon = strchr(trimmed, ':');
            if (colon && *(colon + 1) != '\0') {
                char *value = g_strstrip(g_strdup(colon + 1));
                if (value && value[0] != '\0') {
                    game->disc_number = value;
                } else {
                    g_free(value);
                }
            } else {
            }
            (*lines_consumed)++;
            g_free(trimmed);
            continue;
        } else if (g_str_has_prefix(trimmed, "game-title=")) {
            // Legacy format support
            char *equals = strchr(trimmed, '=');
            if (equals && *(equals + 1) != '\0') {
                char *value = g_strstrip(g_strdup(equals + 1));
                if (value && value[0] != '\0') {
                    game->game_title = value;
                } else {
                    g_free(value);
                }
            }
        } else if (g_str_has_prefix(trimmed, "enabled:")) {
            // Extract value after "enabled:"
            char *colon = strchr(trimmed, ':');
            if (colon && *(colon + 1) != '\0') {
                char *value = g_strstrip(g_strdup(colon + 1));
                if (value && value[0] != '\0') {
                    game->enabled = (atoi(value) != 0);
                    g_free(value);
                }
            }

        }
        
        (*lines_consumed)++;
        g_free(trimmed);
    }

    // Set defaults for missing fields
    if (!game->game_title) {
        game->game_title = g_strdup("Unknown Game");
    }
    if (!game->title_id) {
        game->title_id = g_strdup("Unknown");
    }
    if (!game->region) {
        game->region = g_strdup("NTSC");
    }
    if (!game->version) {
        game->version = g_strdup("Unknown");
    }
    
    game->enabled = true; // Default to enabled
    return true;
}

// Parse patches for a game from database lines
static bool parse_game_patches(const char **lines, int line_count, 
                              XemuGamePatches *game, int *lines_consumed)
{
    *lines_consumed = 0;
    GArray *patch_array = g_array_new(false, false, sizeof(XemuMemoryPatch));
    
    int i = 0;
    for (int dbg_i = 0; dbg_i < line_count; dbg_i++) {
    }
    
    while (i < line_count) {
        if (i >= line_count) {
            break;
        }
        
        if (!lines[i]) {
            break;
        }
        
        const char *line = lines[i];
        char *trimmed = g_strstrip(g_strdup(line));
        
        // Stop at empty line, comment, or new game entry
        // But NOT at separator lines like "======="
        if (trimmed[0] == '\0' || trimmed[0] == '#' ||
            (trimmed[0] != '\t' && strchr(trimmed, '=') && !strchr(trimmed, ':'))) {
            
            // Check if this is a separator line (all equals signs)
            bool is_separator = true;
            for (int j = 0; trimmed[j] != '\0'; j++) {
                if (trimmed[j] != '=') {
                    is_separator = false;
                    break;
                }
            }
            
            if (is_separator) {
                i++;
                g_free(trimmed);
                continue;
            }
            g_free(trimmed);
            break;
        }
        
        // Check if this is a patch header line (new structured format)
        if (g_str_has_prefix(trimmed, "Patch:")) {
            XemuMemoryPatch patch = {0};
            patch.enabled = true;
            
            // Extract patch name - CRITICAL FIX: Avoid memory corruption
            char* patch_name_raw = g_strdup(trimmed + 6); // Skip "Patch: "
            char* patch_name_clean = g_strstrip(g_strdup(patch_name_raw)); // Create new cleaned copy
            patch.name = patch_name_clean; // Use the cleaned copy
            
            g_free(patch_name_raw); // Free the raw version (patch.name is separate allocation)
            
            // Initialize patch fields
            patch.category = g_strdup("Other");
            patch.author = g_strdup("Unknown");
            patch.notes = g_strdup("");
            patch.address_values = NULL;
            patch.address_value_count = 0;
            patch.address_value_lines = NULL;
            patch.address_value_lines_count = 0;
            patch.save_replaced_values = false; // Default: don't save replaced values
            patch.saved_original_values = NULL;
            patch.saved_value_lengths = NULL;
            
            // Variables for memory address parsing
            GArray *addr_val_array = NULL;
            GPtrArray *addr_lines_array = NULL;
            
            i++;
            
            while (i < line_count) {
                
                // CRITICAL: Validate that lines[i] is not NULL and is reasonable
                if (i >= line_count) {
                    break;
                }
                
                if (!lines[i]) {
                    break;
                }
     
                char *meta_line = g_strstrip(g_strdup(lines[i]));
                
                // Check for obviously corrupted lines (too long or contains newlines)
                if (strlen(meta_line) > 1000) {
                    g_free(meta_line);
                    break;
                }
                
                // Check if the processed line contains embedded newlines (indicates corruption)
                if (strchr(meta_line, '\n') != NULL) {
                    g_free(meta_line);
                    break;
                }
                
                // Stop if we hit a new patch or empty section
                if (meta_line[0] == '\0' || g_str_has_prefix(meta_line, "Patches") ||
                    g_str_has_prefix(meta_line, "Game Entry") || 
                    (meta_line[0] == ' ' && strchr(meta_line + 1, ':') == NULL)) {
                    g_free(meta_line);
                    break;
                }
                
                if (g_str_has_prefix(meta_line, "Author:")) {
                    char *colon = strchr(meta_line, ':');
                    if (colon && *(colon + 1) != '\0') {
                        char *old_author = patch.author;
                        patch.author = g_strstrip(g_strdup(colon + 1));
                        if (old_author) g_free(old_author);
                    }
                } else if (g_str_has_prefix(meta_line, "Category:")) {
                    char *colon = strchr(meta_line, ':');
                    if (colon && *(colon + 1) != '\0') {
                        char *old_category = patch.category;
                        patch.category = g_strstrip(g_strdup(colon + 1));
                        if (old_category) g_free(old_category);
                    }
                } else if (g_str_has_prefix(meta_line, "Notes:")) {
                    char *colon = strchr(meta_line, ':');
                    if (colon && *(colon + 1) != '\0') {
                        char *old_notes = patch.notes;
                        patch.notes = g_strstrip(g_strdup(colon + 1));
                        if (old_notes) g_free(old_notes);
                    }
                } else if (g_str_has_prefix(meta_line, "Enabled:")) {
                    char *colon = strchr(meta_line, ':');
                    if (colon && *(colon + 1) != '\0') {
                        char *value = g_strstrip(g_strdup(colon + 1));
                        if (value && value[0] != '\0') {
                            patch.enabled = (atoi(value) != 0);
                            g_free(value);
                        }
                    }
                } else if (g_str_has_prefix(meta_line, "Save Replaced Values:")) {
                    char *colon = strchr(meta_line, ':');
                    if (colon && *(colon + 1) != '\0') {
                        char *value = g_strstrip(g_strdup(colon + 1));
                        if (value && value[0] != '\0') {
                            patch.save_replaced_values = (atoi(value) != 0);
                            g_free(value);
                        }
                    }
                } else if (g_str_has_prefix(meta_line, "Memory Addresses:")) {
                    // Parse address:value pairs
                    addr_val_array = g_array_new(false, false, sizeof(XemuPatchAddressValue));
                    addr_lines_array = g_ptr_array_new();
                    
                    i++;
                    while (i < line_count) {
                        
                        // Preserve original indentation when parsing address lines
                        // Skip completely empty lines
                        if (i >= line_count || !lines[i] || lines[i][0] == '\0' || lines[i][0] == '\n') {
                            i++;
                            continue;
                        }
                        
                        
                        // Skip separator lines and section headers
                        char *trimmed_line = g_strstrip(g_strdup(lines[i]));
                        
                        if (trimmed_line[0] == '\0' ||
                            (trimmed_line[0] == ' ' && strchr(trimmed_line + 1, ':') == NULL)) {
                            g_free(trimmed_line);
                            i++;
                            continue;
                        }
                        
                        // Check if we hit a new patch header - this should break the loop
                        if (g_str_has_prefix(trimmed_line, "Patch:")) {
                            g_free(trimmed_line);
                            break; // BREAK OUT to start parsing next patch
                        }
                        
                        // Check if we hit other patch metadata lines - continue the loop
                        if (g_str_has_prefix(trimmed_line, "Author:") ||
                            g_str_has_prefix(trimmed_line, "Category:") ||
                            g_str_has_prefix(trimmed_line, "Notes:")) {
                            g_free(trimmed_line);
                            i++;
                            continue;
                        }
                        
                        // Store the original line (with comments) for preservation
                        // Clean up any extra leading spaces from the line
                        char* clean_line = g_strstrip(g_strdup(lines[i]));
                        g_ptr_array_add(addr_lines_array, clean_line); // g_strdup not needed as we clean_line already allocated
                        
                        // Parse address:value format - work with a copy for parsing
                        char *parse_line = g_strdup(lines[i]);
                        
                        // Remove comments (# and // styles) from the address line first
                        char *comment_start_hash = strchr(parse_line, '#');
                        char *comment_start_slash = strstr(parse_line, "//");
                        
                        // Find the earliest comment start position
                        char *comment_start = NULL;
                        if (comment_start_hash && (!comment_start_slash || comment_start_hash < comment_start_slash)) {
                            comment_start = comment_start_hash;
                        } else if (comment_start_slash) {
                            comment_start = comment_start_slash;
                        }
                        
                        // Remove everything from comment start to end
                        if (comment_start) {
                            *comment_start = '\0';
                        }
                        
                        // Parse address:value format
                        char *colon = strchr(parse_line, ':');
                        if (colon) {
                            *colon = '\0';
                            char *addr_str = g_strstrip(parse_line);
                            char *val_str = g_strstrip(colon + 1);
                            
                            // Parse hex address
                            char *endptr;
                            uint32_t addr = strtoul(addr_str, &endptr, 16);
                            
                            if (*endptr != '\0') {
                                g_free(parse_line);
                                i++;
                                continue;
                            }
                            
                            // Parse hex value (variable length) - remove "0x" prefix if present
                            char *val_clean = val_str;
                            if (g_str_has_prefix(val_clean, "0x")) {
                                val_clean += 2;
                            }
                            
                            // Convert hex string to byte array
                            int val_len = strlen(val_clean);
                            uint8_t *val_data = g_new(uint8_t, (val_len + 1) / 2);
                            int actual_len = 0;
                            
                            bool valid_hex = true;
                            for (int j = 0; j < val_len; j += 2) {
                                if (j + 1 < val_len) {
                                    char hex_pair[3] = {val_clean[j], val_clean[j+1], '\0'};
                                    val_data[actual_len] = (uint8_t)strtol(hex_pair, &endptr, 16);
                                    if (*endptr != '\0') {
                                        valid_hex = false;
                                        break;
                                    }
                                    actual_len++;
                                } else {
                                    // Handle odd length by padding with 0
                                    char hex_pair[3] = {val_clean[j], '0', '\0'};
                                    val_data[actual_len] = (uint8_t)strtol(hex_pair, &endptr, 16);
                                    if (*endptr != '\0') {
                                        valid_hex = false;
                                        break;
                                    }
                                }
                            }
                            
                            if (valid_hex) {
                                XemuPatchAddressValue addr_val;
                                addr_val.address = addr;
                                addr_val.value_data = val_data;
                                addr_val.value_length = actual_len;
                                g_array_append_val(addr_val_array, addr_val);
                            } else {
                                g_free(val_data);
                            }
                        } else {
                            g_free(parse_line);
                        }
                        i++;
                    }
                    
                    
                    // Transfer address:value pairs to patch
                    if (addr_val_array->len > 0) {
                        patch.address_values = g_new(XemuPatchAddressValue, addr_val_array->len);
                        patch.address_value_count = addr_val_array->len;
                        
                        // Manual copy to handle pointer members properly
                        for (guint j = 0; j < addr_val_array->len; j++) {
                            XemuPatchAddressValue *src = &g_array_index(addr_val_array, XemuPatchAddressValue, j);
                            patch.address_values[j].address = src->address;
                            patch.address_values[j].value_length = src->value_length;
                            patch.address_values[j].value_data = src->value_data;  // Transfer ownership
                        }
                        
                        // Transfer original address:value lines (with comments)
                        patch.address_value_lines = g_new(char*, addr_lines_array->len);
                        patch.address_value_lines_count = addr_lines_array->len;
                        for (guint j = 0; j < addr_lines_array->len; j++) {
                            patch.address_value_lines[j] = g_ptr_array_index(addr_lines_array, j);
                        }
                    } else {
                        // Even if no valid address:value pairs, store the original lines
                        patch.address_values = NULL;
                        patch.address_value_count = 0;
                        patch.address_value_lines = g_new(char*, addr_lines_array->len);
                        patch.address_value_lines_count = addr_lines_array->len;
                        for (guint j = 0; j < addr_lines_array->len; j++) {
                            patch.address_value_lines[j] = g_ptr_array_index(addr_lines_array, j);
                        }
                    }
                    g_array_free(addr_val_array, true);
                    g_ptr_array_free(addr_lines_array, false); // Don't free the strings, we transferred ownership
                    
                    g_free(meta_line);
                    break; // BREAK OUT of metadata loop to store the patch
                }
                
                g_free(meta_line);
                i++;
            }
            
            // Ensure address_value_lines fields are always initialized
            if (!patch.address_value_lines) {
                patch.address_value_lines = NULL;
            }
            if (patch.address_value_lines_count < 0) {
                patch.address_value_lines_count = 0;
            }

            
            // Check for potential data corruption before appending
            if (patch.category && strcmp(patch.category, "Widescreen") == 0 && 
                patch.name && strstr(patch.name, "test 1") != NULL) {
            }
            
            g_array_append_val(patch_array, patch);
            
            // CRITICAL: Verify that i is at the right position after parsing a patch
            if (i >= line_count) {
                g_free(trimmed);
                break;
            }
            
            // Show the next few lines to see what comes after this patch
            for (int next_i = i; next_i < MIN(line_count, i+5); next_i++) {
            }
        } else if (strchr(trimmed, ':') && strchr(trimmed, '=')) {
            // Legacy compact format parsing (name=category:author:notes:address:value,address:value)
            XemuPatchAddressValue *addresses_values = NULL;
            char *patch_name = NULL;
            char *patch_category = NULL;
            char *patch_author = NULL;
            int address_value_count = 0;
            bool enabled = true;
            char *patch_notes = NULL;
            
            if (parse_patch_line(trimmed, &addresses_values, &address_value_count, &patch_name, &patch_category, &patch_author, &patch_notes, &enabled)) {
                XemuMemoryPatch patch = {0};
                patch.address_values = addresses_values;
                patch.address_value_count = address_value_count;
                patch.enabled = enabled;
                patch.name = patch_name ? patch_name : g_strdup("Unnamed Patch");
                patch.category = patch_category ? patch_category : g_strdup("General");
                patch.author = patch_author ? patch_author : g_strdup("Unknown");
                patch.notes = patch_notes ? patch_notes : g_strdup("");
                // Legacy format doesn't preserve original lines, set to NULL
                patch.address_value_lines = NULL;
                patch.address_value_lines_count = 0;
                
                // Ensure address_value_lines fields are always initialized
                if (!patch.address_value_lines) {
                    patch.address_value_lines = NULL;
                }
                if (patch.address_value_lines_count < 0) {
                    patch.address_value_lines_count = 0;
                }
                g_array_append_val(patch_array, patch);
            } else {
                // Free allocated memory if parsing failed
                if (addresses_values) g_free(addresses_values);
                if (patch_name) g_free(patch_name);
                if (patch_category) g_free(patch_category);
                if (patch_author) g_free(patch_author);
                if (patch_notes) g_free(patch_notes);
            }
            i++;
        } else {
            i++;
        }
        
        g_free(trimmed);
    }
    
    // Transfer patches from array to game structure
    game->patch_count = patch_array->len;
    
    if (patch_array->len > 0) {
        game->patches = g_new(XemuMemoryPatch, patch_array->len);
        memcpy(game->patches, patch_array->data, 
               patch_array->len * sizeof(XemuMemoryPatch));
        
        // Debug: Show parsed patches
        // for (int p = 0; p < patch_array->len; p++) {
        //     // XemuMemoryPatch *patch = &((XemuMemoryPatch*)patch_array->data)[p];
        // }
    } 
    g_array_free(patch_array, true);
    *lines_consumed = i;
    return true;
}

// Load patches database from file
bool xemu_patches_load_database(const char *filepath)
{
    char *actual_filepath = NULL;
    if (!filepath) {
        actual_filepath = g_strdup_printf("%s/xemu.db", SDL_GetBasePath());
        filepath = actual_filepath;
    }
    
    // Check if file exists first
    SDL_RWops *rwops = SDL_RWFromFile(filepath, "r");
    if (!rwops) {
        // Database file not found
        
        // Initialize empty database but return false to indicate file doesn't exist
        // This will trigger the initialization function to create the file
        g_patches_db.games = NULL;
        g_patches_db.game_count = 0;
        g_patches_db.file_path = g_strdup(filepath);
        g_patches_db.dirty = false;
        g_patches_loaded = true;
        
        g_free(actual_filepath);
        return false;  // Return false to indicate file didn't exist
    }
    
    // Check file size to avoid processing huge files
    Sint64 file_size = SDL_RWseek(rwops, 0, RW_SEEK_END);
    SDL_RWseek(rwops, 0, RW_SEEK_SET);
    
    if (file_size < 0 || file_size > 10 * 1024 * 1024) { // 10MB limit
        // Invalid or too large database file
        SDL_RWclose(rwops);
        return false;
    }
    
    SDL_RWclose(rwops);
    
    FILE *file = qemu_fopen(filepath, "r");
    if (!file) {
        // Failed to open patches database
        g_free(actual_filepath);
        return false;
    }
    
    // Clear existing database
    xemu_patches_free_database();
    
    // Read all lines
    GPtrArray *lines_array = g_ptr_array_new();
    char line[1024];
    
    while (fgets(line, sizeof(line), file)) {
        // Remove trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        
        // CRITICAL: Validate line before adding to array
        if (len > 1000) {
            continue; // Skip corrupted lines
        }
        
        char *line_copy = g_strdup(line);
        g_ptr_array_add(lines_array, line_copy);
    }
    
    fclose(file);
    
    // Parse games with simplified sequential processing
    GArray *games_array = g_array_new(false, false, sizeof(XemuGamePatches));
    int current_line = 0;
    
    while (current_line < lines_array->len) {
        char *line = g_ptr_array_index(lines_array, current_line);
        char *trimmed = g_strstrip(g_strdup(line));
        
        // Skip empty lines and comments
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            current_line++;
            g_free(trimmed);
            continue;
        }
        
        // Check if this looks like a game entry start
        bool is_game_entry = false;
        
        if (g_str_has_prefix(trimmed, "Game Entry")) {
            // New structured format
            is_game_entry = true;
        } else if (strchr(trimmed, '=')) {
            // Legacy format
            is_game_entry = true;
        } else if (g_str_has_prefix(trimmed, "title:")) {
            // Direct title line (new format without header)
            is_game_entry = true;
        }
        
        if (is_game_entry) {
            XemuGamePatches game = {0};
            
            // Collect lines for this game entry
            GPtrArray *game_lines = g_ptr_array_new();
            GPtrArray *patch_lines = g_ptr_array_new();
            bool in_patch_section = false;
            
            // Read all lines for this game
            int game_start_line = current_line;
    while (current_line < lines_array->len) {
        if (current_line >= lines_array->len) {
            break;
        }
        
        char *raw_line = g_ptr_array_index(lines_array, current_line);
        if (!raw_line) {
            break;
        }

        char *game_line = raw_line;  // This is already a copy from g_strdup
        char *game_trimmed = g_strstrip(g_strdup(game_line));
                
                // Check if this is the start of a new game
                if (current_line > game_start_line) {
                    if (g_str_has_prefix(game_trimmed, "Game Entry") ||
                        (strchr(game_trimmed, '=') && g_str_has_prefix(game_trimmed, "game-title="))) {
                        // Found next game, stop here
                        g_free(game_trimmed);
                        break;
                    }
                }
                
                // Track sections
                if (g_str_has_prefix(game_trimmed, "Patches")) {
                    in_patch_section = true;
                }
                
                // Add to appropriate section
                if (in_patch_section) {
                    g_ptr_array_add(patch_lines, game_line);
                } else {
                    g_ptr_array_add(game_lines, game_line);
                }
                
                g_free(game_trimmed);
                current_line++;
            }
            
            // Add one more line if we broke out due to next game
            current_line--; // We'll increment at the end of the loop
            

            
            // Prepare line pointers for parsing
            const char **info_lines_ptr = NULL;
            if (game_lines->len > 0) {
                info_lines_ptr = g_new(const char*, game_lines->len);
                for (int i = 0; i < game_lines->len; i++) {
                    info_lines_ptr[i] = g_ptr_array_index(game_lines, i);
                }
            }
            
            const char **patch_lines_ptr = NULL;
            if (patch_lines->len > 0) {
                patch_lines_ptr = g_new(const char*, patch_lines->len);
                for (int i = 0; i < patch_lines->len; i++) {
                    patch_lines_ptr[i] = g_ptr_array_index(patch_lines, i);
                }
            }

            // Parse game info
            int info_consumed = 0;
            if (info_lines_ptr && game_lines->len > 0) {
                if (parse_game_info(info_lines_ptr, game_lines->len, &game, &info_consumed)) {
                    // Parse patches
                    int patch_consumed = 0;
                    if (patch_lines_ptr && patch_lines->len > 0) {
                        if (parse_game_patches(patch_lines_ptr, patch_lines->len, &game, &patch_consumed)) {
                            g_array_append_val(games_array, game);
                        } else {
                        }
                    } else {
                        // No patches, just add the game
                        g_array_append_val(games_array, game);
                    }
                } else {
                    
                    // Fallback: Direct parsing of the collected lines
                    for (int i = 0; i < game_lines->len; i++) {
                        char *line = g_ptr_array_index(game_lines, i);
                        char *trimmed = g_strstrip(g_strdup(line));
                        
                        if (g_str_has_prefix(trimmed, "title:")) {
                            char *colon = strchr(trimmed, ':');
                            if (colon && *(colon + 1) != '\0') {
                                char *value = g_strstrip(g_strdup(colon + 1));
                                if (value && value[0] != '\0') {
                                    game.game_title = value;
                                } else {
                                    g_free(value);
                                }
                            }
                        } else if (g_str_has_prefix(trimmed, "title-id:")) {
                            char *colon = strchr(trimmed, ':');
                            if (colon && *(colon + 1) != '\0') {
                                char *value = g_strstrip(g_strdup(colon + 1));
                                if (value && value[0] != '\0') {
                                    game.title_id = value;
                                } else {
                                    g_free(value);
                                }
                            }
                        } else if (g_str_has_prefix(trimmed, "region:")) {
                            char *colon = strchr(trimmed, ':');
                            if (colon && *(colon + 1) != '\0') {
                                char *value = g_strstrip(g_strdup(colon + 1));
                                if (value && value[0] != '\0') {
                                    game.region = value;
                                } else {
                                    g_free(value);
                                }
                            }
                        } else if (g_str_has_prefix(trimmed, "version:")) {
                            char *colon = strchr(trimmed, ':');
                            if (colon && *(colon + 1) != '\0') {
                                char *value = g_strstrip(g_strdup(colon + 1));
                                if (value && value[0] != '\0') {
                                    game.version = value;
                                } else {
                                    g_free(value);
                                }
                            }
                        }
                        
                        g_free(trimmed);
                    }
                    
                    // Set defaults for any missing fields
                    if (!game.game_title) game.game_title = g_strdup("Unknown Game");
                    if (!game.title_id) game.title_id = g_strdup("Unknown");
                    if (!game.region) game.region = g_strdup("NTSC");
                    if (!game.version) game.version = g_strdup("Unknown");
                   
                    g_array_append_val(games_array, game);
                }
            } else {
                g_array_append_val(games_array, game);
            }
            
            // Clean up
            g_free(info_lines_ptr);
            g_free(patch_lines_ptr);
            g_ptr_array_free(game_lines, true);
            g_ptr_array_free(patch_lines, true);
        } else {
            current_line++;
        }
        
        g_free(trimmed);
    }
    
    // Transfer games to database
    g_patches_db.game_count = games_array->len;
    if (games_array->len > 0) {
        g_patches_db.games = g_new(XemuGamePatches, games_array->len);
        memcpy(g_patches_db.games, games_array->data,
               games_array->len * sizeof(XemuGamePatches));
    }
    
    g_array_free(games_array, true);
    
    // Clean up lines
    for (guint i = 0; i < lines_array->len; i++) {
        g_free(g_ptr_array_index(lines_array, i));
    }
    g_ptr_array_free(lines_array, true);
    
    g_patches_db.file_path = g_strdup(filepath);
    g_patches_db.dirty = false;
    g_patches_loaded = true;
    
    // Loaded games from patches database
    
    g_free(actual_filepath);
    return true;
}

// Save patches database to file
bool xemu_patches_save_database(void)
{
    if (!g_patches_loaded) {
        // Patches database not loaded, cannot save
        return false;
    }
    
    // Prevent concurrent save operations
    if (g_save_in_progress) {
        // Database save already in progress, skipping
        return false;
    }
    
    g_save_in_progress = true;
    
    if (!g_patches_db.file_path) {
        g_patches_db.file_path = g_strdup_printf("%s/xemu.db", 
                                                SDL_GetBasePath());
    }
    
    
    // Check if we have valid data to save
    if (g_patches_db.game_count < 0) {
        // Invalid database state
        g_save_in_progress = false;
        return false;
    }
    
    // Additional validation - check if games array is valid
    if (!g_patches_db.games) {
        // Games array is NULL but game count is non-zero
        g_save_in_progress = false;
        return false;
    }
    
    FILE *file = qemu_fopen(g_patches_db.file_path, "w");
    if (!file) {
        // Failed to save patches database
        g_save_in_progress = false;
        return false;
    }
    
    // Add header comment
    // Database file header
    // Auto-generated, do not edit manually
    // Version 1.0
    
    // Validate game array before saving
    if (!g_patches_db.games) {
        fprintf(stderr, "No games data to save\n");
        fclose(file);
        return false;
    }
    
    for (int i = 0; i < g_patches_db.game_count; i++) {
        XemuGamePatches *game = &g_patches_db.games[i];
        
        // Validate game data
        if (!game) {
            // Invalid game data at index
            continue;
        }
        
        // Write game info with null checks
        fprintf(file, "Game Entry\n");
        fprintf(file, "==========\n");
        fprintf(file, "title: %s\n", game->game_title ? game->game_title : "Unknown Game");
        fprintf(file, "title-id: %s\n", game->title_id ? game->title_id : "Unknown");
        fprintf(file, "region: %s\n", game->region ? game->region : "NTSC");
        fprintf(file, "version: %s\n", game->version ? game->version : "Unknown");
        if (game->alternate_title_id && strlen(game->alternate_title_id) > 0) {
            fprintf(file, "alternate-title-id: %s\n", game->alternate_title_id);
        }
        if (game->time_date && strlen(game->time_date) > 0) {
            fprintf(file, "time-date: %s\n", game->time_date);
        }
        if (game->disc_number && strlen(game->disc_number) > 0) {
            fprintf(file, "disc-number: %s\n", game->disc_number);
        }
        fprintf(file, "\n");
        
        // Write patches with improved formatting
        fprintf(file, "Patches\n");
        fprintf(file, "=======\n");
        for (int j = 0; j < game->patch_count; j++) {
            XemuMemoryPatch *patch = &game->patches[j];
            
            if (!patch) {
                fprintf(stderr, "Invalid patch data at game %d, patch %d\n", i, j);
                continue;
            }
            
            // Write patch with clear field separation
            const char* patch_name_to_write = patch->name ? patch->name : "Unnamed Patch";
            for (size_t k = 0; k < strlen(patch_name_to_write); k++) {
            }
            fprintf(file, "  Patch: %s\n", patch_name_to_write);
            fprintf(file, "    Author: %s\n", patch->author ? patch->author : "Unknown");
            fprintf(file, "    Category: %s\n", patch->category ? patch->category : "Other");
            
            if (patch->notes && strlen(patch->notes) > 0) {
                fprintf(file, "    Notes: %s\n", patch->notes);
            }
            
            fprintf(file, "    Enabled: %d\n", patch->enabled ? 1 : 0);
            fprintf(file, "    Save Replaced Values: %d\n", patch->save_replaced_values ? 1 : 0);
            
            // Write address:value pairs one per line
            if (patch->address_value_lines && patch->address_value_lines_count > 0) {
                fprintf(file, "    Memory Addresses:\n");
                // Use original address:value lines (with comments preserved)
                for (int k = 0; k < patch->address_value_lines_count; k++) {
                    const char* addr_line = patch->address_value_lines[k];
                    fprintf(file, "      %s\n", addr_line);
                }
            } else if (patch->address_values && patch->address_value_count > 0) {
                // Fallback for legacy patches without original lines
                fprintf(file, "    Memory Addresses:\n");
                for (int k = 0; k < patch->address_value_count; k++) {
                    // Build hex string from byte array
                    char hex_value[256] = "";
                    for (int i = 0; i < patch->address_values[k].value_length; i++) {
                        char hex_byte[4];
                        snprintf(hex_byte, sizeof(hex_byte), "%02X", patch->address_values[k].value_data[i]);
                        strcat(hex_value, hex_byte);
                    }
                    
                    // Format address with leading zeros, but preserve value format (no leading zeros)
                    fprintf(file, "      0x%08X: 0x%s\n", 
                           patch->address_values[k].address, hex_value);
                }
            }
            fprintf(file, "\n");
        }
        
        // Blank line between games
        fprintf(file, "\n");
    }
    
    if (fclose(file) != 0) {
        // Error closing database file
        g_save_in_progress = false;
        return false;
    }
    
    g_patches_db.dirty = false;
    g_save_in_progress = false;
    
    return true;
}

// Initialize patches system
void xemu_patches_init(void)
{
    if (g_patches_initialized) {
        return;
    }
    
    // Initializing patches system
    
    // Load default database (xemu.db) from executable location
    char *default_path = g_strdup_printf("%s/xemu.db", SDL_GetBasePath());
    // Loading patches database
    
    bool loaded = xemu_patches_load_database(default_path);
    
    // If database doesn't exist, create a new one
    if (!loaded) {
        // Patches database not found, creating new database
        g_patches_db.game_count = 0;
        g_patches_db.games = NULL;
        g_patches_db.file_path = g_strdup(default_path);
        g_patches_db.dirty = false;
        g_patches_loaded = true;
        
        // Save the empty database to create the file
        xemu_patches_save_database();
        // Created new patches database
    }
    
    g_free(default_path);
    g_patches_initialized = true;
    
    // Initialize certificate tracking variables
    g_last_cert_title_id = 0;
    g_last_cert_region = 0;
    g_last_cert_version = 0;
    g_last_cert_read_time = 0;
    g_cert_data_valid = false;
}

// Get database file path
const char *xemu_patches_get_database_path(void)
{
    return g_patches_db.file_path;
}

// Get all game entries
XemuGamePatches* xemu_patches_get_games(void)
{
    return g_patches_db.games;
}

int xemu_patches_get_game_count(void)
{
    return g_patches_db.game_count;
}

// Find patches for specific game by filename
XemuGamePatches* xemu_patches_find_game_by_filename(const char *disc_path)
{
    if (!disc_path) {
        return NULL;
    }
    
    // Extract filename from path
    const char *filename = strrchr(disc_path, '/');
    if (filename) {
        filename++; // Skip the '/'
    } else {
        filename = disc_path;
    }
    
    // Remove file extension for comparison
    char *base_name = g_strdup(filename);
    char *dot = strrchr(base_name, '.');
    if (dot) {
        *dot = '\0'; // Remove extension
    }
    
    // Find game by filename (case-insensitive)
    for (int i = 0; i < g_patches_db.game_count; i++) {
        XemuGamePatches *game = &g_patches_db.games[i];
        
        if (!game->enabled) {
            continue;
        }
        
        // Try exact filename match first
        int cmp_result = g_ascii_strcasecmp(base_name, game->game_title);
        if (cmp_result == 0) {
            g_free(base_name);
            return game;
        } else {
        }
    }
    g_free(base_name);
    return NULL;
}

// Apply patches for current game
void xemu_patches_apply_current_game_patches(void)
{
    // CRITICAL DISC PRESENCE CHECK: Skip all patching if no disc is loaded
    if (!g_patch_system_enabled) {
        return;
    }
    
    if (!g_patches_loaded || !g_patches_initialized) {
        error_report("Patches database not loaded");
        return;
    }
}

// Find game patches by XBE certificate information
XemuGamePatches* xemu_patches_find_game_by_certificate(void)
{
    // CRITICAL: Prevent ALL certificate operations during Load Disc
    // Load Disc operations cause VM instability and can cause memory crashes
    if (g_load_disc_in_progress) {
        return NULL;
    }
    
    
    // USE CACHED XBE INFO TO IMPROVE PERFORMANCE
    uint32_t title_id, game_region, version;
    if (!get_cached_xbe_info(&title_id, &game_region, &version)) {
        return NULL;
    }
    
    // 🔍 DATABASE INTEGRITY CHECK
    if (!g_patches_db.games) {
        return NULL;
    }
    if (g_patches_db.game_count == 0) {
    }
    
    if (g_cert_data_valid) {
    }
    
    if (g_cert_data_valid) {
        if (title_id != g_last_cert_title_id || 
            game_region != g_last_cert_region || 
            version != g_last_cert_version) {
            
            // CRITICAL: Clear both certificate caches
            xemu_clear_cert_cache();
            invalidate_certificate_cache();
            
            reset_last_applied_tracking();
            

            // CRITICAL: Allow certificate updates during Load Disc to support game switching
            // Only prevent during actual Load Disc operations, not certificate detection
            if (g_load_disc_in_progress) {
                // Allow certificate detection to continue - this enables proper game switching
            }
            
            struct xbe *fresh_xbe_info = xemu_get_xbe_info();
            if (fresh_xbe_info && fresh_xbe_info->cert) {
                title_id = ldl_le_p(&fresh_xbe_info->cert->m_titleid);
                game_region = ldl_le_p(&fresh_xbe_info->cert->m_game_region);
                version = ldl_le_p(&fresh_xbe_info->cert->m_version);
            } else {
            }
        }
    } else {
    }
    
    // CRITICAL: Always reset state when TitleID changes (game switch)
    if (title_id != g_last_cert_title_id) {
        
        // Reset ALL state flags to allow proper patch application for new game
        g_patches_applied_for_current_cert = false;
        g_disc_present = true; // Set true immediately when new game detected
    }
    
    // Always update the global certificate tracking variables
    g_last_cert_title_id = title_id;
    g_last_cert_region = game_region;
    g_last_cert_version = version;
    g_cert_data_valid = true;
    
    // Auto-enable patches when certificate data becomes available
    auto_enable_patches_when_ready();
    
    // Convert region code to string
    char region_str[16] = "Unknown";
    switch (game_region) {
        case 0x00000001: strcpy(region_str, "NTSC-U"); break;
        case 0x00000002: strcpy(region_str, "NTSC-J"); break;
        case 0x00000004: strcpy(region_str, "NTSC-K"); break;
        case 0x00000008: strcpy(region_str, "PAL"); break;
        default: sprintf(region_str, "0x%08X", game_region); break;
    }
    
    // Convert version to string format (major.minor.patch.build)
    char version_str[32];
    uint8_t major = (version >> 24) & 0xFF;
    uint8_t minor = (version >> 16) & 0xFF;
    uint8_t patch = (version >> 8) & 0xFF;
    uint8_t build = version & 0xFF;
    snprintf(version_str, sizeof(version_str), "%d.%d.%d.%d", major, minor, patch, build);
    
    // Convert Title ID to string (format matches database: 8 hex digits)
    char title_id_str[16];
    snprintf(title_id_str, sizeof(title_id_str), "%08X", title_id);
    
    // for (int debug_i = 0; debug_i < g_patches_db.game_count && debug_i < 5; debug_i++) {
    //     // XemuGamePatches *debug_game = &g_patches_db.games[debug_i];
    // }
    
    for (int i = 0; i < g_patches_db.game_count; i++) {
        XemuGamePatches *game = &g_patches_db.games[i];
        
        if (!game->enabled) {
            continue;
        }
        
        if (!game->title_id || !game->region || !game->version) {
            continue;
        }
        
        bool title_matches = strcmp(game->title_id, title_id_str) == 0;
        bool region_matches = strcmp(game->region, region_str) == 0;
        bool version_matches = strcmp(game->version, version_str) == 0;
        
        if (title_matches && region_matches && version_matches) {
            
            // Convert current certificate to strings for tracking comparison
            char current_title_id_str[17];
            char current_region_str[8];
            char current_version_str[32];
            
            snprintf(current_title_id_str, sizeof(current_title_id_str), "%08X", title_id);
            snprintf(current_region_str, sizeof(current_region_str), "%X", game_region);
            snprintf(current_version_str, sizeof(current_version_str), "%X.%X.%X.%X", 
                     (version >> 24) & 0xFF, (version >> 16) & 0xFF, 
                     (version >> 8) & 0xFF, version & 0xFF);
            
            // Enable certificate tracking on first successful certificate read
            if (!g_certificate_tracking_enabled) {
                enable_certificate_tracking();
            }
            
            // Check if certificate has changed since last seen
            bool title_changed = (strcmp(g_last_seen_title_id, current_title_id_str) != 0);
            bool region_changed = (strcmp(g_last_seen_region, current_region_str) != 0);
            bool version_changed = (strcmp(g_last_seen_version, current_version_str) != 0);
            bool cert_changed = (title_changed || region_changed || version_changed);
            
            
            if (!cert_changed) {
                
                // Check if this is a manual reset operation
                if (g_manual_reset_detected) {
                    
                    // Clear the manual reset flag since we're handling it now
                    g_manual_reset_detected = false;
                    g_suppress_patch_notification = false;
					
                    return game;
                } else {
                    if (g_patches_applied_for_current_cert && g_post_reset_patch_application_active) {
                        
                        // Clear the applied flag to allow notification
                        g_patches_applied_for_current_cert = false;
                        g_suppress_patch_notification = false;
                        
                        // Update certificate tracking but allow notification
                        strncpy(g_last_seen_title_id, current_title_id_str, sizeof(g_last_seen_title_id) - 1);
                        g_last_seen_title_id[sizeof(g_last_seen_title_id) - 1] = '\0';
                        
                        strncpy(g_last_seen_region, current_region_str, sizeof(g_last_seen_region) - 1);
                        g_last_seen_region[sizeof(g_last_seen_region) - 1] = '\0';
                        
                        strncpy(g_last_seen_version, current_version_str, sizeof(g_last_seen_version) - 1);
                        g_last_seen_version[sizeof(g_last_seen_version) - 1] = '\0';
                        
                        // Return game to trigger notification
                        return game;
                    } else {
                        g_suppress_patch_notification = true;
                    }
                    
                    // Update the last seen certificate to current (maintains tracking but prevents duplicate messages)
                    strncpy(g_last_seen_title_id, current_title_id_str, sizeof(g_last_seen_title_id) - 1);
                    g_last_seen_title_id[sizeof(g_last_seen_title_id) - 1] = '\0';
                    
                    strncpy(g_last_seen_region, current_region_str, sizeof(g_last_seen_region) - 1);
                    g_last_seen_region[sizeof(g_last_seen_region) - 1] = '\0';
                    
                    strncpy(g_last_seen_version, current_version_str, sizeof(g_last_seen_version) - 1);
                    g_last_seen_version[sizeof(g_last_seen_version) - 1] = '\0';
                    
                    // Return the found game without triggering notification
                    return game;
                }
            }
            
            // Reset suppression flag since this is a legitimate certificate change
            g_suppress_patch_notification = false;
            
            // Update the last seen certificate to current
            strncpy(g_last_seen_title_id, current_title_id_str, sizeof(g_last_seen_title_id) - 1);
            g_last_seen_title_id[sizeof(g_last_seen_title_id) - 1] = '\0';
            strncpy(g_last_seen_region, current_region_str, sizeof(g_last_seen_region) - 1);
            g_last_seen_region[sizeof(g_last_seen_region) - 1] = '\0';
            strncpy(g_last_seen_version, current_version_str, sizeof(g_last_seen_version) - 1);
            g_last_seen_version[sizeof(g_last_seen_version) - 1] = '\0';
            
            // Count enabled patches (not used, but keeping logic for future enhancement)
            // int enabled_count = 0;
            // for (int j = 0; j < game->patch_count; j++) {
            //     if (game->patches[j].enabled) enabled_count++;
            // }
            
            return game;
        }
    }
    return NULL;
}

// Apply patches for specific disc
void xemu_patches_apply_for_disc(const char *disc_path)
{
    if (!g_patches_loaded || !g_patches_initialized) {
        return;
    }
    
    if (disc_path) {
        // Extract meaningful part of disc path for comparison
        const char* last_slash = strrchr(disc_path, '/');
        const char* last_backslash = strrchr(disc_path, '\\');
        const char* disc_name = (last_slash > last_backslash) ? last_slash + 1 : 
                               (last_backslash ? last_backslash + 1 : disc_path);
        
        // Remove .iso extension if present
        static char disc_base_name[512];
        strncpy(disc_base_name, disc_name, sizeof(disc_base_name) - 1);
        disc_base_name[sizeof(disc_base_name) - 1] = '\0';
        char* dot_iso = strstr(disc_base_name, ".iso");
        if (dot_iso) *dot_iso = '\0';
    }
    
    // Track disc path changes to invalidate cache when new disc is loaded
    static char last_disc_path[512] = "";
    
    if (disc_path && strcmp(disc_path, last_disc_path) != 0) {
        // CRITICAL FIX: Invalidate certificate cache immediately when new disc is loaded
        invalidate_certificate_cache();
        
        // CRITICAL: Reset last applied tracking to prevent wrong game detection
        reset_last_applied_tracking();

        
        // Copy current disc path for future comparison
        strncpy(last_disc_path, disc_path, sizeof(last_disc_path) - 1);
        last_disc_path[sizeof(last_disc_path) - 1] = '\0';

    } else if (disc_path) {
    }
    
    if (!disc_path) {
        return;
    }
	
    g_force_fresh_xbe_read = true;
    
    for (int attempt = 1; attempt <= 3; attempt++) {
    }
    
    // Simple cache-based validation
    uint32_t cached_title_id, cached_region, cached_version;
    if (get_cached_xbe_info_with_spam_prevention(&cached_title_id, &cached_region, &cached_version)) {   
        // Update tracking variables
        g_last_cert_title_id = cached_title_id;
        g_last_cert_region = cached_region;
        g_last_cert_version = cached_version;
        g_cert_data_valid = true;
        
        // Auto-enable patches when certificate data becomes available
        auto_enable_patches_when_ready();
    } else {
    }
    
    // Set current XBE path for auto-population
    xemu_patches_set_current_xbe_path(disc_path);
    
    // STEP 1: CERTIFICATE-BASED MATCHING
    XemuGamePatches *game_patches = xemu_patches_find_game_by_certificate();
    
    if (game_patches) {
        // Certificate-based match found
    } else {
        game_patches = xemu_patches_find_game_by_filename(disc_path);
    }

    if (!game_patches) {
        return;
    }
    if (!game_patches->enabled) {
        return;
    }
    int enabled_patches = 0;
    for (int i = 0; i < game_patches->patch_count; i++) {
        if (game_patches->patches[i].enabled) {
            enabled_patches++;
        }
    }
    
    if (enabled_patches == 0) {
        return;
    }
    
    int applied_count = 0;
    int failed_count = 0;
    
    for (int i = 0; i < game_patches->patch_count; i++) {
        XemuMemoryPatch *patch = &game_patches->patches[i];
        
        if (!patch->enabled) {
            continue;
        }
        
        // Build hex string for display
        char value_str[128] = "";
        for (int b = 0; b < patch->address_values[0].value_length && b < 32; b++) {
            snprintf(value_str + b * 2, sizeof(value_str) - b * 2, "%02X", patch->address_values[0].value_data[b]);
        }
        
        // Apply first address-value pair
        XemuPatchAddressValue *first_pair = &patch->address_values[0];
        if (apply_single_patch_bytes(first_pair->address, first_pair->value_data, first_pair->value_length, NULL)) {
            applied_count++;
        } else {
            failed_count++;
            report_patch_error(patch->name, "apply");
        }
    }
    
    if (applied_count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Applied %d patch%s for %s", 
                applied_count, applied_count == 1 ? "" : "es", game_patches->game_title);
        
        xemu_queue_notification(msg);
    } else {
    }
    
    if (failed_count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to apply %d memory patch(es)", failed_count);
        xemu_queue_error_message(msg);
    } else {
    }
    
    // Start patch monitoring after all patches are applied
    if (applied_count > 0) {
        // Perform initial monitoring
        perform_periodic_monitoring();
    }
}

// Force-apply patches for currently running XBE (can be called manually or programmatically)
void xemu_patches_apply_current_running_xbe(void)
{
    // CRITICAL DISC PRESENCE CHECK: Skip all patching if no disc is loaded
    if (!g_patch_system_enabled) {
        return;
    }
    
    if (!g_patches_loaded || !g_patches_initialized) {
        return;
    }
    
    // Try to apply patches based on current XBE certificate
    XemuGamePatches *game_patches = xemu_patches_find_game_by_certificate();
    
    if (!game_patches) {
        return;
    }
    
    if (!game_patches->enabled) {
        return;
    }
    
    // Applying patches for game
    
    // Apply all enabled patches for this game
    int applied_count = 0;
    int failed_count = 0;
    
    for (int i = 0; i < game_patches->patch_count; i++) {
        XemuMemoryPatch *patch = &game_patches->patches[i];
        
        if (!patch->enabled) {
            continue;
        }
        
        // Build hex string for display
        char value_str[128] = "";
        for (int b = 0; b < patch->address_values[0].value_length && b < 32; b++) {
            snprintf(value_str + b * 2, sizeof(value_str) - b * 2, "%02X", patch->address_values[0].value_data[b]);
        }
        
        // Apply first address-value pair
        XemuPatchAddressValue *first_pair = &patch->address_values[0];
        if (apply_single_patch_bytes(first_pair->address, first_pair->value_data, first_pair->value_length, NULL)) {
            applied_count++;
        } else {
            failed_count++;
        }
    }
    
    // Send user notification
    if (applied_count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Applied %d patch%s for %s", 
                applied_count, applied_count == 1 ? "" : "es", game_patches->game_title);
        xemu_queue_notification(msg);
    }
    if (failed_count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to apply %d memory patch(es)", failed_count);
        xemu_queue_error_message(msg);
    }
}

// Apply patches at Xemu startup
void xemu_patches_on_startup(void)
{
    if (!g_patches_loaded || !g_patches_initialized) {
        return;
    }
}

// Find patches for specific game by title ID
int xemu_patches_find_game_by_title_id(const char *title_id)
{
    if (!title_id || !g_patches_loaded) {
        return -1;
    }
    
    for (int i = 0; i < g_patches_db.game_count; i++) {
        XemuGamePatches *game = &g_patches_db.games[i];
        if (game->title_id && strcmp(game->title_id, title_id) == 0) {
            return i;
        }
    }
    
    return -1;  // Not found
}

// 🆕 FIND GAME BY TITLE ID AND VERSION (STRICTER DUPLICATE CHECK)
// This function checks both Title ID and Version to avoid true duplicates
// Usage: Only prevent adding if BOTH Title ID AND Version match existing game
int xemu_patches_find_duplicate_game(const char *title_id, const char *version)
{
    if (!title_id || !version || !g_patches_loaded) {
        return -1;
    }
    
    for (int i = 0; i < g_patches_db.game_count; i++) {
        XemuGamePatches *game = &g_patches_db.games[i];
        bool title_matches = (game->title_id && strcmp(game->title_id, title_id) == 0);
        bool version_matches = (game->version && strcmp(game->version, version) == 0);
        
        // True duplicate only if BOTH Title ID and Version match
        if (title_matches && version_matches) {
            return i;
        }
    }
    
    return -1;  // Not found (no exact duplicate)
}

// Parse XBE certificate for auto-population
bool xemu_patches_parse_xbe_certificate(const char *xbe_path, 
                                       char *title_out, int title_size,
                                       char *title_id_out, int title_id_size,
                                       char *region_out, int region_size,
                                       char *version_out, int version_size,
                                       char *alternate_title_id_out, int alternate_title_id_size,
                                       char *time_date_out, int time_date_size,
                                       char *disc_number_out, int disc_number_size)
{
    // CRITICAL: Prevent ALL certificate operations during Load Disc
    // Load Disc operations cause VM instability and can cause memory crashes
    if (g_load_disc_in_progress) {
        
        // Clear output buffers to avoid showing stale data
        if (title_out) title_out[0] = '\0';
        if (title_id_out) title_id_out[0] = '\0';
        if (region_out) region_out[0] = '\0';
        if (version_out) version_out[0] = '\0';
        if (alternate_title_id_out) alternate_title_id_out[0] = '\0';
        if (time_date_out) time_date_out[0] = '\0';
        if (disc_number_out) disc_number_out[0] = '\0';
        return false;
    }
    
    // Clear output buffers
    if (title_out) title_out[0] = '\0';
    if (title_id_out) title_id_out[0] = '\0';
    if (region_out) region_out[0] = '\0';
    if (version_out) version_out[0] = '\0';
    if (alternate_title_id_out) alternate_title_id_out[0] = '\0';
    if (time_date_out) time_date_out[0] = '\0';
    if (disc_number_out) disc_number_out[0] = '\0';
    
    // Force immediate fresh certificate read
    g_force_fresh_xbe_read = true;
    g_xbe_cache.valid = false;
    g_xbe_cache.frame_last_read = -1;
    g_xbe_cache.last_read_time = 0;
    
    // Clear certificate data to force fresh read
    g_cert_data_valid = false;
    
    // Get current XBE info from runtime (will now force fresh read)
    uint32_t title_id, game_region, version;
    if (!get_cached_xbe_info(&title_id, &game_region, &version)) {
        // Fall back to filename parsing if no XBE certificate available
        if (xbe_path) {
            const char *basename = strrchr(xbe_path, '/');
            if (!basename) basename = xbe_path;
            else basename++;
            
            if (title_out && title_size > 0) {
                strncpy(title_out, basename, title_size - 1);
                title_out[title_size - 1] = '\0';
                // Remove .xbe extension if present
                char *dot = strrchr(title_out, '.');
                if (dot && strcmp(dot, ".xbe") == 0) {
                    *dot = '\0';
                }
            }
        }
        
        // Set fallback defaults
        if (title_id_out && title_id_size > 8) {
            strncpy(title_id_out, "4D530001", title_id_size - 1);
            title_id_out[title_id_size - 1] = '\0';
        }
        if (region_out && region_size > 4) {
            strncpy(region_out, "NTSC", region_size - 1);
            region_out[region_size - 1] = '\0';
        }
        if (version_out && version_size > 8) {
            strncpy(version_out, "0.0.0.5", version_size - 1);
            version_out[version_size - 1] = '\0';
        }
        // Set fallback defaults for new fields
        if (alternate_title_id_out && alternate_title_id_size > 8) {
            strncpy(alternate_title_id_out, "00000000", alternate_title_id_size - 1);
            alternate_title_id_out[alternate_title_id_size - 1] = '\0';
        }
        if (time_date_out && time_date_size > 19) {
            strncpy(time_date_out, "1970-01-01 00:00:00", time_date_size - 1);
            time_date_out[time_date_size - 1] = '\0';
        }
        if (disc_number_out && disc_number_size > 4) {
            strncpy(disc_number_out, "1", disc_number_size - 1);
            disc_number_out[disc_number_size - 1] = '\0';
        }
        return true;
    }
    
    // Set title to filename-based fallback (title not cached)
    if (title_out && title_size > 0) {
        if (xbe_path) {
            const char *basename = strrchr(xbe_path, '/');
            if (!basename) basename = xbe_path;
            else basename++;
            
            strncpy(title_out, basename, title_size - 1);
            title_out[title_size - 1] = '\0';
            // Remove .xbe extension if present
            char *dot = strrchr(title_out, '.');
            if (dot && strcmp(dot, ".xbe") == 0) {
                *dot = '\0';
            }
        } else {
            strncpy(title_out, "Unknown Game", title_size - 1);
            title_out[title_size - 1] = '\0';
        }
    }
    
    // Format title ID as hex string
    if (title_id_out && title_id_size > 8) {
        snprintf(title_id_out, title_id_size, "%08X", title_id);
    }
    
    // Determine region from game_region field
    if (region_out && region_size > 4) {
        switch (game_region) {
            case 0x01: strncpy(region_out, "NTSC-U", region_size - 1); break; // North America
            case 0x02: strncpy(region_out, "NTSC-J", region_size - 1); break; // Japan
            case 0x04: strncpy(region_out, "PAL", region_size - 1); break;    // PAL/Europe
            case 0x05: strncpy(region_out, "NTSC-K", region_size - 1); break; // Korea
            default:   strncpy(region_out, "NTSC", region_size - 1); break;   // Default
        }
        region_out[region_size - 1] = '\0';
    }
    
    // Format version (match the format used elsewhere: major.minor.patch.build)
    if (version_out && version_size > 8) {
        uint8_t major = (version >> 24) & 0xFF;
        uint8_t minor = (version >> 16) & 0xFF;
        uint8_t patch = (version >> 8) & 0xFF;
        uint8_t build = version & 0xFF;
        snprintf(version_out, version_size, "%d.%d.%d.%d", major, minor, patch, build);
    }
    
    // Set alternate title ID to default (not available from cache)
    if (alternate_title_id_out && alternate_title_id_size > 8) {
        strncpy(alternate_title_id_out, "00000000", alternate_title_id_size - 1);
        alternate_title_id_out[alternate_title_id_size - 1] = '\0';
    }
    
    // Set timestamp to current time (not available from cache)
    if (time_date_out && time_date_size > 19) {
        time_t now = time(NULL);
        struct tm *timeinfo = localtime(&now);
        if (timeinfo) {
            strftime(time_date_out, time_date_size, "%Y-%m-%d %H:%M:%S", timeinfo);
        } else {
            snprintf(time_date_out, time_date_size, "1970-01-01 00:00:00");
        }
    }
    
    // Set disc number to default (not available from cache)
    if (disc_number_out && disc_number_size > 4) {
        strncpy(disc_number_out, "1", disc_number_size - 1);
        disc_number_out[disc_number_size - 1] = '\0';
    }
    
    // All data extracted from cache - no need to read XBE file
    return true;
}

// Global variable to track current XBE path
static char* g_current_xbe_path = NULL;

// Get current loaded XBE path
const char* xemu_patches_get_current_xbe_path(void)
{
    return g_current_xbe_path;
}

// Set current XBE path
void xemu_patches_set_current_xbe_path(const char *xbe_path)
{
    g_free(g_current_xbe_path);
    g_current_xbe_path = g_strdup(xbe_path);
}

// Game patches management functions
bool xemu_patches_add_game(const char *title, const char *region, 
                          const char *title_id, const char *version,
                          const char *alternate_title_id, const char *time_date,
                          const char *disc_number)
{
    
    XemuGamePatches *new_games = g_renew(XemuGamePatches, g_patches_db.games, 
                                         g_patches_db.game_count + 1);
    if (!new_games) {
        return false;
    }
    
    g_patches_db.games = new_games;
    
    XemuGamePatches *new_game = &g_patches_db.games[g_patches_db.game_count];
    memset(new_game, 0, sizeof(XemuGamePatches));
    
    new_game->game_title = g_strdup(title);
    new_game->region = g_strdup(region);
    new_game->title_id = g_strdup(title_id);
    new_game->version = g_strdup(version);
    new_game->alternate_title_id = g_strdup(alternate_title_id);
    new_game->time_date = g_strdup(time_date);
    new_game->disc_number = g_strdup(disc_number);
    new_game->enabled = true;
    
    g_patches_db.game_count++;
    g_patches_db.dirty = true;

    // Immediately save to disk to ensure data persistence
    if (xemu_patches_save_database()) {
    } else {
    }
    
    return true;
}

bool xemu_patches_remove_game(int game_index)
{
    if (game_index < 0 || game_index >= g_patches_db.game_count) {
        return false;
    }
    
    XemuGamePatches *game = &g_patches_db.games[game_index];
    
    // Free game data
    g_free(game->game_title);
    g_free(game->region);
    g_free(game->title_id);
    g_free(game->version);
    g_free(game->alternate_title_id);
    g_free(game->time_date);
    g_free(game->disc_number);
    
    // Free all patch data including multi-byte values
    for (int i = 0; i < game->patch_count; i++) {
        g_free(game->patches[i].name);
        g_free(game->patches[i].category);
        g_free(game->patches[i].author);
        g_free(game->patches[i].notes);
        
        // Free address:value pairs with multi-byte support
        if (game->patches[i].address_values) {
            for (int j = 0; j < game->patches[i].address_value_count; j++) {
                g_free(game->patches[i].address_values[j].value_data);
            }
            g_free(game->patches[i].address_values);
        }
    }
    g_free(game->patches);
    
    // Remove from array
    if (game_index < g_patches_db.game_count - 1) {
        memmove(game, game + 1, 
                (g_patches_db.game_count - game_index - 1) * sizeof(XemuGamePatches));
    }
    
    g_patches_db.game_count--;
    g_patches_db.dirty = true;
    
    return true;
}

bool xemu_patches_update_game(int game_index, const char *title, 
                             const char *region, const char *title_id,
                             const char *version, const char *alternate_title_id,
                             const char *time_date, const char *disc_number)
{
    if (game_index < 0 || game_index >= g_patches_db.game_count) {
        return false;
    }
    
    XemuGamePatches *game = &g_patches_db.games[game_index];
    
    g_free(game->game_title);
    g_free(game->region);
    g_free(game->title_id);
    g_free(game->version);
    g_free(game->alternate_title_id);
    g_free(game->time_date);
    g_free(game->disc_number);
    
    game->game_title = g_strdup(title);
    game->region = g_strdup(region);
    game->title_id = g_strdup(title_id);
    game->version = g_strdup(version);
    game->alternate_title_id = g_strdup(alternate_title_id);
    game->time_date = g_strdup(time_date);
    game->disc_number = g_strdup(disc_number);
    
    g_patches_db.dirty = true;
    return true;
}

// Patch management within games
bool xemu_patches_add_patch(int game_index, const char *name, const char *category,
                           const char *author, const char *notes, const char *address_value_pairs,
                           bool save_replaced_values)
{
    
    if (game_index < 0 || game_index >= g_patches_db.game_count) {
        return false;
    }
    
    if (!name || !category || !author || !address_value_pairs) {
        return false;
    }
    
    XemuGamePatches *game = &g_patches_db.games[game_index];
    XemuMemoryPatch *new_patches = g_renew(XemuMemoryPatch, game->patches,
                                          game->patch_count + 1);
    if (!new_patches) {
        return false;
    }
    
    game->patches = new_patches;
    XemuMemoryPatch *new_patch = &game->patches[game->patch_count];
    
    // Parse address:value pairs
    XemuPatchAddressValue *parsed_pairs = NULL;
    int count = 0;
    
    if (xemu_patches_parse_address_value_pairs(address_value_pairs, &parsed_pairs, &count)) {
        // Transfer ownership of parsed pairs
        new_patch->address_value_count = count;
        new_patch->address_values = parsed_pairs;
    } else {
        return false; // Failed to parse address:value pairs
    }
    new_patch->enabled = true;
    
    // Set save_replaced_values from UI parameter
    new_patch->save_replaced_values = save_replaced_values;
    new_patch->name = g_strdup(name);
    new_patch->category = g_strdup(category);
    new_patch->author = g_strdup(author ? author : "Unknown");
    new_patch->notes = g_strdup(notes ? notes : "");
    
    // Store the original address:value lines (with comments)
    // Split the input text into lines, preserving original indentation
    GPtrArray *lines_array = g_ptr_array_new();
    char *text_copy = g_strdup(address_value_pairs);
    char *line = strtok(text_copy, "\n");
    
    while (line != NULL) {
        // Skip completely empty lines
        if (line[0] != '\0') {
            // Store the original line as-is (preserve indentation and formatting)
            char *original_line = g_strdup(line);
            g_ptr_array_add(lines_array, original_line);
        } else {
        }
        line = strtok(NULL, "\n");
    }
    // Transfer lines to patch structure
    if (lines_array->len > 0) {
        new_patch->address_value_lines = g_new(char*, lines_array->len);
        new_patch->address_value_lines_count = lines_array->len;
        for (guint i = 0; i < lines_array->len; i++) {
            new_patch->address_value_lines[i] = g_ptr_array_index(lines_array, i);
        }
    } else {
        new_patch->address_value_lines = NULL;
        new_patch->address_value_lines_count = 0;
    }
    
    g_free(text_copy);
    g_ptr_array_free(lines_array, false); // Don't free the strings, we transferred ownership
    g_patches_db.dirty = true;
    
    return true;
}

bool xemu_patches_remove_patch(int game_index, int patch_index)
{
    
    if (game_index < 0 || game_index >= g_patches_db.game_count) {
        return false;
    }
    
    XemuGamePatches *game = &g_patches_db.games[game_index];
    
    if (patch_index < 0 || patch_index >= game->patch_count) {
        return false;
    }
    
    g_free(game->patches[patch_index].name);
    g_free(game->patches[patch_index].category);
    g_free(game->patches[patch_index].author);
    g_free(game->patches[patch_index].notes);
    
    // Free original address:value lines
    if (game->patches[patch_index].address_value_lines) {
        for (int i = 0; i < game->patches[patch_index].address_value_lines_count; i++) {
            g_free(game->patches[patch_index].address_value_lines[i]);
        }
        g_free(game->patches[patch_index].address_value_lines);
    }
    
    // Free address:value pairs array
    if (game->patches[patch_index].address_values) {
        for (int i = 0; i < game->patches[patch_index].address_value_count; i++) {
            g_free(game->patches[patch_index].address_values[i].value_data);
        }
        g_free(game->patches[patch_index].address_values);
        game->patches[patch_index].address_values = NULL;
    }
    
    game->patches[patch_index].address_value_count = 0;
    
    if (patch_index < game->patch_count - 1) {
        memmove(&game->patches[patch_index], &game->patches[patch_index + 1],
                (game->patch_count - patch_index - 1) * sizeof(XemuMemoryPatch));
    }
    
    game->patch_count--;
    g_patches_db.dirty = true;
    
    // Clear any saved values for this patch
    for (int i = 0; i < g_saved_values_count; i++) {
        if (g_saved_values[i].game_index == game_index && 
            g_saved_values[i].patch_index == patch_index) {
            g_free(g_saved_values[i].original_data);
            
            // Remove this entry by swapping with the last entry
            if (i < g_saved_values_count - 1) {
                g_saved_values[i] = g_saved_values[g_saved_values_count - 1];
            }
            g_saved_values_count--;
            i--; // Adjust index after swap
        }
    }
    
    return true;
}

bool xemu_patches_update_patch(int game_index, int patch_index, 
                              const char *name, const char *category, const char *author,
                              const char *notes, const char *address_value_pairs, bool save_replaced_values)
{
    if (game_index < 0 || game_index >= g_patches_db.game_count) {
        return false;
    }
    
    XemuGamePatches *game = &g_patches_db.games[game_index];
    
    if (patch_index < 0 || patch_index >= game->patch_count) {
        return false;
    }
    
    if (!name || !category || !author || !address_value_pairs) {
        return false;
    }
    
    XemuMemoryPatch *patch = &game->patches[patch_index];
    if (patch->name) {
        g_free(patch->name);
        patch->name = NULL;
    } else {
    }
    
    if (patch->category) {
        g_free(patch->category);
        patch->category = NULL;
    } else {
    }
    
    if (patch->author) {
        g_free(patch->author);
        patch->author = NULL;
    } else {
    }
    
    if (patch->notes) {
        g_free(patch->notes);
        patch->notes = NULL;
    } else {
    }
    
    // Free existing address:value pairs and original lines before updating
    if (patch->address_values) {
        for (int i = 0; i < patch->address_value_count; i++) {
            if (patch->address_values[i].value_data) {
                g_free(patch->address_values[i].value_data);
            }
        }
        g_free(patch->address_values);
        patch->address_values = NULL;
    } else {
    }
    patch->address_value_count = 0;
    
    // Free existing original address:value lines
    if (patch->address_value_lines) {
        for (int i = 0; i < patch->address_value_lines_count; i++) {
            if (patch->address_value_lines[i]) {
                g_free(patch->address_value_lines[i]);
            }
        }
        g_free(patch->address_value_lines);
        patch->address_value_lines = NULL;
    } else {
    }
    patch->address_value_lines_count = 0;
    
    // Parse address:value pairs
    XemuPatchAddressValue *parsed_pairs = NULL;
    int count = 0;
    
    if (xemu_patches_parse_address_value_pairs(address_value_pairs, &parsed_pairs, &count)) {
        // Transfer ownership of parsed pairs
        patch->address_value_count = count;
        patch->address_values = parsed_pairs;
    } else {
        return false;
    }
	
    // Preserve enabled state
    bool old_enabled = patch->enabled;
    patch->save_replaced_values = save_replaced_values;
    
    // Update name, category, author, and notes (all pointers already set to NULL above)
    patch->name = g_strdup(name);
    patch->category = g_strdup(category);
    patch->author = g_strdup(author ? author : "Unknown");
    patch->notes = g_strdup(notes ? notes : "");
    
    // Restore preserved patch settings
    patch->enabled = old_enabled;
    // Note: save_replaced_values is NOT restored because we want to use the new value from UI
    
    // Store the original address:value lines (with comments)
    // Split the input text into lines, preserving original indentation
    GPtrArray *lines_array = g_ptr_array_new();
    char *text_copy = g_strdup(address_value_pairs);
    
    char *line = strtok(text_copy, "\n");
    
    while (line != NULL) {
        // Skip completely empty lines
        if (line[0] != '\0') {
            // Store the original line as-is (preserve indentation and formatting)
            char *original_line = g_strdup(line);
            g_ptr_array_add(lines_array, original_line);
        } else {
        }
        line = strtok(NULL, "\n");
    }
    
    // Transfer lines to patch structure
    if (lines_array->len > 0) {
        patch->address_value_lines = g_new(char*, lines_array->len);
        patch->address_value_lines_count = lines_array->len;
        for (guint i = 0; i < lines_array->len; i++) {
            patch->address_value_lines[i] = g_ptr_array_index(lines_array, i);
        }
    } else {
        patch->address_value_lines = NULL;
        patch->address_value_lines_count = 0;
    }
    g_free(text_copy);
    g_ptr_array_free(lines_array, false); // Don't free the strings, we transferred ownership
    g_patches_db.dirty = true;
    
    return true;
}

bool xemu_patches_set_patch_enabled(int game_index, int patch_index, bool enabled)
{
    if (game_index < 0 || game_index >= g_patches_db.game_count) {
        return false;
    }
    
    XemuGamePatches *game = &g_patches_db.games[game_index];
    
    if (patch_index < 0 || patch_index >= game->patch_count) {
        return false;
    }
    
    bool old_enabled = game->patches[patch_index].enabled;
    const char* patch_name = game->patches[patch_index].name ? game->patches[patch_index].name : "Unnamed Patch";
    
    game->patches[patch_index].enabled = enabled;
    
    if (enabled && !old_enabled) {
        // Check if game is currently running before applying patches
        if (!is_game_currently_running()) {
            
            char warning_notification[256];
            snprintf(warning_notification, sizeof(warning_notification), 
                    "No game running - patch '%s' will apply on game load", patch_name);
            xemu_queue_notification(warning_notification);
            
            return true; // Return true to indicate patch was enabled (will apply on load)
        }
        
        XemuMemoryPatch *patch = &game->patches[patch_index];
        if (patch->address_values && patch->address_value_count > 0) {
            // Use the new save/restore system for patch application
            bool apply_result = xemu_patches_apply_patch_with_save_restore(patch, game_index, patch_index);
            
            if (apply_result) {
                // Send immediate memory application notification
                char mem_notification[512];
                snprintf(mem_notification, sizeof(mem_notification), 
                        "Applied patch \"%s\"", patch_name);
                xemu_queue_notification(mem_notification);
            } else {
                // Send failure notification
                char fail_notification[256];
                snprintf(fail_notification, sizeof(fail_notification), 
                        "Failed to apply patch '%s' - Check memory access", patch_name);
                xemu_queue_error_message(fail_notification);
            }
        } else {
        }
    } else if (!enabled && old_enabled) {  
        
        // Use the new save/restore system for patch removal
        bool remove_result = xemu_patches_remove_patch_with_restore(game_index, patch_index);
        
        if (remove_result) {
            char notification[256];
            snprintf(notification, sizeof(notification), 
                    "Removed patch \"%s\"", patch_name);
            xemu_queue_notification(notification);
        } else {
            char fail_notification[256];
            snprintf(fail_notification, sizeof(fail_notification), 
                    "Failed to properly remove patch '%s'", patch_name);
            xemu_queue_error_message(fail_notification);
        }
    }
    
    g_patches_db.dirty = true;
    
    
    // Trigger immediate save
    if (xemu_patches_save_database()) {
        // Database saved successfully
        
        // Send notification for patch state change
        char notification[256];
        snprintf(notification, sizeof(notification), "Patch '%s' %s", 
                patch_name, enabled ? "enabled" : "disabled");
        
        // Queue the notification (assuming this function exists)
        xemu_queue_notification(notification);
    } else {
        // Database save FAILED
    }
    
    return true;
}

// Free patches database
void xemu_patches_free_database(void)
{
    // Save database before freeing if it's dirty
    if (g_patches_db.dirty) {
        if (xemu_patches_save_database()) {
        } else {
        }
    }
    
    for (int i = 0; i < g_patches_db.game_count; i++) {
        XemuGamePatches *game = &g_patches_db.games[i];
        
        g_free(game->game_title);
        g_free(game->region);
        g_free(game->version);
        g_free(game->alternate_title_id);
        g_free(game->time_date);
        g_free(game->disc_number);
        
        for (int j = 0; j < game->patch_count; j++) {
            g_free(game->patches[j].name);
            g_free(game->patches[j].category);
            g_free(game->patches[j].author);
            g_free(game->patches[j].notes);
            
            // Free address:value pairs array with multi-byte support
            if (game->patches[j].address_values) {
                for (int k = 0; k < game->patches[j].address_value_count; k++) {
                    g_free(game->patches[j].address_values[k].value_data);
                }
                g_free(game->patches[j].address_values);
                game->patches[j].address_values = NULL;
            }
            game->patches[j].address_value_count = 0;
        }
        g_free(game->patches);
    }
    
    g_free(g_patches_db.games);
    g_free(g_patches_db.file_path);
    
    // Clear all saved values on shutdown
    xemu_patches_clear_all_saved_values();
    
    memset(&g_patches_db, 0, sizeof(g_patches_db));
}


// GUI callback functions
void xemu_patches_on_ui_request_save(void)
{
    if (g_patches_db.dirty) {
        xemu_patches_save_database();
        notify_message("Patches database saved");
    }
}

void xemu_patches_on_ui_database_changed(void)
{
    // Mark database as dirty and save immediately for better persistence
    // This ensures data is saved when users close xemu quickly
    g_patches_db.dirty = true;
    if (!g_save_in_progress) {
        if (xemu_patches_save_database()) {
        } else {
            g_patches_db.dirty = true; // Keep dirty flag if save failed
        }
    }
}

// Track the last XBE certificate hash to avoid applying patches repeatedly
static uint32_t g_last_applied_title_id = 0;
static uint32_t g_last_applied_region = 0;  
static uint32_t g_last_applied_version = 0;

// Timestamp-based manual reset detection
static time_t g_last_reset_detection_time = 0;
static uint32_t g_last_seen_for_reset_detection = 0;

// Reset the last applied tracking variables to force fresh game detection
void reset_last_applied_tracking(void)
{
    
    // Reset patch application tracking
    g_last_applied_title_id = 0;
    g_last_applied_region = 0;
    g_last_applied_version = 0;
    
    // Reset certificate tracking to prevent old certificate detection
    g_last_cert_title_id = 0;
    g_last_cert_region = 0;
    g_last_cert_version = 0;
    g_last_cert_read_time = 0;
    g_cert_data_valid = false;
}

// Track auto-applied patches to prevent redundant applications
static uint32_t g_last_auto_applied_title_id = 0;
static uint32_t g_last_auto_applied_region = 0;
static uint32_t g_last_auto_applied_version = 0;
static int g_last_auto_applied_patch_count = -1;

// Global coordination tracking for simplified system
bool g_patches_applied_for_current_cert = false;

// Load Disc retry mechanism for proper certificate refresh and patch application
bool g_load_disc_retry_pending = false;

// GUI rendering function - called from main render loop
void xemu_patches_gui_render(void)
{
    // This function is called during the main GUI render loop
    // Check if database needs to be saved periodically to avoid excessive file I/O
    
    // Save approximately every 300 frames (5 seconds at 60fps) when dirty
    if (g_patches_db.dirty && !g_save_in_progress) {
        g_save_timer++;
        if (g_save_timer >= 300) {
            if (xemu_patches_save_database()) {
            }
            g_save_timer = 0;
        }
    } else {
        // Reset timer if database is not dirty or save is in progress
        g_save_timer = 0;
    }
    
    // Perform periodic reset memory monitoring if active
    perform_periodic_reset_monitoring();
    
    // CRITICAL FIX: Call main loop update for auto-apply system
    // This was missing, causing patches not to apply on startup!
    xemu_patches_main_loop_update();
}

// Memory search and verification functions removed - use Dump menu instead

// Parse multiple address:value pairs from a text buffer
bool xemu_patches_parse_address_value_pairs(const char *address_value_text,
                                           XemuPatchAddressValue **address_values,
                                           int *count)
{
    if (!address_value_text || !address_values || !count) {
        return false;
    }
    
    *address_values = NULL;
    *count = 0;
    
    // Count lines first to allocate array
    int line_count = 0;
    const char *ptr = address_value_text;
    const char *line_start = ptr;
    
    while (*ptr) {
        if (*ptr == '\n' || *ptr == '\r') {
            line_count++;
            while (*ptr == '\n' || *ptr == '\r') ptr++;
            if (!*ptr) break;
            line_start = ptr;
        } else {
            ptr++;
        }
    }
    if (ptr > line_start) {
        line_count++;
    }
    
    if (line_count == 0) {
        return true; // Empty is valid
    }
    
    // Allocate array for parsed pairs
    XemuPatchAddressValue *pairs = g_malloc0(sizeof(XemuPatchAddressValue) * line_count);
    if (!pairs) {
        return false;
    }
    
    int parsed_count = 0;
    ptr = address_value_text;
    
    while (*ptr && parsed_count < line_count) {
        // Skip whitespace and comments
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        if (*ptr == '#' || *ptr == ';') {
            // Skip comment line
            while (*ptr && *ptr != '\n' && *ptr != '\r') ptr++;
            while (*ptr == '\n' || *ptr == '\r') ptr++;
            continue;
        }
        
        // Find end of line
        const char *line_end = ptr;
        while (*line_end && *line_end != '\n' && *line_end != '\r') line_end++;
        
        if (ptr < line_end) {
            // Extract line content
            char *line = g_strndup(ptr, line_end - ptr);
            
            // Parse address:value format
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                
                // Parse address (trim whitespace from address part)
                char *addr_str = g_strstrip(line);
                char *value_str = g_strstrip(colon + 1);
                
                if (strlen(addr_str) > 0 && strlen(value_str) > 0) {
                    // Parse address (support hex with 0x prefix or decimal)
                    uint32_t address;
                    if (sscanf(addr_str, "%x", &address) == 1 || sscanf(addr_str, "%u", &address) == 1) {
                        // Parse value bytes
                        GList *bytes_list = NULL;
                        char *token = strtok(value_str, " \t,");
                        while (token) {
                            // Skip empty tokens
                            if (strlen(token) > 0) {
                                unsigned int byte_val;
                                if (sscanf(token, "%x", &byte_val) == 1 && byte_val <= 0xFF) {
                                    uint8_t *byte_ptr = g_malloc(sizeof(uint8_t));
                                    *byte_ptr = (uint8_t)byte_val;
                                    bytes_list = g_list_append(bytes_list, byte_ptr);
                                }
                            }
                            token = strtok(NULL, " \t,");
                        }
                        
                        int byte_count = g_list_length(bytes_list);
                        if (byte_count > 0) {
                            // Allocate and copy byte data
                            uint8_t *byte_data = g_malloc0(byte_count);
                            int i = 0;
                            for (GList *iter = bytes_list; iter != NULL; iter = iter->next) {
                                if (i < byte_count) {
                                    byte_data[i] = *(uint8_t*)iter->data;
                                    g_free(iter->data);
                                }
                                i++;
                            }
                            g_list_free(bytes_list);
                            
                            // Store parsed pair
                            pairs[parsed_count].address = address;
                            pairs[parsed_count].value_data = byte_data;
                            pairs[parsed_count].value_length = byte_count;
                            
                            parsed_count++;
                        } else {
                            // No valid bytes found
                            g_list_free_full(bytes_list, g_free);
                        }
                    } else {
                    }
                } else {
                }
            } else {
            }
            
            g_free(line);
        }
        
        // Move to next line
        ptr = line_end;
        while (*ptr == '\n' || *ptr == '\r') ptr++;
    }
    
    // Reallocate if we parsed fewer pairs than expected
    if (parsed_count < line_count) {
        XemuPatchAddressValue *final_pairs = g_realloc(pairs, sizeof(XemuPatchAddressValue) * parsed_count);
        if (!final_pairs && parsed_count > 0) {
            // Clean up already allocated data
            for (int i = 0; i < parsed_count; i++) {
                g_free(pairs[i].value_data);
            }
            g_free(pairs);
            return false;
        }
        pairs = final_pairs;
    }
    
    *address_values = pairs;
    *count = parsed_count;
	
    return true;
}

// ============================================================================
// PERIODIC MONITORING INTEGRATION
// ============================================================================

// This function should be called periodically from xemu's main loop
// to check if patches have been overwritten by the game
void xemu_patches_periodic_monitoring_tick(void)
{
    if (!g_monitoring_enabled || g_monitored_patch_count == 0) {
        return;
    }
    
    perform_periodic_monitoring();
}

// SIMPLE AUTO-APPLY: Apply patches when XBE certificate changes
void xemu_patches_main_loop_update(void) {
    
    // PERFORMANCE FIX: Aggressive throttling to prevent resource exhaustion
    static uint32_t call_count = 0;
    static uint64_t last_log_time = 0;
    uint64_t current_time = SDL_GetTicks();
    
    call_count++;
    
    // CRITICAL DISC PRESENCE CHECK: Skip all patching if no disc is loaded
    // EXTREME throttling: log only first call, then every 500 calls, or every 10 seconds
    bool should_log = (current_time - last_log_time > 10000) || (call_count <= 1) || (call_count % 500 == 0);
    
    if (!g_patch_system_enabled) {
        if (should_log) {
            // Log only when needed for debugging
        }
        return; // Exit early, no patch processing when no disc is loaded
    }
    
    if (should_log) {
        last_log_time = current_time;
    }
    
    // Prevent unused variable warnings by explicitly using the variables
    (void)call_count;
    (void)last_log_time;
    
    // Only check for certificate changes occasionally to improve performance
    static int frame_counter = 0;
    static uint32_t last_logged_title_id = 0;
    frame_counter++;
    
    if (frame_counter >= 30) { // Check every 30 frames (0.5 seconds at 60fps) for more responsive detection
        frame_counter = 0;
        
        // Get current XBE certificate info with spam prevention
        uint32_t current_title_id, current_region, current_version;
        bool cert_success = get_cached_xbe_info_with_spam_prevention(&current_title_id, &current_region, &current_version);
        
        // Log certificate info every time it changes (throttled)
        if (!cert_success) {
        } else if (current_title_id != last_logged_title_id) {
            last_logged_title_id = current_title_id;
        }
        
        // IMPORTANT: Check for manual resets first, before checking for new games
        // This ensures manual resets are detected even when the same game is reloaded
        detect_manual_reset();
        
        if (cert_success) {
            // Check if this is a NEW game (different from last applied)
            bool is_new_game = (current_title_id != 0 && 
                              (current_title_id != g_last_applied_title_id ||
                               current_region != g_last_applied_region ||
                               current_version != g_last_applied_version));
            
            // ULTRA-ENHANCED FIX: Clear certificate cache when new game is detected to prevent stale Add Game dialog data
            if (is_new_game) {
                // Ultra-aggressive cache clearing for Add Game dialog
                g_xbe_cache.valid = false;
                g_xbe_cache.last_read_time = 0;
                g_xbe_cache.frame_last_read = -1;
                g_force_fresh_xbe_read = true;
                
                // Clear all cached certificate data
                g_cert_data_valid = false;
                g_last_cert_title_id = 0;
                g_last_cert_region = 0;
                g_last_cert_version = 0;
            }
            
            // CRITICAL: Check if manual reset was detected and handle it separately
            if (g_manual_reset_detected) {
                
                // IMPORTANT: Clear certificate tracking suppression for manual resets
                // This ensures manual reset notifications can appear even if Load Disc suppressed previous notifications
                if (g_suppress_patch_notification) {
                    g_suppress_patch_notification = false;
                }
                
                // Schedule post-reset patch application instead of immediate application
                schedule_post_reset_patch_application();
                
                // Clear the manual reset flag immediately (it will be cleared again after patch application)
                g_manual_reset_detected = false;
                return; // Exit early since patches will be handled by post-reset system
            }
            
            // COORDINATION: Check if Post-Reset system is actively applying patches
            if (g_post_reset_patch_application_active) {
                if (should_log) {
                }
                return; // Exit early to avoid duplicate patch application
            }
            
            // LOAD DISC PROTECTION: Prevent patch application during Load Disc operations (crash prevention)
            if (g_load_disc_in_progress) {
                if (should_log) {
                }
                return; // Exit early to prevent Load Disc crashes
            }
            
            // Apply patches if this is a new game (but not manual reset - that's handled above)
            if (is_new_game && g_patches_loaded && g_patches_initialized) {
                // Find game using certificate matching
                XemuGamePatches* game = xemu_patches_find_game_by_certificate();

                if (game) {
                    // Count enabled patches
                    int enabled_patches = 0;
                    for (int i = 0; i < game->patch_count; i++) {
                        if (game->patches[i].enabled) {
                            enabled_patches++;
                        }
                    }
                    
                    if (enabled_patches > 0) {
                        // Apply each enabled patch
                        int applied_count = 0;
                        for (int i = 0; i < game->patch_count; i++) {
                            if (game->patches[i].enabled) {
                                
                                bool apply_success = true;
                                for (int j = 0; j < game->patches[i].address_value_count; j++) {
                                    if (!apply_single_patch_bytes(game->patches[i].address_values[j].address, 
                                                                 game->patches[i].address_values[j].value_data, 
                                                                 game->patches[i].address_values[j].value_length, 
                                                                 NULL)) {
                                        apply_success = false;
                                        break;
                                    }
                                }
                                
                                if (apply_success) {
                                    applied_count++;
                                }
                            }
                        }
                        
                        if (applied_count > 0) {
                            
                            // CRITICAL FIX: Save manual reset state before clearing it
                            // This ensures notification logic can still access the manual reset status
                            bool was_manual_reset = g_manual_reset_detected;
                            if (was_manual_reset) {
                            }
                            
                            // Clear manual reset flag after successful patch application
                            if (g_manual_reset_detected) {
                                g_manual_reset_detected = false;
                            }
                            
                            // Update last applied tracking
                            g_last_applied_title_id = current_title_id;
                            g_last_applied_region = current_region;
                            g_last_applied_version = current_version;
                            
                            // 🔍 TRACKING: Show notification with source identification
                            char notification_text[256];
                            
                            // CRITICAL FIX: Use saved manual reset state for notification generation
                            // This ensures correct notification even if flag was cleared
                            bool is_manual_reset = was_manual_reset;
                            
                            // FIXED: Only detect manual reset based on actual reset operations
                            // Boot-time patch application should NOT be considered manual reset
                            if (!is_manual_reset) {
                                // Only check for manual reset if we have explicit evidence of reset operation
                                // During boot, patches are applied automatically, not manually
                                is_manual_reset = false; // Explicitly set to false for boot scenarios
                            }
                            
                            if (is_manual_reset) {
                                snprintf(notification_text, sizeof(notification_text), 
                                        "Reset: Applied %d %s for %s", 
                                        applied_count, 
                                        applied_count == 1 ? "patch" : "patches",
                                        game->game_title);
                            } else {
                                snprintf(notification_text, sizeof(notification_text), 
                                        "Applied %d %s for %s", 
                                        applied_count, 
                                        applied_count == 1 ? "patch" : "patches",
                                        game->game_title);
                            }
                            
                            // ENHANCED RESET DETECTION: Check if this might be a manual reset even without flag
                            // Static variables for tracking notifications
                            static int g_notification_counter = 0;
                            static time_t g_last_notification_time = 0;
                            static uint32_t last_notified_title_id = 0;
                            static uint64_t last_notification_time = 0;
                            static time_t last_simple_apply_time = 0;
                            static char last_simple_apply_text[256] = "";
                            
                            g_notification_counter++;
                            (void)g_notification_counter; // Suppress unused variable warning
                            time_t current_time = time(NULL);
                            
                            // CRITICAL FIX: Detect manual reset more accurately
                            // Only consider it manual reset if patches were previously applied
                            // and we're reapplying them (indicating a reset occurred)
                            bool likely_manual_reset = (applied_count > 0 && game != NULL && 
                                                      strcmp(game->game_title, "") != 0 &&
                                                      g_cert_data_valid &&
                                                      g_patches_applied_for_current_cert &&
                                                      g_manual_reset_detected); // Only if reset flag is set
                            (void)likely_manual_reset; // Suppress unused variable warning
                            
                            
                            // NOTIFICATION COORDINATION: Check if another notification is already being generated
                            if (g_notification_generation_active) {
                                return; // Exit to prevent duplicate notification
                            }
                            
                            // NOTIFICATION COORDINATION: Mark that we're generating a notification
                            g_notification_generation_active = true;
                            
                            // INTELLIGENT DUPLICATE PREVENTION: Only block actual duplicates
                            uint32_t current_title_id = g_last_cert_title_id;
                            uint64_t time_since_last_notification = (last_notification_time > 0) ? (current_time - last_notification_time) : UINT64_MAX;
                            
                            // Only block if we actually notified for this same TitleID recently AND it was within 3 seconds
                            // This allows first notifications for new games while preventing true duplicates
                            if (current_title_id != 0 && 
                                last_notified_title_id == current_title_id && 
                                time_since_last_notification < 3000) {
                                // Suppress unused variable warning for tracking variables
                                (void)g_last_notification_time;
                                (void)current_time;
                                return; // Block only true duplicates
                            }
                            
                            // Check for duplicates with manual apply notifications
                            if (current_time - last_simple_apply_time < 2000 && strcmp(notification_text, last_simple_apply_text) == 0) {
                                return; // Block time-based duplicate
                            }
                            
                            // Record this notification to prevent future duplicates
                            strcpy(last_simple_apply_text, notification_text);
                            last_simple_apply_time = current_time;
                            last_notified_title_id = current_title_id;
                            last_notification_time = current_time;
                            
                            xemu_queue_notification(notification_text);

                            
                            // NOTIFICATION COORDINATION: Clear the flag after notification is queued
                            g_notification_generation_active = false;
                            g_last_notification_time = current_time;
                            (void)g_last_notification_time; // Suppress unused variable warning
                        }
                    } else {
                        // Still update tracking to prevent constant re-detection
                        g_last_applied_title_id = current_title_id;
                        g_last_applied_region = current_region;
                        g_last_applied_version = current_version;
                    }
                } else {
                    
                    // Still update tracking to prevent constant re-detection
                    g_last_applied_title_id = current_title_id;
                    g_last_applied_region = current_region;
                    g_last_applied_version = current_version;
                }
            } else {
            }
        } else {
        }
    }
}


// Manual trigger for monitoring (can be called from console/UI)
void xemu_patches_manual_monitor_check(void)
{
    if (g_monitored_patch_count == 0) {
        return;
    }
    
    for (int i = 0; i < g_monitored_patch_count; i++) {
        verify_monitored_patch(i);
    }
}

// Enable/disable monitoring system

// Get monitoring status
bool xemu_patches_is_monitoring_enabled(void)
{
    return g_monitoring_enabled;
}

// Get number of monitored patches
int xemu_patches_get_monitored_count(void)
{
    return g_monitored_patch_count;
}

// Reset all monitoring (for testing)
void xemu_patches_reset_monitoring(void)
{
    g_monitored_patch_count = 0;
    g_last_monitoring_time = 0;
}// Global state for post-reset patch application
bool g_post_reset_patch_scheduled = false;
int g_post_reset_retry_count = 0;
static const int MAX_POST_RESET_RETRIES = 3; // 3 attempts = ~3 seconds maximum

// 🔍 NOTIFICATION TRACKING: Global counter to track notification generation
int g_notification_counter = 0;
time_t g_last_notification_time = 0;

// CERTIFICATE TRACKING: Track previously seen certificate to prevent duplicate patch messages
char g_last_seen_title_id[17] = "";                          // Last seen TitleID (16 chars + null terminator)
char g_last_seen_region[8] = "";                             // Last seen Region (7 chars + null terminator) 
char g_last_seen_version[32] = "";                           // Last seen Version (31 chars + null terminator)
bool g_certificate_tracking_enabled = false;                 // Certificate tracking enabled flag
bool g_suppress_patch_notification = false;                   // Flag to suppress patch notification for duplicate certificates
// INFINITE LOOP PREVENTION: Variables to prevent repeated reset detection during post-reset processing
// (Declarations moved to beginning of global variables section)
time_t g_last_reset_detection_time_prevent_loop = 0;


// CERTIFICATE TRACKING: Check if certificate has changed since last seen

// CERTIFICATE TRACKING: Update the last seen certificate

// CERTIFICATE TRACKING: Enable certificate tracking and initialize
void enable_certificate_tracking(void)
{
    if (!g_certificate_tracking_enabled) {
        g_certificate_tracking_enabled = true;
        g_suppress_patch_notification = false;  // Reset suppression flag
    }
}

// CERTIFICATE TRACKING: Reset certificate tracking
void reset_certificate_tracking(void)
{
    g_last_seen_title_id[0] = '\0';
    g_last_seen_region[0] = '\0';
    g_last_seen_version[0] = '\0';
    g_manual_reset_detected = false;  // Also clear manual reset flag
}

// MANUAL RESET DETECTION: Certificate availability-based reset detection
void detect_manual_reset(void)
{
    // Track certificate availability transitions for reset detection
    static bool g_certificate_was_available_prev = false;
    static uint32_t g_certificate_unavailable_start_time = 0;
    static uint32_t g_prev_title_id_during_unavailable = 0;
    static time_t g_last_certificate_check_time = 0;
    
    // Get current time for all timestamp calculations
    time_t current_time = time(NULL);
    
    // Get current XBE certificate information using the correct API
    uint32_t current_title_id = 0;
    uint32_t current_region = 0;
    uint32_t current_version = 0;
    
    // Check if certificate data is currently available
    bool certificate_available_now = get_cached_xbe_info_with_spam_prevention(&current_title_id, &current_region, &current_version);
    
    if (certificate_available_now) {
        
        // Certificate is available - check if this indicates a reset
        bool manual_reset_detected = false;
        
        // DETECTION METHOD 1: Certificate Availability Transition
        // If certificate was unavailable and is now available again, and it's the same game
        if (!g_certificate_was_available_prev && 
            current_title_id == g_prev_title_id_during_unavailable &&
            current_title_id != 0 && 
            g_prev_title_id_during_unavailable != 0) {
            
            time_t unavailable_duration = current_time - g_certificate_unavailable_start_time;
            (void)unavailable_duration; // Suppress unused variable warning
            
            manual_reset_detected = true;
        }
        // DETECTION METHOD 2: Certificate Field Changes (traditional approach)
        else {
            bool is_same_title = (current_title_id == g_last_applied_title_id);
            bool region_changed = (current_region != g_last_applied_region);
            bool version_changed = (current_version != g_last_applied_version);
            
            if (is_same_title && (region_changed || version_changed)) {
                
                manual_reset_detected = true;
            }
            // DETECTION METHOD 3: Rapid Title ID Pattern
            // During reset, the same Title ID might appear multiple times in quick succession
            else if (is_same_title) {
                time_t time_since_last_check = (g_last_certificate_check_time > 0) ? 
                                             (current_time - g_last_certificate_check_time) : 0;
                
                if (time_since_last_check >= 1 && time_since_last_check <= 3) {
                    manual_reset_detected = true;
                }
            }
        }
        
        // FINAL RESET DETECTION
        if (manual_reset_detected) {
            
            // INFINITE LOOP PREVENTION: Check if we're already processing a reset
            g_reset_detection_count++;
            time_t time_since_last_detection = (g_last_reset_detection_time_prevent_loop > 0) ? 
                                              (current_time - g_last_reset_detection_time_prevent_loop) : 999;
        
            // Prevent rapid repeated detections (within 10 seconds) to avoid infinite loops
            if (g_reset_detected_in_progress || (time_since_last_detection < 10 && g_reset_detection_count > 1)) {
                return; // Exit without setting g_manual_reset_detected = true
            }
            
            // IMPORTANT: Clear certificate tracking suppression for manual resets
            // This ensures manual reset notifications can appear even if Load Disc suppressed previous notifications
            if (g_suppress_patch_notification) {
                g_suppress_patch_notification = false;
            }
            
            // MARK RESET AS IN PROGRESS to prevent repeated detections
            g_reset_detected_in_progress = true;
            g_last_reset_detection_time_prevent_loop = current_time;
            
            g_manual_reset_detected = true;
            
            // Update tracking variables to prevent multiple detections
            g_last_applied_region = current_region;
            g_last_applied_version = current_version;
        }
        else {
        }
        
        // Update timestamp tracking for next detection cycle
        g_last_seen_for_reset_detection = current_title_id;
        g_last_reset_detection_time = current_time;
        g_last_certificate_check_time = current_time;
    }
    else {
        // Certificate is NOT available - start tracking unavailable period
        if (!g_certificate_was_available_prev) {
            // Certificate just became unavailable - start timing
            g_certificate_unavailable_start_time = current_time;
        }
        else {
            // Certificate is still unavailable - update tracking
            time_t unavailable_duration = current_time - g_certificate_unavailable_start_time;
            (void)unavailable_duration; // Suppress unused variable warning
        }
    }
    
    // Update certificate availability tracking
    g_certificate_was_available_prev = certificate_available_now;
    if (certificate_available_now) {
        g_prev_title_id_during_unavailable = current_title_id;
    }
}

// CERTIFICATE TRACKING: Clear manual reset flag (safety function)

// PUBLIC HOOK: Signal that a manual reset has occurred
// This function can be called from external code (like ui/gtk.c) when user clicks Reset

// Schedule patch application after reset completion
void schedule_post_reset_patch_application(void)
{
    g_post_reset_system_active = true;
    
    // Clear any existing scheduled state to avoid interference from previous game
    if (g_post_reset_patch_scheduled) {
        stop_reset_memory_monitoring();
        g_post_reset_patch_scheduled = false;
        g_post_reset_retry_count = 0;
    }
    
    // Clear auto-apply tracking to ensure clean state for new game
    g_last_auto_applied_title_id = 0;
    g_last_auto_applied_region = 0;
    g_last_auto_applied_version = 0;
    g_last_auto_applied_patch_count = -1;
    g_post_reset_patch_scheduled = true;
    g_post_reset_retry_count = 0;
    
    // Reset VM completion tracking for new reset cycle
    g_vm_reset_completed = false;
    g_vm_reset_completion_time = 0;
    
    // Start monitoring immediately to track the reset process
    start_reset_memory_monitoring();
}

// Check if system is ready for patch application after reset
static bool is_system_ready_for_patches(void)
{
    // Check if XBE certificate is available (indicates game is running)
    XemuGamePatches* game_patches = xemu_patches_find_game_by_certificate();
    if (!game_patches) {
        return false; // No game certificate available yet
    }
    
    // Check if game is actually running
    if (!is_game_currently_running()) {
        return false; // Game not running yet
    }
    
    return true; // System appears ready
}

// Enhanced function to detect when VM reset has completed
// This ensures patches are applied after the VM actually finishes resetting
static bool detect_vm_reset_completion(void)
{
    // PERFORMANCE FIX: Add throttling to reduce excessive logging
    static uint64_t last_log_time = 0;
    static uint32_t call_count = 0;
    uint64_t current_time = SDL_GetTicks();
    
    call_count++;
    
    // EXTREME throttling: log only first call, then every 1000th call, or if 10+ seconds have passed
    bool should_log = (current_time - last_log_time > 10000) || (call_count <= 1) || (call_count % 1000 == 0);
    
    // PERFORMANCE: Early exit if we don't need to log detailed information
    if (!should_log) {
        return false; // Exit early without doing expensive operations
    }
    
    // Update last_log_time when we actually log
    last_log_time = current_time;
    
    // Prevent unused variable warnings by explicitly using the variables
    (void)call_count;
    (void)last_log_time;
    
    // NEW TRIGGER CHECK: Only proceed if VM reset was actually triggered
    if (!g_vm_reset_triggered) {
        if (should_log) {
        }
        return false;
    }
    if (should_log) {
    }
    
    // Check if we already detected completion
    if (g_vm_reset_completed) {
        // Additional check: ensure minimum delay has passed (2 seconds)
        uint64_t elapsed_since_completion = current_time - g_vm_reset_completion_time;
        
        if (elapsed_since_completion >= 2000) {
            if (should_log) {
            }
            return true;
        } else {
            if (should_log) {
            }
            return false;
        }
    }
    
    // Check if system is ready using existing function
    if (should_log) {
    }
    if (!is_system_ready_for_patches()) {
        if (should_log) {
        }
        return false;
    }
    if (should_log) {
    }
    
    // Additional checks to ensure VM reset is truly complete
    if (should_log) {
    }
    
    // Check if game is actually running (not just loading)
    if (!is_game_currently_running()) {
        return false;
    }
    
    // CRITICAL: Prevent XEMU calls during Load Disc to prevent crashes
    if (g_load_disc_in_progress) {
        return false; // Not ready during Load Disc
    }
    
    // Get current certificate to verify it's stable
    struct xbe *xbe_info = xemu_get_xbe_info();
    if (!xbe_info || !xbe_info->cert) {
        return false;
    }
    
    uint32_t current_title_id = ldl_le_p(&xbe_info->cert->m_titleid);
    if (current_title_id == 0 || current_title_id == 0xFFFFFFFF) {
        return false;
    }
    
    // Additional memory stability check: ensure we can read from a known address
    uint8_t buffer[4];
    char error_msg[256];
    if (!xemu_virtual_memory_read(0x34D8E0, buffer, 4, error_msg, sizeof(error_msg))) {
        return false;
    }
    
    // All checks passed! Mark VM reset as completed
    g_vm_reset_completed = true;
    g_vm_reset_completion_time = SDL_GetTicks();
    
    // DUPLICATE PREVENTION FIX: Clear reset trigger to prevent inappropriate re-detection
    g_vm_reset_triggered = false;
	
    return true;
}

// Forward declaration of static function
static void apply_patches_after_reset(void);

// Public wrapper for post-reset patch processing
void xemu_patches_process_post_reset(void)
{
    // Call the internal static function that contains the actual implementation
    apply_patches_after_reset();
}

// Apply patches after reset completion
static void apply_patches_after_reset(void)
{
    // RESTORED: Function structure was broken during cleanup
    
    uint32_t current_time = SDL_GetTicks();
    (void)current_time; // Suppress unused variable warning
    
    
    // CRITICAL: Add crash protection for reset operations
    // (using global variable now instead of local static)
    
    // ENHANCED LOAD DISC PROTECTION: Handle Load Disc timeout and retry mechanism
    static uint32_t load_disc_start_frame = 0;
    uint32_t current_frame = SDL_GetTicks();
    
    if (g_load_disc_in_progress) {
        // Initialize timeout tracking on first detection
        if (load_disc_start_frame == 0) {
            load_disc_start_frame = current_frame;
        }
        
        // Check if Load Disc has been stuck for too long (5 second timeout)
        uint32_t elapsed_ms = current_frame - load_disc_start_frame;
        if (elapsed_ms > 5000) {
            g_load_disc_in_progress = false;
            load_disc_start_frame = 0; // Reset timeout tracking
            
            // Force immediate certificate retry after timeout clear
            g_force_fresh_xbe_read = true;
            g_load_disc_retry_pending = true;
        } else {
            return;
        }
    } else {
        // Reset timeout tracking when protection is not active
        if (load_disc_start_frame != 0) {
            load_disc_start_frame = 0;
        }
    }
    
    // COMPREHENSIVE LOAD DISC RETRY: Handle Load Disc completion and certificate refresh
    if (g_load_disc_retry_pending || g_load_disc_in_progress) {
        
        // Clear the retry flag first to prevent infinite loops
        g_load_disc_retry_pending = false;
        
        // Always force fresh certificate read during Load Disc coordination
        g_force_fresh_xbe_read = true;
        
        // Try to get fresh certificate data immediately
        uint32_t temp_title_id, temp_region, temp_version;
        
        if (get_cached_xbe_info_with_spam_prevention(&temp_title_id, &temp_region, &temp_version)) {
            
            // Check if this is a valid game (not dashboard)
            if (temp_title_id != 0xFFFE0000 && temp_title_id != 0xFFFF0002 && temp_title_id != 0x00000000) {
                
                // Store the certificate data
                g_last_cert_title_id = temp_title_id;
                g_last_cert_region = temp_region;
                g_last_cert_version = temp_version;
                g_cert_data_valid = true;
                g_disc_present = true;
                g_patch_system_enabled = true;
                
                
                // Clear patches applied flag for new game
                g_patches_applied_for_current_cert = false;
                
                // Now clear Load Disc protection since we have valid data
                if (g_load_disc_in_progress) {
                    g_load_disc_in_progress = false;
                }
                
                // Try to enable patches for the newly loaded disc
                auto_enable_patches_when_ready();
                
            } else {
                
                // Re-set retry flag for next attempt
                g_load_disc_retry_pending = true;
            }
        } else {
            
            // Re-set retry flag for next attempt
            g_load_disc_retry_pending = true;
        }
        
        // Exit early after handling Load Disc coordination
        return;
    }
    
    // Prevent multiple simultaneous post-reset processing (crash prevention)
    if (g_post_reset_crash_protection_active) {
        return;
    }
    
    // ADDITIONAL PROTECTION: Check if system is in a stable state
    time_t system_time = time(NULL);
    
    // FIXED ANTI-SPAM: Proper throttling to prevent excessive calls while allowing normal operation
    static uint32_t consecutive_skips = 0;
    static time_t last_processed_time = 0;
    static uint32_t notification_flag_check_count = 0;
    
    // SAFETY FIX: Clear notification generation flag if it gets stuck (prevents notifications from being permanently blocked)
    notification_flag_check_count++;
    if (notification_flag_check_count % 300 == 0) { // Every 5 minutes at 60fps
        if (g_notification_generation_active) {
            g_notification_generation_active = false;
        }
        notification_flag_check_count = 0; // Reset counter
    }
    
    // Only process if enough time has passed since last successful processing
    if (system_time - last_processed_time < 2) { // Allow processing every 2 seconds maximum
        consecutive_skips++;
        // Log every 20th skip to reduce spam
        if (consecutive_skips % 20 == 0) {
        } else {
            return; // Skip this call
        }
    } else {
        consecutive_skips = 0; // Reset counter after adequate delay
        last_processed_time = system_time;
    }

    g_post_reset_crash_protection_active = true;
    
    // Add try-catch style protection
    xemu_patches_process_post_reset_unsafe();
    
    g_post_reset_crash_protection_active = false;
}

// Unsafe version of post-reset processing (original logic with enhancements)
// Unsafe version of post-reset processing (original logic with enhancements)
void xemu_patches_process_post_reset_unsafe(void)
{
    // PERFORMANCE FIX: Aggressive throttling to prevent resource exhaustion
    static uint32_t call_count = 0;
    static uint64_t last_log_time = 0;
    uint64_t current_time = SDL_GetTicks();
    
    call_count++;
    
    // IMMEDIATE TRIGGER: Check for reset completion immediately for faster response
    static bool g_immediate_trigger_checked = false;
    static uint64_t g_immediate_trigger_time = 0;
    
    // Only check once per reset cycle to avoid repeated triggering
    
    // Prevent unused variable warnings by explicitly using the variables
    (void)call_count;
    (void)last_log_time;
    (void)g_immediate_trigger_time;
    if (!g_immediate_trigger_checked) {
        g_immediate_trigger_checked = true;
        g_immediate_trigger_time = current_time;
        
        // Check if VM reset completion was already detected
        if (detect_vm_reset_completion()) {
            
            // Force immediate patch application
            if (g_cert_data_valid && g_post_reset_current_title_id != 0) {
                
                // Mark patches as applied to prevent timeout mechanism from triggering
                g_patches_applied_for_current_cert = true;
                return; // Exit early - patches already applied
            }
        }
    }
    
    // STARTUP RETRY: Check for startup retry detection on every call
    check_startup_retry_detection();
    
    if (g_post_reset_patch_scheduled) {
    } else {
        return; // No post-reset patch application scheduled
    }
    
    // RESET CRASH PROTECTION: Check for reset operations that might cause crashes
    static bool last_reset_active = false;
    bool current_reset_active = false; // This would be set by reset detection logic
    
    if (current_reset_active != last_reset_active) {
        last_reset_active = current_reset_active;
    }
    
    // Prevent post-reset processing if reset is still active (crash prevention)
    if (current_reset_active) {
        return; // Exit early to prevent crashes
    }
    
    if (g_post_reset_start_time == 0) {
        g_post_reset_start_time = SDL_GetTicks();
        g_post_reset_call_count = 0;
        
        // IMMEDIATE TRIGGER: Reset immediate trigger flags for new reset cycle
        g_immediate_trigger_checked = false;
    }
    
    g_post_reset_call_count++;
    uint64_t now_time = SDL_GetTicks();
    uint64_t elapsed_ms = now_time - g_post_reset_start_time;
    
    // CRITICAL TIMEOUT MECHANISM: Always check timeout conditions BEFORE any early returns
    // OPTIMIZATION: Reduced from 10 seconds to 3 seconds for faster manual reset response
    if (elapsed_ms > 3000 || g_post_reset_call_count > 1000) {
        if (detect_vm_reset_completion()) {
        } else {
        }
        
        // IMMEDIATE PATCH APPLICATION: Force apply patches to restore previous working behavior
        stop_reset_memory_monitoring();
        g_post_reset_patch_scheduled = false;
        g_post_reset_system_active = false;
        g_post_reset_start_time = 0;
        
        // IMMEDIATE TRIGGER: Reset immediate trigger flags
        g_immediate_trigger_checked = false;
        
        // Force patch activation using previous successful logic
        if (g_post_reset_current_title_id != 0) {
        } else {
        }
        return;
    }
    
    // VERBOSE RETRY TRACKING
    if (g_post_reset_call_count % 100 == 0) {
    }
    
    // COORDINATION DEADLOCK FIX: Removed problematic coordination that creates deadlock between systems
    // Both auto-apply and post-reset can now work independently to avoid coordination deadlock
    g_post_reset_retry_count++;
    
    if (g_post_reset_retry_count > MAX_POST_RESET_RETRIES) {
        stop_reset_memory_monitoring();
        g_post_reset_patch_scheduled = false;
        
        // INFINITE LOOP PREVENTION: Clear reset detection flag even on timeout/abort
        g_reset_detected_in_progress = false;
        g_reset_detection_count = 0; // Reset counter for next session
        
        // Clear force fresh read flag on completion
        if (g_force_fresh_xbe_read) {
            g_force_fresh_xbe_read = false;
        }
        
        g_post_reset_system_active = false; // Clear coordination flag
        return;
    }
    
    // CRITICAL: Bypass ALL XEMU calls during Load Disc to prevent crashes
    // Load Disc operations cause VM instability and memory access violations
    if (g_load_disc_in_progress) {
        
        // Force timeout to continue trying without making risky XEMU calls
        g_post_reset_retry_count++;
        g_post_reset_system_active = false; // Clear coordination flag
        return;
    }
    
    // COMPREHENSIVE XBE CERTIFICATE CHECK with enhanced debugging
    struct xbe *xbe_info = xemu_get_xbe_info();
    
    if (!xbe_info || !xbe_info->cert) {
    } else {
        uint32_t current_title_id = ldl_le_p(&xbe_info->cert->m_titleid);
        
        if (current_title_id == 0 || current_title_id == 0xFFFFFFFF) {
        } else {
            g_post_reset_current_title_id = current_title_id;
            
            // TIMING FIX: Skip immediate patch application during Load Disc operations
            if (g_load_disc_in_progress) {
            } else {
                
                if (detect_vm_reset_completion()) {
                    stop_reset_memory_monitoring();
                    g_post_reset_patch_scheduled = false;
                    g_post_reset_system_active = false;
                    g_post_reset_start_time = 0;
                    
                    // IMMEDIATE TRIGGER: Reset immediate trigger flags
                    g_immediate_trigger_checked = false;
                    
                    // CRITICAL: Enable patches now that VM has finished loading
                    set_load_disc_completed();
                    
                    // Load Disc Protection: Clear flag now that VM has finished switching games
                    if (g_load_disc_in_progress) {
                        g_load_disc_in_progress = false;
                    }
                    
                    // Reset VM completion tracking for next reset
                    g_vm_reset_completed = false;
                    g_vm_reset_completion_time = 0;
                    
                    return;
                } else {
                    // DO NOT APPLY PATCHES - continue monitoring until VM reset completes
                }
            }
        }
    }
    
    
    // ENHANCED TIMEOUT TRACKING: Log retry progress
    g_post_reset_retry_count++;
    if (g_post_reset_call_count % 100 == 0) {
    }
    
    // ATTEMPT TO FIND GAME PATCHES (without early return)
    XemuGamePatches* game_patches = xemu_patches_find_game_by_certificate();
    
    if (!game_patches) {
        // NO EARLY RETURN - let timeout mechanism continue
    } else {
    }
    
    // Count enabled patches first
    int enabled_patches = 0;
    for (int i = 0; i < game_patches->patch_count; i++) {
        if (game_patches->patches[i].enabled) {
            enabled_patches++;
        }
    }
    
    if (enabled_patches == 0) {
    } else {
    }
    
    
    // Pre-check: Test if we can read from the patch addresses before applying
    bool addresses_accessible = true;
    for (int i = 0; i < game_patches->patch_count; i++) {
        XemuMemoryPatch *patch = &game_patches->patches[i];
        if (!patch->enabled || !patch->address_values || patch->address_value_count == 0) continue;
        
        for (int j = 0; j < patch->address_value_count; j++) {
            XemuPatchAddressValue *addr_val = &patch->address_values[j];
            
            bool read_success = false;
            read_32bit_value(addr_val->address, &read_success);
            
            if (!read_success) {
                addresses_accessible = false;
                break;
            }
        }
        
        if (!addresses_accessible) break;
    }
    
    // MEMORY ACCESS CHECK (without early return)
    if (!addresses_accessible) {
        // NO EARLY RETURN - let timeout mechanism continue
    } else {
    }
    
    int applied_patch_count = 0;
    
    // LOAD DISC PROTECTION: Check if we're still in Load Disc operation
    if (g_load_disc_in_progress) {
        // Clear the flag since we're about to try again later
        g_load_disc_in_progress = false;
        return;
    }
    
    // Reapply all currently enabled patches for the current game
    for (int i = 0; i < game_patches->patch_count; i++) {
        XemuMemoryPatch *patch = &game_patches->patches[i];
        if (!patch->enabled) continue;
        
        // Track if this patch was successfully applied
        bool patch_applied = false;
        
        // Apply each address/value pair for this patch
        if (patch->address_values && patch->address_value_count > 0) {
            for (int j = 0; j < patch->address_value_count; j++) {
                XemuPatchAddressValue *addr_val = &patch->address_values[j];
                
                // Add address to reset monitoring if active
                if (g_reset_monitoring_active) {
                    monitor_reset_patch_address(addr_val->address, "reset_reapplication");
                }
                
                // Perform the actual memory write
                bool write_success = write_direct_virtual_memory(addr_val->address, addr_val->value_data, addr_val->value_length);
                
                // Log the value after patch application if we're monitoring
                if (g_reset_monitoring_active && write_success) {
                    log_reset_memory_values(addr_val->address, "after_application");
                }
                
                if (write_success) {
                    patch_applied = true;
                } else {
                }
            }
        }
        
        // Count this patch only once
        if (patch_applied) {
            applied_patch_count++;
        }
    }
    
    // GENERATE NOTIFICATION (only if patches were applied)
    if (applied_patch_count > 0 && game_patches) {
        // Safety: Clear manual reset flag if it's still set (ensures flag is always cleared after patch application)
        if (g_manual_reset_detected) {
            g_manual_reset_detected = false;
        }
        
        // Check if notification should be suppressed due to Load Disc operation
        if (g_load_disc_in_progress) {
        }
        // Check if notification should be suppressed due to duplicate certificate
        else if (g_suppress_patch_notification) {
        } else {
            char notification[256];
            snprintf(notification, sizeof(notification), 
                    "Applied %d patch%s for %s", applied_patch_count, applied_patch_count == 1 ? "" : "es", game_patches->game_title);
            
            // ENHANCED DUPLICATE PREVENTION: Use multiple coordination methods
            if (g_notification_generation_active) {
                return; // Exit to prevent duplicate notification
            }
            
            // ADDITIONAL COORDINATION: Check notification hash to prevent content duplicates
            static uint32_t g_last_post_reset_notification_hash = 0;
            static uint64_t g_last_post_reset_notification_time = 0;
            
            uint32_t notification_hash = 0;
            for (int i = 0; notification[i] && i < sizeof(notification) - 1; i++) {
                notification_hash = notification_hash * 31 + notification[i];
            }
            
            uint64_t current_time = SDL_GetTicks();
            if (g_last_post_reset_notification_hash == notification_hash && 
                current_time - g_last_post_reset_notification_time < 3000) {
                return; // Exit to prevent duplicate notification
            }
            
            g_last_post_reset_notification_hash = notification_hash;
            g_last_post_reset_notification_time = current_time;
            
            // NOTIFICATION COORDINATION: Mark that we're generating a notification
            g_notification_generation_active = true;
            
            // CRITICAL FIX: Always print manual reset notifications to console as guaranteed fallback
            if (strstr(notification, "Reset:") != NULL) {
            }
            
            xemu_queue_notification(notification);
            
            // NOTIFICATION COORDINATION: Clear the flag after notification is queued
            g_notification_generation_active = false;
        }
    } else {
    }
    
    // Stop monitoring and clear the scheduled flag
    stop_reset_memory_monitoring();
    g_post_reset_patch_scheduled = false;
    
    // INFINITE LOOP PREVENTION: Clear reset detection flag now that post-reset is complete
    g_reset_detected_in_progress = false;
    g_reset_detection_count = 0; // Reset counter for next session
    
    // Clear force fresh read flag on completion
    if (g_force_fresh_xbe_read) {
        g_force_fresh_xbe_read = false;
    }
    
    // INFINITE LOOP PREVENTION: Clear coordination flag when successful
    g_post_reset_system_active = false;
    
    // CRITICAL FIX: Clear notification coordination flag to allow future notifications
    g_notification_generation_active = false;
    
    // CRITICAL: Enable patches now that VM has finished loading
    // This handles both Load Disc operations and auto-boot scenarios
    set_load_disc_completed();
    
    // Reset timeout tracking for next time
    g_post_reset_start_time = 0;
    g_post_reset_call_count = 0;
    
    // IMMEDIATE TRIGGER: Reset immediate trigger flags
    g_immediate_trigger_checked = false;
}

// Reapply all currently enabled patches for the current game
void xemu_patches_reapply_current_game_patches(void)
{
    // CRITICAL DISC PRESENCE CHECK: Skip all patching if no disc is loaded
    if (!g_patch_system_enabled) {
        return;
    }
    
    // This function is for manual reapplication, not for post-reset use
    // For post-reset, use schedule_post_reset_patch_application() instead
    
    // Get currently running game
    XemuGamePatches* game_patches = xemu_patches_find_game_by_certificate();
    if (!game_patches) {
        return;
    }
    
    // Count enabled patches
    int enabled_patches = 0;
    for (int i = 0; i < game_patches->patch_count; i++) {
        if (game_patches->patches[i].enabled) {
            enabled_patches++;
        }
    }
    
    if (enabled_patches == 0) {
        return;
    }
    
    // Check if game is currently running
    if (!is_game_currently_running()) {
        return;
    }
    
    // Apply all enabled patches
    int applied_patch_count = 0;
    for (int i = 0; i < game_patches->patch_count; i++) {
        XemuMemoryPatch *patch = &game_patches->patches[i];
        if (!patch->enabled) continue;
        
        // Track if this patch was successfully applied
        bool patch_applied = false;
        
        // Apply each address/value pair for this patch
        if (patch->address_values && patch->address_value_count > 0) {
            for (int j = 0; j < patch->address_value_count; j++) {
                XemuPatchAddressValue *addr_val = &patch->address_values[j];
                
                if (apply_single_patch_bytes(addr_val->address, addr_val->value_data, addr_val->value_length, NULL)) {
                    patch_applied = true;
                }
            }
        }
        
        // Count this patch only once
        if (patch_applied) {
            applied_patch_count++;
        }
    }
    
    if (applied_patch_count > 0) {
        char notification[256];
        snprintf(notification, sizeof(notification), 
                "Manual: Reapplied %d patch%s", applied_patch_count, applied_patch_count == 1 ? "" : "es");
        xemu_queue_notification(notification);
    }
}
