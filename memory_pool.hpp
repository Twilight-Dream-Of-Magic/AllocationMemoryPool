#pragma once
#ifndef MEMORY_POOL_HPP
#define MEMORY_POOL_HPP

#include "os_memory.hpp"

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <cassert>

#include <iostream>
#include <algorithm>
#include <utility>
#include <type_traits>
#include <new>
#include <mutex>
#include <shared_mutex>
#include <memory>

/**
 * @file memory_pool.hpp
 * @brief 分层内存池实现 / Hierarchical Memory Pool
 * 
 * @details
 * 采用四层结构处理不同尺寸的内存分配：
 * 1. 小内存 (<1 MiB)  – 线程本地缓存 + 全局 64 桶
 * 2. 中内存 (1 MiB–512 MiB) – 无锁共享空闲链表 + Buddy-based + 标记抢占法
 * 3. 大内存 (512 MiB–1 GiB) – 直接操作系统分配并跟踪 (因为没有任何算法能够处理这个大小)
 * 4. 超大内存 (≥1 GiB) – 单块映射，释放即返还操作系统
 */

// ============================ 对齐常量 ============================
static constexpr std::size_t CLASS_DEFAULT_ALIGNMENT = 64;			//!< 类默认对齐 / Class Default alignment
static constexpr std::size_t DEFAULT_ALIGNMENT = alignof( void* );	//!< 默认对齐 / Default alignment
static constexpr std::size_t MIN_ALLOWED_ALIGNMENT = 2;				//!< 最小对齐 / Minimum alignment
static constexpr std::size_t MAX_ALLOWED_ALIGNMENT = 64 * 1024;		//!< 最大允许对齐值 / Maximum allowed alignment (64 KiB)

// GCC/Clang/MSVC 在 x86_64 & AArch64 下均支持 16-byte CompareAndSwap
// ====================== 128-bit CompareAndSwap 支持探测 ======================
#if ( defined( __x86_64__ ) || defined( _M_X64 ) || defined( __aarch64__ ) ) && defined( __cpp_lib_atomic_ref ) || ( defined( __SSE2__ ) || ( defined( _M_IX86_FP ) && _M_IX86_FP >= 2 ) || ( defined( __ARM_NEON ) || defined( __aarch64__ ) ) )
#define SUPPORT_128BIT_CAS 1
#else
#define SUPPORT_128BIT_CAS 0
#endif

static constexpr std::uint64_t ALIGN_SENTINEL = 0xDEADBEEFCAFEBABEull;	//!< 对齐标识符 / Alignment sentinel
struct AlignHeader
{
	std::uint64_t tag;	 //!< == ALIGN_SENTINEL
	void*		  raw;	 //!< 原始块首地址 / Raw block base address
	std::size_t	  size;	 //!< 总字节数 / Total bytes
};
static constexpr std::size_t ALIGN_HEADER_BYTES = sizeof( AlignHeader );  //!< 对齐头部字节数 / Alignment header size

struct NotAlignHeader
{
	std::uint32_t owner_type;  //!< 0 = nullpointer 1 = Small, 2 = Medium, 3 = Large, 4 = Huge
	void*		  raw;		   //!< 原始块首地址 / Raw block base address
};

static constexpr std::size_t NOT_ALIGN_HEADER_BYTES = sizeof( NotAlignHeader );	 //!< 对齐头部字节数 / Alignment header size

// ============================ Header 结构 ============================

/* -------------- 小块头 -------------- */
struct alignas( CLASS_DEFAULT_ALIGNMENT ) SmallMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x534D4853;	//!< 'SMHS'
	std::uint32_t				   magic;				//!< 魔法值 / Magic value
	std::uint32_t				   bucket_index;		//!< 桶索引 / Bucket index
	std::uint32_t				   block_size;			//!< 块大小 / Block size
	std::atomic<bool>			   is_free;				//!< 是否空闲 / Free flag
	SmallMemoryHeader*			   next;				//!< 下一个块头 / Next block header
	std::uint8_t				   in_tls;

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( SmallMemoryHeader );  //!< 获取数据指针 / Get data pointer
	}
};

/* -------------- 中块头 / 脚 -------------- */
struct alignas( CLASS_DEFAULT_ALIGNMENT ) MediumMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x4D4D4853;	//!< 'MMHS'
	std::uint32_t				   magic;				//!< 魔法值 / Magic value
	std::size_t					   block_size;			//!< 块大小 / Block size
	std::atomic<bool>			   is_free;				//!< 是否空闲 / Free flag
	MediumMemoryHeader*			   next;				//!< 下一个块头 / Next block header

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( MediumMemoryHeader );	//!< 获取数据指针 / Get data pointer
	}
};



