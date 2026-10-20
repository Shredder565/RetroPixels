// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Some X11 headers #define None, which would break the enumerators named None below.
#undef None

#include <array>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "VideoCommon/CPMemory.h"

// Transform unit (XF) memory: matrices and lights at 0x0000-0x067f, registers at
// 0x1000-0x1057, all addressed in 32-bit words.

constexpr size_t NUM_XF_COLOR_CHANNELS = 2;

// ---------------------------------------------------------------------------------------------
// Field types

// Texture coordinate generation: size of the texture matrix.
enum class TexSize : u8
{
  ST = 0,
  STQ = 1
};
template <>
struct fmt::formatter<TexSize> : EnumFormatter<TexSize::STQ>
{
  constexpr formatter() : EnumFormatter({"ST (2x4 matrix)", "STQ (3x4 matrix)"}) {}
};

// Texture coordinate generation: shape of the input vector.
enum class TexInputForm : u8
{
  AB11 = 0,
  ABC1 = 1
};
template <>
struct fmt::formatter<TexInputForm> : EnumFormatter<TexInputForm::ABC1>
{
  constexpr formatter() : EnumFormatter({"AB11", "ABC1"}) {}
};

// Normals supplied per vertex.
enum class NormalCount : u32
{
  None = 0,
  Normal = 1,
  NormalTangentBinormal = 2,
  // Not an intended setting, but on hardware it acts like NormalTangentBinormal (at least one
  // commercial game sets it).
  Invalid = 3,
};
template <>
struct fmt::formatter<NormalCount> : EnumFormatter<NormalCount::Invalid>
{
  static constexpr array_type names = {
      "None",
      "Normal only",
      "Normal, tangent, and binormal",
      "Invalid (Normal, tangent, and binormal)",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// How a texture coordinate is produced.
enum class TexGenType : u8
{
  Regular = 0,
  EmbossMap = 1,  // bump mapping
  Color0 = 2,
  Color1 = 3
};
template <>
struct fmt::formatter<TexGenType> : EnumFormatter<TexGenType::Color1>
{
  static constexpr array_type names = {
      "Regular",
      "Emboss map (used when bump mapping)",
      "Color channel 0",
      "Color channel 1",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// Vertex attribute a texture coordinate is generated from. Geom, Normal and the binormals are
// three-component inputs.
enum class SourceRow : u8
{
  Geom = 0,
  Normal = 1,
  Colors = 2,
  BinormalT = 3,
  BinormalB = 4,
  Tex0 = 5,
  Tex1 = 6,
  Tex2 = 7,
  Tex3 = 8,
  Tex4 = 9,
  Tex5 = 10,
  Tex6 = 11,
  Tex7 = 12
};
template <>
struct fmt::formatter<SourceRow> : EnumFormatter<SourceRow::Tex7>
{
  static constexpr array_type names = {
      "Geometry (input is ABC1)",
      "Normal (input is ABC1)",
      "Colors",
      "Binormal T (input is ABC1)",
      "Binormal B (input is ABC1)",
      "Tex 0",
      "Tex 1",
      "Tex 2",
      "Tex 3",
      "Tex 4",
      "Tex 5",
      "Tex 6",
      "Tex 7",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// Lighting channel: where the material colour comes from.
enum class MatSource : u32
{
  MatColorRegister = 0,
  Vertex = 1,
};
template <>
struct fmt::formatter<MatSource> : EnumFormatter<MatSource::Vertex>
{
  constexpr formatter() : EnumFormatter({"Material color register", "Vertex color"}) {}
};

// Lighting channel: where the ambient colour comes from.
enum class AmbSource : u32
{
  AmbColorRegister = 0,
  Vertex = 1,
};
template <>
struct fmt::formatter<AmbSource> : EnumFormatter<AmbSource::Vertex>
{
  constexpr formatter() : EnumFormatter({"Ambient color register", "Vertex color"}) {}
};

// Treatment of the diffuse term N.L.
enum class DiffuseFunc : u32
{
  None = 0,
  Sign = 1,
  Clamp = 2
};
template <>
struct fmt::formatter<DiffuseFunc> : EnumFormatter<DiffuseFunc::Clamp>
{
  constexpr formatter() : EnumFormatter({"None", "Sign", "Clamp"}) {}
};

// Light attenuation model.
enum class AttenuationFunc : u32
{
  None = 0,
  Spec = 1,  // point lights
  Dir = 2,   // directional lights
  Spot = 3   // spot lights
};
template <>
struct fmt::formatter<AttenuationFunc> : EnumFormatter<AttenuationFunc::Spot>
{
  static constexpr array_type names = {
      "No attenuation",
      "Point light attenuation",
      "Directional light attenuation",
      "Spot light attenuation",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

enum class ProjectionType : u32
{
  Perspective = 0,
  Orthographic = 1
};
template <>
struct fmt::formatter<ProjectionType> : EnumFormatter<ProjectionType::Orthographic>
{
  constexpr formatter() : EnumFormatter({"Perspective", "Orthographic"}) {}
};

// ---------------------------------------------------------------------------------------------
// Address map

enum
{
  XFMEM_POSMATRICES = 0x000,
  XFMEM_POSMATRICES_END = 0x100,
  XFMEM_NORMALMATRICES = 0x400,
  XFMEM_NORMALMATRICES_END = 0x460,
  XFMEM_POSTMATRICES = 0x500,
  XFMEM_POSTMATRICES_END = 0x600,
  XFMEM_LIGHTS = 0x600,
  XFMEM_LIGHTS_END = 0x680,
  XFMEM_REGISTERS_START = 0x1000,
  XFMEM_ERROR = 0x1000,
  XFMEM_DIAG = 0x1001,
  XFMEM_STATE0 = 0x1002,
  XFMEM_STATE1 = 0x1003,
  XFMEM_CLOCK = 0x1004,
  XFMEM_CLIPDISABLE = 0x1005,
  XFMEM_SETGPMETRIC = 0x1006,   // performance counter 0
  XFMEM_UNKNOWN_1007 = 0x1007,  // presumably performance counter 1
  XFMEM_VTXSPECS = 0x1008,
  XFMEM_SETNUMCHAN = 0x1009,
  XFMEM_SETCHAN0_AMBCOLOR = 0x100a,
  XFMEM_SETCHAN1_AMBCOLOR = 0x100b,
  XFMEM_SETCHAN0_MATCOLOR = 0x100c,
  XFMEM_SETCHAN1_MATCOLOR = 0x100d,
  XFMEM_SETCHAN0_COLOR = 0x100e,
  XFMEM_SETCHAN1_COLOR = 0x100f,
  XFMEM_SETCHAN0_ALPHA = 0x1010,
  XFMEM_SETCHAN1_ALPHA = 0x1011,
  XFMEM_DUALTEX = 0x1012,
  XFMEM_UNKNOWN_GROUP_1_START = 0x1013,
  XFMEM_UNKNOWN_GROUP_1_END = 0x1017,
  XFMEM_SETMATRIXINDA = 0x1018,
  XFMEM_SETMATRIXINDB = 0x1019,
  XFMEM_SETVIEWPORT = 0x101a,    // six words
  XFMEM_SETPROJECTION = 0x1020,  // six parameters, then the projection type at 0x1026
  XFMEM_UNKNOWN_GROUP_2_START = 0x1027,
  XFMEM_UNKNOWN_GROUP_2_END = 0x103e,
  XFMEM_SETNUMTEXGENS = 0x103f,
  XFMEM_SETTEXMTXINFO = 0x1040,  // eight words
  XFMEM_UNKNOWN_GROUP_3_START = 0x1048,
  XFMEM_UNKNOWN_GROUP_3_END = 0x104f,
  XFMEM_SETPOSTMTXINFO = 0x1050,  // eight words
  XFMEM_REGISTERS_END = 0x1058,
};

// ---------------------------------------------------------------------------------------------
// Registers

// Lighting channel configuration (0x100e-0x1011).
union LitChannel
{
  BitField<0, 1, MatSource> matsource;
  BitField<1, 1, bool, u32> enablelighting;
  BitField<2, 4, u32> lightMask0_3;
  BitField<6, 1, AmbSource> ambsource;
  BitField<7, 2, DiffuseFunc> diffusefunc;
  BitField<9, 2, AttenuationFunc> attnfunc;
  BitField<11, 4, u32> lightMask4_7;
  u32 hex;

  // Lights 0-7 as one mask; empty when lighting is off.
  unsigned int GetFullLightMask() const
  {
    if (!enablelighting)
      return 0;
    return (lightMask4_7 << 4) | lightMask0_3;
  }
};
template <>
struct fmt::formatter<LitChannel>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const LitChannel& c, FormatContext& ctx) const
  {
    const unsigned int mask = c.GetFullLightMask();
    auto out = fmt::format_to(ctx.out(), "Material source: {}\nEnable lighting: {}\n",
                              c.matsource, c.enablelighting ? "Yes" : "No");
    out = fmt::format_to(out, "Light mask: {:x} ({:08b})\n", mask, mask);
    return fmt::format_to(out, "Ambient source: {}\nDiffuse function: {}\nAttenuation function: {}",
                          c.ambsource, c.diffusefunc, c.attnfunc);
  }
};

// 0x1005; all three bits are reported but not emulated.
union ClipDisable
{
  BitField<0, 1, bool, u32> disable_clipping_detection;
  BitField<1, 1, bool, u32> disable_trivial_rejection;
  BitField<2, 1, bool, u32> disable_cpoly_clipping_acceleration;
  u32 hex;
};
template <>
struct fmt::formatter<ClipDisable>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const ClipDisable& bits, FormatContext& ctx) const
  {
    const auto yes_no = [](bool b) { return b ? "Yes" : "No"; };
    return fmt::format_to(
        ctx.out(),
        "Disable clipping detection: {}\nDisable trivial rejection: {}\n"
        "Disable cpoly clipping acceleration: {}",
        yes_no(bits.disable_clipping_detection), yes_no(bits.disable_trivial_rejection),
        yes_no(bits.disable_cpoly_clipping_acceleration));
  }
};

// 0x1008: what each vertex carries into the transform unit.
union INVTXSPEC
{
  BitField<0, 2, u32> numcolors;
  BitField<2, 2, NormalCount> numnormals;
  BitField<4, 4, u32> numtextures;
  u32 hex;
};
template <>
struct fmt::formatter<INVTXSPEC>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const INVTXSPEC& s, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Num colors: {}\nNum normals: {}\nNum textures: {}",
                          s.numcolors, s.numnormals, s.numtextures);
  }
};

// 0x1040-0x1047: texture coordinate generation, one per texgen.
union TexMtxInfo
{
  BitField<0, 1, u32> unknown;
  BitField<1, 1, TexSize, u32> projection;
  BitField<2, 1, TexInputForm, u32> inputform;
  BitField<3, 1, u32> unknown2;
  BitField<4, 3, TexGenType, u32> texgentype;
  BitField<7, 5, SourceRow, u32> sourcerow;
  BitField<12, 3, u32> embosssourceshift;  // texgen whose result is embossed
  BitField<15, 3, u32> embosslightshift;   // light used for embossing
  u32 hex;
};
template <>
struct fmt::formatter<TexMtxInfo>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const TexMtxInfo& t, FormatContext& ctx) const
  {
    auto out = fmt::format_to(ctx.out(), "Projection: {}\nInput form: {}\nTex gen type: {}\n",
                              t.projection, t.inputform, t.texgentype);
    return fmt::format_to(out, "Source row: {}\nEmboss source shift: {}\nEmboss light shift: {}",
                          t.sourcerow, t.embosssourceshift, t.embosslightshift);
  }
};

