#pragma once

#include "Common.hpp"

#include <string>
#include <filesystem>
#include <stdexcept>

namespace NC
{
//=============================

	// exception (abstract base) =============================

	class exception : public std::exception
	{
	public:
		exception(const std::string& error, const std::string& reason);
		const char* what() const noexcept override;

	protected:
		exception();
		void build(const std::string& error, const std::string& reason);

	private:
		std::string msg;
	};
	
	// file_error =============================

	class file_error : public exception
	{
	public:
		enum Operation
		{
			read,
			write,
			find
		};

		file_error(const std::string& error, const std::filesystem::path& file_path, Operation operation);
	};

//=============================
}
