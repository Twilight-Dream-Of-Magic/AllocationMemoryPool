#include "memory_pool.hpp"

// ============================ 静态成员定义 ============================
std::atomic<bool> MemoryPool::construction_warning_shown { false };

/* -------- TLS 实例 -------- */
thread_local SmallMemoryManager::ThreadLocalCache SmallMemoryManager::thread_local_cache;

/* =====================================================================
 *  SmallMemoryManager — 实现
 * ===================================================================== */

/* -------- allocate -------- */
void* SmallMemoryManager::allocate( std::size_t bytes, std::size_t alignment = sizeof( std::max_align_t ) )
{
	const std::size_t index = calculate_bucket_index( bytes );	// 计算桶索引 / Calculate bucket index
	const std::size_t bucket_bytes = BUCKET_SIZES[ index ];		// 获取桶大小 / Get bucket size

	/* 1) 线程本地缓存 / Thread local cache */
	if ( auto* header = thread_local_cache.buckets[ index ] )
	{
		thread_local_cache.buckets[ index ] = header->next;
		header->is_free.store( false, std::memory_order_relaxed );	// 标记为已分配 / Mark as allocated
		header->magic = SmallMemoryHeader::MAGIC;					// 设置魔法值 / Set magic value
		return header->data();										// 返回数据指针 / Return data pointer
	}

	/* 2) 全局 ABA-safe 栈 / Global ABA-safe stack */
	GlobalBucket& bucket = global_buckets[ index ];

#if SUPPORT_128BIT_CAS
	typename PointerTag old_head = bucket.head.load( std::memory_order_acquire );

	while ( old_head.pointer )
	{
		SmallMemoryHeader*	candidate = old_head.pointer;
		typename PointerTag new_head = { candidate->next, old_head.tag + 1 };

		if ( bucket.head.compare_exchange_weak( old_head, new_head, std::memory_order_acq_rel, std::memory_order_acquire ) )
		{
			candidate->is_free.store( false, std::memory_order_relaxed );  // 标记为已分配 / Mark as allocated
			candidate->magic = SmallMemoryHeader::MAGIC;				   // 设置魔法值 / Set magic value
			return candidate->data();									   // 返回数据指针 / Return data pointer
		}
	}
#else
	{
		std::lock_guard<std::mutex> lock( bucket.mutex );					 // 加锁保护 / Lock protection
		if ( auto* header = bucket.head.load( std::memory_order_relaxed ) )	 // 检查是否有可用块 / Check for available block
		{
			bucket.head.store( header->next, std::memory_order_relaxed );  // 更新头指针 / Update head pointer
			header->is_free.store( false, std::memory_order_relaxed );	   // 标记为已分配 / Mark as allocated
			header->magic = SmallMemoryHeader::MAGIC;					   // 设置魔法值 / Set magic value
			return header->data();										   // 返回数据指针 / Return data pointer
		}
	}
#endif

	/* 3) 向操作系统申请新 Chunk / Request new chunk from OS */
	const std::size_t block_bytes = sizeof( SmallMemoryHeader ) + bucket_bytes;
	const std::size_t chunk_size = std::max<std::size_t>( 1 * 1024 * 1024, block_bytes * 128 );	 // 最大值 / Max size

	void* chunk_memory;
	{
		std::scoped_lock<std::mutex> lock( chunk_mutex );  // 加锁保护 / Lock protection
		chunk_memory = os_memory::allocate_tracked( chunk_size, alignment );
		if ( !chunk_memory )
			throw std::bad_alloc();	 // 申请失败抛出异常 / Throw exception on failure
		allocated_chunks.emplace_back( chunk_memory, chunk_size );
	}

	/* 切分 chunk / Split chunk */
	const std::size_t block_count = chunk_size / block_bytes;
	if ( block_count == 0 )
	{
		os_memory::deallocate_tracked( chunk_memory, chunk_size );	// 释放内存 / Deallocate memory
		throw std::bad_alloc();										// 申请失败抛出异常 / Throw exception on failure
	}

	char*			   cursor = static_cast<char*>( chunk_memory );
	SmallMemoryHeader* first_block = nullptr;
	SmallMemoryHeader* previous_block = nullptr;

	for ( std::size_t i = 0; i < block_count; ++i, cursor += block_bytes )
	{
		auto* block = reinterpret_cast<SmallMemoryHeader*>( cursor );
		block->bucket_index = static_cast<std::uint32_t>( index );
		block->block_size = static_cast<std::uint32_t>( bucket_bytes );
		block->is_free.store( true, std::memory_order_relaxed );  // 标记为可用 / Mark as free
		block->magic = SmallMemoryHeader::MAGIC;				  // 设置魔法值 / Set magic value
		block->next = nullptr;

		if ( !first_block )
			first_block = block;
		if ( previous_block )
			previous_block->next = block;
		previous_block = block;
	}

	/* 4) 将多余块批量推入全局栈 / Push excess blocks into global stack */
	if ( block_count > 1 )
	{
#if SUPPORT_128BIT_CAS
		typename PointerTag head_snapshot = bucket.head.load( std::memory_order_relaxed );
		previous_block->next = head_snapshot.pointer;  // 连接到原全局链表 / Link to original global list

		typename PointerTag new_snapshot = { first_block->next, head_snapshot.tag + 1 };

		while ( !bucket.head.compare_exchange_weak( head_snapshot, new_snapshot, std::memory_order_release, std::memory_order_relaxed ) )
		{
			previous_block->next = head_snapshot.pointer;  // 重试 / Retry
			new_snapshot.tag = head_snapshot.tag + 1;
		}
#else
		std::lock_guard<std::mutex> lock( bucket.mutex );  // 加锁保护 / Lock protection
		previous_block->next = bucket.head.load( std::memory_order_relaxed );
		bucket.head.store( first_block, std::memory_order_relaxed );  // 更新头指针 / Update head pointer
#endif
	}

	/* 返回首块 / Return the first block */
	if ( first_block != nullptr )
	{
		first_block->is_free.store( false, std::memory_order_relaxed );	 // 标记为已分配 / Mark as allocated
		first_block->magic = SmallMemoryHeader::MAGIC;					 // 设置魔法值 / Set magic value
		return first_block->data();										 // 返回数据指针 / Return data pointer
	}
	else
	{
		throw std::runtime_error( "first_block is null during allocation." );  // 报错 / Error
	}
}

