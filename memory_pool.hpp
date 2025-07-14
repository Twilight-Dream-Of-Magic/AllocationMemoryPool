/**
* @file memory_pool.hpp
* @brief 轻量级用户态内存池实现 / Lightweight user-mode memory pool
* 
* @details 
* 1. 按「小 / 中 / 大」三段式桶规则映射 32 个桶；
* 2. 线程本地缓存 + 全局空闲链表 + 定时压缩(归还完整 Chunk 至 OS)。
*
* 代码风格说明 / Style Notes
* ---------------------------------------------------------------------------
* 1. 彻底避免缩写：所有标识符均使用完整单词 (block, index, pointer...)；
* 2. 中英文注释并存，方便自我审阅与团队协作；
*/
#pragma once
#ifndef MEMORY_POOL_HPP
#define MEMORY_POOL_HPP

#include "os_memory.hpp"

#include <cstdint>
#include <cstddef>
#include <cstdlib>

#include <iostream>
#include <mutex>
#include <vector>
#include <utility>
#include <chrono>
#include <algorithm>
#include <atomic>

struct BlockHeader
{
	size_t		 size = 0;			 //!< 数据区大小 / data size
	bool		 is_useable = true;	 //!< 是否空闲 / is free
	BlockHeader* next_in_list = nullptr;
	BlockHeader* previous_in_list = nullptr;

	/** @return 指向数据区的指针 / pointer to user data */
	void* data()
	{
		return this + 1;
	}
};

struct MemoryChunk
{
	void*  base_address = nullptr;	//!< Chunk 起始地址 / chunk base
	size_t size = 0;				//!< Chunk 大小 / chunk size
};

class MemoryPool
{
private:
	/*--------------------------- 常量 / Constants ---------------------------*/
	static constexpr size_t				  DEFAULT_CHUNK_SIZE = 64 * 1024 * 1024;  // 64 MB
	static constexpr size_t				  MINIMUM_BLOCK_SIZE = 32;				  // 最小分配单位
	static constexpr size_t				  TOTAL_BUCKET_COUNT = 32;				  // 桶数量
	static constexpr std::chrono::seconds COMPACT_INTERVAL { 30 };				  // 压缩间隔

	/*--------------------------- 线程缓存 / TLS ----------------------------*/
	struct ThreadLocalCache
	{
		BlockHeader* buckets[ TOTAL_BUCKET_COUNT ] = { nullptr };
	};
	static thread_local ThreadLocalCache thread_local_cache;

	/*--------------------------- 全局状态 / Global --------------------------*/
	std::vector<MemoryChunk>			  chunk_list;				   //!< 已分配的所有 Chunk / All allocated chunks
	BlockHeader*						  global_free_list = nullptr;  //!< 全局空闲链表 / Global free list
	std::timed_mutex					  global_mutex;				   //!< 全局互斥 / Global mutex
	std::chrono::steady_clock::time_point last_compact_time;
	// 只提示一次
    static std::atomic<bool> warning_shown;

	/*--------------------------- 工具函数 / Helpers -------------------------*/
	/** 计算桶下标 / map size → bucket index (small / medium / large) */
	static size_t calculate_bucket_index( size_t size )
	{
		if ( size <= 256 )	// Small (16 B 步长)
		{
			return ( size + 15 ) / 16 - 1;
		}
		else if ( size <= 4096 )  // Medium (256 B 步长)
		{
			return 16 + ( size + 255 ) / 256 - 1;
		}
		else  // Large (≥ 4 KB)
		{
			return TOTAL_BUCKET_COUNT - 1;
		}
	}

	/** 判断两个块是否物理相邻 / physical adjacency */
	static bool are_blocks_adjacent( BlockHeader* first, BlockHeader* second )
	{
		if ( !first || !second )
			return false;
		char* first_block_end = reinterpret_cast<char*>( first ) + sizeof( BlockHeader ) + first->size;
		return first_block_end == reinterpret_cast<char*>( second );
	}

	/** 是否需要执行压缩 / should compact now? */
	bool need_compaction() const
	{
		return ( std::chrono::steady_clock::now() - last_compact_time > COMPACT_INTERVAL );
	}

	/** 从全局空闲链表中分配 / allocate from global list */
	BlockHeader* allocate_from_global( size_t required_size );

