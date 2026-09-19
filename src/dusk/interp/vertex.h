#pragma once

class J3DVertexBuffer;
class J3DDeformData;

namespace dusk::interp::vertex {

void capture(J3DVertexBuffer* buffer, const J3DDeformData* deformation);
void* positions(const J3DVertexBuffer* buffer, void* current);
void* normals(const J3DVertexBuffer* buffer, void* current);
void reset(const J3DVertexBuffer* buffer);
void invalidate(const J3DDeformData* deformation);
void prune();
void clear();

}  // namespace dusk::interp::vertex
