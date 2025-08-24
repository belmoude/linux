// SPDX-License-Identifier: GPL-2.0
/*
 * Confidential Computing Page State Migration
 * 
 * Implementation of atomic page state switching between private (encrypted)
 * and shared (decrypted) states using migration entry mechanism.
 * 
 * Author: AI Assistant
 */

#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/rmap.h>
#include <linux/migrate.h>
#include <linux/mem_encrypt.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/hash.h>
#include <linux/sched.h>
#include <asm/tlbflush.h>
#include "cc_page_migration.h"

/* Global data structures */
static DEFINE_SPINLOCK(cc_migration_lock);
static struct kmem_cache *cc_migration_cache;
static DECLARE_WAIT_QUEUE_HEAD(cc_transition_waitq);

/* Hash table for tracking CC migration entries */
#define CC_MIGRATION_HASH_BITS 8
#define CC_MIGRATION_HASH_SIZE (1 << CC_MIGRATION_HASH_BITS)
static struct hlist_head cc_migration_hash[CC_MIGRATION_HASH_SIZE];

/* Statistics */
static atomic_long_t cc_transition_count = ATOMIC_LONG_INIT(0);
static atomic_long_t cc_migration_entries_count = ATOMIC_LONG_INIT(0);

/* 
 * Hash function for migration entries 
 */
static inline unsigned int cc_migration_hash_func(struct page *page)
{
	return hash_ptr(page, CC_MIGRATION_HASH_BITS);
}

/*
 * Initialize CC migration subsystem
 */
static int __init cc_migration_init(void)
{
	int i;
	
	cc_migration_cache = kmem_cache_create("cc_migration_entry",
					      sizeof(struct cc_migration_entry),
					      0, SLAB_PANIC, NULL);
	if (!cc_migration_cache)
		return -ENOMEM;
	
	for (i = 0; i < CC_MIGRATION_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&cc_migration_hash[i]);
	
	pr_info("CC page migration subsystem initialized\n");
	return 0;
}

/*
 * Create a CC migration entry
 */
swp_entry_t make_cc_migration_entry(struct page *page, enum cc_page_state target_state)
{
	struct cc_migration_entry *entry;
	unsigned int hash;
	swp_entry_t swp_entry;
	
	entry = kmem_cache_alloc(cc_migration_cache, GFP_KERNEL);
	if (!entry)
		return swp_entry_to_pte(swp_entry_t){0};
	
	entry->page = page;
	entry->target_state = target_state;
	atomic_set(&entry->ref_count, 1);
	spin_lock_init(&entry->lock);
	
	hash = cc_migration_hash_func(page);
	
	spin_lock(&cc_migration_lock);
	hlist_add_head(&entry->page->lru, &cc_migration_hash[hash]);
	atomic_long_inc(&cc_migration_entries_count);
	spin_unlock(&cc_migration_lock);
	
	/* Create swap entry with special type for CC migration */
	swp_entry = swp_entry(SWP_MIGRATION_READ + target_state, (unsigned long)entry);
	
	return swp_entry;
}

/*
 * Get page from CC migration entry
 */
struct page *cc_migration_entry_to_page(swp_entry_t entry)
{
	struct cc_migration_entry *cc_entry;
	
	if (!is_cc_migration_entry(entry))
		return NULL;
	
	cc_entry = (struct cc_migration_entry *)swp_offset(entry);
	if (!cc_entry)
		return NULL;
	
	return cc_entry->page;
}

/*
 * Check if this is a CC migration entry
 */
int is_cc_migration_entry(swp_entry_t entry)
{
	int type = swp_type(entry);
	return (type >= SWP_MIGRATION_READ && 
		type <= SWP_MIGRATION_READ + CC_MIGRATION_ENTRY_MASK);
}

/*
 * Free CC migration entry
 */
void free_cc_migration_entry(swp_entry_t entry)
{
	struct cc_migration_entry *cc_entry;
	
	if (!is_cc_migration_entry(entry))
		return;
	
	cc_entry = (struct cc_migration_entry *)swp_offset(entry);
	if (!cc_entry)
		return;
	
	spin_lock(&cc_migration_lock);
	if (atomic_dec_and_test(&cc_entry->ref_count)) {
		hlist_del(&cc_entry->page->lru);
		atomic_long_dec(&cc_migration_entries_count);
		kmem_cache_free(cc_migration_cache, cc_entry);
	}
	spin_unlock(&cc_migration_lock);
}

/*
 * Get current page state
 */