/* -------------- 大 / 超大 块头 -------------- */
struct alignas( CLASS_DEFAULT_ALIGNMENT ) LargeMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x4C4D4853;	//!< 'LMHS'
	std::uint32_t				   magic;				//!< 魔法值 / Magic value
	std::size_t					   block_size;			//!< 块大小 / Block size

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( LargeMemoryHeader );  //!< 获取数据指针 / Get data pointer
	}
};

struct alignas( CLASS_DEFAULT_ALIGNMENT ) HugeMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x484D4853;	//!< 'HMHS'
	std::uint32_t				   magic;				//!< 魔法值 / Magic value
	std::size_t					   block_size;			//!< 块大小 / Block size

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( HugeMemoryHeader );  //!< 获取数据指针 / Get data pointer
	}
};

/*------------------------------------------------------------------*\
|  1. SmallMemoryManager — 64 桶 + TLS + 全局 ABA-safe 栈             |
\*------------------------------------------------------------------*/
struct alignas( CLASS_DEFAULT_ALIGNMENT ) SmallMemoryManager
{
	static constexpr std::size_t BUCKET_COUNT = 64;	 //!< 桶数量 / Number of buckets

	/* ==========================================================
        * 64-桶尺寸表 (前 32 线性 + 后 32 几何级数，最后落 1 MiB)
        * ========================================================== */
	static constexpr std::array<std::size_t, BUCKET_COUNT> BUCKET_SIZES = { 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 128, 136, 144, 152, 160, 168, 176, 184, 192, 200, 208, 216, 224, 232, 240, 248, 256, 336, 432, 560, 728, 944, 1224, 1584, 2048, 2656, 3448, 4472, 5800, 7520, 9744, 12640, 16384, 21248, 27560, 35736, 46344, 60104, 77936, 101072, 131072, 169984, 220440, 285872, 370728, 480776, 623488, 808568, 1048576 };

	// -------- Thread-local 缓存 --------
	struct ThreadLocalCache
	{
		SmallMemoryHeader* buckets[ BUCKET_COUNT ] = { nullptr };  //!< 每个桶的本地缓存 / Thread-local cache for each bucket
		std::size_t		   deallocation_counter = 0;			   //!< 释放计数 / Deallocation counter
	};
	static thread_local ThreadLocalCache thread_local_cache;

	// -------- 全局桶头 (ABA-safe) --------
	struct alignas( CLASS_DEFAULT_ALIGNMENT ) PointerTag  // [FIX] 16 B 对齐，保证单指令 CAS
	{
		SmallMemoryHeader* pointer;	 //!< 指向内存块的指针 / Pointer to memory block
		std::uint64_t	   tag;		 //!< 标记 / Tag
	};

	struct alignas( CLASS_DEFAULT_ALIGNMENT ) GlobalBucket
	{
#if SUPPORT_128BIT_CAS
		std::atomic<PointerTag> head;  //!< 原子头指针 / Atomic head pointer
#else
		std::atomic<SmallMemoryHeader*> head;	//!< 原子头指针 / Atomic head pointer
		std::mutex						mutex;	//!< 互斥锁 / Mutex for synchronization
#endif
	};
	std::array<GlobalBucket, BUCKET_COUNT> global_buckets;

	// -------- 已分配 Chunk 跟踪 --------
	std::mutex								   chunk_mutex;		  //!< 锁保护的已分配块 / Mutex for allocated chunks
	std::vector<std::pair<void*, std::size_t>> allocated_chunks;  //!< 已分配的块 / Allocated chunks

	// ======================== 桶映射函数（保持外部接口名） ========================
	[[nodiscard]] static std::size_t calculate_bucket_index( std::size_t bytes )
	{
		std::size_t low = 0, high = BUCKET_COUNT - 1;
		while ( low < high )
		{
			const std::size_t middle = ( low + high ) >> 1;
			if ( bytes <= BUCKET_SIZES[ middle ] )
				high = middle;
			else
				low = middle + 1;
		}
		return low;
	}

	// ----------------------- 核心接口 -----------------------
	void* allocate( std::size_t bytes, std::size_t alignment );
	void  deallocate( SmallMemoryHeader* header );
	void  flush_thread_local_cache();
	void  release_resources();
};

