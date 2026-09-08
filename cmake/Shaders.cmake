function(gameengine_add_engine_shaders)
    find_program(SLANGC NAMES slangc REQUIRED)
    set(ENGINE_SHADER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/Engine/Shaders")
    set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/resources/shaders" CACHE PATH "Compiled engine shader directory")
    file(GLOB_RECURSE shader_modules CONFIGURE_DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/*.slang")
    # Only entry-point modules produce SPIR-V.  The remaining .slang files are
    # imports and are still explicit dependencies of every compilation.
    set(shader_entries
        Culling/gpu_culling.slang Culling/gpu_instance_culling.slang Culling/hiz_initialize.slang Culling/hiz_reduce.slang
        Grass/grass_generate.slang Grass/grass_cull.slang Grass/grass_packed_cull.slang Grass/grass_cluster_cull.slang Grass/grass_packed_bin.slang Grass/grass_packed_prefix.slang Grass/grass_packed_scatter.slang Grass/grass_packed_finalize.slang Grass/grass_classify.slang Grass/grass_build_dispatch.slang Grass/grass_forward.slang Grass/grass_shadow.slang Grass/grass_velocity.slang Grass/grass_build_indirect.slang Grass/grass_finalize_indirect.slang Grass/grass_prefix_sum.slang Grass/grass_scatter_instances.slang
        Environment/skybox.slang Forward/forward_pbr.slang Forward/selection_outline.slang Particles/particle_billboard.slang Particles/particle_simulation.slang PostProcess/aces_tonemap.slang PostProcess/bloom_downsample.slang PostProcess/temporal_aa.slang PostProcess/temporal_velocity.slang Samples/basic_pbr.slang Shadow/shadow_map.slang UI/canvas.slang)
    set(shader_outputs)
    foreach(shader_entry IN LISTS shader_entries)
        set(shader_source "${ENGINE_SHADER_SOURCE_DIR}/${shader_entry}")
        file(RELATIVE_PATH shader_relative "${ENGINE_SHADER_SOURCE_DIR}" "${shader_source}")
        string(REGEX REPLACE "\\.slang$" ".spv" shader_relative "${shader_relative}")
        set(shader_output "${SHADER_OUT_DIR}/${shader_relative}")
        get_filename_component(shader_output_dir "${shader_output}" DIRECTORY)
        list(APPEND shader_outputs "${shader_output}")
        add_custom_command(OUTPUT "${shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_output_dir}"
            COMMAND ${SLANGC} "${shader_source}" -target spirv -profile glsl_460 -emit-spirv-directly -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -o "${shader_output}"
            DEPENDS "${shader_source}" ${shader_modules} COMMENT "Compiling Slang shader ${shader_relative}" VERBATIM)
    endforeach()
    add_custom_target(EngineShaders DEPENDS ${shader_outputs})
    set(GAMEENGINE_SHADER_OUTPUT_DIR "${SHADER_OUT_DIR}" PARENT_SCOPE)
endfunction()
