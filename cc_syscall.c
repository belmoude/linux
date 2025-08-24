// SPDX-License-Identifier: GPL-2.0
/*
 * Confidential Computing System Call Interface
 * 
 * Provides system calls for user-space programs to control
 * page encryption state transitions in confidential computing environments.
 */

#include <linux/syscalls.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/security.h>
#include <linux/capability.h>
#include <linux/uaccess.h>
#include "cc_page_migration.h"

/* System call numbers (would be allocated in the kernel) */
#define __NR_cc_set_page_state		448
#define __NR_cc_get_page_state		449
#define __NR_cc_batch_set_state		450

/*
 * Structure for batch operations
 */
struct cc_batch_request {
	unsigned long start_addr;
	unsigned long end_addr;
	enum cc_page_state target_state;
	unsigned int flags;
};

/*
 * Structure for getting page state information
 */
struct cc_page_state_info {
	unsigned long addr;
	enum cc_page_state current_state;
	bool is_transitioning;
	unsigned long transition_count;
};

/*
 * Set encryption state for a single page
 * 
 * @addr: Virtual address of the page (must be page-aligned)
 * @state: Target state (CC_PAGE_PRIVATE or CC_PAGE_SHARED)
 * @flags: Operation flags
 * 
 * Returns: 0 on success, negative error code on failure
 */
SYSCALL_DEFINE3(cc_set_page_state, unsigned long, addr, 
		int, state, unsigned int, flags)
{
	struct page *page;
	struct vm_area_struct *vma;
	int ret;
	
	/* Validate parameters */
	if (!IS_ALIGNED(addr, PAGE_SIZE))
		return -EINVAL;
	
	if (state != CC_PAGE_PRIVATE && state != CC_PAGE_SHARED)
		return -EINVAL;
	
	/* Check if the calling process has permission */
	if (!capable(CAP_SYS_ADMIN)) {
		/* Non-privileged processes can only modify their own memory */
		if (!find_vma(current->mm, addr))
			return -EACCES;
	}
	
	/* Get the VMA and page */
	down_read(&current->mm->mmap_sem);
	vma = find_vma(current->mm, addr);
	if (!vma || addr < vma->vm_start) {
		up_read(&current->mm->mmap_sem);
		return -EFAULT;
	}
	
	/* Get the page structure */
	ret = get_user_pages_remote(current, current->mm, addr, 1,
				   FOLL_WRITE, &page, NULL, NULL);
	if (ret != 1) {
		up_read(&current->mm->mmap_sem);
		return ret < 0 ? ret : -EFAULT;
	}
	
	up_read(&current->mm->mmap_sem);
	
	/* Perform the state transition */
	ret = cc_switch_page_state(page, state, flags);
	
	put_page(page);
	return ret;
}

/*
 * Get encryption state for a page
 * 
 * @addr: Virtual address of the page
 * @info: Pointer to structure to receive state information
 * 
 * Returns: 0 on success, negative error code on failure
 */
SYSCALL_DEFINE2(cc_get_page_state, unsigned long, addr,
		struct cc_page_state_info __user *, info)
{
	struct page *page;
	struct vm_area_struct *vma;
	struct cc_page_state_info state_info;
	struct cc_page_info *page_info;
	int ret;
	
	if (!info)
		return -EINVAL;
	
	if (!IS_ALIGNED(addr, PAGE_SIZE))
		return -EINVAL;
	
	/* Get the VMA and page */
	down_read(&current->mm->mmap_sem);
	vma = find_vma(current->mm, addr);
	if (!vma || addr < vma->vm_start) {
		up_read(&current->mm->mmap_sem);
		return -EFAULT;
	}
	
	ret = get_user_pages_remote(current, current->mm, addr, 1,
				   0, &page, NULL, NULL);
	if (ret != 1) {
		up_read(&current->mm->mmap_sem);
		return ret < 0 ? ret : -EFAULT;
	}
	
	up_read(&current->mm->mmap_sem);
	
	/* Fill in the state information */
	state_info.addr = addr;
	state_info.current_state = cc_get_page_state(page);
	state_info.is_transitioning = cc_page_is_transitioning(page);
	
	if (PagePrivate(page)) {
		page_info = (struct cc_page_info *)page_private(page);
		if (page_info) {
			state_info.transition_count = 
				atomic_read(&page_info->transition_count);
		} else {
			state_info.transition_count = 0;
		}
	} else {
		state_info.transition_count = 0;
	}
	
	put_page(page);
	
	/* Copy to user space */
	if (copy_to_user(info, &state_info, sizeof(state_info)))
		return -EFAULT;
	
	return 0;
}

/*
 * Batch operation: set state for multiple pages
 * 
 * @requests: Array of batch request structures
 * @count: Number of requests in the array
 * 
 * Returns: Number of successfully processed requests, or negative error code
 */
SYSCALL_DEFINE2(cc_batch_set_state, 
		struct cc_batch_request __user *, requests,
		unsigned int, count)
{
	struct cc_batch_request *batch;
	int i, ret, successful = 0;
	
	if (!requests || count == 0)
		return -EINVAL;
	
	if (count > 1024)  /* Reasonable limit */
		return -E2BIG;
	
	/* Check permissions */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	
	/* Allocate kernel memory for batch requests */
	batch = kmalloc_array(count, sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return -ENOMEM;
	
	/* Copy from user space */
	if (copy_from_user(batch, requests, count * sizeof(*batch))) {
		kfree(batch);
		return -EFAULT;
	}
	
	/* Process each request */
	for (i = 0; i < count; i++) {
		ret = cc_switch_page_range(batch[i].start_addr,
					   batch[i].end_addr,
					   batch[i].target_state,
					   batch[i].flags);
		if (ret == 0)
			successful++;
	}
	
	kfree(batch);
	return successful;
}

/*
 * Initialize the system call interface
 */
static int __init cc_syscall_init(void)
{
	pr_info("CC syscall interface initialized\n");
	return 0;
}

module_init(cc_syscall_init);