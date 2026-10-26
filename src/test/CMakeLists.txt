include(GoogleTest)

function(aec_add_test name)
    add_executable(${name} ${ARGN})
    target_compile_options(${name} PRIVATE ${AEC_WARNING_FLAGS})
    target_link_libraries(${name} PRIVATE GTest::gtest_main)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

aec_add_test(test_settings test_settings.cc)
target_link_libraries(test_settings PRIVATE AndorEMCCDcontroller::Protocol)

if(AEC_BUILD_GUI)
    aec_add_test(test_analysis test_analysis.cc)
    target_link_libraries(test_analysis PRIVATE AndorEMCCDcontroller::Analysis)

    aec_add_test(test_imageio test_imageio.cc)
    # libtiff and CFITSIO read back the orientation tag and keywords.
    target_link_libraries(test_imageio PRIVATE AndorEMCCDcontroller::ImageIO
                          TIFF::TIFF CFITSIO::CFITSIO)
endif()

if(AEC_BUILD_BACKEND)
    aec_add_test(test_qe_table test_qe_table.cc)
    target_link_libraries(test_qe_table PRIVATE AndorEMCCDcontroller::Camera)

    aec_add_test(test_telemetry test_telemetry.cc)
    target_link_libraries(test_telemetry PRIVATE AndorEMCCDcontroller::Engine)

    aec_add_test(test_backend test_backend.cc)
    target_link_libraries(test_backend PRIVATE AndorEMCCDcontroller::Engine)
    if(AEC_BUILD_GUI)
        target_link_libraries(test_backend PRIVATE AndorEMCCDcontroller::Client)
        target_compile_definitions(test_backend PRIVATE AEC_TEST_CLIENT=1)
    endif()
endif()

if(AEC_BUILD_GUI)
    aec_add_test(test_conversion_panel test_conversion_panel.cc
        ../exec/gui/ConversionPanel.cc ../exec/gui/ConversionPanel.h)
    set_target_properties(test_conversion_panel PROPERTIES AUTOMOC ON)
    target_link_libraries(test_conversion_panel PRIVATE
        AndorEMCCDcontroller::Protocol Qt6::Widgets)

    aec_add_test(test_stream_client test_stream_client.cc)
    target_link_libraries(test_stream_client PRIVATE AndorEMCCDcontroller::Client)
endif()

if(AEC_BUILD_GUI)
    aec_add_test(test_frame_ring test_frame_ring.cc)
    target_link_libraries(test_frame_ring PRIVATE AndorEMCCDcontroller::Client)
endif()

if(AEC_BUILD_GUI)
    aec_add_test(test_stream_recorder test_stream_recorder.cc)
    target_link_libraries(test_stream_recorder PRIVATE AndorEMCCDcontroller::ImageIO)
endif()
