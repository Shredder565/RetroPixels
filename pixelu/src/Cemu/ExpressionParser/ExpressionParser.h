// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Common/precompiled.h"

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <charconv>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include <boost/functional/hash_fwd.hpp>
#include <fmt/format.h>

#ifdef __clang__
#include "Common/unix/fast_float.h"
#endif

// Arithmetic expression evaluator used by graphic packs (rules.txt values,
// preset conditions, patch operands), the PPC assembler and the debugger.
//
// Grammar, as graphic packs expect it:
//  - numbers: decimal ("12", "1.5", "2e3"), "0x" hex, and a leading 0 followed
//    by a hex digit is hex too ("01C" = 0x1C) unless the rest of the
//    expression contains a '.'; a '+'/'-' where an operand is expected is the
//    number's sign
//  - names: [A-Za-z_$][A-Za-z0-9_@.]*, looked up in the constant table, then
//    through the constant callback; "name(" calls the function callback
//  - binary operators (lowest to highest): == != < <= > >=, then + -, then
//    * / %, then ^ (right associative); x/0 and x%0 give 0
//  - one address suffix (@ha @hi @h @lo @l) anywhere applies to the final
//    result; the same suffix may repeat, two different ones are an error
// Values are computed in TType; literals are read as double first.
template<class TType = double>
class TExpressionParser
{
public:
	static_assert(std::is_arithmetic_v<TType>);
	using ConstantCallback_t = TType(*)(std::string_view var_name);
	using FunctionCallback_t = TType(*)(std::string_view var_name, TType parameter);

	// 16-bit halves of a 32-bit address, as used by PPC lis/addi pairs
	static TType ExpressionFuncHA(TType input)
	{
		const uint32 v = (uint32)input;
		return (TType)(uint32)(((v + 0x8000u) >> 16) & 0xFFFFu);
	}

	static TType ExpressionFuncHI(TType input)
	{
		const uint32 v = (uint32)input;
		return (TType)(uint32)(v >> 16);
	}

	static TType ExpressionFuncLO(TType input)
	{
		const uint32 v = (uint32)input;
		return (TType)(uint32)(v & 0xFFFFu);
	}

	template<typename T>
	T Evaluate(std::string_view expression) const
	{
		static_assert(std::is_arithmetic_v<T>, "type T must be an arithmetic type");
		return (T)Evaluate(expression);
	}

	[[nodiscard]] TType Evaluate(std::string_view expression) const
	{
		if (expression.empty())
			throw std::runtime_error("empty expression is not allowed");
		std::vector<Item> program;
		AddrPart part = AddrPart::Whole;
		Translate(expression, program, part);
		std::vector<TType> values;
		for (const Item& item : program)
			Execute(item, values);
		if (values.empty())
			throw std::runtime_error("empty expression is not allowed");
		TType& result = values.back();
		switch (part)
		{
		case AddrPart::High: result = ExpressionFuncHI(result); break;
		case AddrPart::HighAdjusted: result = ExpressionFuncHA(result); break;
		case AddrPart::Low: result = ExpressionFuncLO(result); break;
		default: break;
		}
		return result;
	}

	// syntax check: every name resolves to 1, no functions
	[[nodiscard]] bool IsValidExpression(std::string_view expression) const
	{
		TExpressionParser<TType> probe;
		probe.AddConstantCallback([](std::string_view) -> TType { return (TType)1; });
		return probe.Succeeds(expression);
	}

	// can be evaluated with the constants and callbacks set on this parser
	[[nodiscard]] bool IsConstantExpression(std::string_view expression) const
	{
		return Succeeds(expression);
	}

	void AddConstant(std::string_view name, TType value)
	{
		m_constants[std::string(name)] = value;
	}

	// keeps an existing value
	void TryAddConstant(std::string_view name, TType value)
	{
		m_constants.try_emplace(std::string(name), value);
	}

	void AddConstant(std::string_view name, std::string_view value)
	{
		AddConstant(name, (TType)ReadLiteral(value, nullptr));
	}

	void AddConstantCallback(ConstantCallback_t callback)
	{
		m_constant_callback = callback;
	}