	/** 合并全局空闲链表内相邻块 / coalesce physically adjacent blocks */
	void coalesce_global_free_list();

	/** 检测 Chunk 是否完全空闲 / is whole chunk free? */
	bool is_chunk_fully_free( const MemoryChunk& chunk ) const;

	/** 将完整空闲 Chunk 归还 OS / return free chunks to OS */
	void perform_compaction();

	/** 回收线程缓存至全局，并尝试合并 / flush thread cache */
	void flush_thread_local_cache();

public:
	MemoryPool() : last_compact_time( std::chrono::steady_clock::now() )
	{
		if ( warning_shown.load() )
		{
			// 黄色：中文提示
			std::cout << "\033[33m[MemoryPool 警告] 直接使用 MemoryPool 可能导致跟踪遗漏或重复，"
						 "请改用 PoolAllocator ！\033[0m\n";
			// 绿色：英文提示
			std::cout << "\033[32m[MemoryPool Warning] Direct use of MemoryPool may cause tracking "
						 "misses or duplicates. Please use PoolAllocator instead!\033[0m\n";

			warning_shown.exchange( true );
		}
	}

	/*---------------------------- 外部接口 / API ----------------------------*/
	void* allocate( size_t size, const char* file = nullptr, uint32_t line = 0, bool nothrow = false );

	void deallocate( void* pointer );

	~MemoryPool();
};

/*========================= 私有成员实现 / Private =========================*/
BlockHeader* MemoryPool::allocate_from_global( size_t required_size )
{
	// 尝试获取全局互斥锁，防止多个线程同时修改全局空闲链表
	// Try to lock the global mutex to safely modify the global free list.
	if ( !global_mutex.try_lock_for( std::chrono::seconds( 5 ) ) )
		return nullptr;

	// 在全局空闲链表中查找“最佳匹配”块，即满足所需大小且尽可能小的块
	// Iterate global free list to find the best-fit block that is free and of sufficient size.
	BlockHeader* best_fit_block = nullptr;
	for ( BlockHeader* block = global_free_list; block; block = block->next_in_list )
	{
		// 当块可用且其大小大于或等于请求大小时，考虑该块
		// If the block is free and its size is greater than or equal to the required size:
		if ( block->is_useable && block->size >= required_size && ( !best_fit_block || block->size < best_fit_block->size ) )
		{
			best_fit_block = block;
			// 如果块大小正好匹配，则无需搜索更多
			// If perfect match found, break early.
			if ( block->size == required_size )
				break;	// 完美匹配 / Perfect fit found!
		}
	}

	// 若找到合适的块，则将其从全局空闲链表中移除并返回
	// If a suitable block is found, remove it from the global free list and mark it as in-use.
	if ( best_fit_block ) /* ---- 从空闲链表取出 ---- */
	{
		// 更新邻接指针，断开当前块与全局链表的连接
		// Adjust linked list pointers to detach the block.
		if ( best_fit_block->previous_in_list )
			best_fit_block->previous_in_list->next_in_list = best_fit_block->next_in_list;
		if ( best_fit_block->next_in_list )
			best_fit_block->next_in_list->previous_in_list = best_fit_block->previous_in_list;
		if ( best_fit_block == global_free_list )
			global_free_list = best_fit_block->next_in_list;

		// 清空当前块的链表指针，并标记为已使用
		// Reset the block's pointers and mark it as in-use.
		best_fit_block->previous_in_list = best_fit_block->next_in_list = nullptr;
		best_fit_block->is_useable = false;
		global_mutex.unlock();
		return best_fit_block;
	}

	/* ---- 未能在全局空闲链表中找到合适块，必须向 OS 请求新的 Chunk ---- */
	// Calculate the total memory needed (including BlockHeader overhead)
	size_t needed_total_size = required_size + sizeof( BlockHeader );
	// 确保 Chunk 至少为默认大小
	// Ensure the chunk allocated is at least the default chunk size.
	size_t chunk_size = std::max( needed_total_size, DEFAULT_CHUNK_SIZE );

	// 向操作系统申请内存块
	// Request a new memory chunk from the OS.
	void* raw_memory = os_memory::allocate_memory( chunk_size );
	if ( !raw_memory )
	{
		std::cerr << "OS allocation failed (" << chunk_size << " bytes)\n";
		global_mutex.unlock();
		return nullptr;
	}

	// 将新申请的 Chunk 记录到列表中，用于后续管理和释放
	// Record newly allocated Chunks in a list for subsequent management and de-allocation.
	chunk_list.push_back( { raw_memory, chunk_size } );

	// 将整个 Chunk 切分为一个大块，这个大块初始时为未分配状态，然后分割出所需的块部分
	// Partition the new chunk into a block and possibly a remaining free block.
	auto* new_block = static_cast<BlockHeader*>( raw_memory );
	new_block->size = chunk_size - sizeof( BlockHeader );
	new_block->is_useable = false;	// 当前直接分配所需内存，标记为已使用
	new_block->next_in_list = nullptr;
	new_block->previous_in_list = nullptr;

	/* ---- 分割剩余内存，将其挂回全局空闲链表 ---- */
	// 计算剩余内存: 总内存减去已分配所需内存
	// Calculate remaining size after allocating the required block.
	size_t remaining_size = new_block->size - required_size;
	// 如果剩余内存足够容纳一个新的 BlockHeader 和最小可分配内存，则将其分割出来
	// If the remaining space is enough to store a new block (header + minimum block size), partition it.
	if ( remaining_size >= sizeof( BlockHeader ) + MINIMUM_BLOCK_SIZE )
	{
		// 定位用户区的结束，并计算自由块的起始地址
		// Determine the start address of the free block from the allocated chunk.
		char* user_area = reinterpret_cast<char*>( new_block ) + sizeof( BlockHeader );
		auto* free_block = reinterpret_cast<BlockHeader*>( user_area + required_size );
		free_block->size = remaining_size - sizeof( BlockHeader );
		free_block->is_useable = true;
		// 将新的空闲块挂接到全局空闲链表前端
		// Insert the new free block at the beginning of the global free list.
		free_block->next_in_list = global_free_list;
		free_block->previous_in_list = nullptr;
		if ( global_free_list )
			global_free_list->previous_in_list = free_block;
		global_free_list = free_block;

		// 调整已分配块的大小为正好符合请求的大小
		// Adjust the allocated block's size to exactly match the requested size.
		new_block->size = required_size;
	}

	// 解锁全局互斥锁，结束内存分配操作，并返回新分配的块
	// Unlock the global mutex and return the allocated block.
	global_mutex.unlock();
	return new_block;
}

