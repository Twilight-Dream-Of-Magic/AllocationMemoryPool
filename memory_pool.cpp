#include "memory_pool.hpp"

// ============================ 静态成员定义 ============================
std::atomic<bool> MemoryPool::construction_warning_shown { false };

/* -------- TLS 实例 -------- */
thread_local MemoryPool::SmallMemoryManager::ThreadLocalCache MemoryPool::SmallMemoryManager::thread_local_cache;

/* =====================================================================
 *  SmallMemoryManager — 实现
 * ===================================================================== */

/* -------- allocate -------- */
void* MemoryPool::SmallMemoryManager::allocate( std::size_t bytes )
{
	const std::size_t index = calculate_bucket_index( bytes );
	const std::size_t bucket_bytes = BUCKET_SIZES[ index ];

	/* 1) 线程本地 */
	if ( auto* header = thread_local_cache.buckets[ index ] )
	{
		thread_local_cache.buckets[ index ] = header->next;
		header->is_free.store( false, std::memory_order_relaxed );
		header->magic = SmallMemoryHeader::MAGIC;
		return header->data();
	}

	/* 2) 全局 ABA-safe 栈 */
	GlobalBucket& bucket = global_buckets[ index ];

#if SUPPORT_128BIT_CAS
	typename PointerTag old_head = bucket.head.load( std::memory_order_acquire );

	while ( old_head.pointer )
	{
		SmallMemoryHeader*	candidate = old_head.pointer;
		typename PointerTag new_head = { candidate->next, old_head.tag + 1 };

		if ( bucket.head.compare_exchange_weak( old_head, new_head, std::memory_order_acq_rel, std::memory_order_acquire ) )
		{
			candidate->is_free.store( false, std::memory_order_relaxed );
			candidate->magic = SmallMemoryHeader::MAGIC;
			return candidate->data();
		}
	}
#else
	{
		std::lock_guard<std::mutex> lock( bucket.mutex );
		if ( auto* header = bucket.head.load( std::memory_order_relaxed ) )
		{
			bucket.head.store( header->next, std::memory_order_relaxed );
			header->is_free.store( false, std::memory_order_relaxed );
			header->magic = SmallMemoryHeader::MAGIC;
			return header->data();
		}
	}
#endif

	/* 3) 向 OS 申请新 Chunk */
	const std::size_t block_bytes = sizeof( SmallMemoryHeader ) + bucket_bytes;
	const std::size_t chunk_size = std::max<std::size_t>( 1 * 1024 * 1024, block_bytes * 128 );

	void* chunk_mem;
	{
		std::lock_guard<std::mutex> g( chunk_mutex );
		chunk_mem = os_memory::allocate_memory( chunk_size );
		if ( !chunk_mem )
			throw std::bad_alloc();
		allocated_chunks.emplace_back( chunk_mem, chunk_size );
	}

	/* 切分 chunk */
	const std::size_t block_count = chunk_size / block_bytes;
	if ( block_count == 0 )
	{
		os_memory::deallocate_memory( chunk_mem, chunk_size );
		throw std::bad_alloc();
	}

	char*			   cursor = static_cast<char*>( chunk_mem );
	SmallMemoryHeader* first_block = nullptr;
	SmallMemoryHeader* prev_block = nullptr;

	for ( std::size_t i = 0; i < block_count; ++i, cursor += block_bytes )
	{
		auto* block = reinterpret_cast<SmallMemoryHeader*>( cursor );
		block->bucket_index = static_cast<std::uint32_t>( index );
		block->block_size = static_cast<std::uint32_t>( bucket_bytes );
		block->is_free.store( true, std::memory_order_relaxed );
		block->magic = SmallMemoryHeader::MAGIC;
		block->next = nullptr;

		if ( !first_block )
			first_block = block;
		if ( prev_block )
			prev_block->next = block;
		prev_block = block;
	}

	/* 4) 把多余块批量推入全局栈 */
	if ( block_count > 1 )
	{
#if SUPPORT_128BIT_CAS
		typename PointerTag head_snapshot = bucket.head.load( std::memory_order_relaxed );
		prev_block->next = head_snapshot.pointer;  // 整个链表连接到原全局链表

		typename PointerTag new_snapshot = { first_block->next, head_snapshot.tag + 1 };

		while ( !bucket.head.compare_exchange_weak( head_snapshot, new_snapshot, std::memory_order_release, std::memory_order_relaxed ) )
		{
			prev_block->next = head_snapshot.pointer;
			new_snapshot.tag = head_snapshot.tag + 1;
		}
#else
		std::lock_guard<std::mutex> lock( bucket.mutex );
		prev_block->next = bucket.head.load( std::memory_order_relaxed );
		bucket.head.store( first_block, std::memory_order_relaxed );
#endif
	}

	/* 返回首块 */
	if ( first_block != nullptr )
	{
		first_block->is_free.store( false, std::memory_order_relaxed );
		first_block->magic = SmallMemoryHeader::MAGIC;
		return first_block->data();
	}
	else
	{
		throw std::runtime_error( "first_block is null during allocation." );
	}
}

