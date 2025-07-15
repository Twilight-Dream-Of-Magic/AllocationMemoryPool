/**
 * @file safe_memory_leak_reporter.hpp
 * @brief 安全的内存泄漏报告器 / Safe memory leak reporter
 *
 * @details
 * 1. 支持自动或手动模式的内存泄漏报告 / Support automatic or manual leak reporting;
 * 2. 自动模式下在程序退出时报告 / Automatic mode reports at exit;
 * 3. 使用低级 IO 避免报告过程中再次分配内存 / Use low-level IO to avoid allocations during reporting.
 *
 * 代码风格说明 / Style Notes
 * ---------------------------------------------------------------------------
 * 1. 完全避免缩写：所有标识符使用完整单词（report, mode, destruction...）；
 * 2. PascalCase 用于类型 / PascalCase for types;
 * 3. snake_case 用于函数、变量 / snake_case for functions and variables;
 * 4. SCREAMING_SNAKE_CASE 用于常量 / SCREAMING_SNAKE_CASE for constants;
 * 5. 中英文注释并存，支持 Doxygen 文档 / Bilingual comments with Doxygen support.
 */

#pragma once
#ifndef SAFE_MEMORY_LEAK_REPORTER_HPP
#define SAFE_MEMORY_LEAK_REPORTER_HPP

#include "memory_tracker.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>

/**
 * @class SafeMemoryLeakReporter
 * @brief 安全的内存泄漏报告单例 / Singleton for safe memory leak reporting
 */
class SafeMemoryLeakReporter
{
public:
	/// @brief 报告模式 / Reporting mode
	enum ReportMode
	{
		Automatic,	//!< 自动模式，在程序退出时报告 / automatic at exit
		Manual,		//!< 手动模式，仅在调用 report() 时报告 / manual on demand
		Disabled	//!< 禁用模式，不进行报告 / disabled
	};

	/// @brief 获取单例实例 / Get singleton instance
	static SafeMemoryLeakReporter& get()
	{
		static SafeMemoryLeakReporter reporter;
		return reporter;
	}

	/**
     * @brief 初始化报告器 / Initialize reporter
     * @param report_mode    报告模式 / reporting mode
     * @param detailed       是否开启详细追踪 / detailed tracking if true
     * @param output_stream  输出流，默认为 std::cerr / output stream, default std::cerr
     */
	void initialize( ReportMode report_mode = Automatic, bool detailed = true, std::ostream* output_stream = &std::cerr )
	{
		report_mode_ = report_mode;
		detailed_tracking_ = detailed;
		output_stream_ = output_stream;

		// 启用 MemoryTracker 跟踪 / enable MemoryTracker if not already usable
		if ( !MemoryTracker::instance().is_useable() )
		{
			MemoryTracker::instance().enable( detailed_tracking_ );
		}

		// 自动模式下注册 atexit 回调 / register exit handler in automatic mode
		if ( report_mode_ == Automatic )
		{
			std::atexit( &SafeMemoryLeakReporter::exit_handler );
		}
	}

	/// @brief 手动触发报告 / Manually trigger report
	void report()
	{
		report_if_safe();
	}

	/**
     * @brief 设置报告模式 / Set reporting mode
     * @param report_mode    新的报告模式 / new reporting mode
     */
	void set_report_mode( ReportMode report_mode )
	{
		report_mode_ = report_mode;
	}

	/// @brief 禁用报告 / Disable reporting
	void disable()
	{
		report_mode_ = Disabled;
	}

	/**
     * @brief 启用报告 / Enable reporting with current settings
     * @param report_mode    报告模式 / reporting mode
     */
	void enable( ReportMode report_mode )
	{
		initialize( report_mode, detailed_tracking_, output_stream_ );
	}

	/// @brief 在全局析构期间的标志 / Flag for global destruction period
	static std::atomic<bool> in_global_destruction;

private:
	SafeMemoryLeakReporter() = default;
	~SafeMemoryLeakReporter() = default;
	SafeMemoryLeakReporter( const SafeMemoryLeakReporter& ) = delete;
	SafeMemoryLeakReporter& operator=( const SafeMemoryLeakReporter& ) = delete;

	/// @brief 程序退出时的回调入口 / atexit callback entry
	static void exit_handler()
	{
		get().report_if_safe();
	}

	/// @brief 在安全时报告 / Report if not disabled or during destruction
	void report_if_safe()
	{
		if ( report_mode_ != Disabled && !in_global_destruction )
		{
			perform_report();
		}
	}

	/// @brief 执行报告 / Perform reporting
	void perform_report()
	{
		if ( !output_stream_ || !MemoryTracker::instance().is_useable() )
		{
			return;
		}

		write_output( "\n=== SafeMemoryLeakReporter Report ===\n" );
		MemoryTracker::instance().report_leaks( *output_stream_ );
		write_output( "======================================\n\n" );
	}

	/**
     * @brief 低级输出函数 / Low-level output to avoid allocations
     * @param message        常量字符串 / constant message
     */
	void write_output( const char* message )
	{
		if ( output_stream_ == &std::cerr )
		{
			std::fwrite( message, 1, std::strlen( message ), stderr );
		}
		else if ( output_stream_ == &std::cout )
		{
			std::fwrite( message, 1, std::strlen( message ), stdout );
		}
		else
		{
			*output_stream_ << message;
		}
	}

	ReportMode	  report_mode_ = Automatic;		//!< 当前报告模式 / current reporting mode
	bool		  detailed_tracking_ = true;	//!< 是否详细追踪 / detailed tracking enabled
	std::ostream* output_stream_ = &std::cerr;	//!< 输出流 / output stream
};

// 在全局析构期间设置标志 / Set flag during global destruction
std::atomic<bool> SafeMemoryLeakReporter::in_global_destruction { false };

/// @brief 全局析构监视器 / Monitor for global destruction
class AtExitDestructionMonitor
{
public:
	~AtExitDestructionMonitor()
	{
		SafeMemoryLeakReporter::in_global_destruction = true;
	}
};

static AtExitDestructionMonitor global_destruction_monitor;	 //!< 全局实例，用于监视析构 / Global instance to monitor destruction

#endif	// SAFE_MEMORY_LEAK_REPORTER_HPP
