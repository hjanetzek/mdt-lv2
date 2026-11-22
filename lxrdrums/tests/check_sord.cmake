# sord_validate on the bundle with the LV2 specifications it uses
# (-DSORD -DSPECS -DSCHEMA -DBUNDLE)
set (files)
foreach (spec core atom midi urid time parameters port-groups port-props units log options buf-size ui patch state event schemas)
	file (GLOB ttl ${SPECS}/${spec}.lv2/*.ttl)
	list (APPEND files ${ttl})
endforeach ()
execute_process (COMMAND ${SORD} ${files} ${SCHEMA} ${BUNDLE}/manifest.ttl ${BUNDLE}/edrumsynth.ttl
	RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message ("${out}${err}")
if (NOT rc EQUAL 0 OR "${out}${err}" MATCHES "error:")
	message (FATAL_ERROR "sord_validate failed")
endif ()
