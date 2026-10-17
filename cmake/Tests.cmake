option(REFD2D_BUILD_TESTS "Build Windows WARP rendering regression tests" OFF)
if(REFD2D_BUILD_TESTS)
    enable_testing()
    set(_refd2d_test_source_dir "${CMAKE_CURRENT_LIST_DIR}/..")
    add_executable(hdr-render-tests
        "${_refd2d_test_source_dir}/tests/HdrRenderTests.cpp"
        "${_refd2d_test_source_dir}/tests/HdrOn12Tests.cpp"
        "${_refd2d_test_source_dir}/tests/PluginLifecycleTests.cpp"
        "${_refd2d_test_source_dir}/src/Plugin.cpp"
        "${_refd2d_test_source_dir}/src/DrawList.cpp"
        "${_refd2d_test_source_dir}/src/D2DFont.cpp"
        "${_refd2d_test_source_dir}/src/D2DImage.cpp"
        "${_refd2d_test_source_dir}/src/D2DPainter.cpp"
        "${_refd2d_test_source_dir}/src/D3D12Renderer.cpp"
        "${_refd2d_test_source_dir}/src/D3D12CommandContext.cpp"
        "${_refd2d_test_source_dir}/src/HDRDisplayInfo.cpp"
    )
    target_include_directories(hdr-render-tests PRIVATE
        "${_refd2d_test_source_dir}/src"
        "${_refd2d_test_source_dir}/deps/reframework/include"
    )
    target_compile_features(hdr-render-tests PRIVATE cxx_std_20)
    target_link_libraries(hdr-render-tests PRIVATE utf8cpp sol2::sol2 lua d2d1 dwrite d3d11 d3d12 dxgi d3dcompiler dxguid windowscodecs ole32)
    add_test(NAME hdr-render-tests COMMAND hdr-render-tests)
    set_tests_properties(hdr-render-tests PROPERTIES TIMEOUT 60)
    unset(_refd2d_test_source_dir)
endif()