// ============================ 中内存管理器 ============================
struct MediumMemoryManager
{
	static constexpr std::size_t MIN_BUCKET_BYTES_UNIT = 1 << 20;  //!< 1 MiB / Minimum bucket bytss unit size
	static constexpr int		 LEVEL_COUNT = 10;				   //!< 1 MiB–512 MiB / Number of levels
	static constexpr std::size_t CHUNK_SIZE = 64ull << 20;		   // 64 MiB per chunk / Chunk size for OS allocation

	struct alignas( CLASS_DEFAULT_ALIGNMENT ) PointerTag
	{
		MediumMemoryHeader* pointer;
		std::uint64_t		tag;
	};

	struct alignas( CLASS_DEFAULT_ALIGNMENT ) FreeList
	{
		std::atomic<PointerTag> head;
		std::atomic_size_t		count { 0 };
	};
	std::atomic<uint16_t> free_list_level_mask { 0 };

	struct alignas( CLASS_DEFAULT_ALIGNMENT ) MergeRequest
	{
		MediumMemoryHeader* block;
		int					order;
	};

	static constexpr size_t MERGE_QUEUE_SIZE = 128;
	struct alignas( CLASS_DEFAULT_ALIGNMENT ) MergeQueue
	{
		std::atomic<MergeRequest> requests[ MERGE_QUEUE_SIZE ];	 //!< 固定大小环形缓冲区 / Fixed-size circular buffer
		std::atomic<std::size_t>  head { 0 };
		std::atomic<std::size_t>  tail { 0 };
	};

	MergeQueue		  merge_queue;
	std::atomic<bool> merge_worker_active { false };

	std::array<FreeList, LEVEL_COUNT>		   free_lists;
	std::mutex								   chunk_mutex;
	std::vector<std::pair<void*, std::size_t>> allocated_chunks;

	// ----------------------- 核心接口 -----------------------
	void* allocate( std::size_t bytes, std::size_t alignment );
	void  deallocate( MediumMemoryHeader* header );
	void  release_resources();

private:
	static int order_from_size( std::size_t actual_byte_size )
	{
		std::size_t need = actual_byte_size;
		if ( need < MIN_BUCKET_BYTES_UNIT )
			need = MIN_BUCKET_BYTES_UNIT;

		int			order = 0;
		std::size_t cap = MIN_BUCKET_BYTES_UNIT;
		while ( cap < need && order < LEVEL_COUNT - 1 )
		{
			cap <<= 1;
			++order;
		}
		return order;
	}
	static std::size_t size_from_order( int order )
	{
		return MIN_BUCKET_BYTES_UNIT << order;
	};

	void				prepare_block( MediumMemoryHeader* block, int order );
	
	/**
	 * @brief 将内存块推入空闲链表 / Pushes a memory block into the free list
	 * 
	 * @details
	 * 此函数实现将空闲内存块安全添加到指定层级的空闲链表头部。
	 * 使用原子操作解决并发冲突，并更新空闲层级位掩码状态。
	 * 
	 * This function safely adds a free memory block to the head of the specified order's free list.
	 * Uses atomic operations to handle concurrent access and updates the free level mask status.
	 *
	 * @param header 要添加的内存块头部指针 / Pointer to the memory block header to add
	 * @param order 内存块所在的层级 / Order of the memory block
	 */
	void				push_block( MediumMemoryHeader* header, int order );

	/**
	 * @brief 从空闲链表弹出内存块 / Pops a memory block from the free list
	 * 
	 * @details
	 * 此函数实现从指定层级的空闲链表安全移除并返回头部内存块。
	 * 使用原子操作解决并发冲突，并更新空闲层级位掩码状态。
	 * 
	 * This function safely removes and returns the head block from the specified order's free list.
	 * Uses atomic operations to handle concurrent access and updates the free level mask status.
	 *
	 * @param order 目标层级 / Target order
	 * @return MediumMemoryHeader* 弹出的内存块头部指针，链表为空时返回nullptr
	 *         Pointer to popped memory block header, nullptr if list empty
	 */
	MediumMemoryHeader* pop_block( int order );

	/**
	 * @brief 处理合并请求队列 / Processes the merge request queue
	 * 
	 * @details
	 * 此函数运行在专用合并工作线程中，持续处理异步合并请求队列。
	 * 实现环形缓冲区的无锁消费逻辑，高效处理伙伴块合并请求。
	 * 
	 * This function runs in a dedicated merge worker thread, continuously processing
	 * the asynchronous merge request queue. Implements lock-free consumption logic
	 * for a circular buffer to efficiently handle buddy block merge requests.
	 *
	 * @note 环形缓冲区工作流程 / Circular buffer workflow:
	 *       [头指针] → 待处理请求 → [尾指针]
	 *       [Head] → pending requests → [Tail]
	 *       缓冲区满时: (尾+1)%size == 头
	 *       缓冲区空时: 头 == 尾
	 */
	void				process_merge_queue();