	void SetFunctionCallback(FunctionCallback_t callback)
	{
		m_function_callback = callback;
	}

private:
	enum class AddrPart
	{
		Whole,
		High,         // @h, @hi
		HighAdjusted, // @ha
		Low,          // @l, @lo
	};

	enum class Op
	{
		Add, Sub, Mul, Div, Pow, Mod,
		Eq, Ne, Lt, Le, Gt, Ge,
	};

	enum class ItemKind
	{
		Value,
		Binary,
		Call,
		OpenParen, // only on the pending stack
	};

	struct Item
	{
		ItemKind kind;
		TType value{};
		Op op{};
		int level = 0;
		bool rightToLeft = false;
		std::string_view name;
	};

	std::unordered_map<std::string, TType> m_constants;
	ConstantCallback_t m_constant_callback = nullptr;
	FunctionCallback_t m_function_callback = nullptr;

	bool Succeeds(std::string_view expression) const
	{
		try
		{
			static_cast<void>(Evaluate(expression));
		}
		catch (...)
		{
			return false;
		}
		return true;
	}

	static bool IsSpaceChar(char c) { return std::isblank((unsigned char)c) != 0; }
	static bool IsDigitChar(char c) { return std::isdigit((unsigned char)c) != 0; }
	static bool IsHexChar(char c) { return std::isxdigit((unsigned char)c) != 0; }
	static bool IsLetterChar(char c) { return std::isalpha((unsigned char)c) != 0; }
	static char Lower(char c) { return (char)std::tolower((unsigned char)c); }

	static void SetAddrPart(AddrPart& current, AddrPart wanted)
	{
		if (current == wanted)
			return;
		if (current != AddrPart::Whole)
			throw std::runtime_error("Mismatching relocation modifiers (suffix @..) in expression");
		current = wanted;
	}

	// Reads an @suffix at the start of text. Returns how many characters it
	// covers (0 = none). An unknown suffix throws unless lenient is set.
	static size_t ReadAddrSuffix(std::string_view text, AddrPart& current, bool lenient)
	{
		if (text.size() < 2 || text[0] != '@')
			return 0;
		const char first = Lower(text[1]);
		const char second = text.size() >= 3 ? Lower(text[2]) : '\0';
		if (first == 'h' && second == 'a')
		{
			SetAddrPart(current, AddrPart::HighAdjusted);
			return 3;
		}
		if (first == 'h' && second == 'i')
		{
			SetAddrPart(current, AddrPart::High);
			return 3;
		}
		if (first == 'l' && second == 'o')
		{
			SetAddrPart(current, AddrPart::Low);
			return 3;
		}
		if (first == 'h' || first == 'l')
		{
			SetAddrPart(current, first == 'h' ? AddrPart::High : AddrPart::Low);
			return 2;
		}
		if (!lenient)
			throw std::runtime_error(fmt::format("Unknown relocation modifier (only @lo, @hi, @ha, @l, @h are supported) at: {}", text));
		return 0;
	}

	static std::from_chars_result ParseDouble(const char* first, const char* last, double& out)
	{
#ifdef __clang__
		const fast_float::from_chars_result r = fast_float::from_chars(first, last, out);
		std::from_chars_result conv;
		conv.ptr = r.ptr;
		conv.ec = r.ec;
		return conv;
#else
		return std::from_chars(first, last, out);
#endif
	}