// 0x1050-0x1057: the second ("dual") texture transform, one per texgen.
union PostMtxInfo
{
  BitField<0, 6, u32> index;  // first row of the post-transform matrix
  BitField<6, 2, u32> unused;
  BitField<8, 1, bool, u32> normalize;  // normalize the coordinate before the second transform
  u32 hex;
};
template <>
struct fmt::formatter<PostMtxInfo>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const PostMtxInfo& t, FormatContext& ctx) const
  {
    const char* const normalize = t.normalize ? "Yes" : "No";
    return fmt::format_to(ctx.out(), "Index: {}\nNormalize before send operation: {}", t.index,
                          normalize);
  }
};

union NumColorChannel  // 0x1009
{
  BitField<0, 2, u32> numColorChans;
  u32 hex;
};

union NumTexGen  // 0x103f
{
  BitField<0, 4, u32> numTexGens;
  u32 hex;
};

union DualTexInfo  // 0x1012
{
  BitField<0, 1, bool, u32> enabled;
  u32 hex;
};

// ---------------------------------------------------------------------------------------------
// Memory layout

// One of the eight hardware lights: sixteen words.
struct Light
{
  u32 useless[3];
  u8 color[4];
  float cosatt[3];   // angular attenuation coefficients
  float distatt[3];  // distance attenuation coefficients

