// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/MeshAsset.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include <tinygltf/tiny_gltf.h>

#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"

namespace VideoCommon
{
namespace
{
// ---------------------------------------------------------------------------------------------
// glTF import (following the Khronos glTF 2.0 specification)
// ---------------------------------------------------------------------------------------------

Common::Vec3 Vec3FromDoubles(const std::vector<double>& v)
{
  return Common::Vec3{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

// A node's local transform: an explicit matrix, or scale, then rotation, then translation
// composed onto the identity.
Common::Matrix44 LocalTransform(const tinygltf::Node& node)
{
  if (!node.matrix.empty())
  {
    Common::Matrix44 explicit_matrix;
    const std::size_t count = std::min(node.matrix.size(), explicit_matrix.data.size());
    for (std::size_t i = 0; i < count; ++i)
      explicit_matrix.data[i] = static_cast<float>(node.matrix[i]);
    return explicit_matrix;
  }

  Common::Matrix44 result = Common::Matrix44::Identity();
  if (!node.scale.empty())
    result *= Common::Matrix44::FromMatrix33(Common::Matrix33::Scale(Vec3FromDoubles(node.scale)));
  if (!node.rotation.empty())
  {
    // glTF stores quaternions as (x, y, z, w).
    const auto& q = node.rotation;
    result *= Common::Matrix44::FromQuaternion(
        Common::Quaternion(static_cast<float>(q[3]), static_cast<float>(q[0]),
                           static_cast<float>(q[1]), static_cast<float>(q[2])));
  }
  if (!node.translation.empty())
    result *= Common::Matrix44::Translate(Vec3FromDoubles(node.translation));
  return result;
}

// Maps a glTF component type onto a vertex attribute format. Doubles and 32-bit unsigned
// integers have no equivalent; types this does not know leave the format untouched.
bool SetComponentFormat(int component_type, AttributeFormat* format)
{
  ComponentFormat type;
  bool integer = false;
  switch (component_type)
  {
  case TINYGLTF_COMPONENT_TYPE_BYTE:
    type = ComponentFormat::Byte;
    break;
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
    type = ComponentFormat::UByte;
    break;
  case TINYGLTF_COMPONENT_TYPE_SHORT:
    type = ComponentFormat::Short;
    break;
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
    type = ComponentFormat::UShort;
    break;
  case TINYGLTF_COMPONENT_TYPE_FLOAT:
    type = ComponentFormat::Float;
    break;
  case TINYGLTF_COMPONENT_TYPE_INT:
    // Signed 32-bit integers are passed through as float-sized integer data.
    type = ComponentFormat::Float;
    integer = true;
    break;
  case TINYGLTF_COMPONENT_TYPE_DOUBLE:
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    return false;
  default:
    return true;
  }
  format->type = type;
  format->integer = integer;
  return true;
}

// Bytes one element of the accessor occupies, or 0 (after logging) if its type is invalid.
u32 ElementSize(const tinygltf::Accessor& accessor, std::string_view what)
{
  const int components = tinygltf::GetNumComponentsInType(static_cast<u32>(accessor.type));
  if (components == -1)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to {}, component count was invalid", what);
    return 0;
  }
  const int bytes = tinygltf::GetComponentSizeInBytes(static_cast<u32>(accessor.componentType));
  if (bytes == -1)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to {}, component size was invalid", what);
    return 0;
  }
  return static_cast<u32>(bytes * components);
}

// Copies one attribute into the interleaved vertex buffer at byte `*offset` of each vertex and
// moves *offset past it.
bool CopyAttribute(const tinygltf::Model& model, int accessor_index, std::size_t* offset,
                   MeshDataChunk* chunk)
{
  const tinygltf::Accessor& accessor = model.accessors[static_cast<std::size_t>(accessor_index)];
  const u32 element = ElementSize(accessor, "copy buffer data from primitive");
  if (element == 0)
    return false;

  const tinygltf::BufferView& view =
      model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
  const tinygltf::Buffer& buffer = model.buffers[static_cast<std::size_t>(view.buffer)];
  const u8* source = buffer.data.data() + accessor.byteOffset + view.byteOffset;
  // A zero byteStride means elements follow each other without gaps.
  const std::size_t source_step = view.byteStride != 0 ? view.byteStride : element;

  u8* target = chunk->vertex_data.get() + *offset;
  for (std::size_t i = 0; i < accessor.count; ++i)
    std::memcpy(target + i * chunk->vertex_stride, source + i * source_step, element);

  *offset += element;
  return true;
}

// Optional per-vertex attributes in the order they are laid out after the position.
struct OptionalAttribute
{
  const char* name;
  enum class Slot
  {
    Color,
    Normal,
    Texcoord
  } slot;
  std::size_t index;
};

constexpr std::array<OptionalAttribute, 11> OPTIONAL_ATTRIBUTES{{
    {"COLOR_0", OptionalAttribute::Slot::Color, 0},
    {"COLOR_1", OptionalAttribute::Slot::Color, 1},
    {"NORMAL", OptionalAttribute::Slot::Normal, 0},
    {"TEXCOORD_0", OptionalAttribute::Slot::Texcoord, 0},
    {"TEXCOORD_1", OptionalAttribute::Slot::Texcoord, 1},
    {"TEXCOORD_2", OptionalAttribute::Slot::Texcoord, 2},
    {"TEXCOORD_3", OptionalAttribute::Slot::Texcoord, 3},
    {"TEXCOORD_4", OptionalAttribute::Slot::Texcoord, 4},
    {"TEXCOORD_5", OptionalAttribute::Slot::Texcoord, 5},
    {"TEXCOORD_6", OptionalAttribute::Slot::Texcoord, 6},
    {"TEXCOORD_7", OptionalAttribute::Slot::Texcoord, 7},
}};

// Every attribute that contributes to the vertex size.
constexpr std::array<const char*, 12> STRIDE_ATTRIBUTES{
    "POSITION",   "NORMAL",     "COLOR_0",    "COLOR_1",    "TEXCOORD_0", "TEXCOORD_1",
    "TEXCOORD_2", "TEXCOORD_3", "TEXCOORD_4", "TEXCOORD_5", "TEXCOORD_6", "TEXCOORD_7"};

bool ReadIndices(std::string_view file, const tinygltf::Model& model, int accessor_index,
                 MeshDataChunk* chunk)
{
  const tinygltf::Accessor& accessor = model.accessors[static_cast<std::size_t>(accessor_index)];
  const tinygltf::BufferView& view =
      model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
  const tinygltf::Buffer& buffer = model.buffers[static_cast<std::size_t>(view.buffer)];

  const int stride = accessor.ByteStride(view);
  if (stride == -1)
  {
    ERROR_LOG_FMT(VIDEO, "Mesh '{}' has invalid stride", file);
    return false;
  }

  chunk->indices = std::make_unique_for_overwrite<u16[]>(accessor.count);
  if (accessor.count != 0 && accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
  {
    ERROR_LOG_FMT(
        VIDEO, "Mesh '{}' uses an indice format of unsigned int which is not currently supported",
        file);
    return false;
  }

  const u8* source = buffer.data.data() + accessor.byteOffset + view.byteOffset;
  const auto step = static_cast<std::size_t>(stride);
  for (std::size_t i = 0; i < accessor.count; ++i)
  {
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
      std::memcpy(&chunk->indices[i], source + i * step, sizeof(u16));
    else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
      chunk->indices[i] = source[i * step];
  }
  chunk->num_indices = static_cast<u32>(accessor.count);
  return true;
}

bool ReadPrimitive(std::string_view file, const tinygltf::Model& model,
                   const tinygltf::Primitive& primitive, const Common::Matrix44& transform,
                   MeshDataChunk* chunk)
{
  chunk->transform = transform;
  if (primitive.indices == -1)
  {
    ERROR_LOG_FMT(VIDEO, "Mesh '{}' is expected to have indices but doesn't have any", file);
    return false;
  }
  chunk->material_name = model.materials[static_cast<std::size_t>(primitive.material)].name;

  if (!ReadIndices(file, model, primitive.indices, chunk))
    return false;

  switch (primitive.mode)
  {
  case TINYGLTF_MODE_TRIANGLES:
    chunk->primitive_type = PrimitiveType::Triangles;
    break;
  case TINYGLTF_MODE_TRIANGLE_STRIP:
    chunk->primitive_type = PrimitiveType::TriangleStrip;
    break;
  case TINYGLTF_MODE_TRIANGLE_FAN:
    ERROR_LOG_FMT(VIDEO, "Mesh '{}' requires triangle fan but that is not supported", file);
    return false;
  case TINYGLTF_MODE_LINE:
    chunk->primitive_type = PrimitiveType::Lines;
    break;
  case TINYGLTF_MODE_POINTS:
    chunk->primitive_type = PrimitiveType::Points;
    break;
  default:
    break;
  }

  const auto& attributes = primitive.attributes;
  const auto find_attribute = [&](const char* name) { return attributes.find(name); };

  chunk->vertex_stride = 0;
  for (const char* name : STRIDE_ATTRIBUTES)
  {
    const auto it = find_attribute(name);
    if (it == attributes.end())
      continue;
    const u32 element =
        ElementSize(model.accessors[static_cast<std::size_t>(it->second)], "update vertex stride");
    if (element == 0)
      return false;
    chunk->vertex_stride += element;
  }
  PortableVertexDeclaration& decl = chunk->vertex_declaration;
  decl.stride = static_cast<int>(chunk->vertex_stride);

  // The position is required and comes first in each vertex.
  const auto position = find_attribute("POSITION");
  if (position == attributes.end())
  {
    ERROR_LOG_FMT(VIDEO, "Mesh '{}' does not provide a POSITION attribute, that is required", file);
    return false;
  }
  const tinygltf::Accessor& position_accessor =
      model.accessors[static_cast<std::size_t>(position->second)];
  chunk->num_vertices = static_cast<u32>(position_accessor.count);
  chunk->vertex_data =
      std::make_unique_for_overwrite<u8[]>(chunk->num_vertices * chunk->vertex_stride);

  std::size_t offset = 0;
  if (!CopyAttribute(model, position->second, &offset, chunk))
    return false;
  chunk->components_available = 0;
  decl.position.enable = true;
  decl.position.components = 3;
  decl.position.offset = 0;
  if (!SetComponentFormat(position_accessor.componentType, &decl.position))
  {
    ERROR_LOG_FMT(VIDEO, "Mesh '{}' has invalid attribute format for position", file);
    return false;
  }

  // glTF requires bounds on positions; they are kept for culling and placement.
  if (position_accessor.minValues.size() != 3)
  {
    ERROR_LOG_FMT(VIDEO, "Mesh '{}' is expected to have a minimum value but it is missing", file);
    return false;
  }
  chunk->minimum_position = Vec3FromDoubles(position_accessor.minValues);
  if (position_accessor.maxValues.size() != 3)
  {
    ERROR_LOG_FMT(VIDEO, "Mesh '{}' is expected to have a maximum value but it is missing", file);
    return false;
  }
  chunk->maximum_position = Vec3FromDoubles(position_accessor.maxValues);

  for (const OptionalAttribute& attribute : OPTIONAL_ATTRIBUTES)
  {
    AttributeFormat* format = nullptr;
    int components = 0;
    u32 flag = 0;
    switch (attribute.slot)
    {
    case OptionalAttribute::Slot::Color:
      format = &decl.colors[attribute.index];
      components = 3;
      flag = VB_HAS_COL0 << attribute.index;
      break;
    case OptionalAttribute::Slot::Normal:
      format = &decl.normals[attribute.index];
      components = 3;
      flag = VB_HAS_NORMAL;
      break;
    case OptionalAttribute::Slot::Texcoord:
      format = &decl.texcoords[attribute.index];
      components = 2;
      flag = VB_HAS_UV0 << attribute.index;
      break;
    }

    const auto it = find_attribute(attribute.name);
    if (it == attributes.end())
    {
      format->enable = false;
      continue;
    }

    format->offset = static_cast<int>(offset);
    if (!CopyAttribute(model, it->second, &offset, chunk))
      return false;
    chunk->components_available |= flag;
    format->enable = true;
    format->components = components;
    if (!SetComponentFormat(model.accessors[static_cast<std::size_t>(it->second)].componentType,
                            format))
    {
      ERROR_LOG_FMT(VIDEO, "Mesh '{}' has invalid attribute format for {}", file, attribute.name);
      return false;
    }
  }

  // Draws that need a position matrix index turn this on themselves.
  decl.posmtx.enable = false;
  return true;
}

bool ReadNodeTree(std::string_view file, const tinygltf::Model& model, const tinygltf::Node& node,
                  const Common::Matrix44& transform, MeshData* data)
{
  if (node.mesh != -1)
  {
    for (const tinygltf::Primitive& primitive :
         model.meshes[static_cast<std::size_t>(node.mesh)].primitives)
    {
      MeshDataChunk chunk;
      if (!ReadPrimitive(file, model, primitive, transform, &chunk))
        return false;
      data->m_mesh_chunks.push_back(std::move(chunk));
    }
  }

  for (const int child_index : node.children)
  {
    const tinygltf::Node& child = model.nodes[static_cast<std::size_t>(child_index)];
    if (!ReadNodeTree(file, model, child, transform * LocalTransform(child), data))
      return false;
  }
  return true;
}

bool ReadModel(std::string_view file, const tinygltf::Model& model, MeshData* data)
{
  const int scene = model.defaultScene == -1 ? 0 : model.defaultScene;
  for (const int node_index : model.scenes[static_cast<std::size_t>(scene)].nodes)
  {
    const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(node_index)];
    if (!ReadNodeTree(file, model, node, LocalTransform(node), data))
      return false;
  }

  // Material conversion is not implemented: each glTF material starts out unmapped.
  for (const tinygltf::Material& material : model.materials)
    data->m_mesh_material_to_material_asset_id.insert_or_assign(material.name, "");
  return true;
}

// ---------------------------------------------------------------------------------------------
// Binary mesh format
//
// size_t chunk count, then for every chunk:
//   u32 vertex count, u32 vertex stride, vertex bytes (count * stride),
//   u32 index count, u16 indices,
//   PortableVertexDeclaration, PrimitiveType, u32 components_available,
//   Vec3 minimum, Vec3 maximum, 16 floats transform,
//   size_t name length, name bytes.
// All values are stored in host byte order and size.
// ---------------------------------------------------------------------------------------------

class ByteReader
{
public:
  explicit ByteReader(std::span<const u8> bytes) : m_rest(bytes) {}