/* -------- deallocate -------- */
void SmallMemoryManager::deallocate( SmallMemoryHeader* header )
{
	bool expected = false;
	if ( !header->is_free.compare_exchange_strong( expected, true, std::memory_order_release, std::memory_order_relaxed ) )
		return;	 // 双重释放 / Double free

	if ( header->in_tls )  // ★ 新增：检测双重回收
	{
		return;
	}

	if ( header->magic != SmallMemoryHeader::MAGIC )
	{
		std::cerr << "[Small] invalid magic during deallocation\n";	 // 魔法值错误 / Invalid magic value
		return;
	}
	header->magic = 0;	 // 防止重复使用 / Prevent reuse
	header->in_tls = 1;	 // ★ 标记“已进 TLS”——放最前

	const std::size_t index = header->bucket_index;
	header->next = thread_local_cache.buckets[ index ];
	thread_local_cache.buckets[ index ] = header;

	if ( ++thread_local_cache.deallocation_counter >= 256 )
		flush_thread_local_cache();
}


/* -------- flush TLS -------- */
void SmallMemoryManager::flush_thread_local_cache()
{
	for ( std::size_t i = 0; i < BUCKET_COUNT; ++i )
	{
		SmallMemoryHeader* local_head = std::exchange( thread_local_cache.buckets[ i ], nullptr );	// 获取线程本地缓存头部 / Get thread-local cache head
		if ( !local_head )
			continue;

		GlobalBucket& bucket = global_buckets[ i ];	 // 获取全局桶 / Get global bucket

		/* 找尾结点一次即可，循环外保证正确性 / Find the tail node once, ensure correctness outside the loop */
		SmallMemoryHeader* tail = local_head;
		while ( tail->next )
			tail = tail->next;

#if SUPPORT_128BIT_CAS
		typename PointerTag head_snapshot = bucket.head.load( std::memory_order_relaxed );

		do
		{
			tail->next = head_snapshot.pointer;	 // 将尾部连接到全局链表头 / Link tail to the global list head
		} while ( !bucket.head.compare_exchange_weak( head_snapshot, { local_head, head_snapshot.tag + 1 }, std::memory_order_release, std::memory_order_relaxed ) );
#else
		{
			std::lock_guard<std::mutex> lock( bucket.mutex );			 // 加锁保护 / Lock protection
			tail->next = bucket.head.load( std::memory_order_relaxed );	 // 将尾部连接到全局链表头 / Link tail to the global list head
			bucket.head.store( local_head, std::memory_order_relaxed );	 // 更新全局链表头 / Update global list head
		}
#endif

		/* 清 in_tls 标记 */
		for ( SmallMemoryHeader* node = local_head; node; node = node->next )
			node->in_tls = 0;

		thread_local_cache.buckets[ i ] = nullptr;	// 清空线程本地缓存 / Clear thread-local cache
	}
	thread_local_cache.deallocation_counter = 0;  // 重置释放计数器 / Reset deallocation counter
}

/* -------- release_resources -------- */
void SmallMemoryManager::release_resources()
{
	flush_thread_local_cache();	 // 清空线程本地缓存 / Clear thread-local cache

	std::lock_guard<std::mutex> this_lock_guard( chunk_mutex );	 // 加锁保护 / Lock protection
	for ( auto& [ pointer, size ] : allocated_chunks )
		os_memory::deallocate_tracked( pointer, size );	 // 释放已分配的内存 / Deallocate allocated memory
	allocated_chunks.clear();							 // 清空已分配块 / Clear allocated chunks

	for ( auto& bucket : global_buckets )
	{
#if SUPPORT_128BIT_CAS
		bucket.head.store( { nullptr, 0 }, std::memory_order_relaxed );	 // 清空全局桶头 / Clear global bucket head
#else
		bucket.head.store( nullptr, std::memory_order_relaxed );  // 清空全局桶头 / Clear global bucket head
#endif
	}
}

/* =====================================================================
 *  MediumMemoryManager — 实现
 * ===================================================================== */

