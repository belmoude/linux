// SPDX-License-Identifier: GPL-2.0
/*
 * Test program for Confidential Computing Page State Migration
 * 
 * This program tests the functionality of atomic page state switching
 * between private (encrypted) and shared (decrypted) states.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* System call numbers (should match kernel definitions) */
#define __NR_cc_set_page_state		448
#define __NR_cc_get_page_state		449
#define __NR_cc_batch_set_state		450

/* Page states */
enum cc_page_state {
	CC_PAGE_PRIVATE = 0,	/* Page is encrypted (private) */
	CC_PAGE_SHARED = 1,	/* Page is decrypted (shared) */
	CC_PAGE_TRANSITIONING = 2, /* Page is in transition state */
};

/* Flags */
#define CC_MIGRATE_SYNC		0x1
#define CC_MIGRATE_ASYNC	0x2
#define CC_MIGRATE_FORCE	0x4

/* Structures matching kernel definitions */
struct cc_page_state_info {
	unsigned long addr;
	enum cc_page_state current_state;
	int is_transitioning;
	unsigned long transition_count;
};

struct cc_batch_request {
	unsigned long start_addr;
	unsigned long end_addr;
	enum cc_page_state target_state;
	unsigned int flags;
};

/* Test configuration */
#define TEST_PAGES		10
#define TEST_THREADS		4
#define TEST_ITERATIONS		100

/* Global test data */
static void *test_memory;
static size_t test_size;
static volatile int test_running = 1;
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Statistics */
static unsigned long transitions_attempted = 0;
static unsigned long transitions_successful = 0;
static unsigned long concurrent_accesses = 0;

/*
 * Wrapper functions for system calls
 */
static int cc_set_page_state(unsigned long addr, int state, unsigned int flags)
{
	return syscall(__NR_cc_set_page_state, addr, state, flags);
}

static int cc_get_page_state(unsigned long addr, struct cc_page_state_info *info)
{
	return syscall(__NR_cc_get_page_state, addr, info);
}

static int cc_batch_set_state(struct cc_batch_request *requests, unsigned int count)
{
	return syscall(__NR_cc_batch_set_state, requests, count);
}

/*
 * Test helper functions
 */
static void print_page_state(const char *prefix, struct cc_page_state_info *info)
{
	const char *state_str;
	
	switch (info->current_state) {
	case CC_PAGE_PRIVATE:
		state_str = "PRIVATE";
		break;
	case CC_PAGE_SHARED:
		state_str = "SHARED";
		break;
	case CC_PAGE_TRANSITIONING:
		state_str = "TRANSITIONING";
		break;
	default:
		state_str = "UNKNOWN";
	}
	
	printf("%s: addr=0x%lx, state=%s, transitioning=%d, count=%lu\n",
	       prefix, info->addr, state_str, info->is_transitioning, 
	       info->transition_count);
}

/*
 * Thread function for concurrent memory access
 */
static void *memory_access_thread(void *arg)
{
	int thread_id = *(int *)arg;
	char *mem = (char *)test_memory;
	unsigned long accesses = 0;
	
	printf("Thread %d: Starting memory access...\n", thread_id);
	
	while (test_running) {
		/* Access different pages randomly */
		int page_offset = rand() % TEST_PAGES;
		volatile char *page_addr = mem + (page_offset * 4096);
		
		/* Read from the page */
		volatile char data = *page_addr;
		
		/* Write to the page */
		*page_addr = (char)(accesses & 0xFF);
		
		accesses++;
		
		/* Small delay to allow state transitions */
		usleep(100);
	}
	
	pthread_mutex_lock(&test_mutex);
	concurrent_accesses += accesses;
	pthread_mutex_unlock(&test_mutex);
	
	printf("Thread %d: Completed %lu accesses\n", thread_id, accesses);
	return NULL;
}

/*
 * Thread function for state transitions
 */
static void *state_transition_thread(void *arg)
{
	int thread_id = *(int *)arg;
	char *mem = (char *)test_memory;
	struct cc_page_state_info info;
	unsigned long attempts = 0, successful = 0;
	
	printf("Thread %d: Starting state transitions...\n", thread_id);
	
	while (test_running) {
		/* Select a random page */
		int page_offset = rand() % TEST_PAGES;
		unsigned long page_addr = (unsigned long)(mem + (page_offset * 4096));
		
		/* Get current state */
		if (cc_get_page_state(page_addr, &info) == 0) {
			enum cc_page_state new_state;
			
			/* Toggle state */
			if (info.current_state == CC_PAGE_PRIVATE) {
				new_state = CC_PAGE_SHARED;
			} else {
				new_state = CC_PAGE_PRIVATE;
			}
			
			/* Attempt state transition */
			attempts++;
			if (cc_set_page_state(page_addr, new_state, CC_MIGRATE_SYNC) == 0) {
				successful++;
			}
		}
		
		/* Delay between transitions */
		usleep(1000);
	}
	
	pthread_mutex_lock(&test_mutex);
	transitions_attempted += attempts;
	transitions_successful += successful;
	pthread_mutex_unlock(&test_mutex);
	
	printf("Thread %d: %lu/%lu transitions successful\n", 
	       thread_id, successful, attempts);
	return NULL;
}

/*
 * Test basic functionality
 */
