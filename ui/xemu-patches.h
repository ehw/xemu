/*
 * xemu Memory Patches System
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

#ifndef XEMU_PATCHES_H
#define XEMU_PATCHES_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Individual address:value pair structure (supports variable-length byte arrays)
typedef struct {
    uint32_t address;     // Memory address to patch
    uint8_t *value_data;  // Variable-length byte array to write
    int value_length;     // Number of bytes in value_data
} XemuPatchAddressValue;

// Memory patch structure
typedef struct {
    XemuPatchAddressValue *address_values;  // Array of address:value pairs
    int address_value_count;                // Number of address:value pairs
    bool enabled;                           // Whether this patch is enabled
    char *name;                             // Name/description of the patch
    char *category;                         // Category of the patch (Cheat, Enhancement, Widescreen, etc.)
    char *author;                           // Author of the patch (optional)
    char *notes;                            // Optional notes about the patch
    char **address_value_lines;             // Original address:value lines (with comments)
    int address_value_lines_count;          // Number of address:value lines
    bool save_replaced_values;              // Whether to save and restore original memory values
    void *saved_original_values;             // Saved original memory values
    int *saved_value_lengths;                // Lengths of saved original values
} XemuMemoryPatch;

// Game entry structure  
typedef struct {
    char *game_title;     // Game title
    char *region;         // Region (NTSC/PAL/JAP)
    char *title_id;       // Xbox Title ID (e.g. "4D530001")
    char *version;        // Version string
    char *alternate_title_id;  // Alternate title ID from XBE certificate
    char *time_date;      // Time date from XBE certificate (yyyy-mm-dd hh:mm:ss format)
    char *disc_number;    // Disc number from XBE certificate

    XemuMemoryPatch *patches;  // Array of patches
    int patch_count;      // Number of patches
    bool enabled;         // Whether patches for this game are enabled
} XemuGamePatches;

// Main patches database
typedef struct {
    XemuGamePatches *games;   // Array of game entries
    int game_count;           // Number of games
    char *file_path;          // Path to patches database file
    bool dirty;               // Whether database needs to be saved
} XemuPatchesDatabase;

// Forward declarations
// (XBE dependencies removed - using disc path instead)

// Initialize patches system
void xemu_patches_init(void);

// Load patches database from file
bool xemu_patches_load_database(const char *filepath);

// Save patches database to file
bool xemu_patches_save_database(void);

// Get database file path (defaults to xemu.db)
const char *xemu_patches_get_database_path(void);

// Get all game entries
XemuGamePatches* xemu_patches_get_games(void);
int xemu_patches_get_game_count(void);

// Check if game already exists in database (by title-id)
int xemu_patches_find_game_by_title_id(const char *title_id);

// 🆕 Check if exact duplicate exists (by title-id AND version)
// Only prevents adding if BOTH title-id and version match existing game
int xemu_patches_find_duplicate_game(const char *title_id, const char *version);

// Parse XBE certificate for auto-population
bool xemu_patches_parse_xbe_certificate(const char *xbe_path, 
                                       char *title_out, int title_size,
                                       char *title_id_out, int title_id_size,
                                       char *region_out, int region_size,
                                       char *version_out, int version_size,
                                       char *alternate_title_id_out, int alternate_title_id_size,
                                       char *time_date_out, int time_date_size,
                                       char *disc_number_out, int disc_number_size);

// Parse multiple address:value pairs from a text buffer
bool xemu_patches_parse_address_value_pairs(const char *address_value_text,
                                           XemuPatchAddressValue **address_values,
                                           int *count);

// Get current loaded XBE path (for auto-population)
const char* xemu_patches_get_current_xbe_path(void);

// Set current XBE path (called when game is loaded)
void xemu_patches_set_current_xbe_path(const char *xbe_path);

// Find patches for specific game by disc filename
XemuGamePatches* xemu_patches_find_game_by_filename(const char *disc_path);

// Find patches for current game by XBE certificate (more reliable)
XemuGamePatches* xemu_patches_find_game_by_certificate(void);

// Force invalidate certificate cache (used during game switches)
void invalidate_certificate_cache(void);

// INTERNAL: Detect manual resets through VM state monitoring
void detect_manual_reset(void);

// PUBLIC HOOK: Signal that a manual reset has occurred (called from ui/gtk.c)
// void signal_manual_reset(void);  // INLINED: Replaced with direct variable access

// Global variables for database status (for UI access)
extern bool g_patches_loaded;
extern bool g_patches_initialized;

// Apply patches for disc (called when disc is loaded)
void xemu_patches_apply_for_disc(const char *disc_path);

// Force-apply patches for currently running XBE (can be called manually)
void xemu_patches_apply_current_running_xbe(void);

// Apply patches for current game (deprecated - use xemu_patches_apply_for_disc)
void xemu_patches_apply_current_game_patches(void);

// Apply patches at Xemu startup (called from main application)
void xemu_patches_on_startup(void);

// Reapply all currently enabled patches for the current game
void xemu_patches_reapply_current_game_patches(void);

// Game patches management
bool xemu_patches_add_game(const char *title, const char *region, 
                          const char *title_id, const char *version,
                          const char *alternate_title_id, const char *time_date,
                          const char *disc_number);
bool xemu_patches_remove_game(int game_index);
bool xemu_patches_update_game(int game_index, const char *title, 
                             const char *region, const char *title_id,
                             const char *version, const char *alternate_title_id,
                             const char *time_date, const char *disc_number);

// Patch management within games
bool xemu_patches_add_patch(int game_index, const char *name, const char *category,
                           const char *author, const char *notes, const char *address_value_pairs, bool save_replaced_values);
bool xemu_patches_remove_patch(int game_index, int patch_index);
bool xemu_patches_update_patch(int game_index, int patch_index, 
                              const char *name, const char *category, const char *author,
                              const char *notes, const char *address_value_pairs, bool save_replaced_values);
bool xemu_patches_set_patch_enabled(int game_index, int patch_index, 
                                    bool enabled);


// Utility functions
void xemu_patches_free_database(void);

// GUI callback functions (called from UI thread)
void xemu_patches_on_ui_request_save(void);
void xemu_patches_on_ui_database_changed(void);

// GUI rendering function (called from main render loop)
void xemu_patches_gui_render(void);

// Comprehensive Xbox memory verification function
void xemu_patches_verify_xbox_memory_mapping(void);

// PATCH MONITORING SYSTEM FUNCTIONS
// These functions monitor applied patches to detect when games overwrite them
void xemu_patches_periodic_monitoring_tick(void);              // Call this periodically from main loop
void xemu_patches_main_loop_update(void);                      // Main loop update for auto-apply
void xemu_patches_manual_monitor_check(void);                  // Manual trigger for monitoring
// void xemu_patches_set_monitoring_enabled(bool enabled);       // INLINED: Replaced with direct variable access
bool xemu_patches_is_monitoring_enabled(void);                // Check if monitoring is enabled
int xemu_patches_get_monitored_count(void);                   // Get number of monitored patches
void xemu_patches_reset_monitoring(void);                     // Reset all monitoring data

// POST-RESET PATCH APPLICATION SYSTEM
// Applies patches after reset completion using real state indicators
void schedule_post_reset_patch_application(void);             // Schedule patch application after reset
void xemu_patches_process_post_reset(void);                   // Process post-reset application (call from main loop)
void xemu_patches_process_post_reset_unsafe(void);             // Unsafe version of post-reset processing (for internal use)

// Reset all tracking variables to force fresh game detection
void reset_last_applied_tracking(void);                       // Reset tracking variables for game switching

// Load Disc operation tracking to prevent early patch application
// void set_load_disc_in_progress(void);                         // INLINED: Replaced with direct variable access

// Disc presence management for crash prevention
void set_load_disc_completed(void);                           // Called when Load Disc completes (disc loaded)
// void set_disc_ejected(void);                                  // INLINED: Replaced with direct variable access
void initialize_disc_presence_tracking(void);                 // Initialize on xemu startup
// bool is_disc_present(void);                                   // INLINED: Replaced with direct variable access
bool is_disc_present_enhanced(void);                          // Enhanced disc presence check with debugging
void auto_enable_patches_when_ready(void);                    // Auto-enable patches when certificate data available
void apply_patches_for_auto_boot(void);                       // Apply patches for auto-boot scenario (internal use)
// bool is_patch_system_enabled(void);                           // INLINED: Replaced with direct variable access

// Reset monitoring functions (used by snapshot system)
void start_reset_memory_monitoring(void);                     // Start monitoring memory during reset
void stop_reset_memory_monitoring(void);                      // Stop reset memory monitoring

// Certificate tracking functions for preventing duplicate patch messages
void enable_certificate_tracking(void);                       // Enable certificate tracking
void reset_certificate_tracking(void);                        // Reset certificate tracking
// bool has_certificate_changed(const char *title_id, const char *region, const char *version);  // INLINED: Replaced with direct logic
// void update_last_seen_certificate(const char *title_id, const char *region, const char *version);  // INLINED: Replaced with direct logic
// void clear_manual_reset_flag(void);                           // INLINED: Replaced with direct variable access

// Global variables for reset/reload patch scheduling
extern bool g_post_reset_patch_scheduled;                     // Flag to schedule post-reset patch application
extern bool g_post_reset_system_active;                       // Coordination flag to disable auto-apply when post-reset is active
extern bool g_load_disc_in_progress;                          // Load Disc operation tracking flag

// Certificate tracking for preventing duplicate patch messages during Load Disc operations
extern char g_last_seen_title_id[17];                         // Last seen TitleID (16 chars + null terminator)
extern char g_last_seen_region[8];                            // Last seen Region (7 chars + null terminator) 
extern char g_last_seen_version[32];                          // Last seen Version (31 chars + null terminator)
extern bool g_certificate_tracking_enabled;                   // Certificate tracking enabled flag
extern bool g_suppress_patch_notification;                    // Flag to suppress patch notification for duplicate certificates
extern bool g_manual_reset_detected;                          // Flag to track manual reset operations (bypass certificate tracking)
extern int g_post_reset_retry_count;                          // Retry counter for patch application
extern bool g_patches_applied_for_current_cert;               // Track if patches already applied for current certificate

// Load Disc retry mechanism for proper certificate refresh and patch application
extern bool g_load_disc_retry_pending;                        // Flag to retry certificate reading after Load Disc completes

// Global variables for disc presence and reset management (for UI access)
extern bool g_disc_present;                                  // Is a disc currently loaded?
extern bool g_patch_system_enabled;                          // Should we apply patches? (only when disc present)
extern bool g_vm_reset_triggered;                            // Set to true only when actual reset events occur
extern bool g_force_fresh_xbe_read;                          // Flag to force fresh XBE certificate reads

// XBE PRE-LOADING PATCH SYSTEM
// This patches the XBE file BEFORE the Xbox loads it, bypassing virtual memory overwriting
bool xemu_patches_apply_xbe_preloading(const char *disc_path); // Apply pre-loading patches

// SAVE REPLACED VALUES SYSTEM
// This system saves original memory values when patches are applied and restores them when patches are disabled

// Apply patch with save/replace logic
bool xemu_patches_apply_patch_with_save_restore(XemuMemoryPatch *patch, int game_index, int patch_index);

// Remove patch with restore logic
bool xemu_patches_remove_patch_with_restore(int game_index, int patch_index);

// Clear all saved values for a specific game (called on reset/snapshot load)
void xemu_patches_clear_saved_values_for_game(int game_index);

// Clear all saved values for all games (called on xemu exit)
void xemu_patches_clear_all_saved_values(void);

// External database instance (for popup menu access)
extern XemuPatchesDatabase g_patches_db;

#ifdef __cplusplus
}
#endif

#endif // XEMU_PATCHES_H