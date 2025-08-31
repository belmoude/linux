/*
 * Pin校验机制的使用示例
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "strict_pin_validator.h"

/**
 * 示例1：在内存回收函数中使用
 */
static int example_folio_reclaim(struct folio *folio)
{
    /* 严格校验：回收前确保页面未被pin */
    ASSERT_FOLIO_NOT_PINNED_FOR_RECLAIM(folio);
    
    /* 继续回收操作... */
    pr_info("Reclaiming folio pfn=%lu\n", folio_pfn(folio));
    
    return 0;
}

/**
 * 示例2：在页面迁移函数中使用
 */
static int example_folio_migrate(struct folio *old_folio, struct folio *new_folio)
{
    /* 严格校验：迁移前确保源页面未被pin */
    ASSERT_FOLIO_NOT_PINNED_FOR_MIGRATION(old_folio);
    
    /* 继续迁移操作... */
    pr_info("Migrating folio from pfn=%lu to pfn=%lu\n", 
            folio_pfn(old_folio), folio_pfn(new_folio));
    
    return 0;
}

/**
 * 示例3：在THP分割函数中使用
 */
static int example_split_huge_page(struct folio *folio)
{
    /* 严格校验：分割前确保THP未被pin */
    if (!folio_test_large(folio)) {
        pr_err("Not a large folio\n");
        return -EINVAL;
    }
    
    STRICT_ASSERT_NOT_PINNED(folio, "THP split");
    
    /* 继续分割操作... */
    pr_info("Splitting THP folio pfn=%lu, order=%u\n", 
            folio_pfn(folio), folio_order(folio));
    
    return 0;
}

/**
 * 示例4：批量页面操作前的校验
 */
static int example_batch_operation(struct page **pages, unsigned long npages)
{
    int ret;
    
    /* 批量校验所有页面 */
    ret = pages_assert_not_pinned(pages, npages, 
                                 "Batch operation on pinned pages");
    if (ret) {
        pr_err("Batch operation aborted due to pinned pages\n");
        return ret;
    }
    
    /* 继续批量操作... */
    pr_info("Batch operation on %lu pages proceeding\n", npages);
    
    return 0;
}

/**
 * 示例5：VMA操作前的校验
 */
static int example_vma_operation(struct vm_area_struct *vma, 
                                unsigned long start, unsigned long end)
{
    int ret;
    
    /* 校验VMA范围内的页面 */
    ret = vma_assert_not_pinned(vma, start, end, 
                               "VMA operation on pinned pages");
    if (ret) {
        pr_err("VMA operation aborted: pinned pages detected in range 0x%lx-0x%lx\n",
               start, end);
        return ret;
    }
    
    /* 继续VMA操作... */
    pr_info("VMA operation proceeding on range 0x%lx-0x%lx\n", start, end);
    
    return 0;
}

/**
 * 示例6：调试模式下的详细检查
 */
static void example_debug_check(struct folio *folio)
{
#ifdef CONFIG_DEBUG_VM
    struct pin_status_info info;
    enum strict_pin_result result;
    
    pr_info("=== DEBUG PIN CHECK ===\n");
    
    result = __folio_strict_pin_check(folio, &info);
    
    pr_info("Folio pfn=%lu:\n", folio_pfn(folio));
    pr_info("  Result: %s\n", 
            result == STRICT_PIN_NOT_PINNED ? "NOT_PINNED" :
            result == STRICT_PIN_DEFINITELY_PINNED ? "DEFINITELY_PINNED" :
            result == STRICT_PIN_LIKELY_PINNED ? "LIKELY_PINNED" : "UNCERTAIN");
    pr_info("  Refcount: %d, Mapcount: %d\n", info.ref_count, info.map_count);
    pr_info("  Expected base: %d\n", info.expected_base_refs);
    
    if (info.has_precise_tracking) {
        pr_info("  Precise pin count: %d\n", info.pin_count);
    } else {
        int excess = info.ref_count - info.expected_base_refs;
        pr_info("  Excess refs: %d\n", excess);
        if (excess >= GUP_PIN_COUNTING_BIAS) {
            pr_info("  Estimated pin count: %d\n", excess / GUP_PIN_COUNTING_BIAS);
        }
    }
    pr_info("  Reason: %s\n", info.reason);
    pr_info("=======================\n");
    
    /* 如果需要，还可以打印详细的folio状态 */
    folio_debug_pin_status(folio);
#endif
}

