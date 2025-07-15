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

	void* chunk_mem;
	{
		std::lock_guard<std::mutex> this_lock_guard( chunk_mutex );  // 加锁保护 / Lock protection
		chunk_mem = os_memory::allocate_memory( chunk_size );
		if ( !chunk_mem )
			throw std::bad_alloc();	 // 申请失败抛出异常 / Throw exception on failure
		allocated_chunks.emplace_back( chunk_mem, chunk_size );
	}

	/* 切分 chunk / Split chunk */
	const std::size_t block_count = chunk_size / block_bytes;
	if ( block_count == 0 )
	{
		os_memory::deallocate_memory( chunk_mem, chunk_size );	// 释放内存 / Deallocate memory
		throw std::bad_alloc();									// 申请失败抛出异常 / Throw exception on failure
	}

	char*			   cursor = static_cast<char*>( chunk_mem );
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
		prev_block->next = bucket.head.load( std::memory_order_relaxed );
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
void MemoryPool::SmallMemoryManager::deallocate( SmallMemoryHeader* header )
{
	bool expected = false;
	if ( !header->is_free.compare_exchange_strong( expected, true, std::memory_order_release, std::memory_order_relaxed ) )
		return;	 // 双重释放 / Double free

	if ( header->magic != SmallMemoryHeader::MAGIC )
	{
		std::cerr << "[Small] invalid magic during deallocation\n";	 // 魔法值错误 / Invalid magic value
		return;
	}
	header->magic = 0;	// 防止重复使用 / Prevent reuse

	const std::size_t index = header->bucket_index;

	header->next = thread_local_cache.buckets[ index ];
	thread_local_cache.buckets[ index ] = header;

	if ( ++thread_local_cache.deallocation_counter >= 256 )	 // [FIX] 阈值放宽 / Threshold relaxed
		flush_thread_local_cache();
}


/* -------- flush TLS -------- */
void MemoryPool::SmallMemoryManager::flush_thread_local_cache()
{
	for ( std::size_t i = 0; i < BUCKET_COUNT; ++i )
	{
		SmallMemoryHeader* local_head = thread_local_cache.buckets[ i ];  // 获取线程本地缓存头部 / Get thread-local cache head
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
		thread_local_cache.buckets[ i ] = nullptr;	// 清空线程本地缓存 / Clear thread-local cache
	}
	thread_local_cache.deallocation_counter = 0;  // 重置释放计数器 / Reset deallocation counter
}

/* -------- release_resources -------- */
void MemoryPool::SmallMemoryManager::release_resources()
{
	flush_thread_local_cache();	 // 清空线程本地缓存 / Clear thread-local cache

	std::lock_guard<std::mutex> this_lock_guard( chunk_mutex );  // 加锁保护 / Lock protection
	for ( auto& [ pointer, size ] : allocated_chunks )
		os_memory::deallocate_memory( pointer, size );	// 释放已分配的内存 / Deallocate allocated memory
	allocated_chunks.clear();							// 清空已分配块 / Clear allocated chunks

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
void* MemoryPool::MediumMemoryManager::allocate( std::size_t bytes )
{
	const int want_order = order_from_size( bytes );  // 获取所需的内存块级别 / Get the required block level

	/* 1) 在同级或更高级别寻找空闲块 / Look for free blocks at the same level or higher */
	for ( int current_order = want_order; current_order < LEVEL_COUNT; ++current_order )
	{
		if ( auto* block = pop_block( current_order ) )
		{
			// 向下切分到目标级别 / Split down to the target level
			if ( current_order > want_order )
				block = split_to_order( block, current_order, want_order );

			block->is_free.store( false, std::memory_order_relaxed );  // 标记为已分配 / Mark as allocated
			block->magic = MediumMemoryHeader::MAGIC;				   // 设置魔法值 / Set magic value
			block->block_size = size_from_order( want_order );		   // 设置块大小 / Set block size
			block->next = nullptr;

			return block->data();  // 返回数据指针 / Return data pointer
		}
	}

	/* 2) 无可用块 —— 申请新 chunk，再尝试一次 / No available blocks — request a new chunk and try again */
	auto* fresh = request_new_chunk( want_order );
	if ( !fresh )
		throw std::bad_alloc();	 // 如果申请失败，抛出异常 / Throw exception if allocation fails

	return fresh->data();  // 返回数据指针 / Return data pointer
}

/*──────────────── deallocate ────────────────*/
void MemoryPool::MediumMemoryManager::deallocate( MediumMemoryHeader* header )
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
void MemoryPool::MediumMemoryManager::push_block( MediumMemoryHeader* header, int order )
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
}