  // The last six words are read either as position + direction or, for specular lights, as
  // direction + half angle.
  union
  {
    struct
    {
      float dpos[3];
      float ddir[3];
    };

    struct
    {
      float sdir[3];
      float shalfangle[3];
    };
  };
};

struct Viewport
{
  float wd;
  float ht;
  float zRange;
  float xOrig;
  float yOrig;
  float farZ;
};

struct Projection
{
  using Raw = std::array<float, 6>;

  Raw rawProjection;
  ProjectionType type;
};

// The whole XF address space, so a word address indexes it directly as u32s.
struct alignas(16) XFMemory
{
  float posMatrices[256];                   // 0x0000
  u32 unk0[768];                            // 0x0100
  float normalMatrices[96];                 // 0x0400
  u32 unk1[160];                            // 0x0460
  float postMatrices[256];                  // 0x0500
  Light lights[8];                          // 0x0600
  u32 unk2[2432];                           // 0x0680
  u32 error;                                // 0x1000
  u32 diag;                                 // 0x1001
  u32 state0;                               // 0x1002
  u32 state1;                               // 0x1003
  u32 xfClock;                              // 0x1004
  ClipDisable clipDisable;                  // 0x1005
  u32 perf0;                                // 0x1006
  u32 perf1;                                // 0x1007
  INVTXSPEC invtxspec;                      // 0x1008
  NumColorChannel numChan;                  // 0x1009
  u32 ambColor[NUM_XF_COLOR_CHANNELS];      // 0x100a
  u32 matColor[NUM_XF_COLOR_CHANNELS];      // 0x100c
  LitChannel color[NUM_XF_COLOR_CHANNELS];  // 0x100e
  LitChannel alpha[NUM_XF_COLOR_CHANNELS];  // 0x1010
  DualTexInfo dualTexTrans;                 // 0x1012
  u32 unk3;                                 // 0x1013
  u32 unk4;                                 // 0x1014
  u32 unk5;                                 // 0x1015
  u32 unk6;                                 // 0x1016
  u32 unk7;                                 // 0x1017
  TMatrixIndexA MatrixIndexA;               // 0x1018
  TMatrixIndexB MatrixIndexB;               // 0x1019
  Viewport viewport;                        // 0x101a
  Projection projection;                    // 0x1020
  u32 unk8[24];                             // 0x1027
  NumTexGen numTexGen;                      // 0x103f
  TexMtxInfo texMtxInfo[8];                 // 0x1040
  u32 unk9[8];                              // 0x1048
  PostMtxInfo postMtxInfo[8];               // 0x1050
};
static_assert(sizeof(XFMemory) == sizeof(u32) * XFMEM_REGISTERS_END);

extern XFMemory xfmem;

// Command handlers, defined in XFStructs.cpp.
void LoadXFReg(u16 base_address, u8 transfer_size, const u8* data);
void LoadIndexedXF(CPArray array, u32 index, u16 address, u8 size);
void PreprocessIndexedXF(CPArray array, u32 index, u16 address, u8 size);
