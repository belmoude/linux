# Confidential Computing Page State Migration

这个项目实现了在机密计算环境下页面加密状态的原子切换机制，解决了页面加密状态与页表PTE条目的C-bit并非原子操作带来的安全问题。

## 背景问题

在机密计算（Confidential Computing）环境中（如AMD SEV、Intel TDX），内存页面有两种状态：
- **私有（Private）**：页面被加密，只有可信执行环境可以访问
- **共享（Shared）**：页面未加密，可以与主机操作系统共享

问题在于：
1. 页面的实际加密状态切换（通过硬件指令）
2. 页表PTE条目中C-bit的更新

这两个操作不是原子的，如果与并发的内存访问发生竞争，可能导致安全漏洞。

## 解决方案

本项目借鉴Linux内核页面迁移（page migration）机制的思路：

1. **插入迁移条目**：将所有映射到目标页面的PTE替换为特殊的"CC迁移条目"
2. **阻塞并发访问**：任何对该页面的访问都会触发缺页异常并阻塞等待
3. **原子状态切换**：在确保没有并发访问的情况下，安全地执行加密/解密操作
4. **恢复正常映射**：更新C-bit并恢复正常的PTE映射

## 核心特性

### 原子性保证
- 使用迁移条目机制确保状态切换期间没有并发访问
- 所有并发访问被自动阻塞直到切换完成

### 高性能
- 批量操作支持多页面同时切换
- 优化的TLB刷新减少性能开销
- 支持异步操作模式

### 安全性
- 防止状态切换期间的数据泄露
- 完整的错误恢复机制
- 权限检查和访问控制

## 架构设计

```
用户空间应用
    ↓ (系统调用)
cc_syscall.c - 系统调用接口
    ↓
cc_page_migration.c - 核心状态切换逻辑
    ↓
cc_pte_ops.c - PTE操作和迁移条目管理
    ↓
硬件加密/解密接口
```

### 主要组件

#### 1. `cc_page_migration.h` - 头文件和数据结构
定义了核心数据结构和接口：
- `enum cc_page_state` - 页面状态枚举
- `struct cc_migration_entry` - CC迁移条目结构
- `struct cc_page_info` - 页面状态跟踪信息

#### 2. `cc_page_migration.c` - 核心实现
实现状态切换的主要逻辑：
- `cc_switch_page_state()` - 单页面状态切换
- `cc_switch_page_range()` - 批量页面状态切换
- 迁移条目管理和同步机制

#### 3. `cc_pte_ops.c` - PTE操作
实现底层的页表操作：
- `cc_try_to_unmap()` - 替换PTE为迁移条目
- `cc_remove_migration_ptes()` - 恢复正常PTE映射
- `cc_update_page_cbit()` - 更新C-bit状态

#### 4. `cc_syscall.c` - 系统调用接口
提供用户空间接口：
- `cc_set_page_state()` - 设置页面加密状态
- `cc_get_page_state()` - 获取页面状态信息
- `cc_batch_set_state()` - 批量状态设置

#### 5. `test_cc_migration.c` - 测试程序
全面的测试套件：
- 基本功能测试
- 批量操作测试
- 并发访问测试

## 编译和安装

### 环境要求
- Linux内核开发环境
- GCC编译器
- Make工具
- pthread库

### 编译步骤

```bash
# 编译内核模块和测试程序
make all

# 仅编译内核模块
make module

# 仅编译测试程序
make test
```

### 安装和加载

```bash
# 安装内核模块
sudo make install

# 加载模块
sudo make load

# 卸载模块
sudo make unload
```

## 使用方法

### 系统调用接口

#### 设置页面状态
```c
#include <sys/syscall.h>

// 将页面设置为私有（加密）状态
int ret = syscall(__NR_cc_set_page_state, page_addr, CC_PAGE_PRIVATE, CC_MIGRATE_SYNC);

// 将页面设置为共享（非加密）状态
int ret = syscall(__NR_cc_set_page_state, page_addr, CC_PAGE_SHARED, CC_MIGRATE_SYNC);
```

