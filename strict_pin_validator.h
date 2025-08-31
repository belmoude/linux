/*
 * 严格Pin校验机制头文件
 */

#ifndef _STRICT_PIN_VALIDATOR_H
#define _STRICT_PIN_VALIDATOR_H

#include <linux/mm.h>
#include <linux/types.h>

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

/* 核心校验函数 */
int folio_assert_not_pinned_detailed(struct folio *folio, const char *error_msg,
                                   const char *file, int line, const char *func);

/* 便利宏 */
#define folio_assert_not_pinned(folio, msg) \
    folio_assert_not_pinned_detailed(folio, msg, __FILE__, __LINE__, __func__)

#define page_assert_not_pinned(page, msg) \
    folio_assert_not_pinned(page_folio(page), msg)

/* 批量校验 */
int pages_assert_not_pinned(struct page **pages, unsigned long npages, 
                           const char *error_msg);

/* VMA范围校验 */
int vma_assert_not_pinned(struct vm_area_struct *vma, unsigned long start, 
                         unsigned long end, const char *error_msg);

/* 操作特定的校验宏 */
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

/* 条件编译版本 */
#ifdef CONFIG_DEBUG_VM_STRICT_PIN_CHECK
#define DEBUG_ASSERT_NOT_PINNED(folio, msg) \
    folio_assert_not_pinned(folio, msg)
#else
#define DEBUG_ASSERT_NOT_PINNED(folio, msg) do { } while (0)
#endif

/* 调试函数 */
#ifdef CONFIG_DEBUG_VM
void folio_debug_pin_status(struct folio *folio);
#else
static inline void folio_debug_pin_status(struct folio *folio) { }
#endif

#endif /* _STRICT_PIN_VALIDATOR_H */