/*──────────────── allocate ────────────────*/
void* MediumMemoryManager::allocate( std::size_t bytes, std::size_t alignment = sizeof( std::max_align_t ) )
{
	const int want_order = order_from_size( bytes );  // 获取所需的内存块级别 / Get the required block level

	// 边界检查：确保请求的order在有效范围内
	if ( want_order < 0 || want_order >= LEVEL_COUNT )
	{
		throw std::bad_alloc();
	}

	/* 第一阶段：常规分配尝试 */
	for ( int current_order = want_order; current_order < LEVEL_COUNT; ++current_order )
	{
		if ( auto* block = pop_block( current_order ) )
		{
			if ( current_order > want_order )
			{
				block = split_to_order( block, current_order, want_order );
			}
			prepare_block( block, want_order );
			return block->data();
		}
	}

	/* 第二阶段：申请新chunk */
	auto* fresh = request_new_chunk( want_order, alignment );
	if ( !fresh )
		throw std::bad_alloc();

	// 占用检测：统计非空级别数量
	size_t occupied_levels = 0;
	for ( int i = 0; i < LEVEL_COUNT; i++ )
	{
		if ( free_lists[ i ].head.load( std::memory_order_acquire ).pointer )
		{
			occupied_levels++;
		}
	}

	const uint16_t mask = free_list_level_mask.load( std::memory_order_acquire );

	// 策略选择
	if ( mask != 0 )
	{
		/* 情况1：系统中有可用块 - 资源回收优先 */
		push_block( fresh, want_order );  // 将新块加入资源池

		// 重新尝试分配，利用可能的新资源
		for ( int current_order = want_order; current_order < LEVEL_COUNT; ++current_order )
		{
			if ( auto* block = pop_block( current_order ) )
			{
				if ( current_order > want_order )
				{
					block = split_to_order( block, current_order, want_order );
				}
				prepare_block( block, want_order );
				return block->data();
			}
		}
	}
	else
	{
		/* 情况2：系统完全干涸 - 直接使用新资源 */
		if ( want_order < LEVEL_COUNT - 1 )
		{
			// 仅当需要时才分割
			fresh = split_to_order( fresh, want_order, want_order );
		}
		prepare_block( fresh, want_order );
		return fresh->data();
	}

	// 终极保障
	throw std::bad_alloc();
}

/*──────────────── deallocate ────────────────*/
void MediumMemoryManager::deallocate( MediumMemoryHeader* header )
{
	/* 确保不是重复释放 / Ensure no double free */
	bool expected = false;
	if ( !header->is_free.compare_exchange_strong( expected, true, std::memory_order_acq_rel, std::memory_order_relaxed ) )
	{
		return;	 // 如果已经释放过，直接返回 / Return if already freed
	}

	/* 校验魔法 / Validate magic value */
	if ( header->magic != MediumMemoryHeader::MAGIC )
	{
		std::cerr << "[MediumBuddy] invalid magic during deallocation\n";  // 魔法值无效 / Invalid magic value
		return;
	}

	// 创建合并请求 / Create merge request
	const int order = order_from_size( header->block_size );

	// 无锁入队 - 环形缓冲区实现 / Lock-free enqueue - Circular buffer implementation
	std::size_t cycle_queue_tail = merge_queue.tail.load( std::memory_order_relaxed );
	std::size_t cycle_queue_next_tail = ( cycle_queue_tail + 1 ) % MERGE_QUEUE_SIZE;

	// 检查队列是否满 / Check if the queue is full
	if ( cycle_queue_next_tail == merge_queue.head.load( std::memory_order_acquire ) )
	{
		// 队列满时直接尝试合并（安全，因为此时只有当前线程操作）/ If the queue is full, attempt merging directly (safe because only the current thread is operating)
		try_merge_buddy( header, order );
		return;
	}

	// 加入队列 / Add to queue
	merge_queue.requests[ cycle_queue_tail ].store( { header, order }, std::memory_order_relaxed );
	merge_queue.tail.store( cycle_queue_next_tail, std::memory_order_release );

	// 确保合并工作线程运行 / Ensure the merge worker thread runs
	if ( !merge_worker_active.load( std::memory_order_acquire ) && !merge_worker_active.exchange( true, std::memory_order_acq_rel ) )
	{
		std::thread( [ this ] { process_merge_queue(); } ).detach();  // 启动合并线程 / Start the merge thread
	}
}

/*──────────────── 内部工具实现 ────────────────*/
inline void MediumMemoryManager::prepare_block( MediumMemoryHeader* block, int order )
{
	block->is_free.store( false, std::memory_order_relaxed );
	block->magic = MediumMemoryHeader::MAGIC;
	block->block_size = size_from_order( order );
	block->next = nullptr;
}

void MediumMemoryManager::push_block( MediumMemoryHeader* header, int order )
{
	header->next = nullptr;
	header->is_free.store( true, std::memory_order_relaxed );  // 标记为可用 / Mark as free

	auto&	   list = free_lists[ order ];	// 获取空闲链表 / Get free list for the order
	PointerTag head = list.head.load( std::memory_order_relaxed );

	do
	{
		header->next = head.pointer;  // 将当前块链接到链表头 / Link the current block to the list head
		PointerTag new_head { header, head.tag + 1 };
	} while ( !list.head.compare_exchange_weak( head, { header, head.tag + 1 }, std::memory_order_release, std::memory_order_relaxed ) );

	// 原子更新位掩码
	uint16_t current_mask = free_list_level_mask.load( std::memory_order_relaxed );
	uint16_t new_mask;
	do
	{
		new_mask = current_mask | ( 1 << order );  // 设置该级别位
	} while ( !free_list_level_mask.compare_exchange_weak( current_mask, new_mask, std::memory_order_release, std::memory_order_relaxed ) );
}