	/**
	 * @brief 尝试从空闲链表中移除特定内存块 / Attempts to remove a specific memory block from the free list
	 * 
	 * @details
	 * 此函数实现无锁链表中特定节点的安全移除。当需要合并内存块时，必须先将伙伴块从空闲链表移除。
	 * 函数处理两种场景：
	 *   1. 目标块是链表头节点
	 *   2. 目标块位于链表中间
	 * 使用原子操作和标签（tag）解决ABA问题，确保并发环境下的操作安全。
	 * 
	 * This function implements safe removal of a specific node from a lock-free linked list.
	 * When merging memory blocks, the buddy block must first be removed from the free list.
	 * The function handles two scenarios:
	 *   1. The target block is the head of the list
	 *   2. The target block is in the middle of the list
	 * Uses atomic operations with tags to solve ABA problems and ensure safety in concurrent environments.
	 *
	 * @param header 要移除的内存块头部指针 / Pointer to the memory block header to remove
	 * @param order 内存块所在的层级 / Order of the memory block
	 * @return true 成功移除 / Successfully removed
	 * @return false 移除失败（块不在链表中）/ Removal failed (block not in list)
	 *
	 * @note ABA问题解决方案 / ABA problem solution:
	 *       每个原子指针附带tag计数器，每次修改递增tag
	 *       Each atomic pointer has a tag counter that increments on every modification
	 */
	bool				try_remove_from_freelist( MediumMemoryHeader* header, int order );

	/**
	 * @brief 尝试合并伙伴内存块 / Attempts to merge buddy memory blocks
	 * 
	 * @details
	 * 此函数实现伙伴系统的核心合并逻辑。当内存块被释放时，会尝试与其伙伴块（地址相邻且大小相等的块）合并，
	 * 形成更大的内存块。合并操作会递归进行，直到无法再合并或达到最大块大小。
	 * 
	 * This function implements the core merging logic of the buddy system. When a memory block is freed,
	 * it attempts to merge with its buddy block (an adjacent block of the same size) to form a larger block.
	 * The merging process recurses until no more merges are possible or the maximum block size is reached.
	 *
	 * @param block 指向要尝试合并的内存块头部的指针 / Pointer to the memory block header to merge
	 * @param order 当前内存块的层级 / Current order of the memory block
	 *
	 * @note 内存布局示例 / Memory layout example:
	 *       合并前 (Before merge):
	 *          [块A 头部] [块A 数据] [块B 头部] [块B 数据]  (块A和块B是伙伴块)
	 *          [Block A Header] [Block A Data] [Block B Header] [Block B Data] (A and B are buddies)
	 *       
	 *       合并后 (After merge):
	 *          [合并块头部] [合并数据区域]
	 *          [Merged Block Header] [Merged Data Area]
	 */
	void				try_merge_buddy( MediumMemoryHeader* block, int order );

	/**
	 * @brief 将内存块分割到指定层级 / Splits a memory block to a specified order
	 * 
	 * @details
	 * 此函数用于将较大的内存块分割成更小的块（按照伙伴系统规则）直到达到目标层级。
	 * 每次分割将当前块分成两个相等大小的伙伴块，右伙伴块被标记为空闲并加入对应层级的空闲链表。
	 * 
	 * This function splits a larger memory block into smaller blocks (following the buddy system rules)
	 * until it reaches the target order. Each split divides the current block into two equal-sized buddy
	 * blocks, with the right buddy marked as free and added to the free list of its order.
	 *
	 * @param block      指向要分割的内存块头部的指针 / Pointer to the memory block header to split
	 * @param from_order 当前块的层级 / Current order of the block
	 * @param to_order   目标分割层级 / Target order to split to
	 * @return           分割后剩余的左侧块头部指针 / Pointer to the left block header after splitting
	 *
	 * @note 内存布局示例 / Memory layout example:
	 *       分割前 (Before split): 
	 *          [块头部 | 数据区域] (大小 = size_from_order(from_order))
	 *          [Header | Data Area] (size = size_from_order(from_order))
	 *       
	 *       分割后 (After split):
	 *          [左块头部 | 左数据] [右块头部 | 右数据]
	 *          [Left Header | Left Data] [Right Header | Right Data]
	 *          |←── half ──→| |←── half ──→|
	 *          |←─────── size_from_order(from_order) ───────→|
	 */
	MediumMemoryHeader* split_to_order( MediumMemoryHeader* block, int from_order, int to_order );
	MediumMemoryHeader* request_new_chunk( int min_order, std::size_t alignment );
};

