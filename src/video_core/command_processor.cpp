// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/vector_math.h"
#include "core/hle/service/gsp_gpu.h"
#include "core/hw/gpu.h"
#include "core/memory.h"
#include "core/tracer/recorder.h"
#include "video_core/command_processor.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/pica_state.h"
#include "video_core/pica_types.h"
#include "video_core/primitive_assembly.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/regs.h"
#include "video_core/regs_pipeline.h"
#include "video_core/regs_texturing.h"
#include "video_core/renderer_base.h"
#include "video_core/shader/shader.h"
#include "video_core/vertex_loader.h"
#include "video_core/video_core.h"

namespace Pica {

namespace CommandProcessor {

static int vs_float_regs_counter = 0;
static u32 vs_uniform_write_buffer[4];

static int gs_float_regs_counter = 0;
static u32 gs_uniform_write_buffer[4];

static int default_attr_counter = 0;
static u32 default_attr_write_buffer[3];

// Expand a 4-bit mask to 4-byte mask, e.g. 0b0101 -> 0x00FF00FF
static const u32 expand_bits_to_bytes[] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

MICROPROFILE_DEFINE(GPU_Drawing, "GPU", "Drawing", MP_RGB(50, 50, 240));

static const char* GetShaderSetupTypeName(Shader::ShaderSetup& setup) {
    if (&setup == &g_state.vs) {
        return "vertex shader";
    }
    if (&setup == &g_state.gs) {
        return "geometry shader";
    }
    return "unknown shader";
}

static void WriteUniformBoolReg(Shader::ShaderSetup& setup, u32 value) {
    for (unsigned i = 0; i < setup.uniforms.b.size(); ++i)
        setup.uniforms.b[i] = (value & (1 << i)) != 0;
}

static void WriteUniformIntReg(Shader::ShaderSetup& setup, unsigned index,
                               const Math::Vec4<u8>& values) {
    ASSERT(index < setup.uniforms.i.size());
    setup.uniforms.i[index] = values;
    LOG_TRACE(HW_GPU, "Set %s integer uniform %d to %02x %02x %02x %02x",
              GetShaderSetupTypeName(setup), index, values.x, values.y, values.z, values.w);
}

static void WriteUniformFloatReg(ShaderRegs& config, Shader::ShaderSetup& setup,
                                 int& float_regs_counter, u32 uniform_write_buffer[4], u32 value) {
    auto& uniform_setup = config.uniform_setup;

    // TODO: Does actual hardware indeed keep an intermediate buffer or does
    //       it directly write the values?
    uniform_write_buffer[float_regs_counter++] = value;

    // Uniforms are written in a packed format such that four float24 values are encoded in
    // three 32-bit numbers. We write to internal memory once a full such vector is
    // written.
    if ((float_regs_counter >= 4 && uniform_setup.IsFloat32()) ||
        (float_regs_counter >= 3 && !uniform_setup.IsFloat32())) {
        float_regs_counter = 0;

        auto& uniform = setup.uniforms.f[uniform_setup.index];

        if (uniform_setup.index >= 96) {
            LOG_ERROR(HW_GPU, "Invalid %s float uniform index %d", GetShaderSetupTypeName(setup),
                      (int)uniform_setup.index);
        } else {

            // NOTE: The destination component order indeed is "backwards"
            if (uniform_setup.IsFloat32()) {
                for (auto i : {0, 1, 2, 3})
                    uniform[3 - i] = float24::FromFloat32(*(float*)(&uniform_write_buffer[i]));
            } else {
                // TODO: Untested
                uniform.w = float24::FromRaw(uniform_write_buffer[0] >> 8);
                uniform.z = float24::FromRaw(((uniform_write_buffer[0] & 0xFF) << 16) |
                                             ((uniform_write_buffer[1] >> 16) & 0xFFFF));
                uniform.y = float24::FromRaw(((uniform_write_buffer[1] & 0xFFFF) << 8) |
                                             ((uniform_write_buffer[2] >> 24) & 0xFF));
                uniform.x = float24::FromRaw(uniform_write_buffer[2] & 0xFFFFFF);
            }

            LOG_TRACE(HW_GPU, "Set %s float uniform %x to (%f %f %f %f)",
                      GetShaderSetupTypeName(setup), (int)uniform_setup.index,
                      uniform.x.ToFloat32(), uniform.y.ToFloat32(), uniform.z.ToFloat32(),
                      uniform.w.ToFloat32());

            // TODO: Verify that this actually modifies the register!
            uniform_setup.index.Assign(uniform_setup.index + 1);
        }
    }
}

static void WriteProgramCode(ShaderRegs& config, Shader::ShaderSetup& setup,
                             unsigned max_program_code_length, u32 value) {
    if (config.program.offset >= max_program_code_length) {
        LOG_ERROR(HW_GPU, "Invalid %s program offset %d", GetShaderSetupTypeName(setup),
                  (int)config.program.offset);
    } else {
        setup.program_code[config.program.offset] = value;
        config.program.offset++;
    }
}

static void WriteSwizzlePatterns(ShaderRegs& config, Shader::ShaderSetup& setup, u32 value) {
    if (config.swizzle_patterns.offset >= setup.swizzle_data.size()) {
        LOG_ERROR(HW_GPU, "Invalid %s swizzle pattern offset %d", GetShaderSetupTypeName(setup),
                  (int)config.swizzle_patterns.offset);
    } else {
        setup.swizzle_data[config.swizzle_patterns.offset] = value;
        config.swizzle_patterns.offset++;
    }
}

static void WritePicaReg(u32 id, u32 value, u32 mask) {
    auto& regs = g_state.regs;

    if (id >= Regs::NUM_REGS) {
        LOG_ERROR(HW_GPU,
                  "Commandlist tried to write to invalid register 0x%03X (value: %08X, mask: %X)",
                  id, value, mask);
        return;
    }

    // TODO: Figure out how register masking acts on e.g. vs.uniform_setup.set_value
    u32 old_value = regs.reg_array[id];

    const u32 write_mask = expand_bits_to_bytes[mask];

    regs.reg_array[id] = (old_value & ~write_mask) | (value & write_mask);

    // Double check for is_pica_tracing to avoid call overhead
    if (DebugUtils::IsPicaTracing()) {
        DebugUtils::OnPicaRegWrite({(u16)id, (u16)mask, regs.reg_array[id]});
    }

    if (g_debug_context)
        g_debug_context->OnEvent(DebugContext::Event::PicaCommandLoaded,
                                 reinterpret_cast<void*>(&id));

    switch (id) {
    // Trigger IRQ
    case PICA_REG_INDEX(trigger_irq):
        Service::GSP::SignalInterrupt(Service::GSP::InterruptId::P3D);
        break;

    case PICA_REG_INDEX(pipeline.triangle_topology):
        g_state.primitive_assembler.Reconfigure(regs.pipeline.triangle_topology);
        break;

    case PICA_REG_INDEX(pipeline.restart_primitive):
        g_state.primitive_assembler.Reset();
        break;

    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.index):
        g_state.immediate.current_attribute = 0;
        g_state.immediate.reset_geometry_pipeline = true;
        default_attr_counter = 0;
        break;

    // Load default vertex input attributes
    case PICA_REG_INDEX_WORKAROUND(pipeline.vs_default_attributes_setup.set_value[0], 0x233):
    case PICA_REG_INDEX_WORKAROUND(pipeline.vs_default_attributes_setup.set_value[1], 0x234):
    case PICA_REG_INDEX_WORKAROUND(pipeline.vs_default_attributes_setup.set_value[2], 0x235): {
        // TODO: Does actual hardware indeed keep an intermediate buffer or does
        //       it directly write the values?
        default_attr_write_buffer[default_attr_counter++] = value;

        // Default attributes are written in a packed format such that four float24 values are
        // encoded in
        // three 32-bit numbers. We write to internal memory once a full such vector is
        // written.
        if (default_attr_counter >= 3) {
            default_attr_counter = 0;

            auto& setup = regs.pipeline.vs_default_attributes_setup;

            if (setup.index >= 16) {
                LOG_ERROR(HW_GPU, "Invalid VS default attribute index %d", (int)setup.index);
                break;
            }

            Math::Vec4<float24> attribute;

            // NOTE: The destination component order indeed is "backwards"
            attribute.w = float24::FromRaw(default_attr_write_buffer[0] >> 8);
            attribute.z = float24::FromRaw(((default_attr_write_buffer[0] & 0xFF) << 16) |
                                           ((default_attr_write_buffer[1] >> 16) & 0xFFFF));
            attribute.y = float24::FromRaw(((default_attr_write_buffer[1] & 0xFFFF) << 8) |
                                           ((default_attr_write_buffer[2] >> 24) & 0xFF));
            attribute.x = float24::FromRaw(default_attr_write_buffer[2] & 0xFFFFFF);

            LOG_TRACE(HW_GPU, "Set default VS attribute %x to (%f %f %f %f)", (int)setup.index,
                      attribute.x.ToFloat32(), attribute.y.ToFloat32(), attribute.z.ToFloat32(),
                      attribute.w.ToFloat32());

            // TODO: Verify that this actually modifies the register!
            if (setup.index < 15) {
                g_state.input_default_attributes.attr[setup.index] = attribute;
                setup.index++;
            } else {
                // Put each attribute into an immediate input buffer.  When all specified immediate
                // attributes are present, the Vertex Shader is invoked and everything is sent to
                // the primitive assembler.

                auto& immediate_input = g_state.immediate.input_vertex;
                auto& immediate_attribute_id = g_state.immediate.current_attribute;

                immediate_input.attr[immediate_attribute_id] = attribute;

                if (immediate_attribute_id < regs.pipeline.max_input_attrib_index) {
                    immediate_attribute_id += 1;
                } else {
                    MICROPROFILE_SCOPE(GPU_Drawing);
                    immediate_attribute_id = 0;

                    auto* shader_engine = Shader::GetEngine();
                    shader_engine->SetupBatch(g_state.vs, regs.vs.main_offset);

                    // Send to vertex shader
                    if (g_debug_context)
                        g_debug_context->OnEvent(DebugContext::Event::VertexShaderInvocation,
                                                 static_cast<void*>(&immediate_input));
                    Shader::UnitState shader_unit;
                    Shader::AttributeBuffer output{};

                    shader_unit.LoadInput(regs.vs, immediate_input);
                    shader_engine->Run(g_state.vs, shader_unit);
                    shader_unit.WriteOutput(regs.vs, output);

                    // Send to geometry pipeline
                    if (g_state.immediate.reset_geometry_pipeline) {
                        g_state.geometry_pipeline.Reconfigure();
                        g_state.immediate.reset_geometry_pipeline = false;
                    }
                    ASSERT(!g_state.geometry_pipeline.NeedIndexInput());
                    g_state.geometry_pipeline.Setup(shader_engine);
                    g_state.geometry_pipeline.SubmitVertex(output);
                }
            }
        }
        break;
    }

    case PICA_REG_INDEX(pipeline.gpu_mode):
        if (regs.pipeline.gpu_mode == PipelineRegs::GPUMode::Configuring) {
            MICROPROFILE_SCOPE(GPU_Drawing);

            // Draw immediate mode triangles when GPU Mode is set to GPUMode::Configuring
            VideoCore::g_renderer->Rasterizer()->DrawTriangles();

            if (g_debug_context) {
                g_debug_context->OnEvent(DebugContext::Event::FinishedPrimitiveBatch, nullptr);
            }
        }
        break;

    case PICA_REG_INDEX_WORKAROUND(pipeline.command_buffer.trigger[0], 0x23c):
    case PICA_REG_INDEX_WORKAROUND(pipeline.command_buffer.trigger[1], 0x23d): {
        unsigned index =
            static_cast<unsigned>(id - PICA_REG_INDEX(pipeline.command_buffer.trigger[0]));
        u32* head_ptr = (u32*)Memory::GetPhysicalPointer(
            regs.pipeline.command_buffer.GetPhysicalAddress(index));
        g_state.cmd_list.head_ptr = g_state.cmd_list.current_ptr = head_ptr;
        g_state.cmd_list.length = regs.pipeline.command_buffer.GetSize(index) / sizeof(u32);
        break;
    }

    // It seems like these trigger vertex rendering
    case PICA_REG_INDEX(pipeline.trigger_draw):
    case PICA_REG_INDEX(pipeline.trigger_draw_indexed): {
        MICROPROFILE_SCOPE(GPU_Drawing);

#if PICA_LOG_TEV
        DebugUtils::DumpTevStageConfig(regs.GetTevStages());
#endif
        if (g_debug_context)
            g_debug_context->OnEvent(DebugContext::Event::IncomingPrimitiveBatch, nullptr);

        // Processes information about internal vertex attributes to figure out how a vertex is
        // loaded.
        // Later, these can be compiled and cached.
        const u32 base_address = regs.pipeline.vertex_attributes.GetPhysicalBaseAddress();
        VertexLoader loader(regs.pipeline);

        // Load vertices
        bool is_indexed = (id == PICA_REG_INDEX(pipeline.trigger_draw_indexed));

        const auto& index_info = regs.pipeline.index_array;
        const u8* index_address_8 = Memory::GetPhysicalPointer(base_address + index_info.offset);
        const u16* index_address_16 = reinterpret_cast<const u16*>(index_address_8);
        bool index_u16 = index_info.format != 0;

        PrimitiveAssembler<Shader::OutputVertex>& primitive_assembler = g_state.primitive_assembler;

        if (g_debug_context && g_debug_context->recorder) {
            for (int i = 0; i < 3; ++i) {
                const auto texture = regs.texturing.GetTextures()[i];
                if (!texture.enabled)
                    continue;

                u8* texture_data = Memory::GetPhysicalPointer(texture.config.GetPhysicalAddress());
                g_debug_context->recorder->MemoryAccessed(
                    texture_data, Pica::TexturingRegs::NibblesPerPixel(texture.format) *
                                      texture.config.width / 2 * texture.config.height,
                    texture.config.GetPhysicalAddress());
            }
        }

        DebugUtils::MemoryAccessTracker memory_accesses;

        // Simple circular-replacement vertex cache
        // The size has been tuned for optimal balance between hit-rate and the cost of lookup
        const size_t VERTEX_CACHE_SIZE = 32;
        std::array<u16, VERTEX_CACHE_SIZE> vertex_cache_ids;
        std::array<Shader::AttributeBuffer, VERTEX_CACHE_SIZE> vertex_cache;
        Shader::AttributeBuffer vs_output;

        unsigned int vertex_cache_pos = 0;
        vertex_cache_ids.fill(-1);

        auto* shader_engine = Shader::GetEngine();
        Shader::UnitState shader_unit;

        shader_engine->SetupBatch(g_state.vs, regs.vs.main_offset);

        g_state.geometry_pipeline.Reconfigure();
        g_state.geometry_pipeline.Setup(shader_engine);
        if (g_state.geometry_pipeline.NeedIndexInput())
            ASSERT(is_indexed);

        for (unsigned int index = 0; index < regs.pipeline.num_vertices; ++index) {
            // Indexed rendering doesn't use the start offset
            unsigned int vertex =
                is_indexed ? (index_u16 ? index_address_16[index] : index_address_8[index])
                           : (index + regs.pipeline.vertex_offset);

            // -1 is a common special value used for primitive restart. Since it's unknown if
            // the PICA supports it, and it would mess up the caching, guard against it here.
            ASSERT(vertex != -1);

            bool vertex_cache_hit = false;

            if (is_indexed) {
                if (g_state.geometry_pipeline.NeedIndexInput()) {
                    g_state.geometry_pipeline.SubmitIndex(vertex);
                    continue;
                }

                if (g_debug_context && Pica::g_debug_context->recorder) {
                    int size = index_u16 ? 2 : 1;
                    memory_accesses.AddAccess(base_address + index_info.offset + size * index,
                                              size);
                }

                for (unsigned int i = 0; i < VERTEX_CACHE_SIZE; ++i) {
                    if (vertex == vertex_cache_ids[i]) {
                        vs_output = vertex_cache[i];
                        vertex_cache_hit = true;
                        break;
                    }
                }
            }

            if (!vertex_cache_hit) {
                // Initialize data for the current vertex
                Shader::AttributeBuffer input;
                loader.LoadVertex(base_address, index, vertex, input, memory_accesses);

                // Send to vertex shader
                if (g_debug_context)
                    g_debug_context->OnEvent(DebugContext::Event::VertexShaderInvocation,
                                             (void*)&input);
                shader_unit.LoadInput(regs.vs, input);
                shader_engine->Run(g_state.vs, shader_unit);
                shader_unit.WriteOutput(regs.vs, vs_output);

                if (is_indexed) {
                    vertex_cache[vertex_cache_pos] = vs_output;
                    vertex_cache_ids[vertex_cache_pos] = vertex;
                    vertex_cache_pos = (vertex_cache_pos + 1) % VERTEX_CACHE_SIZE;
                }
            }

            // Send to geometry pipeline
            g_state.geometry_pipeline.SubmitVertex(vs_output);
        }

        for (auto& range : memory_accesses.ranges) {
            g_debug_context->recorder->MemoryAccessed(Memory::GetPhysicalPointer(range.first),
                                                      range.second, range.first);
        }

        VideoCore::g_renderer->Rasterizer()->DrawTriangles();
        if (g_debug_context) {
            g_debug_context->OnEvent(DebugContext::Event::FinishedPrimitiveBatch, nullptr);
        }

        break;
    }

    case PICA_REG_INDEX(gs.bool_uniforms):
        WriteUniformBoolReg(g_state.gs, g_state.regs.gs.bool_uniforms.Value());
        break;

    case PICA_REG_INDEX_WORKAROUND(gs.int_uniforms[0], 0x281):
    case PICA_REG_INDEX_WORKAROUND(gs.int_uniforms[1], 0x282):
    case PICA_REG_INDEX_WORKAROUND(gs.int_uniforms[2], 0x283):
    case PICA_REG_INDEX_WORKAROUND(gs.int_uniforms[3], 0x284): {
        unsigned index = (id - PICA_REG_INDEX_WORKAROUND(gs.int_uniforms[0], 0x281));
        auto values = regs.gs.int_uniforms[index];
        WriteUniformIntReg(g_state.gs, index,
                           Math::Vec4<u8>(values.x, values.y, values.z, values.w));
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(gs.uniform_setup.set_value[0], 0x291):
    case PICA_REG_INDEX_WORKAROUND(gs.uniform_setup.set_value[1], 0x292):
    case PICA_REG_INDEX_WORKAROUND(gs.uniform_setup.set_value[2], 0x293):
    case PICA_REG_INDEX_WORKAROUND(gs.uniform_setup.set_value[3], 0x294):
    case PICA_REG_INDEX_WORKAROUND(gs.uniform_setup.set_value[4], 0x295):
    case PICA_REG_INDEX_WORKAROUND(gs.uniform_setup.set_value[5], 0x296):
    case PICA_REG_INDEX_WORKAROUND(gs.uniform_setup.set_value[6], 0x297):
    case PICA_REG_INDEX_WORKAROUND(gs.uniform_setup.set_value[7], 0x298): {
        WriteUniformFloatReg(g_state.regs.gs, g_state.gs, gs_float_regs_counter,
                             gs_uniform_write_buffer, value);
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(gs.program.set_word[0], 0x29c):
    case PICA_REG_INDEX_WORKAROUND(gs.program.set_word[1], 0x29d):
    case PICA_REG_INDEX_WORKAROUND(gs.program.set_word[2], 0x29e):
    case PICA_REG_INDEX_WORKAROUND(gs.program.set_word[3], 0x29f):
    case PICA_REG_INDEX_WORKAROUND(gs.program.set_word[4], 0x2a0):
    case PICA_REG_INDEX_WORKAROUND(gs.program.set_word[5], 0x2a1):
    case PICA_REG_INDEX_WORKAROUND(gs.program.set_word[6], 0x2a2):
    case PICA_REG_INDEX_WORKAROUND(gs.program.set_word[7], 0x2a3): {
        WriteProgramCode(g_state.regs.gs, g_state.gs, 4096, value);
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(gs.swizzle_patterns.set_word[0], 0x2a6):
    case PICA_REG_INDEX_WORKAROUND(gs.swizzle_patterns.set_word[1], 0x2a7):
    case PICA_REG_INDEX_WORKAROUND(gs.swizzle_patterns.set_word[2], 0x2a8):
    case PICA_REG_INDEX_WORKAROUND(gs.swizzle_patterns.set_word[3], 0x2a9):
    case PICA_REG_INDEX_WORKAROUND(gs.swizzle_patterns.set_word[4], 0x2aa):
    case PICA_REG_INDEX_WORKAROUND(gs.swizzle_patterns.set_word[5], 0x2ab):
    case PICA_REG_INDEX_WORKAROUND(gs.swizzle_patterns.set_word[6], 0x2ac):
    case PICA_REG_INDEX_WORKAROUND(gs.swizzle_patterns.set_word[7], 0x2ad): {
        WriteSwizzlePatterns(g_state.regs.gs, g_state.gs, value);
        break;
    }

    case PICA_REG_INDEX(vs.bool_uniforms):
        WriteUniformBoolReg(g_state.vs, g_state.regs.vs.bool_uniforms.Value());
        break;

    case PICA_REG_INDEX_WORKAROUND(vs.int_uniforms[0], 0x2b1):
    case PICA_REG_INDEX_WORKAROUND(vs.int_uniforms[1], 0x2b2):
    case PICA_REG_INDEX_WORKAROUND(vs.int_uniforms[2], 0x2b3):
    case PICA_REG_INDEX_WORKAROUND(vs.int_uniforms[3], 0x2b4): {
        unsigned index = (id - PICA_REG_INDEX_WORKAROUND(vs.int_uniforms[0], 0x2b1));
        auto values = regs.vs.int_uniforms[index];
        WriteUniformIntReg(g_state.vs, index,
                           Math::Vec4<u8>(values.x, values.y, values.z, values.w));
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(vs.uniform_setup.set_value[0], 0x2c1):
    case PICA_REG_INDEX_WORKAROUND(vs.uniform_setup.set_value[1], 0x2c2):
    case PICA_REG_INDEX_WORKAROUND(vs.uniform_setup.set_value[2], 0x2c3):
    case PICA_REG_INDEX_WORKAROUND(vs.uniform_setup.set_value[3], 0x2c4):
    case PICA_REG_INDEX_WORKAROUND(vs.uniform_setup.set_value[4], 0x2c5):
    case PICA_REG_INDEX_WORKAROUND(vs.uniform_setup.set_value[5], 0x2c6):
    case PICA_REG_INDEX_WORKAROUND(vs.uniform_setup.set_value[6], 0x2c7):
    case PICA_REG_INDEX_WORKAROUND(vs.uniform_setup.set_value[7], 0x2c8): {
        WriteUniformFloatReg(g_state.regs.vs, g_state.vs, vs_float_regs_counter,
                             vs_uniform_write_buffer, value);
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(vs.program.set_word[0], 0x2cc):
    case PICA_REG_INDEX_WORKAROUND(vs.program.set_word[1], 0x2cd):
    case PICA_REG_INDEX_WORKAROUND(vs.program.set_word[2], 0x2ce):
    case PICA_REG_INDEX_WORKAROUND(vs.program.set_word[3], 0x2cf):
    case PICA_REG_INDEX_WORKAROUND(vs.program.set_word[4], 0x2d0):
    case PICA_REG_INDEX_WORKAROUND(vs.program.set_word[5], 0x2d1):
    case PICA_REG_INDEX_WORKAROUND(vs.program.set_word[6], 0x2d2):
    case PICA_REG_INDEX_WORKAROUND(vs.program.set_word[7], 0x2d3): {
        WriteProgramCode(g_state.regs.vs, g_state.vs, 512, value);
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(vs.swizzle_patterns.set_word[0], 0x2d6):
    case PICA_REG_INDEX_WORKAROUND(vs.swizzle_patterns.set_word[1], 0x2d7):
    case PICA_REG_INDEX_WORKAROUND(vs.swizzle_patterns.set_word[2], 0x2d8):
    case PICA_REG_INDEX_WORKAROUND(vs.swizzle_patterns.set_word[3], 0x2d9):
    case PICA_REG_INDEX_WORKAROUND(vs.swizzle_patterns.set_word[4], 0x2da):
    case PICA_REG_INDEX_WORKAROUND(vs.swizzle_patterns.set_word[5], 0x2db):
    case PICA_REG_INDEX_WORKAROUND(vs.swizzle_patterns.set_word[6], 0x2dc):
    case PICA_REG_INDEX_WORKAROUND(vs.swizzle_patterns.set_word[7], 0x2dd): {
        WriteSwizzlePatterns(g_state.regs.vs, g_state.vs, value);
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(lighting.lut_data[0], 0x1c8):
    case PICA_REG_INDEX_WORKAROUND(lighting.lut_data[1], 0x1c9):
    case PICA_REG_INDEX_WORKAROUND(lighting.lut_data[2], 0x1ca):
    case PICA_REG_INDEX_WORKAROUND(lighting.lut_data[3], 0x1cb):
    case PICA_REG_INDEX_WORKAROUND(lighting.lut_data[4], 0x1cc):
    case PICA_REG_INDEX_WORKAROUND(lighting.lut_data[5], 0x1cd):
    case PICA_REG_INDEX_WORKAROUND(lighting.lut_data[6], 0x1ce):
    case PICA_REG_INDEX_WORKAROUND(lighting.lut_data[7], 0x1cf): {
        auto& lut_config = regs.lighting.lut_config;

        ASSERT_MSG(lut_config.index < 256, "lut_config.index exceeded maximum value of 255!");

        g_state.lighting.luts[lut_config.type][lut_config.index].raw = value;
        lut_config.index.Assign(lut_config.index + 1);
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(texturing.fog_lut_data[0], 0xe8):
    case PICA_REG_INDEX_WORKAROUND(texturing.fog_lut_data[1], 0xe9):
    case PICA_REG_INDEX_WORKAROUND(texturing.fog_lut_data[2], 0xea):
    case PICA_REG_INDEX_WORKAROUND(texturing.fog_lut_data[3], 0xeb):
    case PICA_REG_INDEX_WORKAROUND(texturing.fog_lut_data[4], 0xec):
    case PICA_REG_INDEX_WORKAROUND(texturing.fog_lut_data[5], 0xed):
    case PICA_REG_INDEX_WORKAROUND(texturing.fog_lut_data[6], 0xee):
    case PICA_REG_INDEX_WORKAROUND(texturing.fog_lut_data[7], 0xef): {
        g_state.fog.lut[regs.texturing.fog_lut_offset % 128].raw = value;
        regs.texturing.fog_lut_offset.Assign(regs.texturing.fog_lut_offset + 1);
        break;
    }

    case PICA_REG_INDEX_WORKAROUND(texturing.proctex_lut_data[0], 0xb0):
    case PICA_REG_INDEX_WORKAROUND(texturing.proctex_lut_data[1], 0xb1):
    case PICA_REG_INDEX_WORKAROUND(texturing.proctex_lut_data[2], 0xb2):
    case PICA_REG_INDEX_WORKAROUND(texturing.proctex_lut_data[3], 0xb3):
    case PICA_REG_INDEX_WORKAROUND(texturing.proctex_lut_data[4], 0xb4):
    case PICA_REG_INDEX_WORKAROUND(texturing.proctex_lut_data[5], 0xb5):
    case PICA_REG_INDEX_WORKAROUND(texturing.proctex_lut_data[6], 0xb6):
    case PICA_REG_INDEX_WORKAROUND(texturing.proctex_lut_data[7], 0xb7): {
        auto& index = regs.texturing.proctex_lut_config.index;
        auto& pt = g_state.proctex;

        switch (regs.texturing.proctex_lut_config.ref_table.Value()) {
        case TexturingRegs::ProcTexLutTable::Noise:
            pt.noise_table[index % pt.noise_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::ColorMap:
            pt.color_map_table[index % pt.color_map_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::AlphaMap:
            pt.alpha_map_table[index % pt.alpha_map_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::Color:
            pt.color_table[index % pt.color_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::ColorDiff:
            pt.color_diff_table[index % pt.color_diff_table.size()].raw = value;
            break;
        }
        index.Assign(index + 1);
        break;
    }
    default:
        break;
    }

    VideoCore::g_renderer->Rasterizer()->NotifyPicaRegisterChanged(id);

    if (g_debug_context)
        g_debug_context->OnEvent(DebugContext::Event::PicaCommandProcessed,
                                 reinterpret_cast<void*>(&id));
}

void ProcessCommandList(const u32* list, u32 size) {
    g_state.cmd_list.head_ptr = g_state.cmd_list.current_ptr = list;
    g_state.cmd_list.length = size / sizeof(u32);

    while (g_state.cmd_list.current_ptr < g_state.cmd_list.head_ptr + g_state.cmd_list.length) {

        // Align read pointer to 8 bytes
        if ((g_state.cmd_list.head_ptr - g_state.cmd_list.current_ptr) % 2 != 0)
            ++g_state.cmd_list.current_ptr;

        u32 value = *g_state.cmd_list.current_ptr++;
        const CommandHeader header = {*g_state.cmd_list.current_ptr++};

        WritePicaReg(header.cmd_id, value, header.parameter_mask);

        for (unsigned i = 0; i < header.extra_data_length; ++i) {
            u32 cmd = header.cmd_id + (header.group_commands ? i + 1 : 0);
            WritePicaReg(cmd, *g_state.cmd_list.current_ptr++, header.parameter_mask);
        }
    }
}

} // namespace

} // namespace
