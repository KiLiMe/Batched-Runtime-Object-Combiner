# cmake -DSRC=<file> -DDST=<file> -P CopyIfMissing.cmake: copies SRC to DST only when DST does not exist.
if(NOT EXISTS "${DST}")
	get_filename_component(dir "${DST}" DIRECTORY)
	file(MAKE_DIRECTORY "${dir}")
	file(COPY_FILE "${SRC}" "${DST}")
endif()
