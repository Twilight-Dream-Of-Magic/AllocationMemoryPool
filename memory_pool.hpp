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
#include <mutex>
#include <vector>
#include <utility>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <forward_list>
#include <memory>
#include <array>
#include <new>

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
static constexpr std::size_t CACHE_LINE_SIZE = 64;	  //!< 缓存行大小 / Cache line size
static constexpr std::size_t DEFAULT_ALIGNMENT = 16;  //!< 默认对齐 / Default alignment
static constexpr std::size_t MIN_ALIGNMENT = 8;		  //!< 最小对齐 / Minimum alignment

// GCC/Clang/MSVC 在 x86_64 & AArch64 下均支持 16-byte CompareAndSwap
// ====================== 128-bit CompareAndSwap 支持探测 ======================
#if ( defined( __x86_64__ ) || defined( _M_X64 ) || defined( __aarch64__ ) ) && defined( __cpp_lib_atomic_ref ) || ( defined( __SSE2__ ) || ( defined( _M_IX86_FP ) && _M_IX86_FP >= 2 ) || ( defined( __ARM_NEON ) || defined( __aarch64__ ) ) )
#define SUPPORT_128BIT_CAS 1
#else
#define SUPPORT_128BIT_CAS 0
#endif

static constexpr std::uint64_t ALIGN_SENTINEL = 0xDEADBEEFCAFEBABEull;	//!< 对齐标识符 / Alignment sentinel
using AlignHeader = struct
{
	std::uint64_t tag;	 //!< == ALIGN_SENTINEL
	void*		  raw;	 //!< 原始块首地址 / Raw block base address
	std::size_t	  size;	 //!< 总字节数 / Total bytes
};
static constexpr std::size_t ALIGN_HEADER_BYTES = sizeof( AlignHeader );  //!< 对齐头部字节数 / Alignment header size

// ============================ Header 结构 ============================

/* -------------- 小块头 -------------- */
struct alignas( DEFAULT_ALIGNMENT ) SmallMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x534D4853;	//!< 'SMHS'
	std::uint32_t				   magic;				//!< 魔法值 / Magic value
	std::uint32_t				   bucket_index;		//!< 桶索引 / Bucket index
	std::uint32_t				   block_size;			//!< 块大小 / Block size
	std::atomic<bool>			   is_free;				//!< 是否空闲 / Free flag
	SmallMemoryHeader*			   next;				//!< 下一个块头 / Next block header

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( SmallMemoryHeader );  //!< 获取数据指针 / Get data pointer
	}
};

/* -------------- 中块头 / 脚 -------------- */
struct alignas( DEFAULT_ALIGNMENT ) MediumMemoryHeader
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

struct alignas( DEFAULT_ALIGNMENT ) MediumMemoryFooter
{
	std::size_t block_size;	 //!< 块大小 / Block size
};

/* -------------- 大 / 超大 块头 -------------- */
struct alignas( DEFAULT_ALIGNMENT ) LargeMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x4C4D4853;	//!< 'LMHS'
	std::uint32_t				   magic;				//!< 魔法值 / Magic value
	std::size_t					   block_size;			//!< 块大小 / Block size

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( LargeMemoryHeader );  //!< 获取数据指针 / Get data pointer
	}
};

struct alignas( DEFAULT_ALIGNMENT ) HugeMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x484D4853;	//!< 'HMHS'
	std::uint32_t				   magic;				//!< 魔法值 / Magic value
	std::size_t					   block_size;			//!< 块大小 / Block size

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( HugeMemoryHeader );  //!< 获取数据指针 / Get data pointer
	}
};

// ============================ MemoryPool 主类 ============================
class MemoryPool
{
private:
	// ------------------ 层级尺寸阈值 ------------------
	static constexpr std::size_t SMALL_BLOCK_MAX_SIZE = 1 * 1024 * 1024;		 //!< 1 MiB (含) / Up to 1 MiB
	static constexpr std::size_t MEDIUM_BLOCK_MAX_SIZE = 512 * 1024 * 1024;		 //!< 512 MiB (含) / Up to 512 MiB
	static constexpr std::size_t HUGE_BLOCK_THRESHOLD = 1 * 1024 * 1024 * 1024;	 //!< 1 GiB (含) / Up to 1 GiB

