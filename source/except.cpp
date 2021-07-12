#include "except.hpp"

#include <sstream>

namespace ncp
{
//=============================

	// exception =============================

	exception::exception() {}
	exception::exception(const std::string& reason) :
		_M_msg(reason)
	{}

	const char* exception::what() const noexcept
	{
		return _M_msg.c_str();
	}

	// file_error =============================

	static const char* file_ops[] = {
		"Could not open file for reading: ",
		"Could not open file for writing: ",
		"Could not create file: ",
		"Could not find file: "
	};

	file_error::file_error(const std::filesystem::path& file_path, file_error::operation operation) noexcept :
		_M_file_path(file_path),
		_M_operation(operation)
	{
		std::ostringstream oss;
		oss << file_ops[_M_operation] << OSTR(_M_file_path.string());
		_M_msg = oss.str();
	}

	// dir_error =============================

	static const char* dir_ops[] = {
		"Could not create directory: ",
		"Could not find directory: "
	};

	dir_error::dir_error(const std::filesystem::path& dir_path, dir_error::operation operation) noexcept :
		_M_dir_path(dir_path),
		_M_operation(operation)
	{
		std::ostringstream oss;
		oss << dir_ops[_M_operation] << OSTR(_M_dir_path.string());
		_M_msg = oss.str();
	}

//=============================
}