MediumMemoryHeader* MediumMemoryManager::pop_block( int order )
{
	auto&	   list = free_lists[ order ];	// 获取空闲链表 / Get free list for the order
	PointerTag head = list.head.load( std::memory_order_acquire );

	while ( head.pointer )
	{
		PointerTag next { head.pointer->next, head.tag + 1 };

		//Try CompreAndSwap
		if ( list.head.compare_exchange_weak( head, next, std::memory_order_acq_rel, std::memory_order_acquire ) )
		{
			// 成功取出一个块，检查取出后链表是否为空
			if ( next.pointer == nullptr )
			{
				// 链表变空，清除掩码位
				uint16_t current_mask = free_list_level_mask.load( std::memory_order_relaxed );
				uint16_t new_mask;
				do
				{
					new_mask = current_mask & ~( 1 << order );
				} while ( !free_list_level_mask.compare_exchange_weak( current_mask, new_mask, std::memory_order_release, std::memory_order_relaxed ) );
			}
			return head.pointer;
		}
		// CompreAndSwap Failed, Retry
	}

	// 链表已经为空，确保掩码位被清除
	uint16_t current_mask = free_list_level_mask.load( std::memory_order_relaxed );
	uint16_t new_mask;
	do
	{
		new_mask = current_mask & ~( 1 << order );
	} while ( !free_list_level_mask.compare_exchange_weak( current_mask, new_mask, std::memory_order_release, std::memory_order_relaxed ) );

	return nullptr;
}

/*──────────────── process_merge_queue ────────────────*/
void MediumMemoryManager::process_merge_queue()
{
	while ( true )
	{
		std::size_t cycle_queue_head = merge_queue.head.load( std::memory_order_relaxed );	// 获取队列头 / Get the queue head
		std::size_t cycle_queue_tail = merge_queue.tail.load( std::memory_order_acquire );	// 获取队列尾 / Get the queue tail

		// 队列为空 / If the queue is empty
		if ( cycle_queue_head == cycle_queue_tail )
		{
			// 设置非活跃状态前做最后检查 / Final check before setting inactive state
			if ( merge_queue.head.load( std::memory_order_acquire ) == merge_queue.tail.load( std::memory_order_acquire ) )
			{
				merge_worker_active.store( false, std::memory_order_release );	// 设置工作线程非活跃 / Set the worker thread inactive
				return;
			}
			continue;  // 继续循环 / Continue looping
		}

		// 取出请求 / Retrieve the request
		MergeRequest request = merge_queue.requests[ cycle_queue_head ].load( std::memory_order_relaxed );

		// 移动头指针 / Move the head pointer
		std::size_t cycle_queue_next_head = ( cycle_queue_head + 1 ) % MERGE_QUEUE_SIZE;
		merge_queue.head.store( cycle_queue_next_head, std::memory_order_release );

		// 处理合并 / Process the merge
		try_merge_buddy( request.block, request.order );
	}
}

/*──────────────── try_merge_buddy ────────────────*/
void MediumMemoryManager::try_merge_buddy( MediumMemoryHeader* block, int order )
{
	// 查找chunk的逻辑（与原始merge_buddy相同）/ Logic for finding the chunk (same as original merge_buddy)
	void*		chunk_base = nullptr;
	std::size_t chunk_bytes = 0;
	for ( auto const& chunk : allocated_chunks )
	{
		char* base = static_cast<char*>( chunk.first );
		if ( reinterpret_cast<char*>( block ) >= base && reinterpret_cast<char*>( block ) < base + chunk.second )
		{
			chunk_base = base;
			chunk_bytes = chunk.second;
			break;
		}
	}
	if ( !chunk_base )
		return;

	/* 尝试合并伙伴 / Try to merge buddy */
	while ( order < LEVEL_COUNT - 1 )
	{
		// 计算当前块在chunk中的偏移量 / Calculate block's offset within the chunk
		std::uintptr_t offset = reinterpret_cast<char*>( block ) - static_cast<char*>( chunk_base );
		
		// 使用XOR计算伙伴块的偏移量 / Calculate buddy's offset using XOR trick
		std::uintptr_t buddy_offset = offset ^ ( size_from_order( order ) );

		// 检查伙伴块是否超出chunk边界 / Check if buddy exceeds chunk boundary
		if ( buddy_offset + size_from_order( order ) > chunk_bytes )
			break;	// buddy 超出 chunk / Buddy exceeds chunk size

		// 获取伙伴块头部指针 / Get buddy block header pointer
		auto* buddy = reinterpret_cast<MediumMemoryHeader*>( static_cast<char*>( chunk_base ) + buddy_offset );

		// 使用原子操作检查buddy状态 / Use atomic operation to check buddy state
		if ( !buddy->is_free.load( std::memory_order_acquire ) || buddy->block_size != size_from_order( order ) )
		{
			break;
		}

		/* 尝试原子性地移除buddy / Try to atomically remove buddy */
		if ( !try_remove_from_freelist( buddy, order ) )
		{
			break;
		}

		/* 再次确认buddy状态 / Recheck buddy state */
		if ( !buddy->is_free.load( std::memory_order_acquire ) || buddy->block_size != size_from_order( order ) )
		{
			// 放回freelist / Put back to freelist
			push_block( buddy, order );
			break;
		}

		/* 合并 - 取较小地址为新块首 / Merge - take the smaller address as the new block's start */
		block = ( offset < buddy_offset ) ? block : buddy;
		block->block_size = size_from_order( order ) << 1;	// 合并后的块大小 / Merged block size
		order++;
	}

	// 将合并后的块放回空闲列表 / Push the merged block back to the free list
	push_block( block, order );
}