/* -------- deallocate -------- */
void MemoryPool::SmallMemoryManager::deallocate( SmallMemoryHeader* header )
{
	bool expected = false;
	if ( !header->is_free.compare_exchange_strong( expected, true, std::memory_order_release, std::memory_order_relaxed ) )
		return;	 // double free

	if ( header->magic != SmallMemoryHeader::MAGIC )
	{
		std::cerr << "[Small] invalid magic during deallocation\n";
		return;
	}
	header->magic = 0;	// 防止重复使用

	const std::size_t index = header->bucket_index;

	header->next = thread_local_cache.buckets[ index ];
	thread_local_cache.buckets[ index ] = header;

	if ( ++thread_local_cache.deallocation_counter >= 256 )	 // [FIX] 阈值放宽
		flush_thread_local_cache();
}

/* -------- flush TLS -------- */
void MemoryPool::SmallMemoryManager::flush_thread_local_cache()
{
	for ( std::size_t i = 0; i < BUCKET_COUNT; ++i )
	{
		SmallMemoryHeader* local_head = thread_local_cache.buckets[ i ];
		if ( !local_head )
			continue;

		GlobalBucket& bucket = global_buckets[ i ];

		/* 找尾结点一次即可，循环外保证正确性 */
		SmallMemoryHeader* tail = local_head;
		while ( tail->next )
			tail = tail->next;

#if SUPPORT_128BIT_CAS
		typename PointerTag head_snapshot = bucket.head.load( std::memory_order_relaxed );

		do
		{
			tail->next = head_snapshot.pointer;
		} while ( !bucket.head.compare_exchange_weak( head_snapshot, { local_head, head_snapshot.tag + 1 }, std::memory_order_release, std::memory_order_relaxed ) );
#else
		{
			std::lock_guard<std::mutex> lock( bucket.mutex );
			tail->next = bucket.head.load( std::memory_order_relaxed );
			bucket.head.store( local_head, std::memory_order_relaxed );
		}
#endif
		thread_local_cache.buckets[ i ] = nullptr;
	}
	thread_local_cache.deallocation_counter = 0;
}

/* -------- release_resources -------- */
void MemoryPool::SmallMemoryManager::release_resources()
{
	flush_thread_local_cache();

	std::lock_guard<std::mutex> g( chunk_mutex );
	for ( auto& [ pointer, size ] : allocated_chunks )
		os_memory::deallocate_memory( pointer, size );
	allocated_chunks.clear();

	for ( auto& bucket : global_buckets )
	{
#if SUPPORT_128BIT_CAS
		bucket.head.store( { nullptr, 0 }, std::memory_order_relaxed );
#else
		bucket.head.store( nullptr, std::memory_order_relaxed );
#endif
	}
}

/* =====================================================================
 *  MediumMemoryManager — 实现
 * ===================================================================== */

