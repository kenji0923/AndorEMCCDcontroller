# ----- Custom function definition start -----

# Add the standard include directory.
function(AndorEMCCDcontroller_add_include_directory_std target_name scope)
    target_include_directories(${target_name} ${scope}
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/include/${PROJECT_NAME}>
    )
endfunction()


# Add library with a standard filenames
# The third argument can be used for specifying library type (STATIC|SHARED|INTERFACE)
function(AndorEMCCDcontroller_add_library_std target_name library_file_name)
    set(library_type "")

    if(${ARGC} GREATER 2)
	set(library_type ${ARGV2})
    endif()


    set(include_scope "PUBLIC")

    if("${library_type}" STREQUAL "INTERFACE")
	add_library(${target_name} ${library_type} include/${PROJECT_NAME}/${library_file_name}.h)
	set(include_scope "INTERFACE")
    else()
	add_library(${target_name} ${library_type} ${library_file_name}.cc include/${PROJECT_NAME}/${library_file_name}.h)
	target_compile_options(${target_name} PRIVATE ${AEC_WARNING_FLAGS})
    endif()

    AndorEMCCDcontroller_add_include_directory_std(${target_name} ${include_scope})

    add_library(${PROJECT_NAME}::${target_name} ALIAS ${target_name})

    install(TARGETS ${target_name}
	EXPORT ${PROJECT_NAME}Targets
	LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
	INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/${PROJECT_NAME}
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endfunction()


# Install the package configuration files so that other projects can locate this
# project with find_package(${PROJECT_NAME}) and link ${PROJECT_NAME}::<target>.
# Call this once, after all libraries have been declared with the helper above.
function(AndorEMCCDcontroller_install_package)
    include(CMakePackageConfigHelpers)

    set(config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

    install(EXPORT ${PROJECT_NAME}Targets
	NAMESPACE ${PROJECT_NAME}::
	DESTINATION ${config_dir}
    )

    configure_package_config_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
	INSTALL_DESTINATION ${config_dir}
    )

    write_basic_package_version_file(
	${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion
    )

    install(FILES
	${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
	${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
	DESTINATION ${config_dir}
    )
endfunction()

# ----- Function definiion end -----

# Protocol: shared by the backend, the frontend and the tests.
AndorEMCCDcontroller_add_library_std(Protocol Protocol)
target_sources(Protocol PRIVATE
    CameraSettings.cc include/${PROJECT_NAME}/CameraSettings.h)
target_link_libraries(Protocol PUBLIC nlohmann_json::nlohmann_json)

if(AEC_BUILD_BACKEND)
    find_package(Threads REQUIRED)

    AndorEMCCDcontroller_add_library_std(Camera Camera)
    target_sources(Camera PRIVATE
        SimCamera.cc include/${PROJECT_NAME}/SimCamera.h)
    target_link_libraries(Camera PUBLIC Protocol Threads::Threads)
    if(AEC_HAVE_ANDOR_SDK)
        target_sources(Camera PRIVATE
            AndorCamera.cc include/${PROJECT_NAME}/AndorCamera.h)
        target_include_directories(Camera PRIVATE ${ANDOR_INCLUDE_DIR})
        target_link_libraries(Camera PRIVATE ${ANDOR_LIBRARY})
        target_compile_definitions(Camera PUBLIC AEC_HAVE_ANDOR_SDK=1)
    else()
        target_sources(Camera PRIVATE AndorCameraStub.cc)
    endif()

    AndorEMCCDcontroller_add_library_std(Engine AcquisitionEngine)
    target_sources(Engine PRIVATE
        FrameStore.cc include/${PROJECT_NAME}/FrameStore.h
        ControlServer.cc include/${PROJECT_NAME}/ControlServer.h
        ThermalWatch.cc include/${PROJECT_NAME}/ThermalWatch.h
        Telemetry.cc include/${PROJECT_NAME}/Telemetry.h)
    target_link_libraries(Engine PUBLIC Camera cppzmq spdlog::spdlog)
endif()

if(AEC_BUILD_GUI)
    # Frontend libraries (Qt free, so they can be unit tested).
    AndorEMCCDcontroller_add_library_std(Analysis Analysis)
    target_sources(Analysis PRIVATE
        Colormap.cc include/${PROJECT_NAME}/Colormap.h
        Expression.cc include/${PROJECT_NAME}/Expression.h)
    target_link_libraries(Analysis PUBLIC Protocol PRIVATE tinyexpr)

    AndorEMCCDcontroller_add_library_std(ImageIO ImageIO)
    target_sources(ImageIO PRIVATE StreamRecorder.cc include/${PROJECT_NAME}/StreamRecorder.h)
    target_link_libraries(ImageIO PUBLIC Protocol
        PRIVATE TIFF::TIFF PNG::PNG CFITSIO::CFITSIO)

    AndorEMCCDcontroller_add_library_std(Client CameraClient)
    target_sources(Client PRIVATE FrameRing.cc include/${PROJECT_NAME}/FrameRing.h)
    target_link_libraries(Client PUBLIC Protocol cppzmq)
endif()
