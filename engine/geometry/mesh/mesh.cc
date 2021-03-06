// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ink/engine/geometry/mesh/mesh.h"

#include <cstddef>
#include <cstdint>
#include <numeric>

#include "ink/engine/geometry/algorithms/envelope.h"
#include "ink/engine/geometry/algorithms/transform.h"
#include "ink/engine/util/dbg/errors.h"
#include "ink/engine/util/funcs/utils.h"
#include "ink/engine/util/time/time_types.h"

namespace ink {
namespace {
constexpr uint16_t kMax16 = std::numeric_limits<uint16_t>::max();

template <typename PositionGetter, typename IndexType>
void NormalizeTriangleHelper(PositionGetter position_getter,
                             std::vector<IndexType>* indices) {
  ASSERT(indices->size() % 3 == 0);
  int n_triangles = indices->size() / 3;
  for (int i = 0; i < n_triangles; ++i) {
    geometry::Triangle t(position_getter((*indices)[3 * i]),
                         position_getter((*indices)[3 * i + 1]),
                         position_getter((*indices)[3 * i + 2]));
    if (t.SignedArea() < 0)
      std::swap((*indices)[3 * i + 1], (*indices)[3 * i + 2]);
  }
}
}  // namespace

ShaderMetadata::ShaderMetadata()
    : is_particle_(false),
      is_animated_(false),
      is_cycling_(false),
      init_time_(0),
      is_eraser_(false) {}

ShaderMetadata ShaderMetadata::Animated(FrameTimeS init_time) {
  ShaderMetadata metadata;
  metadata.is_animated_ = true;
  metadata.init_time_ = init_time;
  return metadata;
}

ShaderMetadata ShaderMetadata::Eraser() {
  ShaderMetadata metadata;
  metadata.is_eraser_ = true;
  return metadata;
}

ShaderMetadata ShaderMetadata::Particle(FrameTimeS init_time, bool cycling) {
  ShaderMetadata metadata;
  metadata.is_particle_ = true;
  metadata.is_cycling_ = cycling;
  metadata.init_time_ = init_time;
  return metadata;
}

Mesh::Mesh() {}
Mesh::Mesh(const Mesh& other) { *this = other; }
Mesh::Mesh(const std::vector<Vertex>& verts) : verts(verts) {}
Mesh& Mesh::operator=(const Mesh& other) {
  verts = other.verts;
  idx = other.idx;
  combined_idx = other.combined_idx;
  backend_vert_data.reset();
  texture.reset(other.texture ? new TextureInfo(*other.texture) : nullptr);
  object_matrix = other.object_matrix;
  shader_metadata = ShaderMetadata(other.shader_metadata);
  return *this;
}
Mesh::~Mesh() {}

void Mesh::Clear() {
  verts.clear();
  idx.clear();
  combined_idx.clear();
  backend_vert_data.reset();
}

void Mesh::Append(const Mesh& other) {
  ASSERT(other.verts.empty() || verts.empty() ||
         other.idx.empty() == idx.empty());

  auto startidx = verts.size();
  auto t = glm::inverse(object_matrix) * other.object_matrix;
  for (auto ov : other.verts) {
    ov.position = geometry::Transform(ov.position, t);
    verts.emplace_back(ov);
  }

  for (auto i : other.idx) {
    idx.push_back(i + startidx);
  }
}

void Mesh::Deindex() {
  if (idx.empty()) return;
  std::vector<Vertex> new_verts(idx.size());
  for (size_t i = 0; i < idx.size(); i++) {
    new_verts[i] = verts[idx[i]];
  }
  idx.clear();
  verts.swap(new_verts);
}

void Mesh::GenIndex() {
  idx.resize(verts.size());
  std::iota(idx.begin(), idx.end(), 0);
}

void Mesh::NormalizeTriangleOrientation() {
  NormalizeTriangleHelper(
      [this](IndexType index) { return verts[index].position; }, &idx);
}

glm::vec2 Mesh::ObjectPosToWorld(const glm::vec2& object_pos) const {
  return glm::vec2(object_matrix * glm::vec4(object_pos, 1, 1));
}

std::vector<uint16_t> Mesh::Index16() const {
  ASSERT(Has16BitIndex());
  if (!Has16BitIndex()) {
    SLOG(SLOG_ERROR, "mesh overflowing vert index");
  }
  return std::vector<uint16_t>(idx.begin(), idx.end());
}

bool Mesh::Has16BitIndex() const { return verts.size() < kMax16; }

size_t Mesh::IndexSize() const { return idx.size(); }

uint32_t Mesh::IndexAt(size_t n) const { return idx[n]; }
////////////////////////////////////

VertFormat OptimizedMesh::VertexFormat(ShaderType shader_type) {
  VertFormat fmt = VertFormat::x12y12;
  switch (shader_type) {
    case ColoredVertShader:
      fmt = VertFormat::x11a7r6y11g7b6;
      break;
    case SingleColorShader:
    case EraseShader:
      fmt = VertFormat::x12y12;
      break;
    case TexturedVertShader:
      fmt = VertFormat::x11a7r6y11g7b6u12v12;
      break;
    default:
      ASSERT(false);
      break;
  }
  return fmt;
}

OptimizedMesh::OptimizedMesh(const OptimizedMesh& other)
    : type(other.type),
      verts(other.verts),
      texture(other.texture ? new TextureInfo(*other.texture) : nullptr),
      object_matrix(other.object_matrix),
      color(other.color),
      mul_color_modifier(other.mul_color_modifier),
      add_color_modifier(other.add_color_modifier),
      idx_(other.idx_) {}

OptimizedMesh::OptimizedMesh(ShaderType type, const Mesh& mesh)
    : OptimizedMesh(type, mesh, geometry::Envelope(mesh.verts)) {}

OptimizedMesh::OptimizedMesh(ShaderType type, const Mesh& mesh, Rect envelope)
    : type(type),
      texture(mesh.texture ? new TextureInfo(*mesh.texture) : nullptr),
      color(mesh.verts.begin()->color),
      mul_color_modifier(glm::vec4(1, 1, 1, 1)),
      add_color_modifier(glm::vec4(0, 0, 0, 0)) {
  EXPECT(!mesh.idx.empty() && mesh.idx.size() % 3 == 0);
  EXPECT(!mesh.verts.empty());
  ASSERT(envelope.Contains(geometry::Envelope(mesh.verts)));

  VertFormat fmt = VertexFormat(type);

  auto m = PackedVertList::CalcTransformForFormat(envelope, fmt);
  verts = PackedVertList::PackVerts(mesh.verts, m, fmt);
  mbr = geometry::Transform(geometry::Envelope(mesh.verts), m);

  // We need to normalize the triangles using the packed vertices, because the
  // vertex positions are rounded when packed, which can cause a triangle to
  // flip orientation.
  if (mesh.verts.size() < kMax16) {
    idx_ = std::vector<uint16_t>(mesh.idx.begin(), mesh.idx.end());
    NormalizeTriangleHelper(
        [this](uint16_t index) {
          Vertex v;
          verts.UnpackVertex(index, &v);
          return v.position;
        },
        &absl::get<std::vector<uint16_t>>(idx_));
  } else {
    idx_ = mesh.idx;
    NormalizeTriangleHelper(
        [this](uint32_t index) {
          Vertex v;
          verts.UnpackVertex(index, &v);
          return v.position;
        },
        &absl::get<std::vector<uint32_t>>(idx_));
  }

  // m is meshcoords->objectcoords
  // inverse m is objectcoords->meshcoords
  // mesh.objectMatrix is meshcoords->worldcoords
  // objectmatrix should be objectcoords->worldcoords
  object_matrix = mesh.object_matrix * glm::inverse(m);

  Validate();
}

OptimizedMesh::~OptimizedMesh() {}

void OptimizedMesh::ClearCpuMemoryVerts() {
  // keep type, vbo, and color
  idx_ = std::vector<uint16_t>();
  verts.Clear();
}

Mesh OptimizedMesh::ToMesh() const {
  Mesh m;
  m.verts.resize(verts.size());
  for (uint32_t i = 0; i < verts.size(); ++i) {
    verts.UnpackVertex(i, &m.verts[i]);
    if (type == ShaderType::SingleColorShader) {
      m.verts[i].color = color;
    }
    m.verts[i].color =
        m.verts[i].color * mul_color_modifier + add_color_modifier;
  }
  if (absl::holds_alternative<std::vector<uint32_t>>(idx_)) {
    const auto& v = absl::get<std::vector<uint32_t>>(idx_);
    m.idx.assign(v.begin(), v.end());
  } else {
    const auto& v = absl::get<std::vector<uint16_t>>(idx_);
    m.idx.assign(v.begin(), v.end());
  }
  m.object_matrix = object_matrix;
  if (texture) m.texture = absl::make_unique<TextureInfo>(*texture);

  return m;
}

size_t OptimizedMesh::IndexSize() const {
  if (absl::holds_alternative<std::vector<uint32_t>>(idx_)) {
    return absl::get<std::vector<uint32_t>>(idx_).size();
  }
  return absl::get<std::vector<uint16_t>>(idx_).size();
}

uint32_t OptimizedMesh::IndexAt(size_t n) const {
  if (absl::holds_alternative<std::vector<uint32_t>>(idx_)) {
    return absl::get<std::vector<uint32_t>>(idx_).at(n);
  }
  return absl::get<std::vector<uint16_t>>(idx_).at(n);
}

void OptimizedMesh::Validate() const {
  if (IndexSize() == 0) return;
  ASSERT(IndexSize() % 3 == 0);
}

Rect OptimizedMesh::WorldBounds() const {
  return geometry::Transform(mbr, object_matrix);
}

std::vector<uint16_t> OptimizedMesh::Index16() const {
  if (absl::holds_alternative<std::vector<uint16_t>>(idx_)) {
    return absl::get<std::vector<uint16_t>>(idx_);
  }
  RUNTIME_ERROR("Cannot represent this optimized mesh's index in 16 bits");
}

}  // namespace ink