void* MemoryPool::MediumMemoryManager::allocate( std::size_t bytes )
{
	const std::size_t total_request = bytes + sizeof( MediumMemoryHeader ) + sizeof( MediumMemoryFooter );
	const int		  start_bin = get_bin_index( total_request );

	// 1. 在合适的 bin 中搜索
	for ( int bin_index = start_bin; bin_index < BIN_COUNT; ++bin_index )
	{
		Bin& bin = bins[ bin_index ];

#if SUPPORT_128BIT_CAS
		// 无锁版实现
		PointerTag head = bin.head.load( std::memory_order_acquire );
		while ( head.pointer )
		{
			MediumMemoryHeader* candidate = head.pointer;
			if ( candidate->block_size >= bytes )
			{
				PointerTag new_head = { candidate->next, head.tag + 1 };
				if ( bin.head.compare_exchange_weak( head, new_head, std::memory_order_acq_rel, std::memory_order_acquire ) )
				{

					candidate->is_free.store( false, std::memory_order_relaxed );
					candidate->magic = MediumMemoryHeader::MAGIC;

					// 分割逻辑（保留原始实现）
					if ( candidate->block_size - bytes > sizeof( MediumMemoryHeader ) + sizeof( MediumMemoryFooter ) + 256 )
					{

						const std::size_t remain = candidate->block_size - bytes - sizeof( MediumMemoryHeader ) - sizeof( MediumMemoryFooter );

						char* split_byte_position = reinterpret_cast<char*>( candidate->data() ) + bytes;
						auto* new_header = reinterpret_cast<MediumMemoryHeader*>( split_byte_position );

						// 设置新块信息（保留 magic number）
						new_header->block_size = remain;
						new_header->is_free.store( true, std::memory_order_relaxed );
						new_header->magic = MediumMemoryHeader::MAGIC;
						new_header->next = nullptr;

						auto* new_footer = reinterpret_cast<MediumMemoryFooter*>( split_byte_position + sizeof( MediumMemoryHeader ) + remain );
						new_footer->block_size = remain;

						auto* cand_footer = reinterpret_cast<MediumMemoryFooter*>( split_byte_position - sizeof( MediumMemoryFooter ) );
						cand_footer->block_size = bytes;

						candidate->block_size = bytes;

						// 将剩余块放回空闲链表
						deallocate( new_header );
					}
					return candidate->data();
				}
			}
			else
			{
				head.pointer = candidate->next;
			}
		}
#else
		// 带锁版实现
		{
			std::lock_guard<std::mutex> lock( bin.mutex );
			MediumMemoryHeader*			current = bin.head.load( std::memory_order_relaxed );
			MediumMemoryHeader*			prev = nullptr;

			while ( current )
			{
				if ( current->block_size >= bytes )
				{
					// 从链表中移除
					if ( prev )
						prev->next = current->next;
					else
						bin.head.store( current->next, std::memory_order_relaxed );

					current->is_free.store( false, std::memory_order_relaxed );
					current->magic = MediumMemoryHeader::MAGIC;

					// 分割逻辑（同上）
					if ( current->block_size - bytes > sizeof( MediumMemoryHeader ) + sizeof( MediumMemoryFooter ) + 256 )
					{
						// ... 与无锁版相同的分割代码 ...
					}
					return current->data();
				}
				prev = current;
				current = current->next;
			}
		}
#endif
	}

	// 2. 无合适块则分配新 chunk（保留原始实现）
	const std::size_t chunk_size = std::max<std::size_t>( 64 * 1024 * 1024, ( total_request + ( ( 1 << 20 ) - 1 ) ) & ~( ( 1 << 20 ) - 1 ) );

	void* chunk_memory;
	{
		std::lock_guard<std::mutex> g( chunk_mutex );
		chunk_memory = os_memory::allocate_memory( chunk_size );
		if ( !chunk_memory )
			throw std::bad_alloc();
		allocated_chunks.emplace_back( chunk_memory, chunk_size );
	}

	// 初始化块（保留 magic number）
	auto* first_header = static_cast<MediumMemoryHeader*>( chunk_memory );
	first_header->block_size = bytes;
	first_header->is_free.store( false, std::memory_order_relaxed );
	first_header->magic = MediumMemoryHeader::MAGIC;
	first_header->next = nullptr;

	auto* first_footer = reinterpret_cast<MediumMemoryFooter*>( reinterpret_cast<char*>( first_header ) + sizeof( MediumMemoryHeader ) + bytes );
	first_footer->block_size = bytes;

	// 处理剩余空间（保留原始逻辑）
	const std::size_t remain_total = chunk_size - ( sizeof( MediumMemoryHeader ) + bytes + sizeof( MediumMemoryFooter ) );

	constexpr std::size_t overhead = sizeof( MediumMemoryHeader ) + sizeof( MediumMemoryFooter );

	if ( remain_total >= overhead + 256 )
	{
		const std::size_t remain_data = remain_total - overhead;
		char*			  new_hdr_ptr = reinterpret_cast<char*>( first_footer ) + sizeof( MediumMemoryFooter );
		auto*			  new_header = reinterpret_cast<MediumMemoryHeader*>( new_hdr_ptr );

		new_header->block_size = remain_data;
		new_header->is_free.store( true, std::memory_order_relaxed );
		new_header->magic = MediumMemoryHeader::MAGIC;
		new_header->next = nullptr;

		auto* new_footer = reinterpret_cast<MediumMemoryFooter*>( new_hdr_ptr + sizeof( MediumMemoryHeader ) + remain_data );
		new_footer->block_size = remain_data;

		deallocate( new_header );
	}

	return first_header->data();
}

