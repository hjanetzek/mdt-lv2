# the plugin's imported symbols: only libc / libm functions from the allow
# list (no libstdc++, no allocation besides instantiate's calloc, no locks;
# strcmp: the feature URIs;
# pow, sin, tan, tanh build the tables at instantiate; sqrtf, if not
# inlined, runs when the level changes) (-DNM -DPLUGIN)
set (allowed calloc free memcpy memset memmove strcmp pow sin tan tanh exp2 sqrtf sqrt vfprintf fprintf stderr
	__stack_chk_fail __stack_chk_guard __cxa_finalize __gmon_start__ _ITM_deregisterTMCloneTable
	_ITM_registerTMCloneTable __errno_location __memcpy_chk __memset_chk __vfprintf_chk __fprintf_chk)
# the compiler's run-time helpers (32-bit ARM: __aeabi_*, divisions, ...)
set (allowed_regex "^(__aeabi_|__gnu_|__udiv|__div|__mod|__umod)")
execute_process (COMMAND ${NM} -D --undefined-only ${PLUGIN} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if (NOT rc EQUAL 0)
	message (FATAL_ERROR "nm failed")
endif ()
string (REPLACE "\n" ";" lines "${out}")
set (bad)
set (all)
foreach (line ${lines})
	string (REGEX REPLACE "^ *[a-zA-Z] +" "" sym "${line}")
	string (REGEX REPLACE "@.*$" "" sym "${sym}")
	if (sym STREQUAL "")
		continue ()
	endif ()
	list (APPEND all ${sym})
	list (FIND allowed ${sym} i)
	if (i LESS 0 AND NOT sym MATCHES "${allowed_regex}")
		list (APPEND bad ${sym})
	endif ()
endforeach ()
message ("imports: ${all}")
if (bad)
	message (FATAL_ERROR "unexpected imports: ${bad}")
endif ()
