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
 * 2. 中内存 (1 MiB–512 MiB) – 无锁共享空闲链表 + 合并 / 切分
 * 3. 大内存 (512 MiB–1 GiB) – 直接操作系统分配并跟踪
 * 4. 超大内存 (≥1 GiB) – 单块映射，释放即返还操作系统
 */

// ============================ 前置结构体声明 ============================
struct SmallMemoryHeader;
struct MediumMemoryHeader;
struct MediumMemoryFooter;
struct LargeMemoryHeader;
struct HugeMemoryHeader;

// ============================ 对齐常量 ============================
static constexpr std::size_t CACHE_LINE_SIZE = 64;
static constexpr std::size_t DEFAULT_ALIGNMENT = 16;
static constexpr std::size_t MIN_ALIGNMENT = 8;

// ====================== 128-bit CAS 支持探测 ======================
#if ( defined( __x86_64__ ) || defined( _M_X64 ) || defined( __aarch64__ ) ) && defined( __cpp_lib_atomic_ref )
// GCC/Clang/MSVC 在 x86_64 & AArch64 下均支持 16-byte CopyAndSwap
#define SUPPORT_128BIT_CAS 1
#else
#define SUPPORT_128BIT_CAS 0
#endif

static constexpr std::uint64_t ALIGN_SENTINEL = 0xDEADBEEFCAFEBABEull;
using AlignHeader = struct
{
	std::uint64_t tag;	 // == ALIGN_SENTINEL
	void*		  raw;	 // 原始块首地址
	std::size_t	  size;	 // os_memory::allocate_memory() 的总字节数
};
static constexpr std::size_t ALIGN_HEADER_BYTES = sizeof( AlignHeader );

// ============================ MemoryPool 主类 ============================
class MemoryPool
{
private:
	// ------------------ 层级尺寸阈值 ------------------
	static constexpr std::size_t SMALL_BLOCK_MAX_SIZE = 1 * 1024 * 1024;		 // 1 MiB (含)
	static constexpr std::size_t MEDIUM_BLOCK_MAX_SIZE = 512 * 1024 * 1024;		 // 512 MiB (含)
	static constexpr std::size_t HUGE_BLOCK_THRESHOLD = 1 * 1024 * 1024 * 1024;	 // 1 GiB (含)

	/*------------------------------------------------------------------*\
    |  1. SmallMemoryManager — 64 桶 + TLS + 全局 ABA-safe 栈             |
    \*------------------------------------------------------------------*/
	struct alignas( CACHE_LINE_SIZE ) SmallMemoryManager
	{
		static constexpr std::size_t BUCKET_COUNT = 64;

		
		/* ==========================================================
         * 64-桶尺寸表 (前 32 线性 + 后 32 几何级数，最后落 1 MiB)
         * ========================================================== */
		static constexpr std::array<std::size_t, BUCKET_COUNT> BUCKET_SIZES = {
			8,	   16,	  24,	 32,	40,	   48,	  56,	  64,	  72,	  80,	  88,	  96,	  104,	  112,	  120,	  128,	   //
			136,   144,	  152,	 160,	168,   176,	  184,	  192,	  200,	  208,	  216,	  224,	  232,	  240,	  248,	  256,	   //
			336,   432,	  560,	 728,	944,   1224,  1584,	  2048,	  2656,	  3448,	  4472,	  5800,	  7520,	  9744,	  12640,  16384,   //
			21248, 27560, 35736, 46344, 60104, 77936, 101072, 131072, 169984, 220440, 285872, 370728, 480776, 623488, 808568, 1048576  //
		};

		// -------- Thread-local 缓存 --------
		struct ThreadLocalCache
		{
			SmallMemoryHeader* buckets[ BUCKET_COUNT ] = { nullptr };
			std::size_t		   deallocation_counter = 0;
		};
		static thread_local ThreadLocalCache thread_local_cache;

		// -------- 全局桶头 (ABA-safe) --------
		struct alignas( 16 ) PointerTag	 // [FIX] 16 B 对齐，保证单指令 CAS
		{
			SmallMemoryHeader* pointer;
			std::uint64_t	   tag;
		};

		struct alignas( CACHE_LINE_SIZE ) GlobalBucket
		{
#if SUPPORT_128BIT_CAS
			std::atomic<PointerTag> head;
#else
			std::atomic<SmallMemoryHeader*> head;
			std::mutex						mutex;
#endif
		};
		std::array<GlobalBucket, BUCKET_COUNT> global_buckets;

		// -------- 已分配 Chunk 跟踪 --------
		std::mutex								   chunk_mutex;
		std::vector<std::pair<void*, std::size_t>> allocated_chunks;

		// ======================== 桶映射函数（保持外部接口名） ========================
		[[nodiscard]] static std::size_t calculate_bucket_index( std::size_t bytes )
		{
			std::size_t low = 0, high = BUCKET_COUNT - 1;
			while ( low < high )
			{
				const std::size_t mid = ( low + high ) >> 1;
				if ( bytes <= BUCKET_SIZES[ mid ] )
					high = mid;
				else
					low = mid + 1;
			}
			return low;
		}

