function(gameengine_add_engine_shaders)
    set(SLANGC "${GAMEENGINE_SLANGC}")
    set(ENGINE_SHADER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/Engine/Shaders")
    set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/resources/shaders" CACHE PATH "Compiled engine shader directory")
    file(GLOB_RECURSE shader_modules CONFIGURE_DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/*.slang")
    # Only entry-point modules produce SPIR-V.  The remaining .slang files are
    # imports and are still explicit dependencies of every compilation.
    set(shader_entries
        Culling/gpu_culling.slang Culling/gpu_instance_culling.slang Culling/meshlet_culling.slang Culling/hiz_initialize.slang Culling/hiz_reduce.slang Culling/clustered_light_culling.slang
        Grass/grass_generate.slang Grass/grass_cull.slang Grass/grass_packed_cull.slang Grass/grass_cluster_cull.slang Grass/grass_packed_bin.slang Grass/grass_packed_prefix.slang Grass/grass_packed_scatter.slang Grass/grass_packed_finalize.slang Grass/grass_classify.slang Grass/grass_build_dispatch.slang Grass/grass_forward.slang Grass/grass_shadow.slang Grass/grass_velocity.slang Grass/grass_build_indirect.slang Grass/grass_finalize_indirect.slang Grass/grass_prefix_sum.slang Grass/grass_scatter_instances.slang
        Environment/skybox.slang Forward/forward_pbr.slang Forward/selection_outline.slang Particles/particle_billboard.slang Particles/particle_simulation.slang PostProcess/aces_tonemap.slang PostProcess/bloom_downsample.slang PostProcess/temporal_aa.slang PostProcess/temporal_velocity.slang Samples/basic_pbr.slang Shadow/shadow_map.slang UI/canvas.slang)
    set(shader_outputs)
    foreach(shader_entry IN LISTS shader_entries)
        set(shader_source "${ENGINE_SHADER_SOURCE_DIR}/${shader_entry}")
        file(RELATIVE_PATH shader_relative "${ENGINE_SHADER_SOURCE_DIR}" "${shader_source}")
        string(REGEX REPLACE "\\.slang$" ".spv" shader_relative "${shader_relative}")
        # Runtime shader references use a flat `shaders/<name>.spv` layout.
        get_filename_component(shader_filename "${shader_relative}" NAME)
        set(shader_output "${SHADER_OUT_DIR}/${shader_filename}")
        get_filename_component(shader_output_dir "${shader_output}" DIRECTORY)
        list(APPEND shader_outputs "${shader_output}")
        add_custom_command(OUTPUT "${shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_output_dir}"
            COMMAND ${SLANGC} "${shader_source}" -target spirv -profile glsl_460 -emit-spirv-directly -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -o "${shader_output}"
            DEPENDS "${shader_source}" ${shader_modules} COMMENT "Compiling Slang shader ${shader_relative}" VERBATIM)
    endforeach()
    # Surface families share the forward material descriptor contract.  They
    # are separate SPIR-V modules so ForwardPass can select one pipeline per
    # MaterialShader without any per-entity shader file lookup.
    foreach(variant_name IN ITEMS unlit hologram water)
        if(variant_name STREQUAL "unlit")
            set(variant_define 1)
        elseif(variant_name STREQUAL "hologram")
            set(variant_define 2)
        else()
            set(variant_define 3)
        endif()
        set(variant_output "${SHADER_OUT_DIR}/forward_${variant_name}.spv")
        list(APPEND shader_outputs "${variant_output}")
        add_custom_command(OUTPUT "${variant_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
            COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" -target spirv -profile glsl_460 -emit-spirv-directly -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D MATERIAL_VARIANT=${variant_define} -o "${variant_output}"
            DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" ${shader_modules}
            COMMENT "Compiling forward ${variant_name} material shader" VERBATIM)
    endforeach()
    # The editor Scene View does not allocate a motion-vector target.  Build
    # matching forward modules whose fragment entry point writes color only.
    foreach(variant_name IN ITEMS pbr unlit hologram water)
        if(variant_name STREQUAL "pbr")
            set(variant_define 0)
        elseif(variant_name STREQUAL "unlit")
            set(variant_define 1)
        elseif(variant_name STREQUAL "hologram")
            set(variant_define 2)
        else()
            set(variant_define 3)
        endif()
        set(variant_output "${SHADER_OUT_DIR}/forward_${variant_name}_no_velocity.spv")
        list(APPEND shader_outputs "${variant_output}")
        add_custom_command(OUTPUT "${variant_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
            COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" -target spirv -profile glsl_460 -emit-spirv-directly -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D MATERIAL_VARIANT=${variant_define} -D FORWARD_OUTPUT_VELOCITY=0 -o "${variant_output}"
            DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" ${shader_modules}
            COMMENT "Compiling color-only forward ${variant_name} material shader" VERBATIM)
    endforeach()
    set(grass_no_velocity_output "${SHADER_OUT_DIR}/grass_forward_no_velocity.spv")
    list(APPEND shader_outputs "${grass_no_velocity_output}")
    add_custom_command(OUTPUT "${grass_no_velocity_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
        COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Grass/grass_forward.slang" -target spirv -profile glsl_460 -emit-spirv-directly -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D FORWARD_OUTPUT_VELOCITY=0 -o "${grass_no_velocity_output}"
        DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Grass/grass_forward.slang" ${shader_modules}
        COMMENT "Compiling color-only grass forward shader" VERBATIM)
    add_custom_target(EngineShaders DEPENDS ${shader_outputs})
    set(GAMEENGINE_SHADER_OUTPUT_DIR "${SHADER_OUT_DIR}" PARENT_SCOPE)
endfunction()
