/**
 * @file   memory_tracker.hpp
 * @brief  内存分配追踪器 / Memory allocation tracker
 *
 * @details
 * 1. 采用「开启 / 关闭」两级追踪开关，可选详细模式（记录源码位置）。  
 * 2. 彻底避免缩写：所有标识符使用完整英文单词（例如 allocation_iterator、memory_mutex）。  
 * 3. 提供内存泄漏报告与当前内存占用统计。  
 * 4. 线程安全：所有共享数据均受 std::mutex 保护。
 *
 * 代码风格说明 / Style Notes
 * ---------------------------------------------------------------------------
 * 1. 彻底避免缩写：所有标识符均使用完整单词 (block, index, pointer...)；
 * 2. 中英文注释并存，方便自我审阅与团队协作；
 */

#pragma once
#ifndef MEMORY_TRACKER_HPP
#define MEMORY_TRACKER_HPP

#include <cstdint>
#include <cstddef>
#include <cstdlib>

#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>

/*--------------------------------- 结构体 / Struct -------------------------*/
/**
 * @struct AllocationInformation
 * @brief  单次分配记录 / One allocation record
 */
struct AllocationInformation
{
	size_t		size = 0;				 //!< 分配字节数 / bytes allocated
	const char* file_path = nullptr;	 //!< 源文件路径 / source file
	uint32_t	line_number = 0;		 //!< 源代码行号 / line number
	void*		user_pointer = nullptr;	 //!< 用户可见指针 / user pointer
	void*		raw_pointer = nullptr;	 //!< 实际起始指针 / raw pointer (for aligned or pooled alloc)
};

/*---------------------------------- 类 / Class -----------------------------*/
class MemoryTracker
{
public:
	/*----------------------------- 单例接口 / Singleton ---------------------*/
	static MemoryTracker& instance()
	{
		static MemoryTracker tracker_instance;
		return tracker_instance;
	}

	/*----------------------------- 追踪控制 / Control -----------------------*/
	/**
     * @brief 启用追踪 / Enable tracking
     * @param is_detailed 是否记录源码位置 / record file & line?
     */
	void enable( bool is_detailed = false )
	{
		std::scoped_lock<std::mutex> scoped_lock( memory_mutex );
		tracking_enabled = true;
		detailed_tracking_on = is_detailed;
	}

	/** 关闭追踪 / Disable tracking */
	void disable()
	{
		std::scoped_lock<std::mutex> scoped_lock( memory_mutex );
		tracking_enabled = false;
	}

	bool is_useable()
	{
		return tracking_enabled;
	}

	/*----------------------------- 分配 / Allocation ------------------------*/
	/**
     * @brief 记录一次分配 / Track an allocation
     * @note  若未开启追踪或指针为空，则直接返回。
     */
	void track_allocation( void* user_pointer, size_t allocation_size, const char* file_path = nullptr, uint32_t line_number = 0, void* raw_pointer = nullptr )
	{
		if ( !tracking_enabled || !user_pointer )
			return;

		std::scoped_lock<std::mutex> scoped_lock( memory_mutex );
		allocation_map[ user_pointer ] = { allocation_size, file_path, line_number, user_pointer, raw_pointer ? raw_pointer : user_pointer };
	}

	/**
     * @brief 根据用户指针查找原始指针 / Get raw pointer by user pointer
     */
	void* find_tracked_pointer( void* user_pointer )
	{
		if ( !user_pointer )
			return nullptr;

		std::scoped_lock<std::mutex> scoped_lock( memory_mutex );
		auto						 allocation_iterator = allocation_map.find( user_pointer );
		return ( allocation_iterator != allocation_map.end() ) ? allocation_iterator->second.raw_pointer : nullptr;
	}

	/*----------------------------- 释放 / Deallocation ----------------------*/
	/**
     * @brief 记录一次释放 / Track a deallocation
     */
	void track_deallocation( void* user_pointer )
	{
		if ( !tracking_enabled || !user_pointer )
			return;

		std::scoped_lock<std::mutex> scoped_lock( memory_mutex );
		allocation_map.erase( user_pointer );
	}

	/*----------------------------- 报告 / Reports ---------------------------*/
	/**
     * @brief 打印内存泄漏报告 / Print memory-leak report
     */
	void report_leaks( std::ostream& output_stream = std::cout )
	{
		if ( !tracking_enabled )
			return;

		std::vector<AllocationInformation> leak_list;

		{
			std::scoped_lock<std::mutex> scoped_lock( memory_mutex );
			if ( allocation_map.empty() )
			{
				output_stream << "No memory leaks detected.\n";
				return;
			}
			leak_list.reserve( allocation_map.size() );
			for ( const auto& pair : allocation_map )
			{
				leak_list.push_back( pair.second );
			}
		}

		output_stream << "\n=== Memory Leak Report ===\n";
		output_stream << "Total leaks: " << leak_list.size() << "\n\n";
		for ( const auto& allocation : leak_list )
		{
			output_stream << "Leaked " << allocation.size << " bytes at " << allocation.user_pointer;
			if ( detailed_tracking_on && allocation.file_path )
			{
				output_stream << " (allocated at " << allocation.file_path << ":" << allocation.line_number << ")";
			}
			output_stream << '\n';
		}
		output_stream << "=== End of Report ===\n";
	}

	/**
     * @brief 获取当前占用内存字节数 / Get current total allocated size
     */
	size_t current_memory_usage()
	{
		std::scoped_lock<std::mutex> scoped_lock( memory_mutex );
		size_t						 total_size = 0;
		for ( const auto& pair : allocation_map )
		{
			total_size += pair.second.size;
		}
		return total_size;
	}

private:
	/*----------------------------- 构造 & 数据 / Ctor & Data ---------------*/
	MemoryTracker() = default;
	~MemoryTracker()
	{
		// 如果追踪已启用且存在未释放的内存分配记录，则打印内存泄漏报告  
		// If tracking is enabled and there are unfreed memory allocation records, print a memory leak report  
		if(tracking_enabled && !allocation_map.empty())  
		{  
			report_leaks();
			tracking_enabled = false;
		}
	}
	MemoryTracker( const MemoryTracker& ) = delete;
	MemoryTracker& operator=( const MemoryTracker& ) = delete;

	std::mutex memory_mutex;				  //!< 保护 allocation_map / mutex guarding map
	bool	   tracking_enabled = false;	  //!< 全局开关 / global enable flag
	bool	   detailed_tracking_on = false;  //!< 是否记录源码位置 / detailed mode

	std::unordered_map<void*, AllocationInformation> allocation_map;  //!< 追踪表 / tracking map
};

#endif	// MEMORY_TRACKER_HPP
