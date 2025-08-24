/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CC_PAGE_MIGRATION_H
#define _CC_PAGE_MIGRATION_H

#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/rmap.h>
#include <linux/migrate.h>
#include <linux/mem_encrypt.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

/*
 * Confidential Computing Page State Migration
 * 
 * This module implements atomic page state switching between private (encrypted)
 * and shared (decrypted) states in confidential computing environments.
 * It borrows the migration entry mechanism from page migration to ensure
 * atomicity and prevent race conditions.
 */

/* Page encryption states */
enum cc_page_state {
	CC_PAGE_PRIVATE = 0,	/* Page is encrypted (private) */
	CC_PAGE_SHARED = 1,	/* Page is decrypted (shared) */
	CC_PAGE_TRANSITIONING = 2, /* Page is in transition state */
};

/* CC migration entry types */
#define CC_MIGRATION_ENTRY_PRIVATE	0x1
#define CC_MIGRATION_ENTRY_SHARED	0x2
#define CC_MIGRATION_ENTRY_MASK	0x3

/* Flags for cc_migrate_page() */
#define CC_MIGRATE_SYNC		0x1	/* Synchronous migration */
#define CC_MIGRATE_ASYNC	0x2	/* Asynchronous migration */
#define CC_MIGRATE_FORCE	0x4	/* Force migration even if page is busy */

/*
 * CC migration entry structure
 * Similar to migration entries but for confidential computing state switching
 */
struct cc_migration_entry {
	struct page *page;
	enum cc_page_state target_state;
	atomic_t ref_count;
	spinlock_t lock;
};

/*
 * Per-page CC state tracking
 */
struct cc_page_info {
	enum cc_page_state current_state;
	enum cc_page_state target_state;
	struct cc_migration_entry *migration_entry;
	atomic_t transition_count;
	spinlock_t state_lock;
};

/* Function declarations */

/* Core state switching functions */
int cc_switch_page_state(struct page *page, enum cc_page_state target_state, 
			 unsigned int flags);
int cc_switch_page_range(unsigned long start, unsigned long end,
			 enum cc_page_state target_state, unsigned int flags);

/* Migration entry management */
swp_entry_t make_cc_migration_entry(struct page *page, enum cc_page_state target_state);
struct page *cc_migration_entry_to_page(swp_entry_t entry);
int is_cc_migration_entry(swp_entry_t entry);
void free_cc_migration_entry(swp_entry_t entry);

/* PTE manipulation functions */
int cc_try_to_unmap(struct page *page, enum ttu_flags flags);
void cc_remove_migration_ptes(struct page *old, struct page *new, bool locked);
int cc_replace_page_entries(struct page *page, enum cc_page_state target_state);

/* State management functions */
enum cc_page_state cc_get_page_state(struct page *page);
int cc_set_page_state(struct page *page, enum cc_page_state state);
bool cc_page_is_transitioning(struct page *page);

/* Hardware-specific functions */
int cc_encrypt_page(struct page *page);
int cc_decrypt_page(struct page *page);
int cc_update_page_cbit(struct page *page, bool encrypted);

/* Synchronization and waiting */
int cc_wait_on_page_transition(struct page *page);
void cc_wake_up_page_transition(struct page *page);

/* Debug and statistics */
void cc_dump_page_state(struct page *page);
unsigned long cc_get_transition_stats(void);

/* Helper macros */
#define CC_PAGE_STATE(page) cc_get_page_state(page)
#define CC_PAGE_IS_PRIVATE(page) (CC_PAGE_STATE(page) == CC_PAGE_PRIVATE)
#define CC_PAGE_IS_SHARED(page) (CC_PAGE_STATE(page) == CC_PAGE_SHARED)
#define CC_PAGE_IS_TRANSITIONING(page) cc_page_is_transitioning(page)

/* Error codes */
#define CC_SUCCESS		0
#define CC_ERROR_BUSY		-EBUSY
#define CC_ERROR_INVALID	-EINVAL
#define CC_ERROR_NOMEM		-ENOMEM
#define CC_ERROR_HARDWARE	-EIO

#endif /* _CC_PAGE_MIGRATION_H */