#include "repiu/engine/glide_vertex_stream.h"

namespace repiu::engine
{
namespace
{

// 4 MB holds about 95,000 vertices: several frames of PIU's heaviest scenes
// between orphanings.
constexpr std::size_t kStreamCapacityBytes = 4U * 1024U * 1024U;

}  // namespace

bool GlideVertexStream::Initialize(const GlApi* api)
{
    Shutdown();
    if (api == nullptr)
    {
        return false;
    }
    api_ = api;
    api_->gen_vertex_arrays(1, &vertex_array_);
    api_->gen_buffers(1, &buffer_);
    if (vertex_array_ == 0U || buffer_ == 0U)
    {
        Shutdown();
        return false;
    }
    api_->bind_vertex_array(vertex_array_);
    api_->bind_buffer(GL_ARRAY_BUFFER, buffer_);
    capacity_bytes_ = kStreamCapacityBytes;
    api_->buffer_data(GL_ARRAY_BUFFER,
                      static_cast<GLsizeiptr>(capacity_bytes_), nullptr,
                      GL_STREAM_DRAW);
    write_offset_ = 0U;
    constexpr GLsizei kStride = sizeof(GlideStreamVertex);
    api_->enable_vertex_attrib_array(kGlideVertexPositionSlot);
    api_->vertex_attrib_pointer(
        kGlideVertexPositionSlot, 3, GL_FLOAT, GL_FALSE, kStride,
        reinterpret_cast<const void*>(offsetof(GlideStreamVertex, x)));
    api_->enable_vertex_attrib_array(kGlideVertexColorSlot);
    api_->vertex_attrib_pointer(
        kGlideVertexColorSlot, 4, GL_FLOAT, GL_FALSE, kStride,
        reinterpret_cast<const void*>(offsetof(GlideStreamVertex, r)));
    api_->enable_vertex_attrib_array(kGlideVertexTexCoordSlot);
    api_->vertex_attrib_pointer(
        kGlideVertexTexCoordSlot, 4, GL_FLOAT, GL_FALSE, kStride,
        reinterpret_cast<const void*>(offsetof(GlideStreamVertex, s)));
    return true;
}

void GlideVertexStream::Shutdown()
{
    if (api_ != nullptr)
    {
        if (buffer_ != 0U)
        {
            api_->delete_buffers(1, &buffer_);
        }
        if (vertex_array_ != 0U)
        {
            api_->delete_vertex_arrays(1, &vertex_array_);
        }
    }
    api_ = nullptr;
    vertex_array_ = 0U;
    buffer_ = 0U;
    capacity_bytes_ = 0U;
    write_offset_ = 0U;
}

void GlideVertexStream::Bind() const
{
    if (api_ == nullptr)
    {
        return;
    }
    api_->bind_vertex_array(vertex_array_);
    api_->bind_buffer(GL_ARRAY_BUFFER, buffer_);
}

bool GlideVertexStream::Draw(GLenum primitive,
                             const GlideStreamVertex* vertices,
                             std::size_t count)
{
    if (api_ == nullptr || vertices == nullptr || count == 0U)
    {
        return api_ != nullptr;
    }
    const std::size_t bytes = count * sizeof(GlideStreamVertex);
    if (bytes > capacity_bytes_)
    {
        return false;
    }
    if (write_offset_ + bytes > capacity_bytes_)
    {
        api_->buffer_data(GL_ARRAY_BUFFER,
                          static_cast<GLsizeiptr>(capacity_bytes_), nullptr,
                          GL_STREAM_DRAW);
        write_offset_ = 0U;
        ++orphan_count_;
    }
    api_->buffer_sub_data(GL_ARRAY_BUFFER,
                          static_cast<GLintptr>(write_offset_),
                          static_cast<GLsizeiptr>(bytes), vertices);
    glDrawArrays(primitive,
                 static_cast<GLint>(write_offset_ / sizeof(GlideStreamVertex)),
                 static_cast<GLsizei>(count));
    write_offset_ += bytes;
    return true;
}

}  // namespace repiu::engine