  bool Read(void* out, std::size_t size)
  {
    if (size > m_rest.size())
      return false;
    if (size != 0)
      std::memcpy(out, m_rest.data(), size);
    m_rest = m_rest.subspan(size);
    return true;
  }

  template <typename T>
  bool Read(T* out)
  {
    return Read(out, sizeof(T));
  }

private:
  std::span<const u8> m_rest;
};

bool ReadChunk(ByteReader& in, MeshDataChunk* chunk)
{
  if (!in.Read(&chunk->num_vertices) || !in.Read(&chunk->vertex_stride))
    return false;
  const std::size_t vertex_bytes = chunk->num_vertices * chunk->vertex_stride;
  chunk->vertex_data = std::make_unique_for_overwrite<u8[]>(vertex_bytes);
  if (!in.Read(chunk->vertex_data.get(), vertex_bytes))
    return false;

  if (!in.Read(&chunk->num_indices))
    return false;
  chunk->indices = std::make_unique_for_overwrite<u16[]>(chunk->num_indices);
  if (!in.Read(chunk->indices.get(), std::size_t{chunk->num_indices} * sizeof(u16)))
    return false;

  if (!in.Read(&chunk->vertex_declaration) || !in.Read(&chunk->primitive_type) ||
      !in.Read(&chunk->components_available) || !in.Read(&chunk->minimum_position) ||
      !in.Read(&chunk->maximum_position) ||
      !in.Read(chunk->transform.data.data(), sizeof(chunk->transform.data)))
  {
    return false;
  }

  std::size_t name_size = 0;
  if (!in.Read(&name_size))
    return false;
  chunk->material_name.resize(name_size);
  return in.Read(chunk->material_name.data(), name_size);
}
}  // namespace

bool MeshData::FromJson(const VideoCommon::CustomAssetLibrary::AssetID& asset_id,
                        const picojson::object& json, MeshData* data)
{
  const auto mapping = json.find("material_mapping");
  if (mapping == json.end())
    return true;

  if (!mapping->second.is<picojson::object>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' failed to parse json, expected 'material_mapping' to be of type "
                  "object",
                  asset_id);
    return false;
  }