enum cc_page_state cc_get_page_state(struct page *page)
{
	struct cc_page_info *info;
	
	if (!page || !PagePrivate(page))
		return CC_PAGE_SHARED;  /* Default to shared if no info */
	
	info = (struct cc_page_info *)page_private(page);
	if (!info)
		return CC_PAGE_SHARED;
	
	return info->current_state;
}

/*
 * Set page state
 */
int cc_set_page_state(struct page *page, enum cc_page_state state)
{
	struct cc_page_info *info;
	
	if (!page)
		return -EINVAL;
	
	if (!PagePrivate(page)) {
		info = kzalloc(sizeof(*info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;
		
		spin_lock_init(&info->state_lock);
		atomic_set(&info->transition_count, 0);
		set_page_private(page, (unsigned long)info);
		SetPagePrivate(page);
	} else {
		info = (struct cc_page_info *)page_private(page);
	}
	
	spin_lock(&info->state_lock);
	info->current_state = state;
	spin_unlock(&info->state_lock);
	
	return 0;
}

/*
 * Check if page is in transition state
 */
bool cc_page_is_transitioning(struct page *page)
{
	struct cc_page_info *info;
	
	if (!page || !PagePrivate(page))
		return false;
	
	info = (struct cc_page_info *)page_private(page);
	if (!info)
		return false;
	
	return info->current_state == CC_PAGE_TRANSITIONING;
}

/*
 * Hardware-specific page encryption
 */
int cc_encrypt_page(struct page *page)
{
	void *page_addr;
	int ret;
	
	if (!page)
		return -EINVAL;
	
	page_addr = page_address(page);
	if (!page_addr)
		return -EFAULT;
	
	/* Call hardware-specific encryption function */
	ret = set_memory_encrypted((unsigned long)page_addr, 1);
	if (ret)
		return ret;
	
	/* Update C-bit in page table if needed */
	return cc_update_page_cbit(page, true);
}

/*
 * Hardware-specific page decryption
 */
int cc_decrypt_page(struct page *page)
{
	void *page_addr;
	int ret;
	
	if (!page)
		return -EINVAL;
	
	page_addr = page_address(page);
	if (!page_addr)
		return -EFAULT;
	
	/* Call hardware-specific decryption function */
	ret = set_memory_decrypted((unsigned long)page_addr, 1);
	if (ret)
		return ret;
	
	/* Update C-bit in page table */
	return cc_update_page_cbit(page, false);
}

/*
 * Update C-bit in page table entries
 */
int cc_update_page_cbit(struct page *page, bool encrypted)
{
	struct rmap_walk_control rwc = {
		.rmap_one = NULL,  /* Will be set based on operation */
		.arg = &encrypted,
		.try_lock = false,
		.anon_lock = page_lock_anon_vma_read,
	};
	
	/* Implementation depends on architecture specifics */
	/* This would need to be implemented for specific platforms like AMD SEV or Intel TDX */
	
	return 0;
}

/*
 * Unmap all PTEs pointing to this page and replace with CC migration entries
 */
int cc_try_to_unmap(struct page *page, enum ttu_flags flags)
{
	struct rmap_walk_control rwc = {
		.rmap_one = NULL,  /* Custom function for CC migration */
		.arg = page,
		.try_lock = !!(flags & TTU_RMAP_LOCKED),
		.anon_lock = page_lock_anon_vma_read,
	};
	
	/* This would implement similar logic to try_to_unmap() but insert
	 * CC migration entries instead of regular migration entries */
	
	return SWAP_SUCCESS;
}

/*
 * Remove CC migration entries and restore normal PTEs
 */
void cc_remove_migration_ptes(struct page *old, struct page *new, bool locked)
{
	struct rmap_walk_control rwc = {
		.rmap_one = NULL,  /* Custom function for removing CC migration PTEs */
		.arg = new,
		.try_lock = !locked,
		.anon_lock = page_lock_anon_vma_read,
	};
	
	/* This would implement similar logic to remove_migration_ptes() but
	 * handle CC migration entries specifically */
}

/*
 * Wait for page transition to complete
 */
int cc_wait_on_page_transition(struct page *page)
{
	struct cc_page_info *info;
	
	if (!page || !PagePrivate(page))
		return 0;
	
	info = (struct cc_page_info *)page_private(page);
	if (!info)
		return 0;
	
	return wait_event_interruptible(cc_transition_waitq,
					info->current_state != CC_PAGE_TRANSITIONING);
}

/*
 * Wake up processes waiting for page transition
 */
void cc_wake_up_page_transition(struct page *page)
{
	wake_up_all(&cc_transition_waitq);
}

/*
 * Core function: Switch page state between private and shared
 */
int cc_switch_page_state(struct page *page, enum cc_page_state target_state, 
			 unsigned int flags)
{
	enum cc_page_state current_state;
	swp_entry_t migration_entry;
	int ret = 0;
	
	if (!page)
		return -EINVAL;
	
	/* Lock the page to prevent concurrent access */
	lock_page(page);
	
	current_state = cc_get_page_state(page);
	
	/* Check if transition is needed */
	if (current_state == target_state) {
		unlock_page(page);
		return 0;
	}
	
	/* Set transitioning state */
	ret = cc_set_page_state(page, CC_PAGE_TRANSITIONING);
	if (ret) {
		unlock_page(page);
		return ret;
	}
	
	/* Create migration entry */
	migration_entry = make_cc_migration_entry(page, target_state);
	if (!migration_entry.val) {
		cc_set_page_state(page, current_state);
		unlock_page(page);
		return -ENOMEM;
	}
	
	/* Unmap all PTEs and replace with migration entries */
	ret = cc_try_to_unmap(page, TTU_MIGRATION | TTU_IGNORE_MLOCK | TTU_IGNORE_ACCESS);
	if (ret != SWAP_SUCCESS) {
		free_cc_migration_entry(migration_entry);
		cc_set_page_state(page, current_state);
		unlock_page(page);
		return -EBUSY;
	}
	
	/* Perform the actual encryption/decryption */
	if (target_state == CC_PAGE_PRIVATE) {
		ret = cc_encrypt_page(page);
	} else if (target_state == CC_PAGE_SHARED) {
		ret = cc_decrypt_page(page);
	}
	
	if (ret) {
		/* Restore original PTEs on failure */
		cc_remove_migration_ptes(page, page, true);
		free_cc_migration_entry(migration_entry);
		cc_set_page_state(page, current_state);
		unlock_page(page);
		return ret;
	}
	
	/* Update page state */
	ret = cc_set_page_state(page, target_state);
	if (ret) {
		/* This shouldn't fail, but handle it anyway */
		cc_remove_migration_ptes(page, page, true);
		free_cc_migration_entry(migration_entry);
		unlock_page(page);
		return ret;
	}
	
	/* Remove migration entries and restore normal PTEs */
	cc_remove_migration_ptes(page, page, true);
	free_cc_migration_entry(migration_entry);
	
	/* Flush TLB to ensure consistency */
	flush_tlb_page(NULL, (unsigned long)page_address(page));
	
	/* Update statistics */
	atomic_long_inc(&cc_transition_count);
	
	/* Wake up any waiting processes */
	cc_wake_up_page_transition(page);
	
	unlock_page(page);
	
	return 0;
}

/*
 * Switch state for a range of pages
 */
int cc_switch_page_range(unsigned long start, unsigned long end,
			 enum cc_page_state target_state, unsigned int flags)
{
	unsigned long addr;
	struct page *page;
	int ret = 0;
	int failed = 0;
	
	for (addr = start; addr < end; addr += PAGE_SIZE) {
		page = virt_to_page(addr);
		if (!page)
			continue;
		
		ret = cc_switch_page_state(page, target_state, flags);
		if (ret)
			failed++;
	}
	
	return failed ? -EFAULT : 0;
}

/*
 * Debug function: dump page state
 */
void cc_dump_page_state(struct page *page)
{
	struct cc_page_info *info;
	enum cc_page_state state;
	
	if (!page) {
		pr_info("CC: Page is NULL\n");
		return;
	}
	
	state = cc_get_page_state(page);
	
	pr_info("CC: Page %p state: %s\n", page,
		state == CC_PAGE_PRIVATE ? "PRIVATE" :
		state == CC_PAGE_SHARED ? "SHARED" :
		state == CC_PAGE_TRANSITIONING ? "TRANSITIONING" : "UNKNOWN");
	
	if (PagePrivate(page)) {
		info = (struct cc_page_info *)page_private(page);
		if (info) {
			pr_info("CC: Transition count: %d\n",
				atomic_read(&info->transition_count));
		}
	}
}

/*
 * Get transition statistics
 */
unsigned long cc_get_transition_stats(void)
{
	return atomic_long_read(&cc_transition_count);
}

/* Module initialization */
module_init(cc_migration_init);