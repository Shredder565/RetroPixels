// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"

#include "Cafe/HW/Latte/Core/LatteQueryObject.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

// GX2 occlusion queries. Guest queries can overlap, host queries cannot, so the time
// between query begin/end commands is split into consecutive host queries. Every
// begin/end gets an increasing event id; a host query adds its sample count to all
// guest queries whose event range contains it. A guest query completes once every
// host query up to its end event has delivered a result.

namespace
{
	struct GuestQuery
	{
		MPTR queryMPTR;
		uint64 eventStart;
		uint64 eventEnd;
		uint64 samplesPassed;
		bool ended;
	};

	uint64 s_nextEventId = 1;
	std::vector<GuestQuery> s_guestQueries; // in begin order
	std::vector<LatteQueryObject*> s_hostQueriesPending; // ended, waiting for results, in end order
	LatteQueryObject* s_activeHostQuery = nullptr;
	uint64 s_lastCompletedEvent = 0;
	sint32 s_beginsSinceUpdate = 0;

	uint64 NextEventId()
	{
		return s_nextEventId++;
	}

	void StartHostQuery(uint64 eventId)
	{
		LatteQueryObject* query = g_renderer->occlusionQuery_create();
		query->queryEventStart = eventId;
		query->begin();
		s_activeHostQuery = query;
	}

	void EndActiveHostQuery(uint64 eventId)
	{
		LatteQueryObject* query = s_activeHostQuery;
		if (!query)
			return;
		cemu_assert_debug(!query->queryEnded);
		query->queryEnded = true;
		query->queryEventEnd = eventId;
		query->end();
		s_hostQueriesPending.emplace_back(query);
		s_activeHostQuery = nullptr;
	}

	// writes the result into the guest query object
	void PublishGuestResult(const GuestQuery& q)
	{
		// the sample count is stored in host byte order, GX2QueryGetOcclusionResult reads it back the same way
		uint64* words = (uint64*)memory_getPointerFromVirtualOffset(q.queryMPTR);
		words[0] = 0;
		words[1] = q.samplesPassed;
		words[2] = 0;
		words[3] = 0;
		// clearing this word (the 'OCPU' marker of CPU queries) tells GX2QueryGetOcclusionResult that the result is ready
		words[4] = 0;
	}
}

void LatteQuery_UpdateFinishedQueries()
{
	g_renderer->occlusionQuery_updateState();
	// results arrive in order, stop at the first host query that has none yet
	while (!s_hostQueriesPending.empty())
	{
		LatteQueryObject* query = s_hostQueriesPending.front();
		cemu_assert_debug(query->queryEnded);
		uint64 samples;
		if (!query->getResult(samples))
			break;
		cemu_assert_debug(s_lastCompletedEvent < query->queryEventEnd);
		s_lastCompletedEvent = query->queryEventEnd;
		for (GuestQuery& q : s_guestQueries)
		{
			if (query->queryEventStart >= q.eventStart && query->queryEventEnd <= q.eventEnd)
				q.samplesPassed += samples;
		}
		s_hostQueriesPending.erase(s_hostQueriesPending.begin());
		g_renderer->occlusionQuery_destroy(query);
	}
	// complete guest queries whose whole range is covered
	for (size_t i = 0; i < s_guestQueries.size();)
	{
		const GuestQuery& q = s_guestQueries[i];
		if (q.ended && s_lastCompletedEvent >= q.eventEnd)
		{
			PublishGuestResult(q);
			s_guestQueries.erase(s_guestQueries.begin() + i);
			continue;
		}
		i++;
	}
}

void LatteQuery_UpdateFinishedQueriesForceFinishAll()
{
	cemu_assert_debug(s_activeHostQuery == nullptr);
	// after the flush every host query result becomes available
	g_renderer->occlusionQuery_flush();
	do
	{
		LatteQuery_UpdateFinishedQueries();
	} while (!s_hostQueriesPending.empty());
}

void LatteQuery_BeginOcclusionQuery(MPTR queryMPTR)
{
	// poll for results every eighth begin
	if (s_beginsSinceUpdate < 7)
		s_beginsSinceUpdate++;
	else
	{
		LatteQuery_UpdateFinishedQueries();
		s_beginsSinceUpdate = 0;
	}
	for (const GuestQuery& q : s_guestQueries)
	{
		if (q.queryMPTR == queryMPTR)
		{
			cemuLog_logDebug(LogType::Force, "GX2 occlusion query {:08x} begun while already active", queryMPTR);
			return;
		}
	}
	const uint64 eventId = NextEventId();
	EndActiveHostQuery(eventId);
	s_guestQueries.push_back({ queryMPTR, eventId, 0, 0, false });
	StartHostQuery(eventId);
}

void LatteQuery_EndOcclusionQuery(MPTR queryMPTR)
{
	if (queryMPTR == MPTR_NULL)
		return;
	const uint64 eventId = NextEventId();
	for (GuestQuery& q : s_guestQueries)
	{
		if (q.queryMPTR == queryMPTR)
		{
			q.eventEnd = eventId;
			q.ended = true;
			break;
		}
	}
	EndActiveHostQuery(eventId);
	// guest queries that are still open need a new host query
	const bool anyOpen = std::any_of(s_guestQueries.begin(), s_guestQueries.end(), [](const GuestQuery& q) { return !q.ended; });
	if (anyOpen)
		StartHostQuery(eventId);
}

void LatteQuery_CancelActiveGPU7Queries()
{
	cemu_assert_debug(s_activeHostQuery == nullptr);
}

void LatteQuery_Init()
{
}