MediumMemoryHeader* MemoryPool::MediumMemoryManager::pop_block( int order )
{
	auto&	   list = free_lists[ order ];	// 获取空闲链表 / Get free list for the order
	PointerTag head = list.head.load( std::memory_order_acquire );

	while ( head.pointer )
	{
		PointerTag next { head.pointer->next, head.tag + 1 };
		if ( list.head.compare_exchange_weak( head, next, std::memory_order_acq_rel, std::memory_order_acquire ) )
			return head.pointer;  // 返回空闲块 / Return the free block
	}

	return nullptr;	 // 如果没有空闲块，返回空指针 / Return nullptr if no free blocks
}

/*──────────────── process_merge_queue ────────────────*/
void MemoryPool::MediumMemoryManager::process_merge_queue()
{
	while ( true )
	{
		std::size_t cycle_queue_head = merge_queue.head.load( std::memory_order_relaxed );	 // 获取队列头 / Get the queue head
		std::size_t cycle_queue_tail = merge_queue.tail.load( std::memory_order_acquire );	 // 获取队列尾 / Get the queue tail

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
void MemoryPool::MediumMemoryManager::try_merge_buddy( MediumMemoryHeader* block, int order )
{
	// 查找chunk的逻辑（与原始merge_buddy相同）/ Logic for finding the chunk (same as original merge_buddy)
	void*		chunk_base = nullptr;
	std::size_t chunk_bytes = 0;
	for ( auto const& rec : allocated_chunks )
	{
		char* base = static_cast<char*>( rec.first );
		if ( reinterpret_cast<char*>( block ) >= base && reinterpret_cast<char*>( block ) < base + rec.second )
		{
			chunk_base = base;
			chunk_bytes = rec.second;
			break;
		}
	}
	if ( !chunk_base )
		return;

	/* 尝试合并伙伴 / Try to merge buddy */
	while ( order < LEVEL_COUNT - 1 )
	{
		std::uintptr_t offset = reinterpret_cast<char*>( block ) - static_cast<char*>( chunk_base );
		std::uintptr_t buddy_offset = offset ^ ( size_from_order( order ) );

		if ( buddy_offset + size_from_order( order ) > chunk_bytes )
			break;	// buddy 超出 chunk / Buddy exceeds chunk size

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
bool MemoryPool::MediumMemoryManager::try_remove_from_freelist( MediumMemoryHeader* header, int order )
{
	auto&	   list = free_lists[ order ];	// 获取对应级别的空闲链表 / Get the corresponding free list for the order
	PointerTag head = list.head.load( std::memory_order_acquire );

	while ( true )
	{
		if ( head.pointer == header )
		{
			PointerTag next { header->next, head.tag + 1 };
			if ( list.head.compare_exchange_weak( head, { header, head.tag + 1 }, std::memory_order_acq_rel, std::memory_order_acquire ) )
			{
				return true;  // 成功移除 / Successfully removed
			}
		}
		else
		{
			// 遍历链表查找 / Traverse the list to find
			MediumMemoryHeader* current = head.pointer;
			while ( current )
			{
				if ( current == header )
				{
					// 找到后尝试移除 / Found, attempt to remove
					PointerTag new_head { head.pointer, head.tag + 1 };
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

MediumMemoryHeader* MemoryPool::MediumMemoryManager::split_to_order( MediumMemoryHeader* block, int from_order, int to_order )
{
	for ( int ord = from_order - 1; ord >= to_order; --ord )
	{
		std::size_t half = size_from_order( ord );							  // 计算每个块的一半 / Calculate half of the block size
		char*		right_pointer = reinterpret_cast<char*>( block ) + half;  // 获取右侧块的位置 / Get the position of the right block

		auto* right_header = reinterpret_cast<MediumMemoryHeader*>( right_pointer );  // 获取右侧块的头部 / Get the header of the right block
		right_header->block_size = half;											  // 设置右侧块的大小 / Set the size of the right block
		right_header->is_free.store( true, std::memory_order_relaxed );				  // 标记为可用 / Mark as free
		right_header->magic = MediumMemoryHeader::MAGIC;							  // 设置魔法值 / Set magic value
		right_header->next = nullptr;												  // 设置下一块为空 / Set next to null

		push_block( right_header, ord );  // 将右侧块推入空闲链表 / Push the right block into the free list
		block->block_size = half;		  // 更新原块的大小 / Update the original block's size
	}
	return block;  // 返回原块 / Return the original block
}

MediumMemoryHeader* MemoryPool::MediumMemoryManager::request_new_chunk( int min_order )
{
	std::lock_guard<std::mutex> this_lock_guard( chunk_mutex );  // 加锁保护 / Lock protection

	// 计算需要的内存块大小 / Calculate the required chunk size
	const std::size_t chunk_bytes = size_from_order( min_order );				 // 使用 min_order 来计算内存块大小 / Use min_order to calculate the chunk size
	void*			  chunk_memory = os_memory::allocate_memory( chunk_bytes );	 // 向操作系统请求内存 / Request memory from the OS
	if ( !chunk_memory )
		return nullptr;	 // 申请失败，返回空指针 / Return null if allocation fails

	allocated_chunks.emplace_back( chunk_memory, chunk_bytes );	 // 记录已分配的 chunk / Record the allocated chunk

	// 整个 chunk 作为一个大块先放进最高层，再让 allocate 重新拿 / Place the entire chunk into the highest level first, then let allocate reuse it
	auto* header = static_cast<MediumMemoryHeader*>( chunk_memory );
	header->block_size = chunk_bytes;						   // 设置块大小 / Set the chunk size
	header->is_free.store( true, std::memory_order_relaxed );  // 标记为可用 / Mark as free
	header->magic = MediumMemoryHeader::MAGIC;				   // 设置魔法值 / Set magic value
	header->next = nullptr;									   // 设置下一块为空 / Set next to null

	// 直接使用 min_order 来设置 top_order，确保内存块符合请求的级别 / Directly set top_order using min_order to ensure the chunk matches the requested level
	int top_order = min_order;
	push_block( header, top_order );  // 推入相应级别的空闲链表 / Push into the free list at the corresponding level

	// 再弹出可满足请求的块 / Pop a block that meets the request
	return pop_block( min_order );	// 确保使用 min_order 进行 pop / Ensure the block popped meets min_order
}

/*──────────────── release_resources ────────────────*/
void MemoryPool::MediumMemoryManager::release_resources()
{
	/* 清空 freelist / Clear the freelist */
	for ( auto& fl : free_lists )
	{
		fl.head.store( { nullptr, 0 }, std::memory_order_relaxed );	 // 设置空头指针 / Set head pointer to null
	}

	/* 释放 chunks / Deallocate chunks */
	std::lock_guard<std::mutex> this_lock_guard( chunk_mutex );  // 加锁保护 / Lock protection
	for ( auto& [ pointer, size ] : allocated_chunks )
		os_memory::deallocate_memory( pointer, size );	// 释放内存 / Deallocate memory
	allocated_chunks.clear();							// 清空已分配块 / Clear the allocated chunks
}

/* =====================================================================
 *  LargeMemoryManager — 实现
 * ===================================================================== */

void* MemoryPool::LargeMemoryManager::allocate( std::size_t bytes )
{
	const std::size_t total = sizeof( LargeMemoryHeader ) + bytes;	 // 计算总内存大小 / Calculate total memory size
	void*			  memory = os_memory::allocate_memory( total );	 // 向操作系统申请内存 / Request memory from the OS
	if ( !memory )
		throw std::bad_alloc();	 // 如果申请失败，抛出异常 / Throw exception if allocation fails

	auto* header = static_cast<LargeMemoryHeader*>( memory );  // 获取内存头部 / Get the memory header
	header->magic = LargeMemoryHeader::MAGIC;				   // 设置魔法值 / Set magic value
	header->block_size = bytes;								   // 设置块大小 / Set block size

	std::lock_guard<std::mutex> this_lock_guard( tracking_mutex );  // 加锁保护 / Lock protection
	active_blocks.push_back( header );				  // 记录已分配的块 / Record the allocated block

	return header->data();	// 返回数据指针 / Return data pointer
}

void MemoryPool::LargeMemoryManager::deallocate( LargeMemoryHeader* header )
{
	if ( header->magic != LargeMemoryHeader::MAGIC )
	{
		std::cerr << "[Large] invalid magic during deallocation\n";	 // 魔法值无效 / Invalid magic value
		return;
	}
	header->magic = 0;	// 清除魔法值 / Clear magic value

	{
		std::lock_guard<std::mutex> this_lock_guard( tracking_mutex );												   // 加锁保护 / Lock protection
		auto						iter = std::find( active_blocks.begin(), active_blocks.end(), header );  // 查找并删除已释放块 / Find and remove the deallocated block
		if ( iter != active_blocks.end() )
			active_blocks.erase( iter );	// 删除已释放块 / Remove the deallocated block
	}

	os_memory::deallocate_memory( header, sizeof( LargeMemoryHeader ) + header->block_size );  // 释放内存 / Deallocate memory
}

void MemoryPool::LargeMemoryManager::release_resources()
{
	std::lock_guard<std::mutex> this_lock_guard( tracking_mutex );  // 加锁保护 / Lock protection
	for ( auto* header : active_blocks )
		os_memory::deallocate_memory( header, sizeof( LargeMemoryHeader ) + header->block_size );  // 释放所有已分配的内存 / Deallocate all allocated memory
	active_blocks.clear();																		   // 清空已分配块 / Clear the allocated blocks
}

/* =====================================================================
 *  HugeMemoryManager — 实现
 * ===================================================================== */

void* MemoryPool::HugeMemoryManager::allocate( std::size_t bytes )
{
	const std::size_t total = sizeof( HugeMemoryHeader ) + bytes;	 // 计算总内存大小 / Calculate total memory size
	void*			  memory = os_memory::allocate_memory( total );	 // 向操作系统申请内存 / Request memory from the OS
	if ( !memory )
		throw std::bad_alloc();	 // 如果申请失败，抛出异常 / Throw exception if allocation fails

	auto* header = static_cast<HugeMemoryHeader*>( memory );  // 获取内存头部 / Get the memory header
	header->magic = HugeMemoryHeader::MAGIC;				  // 设置魔法值 / Set magic value
	header->block_size = bytes;								  // 设置块大小 / Set block size

	std::lock_guard<std::mutex> this_lock_guard( tracking_mutex );  // 加锁保护 / Lock protection
	active_blocks.emplace_back( memory, total );	  // 记录已分配的块 / Record the allocated block

	return header->data();	// 返回数据指针 / Return data pointer
}

void MemoryPool::HugeMemoryManager::deallocate( HugeMemoryHeader* header )
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
			os_memory::deallocate_memory( iter->first, iter->second );	// 释放内存 / Deallocate memory
			active_blocks.erase( iter );								// 删除已释放块 / Remove the deallocated block
			return;
		}
	}

	os_memory::deallocate_memory( header, sizeof( HugeMemoryHeader ) + header->block_size );  // 释放内存 / Deallocate memory
}

void MemoryPool::HugeMemoryManager::release_resources()
{
	std::lock_guard<std::mutex> lock( tracking_mutex );	 // 加锁保护 / Lock protection
	for ( auto& [ pointer, size ] : active_blocks )
		os_memory::deallocate_memory( pointer, size );	// 释放所有已分配的内存 / Deallocate all allocated memory
	active_blocks.clear();								// 清空已分配块 / Clear the allocated blocks
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
		std::cerr << "\033[33m[MemoryPool] 直接使用 MemoryPool 仅适合内部场景，生产代码请封装成 PoolAllocator！\033[0m\n";										   // 警告信息 / Warning message
		std::cerr << "\033[32m[MemoryPool Warning] Direct use of MemoryPool may cause tracking misses or duplicates. Please use PoolAllocator instead!\033[0m\n";  // 警告信息 / Warning message
	}
}

MemoryPool::~MemoryPool()
{
	is_destructing.store( true, std::memory_order_release );  // 设置销毁标志 / Set destruction flag
	flush_current_thread_cache();							  // 刷新当前线程缓存 / Flush current thread cache

	huge_manager.release_resources();	 // 释放超大内存资源 / Release huge memory resources
	large_manager.release_resources();	 // 释放大内存资源 / Release large memory resources
	medium_manager.release_resources();	 // 释放中等内存资源 / Release medium memory resources
	small_manager.release_resources();	 // 释放小内存资源 / Release small memory resources
}

/* ────────────────────────────────────────────────────────────
 *  1. 统一入口：allocate / Unified entry: allocate
 * ────────────────────────────────────────────────────────────*/
void* MemoryPool::allocate( std::size_t requested_bytes, std::size_t requested_alignment, const char* file, std::uint32_t line, bool nothrow )
{
	// —— 对齐值合法化 / Alignment value legalization ——
	if ( !is_power_of_two( requested_alignment ) || requested_alignment == 0 || requested_alignment > MAX_ALLOWED_ALIGNMENT )
	{
		requested_alignment = DEFAULT_ALIGNMENT;  // 如果不合法则使用默认对齐值 / If invalid, use default alignment
	}

	// —— 快速路径：≤ DEFAULT_ALIGNMENT 对齐 / Fast path: <= DEFAULT_ALIGNMENT alignment ——
	if ( requested_alignment <= DEFAULT_ALIGNMENT )
	{
		void* result = nullptr;
		if ( requested_bytes <= SMALL_BLOCK_MAX_SIZE )
		{
			result = small_manager.allocate( requested_bytes );	 // 小块内存分配 / Allocate small block
		}
		else if ( requested_bytes <= MEDIUM_BLOCK_MAX_SIZE )
		{
			result = medium_manager.allocate( requested_bytes );  // 中等块内存分配 / Allocate medium block
		}
		else if ( requested_bytes <= HUGE_BLOCK_THRESHOLD )
		{
			result = large_manager.allocate( requested_bytes );	 // 大块内存分配 / Allocate large block
		}
		else
		{
			result = huge_manager.allocate( requested_bytes );	// 超大块内存分配 / Allocate huge block
		}
		return result;
	}

	// —— 慢速路径：> DEFAULT_ALIGNMENT 对齐 / Slow path: > DEFAULT_ALIGNMENT alignment ——
	const std::size_t header_bytes = sizeof( AlignHeader );									   // 计算对齐头大小 / Calculate alignment header size
	const std::size_t total_bytes = requested_bytes + requested_alignment - 1 + header_bytes;  // 计算总内存大小 / Calculate total memory size

	// 申请原始块 / Allocate raw block
	void* raw_block = ( total_bytes <= SMALL_BLOCK_MAX_SIZE ) ? small_manager.allocate( total_bytes ) : os_memory::allocate_memory( total_bytes );

	if ( !raw_block )
	{
		if ( !nothrow )
		{
			throw std::bad_alloc();	 // 申请失败抛出异常 / Throw exception if allocation fails
		}
		return nullptr;	 // 如果不抛出异常，则返回空指针 / Return null if not throwing exception
	}

	// 对齐处理 / Alignment handling
	std::uintptr_t base_address = reinterpret_cast<std::uintptr_t>( raw_block ) + header_bytes;				// 计算对齐后的地址 / Calculate aligned address
	void*		   aligned_pointer = reinterpret_cast<void*>( align_up( base_address, requested_alignment ) );	// 获取对齐后的指针 / Get the aligned pointer

	// 设置对齐头 / Set alignment header
	auto* align_header = reinterpret_cast<AlignHeader*>( static_cast<char*>( aligned_pointer ) - header_bytes );
	align_header->tag = ALIGN_SENTINEL;												 // 设置标记 / Set sentinel tag
	align_header->raw = raw_block;													 // 设置原始块指针 / Set raw block pointer
	align_header->size = ( total_bytes <= SMALL_BLOCK_MAX_SIZE ) ? 0 : total_bytes;	 // 设置块大小 / Set block size

	return aligned_pointer;	 // 返回对齐后的指针 / Return the aligned pointer
}


/* ────────────────────────────────────────────────────────────
 *  2. 统一入口：deallocate / Unified entry: deallocate
 * ────────────────────────────────────────────────────────────*/
void MemoryPool::deallocate( void* user_pointer )
{
	// 如果指针为空，直接返回 / If the pointer is null, return immediately
	if ( !user_pointer )
	{
		return;
	}

	// ① Huge (16 B 头) / Huge (16B header)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( HugeMemoryHeader ) )
	{
		auto* header = reinterpret_cast<HugeMemoryHeader*>( static_cast<char*>( user_pointer ) - sizeof( HugeMemoryHeader ) );
		if ( header->magic == HugeMemoryHeader::MAGIC )
		{
			huge_manager.deallocate( header );	// 释放超大内存块 / Deallocate huge memory block
			return;
		}
	}

	// ② Large (16 B 头) / Large (16B header)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( LargeMemoryHeader ) )
	{
		auto* header = reinterpret_cast<LargeMemoryHeader*>( static_cast<char*>( user_pointer ) - sizeof( LargeMemoryHeader ) );
		if ( header->magic == LargeMemoryHeader::MAGIC )
		{
			large_manager.deallocate( header );	 // 释放大内存块 / Deallocate large memory block
			return;
		}
	}

	// ③ Medium (24 B 头) / Medium (24B header)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( MediumMemoryHeader ) )
	{
		auto* header = reinterpret_cast<MediumMemoryHeader*>( static_cast<char*>( user_pointer ) - sizeof( MediumMemoryHeader ) );
		if ( header->magic == MediumMemoryHeader::MAGIC )
		{
			medium_manager.deallocate( header );  // 释放中等内存块 / Deallocate medium memory block
			return;
		}
	}

	// ④ Small (24 B 头) / Small (24B header)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( SmallMemoryHeader ) )
	{
		auto* header = reinterpret_cast<SmallMemoryHeader*>( static_cast<char*>( user_pointer ) - sizeof( SmallMemoryHeader ) );
		if ( header->magic == SmallMemoryHeader::MAGIC )
		{
			small_manager.deallocate( header );	 // 释放小内存块 / Deallocate small memory block
			return;
		}
	}

	// ⑤ AlignHeader (对齐头) / AlignHeader (Alignment header)
	if ( reinterpret_cast<std::uintptr_t>( user_pointer ) >= sizeof( AlignHeader ) )
	{
		auto* align_header = reinterpret_cast<AlignHeader*>( static_cast<char*>( user_pointer ) - sizeof( AlignHeader ) );
		if ( align_header->tag == ALIGN_SENTINEL )
		{
			if ( align_header->size == 0 )
			{
				auto* header = reinterpret_cast<SmallMemoryHeader*>( static_cast<char*>( align_header->raw ) - sizeof( SmallMemoryHeader ) );
				small_manager.deallocate( header );  // 释放小内存块 / Deallocate small memory block
			}
			else
			{
				os_memory::deallocate_memory( align_header->raw, align_header->size );	// 释放原始内存 / Deallocate raw memory
			}
			return;
		}
	}

	throw std::bad_alloc();	 // 无效指针，抛出异常 / Invalid pointer, throw exception
}

void MemoryPool::flush_current_thread_cache()
{
	small_manager.flush_thread_local_cache();  // 刷新线程本地缓存 / Flush thread-local cache
}
