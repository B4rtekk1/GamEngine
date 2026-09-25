function(gameengine_add_engine_shaders)
    set(SLANGC "${GAMEENGINE_SLANGC}")
    # Nsight needs NonSemantic.Shader.DebugInfo in the SPIR-V handed to Vulkan.
    # Keep it in every configuration: multi-config builds share this output
    # directory, so a Release build can otherwise replace Debug's shaders.
    set(shader_debug_options -g2)
    set(ENGINE_SHADER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/Engine/Shaders")
    set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/resources/shaders" CACHE PATH "Compiled engine shader directory")
    file(GLOB_RECURSE shader_modules CONFIGURE_DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/*.slang")
    # Only entry-point modules produce SPIR-V.  The remaining .slang files are
    # imports and are still explicit dependencies of every compilation.
    set(shader_entries
        GI/ddgi_trace.slang GI/ddgi_validate.slang GI/ddgi_relocate.slang GI/ddgi_classify.slang
        GI/ddgi_scroll_reset.slang GI/ddgi_schedule.slang GI/ddgi_finish.slang
        GI/ddgi_blend_irradiance.slang GI/ddgi_blend_distance.slang
        Culling/gpu_culling.slang Culling/gpu_instance_culling.slang Culling/meshlet_culling.slang Culling/meshlet_build_dispatch.slang Culling/meshlet_build_indirect.slang Culling/shadow_meshlet_culling.slang Culling/shadow_meshlet_build_indirect.slang Culling/hiz_initialize.slang Culling/hiz_reduce.slang Culling/clustered_light_culling.slang
        Grass/grass_generate.slang Grass/grass_cull.slang Grass/grass_packed_cull.slang Grass/grass_cluster_cull.slang Grass/grass_shadow_page_cull.slang Grass/grass_packed_bin.slang Grass/grass_packed_prefix.slang Grass/grass_packed_scatter.slang Grass/grass_packed_finalize.slang Grass/grass_classify.slang Grass/grass_build_dispatch.slang Grass/grass_forward.slang Grass/grass_shadow.slang Grass/grass_velocity.slang Grass/grass_build_indirect.slang Grass/grass_finalize_indirect.slang Grass/grass_prefix_sum.slang Grass/grass_scatter_instances.slang
        Environment/skybox.slang Forward/forward_pbr.slang Forward/depth_velocity.slang Forward/selection_outline.slang Particles/particle_billboard.slang Particles/particle_simulation.slang Water/water_page_cull.slang Water/water_page_build_indirect.slang Water/water_sssr_depth_init.slang Water/water_sssr_depth_reduce.slang Water/water_state_allocate.slang Water/water_state_update.slang Water/water_far_ocean_prepass.slang Water/water_authored_prepass.slang Water/water_virtual_prepass.slang Water/water_tile_classify.slang Water/water_tile_build_dispatch.slang Water/water_virtual_shade.slang Water/water_virtual_composite.slang PostProcess/aces_tonemap.slang PostProcess/bloom_downsample.slang PostProcess/gtao.slang PostProcess/gtao_prefilter_depth.slang PostProcess/gtao_denoise.slang PostProcess/gtao_upsample_compute.slang PostProcess/temporal_aa.slang PostProcess/temporal_velocity.slang Samples/basic_pbr.slang Shadow/shadow_map.slang Shadow/shadow_opaque.slang Shadow/vsm_page_marking.slang Shadow/vsm_page_compact.slang Shadow/rt_contact_shadow.slang UI/canvas.slang)
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
            COMMAND ${SLANGC} "${shader_source}" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -o "${shader_output}"
            DEPENDS "${shader_source}" ${shader_modules} COMMENT "Compiling Slang shader ${shader_relative}" COMMAND_EXPAND_LISTS VERBATIM)
    endforeach()
    set(directional_visibility_output "${SHADER_OUT_DIR}/directional_visibility.spv")
    list(APPEND shader_outputs "${directional_visibility_output}")
    add_custom_command(OUTPUT "${directional_visibility_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
        COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Shadow/directional_visibility.slang" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D SHADOW_COMPUTE=1 -o "${directional_visibility_output}"
        DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Shadow/directional_visibility.slang" ${shader_modules}
        COMMENT "Compiling directional visibility resolve" COMMAND_EXPAND_LISTS VERBATIM)
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
        list(APPEND shader_outputs "${gtao_output}")
        add_custom_command(OUTPUT "${gtao_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
            COMMAND ${SLANGC} "${gtao_main_source}" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D GTAO_DIRECTIONS=${gtao_directions} -D GTAO_STEPS=${gtao_steps} -D GTAO_HALF_RES=${gtao_half_res} -o "${gtao_output}"
            DEPENDS "${gtao_main_source}" ${shader_modules}
            COMMENT "Compiling GTAO ${gtao_variant} shader variant" COMMAND_EXPAND_LISTS VERBATIM)
    endforeach()
    set(depth_velocity_no_velocity_output "${SHADER_OUT_DIR}/depth_velocity_no_velocity.spv")
    list(APPEND shader_outputs "${depth_velocity_no_velocity_output}")
    add_custom_command(OUTPUT "${depth_velocity_no_velocity_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
        COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Forward/depth_velocity.slang" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D DEPTH_OUTPUT_VELOCITY=0 -o "${depth_velocity_no_velocity_output}"
        DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Forward/depth_velocity.slang" ${shader_modules}
        COMMENT "Compiling depth-only shader without velocity" COMMAND_EXPAND_LISTS VERBATIM)
    foreach(temporal_variant IN ITEMS skybox selection_outline particle_billboard)
        set(temporal_variant_output "${SHADER_OUT_DIR}/${temporal_variant}_no_velocity.spv")
        list(APPEND shader_outputs "${temporal_variant_output}")
        if(temporal_variant STREQUAL "skybox")
            set(temporal_variant_define SKY_OUTPUT_VELOCITY=0)
            set(temporal_variant_source "${ENGINE_SHADER_SOURCE_DIR}/Environment/skybox.slang")
        elseif(temporal_variant STREQUAL "selection_outline")
            set(temporal_variant_define OUTLINE_OUTPUT_VELOCITY=0)
            set(temporal_variant_source "${ENGINE_SHADER_SOURCE_DIR}/Forward/selection_outline.slang")
        else()
            set(temporal_variant_define PARTICLE_OUTPUT_VELOCITY=0)
            set(temporal_variant_source "${ENGINE_SHADER_SOURCE_DIR}/Particles/particle_billboard.slang")
        endif()
        add_custom_command(OUTPUT "${temporal_variant_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
            COMMAND ${SLANGC} "${temporal_variant_source}" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D ${temporal_variant_define} -o "${temporal_variant_output}"
            DEPENDS "${temporal_variant_source}" ${shader_modules}
            COMMENT "Compiling color-only ${temporal_variant} shader" COMMAND_EXPAND_LISTS VERBATIM)
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
            list(APPEND shader_outputs "${pbr_class_output}")
            add_custom_command(OUTPUT "${pbr_class_output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
                COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D MATERIAL_VARIANT=${pbr_class_define} -D FORWARD_OUTPUT_VELOCITY=${pbr_velocity_define} -o "${pbr_class_output}"
                DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" ${shader_modules}
                COMMENT "Compiling forward PBR ${pbr_class} shader" COMMAND_EXPAND_LISTS VERBATIM)
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
            list(APPEND shader_outputs "${resolved_output}")
            add_custom_command(OUTPUT "${resolved_output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
                COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D MATERIAL_VARIANT=${resolved_define} -D FORWARD_OUTPUT_VELOCITY=${resolved_velocity_define} -D FORWARD_RESOLVED_VSM=1 -o "${resolved_output}"
                DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" ${shader_modules}
                COMMENT "Compiling resolved forward PBR ${resolved_class} shader" COMMAND_EXPAND_LISTS VERBATIM)
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
        list(APPEND shader_outputs "${variant_output}")
        add_custom_command(OUTPUT "${variant_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
            COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D MATERIAL_VARIANT=${variant_define} -o "${variant_output}"
            DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" ${shader_modules}
            COMMENT "Compiling forward ${variant_name} material shader" COMMAND_EXPAND_LISTS VERBATIM)
    endforeach()
    set(water_shader_source "${ENGINE_SHADER_SOURCE_DIR}/Water/water_surface.slang")
    set(water_shader_output "${SHADER_OUT_DIR}/forward_water.spv")
    list(APPEND shader_outputs "${water_shader_output}")
    add_custom_command(OUTPUT "${water_shader_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
        COMMAND ${SLANGC} "${water_shader_source}" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -o "${water_shader_output}"
        DEPENDS "${water_shader_source}" ${shader_modules}
        COMMENT "Compiling dedicated water surface shader" COMMAND_EXPAND_LISTS VERBATIM)
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
        list(APPEND shader_outputs "${variant_output}")
        add_custom_command(OUTPUT "${variant_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
            COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D MATERIAL_VARIANT=${variant_define} -D FORWARD_OUTPUT_VELOCITY=0 -o "${variant_output}"
            DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Forward/forward_pbr.slang" ${shader_modules}
            COMMENT "Compiling color-only forward ${variant_name} material shader" COMMAND_EXPAND_LISTS VERBATIM)
    endforeach()
    set(water_no_velocity_output "${SHADER_OUT_DIR}/forward_water_no_velocity.spv")
    list(APPEND shader_outputs "${water_no_velocity_output}")
    add_custom_command(OUTPUT "${water_no_velocity_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
        COMMAND ${SLANGC} "${water_shader_source}" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D FORWARD_OUTPUT_VELOCITY=0 -o "${water_no_velocity_output}"
        DEPENDS "${water_shader_source}" ${shader_modules}
        COMMENT "Compiling color-only dedicated water surface shader" COMMAND_EXPAND_LISTS VERBATIM)
    set(grass_no_velocity_output "${SHADER_OUT_DIR}/grass_forward_no_velocity.spv")
    list(APPEND shader_outputs "${grass_no_velocity_output}")
    add_custom_command(OUTPUT "${grass_no_velocity_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}"
        COMMAND ${SLANGC} "${ENGINE_SHADER_SOURCE_DIR}/Grass/grass_forward.slang" -target spirv -profile glsl_460 -emit-spirv-directly "$<$<CONFIG:Release>:-O3;-fp-mode;fast>" ${shader_debug_options} -matrix-layout-row-major -I "${ENGINE_SHADER_SOURCE_DIR}" -D FORWARD_OUTPUT_VELOCITY=0 -o "${grass_no_velocity_output}"
        DEPENDS "${ENGINE_SHADER_SOURCE_DIR}/Grass/grass_forward.slang" ${shader_modules}
        COMMENT "Compiling color-only grass forward shader" COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_target(EngineShaders DEPENDS ${shader_outputs})
    set(GAMEENGINE_SHADER_OUTPUT_DIR "${SHADER_OUT_DIR}" PARENT_SCOPE)
endfunction()
