// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <boost/container/small_vector.hpp>

#include "../fsc.h"

// A Wii U style path split into its names. '/' and '\' both separate names,
// repeated separators are collapsed, "." names are dropped (".." is kept as
// a name, nothing is resolved). Names compare ASCII case-insensitively.
class FSCPath
{
  public:
	FSCPath(std::string_view path)
	{
		size_t pos = 0;
		while (pos < path.size() && IsSeparator(path[pos]))
			pos++;
		while (pos < path.size())
		{
			size_t end = pos;
			while (end < path.size() && !IsSeparator(path[end]))
				end++;
			AddName(path.substr(pos, end - pos));
			pos = end;
			while (pos < path.size() && IsSeparator(path[pos]))
				pos++;
		}
	}

	size_t GetNodeCount() const
	{
		return m_names.size();
	}

	std::string_view GetNodeName(size_t index) const
	{
		if (index >= m_names.size())
			return {};
		return std::string_view(m_chars.data() + m_names[index].first, m_names[index].second);
	}

	static bool MatchNodeName(std::string_view name1, std::string_view name2)
	{
		if (name1.size() != name2.size())
			return false;
		for (size_t i = 0; i < name1.size(); i++)
		{
			if (FoldCase(name1[i]) != FoldCase(name2[i]))
				return false;
		}
		return true;
	}

	bool MatchNodeName(sint32 index, std::string_view name) const
	{
		if (index < 0 || (size_t)index >= m_names.size())
			return false;
		return MatchNodeName(GetNodeName((size_t)index), name);
	}

  private:
	static constexpr size_t kMaxNameLength = 0xFFFF;
	static constexpr size_t kMaxTotalLength = 0xFFFF; // further names are ignored once this much is stored

	static bool IsSeparator(char c)
	{
		return c == '/' || c == '\\';
	}

	static char FoldCase(char c)
	{
		return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}

	void AddName(std::string_view name)
	{
		if (m_chars.size() > kMaxTotalLength || name == ".")
			return;
		name = name.substr(0, kMaxNameLength);
		m_names.emplace_back((uint32)m_chars.size(), (uint32)name.size());
		m_chars.insert(m_chars.end(), name.begin(), name.end());
	}

	boost::container::small_vector<std::pair<uint32, uint32>, 8> m_names; // offset and length in m_chars
	boost::container::small_vector<char, 64> m_chars;
};

// In-memory tree of files, each carrying a pointer to user data. Directories
// are created implicitly by addFile. Entries keep the order they were added in.
template<typename F>
class FSAFileTree
{
	struct Node
	{
		std::string name;
		bool isFile{false};
		size_t fileSize{0};
		F* custom{nullptr};
		std::vector<std::unique_ptr<Node>> children;

		Node* FindChild(std::string_view childName)
		{
			for (auto& child : children)
			{
				if (FSCPath::MatchNodeName(child->name, childName))
					return child.get();
			}
			return nullptr;
		}
	};

	// iterates a directory node; the tree must outlive it
	class DirectoryIterator : public FSCVirtualFile
	{
	  public:
		explicit DirectoryIterator(const Node* dir) : m_dir(dir) {}

		sint32 fscGetType() override
		{
			return FSC_TYPE_DIRECTORY;
		}

		bool fscDirNext(FSCDirEntry* dirEntry) override
		{
			if (m_next >= m_dir->children.size())
				return false;
			const Node& child = *m_dir->children[m_next++];
			const size_t len = std::min(child.name.size(), sizeof(dirEntry->path) - 1);
			std::memcpy(dirEntry->path, child.name.data(), len);
			dirEntry->path[len] = '\0';
			dirEntry->isFile = child.isFile;
			dirEntry->isDirectory = !child.isFile;
			dirEntry->fileSize = child.isFile ? (uint32)child.fileSize : 0;
			return true;
		}

		bool fscRewindDir() override
		{
			m_next = 0;
			return true;
		}

	  private:
		const Node* m_dir;
		size_t m_next{0};
	};

