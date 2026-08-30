#include "glob.hpp"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace Glob {

namespace {

std::string normalize(std::string_view pattern)
{
	std::string out(pattern);
	std::replace(out.begin(), out.end(), '\\', '/');
	return out;
}

std::vector<std::string_view> split(std::string_view path)
{
	std::vector<std::string_view> segments;
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string_view::npos)
		{
			if (start < path.size())
				segments.push_back(path.substr(start));
			break;
		}
		if (end != start) // collapse '//' and ignore a trailing '/'
			segments.push_back(path.substr(start, end - start));
		start = end + 1;
	}
	return segments;
}

/// Match a bracket expression starting at pattern[i] (which is '['), against c.
/// On success advances i past the closing ']'. On a malformed class the '[' is
/// treated as a literal, matching shell behavior.
bool matchClass(std::string_view pat, size_t& i, char c)
{
	size_t j = i + 1;
	bool negate = false;
	if (j < pat.size() && (pat[j] == '!' || pat[j] == '^'))
	{
		negate = true;
		j++;
	}

	bool matched = false;
	bool first = true;
	for (; j < pat.size(); j++)
	{
		if (pat[j] == ']' && !first)
		{
			i = j + 1;
			return matched != negate;
		}
		first = false;

		// A range needs a '-' that is neither the first nor the last element.
		if (j + 2 < pat.size() && pat[j + 1] == '-' && pat[j + 2] != ']')
		{
			if (c >= pat[j] && c <= pat[j + 2])
				matched = true;
			j += 2;
			continue;
		}

		if (pat[j] == c)
			matched = true;
	}

	return false; // unterminated '[' -> caller falls back to literal
}

/// Wildcard match within a single path segment, with backtracking for '*'.
bool matchSegment(std::string_view pat, std::string_view name)
{
	size_t p = 0, n = 0;
	size_t starPat = std::string_view::npos, starName = 0;

	while (n < name.size())
	{
		if (p < pat.size() && pat[p] == '*')
		{
			starPat = p++;
			starName = n;
			continue;
		}

		bool ok = false;
		if (p < pat.size())
		{
			if (pat[p] == '?')
			{
				ok = true;
				p++;
			}
			else if (pat[p] == '[')
			{
				size_t save = p;
				if (matchClass(pat, p, name[n]))
				{
					ok = true;
				}
				else if (p == save) // malformed class: literal '['
				{
					ok = (pat[p] == name[n]);
					if (ok) p++;
				}
			}
			else
			{
				ok = (pat[p] == name[n]);
				if (ok) p++;
			}
		}

		if (ok)
		{
			n++;
			continue;
		}

		if (starPat != std::string_view::npos)
		{
			p = starPat + 1;
			n = ++starName;
			continue;
		}
		return false;
	}

	while (p < pat.size() && pat[p] == '*')
		p++;
	return p == pat.size();
}

bool matchSegments(const std::vector<std::string_view>& pat, size_t pi,
                   const std::vector<std::string_view>& path, size_t si)
{
	while (pi < pat.size())
	{
		if (pat[pi] == "**")
		{
			// Collapse consecutive '**' segments.
			while (pi + 1 < pat.size() && pat[pi + 1] == "**")
				pi++;
			if (pi + 1 == pat.size())
				return true; // trailing '**' absorbs the remainder
			for (size_t skip = si; skip <= path.size(); skip++)
			{
				if (matchSegments(pat, pi + 1, path, skip))
					return true;
			}
			return false;
		}

		if (si >= path.size())
			return false;
		if (!matchSegment(pat[pi], path[si]))
			return false;
		pi++;
		si++;
	}
	return si == path.size();
}

/// Longest leading run of segments containing no metacharacters.
std::string literalPrefix(const std::vector<std::string_view>& segments)
{
	std::string prefix;
	for (std::string_view segment : segments)
	{
		if (hasWildcard(segment))
			break;
		if (!prefix.empty())
			prefix += '/';
		prefix += segment;
	}
	return prefix;
}

} // namespace

bool hasWildcard(std::string_view pattern)
{
	return pattern.find_first_of("*?[{") != std::string_view::npos;
}

