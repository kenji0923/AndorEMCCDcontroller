# ----- Custom function definition start -----

# Add an executable with standard install (goes to the bin directory).
# Pass the target name followed by its source files.
function(AndorEMCCDcontroller_add_executable_std target_name)
    add_executable(${target_name} ${ARGN})
    target_compile_options(${target_name} PRIVATE ${AEC_WARNING_FLAGS})

    install(TARGETS ${target_name}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endfunction()

# ----- Function definiion end -----

if(AEC_BUILD_BACKEND)
    AndorEMCCDcontroller_add_executable_std(andor_emccd_backend
        andor_emccd_backend.cc)
    target_link_libraries(andor_emccd_backend PRIVATE
        AndorEMCCDcontroller::Engine argparse::argparse)
    if(AEC_HAVE_ANDOR_SDK)
        # libandor.so lives in /usr/local/lib after install_andor.
        set_target_properties(andor_emccd_backend PROPERTIES
            INSTALL_RPATH "${ANDOR_SDK_ROOT}/lib")
    endif()
endif()

if(AEC_BUILD_GUI)
    add_subdirectory(gui)
endif()