/*──────────────── try_remove_from_freelist ────────────────*/
bool MediumMemoryManager::try_remove_from_freelist( MediumMemoryHeader* header, int order )
{
	auto&	   list = free_lists[ order ];	// 获取对应级别的空闲链表 / Get the corresponding free list for the order
	PointerTag head = list.head.load( std::memory_order_acquire );

	while ( true )
	{
		if ( head.pointer == header )
		{
			// =====================================================
			// 场景1: 目标块是链表头节点 / Scenario 1: Target is the head
			// =====================================================

			PointerTag next { header->next, head.tag + 1 };

			// 尝试原子性地替换链表头 / Attempt atomic head replacement
			/*
			 * 使用CAS解决ABA问题：
			 * - 比较当前head和之前加载的head
			 * - 如果相同，替换为new_head
			 * - 如果不同，重试
			 * 
			 * CAS solves ABA problem:
			 * - Compare current head with previously loaded head
			 * - If same, replace with new_head
			 * - If different, retry
			 */
			if ( list.head.compare_exchange_weak( head, { header, head.tag + 1 }, std::memory_order_acq_rel, std::memory_order_acquire ) )
			{
				return true;  // 成功移除 / Successfully removed
			}
		}
		else
		{
			// =====================================================
			// 场景2: 目标块在链表中间 / Scenario 2: Target is in list middle
			// =====================================================

			// 遍历链表查找 / Traverse the list to find
			MediumMemoryHeader* current = head.pointer;
			while ( current )
			{
				if ( current == header )
				{
					PointerTag new_head { head.pointer, head.tag + 1 };

					// 尝试原子性地更新链表头 / Attempt atomic head update
					/*
					 * 为什么更新链表头？ 
					 * 在无锁链表中，中间节点移除非常复杂。这里使用简化策略：
					 * 通过修改链表头标签强制其他线程重试，间接使目标块"失效"
					 * 
					 * Why update list head?
					 * Removing middle nodes in lock-free lists is complex.
					 * Simplified strategy: Force other threads to retry by
					 * changing head tag, indirectly "invalidating" target
					 */
					if ( list.head.compare_exchange_weak( head, new_head, std::memory_order_acq_rel, std::memory_order_acquire ) )
					{
						return true;  // 成功移除 / Successfully removed
					}
					break;
				}
				current = current->next;
			}
			return false;  // 没找到 / Not found
		}
	}
}

MediumMemoryHeader* MediumMemoryManager::split_to_order( MediumMemoryHeader* block, int from_order, int to_order )
{
	// 确保目标order在有效范围内
	if ( to_order < 0 || to_order >= LEVEL_COUNT || to_order > from_order )
	{
		return block;
	}

	for ( int current_order = from_order - 1; current_order >= to_order; --current_order )
	{
		std::size_t half = size_from_order( current_order );				  // 计算每个块的一半 / Calculate half of the block size
		char*		right_pointer = reinterpret_cast<char*>( block ) + half;  // 获取右侧块的位置 / Get the position of the right block

		auto* right_header = reinterpret_cast<MediumMemoryHeader*>( right_pointer );  // 获取右侧块的头部 / Get the header of the right block
		right_header->block_size = half;											  // 设置右侧块的大小 / Set the size of the right block
		right_header->is_free.store( true, std::memory_order_relaxed );				  // 标记为可用 / Mark as free
		right_header->magic = MediumMemoryHeader::MAGIC;							  // 设置魔法值 / Set magic value
		right_header->next = nullptr;												  // 设置下一块为空 / Set next to null

		push_block( right_header, current_order );	// 将右侧块推入空闲链表 / Push the right block into the free list
		block->block_size = half;					// 更新原块的大小 / Update the original block's size
	}
	return block;  // 返回原块 / Return the original block
}

MediumMemoryHeader* MediumMemoryManager::request_new_chunk( int min_order, std::size_t alignment = sizeof( std::max_align_t ) )
{
	std::lock_guard<std::mutex> this_lock_guard( chunk_mutex );	 // 加锁保护 / Lock protection

	// 计算需要的内存块大小 / Calculate the required chunk size
	const std::size_t chunk_bytes = size_from_order( min_order );							 // 使用 min_order 来计算内存块大小 / Use min_order to calculate the chunk size
	void*			  chunk_memory = os_memory::allocate_tracked( chunk_bytes, alignment );	 // 向操作系统请求内存 / Request memory from the OS
	if ( !chunk_memory )
		return nullptr;	 // 申请失败，返回空指针 / Return null if allocation fails

	allocated_chunks.emplace_back( chunk_memory, chunk_bytes );	 // 记录已分配的 chunk / Record the allocated chunk

	// 整个 chunk 作为一个大块先放进最高层，再让 allocate 重新拿 / Place the entire chunk into the highest level first, then let allocate reuse it
	auto* header = static_cast<MediumMemoryHeader*>( chunk_memory );
	header->block_size = chunk_bytes;						   // 设置块大小 / Set the chunk size
	header->is_free.store( true, std::memory_order_relaxed );  // 标记为可用 / Mark as free
	header->magic = MediumMemoryHeader::MAGIC;				   // 设置魔法值 / Set magic value
	header->next = nullptr;									   // 设置下一块为空 / Set next to null

	return header;
}

