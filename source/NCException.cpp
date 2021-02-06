#include "NCException.hpp"

namespace NC
{
//=============================

	// exception (abstract base) =============================

	exception::exception() {}
	exception::exception(const std::string& error, const std::string& reason)
	{
		build(error, reason);
	}

	const char* exception::what() const noexcept
	{
		return msg.c_str();
	}

	void exception::build(const std::string& error, const std::string& reason)
	{
		std::ostringstream oss;
		oss << OERROR << error << "\n" << OREASON << reason;
		msg = oss.str();
	}

	// file_error =============================

	static const char* file_ops[] = {
		"Unable to find file: ",
		"Unable to open file for reading: ",
		"Unable to open file for writing: "
	};

	file_error::file_error(const std::string& error, const std::filesystem::path& file_path, Operation operation)
	{
		std::ostringstream oss;
		oss << file_ops[operation] << OSTR(file_path.string());
		build(error, oss.str());
	}

//=============================
}
