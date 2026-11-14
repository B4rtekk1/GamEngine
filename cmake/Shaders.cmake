# Compiles one Slang entry module to SPIR-V and appends the output to the
# caller's `shader_outputs`.  slangc writes a depfile listing every imported
# module, so editing one shared .slang rebuilds only the shaders importing it.
#   SOURCE <file> OUTPUT <file> [PROFILE <profile>] [COMMENT <text>]
#   [DEFINES NAME=VALUE...] [EXTRA_ARGS <slangc args>...]
function(gameengine_compile_shader)
    cmake_parse_arguments(SHADER "" "SOURCE;OUTPUT;PROFILE;COMMENT" "DEFINES;EXTRA_ARGS" ${ARGN})
    if(NOT SHADER_PROFILE)
        set(SHADER_PROFILE glsl_460)
    endif()
    if(NOT SHADER_COMMENT)
        file(RELATIVE_PATH shader_relative "${ENGINE_SHADER_SOURCE_DIR}" "${SHADER_SOURCE}")
        set(SHADER_COMMENT "Compiling Slang shader ${shader_relative}")
    endif()
    set(shader_defines)
    foreach(shader_define IN LISTS SHADER_DEFINES)
        list(APPEND shader_defines -D "${shader_define}")
    endforeach()
    get_filename_component(shader_output_dir "${SHADER_OUTPUT}" DIRECTORY)
    set(shader_depfile "${SHADER_OUTPUT}.d")
    add_custom_command(OUTPUT "${SHADER_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_output_dir}"
        COMMAND ${SLANGC} "${SHADER_SOURCE}" -target spirv -profile ${SHADER_PROFILE} ${SHADER_EXTRA_ARGS}
                -emit-spirv-directly "$<$<CONFIG:Release,RelWithDebInfo>:-O3;-fp-mode;fast>" ${shader_debug_options}
                -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" ${shader_defines}
                -o "${SHADER_OUTPUT}" -depfile "${shader_depfile}"
        DEPENDS "${SHADER_SOURCE}"
        DEPFILE "${shader_depfile}"
        COMMENT "${SHADER_COMMENT}" COMMAND_EXPAND_LISTS VERBATIM)
    set(shader_outputs ${shader_outputs} "${SHADER_OUTPUT}" PARENT_SCOPE)
endfunction()

