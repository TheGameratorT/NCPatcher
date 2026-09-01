#pragma once

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
		explicit exception(std::string reason);
		[[nodiscard]] const char* what() const noexcept override;

	protected:
		exception();
		std::string m_msg;
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

		explicit file_error(std::filesystem::path file_path, file_error::operation operation) noexcept;

	private:
		std::filesystem::path m_file_path;
		file_error::operation m_operation;
	};

	// cancelled =============================

	// Thrown at a checkpoint once a stop has been asked for. Its own type
	// rather than an exception with a recognisable message, because the caller
	// has to tell "you asked us to stop" from "the build failed": they are
	// different exit codes, a different result status, and different things to
	// put in front of a person.
	class cancelled : public exception
	{
	public:
		cancelled();
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

		explicit dir_error(std::filesystem::path dir_path, dir_error::operation operation) noexcept;

	private:
		std::filesystem::path m_dir_path;
		dir_error::operation m_operation;
	};

//=============================
}
