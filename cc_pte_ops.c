// SPDX-License-Identifier: GPL-2.0
/*
 * Confidential Computing PTE Operations
 * 
 * Architecture-specific PTE manipulation functions for CC page migration.
 * This implements the core atomicity mechanism by replacing PTEs with
 * migration entries during state transitions.
 */

#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
#include <asm/tlbflush.h>
#include <asm/pgtable.h>
#include "cc_page_migration.h"

/*
 * Custom rmap callback for inserting CC migration entries
 */
static bool cc_try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
				unsigned long address, void *arg)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	pte_t pteval;
	spinlock_t *ptl;
	swp_entry_t swp_entry;
	enum cc_page_state target_state = *(enum cc_page_state *)arg;
	
	/* Get the PTE and lock it */
	pte = page_check_address(page, mm, address, &ptl, 0);
	if (!pte)
		return true;
	
	/* Check if this PTE actually points to our page */
	if (page_to_pfn(page) != pte_pfn(*pte))
		goto out_unlock;
	
	/* Save the original PTE value */
	pteval = ptep_get_and_clear(mm, address, pte);
	
	/* Create CC migration entry */
	swp_entry = make_cc_migration_entry(page, target_state);
	if (!swp_entry.val) {
		set_pte_at(mm, address, pte, pteval);
		goto out_unlock;
	}
	
	/* Insert the CC migration entry */
	if (pte_write(pteval))
		swp_entry = make_migration_entry_write(swp_entry);
	if (pte_young(pteval))
		swp_entry = make_migration_entry_young(swp_entry);
	
	set_pte_at(mm, address, pte, swp_entry_to_pte(swp_entry));
	
	/*
	 * Invalidate this mapping immediately to ensure any concurrent
	 * access will trigger a page fault and wait for the transition
	 * to complete.
	 */
	flush_tlb_page(vma, address);
	
	/* 
	 * Send MMU notifier events to inform any external consumers
	 * (like VFIO, KVM) about the mapping change
	 */
	mmu_notifier_invalidate_range_start(mm, address, address + PAGE_SIZE);
	mmu_notifier_invalidate_range_end(mm, address, address + PAGE_SIZE);
	
	pte_unmap_unlock(pte, ptl);
	return true;

out_unlock:
	pte_unmap_unlock(pte, ptl);
	return false;
}

/*
 * Custom rmap callback for removing CC migration entries
 */
static bool cc_remove_migration_pte_one(struct page *page, struct vm_area_struct *vma,
					unsigned long address, void *arg)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	pte_t new_pte;
	spinlock_t *ptl;
	swp_entry_t entry;
	struct page *new_page = (struct page *)arg;
	
	/* Get the PTE and lock it */
	pte = page_check_address(page, mm, address, &ptl, 1);
	if (!pte)
		return true;
	
	/* Check if this is a CC migration entry */
	if (!is_swap_pte(*pte)) {
		pte_unmap_unlock(pte, ptl);
		return true;
	}
	
	entry = pte_to_swp_entry(*pte);
	if (!is_cc_migration_entry(entry)) {
		pte_unmap_unlock(pte, ptl);
		return true;
	}
	
	/* Verify this migration entry points to our page */
	if (cc_migration_entry_to_page(entry) != page) {
		pte_unmap_unlock(pte, ptl);
		return true;
	}
	
	/* Create new PTE pointing to the (possibly same) page */
	new_pte = pte_mkold(mk_pte(new_page, vma->vm_page_prot));
	
	/* Restore write permission if it was present */
	if (is_migration_entry_write(entry))
		new_pte = maybe_mkwrite(new_pte, vma);
	
	/* Restore young bit if it was set */
	if (is_migration_entry_young(entry))
		new_pte = pte_mkyoung(new_pte);
	
	/* Update C-bit based on new page state */
	enum cc_page_state new_state = cc_get_page_state(new_page);
	if (new_state == CC_PAGE_PRIVATE) {
		/* Set C-bit for encrypted pages */
		new_pte = pte_set_flags(new_pte, _PAGE_ENC);
	} else {
		/* Clear C-bit for shared pages */
		new_pte = pte_clear_flags(new_pte, _PAGE_ENC);
	}
	
	/* Install the new PTE */
	set_pte_at(mm, address, pte, new_pte);
	
	/* Flush TLB for this mapping */
	flush_tlb_page(vma, address);
	
	/* Free the migration entry */
	free_cc_migration_entry(entry);
	
	pte_unmap_unlock(pte, ptl);
	return true;
}

/*
 * Unmap all PTEs pointing to this page and replace with CC migration entries
 */
