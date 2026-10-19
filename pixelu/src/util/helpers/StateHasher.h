// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Fast 64-bit hash over a short sequence of 64-bit words, used for in-memory
// cache keys (it is never stored on disk). Two independent lanes take one
// word each per MixIn() call so the multiplies can run in parallel; Finish()
// merges the lanes and runs a final avalanche step.
class DualStateHasher
{
public:
	DualStateHasher() = default;

	FORCE_INLINE void MixIn(uint64 a, uint64 b)
	{
		m_laneA = absorb(m_laneA, a);
		m_laneB = absorb(m_laneB, b);
		m_count += 2;
	}

	FORCE_INLINE void MixInSingle(uint64 a)
	{
		m_laneA = absorb(m_laneA, a);
		m_count += 1;
	}

	FORCE_INLINE uint64 Finish()
	{
		uint64 v = m_laneA ^ std::rotl(m_laneB, 29) ^ (m_count * kOddA);
		// splitmix64 finaliser
		v ^= v >> 30;
		v *= 0xBF58476D1CE4E5B9ull;
		v ^= v >> 27;
		v *= 0x94D049BB133111EBull;
		v ^= v >> 31;
		return v;
	}

private:
	static constexpr uint64 kOddA = 0xA24BAED4963EE407ull;
	static constexpr uint64 kOddB = 0x9FB21C651E98DF25ull;

	static FORCE_INLINE uint64 absorb(uint64 lane, uint64 word)
	{
		lane ^= word * kOddA;
		lane = std::rotl(lane, 31);
		return lane * kOddB;
	}

	uint64 m_laneA{ 0x6A09E667F3BCC908ull };
	uint64 m_laneB{ 0xBB67AE8584CAA73Bull };
	uint64 m_count{ 0 };
};
