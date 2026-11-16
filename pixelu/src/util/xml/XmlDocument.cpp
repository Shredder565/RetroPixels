// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/xml/XmlDocument.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <locale>

namespace rpxml
{
	// ---- node kinds the document keeps besides elements ----

	class XMLDocument::Text final : public XMLNode
	{
	  public:
		Text(XMLDocument* doc, std::string text) : XMLNode(doc), text(std::move(text)) {}
		std::string text;

	  private:
		void Write(std::string& out, int) const override;
	};

	// written back as it was: a declaration, comment or DOCTYPE
	class XMLDocument::Markup final : public XMLNode
	{
	  public:
		Markup(XMLDocument* doc, std::string raw) : XMLNode(doc), raw(std::move(raw)) {}
		std::string raw;
		XMLNode* ToDeclaration() override { return raw.compare(0, 5, "<?xml") == 0 ? this : nullptr; }

	  private:
		void Write(std::string& out, int depth) const override
		{
			out.append(depth * 4, ' ');
			out += raw;
			out += '\n';
		}
	};

	template<typename N, typename... A>
	N* XMLDocument::Make(A&&... args)
	{
		std::unique_ptr<N> node(new N(this, std::forward<A>(args)...));
		N* p = node.get();
		m_nodes.push_back(std::move(node));
		return p;
	}

	// ---- escaping ----

	namespace
	{
		void Escape(std::string& out, const std::string& s, bool attribute)
		{
			for (char c : s)
			{
				switch (c)
				{
				case '&': out += "&amp;"; break;
				case '<': out += "&lt;"; break;
				case '>': out += "&gt;"; break;
				case '"':
					if (attribute)
						out += "&quot;";
					else
						out += c;
					break;
				default: out += c;
				}
			}
		}

		void AppendUTF8(std::string& out, uint32_t cp)
		{
			if (cp < 0x80)
				out += (char)cp;
			else if (cp < 0x800)
			{
				out += (char)(0xc0 | cp >> 6);
				out += (char)(0x80 | (cp & 0x3f));
			}
			else if (cp < 0x10000)
			{
				out += (char)(0xe0 | cp >> 12);
				out += (char)(0x80 | (cp >> 6 & 0x3f));
				out += (char)(0x80 | (cp & 0x3f));
			}
			else
			{
				out += (char)(0xf0 | cp >> 18);
				out += (char)(0x80 | (cp >> 12 & 0x3f));
				out += (char)(0x80 | (cp >> 6 & 0x3f));
				out += (char)(0x80 | (cp & 0x3f));
			}
		}

		// Unescape turns entities into characters; unknown ones stay as written
		std::string Unescape(const char* p, const char* end)
		{
			std::string out;
			out.reserve(end - p);
			while (p < end)
			{
				if (*p != '&')
				{
					out += *p++;
					continue;
				}
				const char* semi = std::find(p, end, ';');
				const std::string name(p + 1, semi);
				static const std::pair<const char*, char> kNamed[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}};
				bool done = false;
				if (semi < end)
				{
					for (const auto& [n, c] : kNamed)
						if (name == n)
						{
							out += c;
							done = true;
						}
					if (!done && name.size() > 1 && name[0] == '#')
					{
						const bool hex = name[1] == 'x' || name[1] == 'X';
						char* stop = nullptr;
						const unsigned long cp = std::strtoul(name.c_str() + (hex ? 2 : 1), &stop, hex ? 16 : 10);
						if (*stop == 0 && cp <= 0x10ffff)
						{
							AppendUTF8(out, (uint32_t)cp);
							done = true;
						}
					}
				}
				if (done)
					p = semi + 1;
				else
					out += *p++;
			}
			return out;
		}

		bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
		bool IsNameChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == ':' || c == '-' || c == '.' || (unsigned char)c >= 0x80; }
	} // namespace

	// ---- reading ----

	class XMLDocument::Reader
	{
	  public:
		Reader(XMLDocument* doc, const char* text, size_t length) : m_doc(doc), m_p(text), m_end(text + length) {}

		// Content reads nodes into parent until the end or a closing tag
		bool Content(XMLNode* parent)
		{
			while (m_p < m_end)
			{
				if (*m_p != '<')
				{
					const char* start = m_p;
					m_p = std::find(m_p, m_end, '<');
					if (!std::all_of(start, m_p, IsSpace))
						parent->Adopt(m_doc->Make<Text>(Unescape(start, m_p)), false);
					continue;
				}
				if (Starts("</"))
					return true;
				if (Starts("<?") || Starts("<!--") || (Starts("<!") && !Starts("<![CDATA[")))
				{
					const char* close = Starts("<?") ? "?>" : Starts("<!--") ? "-->" : ">";
					const char* start = m_p;
					if (!SkipPast(close))
						return false;
					if (parent == m_doc)
						parent->Adopt(m_doc->Make<Markup>(std::string(start, m_p)), false);
					continue;
				}
				if (Starts("<![CDATA["))
				{
					const char* start = m_p + 9;
					if (!SkipPast("]]>"))
						return false;
					parent->Adopt(m_doc->Make<Text>(std::string(start, m_p - 3)), false);
					continue;
				}
				if (!Element(parent))
					return false;
			}
			return parent == m_doc;
		}

	  private:
		bool Starts(const char* s) const
		{
			const size_t n = std::strlen(s);
			return (size_t)(m_end - m_p) >= n && std::memcmp(m_p, s, n) == 0;
		}

		bool SkipPast(const char* s)
		{
			const size_t n = std::strlen(s);
			for (; m_p + n <= m_end; m_p++)
				if (std::memcmp(m_p, s, n) == 0)
				{
					m_p += n;
					return true;
				}
			return false;
		}

		void SkipSpace()
		{
			while (m_p < m_end && IsSpace(*m_p))
				m_p++;
		}

		std::string Name()
		{
			const char* start = m_p;
			while (m_p < m_end && IsNameChar(*m_p))
				m_p++;
			return std::string(start, m_p);
		}

		bool Element(XMLNode* parent)
		{
			m_p++; // '<'
			const std::string name = Name();
			if (name.empty())
				return false;
			XMLElement* e = m_doc->Make<XMLElement>(name);
			parent->Adopt(e, false);
			for (;;)
			{
				SkipSpace();
				if (m_p >= m_end)
					return false;
				if (Starts("/>"))
				{
					m_p += 2;
					return true;
				}
				if (*m_p == '>')
				{
					m_p++;
					break;
				}
				const std::string attr = Name();
				SkipSpace();
				if (attr.empty() || m_p >= m_end || *m_p != '=')
					return false;
				m_p++;
				SkipSpace();
				if (m_p >= m_end || (*m_p != '"' && *m_p != '\''))
					return false;
				const char quote = *m_p++;
				const char* close = std::find(m_p, m_end, quote);
				if (close == m_end)
					return false;
				e->m_attributes.emplace_back(attr, Unescape(m_p, close));
				m_p = close + 1;
			}
			if (!Content(e) || m_p >= m_end)
				return false;
			m_p += 2; // "</"
			if (Name() != name)
				return false;
			SkipSpace();
			if (m_p >= m_end || *m_p != '>')
				return false;
			m_p++;
			return true;
		}

		XMLDocument* m_doc;
		const char* m_p;
		const char* m_end;
	};

	void XMLDocument::Clear()
	{
		m_children.clear();
		m_nodes.clear();
	}

	XMLError XMLDocument::Parse(const char* text, size_t length)
	{
		Clear();
		if (text == nullptr || length == 0)
			return XML_ERROR_EMPTY_DOCUMENT;
		length = std::find(text, text + length, '\0') - text;
		// a UTF-8 byte order mark isn't content
		if (length >= 3 && std::memcmp(text, "\xEF\xBB\xBF", 3) == 0)
		{
			text += 3;
			length -= 3;
		}
		Reader reader(this, text, length);
		if (!reader.Content(this))
		{
			Clear();
			return XML_ERROR_PARSING;
		}
		return FirstChildElement() ? XML_SUCCESS : XML_ERROR_EMPTY_DOCUMENT;
	}

	XMLElement* XMLDocument::NewElement(const char* name) { return Make<XMLElement>(name); }

	XMLNode* XMLDocument::NewDeclaration() { return Make<Markup>("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"); }

	// ---- the tree ----

	void XMLNode::Adopt(XMLNode* node, bool atFront)
	{
		if (node->m_parent)
		{
			auto& siblings = node->m_parent->m_children;
			siblings.erase(std::find(siblings.begin(), siblings.end(), node));
		}
		node->m_parent = this;
		m_children.insert(atFront ? m_children.begin() : m_children.end(), node);
	}

	XMLNode* XMLNode::InsertEndChild(XMLNode* node)
	{
		Adopt(node, false);
		return node;
	}

	XMLNode* XMLNode::InsertFirstChild(XMLNode* node)
	{
		Adopt(node, true);
		return node;
	}

	XMLElement* XMLNode::FirstChildElement(const char* name) const
	{
		for (XMLNode* n : m_children)
			if (XMLElement* e = n->ToElement(); e && (!name || std::strcmp(e->Name(), name) == 0))
				return e;
		return nullptr;
	}

	XMLElement* XMLNode::NextSiblingElement(const char* name) const
	{
		if (!m_parent)
			return nullptr;
		const auto& siblings = m_parent->m_children;
		auto it = std::find(siblings.begin(), siblings.end(), this);
		for (++it; it != siblings.end(); ++it)
			if (XMLElement* e = (*it)->ToElement(); e && (!name || std::strcmp(e->Name(), name) == 0))
				return e;
		return nullptr;
	}

	const char* XMLElement::GetText() const
	{
		if (m_children.empty())
			return nullptr;
		if (auto* t = dynamic_cast<XMLDocument::Text*>(m_children.front()))
			return t->text.c_str();
		return nullptr;
	}

	void XMLElement::SetText(const char* text)
	{
		if (auto* t = m_children.empty() ? nullptr : dynamic_cast<XMLDocument::Text*>(m_children.front()))
		{
			t->text = text;
			return;
		}
		Adopt(m_doc->Make<XMLDocument::Text>(text), true);
	}

	const char* XMLElement::Attribute(const char* name, const char* value) const
	{
		for (const auto& [n, v] : m_attributes)
			if (n == name)
				return (!value || v == value) ? v.c_str() : nullptr;
		return nullptr;
	}

	void XMLElement::SetAttribute(const char* name, const char* value)
	{
		for (auto& [n, v] : m_attributes)
			if (n == name)
			{
				v = value;
				return;
			}
		m_attributes.emplace_back(name, value);
	}

	// ---- values ----

	bool XMLElement::ToBool(const char* s, bool otherwise)
	{
		if (!s)
			return otherwise;
		std::string v(s);
		std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (v == "1" || v == "true")
			return true;
		if (v == "0" || v == "false")
			return false;
		return otherwise;
	}

	float XMLElement::ToFloat(const char* s, float otherwise)
	{
		if (!s)
			return otherwise;
		std::istringstream in(s);
		in.imbue(std::locale::classic());
		float v;
		in >> std::ws >> v;
		return in.fail() ? otherwise : v;
	}

	double XMLElement::ToDouble(const char* s, double otherwise)
	{
		if (!s)
			return otherwise;
		std::istringstream in(s);
		in.imbue(std::locale::classic());
		double v;
		in >> std::ws >> v;
		return in.fail() ? otherwise : v;
	}

	int64_t XMLElement::ToInt64(const char* s, int64_t otherwise)
	{
		if (!s)
			return otherwise;
		while (IsSpace(*s))
			s++;
		const bool negative = *s == '-';
		const char* digits = (*s == '-' || *s == '+') ? s + 1 : s;
		const bool hex = digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
		char* stop = nullptr;
		errno = 0;
		const unsigned long long v = std::strtoull(hex ? digits + 2 : digits, &stop, hex ? 16 : 10);
		if (stop == (hex ? digits + 2 : digits) || errno == ERANGE)
			return otherwise;
		return negative ? -(int64_t)v : (int64_t)v;
	}

	uint64_t XMLElement::ToUInt64(const char* s, uint64_t otherwise)
	{
		return (uint64_t)ToInt64(s, (int64_t)otherwise);
	}

	std::string XMLElement::Format(float v)
	{
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out.precision(8);
		out << v;
		return out.str();
	}

	std::string XMLElement::Format(double v)
	{
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out.precision(17);
		out << v;
		return out.str();
	}

	// ---- writing ----
	//
	// One element per line, indented four spaces a level; an element whose
	// only child is text keeps it on its line.

	void XMLNode::WriteChildren(std::string& out, int depth) const
	{
		for (const XMLNode* n : m_children)
			n->Write(out, depth);
	}

	void XMLDocument::Text::Write(std::string& out, int) const { Escape(out, text, false); }

	void XMLElement::Write(std::string& out, int depth) const
	{
		out.append(depth * 4, ' ');
		out += '<';
		out += m_name;
		for (const auto& [n, v] : m_attributes)
		{
			out += ' ';
			out += n;
			out += "=\"";
			Escape(out, v, true);
			out += '"';
		}
		if (m_children.empty())
		{
			out += "/>\n";
			return;
		}
		out += '>';
		const bool inlineText = m_children.size() == 1 && GetText();
		if (inlineText)
			m_children.front()->Write(out, 0);
		else
		{
			out += '\n';
			for (const XMLNode* n : m_children)
			{
				if (n->ToElement() || !dynamic_cast<const XMLDocument::Text*>(n))
					n->Write(out, depth + 1);
				else
				{
					out.append((depth + 1) * 4, ' ');
					n->Write(out, 0);
					out += '\n';
				}
			}
			out.append(depth * 4, ' ');
		}
		out += "</";
		out += m_name;
		out += ">\n";
	}

	XMLError XMLDocument::SaveFile(FILE* file) const
	{
		std::string out;
		Write(out, 0);
		return std::fwrite(out.data(), 1, out.size(), file) == out.size() ? XML_SUCCESS : XML_ERROR_FILE_WRITE;
	}

	void XMLDocument::Print(XMLPrinter* printer) const
	{
		printer->m_text.clear();
		Write(printer->m_text, 0);
	}
} // namespace rpxml
