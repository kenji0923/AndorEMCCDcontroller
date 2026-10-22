set(CMAKE_AUTOMOC ON)

# Windows embeds the .ico into the .exe (Explorer and taskbar icon).
set(AEC_GUI_WIN_RESOURCE)
if(WIN32)
    set(AEC_GUI_WIN_RESOURCE resources/andor_emccd_gui.rc)
endif()

AndorEMCCDcontroller_add_executable_std(andor_emccd_gui
    ${AEC_GUI_WIN_RESOURCE}
    main.cc
    AppIcon.cc AppIcon.h
    MainWindow.cc MainWindow.h
    ImageView.cc ImageView.h
    SettingsPanel.cc SettingsPanel.h
    AnalysisPanel.cc AnalysisPanel.h
    TrendPlot.cc TrendPlot.h
    LiveProcessor.cc LiveProcessor.h
    UdpSender.cc UdpSender.h)
target_link_libraries(andor_emccd_gui PRIVATE
    AndorEMCCDcontroller::Client
    AndorEMCCDcontroller::Analysis
    AndorEMCCDcontroller::ImageIO
    Qt6::Widgets Qt6::Network)
if(WIN32)
    set_target_properties(andor_emccd_gui PROPERTIES WIN32_EXECUTABLE ON)
    target_link_libraries(andor_emccd_gui PRIVATE ws2_32)
endif()
if(UNIX AND NOT APPLE)
    # Desktop menu entry and icon (the window icon itself is drawn in code).
    install(FILES resources/andor_emccd_gui.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES resources/andor_emccd_gui.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()
