find_package(Doxygen)

if (DOXYGEN_FOUND)
    set (DOXYGEN_SOURCE_BROWSER YES)
    set (DOXYGEN_INLINE_SOURCES YES)
    set (EXTRACT_ALL YES)
    doxygen_add_docs(doc
	${PROJECT_SOURCE_DIR}/core
	COMMENT "Generate manual pages"
    )
endif()
