# gcc (and other Unix-like compilers, e.g. MinGW)

# Compiler flag modules.
INCLUDE(CheckCCompilerFlag)
INCLUDE(CheckCXXCompilerFlag)

# Check what flag is needed for C99 support.
INCLUDE(CheckC99CompilerFlag)
CHECK_C99_COMPILER_FLAG(RP_C99_CFLAG)

# Check what flag is needed for C++ 2011 support.
INCLUDE(CheckCXX11CompilerFlag)
CHECK_CXX11_COMPILER_FLAG(RP_CXX11_CXXFLAG)

# Disable C++ exceptions.
# NOTE: Disabled due to dynamic_cast<> usage in KDE's kio_thumbnail.so.
#INCLUDE(CheckCXXNoExceptionsCompilerFlag)
#CHECK_CXX_NO_EXCEPTIONS_COMPILER_FLAG(RP_CXX_NO_EXCEPTIONS_CXXFLAG)

SET(RP_C_FLAGS_COMMON "${RP_C99_CFLAG}")
SET(RP_CXX_FLAGS_COMMON "${RP_CXX11_CXXFLAG} ${RP_CXX_NO_RTTI_CXXFLAG} ${RP_CXX_NO_EXCEPTIONS_CXXFLAG}")
SET(RP_EXE_LINKER_FLAGS_COMMON "")
UNSET(RP_C99_CFLAG)
UNSET(RP_CXX11_CXXFLAG)
UNSET(RP_CXX_NO_RTTI_CXXFLAG)
UNSET(RP_CXX_NO_EXCEPTIONS_CXXFLAG)

# Test for common CFLAGS and CXXFLAGS.
FOREACH(FLAG_TEST "-Wall" "-Wextra" "-fstrict-aliasing" "-fvisibility=hidden" "-Wno-multichar")
	CHECK_C_COMPILER_FLAG("${FLAG_TEST}" CFLAG_${FLAG_TEST})
	IF(CFLAG_${FLAG_TEST})
		SET(RP_C_FLAGS_COMMON "${RP_C_FLAGS_COMMON} ${FLAG_TEST}")
	ENDIF(CFLAG_${FLAG_TEST})
	UNSET(CFLAG_${FLAG_TEST})
	
	CHECK_CXX_COMPILER_FLAG("${FLAG_TEST}" CXXFLAG_${FLAG_TEST})
	IF(CXXFLAG_${FLAG_TEST})
		SET(RP_CXX_FLAGS_COMMON "${RP_CXX_FLAGS_COMMON} ${FLAG_TEST}")
	ENDIF(CXXFLAG_${FLAG_TEST})
	UNSET(CXXFLAG_${FLAG_TEST})
ENDFOREACH()

# Test for common CXXFLAGS.
FOREACH(FLAG_TEST "-fvisibility-inlines-hidden")
	CHECK_CXX_COMPILER_FLAG("${FLAG_TEST}" CXXFLAG_${FLAG_TEST})
	IF(CXXFLAG_${FLAG_TEST})
		SET(RP_CXX_FLAGS_COMMON "${RP_CXX_FLAGS_COMMON} ${FLAG_TEST}")
	ENDIF(CXXFLAG_${FLAG_TEST})
	UNSET(CXXFLAG_${FLAG_TEST})
ENDFOREACH()

# Test for common LDFLAGS.
# TODO: Doesn't work on OS X. (which means it's not really testing it!)
IF(NOT APPLE)
	FOREACH(FLAG_TEST "-Wl,-O1" "-Wl,--sort-common" "-Wl,--as-needed")
		CHECK_C_COMPILER_FLAG("${FLAG_TEST}" LDFLAG_${FLAG_TEST})
		IF(LDFLAG_${FLAG_TEST})
			SET(RP_EXE_LINKER_FLAGS_COMMON "${RP_EXE_LINKER_FLAGS_COMMON} ${FLAG_TEST}")
		ENDIF(LDFLAG_${FLAG_TEST})
		UNSET(LDFLAG_${FLAG_TEST})
	ENDFOREACH()
ENDIF(NOT APPLE)
SET(RP_SHARED_LINKER_FLAGS_COMMON "${RP_EXE_LINKER_FLAGS_COMMON}")

# Check for -Og.
# This flag was added in gcc-4.8, and enables optimizations that
# don't interfere with debugging.
CHECK_C_COMPILER_FLAG("-Og" CFLAG_OPTIMIZE_DEBUG)
IF (CFLAG_OPTIMIZE_DEBUG)
	SET(CFLAG_OPTIMIZE_DEBUG "-Og")
ELSE(CFLAG_OPTIMIZE_DEBUG)
	SET(CFLAG_OPTIMIZE_DEBUG "-O0")
ENDIF(CFLAG_OPTIMIZE_DEBUG)

# Debug/release flags.
SET(RP_C_FLAGS_DEBUG		"${CFLAG_OPTIMIZE_DEBUG} -ggdb -DDEBUG -D_DEBUG")
SET(RP_CXX_FLAGS_DEBUG		"${CFLAG_OPTIMIZE_DEBUG} -ggdb -DDEBUG -D_DEBUG")
SET(RP_C_FLAGS_RELEASE		"-O2 -ggdb -DNDEBUG")
SET(RP_CXX_FLAGS_RELEASE	"-O2 -ggdb -DNDEBUG")

# Unset temporary variables.
UNSET(CFLAG_OPTIMIZE_DEBUG)
