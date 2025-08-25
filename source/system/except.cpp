#include "except.hpp"

#include <sstream>
#include <utility>

#include "log.hpp"

namespace ncp
{
//=============================

	// exception =============================

	exception::exception() = default;
	exception::exception(std::string reason) :
		m_msg(std::move(reason))
	{}

	const char* exception::what() const noexcept
	{
		return m_msg.c_str();
	}

	// file_error =============================

	static const char* file_ops[] = {
		"Could not open file for reading: ",
		"Could not open file for writing: ",
		"Could not create file: ",
		"Could not find file: "
	};

	file_error::file_error(std::filesystem::path file_path, file_error::operation operation) noexcept :
		m_file_path(std::move(file_path)),
		m_operation(operation)
	{
		std::ostringstream oss;
		oss << file_ops[m_operation] << OSTR(m_file_path.string());
		m_msg = oss.str();
	}

	// dir_error =============================

	static const char* dir_ops[] = {
		"Could not create directory: ",
		"Could not find directory: "
	};

	dir_error::dir_error(std::filesystem::path dir_path, dir_error::operation operation) noexcept :
		m_dir_path(std::move(dir_path)),
		m_operation(operation)
	{
		std::ostringstream oss;
		oss << dir_ops[m_operation] << OSTR(m_dir_path.string());
		m_msg = oss.str();
	}

//=============================
}