std::vector<std::string> expandBraces(std::string_view pattern)
{
	size_t open = std::string_view::npos;
	int depth = 0;
	for (size_t i = 0; i < pattern.size(); i++)
	{
		if (pattern[i] == '{')
		{
			if (depth++ == 0)
				open = i;
		}
		else if (pattern[i] == '}' && depth > 0)
		{
			if (--depth == 0)
			{
				// Split the alternation on top-level commas.
				std::vector<std::string> out;
				std::string_view head = pattern.substr(0, open);
				std::string_view tail = pattern.substr(i + 1);
				std::string_view body = pattern.substr(open + 1, i - open - 1);

				size_t start = 0;
				int inner = 0;
				for (size_t j = 0; j <= body.size(); j++)
				{
					if (j == body.size() || (body[j] == ',' && inner == 0))
					{
						std::string combined;
						combined += head;
						combined += body.substr(start, j - start);
						combined += tail;
						for (std::string& sub : expandBraces(combined))
							out.push_back(std::move(sub));
						start = j + 1;
					}
					else if (body[j] == '{') inner++;
					else if (body[j] == '}') inner--;
				}
				return out;
			}
		}
	}
	return { std::string(pattern) };
}

bool match(std::string_view pattern, std::string_view path)
{
	std::string np = normalize(pattern);
	std::string ns = normalize(path);
	std::vector<std::string_view> pathSegments = split(ns);

	if (np.find('{') != std::string::npos)
	{
		for (const std::string& single : expandBraces(np))
		{
			std::vector<std::string_view> patSegments = split(single);
			if (matchSegments(patSegments, 0, pathSegments, 0))
				return true;
		}
		return false;
	}

	std::vector<std::string_view> patSegments = split(np);
	return matchSegments(patSegments, 0, pathSegments, 0);
}

std::vector<fs::path> expand(std::string_view pattern, const fs::path& baseDir, const Options& options)
{
	std::vector<fs::path> out;

	// baseDir only says where to start looking. Results come back exactly as the
	// pattern wrote them, so a relative pattern yields relative paths: object
	// file paths are derived from the source path, and absolutising them here
	// would relocate build output.
	const fs::path root = baseDir.empty() ? fs::path(".") : baseDir;

	auto emit = [&](const fs::path& asWritten) { out.push_back(asWritten); };

	for (const std::string& single : expandBraces(normalize(pattern)))
	{
		std::error_code ec;

		if (!hasWildcard(single))
		{
			// Literal path: preserve the historical non-glob behavior.
			const fs::path literal = fs::path(single);
			const fs::path probe = literal.is_absolute() ? literal : root / literal;
			if (!fs::exists(probe, ec))
				continue;

			if (options.directoriesOnly)
			{
				if (fs::is_directory(probe, ec))
					emit(literal);
			}
			else if (fs::is_regular_file(probe, ec))
			{
				emit(literal);
			}
			else if (fs::is_directory(probe, ec))
			{
				std::vector<fs::path> names;
				for (const auto& entry : fs::directory_iterator(probe, ec))
				{
					if (entry.is_regular_file(ec))
						names.push_back(entry.path().filename());
				}
				std::sort(names.begin(), names.end());
				for (const fs::path& name : names)
					emit(literal / name);
			}
			continue;
		}

		const std::vector<std::string_view> segments = split(single);
		std::string prefix = literalPrefix(segments);
		// split() drops the leading empty segment of an absolute pattern, so
		// restore the root here or the prefix silently becomes relative.
		if (!single.empty() && single.front() == '/')
			prefix.insert(prefix.begin(), '/');
		const fs::path patternRoot = prefix.empty() ? fs::path() : fs::path(prefix);
		const fs::path walkRoot = patternRoot.empty()
			? root
			: (patternRoot.is_absolute() ? patternRoot : root / patternRoot);

		if (!fs::is_directory(walkRoot, ec))
			continue;

		auto iter = fs::recursive_directory_iterator(walkRoot, ec);
		if (ec)
			continue;

		for (const auto& entry : iter)
		{
			const bool isDir = entry.is_directory(ec);
			if (options.directoriesOnly ? !isDir : !entry.is_regular_file(ec))
				continue;

			// Rebuild the path as written in the pattern, so matching is done
			// against the pattern's own frame of reference.
			const fs::path tail = fs::relative(entry.path(), walkRoot, ec);
			if (ec)
				continue;
			const fs::path asWritten = patternRoot.empty() ? tail : patternRoot / tail;

			if (match(single, asWritten.generic_string()))
				emit(asWritten);
		}
	}

	// Deterministic order: directory_iterator's order is unspecified, so
	// without this the link order (and therefore the output) can vary.
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

} // namespace Glob