int cc_try_to_unmap(struct page *page, enum ttu_flags flags)
{
	struct rmap_walk_control rwc = {
		.rmap_one = cc_try_to_unmap_one,
		.arg = &page,  /* We'll modify this to pass target_state */
		.try_lock = !!(flags & TTU_RMAP_LOCKED),
		.anon_lock = page_lock_anon_vma_read,
	};
	enum cc_page_state target_state;
	struct cc_page_info *info;
	
	if (!page || !PagePrivate(page))
		return SWAP_FAIL;
	
	info = (struct cc_page_info *)page_private(page);
	if (!info)
		return SWAP_FAIL;
	
	target_state = info->target_state;
	rwc.arg = &target_state;
	
	/*
	 * We need to be careful about the order of operations here:
	 * 1. First, we unmap all PTEs and insert migration entries
	 * 2. This ensures any concurrent access will fault and wait
	 * 3. Only then do we proceed with the actual state change
	 */
	
	return rmap_walk(page, &rwc) ? SWAP_SUCCESS : SWAP_AGAIN;
}

/*
 * Remove CC migration entries and restore normal PTEs
 */
void cc_remove_migration_ptes(struct page *old, struct page *new, bool locked)
{
	struct rmap_walk_control rwc = {
		.rmap_one = cc_remove_migration_pte_one,
		.arg = new,
		.try_lock = !locked,
		.anon_lock = page_lock_anon_vma_read,
	};
	
	/*
	 * Walk through all mappings and restore normal PTEs.
	 * This is the final step that allows normal access to resume.
	 */
	rmap_walk(old, &rwc);
}

/*
 * Handle page fault on CC migration entry
 * This is called when a process tries to access a page that's in transition
 */
vm_fault_t cc_migration_entry_wait(struct mm_struct *mm, pmd_t *pmd,
				   unsigned long address, pte_t *pte,
				   spinlock_t *ptl, swp_entry_t entry)
{
	struct page *page;
	vm_fault_t ret = 0;
	
	if (!is_cc_migration_entry(entry)) {
		pte_unmap_unlock(pte, ptl);
		return 0;
	}
	
	page = cc_migration_entry_to_page(entry);
	if (!page) {
		pte_unmap_unlock(pte, ptl);
		return VM_FAULT_SIGBUS;
	}
	
	pte_unmap_unlock(pte, ptl);
	
	/*
	 * Wait for the page transition to complete.
	 * This is where the "blocking" behavior happens - any process
	 * that tries to access the page during transition will wait here.
	 */
	ret = cc_wait_on_page_transition(page);
	if (ret)
		return VM_FAULT_RETRY;
	
	/*
	 * After waiting, we retry the page fault.
	 * By this time, the migration entry should have been replaced
	 * with a normal PTE pointing to the page in its new state.
	 */
	return VM_FAULT_RETRY;
}

/*
 * Replace all page entries for a single VMA
 */
static int cc_replace_page_entries_vma(struct vm_area_struct *vma,
				       struct page *page,
				       enum cc_page_state target_state)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long address;
	pte_t *pte;
	spinlock_t *ptl;
	int ret = 0;
	
	address = page_address_in_vma(page, vma);
	if (address == -EFAULT)
		return 0;
	
	pte = page_check_address(page, mm, address, &ptl, 0);
	if (!pte)
		return 0;
	
	/* This VMA maps our page, so we need to update the C-bit */
	if (target_state == CC_PAGE_PRIVATE) {
		/* Set C-bit for private (encrypted) pages */
		*pte = pte_set_flags(*pte, _PAGE_ENC);
	} else {
		/* Clear C-bit for shared (unencrypted) pages */
		*pte = pte_clear_flags(*pte, _PAGE_ENC);
	}
	
	/* Flush TLB for this mapping */
	flush_tlb_page(vma, address);
	
	pte_unmap_unlock(pte, ptl);
	return ret;
}

/*
 * Update C-bit in all page table entries pointing to this page
 * This is architecture-specific and crucial for confidential computing
 */
int cc_update_page_cbit(struct page *page, bool encrypted)
{
	struct rmap_walk_control rwc = {
		.rmap_one = NULL,  /* We'll use a custom approach */
		.arg = &encrypted,
		.try_lock = false,
		.anon_lock = page_lock_anon_vma_read,
	};
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	struct vm_area_struct *vma;
	enum cc_page_state target_state;
	
	target_state = encrypted ? CC_PAGE_PRIVATE : CC_PAGE_SHARED;
	
	/*
	 * For anonymous pages, walk through all VMAs that map this page
	 */
	if (PageAnon(page)) {
		anon_vma = page_lock_anon_vma_read(page);
		if (!anon_vma)
			return 0;
		
		anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, 0, ULONG_MAX) {
			vma = avc->vma;
			cc_replace_page_entries_vma(vma, page, target_state);
		}
		
		page_unlock_anon_vma_read(anon_vma);
	}
	
	/*
	 * For file pages, we would need to walk through the file's
	 * address space mappings. This is more complex and would
	 * require additional implementation.
	 */
	
	return 0;
}

/*
 * Batch operation: replace page entries for multiple pages
 */
int cc_replace_page_entries(struct page *page, enum cc_page_state target_state)
{
	/*
	 * This function could be optimized to handle multiple pages
	 * at once, reducing TLB flush overhead.
	 */
	return cc_update_page_cbit(page, target_state == CC_PAGE_PRIVATE);
}