/*──────────────── release_resources ────────────────*/
void MediumMemoryManager::release_resources()
{
	/* 清空 freelist / Clear the freelist */
	for ( auto& free_list : free_lists )
	{
		free_list.head.store( { nullptr, 0 }, std::memory_order_relaxed );	// 设置空头指针 / Set head pointer to null
	}

	for ( auto& [ pointer, size ] : allocated_chunks )
		os_memory::deallocate_tracked( pointer, size );	 // 释放内存 / Deallocate memory
	allocated_chunks.clear();							 // 清空已分配块 / Clear the allocated chunks
}

/* =====================================================================
 *  LargeMemoryManager — 实现
 * ===================================================================== */

void* LargeMemoryManager::allocate( std::size_t bytes, std::size_t alignment = sizeof( std::max_align_t ) )
{
	const std::size_t total = sizeof( LargeMemoryHeader ) + bytes;				 // 计算总内存大小 / Calculate total memory size
	void*			  memory = os_memory::allocate_tracked( total, alignment );	 // 向操作系统申请内存 / Request memory from the OS
	if ( !memory )
		throw std::bad_alloc();	 // 如果申请失败，抛出异常 / Throw exception if allocation fails

	auto* header = static_cast<LargeMemoryHeader*>( memory );  // 获取内存头部 / Get the memory header
	header->magic = LargeMemoryHeader::MAGIC;				   // 设置魔法值 / Set magic value
	header->block_size = bytes;								   // 设置块大小 / Set block size

	std::lock_guard<std::mutex> this_lock_guard( tracking_mutex );	// 加锁保护 / Lock protection
	active_blocks.push_back( header );								// 记录已分配的块 / Record the allocated block

	return header->data();	// 返回数据指针 / Return data pointer
}

void LargeMemoryManager::deallocate( LargeMemoryHeader* header )
{
	if ( header->magic != LargeMemoryHeader::MAGIC )
	{
		std::cerr << "[Large] invalid magic during deallocation\n";	 // 魔法值无效 / Invalid magic value
		return;
	}
	header->magic = 0;	// 清除魔法值 / Clear magic value

	{
		std::lock_guard<std::mutex> this_lock_guard( tracking_mutex );										 // 加锁保护 / Lock protection
		auto						iter = std::find( active_blocks.begin(), active_blocks.end(), header );	 // 查找并删除已释放块 / Find and remove the deallocated block
		if ( iter != active_blocks.end() )
			active_blocks.erase( iter );  // 删除已释放块 / Remove the deallocated block
	}

	os_memory::deallocate_tracked( header, sizeof( LargeMemoryHeader ) + header->block_size );	// 释放内存 / Deallocate memory
}

void LargeMemoryManager::release_resources()
{
	{
		std::scoped_lock<std::mutex> lock( tracking_mutex );
		for ( auto* header : active_blocks )
			os_memory::deallocate_tracked( header, sizeof( LargeMemoryHeader ) + header->block_size );	// 释放所有已分配的内存 / Deallocate all allocated memory
		active_blocks.clear();																			// 清空已分配块 / Clear the allocated blocks
	}
}

/* =====================================================================
 *  HugeMemoryManager — 实现
 * ===================================================================== */

void* HugeMemoryManager::allocate( std::size_t bytes, std::size_t alignment = sizeof( std::max_align_t ) )
{
	const std::size_t total = sizeof( HugeMemoryHeader ) + bytes;				 // 计算总内存大小 / Calculate total memory size
	void*			  memory = os_memory::allocate_tracked( total, alignment );	 // 向操作系统申请内存 / Request memory from the OS
	if ( !memory )
		throw std::bad_alloc();	 // 如果申请失败，抛出异常 / Throw exception if allocation fails

	auto* header = static_cast<HugeMemoryHeader*>( memory );  // 获取内存头部 / Get the memory header
	header->magic = HugeMemoryHeader::MAGIC;				  // 设置魔法值 / Set magic value
	header->block_size = bytes;								  // 设置块大小 / Set block size

	std::lock_guard<std::mutex> this_lock_guard( tracking_mutex );	// 加锁保护 / Lock protection
	active_blocks.emplace_back( memory, total );					// 记录已分配的块 / Record the allocated block

	return header->data();	// 返回数据指针 / Return data pointer
}