		// ----------------------- 核心接口 -----------------------
		void* allocate( std::size_t bytes );
		void  deallocate( SmallMemoryHeader* header );
		void  flush_thread_local_cache();
		void  release_resources();
	};

	/*------------------------------------------------------------------*\
    |  2. MediumMemoryManager — 64 MiB Chunk + 无锁空闲链表 + 合并         |
    \*------------------------------------------------------------------*/
	struct MediumMemoryManager
	{
		struct alignas( 16 ) PointerTag
		{
			MediumMemoryHeader* pointer;
			std::uint64_t		tag;
		};

		static constexpr int BIN_COUNT = 10;  // 1MiB, 2MiB, 4MiB, ..., 512MiB

		struct Bin
		{
#if SUPPORT_128BIT_CAS
			std::atomic<PointerTag> head;
#else
			std::atomic<MediumMemoryHeader*> head;
			std::mutex						 mutex;
#endif
			std::atomic_size_t count { 0 };
		};

		std::array<Bin, BIN_COUNT> bins;

		// 其他成员保持不变
		std::mutex								   chunk_mutex;
		std::vector<std::pair<void*, std::size_t>> allocated_chunks;

		// 新增辅助函数
		static constexpr std::array<std::size_t, BIN_COUNT> BIN_SIZES = {
			1 << 20,	// 1 MiB
			2 << 20,	// 2 MiB
			4 << 20,	// 4 MiB
			8 << 20,	// 8 MiB
			16 << 20,	// 16 MiB
			32 << 20,	// 32 MiB
			64 << 20,	// 64 MiB
			128 << 20,	// 128 MiB
			256 << 20,	// 256 MiB
			512 << 20	// 512 MiB
		};

		int get_bin_index( std::size_t size )
		{
			for ( int i = 0; i < BIN_COUNT - 1; ++i )
			{
				if ( size <= BIN_SIZES[ i ] )
					return i;
			}
			return BIN_COUNT - 1;
		}

		// 核心接口
		void* allocate( std::size_t bytes );
		void  deallocate( MediumMemoryHeader* header );
		void  release_resources();
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
	SmallMemoryManager	small_manager;
	MediumMemoryManager medium_manager;
	LargeMemoryManager	large_manager;
	HugeMemoryManager	huge_manager;

	std::atomic<bool>		 is_destructing { false };
	static std::atomic<bool> construction_warning_shown;

public:
	MemoryPool();
	~MemoryPool();

	void* allocate( std::size_t bytes, std::size_t alignment = MIN_ALIGNMENT, const char* source_file = nullptr, std::uint32_t source_line = 0, bool nothrow = false );
	void deallocate( void* pointer );
	void flush_current_thread_cache();
};

// ============================ Header 结构 ============================

/* -------------- 小块头 -------------- */
struct alignas( DEFAULT_ALIGNMENT ) SmallMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x534D4853; // 'SMHS'
	std::uint32_t		magic;
	std::uint32_t		bucket_index;
	std::uint32_t		block_size;
	std::atomic<bool>	is_free;
	SmallMemoryHeader*	next;

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( SmallMemoryHeader );
	}
};

/* -------------- 中块头 / 脚 -------------- */
struct alignas( DEFAULT_ALIGNMENT ) MediumMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x4D4D4853; // 'MMHS'
	std::uint32_t		magic;
	std::size_t			block_size;
	std::atomic<bool>	is_free;
	MediumMemoryHeader*	next;

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( MediumMemoryHeader );
	}
};

struct alignas( DEFAULT_ALIGNMENT ) MediumMemoryFooter
{
	std::size_t block_size;
};

/* -------------- 大 / 超大 块头 -------------- */
struct alignas( DEFAULT_ALIGNMENT ) LargeMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x4C4D4853;	// 'LMHS'
	std::uint32_t		magic;
	std::size_t			block_size;

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( LargeMemoryHeader );
	}
};

struct alignas( DEFAULT_ALIGNMENT ) HugeMemoryHeader
{
	static constexpr std::uint32_t MAGIC = 0x484D4853;	// 'HMHS'
	std::uint32_t		magic;
	std::size_t			block_size;

	void* data()
	{
		return reinterpret_cast<char*>( this ) + sizeof( HugeMemoryHeader );
	}
};

/* ============================================================
 *  内部小工具
 * ============================================================ */
constexpr std::size_t MAX_ALLOWED_ALIGNMENT = 64 * 1024;  // 64 KiB
constexpr inline bool is_power_of_two( std::size_t v ) noexcept
{
	return v && !( v & ( v - 1 ) );
}

inline std::uintptr_t align_up( std::uintptr_t v, std::size_t alignment ) noexcept
{
	return ( v + alignment - 1 ) & ~( alignment - 1 );
}


#endif	// MEMORY_POOL_HPP