void MemoryPool::coalesce_global_free_list()
{
	if ( !global_free_list )
		return;

	/* 1. 按地址排序 / Step 1: Sort by address */
	std::vector<BlockHeader*> sorted_blocks;
	for ( BlockHeader* block = global_free_list; block; block = block->next_in_list )
		sorted_blocks.push_back( block );

	std::sort( sorted_blocks.begin(), sorted_blocks.end(), []( BlockHeader* a, BlockHeader* b ) { return a < b; } );

	/* 2. 重建链表 / Step 2: Rebuild the linked list */
	global_free_list = nullptr;
	BlockHeader* previous_block = nullptr;
	for ( BlockHeader* block : sorted_blocks )
	{
		block->previous_in_list = previous_block;
		block->next_in_list = nullptr;
		if ( previous_block )
			previous_block->next_in_list = block;
		else
			global_free_list = block;
		previous_block = block;
	}

	/* 3. 物理合并相邻块 / Step 3: Coalesce physically adjacent blocks */
	for ( BlockHeader* current = global_free_list; current && current->next_in_list; )
	{
		BlockHeader* next_block = current->next_in_list;
		if ( are_blocks_adjacent( current, next_block ) )
		{
			current->size += sizeof( BlockHeader ) + next_block->size;
			current->next_in_list = next_block->next_in_list;
			if ( next_block->next_in_list )
				next_block->next_in_list->previous_in_list = current;
		}
		else
		{
			current = next_block;
		}
	}
}

bool MemoryPool::is_chunk_fully_free( const MemoryChunk& chunk ) const
{
	for ( BlockHeader* block = global_free_list; block; block = block->next_in_list )
	{
		if ( reinterpret_cast<void*>( block ) == chunk.base_address && block->size + sizeof( BlockHeader ) == chunk.size )
			return true;
	}
	return false;
}