void MemoryPool::MediumMemoryManager::deallocate( MediumMemoryHeader* header )
{
	// 1. 安全检查（保留原始实现）
	bool expected = false;
	if ( !header->is_free.compare_exchange_strong( expected, true, std::memory_order_release, std::memory_order_relaxed ) )
	{
		return;	 // double free
	}

	if ( header->magic != MediumMemoryHeader::MAGIC )
	{
		std::cerr << "[Medium] invalid magic during deallocation\n";
		return;
	}
	header->magic = 0;	// 防止重复使用

	// 2. 合并相邻块（保留原始实现）
	char* chunk_start = nullptr;
	char* chunk_end = nullptr;
	for ( auto const& ch : allocated_chunks )
	{
		char* cs = static_cast<char*>( ch.first );
		char* ce = cs + ch.second;
		if ( reinterpret_cast<char*>( header ) >= cs && reinterpret_cast<char*>( header ) < ce )
		{
			chunk_start = cs;
			chunk_end = ce;
			break;
		}
	}
	if ( !chunk_start )
	{
		std::cerr << "[Medium] header not in any chunk\n";
		return;
	}

	// 向右合并
	char* right_header_pointer = reinterpret_cast<char*>( header ) + sizeof( MediumMemoryHeader ) + header->block_size + sizeof( MediumMemoryFooter );

	if ( right_header_pointer + sizeof( MediumMemoryHeader ) <= chunk_end )
	{
		auto* right_header = reinterpret_cast<MediumMemoryHeader*>( right_header_pointer );
		if ( right_header->magic == MediumMemoryHeader::MAGIC && right_header->is_free.load( std::memory_order_acquire ) )
		{
			header->block_size += sizeof( MediumMemoryHeader ) + right_header->block_size + sizeof( MediumMemoryFooter );
			right_header->magic = 0;  // poison merged block
		}
	}

	// 向左合并
	if ( reinterpret_cast<char*>( header ) - sizeof( MediumMemoryFooter ) >= chunk_start )
	{
		auto* left_footer = reinterpret_cast<MediumMemoryFooter*>( reinterpret_cast<char*>( header ) - sizeof( MediumMemoryFooter ) );

		if ( left_footer->block_size > 0 && left_footer->block_size <= MEDIUM_BLOCK_MAX_SIZE && reinterpret_cast<char*>( header ) >= chunk_start + left_footer->block_size + sizeof( MediumMemoryHeader ) )
		{

			auto* left_header = reinterpret_cast<MediumMemoryHeader*>( reinterpret_cast<char*>( header ) - left_footer->block_size - sizeof( MediumMemoryHeader ) - sizeof( MediumMemoryFooter ) );

			if ( left_header->magic == MediumMemoryHeader::MAGIC && left_header->is_free.load( std::memory_order_acquire ) )
			{
				left_header->block_size += sizeof( MediumMemoryHeader ) + header->block_size + sizeof( MediumMemoryFooter );
				header = left_header;
				left_header->magic = 0;
			}
		}
	}

	// 更新 footer
	auto* footer = reinterpret_cast<MediumMemoryFooter*>( reinterpret_cast<char*>( header ) + sizeof( MediumMemoryHeader ) + header->block_size );
	footer->block_size = header->block_size;

	// 3. 根据大小放入合适的 bin
	const int bin_index = get_bin_index( header->block_size );
	Bin&	  bin = bins[ bin_index ];

#if SUPPORT_128BIT_CAS
	// 无锁版插入
	PointerTag head = bin.head.load( std::memory_order_relaxed );
	do
	{
		header->next = head.pointer;
		PointerTag new_head = { header, head.tag + 1 };
		if ( bin.head.compare_exchange_weak( head, new_head, std::memory_order_release, std::memory_order_relaxed ) )
		{
			break;
		}
	} while ( true );
#else
	// 带锁版插入
	{
		std::lock_guard<std::mutex> lock( bin.mutex );
		header->next = bin.head.load( std::memory_order_relaxed );
		bin.head.store( header, std::memory_order_relaxed );
	}
#endif
}