void HugeMemoryManager::deallocate( HugeMemoryHeader* header )
{
	if ( header->magic != HugeMemoryHeader::MAGIC )
	{
		std::cerr << "[Huge] invalid magic during deallocation\n";	// 魔法值无效 / Invalid magic value
		return;
	}
	header->magic = 0;	// 清除魔法值 / Clear magic value

	{
		std::lock_guard<std::mutex> this_lock_guard( tracking_mutex );																													   // 加锁保护 / Lock protection
		auto						iter = std::find_if( active_blocks.begin(), active_blocks.end(), [ header ]( auto& reference_object ) { return reference_object.first == header; } );  // 查找并删除已释放块 / Find and remove the deallocated block
		if ( iter != active_blocks.end() )
		{
			os_memory::deallocate_tracked( iter->first, iter->second );	 // 释放内存 / Deallocate memory
			active_blocks.erase( iter );								 // 删除已释放块 / Remove the deallocated block
			return;
		}
	}

	os_memory::deallocate_tracked( header, sizeof( HugeMemoryHeader ) + header->block_size );  // 释放内存 / Deallocate memory
}

void HugeMemoryManager::release_resources()
{
	{
		std::scoped_lock<std::mutex> lock( tracking_mutex );
		for ( auto& [ pointer, size ] : active_blocks )
			os_memory::deallocate_tracked( pointer, size );	 // 释放所有已分配的内存 / Deallocate all allocated memory
		active_blocks.clear();								 // 清空已分配块 / Clear the allocated blocks
	}
}

// =====================================================================
//  MemoryPool 实现
// =====================================================================

MemoryPool::MemoryPool()
{
	/* 初始化全局小桶 & 中层空闲链头 / Initialize global small buckets and medium-level free list heads */
	for ( auto& bucket : small_manager.global_buckets )
	{
#if SUPPORT_128BIT_CAS
		bucket.head.store( { nullptr, 0 }, std::memory_order_relaxed );	 // 使用128位CAS / Use 128-bit CAS
#else
		bucket.head.store( nullptr, std::memory_order_relaxed );  // 普通指针CAS / Normal pointer CAS
#endif
	}

	if ( !construction_warning_shown.exchange( true, std::memory_order_relaxed ) )
	{
		std::cerr << "\033[33m[MemoryPool Warning] 直接使用 MemoryPool 仅适合内部场景，生产代码请封装成 PoolAllocator！\033[0m\n";										   // 警告信息 / Warning message
		std::cerr << "\033[32m[MemoryPool Warning] Direct use of MemoryPool may cause tracking misses or duplicates. Please use PoolAllocator instead!\033[0m\n";  // 警告信息 / Warning message
	}
}

MemoryPool::~MemoryPool()
{
	/* 1. 标记正在销毁（让各个 manager 在 flush/释放时知道不要往 thread-cache 再放） */
	is_destructing.store( true, std::memory_order_release );  // 设置销毁标志 / Set destruction flag

	/* 2. 刷新本线程的缓存，回收所有没还给 OS 的小/中/大块 */
	flush_current_thread_cache();  // 刷新当前线程缓存 / Flush current thread cache

	/* 3. 依次释放各等级的剩余资源 */
	huge_manager.release_resources();	 // 释放超大内存资源 / Release huge memory resources
	large_manager.release_resources();	 // 释放大内存资源 / Release large memory resources
	medium_manager.release_resources();	 // 释放中等内存资源 / Release medium memory resources
	small_manager.release_resources();	 // 释放小内存资源 / Release small memory resources

	/* 4. 最终检查，全局计数器应为 0 */
	const uint64_t leaked = os_memory::used_memory_bytes_counter.load( std::memory_order_seq_cst );
	if ( leaked != 0 )
	{
		std::cerr << "[MemoryPool] Memory leak detected: " << leaked << " bytes still allocated." << std::endl;
	}

	/* 5. 检查操作计数，allocate/deallocate 是否成对 */
	const uint64_t original_point = os_memory::user_operation_counter.load( std::memory_order_seq_cst );
	if ( original_point != 0 )
	{
		std::cerr << "[MemoryPool] Operation imbalance detected: " << original_point << " net operations (allocs minus frees)." << std::endl;
	}
}

