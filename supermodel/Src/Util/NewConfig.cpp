// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Util/NewConfig.h"
#include <iostream>
#include <sstream>

namespace Util
{
  namespace Config
  {
    void Node::RequireValue() const
    {
      if (m_missing)
        throw std::range_error(Util::Format() << "Node \"" << m_key << "\" does not exist");
      if (!m_value)
        throw std::logic_error(Util::Format() << "Node \"" << m_key << "\" has no value");
    }

    Node &Node::Placeholder(const std::string &key) const
    {
      auto found = m_placeholders.find(key);
      if (found != m_placeholders.end())
        return found->second;
      Node &made = m_placeholders.emplace(key, Node(key)).first->second;
      made.m_missing = true;
      return made;
    }

    template <typename NodeType>
    NodeType *Node::Find(NodeType *from, const std::string &path)
    {
      for (const std::string &part : Util::Format(path).Split('/'))
      {
        auto child = from->m_byKey.find(part);
        if (child == from->m_byKey.end())
          return nullptr;
        from = child->second.get();
      }
      return from;
    }

    Node &Node::operator[](const std::string &path)
    {
      // Like Find(), but on a miss hand back the placeholder for the first
      // missing part, kept by the deepest node that was found.
      Node *at = this;
      for (const std::string &part : Util::Format(path).Split('/'))
      {
        auto child = at->m_byKey.find(part);
        if (child == at->m_byKey.end())
          return at->Placeholder(part);
        at = child->second.get();
      }
      return *at;
    }

    const Node &Node::operator[](const std::string &path) const
    {
      return const_cast<Node &>(*this)[path];
    }

    Node &Node::Get(const std::string &path)
    {
      if (Node *found = TryGet(path))
        return *found;
      throw std::range_error(Util::Format() << "Node \"" << path << "\" does not exist");
    }

    const Node &Node::Get(const std::string &path) const
    {
      if (const Node *found = TryGet(path))
        return *found;
      throw std::range_error(Util::Format() << "Node \"" << path << "\" does not exist");
    }

    Node *Node::TryGet(const std::string &path)
    {
      return Find<Node>(this, path);
    }

    const Node *Node::TryGet(const std::string &path) const
    {
      return Find<const Node>(this, path);
    }

    void Node::Serialize(std::ostream *os, size_t indent_level) const
    {
      *os << std::string(2 * indent_level, ' ') << m_key << "=\"";
      if (m_value)
        m_value->Serialize(os);
      *os << "\"  children={";
      for (const auto &entry : m_byKey)
        *os << ' ' << entry.first;
      *os << " }" << std::endl;
      for (const Node &child : *this)
        child.Serialize(os, indent_level + 1);
    }

    std::string Node::ToString(size_t indent_level) const
    {
      std::ostringstream os;
      Serialize(&os, indent_level);
      return os.str();
    }

    Node &Node::AddEmpty(const std::string &path)
    {
      std::vector<std::string> parts = Util::Format(path).Split('/');
      Node *parent = this;
      for (size_t i = 0; i + 1 < parts.size(); i++)
      {
        auto existing = parent->m_byKey.find(parts[i]);
        if (existing != parent->m_byKey.end())
        {
          parent = existing->second.get();
        }
        else
        {
          Ptr made = std::make_shared<Node>(parts[i]);
          parent->Append(made);
          parent = made.get();
        }
      }
      Ptr leaf = std::make_shared<Node>(parts.back());
      parent->Append(leaf);
      return *leaf;
    }

    void Node::Append(const Ptr &child)
    {
      if (m_tail)
        m_tail->m_next = child;
      else
        m_head = child;
      m_tail = child;
      m_byKey[child->m_key] = child;
    }

    void Node::CopyFrom(const Node &that)
    {
      if (this == &that)
        return;
      // The copy stands alone: no value, children or sibling of its own.
      m_value.reset();
      m_next.reset();
      m_head.reset();
      m_tail.reset();
      m_byKey.clear();
      m_key = that.m_key;
      if (that.m_value)
        m_value = that.m_value->MakeCopy();
      for (const Node &child : that)
        Append(std::make_shared<Node>(child));
    }

    void Node::Exchange(Node &other)
    {
      // The placeholders and the missing mark stay with each node.
      m_key.swap(other.m_key);
      m_value.swap(other.m_value);
      m_next.swap(other.m_next);
      m_head.swap(other.m_head);
      m_tail.swap(other.m_tail);
      m_byKey.swap(other.m_byKey);
    }

    Node &Node::operator=(const Node &rhs)
    {
      CopyFrom(rhs);
      return *this;
    }

    Node &Node::operator=(Node &&rhs) noexcept
    {
      Exchange(rhs);
      return *this;
    }

    Node::Node()
      : m_key("config")
    {}

    Node::Node(const std::string &key)
      : m_key(key)
    {}

    Node::Node(const std::string &key, const std::string &value)
      : m_key(key),
        m_value(std::make_shared<ValueInstance<std::string>>(value))
    {}

    Node::Node(const Node &that)
    {
      CopyFrom(that);
    }

    Node::Node(Node &&that) noexcept
    {
      Exchange(that);
    }

    Node::~Node()
    {}

    void PrintConfigTree(const Node &config, int indent_level, int tab_stops)
    {
      int indent = tab_stops * indent_level;
      std::cout << std::string(indent > 0 ? size_t(indent) : 0, ' ') << config.Key();
      if (config.Exists())
        std::cout << " = " << config.ValueAs<std::string>();
      std::cout << std::endl;
      for (const Node &child : config)
        PrintConfigTree(child, indent_level + 1, tab_stops);
    }
  } // Config
} // Util