	// follows the first 'depth' names of the path, optionally creating missing directories
	Node* Walk(const FSCPath& path, size_t depth, bool createMissing)
	{
		Node* node = &m_root;
		for (size_t i = 0; i < depth; i++)
		{
			Node* child = node->FindChild(path.GetNodeName(i));
			if (!child)
			{
				if (!createMissing)
					return nullptr;
				auto created = std::make_unique<Node>();
				created->name = path.GetNodeName(i);
				child = created.get();
				node->children.push_back(std::move(created));
			}
			node = child;
		}
		return node;
	}

  public:
	FSAFileTree() = default;

	// false if the path is empty or the name already exists in its directory
	bool addFile(std::string_view path, size_t fileSize, F* custom)
	{
		const FSCPath p(path);
		if (p.GetNodeCount() == 0)
			return false;
		Node* dir = Walk(p, p.GetNodeCount() - 1, true);
		const std::string_view name = p.GetNodeName(p.GetNodeCount() - 1);
		if (dir->FindChild(name))
			return false;
		auto file = std::make_unique<Node>();
		file->name = name;
		file->isFile = true;
		file->fileSize = fileSize;
		file->custom = custom;
		dir->children.push_back(std::move(file));
		return true;
	}

	bool getFile(std::string_view path, F*& custom)
	{
		const FSCPath p(path);
		if (p.GetNodeCount() == 0)
			return false;
		Node* node = Walk(p, p.GetNodeCount(), false);
		if (!node || !node->isFile)
			return false;
		custom = node->custom;
		return true;
	}

	// the caller owns the returned iterator; the tree root itself cannot be opened
	bool getDirectory(std::string_view path, FSCVirtualFile*& dirIterator)
	{
		const FSCPath p(path);
		if (p.GetNodeCount() == 0)
			return false;
		Node* node = Walk(p, p.GetNodeCount(), false);
		if (!node || node->isFile)
			return false;
		dirIterator = new DirectoryIterator(node);
		return true;
	}

	// removes a file node (not its user data)
	bool removeFile(std::string_view path)
	{
		const FSCPath p(path);
		if (p.GetNodeCount() == 0)
			return false;
		Node* dir = Walk(p, p.GetNodeCount() - 1, false);
		if (!dir)
			return false;
		const std::string_view name = p.GetNodeName(p.GetNodeCount() - 1);
		auto it = std::find_if(dir->children.begin(), dir->children.end(), [name](const std::unique_ptr<Node>& n) { return FSCPath::MatchNodeName(n->name, name); });
		if (it == dir->children.end() || !(*it)->isFile)
			return false;
		dir->children.erase(it);
		return true;
	}

	// calls fn(name, isDirectory, custom) for every entry of a directory
	template<typename TFunc>
	bool listDirectory(std::string_view path, TFunc fn)
	{
		const FSCPath p(path);
		Node* node = Walk(p, p.GetNodeCount(), false);
		if (!node || node->isFile)
			return false;
		for (auto& child : node->children)
			fn(child->name, !child->isFile, child->custom);
		return true;
	}

  private:
	Node m_root;
};

// self check of the path parser, run by FSTVolumeTest()
[[maybe_unused]] static void FSTPathUnitTest()
{
	struct Case
	{
		const char* path;
		std::vector<const char*> names;
	};
	const Case cases[] = {
		{"", {}},
		{"////", {}},
		{"/vol/content", {"vol", "content"}},
		{"vol\\content\\", {"VOL", "Content"}},
		{"//vol///content//\\", {"vol", "content"}},
		{"/vol/./code/../app.xml", {"vol", "code", "..", "APP.XML"}},
	};
	for (const Case& c : cases)
	{
		FSCPath p(c.path);
		cemu_assert_debug(p.GetNodeCount() == c.names.size());
		for (size_t i = 0; i < c.names.size() && i < p.GetNodeCount(); i++)
			cemu_assert_debug(p.MatchNodeName((sint32)i, c.names[i]));
		cemu_assert_debug(!p.MatchNodeName((sint32)c.names.size(), "x"));
	}
}