	/*------------------------------------------------------------------*\
    |  1. SmallMemoryManager — 64 桶 + TLS + 全局 ABA-safe 栈             |
    \*------------------------------------------------------------------*/
	struct alignas( CACHE_LINE_SIZE ) SmallMemoryManager
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
		struct alignas( 16 ) PointerTag	 // [FIX] 16 B 对齐，保证单指令 CAS
		{
			SmallMemoryHeader* pointer;	 //!< 指向内存块的指针 / Pointer to memory block
			std::uint64_t	   tag;		 //!< 标记 / Tag
		};

		struct alignas( CACHE_LINE_SIZE ) GlobalBucket
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
		void* allocate( std::size_t bytes );
		void  deallocate( SmallMemoryHeader* header );
		void  flush_thread_local_cache();
		void  release_resources();
	};

	// ============================ 中内存管理器 ============================
	struct MediumMemoryManager
	{
		static constexpr std::size_t MIN_UNIT = 1 << 20;		//!< 1 MiB / Minimum unit size
		static constexpr int		 LEVEL_COUNT = 10;			//!< 1 MiB–512 MiB / Number of levels
		static constexpr std::size_t CHUNK_SIZE = 64ull << 20;	// 64 MiB per chunk / Chunk size for OS allocation

		struct alignas( 16 ) PointerTag
		{
			MediumMemoryHeader* pointer;
			std::uint64_t		tag;
		};

		struct alignas( CACHE_LINE_SIZE ) FreeList
		{
			std::atomic<PointerTag> head;
			std::atomic_size_t		count { 0 };
		};

		struct alignas( 64 ) MergeRequest
		{
			MediumMemoryHeader* block;
			int					order;
		};

		static constexpr size_t MERGE_QUEUE_SIZE = 128;
		struct alignas( 64 ) MergeQueue
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

		void* allocate( std::size_t bytes );
		void  deallocate( MediumMemoryHeader* header );
		void  release_resources();

	private:
		static int order_from_size( std::size_t sz )
		{
			std::size_t need = sz + sizeof( MediumMemoryHeader );
			if ( need < MIN_UNIT )
				need = MIN_UNIT;

			int			order = 0;
			std::size_t cap = MIN_UNIT;
			while ( cap < need && order < LEVEL_COUNT - 1 )
			{
				cap <<= 1;
				++order;
			}
			return order;
		}
		static std::size_t size_from_order( int order )
		{
			return MIN_UNIT << order;
		};
		void				push_block( MediumMemoryHeader* header, int order );
		MediumMemoryHeader* pop_block( int order );
		void				process_merge_queue();
		bool				try_remove_from_freelist( MediumMemoryHeader* header, int order );
		void				try_merge_buddy( MediumMemoryHeader* block, int order );
		MediumMemoryHeader* split_to_order( MediumMemoryHeader* block, int from_order, int to_order );
		MediumMemoryHeader* request_new_chunk( int min_order );
	};

	/*------------------------------------------------------------------*/
	struct LargeMemoryManager
	{
		std::mutex						tracking_mutex;
		std::vector<LargeMemoryHeader*> active_blocks;

		void* allocate( std::size_t bytes );
		void  deallocate( LargeMemoryHeader* header );
		void  release_resources();
	};

	/*------------------------------------------------------------------*/
	struct HugeMemoryManager
	{
		std::mutex								   tracking_mutex;
		std::vector<std::pair<void*, std::size_t>> active_blocks;

		void* allocate( std::size_t bytes );
		void  deallocate( HugeMemoryHeader* header );
		void  release_resources();
	};

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

	void* allocate( std::size_t bytes, std::size_t alignment = MIN_ALIGNMENT, const char* source_file = nullptr, std::uint32_t source_line = 0, bool nothrow = false );
	void  deallocate( void* pointer );
	void  flush_current_thread_cache();
};


/* ============================================================
 *  内部小工具 / Internal Utilities
 * ============================================================ */
constexpr std::size_t MAX_ALLOWED_ALIGNMENT = 64 * 1024;  //!< 最大允许对齐值 / Maximum allowed alignment (64 KiB)

constexpr inline bool is_power_of_two( std::size_t v ) noexcept
{
	return v && !( v & ( v - 1 ) );	 //!< 判断是否为 2 的幂 / Check if it's a power of two
}

inline std::uintptr_t align_up( std::uintptr_t v, std::size_t alignment ) noexcept
{
	return ( v + alignment - 1 ) & ~( alignment - 1 );	//!< 向上对齐 / Align to the specified boundary
}


#endif	// MEMORY_POOL_HPP