	// Reads a numeric literal (with optional sign) from the start of text.
	// *consumed receives the number of characters used.
	static double ReadLiteral(std::string_view text, size_t* consumed)
	{
		if (text.empty())
			throw std::runtime_error("can't parse empty number");
		const char* const start = text.data();
		std::string_view body = text;
		bool negate = false;
		if (body[0] == '-' || body[0] == '+')
		{
			negate = body[0] == '-';
			body.remove_prefix(1);
		}

		double value = 0.0;
		std::from_chars_result res{};
		size_t hexPrefix = 0;
		if (body.size() >= 3 && body[0] == '0' && Lower(body[1]) == 'x')
			hexPrefix = 2;
		else if (body.size() >= 2 && body[0] == '0' && IsHexChar(body[1]) && body.find('.') == std::string_view::npos)
			hexPrefix = 1;

		if (hexPrefix != 0)
		{
			body.remove_prefix(hexPrefix);
			size_t len = 0;
			while (len < body.size() && IsHexChar(body[len]))
				len++;
			int64_t integer = 0;
			res = std::from_chars(body.data(), body.data() + len, integer, 16);
			value = (double)integer;
		}
		else
		{
			if (!body.empty() && body[0] == '+')
				body.remove_prefix(1);
			// digits with at most one '.' and at most one 'e' anywhere among them
			size_t len = 0;
			bool sawDot = false, sawExp = false;
			for (; len < body.size(); len++)
			{
				const char c = body[len];
				if (IsDigitChar(c))
					continue;
				if (c == '.' && !sawDot)
				{
					sawDot = true;
					continue;
				}
				if (Lower(c) == 'e' && !sawExp)
				{
					sawExp = true;
					continue;
				}
				break;
			}
			res = ParseDouble(body.data(), body.data() + len, value);
		}

		if (res.ec != std::errc())
			throw std::runtime_error(fmt::format("can't parse number: {}", body));
		if (consumed)
			*consumed = (size_t)(res.ptr - start);
		return negate ? value * -1.0 : value;
	}

	static Item MakeBinary(Op op, int level, bool rightToLeft = false)
	{
		Item item{ ItemKind::Binary };
		item.op = op;
		item.level = level;
		item.rightToLeft = rightToLeft;
		return item;
	}

	TType LookupName(std::string_view name, std::string_view expression) const
	{
		if (auto it = m_constants.find(std::string(name)); it != m_constants.end())
			return it->second;
		if (!m_constant_callback)
			throw std::runtime_error(fmt::format("unknown constant found \"{}\" in expression: {}", name, expression));
		return m_constant_callback(name);
	}

	// Converts the text to postfix order (shunting-yard). A pending function
	// call stays on the pending stack after its ')' and is only moved to the
	// output by a later ')' or at the end, so it also takes in any binary
	// operators that follow it on its level ("f(2)*3" calls f with 6).
	void Translate(std::string_view expression, std::vector<Item>& program, AddrPart& part) const
	{
		std::vector<Item> pending;
		bool wantOperand = true;
		size_t pos = 0;
		const size_t size = expression.size();
		while (pos < size)
		{
			const char c = expression[pos];
			if (IsSpaceChar(c))
			{
				pos++;
				continue;
			}

			if (IsDigitChar(c) || (wantOperand && (c == '+' || c == '-')))
			{
				size_t used = 0;
				const double literal = ReadLiteral(expression.substr(pos), &used);
				Item item{ ItemKind::Value };
				item.value = (TType)literal;
				program.push_back(item);
				pos += used;
				wantOperand = false;
				pos += ReadAddrSuffix(expression.substr(pos), part, false);
				continue;
			}

			if (IsLetterChar(c) || c == '_' || c == '$')
			{
				size_t end = pos + 1;
				while (end < size)
				{
					const char n = expression[end];
					if (!IsLetterChar(n) && !IsDigitChar(n) && n != '_' && n != '@' && n != '.')
						break;
					end++;
				}
				const size_t wordLen = end - pos;
				std::string_view name = expression.substr(pos, wordLen);
				const size_t at = name.rfind('@');
				if (at != std::string_view::npos)
					name.remove_suffix(ReadAddrSuffix(name.substr(at), part, true));

				if (m_function_callback && end < size && expression[end] == '(')
				{
					Item call{ ItemKind::Call };
					call.name = name;
					pending.push_back(call);
					pending.push_back(Item{ ItemKind::OpenParen });
					pos = end + 1;
					wantOperand = true;
					continue;
				}

				Item item{ ItemKind::Value };
				item.value = LookupName(name, expression);
				program.push_back(item);
				pos = end;
				wantOperand = false;
				continue;
			}

			if (c == '(')
			{
				pending.push_back(Item{ ItemKind::OpenParen });
				pos++;
				wantOperand = true;
				continue;
			}

			if (c == ')')
			{
				// note: does not change wantOperand
				bool closed = false;
				while (!pending.empty())
				{
					const Item top = pending.back();
					pending.pop_back();
					if (top.kind == ItemKind::OpenParen)
					{
						closed = true;
						break;
					}
					program.push_back(top);
				}
				if (!closed)
					throw std::runtime_error(fmt::format("parentheses mismatch: {}", expression));
				pos++;
				continue;
			}

			const char next = pos + 1 < size ? expression[pos + 1] : '\0';
			Item binary{ ItemKind::Value };
			bool known = true;
			switch (c)
			{
			case '+': binary = MakeBinary(Op::Add, 2); break;
			case '-': binary = MakeBinary(Op::Sub, 2); break;
			case '*': binary = MakeBinary(Op::Mul, 3); break;
			case '/': binary = MakeBinary(Op::Div, 3); break;
			case '%': binary = MakeBinary(Op::Mod, 3); break;
			case '^': binary = MakeBinary(Op::Pow, 4, true); break;
			case '<':
				binary = MakeBinary(next == '=' ? Op::Le : Op::Lt, 1);
				pos += next == '=' ? 1 : 0;
				break;
			case '>':
				binary = MakeBinary(next == '=' ? Op::Ge : Op::Gt, 1);
				pos += next == '=' ? 1 : 0;
				break;
			case '=':
			case '!':
				known = next == '=';
				if (known)
				{
					binary = MakeBinary(c == '=' ? Op::Eq : Op::Ne, 1);
					pos++;
				}
				break;
			default:
				known = false;
				break;
			}
			if (!known)
				throw std::runtime_error(fmt::format("invalid operator found in expression: {}", expression));

			while (!pending.empty() && pending.back().kind == ItemKind::Binary)
			{
				const int topLevel = pending.back().level;
				const bool yields = binary.rightToLeft ? (binary.level < topLevel) : (binary.level <= topLevel);
				if (!yields)
					break;
				program.push_back(pending.back());
				pending.pop_back();
			}
			pending.push_back(binary);
			pos++;
			wantOperand = true;
		}

		while (!pending.empty())
		{
			if (pending.back().kind == ItemKind::OpenParen)
				throw std::runtime_error(fmt::format("parentheses mismatch in expression: {}", expression));
			program.push_back(pending.back());
			pending.pop_back();
		}
	}

