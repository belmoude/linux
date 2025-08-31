/*
 * 严格的Pin User Page校验机制
 * 目的：准确检测页面是否被pin，如果被pin则报错
 */

#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/bug.h>
#include <linux/ratelimit.h>

/**
 * 严格pin校验的结果类型
 */
enum strict_pin_result {
    STRICT_PIN_NOT_PINNED = 0,      /* 确定未被pin */
    STRICT_PIN_DEFINITELY_PINNED,   /* 确定被pin */
    STRICT_PIN_LIKELY_PINNED,       /* 很可能被pin（小页面的模糊判断） */
    STRICT_PIN_UNCERTAIN,           /* 无法确定 */
};

/**
 * 详细的pin状态信息
 */
struct pin_status_info {
    enum strict_pin_result result;
    int pin_count;              /* 精确pin计数（如果可用） */
    int ref_count;              /* 当前引用计数 */
    int map_count;              /* 映射计数 */
    int expected_base_refs;     /* 预期的基础引用计数 */
    bool has_precise_tracking;  /* 是否有精确跟踪 */
    const char *reason;         /* 判断原因 */
};

/**
 * 计算页面的预期基础引用计数
 */
static int calculate_expected_base_refs(struct folio *folio)
{
    int expected = 0;
    
    /* 页面缓存引用 */
    if (folio->mapping)
        expected++;
    
    /* LRU引用 */
    if (folio_test_lru(folio))
        expected++;
    
    /* swap缓存引用 */
    if (folio_test_swapcache(folio))
        expected++;
    
    /* 页表映射引用 */
    expected += folio_mapcount(folio);
    
    return expected;
}

/**
 * 严格校验页面是否被pin
 * @folio: 要检查的folio
 * @info: 返回详细的状态信息（可选）
 * 
 * 返回: strict_pin_result枚举值
 */
static enum strict_pin_result __folio_strict_pin_check(struct folio *folio, 
                                                      struct pin_status_info *info)
{
    struct pin_status_info local_info = {0};
    if (!info)
        info = &local_info;
    
    info->ref_count = folio_ref_count(folio);
    info->map_count = folio_mapcount(folio);
    info->expected_base_refs = calculate_expected_base_refs(folio);
    
    /* 第一层：进程级检查 */
    if (!test_bit(MMF_HAS_PINNED, &current->mm->flags)) {
        info->result = STRICT_PIN_NOT_PINNED;
        info->reason = "Process never used pin_user_pages";
        return STRICT_PIN_NOT_PINNED;
    }
    
    /* 第二层：特殊页面处理 */
    if (is_zero_folio(folio)) {
        info->result = STRICT_PIN_NOT_PINNED;
        info->reason = "Zero page cannot be pinned";
        return STRICT_PIN_NOT_PINNED;
    }
    
    /* 第三层：精确检测（大页面） */
    if (folio_has_pincount(folio)) {
        info->has_precise_tracking = true;
        info->pin_count = atomic_read(&folio->_pincount);
        
        if (info->pin_count > 0) {
            info->result = STRICT_PIN_DEFINITELY_PINNED;
            info->reason = "Precise pincount > 0";
            return STRICT_PIN_DEFINITELY_PINNED;
        } else {
            info->result = STRICT_PIN_NOT_PINNED;
            info->reason = "Precise pincount = 0";
            return STRICT_PIN_NOT_PINNED;
        }
    }
    
    /* 第四层：小页面分析 */
    info->has_precise_tracking = false;
    
    if (info->ref_count < GUP_PIN_COUNTING_BIAS) {
        /* 引用计数小于bias，肯定没有pin */
        info->result = STRICT_PIN_NOT_PINNED;
        info->reason = "Refcount below PIN_BIAS";
        return STRICT_PIN_NOT_PINNED;
    }
    
    /* 分析是否可能是正常引用导致的高计数 */
    int excess_refs = info->ref_count - info->expected_base_refs;
    