void MemoryPool::MediumMemoryManager::release_resources()
{
	// 清空所有 bin
	for ( auto& bin : bins )
	{
#if SUPPORT_128BIT_CAS
		bin.head.store( { nullptr, 0 }, std::memory_order_relaxed );
#else
		bin.head.store( nullptr, std::memory_order_relaxed );
#endif
	}

	// 释放所有 chunks（保留原始实现）
	std::lock_guard<std::mutex> g( chunk_mutex );
	for ( auto& [ pointer, size ] : allocated_chunks )
	{
		os_memory::deallocate_memory( pointer, size );
	}
	allocated_chunks.clear();
}

/* =====================================================================
 *  LargeMemoryManager — 实现
 * ===================================================================== */

void* MemoryPool::LargeMemoryManager::allocate( std::size_t bytes )
{
	const std::size_t total = sizeof( LargeMemoryHeader ) + bytes;
	void*			  memory = os_memory::allocate_memory( total );
	if ( !memory )
		throw std::bad_alloc();

	auto* header = static_cast<LargeMemoryHeader*>( memory );
	header->magic = LargeMemoryHeader::MAGIC;
	header->block_size = bytes;

	std::lock_guard<std::mutex> g( tracking_mutex );
	active_blocks.push_back( header );

	return header->data();
}

void MemoryPool::LargeMemoryManager::deallocate( LargeMemoryHeader* header )
{
	if ( header->magic != LargeMemoryHeader::MAGIC )
	{
		std::cerr << "[Large] invalid magic during deallocation\n";
		return;
	}
	header->magic = 0;

	{
		std::lock_guard<std::mutex> g( tracking_mutex );
		auto						it = std::find( active_blocks.begin(), active_blocks.end(), header );
		if ( it != active_blocks.end() )
			active_blocks.erase( it );
	}
	os_memory::deallocate_memory( header, sizeof( LargeMemoryHeader ) + header->block_size );
}

void MemoryPool::LargeMemoryManager::release_resources()
{
	std::lock_guard<std::mutex> g( tracking_mutex );
	for ( auto* header : active_blocks )
		os_memory::deallocate_memory( header, sizeof( LargeMemoryHeader ) + header->block_size );
	active_blocks.clear();
}

/* =====================================================================
 *  HugeMemoryManager — 实现
 * ===================================================================== */