  for (const auto& [material, target] : mapping->second.get<picojson::object>())
  {
    if (!target.is<std::string>())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Asset '{}' failed to parse json, material name '{}' linked to a non-string "
                    "value",
                    asset_id, material);
      return false;
    }
    data->m_mesh_material_to_material_asset_id[material] = target.get<std::string>();
  }
  return true;
}

void MeshData::ToJson(picojson::object& obj, const MeshData& data)
{
  picojson::object mapping;
  for (const auto& [material, target] : data.m_mesh_material_to_material_asset_id)
    mapping.emplace(material, target);
  obj.emplace("material_mapping", std::move(mapping));
}

bool MeshData::FromDolphinMesh(std::span<const u8> raw_data, MeshData* data)
{
  ByteReader in(raw_data);
  std::size_t chunk_count = 0;
  if (!in.Read(&chunk_count))
    return false;

  for (std::size_t i = 0; i < chunk_count; ++i)
  {
    MeshDataChunk chunk;
    if (!ReadChunk(in, &chunk))
      return false;
    data->m_mesh_chunks.push_back(std::move(chunk));
  }
  return true;
}

bool MeshData::ToDolphinMesh(File::IOFile* file_data, const MeshData& data)
{
  const std::size_t chunk_count = data.m_mesh_chunks.size();
  // The count is written unchecked; a failing file is caught by the first chunk write.
  file_data->WriteBytes(&chunk_count, sizeof(chunk_count));

  for (const MeshDataChunk& chunk : data.m_mesh_chunks)
  {
    const std::size_t name_size = chunk.material_name.size();
    const std::array<std::pair<const void*, std::size_t>, 13> pieces{{
        {&chunk.num_vertices, sizeof(u32)},
        {&chunk.vertex_stride, sizeof(u32)},
        {chunk.vertex_data.get(), chunk.num_vertices * chunk.vertex_stride},
        {&chunk.num_indices, sizeof(u32)},
        {chunk.indices.get(), chunk.num_indices * sizeof(u16)},
        {&chunk.vertex_declaration, sizeof(PortableVertexDeclaration)},
        {&chunk.primitive_type, sizeof(PrimitiveType)},
        {&chunk.components_available, sizeof(u32)},
        {&chunk.minimum_position, sizeof(Common::Vec3)},
        {&chunk.maximum_position, sizeof(Common::Vec3)},
        {chunk.transform.data.data(), sizeof(chunk.transform.data)},
        {&name_size, sizeof(name_size)},
        {chunk.material_name.data(), name_size},
    }};
    for (const auto& [bytes, size] : pieces)
    {
      if (!file_data->WriteBytes(bytes, size))
        return false;
    }
  }
  return true;
}

bool MeshData::FromGLTF(std::string_view gltf_file, MeshData* data)
{
  if (gltf_file.ends_with(".glb"))
  {
    ERROR_LOG_FMT(VIDEO, "File '{}' with glb extension is not supported at this time", gltf_file);
    return false;
  }
  if (!gltf_file.ends_with(".gltf"))
  {
    ERROR_LOG_FMT(VIDEO, "GLTF '{}' has invalid extension", gltf_file);
    return false;
  }

  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string errors;
  std::string warnings;
  if (!loader.LoadASCIIFromFile(&model, &errors, &warnings, std::string{gltf_file}))
  {
    ERROR_LOG_FMT(VIDEO, "File '{}' was invalid GLTF, error: {}, warning: {}", gltf_file, errors,
                  warnings);
    return false;
  }
  return ReadModel(gltf_file, model, data);
}

CustomAssetLibrary::LoadInfo MeshAsset::LoadImpl(const CustomAssetLibrary::AssetID& asset_id)
{
  return ReadAndPublish([&](MeshData* data) { return m_owning_library->LoadMesh(asset_id, data); });
}
}  // namespace VideoCommon