/*------------------------------------------------------------------*/
struct LargeMemoryManager
{
	std::mutex						tracking_mutex;
	std::vector<LargeMemoryHeader*> active_blocks;

	void* allocate( std::size_t bytes, std::size_t alignment );
	void  deallocate( LargeMemoryHeader* header );
	void  release_resources();
};

/*------------------------------------------------------------------*/
struct HugeMemoryManager
{
	std::mutex								   tracking_mutex;
	std::vector<std::pair<void*, std::size_t>> active_blocks;

	void* allocate( std::size_t bytes, std::size_t alignment );
	void  deallocate( HugeMemoryHeader* header );
	void  release_resources();
};

// ============================ MemoryPool 主类 ============================
class MemoryPool
{
private:
	// ------------------ 层级尺寸阈值 ------------------
	static constexpr std::size_t SMALL_BLOCK_MAX_SIZE = 1 * 1024 * 1024;		 //!< 1 MiB (含) / Up to 1 MiB
	static constexpr std::size_t MEDIUM_BLOCK_MAX_SIZE = 512 * 1024 * 1024;		 //!< 512 MiB (含) / Up to 512 MiB
	static constexpr std::size_t HUGE_BLOCK_THRESHOLD = 1 * 1024 * 1024 * 1024;	 //!< 1 GiB (含) / Up to 1 GiB

	// ======================== 成员实例 ========================
	SmallMemoryManager	small_manager;	 //!< 小内存管理器实例 / Small memory manager instance
	MediumMemoryManager medium_manager;	 //!< 中内存管理器实例 / Medium memory manager instance
	LargeMemoryManager	large_manager;	 //!< 大内存管理器实例 / Large memory manager instance
	HugeMemoryManager	huge_manager;	 //!< 超大内存管理器实例 / Huge memory manager instance

	std::atomic<bool>		 is_destructing { false };	  //!< 析构标记 / Destruction flag
	static std::atomic<bool> construction_warning_shown;  //!< 构造警告是否已显示 / Whether construction warning has been shown

public:
	MemoryPool();
	~MemoryPool();

	void* allocate( std::size_t bytes, std::size_t alignment = MIN_ALLOWED_ALIGNMENT, const char* source_file = nullptr, std::uint32_t source_line = 0, bool nothrow = false );
	void  deallocate( void* pointer );
	void  flush_current_thread_cache();
};


namespace os_memory::memory_pool
{
	/* ============================================================
 *  内部小工具 / Internal Utilities
 * ============================================================ */

	/**
	 * @brief 严格检查 value 是否为非零的 2 的幂，并返回它本身
	 *        Strictly checks whether value is a non-zero power of two, then returns it.
	 *
	 * @details
	 * - Release 模式下使用 std::abort() 立即中止程序
	 *   In Release builds, calls std::abort() to immediately terminate.
	 * - Debug 模式下使用 assert() 触发断言，便于定位问题
	 *   In Debug builds, uses assert() to trigger an assertion failure for easier debugging.
	 *
	 * @param value 要检查的值 / The value to check.
	 * @return 传入的 value（保证是非零的 2 的幂）/ The input value (guaranteed to be a non-zero power of two).
	 * @note 传入不合法时会中止程序，不抛异常
	 *       Program terminates on invalid input; no exceptions thrown.
	 */
	constexpr inline bool is_power_of_two( std::size_t value ) noexcept
	{
#if !defined( _DEBUG )
		// Release 模式：value==0 或 非 2 的幂 时中止 / Release mode: abort on zero or non-power-of-two
		if ( value != 1 || ( value & ( value - 1 ) ) != 0 )
		{
			return false;
		}
#else
		// Debug 模式：断言 value 为非零的 2 的幂 / Debug mode: assert value is non-zero power of two
		assert( value != 1 && ( value & ( value - 1 ) ) == 0 && "ensure_power_of_two: value must be non-zero power of two" );
#endif
		return true;
	}
}  // namespace os_memory::memory_pool

#endif	// MEMORY_POOL_HPP