void* MemoryPool::HugeMemoryManager::allocate( std::size_t bytes )
{
	const std::size_t total = sizeof( HugeMemoryHeader ) + bytes;
	void*			  memory = os_memory::allocate_memory( total );
	if ( !memory )
		throw std::bad_alloc();

	auto* header = static_cast<HugeMemoryHeader*>( memory );
	header->magic = HugeMemoryHeader::MAGIC;
	header->block_size = bytes;

	std::lock_guard<std::mutex> g( tracking_mutex );
	active_blocks.emplace_back( memory, total );

	return header->data();
}

void MemoryPool::HugeMemoryManager::deallocate( HugeMemoryHeader* header )
{
	if ( header->magic != HugeMemoryHeader::MAGIC )
	{
		std::cerr << "[Huge] invalid magic during deallocation\n";
		return;
	}
	header->magic = 0;

	{
		std::lock_guard<std::mutex> g( tracking_mutex );
		auto						it = std::find_if( active_blocks.begin(), active_blocks.end(), [ header ]( auto& rec ) { return rec.first == header; } );
		if ( it != active_blocks.end() )
		{
			os_memory::deallocate_memory( it->first, it->second );
			active_blocks.erase( it );
			return;
		}
	}
	os_memory::deallocate_memory( header, sizeof( HugeMemoryHeader ) + header->block_size );
}

void MemoryPool::HugeMemoryManager::release_resources()
{
	std::lock_guard<std::mutex> lock( tracking_mutex );
	for ( auto& [ pointer, size ] : active_blocks )
		os_memory::deallocate_memory( pointer, size );
	active_blocks.clear();
}

// =====================================================================
//  MemoryPool 实现
// =====================================================================

MemoryPool::MemoryPool()
{
	/* 初始化全局小桶 & 中层空闲链头 */
	for ( auto& bucket : small_manager.global_buckets )
	{
#if SUPPORT_128BIT_CAS
		bucket.head.store( { nullptr, 0 }, std::memory_order_relaxed );
#else
		bucket.head.store( nullptr, std::memory_order_relaxed );
#endif
	}

	if ( !construction_warning_shown.exchange( true, std::memory_order_relaxed ) )
	{
		std::cerr << "\033[33m[MemoryPool] 直接使用 MemoryPool 仅适合内部场景，生产代码请封装成 PoolAllocator！\033[0m\n";
		std::cerr << "\033[32m[MemoryPool Warning] Direct use of MemoryPool may cause tracking misses or duplicates. Please use PoolAllocator instead!\033[0m\n";
	}
}

MemoryPool::~MemoryPool()
{
	is_destructing.store( true, std::memory_order_release );
	flush_current_thread_cache();

	huge_manager.release_resources();
	large_manager.release_resources();
	medium_manager.release_resources();
	small_manager.release_resources();
}


/* ============================================================
 * ⬇⬇⬇ 统一入口 —— 根据宏选择调试 / 正式版本
 * ============================================================ */
//#define MP_MEMORY_DEBUG

/* ────────────────────────────────────────────────────────────
 *  1. 统一入口：allocate
 * ────────────────────────────────────────────────────────────*/
