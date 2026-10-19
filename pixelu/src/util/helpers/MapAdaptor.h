// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <map>
#include <boost/iterator/transform_iterator.hpp>

// A range over a container that hands out f(element) instead of the element.
// Used to iterate only the keys or only the values of a map.
template<typename TContainer, typename TFunc>
struct map_adaptor
{
	map_adaptor(const TContainer& container, const TFunc& func) : _t(container), _f(func) {}
	map_adaptor(map_adaptor& other) = delete;
	map_adaptor(map_adaptor&& other) = default;

	[[nodiscard]] auto begin() { return boost::make_transform_iterator(_t.begin(), _f); }
	[[nodiscard]] auto end() { return boost::make_transform_iterator(_t.end(), _f); }
	[[nodiscard]] auto cbegin() const { return boost::make_transform_iterator(_t.cbegin(), _f); }
	[[nodiscard]] auto cend() const { return boost::make_transform_iterator(_t.cend(), _f); }

protected:
	const TContainer& _t;
	TFunc _f;
};

template<typename TContainer, typename TFunc>
auto get_map_adaptor(const TContainer& container, const TFunc& func)
{
	return map_adaptor<TContainer, TFunc>(container, func);
}

template<typename TContainer>
auto get_keys(const TContainer& container)
{
	return get_map_adaptor(container, [](const auto& kv) { return kv.first; });
}

template<typename TContainer>
auto get_values(const TContainer& container)
{
	return get_map_adaptor(container, [](const auto& kv) { return kv.second; });
}
