// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Common/GLInclude/GLInclude.h"

// occlusion query backed by a GL_SAMPLES_PASSED query object
class LatteQueryObjectGL : public LatteQueryObject
{
public:
	bool getResult(uint64& numSamplesPassed) override
	{
		catchOpenGLError();
		GLint available = 0;
		glGetQueryObjectiv(m_glQuery, GL_QUERY_RESULT_AVAILABLE, &available);
		if (!available)
			return false;
		catchOpenGLError();
		GLint64 samples = 0;
		glGetQueryObjecti64v(m_glQuery, GL_QUERY_RESULT, &samples);
		numSamplesPassed = (uint64)samples;
		return true;
	}

	void begin() override
	{
		catchOpenGLError();
		glBeginQueryIndexed(GL_SAMPLES_PASSED, index, m_glQuery);
		catchOpenGLError();
	}

	void end() override
	{
		glEndQueryIndexed(GL_SAMPLES_PASSED, index);
		catchOpenGLError();
	}

	void ResetForReuse()
	{
		queryEnded = false;
		queryEventStart = 0;
		queryEventEnd = 0;
	}

	GLuint m_glQuery{};
};

LatteQueryObject* OpenGLRenderer::occlusionQuery_create()
{
	// destroyed queries are recycled, oldest first
	if (!list_queryCacheOcclusion.empty())
	{
		LatteQueryObjectGL* recycled = list_queryCacheOcclusion.front();
		list_queryCacheOcclusion.erase(list_queryCacheOcclusion.begin());
		recycled->ResetForReuse();
		return recycled;
	}
	auto* query = new LatteQueryObjectGL();
	glGenQueries(1, &query->m_glQuery);
	query->index = 0;
	query->ResetForReuse();
	catchOpenGLError();
	return query;
}

void OpenGLRenderer::occlusionQuery_destroy(LatteQueryObject* queryObj)
{
	list_queryCacheOcclusion.push_back(static_cast<LatteQueryObjectGL*>(queryObj));
}

void OpenGLRenderer::occlusionQuery_flush()
{
	glFlush();
}