// MemoryPool::allocate - cleaned-up version with braces balanced
void* MemoryPool::allocate( std::size_t requested_bytes, std::size_t requested_alignment, const char* file, std::uint32_t line, bool nothrow )
{
	// —— 对齐值合法化 ——
	if ( !is_power_of_two( requested_alignment ) || requested_alignment == 0 || requested_alignment > MAX_ALLOWED_ALIGNMENT )
	{
		requested_alignment = DEFAULT_ALIGNMENT;
	}

	// —— 快速路径：≤ DEFAULT_ALIGNMENT 对齐 ——
	if ( requested_alignment <= DEFAULT_ALIGNMENT )
	{
		void* result = nullptr;
		if ( requested_bytes <= SMALL_BLOCK_MAX_SIZE )
		{
			result = small_manager.allocate( requested_bytes );
		}
		else if ( requested_bytes <= MEDIUM_BLOCK_MAX_SIZE )
		{
			result = medium_manager.allocate( requested_bytes );
		}
		else if ( requested_bytes <= HUGE_BLOCK_THRESHOLD )
		{
			result = large_manager.allocate( requested_bytes );
		}
		else
		{
			result = huge_manager.allocate( requested_bytes );
		}
		return result;
	}

	// —— 慢速路径：> DEFAULT_ALIGNMENT 对齐 ——
	const std::size_t header_bytes = sizeof( AlignHeader );
	const std::size_t total_bytes = requested_bytes + requested_alignment - 1 + header_bytes;

	// 申请原始块
	void* raw_block = ( total_bytes <= SMALL_BLOCK_MAX_SIZE ) ? small_manager.allocate( total_bytes ) : os_memory::allocate_memory( total_bytes );

	if ( !raw_block )
	{
		if ( !nothrow )
		{
			throw std::bad_alloc();
		}
		return nullptr;
	}

	// 对齐处理
	std::uintptr_t base_address = reinterpret_cast<std::uintptr_t>( raw_block ) + header_bytes;
	void*		   aligned_ptr = reinterpret_cast<void*>( align_up( base_address, requested_alignment ) );

	// 设置对齐头
	auto* align_header = reinterpret_cast<AlignHeader*>( static_cast<char*>( aligned_ptr ) - header_bytes );
	align_header->tag = ALIGN_SENTINEL;
	align_header->raw = raw_block;
	align_header->size = ( total_bytes <= SMALL_BLOCK_MAX_SIZE ) ? 0 : total_bytes;

	return aligned_ptr;
}


/* ────────────────────────────────────────────────────────────
 *  2. 统一入口：deallocate
 * ────────────────────────────────────────────────────────────*/
// MemoryPool::deallocate - cleaned-up version with braces balanced
void MemoryPool::deallocate( void* user_pointer )
{
	// 如果指针为空，直接返回
	if ( !user_pointer )
	{
		return;
	}

	// ① Huge (16 B 头)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( HugeMemoryHeader ) )
	{
		auto* header = reinterpret_cast<HugeMemoryHeader*>( static_cast<char*>( user_pointer ) - sizeof( HugeMemoryHeader ) );
		if ( header->magic == HugeMemoryHeader::MAGIC )
		{
			huge_manager.deallocate( header );
			return;
		}
	}

	// ② Large (16 B 头)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( LargeMemoryHeader ) )
	{
		auto* header = reinterpret_cast<LargeMemoryHeader*>( static_cast<char*>( user_pointer ) - sizeof( LargeMemoryHeader ) );
		if ( header->magic == LargeMemoryHeader::MAGIC )
		{
			large_manager.deallocate( header );
			return;
		}
	}

	// ③ Medium (24 B 头)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( MediumMemoryHeader ) )
	{
		auto* header = reinterpret_cast<MediumMemoryHeader*>( static_cast<char*>( user_pointer ) - sizeof( MediumMemoryHeader ) );
		if ( header->magic == MediumMemoryHeader::MAGIC )
		{
			medium_manager.deallocate( header );
			return;
		}
	}

	// ④ Small (24 B 头)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( SmallMemoryHeader ) )
	{
		auto* header = reinterpret_cast<SmallMemoryHeader*>( static_cast<char*>( user_pointer ) - sizeof( SmallMemoryHeader ) );
		if ( header->magic == SmallMemoryHeader::MAGIC )
		{
			small_manager.deallocate( header );
			return;
		}
	}

	// ⑤ AlignHeader (对齐头)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( AlignHeader ) )
	{
		auto* align_header = reinterpret_cast<AlignHeader*>( static_cast<char*>( user_pointer ) - sizeof( AlignHeader ) );
		if ( align_header->tag == ALIGN_SENTINEL )
		{
			if ( align_header->size == 0 )
			{
				auto* hdr = reinterpret_cast<SmallMemoryHeader*>( static_cast<char*>( align_header->raw ) - sizeof( SmallMemoryHeader ) );
				small_manager.deallocate( hdr );
			}
			else
			{
				os_memory::deallocate_memory( align_header->raw, align_header->size );
			}
			return;
		}
	}

	throw std::bad_alloc();
}

void MemoryPool::flush_current_thread_cache()
{
	small_manager.flush_thread_local_cache();
}