	static bool NearlyEqual(TType a, TType b)
	{
		if constexpr (std::is_floating_point_v<TType>)
			return std::abs(a - b) <= 0.0000000001;
		else
			return a == b;
	}

	static bool ClearlyDifferent(TType a, TType b)
	{
		if constexpr (std::is_floating_point_v<TType>)
			return std::abs(a - b) > 0.0000000001;
		else
			return a != b;
	}

	static TType Combine(Op op, TType a, TType b)
	{
		switch (op)
		{
		case Op::Add: return a + b;
		case Op::Sub: return a - b;
		case Op::Mul: return a * b;
		case Op::Div: return b == 0.0 ? (TType)0.0 : a / b;
		case Op::Pow: return (TType)std::pow(a, b);
		case Op::Mod:
		{
			if (std::round(b) == 0.0)
				return (TType)0.0;
			const sint64 dividend = (sint64)std::round(a);
			const sint64 divisor = (sint64)std::round(b);
			return (TType)(dividend % divisor);
		}
		case Op::Eq: return (TType)(NearlyEqual(a, b) ? 1 : 0);
		case Op::Ne: return (TType)(ClearlyDifferent(a, b) ? 1 : 0);
		case Op::Lt: return (TType)(a < b ? 1 : 0);
		case Op::Le: return (TType)(a <= b ? 1 : 0);
		case Op::Gt: return (TType)(a > b ? 1 : 0);
		case Op::Ge: return (TType)(a >= b ? 1 : 0);
		}
		throw std::runtime_error("unsupported operation constant");
	}

	void Execute(const Item& item, std::vector<TType>& values) const
	{
		if (item.kind == ItemKind::Value)
		{
			values.push_back(item.value);
			return;
		}
		const size_t needed = item.kind == ItemKind::Call ? 1 : 2;
		if (values.size() < needed)
			throw std::runtime_error("not enough parameters for equation");
		if (item.kind == ItemKind::Call)
		{
			values.back() = m_function_callback(item.name, values.back());
			return;
		}
		const TType b = values.back();
		values.pop_back();
		values.back() = Combine(item.op, values.back(), b);
	}
};

class ExpressionParser : public TExpressionParser<double> {};

void ExpressionParser_test();
