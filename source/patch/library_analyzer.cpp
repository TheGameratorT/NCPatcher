#include "library_analyzer.hpp"

#include <algorithm>
#include <sstream>
#include <regex>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "../formats/elf.hpp"
#include "../formats/archive.hpp"
#include "../config/buildconfig.hpp"
#include "../system/process.hpp"
#include "patch_info_analyzer.hpp"

LibraryAnalyzer::LibraryAnalyzer() = default;
LibraryAnalyzer::~LibraryAnalyzer() = default;

void LibraryAnalyzer::initialize(
    const BuildTarget& target,
    const std::filesystem::path& buildDir,
    core::CompilationUnitManager& compilationUnitMgr
)
{
    m_target = &target;
    m_buildDir = &buildDir;
    m_compilationUnitMgr = &compilationUnitMgr;
}

void LibraryAnalyzer::analyzeLibraryDependencies()
{
    Log::info("Analyzing library dependencies...");

    // Get toolchain-specific library search paths
    getToolchainLibraryPaths();

    // Parse global linker flags for additional -L paths and -l libraries
    parseLinkerFlags(m_target->ldFlags);
    
    // Parse region-specific linker flags
    // Note: Currently regions don't have ldFlags, but structure is ready for future expansion
    // for (const auto& region : m_target->regions)
    // {
    //     parseLinkerFlags(region.ldFlags);
    // }

    // Find actual library files
    findLibraryFiles();

    if (ncp::Application::isVerbose())
    {
        Log::out << OINFO << "Library search paths:" << std::endl;
        for (const auto& path : m_librarySearchPaths)
            Log::out << "  " << path << std::endl;
            
        Log::out << OINFO << "Library dependencies found:" << std::endl;
        for (const auto& lib : m_libraryNames)
            Log::out << "  -l" << lib << std::endl;
            
        Log::out << OINFO << "Resolved library paths:" << std::endl;
        for (const auto& path : m_libraryPaths)
            Log::out << "  " << path.string() << std::endl;
    }
}

void LibraryAnalyzer::generateLibraryUnits()
{
    Log::info("Generating library compilation units...");

    // Create units from each library
    for (const auto& libraryPath : m_libraryPaths)
    {
        createUnitsFromLibrary(libraryPath);
    }

    if (ncp::Application::isVerbose())
    {
        Log::out << OINFO << "Generated " << m_compilationUnitMgr->getLibraryUnits().size() 
                 << " library compilation units" << std::endl;
    }
}

void LibraryAnalyzer::parseLinkerFlags(const std::string& ldFlags)
{
    if (ldFlags.empty())
        return;

    // First, split the string by commas to handle comma-separated flags
    std::vector<std::string> flagTokens;
    std::istringstream flagStream(ldFlags);
    std::string token;
    
    while (std::getline(flagStream, token, ','))
    {
        // Trim whitespace
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (!token.empty())
            flagTokens.push_back(token);
    }
    
    // If no commas found, fall back to space-separated parsing
    if (flagTokens.size() <= 1)
    {
        flagTokens.clear();
        std::istringstream iss(ldFlags);
        while (iss >> token)
        {
            flagTokens.push_back(token);
        }
    }
    
    // Process each flag token
    for (const std::string& flag : flagTokens)
    {
        if (flag.starts_with("-L"))
        {
            // Library search path
            std::string path = flag.length() > 2 ? flag.substr(2) : "";
            if (!path.empty())
            {
                m_librarySearchPaths.push_back(path);
            }
        }
        else if (flag.starts_with("-l"))
        {
            // Library name
            std::string libName = flag.length() > 2 ? flag.substr(2) : "";
            if (!libName.empty())
            {
                m_libraryNames.push_back(libName);
            }
        }
        // Skip other flags like --use-blx that aren't library-related
    }
}

