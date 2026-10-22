// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The configuration tree. Each Node has a key, an optional value (see
// Util/GenericValue.h) and an ordered list of children. Paths name nested
// nodes with '/', so node["a/b"] is child "b" of child "a".
//
// - Children keep the order they were added in; the same key may be added
//   more than once, and lookups by key then find the most recently added.
// - operator[] never fails: a path that is not there gives a "missing"
//   placeholder (kept by the node, so the reference stays valid) on which
//   ValueAsDefault() returns its default, Exists() is false and Value(),
//   ValueAs() and SetValue() throw. Get() throws and TryGet() returns
//   nullptr instead.
// - An INI file is a tree whose root (key "Global") holds the global
//   settings and one child per section (see Util/ConfigBuilders.h).

#ifndef INCLUDED_UTIL_CONFIG_H
#define INCLUDED_UTIL_CONFIG_H

#include "Util/GenericValue.h"
#include <iterator>
#include <map>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Util
{
  namespace Config
  {
    class Node
    {
    public:
      // Walks a node's children in order.
      template <typename NodeType>
      class ChildIterator
      {
      public:
        ChildIterator(std::shared_ptr<NodeType> node = nullptr)
          : m_at(std::move(node))
        {}

        ChildIterator &operator++()
        {
          m_at = m_at->m_next;
          return *this;
        }

        ChildIterator operator++(int)
        {
          ChildIterator before = *this;
          ++*this;
          return before;
        }

        NodeType &operator*() const { return *m_at; }
        NodeType *operator->() const { return m_at.get(); }
        bool operator==(const ChildIterator &other) const { return m_at == other.m_at; }
        bool operator!=(const ChildIterator &other) const { return m_at != other.m_at; }

      private:
        std::shared_ptr<NodeType> m_at;
      };

      using iterator = ChildIterator<Node>;
      using const_iterator = ChildIterator<const Node>;

      iterator begin() { return iterator(m_head); }
      iterator end() { return iterator(); }
      const_iterator begin() const { return const_iterator(m_head); }
      const_iterator end() const { return const_iterator(); }

      const std::string &Key() const
      {
        return m_key;
      }

      std::shared_ptr<GenericValue> GetValue() const
      {
        return m_value;
      }

      // Removes the value (the node itself stays).
      void Clear()
      {
        m_value = nullptr;
      }

      // Shares 'value' with whoever else holds it.
      void SetValue(const std::shared_ptr<GenericValue> &value)
      {
        m_value = value;
      }

      template <typename T>
      const T &Value() const
      {
        RequireValue();
        return m_value->Value<T>();
      }

      template <typename T>
      T ValueAs() const
      {
        RequireValue();
        return m_value->ValueAs<T>();
      }

      template <typename T>
      T ValueAsDefault(const T &default_value) const
      {
        return m_value ? m_value->ValueAs<T>() : default_value;
      }

      // Stores 'value', in place if the node already holds a T (so a value
      // shared with another node changes there too), otherwise as a new
      // value of type T (which drops any value range).
      template <typename T>
      void SetValue(const T &value)
      {
        if (m_missing)
          throw std::range_error(Util::Format() << "Node \"" << m_key << "\" does not exist");
        if (m_value && m_value->Is<T>())
          m_value->Set(value);
        else
          m_value = std::make_shared<ValueInstance<T>>(value);
      }

      void SetValue(const char *value)
      {
        SetValue<std::string>(value);
      }

      // Adds a node at 'path'. Missing parents along the path are created;
      // existing ones are reused; the last part is always a new node, so the
      // same leaf can be added more than once.
      template <typename T>
      Node &Add(const std::string &path, const T &value)
      {
        Node &leaf = AddEmpty(path);
        leaf.SetValue(value);
        return leaf;
      }

      // Adds a node whose value is an empty string.
      Node &Add(const std::string &path)
      {
        return Add(path, std::string());
      }

      // Sets the value at 'key' if that node exists, else adds it.
      template <typename T>
      void Set(const std::string &key, const T &value)
      {
        if (Node *found = TryGet(key))
          found->SetValue(value);
        else
          Add(key, value);
      }

      // The same, and also gives the value its GUI group and range.
      template <typename T>
      void Set(const std::string &key, const T &value, const std::string &group, T _min = 0, T _max = 0, const std::vector<T> &list = std::vector<T>{})
      {
        Set(key, value);
        Node *node = TryGet(key);
        if constexpr (std::is_same<T, std::string>::value || std::is_same<T, const char *>::value)
          node->m_value->SetValueRange(std::make_shared<ValueRange>(group, list));
        else
          node->m_value->SetValueRange(std::make_shared<ValueRange>(group, _min, _max, list));
      }

      // Removes the value at 'key', adding a value-less node if needed.
      void SetEmpty(const std::string &key)
      {
        if (Node *found = TryGet(key))
          found->Clear();
        else
          AddEmpty(key);
      }

      bool Empty() const { return !m_value; }
      bool Exists() const { return !Empty(); }
      bool IsLeaf() const { return m_byKey.empty(); }
      bool HasChildren() const { return !IsLeaf(); }

      // Never fails (see above).
      Node &operator[](const std::string &path);
      const Node &operator[](const std::string &path) const;

      // Throw std::range_error if there is no such node.
      Node &Get(const std::string &path);
      const Node &Get(const std::string &path) const;

      // nullptr if there is no such node.
      Node *TryGet(const std::string &path);
      const Node *TryGet(const std::string &path) const;

      // Debug dump: one line per node, children indented by two spaces.
      void Serialize(std::ostream *os, size_t indent_level = 0) const;
      std::string ToString(size_t indent_level = 0) const;

      // Copy assignment makes this node a deep copy of 'rhs' (and detaches
      // it from its siblings); move assignment exchanges the two nodes'
      // keys, values and children.
      Node &operator=(const Node &rhs);
      Node &operator=(Node &&rhs) noexcept;

      Node(const std::string &key);
      Node(const std::string &key, const std::string &value);
      Node(const Node &that);
      Node(Node &&that) noexcept;
      ~Node();

    private:
      using Ptr = std::shared_ptr<Node>;

      Node(); // not for general use

      void RequireValue() const;
      Node &Placeholder(const std::string &key) const;
      Node &AddEmpty(const std::string &path);
      void Append(const Ptr &child);
      void CopyFrom(const Node &that);
      void Exchange(Node &other);

      template <typename NodeType>
      static NodeType *Find(NodeType *from, const std::string &path);

      std::string m_key;
      std::shared_ptr<GenericValue> m_value;          // null: no value
      Ptr m_next;                                     // next sibling
      Ptr m_head;                                     // first child
      Ptr m_tail;                                     // last child
      std::map<std::string, Ptr> m_byKey;             // latest child per key
      mutable std::map<std::string, Node> m_placeholders; // for failed operator[] lookups
      bool m_missing = false;                         // this is such a placeholder
    };

    // Prints the tree to stdout as "key = value" lines.
    void PrintConfigTree(const Node &config, int indent_level = 0, int tab_stops = 2);
  } // Config
} // Util

#endif // INCLUDED_UTIL_CONFIG_H
