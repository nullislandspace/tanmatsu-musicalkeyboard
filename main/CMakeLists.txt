idf_component_register(
	SRCS
		"main.c"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		badge-bsp
		tanmatsu-wifi
	INCLUDE_DIRS
		"."
)

if(IDF_TARGET STREQUAL "esp32p4")
	idf_component_set_property(${COMPONENT_LIB} PRIV_REQUIRES esp-hosted-tanmatsu APPEND)
	idf_component_set_property(${COMPONENT_LIB} PRIV_REQUIRES bt APPEND)
endif()

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