void LibraryAnalyzer::findLibraryFiles()
{
    m_libraryPaths.clear();
    
    for (const std::string& libName : m_libraryNames)
    {
        std::filesystem::path foundPath;
        bool found = false;
        
        // Try different library naming conventions
        std::vector<std::string> libFileNames = {
            "lib" + libName + ".a",   // Static library
            "lib" + libName + ".so",  // Shared library
            libName + ".a",           // Alternative naming
            libName + ".so"
        };
        
        // Search in library search paths (toolchain + -L paths only)
        for (const std::string& searchPath : m_librarySearchPaths)
        {
            for (const std::string& fileName : libFileNames)
            {
                std::filesystem::path candidatePath = std::filesystem::path(searchPath) / fileName;
                if (std::filesystem::exists(candidatePath))
                {
                    foundPath = candidatePath;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        
        if (found)
        {
            m_libraryPaths.push_back(foundPath);
        }
        else if (ncp::Application::isVerbose())
        {
            Log::out << OWARN << "Library not found: -l" << libName << std::endl;
        }
    }
}

void LibraryAnalyzer::getToolchainLibraryPaths()
{
    // Clear any existing paths
    m_librarySearchPaths.clear();
    
    // Build the command to get library search directories
    std::string toolchain = BuildConfig::getToolchain();
    std::string gccCommand = toolchain + "gcc -print-search-dirs";
    
    try
    {
        std::ostringstream output;
        int exitCode = Process::start(gccCommand.c_str(), &output);
        
        if (exitCode != 0)
        {
            if (ncp::Application::isVerbose())
            {
                Log::out << OWARN << "Failed to get library paths from gcc (exit code: " 
                         << exitCode << ")" << std::endl;
            }
            return;
        }
        
        // Parse the output to extract library paths
        std::string outputStr = output.str();
        std::istringstream iss(outputStr);
        std::string line;
        
        while (std::getline(iss, line))
        {
            // Look for the libraries line
            if (line.starts_with("libraries: ="))
            {
                // Extract the paths part (after "libraries: =")
                std::string pathsStr = line.substr(12); // Skip "libraries: ="
                
                // Split by colons to get individual paths
                std::istringstream pathStream(pathsStr);
                std::string path;
                
                while (std::getline(pathStream, path, ':'))
                {
                    // Trim whitespace
                    path.erase(0, path.find_first_not_of(" \t"));
                    path.erase(path.find_last_not_of(" \t") + 1);
                    
                    if (!path.empty() && std::filesystem::exists(path))
                    {
                        m_librarySearchPaths.push_back(path);
                    }
                }
                break;
            }
        }
        
        if (ncp::Application::isVerbose())
        {
            Log::out << OINFO << "Found " << m_librarySearchPaths.size() 
                     << " toolchain library paths" << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        if (ncp::Application::isVerbose())
        {
            Log::out << OWARN << "Error getting library paths from gcc: " << e.what() << std::endl;
        }
    }
}

void LibraryAnalyzer::createUnitsFromLibrary(const std::filesystem::path& libraryPath)
{
    try
    {
        // Check if it's an archive file (.a)
        if (libraryPath.extension() == ".a")
        {
            createUnitsFromArchive(libraryPath);
            return;
        }

        // Create unit from ELF (for .so files or individual .o files)
        Elf32 elf;
        if (!elf.load(libraryPath))
        {
            Log::out << OWARN << "Failed to load library as ELF: " << libraryPath.filename().string() << std::endl;
            return;
        }

        // Create unit from this ELF
        createUnitFromELF(elf, libraryPath);
    }
    catch (const std::exception& e)
    {
        Log::out << OWARN << "Error analyzing library " << libraryPath.filename().string() 
                 << ": " << e.what() << std::endl;
    }
}

void LibraryAnalyzer::createUnitFromELF(const Elf32& elf, const std::filesystem::path& libraryPath, const std::string& objectName)
{
    // Determine the source path (for archives, include member name)
    std::filesystem::path objectPath;
    
    if (!objectName.empty())
    {
		// For archive members, create a path like: /path/to/archive.a:member.o
        objectPath = libraryPath.string() + ":" + objectName;
    }
    else
    {
		// For direct ELF files, use the same path
        objectPath = libraryPath;
    }
    
    // Register the compilation unit
	core::CompilationUnit* unit = m_compilationUnitMgr->createCompilationUnit(
		core::CompilationUnitType::LibraryFile,
		libraryPath,
		objectPath
	);
    
    // Set up the compilation unit info
	unit->setTargetRegion(m_target->getMainRegion());
	
	// No need to setup BuildInfo
}

void LibraryAnalyzer::createUnitsFromArchive(const std::filesystem::path& archivePath)
{
	if (ncp::Application::isVerbose())
	{
    	Log::out << OINFO << ANSI_bYELLOW << "Analyzing archive: " << archivePath.filename().string() << ANSI_RESET << std::endl;
	}

    try
    {
        Archive archive;
        if (!archive.load(archivePath))
        {
            Log::out << OWARN << "Failed to load archive: " << archivePath.filename().string() << std::endl;
            return;
        }

        const auto& members = archive.getMembers();
        if (ncp::Application::isVerbose())
        {
            Log::out << OINFO << "Archive contains " << members.size() << " total members" << std::endl;
        }

        int processedCount = 0;
        int validCount = 0;

        // Process each object file in the archive
        for (const auto& memberPtr : members)
        {
            const ArMember& member = *memberPtr;
            processedCount++;

            // Skip non-object files
            if (!member.name.ends_with(".o"))
            {
                continue;
            }

            try
            {
                // Load ELF directly from memory
                Elf32 elf;
                if (!elf.loadFromMemory(member.data.data(), member.data.size()))
                {
                    continue;
                }

                // Create unit from this member
                createUnitFromELF(elf, archivePath, member.name);
                validCount++;
            }
            catch (const std::exception& e)
            {
                if (ncp::Application::isVerbose())
                {
                    Log::out << OWARN << "Error processing archive member " << member.name 
                             << ": " << e.what() << std::endl;
                }
            }
        }

        Log::out << OINFO << "Processed " << std::dec << validCount << " object files from " 
                 << archivePath.filename().string() << std::endl;
    }
    catch (const std::exception& e)
    {
        Log::out << OWARN << "Error analyzing archive " << archivePath.filename().string() 
                 << ": " << e.what() << std::endl;
    }
}