    if (excess_refs >= GUP_PIN_COUNTING_BIAS) {
        /* 很可能被pin */
        info->pin_count = excess_refs / GUP_PIN_COUNTING_BIAS;
        info->result = STRICT_PIN_LIKELY_PINNED;
        info->reason = "Excess refcount suggests pinning";
        return STRICT_PIN_LIKELY_PINNED;
    } else if (info->ref_count >= GUP_PIN_COUNTING_BIAS) {
        /* 引用计数达到bias但excess不够，不确定 */
        info->result = STRICT_PIN_UNCERTAIN;
        info->reason = "Refcount at PIN_BIAS but excess refs unclear";
        return STRICT_PIN_UNCERTAIN;
    }
    
    info->result = STRICT_PIN_NOT_PINNED;
    info->reason = "Analysis suggests not pinned";
    return STRICT_PIN_NOT_PINNED;
}

/**
 * 严格校验页面未被pin，如果被pin则报错
 * @folio: 要检查的folio
 * @error_msg: 自定义错误消息
 * @file: 调用文件名（通常使用__FILE__）
 * @line: 调用行号（通常使用__LINE__）
 * @func: 调用函数名（通常使用__func__）
 * 
 * 返回: 0表示未被pin，负数表示被pin或检查失败
 */
int folio_assert_not_pinned_detailed(struct folio *folio, const char *error_msg,
                                   const char *file, int line, const char *func)
{
    struct pin_status_info info;
    enum strict_pin_result result;
    static DEFINE_RATELIMIT_STATE(rs, 5 * HZ, 10);
    
    if (!folio) {
        if (__ratelimit(&rs))
            pr_err("STRICT_PIN_CHECK: NULL folio at %s:%d in %s\n", 
                   file, line, func);
        return -EINVAL;
    }
    
    result = __folio_strict_pin_check(folio, &info);
    
    switch (result) {
    case STRICT_PIN_NOT_PINNED:
        /* 正常情况：确定未被pin */
        return 0;
        
    case STRICT_PIN_DEFINITELY_PINNED:
        /* 严重错误：确定被pin */
        if (__ratelimit(&rs)) {
            pr_err("STRICT_PIN_CHECK: %s\n", error_msg ? error_msg : "Folio is definitely pinned");
            pr_err("  Location: %s:%d in %s()\n", file, line, func);
            pr_err("  Folio: pfn=%lu, order=%u, refcount=%d, mapcount=%d\n",
                   folio_pfn(folio), folio_order(folio), info.ref_count, info.map_count);
            pr_err("  Pin count: %d (precise tracking: %s)\n", 
                   info.pin_count, info.has_precise_tracking ? "yes" : "no");
            pr_err("  Reason: %s\n", info.reason);
            
            /* 打印调用栈 */
            dump_stack();
        }
        
        /* 根据配置决定是否panic */
        if (IS_ENABLED(CONFIG_DEBUG_VM))
            BUG();
        
        return -EBUSY;
        
    case STRICT_PIN_LIKELY_PINNED:
        /* 警告：很可能被pin */
        if (__ratelimit(&rs)) {
            pr_warn("STRICT_PIN_CHECK: %s (likely pinned)\n", 
                    error_msg ? error_msg : "Folio is likely pinned");
            pr_warn("  Location: %s:%d in %s()\n", file, line, func);
            pr_warn("  Folio: pfn=%lu, refcount=%d, expected_base=%d\n",
                    folio_pfn(folio), info.ref_count, info.expected_base_refs);
            pr_warn("  Estimated pin count: %d\n", info.pin_count);
            pr_warn("  Reason: %s\n", info.reason);
        }
        return -EBUSY;
        
    case STRICT_PIN_UNCERTAIN:
        /* 不确定的情况 */
        if (__ratelimit(&rs)) {
            pr_info("STRICT_PIN_CHECK: Uncertain pin status at %s:%d in %s()\n",
                    file, line, func);
            pr_info("  Folio: pfn=%lu, refcount=%d\n", 
                    folio_pfn(folio), info.ref_count);
            pr_info("  Reason: %s\n", info.reason);
        }
        
        /* 根据严格程度决定处理方式 */
        if (IS_ENABLED(CONFIG_DEBUG_VM))
            return -EBUSY;  /* 调试模式下采用保守策略 */
        
        return 0;  /* 生产环境下允许不确定的情况 */
    }
    
    return -EINVAL;  /* 不应该到达这里 */
}

