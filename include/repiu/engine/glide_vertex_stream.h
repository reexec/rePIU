#ifndef REPIU_ENGINE_GLIDE_VERTEX_STREAM_H_
#define REPIU_ENGINE_GLIDE_VERTEX_STREAM_H_

#include "repiu/engine/gl_api.h"

#include <cstddef>
#include <cstdint>

namespace repiu::engine
{

// Issue #69: the attribute slots every renderer program binds before linking,
// so one vertex layout feeds the Glide combine program, the present pass and
// the post-process shaders alike.
constexpr GLuint kGlideVertexPositionSlot = 0;
constexpr GLuint kGlideVertexColorSlot = 1;
constexpr GLuint kGlideVertexTexCoordSlot = 2;

// One vertex as immediate mode received it: the window position with the
// orthographic eye depth, the iterated color, and the texture/fog coordinate
// (s/w, t/w, fog oow, texture oow) the combine shader divides per fragment.
struct GlideStreamVertex
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;
    float s = 0.0F;
    float t = 0.0F;
    float fog_oow = 0.0F;
    float texture_oow = 1.0F;
};

// Issue #69: replaces `glBegin`/`glEnd`. Each draw writes its vertices into
// the next free part of one buffer and draws them; at the end of the buffer
// it is orphaned and written from the start, so a draw never waits for the
// GPU to finish one that is still reading the buffer. OpenGL 3.3 core and
// ES 3.0 both have it, and core requires the vertex array object it keeps.
class GlideVertexStream
{
public:
    GlideVertexStream() = default;
    ~GlideVertexStream() = default;

    GlideVertexStream(const GlideVertexStream&) = delete;
    GlideVertexStream& operator=(const GlideVertexStream&) = delete;

    // Creates the buffer and the vertex array in the current context and
    // leaves both bound.
    bool Initialize(const GlApi* api);
    void Shutdown();

    // Binds the stream's vertex array and buffer again, after a pass that
    // bound its own (ImGui restores what it changed; this is for the rest).
    void Bind() const;

    bool Draw(GLenum primitive, const GlideStreamVertex* vertices,
              std::size_t count);

    std::uint64_t orphan_count() const { return orphan_count_; }

private:
    const GlApi* api_ = nullptr;
    GLuint vertex_array_ = 0;
    GLuint buffer_ = 0;
    std::size_t capacity_bytes_ = 0;
    std::size_t write_offset_ = 0;
    std::uint64_t orphan_count_ = 0;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_GLIDE_VERTEX_STREAM_H_
