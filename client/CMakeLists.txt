include(findcoredeps)
#include(findswigdeps) #remove pyhton deps, build for golang

FIND_PACKAGE(SWIG 3.0)

EXECUTE_PROCESS( COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE ARCHITECTURE )
add_custom_command(
		OUTPUT ovpncli_wrap.cxx ovpncli_wrap.h
		COMMENT "Generating ovpncli GO swig files"
		COMMAND ${SWIG_EXECUTABLE} -go -gccgo -c++ -intgosize 64 -outcurrentdir -I${CORE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ovpncli.i
		DEPENDS ovpncli.i
) 

add_library(ovpnclilib STATIC
		ovpncli.cpp
		ovpncli_wrap.cxx
		ovpncli_wrap.h
		)

add_core_dependencies(ovpnclilib)
target_include_directories(ovpnclilib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Use proper python library name to generate libopenvpn3_linux_x86_64.so/a/dylib/dll/
set_target_properties(ovpnclilib PROPERTIES OUTPUT_NAME "libopenvpn3_${ARCHITECTURE}") 
set_target_properties(ovpnclilib PROPERTIES PREFIX "")

if (NOT WIN32)
	# Swig generates code with deprecated python declarations
	set_source_files_properties(ovpncli_wrap.cxx PROPERTIES) 
endif()