/**
 * 便利宏：简化调用
 */
#define folio_assert_not_pinned(folio, msg) \
    folio_assert_not_pinned_detailed(folio, msg, __FILE__, __LINE__, __func__)

/**
 * 页面级别的校验函数
 */
#define page_assert_not_pinned(page, msg) \
    folio_assert_not_pinned(page_folio(page), msg)

/**
 * 批量校验函数
 */
int pages_assert_not_pinned(struct page **pages, unsigned long npages, 
                           const char *error_msg)
{
    unsigned long i;
    int ret;
    
    for (i = 0; i < npages; i++) {
        if (!pages[i])
            continue;
            
        ret = folio_assert_not_pinned(page_folio(pages[i]), error_msg);
        if (ret) {
            pr_err("STRICT_PIN_CHECK: Pin detected in page array at index %lu\n", i);
            return ret;
        }
    }
    
    return 0;
}

/**
 * VMA范围内的校验函数
 */
int vma_assert_not_pinned(struct vm_area_struct *vma, unsigned long start, 
                         unsigned long end, const char *error_msg)
{
    unsigned long addr;
    struct page *page;
    int ret;
    
    if (!test_bit(MMF_HAS_PINNED, &vma->vm_mm->flags))
        return 0;  /* 快速路径：该进程从未pin过页面 */
    
    for (addr = start; addr < end; addr += PAGE_SIZE) {
        page = follow_page(vma, addr, 0);
        if (IS_ERR_OR_NULL(page))
            continue;
            
        ret = page_assert_not_pinned(page, error_msg);
        if (ret) {
            pr_err("STRICT_PIN_CHECK: Pin detected in VMA at addr 0x%lx\n", addr);
            return ret;
        }
    }
    
    return 0;
}

#ifdef CONFIG_DEBUG_VM
/**
 * 调试版本：提供更详细的信息
 */
void folio_debug_pin_status(struct folio *folio)
{
    struct pin_status_info info;
    enum strict_pin_result result;
    
    result = __folio_strict_pin_check(folio, &info);
    
    pr_info("=== FOLIO PIN STATUS DEBUG ===\n");
    pr_info("PFN: %lu, Order: %u\n", folio_pfn(folio), folio_order(folio));
    pr_info("Reference count: %d\n", info.ref_count);
    pr_info("Map count: %d\n", info.map_count);
    pr_info("Expected base refs: %d\n", info.expected_base_refs);
    pr_info("Has precise tracking: %s\n", info.has_precise_tracking ? "yes" : "no");
    
    if (info.has_precise_tracking) {
        pr_info("Precise pin count: %d\n", info.pin_count);
    } else {
        int excess = info.ref_count - info.expected_base_refs;
        pr_info("Excess refs: %d\n", excess);
        if (excess >= GUP_PIN_COUNTING_BIAS) {
            pr_info("Estimated pin count: %d\n", excess / GUP_PIN_COUNTING_BIAS);
        }
    }
    
    pr_info("Result: %s\n", 
            result == STRICT_PIN_NOT_PINNED ? "NOT_PINNED" :
            result == STRICT_PIN_DEFINITELY_PINNED ? "DEFINITELY_PINNED" :
            result == STRICT_PIN_LIKELY_PINNED ? "LIKELY_PINNED" : "UNCERTAIN");
    pr_info("Reason: %s\n", info.reason);
    pr_info("==============================\n");
}
#endif

