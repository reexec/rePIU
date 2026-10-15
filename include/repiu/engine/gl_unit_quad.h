#ifndef REPIU_ENGINE_GL_UNIT_QUAD_H_
#define REPIU_ENGINE_GL_UNIT_QUAD_H_

#include "repiu/engine/gl_api.h"

namespace repiu::engine
{

// Issue #69: the unit square, drawn as a four-vertex strip, that the present
// pass and the post-process pass map a texture with. Its corners (x, y, 0, 1)
// feed both the position slot and the texture-coordinate slot of the shared
// vertex layout (glide_vertex_stream.h), so a program reads (0,0)..(1,1) from
// either; corner (0,0) is the texture's row 0, the bottom scanline.
class GlUnitQuad
{
public:
    GlUnitQuad() = default;
    ~GlUnitQuad() = default;

    GlUnitQuad(const GlUnitQuad&) = delete;
    GlUnitQuad& operator=(const GlUnitQuad&) = delete;

    bool Initialize(const GlApi* api);
    void Shutdown();

    // Binds its own vertex array and draws; the caller restores bindings.
    void Draw() const;

private:
    const GlApi* api_ = nullptr;
    GLuint vertex_array_ = 0;
    GLuint buffer_ = 0;
};

// Compiles and links a program from a vertex and a fragment source, binding
// the shared attribute slots by the names given (any may be null). On failure
// returns 0 and says why.
GLuint BuildGlProgram(const GlApi& api, const char* vertex_source,
                      const char* fragment_source, const char* position_name,
                      const char* color_name, const char* tex_coord_name,
                      std::string* message);

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_GL_UNIT_QUAD_H_