function(gameengine_add_engine_shaders)
    set(SLANGC "${GAMEENGINE_SLANGC}")
    # Nsight needs NonSemantic.Shader.DebugInfo in the SPIR-V handed to Vulkan.
    # Keep it in every configuration: multi-config builds share this output
    # directory, so a Release build can otherwise replace Debug's shaders.
    set(shader_debug_options -g2)
    set(ENGINE_SHADER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/Engine/Shaders")
    set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/resources/shaders" CACHE PATH "Compiled engine shader directory")
    # Only entry-point modules produce SPIR-V.  The remaining .slang files are
    # imports, tracked per output through slangc depfiles.
    set(shader_entries
        Culling/gpu_culling.slang Culling/gpu_instance_culling.slang Culling/meshlet_culling.slang Culling/meshlet_build_dispatch.slang Culling/meshlet_build_indirect.slang Culling/meshlet_build_indexed.slang Culling/meshlet_build_mesh_draws.slang Culling/meshlet_build_mesh_commands.slang Culling/shadow_meshlet_culling.slang Culling/shadow_meshlet_build_indirect.slang Culling/shadow_meshlet_refine.slang Culling/shadow_meshlet_build_mesh_commands.slang Culling/hiz_initialize.slang Culling/hiz_reduce.slang Glass/glass_depth_reduce.slang Glass/glass_depth_seed.slang Culling/clustered_light_culling.slang
        Grass/grass_generate.slang Grass/grass_cull.slang Grass/grass_packed_cull.slang Grass/grass_cluster_cull.slang Grass/grass_shadow_page_cull.slang Grass/grass_packed_bin.slang Grass/grass_packed_prefix.slang Grass/grass_packed_scatter.slang Grass/grass_packed_finalize.slang Grass/grass_classify.slang Grass/grass_build_dispatch.slang Grass/grass_forward.slang Grass/grass_shadow.slang Grass/grass_velocity.slang Grass/grass_build_indirect.slang Grass/grass_finalize_indirect.slang Grass/grass_prefix_sum.slang Grass/grass_scatter_instances.slang
        Environment/skybox.slang Forward/forward_pbr.slang Forward/depth_velocity.slang Particles/particle_billboard.slang Particles/particle_simulation.slang Water/water_page_cull.slang Water/water_depth_init.slang Water/water_page_build_indirect.slang Water/water_sssr_depth_init.slang Water/water_sssr_depth_reduce.slang Water/water_state_allocate.slang Water/water_state_update.slang Water/water_far_ocean_prepass.slang Water/water_authored_prepass.slang Water/water_virtual_prepass.slang Water/water_tile_classify.slang Water/water_tile_build_dispatch.slang Water/water_virtual_shade.slang Water/water_virtual_composite.slang PostProcess/aces_tonemap.slang PostProcess/bloom_downsample.slang PostProcess/bloom_upsample.slang PostProcess/gtao_prefilter_depth.slang PostProcess/gtao_denoise.slang PostProcess/gtao_upsample_compute.slang PostProcess/gtao_denoise_upsample.slang PostProcess/ssgi_capture.slang PostProcess/ssgi_prepare.slang PostProcess/ssgi_trace.slang PostProcess/ssgi_temporal.slang PostProcess/ssgi_resolve.slang RayTracing/cluster_prepare.slang RayTracing/cluster_displace.slang RayTracing/ptlas_patch_instances.slang GI/ddgi_probe_select.slang GI/ddgi_probe_blend.slang PostProcess/temporal_aa.slang PostProcess/temporal_velocity.slang PostProcess/auto_exposure.slang Samples/basic_pbr.slang Shadow/shadow_map.slang Shadow/shadow_opaque.slang Shadow/vsm_page_marking.slang Shadow/vsm_page_compact.slang Shadow/rt_contact_shadow.slang Shadow/local_light_shadow.slang Shadow/local_light_shadow_denoise.slang Culling/tile_light_culling.slang Lighting/deferred_local_lights.slang Lighting/megalights_shade.slang Lighting/megalights_temporal.slang UI/canvas.slang)
    set(shader_outputs)
    foreach(shader_entry IN LISTS shader_entries)
        set(shader_source "${ENGINE_SHADER_SOURCE_DIR}/${shader_entry}")
        file(RELATIVE_PATH shader_relative "${ENGINE_SHADER_SOURCE_DIR}" "${shader_source}")
        string(REGEX REPLACE "\\.slang$" ".spv" shader_relative "${shader_relative}")
        # Runtime shader references use a flat `shaders/<name>.spv` layout.
        get_filename_component(shader_filename "${shader_relative}" NAME)
        set(shader_output "${SHADER_OUT_DIR}/${shader_filename}")
        gameengine_compile_shader(SOURCE "${shader_source}" OUTPUT "${shader_output}")
    endforeach()
    # Mesh-shader VSM casters: the refine pass lists meshlets instead of
    # indexed draws, and meshMain needs SPIR-V 1.4.
    set(shadow_refine_mesh_output "${SHADER_OUT_DIR}/shadow_meshlet_refine_mesh.spv")
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Culling/shadow_meshlet_refine.slang" OUTPUT "${shadow_refine_mesh_output}"
        DEFINES SHADOW_MESH_TASKS=1
        COMMENT "Compiling mesh-shader VSM meshlet refine")
    set(shadow_meshlet_mesh_output "${SHADER_OUT_DIR}/shadow_meshlet_mesh.spv")
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Shadow/shadow_meshlet_mesh.slang" OUTPUT "${shadow_meshlet_mesh_output}"
        PROFILE glsl_460+spirv_1_4
        COMMENT "Compiling mesh-shader VSM caster shader")
    set(shadow_meshlet_mesh_alpha_output "${SHADER_OUT_DIR}/shadow_meshlet_mesh_alpha.spv")
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Shadow/shadow_meshlet_mesh.slang" OUTPUT "${shadow_meshlet_mesh_alpha_output}"
        PROFILE glsl_460+spirv_1_4
        DEFINES SHADOW_MESH_ALPHA_TEST=1
        COMMENT "Compiling alpha-tested mesh-shader VSM caster shader")
    set(directional_visibility_output "${SHADER_OUT_DIR}/directional_visibility.spv")
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Shadow/directional_visibility.slang" OUTPUT "${directional_visibility_output}"
        DEFINES SHADOW_COMPUTE=1
        COMMENT "Compiling directional visibility resolve")
    # Ray tracing pipeline with HitObject: shader execution reordering needs
    # SPIR-V 1.4+. The NV reorder capability keeps Slang's hit object type and
    # its GetClusterID (OpHitObjectGetClusterIdNV) consistent; with the EXT
    # capability Slang mixes an EXT hit object with the NV cluster opcode.
    set(ddgi_trace_output "${SHADER_OUT_DIR}/ddgi_trace.spv")
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/GI/ddgi_trace.slang" OUTPUT "${ddgi_trace_output}"
        PROFILE spirv_1_6
        EXTRA_ARGS -capability spvShaderInvocationReorderNV
        COMMENT "Compiling DDGI ray tracing pipeline")
    # Local light shadows traced through the cluster (Mega Geometry) scene.
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Shadow/local_light_shadow.slang"
        OUTPUT "${SHADER_OUT_DIR}/local_light_shadow_rt.spv"
        PROFILE spirv_1_6
        DEFINES LOCAL_SHADOW_RAYGEN=1
        COMMENT "Compiling local light shadow ray tracing pipeline")
    set(gtao_main_source "${ENGINE_SHADER_SOURCE_DIR}/PostProcess/gtao_main.slang")
    foreach(gtao_variant IN ITEMS low medium high ultra performance_half balanced_half)
        if(gtao_variant STREQUAL "low")
            set(gtao_directions 1)
            set(gtao_steps 2)
            set(gtao_half_res 0)
        elseif(gtao_variant STREQUAL "medium")
            set(gtao_directions 2)
            set(gtao_steps 2)
            set(gtao_half_res 0)
        elseif(gtao_variant STREQUAL "high")
            set(gtao_directions 3)
            set(gtao_steps 3)
            set(gtao_half_res 0)
        elseif(gtao_variant STREQUAL "ultra")
            set(gtao_directions 9)
            set(gtao_steps 3)
            set(gtao_half_res 0)
        elseif(gtao_variant STREQUAL "performance_half")
            set(gtao_directions 3)
            set(gtao_steps 2)
            set(gtao_half_res 1)
        else()
            set(gtao_directions 4)
            set(gtao_steps 3)
            set(gtao_half_res 1)
        endif()
        set(gtao_output "${SHADER_OUT_DIR}/gtao_main_${gtao_variant}.spv")
        gameengine_compile_shader(SOURCE "${gtao_main_source}" OUTPUT "${gtao_output}"
            DEFINES GTAO_DIRECTIONS=${gtao_directions} GTAO_STEPS=${gtao_steps} GTAO_HALF_RES=${gtao_half_res}
            COMMENT "Compiling GTAO ${gtao_variant} shader variant")
    endforeach()
    set(depth_velocity_no_velocity_output "${SHADER_OUT_DIR}/depth_velocity_no_velocity.spv")
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/depth_velocity.slang" OUTPUT "${depth_velocity_no_velocity_output}"
        DEFINES DEPTH_OUTPUT_VELOCITY=0
        COMMENT "Compiling depth-only shader without velocity")
    foreach(temporal_variant IN ITEMS skybox particle_billboard)
        set(temporal_variant_output "${SHADER_OUT_DIR}/${temporal_variant}_no_velocity.spv")
        if(temporal_variant STREQUAL "skybox")
            set(temporal_variant_define SKY_OUTPUT_VELOCITY=0)
            set(temporal_variant_source "${ENGINE_SHADER_SOURCE_DIR}/Environment/skybox.slang")
        else()
            set(temporal_variant_define PARTICLE_OUTPUT_VELOCITY=0)
            set(temporal_variant_source "${ENGINE_SHADER_SOURCE_DIR}/Particles/particle_billboard.slang")
        endif()
        gameengine_compile_shader(SOURCE "${temporal_variant_source}" OUTPUT "${temporal_variant_output}"
            DEFINES ${temporal_variant_define}
            COMMENT "Compiling color-only ${temporal_variant} shader")
    endforeach()
    foreach(pbr_class IN ITEMS normal extended foliage terrain)
        if(pbr_class STREQUAL "normal")
            set(pbr_class_define 3)
        elseif(pbr_class STREQUAL "extended")
            set(pbr_class_define 4)
        elseif(pbr_class STREQUAL "foliage")
            set(pbr_class_define 5)
        else()
            set(pbr_class_define 6)
        endif()
        foreach(velocity_variant IN ITEMS full no_velocity)
            if(velocity_variant STREQUAL "full")
                set(pbr_class_output "${SHADER_OUT_DIR}/forward_pbr_${pbr_class}.spv")
                set(pbr_velocity_define 1)
            else()
                set(pbr_class_output "${SHADER_OUT_DIR}/forward_pbr_${pbr_class}_no_velocity.spv")
                set(pbr_velocity_define 0)
            endif()
            gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${pbr_class_output}"
                DEFINES MATERIAL_VARIANT=${pbr_class_define} FORWARD_OUTPUT_VELOCITY=${pbr_velocity_define}
                COMMENT "Compiling forward PBR ${pbr_class} shader")
        endforeach()
    endforeach()
    foreach(resolved_class IN ITEMS basic normal extended foliage terrain)
        if(resolved_class STREQUAL "basic")
            set(resolved_define 0)
            set(resolved_stem forward_pbr)
        elseif(resolved_class STREQUAL "normal")
            set(resolved_define 3)
            set(resolved_stem forward_pbr_normal)
        elseif(resolved_class STREQUAL "extended")
            set(resolved_define 4)
            set(resolved_stem forward_pbr_extended)
        elseif(resolved_class STREQUAL "foliage")
            set(resolved_define 5)
            set(resolved_stem forward_pbr_foliage)
        else()
            set(resolved_define 6)
            set(resolved_stem forward_pbr_terrain)
        endif()
        foreach(resolved_velocity IN ITEMS full no_velocity)
            if(resolved_velocity STREQUAL "full")
                set(resolved_output "${SHADER_OUT_DIR}/${resolved_stem}_resolved.spv")
                set(resolved_velocity_define 1)
            else()
                set(resolved_output "${SHADER_OUT_DIR}/${resolved_stem}_no_velocity_resolved.spv")
                set(resolved_velocity_define 0)
            endif()
            gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${resolved_output}"
                DEFINES MATERIAL_VARIANT=${resolved_define} FORWARD_OUTPUT_VELOCITY=${resolved_velocity_define} FORWARD_RESOLVED_VSM=1
                COMMENT "Compiling resolved forward PBR ${resolved_class} shader")
        endforeach()
    endforeach()
    # Surface families share the forward material descriptor contract.  They
    # are separate SPIR-V modules so ForwardPass can select one pipeline per
    # MaterialShader without any per-entity shader file lookup.
    foreach(variant_name IN ITEMS unlit hologram)
        if(variant_name STREQUAL "unlit")
            set(variant_define 1)
        elseif(variant_name STREQUAL "hologram")
            set(variant_define 2)
        endif()
        set(variant_output "${SHADER_OUT_DIR}/forward_${variant_name}.spv")
        gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${variant_output}"
            DEFINES MATERIAL_VARIANT=${variant_define}
            COMMENT "Compiling forward ${variant_name} material shader")
    endforeach()
    set(water_shader_source "${ENGINE_SHADER_SOURCE_DIR}/Water/water_surface.slang")
    set(water_shader_output "${SHADER_OUT_DIR}/forward_water.spv")
    gameengine_compile_shader(SOURCE "${water_shader_source}" OUTPUT "${water_shader_output}"
        COMMENT "Compiling dedicated water surface shader")
    # The editor Scene View does not allocate a motion-vector target.  Build
    # matching forward modules whose fragment entry point writes color only.
    foreach(variant_name IN ITEMS pbr unlit hologram)
        if(variant_name STREQUAL "pbr")
            set(variant_define 0)
        elseif(variant_name STREQUAL "unlit")
            set(variant_define 1)
        elseif(variant_name STREQUAL "hologram")
            set(variant_define 2)
        endif()
        set(variant_output "${SHADER_OUT_DIR}/forward_${variant_name}_no_velocity.spv")
        gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${variant_output}"
            DEFINES MATERIAL_VARIANT=${variant_define} FORWARD_OUTPUT_VELOCITY=0
            COMMENT "Compiling color-only forward ${variant_name} material shader")
    endforeach()
    set(water_no_velocity_output "${SHADER_OUT_DIR}/forward_water_no_velocity.spv")
    gameengine_compile_shader(SOURCE "${water_shader_source}" OUTPUT "${water_no_velocity_output}"
        DEFINES FORWARD_OUTPUT_VELOCITY=0
        COMMENT "Compiling color-only dedicated water surface shader")
    # GlassPass draws transmissive materials over the lit scene with its own
    # attachments (colour only, private depth), so one module serves both views.
    set(glass_shader_output "${SHADER_OUT_DIR}/forward_glass.spv")
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Glass/glass_surface.slang" OUTPUT "${glass_shader_output}"
        COMMENT "Compiling glass surface shader")
    set(grass_no_velocity_output "${SHADER_OUT_DIR}/grass_forward_no_velocity.spv")
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Grass/grass_forward.slang" OUTPUT "${grass_no_velocity_output}"
        DEFINES FORWARD_OUTPUT_VELOCITY=0
        COMMENT "Compiling color-only grass forward shader")
    foreach(meshlet_depth_velocity IN ITEMS full no_velocity)
        if(meshlet_depth_velocity STREQUAL "full")
            set(meshlet_depth_suffix "")
            set(meshlet_depth_define 1)
        else()
            set(meshlet_depth_suffix "_no_velocity")
            set(meshlet_depth_define 0)
        endif()
        set(meshlet_depth_output "${SHADER_OUT_DIR}/depth_velocity${meshlet_depth_suffix}_meshlet.spv")
        gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/depth_velocity.slang" OUTPUT "${meshlet_depth_output}"
            DEFINES DEPTH_OUTPUT_VELOCITY=${meshlet_depth_define} FORWARD_MESHLET_INDEXED=1
            COMMENT "Compiling meshlet depth prepass shader")
        # VK_EXT_mesh_shader twins of the meshlet programs (`_meshshader`):
        # meshMain replaces the vertex stage and needs SPIR-V 1.4.
        set(meshshader_depth_output "${SHADER_OUT_DIR}/depth_velocity${meshlet_depth_suffix}_meshshader.spv")
        gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/depth_velocity.slang" OUTPUT "${meshshader_depth_output}"
            PROFILE glsl_460+spirv_1_4
            DEFINES DEPTH_OUTPUT_VELOCITY=${meshlet_depth_define} FORWARD_MESH_SHADER=1
            COMMENT "Compiling mesh-shader depth prepass shader")
    endforeach()
    # Prepass variants for scenes without displaced materials (_flat).
    foreach(flat_depth_velocity IN ITEMS full no_velocity)
        if(flat_depth_velocity STREQUAL "full")
            set(flat_depth_suffix "")
            set(flat_depth_define 1)
        else()
            set(flat_depth_suffix "_no_velocity")
            set(flat_depth_define 0)
        endif()
        foreach(flat_depth_meshlet IN ITEMS 0 1)
            if(flat_depth_meshlet)
                set(flat_depth_output "${SHADER_OUT_DIR}/depth_velocity${flat_depth_suffix}_flat_meshlet.spv")
            else()
                set(flat_depth_output "${SHADER_OUT_DIR}/depth_velocity${flat_depth_suffix}_flat.spv")
            endif()
            gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/depth_velocity.slang" OUTPUT "${flat_depth_output}"
                DEFINES DEPTH_OUTPUT_VELOCITY=${flat_depth_define} FORWARD_MESHLET_INDEXED=${flat_depth_meshlet} DEPTH_DISPLACEMENT=0
                COMMENT "Compiling depth prepass shader without displacement")
        endforeach()
        set(flat_meshshader_output "${SHADER_OUT_DIR}/depth_velocity${flat_depth_suffix}_flat_meshshader.spv")
        gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/depth_velocity.slang" OUTPUT "${flat_meshshader_output}"
            PROFILE glsl_460+spirv_1_4
            DEFINES DEPTH_OUTPUT_VELOCITY=${flat_depth_define} FORWARD_MESH_SHADER=1 DEPTH_DISPLACEMENT=0
            COMMENT "Compiling mesh-shader depth prepass shader without displacement")
    endforeach()
    # Visibility-buffer prototype (GAMENGINE_VISBUFFER=1): the mesh-shader
    # prepass also writes triangle ids (_vis), and forward_pbr's extended PBR
    # program resolves them in compute; see VisibilityBufferPass.
    foreach(vis_displacement IN ITEMS 1 0)
        if(vis_displacement)
            set(vis_output "${SHADER_OUT_DIR}/depth_velocity_vis_meshshader.spv")
        else()
            set(vis_output "${SHADER_OUT_DIR}/depth_velocity_vis_flat_meshshader.spv")
        endif()
        gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/depth_velocity.slang" OUTPUT "${vis_output}"
            PROFILE glsl_460+spirv_1_4
            DEFINES DEPTH_OUTPUT_VELOCITY=1 FORWARD_MESH_SHADER=1 DEPTH_DISPLACEMENT=${vis_displacement} DEPTH_VISIBILITY_ID=1
            COMMENT "Compiling mesh-shader depth prepass shader with visibility ids")
    endforeach()
    gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang"
        OUTPUT "${SHADER_OUT_DIR}/visibility_resolve.spv"
        DEFINES MATERIAL_VARIANT=4 FORWARD_OUTPUT_VELOCITY=0 FORWARD_RESOLVED_VSM=1 FORWARD_DDGI=0
                FORWARD_DISPLACEMENT=0 FORWARD_LOCAL_LIGHT_LOOP=0 FORWARD_VISIBILITY_RESOLVE=1
        COMMENT "Compiling visibility-buffer resolve shader")
    # Indexed meshlet draws use the same material fragment programs, but their
    # firstInstance directly addresses RendererInstanceData.
    foreach(meshlet_variant IN ITEMS pbr unlit hologram pbr_normal pbr_extended pbr_foliage pbr_terrain)
        if(meshlet_variant STREQUAL "pbr")
            set(meshlet_define 0)
        elseif(meshlet_variant STREQUAL "unlit")
            set(meshlet_define 1)
        elseif(meshlet_variant STREQUAL "hologram")
            set(meshlet_define 2)
        elseif(meshlet_variant STREQUAL "pbr_normal")
            set(meshlet_define 3)
        elseif(meshlet_variant STREQUAL "pbr_extended")
            set(meshlet_define 4)
        elseif(meshlet_variant STREQUAL "pbr_foliage")
            set(meshlet_define 5)
        else()
            set(meshlet_define 6)
        endif()
        foreach(meshlet_velocity IN ITEMS full no_velocity)
            if(meshlet_velocity STREQUAL "full")
                set(meshlet_velocity_suffix "")
                set(meshlet_velocity_define 1)
            else()
                set(meshlet_velocity_suffix "_no_velocity")
                set(meshlet_velocity_define 0)
            endif()
            foreach(meshlet_resolved IN ITEMS plain resolved)
                if(meshlet_resolved STREQUAL "plain")
                    set(meshlet_resolved_suffix "")
                    set(meshlet_resolved_define 0)
                else()
                    set(meshlet_resolved_suffix "_resolved")
                    set(meshlet_resolved_define 1)
                endif()
                set(meshlet_output "${SHADER_OUT_DIR}/forward_${meshlet_variant}${meshlet_velocity_suffix}${meshlet_resolved_suffix}_meshlet.spv")
                gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${meshlet_output}"
                    DEFINES MATERIAL_VARIANT=${meshlet_define} FORWARD_OUTPUT_VELOCITY=${meshlet_velocity_define} FORWARD_RESOLVED_VSM=${meshlet_resolved_define} FORWARD_MESHLET_INDEXED=1
                    COMMENT "Compiling forward ${meshlet_variant} meshlet shader")
                set(meshshader_output "${SHADER_OUT_DIR}/forward_${meshlet_variant}${meshlet_velocity_suffix}${meshlet_resolved_suffix}_meshshader.spv")
                gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${meshshader_output}"
                    PROFILE glsl_460+spirv_1_4
                    DEFINES MATERIAL_VARIANT=${meshlet_define} FORWARD_OUTPUT_VELOCITY=${meshlet_velocity_define} FORWARD_RESOLVED_VSM=${meshlet_resolved_define} FORWARD_MESH_SHADER=1
                    COMMENT "Compiling forward ${meshlet_variant} mesh shader")
                # Without the displacement interpolants (FORWARD_DISPLACEMENT=0),
                # selected while no material is displaced.
                set(flat_meshshader_output "${SHADER_OUT_DIR}/forward_${meshlet_variant}${meshlet_velocity_suffix}${meshlet_resolved_suffix}_flat_meshshader.spv")
                gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${flat_meshshader_output}"
                    PROFILE glsl_460+spirv_1_4
                    DEFINES MATERIAL_VARIANT=${meshlet_define} FORWARD_OUTPUT_VELOCITY=${meshlet_velocity_define} FORWARD_RESOLVED_VSM=${meshlet_resolved_define} FORWARD_MESH_SHADER=1 FORWARD_DISPLACEMENT=0
                    COMMENT "Compiling forward ${meshlet_variant} mesh shader without displacement")
            endforeach()
        endforeach()
    endforeach()
    # Lighting-pass variants without the DDGI probe lookup (_noddgi). The
    # lighting pass reads the depth prepass (resolved VSM) and writes no
    # velocity, so only those programs need them.
    foreach(noddgi_class IN ITEMS basic normal extended foliage terrain)
        if(noddgi_class STREQUAL "basic")
            set(noddgi_define 0)
            set(noddgi_stem forward_pbr)
        elseif(noddgi_class STREQUAL "normal")
            set(noddgi_define 3)
            set(noddgi_stem forward_pbr_normal)
        elseif(noddgi_class STREQUAL "extended")
            set(noddgi_define 4)
            set(noddgi_stem forward_pbr_extended)
        elseif(noddgi_class STREQUAL "foliage")
            set(noddgi_define 5)
            set(noddgi_stem forward_pbr_foliage)
        else()
            set(noddgi_define 6)
            set(noddgi_stem forward_pbr_terrain)
        endif()
        foreach(noddgi_meshlet IN ITEMS 0 1)
            if(noddgi_meshlet)
                set(noddgi_output "${SHADER_OUT_DIR}/${noddgi_stem}_no_velocity_resolved_noddgi_meshlet.spv")
            else()
                set(noddgi_output "${SHADER_OUT_DIR}/${noddgi_stem}_no_velocity_resolved_noddgi.spv")
            endif()
            gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${noddgi_output}"
                DEFINES MATERIAL_VARIANT=${noddgi_define} FORWARD_OUTPUT_VELOCITY=0 FORWARD_RESOLVED_VSM=1
                        FORWARD_MESHLET_INDEXED=${noddgi_meshlet} FORWARD_DDGI=0
                COMMENT "Compiling forward PBR ${noddgi_class} shader without DDGI")
        endforeach()
        set(noddgi_meshshader_output "${SHADER_OUT_DIR}/${noddgi_stem}_no_velocity_resolved_noddgi_meshshader.spv")
        gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${noddgi_meshshader_output}"
            PROFILE glsl_460+spirv_1_4
            DEFINES MATERIAL_VARIANT=${noddgi_define} FORWARD_OUTPUT_VELOCITY=0 FORWARD_RESOLVED_VSM=1
                    FORWARD_MESH_SHADER=1 FORWARD_DDGI=0
            COMMENT "Compiling forward PBR ${noddgi_class} mesh shader without DDGI")
        set(noddgi_flat_meshshader_output "${SHADER_OUT_DIR}/${noddgi_stem}_no_velocity_resolved_noddgi_flat_meshshader.spv")
        gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" OUTPUT "${noddgi_flat_meshshader_output}"
            PROFILE glsl_460+spirv_1_4
            DEFINES MATERIAL_VARIANT=${noddgi_define} FORWARD_OUTPUT_VELOCITY=0 FORWARD_RESOLVED_VSM=1
                    FORWARD_MESH_SHADER=1 FORWARD_DDGI=0 FORWARD_DISPLACEMENT=0
            COMMENT "Compiling forward PBR ${noddgi_class} mesh shader without DDGI and displacement")
        # `_deferred` twins of the four lighting-pass mesh-shader programs,
        # used while the deferred compute pass adds the local lights: they
        # compile out the local light walk (FORWARD_LOCAL_LIGHT_LOOP=0).
        foreach(deferred_ddgi IN ITEMS 1 0)
            foreach(deferred_displacement IN ITEMS 1 0)
                set(deferred_name "${noddgi_stem}_no_velocity_resolved")
                if(NOT deferred_ddgi)
                    string(APPEND deferred_name "_noddgi")
                endif()
                string(APPEND deferred_name "_deferred")
                if(NOT deferred_displacement)
                    string(APPEND deferred_name "_flat")
                endif()
                gameengine_compile_shader(SOURCE "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang"
                    OUTPUT "${SHADER_OUT_DIR}/${deferred_name}_meshshader.spv"
                    PROFILE glsl_460+spirv_1_4
                    DEFINES MATERIAL_VARIANT=${noddgi_define} FORWARD_OUTPUT_VELOCITY=0 FORWARD_RESOLVED_VSM=1
                            FORWARD_MESH_SHADER=1 FORWARD_DDGI=${deferred_ddgi}
                            FORWARD_DISPLACEMENT=${deferred_displacement} FORWARD_LOCAL_LIGHT_LOOP=0
                    COMMENT "Compiling forward PBR ${noddgi_class} mesh shader without local lights")
            endforeach()
        endforeach()
    endforeach()
    add_custom_target(EngineShaders DEPENDS ${shader_outputs})
    set(GAMEENGINE_SHADER_OUTPUT_DIR "${SHADER_OUT_DIR}" PARENT_SCOPE)
endfunction()
