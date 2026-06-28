#include "rom/ssc_compressor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spintool::rom
{
	namespace
	{
		struct SSCToken
		{
			bool raw = true;
			Uint8 value = 0;
			Uint16 source = 0;
			Uint8 length = 0;
		};

		Uint32 ThreeByteKey(const std::vector<Uint8>& data, const std::size_t offset)
		{
			return (static_cast<Uint32>(data[offset]) << 16U) |
				(static_cast<Uint32>(data[offset + 1U]) << 8U) |
				static_cast<Uint32>(data[offset + 2U]);
		}
	}

	SSCCompressionResult SSCCompressor::CompressData(
		const std::vector<Uint8>& in_data,
		Uint32 offset,
		Uint32 working_data_size_hint
	)
	{
		(void)working_data_size_hint;
		if (offset >= in_data.size())
		{
			// A header followed by the zero-source copy token is the empty stream.
			return { 0U, 0U, 0U };
		}

		constexpr std::size_t kWindowSize = 0x1000U;
		constexpr std::size_t kMaximumLength = 17U;
		constexpr std::size_t kMinimumUsefulLength = 3U;
		constexpr std::size_t kMaximumCandidates = 256U;

		std::vector<SSCToken> tokens;
		tokens.reserve(in_data.size() / 2U + 1U);
		std::unordered_map<Uint32, std::deque<std::size_t>> positions_by_key;

		auto add_position = [&](const std::size_t position)
		{
			if (position + 2U >= in_data.size())
			{
				return;
			}
			const Uint32 key = ThreeByteKey(in_data, position);
			auto& positions = positions_by_key[key];
			positions.emplace_back(position);
			const std::size_t minimum_position =
				position >= kWindowSize ? position - kWindowSize + 1U : 0U;
			while (!positions.empty() && positions.front() < minimum_position)
			{
				positions.pop_front();
			}
		};

		std::size_t position = static_cast<std::size_t>(offset);
		while (position < in_data.size())
		{
			std::size_t best_length = 0U;
			std::size_t best_position = 0U;

			if (position + kMinimumUsefulLength <= in_data.size())
			{
				const Uint32 key = ThreeByteKey(in_data, position);
				const auto found = positions_by_key.find(key);
				if (found != positions_by_key.end())
				{
					const auto& candidates = found->second;
					std::size_t tested = 0U;
					for (auto candidate = candidates.rbegin();
						candidate != candidates.rend() && tested < kMaximumCandidates;
						++candidate, ++tested)
					{
						const std::size_t source_position = *candidate;
						if (source_position >= position ||
							position - source_position > kWindowSize)
						{
							continue;
						}
						const Uint16 source_index = static_cast<Uint16>(
							(source_position + 1U) & 0x0FFFU
						);
						// Source zero is reserved by the format as the end marker.
						if (source_index == 0U)
						{
							continue;
						}

						const std::size_t distance = position - source_position;
						const std::size_t maximum_length = std::min(
							kMaximumLength,
							in_data.size() - position
						);
						std::size_t length = 0U;
						while (length < maximum_length &&
							in_data[position + length] ==
								in_data[source_position + (length % distance)])
						{
							++length;
						}
						if (length > best_length)
						{
							best_length = length;
							best_position = source_position;
							if (best_length == maximum_length)
							{
								break;
							}
						}
					}
				}
			}

			std::size_t consumed = 1U;
			if (best_length >= kMinimumUsefulLength)
			{
				SSCToken token;
				token.raw = false;
				token.source = static_cast<Uint16>((best_position + 1U) & 0x0FFFU);
				token.length = static_cast<Uint8>(best_length);
				tokens.emplace_back(token);
				consumed = best_length;
			}
			else
			{
				SSCToken token;
				token.raw = true;
				token.value = in_data[position];
				tokens.emplace_back(token);
			}

			for (std::size_t added = 0U; added < consumed; ++added)
			{
				add_position(position + added);
			}
			position += consumed;
		}

		// A copy token with source zero terminates the stream.
		SSCToken terminator;
		terminator.raw = false;
		terminator.source = 0U;
		terminator.length = 2U;
		tokens.emplace_back(terminator);

		SSCCompressionResult out_data;
		out_data.reserve(tokens.size() * 2U);
		for (std::size_t token_offset = 0U;
			token_offset < tokens.size();
			token_offset += 8U)
		{
			const std::size_t group_size = std::min<std::size_t>(
				8U,
				tokens.size() - token_offset
			);
			Uint8 header = 0U;
			for (std::size_t index = 0U; index < group_size; ++index)
			{
				if (tokens[token_offset + index].raw)
				{
					header = static_cast<Uint8>(header | (1U << index));
				}
			}
			out_data.emplace_back(header);

			for (std::size_t index = 0U; index < group_size; ++index)
			{
				const SSCToken& token = tokens[token_offset + index];
				if (token.raw)
				{
					out_data.emplace_back(token.value);
					continue;
				}

				out_data.emplace_back(static_cast<Uint8>(token.source & 0x00FFU));
				const Uint8 length_code = token.source == 0U
					? 0U
					: static_cast<Uint8>(token.length - 2U);
				out_data.emplace_back(static_cast<Uint8>(
					((token.source >> 4U) & 0xF0U) |
					(length_code & 0x0FU)
				));
			}
		}

		return out_data;
	}
}