void MemoryPool::perform_compaction()
{
	if ( !global_mutex.try_lock_for( std::chrono::seconds( 10 ) ) )
		return;

	size_t total_released_bytes = 0;
	for ( auto iterator = chunk_list.begin(); iterator != chunk_list.end(); )
	{
		if ( is_chunk_fully_free( *iterator ) )
		{
			os_memory::deallocate_memory( iterator->base_address, iterator->size );
			total_released_bytes += iterator->size;
			iterator = chunk_list.erase( iterator );
		}
		else
			++iterator;
	}

	if ( total_released_bytes )
	{
		std::cout << "[MemoryPool] Compacted " << total_released_bytes / ( 1024 * 1024 ) << " MB\n";
	}

	last_compact_time = std::chrono::steady_clock::now();
	global_mutex.unlock();
}

void MemoryPool::flush_thread_local_cache()
{
	if ( !global_mutex.try_lock_for( std::chrono::seconds( 5 ) ) )
		return;

	for ( size_t index = 0; index < TOTAL_BUCKET_COUNT; ++index )
	{
		BlockHeader* block = thread_local_cache.buckets[ index ];
		while ( block )
		{
			BlockHeader* next_block = block->next_in_list;

			block->next_in_list = global_free_list;
			block->previous_in_list = nullptr;
			if ( global_free_list )
				global_free_list->previous_in_list = block;
			global_free_list = block;

			block = next_block;
		}
		thread_local_cache.buckets[ index ] = nullptr;
	}

	coalesce_global_free_list();
	global_mutex.unlock();
}

/*=========================== 公共接口实现 / API ===========================*/
void* MemoryPool::allocate( size_t size, const char* file, uint32_t line, bool nothrow )
{
	if ( !size )
		return nullptr;
	size = std::max( size, MINIMUM_BLOCK_SIZE );

	size_t bucket_index = calculate_bucket_index( size );

	/* ---- 尝试线程缓存，在各自线程的局部缓存中寻找预先存储空闲块 ---- */
	// Try to allocate from the thread-local cache.
	if ( BlockHeader* cached_block = thread_local_cache.buckets[ bucket_index ] )
	{
		thread_local_cache.buckets[ bucket_index ] = cached_block->next_in_list;
		cached_block->is_useable = false;
		void* user_pointer = cached_block->data();
		return user_pointer;
	}

	/* ---- 未能从线程缓存中获取，退回全局内存池搜索 ---- */
	BlockHeader* allocated_block = allocate_from_global( size );
	if ( !allocated_block )
	{
		if ( !nothrow )
			throw std::bad_alloc();
		std::cerr << "Allocation failed (" << size << " bytes)" << ( file ? std::string { " @ " } + file + ":" + std::to_string( line ) : "" ) << '\n';
		return nullptr;
	}

	void* user_pointer = allocated_block->data();
	return user_pointer;
}

void MemoryPool::deallocate( void* pointer )
{
	if ( !pointer )
		return;

	BlockHeader* header = reinterpret_cast<BlockHeader*>( static_cast<char*>( pointer ) - sizeof( BlockHeader ) );

	header->is_useable = true;

	size_t bucket_index = calculate_bucket_index( header->size );
	header->next_in_list = thread_local_cache.buckets[ bucket_index ];
	thread_local_cache.buckets[ bucket_index ] = header;

	/* ---- 周期性回收线程局部缓存并触发压缩操作 ---- */
	static thread_local size_t local_deallocation_counter = 0;
	if ( ++local_deallocation_counter >= 128 )
	{
		local_deallocation_counter = 0;
		flush_thread_local_cache();
		if ( need_compaction() )
			perform_compaction();
	}
}

MemoryPool::~MemoryPool()
{
	flush_thread_local_cache();
	for ( MemoryChunk& chunk : chunk_list )
		os_memory::deallocate_memory( chunk.base_address, chunk.size );
	chunk_list.clear();
}

std::atomic<bool> MemoryPool::warning_shown{ true };

/*==================== 线程局部缓存实例化 / TLS instantiation ===============*/
thread_local MemoryPool::ThreadLocalCache MemoryPool::thread_local_cache;

#endif	// MEMORY_POOL_HPP