void* MemoryPool::allocate( std::size_t requested_bytes, std::size_t requested_alignment, const char* file, std::uint32_t line, bool nothrow )
{
	/* ── 1. alignment validation ───────────────────────────── */
	if ( (requested_alignment & 1) == 1 || !os_memory::memory_pool::is_power_of_two( requested_alignment ))
	{
		if ( requested_alignment > MAX_ALLOWED_ALIGNMENT && !nothrow )
			throw std::bad_alloc();	 //!< illegal alignment

		if ( requested_alignment > 0 )
		{
			requested_alignment = DEFAULT_ALIGNMENT;
		}
	}

	/* ── 2. slow path : large alignment ────────────────────── */
	if ( requested_alignment > DEFAULT_ALIGNMENT )
	{
		const std::size_t extra_alignment_padding_bytes = requested_alignment - 1 + ALIGN_HEADER_BYTES;
		const std::size_t total_allocated_bytes = requested_bytes + extra_alignment_padding_bytes;

		void* const raw_block_pointer = os_memory::allocate_tracked( total_allocated_bytes, requested_bytes );
		if ( !raw_block_pointer )
		{
			if ( !nothrow )
				throw std::bad_alloc();
			return nullptr;
		}

		char*		data_region_begin = static_cast<char*>( raw_block_pointer ) + ALIGN_HEADER_BYTES;
		std::size_t remaining_space_bytes = total_allocated_bytes - ALIGN_HEADER_BYTES;
		void*		aligned_user_pointer = data_region_begin;

		if ( !std::align( requested_alignment, requested_bytes, aligned_user_pointer, remaining_space_bytes ) )
		{
			os_memory::deallocate_tracked( raw_block_pointer, total_allocated_bytes );
			if ( !nothrow )
				throw std::bad_alloc();
			return nullptr;
		}

		auto* align_header_pointer = reinterpret_cast<AlignHeader*>( static_cast<char*>( aligned_user_pointer ) - ALIGN_HEADER_BYTES );
		align_header_pointer->tag = ALIGN_SENTINEL;
		align_header_pointer->raw = raw_block_pointer;
		align_header_pointer->size = total_allocated_bytes;
		return aligned_user_pointer;
	}

	/* ── 3. fast path : default alignment ──────────────────── */
	const std::size_t total_bytes_including_header = requested_bytes + NOT_ALIGN_HEADER_BYTES;

	std::size_t	  block_header_size_bytes = 0;
	std::uint32_t block_owner_type_identifier = 0;
	void*		  internal_data_region_pointer = nullptr;  // → points to data()

	if ( total_bytes_including_header <= SMALL_BLOCK_MAX_SIZE )
	{
		block_header_size_bytes = sizeof( SmallMemoryHeader );
		block_owner_type_identifier = 1;
		internal_data_region_pointer = small_manager.allocate( total_bytes_including_header, requested_alignment );
	}
	else if ( total_bytes_including_header <= MEDIUM_BLOCK_MAX_SIZE )
	{
		block_header_size_bytes = sizeof( MediumMemoryHeader );
		block_owner_type_identifier = 2;
		internal_data_region_pointer = medium_manager.allocate( total_bytes_including_header, requested_alignment );
	}
	else if ( total_bytes_including_header <= HUGE_BLOCK_THRESHOLD )
	{
		block_header_size_bytes = sizeof( LargeMemoryHeader );
		block_owner_type_identifier = 3;
		internal_data_region_pointer = large_manager.allocate( total_bytes_including_header, requested_alignment );
	}
	else
	{
		block_header_size_bytes = sizeof( HugeMemoryHeader );
		block_owner_type_identifier = 4;
		internal_data_region_pointer = huge_manager.allocate( total_bytes_including_header, requested_alignment );
	}

	if ( !internal_data_region_pointer )
	{
		if ( !nothrow )
			throw std::bad_alloc();
		return nullptr;
	}

	auto* unaligned_block_header = reinterpret_cast<NotAlignHeader*>( internal_data_region_pointer );
	unaligned_block_header->owner_type = block_owner_type_identifier;
	unaligned_block_header->raw = static_cast<char*>( internal_data_region_pointer ) - block_header_size_bytes;

	return static_cast<char*>( internal_data_region_pointer ) + NOT_ALIGN_HEADER_BYTES;
}

/* -------------------------------------------------------------------------- */

void MemoryPool::deallocate( void* user_pointer )
{
	if ( !user_pointer )
		return;

	const std::uintptr_t user_pointer_address = reinterpret_cast<std::uintptr_t>( user_pointer );

	/* ── 1. check large‑alignment header ───────────────────── */
	{
		auto* align_header_pointer = reinterpret_cast<const AlignHeader*>( user_pointer_address - ALIGN_HEADER_BYTES );

		AlignHeader stacked_copy_of_align_header {};
		std::memcpy( &stacked_copy_of_align_header, align_header_pointer, sizeof( stacked_copy_of_align_header ) );

		if ( stacked_copy_of_align_header.tag == ALIGN_SENTINEL )
		{
			os_memory::deallocate_tracked( stacked_copy_of_align_header.raw, stacked_copy_of_align_header.size );
			return;
		}
	}

	/* ── 2. check default‑alignment header ─────────────────── */
	{
		auto* unaligned_header_pointer = reinterpret_cast<const NotAlignHeader*>( user_pointer_address - NOT_ALIGN_HEADER_BYTES );

		NotAlignHeader stacked_copy_of_unaligned_header {};
		std::memcpy( &stacked_copy_of_unaligned_header, unaligned_header_pointer, sizeof( stacked_copy_of_unaligned_header ) );

		switch ( stacked_copy_of_unaligned_header.owner_type )
		{
		case 1:
			small_manager.deallocate( static_cast<SmallMemoryHeader*>( stacked_copy_of_unaligned_header.raw ) );
			return;
		case 2:
			medium_manager.deallocate( static_cast<MediumMemoryHeader*>( stacked_copy_of_unaligned_header.raw ) );
			return;
		case 3:
			large_manager.deallocate( static_cast<LargeMemoryHeader*>( stacked_copy_of_unaligned_header.raw ) );
			return;
		case 4:
			huge_manager.deallocate( static_cast<HugeMemoryHeader*>( stacked_copy_of_unaligned_header.raw ) );
			return;
		default:
#if defined( _DEBUG )
			throw os_memory::bad_dealloc( "deallocate: invalid owner type" );
#endif
		}
	}

#if defined( _DEBUG )
	throw os_memory::bad_dealloc( "deallocate: invalid pointer" );
#endif
}


void MemoryPool::flush_current_thread_cache()
{
	small_manager.flush_thread_local_cache();  // 刷新线程本地缓存 / Flush thread-local cache
}
