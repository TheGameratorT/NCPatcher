#pragma once

#include "common.hpp"

#include <string>
#include <filesystem>
#include <stdexcept>

namespace ncp
{
//=============================

	// exception =============================

	class exception : public std::exception
	{
	public:
		exception(const std::string& reason);
		const char* what() const noexcept override;

	protected:
		exception();
		std::string _M_msg;
	};
	
	// file_error =============================

	class file_error : public exception
	{
	public:
		enum operation
		{
			read = 0,
			write = 1,
			create = 2,
			find = 3
		};

		explicit file_error(const std::filesystem::path& file_path, file_error::operation operation) noexcept;

	private:
		std::filesystem::path _M_file_path;
		file_error::operation _M_operation;
	};

	// dir_error =============================

	class dir_error : public exception
	{
	public:
		enum operation
		{
			create = 0,
			find = 1
		};

		explicit dir_error(const std::filesystem::path& dir_path, dir_error::operation operation) noexcept;

	private:
		std::filesystem::path _M_dir_path;
		dir_error::operation _M_operation;
	};

//=============================
}