/**
 * 示例7：在关键内核路径中集成
 */
static int example_kernel_integration(void)
{
    struct folio *folio;
    struct page *page;
    int ret;
    
    /* 获取一个测试页面 */
    page = alloc_page(GFP_KERNEL);
    if (!page)
        return -ENOMEM;
    
    folio = page_folio(page);
    
    pr_info("Testing strict pin validation on newly allocated page\n");
    
    /* 测试1：新分配的页面应该未被pin */
    ret = folio_assert_not_pinned(folio, "Testing new page");
    if (ret) {
        pr_err("UNEXPECTED: New page appears to be pinned!\n");
        goto cleanup;
    }
    pr_info("✓ New page correctly identified as not pinned\n");
    
    /* 测试2：模拟pin操作后的检测 */
    /* 注意：这里只是演示，实际中应该使用pin_user_pages */
    if (folio_has_pincount(folio)) {
        atomic_inc(&folio->_pincount);
        pr_info("Simulated pin on large folio\n");
        
        ret = folio_assert_not_pinned(folio, "Testing pinned large folio");
        if (ret == 0) {
            pr_err("ERROR: Failed to detect pinned large folio!\n");
        } else {
            pr_info("✓ Pinned large folio correctly detected\n");
        }
        
        atomic_dec(&folio->_pincount);  /* 清理 */
    } else {
        /* 小页面：模拟pin by增加bias */
        folio_ref_add(folio, GUP_PIN_COUNTING_BIAS);
        pr_info("Simulated pin on small folio\n");
        
        ret = folio_assert_not_pinned(folio, "Testing pinned small folio");
        if (ret == 0) {
            pr_err("ERROR: Failed to detect likely pinned small folio!\n");
        } else {
            pr_info("✓ Likely pinned small folio detected\n");
        }
        
        folio_ref_sub(folio, GUP_PIN_COUNTING_BIAS);  /* 清理 */
    }
    
    /* 测试3：调试信息 */
    example_debug_check(folio);
    
cleanup:
    __free_page(page);
    return ret;
}

/**
 * 示例8：在现有内核函数中集成的模板
 */

/*
 * 在mm/vmscan.c的shrink_folio_list中集成：
 */
#if 0
static int shrink_folio_list_with_pin_check(struct list_head *folio_list, ...)
{
    struct folio *folio, *next;
    
    list_for_each_entry_safe(folio, next, folio_list, lru) {
        /* 添加pin校验 */
        if (folio_assert_not_pinned(folio, "folio reclaim") != 0) {
            /* pin的页面不能回收，移到不可回收列表 */
            list_move(&folio->lru, &ret_folios);
            continue;
        }
        
        /* 继续原有的回收逻辑... */
    }
}
#endif

/*
 * 在mm/migrate.c的migrate_pages中集成：
 */
#if 0
static int migrate_pages_with_pin_check(struct list_head *from, ...)
{
    struct folio *folio, *next;
    
    list_for_each_entry_safe(folio, next, from, lru) {
        /* 添加pin校验 */
        if (folio_assert_not_pinned(folio, "page migration") != 0) {
            /* pin的页面不能迁移 */
            nr_failed++;
            continue;
        }
        
        /* 继续原有的迁移逻辑... */
    }
}
#endif

/*
 * 在mm/huge_memory.c的split_huge_page中集成：
 */
#if 0
int split_huge_page_with_pin_check(struct page *page)
{
    struct folio *folio = page_folio(page);
    
    /* 添加pin校验 */
    ASSERT_FOLIO_NOT_PINNED_FOR_SPLIT(folio);
    
    /* 继续原有的分割逻辑... */
    return __split_huge_page(folio, ...);
}
#endif

#endif /* _STRICT_PIN_VALIDATOR_H */