/**
 * 在关键路径中使用的校验宏
 */

/* 内存回收前的校验 */
#define ASSERT_FOLIO_NOT_PINNED_FOR_RECLAIM(folio) \
    do { \
        int __ret = folio_assert_not_pinned(folio, \
            "Attempting to reclaim pinned folio"); \
        if (__ret) { \
            pr_err("RECLAIM_ERROR: Cannot reclaim pinned folio pfn=%lu\n", \
                   folio_pfn(folio)); \
            return __ret; \
        } \
    } while (0)

/* 页面迁移前的校验 */
#define ASSERT_FOLIO_NOT_PINNED_FOR_MIGRATION(folio) \
    do { \
        int __ret = folio_assert_not_pinned(folio, \
            "Attempting to migrate pinned folio"); \
        if (__ret) { \
            pr_err("MIGRATION_ERROR: Cannot migrate pinned folio pfn=%lu\n", \
                   folio_pfn(folio)); \
            return __ret; \
        } \
    } while (0)

/* THP分割前的校验 */
#define ASSERT_FOLIO_NOT_PINNED_FOR_SPLIT(folio) \
    do { \
        int __ret = folio_assert_not_pinned(folio, \
            "Attempting to split pinned THP folio"); \
        if (__ret) { \
            pr_err("SPLIT_ERROR: Cannot split pinned THP folio pfn=%lu\n", \
                   folio_pfn(folio)); \
            return __ret; \
        } \
    } while (0)

/* 通用的严格校验宏 */
#define STRICT_ASSERT_NOT_PINNED(folio, operation) \
    do { \
        int __ret = folio_assert_not_pinned(folio, \
            "Operation '" operation "' on pinned folio"); \
        if (__ret) { \
            pr_err("STRICT_PIN_ERROR: " operation " failed on pinned folio pfn=%lu\n", \
                   folio_pfn(folio)); \
            return __ret; \
        } \
    } while (0)

/**
 * 条件编译的校验版本
 */
#ifdef CONFIG_DEBUG_VM_STRICT_PIN_CHECK
#define DEBUG_ASSERT_NOT_PINNED(folio, msg) \
    folio_assert_not_pinned(folio, msg)
#else
#define DEBUG_ASSERT_NOT_PINNED(folio, msg) do { } while (0)
#endif

/**
 * 示例使用函数
 */
static int example_memory_operation(struct folio *folio)
{
    /* 在进行可能与pin冲突的操作前进行校验 */
    STRICT_ASSERT_NOT_PINNED(folio, "memory operation");
    
    /* 继续进行内存操作... */
    pr_info("Memory operation proceeding on folio pfn=%lu\n", folio_pfn(folio));
    
    return 0;
}

/**
 * 高级校验：结合多种信息源
 */
static bool folio_advanced_pin_analysis(struct folio *folio)
{
    struct pin_status_info info;
    enum strict_pin_result result;
    
    result = __folio_strict_pin_check(folio, &info);
    
    if (result == STRICT_PIN_DEFINITELY_PINNED || 
        result == STRICT_PIN_NOT_PINNED) {
        return result == STRICT_PIN_DEFINITELY_PINNED;
    }
    
    /* 对于不确定的情况，进行进一步分析 */
    
    /* 检查是否是共享页面（更可能有高引用计数） */
    if (folio_test_anon(folio)) {
        /* 匿名页面通常引用计数较低，更可能是pin */
        return result == STRICT_PIN_LIKELY_PINNED;
    }
    
    /* 文件页面可能有很多映射，需要更仔细的分析 */
    if (info.map_count > 100) {
        /* 高映射计数的文件页面，假阳性可能性高 */
        return false;
    }
    
    return result == STRICT_PIN_LIKELY_PINNED;
}