#### 获取页面状态
```c
struct cc_page_state_info info;
int ret = syscall(__NR_cc_get_page_state, page_addr, &info);

printf("Page state: %s\n", 
       info.current_state == CC_PAGE_PRIVATE ? "PRIVATE" : "SHARED");
```

#### 批量操作
```c
struct cc_batch_request requests[3];
requests[0].start_addr = addr1;
requests[0].end_addr = addr1 + PAGE_SIZE;
requests[0].target_state = CC_PAGE_PRIVATE;
requests[0].flags = CC_MIGRATE_SYNC;

int successful = syscall(__NR_cc_batch_set_state, requests, 3);
```

### 运行测试

```bash
# 编译并运行测试
make run-test
```

测试程序包括：
1. **基本功能测试** - 验证单页面状态切换
2. **批量操作测试** - 验证多页面同时切换
3. **并发访问测试** - 验证并发安全性

## 技术细节

### 迁移条目机制

当页面需要切换状态时：

1. **获取页面锁** - 防止其他操作干扰
2. **创建CC迁移条目** - 包含目标状态信息
3. **替换所有PTE** - 使用rmap机制找到所有映射
4. **刷新TLB** - 确保CPU缓存一致性
5. **执行状态切换** - 调用硬件加密/解密接口
6. **恢复PTE映射** - 设置正确的C-bit状态
7. **释放页面锁** - 允许正常访问

### 并发处理

任何试图访问处于切换状态页面的进程会：

1. **触发缺页异常** - 因为PTE被替换为迁移条目
2. **识别CC迁移条目** - 页面错误处理程序识别特殊条目
3. **进入等待状态** - 在等待队列中阻塞
4. **状态切换完成后唤醒** - 重试页面访问
5. **正常访问页面** - 使用新的加密状态

### 错误处理

系统提供完整的错误恢复机制：

- **状态切换失败** - 恢复原始PTE映射和页面状态
- **内存不足** - 释放已分配的资源
- **权限错误** - 安全地拒绝访问
- **硬件错误** - 记录错误并恢复一致状态

## 性能考虑

### 优化策略
- **批量TLB刷新** - 减少TLB操作开销
- **内存池管理** - 预分配迁移条目结构
- **异步操作支持** - 非阻塞状态切换
- **统计信息收集** - 性能监控和调优

### 性能指标
- **状态切换延迟** - 单页面切换时间
- **并发访问阻塞时间** - 等待状态切换完成的时间
- **批量操作吞吐量** - 单位时间处理的页面数量

## 安全考虑

### 权限控制
- **CAP_SYS_ADMIN权限** - 批量操作需要管理员权限
- **VMA检查** - 只能修改进程自己的内存
- **地址验证** - 确保页面地址有效

### 数据保护
- **原子状态切换** - 防止中间状态暴露
- **完整性检查** - 验证状态切换结果
- **错误恢复** - 确保失败时的安全状态

## 支持的硬件平台

目前支持以下机密计算平台：

### AMD SEV (Secure Encrypted Virtualization)
- SEV-ES (Encrypted State)
- SEV-SNP (Secure Nested Paging)

### Intel TDX (Trust Domain Extensions)
- 基于Intel VT技术的可信执行环境

### 扩展支持
架构设计支持轻松添加新的硬件平台支持。

## 贡献指南

### 代码风格
- 遵循Linux内核编码规范
- 使用`make dev-check`检查代码风格

### 测试要求
- 所有新功能必须包含相应测试
- 确保并发安全性测试通过

### 文档更新
- 更新相关API文档
- 添加使用示例

## 许可证

本项目采用GPL-2.0许可证，与Linux内核保持一致。

## 联系方式

如有问题或建议，请通过以下方式联系：
- 创建GitHub Issue
- 发送邮件到开发邮件列表

---

**注意**：这是一个实验性实现，用于研究和演示目的。在生产环境中使用前，请进行充分的测试和安全评估。