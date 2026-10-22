// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "ExpressionParser.h"

namespace
{
	template<typename TResult, typename TCompute = double>
	TResult EvalAs(const char* text)
	{
		TExpressionParser<TCompute> parser;
		return (TResult)parser.Evaluate(text);
	}
}

// self-check run from UnitTests() in debug builds
void ExpressionParser_test()
{
	// literals and signs
	cemu_assert_debug(EvalAs<sint64>("0x4A0") == 0x4A0);
	cemu_assert_debug(EvalAs<sint64>("0x1+0x2+0x4") == 7);
	cemu_assert_debug(EvalAs<sint64>("1+-6") == -5);
	cemu_assert_debug(EvalAs<sint64>("0x10-0x11") == -1);
	cemu_assert_debug(EvalAs<sint64>("0A+1") == 11);  // leading 0 means hex
	cemu_assert_debug(EvalAs<sint64>("010+1.5") == 11); // ... unless a '.' follows somewhere
	cemu_assert_debug(EvalAs<sint64>("+7") == 7);
	cemu_assert_debug(EvalAs<sint64>("2*-3") == -6);

	// double vs integer arithmetic
	cemu_assert_debug(EvalAs<sint64>("(7 / 2) * 4") == 14);
	cemu_assert_debug(EvalAs<sint64, sint64>("(7 / 2) * 4") == 12);
	cemu_assert_debug(EvalAs<sint64>("9 % 4") == 1);
	cemu_assert_debug(EvalAs<sint64>("5 / 0") == 0);
	cemu_assert_debug(EvalAs<sint64>("2 ^ 3 ^ 2") == 512);

	// comparisons evaluate left to right
	cemu_assert_debug(EvalAs<float>("3 >= 3") == 1.0f);
	cemu_assert_debug(EvalAs<float>("2 < 1") == 0.0f);
	cemu_assert_debug(EvalAs<float>("4 > 3 > 2") == 0.0f);
	cemu_assert_debug(EvalAs<float>("4 > 3 > -1") == 1.0f);
	cemu_assert_debug(EvalAs<float>("(1 != 2) == 1") == 1.0f);

	// address suffixes apply to the whole result
	ExpressionParser parser;
	parser.AddConstant("base", 0x1234ABCD);
	cemu_assert_debug(parser.Evaluate("base@ha") == 0x1235);
	cemu_assert_debug(parser.Evaluate("base+0x10@l") == 0xABDD);
	cemu_assert_debug(parser.Evaluate("base@h") == 0x1234);
	cemu_assert_debug(parser.IsValidExpression("base@ha + base@ha"));
	cemu_assert_debug(!parser.IsValidExpression("base@ha + base@lo"));
	cemu_assert_debug(!parser.IsValidExpression("(1"));
	cemu_assert_debug(!parser.IsConstantExpression("unknownName"));
}
