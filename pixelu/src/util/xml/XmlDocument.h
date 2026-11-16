// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// A small XML reader and writer for PixelU's settings and the system's XML
// files: elements, attributes and text, with declarations, comments, CDATA
// and the standard and numeric entities understood on reading.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace rpxml
{
	enum XMLError
	{
		XML_SUCCESS = 0,
		XML_ERROR_EMPTY_DOCUMENT,
		XML_ERROR_PARSING,
		XML_ERROR_FILE_WRITE,
	};

	class XMLDocument;
	class XMLElement;
	class XMLPrinter;

	// a node in the tree: the document, an element, text, or a declaration
	// or comment kept for writing back
	class XMLNode
	{
	  public:
		virtual ~XMLNode() = default;

		XMLNode* FirstChild() const { return m_children.empty() ? nullptr : m_children.front(); }
		XMLElement* FirstChildElement(const char* name = nullptr) const;
		XMLElement* NextSiblingElement(const char* name = nullptr) const;
		XMLNode* InsertEndChild(XMLNode* node);
		XMLNode* InsertFirstChild(XMLNode* node);
		virtual XMLElement* ToElement() { return nullptr; }
		virtual const XMLElement* ToElement() const { return nullptr; }
		// this node, if it is an <?xml ...?> declaration
		virtual XMLNode* ToDeclaration() { return nullptr; }

	  protected:
		friend class XMLDocument;
		friend class XMLElement;
		explicit XMLNode(XMLDocument* doc) : m_doc(doc) {}
		void Adopt(XMLNode* node, bool atFront);
		virtual void Write(std::string& out, int depth) const = 0;
		void WriteChildren(std::string& out, int depth) const;

		XMLDocument* m_doc;
		XMLNode* m_parent = nullptr;
		std::vector<XMLNode*> m_children; // owned by the document
	};

	class XMLElement final : public XMLNode
	{
	  public:
		const char* Name() const { return m_name.c_str(); }
		XMLElement* ToElement() override { return this; }
		const XMLElement* ToElement() const override { return this; }

		// the text, if the element's first child is text
		const char* GetText() const;
		void SetText(const char* text);

		// the attribute's value, or null; with value, only if it equals value
		const char* Attribute(const char* name, const char* value = nullptr) const;
		void SetAttribute(const char* name, const char* value);
		template<typename T>
		void SetAttribute(const char* name, const T& value)
		{
			if constexpr (std::is_convertible_v<const T&, const char*>)
				SetAttribute(name, static_cast<const char*>(value));
			else
				SetAttribute(name, Format(value).c_str());
		}

		bool BoolText(bool otherwise) const { return ToBool(GetText(), otherwise); }
		float FloatText(float otherwise) const { return ToFloat(GetText(), otherwise); }
		int IntText(int otherwise) const { return (int)ToInt64(GetText(), otherwise); }
		unsigned UnsignedText(unsigned otherwise) const { return (unsigned)ToUInt64(GetText(), otherwise); }
		int64_t Int64Text(int64_t otherwise) const { return ToInt64(GetText(), otherwise); }

		bool BoolAttribute(const char* name, bool otherwise) const { return ToBool(Attribute(name), otherwise); }
		float FloatAttribute(const char* name, float otherwise) const { return ToFloat(Attribute(name), otherwise); }
		int IntAttribute(const char* name, int otherwise) const { return (int)ToInt64(Attribute(name), otherwise); }
		unsigned UnsignedAttribute(const char* name, unsigned otherwise) const { return (unsigned)ToUInt64(Attribute(name), otherwise); }
		int64_t Int64Attribute(const char* name, int64_t otherwise) const { return ToInt64(Attribute(name), otherwise); }

	  private:
		friend class XMLDocument;
		XMLElement(XMLDocument* doc, std::string name) : XMLNode(doc), m_name(std::move(name)) {}
		void Write(std::string& out, int depth) const override;

		static bool ToBool(const char* s, bool otherwise);
		static float ToFloat(const char* s, float otherwise);
		static int64_t ToInt64(const char* s, int64_t otherwise);
		static uint64_t ToUInt64(const char* s, uint64_t otherwise);
		static std::string Format(bool v) { return v ? "true" : "false"; }
		static std::string Format(float v);
		static std::string Format(double v);
		template<typename T>
		static std::string Format(T v)
		{
			static_assert(std::is_integral_v<T>, "attribute values are strings, numbers or bools");
			return std::to_string(v);
		}

		std::string m_name;
		std::vector<std::pair<std::string, std::string>> m_attributes;
	};

	class XMLDocument final : public XMLNode
	{
	  public:
		XMLDocument() : XMLNode(this) {}
		XMLDocument(const XMLDocument&) = delete;
		XMLDocument& operator=(const XMLDocument&) = delete;

		XMLError Parse(const char* text, size_t length);
		XMLElement* NewElement(const char* name);
		// the <?xml version="1.0" encoding="UTF-8"?> line
		XMLNode* NewDeclaration();
		XMLError SaveFile(FILE* file) const;
		void Print(XMLPrinter* printer) const;

	  private:
		friend class XMLNode;
		friend class XMLElement;
		class Text;
		class Markup;
		class Reader;
		void Write(std::string& out, int depth) const override { WriteChildren(out, depth); }
		template<typename N, typename... A>
		N* Make(A&&... args);
		void Clear();

		std::vector<std::unique_ptr<XMLNode>> m_nodes; // every node made, for the document's lifetime
	};

	// XMLPrinter collects a document's text.
	class XMLPrinter
	{
	  public:
		const char* CStr() const { return m_text.c_str(); }

	  private:
		friend class XMLDocument;
		std::string m_text;
	};
} // namespace rpxml