static int test_basic_functionality(void)
{
	struct cc_page_state_info info;
	unsigned long addr = (unsigned long)test_memory;
	int ret;
	
	printf("\n=== Basic Functionality Test ===\n");
	
	/* Get initial state */
	ret = cc_get_page_state(addr, &info);
	if (ret != 0) {
		printf("ERROR: Failed to get initial page state: %s\n", strerror(errno));
		return -1;
	}
	print_page_state("Initial state", &info);
	
	/* Test transition to private */
	printf("Transitioning to PRIVATE...\n");
	ret = cc_set_page_state(addr, CC_PAGE_PRIVATE, CC_MIGRATE_SYNC);
	if (ret != 0) {
		printf("ERROR: Failed to set page to PRIVATE: %s\n", strerror(errno));
		return -1;
	}
	
	ret = cc_get_page_state(addr, &info);
	if (ret == 0) {
		print_page_state("After PRIVATE transition", &info);
	}
	
	/* Test transition to shared */
	printf("Transitioning to SHARED...\n");
	ret = cc_set_page_state(addr, CC_PAGE_SHARED, CC_MIGRATE_SYNC);
	if (ret != 0) {
		printf("ERROR: Failed to set page to SHARED: %s\n", strerror(errno));
		return -1;
	}
	
	ret = cc_get_page_state(addr, &info);
	if (ret == 0) {
		print_page_state("After SHARED transition", &info);
	}
	
	printf("Basic functionality test PASSED\n");
	return 0;
}

/*
 * Test batch operations
 */
static int test_batch_operations(void)
{
	struct cc_batch_request requests[3];
	char *mem = (char *)test_memory;
	int ret;
	
	printf("\n=== Batch Operations Test ===\n");
	
	/* Setup batch requests */
	requests[0].start_addr = (unsigned long)(mem + 0);
	requests[0].end_addr = (unsigned long)(mem + 4096);
	requests[0].target_state = CC_PAGE_PRIVATE;
	requests[0].flags = CC_MIGRATE_SYNC;
	
	requests[1].start_addr = (unsigned long)(mem + 4096);
	requests[1].end_addr = (unsigned long)(mem + 8192);
	requests[1].target_state = CC_PAGE_SHARED;
	requests[1].flags = CC_MIGRATE_SYNC;
	
	requests[2].start_addr = (unsigned long)(mem + 8192);
	requests[2].end_addr = (unsigned long)(mem + 12288);
	requests[2].target_state = CC_PAGE_PRIVATE;
	requests[2].flags = CC_MIGRATE_SYNC;
	
	/* Execute batch operation */
	ret = cc_batch_set_state(requests, 3);
	if (ret < 0) {
		printf("ERROR: Batch operation failed: %s\n", strerror(errno));
		return -1;
	}
	
	printf("Batch operation successful: %d requests processed\n", ret);
	return 0;
}

/*
 * Test concurrent access
 */
static int test_concurrent_access(void)
{
	pthread_t access_threads[TEST_THREADS];
	pthread_t transition_threads[2];
	int thread_ids[TEST_THREADS + 2];
	int i;
	
	printf("\n=== Concurrent Access Test ===\n");
	
	/* Reset statistics */
	transitions_attempted = 0;
	transitions_successful = 0;
	concurrent_accesses = 0;
	test_running = 1;
	
	/* Create memory access threads */
	for (i = 0; i < TEST_THREADS; i++) {
		thread_ids[i] = i;
		if (pthread_create(&access_threads[i], NULL, 
				   memory_access_thread, &thread_ids[i]) != 0) {
			printf("ERROR: Failed to create access thread %d\n", i);
			return -1;
		}
	}
	
	/* Create state transition threads */
	for (i = 0; i < 2; i++) {
		thread_ids[TEST_THREADS + i] = TEST_THREADS + i;
		if (pthread_create(&transition_threads[i], NULL,
				   state_transition_thread, &thread_ids[TEST_THREADS + i]) != 0) {
			printf("ERROR: Failed to create transition thread %d\n", i);
			return -1;
		}
	}
	
	/* Run test for a few seconds */
	sleep(5);
	
	/* Stop all threads */
	test_running = 0;
	
	/* Wait for all threads to complete */
	for (i = 0; i < TEST_THREADS; i++) {
		pthread_join(access_threads[i], NULL);
	}
	for (i = 0; i < 2; i++) {
		pthread_join(transition_threads[i], NULL);
	}
	
	/* Print statistics */
	printf("Concurrent access test completed:\n");
	printf("  Memory accesses: %lu\n", concurrent_accesses);
	printf("  Transitions attempted: %lu\n", transitions_attempted);
	printf("  Transitions successful: %lu\n", transitions_successful);
	printf("  Success rate: %.2f%%\n", 
	       transitions_attempted > 0 ? 
	       (100.0 * transitions_successful / transitions_attempted) : 0.0);
	
	return 0;
}

/*
 * Main test function
 */
int main(int argc, char *argv[])
{
	int ret = 0;
	
	printf("Confidential Computing Page Migration Test\n");
	printf("==========================================\n");
	
	/* Allocate test memory */
	test_size = TEST_PAGES * 4096;
	test_memory = mmap(NULL, test_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (test_memory == MAP_FAILED) {
		printf("ERROR: Failed to allocate test memory: %s\n", strerror(errno));
		return 1;
	}
	
	printf("Allocated %zu bytes at %p\n", test_size, test_memory);
	
	/* Initialize memory with test pattern */
	memset(test_memory, 0xAA, test_size);
	
	/* Run tests */
	if (test_basic_functionality() != 0) {
		ret = 1;
		goto cleanup;
	}
	
	if (test_batch_operations() != 0) {
		ret = 1;
		goto cleanup;
	}
	
	if (test_concurrent_access() != 0) {
		ret = 1;
		goto cleanup;
	}
	
	printf("\n=== ALL TESTS PASSED ===\n");

cleanup:
	/* Free test memory */
	if (munmap(test_memory, test_size) != 0) {
		printf("WARNING: Failed to free test memory: %s\n", strerror(errno));
	}
	
	return ret;
}